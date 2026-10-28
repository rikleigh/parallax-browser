/* jitpool: how much executable (JIT shared) memory can payloads get?
 * elfldr backs every payload's executable PT_LOAD with
 * sceKernelJitCreateSharedMemory, and a third WebKit payload (67 MB of
 * .text) failed in elfldr's mmap (2026-10-28). Allocates 4 MB JIT shm
 * blocks (each mapped RX) until the call fails, prints the total, frees. */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int sceKernelJitCreateSharedMemory(const char *name, size_t len, int max_prot, int *fd);

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    enum { BLOCK = 4 << 20, MAX = 512 };
    static int fds[MAX];
    static void *maps[MAX];
    int n = 0, rc = 0;
    for (; n < MAX; n++) {
        rc = sceKernelJitCreateSharedMemory("ps5wk-jitpool", BLOCK, PROT_READ | PROT_WRITE | PROT_EXEC, &fds[n]);
        if (rc)
            break;
        maps[n] = mmap(NULL, BLOCK, PROT_READ | PROT_EXEC, MAP_SHARED, fds[n], 0);
        if (maps[n] == MAP_FAILED) {
            printf("mmap of block %d failed\n", n);
            close(fds[n]);
            break;
        }
    }
    printf("RESULT webkit-jitpool %s %d MB of JIT shared memory (4 MB blocks) before 0x%08x\n",
           n ? "PASS" : "FAIL", n * 4, (unsigned)rc);
    for (int i = 0; i < n; i++) {
        munmap(maps[i], BLOCK);
        close(fds[i]);
    }
    return 0;
}
