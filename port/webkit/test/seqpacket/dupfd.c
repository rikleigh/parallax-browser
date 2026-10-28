/* dupfd: WTF duplicates fds with fcntl(F_DUPFD_CLOEXEC). If libkernel returns
 * the errno as a positive result (as it does for readlink/ppoll), WebKit
 * takes that number as the new fd. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int fd = shm_open(SHM_ANON, O_CREAT | O_RDWR, 0600);
    errno = 0;
    int a = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    int ea = errno;
    errno = 0;
    int b = fcntl(fd, F_DUPFD, 0);
    int eb = errno;
    int c = dup(fd);
    struct stat sa = {0}, sb = {0}, sc = {0}, s0 = {0};
    fstat(fd, &s0); int ra = fstat(a, &sa); int rb = fstat(b, &sb); fstat(c, &sc);
    int flags = a >= 0 ? fcntl(a, F_GETFD) : -1;
    printf("RESULT webkit-dupfd %s fd %d mode 0%o; F_DUPFD_CLOEXEC -> %d (errno %d, fstat %d mode 0%o, FD_CLOEXEC %d); F_DUPFD -> %d (errno %d, fstat %d mode 0%o); dup -> %d mode 0%o\n",
           (ra == 0 && sa.st_mode == s0.st_mode) ? "PASS" : "FAIL", fd, (unsigned)s0.st_mode, a, ea, ra, (unsigned)sa.st_mode,
           flags >= 0 ? (flags & FD_CLOEXEC) : -1, b, eb, rb, (unsigned)sb.st_mode, c, (unsigned)sc.st_mode);
    return 0;
}
