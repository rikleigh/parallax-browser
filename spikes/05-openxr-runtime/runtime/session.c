/* session.c - session state machine, reference/action spaces, frame loop
 * and views for libopenxr_psvr2. */
#include "xr_internal.h"
#include "xr_math.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ALL_LOCATION_BITS                                                                          \
    (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |              \
     XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT)

/* -- session --------------------------------------------------------------------------- */
static const XrGraphicsBindingEGLMNDX *find_egl_binding(const void *next)
{
    for (const XrBaseInStructure *n = next; n; n = n->next)
        if (n->type == XR_TYPE_GRAPHICS_BINDING_EGL_MNDX)
            return (const XrGraphicsBindingEGLMNDX *)n;
    return NULL;
}

XRRT_CORE XrResult XRAPI_CALL xrCreateSession(XrInstance instance, const XrSessionCreateInfo *createInfo,
                                              XrSession *session)
{
    RT_GUARD();
    rt_instance *inst = rt_get_instance(instance);
    if (!inst)
        return XR_ERROR_HANDLE_INVALID;
    if (!createInfo || !session || createInfo->type != XR_TYPE_SESSION_CREATE_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    if (createInfo->systemId != RT_SYSTEM_ID || !inst->system_fetched)
        return XR_ERROR_SYSTEM_INVALID;
    const XrGraphicsBindingEGLMNDX *egl = find_egl_binding(createInfo->next);
    if (!egl || !inst->ext.egl || !inst->ext.gles) {
        RT_ERR("xrCreateSession: needs XR_KHR_opengl_es_enable + XR_MNDX_egl_enable and an "
               "XrGraphicsBindingEGLMNDX (headless sessions are not supported)");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    if (!inst->gfx_requirements_queried)
        return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
    if (inst->session)
        return XR_ERROR_LIMIT_REACHED;
    XrResult r = inst->be->session_create(egl);
    if (XR_FAILED(r))
        return r;
    rt_session *s = calloc(1, sizeof *s);
    if (!s) {
        inst->be->session_destroy();
        return XR_ERROR_OUT_OF_MEMORY;
    }
    s->inst = inst;
    s->handle = (XrSession)(uintptr_t)rt_handle_new(OBJ_SESSION, s);
    s->state = XR_SESSION_STATE_UNKNOWN;
    s->display_period = inst->info.frame_period_ns;
    s->stats.eye_w = inst->info.rec_w;
    s->stats.eye_h = inst->info.rec_h;
    inst->session = s;
    *session = s->handle;
    rt_session_set_state(s, XR_SESSION_STATE_IDLE);
    /* Nothing to wait for: the headset was opened at xrGetSystem. */
    rt_session_set_state(s, XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}

void rt_session_destroy_locked(rt_session *s)
{
    rt_instance *inst = s->inst;
    while (s->spaces)
        rt_space_destroy_locked(s->spaces);
    while (s->swapchains)
        rt_swapchain_destroy_locked(s->swapchains);
    if (s->running)
        inst->be->session_end();
    inst->be->session_destroy();
    rt_event_drop_session(inst, s->handle);
    for (rt_action_set *set = inst->sets; set; set = set->next)
        set->attached = 0;
    rt_handle_free(RT_H(s->handle));
    inst->session = NULL;
    free(s);
}

XRRT_CORE XrResult XRAPI_CALL xrDestroySession(XrSession session)
{
    RT_GUARD();
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    rt_session_destroy_locked(s);
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrBeginSession(XrSession session, const XrSessionBeginInfo *beginInfo)
{
    RT_GUARD();
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    if (!beginInfo || beginInfo->type != XR_TYPE_SESSION_BEGIN_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    if (beginInfo->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    if (s->running)
        return XR_ERROR_SESSION_RUNNING;
    if (s->state != XR_SESSION_STATE_READY)
        return XR_ERROR_SESSION_NOT_READY;
    XrResult r = s->inst->be->session_begin();
    if (XR_FAILED(r))
        return r;
    s->running = 1;
    s->view_config = beginInfo->primaryViewConfigurationType;
    s->waits_pending = 0;
    s->frame_begun = 0;
    s->frame_ended_once = 0;
    /* The frame loop is ours from here on; VISIBLE/FOCUSED follow the first
     * xrEndFrame. */
    rt_session_set_state(s, XR_SESSION_STATE_SYNCHRONIZED);
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrRequestExitSession(XrSession session)
{
    RT_GUARD();
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    if (!s->running)
        return XR_ERROR_SESSION_NOT_RUNNING;
    s->exit_requested = 1;
    /* Walk down the state diagram, one event per step. */
    if (s->state == XR_SESSION_STATE_FOCUSED)
        rt_session_set_state(s, XR_SESSION_STATE_VISIBLE);
    if (s->state == XR_SESSION_STATE_VISIBLE)
        rt_session_set_state(s, XR_SESSION_STATE_SYNCHRONIZED);
    rt_session_set_state(s, XR_SESSION_STATE_STOPPING);
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrEndSession(XrSession session)
{
    RT_GUARD();
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    if (!s->running)
        return XR_ERROR_SESSION_NOT_RUNNING;
    if (s->state != XR_SESSION_STATE_STOPPING)
        return XR_ERROR_SESSION_NOT_STOPPING;
    s->inst->be->session_end();
    s->running = 0;
    s->frame_begun = 0;
    s->waits_pending = 0;
    rt_session_set_state(s, XR_SESSION_STATE_IDLE);
    if (s->exit_requested)
        rt_session_set_state(s, XR_SESSION_STATE_EXITING);
    else
        rt_session_set_state(s, XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}

/* -- spaces ---------------------------------------------------------------------------- */
static int ref_space_supported(rt_session *s, XrReferenceSpaceType t)
{
    switch (t) {
    case XR_REFERENCE_SPACE_TYPE_VIEW:
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
    case XR_REFERENCE_SPACE_TYPE_STAGE:
        return 1;
    case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR:
        /* Core in 1.1, else needs XR_EXT_local_floor enabled. (WebKit checks
         * the extension is *supported* but never enables it, so on 1.0 it
         * falls back to STAGE - which works the same here.) */
        return s->inst->ext.local_floor || XR_VERSION_MINOR(s->inst->api_version) >= 1;
    default:
        return 0;
    }
}

XRRT_CORE XrResult XRAPI_CALL xrEnumerateReferenceSpaces(XrSession session, uint32_t spaceCapacityInput,
                                                         uint32_t *spaceCountOutput,
                                                         XrReferenceSpaceType *spaces)
{
    RT_GUARD();
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    XrReferenceSpaceType list[4];
    uint32_t n = 0;
    list[n++] = XR_REFERENCE_SPACE_TYPE_VIEW;
    list[n++] = XR_REFERENCE_SPACE_TYPE_LOCAL;
    list[n++] = XR_REFERENCE_SPACE_TYPE_STAGE;
    if (ref_space_supported(s, XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR))
        list[n++] = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR;
    int copy;
    XrResult r = rt_two_call(spaceCapacityInput, spaceCountOutput, n, spaces, &copy);
    if (copy)
        memcpy(spaces, list, n * sizeof *list);
    return r;
}

static rt_space *space_new(rt_session *s)
{
    rt_space *sp = calloc(1, sizeof *sp);
    if (!sp)
        return NULL;
    sp->sess = s;
    sp->handle = (XrSpace)(uintptr_t)rt_handle_new(OBJ_SPACE, sp);
    sp->next = s->spaces;
    s->spaces = sp;
    return sp;
}

XRRT_CORE XrResult XRAPI_CALL xrCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo *createInfo,
                                                     XrSpace *space)
{
    RT_GUARD();
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    if (!createInfo || !space || createInfo->type != XR_TYPE_REFERENCE_SPACE_CREATE_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    if (!ref_space_supported(s, createInfo->referenceSpaceType))
        return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    if (!rt_pose_ok(&createInfo->poseInReferenceSpace))
        return XR_ERROR_POSE_INVALID;
    rt_space *sp = space_new(s);
    if (!sp)
        return XR_ERROR_OUT_OF_MEMORY;
    sp->ref = createInfo->referenceSpaceType;
    sp->offset = createInfo->poseInReferenceSpace;
    sp->offset.orientation = q_normalize(sp->offset.orientation);
    sp->hand = -1;
    *space = sp->handle;
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo *createInfo,
                                                  XrSpace *space)
{
    RT_GUARD();
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    if (!createInfo || !space || createInfo->type != XR_TYPE_ACTION_SPACE_CREATE_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    rt_action *a = rt_handle_get(RT_H(createInfo->action), OBJ_ACTION);
    if (!a)
        return XR_ERROR_HANDLE_INVALID;
    if (a->type != XR_ACTION_TYPE_POSE_INPUT)
        return XR_ERROR_ACTION_TYPE_MISMATCH;
    int hand = -1;
    if (createInfo->subactionPath != XR_NULL_PATH) {
        if (!rt_path_is_valid(s->inst, createInfo->subactionPath))
            return XR_ERROR_PATH_INVALID;
        uint32_t i = 0;
        while (i < a->n_sub && a->sub[i] != createInfo->subactionPath)
            i++;
        if (i == a->n_sub)
            return XR_ERROR_PATH_UNSUPPORTED;
        hand = a->sub_hand[i];
    }
    if (!rt_pose_ok(&createInfo->poseInActionSpace))
        return XR_ERROR_POSE_INVALID;
    rt_space *sp = space_new(s);
    if (!sp)
        return XR_ERROR_OUT_OF_MEMORY;
    sp->is_action = 1;
    sp->action = a;
    sp->hand = hand;
    sp->offset = createInfo->poseInActionSpace;
    sp->offset.orientation = q_normalize(sp->offset.orientation);
    *space = sp->handle;
    return XR_SUCCESS;
}

void rt_space_destroy_locked(rt_space *sp)
{
    rt_session *s = sp->sess;
    for (rt_space **pp = &s->spaces; *pp; pp = &(*pp)->next) {
        if (*pp == sp) {
            *pp = sp->next;
            break;
        }
    }
    rt_handle_free(RT_H(sp->handle));
    free(sp);
}

XRRT_CORE XrResult XRAPI_CALL xrDestroySpace(XrSpace space)
{
    RT_GUARD();
    rt_space *sp = rt_handle_get(RT_H(space), OBJ_SPACE);
    if (!sp)
        return XR_ERROR_HANDLE_INVALID;
    rt_space_destroy_locked(sp);
    return XR_SUCCESS;
}

/* LOCAL is gravity aligned at the first tracked head pose (yaw only);
 * STAGE / LOCAL_FLOOR share its x/z and heading with y at the floor. */
static void ensure_origin(rt_session *s, XrTime t)
{
    if (s->origin_set)
        return;
    rt_views_t v;
    memset(&v, 0, sizeof v);
    s->inst->be->locate_views(t, &v);
    if (!v.orientation_valid)
        return;
    s->origin.position = v.head.position;
    s->origin.orientation = q_yaw_only(v.head.orientation);
    s->origin_set = 1;
    RT_INFO("LOCAL origin at (%.3f %.3f %.3f)", (double)s->origin.position.x,
            (double)s->origin.position.y, (double)s->origin.position.z);
}

static XrPosef stage_origin(rt_session *s)
{
    XrPosef o = s->origin_set ? s->origin : pose_identity();
    const rt_backend_info *bi = &s->inst->info;
    o.position.y = bi->floor_known ? bi->floor_y : o.position.y - bi->default_eye_height;
    return o;
}

XrSpaceLocationFlags rt_space_pose(rt_session *s, rt_space *sp, XrTime t, XrPosef *out)
{
    ensure_origin(s, t);
    XrPosef base = pose_identity();
    XrSpaceLocationFlags flags = 0;
    if (sp->is_action) {
        rt_action *a = sp->action;
        int src = -1, hand = -1;
        if (!a->alive)
            return 0;
        for (uint32_t i = 0; i < a->n_bind && src < 0; i++) {
            if (sp->hand >= 0 && a->bind[i].hand != sp->hand)
                continue;
            if (a->bind[i].src == SRC_GRIP_POSE || a->bind[i].src == SRC_AIM_POSE) {
                rt_controller_t c;
                memset(&c, 0, sizeof c);
                s->inst->be->controller(a->bind[i].hand, t, &c);
                if (!c.active)
                    continue;
                src = a->bind[i].src;
                hand = a->bind[i].hand;
                base = src == SRC_GRIP_POSE ? c.grip : c.aim;
            }
        }
        if (src < 0 || !s->sets_attached || !a->alive || !a->set || !a->set->attached)
            return 0;
        (void)hand;
        flags = ALL_LOCATION_BITS;
    } else {
        switch (sp->ref) {
        case XR_REFERENCE_SPACE_TYPE_VIEW: {
            rt_views_t v;
            memset(&v, 0, sizeof v);
            s->inst->be->locate_views(t, &v);
            base = v.head;
            flags |= v.orientation_valid ? XR_SPACE_LOCATION_ORIENTATION_VALID_BIT : 0;
            flags |= v.position_valid ? XR_SPACE_LOCATION_POSITION_VALID_BIT : 0;
            flags |= v.orientation_tracked ? XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT : 0;
            flags |= v.position_tracked ? XR_SPACE_LOCATION_POSITION_TRACKED_BIT : 0;
            break;
        }
        case XR_REFERENCE_SPACE_TYPE_LOCAL:
            base = s->origin_set ? s->origin : pose_identity();
            flags = ALL_LOCATION_BITS;
            break;
        case XR_REFERENCE_SPACE_TYPE_STAGE:
        case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR:
            base = stage_origin(s);
            flags = ALL_LOCATION_BITS;
            break;
        default:
            return 0;
        }
    }
    *out = pose_mul(base, sp->offset);
    return flags;
}

static XrSpaceLocationFlags relate(XrSpaceLocationFlags a, XrSpaceLocationFlags b)
{
    if (!(a & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) || !(b & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        return 0;
    XrSpaceLocationFlags f = a & b;
    return f;
}

static XrResult locate_one(rt_session *s, rt_space *sp, rt_space *base, XrTime t, XrPosef *pose,
                           XrSpaceLocationFlags *flags)
{
    XrPosef ps, pb;
    const XrSpaceLocationFlags fs = rt_space_pose(s, sp, t, &ps);
    const XrSpaceLocationFlags fb = rt_space_pose(s, base, t, &pb);
    *flags = relate(fs, fb);
    *pose = *flags ? pose_relative(pb, ps) : pose_identity();
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time,
                                            XrSpaceLocation *location)
{
    RT_GUARD();
    rt_space *sp = rt_handle_get(RT_H(space), OBJ_SPACE);
    rt_space *base = rt_handle_get(RT_H(baseSpace), OBJ_SPACE);
    if (!sp || !base)
        return XR_ERROR_HANDLE_INVALID;
    if (sp->sess != base->sess)
        return XR_ERROR_VALIDATION_FAILURE;
    if (!location || location->type != XR_TYPE_SPACE_LOCATION)
        return XR_ERROR_VALIDATION_FAILURE;
    if (time <= 0)
        return XR_ERROR_TIME_INVALID;
    locate_one(sp->sess, sp, base, time, &location->pose, &location->locationFlags);
    for (XrBaseOutStructure *n = (XrBaseOutStructure *)location->next; n; n = n->next) {
        if (n->type == XR_TYPE_SPACE_VELOCITY)
            ((XrSpaceVelocity *)n)->velocityFlags = 0;
    }
    return XR_SUCCESS;
}

/* OpenXR 1.1 core. */
XRRT_CORE XrResult XRAPI_CALL xrLocateSpaces(XrSession session, const XrSpacesLocateInfo *locateInfo,
                                             XrSpaceLocations *spaceLocations)
{
    RT_GUARD();
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    if (!locateInfo || !spaceLocations || locateInfo->type != XR_TYPE_SPACES_LOCATE_INFO ||
        spaceLocations->type != XR_TYPE_SPACE_LOCATIONS ||
        spaceLocations->locationCount != locateInfo->spaceCount ||
        (locateInfo->spaceCount && (!locateInfo->spaces || !spaceLocations->locations)))
        return XR_ERROR_VALIDATION_FAILURE;
    if (locateInfo->time <= 0)
        return XR_ERROR_TIME_INVALID;
    rt_space *base = rt_handle_get(RT_H(locateInfo->baseSpace), OBJ_SPACE);
    if (!base || base->sess != s)
        return XR_ERROR_HANDLE_INVALID;
    for (uint32_t i = 0; i < locateInfo->spaceCount; i++) {
        rt_space *sp = rt_handle_get(RT_H(locateInfo->spaces[i]), OBJ_SPACE);
        if (!sp || sp->sess != s)
            return XR_ERROR_HANDLE_INVALID;
        locate_one(s, sp, base, locateInfo->time, &spaceLocations->locations[i].pose,
                   &spaceLocations->locations[i].locationFlags);
    }
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrGetReferenceSpaceBoundsRect(XrSession session, XrReferenceSpaceType referenceSpaceType,
                                                            XrExtent2Df *bounds)
{
    RT_GUARD();
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    if (!bounds)
        return XR_ERROR_VALIDATION_FAILURE;
    if (!ref_space_supported(s, referenceSpaceType))
        return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    bounds->width = bounds->height = 0;
    return XR_SPACE_BOUNDS_UNAVAILABLE;
}

/* -- views ----------------------------------------------------------------------------------- */
static void fov_from_tan(const float t[4], XrFovf *f)
{
    f->angleLeft = -atanf(t[0]);
    f->angleRight = atanf(t[1]);
    f->angleUp = atanf(t[2]);
    f->angleDown = -atanf(t[3]);
}

XRRT_CORE XrResult XRAPI_CALL xrLocateViews(XrSession session, const XrViewLocateInfo *viewLocateInfo,
                                            XrViewState *viewState, uint32_t viewCapacityInput,
                                            uint32_t *viewCountOutput, XrView *views)
{
    RT_GUARD();
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    if (!viewLocateInfo || !viewState || viewLocateInfo->type != XR_TYPE_VIEW_LOCATE_INFO ||
        viewState->type != XR_TYPE_VIEW_STATE)
        return XR_ERROR_VALIDATION_FAILURE;
    if (viewLocateInfo->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    if (viewLocateInfo->displayTime <= 0)
        return XR_ERROR_TIME_INVALID;
    rt_space *base = rt_handle_get(RT_H(viewLocateInfo->space), OBJ_SPACE);
    if (!base || base->sess != s)
        return XR_ERROR_HANDLE_INVALID;
    int copy;
    XrResult r = rt_two_call(viewCapacityInput, viewCountOutput, RT_VIEW_COUNT, views, &copy);
    if (!copy)
        return r;
    for (uint32_t i = 0; i < RT_VIEW_COUNT; i++)
        if (views[i].type != XR_TYPE_VIEW)
            return XR_ERROR_VALIDATION_FAILURE;

    const XrTime t = viewLocateInfo->displayTime;
    XrPosef pb;
    const XrSpaceLocationFlags fb = rt_space_pose(s, base, t, &pb);
    rt_views_t v;
    memset(&v, 0, sizeof v);
    s->inst->be->locate_views(t, &v);
    XrSpaceLocationFlags fv = 0;
    fv |= v.orientation_valid ? XR_SPACE_LOCATION_ORIENTATION_VALID_BIT : 0;
    fv |= v.position_valid ? XR_SPACE_LOCATION_POSITION_VALID_BIT : 0;
    fv |= v.orientation_tracked ? XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT : 0;
    fv |= v.position_tracked ? XR_SPACE_LOCATION_POSITION_TRACKED_BIT : 0;
    viewState->viewStateFlags = relate(fv, fb);
    for (uint32_t i = 0; i < RT_VIEW_COUNT; i++) {
        views[i].pose = viewState->viewStateFlags ? pose_relative(pb, v.eye[i]) : pose_identity();
        fov_from_tan(s->inst->info.tan[i], &views[i].fov);
    }
    return XR_SUCCESS;
}

/* -- frame loop -------------------------------------------------------------------------------- */
static int should_render(const rt_session *s)
{
    return s->state == XR_SESSION_STATE_VISIBLE || s->state == XR_SESSION_STATE_FOCUSED;
}

XRRT_CORE XrResult XRAPI_CALL xrWaitFrame(XrSession session, const XrFrameWaitInfo *frameWaitInfo,
                                          XrFrameState *frameState)
{
    rt_lock();
    rt_session *s = rt_get_session(session);
    XrResult r = XR_SUCCESS;
    if (!s)
        r = XR_ERROR_HANDLE_INVALID;
    else if ((frameWaitInfo && frameWaitInfo->type != XR_TYPE_FRAME_WAIT_INFO) || !frameState ||
             frameState->type != XR_TYPE_FRAME_STATE)
        r = XR_ERROR_VALIDATION_FAILURE;
    else if (!s->running)
        r = XR_ERROR_SESSION_NOT_RUNNING;
    if (XR_FAILED(r)) {
        rt_unlock();
        return r;
    }
    const rt_backend_ops *be = s->inst->be;
    rt_unlock();

    /* Pace without the lock: other threads may poll events meanwhile. */
    rt_frame_t f;
    memset(&f, 0, sizeof f);
    const int rc = be->wait_frame(&f);

    rt_lock();
    if (rt_get_session(session) != s || !s->running) {
        rt_unlock();
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    if (rc != 0)
        s->stats.backend_errors++;
    if (f.display_time <= s->last_display_time)
        f.display_time = s->last_display_time + (f.period ? f.period : s->display_period);
    s->last_display_time = f.display_time;
    if (f.period)
        s->display_period = f.period;
    /* Presence: FOCUSED while the headset is worn, VISIBLE otherwise. */
    if (s->frame_ended_once && !s->exit_requested) {
        const int present = be->user_present();
        if (present && s->state == XR_SESSION_STATE_VISIBLE)
            rt_session_set_state(s, XR_SESSION_STATE_FOCUSED);
        else if (!present && s->state == XR_SESSION_STATE_FOCUSED)
            rt_session_set_state(s, XR_SESSION_STATE_VISIBLE);
    }
    frameState->predictedDisplayTime = f.display_time;
    frameState->predictedDisplayPeriod = s->display_period;
    frameState->shouldRender = should_render(s) ? XR_TRUE : XR_FALSE;
    s->waits_pending++;
    s->stats.frames_waited++;
    rt_unlock();
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrBeginFrame(XrSession session, const XrFrameBeginInfo *frameBeginInfo)
{
    RT_GUARD();
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    if (frameBeginInfo && frameBeginInfo->type != XR_TYPE_FRAME_BEGIN_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    if (!s->running)
        return XR_ERROR_SESSION_NOT_RUNNING;
    if (s->waits_pending == 0)
        return XR_ERROR_CALL_ORDER_INVALID;
    s->waits_pending--;
    s->stats.frames_begun++;
    if (s->frame_begun) {
        s->stats.frames_discarded++;
        return XR_FRAME_DISCARDED;
    }
    s->frame_begun = 1;
    return XR_SUCCESS;
}

static XrResult check_sub_image(rt_session *s, const XrSwapchainSubImage *si, rt_swapchain **out)
{
    rt_swapchain *sc = rt_handle_get(RT_H(si->swapchain), OBJ_SWAPCHAIN);
    if (!sc || sc->sess != s)
        return XR_ERROR_HANDLE_INVALID;
    if (sc->last_released < 0)
        return XR_ERROR_LAYER_INVALID;
    if (si->imageArrayIndex >= sc->info.arraySize)
        return XR_ERROR_VALIDATION_FAILURE;
    const XrRect2Di *r = &si->imageRect;
    if (r->offset.x < 0 || r->offset.y < 0 || r->extent.width <= 0 || r->extent.height <= 0 ||
        (uint64_t)r->offset.x + (uint64_t)r->extent.width > sc->info.width ||
        (uint64_t)r->offset.y + (uint64_t)r->extent.height > sc->info.height)
        return XR_ERROR_SWAPCHAIN_RECT_INVALID;
    *out = sc;
    return XR_SUCCESS;
}

static rt_layout_t classify(const rt_swapchain *sc[2], const XrRect2Di r[2], uint32_t eye_w, uint32_t eye_h)
{
    if (sc[0] != sc[1])
        return RT_LAYOUT_TWO_SWAPCHAINS;
    const int32_t half = (int32_t)(sc[0]->info.width / 2), h = (int32_t)sc[0]->info.height;
    if (r[0].offset.x == 0 && r[0].offset.y == 0 && r[0].extent.width == half && r[0].extent.height == h &&
        r[1].offset.x == half && r[1].offset.y == 0 && r[1].extent.width == half && r[1].extent.height == h)
        return ((uint32_t)half == eye_w && (uint32_t)h == eye_h) ? RT_LAYOUT_SBS_EXACT : RT_LAYOUT_SBS_SCALED;
    return RT_LAYOUT_OTHER;
}

XRRT_CORE XrResult XRAPI_CALL xrEndFrame(XrSession session, const XrFrameEndInfo *frameEndInfo)
{
    RT_GUARD();
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    if (!frameEndInfo || frameEndInfo->type != XR_TYPE_FRAME_END_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    if (!s->running)
        return XR_ERROR_SESSION_NOT_RUNNING;
    if (!s->frame_begun)
        return XR_ERROR_CALL_ORDER_INVALID;
    if (frameEndInfo->displayTime <= 0)
        return XR_ERROR_TIME_INVALID;
    if (frameEndInfo->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
        return XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;
    if (frameEndInfo->layerCount > RT_MAX_LAYERS)
        return XR_ERROR_LAYER_LIMIT_EXCEEDED;
    if (frameEndInfo->layerCount && !frameEndInfo->layers)
        return XR_ERROR_LAYER_INVALID;

    rt_submit_t sub;
    memset(&sub, 0, sizeof sub);
    sub.display_time = frameEndInfo->displayTime;
    uint32_t ignored = 0;
    /* Validate everything before acting on anything. */
    for (uint32_t i = 0; i < frameEndInfo->layerCount; i++) {
        const XrCompositionLayerBaseHeader *l = frameEndInfo->layers[i];
        if (!l)
            return XR_ERROR_LAYER_INVALID;
        rt_space *sp = rt_handle_get(RT_H(l->space), OBJ_SPACE);
        if (!sp || sp->sess != s)
            return XR_ERROR_HANDLE_INVALID;
        if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const XrCompositionLayerProjection *p = (const XrCompositionLayerProjection *)l;
            if (p->viewCount != RT_VIEW_COUNT || !p->views)
                return XR_ERROR_VALIDATION_FAILURE;
            const rt_swapchain *scs[2];
            XrRect2Di rects[2];
            for (uint32_t e = 0; e < RT_VIEW_COUNT; e++) {
                const XrCompositionLayerProjectionView *v = &p->views[e];
                if (v->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW)
                    return XR_ERROR_VALIDATION_FAILURE;
                if (!rt_pose_ok(&v->pose))
                    return XR_ERROR_POSE_INVALID;
                if (!(v->fov.angleLeft < v->fov.angleRight && v->fov.angleDown < v->fov.angleUp))
                    return XR_ERROR_VALIDATION_FAILURE;
                rt_swapchain *sc;
                XrResult r = check_sub_image(s, &v->subImage, &sc);
                if (XR_FAILED(r))
                    return r;
                scs[e] = sc;
                rects[e] = v->subImage.imageRect;
            }
            if (sub.has_projection) {
                ignored++;
                continue;
            }
            /* The first projection layer goes to the headset. */
            XrPosef layer_pose;
            const XrSpaceLocationFlags lf = rt_space_pose(s, sp, frameEndInfo->displayTime, &layer_pose);
            sub.has_projection = 1;
            sub.layout = classify(scs, rects, s->inst->info.rec_w, s->inst->info.rec_h);
            for (uint32_t e = 0; e < RT_VIEW_COUNT; e++) {
                const XrCompositionLayerProjectionView *v = &p->views[e];
                rt_submit_view_t *o = &sub.view[e];
                o->sc_priv = scs[e]->be_priv;
                o->image = scs[e]->images[scs[e]->last_released];
                o->sc_w = scs[e]->info.width;
                o->sc_h = scs[e]->info.height;
                o->format = scs[e]->info.format;
                o->rect = rects[e];
                o->tan[0] = tanf(-v->fov.angleLeft);
                o->tan[1] = tanf(v->fov.angleRight);
                o->tan[2] = tanf(v->fov.angleUp);
                o->tan[3] = tanf(-v->fov.angleDown);
                o->pose = lf ? pose_mul(layer_pose, v->pose) : v->pose;
            }
        } else if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            const XrCompositionLayerQuad *q = (const XrCompositionLayerQuad *)l;
            rt_swapchain *sc;
            XrResult r = check_sub_image(s, &q->subImage, &sc);
            if (XR_FAILED(r))
                return r;
            if (!rt_pose_ok(&q->pose))
                return XR_ERROR_POSE_INVALID;
            ignored++; /* TODO: composite quads into the eye buffer */
        } else {
            return XR_ERROR_LAYER_INVALID;
        }
    }

    s->frame_begun = 0;
    s->stats.frames_ended++;
    s->stats.layers_ignored += ignored;
    if (sub.has_projection) {
        s->stats.frames_submitted++;
        s->stats.last_layout = sub.layout;
        s->stats.last_src_w = (uint32_t)sub.view[0].rect.extent.width;
        s->stats.last_src_h = (uint32_t)sub.view[0].rect.extent.height;
    } else {
        s->stats.frames_empty++;
    }
    if (s->inst->be->submit(&sub, &s->stats) != 0)
        s->stats.backend_errors++;

    if (!s->frame_ended_once) {
        s->frame_ended_once = 1;
        if (!s->exit_requested) {
            rt_session_set_state(s, XR_SESSION_STATE_VISIBLE);
            if (s->inst->be->user_present())
                rt_session_set_state(s, XR_SESSION_STATE_FOCUSED);
        }
    }
    return XR_SUCCESS;
}
