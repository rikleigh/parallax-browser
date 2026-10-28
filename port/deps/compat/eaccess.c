/* eaccess(2)/faccessat(2) on PS5 payloads return ENOSYS (78) as a positive
 * value (13.60). libxkbcommon's include-path check used eaccess and refused
 * every directory. Wrapped (-Wl,--wrap=eaccess,--wrap=faccessat): positive
 * results become -1/errno, and ENOSYS falls back to access(), which works
 * (payloads run with one uid, so real and effective ids agree). */
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

int __real_eaccess(const char *path, int mode);
int __real_faccessat(int fd, const char *path, int mode, int flag);

int __wrap_eaccess(const char *path, int mode)
{
    int r = __real_eaccess(path, mode);
    if (r <= 0)
        return r;
    if (r == ENOSYS)
        return access(path, mode);
    errno = r;
    return -1;
}

int __wrap_faccessat(int fd, const char *path, int mode, int flag)
{
    int r = __real_faccessat(fd, path, mode, flag);
    if (r <= 0)
        return r;
    if (r == ENOSYS && (fd == AT_FDCWD || (path && path[0] == '/')))
        return access(path, mode);
    errno = r;
    return -1;
}
