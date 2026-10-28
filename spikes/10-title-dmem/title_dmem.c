/* Spike 10 - a direct-memory heap in a PS5 title (R8).
 *
 * A registered title has ~420 MB of flexible memory and gigabytes of direct
 * memory. The browser's title must also host the hardware video decoder,
 * whose CPU part is flexible memory (223 MB for 8K HEVC at depth 4). This
 * title moves its own malloc heap into direct memory (dheap.c) and checks:
 *
 *   start       flexible / direct memory at start
 *   reserve,    16 GiB reserved at 0x1000000000; 64 MB chunks committed
 *   commit,     (main direct memory, type 12, fixed map), every page written
 *   decommit    and verified, up to 4 GB; decommit; flexible unchanged?
 *   cxx, glib   C++ new/delete and glib's g_malloc land in the heap
 *   stress      8 threads, 16 B - 4 MB, cross-thread frees, realloc,
 *               posix_memalign, calloc; 1.5 GB live at peak
 *   stacks      64 threads on 2 MB direct-memory stacks
 *   decoder     1.5 GB live in the heap + the 8K HEVC decoder at depth 4
 *   throughput  the heap against libc's malloc
 *   jit         RW -> RX with sceKernelMprotect, then call the code
 *
 * Built twice (build-title.sh): the normal title, and with -DPAD a title
 * padded with ~100 MB of non-zero text and rodata, which only reports its
 * start-up memory and what touching the padding costs (does a big ELF eat
 * flexible memory?).
 */
#define SPIKE_NAME "10-title-dmem"
#include "../common/title.h"
#include "dmem.h"
#include "../09-hw-decode/au.h"
#include "../09-hw-decode/vdec2.h"

#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/stat.h>

#ifndef LOG_HOST
#define LOG_HOST "192.168.1.100"
#endif
#ifndef LOG_PORT
#define LOG_PORT 9999
#endif

#define MB (1024.0 * 1024.0)
#define CLIP "/data/ps5webkit/hwdec/vrclip_7680x3840.mp4"

int cxx_test(char *out, size_t n);  /* cxx_test.cpp */
int glib_test(char *out, size_t n); /* glib_test.c, or -1 when not linked */

size_t dmem_flexible_free(void)
{
    size_t f = 0;
    sceKernelAvailableFlexibleMemorySize(&f);
    return f;
}

size_t dmem_direct_free(void)
{
    int64_t start = 0;
    size_t size = 0;
    sceKernelAvailableDirectMemorySize(0, (int64_t)sceKernelGetDirectMemorySize(), DMEM_PAGE, &start,
                                       &size);
    return size;
}

static double fmb(size_t b)
{
    return (double)b / MB;
}

static uint64_t rnd(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

/* Log-uniform-ish: a power of two between lo and hi, then uniform inside it. */
static size_t rand_size(uint64_t *s, unsigned lo_log2, unsigned hi_log2)
{
    const unsigned e = lo_log2 + (unsigned)(rnd(s) % (hi_log2 - lo_log2));
    return ((size_t)1 << e) + (size_t)(rnd(s) % ((size_t)1 << e));
}

static size_t g_flex_ctor, g_direct_ctor;
static dheap_stats_t g_heap_ctor;
__attribute__((constructor(101))) static void early(void)
{
    g_flex_ctor = dmem_flexible_free();
    g_direct_ctor = dmem_direct_free();
    dheap_stats(&g_heap_ctor);
}

static void log_heap(const char *when)
{
    dheap_stats_t h;
    dheap_stats(&h);
    spike_log("heap %s: range %p+%zu GB (%s rc 0x%x), prot 0x%x, %s, mapped %.1f MB (peak %.1f), "
              "%u segments, %llu maps / %llu unmaps (%llu refused, %llu failed), %u arenas, "
              "%llu libc fallbacks",
              when, (void *)h.base, h.range >> 30, h.init_step, (unsigned)h.init_rc, h.prot,
              h.alloc_main == 1 ? "AllocateMainDirectMemory" : "AllocateDirectMemory", fmb(h.mapped),
              fmb(h.peak), h.segments, h.maps, h.unmaps, h.refused_unmaps, h.map_failures, h.arenas,
              h.libc_fallbacks);
}

/* -- 1. start --------------------------------------------------------------- */
static size_t g_flex_start;

static void test_start(void)
{
    size_t conf = 0;
    const int crc = sceKernelConfiguredFlexibleMemorySize(&conf);
    g_flex_start = dmem_flexible_free();
    SPIKE_PASS("start",
               "flexible available %.1f MB at main (%.1f MB at the first constructor), configured "
               "%.1f MB (rc 0x%x); direct memory %.0f MB, largest free block %.0f MB (%.0f MB at the "
               "first constructor); heap mapped %.1f MB before main",
               fmb(g_flex_start), fmb(g_flex_ctor), fmb(conf), (unsigned)crc,
               fmb(sceKernelGetDirectMemorySize()), fmb(dmem_direct_free()), fmb(g_direct_ctor),
               fmb(g_heap_ctor.mapped));
    log_heap("at main");
}

#ifdef PAD
/* -- 6. padding (pad.S): ~50 MB text + ~50 MB rodata, all non-zero ------------ */
extern const uint8_t ps5wk_pad_text[], ps5wk_pad_text_end[];
extern const uint8_t ps5wk_pad_ro[], ps5wk_pad_ro_end[];

static uint64_t touch(const uint8_t *a, const uint8_t *b)
{
    uint64_t sum = 0;
    for (const volatile uint8_t *p = a; p < b; p += 4096)
        sum += *p;
    return sum;
}

/* Text is execute-only: run every page (4095 NOPs + RET) instead of reading it. */
static uint64_t run_text(const uint8_t *a, const uint8_t *b)
{
    uint64_t pages = 0;
    for (const uint8_t *p = a; p < b; p += 4096, pages++)
        ((void (*)(void))(uintptr_t)p)();
    return pages;
}

static void test_pad(void)
{
    const size_t f0 = dmem_flexible_free();
    const double t0 = spike_now_ms();
    const uint64_t s1 = run_text(ps5wk_pad_text, ps5wk_pad_text_end);
    const size_t f1 = dmem_flexible_free();
    const uint64_t s2 = touch(ps5wk_pad_ro, ps5wk_pad_ro_end);
    const size_t f2 = dmem_flexible_free();
    SPIKE_PASS("binary-size",
               "padded build: %.1f MB text pad + %.1f MB rodata pad (%llu text pages run, rodata sum "
               "0x%llx); flexible at first constructor %.1f MB, at main %.1f MB, after running the "
               "text pad %.1f MB, "
               "after the rodata pad %.1f MB (%.0f ms)",
               fmb((size_t)(ps5wk_pad_text_end - ps5wk_pad_text)),
               fmb((size_t)(ps5wk_pad_ro_end - ps5wk_pad_ro)), (unsigned long long)s1,
               (unsigned long long)s2, fmb(g_flex_ctor), fmb(f0), fmb(f1), fmb(f2),
               spike_now_ms() - t0);
}
#endif

/* -- 2. reserve / commit / decommit ------------------------------------------ */
#define RANGE_HINT ((uintptr_t)0x1000000000ull)
#define RANGE_LEN ((size_t)16 << 30)
#define CHUNK ((size_t)64 << 20)
#define MAX_CHUNKS 64 /* 4 GB */
/* Slots in the range after the chunks, for the JIT page and thread stacks. */
#define JIT_OFF ((size_t)5 << 30)
#define STACK_OFF ((size_t)6 << 30)

static uint8_t *g_range;
static int g_prot = DMEM_PROT_CPU_RW;

typedef struct {
    void *addr;
    int64_t phys;
    size_t len;
} commit_t;

static int commit(commit_t *c, void *at, size_t len, int prot, int *map_rc)
{
    c->addr = NULL;
    c->phys = -1;
    c->len = len;
    int rc = sceKernelAllocateMainDirectMemory(len, DMEM_PAGE, DMEM_TYPE_WB, &c->phys);
    if (rc != 0)
        return rc;
    void *a = at;
    rc = sceKernelMapDirectMemory(&a, len, prot, DMEM_MAP_FIXED, c->phys, DMEM_PAGE);
    if (map_rc)
        *map_rc = rc;
    if (rc != 0 || a != at) {
        if (rc == 0)
            sceKernelMunmap(a, len);
        sceKernelReleaseDirectMemory(c->phys, len);
        c->phys = -1;
        return rc ? rc : -2;
    }
    c->addr = a;
    return 0;
}

/* Returns munmap's rc; *va_freed: whether munmap handed the VA back (the
 * reservation probe succeeded; it is ours again either way). */
static int decommit(commit_t *c, int *va_freed, int *release_rc)
{
    int rc = sceKernelMunmap(c->addr, c->len);
    void *again = c->addr;
    int r = sceKernelReserveVirtualRange(&again, c->len, DMEM_MAP_FIXED | DMEM_MAP_NO_OVERWRITE,
                                         DMEM_PAGE);
    if (va_freed)
        *va_freed = r == 0;
    int rr = sceKernelReleaseDirectMemory(c->phys, c->len);
    if (release_rc)
        *release_rc = rr;
    c->addr = NULL;
    c->phys = -1;
    return rc;
}

static void write_pages(uint8_t *p, size_t len, uint64_t seed)
{
    for (size_t off = 0; off < len; off += 4096)
        *(volatile uint64_t *)(p + off) = seed ^ off;
}

static size_t verify_pages(const uint8_t *p, size_t len, uint64_t seed)
{
    size_t bad = 0;
    for (size_t off = 0; off < len; off += 4096)
        bad += *(const volatile uint64_t *)(p + off) != (seed ^ off);
    return bad;
}

static size_t nonzero_pages(const uint8_t *p, size_t len)
{
    size_t nz = 0;
    for (size_t off = 0; off < len; off += DMEM_PAGE) {
        const volatile uint64_t *q = (const volatile uint64_t *)(p + off);
        nz += (q[0] | q[1] | q[(DMEM_PAGE / 8) - 1]) != 0;
    }
    return nz;
}

static void test_range(void)
{
    const size_t flex0 = dmem_flexible_free(), direct0 = dmem_direct_free();
    void *base = (void *)RANGE_HINT;
    int rc = sceKernelReserveVirtualRange(&base, RANGE_LEN, DMEM_MAP_FIXED | DMEM_MAP_NO_OVERWRITE,
                                          DMEM_PAGE);
    if (rc != 0 || base != (void *)RANGE_HINT) {
        SPIKE_FAIL("reserve", "16 GiB at 0x1000000000: rc 0x%08x, got %p", (unsigned)rc, base);
        return;
    }
    g_range = base;
    /* A second reservation over it must be refused (NO_OVERWRITE). */
    void *over = (void *)(RANGE_HINT + CHUNK);
    const int orc = sceKernelReserveVirtualRange(&over, CHUNK, DMEM_MAP_FIXED | DMEM_MAP_NO_OVERWRITE,
                                                 DMEM_PAGE);
    SPIKE_PASS("reserve", "16 GiB at %p (FIXED|NO_OVERWRITE, 16 KiB): rc 0; a second reservation "
               "inside it: rc 0x%08x; flexible %.1f MB after", base, (unsigned)orc,
               fmb(dmem_flexible_free()));

    /* Which protection does a fixed map over the reservation accept? */
    commit_t c;
    int rc03 = 1, rcf2 = 1;
    int arc = commit(&c, g_range, CHUNK, DMEM_PROT_CPU_RW, &rc03);
    if (arc == 0)
        decommit(&c, NULL, NULL);
    int arc2 = commit(&c, g_range, CHUNK, DMEM_PROT_CPU_GPU_RW, &rcf2);
    if (arc2 == 0)
        decommit(&c, NULL, NULL);
    /* The ranged allocator, for comparison. */
    int64_t phys = -1;
    const int rrc = sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(), CHUNK,
                                                  DMEM_PAGE, DMEM_TYPE_WB, &phys);
    if (rrc == 0)
        sceKernelReleaseDirectMemory(phys, CHUNK);
    g_prot = rc03 == 0 ? DMEM_PROT_CPU_RW : DMEM_PROT_CPU_GPU_RW;
    spike_log("prot probe: AllocateMain+Map 0x03 -> alloc/map rc 0x%x / 0x%x; 0xf2 -> 0x%x / 0x%x; "
              "AllocateDirectMemory -> 0x%x", (unsigned)arc, (unsigned)rc03, (unsigned)arc2,
              (unsigned)rcf2, (unsigned)rrc);

    static commit_t chunks[MAX_CHUNKS];
    int n = 0, stop_rc = 0, mprot_rc = 0;
    size_t fresh_nonzero = 0, fresh_pages = 0;
    const double t0 = spike_now_ms();
    for (; n < MAX_CHUNKS; n++) {
        uint8_t *at = g_range + (size_t)n * CHUNK;
        int mrc = 0;
        rc = commit(&chunks[n], at, CHUNK, g_prot, &mrc);
        if (rc != 0) {
            stop_rc = rc;
            break;
        }
        /* The recipe's last step: protect to the final protection. */
        const int prc = sceKernelMprotect(at, CHUNK, DMEM_PROT_CPU_RW);
        if (prc && !mprot_rc)
            mprot_rc = prc;
        fresh_nonzero += nonzero_pages(at, CHUNK);
        fresh_pages += CHUNK / DMEM_PAGE;
        write_pages(at, CHUNK, 0x5a5a000000000000ull + (uint64_t)n);
    }
    const double commit_ms = spike_now_ms() - t0;
    const size_t flex_peak = dmem_flexible_free(), direct_peak = dmem_direct_free();
    size_t bad = 0;
    for (int i = 0; i < n; i++)
        bad += verify_pages(chunks[i].addr, CHUNK, 0x5a5a000000000000ull + (uint64_t)i);
    const size_t total = (size_t)n * CHUNK;
    if (n == MAX_CHUNKS || stop_rc)
        SPIKE_RESULT("commit", n > 0 && bad == 0 ? "PASS" : "FAIL",
                     "prot 0x03 map rc 0x%x, 0xf2 map rc 0x%x -> using 0x%x; %d x 64 MB = %.0f MB "
                     "committed in %.0f ms (%s), mprotect rc 0x%x; every 4 KiB written and read back: "
                     "%zu bad; fresh pages non-zero: %zu of %zu; flexible %.1f MB before, %.1f MB "
                     "with %.0f MB committed; largest free direct block %.0f -> %.0f MB",
                     (unsigned)rc03, (unsigned)rcf2, g_prot, n, fmb(total), commit_ms,
                     stop_rc ? "stopped by an allocation refusal" : "stopped at the 4 GB cap",
                     (unsigned)mprot_rc, bad, fresh_nonzero, fresh_pages, fmb(flex0), fmb(flex_peak),
                     fmb(total), fmb(direct0), fmb(direct_peak));
    if (stop_rc)
        spike_log("commit stopped at chunk %d: rc 0x%08x", n, (unsigned)stop_rc);

    /* Decommit everything; then a recommit at the same address, then a
     * partial release (what a page allocator's decommit of part of a span needs). */
    int va_freed = 0, unmap_rc = 0, rel_rc = 0;
    const double d0 = spike_now_ms();
    for (int i = 0; i < n; i++) {
        int vf = 0, rr = 0;
        const int u = decommit(&chunks[i], &vf, &rr);
        if (u && !unmap_rc)
            unmap_rc = u;
        if (rr && !rel_rc)
            rel_rc = rr;
        va_freed += vf;
    }
    const double decommit_ms = spike_now_ms() - d0;
    const size_t flex_after = dmem_flexible_free(), direct_after = dmem_direct_free();
    int re_rc = commit(&chunks[0], g_range, CHUNK, g_prot, NULL);
    size_t re_nonzero = 0, re_bad = 0;
    if (re_rc == 0) {
        re_nonzero = nonzero_pages(g_range, CHUNK);
        write_pages(g_range, CHUNK, 7);
        re_bad = verify_pages(g_range, CHUNK, 7);
        decommit(&chunks[0], NULL, NULL);
    }
    /* Partial: 64 MB mapped, the last 16 MB unmapped and released on its own. */
    int p_rc = commit(&chunks[0], g_range, CHUNK, g_prot, NULL), pu = -1, pr = -1, pr2 = -1;
    size_t d_mid = 0;
    if (p_rc == 0) {
        const size_t keep = CHUNK - (16u << 20);
        pu = sceKernelMunmap(g_range + keep, CHUNK - keep);
        pr = sceKernelReleaseDirectMemory(chunks[0].phys + (int64_t)keep, CHUNK - keep);
        write_pages(g_range, keep, 9);
        d_mid = verify_pages(g_range, keep, 9);
        sceKernelMunmap(g_range, keep);
        pr2 = sceKernelReleaseDirectMemory(chunks[0].phys, keep);
        void *again = g_range;
        sceKernelReserveVirtualRange(&again, CHUNK, DMEM_MAP_FIXED | DMEM_MAP_NO_OVERWRITE, DMEM_PAGE);
    }
    SPIKE_RESULT("decommit", unmap_rc == 0 && rel_rc == 0 && re_rc == 0 && re_bad == 0 ? "PASS" : "FAIL",
                 "%d chunks: munmap rc 0x%x, release rc 0x%x in %.0f ms; the VA was free again after "
                 "munmap for %d of %d (re-reserved); recommit at the same address rc 0x%x, %zu of "
                 "%zu pages non-zero before writing (old data?), %zu bad after; partial: unmap+release "
                 "the last 16 MB of a 64 MB commit rc 0x%x / 0x%x, first 48 MB still fine (%zu bad), "
                 "release of the rest rc 0x%x; largest free direct block %.0f MB",
                 n, (unsigned)unmap_rc, (unsigned)rel_rc, decommit_ms, va_freed, n, (unsigned)re_rc,
                 re_nonzero, CHUNK / DMEM_PAGE, re_bad, (unsigned)pu, (unsigned)pr, d_mid,
                 (unsigned)pr2, fmb(direct_after));
    const long dflex = (long)flex0 - (long)flex_peak;
    SPIKE_RESULT("flexible-unchanged", flex_peak == flex0 && flex_after == flex0 ? "PASS" : "FAIL",
                 "flexible %.2f MB before, %.2f MB with %.0f MB of direct memory committed (%+ld KB), "
                 "%.2f MB after decommit", fmb(flex0), fmb(flex_peak), fmb(total), -dflex / 1024,
                 fmb(flex_after));
}

/* -- 3. JIT: RW -> RX ---------------------------------------------------------- */
static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_sig;

static void on_fault(int sig)
{
    g_sig = sig;
    siglongjmp(g_jmp, 1);
}

static void test_jit(void)
{
    if (!g_range) {
        SPIKE_SKIP("jit", "no range");
        return;
    }
    /* int f(int x) { return x + 42; } */
    static const uint8_t code[] = {0x89, 0xf8, 0x83, 0xc0, 0x2a, 0xc3};
    uint8_t *at = g_range + JIT_OFF;
    commit_t c;
    int rwx_rc = 0, rx_map_rc = 0;
    /* Exec at map time: RWX, then RX. */
    if (commit(&c, at, DMEM_UNIT, 0x07, &rwx_rc) == 0)
        decommit(&c, NULL, NULL);
    if (commit(&c, at, DMEM_UNIT, DMEM_PROT_CPU_RX, &rx_map_rc) == 0)
        decommit(&c, NULL, NULL);
    int rc = commit(&c, at, DMEM_UNIT, g_prot, NULL);
    if (rc != 0) {
        SPIKE_FAIL("jit", "commit rc 0x%08x", (unsigned)rc);
        return;
    }
    memcpy(at, code, sizeof code);
    const int prx = sceKernelMprotect(at, DMEM_UNIT, DMEM_PROT_CPU_RX);
    struct sigaction sa, old_segv, old_bus;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fault;
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);
    int result = -1;
    g_sig = 0;
    if (prx == 0 && sigsetjmp(g_jmp, 1) == 0) {
        int (*fn)(int) = (int (*)(int))(void *)at;
        result = fn(1);
    }
    /* Back to RW and patch (what a JIT does next), then RX again. */
    int prw = -1, prx2 = -1, result2 = -1;
    if (result == 43) {
        prw = sceKernelMprotect(at, DMEM_UNIT, DMEM_PROT_CPU_RW);
        if (prw == 0) {
            at[4] = 100; /* add eax, 100 */
            prx2 = sceKernelMprotect(at, DMEM_UNIT, DMEM_PROT_CPU_RX);
            if (prx2 == 0 && sigsetjmp(g_jmp, 1) == 0) {
                int (*fn)(int) = (int (*)(int))(void *)at;
                result2 = fn(1);
            }
        }
    }
    const int prwx = sceKernelMprotect(at, DMEM_UNIT, 0x07);
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);
    sceKernelMprotect(at, DMEM_UNIT, DMEM_PROT_CPU_RW);
    decommit(&c, NULL, NULL);
    SPIKE_RESULT("jit", result == 43 && result2 == 101 ? "PASS" : "FAIL",
                 "map with RWX rc 0x%x, with RX rc 0x%x; RW map + mprotect RX rc 0x%x, call -> %d "
                 "(want 43)%s; mprotect RW rc 0x%x, patch, RX rc 0x%x, call -> %d (want 101); "
                 "mprotect RWX rc 0x%x",
                 (unsigned)rwx_rc, (unsigned)rx_map_rc, (unsigned)prx, result,
                 g_sig ? (g_sig == SIGSEGV ? " SIGSEGV" : " SIGBUS") : "", (unsigned)prw,
                 (unsigned)prx2, result2, (unsigned)prwx);
}

/* -- 4a/b. C++ and glib ---------------------------------------------------------- */
static void test_cxx_glib(void)
{
    char d[512];
    int r = cxx_test(d, sizeof d);
    SPIKE_RESULT("cxx", r == 0 ? "PASS" : "FAIL", "%s", d);
    r = glib_test(d, sizeof d);
    if (r < 0)
        SPIKE_SKIP("glib", "%s", d);
    else
        SPIKE_RESULT("glib", r == 0 ? "PASS" : "FAIL", "%s", d);
}

/* -- 4d. multi-threaded stress ----------------------------------------------------- */
#define NSLOTS 16384
#define STRESS_THREADS 8
#define STRESS_TARGET ((long long)1536 << 20)
#define CHURN_OPS 40000

typedef struct {
    uint64_t size;
    uint32_t tag;
    uint32_t owner;
} hdr_t;

static _Atomic(uint8_t *) slots[NSLOTS];
static atomic_llong s_live, s_peak;
static atomic_ullong s_allocs, s_frees, s_cross, s_reallocs, s_memalign, s_calloc, s_bad, s_fail,
    s_ops;
static atomic_int s_phase; /* 0 ramp, 1 churn */
static atomic_int s_done;   /* workers finished */

static uint64_t tail_of(uint64_t size, uint32_t tag)
{
    return size ^ ((uint64_t)tag * 0x9e3779b97f4a7c15ull);
}

static void stamp(uint8_t *p, size_t size, uint32_t tag, uint32_t owner)
{
    hdr_t h = {size, tag, owner};
    memcpy(p, &h, sizeof h);
    if (size >= 24) {
        uint64_t t = tail_of(size, tag);
        memcpy(p + size - 8, &t, 8);
    }
}

static int check(const uint8_t *p, hdr_t *h)
{
    memcpy(h, p, sizeof *h);
    if (h->size < 16 || h->size > (8u << 20))
        return 0;
    if (h->size >= 24) {
        uint64_t t;
        memcpy(&t, p + h->size - 8, 8);
        if (t != tail_of(h->size, h->tag))
            return 0;
    }
    return 1;
}

static void live_add(long long d)
{
    long long v = atomic_fetch_add(&s_live, d) + d;
    long long pk = atomic_load(&s_peak);
    while (v > pk && !atomic_compare_exchange_weak(&s_peak, &pk, v))
        ;
}

static void release_block(uint8_t *old, uint32_t me)
{
    hdr_t h;
    if (!check(old, &h)) {
        if (atomic_fetch_add(&s_bad, 1) < 4)
            spike_log("stress: corrupt block %p (size %llu tag %u)", old, (unsigned long long)h.size,
                      h.tag);
        return; /* leak it rather than free garbage */
    }
    if (!dheap_owns(old))
        atomic_fetch_add(&s_fail, 1);
    atomic_fetch_add(&s_cross, h.owner != me);
    live_add(-(long long)h.size);
    free(old);
    atomic_fetch_add(&s_frees, 1);
}

static void put(int i, uint8_t *p, uint32_t me)
{
    uint8_t *old = atomic_exchange(&slots[i], p);
    if (old)
        release_block(old, me);
}

static uint8_t *alloc_block(uint64_t *rs, uint32_t me, int fill)
{
    const size_t size = rand_size(rs, 4, 22); /* 16 B .. 4 MB - 1 */
    const size_t sz = size > (4u << 20) ? (4u << 20) : size;
    const unsigned kind = (unsigned)(rnd(rs) % 10);
    uint8_t *p = NULL;
    if (kind == 0) {
        const size_t align = (size_t)16 << (rnd(rs) % 9); /* 16 .. 4096 */
        void *q = NULL;
        if (posix_memalign(&q, align, sz) == 0 && ((uintptr_t)q & (align - 1)) == 0)
            p = q;
        else if (q)
            atomic_fetch_add(&s_bad, 1);
        atomic_fetch_add(&s_memalign, 1);
    } else if (kind == 1) {
        p = calloc(1, sz);
        if (p) {
            uint64_t z0, z1;
            memcpy(&z0, p, 8);
            memcpy(&z1, p + sz - 8, 8);
            if (z0 | z1)
                atomic_fetch_add(&s_bad, 1);
        }
        atomic_fetch_add(&s_calloc, 1);
    } else {
        p = malloc(sz);
    }
    if (!p) {
        atomic_fetch_add(&s_fail, 1);
        return NULL;
    }
    const uint32_t tag = (uint32_t)rnd(rs);
    if (fill && sz > 24)
        memset(p + 16, (int)(tag & 0xff), sz - 24);
    stamp(p, sz, tag, me);
    live_add((long long)sz);
    atomic_fetch_add(&s_allocs, 1);
    return p;
}

static void realloc_op(uint64_t *rs, int i, uint32_t me)
{
    uint8_t *old = atomic_exchange(&slots[i], NULL);
    if (!old)
        return;
    hdr_t h;
    if (!check(old, &h)) {
        atomic_fetch_add(&s_bad, 1);
        return;
    }
    size_t ns = rand_size(rs, 4, 22);
    if (ns > (4u << 20))
        ns = 4u << 20;
    uint8_t *q = realloc(old, ns);
    if (!q) {
        atomic_fetch_add(&s_fail, 1);
        put(i, old, me);
        return;
    }
    hdr_t h2;
    memcpy(&h2, q, sizeof h2);
    if (h2.size != h.size || h2.tag != h.tag)
        atomic_fetch_add(&s_bad, 1);
    stamp(q, ns, h.tag, me);
    live_add((long long)ns - (long long)h.size);
    atomic_fetch_add(&s_reallocs, 1);
    put(i, q, me);
}

static void *stress_thread(void *arg)
{
    const uint32_t me = (uint32_t)(uintptr_t)arg;
    uint64_t rs = 0x9e3779b97f4a7c15ull * (me + 1);
    while (atomic_load(&s_live) < STRESS_TARGET) {
        const int i = (int)(rnd(&rs) % NSLOTS);
        if (rnd(&rs) % 10 == 0)
            realloc_op(&rs, i, me);
        else {
            uint8_t *p = alloc_block(&rs, me, 1);
            if (!p)
                break;
            put(i, p, me);
        }
        atomic_fetch_add(&s_ops, 1);
    }
    atomic_store(&s_phase, 1);
    for (int op = 0; op < CHURN_OPS; op++) {
        const int i = (int)(rnd(&rs) % NSLOTS);
        const unsigned r = (unsigned)(rnd(&rs) % 10);
        if (atomic_load(&s_live) >= STRESS_TARGET || r < 3) {
            uint8_t *old = atomic_exchange(&slots[i], NULL);
            if (old)
                release_block(old, me);
        } else if (r == 3) {
            realloc_op(&rs, i, me);
        } else {
            uint8_t *p = alloc_block(&rs, me, 0);
            if (p)
                put(i, p, me);
        }
        atomic_fetch_add(&s_ops, 1);
    }
    atomic_fetch_add(&s_done, 1);
    return NULL;
}

static void test_stress(void)
{
    const size_t flex0 = dmem_flexible_free();
    pthread_t th[STRESS_THREADS];
    const double t0 = spike_now_ms();
    int created = 0;
    for (int i = 0; i < STRESS_THREADS; i++)
        created += pthread_create(&th[i], NULL, stress_thread, (void *)(uintptr_t)i) == 0;
    size_t flex_min = flex0;
    double ramp_ms = 0;
    while (atomic_load(&s_done) < created && spike_now_ms() - t0 < 180000) {
        /* Sample while the workers run. */
        const size_t f = dmem_flexible_free();
        if (f < flex_min)
            flex_min = f;
        if (!ramp_ms && atomic_load(&s_phase))
            ramp_ms = spike_now_ms() - t0;
        usleep(20 * 1000);
    }
    for (int i = 0; i < created; i++)
        pthread_join(th[i], NULL);
    const double run_ms = spike_now_ms() - t0;
    dheap_stats_t hpk;
    dheap_stats(&hpk);
    const long long peak = atomic_load(&s_peak), live_end = atomic_load(&s_live);
    size_t left = 0;
    for (int i = 0; i < NSLOTS; i++) {
        uint8_t *p = atomic_exchange(&slots[i], NULL);
        if (p) {
            release_block(p, 99);
            left++;
        }
    }
    dheap_trim();
    dheap_stats_t h;
    dheap_stats(&h);
    const size_t flex1 = dmem_flexible_free();
    const int ok = created == STRESS_THREADS && atomic_load(&s_bad) == 0 && atomic_load(&s_fail) == 0 &&
                   peak >= STRESS_TARGET && atomic_load(&s_live) == 0;
    SPIKE_RESULT("stress", ok ? "PASS" : "FAIL",
                 "%d threads, 16 B-4 MB: %llu ops in %.0f ms (ramp to 1.5 GB in %.0f ms): %llu allocs "
                 "(%llu posix_memalign, %llu calloc), %llu reallocs, %llu frees (%llu cross-thread); "
                 "peak live %.0f MB, heap mapped peak %.0f MB, %llu bad, %llu failed/not in heap, "
                 "%llu libc fallbacks; %zu blocks left at the end (%.0f MB live) freed by main; heap "
                 "mapped %.0f MB after free+trim (%u segments); flexible %.1f MB before, min %.1f MB "
                 "during, %.1f MB after",
                 created, (unsigned long long)atomic_load(&s_ops), run_ms, ramp_ms,
                 (unsigned long long)atomic_load(&s_allocs), (unsigned long long)atomic_load(&s_memalign),
                 (unsigned long long)atomic_load(&s_calloc), (unsigned long long)atomic_load(&s_reallocs),
                 (unsigned long long)atomic_load(&s_frees), (unsigned long long)atomic_load(&s_cross),
                 peak / MB, fmb(hpk.peak), (unsigned long long)atomic_load(&s_bad),
                 (unsigned long long)atomic_load(&s_fail), h.libc_fallbacks, left, live_end / MB,
                 fmb(h.mapped), h.segments, fmb(flex0), fmb(flex_min), fmb(flex1));
    log_heap("after stress");
}

/* -- 7. thread stacks in direct memory ------------------------------------------------ */
#define NTHREADS 64
#define STACK_SIZE ((size_t)2 << 20)
#define STACK_STRIDE (STACK_SIZE + DMEM_UNIT) /* an unmapped 64 KiB guard below each stack */

typedef struct {
    uint8_t *lo, *hi;
    uintptr_t local;
    int ok;
} stack_arg_t;

static atomic_int st_running, st_release;

static void *stack_thread(void *arg)
{
    stack_arg_t *a = arg;
    volatile uint8_t buf[1 << 20]; /* 1 MB of the stack */
    for (size_t i = 0; i < sizeof buf; i += 4096)
        buf[i] = (uint8_t)i;
    a->local = (uintptr_t)&buf[0];
    a->ok = a->lo ? (a->local >= (uintptr_t)a->lo && a->local < (uintptr_t)a->hi) : 1;
    atomic_fetch_add(&st_running, 1);
    while (!atomic_load(&st_release))
        usleep(1000);
    return NULL;
}

static int run_threads(int direct, size_t *flex_alive, size_t *stack_default, int *create_rc)
{
    static stack_arg_t args[NTHREADS];
    static commit_t stacks[NTHREADS];
    pthread_t th[NTHREADS];
    int created = 0;
    atomic_store(&st_running, 0);
    atomic_store(&st_release, 0);
    *create_rc = 0;
    for (int i = 0; i < NTHREADS; i++) {
        pthread_attr_t at;
        pthread_attr_init(&at);
        memset(&args[i], 0, sizeof args[i]);
        if (direct) {
            uint8_t *lo = g_range + STACK_OFF + (size_t)i * STACK_STRIDE + DMEM_UNIT;
            int rc = commit(&stacks[i], lo, STACK_SIZE, g_prot, NULL);
            if (rc != 0) {
                *create_rc = rc;
                pthread_attr_destroy(&at);
                break;
            }
            args[i].lo = lo;
            args[i].hi = lo + STACK_SIZE;
            pthread_attr_setstack(&at, lo, STACK_SIZE);
        } else {
            pthread_attr_setstacksize(&at, STACK_SIZE);
            if (i == 0)
                pthread_attr_getstacksize(&at, stack_default);
        }
        int rc = pthread_create(&th[i], &at, stack_thread, &args[i]);
        pthread_attr_destroy(&at);
        if (rc != 0) {
            *create_rc = rc;
            if (direct)
                decommit(&stacks[i], NULL, NULL);
            break;
        }
        created++;
    }
    for (int w = 0; w < 5000 && atomic_load(&st_running) < created; w++)
        usleep(1000);
    *flex_alive = dmem_flexible_free();
    atomic_store(&st_release, 1);
    int ok = 0;
    for (int i = 0; i < created; i++) {
        pthread_join(th[i], NULL);
        ok += args[i].ok;
        if (direct)
            decommit(&stacks[i], NULL, NULL);
    }
    return created == NTHREADS && ok == NTHREADS ? NTHREADS : -(created * 1000 + ok);
}

static void test_stacks(void)
{
    if (!g_range) {
        SPIKE_SKIP("stacks", "no range");
        return;
    }
    size_t f0 = dmem_flexible_free(), fa = 0, fb = 0, dflt = 0;
    int rc1 = 0, rc2 = 0;
    const int d = run_threads(1, &fa, &dflt, &rc1);
    const size_t f1 = dmem_flexible_free();
    const int p = run_threads(0, &fb, &dflt, &rc2);
    const size_t f2 = dmem_flexible_free();
    SPIKE_RESULT("stacks", d == NTHREADS ? "PASS" : "FAIL",
                 "64 threads on 2 MB direct-memory stacks (pthread_attr_setstack, 64 KiB guard), 1 MB "
                 "of each touched, all alive at once: %s (create rc 0x%x); flexible %.1f MB before, "
                 "%.1f MB with them alive, %.1f MB after. Same with libc stacks of 2 MB: %s (rc 0x%x), "
                 "flexible %.1f MB alive (%.1f MB used), %.1f MB after",
                 d == NTHREADS ? "OK, every local inside its own stack" : "FAILED", (unsigned)rc1,
                 fmb(f0), fmb(fa), fmb(f1), p == NTHREADS ? "OK" : "FAILED", (unsigned)rc2, fmb(fb),
                 fmb(f1) - fmb(fb), fmb(f2));
    if (d != NTHREADS || p != NTHREADS)
        spike_log("stacks: direct %d, libc %d (negative: -(created*1000+ok))", d, p);
}

/* -- 5. decoder next to a 1.5 GB heap ---------------------------------------------- */
static uint8_t *load_clip(const char *path, size_t *size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)st.st_size);
    size_t got = 0;
    while (buf && got < (size_t)st.st_size) {
        const ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    if (!buf || got != (size_t)st.st_size) {
        free(buf);
        return NULL;
    }
    *size = got;
    return buf;
}

static void vlog(const char *s)
{
    spike_log("%s", s);
}

static void test_decoder(int vdec_rc)
{
    if (vdec_rc != 0) {
        SPIKE_FAIL("decoder", "libSceVideodec2 not bound (0x%08x): %s", (unsigned)vdec_rc,
                   vdec2_load_report());
        return;
    }
    const size_t flex0 = dmem_flexible_free();
    /* 1.5 GB live in the heap: random sizes, every byte written. */
    enum { MAXB = 65536 };
    uint8_t **blocks = malloc(MAXB * sizeof *blocks);
    size_t live = 0;
    int nb = 0;
    uint64_t rs = 12345;
    while (blocks && live < (size_t)STRESS_TARGET && nb < MAXB) {
        size_t sz = rand_size(&rs, 4, 22);
        if (sz > (4u << 20))
            sz = 4u << 20;
        uint8_t *p = malloc(sz);
        if (!p)
            break;
        memset(p, nb & 0xff, sz);
        blocks[nb++] = p;
        live += sz;
    }
    int owned = 0;
    for (int i = 0; i < nb; i++)
        owned += dheap_owns(blocks[i]);
    const size_t flex_heap = dmem_flexible_free();
    log_heap("with the 1.5 GB ballast");

    size_t clip_size = 0;
    const double l0 = spike_now_ms();
    uint8_t *clip = load_clip(CLIP, &clip_size);
    static au_src_t src;
    if (!clip || au_open(&src, clip, clip_size, "mp4") != 0) {
        SPIKE_FAIL("decoder", "%s: cannot load (errno %d)", CLIP, errno);
        goto out;
    }
    spike_log("loaded %s: %.0f MB in %.0f ms (in the heap: %d)", CLIP, fmb(clip_size),
              spike_now_ms() - l0, dheap_owns(clip));
    const size_t flex_clip = dmem_flexible_free();
    vdec2_config_t cfg = {.codec = VDEC2_CODEC_HEVC, .profile = 1, .width = 7680, .height = 3840,
                          .pipeline_depth = 4};
    vdec2_info_t info;
    const char *stage;
    int rc;
    const double o0 = spike_now_ms();
    vdec2_t *v = vdec2_open(&cfg, &info, &stage, &rc);
    const double open_ms = spike_now_ms() - o0;
    if (!v) {
        SPIKE_FAIL("decoder", "bring-up failed at %s rc=0x%08x with %.0f MB live in the heap; "
                   "flexible %.1f MB", stage, (unsigned)rc, live / MB, fmb(dmem_flexible_free()));
        goto out;
    }
    const size_t flex_open = dmem_flexible_free();
    unsigned aus = 0, frames = 0, errors = 0, flagged = 0;
    unsigned first_rc = 0;
    uint64_t pts = 0;
    const double t0 = spike_now_ms();
    while (spike_now_ms() - t0 < 10000.0) {
        int key = 0, hidden = 0;
        uint8_t *slot = vdec2_input_slot(v);
        const long n = au_next(&src, slot, VDEC2_INPUT_SLOT_BYTES, &key, &hidden);
        if (n < 0)
            break;
        if (n == 0) {
            au_rewind(&src);
            continue;
        }
        vdec2_picture_t pic;
        const int drc = vdec2_decode(v, slot, (size_t)n, pts++, &pic);
        aus++;
        if (drc != 0) {
            if (!errors)
                first_rc = (unsigned)drc;
            if (++errors > 64 && frames == 0)
                break;
            continue;
        }
        if (pic.valid) {
            flagged += pic.error != 0;
            frames++;
        }
    }
    for (int i = 0; i < 32; i++) {
        vdec2_picture_t pic;
        if (vdec2_flush(v, &pic) != 0 || !pic.valid)
            break;
        frames++;
    }
    const double ms = spike_now_ms() - t0;
    const size_t flex_dec = dmem_flexible_free();
    vdec2_close(v);
    const size_t flex_closed = dmem_flexible_free();
    /* The heap is still intact. */
    size_t bad = 0;
    for (int i = 0; i < nb; i++)
        bad += blocks[i][0] != (uint8_t)(i & 0xff);
    SPIKE_RESULT("decoder", frames > 0 && errors == 0 && bad == 0 ? "PASS" : "FAIL",
                 "%.0f MB live in the direct heap (%d blocks, %d in the heap) + the %.0f MB clip: 8K "
                 "HEVC depth 4 up in %.0f ms, %u AUs -> %u pictures in %.0f ms = %.1f fps, %u errors "
                 "(first 0x%08x), %u flagged; decoder asked %.0f MB flexible; flexible %.1f MB at "
                 "start, %.1f MB with the heap, %.1f MB with the clip, %.1f MB decoder open, %.1f MB "
                 "after 10 s, %.1f MB after close; heap blocks intact: %zu bad",
                 live / MB, nb, owned, fmb(clip_size), open_ms, aus, frames, ms, frames * 1000.0 / ms,
                 errors, first_rc, flagged, (double)info.cpu_bytes / MB, fmb(flex0), fmb(flex_heap),
                 fmb(flex_clip), fmb(flex_open), fmb(flex_dec), fmb(flex_closed), bad);
out:
    free(clip);
    for (int i = 0; i < nb; i++)
        free(blocks[i]);
    free(blocks);
    dheap_trim();
    log_heap("after the decoder test");
}

/* -- 4c. throughput against libc ----------------------------------------------------- */
#define TP_SLOTS 1024

typedef struct {
    int libc;
    int ops;
    unsigned seed;
} tp_arg_t;

static atomic_int tp_go;

static void *tp_thread(void *arg)
{
    const tp_arg_t *a = arg;
    void *local[TP_SLOTS];
    memset(local, 0, sizeof local);
    uint64_t rs = 0x2545f4914f6cdd1dull * (a->seed + 1);
    while (!atomic_load(&tp_go))
        ;
    for (int op = 0; op < a->ops; op++) {
        const int i = (int)(rnd(&rs) % TP_SLOTS);
        if (local[i]) {
            if (a->libc)
                __real_free(local[i]);
            else
                free(local[i]);
        }
        const size_t sz = rand_size(&rs, 4, 16); /* 16 B .. 64 KB */
        uint8_t *p = a->libc ? __real_malloc(sz) : malloc(sz);
        if (p)
            p[0] = 1;
        local[i] = p;
    }
    for (int i = 0; i < TP_SLOTS; i++)
        if (local[i]) {
            if (a->libc)
                __real_free(local[i]);
            else
                free(local[i]);
        }
    return NULL;
}

static double tp_run(int libc, int threads, int ops)
{
    pthread_t th[8];
    tp_arg_t args[8];
    atomic_store(&tp_go, 0);
    for (int i = 0; i < threads; i++) {
        args[i] = (tp_arg_t){libc, ops, (unsigned)i};
        pthread_create(&th[i], NULL, tp_thread, &args[i]);
    }
    usleep(20 * 1000);
    const double t0 = spike_now_ms();
    atomic_store(&tp_go, 1);
    for (int i = 0; i < threads; i++)
        pthread_join(th[i], NULL);
    const double ms = spike_now_ms() - t0;
    return (double)threads * ops / ms / 1000.0; /* M ops/s */
}

static void test_throughput(void)
{
    const size_t f0 = dmem_flexible_free();
    const double h1 = tp_run(0, 1, 400000), l1 = tp_run(1, 1, 400000);
    const double h2 = tp_run(0, 2, 100000), l2 = tp_run(1, 2, 100000);
    const double h8 = tp_run(0, 8, 50000), l8 = tp_run(1, 8, 50000);
    const size_t f1 = dmem_flexible_free();
    SPIKE_PASS("throughput",
               "malloc+free pairs, 16 B-64 KB, 1024 live per thread: direct heap %.2f M/s vs libc "
               "%.2f M/s on 1 thread (400k); %.2f vs %.2f M/s on 2 threads (2 x 100k); %.2f vs %.2f "
               "M/s on 8 threads (8 x 50k); flexible %.1f -> %.1f MB (libc's heap grew by %.1f MB)",
               h1, l1, h2, l2, h8, l8, fmb(f0), fmb(f1), fmb(f0) - fmb(f1));
}

int main(void)
{
#ifndef PAD
    /* Before the unjail (PS5VR: the module won't load after it). */
    const int vrc = vdec2_load();
#endif
    spike_title_init("spike10", LOG_HOST, LOG_PORT);
#ifdef PAD
    spike_log("ps5webkit spike 10 (title, PADDED build): binary size vs flexible memory, pid %d",
              (int)getpid());
    test_start();
    test_pad();
#else
    spike_log("ps5webkit spike 10 (title): direct-memory heap, pid %d", (int)getpid());
    vdec2_set_logger(vlog);
    spike_log("vdec2: %s", vdec2_load_report());
    test_start();
    test_range();
    test_cxx_glib();
    test_stress();
    /* Before the stacks test: libc thread stacks take flexible memory and
     * keep it after the threads end (seen on the first run). */
    test_decoder(vrc);
    test_stacks();
    test_throughput();
    test_jit();
    log_heap("at the end");
    spike_log("memory at the end: flexible %.1f MB (%.1f MB at main)", fmb(dmem_flexible_free()),
              fmb(g_flex_start));
#endif
    spike_log("spike 10 done");
    return 0;
}
