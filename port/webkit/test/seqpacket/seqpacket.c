/* seqpacket: does SCM_RIGHTS work over SOCK_SEQPACKET socketpairs on the PS5
 * (WebKit's IPC::Connection socket type on Unix)? Passes one end of a second
 * socketpair over the first, twice (as NetworkProcess -> UIProcess ->
 * WebProcess does), then checks the received end still talks to its peer. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
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

static int recvfd(int s, int *flags)
{
    char byte;
    struct iovec iov = { &byte, 1 };
    char control[CMSG_SPACE(sizeof(int)) * 4];
    struct msghdr m = { 0 };
    m.msg_iov = &iov; m.msg_iovlen = 1; m.msg_control = control; m.msg_controllen = sizeof control;
    if (recvmsg(s, &m, 0) < 0) return -2;
    *flags = m.msg_flags;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_type == SCM_RIGHTS) { int fd; memcpy(&fd, CMSG_DATA(c), sizeof fd); return fd; }
    return -1;
}

static void test(const char *name, int type)
{
    int a[2], b[2], c[2];
    if (socketpair(AF_UNIX, type, 0, a) || socketpair(AF_UNIX, type, 0, b) || socketpair(AF_UNIX, type, 0, c)) {
        printf("RESULT webkit-seqpacket %s FAIL socketpair errno %d\n", name, errno);
        return;
    }
    int f1 = 0, f2 = 0;
    int s1 = sendfd(a[0], b[1]);
    int r1 = recvfd(a[1], &f1);
    int s2 = r1 >= 0 ? sendfd(c[0], r1) : -1;
    int r2 = r1 >= 0 ? recvfd(c[1], &f2) : -1;
    char out[8] = "ping", in[8] = { 0 };
    ssize_t w = r2 >= 0 ? write(b[0], out, 5) : -1;
    ssize_t rd = r2 >= 0 ? read(r2, in, sizeof in) : -1;
    int ok = rd == 5 && !strcmp(in, "ping");
    printf("RESULT webkit-seqpacket %s %s send %d/%d recv fd %d/%d flags 0x%x/0x%x write %zd read %zd \"%s\" (errno %d)\n",
           name, ok ? "PASS" : "FAIL", s1, s2, r1, r2, f1, f2, w, rd, in, errno);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test("stream", SOCK_STREAM);
    test("seqpacket", SOCK_SEQPACKET);
    test("dgram", SOCK_DGRAM);
    return 0;
}
