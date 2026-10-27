/* xr_internal.h - libopenxr_psvr2 internals.
 *
 * A minimal OpenXR runtime: exactly what WebKit's OpenXR backend
 * (Source/WebKit/UIProcess/XR/openxr) calls, plus the rest of the 1.0 core
 * so that the official loader's dispatch table is complete. One instance,
 * one session, one global lock. Devices live behind backend.h.
 */
#pragma once

#define EGL_NO_PLATFORM_SPECIFIC_TYPES 1
#include <EGL/egl.h>

#define XR_USE_PLATFORM_EGL 1
#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <stdint.h>
#include <stddef.h>

#include "backend.h"

/* Core xr* entry points are exported by default so that WebKit can link the
 * runtime statically with no loader (PS5). Build with -DXRRT_EXPORT_CORE=0
 * to export only xrNegotiateLoaderRuntimeInterface/xrGetInstanceProcAddr. */
#ifndef XRRT_EXPORT_CORE
#define XRRT_EXPORT_CORE 1
#endif
#define XRRT_VISIBLE __attribute__((visibility("default")))
#if XRRT_EXPORT_CORE
#define XRRT_CORE XRRT_VISIBLE
#else
#define XRRT_CORE __attribute__((visibility("hidden")))
#endif

#define RT_RUNTIME_NAME "libopenxr_psvr2"
#define RT_RUNTIME_VERSION XR_MAKE_VERSION(0, 1, 0)
#define RT_SYSTEM_ID ((XrSystemId)1)
#define RT_VIEW_COUNT 2
#define RT_MAX_LAYERS 16
#define RT_MAX_EVENTS 32
#define RT_MAX_ACTION_BINDINGS 8
#define RT_MAX_ATTACHED_SETS 16
#define RT_MAX_SUBACTIONS 4
#define RT_MAX_SUGGESTED 128

/* -- logging ---------------------------------------------------------------- */
void rt_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define RT_ERR(...) rt_log(1, __VA_ARGS__)
#define RT_INFO(...) rt_log(2, __VA_ARGS__)
#define RT_DBG(...) rt_log(3, __VA_ARGS__)

/* -- lock --------------------------------------------------------------------- */
void rt_lock(void);
void rt_unlock(void);
static inline void rt_guard_release(int *unused)
{
    (void)unused;
    rt_unlock();
}
/* Holds the runtime lock until the end of the enclosing scope. */
#define RT_GUARD()                                                                                \
    rt_lock();                                                                                    \
    int rt_guard_ __attribute__((cleanup(rt_guard_release), unused)) = 0

/* -- handles -------------------------------------------------------------------- */
typedef enum {
    OBJ_FREE = 0,
    OBJ_INSTANCE,
    OBJ_SESSION,
    OBJ_SPACE,
    OBJ_SWAPCHAIN,
    OBJ_ACTION_SET,
    OBJ_ACTION,
    OBJ_HAND_TRACKER,
} obj_type_t;

/* Handles are generation-tagged slot indices, never pointers, so a stale or
 * garbage handle is rejected without dereferencing anything. */
uint64_t rt_handle_new(obj_type_t type, void *obj);
void *rt_handle_get(uint64_t handle, obj_type_t type);
void rt_handle_free(uint64_t handle);
#define RT_H(h) ((uint64_t)(uintptr_t)(h))

/* -- objects --------------------------------------------------------------------- */
typedef struct rt_instance rt_instance;
typedef struct rt_session rt_session;
typedef struct rt_action_set rt_action_set;
typedef struct rt_action rt_action;

enum { HAND_LEFT = 0, HAND_RIGHT = 1, HAND_COUNT = 2 };

/* Input sources of /interaction_profiles/khr/simple_controller. */
typedef enum {
    SRC_SELECT_CLICK = 0,
    SRC_MENU_CLICK,
    SRC_GRIP_POSE,
    SRC_AIM_POSE,
    SRC_HAPTIC,
    SRC_COUNT
} rt_source_t;

typedef struct {
    int hand;          /* HAND_LEFT / HAND_RIGHT */
    rt_source_t src;
    XrPath path;       /* the binding path as suggested */
} rt_binding_t;

typedef struct {
    XrBool32 active;
    XrBool32 changed;
    XrTime last_change;
    float value;       /* bool as 0/1, float as is */
    XrVector2f vec;
} rt_action_state_t;

struct rt_action {
    XrAction handle;
    rt_action_set *set;
    int alive;                    /* 0 once its set is destroyed (spaces may still use it) */
    char name[XR_MAX_ACTION_NAME_SIZE];
    char localized[XR_MAX_LOCALIZED_ACTION_NAME_SIZE];
    XrActionType type;
    uint32_t n_sub;
    XrPath sub[RT_MAX_SUBACTIONS];
    int sub_hand[RT_MAX_SUBACTIONS]; /* HAND_* or -1 (head/gamepad: never bound) */
    /* resolved at xrAttachSessionActionSets */
    uint32_t n_bind;
    rt_binding_t bind[RT_MAX_ACTION_BINDINGS];
    rt_action_state_t state[HAND_COUNT];
    rt_action *next_all;          /* instance-wide list, freed with the instance */
};

struct rt_action_set {
    XrActionSet handle;
    rt_instance *inst;
    char name[XR_MAX_ACTION_SET_NAME_SIZE];
    char localized[XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE];
    uint32_t priority;
    int attached;
    rt_action_set *next;
};

typedef struct {
    XrPath profile;
    uint32_t count;
    XrActionSuggestedBinding *bindings;
} rt_suggestion_t;

typedef struct {
    XrEventDataBuffer ev;
    XrSession session;
} rt_event_t;

struct rt_instance {
    XrInstance handle;
    XrVersion api_version;
    char app_name[XR_MAX_APPLICATION_NAME_SIZE];
    struct {
        int gles, egl, local_floor, hand_tracking;
    } ext;
    int system_fetched;
    int gfx_requirements_queried;
    const rt_backend_ops *be;
    rt_backend_info info;
    int backend_open;
    /* paths: index + 1 is the XrPath */
    char **paths;
    uint32_t n_paths, cap_paths;
    /* event queue (ring) */
    rt_event_t events[RT_MAX_EVENTS];
    uint32_t ev_head, ev_count;
    int events_lost;
    rt_session *session;
    rt_action_set *sets;
    rt_action *actions;
    rt_suggestion_t suggested[RT_MAX_SUGGESTED];
    uint32_t n_suggested;
    /* well-known paths */
    XrPath p_hand[HAND_COUNT], p_head, p_gamepad, p_simple_profile;
};

/* -- spaces / swapchains ------------------------------------------------------------ */
typedef struct rt_space {
    XrSpace handle;
    rt_session *sess;
    int is_action;
    XrReferenceSpaceType ref;
    rt_action *action;
    int hand; /* -1 = any */
    XrPosef offset;
    struct rt_space *next;
} rt_space;

#define RT_MAX_IMAGES 4
typedef struct rt_swapchain {
    XrSwapchain handle;
    rt_session *sess;
    XrSwapchainCreateInfo info;
    uint32_t n_images;
    uint64_t images[RT_MAX_IMAGES]; /* GL texture names */
    void *be_priv;
    /* FIFO acquire/wait/release bookkeeping */
    uint32_t next_acquire;
    uint32_t acquired[RT_MAX_IMAGES]; /* queue of acquired indices */
    uint32_t n_acquired;
    uint32_t n_waited;                /* acquired[0..n_waited) have been waited */
    int64_t last_released;            /* -1 = never */
    struct rt_swapchain *next;
} rt_swapchain;

struct rt_session {
    XrSession handle;
    rt_instance *inst;
    XrSessionState state;
    int running;
    int exit_requested;
    int lost;
    XrViewConfigurationType view_config;
    /* frame loop */
    uint32_t waits_pending;   /* xrWaitFrame calls not yet consumed by xrBeginFrame */
    int frame_begun;
    int frame_ended_once;
    XrTime last_display_time;
    XrDuration display_period;
    /* LOCAL origin in tracking space, set at the first tracked locate */
    int origin_set;
    XrPosef origin;
    /* objects */
    rt_space *spaces;
    rt_swapchain *swapchains;
    /* input */
    int sets_attached;
    rt_action_set *attached[RT_MAX_ATTACHED_SETS];
    uint32_t n_attached;
    XrPath current_profile[HAND_COUNT];
    XrTime last_sync_time;
    /* statistics for the smoke test / logs */
    rt_stats_t stats;
};

/* -- helpers shared across files ----------------------------------------------------- */
XrResult rt_path_intern(rt_instance *inst, const char *s, XrPath *out);
const char *rt_path_str(rt_instance *inst, XrPath p);
int rt_path_is_valid(rt_instance *inst, XrPath p);

void rt_event_push(rt_instance *inst, const XrEventDataBuffer *ev, XrSession session);
void rt_event_drop_session(rt_instance *inst, XrSession session);
void rt_session_set_state(rt_session *s, XrSessionState state);

rt_instance *rt_get_instance(XrInstance h);
rt_session *rt_get_session(XrSession h);

void rt_session_destroy_locked(rt_session *s);
void rt_swapchain_destroy_locked(rt_swapchain *sc);
void rt_space_destroy_locked(rt_space *sp);
void rt_action_set_destroy_locked(rt_action_set *set);

/* tracking-space pose of a space at time t; returns location flags */
XrSpaceLocationFlags rt_space_pose(rt_session *s, rt_space *sp, XrTime t, XrPosef *out);
void rt_input_attach_resolve(rt_session *s);
void rt_input_sync_profiles(rt_session *s);
int rt_hand_of_path(rt_instance *inst, XrPath p);

static inline XrBool32 rt_pose_ok(const XrPosef *p)
{
    const float n = p->orientation.x * p->orientation.x + p->orientation.y * p->orientation.y +
                    p->orientation.z * p->orientation.z + p->orientation.w * p->orientation.w;
    return n > 0.98f && n < 1.02f;
}

/* Two-call idiom: writes the needed count, then reports whether the caller
 * may copy (1), should just return (0, count-only query), or failed. */
static inline XrResult rt_two_call(uint32_t capacity, uint32_t *count_out, uint32_t needed,
                                   const void *array, int *copy)
{
    *copy = 0;
    if (!count_out)
        return XR_ERROR_VALIDATION_FAILURE;
    *count_out = needed;
    if (capacity == 0)
        return XR_SUCCESS;
    if (capacity < needed)
        return XR_ERROR_SIZE_INSUFFICIENT;
    if (!array)
        return XR_ERROR_VALIDATION_FAILURE;
    *copy = 1;
    return XR_SUCCESS;
}

/* Extension entry points (not exported, reached via xrGetInstanceProcAddr). */
XrResult rt_xrGetOpenGLESGraphicsRequirementsKHR(XrInstance, XrSystemId,
                                                 XrGraphicsRequirementsOpenGLESKHR *);
XrResult rt_xrCreateHandTrackerEXT(XrSession, const XrHandTrackerCreateInfoEXT *, XrHandTrackerEXT *);
XrResult rt_xrDestroyHandTrackerEXT(XrHandTrackerEXT);
XrResult rt_xrLocateHandJointsEXT(XrHandTrackerEXT, const XrHandJointsLocateInfoEXT *,
                                  XrHandJointLocationsEXT *);
