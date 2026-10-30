/*
 * View in VR's layout guess (apps/title/vrlayout.c) on the Mac: reads
 * thumbnails as the title makes them (128x128 RGB, each cell the average of
 * its part of the picture; run.sh has FFmpeg make them from test clips),
 * turns them into luma with the title's weights and prints the guess.
 *
 *   vrlayout-test <width> <height> <expected projection|-> < frames.rgb
 *   vrlayout-test <width> <height> <expected|-> taps < full-size frames.rgb
 *
 * "taps": the input is whole frames (width x height RGB) and the thumbnail
 * is made the way the title's shader makes it (8 x 8 bilinear samples
 * spread over each cell) instead of FFmpeg's exact area average.
 *
 * Exit status 1 when the guess is not the expected projection.
 */
#include "vrlayout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <width> <height> <expected|-> < frames.rgb\n", argv[0]);
        return 2;
    }
    int width = atoi(argv[1]), height = atoi(argv[2]);
    enum { N = VRLAYOUT_SIZE };
    static unsigned char rgb[N * N * 3], luma[N * N];
    VRLayoutStats stats;
    memset(&stats, 0, sizeof stats);
    int taps = argc > 4 && !strcmp(argv[4], "taps");
    size_t frameBytes = taps ? (size_t)width * height * 3 : sizeof rgb;
    unsigned char* frame = taps ? malloc(frameBytes) : rgb;
    while (frame && fread(frame, 1, frameBytes, stdin) == frameBytes) {
        if (taps) {
            /* PS5RendererVR.cpp's thumbnail shader: texture() with GL_LINEAR,
             * clamp to edge, at 8 x 8 points of each cell. */
            for (int cy = 0; cy < N; cy++) {
                for (int cx = 0; cx < N; cx++) {
                    double sum[3] = { 0, 0, 0 };
                    for (int j = 0; j < 8; j++) {
                        for (int i = 0; i < 8; i++) {
                            double u = (cx + (i + 0.5) / 8) / N * width - 0.5, v = (cy + (j + 0.5) / 8) / N * height - 0.5;
                            int x0 = (int)u, y0 = (int)v;
                            double fx = u - x0, fy = v - y0;
                            for (int c = 0; c < 3; c++) {
#define PX(x, y) frame[((size_t)((y) < 0 ? 0 : (y) >= height ? height - 1 : (y)) * width + ((x) < 0 ? 0 : (x) >= width ? width - 1 : (x))) * 3 + c]
                                sum[c] += (1 - fy) * ((1 - fx) * PX(x0, y0) + fx * PX(x0 + 1, y0)) + fy * ((1 - fx) * PX(x0, y0 + 1) + fx * PX(x0 + 1, y0 + 1));
                            }
                        }
                    }
                    for (int c = 0; c < 3; c++)
                        rgb[(cy * N + cx) * 3 + c] = (unsigned char)(sum[c] / 64 + 0.5);
                }
            }
        }
        /* The title's thumbnail shader: dot(rgb, (0.2126, 0.7152, 0.0722)). */
        for (int i = 0; i < N * N; i++)
            luma[i] = (unsigned char)(0.2126 * rgb[3 * i] + 0.7152 * rgb[3 * i + 1] + 0.0722 * rgb[3 * i + 2] + 0.5);
        vrlayout_add(&stats, luma, N);
    }
    VRLayoutGuess guess;
    vrlayout_classify(stats.frames ? &stats : NULL, width, height, &guess);
    int d = stats.detailed ? stats.detailed : 1;
    printf("%d %s (%s) [frames %d, detailed %d: sbs %.2f tb %.2f wrap %.2f centre %.2f detail %.1f fisheye %d]\n", guess.projection,
        guess.conclusive ? "conclusive" : "shape only", guess.why, stats.frames, stats.detailed, stats.sbs / d, stats.tb / d,
        stats.wrap / d, stats.centre / d, stats.detail / d, stats.fisheyeSBS);
    return strcmp(argv[3], "-") && atoi(argv[3]) != guess.projection;
}
