/* openat: what libkernel's openat() returns when it fails, in a payload
 * (2026-10-28). libc++'s std::filesystem::remove_all opens every entry with
 * openat(O_DIRECTORY) and closes what it got if fdopendir() fails; a
 * positive errno taken for an fd closed someone else's socket (fd 20 =
 * ENOTDIR) in the NetworkProcess.
 *   prospero-clang -O2 openat.c -o openat.elf
 *   cat openat.elf | socat -t 20 - TCP:$PS5_HOST:9021 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static void probe(const char *what, int dir, const char *path, int flags)
{
    errno = 0;
    int r = openat(dir, path, flags, 0600);
    int e = errno;
    struct stat st;
    int valid = r >= 0 && fstat(r, &st) == 0;
    printf("RESULT openat %s -> %d (errno %d)%s\n", what, r, e, r >= 0 ? (valid ? ", a real fd" : ", NOT an open fd") : "");
    if (valid)
        close(r);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *base = "/data/ps5webkit/openat-probe";
    mkdir(base, 0777);
    char file[256];
    snprintf(file, sizeof file, "%s/file", base);
    FILE *f = fopen(file, "w");
    if (f)
        fclose(f);
    int dir = open(base, O_RDONLY | O_DIRECTORY);
    printf("RESULT openat base dir fd %d (errno %d)\n", dir, errno);
    /* Fill a few low fds so a positive errno lands on an open descriptor. */
    for (int i = 0; i < 24; i++)
        open("/dev/null", O_RDONLY);
    probe("file with O_DIRECTORY (ENOTDIR expected)", dir, "file", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    probe("missing (ENOENT expected)", dir, "missing", O_RDONLY);
    probe("file O_RDONLY (fd expected)", dir, "file", O_RDONLY);
    probe("absolute file with O_DIRECTORY", AT_FDCWD, file, O_RDONLY | O_DIRECTORY);
    probe("absolute missing", AT_FDCWD, "/data/ps5webkit/openat-probe/missing", O_RDONLY);
    errno = 0;
    int r = open(file, O_RDONLY | O_DIRECTORY);
    printf("RESULT openat open() file with O_DIRECTORY -> %d (errno %d)\n", r, errno);
    errno = 0;
    struct stat st;
    r = fstatat(dir, "missing", &st, 0);
    printf("RESULT openat fstatat missing -> %d (errno %d)\n", r, errno);
    errno = 0;
    r = unlinkat(dir, "missing", 0);
    printf("RESULT openat unlinkat missing -> %d (errno %d)\n", r, errno);
    unlink(file);
    rmdir(base);
    return 0;
}
