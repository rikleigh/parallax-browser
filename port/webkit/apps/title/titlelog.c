/*
 * titlelog.c - the title's log output, which no thread of the title waits on.
 *
 * Until 2026-11-02 spike_log() sent every line to the Mac's
 * tools/logserver.sh with a blocking send(), and stderr (WebKit, GLib, and
 * the children's output that WPEProcessManagerPS5 relays as "[webN] ...")
 * was an fdopen() stream on the same socket. When the PS5 lost the network,
 * the socket's send buffer filled and each thread stopped at its next log
 * line: the render thread at its "[present]" line every 5 s (the picture
 * froze), the main thread, and the relay threads (then the children, whose
 * elfldr connections nobody read). See CONSOLE-HEALTH.md, "Logging".
 *
 * Now every line goes into a queue, and two threads of ours take it from
 * there:
 *
 *  - spike_log() (spike_log_sink), the relay (ps5wk_log_line_hook) and
 *    stderr put whole lines into a 512 KB ring under a mutex held for a
 *    memcpy. A line that does not fit is dropped and counted, and the next
 *    line that fits is preceded by "[log] N lines dropped: ...".
 *  - stderr is a pipe. "log-stderr" moves what arrives into the ring, line
 *    by line (a caller also takes what is waiting in the pipe before it adds
 *    its own line, so one thread's stderr and spike_log lines stay in order).
 *  - "log-send" empties the ring: every line to the file log
 *    (/data/ps5webkit/ps5browser.log; at 32 MB it becomes .log.1 and a new
 *    file starts), and to TCP through a bounded buffer (512 KB): connect()
 *    and send() never block (O_NONBLOCK, MSG_DONTWAIT, poll for room).
 *    While the log server is slow, gone or unreachable, the lines that do
 *    not fit are dropped on the TCP side only (they are in the file) and
 *    noted when it takes lines again. A connection that takes nothing for
 *    30 s is reset; reconnects back off from 0.25 s to 4 s.
 *  - spike_log_flush() waits, for a bounded time, until the queue has
 *    reached the file and the connection: before the title asks the
 *    launcher for its SIGKILL (title_exit.c) and in abort() (titleabort.c).
 *
 * Test knob: /data/ps5webkit/log-sink, read at start, one line
 *   [host]:port [queue=<KB>] [stall=<s>]
 * points the TCP log at another listener (e.g. one that can be stopped
 * without touching tools/logserver.sh), with a smaller TCP buffer (to see
 * drops sooner) or another stall timeout.
 *
 * port/webkit/test/logsink/ runs this file on the Mac against a listener
 * that stops reading (run.sh).
 */
#include "common/title.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>

/* WPEProcessManagerPS5.cpp: where the relay sends the children's lines. */
extern void (*ps5wk_log_line_hook)(const char* line, size_t n);

#define RING_SIZE (512u << 10)
#define FILE_ROTATE_BYTES (32u << 20)
#define CONNECT_TIMEOUT_MS 5000
#define BACKOFF_MIN_MS 250
#define BACKOFF_MAX_MS 4000

static pthread_mutex_t lock;
static int started;
static pthread_t senderThread;

/* The ring. Positions count bytes since start; producers write at head,
 * the sender takes from tail. Holds whole lines only. */
static char ring[RING_SIZE];
static uint64_t ringHead, ringTail;
static uint64_t ringDropped; /* lines refused since the last note */
static uint64_t processed;   /* ring position the sender has handed to the sinks */
static int wakePending;
static int wakeR = -1, wakeW = -1;

/* stderr: the pipe's read end and the line it has not finished yet. */
static int stderrR = -1;
static int stderrBySender;
static char partial[4096];
static size_t partialLen;

/* Sinks and their settings. */
static char dataDir[256] = "/data/ps5webkit";
static char logPath[320];
static char host[64];
static int port;
static size_t queueMax = 512u << 10;
static int stallMs = 30000;
static uint64_t fileBytes;

/* TCP (sender thread only, except tcpBacklog). */
static int tcp = -1, connecting, everConnected;
static char* out;
static size_t outCap, outSize, outStart, outLen; /* outCap for lines; notes may use the rest of outSize */
static uint64_t tcpDropped;
static int64_t droppedSince, lastProgress, connectStarted, nextAttempt, disconnectedAt;
static int backoffMs = BACKOFF_MIN_MS;
static int sentMidLine;
static char lossReason[96]; /* repeated when connected again: its own note may have been dropped */
static volatile int tcpBacklog; /* connected with bytes not yet sent (flush) */

static int64_t nowMS(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void setNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ---- the ring (lock held) ---- */

static size_t ringFree(void)
{
    return RING_SIZE - (size_t)(ringHead - ringTail);
}

static void ringPut(const char* p, size_t n)
{
    size_t at = (size_t)(ringHead % RING_SIZE);
    size_t first = n < RING_SIZE - at ? n : RING_SIZE - at;
    memcpy(ring + at, p, first);
    memcpy(ring, p + first, n - first);
    ringHead += n;
}

static void pushLocked(const char* p, size_t n)
{
    if (ringDropped) {
        char note[96];
        int k = snprintf(note, sizeof note, "[log] %llu lines dropped: the log queue was full\n", (unsigned long long)ringDropped);
        if (ringFree() < (size_t)k + n) {
            ringDropped++;
            return;
        }
        ringPut(note, (size_t)k);
        ringDropped = 0;
    } else if (ringFree() < n) {
        ringDropped = 1;
        return;
    }
    ringPut(p, n);
}

static void wakeLocked(void)
{
    if (wakePending || wakeW < 0)
        return;
    wakePending = 1;
    char c = 1;
    ssize_t ignored = write(wakeW, &c, 1); /* non-blocking; a full pipe wakes anyway */
    (void)ignored;
}

/* Whatever stderr's writers left in the pipe, as lines (non-blocking). */
static void absorbStderrLocked(void)
{
    if (stderrR < 0)
        return;
    char chunk[4096];
    for (;;) {
        ssize_t r = read(stderrR, chunk, sizeof chunk);
        if (r <= 0)
            return;
        const char* p = chunk;
        size_t left = (size_t)r;
        while (left) {
            const char* nl = memchr(p, '\n', left);
            size_t take = nl ? (size_t)(nl - p) + 1 : left;
            size_t room = sizeof partial - partialLen;
            if (nl ? take > room : take >= room) {
                /* An overlong line: cut it (room for the '\n'), the rest
                 * starts the next one. */
                size_t cut = room - 1;
                memcpy(partial + partialLen, p, cut);
                partial[sizeof partial - 1] = '\n';
                pushLocked(partial, sizeof partial);
                partialLen = 0;
                p += cut;
                left -= cut;
                continue;
            }
            memcpy(partial + partialLen, p, take);
            partialLen += take;
            p += take;
            left -= take;
            if (nl) {
                pushLocked(partial, partialLen);
                partialLen = 0;
            }
        }
        if ((size_t)r < sizeof chunk)
            return;
    }
}

/* spike_log_sink and ps5wk_log_line_hook: whole lines ending in '\n'. */
static void queueLine(const char* line, size_t n)
{
    if (!n)
        return;
    pthread_mutex_lock(&lock);
    absorbStderrLocked();
    if (line[n - 1] == '\n')
        pushLocked(line, n);
    else if (n < 4096) {
        char copy[4096];
        memcpy(copy, line, n);
        copy[n] = '\n';
        pushLocked(copy, n + 1);
    }
    wakeLocked();
    pthread_mutex_unlock(&lock);
}

static void notef(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void notef(const char* fmt, ...)
{
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof line - 2)
        n = (int)sizeof line - 2;
    line[n++] = '\n';
    queueLine(line, (size_t)n);
}

/* ---- "log-stderr" ---- */

static void* stderrThread(void* data)
{
    int fd = (int)(intptr_t)data;
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        int r = poll(&p, 1, -1);
        if (r < 0 && errno != EINTR)
            return NULL;
        if (r <= 0)
            continue;
        if ((p.revents & POLLNVAL) || ((p.revents & (POLLHUP | POLLERR)) && !(p.revents & POLLIN)))
            return NULL;
        pthread_mutex_lock(&lock);
        absorbStderrLocked();
        wakeLocked();
        pthread_mutex_unlock(&lock);
    }
}

/* ---- "log-send": the file ---- */

static void fileWrite(const char* p, size_t n)
{
    int fd = spike_log_file_fd;
    if (fd < 0)
        return;
    if (fileBytes + n > FILE_ROTATE_BYTES && logPath[0]) {
        char old[336];
        snprintf(old, sizeof old, "%s.1", logPath);
        int renamed = rename(logPath, old) == 0;
        int fresh = open(logPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        spike_log_file_fd = fresh;
        close(fd);
        fd = fresh;
        fileBytes = 0;
        if (fd < 0)
            return;
        char note[256];
        int k = snprintf(note, sizeof note, "[log] %s continues here (%s)\n", logPath,
            renamed ? "the first 32 MB are in .1" : "rename failed: the first 32 MB are gone");
        if (write(fd, note, (size_t)k) == k)
            fileBytes += (uint64_t)k;
    }
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            close(fd);
            spike_log_file_fd = -1;
            return;
        }
        p += w;
        n -= (size_t)w;
        fileBytes += (uint64_t)w;
    }
}

/* ---- "log-send": TCP ---- */

static void outAppend(const char* p, size_t n)
{
    if (!outLen) {
        outStart = 0;
        lastProgress = nowMS(); /* the stall clock runs while bytes wait */
    }
    if (outStart + outLen + n > outSize) {
        memmove(out, out + outStart, outLen);
        outStart = 0;
    }
    memcpy(out + outStart + outLen, p, n);
    outLen += n;
}

static void queueTcp(const char* p, size_t n, int64_t now)
{
    size_t room = outLen < outCap ? outCap - outLen : 0;
    if (tcpDropped) {
        char note[256];
        int k = snprintf(note, sizeof note, "[log] %llu lines dropped over %.1f s: the log server was not taking them (%s)\n",
            (unsigned long long)tcpDropped, (now - droppedSince) / 1000.0,
            spike_log_file_fd >= 0 ? "they are in the file log" : "no file log");
        if (room < (size_t)k + n) {
            tcpDropped++;
            return;
        }
        outAppend(note, (size_t)k);
        tcpDropped = 0;
    } else if (room < n) {
        tcpDropped = 1;
        droppedSince = now;
        return;
    }
    outAppend(p, n);
}

/* Our own notes: past the lines' limit if need be. */
static void noteTcp(const char* p, size_t n)
{
    if (outLen + n <= outSize)
        outAppend(p, n);
}

/* Puts a note before everything waiting, if it fits. */
static void prependTcp(const char* p, size_t n)
{
    if (outLen + n > outSize)
        return;
    if (!outLen)
        lastProgress = nowMS();
    if (outStart >= n) {
        outStart -= n;
    } else {
        memmove(out + n, out + outStart, outLen);
        outStart = 0;
    }
    memcpy(out + outStart, p, n);
    outLen += n;
}

static void scheduleRetry(int64_t now)
{
    nextAttempt = now + backoffMs;
    backoffMs = backoffMs * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoffMs * 2;
}

static void disconnect(int64_t now, const char* why, int reset)
{
    if (reset) {
        /* RST: the old connection's unsent data never arrives after the new one's. */
        struct linger l = { 1, 0 };
        setsockopt(tcp, SOL_SOCKET, SO_LINGER, &l, sizeof l);
    }
    close(tcp);
    tcp = -1;
    int wasConnected = !connecting;
    connecting = 0;
    if (sentMidLine && outLen) {
        /* The rest of a line half sent: useless on a new connection. */
        char* nl = memchr(out + outStart, '\n', outLen);
        size_t skip = nl ? (size_t)(nl - (out + outStart)) + 1 : outLen;
        outStart += skip;
        outLen -= skip;
        if (!tcpDropped)
            droppedSince = now;
        tcpDropped++;
    }
    sentMidLine = 0;
    scheduleRetry(now);
    if (wasConnected) {
        disconnectedAt = now;
        snprintf(lossReason, sizeof lossReason, "%s", why);
        notef("[log] connection to the log server %s:%d lost (%s); reconnecting, %zu KB waiting", host, port, why, outLen >> 10);
    }
}

static void connected(int64_t now)
{
    connecting = 0;
    backoffMs = BACKOFF_MIN_MS;
    lastProgress = now;
    char note[256];
    int k;
    if (everConnected) {
        /* After what was logged while there was no connection. */
        k = snprintf(note, sizeof note, "[log] connected to %s:%d again after %.1f s (lost: %s)\n", host, port,
            (now - disconnectedAt) / 1000.0, lossReason);
        noteTcp(note, (size_t)k);
    } else {
        /* Before the lines that waited for the first connection. */
        k = snprintf(note, sizeof note, "[log] connected to %s:%d\n", host, port);
        prependTcp(note, (size_t)k);
    }
    everConnected = 1;
}

static void startConnect(int64_t now)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        nextAttempt = INT64_MAX;
        return;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        scheduleRetry(now);
        return;
    }
    setNonBlocking(fd);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    tcp = fd;
    connecting = 1;
    connectStarted = now;
    if (connect(fd, (struct sockaddr*)&a, sizeof a) == 0)
        connected(now);
    else if (errno != EINPROGRESS && errno != EINTR) {
        close(fd);
        tcp = -1;
        connecting = 0;
        scheduleRetry(now);
    }
}

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS (MSG_DONTWAIT | MSG_NOSIGNAL)
#else
#define SEND_FLAGS MSG_DONTWAIT
#endif

static void trySend(int64_t now)
{
    while (outLen) {
        ssize_t w = send(tcp, out + outStart, outLen, SEND_FLAGS);
        if (w > 0) {
            sentMidLine = out[outStart + (size_t)w - 1] != '\n';
            outStart += (size_t)w;
            outLen -= (size_t)w;
            lastProgress = now;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w == 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)
            break;
        char why[64];
        snprintf(why, sizeof why, "send: errno %d", errno);
        disconnect(now, why, 0);
        return;
    }
    if (outLen && now - lastProgress > stallMs) {
        char why[64];
        snprintf(why, sizeof why, "it took nothing for %d s", stallMs / 1000);
        disconnect(now, why, 1);
    }
}

static void* sendThread(void* data)
{
    (void)data;
    static char batch[RING_SIZE];
    for (;;) {
        int64_t now = nowMS();
        if (tcp < 0 && now >= nextAttempt)
            startConnect(now);

        struct pollfd p[3] = { { wakeR, POLLIN, 0 }, { -1, 0, 0 }, { stderrBySender ? stderrR : -1, POLLIN, 0 } };
        int timeout = 1000;
        if (tcp >= 0) {
            p[1].fd = tcp;
            p[1].events = POLLIN | ((connecting || outLen) ? POLLOUT : 0);
            if (connecting)
                timeout = 250;
        } else if (nextAttempt != INT64_MAX) {
            int64_t until = nextAttempt - now;
            timeout = until < 0 ? 0 : until > 1000 ? 1000 : (int)until;
        }
        int ready = poll(p, 3, timeout); /* entries with fd -1 are skipped */
        if (ready < 0 && errno != EINTR)
            usleep(10 * 1000);
        now = nowMS();
        if (p[0].revents & POLLIN) {
            char drain[64];
            while (read(wakeR, drain, sizeof drain) > 0) { }
        }

        /* The ring to the sinks. The bytes between tail and head are ours
         * to read until tail moves. */
        pthread_mutex_lock(&lock);
        if (stderrBySender)
            absorbStderrLocked();
        wakePending = 0;
        uint64_t head = ringHead, tail = ringTail;
        pthread_mutex_unlock(&lock);
        size_t n = (size_t)(head - tail);
        if (n) {
            size_t at = (size_t)(tail % RING_SIZE);
            size_t first = n < RING_SIZE - at ? n : RING_SIZE - at;
            memcpy(batch, ring + at, first);
            memcpy(batch + first, ring, n - first);
            pthread_mutex_lock(&lock);
            ringTail = head;
            pthread_mutex_unlock(&lock);
            fileWrite(batch, n);
            for (const char* q = batch; q < batch + n;) {
                const char* nl = memchr(q, '\n', (size_t)(batch + n - q));
                size_t len = nl ? (size_t)(nl - q) + 1 : (size_t)(batch + n - q);
                queueTcp(q, len, now);
                q += len;
            }
        }

        if (tcp >= 0 && connecting) {
            int error = 0;
            socklen_t size = sizeof error;
            if (p[1].revents & (POLLOUT | POLLERR | POLLHUP)) {
                if (getsockopt(tcp, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && !error)
                    connected(now);
                else
                    disconnect(now, "connect failed", 0);
            } else if (now - connectStarted > CONNECT_TIMEOUT_MS)
                disconnect(now, "connect timed out", 0);
        }
        if (tcp >= 0 && !connecting && (p[1].revents & POLLIN)) {
            char discard[256];
            ssize_t r = recv(tcp, discard, sizeof discard, MSG_DONTWAIT);
            if (r == 0)
                disconnect(now, "closed by the log server", 0);
            else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                char why[64];
                snprintf(why, sizeof why, "recv: errno %d", errno);
                disconnect(now, why, 0);
            }
        }
        if (tcp >= 0 && !connecting)
            trySend(now);
        tcpBacklog = tcp >= 0 && !connecting && outLen;
        __atomic_store_n(&processed, head, __ATOMIC_RELEASE);
    }
    return NULL;
}

/* ---- spike_log_flush ---- */

static int flushLog(int timeoutMs)
{
    if (!started)
        return 1;
    if (pthread_equal(pthread_self(), senderThread))
        return 0;
    /* abort() may come while another thread holds the lock: do not wait on it for long. */
    int locked = 0;
    for (int waited = 0; waited <= timeoutMs; waited++) {
        if (!pthread_mutex_trylock(&lock)) {
            locked = 1;
            break;
        }
        usleep(1000);
    }
    if (!locked)
        return 0;
    absorbStderrLocked();
    uint64_t target = ringHead;
    wakeLocked();
    pthread_mutex_unlock(&lock);
    for (int waited = 0;; waited += 2) {
        if (__atomic_load_n(&processed, __ATOMIC_ACQUIRE) >= target && !tcpBacklog)
            return 1;
        if (waited >= timeoutMs)
            return 0;
        usleep(2000);
    }
}

/* ---- start ---- */

/* /data/ps5webkit/log-sink: "[host]:port [queue=<KB>] [stall=<s>]". */
static void readOverride(char* applied, size_t size)
{
    applied[0] = 0;
    char path[320];
    snprintf(path, sizeof path, "%s/log-sink", dataDir);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    char text[256];
    ssize_t n = read(fd, text, sizeof text - 1);
    close(fd);
    if (n <= 0)
        return;
    text[n] = 0;
    char* save = NULL;
    for (char* token = strtok_r(text, " \t\r\n", &save); token; token = strtok_r(NULL, " \t\r\n", &save)) {
        if (!strncmp(token, "queue=", 6)) {
            long kb = strtol(token + 6, NULL, 10);
            if (kb >= 8 && kb <= 65536)
                queueMax = (size_t)kb << 10;
        } else if (!strncmp(token, "stall=", 6)) {
            long s = strtol(token + 6, NULL, 10);
            if (s >= 1 && s <= 3600)
                stallMs = (int)s * 1000;
        } else {
            char* colon = strrchr(token, ':');
            const char* portText = colon ? colon + 1 : token;
            long value = strtol(portText, NULL, 10);
            if (value > 0 && value < 65536) {
                port = (int)value;
                if (colon && colon > token) {
                    *colon = 0;
                    snprintf(host, sizeof host, "%s", token);
                }
            }
        }
    }
    snprintf(applied, size, " (%s: TCP %s:%d, queue %zu KB, stall %d s)", path, host, port, queueMax >> 10, stallMs / 1000);
}

static int startLog(const char* name, const char* logHost, int logPort)
{
    if (started)
        return 0;
    snprintf(host, sizeof host, "%s", logHost);
    port = logPort;
    snprintf(logPath, sizeof logPath, "%s/%s.log", dataDir, name);
    char applied[512];
    readOverride(applied, sizeof applied);
    outSize = queueMax + 4096;
    out = malloc(outSize);
    if (!out)
        return -1;
    outCap = queueMax;
    if (pthread_mutex_init(&lock, NULL))
        return -1;
    int w[2];
    if (pipe(w))
        return -1;
    setNonBlocking(w[0]);
    setNonBlocking(w[1]);
    wakeR = w[0];
    wakeW = w[1];

    started = 1;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&senderThread, &attributes, sendThread, NULL)) {
        started = 0;
        pthread_attr_destroy(&attributes);
        return -1;
    }

    /* stderr into a pipe. A title cannot dup2 over fd 2 (EBUSY): then
     * libc's stderr stream is pointed at the pipe instead. */
    const char* stderrHow = "not captured (no pipe)";
    int e[2];
    if (!pipe(e) || !socketpair(AF_UNIX, SOCK_STREAM, 0, e)) {
        setNonBlocking(e[0]);
        fflush(stderr);
        if (dup2(e[1], 2) == 2) {
            close(e[1]);
            stderrHow = "fd 2 is a pipe";
        } else {
            FILE* f = fdopen(e[1], "w");
            if (f) {
                setvbuf(f, NULL, _IOLBF, 0);
                stderr = f;
                stderrHow = "the stderr stream writes into a pipe";
            } else {
                close(e[1]);
                close(e[0]);
                e[0] = -1;
                stderrHow = "not captured (fdopen failed)";
            }
        }
        pthread_t thread;
        if (e[0] >= 0 && pthread_create(&thread, &attributes, stderrThread, (void*)(intptr_t)e[0])) {
            stderrBySender = 1; /* the sender polls it instead */
            stderrHow = "a pipe, read by the sender (no thread of its own)";
        }
        pthread_mutex_lock(&lock);
        stderrR = e[0];
        pthread_mutex_unlock(&lock);
    }
    pthread_attr_destroy(&attributes);
    spike_log_sink = queueLine;
    spike_log_flush_hook = flushLog;
    ps5wk_log_line_hook = queueLine;
    notef("[log] non-blocking log: TCP %s:%d (buffer %zu KB, dropped lines noted), file %s, stderr: %s%s", host, port,
        queueMax >> 10, spike_log_file_fd >= 0 ? logPath : "off", stderrHow, applied);
    return 0;
}

/* Called by main before spike_title_init(), which then starts the log
 * through spike_title_log_start_hook (after the unjail: /data and the
 * network are there). */
void ps5wk_titlelog_install(const char* dir)
{
    if (dir)
        snprintf(dataDir, sizeof dataDir, "%s", dir);
    spike_title_log_start_hook = startLog;
}
