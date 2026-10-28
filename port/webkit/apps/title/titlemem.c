/*
 * titlemem.c - keep the browser title's memory out of flexible memory.
 *
 * A registered title has ~440 MB of flexible memory (13.60), and the title
 * image (~112 MB) comes out of it 1:1 (Spike 10). Everything the UIProcess
 * allocates goes to direct memory instead, as Spike 10 recommends:
 *
 * - malloc and its family: Spike 10's dheap.c (dlmalloc over direct memory),
 *   compiled from spikes/10-title-dmem and linked with --wrap=malloc,...
 *   It replaces ps5-opengl's app_heap.c, whose 128 MB mspace was an
 *   anonymous mmap (flexible memory).
 * - anonymous private read-write mmaps (libpas's 128 MB compact heap
 *   reservation and pages, WTF's OSAllocator, Mesa): direct memory mapped
 *   where the kernel likes, zero-filled, in a table so munmap (whole or
 *   partial), madvise and MAP_FIXED re-zeroing (libpas's zero fill) work.
 *   Linked with --wrap=mmap,munmap,madvise. Mappings with an fd (the
 *   WebProcess's shared-memory frames), PROT_NONE reservations and
 *   MAP_FIXED mappings outside the table go to the real calls.
 * - thread stacks (--wrap=pthread_create,pthread_join,pthread_exit): libc's
 *   stacks are flexible memory that is never given back (Spike 10). Each
 *   thread without its own stack gets one from direct memory with an
 *   inaccessible 16 KiB guard below it; it is freed at pthread_join, or
 *   2 s after a detached thread's function returned.
 *
 * ps5wk_title_mem_log() reports flexible memory and what each part holds.
 */
#define SPIKE_NAME "ps5browser"
#include "common/spike.h"
#include "dmem.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define PAGE ((size_t)DMEM_PAGE)
#define UNIT ((size_t)DMEM_UNIT)

static size_t round_up(size_t v, size_t a)
{
    return (v + a - 1) & ~(a - 1);
}

size_t dmem_flexible_free(void)
{
    size_t s = 0;
    return sceKernelAvailableFlexibleMemorySize(&s) == 0 ? s : 0;
}

/* -- direct-memory mappings (anonymous mmap) --------------------------------- */

typedef struct {
    uintptr_t base;
    size_t len;
    int64_t phys;
} dmap_t;

#define MAX_DMAPS 16384
static dmap_t dmaps[MAX_DMAPS]; /* sorted by base */
static unsigned ndmaps;
static size_t dmap_bytes, dmap_peak;
static unsigned long long dmap_count, dmap_failures, dmap_zero_fills, dmap_passthrough;
static pthread_mutex_t dmap_lock = PTHREAD_MUTEX_INITIALIZER;

void* __real_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off);
int __real_munmap(void* addr, size_t len);
int __real_madvise(void* addr, size_t len, int advice);

/* Index of the first mapping that ends after a (lock held). */
static unsigned dmap_find(uintptr_t a)
{
    unsigned lo = 0, hi = ndmaps;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (dmaps[mid].base + dmaps[mid].len <= a)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* 1 if [a, a+len) lies inside one mapping (lock held). */
static int dmap_contains(uintptr_t a, size_t len)
{
    unsigned i = dmap_find(a);
    return i < ndmaps && dmaps[i].base <= a && a + len <= dmaps[i].base + dmaps[i].len;
}

static int dmap_overlaps(uintptr_t a, size_t len)
{
    unsigned i = dmap_find(a);
    return i < ndmaps && dmaps[i].base < a + len;
}

static void dmap_insert(dmap_t m)
{
    unsigned i = dmap_find(m.base);
    memmove(&dmaps[i + 1], &dmaps[i], (ndmaps - i) * sizeof dmaps[0]);
    dmaps[i] = m;
    ndmaps++;
}

/* A new direct-memory mapping of len bytes (a multiple of UNIT), CPU RW, at
 * an address aligned to align (a power of two >= UNIT). */
static void* dmap_new(size_t len, size_t align)
{
    int64_t phys = 0;
    void* addr = NULL;
    if (sceKernelAllocateMainDirectMemory(len, UNIT, DMEM_TYPE_WB, &phys) != 0)
        return NULL;
    if (sceKernelMapDirectMemory(&addr, len, DMEM_PROT_CPU_RW, 0, phys, align) != 0 || !addr
        || ((uintptr_t)addr & (align - 1))) {
        if (addr)
            sceKernelMunmap(addr, len);
        sceKernelReleaseDirectMemory(phys, len);
        if (align <= UNIT)
            return NULL;
        /* Retry with the physical range aligned as well. */
        addr = NULL;
        if (sceKernelAllocateMainDirectMemory(len, align, DMEM_TYPE_WB, &phys) != 0)
            return NULL;
        if (sceKernelMapDirectMemory(&addr, len, DMEM_PROT_CPU_RW, 0, phys, align) != 0 || !addr
            || ((uintptr_t)addr & (align - 1))) {
            if (addr)
                sceKernelMunmap(addr, len);
            sceKernelReleaseDirectMemory(phys, len);
            return NULL;
        }
    }
    pthread_mutex_lock(&dmap_lock);
    if (ndmaps == MAX_DMAPS) {
        pthread_mutex_unlock(&dmap_lock);
        sceKernelMunmap(addr, len);
        sceKernelReleaseDirectMemory(phys, len);
        return NULL;
    }
    dmap_insert((dmap_t) { (uintptr_t)addr, len, phys });
    dmap_bytes += len;
    if (dmap_bytes > dmap_peak)
        dmap_peak = dmap_bytes;
    dmap_count++;
    pthread_mutex_unlock(&dmap_lock);
    return addr;
}

/* Unmap and release [a, a+len) wherever it overlaps mappings (lock held).
 * a and len are PAGE multiples. */
static void dmap_release_range(uintptr_t a, size_t len)
{
    const uintptr_t end = a + len;
    unsigned i = dmap_find(a);
    while (i < ndmaps && dmaps[i].base < end) {
        dmap_t m = dmaps[i];
        uintptr_t lo = a > m.base ? a : m.base;
        uintptr_t hi = end < m.base + m.len ? end : m.base + m.len;
        sceKernelMunmap((void*)lo, hi - lo);
        sceKernelReleaseDirectMemory(m.phys + (int64_t)(lo - m.base), hi - lo);
        dmap_bytes -= hi - lo;
        dmap_t left = { m.base, lo - m.base, m.phys };
        dmap_t right = { hi, m.base + m.len - hi, m.phys + (int64_t)(hi - m.base) };
        memmove(&dmaps[i], &dmaps[i + 1], (ndmaps - i - 1) * sizeof dmaps[0]);
        ndmaps--;
        if (right.len && ndmaps < MAX_DMAPS) {
            dmap_insert(right);
        }
        if (left.len && ndmaps < MAX_DMAPS) {
            dmap_insert(left);
            i++;
        }
    }
}

void* __wrap_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off)
{
    const int anon = (flags & MAP_ANON) && fd == -1 && !(flags & MAP_SHARED);
    if (!anon || !len || !(prot & (PROT_READ | PROT_WRITE)) || (prot & PROT_EXEC)) {
        __atomic_fetch_add(&dmap_passthrough, 1, __ATOMIC_RELAXED);
        return __real_mmap(addr, len, prot, flags, fd, off);
    }
    if (flags & MAP_FIXED) {
        /* libpas zero-fills by mapping fresh pages over its own: inside one
         * of ours, clear them instead. */
        pthread_mutex_lock(&dmap_lock);
        int inside = dmap_contains((uintptr_t)addr, len);
        pthread_mutex_unlock(&dmap_lock);
        if (inside) {
            memset(addr, 0, len);
            __atomic_fetch_add(&dmap_zero_fills, 1, __ATOMIC_RELAXED);
            return addr;
        }
        __atomic_fetch_add(&dmap_passthrough, 1, __ATOMIC_RELAXED);
        return __real_mmap(addr, len, prot, flags, fd, off);
    }
    /* MAP_ALIGNED(n) (FreeBSD): JSC's structure heap wants 128 MB alignment. */
    const unsigned alignShift = ((unsigned)flags & MAP_ALIGNMENT_MASK) >> MAP_ALIGNMENT_SHIFT;
    size_t align = UNIT;
    if (alignShift == 1)
        align = (size_t)2 << 20; /* MAP_ALIGNED_SUPER */
    else if (alignShift > 16 && alignShift < 40)
        align = (size_t)1 << alignShift;
    const size_t size = round_up(len, UNIT);
    void* p = dmap_new(size, align);
    if (!p) {
        __atomic_fetch_add(&dmap_failures, 1, __ATOMIC_RELAXED);
        spike_log("[mem] anonymous mmap of %zu bytes (align 0x%zx): no direct memory, using flexible", len, align);
        return __real_mmap(addr, len, prot, flags, fd, off);
    }
    memset(p, 0, size); /* recommitted direct memory is not zero (Spike 10) */
    if (size >= ((size_t)8 << 20))
        spike_log("[mem] anonymous mmap of %.1f MB (align 0x%zx, prot %d) -> direct memory at %p", size / 1048576.0,
            align, prot, p);
    return p;
}

int __wrap_munmap(void* addr, size_t len)
{
    const uintptr_t a = (uintptr_t)addr & ~(PAGE - 1);
    const size_t l = round_up((uintptr_t)addr + len, PAGE) - a;
    pthread_mutex_lock(&dmap_lock);
    if (dmap_overlaps(a, l)) {
        dmap_release_range(a, l);
        pthread_mutex_unlock(&dmap_lock);
        return 0;
    }
    pthread_mutex_unlock(&dmap_lock);
    return __real_munmap(addr, len);
}

int __wrap_madvise(void* addr, size_t len, int advice)
{
    pthread_mutex_lock(&dmap_lock);
    int ours = dmap_overlaps((uintptr_t)addr, len);
    pthread_mutex_unlock(&dmap_lock);
    if (ours)
        return 0; /* direct memory stays committed */
    return __real_madvise(addr, len, advice);
}

/* -- thread stacks ------------------------------------------------------------ */

typedef struct stack_rec {
    struct stack_rec* next;
    void* (*fn)(void*);
    void* arg;
    void* mem; /* the dmap: guard + stack */
    size_t memLen;
    pthread_t thread;
    _Atomic double finishedMs; /* 0 while running */
    int joined;
} stack_rec;

static stack_rec* stacks;
static pthread_mutex_t stack_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned stacks_live, threads_created, threads_libc;
static size_t stack_bytes;
static size_t default_stack_size;
static pthread_key_t stack_key;
static pthread_once_t stack_once = PTHREAD_ONCE_INIT;

int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
int __real_pthread_join(pthread_t, void**);
void __real_pthread_exit(void*) __attribute__((noreturn));

static void stack_init(void)
{
    pthread_attr_t a;
    size_t s = 0;
    if (pthread_attr_init(&a) == 0) {
        pthread_attr_getstacksize(&a, &s);
        pthread_attr_destroy(&a);
    }
    default_stack_size = s >= (256u << 10) ? s : (2u << 20);
    pthread_key_create(&stack_key, NULL);
}

void __wrap_free(void* p);
void* __wrap_calloc(size_t n, size_t size);

static void stack_free(stack_rec* r)
{
    sceKernelMprotect(r->mem, PAGE, DMEM_PROT_CPU_RW); /* the guard back to RW before release */
    __wrap_munmap(r->mem, r->memLen);
    stack_bytes -= r->memLen;
    stacks_live--;
}

/* Free the stacks of threads that finished more than 2 s ago (lock held). */
static void stack_reap(void)
{
    const double now = spike_now_ms();
    for (stack_rec** p = &stacks; *p;) {
        stack_rec* r = *p;
        double f = atomic_load(&r->finishedMs);
        if (f > 0 && now - f > 2000) {
            *p = r->next;
            stack_free(r);
            __wrap_free(r);
        } else {
            p = &r->next;
        }
    }
}

static void stack_finished(void)
{
    stack_rec* r = pthread_getspecific(stack_key);
    if (r)
        atomic_store(&r->finishedMs, spike_now_ms());
}

static void* stack_trampoline(void* p)
{
    stack_rec* r = p;
    pthread_setspecific(stack_key, r);
    void* result = r->fn(r->arg);
    stack_finished();
    return result;
}

void __wrap_pthread_exit(void* value)
{
    stack_finished();
    __real_pthread_exit(value);
}

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*fn)(void*), void* arg)
{
    pthread_once(&stack_once, stack_init);
    void* own = NULL;
    size_t ownSize = 0;
    if (attr && pthread_attr_getstack(attr, &own, &ownSize) == 0 && own) {
        __atomic_fetch_add(&threads_libc, 1, __ATOMIC_RELAXED);
        return __real_pthread_create(thread, attr, fn, arg);
    }

    size_t size = default_stack_size;
    int detach = PTHREAD_CREATE_JOINABLE;
    if (attr) {
        size_t s = 0;
        if (pthread_attr_getstacksize(attr, &s) == 0 && s)
            size = s;
        pthread_attr_getdetachstate(attr, &detach);
    }
    size = round_up(size < (64u << 10) ? (64u << 10) : size, UNIT);
    const size_t memLen = size + UNIT; /* the lowest 16 KiB of the extra unit is the guard */

    stack_rec* r = __wrap_calloc(1, sizeof *r);
    void* mem = r ? dmap_new(memLen, UNIT) : NULL;
    pthread_attr_t a;
    if (!mem || pthread_attr_init(&a) != 0) {
        if (mem)
            __wrap_munmap(mem, memLen);
        __wrap_free(r);
        __atomic_fetch_add(&threads_libc, 1, __ATOMIC_RELAXED);
        return __real_pthread_create(thread, attr, fn, arg);
    }
    sceKernelMprotect(mem, PAGE, 0); /* guard: no access */
    pthread_attr_setdetachstate(&a, detach);
    pthread_attr_setstack(&a, (char*)mem + UNIT, size);
    if (attr) {
        int v;
        struct sched_param sp;
        if (pthread_attr_getinheritsched(attr, &v) == 0)
            pthread_attr_setinheritsched(&a, v);
        if (pthread_attr_getschedpolicy(attr, &v) == 0)
            pthread_attr_setschedpolicy(&a, v);
        if (pthread_attr_getschedparam(attr, &sp) == 0)
            pthread_attr_setschedparam(&a, &sp);
    }
    r->fn = fn;
    r->arg = arg;
    r->mem = mem;
    r->memLen = memLen;

    pthread_mutex_lock(&stack_lock);
    stack_reap();
    r->next = stacks;
    stacks = r;
    stacks_live++;
    threads_created++;
    stack_bytes += memLen;
    pthread_mutex_unlock(&stack_lock);

    int rc = __real_pthread_create(&r->thread, &a, stack_trampoline, r);
    pthread_attr_destroy(&a);
    if (rc != 0) {
        pthread_mutex_lock(&stack_lock);
        for (stack_rec** p = &stacks; *p; p = &(*p)->next) {
            if (*p == r) {
                *p = r->next;
                break;
            }
        }
        stack_free(r);
        threads_created--;
        pthread_mutex_unlock(&stack_lock);
        __wrap_free(r);
        return rc;
    }
    *thread = r->thread;
    return 0;
}

int __wrap_pthread_join(pthread_t thread, void** value)
{
    int rc = __real_pthread_join(thread, value);
    if (rc == 0) {
        pthread_mutex_lock(&stack_lock);
        for (stack_rec** p = &stacks; *p; p = &(*p)->next) {
            stack_rec* r = *p;
            if (r->thread == thread) {
                *p = r->next;
                stack_free(r);
                __wrap_free(r);
                break;
            }
        }
        pthread_mutex_unlock(&stack_lock);
    }
    return rc;
}

/* -- report ---------------------------------------------------------------------- */

size_t ps5wk_title_flexible_free(void)
{
    return dmem_flexible_free();
}

/* Direct memory this process holds, all of it (the heap and mmaps above,
 * ps5-opengl's buffers and textures, the video decoder's memory). */
typedef struct {
    long start, end;
    int memory_type;
} dmq_info_t;
int sceKernelDirectMemoryQuery(long offset, int flags, dmq_info_t* info, size_t info_size);

double ps5wk_title_direct_mb(void);
double ps5wk_title_direct_mb(void)
{
    long offset = 0;
    double total = 0;
    for (int i = 0; i < 65536; i++) {
        dmq_info_t info;
        memset(&info, 0, sizeof info);
        if (sceKernelDirectMemoryQuery(offset, 1 /* the next allocation at or after offset */, &info, sizeof info) != 0 || info.end <= offset)
            break;
        total += (double)(info.end - info.start);
        offset = info.end;
    }
    return total / 1048576.0;
}

void ps5wk_title_mem_log(const char* milestone)
{
    dheap_stats_t h;
    dheap_stats(&h);
    pthread_mutex_lock(&dmap_lock);
    size_t mapped = dmap_bytes, peak = dmap_peak;
    unsigned n = ndmaps;
    pthread_mutex_unlock(&dmap_lock);
    pthread_mutex_lock(&stack_lock);
    unsigned live = stacks_live, created = threads_created;
    size_t sb = stack_bytes;
    pthread_mutex_unlock(&stack_lock);
    spike_log("[mem] %s: flexible %.1f MB free; direct memory %.1f MB in use; direct heap %.1f MB mapped (peak %.1f, %llu libc fallbacks, init %s); "
              "anon mmaps %.1f MB in %u (peak %.1f MB, %llu made, %llu failed, %llu zero fills, %llu passed through); "
              "thread stacks %u live, %.1f MB (%u created, %u libc)",
        milestone, dmem_flexible_free() / 1048576.0, ps5wk_title_direct_mb(), h.mapped / 1048576.0, h.peak / 1048576.0, h.libc_fallbacks,
        h.init_step, (mapped - sb) / 1048576.0, n, peak / 1048576.0, dmap_count, dmap_failures, dmap_zero_fills,
        dmap_passthrough, live, sb / 1048576.0, created, threads_libc);
}
