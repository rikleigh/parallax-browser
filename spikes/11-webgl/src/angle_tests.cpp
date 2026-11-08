// angle_tests.cpp - WebGL-style use of ANGLE's GLES API (the GL_*/EGL_*
// entry points WebKit's GraphicsContextGLANGLE calls), over ps5-opengl.
//
//   angle-display        EGL_GetPlatformDisplayEXT(ANGLE, OpenGL ES device EGL)
//   angle-context-es3    WebGL2-style ES 3.0 context, surfaceless or pbuffer
//   angle-shader         ESSL 300 es compile + link through ANGLE's translator
//   angle-fbo            RGBA8 texture FBO complete
//   angle-triangle       clear blue, draw a red triangle, readPixels check
//   angle-texture        64x64 checker texImage2D, sampled, readPixels check
//   angle-fence          GL_FenceSync / ClientWaitSync (WebGL2 sync)
//   angle-context-es2    WebGL1-style ES 2.0 context, #version 100 triangle
//   angle-draw-bench     2000 strip draws + 2 uniform4f per draw, per-draw us
//   angle-tex-upload     1920x1080 RGBA texSubImage2D (video-as-texture cost)
//   angle-readpixels     256x256 readPixels
//   share-*              share_probe_run: WebGL sharing with a GL 4.6 core
//                        context the way WebKit sets it up (see below)
#define EGL_NO_PLATFORM_SPECIFIC_TYPES 1
#define GL_GLES_PROTOTYPES 0
#define EGL_EGL_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include "libGLESv2/entry_points_egl_autogen.h"
#include "libGLESv2/entry_points_egl_ext_autogen.h"
#include "libGLESv2/entry_points_gles_2_0_autogen.h"
#include "libGLESv2/entry_points_gles_3_0_autogen.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

extern "C" {
void spike11_log(const char *fmt, ...);
void spike11_result(const char *test, int pass, const char *fmt, ...);
double spike11_now_ms(void);
double spike11_cpu_ms(void);
void angle_tests_run(void);
void share_probe_run(void);
// share_native.c (ps5-opengl directly) and angle_native.cpp (ANGLE internals)
int share_native_create(char *info, unsigned n);
int share_native_make_current(void);
int share_native_create_anchor(char *info, unsigned n);
int share_native_make_anchor_current(void);
int share_native_core_is_current(void);
void share_native_release(void);
void share_native_bind_es(void);
void share_native_bind_gl(void);
int share_native_current(char *info, unsigned n);
unsigned share_native_texture(int w, int h, uint8_t r, uint8_t g, uint8_t b);
int share_native_read(unsigned tex, int x, int y, uint8_t out[4]);
int share_native_copy(unsigned src, unsigned dst, int w, int h);
void *share_native_fence(void);
unsigned share_native_wait(void *sync, double *ms);
void share_native_delete(unsigned tex);
void share_native_destroy(void);
unsigned angle_ps5_native_texture(unsigned name);
}

namespace {

EGLDisplay g_dpy = EGL_NO_DISPLAY;
EGLConfig g_cfg = nullptr;
EGLSurface g_surf = EGL_NO_SURFACE;
bool g_surfaceless = false;

const char *kVs300 =
    "#version 300 es\n"
    "layout(location=0) in vec2 p;\n"
    "uniform vec4 xform;\n"
    "uniform vec4 color;\n"
    "out vec4 c;\n"
    "void main() { c = color; gl_Position = vec4(p * xform.zw + xform.xy, 0.0, 1.0); }\n";
const char *kFs300 =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec4 c;\n"
    "out vec4 o;\n"
    "void main() { o = c; }\n";
const char *kVsTex =
    "#version 300 es\n"
    "layout(location=0) in vec2 p;\n"
    "out vec2 uv;\n"
    "void main() { uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";
const char *kFsTex =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D tex;\n"
    "in vec2 uv;\n"
    "out vec4 o;\n"
    "void main() { o = texture(tex, uv); }\n";
const char *kVs100 =
    "attribute vec2 p;\n"
    "uniform vec4 xform;\n"
    "void main() { gl_Position = vec4(p * xform.zw + xform.xy, 0.0, 1.0); }\n";
const char *kFs100 =
    "precision mediump float;\n"
    "uniform vec4 color;\n"
    "void main() { gl_FragColor = color; }\n";

const float kStrip[] = {-1, -1, 1, -1, -1, 1, 1, 1};
const float kTri[] = {-0.8f, -0.8f, 0.8f, -0.8f, 0.0f, 0.8f};

GLenum err_of(const char *what)
{
    GLenum e = GL_GetError();
    if (e != GL_NO_ERROR)
        spike11_log("  GL error 0x%04x after %s", e, what);
    return e;
}

GLuint shader(GLenum type, const char *src, char *log, size_t n)
{
    GLuint s = GL_CreateShader(type);
    GL_ShaderSource(s, 1, &src, nullptr);
    GL_CompileShader(s);
    GLint ok = 0;
    GL_GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GL_GetShaderInfoLog(s, (GLsizei)n, nullptr, log);
        GL_DeleteShader(s);
        return 0;
    }
    return s;
}

GLuint program(const char *vs, const char *fs, char *log, size_t n)
{
    log[0] = 0;
    GLuint v = shader(GL_VERTEX_SHADER, vs, log, n);
    GLuint f = v ? shader(GL_FRAGMENT_SHADER, fs, log, n) : 0;
    if (!v || !f)
        return 0;
    GLuint p = GL_CreateProgram();
    GL_AttachShader(p, v);
    GL_AttachShader(p, f);
    GL_LinkProgram(p);
    GLint ok = 0;
    GL_GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GL_GetProgramInfoLog(p, (GLsizei)n, nullptr, log);
        GL_DeleteProgram(p);
        return 0;
    }
    GL_DeleteShader(v);
    GL_DeleteShader(f);
    return p;
}

EGLContext make_context(int major, int minor)
{
    const EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION, major, EGL_CONTEXT_MINOR_VERSION, minor, EGL_NONE};
    return EGL_CreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, attrs);
}

bool make_current(EGLContext ctx)
{
    if (g_surfaceless && EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
        return true;
    if (g_surf == EGL_NO_SURFACE) {
        const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        g_surf = EGL_CreatePbufferSurface(g_dpy, g_cfg, pb);
        if (g_surf == EGL_NO_SURFACE) {
            spike11_log("  pbuffer: EGL error 0x%04x", EGL_GetError());
            return false;
        }
        g_surfaceless = false;
    }
    return EGL_MakeCurrent(g_dpy, g_surf, g_surf, ctx) == EGL_TRUE;
}

struct Fbo {
    GLuint tex = 0, fbo = 0;
    int w = 0, h = 0;
};

Fbo make_fbo(int w, int h, bool es3)
{
    Fbo f;
    f.w = w;
    f.h = h;
    GL_GenTextures(1, &f.tex);
    GL_BindTexture(GL_TEXTURE_2D, f.tex);
    if (es3)
        GL_TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    else
        GL_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GL_GenFramebuffers(1, &f.fbo);
    GL_BindFramebuffer(GL_FRAMEBUFFER, f.fbo);
    GL_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f.tex, 0);
    GL_Viewport(0, 0, w, h);
    return f;
}

void pixel(int x, int y, uint8_t out[4])
{
    GL_ReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
}

bool is(const uint8_t p[4], int r, int g, int b)
{
    return abs(p[0] - r) <= 2 && abs(p[1] - g) <= 2 && abs(p[2] - b) <= 2;
}

// --- tests -----------------------------------------------------------------

bool test_display()
{
    const EGLAttrib attrs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_OPENGLES_ANGLE,
        EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_DEVICE_TYPE_EGL_ANGLE,
        EGL_NONE,
    };
    const char *client = EGL_QueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    spike11_log("  ANGLE client extensions: %s", client ? client : "(null)");
    const double t0 = spike11_now_ms();
    g_dpy = EGL_GetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, attrs);
    if (g_dpy == EGL_NO_DISPLAY) {
        spike11_result("angle-display", 0, "EGL_GetPlatformDisplay: 0x%04x", EGL_GetError());
        return false;
    }
    EGLint major = 0, minor = 0;
    if (!EGL_Initialize(g_dpy, &major, &minor)) {
        spike11_result("angle-display", 0, "EGL_Initialize: 0x%04x", EGL_GetError());
        return false;
    }
    const double t1 = spike11_now_ms();
    spike11_result("angle-display", 1, "EGL %d.%d vendor \"%s\" version \"%s\" apis \"%s\" (%.0f ms)", major, minor,
                   EGL_QueryString(g_dpy, EGL_VENDOR), EGL_QueryString(g_dpy, EGL_VERSION),
                   EGL_QueryString(g_dpy, EGL_CLIENT_APIS), t1 - t0);
    const char *ext = EGL_QueryString(g_dpy, EGL_EXTENSIONS);
    spike11_log("  ANGLE display extensions: %s", ext ? ext : "(null)");
    g_surfaceless = ext && strstr(ext, "EGL_KHR_surfaceless_context");

    const EGLint cfg_attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE};
    EGLint n = 0;
    if (!EGL_ChooseConfig(g_dpy, cfg_attrs, &g_cfg, 1, &n) || n != 1) {
        spike11_result("angle-config", 0, "EGL_ChooseConfig: %d configs, 0x%04x", n, EGL_GetError());
        return false;
    }
    EGLint id = 0, rt = 0;
    EGL_GetConfigAttrib(g_dpy, g_cfg, EGL_CONFIG_ID, &id);
    EGL_GetConfigAttrib(g_dpy, g_cfg, EGL_RENDERABLE_TYPE, &rt);
    spike11_result("angle-config", 1, "config id %d renderable 0x%x, surfaceless %d", id, rt, g_surfaceless);
    return true;
}

EGLContext test_context_es3()
{
    const double t0 = spike11_now_ms();
    EGLContext ctx = make_context(3, 0);
    if (ctx == EGL_NO_CONTEXT) {
        spike11_result("angle-context-es3", 0, "EGL_CreateContext 3.0: 0x%04x", EGL_GetError());
        return EGL_NO_CONTEXT;
    }
    if (!make_current(ctx)) {
        spike11_result("angle-context-es3", 0, "EGL_MakeCurrent: 0x%04x", EGL_GetError());
        return EGL_NO_CONTEXT;
    }
    const double t1 = spike11_now_ms();
    GLint next = 0;
    GL_GetIntegerv(GL_NUM_EXTENSIONS, &next);
    spike11_result("angle-context-es3", 1, "GL \"%s\" renderer \"%s\" GLSL \"%s\", %d extensions, %s (%.0f ms)",
                   GL_GetString(GL_VERSION), GL_GetString(GL_RENDERER), GL_GetString(GL_SHADING_LANGUAGE_VERSION),
                   next, g_surfaceless ? "surfaceless" : "pbuffer", t1 - t0);
    const char *all = (const char *)GL_GetString(GL_EXTENSIONS);
    static const char *want[] = {"GL_OES_texture_float", "GL_OES_texture_float_linear", "GL_OES_texture_half_float",
                                 "GL_EXT_color_buffer_float", "GL_OES_element_index_uint", "GL_OES_standard_derivatives",
                                 "GL_EXT_texture_filter_anisotropic", "GL_OES_vertex_array_object", "GL_ANGLE_instanced_arrays",
                                 "GL_EXT_sRGB", "GL_OES_EGL_image", "GL_OES_EGL_image_external", "GL_KHR_debug",
                                 "GL_ANGLE_framebuffer_blit", "GL_ANGLE_framebuffer_multisample", "GL_EXT_disjoint_timer_query",
                                 "GL_OES_texture_npot", "GL_OES_rgb8_rgba8", "GL_EXT_texture_compression_s3tc"};
    char line[1024] = "";
    for (const char *w : want) {
        char tmp[64];
        snprintf(tmp, sizeof tmp, " %s%s", all && strstr(all, w) ? "+" : "-", w + 3);
        strncat(line, tmp, sizeof line - strlen(line) - 1);
    }
    spike11_log("  WebGL-relevant:%s", line);
    GLint maxtex = 0, maxrb = 0, maxvtu = 0, maxva = 0;
    GL_GetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
    GL_GetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxrb);
    GL_GetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &maxvtu);
    GL_GetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxva);
    spike11_log("  limits: max texture %d, renderbuffer %d, vertex texture units %d, vertex attribs %d", maxtex, maxrb,
                maxvtu, maxva);
    return ctx;
}

GLuint test_shader()
{
    char log[512];
    const double t0 = spike11_now_ms();
    GLuint p = program(kVs300, kFs300, log, sizeof log);
    const double t1 = spike11_now_ms();
    if (!p) {
        spike11_result("angle-shader", 0, "ESSL 300 es: %s", log);
        return 0;
    }
    GLint xf = GL_GetUniformLocation(p, "xform"), col = GL_GetUniformLocation(p, "color");
    spike11_result("angle-shader", xf >= 0 && col >= 0, "ESSL 300 es compiled + linked in %.1f ms; uniforms xform %d color %d",
                   t1 - t0, xf, col);
    return p;
}

void test_fbo_triangle(GLuint prog, Fbo *out)
{
    *out = make_fbo(256, 256, true);
    GLenum st = GL_CheckFramebufferStatus(GL_FRAMEBUFFER);
    spike11_result("angle-fbo", st == GL_FRAMEBUFFER_COMPLETE, "RGBA8 256x256 texture FBO status 0x%x", st);

    GLuint vao = 0, vbo = 0;
    GL_GenVertexArrays(1, &vao);
    GL_BindVertexArray(vao);
    GL_GenBuffers(1, &vbo);
    GL_BindBuffer(GL_ARRAY_BUFFER, vbo);
    GL_BufferData(GL_ARRAY_BUFFER, sizeof kTri, kTri, GL_STATIC_DRAW);
    GL_EnableVertexAttribArray(0);
    GL_VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    GL_UseProgram(prog);
    GL_Uniform4f(GL_GetUniformLocation(prog, "xform"), 0, 0, 1, 1);
    GL_Uniform4f(GL_GetUniformLocation(prog, "color"), 1, 0, 0, 1);
    GL_ClearColor(0, 0, 1, 1);
    GL_Clear(GL_COLOR_BUFFER_BIT);
    GL_DrawArrays(GL_TRIANGLES, 0, 3);
    GL_Finish();
    uint8_t c[4], k[4], t[4];
    pixel(128, 100, c);  // inside the triangle
    pixel(4, 250, k);    // top-left corner: cleared
    pixel(128, 240, t);  // above the apex: cleared
    GLenum e = err_of("triangle");
    spike11_result("angle-triangle", e == GL_NO_ERROR && is(c, 255, 0, 0) && is(k, 0, 0, 255) && is(t, 0, 0, 255),
                   "centre %d,%d,%d (want red) corner %d,%d,%d apex+ %d,%d,%d (want blue), GL error 0x%x", c[0], c[1], c[2],
                   k[0], k[1], k[2], t[0], t[1], t[2], e);
}

void test_texture(Fbo *fbo)
{
    char log[512];
    GLuint p = program(kVsTex, kFsTex, log, sizeof log);
    if (!p) {
        spike11_result("angle-texture", 0, "shader: %s", log);
        return;
    }
    std::vector<uint8_t> px(64 * 64 * 4);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            uint8_t *q = &px[(y * 64 + x) * 4];
            bool on = ((x / 32) ^ (y / 32)) & 1;
            q[0] = on ? 255 : 0;
            q[1] = on ? 0 : 255;
            q[2] = 0;
            q[3] = 255;
        }
    GLuint tex = 0;
    GL_ActiveTexture(GL_TEXTURE0);
    GL_GenTextures(1, &tex);
    GL_BindTexture(GL_TEXTURE_2D, tex);
    GL_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint vao = 0, vbo = 0;
    GL_GenVertexArrays(1, &vao);
    GL_BindVertexArray(vao);
    GL_GenBuffers(1, &vbo);
    GL_BindBuffer(GL_ARRAY_BUFFER, vbo);
    GL_BufferData(GL_ARRAY_BUFFER, sizeof kStrip, kStrip, GL_STATIC_DRAW);
    GL_EnableVertexAttribArray(0);
    GL_VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    GL_UseProgram(p);
    GL_Uniform1i(GL_GetUniformLocation(p, "tex"), 0);
    GL_BindFramebuffer(GL_FRAMEBUFFER, fbo->fbo);
    GL_DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    GL_Finish();
    uint8_t a[4], b[4];
    pixel(32, 32, a);    // bottom-left quadrant: (0,0) tile -> green
    pixel(200, 32, b);   // bottom-right quadrant: (1,0) tile -> red
    GLenum e = err_of("texture");
    spike11_result("angle-texture", e == GL_NO_ERROR && is(a, 0, 255, 0) && is(b, 255, 0, 0),
                   "64x64 checker sampled: bl %d,%d,%d (want green) br %d,%d,%d (want red), GL error 0x%x", a[0], a[1],
                   a[2], b[0], b[1], b[2], e);
}

void test_fence()
{
    GLsync s = GL_FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!s) {
        spike11_result("angle-fence", 0, "GL_FenceSync returned 0 (error 0x%x)", GL_GetError());
        return;
    }
    GL_Flush();
    const double t0 = spike11_now_ms();
    GLenum r = GL_ClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
    const double t1 = spike11_now_ms();
    GLint status = 0;
    GLsizei n = 0;
    GL_GetSynciv(s, GL_SYNC_STATUS, 1, &n, &status);
    GL_DeleteSync(s);
    spike11_result("angle-fence", r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED,
                   "ClientWaitSync 0x%x in %.2f ms, status 0x%x", r, t1 - t0, status);
}

void test_draw_bench(GLuint prog)
{
    const int WARMUP = 3, FRAMES = 10, DRAWS = 2000;
    GLuint vao = 0, vbo = 0;
    GL_GenVertexArrays(1, &vao);
    GL_BindVertexArray(vao);
    GL_GenBuffers(1, &vbo);
    GL_BindBuffer(GL_ARRAY_BUFFER, vbo);
    GL_BufferData(GL_ARRAY_BUFFER, sizeof kStrip, kStrip, GL_STATIC_DRAW);
    GL_EnableVertexAttribArray(0);
    GL_VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    GL_UseProgram(prog);
    GLint xf = GL_GetUniformLocation(prog, "xform"), col = GL_GetUniformLocation(prog, "color");
    double issue_ms = 0, issue_cpu = 0, finish_ms = 0, first = 0;
    GL_Finish();
    for (int f = 0; f < WARMUP + FRAMES; f++) {
        const double w0 = spike11_now_ms(), c0 = spike11_cpu_ms();
        GL_Clear(GL_COLOR_BUFFER_BIT);
        for (int d = 0; d < DRAWS; d++) {
            const float a = (float)(d * 37 % 1000) / 500.0f - 1.0f, b = (float)(d * 91 % 1000) / 500.0f - 1.0f;
            GL_Uniform4f(xf, a, b, 0.02f, 0.02f);
            GL_Uniform4f(col, (float)(d & 255) / 255.0f, 0.5f, (float)(f & 7) / 7.0f, 1.0f);
            GL_DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        const double w1 = spike11_now_ms(), c1 = spike11_cpu_ms();
        GL_Finish();
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
    GLenum e = err_of("draw-bench");
    spike11_result("angle-draw-bench", e == GL_NO_ERROR,
                   "%d draws: frame %.2f ms = issue %.2f (cpu %.2f) + finish %.2f; per draw %.1f us (cpu %.1f); "
                   "first frame %.1f ms",
                   DRAWS, issue_ms + finish_ms, issue_ms, issue_cpu, finish_ms, issue_ms * 1e3 / DRAWS,
                   issue_cpu * 1e3 / DRAWS, first);
}

void test_tex_upload()
{
    const int W = 1920, H = 1080;
    std::vector<uint8_t> px((size_t)W * H * 4, 0x80);
    GLuint tex = 0;
    GL_GenTextures(1, &tex);
    GL_BindTexture(GL_TEXTURE_2D, tex);
    GL_TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
    GL_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    GL_Finish();
    double up = 0;
    for (int i = 0; i < 10; i++) {
        px[i] = (uint8_t)i;
        const double t0 = spike11_now_ms();
        GL_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        GL_Finish();
        up += spike11_now_ms() - t0;
    }
    GLenum e = err_of("tex-upload");
    spike11_result("angle-tex-upload-1080p", e == GL_NO_ERROR, "%.2f ms per 1920x1080 RGBA texSubImage2D+finish", up / 10);
    GL_DeleteTextures(1, &tex);
}

void test_readpixels(Fbo *fbo)
{
    std::vector<uint8_t> rb(256 * 256 * 4);
    GL_BindFramebuffer(GL_FRAMEBUFFER, fbo->fbo);
    double ms = 0;
    for (int i = 0; i < 10; i++) {
        const double t0 = spike11_now_ms();
        GL_ReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, rb.data());
        ms += spike11_now_ms() - t0;
    }
    GLenum e = err_of("readpixels");
    spike11_result("angle-readpixels-256", e == GL_NO_ERROR, "%.2f ms per 256x256 readPixels", ms / 10);
}

void test_context_es2()
{
    EGLContext ctx = make_context(2, 0);
    if (ctx == EGL_NO_CONTEXT) {
        spike11_result("angle-context-es2", 0, "EGL_CreateContext 2.0: 0x%04x", EGL_GetError());
        return;
    }
    if (!make_current(ctx)) {
        spike11_result("angle-context-es2", 0, "EGL_MakeCurrent: 0x%04x", EGL_GetError());
        return;
    }
    char log[512];
    GLuint p = program(kVs100, kFs100, log, sizeof log);
    if (!p) {
        spike11_result("angle-context-es2", 0, "GL \"%s\": ESSL 100 shader: %s", GL_GetString(GL_VERSION), log);
        return;
    }
    Fbo f = make_fbo(128, 128, false);
    (void)f;
    GLuint vbo = 0;
    GL_GenBuffers(1, &vbo);
    GL_BindBuffer(GL_ARRAY_BUFFER, vbo);
    GL_BufferData(GL_ARRAY_BUFFER, sizeof kTri, kTri, GL_STATIC_DRAW);
    GLint loc = GL_GetAttribLocation(p, "p");
    GL_EnableVertexAttribArray(loc);
    GL_VertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    GL_UseProgram(p);
    GL_Uniform4f(GL_GetUniformLocation(p, "xform"), 0, 0, 1, 1);
    GL_Uniform4f(GL_GetUniformLocation(p, "color"), 0, 1, 0, 1);
    GL_ClearColor(0, 0, 0, 1);
    GL_Clear(GL_COLOR_BUFFER_BIT);
    GL_DrawArrays(GL_TRIANGLES, 0, 3);
    GL_Finish();
    uint8_t c[4], k[4];
    pixel(64, 50, c);
    pixel(2, 125, k);
    GLenum e = err_of("es2");
    spike11_result("angle-context-es2", e == GL_NO_ERROR && is(c, 0, 255, 0) && is(k, 0, 0, 0),
                   "GL \"%s\" GLSL \"%s\": #version 100 triangle centre %d,%d,%d (want green) corner %d,%d,%d (want black)",
                   GL_GetString(GL_VERSION), GL_GetString(GL_SHADING_LANGUAGE_VERSION), c[0], c[1], c[2], k[0], k[1], k[2]);
    EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    EGL_DestroyContext(g_dpy, ctx);
}

}  // namespace

#ifdef HOST_CHECK
// Spike 2's Rosetta harness: the real EGL facade, driver caps and Mesa, but
// no GPU and no PS5 shader compiler (psbc aborts), so stop before linking.
void test_shader_compile_only(const char *name, const char *vs, const char *fs)
{
    char log[512] = "";
    GLuint v = shader(GL_VERTEX_SHADER, vs, log, sizeof log);
    GLuint f = v ? shader(GL_FRAGMENT_SHADER, fs, log, sizeof log) : 0;
    spike11_result(name, v && f, "ANGLE translation + Mesa front end: vs %u fs %u %s", v, f, log);
}
#endif

void angle_tests_run(void)
{
    if (!test_display())
        return;
    EGLContext es3 = test_context_es3();
#ifdef HOST_CHECK
    if (es3 != EGL_NO_CONTEXT) {
        test_shader_compile_only("angle-shader-es3-compile", kVs300, kFs300);
        test_shader_compile_only("angle-shader-tex-compile", kVsTex, kFsTex);
        EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        EGL_DestroyContext(g_dpy, es3);
    }
    EGLContext es2 = make_context(2, 0);
    if (es2 != EGL_NO_CONTEXT && make_current(es2)) {
        spike11_result("angle-context-es2", 1, "GL \"%s\" GLSL \"%s\"", GL_GetString(GL_VERSION),
                       GL_GetString(GL_SHADING_LANGUAGE_VERSION));
        test_shader_compile_only("angle-shader-es2-compile", kVs100, kFs100);
        EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        EGL_DestroyContext(g_dpy, es2);
    } else
        spike11_result("angle-context-es2", 0, "0x%04x", EGL_GetError());
    spike11_log("angle host check done");
    return;
#endif
    if (es3 != EGL_NO_CONTEXT) {
        GLuint prog = test_shader();
        if (prog) {
            Fbo fbo;
            test_fbo_triangle(prog, &fbo);
            test_texture(&fbo);
            test_fence();
            GL_BindFramebuffer(GL_FRAMEBUFFER, fbo.fbo);
            GL_Viewport(0, 0, fbo.w, fbo.h);
            test_draw_bench(prog);
            test_tex_upload();
            test_readpixels(&fbo);
        }
        EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        EGL_DestroyContext(g_dpy, es3);
    }
    test_context_es2();
    // The display is left initialised on purpose: native_bench.c reuses
    // ps5-opengl's one display, and EGL_Terminate would tear it down.
    EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    spike11_log("angle tests done");
}

// --- share group -------------------------------------------------------------
// The browser's compositor draws with one GL 4.6 core context (PS5Renderer).
// WebKit shares WebGL with such a context as PlatformDisplay::
// angleSharingGLContext() and GraphicsContextGLEGL do: an ANGLE context that
// wraps the native context (EGL_EXTERNAL_CONTEXT_ANGLE, made while it is
// current), then WebGL contexts in virtualization group 0 that share with it.
// ANGLE then makes the group's native ES context with the wrapped context as
// its share context. ANGLE here has no desktop GL backend, so it can't wrap
// the 4.6 core context itself (it would have no GL functions): it wraps an ES
// 3.2 "anchor" made in the core context's share group, which share groups
// being transitive comes to the same. Checked here: that the native context is ES although the
// thread has EGL_OPENGL_API bound (ANGLE binds ES once, at display set-up),
// that the core context sees what ANGLE drew under ANGLE's native name, that
// a fence from ANGLE's context can be waited on in the core one, and that the
// core context can copy into ANGLE's texture (the video path).

namespace {

EGLContext share_webgl_context(EGLContext ext, bool *es, char *info, size_t n)
{
    const EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3,
                            EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE, EGL_TRUE,
                            EGL_ROBUST_RESOURCE_INITIALIZATION_ANGLE, EGL_TRUE,
                            EGL_CONTEXT_CLIENT_ARRAYS_ENABLED_ANGLE, EGL_FALSE,
                            EGL_CONTEXT_BIND_GENERATES_RESOURCE_CHROMIUM, EGL_FALSE,
                            EGL_CONTEXT_VIRTUALIZATION_GROUP_ANGLE, 0,
                            EGL_NONE};
    *es = false;
    EGLContext ctx = EGL_CreateContext(g_dpy, g_cfg, ext, attrs);
    if (ctx == EGL_NO_CONTEXT) {
        snprintf(info, n, "EGL_CreateContext 0x%04x", EGL_GetError());
        return EGL_NO_CONTEXT;
    }
    const bool coreStillCurrent = share_native_core_is_current();
    if (!make_current(ctx)) {
        snprintf(info, n, "EGL_MakeCurrent 0x%04x", EGL_GetError());
        EGL_DestroyContext(g_dpy, ctx);
        return EGL_NO_CONTEXT;
    }
    char native[160];
    *es = share_native_current(native, sizeof native);
    snprintf(info, n, "GL \"%s\"; %s; core context %s current after EGL_CreateContext", GL_GetString(GL_VERSION),
             native, coreStillCurrent ? "still" : "no longer");
    return ctx;
}

} // namespace

void share_probe_run(void)
{
    if (g_dpy == EGL_NO_DISPLAY)
        return;
    char info[512];
    EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (!share_native_create(info, sizeof info)) {
        spike11_result("share-core-context", 0, "%s", info);
        return;
    }
    spike11_result("share-core-context", 1, "%s", info);
    spike11_log("  share: a texture in the core context");
    const unsigned green = share_native_texture(64, 64, 0, 255, 0);

    const int anchored = share_native_create_anchor(info, sizeof info);
    spike11_result("share-anchor", anchored, "%s", info);
    if (!anchored) {
        share_native_destroy();
        return;
    }
    spike11_log("  share: core texture %u; the ANGLE external context around the anchor", green);
    share_native_make_anchor_current();
    const EGLint extAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_EXTERNAL_CONTEXT_ANGLE, EGL_TRUE, EGL_NONE};
    EGLContext ext = EGL_CreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, extAttrs);
    share_native_make_current();
    spike11_result("share-external-context", ext != EGL_NO_CONTEXT, "EGL_EXTERNAL_CONTEXT_ANGLE around the anchor: %s 0x%04x",
                   ext != EGL_NO_CONTEXT ? "created" : "failed", EGL_GetError());
    if (ext == EGL_NO_CONTEXT) {
        share_native_destroy();
        return;
    }

    // ANGLE binds EGL_OPENGL_ES_API once, at display set-up, and creates each
    // virtualization group's native context with whatever API the thread has
    // bound. The compositor's thread has EGL_OPENGL_API bound, which gives a
    // desktop GL context and the same null-function crash as above. A port
    // binds ES first (or patches DisplayEGL::initializeContext to); so does this.
    if (getenv("SPIKE11_SHARE_GL_BOUND"))
        spike11_log("  share: leaving EGL_OPENGL_API bound (expect a crash)");
    else
        share_native_bind_es();
    bool es = false;
    spike11_log("  share: the WebGL context");
    EGLContext webgl = share_webgl_context(ext, &es, info, sizeof info);
    spike11_result("share-webgl-context", webgl != EGL_NO_CONTEXT && es, "%s", info);
    if (webgl == EGL_NO_CONTEXT) {
        EGL_DestroyContext(g_dpy, ext);
        share_native_destroy();
        return;
    }
    share_native_bind_gl();

#ifdef HOST_CHECK
    // No PS5 shader compiler on the Mac: linking a program would abort.
    EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    EGL_DestroyContext(g_dpy, webgl);
    EGL_DestroyContext(g_dpy, ext);
    share_native_destroy();
    spike11_log("share probe done (host: contexts only)");
    return;
#endif
    // Draw in ANGLE: blue, with a red triangle.
    spike11_log("  share: drawing through ANGLE");
    char log[512];
    GLuint prog = program(kVs300, kFs300, log, sizeof log);
    if (!prog) {
        spike11_result("share-draw", 0, "shader: %s", log);
        EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        EGL_DestroyContext(g_dpy, webgl);
        EGL_DestroyContext(g_dpy, ext);
        share_native_destroy();
        return;
    }
    Fbo fbo = make_fbo(256, 256, true);
    GLuint vao = 0, vbo = 0;
    GL_GenVertexArrays(1, &vao);
    GL_BindVertexArray(vao);
    GL_GenBuffers(1, &vbo);
    GL_BindBuffer(GL_ARRAY_BUFFER, vbo);
    GL_BufferData(GL_ARRAY_BUFFER, sizeof kTri, kTri, GL_STATIC_DRAW);
    GL_EnableVertexAttribArray(0);
    GL_VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    GL_UseProgram(prog);
    GL_Uniform4f(GL_GetUniformLocation(prog, "xform"), 0, 0, 1, 1);
    GL_Uniform4f(GL_GetUniformLocation(prog, "color"), 1, 0, 0, 1);
    GL_ClearColor(0, 0, 1, 1);
    GL_Clear(GL_COLOR_BUFFER_BIT);
    GL_DrawArrays(GL_TRIANGLES, 0, 3);
    GL_Flush();
    void *fence = share_native_fence();
    uint8_t c[4], k[4];
    pixel(128, 100, c);
    pixel(4, 250, k);
    const unsigned nativeTex = angle_ps5_native_texture(fbo.tex);
    GLenum e = err_of("share draw");
    spike11_result("share-draw", e == GL_NO_ERROR && is(c, 255, 0, 0) && is(k, 0, 0, 255) && nativeTex,
                   "ANGLE texture %u is native texture %u; ANGLE reads centre %d,%d,%d corner %d,%d,%d, GL error 0x%x", fbo.tex,
                   nativeTex, c[0], c[1], c[2], k[0], k[1], k[2], e);

    // The compositor's side: wait for ANGLE's fence, then read ANGLE's texture.
    EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    share_native_make_current();
    double ms = 0;
    const unsigned w = fence ? share_native_wait(fence, &ms) : 0;
    spike11_result("share-fence", w == GL_ALREADY_SIGNALED || w == GL_CONDITION_SATISFIED,
                   "fence from ANGLE's native context waited on in the core context: status 0x%x in %.2f ms", w, ms);
    int r1 = share_native_read(nativeTex, 128, 100, c), r2 = share_native_read(nativeTex, 4, 250, k);
    spike11_result("share-texture", !r1 && !r2 && is(c, 255, 0, 0) && is(k, 0, 0, 255),
                   "core context reads native texture %u: centre %d,%d,%d (want red) corner %d,%d,%d (want blue), errors %d %d",
                   nativeTex, c[0], c[1], c[2], k[0], k[1], k[2], r1, r2);

    // The video path: the core context copies its own texture into ANGLE's.
    const int ce = green ? share_native_copy(green, nativeTex, 64, 64) : -1;
    share_native_release();
    make_current(webgl);
    GL_BindFramebuffer(GL_FRAMEBUFFER, fbo.fbo);
    uint8_t g[4];
    pixel(10, 10, g);
    pixel(128, 100, c);
    e = err_of("share copy read");
    spike11_result("share-copy", !ce && e == GL_NO_ERROR && is(g, 0, 255, 0) && is(c, 255, 0, 0),
                   "glCopyImageSubData core texture %u -> native %u (error %d); ANGLE reads 10,10 %d,%d,%d (want green) "
                   "centre %d,%d,%d (want red)",
                   green, nativeTex, ce, g[0], g[1], g[2], c[0], c[1], c[2]);

    GL_DeleteBuffers(1, &vbo);
    GL_DeleteVertexArrays(1, &vao);
    GL_DeleteFramebuffers(1, &fbo.fbo);
    GL_DeleteTextures(1, &fbo.tex);
    GL_DeleteProgram(prog);
    EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    EGL_DestroyContext(g_dpy, webgl);
    EGL_DestroyContext(g_dpy, ext);
    if (share_native_make_current() && green)
        share_native_delete(green);
    share_native_destroy();
    spike11_log("share probe done");
}
