/*
 * View in VR: the frame-layout guess from the video's own pictures. See
 * vrlayout.h.
 *
 * Halves are compared after taking each row's mean out of each half, so
 * only the detail along a row counts: a 360 picture's sky above and ground
 * below make every pair of halves alike row by row, but its left half
 * (behind and to the left) and right half (to the right) show different
 * things. Stereo halves are the same scene a few cells apart (disparity),
 * so the best of a few small shifts is taken.
 */
#include "vrlayout.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define N VRLAYOUT_SIZE
#define MAX_DX 4 /* cells: about 11 degrees of disparity for 180 side by side */
#define MAX_DY 1

/* Thresholds (test/vrlayout runs them over synthetic clips). */
#define MIN_DETAIL 2.5     /* residual luma standard deviation: below, halves can't be told apart */
#define MATCH 0.60         /* halves this alike (the other split not): stereo */
#define UNRELATED 0.40     /* halves less alike: one picture (mono) */
#define WRAP_JOINS 0.25    /* at most this share of rows break across the left/right edges: they join */

/* Each row's mean, then each column's, out of a w x h block at src (row
 * stride N), into out: what is left is detail along both directions (the
 * sky above and the ground below, or upright stripes and bars running
 * through both halves, would make any two halves alike). */
static void residual(const unsigned char* src, int w, int h, float* out)
{
    for (int y = 0; y < h; y++) {
        double sum = 0;
        for (int x = 0; x < w; x++)
            sum += src[y * N + x];
        float mean = (float)(sum / w);
        for (int x = 0; x < w; x++)
            out[y * w + x] = src[y * N + x] - mean;
    }
    for (int x = 0; x < w; x++) {
        double sum = 0;
        for (int y = 0; y < h; y++)
            sum += out[y * w + x];
        float mean = (float)(sum / h);
        for (int y = 0; y < h; y++)
            out[y * w + x] -= mean;
    }
}

/* Best normalised correlation of a and b (w x h) over small shifts of b;
 * *detail: the smaller standard deviation of the two, unshifted. */
static double bestCorrelation(const float* a, const float* b, int w, int h, double* detail)
{
    double best = -1;
    *detail = 0;
    for (int dy = -MAX_DY; dy <= MAX_DY; dy++) {
        for (int dx = -MAX_DX; dx <= MAX_DX; dx++) {
            double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
            int n = 0;
            for (int y = MAX_DY; y < h - MAX_DY; y++) {
                for (int x = MAX_DX; x < w - MAX_DX; x++) {
                    double va = a[y * w + x], vb = b[(y + dy) * w + x + dx];
                    sa += va;
                    sb += vb;
                    saa += va * va;
                    sbb += vb * vb;
                    sab += va * vb;
                    n++;
                }
            }
            double varA = saa - sa * sa / n, varB = sbb - sb * sb / n, cov = sab - sa * sb / n;
            if (!dx && !dy)
                *detail = sqrt(fmin(varA, varB) / n);
            if (varA > 1e-6 && varB > 1e-6) {
                double r = cov / sqrt(varA * varB);
                if (r > best)
                    best = r;
            }
        }
    }
    return best;
}

/* A bright circle on black in the w x h block at x0 (dark corners outside
 * the inscribed circle, brighter inside). */
static int fisheyeHalf(const unsigned char* luma, int x0, int w, int h)
{
    double cornerSum = 0, insideSum = 0;
    int corners = 0, inside = 0, brightCorners = 0;
    for (int y = 0; y < h; y++) {
        double v = (y + 0.5) / h * 2 - 1;
        for (int x = 0; x < w; x++) {
            double u = (x + 0.5) / w * 2 - 1, r = sqrt(u * u + v * v);
            unsigned char value = luma[y * N + x0 + x];
            if (r > 1.08) {
                cornerSum += value;
                corners++;
                brightCorners += value > 40;
            } else if (r < 0.85) {
                insideSum += value;
                inside++;
            }
        }
    }
    if (!corners || !inside)
        return 0;
    double cornerMean = cornerSum / corners, insideMean = insideSum / inside;
    return cornerMean < 14 && brightCorners * 33 < corners && insideMean > cornerMean + 15;
}

/* a b | c d: does the step from b to c stand out from the steps beside it? */
static int breaks(int a, int b, int c, int d)
{
    int step = abs(c - b), beside = abs(b - a) > abs(d - c) ? abs(b - a) : abs(d - c);
    return step > 2 * beside + 6;
}

void vrlayout_add(VRLayoutStats* stats, const unsigned char* luma, int size)
{
    if (size != N)
        return;
    stats->frames++;
    static float a[N * N / 2], b[N * N / 2];
    double detailSBS, detailTB;
    residual(luma, N / 2, N, a);
    residual(luma + N / 2, N / 2, N, b);
    double sbs = bestCorrelation(a, b, N / 2, N, &detailSBS);
    residual(luma, N, N / 2, a);
    residual(luma + N * N / 2, N, N / 2, b);
    double tb = bestCorrelation(a, b, N, N / 2, &detailTB);

    /* Seams: the left and right edges (they join in a 360 picture) and the
     * centre line. A row breaks there when the step across is well over the
     * steps just beside it; the share of rows that break (poles left out:
     * every column looks alike there). Smooth rows count as joining, so a
     * flat picture with plain edges breaks in fewer rows: on the console
     * the 360 test world broke in 17% of rows, the flat 2:1 fractal zoom in
     * 34-99%. */
    int wrapBreaks = 0, centreBreaks = 0, rows = 0;
    for (int y = N / 8; y < N - N / 8; y++) {
        const unsigned char* row = luma + y * N;
        wrapBreaks += breaks(row[N - 2], row[N - 1], row[0], row[1]);
        centreBreaks += breaks(row[N / 2 - 2], row[N / 2 - 1], row[N / 2], row[N / 2 + 1]);
        rows++;
    }
    if (fisheyeHalf(luma, 0, N / 2, N) && fisheyeHalf(luma, N / 2, N / 2, N))
        stats->fisheyeSBS++;
    double detail = fmin(detailSBS, detailTB);
    if (detail < MIN_DETAIL)
        return;
    stats->detailed++;
    stats->sbs += sbs;
    stats->tb += tb;
    stats->wrap += (double)wrapBreaks / rows;
    stats->centre += (double)centreBreaks / rows;
    stats->detail += detail;
}

static int near(double value, double target)
{
    return target > 0 && fabs(value / target - 1) < 0.04;
}

const char* vrlayout_shape(int width, int height, char* out, unsigned outSize)
{
    static const struct { int w, h; } shapes[] = { { 2, 1 }, { 1, 1 }, { 1, 2 }, { 4, 1 }, { 16, 9 }, { 4, 3 }, { 32, 9 }, { 9, 16 } };
    if (width <= 0 || height <= 0) {
        snprintf(out, outSize, "size unknown");
        return out;
    }
    double aspect = (double)width / height;
    for (unsigned i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
        if (fabs(aspect / ((double)shapes[i].w / shapes[i].h) - 1) < 0.02) {
            snprintf(out, outSize, "%d:%d", shapes[i].w, shapes[i].h);
            return out;
        }
    }
    snprintf(out, outSize, "%dx%d", width, height);
    return out;
}

void vrlayout_aspect_key(int width, int height, char* out, unsigned outSize)
{
    int key = width > 0 && height > 0 ? (int)((double)width / height * 20 + 0.5) * 5 : 0;
    snprintf(out, outSize, "%d", key);
}

/* Which way a frame of this shape would be split for stereo: 2:1 and 4:1
 * side by side (square or 2:1 eyes: 180 or fisheye, 360), 1:1 and 1:2 top
 * and bottom (2:1 or square eyes: 360, 180); other shapes are flat video. */
static int splitFor(double aspect)
{
    return near(aspect, 2) || near(aspect, 4) ? 1 : near(aspect, 1) || near(aspect, 0.5) ? 2 : 0;
}

void vrlayout_classify(const VRLayoutStats* stats, int width, int height, VRLayoutGuess* guess)
{
    char shape[32];
    vrlayout_shape(width, height, shape, sizeof shape);
    double aspect = width > 0 && height > 0 ? (double)width / height : 0;
    memset(guess, 0, sizeof *guess);
    guess->fisheye = 180;
    guess->stereo = -1;
    int split = splitFor(aspect);
    if (width > 0 && height > 0 && !split) {
        guess->projection = VRLAYOUT_FLAT;
        guess->conclusive = 1;
        snprintf(guess->why, sizeof guess->why, "%s frame: not a VR shape", shape);
        return;
    }
    int n = stats ? stats->detailed : 0;
    char note[64] = "";
    if (n > 0 && split) {
        double sbs = stats->sbs / n, tb = stats->tb / n, wrap = stats->wrap / n;
        double score = split == 1 ? sbs : tb, other = split == 1 ? tb : sbs;
        const char* halves = split == 1 ? "halves" : "top/bottom";
        if (score >= MATCH && other < MATCH) {
            guess->conclusive = 1;
            guess->stereo = split;
            if (split == 1 && near(aspect, 2) && stats->fisheyeSBS * 2 > stats->frames) {
                guess->projection = VRLAYOUT_FISHEYE_SBS;
                guess->fisheye = 190; /* the circle doesn't tell the lens; 190 is common */
                snprintf(guess->why, sizeof guess->why, "%s frame, fisheye circles, halves match %.2f", shape, score);
                return;
            }
            guess->projection = split == 1 ? (near(aspect, 2) ? VRLAYOUT_180_SBS : VRLAYOUT_360_SBS)
                                           : (near(aspect, 1) ? VRLAYOUT_360_TB : VRLAYOUT_180_TB);
            snprintf(guess->why, sizeof guess->why, "%s frame, %s match %.2f", shape, halves, score);
            return;
        }
        if (score < UNRELATED) {
            /* One picture: on a 2:1 frame a 360 one if its edges join. */
            int joins = wrap < WRAP_JOINS;
            guess->conclusive = 1;
            guess->stereo = 0;
            if (near(aspect, 2)) {
                guess->projection = joins ? VRLAYOUT_360 : VRLAYOUT_FLAT;
                snprintf(guess->why, sizeof guess->why, "%s frame, halves differ %.2f, edges %s (%.0f%% break)", shape, score,
                    joins ? "join" : "don't join", wrap * 100);
            } else {
                guess->projection = VRLAYOUT_FLAT;
                snprintf(guess->why, sizeof guess->why, "%s frame, %s differ %.2f", shape, halves, score);
            }
            return;
        }
        snprintf(note, sizeof note, ", %s unclear %.2f", halves, score);
        if (score >= MATCH)
            snprintf(note, sizeof note, ", both splits match %.2f/%.2f", sbs, tb);
    } else if (stats && stats->frames)
        snprintf(note, sizeof note, ", too little detail to compare");
    else if (stats)
        snprintf(note, sizeof note, ", no picture to compare");

    /* The frame's shape alone: what VR video libraries mostly use. */
    if (near(aspect, 2) && width >= 1280)
        guess->projection = VRLAYOUT_180_SBS;
    else if (near(aspect, 1) && width >= 1440)
        guess->projection = VRLAYOUT_360_TB;
    else if (near(aspect, 0.5) && height >= 1440)
        guess->projection = VRLAYOUT_180_TB;
    else
        guess->projection = VRLAYOUT_FLAT;
    snprintf(guess->why, sizeof guess->why, "%s frame%s%s", shape, note,
        guess->projection == VRLAYOUT_FLAT ? "" : ": the usual layout for that shape");
}
