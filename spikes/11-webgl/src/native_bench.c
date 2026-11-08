/* native_bench.c - the ANGLE workloads again, straight through ps5-opengl's
 * EGL and a GLES 3.0 context, for the comparison figures. Same shaders,
 * same draw pattern (Spike 2b's "uniform-draw": a strip and two glUniform4f
 * per draw), same texture upload. Runs after the ANGLE tests on the same
 * (still initialised) EGL display. */
#define EGL_NO_PLATFORM_SPECIFIC_TYPES 1
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void spike11_log(const char *fmt, ...);
void spike11_result(const char *test, int pass, const char *fmt, ...);
double spike11_now_ms(void);
double spike11_cpu_ms(void);

#define WARMUP 3
#define FRAMES 10
#define DRAWS 2000

static const char *kVs =
    "#version 300 es\n"
    "layout(location=0) in vec2 p;\n"
    "uniform vec4 xform;\n"
    "uniform vec4 color;\n"
    "out vec4 c;\n"
    "void main() { c = color; gl_Position = vec4(p * xform.zw + xform.xy, 0.0, 1.0); }\n";
static const char *kFs =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec4 c;\n"
    "out vec4 o;\n"
    "void main() { o = c; }\n";
static const float kStrip[] = {-1, -1, 1, -1, -1, 1, 1, 1};

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        spike11_log("  native shader: %s", log);
        return 0;
    }
    return s;
}

void native_bench_run(void)
{
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
        spike11_result("native-init", 0, "eglInitialize 0x%04x", eglGetError());
        return;
    }
    static const EGLint cfg_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                       EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                       EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n) || n != 1) {
        spike11_result("native-init", 0, "no ES3 config (0x%04x)", eglGetError());
        return;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint ctx_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        spike11_result("native-init", 0, "context %p makeCurrent error 0x%04x", ctx, eglGetError());
        return;
    }
    spike11_result("native-init", 1, "EGL %d.%d, GL \"%s\" GLSL \"%s\"", major, minor,
                   glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));

    /* FBO 256x256 RGBA8, like the ANGLE test */
    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 256, 256);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, 256, 256);

    GLuint vs = shader(GL_VERTEX_SHADER, kVs), fs = shader(GL_FRAGMENT_SHADER, kFs);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!vs || !fs || !linked) {
        spike11_result("native-program", 0, "compile/link failed");
        return;
    }
    glUseProgram(prog);
    GLint xf = glGetUniformLocation(prog, "xform"), col = glGetUniformLocation(prog, "color");
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof kStrip, kStrip, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

    double issue_ms = 0, issue_cpu = 0, finish_ms = 0, first = 0;
    glFinish();
    for (int f = 0; f < WARMUP + FRAMES; f++) {
        const double w0 = spike11_now_ms(), c0 = spike11_cpu_ms();
        glClear(GL_COLOR_BUFFER_BIT);
        for (int d = 0; d < DRAWS; d++) {
            const float a = (float)(d * 37 % 1000) / 500.0f - 1.0f, b = (float)(d * 91 % 1000) / 500.0f - 1.0f;
            glUniform4f(xf, a, b, 0.02f, 0.02f);
            glUniform4f(col, (float)(d & 255) / 255.0f, 0.5f, (float)(f & 7) / 7.0f, 1.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        const double w1 = spike11_now_ms(), c1 = spike11_cpu_ms();
        glFinish();
        const double w2 = spike11_now_ms();
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
    GLenum err = glGetError();
    spike11_result("native-draw-bench", err == GL_NO_ERROR,
                   "%d draws: frame %.2f ms = issue %.2f (cpu %.2f) + finish %.2f; per draw %.1f us "
                   "(cpu %.1f); first frame %.1f ms; GL error 0x%x",
                   DRAWS, issue_ms + finish_ms, issue_ms, issue_cpu, finish_ms, issue_ms * 1e3 / DRAWS,
                   issue_cpu * 1e3 / DRAWS, first, err);

    /* 1920x1080 RGBA upload (what a video frame would cost as a CPU texture) */
    const int W = 1920, H = 1080;
    uint8_t *pix = malloc((size_t)W * H * 4);
    memset(pix, 0x80, (size_t)W * H * 4);
    GLuint vtex;
    glGenTextures(1, &vtex);
    glBindTexture(GL_TEXTURE_2D, vtex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pix);
    glFinish();
    double up_ms = 0;
    for (int i = 0; i < 10; i++) {
        pix[i] = (uint8_t)i;
        const double t0 = spike11_now_ms();
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pix);
        glFinish();
        up_ms += spike11_now_ms() - t0;
    }
    err = glGetError();
    spike11_result("native-tex-upload-1080p", err == GL_NO_ERROR, "%.2f ms per 1920x1080 RGBA texSubImage2D+finish (GL error 0x%x)",
                   up_ms / 10, err);
    free(pix);

    /* readPixels of the 256x256 FBO */
    uint8_t *rb = malloc(256 * 256 * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    double rp_ms = 0;
    for (int i = 0; i < 10; i++) {
        const double t0 = spike11_now_ms();
        glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, rb);
        rp_ms += spike11_now_ms() - t0;
    }
    err = glGetError();
    spike11_result("native-readpixels-256", err == GL_NO_ERROR, "%.2f ms per 256x256 readPixels (GL error 0x%x)", rp_ms / 10, err);
    free(rb);

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
}
