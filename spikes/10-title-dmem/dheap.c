/* dheap.c - malloc and its family in direct memory, for a PS5 title.
 *
 * A registered title has ~420 MB of flexible memory (what anonymous mmap and
 * libc's own heap draw from) and gigabytes of direct memory. This file moves
 * the title's own heap into direct memory, so flexible memory is left to the
 * system libraries that insist on it (the video decoder's CPU part).
 *
 * Page source: one 16 GiB virtual range reserved at the first allocation
 * (sceKernelReserveVirtualRange, FIXED | NO_OVERWRITE at DHEAP_HINT). A
 * segment is direct memory allocated for it (sceKernelAllocateMainDirectMemory,
 * type 12) and mapped CPU read-write at a fixed address in the range, first
 * fit, with a 64 KiB hole after every segment so dlmalloc never sees two
 * segments as contiguous and every unmap it asks for is exactly one segment.
 * Unmapping gives the direct memory back and reserves the hole again.
 *
 * Allocator: dlmalloc 2.8.6 (dlmalloc/malloc.c, MIT-0, unmodified) as up to
 * eight locked mspaces ("arenas"); a thread takes one round robin at its
 * first allocation. FOOTERS lets free/realloc from any thread find the
 * block's own arena. A pointer is the heap's exactly when it lies in the
 * range; anything else (libc's strdup, a block allocated before the heap or
 * after it refused) goes back to libc.
 *
 * Link with --wrap for malloc, calloc, realloc, free, posix_memalign,
 * aligned_alloc, memalign and malloc_usable_size. --wrap rewrites every
 * reference in the title's own objects and static archives (libc++'s
 * operator new, glib), not those inside system modules (libc.prx's strdup).
 */
#include "dmem.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

/* -- page source ------------------------------------------------------------ */

#define SEG_HOLE DMEM_UNIT
#define MAX_SEGS 32768

typedef struct {
    uintptr_t base;
    size_t len;
    int64_t phys;
} seg_t;

static seg_t segs[MAX_SEGS]; /* sorted by base */
static unsigned nsegs;
static pthread_mutex_t seg_lock = PTHREAD_MUTEX_INITIALIZER;
static dheap_stats_t S = {.init_step = "not started", .prot = DMEM_PROT_CPU_RW, .map0_rc = 1};
static atomic_int range_state; /* 0 none, 1 in progress, 2 ready, 3 refused */

static size_t round_up(size_t v, size_t a)
{
    return (v + a - 1) & ~(a - 1);
}

static void reserve_range(void)
{
    void *addr = (void *)DHEAP_HINT;
    int rc = sceKernelReserveVirtualRange(&addr, DHEAP_RANGE, DMEM_MAP_FIXED | DMEM_MAP_NO_OVERWRITE,
                                          DMEM_PAGE);
    if (rc != 0) {
        /* Anywhere the kernel likes. */
        addr = NULL;
        rc = sceKernelReserveVirtualRange(&addr, DHEAP_RANGE, 0, DMEM_UNIT);
    }
    if (rc != 0 || !addr) {
        S.init_rc = rc ? rc : -1;
        S.init_step = "sceKernelReserveVirtualRange";
        atomic_store(&range_state, 3);
        return;
    }
    S.base = (uintptr_t)addr;
    S.range = DHEAP_RANGE;
    S.init_step = "ready";
    atomic_store(&range_state, 2);
}

static int range_ready(void)
{
    int st = atomic_load(&range_state);
    if (st == 2)
        return 1;
    if (st == 0) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&range_state, &expected, 1))
            reserve_range();
    }
    while ((st = atomic_load(&range_state)) == 1)
        ;
    return st == 2;
}

static int alloc_direct(size_t len, int64_t *phys)
{
    if (S.alloc_main >= 0) {
        if (sceKernelAllocateMainDirectMemory(len, DMEM_UNIT, DMEM_TYPE_WB, phys) == 0) {
            S.alloc_main = 1;
            return 0;
        }
        if (S.alloc_main == 0)
            S.alloc_main = -1; /* refused before it ever worked: use the ranged call */
    }
    return sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(), len, DMEM_UNIT,
                                         DMEM_TYPE_WB, phys);
}

/* dlmalloc's MMAP: a fresh segment of len bytes, or MFAIL. */
static void *seg_map(size_t len)
{
    if (!range_ready())
        return (void *)~(uintptr_t)0;
    len = round_up(len, DMEM_UNIT);
    pthread_mutex_lock(&seg_lock);
    uintptr_t at = S.base;
    unsigned slot = 0;
    for (; slot < nsegs; slot++) {
        if (segs[slot].base >= at + len + SEG_HOLE)
            break;
        at = segs[slot].base + segs[slot].len + SEG_HOLE;
    }
    void *result = (void *)~(uintptr_t)0;
    int64_t phys = -1;
    if (nsegs < MAX_SEGS && at + len <= S.base + S.range && alloc_direct(len, &phys) == 0) {
        void *addr = (void *)at;
        int rc = sceKernelMapDirectMemory(&addr, len, S.prot, DMEM_MAP_FIXED, phys, DMEM_UNIT);
        if (S.map0_rc == 1) { /* first commit: record 0x03's answer, fall back to 0xf2 */
            S.map0_rc = rc;
            if (rc != 0) {
                S.prot = DMEM_PROT_CPU_GPU_RW;
                addr = (void *)at;
                rc = sceKernelMapDirectMemory(&addr, len, S.prot, DMEM_MAP_FIXED, phys, DMEM_UNIT);
            }
        }
        if (rc == 0 && (uintptr_t)addr == at) {
            memmove(&segs[slot + 1], &segs[slot], (nsegs - slot) * sizeof segs[0]);
            segs[slot] = (seg_t){at, len, phys};
            nsegs++;
            S.mapped += len;
            if (S.mapped > S.peak)
                S.peak = S.mapped;
            S.maps++;
            result = addr;
        } else {
            if (rc == 0)
                sceKernelMunmap(addr, len);
            sceKernelReleaseDirectMemory(phys, len);
            S.map_failures++;
        }
    } else {
        S.map_failures++;
    }
    pthread_mutex_unlock(&seg_lock);
    return result;
}

/* dlmalloc's MUNMAP: only whole segments (dlmalloc treats -1 as "kept"). */
static int seg_unmap(void *p, size_t len)
{
    const uintptr_t a = (uintptr_t)p;
    pthread_mutex_lock(&seg_lock);
    unsigned lo = 0, hi = nsegs;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (segs[mid].base < a)
            lo = mid + 1;
        else
            hi = mid;
    }
    int rc = -1;
    if (lo < nsegs && segs[lo].base == a && round_up(len, DMEM_UNIT) == segs[lo].len) {
        const seg_t s = segs[lo];
        sceKernelMunmap(p, s.len);
        /* Keep the hole ours: munmap may have handed the VA back. */
        void *again = p;
        sceKernelReserveVirtualRange(&again, s.len, DMEM_MAP_FIXED | DMEM_MAP_NO_OVERWRITE, DMEM_PAGE);
        sceKernelReleaseDirectMemory(s.phys, s.len);
        memmove(&segs[lo], &segs[lo + 1], (nsegs - lo - 1) * sizeof segs[0]);
        nsegs--;
        S.mapped -= s.len;
        S.unmaps++;
        rc = 0;
    } else {
        S.refused_unmaps++;
    }
    pthread_mutex_unlock(&seg_lock);
    return rc;
}

/* -- dlmalloc ---------------------------------------------------------------- */

#define ONLY_MSPACES 1
#define MSPACES 1
#define USE_LOCKS 1
#define USE_SPIN_LOCKS 0 /* pthread mutexes: a spinning waiter can starve a preempted owner */
#define FOOTERS 1
#define HAVE_MORECORE 0
#define HAVE_MMAP 1
#define HAVE_MREMAP 0
#define MMAP(s) seg_map(s)
#define DIRECT_MMAP(s) seg_map(s)
#define MUNMAP(a, s) seg_unmap((a), (s))
#define MMAP_CLEARS 0 /* not relying on fresh direct memory being zero */
#define malloc_getpagesize ((size_t)DMEM_UNIT)
#define DEFAULT_GRANULARITY ((size_t)2 << 20)
#define DEFAULT_MMAP_THRESHOLD ((size_t)1 << 20)
#define DEFAULT_TRIM_THRESHOLD ((size_t)-1) /* tails can't be unmapped; whole segments still are */
#define NO_MALLINFO 1
#define NO_MALLOC_STATS 1
#define LACKS_SYS_MMAN_H 1
#define MALLOC_FAILURE_ACTION errno = ENOMEM;
#define DLMALLOC_EXPORT static __attribute__((unused))
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wall"
#pragma clang diagnostic ignored "-Wextra"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wnull-pointer-arithmetic"
#pragma clang diagnostic ignored "-Wexpansion-to-defined"
#include "dlmalloc/malloc.c"
#pragma clang diagnostic pop

/* -- arenas ------------------------------------------------------------------- */

static mspace arenas[DHEAP_ARENAS];
static atomic_int arena_state[DHEAP_ARENAS]; /* 0 none, 1 making, 2 ready, 3 refused */
static atomic_uint arena_next;
/* The thread's arena index + 1, in a pthread key: the SDK's __thread is
 * emulated TLS, whose __emutls_get_address calls malloc (it recursed into
 * here and overflowed the stack on the first console run). */
static pthread_key_t arena_key;
static atomic_int key_state; /* 0 none, 1 making, 2 ready */

static mspace arena(void)
{
    int i = 0;
    if (atomic_load_explicit(&key_state, memory_order_acquire) == 2) {
        const intptr_t v = (intptr_t)pthread_getspecific(arena_key);
        if (v) {
            i = (int)v - 1;
        } else {
            i = (int)(atomic_fetch_add(&arena_next, 1) % DHEAP_ARENAS);
            pthread_setspecific(arena_key, (void *)(intptr_t)(i + 1));
        }
    } else {
        int expected = 0;
        if (atomic_compare_exchange_strong(&key_state, &expected, 1))
            atomic_store_explicit(&key_state, pthread_key_create(&arena_key, NULL) == 0 ? 2 : 3,
                                  memory_order_release);
        /* arena 0 until the key exists (start-up, one thread) */
    }
    int st = atomic_load_explicit(&arena_state[i], memory_order_acquire);
    if (st == 2)
        return arenas[i];
    if (st == 0) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&arena_state[i], &expected, 1)) {
            arenas[i] = range_ready() ? create_mspace(0, 1) : NULL;
            atomic_store_explicit(&arena_state[i], arenas[i] ? 2 : 3, memory_order_release);
        }
    }
    while ((st = atomic_load_explicit(&arena_state[i], memory_order_acquire)) == 1)
        ;
    return st == 2 ? arenas[i] : NULL;
}

int dheap_owns(const void *p)
{
    const uintptr_t a = (uintptr_t)p;
    return atomic_load_explicit(&range_state, memory_order_acquire) == 2 && a >= S.base &&
           a - S.base < S.range;
}

void dheap_stats(dheap_stats_t *st)
{
    pthread_mutex_lock(&seg_lock);
    *st = S;
    st->segments = nsegs;
    st->arenas = 0;
    for (int i = 0; i < DHEAP_ARENAS; i++)
        st->arenas += atomic_load(&arena_state[i]) == 2;
    pthread_mutex_unlock(&seg_lock);
}

void dheap_trim(void)
{
    for (int i = 0; i < DHEAP_ARENAS; i++)
        if (atomic_load(&arena_state[i]) == 2)
            mspace_trim(arenas[i], 0);
}

/* -- the wrapped family ------------------------------------------------------- */

void *__real_calloc(size_t n, size_t size);
void *__real_realloc(void *p, size_t size);
int __real_posix_memalign(void **out, size_t align, size_t size);
void *__real_aligned_alloc(size_t align, size_t size);
void *__real_memalign(size_t align, size_t size);
size_t __real_malloc_usable_size(const void *p);

static void *fallback(void *p)
{
    if (p)
        __atomic_fetch_add(&S.libc_fallbacks, 1, __ATOMIC_RELAXED);
    return p;
}

void *__wrap_malloc(size_t size)
{
    mspace m = arena();
    void *p = m ? mspace_malloc(m, size) : NULL;
    return p ? p : fallback(__real_malloc(size));
}

void *__wrap_calloc(size_t n, size_t size)
{
    mspace m = arena();
    void *p = m ? mspace_calloc(m, n, size) : NULL;
    return p ? p : fallback(__real_calloc(n, size));
}

void __wrap_free(void *p)
{
    if (!p)
        return;
    if (dheap_owns(p))
        mspace_free(arenas[0], p); /* FOOTERS: the block's own arena */
    else
        __real_free(p);
}

void *__wrap_realloc(void *p, size_t size)
{
    if (!p)
        return __wrap_malloc(size);
    if (!dheap_owns(p))
        return __real_realloc(p, size);
    if (size == 0) {
        __wrap_free(p);
        return NULL;
    }
    void *q = mspace_realloc(arenas[0], p, size); /* FOOTERS: the block's own arena */
    if (q)
        return q;
    /* Heap exhausted: move the block to libc. */
    q = fallback(__real_malloc(size));
    if (q) {
        size_t old = mspace_usable_size(p);
        memcpy(q, p, old < size ? old : size);
        __wrap_free(p);
    }
    return q;
}

static void *aligned(size_t align, size_t size)
{
    mspace m = arena();
    return m ? mspace_memalign(m, align, size) : NULL;
}

int __wrap_posix_memalign(void **out, size_t align, size_t size)
{
    if (align < sizeof(void *) || (align & (align - 1)))
        return EINVAL;
    void *p = aligned(align, size);
    if (p) {
        *out = p;
        return 0;
    }
    const int rc = __real_posix_memalign(out, align, size);
    if (rc == 0)
        fallback(*out);
    return rc;
}

void *__wrap_aligned_alloc(size_t align, size_t size)
{
    void *p = aligned(align, size);
    return p ? p : fallback(__real_aligned_alloc(align, size));
}

void *__wrap_memalign(size_t align, size_t size)
{
    void *p = aligned(align, size);
    return p ? p : fallback(__real_memalign(align, size));
}

size_t __wrap_malloc_usable_size(const void *p)
{
    if (!p)
        return 0;
    return dheap_owns(p) ? mspace_usable_size(p) : __real_malloc_usable_size(p);
}
