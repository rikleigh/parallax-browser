/*
 * titlevdec.c - the browser title's hardware video decoder backend
 * (Milestone 2 step 2b): PS5VideoDecoder (wpe-ps5) over Spike 9's vdec2
 * (libSceVideodec2, bound by 13.60 export offsets). Pictures go straight
 * into the GL buffers the renderer hands PS5VideoDecoder (ps5-opengl backs
 * them with GPU-visible direct memory, the decoder's own memory type).
 *
 * vdec2_load() must run before the etaHEN unjail (PS5VR: the module does not
 * load after it), so main() calls ps5wk_title_vdec_preload() first thing.
 * /data/ps5webkit/no-hwdec (a file) leaves decoding to the WebProcess
 * (software), for A/B runs.
 *
 * vdec2.c is derived from PS5VR (GPL-3.0-or-later), like the rest of the
 * title's PS5 runtime (FEASIBILITY.md section 6).
 */
#include "09-hw-decode/vdec2.h"
#include "PS5VideoDecoder.h"
#include "PS5VideoProtocol.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int sceKernelAvailableFlexibleMemorySize(size_t *size);

static int s_load_rc = -1;
static int s_installed;
double ps5wk_title_direct_mb(void); /* titlemem.c */

static void log_line(const char *line)
{
    fprintf(stderr, "[vdec] %s\n", line);
}

void ps5wk_title_vdec_preload(void);
void ps5wk_title_vdec_preload(void)
{
    s_load_rc = vdec2_load();
}

static double flexible_mb(void)
{
    size_t size = 0;
    sceKernelAvailableFlexibleMemorySize(&size);
    return size / 1048576.0;
}

static void *tv_open(unsigned codec, unsigned profile, unsigned depth, int width, int height, size_t *frame_bytes,
    size_t *frame_align, char *error, size_t error_size)
{
    vdec2_config_t c;
    memset(&c, 0, sizeof c);
    c.width = width;
    c.height = height;
    c.caller_frames = 1;
    /* Spike 9: depth 4 nearly doubles 8K throughput but needs 223 MB of
     * flexible memory there (139 MB at depth 2, still 79 fps). Since pages
     * run in the title they share its flexible memory, and a 4K H.264
     * decoder at depth 4 (71 MB) often didn't fit (2026-11-16): depth 2
     * above 1080p, depth 4 (cheap) up to it. */
    c.pipeline_depth = (long)width * height > 1920L * 1088 ? 2 : 4;
    switch (codec) {
    case PS5WK_VCODEC_H264:
        c.codec = VDEC2_CODEC_AVC;
        c.profile = 100; /* High covers Baseline and Main */
        break;
    case PS5WK_VCODEC_HEVC:
        c.codec = VDEC2_CODEC_HEVC;
        c.profile = depth > 8 ? 2 : 1;
        break;
    default:
        c.codec = VDEC2_CODEC_VP9;
        c.profile = depth > 8 ? 2 : 0;
        break;
    }
    (void)profile;
    double flex_before = flexible_mb(), direct_before = ps5wk_title_direct_mb();
    vdec2_info_t info;
    const char *stage = "";
    int rc = 0;
    vdec2_t *v = vdec2_open(&c, &info, &stage, &rc);
    if (!v) {
        snprintf(error, error_size, "%s rc 0x%08x; flexible %.0f MB free", stage, (unsigned)rc, flex_before);
        return NULL;
    }
    *frame_bytes = vdec2_frame_size(v);
    *frame_align = info.frame_alignment ? info.frame_alignment : 0x10000;
    char line[320];
    snprintf(line, sizeof line,
        "Videodec2 %dx%d codec %u profile %u depth %d: decoder memory flexible %.1f MB, direct %.1f MB (+ queue %.1f, input 32); "
        "frame %.1f MB (align 0x%x), level %u; flexible free %.0f -> %.0f MB, direct memory in use %.0f -> %.0f MB",
        width, height, c.codec, c.profile, c.pipeline_depth, info.cpu_bytes / 1048576.0,
        (info.gpu_bytes + info.cpu_gpu_bytes) / 1048576.0, info.compute_bytes / 1048576.0, info.frame_bytes / 1048576.0,
        info.frame_alignment, info.level, flex_before, flexible_mb(), direct_before, ps5wk_title_direct_mb());
    log_line(line);
    return v;
}

static void to_picture(const vdec2_picture_t *in, struct ps5wk_vdec_picture *out)
{
    memset(out, 0, sizeof *out);
    out->valid = in->valid;
    out->error = in->error;
    out->width = in->width;
    out->height = in->height;
    out->pitch_bytes = in->pitch_bytes;
    out->buffer = in->buffer;
    out->has_pts = in->has_pts;
    out->pts = in->pts;
}

static int tv_decode(void *h, const void *au, size_t size, uint64_t pts, int64_t pts_ns, void *frame,
    struct ps5wk_vdec_picture *pic)
{
    (void)pts_ns;
    vdec2_picture_t p;
    int rc = vdec2_decode_into((vdec2_t *)h, au, size, pts, frame, &p);
    to_picture(&p, pic);
    return rc;
}

static int tv_flush(void *h, void *frame, struct ps5wk_vdec_picture *pic)
{
    vdec2_picture_t p;
    int rc = vdec2_flush_into((vdec2_t *)h, frame, &p);
    to_picture(&p, pic);
    return rc;
}

static int tv_reset(void *h)
{
    return vdec2_renew((vdec2_t *)h);
}

static void tv_close(void *h)
{
    vdec2_close((vdec2_t *)h);
    char line[160];
    snprintf(line, sizeof line, "decoder closed: flexible free %.0f MB, direct memory in use %.0f MB (its output buffers go back with the next frame)",
        flexible_mb(), ps5wk_title_direct_mb());
    log_line(line);
}

static const char *tv_report(void)
{
    return vdec2_load_report();
}

static const struct ps5wk_vdec_backend title_backend = {
    "Videodec2",
    PS5WK_VCODEC_H264 | PS5WK_VCODEC_HEVC | PS5WK_VCODEC_VP9 | PS5WK_VCODEC_10BIT,
    8192,
    4352,
    tv_open,
    tv_decode,
    tv_flush,
    tv_reset,
    tv_close,
    tv_report,
};

/* /data/ps5webkit/vdec-probe (a file): before the backend is installed, open
 * and close a decoder for each size below and log what Videodec2 accepts
 * (portrait and square VR formats; the first headset test found 1920x3840
 * HEVC refused at QueryDecoderMemoryInfo, 0x811d0200). */
static void probe_sizes(void)
{
    static const struct { unsigned codec, profile, level; int w, h; } k[] = {
        { VDEC2_CODEC_HEVC, 1, 0, 3840, 1920 }, { VDEC2_CODEC_HEVC, 1, 0, 1920, 3840 }, { VDEC2_CODEC_HEVC, 1, 183, 1920, 3840 },
        { VDEC2_CODEC_HEVC, 1, 0, 1920, 1920 }, { VDEC2_CODEC_HEVC, 1, 0, 1920, 2048 }, { VDEC2_CODEC_HEVC, 1, 0, 1920, 2176 },
        { VDEC2_CODEC_HEVC, 1, 0, 1920, 2304 }, { VDEC2_CODEC_HEVC, 1, 0, 1920, 2560 }, { VDEC2_CODEC_HEVC, 1, 0, 1080, 1920 },
        { VDEC2_CODEC_HEVC, 1, 0, 2160, 3840 }, { VDEC2_CODEC_HEVC, 1, 0, 2048, 4096 }, { VDEC2_CODEC_HEVC, 1, 0, 2880, 5760 },
        { VDEC2_CODEC_HEVC, 1, 0, 2880, 2880 }, { VDEC2_CODEC_HEVC, 1, 0, 3840, 3840 }, { VDEC2_CODEC_HEVC, 1, 0, 4096, 4096 },
        { VDEC2_CODEC_HEVC, 1, 0, 4352, 4352 }, { VDEC2_CODEC_HEVC, 1, 0, 5120, 5120 }, { VDEC2_CODEC_HEVC, 1, 0, 5760, 5760 },
        { VDEC2_CODEC_HEVC, 1, 0, 7680, 3840 }, { VDEC2_CODEC_HEVC, 1, 0, 8192, 4352 }, { VDEC2_CODEC_HEVC, 1, 0, 8192, 8192 },
        { VDEC2_CODEC_HEVC, 2, 0, 1920, 3840 }, { VDEC2_CODEC_HEVC, 2, 0, 3840, 3840 },
        { VDEC2_CODEC_AVC, 100, 0, 1080, 1920 }, { VDEC2_CODEC_AVC, 100, 0, 1920, 1920 }, { VDEC2_CODEC_AVC, 100, 0, 1088, 1920 },
        { VDEC2_CODEC_AVC, 100, 0, 2160, 3840 }, { VDEC2_CODEC_AVC, 100, 0, 2304, 4096 },
        { VDEC2_CODEC_VP9, 0, 0, 1080, 1920 }, { VDEC2_CODEC_VP9, 0, 0, 2160, 3840 }, { VDEC2_CODEC_VP9, 0, 0, 3840, 3840 },
        { VDEC2_CODEC_VP9, 0, 0, 4096, 4096 },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        vdec2_config_t c;
        memset(&c, 0, sizeof c);
        c.codec = k[i].codec;
        c.profile = k[i].profile;
        c.level = k[i].level;
        c.width = k[i].w;
        c.height = k[i].h;
        c.caller_frames = 1;
        c.pipeline_depth = (long)k[i].w * k[i].h > 3840L * 2176 ? 2 : 4;
        vdec2_info_t info;
        memset(&info, 0, sizeof info);
        const char *stage = "";
        int rc = 0;
        vdec2_t *v = vdec2_open(&c, &info, &stage, &rc);
        fprintf(stderr, "[vdec] probe %s %s profile %u level %u %dx%d (%s): %s%s rc 0x%08x; flexible %.1f MB, frame %.1f MB\n",
            v ? "OPEN" : "REFUSED", c.codec == VDEC2_CODEC_HEVC ? "HEVC" : c.codec == VDEC2_CODEC_AVC ? "H.264" : "VP9", c.profile,
            info.level, k[i].w, k[i].h, k[i].h > k[i].w ? "portrait" : k[i].h == k[i].w ? "square" : "landscape",
            v ? "" : "at ", v ? "" : stage, (unsigned)rc, info.cpu_bytes / 1048576.0, info.frame_bytes / 1048576.0);
        if (v)
            vdec2_close(v);
    }
}

/* After the log is up: install the backend if the module bound. */
void ps5wk_title_vdec_install(void);
void ps5wk_title_vdec_install(void)
{
    vdec2_set_logger(log_line);
    int off = open("/data/ps5webkit/no-hwdec", O_RDONLY);
    if (off >= 0) {
        close(off);
        fprintf(stderr, "[vdec] /data/ps5webkit/no-hwdec: hardware decoding off (%s)\n", vdec2_load_report());
        return;
    }
    if (s_load_rc != 0) {
        fprintf(stderr, "[vdec] libSceVideodec2 not bound (0x%08x): %s; software decoding only\n", (unsigned)s_load_rc, vdec2_load_report());
        return;
    }
    int probe = open("/data/ps5webkit/vdec-probe", O_RDONLY);
    if (probe >= 0) {
        close(probe);
        probe_sizes();
    }
    ps5wk_video_set_decoder_backend(&title_backend);
    s_installed = 1;
}

/* For the in-title WebProcess's media limit (ps5browser.c setUpInTitleWeb):
 * what the title's video server tells a payload WebProcess
 * (PS5VideoServer.cpp HELLO_ACK, ps5wk-child.cpp queryTitleDecoder). */
int ps5wk_title_vdec_max_size(int *width, int *height);
int ps5wk_title_vdec_max_size(int *width, int *height)
{
    *width = s_installed ? title_backend.max_width : 0;
    *height = s_installed ? title_backend.max_height : 0;
    return s_installed;
}
