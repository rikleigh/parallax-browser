/* rvprobe3: with the range below the top occupied, where does a hinted mmap
 * inside it land, and is a result at or above 0xFC00000000 a real mapping? */
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
#define TOP 0xFC00000000ull
static int used(void) { int ct = 0, ca = 0, gt, ga; sceKernelGetPageTableStats(&ct, &ca, &gt, &ga); return ct - ca; }
static sigjmp_buf jb; static volatile uintptr_t faultAddr; static volatile int faultSig, faultCode;
static void onfault(int s, siginfo_t* i, void* c) { faultSig = s; faultCode = i->si_code; faultAddr = (uintptr_t)i->si_addr; siglongjmp(jb, 1); }
static void touch(const char* what, volatile unsigned char* p, int write)
{
    faultSig = 0;
    if (!sigsetjmp(jb, 1)) { if (write) *p = 0x5a; else (void)*p; printf("RV3   %s %p: ok (reads %d)\n", what, (void*)p, *p); }
    else printf("RV3   %s %p: signal %d code %d si_addr %#lx\n", what, (void*)p, faultSig, faultCode, (unsigned long)faultAddr);
}
static void hinted(const char* state, unsigned char* hint, size_t len)
{
    int u = used();
    errno = 0;
    unsigned char* h = mmap(hint, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    printf("RV3  [%s] hint %p, %zu MB: returned %p errno %d; page tables %+d\n", state, (void*)hint, (size_t)(len / MB), (void*)h, h == MAP_FAILED ? errno : 0, used() - u);
    if (h == MAP_FAILED) return;
    touch("write its first byte", h, 1);
    touch("write its last byte", h + len - 1, 1);
    printf("RV3   munmap: %d\n", munmap(h, len));
}
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = onfault; sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0);
    size_t size = 2048 * MB;
    unsigned char* base = (unsigned char*)(TOP - size);
    hinted("range free", base + 1024 * MB, 16 * MB);
    hinted("range free", (unsigned char*)TOP, 16 * MB);
    hinted("range free, straddling the top", (unsigned char*)(TOP - 8 * MB), 16 * MB);
    unsigned char* m = mmap(base, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    printf("RV3 plain mmap of the 2048 MB below the top: %p\n", (void*)m);
    if (m == base) {
        hinted("range mapped", base + 1024 * MB, 16 * MB);
        hinted("range mapped", base + 1024 * MB, 1024 * MB);
        hinted("range mapped, hint at base", base, 16 * MB);
        munmap(m, size);
    } else if (m != MAP_FAILED) munmap(m, size);
    void* a = base;
    int rc = sceKernelReserveVirtualRange(&a, size, 0x90, 0x4000);
    printf("RV3 reservation of the 2048 MB below the top: rc %#x at %p\n", rc, a);
    if (!rc) {
        hinted("range reserved", base + 1024 * MB, 16 * MB);
        hinted("range reserved", base + 1024 * MB, 1024 * MB);
        hinted("range reserved, hint at base", base, 16 * MB);
        sceKernelMunmap(a, size);
    }
    printf("RV3 done\n");
    return 0;
}
