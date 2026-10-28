/* link(2)/symlink(2) on PS5 payloads return the errno value (EOPNOTSUPP, 45,
 * on /data) as a positive result (13.60). Wrapped (-Wl,--wrap=link,
 * --wrap=symlink) so callers see -1/errno. */
#include <errno.h>
#include <unistd.h>

int __real_link(const char *from, const char *to);
int __real_symlink(const char *target, const char *path);

int __wrap_link(const char *from, const char *to)
{
    int r = __real_link(from, to);
    if (r > 0) {
        errno = r;
        return -1;
    }
    return r;
}

int __wrap_symlink(const char *target, const char *path)
{
    int r = __real_symlink(target, path);
    if (r > 0) {
        errno = r;
        return -1;
    }
    return r;
}
