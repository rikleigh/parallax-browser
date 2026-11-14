/* rvprobe: does sceKernelReserveVirtualRange cost CPU page tables, from a
 * payload? Can plain mmap(MAP_FIXED) commit inside it, and does an access to
 * the uncommitted part fault with its address? */
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
int sceKernelGetPageTableStats(int*, int*, int*, int*);
int sceKernelReserveVirtualRange(void** addr, size_t len, int flags, size_t align);
int sceKernelMunmap(void* addr, size_t len);
#define MB (1ull << 20)
static int used(void) { int ct = 0, ca = 0, gt, ga; sceKernelGetPageTableStats(&ct, &ca, &gt, &ga); return ct - ca; }
static sigjmp_buf jb; static volatile uintptr_t faultAddr; static volatile int faultSig, faultCode;
static void onfault(int s, siginfo_t* i, void* c) { faultSig = s; faultCode = i->si_code; faultAddr = (uintptr_t)i->si_addr; siglongjmp(jb, 1); }
static void touch(const char* what, volatile unsigned char* p, int write)
{
    faultSig = 0;
    if (!sigsetjmp(jb, 1)) { if (write) *p = 0x5a; else (void)*p; printf("RV   %s %p: ok\n", what, (void*)p); }
    else printf("RV   %s %p: signal %d code %d si_addr %#lx\n", what, (void*)p, faultSig, faultCode, (unsigned long)faultAddr);
}
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = onfault; sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0);
    size_t sizes[] = { 512 * MB, 4096 * MB, 10240 * MB };
    for (int k = 0; k < 3; k++) {
        size_t size = sizes[k];
        int u0 = used();
        void* a = NULL;
        int rc = sceKernelReserveVirtualRange(&a, size, 0, 0x200000);
        int u1 = used();
        printf("RV reserve %zu MB: rc %#x at %p; page tables %d -> %d (%+d)\n", (size_t)(size / MB), rc, a, u0, u1, u1 - u0);
        if (rc || !a) continue;
        unsigned char* p = a;
        touch("read reserved start", p, 0);
        touch("write reserved middle", p + size / 2 + 12345, 1);
        touch("read reserved last byte", p + size - 1, 0);
        void* other = mmap(NULL, 32 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        printf("RV   another 32 MB mmap: %p (%s the reservation)\n", other, (unsigned char*)other >= p && (unsigned char*)other < p + size ? "INSIDE" : "outside");
        if (other != MAP_FAILED) munmap(other, 32 * MB);
        void* q = mmap(p, 64 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        int u2 = used();
        printf("RV   mmap FIXED 64 MB RW at start: %p errno %d; page tables %+d\n", q, q == MAP_FAILED ? errno : 0, u2 - u1);
        if (q != MAP_FAILED) {
            touch("write committed", p + 5 * MB, 1);
            touch("read just past committed", p + 64 * MB, 0);
            int mr = mprotect(p + 32 * MB, 32 * MB, PROT_NONE);
            printf("RV   mprotect upper 32 MB none: %d errno %d; page tables %+d\n", mr, mr ? errno : 0, used() - u2);
            touch("read protected", p + 40 * MB, 0);
            mr = mprotect(p + 32 * MB, 32 * MB, PROT_READ | PROT_WRITE);
            touch("write after unprotect", p + 40 * MB, 1);
        }
        /* grow: commit a second piece further in */
        void* q2 = mmap(p + size - 64 * MB, 64 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        printf("RV   mmap FIXED 64 MB RW at end: %p errno %d; page tables %+d in all\n", q2, q2 == MAP_FAILED ? errno : 0, used() - u0);
        if (q2 != MAP_FAILED) touch("write committed end", p + size - 1, 1);
        int ur = sceKernelMunmap(p, size);
        printf("RV   sceKernelMunmap all: %#x; page tables %+d from the start\n", ur, used() - u0);
        touch("read after release", p, 0);
    }
    printf("RV done\n");
    return 0;
}
