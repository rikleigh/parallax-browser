/*
 * QR code encoder: see qrcode.h. Byte mode, level M, versions 1-10.
 */
#include "qrcode.h"

#include <string.h>

/* Level M, versions 1-10: error correction codewords per block, blocks in
 * group 1 and their data codewords, blocks in group 2 (one more each). */
static const struct {
    uint8_t ecPerBlock, blocks1, data1, blocks2;
} kBlocks[11] = {
    { 0, 0, 0, 0 },
    { 10, 1, 16, 0 }, { 16, 1, 28, 0 }, { 26, 1, 44, 0 }, { 18, 2, 32, 0 }, { 24, 2, 43, 0 },
    { 16, 4, 27, 0 }, { 18, 4, 31, 0 }, { 22, 2, 38, 2 }, { 22, 3, 36, 2 }, { 26, 4, 43, 1 },
};
/* Alignment pattern centres (versions 2-10). */
static const uint8_t kAlign[11][3] = {
    { 0 }, { 0 }, { 6, 18 }, { 6, 22 }, { 6, 26 }, { 6, 30 }, { 6, 34 }, { 6, 22, 38 }, { 6, 24, 42 }, { 6, 26, 46 }, { 6, 28, 50 },
};

static int dataCodewords(int version)
{
    return kBlocks[version].blocks1 * kBlocks[version].data1 + kBlocks[version].blocks2 * (kBlocks[version].data1 + 1);
}

/* ---- Reed-Solomon over GF(256), polynomial 0x11d */

static uint8_t gfMul(uint8_t a, uint8_t b)
{
    unsigned r = 0;
    for (int i = 7; i >= 0; i--) {
        r = (r << 1) ^ ((r >> 7) * 0x11d);
        r ^= ((b >> i) & 1) * a;
    }
    return (uint8_t)r;
}

static void rsDivisor(int degree, uint8_t* out)
{
    memset(out, 0, (size_t)degree);
    out[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (int j = 0; j < degree; j++) {
            out[j] = gfMul(out[j], root);
            if (j + 1 < degree)
                out[j] ^= out[j + 1];
        }
        root = gfMul(root, 0x02);
    }
}

static void rsRemainder(const uint8_t* data, int length, const uint8_t* divisor, int degree, uint8_t* out)
{
    memset(out, 0, (size_t)degree);
    for (int i = 0; i < length; i++) {
        uint8_t factor = data[i] ^ out[0];
        memmove(out, out + 1, (size_t)degree - 1);
        out[degree - 1] = 0;
        for (int j = 0; j < degree; j++)
            out[j] ^= gfMul(divisor[j], factor);
    }
}

/* ---- the grid */

typedef struct {
    int size;
    uint8_t* m;          /* modules */
    uint8_t fn[QR_MAX_SIZE * QR_MAX_SIZE]; /* 1: a function module (not data) */
} Grid;

static void set(Grid* g, int x, int y, int dark)
{
    if (x < 0 || y < 0 || x >= g->size || y >= g->size)
        return;
    g->m[y * g->size + x] = (uint8_t)dark;
    g->fn[y * g->size + x] = 1;
}

static void finder(Grid* g, int cx, int cy)
{
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
            int d = dx < 0 ? -dx : dx, e = dy < 0 ? -dy : dy;
            int dist = d > e ? d : e;
            set(g, cx + dx, cy + dy, dist != 2 && dist != 4);
        }
    }
}

static void alignment(Grid* g, int cx, int cy)
{
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int d = dx < 0 ? -dx : dx, e = dy < 0 ? -dy : dy;
            set(g, cx + dx, cy + dy, (d > e ? d : e) != 1);
        }
    }
}

static void formatBits(Grid* g, int mask)
{
    int data = (0 << 3) | mask; /* level M: 00 */
    int rem = data;
    for (int i = 0; i < 10; i++)
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = ((data << 10) | rem) ^ 0x5412;
    int s = g->size;
    for (int i = 0; i <= 5; i++)
        set(g, 8, i, (bits >> i) & 1);
    set(g, 8, 7, (bits >> 6) & 1);
    set(g, 8, 8, (bits >> 7) & 1);
    set(g, 7, 8, (bits >> 8) & 1);
    for (int i = 9; i < 15; i++)
        set(g, 14 - i, 8, (bits >> i) & 1);
    for (int i = 0; i < 8; i++)
        set(g, s - 1 - i, 8, (bits >> i) & 1);
    for (int i = 8; i < 15; i++)
        set(g, 8, s - 15 + i, (bits >> i) & 1);
    set(g, 8, s - 8, 1); /* the dark module */
}

static void versionBits(Grid* g, int version)
{
    if (version < 7)
        return;
    int rem = version;
    for (int i = 0; i < 12; i++)
        rem = (rem << 1) ^ ((rem >> 11) * 0x1f25);
    long bits = ((long)version << 12) | rem;
    for (int i = 0; i < 18; i++) {
        int bit = (bits >> i) & 1, a = g->size - 11 + i % 3, b = i / 3;
        set(g, a, b, bit);
        set(g, b, a, bit);
    }
}

static void functionPatterns(Grid* g, int version)
{
    int s = g->size;
    for (int i = 0; i < s; i++) {
        set(g, 6, i, i % 2 == 0);
        set(g, i, 6, i % 2 == 0);
    }
    finder(g, 3, 3);
    finder(g, s - 4, 3);
    finder(g, 3, s - 4);
    int n = version >= 7 ? 3 : version >= 2 ? 2 : 0;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0))
                continue;
            alignment(g, kAlign[version][i], kAlign[version][j]);
        }
    }
    formatBits(g, 0); /* reserved now, written for real after masking */
    versionBits(g, version);
}

static void placeData(Grid* g, const uint8_t* codewords, int count)
{
    int s = g->size, i = 0;
    for (int right = s - 1; right >= 1; right -= 2) {
        if (right == 6)
            right = 5;
        for (int vert = 0; vert < s; vert++) {
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                int upward = ((right + 1) & 2) == 0;
                int y = upward ? s - 1 - vert : vert;
                if (!g->fn[y * s + x] && i < count * 8) {
                    g->m[y * s + x] = (codewords[i >> 3] >> (7 - (i & 7))) & 1;
                    i++;
                }
            }
        }
    }
}

static void applyMask(Grid* g, int mask)
{
    int s = g->size;
    for (int y = 0; y < s; y++) {
        for (int x = 0; x < s; x++) {
            if (g->fn[y * s + x])
                continue;
            int invert;
            switch (mask) {
            case 0: invert = (x + y) % 2 == 0; break;
            case 1: invert = y % 2 == 0; break;
            case 2: invert = x % 3 == 0; break;
            case 3: invert = (x + y) % 3 == 0; break;
            case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
            case 5: invert = x * y % 2 + x * y % 3 == 0; break;
            case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
            default: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
            }
            g->m[y * s + x] ^= (uint8_t)invert;
        }
    }
}

/* The spec's penalty: runs, 2x2 blocks, finder-like patterns, balance. */
static long penalty(const Grid* g)
{
    int s = g->size;
    long score = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int a = 0; a < s; a++) {
            int run = 0, last = -1;
            unsigned window = 0;
            for (int b = 0; b < s; b++) {
                int v = pass ? g->m[b * s + a] : g->m[a * s + b];
                if (v == last) {
                    run++;
                    if (run == 5)
                        score += 3;
                    else if (run > 5)
                        score++;
                } else {
                    last = v;
                    run = 1;
                }
                window = ((window << 1) | (unsigned)v) & 0x7ff;
                if (b >= 10 && (window == 0x5d0 || window == 0x05d)) /* 10111010000, 00001011101 */
                    score += 40;
            }
        }
    }
    int dark = 0;
    for (int y = 0; y < s; y++) {
        for (int x = 0; x < s; x++) {
            int v = g->m[y * s + x];
            dark += v;
            if (x + 1 < s && y + 1 < s && v == g->m[y * s + x + 1] && v == g->m[(y + 1) * s + x] && v == g->m[(y + 1) * s + x + 1])
                score += 3;
        }
    }
    int total = s * s;
    int k = 0;
    while ((dark * 20 < (9 - k) * total || dark * 20 > (11 + k) * total))
        k++;
    return score + k * 10;
}

int qr_encode(const char* text, size_t length, uint8_t* modules)
{
    int version = 1;
    for (; version <= 10; version++) {
        int countBits = version < 10 ? 8 : 16;
        if (4 + countBits + 8 * (long)length <= dataCodewords(version) * 8)
            break;
    }
    if (version > 10)
        return 0;

    /* data: mode, count, bytes, terminator, padding */
    uint8_t data[400];
    const int capacity = dataCodewords(version);
    memset(data, 0, sizeof data);
    int bit = 0;
#define PUT(value, n)                                                    \
    for (int i_ = (n) - 1; i_ >= 0; i_--, bit++)                         \
        if (((value) >> i_) & 1)                                         \
            data[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7));
    PUT(4, 4);
    PUT((unsigned)length, version < 10 ? 8 : 16);
    for (size_t i = 0; i < length; i++)
        PUT((uint8_t)text[i], 8);
    int terminator = capacity * 8 - bit < 4 ? capacity * 8 - bit : 4;
    bit += terminator;
    bit = (bit + 7) & ~7;
    for (int pad = 0; bit < capacity * 8; pad ^= 1)
        PUT(pad ? 0x11 : 0xec, 8);
#undef PUT

    /* blocks, their error correction, interleaved */
    const int ec = kBlocks[version].ecPerBlock;
    const int blocks = kBlocks[version].blocks1 + kBlocks[version].blocks2;
    uint8_t divisor[32], ecc[8][32];
    int blockStart[8], blockLength[8];
    rsDivisor(ec, divisor);
    for (int b = 0, at = 0; b < blocks; b++) {
        blockLength[b] = kBlocks[version].data1 + (b >= kBlocks[version].blocks1);
        blockStart[b] = at;
        rsRemainder(data + at, blockLength[b], divisor, ec, ecc[b]);
        at += blockLength[b];
    }
    uint8_t codewords[400];
    int n = 0;
    for (int i = 0; i <= kBlocks[version].data1; i++) {
        for (int b = 0; b < blocks; b++) {
            if (i < blockLength[b])
                codewords[n++] = data[blockStart[b] + i];
        }
    }
    for (int i = 0; i < ec; i++) {
        for (int b = 0; b < blocks; b++)
            codewords[n++] = ecc[b][i];
    }

    Grid g;
    g.size = 17 + 4 * version;
    g.m = modules;
    memset(g.m, 0, (size_t)g.size * g.size);
    memset(g.fn, 0, sizeof g.fn);
    functionPatterns(&g, version);
    placeData(&g, codewords, n);

    int best = 0;
    long bestScore = -1;
    for (int mask = 0; mask < 8; mask++) {
        applyMask(&g, mask);
        formatBits(&g, mask);
        long score = penalty(&g);
        if (bestScore < 0 || score < bestScore) {
            bestScore = score;
            best = mask;
        }
        applyMask(&g, mask); /* undo */
    }
    applyMask(&g, best);
    formatBits(&g, best);
    return g.size;
}
