/* runtime.c - instance, system, paths, events, dispatch and loader
 * negotiation for libopenxr_psvr2. */
#include "xr_internal.h"

#include <openxr/openxr_reflection.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -- logging ------------------------------------------------------------------ */
static int s_log_level = -1;

void rt_log(int level, const char *fmt, ...)
{
    if (s_log_level < 0) {
        const char *e = getenv("XR_PSVR2_LOG");
        s_log_level = e ? atoi(e) : 1;
    }
    if (level > s_log_level)
        return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[openxr_psvr2] %s\n", line);
}

/* -- lock ------------------------------------------------------------------------ */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

void rt_lock(void)
{
    pthread_mutex_lock(&s_lock);
}

void rt_unlock(void)
{
    pthread_mutex_unlock(&s_lock);
}

/* -- handles ---------------------------------------------------------------------- */
#define RT_MAX_HANDLES 4096
static struct {
    uint32_t gen;
    obj_type_t type;
    void *obj;
} s_slots[RT_MAX_HANDLES];
static uint32_t s_slot_hint;

uint64_t rt_handle_new(obj_type_t type, void *obj)
{
    for (uint32_t n = 0; n < RT_MAX_HANDLES; n++) {
        const uint32_t i = (s_slot_hint + n) % RT_MAX_HANDLES;
        if (s_slots[i].type != OBJ_FREE)
            continue;
        s_slots[i].gen++;
        if (s_slots[i].gen == 0)
            s_slots[i].gen = 1;
        s_slots[i].type = type;
        s_slots[i].obj = obj;
        s_slot_hint = i + 1;
        return ((uint64_t)s_slots[i].gen << 32) | (uint64_t)(i + 1);
    }
    return 0;
}

void *rt_handle_get(uint64_t h, obj_type_t type)
{
    const uint32_t idx = (uint32_t)(h & 0xffffffffu);
    const uint32_t gen = (uint32_t)(h >> 32);
    if (idx == 0 || idx > RT_MAX_HANDLES)
        return NULL;
    if (s_slots[idx - 1].type != type || s_slots[idx - 1].gen != gen)
        return NULL;
    return s_slots[idx - 1].obj;
}

void rt_handle_free(uint64_t h)
{
    const uint32_t idx = (uint32_t)(h & 0xffffffffu);
    if (idx == 0 || idx > RT_MAX_HANDLES)
        return;
    s_slots[idx - 1].type = OBJ_FREE;
    s_slots[idx - 1].obj = NULL;
}

rt_instance *rt_get_instance(XrInstance h)
{
    return rt_handle_get(RT_H(h), OBJ_INSTANCE);
}

rt_session *rt_get_session(XrSession h)
{
    return rt_handle_get(RT_H(h), OBJ_SESSION);
}

/* -- paths ---------------------------------------------------------------------------- */
/* Well-formed: "/" then components of [a-z0-9-_.], no empty, "." or ".."
 * components, no trailing slash. */
static int path_well_formed(const char *s)
{
    const size_t n = strlen(s);
    if (n < 2 || n >= XR_MAX_PATH_LENGTH || s[0] != '/' || s[n - 1] == '/')
        return 0;
    const char *comp = s + 1;
    for (const char *c = s + 1;; c++) {
        if (*c == '/' || *c == 0) {
            const size_t len = (size_t)(c - comp);
            if (len == 0)
                return 0;
            int dots = 1;
            for (const char *d = comp; d < c; d++)
                dots &= *d == '.';
            if (dots)
                return 0;
            if (*c == 0)
                break;
            comp = c + 1;
            continue;
        }
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' ||
              *c == '.'))
            return 0;
    }
    return 1;
}

XrResult rt_path_intern(rt_instance *inst, const char *s, XrPath *out)
{
    if (!s || !out)
        return XR_ERROR_VALIDATION_FAILURE;
    if (!path_well_formed(s))
        return XR_ERROR_PATH_FORMAT_INVALID;
    for (uint32_t i = 0; i < inst->n_paths; i++) {
        if (!strcmp(inst->paths[i], s)) {
            *out = (XrPath)(i + 1);
            return XR_SUCCESS;
        }
    }
    if (inst->n_paths == inst->cap_paths) {
        const uint32_t cap = inst->cap_paths ? inst->cap_paths * 2 : 64;
        if (cap > 65536)
            return XR_ERROR_PATH_COUNT_EXCEEDED;
        char **p = realloc(inst->paths, cap * sizeof *p);
        if (!p)
            return XR_ERROR_OUT_OF_MEMORY;
        inst->paths = p;
        inst->cap_paths = cap;
    }
    inst->paths[inst->n_paths] = strdup(s);
    if (!inst->paths[inst->n_paths])
        return XR_ERROR_OUT_OF_MEMORY;
    *out = (XrPath)(++inst->n_paths);
    return XR_SUCCESS;
}

int rt_path_is_valid(rt_instance *inst, XrPath p)
{
    return p != XR_NULL_PATH && p <= inst->n_paths;
}

const char *rt_path_str(rt_instance *inst, XrPath p)
{
    return rt_path_is_valid(inst, p) ? inst->paths[p - 1] : NULL;
}

int rt_hand_of_path(rt_instance *inst, XrPath p)
{
    if (p == inst->p_hand[HAND_LEFT])
        return HAND_LEFT;
    if (p == inst->p_hand[HAND_RIGHT])
        return HAND_RIGHT;
    return -1;
}

XRRT_CORE XrResult XRAPI_CALL xrStringToPath(XrInstance instance, const char *pathString, XrPath *path)
{
    RT_GUARD();
    rt_instance *inst = rt_get_instance(instance);
    if (!inst)
        return XR_ERROR_HANDLE_INVALID;
    return rt_path_intern(inst, pathString, path);
}

XRRT_CORE XrResult XRAPI_CALL xrPathToString(XrInstance instance, XrPath path, uint32_t bufferCapacityInput,
                                             uint32_t *bufferCountOutput, char *buffer)
{
    RT_GUARD();
    rt_instance *inst = rt_get_instance(instance);
    if (!inst)
        return XR_ERROR_HANDLE_INVALID;
    const char *s = rt_path_str(inst, path);
    if (!s)
        return XR_ERROR_PATH_INVALID;
    int copy;
    const uint32_t need = (uint32_t)strlen(s) + 1;
    XrResult r = rt_two_call(bufferCapacityInput, bufferCountOutput, need, buffer, &copy);
    if (copy)
        memcpy(buffer, s, need);
    return r;
}

/* -- events ------------------------------------------------------------------------------ */
void rt_event_push(rt_instance *inst, const XrEventDataBuffer *ev, XrSession session)
{
    if (inst->ev_count == RT_MAX_EVENTS) {
        inst->events_lost++;
        /* drop the oldest */
        inst->ev_head = (inst->ev_head + 1) % RT_MAX_EVENTS;
        inst->ev_count--;
    }
    const uint32_t slot = (inst->ev_head + inst->ev_count) % RT_MAX_EVENTS;
    inst->events[slot].ev = *ev;
    inst->events[slot].session = session;
    inst->ev_count++;
}

void rt_event_drop_session(rt_instance *inst, XrSession session)
{
    rt_event_t keep[RT_MAX_EVENTS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < inst->ev_count; i++) {
        const rt_event_t *e = &inst->events[(inst->ev_head + i) % RT_MAX_EVENTS];
        if (e->session != session)
            keep[n++] = *e;
    }
    for (uint32_t i = 0; i < n; i++)
        inst->events[i] = keep[i];
    inst->ev_head = 0;
    inst->ev_count = n;
}

void rt_session_set_state(rt_session *s, XrSessionState state)
{
    if (s->state == state)
        return;
    RT_INFO("session state %d -> %d", (int)s->state, (int)state);
    s->state = state;
    XrEventDataBuffer buf;
    memset(&buf, 0, sizeof buf);
    XrEventDataSessionStateChanged *e = (XrEventDataSessionStateChanged *)&buf;
    e->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    e->session = s->handle;
    e->state = state;
    e->time = s->inst->be->now();
    rt_event_push(s->inst, &buf, s->handle);
}

XRRT_CORE XrResult XRAPI_CALL xrPollEvent(XrInstance instance, XrEventDataBuffer *eventData)
{
    RT_GUARD();
    rt_instance *inst = rt_get_instance(instance);
    if (!inst)
        return XR_ERROR_HANDLE_INVALID;
    if (!eventData || eventData->type != XR_TYPE_EVENT_DATA_BUFFER)
        return XR_ERROR_VALIDATION_FAILURE;
    if (inst->events_lost) {
        XrEventDataEventsLost *e = (XrEventDataEventsLost *)eventData;
        memset(eventData, 0, sizeof *eventData);
        e->type = XR_TYPE_EVENT_DATA_EVENTS_LOST;
        e->lostEventCount = (uint32_t)inst->events_lost;
        inst->events_lost = 0;
        return XR_SUCCESS;
    }
    if (inst->ev_count == 0)
        return XR_EVENT_UNAVAILABLE;
    *eventData = inst->events[inst->ev_head].ev;
    inst->ev_head = (inst->ev_head + 1) % RT_MAX_EVENTS;
    inst->ev_count--;
    return XR_SUCCESS;
}

/* -- extensions ----------------------------------------------------------------------------- */
static const XrExtensionProperties kExtensions[] = {
    {XR_TYPE_EXTENSION_PROPERTIES, NULL, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
     XR_KHR_opengl_es_enable_SPEC_VERSION},
    {XR_TYPE_EXTENSION_PROPERTIES, NULL, XR_MNDX_EGL_ENABLE_EXTENSION_NAME,
     XR_MNDX_egl_enable_SPEC_VERSION},
    {XR_TYPE_EXTENSION_PROPERTIES, NULL, XR_EXT_LOCAL_FLOOR_EXTENSION_NAME,
     XR_EXT_local_floor_SPEC_VERSION},
    /* Advertised so WebKit's hand-tracking plumbing links up; the system
     * reports supportsHandTracking = XR_FALSE until PS VR2 hands are wired. */
    {XR_TYPE_EXTENSION_PROPERTIES, NULL, XR_EXT_HAND_TRACKING_EXTENSION_NAME,
     XR_EXT_hand_tracking_SPEC_VERSION},
};
#define N_EXTENSIONS (sizeof kExtensions / sizeof kExtensions[0])

XRRT_CORE XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(const char *layerName,
                                                                     uint32_t propertyCapacityInput,
                                                                     uint32_t *propertyCountOutput,
                                                                     XrExtensionProperties *properties)
{
    if (layerName)
        return XR_ERROR_API_LAYER_NOT_PRESENT;
    int copy;
    XrResult r = rt_two_call(propertyCapacityInput, propertyCountOutput, (uint32_t)N_EXTENSIONS,
                             properties, &copy);
    if (!copy)
        return r;
    for (uint32_t i = 0; i < N_EXTENSIONS; i++) {
        if (properties[i].type != XR_TYPE_EXTENSION_PROPERTIES)
            return XR_ERROR_VALIDATION_FAILURE;
        void *next = properties[i].next;
        properties[i] = kExtensions[i];
        properties[i].next = next;
    }
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrEnumerateApiLayerProperties(uint32_t propertyCapacityInput,
                                                            uint32_t *propertyCountOutput,
                                                            XrApiLayerProperties *properties)
{
    int copy;
    return rt_two_call(propertyCapacityInput, propertyCountOutput, 0, properties, &copy);
}

/* -- instance -------------------------------------------------------------------------------- */
static rt_instance *s_instance; /* one at a time: the backend is a singleton */

static const rt_backend_ops *pick_backend(void)
{
#ifdef XRRT_HAVE_PSVR2
    const char *e = getenv("XR_PSVR2_BACKEND");
    if (!(e && !strcmp(e, "mock")))
        return &rt_backend_psvr2;
#endif
    return &rt_backend_mock;
}

XRRT_CORE XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo *createInfo, XrInstance *instance)
{
    RT_GUARD();
    if (!createInfo || !instance || createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    const XrVersion v = createInfo->applicationInfo.apiVersion;
    if (XR_VERSION_MAJOR(v) != 1 || XR_VERSION_MINOR(v) > 1)
        return XR_ERROR_API_VERSION_UNSUPPORTED;
    if (createInfo->applicationInfo.applicationName[0] == 0)
        return XR_ERROR_NAME_INVALID;
    if (createInfo->enabledApiLayerCount > 0)
        return XR_ERROR_API_LAYER_NOT_PRESENT;
    if (createInfo->enabledExtensionCount && !createInfo->enabledExtensionNames)
        return XR_ERROR_VALIDATION_FAILURE;
    int gles = 0, egl = 0, floor_ = 0, hands = 0;
    for (uint32_t i = 0; i < createInfo->enabledExtensionCount; i++) {
        const char *name = createInfo->enabledExtensionNames[i];
        if (!name)
            return XR_ERROR_VALIDATION_FAILURE;
        if (!strcmp(name, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME))
            gles = 1;
        else if (!strcmp(name, XR_MNDX_EGL_ENABLE_EXTENSION_NAME))
            egl = 1;
        else if (!strcmp(name, XR_EXT_LOCAL_FLOOR_EXTENSION_NAME))
            floor_ = 1;
        else if (!strcmp(name, XR_EXT_HAND_TRACKING_EXTENSION_NAME))
            hands = 1;
        else {
            RT_ERR("xrCreateInstance: extension %s not present", name);
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    if (s_instance)
        return XR_ERROR_LIMIT_REACHED;
    rt_instance *inst = calloc(1, sizeof *inst);
    if (!inst)
        return XR_ERROR_OUT_OF_MEMORY;
    inst->api_version = v;
    snprintf(inst->app_name, sizeof inst->app_name, "%s", createInfo->applicationInfo.applicationName);
    inst->ext.gles = gles;
    inst->ext.egl = egl;
    inst->ext.local_floor = floor_;
    inst->ext.hand_tracking = hands;
    inst->be = pick_backend();
    inst->handle = (XrInstance)(uintptr_t)rt_handle_new(OBJ_INSTANCE, inst);
    if (!inst->handle) {
        free(inst);
        return XR_ERROR_LIMIT_REACHED;
    }
    rt_path_intern(inst, "/user/hand/left", &inst->p_hand[HAND_LEFT]);
    rt_path_intern(inst, "/user/hand/right", &inst->p_hand[HAND_RIGHT]);
    rt_path_intern(inst, "/user/head", &inst->p_head);
    rt_path_intern(inst, "/user/gamepad", &inst->p_gamepad);
    rt_path_intern(inst, "/interaction_profiles/khr/simple_controller", &inst->p_simple_profile);
    s_instance = inst;
    *instance = inst->handle;
    RT_INFO("instance for \"%s\" (API %u.%u), backend %s, gles %d egl %d local_floor %d hands %d",
            inst->app_name, XR_VERSION_MAJOR(v), XR_VERSION_MINOR(v), inst->be->name, gles, egl,
            floor_, hands);
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrDestroyInstance(XrInstance instance)
{
    RT_GUARD();
    rt_instance *inst = rt_get_instance(instance);
    if (!inst)
        return XR_ERROR_HANDLE_INVALID;
    if (inst->session)
        rt_session_destroy_locked(inst->session);
    while (inst->sets)
        rt_action_set_destroy_locked(inst->sets);
    for (rt_action *a = inst->actions, *n; a; a = n) {
        n = a->next_all;
        free(a);
    }
    for (uint32_t i = 0; i < inst->n_suggested; i++)
        free(inst->suggested[i].bindings);
    for (uint32_t i = 0; i < inst->n_paths; i++)
        free(inst->paths[i]);
    free(inst->paths);
    if (inst->backend_open)
        inst->be->close();
    rt_handle_free(RT_H(inst->handle));
    if (s_instance == inst)
        s_instance = NULL;
    free(inst);
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrGetInstanceProperties(XrInstance instance,
                                                      XrInstanceProperties *instanceProperties)
{
    RT_GUARD();
    rt_instance *inst = rt_get_instance(instance);
    if (!inst)
        return XR_ERROR_HANDLE_INVALID;
    if (!instanceProperties || instanceProperties->type != XR_TYPE_INSTANCE_PROPERTIES)
        return XR_ERROR_VALIDATION_FAILURE;
    instanceProperties->runtimeVersion = RT_RUNTIME_VERSION;
    snprintf(instanceProperties->runtimeName, sizeof instanceProperties->runtimeName, "%s (%s)",
             RT_RUNTIME_NAME, inst->be->name);
    return XR_SUCCESS;
}

/* -- result / structure strings --------------------------------------------------------------- */
#define RT_ENUM_CASE(name, val)                                                                    \
    case name:                                                                                     \
        s = #name;                                                                                 \
        break;

XRRT_CORE XrResult XRAPI_CALL xrResultToString(XrInstance instance, XrResult value,
                                               char buffer[XR_MAX_RESULT_STRING_SIZE])
{
    RT_GUARD();
    if (!rt_get_instance(instance))
        return XR_ERROR_HANDLE_INVALID;
    if (!buffer)
        return XR_ERROR_VALIDATION_FAILURE;
    const char *s = NULL;
    switch (value) {
        XR_LIST_ENUM_XrResult(RT_ENUM_CASE)
    default:
        break;
    }
    if (s)
        snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "%s", s);
    else
        snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "XR_UNKNOWN_%s_%d",
                 value >= 0 ? "SUCCESS" : "FAILURE", (int)value);
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrStructureTypeToString(XrInstance instance, XrStructureType value,
                                                      char buffer[XR_MAX_STRUCTURE_NAME_SIZE])
{
    RT_GUARD();
    if (!rt_get_instance(instance))
        return XR_ERROR_HANDLE_INVALID;
    if (!buffer)
        return XR_ERROR_VALIDATION_FAILURE;
    const char *s = NULL;
    switch (value) {
        XR_LIST_ENUM_XrStructureType(RT_ENUM_CASE)
    default:
        break;
    }
    if (s)
        snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "%s", s);
    else
        snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "XR_UNKNOWN_STRUCTURE_TYPE_%d", (int)value);
    return XR_SUCCESS;
}

/* -- system ---------------------------------------------------------------------------------- */
XRRT_CORE XrResult XRAPI_CALL xrGetSystem(XrInstance instance, const XrSystemGetInfo *getInfo,
                                          XrSystemId *systemId)
{
    RT_GUARD();
    rt_instance *inst = rt_get_instance(instance);
    if (!inst)
        return XR_ERROR_HANDLE_INVALID;
    if (!getInfo || !systemId || getInfo->type != XR_TYPE_SYSTEM_GET_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    if (getInfo->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
        if (getInfo->formFactor == XR_FORM_FACTOR_HANDHELD_DISPLAY)
            return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (!inst->backend_open) {
        memset(&inst->info, 0, sizeof inst->info);
        if (inst->be->open(&inst->info) != 0) {
            RT_ERR("backend %s: no headset", inst->be->name);
            return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
        }
        inst->backend_open = 1;
        RT_INFO("system \"%s\": %ux%u per eye, tangents L(%.3f %.3f %.3f %.3f) R(%.3f %.3f %.3f %.3f)",
                inst->info.system_name, inst->info.rec_w, inst->info.rec_h,
                (double)inst->info.tan[0][0], (double)inst->info.tan[0][1],
                (double)inst->info.tan[0][2], (double)inst->info.tan[0][3],
                (double)inst->info.tan[1][0], (double)inst->info.tan[1][1],
                (double)inst->info.tan[1][2], (double)inst->info.tan[1][3]);
    }
    inst->system_fetched = 1;
    *systemId = RT_SYSTEM_ID;
    return XR_SUCCESS;
}

static rt_instance *instance_system(XrInstance instance, XrSystemId systemId, XrResult *err)
{
    rt_instance *inst = rt_get_instance(instance);
    if (!inst) {
        *err = XR_ERROR_HANDLE_INVALID;
        return NULL;
    }
    if (systemId != RT_SYSTEM_ID || !inst->system_fetched) {
        *err = XR_ERROR_SYSTEM_INVALID;
        return NULL;
    }
    return inst;
}

XRRT_CORE XrResult XRAPI_CALL xrGetSystemProperties(XrInstance instance, XrSystemId systemId,
                                                    XrSystemProperties *properties)
{
    RT_GUARD();
    XrResult err;
    rt_instance *inst = instance_system(instance, systemId, &err);
    if (!inst)
        return err;
    if (!properties || properties->type != XR_TYPE_SYSTEM_PROPERTIES)
        return XR_ERROR_VALIDATION_FAILURE;
    properties->systemId = systemId;
    properties->vendorId = inst->info.vendor_id;
    snprintf(properties->systemName, sizeof properties->systemName, "%s", inst->info.system_name);
    properties->graphicsProperties.maxSwapchainImageWidth = 2 * inst->info.max_w;
    properties->graphicsProperties.maxSwapchainImageHeight = inst->info.max_h;
    properties->graphicsProperties.maxLayerCount = RT_MAX_LAYERS;
    properties->trackingProperties.orientationTracking = XR_TRUE;
    properties->trackingProperties.positionTracking = inst->info.position_tracking ? XR_TRUE : XR_FALSE;
    for (XrBaseOutStructure *n = (XrBaseOutStructure *)properties->next; n; n = n->next) {
        if (n->type == XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT)
            ((XrSystemHandTrackingPropertiesEXT *)n)->supportsHandTracking = XR_FALSE;
    }
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrEnumerateViewConfigurations(XrInstance instance, XrSystemId systemId,
                                                            uint32_t viewConfigurationTypeCapacityInput,
                                                            uint32_t *viewConfigurationTypeCountOutput,
                                                            XrViewConfigurationType *viewConfigurationTypes)
{
    RT_GUARD();
    XrResult err;
    if (!instance_system(instance, systemId, &err))
        return err;
    int copy;
    XrResult r = rt_two_call(viewConfigurationTypeCapacityInput, viewConfigurationTypeCountOutput, 1,
                             viewConfigurationTypes, &copy);
    if (copy)
        viewConfigurationTypes[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    return r;
}

XRRT_CORE XrResult XRAPI_CALL xrGetViewConfigurationProperties(XrInstance instance, XrSystemId systemId,
                                                               XrViewConfigurationType viewConfigurationType,
                                                               XrViewConfigurationProperties *props)
{
    RT_GUARD();
    XrResult err;
    if (!instance_system(instance, systemId, &err))
        return err;
    if (!props || props->type != XR_TYPE_VIEW_CONFIGURATION_PROPERTIES)
        return XR_ERROR_VALIDATION_FAILURE;
    if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    props->viewConfigurationType = viewConfigurationType;
    /* psvr2_submit takes the tangents each frame was rendered with. */
    props->fovMutable = XR_TRUE;
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(XrInstance instance, XrSystemId systemId,
                                                                XrViewConfigurationType viewConfigurationType,
                                                                uint32_t viewCapacityInput,
                                                                uint32_t *viewCountOutput,
                                                                XrViewConfigurationView *views)
{
    RT_GUARD();
    XrResult err;
    rt_instance *inst = instance_system(instance, systemId, &err);
    if (!inst)
        return err;
    if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    int copy;
    XrResult r = rt_two_call(viewCapacityInput, viewCountOutput, RT_VIEW_COUNT, views, &copy);
    if (!copy)
        return r;
    for (uint32_t i = 0; i < RT_VIEW_COUNT; i++) {
        if (views[i].type != XR_TYPE_VIEW_CONFIGURATION_VIEW)
            return XR_ERROR_VALIDATION_FAILURE;
        views[i].recommendedImageRectWidth = inst->info.rec_w;
        views[i].recommendedImageRectHeight = inst->info.rec_h;
        views[i].maxImageRectWidth = inst->info.max_w;
        views[i].maxImageRectHeight = inst->info.max_h;
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrEnumerateEnvironmentBlendModes(XrInstance instance, XrSystemId systemId,
                                                               XrViewConfigurationType viewConfigurationType,
                                                               uint32_t environmentBlendModeCapacityInput,
                                                               uint32_t *environmentBlendModeCountOutput,
                                                               XrEnvironmentBlendMode *environmentBlendModes)
{
    RT_GUARD();
    XrResult err;
    if (!instance_system(instance, systemId, &err))
        return err;
    if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    int copy;
    XrResult r = rt_two_call(environmentBlendModeCapacityInput, environmentBlendModeCountOutput, 1,
                             environmentBlendModes, &copy);
    if (copy)
        environmentBlendModes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return r;
}

/* -- XR_KHR_opengl_es_enable ------------------------------------------------------------------- */
XrResult rt_xrGetOpenGLESGraphicsRequirementsKHR(XrInstance instance, XrSystemId systemId,
                                                 XrGraphicsRequirementsOpenGLESKHR *req)
{
    RT_GUARD();
    XrResult err;
    rt_instance *inst = instance_system(instance, systemId, &err);
    if (!inst)
        return err;
    if (!req || req->type != XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR)
        return XR_ERROR_VALIDATION_FAILURE;
    /* ES 3.0 for glTexStorage2D / glBlitFramebuffer; ps5-opengl offers desktop
     * 4.6, which covers everything the PS5 backend calls. */
    req->minApiVersionSupported = XR_MAKE_VERSION(3, 0, 0);
    req->maxApiVersionSupported = XR_MAKE_VERSION(3, 2, 0);
    inst->gfx_requirements_queried = 1;
    return XR_SUCCESS;
}

/* -- XR_EXT_hand_tracking (stub: the system reports no support) ---------------------------------- */
XrResult rt_xrCreateHandTrackerEXT(XrSession session, const XrHandTrackerCreateInfoEXT *createInfo,
                                   XrHandTrackerEXT *handTracker)
{
    RT_GUARD();
    if (!rt_get_session(session))
        return XR_ERROR_HANDLE_INVALID;
    if (!createInfo || !handTracker || createInfo->type != XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT)
        return XR_ERROR_VALIDATION_FAILURE;
    /* TODO(PS VR2 hands): PS5VR drives libSceVrHand with the headset cameras. */
    return XR_ERROR_FEATURE_UNSUPPORTED;
}

XrResult rt_xrDestroyHandTrackerEXT(XrHandTrackerEXT handTracker)
{
    RT_GUARD();
    return rt_handle_get(RT_H(handTracker), OBJ_HAND_TRACKER) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

XrResult rt_xrLocateHandJointsEXT(XrHandTrackerEXT handTracker, const XrHandJointsLocateInfoEXT *locateInfo,
                                  XrHandJointLocationsEXT *locations)
{
    RT_GUARD();
    (void)locateInfo;
    if (!rt_handle_get(RT_H(handTracker), OBJ_HAND_TRACKER))
        return XR_ERROR_HANDLE_INVALID;
    if (locations)
        locations->isActive = XR_FALSE;
    return XR_SUCCESS;
}

/* -- private: statistics ------------------------------------------------------------------------- */
static XrResult rt_xrGetRuntimeStatsPSVR2(XrSession session, rt_stats_t *stats)
{
    RT_GUARD();
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    if (!stats)
        return XR_ERROR_VALIDATION_FAILURE;
    *stats = s->stats;
    return XR_SUCCESS;
}

/* -- dispatch ------------------------------------------------------------------------------------ */
typedef struct {
    const char *name;
    PFN_xrVoidFunction fn;
    int ext; /* 0 core, 1 gles, 2 hand tracking, 3 private */
} rt_proc_t;

#define CORE(n) {#n, (PFN_xrVoidFunction)n, 0}
static const rt_proc_t kProcs[] = {
    CORE(xrGetInstanceProcAddr),
    CORE(xrEnumerateApiLayerProperties),
    CORE(xrEnumerateInstanceExtensionProperties),
    CORE(xrCreateInstance),
    CORE(xrDestroyInstance),
    CORE(xrGetInstanceProperties),
    CORE(xrPollEvent),
    CORE(xrResultToString),
    CORE(xrStructureTypeToString),
    CORE(xrGetSystem),
    CORE(xrGetSystemProperties),
    CORE(xrEnumerateEnvironmentBlendModes),
    CORE(xrCreateSession),
    CORE(xrDestroySession),
    CORE(xrEnumerateReferenceSpaces),
    CORE(xrCreateReferenceSpace),
    CORE(xrGetReferenceSpaceBoundsRect),
    CORE(xrCreateActionSpace),
    CORE(xrLocateSpace),
    CORE(xrLocateSpaces),
    CORE(xrDestroySpace),
    CORE(xrEnumerateViewConfigurations),
    CORE(xrGetViewConfigurationProperties),
    CORE(xrEnumerateViewConfigurationViews),
    CORE(xrEnumerateSwapchainFormats),
    CORE(xrCreateSwapchain),
    CORE(xrDestroySwapchain),
    CORE(xrEnumerateSwapchainImages),
    CORE(xrAcquireSwapchainImage),
    CORE(xrWaitSwapchainImage),
    CORE(xrReleaseSwapchainImage),
    CORE(xrBeginSession),
    CORE(xrEndSession),
    CORE(xrRequestExitSession),
    CORE(xrWaitFrame),
    CORE(xrBeginFrame),
    CORE(xrEndFrame),
    CORE(xrLocateViews),
    CORE(xrStringToPath),
    CORE(xrPathToString),
    CORE(xrCreateActionSet),
    CORE(xrDestroyActionSet),
    CORE(xrCreateAction),
    CORE(xrDestroyAction),
    CORE(xrSuggestInteractionProfileBindings),
    CORE(xrAttachSessionActionSets),
    CORE(xrGetCurrentInteractionProfile),
    CORE(xrGetActionStateBoolean),
    CORE(xrGetActionStateFloat),
    CORE(xrGetActionStateVector2f),
    CORE(xrGetActionStatePose),
    CORE(xrSyncActions),
    CORE(xrEnumerateBoundSourcesForAction),
    CORE(xrGetInputSourceLocalizedName),
    CORE(xrApplyHapticFeedback),
    CORE(xrStopHapticFeedback),
    {"xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction)rt_xrGetOpenGLESGraphicsRequirementsKHR, 1},
    {"xrCreateHandTrackerEXT", (PFN_xrVoidFunction)rt_xrCreateHandTrackerEXT, 2},
    {"xrDestroyHandTrackerEXT", (PFN_xrVoidFunction)rt_xrDestroyHandTrackerEXT, 2},
    {"xrLocateHandJointsEXT", (PFN_xrVoidFunction)rt_xrLocateHandJointsEXT, 2},
    {"xrGetRuntimeStatsPSVR2", (PFN_xrVoidFunction)rt_xrGetRuntimeStatsPSVR2, 3},
};
#undef CORE

XRRT_VISIBLE XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char *name,
                                                       PFN_xrVoidFunction *function)
{
    if (!name || !function)
        return XR_ERROR_VALIDATION_FAILURE;
    *function = NULL;
    rt_instance *inst = NULL;
    if (instance == XR_NULL_HANDLE) {
        if (strcmp(name, "xrEnumerateInstanceExtensionProperties") &&
            strcmp(name, "xrEnumerateApiLayerProperties") && strcmp(name, "xrCreateInstance"))
            return XR_ERROR_HANDLE_INVALID;
    } else {
        rt_lock();
        inst = rt_get_instance(instance);
        rt_unlock();
        if (!inst)
            return XR_ERROR_HANDLE_INVALID;
    }
    for (size_t i = 0; i < sizeof kProcs / sizeof kProcs[0]; i++) {
        if (strcmp(kProcs[i].name, name))
            continue;
        if (kProcs[i].ext == 1 && !(inst && inst->ext.gles))
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        if (kProcs[i].ext == 2 && !(inst && inst->ext.hand_tracking))
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        if (kProcs[i].ext == 3 && !inst)
            return XR_ERROR_HANDLE_INVALID;
        *function = kProcs[i].fn;
        return XR_SUCCESS;
    }
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

/* -- loader negotiation ----------------------------------------------------------------------------- */
XRRT_VISIBLE XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo *loaderInfo,
                                                                   XrNegotiateRuntimeRequest *runtimeRequest)
{
    if (!loaderInfo || !runtimeRequest ||
        loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo) ||
        runtimeRequest->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
        runtimeRequest->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION ||
        runtimeRequest->structSize != sizeof(XrNegotiateRuntimeRequest))
        return XR_ERROR_INITIALIZATION_FAILED;
    if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    /* We implement 1.0 and 1.1 (major.minor); the loader passes ranges. */
    const XrVersion ours = XR_MAKE_VERSION(1, 1, XR_VERSION_PATCH(XR_CURRENT_API_VERSION));
    if (XR_VERSION_MAJOR(loaderInfo->minApiVersion) > 1 || XR_VERSION_MAJOR(loaderInfo->maxApiVersion) < 1)
        return XR_ERROR_INITIALIZATION_FAILED;
    runtimeRequest->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    runtimeRequest->runtimeApiVersion = loaderInfo->maxApiVersion < ours ? loaderInfo->maxApiVersion : ours;
    runtimeRequest->getInstanceProcAddr = xrGetInstanceProcAddr;
    RT_INFO("negotiated with loader: interface %u, API %u.%u.%u", XR_CURRENT_LOADER_RUNTIME_VERSION,
            XR_VERSION_MAJOR(runtimeRequest->runtimeApiVersion),
            XR_VERSION_MINOR(runtimeRequest->runtimeApiVersion),
            XR_VERSION_PATCH(runtimeRequest->runtimeApiVersion));
    return XR_SUCCESS;
}

#ifndef XRRT_HAVE_PSVR2
XRRT_VISIBLE int xr_psvr2_preload(void)
{
    return 0;
}
#endif
