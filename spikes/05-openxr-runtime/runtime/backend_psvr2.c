/* backend_psvr2.c - PS5 backend: PS VR2 through common/psvr2.
 *
 * Mapping (see NOTES.md):
 *   xrGetSystem      psvr2_preload + psvr2_open(initial user): Hmd2,
 *                    VrTracker2, reprojection. FOV = psvr2_eye_tangents with
 *                    the same 1.15 overscan Spike 3 uses.
 *   xrBeginSession   three present buffers of (2*eye_w) x eye_h, then
 *                    psvr2_start (120 Hz VR mode, half-rate rendering).
 *                    zerocopy: GL textures exported as T#s with
 *                    eglExportTextureDescriptorPS5 (ps5-opengl patch);
 *                    copy: linear direct memory + a GL staging texture.
 *   xrWaitFrame      psvr2_begin_frame: waits for the 60 Hz slot, begins the
 *                    Hmd2 frame and fetches the pose for its display time.
 *   swapchains       plain GL textures (glTexStorage2D) in the app's EGL
 *                    context (XrGraphicsBindingEGLMNDX.context).
 *   xrEndFrame       per eye one glBlitFramebuffer from the released
 *                    swapchain image's imageRect into the eye half of present
 *                    buffer (frame % 3), flipped vertically (GL row 0 is the
 *                    bottom, Hmd2 wants the top first); copy mode adds a
 *                    glReadPixels into linear memory. Then psvr2_submit with
 *                    the tangents of the FOV the layer says it rendered with
 *                    and the pose/frame psvr2_begin_frame returned for that
 *                    display time.
 *
 * Compiles with prospero-clang; cannot be run off-console.
 */
#include "xr_internal.h"
#include "xr_math.h"

#define GL_GLEXT_PROTOTYPES 1
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "psvr2/psvr2.h"

#ifndef XRRT_PSVR2_EYE_H
#define XRRT_PSVR2_EYE_H 1536 /* per-eye rows; panels are 2000x2040 */
#endif
#define OVERSCAN 1.15f
#define IPD_DEFAULT 0.063f
#define FRAME_NS 16666667LL
#define N_PRESENT PSVR2_MAX_BUFFERS
#define FRAME_RING 8

int sceUserServiceInitialize(void *);
int sceUserServiceGetInitialUser(int *);
uint64_t sceKernelGetProcessTime(void);
int sceKernelUsleep(unsigned usec);

typedef EGLBoolean (*export_tsharp_fn)(EGLDisplay, EGLContext, GLuint, uint32_t[8], uint64_t *, uint32_t *,
                                       uint32_t *, uint32_t *, uint32_t *);

static struct {
    int open, active;
    rt_backend_info info;
    float tan[2][4];
    uint32_t eye_w, eye_h;
    /* app's EGL objects */
    EGLDisplay dpy;
    EGLContext ctx;
    PFN_xrEglGetProcAddressMNDX getproc;
    /* present buffers */
    int zerocopy;
    GLuint present_tex[N_PRESENT], present_fbo[N_PRESENT];
    GLuint stage_tex, stage_fbo, read_fbo;
    uint8_t *linear;
    uint32_t pitch_px;
    uint64_t one;
    uint32_t tsharp[N_PRESENT][8];
    /* frames from psvr2_begin_frame, keyed by display time */
    psvr2_frame_t ring[FRAME_RING];
    uint32_t ring_pos;
    psvr2_frame_t latest;
    int have_latest;
    psvr2_frame_t last_sub;
    float last_tan[2][4];
    int last_index, have_sub;
    XrTime next_slot; /* clock pacing when psvr2_begin_frame fails */
    unsigned begin_errors;
} p = {.last_index = -1};

static void log_line(const char *s)
{
    RT_INFO("%s", s);
}

static XrTime ps_now(void)
{
    return (XrTime)sceKernelGetProcessTime() * 1000;
}

/* -- EGL context: GL calls go to the app's context ------------------------------ */
typedef struct {
    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface draw, read;
    int switched;
} ctx_save_t;

static int ctx_enter(ctx_save_t *s)
{
    memset(s, 0, sizeof *s);
    s->ctx = eglGetCurrentContext();
    if (s->ctx == p.ctx)
        return 0;
    s->dpy = eglGetCurrentDisplay();
    s->draw = eglGetCurrentSurface(EGL_DRAW);
    s->read = eglGetCurrentSurface(EGL_READ);
    if (!eglMakeCurrent(p.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, p.ctx)) {
        RT_ERR("psvr2: cannot make the session's EGL context current (0x%04x)", eglGetError());
        return -1;
    }
    s->switched = 1;
    return 0;
}

static void ctx_leave(const ctx_save_t *s)
{
    if (!s->switched)
        return;
    if (s->ctx == EGL_NO_CONTEXT)
        eglMakeCurrent(p.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    else
        eglMakeCurrent(s->dpy, s->draw, s->read, s->ctx);
}

/* -- open / close ----------------------------------------------------------------- */
XRRT_VISIBLE int xr_psvr2_preload(void)
{
    psvr2_set_logger(log_line);
    return psvr2_preload();
}

static int ps_open(rt_backend_info *info)
{
    psvr2_set_logger(log_line);
    if (psvr2_preload() != 0)
        return -1;
    int user = -1;
    sceUserServiceInitialize(NULL); /* may already be initialised by the app */
    sceUserServiceGetInitialUser(&user);
    if (psvr2_open(user) != 0)
        return -1;
    psvr2_eye_tangents(0, OVERSCAN, p.tan[0]);
    psvr2_eye_tangents(1, OVERSCAN, p.tan[1]);
    p.eye_h = XRRT_PSVR2_EYE_H;
    p.eye_w = (uint32_t)((float)p.eye_h * (p.tan[0][0] + p.tan[0][1]) / (p.tan[0][2] + p.tan[0][3]) + 8) & ~15u;
    info->system_name = "PS VR2";
    info->vendor_id = 0x054c;
    memcpy(info->tan, p.tan, sizeof p.tan);
    info->rec_w = p.eye_w;
    info->rec_h = p.eye_h;
    info->max_w = info->max_h = 4096;
    info->frame_period_ns = FRAME_NS;
    /* The tracker's origin height is unknown: STAGE assumes the head starts
     * default_eye_height above the floor (as Spike 3's world does). */
    info->floor_known = 0;
    info->default_eye_height = 1.6f;
    info->position_tracking = 1;
    p.info = *info;
    p.open = 1;
    return 0;
}

static void ps_close(void)
{
    if (!p.open)
        return;
    psvr2_close(); /* must run before exit: VrTracker2's destructor faults otherwise */
    p.open = 0;
}

/* -- session ------------------------------------------------------------------------ */
static XrResult ps_session_create(const XrGraphicsBindingEGLMNDX *b)
{
    if (!b->display || !b->context) {
        RT_ERR("psvr2: XrGraphicsBindingEGLMNDX needs a display and a context");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    p.dpy = b->display;
    p.ctx = b->context;
    p.getproc = b->getProcAddress;
    return XR_SUCCESS;
}

static void ps_session_destroy(void)
{
    p.dpy = EGL_NO_DISPLAY;
    p.ctx = EGL_NO_CONTEXT;
}

static void free_present(void)
{
    if (p.present_fbo[0])
        glDeleteFramebuffers(N_PRESENT, p.present_fbo);
    if (p.present_tex[0])
        glDeleteTextures(N_PRESENT, p.present_tex);
    if (p.stage_fbo)
        glDeleteFramebuffers(1, &p.stage_fbo);
    if (p.stage_tex)
        glDeleteTextures(1, &p.stage_tex);
    if (p.read_fbo)
        glDeleteFramebuffers(1, &p.read_fbo);
    memset(p.present_fbo, 0, sizeof p.present_fbo);
    memset(p.present_tex, 0, sizeof p.present_tex);
    p.stage_fbo = p.stage_tex = p.read_fbo = 0;
    /* direct memory (copy path) is kept for the next session */
}

static GLuint make_target(GLuint *fbo, GLsizei w, GLsizei h)
{
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    return tex;
}

static XrResult ps_session_begin(void)
{
    ctx_save_t cs;
    if (ctx_enter(&cs) != 0)
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    GLint old_fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fb);
    const GLsizei W = (GLsizei)(2 * p.eye_w), H = (GLsizei)p.eye_h;

    /* Same hand-off decision as Spike 3b: export GL's own memory if the
     * driver has the patch, else read back into linear direct memory. */
    export_tsharp_fn exp = NULL;
    if (p.getproc)
        exp = (export_tsharp_fn)p.getproc("eglExportTextureDescriptorPS5");
    if (!exp)
        exp = (export_tsharp_fn)eglGetProcAddress("eglExportTextureDescriptorPS5");
    p.zerocopy = exp != NULL;
    for (int b = 0; b < N_PRESENT; b++)
        p.present_tex[b] = make_target(&p.present_fbo[b], W, H);
    glFinish();
    for (int b = 0; p.zerocopy && b < N_PRESENT; b++) {
        uint64_t addr = 0;
        uint32_t w, h, pitch, sw;
        if (!exp(p.dpy, p.ctx, p.present_tex[b], p.tsharp[b], &addr, &w, &h, &pitch, &sw)) {
            RT_ERR("psvr2: export of present buffer %d refused (0x%04x): copy path", b, eglGetError());
            p.zerocopy = 0;
        } else {
            RT_INFO("psvr2: present %d addr 0x%llx %ux%u pitch %u sw_mode %u", b, (unsigned long long)addr, w,
                    h, pitch, sw);
        }
    }
    if (!p.zerocopy) {
        glDeleteFramebuffers(N_PRESENT, p.present_fbo);
        glDeleteTextures(N_PRESENT, p.present_tex);
        memset(p.present_fbo, 0, sizeof p.present_fbo);
        memset(p.present_tex, 0, sizeof p.present_tex);
        p.stage_tex = make_target(&p.stage_fbo, W, H);
        if (!p.linear) {
            p.pitch_px = ((uint32_t)W + 255) & ~255u;
            p.one = ((uint64_t)p.pitch_px * 4 * (uint64_t)H + 0xffff) & ~0xffffull;
            p.linear = psvr2_direct_alloc(p.one * N_PRESENT, 0x10000);
        }
        if (!p.linear) {
            free_present();
            glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)old_fb);
            ctx_leave(&cs);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        memset(p.linear, 0, p.one * N_PRESENT);
        for (int b = 0; b < N_PRESENT; b++)
            psvr2_build_tsharp_rgba8(p.tsharp[b], (uint64_t)(uintptr_t)(p.linear + b * p.one), (uint32_t)W,
                                     (uint32_t)H, p.pitch_px * 4, 0);
    }
    glGenFramebuffers(1, &p.read_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)old_fb);
    ctx_leave(&cs);

    if (psvr2_start((const uint32_t(*)[8])p.tsharp, N_PRESENT, p.eye_w, p.eye_h) != 0) {
        RT_ERR("psvr2: VR mode / buffer registration failed (%s path)", p.zerocopy ? "zerocopy" : "copy");
        if (ctx_enter(&cs) == 0) {
            free_present();
            ctx_leave(&cs);
        }
        return XR_ERROR_RUNTIME_FAILURE;
    }
    RT_INFO("psvr2: VR mode, %s path, %dx%d present buffers", p.zerocopy ? "zerocopy" : "copy", W, H);
    p.active = 1;
    p.have_latest = p.have_sub = 0;
    p.last_index = -1;
    p.next_slot = 0;
    memset(p.ring, 0, sizeof p.ring);
    return XR_SUCCESS;
}

static void ps_session_end(void)
{
    if (!p.active)
        return;
    psvr2_stop();
    p.active = 0;
    ctx_save_t cs;
    if (ctx_enter(&cs) == 0) {
        free_present();
        ctx_leave(&cs);
    }
}

/* -- frames and poses ----------------------------------------------------------------- */
static int ps_wait_frame(rt_frame_t *f)
{
    psvr2_frame_t pf;
    if (p.active && psvr2_begin_frame(&pf) == 0) {
        p.ring[p.ring_pos++ % FRAME_RING] = pf;
        p.latest = pf;
        p.have_latest = 1;
        f->frame = pf.frame;
        f->display_time = (XrTime)pf.display_time * 1000;
        f->period = FRAME_NS;
        return 0;
    }
    /* No Hmd2 frame: keep the app paced on the clock, poses go stale. */
    p.begin_errors++;
    XrTime now = ps_now();
    if (p.next_slot > now && p.next_slot - now <= FRAME_NS) {
        sceKernelUsleep((unsigned)((p.next_slot - now) / 1000));
        now = p.next_slot;
    }
    p.next_slot = (p.next_slot && now < p.next_slot + FRAME_NS) ? p.next_slot + FRAME_NS : now + FRAME_NS;
    f->frame = 0;
    f->display_time = p.next_slot;
    f->period = FRAME_NS;
    return -1;
}

static XrPosef to_pose(const psvr2_pose_t *ps)
{
    XrPosef r;
    r.position.x = ps->pos[0];
    r.position.y = ps->pos[1];
    r.position.z = ps->pos[2];
    r.orientation.x = ps->quat[0];
    r.orientation.y = ps->quat[1];
    r.orientation.z = ps->quat[2];
    r.orientation.w = ps->quat[3];
    r.orientation = q_normalize(r.orientation);
    return r;
}

/* psvr2 only exposes the pose of the frame it began (VrTracker2GetResult is
 * internal to psvr2.c), so every time maps to the latest frame's pose. */
static void ps_locate_views(XrTime t, rt_views_t *v)
{
    (void)t;
    if (!p.have_latest || !p.latest.tracked) {
        v->head = pose_identity();
        v->eye[0] = v->eye[1] = pose_identity();
        v->eye[0].position.x = -0.5f * IPD_DEFAULT;
        v->eye[1].position.x = 0.5f * IPD_DEFAULT;
        v->orientation_valid = v->position_valid = 0;
        v->orientation_tracked = v->position_tracked = 0;
        return;
    }
    const psvr2_frame_t *f = &p.latest;
    v->head = to_pose(&f->head);
    for (int eye = 0; eye < 2; eye++) {
        const psvr2_pose_t *e = &f->eye[eye];
        const int have_q = e->quat[0] != 0 || e->quat[1] != 0 || e->quat[2] != 0 || e->quat[3] != 0;
        if (f->eye_pos_valid) {
            v->eye[eye] = to_pose(e);
            if (!have_q)
                v->eye[eye].orientation = v->head.orientation;
        } else {
            const XrPosef off = {{0, 0, 0, 1}, {(eye ? 0.5f : -0.5f) * IPD_DEFAULT, 0, 0}};
            v->eye[eye] = pose_mul(v->head, off);
            if (have_q)
                v->eye[eye].orientation = to_pose(e).orientation;
        }
    }
    v->orientation_valid = v->position_valid = 1;
    v->orientation_tracked = v->position_tracked = 1;
}

/* TODO(Sense controllers): see NOTES.md - scePadOpen(user, 3/4), register
 * with sceVrTracker2RegisterDevice(2/1, pad), pose from
 * sceVrTracker2GetResult(pad handle), buttons from
 * scePadVrControllerReadState. Needs a psvr2_controller() in common/psvr2. */
static void ps_controller(int hand, XrTime t, rt_controller_t *c)
{
    (void)hand;
    (void)t;
    memset(c, 0, sizeof *c);
}

static int ps_user_present(void)
{
    return psvr2_worn();
}

/* -- swapchains ------------------------------------------------------------------------- */
static XrResult ps_swapchain_create(const XrSwapchainCreateInfo *info, uint32_t count, uint64_t *images,
                                    void **priv)
{
    ctx_save_t cs;
    if (ctx_enter(&cs) != 0)
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    GLint old = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old);
    while (glGetError() != GL_NO_ERROR) {
    }
    GLuint tex[RT_MAX_IMAGES] = {0};
    glGenTextures((GLsizei)count, tex);
    for (uint32_t i = 0; i < count; i++) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexStorage2D(GL_TEXTURE_2D, (GLsizei)info->mipCount, (GLenum)info->format, (GLsizei)info->width,
                       (GLsizei)info->height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        images[i] = tex[i];
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)old);
    const GLenum err = glGetError();
    ctx_leave(&cs);
    if (err != GL_NO_ERROR) {
        RT_ERR("psvr2: swapchain textures failed (GL 0x%04x)", err);
        if (ctx_enter(&cs) == 0) {
            glDeleteTextures((GLsizei)count, tex);
            ctx_leave(&cs);
        }
        return XR_ERROR_RUNTIME_FAILURE;
    }
    *priv = NULL;
    return XR_SUCCESS;
}

static void ps_swapchain_destroy(void *priv, uint32_t count, const uint64_t *images)
{
    (void)priv;
    ctx_save_t cs;
    if (p.ctx == EGL_NO_CONTEXT || ctx_enter(&cs) != 0)
        return;
    GLuint tex[RT_MAX_IMAGES];
    for (uint32_t i = 0; i < count && i < RT_MAX_IMAGES; i++)
        tex[i] = (GLuint)images[i];
    glDeleteTextures((GLsizei)count, tex);
    ctx_leave(&cs);
}

/* -- submit ------------------------------------------------------------------------------- */
static const psvr2_frame_t *frame_for(XrTime display_time)
{
    for (int i = 0; i < FRAME_RING; i++)
        if (p.ring[i].frame && (XrTime)p.ring[i].display_time * 1000 == display_time)
            return &p.ring[i];
    return p.have_latest ? &p.latest : NULL;
}

/* Swapchain rects -> eye halves of present buffer `b`, flipped vertically. */
static int blit_eyes(const rt_submit_t *s, int b)
{
    GLint rfb = 0, dfb = 0, ppb = 0, pack_row = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB); /* raw byte copy: Hmd2 samples a UNORM T# */
    while (glGetError() != GL_NO_ERROR) {
    }

    const GLuint dst = p.zerocopy ? p.present_fbo[b] : p.stage_fbo;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, p.read_fbo);
    GLuint bound = 0;
    for (int e = 0; e < 2; e++) {
        const rt_submit_view_t *v = &s->view[e];
        if ((GLuint)v->image != bound) {
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (GLuint)v->image, 0);
            bound = (GLuint)v->image;
        }
        const GLint sx0 = v->rect.offset.x, sy0 = v->rect.offset.y;
        const GLint sx1 = sx0 + v->rect.extent.width, sy1 = sy0 + v->rect.extent.height;
        const GLint dx0 = e * (GLint)p.eye_w, dx1 = dx0 + (GLint)p.eye_w;
        const int scaled = v->rect.extent.width != (int32_t)p.eye_w || v->rect.extent.height != (int32_t)p.eye_h;
        glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, (GLint)p.eye_h, dx1, 0, GL_COLOR_BUFFER_BIT,
                          scaled ? GL_LINEAR : GL_NEAREST);
    }
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

    if (p.zerocopy) {
        glFinish(); /* the compositor must not sample a half-written buffer */
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, p.stage_fbo);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &ppb);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)p.pitch_px);
        glReadPixels(0, 0, (GLsizei)(2 * p.eye_w), (GLsizei)p.eye_h, GL_RGBA, GL_UNSIGNED_BYTE,
                     p.linear + (uint64_t)b * p.one);
        glPixelStorei(GL_PACK_ROW_LENGTH, pack_row);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)ppb);
    }
    const GLenum err = glGetError();

    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)dfb);
    if (scissor)
        glEnable(GL_SCISSOR_TEST);
    if (srgb)
        glEnable(GL_FRAMEBUFFER_SRGB);
    if (err != GL_NO_ERROR) {
        RT_ERR("psvr2: blit/readback GL error 0x%04x", err);
        return -1;
    }
    return 0;
}

static int ps_submit(const rt_submit_t *s, rt_stats_t *stats)
{
    (void)stats;
    if (!p.active)
        return -1;
    const psvr2_frame_t *cur = frame_for(s->display_time);
    if (!cur)
        return -1; /* no Hmd2 frame began: nothing to submit against */

    if (!s->has_projection) {
        /* Keep the compositor fed: the last image again with the pose it was
         * rendered with (reprojection corrects it), or black buffer 0. */
        psvr2_frame_t f = p.have_sub ? p.last_sub : *cur;
        f.frame = cur->frame;
        f.display_time = cur->display_time;
        const int idx = p.have_sub ? p.last_index : 0;
        float tan[2][4];
        memcpy(tan, p.have_sub ? p.last_tan : p.tan, sizeof tan);
        return psvr2_submit(idx, &f, (const float(*)[4])tan);
    }

    const int b = (int)(cur->frame % N_PRESENT);
    ctx_save_t cs;
    if (ctx_enter(&cs) != 0)
        return -1;
    const int rc = blit_eyes(s, b);
    ctx_leave(&cs);
    if (rc != 0)
        return -1;
    float tan[2][4];
    for (int e = 0; e < 2; e++)
        memcpy(tan[e], s->view[e].tan, sizeof tan[e]);
    const int r = psvr2_submit(b, cur, (const float(*)[4])tan);
    p.last_sub = *cur;
    memcpy(p.last_tan, tan, sizeof tan);
    p.last_index = b;
    p.have_sub = 1;
    return r;
}

const rt_backend_ops rt_backend_psvr2 = {
    .name = "psvr2",
    .open = ps_open,
    .close = ps_close,
    .session_create = ps_session_create,
    .session_destroy = ps_session_destroy,
    .session_begin = ps_session_begin,
    .session_end = ps_session_end,
    .now = ps_now,
    .wait_frame = ps_wait_frame,
    .locate_views = ps_locate_views,
    .controller = ps_controller,
    .user_present = ps_user_present,
    .swapchain_create = ps_swapchain_create,
    .swapchain_destroy = ps_swapchain_destroy,
    .submit = ps_submit,
};
