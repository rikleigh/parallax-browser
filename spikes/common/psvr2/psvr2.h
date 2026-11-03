/* psvr2 - a minimal PS VR2 output backend for the ps5webkit spikes.
 *
 * Drives the headset through the system's libSceHmd2 / libSceVrTracker2 the
 * way PS5VR does (https://github.com/kavera001-dev/PS5VR, GPL-3.0-or-later;
 * this file is derived from its app/src/app_vr.c): Hmd2 reprojection at
 * 120 Hz with half-rate rendering, one type 0x10 layer per frame whose two
 * textures are the same side-by-side eye buffer.
 *
 * The caller owns the eye buffers. Each is described by a 32-byte GFX10
 * texture descriptor (T#) over GPU-visible memory; how that memory gets its
 * pixels (an AGC draw, a GL texture exported zero-copy, a CPU copy) is the
 * question Spike 3 answers.
 *
 * Firmware: functions the libraries do not export by name are reached at
 * per-firmware offsets (exports/vr_exports_<fw>.h), each guarded by an anchor
 * string, so a wrong table fails safely instead of calling into the void.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSVR2_MAX_BUFFERS 3

typedef struct {
    float pos[3];  /* metres, tracking space */
    float quat[4]; /* x, y, z, w */
} psvr2_pose_t;

typedef struct {
    uint64_t frame;
    uint64_t display_time; /* predicted, process time in us */
    int tracked;
    psvr2_pose_t head;
    psvr2_pose_t eye[2];   /* 0 left, 1 right; see psvr2.c on positions */
    int eye_pos_valid;     /* eye positions came from the tracker */
} psvr2_frame_t;

typedef void (*psvr2_log_fn)(const char *line);
void psvr2_set_logger(psvr2_log_fn fn);

/* Load and bind the libraries. In a title this must run early, while the
 * sandbox's /<random word>/common/lib path is still how they are reached. */
int psvr2_preload(void);
/* Hmd2 + tracker + reprojection for the given user. A failure undoes what
 * was set up; psvr2_close releases everything, its direct memory included,
 * and open can be called again afterwards. */
int psvr2_open(int user_id);
/* psvr2_open's result when psvr2_require_headset(1) is set and Hmd2 reports
 * no headset (device status not 0: 2 with the PS VR2 unplugged, 0 with it
 * connected, worn or not). Nothing is left open. */
#define PSVR2_NO_HEADSET (-3)
/* 1: psvr2_open refuses (PSVR2_NO_HEADSET) before the tracker, the
 * reprojection and VR mode when no headset is connected. Default 0 (the
 * spikes and vr-probe open Hmd2 without a headset). */
void psvr2_require_headset(int on);
/* Hmd2's device information, live while open (else what the last open saw):
 * status (0 = connected), worn flag, lens distance in mm. Returns the call's
 * result (0 = read). */
int psvr2_device_status(uint32_t *status, int *worn, int *lens_mm);
/* Hmd2 only, opened and closed again (no tracker, no VR mode: nothing shows
 * in the headset): 0 if the headset could be opened, with its device status
 * and worn flag; else the error. */
int psvr2_probe(int user_id, uint32_t *status, int *worn);
/* FOV tangents of one eye as the headset reports them: out, in, up, down. */
void psvr2_fov(float tan[4]);
/* Tangents to render and submit for eye 0/1 (left, right, up, down), with
 * the overscan margin the reprojection needs. */
void psvr2_eye_tangents(int eye, float overscan, float tan[4]);
/* 1 while the headset is on the head. */
int psvr2_worn(void);

/* Enter VR mode with `count` side-by-side eye buffers of (2*eye_w) x eye_h. */
int psvr2_start(const uint32_t tsharp[][8], int count, uint32_t eye_w, uint32_t eye_h);
/* Wait for the next frame slot and fetch the pose to render it with. */
int psvr2_begin_frame(psvr2_frame_t *f);
/* Hand buffer `index` (rendered with f's pose and the given tangents) to
 * the compositor. The GPU work writing it must be complete. */
int psvr2_submit(int index, const psvr2_frame_t *f, const float tan[2][4]);
void psvr2_stop(void);
void psvr2_close(void);
/* Asks ShellUI to close the title (as the PS menu's Close does); 0 when asked.
 * The process is ended shortly after, so wait rather than return from main. */
int psvr2_exit_app(void);

/* GFX10 T# for an RGBA8 2D texture. sw_mode 0 = linear (pitch in bytes,
 * 256-byte aligned base), 27 = SW_64KB_R_X (what PS5VR and Sony titles hand
 * Hmd2; 64 KB aligned base, pitch implied). */
int psvr2_build_tsharp_rgba8(uint32_t out[8], uint64_t gpu_address, uint32_t width,
                             uint32_t height, uint32_t pitch_bytes, uint32_t sw_mode);

/* GPU-visible memory the compositor can read (direct memory, WB onion). */
void *psvr2_direct_alloc(uint64_t size, uint64_t align);

#ifdef __cplusplus
}
#endif
