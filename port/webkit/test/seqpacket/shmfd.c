/* shmfd: WebKit's out-of-line IPC bodies are shared memory fds passed with
 * SCM_RIGHTS. On the console the WebProcess received a socket instead
 * (fstat mode 0140666, mmap ENODEV). Check shm fds as WebKit creates them
 * (shm_open(SHM_ANON) or a named shm_open + unlink): fstat before and after
 * passing them over a SOCK_SEQPACKET socketpair, and mmap PROT_READ. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int sendfd(int s, int fd)
{
    char byte = 'x';
    struct iovec iov = { &byte, 1 };
    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof control);
    struct msghdr m = { 0 };
    m.msg_iov = &iov; m.msg_iovlen = 1; m.msg_control = control; m.msg_controllen = sizeof control;
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    return (int)sendmsg(s, &m, 0);
}

static int recvfd(int s)
{
    char byte;
    struct iovec iov = { &byte, 1 };
    char control[CMSG_SPACE(sizeof(int)) * 4];
    struct msghdr m = { 0 };
    m.msg_iov = &iov; m.msg_iovlen = 1; m.msg_control = control; m.msg_controllen = sizeof control;
    if (recvmsg(s, &m, 0) < 0) return -2;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_type == SCM_RIGHTS) { int fd; memcpy(&fd, CMSG_DATA(c), sizeof fd); return fd; }
    return -1;
}

static void test(const char *name, int fd)
{
    struct stat a = { 0 }, b = { 0 };
    int ok1 = fd >= 0 && ftruncate(fd, 4457) == 0;
    char *w = ok1 ? mmap(NULL, 4457, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    if (w != MAP_FAILED) strcpy(w, "shared");
    fstat(fd, &a);
    int sp[2];
    socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sp);
    int s = sendfd(sp[0], fd);
    int r = recvfd(sp[1]);
    fstat(r, &b);
    char *p = r >= 0 ? mmap(NULL, 4457, PROT_READ, MAP_SHARED, r, 0) : MAP_FAILED;
    int e = errno;
    int ok = p != MAP_FAILED && !strcmp(p, "shared");
    printf("RESULT webkit-shmfd %s %s fd %d mode 0%o size %lld; sent %d, received %d mode 0%o size %lld; mmap %s (errno %d)\n",
           name, ok ? "PASS" : "FAIL", fd, (unsigned)a.st_mode, (long long)a.st_size, s, r, (unsigned)b.st_mode,
           (long long)b.st_size, p != MAP_FAILED ? "ok" : "failed", e);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test("SHM_ANON", shm_open(SHM_ANON, O_CREAT | O_RDWR, 0600));
    int nfd = shm_open("/WK2SharedMemory.probe", O_CREAT | O_RDWR, 0600);
    shm_unlink("/WK2SharedMemory.probe");
    test("named", nfd);
    return 0;
}
