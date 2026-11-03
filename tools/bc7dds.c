/* bc7dds.c - make a home screen background (sce_sys/pic0.dds, pic1.dds) for
 * a title from an uncompressed 24-bit BMP (sips -s format bmp). The system's
 * own backgrounds are 3840x2160 BC7 (DXGI 98) DDS files with a DX10 header,
 * one mip level; this writes the same layout. Opaque images only: every
 * block is BC7 mode 6 (one subset, 7-bit endpoints + p-bit, 4-bit indices)
 * with alpha 255, endpoints along the block's principal axis, then refit by
 * least squares.
 *   cc -O2 -o bc7dds tools/bc7dds.c -lm
 *   bc7dds in.bmp out.dds [alphamode]   (DX10 miscFlags2: 3 opaque, 1 straight)
 *   bc7dds -d in.dds out.ppm            (decode, to check what was written)
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int W4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

static int interp(int e0, int e1, int w) { return ((64 - w) * e0 + w * e1 + 32) >> 6; }

/* 7-bit endpoint with p-bit 1 (alpha 255 needs it): the value is v*2+1. */
static int q7(float v)
{
    int q = (int)lrintf((v - 1.0f) / 2.0f);
    return q < 0 ? 0 : q > 127 ? 127 : q;
}

static void bits_put(uint8_t *b, int *pos, uint32_t v, int n)
{
    for (int i = 0; i < n; i++, (*pos)++)
        if (v >> i & 1)
            b[*pos >> 3] |= (uint8_t)(1u << (*pos & 7));
}

static uint32_t bits_get(const uint8_t *b, int *pos, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++, (*pos)++)
        v |= (uint32_t)(b[*pos >> 3] >> (*pos & 7) & 1) << i;
    return v;
}

/* Best index for each pixel against endpoints e[2][3] (8-bit); squared error. */
static long assign(const float px[16][3], const int e[2][3], int idx[16])
{
    long err = 0;
    for (int i = 0; i < 16; i++) {
        long best = -1;
        for (int k = 0; k < 16; k++) {
            long d = 0;
            for (int c = 0; c < 3; c++) {
                long t = interp(e[0][c], e[1][c], W4[k]) - lrintf(px[i][c]);
                d += t * t;
            }
            if (best < 0 || d < best) { best = d; idx[i] = k; }
        }
        err += best;
    }
    return err;
}

static void to_endpoints(const float lo[3], const float hi[3], int q[2][3], int e[2][3])
{
    for (int c = 0; c < 3; c++) {
        q[0][c] = q7(lo[c]); q[1][c] = q7(hi[c]);
        e[0][c] = q[0][c] * 2 + 1; e[1][c] = q[1][c] * 2 + 1;
    }
}

static void encode_block(const float px[16][3], uint8_t out[16])
{
    float mean[3] = { 0 };
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 3; c++) mean[c] += px[i][c] / 16.0f;
    float cov[6] = { 0 };
    for (int i = 0; i < 16; i++) {
        float d[3] = { px[i][0] - mean[0], px[i][1] - mean[1], px[i][2] - mean[2] };
        cov[0] += d[0] * d[0]; cov[1] += d[0] * d[1]; cov[2] += d[0] * d[2];
        cov[3] += d[1] * d[1]; cov[4] += d[1] * d[2]; cov[5] += d[2] * d[2];
    }
    float ax[3] = { 0.577f, 0.577f, 0.577f };
    for (int it = 0; it < 8; it++) {
        float n[3] = { cov[0] * ax[0] + cov[1] * ax[1] + cov[2] * ax[2],
                       cov[1] * ax[0] + cov[3] * ax[1] + cov[4] * ax[2],
                       cov[2] * ax[0] + cov[4] * ax[1] + cov[5] * ax[2] };
        float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l < 1e-6f) break;
        for (int c = 0; c < 3; c++) ax[c] = n[c] / l;
    }
    float tmin = 1e9f, tmax = -1e9f;
    for (int i = 0; i < 16; i++) {
        float t = (px[i][0] - mean[0]) * ax[0] + (px[i][1] - mean[1]) * ax[1] + (px[i][2] - mean[2]) * ax[2];
        if (t < tmin) tmin = t;
        if (t > tmax) tmax = t;
    }
    float lo[3], hi[3];
    for (int c = 0; c < 3; c++) { lo[c] = mean[c] + ax[c] * tmin; hi[c] = mean[c] + ax[c] * tmax; }

    int q[2][3], e[2][3], idx[16], bq[2][3], bidx[16];
    to_endpoints(lo, hi, q, e);
    long best = assign(px, e, idx);
    memcpy(bq, q, sizeof q); memcpy(bidx, idx, sizeof idx);
    /* Least-squares refit of the endpoints to the chosen weights. */
    for (int it = 0; it < 2 && best > 0; it++) {
        double aa = 0, ab = 0, bb = 0, ax_[3] = { 0 }, bx[3] = { 0 };
        for (int i = 0; i < 16; i++) {
            double w = W4[bidx[i]] / 64.0, a = 1.0 - w;
            aa += a * a; ab += a * w; bb += w * w;
            for (int c = 0; c < 3; c++) { ax_[c] += a * px[i][c]; bx[c] += w * px[i][c]; }
        }
        double det = aa * bb - ab * ab;
        if (fabs(det) < 1e-9) break;
        for (int c = 0; c < 3; c++) {
            lo[c] = (float)((ax_[c] * bb - bx[c] * ab) / det);
            hi[c] = (float)((bx[c] * aa - ax_[c] * ab) / det);
        }
        to_endpoints(lo, hi, q, e);
        long err = assign(px, e, idx);
        if (err >= best) break;
        best = err; memcpy(bq, q, sizeof q); memcpy(bidx, idx, sizeof idx);
    }
    /* The first index is stored in 3 bits: its top bit must be 0. */
    if (bidx[0] & 8) {
        for (int c = 0; c < 3; c++) { int t = bq[0][c]; bq[0][c] = bq[1][c]; bq[1][c] = t; }
        for (int i = 0; i < 16; i++) bidx[i] = 15 - bidx[i];
    }
    memset(out, 0, 16);
    int pos = 0;
    bits_put(out, &pos, 1u << 6, 7);                 /* mode 6 */
    for (int c = 0; c < 3; c++) { bits_put(out, &pos, bq[0][c], 7); bits_put(out, &pos, bq[1][c], 7); }
    bits_put(out, &pos, 127, 7); bits_put(out, &pos, 127, 7);  /* alpha */
    bits_put(out, &pos, 1, 1); bits_put(out, &pos, 1, 1);      /* p-bits */
    bits_put(out, &pos, bidx[0], 3);
    for (int i = 1; i < 16; i++) bits_put(out, &pos, bidx[i], 4);
}

static void decode_block(const uint8_t in[16], uint8_t rgb[16][3])
{
    int pos = 0;
    if (bits_get(in, &pos, 7) != 1u << 6) { memset(rgb, 255, 48); return; }  /* only mode 6 */
    int q[2][3];
    for (int c = 0; c < 3; c++) { q[0][c] = bits_get(in, &pos, 7); q[1][c] = bits_get(in, &pos, 7); }
    bits_get(in, &pos, 14);
    int p0 = bits_get(in, &pos, 1), p1 = bits_get(in, &pos, 1);
    for (int i = 0; i < 16; i++) {
        int k = bits_get(in, &pos, i ? 4 : 3);
        for (int c = 0; c < 3; c++)
            rgb[i][c] = (uint8_t)interp(q[0][c] * 2 + p0, q[1][c] * 2 + p1, W4[k]);
    }
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static uint8_t *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len);
    if (fread(b, 1, *len, f) != (size_t)*len) { perror(path); exit(1); }
    fclose(f);
    return b;
}

static int decode_file(const char *in, const char *out)
{
    long len;
    uint8_t *d = slurp(in, &len);
    if (len < 148 || memcmp(d, "DDS ", 4) || le32(d + 128) != 98) { fprintf(stderr, "%s: not a BC7 DX10 DDS\n", in); return 1; }
    int h = le32(d + 12), w = le32(d + 16);
    uint8_t *img = malloc((size_t)w * h * 3);
    const uint8_t *blk = d + 148;
    for (int by = 0; by < h / 4; by++)
        for (int bx = 0; bx < w / 4; bx++, blk += 16) {
            uint8_t rgb[16][3];
            decode_block(blk, rgb);
            for (int i = 0; i < 16; i++)
                memcpy(img + (((size_t)by * 4 + i / 4) * w + bx * 4 + i % 4) * 3, rgb[i], 3);
        }
    FILE *f = fopen(out, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(img, 1, (size_t)w * h * 3, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "-d"))
        return decode_file(argv[2], argv[3]);
    if (argc < 3) { fprintf(stderr, "usage: bc7dds in.bmp out.dds [alphamode] | bc7dds -d in.dds out.ppm\n"); return 2; }
    long len;
    uint8_t *b = slurp(argv[1], &len);
    if (len < 54 || b[0] != 'B' || b[1] != 'M') { fprintf(stderr, "%s: not a BMP\n", argv[1]); return 1; }
    uint32_t off = le32(b + 10);
    int w = (int32_t)le32(b + 18), h = (int32_t)le32(b + 22), bpp = b[28] | b[29] << 8;
    int topdown = h < 0;
    if (topdown) h = -h;
    if ((bpp != 24 && bpp != 32) || le32(b + 30) != 0 || w % 4 || h % 4) {
        fprintf(stderr, "%s: need an uncompressed 24/32-bit BMP, sides a multiple of 4 (%dx%d, %d bpp)\n", argv[1], w, h, bpp);
        return 1;
    }
    int bytespp = bpp / 8;
    size_t stride = ((size_t)w * bytespp + 3) & ~(size_t)3;
    if (off + stride * h > (size_t)len) { fprintf(stderr, "%s: truncated\n", argv[1]); return 1; }

    size_t dsz = (size_t)w * h;                    /* BC7: 1 byte per pixel */
    uint8_t *dds = calloc(1, 148 + dsz);
    memcpy(dds, "DDS ", 4);
    put32(dds + 4, 124);
    put32(dds + 8, 0x000A1007);                    /* caps height width pixelformat mipcount linearsize */
    put32(dds + 12, h); put32(dds + 16, w);
    put32(dds + 20, (uint32_t)dsz);
    put32(dds + 24, 1);                            /* depth */
    put32(dds + 28, 1);                            /* mip levels */
    put32(dds + 76, 32); put32(dds + 80, 4);       /* pixel format: FourCC */
    memcpy(dds + 84, "DX10", 4);
    put32(dds + 108, 0x1000);                      /* DDSCAPS_TEXTURE */
    put32(dds + 128, 98);                          /* DXGI_FORMAT_BC7_UNORM */
    put32(dds + 132, 3);                           /* 2D texture */
    put32(dds + 140, 1);                           /* array size */
    put32(dds + 144, argc > 3 ? (uint32_t)atoi(argv[3]) : 3);

    uint8_t *blk = dds + 148;
    double se = 0;
    for (int by = 0; by < h / 4; by++)
        for (int bx = 0; bx < w / 4; bx++, blk += 16) {
            float px[16][3];
            for (int i = 0; i < 16; i++) {
                int y = by * 4 + i / 4, x = bx * 4 + i % 4;
                const uint8_t *p = b + off + stride * (topdown ? y : h - 1 - y) + (size_t)x * bytespp;
                px[i][0] = p[2]; px[i][1] = p[1]; px[i][2] = p[0];   /* BMP is BGR */
            }
            encode_block(px, blk);
            uint8_t rgb[16][3];
            decode_block(blk, rgb);
            for (int i = 0; i < 16; i++)
                for (int c = 0; c < 3; c++) { double t = rgb[i][c] - px[i][c]; se += t * t; }
        }
    FILE *f = fopen(argv[2], "wb");
    if (!f || fwrite(dds, 1, 148 + dsz, f) != 148 + dsz || fclose(f)) { perror(argv[2]); return 1; }
    double mse = se / ((double)w * h * 3);
    printf("%s: %dx%d BC7, %zu bytes, PSNR %.1f dB\n", argv[2], w, h, 148 + dsz, mse > 0 ? 10 * log10(65025.0 / mse) : 99.0);
    return 0;
}
