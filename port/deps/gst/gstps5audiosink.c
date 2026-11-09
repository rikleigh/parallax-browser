/* ps5audiosink: GStreamer audio sink for the PS5 port.
 *
 * Registered as the static plugin "ps5audio" (gststaticplugins.c) with rank
 * PRIMARY and klass Sink/Audio, so autoaudiosink (what WebKit's
 * createAutoAudioSink makes) picks it.
 *
 * Two outputs:
 *
 * - **Server** (when PS5WK_AUDIO_SOCKET names a unix socket; the WebKit
 *   WebProcess payloads set it): PCM goes to the browser title, which plays
 *   it with sceAudioOut (port/webkit/apps/common/ps5audioserver.c). The
 *   console's audio daemon never released what payload processes took:
 *   after eight payload processes had used sceAudioOut, sceAudioOutInit
 *   failed (0x80260002) and then blocked in every new process until a
 *   restart. The title is one long-lived application the system cleans up.
 *
 * - **Direct** sceAudioOut otherwise (port/deps/smoke, standalone payloads):
 *   the main port of the system user (255), S16, 48 kHz, 256 frames per
 *   sceAudioOutOutput, which blocks until the port takes the buffer. From a
 *   payload this costs one of the audio daemon's slots per process (above).
 *
 * GstAudioSink runs write() on its own ring-buffer thread, so blocking there
 * (in sceAudioOutOutput, or until the title has played what was sent) is
 * what paces the pipeline clock. In server mode the socket alone would not:
 * it takes up to 2048 mono frames at once, so at the start of a stream the
 * ring-buffer thread sent everything written and then caught up with the
 * streaming thread, played not-yet-written segments as silence and dropped
 * their samples when they came (a 21 ms dropout 43 ms into a mono clip). A
 * write waits until at most PS5_SERVER_AHEAD frames are unplayed, as a
 * device would. Caps are fixed to what the port takes; playsink's
 * audioconvert/audioresample convert upstream.
 */
#define PACKAGE "ps5webkit"
#include <gst/gst.h>
#include <gst/audio/gstaudiosink.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define PS5_GRAIN 256 /* frames per sceAudioOutOutput */
/* Server mode: the most frames sent but not yet played by the title (see
 * server_wait_for_room). */
#define PS5_SERVER_AHEAD (4 * PS5_GRAIN)
#define PS5_SERVER_WAIT_MAX_US 250000
#define PS5_RATE 48000
#define PS5_USER_SYSTEM 255
#define PS5_PORT_MAIN 0
#define PS5_PARAM_S16_STEREO 1
#define PS5_PARAM_S16_MONO 0
#define PS5_ERR_ALREADY_INIT ((int)0x8026000e)

/* Wire format of port/webkit/apps/common/ps5audioserver.h (keep in sync). */
#define PS5WK_AUDIO_MAGIC 0x41355350u
#define PS5WK_AUDIO_VERSION 1
struct ps5wk_audio_hello {
    uint32_t magic, version, rate, channels;
    int32_t pid;
    uint32_t reserved;
};

int sceAudioOutInit(void);
int sceAudioOutOpen(int userId, int type, int index, unsigned len, unsigned freq, unsigned param);
int sceAudioOutOutput(int handle, const void *ptr);
int sceAudioOutClose(int handle);
int sceAudioOutSetVolume(int handle, int flag, int *vol);

GST_DEBUG_CATEGORY_STATIC(ps5_audio_sink_debug);
#define GST_CAT_DEFAULT ps5_audio_sink_debug

#define GST_TYPE_PS5_AUDIO_SINK (gst_ps5_audio_sink_get_type())
G_DECLARE_FINAL_TYPE(GstPs5AudioSink, gst_ps5_audio_sink, GST, PS5_AUDIO_SINK, GstAudioSink)

struct _GstPs5AudioSink {
    GstAudioSink parent;
    const char *socket_path; /* server mode when set */
    int handle;              /* direct mode: sceAudioOut port */
    int fd;                  /* server mode: connection, -1 = (re)connect on write */
    guint channels;
    guint bpf;          /* bytes per frame */
    guint8 *grain;      /* direct mode: PS5_GRAIN frames being filled */
    guint filled;       /* bytes in grain */
    guint64 written;    /* frames sent on the current connection / port */
    guint64 played;     /* server mode: frames the title reported played */
    guint64 grains, late;
    gint64 started_us;
    guint64 waits_timed_out; /* server mode: the title stopped reporting */
};

G_DEFINE_TYPE(GstPs5AudioSink, gst_ps5_audio_sink, GST_TYPE_AUDIO_SINK)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("audio/x-raw, format = (string) " GST_AUDIO_NE(S16) ", layout = (string) interleaved, "
                    "rate = (int) 48000, channels = (int) [ 1, 2 ]"));

static gboolean ps5_open(GstAudioSink *sink)
{
    GstPs5AudioSink *self = GST_PS5_AUDIO_SINK(sink);
    self->socket_path = g_getenv("PS5WK_AUDIO_SOCKET");
    if (self->socket_path && *self->socket_path)
        return TRUE;
    self->socket_path = NULL;
    int rc = sceAudioOutInit();
    if (rc != 0 && rc != PS5_ERR_ALREADY_INIT) {
        GST_ELEMENT_ERROR(sink, RESOURCE, OPEN_WRITE, (NULL), ("sceAudioOutInit: 0x%08x", (unsigned)rc));
        return FALSE;
    }
    return TRUE;
}

static void server_disconnect(GstPs5AudioSink *self)
{
    if (self->fd >= 0)
        close(self->fd);
    self->fd = -1;
    self->written = self->played = 0;
}

static gboolean server_connect(GstPs5AudioSink *self)
{
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    g_strlcpy(address.sun_path, self->socket_path, sizeof address.sun_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof address)) {
        GST_WARNING_OBJECT(self, "connect %s: errno %d", self->socket_path, errno);
        if (fd >= 0)
            close(fd);
        return FALSE;
    }
    int size = 4 * PS5_GRAIN * 4;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof size);
    struct ps5wk_audio_hello hello = { PS5WK_AUDIO_MAGIC, PS5WK_AUDIO_VERSION, PS5_RATE, self->channels, (int32_t)getpid(), 0 };
    if (send(fd, &hello, sizeof hello, 0) != (ssize_t)sizeof hello) {
        close(fd);
        return FALSE;
    }
    self->fd = fd;
    self->written = self->played = 0;
    return TRUE;
}

static gboolean ps5_prepare(GstAudioSink *asink, GstAudioRingBufferSpec *spec)
{
    GstPs5AudioSink *self = GST_PS5_AUDIO_SINK(asink);
    self->channels = GST_AUDIO_INFO_CHANNELS(&spec->info);
    self->bpf = GST_AUDIO_INFO_BPF(&spec->info);
    self->filled = 0;
    self->grains = self->late = 0;
    self->started_us = 0;
    self->written = self->played = 0;
    spec->segsize = PS5_GRAIN * self->bpf;
    spec->segtotal = 8;
    if (self->socket_path) {
        if (!server_connect(self)) {
            GST_ELEMENT_ERROR(asink, RESOURCE, OPEN_WRITE, (NULL), ("cannot reach the audio server at %s", self->socket_path));
            return FALSE;
        }
        g_printerr("[ps5audiosink] %u ch to the title's audio server (%s)\n", self->channels, self->socket_path);
        return TRUE;
    }
    unsigned param = self->channels == 1 ? PS5_PARAM_S16_MONO : PS5_PARAM_S16_STEREO;
    self->handle = sceAudioOutOpen(PS5_USER_SYSTEM, PS5_PORT_MAIN, 0, PS5_GRAIN, PS5_RATE, param);
    if (self->handle < 0) {
        GST_ELEMENT_ERROR(asink, RESOURCE, OPEN_WRITE, (NULL), ("sceAudioOutOpen: 0x%08x", (unsigned)self->handle));
        return FALSE;
    }
    int vol[8] = { 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768 };
    sceAudioOutSetVolume(self->handle, (1 << self->channels) - 1, vol);
    self->grain = g_malloc0(PS5_GRAIN * self->bpf);
    g_printerr("[ps5audiosink] open: handle 0x%x, %u ch, %u Hz, grain %u frames (direct sceAudioOut)\n", self->handle,
        self->channels, GST_AUDIO_INFO_RATE(&spec->info), PS5_GRAIN);
    return TRUE;
}

static gboolean ps5_unprepare(GstAudioSink *asink)
{
    GstPs5AudioSink *self = GST_PS5_AUDIO_SINK(asink);
    if (self->socket_path) {
        server_disconnect(self);
        return TRUE;
    }
    if (self->handle >= 0) {
        gint64 elapsed = self->started_us ? g_get_monotonic_time() - self->started_us : 0;
        g_printerr("[ps5audiosink] close: %" G_GUINT64_FORMAT " grains (%.1f s of audio) in %.1f s, %" G_GUINT64_FORMAT
                   " slow\n", self->grains, self->grains * (double)PS5_GRAIN / PS5_RATE, elapsed / 1e6, self->late);
        sceAudioOutClose(self->handle);
        self->handle = -1;
    }
    g_clear_pointer(&self->grain, g_free);
    return TRUE;
}

static gboolean ps5_close(GstAudioSink *sink)
{
    return TRUE;
}

static void ps5_output_grain(GstPs5AudioSink *self)
{
    gint64 t0 = g_get_monotonic_time();
    if (!self->started_us)
        self->started_us = t0;
    int rc = sceAudioOutOutput(self->handle, self->grain);
    if (rc < 0)
        GST_WARNING_OBJECT(self, "sceAudioOutOutput: 0x%08x", (unsigned)rc);
    if (g_get_monotonic_time() - t0 > 2 * 1000000 * PS5_GRAIN / PS5_RATE)
        self->late++;
    self->grains++;
    self->written += PS5_GRAIN;
    self->filled = 0;
}

/* Latest "frames played" count from the server (it sends one per grain).
 * fd: the connection the caller saw; reset() may close it from another
 * thread, so nothing is read once it is no longer the current one. */
static void server_read_played_on(GstPs5AudioSink *self, int fd)
{
    guint64 counts[64];
    while (fd >= 0 && fd == self->fd) {
        ssize_t n = recv(fd, counts, sizeof counts, MSG_DONTWAIT);
        if (n < (ssize_t)sizeof counts[0])
            break;
        self->played = counts[n / sizeof counts[0] - 1];
    }
}

static void server_read_played(GstPs5AudioSink *self)
{
    server_read_played_on(self, self->fd);
}

/* Block, like a device, until sending `frames` more leaves at most
 * PS5_SERVER_AHEAD frames unplayed by the title. Returns early when reset()
 * drops the connection, or after PS5_SERVER_WAIT_MAX_US when the title stops
 * reporting (then the socket's own blocking paces). */
static void server_wait_for_room(GstPs5AudioSink *self, guint frames)
{
    int fd = self->fd;
    gint64 deadline = g_get_monotonic_time() + PS5_SERVER_WAIT_MAX_US;
    while (fd >= 0 && fd == self->fd) {
        server_read_played_on(self, fd);
        if (self->written + frames <= self->played + PS5_SERVER_AHEAD)
            return;
        if (g_get_monotonic_time() > deadline) {
            if (!self->waits_timed_out++)
                GST_WARNING_OBJECT(self, "the title reported nothing played for %d ms", PS5_SERVER_WAIT_MAX_US / 1000);
            return;
        }
        struct pollfd p = { fd, POLLIN, 0 };
        poll(&p, 1, 5);
    }
}

static gint ps5_write(GstAudioSink *asink, gpointer data, guint length)
{
    GstPs5AudioSink *self = GST_PS5_AUDIO_SINK(asink);
    if (self->socket_path) {
        if (self->fd < 0 && !server_connect(self)) {
            /* No server (title gone): drop at the real-time rate. */
            g_usleep((gulong)(length / self->bpf) * G_USEC_PER_SEC / PS5_RATE);
            return length;
        }
        server_wait_for_room(self, length / self->bpf);
        if (self->fd < 0)
            return length; /* reset() while waiting: dropped, as a flush does */
        const guint8 *p = data;
        guint left = length;
        while (left) {
            ssize_t n = send(self->fd, p, left, 0);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                GST_WARNING_OBJECT(self, "audio server connection lost: errno %d", errno);
                server_disconnect(self);
                break;
            }
            p += n;
            left -= (guint)n;
        }
        self->written += length / self->bpf;
        if (self->fd >= 0)
            server_read_played(self);
        return length;
    }
    const guint8 *p = data;
    guint left = length, grain_bytes = PS5_GRAIN * self->bpf;
    while (left) {
        guint n = MIN(left, grain_bytes - self->filled);
        memcpy(self->grain + self->filled, p, n);
        self->filled += n;
        p += n;
        left -= n;
        if (self->filled == grain_bytes)
            ps5_output_grain(self);
    }
    return length;
}

static guint ps5_delay(GstAudioSink *asink)
{
    GstPs5AudioSink *self = GST_PS5_AUDIO_SINK(asink);
    if (self->socket_path) {
        if (self->fd < 0)
            return 0;
        server_read_played(self);
        /* Frames sent but not yet played by the title, plus about two grains
         * queued inside its sceAudioOut port. */
        guint64 queued = self->written > self->played ? self->written - self->played : 0;
        return (guint)MIN(queued, (guint64)PS5_RATE) + 2 * PS5_GRAIN;
    }
    /* Frames not yet heard: the partial grain plus, as measured, about two
     * grains queued in the port. */
    return (self->bpf ? self->filled / self->bpf : 0) + 2 * PS5_GRAIN;
}

static void ps5_reset(GstAudioSink *asink)
{
    GstPs5AudioSink *self = GST_PS5_AUDIO_SINK(asink);
    self->filled = 0;
    /* Server mode: closing the connection makes the title drop what it has
     * queued (flush/seek); the next write reconnects. */
    if (self->socket_path)
        server_disconnect(self);
}

static void gst_ps5_audio_sink_init(GstPs5AudioSink *self)
{
    self->handle = -1;
    self->fd = -1;
}

static void gst_ps5_audio_sink_class_init(GstPs5AudioSinkClass *klass)
{
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstAudioSinkClass *sink_class = GST_AUDIO_SINK_CLASS(klass);
    gst_element_class_set_static_metadata(element_class, "PS5 audio sink", "Sink/Audio",
        "Plays audio through sceAudioOut, directly or through the browser title (48 kHz S16)", "ps5webkit");
    gst_element_class_add_static_pad_template(element_class, &sink_template);
    sink_class->open = ps5_open;
    sink_class->prepare = ps5_prepare;
    sink_class->unprepare = ps5_unprepare;
    sink_class->close = ps5_close;
    sink_class->write = ps5_write;
    sink_class->delay = ps5_delay;
    sink_class->reset = ps5_reset;
}

static gboolean plugin_init(GstPlugin *plugin)
{
    GST_DEBUG_CATEGORY_INIT(ps5_audio_sink_debug, "ps5audiosink", 0, "PS5 audio sink");
    return gst_element_register(plugin, "ps5audiosink", GST_RANK_PRIMARY, GST_TYPE_PS5_AUDIO_SINK);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, ps5audio, "PS5 audio sink", plugin_init, "1.0", "LGPL",
    "ps5webkit", "https://github.com/")
