/* Spike 3b - GL-rendered, head-tracked stereo in the PS VR2 (the key gate).
 *
 * Renders a small world (cube ring + floor grid) with ps5-opengl into a
 * side-by-side eye texture using the tracker's pose for the predicted display
 * time, and hands each frame to Hmd2 through psvr2. Two hand-off paths:
 *
 *   zerocopy  three GL textures exported with eglExportTextureDescriptorPS5
 *             (spikes/02-gl-egl/ps5-opengl patch): the compositor samples
 *             GL's own memory. This is what a WebXR swapchain needs.
 *   copy      fallback with stock ps5-opengl: render to one FBO, glReadPixels
 *             into a linear direct-memory buffer, submit a linear T#.
 *             Proves the pipeline end to end even if it costs a readback.
 *
 * Pass: stable, correctly stereo, world-locked cubes at 60 fps rendered
 * (120 Hz out) with no judder. Logged per run: path used, render and hand-off
 * ms, missed slots, submit errors, tracking.
 *
 * Runs as a registered title (ps5-opengl requires it); logs go to
 * tools/logserver.sh.
 */
#define SPIKE_NAME "03b-gl-hmd2"
#include "../common/title.h"
#include "../common/glutil.h"
#include "../common/psvr2/psvr2.h"

#include <stdint.h>
#include <stdlib.h>

#ifndef LOG_HOST
#define LOG_HOST "192.168.1.100"
#endif
#ifndef LOG_PORT
#define LOG_PORT 9999
#endif
#ifndef SECONDS
#define SECONDS 60
#endif
#ifndef EYE
#define EYE 1536 /* per-eye pixels; PS VR2 panels are 2000x2040 */
#endif
#define OVERSCAN 1.15f /* margin the reprojection turns into on head motion */
#define IPD_DEFAULT 0.063f

int sceUserServiceInitialize(void *);
int sceUserServiceGetInitialUser(int *);

typedef EGLBoolean (*export_tsharp_fn)(EGLDisplay, EGLContext, GLuint, uint32_t[8], uint64_t *,
                                       uint32_t *, uint32_t *, uint32_t *, uint32_t *);

static void log_line(const char *s)
{
    spike_log("%s", s);
}

/* -- the world ------------------------------------------------------------ */
static const char *kVs =
    "#version 330 core\n"
    "layout(location=0) in vec3 p;\n"
    "layout(location=1) in vec3 n;\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 model;\n"
    "out vec3 vn;\n"
    "void main() { vn = mat3(model) * n; gl_Position = mvp * vec4(p, 1.0); }\n";
static const char *kFs =
    "#version 330 core\n"
    "in vec3 vn;\n"
    "uniform vec3 color;\n"
    "uniform float lit;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  float d = mix(1.0, 0.35 + 0.65 * max(dot(normalize(vn), normalize(vec3(0.4, 1.0, 0.3))), 0.0), lit);\n"
    "  o = vec4(color * d, 1.0);\n"
    "}\n";

static GLuint g_prog, g_cube_vao, g_grid_vao;
static GLint u_mvp, u_model, u_color, u_lit;
static int g_grid_verts;

static void build_world(void)
{
    static const float f[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    float v[36 * 6];
    int k = 0;
    for (int face = 0; face < 6; face++) {
        const float *nrm = f[face];
        /* two axes spanning the face */
        float a[3] = {nrm[1], nrm[2], nrm[0]}, b[3] = {nrm[2], nrm[0], nrm[1]};
        static const int quad[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
        for (int q = 0; q < 6; q++) {
            for (int i = 0; i < 3; i++)
                v[k + i] = 0.5f * (nrm[i] + quad[q][0] * a[i] + quad[q][1] * b[i]);
            v[k + 3] = nrm[0], v[k + 4] = nrm[1], v[k + 5] = nrm[2];
            k += 6;
        }
    }
    GLuint vbo;
    glGenVertexArrays(1, &g_cube_vao);
    glBindVertexArray(g_cube_vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void *)12);

    enum { LINES = 41 }; /* 20 m x 20 m, 0.5 m spacing */
    static float g[LINES * 4 * 6];
    int gi = 0;
    for (int i = 0; i < LINES; i++) {
        const float t = -10.0f + 0.5f * (float)i;
        const float seg[4][3] = {{t, 0, -10}, {t, 0, 10}, {-10, 0, t}, {10, 0, t}};
        for (int s = 0; s < 4; s++) {
            g[gi++] = seg[s][0], g[gi++] = seg[s][1], g[gi++] = seg[s][2];
            g[gi++] = 0, g[gi++] = 1, g[gi++] = 0;
        }
    }
    g_grid_verts = LINES * 4;
    glGenVertexArrays(1, &g_grid_vao);
    glBindVertexArray(g_grid_vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof g, g, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void *)12);
}

static void draw_world(mat4 vp, const float anchor[3], uint64_t frame)
{
    glUseProgram(g_prog);
    /* floor 1.6 m below where the head first was */
    mat4 m = mat4_translate(anchor[0], anchor[1] - 1.6f, anchor[2]);
    glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mat4_mul(vp, m).m);
    glUniformMatrix4fv(u_model, 1, GL_FALSE, m.m);
    glUniform3f(u_color, 0.3f, 0.8f, 1.0f);
    glUniform1f(u_lit, 0.0f);
    glBindVertexArray(g_grid_vao);
    glDrawArrays(GL_LINES, 0, g_grid_verts);

    /* 12 cubes on a 2 m ring at eye height, one spinning slowly (motion cue) */
    glUniform1f(u_lit, 1.0f);
    glBindVertexArray(g_cube_vao);
    for (int i = 0; i < 12; i++) {
        const float a = (float)i * 6.2831853f / 12.0f;
        const float c[3] = {0.5f + 0.5f * cosf(a), 0.5f + 0.5f * cosf(a + 2.1f), 0.5f + 0.5f * cosf(a + 4.2f)};
        mat4 model = mat4_mul(mat4_translate(anchor[0] + 2.0f * sinf(a), anchor[1] + (i % 3 - 1) * 0.4f,
                                             anchor[2] - 2.0f * cosf(a)),
                              mat4_scale(0.3f));
        if (i == 0) {
            const float h = (float)(frame % 360) * 0.0174533f * 0.5f;
            const float q[4] = {0, sinf(h), 0, cosf(h)};
            model = mat4_mul(model, mat4_quat(q));
        }
        glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mat4_mul(vp, model).m);
        glUniformMatrix4fv(u_model, 1, GL_FALSE, model.m);
        glUniform3f(u_color, c[0], c[1], c[2]);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }
}

/* Eye position: the tracker's when plausible, else head +/- IPD/2. */
static void eye_pose(const psvr2_frame_t *f, int eye, float pos[3], float quat[4])
{
    const int have_q = f->eye[eye].quat[0] != 0 || f->eye[eye].quat[1] != 0 ||
                       f->eye[eye].quat[2] != 0 || f->eye[eye].quat[3] != 0;
    memcpy(quat, have_q ? f->eye[eye].quat : f->head.quat, 16);
    if (f->eye_pos_valid) {
        memcpy(pos, f->eye[eye].pos, 12);
        return;
    }
    const mat4 r = mat4_quat(f->head.quat);
    const float off = (eye ? 0.5f : -0.5f) * IPD_DEFAULT;
    for (int i = 0; i < 3; i++)
        pos[i] = f->head.pos[i] + r.m[i] * off; /* column 0 = head's right axis */
}

int main(void)
{
    spike_title_init("spike03b", LOG_HOST, LOG_PORT);
    spike_log("ps5webkit spike 03b: GL -> PS VR2 (pid %d)", (int)getpid());
    psvr2_set_logger(log_line);
    if (psvr2_preload() != 0) { /* early: sandbox library path */
        SPIKE_FAIL("preload", "Hmd2/VrTracker2 not bound on this firmware");
        return 1;
    }

    /* GL: pbuffer-backed 4.6 core context; we never present to the TV. */
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    static const EGLint cfg_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                       EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                       EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24,
                                       EGL_NONE};
    static const EGLint ctx_attrs[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 6,
                                       EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
                                       EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE};
    static const EGLint pb_attrs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface pb = EGL_NO_SURFACE;
    if (eglInitialize(dpy, NULL, NULL) && eglBindAPI(EGL_OPENGL_API) &&
        eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n) && n == 1) {
        pb = eglCreatePbufferSurface(dpy, cfg, pb_attrs);
        ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    }
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, pb, pb, ctx)) {
        SPIKE_FAIL("gl-init", "EGL context 0x%04x", eglGetError());
        return 1;
    }
    char err[512];
    g_prog = glu_program(kVs, kFs, err, sizeof err);
    if (!g_prog) {
        SPIKE_FAIL("gl-init", "shader: %s", err);
        return 1;
    }
    u_mvp = glGetUniformLocation(g_prog, "mvp");
    u_model = glGetUniformLocation(g_prog, "model");
    u_color = glGetUniformLocation(g_prog, "color");
    u_lit = glGetUniformLocation(g_prog, "lit");
    build_world();
    SPIKE_PASS("gl-init", "%s", (const char *)glGetString(GL_VERSION));

    int user = -1;
    sceUserServiceInitialize(NULL);
    sceUserServiceGetInitialUser(&user);
    if (psvr2_open(user) != 0) {
        SPIKE_FAIL("open", "headset setup failed (user 0x%x)", user);
        return 1;
    }

    /* Eye targets: three textures + one depth buffer. */
    const GLsizei W = 2 * EYE, H = EYE;
    GLuint tex[PSVR2_MAX_BUFFERS], fbo[PSVR2_MAX_BUFFERS], depth;
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
    glGenTextures(PSVR2_MAX_BUFFERS, tex);
    glGenFramebuffers(PSVR2_MAX_BUFFERS, fbo);
    for (int b = 0; b < PSVR2_MAX_BUFFERS; b++) {
        glBindTexture(GL_TEXTURE_2D, tex[b]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[b]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[b], 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    }

    uint32_t t[PSVR2_MAX_BUFFERS][8];
    export_tsharp_fn exp = (export_tsharp_fn)eglGetProcAddress("eglExportTextureDescriptorPS5");
    int zerocopy = exp != NULL;
    uint8_t *linear = NULL;
    uint32_t pitch_px = 0;
    uint64_t one = 0;
    for (int b = 0; zerocopy && b < PSVR2_MAX_BUFFERS; b++) {
        uint64_t addr = 0;
        uint32_t w, h, pitch, sw;
        if (!exp(dpy, ctx, tex[b], t[b], &addr, &w, &h, &pitch, &sw)) {
            spike_log("export of texture %d refused (0x%04x): falling back to copy", b, eglGetError());
            zerocopy = 0;
        } else {
            spike_log("texture %d: addr 0x%llx %ux%u pitch %u sw_mode %u", b,
                      (unsigned long long)addr, w, h, pitch, sw);
        }
    }
    if (!zerocopy) {
        pitch_px = ((uint32_t)W + 255) & ~255u;
        one = ((uint64_t)pitch_px * 4 * H + 0xffff) & ~0xffffull;
        linear = psvr2_direct_alloc(one * PSVR2_MAX_BUFFERS, 0x10000);
        if (!linear) {
            SPIKE_FAIL("start", "no direct memory for the copy path");
            psvr2_close();
            return 1;
        }
        for (int b = 0; b < PSVR2_MAX_BUFFERS; b++)
            psvr2_build_tsharp_rgba8(t[b], (uint64_t)(uintptr_t)(linear + b * one), (uint32_t)W,
                                     (uint32_t)H, pitch_px * 4, 0);
    }
    const char *path = zerocopy ? "zerocopy" : "copy";
    if (psvr2_start(t, PSVR2_MAX_BUFFERS, EYE, EYE) != 0) {
        SPIKE_FAIL("start", "VR mode / registration failed (%s path)", path);
        psvr2_close();
        return 1;
    }
    SPIKE_PASS("start", "%s path, %dx%d eye buffers", path, W, H);

    float tan[2][4];
    psvr2_eye_tangents(0, OVERSCAN, tan[0]);
    psvr2_eye_tangents(1, OVERSCAN, tan[1]);
    float anchor[3] = {0, 0, 0};
    int anchored = 0;
    unsigned frames = 0, errors = 0, missed = 0, tracked = 0;
    double render_ms = 0, handoff_ms = 0, worst = 0;
    glEnable(GL_DEPTH_TEST);
    const double t0 = spike_now_ms();
    while (spike_now_ms() - t0 < SECONDS * 1000.0) {
        psvr2_frame_t f;
        if (psvr2_begin_frame(&f) != 0) {
            errors++;
            continue;
        }
        if (f.tracked && !anchored) {
            memcpy(anchor, f.head.pos, sizeof anchor);
            anchored = 1;
            spike_log("anchor (%.2f %.2f %.2f), eye positions from tracker: %d", (double)anchor[0],
                      (double)anchor[1], (double)anchor[2], f.eye_pos_valid);
        }
        tracked += f.tracked != 0;
        const int b = (int)(f.frame % PSVR2_MAX_BUFFERS);
        const double r0 = spike_now_ms();
        glBindFramebuffer(GL_FRAMEBUFFER, zerocopy ? fbo[b] : fbo[0]);
        glClearColor(0.05f, 0.05f, 0.08f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        for (int eye = 0; eye < 2; eye++) {
            float pos[3], quat[4];
            eye_pose(&f, eye, pos, quat);
            glViewport(eye * EYE, 0, EYE, EYE);
            /* flip_y: memory row 0 must be the image's top for the compositor */
            const mat4 vp = mat4_mul(mat4_frustum_tan(tan[eye], 0.05f, 100.0f, 1),
                                     mat4_view_from_pose(pos, quat));
            draw_world(vp, anchor, f.frame);
        }
        const double r1 = spike_now_ms();
        if (zerocopy) {
            glFinish(); /* the compositor must not sample a half-drawn buffer */
        } else {
            glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)pitch_px);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, linear + b * one);
            glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        }
        const double r2 = spike_now_ms();
        if (psvr2_submit(b, &f, tan) != 0)
            errors++;
        render_ms += r1 - r0;
        handoff_ms += r2 - r1;
        if (r2 - r0 > worst)
            worst = r2 - r0;
        missed += r2 - r0 > 16.0;
        frames++;
    }
    const double secs = (spike_now_ms() - t0) / 1000.0;
    psvr2_close();

    const double fps = frames / secs;
    SPIKE_RESULT("render", fps > 55 && missed < frames / 50 ? "PASS" : "FAIL",
                 "%s: %u frames, %.1f fps, render %.2f ms + hand-off %.2f ms avg, worst %.1f ms, "
                 "%u over budget", path, frames, fps, frames ? render_ms / frames : 0,
                 frames ? handoff_ms / frames : 0, worst, missed);
    SPIKE_RESULT("submit", errors ? "FAIL" : "PASS", "%u errors", errors);
    SPIKE_RESULT("tracking", tracked > frames / 2 ? "PASS" : "FAIL", "%u/%u frames tracked", tracked,
                 frames);
    spike_log("VISUAL CHECK: world-locked cube ring + floor grid, correct depth (not cross-eyed),"
              " no judder when turning? Record in FEASIBILITY.md (03b %s visual).", path);
    return 0;
}
