/*
 * ps5malloc.c - malloc and its family on WebKit's libpas for ps5wk-child
 * (linked with --wrap=malloc,...; apps/CMakeLists.txt).
 *
 * Why: the console's libc malloc serialises threads very badly (a
 * malloc/free pair 0.3 us on one thread, 48-162 us each with 2-6 threads,
 * test/lockbench). Patch 0032 moved Skia and C++ new/delete to libpas; this
 * moves the C code too: glib (g_malloc), GStreamer, libsoup, FreeType,
 * HarfBuzz, ICU, fontconfig, libxml2, SQLite, OpenSSL... libpas has
 * per-thread caches, and malloc(n) here is exactly WTF::fastMalloc(n)'s heap
 * (bmalloc_try_allocate: the common primitive heap), so a block from new and
 * one from malloc can even be freed by the other.
 *
 * Ownership: the console's libc still allocates some memory our code frees
 * (getcwd(NULL) and realpath(NULL) are imported from it; strdup and vasprintf
 * are wrapped below) and
 * blocks it allocated before this decided. A pointer is libpas's exactly when
 * it lies in an anonymous mapping this executable made with mmap: libpas gets
 * all its memory from mmap (pas_page_malloc), and the console's malloc never
 * comes through our --wrap=mmap (it is another module, bound to libkernel).
 * ps5wk-child's __wrap_mmap/__wrap_munmap call ps5malloc_note_map/unmap,
 * which keep a bitmap of 16 KiB pages (two levels: 2 GiB chunks of 16 KiB
 * bitmaps, made on first use). free/realloc/malloc_usable_size of anything
 * else go to the console's libc. The other direction (the console's libc
 * freeing or reallocating a block we made) needs a libc function that takes
 * ownership of a caller's block: none of the imported ones does (checked
 * against the import list: no getline/getdelim/putenv/open_memstream).
 *
 * Emulated TLS: the SDK compiles __thread as emulated TLS, and compiler-rt's
 * __emutls_get_address (libc.a, weak) mallocs a thread's TLS block on first
 * use. libpas keeps its thread cache in a __thread variable, so with malloc on
 * libpas that first use would recurse forever. This file defines
 * __emutls_get_address (same ABI and logic as compiler-rt's emutls.c) with
 * the console's malloc for those blocks.
 *
 * Off switch: if PS5MALLOC_OFF_FILE exists at the first malloc (before main),
 * malloc and its family go to the console's libc (free still looks at the
 * bitmap, so blocks from new still go back to libpas).
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef PS5MALLOC_OFF_FILE
#define PS5MALLOC_OFF_FILE "/data/ps5webkit/no-pasmalloc"
#endif

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* libpas, through what WebKit's build exports (bmalloc_try_allocate and
 * friends are compiled out: PAS_BMALLOC_HIDDEN). WTF::tryFastAlignedMalloc
 * is fastMalloc's inline path with a null return (alignment 16 is libpas's
 * minimum, so it takes the same fast path), WTF::fastFree is operator
 * delete's, bmalloc::api::tryReallocOutOfLine is fastRealloc's without the
 * crash. The C++ names are bound with asm labels; their PRESERVE_MOST
 * convention only saves more registers than a C caller expects. */
void* wtf_try_aligned_malloc(size_t alignment, size_t size) __asm__("_ZN3WTF20tryFastAlignedMallocEmm");
void wtf_free(void*) __asm__("_ZN3WTF8fastFreeEPv");
void* bmalloc_try_realloc(void* old, size_t size) __asm__("_ZN7bmalloc3api19tryReallocOutOfLineEPvm");
size_t bmalloc_get_allocation_size(void*);

static inline void* bmalloc_try_allocate(size_t size) { return wtf_try_aligned_malloc(16, size); }
static inline void* bmalloc_try_allocate_zeroed(size_t size)
{
    void* p = wtf_try_aligned_malloc(16, size);
    if (p)
        memset(p, 0, size);
    return p;
}
static inline void* bmalloc_try_allocate_with_alignment(size_t size, size_t alignment) { return wtf_try_aligned_malloc(alignment, size); }
static inline void bmalloc_deallocate(void* p) { wtf_free(p); }

void* __real_malloc(size_t);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void __real_free(void*);
int __real_posix_memalign(void**, size_t, size_t);
void* __real_aligned_alloc(size_t, size_t);
size_t __real_malloc_usable_size(const void*);
void* __real_mmap(void*, size_t, int, int, int, off_t);
int __real_munmap(void*, size_t);

/* -- which pointers are ours ------------------------------------------------- */

#define PAGE_SHIFT 14 /* 16 KiB, the console's page size */
#define CHUNK_SHIFT 31
#define ADDRESS_BITS 47
#define CHUNKS (1u << (ADDRESS_BITS - CHUNK_SHIFT))
#define CHUNK_WORDS ((1u << (CHUNK_SHIFT - PAGE_SHIFT)) / 64)
static uint64_t* chunks[CHUNKS];
static uint64_t bitmapFailures;

static uint64_t* chunk_for(uintptr_t page, int create)
{
    uintptr_t index = page >> (CHUNK_SHIFT - PAGE_SHIFT);
    if (index >= CHUNKS)
        return NULL;
    uint64_t* chunk = __atomic_load_n(&chunks[index], __ATOMIC_ACQUIRE);
    if (chunk || !create)
        return chunk;
    void* fresh = __real_mmap(NULL, CHUNK_WORDS * 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (fresh == MAP_FAILED) {
        __atomic_fetch_add(&bitmapFailures, 1, __ATOMIC_RELAXED);
        return NULL;
    }
    uint64_t* expected = NULL;
    if (__atomic_compare_exchange_n(&chunks[index], &expected, (uint64_t*)fresh, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return (uint64_t*)fresh;
    __real_munmap(fresh, CHUNK_WORDS * 8); /* lost the race */
    return expected;
}

static void mark(uintptr_t address, size_t length, int set)
{
    if (!length)
        return;
    uintptr_t page = address >> PAGE_SHIFT;
    uintptr_t end = (address + length + ((1u << PAGE_SHIFT) - 1)) >> PAGE_SHIFT;
    while (page < end) {
        uint64_t* chunk = chunk_for(page, set);
        uintptr_t chunkEnd = (page | ((CHUNK_WORDS * 64) - 1)) + 1;
        uintptr_t stop = end < chunkEnd ? end : chunkEnd;
        if (chunk) {
            for (uintptr_t p = page; p < stop;) {
                unsigned bit = (unsigned)(p & 63);
                unsigned n = (unsigned)(stop - p < 64 - bit ? stop - p : 64 - bit);
                uint64_t mask = (n == 64 ? ~(uint64_t)0 : (((uint64_t)1 << n) - 1)) << bit;
                uint64_t* w = &chunk[(p & ((CHUNK_WORDS * 64) - 1)) >> 6];
                if (set)
                    __atomic_fetch_or(w, mask, __ATOMIC_RELEASE);
                else
                    __atomic_fetch_and(w, ~mask, __ATOMIC_RELEASE);
                p += n;
            }
        }
        page = stop;
    }
}

/* Called by ps5wk-child's mmap/munmap wrappers after a successful call. */
void ps5malloc_note_map(void* address, size_t length, int flags)
{
    if (flags & MAP_ANON)
        mark((uintptr_t)address, length, 1);
}

void ps5malloc_note_unmap(void* address, size_t length)
{
    mark((uintptr_t)address, length, 0);
}

static inline int ours(const void* p)
{
    uintptr_t page = (uintptr_t)p >> PAGE_SHIFT;
    uintptr_t index = page >> (CHUNK_SHIFT - PAGE_SHIFT);
    if (UNLIKELY(index >= CHUNKS))
        return 0;
    uint64_t* chunk = __atomic_load_n(&chunks[index], __ATOMIC_ACQUIRE);
    if (UNLIKELY(!chunk))
        return 0;
    uintptr_t bit = page & ((CHUNK_WORDS * 64) - 1);
    return (int)(__atomic_load_n(&chunk[bit >> 6], __ATOMIC_ACQUIRE) >> (bit & 63) & 1);
}

int ps5malloc_owns(const void* p) { return p && ours(p); }

/* -- mode and statistics ---------------------------------------------------- */

static int mode; /* 0 undecided, 1 libpas, 2 the console's libc */
static uint64_t foreignFrees, foreignReallocs, fallbacks;

static __attribute__((noinline)) int decide(void)
{
    int saved = errno;
    int m = access(PS5MALLOC_OFF_FILE, F_OK) == 0 ? 2 : 1;
    errno = saved;
    int expected = 0;
    __atomic_compare_exchange_n(&mode, &expected, m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return __atomic_load_n(&mode, __ATOMIC_ACQUIRE) == 1;
}

static inline int pas(void)
{
    int m = __atomic_load_n(&mode, __ATOMIC_RELAXED);
    if (LIKELY(m == 1))
        return 1;
    if (m == 2)
        return 0;
    return decide();
}

typedef struct {
    int pas;
    uint64_t foreign_frees, foreign_reallocs, fallbacks, bitmap_failures;
} ps5malloc_stats_t;

void ps5malloc_stats(ps5malloc_stats_t* out)
{
    out->pas = pas();
    out->foreign_frees = __atomic_load_n(&foreignFrees, __ATOMIC_RELAXED);
    out->foreign_reallocs = __atomic_load_n(&foreignReallocs, __ATOMIC_RELAXED);
    out->fallbacks = __atomic_load_n(&fallbacks, __ATOMIC_RELAXED);
    out->bitmap_failures = __atomic_load_n(&bitmapFailures, __ATOMIC_RELAXED);
}

static void* fallback(void* p)
{
    __atomic_fetch_add(&fallbacks, 1, __ATOMIC_RELAXED);
    return p;
}

/* -- the family -------------------------------------------------------------- */

void* __wrap_malloc(size_t size)
{
    if (UNLIKELY(!pas()))
        return __real_malloc(size);
    void* p = bmalloc_try_allocate(size ? size : 1);
    if (LIKELY(p))
        return p;
    return fallback(__real_malloc(size));
}

void* __wrap_calloc(size_t n, size_t size)
{
    if (UNLIKELY(!pas()))
        return __real_calloc(n, size);
    size_t total;
    if (__builtin_mul_overflow(n, size, &total)) {
        errno = ENOMEM;
        return NULL;
    }
    void* p = bmalloc_try_allocate_zeroed(total ? total : 1);
    if (LIKELY(p))
        return p;
    return fallback(__real_calloc(n, size));
}

void __wrap_free(void* p)
{
    if (!p)
        return;
    if (LIKELY(ours(p))) {
        bmalloc_deallocate(p);
        return;
    }
    __atomic_fetch_add(&foreignFrees, 1, __ATOMIC_RELAXED);
    __real_free(p);
}

void* __wrap_realloc(void* p, size_t size)
{
    if (!p)
        return __wrap_malloc(size);
    if (UNLIKELY(!ours(p))) {
        __atomic_fetch_add(&foreignReallocs, 1, __ATOMIC_RELAXED);
        return __real_realloc(p, size);
    }
    /* realloc(p, 0) keeps a minimal block, as FreeBSD's does. */
    void* q = bmalloc_try_realloc(p, size ? size : 1);
    if (!q)
        errno = ENOMEM;
    return q;
}

static void* aligned(size_t alignment, size_t size)
{
    if (alignment < 16)
        alignment = 16;
    return bmalloc_try_allocate_with_alignment(size ? size : 1, alignment);
}

int __wrap_posix_memalign(void** out, size_t alignment, size_t size)
{
    if (alignment < sizeof(void*) || (alignment & (alignment - 1)))
        return EINVAL;
    if (UNLIKELY(!pas()))
        return __real_posix_memalign(out, alignment, size);
    void* p = aligned(alignment, size);
    if (LIKELY(p)) {
        *out = p;
        return 0;
    }
    int rc = __real_posix_memalign(out, alignment, size);
    if (!rc)
        fallback(*out);
    return rc;
}

void* __wrap_aligned_alloc(size_t alignment, size_t size)
{
    if (!alignment || (alignment & (alignment - 1))) {
        errno = EINVAL;
        return NULL;
    }
    if (UNLIKELY(!pas()))
        return __real_aligned_alloc(alignment, size);
    void* p = aligned(alignment, size);
    return p ? p : fallback(__real_aligned_alloc(alignment, size));
}

void* __wrap_memalign(size_t alignment, size_t size)
{
    void* p = NULL;
    return __wrap_posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, size) ? NULL : p;
}

size_t __wrap_malloc_usable_size(const void* p)
{
    if (!p)
        return 0;
    return ours(p) ? bmalloc_get_allocation_size((void*)p) : __real_malloc_usable_size(p);
}

/* The console's libc allocates strdup's and vasprintf's results with its own
 * malloc, which our code then frees (fontconfig and glib call them a lot: 77k
 * foreign frees in a 48-load session). Made here instead. */
char* __wrap_strdup(const char* s)
{
    size_t n = strlen(s) + 1;
    char* p = (char*)__wrap_malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

int __wrap_vasprintf(char** out, const char* format, va_list arguments)
{
    va_list copy;
    va_copy(copy, arguments);
    int n = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    *out = NULL;
    if (n < 0)
        return -1;
    char* p = (char*)__wrap_malloc((size_t)n + 1);
    if (!p)
        return -1;
    vsnprintf(p, (size_t)n + 1, format, arguments);
    *out = p;
    return n;
}

/* -- emulated TLS (compiler-rt's emutls.c, with the console's malloc) -------- */

typedef struct __emutls_control {
    size_t size;
    size_t align;
    union {
        uintptr_t index;
        void* address;
    } object;
    void* value;
} __emutls_control;

typedef struct {
    uintptr_t skip_destructor_rounds;
    uintptr_t size; /* number of elements in data */
    void* data[];
} emutls_array;

/* Other keys' destructors (libpas's thread cache) may still use TLS after
 * ours ran once: keep the blocks for one more round. */
#define EMUTLS_SKIP_DESTRUCTOR_ROUNDS 1

static pthread_key_t emutls_key;
static int emutls_key_state; /* 0 none, 1 making, 2 ready */
static uintptr_t emutls_objects;
static int emutls_index_lock;

static void emutls_free_object(void* block)
{
    if (block)
    { void* base; memcpy(&base, (char*)block - sizeof(void*), sizeof base); __real_free(base); }
}

static void emutls_destructor(void* value)
{
    emutls_array* array = (emutls_array*)value;
    if (array->skip_destructor_rounds > 0) {
        array->skip_destructor_rounds--;
        pthread_setspecific(emutls_key, array);
        return;
    }
    for (uintptr_t i = 0; i < array->size; i++)
        emutls_free_object(array->data[i]);
    __real_free(array);
}

static void emutls_init(void)
{
    int state = __atomic_load_n(&emutls_key_state, __ATOMIC_ACQUIRE);
    if (state == 2)
        return;
    int expected = 0;
    if (__atomic_compare_exchange_n(&emutls_key_state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (pthread_key_create(&emutls_key, emutls_destructor))
            __builtin_trap();
        __atomic_store_n(&emutls_key_state, 2, __ATOMIC_RELEASE);
        return;
    }
    while (__atomic_load_n(&emutls_key_state, __ATOMIC_ACQUIRE) != 2)
        sched_yield();
}

static uintptr_t emutls_index(__emutls_control* control)
{
    uintptr_t index = __atomic_load_n(&control->object.index, __ATOMIC_ACQUIRE);
    if (LIKELY(index))
        return index;
    emutls_init();
    while (__atomic_exchange_n(&emutls_index_lock, 1, __ATOMIC_ACQUIRE))
        sched_yield();
    index = control->object.index;
    if (!index) {
        index = ++emutls_objects;
        __atomic_store_n(&control->object.index, index, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&emutls_index_lock, 0, __ATOMIC_RELEASE);
    return index;
}

static void* emutls_new_object(__emutls_control* control)
{
    size_t size = control->size, align = control->align;
    if (align < sizeof(void*))
        align = sizeof(void*);
    if (align & (align - 1))
        __builtin_trap();
    char* base = (char*)__real_malloc(size + align + sizeof(void*) - 1 + sizeof(void*));
    if (!base)
        __builtin_trap();
    char* object = (char*)(((uintptr_t)(base + sizeof(void*)) + align - 1) & ~(uintptr_t)(align - 1));
    memcpy(object - sizeof(void*), &base, sizeof base);
    if (control->value)
        memcpy(object, control->value, size);
    else
        memset(object, 0, size);
    return object;
}

void* __emutls_get_address(__emutls_control* control)
{
    uintptr_t index = emutls_index(control);
    emutls_array* array = (emutls_array*)pthread_getspecific(emutls_key);
    if (UNLIKELY(!array || index > array->size)) {
        uintptr_t old = array ? array->size : 0;
        /* Room for index, rounded so the header plus data is a multiple of 16 words. */
        uintptr_t size = ((index + 2 + 15) & ~(uintptr_t)15) - 2;
        emutls_array* bigger = (emutls_array*)__real_realloc(array, sizeof(emutls_array) + size * sizeof(void*));
        if (!bigger)
            __builtin_trap();
        if (!array)
            bigger->skip_destructor_rounds = EMUTLS_SKIP_DESTRUCTOR_ROUNDS;
        memset(bigger->data + old, 0, (size - old) * sizeof(void*));
        bigger->size = size;
        array = bigger;
        pthread_setspecific(emutls_key, array);
    }
    void* object = array->data[index - 1];
    if (UNLIKELY(!object)) {
        object = emutls_new_object(control);
        array->data[index - 1] = object;
    }
    return object;
}
