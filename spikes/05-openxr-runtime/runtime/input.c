/* input.c - action sets, actions, bindings and action state for
 * libopenxr_psvr2.
 *
 * The one interaction profile that drives state is
 * /interaction_profiles/khr/simple_controller: WebKit's OpenXRInputMappings.h
 * maps it (plus ext/hand_interaction_ext, which needs XR_EXT_hand_interaction
 * and is not offered). Suggestions for other spec profiles are accepted and
 * ignored. Controller data comes from the backend (rt_controller_t); the mock
 * reports none unless XR_PSVR2_MOCK_CONTROLLERS=1, psvr2 has none yet.
 */
#include "xr_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Spec-defined (1.0 + 1.1 core) profiles: suggestions are accepted. */
static const char *const kKnownProfiles[] = {
    "/interaction_profiles/khr/simple_controller",
    "/interaction_profiles/google/daydream_controller",
    "/interaction_profiles/htc/vive_controller",
    "/interaction_profiles/htc/vive_pro",
    "/interaction_profiles/microsoft/motion_controller",
    "/interaction_profiles/microsoft/xbox_controller",
    "/interaction_profiles/oculus/go_controller",
    "/interaction_profiles/oculus/touch_controller",
    "/interaction_profiles/valve/index_controller",
    "/interaction_profiles/bytedance/pico4_controller",
    "/interaction_profiles/bytedance/pico_neo3_controller",
    "/interaction_profiles/bytedance/pico_g3_controller",
    "/interaction_profiles/htc/vive_cosmos_controller",
    "/interaction_profiles/htc/vive_focus3_controller",
    "/interaction_profiles/meta/touch_pro_controller",
    "/interaction_profiles/meta/touch_plus_controller",
    "/interaction_profiles/meta/touch_controller_rift_cv1",
    "/interaction_profiles/meta/touch_controller_quest_1_rift_s",
    "/interaction_profiles/meta/touch_controller_quest_2",
    "/interaction_profiles/samsung/odyssey_controller",
    "/interaction_profiles/hp/mixed_reality_controller",
    "/interaction_profiles/logitech/mx_ink_stylus_logitech",
};

/* simple_controller sources (with and without the component). */
static const struct {
    const char *suffix;
    rt_source_t src;
} kSimpleSources[] = {
    {"/input/select/click", SRC_SELECT_CLICK}, {"/input/select", SRC_SELECT_CLICK},
    {"/input/menu/click", SRC_MENU_CLICK},     {"/input/menu", SRC_MENU_CLICK},
    {"/input/grip/pose", SRC_GRIP_POSE},       {"/input/grip", SRC_GRIP_POSE},
    {"/input/aim/pose", SRC_AIM_POSE},         {"/input/aim", SRC_AIM_POSE},
    {"/output/haptic", SRC_HAPTIC},
};

static int name_ok(const char *s, size_t max)
{
    const size_t n = strnlen(s, max);
    if (n == 0 || n == max)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-' ||
              s[i] == '_' || s[i] == '.'))
            return 0;
    return 1;
}

static int localized_ok(const char *s, size_t max)
{
    const size_t n = strnlen(s, max);
    return n > 0 && n < max;
}

/* -- action sets ------------------------------------------------------------------------- */
XRRT_CORE XrResult XRAPI_CALL xrCreateActionSet(XrInstance instance, const XrActionSetCreateInfo *createInfo,
                                                XrActionSet *actionSet)
{
    RT_GUARD();
    rt_instance *inst = rt_get_instance(instance);
    if (!inst)
        return XR_ERROR_HANDLE_INVALID;
    if (!createInfo || !actionSet || createInfo->type != XR_TYPE_ACTION_SET_CREATE_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    if (!name_ok(createInfo->actionSetName, XR_MAX_ACTION_SET_NAME_SIZE))
        return XR_ERROR_PATH_FORMAT_INVALID;
    if (!localized_ok(createInfo->localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE))
        return XR_ERROR_LOCALIZED_NAME_INVALID;
    for (rt_action_set *s = inst->sets; s; s = s->next) {
        if (!strcmp(s->name, createInfo->actionSetName))
            return XR_ERROR_NAME_DUPLICATED;
        if (!strcmp(s->localized, createInfo->localizedActionSetName))
            return XR_ERROR_LOCALIZED_NAME_DUPLICATED;
    }
    rt_action_set *set = calloc(1, sizeof *set);
    if (!set)
        return XR_ERROR_OUT_OF_MEMORY;
    set->inst = inst;
    snprintf(set->name, sizeof set->name, "%s", createInfo->actionSetName);
    snprintf(set->localized, sizeof set->localized, "%s", createInfo->localizedActionSetName);
    set->priority = createInfo->priority;
    set->handle = (XrActionSet)(uintptr_t)rt_handle_new(OBJ_ACTION_SET, set);
    set->next = inst->sets;
    inst->sets = set;
    *actionSet = set->handle;
    return XR_SUCCESS;
}

void rt_action_set_destroy_locked(rt_action_set *set)
{
    rt_instance *inst = set->inst;
    for (rt_action *a = inst->actions; a; a = a->next_all) {
        if (a->set != set || !a->alive)
            continue;
        /* The struct stays until the instance goes: action spaces may hold it. */
        rt_handle_free(RT_H(a->handle));
        a->alive = 0;
        a->set = NULL;
    }
    rt_session *s = inst->session;
    if (s) {
        for (uint32_t i = 0; i < s->n_attached; i++)
            if (s->attached[i] == set)
                s->attached[i] = NULL;
    }
    for (rt_action_set **pp = &inst->sets; *pp; pp = &(*pp)->next) {
        if (*pp == set) {
            *pp = set->next;
            break;
        }
    }
    rt_handle_free(RT_H(set->handle));
    free(set);
}

XRRT_CORE XrResult XRAPI_CALL xrDestroyActionSet(XrActionSet actionSet)
{
    RT_GUARD();
    rt_action_set *set = rt_handle_get(RT_H(actionSet), OBJ_ACTION_SET);
    if (!set)
        return XR_ERROR_HANDLE_INVALID;
    rt_action_set_destroy_locked(set);
    return XR_SUCCESS;
}

/* -- actions ------------------------------------------------------------------------------- */
XRRT_CORE XrResult XRAPI_CALL xrCreateAction(XrActionSet actionSet, const XrActionCreateInfo *createInfo,
                                             XrAction *action)
{
    RT_GUARD();
    rt_action_set *set = rt_handle_get(RT_H(actionSet), OBJ_ACTION_SET);
    if (!set)
        return XR_ERROR_HANDLE_INVALID;
    if (!createInfo || !action || createInfo->type != XR_TYPE_ACTION_CREATE_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    if (set->attached)
        return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    switch (createInfo->actionType) {
    case XR_ACTION_TYPE_BOOLEAN_INPUT:
    case XR_ACTION_TYPE_FLOAT_INPUT:
    case XR_ACTION_TYPE_VECTOR2F_INPUT:
    case XR_ACTION_TYPE_POSE_INPUT:
    case XR_ACTION_TYPE_VIBRATION_OUTPUT:
        break;
    default:
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (!name_ok(createInfo->actionName, XR_MAX_ACTION_NAME_SIZE))
        return XR_ERROR_PATH_FORMAT_INVALID;
    if (!localized_ok(createInfo->localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE))
        return XR_ERROR_LOCALIZED_NAME_INVALID;
    rt_instance *inst = set->inst;
    for (rt_action *a = inst->actions; a; a = a->next_all) {
        if (a->set != set || !a->alive)
            continue;
        if (!strcmp(a->name, createInfo->actionName))
            return XR_ERROR_NAME_DUPLICATED;
        if (!strcmp(a->localized, createInfo->localizedActionName))
            return XR_ERROR_LOCALIZED_NAME_DUPLICATED;
    }
    if (createInfo->countSubactionPaths > RT_MAX_SUBACTIONS ||
        (createInfo->countSubactionPaths && !createInfo->subactionPaths))
        return XR_ERROR_PATH_UNSUPPORTED;
    for (uint32_t i = 0; i < createInfo->countSubactionPaths; i++) {
        const XrPath p = createInfo->subactionPaths[i];
        if (!rt_path_is_valid(inst, p))
            return XR_ERROR_PATH_INVALID;
        if (p != inst->p_hand[0] && p != inst->p_hand[1] && p != inst->p_head && p != inst->p_gamepad)
            return XR_ERROR_PATH_UNSUPPORTED;
        for (uint32_t j = 0; j < i; j++)
            if (createInfo->subactionPaths[j] == p)
                return XR_ERROR_PATH_UNSUPPORTED;
    }
    rt_action *a = calloc(1, sizeof *a);
    if (!a)
        return XR_ERROR_OUT_OF_MEMORY;
    a->set = set;
    a->alive = 1;
    snprintf(a->name, sizeof a->name, "%s", createInfo->actionName);
    snprintf(a->localized, sizeof a->localized, "%s", createInfo->localizedActionName);
    a->type = createInfo->actionType;
    a->n_sub = createInfo->countSubactionPaths;
    for (uint32_t i = 0; i < a->n_sub; i++) {
        a->sub[i] = createInfo->subactionPaths[i];
        a->sub_hand[i] = rt_hand_of_path(inst, a->sub[i]);
    }
    a->handle = (XrAction)(uintptr_t)rt_handle_new(OBJ_ACTION, a);
    a->next_all = inst->actions;
    inst->actions = a;
    *action = a->handle;
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrDestroyAction(XrAction action)
{
    RT_GUARD();
    rt_action *a = rt_handle_get(RT_H(action), OBJ_ACTION);
    if (!a)
        return XR_ERROR_HANDLE_INVALID;
    rt_handle_free(RT_H(a->handle));
    a->alive = 0;
    return XR_SUCCESS;
}

/* -- bindings ----------------------------------------------------------------------------------- */
/* Parse "/user/hand/<left|right><suffix>" for simple_controller. */
static int simple_source(rt_instance *inst, XrPath p, int *hand, rt_source_t *src)
{
    const char *s = rt_path_str(inst, p);
    if (!s)
        return 0;
    const char *rest;
    if (!strncmp(s, "/user/hand/left/", 16)) {
        *hand = HAND_LEFT;
        rest = s + 15;
    } else if (!strncmp(s, "/user/hand/right/", 17)) {
        *hand = HAND_RIGHT;
        rest = s + 16;
    } else {
        return 0;
    }
    for (size_t i = 0; i < sizeof kSimpleSources / sizeof kSimpleSources[0]; i++) {
        if (!strcmp(rest, kSimpleSources[i].suffix)) {
            *src = kSimpleSources[i].src;
            return 1;
        }
    }
    return 0;
}

static int source_fits(XrActionType t, rt_source_t src)
{
    switch (src) {
    case SRC_SELECT_CLICK:
    case SRC_MENU_CLICK:
        return t == XR_ACTION_TYPE_BOOLEAN_INPUT || t == XR_ACTION_TYPE_FLOAT_INPUT;
    case SRC_GRIP_POSE:
    case SRC_AIM_POSE:
        return t == XR_ACTION_TYPE_POSE_INPUT;
    case SRC_HAPTIC:
        return t == XR_ACTION_TYPE_VIBRATION_OUTPUT;
    default:
        return 0;
    }
}

XRRT_CORE XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(XrInstance instance,
                                                                  const XrInteractionProfileSuggestedBinding *suggestedBindings)
{
    RT_GUARD();
    rt_instance *inst = rt_get_instance(instance);
    if (!inst)
        return XR_ERROR_HANDLE_INVALID;
    const XrInteractionProfileSuggestedBinding *sb = suggestedBindings;
    if (!sb || sb->type != XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING || sb->countSuggestedBindings == 0 ||
        !sb->suggestedBindings)
        return XR_ERROR_VALIDATION_FAILURE;
    const char *profile = rt_path_str(inst, sb->interactionProfile);
    if (!profile)
        return XR_ERROR_PATH_INVALID;
    int known = 0;
    for (size_t i = 0; i < sizeof kKnownProfiles / sizeof kKnownProfiles[0]; i++)
        known |= !strcmp(profile, kKnownProfiles[i]);
    if (!known)
        return XR_ERROR_PATH_UNSUPPORTED;
    for (rt_action_set *set = inst->sets; set; set = set->next)
        if (set->attached)
            return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    const int simple = sb->interactionProfile == inst->p_simple_profile;
    for (uint32_t i = 0; i < sb->countSuggestedBindings; i++) {
        rt_action *a = rt_handle_get(RT_H(sb->suggestedBindings[i].action), OBJ_ACTION);
        if (!a)
            return XR_ERROR_HANDLE_INVALID;
        if (!rt_path_is_valid(inst, sb->suggestedBindings[i].binding))
            return XR_ERROR_PATH_INVALID;
        if (simple) {
            int hand;
            rt_source_t src;
            if (!simple_source(inst, sb->suggestedBindings[i].binding, &hand, &src) ||
                !source_fits(a->type, src)) {
                RT_ERR("simple_controller: unsupported binding %s for action %s",
                       rt_path_str(inst, sb->suggestedBindings[i].binding), a->name);
                return XR_ERROR_PATH_UNSUPPORTED;
            }
        }
    }
    /* A later suggestion for the same profile replaces the earlier one. */
    rt_suggestion_t *slot = NULL;
    for (uint32_t i = 0; i < inst->n_suggested; i++)
        if (inst->suggested[i].profile == sb->interactionProfile)
            slot = &inst->suggested[i];
    if (!slot) {
        if (inst->n_suggested == RT_MAX_SUGGESTED)
            return XR_ERROR_LIMIT_REACHED;
        slot = &inst->suggested[inst->n_suggested++];
        slot->profile = sb->interactionProfile;
    }
    free(slot->bindings);
    slot->bindings = malloc(sb->countSuggestedBindings * sizeof *slot->bindings);
    if (!slot->bindings) {
        slot->count = 0;
        return XR_ERROR_OUT_OF_MEMORY;
    }
    memcpy(slot->bindings, sb->suggestedBindings, sb->countSuggestedBindings * sizeof *slot->bindings);
    slot->count = sb->countSuggestedBindings;
    return XR_SUCCESS;
}

void rt_input_attach_resolve(rt_session *s)
{
    rt_instance *inst = s->inst;
    const rt_suggestion_t *simple = NULL;
    for (uint32_t i = 0; i < inst->n_suggested; i++)
        if (inst->suggested[i].profile == inst->p_simple_profile)
            simple = &inst->suggested[i];
    for (rt_action *a = inst->actions; a; a = a->next_all) {
        if (!a->alive || !a->set || !a->set->attached)
            continue;
        a->n_bind = 0;
        memset(a->state, 0, sizeof a->state);
        for (uint32_t i = 0; simple && i < simple->count; i++) {
            if (simple->bindings[i].action != a->handle || a->n_bind == RT_MAX_ACTION_BINDINGS)
                continue;
            rt_binding_t *b = &a->bind[a->n_bind];
            if (!simple_source(inst, simple->bindings[i].binding, &b->hand, &b->src))
                continue;
            /* A subaction-filtered action only sees its own hands. */
            int allowed = a->n_sub == 0;
            for (uint32_t k = 0; k < a->n_sub; k++)
                allowed |= a->sub_hand[k] == b->hand;
            if (!allowed)
                continue;
            b->path = simple->bindings[i].binding;
            a->n_bind++;
        }
    }
}

/* The current profile per hand: simple_controller while that hand's
 * controller is active and something is bound, else none. Changes are
 * reported with XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED. */
void rt_input_sync_profiles(rt_session *s)
{
    rt_instance *inst = s->inst;
    int bound = 0;
    for (uint32_t i = 0; i < inst->n_suggested; i++)
        bound |= inst->suggested[i].profile == inst->p_simple_profile;
    int changed = 0;
    for (int h = 0; h < HAND_COUNT; h++) {
        rt_controller_t c;
        memset(&c, 0, sizeof c);
        inst->be->controller(h, s->last_display_time > 0 ? s->last_display_time : inst->be->now(), &c);
        const XrPath want = (c.active && bound) ? inst->p_simple_profile : XR_NULL_PATH;
        if (want != s->current_profile[h]) {
            s->current_profile[h] = want;
            changed = 1;
        }
    }
    if (changed) {
        XrEventDataBuffer buf;
        memset(&buf, 0, sizeof buf);
        XrEventDataInteractionProfileChanged *e = (XrEventDataInteractionProfileChanged *)&buf;
        e->type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED;
        e->session = s->handle;
        rt_event_push(inst, &buf, s->handle);
    }
}

XRRT_CORE XrResult XRAPI_CALL xrAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo *attachInfo)
{
    RT_GUARD();
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    if (!attachInfo || attachInfo->type != XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO ||
        attachInfo->countActionSets == 0 || !attachInfo->actionSets)
        return XR_ERROR_VALIDATION_FAILURE;
    if (s->sets_attached)
        return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    if (attachInfo->countActionSets > RT_MAX_ATTACHED_SETS)
        return XR_ERROR_LIMIT_REACHED;
    for (uint32_t i = 0; i < attachInfo->countActionSets; i++)
        if (!rt_handle_get(RT_H(attachInfo->actionSets[i]), OBJ_ACTION_SET))
            return XR_ERROR_HANDLE_INVALID;
    for (uint32_t i = 0; i < attachInfo->countActionSets; i++) {
        rt_action_set *set = rt_handle_get(RT_H(attachInfo->actionSets[i]), OBJ_ACTION_SET);
        set->attached = 1;
        s->attached[s->n_attached++] = set;
    }
    s->sets_attached = 1;
    rt_input_attach_resolve(s);
    rt_input_sync_profiles(s);
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrGetCurrentInteractionProfile(XrSession session, XrPath topLevelUserPath,
                                                             XrInteractionProfileState *interactionProfile)
{
    RT_GUARD();
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    if (!interactionProfile || interactionProfile->type != XR_TYPE_INTERACTION_PROFILE_STATE)
        return XR_ERROR_VALIDATION_FAILURE;
    if (!s->sets_attached)
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    rt_instance *inst = s->inst;
    if (!rt_path_is_valid(inst, topLevelUserPath))
        return XR_ERROR_PATH_INVALID;
    const int hand = rt_hand_of_path(inst, topLevelUserPath);
    if (hand < 0) {
        if (topLevelUserPath == inst->p_head || topLevelUserPath == inst->p_gamepad) {
            interactionProfile->interactionProfile = XR_NULL_PATH;
            return XR_SUCCESS;
        }
        return XR_ERROR_PATH_UNSUPPORTED;
    }
    interactionProfile->interactionProfile = s->current_profile[hand];
    return XR_SUCCESS;
}

/* -- sync / state ------------------------------------------------------------------------------------ */
static int set_attached(rt_session *s, rt_action_set *set)
{
    for (uint32_t i = 0; i < s->n_attached; i++)
        if (s->attached[i] == set)
            return 1;
    return 0;
}

static void sync_action(rt_action *a, const rt_controller_t c[HAND_COUNT], int focused, XrTime now)
{
    for (int h = 0; h < HAND_COUNT; h++) {
        rt_action_state_t ns;
        memset(&ns, 0, sizeof ns);
        int bound = 0;
        for (uint32_t i = 0; i < a->n_bind; i++) {
            const rt_binding_t *b = &a->bind[i];
            if (b->hand != h || !c[h].active || !focused)
                continue;
            bound = 1;
            float v = 0;
            switch (b->src) {
            case SRC_SELECT_CLICK:
                v = a->type == XR_ACTION_TYPE_FLOAT_INPUT ? (c[h].trigger > 0 ? c[h].trigger : (float)c[h].select)
                                                         : (float)(c[h].select != 0);
                break;
            case SRC_MENU_CLICK:
                v = (float)(c[h].menu != 0);
                break;
            default:
                break;
            }
            if (v > ns.value)
                ns.value = v;
        }
        ns.active = bound ? XR_TRUE : XR_FALSE;
        rt_action_state_t *os = &a->state[h];
        const int value_changed = os->value != ns.value;
        ns.changed = (os->active && ns.active && value_changed) ? XR_TRUE : XR_FALSE;
        ns.last_change = (ns.active && (value_changed || !os->active)) ? now : os->last_change;
        *os = ns;
    }
}

XRRT_CORE XrResult XRAPI_CALL xrSyncActions(XrSession session, const XrActionsSyncInfo *syncInfo)
{
    RT_GUARD();
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    if (!syncInfo || syncInfo->type != XR_TYPE_ACTIONS_SYNC_INFO ||
        (syncInfo->countActiveActionSets && !syncInfo->activeActionSets))
        return XR_ERROR_VALIDATION_FAILURE;
    rt_instance *inst = s->inst;
    for (uint32_t i = 0; i < syncInfo->countActiveActionSets; i++) {
        rt_action_set *set = rt_handle_get(RT_H(syncInfo->activeActionSets[i].actionSet), OBJ_ACTION_SET);
        if (!set)
            return XR_ERROR_HANDLE_INVALID;
        if (!set_attached(s, set))
            return XR_ERROR_ACTIONSET_NOT_ATTACHED;
        const XrPath sp = syncInfo->activeActionSets[i].subactionPath;
        if (sp != XR_NULL_PATH && !rt_path_is_valid(inst, sp))
            return XR_ERROR_PATH_INVALID;
        if (sp != XR_NULL_PATH && rt_hand_of_path(inst, sp) < 0 && sp != inst->p_head && sp != inst->p_gamepad)
            return XR_ERROR_PATH_UNSUPPORTED;
    }
    const int focused = s->state == XR_SESSION_STATE_FOCUSED;
    const XrTime now = s->last_display_time > 0 ? s->last_display_time : inst->be->now();
    s->last_sync_time = now;
    rt_controller_t c[HAND_COUNT];
    memset(c, 0, sizeof c);
    for (int h = 0; h < HAND_COUNT; h++)
        inst->be->controller(h, now, &c[h]);
    rt_input_sync_profiles(s);
    for (rt_action *a = inst->actions; a; a = a->next_all) {
        if (!a->alive || !a->set || !a->set->attached)
            continue;
        int active_set = 0;
        for (uint32_t i = 0; i < syncInfo->countActiveActionSets; i++) {
            if (syncInfo->activeActionSets[i].actionSet != a->set->handle)
                continue;
            const int sh = rt_hand_of_path(inst, syncInfo->activeActionSets[i].subactionPath);
            active_set |= syncInfo->activeActionSets[i].subactionPath == XR_NULL_PATH ? 3 : (sh >= 0 ? 1 << sh : 0);
        }
        rt_controller_t cc[HAND_COUNT];
        for (int h = 0; h < HAND_COUNT; h++) {
            cc[h] = c[h];
            if (!(active_set & (1 << h)))
                cc[h].active = 0;
        }
        sync_action(a, cc, focused, now);
    }
    return focused ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
}

/* Resolve action + subaction path to the combined state. */
static XrResult get_state(XrSession session, const XrActionStateGetInfo *getInfo, XrActionType want,
                          rt_action_state_t *out)
{
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    if (!getInfo || getInfo->type != XR_TYPE_ACTION_STATE_GET_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    rt_action *a = rt_handle_get(RT_H(getInfo->action), OBJ_ACTION);
    if (!a)
        return XR_ERROR_HANDLE_INVALID;
    if (a->type != want)
        return XR_ERROR_ACTION_TYPE_MISMATCH;
    if (!a->set || !set_attached(s, a->set))
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    memset(out, 0, sizeof *out);
    if (getInfo->subactionPath != XR_NULL_PATH) {
        if (!rt_path_is_valid(s->inst, getInfo->subactionPath))
            return XR_ERROR_PATH_INVALID;
        uint32_t i = 0;
        while (i < a->n_sub && a->sub[i] != getInfo->subactionPath)
            i++;
        if (i == a->n_sub)
            return XR_ERROR_PATH_UNSUPPORTED;
        if (a->sub_hand[i] >= 0)
            *out = a->state[a->sub_hand[i]];
        return XR_SUCCESS;
    }
    for (int h = 0; h < HAND_COUNT; h++) {
        const rt_action_state_t *st = &a->state[h];
        if (!st->active)
            continue;
        if (!out->active || st->value > out->value)
            out->value = st->value;
        out->changed |= st->changed;
        if (st->last_change > out->last_change)
            out->last_change = st->last_change;
        out->active = XR_TRUE;
    }
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo *getInfo,
                                                      XrActionStateBoolean *state)
{
    RT_GUARD();
    if (!state || state->type != XR_TYPE_ACTION_STATE_BOOLEAN)
        return rt_get_session(session) ? XR_ERROR_VALIDATION_FAILURE : XR_ERROR_HANDLE_INVALID;
    rt_action_state_t st;
    XrResult r = get_state(session, getInfo, XR_ACTION_TYPE_BOOLEAN_INPUT, &st);
    if (XR_FAILED(r))
        return r;
    state->currentState = st.value > 0.5f ? XR_TRUE : XR_FALSE;
    state->changedSinceLastSync = st.changed;
    state->lastChangeTime = st.last_change;
    state->isActive = st.active;
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo *getInfo,
                                                    XrActionStateFloat *state)
{
    RT_GUARD();
    if (!state || state->type != XR_TYPE_ACTION_STATE_FLOAT)
        return rt_get_session(session) ? XR_ERROR_VALIDATION_FAILURE : XR_ERROR_HANDLE_INVALID;
    rt_action_state_t st;
    XrResult r = get_state(session, getInfo, XR_ACTION_TYPE_FLOAT_INPUT, &st);
    if (XR_FAILED(r))
        return r;
    state->currentState = st.value;
    state->changedSinceLastSync = st.changed;
    state->lastChangeTime = st.last_change;
    state->isActive = st.active;
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo *getInfo,
                                                       XrActionStateVector2f *state)
{
    RT_GUARD();
    if (!state || state->type != XR_TYPE_ACTION_STATE_VECTOR2F)
        return rt_get_session(session) ? XR_ERROR_VALIDATION_FAILURE : XR_ERROR_HANDLE_INVALID;
    rt_action_state_t st;
    XrResult r = get_state(session, getInfo, XR_ACTION_TYPE_VECTOR2F_INPUT, &st);
    if (XR_FAILED(r))
        return r;
    /* simple_controller has no 2D inputs: never active. */
    state->currentState = st.vec;
    state->changedSinceLastSync = st.changed;
    state->lastChangeTime = st.last_change;
    state->isActive = st.active;
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrGetActionStatePose(XrSession session, const XrActionStateGetInfo *getInfo,
                                                   XrActionStatePose *state)
{
    RT_GUARD();
    if (!state || state->type != XR_TYPE_ACTION_STATE_POSE)
        return rt_get_session(session) ? XR_ERROR_VALIDATION_FAILURE : XR_ERROR_HANDLE_INVALID;
    rt_action_state_t st;
    XrResult r = get_state(session, getInfo, XR_ACTION_TYPE_POSE_INPUT, &st);
    if (XR_FAILED(r))
        return r;
    state->isActive = st.active;
    return XR_SUCCESS;
}

XRRT_CORE XrResult XRAPI_CALL xrEnumerateBoundSourcesForAction(XrSession session,
                                                               const XrBoundSourcesForActionEnumerateInfo *enumerateInfo,
                                                               uint32_t sourceCapacityInput, uint32_t *sourceCountOutput,
                                                               XrPath *sources)
{
    RT_GUARD();
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    if (!enumerateInfo || enumerateInfo->type != XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    rt_action *a = rt_handle_get(RT_H(enumerateInfo->action), OBJ_ACTION);
    if (!a)
        return XR_ERROR_HANDLE_INVALID;
    if (!a->set || !set_attached(s, a->set))
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    XrPath list[RT_MAX_ACTION_BINDINGS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < a->n_bind; i++)
        if (s->current_profile[a->bind[i].hand] != XR_NULL_PATH)
            list[n++] = a->bind[i].path;
    int copy;
    XrResult r = rt_two_call(sourceCapacityInput, sourceCountOutput, n, sources, &copy);
    if (copy)
        memcpy(sources, list, n * sizeof *list);
    return r;
}

XRRT_CORE XrResult XRAPI_CALL xrGetInputSourceLocalizedName(XrSession session,
                                                            const XrInputSourceLocalizedNameGetInfo *getInfo,
                                                            uint32_t bufferCapacityInput, uint32_t *bufferCountOutput,
                                                            char *buffer)
{
    RT_GUARD();
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    if (!getInfo || getInfo->type != XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO || getInfo->whichComponents == 0)
        return XR_ERROR_VALIDATION_FAILURE;
    if (!s->sets_attached)
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    int hand;
    rt_source_t src;
    if (!simple_source(s->inst, getInfo->sourcePath, &hand, &src))
        return XR_ERROR_PATH_UNSUPPORTED;
    static const char *const kSrcNames[SRC_COUNT] = {"Trigger", "Options", "Grip", "Aim", "Haptics"};
    char name[128] = "";
    if (getInfo->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT)
        strcat(name, hand ? "Right Hand " : "Left Hand ");
    if (getInfo->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_INTERACTION_PROFILE_BIT)
        strcat(name, "PS VR2 Sense Controller ");
    if (getInfo->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT)
        strcat(name, kSrcNames[src]);
    size_t n = strlen(name);
    while (n && name[n - 1] == ' ')
        name[--n] = 0;
    int copy;
    XrResult r = rt_two_call(bufferCapacityInput, bufferCountOutput, (uint32_t)n + 1, buffer, &copy);
    if (copy)
        memcpy(buffer, name, n + 1);
    return r;
}

static XrResult haptic_common(XrSession session, const XrHapticActionInfo *info)
{
    rt_session *s = rt_get_session(session);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_HAPTIC_ACTION_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    rt_action *a = rt_handle_get(RT_H(info->action), OBJ_ACTION);
    if (!a)
        return XR_ERROR_HANDLE_INVALID;
    if (a->type != XR_ACTION_TYPE_VIBRATION_OUTPUT)
        return XR_ERROR_ACTION_TYPE_MISMATCH;
    if (!a->set || !set_attached(s, a->set))
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    if (info->subactionPath != XR_NULL_PATH) {
        uint32_t i = 0;
        while (i < a->n_sub && a->sub[i] != info->subactionPath)
            i++;
        if (i == a->n_sub)
            return XR_ERROR_PATH_UNSUPPORTED;
    }
    return s->state == XR_SESSION_STATE_FOCUSED ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
}

XRRT_CORE XrResult XRAPI_CALL xrApplyHapticFeedback(XrSession session, const XrHapticActionInfo *hapticActionInfo,
                                                    const XrHapticBaseHeader *hapticFeedback)
{
    RT_GUARD();
    if (!hapticFeedback)
        return XR_ERROR_VALIDATION_FAILURE;
    /* TODO: Sense controller rumble (scePad) / headset (sceHmd2SetVibration). */
    return haptic_common(session, hapticActionInfo);
}

XRRT_CORE XrResult XRAPI_CALL xrStopHapticFeedback(XrSession session, const XrHapticActionInfo *hapticActionInfo)
{
    RT_GUARD();
    return haptic_common(session, hapticActionInfo);
}
