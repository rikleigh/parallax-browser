/* backend_mock.c - headless backend: a synthetic PS VR2.
 *
 * Head: standing 1.65 m above a known floor (tracking origin on the floor),
 * slow +/-30 degree yaw sweep (0.1 Hz) with a 1 cm bob (0.5 Hz), all a pure
 * function of time so any XrTime can be located. Frames: 60 Hz from the
 * monotonic clock, like psvr2's half-rate schedule. Swapchain images: fake
 * GL texture names (no GL). Controllers: inactive unless
 * XR_PSVR2_MOCK_CONTROLLERS=1, which makes both hands active with a select
 * press toggling every 0.5 s (exercises the action plumbing).
 */
#include "xr_internal.h"
#include "xr_math.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MOCK_PERIOD_NS 16666667LL
#define MOCK_EYE_HEIGHT 1.65f
#define MOCK_IPD 0.064f
#define MOCK_OVERSCAN 1.15f
#define TWO_PI 6.2831853f

/* PS VR2-like per-eye display tangents (out, in, up, down); the render FOV
 * adds the reprojection overscan the real backend uses. */
static const float kDisplayTan[4] = {1.30f, 1.00f, 1.20f, 1.30f};

static struct {
    int open, in_session, controllers;
    XrTime t0, next_slot;
    uint32_t next_tex;
    uint64_t frame;
} m;

static XrTime mock_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (XrTime)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void sleep_until(XrTime t)
{
    for (;;) {
        const XrTime now = mock_now();
        if (now >= t)
            return;
        const XrTime d = t - now;
        struct timespec ts = {(time_t)(d / 1000000000LL), (long)(d % 1000000000LL)};
        if (nanosleep(&ts, NULL) != 0 && errno != EINTR)
            return;
    }
}

static int mock_open(rt_backend_info *info)
{
    m.open = 1;
    m.t0 = mock_now();
    m.next_tex = 1000;
    const char *c = getenv("XR_PSVR2_MOCK_CONTROLLERS");
    m.controllers = c && atoi(c) != 0;
    info->system_name = "PS VR2 (mock)";
    info->vendor_id = 0x054c; /* Sony */
    for (int eye = 0; eye < 2; eye++) {
        /* left eye: out is left; right eye: in is left (psvr2_eye_tangents) */
        info->tan[eye][0] = (eye ? kDisplayTan[1] : kDisplayTan[0]) * MOCK_OVERSCAN;
        info->tan[eye][1] = (eye ? kDisplayTan[0] : kDisplayTan[1]) * MOCK_OVERSCAN;
        info->tan[eye][2] = kDisplayTan[2] * MOCK_OVERSCAN;
        info->tan[eye][3] = kDisplayTan[3] * MOCK_OVERSCAN;
    }
    /* Square pixels at 1536 rows (the eye height Spike 3 renders). */
    info->rec_h = 1536;
    info->rec_w = (uint32_t)(1536.0f * (info->tan[0][0] + info->tan[0][1]) /
                             (info->tan[0][2] + info->tan[0][3]) + 8) & ~15u;
    info->max_w = info->max_h = 4096;
    info->frame_period_ns = MOCK_PERIOD_NS;
    info->floor_known = 1;
    info->floor_y = 0.0f;
    info->default_eye_height = 1.6f;
    info->position_tracking = 1;
    return 0;
}

static void mock_close(void)
{
    m.open = 0;
}

static XrResult mock_session_create(const XrGraphicsBindingEGLMNDX *b)
{
    (void)b; /* null EGL handles are fine: no GL here */
    return XR_SUCCESS;
}

static void mock_session_destroy(void)
{
}

static XrResult mock_session_begin(void)
{
    m.in_session = 1;
    m.next_slot = 0;
    return XR_SUCCESS;
}

static void mock_session_end(void)
{
    m.in_session = 0;
}

/* Same schedule as psvr2_begin_frame: fixed 60 Hz slots, re-anchored after
 * a stall. The predicted display time is one period after the slot. */
static int mock_wait_frame(rt_frame_t *f)
{
    XrTime now = mock_now();
    if (m.next_slot > now && m.next_slot - now <= MOCK_PERIOD_NS) {
        sleep_until(m.next_slot);
        now = m.next_slot;
    }
    const XrTime slot = (m.next_slot && now < m.next_slot + MOCK_PERIOD_NS) ? m.next_slot : now;
    m.next_slot = slot + MOCK_PERIOD_NS;
    f->frame = ++m.frame;
    f->period = MOCK_PERIOD_NS;
    f->display_time = slot + MOCK_PERIOD_NS;
    return 0;
}

static XrPosef head_at(XrTime t)
{
    const float s = (float)((double)(t - m.t0) * 1e-9);
    XrPosef p;
    p.orientation = q_axis_angle(0, 1, 0, 0.5236f * sinf(TWO_PI * 0.1f * s));
    p.position.x = 0.0f;
    p.position.y = MOCK_EYE_HEIGHT + 0.01f * sinf(TWO_PI * 0.5f * s);
    p.position.z = 0.0f;
    return p;
}

static void mock_locate_views(XrTime t, rt_views_t *v)
{
    v->head = head_at(t);
    for (int eye = 0; eye < 2; eye++) {
        const XrPosef off = {{0, 0, 0, 1}, {(eye ? 0.5f : -0.5f) * MOCK_IPD, 0, 0}};
        v->eye[eye] = pose_mul(v->head, off);
    }
    v->orientation_valid = v->position_valid = 1;
    v->orientation_tracked = v->position_tracked = 1;
}

static void mock_controller(int hand, XrTime t, rt_controller_t *c)
{
    memset(c, 0, sizeof *c);
    if (!m.controllers)
        return;
    const float s = (float)((double)(t - m.t0) * 1e-9);
    c->active = 1;
    c->grip.orientation = q_identity();
    c->grip.position.x = hand ? 0.2f : -0.2f;
    c->grip.position.y = 1.2f;
    c->grip.position.z = -0.3f;
    /* aim: tilted 30 degrees down from the grip */
    c->aim = pose_mul(c->grip, (XrPosef){q_axis_angle(1, 0, 0, -0.5236f), {0, 0, -0.05f}});
    c->select = ((int)(s * 2.0f) & 1) != 0;
    c->trigger = c->select ? 1.0f : 0.0f;
}

static int mock_user_present(void)
{
    return 1;
}

static XrResult mock_swapchain_create(const XrSwapchainCreateInfo *info, uint32_t count, uint64_t *images,
                                      void **priv)
{
    (void)info;
    for (uint32_t i = 0; i < count; i++)
        images[i] = m.next_tex++;
    *priv = NULL;
    return XR_SUCCESS;
}

static void mock_swapchain_destroy(void *priv, uint32_t count, const uint64_t *images)
{
    (void)priv;
    (void)count;
    (void)images;
}

static int mock_submit(const rt_submit_t *s, rt_stats_t *stats)
{
    (void)stats;
    if (!s->has_projection)
        return 0;
    /* What the PS5 backend would do: one blit per eye into the SBS buffer. */
    for (int e = 0; e < 2; e++)
        if (s->view[e].image == 0 || s->view[e].rect.extent.width <= 0)
            return -1;
    return 0;
}

const rt_backend_ops rt_backend_mock = {
    .name = "mock",
    .open = mock_open,
    .close = mock_close,
    .session_create = mock_session_create,
    .session_destroy = mock_session_destroy,
    .session_begin = mock_session_begin,
    .session_end = mock_session_end,
    .now = mock_now,
    .wait_frame = mock_wait_frame,
    .locate_views = mock_locate_views,
    .controller = mock_controller,
    .user_present = mock_user_present,
    .swapchain_create = mock_swapchain_create,
    .swapchain_destroy = mock_swapchain_destroy,
    .submit = mock_submit,
};
