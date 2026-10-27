/* Spike 2b - where ps5-opengl's per-draw time goes.
 *
 * Spike 2's webgl-load (2000 x {2 glUniform4f + glDrawArrays} + glFinish)
 * took 132 ms per frame: ~66 us per draw, where a desktop driver needs a few.
 * WebGL (through ANGLE's GL backend) and three.js issue exactly this kind of
 * stream, so this splits the cost:
 *
 *   draw-only        2000 draws, no state change between them
 *   uniform-only     2000 x 2 glUniform4f, one draw at the end
 *   uniform-draw     the Spike 2 workload (baseline)
 *   triangles        the same with GL_TRIANGLES (6 vertices), no strip
 *   indexed          the same with glDrawElements (uint16 indices)
 *   ubo-subdata      glBufferSubData of a 32-byte UBO + draw
 *   ubo-range        glBindBufferRange into a pre-filled UBO + draw
 *   instanced        one glDrawArraysInstanced of 2000 instances
 *   scale-N          uniform-draw at 250/1000/4000 draws (linearity)
 *   small-target     uniform-draw into a 64x64 target (fill rate excluded)
 *   gles             uniform-draw on a GLES 3.2 context (the WebGL route)
 *   glthread-*       draw-only and uniform-draw on a context made with
 *                    PS5_GLTHREAD=1 (Mesa's glthread: the driver runs on
 *                    its own thread, the caller only records commands)
 *
 * The driver's own stdout (its profile reports in profiling builds) goes to
 * /data/ps5webkit/spike02b-<build>-driver.log; fetch it over FTP.
 *
 * Each case reports issue time (wall and thread CPU, per draw), the
 * glFinish wait, and the whole frame. Issue >> CPU means the driver blocks
 * inside GL calls; finish >> 0 with fast issue means GPU-side cost.
 */
#define SPIKE_NAME "02b-draw-bench"
#include "../../common/title.h"
#include "../../common/glutil.h"

#include <stdint.h>
#include <stdlib.h>

#ifndef LOG_HOST
#define LOG_HOST "192.168.1.20"
#define LOG_PORT 9999
#endif

#ifndef BENCH_VARIANT
#define BENCH_VARIANT "?"
#endif

enum { FRAMES = 40, WARMUP = 3, DRAWS = 2000 };

static EGLDisplay g_dpy;
static EGLConfig g_cfg;

static const char *kVs =
    "layout(location=0) in vec2 p;\n"
    "uniform vec4 xform;\n"
    "void main() { gl_Position = vec4(p * xform.zw + xform.xy, 0.0, 1.0); }\n";
static const char *kFs =
    "uniform vec4 color;\n"
    "out vec4 o;\n"
    "void main() { o = color; }\n";
static const char *kVsUbo =
    "layout(location=0) in vec2 p;\n"
    "layout(std140) uniform Draw { vec4 xform; vec4 color; } u;\n"
    "out vec4 c;\n"
    "void main() { c = u.color; gl_Position = vec4(p * u.xform.zw + u.xform.xy, 0.0, 1.0); }\n";
static const char *kVsInst =
    "layout(location=0) in vec2 p;\n"
    "layout(location=1) in vec4 xform;\n"
    "layout(location=2) in vec4 color;\n"
    "out vec4 c;\n"
    "void main() { c = color; gl_Position = vec4(p * xform.zw + xform.xy, 0.0, 1.0); }\n";
static const char *kFsVarying =
    "in vec4 c;\n"
    "out vec4 o;\n"
    "void main() { o = c; }\n";

static const float kStrip[] = {-1, -1, 1, -1, -1, 1, 1, 1};
static const float kTris[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
static const uint16_t kIdx[] = {0, 1, 2, 2, 1, 3};

typedef struct {
    GLuint prog, prog_ubo, prog_inst;
    GLint xf, col;
    GLuint vao_strip, vao_tris, vao_inst, ubo, ubo_big;
    GLint ubo_align;
} scene_t;
static scene_t g;

typedef enum {
    K_DRAW_ONLY, K_UNIFORM_ONLY, K_UNIFORM_DRAW, K_TRIANGLES, K_INDEXED,
    K_UBO_SUBDATA, K_UBO_RANGE, K_INSTANCED,
} kind_t;

static inline void pos(int d, float *a, float *b)
{
    *a = (float)(d * 37 % 1000) / 500.0f - 1.0f;
    *b = (float)(d * 91 % 1000) / 500.0f - 1.0f;
}

static void issue(kind_t k, int f, int draws)
{
    glClear(GL_COLOR_BUFFER_BIT);
    float a, b;
    switch (k) {
    case K_DRAW_ONLY:
        glUseProgram(g.prog);
        glBindVertexArray(g.vao_strip);
        glUniform4f(g.xf, 0, 0, 0.02f, 0.02f);
        glUniform4f(g.col, 1, 0.5f, 0, 1);
        for (int d = 0; d < draws; d++)
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        break;
    case K_UNIFORM_ONLY:
        glUseProgram(g.prog);
        glBindVertexArray(g.vao_strip);
        for (int d = 0; d < draws; d++) {
            pos(d, &a, &b);
            glUniform4f(g.xf, a, b, 0.02f, 0.02f);
            glUniform4f(g.col, (float)(d & 255) / 255.0f, 0.5f, (float)f / FRAMES, 1);
        }
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        break;
    case K_UNIFORM_DRAW:
    case K_TRIANGLES:
    case K_INDEXED:
        glUseProgram(g.prog);
        glBindVertexArray(k == K_UNIFORM_DRAW ? g.vao_strip : g.vao_tris);
        for (int d = 0; d < draws; d++) {
            pos(d, &a, &b);
            glUniform4f(g.xf, a, b, 0.02f, 0.02f);
            glUniform4f(g.col, (float)(d & 255) / 255.0f, 0.5f, (float)f / FRAMES, 1);
            if (k == K_UNIFORM_DRAW)
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            else if (k == K_TRIANGLES)
                glDrawArrays(GL_TRIANGLES, 0, 6);
            else
                glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        }
        break;
    case K_UBO_SUBDATA:
        glUseProgram(g.prog_ubo);
        glBindVertexArray(g.vao_strip);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, g.ubo);
        for (int d = 0; d < draws; d++) {
            pos(d, &a, &b);
            const float u[8] = {a, b, 0.02f, 0.02f, (float)(d & 255) / 255.0f, 0.5f, (float)f / FRAMES, 1};
            glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof u, u);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        break;
    case K_UBO_RANGE:
        glUseProgram(g.prog_ubo);
        glBindVertexArray(g.vao_strip);
        for (int d = 0; d < draws; d++) {
            glBindBufferRange(GL_UNIFORM_BUFFER, 0, g.ubo_big, (GLintptr)d * g.ubo_align, 32);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        break;
    case K_INSTANCED:
        glUseProgram(g.prog_inst);
        glBindVertexArray(g.vao_inst);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, draws);
        break;
    }
}

static double cpu_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return -1;
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int check_gl(const char *what)
{
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        spike_log("  GL error 0x%04x in %s", e, what);
        return -1;
    }
    return 0;
}

/* Returns ms per frame. */
static double bench(const char *name, kind_t k, int draws)
{
    double first = 0, issue_ms = 0, issue_cpu = 0, finish_ms = 0;
    glFinish();
    for (int f = 0; f < WARMUP + FRAMES; f++) {
        const double w0 = spike_now_ms(), c0 = cpu_ms();
        issue(k, f, draws);
        const double w1 = spike_now_ms(), c1 = cpu_ms();
        glFinish();
        const double w2 = spike_now_ms();
        if (f == 0)
            first = w2 - w0;
        if (f < WARMUP)
            continue;
        issue_ms += w1 - w0;
        issue_cpu += c1 - c0;
        finish_ms += w2 - w1;
    }
    issue_ms /= FRAMES;
    issue_cpu /= FRAMES;
    finish_ms /= FRAMES;
    const double frame = issue_ms + finish_ms;
    const int ops = k == K_INSTANCED ? 1 : draws;
    if (check_gl(name) != 0) {
        SPIKE_FAIL(name, "GL error");
        return frame;
    }
    SPIKE_RESULT(name, "PASS",
                 "%d %s: frame %.2f ms = issue %.2f (cpu %.2f) + finish %.2f; per call %.1f us "
                 "(cpu %.1f); first frame %.1f ms",
                 draws, k == K_INSTANCED ? "instances" : "draws", frame, issue_ms, issue_cpu,
                 finish_ms, issue_ms * 1e3 / ops, issue_cpu * 1e3 / ops, first);
    return frame;
}

static GLuint program(const char *version, const char *vs, const char *fs)
{
    char vsrc[1024], fsrc[1024], err[512];
    snprintf(vsrc, sizeof vsrc, "%s%s", version, vs);
    snprintf(fsrc, sizeof fsrc, "%s%s", version, fs);
    GLuint p = glu_program(vsrc, fsrc, err, sizeof err);
    if (!p)
        spike_log("  shader: %s", err);
    return p;
}

static GLuint vao_with(const float *v, size_t size)
{
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)size, v, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    return vao;
}

static int setup(const char *version, int full)
{
    memset(&g, 0, sizeof g);
    g.prog = program(version, kVs, kFs);
    if (!g.prog)
        return -1;
    g.xf = glGetUniformLocation(g.prog, "xform");
    g.col = glGetUniformLocation(g.prog, "color");
    g.vao_strip = vao_with(kStrip, sizeof kStrip);
    g.vao_tris = vao_with(kTris, sizeof kTris);
    if (!full)
        return 0;
    /* indexed draws use the strip's 4 vertices through kIdx */
    glBindVertexArray(g.vao_tris);
    GLuint ibo;
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof kIdx, kIdx, GL_STATIC_DRAW);

    g.prog_ubo = program(version, kVsUbo, kFsVarying);
    g.prog_inst = program(version, kVsInst, kFsVarying);
    if (!g.prog_ubo || !g.prog_inst)
        return -1;
    glUniformBlockBinding(g.prog_ubo, glGetUniformBlockIndex(g.prog_ubo, "Draw"), 0);
    glGenBuffers(1, &g.ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, g.ubo);
    glBufferData(GL_UNIFORM_BUFFER, 32, NULL, GL_DYNAMIC_DRAW);
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &g.ubo_align);
    if (g.ubo_align < 32)
        g.ubo_align = 32;
    const size_t big = (size_t)g.ubo_align * 4000;
    float *u = calloc(1, big);
    float *inst = malloc(sizeof(float) * 8 * 4000);
    for (int d = 0; d < 4000 && u && inst; d++) {
        float a, b;
        pos(d, &a, &b);
        const float row[8] = {a, b, 0.02f, 0.02f, (float)(d & 255) / 255.0f, 0.5f, 0.5f, 1};
        memcpy((char *)u + (size_t)d * g.ubo_align, row, sizeof row);
        memcpy(inst + d * 8, row, sizeof row);
    }
    glGenBuffers(1, &g.ubo_big);
    glBindBuffer(GL_UNIFORM_BUFFER, g.ubo_big);
    glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)big, u, GL_STATIC_DRAW);
    g.vao_inst = vao_with(kStrip, sizeof kStrip);
    GLuint ivbo;
    glGenBuffers(1, &ivbo);
    glBindBuffer(GL_ARRAY_BUFFER, ivbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 8 * 4000, inst, GL_STATIC_DRAW);
    for (int i = 0; i < 2; i++) {
        glEnableVertexAttribArray(1 + i);
        glVertexAttribPointer(1 + i, 4, GL_FLOAT, GL_FALSE, 32, (void *)(uintptr_t)(16 * i));
        glVertexAttribDivisor(1 + i, 1);
    }
    free(u);
    free(inst);
    spike_log("  UBO offset alignment %d", g.ubo_align);
    return check_gl("setup");
}

static GLuint target(GLsizei w, GLsizei h)
{
    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    return fbo;
}

static EGLContext make_context(EGLenum api, int major, int minor, int core)
{
    EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, major, EGL_CONTEXT_MINOR_VERSION_KHR, minor,
                      EGL_NONE, EGL_NONE, EGL_NONE};
    if (core) {
        attrs[4] = EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR;
        attrs[5] = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR;
    }
    eglBindAPI(api);
    EGLContext c = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, attrs);
    if (c == EGL_NO_CONTEXT || !eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, c))
        return EGL_NO_CONTEXT;
    return c;
}

int main(void)
{
    spike_title_init("spike02b", LOG_HOST, LOG_PORT);
    spike_log("ps5webkit spike 02b: draw-call cost, driver build %s (pid %d)", BENCH_VARIANT,
              (int)getpid());
    /* ps5-opengl points stdout at /download0, which FTP cannot read. Its
     * runtime_shims.c uses freopen(); dup2() onto fileno() left the file
     * empty on 13.60, so do the same as the shim. */
    char path[96];
    snprintf(path, sizeof path, "/data/ps5webkit/spike02b-%s-driver.log", BENCH_VARIANT);
    const int dfd = freopen(path, "w", stdout) ? 0 : -1;
    if (dfd == 0) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        freopen(path, "a", stderr);
    }
    spike_log("  driver output -> %s (%s)", path, dfd >= 0 ? "ok" : "failed");
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    /* One config for both APIs if the driver offers it; Spike 2's GL one otherwise. */
    static const EGLint both[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT | EGL_OPENGL_ES3_BIT, EGL_NONE};
    static const EGLint gl_only[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLint n = 0;
    /* The screen's glthread caps (ps5_screen.c) read PS5_GLTHREAD once, at
     * eglInitialize; each context reads it again when created. Only the
     * glthread-* cases' context is made with it set. */
    setenv("PS5_GLTHREAD", "1", 1);
    const EGLBoolean init_ok = eglInitialize(g_dpy, NULL, NULL);
    unsetenv("PS5_GLTHREAD");
    if (!init_ok ||
        ((!eglChooseConfig(g_dpy, both, &g_cfg, 1, &n) || n != 1) &&
         (!eglChooseConfig(g_dpy, gl_only, &g_cfg, 1, &n) || n != 1))) {
        SPIKE_FAIL("egl", "init/config 0x%04x", eglGetError());
        return 1;
    }
    spike_log("  thread CPU clock %s", cpu_ms() >= 0 ? "available" : "unavailable (cpu = -1)");

    EGLContext gl = make_context(EGL_OPENGL_API, 4, 6, 1);
    if (gl == EGL_NO_CONTEXT || setup("#version 330 core\n", 1) != 0) {
        SPIKE_FAIL("gl-setup", "GL 4.6 context/programs 0x%04x", eglGetError());
        return 1;
    }
    spike_log("  %s / %s", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    target(1920, 1080);
    bench("draw-only", K_DRAW_ONLY, DRAWS);
    bench("uniform-only", K_UNIFORM_ONLY, DRAWS);
    bench("uniform-draw", K_UNIFORM_DRAW, DRAWS);
    bench("triangles", K_TRIANGLES, DRAWS);
    bench("indexed", K_INDEXED, DRAWS);
    bench("ubo-subdata", K_UBO_SUBDATA, DRAWS);
    bench("ubo-range", K_UBO_RANGE, DRAWS);
    bench("instanced", K_INSTANCED, DRAWS);
    bench("scale-250", K_UNIFORM_DRAW, 250);
    bench("scale-1000", K_UNIFORM_DRAW, 1000);
    bench("scale-4000", K_UNIFORM_DRAW, 4000);
    target(64, 64);
    bench("small-target", K_UNIFORM_DRAW, DRAWS);
    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    EGLContext es = make_context(EGL_OPENGL_ES_API, 3, 2, 0);
    if (es == EGL_NO_CONTEXT || setup("#version 300 es\nprecision mediump float;\n", 0) != 0) {
        SPIKE_FAIL("gles", "GLES 3.2 context/program 0x%04x", eglGetError());
    } else {
        target(1920, 1080);
        bench("gles", K_UNIFORM_DRAW, DRAWS);
    }
    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    /* ps5_egl.c starts glthread for contexts created with PS5_GLTHREAD=1. */
    setenv("PS5_GLTHREAD", "1", 1);
    EGLContext threaded = make_context(EGL_OPENGL_API, 4, 6, 1);
    if (threaded == EGL_NO_CONTEXT || setup("#version 330 core\n", 0) != 0) {
        SPIKE_FAIL("glthread", "GL 4.6 context with PS5_GLTHREAD=1: 0x%04x", eglGetError());
    } else {
        target(1920, 1080);
        bench("glthread-draw-only", K_DRAW_ONLY, DRAWS);
        bench("glthread-uniform-draw", K_UNIFORM_DRAW, DRAWS);
    }
    unsetenv("PS5_GLTHREAD");
    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(g_dpy);
    spike_log("done");
    return 0;
}
