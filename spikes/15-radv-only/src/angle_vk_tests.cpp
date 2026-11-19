// angle_vk_tests.cpp - spike 15a: ANGLE's Vulkan backend on RADV, the title
// linking no ps5-opengl. The checks and workloads are Spike 11's
// (spikes/11-webgl/src/angle_tests.cpp), so their numbers compare directly
// with ANGLE over ps5-opengl (8.3 us per draw, 34.5 ms per 1080p upload):
//
//   angle-display        EGL_GetPlatformDisplay(ANGLE, Vulkan, VK_KHR_display)
//   angle-context-es3    WebGL2-style ES 3.0 context, surfaceless or pbuffer
//   angle-shader         ESSL 300 es compile + link (ANGLE's SPIR-V output)
//   angle-fbo            RGBA8 texture FBO complete
//   angle-triangle       clear blue, draw a red triangle, readPixels check
//   angle-texture        64x64 checker texImage2D, sampled, readPixels check
//   angle-fence          GL_FenceSync / ClientWaitSync (WebGL2 sync)
//   angle-context-es2    WebGL1-style ES 2.0 context, #version 100 triangle
//   angle-draw-bench     2000 strip draws + 2 uniform4f per draw, per-draw us
//   angle-tex-upload     1920x1080 RGBA texSubImage2D (video-as-texture cost)
//   angle-readpixels     256x256 readPixels
// and new here:
//   angle-present        a 1920x1080 window surface on VK_KHR_display
//                        (VideoOut): frames/s over 10 s, swap interval 1
//   angle-present-draws  the same with 2000 draws a frame (the per-draw cost
//                        while presenting)
//   angle-compositor     a TextureMapper-shaped 1080p frame offscreen: ~300
//                        layers, 6 shader variants, texture binds, blending,
//                        scissors, 4 tile uploads; first frame = pipeline stall
//   angle-present-compositor  the same frame on the TV, 600 frames
#define EGL_NO_PLATFORM_SPECIFIC_TYPES 1
#define GL_GLES_PROTOTYPES 0
#define EGL_EGL_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>

#include "libGLESv2/entry_points_egl_autogen.h"
#include "libGLESv2/entry_points_egl_ext_autogen.h"
#include "libGLESv2/entry_points_gles_2_0_autogen.h"
#include "libGLESv2/entry_points_gles_3_0_autogen.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <functional>
#include <string>
#include <vector>

extern "C" {
void spike15_log(const char *fmt, ...);
void spike15_result(const char *test, int pass, const char *fmt, ...);
double spike15_now_ms(void);
double spike15_cpu_ms(void);
void angle_vk_tests_run(void);
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
        spike15_log("  GL error 0x%04x after %s", e, what);
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
            spike15_log("  pbuffer: EGL error 0x%04x", EGL_GetError());
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
    // Vulkan on the "simple" display: window surfaces are VK_KHR_display
    // surfaces, which PS5_Mesa's RADV puts on VideoOut.
    const EGLAttrib attrs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE,
        EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE, EGL_PLATFORM_VULKAN_DISPLAY_MODE_SIMPLE_ANGLE,
        EGL_NONE,
    };
    const char *client = EGL_QueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    spike15_log("  ANGLE client extensions: %s", client ? client : "(null)");
    const double t0 = spike15_now_ms();
    g_dpy = EGL_GetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, attrs);
    if (g_dpy == EGL_NO_DISPLAY) {
        spike15_result("angle-display", 0, "EGL_GetPlatformDisplay: 0x%04x", EGL_GetError());
        return false;
    }
    EGLint major = 0, minor = 0;
    if (!EGL_Initialize(g_dpy, &major, &minor)) {
        spike15_result("angle-display", 0, "EGL_Initialize: 0x%04x", EGL_GetError());
        return false;
    }
    const double t1 = spike15_now_ms();
    spike15_result("angle-display", 1, "EGL %d.%d vendor \"%s\" version \"%s\" apis \"%s\" (%.0f ms)", major, minor,
                   EGL_QueryString(g_dpy, EGL_VENDOR), EGL_QueryString(g_dpy, EGL_VERSION),
                   EGL_QueryString(g_dpy, EGL_CLIENT_APIS), t1 - t0);
    const char *ext = EGL_QueryString(g_dpy, EGL_EXTENSIONS);
    spike15_log("  ANGLE display extensions: %s", ext ? ext : "(null)");
    g_surfaceless = ext && strstr(ext, "EGL_KHR_surfaceless_context");

    const EGLint cfg_attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT | EGL_WINDOW_BIT,
                                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE};
    EGLint n = 0;
    if (!EGL_ChooseConfig(g_dpy, cfg_attrs, &g_cfg, 1, &n) || n != 1) {
        spike15_result("angle-config", 0, "EGL_ChooseConfig: %d configs, 0x%04x", n, EGL_GetError());
        return false;
    }
    EGLint id = 0, rt = 0;
    EGL_GetConfigAttrib(g_dpy, g_cfg, EGL_CONFIG_ID, &id);
    EGL_GetConfigAttrib(g_dpy, g_cfg, EGL_RENDERABLE_TYPE, &rt);
    spike15_result("angle-config", 1, "config id %d renderable 0x%x, surfaceless %d", id, rt, g_surfaceless);
    return true;
}

EGLContext test_context_es3()
{
    const double t0 = spike15_now_ms();
    EGLContext ctx = make_context(3, 0);
    if (ctx == EGL_NO_CONTEXT) {
        spike15_result("angle-context-es3", 0, "EGL_CreateContext 3.0: 0x%04x", EGL_GetError());
        return EGL_NO_CONTEXT;
    }
    if (!make_current(ctx)) {
        spike15_result("angle-context-es3", 0, "EGL_MakeCurrent: 0x%04x", EGL_GetError());
        return EGL_NO_CONTEXT;
    }
    const double t1 = spike15_now_ms();
    GLint next = 0;
    GL_GetIntegerv(GL_NUM_EXTENSIONS, &next);
    spike15_result("angle-context-es3", 1, "GL \"%s\" renderer \"%s\" GLSL \"%s\", %d extensions, %s (%.0f ms)",
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
    spike15_log("  WebGL-relevant:%s", line);
    GLint maxtex = 0, maxrb = 0, maxvtu = 0, maxva = 0;
    GL_GetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
    GL_GetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxrb);
    GL_GetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &maxvtu);
    GL_GetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxva);
    spike15_log("  limits: max texture %d, renderbuffer %d, vertex texture units %d, vertex attribs %d", maxtex, maxrb,
                maxvtu, maxva);
    return ctx;
}

GLuint test_shader()
{
    char log[512];
    const double t0 = spike15_now_ms();
    GLuint p = program(kVs300, kFs300, log, sizeof log);
    const double t1 = spike15_now_ms();
    if (!p) {
        spike15_result("angle-shader", 0, "ESSL 300 es: %s", log);
        return 0;
    }
    GLint xf = GL_GetUniformLocation(p, "xform"), col = GL_GetUniformLocation(p, "color");
    spike15_result("angle-shader", xf >= 0 && col >= 0, "ESSL 300 es compiled + linked in %.1f ms; uniforms xform %d color %d",
                   t1 - t0, xf, col);
    return p;
}

void test_fbo_triangle(GLuint prog, Fbo *out)
{
    spike15_log("  step: fbo");
    *out = make_fbo(256, 256, true);
    GLenum st = GL_CheckFramebufferStatus(GL_FRAMEBUFFER);
    spike15_result("angle-fbo", st == GL_FRAMEBUFFER_COMPLETE, "RGBA8 256x256 texture FBO status 0x%x", st);

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
    spike15_log("  step: clear + draw");
    GL_Clear(GL_COLOR_BUFFER_BIT);
    GL_DrawArrays(GL_TRIANGLES, 0, 3);
    spike15_log("  step: finish");
    GL_Finish();
    spike15_log("  step: readPixels");
    uint8_t c[4], k[4], t[4];
    pixel(128, 100, c);  // inside the triangle
    pixel(4, 250, k);    // top-left corner: cleared
    pixel(128, 240, t);  // above the apex: cleared
    GLenum e = err_of("triangle");
    spike15_result("angle-triangle", e == GL_NO_ERROR && is(c, 255, 0, 0) && is(k, 0, 0, 255) && is(t, 0, 0, 255),
                   "centre %d,%d,%d (want red) corner %d,%d,%d apex+ %d,%d,%d (want blue), GL error 0x%x", c[0], c[1], c[2],
                   k[0], k[1], k[2], t[0], t[1], t[2], e);
}

void test_texture(Fbo *fbo)
{
    char log[512];
    GLuint p = program(kVsTex, kFsTex, log, sizeof log);
    if (!p) {
        spike15_result("angle-texture", 0, "shader: %s", log);
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
    spike15_result("angle-texture", e == GL_NO_ERROR && is(a, 0, 255, 0) && is(b, 255, 0, 0),
                   "64x64 checker sampled: bl %d,%d,%d (want green) br %d,%d,%d (want red), GL error 0x%x", a[0], a[1],
                   a[2], b[0], b[1], b[2], e);
}

void test_fence()
{
    GLsync s = GL_FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!s) {
        spike15_result("angle-fence", 0, "GL_FenceSync returned 0 (error 0x%x)", GL_GetError());
        return;
    }
    GL_Flush();
    const double t0 = spike15_now_ms();
    GLenum r = GL_ClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
    const double t1 = spike15_now_ms();
    GLint status = 0;
    GLsizei n = 0;
    GL_GetSynciv(s, GL_SYNC_STATUS, 1, &n, &status);
    GL_DeleteSync(s);
    spike15_result("angle-fence", r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED,
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
        const double w0 = spike15_now_ms(), c0 = spike15_cpu_ms();
        GL_Clear(GL_COLOR_BUFFER_BIT);
        for (int d = 0; d < DRAWS; d++) {
            const float a = (float)(d * 37 % 1000) / 500.0f - 1.0f, b = (float)(d * 91 % 1000) / 500.0f - 1.0f;
            GL_Uniform4f(xf, a, b, 0.02f, 0.02f);
            GL_Uniform4f(col, (float)(d & 255) / 255.0f, 0.5f, (float)(f & 7) / 7.0f, 1.0f);
            GL_DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        const double w1 = spike15_now_ms(), c1 = spike15_cpu_ms();
        GL_Finish();
        const double w2 = spike15_now_ms();
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
    spike15_result("angle-draw-bench", e == GL_NO_ERROR,
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
        const double t0 = spike15_now_ms();
        GL_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        GL_Finish();
        up += spike15_now_ms() - t0;
    }
    GLenum e = err_of("tex-upload");
    spike15_result("angle-tex-upload-1080p", e == GL_NO_ERROR, "%.2f ms per 1920x1080 RGBA texSubImage2D+finish", up / 10);
    GL_DeleteTextures(1, &tex);
}

void test_readpixels(Fbo *fbo)
{
    std::vector<uint8_t> rb(256 * 256 * 4);
    GL_BindFramebuffer(GL_FRAMEBUFFER, fbo->fbo);
    double ms = 0;
    for (int i = 0; i < 10; i++) {
        const double t0 = spike15_now_ms();
        GL_ReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, rb.data());
        ms += spike15_now_ms() - t0;
    }
    GLenum e = err_of("readpixels");
    spike15_result("angle-readpixels-256", e == GL_NO_ERROR, "%.2f ms per 256x256 readPixels", ms / 10);
}

void test_context_es2()
{
    EGLContext ctx = make_context(2, 0);
    if (ctx == EGL_NO_CONTEXT) {
        spike15_result("angle-context-es2", 0, "EGL_CreateContext 2.0: 0x%04x", EGL_GetError());
        return;
    }
    if (!make_current(ctx)) {
        spike15_result("angle-context-es2", 0, "EGL_MakeCurrent: 0x%04x", EGL_GetError());
        return;
    }
    char log[512];
    GLuint p = program(kVs100, kFs100, log, sizeof log);
    if (!p) {
        spike15_result("angle-context-es2", 0, "GL \"%s\": ESSL 100 shader: %s", GL_GetString(GL_VERSION), log);
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
    spike15_result("angle-context-es2", e == GL_NO_ERROR && is(c, 0, 255, 0) && is(k, 0, 0, 0),
                   "GL \"%s\" GLSL \"%s\": #version 100 triangle centre %d,%d,%d (want green) corner %d,%d,%d (want black)",
                   GL_GetString(GL_VERSION), GL_GetString(GL_SHADING_LANGUAGE_VERSION), c[0], c[1], c[2], k[0], k[1], k[2]);
    EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    EGL_DestroyContext(g_dpy, ctx);
}


// ES 3.1 (SSBOs, compute): what the compositor's video conversion needs once
// its "#version 430 core" shader becomes "#version 310 es" (Route A).
void test_context_es31()
{
    EGLContext ctx = make_context(3, 1);
    if (ctx == EGL_NO_CONTEXT || !make_current(ctx)) {
        spike15_result("angle-context-es31", 0, "ES 3.1 context: 0x%04x", EGL_GetError());
        return;
    }
    GLint ssbo = 0, cs = 0;
    GL_GetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &ssbo);
    GL_GetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &cs);
    spike15_result("angle-context-es31", ssbo > 0, "GL \"%s\" GLSL \"%s\", SSBO bindings %d, compute invocations %d",
                   GL_GetString(GL_VERSION), GL_GetString(GL_SHADING_LANGUAGE_VERSION), ssbo, cs);
    EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    EGL_DestroyContext(g_dpy, ctx);
}

// --- a compositor-like frame (TextureMapper's shape) -------------------------
//
// What WebKit's TextureMapper does per layer: pick a shader variant, bind
// the layer's texture, set mat4 model-view-projection and texture matrices
// and opacity, turn blending on for non-opaque layers, scissor for clips,
// draw a unit quad. A frame here is about 300 layers at 1920x1080:
//   - page tiles: 8x5 opaque 256x256 textured tiles that scroll (variant 0,
//     blend off);
//   - 160 element layers, blended, cycling (in runs of 4) through textured
//     with opacity, anti-aliased edges, rounded-corner clip, a colour-matrix
//     filter (sepia/grey) and solid colour; every 4th one is scissored;
//   - 96 small solid layers (badges, carets);
//   - 2 full-screen translucent overlays (fill cost);
// and 4 tile repaints a frame (256x256 texSubImage2D each).

const char *kVsComp =
    "#version 300 es\n"
    "layout(location=0) in vec2 p;\n"
    "uniform mat4 u_matrix;\n"
    "uniform mat4 u_texMatrix;\n"
    "out vec2 v_uv;\n"
    "out vec2 v_p;\n"
    "void main() { v_p = p; v_uv = (u_texMatrix * vec4(p, 0.0, 1.0)).xy; gl_Position = u_matrix * vec4(p, 0.0, 1.0); }\n";
// Variant bodies; each gets the same header.
const char *kFsCompHead =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D s;\n"
    "uniform float u_opacity;\n"
    "uniform vec4 u_color;\n"
    "uniform vec4 u_rect;\n"   // layer w, h (px), corner radius, aa scale
    "uniform mat4 u_colorMatrix;\n"
    "in vec2 v_uv;\n"
    "in vec2 v_p;\n"
    "out vec4 o;\n";
const char *kFsCompBody[] = {
    // 0 opaque texture
    "void main() { o = texture(s, v_uv); }\n",
    // 1 texture * opacity
    "void main() { o = texture(s, v_uv) * u_opacity; }\n",
    // 2 anti-aliased edges
    "void main() { vec2 e = min(v_p, 1.0 - v_p) * u_rect.xy; float a = clamp(min(e.x, e.y) * u_rect.w, 0.0, 1.0);\n"
    "  o = texture(s, v_uv) * (u_opacity * a); }\n",
    // 3 rounded-corner clip
    "void main() { vec2 h = 0.5 * u_rect.xy; vec2 d = abs(v_p * u_rect.xy - h) - (h - u_rect.z);\n"
    "  float dist = length(max(d, 0.0)) - u_rect.z; float a = clamp(0.5 - dist, 0.0, 1.0);\n"
    "  o = texture(s, v_uv) * (u_opacity * a); }\n",
    // 4 colour-matrix filter
    "void main() { vec4 c = texture(s, v_uv); vec4 f = u_colorMatrix * vec4(c.rgb, 1.0);\n"
    "  o = vec4(clamp(f.rgb, 0.0, 1.0) * c.a, c.a) * u_opacity; }\n",
    // 5 solid colour
    "void main() { o = vec4(u_color.rgb * u_color.a, u_color.a) * u_opacity; }\n",
};
constexpr int kCompVariants = 6;
constexpr int kTile = 256, kTiles = 64;

struct CompProgram {
    GLuint prog = 0;
    GLint matrix = -1, texMatrix = -1, opacity = -1, color = -1, rect = -1, colorMatrix = -1;
};

struct Compositor {
    CompProgram v[kCompVariants];
    GLuint tex[kTiles] = {};
    GLuint vao = 0, vbo = 0;
    std::vector<uint8_t> repaint;
    double compile_ms = 0;
    // State changes in one frame (counted by the first frame).
    int layers = 0, prog_switches = 0, binds = 0, blend_toggles = 0, scissor_toggles = 0;

    bool init(char *log, size_t n)
    {
        const double t0 = spike15_now_ms();
        for (int i = 0; i < kCompVariants; i++) {
            std::string fs = std::string(kFsCompHead) + kFsCompBody[i];
            v[i].prog = program(kVsComp, fs.c_str(), log, n);
            if (!v[i].prog)
                return false;
            GLuint p = v[i].prog;
            v[i].matrix = GL_GetUniformLocation(p, "u_matrix");
            v[i].texMatrix = GL_GetUniformLocation(p, "u_texMatrix");
            v[i].opacity = GL_GetUniformLocation(p, "u_opacity");
            v[i].color = GL_GetUniformLocation(p, "u_color");
            v[i].rect = GL_GetUniformLocation(p, "u_rect");
            v[i].colorMatrix = GL_GetUniformLocation(p, "u_colorMatrix");
        }
        compile_ms = spike15_now_ms() - t0;
        static const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
        GL_GenVertexArrays(1, &vao);
        GL_BindVertexArray(vao);
        GL_GenBuffers(1, &vbo);
        GL_BindBuffer(GL_ARRAY_BUFFER, vbo);
        GL_BufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
        GL_EnableVertexAttribArray(0);
        GL_VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        // Tiles: a checker in a hue of their own, a darker diagonal band.
        std::vector<uint8_t> px((size_t)kTile * kTile * 4);
        GL_GenTextures(kTiles, tex);
        for (int t = 0; t < kTiles; t++) {
            const int r = 60 + (t * 53) % 180, g = 60 + (t * 97) % 180, b = 60 + (t * 31) % 180;
            for (int y = 0; y < kTile; y++)
                for (int x = 0; x < kTile; x++) {
                    uint8_t *q = &px[((size_t)y * kTile + x) * 4];
                    const bool on = ((x >> 5) ^ (y >> 5)) & 1;
                    const bool band = ((x + y) & 63) < 8;
                    const int k = on ? 255 : 200, d = band ? 2 : 1;
                    q[0] = (uint8_t)(r * k / 255 / d);
                    q[1] = (uint8_t)(g * k / 255 / d);
                    q[2] = (uint8_t)(b * k / 255 / d);
                    q[3] = 255;
                }
            GL_BindTexture(GL_TEXTURE_2D, tex[t]);
            GL_TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kTile, kTile);
            GL_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kTile, kTile, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        repaint = px;
        return err_of("compositor init") == GL_NO_ERROR;
    }

    void destroy()
    {
        for (auto &p : v)
            GL_DeleteProgram(p.prog);
        GL_DeleteTextures(kTiles, tex);
        GL_DeleteBuffers(1, &vbo);
        GL_DeleteVertexArrays(1, &vao);
    }

    // Column-major: the unit quad onto the pixel rect (x, y, w, h) of a W x H
    // target, y down.
    static void rect_matrix(float m[16], float x, float y, float w, float h, int W, int H)
    {
        memset(m, 0, 16 * sizeof(float));
        m[0] = 2 * w / W;
        m[5] = -2 * h / H;
        m[10] = 1;
        m[12] = 2 * x / W - 1;
        m[13] = 1 - 2 * y / H;
        m[15] = 1;
    }

    // One frame into the bound framebuffer (W x H). `count` fills the state
    // change counters.
    void frame(int f, int W, int H, bool count)
    {
        static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        static const float sepia[16] = {0.393f, 0.349f, 0.272f, 0, 0.769f, 0.686f, 0.534f, 0,
                                        0.189f, 0.168f, 0.131f, 0, 0, 0, 0, 1};
        static const float grey[16] = {0.299f, 0.299f, 0.299f, 0, 0.587f, 0.587f, 0.587f, 0,
                                       0.114f, 0.114f, 0.114f, 0, 0, 0, 0, 1};
        int cur = -1, curTex = -1;
        bool blend = false, scissor = false;
        float m[16];
        auto use = [&](int i) {
            if (i != cur) {
                GL_UseProgram(v[i].prog);
                cur = i;
                if (count)
                    prog_switches++;
            }
        };
        auto bind = [&](int t) {
            if (t != curTex) {
                GL_BindTexture(GL_TEXTURE_2D, tex[t]);
                curTex = t;
                if (count)
                    binds++;
            }
        };
        auto set_blend = [&](bool on) {
            if (on != blend) {
                on ? GL_Enable(GL_BLEND) : GL_Disable(GL_BLEND);
                blend = on;
                if (count)
                    blend_toggles++;
            }
        };
        auto set_scissor = [&](bool on, int x, int y, int w, int h) {
            if (on != scissor) {
                on ? GL_Enable(GL_SCISSOR_TEST) : GL_Disable(GL_SCISSOR_TEST);
                scissor = on;
                if (count)
                    scissor_toggles++;
            }
            if (on)
                GL_Scissor(x, H - y - h, w, h);
        };
        auto draw = [&](int i, float x, float y, float w, float h) {
            rect_matrix(m, x, y, w, h, W, H);
            GL_UniformMatrix4fv(v[i].matrix, 1, GL_FALSE, m);
            GL_UniformMatrix4fv(v[i].texMatrix, 1, GL_FALSE, ident);
            GL_DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            if (count)
                layers++;
        };

        // Repaints: 4 tiles a frame get a moving band.
        for (int i = 0; i < 4; i++) {
            const int t = (f * 4 + i) % kTiles;
            uint8_t *q = repaint.data();
            for (int y = 0; y < kTile; y++)
                memset(q + ((size_t)y * kTile + ((f * 8 + y) & (kTile - 1))) * 4, 255, 4);
            bind(t);
            GL_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kTile, kTile, GL_RGBA, GL_UNSIGNED_BYTE, q);
        }

        GL_ClearColor(1, 1, 1, 1);
        GL_Clear(GL_COLOR_BUFFER_BIT);
        GL_BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // premultiplied, as TextureMapper

        // Page tiles, scrolling 4 px a frame.
        const int scroll = (f * 4) % kTile, first = (f * 4) / kTile;
        use(0);
        set_blend(false);
        set_scissor(false, 0, 0, 0, 0);
        for (int row = 0; row < (H + kTile - 1) / kTile + 1; row++)
            for (int col = 0; col < (W + kTile - 1) / kTile; col++) {
                bind((((row + first) * 8) + col) % kTiles);
                draw(0, (float)col * kTile, (float)(row * kTile - scroll), kTile, kTile);
            }

        // Element layers.
        set_blend(true);
        for (int i = 0; i < 160; i++) {
            const int var = 1 + (i / 4) % 5;
            use(var);
            const float w = 120.0f + (i * 37) % 300, h = 80.0f + (i * 53) % 200;
            const float x = (float)((i * 211 + f * (1 + i % 3)) % (W + 200)) - 100;
            const float y = (float)((i * 97) % (H - 100));
            if (i % 4 == 3)
                set_scissor(true, (int)x + 10, (int)y + 10, (int)w / 2, (int)h);
            else
                set_scissor(false, 0, 0, 0, 0);
            const CompProgram &p = v[var];
            GL_Uniform1f(p.opacity, 0.55f + 0.4f * (float)((i + f) % 60) / 59.0f);
            if (var == 2)
                GL_Uniform4f(p.rect, w, h, 0, 1);
            if (var == 3)
                GL_Uniform4f(p.rect, w, h, 24, 1);
            if (var == 4)
                GL_UniformMatrix4fv(p.colorMatrix, 1, GL_FALSE, (i & 8) ? sepia : grey);
            if (var == 5)
                GL_Uniform4f(p.color, (float)(i % 7) / 6, 0.3f, (float)(i % 5) / 4, 0.8f);
            else
                bind((i * 7) % kTiles);
            draw(var, x, y, w, h);
        }

        // Small solid layers.
        set_scissor(false, 0, 0, 0, 0);
        use(5);
        GL_Uniform1f(v[5].opacity, 1.0f);
        for (int i = 0; i < 96; i++) {
            GL_Uniform4f(v[5].color, (float)(i % 3) / 2, (float)(i % 4) / 3, 1, 0.9f);
            draw(5, (float)((i * 173 + f * 2) % W), (float)((i * 71) % H), 24, 24);
        }

        // Two full-screen translucent overlays.
        use(1);
        GL_Uniform1f(v[1].opacity, 0.15f);
        bind(f / 30 % kTiles);
        draw(1, 0, 0, (float)W, (float)H);
        bind((f / 30 + 1) % kTiles);
        draw(1, 0, 0, (float)W, (float)H);
        set_blend(false);
    }
};

void test_compositor_offscreen()
{
    const int W = 1920, H = 1080, WARMUP = 3, FRAMES = 60;
    char log[512];
    Compositor c;
    if (!c.init(log, sizeof log)) {
        spike15_result("angle-compositor", 0, "init: %s", log);
        return;
    }
    Fbo fbo = make_fbo(W, H, true);
    if (GL_CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        spike15_result("angle-compositor", 0, "1080p FBO incomplete");
        return;
    }
    GL_Viewport(0, 0, W, H);
    GL_Finish();
    double first = 0, issue = 0, cpu = 0, fin = 0, worst = 0;
    for (int f = 0; f < WARMUP + FRAMES; f++) {
        const double w0 = spike15_now_ms(), c0 = spike15_cpu_ms();
        c.frame(f, W, H, f == 0);
        const double w1 = spike15_now_ms(), c1 = spike15_cpu_ms();
        GL_Finish();
        const double w2 = spike15_now_ms();
        if (f == 0)
            first = w2 - w0;
        if (f < WARMUP)
            continue;
        issue += w1 - w0;
        cpu += c1 - c0;
        fin += w2 - w1;
        worst = w2 - w0 > worst ? w2 - w0 : worst;
    }
    uint8_t px[4];
    pixel(W / 2, H / 2, px);
    GLenum e = err_of("compositor");
    spike15_log("  a frame: %d layers, %d program switches, %d texture binds, %d blend and %d scissor toggles, 4 tile "
                "uploads; 6 variants compiled + linked in %.1f ms",
                c.layers, c.prog_switches, c.binds, c.blend_toggles, c.scissor_toggles, c.compile_ms);
    spike15_result("angle-compositor", e == GL_NO_ERROR,
                   "1920x1080, %d frames: frame %.2f ms = issue %.2f (cpu %.2f) + finish %.2f, worst %.2f; %.1f us cpu a "
                   "layer; first frame %.1f ms (pipelines); centre %u,%u,%u",
                   FRAMES, (issue + fin) / FRAMES, issue / FRAMES, cpu / FRAMES, fin / FRAMES, worst,
                   cpu / FRAMES * 1e3 / c.layers, first, px[0], px[1], px[2]);
    GL_BindFramebuffer(GL_FRAMEBUFFER, 0);
    GL_DeleteFramebuffers(1, &fbo.fbo);
    GL_DeleteTextures(1, &fbo.tex);
    c.destroy();
}

// --- present on the TV (VK_KHR_display on VideoOut) --------------------------

struct FrameStats {
    int frames = 0, slow = 0;
    double first = 0, first_frame = 0, last = 0, worst = 0, issue = 0, issue_cpu = 0, swap = 0;
};

// Present `frames` frames, each drawn by `draw(f)`; interval = swap to swap.
// `first` is the first frame's draw + swap (pipelines built on first use).
void present_loop(EGLSurface win, int frames, const std::function<void(int)> &draw, FrameStats *st)
{
    double prev = 0;
    for (int f = 0; f < frames; f++) {
        const double w0 = spike15_now_ms(), c0 = spike15_cpu_ms();
        draw(f);
        const double w1 = spike15_now_ms(), c1 = spike15_cpu_ms();
        if (!EGL_SwapBuffers(g_dpy, win)) {
            spike15_log("  EGL_SwapBuffers failed at frame %d: 0x%04x", f, EGL_GetError());
            break;
        }
        const double w2 = spike15_now_ms();
        st->issue += w1 - w0;
        st->issue_cpu += c1 - c0;
        st->swap += w2 - w1;
        if (f == 0) {
            st->first = w2;
            st->first_frame = w2 - w0;
        } else {
            const double dt = w2 - prev;
            st->worst = dt > st->worst ? dt : st->worst;
            st->slow += dt > 20.0;
        }
        prev = st->last = w2;
        st->frames++;
    }
}

// One frame: a clear whose colour pulses, then `draws` small quads (2000 is
// Spike 11's draw-bench load), and a red square crossing the screen.
void present_frames(GLuint prog, EGLSurface win, int frames, int draws, FrameStats *st)
{
    GLint xf = GL_GetUniformLocation(prog, "xform"), col = GL_GetUniformLocation(prog, "color");
    present_loop(win, frames, [&](int f) {
        const float t = (float)(f % 120) / 119.0f;
        GL_ClearColor(0.1f, 0.2f + 0.6f * t, 0.3f, 1);
        GL_Clear(GL_COLOR_BUFFER_BIT);
        for (int d = 0; d < draws; d++) {
            const float a = (float)(d * 37 % 1000) / 500.0f - 1.0f, b = (float)(d * 91 % 1000) / 500.0f - 1.0f;
            GL_Uniform4f(xf, a, b, 0.02f, 0.02f);
            GL_Uniform4f(col, (float)(d & 255) / 255.0f, 0.5f, t, 1.0f);
            GL_DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        // A red square that crosses the screen once every 2 s: on the TV,
        // smooth motion means one frame a vblank.
        GL_Uniform4f(xf, -0.9f + 1.8f * (float)(f % 120) / 119.0f, 0.0f, 0.1f, 0.1f * 16.0f / 9.0f);
        GL_Uniform4f(col, 1, 0, 0, 1);
        GL_DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }, st);
}

void test_present(GLuint prog_unused)
{
    (void)prog_unused;
    // ANGLE's simple display reads this as its native window: the swapchain
    // size. RADV's WSI takes 1920x1080 and 3840x2160 as VideoOut buffers
    // directly (any other size is blitted into one).
    struct { uint16_t width, height; } window = {1920, 1080};
    // RADV's VideoOut WSI offers B8G8R8A8 surface formats only, and ANGLE
    // refuses (EGL_BAD_MATCH) a window surface whose config's format the
    // surface lacks, so the RGBA8 config used offscreen can't be it. Try the
    // 8888 window configs until one takes.
    const double t0 = spike15_now_ms();
    EGLConfig cfgs[64];
    EGLint ncfg = 0;
    EGL_GetConfigs(g_dpy, cfgs, 64, &ncfg);
    EGLSurface win = EGL_NO_SURFACE;
    EGLConfig wcfg = nullptr;
    EGLint lastErr = 0, tried = 0;
    for (EGLint i = 0; i < ncfg && win == EGL_NO_SURFACE; i++) {
        EGLint st = 0, r = 0, g = 0, b = 0, a = 0, id = 0;
        EGL_GetConfigAttrib(g_dpy, cfgs[i], EGL_SURFACE_TYPE, &st);
        EGL_GetConfigAttrib(g_dpy, cfgs[i], EGL_RED_SIZE, &r);
        EGL_GetConfigAttrib(g_dpy, cfgs[i], EGL_GREEN_SIZE, &g);
        EGL_GetConfigAttrib(g_dpy, cfgs[i], EGL_BLUE_SIZE, &b);
        EGL_GetConfigAttrib(g_dpy, cfgs[i], EGL_ALPHA_SIZE, &a);
        EGL_GetConfigAttrib(g_dpy, cfgs[i], EGL_CONFIG_ID, &id);
        if (!(st & EGL_WINDOW_BIT) || r != 8 || g != 8 || b != 8 || a != 8)
            continue;
        tried++;
        win = EGL_CreateWindowSurface(g_dpy, cfgs[i], (EGLNativeWindowType)&window, nullptr);
        if (win != EGL_NO_SURFACE) {
            wcfg = cfgs[i];
            spike15_log("  window surface on config id %d (%d 8888 window configs tried)", id, tried);
        } else
            lastErr = EGL_GetError();
    }
    if (win == EGL_NO_SURFACE) {
        spike15_result("angle-present", 0, "EGL_CreateWindowSurface: 0x%04x on all %d 8888 window configs of %d",
                       lastErr, tried, ncfg);
        return;
    }
    const EGLint ctxAttrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    EGLContext ctx = EGL_CreateContext(g_dpy, wcfg, EGL_NO_CONTEXT, ctxAttrs);
    if (ctx == EGL_NO_CONTEXT || !EGL_MakeCurrent(g_dpy, win, win, ctx)) {
        spike15_result("angle-present", 0, "context on the window: 0x%04x", EGL_GetError());
        EGL_DestroySurface(g_dpy, win);
        return;
    }
    EGL_SwapInterval(g_dpy, 1);
    EGLint w = 0, h = 0;
    EGL_QuerySurface(g_dpy, win, EGL_WIDTH, &w);
    EGL_QuerySurface(g_dpy, win, EGL_HEIGHT, &h);
    const double t1 = spike15_now_ms();
    spike15_log("  window surface %dx%d, surface + context %.1f ms", w, h, t1 - t0);

    char log[512];
    GLuint prog = program(kVs300, kFs300, log, sizeof log);
    if (!prog) {
        spike15_result("angle-present", 0, "shader: %s", log);
        return;
    }
    GLuint vao = 0, vbo = 0;
    GL_GenVertexArrays(1, &vao);
    GL_BindVertexArray(vao);
    GL_GenBuffers(1, &vbo);
    GL_BindBuffer(GL_ARRAY_BUFFER, vbo);
    GL_BufferData(GL_ARRAY_BUFFER, sizeof kStrip, kStrip, GL_STATIC_DRAW);
    GL_EnableVertexAttribArray(0);
    GL_VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    GL_UseProgram(prog);
    GL_Viewport(0, 0, w, h);

    struct Case {
        const char *name;
        int frames, draws;
    } cases[] = {{"angle-present", 600, 0}, {"angle-present-draws", 600, 2000}};
    for (const Case &c : cases) {
        FrameStats st;
        present_frames(prog, win, c.frames, c.draws, &st);
        GLenum e = err_of(c.name);
        const double span = st.last - st.first;
        const double fps = st.frames > 1 && span > 0 ? (st.frames - 1) * 1000.0 / span : 0;
        const int n = st.frames ? st.frames : 1;
        spike15_result(c.name, e == GL_NO_ERROR && st.frames == c.frames && fps > 55,
                       "%d frames, %d draws each: %.1f frames/s, worst interval %.1f ms, %d over 20 ms; "
                       "issue %.2f ms (cpu %.2f), swap %.2f ms a frame%s",
                       st.frames, c.draws, fps, st.worst, st.slow, st.issue / n, st.issue_cpu / n, st.swap / n,
                       c.draws ? "" : " (first swap included)");
        if (c.draws)
            spike15_log("  per draw while presenting: %.1f us (cpu %.1f)", st.issue * 1e3 / n / c.draws,
                        st.issue_cpu * 1e3 / n / c.draws);
    }
    GL_DeleteBuffers(1, &vbo);
    GL_DeleteVertexArrays(1, &vao);
    GL_DeleteProgram(prog);

    // The compositor frame on the TV: scrolling tiles under moving panels.
    {
        Compositor comp;
        if (!comp.init(log, sizeof log)) {
            spike15_result("angle-present-compositor", 0, "init: %s", log);
        } else {
            GL_Viewport(0, 0, w, h);
            FrameStats st;
            present_loop(win, 600, [&](int f) { comp.frame(f, w, h, f == 0); }, &st);
            GLenum e = err_of("present-compositor");
            const double span = st.last - st.first;
            const double fps = st.frames > 1 && span > 0 ? (st.frames - 1) * 1000.0 / span : 0;
            const int n = st.frames ? st.frames : 1;
            spike15_result("angle-present-compositor", e == GL_NO_ERROR && st.frames == 600 && fps > 55,
                           "%d frames of %d layers: %.1f frames/s, worst interval %.1f ms, %d over 20 ms; issue %.2f ms "
                           "(cpu %.2f), swap %.2f ms a frame; first frame %.1f ms",
                           st.frames, comp.layers, fps, st.worst, st.slow, st.issue / n, st.issue_cpu / n, st.swap / n,
                           st.first_frame);
            comp.destroy();
        }
    }
    EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    EGL_DestroySurface(g_dpy, win);
    EGL_DestroyContext(g_dpy, ctx);
}

}  // namespace

void angle_vk_tests_run(void)
{
    if (!test_display())
        return;
    EGLContext es3 = test_context_es3();
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
            test_compositor_offscreen();
        }
        EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        EGL_DestroyContext(g_dpy, es3);
    }
    test_context_es2();
    test_context_es31();
    if (!getenv("SPIKE15_NO_PRESENT"))
        test_present(0);
    EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    EGL_Terminate(g_dpy);
    spike15_log("angle tests done");
}
