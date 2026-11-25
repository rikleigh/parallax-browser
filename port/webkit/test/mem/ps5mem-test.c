/*
 * Host test for apps/common/ps5mem.c (x86-64 with AVX2; on an Apple Silicon
 * Mac it runs under Rosetta):
 *
 *   clang -arch x86_64 -O2 -mavx2 -fno-builtin -DPS5MEM_PREFIX=ps5_ \
 *       -o /tmp/ps5mem-test port/webkit/test/mem/ps5mem-test.c port/webkit/apps/common/ps5mem.c && /tmp/ps5mem-test
 *
 * Every length up to 700 (and some long ones) at every alignment of both
 * ends, copies that overlap by every small distance in both directions, and
 * guard bytes around each destination. Then a rough speed comparison with
 * the host's own functions.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void* ps5_memmove(void*, const void*, size_t);
void* ps5_memcpy(void*, const void*, size_t);
void* ps5_memset(void*, int, size_t);

#define ARENA (1 << 20)
static unsigned char a[ARENA], b[ARENA], model[ARENA];
static uint32_t seed = 12345;
static uint32_t rnd(void) { seed = seed * 1664525u + 1013904223u; return seed >> 8; }
static void fill(unsigned char* p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = (unsigned char)rnd(); }
static long failures;

static void fail(const char* what, size_t n, size_t x, size_t y)
{
    if (failures++ < 20)
        printf("FAIL %s: n %zu, offsets %zu and %zu\n", what, n, x, y);
}

/* Reference move, a byte at a time in the safe direction. */
static void slowMove(unsigned char* d, const unsigned char* s, size_t n)
{
    if (d < s)
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    else
        for (size_t i = n; i--;) d[i] = s[i];
}

static void testCopy(size_t n, size_t sourceOffset, size_t destinationOffset)
{
    /* Separate arrays: memcpy and memmove. */
    memset(b, 0xA5, destinationOffset + n + 64 + 64);
    void* r = ps5_memcpy(b + 64 + destinationOffset, a + sourceOffset, n);
    if (r != b + 64 + destinationOffset || memcmp(b + 64 + destinationOffset, a + sourceOffset, n))
        fail("memcpy", n, sourceOffset, destinationOffset);
    for (size_t i = 0; i < 64; i++)
        if (b[destinationOffset + i] != 0xA5 || b[64 + destinationOffset + n + i] != 0xA5) { fail("memcpy guard", n, sourceOffset, destinationOffset); break; }
}

static void testOverlap(size_t n, size_t base, long distance)
{
    /* One array: the destination is `distance` bytes from the source. */
    size_t span = n + 1200;
    fill(b, span);
    memcpy(model, b, span);
    unsigned char* s = b + 512 + base;
    unsigned char* d = s + distance;
    slowMove(model + (d - b), model + (s - b), n);
    ps5_memmove(d, s, n);
    if (memcmp(b, model, span))
        fail("memmove overlap", n, base, (size_t)(distance + 512));
    /* memcpy is the same function here: it must cope too. */
    fill(b, span);
    memcpy(model, b, span);
    slowMove(model + (d - b), model + (s - b), n);
    ps5_memcpy(d, s, n);
    if (memcmp(b, model, span))
        fail("memcpy overlap", n, base, (size_t)(distance + 512));
}

static void testSet(size_t n, size_t offset, int value)
{
    memset(b, 0x5A, offset + n + 128);
    void* r = ps5_memset(b + 64 + offset, value, n);
    if (r != b + 64 + offset)
        fail("memset result", n, offset, 0);
    for (size_t i = 0; i < n; i++)
        if (b[64 + offset + i] != (unsigned char)value) { fail("memset", n, offset, i); break; }
    for (size_t i = 0; i < 64; i++)
        if (b[offset + i] != 0x5A || b[64 + offset + n + i] != 0x5A) { fail("memset guard", n, offset, 0); break; }
}

static double seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void* (*volatile hostMove)(void*, const void*, size_t) = memmove;
static void* (*volatile ours)(void*, const void*, size_t) = ps5_memmove;

static void speed(size_t n)
{
    size_t rounds = (size_t)(200000000 / (n + 40));
    double t = seconds();
    for (size_t i = 0; i < rounds; i++)
        ours(b + (i & 63), a + ((i * 7) & 63), n);
    double mine = seconds() - t;
    t = seconds();
    for (size_t i = 0; i < rounds; i++)
        hostMove(b + (i & 63), a + ((i * 7) & 63), n);
    double host = seconds() - t;
    printf("  %7zu bytes: ps5mem %6.1f ns, host %6.1f ns a call\n", n, mine / rounds * 1e9, host / rounds * 1e9);
}

int main(void)
{
    fill(a, ARENA);
    long cases = 0;
    for (size_t n = 0; n <= 700; n++)
        for (size_t so = 0; so < 33; so += (n > 300 ? 3 : 1))
            for (size_t dof = 0; dof < 33; dof += (n > 300 ? 5 : 1)) { testCopy(n, so, dof); cases++; }
    static const size_t longs[] = { 1000, 1023, 1024, 1025, 4095, 4096, 4097, 65536, 65537, 100000, 500001 };
    for (unsigned k = 0; k < sizeof longs / sizeof *longs; k++)
        for (size_t so = 0; so < 33; so += 4)
            for (size_t dof = 0; dof < 33; dof += 3) { testCopy(longs[k], so, dof); cases++; }
    for (size_t n = 0; n <= 700; n += (n > 300 ? 7 : 1))
        for (size_t base = 0; base < 33; base += (n > 100 ? 11 : 4))
            for (long distance = -300; distance <= 300; distance += (n > 300 && labs(distance) > 40 ? 13 : 1)) { testOverlap(n, base, distance); cases++; }
    for (unsigned k = 0; k < sizeof longs / sizeof *longs; k++)
        for (long distance = -400; distance <= 400; distance += 7) { testOverlap(longs[k], k, distance); cases++; }
    for (size_t n = 0; n <= 700; n++)
        for (size_t offset = 0; offset < 33; offset++) { testSet(n, offset, (int)(n * 31 + offset)); cases++; }
    for (unsigned k = 0; k < sizeof longs / sizeof *longs; k++)
        for (size_t offset = 0; offset < 33; offset++) { testSet(longs[k], offset, 0x100 + (int)k); cases++; }
    printf("%ld cases, %ld failures\n", cases, failures);
    if (failures)
        return 1;
    printf("speed (unaligned ends):\n");
    static const size_t sizes[] = { 5, 13, 24, 48, 100, 200, 500, 2000, 16000, 200000 };
    for (unsigned k = 0; k < sizeof sizes / sizeof *sizes; k++)
        speed(sizes[k]);
    return 0;
}
