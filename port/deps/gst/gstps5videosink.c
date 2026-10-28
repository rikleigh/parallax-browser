/* ps5videosink: hole-punch video sink for the PS5 port (Milestone 2 step 2a).
 *
 * WebKit's MediaPlayerPrivateGStreamer makes it (patch 0017) instead of its
 * appsink-based webkitvideosink. Decoded frames do not go into the page:
 * the page punches a transparent hole where the <video> is, and this sink
 * hands each frame to the browser title through shared memory, where the
 * title draws it under the page bitmap (port/webkit/wpe-ps5/PS5VideoServer.cpp,
 * PS5Renderer.cpp). Video frames therefore never repaint the page.
 *
 * - Transport: one unix stream connection per sink to PS5WK_VIDEO_SOCKET
 *   (set by ps5wk-child: /system_tmp/ps5wk/video-<UIProcess pid>.sock), a
 *   ring of PS5_SLOTS frame slots in anonymous shared memory whose fd goes
 *   over the connection (SCM_RIGHTS), 128-byte messages
 *   (port/webkit/wpe-ps5/PS5VideoProtocol.h). The title copies a slot into
 *   GPU memory when it shows the frame and releases it.
 * - Hardware decoding (step 2b): buffers from ps5vdecproxy
 *   (video/x-raw(memory:PS5Remote)) name a picture the title decoded
 *   itself; they go out as REMOTE frames (decoder id + token), with no
 *   shared memory at all.
 * - Formats: I420 and NV12 (what avdec_* produce for 8-bit 4:2:0; playsink
 *   converts anything else). Planes are copied into the slot with 64-byte
 *   aligned strides; the title converts YUV to RGB on the GPU.
 * - Timing: the sink runs PS5_LEAD early (get_times) and stamps each
 *   frame with the CLOCK_MONOTONIC time at which it is due, from the
 *   pipeline clock (the audio sink's clock when there is audio, so frames
 *   follow what the title's sceAudioOut has played). The title shows each
 *   frame at the first vsync at or after (nearly) that time. The preroll
 *   frame is shown at once.
 * - WebKit still gets every sample through "repaint-requested" (for the
 *   video size, current-time tasks and canvas drawImage), but no longer
 *   repaints for it.
 * - ps5_video_sink_set_geometry()/ps5_video_sink_set_visible(): called by
 *   WebKit's paint with the hole's rectangle in page-buffer pixels.
 * - "stats" (rendered/dropped, read by WebKit for getVideoPlaybackQuality)
 *   counts the frames the title showed and dropped, plus the sink's own
 *   QoS drops.
 */
#define PACKAGE "ps5webkit"
#include <gst/gst.h>
#include <gst/video/gstvideosink.h>
#include <gst/video/video.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Wire format: port/webkit/wpe-ps5/PS5VideoProtocol.h (build.sh puts it on the include path). */
#include "PS5VideoProtocol.h"

#define PS5_SLOTS 6
#define PS5_LEAD (40 * GST_MSECOND)  /* frames reach the title this much early */
#define PS5_SLOT_WAIT_MS 100         /* then a frame is dropped */
#define PS5_MIN_SLOT_BYTES (1920 * 1088 * 3 / 2) /* 1080p without reallocating */

GST_DEBUG_CATEGORY_STATIC(ps5_video_sink_debug);
#define GST_CAT_DEFAULT ps5_video_sink_debug

#define GST_TYPE_PS5_VIDEO_SINK (gst_ps5_video_sink_get_type())
G_DECLARE_FINAL_TYPE(GstPs5VideoSink, gst_ps5_video_sink, GST, PS5_VIDEO_SINK, GstVideoSink)

struct _GstPs5VideoSink {
    GstVideoSink parent;

    GMutex lock;             /* fd writes, geometry, slots */
    int fd;                  /* connection to the title, -1 */
    gboolean connect_failed; /* do not retry every frame */
    gboolean flushing;

    /* shared memory ring */
    int shm_fd;
    guint8 *shm;
    gsize slot_bytes;
    guint generation;
    gboolean busy[PS5_SLOTS];
    guint next_slot;

    /* negotiated format */
    GstVideoInfo info;
    gboolean have_info;
    guint format, colorimetry;
    gboolean remote; /* ps5vdecproxy's placeholders */
    gint64 default_max_lateness;
    guint64 remote_sent, remote_dropped;
    struct ps5wk_remote_picture prepared; /* the last placeholder prepare() saw */
    gboolean prepared_pending;            /* ... and not rendered yet */
    guint stride[3], offset[3];
    gsize frame_bytes;

    /* CLOCK_MONOTONIC - pipeline clock, smoothed (ns) */
    gint64 clock_offset;
    gboolean have_offset;

    /* geometry (page-buffer pixels) */
    int rect[4];
    gboolean visible, have_geometry, geometry_sent;

    /* counts */
    guint64 sent, no_slot;
    guint32 title_displayed, title_dropped;
    gint64 log_window;
    guint64 log_sent, log_no_slot;
    gint64 lead_sum_us, lead_min_us;
    guint lead_count;
};

enum { PROP_0, PROP_STATS };
enum { SIGNAL_REPAINT_REQUESTED, LAST_SIGNAL };
static guint signals[LAST_SIGNAL];

G_DEFINE_TYPE(GstPs5VideoSink, gst_ps5_video_sink, GST_TYPE_VIDEO_SINK)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE("{ I420, NV12 }") "; "
        GST_VIDEO_CAPS_MAKE_WITH_FEATURES("memory:PS5Remote", "{ NV12, P010_10LE }")));

/* ------------------------------------------------------------ connection */

static int send_msg(GstPs5VideoSink *self, const struct ps5wk_video_msg *msg, int fd_to_pass)
{
    if (self->fd < 0)
        return -1;
    struct iovec iov = { (void *)msg, sizeof *msg };
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    char control[CMSG_SPACE(sizeof(int))];
    if (fd_to_pass >= 0) {
        memset(control, 0, sizeof control);
        mh.msg_control = control;
        mh.msg_controllen = sizeof control;
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd_to_pass, sizeof(int));
    }
    for (;;) {
        ssize_t n = sendmsg(self->fd, &mh, 0);
        if (n == (ssize_t)sizeof *msg)
            return 0;
        if (n < 0 && errno == EINTR)
            continue;
        GST_WARNING_OBJECT(self, "video server connection lost (sendmsg %zd, errno %d)", n, errno);
        close(self->fd);
        self->fd = -1;
        return -1;
    }
}

static void send_geometry_locked(GstPs5VideoSink *self)
{
    if (self->fd < 0 || !self->have_geometry)
        return;
    struct ps5wk_video_msg msg;
    memset(&msg, 0, sizeof msg);
    msg.type = PS5WK_VMSG_GEOMETRY;
    msg.visible = self->visible;
    memcpy(msg.rect, self->rect, sizeof msg.rect);
    if (!send_msg(self, &msg, -1))
        self->geometry_sent = TRUE;
}

static void free_shm_locked(GstPs5VideoSink *self)
{
    if (self->shm)
        munmap(self->shm, self->slot_bytes * PS5_SLOTS);
    if (self->shm_fd >= 0)
        close(self->shm_fd);
    self->shm = NULL;
    self->shm_fd = -1;
    self->slot_bytes = 0;
    memset(self->busy, 0, sizeof self->busy);
}

/* A ring big enough for frame_bytes, announced to the title. */
static gboolean ensure_shm_locked(GstPs5VideoSink *self, gsize frame_bytes)
{
    if (self->shm && self->slot_bytes >= frame_bytes)
        return TRUE;
    free_shm_locked(self);
    gsize slot = MAX(frame_bytes, (gsize)PS5_MIN_SLOT_BYTES);
    slot = (slot + 0xffff) & ~(gsize)0xffff;
    int fd = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
    if (fd < 0 || ftruncate(fd, (off_t)(slot * PS5_SLOTS))) {
        GST_ERROR_OBJECT(self, "shared memory for %u x %zu bytes: errno %d", PS5_SLOTS, slot, errno);
        if (fd >= 0)
            close(fd);
        return FALSE;
    }
    void *p = mmap(NULL, slot * PS5_SLOTS, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        GST_ERROR_OBJECT(self, "mmap of %zu bytes: errno %d", slot * PS5_SLOTS, errno);
        close(fd);
        return FALSE;
    }
    self->shm_fd = fd;
    self->shm = p;
    self->slot_bytes = slot;
    self->generation++;
    self->next_slot = 0;
    struct ps5wk_video_msg msg;
    memset(&msg, 0, sizeof msg);
    msg.type = PS5WK_VMSG_SHM;
    msg.slots = PS5_SLOTS;
    msg.slot_bytes = (uint32_t)slot;
    msg.generation = self->generation;
    send_msg(self, &msg, fd);
    g_printerr("[ps5videosink] %p: %u slots of %.1f MB (generation %u)\n", (void *)self, PS5_SLOTS, slot / 1048576.0, self->generation);
    return TRUE;
}

static void connect_locked(GstPs5VideoSink *self)
{
    if (self->fd >= 0 || self->connect_failed)
        return;
    const char *path = g_getenv("PS5WK_VIDEO_SOCKET");
    if (!path || !*path) {
        self->connect_failed = TRUE;
        g_printerr("[ps5videosink] no PS5WK_VIDEO_SOCKET: frames are dropped\n");
        return;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    g_strlcpy(address.sun_path, path, sizeof address.sun_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof address)) {
        g_printerr("[ps5videosink] connect %s: errno %d; frames are dropped\n", path, errno);
        if (fd >= 0)
            close(fd);
        self->connect_failed = TRUE;
        return;
    }
    self->fd = fd;
    free_shm_locked(self); /* a new connection gets a new ring (SHM message) */
    struct ps5wk_video_msg msg;
    memset(&msg, 0, sizeof msg);
    msg.type = PS5WK_VMSG_HELLO;
    msg.magic = PS5WK_VIDEO_MAGIC;
    msg.version = PS5WK_VIDEO_VERSION;
    msg.pid = (int32_t)getpid();
    send_msg(self, &msg, -1);
    self->title_displayed = self->title_dropped = 0;
    self->geometry_sent = FALSE;
    send_geometry_locked(self);
    g_printerr("[ps5videosink] %p: connected to %s\n", (void *)self, path);
}

static void disconnect_locked(GstPs5VideoSink *self)
{
    if (self->fd >= 0)
        close(self->fd);
    self->fd = -1;
    free_shm_locked(self);
    self->connect_failed = FALSE;
}

/* Reads RELEASE messages; waits up to timeout_ms for one. */
static void read_releases_locked(GstPs5VideoSink *self, int timeout_ms)
{
    while (self->fd >= 0) {
        struct ps5wk_video_msg msg;
        ssize_t n = recv(self->fd, &msg, sizeof msg, MSG_DONTWAIT | MSG_PEEK);
        if (n < (ssize_t)sizeof msg) {
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                GST_WARNING_OBJECT(self, "video server closed the connection");
                close(self->fd);
                self->fd = -1;
                return;
            }
            if (timeout_ms <= 0)
                return;
            int fd = self->fd;
            g_mutex_unlock(&self->lock);
            struct pollfd pfd = { fd, POLLIN, 0 };
            poll(&pfd, 1, MIN(timeout_ms, 10));
            g_mutex_lock(&self->lock);
            timeout_ms -= 10;
            if (self->flushing)
                return;
            continue;
        }
        n = recv(self->fd, &msg, sizeof msg, 0);
        if (n != (ssize_t)sizeof msg)
            continue;
        if (msg.type == PS5WK_VMSG_RELEASE) {
            if (msg.generation == self->generation && msg.slot < PS5_SLOTS)
                self->busy[msg.slot] = FALSE;
            self->title_displayed = msg.displayed;
            self->title_dropped = msg.dropped;
            timeout_ms = 0; /* got one: stop waiting */
        }
    }
}

static int free_slot_locked(GstPs5VideoSink *self)
{
    for (guint i = 0; i < PS5_SLOTS; i++) {
        guint s = (self->next_slot + i) % PS5_SLOTS;
        if (!self->busy[s])
            return (int)s;
    }
    return -1;
}

/* ------------------------------------------------------------ geometry API */

void ps5_video_sink_set_geometry(GstElement *element, int x, int y, int width, int height);
void ps5_video_sink_set_visible(GstElement *element, gboolean visible);

void ps5_video_sink_set_geometry(GstElement *element, int x, int y, int width, int height)
{
    if (!element || !GST_IS_PS5_VIDEO_SINK(element))
        return;
    GstPs5VideoSink *self = GST_PS5_VIDEO_SINK(element);
    g_mutex_lock(&self->lock);
    int rect[4] = { x, y, width, height };
    if (!self->have_geometry || !self->visible || memcmp(rect, self->rect, sizeof rect) || !self->geometry_sent) {
        memcpy(self->rect, rect, sizeof rect);
        self->visible = TRUE;
        self->have_geometry = TRUE;
        self->geometry_sent = FALSE;
        send_geometry_locked(self);
    }
    g_mutex_unlock(&self->lock);
}

void ps5_video_sink_set_visible(GstElement *element, gboolean visible)
{
    if (!element || !GST_IS_PS5_VIDEO_SINK(element))
        return;
    GstPs5VideoSink *self = GST_PS5_VIDEO_SINK(element);
    g_mutex_lock(&self->lock);
    if (self->have_geometry && self->visible != !!visible) {
        self->visible = !!visible;
        send_geometry_locked(self);
    }
    g_mutex_unlock(&self->lock);
}

/* ------------------------------------------------------------ GstBaseSink */

static gboolean ps5_start(GstBaseSink *bsink)
{
    GstPs5VideoSink *self = GST_PS5_VIDEO_SINK(bsink);
    g_mutex_lock(&self->lock);
    self->connect_failed = FALSE;
    connect_locked(self);
    self->have_offset = FALSE;
    self->sent = self->no_slot = 0;
    g_mutex_unlock(&self->lock);
    return TRUE;
}

static gboolean ps5_stop(GstBaseSink *bsink)
{
    GstPs5VideoSink *self = GST_PS5_VIDEO_SINK(bsink);
    g_mutex_lock(&self->lock);
    if (self->sent)
        g_printerr("[ps5videosink] %p: stop after %" G_GUINT64_FORMAT " frames sent (title: %u shown, %u dropped; %" G_GUINT64_FORMAT
                   " without a free slot; %" G_GUINT64_FORMAT " hardware-decoded, %" G_GUINT64_FORMAT " of them dropped here as late)\n",
            (void *)self, self->sent, self->title_displayed, self->title_dropped, self->no_slot, self->remote_sent, self->remote_dropped);
    disconnect_locked(self);
    g_mutex_unlock(&self->lock);
    return TRUE;
}

static gboolean ps5_set_caps(GstBaseSink *bsink, GstCaps *caps)
{
    GstPs5VideoSink *self = GST_PS5_VIDEO_SINK(bsink);
    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, caps))
        return FALSE;
    guint w = GST_VIDEO_INFO_WIDTH(&info), h = GST_VIDEO_INFO_HEIGHT(&info);
    guint ch = (h + 1) / 2, cw = (w + 1) / 2;
    GstCapsFeatures *features = gst_caps_get_features(caps, 0);
    gboolean remote = features && gst_caps_features_contains(features, "memory:PS5Remote");
    /* Hardware-decoded frames are never dropped here: GstBaseSink drops a
     * late buffer before prepare() and render(), so the title never learnt
     * that its picture was gone, and a burst of late frames (YouTube under
     * load, TV test 2026-10-28) left every decoder slot held: playback stuck.
     * The title shows the newest due frame at each vsync and gives the
     * skipped ones back, so late frames are dropped there instead. */
    gst_base_sink_set_max_lateness(bsink, remote ? -1 : self->default_max_lateness);
    g_mutex_lock(&self->lock);
    self->info = info;
    self->have_info = TRUE;
    self->remote = remote;
    if (remote) {
        self->format = PS5WK_VFMT_REMOTE;
        memset(self->stride, 0, sizeof self->stride);
        memset(self->offset, 0, sizeof self->offset);
        self->frame_bytes = 0;
    } else
    switch (GST_VIDEO_INFO_FORMAT(&info)) {
    case GST_VIDEO_FORMAT_I420:
        self->format = PS5WK_VFMT_I420;
        self->stride[0] = GST_ROUND_UP_64(w);
        self->stride[1] = self->stride[2] = GST_ROUND_UP_64(cw);
        self->offset[0] = 0;
        self->offset[1] = self->stride[0] * h;
        self->offset[2] = self->offset[1] + self->stride[1] * ch;
        self->frame_bytes = self->offset[2] + (gsize)self->stride[2] * ch;
        break;
    case GST_VIDEO_FORMAT_NV12:
        self->format = PS5WK_VFMT_NV12;
        self->stride[0] = self->stride[1] = GST_ROUND_UP_64(w);
        self->stride[2] = 0;
        self->offset[0] = 0;
        self->offset[1] = self->stride[0] * h;
        self->offset[2] = 0;
        self->frame_bytes = self->offset[1] + (gsize)self->stride[1] * ch;
        break;
    default:
        g_mutex_unlock(&self->lock);
        return FALSE;
    }
    GstVideoColorimetry *c = &GST_VIDEO_INFO_COLORIMETRY(&info);
    gboolean bt709 = c->matrix == GST_VIDEO_COLOR_MATRIX_BT709 || (c->matrix == GST_VIDEO_COLOR_MATRIX_UNKNOWN && h >= 720);
    self->colorimetry = (bt709 ? PS5WK_VCOLOR_BT709 : 0) | (c->range == GST_VIDEO_COLOR_RANGE_0_255 ? PS5WK_VCOLOR_FULL_RANGE : 0);
    g_printerr("[ps5videosink] %p: %ux%u %s%s, %s %s range\n", (void *)self, w, h, gst_video_format_to_string(GST_VIDEO_INFO_FORMAT(&info)),
        remote ? " decoded in the title" : "", bt709 ? "BT.709" : "BT.601", c->range == GST_VIDEO_COLOR_RANGE_0_255 ? "full" : "limited");
    g_mutex_unlock(&self->lock);
    return TRUE;
}

/* CLOCK_MONOTONIC time (us) at which the buffer is due, 0 if unknown. */
static gint64 target_time_us(GstPs5VideoSink *self, GstBuffer *buffer)
{
    GstBaseSink *bsink = GST_BASE_SINK(self);
    GstClockTime pts = GST_BUFFER_PTS(buffer);
    if (!GST_CLOCK_TIME_IS_VALID(pts) || GST_STATE(self) != GST_STATE_PLAYING)
        return 0;
    GstClock *clock = gst_element_get_clock(GST_ELEMENT(self));
    if (!clock)
        return 0;
    GST_OBJECT_LOCK(bsink);
    guint64 running = gst_segment_to_running_time(&bsink->segment, GST_FORMAT_TIME, pts);
    GST_OBJECT_UNLOCK(bsink);
    if (!GST_CLOCK_TIME_IS_VALID(running)) {
        gst_object_unref(clock);
        return 0;
    }
    GstClockTime due = running + gst_element_get_base_time(GST_ELEMENT(self)) + gst_base_sink_get_latency(bsink);
    GstClockTime now_clock = gst_clock_get_time(clock);
    gint64 now_mono = g_get_monotonic_time();
    gst_object_unref(clock);
    /* The audio clock moves in grains (5.3 ms); smooth the mapping so frame
     * times keep their spacing, and follow any jump of more than 20 ms. */
    gint64 sample = now_mono * 1000 - (gint64)now_clock;
    if (!self->have_offset || ABS(sample - self->clock_offset) > 20 * GST_MSECOND) {
        self->clock_offset = sample;
        self->have_offset = TRUE;
    } else
        self->clock_offset += (sample - self->clock_offset) / 16;
    return ((gint64)due + self->clock_offset) / 1000;
}

static void copy_planes(GstPs5VideoSink *self, GstVideoFrame *frame, guint8 *dst)
{
    guint planes = GST_VIDEO_FRAME_N_PLANES(frame);
    for (guint p = 0; p < planes && p < 3; p++) {
        const guint8 *src = GST_VIDEO_FRAME_PLANE_DATA(frame, p);
        guint src_stride = GST_VIDEO_FRAME_PLANE_STRIDE(frame, p);
        guint rows = GST_VIDEO_FRAME_COMP_HEIGHT(frame, p);
        guint row_bytes = GST_VIDEO_FRAME_COMP_WIDTH(frame, p) * GST_VIDEO_FRAME_COMP_PSTRIDE(frame, p);
        guint8 *d = dst + self->offset[p];
        if (src_stride == self->stride[p])
            memcpy(d, src, (gsize)src_stride * rows);
        else
            for (guint r = 0; r < rows; r++)
                memcpy(d + (gsize)r * self->stride[p], src + (gsize)r * src_stride, row_bytes);
    }
}

static void maybe_log(GstPs5VideoSink *self)
{
    gint64 now = g_get_monotonic_time();
    if (!self->log_window)
        self->log_window = now;
    if (now - self->log_window < 5 * G_USEC_PER_SEC)
        return;
    double s = (now - self->log_window) / 1e6;
    guint64 sent = self->sent - self->log_sent, no_slot = self->no_slot - self->log_no_slot;
    if (sent || no_slot)
        g_printerr("[ps5videosink] %p: %.1f frames/s sent, %.1f ms before due on average (%.1f min), %" G_GUINT64_FORMAT
                   " without a free slot; title so far: %u shown, %u dropped\n",
            (void *)self, sent / s, self->lead_count ? self->lead_sum_us / 1000.0 / self->lead_count : 0.0,
            self->lead_count ? self->lead_min_us / 1000.0 : 0.0, no_slot, self->title_displayed, self->title_dropped);
    self->lead_sum_us = self->lead_min_us = 0;
    self->lead_count = 0;
    self->log_window = now;
    self->log_sent = self->sent;
    self->log_no_slot = self->no_slot;
}

static void emit_sample(GstPs5VideoSink *self, GstBuffer *buffer)
{
    GstBaseSink *bsink = GST_BASE_SINK(self);
    GstCaps *caps = gst_pad_get_current_caps(GST_BASE_SINK_PAD(bsink));
    GST_OBJECT_LOCK(bsink);
    GstSegment segment = bsink->segment;
    GST_OBJECT_UNLOCK(bsink);
    GstSample *sample = gst_sample_new(buffer, caps, &segment, NULL);
    g_signal_emit(self, signals[SIGNAL_REPAINT_REQUESTED], 0, sample);
    gst_sample_unref(sample);
    if (caps)
        gst_caps_unref(caps);
}

static GstFlowReturn show(GstPs5VideoSink *self, GstBuffer *buffer, gboolean preroll)
{
    gint64 target = preroll ? 0 : target_time_us(self, buffer);
    g_mutex_lock(&self->lock);
    if (!self->have_info) {
        g_mutex_unlock(&self->lock);
        return GST_FLOW_NOT_NEGOTIATED;
    }
    connect_locked(self);
    if (self->remote) {
        read_releases_locked(self, 0);
        struct ps5wk_remote_picture remote;
        if (self->fd >= 0 && gst_buffer_extract(buffer, 0, &remote, sizeof remote) == sizeof remote && remote.magic == PS5WK_REMOTE_MAGIC) {
            struct ps5wk_video_msg msg;
            memset(&msg, 0, sizeof msg);
            msg.type = PS5WK_VMSG_FRAME;
            msg.format = PS5WK_VFMT_REMOTE;
            msg.ps5wk_decoder = remote.decoder;
            msg.ps5wk_token = remote.token;
            msg.generation = remote.epoch;
            msg.width = GST_VIDEO_INFO_WIDTH(&self->info);
            msg.height = GST_VIDEO_INFO_HEIGHT(&self->info);
            msg.colorimetry = self->colorimetry;
            msg.target_us = target;
            msg.pts_ns = GST_BUFFER_PTS_IS_VALID(buffer) ? (int64_t)GST_BUFFER_PTS(buffer) : -1;
            if (target) {
                gint64 lead = target - g_get_monotonic_time();
                self->lead_sum_us += lead;
                if (!self->lead_count || lead < self->lead_min_us)
                    self->lead_min_us = lead;
                self->lead_count++;
            }
            if (self->prepared_pending && self->prepared.token == remote.token)
                self->prepared_pending = FALSE;
            if (!send_msg(self, &msg, -1)) {
                self->sent++;
                self->remote_sent++;
            }
        }
        maybe_log(self);
    } else if (self->fd >= 0 && ensure_shm_locked(self, self->frame_bytes)) {
        read_releases_locked(self, 0);
        int slot = free_slot_locked(self);
        if (slot < 0 && !self->flushing) {
            read_releases_locked(self, PS5_SLOT_WAIT_MS);
            slot = free_slot_locked(self);
        }
        if (slot < 0)
            self->no_slot++;
        else if (self->fd >= 0) {
            GstVideoFrame frame;
            if (gst_video_frame_map(&frame, &self->info, buffer, GST_MAP_READ)) {
                copy_planes(self, &frame, self->shm + (gsize)slot * self->slot_bytes);
                gst_video_frame_unmap(&frame);
                struct ps5wk_video_msg msg;
                memset(&msg, 0, sizeof msg);
                msg.type = PS5WK_VMSG_FRAME;
                msg.slot = (uint32_t)slot;
                msg.generation = self->generation;
                msg.format = self->format;
                msg.width = GST_VIDEO_INFO_WIDTH(&self->info);
                msg.height = GST_VIDEO_INFO_HEIGHT(&self->info);
                msg.colorimetry = self->colorimetry;
                memcpy(msg.stride, self->stride, sizeof msg.stride);
                memcpy(msg.offset, self->offset, sizeof msg.offset);
                msg.target_us = target;
                msg.pts_ns = GST_BUFFER_PTS_IS_VALID(buffer) ? (int64_t)GST_BUFFER_PTS(buffer) : -1;
                if (target) {
                    gint64 lead = target - g_get_monotonic_time();
                    self->lead_sum_us += lead;
                    if (!self->lead_count || lead < self->lead_min_us)
                        self->lead_min_us = lead;
                    self->lead_count++;
                }
                if (!send_msg(self, &msg, -1)) {
                    self->busy[slot] = TRUE;
                    self->next_slot = (guint)(slot + 1) % PS5_SLOTS;
                    self->sent++;
                }
            }
        }
        maybe_log(self);
    }
    g_mutex_unlock(&self->lock);
    emit_sample(self, buffer);
    return GST_FLOW_OK;
}

static GstFlowReturn ps5_render(GstBaseSink *bsink, GstBuffer *buffer)
{
    return show(GST_PS5_VIDEO_SINK(bsink), buffer, FALSE);
}

static GstFlowReturn ps5_preroll(GstBaseSink *bsink, GstBuffer *buffer)
{
    return show(GST_PS5_VIDEO_SINK(bsink), buffer, TRUE);
}

/* A placeholder that got as far as prepare() but was never rendered goes
 * back to the title's decoder. (GstBaseSink drops late buffers *before*
 * prepare(); that case is handled by not dropping hardware-decoded frames
 * at all: max-lateness -1 in set_caps.) */
static GstFlowReturn ps5_prepare(GstBaseSink *bsink, GstBuffer *buffer)
{
    GstPs5VideoSink *self = GST_PS5_VIDEO_SINK(bsink);
    if (!self->remote)
        return GST_FLOW_OK;
    struct ps5wk_remote_picture remote;
    if (gst_buffer_extract(buffer, 0, &remote, sizeof remote) != sizeof remote || remote.magic != PS5WK_REMOTE_MAGIC)
        return GST_FLOW_OK;
    g_mutex_lock(&self->lock);
    if (self->prepared_pending && self->prepared.token != remote.token) {
        struct ps5wk_video_msg msg;
        memset(&msg, 0, sizeof msg);
        msg.type = PS5WK_VMSG_RDROP;
        msg.ps5wk_decoder = self->prepared.decoder;
        msg.ps5wk_token = self->prepared.token;
        send_msg(self, &msg, -1);
        self->remote_dropped++;
    }
    self->prepared = remote;
    self->prepared_pending = TRUE;
    g_mutex_unlock(&self->lock);
    return GST_FLOW_OK;
}

static gboolean ps5_unlock(GstBaseSink *bsink)
{
    GstPs5VideoSink *self = GST_PS5_VIDEO_SINK(bsink);
    self->flushing = TRUE; /* read_releases_locked checks it every 10 ms */
    return TRUE;
}

static gboolean ps5_unlock_stop(GstBaseSink *bsink)
{
    GST_PS5_VIDEO_SINK(bsink)->flushing = FALSE;
    return TRUE;
}

static gboolean ps5_event(GstBaseSink *bsink, GstEvent *event)
{
    GstPs5VideoSink *self = GST_PS5_VIDEO_SINK(bsink);
    if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP) {
        g_mutex_lock(&self->lock);
        self->prepared_pending = FALSE; /* the title's flush took them back */
        struct ps5wk_video_msg msg;
        memset(&msg, 0, sizeof msg);
        msg.type = PS5WK_VMSG_FLUSH;
        send_msg(self, &msg, -1);
        self->have_offset = FALSE;
        g_mutex_unlock(&self->lock);
    }
    return GST_BASE_SINK_CLASS(gst_ps5_video_sink_parent_class)->event(bsink, event);
}

/* Sync PS5_LEAD early (render() stamps the real due time): playsink owns
 * ts-offset (its av-offset), so the start time is moved here instead. The
 * end stays, so a buffer at the segment start is clipped, not dropped. */
static void ps5_get_times(GstBaseSink *bsink, GstBuffer *buffer, GstClockTime *start, GstClockTime *end)
{
    GstClockTime pts = GST_BUFFER_PTS(buffer);
    *start = *end = GST_CLOCK_TIME_NONE;
    if (!GST_CLOCK_TIME_IS_VALID(pts))
        return;
    *start = pts > PS5_LEAD ? pts - PS5_LEAD : 0;
    if (GST_BUFFER_DURATION_IS_VALID(buffer))
        *end = pts + GST_BUFFER_DURATION(buffer);
}

static gboolean ps5_propose_allocation(GstBaseSink *bsink, GstQuery *query)
{
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
    return TRUE;
}

/* ------------------------------------------------------------ GObject */

static void ps5_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
    GstPs5VideoSink *self = GST_PS5_VIDEO_SINK(object);
    if (id != PROP_STATS) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
        return;
    }
    guint64 rendered = 0, dropped = 0;
    GstStructure *base = gst_base_sink_get_stats(GST_BASE_SINK(self));
    if (base) {
        gst_structure_get_uint64(base, "dropped", &dropped);
        gst_structure_free(base);
    }
    g_mutex_lock(&self->lock);
    rendered = self->title_displayed;
    dropped += self->title_dropped + self->no_slot;
    g_mutex_unlock(&self->lock);
    g_value_take_boxed(value, gst_structure_new("application/x-gst-base-sink-stats", "average-rate", G_TYPE_DOUBLE, 0.0,
        "dropped", G_TYPE_UINT64, dropped, "rendered", G_TYPE_UINT64, rendered, NULL));
}

static void ps5_finalize(GObject *object)
{
    GstPs5VideoSink *self = GST_PS5_VIDEO_SINK(object);
    disconnect_locked(self);
    g_mutex_clear(&self->lock);
    G_OBJECT_CLASS(gst_ps5_video_sink_parent_class)->finalize(object);
}

static void gst_ps5_video_sink_init(GstPs5VideoSink *self)
{
    g_mutex_init(&self->lock);
    self->fd = -1;
    self->shm_fd = -1;
    gst_base_sink_set_qos_enabled(GST_BASE_SINK(self), TRUE);
    self->default_max_lateness = gst_base_sink_get_max_lateness(GST_BASE_SINK(self));
}

static void gst_ps5_video_sink_class_init(GstPs5VideoSinkClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstBaseSinkClass *bsink_class = GST_BASE_SINK_CLASS(klass);
    object_class->get_property = ps5_get_property;
    object_class->finalize = ps5_finalize;
    g_object_class_override_property(object_class, PROP_STATS, "stats");
    signals[SIGNAL_REPAINT_REQUESTED] = g_signal_new("repaint-requested", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
        NULL, G_TYPE_NONE, 1, GST_TYPE_SAMPLE | G_SIGNAL_TYPE_STATIC_SCOPE);
    gst_element_class_set_static_metadata(element_class, "PS5 hole-punch video sink", "Sink/Video",
        "Hands decoded frames to the browser title, which draws them under the page", "ps5webkit");
    gst_element_class_add_static_pad_template(element_class, &sink_template);
    bsink_class->start = ps5_start;
    bsink_class->stop = ps5_stop;
    bsink_class->set_caps = ps5_set_caps;
    bsink_class->render = ps5_render;
    bsink_class->preroll = ps5_preroll;
    bsink_class->unlock = ps5_unlock;
    bsink_class->unlock_stop = ps5_unlock_stop;
    bsink_class->event = ps5_event;
    bsink_class->propose_allocation = ps5_propose_allocation;
    bsink_class->get_times = ps5_get_times;
    bsink_class->prepare = ps5_prepare;
}

static gboolean plugin_init(GstPlugin *plugin)
{
    GST_DEBUG_CATEGORY_INIT(ps5_video_sink_debug, "ps5videosink", 0, "PS5 hole-punch video sink");
    /* Rank NONE: only WebKit makes it, by name. The hardware decoder proxy
     * (gstps5vdecproxy.c) is part of the same plugin. */
    extern gboolean ps5_vdec_proxy_register(GstPlugin *plugin);
    return gst_element_register(plugin, "ps5videosink", GST_RANK_NONE, GST_TYPE_PS5_VIDEO_SINK)
        && ps5_vdec_proxy_register(plugin);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, ps5video, "PS5 hole-punch video sink and hardware decoder proxy", plugin_init, "1.0", "LGPL",
    "ps5webkit", "https://github.com/")
