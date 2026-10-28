/*
 * ps5wk-child: WebKit's WebProcess and NetworkProcess as one PS5 payload.
 *
 * The UIProcess (the browser title, or the ps5wk-shot payload) sends this ELF
 * to the local elfldr followed by a header line, and hands over the IPC
 * socket through the reversed rendezvous (wpe-ps5/PS5LaunchProtocol.h).
 * Then this calls WebKit's process entry point with the argv WebKit's
 * launcher would have used: "<name> <process identifier> <ipc fd>".
 *
 * stdout/stderr are the elfldr connection, i.e. they go back to whoever
 * launched us (the title relays them to its TCP log).
 */
#include "PS5LaunchProtocol.h"
#include "../common/crashguard.h"
#include "../common/profiler.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace WebKit {
// Source/WebKit/WebProcess/WebProcessMain.h, NetworkProcess/NetworkProcessMain.h
int WebProcessMain(int argc, char** argv);
int NetworkProcessMain(int argc, char** argv);
}

extern "C" {
int sceKernelAvailableFlexibleMemorySize(size_t*) __attribute__((weak));
// glib-networking's OpenSSL TLS backend, linked statically (GIO module
// registration that a shared build would do by scanning GIO_MODULE_DIR).
void g_io_openssl_load(void*);
}

static double nowMS()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

#define LOG(...) do { fprintf(stderr, "[ps5wk-child %d] ", (int)getpid()); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

// The header line elfldr leaves on stdin after the ELF.
static bool readHeader(char* line, size_t size, int timeoutMS)
{
    size_t used = 0;
    double end = nowMS() + timeoutMS;
    while (used + 1 < size) {
        int left = static_cast<int>(end - nowMS());
        struct pollfd p { 0, POLLIN, 0 };
        if (left <= 0 || poll(&p, 1, left) != 1)
            return false;
        char c;
        ssize_t n = read(0, &c, 1);
        if (n <= 0)
            return false;
        if (c == '\n')
            break;
        line[used++] = c;
    }
    line[used] = 0;
    return true;
}

static int rendezvous(const char* path, unsigned long long processIdentifier)
{
    mkdir(PS5WK_LAUNCH_DIR, 0777);
    chmod(PS5WK_LAUNCH_DIR, 0777); // the title runs under another uid

    struct sockaddr_un address { };
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    int s = -1;
    double start = nowMS();
    while (nowMS() - start < 60000) {
        s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s >= 0 && !connect(s, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)))
            break;
        if (s >= 0)
            close(s);
        s = -1;
        usleep(50 * 1000);
    }
    if (s < 0) {
        LOG("rendezvous: cannot connect to %s (errno %d)", path, errno);
        return -1;
    }

    ps5wk_hello hello { PS5WK_HELLO_MAGIC, processIdentifier, static_cast<int32_t>(getpid()), PS5WK_LAUNCH_VERSION };
    if (send(s, &hello, sizeof(hello), 0) != sizeof(hello)) {
        LOG("rendezvous: hello failed (errno %d)", errno);
        close(s);
        return -1;
    }

    ps5wk_welcome welcome { };
    struct iovec iov { &welcome, sizeof(welcome) };
    char control[CMSG_SPACE(sizeof(int))] { };
    struct msghdr message { };
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    ssize_t n = recvmsg(s, &message, MSG_WAITALL);
    int fd = -1;
    for (struct cmsghdr* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS)
            memcpy(&fd, CMSG_DATA(header), sizeof(int));
    }
    close(s);
    if (n != sizeof(welcome) || welcome.magic != PS5WK_WELCOME_MAGIC || welcome.status || fd < 0) {
        LOG("rendezvous: no IPC fd (n %zd, status %d, fd %d, errno %d)", n, welcome.status, fd, errno);
        if (fd >= 0)
            close(fd);
        return -1;
    }
    LOG("rendezvous: connected to %s after %.0f ms, IPC fd %d", path, nowMS() - start, fd);
    return fd;
}

static void setupEnvironment()
{
    // elfldr payloads start with an empty environment.
    mkdir("/data/ps5webkit", 0777);
    mkdir("/data/ps5webkit/home", 0777);
    setenv("HOME", "/data/ps5webkit/home", 0);
    setenv("XDG_CACHE_HOME", "/data/ps5webkit/home/cache", 0);
    setenv("XDG_DATA_HOME", "/data/ps5webkit/home/data", 0);
    setenv("XDG_CONFIG_HOME", "/data/ps5webkit/home/config", 0);
    setenv("XDG_RUNTIME_DIR", "/system_tmp/ps5wk", 0);
    setenv("TMPDIR", "/system_tmp/ps5wk", 0);
    setenv("GIO_USE_VFS", "local", 0);
    setenv("GSETTINGS_BACKEND", "memory", 0);
    setenv("TZ", "UTC", 0);
    setenv("ICU_DATA", "/data/ps5webkit/share/icu", 0); // icudt76l.dat (ICU stub data linked)
    // MIME types by file extension (share/mime/globs2; no shared-mime-info on
    // the console): file:// pages, MIMETypeRegistry, GIO content types.
    setenv("XDG_DATA_DIRS", "/data/ps5webkit/share", 0);
    // Media (Milestone 2, software decoding in this process): advertise at
    // most 1080p60 to MSE/canPlayType (YouTube then stays at or below 1080p).
    // GST_DEBUG etc. can be added through child.env.
    setenv("WEBKIT_GST_VIDEO_DECODING_LIMIT", "1920x1080@60", 0);
    // Progressive media is buffered on disk (GStreamer downloadbuffer); the
    // default /var/tmp does not exist for payloads.
    mkdir("/data/ps5webkit/home/cache", 0777);
    mkdir("/data/ps5webkit/home/cache/media", 0777);
    setenv("WPE_SHELL_MEDIA_DISK_CACHE_PATH", "/data/ps5webkit/home/cache/media", 0);
    // Optional overrides for debugging, one KEY=VALUE per line (e.g.
    // WEBKIT_DEBUG=Network,Loading,IPC). Read last, so they win.
    if (FILE* f = fopen("/data/ps5webkit/child.env", "r")) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            char* eq = strchr(line, '=');
            if (line[0] == '#' || !eq)
                continue;
            *eq = 0;
            setenv(line, eq + 1, 1);
            LOG("env %s=%s", line, eq + 1);
        }
        fclose(f);
    }
}

struct MainArgs {
    bool network;
    int argc;
    char* argv[4];
    int result;
};

// CPU use of this process (user + system time per wall second, i.e. cores
// busy), logged every 5 s while it is above 0.3 cores: shows what software
// video decoding and painting cost (Milestone 2). PS5WK_CPU_LOG=0 turns it off.
static void* cpuLogger(void* data)
{
    const char* tag = static_cast<const char*>(data);
    struct rusage last;
    getrusage(RUSAGE_SELF, &last);
    double lastWall = nowMS();
    for (;;) {
        struct timespec ts { 5, 0 };
        nanosleep(&ts, nullptr);
        struct rusage now;
        getrusage(RUSAGE_SELF, &now);
        double wall = nowMS();
        auto ms = [](const struct timeval& tv) { return tv.tv_sec * 1e3 + tv.tv_usec / 1e3; };
        double user = ms(now.ru_utime) - ms(last.ru_utime), sys = ms(now.ru_stime) - ms(last.ru_stime);
        double cores = (user + sys) / (wall - lastWall);
        if (cores > 0.3)
            LOG("[cpu] %s: %.2f cores (user %.2f, sys %.2f) over %.1f s", tag, cores, user / (wall - lastWall), sys / (wall - lastWall), (wall - lastWall) / 1e3);
        last = now;
        lastWall = wall;
    }
    return nullptr;
}

static void* runWebKit(void* data)
{
    auto* args = static_cast<MainArgs*>(data);
    if (!args->network)
        ps5wk_profiler_start("web");
    args->result = args->network ? WebKit::NetworkProcessMain(args->argc, args->argv) : WebKit::WebProcessMain(args->argc, args->argv);
    return nullptr;
}

int main(int, char**)
{
    signal(SIGPIPE, SIG_IGN);
    ps5wk_crash_guard_install("child");
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);

    char line[512];
    if (!readHeader(line, sizeof(line), 20000)) {
        LOG("no launch header on stdin; this payload is started by the PS5 WebKit UIProcess (see PS5LaunchProtocol.h)");
        return 1;
    }
    char magic[32], type[16], path[256];
    int version = 0;
    unsigned long long processIdentifier = 0;
    if (sscanf(line, "%31s %d %15s %llu %255s", magic, &version, type, &processIdentifier, path) != 5
        || strcmp(magic, PS5WK_LAUNCH_HEADER) || version != PS5WK_LAUNCH_VERSION) {
        LOG("bad launch header: \"%s\"", line);
        return 1;
    }
    bool network = !strcmp(type, "network");
    char tag[32];
    snprintf(tag, sizeof(tag), "%s%llu", network ? "network" : "web", processIdentifier);
    ps5wk_crash_guard_install(tag);
    size_t flexible = 0;
    if (sceKernelAvailableFlexibleMemorySize)
        sceKernelAvailableFlexibleMemorySize(&flexible);
    LOG("%s process %llu starting (parent %d, flexible memory %zu MB)", type, processIdentifier, (int)getppid(), flexible >> 20);

    int fd = rendezvous(path, processIdentifier);
    if (fd < 0)
        return 1;

    setupEnvironment();
    // Audio goes to the UIProcess's audio server (apps/common/ps5audioserver.c):
    // <dir>/ui-<pid>.sock -> <dir>/audio-<pid>.sock.
    if (!network) {
        char audioPath[256];
        snprintf(audioPath, sizeof(audioPath), "%s", path);
        if (char* slash = strrchr(audioPath, '/')) {
            int uiPid = 0;
            if (sscanf(slash + 1, "ui-%d.sock", &uiPid) == 1) {
                snprintf(slash + 1, sizeof(audioPath) - (slash + 1 - audioPath), "audio-%d.sock", uiPid);
                setenv("PS5WK_AUDIO_SOCKET", audioPath, 0);
            }
        }
    }
    g_io_openssl_load(nullptr);

    static char idArg[32], fdArg[16];
    snprintf(idArg, sizeof(idArg), "%llu", processIdentifier);
    snprintf(fdArg, sizeof(fdArg), "%d", fd);
    static MainArgs args { network, 3, { const_cast<char*>(network ? "WPENetworkProcess" : "WPEWebProcess"), idArg, fdArg, nullptr }, 0 };

    // WebKit's main thread is whichever thread initializes it (WTF generic
    // MainThread); run it on a thread with a known, large stack for JSC.
    static char cpuTag[32];
    snprintf(cpuTag, sizeof(cpuTag), "%s", tag);
    const char* cpuLog = getenv("PS5WK_CPU_LOG");
    if (!network && !(cpuLog && !strcmp(cpuLog, "0"))) {
        pthread_t cpuThread;
        if (!pthread_create(&cpuThread, nullptr, cpuLogger, cpuTag))
            pthread_detach(cpuThread);
    }

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 64 << 20);
    pthread_t thread;
    if (pthread_create(&thread, &attributes, runWebKit, &args)) {
        LOG("pthread_create failed, running on the main thread");
        runWebKit(&args);
    } else
        pthread_join(thread, nullptr);
    LOG("%s process %llu exited with %d", type, processIdentifier, args.result);
    // _exit, not exit: static destructors crashed (WebCore::Font::~Font ->
    // ThreadGlobalData, WTFCrash) every time a page's process ended, and each
    // crash made the system write a crash report for a ~600 MB payload.
    fflush(stdout);
    fflush(stderr);
    _exit(args.result);
}
