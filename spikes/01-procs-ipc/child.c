/* Spike 1 child: spawned by the parent through the local elfldr, the way a
 * WebKit ProcessLauncher would start a WebProcess. Finds the parent's
 * rendezvous socket, receives a shared-memory fd over SCM_RIGHTS, reports
 * into it, and passes a pipe fd back the other way. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "ipc_common.h"

int sceKernelAvailableFlexibleMemorySize(size_t *);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t offset,
                             size_t align);

int main(void)
{
    printf("[child %d] started\n", (int)getpid());
    fflush(stdout);

    int sk = -1;
    for (size_t i = 0; i < sizeof kSockDirs / sizeof *kSockDirs && sk < 0; i++) {
        struct sockaddr_un a;
        memset(&a, 0, sizeof a);
        a.sun_family = AF_UNIX;
        sock_path(a.sun_path, sizeof a.sun_path, kSockDirs[i]);
        sk = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sk >= 0 && connect(sk, (struct sockaddr *)&a, sizeof a) != 0) {
            close(sk);
            sk = -1;
        } else if (sk >= 0) {
            printf("[child] connected to %s\n", a.sun_path);
        }
    }
    if (sk < 0) {
        printf("[child] no rendezvous socket found (errno %d)\n", errno);
        return 1;
    }

    char tag[8] = {0};
    int shm = recv_fd(sk, tag, sizeof tag);
    if (shm < 0) {
        printf("[child] no fd received (errno %d)\n", errno);
        return 1;
    }
    uint8_t *p = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0);
    if (p == MAP_FAILED) {
        printf("[child] mmap of received shm failed (errno %d)\n", errno);
        return 1;
    }

    /* The parent filled the tail with i & 0xff; check a sample. */
    uint32_t ok = 1;
    for (size_t i = sizeof(shm_report_t); i < SHM_SIZE; i += 4093)
        ok &= p[i] == (uint8_t)(i & 0xff);

    size_t flex = 0;
    sceKernelAvailableFlexibleMemorySize(&flex);
    shm_report_t *r = (shm_report_t *)p;
    r->child_pid = (int32_t)getpid();
    r->child_flex_mb = flex >> 20;
    r->pattern_ok = ok;
    snprintf(r->note, sizeof r->note, "child uid %d", (int)getuid());
    /* Map the parent's direct memory by its offset, if it sent one. */
    r->dmem_result = 0;
    if (r->dmem_offset >= 0 && r->dmem_size) {
        void *va = NULL;
        int rc = sceKernelMapDirectMemory(&va, r->dmem_size, 0x33, 0, r->dmem_offset, 0x10000);
        if (rc != 0) {
            r->dmem_result = rc < 0 ? rc : -rc;
        } else {
            volatile uint8_t *d = va;
            r->dmem_result = d[0] == 0xa5 ? 1 : 2;
            d[1] = 0x5a;
        }
    }
    __atomic_store_n(&r->magic, SHM_MAGIC, __ATOMIC_RELEASE);

    /* The reverse direction: hand the parent a pipe and write through it. */
    int pfd[2];
    if (pipe(pfd) == 0 && send_fd(sk, pfd[0], "pipe", 4) == 0) {
        const char msg[] = "hello through a passed pipe";
        if (write(pfd[1], msg, sizeof msg) != (ssize_t)sizeof msg)
            printf("[child] pipe write failed\n");
    }

    /* Stay alive until the parent closes, as a WebProcess would. */
    char c;
    wait_readable(sk, 5000);
    (void)read(sk, &c, 1);
    printf("[child] done\n");
    return 0;
}
