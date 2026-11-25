/*
 * ps5mem.c: memcpy, memmove and memset with AVX2, in the title and in
 * ps5wk-child (plain definitions: the linker takes them before the system
 * libraries' stubs).
 *
 * Why: the executables import these from the console's libkernel, where they
 * are FreeBSD's old "rep movsq, then rep movsb" loops (and "std; rep movsb"
 * backwards for an overlapping move). On the console's Zen 2 a rep move has a
 * start-up cost of tens of cycles and no fast path for short or unaligned
 * blocks, and everything in the browser copies through them: WebKit, Skia,
 * Dawn, JSC, and WebAssembly's memory.copy and memory.fill. A PS2 emulator
 * page (ssx3.gamingretro.online, 2026-11-25) had 61% of its busiest worker's
 * CPU in libkernel's memmove.
 *
 * How: up to 256 bytes with no loop, from loads and stores that overlap in
 * the middle (every load before the first store, so overlapping blocks are
 * safe); longer blocks 128 bytes a turn with the destination aligned to 32,
 * forwards or backwards by how the blocks overlap. memcpy is memmove: the
 * check costs nothing next to the copy, and code that passes overlapping
 * blocks to memcpy keeps working.
 *
 * Built with -mavx2 -fno-builtin (apps/CMakeLists.txt): without the second
 * the compiler may turn a loop here into a call to the function it is in.
 * PS5MEM_PREFIX=<name> gives the functions other names (the host test,
 * test/mem/ps5mem-test.c).
 */
#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>

#ifdef PS5MEM_PREFIX
#define PS5MEM_PASTE2(a, b) a##b
#define PS5MEM_PASTE(a, b) PS5MEM_PASTE2(a, b)
#define PS5MEM_NAME(name) PS5MEM_PASTE(PS5MEM_PREFIX, name)
#else
#define PS5MEM_NAME(name) name
#endif

#define INLINE static inline __attribute__((always_inline))
#define NO_BUILTIN __attribute__((no_builtin("memcpy", "memmove", "memset")))

typedef struct __attribute__((packed, may_alias)) { uint64_t v; } U64;
typedef struct __attribute__((packed, may_alias)) { uint32_t v; } U32;
typedef struct __attribute__((packed, may_alias)) { uint16_t v; } U16;

INLINE uint64_t get64(const unsigned char* p) { return ((const U64*)p)->v; }
INLINE uint32_t get32(const unsigned char* p) { return ((const U32*)p)->v; }
INLINE uint16_t get16(const unsigned char* p) { return ((const U16*)p)->v; }
INLINE void put64(unsigned char* p, uint64_t v) { ((U64*)p)->v = v; }
INLINE void put32(unsigned char* p, uint32_t v) { ((U32*)p)->v = v; }
INLINE void put16(unsigned char* p, uint16_t v) { ((U16*)p)->v = v; }
INLINE __m128i get128(const unsigned char* p) { return _mm_loadu_si128((const __m128i*)p); }
INLINE void put128(unsigned char* p, __m128i v) { _mm_storeu_si128((__m128i*)p, v); }
INLINE __m256i get256(const unsigned char* p) { return _mm256_loadu_si256((const __m256i*)p); }
INLINE void put256(unsigned char* p, __m256i v) { _mm256_storeu_si256((__m256i*)p, v); }
INLINE void putAligned256(unsigned char* p, __m256i v) { _mm256_store_si256((__m256i*)p, v); }

NO_BUILTIN void* PS5MEM_NAME(memmove)(void* destination, const void* source, size_t n)
{
    unsigned char* d = (unsigned char*)destination;
    const unsigned char* s = (const unsigned char*)source;

    if (n <= 16) {
        if (n >= 8) {
            uint64_t a = get64(s), b = get64(s + n - 8);
            put64(d, a);
            put64(d + n - 8, b);
        } else if (n >= 4) {
            uint32_t a = get32(s), b = get32(s + n - 4);
            put32(d, a);
            put32(d + n - 4, b);
        } else if (n >= 2) {
            uint16_t a = get16(s), b = get16(s + n - 2);
            put16(d, a);
            put16(d + n - 2, b);
        } else if (n)
            d[0] = s[0];
        return destination;
    }
    if (n <= 32) {
        __m128i a = get128(s), b = get128(s + n - 16);
        put128(d, a);
        put128(d + n - 16, b);
        return destination;
    }
    if (n <= 64) {
        __m256i a = get256(s), b = get256(s + n - 32);
        put256(d, a);
        put256(d + n - 32, b);
        return destination;
    }
    if (n <= 128) {
        __m256i a = get256(s), b = get256(s + 32), c = get256(s + n - 64), e = get256(s + n - 32);
        put256(d, a);
        put256(d + 32, b);
        put256(d + n - 64, c);
        put256(d + n - 32, e);
        return destination;
    }
    if (n <= 256) {
        __m256i a0 = get256(s), a1 = get256(s + 32), a2 = get256(s + 64), a3 = get256(s + 96);
        __m256i b0 = get256(s + n - 128), b1 = get256(s + n - 96), b2 = get256(s + n - 64), b3 = get256(s + n - 32);
        put256(d, a0);
        put256(d + 32, a1);
        put256(d + 64, a2);
        put256(d + 96, a3);
        put256(d + n - 128, b0);
        put256(d + n - 96, b1);
        put256(d + n - 64, b2);
        put256(d + n - 32, b3);
        return destination;
    }

    if ((uintptr_t)d - (uintptr_t)s >= n) {
        /* Forwards: the destination starts before the source, or past its
         * end. The first 32 and the last 128 bytes are read before anything
         * is written; a turn reads its 128 bytes before it writes them, and
         * what it writes lies before what later turns read. */
        __m256i head = get256(s);
        __m256i t0 = get256(s + n - 128), t1 = get256(s + n - 96), t2 = get256(s + n - 64), t3 = get256(s + n - 32);
        size_t i = 32 - ((uintptr_t)d & 31);
        for (; i < n - 128; i += 128) {
            __m256i a = get256(s + i), b = get256(s + i + 32), c = get256(s + i + 64), e = get256(s + i + 96);
            putAligned256(d + i, a);
            putAligned256(d + i + 32, b);
            putAligned256(d + i + 64, c);
            putAligned256(d + i + 96, e);
        }
        put256(d, head);
        put256(d + n - 128, t0);
        put256(d + n - 96, t1);
        put256(d + n - 64, t2);
        put256(d + n - 32, t3);
        return destination;
    }

    /* Backwards: the destination starts inside the source. The mirror image:
     * the last 32 and the first 128 bytes are read first, and the turns go
     * down from the end. */
    __m256i tail = get256(s + n - 32);
    __m256i h0 = get256(s), h1 = get256(s + 32), h2 = get256(s + 64), h3 = get256(s + 96);
    size_t i = n - ((uintptr_t)(d + n) & 31);
    while (i > 128) {
        i -= 128;
        __m256i a = get256(s + i), b = get256(s + i + 32), c = get256(s + i + 64), e = get256(s + i + 96);
        putAligned256(d + i, a);
        putAligned256(d + i + 32, b);
        putAligned256(d + i + 64, c);
        putAligned256(d + i + 96, e);
    }
    put256(d + n - 32, tail);
    put256(d, h0);
    put256(d + 32, h1);
    put256(d + 64, h2);
    put256(d + 96, h3);
    return destination;
}

NO_BUILTIN void* PS5MEM_NAME(memcpy)(void* destination, const void* source, size_t n)
{
    return PS5MEM_NAME(memmove)(destination, source, n);
}

NO_BUILTIN void* PS5MEM_NAME(memset)(void* destination, int value, size_t n)
{
    unsigned char* d = (unsigned char*)destination;
    unsigned char byte = (unsigned char)value;

    if (n <= 16) {
        uint64_t pattern = byte * 0x0101010101010101ULL;
        if (n >= 8) {
            put64(d, pattern);
            put64(d + n - 8, pattern);
        } else if (n >= 4) {
            put32(d, (uint32_t)pattern);
            put32(d + n - 4, (uint32_t)pattern);
        } else if (n >= 2) {
            put16(d, (uint16_t)pattern);
            put16(d + n - 2, (uint16_t)pattern);
        } else if (n)
            d[0] = byte;
        return destination;
    }
    if (n <= 32) {
        __m128i v = _mm_set1_epi8((char)byte);
        put128(d, v);
        put128(d + n - 16, v);
        return destination;
    }
    __m256i v = _mm256_set1_epi8((char)byte);
    if (n <= 64) {
        put256(d, v);
        put256(d + n - 32, v);
        return destination;
    }
    if (n <= 128) {
        put256(d, v);
        put256(d + 32, v);
        put256(d + n - 64, v);
        put256(d + n - 32, v);
        return destination;
    }
    if (n <= 256) {
        put256(d, v);
        put256(d + 32, v);
        put256(d + 64, v);
        put256(d + 96, v);
        put256(d + n - 128, v);
        put256(d + n - 96, v);
        put256(d + n - 64, v);
        put256(d + n - 32, v);
        return destination;
    }
    put256(d, v);
    for (size_t i = 32 - ((uintptr_t)d & 31); i < n - 128; i += 128) {
        putAligned256(d + i, v);
        putAligned256(d + i + 32, v);
        putAligned256(d + i + 64, v);
        putAligned256(d + i + 96, v);
    }
    put256(d + n - 128, v);
    put256(d + n - 96, v);
    put256(d + n - 64, v);
    put256(d + n - 32, v);
    return destination;
}
