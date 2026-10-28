// Audio output for the WebProcess payloads, served by the UIProcess.
//
// Why: an elfldr payload can open sceAudioOut (port/webkit/test/audio), but
// the system audio daemon never released what payload processes took: after
// eight payload processes had used it (2026-10-28, open+close or not),
// sceAudioOutInit failed with 0x80260002 and then blocked in every new
// process until the console was restarted. A registered title is an
// application the system cleans up after, and there is only one of it, so
// the WebProcesses' ps5audiosink (port/deps/gst/gstps5audiosink.c) ships PCM
// here and the title plays it.
//
// Protocol (unix stream socket PS5WK_LAUNCH_DIR/audio-<ui pid>.sock, one
// connection per audio stream): the client sends struct ps5wk_audio_hello,
// then interleaved S16 PCM at 48 kHz. The server plays it in grains of 256
// frames through its own sceAudioOut port (main port, system user) and
// answers each grain with a uint64 count of frames played (sent with
// MSG_DONTWAIT; the client reads the latest one for its delay()). Closing
// the connection drops whatever was queued (flush, seek, pause->stop).
//
// The shot harness (a payload) uses PS5WK_AUDIO_NULL: grains are consumed at
// the real-time rate with no sceAudioOut, so test runs leave the audio
// system alone.
#include "ps5audioserver.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define GRAIN 256
#define RATE 48000

int sceAudioOutInit(void) __attribute__((weak));
int sceAudioOutOpen(int userId, int type, int index, unsigned len, unsigned freq, unsigned param) __attribute__((weak));
int sceAudioOutOutput(int handle, const void* ptr) __attribute__((weak));
int sceAudioOutClose(int handle) __attribute__((weak));
int sceAudioOutSetVolume(int handle, int flag, int* vol) __attribute__((weak));

static int mode;
static char socketPath[108];
static int streamsOpen;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#define MAX_PORTS 16
static int openPorts[MAX_PORTS]; // sceAudioOut handles + 1 (0 = free slot)
static int stopping;
static int clientFds[MAX_PORTS]; // connections being served, + 1 (0 = free)
static int threadsRunning;       // stream threads, counted from accept()

static void forgetClient(int fd)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_PORTS; i++)
        if (clientFds[i] == fd + 1)
            clientFds[i] = 0;
    pthread_mutex_unlock(&lock);
}

static void registerPort(int handle)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_PORTS; i++)
        if (!openPorts[i]) {
            openPorts[i] = handle + 1;
            break;
        }
    pthread_mutex_unlock(&lock);
}

// Returns 0 if ps5wk_audio_server_stop() already closed it.
static int unregisterPort(int handle)
{
    int found = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_PORTS; i++)
        if (openPorts[i] == handle + 1) {
            openPorts[i] = 0;
            found = 1;
            break;
        }
    pthread_mutex_unlock(&lock);
    return found;
}

#define LOGA(...) do { fprintf(stderr, "[ps5audio] " __VA_ARGS__); fputc('\n', stderr); } while (0)

static double nowMS(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int readFull(int fd, void* buffer, size_t size)
{
    char* p = buffer;
    while (size) {
        ssize_t n = recv(fd, p, size, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

static int ensureSceAudio(void)
{
    static int state; // 0 unknown, 1 ok, -1 failed
    pthread_mutex_lock(&lock);
    if (!state) {
        LOGA("calling sceAudioOutInit (%s)", sceAudioOutInit ? "bound" : "NOT bound");
        int rc = sceAudioOutInit ? sceAudioOutInit() : -1;
        state = (rc == 0 || rc == (int)0x8026000e) ? 1 : -1;
        LOGA("sceAudioOutInit -> 0x%08x", (unsigned)rc);
    }
    pthread_mutex_unlock(&lock);
    return state > 0;
}

static void* streamThread(void* data)
{
    int fd = (int)(intptr_t)data;
    struct ps5wk_audio_hello hello;
    if (readFull(fd, &hello, sizeof hello) || hello.magic != PS5WK_AUDIO_MAGIC || hello.version != PS5WK_AUDIO_VERSION
        || hello.rate != RATE || (hello.channels != 1 && hello.channels != 2)) {
        LOGA("bad hello (magic 0x%x version %u rate %u channels %u)", hello.magic, hello.version, hello.rate, hello.channels);
        forgetClient(fd);
        close(fd);
        pthread_mutex_lock(&lock);
        threadsRunning--;
        pthread_mutex_unlock(&lock);
        return NULL;
    }
    pthread_mutex_lock(&lock);
    int streamNumber = ++streamsOpen;
    pthread_mutex_unlock(&lock);
    int handle = -1;
    if ((mode == PS5WK_AUDIO_SCE || mode == PS5WK_AUDIO_SILENT) && ensureSceAudio()) {
        handle = sceAudioOutOpen(255, 0, 0, GRAIN, RATE, hello.channels == 1 ? 0 : 1);
        if (handle >= 0) {
            // PS5WK_AUDIO_SILENT: real ports at volume 0 (log-only test runs).
            int level = mode == PS5WK_AUDIO_SILENT ? 0 : 32768;
            int vol[2] = { level, level };
            sceAudioOutSetVolume(handle, 3, vol);
            registerPort(handle);
        }
    }
    LOGA("stream from pid %d: %u ch, %s (handle 0x%x), %d open", hello.pid, hello.channels,
        mode == PS5WK_AUDIO_SCE ? "sceAudioOut" : mode == PS5WK_AUDIO_SILENT ? "sceAudioOut at volume 0" : "null output", (unsigned)handle, streamNumber);
    size_t grainBytes = (size_t)GRAIN * hello.channels * 2;
    int16_t* grain = malloc(grainBytes);
    uint64_t played = 0;
    double start = nowMS(), next = start;
    while (grain && !stopping && !readFull(fd, grain, grainBytes)) {
        if (handle >= 0)
            sceAudioOutOutput(handle, grain);
        else {
            // Null output (or no port): consume at the real-time rate.
            next += GRAIN * 1000.0 / RATE;
            double wait = next - nowMS();
            if (wait > 0) {
                struct timespec ts = { 0, (long)(wait * 1e6) };
                nanosleep(&ts, NULL);
            } else if (wait < -100)
                next = nowMS();
        }
        played += GRAIN;
        send(fd, &played, sizeof played, MSG_DONTWAIT);
    }
    if (handle >= 0 && unregisterPort(handle)) {
        sceAudioOutOutput(handle, NULL);
        sceAudioOutClose(handle);
    }
    free(grain);
    forgetClient(fd);
    close(fd);
    pthread_mutex_lock(&lock);
    streamsOpen--;
    threadsRunning--;
    pthread_mutex_unlock(&lock);
    LOGA("stream from pid %d closed: %.1f s of audio in %.1f s", hello.pid, played / (double)RATE, (nowMS() - start) / 1e3);
    return NULL;
}

static void* listenThread(void* unused)
{
    (void)unused;
    int fd = -1;
    // The first payload creates PS5WK_LAUNCH_DIR (0777); a title can only
    // bind in a directory a payload made.
    for (int attempt = 0; fd < 0; attempt++) {
        struct sockaddr_un address;
        memset(&address, 0, sizeof address);
        address.sun_family = AF_UNIX;
        strncpy(address.sun_path, socketPath, sizeof address.sun_path - 1);
        unlink(socketPath);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0 && !bind(fd, (struct sockaddr*)&address, sizeof address) && !listen(fd, 8))
            break;
        if (fd >= 0)
            close(fd);
        fd = -1;
        if (attempt == 0 || attempt % 120 == 0)
            LOGA("waiting to listen on %s (errno %d)", socketPath, errno);
        struct timespec ts = { 0, 500 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    chmod(socketPath, 0777);
    LOGA("listening on %s (%s)", socketPath, mode == PS5WK_AUDIO_SCE ? "sceAudioOut" : "null output");
    for (;;) {
        int client = accept(fd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            LOGA("accept: errno %d", errno);
            struct timespec ts = { 0, 100 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            continue;
        }
        // Keep the queue short: the client's sink paces itself by our
        // consumption (about 4 grains, 21 ms, in the socket buffer).
        int size = 4 * GRAIN * 4;
        setsockopt(client, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
        pthread_mutex_lock(&lock);
        int slot = -1;
        if (!stopping)
            for (int i = 0; i < MAX_PORTS && slot < 0; i++)
                if (!clientFds[i])
                    slot = i;
        if (slot >= 0) {
            clientFds[slot] = client + 1;
            threadsRunning++;
        }
        pthread_mutex_unlock(&lock);
        if (slot < 0) { // winding down, or too many streams
            close(client);
            continue;
        }
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        if (pthread_create(&thread, &attr, streamThread, (void*)(intptr_t)client)) {
            forgetClient(client);
            close(client);
            pthread_mutex_lock(&lock);
            threadsRunning--;
            pthread_mutex_unlock(&lock);
        } else
            pthread_detach(thread);
    }
    return NULL;
}

void ps5wk_audio_server_start(int outputMode)
{
    if (socketPath[0])
        return;
    mode = outputMode;
    snprintf(socketPath, sizeof socketPath, PS5WK_AUDIO_SOCKET_FORMAT, (int)getpid());
    pthread_t thread;
    if (!pthread_create(&thread, NULL, listenThread, NULL))
        pthread_detach(thread);
}

// Closes every sceAudioOut port the server holds. The title calls it on the
// exit paths it controls (main returning); when the system closes the title
// (SIGKILL) the system has to free them (checked with the self-test below
// over repeated launch/kill cycles).
void ps5wk_audio_server_stop(void)
{
    // 1. No new streams; wake every stream thread out of recv(). A thread in
    //    sceAudioOutOutput returns within one grain (5.3 ms), sees `stopping`
    //    and closes its own port (a port is never closed under a thread that
    //    is writing to it).
    pthread_mutex_lock(&lock);
    stopping = 1;
    int woken = 0;
    for (int i = 0; i < MAX_PORTS; i++)
        if (clientFds[i]) {
            shutdown(clientFds[i] - 1, SHUT_RDWR);
            woken++;
        }
    pthread_mutex_unlock(&lock);
    // 2. Wait (up to 2 s) for the stream threads to finish.
    double t0 = nowMS();
    int left;
    for (;;) {
        pthread_mutex_lock(&lock);
        left = threadsRunning;
        pthread_mutex_unlock(&lock);
        if (!left || nowMS() - t0 > 2000)
            break;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    // 3. Ports no thread owns any more (the self-test's), or of threads that
    //    did not finish in time.
    pthread_mutex_lock(&lock);
    int handles[MAX_PORTS], n = 0;
    for (int i = 0; i < MAX_PORTS; i++)
        if (openPorts[i]) {
            handles[n++] = openPorts[i] - 1;
            openPorts[i] = 0;
        }
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < n; i++)
        sceAudioOutClose(handles[i]);
    LOGA("stopped: %d stream(s) woken, %d thread(s) still running after %.0f ms, %d port(s) closed here", woken, left, nowMS() - t0, n);
}

int ps5wk_audio_server_active_streams(void)
{
    pthread_mutex_lock(&lock);
    int n = threadsRunning;
    pthread_mutex_unlock(&lock);
    return n;
}

// /data/ps5webkit/audio-selftest: open a port at start, play 0.1 s of
// silence and keep the port open, so that killing the title shows whether
// the system frees a title's ports (the next launch must open one again).
static void* selftestThread(void* unused)
{
    (void)unused;
    double t0 = nowMS();
    LOGA("self-test: starting");
    int ok = ensureSceAudio();
    int handle = ok ? sceAudioOutOpen(255, 0, 0, GRAIN, RATE, 1) : -1;
    LOGA("self-test: sceAudioOutOpen -> 0x%08x", (unsigned)handle);
    int outputs = 0;
    double played = 0;
    if (handle >= 0) {
        registerPort(handle);
        static int16_t silence[GRAIN * 2];
        double t1 = nowMS();
        for (; outputs < 20; outputs++)
            if (sceAudioOutOutput(handle, silence) < 0)
                break;
        played = nowMS() - t1;
    }
    fprintf(stderr, "RESULT ps5browser audio-selftest %s init %s, sceAudioOutOpen 0x%08x, %d grains of silence in %.0f ms (%.0f ms after start of the test); port left open\n",
        handle >= 0 && outputs == 20 ? "PASS" : "FAIL", ok ? "ok" : "FAILED", (unsigned)handle, outputs, played, nowMS() - t0);
    return NULL;
}

void ps5wk_audio_server_selftest(void)
{
    pthread_t thread;
    if (!pthread_create(&thread, NULL, selftestThread, NULL))
        pthread_detach(thread);
}
