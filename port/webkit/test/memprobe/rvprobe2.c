/* rvprobe2: a reservation ending at the top of the address space, growth by
 * mprotect or by MAP_FIXED, hint-only mmap into it, partial unmap. */
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
    if (!sigsetjmp(jb, 1)) { if (write) *p = 0x5a; else (void)*p; printf("RV2   %s %p: ok (reads %d)\n", what, (void*)p, *p); }
    else printf("RV2   %s %p: signal %d code %d si_addr %#lx\n", what, (void*)p, faultSig, faultCode, (unsigned long)faultAddr);
}
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = onfault; sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0);
    size_t size = 2048 * MB;
    int flagsets[] = { 0x10 | 0x80, 0x10, 0 };
    unsigned char* p = 0; int u0 = used();
    for (int k = 0; k < 3 && !p; k++) {
        void* a = (void*)(TOP - size);
        int rc = sceKernelReserveVirtualRange(&a, size, flagsets[k], 0x4000);
        printf("RV2 reserve 2048 MB at %#llx flags %#x: rc %#x at %p; page tables %+d\n", (unsigned long long)(TOP - size), flagsets[k], rc, a, used() - u0);
        if (!rc && (uintptr_t)a == TOP - size) p = a;
        else if (!rc) sceKernelMunmap(a, size);
    }
    if (!p) { printf("RV2 no reservation at the top\n"); return 0; }
    void* again = p + 512 * MB;
    int rc2 = sceKernelReserveVirtualRange(&again, 64 * MB, 0x90, 0x4000);
    printf("RV2   second reservation inside: rc %#x\n", rc2);
    void* h = mmap(p + 256 * MB, 16 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    printf("RV2   hint-only mmap into it: %p (%s)\n", h, h == (void*)(p + 256 * MB) ? "AT the hint, inside" : "elsewhere");
    if (h != MAP_FAILED) munmap(h, 16 * MB);
    void* q = mmap(p, 256 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    printf("RV2   commit 256 MB by MAP_FIXED: %p errno %d; page tables %+d\n", q, q == MAP_FAILED ? errno : 0, used() - u0);
    touch("write committed last byte", p + 256 * MB - 1, 1);
    touch("read first reserved byte", p + 256 * MB, 0);
    touch("read last byte below the top", p + size - 1, 0);
    touch("read first byte above the top", p + size, 0);
    touch("read 4 GiB above the base", p + 4096 * MB + 7, 0);
    int mr = mprotect(p + 256 * MB, 64 * MB, PROT_READ | PROT_WRITE);
    printf("RV2   grow by mprotect of reserved 64 MB: %d errno %d; page tables %+d\n", mr, mr ? errno : 0, used() - u0);
    touch("write after mprotect grow", p + 300 * MB, 1);
    void* g = mmap(p + 320 * MB, 64 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    printf("RV2   grow by MAP_FIXED of the next 64 MB: %p errno %d; page tables %+d\n", g, g == MAP_FAILED ? errno : 0, used() - u0);
    touch("write after MAP_FIXED grow", p + 383 * MB, 1);
    touch("earlier data still there", p + 256 * MB - 1, 0);
    int um = munmap(p + 320 * MB, 64 * MB);
    printf("RV2   munmap the grown 64 MB: %d; page tables %+d\n", um, used() - u0);
    touch("read unmapped piece", p + 330 * MB, 0);
    void* o = mmap(NULL, 32 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    printf("RV2   mmap(NULL) afterwards: %p\n", o);
    void* h2 = mmap(p + 320 * MB, 16 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    printf("RV2   hint-only mmap into the unmapped piece: %p (%s)\n", h2, h2 == (void*)(p + 320 * MB) ? "AT the hint: the hole is no longer reserved" : "elsewhere");
    if (h2 != MAP_FAILED) munmap(h2, 16 * MB);
    int ur = sceKernelMunmap(p, size);
    printf("RV2   sceKernelMunmap all: %#x; page tables %+d\n", ur, used() - u0);
    int ur2 = 0; void* a = (void*)(TOP - size);
    ur2 = sceKernelReserveVirtualRange(&a, size, 0x90, 0x4000);
    int ur3 = munmap(a, size);
    printf("RV2   reserve again %#x, plain munmap all: %d errno %d; page tables %+d\n", ur2, ur3, ur3 ? errno : 0, used() - u0);
    a = (void*)(TOP - size);
    printf("RV2   reserve a third time: %#x\n", sceKernelReserveVirtualRange(&a, size, 0x90, 0x4000));
    sceKernelMunmap(a, size);
    printf("RV2 done\n");
    return 0;
}
