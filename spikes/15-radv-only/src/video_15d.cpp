// video_15d.cpp - spike 15d: hardware-decoded video into RADV memory and to
// RGBA on ANGLE (ES 3.1 on Vulkan), in a title with no ps5-opengl.
//
// Today the browser's Videodec2 decodes into ps5-opengl GL buffers (their
// persistent mappings are GPU-visible direct memory, type 12, the decoder's
// own), and PS5Renderer converts each picture with a `#version 430 core`
// fragment shader reading the buffer as an SSBO (PS5VideoGL, Spike 9). The
// same on Route A:
//   - 8 ANGLE GL buffers (GL_EXT_buffer_storage, persistent + coherent
//     mapping) are the decoder's output slots. A PS5 RADV buffer is direct
//     memory type 12 mapped CPU+GPU read/write at one address, so nothing is
//     imported or copied;
//   - the browser's conversion shader as `#version 310 es` (fragment, SSBO),
//     and a compute variant writing an rgba8 image;
//   - each picture letterboxed on the TV (VK_KHR_display) for the owner.
//
// Per clip:
//   video-<clip>   pictures decoded into the GL buffers (every one inside the
//                  slot handed over), real content, both conversions match a
//                  CPU conversion of the decoder's bytes at 64 points, and
//                  times: CPU to issue the conversion (what the renderer's
//                  "copy+convert" stat counts), GPU time (timer queries) and
//                  wall time to glFinish, for the fragment and compute passes
//
// Clips are uploaded by run-15d.sh to /data/ps5webkit-spike/15d/ (Spike 9's,
// spikes/09-hw-decode/media). The decoder is Spike 9's vdec2.c / au.c.
#define EGL_NO_PLATFORM_SPECIFIC_TYPES 1
#define GL_GLES_PROTOTYPES 0
#define EGL_EGL_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>

#include "libGLESv2/entry_points_egl_autogen.h"
#include "libGLESv2/entry_points_egl_ext_autogen.h"
#include "libGLESv2/entry_points_gles_2_0_autogen.h"
#include "libGLESv2/entry_points_gles_3_0_autogen.h"
#include "libGLESv2/entry_points_gles_3_1_autogen.h"
#include "libGLESv2/entry_points_gles_ext_autogen.h"

extern "C" {
#include "09-hw-decode/au.h"
#include "09-hw-decode/vdec2.h"
}

#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
void spike15_log(const char *fmt, ...);
void spike15_result(const char *test, int pass, const char *fmt, ...);
double spike15_now_ms(void);
double spike15_cpu_ms(void);
void video_tests_run(int load_rc);
}

namespace {

constexpr int kSlots = 8;
constexpr const char *kDir = "/data/ps5webkit-spike/15d";

struct Clip {
    const char *file, *kind, *codec;
    int profile, width, height;
    double seconds;
};
const Clip kClips[] = {
    {"h264_1080p.mp4", "mp4", "h264", 100, 1920, 1080, 10},
    {"hevc_4k.mp4", "mp4", "hevc", 1, 3840, 2160, 10},
    {"vp9p2_4k.ivf", "ivf", "vp9", 2, 3840, 2160, 10},
};

EGLDisplay g_dpy = EGL_NO_DISPLAY;
EGLSurface g_win = EGL_NO_SURFACE;
EGLint g_winW = 0, g_winH = 0;
bool g_timer = false;

// The browser's kConvertFragmentShader (PS5Renderer.cpp), as ES 3.1. format_
// 2 = NV12, 4 = NV12 layout with 16-bit samples, 10 bits LSB-aligned
// (Videodec2's 10-bit); color_ bit 0 BT.709, bit 1 full range.
#define CONVERT_BODY                                                                   \
    "layout(std430, binding = 0) readonly buffer Frame { uint d[]; };\n"              \
    "uniform int format_;\n"                                                           \
    "uniform ivec3 strides;\n"                                                         \
    "uniform ivec3 offsets;\n"                                                         \
    "uniform int color_;\n"                                                            \
    "float b8(int a) { return float((d[a >> 2] >> ((a & 3) * 8)) & 0xffu) / 255.0; }\n"   \
    "float b16(int a) { return float((d[a >> 2] >> ((a & 2) * 8)) & 0xffffu) / 1023.0; }\n" \
    "vec4 convert(ivec2 p) {\n"                                                        \
    "  int cy = p.y >> 1, cx = p.x >> 1;\n"                                            \
    "  float y, u, v;\n"                                                               \
    "  if (format_ == 4) {\n"                                                          \
    "    y = b16(offsets.x + p.y * strides.x + p.x * 2);\n"                            \
    "    int c = offsets.y + cy * strides.y + cx * 4;\n"                               \
    "    u = b16(c); v = b16(c + 2);\n"                                                \
    "  } else {\n"                                                                     \
    "    y = b8(offsets.x + p.y * strides.x + p.x);\n"                                 \
    "    int c = offsets.y + cy * strides.y + cx * 2;\n"                               \
    "    u = b8(c); v = b8(c + 1);\n"                                                  \
    "  }\n"                                                                            \
    "  if ((color_ & 2) == 0) {\n"                                                     \
    "    y = (y - 16.0 / 255.0) * (255.0 / 219.0);\n"                                  \
    "    u = (u - 0.5) * (255.0 / 224.0); v = (v - 0.5) * (255.0 / 224.0);\n"          \
    "  } else { u -= 0.5; v -= 0.5; }\n"                                               \
    "  vec3 rgb = (color_ & 1) != 0\n"                                                 \
    "    ? vec3(y + 1.5748 * v, y - 0.1873 * u - 0.4681 * v, y + 1.8556 * u)\n"        \
    "    : vec3(y + 1.402 * v, y - 0.344136 * u - 0.714136 * v, y + 1.772 * u);\n"     \
    "  return vec4(clamp(rgb, 0.0, 1.0), 1.0);\n"                                      \
    "}\n"

const char *kVsFull =
    "#version 310 es\n"
    "out vec2 uv;\n"
    "void main() { vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1) * 4.0 - 1.0;\n"
    "  uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";
const char *kFsConvert =
    "#version 310 es\n"
    "precision highp float;\n"
    "precision highp int;\n" CONVERT_BODY
    "out vec4 o;\n"
    "void main() { o = convert(ivec2(gl_FragCoord.xy)); }\n";
const char *kCsConvert =
    "#version 310 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n" CONVERT_BODY
    "layout(rgba8, binding = 0) writeonly uniform highp image2D dst;\n"
    "uniform ivec2 size;\n"
    "void main() { ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
    "  if (p.x < size.x && p.y < size.y) imageStore(dst, p, convert(p)); }\n";
// Texture row 0 is the picture's top row; the window's GL y 0 is the bottom.
const char *kFsShow =
    "#version 310 es\n"
    "precision mediump float;\n"
    "uniform sampler2D t;\n"
    "in vec2 uv;\n"
    "out vec4 o;\n"
    "void main() { o = texture(t, vec2(uv.x, 1.0 - uv.y)); }\n";

GLuint compile(GLenum type, const char *src, char *log, size_t n)
{
    GLuint s = GL_CreateShader(type);
    GL_ShaderSource(s, 1, &src, nullptr);
    GL_CompileShader(s);
    GLint ok = 0;
    GL_GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GL_GetShaderInfoLog(s, (GLsizei)n, nullptr, log);
        return 0;
    }
    return s;
}

GLuint link(GLuint a, GLuint b, char *log, size_t n)
{
    GLuint p = GL_CreateProgram();
    GL_AttachShader(p, a);
    if (b)
        GL_AttachShader(p, b);
    GL_LinkProgram(p);
    GLint ok = 0;
    GL_GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GL_GetProgramInfoLog(p, (GLsizei)n, nullptr, log);
        return 0;
    }
    return p;
}

struct Programs {
    GLuint convert = 0, compute = 0, show = 0, vao = 0;
    GLint fFormat, fStrides, fOffsets, fColor, cFormat, cStrides, cOffsets, cColor, cSize;
} P;

bool make_programs(char *log, size_t n)
{
    log[0] = 0;
    GLuint vs = compile(GL_VERTEX_SHADER, kVsFull, log, n);
    GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, kFsConvert, log, n) : 0;
    GLuint show = fs ? compile(GL_FRAGMENT_SHADER, kFsShow, log, n) : 0;
    GLuint cs = show ? compile(GL_COMPUTE_SHADER, kCsConvert, log, n) : 0;
    if (!cs)
        return false;
    P.convert = link(vs, fs, log, n);
    P.show = P.convert ? link(vs, show, log, n) : 0;
    P.compute = P.show ? link(cs, 0, log, n) : 0;
    if (!P.compute)
        return false;
    P.fFormat = GL_GetUniformLocation(P.convert, "format_");
    P.fStrides = GL_GetUniformLocation(P.convert, "strides");
    P.fOffsets = GL_GetUniformLocation(P.convert, "offsets");
    P.fColor = GL_GetUniformLocation(P.convert, "color_");
    P.cFormat = GL_GetUniformLocation(P.compute, "format_");
    P.cStrides = GL_GetUniformLocation(P.compute, "strides");
    P.cOffsets = GL_GetUniformLocation(P.compute, "offsets");
    P.cColor = GL_GetUniformLocation(P.compute, "color_");
    P.cSize = GL_GetUniformLocation(P.compute, "size");
    GL_GenVertexArrays(1, &P.vao);
    return true;
}

// The window on the TV and an ES 3.1 context on it (15a: VideoOut's surface
// formats are BGRA8 only, so try the 8888 window configs).
bool gl_up()
{
    const EGLAttrib attrs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE,
        EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE, EGL_PLATFORM_VULKAN_DISPLAY_MODE_SIMPLE_ANGLE,
        EGL_NONE,
    };
    g_dpy = EGL_GetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, attrs);
    if (g_dpy == EGL_NO_DISPLAY || !EGL_Initialize(g_dpy, nullptr, nullptr)) {
        spike15_result("video-gl", 0, "ANGLE display 0x%04x", EGL_GetError());
        return false;
    }
    static struct { uint16_t width, height; } window = {1920, 1080};
    EGLConfig cfgs[64];
    EGLint ncfg = 0;
    EGL_GetConfigs(g_dpy, cfgs, 64, &ncfg);
    EGLConfig wcfg = nullptr;
    for (EGLint i = 0; i < ncfg && g_win == EGL_NO_SURFACE; i++) {
        EGLint st = 0, r = 0, g = 0, b = 0, a = 0;
        EGL_GetConfigAttrib(g_dpy, cfgs[i], EGL_SURFACE_TYPE, &st);
        EGL_GetConfigAttrib(g_dpy, cfgs[i], EGL_RED_SIZE, &r);
        EGL_GetConfigAttrib(g_dpy, cfgs[i], EGL_GREEN_SIZE, &g);
        EGL_GetConfigAttrib(g_dpy, cfgs[i], EGL_BLUE_SIZE, &b);
        EGL_GetConfigAttrib(g_dpy, cfgs[i], EGL_ALPHA_SIZE, &a);
        if (!(st & EGL_WINDOW_BIT) || r != 8 || g != 8 || b != 8 || a != 8)
            continue;
        g_win = EGL_CreateWindowSurface(g_dpy, cfgs[i], (EGLNativeWindowType)&window, nullptr);
        if (g_win != EGL_NO_SURFACE)
            wcfg = cfgs[i];
    }
    const EGLint ctxAttrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    EGLContext ctx = g_win != EGL_NO_SURFACE ? EGL_CreateContext(g_dpy, wcfg, EGL_NO_CONTEXT, ctxAttrs) : EGL_NO_CONTEXT;
    if (ctx == EGL_NO_CONTEXT || !EGL_MakeCurrent(g_dpy, g_win, g_win, ctx)) {
        spike15_result("video-gl", 0, "window + ES 3.1 context: 0x%04x", EGL_GetError());
        return false;
    }
    EGL_SwapInterval(g_dpy, 1);
    EGL_QuerySurface(g_dpy, g_win, EGL_WIDTH, &g_winW);
    EGL_QuerySurface(g_dpy, g_win, EGL_HEIGHT, &g_winH);
    const char *ext = (const char *)GL_GetString(GL_EXTENSIONS);
    const bool storage = ext && strstr(ext, "GL_EXT_buffer_storage");
    g_timer = ext && strstr(ext, "GL_EXT_disjoint_timer_query");
    GLint maxSsbo = 0, fragBlocks = 0, ssboAlign = 0;
    GL_GetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maxSsbo);
    GL_GetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, &fragBlocks);
    GL_GetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &ssboAlign);
    char log[1024];
    const bool progs = storage && make_programs(log, sizeof log);
    spike15_result("video-gl", storage && progs && fragBlocks > 0,
                   "window %dx%d, GL \"%s\"; EXT_buffer_storage %d, EXT_disjoint_timer_query %d; max SSBO %d MB, "
                   "fragment SSBO blocks %d, offset alignment %d; programs %s",
                   g_winW, g_winH, GL_GetString(GL_VERSION), storage, g_timer, maxSsbo >> 20, fragBlocks, ssboAlign,
                   progs ? "linked" : log);
    return storage && progs;
}

uint8_t *load_file(const char *path, size_t *size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return nullptr;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return nullptr;
    }
    // Direct memory, as Spike 9 (the title has little flexible memory);
    // one buffer, grown for a bigger clip.
    static uint8_t *buf;
    static size_t cap;
    if ((size_t)st.st_size > cap) {
        cap = ((size_t)st.st_size + 0xffff) & ~(size_t)0xffff;
        buf = (uint8_t *)vdec2_direct_alloc(cap, 0x10000);
    }
    size_t got = 0;
    while (buf && got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    *size = got;
    return buf && got == (size_t)st.st_size ? buf : nullptr;
}

// The shader's arithmetic on the CPU, for one pixel (BT.709 limited range).
void cpu_convert(const uint8_t *frame, int fmt, int pitch, int uvOff, int x, int y, uint8_t out[3])
{
    auto u16 = [&](size_t a) { return (float)((uint32_t)frame[a] | ((uint32_t)frame[a + 1] << 8)) / 1023.0f; };
    float Y, U, V;
    if (fmt == 4) {
        Y = u16((size_t)y * pitch + (size_t)x * 2);
        const size_t c = (size_t)uvOff + (size_t)(y >> 1) * pitch + (size_t)(x >> 1) * 4;
        U = u16(c);
        V = u16(c + 2);
    } else {
        Y = frame[(size_t)y * pitch + x] / 255.0f;
        const size_t c = (size_t)uvOff + (size_t)(y >> 1) * pitch + (size_t)(x >> 1) * 2;
        U = frame[c] / 255.0f;
        V = frame[c + 1] / 255.0f;
    }
    Y = (Y - 16.0f / 255.0f) * (255.0f / 219.0f);
    U = (U - 0.5f) * (255.0f / 224.0f);
    V = (V - 0.5f) * (255.0f / 224.0f);
    const float rgb[3] = {Y + 1.5748f * V, Y - 0.1873f * U - 0.4681f * V, Y + 1.8556f * U};
    for (int i = 0; i < 3; i++) {
        const float c = rgb[i] < 0 ? 0 : rgb[i] > 1 ? 1 : rgb[i];
        out[i] = (uint8_t)lrintf(c * 255.0f);
    }
}

struct Run {
    const Clip *clip;
    int fmt = 2;
    GLuint buf[kSlots] = {};
    uint8_t *map[kSlots] = {};
    size_t slotOff[kSlots] = {}, slotBytes = 0;
    GLuint fragTex = 0, fragFbo = 0, compTex = 0, compFbo = 0, query[2] = {};
    // stats
    unsigned pictures = 0, outside = 0, timed = 0, checked = 0, flat = 0;
    double issueCpu = 0, issueWall = 0, fragGpu = 0, fragWall = 0, compGpu = 0, compWall = 0, present = 0;
    int worstFrag = 0, worstComp = 0;  // largest channel difference against the CPU
    unsigned yMin = 255, yMax = 0;
};

uint64_t query_ns(GLuint q)
{
    GLuint64 ns = 0;
    GL_GetQueryObjectui64vEXT(q, GL_QUERY_RESULT, &ns);
    return ns;
}

// Check 64 points of `fbo` (W x H, row 0 = picture top) against the CPU.
int check_points(Run *r, GLuint fbo, const uint8_t *frame, int pitch, int uvOff)
{
    const int W = r->clip->width, H = r->clip->height;
    GL_BindFramebuffer(GL_FRAMEBUFFER, fbo);
    int worst = 0;
    for (int i = 0; i < 64; i++) {
        const int x = (int)((i % 8) * (W - 1) / 7 + (i * 37) % 13) % W;
        const int y = (int)((i / 8) * (H - 1) / 7 + (i * 53) % 11) % H;
        uint8_t got[4] = {}, want[3];
        GL_ReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
        cpu_convert(frame, r->fmt, pitch, uvOff, x, y, want);
        for (int c = 0; c < 3; c++) {
            const int d = abs((int)got[c] - (int)want[c]);
            worst = d > worst ? d : worst;
        }
    }
    return worst;
}

void picture(Run *r, const vdec2_picture_t *p)
{
    const Clip *c = r->clip;
    const int s = p->slot;
    const uint8_t *base = r->map[s] + r->slotOff[s];
    const size_t off = (size_t)((const uint8_t *)p->buffer - base);
    if (s < 0 || s >= kSlots || (const uint8_t *)p->buffer < base || off >= r->slotBytes) {
        if (!r->outside++)
            spike15_log("  picture %u at %p is not in GL slot %d", r->pictures, p->buffer, s);
        return;
    }
    const int pitch = (int)p->pitch_bytes;
    const int y0 = (int)(r->slotOff[s] + off), uv0 = y0 + pitch * (int)p->height;
    const bool timed = r->pictures % 10 == 5;
    const bool check = r->pictures == 35 || r->pictures == 305;  // timed ones

    // The fragment pass, as PS5Renderer issues it.
    const double w0 = spike15_now_ms(), c0 = spike15_cpu_ms();
    if (timed && g_timer)
        GL_BeginQuery(GL_TIME_ELAPSED_EXT, r->query[0]);
    GL_BindFramebuffer(GL_FRAMEBUFFER, r->fragFbo);
    GL_Viewport(0, 0, c->width, c->height);
    GL_UseProgram(P.convert);
    GL_BindVertexArray(P.vao);
    GL_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, r->buf[s]);
    GL_Uniform1i(P.fFormat, r->fmt);
    GL_Uniform3i(P.fStrides, pitch, pitch, 0);
    GL_Uniform3i(P.fOffsets, y0, uv0, 0);
    GL_Uniform1i(P.fColor, 1);
    GL_DrawArrays(GL_TRIANGLES, 0, 3);
    if (timed && g_timer)
        GL_EndQuery(GL_TIME_ELAPSED_EXT);
    const double w1 = spike15_now_ms(), c1 = spike15_cpu_ms();
    r->issueWall += w1 - w0;
    r->issueCpu += c1 - c0;
    if (timed) {
        GL_Finish();
        r->fragWall += spike15_now_ms() - w0;
        if (g_timer)
            r->fragGpu += query_ns(r->query[0]) / 1e6;
        // The compute pass on the same picture.
        const double k0 = spike15_now_ms();
        if (g_timer)
            GL_BeginQuery(GL_TIME_ELAPSED_EXT, r->query[1]);
        GL_UseProgram(P.compute);
        GL_Uniform1i(P.cFormat, r->fmt);
        GL_Uniform3i(P.cStrides, pitch, pitch, 0);
        GL_Uniform3i(P.cOffsets, y0, uv0, 0);
        GL_Uniform1i(P.cColor, 1);
        GL_Uniform2i(P.cSize, c->width, c->height);
        GL_BindImageTexture(0, r->compTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        GL_DispatchCompute((GLuint)(c->width + 7) / 8, (GLuint)(c->height + 7) / 8, 1);
        GL_MemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT | GL_PIXEL_BUFFER_BARRIER_BIT);
        if (g_timer)
            GL_EndQuery(GL_TIME_ELAPSED_EXT);
        GL_Finish();
        r->compWall += spike15_now_ms() - k0;
        if (g_timer)
            r->compGpu += query_ns(r->query[1]) / 1e6;
        r->timed++;
        if (check) {
            const uint8_t *frame = r->map[s] + y0;
            const int fd = check_points(r, r->fragFbo, frame, pitch, uv0 - y0);
            const int cd = check_points(r, r->compFbo, frame, pitch, uv0 - y0);
            r->worstFrag = fd > r->worstFrag ? fd : r->worstFrag;
            r->worstComp = cd > r->worstComp ? cd : r->worstComp;
            r->checked++;
            // Real content: the luma range over a grid.
            unsigned lo = 255, hi = 0;
            for (int yy = 0; yy < c->height; yy += c->height / 16)
                for (int xx = 0; xx < c->width; xx += c->width / 16) {
                    const unsigned v = r->fmt == 4 ? (frame[(size_t)yy * pitch + xx * 2] |
                                                      (frame[(size_t)yy * pitch + xx * 2 + 1] << 8)) >> 2
                                                   : frame[(size_t)yy * pitch + xx];
                    lo = v < lo ? v : lo;
                    hi = v > hi ? v : hi;
                }
            r->yMin = lo < r->yMin ? lo : r->yMin;
            r->yMax = hi > r->yMax ? hi : r->yMax;
            r->flat += hi - lo < 32;
            spike15_log("  picture %u: slot %d, pitch %d, coded height %u; vs CPU: fragment %d, compute %d (max "
                        "channel difference); luma %u..%u",
                        r->pictures, s, pitch, p->height, fd, cd, lo, hi);
        }
    }

    // Letterboxed onto the TV.
    const double s0 = spike15_now_ms();
    GL_BindFramebuffer(GL_FRAMEBUFFER, 0);
    GL_Viewport(0, 0, g_winW, g_winH);
    GL_ClearColor(0, 0, 0, 1);
    GL_Clear(GL_COLOR_BUFFER_BIT);
    int vw = g_winW, vh = g_winW * c->height / c->width;
    if (vh > g_winH) {
        vh = g_winH;
        vw = g_winH * c->width / c->height;
    }
    GL_Viewport((g_winW - vw) / 2, (g_winH - vh) / 2, vw, vh);
    GL_UseProgram(P.show);
    GL_BindTexture(GL_TEXTURE_2D, r->fragTex);
    GL_DrawArrays(GL_TRIANGLES, 0, 3);
    EGL_SwapBuffers(g_dpy, g_win);
    r->present += spike15_now_ms() - s0;
    r->pictures++;
}

GLuint rgba_target(int w, int h, GLuint *fbo)
{
    GLuint t = 0;
    GL_GenTextures(1, &t);
    GL_BindTexture(GL_TEXTURE_2D, t);
    GL_TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GL_GenFramebuffers(1, fbo);
    GL_BindFramebuffer(GL_FRAMEBUFFER, *fbo);
    GL_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    return t;
}

uint32_t codec_tag(const char *c)
{
    return !strcmp(c, "h264") ? VDEC2_CODEC_AVC : !strcmp(c, "hevc") ? VDEC2_CODEC_HEVC : VDEC2_CODEC_VP9;
}

void run_clip(const Clip *clip)
{
    char name[64], path[160];
    snprintf(name, sizeof name, "video-%s", clip->file);
    snprintf(path, sizeof path, "%s/%s", kDir, clip->file);
    size_t size = 0;
    const uint8_t *data = load_file(path, &size);
    static au_src_t src;
    if (!data || au_open(&src, data, size, clip->kind) != 0) {
        spike15_result(name, 0, "can't read %s (%zu bytes)", path, size);
        return;
    }
    // The decoder's frame size, from a throwaway bring-up (as Spike 9).
    vdec2_config_t vc = {};
    vc.codec = codec_tag(clip->codec);
    vc.profile = (uint32_t)clip->profile;
    vc.width = clip->width;
    vc.height = clip->height;
    vc.frame_slots = 1;
    vdec2_info_t info;
    const char *stage = "";
    int rc = 0;
    vdec2_t *v = vdec2_open(&vc, &info, &stage, &rc);
    if (!v) {
        spike15_result(name, 0, "decoder probe failed at %s rc=0x%08x", stage, (unsigned)rc);
        return;
    }
    vdec2_close(v);

    Run r;
    r.clip = clip;
    r.fmt = clip->profile == 2 ? 4 : 2;
    const size_t align = info.frame_alignment > 0x10000 ? info.frame_alignment : 0x10000;
    r.slotBytes = ((size_t)info.frame_bytes + 0x3fff) & ~(size_t)0x3fff;
    void *slots[kSlots];
    const GLbitfield fl = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;
    GL_GenBuffers(kSlots, r.buf);
    for (int i = 0; i < kSlots; i++) {
        GL_BindBuffer(GL_SHADER_STORAGE_BUFFER, r.buf[i]);
        GL_BufferStorageEXT(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(r.slotBytes + align), nullptr, fl);
        r.map[i] = (uint8_t *)GL_MapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)(r.slotBytes + align), fl);
        if (!r.map[i]) {
            spike15_result(name, 0, "GL buffer %d: persistent map failed (GL error 0x%x)", i, GL_GetError());
            return;
        }
        const uintptr_t a = (uintptr_t)r.map[i];
        r.slotOff[i] = ((a + align - 1) & ~(uintptr_t)(align - 1)) - a;
        slots[i] = r.map[i] + r.slotOff[i];
    }
    spike15_log("  %s: decoder frame %llu KB (alignment 0x%x); 8 ANGLE GL buffers of %zu KB, the first mapped at %p "
                "(+%zu to align)",
                clip->file, (unsigned long long)info.frame_bytes >> 10, info.frame_alignment,
                (r.slotBytes + align) >> 10, (void *)r.map[0], r.slotOff[0]);
    vc.frame_slots = kSlots;
    vc.external_slots = slots;
    v = vdec2_open(&vc, &info, &stage, &rc);
    if (!v) {
        spike15_result(name, 0, "decoder with ANGLE's buffers as its slots failed at %s rc=0x%08x", stage,
                       (unsigned)rc);
        return;
    }
    r.fragTex = rgba_target(clip->width, clip->height, &r.fragFbo);
    r.compTex = rgba_target(clip->width, clip->height, &r.compFbo);
    if (g_timer)
        GL_GenQueries(2, r.query);

    unsigned aus = 0, errors = 0;
    int firstRc = 0;
    uint64_t pts = 0;
    const double t0 = spike15_now_ms();
    while (spike15_now_ms() - t0 < clip->seconds * 1000.0) {
        int key = 0, hidden = 0;
        uint8_t *slot = (uint8_t *)vdec2_input_slot(v);
        const long n = au_next(&src, slot, VDEC2_INPUT_SLOT_BYTES, &key, &hidden);
        if (n < 0)
            break;
        if (n == 0) {
            au_rewind(&src);
            continue;
        }
        vdec2_picture_t pic;
        rc = vdec2_decode(v, slot, (size_t)n, pts++, &pic);
        aus++;
        if (rc != 0) {
            if (!errors++)
                firstRc = rc;
            if (errors > 64 && !r.pictures)
                break;
            continue;
        }
        if (pic.valid && !hidden)
            picture(&r, &pic);
    }
    const double secs = (spike15_now_ms() - t0) / 1000.0;
    vdec2_close(v);
    GL_Finish();
    const GLenum e = GL_GetError();
    const unsigned n = r.pictures ? r.pictures : 1, t = r.timed ? r.timed : 1;
    const bool ok = r.pictures > 0 && !r.outside && !errors && e == GL_NO_ERROR && r.checked > 0 && !r.flat &&
                    r.worstFrag <= 2 && r.worstComp <= 2;
    spike15_result(name, ok,
                   "%dx%d %s%s: %u pictures in %.1f s (%.1f/s, on the TV at vsync), %u AUs, %u rc errors (first "
                   "0x%x), %u outside the GL slots; vs CPU at 64 points on %u pictures: fragment %d, compute %d; "
                   "fragment: issue %.3f ms (cpu %.3f), GPU %.3f ms, to glFinish %.2f ms; compute: GPU %.3f ms, to "
                   "glFinish %.2f ms (%u timed); present %.2f ms; GL error 0x%x",
                   clip->width, clip->height, clip->codec, r.fmt == 4 ? " 10-bit" : "", r.pictures, secs,
                   r.pictures / secs, aus, errors, (unsigned)firstRc, r.outside, r.checked, r.worstFrag, r.worstComp,
                   r.issueWall / n, r.issueCpu / n, r.fragGpu / t, r.fragWall / t, r.compGpu / t, r.compWall / t,
                   r.timed, r.present / n, e);
    GL_DeleteBuffers(kSlots, r.buf);
    GL_DeleteTextures(1, &r.fragTex);
    GL_DeleteTextures(1, &r.compTex);
    GL_DeleteFramebuffers(1, &r.fragFbo);
    GL_DeleteFramebuffers(1, &r.compFbo);
    if (g_timer)
        GL_DeleteQueries(2, r.query);
}

}  // namespace

void video_tests_run(int load_rc)
{
    vdec2_set_logger([](const char *line) { spike15_log("  vdec2: %s", line); });
    spike15_log("vdec2: %s", vdec2_load_report());
    if (load_rc != 0) {
        spike15_result("video-load", 0, "libSceVideodec2 not bound (0x%08x)", (unsigned)load_rc);
        return;
    }
    if (!gl_up())
        return;
    for (const Clip &c : kClips)
        run_clip(&c);
    EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    EGL_Terminate(g_dpy);
    spike15_log("video tests done");
}
