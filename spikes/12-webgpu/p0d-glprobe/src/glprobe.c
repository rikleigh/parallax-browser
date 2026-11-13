/* glprobe.c - P0d: does ps5-opengl offer what Dawn's OpenGL ES backend
 * (WebGPU compatibility mode) needs?
 *
 * Runs on an OpenGL ES 3.2 context created the way spike 11's "anchor" is
 * (spikes/11-webgl/src/share_native.c): a GL 4.6 core context like the
 * browser compositor's, and an ES 3.2 context with it as the share context.
 * This file: contexts, EGL, caps, limits, entry points, formats, shader front
 * end (all of which the Mac host harness can run). glprobe_gpu.c: everything
 * that links a program, draws or dispatches (console only).
 *
 * Requirement sources are Dawn's src/dawn/native/opengl at bc34d95b
 * (2026-11-12); NOTES.md has the file/line of each.
 */
#include "glp.h"

#include <stdlib.h>

/* ---- results ----------------------------------------------------------- */

static char g_seen[48][48];
static int g_nseen;

static void seen(const char *test)
{
    if (g_nseen < 48)
        snprintf(g_seen[g_nseen++], sizeof g_seen[0], "%s", test);
}

static void emit(const char *test, const char *status, const char *fmt, va_list ap)
{
    char buf[700];
    vsnprintf(buf, sizeof buf, fmt, ap);
    for (char *p = buf; *p; p++)
        if (*p == '\n' || *p == '\r')
            *p = ' ';
    seen(test);
    probe_emit(test, status, buf);
}

void R(const char *test, int pass, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(test, pass ? "PASS" : "FAIL", fmt, ap);
    va_end(ap);
}

void RSKIP(const char *test, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(test, "SKIP", fmt, ap);
    va_end(ap);
}

static char g_detail[700];
static size_t g_detail_n;

void dreset(void)
{
    g_detail_n = 0;
    g_detail[0] = 0;
}

void dcat(const char *fmt, ...)
{
    va_list ap;
    if (g_detail_n >= sizeof g_detail - 1)
        return;
    va_start(ap, fmt);
    int n = vsnprintf(g_detail + g_detail_n, sizeof g_detail - g_detail_n, fmt, ap);
    va_end(ap);
    if (n > 0)
        g_detail_n += (size_t)n < sizeof g_detail - g_detail_n ? (size_t)n : sizeof g_detail - g_detail_n - 1;
}

const char *dstr(void) { return g_detail; }

/* ---- small GL helpers --------------------------------------------------- */

void *gp(const char *name) { return (void *)eglGetProcAddress(name); }

GLenum glerr(void)
{
    GLenum first = glGetError(), e = first;
    for (int i = 0; e != GL_NO_ERROR && i < 64; i++)
        e = glGetError();
    return first;
}

const char *errname(GLenum e)
{
    static char other[16];
    switch (e) {
    case GL_NO_ERROR: return "ok";
    case GL_INVALID_ENUM: return "INVALID_ENUM";
    case GL_INVALID_VALUE: return "INVALID_VALUE";
    case GL_INVALID_OPERATION: return "INVALID_OPERATION";
    case GL_INVALID_FRAMEBUFFER_OPERATION: return "INVALID_FRAMEBUFFER_OPERATION";
    case GL_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
    }
    snprintf(other, sizeof other, "0x%04x", e);
    return other;
}

static char g_exts[512][64];
static int g_next;

static void load_exts(void)
{
    GLint n = 0;
    g_next = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n && g_next < 512; i++) {
        const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e)
            snprintf(g_exts[g_next++], sizeof g_exts[0], "%s", e);
    }
}

int has_ext(const char *name)
{
    for (int i = 0; i < g_next; i++)
        if (!strcmp(g_exts[i], name))
            return 1;
    return 0;
}

GLuint mk_shader(GLenum type, const char *src, char *log, size_t n)
{
    GLuint s = glCreateShader(type);
    GLint ok = GL_FALSE;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLsizei len = 0;
        if (log && n) {
            int at = snprintf(log, n, "%s shader: ", type == GL_VERTEX_SHADER ? "vertex" :
                              type == GL_FRAGMENT_SHADER ? "fragment" : "compute");
            glGetShaderInfoLog(s, (GLsizei)(n - (size_t)at), &len, log + at);
        }
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_program(GLuint a, GLuint b, char *log, size_t n)
{
    GLuint p = glCreateProgram();
    GLint ok = GL_FALSE;
    glAttachShader(p, a);
    if (b)
        glAttachShader(p, b);
    glLinkProgram(p);
    glDeleteShader(a);
    if (b)
        glDeleteShader(b);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLsizei len = 0;
        if (log && n) {
            int at = snprintf(log, n, "link: ");
            glGetProgramInfoLog(p, (GLsizei)(n - (size_t)at), &len, log + at);
        }
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

GLuint mk_program(const char *vs, const char *fs, char *log, size_t n)
{
    if (log && n)
        log[0] = 0;
    GLuint v = mk_shader(GL_VERTEX_SHADER, vs, log, n);
    GLuint f = v ? mk_shader(GL_FRAGMENT_SHADER, fs, log, n) : 0;
    if (!v || !f) {
        if (v)
            glDeleteShader(v);
        return 0;
    }
    return link_program(v, f, log, n);
}

GLuint mk_compute(const char *cs, char *log, size_t n)
{
    if (log && n)
        log[0] = 0;
    GLuint c = mk_shader(GL_COMPUTE_SHADER, cs, log, n);
    return c ? link_program(c, 0, log, n) : 0;
}

void reset_state(void)
{
    glUseProgram(0);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SAMPLE_MASK);
    glDisable(GL_SAMPLE_SHADING);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glStencilMask(0xff);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glViewport(0, 0, W, H);
    glerr();
}

/* ---- contexts ----------------------------------------------------------- */

static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLContext g_core = EGL_NO_CONTEXT;   /* GL 4.6 core, like PS5Renderer::initializeGL */
static EGLContext g_anchor = EGL_NO_CONTEXT; /* ES 3.2 in its share group: the probe's context */
static EGLint g_egl_major, g_egl_minor;

static int make_anchor_current(void)
{
    return eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_anchor) == EGL_TRUE;
}

/* As share_native_create + share_native_create_anchor in spike 11. */
static int setup_contexts(void)
{
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_dpy == EGL_NO_DISPLAY || !eglInitialize(g_dpy, &g_egl_major, &g_egl_minor)) {
        R("ctx-core46", 0, "eglInitialize 0x%04x", eglGetError());
        return 0;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        R("ctx-core46", 0, "eglBindAPI(EGL_OPENGL_API) 0x%04x", eglGetError());
        return 0;
    }
    static const EGLint gl_cfg[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    static const EGLint gl_ctx[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 6,
                                    EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
                                    EGL_NONE};
    EGLConfig cfg;
    EGLint count = 0;
    if (!eglChooseConfig(g_dpy, gl_cfg, &cfg, 1, &count) || count != 1) {
        R("ctx-core46", 0, "no OpenGL config (0x%04x)", eglGetError());
        return 0;
    }
    g_core = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, gl_ctx);
    if (g_core == EGL_NO_CONTEXT || !eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_core)) {
        R("ctx-core46", 0, "4.6 core context %p, makeCurrent 0x%04x", (void *)g_core, eglGetError());
        return 0;
    }
    R("ctx-core46", 1, "GL \"%s\" GLSL \"%s\", surfaceless, no share context (the compositor's)",
      (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));

    static const EGLint es_cfg[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    static const EGLint es_ctx[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 2, EGL_NONE};
    if (!eglChooseConfig(g_dpy, es_cfg, &cfg, 1, &count) || count != 1) {
        R("ctx-anchor-es32", 0, "no ES3 config (0x%04x)", eglGetError());
        return 0;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    g_anchor = eglCreateContext(g_dpy, cfg, g_core, es_ctx);
    const EGLint error = eglGetError();
    eglBindAPI(EGL_OPENGL_API); /* the compositor thread keeps OpenGL bound */
    if (g_anchor == EGL_NO_CONTEXT || !make_anchor_current()) {
        R("ctx-anchor-es32", 0, "ES 3.2 context sharing with the core context: create 0x%04x, makeCurrent 0x%04x",
          error, eglGetError());
        return 0;
    }
    EGLint type = 0, version = 0;
    eglQueryContext(g_dpy, g_anchor, EGL_CONTEXT_CLIENT_TYPE, &type);
    eglQueryContext(g_dpy, g_anchor, EGL_CONTEXT_CLIENT_VERSION, &version);
    load_exts();
    R("ctx-anchor-es32", type == EGL_OPENGL_ES_API, "GL \"%s\" in the core context's share group, client type %s version %d, current surfaceless with EGL_OPENGL_API bound",
      (const char *)glGetString(GL_VERSION), type == EGL_OPENGL_ES_API ? "OpenGL_ES" : "other", version);
    return type == EGL_OPENGL_ES_API;
}

/* ---- group: egl --------------------------------------------------------- */

static int word_in(const char *list, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = list; p && (p = strstr(p, word)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == 0 || p[n] == ' '))
            return 1;
    return 0;
}

static void g_egl(void)
{
    const char *version = eglQueryString(g_dpy, EGL_VERSION);
    const char *vendor = eglQueryString(g_dpy, EGL_VENDOR);
    const char *apis = eglQueryString(g_dpy, EGL_CLIENT_APIS);
    const char *exts = eglQueryString(g_dpy, EGL_EXTENSIONS);
    const char *client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    (void)eglGetError();
    if (!exts)
        exts = "";
    R("egl-display", version != NULL, "eglInitialize %d.%d, version \"%s\" vendor \"%s\" apis \"%s\" client extensions %s%s%s",
      g_egl_major, g_egl_minor, version, vendor, apis, client ? "\"" : "", client ? client : "NULL", client ? "\"" : "");
    probe_log("  EGL display extensions: %s", exts);

    /* BackendGL.cpp:94-101, DisplayEGL.cpp:113-128, ContextEGL.cpp. */
    static const struct { const char *name; int hard; } k[] = {
        {"EGL_EXT_create_context_robustness", 1}, /* BackendGL.cpp:94 */
        {"EGL_KHR_fence_sync", 2},                /* BackendGL.cpp:97: this or reusable_sync */
        {"EGL_KHR_reusable_sync", 2},
        {"EGL_KHR_create_context", 0},            /* ContextEGL.cpp:160; else ES 2 is asked for */
        {"EGL_KHR_no_config_context", 0},         /* ContextEGL.cpp:141; else an EGL_WINDOW_BIT config */
        {"EGL_KHR_surfaceless_context", 0},       /* ContextEGL.cpp:226; else a 1x1 pbuffer */
        {"EGL_KHR_wait_sync", 0},
        {"EGL_KHR_image_base", 0},
        {"EGL_KHR_gl_colorspace", 0},
        {"EGL_EXT_pixel_format_float", 0},
        {"EGL_ANDROID_native_fence_sync", 0},
        {"EGL_KHR_create_context_no_error", 0},
    };
    int robust = 0, sync = 0;
    const int v15 = g_egl_major > 1 || (g_egl_major == 1 && g_egl_minor >= 5);
    dreset();
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) {
        const int have = word_in(exts, k[i].name);
        dcat("%c%s ", have ? '+' : '-', k[i].name);
        if (k[i].hard == 1)
            robust = have || v15; /* promoted to 1.5 in Dawn's table (EGLFunctions.cpp:75) */
        if (k[i].hard == 2)
            sync |= have;
    }
    const int v14 = g_egl_major > 1 || (g_egl_major == 1 && g_egl_minor >= 4);
    const int es = apis && word_in(apis, "OpenGL_ES");
    R("egl-dawn-required", v14 && es && robust && sync,
      "EGL>=1.4 %s, OpenGL_ES api %s, create_context_robustness %s, fence_sync|reusable_sync %s (all four are hard requirements). %s",
      v14 ? "yes" : "NO", es ? "yes" : "NO", robust ? "yes" : "NO", sync ? "yes" : "NO", dstr());

    /* What Dawn's queue calls (UtilsEGL.cpp:93-116, QueueGL.cpp:72-78). */
    static const char *const sync_procs[] = {
        "eglCreateSyncKHR", "eglDestroySyncKHR", "eglClientWaitSyncKHR", "eglGetSyncAttribKHR",
        "eglSignalSyncKHR", "eglWaitSyncKHR", "eglCreateSync", "eglDestroySync", "eglClientWaitSync",
        "eglGetSyncAttrib", "eglWaitSync", "eglCreateImageKHR", "eglDestroyImageKHR", "eglDupNativeFenceFDANDROID",
    };
    int nsync = 0;
    dreset();
    for (size_t i = 0; i < sizeof sync_procs / sizeof *sync_procs; i++) {
        const int have = gp(sync_procs[i]) != NULL;
        nsync += have;
        dcat("%c%s ", have ? '+' : '-', sync_procs[i]);
    }
    R("egl-sync", sync && gp("eglCreateSyncKHR") && gp("eglClientWaitSyncKHR"),
      "EGL_KHR_fence_sync %s, EGL_KHR_reusable_sync %s, EGL_KHR_wait_sync %s; through eglGetProcAddress (%d of %zu): %s",
      word_in(exts, "EGL_KHR_fence_sync") ? "advertised" : "absent",
      word_in(exts, "EGL_KHR_reusable_sync") ? "advertised" : "absent",
      word_in(exts, "EGL_KHR_wait_sync") ? "advertised" : "absent", nsync,
      sizeof sync_procs / sizeof *sync_procs, dstr());

    /* EGLFunctions.cpp:117-206: Dawn loads every egl* entry point through the
     * getProc it is given (eglGetProcAddress when it loads libEGL itself). */
    static const char *const core_procs[] = {
        "eglChooseConfig", "eglCopyBuffers", "eglCreateContext", "eglCreatePbufferSurface", "eglCreatePixmapSurface",
        "eglCreateWindowSurface", "eglDestroyContext", "eglDestroySurface", "eglGetConfigAttrib", "eglGetConfigs",
        "eglGetCurrentDisplay", "eglGetCurrentSurface", "eglGetDisplay", "eglGetError", "eglInitialize",
        "eglMakeCurrent", "eglQueryContext", "eglQueryString", "eglQuerySurface", "eglSwapBuffers", "eglTerminate",
        "eglWaitGL", "eglWaitNative", "eglBindTexImage", "eglReleaseTexImage", "eglSurfaceAttrib", "eglSwapInterval",
        "eglBindAPI", "eglQueryAPI", "eglCreatePbufferFromClientBuffer", "eglReleaseThread", "eglWaitClient",
        "eglGetCurrentContext",
    };
    int ncore = 0;
    for (size_t i = 0; i < sizeof core_procs / sizeof *core_procs; i++)
        ncore += gp(core_procs[i]) != NULL;
    const size_t want = sizeof core_procs / sizeof *core_procs;
    R("egl-getproc-egl", (size_t)ncore == want,
      "%d of %zu EGL 1.0-1.4 entry points resolve through the facade's eglGetProcAddress (Dawn loads them all through its getProc; with fewer, the embedder passes its own getProc: RequestAdapterOptionsGetGLProc)",
      ncore, want);
}

/* ---- group: ctx-dawn ---------------------------------------------------- */

#define P_EGL_ROBUST_ACCESS_EXT 0x30BF
#define P_EGL_RESET_STRATEGY_EXT 0x3138
#define P_EGL_LOSE_CONTEXT_ON_RESET 0x31BF
#define P_EGL_ROBUST_ACCESS_15 0x31B2
#define P_EGL_RESET_STRATEGY_15 0x31BD
#define P_GL_CONTEXT_FLAGS 0x821E
#define P_GL_RESET_NOTIFICATION_STRATEGY 0x8256
#define P_GL_LOSE_CONTEXT_ON_RESET 0x8252
#define P_GL_NO_RESET_NOTIFICATION 0x8261
#define P_GL_CONTEXT_ROBUST_ACCESS 0x90F3

static const char *strategy_name(GLint s)
{
    return s == P_GL_LOSE_CONTEXT_ON_RESET ? "LOSE_CONTEXT_ON_RESET" :
           s == P_GL_NO_RESET_NOTIFICATION ? "NO_RESET_NOTIFICATION" : "?";
}

static const char *reset_name(GLenum s)
{
    return s == GL_NO_ERROR ? "NO_ERROR" : s == GL_GUILTY_CONTEXT_RESET ? "GUILTY" :
           s == GL_INNOCENT_CONTEXT_RESET ? "INNOCENT" : s == GL_UNKNOWN_CONTEXT_RESET ? "UNKNOWN" : "?";
}

/* Describes the current context's robustness state into the detail string. */
static void describe_robustness(void)
{
    GLint flags = 0, strategy = 0;
    GLboolean robust = GL_FALSE;
    glerr();
    glGetIntegerv(P_GL_CONTEXT_FLAGS, &flags);
    const GLenum e1 = glerr();
    glGetIntegerv(P_GL_RESET_NOTIFICATION_STRATEGY, &strategy);
    const GLenum e2 = glerr();
    glGetBooleanv(P_GL_CONTEXT_ROBUST_ACCESS, &robust);
    const GLenum e3 = glerr();
    const GLenum status = glGetGraphicsResetStatus();
    const GLenum e4 = glerr();
    dcat("GL_CONTEXT_FLAGS 0x%x (%s), robust access bit %s, GL_CONTEXT_ROBUST_ACCESS %d (%s), reset strategy %s (%s), glGetGraphicsResetStatus %s (%s)",
         flags, errname(e1), (flags & 0x4) ? "set" : "clear", robust, errname(e3), strategy_name(strategy),
         errname(e2), reset_name(status), errname(e4));
}

static void g_ctx_dawn(void)
{
    /* ContextEGL.cpp:160-222: ES 3.1 asked for, EGL_NO_CONFIG when
     * EGL_KHR_no_config_context, EGL_NO_CONTEXT as the share context, and
     * surfaceless when EGL_KHR_surfaceless_context. First the adapter's
     * temporary context (no robustness: PhysicalDeviceGL.cpp:118). */
    static const EGLint plain[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 1, EGL_NONE};
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLContext c = eglCreateContext(g_dpy, (EGLConfig)0, EGL_NO_CONTEXT, plain);
    EGLint err = eglGetError();
    if (c == EGL_NO_CONTEXT || !eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
        R("ctx-dawn-es31", 0, "ES 3.1, no config, no share context: create 0x%04x makeCurrent 0x%04x", err, eglGetError());
    } else {
        GLint n = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        const char *v = (const char *)glGetString(GL_VERSION);
        R("ctx-dawn-es31", v && !strncmp(v, "OpenGL ES 3.", 12) && v[12] >= '1',
          "ES 3.1 asked for with EGL_NO_CONFIG and no share context, current surfaceless: GL \"%s\" GLSL \"%s\", %d extensions",
          v, (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION), n);
    }
    make_anchor_current();
    if (c != EGL_NO_CONTEXT)
        eglDestroyContext(g_dpy, c);

    /* The device's context (ContextEGL.cpp:179-191): below EGL 1.5 the EXT
     * attributes, with EGL_LOSE_CONTEXT_ON_RESET_EXT. */
    static const EGLint robust[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 1,
                                    P_EGL_ROBUST_ACCESS_EXT, EGL_TRUE,
                                    P_EGL_RESET_STRATEGY_EXT, P_EGL_LOSE_CONTEXT_ON_RESET, EGL_NONE};
    c = eglCreateContext(g_dpy, (EGLConfig)0, EGL_NO_CONTEXT, robust);
    err = eglGetError();
    if (c == EGL_NO_CONTEXT || !eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
        R("ctx-dawn-robust", 0, "ES 3.1 + ROBUST_ACCESS_EXT + LOSE_CONTEXT_ON_RESET_EXT: create 0x%04x makeCurrent 0x%04x",
          err, eglGetError());
    } else {
        GLint strategy = 0;
        glGetIntegerv(P_GL_RESET_NOTIFICATION_STRATEGY, &strategy);
        dreset();
        describe_robustness();
        R("ctx-dawn-robust", glGetGraphicsResetStatus() == GL_NO_ERROR,
          "ES 3.1 + EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT + LOSE_CONTEXT_ON_RESET_EXT accepted: %s%s", dstr(),
          strategy == P_GL_LOSE_CONTEXT_ON_RESET ? "" : " (the strategy asked for is not what the context reports)");
    }
    make_anchor_current();
    if (c != EGL_NO_CONTEXT)
        eglDestroyContext(g_dpy, c);

    /* The EGL 1.5 spelling (used when eglInitialize says 1.5). */
    static const EGLint robust15[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 1,
                                      P_EGL_ROBUST_ACCESS_15, EGL_TRUE,
                                      P_EGL_RESET_STRATEGY_15, P_EGL_LOSE_CONTEXT_ON_RESET, EGL_NONE};
    c = eglCreateContext(g_dpy, (EGLConfig)0, EGL_NO_CONTEXT, robust15);
    err = eglGetError();
    probe_log("  EGL 1.5 robustness attributes (0x31B2/0x31BD): %s (0x%04x)%s", c != EGL_NO_CONTEXT ? "accepted" : "refused",
              err, g_egl_minor >= 5 ? "" : "; Dawn uses the EXT ones below EGL 1.5");
    if (c != EGL_NO_CONTEXT)
        eglDestroyContext(g_dpy, c);
    eglBindAPI(EGL_OPENGL_API);

    /* The probe's own context (no robustness attributes, as the anchor). */
    dreset();
    describe_robustness();
    R("reset-status", glGetGraphicsResetStatus() == GL_NO_ERROR, "anchor context (created without robustness attributes): %s", dstr());
}

/* ---- group: caps -------------------------------------------------------- */

static GLint gi(GLenum pname)
{
    GLint v = -1;
    glGetIntegerv(pname, &v);
    if (glerr() != GL_NO_ERROR)
        return -1;
    return v;
}

static long long gi64(GLenum pname)
{
    GLint64 v = -1;
    glGetInteger64v(pname, &v);
    if (glerr() != GL_NO_ERROR)
        return -1;
    return (long long)v;
}

static GLint gii(GLenum pname, GLuint index)
{
    GLint v = -1;
    glGetIntegeri_v(pname, index, &v);
    if (glerr() != GL_NO_ERROR)
        return -1;
    return v;
}

static void g_caps(void)
{
    R("caps-strings", 1, "GL_VERSION \"%s\" GL_RENDERER \"%s\" GL_VENDOR \"%s\" GL_SHADING_LANGUAGE_VERSION \"%s\", %d extensions",
      (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER),
      (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION), g_next);
    char line[900];
    size_t n = 0;
    for (int i = 0; i < g_next; i++) {
        if (n + strlen(g_exts[i]) + 2 > sizeof line) {
            probe_log("  GL extensions: %s", line);
            n = 0;
        }
        n += (size_t)snprintf(line + n, sizeof line - n, "%s ", g_exts[i]);
    }
    if (n)
        probe_log("  GL extensions: %s", line);

    GLfloat aniso = 0;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &aniso);
    glerr();
    R("caps-limits-texture", gi(GL_MAX_TEXTURE_SIZE) > 0,
      "MAX_TEXTURE_SIZE %d MAX_3D_TEXTURE_SIZE %d MAX_ARRAY_TEXTURE_LAYERS %d MAX_CUBE_MAP_TEXTURE_SIZE %d MAX_RENDERBUFFER_SIZE %d MAX_TEXTURE_BUFFER_SIZE %d MAX_COLOR_ATTACHMENTS %d MAX_DRAW_BUFFERS %d MAX_SAMPLES %d MAX_COLOR_TEXTURE_SAMPLES %d MAX_DEPTH_TEXTURE_SAMPLES %d MAX_INTEGER_SAMPLES %d MAX_FRAMEBUFFER_WIDTH %d MAX_FRAMEBUFFER_LAYERS %d MAX_TEXTURE_MAX_ANISOTROPY %.0f MAX_TEXTURE_LOD_BIAS %d",
      gi(GL_MAX_TEXTURE_SIZE), gi(GL_MAX_3D_TEXTURE_SIZE), gi(GL_MAX_ARRAY_TEXTURE_LAYERS),
      gi(GL_MAX_CUBE_MAP_TEXTURE_SIZE), gi(GL_MAX_RENDERBUFFER_SIZE), gi(GL_MAX_TEXTURE_BUFFER_SIZE),
      gi(GL_MAX_COLOR_ATTACHMENTS), gi(GL_MAX_DRAW_BUFFERS), gi(GL_MAX_SAMPLES), gi(GL_MAX_COLOR_TEXTURE_SAMPLES),
      gi(GL_MAX_DEPTH_TEXTURE_SAMPLES), gi(GL_MAX_INTEGER_SAMPLES), gi(GL_MAX_FRAMEBUFFER_WIDTH),
      gi(GL_MAX_FRAMEBUFFER_LAYERS), aniso, gi(GL_MAX_TEXTURE_LOD_BIAS));

    R("caps-limits-buffers", gi(GL_MAX_UNIFORM_BUFFER_BINDINGS) > 0,
      "MAX_UNIFORM_BUFFER_BINDINGS %d MAX_UNIFORM_BLOCK_SIZE %lld UNIFORM_BUFFER_OFFSET_ALIGNMENT %d MAX_VERTEX_UNIFORM_BLOCKS %d MAX_FRAGMENT_UNIFORM_BLOCKS %d MAX_COMPUTE_UNIFORM_BLOCKS %d MAX_COMBINED_UNIFORM_BLOCKS %d MAX_SHADER_STORAGE_BUFFER_BINDINGS %d MAX_SHADER_STORAGE_BLOCK_SIZE %lld (as GLint %d) SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT %d MAX_COMBINED_SHADER_STORAGE_BLOCKS %d MAX_ELEMENT_INDEX %lld MAX_ELEMENTS_VERTICES %d",
      gi(GL_MAX_UNIFORM_BUFFER_BINDINGS), gi64(GL_MAX_UNIFORM_BLOCK_SIZE), gi(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT),
      gi(GL_MAX_VERTEX_UNIFORM_BLOCKS), gi(GL_MAX_FRAGMENT_UNIFORM_BLOCKS), gi(GL_MAX_COMPUTE_UNIFORM_BLOCKS),
      gi(GL_MAX_COMBINED_UNIFORM_BLOCKS), gi(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS),
      gi64(GL_MAX_SHADER_STORAGE_BLOCK_SIZE), gi(GL_MAX_SHADER_STORAGE_BLOCK_SIZE),
      gi(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT), gi(GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS),
      gi64(GL_MAX_ELEMENT_INDEX), gi(GL_MAX_ELEMENTS_VERTICES));

    R("caps-limits-compute", gii(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0) > 0,
      "MAX_COMPUTE_WORK_GROUP_COUNT %d %d %d MAX_COMPUTE_WORK_GROUP_SIZE %d %d %d MAX_COMPUTE_WORK_GROUP_INVOCATIONS %d MAX_COMPUTE_SHARED_MEMORY_SIZE %d MAX_COMPUTE_SHADER_STORAGE_BLOCKS %d MAX_COMPUTE_IMAGE_UNIFORMS %d MAX_COMPUTE_TEXTURE_IMAGE_UNITS %d MAX_COMPUTE_UNIFORM_COMPONENTS %d MAX_COMPUTE_ATOMIC_COUNTERS %d",
      gii(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0), gii(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 1),
      gii(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 2), gii(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 0),
      gii(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 1), gii(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 2),
      gi(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS), gi(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE),
      gi(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS), gi(GL_MAX_COMPUTE_IMAGE_UNIFORMS),
      gi(GL_MAX_COMPUTE_TEXTURE_IMAGE_UNITS), gi(GL_MAX_COMPUTE_UNIFORM_COMPONENTS),
      gi(GL_MAX_COMPUTE_ATOMIC_COUNTERS));

    R("caps-limits-stages", gi(GL_MAX_VERTEX_ATTRIBS) > 0,
      "MAX_VERTEX_ATTRIBS %d MAX_VERTEX_ATTRIB_BINDINGS %d MAX_VERTEX_ATTRIB_STRIDE %d MAX_VERTEX_ATTRIB_RELATIVE_OFFSET %d MAX_VARYING_VECTORS %d MAX_VARYING_COMPONENTS %d MAX_VERTEX_OUTPUT_COMPONENTS %d MAX_FRAGMENT_INPUT_COMPONENTS %d MAX_TEXTURE_IMAGE_UNITS %d MAX_VERTEX_TEXTURE_IMAGE_UNITS %d MAX_COMBINED_TEXTURE_IMAGE_UNITS %d MAX_IMAGE_UNITS %d MAX_VERTEX_IMAGE_UNIFORMS %d MAX_FRAGMENT_IMAGE_UNIFORMS %d MAX_COMBINED_IMAGE_UNIFORMS %d MAX_VERTEX_SHADER_STORAGE_BLOCKS %d MAX_FRAGMENT_SHADER_STORAGE_BLOCKS %d MAX_VERTEX_UNIFORM_VECTORS %d MAX_FRAGMENT_UNIFORM_VECTORS %d",
      gi(GL_MAX_VERTEX_ATTRIBS), gi(GL_MAX_VERTEX_ATTRIB_BINDINGS), gi(GL_MAX_VERTEX_ATTRIB_STRIDE),
      gi(GL_MAX_VERTEX_ATTRIB_RELATIVE_OFFSET), gi(GL_MAX_VARYING_VECTORS), gi(GL_MAX_VARYING_COMPONENTS),
      gi(GL_MAX_VERTEX_OUTPUT_COMPONENTS), gi(GL_MAX_FRAGMENT_INPUT_COMPONENTS), gi(GL_MAX_TEXTURE_IMAGE_UNITS),
      gi(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS), gi(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS), gi(GL_MAX_IMAGE_UNITS),
      gi(GL_MAX_VERTEX_IMAGE_UNIFORMS), gi(GL_MAX_FRAGMENT_IMAGE_UNIFORMS), gi(GL_MAX_COMBINED_IMAGE_UNIFORMS),
      gi(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS), gi(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS),
      gi(GL_MAX_VERTEX_UNIFORM_VECTORS), gi(GL_MAX_FRAGMENT_UNIFORM_VECTORS));

    /* The limits Dawn derives (PhysicalDeviceGL.cpp:341-430) against the
     * compatibility-mode defaults (src/dawn/native/Limits.cpp, first tier). */
    const int tex_units = gi(GL_MAX_TEXTURE_IMAGE_UNITS) < gi(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS) ?
                          gi(GL_MAX_TEXTURE_IMAGE_UNITS) : gi(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS);
    const int attachments = gi(GL_MAX_COLOR_ATTACHMENTS) < gi(GL_MAX_DRAW_BUFFERS) ?
                            gi(GL_MAX_COLOR_ATTACHMENTS) : gi(GL_MAX_DRAW_BUFFERS);
    int groups = gii(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0);
    for (GLuint i = 1; i < 3; i++)
        if (gii(GL_MAX_COMPUTE_WORK_GROUP_COUNT, i) < groups)
            groups = gii(GL_MAX_COMPUTE_WORK_GROUP_COUNT, i);
    const struct { const char *name; long long have, want; int at_most; } lim[] = {
        {"maxTextureDimension2D", gi(GL_MAX_TEXTURE_SIZE), 4096, 0},
        {"maxTextureDimension3D", gi(GL_MAX_3D_TEXTURE_SIZE), 2048, 0},
        {"maxTextureArrayLayers", gi(GL_MAX_ARRAY_TEXTURE_LAYERS), 256, 0},
        {"maxUniformBuffersPerShaderStage", gi(GL_MAX_UNIFORM_BUFFER_BINDINGS), 12, 0},
        {"maxStorageBuffersPerShaderStage", gi(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS), 8, 0},
        {"maxSampledTexturesPerShaderStage", tex_units, 16, 0},
        {"maxSamplersPerShaderStage", gi(GL_MAX_TEXTURE_IMAGE_UNITS), 16, 0},
        {"maxStorageTexturesPerShaderStage", gi(GL_MAX_COMPUTE_IMAGE_UNIFORMS), 4, 0},
        {"maxStorageTexturesInFragmentStage", gi(GL_MAX_FRAGMENT_IMAGE_UNIFORMS), 4, 0},
        {"maxStorageBuffersInFragmentStage", gi(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS), 4, 0},
        {"maxUniformBufferBindingSize", gi(GL_MAX_UNIFORM_BLOCK_SIZE), 16384, 0},
        {"maxStorageBufferBindingSize", gi(GL_MAX_SHADER_STORAGE_BLOCK_SIZE), 134217728, 0},
        {"minUniformBufferOffsetAlignment", gi(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT), 256, 1},
        {"minStorageBufferOffsetAlignment", gi(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT), 256, 1},
        {"maxVertexBuffers", gi(GL_MAX_VERTEX_ATTRIB_BINDINGS), 8, 0},
        {"maxVertexAttributes", gi(GL_MAX_VERTEX_ATTRIBS), 16, 0},
        {"maxVertexBufferArrayStride", gi(GL_MAX_VERTEX_ATTRIB_STRIDE), 2048, 0},
        {"maxInterStageShaderVariables", gi(GL_MAX_VARYING_VECTORS), 15, 0},
        {"maxColorAttachments", attachments, 4, 0},
        {"maxComputeWorkgroupStorageSize", gi(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE), 16384, 0},
        {"maxComputeInvocationsPerWorkgroup", gi(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS), 128, 0},
        {"maxComputeWorkgroupSizeX", gii(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 0), 128, 0},
        {"maxComputeWorkgroupSizeY", gii(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 1), 128, 0},
        {"maxComputeWorkgroupSizeZ", gii(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 2), 64, 0},
        {"maxComputeWorkgroupsPerDimension", groups, 65535, 0},
    };
    int below = 0;
    dreset();
    for (size_t i = 0; i < sizeof lim / sizeof *lim; i++) {
        const int ok = lim[i].at_most ? (lim[i].have > 0 && lim[i].have <= lim[i].want) : lim[i].have >= lim[i].want;
        if (!ok) {
            below++;
            dcat("%s %lld (compat default %s%lld) ", lim[i].name, lim[i].have, lim[i].at_most ? "<=" : ">=", lim[i].want);
        }
    }
    R("caps-webgpu-compat-limits", below == 0,
      "%zu limits as Dawn derives them against WebGPU compatibility defaults: %d below%s%s", sizeof lim / sizeof *lim,
      below, below ? ": " : "", dstr());
}

/* ---- group: procs ------------------------------------------------------- */

static void g_procs(void)
{
    /* Is a NULL from eglGetProcAddress informative at all? */
    const int bogus = gp("glDefinitelyNotAnEntryPointPS5") != NULL;
    static const struct { int version; const char *name; } k[] = {
#define P(v, n) {v, n},
#include "es32_procs.inc"
#undef P
    };
    int have = 0, missing = 0;
    dreset();
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) {
        if (gp(k[i].name))
            have++;
        else if (missing++ < 12)
            dcat("%s(%d.%d) ", k[i].name, k[i].version / 10, k[i].version % 10);
    }
    R("gl-procs-es32", missing == 0 && !bogus,
      "%d of %zu OpenGL ES 2.0-3.2 core commands resolve through eglGetProcAddress (Dawn's loader asks for every one); an invented name resolves: %s%s%s",
      have, sizeof k / sizeof *k, bogus ? "YES, so this check proves nothing" : "no", missing ? "; missing: " : "", dstr());

    static const char *const ext[] = {
        "glGetGraphicsResetStatusKHR", "glGetGraphicsResetStatusEXT", "glDrawArraysInstancedBaseInstanceEXT",
        "glDrawElementsInstancedBaseVertexBaseInstanceEXT", "glDrawArraysInstancedBaseInstanceANGLE",
        "glMultiDrawArraysIndirectEXT", "glTexBufferEXT", "glTexBufferOES", "glTexStorage2DEXT",
        "glGenQueriesEXT", "glBeginQueryEXT", "glEndQueryEXT", "glQueryCounterEXT", "glGetQueryObjectui64vEXT",
        "glFramebufferTexture2DMultisampleEXT", "glEGLImageTargetTexture2DOES", "glCopyImageSubDataEXT",
        "glCopyImageSubDataOES", "glDrawElementsBaseVertexEXT", "glDrawElementsBaseVertexOES",
        "glBindFragDataLocationIndexedEXT", "glClipControlEXT", "glPolygonOffsetClampEXT", "glBufferStorageEXT",
        "glTextureViewOES", "glTextureViewEXT", "glDebugMessageCallbackKHR", "glPolygonModeNV",
    };
    dreset();
    int n = 0;
    for (size_t i = 0; i < sizeof ext / sizeof *ext; i++) {
        const int h = gp(ext[i]) != NULL;
        n += h;
        if (!h)
            dcat("%s ", ext[i]);
    }
    R("gl-procs-ext", 1, "%d of %zu extension entry points resolve (informational); absent: %s", n, sizeof ext / sizeof *ext, dstr());
}

/* ---- group: extensions -------------------------------------------------- */

static void g_ext(void)
{
    /* required: 1 hard requirement of the backend on this context; 0 feature
     * or toggle only (file/line in NOTES.md). */
    static const struct { const char *name; int required; } k[] = {
        {"GL_EXT_color_buffer_float", 0}, /* required below ES 3.2 only (PhysicalDeviceGL.cpp:163) */
        {"GL_EXT_color_buffer_half_float", 0},
        {"GL_KHR_robustness", 0},         /* glGetGraphicsResetStatus is core in 3.2 */
        {"GL_OES_texture_stencil8", 0},
        {"GL_EXT_texture_format_BGRA8888", 0},
        {"GL_APPLE_texture_format_BGRA8888", 0},
        {"GL_EXT_texture_storage", 0},
        {"GL_EXT_read_format_bgra", 0},
        {"GL_EXT_texture_compression_s3tc", 0},
        {"GL_EXT_texture_compression_s3tc_srgb", 0},
        {"GL_EXT_texture_compression_dxt1", 0},
        {"GL_NV_sRGB_formats", 0},
        {"GL_EXT_texture_compression_rgtc", 0},
        {"GL_EXT_texture_compression_bptc", 0},
        {"GL_KHR_texture_compression_astc_ldr", 0},
        {"GL_KHR_texture_compression_astc_sliced_3d", 0},
        {"GL_EXT_texture_norm16", 0},
        {"GL_EXT_render_snorm", 0},
        {"GL_EXT_float_blend", 0},
        {"GL_EXT_blend_func_extended", 0},
        {"GL_EXT_multisampled_render_to_texture", 0},
        {"GL_OES_sample_variables", 0},
        {"GL_OES_sample_shading", 0},
        {"GL_EXT_texture_filter_anisotropic", 0},
        {"GL_EXT_conservative_depth", 0},
        {"GL_AMD_gpu_shader_half_float", 0},
        {"GL_AMD_gpu_shader_int16", 0},
        {"GL_OES_EGL_sync", 0},
        {"GL_OES_EGL_image", 0},
        {"GL_EXT_base_instance", 0},
        {"GL_ANGLE_base_vertex_base_instance", 0},
        {"GL_EXT_clip_cull_distance", 0},
        {"GL_EXT_clip_control", 0},
        {"GL_EXT_depth_clamp", 0},
        {"GL_EXT_polygon_offset_clamp", 0},
        {"GL_EXT_disjoint_timer_query", 0},
        {"GL_EXT_texture_buffer", 0},
        {"GL_OES_texture_buffer", 0},
        {"GL_OES_copy_image", 0},
        {"GL_EXT_copy_image", 0},
        {"GL_OES_draw_buffers_indexed", 0},
        {"GL_OES_draw_elements_base_vertex", 0},
        {"GL_EXT_sRGB_write_control", 0},
        {"GL_EXT_texture_sRGB_decode", 0},
        {"GL_OES_texture_float_linear", 0},
        {"GL_EXT_texture_view", 0},
        {"GL_OES_texture_view", 0},
        {"GL_EXT_buffer_storage", 0},
        {"GL_KHR_debug", 0},
        {"GL_EXT_multi_draw_indirect", 0},
        {"GL_EXT_shader_framebuffer_fetch", 0},
        {"GL_NV_polygon_mode", 0},
        {"GL_EXT_texture_mirror_clamp_to_edge", 0},
    };
    char present[900];
    size_t np = 0;
    present[0] = 0;
    dreset();
    int have = 0;
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) {
        const int h = has_ext(k[i].name);
        have += h;
        if (h && np < sizeof present - 64)
            np += (size_t)snprintf(present + np, sizeof present - np, "%s ", k[i].name + 3);
        else if (!h)
            dcat("%s ", k[i].name + 3);
    }
    probe_log("  of the extensions Dawn's GL backend looks at, present (GL_ dropped): %s", present);
    const char *v = (const char *)glGetString(GL_VERSION);
    const int es32 = v && !strncmp(v, "OpenGL ES 3.2", 13);
    /* PhysicalDeviceGL.cpp:162-176: ES >= 3.1; below 3.2 EXT_color_buffer_float;
     * glGetGraphicsResetStatus (3.2 core or a robustness extension). */
    R("gl-dawn-hard-requirements", es32 || (has_ext("GL_EXT_color_buffer_float") && has_ext("GL_KHR_robustness")),
      "ES 3.2 %s (so EXT_color_buffer_float %s and KHR_robustness %s are not needed for adapter creation); glGetGraphicsResetStatus %s",
      es32 ? "yes" : "NO", has_ext("GL_EXT_color_buffer_float") ? "present" : "absent",
      has_ext("GL_KHR_robustness") ? "present" : "absent", gp("glGetGraphicsResetStatus") ? "resolves" : "MISSING");
    R("gl-extensions-dawn", 1, "%d of %zu extensions Dawn's GL backend looks at are present; absent (GL_ dropped; none is a hard requirement): %s", have, sizeof k / sizeof *k, dstr());

    /* Features Dawn would switch on (PhysicalDeviceGL.cpp:205-325). */
    const int s3tc = has_ext("GL_EXT_texture_compression_s3tc") ||
                     (has_ext("GL_EXT_texture_compression_dxt1") && has_ext("GL_ANGLE_texture_compression_dxt3") &&
                      has_ext("GL_ANGLE_texture_compression_dxt5"));
    const int s3tc_srgb = has_ext("GL_EXT_texture_compression_s3tc_srgb") || has_ext("GL_NV_sRGB_formats");
    const int rgtc = has_ext("GL_ARB_texture_compression_rgtc") || has_ext("GL_EXT_texture_compression_rgtc");
    const int bptc = has_ext("GL_ARB_texture_compression_bptc") || has_ext("GL_EXT_texture_compression_bptc");
    R("gl-dawn-features", 1,
      "texture-compression-bc %s (s3tc %d, s3tc_srgb %d, rgtc %d, bptc %d), texture-compression-astc %s, texture-compression-etc2 yes (ES 3.0 core), dual-source-blending %s, unorm16 formats %s, float32-blendable %s, shader-f16 %s, msaa-render-to-single-sampled %s, shared-fence-egl-sync %s, indirect-first-instance no (desktop GL only), anisotropic filtering %s, BGRA8 internal format %s, sample variables yes (ES 3.2)",
      s3tc && s3tc_srgb && rgtc && bptc ? "yes" : "NO", s3tc, s3tc_srgb, rgtc, bptc,
      has_ext("GL_KHR_texture_compression_astc_ldr") ? "yes" : "no",
      has_ext("GL_EXT_blend_func_extended") ? "yes" : "no", has_ext("GL_EXT_texture_norm16") ? "yes" : "no",
      has_ext("GL_EXT_float_blend") ? "yes" : "no",
      has_ext("GL_AMD_gpu_shader_half_float") && has_ext("GL_AMD_gpu_shader_int16") ? "yes" : "no",
      has_ext("GL_EXT_multisampled_render_to_texture") ? "yes" : "no", has_ext("GL_OES_EGL_sync") ? "needs EGL_KHR_wait_sync too" : "no",
      has_ext("GL_EXT_texture_filter_anisotropic") ? "yes" : "no",
      has_ext("GL_EXT_texture_format_BGRA8888") || has_ext("GL_APPLE_texture_format_BGRA8888") ? "GL_BGRA8_EXT" : "GL_RGBA8 (wrong on ES: no swizzle)");
}

/* ---- group: shader front end ------------------------------------------- */

/* Shaped like Tint's GLSL output for ES: #version 310 es / 320 es, std430
 * blocks, image2D with explicit formats, a uniform block. */
static const char *const kFrontCs[] = {
    "#version 310 es\n"
    "layout(local_size_x = 64) in;\n"
    "layout(std140, binding = 0) uniform Params { uint mul; uint add; vec4 pad; uint count; } p;\n"
    "layout(std430, binding = 0) buffer Out { uint data[]; } o;\n"
    "void main() { uint i = gl_GlobalInvocationID.x; if (i < p.count && i < uint(o.data.length())) o.data[i] = i * p.mul + p.add; }\n",

    "#version 310 es\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(rgba8, binding = 0) writeonly uniform highp image2D dst8;\n"
    "layout(r32f, binding = 1) uniform highp image2D rw;\n"
    "layout(rgba16f, binding = 2) readonly uniform highp image2D src16;\n"
    "layout(rgba32ui, binding = 3) readonly uniform highp uimage2D srcu;\n"
    "void main() { ivec2 c = ivec2(gl_GlobalInvocationID.xy);\n"
    "  float f = imageLoad(rw, c).r; imageStore(rw, c, vec4(f * 2.0));\n"
    "  imageStore(dst8, c, imageLoad(src16, c) + vec4(imageLoad(srcu, c))); }\n",

    "#version 320 es\n"
    "precision highp float;\n"
    "layout(local_size_x = 16, local_size_y = 4, local_size_z = 2) in;\n"
    "uniform highp sampler2D tex; uniform highp sampler2DArray arr; uniform highp samplerCube cube;\n"
    "layout(std430, binding = 1) buffer Acc { uint counter; float sum[]; } acc;\n"
    "shared float tmp[128];\n"
    "void main() { tmp[gl_LocalInvocationIndex] = textureLod(tex, vec2(0.5), 0.0).r + texelFetch(arr, ivec3(0), 0).g + textureLod(cube, vec3(1.0), 0.0).b;\n"
    "  barrier(); memoryBarrierShared(); atomicAdd(acc.counter, 1u);\n"
    "  acc.sum[gl_GlobalInvocationID.x] = tmp[127u - gl_LocalInvocationIndex] + float(textureSize(tex, 0).x); }\n",
};

static const char *const kFrontVs =
    "#version 310 es\n"
    "layout(location = 0) in vec4 pos; layout(location = 1) in vec2 uv; layout(location = 2) in uint id;\n"
    "layout(std140, binding = 0) uniform Camera { mat4 mvp; vec4 tint; } cam;\n"
    "out vec2 v_uv; flat out uint v_id;\n"
    "void main() { v_uv = uv; v_id = id + uint(gl_InstanceID) + uint(gl_VertexID); gl_Position = cam.mvp * pos; gl_PointSize = 1.0; }\n";

static const char *const kFrontFs =
    "#version 310 es\n"
    "precision highp float; precision highp int;\n"
    "in vec2 v_uv; flat in uint v_id;\n"
    "uniform highp sampler2D tex; uniform highp sampler2DShadow shadow; uniform highp usampler2D stencil;\n"
    "layout(std430, binding = 0) readonly buffer Lights { vec4 colour[]; } lights;\n"
    "layout(location = 0) out vec4 o0; layout(location = 1) out vec4 o1;\n"
    "void main() { o0 = texture(tex, v_uv) * lights.colour[v_id % uint(lights.colour.length())];\n"
    "  o1 = vec4(texture(shadow, vec3(v_uv, 0.5)), float(texelFetch(stencil, ivec2(0), 0).r), dFdx(v_uv.x), 1.0);\n"
    "  if (o0.a < 0.5) discard; gl_FragDepth = 0.5; }\n";

static void g_shader_frontend(void)
{
    char log[400] = "";
    int ok = 0;
    const double t0 = probe_now_ms();
    dreset();
    for (size_t i = 0; i < sizeof kFrontCs / sizeof *kFrontCs; i++) {
        const double t = probe_now_ms();
        GLuint s = mk_shader(GL_COMPUTE_SHADER, kFrontCs[i], log, sizeof log);
        dcat("cs%zu %s %.1f ms%s%s; ", i, s ? "ok" : "FAILED", probe_now_ms() - t, s ? "" : ": ", s ? "" : log);
        ok += s != 0;
        if (s)
            glDeleteShader(s);
    }
    R("shader-compile-compute", ok == 3,
      "glCompileShader only (Mesa front end; no link, no PSBC): %s total %.1f ms (ssbo+ubo+arrayLength; image load/store rgba8/r32f/rgba16f/rgba32ui; samplers+shared+barrier+atomicAdd at 320 es)",
      dstr(), probe_now_ms() - t0);
    GLuint v = mk_shader(GL_VERTEX_SHADER, kFrontVs, log, sizeof log);
    GLuint f = mk_shader(GL_FRAGMENT_SHADER, kFrontFs, v ? log : NULL, v ? sizeof log : 0);
    R("shader-compile-render", v && f, "vertex %s, fragment %s (UBO, fragment SSBO, shadow + stencil samplers, MRT, gl_FragDepth, discard) %s",
      v ? "ok" : "FAILED", f ? "ok" : "FAILED", v && f ? "" : log);
    if (v)
        glDeleteShader(v);
    if (f)
        glDeleteShader(f);
}

/* ---- group: formats ----------------------------------------------------- */

#define P_GL_BGRA_EXT 0x80E1
#define P_GL_BGRA8_EXT 0x93A1
#define P_GL_R16_EXT 0x822A
#define P_GL_RGBA16_EXT 0x805B

/* 'C' complete, 'I' incomplete/unsupported, 'E' GL error on creation. */
static char fbo_state(GLenum internal, GLenum attachment, int samples)
{
    GLuint tex = 0, fbo = 0;
    const GLenum target = samples ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
    char state;
    if (getenv("PROBE_TRACE"))
        probe_log("  trace: format 0x%04x samples %d", internal, samples);
    glerr();
    glGenTextures(1, &tex);
    glBindTexture(target, tex);
    if (samples)
        glTexStorage2DMultisample(target, samples, internal, 16, 16, GL_TRUE);
    else
        glTexStorage2D(target, 1, internal, 16, 16);
    if (glerr() != GL_NO_ERROR) {
        state = 'E';
    } else {
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, target, tex, 0);
        state = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE ? 'C' : 'I';
        if (glerr() != GL_NO_ERROR)
            state = 'E';
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
    }
    glBindTexture(target, 0);
    glDeleteTextures(1, &tex);
    glerr();
    return state;
}

static void g_formats(void)
{
    /* need: 1 a WebGPU core/compat render-attachment format, 0 optional. */
    static const struct { const char *name; GLenum f; int need; } color[] = {
        {"R8", GL_R8, 1}, {"RG8", GL_RG8, 1}, {"RGBA8", GL_RGBA8, 1}, {"SRGB8_A8", GL_SRGB8_ALPHA8, 1},
        {"BGRA8_EXT", P_GL_BGRA8_EXT, 1}, {"RGB10_A2", GL_RGB10_A2, 1}, {"R16F", GL_R16F, 1}, {"RG16F", GL_RG16F, 1},
        {"RGBA16F", GL_RGBA16F, 1}, {"R32F", GL_R32F, 1}, {"RG32F", GL_RG32F, 1}, {"RGBA32F", GL_RGBA32F, 1},
        {"R11F_G11F_B10F", GL_R11F_G11F_B10F, 0}, {"R8UI", GL_R8UI, 1}, {"R8I", GL_R8I, 1}, {"RG8UI", GL_RG8UI, 1},
        {"RGBA8UI", GL_RGBA8UI, 1}, {"RGBA8I", GL_RGBA8I, 1}, {"R16UI", GL_R16UI, 1}, {"RG16UI", GL_RG16UI, 1},
        {"RGBA16UI", GL_RGBA16UI, 1}, {"RGBA16I", GL_RGBA16I, 1}, {"R32UI", GL_R32UI, 1}, {"RG32UI", GL_RG32UI, 1},
        {"RGBA32UI", GL_RGBA32UI, 1}, {"RGBA32I", GL_RGBA32I, 1}, {"RGB10_A2UI", GL_RGB10_A2UI, 0},
        {"R16_EXT", P_GL_R16_EXT, 0}, {"RGBA16_EXT", P_GL_RGBA16_EXT, 0}, {"RGBA8_SNORM", GL_RGBA8_SNORM, 0},
        {"RGB9_E5", GL_RGB9_E5, 0},
    };
    int bad = 0;
    dreset();
    for (size_t i = 0; i < sizeof color / sizeof *color; i++) {
        const char s = fbo_state(color[i].f, GL_COLOR_ATTACHMENT0, 0);
        dcat("%s:%c ", color[i].name, s);
        bad += color[i].need && s != 'C';
    }
    R("formats-color-rt", bad == 0, "glTexStorage2D + FBO completeness, C complete / I incomplete / E error; %d needed formats not renderable: %s", bad, dstr());

    static const struct { const char *name; GLenum f, attachment; } ds[] = {
        {"DEPTH_COMPONENT16", GL_DEPTH_COMPONENT16, GL_DEPTH_ATTACHMENT},
        {"DEPTH_COMPONENT24", GL_DEPTH_COMPONENT24, GL_DEPTH_ATTACHMENT},
        {"DEPTH24_STENCIL8", GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL_ATTACHMENT},
        {"DEPTH_COMPONENT32F", GL_DEPTH_COMPONENT32F, GL_DEPTH_ATTACHMENT},
        {"DEPTH32F_STENCIL8", GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL_ATTACHMENT},
        {"STENCIL_INDEX8", GL_STENCIL_INDEX8, GL_STENCIL_ATTACHMENT},
    };
    bad = 0;
    dreset();
    for (size_t i = 0; i < sizeof ds / sizeof *ds; i++) {
        const char s = fbo_state(ds[i].f, ds[i].attachment, 0);
        const char m = fbo_state(ds[i].f, ds[i].attachment, 4);
        dcat("%s:%c/4x:%c ", ds[i].name, s, m);
        bad += s != 'C';
    }
    R("formats-depth-stencil-rt", bad == 0, "depth/stencil textures as the only attachment, single-sample / 4x multisample texture: %s", dstr());

    static const struct { const char *name; GLenum f; } ms[] = {
        {"RGBA8", GL_RGBA8}, {"SRGB8_A8", GL_SRGB8_ALPHA8}, {"RGBA16F", GL_RGBA16F}, {"RGBA32F", GL_RGBA32F},
        {"R8", GL_R8}, {"RG8", GL_RG8}, {"RGB10_A2", GL_RGB10_A2}, {"RGBA8UI", GL_RGBA8UI},
        {"DEPTH24_STENCIL8", GL_DEPTH24_STENCIL8}, {"DEPTH_COMPONENT32F", GL_DEPTH_COMPONENT32F},
    };
    /* Through eglGetProcAddress: the host build of Mesa's bridge has no
     * linkable glGetInternalformativ. */
    typedef void (*internalformat_fn)(GLenum, GLenum, GLenum, GLsizei, GLint *);
    internalformat_fn get_internalformat = (internalformat_fn)gp("glGetInternalformativ");
    bad = 0;
    dreset();
    for (size_t i = 0; i < sizeof ms / sizeof *ms; i++) {
        GLint count = 0, samples[8] = {0};
        if (get_internalformat)
            get_internalformat(GL_TEXTURE_2D_MULTISAMPLE, ms[i].f, GL_NUM_SAMPLE_COUNTS, 1, &count);
        if (count > 0)
            get_internalformat(GL_TEXTURE_2D_MULTISAMPLE, ms[i].f, GL_SAMPLES, count < 8 ? count : 8, samples);
        const GLenum e = glerr();
        const char s = fbo_state(ms[i].f, ms[i].f == GL_DEPTH24_STENCIL8 ? GL_DEPTH_STENCIL_ATTACHMENT :
                                 ms[i].f == GL_DEPTH_COMPONENT32F ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0, 4);
        dcat("%s:max %d%s,4x:%c ", ms[i].name, count > 0 ? samples[0] : 0, e ? "(err)" : "", s);
        /* WebGPU's only multisample count is 4. */
        bad += (ms[i].f == GL_RGBA8 || ms[i].f == GL_SRGB8_ALPHA8 || ms[i].f == GL_RGBA16F ||
                ms[i].f == GL_DEPTH24_STENCIL8) && s != 'C';
    }
    R("formats-msaa", bad == 0, "GL_SAMPLES for GL_TEXTURE_2D_MULTISAMPLE and a 4x glTexStorage2DMultisample attachment: %s", dstr());

    GLint n = 0, list[256];
    glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &n);
    if (n > 256)
        n = 256;
    glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, list);
    int bc = 0, etc2 = 0, astc = 0, other = 0;
    for (GLint i = 0; i < n; i++) {
        const GLint f = list[i];
        if ((f >= 0x83F0 && f <= 0x83F3) || (f >= 0x8C4C && f <= 0x8C4F) || (f >= 0x8DBB && f <= 0x8DBE) ||
            (f >= 0x8E8C && f <= 0x8E8F))
            bc++;
        else if (f >= 0x9270 && f <= 0x9279)
            etc2++;
        else if ((f >= 0x93B0 && f <= 0x93BD) || (f >= 0x93D0 && f <= 0x93DD))
            astc++;
        else
            other++;
    }
    R("formats-compressed-list", n > 0, "GL_COMPRESSED_TEXTURE_FORMATS: %d (BC/S3TC/RGTC/BPTC %d of 16, ETC2/EAC %d of 10, ASTC LDR %d of 28, other %d)",
      n, bc, etc2, astc, other);
}

/* A 4x multisample BGRA8 texture: what a WebGPU page gets for a multisampled
 * bgra8unorm attachment. In its own group, and last: Mesa's debug build
 * asserts inside glTexStorage2DMultisample for this format on this driver
 * (st_texture_create: the format st picked is not a sampler-view format), so
 * what the console's release build does with it is undefined. */
static void g_bgra_msaa(void)
{
    const char s = fbo_state(P_GL_BGRA8_EXT, GL_COLOR_ATTACHMENT0, 4);
    R("formats-msaa-bgra8", s == 'C', "glTexStorage2DMultisample(4, GL_BGRA8_EXT) as a colour attachment: %s",
      s == 'C' ? "complete" : s == 'I' ? "incomplete framebuffer" : "GL error");
}

/* ---- run ---------------------------------------------------------------- */

static void (*g_fn)(void);
static const char *g_results;

static void group_body(void)
{
    g_nseen = 0;
    reset_state();
    g_fn();
    /* Whatever the group promised and did not report. */
    char names[512];
    snprintf(names, sizeof names, "%s", g_results);
    for (char *save = NULL, *t = strtok_r(names, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
        int found = 0;
        for (int i = 0; i < g_nseen; i++)
            found |= !strcmp(g_seen[i], t);
        if (!found)
            probe_emit(t, "FAIL", "not reported: the group stopped before this test");
    }
    const GLenum e = glerr();
    if (e != GL_NO_ERROR)
        probe_log("  (GL error left over at the end of the group: %s)", errname(e));
}

void glp_group(const char *group, int needs_gpu, const char *results, void (*fn)(void))
{
    if (!probe_selected(group)) {
        char names[512];
        snprintf(names, sizeof names, "%s", results);
        for (char *save = NULL, *t = strtok_r(names, " ", &save); t; t = strtok_r(NULL, " ", &save))
            probe_emit(t, "SKIP", "group left out by the launch argument");
        return;
    }
    g_fn = fn;
    g_results = results;
    probe_run_group(group, needs_gpu, results, group_body);
}

void glprobe_run(void)
{
    g_nseen = 0;
    if (!setup_contexts()) {
        probe_log("no ES 3.2 context in the core context's share group: nothing else can run");
        return;
    }
    glp_group("egl", 0, "egl-display egl-dawn-required egl-sync egl-getproc-egl", g_egl);
    glp_group("ctx-dawn", 0, "ctx-dawn-es31 ctx-dawn-robust reset-status", g_ctx_dawn);
    make_anchor_current();
    glp_group("caps", 0, "caps-strings caps-limits-texture caps-limits-buffers caps-limits-compute caps-limits-stages caps-webgpu-compat-limits", g_caps);
    glp_group("procs", 0, "gl-procs-es32 gl-procs-ext", g_procs);
    glp_group("ext", 0, "gl-dawn-hard-requirements gl-extensions-dawn gl-dawn-features", g_ext);
    glp_group("shader-frontend", 0, "shader-compile-compute shader-compile-render", g_shader_frontend);
    glp_group("formats", 0, "formats-color-rt formats-depth-stencil-rt formats-msaa formats-compressed-list", g_formats);
    gpu_groups_run();
    glp_group("bgra-msaa", 0, "formats-msaa-bgra8", g_bgra_msaa);

    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(g_dpy, g_anchor);
    eglDestroyContext(g_dpy, g_core);
}
