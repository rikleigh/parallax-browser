/* topprobe: where does a payload's address space end, and what does an access beyond it do? */
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#define PG 16384ULL
static sigjmp_buf jb; static volatile void* faultAddr; static volatile int faultSig, faultCode;
static void onFault(int sig, siginfo_t* si, void* ctx) { faultSig = sig; faultCode = si->si_code; faultAddr = si->si_addr; siglongjmp(jb, 1); }
static void tryAccess(const char* name, unsigned long long a, int write)
{
    volatile unsigned char* p = (volatile unsigned char*)a;
    if (!sigsetjmp(jb, 1)) { if (write) *p = 1; else (void)*p; printf("  %s %#llx: no fault\n", name, a); }
    else printf("  %s %#llx: signal %d code %d si_addr %p\n", name, a, faultSig, faultCode, faultAddr);
}
static int mappable(unsigned long long a, void** where)
{
    void* p = mmap((void*)a, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return 0;
    *where = p; munmap(p, PG);
    return p == (void*)a ? 1 : 2;
}
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = onFault; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    unsigned long long lo = 0x8000000000ULL, hi = 0x10000000000ULL; void* w;
    while (hi - lo > PG) {
        unsigned long long mid = (lo + (hi - lo) / 2) & ~(PG - 1);
        int r = mappable(mid, &w);
        if (r == 2) printf("TOP %#llx moved to %p\n", mid, w);
        if (r) lo = mid; else hi = mid;
    }
    printf("TOP last mappable page %#llx, first refused %#llx\n", lo, hi);
    unsigned long long top = hi, size = 64ULL << 20;
    unsigned char* p = mmap((void*)(top - size), size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    printf("TOP 64 MB ending at the top: asked %#llx got %p errno %d\n", top - size, (void*)p, p == MAP_FAILED ? errno : 0);
    if (p == (void*)(top - size)) {
        tryAccess("write last page", top - PG, 1);
        tryAccess("read  top", top, 0);
        tryAccess("write top", top, 1);
        tryAccess("read  top+8", top + 8, 0);
        tryAccess("read  top+64K", top + 65536, 0);
        tryAccess("read  top+1G", top + (1ULL << 30), 0);
        tryAccess("write top+2G", top + (2ULL << 30), 1);
        tryAccess("read  top+4G", top + (4ULL << 30), 0);
        tryAccess("read  top+6G-1", top + (6ULL << 30) - 1, 0);
        /* can a mapping be made to span the top? */
        void* q = mmap((void*)(top - PG), 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        printf("TOP MAP_FIXED spanning the top: %p errno %d\n", q, q == MAP_FAILED ? errno : 0);
        q = mmap((void*)top, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        printf("TOP MAP_FIXED at the top: %p errno %d\n", q, q == MAP_FAILED ? errno : 0);
        q = mmap((void*)(top + (1ULL << 30)), PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        printf("TOP MAP_FIXED at top+1G: %p errno %d\n", q, q == MAP_FAILED ? errno : 0);
    }
    printf("TOP done\n");
    return 0;
}
