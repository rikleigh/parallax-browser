/* Spike 9 - Sony's hardware video decoder from a homebrew title (R16).
 *
 * Reads a job list (/data/ps5webkit/hwdec/jobs.txt, written by run.sh) and
 * for each job:
 *
 *   bench  decode a clip as fast as the decoder goes: bring-up cost, memory
 *          the decoder asks for, sustained fps, per-call latency, output
 *          format (pitch, NV12/P010), and a 1/8-scale PPM of one picture
 *          converted on the CPU so the Mac can check it is the right image.
 *   gl     (title only) the decoder writes into GL buffer storage (persistent
 *          mapping = direct memory, GPU VA == CPU VA); a fragment pass reads
 *          NV12/P010 from it as an SSBO and writes RGBA8, which is drawn on
 *          the TV. No CPU copy of the picture. A readback PPM of the RGBA8
 *          result shows whether the zero-copy path is the right image.
 *
 * Built twice: as a registered title (PPSA99014, build-title.sh) and, with
 * -DPAYLOAD, as an elfldr payload (make), the secondary data point.
 *
 * Job line: <bench|gl> <h264|hevc|ivf|mp4> <path> <h264|hevc|vp9> <profile>
 *           <width> <height> [seconds] [pipeline depth, default 4]
 */
#define SPIKE_NAME "09-hw-decode"
#ifdef PAYLOAD
#include "../common/spike.h"
#else
#include "../common/title.h"
#include "../common/glutil.h"
#endif
#include "au.h"
#include "vdec2.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>

#ifndef LOG_HOST
#define LOG_HOST "192.168.1.100"
#endif
#ifndef LOG_PORT
#define LOG_PORT 9999
#endif
#define DIR "/data/ps5webkit/hwdec"
#define THUMB_FRAME 45 /* which output picture to dump */
#define THUMB_SCALE 8

int sceKernelAvailableFlexibleMemorySize(size_t *size);
int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t align, int64_t *out_start,
                                       size_t *out_size);

static void log_line(const char *s)
{
    spike_log("%s", s);
}

typedef struct {
    char mode[8], kind[8], path[192], codec[8];
    int profile, width, height, seconds, depth;
} job_t;

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static uint32_t codec_tag(const char *c)
{
    return !strcmp(c, "h264") ? VDEC2_CODEC_AVC : !strcmp(c, "hevc") ? VDEC2_CODEC_HEVC
                                                                      : VDEC2_CODEC_VP9;
}

static void mem_report(const char *when)
{
    size_t flex = 0, dsz = 0;
    int64_t dstart = 0;
    sceKernelAvailableFlexibleMemorySize(&flex);
    sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), 0x4000, &dstart, &dsz);
    spike_log("memory %s: flexible available %zu MB, largest free direct block %zu MB", when,
              flex >> 20, dsz >> 20);
}

/* Whole file into direct memory (the title has ~250 MB of flexible memory;
 * the 8K clip alone is 150 MB). */
static uint8_t *load_file(const char *path, size_t *size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return NULL;
    }
    /* One buffer, reused by later jobs (grown when a clip is bigger). */
    static uint8_t *buf;
    static size_t cap;
    if ((size_t)st.st_size > cap) {
        cap = ((size_t)st.st_size + 0xffff) & ~(size_t)0xffff;
        buf = vdec2_direct_alloc(cap, 0x10000);
    }
    size_t got = 0;
    while (buf && got < (size_t)st.st_size) {
        const ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    if (!buf || got != (size_t)st.st_size)
        return NULL;
    *size = got;
    return buf;
}

/* 1/THUMB_SCALE picture, BT.709 limited range -> RGB, as a binary PPM. */
static void dump_thumb(const char *name, const vdec2_picture_t *p, int ten_bit, int disp_w,
                       int disp_h)
{
    const int w = disp_w / THUMB_SCALE, h = disp_h / THUMB_SCALE;
    uint8_t *rgb = malloc((size_t)w * h * 3);
    if (!rgb)
        return;
    const uint8_t *y_plane = p->buffer;
    const uint8_t *uv_plane = y_plane + (size_t)p->pitch_bytes * p->height;
    for (int ty = 0; ty < h; ty++)
        for (int tx = 0; tx < w; tx++) {
            const int x = tx * THUMB_SCALE, yy = ty * THUMB_SCALE;
            float Y, U, V;
            if (ten_bit) {
                const uint16_t *yr = (const uint16_t *)(y_plane + (size_t)yy * p->pitch_bytes);
                const uint16_t *cr = (const uint16_t *)(uv_plane + (size_t)(yy / 2) * p->pitch_bytes);
                /* 10-bit output is LSB-aligned in 16 bits (measured), not
                 * MSB-aligned P010. */
                Y = yr[x] / 1023.0f, U = cr[(x / 2) * 2] / 1023.0f, V = cr[(x / 2) * 2 + 1] / 1023.0f;
            } else {
                const uint8_t *yr = y_plane + (size_t)yy * p->pitch_bytes;
                const uint8_t *cr = uv_plane + (size_t)(yy / 2) * p->pitch_bytes;
                Y = yr[x] / 255.0f, U = cr[(x / 2) * 2] / 255.0f, V = cr[(x / 2) * 2 + 1] / 255.0f;
            }
            Y = (Y - 16 / 255.0f) * (255 / 219.0f);
            U = (U - 0.5f) * (255 / 224.0f);
            V = (V - 0.5f) * (255 / 224.0f);
            const float c[3] = {Y + 1.5748f * V, Y - 0.1873f * U - 0.4681f * V, Y + 1.8556f * U};
            for (int k = 0; k < 3; k++) {
                const float v = c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k];
                rgb[((size_t)ty * w + tx) * 3 + k] = (uint8_t)(v * 255 + 0.5f);
            }
        }
    char path[256];
    snprintf(path, sizeof path, DIR "/%s.ppm", name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        char hdr[64];
        const int n = snprintf(hdr, sizeof hdr, "P6\n%d %d\n255\n", w, h);
        if (write(fd, hdr, (size_t)n) != n || write(fd, rgb, (size_t)w * h * 3) < 0)
            spike_log("thumbnail write failed");
        close(fd);
        spike_log("thumbnail %s (%dx%d)", path, w, h);
    }
    free(rgb);
}

typedef struct {
    unsigned aus, frames, errors, flagged, hidden, hidden_out;
    double call_ms_sum, call_ms_max, elapsed_ms;
    int first_rc;
    unsigned first_rc_at;
} stats_t;

/* Decode loop shared by bench and gl: feeds AUs until the clip ends or the
 * time is up (rewinding short clips until min_frames), calls on_picture for
 * each picture out. */
typedef int (*picture_fn)(void *ctx, const vdec2_picture_t *p, unsigned index);

static void decode_loop(vdec2_t *v, au_src_t *src, const job_t *j, unsigned min_frames,
                        stats_t *st, picture_fn fn, void *ctx)
{
    memset(st, 0, sizeof *st);
    const double t0 = spike_now_ms(), limit = j->seconds * 1000.0;
    uint64_t pts = 0;
    int stop = 0;
    while (!stop && spike_now_ms() - t0 < limit) {
        int key = 0, hidden = 0;
        uint8_t *slot = vdec2_input_slot(v);
        const long n = au_next(src, slot, VDEC2_INPUT_SLOT_BYTES, &key, &hidden);
        if (n < 0) {
            spike_log("AU parse error at AU %u", st->aus);
            break;
        }
        if (n == 0) {
            if (st->frames >= min_frames)
                break;
            au_rewind(src);
            continue;
        }
        vdec2_picture_t pic;
        const double c0 = spike_now_ms();
        const int rc = vdec2_decode(v, slot, (size_t)n, pts++, &pic);
        const double dt = spike_now_ms() - c0;
        st->aus++;
        st->hidden += hidden;
        st->call_ms_sum += dt;
        if (dt > st->call_ms_max)
            st->call_ms_max = dt;
        if (rc != 0) {
            if (!st->errors) {
                st->first_rc = rc;
                st->first_rc_at = st->aus;
            }
            st->errors++;
            if (st->errors > 64 && st->frames == 0)
                break;
            continue;
        }
        if (pic.valid && hidden) {
            st->hidden_out++; /* VP9 alt-ref: decoded, returned, never shown */
            continue;
        }
        if (pic.valid) {
            st->flagged += pic.error != 0;
            if (fn && fn(ctx, &pic, st->frames) != 0)
                stop = 1;
            st->frames++;
        }
    }
    for (int i = 0; i < 32; i++) { /* drain */
        vdec2_picture_t pic;
        if (vdec2_flush(v, &pic) != 0 || !pic.valid)
            break;
        if (fn)
            fn(ctx, &pic, st->frames);
        st->frames++;
    }
    st->elapsed_ms = spike_now_ms() - t0;
}

/* -- bench --------------------------------------------------------------------- */
typedef struct {
    const job_t *j;
    char name[96];
    int ten_bit;
    int complete_checks, complete_changed;
} bench_ctx_t;

/* A sparse hash of the whole picture (every 61st byte). */
static uint32_t picture_hash(const vdec2_picture_t *p)
{
    const uint8_t *b = p->buffer;
    const size_t n = (size_t)p->pitch_bytes * p->height * 3 / 2;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i += 61)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

static int bench_picture(void *vctx, const vdec2_picture_t *p, unsigned index)
{
    bench_ctx_t *c = vctx;
    /* Is a picture complete when Decode hands it back, or still being
     * written? Hash it, wait, hash again (no decoder calls in between). */
    if (index == 10 || index == 100 || index == 300) {
        const uint32_t h0 = picture_hash(p);
        usleep(50 * 1000);
        const uint32_t h1 = picture_hash(p);
        c->complete_checks++;
        c->complete_changed += h0 != h1;
    }
    if (index == 0 || index == THUMB_FRAME)
        spike_log("  picture %u: %ux%u pitch %u (%u bytes) format %u size %llu buffer %p%s", index,
                  p->width, p->height, p->pitch, p->pitch_bytes, p->frame_format,
                  (unsigned long long)p->buffer_size, p->buffer, p->error ? " ERROR FLAG" : "");
    if (index == 0 && c->ten_bit) {
        const uint16_t *y = p->buffer;
        spike_log("  10-bit check: Y samples 0x%04x 0x%04x 0x%04x 0x%04x (<0x400: LSB-aligned; low 6 bits 0: P010)",
                  y[0], y[1], y[1000], y[2000]);
    }
    if (index == THUMB_FRAME)
        dump_thumb(c->name, p, c->ten_bit, c->j->width, c->j->height);
    return 0;
}

static int open_source(const job_t *j, uint8_t **file, size_t *size, au_src_t *src)
{
    const double t0 = spike_now_ms();
    *file = load_file(j->path, size);
    if (!*file) {
        SPIKE_FAIL("load", "%s: cannot read (errno %d)", j->path, errno);
        return -1;
    }
    if (au_open(src, *file, *size, j->kind) != 0) {
        SPIKE_FAIL("load", "%s: not a %s stream", j->path, j->kind);
        return -1;
    }
    spike_log("loaded %s: %zu MB in %.0f ms", j->path, *size >> 20, spike_now_ms() - t0);
    return 0;
}

static void run_bench(const job_t *j)
{
    bench_ctx_t ctx = {.j = j};
    snprintf(ctx.name, sizeof ctx.name, "%s-%s%s", base_name(j->path), j->kind,
             j->depth ? (j->depth == 1 ? "-d1" : j->depth == 2 ? "-d2" : "-dN") : "");
    ctx.ten_bit = j->profile == 2;
    static au_src_t src;
    uint8_t *file;
    size_t size;
    spike_log("== bench %s: %s %dx%d profile %d", ctx.name, j->codec, j->width, j->height, j->profile);
    if (open_source(j, &file, &size, &src) != 0)
        return;
    vdec2_config_t cfg = {.codec = codec_tag(j->codec), .profile = (uint32_t)j->profile,
                          .width = j->width, .height = j->height, .pipeline_depth = j->depth};
    vdec2_info_t info;
    const char *stage;
    int rc;
    mem_report("before open");
    const double o0 = spike_now_ms();
    vdec2_t *v = vdec2_open(&cfg, &info, &stage, &rc);
    const double open_ms = spike_now_ms() - o0;
    spike_log("decoder asks: level %u, compute %llu KB, cpu (flexible) %llu KB, gpu %llu KB, "
              "cpu_gpu %llu KB, frame %llu KB (align 0x%x)", info.level,
              (unsigned long long)info.compute_bytes >> 10, (unsigned long long)info.cpu_bytes >> 10,
              (unsigned long long)info.gpu_bytes >> 10, (unsigned long long)info.cpu_gpu_bytes >> 10,
              (unsigned long long)info.frame_bytes >> 10, info.frame_alignment);
    if (!v) {
        SPIKE_FAIL(ctx.name, "bring-up failed at %s rc=0x%08x", stage, (unsigned)rc);
        return;
    }
    spike_log("decoder up in %.1f ms", open_ms);
    mem_report("after open");
    stats_t st;
    decode_loop(v, &src, j, 600, &st, bench_picture, &ctx);
    vdec2_close(v);
    const double fps = st.frames * 1000.0 / st.elapsed_ms;
    const int ok = st.frames > 0 && st.errors == 0;
    SPIKE_RESULT(ctx.name, ok ? "PASS" : "FAIL",
                 "%dx%d %s: %u AUs -> %u pictures in %.0f ms = %.1f fps; call avg %.2f ms max %.1f ms; "
                 "%u rc errors (first 0x%08x at AU %u), %u flagged, %u hidden (%u returned a picture, dropped), %d fragments; "
                 "depth %d, flexible %llu MB; %d/%d pictures changed after return",
                 j->width, j->height, j->codec, st.aus, st.frames, st.elapsed_ms, fps,
                 st.aus ? st.call_ms_sum / st.aus : 0, st.call_ms_max, st.errors,
                 (unsigned)st.first_rc, st.first_rc_at, st.flagged, st.hidden, st.hidden_out, src.fragments,
                 j->depth ? j->depth : 4, (unsigned long long)info.cpu_bytes >> 20,
                 ctx.complete_changed, ctx.complete_checks);
}

#ifndef PAYLOAD
/* -- gl: zero-copy into GL, shown on the TV -------------------------------------- */
#define GL_SLOTS 8
static const char *kVsFull =
    "#version 430 core\n"
    "out vec2 uv;\n"
    "uniform int flip;\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "  uv = vec2(p.x, flip != 0 ? 1.0 - p.y : p.y);\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
/* NV12 / P010 straight out of the decoder's buffer; one fragment per pixel.
 * Row 0 of the target is the picture's top row (what Hmd2 expects too). */
static const char *kFsConvert =
    "#version 430 core\n"
    "layout(std430, binding = 0) readonly buffer Frame { uint d[]; };\n"
    "uniform int pitch;\n"
    "uniform int uv_off;\n"
    "uniform int ten;\n"
    "out vec4 o;\n"
    "uint b8(int a) { return (d[a >> 2] >> ((a & 3) * 8)) & 0xffu; }\n"
    "uint b16(int a) { return (d[a >> 2] >> ((a & 2) * 8)) & 0xffffu; }\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  float y, u, v;\n"
    "  if (ten == 0) {\n"
    "    y = float(b8(p.y * pitch + p.x)) / 255.0;\n"
    "    int c = uv_off + (p.y >> 1) * pitch + (p.x & ~1);\n"
    "    u = float(b8(c)) / 255.0; v = float(b8(c + 1)) / 255.0;\n"
    "  } else {\n"
    "    y = float(b16(p.y * pitch + p.x * 2)) / 1023.0;\n" /* LSB-aligned 10-bit */
    "    int c = uv_off + (p.y >> 1) * pitch + (p.x >> 1) * 4;\n"
    "    u = float(b16(c)) / 1023.0; v = float(b16(c + 2)) / 1023.0;\n"
    "  }\n"
    "  y = (y - 16.0 / 255.0) * (255.0 / 219.0);\n"
    "  u = (u - 0.5) * (255.0 / 224.0); v = (v - 0.5) * (255.0 / 224.0);\n"
    "  o = vec4(y + 1.5748 * v, y - 0.1873 * u - 0.4681 * v, y + 1.8556 * u, 1.0);\n"
    "}\n";
static const char *kFsShow =
    "#version 430 core\n"
    "in vec2 uv;\n"
    "uniform sampler2D t;\n"
    "out vec4 o;\n"
    "void main() { o = texture(t, uv); }\n";

typedef struct {
    const job_t *j;
    char name[96];
    EGLDisplay dpy;
    EGLSurface surf;
    GLuint buf[GL_SLOTS];
    uint8_t *map[GL_SLOTS];
    size_t slot_off[GL_SLOTS], slot_bytes;
    GLuint conv, show, rgba, fbo, small, small_fbo, vao;
    GLint u_pitch, u_uvoff, u_ten, u_flip_c, u_flip_s;
    int win_w, win_h, ten_bit;
    double conv_ms, present_ms;
    unsigned timed;
    int slot_mismatch;
} gl_ctx_t;

static void gl_dump_readback(gl_ctx_t *c)
{
    const int w = c->j->width / THUMB_SCALE, h = c->j->height / THUMB_SCALE;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, c->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, c->small_fbo);
    glBlitFramebuffer(0, 0, c->j->width, c->j->height, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, c->small_fbo);
    uint8_t *rgba = malloc((size_t)w * h * 4), *rgb = malloc((size_t)w * h * 3);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    for (int i = 0; i < w * h; i++)
        memcpy(rgb + i * 3, rgba + i * 4, 3); /* row 0 = top already */
    char path[256];
    snprintf(path, sizeof path, DIR "/%s-gl.ppm", c->name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        char hdr[64];
        const int n = snprintf(hdr, sizeof hdr, "P6\n%d %d\n255\n", w, h);
        if (write(fd, hdr, (size_t)n) != n || write(fd, rgb, (size_t)w * h * 3) < 0)
            spike_log("gl readback write failed");
        close(fd);
        spike_log("gl readback %s (%dx%d)", path, w, h);
    }
    free(rgba);
    free(rgb);
}

static int gl_picture(void *vctx, const vdec2_picture_t *p, unsigned index)
{
    gl_ctx_t *c = vctx;
    const int s = p->slot;
    /* The picture must be inside the GL buffer we handed the decoder. */
    const uint8_t *base = c->map[s] + c->slot_off[s];
    const size_t off = (size_t)((const uint8_t *)p->buffer - base);
    if ((const uint8_t *)p->buffer < base || off >= c->slot_bytes) {
        if (!c->slot_mismatch++)
            spike_log("picture %u at %p is not in GL slot %d (%p..+%zu)", index, p->buffer, s,
                      (void *)base, c->slot_bytes);
        return 0;
    }
    const double t0 = spike_now_ms();
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, c->buf[s], (GLintptr)(c->slot_off[s] + off),
                      (GLsizeiptr)(c->slot_bytes - off));
    glBindFramebuffer(GL_FRAMEBUFFER, c->fbo);
    glViewport(0, 0, c->j->width, c->j->height);
    glUseProgram(c->conv);
    glUniform1i(c->u_pitch, (GLint)p->pitch_bytes);
    glUniform1i(c->u_uvoff, (GLint)((size_t)p->pitch_bytes * p->height));
    glUniform1i(c->u_ten, c->ten_bit);
    glUniform1i(c->u_flip_c, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    const int timed = index % 30 == 5;
    if (timed)
        glFinish();
    const double t1 = spike_now_ms();
    /* Letterboxed onto the TV. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    int vw = c->win_w, vh = c->win_w * c->j->height / c->j->width;
    if (vh > c->win_h) {
        vh = c->win_h;
        vw = c->win_h * c->j->width / c->j->height;
    }
    glViewport((c->win_w - vw) / 2, (c->win_h - vh) / 2, vw, vh);
    glUseProgram(c->show);
    glUniform1i(c->u_flip_s, 1);
    glBindTexture(GL_TEXTURE_2D, c->rgba);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    eglSwapBuffers(c->dpy, c->surf);
    const double t2 = spike_now_ms();
    if (timed) {
        c->conv_ms += t1 - t0;
        c->present_ms += t2 - t1;
        c->timed++;
    }
    if (index == THUMB_FRAME)
        gl_dump_readback(c);
    return 0;
}

static void run_gl(const job_t *j)
{
    static gl_ctx_t c;
    memset(&c, 0, sizeof c);
    c.j = j;
    c.ten_bit = j->profile == 2;
    snprintf(c.name, sizeof c.name, "%s-%s", base_name(j->path), j->kind);
    spike_log("== gl %s: %s %dx%d", c.name, j->codec, j->width, j->height);

    c.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    static const EGLint cfg_attrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
                                       EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                       EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    static const EGLint ctx_attrs[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 6,
                                       EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
                                       EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    EGLContext ctx = EGL_NO_CONTEXT;
    if (eglInitialize(c.dpy, NULL, NULL) && eglBindAPI(EGL_OPENGL_API) &&
        eglChooseConfig(c.dpy, cfg_attrs, &cfg, 1, &n) && n == 1) {
        c.surf = eglCreateWindowSurface(c.dpy, cfg, (EGLNativeWindowType)0, NULL);
        ctx = eglCreateContext(c.dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    }
    if (ctx == EGL_NO_CONTEXT || c.surf == EGL_NO_SURFACE || !eglMakeCurrent(c.dpy, c.surf, c.surf, ctx)) {
        SPIKE_FAIL("gl-init", "EGL 0x%04x", eglGetError());
        return;
    }
    eglQuerySurface(c.dpy, c.surf, EGL_WIDTH, &c.win_w);
    eglQuerySurface(c.dpy, c.surf, EGL_HEIGHT, &c.win_h);
    char err[512];
    c.conv = glu_program(kVsFull, kFsConvert, err, sizeof err);
    if (c.conv)
        c.show = glu_program(kVsFull, kFsShow, err, sizeof err);
    if (!c.conv || !c.show) {
        SPIKE_FAIL("gl-init", "shader: %s", err);
        return;
    }
    c.u_pitch = glGetUniformLocation(c.conv, "pitch");
    c.u_uvoff = glGetUniformLocation(c.conv, "uv_off");
    c.u_ten = glGetUniformLocation(c.conv, "ten");
    c.u_flip_c = glGetUniformLocation(c.conv, "flip");
    c.u_flip_s = glGetUniformLocation(c.show, "flip");
    glGenVertexArrays(1, &c.vao);
    glBindVertexArray(c.vao);
    GLint max_ssbo = 0;
    glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &max_ssbo);
    spike_log("GL %s, window %dx%d, max SSBO %d MB", (const char *)glGetString(GL_VERSION), c.win_w,
              c.win_h, max_ssbo >> 20);

    /* Target: RGBA8 immutable 2D = SW_64KB_R_X in ps5-opengl, exportable as
     * a T# for Hmd2 (Spike 3b). */
    glGenTextures(1, &c.rgba);
    glBindTexture(GL_TEXTURE_2D, c.rgba);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, j->width, j->height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &c.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, c.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c.rgba, 0);
    glGenTextures(1, &c.small);
    glBindTexture(GL_TEXTURE_2D, c.small);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, j->width / THUMB_SCALE, j->height / THUMB_SCALE);
    glGenFramebuffers(1, &c.small_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, c.small_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c.small, 0);

    static au_src_t src;
    uint8_t *file;
    size_t size;
    if (open_source(j, &file, &size, &src) != 0)
        return;
    /* Ask the decoder its frame size first (a throwaway bring-up). */
    vdec2_config_t vc = {.codec = codec_tag(j->codec), .profile = (uint32_t)j->profile,
                         .width = j->width, .height = j->height, .frame_slots = 1,
                         .pipeline_depth = j->depth};
    vdec2_info_t info;
    const char *stage;
    int rc;
    vdec2_t *v = vdec2_open(&vc, &info, &stage, &rc);
    if (!v) {
        SPIKE_FAIL(c.name, "probe bring-up failed at %s rc=0x%08x", stage, (unsigned)rc);
        return;
    }
    vdec2_close(v);
    const size_t align = info.frame_alignment > 0x10000 ? info.frame_alignment : 0x10000;
    c.slot_bytes = (info.frame_bytes + 0x3fff) & ~(size_t)0x3fff;
    void *slots[GL_SLOTS];
    glGenBuffers(GL_SLOTS, c.buf);
    const GLbitfield fl = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    for (int i = 0; i < GL_SLOTS; i++) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.buf[i]);
        glBufferStorage(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(c.slot_bytes + align), NULL, fl);
        c.map[i] = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)(c.slot_bytes + align), fl);
        if (!c.map[i]) {
            SPIKE_FAIL(c.name, "GL buffer %d: map failed (GL error 0x%x)", i, glGetError());
            return;
        }
        const uintptr_t a = (uintptr_t)c.map[i];
        c.slot_off[i] = ((a + align - 1) & ~(uintptr_t)(align - 1)) - a;
        slots[i] = c.map[i] + c.slot_off[i];
        if (i == 0)
            spike_log("GL buffer 0 mapped at %p (offset %zu to reach 0x%zx alignment), %zu MB per slot",
                      (void *)c.map[i], c.slot_off[i], align, c.slot_bytes >> 20);
    }
    /* A CPU copy, for comparison: what the cheapest non-zero-copy path costs. */
    {
        void *tmp = vdec2_direct_alloc(c.slot_bytes, 0x10000);
        if (tmp) {
            memset(tmp, 0x80, c.slot_bytes);
            const double m0 = spike_now_ms();
            for (int k = 0; k < 4; k++)
                memcpy(slots[k % GL_SLOTS], tmp, (size_t)info.frame_bytes);
            spike_log("CPU copy of one %llu KB frame into a GL buffer: %.2f ms",
                      (unsigned long long)info.frame_bytes >> 10, (spike_now_ms() - m0) / 4);
        }
    }
    vc.frame_slots = GL_SLOTS;
    vc.external_slots = slots;
    v = vdec2_open(&vc, &info, &stage, &rc);
    if (!v) {
        SPIKE_FAIL(c.name, "bring-up with GL buffers failed at %s rc=0x%08x", stage, (unsigned)rc);
        return;
    }
    spike_log("decoder up with its output slots in GL buffer storage");
    stats_t st;
    decode_loop(v, &src, j, 600, &st, gl_picture, &c);
    vdec2_close(v);
    const double fps = st.frames * 1000.0 / st.elapsed_ms;
    const int ok = st.frames > 0 && st.errors == 0 && !c.slot_mismatch;
    SPIKE_RESULT(c.name, ok ? "PASS" : "FAIL",
                 "zero-copy GL: %u pictures in %.0f ms = %.1f fps (decode + convert + present, vsync); "
                 "NV12->RGBA %.2f ms, present %.2f ms (avg of %u timed); %u rc errors, %d outside GL slots",
                 st.frames, st.elapsed_ms, fps, c.timed ? c.conv_ms / c.timed : 0,
                 c.timed ? c.present_ms / c.timed : 0, c.timed, st.errors, c.slot_mismatch);
    spike_log("VISUAL CHECK (TV): the clip played letterboxed, picture correct (colours, no blocks)?");
}
#endif

static int read_jobs(job_t *jobs, int max)
{
    int fd = open(DIR "/jobs.txt", O_RDONLY);
    if (fd < 0)
        return -1;
    static char text[8192];
    const ssize_t n = read(fd, text, sizeof text - 1);
    close(fd);
    if (n <= 0)
        return 0;
    text[n] = 0;
    int count = 0;
    for (char *line = strtok(text, "\n"); line && count < max; line = strtok(NULL, "\n")) {
        job_t *j = &jobs[count];
        memset(j, 0, sizeof *j);
        j->seconds = 40;
        if (line[0] == '#' ||
            sscanf(line, "%7s %7s %191s %7s %d %d %d %d %d", j->mode, j->kind, j->path, j->codec,
                   &j->profile, &j->width, &j->height, &j->seconds, &j->depth) < 7)
            continue;
        count++;
    }
    return count;
}

int main(void)
{
    /* Before the unjail (PS5VR: the module won't load after it). */
    const int lrc = vdec2_load();
#ifdef PAYLOAD
    spike_log("ps5webkit spike 09 (payload): hardware video decode, pid %d", (int)getpid());
#else
    spike_title_init("spike09", LOG_HOST, LOG_PORT);
    spike_log("ps5webkit spike 09 (title): hardware video decode, pid %d", (int)getpid());
#endif
    vdec2_set_logger(log_line);
    spike_log("vdec2: %s", vdec2_load_report());
    if (lrc != 0) {
        SPIKE_FAIL("load", "libSceVideodec2 not bound (0x%08x)", (unsigned)lrc);
        return 1;
    }
    SPIKE_PASS("load", "libSceVideodec2 bound");
    mem_report("at start");
    static job_t jobs[32];
    const int count = read_jobs(jobs, 32);
    if (count <= 0) {
        SPIKE_FAIL("jobs", "no jobs in " DIR "/jobs.txt");
        return 1;
    }
    for (int i = 0; i < count; i++) {
        if (!strcmp(jobs[i].mode, "bench"))
            run_bench(&jobs[i]);
#ifndef PAYLOAD
        else if (!strcmp(jobs[i].mode, "gl"))
            run_gl(&jobs[i]);
#endif
    }
    spike_log("spike 09 done");
    return 0;
}
