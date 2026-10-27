/* openxr_psvr2.h - public, non-standard bits of libopenxr_psvr2.
 *
 * Everything an application needs beyond <openxr/openxr.h>:
 *  - xrGetRuntimeStatsPSVR2, reached with
 *      xrGetInstanceProcAddr(instance, "xrGetRuntimeStatsPSVR2", ...)
 *    (frame/submit counters; the smoke test checks them).
 *  - xr_psvr2_preload(): on the PS5, call it first thing in main() of the
 *    process that will run OpenXR (WebKit's UI process). libSceHmd2 and
 *    libSceVrTracker2 must be loaded while the sandbox library path still
 *    resolves (see common/psvr2/psvr2.h). A no-op in the host build.
 */
#pragma once

#include <stdint.h>
#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RT_LAYOUT_NONE = 0,
    RT_LAYOUT_SBS_EXACT,      /* one swapchain, left/right halves, same size as the eye buffer */
    RT_LAYOUT_SBS_SCALED,     /* one swapchain, left/right halves, other size: scaled blit */
    RT_LAYOUT_TWO_SWAPCHAINS, /* one swapchain per eye */
    RT_LAYOUT_OTHER,          /* any other rects (texture array slices are not supported) */
} rt_layout_t;

typedef struct {
    uint64_t frames_waited;
    uint64_t frames_begun;
    uint64_t frames_discarded;
    uint64_t frames_ended;
    uint64_t frames_submitted;   /* xrEndFrame with a projection layer handed to the backend */
    uint64_t frames_empty;       /* xrEndFrame with no projection layer */
    uint64_t layers_ignored;     /* quad / extra projection layers (not composited) */
    uint64_t backend_errors;
    rt_layout_t last_layout;
    uint32_t last_src_w, last_src_h; /* per-eye source rect */
    uint32_t eye_w, eye_h;           /* backend eye buffer */
} rt_stats_t;

typedef XrResult(XRAPI_PTR *PFN_xrGetRuntimeStatsPSVR2)(XrSession session, rt_stats_t *stats);

int xr_psvr2_preload(void);

#ifdef __cplusplus
}
#endif
