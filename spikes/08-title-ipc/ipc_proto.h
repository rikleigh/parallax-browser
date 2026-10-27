/* Spike 8: wire protocol between the title (client) and the payload (server).
 *
 * The title plays WebKit's UIProcess/GPU process, the payload a WebProcess.
 * Two transports are tried, in this order:
 *   unix  AF_UNIX path socket in /system_tmp (the only directory where a
 *         payload could bind one, Spike 1), chmod 0777 so the title may
 *         connect; carries SCM_RIGHTS
 *   tcp   127.0.0.1 loopback; no fd passing, shared memory by name instead
 * Every message is one fixed 64-byte msg_t; BULK is followed by raw bytes.
 */
#pragma once

#include "../01-procs-ipc/ipc_common.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/mman.h>

#define IPC_SOCK_PATH  "/system_tmp/ps5wk-ipc.sock"
/* Reverse direction: the payload creates this directory 0777 (a title could
 * not bind in /system_tmp itself), the title listens in it and the payload
 * connects. */
#define IPC_BACK_DIR   "/system_tmp/ps5wk"
#define IPC_BACK_PATH  IPC_BACK_DIR "/title.sock"
#define IPC_TCP_PORT   19500
#define IPC_SHM_NAME   "/ps5wk-ipc"
#define IPC_SHM_SIZE   (16u << 20)
#define IPC_REPLY_OFF  (8u << 20)  /* where the client leaves its note */
#define IPC_MAGIC      0x49505357u /* "WSPI" */

enum { SEED_SERVER = 0x11, SEED_NAMED = 0x22, SEED_CLIENT = 0x33 };

enum {
    MSG_HELLO = 1,   /* c->s; first message of a session */
    MSG_WELCOME,     /* s->c; text = named shm, arg 1 = anon shm fd attached */
    MSG_SHM_DONE,    /* c->s; text = note written into both shms, fd attached on unix */
    MSG_SHM_RESULT,  /* s->c; arg = SHM_OK_* bits */
    MSG_PING,        /* c->s, echoed */
    MSG_BULK,        /* c->s; value = byte count that follows; acked with value = bytes read */
    MSG_BYE,         /* c->s; end the session */
    MSG_QUIT,        /* c->s; end the session and stop the server */
    MSG_CONNECT_BACK, /* c->s, first message; text = path the title listens on. The
                         server connects there, acks (arg = errno, 0 ok) and serves
                         a session on the new socket. */
};
enum { SHM_OK_ANON = 1, SHM_OK_NAMED = 2, SHM_OK_REVERSE = 4 };

typedef struct {
    uint32_t magic;
    uint32_t type;
    int32_t pid;
    uint32_t arg;
    uint64_t value;
    char text[40];
} msg_t;
_Static_assert(sizeof(msg_t) == 64, "msg_t is the 64-byte wire format");

static inline msg_t ipc_msg(uint32_t type, uint32_t arg, uint64_t value, const char *text)
{
    msg_t m;
    memset(&m, 0, sizeof m);
    m.magic = IPC_MAGIC;
    m.type = type;
    m.pid = (int32_t)getpid();
    m.arg = arg;
    m.value = value;
    if (text)
        snprintf(m.text, sizeof m.text, "%s", text);
    return m;
}

static inline int ipc_write_all(int s, const void *buf, size_t len)
{
    for (size_t off = 0; off < len;) {
        ssize_t n = send(s, (const char *)buf + off, len - off, 0);
        if (n <= 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

static inline int ipc_read_all(int s, void *buf, size_t len, int timeout_ms)
{
    for (size_t off = 0; off < len;) {
        if (wait_readable(s, timeout_ms) != 0)
            return -1;
        ssize_t n = recv(s, (char *)buf + off, len - off, 0);
        if (n <= 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

static inline int ipc_send(int s, const msg_t *m, int fd)
{
    return fd >= 0 ? send_fd(s, fd, m, sizeof *m) : ipc_write_all(s, m, sizeof *m);
}

/* fd may be NULL when none is expected. The first recvmsg collects any fd;
 * the rest of a short read is read plainly. */
static inline int ipc_recv(int s, msg_t *m, int *fd, int timeout_ms)
{
    if (fd)
        *fd = -1;
    if (wait_readable(s, timeout_ms) != 0)
        return -1;
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {m, sizeof *m};
    struct msghdr h;
    memset(&h, 0, sizeof h);
    h.msg_iov = &iov;
    h.msg_iovlen = 1;
    h.msg_control = cbuf;
    h.msg_controllen = sizeof cbuf;
    ssize_t n = recvmsg(s, &h, 0);
    if (n <= 0)
        return -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&h); c; c = CMSG_NXTHDR(&h, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int got;
            memcpy(&got, CMSG_DATA(c), sizeof got);
            if (fd)
                *fd = got;
            else
                close(got);
        }
    }
    if ((size_t)n < sizeof *m &&
        ipc_read_all(s, (char *)m + n, sizeof *m - (size_t)n, timeout_ms) != 0)
        return -1;
    return m->magic == IPC_MAGIC ? 0 : -1;
}

/* A recognisable pattern, checked sparsely (every 4 KB and the last word). */
static inline void ipc_fill(uint8_t *p, size_t size, uint32_t seed)
{
    uint32_t *w = (uint32_t *)p;
    for (size_t i = 0; i < size / 4; i++)
        w[i] = (uint32_t)i * 2654435761u ^ seed;
}

static inline int ipc_check(const uint8_t *p, size_t size, uint32_t seed)
{
    const uint32_t *w = (const uint32_t *)p;
    const size_t n = size / 4;
    for (size_t i = 0; i < n; i += 1024)
        if (i != IPC_REPLY_OFF / 4 && w[i] != ((uint32_t)i * 2654435761u ^ seed))
            return 0; /* the note an earlier session left is not pattern */
    return w[n - 1] == ((uint32_t)(n - 1) * 2654435761u ^ seed);
}

static inline uint8_t *ipc_map(int fd, size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    return p == MAP_FAILED ? NULL : (uint8_t *)p;
}

/* Anonymous shm filled with the seed's pattern; NULL on failure. */
static inline uint8_t *ipc_anon_shm(int *fd_out, uint32_t seed)
{
    int fd = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
    if (fd < 0)
        return NULL;
    uint8_t *p = ftruncate(fd, IPC_SHM_SIZE) == 0 ? ipc_map(fd, IPC_SHM_SIZE) : NULL;
    if (!p) {
        close(fd);
        return NULL;
    }
    ipc_fill(p, IPC_SHM_SIZE, seed);
    *fd_out = fd;
    return p;
}

static inline void ipc_nodelay(int s)
{
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

/* -- round-trip baseline within one process --------------------------------- */
/* An echo thread on one end of a connected pair, 64-byte ping-pong from the
 * caller on the other: separates the kernel's own socket latency from
 * anything specific to crossing between a title and a payload. */
static void *ipc_echo_main(void *arg)
{
    const int s = (int)(intptr_t)arg;
    msg_t m;
    while (ipc_read_all(s, &m, sizeof m, 2000) == 0 && m.type == MSG_PING)
        if (ipc_write_all(s, &m, sizeof m) != 0)
            break;
    return NULL;
}

/* Average round trip in microseconds, or -1. Closes both sockets. */
static inline double ipc_rtt_pair(int a, int b, int n)
{
    pthread_t t;
    double avg = -1;
    if (pthread_create(&t, NULL, ipc_echo_main, (void *)(intptr_t)b) != 0)
        goto out;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int i = 0;
    for (; i < n; i++) {
        msg_t m;
        memset(&m, 0, sizeof m);
        m.magic = IPC_MAGIC;
        m.type = MSG_PING;
        m.arg = (uint32_t)i;
        if (ipc_write_all(a, &m, sizeof m) != 0 || ipc_read_all(a, &m, sizeof m, 2000) != 0)
            break;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (i == n)
        avg = ((double)(t1.tv_sec - t0.tv_sec) * 1e6 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e3) / n;
    shutdown(a, SHUT_RDWR);
    pthread_join(t, NULL);
out:
    close(a);
    close(b);
    return avg;
}

/* A connected loopback TCP pair, or -1. */
static inline int ipc_tcp_pair(int sv[2])
{
    struct sockaddr_in a;
    socklen_t len = sizeof a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int l = socket(AF_INET, SOCK_STREAM, 0);
    if (l < 0)
        return -1;
    if (bind(l, (struct sockaddr *)&a, sizeof a) != 0 || listen(l, 1) != 0 ||
        getsockname(l, (struct sockaddr *)&a, &len) != 0) {
        close(l);
        return -1;
    }
    sv[0] = socket(AF_INET, SOCK_STREAM, 0);
    if (sv[0] < 0 || connect(sv[0], (struct sockaddr *)&a, sizeof a) != 0 ||
        (sv[1] = accept(l, NULL, NULL)) < 0) {
        if (sv[0] >= 0)
            close(sv[0]);
        close(l);
        return -1;
    }
    close(l);
    ipc_nodelay(sv[0]);
    ipc_nodelay(sv[1]);
    return 0;
}

/* Logs in-process unix-socketpair and loopback-TCP round trips. */
static inline void ipc_rtt_baseline(const char *who, int n)
{
    int sv[2];
    double unix_us = -1, tcp_us = -1;
    int unix_errno = 0;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0)
        unix_us = ipc_rtt_pair(sv[0], sv[1], n);
    else
        unix_errno = errno;
    if (ipc_tcp_pair(sv) == 0)
        tcp_us = ipc_rtt_pair(sv[0], sv[1], n);
    spike_log("%s: in-process round trips (%d x 64 B): socketpair %.1f us%s, loopback tcp %.1f us",
              who, n, unix_us, unix_errno ? " (socketpair failed)" : "", tcp_us);
}
