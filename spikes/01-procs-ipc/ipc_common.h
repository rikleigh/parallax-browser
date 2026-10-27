/* Shared between the Spike 1 parent payload and the child it spawns. */
#pragma once

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* Directories tried, in order, for the rendezvous socket. A payload sees the
 * real root, a title its sandbox, so the first writable one wins. */
static const char *const kSockDirs[] = {"/data/ps5webkit", "/tmp", "/user/temp", "/system_tmp"};
#define SOCK_NAME "spike01.sock"

/* What the child writes into the shared memory it is handed. */
#define SHM_SIZE   (16u << 20)
#define SHM_MAGIC  0x50533557u /* "PS5W" */
typedef struct {
    uint32_t magic;
    int32_t child_pid;
    uint64_t child_flex_mb;
    uint32_t pattern_ok;  /* child verified the parent's pattern */
    char note[200];
    /* dmem-share: the parent's direct (GPU-visible) memory, by offset. The
     * child tries to map the same pages - the zero-copy route for handing a
     * WebProcess's eye buffers to the process that owns the headset. */
    int64_t dmem_offset;  /* -1: none */
    uint64_t dmem_size;
    int32_t dmem_result;  /* 1 mapped and saw the pattern, 2 mapped but wrong data, <0 map error */
} shm_report_t;
#define DMEM_SIZE  (64u << 10)

static inline int send_fd(int sock, int fd, const void *data, size_t len)
{
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {(void *)data, len};
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    memset(cbuf, 0, sizeof cbuf);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof cbuf;
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    return sendmsg(sock, &msg, 0) == (ssize_t)len ? 0 : -1;
}

/* Returns the received fd, or -1 (data still read into buf when present). */
static inline int recv_fd(int sock, void *buf, size_t len)
{
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {buf, len};
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof cbuf;
    if (recvmsg(sock, &msg, 0) <= 0)
        return -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int fd;
            memcpy(&fd, CMSG_DATA(c), sizeof fd);
            return fd;
        }
    }
    return -1;
}

static inline int wait_readable(int fd, int timeout_ms)
{
    struct pollfd p = {fd, POLLIN, 0};
    return poll(&p, 1, timeout_ms) == 1 ? 0 : -1;
}

static inline void sock_path(char *out, size_t size, const char *dir)
{
    snprintf(out, size, "%s/%s", dir, SOCK_NAME);
}
