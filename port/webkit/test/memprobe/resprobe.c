/* resprobe: does an inaccessible reservation cost CPU page tables on the PS5?
 * Phases are matched by time with the kernel log's "page table CPU n/8192". */
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#define MB ((size_t)1 << 20)
#define HOLD 22
static sigjmp_buf jb; static volatile void* faultAddr; static volatile int faultSig, faultCode;
static void onFault(int sig, siginfo_t* si, void* ctx) { faultSig = sig; faultCode = si->si_code; faultAddr = si->si_addr; siglongjmp(jb, 1); }
static void phase(const char* what) { printf("RESPROBE %ld %s\n", (long)time(NULL), what); fflush(stdout); }
static void tryAccess(const char* name, volatile unsigned char* p, int write)
{
    faultSig = 0;
    if (!sigsetjmp(jb, 1)) { if (write) *p = 1; else (void)*p; printf("  access %s %p: no fault\n", name, (void*)p); }
    else printf("  access %s %p: signal %d code %d si_addr %p\n", name, (void*)p, faultSig, faultCode, faultAddr);
    fflush(stdout);
}
static void run(const char* name, int prot, int flags, size_t size)
{
    char b[128];
    unsigned char* p = mmap(NULL, size, prot, flags, -1, 0);
    if (p == MAP_FAILED) { snprintf(b, sizeof b, "%s mmap-failed errno %d", name, errno); phase(b); return; }
    snprintf(b, sizeof b, "%s reserved %zu MB at %p", name, size / MB, (void*)p); phase(b);
    tryAccess("read reserved", p + size / 2, 0);
    sleep(HOLD);
    unsigned char* q = mmap(p, 64 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    snprintf(b, sizeof b, "%s 64 MB RW over the start: %s errno %d", name, q == p ? "ok" : "FAILED", q == p ? 0 : errno); phase(b);
    if (q == p) { for (size_t o = 0; o < 64 * MB; o += 16384) p[o] = 1; tryAccess("write committed", p + 32 * MB, 1); }
    tryAccess("write just past committed", p + 64 * MB, 1);
    tryAccess("read far", p + size - 16384, 0);
    /* does a later mmap without a hint land inside the reservation? */
    void* other = mmap(NULL, 32 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    printf("  another 32 MB mmap: %p (%s the reservation)\n", other, (unsigned char*)other >= p && (unsigned char*)other < p + size ? "INSIDE" : "outside");
    if (other != MAP_FAILED) munmap(other, 32 * MB);
    sleep(HOLD);
    int r = munmap(p, size);
    snprintf(b, sizeof b, "%s unmapped (%d)", name, r); phase(b);
    sleep(HOLD);
}
int main(int argc, char** argv)
{
    size_t size = (size_t)SIZE_MB * MB;
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = onFault; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    phase("baseline"); sleep(HOLD);
    run("PROT_NONE", PROT_NONE, MAP_PRIVATE | MAP_ANON, size);
    run("MAP_GUARD", PROT_NONE, MAP_GUARD, size);
    run("RW (control)", PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 256 * MB);
    phase("done");
    return 0;
}
