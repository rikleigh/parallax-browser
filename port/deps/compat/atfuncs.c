/* openat(2), fstatat(2), unlinkat(2), utimensat(2) on PS5 payloads return an
 * errno value as a positive result instead of -1/errno (13.60, probe
 * port/webkit/test/openat: openat(file, O_DIRECTORY) = 20 (ENOTDIR),
 * openat(missing) = 2, fstatat(missing) = 2, unlinkat(missing) = 2; open()
 * itself is right).
 *
 * The damage (2026-10-28): libc++'s std::filesystem::remove_all opens each
 * entry with openat(O_DIRECTORY) and, when fdopendir() then fails, closes
 * what it got. WebKit's NetworkProcess runs remove_all on an origin's storage
 * directory whenever a WebProcess goes away, so every regular file in there
 * made it close(20): whatever socket held fd 20 lost it under its owner. A
 * TLS connection on fd 20 then failed ("Bad file descriptor"), or the next
 * one to get fd 20 had other writes mixed into its handshake ("tlsv1 alert
 * protocol version", "Connection reset by peer"), on YouTube after process
 * swaps.
 *
 * Wrapped (-Wl,--wrap=openat,--wrap=fstatat,--wrap=unlinkat,--wrap=utimensat):
 * positive results of the calls that return 0 on success become -1/errno.
 * openat returns a descriptor on success, so a positive result is kept only
 * if it is open on the very file the path names (same device and inode);
 * otherwise it was an errno. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

int __real_openat(int fd, const char *path, int flags, ...);
int __real_fstatat(int fd, const char *path, struct stat *st, int flag);
int __real_unlinkat(int fd, const char *path, int flag);
int __real_utimensat(int fd, const char *path, const struct timespec times[2], int flag);

static int positiveToErrno(int r)
{
    if (r > 0) {
        errno = r;
        return -1;
    }
    return r;
}

int __wrap_fstatat(int fd, const char *path, struct stat *st, int flag)
{
    return positiveToErrno(__real_fstatat(fd, path, st, flag));
}

int __wrap_unlinkat(int fd, const char *path, int flag)
{
    return positiveToErrno(__real_unlinkat(fd, path, flag));
}

int __wrap_utimensat(int fd, const char *path, const struct timespec times[2], int flag)
{
    return positiveToErrno(__real_utimensat(fd, path, times, flag));
}

int __wrap_openat(int fd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    int saved = errno;
    int r = __real_openat(fd, path, flags, mode);
    if (r <= 0)
        return r;
    struct stat opened, named;
    int statFlags = (flags & O_NOFOLLOW) ? AT_SYMLINK_NOFOLLOW : 0;
    if (fstat(r, &opened) == 0 && __real_fstatat(fd, path, &named, statFlags) == 0
        && opened.st_dev == named.st_dev && opened.st_ino == named.st_ino) {
        errno = saved;
        return r;
    }
    errno = r;
    return -1;
}
