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
 * - executable anonymous mappings, only when ps5wk_title_mem_exec is set
 *   (the WebProcess in the title, spikes/14-webprocess-in-title: JSC's JIT
 *   pool is one anonymous RWX mmap): a title can't run code from a mapping
 *   made executable at map time, but can once it is made so with
 *   sceKernelMprotect (Spike 13). Mapped read-write in direct memory as
 *   above, then sceKernelMprotect'ed to the protection asked for. Unset,
 *   they go to the real mmap as before.
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
#include <stdio.h>
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

int ps5wk_title_mem_exec;
int __wrap_munmap(void* addr, size_t len);

/* WebAssembly memories of the WebProcess in the title (JSC's
 * BufferMemoryHandle.cpp, spikes/14-webprocess-in-title item 5). JSC
 * reserves each memory's range with sceKernelReserveVirtualRange and commits
 * what it uses with mmap(MAP_FIXED), which in a title takes flexible memory
 * (about 220 MB free). Here: direct memory mapped at the address, zeroed,
 * and in the table so munmap, madvise and MAP_FIXED re-zeroing work as for
 * the other mappings. Only with ps5wk_title_mem_exec (the switch); 0 tells
 * JSC to mmap as before. Address and length in 64 KiB units (wasm pages). */
static unsigned long long commit_count, commit_failures;
static int commit_direct_at(void* address, size_t bytes, const char* what)
{
    const uintptr_t a = (uintptr_t)address;
    if (!ps5wk_title_mem_exec || !bytes)
        return 0;
    if ((a & (PAGE - 1)) || (bytes & (PAGE - 1))) {
        spike_log("[mem] %s commit of %zu bytes at %p: not in 16 KiB pages, mmap instead (flexible memory)", what, bytes, address);
        return 0;
    }
    /* 16 KiB pages work too (Spike 10: commits and partial releases in
     * 16 KiB multiples); 64 KiB where the range allows it. */
    const size_t align = ((a | bytes) & (UNIT - 1)) ? PAGE : UNIT;
    int64_t phys = 0;
    int rc = sceKernelAllocateMainDirectMemory(bytes, align, DMEM_TYPE_WB, &phys);
    if (rc) {
        __atomic_fetch_add(&commit_failures, 1, __ATOMIC_RELAXED);
        spike_log("[mem] %s commit of %.1f MB at %p: no direct memory (rc 0x%08x)", what, bytes / 1048576.0, address, (unsigned)rc);
        return -1;
    }
    void* mapped = address;
    rc = sceKernelMapDirectMemory(&mapped, bytes, DMEM_PROT_CPU_RW, DMEM_MAP_FIXED, phys, 0);
    if (rc || mapped != address) {
        if (!rc && mapped)
            sceKernelMunmap(mapped, bytes);
        sceKernelReleaseDirectMemory(phys, bytes);
        __atomic_fetch_add(&commit_failures, 1, __ATOMIC_RELAXED);
        spike_log("[mem] %s commit of %.1f MB at %p: sceKernelMapDirectMemory(MAP_FIXED) rc 0x%08x -> %p", what, bytes / 1048576.0,
            address, (unsigned)rc, mapped);
        return -1;
    }
    memset(address, 0, bytes); /* direct memory is not zero when recommitted (Spike 10) */
    pthread_mutex_lock(&dmap_lock);
    if (ndmaps == MAX_DMAPS) {
        pthread_mutex_unlock(&dmap_lock);
        sceKernelMunmap(address, bytes);
        sceKernelReleaseDirectMemory(phys, bytes);
        return -1;
    }
    dmap_insert((dmap_t) { a, bytes, phys });
    dmap_bytes += bytes;
    if (dmap_bytes > dmap_peak)
        dmap_peak = dmap_bytes;
    dmap_count++;
    pthread_mutex_unlock(&dmap_lock);
    unsigned long long n = __atomic_add_fetch(&commit_count, 1, __ATOMIC_RELAXED);
    if (n <= 8 || bytes >= ((size_t)32 << 20))
        spike_log("[mem] %s commit %llu: %.1f MB of direct memory at %p (in a reserved range)", what, n, bytes / 1048576.0, address);
    return 1;
}

int ps5wk_title_commit_direct(void* address, size_t bytes)
{
    return commit_direct_at(address, bytes, "wasm");
}

/* Before JSC unmaps a memory's reservation: its direct memory goes back. */
void ps5wk_title_release_direct(void* address, size_t bytes)
{
    const uintptr_t a = (uintptr_t)address & ~(PAGE - 1);
    const size_t l = round_up((uintptr_t)address + bytes, PAGE) - a;
    pthread_mutex_lock(&dmap_lock);
    if (dmap_overlaps(a, l))
        dmap_release_range(a, l);
    pthread_mutex_unlock(&dmap_lock);
}
static unsigned long exec_maps;
static unsigned long long fixed_direct, fixed_passthrough, fixed_passthrough_bytes;

void* __wrap_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off)
{
    const int anon = (flags & MAP_ANON) && fd == -1 && !(flags & MAP_SHARED);
    if (ps5wk_title_mem_exec && anon && len && (prot & PROT_EXEC) && !(flags & MAP_FIXED)) {
        void* p = __wrap_mmap(addr, len, (prot & ~PROT_EXEC) | PROT_READ | PROT_WRITE, flags, fd, off);
        if (p == MAP_FAILED)
            return p;
        const size_t size = round_up(len, PAGE);
        const int kprot = ((prot & PROT_READ) ? 1 : 0) | ((prot & PROT_WRITE) ? 2 : 0) | 4;
        int rc = sceKernelMprotect(p, size, kprot);
        if (rc) {
            spike_log("[mem] executable mmap of %.1f MB at %p: sceKernelMprotect(0x%x) rc 0x%08x", size / 1048576.0, p, kprot,
                (unsigned)rc);
            __wrap_munmap(p, len);
            errno = ENOMEM;
            return MAP_FAILED;
        }
        unsigned long n = __atomic_add_fetch(&exec_maps, 1, __ATOMIC_RELAXED);
        if (n <= 8 || size >= ((size_t)8 << 20))
            spike_log("[mem] executable mmap %lu of %.1f MB (prot %d) -> direct memory at %p, made 0x%x with sceKernelMprotect", n,
                size / 1048576.0, prot, p, kprot);
        return p;
    }
    if (!anon || !len || !(prot & (PROT_READ | PROT_WRITE)) || (prot & PROT_EXEC)) {
        /* A fixed mapping over ours (a PROT_NONE guard or decommit): that
         * part leaves the table first, so it is released and never cleared
         * by a later MAP_FIXED as if it were still ours. */
        if ((flags & MAP_FIXED) && len) {
            const uintptr_t a = (uintptr_t)addr & ~(PAGE - 1);
            const size_t l = round_up((uintptr_t)addr + len, PAGE) - a;
            pthread_mutex_lock(&dmap_lock);
            if (dmap_overlaps(a, l))
                dmap_release_range(a, l);
            pthread_mutex_unlock(&dmap_lock);
        }
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
        /* Outside the table (a range reserved some other way, or mapped by
         * the real mmap before): with pages in the title, a fresh mapping
         * there took flexible memory every time libpas zero-filled or
         * decommitted its pages, until none was left and libpas's
         * PAS_ASSERT ended the browser (2026-11-16, pas_page_malloc_zero_fill;
         * 0 zero fills inside the table, ~200 passed through in a few
         * minutes of video). Whole 16 KiB pages up to 64 MB get direct
         * memory at the address instead, like WebAssembly's commits (the
         * ranges seen were 16 KiB aligned: 1 MB and 64 MB less 16 KiB). */
        const uintptr_t a = (uintptr_t)addr;
        if (ps5wk_title_mem_exec && !(a & (PAGE - 1)) && !(len & (PAGE - 1)) && len <= ((size_t)64 << 20)) {
            pthread_mutex_lock(&dmap_lock);
            if (dmap_overlaps(a, len))
                dmap_release_range(a, len);
            pthread_mutex_unlock(&dmap_lock);
            if (commit_direct_at(addr, len, "fixed mmap") > 0) {
                __atomic_fetch_add(&fixed_direct, 1, __ATOMIC_RELAXED);
                return addr;
            }
        }
        unsigned long long n = __atomic_add_fetch(&fixed_passthrough, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&fixed_passthrough_bytes, len, __ATOMIC_RELAXED);
        if (n <= 24 || !(n & (n - 1))) {
            pthread_mutex_lock(&dmap_lock);
            int overlaps = dmap_overlaps(a, len);
            pthread_mutex_unlock(&dmap_lock);
            spike_log("[mem] fixed mmap %llu to flexible memory: %zu bytes at %p (prot %d, flags 0x%x)%s%s; %.1f MB so far",
                n, len, addr, prot, flags, (a | len) & (PAGE - 1) ? ", not in 16 KiB pages" : "", overlaps ? ", overlapping ours" : "",
                fixed_passthrough_bytes / 1048576.0);
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

/* The sampling profiler's thread table (apps/common/profiler.c, in the
 * title for /data/ps5webkit/title-profile; weak: absent in payload builds). */
void ps5wk_profiler_thread(int starting) __attribute__((weak));

static void* stack_trampoline(void* p)
{
    stack_rec* r = p;
    pthread_setspecific(stack_key, r);
    if (ps5wk_profiler_thread)
        ps5wk_profiler_thread(1);
    void* result = r->fn(r->arg);
    if (ps5wk_profiler_thread)
        ps5wk_profiler_thread(0);
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
    /* With a WebProcess in the title, as ps5wk-child's crash guard does for
     * payloads (MIN_THREAD_STACK): WebKit asks for no size for JavaScript
     * threads (workers) on PS5, and the 64 KiB default is less than JSC's
     * reserved zone, so every worker's first script was "Maximum call stack
     * size exceeded" (2026-11-15). */
    if (ps5wk_title_mem_exec && size < (2u << 20))
        size = 2u << 20;
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

/* WebKit's WebGL contexts in the title (PS5WebGLLayers.cpp; weak: absent
 * in builds without WebGL). */
void ps5wk_webgl_mem_stats(unsigned* contexts, unsigned* made, double* drawingBufferMB) __attribute__((weak));

/* WebKit's WebGPU buffers and textures in the title, against their budget
 * (Source/WebGPU/WebGPUDawn/MemoryBudget.cpp; weak: absent without WebGPU). */
void ps5wk_webgpu_mem_stats(double* usedMB, double* peakMB, double* budgetMB, unsigned* refused) __attribute__((weak));

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
    char webgl[96] = "";
    if (ps5wk_webgl_mem_stats) {
        unsigned contexts = 0, made = 0;
        double mb = 0;
        ps5wk_webgl_mem_stats(&contexts, &made, &mb);
        if (made)
            snprintf(webgl, sizeof webgl, "; WebGL %u contexts (%u made), drawing buffers ~%.1f MB", contexts, made, mb);
    }
    char webgpu[128] = "";
    if (ps5wk_webgpu_mem_stats) {
        double used = 0, peak = 0, budget = 0;
        unsigned refused = 0;
        ps5wk_webgpu_mem_stats(&used, &peak, &budget, &refused);
        if (budget > 0)
            snprintf(webgpu, sizeof webgpu, "; WebGPU buffers and textures %.1f MB (peak %.1f, budget %.0f, %u refused)", used, peak, budget, refused);
    }
    spike_log("[mem] %s: flexible %.1f MB free; direct memory %.1f MB in use; direct heap %.1f MB mapped (peak %.1f, %llu libc fallbacks, init %s); "
              "anon mmaps %.1f MB in %u (peak %.1f MB, %llu made, %llu failed, %llu zero fills, %llu passed through; fixed mmaps outside them: %llu to direct memory, %llu to flexible, %.1f MB); "
              "thread stacks %u live, %.1f MB (%u created, %u libc)%s%s",
        milestone, dmem_flexible_free() / 1048576.0, ps5wk_title_direct_mb(), h.mapped / 1048576.0, h.peak / 1048576.0, h.libc_fallbacks,
        h.init_step, (mapped - sb) / 1048576.0, n, peak / 1048576.0, dmap_count, dmap_failures, dmap_zero_fills,
        dmap_passthrough, fixed_direct, fixed_passthrough, fixed_passthrough_bytes / 1048576.0, live, sb / 1048576.0, created, threads_libc,
        webgl, webgpu);
}
