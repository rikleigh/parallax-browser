/* PS5 payload SDK shims: libc functions JSC/WTF/ICU reference that the
 * ps5-payload-sdk libc / libkernel stubs do not provide. Keep this tiny;
 * each entry is listed in NOTES.md. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* (filled in as link errors show up) */
int ps5shim_present = 1;

/* memmem(3) came from here in Spike 4; libps5compat (port/deps) provides it now. */

/* fcntl(F_DUPFD_CLOEXEC) is broken in the PS5's libkernel: it returns 0 with
 * errno 0 (fd 0, the payload's stdin socket) instead of a new descriptor
 * (port/webkit/test/seqpacket/dupfd.c, 2026-10-28). WTF::dupCloseOnExec and
 * GLib use it, so WebKit sent fd 0 in place of shared memory. Linked with
 * -Wl,--wrap=fcntl: emulate it with F_DUPFD + FD_CLOEXEC. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>

/*
 * In a registered title (2026-10-28, ps5browser's probe), F_GETFD, F_SETFD
 * and F_DUPFD all fail with EINVAL, and pipe2(O_CLOEXEC) too; F_GETFL,
 * F_SETFL(O_NONBLOCK), dup() and socketpair() work. A title can't exec, so
 * close-on-exec means nothing there: F_SETFD reports success, F_GETFD
 * reports no flags, and F_DUPFD falls back to dup(). WebKit's
 * createPlatformConnection() RELEASE_ASSERTs setCloseOnExec(). Payloads are
 * unaffected (the real calls succeed there).
 */
#include <sys/filio.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* dup() fails with EPERM in a title as well. A descriptor sent to ourselves
 * over a socketpair with SCM_RIGHTS arrives as a new descriptor for the same
 * file, which is what dup() makes. */
#include <pthread.h>
#include <sys/socket.h>
#include <sys/uio.h>

static int scmDup(int fd)
{
    static int pair[2] = { -1, -1 };
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    int result = -1, saved = errno;
    pthread_mutex_lock(&lock);
    if (pair[0] < 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        pair[0] = pair[1] = -1;
        pthread_mutex_unlock(&lock);
        return -1;
    }
    char byte = 0;
    struct iovec iov = { &byte, 1 };
    union {
        struct cmsghdr header;
        char buffer[CMSG_SPACE(sizeof(int))];
    } control;
    memset(&control, 0, sizeof control);
    struct msghdr message;
    memset(&message, 0, sizeof message);
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.buffer;
    message.msg_controllen = sizeof control.buffer;
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    if (sendmsg(pair[0], &message, 0) == 1) {
        memset(&control, 0, sizeof control);
        message.msg_controllen = sizeof control.buffer;
        if (recvmsg(pair[1], &message, 0) == 1) {
            cmsg = CMSG_FIRSTHDR(&message);
            if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
                memcpy(&result, CMSG_DATA(cmsg), sizeof(int));
        }
    }
    pthread_mutex_unlock(&lock);
    if (result >= 0)
        errno = saved;
    return result;
}

int __real_dup(int fd) __attribute__((weak)); /* defined only with --wrap=dup */
/* Linked with --wrap=dup in the title (package.sh). */
int __wrap_dup(int fd)
{
    int r = __real_dup ? __real_dup(fd) : -1;
    if (r < 0 && errno == EPERM && fd >= 0)
        r = scmDup(fd);
    return r;
}

/* dup2(): the lowest free descriptor is newfd once it is closed, and the
 * descriptors below it are held open meanwhile. Linked with --wrap=dup2 in
 * the title. */
int __real_dup2(int fd, int newfd) __attribute__((weak));
int __wrap_dup2(int fd, int newfd)
{
    int r = __real_dup2 ? __real_dup2(fd, newfd) : -1;
    if (r >= 0 || errno != EPERM || fd < 0 || newfd < 0 || newfd >= 64)
        return r;
    if (fd == newfd)
        return newfd;
    close(newfd);
    int held[64], n = 0;
    for (;;) {
        int d = scmDup(fd);
        if (d < 0 || d == newfd) {
            r = d;
            break;
        }
        if (d > newfd || n == 64) { /* newfd is taken by someone else */
            close(d);
            errno = EBUSY;
            r = -1;
            break;
        }
        held[n++] = d;
    }
    for (int i = 0; i < n; i++)
        close(held[i]);
    return r;
}

static int dupAny(int fd)
{
    int r = dup(fd);
    if (r < 0 && errno == EPERM)
        r = scmDup(fd);
    return r;
}

/* Emulated O_NONBLOCK (see F_SETFL below). */
#include <stdatomic.h>
#include <stdio.h>
#include <sys/types.h>
#define NB_FDS 4096
static _Atomic char nonBlocking[NB_FDS];
static atomic_int nonBlockingReported;
static int emulatedNonBlocking(int fd)
{
    return fd >= 0 && fd < NB_FDS && atomic_load_explicit(&nonBlocking[fd], memory_order_relaxed);
}
#define NB_FLAGS(fd, flags) (emulatedNonBlocking(fd) ? ((flags) | MSG_DONTWAIT) : (flags))

/* Weak: defined only where the title links with --wrap for these. */
ssize_t __real_recvmsg(int, struct msghdr*, int) __attribute__((weak));
ssize_t __real_sendmsg(int, const struct msghdr*, int) __attribute__((weak));
ssize_t __real_recv(int, void*, size_t, int) __attribute__((weak));
ssize_t __real_send(int, const void*, size_t, int) __attribute__((weak));
ssize_t __real_recvfrom(int, void*, size_t, int, struct sockaddr*, socklen_t*) __attribute__((weak));
ssize_t __real_sendto(int, const void*, size_t, int, const struct sockaddr*, socklen_t) __attribute__((weak));
int __real_close(int) __attribute__((weak));

ssize_t __wrap_recvmsg(int fd, struct msghdr* message, int flags)
{
    return __real_recvmsg(fd, message, NB_FLAGS(fd, flags));
}
ssize_t __wrap_sendmsg(int fd, const struct msghdr* message, int flags)
{
    return __real_sendmsg(fd, message, NB_FLAGS(fd, flags));
}
ssize_t __wrap_recv(int fd, void* buffer, size_t length, int flags)
{
    return __real_recv(fd, buffer, length, NB_FLAGS(fd, flags));
}
ssize_t __wrap_send(int fd, const void* buffer, size_t length, int flags)
{
    return __real_send(fd, buffer, length, NB_FLAGS(fd, flags));
}
ssize_t __wrap_recvfrom(int fd, void* buffer, size_t length, int flags, struct sockaddr* from, socklen_t* fromLength)
{
    return __real_recvfrom(fd, buffer, length, NB_FLAGS(fd, flags), from, fromLength);
}
ssize_t __wrap_sendto(int fd, const void* buffer, size_t length, int flags, const struct sockaddr* to, socklen_t toLength)
{
    return __real_sendto(fd, buffer, length, NB_FLAGS(fd, flags), to, toLength);
}
/* fd tracing (PS5WK_FD_TRACE=1 in the environment; child.env for the
 * payloads): every close() of a socket and every socket() of an inet socket
 * is logged with its caller's stack ("elf +0x..." offsets for
 * llvm-addr2line), so a close of a descriptor that someone else now owns
 * shows who did it. A close() that fails with EBADF (closed twice, before
 * the number was reused) is logged always (at most 50 times). Payloads link
 * with --wrap=close,socket; the title wraps close too. */
#include <netinet/in.h>
size_t backtrace(void**, size_t) __attribute__((weak));
extern char __ehdr_start __attribute__((weak));
static int fdTraceMode = -1;
static atomic_int fdTraceBadCloses;
static int fdTraceEnabled(void)
{
    /* Checked until it is on: the children read child.env after their
     * first closes. */
    if (fdTraceMode <= 0) {
        const char* e = getenv("PS5WK_FD_TRACE");
        fdTraceMode = e && *e == '1';
    }
    return fdTraceMode;
}
static void fdTraceReport(const char* what, int fd, int extra)
{
    char line[512];
    int n = snprintf(line, sizeof line, "[fdtrace %d] %s fd %d (%d) thread %p:", (int)getpid(), what, fd, extra, (void*)pthread_self());
    void* frames[14];
    size_t count = backtrace ? backtrace(frames, 14) : 0;
    uintptr_t base = (uintptr_t)&__ehdr_start;
    for (size_t i = 1; i < count && n < (int)sizeof line - 24; i++)
        n += snprintf(line + n, sizeof line - n, " +0x%lx", (unsigned long)((uintptr_t)frames[i] - base));
    if (n < (int)sizeof line - 1) {
        line[n++] = '\n';
        line[n] = 0;
    }
    write(2, line, strlen(line));
}

int __wrap_close(int fd)
{
    int family = -1;
    if (fd >= 0 && fdTraceEnabled()) {
        struct sockaddr_storage address;
        socklen_t length = sizeof address;
        if (!getsockname(fd, (struct sockaddr*)&address, &length))
            family = address.ss_family;
    }
    if (fd >= 0 && fd < NB_FDS)
        atomic_store_explicit(&nonBlocking[fd], 0, memory_order_relaxed);
    int r = __real_close(fd);
    int saved = errno;
    if (r < 0 && saved == EBADF && fd >= 0 && atomic_fetch_add(&fdTraceBadCloses, 1) < 50)
        fdTraceReport("close EBADF (already closed)", fd, -1);
    else if (family >= 0)
        fdTraceReport(family == AF_INET || family == AF_INET6 ? "close inet socket" : family == AF_UNIX ? "close unix socket" : "close socket", fd, family);
    errno = saved;
    return r;
}

static int ps5TitleSocketNbio(int fd, int on);
int __real_socket(int, int, int) __attribute__((weak));
int __wrap_socket(int domain, int type, int protocol)
{
    int fd = __real_socket(domain, type, protocol);
    /* Title: SOCK_NONBLOCK (GLib's g_socket_new asks for it) leaves the
     * socket blocking; SO_NBIO switches it. */
    if (fd >= 0 && (type & SOCK_NONBLOCK) && __real_recvmsg) {
        int saved = errno;
        int nbio = ps5TitleSocketNbio(fd, 1);
        if (nbio == -1 && fd < NB_FDS)
            atomic_store(&nonBlocking[fd], 1);
        errno = saved;
    }
    if (fd >= 0 && (domain == AF_INET || domain == AF_INET6) && fdTraceEnabled()) {
        int saved = errno;
        fdTraceReport("socket inet", fd, domain);
        errno = saved;
    }
    return fd;
}

/* Title only (where the send/recv wraps are linked): Sony's SO_NBIO, the
 * one way that makes a title's inet socket non-blocking (F_SETFL and
 * SOCK_NONBLOCK are taken but do nothing; FIONBIO is EACCES). 0: set; -1:
 * a socket that refused it; -2: not a socket. The first outcome of each
 * kind is logged. */
#define PS5_SO_NBIO 0x1200
static atomic_int nbioReported[3];
static int ps5TitleSocketNbio(int fd, int on)
{
    int r = setsockopt(fd, SOL_SOCKET, PS5_SO_NBIO, &on, sizeof on);
    int e = errno;
    int kind = !r ? 0 : e == ENOTSOCK ? 2 : 1;
    if (!atomic_exchange(&nbioReported[kind], 1))
        fprintf(stderr, "[ps5shim] SO_NBIO %d on fd %d: %s (errno %d)%s\n", on, fd, !r ? "set" : "refused", r ? e : 0,
            kind == 1 ? "; emulating with MSG_DONTWAIT" : "");
    return !r ? 0 : kind == 2 ? -2 : -1;
}
int __real_fcntl(int fd, int cmd, ...);
static int dupFallback(int fd, intptr_t minFD)
{
    int r = __real_fcntl(fd, F_DUPFD, minFD);
    if (r < 0 && errno == EINVAL && fd >= 0) {
        r = dupAny(fd);
        /* dup() takes the lowest free fd; F_DUPFD wants >= minFD. */
        if (r >= 0 && r < minFD) {
            int fds[64], n = 0, s = r;
            while (s >= 0 && s < minFD && n < 64) {
                fds[n++] = s;
                s = dupAny(fd);
            }
            for (int i = 0; i < n; i++)
                close(fds[i]);
            r = s;
        }
    }
    return r;
}

int __wrap_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    intptr_t arg = va_arg(ap, intptr_t);
    va_end(ap);
    switch (cmd) {
    case F_DUPFD_CLOEXEC: {
        int r = dupFallback(fd, arg);
        if (r >= 0)
            __real_fcntl(r, F_SETFD, FD_CLOEXEC);
        return r;
    }
    case F_DUPFD:
        return dupFallback(fd, arg);
    case F_SETFD: {
        int r = __real_fcntl(fd, cmd, arg);
        if (r < 0 && errno == EINVAL && fd >= 0) {
            errno = 0;
            return 0;
        }
        return r;
    }
    case F_GETFD: {
        int r = __real_fcntl(fd, cmd, arg);
        if (r < 0 && errno == EINVAL && fd >= 0) {
            errno = 0;
            return 0;
        }
        return r;
    }
    case F_GETFL: {
        int r = __real_fcntl(fd, cmd, arg);
        if (r < 0 && (errno == EACCES || errno == EPERM) && fd >= 0) {
            errno = 0;
            r = O_RDWR;
        }
        if (r >= 0 && emulatedNonBlocking(fd))
            r |= O_NONBLOCK;
        return r;
    }
    case F_SETFL: {
        /* Title: F_SETFL and FIONBIO on a socket fail with EACCES (GLib:
         * "Error setting socket to nonblocking mode: Permission denied"), so
         * GSocket's fds stayed blocking and the UI main loop hung in
         * recvmsg. Emulate O_NONBLOCK: remember it, and add MSG_DONTWAIT to
         * that fd's send/recv calls (wrapped in the title). */
        int r = __real_fcntl(fd, cmd, arg);
        if (r < 0 && (errno == EACCES || errno == EPERM) && fd >= 0 && fd < NB_FDS) {
            int saved = errno;
            int on = (arg & O_NONBLOCK) != 0;
            if (!ioctl(fd, FIONBIO, &on))
                return 0;
            if (__real_recvmsg) { /* only where the send/recv wraps are linked */
                atomic_store(&nonBlocking[fd], (char)on);
                if (on && !atomic_exchange(&nonBlockingReported, 1))
                    fprintf(stderr, "[ps5shim] fcntl(F_SETFL, O_NONBLOCK) on fd %d: errno %d; emulating with MSG_DONTWAIT\n", fd, saved);
                errno = 0;
                return 0;
            }
            errno = saved;
        }
        /* Title, inet sockets: F_SETFL(O_NONBLOCK) succeeds and F_GETFL shows
         * it, but the socket stays blocking (a read with no data waits;
         * FIONBIO, which FreeBSD's F_SETFL uses for sockets, is EACCES).
         * GLib's sockets then sat in recv() until the server closed the
         * connection: hlsdemux2 in the in-title WebProcess took ~15 s a
         * segment, a libsoup request on a quiet thread never finished
         * (2026-11-16, ps5browser's wakeup test). Sony's SO_NBIO socket option
         * does switch it; where that fails, MSG_DONTWAIT as above. */
        if (r >= 0 && fd >= 0 && __real_recvmsg) {
            int saved = errno;
            int on = (arg & O_NONBLOCK) != 0;
            int nbio = ps5TitleSocketNbio(fd, on);
            if (nbio != -2) { /* a socket */
                if (fd < NB_FDS)
                    atomic_store(&nonBlocking[fd], (char)(nbio ? on : 0));
                errno = saved;
                return r;
            }
            errno = saved;
        }
        if (r >= 0 && fd >= 0 && fd < NB_FDS)
            atomic_store(&nonBlocking[fd], 0);
        return r;
    }
    default:
        return __real_fcntl(fd, cmd, arg);
    }
}

/* pipe2() fails with EINVAL in a registered title (it works in payloads):
 * GLib's GWakeup aborted "Creating pipes for GWakeup: Invalid argument" at
 * the first title launch (2026-10-28). Linked with -Wl,--wrap=pipe2: fall
 * back to pipe() and then socketpair(AF_UNIX), applying the flags with
 * fcntl. libkernel may also return the errno as a positive value. */
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

int __real_pipe2(int fds[2], int flags);
int __wrap_pipe2(int fds[2], int flags)
{
    static int reported;
    int r = __real_pipe2(fds, flags);
    if (r == 0)
        return 0;
    int saved = r > 0 ? r : errno;
    const char* how = "pipe";
    if (pipe(fds) != 0) {
        how = "socketpair";
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            errno = saved;
            return -1;
        }
    }
    for (int i = 0; i < 2; i++) {
        if (flags & O_CLOEXEC)
            __real_fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        if (flags & O_NONBLOCK)
            __real_fcntl(fds[i], F_SETFL, __real_fcntl(fds[i], F_GETFL, 0) | O_NONBLOCK);
    }
    if (!reported++)
        fprintf(stderr, "[ps5shim] pipe2(flags 0x%x) failed (%d, errno %d); using %s + fcntl\n", flags, r, saved, how);
    return 0;
}
