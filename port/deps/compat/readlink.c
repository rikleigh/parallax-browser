/* readlink(2) on PS5 payloads: libkernel's readlink returns the errno value
 * as a positive result instead of -1/errno (seen on 13.60: readlink(regular
 * file) = 22, readlink(missing) = 2), so callers take it for a link-target
 * length. fontconfig did, and failed to load its config. symlink() itself
 * fails with EOPNOTSUPP on /data, so real links are rare.
 *
 * Linked with -Wl,--wrap=readlink (ps5compat.pc adds it): lstat() first,
 * then the real readlink only for actual symlinks. */
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

ssize_t __real_readlink(const char *path, char *buf, size_t size);

ssize_t __wrap_readlink(const char *path, char *buf, size_t size)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return -1;
    if (!S_ISLNK(st.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    ssize_t n = __real_readlink(path, buf, size);
    /* A link's target length is st_size; anything else is an errno value. */
    if (n >= 0 && st.st_size > 0 && n != (ssize_t)(st.st_size < (off_t)size ? st.st_size : (off_t)size)) {
        errno = (int)n;
        return -1;
    }
    return n;
}
