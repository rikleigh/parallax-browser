/* probe.c - Spike 13 stage 0: which JIT memory schemes work in a title?
 * (README.md). Every result is a RESULT line (spikes/common/spike.h).
 *
 *   start         flexible/direct memory, the main thread's CPU mask
 *   rwx-anon      anonymous memory: mapped RWX; RW then mprotect RWX; executes?
 *   rwx-dmem      direct memory: RW then sceKernelMprotect RWX (0x07); executes?
 *   dual-shm-anon shm_open(SHM_ANON) mapped twice: RW view, RX view (at map
 *                 time, else RW + mprotect RX). Write via one, run via the other
 *   dual-shm-named the same with a named object (unlinked at once)
 *   dual-dmem     one direct-memory allocation mapped at two addresses
 *   cross-modify  per passing dual scheme: one thread runs code in the RX view
 *                 while another rewrites it through the RW view
 *   flip-cost     RX -> RW -> RX mprotect round trips, alone and with 4
 *                 threads running code on other pages of the same mapping
 *   flip-race     a thread runs code on the page being flipped; its faults
 *                 are retried from the signal handler once the page is RX
 *                 (the fallback scheme's handler)
 *   pool-size     256 MB pools (shm and direct): flexible memory before,
 *                 with 64 MB written, after unmapping
 *   cpu           a throughput-bound integer loop pinned to each CPU
 *   memfd_create is not in the SDK's libkernel stubs; SHM_ANON is what it
 *   wraps on FreeBSD.
 * Faults are caught (SIGSEGV/SIGBUS) and reported. */
#define SPIKE_NAME "13-title-jit"
#include "common/title.h"
#include "dmem.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/cpuset.h>

#define PAGE ((size_t)DMEM_PAGE)
#define MASKSZ 8 /* the kernel refuses CPU_SETSIZE/8 with ERANGE */
#define MB (1024.0 * 1024.0)

static double nowSec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double flexMB(void)
{
    size_t s = 0;
    sceKernelAvailableFlexibleMemorySize(&s);
    return s / MB;
}

static uint64_t threadMask(void)
{
    uint64_t m[2] = {0, 0};
    cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, MASKSZ, (cpuset_t *)m);
    return m[0];
}

static int pinSelf(int cpu)
{
    uint64_t m[2] = {1ull << cpu, 0};
    return cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, MASKSZ, (cpuset_t *)m) ? errno : 0;
}

/* ---- code ------------------------------------------------------------- */

/* int f(int x) { return x + 42; }; byte 4 is the immediate. */
static const uint8_t ADD42[] = {0x89, 0xf8, 0x83, 0xc0, 0x2a, 0xc3};
/* int g(void) { return imm32; } with the immediate 4-byte aligned at 4. */
static const uint8_t RETIMM[] = {0x90, 0x90, 0x90, 0xb8, 0, 0, 0, 0, 0xc3};
#define RETIMM_OFF 4

typedef int (*fn1_t)(int);
typedef int (*fn0_t)(void);

/* ---- fault guard on the main thread ------------------------------------ */

static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_sig;
static pthread_t g_mainThread;

/* flip-race: faults at addresses in [g_raceLo, g_raceHi) are retried once
 * the page is executable again. */
static _Atomic uintptr_t g_raceLo, g_raceHi;
static _Atomic int g_raceRX; /* 1 while the page is RX */
static _Atomic unsigned long g_raceFaults;

/* cross-modify: the executor thread's own guard. */
static sigjmp_buf g_execJmp;
static _Atomic(pthread_t) g_execThread;
static volatile sig_atomic_t g_execSig;

static void onFault(int sig, siginfo_t *info, void *ctx)
{
    (void)ctx;
    uintptr_t a = (uintptr_t)info->si_addr;
    if (a >= atomic_load(&g_raceLo) && a < atomic_load(&g_raceHi)) {
        atomic_fetch_add(&g_raceFaults, 1);
        while (!atomic_load(&g_raceRX)) {
        }
        return; /* re-executes the faulting instruction */
    }
    pthread_t self = pthread_self();
    if (pthread_equal(self, atomic_load(&g_execThread))) {
        g_execSig = sig;
        siglongjmp(g_execJmp, 1);
    }
    if (pthread_equal(self, g_mainThread)) {
        g_sig = sig;
        siglongjmp(g_jmp, 1);
    }
    /* Anyone else: die as usual. */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void installGuard(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = onFault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

static const char *sigName(int s)
{
    return s == SIGSEGV ? "SIGSEGV" : s == SIGBUS ? "SIGBUS" : s ? "signal" : "";
}

/* Calls f(1) under the guard; *sig gets the signal (0 if none). */
static int guardedCall1(void *at, int *sig)
{
    volatile int r = -1;
    g_sig = 0;
    if (sigsetjmp(g_jmp, 1) == 0)
        r = ((fn1_t)at)(1);
    *sig = g_sig;
    return r;
}

/* ---- start ------------------------------------------------------------ */

static void testStart(void)
{
    size_t conf = 0;
    sceKernelConfiguredFlexibleMemorySize(&conf);
    SPIKE_PASS("start", "pid %d, flexible %.1f MB available of %.1f MB configured, direct %zu MB, main thread mask %#llx, page %ld",
               (int)getpid(), flexMB(), conf / MB, sceKernelGetDirectMemorySize() >> 20,
               (unsigned long long)threadMask(), sysconf(_SC_PAGESIZE));
}

/* ---- rwx -------------------------------------------------------------- */

static void testRwxAnon(void)
{
    int sig;
    /* Mapped RWX at map time. */
    void *a = mmap(NULL, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
    int mapErr = a == MAP_FAILED ? errno : 0;
    int r1 = -1, s1 = 0;
    if (a != MAP_FAILED) {
        memcpy(a, ADD42, sizeof ADD42);
        r1 = guardedCall1(a, &s1);
        munmap(a, PAGE);
    }
    /* RW, then mprotect RWX. */
    void *b = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    int r2 = -1, s2 = 0, pErr = -1;
    if (b != MAP_FAILED) {
        memcpy(b, ADD42, sizeof ADD42);
        pErr = mprotect(b, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC) ? errno : 0;
        if (!pErr) {
            r2 = guardedCall1(b, &sig);
            s2 = sig;
        }
        munmap(b, PAGE);
    }
    SPIKE_RESULT("rwx-anon", r1 == 43 || r2 == 43 ? "PASS" : "FAIL",
                 "map RWX: errno %d, call -> %d %s; map RW + mprotect RWX: errno %d, call -> %d %s (want 43)",
                 mapErr, r1, sigName(s1), pErr, r2, sigName(s2));
}

typedef struct {
    void *addr;
    int64_t phys;
    size_t len;
} dcommit_t;

static int dmemAlloc(dcommit_t *c, size_t len)
{
    c->addr = NULL;
    c->len = len;
    c->phys = -1;
    return sceKernelAllocateMainDirectMemory(len, DMEM_UNIT, DMEM_TYPE_WB, &c->phys);
}

/* Maps phys anywhere with prot; returns rc, *out the address. */
static int dmemMap(int64_t phys, size_t len, int prot, void **out)
{
    void *a = NULL;
    int rc = sceKernelMapDirectMemory(&a, len, prot, 0, phys, DMEM_UNIT);
    *out = rc ? NULL : a;
    return rc;
}

static void testRwxDmem(void)
{
    dcommit_t c;
    int rc = dmemAlloc(&c, DMEM_UNIT);
    if (rc) {
        SPIKE_FAIL("rwx-dmem", "allocate rc 0x%08x", (unsigned)rc);
        return;
    }
    void *a;
    rc = dmemMap(c.phys, c.len, DMEM_PROT_CPU_RW, &a);
    if (rc) {
        SPIKE_FAIL("rwx-dmem", "map RW rc 0x%08x", (unsigned)rc);
        sceKernelReleaseDirectMemory(c.phys, c.len);
        return;
    }
    memcpy(a, ADD42, sizeof ADD42);
    int prc = sceKernelMprotect(a, c.len, 0x07);
    int sig = 0, r = -1;
    if (!prc)
        r = guardedCall1(a, &sig);
    /* Still writable after the call? (RWX really RWX) */
    int wrote = 0;
    if (r == 43) {
        g_sig = 0;
        if (sigsetjmp(g_jmp, 1) == 0) {
            ((volatile uint8_t *)a)[4] = 100;
            wrote = 1;
        }
    }
    int r2 = wrote ? guardedCall1(a, &sig) : -1;
    sceKernelMunmap(a, c.len);
    sceKernelReleaseDirectMemory(c.phys, c.len);
    SPIKE_RESULT("rwx-dmem", r == 43 && r2 == 101 ? "PASS" : "FAIL",
                 "RW map + sceKernelMprotect 0x07 rc 0x%x, call -> %d (want 43); write while RWX %s, call -> %d (want 101) %s",
                 (unsigned)prc, r, wrote ? "ok" : "faulted", r2, sigName(sig));
}

/* ---- dual mappings ---------------------------------------------------- */

typedef struct {
    const char *name;
    int ok;           /* passed the basic write-here/run-there test */
    uint8_t *rw, *rx; /* the two views */
    size_t len;
    int fd;
    dcommit_t dc;
} dual_t;

/* An RX view: at map time if allowed, else RW + mprotect RX. */
static uint8_t *mapRX(int fd, size_t len, int *direct, int *mapErr, int *protErr)
{
    *protErr = 0;
    void *p = mmap(NULL, len, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    *mapErr = p == MAP_FAILED ? errno : 0;
    *direct = p != MAP_FAILED;
    if (p != MAP_FAILED)
        return p;
    p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        *protErr = -errno;
        return NULL;
    }
    if (mprotect(p, len, PROT_READ | PROT_EXEC)) {
        *protErr = errno;
        munmap(p, len);
        return NULL;
    }
    return p;
}

/* Basic test on a dual view: write ADD42 via rw, run via rx, patch, run. */
static int basicDual(dual_t *d, char *detail, size_t n)
{
    memcpy(d->rw, ADD42, sizeof ADD42);
    int s1 = 0, s2 = 0;
    int r1 = guardedCall1(d->rx, &s1);
    d->rw[4] = 100;
    int r2 = r1 == 43 ? guardedCall1(d->rx, &s2) : -1;
    /* The RX view must not be writable (W^X). */
    int rxWritable = 0;
    g_sig = 0;
    if (sigsetjmp(g_jmp, 1) == 0) {
        ((volatile uint8_t *)d->rx)[PAGE - 1] = 0xcc;
        rxWritable = 1;
    }
    /* Nor the RW view executable. */
    memcpy(d->rw + PAGE, ADD42, sizeof ADD42);
    int s3 = 0;
    int r3 = guardedCall1(d->rw + PAGE, &s3);
    snprintf(detail, n, "views %p (rw) %p (rx); call via rx -> %d %s (want 43); patched via rw -> %d %s (want 101); rx view writable: %s; rw view executes: %s",
             (void *)d->rw, (void *)d->rx, r1, sigName(s1), r2, sigName(s2), rxWritable ? "YES" : "no",
             r3 == 43 ? "YES" : "no");
    return r1 == 43 && r2 == 101;
}

static void testDualShm(dual_t *d, int named)
{
    d->name = named ? "dual-shm-named" : "dual-shm-anon";
    d->len = 4 * PAGE;
    d->ok = 0;
    char path[64];
    snprintf(path, sizeof path, "/ps5wk-jit-%d", (int)getpid());
    d->fd = named ? shm_open(path, O_RDWR | O_CREAT | O_EXCL, 0600) : shm_open(SHM_ANON, O_RDWR, 0600);
    if (d->fd < 0) {
        SPIKE_FAIL(d->name, "shm_open errno %d", errno);
        return;
    }
    if (named)
        shm_unlink(path);
    if (ftruncate(d->fd, (off_t)d->len)) {
        SPIKE_FAIL(d->name, "ftruncate errno %d", errno);
        return;
    }
    void *rw = mmap(NULL, d->len, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
    if (rw == MAP_FAILED) {
        SPIKE_FAIL(d->name, "RW mmap errno %d", errno);
        return;
    }
    d->rw = rw;
    int direct, mapErr, protErr;
    d->rx = mapRX(d->fd, d->len, &direct, &mapErr, &protErr);
    if (!d->rx) {
        SPIKE_FAIL(d->name, "RX view: mmap RX errno %d, RW+mprotect RX errno %d", mapErr, protErr);
        return;
    }
    char detail[600];
    d->ok = basicDual(d, detail, sizeof detail);
    SPIKE_RESULT(d->name, d->ok ? "PASS" : "FAIL", "RX view %s (mmap RX errno %d); %s",
                 direct ? "mapped RX directly" : "mapped RW then mprotect RX", mapErr, detail);
}

static void testDualDmem(dual_t *d)
{
    d->name = "dual-dmem";
    d->len = DMEM_UNIT; /* 64 KiB = 4 pages */
    d->ok = 0;
    d->fd = -1;
    int rc = dmemAlloc(&d->dc, d->len);
    if (rc) {
        SPIKE_FAIL(d->name, "allocate rc 0x%08x", (unsigned)rc);
        return;
    }
    void *rw;
    rc = dmemMap(d->dc.phys, d->len, DMEM_PROT_CPU_RW, &rw);
    if (rc) {
        SPIKE_FAIL(d->name, "first (RW) map rc 0x%08x", (unsigned)rc);
        return;
    }
    d->rw = rw;
    void *rx;
    int rxRc = dmemMap(d->dc.phys, d->len, DMEM_PROT_CPU_RX, &rx);
    int direct = rxRc == 0, rwRc = 0, prc = 0;
    if (!direct) {
        rwRc = dmemMap(d->dc.phys, d->len, DMEM_PROT_CPU_RW, &rx);
        if (rwRc) {
            SPIKE_FAIL(d->name, "second map: RX rc 0x%08x, RW rc 0x%08x (one allocation, two addresses refused?)",
                       (unsigned)rxRc, (unsigned)rwRc);
            return;
        }
        prc = sceKernelMprotect(rx, d->len, DMEM_PROT_CPU_RX);
        if (prc) {
            SPIKE_FAIL(d->name, "second map RW ok, sceKernelMprotect RX rc 0x%08x", (unsigned)prc);
            return;
        }
    }
    d->rx = rx;
    char detail[600];
    d->ok = basicDual(d, detail, sizeof detail);
    SPIKE_RESULT(d->name, d->ok ? "PASS" : "FAIL", "second view %s (RX map rc 0x%x); %s",
                 direct ? "mapped RX directly" : "mapped RW then sceKernelMprotect RX", (unsigned)rxRc, detail);
}

/* ---- cross-modify ------------------------------------------------------ */

static _Atomic int g_execRun, g_execSeen;
static _Atomic unsigned long g_execCalls;
static uint8_t *g_execCode;

static void *executor(void *arg)
{
    (void)arg;
    atomic_store(&g_execThread, pthread_self());
    g_execSig = 0;
    if (sigsetjmp(g_execJmp, 1) == 0) {
        fn0_t f = (fn0_t)(void *)g_execCode;
        while (atomic_load_explicit(&g_execRun, memory_order_relaxed)) {
            atomic_store_explicit(&g_execSeen, f(), memory_order_relaxed);
            atomic_fetch_add_explicit(&g_execCalls, 1, memory_order_relaxed);
        }
    }
    atomic_store(&g_execThread, (pthread_t)0);
    return NULL;
}

static void testCrossModify(dual_t *d)
{
    char name[48];
    snprintf(name, sizeof name, "cross-modify/%s", d->name);
    if (!d->ok) {
        SPIKE_SKIP(name, "%s failed", d->name);
        return;
    }
    uint8_t *rw = d->rw + 2 * PAGE, *rx = d->rx + 2 * PAGE;
    memcpy(rw, RETIMM, sizeof RETIMM);
    _Atomic int *imm = (_Atomic int *)(rw + RETIMM_OFF);
    atomic_store(imm, 0);
    g_execCode = rx;
    atomic_store(&g_execRun, 1);
    atomic_store(&g_execSeen, -1);
    atomic_store(&g_execCalls, 0);
    pthread_t t;
    pthread_create(&t, NULL, executor, NULL);
    /* Wait for it to see 0. */
    double t0 = nowSec();
    while (atomic_load(&g_execSeen) != 0 && nowSec() - t0 < 1) {
    }
    const int N = 10000;
    int seen = 0, timeouts = 0;
    double maxUs = 0, sumUs = 0;
    for (int i = 1; i <= N && !g_execSig; i++) {
        double w0 = nowSec();
        atomic_store(imm, i);
        while (atomic_load(&g_execSeen) != i && nowSec() - w0 < 0.1 && !g_execSig) {
        }
        double us = (nowSec() - w0) * 1e6;
        if (atomic_load(&g_execSeen) == i) {
            seen++;
            sumUs += us;
            if (us > maxUs)
                maxUs = us;
        } else
            timeouts++;
    }
    /* Then a whole new function written next to it, called from this thread. */
    memcpy(d->rw + 3 * PAGE, ADD42, sizeof ADD42);
    int s = 0;
    int r = guardedCall1(d->rx + 3 * PAGE, &s);
    atomic_store(&g_execRun, 0);
    pthread_join(t, NULL);
    int ok = !g_execSig && seen == N && r == 43;
    SPIKE_RESULT(name, ok ? "PASS" : "FAIL",
                 "%d of %d patches seen by the running thread (%d timeouts of 100 ms), mean %.1f us, max %.1f us; executor %lu calls, fault %s; new code next to it -> %d %s",
                 seen, N, timeouts, seen ? sumUs / seen : 0, maxUs, atomic_load(&g_execCalls),
                 g_execSig ? sigName(g_execSig) : "none", r, sigName(s));
}

/* ---- flip cost and race ------------------------------------------------ */

#define FLIP_PAGES 8
static _Atomic int g_bgRun;
static _Atomic unsigned long g_bgCalls;

static void *bgRunner(void *arg)
{
    fn1_t f = (fn1_t)arg;
    while (atomic_load_explicit(&g_bgRun, memory_order_relaxed)) {
        if (f(1) != 43)
            break;
        atomic_fetch_add_explicit(&g_bgCalls, 1, memory_order_relaxed);
    }
    return NULL;
}

static double flipRounds(uint8_t *page, int n, int *err)
{
    *err = 0;
    double t0 = nowSec();
    for (int i = 0; i < n; i++) {
        if (mprotect(page, PAGE, PROT_READ | PROT_WRITE)) {
            *err = errno;
            break;
        }
        page[PAGE - 1] = (uint8_t)i;
        if (mprotect(page, PAGE, PROT_READ | PROT_EXEC)) {
            *err = errno;
            break;
        }
    }
    return (nowSec() - t0) * 1e6 / n;
}

static void testFlip(void)
{
    size_t len = FLIP_PAGES * PAGE;
    uint8_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        SPIKE_FAIL("flip-cost", "mmap errno %d", errno);
        return;
    }
    for (int i = 0; i < FLIP_PAGES; i++)
        memcpy(p + i * PAGE, ADD42, sizeof ADD42);
    if (mprotect(p, len, PROT_READ | PROT_EXEC)) {
        SPIKE_FAIL("flip-cost", "mprotect RX errno %d", errno);
        return;
    }
    int e1, e2;
    double alone = flipRounds(p, 2000, &e1);
    /* 4 threads running code on pages 1..4 while page 0 flips. */
    atomic_store(&g_bgRun, 1);
    atomic_store(&g_bgCalls, 0);
    pthread_t t[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, bgRunner, p + (i + 1) * PAGE);
    usleep(20000);
    double busy = flipRounds(p, 2000, &e2);
    atomic_store(&g_bgRun, 0);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    SPIKE_RESULT("flip-cost", !e1 && !e2 ? "PASS" : "FAIL",
                 "RX->RW->RX of one 16 KiB page: %.2f us alone, %.2f us with 4 threads running code on other pages of the mapping (%lu calls, none faulted); errno %d/%d",
                 alone, busy, atomic_load(&g_bgCalls), e1, e2);

    /* Race: a thread runs code on the page being flipped. */
    atomic_store(&g_raceLo, (uintptr_t)p);
    atomic_store(&g_raceHi, (uintptr_t)p + PAGE);
    atomic_store(&g_raceRX, 1);
    atomic_store(&g_raceFaults, 0);
    atomic_store(&g_bgRun, 1);
    atomic_store(&g_bgCalls, 0);
    pthread_t rt;
    pthread_create(&rt, NULL, bgRunner, p);
    usleep(20000);
    int err = 0, n = 2000;
    double t0 = nowSec();
    for (int i = 0; i < n && !err; i++) {
        atomic_store(&g_raceRX, 0);
        if (mprotect(p, PAGE, PROT_READ | PROT_WRITE))
            err = errno;
        else
            p[PAGE - 1] = (uint8_t)i;
        if (mprotect(p, PAGE, PROT_READ | PROT_EXEC))
            err = errno;
        atomic_store(&g_raceRX, 1);
        for (volatile int k = 0; k < 2000; k++) {
        }
    }
    double secs = nowSec() - t0;
    atomic_store(&g_bgRun, 0);
    pthread_join(rt, NULL);
    atomic_store(&g_raceLo, 0);
    atomic_store(&g_raceHi, 0);
    unsigned long calls = atomic_load(&g_bgCalls), faults = atomic_load(&g_raceFaults);
    SPIKE_RESULT("flip-race", !err ? "PASS" : "FAIL",
                 "%d flips in %.0f ms of the page another thread runs: %lu faults, all retried from the handler; runner %lu calls, all returned 43 (it stops on a wrong result); errno %d",
                 n, secs * 1e3, faults, calls, err);
    munmap(p, len);
}

/* ---- pool size --------------------------------------------------------- */

#define POOL ((size_t)256 << 20)
#define POOL_TOUCH ((size_t)64 << 20)

static void touchPool(uint8_t *p, size_t len)
{
    for (size_t o = 0; o < len; o += PAGE)
        p[o] = 0xc3;
}

static void testPoolShm(void)
{
    double f0 = flexMB();
    int fd = shm_open(SHM_ANON, O_RDWR, 0600);
    if (fd < 0 || ftruncate(fd, (off_t)POOL)) {
        SPIKE_FAIL("pool-size/shm", "shm_open/ftruncate errno %d", errno);
        if (fd >= 0)
            close(fd);
        return;
    }
    double f1 = flexMB();
    uint8_t *rw = mmap(NULL, POOL, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int direct, mapErr, protErr;
    uint8_t *rx = rw != MAP_FAILED ? mapRX(fd, POOL, &direct, &mapErr, &protErr) : NULL;
    if (rw == MAP_FAILED || !rx) {
        SPIKE_FAIL("pool-size/shm", "map 256 MB: rw %s, rx errno %d/%d", rw == MAP_FAILED ? "failed" : "ok", mapErr, protErr);
        if (rw != MAP_FAILED)
            munmap(rw, POOL);
        close(fd);
        return;
    }
    double f2 = flexMB();
    double t0 = nowSec();
    touchPool(rw, POOL_TOUCH);
    double ms = (nowSec() - t0) * 1e3;
    double f3 = flexMB();
    int s = 0;
    int r = guardedCall1(rx + POOL_TOUCH - PAGE, &s); /* 0xc3: ret, returns garbage but must not fault */
    (void)r;
    munmap(rx, POOL);
    munmap(rw, POOL);
    close(fd);
    double f4 = flexMB();
    SPIKE_RESULT("pool-size/shm", !s ? "PASS" : "FAIL",
                 "flexible %.1f MB before, %.1f after ftruncate 256 MB, %.1f with both views mapped, %.1f with 64 MB written (%.0f ms), %.1f after unmap+close; code in it runs: %s",
                 f0, f1, f2, f3, ms, f4, s ? sigName(s) : "yes");
}

static void testPoolDmem(int dualOk)
{
    if (!dualOk) {
        SPIKE_SKIP("pool-size/dmem", "dual-dmem failed");
        return;
    }
    double f0 = flexMB();
    dcommit_t c;
    int rc = dmemAlloc(&c, POOL);
    if (rc) {
        SPIKE_FAIL("pool-size/dmem", "allocate 256 MB rc 0x%08x", (unsigned)rc);
        return;
    }
    void *rw, *rx;
    int r1 = dmemMap(c.phys, POOL, DMEM_PROT_CPU_RW, &rw);
    int r2 = dmemMap(c.phys, POOL, DMEM_PROT_CPU_RX, &rx);
    int prc = 0;
    if (!r1 && r2) {
        r2 = dmemMap(c.phys, POOL, DMEM_PROT_CPU_RW, &rx);
        if (!r2)
            prc = sceKernelMprotect(rx, POOL, DMEM_PROT_CPU_RX);
    }
    if (r1 || r2 || prc) {
        SPIKE_FAIL("pool-size/dmem", "maps rc 0x%x / 0x%x, mprotect 0x%x", (unsigned)r1, (unsigned)r2, (unsigned)prc);
        if (!r1)
            sceKernelMunmap(rw, POOL);
        if (!r2)
            sceKernelMunmap(rx, POOL);
        sceKernelReleaseDirectMemory(c.phys, POOL);
        return;
    }
    double f1 = flexMB();
    touchPool(rw, POOL_TOUCH);
    double f2 = flexMB();
    int s = 0;
    guardedCall1((uint8_t *)rx + POOL_TOUCH - PAGE, &s);
    sceKernelMunmap(rw, POOL);
    sceKernelMunmap(rx, POOL);
    sceKernelReleaseDirectMemory(c.phys, POOL);
    SPIKE_RESULT("pool-size/dmem", !s ? "PASS" : "FAIL",
                 "flexible %.1f MB before, %.1f with both 256 MB views mapped, %.1f with 64 MB written, %.1f after release; code in it runs: %s",
                 f0, f1, f2, flexMB(), s ? sigName(s) : "yes");
}

/* ---- cpu --------------------------------------------------------------- */

static volatile uint64_t g_sink;

static double wideMs(void)
{
    uint64_t a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8;
    double t0 = nowSec();
    for (int i = 0; i < 6000000; i++) {
#define XS(x) x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        XS(a) XS(b) XS(c) XS(d) XS(e) XS(f) XS(g) XS(h)
#undef XS
        __asm__ volatile("" : "+r"(a), "+r"(b), "+r"(c), "+r"(d), "+r"(e), "+r"(f), "+r"(g), "+r"(h));
    }
    g_sink = a ^ b ^ c ^ d ^ e ^ f ^ g ^ h;
    return (nowSec() - t0) * 1e3;
}

typedef struct {
    int cpu, pinErr;
    _Atomic int done;
    double ms[3];
} cpujob_t;

static void *cpuRunner(void *arg)
{
    cpujob_t *j = arg;
    j->pinErr = pinSelf(j->cpu);
    if (!j->pinErr)
        for (int r = 0; r < 3; r++)
            j->ms[r] = wideMs();
    atomic_store(&j->done, 1);
    return NULL;
}

static void testCpu(void)
{
    uint64_t mask = threadMask();
    char out[700];
    size_t o = 0;
    out[0] = 0;
    double best = 1e9, worst = 0;
    for (int c = 0; c < 16; c++) {
        cpujob_t *j = calloc(1, sizeof *j);
        j->cpu = c;
        pthread_t t;
        pthread_create(&t, NULL, cpuRunner, j);
        double t0 = nowSec();
        while (!atomic_load(&j->done) && nowSec() - t0 < 6) {
            usleep(2000);
        }
        if (!atomic_load(&j->done)) {
            o += snprintf(out + o, sizeof out - o, "%d:STARVED ", c);
            pthread_detach(t); /* j leaked: the thread still uses it */
            continue;
        }
        pthread_join(t, NULL);
        if (j->pinErr)
            o += snprintf(out + o, sizeof out - o, "%d:errno%d ", c, j->pinErr);
        else {
            double m = j->ms[0];
            for (int r = 1; r < 3; r++)
                if (j->ms[r] < m)
                    m = j->ms[r];
            if (m < best)
                best = m;
            if (m > worst)
                worst = m;
            o += snprintf(out + o, sizeof out - o, "%d:%.0f ", c, m);
        }
        free(j);
    }
    SPIKE_PASS("cpu", "process mask %#llx; WIDE loop, best of 3, ms per CPU (smtprobe's loop; payload CPU 14 alone measured the same way): %s; fastest %.0f, slowest %.0f",
               (unsigned long long)mask, out, best, worst);
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    g_mainThread = pthread_self();
    spike_title_init("spike13", LOG_HOST, LOG_PORT);
    spike_log("ps5webkit spike 13 stage 0 (title): JIT memory probes, pid %d", (int)getpid());
    installGuard();
    testStart();
    testRwxAnon();
    testRwxDmem();
    dual_t anon = {0}, named = {0}, dm = {0};
    testDualShm(&anon, 0);
    testDualShm(&named, 1);
    testDualDmem(&dm);
    testCrossModify(&anon);
    testCrossModify(&dm);
    testFlip();
    testPoolShm();
    testPoolDmem(dm.ok);
    testCpu();
    spike_log("memory at the end: flexible %.1f MB", flexMB());
    spike_log("spike 13 done");
    spike_log_flush(2000);
    return 0;
}
