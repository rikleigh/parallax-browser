/* xr_smoke.c - Spike 5 host test: replay WebKit's OpenXR call sequence
 * against libopenxr_psvr2 (mock backend).
 *
 * The sequence mirrors Source/WebKit/UIProcess/XR/openxr:
 *   OpenXRExtensions()            xrEnumerateInstanceExtensionProperties (two-call)
 *   createInstance()              MNDX_egl_enable + KHR_opengl_es_enable (+ EXT_hand_tracking)
 *   loadMethods()                 xrGetInstanceProcAddr for the extension functions
 *   initializeSystem()            xrGetSystem(HMD), systemProperties() w/ hand-tracking chain
 *   collectViewConfigurations()   xrEnumerateViewConfigurations / Views
 *   initializeBlendModes()        xrEnumerateEnvironmentBlendModes
 *   createSessionIfNeeded()       xrGetOpenGLESGraphicsRequirementsKHR, xrCreateSession(EGLMNDX)
 *   OpenXRInput::create()         per hand: action set, grip/aim + spaces, 7x3 button
 *                                 actions, 2 axis actions; simple_controller bindings; attach
 *   waitForSessionReady()         xrPollEvent until READY -> xrBeginSession
 *   createLayerProjection()       xrEnumerateSwapchainFormats, xrCreateSwapchain(2*w x h, RGBA8),
 *                                 xrEnumerateSwapchainImages
 *   beginFrame()/populateFrameData()/endFrame()
 *                                 pollEvents, xrWaitFrame, xrBeginFrame, reference spaces
 *                                 (VIEW, LOCAL, LOCAL_FLOOR or STAGE-derived), xrLocateViews x2,
 *                                 xrSyncActions, action states, xrLocateSpace, acquire/wait,
 *                                 release, xrEndFrame(projection layer, SBS imageRects)
 *   endSessionIfExists()          xrRequestExitSession, poll: STOPPING -> xrEndSession,
 *                                 EXITING -> destroy everything
 *
 * Build modes: default dlopen()s the dylib and negotiates like the loader;
 * -DXR_SMOKE_STATIC links the static library and uses the exported xr*
 * symbols directly (the PS5 configuration); -DXR_SMOKE_LOADER links the
 * official libopenxr_loader (runtime found via XR_RUNTIME_JSON).
 */
#define SPIKE_NAME "05-openxr-runtime"
#include "../../common/spike.h"

#define EGL_NO_PLATFORM_SPECIFIC_TYPES 1
#include <EGL/egl.h>
#define XR_USE_PLATFORM_EGL 1
#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include "../runtime/openxr_psvr2.h"
#include "../runtime/xr_math.h"

#include <dlfcn.h>
#include <math.h>
#include <stdlib.h>
#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>
#endif

#if defined(XR_SMOKE_STATIC)
#define MODE "static"
XrResult xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo *, XrNegotiateRuntimeRequest *);
#elif defined(XR_SMOKE_LOADER)
#define MODE "loader"
#else
#define MODE "dlopen"
#endif

#define GL_RGBA8 0x8058
#define GL_SRGB8_ALPHA8 0x8C43

/* -- function table ---------------------------------------------------------------- */
#define XR_GLOBAL_FUNCS(X)                                                                         \
    X(xrEnumerateInstanceExtensionProperties)                                                      \
    X(xrCreateInstance)
#define XR_INSTANCE_FUNCS(X)                                                                       \
    X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrGetSystem) X(xrGetSystemProperties)       \
    X(xrEnumerateViewConfigurations) X(xrEnumerateViewConfigurationViews)                          \
    X(xrEnumerateEnvironmentBlendModes) X(xrEnumerateReferenceSpaces) X(xrCreateReferenceSpace)    \
    X(xrCreateSession) X(xrBeginSession) X(xrEndSession) X(xrRequestExitSession)                   \
    X(xrDestroySession) X(xrPollEvent) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame)                \
    X(xrLocateViews) X(xrLocateSpace) X(xrDestroySpace) X(xrEnumerateSwapchainFormats)             \
    X(xrCreateSwapchain) X(xrDestroySwapchain) X(xrEnumerateSwapchainImages)                       \
    X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage)                  \
    X(xrStringToPath) X(xrPathToString) X(xrCreateActionSet) X(xrDestroyActionSet)                 \
    X(xrCreateAction) X(xrCreateActionSpace) X(xrSuggestInteractionProfileBindings)                \
    X(xrAttachSessionActionSets) X(xrSyncActions) X(xrGetActionStateBoolean)                       \
    X(xrGetActionStateFloat) X(xrGetActionStateVector2f) X(xrGetCurrentInteractionProfile)         \
    X(xrResultToString)

#define DECL(n) static PFN_##n p_##n;
XR_GLOBAL_FUNCS(DECL)
XR_INSTANCE_FUNCS(DECL)
static PFN_xrGetInstanceProcAddr p_gipa;
static PFN_xrGetOpenGLESGraphicsRequirementsKHR p_gles_req;
static PFN_xrCreateHandTrackerEXT p_create_hand;
static PFN_xrDestroyHandTrackerEXT p_destroy_hand;
static PFN_xrLocateHandJointsEXT p_locate_hand;
static PFN_xrGetRuntimeStatsPSVR2 p_stats;

static int load_funcs(XrInstance inst, int global)
{
    int ok = 1;
#define LOAD(n)                                                                                    \
    if (XR_FAILED(p_gipa(inst, #n, (PFN_xrVoidFunction *)&p_##n)) || !p_##n) {                    \
        spike_log("missing %s", #n);                                                               \
        ok = 0;                                                                                    \
    }
    if (global) {
        XR_GLOBAL_FUNCS(LOAD)
    } else {
        XR_INSTANCE_FUNCS(LOAD)
    }
#undef LOAD
#ifdef XR_SMOKE_STATIC
    /* PS5 configuration: WebKit calls the exported core symbols directly. */
    if (global) {
        p_xrCreateInstance = xrCreateInstance;
        p_xrEnumerateInstanceExtensionProperties = xrEnumerateInstanceExtensionProperties;
    } else {
        p_xrPollEvent = xrPollEvent;
        p_xrWaitFrame = xrWaitFrame;
        p_xrBeginFrame = xrBeginFrame;
        p_xrEndFrame = xrEndFrame;
        p_xrLocateViews = xrLocateViews;
        p_xrLocateSpace = xrLocateSpace;
        p_xrSyncActions = xrSyncActions;
    }
#endif
    return ok;
}

/* -- result bookkeeping -------------------------------------------------------------- */
static unsigned g_fail;
static char g_errs[512];

static const char *rs(XrResult r)
{
    static char b[XR_MAX_RESULT_STRING_SIZE];
    snprintf(b, sizeof b, "%d", (int)r);
    return b;
}

/* expect a specific result; collect mismatches for the "errors" RESULT */
static unsigned g_expect_n, g_expect_bad;
static void expect(const char *what, XrResult got, XrResult want)
{
    g_expect_n++;
    if (got == want)
        return;
    g_expect_bad++;
    spike_log("  expect %s: got %d, want %d", what, (int)got, (int)want);
    size_t n = strlen(g_errs);
    snprintf(g_errs + n, sizeof g_errs - n, "%s%s", n ? "," : "", what);
}

#define CHECK(call)                                                                                \
    do {                                                                                           \
        XrResult r_ = (call);                                                                      \
        if (XR_FAILED(r_)) {                                                                       \
            spike_log("%s:%d %s -> %s", __FILE__, __LINE__, #call, rs(r_));                         \
            return r_;                                                                             \
        }                                                                                          \
    } while (0)

#define T(T_, S_) ((T_){.type = (S_)})

/* -- the WebKit-shaped client ------------------------------------------------------------ */
enum { N_BUTTONS = 7, N_AXES = 2 };
static const char *const kButtons[N_BUTTONS] = {"trigger", "squeeze", "touchpad", "thumbstick",
                                                "thumbrest", "buttona", "buttonb"};
static const char *const kAxes[N_AXES] = {"touchpad", "thumbstick"};

typedef struct {
    XrPath sub;
    XrActionSet set;
    XrAction grip, aim;
    XrSpace grip_space, aim_space;
    XrAction press[N_BUTTONS], touch[N_BUTTONS], value[N_BUTTONS];
    XrAction axis[N_AXES];
    char profile[128];
} hand_t;

typedef struct {
    XrInstance inst;
    XrSystemId sys;
    XrSession sess;
    int has_hand_ext, has_floor_ext;
    XrViewConfigurationView vcv[2];
    XrSpace viewer, local, floor_space;
    const char *floor_path;
    XrSwapchain sc;
    uint32_t sc_w, sc_h, n_images, images[8];
    hand_t hand[2];
    XrSessionState state;
    int running, exited;
    XrSessionState seen[32];
    int n_seen;
    int profile_events;
    XrFrameState fs;
    XrView local_views[2];
} client_t;

static void fn_dummy(void)
{
}
static PFN_xrVoidFunction dummy_get_proc(const char *name)
{
    (void)name;
    return fn_dummy;
}

static XrResult create_action(hand_t *h, XrActionType type, const char *name, XrAction *out)
{
    XrActionCreateInfo ci = T(XrActionCreateInfo, XR_TYPE_ACTION_CREATE_INFO);
    ci.actionType = type;
    ci.countSubactionPaths = 1;
    ci.subactionPaths = &h->sub;
    snprintf(ci.actionName, sizeof ci.actionName, "%s", name);
    snprintf(ci.localizedActionName, sizeof ci.localizedActionName, "%s", name);
    return p_xrCreateAction(h->set, &ci, out);
}

static XrResult create_action_space(client_t *c, hand_t *h, XrAction a, XrSpace *out)
{
    XrActionSpaceCreateInfo ci = T(XrActionSpaceCreateInfo, XR_TYPE_ACTION_SPACE_CREATE_INFO);
    ci.action = a;
    ci.subactionPath = h->sub;
    ci.poseInActionSpace.orientation.w = 1;
    return p_xrCreateActionSpace(c->sess, &ci, out);
}

/* OpenXRInput::create + OpenXRInputSource::initialize/suggestBindings */
static XrResult input_create(client_t *c)
{
    XrActionSuggestedBinding b[16];
    uint32_t nb = 0;
    XrActionSet sets[2];
    for (int i = 0; i < 2; i++) {
        hand_t *h = &c->hand[i];
        const char *hn = i ? "right" : "left";
        char buf[128];
        snprintf(buf, sizeof buf, "/user/hand/%s", hn);
        CHECK(p_xrStringToPath(c->inst, buf, &h->sub));
        XrActionSetCreateInfo si = T(XrActionSetCreateInfo, XR_TYPE_ACTION_SET_CREATE_INFO);
        snprintf(si.actionSetName, sizeof si.actionSetName, "input_%s_action_set", hn);
        snprintf(si.localizedActionSetName, sizeof si.localizedActionSetName, "input_%s_action_set", hn);
        CHECK(p_xrCreateActionSet(c->inst, &si, &h->set));
        snprintf(buf, sizeof buf, "input_%s_grip", hn);
        CHECK(create_action(h, XR_ACTION_TYPE_POSE_INPUT, buf, &h->grip));
        CHECK(create_action_space(c, h, h->grip, &h->grip_space));
        snprintf(buf, sizeof buf, "input_%s_aim", hn);
        CHECK(create_action(h, XR_ACTION_TYPE_POSE_INPUT, buf, &h->aim));
        CHECK(create_action_space(c, h, h->aim, &h->aim_space));
        for (int k = 0; k < N_BUTTONS; k++) {
            snprintf(buf, sizeof buf, "input_%s_button_%s_press", hn, kButtons[k]);
            CHECK(create_action(h, XR_ACTION_TYPE_BOOLEAN_INPUT, buf, &h->press[k]));
            snprintf(buf, sizeof buf, "input_%s_button_%s_touch", hn, kButtons[k]);
            CHECK(create_action(h, XR_ACTION_TYPE_BOOLEAN_INPUT, buf, &h->touch[k]));
            snprintf(buf, sizeof buf, "input_%s_button_%s_value", hn, kButtons[k]);
            CHECK(create_action(h, XR_ACTION_TYPE_FLOAT_INPUT, buf, &h->value[k]));
        }
        for (int k = 0; k < N_AXES; k++) {
            snprintf(buf, sizeof buf, "input_%s_axis_%s", hn, kAxes[k]);
            CHECK(create_action(h, XR_ACTION_TYPE_VECTOR2F_INPUT, buf, &h->axis[k]));
        }
        /* simple_controller: grip, aim, trigger -> /input/select/click */
        XrPath p;
        snprintf(buf, sizeof buf, "/user/hand/%s/input/grip/pose", hn);
        CHECK(p_xrStringToPath(c->inst, buf, &p));
        b[nb++] = (XrActionSuggestedBinding){h->grip, p};
        snprintf(buf, sizeof buf, "/user/hand/%s/input/aim/pose", hn);
        CHECK(p_xrStringToPath(c->inst, buf, &p));
        b[nb++] = (XrActionSuggestedBinding){h->aim, p};
        snprintf(buf, sizeof buf, "/user/hand/%s/input/select/click", hn);
        CHECK(p_xrStringToPath(c->inst, buf, &p));
        b[nb++] = (XrActionSuggestedBinding){h->press[0], p};
        sets[i] = h->set;
    }
    XrInteractionProfileSuggestedBinding sb =
        T(XrInteractionProfileSuggestedBinding, XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING);
    CHECK(p_xrStringToPath(c->inst, "/interaction_profiles/khr/simple_controller", &sb.interactionProfile));
    sb.countSuggestedBindings = nb;
    sb.suggestedBindings = b;
    CHECK(p_xrSuggestInteractionProfileBindings(c->inst, &sb));
    XrSessionActionSetsAttachInfo ai = T(XrSessionActionSetsAttachInfo, XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO);
    ai.countActionSets = 2;
    ai.actionSets = sets;
    CHECK(p_xrAttachSessionActionSets(c->sess, &ai));
    return XR_SUCCESS;
}

/* OpenXRInputSource::updateInteractionProfile */
static void update_profiles(client_t *c)
{
    for (int i = 0; i < 2; i++) {
        XrInteractionProfileState st = T(XrInteractionProfileState, XR_TYPE_INTERACTION_PROFILE_STATE);
        c->hand[i].profile[0] = 0;
        if (XR_FAILED(p_xrGetCurrentInteractionProfile(c->sess, c->hand[i].sub, &st)))
            continue;
        uint32_t n = 0;
        if (XR_FAILED(p_xrPathToString(c->inst, st.interactionProfile, sizeof c->hand[i].profile, &n,
                                       c->hand[i].profile)))
            c->hand[i].profile[0] = 0;
    }
}

static void destroy_session_objects(client_t *c)
{
    /* cleanupSessionAndAssociatedResources: spaces, layers, input, session */
    if (c->viewer)
        p_xrDestroySpace(c->viewer);
    if (c->local)
        p_xrDestroySpace(c->local);
    if (c->floor_space)
        p_xrDestroySpace(c->floor_space);
    c->viewer = c->local = c->floor_space = XR_NULL_HANDLE;
    if (c->sc)
        p_xrDestroySwapchain(c->sc);
    c->sc = XR_NULL_HANDLE;
    for (int i = 0; i < 2; i++) { /* ~OpenXRInputSource */
        if (c->hand[i].set)
            p_xrDestroyActionSet(c->hand[i].set);
        if (c->hand[i].grip_space)
            p_xrDestroySpace(c->hand[i].grip_space);
        if (c->hand[i].aim_space)
            p_xrDestroySpace(c->hand[i].aim_space);
    }
    if (c->sess)
        p_xrDestroySession(c->sess);
    c->sess = XR_NULL_HANDLE;
    /* cleanupInstanceAndAssociatedResources */
    if (c->inst)
        p_xrDestroyInstance(c->inst);
    c->inst = XR_NULL_HANDLE;
}

/* OpenXRCoordinator::pollEvents + handleSessionStateChange. 0 = stop. */
static int poll_events(client_t *c)
{
    XrEventDataBuffer ev = T(XrEventDataBuffer, XR_TYPE_EVENT_DATA_BUFFER);
    while (c->inst && p_xrPollEvent(c->inst, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            return 0;
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const XrEventDataSessionStateChanged *e = (const XrEventDataSessionStateChanged *)&ev;
            c->state = e->state;
            if (c->n_seen < 32)
                c->seen[c->n_seen++] = e->state;
            if (e->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = T(XrSessionBeginInfo, XR_TYPE_SESSION_BEGIN_INFO);
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(p_xrBeginSession(c->sess, &bi)))
                    c->running = 1;
            } else if (e->state == XR_SESSION_STATE_STOPPING) {
                p_xrEndSession(c->sess);
                c->running = 0;
            } else if (e->state == XR_SESSION_STATE_EXITING || e->state == XR_SESSION_STATE_LOSS_PENDING) {
                destroy_session_objects(c);
                c->exited = 1;
            }
            return c->sess != XR_NULL_HANDLE;
        }
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
            c->profile_events++;
            update_profiles(c);
            break;
        default:
            break;
        }
        ev = T(XrEventDataBuffer, XR_TYPE_EVENT_DATA_BUFFER);
    }
    return 1;
}

static XrSpace create_ref_space(client_t *c, XrReferenceSpaceType t, XrPosef pose)
{
    XrReferenceSpaceCreateInfo ci = T(XrReferenceSpaceCreateInfo, XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
    ci.referenceSpaceType = t;
    ci.poseInReferenceSpace = pose;
    XrSpace s = XR_NULL_HANDLE;
    p_xrCreateReferenceSpace(c->sess, &ci, &s);
    return s;
}

/* createReferenceSpacesIfNeeded */
static XrResult reference_spaces(client_t *c)
{
    if (c->local)
        return XR_SUCCESS;
    uint32_t n = 0;
    CHECK(p_xrEnumerateReferenceSpaces(c->sess, 0, &n, NULL));
    XrReferenceSpaceType types[8];
    CHECK(p_xrEnumerateReferenceSpaces(c->sess, n, &n, types));
    int has_floor = 0, has_stage = 0;
    for (uint32_t i = 0; i < n; i++) {
        has_floor |= types[i] == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
        has_stage |= types[i] == XR_REFERENCE_SPACE_TYPE_STAGE;
    }
    c->viewer = create_ref_space(c, XR_REFERENCE_SPACE_TYPE_VIEW, pose_identity());
    c->local = create_ref_space(c, XR_REFERENCE_SPACE_TYPE_LOCAL, pose_identity());
    if (has_floor && c->has_floor_ext) {
        c->floor_space = create_ref_space(c, XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT, pose_identity());
        c->floor_path = "LOCAL_FLOOR";
        return XR_SUCCESS;
    }
    if (!has_stage)
        return XR_SUCCESS;
    XrSpace stage = create_ref_space(c, XR_REFERENCE_SPACE_TYPE_STAGE, pose_identity());
    XrSpaceLocation loc = T(XrSpaceLocation, XR_TYPE_SPACE_LOCATION);
    CHECK(p_xrLocateSpace(stage, c->local, c->fs.predictedDisplayTime, &loc));
    CHECK(p_xrDestroySpace(stage));
    XrPosef off = pose_identity();
    off.position.y = loc.pose.position.y;
    c->floor_space = create_ref_space(c, XR_REFERENCE_SPACE_TYPE_LOCAL, off);
    c->floor_path = "STAGE->LOCAL offset";
    return XR_SUCCESS;
}

/* createLayerProjection -> collectSwapchainFormatsIfNeeded + createSwapchain */
static XrResult create_projection_layer(client_t *c, int *formats_ok)
{
    uint32_t n = 0;
    CHECK(p_xrEnumerateSwapchainFormats(c->sess, 0, &n, NULL));
    int64_t f[16];
    if (n == 0 || n > 16)
        return XR_ERROR_RUNTIME_FAILURE;
    CHECK(p_xrEnumerateSwapchainFormats(c->sess, n, &n, f));
    int rgba = 0, srgb = 0;
    for (uint32_t i = 0; i < n; i++) {
        rgba |= f[i] == GL_RGBA8;
        srgb |= f[i] == GL_SRGB8_ALPHA8;
    }
    *formats_ok = rgba && srgb;
    XrSwapchainCreateInfo ci = T(XrSwapchainCreateInfo, XR_TYPE_SWAPCHAIN_CREATE_INFO);
    ci.arraySize = 1;
    ci.format = rgba ? GL_RGBA8 : f[0]; /* selectColorFormat */
    ci.width = c->sc_w = 2 * c->vcv[0].recommendedImageRectWidth;
    ci.height = c->sc_h = c->vcv[0].recommendedImageRectHeight;
    ci.mipCount = 1;
    ci.faceCount = 1;
    ci.sampleCount = 1;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    CHECK(p_xrCreateSwapchain(c->sess, &ci, &c->sc));
    CHECK(p_xrEnumerateSwapchainImages(c->sc, 0, &c->n_images, NULL));
    if (c->n_images == 0 || c->n_images > 8)
        return XR_ERROR_RUNTIME_FAILURE;
    XrSwapchainImageOpenGLESKHR img[8];
    for (uint32_t i = 0; i < c->n_images; i++)
        img[i] = T(XrSwapchainImageOpenGLESKHR, XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR);
    CHECK(p_xrEnumerateSwapchainImages(c->sc, c->n_images, &c->n_images, (XrSwapchainImageBaseHeader *)img));
    for (uint32_t i = 0; i < c->n_images; i++)
        c->images[i] = img[i].image;
    return XR_SUCCESS;
}

/* OpenXRLayerProjection::endFrame + OpenXRCoordinator::endFrame */
static XrResult end_frame(client_t *c, int with_layer, XrEnvironmentBlendMode blend, int bad_rect)
{
    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerProjection layer = T(XrCompositionLayerProjection, XR_TYPE_COMPOSITION_LAYER_PROJECTION);
    const XrCompositionLayerBaseHeader *layers[1] = {(const XrCompositionLayerBaseHeader *)&layer};
    if (with_layer) {
        const int32_t half = (int32_t)c->sc_w / 2;
        for (int i = 0; i < 2; i++) {
            pv[i] = T(XrCompositionLayerProjectionView, XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW);
            pv[i].pose = c->local_views[i].pose;
            pv[i].fov = c->local_views[i].fov;
            pv[i].subImage.swapchain = c->sc;
            pv[i].subImage.imageRect.offset.x = i * half;
            pv[i].subImage.imageRect.offset.y = 0;
            pv[i].subImage.imageRect.extent.width = half + (bad_rect && i ? 1 : 0);
            pv[i].subImage.imageRect.extent.height = (int32_t)c->sc_h;
        }
        layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        layer.space = c->local;
        layer.viewCount = 2;
        layer.views = pv;
    }
    XrFrameEndInfo ei = T(XrFrameEndInfo, XR_TYPE_FRAME_END_INFO);
    ei.displayTime = c->fs.predictedDisplayTime;
    ei.environmentBlendMode = blend;
    ei.layerCount = with_layer ? 1 : 0;
    ei.layers = layers;
    return p_xrEndFrame(c->sess, &ei);
}

/* -- per-frame measurements ---------------------------------------------------------------- */
typedef struct {
    int frames;              /* focused frames measured */
    double t_prev, dt_sum, dt_min, dt_max;
    int late;
    XrTime pdt_prev;
    int pdt_bad, skipped;
    XrPosef origin_prev;
    int origin_have;
    float max_step_m, max_step_deg, yaw_min, yaw_max;
    int view_bad, ipd_bad, fov_bad;
    float ipd;
    XrFovf fov[2];
    int idx_bad, expected_idx;
    int actions_active, actions_checked;
    int select_transitions, select_prev, select_changed_flags;
    int aim_valid, grip_valid, aim_side_bad;
    float floor_y;
    int floor_have;
    int not_render;
} meas_t;

static float yaw_deg(XrQuaternionf q)
{
    const XrVector3f f = q_rotate(q, (XrVector3f){0, 0, -1});
    return atan2f(-f.x, -f.z) * 57.29578f;
}

/* beginFrame + populateFrameData + (render) + endFrame for one frame.
 * Returns 0 to stop. */
static int run_frame(client_t *c, meas_t *m, int measure, int expect_controllers)
{
    if (!poll_events(c))
        return 0;
    if (!c->running)
        return c->sess != XR_NULL_HANDLE;
    XrFrameWaitInfo wi = T(XrFrameWaitInfo, XR_TYPE_FRAME_WAIT_INFO);
    XrFrameState fs = T(XrFrameState, XR_TYPE_FRAME_STATE);
    if (XR_FAILED(p_xrWaitFrame(c->sess, &wi, &fs)))
        return 0;
    const double now = spike_now_ms();
    XrFrameBeginInfo bi = T(XrFrameBeginInfo, XR_TYPE_FRAME_BEGIN_INFO);
    if (XR_FAILED(p_xrBeginFrame(c->sess, &bi)))
        return 0;
    c->fs = fs;
    if (XR_FAILED(reference_spaces(c)))
        return 0;
    if (measure) {
        if (m->frames > 0) {
            const double dt = now - m->t_prev;
            m->dt_sum += dt;
            if (dt < m->dt_min)
                m->dt_min = dt;
            if (dt > m->dt_max)
                m->dt_max = dt;
            m->late += dt > 25.0;
            if (getenv("XR_SMOKE_TRACE") && (dt > 20.0 || dt < 12.0))
                spike_log("  frame %d: interval %.2f ms", m->frames, dt);
            /* steps are whole periods; >1 means the schedule skipped slots after a stall */
            const XrDuration step = fs.predictedDisplayTime - m->pdt_prev;
            if (step <= 0 || step % fs.predictedDisplayPeriod != 0)
                m->pdt_bad++;
            else
                m->skipped += (int)(step / fs.predictedDisplayPeriod) - 1;
        }
        m->t_prev = now;
        m->pdt_prev = fs.predictedDisplayTime;
        if (!fs.shouldRender)
            m->not_render++;
    }
    if (!fs.shouldRender)
        return XR_SUCCEEDED(end_frame(c, 0, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 0));

    /* populateFrameData: views in LOCAL (for the layer), then in VIEW */
    XrViewLocateInfo li = T(XrViewLocateInfo, XR_TYPE_VIEW_LOCATE_INFO);
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = fs.predictedDisplayTime;
    li.space = c->local;
    XrViewState vs = T(XrViewState, XR_TYPE_VIEW_STATE);
    uint32_t n = 0;
    c->local_views[0] = c->local_views[1] = T(XrView, XR_TYPE_VIEW);
    if (XR_FAILED(p_xrLocateViews(c->sess, &li, &vs, 2, &n, c->local_views)) || n != 2)
        return 0;
    XrView vv[2] = {T(XrView, XR_TYPE_VIEW), T(XrView, XR_TYPE_VIEW)};
    li.space = c->viewer;
    if (XR_FAILED(p_xrLocateViews(c->sess, &li, &vs, 2, &n, vv)))
        return 0;

    /* m_input->collectInputSources */
    XrActiveActionSet as[2] = {{c->hand[0].set, XR_NULL_PATH}, {c->hand[1].set, XR_NULL_PATH}};
    XrActionsSyncInfo si = T(XrActionsSyncInfo, XR_TYPE_ACTIONS_SYNC_INFO);
    si.countActiveActionSets = 2;
    si.activeActionSets = as;
    if (XR_FAILED(p_xrSyncActions(c->sess, &si)))
        return 0;
    for (int h = 0; h < 2; h++) {
        hand_t *hd = &c->hand[h];
        XrSpaceLocation aim = T(XrSpaceLocation, XR_TYPE_SPACE_LOCATION);
        XrSpaceLocation grip = T(XrSpaceLocation, XR_TYPE_SPACE_LOCATION);
        p_xrLocateSpace(hd->aim_space, c->local, fs.predictedDisplayTime, &aim);
        p_xrLocateSpace(hd->grip_space, c->local, fs.predictedDisplayTime, &grip);
        for (int k = 0; k < N_BUTTONS; k++) {
            XrActionStateGetInfo gi = T(XrActionStateGetInfo, XR_TYPE_ACTION_STATE_GET_INFO);
            XrActionStateBoolean b = T(XrActionStateBoolean, XR_TYPE_ACTION_STATE_BOOLEAN);
            XrActionStateFloat f = T(XrActionStateFloat, XR_TYPE_ACTION_STATE_FLOAT);
            gi.action = hd->press[k];
            p_xrGetActionStateBoolean(c->sess, &gi, &b);
            if (measure) {
                m->actions_checked++;
                m->actions_active += b.isActive != 0;
            }
            if (measure && k == 0 && b.isActive) {
                if ((int)b.currentState != m->select_prev)
                    m->select_transitions++;
                m->select_changed_flags += b.changedSinceLastSync != 0;
                m->select_prev = (int)b.currentState;
            }
            gi.action = hd->touch[k];
            p_xrGetActionStateBoolean(c->sess, &gi, &b);
            gi.action = hd->value[k];
            p_xrGetActionStateFloat(c->sess, &gi, &f);
            if (measure && k > 0) {
                m->actions_checked += 2;
                m->actions_active += (b.isActive != 0) + (f.isActive != 0);
            }
        }
        for (int k = 0; k < N_AXES; k++) {
            XrActionStateGetInfo gi = T(XrActionStateGetInfo, XR_TYPE_ACTION_STATE_GET_INFO);
            XrActionStateVector2f v = T(XrActionStateVector2f, XR_TYPE_ACTION_STATE_VECTOR2F);
            gi.action = hd->axis[k];
            p_xrGetActionStateVector2f(c->sess, &gi, &v);
            if (measure) {
                m->actions_checked++;
                m->actions_active += v.isActive != 0;
            }
        }
        if (measure) {
            const XrSpaceLocationFlags ok = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            if ((aim.locationFlags & ok) == ok) {
                m->aim_valid++;
                if ((h == 0) != (aim.pose.position.x < 0))
                    m->aim_side_bad++;
            }
            m->grip_valid += (grip.locationFlags & ok) == ok;
        }
    }
    (void)expect_controllers;

    /* origin (viewer in local) and floor */
    XrSpaceLocation org = T(XrSpaceLocation, XR_TYPE_SPACE_LOCATION);
    if (XR_FAILED(p_xrLocateSpace(c->viewer, c->local, fs.predictedDisplayTime, &org)))
        return 0;
    if (c->floor_space) {
        XrSpaceLocation fl = T(XrSpaceLocation, XR_TYPE_SPACE_LOCATION);
        if (XR_SUCCEEDED(p_xrLocateSpace(c->floor_space, c->local, fs.predictedDisplayTime, &fl)) && measure &&
            !m->floor_have) {
            m->floor_y = fl.pose.position.y;
            m->floor_have = 1;
        }
    }

    if (measure) {
        const float y = yaw_deg(org.pose.orientation);
        if (m->frames == 0 || y < m->yaw_min)
            m->yaw_min = y;
        if (m->frames == 0 || y > m->yaw_max)
            m->yaw_max = y;
        if (m->origin_have) {
            const float step = v3_dist(org.pose.position, m->origin_prev.position);
            const float ang = q_angle(org.pose.orientation, m->origin_prev.orientation) * 57.29578f;
            if (step > m->max_step_m)
                m->max_step_m = step;
            if (ang > m->max_step_deg)
                m->max_step_deg = ang;
        }
        m->origin_prev = org.pose;
        m->origin_have = 1;
        /* views: viewer-space eyes straddle the head; local = origin * viewer */
        const float ipd = v3_dist(vv[0].pose.position, vv[1].pose.position);
        m->ipd = ipd;
        if (!(vv[0].pose.position.x < 0 && vv[1].pose.position.x > 0) || ipd < 0.05f || ipd > 0.08f)
            m->ipd_bad++;
        for (int e = 0; e < 2; e++) {
            const XrPosef expect_local = pose_mul(org.pose, vv[e].pose);
            if (v3_dist(expect_local.position, c->local_views[e].pose.position) > 0.001f ||
                q_angle(expect_local.orientation, c->local_views[e].pose.orientation) > 0.002f)
                m->view_bad++;
        }
        m->fov[0] = vv[0].fov;
        m->fov[1] = vv[1].fov;
        /* asymmetric: each eye's outer half-angle is wider than its inner one */
        if (!(-vv[0].fov.angleLeft > vv[0].fov.angleRight && vv[1].fov.angleRight > -vv[1].fov.angleLeft))
            m->fov_bad++;
    }

    /* layer->startFrame: acquire + wait */
    XrSwapchainImageAcquireInfo ai = T(XrSwapchainImageAcquireInfo, XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
    uint32_t idx = 0;
    if (XR_FAILED(p_xrAcquireSwapchainImage(c->sc, &ai, &idx)))
        return 0;
    XrSwapchainImageWaitInfo wti = T(XrSwapchainImageWaitInfo, XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
    wti.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(p_xrWaitSwapchainImage(c->sc, &wti)))
        return 0;
    if (measure) {
        if (m->frames > 0 && (int)idx != m->expected_idx)
            m->idx_bad++;
        m->expected_idx = (int)((idx + 1) % c->n_images);
    }
    /* (the web process renders into c->images[idx] here) */
    XrSwapchainImageReleaseInfo ri = T(XrSwapchainImageReleaseInfo, XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
    if (XR_FAILED(p_xrReleaseSwapchainImage(c->sc, &ri)))
        return 0;
    if (XR_FAILED(end_frame(c, 1, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 0)))
        return 0;
    if (measure)
        m->frames++;
    return 1;
}

/* -- phases ------------------------------------------------------------------------------- */
static int has_ext(const XrExtensionProperties *e, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name))
            return 1;
    return 0;
}

/* initializeDevice + createSessionIfNeeded + OpenXRInput::create */
static XrResult bring_up(client_t *c, int report, XrVersion api)
{
    uint32_t n = 0;
    CHECK(p_xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL));
    XrExtensionProperties ext[32];
    if (n > 32)
        return XR_ERROR_RUNTIME_FAILURE;
    for (uint32_t i = 0; i < n; i++)
        ext[i] = T(XrExtensionProperties, XR_TYPE_EXTENSION_PROPERTIES);
    if (report) {
        uint32_t n1 = 0;
        expect("ext-size-insufficient", p_xrEnumerateInstanceExtensionProperties(NULL, 1, &n1, ext),
               n > 1 ? XR_ERROR_SIZE_INSUFFICIENT : XR_SUCCESS);
        expect("ext-layer-absent", p_xrEnumerateInstanceExtensionProperties("XR_APILAYER_x", 0, &n1, NULL),
               XR_ERROR_API_LAYER_NOT_PRESENT);
    }
    CHECK(p_xrEnumerateInstanceExtensionProperties(NULL, n, &n, ext));
    const int gles = has_ext(ext, n, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME);
    const int egl = has_ext(ext, n, XR_MNDX_EGL_ENABLE_EXTENSION_NAME);
    c->has_hand_ext = has_ext(ext, n, XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    c->has_floor_ext = has_ext(ext, n, XR_EXT_LOCAL_FLOOR_EXTENSION_NAME);
    const int unwanted = has_ext(ext, n, "XR_EXT_hand_interaction") ||
                         has_ext(ext, n, "XR_KHR_composition_layer_cylinder") ||
                         has_ext(ext, n, "XR_ANDROID_trackables") ||
                         has_ext(ext, n, "XR_MSFT_unbounded_reference_space");
    if (report) {
        char list[512] = "";
        for (uint32_t i = 0; i < n; i++)
            snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? " " : "", ext[i].extensionName);
        if (gles && egl && !unwanted)
            SPIKE_PASS("extensions", "%u: %s", n, list);
        else
            SPIKE_FAIL("extensions", "required gles %d egl %d, unexpected optional %d: %s", gles, egl, unwanted, list);
    }

    /* createInstance(): requiredInstanceExtensions() = MNDX (if supported) + GLES */
    const char *names[4];
    uint32_t ne = 0;
    if (egl)
        names[ne++] = XR_MNDX_EGL_ENABLE_EXTENSION_NAME;
    names[ne++] = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
    if (c->has_hand_ext) /* ENABLE(WEBXR_HANDS) */
        names[ne++] = XR_EXT_HAND_TRACKING_EXTENSION_NAME;
    XrInstanceCreateInfo ci = T(XrInstanceCreateInfo, XR_TYPE_INSTANCE_CREATE_INFO);
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "WebKit");
    ci.applicationInfo.applicationVersion = 1;
    snprintf(ci.applicationInfo.engineName, sizeof ci.applicationInfo.engineName, "WebKit");
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = api;
    if (report) {
        const char *bad[] = {"XR_KHR_vulkan_enable2"};
        XrInstanceCreateInfo bci = ci;
        bci.enabledExtensionCount = 1;
        bci.enabledExtensionNames = bad;
        XrInstance tmp = XR_NULL_HANDLE;
        expect("instance-ext-absent", p_xrCreateInstance(&bci, &tmp), XR_ERROR_EXTENSION_NOT_PRESENT);
        bci = ci;
        bci.applicationInfo.apiVersion = XR_MAKE_VERSION(2, 0, 0);
        expect("instance-api-2.0", p_xrCreateInstance(&bci, &tmp), XR_ERROR_API_VERSION_UNSUPPORTED);
    }
    ci.enabledExtensionCount = ne;
    ci.enabledExtensionNames = names;
    CHECK(p_xrCreateInstance(&ci, &c->inst));
    if (!load_funcs(c->inst, 0))
        return XR_ERROR_FUNCTION_UNSUPPORTED;

    /* OpenXRExtensions::loadMethods */
    CHECK(p_gipa(c->inst, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&p_gles_req));
    if (c->has_hand_ext) {
        CHECK(p_gipa(c->inst, "xrCreateHandTrackerEXT", (PFN_xrVoidFunction *)&p_create_hand));
        CHECK(p_gipa(c->inst, "xrDestroyHandTrackerEXT", (PFN_xrVoidFunction *)&p_destroy_hand));
        CHECK(p_gipa(c->inst, "xrLocateHandJointsEXT", (PFN_xrVoidFunction *)&p_locate_hand));
    }
#ifndef XR_SMOKE_LOADER
    /* (the official loader only forwards names it knows) */
    p_gipa(c->inst, "xrGetRuntimeStatsPSVR2", (PFN_xrVoidFunction *)&p_stats);
#endif
    if (report) {
        PFN_xrVoidFunction f = NULL;
        expect("gipa-null-instance", p_gipa(XR_NULL_HANDLE, "xrCreateSession", &f), XR_ERROR_HANDLE_INVALID);
        expect("gipa-unknown", p_gipa(c->inst, "xrDoesNotExist", &f), XR_ERROR_FUNCTION_UNSUPPORTED);
        XrInstanceProperties ip = T(XrInstanceProperties, XR_TYPE_INSTANCE_PROPERTIES);
        p_xrGetInstanceProperties(c->inst, &ip);
        SPIKE_PASS("instance", "%s %u.%u.%u, API %u.%u, %u extensions enabled", ip.runtimeName,
                   XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
                   XR_VERSION_PATCH(ip.runtimeVersion), XR_VERSION_MAJOR(XR_CURRENT_API_VERSION),
                   XR_VERSION_MINOR(XR_CURRENT_API_VERSION), ne);
    }

    /* initializeSystem + systemProperties */
    XrSystemGetInfo sgi = T(XrSystemGetInfo, XR_TYPE_SYSTEM_GET_INFO);
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (report) {
        XrSystemGetInfo hh = sgi;
        hh.formFactor = XR_FORM_FACTOR_HANDHELD_DISPLAY;
        XrSystemId tmp;
        expect("system-handheld", p_xrGetSystem(c->inst, &hh, &tmp), XR_ERROR_FORM_FACTOR_UNSUPPORTED);
    }
    CHECK(p_xrGetSystem(c->inst, &sgi, &c->sys));
    XrSystemHandTrackingPropertiesEXT htp = T(XrSystemHandTrackingPropertiesEXT, XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT);
    htp.supportsHandTracking = XR_TRUE; /* must come back false */
    XrSystemProperties sp = T(XrSystemProperties, XR_TYPE_SYSTEM_PROPERTIES);
    sp.next = &htp;
    CHECK(p_xrGetSystemProperties(c->inst, c->sys, &sp));

    /* collectViewConfigurations + initializeBlendModes */
    uint32_t nvc = 0;
    CHECK(p_xrEnumerateViewConfigurations(c->inst, c->sys, 0, &nvc, NULL));
    XrViewConfigurationType vct[4];
    CHECK(p_xrEnumerateViewConfigurations(c->inst, c->sys, nvc, &nvc, vct));
    uint32_t nv = 0;
    CHECK(p_xrEnumerateViewConfigurationViews(c->inst, c->sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nv, NULL));
    c->vcv[0] = c->vcv[1] = T(XrViewConfigurationView, XR_TYPE_VIEW_CONFIGURATION_VIEW);
    CHECK(p_xrEnumerateViewConfigurationViews(c->inst, c->sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, c->vcv));
    uint32_t nbm = 0;
    CHECK(p_xrEnumerateEnvironmentBlendModes(c->inst, c->sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nbm, NULL));
    XrEnvironmentBlendMode bm[4];
    CHECK(p_xrEnumerateEnvironmentBlendModes(c->inst, c->sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nbm, &nbm, bm));
    if (report) {
        const int ok = sp.trackingProperties.orientationTracking && !htp.supportsHandTracking && nvc == 1 &&
                       vct[0] == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO && nv == 2 && nbm >= 1 &&
                       bm[0] == XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        SPIKE_RESULT("system", ok ? "PASS" : "FAIL",
                     "\"%s\" vendor 0x%04x, orientation %d position %d hands %d, %u view config(s), 2 views "
                     "rec %ux%u (WebKit recommendedResolution %ux%u), max layers %u, blend OPAQUE",
                     sp.systemName, sp.vendorId, sp.trackingProperties.orientationTracking,
                     sp.trackingProperties.positionTracking, htp.supportsHandTracking, nvc,
                     c->vcv[0].recommendedImageRectWidth, c->vcv[0].recommendedImageRectHeight,
                     2 * c->vcv[0].recommendedImageRectWidth, c->vcv[0].recommendedImageRectHeight,
                     sp.graphicsProperties.maxLayerCount);
    }

    /* createSessionIfNeeded: graphics requirements, then XrGraphicsBindingEGLMNDX */
    XrGraphicsBindingEGLMNDX gb = T(XrGraphicsBindingEGLMNDX, XR_TYPE_GRAPHICS_BINDING_EGL_MNDX);
    gb.getProcAddress = dummy_get_proc; /* eglGetProcAddress in WebKit */
    gb.display = NULL;                  /* the mock backend accepts null EGL handles */
    gb.config = NULL;
    gb.context = NULL;
    XrSessionCreateInfo sci = T(XrSessionCreateInfo, XR_TYPE_SESSION_CREATE_INFO);
    sci.systemId = c->sys;
    sci.next = &gb;
    if (report) {
        XrSession tmp;
        expect("session-before-gfx-reqs", p_xrCreateSession(c->inst, &sci, &tmp),
               XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING);
    }
    XrGraphicsRequirementsOpenGLESKHR req = T(XrGraphicsRequirementsOpenGLESKHR, XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR);
    CHECK(p_gles_req(c->inst, c->sys, &req));
    if (report) {
        XrSession tmp;
        XrSessionCreateInfo nob = sci;
        nob.next = NULL;
        expect("session-no-binding", p_xrCreateSession(c->inst, &nob, &tmp), XR_ERROR_GRAPHICS_DEVICE_INVALID);
        expect("session-bad-instance", p_xrCreateSession((XrInstance)(uintptr_t)0xdead00000001ull, &sci, &tmp),
               XR_ERROR_HANDLE_INVALID);
        XrSessionCreateInfo bs = sci;
        bs.systemId = 42;
        expect("session-bad-system", p_xrCreateSession(c->inst, &bs, &tmp), XR_ERROR_SYSTEM_INVALID);
    }
    CHECK(p_xrCreateSession(c->inst, &sci, &c->sess));
    if (report)
        SPIKE_PASS("session", "XrGraphicsBindingEGLMNDX (null EGL handles), GLES %u.%u-%u.%u",
                   XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported),
                   XR_VERSION_MAJOR(req.maxApiVersionSupported), XR_VERSION_MINOR(req.maxApiVersionSupported));
    if (c->has_hand_ext && report) {
        XrHandTrackerCreateInfoEXT hci = T(XrHandTrackerCreateInfoEXT, XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT);
        hci.hand = XR_HAND_LEFT_EXT;
        hci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        XrHandTrackerEXT ht;
        expect("hand-tracker-unsupported", p_create_hand(c->sess, &hci, &ht), XR_ERROR_FEATURE_UNSUPPORTED);
    }
    CHECK(input_create(c));
    return XR_SUCCESS;
}

/* One extra frame whose projection layer uses the given swapchains/rects. */
static XrResult custom_frame(client_t *c, XrSwapchain sc0, XrSwapchain sc1, XrRect2Di r0, XrRect2Di r1)
{
    XrFrameState fs = T(XrFrameState, XR_TYPE_FRAME_STATE);
    CHECK(p_xrWaitFrame(c->sess, NULL, &fs));
    CHECK(p_xrBeginFrame(c->sess, NULL));
    c->fs = fs;
    XrSwapchain scs[2] = {sc0, sc1};
    for (int i = 0; i < (sc0 == sc1 ? 1 : 2); i++) {
        uint32_t idx;
        XrSwapchainImageWaitInfo wti = T(XrSwapchainImageWaitInfo, XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
        wti.timeout = XR_INFINITE_DURATION;
        CHECK(p_xrAcquireSwapchainImage(scs[i], NULL, &idx));
        CHECK(p_xrWaitSwapchainImage(scs[i], &wti));
        CHECK(p_xrReleaseSwapchainImage(scs[i], NULL));
    }
    XrCompositionLayerProjectionView pv[2];
    const XrRect2Di rr[2] = {r0, r1};
    for (int i = 0; i < 2; i++) {
        pv[i] = T(XrCompositionLayerProjectionView, XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW);
        pv[i].pose = c->local_views[i].pose;
        pv[i].fov = c->local_views[i].fov;
        pv[i].subImage.swapchain = scs[i];
        pv[i].subImage.imageRect = rr[i];
    }
    XrCompositionLayerProjection layer = T(XrCompositionLayerProjection, XR_TYPE_COMPOSITION_LAYER_PROJECTION);
    layer.space = c->local;
    layer.viewCount = 2;
    layer.views = pv;
    const XrCompositionLayerBaseHeader *layers[1] = {(const XrCompositionLayerBaseHeader *)&layer};
    XrFrameEndInfo ei = T(XrFrameEndInfo, XR_TYPE_FRAME_END_INFO);
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = 1;
    ei.layers = layers;
    return p_xrEndFrame(c->sess, &ei);
}

static XrSwapchain make_swapchain(client_t *c, uint32_t w, uint32_t h)
{
    XrSwapchainCreateInfo ci = T(XrSwapchainCreateInfo, XR_TYPE_SWAPCHAIN_CREATE_INFO);
    ci.format = GL_RGBA8;
    ci.width = w;
    ci.height = h;
    ci.arraySize = ci.mipCount = ci.faceCount = ci.sampleCount = 1;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    XrSwapchain sc = XR_NULL_HANDLE;
    p_xrCreateSwapchain(c->sess, &ci, &sc);
    return sc;
}

static const char *state_name(XrSessionState s)
{
    static const char *const n[] = {"UNKNOWN", "IDLE", "READY", "SYNCHRONIZED", "VISIBLE",
                                    "FOCUSED", "STOPPING", "LOSS_PENDING", "EXITING"};
    return (unsigned)s < 9 ? n[s] : "?";
}

/* waitForSessionReady + createLayerProjection + frame loop + endSessionIfExists */
static int run_session(client_t *c, int frames, int report, meas_t *m, char *states, size_t states_sz)
{
    memset(m, 0, sizeof *m);
    m->dt_min = 1e9;
    for (int i = 0; i < 100 && !c->running; i++) { /* waitForSessionReady */
        if (!poll_events(c))
            return -1;
        if (!c->running && c->state < XR_SESSION_STATE_READY)
            usleep(10000);
    }
    if (!c->running) {
        SPIKE_FAIL("state-machine", "never became READY/running (state %s)", state_name(c->state));
        return -1;
    }
    int formats_ok = 0;
    XrResult r = create_projection_layer(c, &formats_ok);
    if (report) {
        if (XR_FAILED(r))
            SPIKE_FAIL("swapchain", "create_projection_layer -> %d", (int)r);
        else
            SPIKE_RESULT("swapchain", formats_ok ? "PASS" : "FAIL",
                         "formats RGBA8+SRGB8_ALPHA8 %s, %ux%u RGBA8 x%u images (GL names %u..%u)",
                         formats_ok ? "listed" : "MISSING", c->sc_w, c->sc_h, c->n_images, c->images[0],
                         c->images[c->n_images - 1]);
    }
    if (XR_FAILED(r))
        return -1;

    int warm = 0, ok = 1;
    while (ok && c->state != XR_SESSION_STATE_FOCUSED && warm < 120) {
        ok = run_frame(c, m, 0, 0);
        warm++;
    }
    if (c->state != XR_SESSION_STATE_FOCUSED) {
        SPIKE_FAIL("state-machine", "not FOCUSED after %d frames (state %s)", warm, state_name(c->state));
        return -1;
    }
    const double t0 = spike_now_ms();
    while (ok && m->frames < frames)
        ok = run_frame(c, m, 1, 0);
    const double secs = (spike_now_ms() - t0) / 1000.0;
    if (!ok) {
        SPIKE_FAIL("frames", "frame loop failed after %d frames", m->frames);
        return -1;
    }
    if (report) {
        rt_stats_t st;
        memset(&st, 0, sizeof st);
        int have_stats = p_stats && XR_SUCCEEDED(p_stats(c->sess, &st));
        static const char *const kLayout[] = {"none", "SBS_EXACT", "SBS_SCALED", "TWO_SWAPCHAINS", "OTHER"};
        spike_log("frames: %d focused in %.2f s after %d warm-up; stats %s: waited %llu ended %llu submitted %llu "
                  "empty %llu discarded %llu backend_errors %llu layout %s src %ux%u eye %ux%u",
                  m->frames, secs, warm, have_stats ? "ok" : "n/a", (unsigned long long)st.frames_waited,
                  (unsigned long long)st.frames_ended, (unsigned long long)st.frames_submitted,
                  (unsigned long long)st.frames_empty, (unsigned long long)st.frames_discarded,
                  (unsigned long long)st.backend_errors, kLayout[st.last_layout < 5 ? st.last_layout : 0],
                  st.last_src_w, st.last_src_h, st.eye_w, st.eye_h);
        const double mean = m->dt_sum / (m->frames - 1);
        SPIKE_RESULT("frame-timing", mean > 15.5 && mean < 17.9 && m->late <= m->frames / 50 && m->pdt_bad == 0 &&
                                             m->skipped <= m->frames / 50 &&
                                             m->not_render == 0
                                         ? "PASS"
                                         : "FAIL",
                     "%d frames, %.2f fps, xrWaitFrame interval mean %.2f ms (min %.2f max %.2f), %d late >25 ms, "
                     "predictedDisplayTime off the %.3f ms grid %d, slots skipped %d, shouldRender=false %d",
                     m->frames, m->frames / secs, mean, m->dt_min, m->dt_max, m->late,
                     c->fs.predictedDisplayPeriod / 1e6, m->pdt_bad, m->skipped, m->not_render);
        SPIKE_RESULT("pose-continuity", m->max_step_m < 0.02f && m->max_step_deg < 3.0f && m->yaw_max - m->yaw_min > 5.0f
                                            ? "PASS"
                                            : "FAIL",
                     "max per-frame step %.2f mm / %.3f deg, yaw swept %.1f..%.1f deg", (double)m->max_step_m * 1000,
                     (double)m->max_step_deg, (double)m->yaw_min, (double)m->yaw_max);
        SPIKE_RESULT("views-fov", m->view_bad == 0 && m->ipd_bad == 0 && m->fov_bad == 0 ? "PASS" : "FAIL",
                     "IPD %.1f mm, L fov l/r/u/d %.1f/%.1f/%.1f/%.1f R %.1f/%.1f/%.1f/%.1f deg, local=origin*viewer "
                     "mismatches %d, ipd %d, symmetric-fov %d",
                     (double)m->ipd * 1000, (double)(m->fov[0].angleLeft * 57.29578f),
                     (double)(m->fov[0].angleRight * 57.29578f), (double)(m->fov[0].angleUp * 57.29578f),
                     (double)(m->fov[0].angleDown * 57.29578f), (double)(m->fov[1].angleLeft * 57.29578f),
                     (double)(m->fov[1].angleRight * 57.29578f), (double)(m->fov[1].angleUp * 57.29578f),
                     (double)(m->fov[1].angleDown * 57.29578f), m->view_bad, m->ipd_bad, m->fov_bad);
        SPIKE_RESULT("swapchain-cycling", m->idx_bad == 0 ? "PASS" : "FAIL",
                     "%d frames, image index round-robin over %u, %d out of order; submitted layout %s", m->frames,
                     c->n_images, m->idx_bad, have_stats ? kLayout[st.last_layout < 5 ? st.last_layout : 0] : "?");
        SPIKE_RESULT("spaces", m->floor_have && m->floor_y < -1.5f && m->floor_y > -1.8f ? "PASS" : "FAIL",
                     "VIEW/LOCAL + floor via %s: floor %.3f m below LOCAL", c->floor_path ? c->floor_path : "none",
                     (double)-m->floor_y);
    }
    if (report) {
        /* error codes the frame loop and WebKit rely on, mid-session */
        expect("begin-without-wait", p_xrBeginFrame(c->sess, NULL), XR_ERROR_CALL_ORDER_INVALID);
        XrFrameEndInfo ei = T(XrFrameEndInfo, XR_TYPE_FRAME_END_INFO);
        ei.displayTime = c->fs.predictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        expect("end-without-begin", p_xrEndFrame(c->sess, &ei), XR_ERROR_CALL_ORDER_INVALID);
        XrSessionBeginInfo bi = T(XrSessionBeginInfo, XR_TYPE_SESSION_BEGIN_INFO);
        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        expect("begin-session-running", p_xrBeginSession(c->sess, &bi), XR_ERROR_SESSION_RUNNING);
        expect("end-session-not-stopping", p_xrEndSession(c->sess), XR_ERROR_SESSION_NOT_STOPPING);
        expect("release-unacquired", p_xrReleaseSwapchainImage(c->sc, NULL), XR_ERROR_CALL_ORDER_INVALID);
        XrSwapchainImageWaitInfo wti = T(XrSwapchainImageWaitInfo, XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
        wti.timeout = XR_INFINITE_DURATION;
        expect("wait-unacquired", p_xrWaitSwapchainImage(c->sc, &wti), XR_ERROR_CALL_ORDER_INVALID);
        XrSwapchainCreateInfo sci = T(XrSwapchainCreateInfo, XR_TYPE_SWAPCHAIN_CREATE_INFO);
        sci.format = 0x1908; /* GL_RGBA (unsized) */
        sci.width = sci.height = 64;
        sci.arraySize = sci.mipCount = sci.faceCount = sci.sampleCount = 1;
        XrSwapchain bad_sc;
        expect("swapchain-format", p_xrCreateSwapchain(c->sess, &sci, &bad_sc), XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED);
        uint32_t cnt = 0;
        int64_t one_fmt;
        expect("formats-size-insufficient", p_xrEnumerateSwapchainFormats(c->sess, 1, &cnt, &one_fmt),
               XR_ERROR_SIZE_INSUFFICIENT);
        XrPath pth;
        expect("path-format", p_xrStringToPath(c->inst, "/user//hand", &pth), XR_ERROR_PATH_FORMAT_INVALID);
        expect("path-uppercase", p_xrStringToPath(c->inst, "/User/hand", &pth), XR_ERROR_PATH_FORMAT_INVALID);
        char pbuf[4];
        expect("path-null", p_xrPathToString(c->inst, XR_NULL_PATH, sizeof pbuf, &cnt, pbuf), XR_ERROR_PATH_INVALID);
        expect("path-size-insufficient", p_xrPathToString(c->inst, c->hand[0].sub, sizeof pbuf, &cnt, pbuf),
               XR_ERROR_SIZE_INSUFFICIENT);
        XrSpaceLocation loc = T(XrSpaceLocation, XR_TYPE_SPACE_LOCATION);
        expect("locate-bad-handle", p_xrLocateSpace((XrSpace)(uintptr_t)0x123400000005ull, c->local, 1, &loc),
               XR_ERROR_HANDLE_INVALID);
        expect("locate-time-0", p_xrLocateSpace(c->viewer, c->local, 0, &loc), XR_ERROR_TIME_INVALID);
        XrActionStateGetInfo gi = T(XrActionStateGetInfo, XR_TYPE_ACTION_STATE_GET_INFO);
        gi.action = c->hand[0].press[0];
        XrActionStateFloat fst = T(XrActionStateFloat, XR_TYPE_ACTION_STATE_FLOAT);
        expect("action-type-mismatch", p_xrGetActionStateFloat(c->sess, &gi, &fst), XR_ERROR_ACTION_TYPE_MISMATCH);
        gi.subactionPath = c->hand[1].sub;
        XrActionStateBoolean bst = T(XrActionStateBoolean, XR_TYPE_ACTION_STATE_BOOLEAN);
        expect("action-subaction-unsupported", p_xrGetActionStateBoolean(c->sess, &gi, &bst), XR_ERROR_PATH_UNSUPPORTED);
        XrInteractionProfileSuggestedBinding sb =
            T(XrInteractionProfileSuggestedBinding, XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING);
        XrActionSuggestedBinding one = {c->hand[0].press[0], c->hand[0].sub};
        p_xrStringToPath(c->inst, "/interaction_profiles/khr/simple_controller", &sb.interactionProfile);
        sb.countSuggestedBindings = 1;
        sb.suggestedBindings = &one;
        expect("suggest-after-attach", p_xrSuggestInteractionProfileBindings(c->inst, &sb),
               XR_ERROR_ACTIONSETS_ALREADY_ATTACHED);
        XrReferenceSpaceCreateInfo rci = T(XrReferenceSpaceCreateInfo, XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
        rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_UNBOUNDED_MSFT;
        rci.poseInReferenceSpace = pose_identity();
        XrSpace sp;
        expect("space-unbounded", p_xrCreateReferenceSpace(c->sess, &rci, &sp), XR_ERROR_REFERENCE_SPACE_UNSUPPORTED);
        rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        rci.poseInReferenceSpace.orientation.w = 0.5f;
        expect("space-pose-invalid", p_xrCreateReferenceSpace(c->sess, &rci, &sp), XR_ERROR_POSE_INVALID);
        XrViewLocateInfo li = T(XrViewLocateInfo, XR_TYPE_VIEW_LOCATE_INFO);
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = c->fs.predictedDisplayTime;
        li.space = c->local;
        XrViewState vs = T(XrViewState, XR_TYPE_VIEW_STATE);
        XrView v1 = T(XrView, XR_TYPE_VIEW);
        expect("views-size-insufficient", p_xrLocateViews(c->sess, &li, &vs, 1, &cnt, &v1), XR_ERROR_SIZE_INSUFFICIENT);

        /* one frame with bad submissions, then a good one to close it */
        XrFrameState fs = T(XrFrameState, XR_TYPE_FRAME_STATE);
        p_xrWaitFrame(c->sess, NULL, &fs);
        p_xrBeginFrame(c->sess, NULL);
        c->fs = fs;
        uint32_t idx;
        p_xrAcquireSwapchainImage(c->sc, NULL, &idx);
        expect("acquire-twice-ok", p_xrAcquireSwapchainImage(c->sc, NULL, &idx), XR_SUCCESS);
        p_xrWaitSwapchainImage(c->sc, &wti);
        p_xrReleaseSwapchainImage(c->sc, NULL);
        p_xrWaitSwapchainImage(c->sc, &wti);
        p_xrReleaseSwapchainImage(c->sc, NULL);
        expect("end-bad-rect", end_frame(c, 1, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1), XR_ERROR_SWAPCHAIN_RECT_INVALID);
        expect("end-bad-blend", end_frame(c, 1, XR_ENVIRONMENT_BLEND_MODE_ADDITIVE, 0),
               XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED);
        expect("end-good", end_frame(c, 1, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 0), XR_SUCCESS);

        /* other layouts xrEndFrame must take: one swapchain per eye, and a
         * scaled side-by-side swapchain (framebufferScaleFactor 0.75) */
        static const char *const kLayout[] = {"none", "SBS_EXACT", "SBS_SCALED", "TWO_SWAPCHAINS", "OTHER"};
        const uint32_t ew = c->vcv[0].recommendedImageRectWidth, eh = c->vcv[0].recommendedImageRectHeight;
        XrSwapchain l = make_swapchain(c, ew, eh), rgt = make_swapchain(c, ew, eh);
        const XrRect2Di full = {{0, 0}, {(int32_t)ew, (int32_t)eh}};
        const XrResult r2 = custom_frame(c, l, rgt, full, full);
        rt_stats_t st2 = {0};
        if (p_stats)
            p_stats(c->sess, &st2);
        const uint32_t sw = (ew * 3 / 4) & ~1u, sh = eh * 3 / 4;
        XrSwapchain scaled = make_swapchain(c, 2 * sw, sh);
        const XrRect2Di hl = {{0, 0}, {(int32_t)sw, (int32_t)sh}}, hr = {{(int32_t)sw, 0}, {(int32_t)sw, (int32_t)sh}};
        const XrResult r3 = custom_frame(c, scaled, scaled, hl, hr);
        rt_stats_t st3 = {0};
        if (p_stats)
            p_stats(c->sess, &st3);
        p_xrDestroySwapchain(l);
        p_xrDestroySwapchain(rgt);
        p_xrDestroySwapchain(scaled);
        const int lay_ok = r2 == XR_SUCCESS && r3 == XR_SUCCESS &&
                           (!p_stats || (st2.last_layout == RT_LAYOUT_TWO_SWAPCHAINS && st3.last_layout == RT_LAYOUT_SBS_SCALED));
        SPIKE_RESULT("layouts", lay_ok ? "PASS" : "FAIL",
                     "SBS one swapchain %ux%u (WebKit) ok; two swapchains %ux%u -> %d (%s); scaled SBS %ux%u -> %d (%s)",
                     c->sc_w, c->sc_h, ew, eh, (int)r2, p_stats ? kLayout[st2.last_layout % 5] : "?", 2 * sw, sh,
                     (int)r3, p_stats ? kLayout[st3.last_layout % 5] : "?");
    }

    /* endSessionIfExists: request exit, poll until the session is gone */
    if (XR_FAILED(p_xrRequestExitSession(c->sess))) {
        SPIKE_FAIL("state-machine", "xrRequestExitSession failed");
        return -1;
    }
    for (int i = 0; i < 64 && c->sess != XR_NULL_HANDLE; i++)
        poll_events(c);
    states[0] = 0;
    for (int i = 0; i < c->n_seen; i++)
        snprintf(states + strlen(states), states_sz - strlen(states), "%s%s", i ? ">" : "", state_name(c->seen[i]));
    return 0;
}

int main(int argc, char **argv)
{
    int frames = 300;
    const char *lib = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = atoi(argv[++i]);
        else
            lib = argv[i];
    }
    spike_log("ps5webkit spike 05: OpenXR runtime smoke test (%s mode, %d frames)", MODE, frames);
#ifdef __APPLE__
    /* WebKit's XR WorkQueue runs at user-interactive QoS; without it macOS
     * coalesces the mock's 60 Hz sleeps by several ms. */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

    /* -- negotiate ---------------------------------------------------------- */
#if defined(XR_SMOKE_LOADER)
    p_gipa = xrGetInstanceProcAddr; /* official loader; runtime from XR_RUNTIME_JSON */
    (void)lib;
    SPIKE_PASS("negotiate", "official OpenXR loader, XR_RUNTIME_JSON=%s",
               getenv("XR_RUNTIME_JSON") ? getenv("XR_RUNTIME_JSON") : "(unset)");
#else
    PFN_xrNegotiateLoaderRuntimeInterface negotiate = NULL;
#if defined(XR_SMOKE_STATIC)
    negotiate = xrNegotiateLoaderRuntimeInterface;
    (void)lib;
#else
    if (!lib)
        lib = "build/host/libopenxr_psvr2.dylib";
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        SPIKE_FAIL("negotiate", "dlopen %s: %s", lib, dlerror());
        return 1;
    }
    negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)dlsym(h, "xrNegotiateLoaderRuntimeInterface");
#endif
    XrNegotiateLoaderInfo li = {XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION,
                                sizeof(XrNegotiateLoaderInfo), 1, XR_CURRENT_LOADER_RUNTIME_VERSION,
                                XR_MAKE_VERSION(1, 0, 0), XR_CURRENT_API_VERSION};
    XrNegotiateRuntimeRequest rq = {XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION,
                                    sizeof(XrNegotiateRuntimeRequest), 0, 0, NULL};
    XrNegotiateLoaderInfo bad = li;
    bad.minInterfaceVersion = bad.maxInterfaceVersion = 99;
    XrNegotiateRuntimeRequest brq = rq;
    const XrResult rbad = negotiate ? negotiate(&bad, &brq) : XR_ERROR_RUNTIME_FAILURE;
    const XrResult rn = negotiate ? negotiate(&li, &rq) : XR_ERROR_RUNTIME_FAILURE;
    if (XR_FAILED(rn) || !rq.getInstanceProcAddr || XR_SUCCEEDED(rbad)) {
        SPIKE_FAIL("negotiate", "negotiate %d gipa %p, bad-version %d", (int)rn, (void *)rq.getInstanceProcAddr,
                   (int)rbad);
        return 1;
    }
    p_gipa = rq.getInstanceProcAddr;
    SPIKE_PASS("negotiate", "xrNegotiateLoaderRuntimeInterface: interface %u, API %u.%u.%u; interface 99 refused",
               rq.runtimeInterfaceVersion, XR_VERSION_MAJOR(rq.runtimeApiVersion),
               XR_VERSION_MINOR(rq.runtimeApiVersion), XR_VERSION_PATCH(rq.runtimeApiVersion));
#endif
    if (!load_funcs(XR_NULL_HANDLE, 1)) {
        SPIKE_FAIL("negotiate", "global functions missing");
        return 1;
    }

    /* -- phase 1: WebKit's session, controllers absent ------------------------ */
    client_t c;
    memset(&c, 0, sizeof c);
    XrResult r = bring_up(&c, 1, XR_CURRENT_API_VERSION);
    if (XR_FAILED(r)) {
        SPIKE_FAIL("bring-up", "failed with %d", (int)r);
        return 1;
    }
    SPIKE_PASS("input-setup", "2 action sets x (2 pose + 21 button + 2 axis actions), simple_controller bindings, "
                              "attached");
    const XrSpace stale_space = c.hand[0].aim_space;
    const XrInstance stale_inst = c.inst;
    meas_t m;
    char states[256];
    if (run_session(&c, frames, 1, &m, states, sizeof states) != 0) {
        destroy_session_objects(&c);
        return 1;
    }
    SPIKE_RESULT("input-inactive", m.actions_active == 0 && m.aim_valid == 0 && m.grip_valid == 0 &&
                                         c.hand[0].profile[0] == 0 && c.profile_events == 0
                                     ? "PASS"
                                     : "FAIL",
                 "%d action-state queries, %d active; aim/grip located %d/%d; profile \"%s\"", m.actions_checked,
                 m.actions_active, m.aim_valid, m.grip_valid, c.hand[0].profile);
    const char *want = "IDLE>READY>SYNCHRONIZED>VISIBLE>FOCUSED>VISIBLE>SYNCHRONIZED>STOPPING>IDLE>EXITING";
    SPIKE_RESULT("state-machine", !strcmp(states, want) && c.exited ? "PASS" : "FAIL", "%s", states);
#ifndef XR_SMOKE_LOADER
    { /* destroyed handles are rejected by the runtime (through the loader they are UB) */
        XrEventDataBuffer ev = T(XrEventDataBuffer, XR_TYPE_EVENT_DATA_BUFFER);
        expect("stale-space", p_xrDestroySpace(stale_space), XR_ERROR_HANDLE_INVALID);
        expect("stale-instance", p_xrPollEvent(stale_inst, &ev), XR_ERROR_HANDLE_INVALID);
    }
#else
    (void)stale_space;
    (void)stale_inst;
#endif
    SPIKE_RESULT("errors", g_expect_bad == 0 ? "PASS" : "FAIL", "%u/%u expected XrResult codes%s%s",
                 g_expect_n - g_expect_bad, g_expect_n, g_expect_bad ? "; wrong: " : "", g_errs);

    /* -- phase 2: action plumbing with synthetic controllers --------------------- */
    setenv("XR_PSVR2_MOCK_CONTROLLERS", "1", 1);
    client_t c2;
    memset(&c2, 0, sizeof c2);
    r = bring_up(&c2, 0, XR_API_VERSION_1_0); /* older WebKit headers: STAGE fallback */
    if (XR_FAILED(r)) {
        SPIKE_FAIL("input-plumbing", "bring-up failed with %d", (int)r);
        return 1;
    }
    meas_t m2;
    char states2[256];
    if (run_session(&c2, 90, 0, &m2, states2, sizeof states2) != 0) {
        SPIKE_FAIL("input-plumbing", "session failed");
        return 1;
    }
    const int plumb_ok = c2.profile_events >= 1 && !strcmp(c2.hand[0].profile, "/interaction_profiles/khr/simple_controller") &&
                         m2.select_transitions >= 2 && m2.select_changed_flags >= 2 && m2.aim_valid == 2 * m2.frames &&
                         m2.grip_valid == 2 * m2.frames && m2.aim_side_bad == 0 && c2.exited;
    SPIKE_RESULT("input-plumbing", plumb_ok ? "PASS" : "FAIL",
                 "XR_PSVR2_MOCK_CONTROLLERS=1: %d profile event(s), profile \"%s\", select transitions %d "
                 "(changedSinceLastSync %d), aim/grip located %d/%d of %d, %d active of %d action states (unbound stay "
                 "inactive)",
                 c2.profile_events, c2.hand[0].profile, m2.select_transitions, m2.select_changed_flags, m2.aim_valid,
                 m2.grip_valid, 2 * m2.frames, m2.actions_active, m2.actions_checked);
    SPIKE_RESULT("spaces-api1.0", m2.floor_have && m2.floor_y < -1.5f && m2.floor_y > -1.8f &&
                                       c2.floor_path && !strcmp(c2.floor_path, "STAGE->LOCAL offset")
                                   ? "PASS"
                                   : "FAIL",
                 "apiVersion 1.0, XR_EXT_local_floor not enabled (as WebKit): floor via %s, %.3f m below LOCAL",
                 c2.floor_path ? c2.floor_path : "none", (double)-m2.floor_y);
    (void)g_fail;
    return 0;
}
