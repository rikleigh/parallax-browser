/* Probe libkernel/libc wrappers for the "returns errno as a positive value"
 * bug seen in readlink/symlink/ppoll on 13.60. Each line: call, return, errno.
 * Build and run (needs /data/ps5webkit from upload-runtime.sh):
 *   prospero-clang -O2 -o libcprobe port/deps/smoke/libcprobe.c
 *   prospero-strip -o libcprobe.elf libcprobe
 *   socat -t 30 - TCP:$PS5_HOST:9021 < libcprobe.elf */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define D "/data/ps5webkit"
#define P(expr) do { errno = 0; long r_ = (long)(expr); int e_ = errno; \
    printf("PROBE %-58s = %ld errno=%d%s\n", #expr, r_, e_, (r_ > 0 && e_ == 0 && strstr(#expr, "open") == NULL && strstr(#expr, "poll") == NULL && strstr(#expr, "read") == NULL) ? "  <-- suspicious" : ""); } while (0)

int main(void)
{
    struct stat st;
    setvbuf(stdout, NULL, _IOLBF, 0);
    P(access(D "/share/xkb", R_OK | X_OK));
    P(eaccess(D "/share/xkb", R_OK | X_OK));
    P(faccessat(AT_FDCWD, D "/share/xkb", R_OK | X_OK, AT_EACCESS));
    P(access(D "/missing", R_OK));
    P(eaccess(D "/missing", R_OK));
    P(stat(D "/missing", &st));
    P(lstat(D "/missing", &st));
    P(stat(D "/share/xkb", &st));
    P(open(D "/missing", O_RDONLY));
    P(mkdir(D "/cache/probe-dir", 0755));
    P(mkdir(D "/cache/probe-dir", 0755));
    P(rmdir(D "/cache/probe-dir"));
    P(rmdir(D "/missing"));
    P(unlink(D "/missing"));
    P(rename(D "/missing", D "/missing2"));
    P(chmod(D "/missing", 0644));
    P(utimes(D "/missing", NULL));
    P(truncate(D "/missing", 0));
    P(link(D "/etc/fonts/fonts.conf", D "/cache/probe-link"));
    P(symlink("x", D "/cache/probe-sym"));
    P(readlink(D "/missing", (char[64]){0}, 64));
    P(chdir(D "/missing"));
    P(close(9999));
    P(dup2(9999, 10000));
    P(fcntl(9999, F_GETFL));
    P(fsync(9999));
    P(statfs(D, &(struct statfs){0}));
    struct pollfd pf = { 0, POLLIN, 0 };
    struct timespec ts = { 0, 1000000 };
    P(ppoll(&pf, 1, &ts, NULL));
    P(poll(&pf, 1, 1));
    printf("PROBE done\n");
    return 0;
}
