/* share_native.c - the native half of the share-group probe (share_probe_run
 * in angle_tests.cpp): a GL 4.6 core context made the way the browser's
 * compositor makes its own (PS5Renderer::initializeGL: EGL_OPENGL_API, 4.6
 * core, no share context), and the reads, copies and waits the compositor
 * would do on textures ANGLE drew. ps5-opengl's EGL and GL directly. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>

#include <stdint.h>
#include <stdio.h>

double spike11_now_ms(void);

static EGLDisplay dpy = EGL_NO_DISPLAY;
static EGLContext core = EGL_NO_CONTEXT;
static EGLContext anchor = EGL_NO_CONTEXT;

/* Creates the core context and leaves it current, with EGL_OPENGL_API still
 * bound on this thread, as the compositor's render thread has it. */
int share_native_create(char *info, unsigned n)
{
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
        snprintf(info, n, "eglInitialize 0x%04x", eglGetError());
        return 0;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        snprintf(info, n, "eglBindAPI(EGL_OPENGL_API) 0x%04x", eglGetError());
        return 0;
    }
    static const EGLint cfg_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                       EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                       EGL_NONE};
    static const EGLint ctx_attrs[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 6,
                                       EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
                                       EGL_NONE};
    EGLConfig cfg;
    EGLint count = 0;
    if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &count) || count != 1) {
        snprintf(info, n, "no OpenGL config (0x%04x)", eglGetError());
        return 0;
    }
    core = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (core == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, core)) {
        snprintf(info, n, "4.6 core context %p, makeCurrent 0x%04x", core, eglGetError());
        return 0;
    }
    snprintf(info, n, "GL \"%s\" GLSL \"%s\", surfaceless", glGetString(GL_VERSION),
             glGetString(GL_SHADING_LANGUAGE_VERSION));
    return 1;
}

/* An OpenGL ES 3.2 context in the core context's share group, for ANGLE to
 * wrap with EGL_EXTERNAL_CONTEXT_ANGLE. ANGLE is built without its desktop GL
 * backend (no ANGLE_ENABLE_GL_DESKTOP_BACKEND), so wrapping the 4.6 core
 * context itself leaves it with no GL functions. Leaves EGL_OPENGL_API bound
 * and the core context current. */
int share_native_create_anchor(char *info, unsigned n)
{
    static const EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 2, EGL_NONE};
    EGLConfig cfg;
    EGLint count = 0;
    static const EGLint cfg_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                       EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                       EGL_NONE};
    if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &count) || count != 1) {
        snprintf(info, n, "no ES3 config (0x%04x)", eglGetError());
        return 0;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    anchor = eglCreateContext(dpy, cfg, core, attrs);
    const EGLint error = eglGetError();
    eglBindAPI(EGL_OPENGL_API);
    if (anchor == EGL_NO_CONTEXT) {
        snprintf(info, n, "ES 3.2 context sharing with the core context: 0x%04x", error);
        return 0;
    }
    snprintf(info, n, "ES 3.2 context created with the 4.6 core context as its share context");
    return 1;
}

int share_native_make_anchor_current(void)
{
    return eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, anchor) == EGL_TRUE;
}

int share_native_make_current(void)
{
    return eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, core) == EGL_TRUE;
}

int share_native_core_is_current(void)
{
    return core != EGL_NO_CONTEXT && eglGetCurrentContext() == core;
}

void share_native_release(void)
{
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void share_native_bind_es(void)
{
    eglBindAPI(EGL_OPENGL_ES_API);
}

void share_native_bind_gl(void)
{
    eglBindAPI(EGL_OPENGL_API);
}

/* Describes the native context current on this thread (ANGLE's, when an ANGLE
 * context is current). Returns 1 for an OpenGL ES context. */
int share_native_current(char *info, unsigned n)
{
    EGLContext c = eglGetCurrentContext();
    EGLint type = 0, version = 0;
    if (c == EGL_NO_CONTEXT) {
        snprintf(info, n, "no native context current");
        return 0;
    }
    eglQueryContext(dpy, c, EGL_CONTEXT_CLIENT_TYPE, &type);
    eglQueryContext(dpy, c, EGL_CONTEXT_CLIENT_VERSION, &version);
    snprintf(info, n, "native %s context (client version %d) \"%s\"%s",
             type == EGL_OPENGL_ES_API ? "OpenGL ES" : type == EGL_OPENGL_API ? "OpenGL" : "unknown", version,
             glGetString(GL_VERSION), c == core ? ", the core context itself" : c == anchor ? ", the anchor" : "");
    return type == EGL_OPENGL_ES_API;
}

/* A w x h RGBA8 texture of one colour, made in the core context. */
unsigned share_native_texture(int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    static uint8_t px[64 * 64 * 4];
    if (w * h > 64 * 64)
        return 0;
    for (int i = 0; i < w * h; i++) {
        px[i * 4] = r;
        px[i * 4 + 1] = g;
        px[i * 4 + 2] = b;
        px[i * 4 + 3] = 255;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindTexture(GL_TEXTURE_2D, 0);
    glFinish();
    return glGetError() == GL_NO_ERROR ? tex : 0;
}

/* Reads texel (x, y) of a texture through a framebuffer in the current
 * context. Returns the GL error, or -1 if the texture can't be attached. */
int share_native_read(unsigned tex, int x, int y, uint8_t out[4])
{
    GLuint fbo = 0;
    while (glGetError() != GL_NO_ERROR)
        ;
    if (!glIsTexture(tex))
        return -1;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    const GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    if (status == GL_FRAMEBUFFER_COMPLETE)
        glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    return status == GL_FRAMEBUFFER_COMPLETE ? (int)glGetError() : -1;
}

/* The video path: the compositor copies its own texture into one ANGLE owns.
 * Returns the GL error. */
int share_native_copy(unsigned src, unsigned dst, int w, int h)
{
    while (glGetError() != GL_NO_ERROR)
        ;
    glCopyImageSubData(src, GL_TEXTURE_2D, 0, 0, 0, 0, dst, GL_TEXTURE_2D, 0, 0, 0, 0, w, h, 1);
    glFinish();
    return (int)glGetError();
}

/* A fence in whichever native context is current, flushed so another context
 * in the share group can wait for it. */
void *share_native_fence(void)
{
    GLsync s = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    return s;
}

/* Waits up to 1 s for a fence from share_native_fence in the current context.
 * Returns the glClientWaitSync status. */
unsigned share_native_wait(void *sync, double *ms)
{
    const double t0 = spike11_now_ms();
    const GLenum r = glClientWaitSync((GLsync)sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
    *ms = spike11_now_ms() - t0;
    glDeleteSync((GLsync)sync);
    return r;
}

void share_native_delete(unsigned tex)
{
    GLuint t = tex;
    glDeleteTextures(1, &t);
}

void share_native_destroy(void)
{
    share_native_release();
    if (anchor != EGL_NO_CONTEXT)
        eglDestroyContext(dpy, anchor);
    if (core != EGL_NO_CONTEXT)
        eglDestroyContext(dpy, core);
    anchor = core = EGL_NO_CONTEXT;
    share_native_bind_es();
}
