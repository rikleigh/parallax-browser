/* glprobe_gpu.c - P0d: the groups that link programs, draw and dispatch.
 * Console only: in the Mac harness the PS5 shader compiler and AGC are
 * aborting stubs, so each of these ends as SKIP host there.
 *
 * The GL calls are the ones Dawn's GLES backend issues (CommandBufferGL.cpp,
 * TextureGL.cpp, BufferGL.cpp at bc34d95b), on small 32x32 targets, each
 * checked by reading the result back.
 */
#include "glp.h"

#include <math.h>
#include <stdlib.h>

/* ---- basics ------------------------------------------------------------- */

#define VS_BODY                                                               \
    "layout(location = 0) in vec2 pos;\n"                                     \
    "uniform vec4 xform;\nuniform float z;\n"                                 \
    "void main() { gl_Position = vec4(pos * xform.xy + xform.zw, z, 1.0); }\n"
static const char kVs310[] = "#version 310 es\n" VS_BODY;
static const char kVs320[] = "#version 320 es\n" VS_BODY;
static const char kFsSolid[] =
    "#version 310 es\nprecision highp float;\nuniform vec4 color;\nout vec4 o;\nvoid main() { o = color; }\n";
/* Texel (x, y) of the texture on unit 0 at pixel (x, y), wrapped. */
static const char kFsTex[] =
    "#version 310 es\nprecision highp float;\nuniform highp sampler2D t;\nout vec4 o;\n"
    "void main() { o = texelFetch(t, ivec2(gl_FragCoord.xy) % textureSize(t, 0), 0); }\n";
static const char kFsUtex[] =
    "#version 310 es\nprecision highp float;\nuniform highp usampler2D t;\nout vec4 o;\n"
    "void main() { o = vec4(float(texelFetch(t, ivec2(gl_FragCoord.xy), 0).r) / 255.0, 0.0, 0.0, 1.0); }\n";

static GLuint g_solid, g_tex, g_utex, g_quad_vao, g_quad_vbo, g_half_vao, g_half_vbo;
static char g_log[300];

static void set_xform(GLuint prog, float sx, float sy, float ox, float oy, float z)
{
    glUseProgram(prog);
    glUniform4f(glGetUniformLocation(prog, "xform"), sx, sy, ox, oy);
    glUniform1f(glGetUniformLocation(prog, "z"), z);
}

static int basics(void)
{
    if (g_solid)
        return 1;
    g_solid = mk_program(kVs310, kFsSolid, g_log, sizeof g_log);
    if (!g_solid)
        return 0;
    g_tex = mk_program(kVs310, kFsTex, g_log, sizeof g_log);
    g_utex = g_tex ? mk_program(kVs310, kFsUtex, g_log, sizeof g_log) : 0;
    if (!g_tex || !g_utex) {
        g_solid = 0;
        return 0;
    }
    static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
    /* Vertices 0-5: the left half; 6-11: the right half. */
    static const GLfloat halves[] = {-1, -1, 0, -1, -1, 1, -1, 1, 0, -1, 0, 1,
                                     0,  -1, 1, -1, 0,  1, 0,  1, 1, -1, 1, 1};
    glGenVertexArrays(1, &g_quad_vao);
    glBindVertexArray(g_quad_vao);
    glGenBuffers(1, &g_quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glGenVertexArrays(1, &g_half_vao);
    glBindVertexArray(g_half_vao);
    glGenBuffers(1, &g_half_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_half_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof halves, halves, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return glerr() == GL_NO_ERROR;
}

#define NEED_BASICS(first)                                                    \
    do {                                                                      \
        if (!basics()) {                                                      \
            R(first, 0, "the basic programs did not build: %s", g_log);       \
            return;                                                           \
        }                                                                     \
    } while (0)

typedef struct { GLuint fbo, tex; GLenum status; } target;

static void use_target(const target *t)
{
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, W, H);
}

static GLuint mk_tex(GLenum fmt, int w, int h)
{
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, fmt, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

static GLuint mk_fbo(GLuint tex)
{
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    return fbo;
}

static target mk_target(GLenum fmt)
{
    target t;
    t.tex = mk_tex(fmt, W, H);
    glBindTexture(GL_TEXTURE_2D, 0);
    t.fbo = mk_fbo(t.tex);
    t.status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glViewport(0, 0, W, H);
    return t;
}

static void rm_target(target *t)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &t->fbo);
    glDeleteTextures(1, &t->tex);
}

static void clearc(float r, float g, float b, float a)
{
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
}

/* The unit quad scaled and moved, in one colour. */
static void solid(float sx, float sy, float ox, float oy, float z, float r, float g, float b, float a)
{
    set_xform(g_solid, sx, sy, ox, oy, z);
    glUniform4f(glGetUniformLocation(g_solid, "color"), r, g, b, a);
    glBindVertexArray(g_quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* The texture on unit 0 over the whole target. */
static void blit_tex(GLuint prog, GLenum target_kind, GLuint tex)
{
    set_xform(prog, 1, 1, 0, 0, 0);
    glUniform1i(glGetUniformLocation(prog, "t"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(target_kind, tex);
    glBindVertexArray(g_quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void rd(int x, int y, uint8_t p[4])
{
    p[0] = p[1] = p[2] = p[3] = 0x5b; /* not a colour any test expects */
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
}

static int near4(const uint8_t *p, int r, int g, int b, int a, int tol)
{
    return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol && abs(p[3] - a) <= tol;
}

/* The two pixels most tests look at. */
#define LX 8
#define RX 24
#define MY 16

static const uint32_t *map_ro(GLenum target, size_t bytes)
{
    return (const uint32_t *)glMapBufferRange(target, 0, (GLsizeiptr)bytes, GL_MAP_READ_BIT);
}

static GLuint mk_buffer(GLenum target, size_t bytes, const void *data, GLenum usage)
{
    GLuint b = 0;
    glGenBuffers(1, &b);
    glBindBuffer(target, b);
    glBufferData(target, (GLsizeiptr)bytes, data, usage);
    return b;
}

/* ---- sanity ------------------------------------------------------------- */

static void g_sanity(void)
{
    NEED_BASICS("sanity-draw");
    target t = mk_target(GL_RGBA8);
    uint8_t l[4], r[4];
    clearc(0, 0, 1, 1);
    solid(0.5f, 1, -0.5f, 0, 0, 1, 0, 0, 1);
    rd(LX, MY, l);
    rd(RX, MY, r);
    const GLenum e = glerr();
    R("sanity-draw", t.status == GL_FRAMEBUFFER_COMPLETE && near4(l, 255, 0, 0, 255, 0) && near4(r, 0, 0, 255, 255, 0) && !e,
      "clear + red left half into an RGBA8 texture FBO: left %d,%d,%d,%d right %d,%d,%d,%d, fbo 0x%04x, GL error %s",
      l[0], l[1], l[2], l[3], r[0], r[1], r[2], r[3], t.status, errname(e));
    rm_target(&t);
}

/* ---- compute: storage buffers ------------------------------------------- */

#define N 1024u
#define GUARD 16u

static const char kCsFill[] =
    "#version 310 es\n"
    "layout(local_size_x = 64) in;\n"
    "layout(std430, binding = 0) buffer Out { uint data[]; } o;\n"
    "void main() { uint i = gl_GlobalInvocationID.x; if (i < 1024u) o.data[i] = i * 3u + 7u; }\n";

static const char kCsUbo[] =
    "#version 310 es\n"
    "layout(local_size_x = 64) in;\n"
    "layout(std140, binding = 2) uniform Params { uint mul; uint add; vec4 pad; uint count; } p;\n"
    "layout(std430, binding = 0) buffer Out { uint data[]; } o;\n"
    "layout(std430, binding = 1) buffer Info { uint len; uint count; } info;\n"
    "void main() { uint i = gl_GlobalInvocationID.x;\n"
    "  if (i == 0u) { info.len = uint(o.data.length()); info.count = p.count; }\n"
    "  if (i < p.count) o.data[i] = i * p.mul + p.add + uint(p.pad.y); }\n";

/* Checks data[first..first+count) against i * mul + add and that everything
 * else in [0, total) still holds fill. Returns the number of wrong words and
 * describes the first. */
static unsigned check_fill(const uint32_t *m, unsigned total, unsigned first, unsigned count, uint32_t mul,
                           uint32_t add, uint32_t fill, char *what, size_t n)
{
    unsigned wrong = 0;
    what[0] = 0;
    for (unsigned i = 0; i < total; i++) {
        const int in = i >= first && i < first + count;
        const uint32_t want = in ? (i - first) * mul + add : fill;
        if (m[i] != want && !wrong++)
            snprintf(what, n, "first wrong word %u: 0x%08x, want 0x%08x (%s)", i, m[i], want,
                     in ? "written range" : "outside the written range");
    }
    return wrong;
}

static void g_compute_ssbo(void)
{
    char what[160];
    const unsigned total = N + GUARD;
    uint32_t *init = malloc(total * 4);
    for (unsigned i = 0; i < total; i++)
        init[i] = 0xAAAAAAAAu;

    /* The first compute program of the run: compile + link, then the first
     * dispatch (which is when a lazy back end would do its work). */
    const double t0 = probe_now_ms();
    GLuint prog = mk_compute(kCsFill, g_log, sizeof g_log);
    const double t1 = probe_now_ms();
    if (!prog) {
        R("time-compute-first-compile", 0, "compute program did not build (%.1f ms): %s", t1 - t0, g_log);
        free(init);
        return;
    }
    GLuint ssbo = mk_buffer(GL_SHADER_STORAGE_BUFFER, total * 4, init, GL_DYNAMIC_COPY);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);
    glUseProgram(prog);
    glDispatchCompute(17, 1, 1); /* 1088 invocations; the shader stops at 1024 */
    glMemoryBarrier(GL_ALL_BARRIER_BITS); /* CommandBufferGL.cpp:1251 */
    glFinish();
    const double t2 = probe_now_ms();
    GLenum e = glerr();
    R("time-compute-first-compile", e == GL_NO_ERROR, "compile + link %.1f ms, first glDispatchCompute + glMemoryBarrier + glFinish %.1f ms, GL error %s",
      t1 - t0, t2 - t1, errname(e));

    const uint32_t *m = map_ro(GL_SHADER_STORAGE_BUFFER, total * 4);
    e = glerr();
    unsigned wrong = m ? check_fill(m, total, 0, N, 3, 7, 0xAAAAAAAAu, what, sizeof what) : total;
    R("compute-ssbo-map", m && !wrong, "glDispatchCompute(17,1,1) x 64 writing 1024 uints, glMapBufferRange(READ): %u of %u words wrong %s, map %s (ES has no glGetBufferSubData: eglGetProcAddress says %s)",
      wrong, total, what, m ? "ok" : errname(e), gp("glGetBufferSubData") ? "present" : "absent");
    if (m)
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);

    /* The other way to read without glGetBufferSubData: copy, then map the copy. */
    glBindBuffer(GL_COPY_READ_BUFFER, ssbo);
    GLuint copy = mk_buffer(GL_COPY_WRITE_BUFFER, total * 4, NULL, GL_STREAM_READ);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, total * 4);
    m = map_ro(GL_COPY_WRITE_BUFFER, total * 4);
    e = glerr();
    wrong = m ? check_fill(m, total, 0, N, 3, 7, 0xAAAAAAAAu, what, sizeof what) : total;
    R("compute-ssbo-copyread", m && !wrong, "glCopyBufferSubData to a second buffer, mapped: %u of %u words wrong %s, GL error %s", wrong, total, what, errname(e));
    if (m)
        glUnmapBuffer(GL_COPY_WRITE_BUFFER);
    glDeleteBuffers(1, &copy);
    glDeleteProgram(prog);

    /* A uniform block (std140, bound at an offset as Dawn's dynamic offsets
     * are) and arrayLength, whole-buffer and range bindings. */
    prog = mk_compute(kCsUbo, g_log, sizeof g_log);
    if (!prog) {
        R("compute-ubo-std140", 0, "compute program did not build: %s", g_log);
        glDeleteBuffers(1, &ssbo);
        free(init);
        return;
    }
    GLint ualign = 1, salign = 1;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &ualign);
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &salign);
    const unsigned uoff = (unsigned)(ualign > 256 ? ualign : 256);
    uint8_t *ubytes = calloc(1, uoff + 48);
    struct { uint32_t mul, add, pad0[2]; float pad[4]; uint32_t count, pad1[3]; } params = {5, 11, {0, 0}, {0, 100, 0, 0}, N, {0, 0, 0}};
    memset(ubytes, 0xEE, uoff);
    memcpy(ubytes + uoff, &params, 48);
    GLuint ubo = mk_buffer(GL_UNIFORM_BUFFER, uoff + 48, ubytes, GL_DYNAMIC_DRAW);
    uint32_t info_init[2] = {0xBBBBBBBBu, 0xBBBBBBBBu};
    GLuint info = mk_buffer(GL_SHADER_STORAGE_BUFFER, sizeof info_init, info_init, GL_DYNAMIC_COPY);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, total * 4, init);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, info);
    glBindBufferRange(GL_UNIFORM_BUFFER, 2, ubo, uoff, 48);
    glUseProgram(prog);
    glDispatchCompute(17, 1, 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    m = map_ro(GL_SHADER_STORAGE_BUFFER, total * 4);
    e = glerr();
    wrong = m ? check_fill(m, total, 0, N, 5, 111, 0xAAAAAAAAu, what, sizeof what) : total;
    R("compute-ubo-std140", m && !wrong, "uniform block {uint mul@0, uint add@4, vec4 pad@16, uint count@32} bound with glBindBufferRange at offset %u: data[i] = i*5+11+100: %u of %u words wrong %s, GL error %s",
      uoff, wrong, total, what, errname(e));
    if (m)
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, info);
    m = map_ro(GL_SHADER_STORAGE_BUFFER, sizeof info_init);
    const uint32_t len_base = m ? m[0] : 0, count_seen = m ? m[1] : 0;
    if (m)
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);

    /* The same with the storage buffer bound as a range: 512 words at an
     * aligned offset. */
    const unsigned soff = (unsigned)(salign > 256 ? salign : 256);
    params.count = 512;
    params.mul = 9;
    glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, uoff, 48, &params);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, total * 4, init);
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, ssbo, soff, 512 * 4);
    glDispatchCompute(8, 1, 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    m = map_ro(GL_SHADER_STORAGE_BUFFER, total * 4);
    e = glerr();
    wrong = m ? check_fill(m, total, soff / 4, 512, 9, 111, 0xAAAAAAAAu, what, sizeof what) : total;
    if (m)
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, info);
    m = map_ro(GL_SHADER_STORAGE_BUFFER, sizeof info_init);
    const uint32_t len_range = m ? m[0] : 0;
    if (m)
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    R("compute-ssbo-arraylength", len_base == total && len_range == 512 && count_seen == N && !wrong,
      "data.length() with glBindBufferBase: %u (want %u); with glBindBufferRange(offset %u, 512 words): %u (want 512), and the writes through the range: %u words wrong %s; count read from the UBO %u, GL error %s",
      len_base, total, soff, len_range, wrong, what, count_seen, errname(e));

    glDeleteBuffers(1, &ssbo);
    glDeleteBuffers(1, &info);
    glDeleteBuffers(1, &ubo);
    glDeleteProgram(prog);
    free(ubytes);
    free(init);
}

/* ---- compute: images ---------------------------------------------------- */

static const char kCsStore[] =
    "#version 310 es\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(rgba8, binding = 0) writeonly uniform highp image2D dst8;\n"
    "layout(r32f, binding = 1) writeonly uniform highp image2D dstf;\n"
    "void main() { ivec2 c = ivec2(gl_GlobalInvocationID.xy);\n"
    "  imageStore(dst8, c, vec4(float(c.x) / 31.0, float(c.y) / 31.0, 0.25, 1.0));\n"
    "  imageStore(dstf, c, vec4(float(c.x) * 1.5 + float(c.y) * 100.0 - 7.0)); }\n";

static const char kCsLoad[] =
    "#version 310 es\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(r32f, binding = 0) readonly uniform highp image2D srcf;\n"
    "layout(rgba8, binding = 1) readonly uniform highp image2D src8;\n"
    "layout(rgba8, binding = 2) writeonly uniform highp image2D dst;\n"
    "void main() { ivec2 c = ivec2(gl_GlobalInvocationID.xy);\n"
    "  float f = imageLoad(srcf, c).r; vec4 p = imageLoad(src8, ivec2(31) - c);\n"
    "  float want = float(c.x) * 1.5 + float(c.y) * 100.0 - 7.0;\n"
    "  imageStore(dst, c, vec4(p.r, p.g, f == want ? 1.0 : 0.0, 1.0)); }\n";

static const char kCsReadWrite[] =
    "#version 310 es\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(r32f, binding = 0) uniform highp image2D rw;\n"
    "void main() { ivec2 c = ivec2(gl_GlobalInvocationID.xy);\n"
    "  float f = imageLoad(rw, c).r; imageStore(rw, c, vec4(f * 2.0 + 1.0)); }\n";

static int unorm(int v, int max) { return (v * 255 * 2 + max) / (max * 2); }

/* Reads the whole 32x32 R32F texture back and counts texels that are not
 * scale * (x * 1.5 + y * 100 - 7) + bias. */
static int check_r32f(GLuint tex, float scale, float bias, char *what, size_t n)
{
    static GLfloat f[W * H * 4];
    GLuint fbo = mk_fbo(tex);
    int wrong = 0;
    what[0] = 0;
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        snprintf(what, n, "an R32F texture is not a complete framebuffer");
        wrong = W * H;
    } else {
        memset(f, 0, sizeof f);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_FLOAT, f);
        const GLenum e = glerr();
        if (e != GL_NO_ERROR) {
            snprintf(what, n, "glReadPixels(GL_RGBA, GL_FLOAT): %s", errname(e));
            wrong = W * H;
        }
        for (int y = 0; y < H && !e; y++)
            for (int x = 0; x < W; x++) {
                const float want = scale * ((float)x * 1.5f + (float)y * 100.0f - 7.0f) + bias;
                if (f[(y * W + x) * 4] != want && !wrong++)
                    snprintf(what, n, "first wrong texel (%d,%d): %g, want %g", x, y, (double)f[(y * W + x) * 4], (double)want);
            }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    return wrong;
}

static void g_compute_image(void)
{
    char what[160];
    static uint8_t px[W * H * 4];
    GLuint store = mk_compute(kCsStore, g_log, sizeof g_log);
    if (!store) {
        R("compute-image-store-rgba8", 0, "imageStore program did not build: %s", g_log);
        return;
    }
    GLuint t8 = mk_tex(GL_RGBA8, W, H), tf = mk_tex(GL_R32F, W, H), out = mk_tex(GL_RGBA8, W, H);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(store);
    glBindImageTexture(0, t8, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(1, tf, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
    glDispatchCompute(4, 4, 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    GLenum e = glerr();

    GLuint fbo = mk_fbo(t8);
    int wrong = 0;
    what[0] = 0;
    memset(px, 0x5b, sizeof px);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t *p = px + (y * W + x) * 4;
            if (!near4(p, unorm(x, 31), unorm(y, 31), 64, 255, 1) && !wrong++)
                snprintf(what, sizeof what, "first wrong texel (%d,%d): %d,%d,%d,%d want %d,%d,64,255", x, y, p[0], p[1],
                         p[2], p[3], unorm(x, 31), unorm(y, 31));
        }
    R("compute-image-store-rgba8", !wrong && !e, "imageStore to an rgba8 image, glDispatchCompute(4,4,1) x 8x8, read through an FBO: %d of %d texels wrong %s, GL error %s",
      wrong, W * H, what, errname(e));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);

    wrong = check_r32f(tf, 1, 0, what, sizeof what);
    R("compute-image-store-r32f", !wrong, "imageStore to an r32f image, read through an FBO as GL_RGBA/GL_FLOAT: %d of %d texels wrong %s", wrong, W * H, what);

    GLuint load = mk_compute(kCsLoad, g_log, sizeof g_log);
    if (!load) {
        R("compute-image-load", 0, "imageLoad program did not build: %s", g_log);
    } else {
        glUseProgram(load);
        glBindImageTexture(0, tf, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
        glBindImageTexture(1, t8, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
        glBindImageTexture(2, out, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glDispatchCompute(4, 4, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        e = glerr();
        fbo = mk_fbo(out);
        wrong = 0;
        int float_wrong = 0;
        what[0] = 0;
        memset(px, 0x5b, sizeof px);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const uint8_t *p = px + (y * W + x) * 4;
                float_wrong += p[2] != 255;
                if (!near4(p, unorm(31 - x, 31), unorm(31 - y, 31), 255, 255, 1) && !wrong++)
                    snprintf(what, sizeof what, "first wrong texel (%d,%d): %d,%d,%d,%d want %d,%d,255,255", x, y, p[0],
                             p[1], p[2], p[3], unorm(31 - x, 31), unorm(31 - y, 31));
            }
        R("compute-image-load", !wrong && !e, "imageLoad from the r32f and rgba8 images written by the first dispatch into a third: %d of %d texels wrong (%d with the r32f value wrong) %s, GL error %s",
          wrong, W * H, float_wrong, what, errname(e));
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteProgram(load);
    }

    GLuint rw = mk_compute(kCsReadWrite, g_log, sizeof g_log);
    if (!rw) {
        R("compute-image-r32f-readwrite", 0, "read-write image program did not build: %s", g_log);
    } else {
        glUseProgram(rw);
        glBindImageTexture(0, tf, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
        glDispatchCompute(4, 4, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        e = glerr();
        wrong = check_r32f(tf, 2, 1, what, sizeof what);
        R("compute-image-r32f-readwrite", !wrong && !e, "imageLoad + imageStore on one r32f image (GL_READ_WRITE), f = f*2+1 in place: %d of %d texels wrong %s, GL error %s",
          wrong, W * H, what, errname(e));
        glDeleteProgram(rw);
    }
    glDeleteProgram(store);
    glDeleteTextures(1, &t8);
    glDeleteTextures(1, &tf);
    glDeleteTextures(1, &out);
}

/* ---- indirect ----------------------------------------------------------- */

static const char kCsIndirect[] =
    "#version 310 es\n"
    "layout(local_size_x = 8) in;\n"
    "layout(std430, binding = 0) buffer Out { uint data[]; } o;\n"
    "void main() { o.data[gl_GlobalInvocationID.y * 32u + gl_GlobalInvocationID.x] = 1u + gl_WorkGroupID.x + 10u * gl_WorkGroupID.y; }\n";

static void g_indirect(void)
{
    NEED_BASICS("indirect-draw-arrays");
    uint8_t l[4], r[4];
    target t = mk_target(GL_RGBA8);

    /* DrawArraysIndirectCommand {count, instanceCount, first, reserved}. Two
     * commands in one buffer: at 0 the left half, at 16 the right half. */
    static const GLuint arrays[8] = {6, 1, 0, 0, 6, 1, 6, 0};
    GLuint cmd = mk_buffer(GL_DRAW_INDIRECT_BUFFER, sizeof arrays, arrays, GL_STATIC_DRAW);
    clearc(0, 0, 1, 1);
    set_xform(g_solid, 1, 1, 0, 0, 0);
    glUniform4f(glGetUniformLocation(g_solid, "color"), 1, 0, 0, 1);
    glBindVertexArray(g_half_vao);
    glDrawArraysIndirect(GL_TRIANGLES, (const void *)(uintptr_t)16);
    GLenum e = glerr();
    rd(LX, MY, l);
    rd(RX, MY, r);
    int ok = near4(l, 0, 0, 255, 255, 0) && near4(r, 255, 0, 0, 255, 0);
    glUniform4f(glGetUniformLocation(g_solid, "color"), 0, 1, 0, 1);
    glDrawArraysIndirect(GL_TRIANGLES, (const void *)(uintptr_t)0);
    uint8_t l2[4];
    rd(LX, MY, l2);
    ok = ok && near4(l2, 0, 255, 0, 255, 0);
    R("indirect-draw-arrays", ok && !e, "glDrawArraysIndirect at offset 16 {6,1,first 6}: left %d,%d,%d right %d,%d,%d (want blue, red); at offset 0 {6,1,first 0}: left %d,%d,%d (want green); GL error %s",
      l[0], l[1], l[2], r[0], r[1], r[2], l2[0], l2[1], l2[2], errname(e));
    glDeleteBuffers(1, &cmd);

    /* DrawElementsIndirectCommand {count, instanceCount, firstIndex,
     * baseVertex, reserved} at offset 20: indices 3..8 of the element buffer,
     * base vertex 6 (the right half). */
    static const GLushort indices[9] = {0, 0, 0, 0, 1, 2, 3, 4, 5};
    static const GLuint elements[10] = {0xdead, 0xdead, 0xdead, 0xdead, 0xdead, 6, 1, 3, 6, 0};
    GLuint vao = 0, ebo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_half_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    ebo = mk_buffer(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STATIC_DRAW);
    cmd = mk_buffer(GL_DRAW_INDIRECT_BUFFER, sizeof elements, elements, GL_STATIC_DRAW);
    clearc(0, 0, 1, 1);
    glUniform4f(glGetUniformLocation(g_solid, "color"), 1, 0, 0, 1);
    glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT, (const void *)(uintptr_t)20);
    e = glerr();
    rd(LX, MY, l);
    rd(RX, MY, r);
    R("indirect-draw-elements", near4(l, 0, 0, 255, 255, 0) && near4(r, 255, 0, 0, 255, 0) && !e,
      "glDrawElementsIndirect(GL_UNSIGNED_SHORT) at offset 20 {6,1,firstIndex 3,baseVertex 6}: left %d,%d,%d right %d,%d,%d (want blue, red), GL error %s",
      l[0], l[1], l[2], r[0], r[1], r[2], errname(e));
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &ebo);
    glDeleteBuffers(1, &cmd);
    rm_target(&t);

    /* glDispatchComputeIndirect {x, y, z} at offset 8: 4 x 2 x 1 groups of 8. */
    GLuint prog = mk_compute(kCsIndirect, g_log, sizeof g_log);
    if (!prog) {
        R("indirect-dispatch", 0, "compute program did not build: %s", g_log);
        return;
    }
    uint32_t init[80];
    for (int i = 0; i < 80; i++)
        init[i] = 0xAAAAAAAAu;
    static const GLuint groups[5] = {99, 99, 4, 2, 1};
    GLuint ssbo = mk_buffer(GL_SHADER_STORAGE_BUFFER, sizeof init, init, GL_DYNAMIC_COPY);
    GLuint ind = mk_buffer(GL_DISPATCH_INDIRECT_BUFFER, sizeof groups, groups, GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);
    glUseProgram(prog);
    glDispatchComputeIndirect(8);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    e = glerr();
    const uint32_t *m = map_ro(GL_SHADER_STORAGE_BUFFER, sizeof init);
    unsigned wrong = 0;
    char what[120] = "";
    for (unsigned i = 0; m && i < 80; i++) {
        const uint32_t want = i < 64 ? 1 + (i % 32) / 8 + 10 * (i / 32) : 0xAAAAAAAAu;
        if (m[i] != want && !wrong++)
            snprintf(what, sizeof what, "first wrong word %u: 0x%x, want 0x%x", i, m[i], want);
    }
    R("indirect-dispatch", m && !wrong && !e, "glDispatchComputeIndirect(8) with {4,2,1} in the buffer: %u of 80 words wrong %s, GL error %s", m ? wrong : 80, what, errname(e));
    if (m)
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    glDeleteBuffers(1, &ssbo);
    glDeleteBuffers(1, &ind);
    glDeleteProgram(prog);
}

/* ---- glCopyImageSubData ------------------------------------------------- */

static void pattern(uint8_t *px, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = px + (y * w + x) * 4;
            p[0] = (uint8_t)(x * 8);
            p[1] = (uint8_t)(y * 8);
            p[2] = 77;
            p[3] = 255;
        }
}

/* Counts the texels of the bound read framebuffer's w x h area that differ
 * from: the pattern shifted by (sx, sy) inside the rectangle (dx, dy, cw, ch),
 * zero outside it. */
static int check_copy(int w, int h, int dx, int dy, int cw, int ch, int sx, int sy, char *what, size_t n)
{
    static uint8_t px[W * H * 4];
    int wrong = 0;
    what[0] = 0;
    memset(px, 0x5b, sizeof px);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *p = px + (y * w + x) * 4;
            const int in = x >= dx && x < dx + cw && y >= dy && y < dy + ch;
            const int r = in ? (x - dx + sx) * 8 : 0, g = in ? (y - dy + sy) * 8 : 0, b = in ? 77 : 0, a = in ? 255 : 0;
            if (!near4(p, r, g, b, a, 0) && !wrong++)
                snprintf(what, n, "first wrong texel (%d,%d): %d,%d,%d,%d want %d,%d,%d,%d", x, y, p[0], p[1], p[2], p[3], r, g, b, a);
        }
    return wrong;
}

static void g_copy_image(void)
{
    NEED_BASICS("copy-image-2d");
    static uint8_t src_px[W * H * 4], zero[W * H * 4];
    char what[160];
    pattern(src_px, W, H);
    GLuint src = mk_tex(GL_RGBA8, W, H);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, src_px);

    GLuint dst = mk_tex(GL_RGBA8, W, H);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    glerr();
    glCopyImageSubData(src, GL_TEXTURE_2D, 0, 4, 4, 0, dst, GL_TEXTURE_2D, 0, 16, 8, 0, 8, 8, 1);
    GLenum e = glerr();
    GLuint fbo = mk_fbo(dst);
    int wrong = check_copy(W, H, 16, 8, 8, 8, 4, 4, what, sizeof what);
    R("copy-image-2d", !wrong && !e, "glCopyImageSubData 8x8 from (4,4) of an uploaded RGBA8 texture to (16,8) of a zeroed one: %d of %d texels wrong %s, GL error %s",
      wrong, W * H, what, errname(e));
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &dst);

    /* Into layer 2 of a 16x16x4 array (UtilsGL.cpp:89 copies layer by layer). */
    GLuint arr = 0;
    glGenTextures(1, &arr);
    glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 16, 16, 4);
    for (int layer = 0; layer < 4; layer++)
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, 16, 16, 1, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    glerr();
    glCopyImageSubData(src, GL_TEXTURE_2D, 0, 8, 8, 0, arr, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 2, 16, 16, 1);
    e = glerr();
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, arr, 0, 2);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    wrong = check_copy(16, 16, 0, 0, 16, 16, 8, 8, what, sizeof what);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, arr, 0, 1);
    char what1[160];
    const int wrong1 = check_copy(16, 16, 0, 0, 0, 0, 0, 0, what1, sizeof what1);
    /* And back out of the array into a 2D texture. */
    GLuint back = mk_tex(GL_RGBA8, W, H);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    glCopyImageSubData(arr, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 2, back, GL_TEXTURE_2D, 0, 8, 8, 0, 16, 16, 1);
    const GLenum e2 = glerr();
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, back, 0);
    char what2[160];
    const int wrong2 = check_copy(W, H, 8, 8, 16, 16, 8, 8, what2, sizeof what2);
    R("copy-image-2darray", !wrong && !wrong1 && !wrong2 && !e && !e2,
      "2D -> layer 2 of a 2D array: %d of 256 texels wrong %s (fbo 0x%04x), layer 1 untouched: %d wrong %s; layer 2 -> 2D: %d of %d wrong %s; GL errors %s, %s",
      wrong, what, status, wrong1, what1, wrong2, W * H, what2, errname(e), errname(e2));
    glDeleteTextures(1, &back);
    glDeleteTextures(1, &arr);

    /* Into mip level 1 of a two-level texture. */
    GLuint mip = 0;
    glGenTextures(1, &mip);
    glBindTexture(GL_TEXTURE_2D, mip);
    glTexStorage2D(GL_TEXTURE_2D, 2, GL_RGBA8, W, H);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    glTexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, W / 2, H / 2, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    glerr();
    glCopyImageSubData(src, GL_TEXTURE_2D, 0, 2, 3, 0, mip, GL_TEXTURE_2D, 1, 4, 4, 0, 8, 8, 1);
    e = glerr();
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mip, 1);
    wrong = check_copy(16, 16, 4, 4, 8, 8, 2, 3, what, sizeof what);
    R("copy-image-mip", !wrong && !e, "level 0 -> level 1 of a two-level texture: %d of 256 texels wrong %s, GL error %s", wrong, what, errname(e));
    glDeleteTextures(1, &mip);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &src);

    /* A source the GPU drew (not a CPU upload), then a CPU upload over part
     * of the copy: both orders of the driver's CPU and GPU copies. */
    target drawn = mk_target(GL_RGBA8);
    clearc(0, 0, 1, 1);
    solid(0.5f, 1, -0.5f, 0, 0, 1, 0, 0, 1);
    dst = mk_tex(GL_RGBA8, W, H);
    glerr();
    glCopyImageSubData(drawn.tex, GL_TEXTURE_2D, 0, 0, 0, 0, dst, GL_TEXTURE_2D, 0, 0, 0, 0, W, H, 1);
    e = glerr();
    static const uint8_t green[4 * 4 * 4] = {
        0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255,
        0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255,
        0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
    glBindTexture(GL_TEXTURE_2D, dst);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, green);
    fbo = mk_fbo(dst);
    uint8_t l[4], r[4], c[4];
    rd(LX, MY, l);
    rd(RX, MY, r);
    rd(1, 1, c);
    R("copy-image-rendered", near4(l, 255, 0, 0, 255, 0) && near4(r, 0, 0, 255, 255, 0) && near4(c, 0, 255, 0, 255, 0) && !e,
      "source drawn by the GPU, copied whole, then a 4x4 glTexSubImage2D over the copy's corner: left %d,%d,%d right %d,%d,%d corner %d,%d,%d (want red, blue, green), GL error %s",
      l[0], l[1], l[2], r[0], r[1], r[2], c[0], c[1], c[2], errname(e));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &dst);
    rm_target(&drawn);
}

/* ---- sync --------------------------------------------------------------- */

static void g_sync(void)
{
    NEED_BASICS("sync-gl-fence");
    target t = mk_target(GL_RGBA8);
    clearc(0, 0, 0, 1);
    for (int i = 0; i < 50; i++)
        solid(1, 1, 0, 0, 0, 1, 1, 1, 1);
    GLsync s = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    GLenum e = glerr();
    if (!s) {
        R("sync-gl-fence", 0, "glFenceSync returned 0, GL error %s", errname(e));
        rm_target(&t);
        return;
    }
    const double t0 = probe_now_ms();
    const GLenum first = glClientWaitSync(s, 0, 0); /* no flush, no wait */
    const GLenum r = glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
    const double ms = probe_now_ms() - t0;
    GLint status = 0;
    glGetSynciv(s, GL_SYNC_STATUS, 1, NULL, &status);
    e = glerr();
    glDeleteSync(s);
    R("sync-gl-fence", (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED) && status == GL_SIGNALED && !e,
      "glFenceSync after 50 draws; glClientWaitSync(0, 0) 0x%04x, then (FLUSH_COMMANDS, 1 s) 0x%04x %s in %.2f ms, GL_SYNC_STATUS %s, GL error %s (Dawn waits with EGL sync objects, not these)",
      first, r, r == GL_ALREADY_SIGNALED ? "ALREADY_SIGNALED" : r == GL_CONDITION_SATISFIED ? "CONDITION_SATISFIED" :
      r == GL_TIMEOUT_EXPIRED ? "TIMEOUT_EXPIRED" : "WAIT_FAILED", ms, status == GL_SIGNALED ? "SIGNALED" : "UNSIGNALED", errname(e));
    rm_target(&t);
}

/* ---- texture buffers ---------------------------------------------------- */

static const char kFsTexBuf[] =
    "#version 320 es\nprecision highp float;\nuniform highp samplerBuffer tb;\nout vec4 o;\n"
    "void main() { vec4 c = texelFetch(tb, int(gl_FragCoord.x) / 8);\n"
    "  o = textureSize(tb) == 4 ? c : vec4(0.0, 0.0, 0.0, 0.0); }\n";

static void g_texbuffer(void)
{
    NEED_BASICS("texture-buffer");
    GLuint prog = mk_program(kVs320, kFsTexBuf, g_log, sizeof g_log);
    if (!prog) {
        R("texture-buffer", 0, "samplerBuffer program (#version 320 es) did not build: %s", g_log);
        return;
    }
    static const uint8_t bytes[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    static const float floats[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    target t = mk_target(GL_RGBA8);
    GLuint buf = mk_buffer(GL_TEXTURE_BUFFER, sizeof bytes, bytes, GL_STATIC_DRAW), tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_BUFFER, tex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA8, buf);
    GLenum e = glerr();
    clearc(0, 0, 0, 0);
    blit_tex(prog, GL_TEXTURE_BUFFER, tex);
    uint8_t p[4][4];
    for (int i = 0; i < 4; i++)
        rd(i * 8 + 4, MY, p[i]);
    int ok = near4(p[0], 255, 0, 0, 255, 0) && near4(p[1], 0, 255, 0, 255, 0) && near4(p[2], 0, 0, 255, 255, 0) &&
             near4(p[3], 255, 255, 255, 255, 0);
    /* R32F from a second buffer. */
    GLuint fbuf = mk_buffer(GL_TEXTURE_BUFFER, sizeof floats, floats, GL_STATIC_DRAW);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32F, fbuf);
    GLenum e2 = glerr();
    blit_tex(prog, GL_TEXTURE_BUFFER, tex);
    uint8_t q[4][4];
    for (int i = 0; i < 4; i++)
        rd(i * 8 + 4, MY, q[i]);
    const int okf = near4(q[0], 64, 0, 0, 255, 1) && near4(q[1], 128, 0, 0, 255, 1) && near4(q[2], 191, 0, 0, 255, 1) &&
                    near4(q[3], 255, 0, 0, 255, 1);
    R("texture-buffer", ok && okf && !e && !e2,
      "glTexBuffer(GL_RGBA8) + texelFetch(samplerBuffer): %d,%d,%d %d,%d,%d %d,%d,%d %d,%d,%d (want red green blue white); GL_R32F: red %d %d %d %d (want 64 128 191 255); GL errors %s, %s",
      p[0][0], p[0][1], p[0][2], p[1][0], p[1][1], p[1][2], p[2][0], p[2][1], p[2][2], p[3][0], p[3][1], p[3][2],
      q[0][0], q[1][0], q[2][0], q[3][0], errname(e), errname(e2));
    glBindTexture(GL_TEXTURE_BUFFER, 0);
    glDeleteTextures(1, &tex);
    glDeleteBuffers(1, &buf);
    glDeleteBuffers(1, &fbuf);
    glDeleteProgram(prog);
    rm_target(&t);
}

/* ---- multisampling ------------------------------------------------------ */

typedef struct { GLuint fbo, tex, depth; GLenum status; } ms_target;

static ms_target mk_ms(GLenum fmt, int with_depth)
{
    ms_target m = {0, 0, 0, 0};
    glGenTextures(1, &m.tex);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m.tex);
    glTexStorage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, fmt, W, H, GL_TRUE); /* TextureGL.cpp:173 */
    glGenFramebuffers(1, &m.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, m.tex, 0);
    if (with_depth) {
        glGenTextures(1, &m.depth);
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m.depth);
        glTexStorage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_DEPTH24_STENCIL8, W, H, GL_TRUE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D_MULTISAMPLE, m.depth, 0);
    }
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
    m.status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glViewport(0, 0, W, H);
    return m;
}

static void rm_ms(ms_target *m)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &m->fbo);
    glDeleteTextures(1, &m->tex);
    if (m->depth)
        glDeleteTextures(1, &m->depth);
}

/* CommandBufferGL.cpp:670: the resolve is a glBlitFramebuffer. Leaves dst
 * bound for reading. */
static GLenum resolve(const ms_target *m, const target *dst)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst->fbo);
    glerr();
    glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    const GLenum e = glerr();
    glBindFramebuffer(GL_FRAMEBUFFER, dst->fbo);
    return e;
}

static const char kFsSampleId[] =
    "#version 320 es\nprecision highp float;\nout vec4 o;\n"
    "void main() { o = vec4(float(gl_SampleID) / 3.0, 0.0, 0.0, 1.0); }\n";

static void ms_resolve_test(const char *name, GLenum fmt, int is_float)
{
    ms_target m = mk_ms(fmt, 1);
    target dst = mk_target(fmt);
    if (m.status != GL_FRAMEBUFFER_COMPLETE || dst.status != GL_FRAMEBUFFER_COMPLETE) {
        R(name, 0, "4x multisample FBO 0x%04x (colour texture + DEPTH24_STENCIL8 texture), resolve target 0x%04x, GL error %s", m.status, dst.status, errname(glerr()));
        rm_ms(&m);
        rm_target(&dst);
        return;
    }
    /* A white triangle over the lower-left half: its long edge runs through
     * the pixels (i, 31 - i). */
    static const GLfloat tri[] = {-1, -1, 1, -1, -1, 1};
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    vbo = mk_buffer(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, m.fbo);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0, 0, 0, 1);
    glClearDepthf(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    set_xform(g_solid, 1, 1, 0, 0, 0);
    glUniform4f(glGetUniformLocation(g_solid, "color"), 1, 1, 1, 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_DEPTH_TEST);
    GLenum e = glerr();
    const GLenum be = resolve(&m, &dst);
    int inside = 0, outside = 0, edge = 0;
    if (is_float) {
        GLfloat f[4] = {-1, -1, -1, -1};
        glReadPixels(4, 4, 1, 1, GL_RGBA, GL_FLOAT, f);
        inside = f[0] == 1.0f;
        glReadPixels(28, 28, 1, 1, GL_RGBA, GL_FLOAT, f);
        outside = f[0] == 0.0f;
        for (int i = 0; i < W; i++) {
            glReadPixels(i, 31 - i, 1, 1, GL_RGBA, GL_FLOAT, f);
            edge += f[0] > 0.01f && f[0] < 0.99f;
        }
    } else {
        uint8_t p[4];
        rd(4, 4, p);
        inside = p[0] == 255;
        rd(28, 28, p);
        outside = p[0] == 0;
        for (int i = 0; i < W; i++) {
            rd(i, 31 - i, p);
            edge += p[0] > 0 && p[0] < 255;
        }
    }
    const GLenum re = glerr();
    R(name, inside && outside && edge >= 8 && !e && !be && !re,
      "glTexStorage2DMultisample(4x) colour + depth-stencil, triangle, glBlitFramebuffer resolve: inside %s, outside %s, %d of 32 edge pixels are intermediate (want most); GL errors draw %s, blit %s, read %s",
      inside ? "white" : "WRONG", outside ? "black" : "WRONG", edge, errname(e), errname(be), errname(re));
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
    rm_ms(&m);
    rm_target(&dst);
}

static void g_msaa(void)
{
    NEED_BASICS("msaa-resolve-rgba8");
    ms_resolve_test("msaa-resolve-rgba8", GL_RGBA8, 0);
    ms_resolve_test("msaa-resolve-rgba16f", GL_RGBA16F, 1);

    ms_target m = mk_ms(GL_RGBA8, 0);
    target dst = mk_target(GL_RGBA8);
    uint8_t p[4];
    if (m.status != GL_FRAMEBUFFER_COMPLETE) {
        R("sample-shading", 0, "4x multisample RGBA8 FBO 0x%04x", m.status);
        rm_ms(&m);
        rm_target(&dst);
        return;
    }
    /* gl_SampleID makes the shader run per sample: the resolve averages
     * 0, 1/3, 2/3, 1. */
    GLuint prog = mk_program(kVs320, kFsSampleId, g_log, sizeof g_log);
    if (!prog) {
        R("sample-shading", 0, "gl_SampleID program (#version 320 es) did not build: %s", g_log);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, m.fbo);
        clearc(0, 0, 0, 1);
        glEnable(GL_SAMPLE_SHADING);
        glMinSampleShading(1.0f);
        set_xform(prog, 1, 1, 0, 0, 0);
        glBindVertexArray(g_quad_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisable(GL_SAMPLE_SHADING);
        const GLenum e = glerr();
        resolve(&m, &dst);
        rd(LX, MY, p);
        R("sample-shading", abs(p[0] - 128) <= 3 && !e, "gl_SampleID / 3 in red with GL_SAMPLE_SHADING, resolved: red %d (want 127 or 128: the mean of four samples), GL error %s", p[0], errname(e));
        glDeleteProgram(prog);
    }

    /* RenderPipelineGL.cpp:347: glSampleMaski (GL_SAMPLE_MASK is always on). */
    glBindFramebuffer(GL_FRAMEBUFFER, m.fbo);
    clearc(0, 0, 0, 1);
    glEnable(GL_SAMPLE_MASK);
    glSampleMaski(0, 0x5);
    solid(1, 1, 0, 0, 0, 1, 1, 1, 1);
    glSampleMaski(0, 0xffffffffu);
    glDisable(GL_SAMPLE_MASK);
    GLenum e = glerr();
    resolve(&m, &dst);
    rd(LX, MY, p);
    R("sample-mask", abs(p[0] - 128) <= 3 && !e, "white with glSampleMaski(0, 0x5) on 4x, resolved: %d (want 127 or 128: two of four samples), GL error %s", p[0], errname(e));

    /* RenderPipelineGL.cpp:349. */
    glBindFramebuffer(GL_FRAMEBUFFER, m.fbo);
    clearc(0, 0, 0, 1);
    glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    solid(1, 1, 0, 0, 0, 1, 1, 1, 0.5f);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    e = glerr();
    resolve(&m, &dst);
    rd(LX, MY, p);
    R("alpha-to-coverage", p[0] >= 48 && p[0] <= 208 && !e, "white, alpha 0.5, GL_SAMPLE_ALPHA_TO_COVERAGE on 4x, resolved: %d (want about half; 255 means no coverage reduction), GL error %s", p[0], errname(e));
    rm_ms(&m);
    rm_target(&dst);
}

/* ---- float render targets ----------------------------------------------- */

static int feq(float got, float want)
{
    return fabsf(got - want) <= 0.03f * fabsf(want) + 1e-4f;
}

static void float_rt(const char *name, GLenum fmt, int channels, int is_signed)
{
    target t = mk_target(fmt);
    if (t.status != GL_FRAMEBUFFER_COMPLETE) {
        R(name, 0, "not a complete framebuffer: 0x%04x, GL error %s", t.status, errname(glerr()));
        rm_target(&t);
        return;
    }
    const float g_in = is_signed ? -1.0f : 0.25f;
    GLfloat l[4] = {-9, -9, -9, -9}, r[4] = {-9, -9, -9, -9};
    clearc(0.5f, 2.0f, 0.125f, 1.0f);
    solid(0.5f, 1, -0.5f, 0, 0, 2.5f, g_in, 1000.0f, 0.5f);
    glReadPixels(LX, MY, 1, 1, GL_RGBA, GL_FLOAT, l);
    glReadPixels(RX, MY, 1, 1, GL_RGBA, GL_FLOAT, r);
    const GLenum e = glerr();
    /* Channels the format lacks read as 0, alpha as 1. */
    const float wl[4] = {2.5f, channels > 1 ? g_in : 0, channels > 2 ? 1000.0f : 0, channels > 3 ? 0.5f : 1};
    const float wr[4] = {0.5f, channels > 1 ? 2.0f : 0, channels > 2 ? 0.125f : 0, 1};
    int ok = !e;
    for (int i = 0; i < 4; i++)
        ok = ok && feq(l[i], wl[i]) && feq(r[i], wr[i]);
    R(name, ok, "draw (2.5, %g, 1000, 0.5) left over a clear to (0.5, 2, 0.125, 1), glReadPixels(GL_RGBA, GL_FLOAT): left %g,%g,%g,%g right %g,%g,%g,%g, GL error %s",
      (double)g_in, (double)l[0], (double)l[1], (double)l[2], (double)l[3], (double)r[0], (double)r[1], (double)r[2], (double)r[3], errname(e));
    rm_target(&t);
}

static void g_float_rt(void)
{
    NEED_BASICS("float-rt-rgba16f");
    float_rt("float-rt-rgba16f", GL_RGBA16F, 4, 1);
    float_rt("float-rt-rgba32f", GL_RGBA32F, 4, 1);
    float_rt("float-rt-rg16f", GL_RG16F, 2, 1);
    float_rt("float-rt-r32f", GL_R32F, 1, 1);
    float_rt("float-rt-r11g11b10f", GL_R11F_G11F_B10F, 3, 0);

    target t = mk_target(GL_RGBA16F);
    GLint rf = 0, rt = 0;
    uint16_t h[4] = {0, 0, 0, 0};
    clearc(1.0f, 2.0f, -2.0f, 0.5f);
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &rf);
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &rt);
    glerr();
    glReadPixels(LX, MY, 1, 1, GL_RGBA, GL_HALF_FLOAT, h);
    const GLenum e = glerr();
    R("readpixels-float", 1, "RGBA16F: GL_IMPLEMENTATION_COLOR_READ_FORMAT 0x%04x TYPE 0x%04x; glReadPixels(GL_RGBA, GL_HALF_FLOAT) %s: 0x%04x 0x%04x 0x%04x 0x%04x (1, 2, -2, 0.5 are 3c00 4000 c000 3800); GL_RGBA/GL_FLOAT is checked by the float-rt results",
      rf, rt, errname(e), h[0], h[1], h[2], h[3]);
    rm_target(&t);
}

/* ---- depth and stencil -------------------------------------------------- */

static void depth_stencil(const char *name, GLenum fmt, int has_depth, int has_stencil)
{
    target t = mk_target(GL_RGBA8);
    GLuint ds = mk_tex(fmt, W, H);
    glBindTexture(GL_TEXTURE_2D, 0);
    GLenum e = glerr();
    const GLenum attachment = has_depth && has_stencil ? GL_DEPTH_STENCIL_ATTACHMENT :
                              has_depth ? GL_DEPTH_ATTACHMENT : GL_STENCIL_ATTACHMENT;
    glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, ds, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE || e) {
        R(name, 0, "glTexStorage2D %s, RGBA8 + this as a framebuffer: 0x%04x", errname(e), status);
        glDeleteTextures(1, &ds);
        rm_target(&t);
        return;
    }
    /* Clears as CommandBufferGL.cpp:1443-1449 does them. */
    clearc(0, 0, 1, 1);
    const GLfloat one = 1.0f;
    const GLint zero = 0;
    if (has_depth && has_stencil)
        glClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0);
    else if (has_depth)
        glClearBufferfv(GL_DEPTH, 0, &one);
    else
        glClearBufferiv(GL_STENCIL, 0, &zero);
    if (has_depth)
        glEnable(GL_DEPTH_TEST);
    if (has_stencil) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_ALWAYS, 0x5A, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    }
    /* Red, near (window depth 0.25), left half; writes stencil 0x5A. */
    solid(0.5f, 1, -0.5f, 0, -0.5f, 1, 0, 0, 1);
    if (has_stencil)
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    /* Green, far (0.75), everywhere: the depth test keeps red on the left. */
    solid(1, 1, 0, 0, 0.5f, 0, 1, 0, 1);
    uint8_t l[4], r[4];
    rd(LX, MY, l);
    rd(RX, MY, r);
    const int depth_ok = has_depth ? near4(l, 255, 0, 0, 255, 0) && near4(r, 0, 255, 0, 255, 0) :
                                     near4(l, 0, 255, 0, 255, 0) && near4(r, 0, 255, 0, 255, 0);
    int stencil_ok = 1;
    uint8_t sl[4] = {0, 0, 0, 0}, sr[4] = {0, 0, 0, 0};
    if (has_stencil) {
        /* White where stencil == 0x5A: the left half only. */
        glDisable(GL_DEPTH_TEST);
        glStencilFunc(GL_EQUAL, 0x5A, 0xff);
        solid(1, 1, 0, 0, 0, 1, 1, 1, 1);
        rd(LX, MY, sl);
        rd(RX, MY, sr);
        stencil_ok = near4(sl, 255, 255, 255, 255, 0) && near4(sr, 0, 255, 0, 255, 0);
    }
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    e = glerr();

    /* Sampled: depth as a float texture, stencil through
     * GL_DEPTH_STENCIL_TEXTURE_MODE (CommandBufferGL.cpp:459). */
    target out = mk_target(GL_RGBA8);
    uint8_t dl[4] = {0, 0, 0, 0}, dr[4] = {0, 0, 0, 0}, ul[4] = {0, 0, 0, 0}, ur[4] = {0, 0, 0, 0};
    int sample_depth_ok = 1, sample_stencil_ok = 1;
    GLenum se = GL_NO_ERROR, ue = GL_NO_ERROR;
    if (has_depth) {
        clearc(0, 0, 0, 0);
        blit_tex(g_tex, GL_TEXTURE_2D, ds);
        rd(LX, MY, dl);
        rd(RX, MY, dr);
        se = glerr();
        sample_depth_ok = abs(dl[0] - 64) <= 2 && abs(dr[0] - 191) <= 2 && !se;
    }
    if (has_stencil) {
        glBindTexture(GL_TEXTURE_2D, ds);
        if (has_depth)
            glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);
        clearc(0, 0, 0, 0);
        blit_tex(g_utex, GL_TEXTURE_2D, ds);
        rd(LX, MY, ul);
        rd(RX, MY, ur);
        ue = glerr();
        sample_stencil_ok = ul[0] == 0x5A && ur[0] == 0 && !ue;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    R(name, depth_ok && stencil_ok && sample_depth_ok && sample_stencil_ok && !e,
      "attached to an RGBA8 FBO: depth test %s (left %d,%d,%d right %d,%d,%d), stencil test %s (left %d,%d,%d right %d,%d,%d), sampled depth %s (%d / %d, want 64 / 191, %s), sampled stencil %s (%d / %d, want 90 / 0, %s), GL error %s",
      !has_depth ? "n/a" : depth_ok ? "ok" : "WRONG", l[0], l[1], l[2], r[0], r[1], r[2],
      !has_stencil ? "n/a" : stencil_ok ? "ok" : "WRONG", sl[0], sl[1], sl[2], sr[0], sr[1], sr[2],
      !has_depth ? "n/a" : sample_depth_ok ? "ok" : "WRONG", dl[0], dr[0], errname(se),
      !has_stencil ? "n/a" : sample_stencil_ok ? "ok" : "WRONG", ul[0], ur[0], errname(ue), errname(e));
    rm_target(&out);
    glDeleteTextures(1, &ds);
    rm_target(&t);
}

static void g_depth(void)
{
    NEED_BASICS("depth-stencil-d16");
    depth_stencil("depth-stencil-d16", GL_DEPTH_COMPONENT16, 1, 0);
    depth_stencil("depth-stencil-d24", GL_DEPTH_COMPONENT24, 1, 0);
    depth_stencil("depth-stencil-d24s8", GL_DEPTH24_STENCIL8, 1, 1);
    depth_stencil("depth-stencil-d32f", GL_DEPTH_COMPONENT32F, 1, 0);
    depth_stencil("depth-stencil-d32fs8", GL_DEPTH32F_STENCIL8, 1, 1);
    depth_stencil("depth-stencil-s8", GL_STENCIL_INDEX8, 0, 1);
}

/* ---- instancing, base vertex, base instance, MRT ------------------------ */

static const char kVsInst[] =
    "#version 310 es\n"
    "layout(location = 0) in vec2 pos; layout(location = 1) in vec4 icol; layout(location = 2) in float ioff;\n"
    "out vec4 c;\n"
    "void main() { c = icol; gl_Position = vec4(pos.x * 0.5 + ioff, pos.y, 0.0, 1.0); }\n";
static const char kFsVary[] =
    "#version 310 es\nprecision highp float;\nin vec4 c;\nout vec4 o;\nvoid main() { o = c; }\n";
static const char kFsMrt[] =
    "#version 310 es\nprecision highp float;\n"
    "layout(location = 0) out vec4 o0; layout(location = 1) out vec4 o1; layout(location = 2) out vec4 o2; layout(location = 3) out vec4 o3;\n"
    "void main() { o0 = vec4(1.0, 0.0, 0.0, 1.0); o1 = vec4(0.0, 1.0, 0.0, 1.0); o2 = vec4(0.0, 0.0, 1.0, 1.0); o3 = vec4(1.0, 1.0, 0.0, 1.0); }\n";

static void g_draws(void)
{
    NEED_BASICS("draw-instanced-divisor");
    uint8_t l[4], r[4];
    target t = mk_target(GL_RGBA8);
    GLuint inst = mk_program(kVsInst, kFsVary, g_log, sizeof g_log);
    if (!inst) {
        R("draw-instanced-divisor", 0, "instancing program did not build: %s", g_log);
        rm_target(&t);
        return;
    }
    /* Vertices 0-5 degenerate, 6-11 the full quad: only a base vertex of 6
     * draws anything. Per-instance: colour + x offset. */
    static const GLfloat verts[24] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
    static const GLfloat instances[15] = {1, 0, 0, 1, -0.5f, 0, 1, 0, 1, 0.5f, 1, 1, 0, 1, 0.5f};
    static const GLushort indices[6] = {0, 1, 2, 3, 4, 5};
    GLuint vao = 0, vbo = 0, ibo = 0, ebo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    vbo = mk_buffer(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *)(uintptr_t)(12 * sizeof(GLfloat)));
    ibo = mk_buffer(GL_ARRAY_BUFFER, sizeof instances, instances, GL_STATIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), 0);
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), (const void *)(uintptr_t)(4 * sizeof(GLfloat)));
    glVertexAttribDivisor(2, 1);
    ebo = mk_buffer(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STATIC_DRAW);
    glUseProgram(inst);

    /* What Dawn issues (CommandBufferGL.cpp:1473, 1495): instanced draws,
     * base vertex folded into the attribute offsets (:276), as above. */
    clearc(0, 0, 1, 1);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, 2);
    rd(LX, MY, l);
    rd(RX, MY, r);
    int ok = near4(l, 255, 0, 0, 255, 0) && near4(r, 0, 255, 0, 255, 0);
    clearc(0, 0, 1, 1);
    glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0, 2);
    uint8_t l2[4], r2[4];
    rd(LX, MY, l2);
    rd(RX, MY, r2);
    GLenum e = glerr();
    ok = ok && near4(l2, 255, 0, 0, 255, 0) && near4(r2, 0, 255, 0, 255, 0);
    R("draw-instanced-divisor", ok && !e, "glDrawArraysInstanced x2 with glVertexAttribDivisor colour + offset: left %d,%d,%d right %d,%d,%d; glDrawElementsInstanced: left %d,%d,%d right %d,%d,%d (want red, green), GL error %s",
      l[0], l[1], l[2], r[0], r[1], r[2], l2[0], l2[1], l2[2], r2[0], r2[1], r2[2], errname(e));

    /* The ES 3.2 entry points themselves (Dawn does not call them). */
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    clearc(0, 0, 1, 1);
    glDrawElementsInstancedBaseVertex(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0, 2, 6);
    rd(LX, MY, l);
    rd(RX, MY, r);
    e = glerr();
    R("draw-elements-instanced-base-vertex", near4(l, 255, 0, 0, 255, 0) && near4(r, 0, 255, 0, 255, 0) && !e,
      "glDrawElementsInstancedBaseVertex(6 indices, 2 instances, base vertex 6): left %d,%d,%d right %d,%d,%d (want red, green; blue means the base vertex was ignored), GL error %s",
      l[0], l[1], l[2], r[0], r[1], r[2], errname(e));

    /* EXT_base_instance: instance 2 only (yellow, right). */
    typedef void (*base_instance_fn)(GLenum, GLint, GLsizei, GLsizei, GLuint);
    base_instance_fn base_instance = (base_instance_fn)gp("glDrawArraysInstancedBaseInstanceEXT");
    if (!has_ext("GL_EXT_base_instance") || !base_instance) {
        RSKIP("base-instance", "GL_EXT_base_instance %s, glDrawArraysInstancedBaseInstanceEXT %s, GL_ANGLE_base_vertex_base_instance %s (optional: Dawn only offers indirect-first-instance on desktop GL)",
              has_ext("GL_EXT_base_instance") ? "advertised" : "absent", base_instance ? "resolves" : "absent",
              has_ext("GL_ANGLE_base_vertex_base_instance") ? "advertised" : "absent");
    } else {
        clearc(0, 0, 1, 1);
        base_instance(GL_TRIANGLES, 6, 6, 1, 2);
        rd(LX, MY, l);
        rd(RX, MY, r);
        e = glerr();
        R("base-instance", near4(l, 0, 0, 255, 255, 0) && near4(r, 255, 255, 0, 255, 0) && !e,
          "glDrawArraysInstancedBaseInstanceEXT(1 instance, base instance 2): left %d,%d,%d right %d,%d,%d (want blue, yellow), GL error %s",
          l[0], l[1], l[2], r[0], r[1], r[2], errname(e));
    }
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(inst);

    /* glDrawElementsBaseVertex with the plain program. */
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    clearc(0, 0, 1, 1);
    set_xform(g_solid, 1, 1, 0, 0, 0);
    glUniform4f(glGetUniformLocation(g_solid, "color"), 1, 0, 0, 1);
    glDrawElementsBaseVertex(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0, 6);
    rd(LX, MY, l);
    e = glerr();
    R("draw-elements-base-vertex", near4(l, 255, 0, 0, 255, 0) && !e, "glDrawElementsBaseVertex(base vertex 6): %d,%d,%d (want red; blue means the base vertex was ignored), GL error %s",
      l[0], l[1], l[2], errname(e));
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &ibo);
    glDeleteBuffers(1, &ebo);
    rm_target(&t);

    /* Four colour attachments (the compatibility-mode minimum). */
    GLuint mrt = mk_program(kVs310, kFsMrt, g_log, sizeof g_log);
    if (!mrt) {
        R("mrt-4", 0, "four-output program did not build: %s", g_log);
        return;
    }
    GLuint fbo = 0, tex[4];
    static const GLenum bufs[4] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    for (int i = 0; i < 4; i++) {
        tex[i] = mk_tex(GL_RGBA8, W, H);
        glFramebufferTexture2D(GL_FRAMEBUFFER, bufs[i], GL_TEXTURE_2D, tex[i], 0);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glDrawBuffers(4, bufs);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glViewport(0, 0, W, H);
    clearc(0, 0, 0, 0);
    set_xform(mrt, 1, 1, 0, 0, 0);
    glBindVertexArray(g_quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    uint8_t p[4][4];
    static const int want[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}};
    ok = 1;
    for (int i = 0; i < 4; i++) {
        glReadBuffer(bufs[i]);
        rd(LX, MY, p[i]);
        ok = ok && near4(p[i], want[i][0], want[i][1], want[i][2], 255, 0);
    }
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    e = glerr();
    R("mrt-4", ok && status == GL_FRAMEBUFFER_COMPLETE && !e, "glDrawBuffers(4): %d,%d,%d %d,%d,%d %d,%d,%d %d,%d,%d (want red green blue yellow), fbo 0x%04x, GL error %s",
      p[0][0], p[0][1], p[0][2], p[1][0], p[1][1], p[1][2], p[2][0], p[2][1], p[2][2], p[3][0], p[3][1], p[3][2], status, errname(e));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(4, tex);
    glDeleteProgram(mrt);
}

/* ---- clip distances ----------------------------------------------------- */

#define P_GL_CLIP_DISTANCE0_EXT 0x3000

static const char kVsClip[] =
    "#version 310 es\n#extension GL_EXT_clip_cull_distance : require\n"
    "layout(location = 0) in vec2 pos;\n"
    "void main() { gl_Position = vec4(pos, 0.0, 1.0); gl_ClipDistance[0] = pos.x; }\n";

static void g_clip(void)
{
    NEED_BASICS("clip-distance");
    if (!has_ext("GL_EXT_clip_cull_distance")) {
        RSKIP("clip-distance", "GL_EXT_clip_cull_distance absent (optional: WebGPU's clip-distances feature)");
        return;
    }
    GLuint prog = mk_program(kVsClip, kFsSolid, g_log, sizeof g_log);
    if (!prog) {
        R("clip-distance", 0, "GL_EXT_clip_cull_distance advertised, gl_ClipDistance program did not build: %s", g_log);
        return;
    }
    target t = mk_target(GL_RGBA8);
    uint8_t l[4], r[4];
    clearc(0, 0, 1, 1);
    glUseProgram(prog);
    glUniform4f(glGetUniformLocation(prog, "color"), 1, 0, 0, 1);
    glEnable(P_GL_CLIP_DISTANCE0_EXT);
    glBindVertexArray(g_quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(P_GL_CLIP_DISTANCE0_EXT);
    const GLenum e = glerr();
    rd(LX, MY, l);
    rd(RX, MY, r);
    R("clip-distance", near4(l, 0, 0, 255, 255, 0) && near4(r, 255, 0, 0, 255, 0) && !e, "gl_ClipDistance[0] = x with GL_CLIP_DISTANCE0_EXT: left %d,%d,%d right %d,%d,%d (want blue, red), GL error %s",
      l[0], l[1], l[2], r[0], r[1], r[2], errname(e));
    glDeleteProgram(prog);
    rm_target(&t);
}

/* ---- BGRA8 -------------------------------------------------------------- */

#define P_GL_BGRA_EXT 0x80E1
#define P_GL_BGRA8_EXT 0x93A1

static void g_bgra(void)
{
    NEED_BASICS("bgra8");
    if (!has_ext("GL_EXT_texture_format_BGRA8888") && !has_ext("GL_APPLE_texture_format_BGRA8888")) {
        R("bgra8", 0, "neither GL_EXT_texture_format_BGRA8888 nor the APPLE one: Dawn falls back to GL_RGBA8 with no swizzle on ES (GLFormat.cpp:34), so bgra8unorm would come out with red and blue swapped");
        return;
    }
    /* B, G, R, A bytes. */
    static uint8_t bgra[W * H * 4];
    for (int i = 0; i < W * H; i++) {
        bgra[i * 4] = 10;
        bgra[i * 4 + 1] = 20;
        bgra[i * 4 + 2] = 30;
        bgra[i * 4 + 3] = 255;
    }
    /* Immutable storage (needs EXT_texture_storage for this format,
     * GLFormat.cpp:52), else glTexImage2D. */
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glerr();
    glTexStorage2D(GL_TEXTURE_2D, 1, P_GL_BGRA8_EXT, W, H);
    const GLenum storage = glerr();
    GLenum image = GL_NO_ERROR;
    if (storage) {
        glTexImage2D(GL_TEXTURE_2D, 0, P_GL_BGRA_EXT, W, H, 0, P_GL_BGRA_EXT, GL_UNSIGNED_BYTE, NULL);
        image = glerr();
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, P_GL_BGRA_EXT, GL_UNSIGNED_BYTE, bgra);
    const GLenum upload = glerr();
    /* Sampled into RGBA8: red must be 30. */
    target t = mk_target(GL_RGBA8);
    uint8_t s[4], d[4], b[4] = {0, 0, 0, 0};
    clearc(0, 0, 0, 0);
    blit_tex(g_tex, GL_TEXTURE_2D, tex);
    rd(LX, MY, s);
    /* As a render target: pure red, read as RGBA and (EXT_read_format_bgra) BGRA. */
    GLuint fbo = mk_fbo(tex);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    clearc(1, 0, 0, 1);
    rd(LX, MY, d);
    glerr();
    glReadPixels(LX, MY, 1, 1, P_GL_BGRA_EXT, GL_UNSIGNED_BYTE, b);
    const GLenum read_bgra = glerr();
    R("bgra8", !upload && near4(s, 30, 20, 10, 255, 0) && status == GL_FRAMEBUFFER_COMPLETE && near4(d, 255, 0, 0, 255, 0),
      "glTexStorage2D(GL_BGRA8_EXT) %s%s%s; glTexSubImage2D(GL_BGRA_EXT) %s; sampled %d,%d,%d,%d (want 30,20,10,255); as a render target fbo 0x%04x, cleared red reads %d,%d,%d,%d as RGBA; glReadPixels(GL_BGRA_EXT) %s -> %d,%d,%d,%d (want 0,0,255,255; GL_EXT_read_format_bgra %s, GL_EXT_texture_storage %s)",
      errname(storage), storage ? ", glTexImage2D(GL_BGRA_EXT) " : "", storage ? errname(image) : "", errname(upload),
      s[0], s[1], s[2], s[3], status, d[0], d[1], d[2], d[3], errname(read_bgra), b[0], b[1], b[2], b[3],
      has_ext("GL_EXT_read_format_bgra") ? "advertised" : "absent", has_ext("GL_EXT_texture_storage") ? "advertised" : "absent");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    rm_target(&t);
}

/* ---- compressed textures ------------------------------------------------ */

static void setbits(uint8_t *b, int pos, int n, unsigned v)
{
    for (int i = 0; i < n; i++)
        if (v & (1u << i))
            b[(pos + i) / 8] |= (uint8_t)(1u << ((pos + i) % 8));
}

static void compressed(const char *name, GLenum fmt, const uint8_t *block, int bytes, int r, int g, int b, int tol)
{
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glerr();
    /* TextureGL.cpp: immutable storage, then glCompressedTexSubImage2D. */
    glTexStorage2D(GL_TEXTURE_2D, 1, fmt, 4, 4);
    const GLenum storage = glerr();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, fmt, bytes, block);
    const GLenum upload = glerr();
    if (storage || upload) {
        R(name, 0, "format 0x%04x: glTexStorage2D %s, glCompressedTexSubImage2D %s", fmt, errname(storage), errname(upload));
    } else {
        target t = mk_target(GL_RGBA8);
        uint8_t p[4];
        clearc(0, 0, 1, 0);
        blit_tex(g_tex, GL_TEXTURE_2D, tex);
        rd(LX, MY, p);
        const GLenum e = glerr();
        R(name, near4(p, r, g, b, 255, tol) && !e, "format 0x%04x: one 4x4 block, sampled: %d,%d,%d,%d (want %d,%d,%d,255), GL error %s", fmt, p[0], p[1], p[2], p[3], r, g, b, errname(e));
        rm_target(&t);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &tex);
}

static void g_compressed(void)
{
    NEED_BASICS("compressed-bc1");
    /* Single-colour blocks, by hand. BC1: colour0 = colour1 = 0xF800 (red). */
    static const uint8_t bc1[8] = {0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0};
    /* BC3: alpha 255/255 + the BC1 colour block. */
    static const uint8_t bc3[16] = {255, 255, 0, 0, 0, 0, 0, 0, 0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0};
    /* BC4: red0 = red1 = 255. BC5: the same twice. */
    static const uint8_t bc5[16] = {255, 255, 0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0};
    /* BC7 mode 6: endpoints (127,0,0,127) with p-bit 1: 255,1,1,255. */
    uint8_t bc7[16] = {0};
    setbits(bc7, 6, 1, 1);
    setbits(bc7, 7, 7, 127);
    setbits(bc7, 14, 7, 127);
    setbits(bc7, 49, 7, 127);
    setbits(bc7, 56, 7, 127);
    setbits(bc7, 63, 1, 1);
    setbits(bc7, 64, 1, 1);
    /* ETC2 RGB8, individual mode, both base colours (15,0,0), table 0,
     * every pixel index 0 (+2): 255,2,2. */
    static const uint8_t etc2[8] = {0xFF, 0x00, 0x00, 0x00, 0, 0, 0, 0};
    /* EAC alpha: base 255, multiplier 0; then the RGB block. */
    static const uint8_t etc2a[16] = {255, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0x00, 0x00, 0x00, 0, 0, 0, 0};
    /* ASTC void-extent block, RGBA16 = (ffff, 0, 0, ffff). */
    static const uint8_t astc[16] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF};
    compressed("compressed-bc1", 0x83F1, bc1, 8, 255, 0, 0, 8);       /* COMPRESSED_RGBA_S3TC_DXT1_EXT */
    compressed("compressed-bc1-srgb", 0x8C4D, bc1, 8, 255, 0, 0, 8);  /* COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT */
    compressed("compressed-bc3", 0x83F3, bc3, 16, 255, 0, 0, 8);      /* COMPRESSED_RGBA_S3TC_DXT5_EXT */
    compressed("compressed-bc4", 0x8DBB, bc5, 8, 255, 0, 0, 8);       /* COMPRESSED_RED_RGTC1_EXT */
    compressed("compressed-bc5", 0x8DBD, bc5, 16, 255, 255, 0, 8);    /* COMPRESSED_RED_GREEN_RGTC2_EXT */
    compressed("compressed-bc7", 0x8E8C, bc7, 16, 255, 1, 1, 8);      /* COMPRESSED_RGBA_BPTC_UNORM_EXT */
    compressed("compressed-etc2-rgb8", 0x9274, etc2, 8, 255, 2, 2, 8);   /* COMPRESSED_RGB8_ETC2 */
    compressed("compressed-etc2-rgba8", 0x9278, etc2a, 16, 255, 2, 2, 8); /* COMPRESSED_RGBA8_ETC2_EAC */
    compressed("compressed-astc-4x4", 0x93B0, astc, 16, 255, 0, 0, 8);   /* COMPRESSED_RGBA_ASTC_4x4_KHR */
}

/* ---- timer queries ------------------------------------------------------ */

#define P_GL_TIME_ELAPSED_EXT 0x88BF
#define P_GL_TIMESTAMP_EXT 0x8E28
#define P_GL_GPU_DISJOINT_EXT 0x8FBB
#define P_GL_QUERY_COUNTER_BITS_EXT 0x8864

static void g_timer(void)
{
    NEED_BASICS("timer-query");
    typedef void (*gen_fn)(GLsizei, GLuint *);
    typedef void (*begin_fn)(GLenum, GLuint);
    typedef void (*end_fn)(GLenum);
    typedef void (*getuiv_fn)(GLuint, GLenum, GLuint *);
    typedef void (*getui64_fn)(GLuint, GLenum, GLuint64 *);
    typedef void (*getqiv_fn)(GLenum, GLenum, GLint *);
    typedef void (*counter_fn)(GLuint, GLenum);
    gen_fn gen = (gen_fn)gp("glGenQueriesEXT");
    begin_fn begin = (begin_fn)gp("glBeginQueryEXT");
    end_fn end = (end_fn)gp("glEndQueryEXT");
    getuiv_fn getuiv = (getuiv_fn)gp("glGetQueryObjectuivEXT");
    getui64_fn getui64 = (getui64_fn)gp("glGetQueryObjectui64vEXT");
    getqiv_fn getqiv = (getqiv_fn)gp("glGetQueryivEXT");
    counter_fn counter = (counter_fn)gp("glQueryCounterEXT");
    if (!has_ext("GL_EXT_disjoint_timer_query") || !gen || !begin || !end || !getuiv || !getui64 || !getqiv || !counter) {
        RSKIP("timer-query", "GL_EXT_disjoint_timer_query %s; entry points %s (optional: WebGPU's timestamp-query; Dawn's GL backend does not implement it)",
              has_ext("GL_EXT_disjoint_timer_query") ? "advertised" : "absent",
              gen && begin && end && getuiv && getui64 && getqiv && counter ? "resolve" : "do not all resolve");
        return;
    }
    target t = mk_target(GL_RGBA8);
    GLuint q[2] = {0, 0}, available = 0;
    GLuint64 elapsed = 0, stamp = 0;
    GLint bits_elapsed = 0, bits_stamp = 0, disjoint = 0;
    glerr();
    getqiv(P_GL_TIME_ELAPSED_EXT, P_GL_QUERY_COUNTER_BITS_EXT, &bits_elapsed);
    getqiv(P_GL_TIMESTAMP_EXT, P_GL_QUERY_COUNTER_BITS_EXT, &bits_stamp);
    gen(2, q);
    begin(P_GL_TIME_ELAPSED_EXT, q[0]);
    clearc(0, 0, 0, 1);
    for (int i = 0; i < 200; i++)
        solid(1, 1, 0, 0, 0, 1, 1, 1, 1);
    end(P_GL_TIME_ELAPSED_EXT);
    counter(q[1], P_GL_TIMESTAMP_EXT);
    glFinish();
    const double t0 = probe_now_ms();
    while (!available && probe_now_ms() - t0 < 1000) {
        getuiv(q[0], GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available)
            glFlush();
    }
    if (available) {
        getui64(q[0], GL_QUERY_RESULT, &elapsed);
        getui64(q[1], GL_QUERY_RESULT, &stamp);
    }
    glGetIntegerv(P_GL_GPU_DISJOINT_EXT, &disjoint);
    const GLenum e = glerr();
    R("timer-query", available && elapsed > 0 && !e, "GL_TIME_ELAPSED_EXT around 200 draws: %s, %llu ns (counter bits %d); glQueryCounterEXT(GL_TIMESTAMP_EXT) %llu ns (bits %d); GL_GPU_DISJOINT_EXT %d, GL error %s",
      available ? "available" : "NOT available after 1 s", (unsigned long long)elapsed, bits_elapsed, (unsigned long long)stamp, bits_stamp, disjoint, errname(e));
    rm_target(&t);
}

/* ---- primitive restart -------------------------------------------------- */

static void g_restart(void)
{
    NEED_BASICS("primitive-restart");
    /* Two triangles, far left and far right, as one strip with a restart
     * between them. If the restart index is dropped instead of honoured, the
     * strip's joining triangles cover the middle. */
    static const GLfloat verts[12] = {-0.9f, -0.5f, -0.9f, 0.5f, -0.5f, 0.0f, 0.5f, 0.0f, 0.9f, 0.5f, 0.9f, -0.5f};
    static const GLushort i16[7] = {0, 1, 2, 0xFFFF, 3, 4, 5};
    static const GLuint i32[7] = {0, 1, 2, 0xFFFFFFFFu, 3, 4, 5};
    target t = mk_target(GL_RGBA8);
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    GLuint vbo = mk_buffer(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    GLuint e16 = mk_buffer(GL_ELEMENT_ARRAY_BUFFER, sizeof i16, i16, GL_STATIC_DRAW);
    set_xform(g_solid, 1, 1, 0, 0, 0);
    glUniform4f(glGetUniformLocation(g_solid, "color"), 1, 0, 0, 1);
    glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX); /* CommandBufferGL.cpp:1487 */
    uint8_t a[2][4], m[2][4], b[2][4];
    clearc(0, 0, 1, 1);
    glDrawElements(GL_TRIANGLE_STRIP, 7, GL_UNSIGNED_SHORT, 0);
    /* NDC (-0.8, 0) -> pixel 3; (0.2, 0.03) -> pixel 19; (0.8, 0) -> 28. */
    rd(3, MY, a[0]);
    rd(19, MY, m[0]);
    rd(28, MY, b[0]);
    GLuint e32 = mk_buffer(GL_ELEMENT_ARRAY_BUFFER, sizeof i32, i32, GL_STATIC_DRAW);
    clearc(0, 0, 1, 1);
    glDrawElements(GL_TRIANGLE_STRIP, 7, GL_UNSIGNED_INT, 0);
    rd(3, MY, a[1]);
    rd(19, MY, m[1]);
    rd(28, MY, b[1]);
    glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    const GLenum e = glerr();
    int ok = !e;
    for (int i = 0; i < 2; i++)
        ok = ok && near4(a[i], 255, 0, 0, 255, 0) && near4(m[i], 0, 0, 255, 255, 0) && near4(b[i], 255, 0, 0, 255, 0);
    R("primitive-restart", ok, "GL_PRIMITIVE_RESTART_FIXED_INDEX, strip 0 1 2 R 3 4 5: uint16 left %d,%d,%d middle %d,%d,%d right %d,%d,%d; uint32 left %d,%d,%d middle %d,%d,%d right %d,%d,%d (want red, blue, red), GL error %s",
      a[0][0], a[0][1], a[0][2], m[0][0], m[0][1], m[0][2], b[0][0], b[0][1], b[0][2],
      a[1][0], a[1][1], a[1][2], m[1][0], m[1][1], m[1][2], b[1][0], b[1][1], b[1][2], errname(e));
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &e16);
    glDeleteBuffers(1, &e32);
    rm_target(&t);
}

/* ---- sRGB --------------------------------------------------------------- */

static void g_srgb(void)
{
    NEED_BASICS("srgb-framebuffer");
    /* On ES there is no GL_FRAMEBUFFER_SRGB switch (DeviceGL.cpp:200-205):
     * writes to an sRGB attachment are always encoded, reads decoded. Linear
     * 0.5 is 188 encoded. */
    target s = mk_target(GL_SRGB8_ALPHA8), out = mk_target(GL_RGBA8);
    uint8_t drawn[4], cleared[4], sampled[4], blitted[4];
    use_target(&s);
    clearc(0.5f, 0.5f, 0.5f, 1);
    rd(RX, MY, cleared);
    solid(0.5f, 1, -0.5f, 0, 0, 0.5f, 0.5f, 0.5f, 1);
    rd(LX, MY, drawn);
    use_target(&out);
    clearc(0, 0, 0, 0);
    blit_tex(g_tex, GL_TEXTURE_2D, s.tex);
    rd(LX, MY, sampled);
    glBindTexture(GL_TEXTURE_2D, 0);
    clearc(0, 0, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, out.fbo);
    glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    const GLenum be = glerr();
    glBindFramebuffer(GL_FRAMEBUFFER, out.fbo);
    rd(LX, MY, blitted);
    const GLenum e = glerr();
    R("srgb-framebuffer", s.status == GL_FRAMEBUFFER_COMPLETE && abs(drawn[0] - 188) <= 2 && abs(cleared[0] - 188) <= 2 && abs(sampled[0] - 128) <= 2 && !e,
      "SRGB8_ALPHA8 target: draw of linear 0.5 stores %d, clear stores %d (want 188: encoded); sampled back %d (want 127 or 128: decoded); glBlitFramebuffer to RGBA8 gives %d (%s; 128 = converted, 188 = raw copy); GL_EXT_sRGB_write_control %s, GL error %s",
      drawn[0], cleared[0], sampled[0], blitted[0], errname(be), has_ext("GL_EXT_sRGB_write_control") ? "advertised" : "absent", errname(e));
    rm_target(&s);
    rm_target(&out);
}

/* ---- uniform buffers in a draw ------------------------------------------ */

static const char kFsUbo[] =
    "#version 310 es\nprecision highp float;\n"
    "layout(std140, binding = 3) uniform U { float a; vec3 b; vec2 c; float arr[2]; } u;\n"
    "out vec4 o;\nvoid main() { o = vec4(u.a, u.b.y, u.c.x, u.arr[1]); }\n";

static void g_ubo(void)
{
    NEED_BASICS("ubo-std140");
    GLuint prog = mk_program(kVs310, kFsUbo, g_log, sizeof g_log);
    if (!prog) {
        R("ubo-std140", 0, "uniform block program did not build: %s", g_log);
        return;
    }
    /* std140: a@0, b@16, c@32, arr@48 (stride 16), size 80. */
    static const char *const names[4] = {"U.a", "U.b", "U.c", "U.arr[0]"};
    GLuint idx[4] = {0, 0, 0, 0};
    GLint offs[4] = {-1, -1, -1, -1}, size = 0, stride = 0;
    glGetUniformIndices(prog, 4, names, idx);
    glGetActiveUniformsiv(prog, 4, idx, GL_UNIFORM_OFFSET, offs);
    glGetActiveUniformsiv(prog, 1, &idx[3], GL_UNIFORM_ARRAY_STRIDE, &stride);
    glGetActiveUniformBlockiv(prog, glGetUniformBlockIndex(prog, "U"), GL_UNIFORM_BLOCK_DATA_SIZE, &size);
    GLint align = 1;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
    const unsigned off = (unsigned)(align > 256 ? align : 256);
    float *data = calloc(1, off + 80);
    float *u = (float *)((uint8_t *)data + off);
    for (unsigned i = 0; i < off / 4; i++)
        data[i] = 0.9f; /* what a binding that ignored the offset would read */
    u[0] = 0.25f;
    u[5] = 0.5f;
    u[8] = 0.75f;
    u[16] = 1.0f;
    GLuint ubo = mk_buffer(GL_UNIFORM_BUFFER, off + 80, data, GL_DYNAMIC_DRAW);
    glBindBufferRange(GL_UNIFORM_BUFFER, 3, ubo, off, 80);
    target t = mk_target(GL_RGBA8);
    uint8_t p[4], q[4];
    clearc(0, 0, 0, 0);
    set_xform(prog, 1, 1, 0, 0, 0);
    glBindVertexArray(g_quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    rd(LX, MY, p);
    /* Update in place (glBufferSubData) and draw again: no stale copy. */
    const float half = 0.5f;
    glBufferSubData(GL_UNIFORM_BUFFER, off, 4, &half);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    rd(LX, MY, q);
    const GLenum e = glerr();
    R("ubo-std140", near4(p, 64, 128, 191, 255, 1) && near4(q, 128, 128, 191, 255, 1) && offs[0] == 0 && offs[1] == 16 && offs[2] == 32 && offs[3] == 48 && !e,
      "std140 {float a; vec3 b; vec2 c; float arr[2]} offsets %d,%d,%d,%d stride %d size %d (want 0,16,32,48 stride 16 size 80); bound with glBindBufferRange at %u: %d,%d,%d,%d (want 64,128,191,255); after glBufferSubData of a: %d (want 128), GL error %s",
      offs[0], offs[1], offs[2], offs[3], stride, size, off, p[0], p[1], p[2], p[3], q[0], errname(e));
    glDeleteBuffers(1, &ubo);
    glDeleteProgram(prog);
    free(data);
    rm_target(&t);
}

/* ---- pixel buffers ------------------------------------------------------ */

static void g_pbo(void)
{
    NEED_BASICS("readpixels-pbo");
    /* Texture-to-buffer copies are glReadPixels into GL_PIXEL_PACK_BUFFER
     * (CommandBufferGL.cpp:943); buffer-to-texture, glTexSubImage2D from
     * GL_PIXEL_UNPACK_BUFFER (:890). */
    target t = mk_target(GL_RGBA8);
    clearc(0, 0, 1, 1);
    solid(0.5f, 1, -0.5f, 0, 0, 1, 0, 0, 1);
    GLuint pack = mk_buffer(GL_PIXEL_PACK_BUFFER, W * H * 4, NULL, GL_STREAM_READ);
    glerr();
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    GLenum e = glerr();
    const uint8_t *m = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, W * H * 4, GL_MAP_READ_BIT);
    int wrong = W * H;
    if (m) {
        wrong = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                wrong += !near4(m + (y * W + x) * 4, x < 16 ? 255 : 0, 0, x < 16 ? 0 : 255, 255, 0);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    R("readpixels-pbo", m && !wrong && !e, "glReadPixels into a GL_PIXEL_PACK_BUFFER, mapped: %d of %d texels wrong, GL error %s", wrong, W * H, errname(e));
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glDeleteBuffers(1, &pack);
    rm_target(&t);

    static uint8_t px[W * H * 4];
    char what[160];
    pattern(px, W, H);
    GLuint unpack = mk_buffer(GL_PIXEL_UNPACK_BUFFER, sizeof px, px, GL_STATIC_DRAW);
    GLuint tex = mk_tex(GL_RGBA8, W, H);
    glerr();
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    e = glerr();
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    GLuint fbo = mk_fbo(tex);
    wrong = check_copy(W, H, 0, 0, W, H, 0, 0, what, sizeof what);
    R("texsubimage-pbo", !wrong && !e, "glTexSubImage2D from a GL_PIXEL_UNPACK_BUFFER, read through an FBO: %d of %d texels wrong %s, GL error %s", wrong, W * H, what, errname(e));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glDeleteBuffers(1, &unpack);
}

/* ---- compute feeding draws, and the rest of what Tint emits -------------- */

static const char kCsColour[] =
    "#version 310 es\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(rgba8, binding = 0) writeonly uniform highp image2D dst;\n"
    "uniform vec4 colour;\n"
    "void main() { imageStore(dst, ivec2(gl_GlobalInvocationID.xy), colour); }\n";

static const char kCsVerts[] =
    "#version 310 es\n"
    "layout(local_size_x = 1) in;\n"
    "layout(std430, binding = 0) buffer V { vec4 v[6]; } o;\n"
    "uniform float shift;\n"
    "void main() { const vec2 q[6] = vec2[6](vec2(0.0, -1.0), vec2(1.0, -1.0), vec2(0.0, 1.0), vec2(0.0, 1.0), vec2(1.0, -1.0), vec2(1.0, 1.0));\n"
    "  for (int i = 0; i < 6; i++) o.v[i] = vec4(q[i].x + shift, q[i].y, 0.0, 1.0); }\n";

static const char kFsSsbo[] =
    "#version 310 es\nprecision highp float;\n"
    "layout(std430, binding = 3) readonly buffer C { vec4 c[]; } s;\n"
    "out vec4 o;\nvoid main() { o = s.c[1] * float(s.c.length()) / 3.0; }\n";

static const char kCsSample[] =
    "#version 310 es\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "uniform highp sampler2D t;\n"
    "layout(std430, binding = 0) buffer Out { uint data[]; } o;\n"
    "void main() { ivec2 c = ivec2(gl_GlobalInvocationID.xy);\n"
    "  vec4 a = texelFetch(t, c, 0);\n"
    "  vec4 b = textureLod(t, (vec2(c) + 0.5) / 32.0, 0.0);\n"
    "  o.data[c.y * 32 + c.x] = uint(a.r * 255.0 + 0.5) | (uint(a.g * 255.0 + 0.5) << 8) | (uint(b.r * 255.0 + 0.5) << 16) | (uint(b.g * 255.0 + 0.5) << 24); }\n";

static const char kCsEight[] =
    "#version 310 es\n"
    "layout(local_size_x = 1) in;\n"
    "layout(std430, binding = 0) buffer B0 { uint v[]; } b0;\n"
    "layout(std430, binding = 1) buffer B1 { uint v[]; } b1;\n"
    "layout(std430, binding = 2) buffer B2 { uint v[]; } b2;\n"
    "layout(std430, binding = 3) buffer B3 { uint v[]; } b3;\n"
    "layout(std430, binding = 4) buffer B4 { uint v[]; } b4;\n"
    "layout(std430, binding = 5) buffer B5 { uint v[]; } b5;\n"
    "layout(std430, binding = 6) buffer B6 { uint v[]; } b6;\n"
    "layout(std430, binding = 7) buffer B7 { uint v[]; } b7;\n"
    "void main() { b7.v[2] = b0.v[1] + b1.v[1] + b2.v[1] + b3.v[1] + b4.v[1] + b5.v[1] + b6.v[1];\n"
    "  b0.v[0] = 100u; b1.v[0] = 101u; b2.v[0] = 102u; b3.v[0] = 103u; b4.v[0] = 104u; b5.v[0] = 105u; b6.v[0] = 106u; b7.v[0] = 107u; }\n";

static const char kCsShared[] =
    "#version 310 es\n"
    "layout(local_size_x = 64) in;\n"
    "layout(std430, binding = 0) buffer Out { uint data[]; } o;\n"
    "shared uint tmp[64];\n"
    "void main() { uint l = gl_LocalInvocationIndex;\n"
    "  tmp[l] = gl_GlobalInvocationID.x * 2u + 1u;\n"
    "  memoryBarrierShared(); barrier();\n"
    "  o.data[gl_GlobalInvocationID.x] = tmp[63u - l]; }\n";

static const char kCsAtomic[] =
    "#version 310 es\n"
    "layout(local_size_x = 64) in;\n"
    "layout(std430, binding = 0) buffer C { uint counter; uint highest; } c;\n"
    "void main() { atomicAdd(c.counter, 1u); atomicMax(c.highest, gl_GlobalInvocationID.x); }\n";

static void g_compute_more(void)
{
    NEED_BASICS("memory-barrier-texture");
    uint8_t l[4], r[4], l2[4];
    target t = mk_target(GL_RGBA8);
    GLenum e;

    /* A texture written by compute, sampled by a draw, written again,
     * sampled again. */
    GLuint colour = mk_compute(kCsColour, g_log, sizeof g_log);
    if (!colour) {
        R("memory-barrier-texture", 0, "imageStore program did not build: %s", g_log);
    } else {
        GLuint tex = mk_tex(GL_RGBA8, W, H);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(colour);
        glBindImageTexture(0, tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glUniform4f(glGetUniformLocation(colour, "colour"), 0, 1, 0, 1);
        glDispatchCompute(4, 4, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        use_target(&t);
        clearc(0, 0, 0, 0);
        blit_tex(g_tex, GL_TEXTURE_2D, tex);
        rd(LX, MY, l);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(colour);
        glUniform4f(glGetUniformLocation(colour, "colour"), 1, 0, 0, 1);
        glDispatchCompute(4, 4, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        blit_tex(g_tex, GL_TEXTURE_2D, tex);
        rd(LX, MY, l2);
        e = glerr();
        R("memory-barrier-texture", near4(l, 0, 255, 0, 255, 0) && near4(l2, 255, 0, 0, 255, 0) && !e,
          "imageStore green, glMemoryBarrier(GL_ALL_BARRIER_BITS), sampled by a draw: %d,%d,%d; imageStore red over it, sampled again: %d,%d,%d (want green then red), GL error %s",
          l[0], l[1], l[2], l2[0], l2[1], l2[2], errname(e));
        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteTextures(1, &tex);
        glDeleteProgram(colour);
    }

    /* A storage buffer written by compute, then the vertex buffer of a draw. */
    GLuint verts = mk_compute(kCsVerts, g_log, sizeof g_log);
    if (!verts) {
        R("compute-to-vertex-buffer", 0, "compute program did not build: %s", g_log);
    } else {
        GLuint vao = 0, buf = mk_buffer(GL_SHADER_STORAGE_BUFFER, 6 * 16, NULL, GL_DYNAMIC_COPY);
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, buf);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buf);
        glUseProgram(verts);
        glUniform1f(glGetUniformLocation(verts, "shift"), 0.0f);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        use_target(&t);
        clearc(0, 0, 1, 1);
        set_xform(g_solid, 1, 1, 0, 0, 0);
        glUniform4f(glGetUniformLocation(g_solid, "color"), 1, 0, 0, 1);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        rd(LX, MY, l);
        rd(RX, MY, r);
        /* Move the quad to the left half and draw again. */
        glUseProgram(verts);
        glUniform1f(glGetUniformLocation(verts, "shift"), -1.0f);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        clearc(0, 0, 1, 1);
        glUseProgram(g_solid);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        uint8_t r2[4];
        rd(LX, MY, l2);
        rd(RX, MY, r2);
        e = glerr();
        R("compute-to-vertex-buffer", near4(l, 0, 0, 255, 255, 0) && near4(r, 255, 0, 0, 255, 0) && near4(l2, 255, 0, 0, 255, 0) && near4(r2, 0, 0, 255, 255, 0) && !e,
          "compute writes six vertices (right half) into a buffer drawn as GL_ARRAY_BUFFER: left %d,%d,%d right %d,%d,%d (want blue, red); rewritten to the left half: left %d,%d,%d right %d,%d,%d (want red, blue), GL error %s",
          l[0], l[1], l[2], r[0], r[1], r[2], l2[0], l2[1], l2[2], r2[0], r2[1], r2[2], errname(e));
        glBindVertexArray(0);
        glDeleteVertexArrays(1, &vao);
        glDeleteBuffers(1, &buf);
        glDeleteProgram(verts);
    }

    /* A storage buffer read in a fragment shader (compat: at least 4). */
    GLint fs_blocks = 0;
    glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, &fs_blocks);
    GLuint fs_ssbo = fs_blocks > 0 ? mk_program(kVs310, kFsSsbo, g_log, sizeof g_log) : 0;
    if (!fs_ssbo) {
        R("fragment-ssbo", 0, "GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS %d; program %s", fs_blocks, fs_blocks > 0 ? g_log : "not tried");
    } else {
        static const float colours[12] = {0, 0, 1, 1, 1, 1, 0, 1, 0, 1, 0, 1};
        GLuint buf = mk_buffer(GL_SHADER_STORAGE_BUFFER, sizeof colours, colours, GL_STATIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, buf);
        use_target(&t);
        clearc(0, 0, 0, 0);
        set_xform(fs_ssbo, 1, 1, 0, 0, 0);
        glBindVertexArray(g_quad_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        rd(LX, MY, l);
        e = glerr();
        R("fragment-ssbo", near4(l, 255, 255, 0, 255, 0) && !e, "fragment shader reads c[1] of a std430 storage buffer and its length (3): %d,%d,%d,%d (want 255,255,0,255), GL error %s (GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS %d)",
          l[0], l[1], l[2], l[3], errname(e), fs_blocks);
        glDeleteBuffers(1, &buf);
        glDeleteProgram(fs_ssbo);
    }
    rm_target(&t);

    /* texelFetch and textureLod in a compute shader. */
    GLuint sample = mk_compute(kCsSample, g_log, sizeof g_log);
    if (!sample) {
        R("compute-texture-sample", 0, "compute program did not build: %s", g_log);
    } else {
        static uint8_t px[W * H * 4];
        pattern(px, W, H);
        GLuint tex = mk_tex(GL_RGBA8, W, H);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
        GLuint buf = mk_buffer(GL_SHADER_STORAGE_BUFFER, W * H * 4, NULL, GL_DYNAMIC_COPY);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buf);
        glUseProgram(sample);
        glUniform1i(glGetUniformLocation(sample, "t"), 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glDispatchCompute(4, 4, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        e = glerr();
        const uint32_t *m = map_ro(GL_SHADER_STORAGE_BUFFER, W * H * 4);
        unsigned wrong = 0;
        char what[120] = "";
        for (unsigned i = 0; m && i < W * H; i++) {
            const uint32_t x = (i % W) * 8, y = (i / W) * 8, want = x | y << 8 | x << 16 | y << 24;
            if (m[i] != want && !wrong++)
                snprintf(what, sizeof what, "first wrong texel %u,%u: 0x%08x, want 0x%08x (low half texelFetch, high half textureLod)", i % W, i / W, m[i], want);
        }
        R("compute-texture-sample", m && !wrong && !e, "texelFetch + textureLod of an RGBA8 texture in compute: %u of %d texels wrong %s, GL error %s", m ? wrong : W * H, W * H, what, errname(e));
        if (m)
            glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteTextures(1, &tex);
        glDeleteBuffers(1, &buf);
        glDeleteProgram(sample);
    }

    /* Eight storage buffers in one shader (maxStorageBuffersPerShaderStage). */
    GLuint eight = mk_compute(kCsEight, g_log, sizeof g_log);
    if (!eight) {
        R("compute-8-ssbos", 0, "compute program did not build: %s", g_log);
    } else {
        GLuint bufs[8];
        for (uint32_t i = 0; i < 8; i++) {
            const uint32_t init[3] = {0, i + 1, 0};
            bufs[i] = mk_buffer(GL_SHADER_STORAGE_BUFFER, sizeof init, init, GL_DYNAMIC_COPY);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, bufs[i]);
        }
        glUseProgram(eight);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        e = glerr();
        uint32_t got[8], sum = 0;
        int ok = 1;
        for (uint32_t i = 0; i < 8; i++) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, bufs[i]);
            const uint32_t *m = map_ro(GL_SHADER_STORAGE_BUFFER, 12);
            got[i] = m ? m[0] : 0;
            if (m && i == 7)
                sum = m[2];
            ok = ok && got[i] == 100 + i;
            if (m)
                glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        }
        R("compute-8-ssbos", ok && sum == 28 && !e, "eight storage buffers bound at 0-7: v[0] = %u %u %u %u %u %u %u %u (want 100..107), sum of the seven v[1] read back %u (want 28), GL error %s",
          got[0], got[1], got[2], got[3], got[4], got[5], got[6], got[7], sum, errname(e));
        glDeleteBuffers(8, bufs);
        glDeleteProgram(eight);
    }

    /* Workgroup-shared memory with barrier() (WGSL var<workgroup> +
     * workgroupBarrier). */
    GLuint shared = mk_compute(kCsShared, g_log, sizeof g_log);
    if (!shared) {
        R("compute-shared-memory", 0, "compute program did not build: %s", g_log);
    } else {
        uint32_t init[256];
        memset(init, 0xAA, sizeof init);
        GLuint buf = mk_buffer(GL_SHADER_STORAGE_BUFFER, sizeof init, init, GL_DYNAMIC_COPY);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buf);
        glUseProgram(shared);
        glDispatchCompute(4, 1, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        e = glerr();
        const uint32_t *m = map_ro(GL_SHADER_STORAGE_BUFFER, sizeof init);
        unsigned wrong = 0;
        char what[120] = "";
        for (unsigned i = 0; m && i < 256; i++) {
            const uint32_t want = ((i / 64) * 64 + 63 - i % 64) * 2 + 1;
            if (m[i] != want && !wrong++)
                snprintf(what, sizeof what, "first wrong word %u: %u, want %u", i, m[i], want);
        }
        R("compute-shared-memory", m && !wrong && !e, "shared uint tmp[64], each invocation writes its slot, barrier(), reads the mirrored slot: %u of 256 words wrong %s, GL error %s", m ? wrong : 256, what, errname(e));
        if (m)
            glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        glDeleteBuffers(1, &buf);
        glDeleteProgram(shared);
    }

    /* Atomics on a storage buffer (WGSL atomic<u32>). */
    GLuint atomic = mk_compute(kCsAtomic, g_log, sizeof g_log);
    if (!atomic) {
        R("compute-atomic", 0, "compute program did not build: %s", g_log);
    } else {
        const uint32_t init[2] = {0, 0};
        GLuint buf = mk_buffer(GL_SHADER_STORAGE_BUFFER, sizeof init, init, GL_DYNAMIC_COPY);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buf);
        glUseProgram(atomic);
        glDispatchCompute(16, 1, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        e = glerr();
        const uint32_t *m = map_ro(GL_SHADER_STORAGE_BUFFER, sizeof init);
        const uint32_t count = m ? m[0] : 0, highest = m ? m[1] : 0;
        R("compute-atomic", m && count == 1024 && highest == 1023 && !e, "1024 invocations of atomicAdd(counter, 1) and atomicMax(highest, id): counter %u (want 1024), highest %u (want 1023), GL error %s", count, highest, errname(e));
        if (m)
            glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        glDeleteBuffers(1, &buf);
        glDeleteProgram(atomic);
    }
}

/* ---- timing ------------------------------------------------------------- */

static const char kCsTrivial[] =
    "#version 310 es\n"
    "layout(local_size_x = 1) in;\n"
    "layout(std430, binding = 0) buffer Out { uint data[]; } o;\n"
    "void main() { o.data[0] += 1u; }\n";

static void g_timing(void)
{
    GLuint prog = mk_compute(kCsTrivial, g_log, sizeof g_log);
    if (!prog) {
        R("time-compute-dispatch-1000", 0, "compute program did not build: %s", g_log);
        return;
    }
    const uint32_t init[4] = {0, 0, 0, 0};
    GLuint buf = mk_buffer(GL_SHADER_STORAGE_BUFFER, sizeof init, init, GL_DYNAMIC_COPY);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buf);
    glUseProgram(prog);
    glDispatchCompute(1, 1, 1); /* warm up: anything lazy happens here */
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    glFinish();
    const double t0 = probe_now_ms();
    for (int i = 0; i < 1000; i++) {
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS); /* as Dawn: after every dispatch */
    }
    const double t1 = probe_now_ms();
    glFinish();
    const double t2 = probe_now_ms();
    const GLenum e = glerr();
    const uint32_t *m = map_ro(GL_SHADER_STORAGE_BUFFER, sizeof init);
    const uint32_t count = m ? m[0] : 0;
    if (m)
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    R("time-compute-dispatch-1000", count == 1001 && !e, "1000 x (glDispatchCompute(1,1,1) + glMemoryBarrier(ALL)) of data[0] += 1: %.2f ms to issue, %.2f ms more in glFinish, %.1f us per dispatch; counter %u (want 1001: every dispatch ran, in order), GL error %s",
      t1 - t0, t2 - t1, (t2 - t0), count, errname(e));
    glDeleteBuffers(1, &buf);
    glDeleteProgram(prog);
}

/* ---- run ---------------------------------------------------------------- */

void gpu_groups_run(void)
{
    /* The baseline, then compute (the open question), then the rest. One
     * group that crashes the title can be left out of the next run with
     * skip=<group> as the launch argument. */
    glp_group("sanity", 1, "sanity-draw", g_sanity);
    glp_group("compute-ssbo", 1, "time-compute-first-compile compute-ssbo-map compute-ssbo-copyread compute-ubo-std140 compute-ssbo-arraylength", g_compute_ssbo);
    glp_group("compute-image", 1, "compute-image-store-rgba8 compute-image-store-r32f compute-image-load compute-image-r32f-readwrite", g_compute_image);
    glp_group("indirect", 1, "indirect-draw-arrays indirect-draw-elements indirect-dispatch", g_indirect);
    glp_group("compute-more", 1, "memory-barrier-texture compute-to-vertex-buffer fragment-ssbo compute-texture-sample compute-8-ssbos compute-shared-memory compute-atomic", g_compute_more);
    glp_group("copy-image", 1, "copy-image-2d copy-image-2darray copy-image-mip copy-image-rendered", g_copy_image);
    glp_group("sync", 1, "sync-gl-fence", g_sync);
    glp_group("ubo", 1, "ubo-std140", g_ubo);
    glp_group("draws", 1, "draw-instanced-divisor draw-elements-instanced-base-vertex base-instance draw-elements-base-vertex mrt-4", g_draws);
    glp_group("restart", 1, "primitive-restart", g_restart);
    glp_group("pbo", 1, "readpixels-pbo texsubimage-pbo", g_pbo);
    glp_group("float-rt", 1, "float-rt-rgba16f float-rt-rgba32f float-rt-rg16f float-rt-r32f float-rt-r11g11b10f readpixels-float", g_float_rt);
    glp_group("depth", 1, "depth-stencil-d16 depth-stencil-d24 depth-stencil-d24s8 depth-stencil-d32f depth-stencil-d32fs8 depth-stencil-s8", g_depth);
    glp_group("srgb", 1, "srgb-framebuffer", g_srgb);
    glp_group("bgra", 1, "bgra8", g_bgra);
    glp_group("msaa", 1, "msaa-resolve-rgba8 msaa-resolve-rgba16f sample-shading sample-mask alpha-to-coverage", g_msaa);
    glp_group("texbuffer", 1, "texture-buffer", g_texbuffer);
    glp_group("compressed", 1, "compressed-bc1 compressed-bc1-srgb compressed-bc3 compressed-bc4 compressed-bc5 compressed-bc7 compressed-etc2-rgb8 compressed-etc2-rgba8 compressed-astc-4x4", g_compressed);
    glp_group("clip", 1, "clip-distance", g_clip);
    glp_group("timer", 1, "timer-query", g_timer);
    glp_group("timing", 1, "time-compute-dispatch-1000", g_timing);
}
