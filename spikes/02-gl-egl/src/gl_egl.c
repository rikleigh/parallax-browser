/* Spike 2 - what ps5-opengl's EGL/GL can give WebKit.
 *
 * WPE WebKit composites with EGL + GLES and draws WebGL through ANGLE; its
 * WebXR backend wants GLES contexts too. This title probes, on the console:
 *
 *   egl-init         display, version, client APIs, extensions
 *   gl-pbuffer       a desktop GL 4.6 core context on a pbuffer (no window)
 *   surfaceless      eglMakeCurrent with no surface (EGL_KHR_surfaceless_context)
 *   fbo-render       draw into a texture FBO and read it back (offscreen)
 *   gles-context     an OpenGL ES 3 context: FAIL on stock ps5-opengl,
 *                    expected to PASS with spikes/02-gl-egl/ps5-opengl patches
 *   gles-render      #version 300 es shaders drawing into an FBO
 *   shared-thread    a shared context on a second thread renders a texture
 *                    the first context samples, synchronised with fences
 *   egl-image        EGLImage entry points (WebKit's texture sharing path)
 *   export-tsharp    the zero-copy export for the PS VR2 hand-off (patch)
 *   webgl-load       a WebGL-like workload: 2000 small draws/frame with
 *                    uniform changes, ms per frame
 *
 * Gate: GLES2+ contexts (directly or via ANGLE on GL) and offscreen
 * rendering work.
 *
 * Runs as a registered title: logs go to tools/logserver.sh on the Mac
 * (LOG_HOST/LOG_PORT set at build time). Build with -DWITH_PROCS_TESTS to
 * also run Spike 1's process/IPC tests from title context.
 */
#define SPIKE_NAME "02-gl-egl"
#include "../../common/title.h"
#include "../../common/glutil.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef LOG_HOST
#define LOG_HOST "192.168.1.100"
#endif
#ifndef LOG_PORT
#define LOG_PORT 9999
#endif

#ifdef WITH_PROCS_TESTS
void procs_ipc_run_all(void);
#endif

static EGLDisplay g_dpy;
static EGLConfig g_cfg;

static const char *kVs330 =
    "#version 330 core\n"
    "layout(location=0) in vec2 p;\n"
    "uniform vec4 xform;\n" /* xy offset, zw scale */
    "void main() { gl_Position = vec4(p * xform.zw + xform.xy, 0.0, 1.0); }\n";
static const char *kFs330 =
    "#version 330 core\n"
    "uniform vec4 color;\n"
    "out vec4 o;\n"
    "void main() { o = color; }\n";
static const char *kVsEs =
    "#version 300 es\n"
    "layout(location=0) in vec2 p;\n"
    "void main() { gl_Position = vec4(p, 0.0, 1.0); }\n";
static const char *kFsEs =
    "#version 300 es\n"
    "precision mediump float;\n"
    "out vec4 o;\n"
    "void main() { o = vec4(0.0, 1.0, 0.0, 1.0); }\n";

static const float kQuad[] = {-1, -1, 1, -1, -1, 1, 1, 1};

static int check_gl(const char *what)
{
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        spike_log("  GL error 0x%04x after %s", e, what);
        return -1;
    }
    return 0;
}

static GLuint make_fbo_texture(GLsizei w, GLsizei h, GLuint *fbo)
{
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    return tex;
}

static void test_egl_init(void)
{
    EGLint major = 0, minor = 0;
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_dpy == EGL_NO_DISPLAY || !eglInitialize(g_dpy, &major, &minor)) {
        SPIKE_FAIL("egl-init", "eglInitialize 0x%04x", eglGetError());
        return;
    }
    SPIKE_PASS("egl-init", "EGL %d.%d vendor \"%s\" version \"%s\" apis \"%s\"", major, minor,
               eglQueryString(g_dpy, EGL_VENDOR), eglQueryString(g_dpy, EGL_VERSION),
               eglQueryString(g_dpy, EGL_CLIENT_APIS));
    spike_log("  EGL extensions: %s", eglQueryString(g_dpy, EGL_EXTENSIONS));
}

static EGLContext make_gl_context(EGLContext share)
{
    static const EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 6,
                                   EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
                                   EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE};
    eglBindAPI(EGL_OPENGL_API);
    return eglCreateContext(g_dpy, g_cfg, share, attrs);
}

static EGLContext g_ctx;
static EGLSurface g_pbuf;

static void test_gl_pbuffer(void)
{
    static const EGLint cfg_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                       EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                       EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    static const EGLint pb_attrs[] = {EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(g_dpy, cfg_attrs, &g_cfg, 1, &n) || n != 1) {
        SPIKE_FAIL("gl-pbuffer", "no pbuffer config (0x%04x)", eglGetError());
        return;
    }
    g_pbuf = eglCreatePbufferSurface(g_dpy, g_cfg, pb_attrs);
    g_ctx = make_gl_context(EGL_NO_CONTEXT);
    if (g_pbuf == EGL_NO_SURFACE || g_ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent(g_dpy, g_pbuf, g_pbuf, g_ctx)) {
        SPIKE_FAIL("gl-pbuffer", "pbuffer %p context %p error 0x%04x", g_pbuf, g_ctx, eglGetError());
        return;
    }
    GLint next = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &next);
    SPIKE_PASS("gl-pbuffer", "GL \"%s\" renderer \"%s\" GLSL \"%s\", %d extensions",
               (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER),
               (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION), next);
    /* The extensions ANGLE's GL backend and WebGL 2 lean on. */
    static const char *const kWanted[] = {"GL_ARB_ES3_compatibility", "GL_ARB_ES3_1_compatibility",
                                          "GL_ARB_ES3_2_compatibility", "GL_KHR_debug",
                                          "GL_ARB_buffer_storage", "GL_EXT_texture_compression_s3tc",
                                          "GL_ARB_texture_storage", "GL_ARB_sync"};
    for (size_t w = 0; w < sizeof kWanted / sizeof *kWanted; w++) {
        int found = 0;
        for (GLint i = 0; i < next && !found; i++)
            found = !strcmp((const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i), kWanted[w]);
        spike_log("  %-34s %s", kWanted[w], found ? "yes" : "NO");
    }
}

static void test_surfaceless(void)
{
    if (g_ctx == EGL_NO_CONTEXT) {
        SPIKE_SKIP("surfaceless", "no context");
        return;
    }
    EGLBoolean ok = eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_ctx);
    if (ok)
        SPIKE_PASS("surfaceless", "context current with no surface");
    else
        SPIKE_FAIL("surfaceless", "0x%04x", eglGetError());
    eglMakeCurrent(g_dpy, g_pbuf, g_pbuf, g_ctx);
}

/* Draws a red background and a green centre quad; checks both. */
static int draw_and_verify(GLuint prog, GLsizei w, GLsizei h, int es)
{
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof kQuad, kQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glViewport(0, 0, w, h);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog);
    if (!es) {
        glUniform4f(glGetUniformLocation(prog, "xform"), 0, 0, 0.5f, 0.5f);
        glUniform4f(glGetUniformLocation(prog, "color"), 0, 1, 0, 1);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    uint8_t centre[4] = {0}, corner[4] = {0};
    glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, centre);
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    const int ok = centre[1] > 200 && centre[0] < 50 && (es || (corner[0] > 200 && corner[1] < 50));
    if (!ok)
        spike_log("  centre %02x%02x%02x%02x corner %02x%02x%02x%02x", centre[0], centre[1],
                  centre[2], centre[3], corner[0], corner[1], corner[2], corner[3]);
    return ok && check_gl("draw") == 0;
}

static GLuint g_prog;

static void test_fbo_render(void)
{
    if (g_ctx == EGL_NO_CONTEXT) {
        SPIKE_SKIP("fbo-render", "no context");
        return;
    }
    char err[512];
    g_prog = glu_program(kVs330, kFs330, err, sizeof err);
    if (!g_prog) {
        SPIKE_FAIL("fbo-render", "shader: %s", err);
        return;
    }
    GLuint fbo, tex = make_fbo_texture(1024, 1024, &fbo);
    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    const double t0 = spike_now_ms();
    const int ok = st == GL_FRAMEBUFFER_COMPLETE && draw_and_verify(g_prog, 1024, 1024, 0);
    if (ok)
        SPIKE_PASS("fbo-render", "1024x1024 RGBA8 texture FBO drawn and read back in %.2f ms",
                   spike_now_ms() - t0);
    else
        SPIKE_FAIL("fbo-render", "framebuffer status 0x%04x", st);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
}

static void test_gles(void)
{
    static const EGLint cfg_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                       EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_NONE};
    static const EGLint ctx_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        SPIKE_FAIL("gles-context", "eglBindAPI(EGL_OPENGL_ES_API) refused (0x%04x): stock "
                   "ps5-opengl; WebGL needs ANGLE-on-GL or the GLES patch", eglGetError());
        SPIKE_SKIP("gles-render", "no GLES context");
        eglBindAPI(EGL_OPENGL_API);
        return;
    }
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(g_dpy, cfg_attrs, &cfg, 1, &n) || n != 1)
        cfg = g_cfg; /* try the GL config anyway */
    EGLContext es = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (es == EGL_NO_CONTEXT || !eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, es)) {
        SPIKE_FAIL("gles-context", "ES 3 context 0x%04x", eglGetError());
        SPIKE_SKIP("gles-render", "no GLES context");
        eglBindAPI(EGL_OPENGL_API);
        eglMakeCurrent(g_dpy, g_pbuf, g_pbuf, g_ctx);
        return;
    }
    SPIKE_PASS("gles-context", "\"%s\" GLSL \"%s\"", (const char *)glGetString(GL_VERSION),
               (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
    char err[512];
    GLuint p = glu_program(kVsEs, kFsEs, err, sizeof err);
    GLuint fbo, tex = make_fbo_texture(256, 256, &fbo);
    if (p && draw_and_verify(p, 256, 256, 1))
        SPIKE_PASS("gles-render", "#version 300 es program drew into an FBO");
    else
        SPIKE_FAIL("gles-render", "%s", p ? "wrong pixels" : err);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(g_dpy, es);
    eglBindAPI(EGL_OPENGL_API);
    eglMakeCurrent(g_dpy, g_pbuf, g_pbuf, g_ctx);
}

/* -- a shared context on another thread (WebKit's compositor/WebGL split) - */
typedef struct {
    EGLContext ctx;
    GLuint tex;
    GLsync fence;
    int ok;
} share_job_t;

static void *share_thread(void *arg)
{
    share_job_t *j = arg;
    if (!eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, j->ctx))
        return NULL;
    char err[256];
    GLuint p = glu_program(kVs330, kFs330, err, sizeof err);
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, j->tex, 0);
    j->ok = p && draw_and_verify(p, 512, 512, 0);
    j->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    glDeleteFramebuffers(1, &fbo);
    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return NULL;
}

static void test_shared_thread(void)
{
    if (g_ctx == EGL_NO_CONTEXT || !g_prog) {
        SPIKE_SKIP("shared-thread", "no context");
        return;
    }
    share_job_t j = {make_gl_context(g_ctx), 0, 0, 0};
    if (j.ctx == EGL_NO_CONTEXT) {
        SPIKE_FAIL("shared-thread", "shared context 0x%04x", eglGetError());
        return;
    }
    glGenTextures(1, &j.tex);
    glBindTexture(GL_TEXTURE_2D, j.tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 512, 512);
    glFinish();
    /* Mesa's shader linker (nir_opt_varyings) keeps a very large struct on
     * the stack: on the PS5's default pthread stack, linking a program on
     * this thread overflowed it (SIGSEGV in nir_opt_varyings, run 2). Any
     * thread that compiles shaders - WebKit's compositor and WebGL threads
     * included - needs a big stack. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_t t;
    pthread_create(&t, &attr, share_thread, &j);
    pthread_attr_destroy(&attr);
    pthread_join(t, NULL);
    /* Back on this context: wait for the other's fence, read its texture. */
    int seen = 0;
    if (j.fence) {
        glWaitSync(j.fence, 0, GL_TIMEOUT_IGNORED);
        GLuint fbo;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, j.tex, 0);
        uint8_t px[4] = {0};
        glReadPixels(256, 256, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        seen = px[1] > 200 && px[0] < 50;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteSync(j.fence);
    }
    if (j.ok && seen)
        SPIKE_PASS("shared-thread", "texture drawn on thread 2's shared context, read on thread 1");
    else
        SPIKE_FAIL("shared-thread", "thread draw %d, visible on main context %d", j.ok, seen);
    glDeleteTextures(1, &j.tex);
    eglDestroyContext(g_dpy, j.ctx);
}

static void test_egl_image(void)
{
    void *create = (void *)eglGetProcAddress("eglCreateImageKHR");
    void *create15 = (void *)eglGetProcAddress("eglCreateImage");
    void *target = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    void *dmabuf = (void *)eglGetProcAddress("eglExportDMABUFImageMESA");
    if (create || create15)
        SPIKE_PASS("egl-image", "eglCreateImage(KHR) %p/%p, target %p, dmabuf export %p", create,
                   create15, target, dmabuf);
    else
        SPIKE_FAIL("egl-image", "no EGLImage: WebKit's DMA-buf/EGLImage sharing needs a PS5 "
                   "replacement (shared contexts or the T# export)");
}

typedef EGLBoolean (*export_tsharp_fn)(EGLDisplay, EGLContext, GLuint, uint32_t[8], uint64_t *,
                                       uint32_t *, uint32_t *, uint32_t *, uint32_t *);

static void test_export_tsharp(void)
{
    export_tsharp_fn fn = (export_tsharp_fn)eglGetProcAddress("eglExportTextureDescriptorPS5");
    if (!fn) {
        SPIKE_SKIP("export-tsharp", "eglExportTextureDescriptorPS5 absent (stock ps5-opengl)");
        return;
    }
    GLuint fbo, tex = make_fbo_texture(2048, 1024, &fbo);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    uint32_t t[8];
    uint64_t addr = 0;
    uint32_t w = 0, h = 0, pitch = 0, sw = 0;
    if (fn(g_dpy, g_ctx, tex, t, &addr, &w, &h, &pitch, &sw))
        SPIKE_PASS("export-tsharp", "addr 0x%llx %ux%u pitch %u sw_mode %u T# %08x %08x %08x %08x",
                   (unsigned long long)addr, w, h, pitch, sw, t[0], t[1], t[2], t[3]);
    else
        SPIKE_FAIL("export-tsharp", "export refused (0x%04x)", eglGetError());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
}

static void test_webgl_load(void)
{
    if (!g_prog) {
        SPIKE_SKIP("webgl-load", "no program");
        return;
    }
    GLuint fbo, tex = make_fbo_texture(1920, 1080, &fbo);
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof kQuad, kQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glUseProgram(g_prog);
    const GLint xf = glGetUniformLocation(g_prog, "xform"), col = glGetUniformLocation(g_prog, "color");
    glViewport(0, 0, 1920, 1080);
    enum { FRAMES = 120, DRAWS = 2000 };
    glFinish();
    const double t0 = spike_now_ms();
    for (int f = 0; f < FRAMES; f++) {
        glClear(GL_COLOR_BUFFER_BIT);
        for (int d = 0; d < DRAWS; d++) {
            const float a = (float)(d * 37 % 1000) / 500.0f - 1.0f, b = (float)(d * 91 % 1000) / 500.0f - 1.0f;
            glUniform4f(xf, a, b, 0.02f, 0.02f);
            glUniform4f(col, (float)(d & 255) / 255.0f, 0.5f, (float)f / FRAMES, 1);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        glFinish(); /* the cost a browser frame really pays */
    }
    const double ms = (spike_now_ms() - t0) / FRAMES;
    if (check_gl("webgl-load") == 0)
        SPIKE_RESULT("webgl-load", ms < 16.7 ? "PASS" : "FAIL",
                     "%d draws + 2 uniform updates each at 1080p: %.2f ms/frame (%.0f fps)", DRAWS,
                     ms, 1000.0 / ms);
    else
        SPIKE_FAIL("webgl-load", "GL error during the workload");
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
}

int main(void)
{
    spike_title_init("spike02", LOG_HOST, LOG_PORT);
    spike_notify("ps5webkit: spike 02 running");
    spike_log("ps5webkit spike 02: EGL/GL capabilities (pid %d)", (int)getpid());
    test_egl_init();
    if (g_dpy != EGL_NO_DISPLAY) {
        test_gl_pbuffer();
        test_surfaceless();
        test_fbo_render();
        test_gles();
        test_shared_thread();
        test_egl_image();
        test_export_tsharp();
        test_webgl_load();
        eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglTerminate(g_dpy);
    }
#ifdef WITH_PROCS_TESTS
    procs_ipc_run_all();
#endif
    spike_log("done");
    spike_notify("ps5webkit: spike 02 done");
    return 0;
}
