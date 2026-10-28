/* GStreamer smoke test for PS5 payloads (Milestone 2 sysroot).
 *
 * Links libgstreamer-full-1.0.a (static core, libraries and plugins,
 * registered by gst_init() through gst_init_static_plugins()) and checks:
 *   gst-init     gst_init() works, static plugins registered
 *   factories    the elements WebKit's media player and MSE need exist
 *   demux-mp4    appsrc (MP4 from memory) ! qtdemux ! fakesink, per stream
 *   demux-webm   the same with WebM (VP9 + Opus) and matroskademux
 *   decode-*     each stream through a queue, its parser and avdec_* (gst-libav):
 *                H.264, AAC, VP9, Opus, AV1 (avdec_libdav1d)
 *
 * The clips are generated on the host by build.sh (2 s each) and linked in
 * as byte arrays: smoke.mp4 (H.264 + AAC), smoke.webm (VP9 + Opus),
 * smoke-av1.mp4 (AV1).
 * Every check prints "RESULT <name> PASS|FAIL <detail>".
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

extern const unsigned char smoke_mp4[], smoke_webm[], smoke_av1_mp4[];
extern const unsigned int smoke_mp4_len, smoke_webm_len, smoke_av1_mp4_len;

typedef struct {
    const char *name;          /* RESULT name suffix */
    const unsigned char *data;
    unsigned int len;
    const char *caps;          /* appsrc caps */
    const char *demuxer;
    gboolean want_audio;
} Clip;

static int n_pass, n_fail;

static void result(const char *name, gboolean ok, const char *fmt, ...)
{
    char detail[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    printf("RESULT %s %s %s\n", name, ok ? "PASS" : "FAIL", detail);
    fflush(stdout);
    if (ok)
        n_pass++;
    else
        n_fail++;
}

/* ------------------------------------------------------------ factories */
static void test_factories(void)
{
    static const char *need[] = {
        /* WebKit playback / MSE plumbing */
        "appsrc", "appsink", "typefind", "queue", "queue2", "multiqueue", "tee", "identity",
        "capsfilter", "fakesink", "decodebin3", "parsebin", "uridecodebin3", "urisourcebin",
        "playbin3", "audioconvert", "audioresample", "videoconvertscale", "volume",
        /* containers and adaptive streaming */
        "qtdemux", "matroskademux", "tsdemux", "hlsdemux2", "dashdemux2", "id3demux",
        /* parsers */
        "h264parse", "h265parse", "vp9parse", "av1parse", "aacparse", "mpegaudioparse",
        /* software decoders (gst-libav) */
        "avdec_h264", "avdec_h265", "avdec_vp9", "avdec_vp8", "avdec_libdav1d", "avdec_aac",
        "avdec_opus", "avdec_mp3", "avdec_flac",
    };
    GString *missing = g_string_new(NULL);
    guint n = G_N_ELEMENTS(need), found = 0;
    for (guint i = 0; i < n; i++) {
        GstElementFactory *f = gst_element_factory_find(need[i]);
        if (f) {
            found++;
            gst_object_unref(f);
        } else {
            g_string_append_printf(missing, " %s", need[i]);
        }
    }
    result("factories", found == n, "%u/%u element factories present%s%s", found, n,
           missing->len ? ", missing:" : "", missing->str);
    g_string_free(missing, TRUE);
}

/* ----------------------------------------------------------- pipelines */
typedef struct {
    GstElement *pipeline;
    const Clip *clip;
    gboolean decode;
    int threads; /* avdec max-threads, 0 = auto */
    const char *thread_type;
    guint video_buffers, audio_buffers;
    char video_caps[160], audio_caps[160];
    char video_dec[32], audio_dec[32];
    char error[256];
} Demux;

static GstPadProbeReturn count_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    guint *counter = data;
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)
        g_atomic_int_inc(counter);
    return GST_PAD_PROBE_OK;
}

static void on_pad_added(GstElement *demux, GstPad *pad, gpointer data)
{
    Demux *d = data;
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps)
        caps = gst_pad_query_caps(pad, NULL);
    const GstStructure *s = gst_caps_get_structure(caps, 0);
    const char *media = gst_structure_get_name(s);
    gboolean is_video = g_str_has_prefix(media, "video/");
    gchar *desc = gst_caps_to_string(caps);
    g_strlcpy(is_video ? d->video_caps : d->audio_caps, desc, 160);
    g_free(desc);

    GstElement *sink = gst_element_factory_make("fakesink", NULL);
    /* async=FALSE: with one streaming thread and no queues (demux-only
     * runs), a prerolling sink would block the demuxer before the other
     * stream's sink gets data. */
    g_object_set(sink, "sync", FALSE, "async", FALSE, NULL);
    GstElement *first = sink;
    gst_bin_add(GST_BIN(d->pipeline), sink);
    if (d->decode) {
        const char *parse = "identity", *dec = NULL;
        if (!strcmp(media, "video/x-h264")) { parse = "h264parse"; dec = "avdec_h264"; }
        else if (!strcmp(media, "video/x-vp9")) { parse = "vp9parse"; dec = "avdec_vp9"; }
        else if (!strcmp(media, "video/x-av1")) { parse = g_getenv("SMOKE_AV1_PARSE") ? g_getenv("SMOKE_AV1_PARSE") : "av1parse"; dec = "avdec_libdav1d"; }
        else if (!strcmp(media, "audio/mpeg")) { parse = "aacparse"; dec = "avdec_aac"; }
        else if (!strcmp(media, "audio/x-opus")) { dec = "avdec_opus"; }
        else { dec = "identity"; }
        g_strlcpy(is_video ? d->video_dec : d->audio_dec, dec, 32);
        /* A queue per stream, as WebKit's pipelines have (multiqueue/queue
         * after the demuxer): one streaming thread per branch. */
        GstElement *qu = gst_element_factory_make("queue", NULL);
        GstElement *p = gst_element_factory_make(parse, NULL);
        GstElement *q = gst_element_factory_make(dec, NULL);
        if (is_video && d->threads)
            g_object_set(q, "max-threads", d->threads, NULL);
        if (is_video && d->thread_type)
            gst_util_set_object_arg(G_OBJECT(q), "thread-type", d->thread_type);
        gst_bin_add_many(GST_BIN(d->pipeline), qu, p, q, NULL);
        gst_element_link_many(qu, p, q, sink, NULL);
        gst_element_sync_state_with_parent(qu);
        gst_element_sync_state_with_parent(p);
        gst_element_sync_state_with_parent(q);
        first = qu;
    }
    gst_element_sync_state_with_parent(sink);
    GstPad *sinkpad = gst_element_get_static_pad(sink, "sink");
    gst_pad_add_probe(sinkpad, GST_PAD_PROBE_TYPE_BUFFER, count_probe,
                      is_video ? &d->video_buffers : &d->audio_buffers, NULL);
    gst_object_unref(sinkpad);
    GstPad *in = gst_element_get_static_pad(first, "sink");
    GstPadLinkReturn lr = gst_pad_link(pad, in);
    if (lr != GST_PAD_LINK_OK)
        g_snprintf(d->error, sizeof d->error, "link %s pad failed: %d", media, lr);
    gst_object_unref(in);
    gst_caps_unref(caps);
}

static void run_demux(const Clip *clip, gboolean decode, int threads, const char *thread_type)
{
    Demux d = { 0 };
    d.clip = clip;
    d.decode = decode;
    d.threads = threads;
    d.thread_type = thread_type;
    d.pipeline = gst_pipeline_new("smoke");
    GstElement *src = gst_element_factory_make("appsrc", "src");
    GstElement *demux = gst_element_factory_make(clip->demuxer, "demux");
    if (!src || !demux) {
        result(decode ? "decode" : "demux", FALSE, "appsrc=%p %s=%p", (void *)src, clip->demuxer, (void *)demux);
        return;
    }
    GstCaps *caps = gst_caps_from_string(clip->caps);
    g_object_set(src, "caps", caps, "format", GST_FORMAT_BYTES, "stream-type", 0 /* stream */, NULL);
    gst_caps_unref(caps);
    gst_bin_add_many(GST_BIN(d.pipeline), src, demux, NULL);
    gst_element_link(src, demux);
    g_signal_connect(demux, "pad-added", G_CALLBACK(on_pad_added), &d);

    /* The whole file as one buffer, then EOS: as WebKit's MSE feeds data. */
    GstBuffer *buf = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY, (gpointer)clip->data, clip->len, 0,
                                                 clip->len, NULL, NULL);
    gst_element_set_state(d.pipeline, GST_STATE_PLAYING);
    gint64 t0 = g_get_monotonic_time();
    gst_app_src_push_buffer(GST_APP_SRC(src), buf);
    gst_app_src_end_of_stream(GST_APP_SRC(src));

    GstBus *bus = gst_element_get_bus(d.pipeline);
    gboolean eos = FALSE;
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 20 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    double ms = (g_get_monotonic_time() - t0) / 1000.0;
    if (msg) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
            eos = TRUE;
        } else {
            GError *err = NULL;
            gchar *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            g_snprintf(d.error, sizeof d.error, "%s: %s (%s)", GST_OBJECT_NAME(msg->src), err->message, dbg ? dbg : "");
            g_clear_error(&err);
            g_free(dbg);
        }
        gst_message_unref(msg);
    } else if (!d.error[0]) {
        g_strlcpy(d.error, "timeout (20 s) waiting for EOS", sizeof d.error);
    }
    gst_object_unref(bus);
    gst_element_set_state(d.pipeline, GST_STATE_NULL);

    char name[48];
    const char *err_sep = d.error[0] ? ", error: " : "";
    if (!decode) {
        g_snprintf(name, sizeof name, "demux-%s", clip->name);
        result(name, eos && d.video_buffers > 0 && (!clip->want_audio || d.audio_buffers > 0),
               "%u-byte clip from memory via %s, %.0f ms: video %u samples [%.70s], audio %u samples [%.70s]%s%s",
               clip->len, clip->demuxer, ms, d.video_buffers, d.video_caps, d.audio_buffers, d.audio_caps, err_sep,
               d.error);
    } else {
        g_snprintf(name, sizeof name, "decode-%s-video", clip->name);
        result(name, eos && d.video_buffers > 0, "%s (max-threads=%d): %u frames decoded, %.0f ms%s%s", d.video_dec,
               d.threads, d.video_buffers, ms, err_sep, d.error);
        if (clip->want_audio) {
            g_snprintf(name, sizeof name, "decode-%s-audio", clip->name);
            result(name, eos && d.audio_buffers > 0, "%s: %u buffers decoded%s%s", d.audio_dec, d.audio_buffers,
                   err_sep, d.error);
        }
    }
    gst_object_unref(d.pipeline);
}

static gpointer watchdog(gpointer data)
{
    g_usleep(120 * G_USEC_PER_SEC);
    printf("RESULT watchdog FAIL still running after 120 s\n");
    fflush(stdout);
    _exit(2);
    return data;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_thread_unref(g_thread_new("watchdog", watchdog, NULL));
    GError *err = NULL;
    gint64 t0 = g_get_monotonic_time();
    gboolean ok = gst_init_check(&argc, &argv, &err);
    double ms = (g_get_monotonic_time() - t0) / 1000.0;
    GList *plugins = ok ? gst_registry_get_plugin_list(gst_registry_get()) : NULL;
    guint nplugins = g_list_length(plugins);
    gst_plugin_list_free(plugins);
    gchar *ver = gst_version_string();
    result("gst-init", ok && nplugins > 0, "%s, %.0f ms, %u static plugins registered%s%s", ver, ms, nplugins,
           err ? ", error: " : "", err ? err->message : "");
    g_free(ver);
    if (!ok)
        return 1;

    test_factories();
    static const Clip clips[] = {
        { "mp4", smoke_mp4, 0, "video/quicktime", "qtdemux", TRUE },
        { "webm", smoke_webm, 0, "video/webm", "matroskademux", TRUE },
        { "av1", smoke_av1_mp4, 0, "video/quicktime", "qtdemux", FALSE },
    };
    Clip c[3];
    memcpy(c, clips, sizeof c);
    c[0].len = smoke_mp4_len;
    c[1].len = smoke_webm_len;
    c[2].len = smoke_av1_mp4_len;
    run_demux(&c[0], FALSE, 0, NULL);
    run_demux(&c[1], FALSE, 0, NULL);
    for (int i = 0; i < 3; i++)
        run_demux(&c[i], TRUE, 0, NULL);

    printf("RESULT summary %s %d passed, %d failed\n", n_fail ? "FAIL" : "PASS", n_pass, n_fail);
    fflush(stdout);
    return n_fail ? 1 : 0;
}
