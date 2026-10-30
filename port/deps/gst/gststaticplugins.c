/* Static GStreamer plugin registration for PS5 payloads.
 *
 * libgstreamer-1.0 in the WPE sysroot is built with gstreamer-static-full,
 * so gst_init() calls gst_init_static_plugins() (this file) instead of
 * scanning plugin directories; payloads cannot load plugins at run time.
 * build.sh (step gst_full) compiles this into libgstreamer-full-1.0.a,
 * together with every GStreamer library and the plugin archives listed here,
 * the same layout as upstream's gstreamer-full. WebKit's USE_GSTREAMER_FULL
 * links exactly that archive.
 *
 * Keep GST_PS5_PLUGINS in sync with GST_FULL_PLUGINS in build.sh.
 * Not registered on purpose: "soup" (souphttpsrc): WebKit fetches media
 * through its own webkitwebsrc and NetworkProcess; adaptivedemux2 links
 * libsoup itself for its downloads (WebKit demotes hlsdemux2/dashdemux2 for
 * that reason). "hls" is gst-plugins-bad's legacy hlsdemux: it downloads
 * through gst_element_make_from_uri, i.e. webkitwebsrc and the
 * NetworkProcess, which is WebKit's supported HLS path.
 */
#include <gst/gst.h>

#define GST_PS5_PLUGINS(X)                                                   \
    /* core */           X(coreelements)                                     \
    /* -base */          X(app) X(audioconvert) X(audioresample)             \
                         X(typefindfunctions) X(playback) X(videoconvertscale) \
                         X(volume) X(pbtypes) X(rawparse) X(subparse) X(gio)   \
    /* -good */          X(isomp4) X(matroska) X(audioparsers) X(id3demux)   \
                         X(apetag) X(icydemux) X(wavparse) X(adaptivedemux2) \
                         X(autodetect) X(deinterlace) X(videofilter) X(audiofx) \
    /* -bad */           X(videoparsersbad) X(mpegtsdemux) X(mpegpsdemux)    \
                         X(id3tag) X(hls)                                    \
    /* gst-libav */      X(libav)                                            \
    /* ours: audio and hole-punch video to the title */ X(ps5audio) X(ps5video)

#define DECLARE(name) GST_PLUGIN_STATIC_DECLARE(name);
GST_PS5_PLUGINS(DECLARE)

void gst_init_static_plugins(void);

void gst_init_static_plugins(void)
{
    static gsize done = 0;
    if (g_once_init_enter(&done)) {
#define REGISTER(name) GST_PLUGIN_STATIC_REGISTER(name);
        GST_PS5_PLUGINS(REGISTER)
        g_once_init_leave(&done, 1);
    }
}
