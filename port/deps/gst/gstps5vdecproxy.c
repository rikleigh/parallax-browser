/* ps5vdecproxy: hardware video decoding for the PS5 port (Milestone 2 step 2b).
 *
 * Sony's decoder (libSceVideodec2) only works in an application process,
 * i.e. the browser title, not in the WebProcess payload. So this decoder
 * element decodes nothing itself: it sends each access unit to the title
 * (port/webkit/wpe-ps5/PS5VideoDecoder.cpp), which decodes it straight into
 * the GL buffers its compositor reads, and answers with PICTURE messages in
 * display order. For each picture this element outputs a placeholder buffer
 * (video/x-raw(memory:PS5Remote), a struct ps5wk_remote_picture in its
 * memory) that ps5videosink turns into a REMOTE frame: playbin, QoS, A/V sync
 * (the audio clock), MSE and WebKit's hole-punch geometry work as with
 * software decoding, and decoded frames never cross the process boundary.
 *
 * - Ranked above avdec_* (PRIMARY + 100) for H.264 (8-bit 4:2:0 up to
 *   4096x2304), HEVC Main/Main10 and VP9 profiles 0/2 (up to 8192x4352),
 *   progressive only. Anything else is refused in the accept-caps query,
 *   and so is any size the title cannot open a decoder for (the first
 *   accept-caps query asks it: DOPEN, kept for set_format); when the title
 *   has no decoder (ps5wk-shot, PS5WK_HWDEC=0, or
 *   /data/ps5webkit/no-hwdec in the title) the element fails to reach
 *   READY without posting an error: decodebin3 then takes the next decoder
 *   (avdec_*, dav1d), so software decoding stays the fallback.
 * - Input: AU-aligned H.264/HEVC in avc/avc3/hvc1/hev1 or byte-stream (the
 *   packetized formats are converted to Annex-B here, with codec_data's
 *   parameter sets before keyframes), VP9 frames/superframes.
 * - Flow: handle_frame sends the AU and keeps the frame pending; replies
 *   finish frames (or release them: VP9 hidden frames, frames before the
 *   first keyframe, errors). At most PS5_MAX_PENDING frames are pending.
 *   flush = DFLUSH with a new epoch (replies of older epochs are ignored),
 *   drain/finish = DDRAIN and wait for DRAINED.
 * - Waits for the title end at a flush or a stop (the sink pad's flushing
 *   flag, plus a wakeup pipe so the poll returns at once): the WebProcess
 *   main thread deactivates the pads when a page drops a video and waits
 *   for the streaming thread. If the title sends nothing at all for
 *   PS5_STALL_MS, the stream fails with a decode error instead.
 * Wire format: port/webkit/wpe-ps5/PS5VideoProtocol.h.
 */
#define PACKAGE "ps5webkit"
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideodecoder.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "PS5VideoProtocol.h"

/* Frames in flight before handle_frame waits for a picture. More than the
 * longest the hardware can hold before its first picture: an H.264/HEVC
 * stream without the VUI reorder limits makes the decoder fill the level's
 * whole DPB (16 frames at small sizes: 380x222 level 2.2) on top of
 * Videodec2's pipeline depth of 4. With 16 a VR site's hover previews
 * never got a picture (17 AUs sent, the title waiting for more). Below the
 * title's AU queue (64). */
#define PS5_MAX_PENDING 32
#define PS5_REPLY_TIMEOUT_MS 3000
#define PS5_STALL_MS 9000
#define PS5_REMOTE_FEATURE "memory:PS5Remote"

GST_DEBUG_CATEGORY_STATIC(ps5_vdec_proxy_debug);
#define GST_CAT_DEFAULT ps5_vdec_proxy_debug

#define GST_TYPE_PS5_VDEC_PROXY (gst_ps5_vdec_proxy_get_type())
G_DECLARE_FINAL_TYPE(GstPs5VdecProxy, gst_ps5_vdec_proxy, GST, PS5_VDEC_PROXY, GstVideoDecoder)

struct _GstPs5VdecProxy {
    GstVideoDecoder parent;
    int fd;
    int wake[2];             /* wakes a waiting poll at a flush or a stop */
    gint stopping;           /* PAUSED -> READY under way (atomic) */
    guint codecs;            /* HELLO_ACK */
    int max_width, max_height;
    guint decoder_id;        /* DOPEN_ACK */
    guint epoch;
    guint codec, profile, depth;
    int width, height;
    GstVideoCodecState *input_state;
    /* packetized H.264/HEVC -> Annex-B */
    guint nal_length_size;   /* 0: byte-stream already */
    GByteArray *param_sets;  /* from codec_data, Annex-B */
    GByteArray *au;          /* scratch */
    gboolean drained;
    /* counts */
    guint64 sent, sent_bytes, pictures, released, stale, clipped, other_epoch, late, replies;
    gint64 log_window;
    guint64 log_sent, log_pictures;
    double log_wait_ms;
};

G_DEFINE_TYPE(GstPs5VdecProxy, gst_ps5_vdec_proxy, GST_TYPE_VIDEO_DECODER)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-h264, stream-format = (string) { avc, avc3, byte-stream }, alignment = (string) au; "
                    "video/x-h265, stream-format = (string) { hvc1, hev1, byte-stream }, alignment = (string) au; "
                    "video/x-vp9"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE_WITH_FEATURES(PS5_REMOTE_FEATURE, "{ NV12, P010_10LE }")));

/* ------------------------------------------------------------ connection */

static gboolean send_all(GstPs5VdecProxy *self, const void *data, gsize size)
{
    const guint8 *p = data;
    while (size && self->fd >= 0) {
        ssize_t n = send(self->fd, p, size, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            GST_WARNING_OBJECT(self, "title connection lost (send errno %d)", errno);
            close(self->fd);
            self->fd = -1;
            return FALSE;
        }
        p += n;
        size -= (gsize)n;
    }
    return self->fd >= 0;
}

/* A flush or a stop is under way: waits for the title give up. */
static gboolean flushing(GstPs5VdecProxy *self)
{
    GstVideoDecoder *dec = GST_VIDEO_DECODER(self);
    return g_atomic_int_get(&self->stopping) || GST_PAD_IS_FLUSHING(GST_VIDEO_DECODER_SINK_PAD(dec))
        || GST_PAD_IS_FLUSHING(GST_VIDEO_DECODER_SRC_PAD(dec));
}

static void wake_up(GstPs5VdecProxy *self)
{
    if (self->wake[1] >= 0 && write(self->wake[1], "w", 1) < 0) {
        /* full already: a wakeup is pending anyway */
    }
}

/* The wakeup was seen (flush done, or stopped): later waits wait again. */
static void wake_clear(GstPs5VdecProxy *self)
{
    char buffer[64];
    while (self->wake[0] >= 0 && read(self->wake[0], buffer, sizeof buffer) > 0) {
    }
}

/* One whole message; FALSE on timeout (timeout_ms >= 0), a wakeup (left
 * pending until wake_clear) or a closed connection. */
static gboolean recv_msg(GstPs5VdecProxy *self, struct ps5wk_video_msg *msg, int timeout_ms)
{
    if (self->fd < 0)
        return FALSE;
    if (timeout_ms >= 0) {
        struct pollfd pfd[2] = { { self->fd, POLLIN, 0 }, { self->wake[0], POLLIN, 0 } };
        if (poll(pfd, self->wake[0] >= 0 ? 2 : 1, timeout_ms) <= 0 || !(pfd[0].revents & (POLLIN | POLLHUP | POLLERR)))
            return FALSE;
    }
    guint8 *p = (guint8 *)msg;
    gsize got = 0;
    while (got < sizeof *msg) {
        ssize_t n = recv(self->fd, p + got, sizeof *msg - got, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            GST_WARNING_OBJECT(self, "title connection closed");
            close(self->fd);
            self->fd = -1;
            return FALSE;
        }
        got += (gsize)n;
    }
    return TRUE;
}

static gboolean connect_title(GstPs5VdecProxy *self)
{
    if (self->fd >= 0)
        return TRUE;
    const char *path = g_getenv("PS5WK_VIDEO_SOCKET");
    if (!path || !*path || !g_strcmp0(g_getenv("PS5WK_HWDEC"), "0"))
        return FALSE;
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    g_strlcpy(address.sun_path, path, sizeof address.sun_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof address)) {
        if (fd >= 0)
            close(fd);
        return FALSE;
    }
    self->fd = fd;
    struct ps5wk_video_msg msg;
    memset(&msg, 0, sizeof msg);
    msg.type = PS5WK_VMSG_HELLO;
    msg.magic = PS5WK_VIDEO_MAGIC;
    msg.version = PS5WK_VIDEO_VERSION;
    msg.pid = (int32_t)getpid();
    msg.role = PS5WK_VROLE_DECODER;
    if (!send_all(self, &msg, sizeof msg))
        return FALSE;
    while (recv_msg(self, &msg, 2000)) {
        if (msg.type == PS5WK_VMSG_HELLO_ACK) {
            self->codecs = msg.role;
            self->max_width = (int)msg.width;
            self->max_height = (int)msg.height;
            if (self->codecs)
                return TRUE;
            break;
        }
    }
    if (self->fd >= 0)
        close(self->fd);
    self->fd = -1;
    return FALSE;
}

/* ------------------------------------------------------------ caps */

static guint codec_of(const GstStructure *s)
{
    const char *name = gst_structure_get_name(s);
    return !strcmp(name, "video/x-h264") ? PS5WK_VCODEC_H264 : !strcmp(name, "video/x-h265") ? PS5WK_VCODEC_HEVC
        : !strcmp(name, "video/x-vp9") ? PS5WK_VCODEC_VP9 : 0;
}

/* Bit depth the stream needs, 0 if the profile is not one the hardware has. */
static guint depth_of(guint codec, const GstStructure *s)
{
    const char *profile = gst_structure_get_string(s, "profile");
    const char *chroma = gst_structure_get_string(s, "chroma-format");
    guint luma = 0;
    gst_structure_get_uint(s, "bit-depth-luma", &luma);
    if (chroma && strcmp(chroma, "4:2:0"))
        return 0;
    switch (codec) {
    case PS5WK_VCODEC_H264:
        if (profile && strcmp(profile, "constrained-baseline") && strcmp(profile, "baseline") && strcmp(profile, "main")
            && strcmp(profile, "high") && strcmp(profile, "progressive-high") && strcmp(profile, "constrained-high"))
            return 0;
        return luma > 8 ? 0 : 8;
    case PS5WK_VCODEC_HEVC:
        if (profile && !strcmp(profile, "main-10"))
            return 10;
        if (profile && strcmp(profile, "main"))
            return 0;
        return luma > 10 ? 0 : luma == 10 ? 10 : 8;
    case PS5WK_VCODEC_VP9:
        if (profile && !strcmp(profile, "2"))
            return 10;
        if (profile && strcmp(profile, "0"))
            return 0;
        return luma > 8 ? 10 : 8;
    }
    return 0;
}

static gboolean caps_supported(GstPs5VdecProxy *self, GstCaps *caps, char *why, gsize why_size)
{
    if (!caps || gst_caps_is_empty(caps) || gst_caps_get_size(caps) < 1)
        return FALSE;
    const GstStructure *s = gst_caps_get_structure(caps, 0);
    guint codec = codec_of(s);
    if (!codec || !(self->codecs & codec)) {
        g_snprintf(why, why_size, "codec not in the title's decoder");
        return FALSE;
    }
    guint depth = depth_of(codec, s);
    if (!depth || (depth > 8 && !(self->codecs & PS5WK_VCODEC_10BIT))) {
        g_snprintf(why, why_size, "profile/bit depth/chroma not supported");
        return FALSE;
    }
    const char *interlace = gst_structure_get_string(s, "interlace-mode");
    if (interlace && strcmp(interlace, "progressive")) {
        g_snprintf(why, why_size, "interlaced");
        return FALSE;
    }
    int w = 0, h = 0;
    gst_structure_get_int(s, "width", &w);
    gst_structure_get_int(s, "height", &h);
    int max_w = codec == PS5WK_VCODEC_H264 ? 4096 : 8192, max_h = codec == PS5WK_VCODEC_H264 ? 2304 : 4352;
    max_w = MIN(max_w, self->max_width ? self->max_width : max_w);
    max_h = MIN(max_h, self->max_height ? self->max_height : max_h);
    if (w > max_w || h > max_h || (codec == PS5WK_VCODEC_H264 && (gint64)w * h > 4096 * 2304)) {
        g_snprintf(why, why_size, "%dx%d too large", w, h);
        return FALSE;
    }
    /* Videodec2 (13.60) holds HEVC streams to their own level more tightly
     * than the spec: a level 5.x stream taller than 2176 lines or wider than
     * 4096 (2048x4096, 2880x2880; legal 5.1, what encoders write) fails every
     * decode call (0x811d0111) even in a level 6.1 decoder. Software then. */
    const char *level = gst_structure_get_string(s, "level");
    if (codec == PS5WK_VCODEC_HEVC && (h > 2176 || w > 4096) && level && g_ascii_strtod(level, NULL) < 6.0) {
        g_snprintf(why, why_size, "HEVC level %s at %dx%d (the hardware wants level 6 above 2176 lines)", level, w, h);
        return FALSE;
    }
    return TRUE;
}

/* DOPEN: the title opens (or keeps) a decoder for this codec and size. */
static gboolean title_open(GstPs5VdecProxy *self, guint codec, guint depth, int w, int h)
{
    struct ps5wk_video_msg msg;
    memset(&msg, 0, sizeof msg);
    msg.type = PS5WK_VMSG_DOPEN;
    msg.role = codec;
    msg.ps5wk_profile = depth > 8 ? 2 : 1;
    msg.ps5wk_bit_depth = depth;
    msg.width = (uint32_t)MAX(w, 16);
    msg.height = (uint32_t)MAX(h, 16);
    msg.ps5wk_epoch = self->epoch;
    if (!send_all(self, &msg, sizeof msg))
        return FALSE;
    while (recv_msg(self, &msg, 6000)) {
        if (msg.type == PS5WK_VMSG_DOPEN_ACK) {
            if (msg.status == 0)
                self->decoder_id = msg.ps5wk_decoder;
            return msg.status == 0;
        }
        /* PICTUREs of the previous format, still in flight: dropped (released by the flush/drain before). */
    }
    return FALSE;
}

static gboolean ps5_sink_query(GstVideoDecoder *dec, GstQuery *query)
{
    GstPs5VdecProxy *self = GST_PS5_VDEC_PROXY(dec);
    if (GST_QUERY_TYPE(query) == GST_QUERY_ACCEPT_CAPS) {
        GstCaps *caps;
        gst_query_parse_accept_caps(query, &caps);
        GstCaps *templ = gst_pad_get_pad_template_caps(GST_VIDEO_DECODER_SINK_PAD(dec));
        gboolean ok = gst_caps_can_intersect(caps, templ);
        gst_caps_unref(templ);
        char why[96] = "not in the template";
        if (ok)
            ok = caps_supported(self, caps, why, sizeof why);
        /* decodebin3 asks this at READY, before linking, and takes the next
         * decoder (avdec_*) if we say no. So the title opens the decoder now:
         * whatever Videodec2 refuses falls back to software decoding instead
         * of failing playback in set_format (the first headset test's
         * 1920x3840 HEVC). set_format's DOPEN then keeps this decoder. Only
         * before the first decoder: later caps changes arrive with frames in
         * flight on the connection. */
        const GstStructure *s = ok ? gst_caps_get_structure(caps, 0) : NULL;
        int w = 0, h = 0;
        if (s && !self->decoder_id && gst_structure_get_int(s, "width", &w) && gst_structure_get_int(s, "height", &h) && w > 0 && h > 0) {
            guint codec = codec_of(s), depth = depth_of(codec, s);
            gint64 start = g_get_monotonic_time();
            ok = connect_title(self) && title_open(self, codec, depth, w, h);
            g_printerr("[ps5vdecproxy] %p: %s %dx%d %u-bit: %s in %.0f ms\n", (void *)self, gst_structure_get_name(s), w, h, depth,
                ok ? "the title opened a hardware decoder" : "the title cannot decode it, software decoding instead",
                (g_get_monotonic_time() - start) / 1000.0);
            if (!ok)
                g_snprintf(why, sizeof why, "the title could not open a decoder for %dx%d", w, h);
        }
        if (!ok)
            g_printerr("[ps5vdecproxy] %p: not decoding %s in hardware: %s\n", (void *)self,
                caps && gst_caps_get_size(caps) ? gst_structure_get_name(gst_caps_get_structure(caps, 0)) : "?", why);
        gst_query_set_accept_caps_result(query, ok);
        return TRUE;
    }
    return GST_VIDEO_DECODER_CLASS(gst_ps5_vdec_proxy_parent_class)->sink_query(dec, query);
}

/* codec_data (avcC / hvcC) -> NAL length size + Annex-B parameter sets. */
static void parse_codec_data(GstPs5VdecProxy *self, guint codec, GstBuffer *codec_data)
{
    g_byte_array_set_size(self->param_sets, 0);
    self->nal_length_size = 0;
    if (!codec_data)
        return;
    GstMapInfo map;
    if (!gst_buffer_map(codec_data, &map, GST_MAP_READ))
        return;
    const guint8 *d = map.data;
    gsize n = map.size, i;
    static const guint8 start[4] = { 0, 0, 0, 1 };
#define PUT(p, len) do { g_byte_array_append(self->param_sets, start, 4); g_byte_array_append(self->param_sets, (p), (len)); } while (0)
    if (codec == PS5WK_VCODEC_H264 && n >= 7 && d[0] == 1) {
        self->nal_length_size = (d[4] & 3) + 1;
        guint sps = d[5] & 0x1f;
        i = 6;
        for (guint k = 0; k < sps && i + 2 <= n; k++) {
            gsize len = (gsize)d[i] << 8 | d[i + 1];
            i += 2;
            if (i + len > n)
                break;
            PUT(d + i, len);
            i += len;
        }
        if (i < n) {
            guint pps = d[i++];
            for (guint k = 0; k < pps && i + 2 <= n; k++) {
                gsize len = (gsize)d[i] << 8 | d[i + 1];
                i += 2;
                if (i + len > n)
                    break;
                PUT(d + i, len);
                i += len;
            }
        }
    } else if (codec == PS5WK_VCODEC_HEVC && n >= 23 && d[0] == 1) {
        self->nal_length_size = (d[21] & 3) + 1;
        guint arrays = d[22];
        i = 23;
        for (guint a = 0; a < arrays && i + 3 <= n; a++) {
            guint count = (guint)d[i + 1] << 8 | d[i + 2];
            i += 3;
            for (guint k = 0; k < count && i + 2 <= n; k++) {
                gsize len = (gsize)d[i] << 8 | d[i + 1];
                i += 2;
                if (i + len > n)
                    break;
                PUT(d + i, len);
                i += len;
            }
        }
    }
#undef PUT
    gst_buffer_unmap(codec_data, &map);
}

static GstFlowReturn ps5_drain_all(GstPs5VdecProxy *self);

static gboolean ps5_set_format(GstVideoDecoder *dec, GstVideoCodecState *state)
{
    GstPs5VdecProxy *self = GST_PS5_VDEC_PROXY(dec);
    const GstStructure *s = gst_caps_get_structure(state->caps, 0);
    guint codec = codec_of(s), depth = depth_of(codec, s);
    int w = GST_VIDEO_INFO_WIDTH(&state->info), h = GST_VIDEO_INFO_HEIGHT(&state->info);
    if (!codec || !depth || !connect_title(self))
        return FALSE;
    const char *format = gst_structure_get_string(s, "stream-format");
    gboolean packetized = format && (!strcmp(format, "avc") || !strcmp(format, "avc3") || !strcmp(format, "hvc1") || !strcmp(format, "hev1"));
    parse_codec_data(self, codec, packetized ? state->codec_data : NULL);
    if (packetized && !self->nal_length_size)
        self->nal_length_size = 4;

    /* A new format on a running decoder (a resolution switch: GstVideoDecoder
     * doesn't drain on new caps): the pictures of the old format come out
     * first, then the title resets the hardware (DRAINED). Videodec2 failed
     * every H.264 decode call after an in-band 396x704 -> 540x960 switch
     * without it (BBC News). */
    gboolean drained = FALSE;
    if (self->decoder_id && self->width && (codec != self->codec || depth != self->depth || w != self->width || h != self->height)) {
        GstFlowReturn ret = ps5_drain_all(self);
        drained = TRUE;
        if (ret != GST_FLOW_OK && ret != GST_FLOW_FLUSHING)
            g_printerr("[ps5vdecproxy] %p: drain before the new format: %s\n", (void *)self, gst_flow_get_name(ret));
    }

    /* A new decoder only when it changes (a resolution switch keeps it if it fits; the title decides). */
    gint64 start = g_get_monotonic_time();
    gboolean ok = title_open(self, codec, depth, w, h);
    g_printerr("[ps5vdecproxy] %p: %s %dx%d %u-bit (%s): %s in %.0f ms%s%s\n", (void *)self, gst_structure_get_name(s), w, h, depth,
        format ? format : "frames", ok ? "hardware decoder" : "title refused", (g_get_monotonic_time() - start) / 1000.0,
        drained ? " (drained the old format first)" : "", ok ? "" : " (playback fails: the title accepted these caps before)");
    if (!ok)
        return FALSE;
    self->codec = codec;
    self->depth = depth;
    self->width = w;
    self->height = h;
    if (self->input_state)
        gst_video_codec_state_unref(self->input_state);
    self->input_state = gst_video_codec_state_ref(state);

    GstVideoCodecState *out = gst_video_decoder_set_output_state(dec, depth > 8 ? GST_VIDEO_FORMAT_P010_10LE : GST_VIDEO_FORMAT_NV12,
        w, h, state);
    if (out->caps)
        gst_caps_unref(out->caps);
    out->caps = gst_video_info_to_caps(&out->info);
    gst_caps_set_features(out->caps, 0, gst_caps_features_new(PS5_REMOTE_FEATURE, NULL));
    gst_video_codec_state_unref(out);
    return gst_video_decoder_negotiate(dec);
}

/* No pool of real frames: the output buffers are placeholders made here. */
static gboolean ps5_decide_allocation(GstVideoDecoder *dec, GstQuery *query)
{
    GstCaps *caps = NULL;
    gst_query_parse_allocation(query, &caps, NULL);
    GstBufferPool *pool = gst_buffer_pool_new();
    GstStructure *config = gst_buffer_pool_get_config(pool);
    gst_buffer_pool_config_set_params(config, caps, sizeof(struct ps5wk_remote_picture), 0, 0);
    gst_buffer_pool_set_config(pool, config);
    while (gst_query_get_n_allocation_pools(query))
        gst_query_remove_nth_allocation_pool(query, 0);
    gst_query_add_allocation_pool(query, pool, sizeof(struct ps5wk_remote_picture), 0, 0);
    gst_object_unref(pool);
    return TRUE;
}

/* ------------------------------------------------------------ replies */

static void maybe_log(GstPs5VdecProxy *self)
{
    gint64 now = g_get_monotonic_time();
    if (!self->log_window)
        self->log_window = now;
    if (now - self->log_window < 5 * G_USEC_PER_SEC)
        return;
    double s = (now - self->log_window) / 1e6;
    if (self->sent != self->log_sent)
        g_printerr("[ps5vdecproxy] %p: %.1f AUs/s sent (%.1f Mbit/s), %.1f pictures/s; waited %.0f ms for the title; "
                   "%" G_GUINT64_FORMAT " released, %" G_GUINT64_FORMAT " stale, %" G_GUINT64_FORMAT " clipped, %" G_GUINT64_FORMAT " late, %" G_GUINT64_FORMAT " of older epochs so far\n",
            (void *)self, (self->sent - self->log_sent) / s, self->sent_bytes * 8 / 1e6 / s,
            (self->pictures - self->log_pictures) / s, self->log_wait_ms, self->released, self->stale, self->clipped, self->late,
            self->other_epoch);
    self->log_window = now;
    self->log_sent = self->sent;
    self->log_pictures = self->pictures;
    self->log_wait_ms = 0;
    self->sent_bytes = 0;
}

static void send_prelease(GstPs5VdecProxy *self, guint token)
{
    struct ps5wk_video_msg msg;
    memset(&msg, 0, sizeof msg);
    msg.type = PS5WK_VMSG_PRELEASE;
    msg.ps5wk_token = token;
    send_all(self, &msg, sizeof msg);
}

/* Handles one reply. */
static GstFlowReturn handle_reply(GstPs5VdecProxy *self, const struct ps5wk_video_msg *msg)
{
    GstVideoDecoder *dec = GST_VIDEO_DECODER(self);
    self->replies++;
    if (msg->type == PS5WK_VMSG_DRAINED) {
        if (msg->ps5wk_epoch == self->epoch)
            self->drained = TRUE;
        return GST_FLOW_OK;
    }
    if (msg->type != PS5WK_VMSG_PICTURE)
        return GST_FLOW_OK;
    if (msg->ps5wk_epoch != self->epoch) {
        self->other_epoch++;
        return GST_FLOW_OK; /* flushed: the title released it */
    }
    GstVideoCodecFrame *frame = gst_video_decoder_get_frame(dec, (int)msg->ps5wk_key);
    if (msg->status) {
        if (frame) {
            self->released++;
            gst_video_decoder_release_frame(dec, frame);
        }
        return GST_FLOW_OK;
    }
    if (!frame) {
        send_prelease(self, msg->ps5wk_token);
        return GST_FLOW_OK;
    }
    /* Display order: pending frames before this one won't get a picture. */
    GList *frames = gst_video_decoder_get_frames(dec);
    for (GList *l = frames; l; l = l->next) {
        GstVideoCodecFrame *f = l->data;
        if (f != frame && GST_CLOCK_TIME_IS_VALID(f->pts) && GST_CLOCK_TIME_IS_VALID(frame->pts) && f->pts < frame->pts) {
            self->stale++;
            gst_video_decoder_release_frame(dec, gst_video_codec_frame_ref(f));
        }
    }
    g_list_free_full(frames, (GDestroyNotify)gst_video_codec_frame_unref);

    /* Outside the segment (decoding restarts at the keyframe before a seek
     * target): the base class would clip it and the sink never see it, so
     * the title's slot comes back now. */
    GstClockTime start = frame->pts;
    GstClockTime stop = GST_CLOCK_TIME_IS_VALID(start) && GST_CLOCK_TIME_IS_VALID(frame->duration) ? start + frame->duration : GST_CLOCK_TIME_NONE;
    if (GST_CLOCK_TIME_IS_VALID(start) && dec->output_segment.format == GST_FORMAT_TIME
        && !gst_segment_clip(&dec->output_segment, GST_FORMAT_TIME, start, stop, NULL, NULL)) {
        self->clipped++;
        send_prelease(self, msg->ps5wk_token);
        gst_video_decoder_release_frame(dec, frame);
        return GST_FLOW_OK;
    }
    /* Too late (the sink's QoS reports, as the base class would judge it):
     * dropped here, where the title can be told. */
    if (gst_video_decoder_get_max_decode_time(dec, frame) < 0) {
        self->late++;
        send_prelease(self, msg->ps5wk_token);
        gst_video_decoder_drop_frame(dec, frame);
        return GST_FLOW_OK;
    }
    struct ps5wk_remote_picture remote = { PS5WK_REMOTE_MAGIC, self->decoder_id, msg->ps5wk_token, msg->ps5wk_epoch };
    frame->output_buffer = gst_buffer_new_memdup(&remote, sizeof remote);
    self->pictures++;
    return gst_video_decoder_finish_frame(dec, frame);
}

static guint pending_frames(GstPs5VdecProxy *self)
{
    GList *frames = gst_video_decoder_get_frames(GST_VIDEO_DECODER(self));
    guint n = g_list_length(frames);
    g_list_free_full(frames, (GDestroyNotify)gst_video_codec_frame_unref);
    return n;
}

/* Replies already there (timeout 0), or wait up to timeout_ms for one. */
static GstFlowReturn read_replies(GstPs5VdecProxy *self, int timeout_ms)
{
    struct ps5wk_video_msg msg;
    GstFlowReturn ret = GST_FLOW_OK;
    gboolean first = TRUE;
    while (ret == GST_FLOW_OK && recv_msg(self, &msg, first ? timeout_ms : 0)) {
        first = FALSE;
        ret = handle_reply(self, &msg);
    }
    if (self->fd < 0 && ret == GST_FLOW_OK)
        ret = GST_FLOW_ERROR;
    return ret;
}

/* ------------------------------------------------------------ decoding */

/* The AU for the title: Annex-B (packetized input converted), parameter
 * sets from codec_data before keyframes. */
static const guint8 *build_au(GstPs5VdecProxy *self, const guint8 *data, gsize size, gboolean keyframe, gsize *out_size)
{
    if (self->codec == PS5WK_VCODEC_VP9 || !self->nal_length_size) {
        *out_size = size;
        return data;
    }
    GByteArray *au = self->au;
    g_byte_array_set_size(au, 0);
    static const guint8 start[4] = { 0, 0, 0, 1 };
    gsize i = 0, ls = self->nal_length_size;
    /* codec_data's parameter sets only when the keyframe carries no SPS and
     * PPS of its own (avc3/hev1, or h264parse keeping them in band): two
     * copies that differ (codec_data written before the encoder changed a
     * PPS) put two different sets with one ID in an AU, which Videodec2
     * refuses (0x811d0301, PS5VR's HEVC remuxes). */
    if (keyframe && self->param_sets->len) {
        gboolean sps = FALSE, pps = FALSE, vps = self->codec == PS5WK_VCODEC_H264;
        for (gsize k = 0; k + ls < size;) {
            gsize len = 0;
            for (gsize b = 0; b < ls; b++)
                len = len << 8 | data[k + b];
            k += ls;
            if (!len || len > size - k)
                break;
            guint type = self->codec == PS5WK_VCODEC_H264 ? data[k] & 0x1f : (data[k] >> 1) & 0x3f;
            sps |= self->codec == PS5WK_VCODEC_H264 ? type == 7 : type == 33;
            pps |= self->codec == PS5WK_VCODEC_H264 ? type == 8 : type == 34;
            vps |= self->codec != PS5WK_VCODEC_H264 && type == 32;
            k += len;
        }
        if (!sps || !pps || !vps)
            g_byte_array_append(au, self->param_sets->data, self->param_sets->len);
    }
    while (i + ls <= size) {
        gsize len = 0;
        for (gsize k = 0; k < ls; k++)
            len = len << 8 | data[i + k];
        i += ls;
        if (len > size - i)
            break;
        g_byte_array_append(au, start, 4);
        g_byte_array_append(au, data + i, len);
        i += len;
    }
    *out_size = au->len;
    return au->data;
}

static GstFlowReturn ps5_handle_frame(GstVideoDecoder *dec, GstVideoCodecFrame *frame)
{
    GstPs5VdecProxy *self = GST_PS5_VDEC_PROXY(dec);
    if (self->fd < 0 || !self->decoder_id) {
        gst_video_decoder_release_frame(dec, frame);
        return GST_FLOW_NOT_NEGOTIATED;
    }
    GstMapInfo map;
    if (!gst_buffer_map(frame->input_buffer, &map, GST_MAP_READ)) {
        gst_video_decoder_release_frame(dec, frame);
        return GST_FLOW_ERROR;
    }
    gboolean keyframe = GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT(frame);
    gsize size = 0;
    const guint8 *au = build_au(self, map.data, map.size, keyframe, &size);
    struct ps5wk_video_msg msg;
    memset(&msg, 0, sizeof msg);
    msg.type = PS5WK_VMSG_AU;
    msg.payload_bytes = (uint32_t)size;
    msg.ps5wk_key = frame->system_frame_number;
    msg.ps5wk_epoch = self->epoch;
    msg.ps5wk_flags = keyframe ? PS5WK_VAU_KEYFRAME : 0;
    msg.pts_ns = GST_CLOCK_TIME_IS_VALID(frame->pts) ? (int64_t)frame->pts : -1;
    gboolean ok = send_all(self, &msg, sizeof msg) && send_all(self, au, size);
    gst_buffer_unmap(frame->input_buffer, &map);
    gst_video_codec_frame_unref(frame); /* the decoder base class keeps it pending */
    if (!ok)
        return GST_FLOW_ERROR;
    self->sent++;
    self->sent_bytes += size;

    GstFlowReturn ret = read_replies(self, 0);
    gint64 start = g_get_monotonic_time(), logged = start, progress = start;
    guint64 replies = self->replies;
    while (ret == GST_FLOW_OK && pending_frames(self) > PS5_MAX_PENDING) {
        if (flushing(self)) {
            ret = GST_FLOW_FLUSHING;
            break;
        }
        ret = read_replies(self, self->wake[0] >= 0 ? PS5_REPLY_TIMEOUT_MS : 100);
        gint64 now = g_get_monotonic_time();
        if (self->replies != replies) {
            replies = self->replies;
            progress = now;
        }
        if (ret == GST_FLOW_OK && now - logged > PS5_REPLY_TIMEOUT_MS * 1000) {
            g_printerr("[ps5vdecproxy] %p: no picture from the title for %d ms with %u frames pending (%" G_GUINT64_FORMAT " AUs sent, %"
                       G_GUINT64_FORMAT " pictures, %" G_GUINT64_FORMAT " released, %" G_GUINT64_FORMAT " stale, %" G_GUINT64_FORMAT
                       " clipped, %" G_GUINT64_FORMAT " of older epochs)\n", (void *)self, (int)((now - progress) / 1000), pending_frames(self),
                self->sent, self->pictures, self->released, self->stale, self->clipped, self->other_epoch);
            logged = now;
        }
        /* The title's decoder is stuck: fail this stream (the page gets a
         * decode error) rather than hold the streaming thread for good. */
        if (ret == GST_FLOW_OK && now - progress > PS5_STALL_MS * 1000) {
            GST_ELEMENT_ERROR(self, STREAM, DECODE, ("The hardware video decoder stopped answering"),
                ("no reply from the title for %d ms with %u frames pending (%" G_GUINT64_FORMAT " AUs sent, %" G_GUINT64_FORMAT " pictures)",
                    PS5_STALL_MS, pending_frames(self), self->sent, self->pictures));
            g_printerr("[ps5vdecproxy] %p: the title's decoder is stuck (%" G_GUINT64_FORMAT " AUs sent, %" G_GUINT64_FORMAT
                       " pictures): stream failed\n", (void *)self, self->sent, self->pictures);
            ret = GST_FLOW_ERROR;
        }
    }
    self->log_wait_ms += (g_get_monotonic_time() - start) / 1000.0;
    maybe_log(self);
    return ret;
}

static GstFlowReturn ps5_drain_all(GstPs5VdecProxy *self)
{
    if (self->fd < 0 || !self->decoder_id)
        return GST_FLOW_OK;
    struct ps5wk_video_msg msg;
    memset(&msg, 0, sizeof msg);
    msg.type = PS5WK_VMSG_DDRAIN;
    msg.ps5wk_epoch = self->epoch;
    self->drained = FALSE;
    if (!send_all(self, &msg, sizeof msg))
        return GST_FLOW_ERROR;
    gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    GstFlowReturn ret = GST_FLOW_OK;
    while (!self->drained && ret == GST_FLOW_OK && g_get_monotonic_time() < deadline) {
        if (flushing(self)) {
            ret = GST_FLOW_FLUSHING;
            break;
        }
        ret = read_replies(self, 200);
    }
    if (!self->drained && ret != GST_FLOW_FLUSHING)
        g_printerr("[ps5vdecproxy] %p: drain did not finish (%s)\n", (void *)self, gst_flow_get_name(ret));
    return ret == GST_FLOW_ERROR && self->fd >= 0 ? GST_FLOW_OK : ret;
}

static GstFlowReturn ps5_finish(GstVideoDecoder *dec)
{
    return ps5_drain_all(GST_PS5_VDEC_PROXY(dec));
}

static GstFlowReturn ps5_drain(GstVideoDecoder *dec)
{
    return ps5_drain_all(GST_PS5_VDEC_PROXY(dec));
}

static gboolean ps5_flush(GstVideoDecoder *dec)
{
    GstPs5VdecProxy *self = GST_PS5_VDEC_PROXY(dec);
    wake_clear(self);
    self->epoch++;
    if (self->fd >= 0 && self->decoder_id) {
        struct ps5wk_video_msg msg;
        memset(&msg, 0, sizeof msg);
        msg.type = PS5WK_VMSG_DFLUSH;
        msg.ps5wk_epoch = self->epoch;
        send_all(self, &msg, sizeof msg);
    }
    return TRUE;
}

static gboolean ps5_start(GstVideoDecoder *dec)
{
    GstPs5VdecProxy *self = GST_PS5_VDEC_PROXY(dec);
    self->epoch = 1;
    self->sent = self->pictures = self->released = self->stale = self->clipped = self->other_epoch = self->late = self->replies = 0;
    wake_clear(self);
    g_atomic_int_set(&self->stopping, 0);
    return TRUE;
}

static gboolean ps5_stop(GstVideoDecoder *dec)
{
    GstPs5VdecProxy *self = GST_PS5_VDEC_PROXY(dec);
    if (self->sent)
        g_printerr("[ps5vdecproxy] %p: stop after %" G_GUINT64_FORMAT " AUs, %" G_GUINT64_FORMAT " pictures, %" G_GUINT64_FORMAT
                   " released, %" G_GUINT64_FORMAT " stale, %" G_GUINT64_FORMAT " clipped, %" G_GUINT64_FORMAT " dropped late\n",
            (void *)self, self->sent, self->pictures, self->released, self->stale, self->clipped, self->late);
    if (self->input_state)
        gst_video_codec_state_unref(self->input_state);
    self->input_state = NULL;
    self->decoder_id = 0;
    if (self->fd >= 0)
        close(self->fd); /* the title closes its decoder */
    self->fd = -1;
    wake_clear(self);
    g_atomic_int_set(&self->stopping, 0);
    return TRUE;
}

/* FLUSH_START (the pad is flushing already): a wait for the title ends now. */
static gboolean ps5_sink_event(GstVideoDecoder *dec, GstEvent *event)
{
    if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START)
        wake_up(GST_PS5_VDEC_PROXY(dec));
    return GST_VIDEO_DECODER_CLASS(gst_ps5_vdec_proxy_parent_class)->sink_event(dec, event);
}

/* Fail READY quietly when the title has no decoder: decodebin3 then tries
 * the next decoder (GstVideoDecoder's own open() failure posts an error). */
static GstStateChangeReturn ps5_change_state(GstElement *element, GstStateChange transition)
{
    GstPs5VdecProxy *self = GST_PS5_VDEC_PROXY(element);
    if (transition == GST_STATE_CHANGE_NULL_TO_READY && !connect_title(self)) {
        GST_INFO_OBJECT(self, "no hardware decoder in the title");
        return GST_STATE_CHANGE_FAILURE;
    }
    /* Deactivating the sink pad waits for handle_frame to return (with the
     * WebProcess main thread, when a page removes a video): end its wait. */
    if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
        g_atomic_int_set(&self->stopping, 1);
        wake_up(self);
    }
    GstStateChangeReturn ret = GST_ELEMENT_CLASS(gst_ps5_vdec_proxy_parent_class)->change_state(element, transition);
    if (transition == GST_STATE_CHANGE_READY_TO_NULL && self->fd >= 0) {
        close(self->fd);
        self->fd = -1;
    }
    return ret;
}

/* ------------------------------------------------------------ GObject */

static void ps5_finalize(GObject *object)
{
    GstPs5VdecProxy *self = GST_PS5_VDEC_PROXY(object);
    if (self->fd >= 0)
        close(self->fd);
    for (int i = 0; i < 2; i++) {
        if (self->wake[i] >= 0)
            close(self->wake[i]);
    }
    g_byte_array_unref(self->param_sets);
    g_byte_array_unref(self->au);
    G_OBJECT_CLASS(gst_ps5_vdec_proxy_parent_class)->finalize(object);
}

static void gst_ps5_vdec_proxy_init(GstPs5VdecProxy *self)
{
    self->fd = -1;
    /* Non-blocking both ends (wake_clear reads until empty); without it the
     * waits just poll in short steps. */
    self->wake[0] = self->wake[1] = -1;
    int p[2];
    if (!pipe(p)) {
        gboolean ok = TRUE;
        for (int i = 0; i < 2; i++) {
            fcntl(p[i], F_SETFL, fcntl(p[i], F_GETFL) | O_NONBLOCK);
            fcntl(p[i], F_SETFD, FD_CLOEXEC);
            ok = ok && (fcntl(p[i], F_GETFL) & O_NONBLOCK);
        }
        if (ok) {
            self->wake[0] = p[0];
            self->wake[1] = p[1];
        } else {
            close(p[0]);
            close(p[1]);
        }
    }
    self->param_sets = g_byte_array_new();
    self->au = g_byte_array_new();
    gst_video_decoder_set_packetized(GST_VIDEO_DECODER(self), TRUE);
    gst_video_decoder_set_needs_format(GST_VIDEO_DECODER(self), TRUE);
    /* No QoS drops in the base class: it drops late output buffers inside
     * finish_frame without telling us, so the title never got those slots
     * back; once a burst of late frames had taken all of them, nothing could
     * be decoded in time again (TV test 2026-10-28: YouTube 4K stuck after
     * a few seconds). handle_reply drops late pictures itself, with a
     * PRELEASE, and ps5videosink doesn't drop them either (max-lateness -1). */
    g_object_set(self, "qos", FALSE, NULL);
}

static void gst_ps5_vdec_proxy_class_init(GstPs5VdecProxyClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstVideoDecoderClass *dec_class = GST_VIDEO_DECODER_CLASS(klass);
    object_class->finalize = ps5_finalize;
    element_class->change_state = ps5_change_state;
    gst_element_class_set_static_metadata(element_class, "PS5 hardware video decoder (in the browser title)",
        "Codec/Decoder/Video/Hardware", "Sends access units to the title, which decodes them with Videodec2 for hole-punch video",
        "ps5webkit");
    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_add_static_pad_template(element_class, &src_template);
    dec_class->start = ps5_start;
    dec_class->stop = ps5_stop;
    dec_class->set_format = ps5_set_format;
    dec_class->handle_frame = ps5_handle_frame;
    dec_class->finish = ps5_finish;
    dec_class->drain = ps5_drain;
    dec_class->flush = ps5_flush;
    dec_class->sink_query = ps5_sink_query;
    dec_class->sink_event = ps5_sink_event;
    dec_class->decide_allocation = ps5_decide_allocation;
}

gboolean ps5_vdec_proxy_register(GstPlugin *plugin);
gboolean ps5_vdec_proxy_register(GstPlugin *plugin)
{
    GST_DEBUG_CATEGORY_INIT(ps5_vdec_proxy_debug, "ps5vdecproxy", 0, "PS5 hardware video decoder proxy");
    /* Above avdec_* (PRIMARY) and dav1d. */
    return gst_element_register(plugin, "ps5vdecproxy", GST_RANK_PRIMARY + 100, GST_TYPE_PS5_VDEC_PROXY);
}
