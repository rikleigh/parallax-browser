/* backend.h - the device side of libopenxr_psvr2.
 *
 * The OpenXR layer (handles, state machine, spaces, actions, validation) is
 * device independent; a backend supplies timing, poses, controller state,
 * swapchain images and frame submission. Two exist:
 *   backend_mock.c   host / headless: synthetic head motion, 60 Hz clock,
 *                    fake GL texture names, no GL.
 *   backend_psvr2.c  PS5: common/psvr2 (Hmd2 reprojection + VrTracker2) and
 *                    GL textures in the app's EGL context.
 *
 * All times are XrTime (ns) in the backend's clock. Poses are in the
 * backend's tracking space (metres, Y up). All calls are made with the
 * runtime lock held, from the thread that called into OpenXR.
 */
#pragma once

#include <stdint.h>
#include <openxr/openxr.h>

#include "openxr_psvr2.h"

typedef struct {
    const char *system_name;
    uint32_t vendor_id;
    uint32_t rec_w, rec_h;     /* recommended per-view image size */
    uint32_t max_w, max_h;     /* per view */
    float tan[2][4];           /* render FOV tangents per eye: left, right, up, down (> 0) */
    int64_t frame_period_ns;   /* the app's frame interval */
    int floor_known;           /* floor_y is the floor in tracking space */
    float floor_y;
    float default_eye_height;  /* used when floor_known == 0 */
    int position_tracking;
} rt_backend_info;

typedef struct {
    XrTime display_time;       /* predicted mid-photon time of this frame */
    XrDuration period;
    uint64_t frame;
} rt_frame_t;

typedef struct {
    XrPosef head;
    XrPosef eye[2];            /* tracking space */
    int orientation_valid, position_valid;
    int orientation_tracked, position_tracked;
} rt_views_t;

/* State of one hand's controller, in simple_controller terms (a PS VR2 Sense
 * controller maps trigger -> select, options -> menu). */
typedef struct {
    int active;                /* connected and tracked */
    XrPosef grip, aim;         /* tracking space */
    int select;                /* 0/1 */
    int menu;
    float trigger;             /* 0..1, for float actions bound to select */
} rt_controller_t;

/* What xrEndFrame hands over: the first projection layer, mapped per eye. */
typedef struct {
    void *sc_priv;             /* backend swapchain object */
    uint64_t image;            /* GL texture name of the last released image */
    uint32_t sc_w, sc_h;
    int64_t format;
    XrRect2Di rect;
    float tan[4];              /* left, right, up, down of the FOV the eye was rendered with */
    XrPosef pose;              /* tracking space */
} rt_submit_view_t;

typedef struct {
    int has_projection;
    rt_layout_t layout;
    rt_submit_view_t view[2];
    XrTime display_time;
} rt_submit_t;

typedef struct rt_backend_ops {
    const char *name;
    /* xrGetSystem: open the device. 0 = ok. */
    int (*open)(rt_backend_info *info);
    void (*close)(void);
    /* xrCreateSession: binding may have NULL handles (the mock accepts it). */
    XrResult (*session_create)(const XrGraphicsBindingEGLMNDX *binding);
    void (*session_destroy)(void);
    /* xrBeginSession / xrEndSession */
    XrResult (*session_begin)(void);
    void (*session_end)(void);
    XrTime (*now)(void);
    /* Block until the next frame slot. Called without the runtime lock. */
    int (*wait_frame)(rt_frame_t *f);
    void (*locate_views)(XrTime t, rt_views_t *v);
    void (*controller)(int hand, XrTime t, rt_controller_t *c);
    int (*user_present)(void);
    /* Swapchain images: fill images[0..count) with GL texture names. */
    XrResult (*swapchain_create)(const XrSwapchainCreateInfo *info, uint32_t count,
                                 uint64_t *images, void **priv);
    void (*swapchain_destroy)(void *priv, uint32_t count, const uint64_t *images);
    /* xrEndFrame. 0 = ok. */
    int (*submit)(const rt_submit_t *s, rt_stats_t *stats);
} rt_backend_ops;

extern const rt_backend_ops rt_backend_mock;
#ifdef XRRT_HAVE_PSVR2
extern const rt_backend_ops rt_backend_psvr2;
#endif
