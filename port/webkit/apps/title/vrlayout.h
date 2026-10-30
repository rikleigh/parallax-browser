/*
 * View in VR: the frame-layout guess from the video's own pictures (plain
 * C, no GLib, so test/vrlayout builds it on the Mac too).
 *
 * The render thread hands over small luma thumbnails of the playing video
 * (VRLAYOUT_SIZE x VRLAYOUT_SIZE cells, each the average of its part of the
 * picture, row 0 = top). Per thumbnail: how well the left and right halves
 * match (side-by-side stereo: the same scene, shifted by a little
 * disparity) and the top and bottom halves (top/bottom stereo), whether the
 * left and right edges join up (a 360 equirectangular picture wraps), and
 * whether each half is a circle on black (fisheye). vrlayout_classify()
 * turns those and the frame's size into a projection, with the reason.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define VRLAYOUT_SIZE 128

/* As WPEDisplayPS5VRProjection (WPEDisplayPS5.h). */
enum {
    VRLAYOUT_FLAT = 0,
    VRLAYOUT_360,
    VRLAYOUT_360_SBS,
    VRLAYOUT_360_TB,
    VRLAYOUT_180,
    VRLAYOUT_180_SBS,
    VRLAYOUT_180_TB,
    VRLAYOUT_FISHEYE_SBS,
};

typedef struct {
    int frames;   /* thumbnails seen */
    int detailed; /* of them, with enough detail to compare halves */
    /* Sums over the detailed thumbnails (vrlayout_classify averages). */
    double sbs;      /* best correlation of the left and right halves, -1..1 */
    double tb;       /* best correlation of the top and bottom halves */
    double wrap;     /* share of rows whose left and right edges don't join (0 in a 360 picture) */
    double centre;   /* the same across the vertical centre line */
    double detail;   /* residual standard deviation of the halves (luma, 0..255) */
    int fisheyeSBS;  /* thumbnails whose halves are both a bright circle with black corners */
} VRLayoutStats;

typedef struct {
    int projection;    /* VRLAYOUT_* */
    double fisheye;    /* field of view for VRLAYOUT_FISHEYE_SBS */
    int conclusive;    /* 1: the pictures decided; 0: the frame's shape alone (or nothing) */
    int stereo;        /* 1 side by side, 2 top/bottom, 0 halves unrelated, -1 unknown */
    char why[128];     /* e.g. "2:1 frame, halves match 0.93" */
} VRLayoutGuess;

/* Adds one thumbnail (size x size bytes, row 0 = top). */
void vrlayout_add(VRLayoutStats* stats, const unsigned char* luma, int size);

/* The guess for a width x height video from the thumbnails so far (stats
 * may be NULL or empty: the frame's shape alone). */
void vrlayout_classify(const VRLayoutStats* stats, int width, int height, VRLayoutGuess* guess);

/* "2:1", "1:1", "1:2", "4:1", "16:9" or "WxH": the frame's shape. */
const char* vrlayout_shape(int width, int height, char* out, unsigned outSize);

/* The shape as a short key for remembering choices (e.g. "200" for 2:1,
 * the aspect ratio x 100, rounded to 5); "0" when unknown. */
void vrlayout_aspect_key(int width, int height, char* out, unsigned outSize);

#ifdef __cplusplus
}
#endif
