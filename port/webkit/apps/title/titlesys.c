/*
 * titlesys.c - libc functions that the payload SDK implements as raw
 * syscalls, for the title.
 *
 * The SDK's libc.a has a syscalls.o with ~230 raw `syscall` wrappers. A
 * payload may make raw syscalls; a registered title may not: the first call
 * of one (dup, 2026-10-28) died with SYSTEM_ILLEGAL_FUNCTION_CALL
 * (0xa002030a). Worse, once any symbol pulls syscalls.o into the link, its
 * definitions override libkernel's exports of the same names (dup, ...).
 * package.sh therefore links the title against libc.a without syscalls.o
 * (libc-title.a), so dup and friends come from libkernel, and the functions
 * libkernel's stub doesn't export are defined here on top of calls a title
 * may make. Paths relative to a directory fd are not supported (no
 * F_GETPATH): only AT_FDCWD or absolute paths.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <unistd.h>

static int atPathOK(int fd, const char* path)
{
    if (fd == AT_FDCWD || (path && path[0] == '/'))
        return 1;
    errno = ENOSYS;
    return 0;
}

int openat(int fd, const char* path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (!atPathOK(fd, path))
        return -1;
    return open(path, flags, mode);
}

int fstatat(int fd, const char* path, struct stat* st, int flag)
{
    if (!atPathOK(fd, path))
        return -1;
    return (flag & AT_SYMLINK_NOFOLLOW) ? lstat(path, st) : stat(path, st);
}

int unlinkat(int fd, const char* path, int flag)
{
    if (!atPathOK(fd, path))
        return -1;
    return (flag & AT_REMOVEDIR) ? rmdir(path) : unlink(path);
}

int utimensat(int fd, const char* path, const struct timespec times[2], int flag)
{
    (void)flag;
    if (!atPathOK(fd, path))
        return -1;
    if (!times)
        return utimes(path, NULL);
    struct timeval tv[2];
    for (int i = 0; i < 2; i++) {
        tv[i].tv_sec = times[i].tv_sec;
        tv[i].tv_usec = times[i].tv_nsec / 1000;
    }
    return utimes(path, tv);
}

int eaccess(const char* path, int mode)
{
    return access(path, mode);
}

/* Ownership and mode changes on links: nothing in the browser needs them. */
int chown(const char* path, uid_t owner, gid_t group)
{
    (void)path;
    (void)owner;
    (void)group;
    return 0;
}

int fchown(int fd, uid_t owner, gid_t group)
{
    (void)fd;
    (void)owner;
    (void)group;
    return 0;
}

int lchown(const char* path, uid_t owner, gid_t group)
{
    (void)path;
    (void)owner;
    (void)group;
    return 0;
}

int lchmod(const char* path, mode_t mode)
{
    (void)path;
    (void)mode;
    return 0;
}

int posix_fadvise(int fd, off_t offset, off_t len, int advice)
{
    (void)fd;
    (void)offset;
    (void)len;
    (void)advice;
    return 0;
}

void closefrom(int lowfd)
{
    for (int fd = lowfd < 0 ? 0 : lowfd; fd < 1024; fd++)
        close(fd);
}

pid_t fork(void)
{
    errno = ENOSYS;
    return -1;
}

int statfs(const char* path, struct statfs* buf)
{
    (void)path;
    (void)buf;
    errno = ENOSYS;
    return -1;
}

int getfsstat(struct statfs* buf, long size, int mode)
{
    (void)buf;
    (void)size;
    (void)mode;
    errno = ENOSYS;
    return -1;
}

/* Not exported by libkernel to titles (libps5compat's __wrap_readlink,
 * __wrap_link and __wrap_symlink call these). /data has no links. */
ssize_t readlink(const char* path, char* buf, size_t size)
{
    (void)path;
    (void)buf;
    (void)size;
    errno = EINVAL;
    return -1;
}

int link(const char* from, const char* to)
{
    (void)from;
    (void)to;
    errno = EOPNOTSUPP;
    return -1;
}

int symlink(const char* from, const char* to)
{
    (void)from;
    (void)to;
    errno = EOPNOTSUPP;
    return -1;
}

// GStreamer's system clock (Milestone 2) waits with clock_nanosleep, a raw
// syscall in the SDK's libc: nanosleep for the remaining time instead
// (relative or TIMER_ABSTIME on the given clock). Returns an errno value,
// as clock_nanosleep does.
int clock_nanosleep(clockid_t clock, int flags, const struct timespec* request, struct timespec* remain)
{
    struct timespec rel = *request;
    if (flags & TIMER_ABSTIME) {
        struct timespec now;
        if (clock_gettime(clock, &now))
            return errno;
        rel.tv_sec = request->tv_sec - now.tv_sec;
        rel.tv_nsec = request->tv_nsec - now.tv_nsec;
        if (rel.tv_nsec < 0) {
            rel.tv_nsec += 1000000000L;
            rel.tv_sec--;
        }
        if (rel.tv_sec < 0)
            return 0;
        remain = NULL;
    }
    return nanosleep(&rel, remain) ? errno : 0;
}

/* A raw syscall in the SDK's libc. libc++'s std::filesystem::current_path
 * sizes its getcwd() buffer with it; WebKit's content filter store
 * (adblock.c) links that in. Fixed values for the console's file systems. */
long pathconf(const char* path, int name)
{
    (void)path;
    switch (name) {
    case _PC_PATH_MAX:
        return 1024;
    case _PC_NAME_MAX:
        return 255;
    case _PC_LINK_MAX:
        return 1;
    default:
        errno = EINVAL;
        return -1;
    }
}

/* lstat() is EPERM in a title (stat() of the same file works; 2026-11-02).
 * libc++'s std::filesystem::symlink_status calls it, and WebKit's
 * FileSystem::deleteFile gives up when that fails: the content filter
 * compiler's temporary NFA files (thousands per compile) and removed filter
 * lists stayed behind (adblock.c). /data has no symbolic links: stat(). */
int lstat(const char* path, struct stat* st)
{
    return stat(path, st);
}

/* The SDK libc's remove() fails in a title too (it returned -1 where unlink()
 * of the same file works); libc++'s std::filesystem::remove calls it. */
int remove(const char* path)
{
    if (!unlink(path))
        return 0;
    int error = errno;
    if ((error == EISDIR || error == EPERM) && !rmdir(path))
        return 0;
    errno = error;
    return -1;
}
