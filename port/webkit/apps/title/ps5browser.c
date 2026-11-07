/*
 * ps5browser: the WebKit UIProcess as PS5 title PPSA99020 (Milestone 1).
 *
 * WPE WebKit with the PS5 WPE Platform backend (wpe-ps5): the page is shown
 * on the TV through ps5-opengl, the DualSense drives it, and the WebProcess
 * and NetworkProcess run as elfldr payloads (/app0/ps5wk-child.elf, launched
 * by WPEProcessManagerPS5). Pages are painted on the CPU (hardware
 * acceleration off) and arrive as shared-memory buffers.
 *
 * Pad (see WPEDisplayPS5.cpp for the full map):
 *   left stick: pointer, X: click (or Enter when the pointer is hidden),
 *   right stick: scroll, D-pad: move between links (spatial navigation),
 *   O: back, Options: the browser menu (address bar, tabs, bookmarks,
 *   history: browserui.c), Square: reload,
 *   L2/R2: zoom out/in, L1/R1: previous/next field, Triangle: keyboard
 *   (also opens by itself when X on a field, the D-pad or L1/R1 focus one),
 *   R3 (press the right stick): Settings, and back to the page from there,
 *   L3 (press the left stick): home page,
 *   touchpad: View in VR (the playing video in the PS VR2 headset; its own
 *   controls while there, see viewinvr.c).
 * (every button is taken: the ad blocker's per-site switch is on the
 * Settings page, R3, adblock.c)
 *
 * Logs go to the Mac's tools/logserver.sh (LOG_HOST:LOG_PORT) and to
 * /data/ps5webkit/ps5browser.log.
 */
#define SPIKE_NAME "ps5browser"
#include "../common/ps5audioserver.h"
#include "../common/webmem.h"
#include "common/title.h"
#include "adblock.h"
#include "browser.h"
#include "downloads.h"
#include "qrcode.h"
#include "settings.h"
#include "profile.h"
#include "viewinvr.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <sys/filio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <wpe/ps5/wpe-ps5.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>

/* The Mac running tools/logserver.sh (the build sets it from
 * PS5WK_LOG_HOST; empty: no TCP log, /data/ps5webkit/log-sink can name one). */
#ifndef LOG_HOST
#define LOG_HOST ""
#define LOG_PORT 9999
#endif

#ifndef PS5WK_HOME_URL
#define PS5WK_HOME_URL "file:///data/ps5webkit/start.html"
#endif

void g_io_openssl_load(void*);

/* titlemem.c, linked into the title only (the payload-style build of this
 * file has no direct-memory heap): weak, NULL there. */
void ps5wk_title_mem_log(const char* milestone) __attribute__((weak));
size_t ps5wk_title_flexible_free(void) __attribute__((weak));
unsigned ps5wk_title_check_imports(char* out, size_t size, unsigned* total) __attribute__((weak));
const char* ps5wk_title_build_id(void) __attribute__((weak));
static double flexibleMB(void)
{
    return ps5wk_title_flexible_free ? ps5wk_title_flexible_free() / 1048576.0 : -1.0;
}
static void memLog(const char* milestone)
{
    if (ps5wk_title_mem_log)
        ps5wk_title_mem_log(milestone);
}

static WebKitWebView* webView;
static GMainLoop* mainLoop;
static WebKitNetworkSession* networkSession;
static gint64 startedAt; /* Unix seconds: the settings page sees a restart */

/* ---- Clean wind-down (asked by the launcher before it SIGKILLs us) ----
 * tools/launcher "stop" connects to PS5WK_CONTROL_SOCKET and sends "quit".
 * The title then: loads about:blank (the WebProcess stops playback and its
 * GStreamer pipeline, so its audio stream closes), waits for the audio
 * streams to end (2 s at most), ends the WebProcess, stops the audio server
 * (wakes and waits for its threads, closes every sceAudioOut port), and
 * leaves the main loop; main returns and title_exit.c asks the launcher for
 * the SIGKILL, now with nothing playing. The launcher kills it anyway if it
 * is still there after a timeout. (2026-10-28: a SIGKILL request during a
 * media test run left the console wedged.) */
#define PS5WK_CONTROL_SOCKET "/system_tmp/ps5wk/ps5browser-control.sock"
static gint64 windDownStart;

static gboolean windDownStep(gpointer data)
{
    (void)data;
    double ms = (g_get_monotonic_time() - windDownStart) / 1000.0;
    int streams = ps5wk_audio_server_active_streams();
    if (streams && ms < 2000)
        return G_SOURCE_CONTINUE;
    spike_log("wind-down: %d audio stream(s) left after %.0f ms; ending the WebProcess and the audio server", streams, ms);
    if (webView)
        webkit_web_view_terminate_web_process(webView);
    ps5wk_audio_server_stop();
    spike_log("wind-down: done after %.0f ms; leaving the main loop", (g_get_monotonic_time() - windDownStart) / 1000.0);
    if (mainLoop)
        g_main_loop_quit(mainLoop);
    return G_SOURCE_REMOVE;
}

static gboolean beginWindDown(gpointer data)
{
    (void)data;
    if (windDownStart)
        return G_SOURCE_REMOVE;
    windDownStart = g_get_monotonic_time();
    /* The headset first: Hmd2/VrTracker2 left open could need a reboot. */
    ps5wk_vr_stop(TRUE, "wind-down");
    /* The open tabs, before about:blank replaces the page. */
    ps5wk_browser_close_menu();
    ps5wk_browser_save();
    /* Downloads still running are saved as paused (Resume continues them
     * from their .part files at the next start). */
    ps5wk_downloads_wind_down();
    spike_log("wind-down: quit requested; %d audio stream(s) open; loading about:blank", ps5wk_audio_server_active_streams());
    if (webView)
        webkit_web_view_load_uri(webView, "about:blank");
    g_timeout_add(50, windDownStep, NULL);
    return G_SOURCE_REMOVE;
}

static const char* pushRequest(const char* text, const char* from, char** uriOut);
static void setShownURI(const char* uri);
static void toggleSettings(void);
static char* homeURL(void);

static void* controlThread(void* unused)
{
    (void)unused;
    int fd = -1;
    /* PS5WK_LAUNCH_DIR exists once the first payload made it; a title can
     * only bind in a directory a payload created. */
    while (fd < 0) {
        struct sockaddr_un address;
        memset(&address, 0, sizeof address);
        address.sun_family = AF_UNIX;
        strncpy(address.sun_path, PS5WK_CONTROL_SOCKET, sizeof address.sun_path - 1);
        unlink(address.sun_path);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0 && !bind(fd, (struct sockaddr*)&address, sizeof address) && !listen(fd, 4))
            break;
        if (fd >= 0)
            close(fd);
        fd = -1;
        usleep(500 * 1000);
    }
    chmod(PS5WK_CONTROL_SOCKET, 0777);
    spike_log("control: listening on %s", PS5WK_CONTROL_SOCKET);
    for (;;) {
        int client = accept(fd, NULL, NULL);
        if (client < 0) {
            usleep(100 * 1000);
            continue;
        }
        char line[4096] = "";
        ssize_t n = recv(client, line, sizeof line - 1, 0);
        line[n > 0 ? n : 0] = 0;
        if (!strncmp(line, "open ", 5)) {
            /* "open <url>": load it, as tools/ps5open's HTTP request does. */
            line[strcspn(line, "\r\n")] = 0;
            char* uri = NULL;
            const char* error = pushRequest(line + 5, "control socket", &uri);
            char* reply = g_strdup_printf("%s %s\n", uri ? "OK loading" : "ERR", uri ? uri : error);
            send(client, reply, strlen(reply), 0);
            g_free(reply);
            g_free(uri);
        } else if (!strncmp(line, "quit", 4)) {
            static const char reply[] = "OK winding down\n";
            send(client, reply, sizeof reply - 1, 0);
            g_main_context_invoke(NULL, beginWindDown, NULL);
        } else {
            static const char reply[] = "ERR unknown command (quit, open <url>)\n";
            send(client, reply, sizeof reply - 1, 0);
        }
        close(client);
    }
    return NULL;
}
static WPEDisplay* display;
static double zoom = 1.0;

static void logHandler(const gchar* domain, GLogLevelFlags level, const gchar* message, gpointer data)
{
    (void)data;
    const char* kind = (level & G_LOG_LEVEL_ERROR) ? "ERROR" : (level & G_LOG_LEVEL_CRITICAL) ? "CRITICAL"
        : (level & G_LOG_LEVEL_WARNING) ? "WARNING" : (level & G_LOG_LEVEL_MESSAGE) ? "msg" : "info";
    spike_log("[%s %s] %s", domain ? domain : "app", kind, message);
}

static char* uriFromInput(const char* text);
char* ps5wk_uri_from_input(const char* text)
{
    return uriFromInput(text);
}

static char* uriFromInput(const char* text)
{
    while (*text == ' ')
        text++;
    if (!*text)
        return NULL;
    if (strstr(text, "://") || !strncmp(text, "about:", 6) || !strncmp(text, "data:", 5))
        return g_strdup(text);
    if (!strchr(text, ' ') && strchr(text, '.'))
        return g_strdup_printf("https://%s", text);
    char* query = g_uri_escape_string(text, NULL, FALSE);
    char* uri = g_strdup_printf("https://lite.duckduckgo.com/lite/?q=%s", query);
    g_free(query);
    return uri;
}

static gboolean onViewEvent(WPEView* view, WPEEvent* event, gpointer data)
{
    (void)view;
    (void)data;
    if (wpe_event_get_event_type(event) != WPE_EVENT_KEYBOARD_KEY_DOWN)
        return FALSE;
    switch (wpe_event_keyboard_get_keyval(event)) {
    case WPE_KEY_Back:
        /* not into the empty first entry of a new tab */
        ps5wk_browser_go_back();
        return TRUE;
    case WPE_KEY_Reload:
        webkit_web_view_reload(webView);
        return TRUE;
    case WPE_KEY_Tools: /* R3 */
        toggleSettings();
        return TRUE;
    case WPE_KEY_HomePage: { /* L3 */
        char* home = homeURL();
        spike_log("home: L3, opening %s", home);
        webkit_web_view_load_uri(webView, home);
        g_free(home);
        return TRUE;
    }
    case WPE_KEY_ZoomIn:
    case WPE_KEY_ZoomOut:
        zoom *= wpe_event_keyboard_get_keyval(event) == WPE_KEY_ZoomIn ? 1.15 : 1 / 1.15;
        zoom = zoom < 0.3 ? 0.3 : zoom > 4.0 ? 4.0 : zoom;
        webkit_web_view_set_zoom_level(webView, zoom);
        spike_log("zoom %.2f", zoom);
        return TRUE;
    case WPE_KEY_Menu:
        /* Options is the browser's button (browserui.c takes it first). */
        ps5wk_browser_toggle_menu();
        return TRUE;
    default:
        return FALSE;
    }
}

/* Performance overlay (port/webkit/OVERLAY.md): Settings' "Performance
 * overlay" is the flag file, which the renderer reads at start; a change in
 * Settings applies at once, and the pad's L3 + R3 updates the file. */
#define PERF_OVERLAY_FLAG "/data/ps5webkit/perf-overlay"

static gboolean perfOverlayFlag(void)
{
    int fd = open(PERF_OVERLAY_FLAG, O_RDONLY); /* not access(): see settings.c */
    if (fd < 0)
        return FALSE;
    close(fd);
    return TRUE;
}

static void applyPerfOverlaySetting(void)
{
    gboolean on = perfOverlayFlag();
    if (on != wpe_display_ps5_get_perf_overlay(WPE_DISPLAY_PS5(display))) {
        spike_log("settings: performance overlay %s", on ? "on" : "off");
        wpe_display_ps5_set_perf_overlay(WPE_DISPLAY_PS5(display), on);
    }
}

static void onPerfOverlayToggled(WPEDisplayPS5* d, gboolean enabled, gpointer data)
{
    (void)d;
    (void)data;
    if (enabled && !perfOverlayFlag()) { /* an existing file keeps its options */
        int fd = open(PERF_OVERLAY_FLAG, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            (void)!write(fd, "1\n", 2);
            close(fd);
        }
    } else if (!enabled)
        unlink(PERF_OVERLAY_FLAG);
}

/* Debug screen readback: /data/ps5webkit/debug-screenshot (a file, any
 * content) or PS5WK_SCREENSHOT=1. Frames go to /data/ps5webkit/shots as PPM
 * (port/webkit/test/fetch-shots.sh fetches and converts them). */
#define SHOT_DIR "/data/ps5webkit/shots"
static gboolean screenshots;
static unsigned shotCount;

static void captureScreen(const char* name)
{
    if (!screenshots)
        return;
    char path[256];
    snprintf(path, sizeof path, SHOT_DIR "/%02u-%s.ppm", ++shotCount, name);
    wpe_display_ps5_capture_screen(WPE_DISPLAY_PS5(display), path);
}

/* Test URL list: /data/ps5webkit/test-urls.txt, one URL per line (# comments),
 * optionally followed by how long to stay on it ("<url> 40"; default 12 s).
 * Each is loaded in turn; after it finishes: a screenshot at +2 s and +10 s,
 * load time and memory in the log, then the next one after the dwell. */
static char** testURLs;
static unsigned testDwell[256];
static unsigned testURLCount, testURLIndex, testURLsLoaded;
static gint64 loadStart;
static guint testTimer;
static gboolean testWaitingForLoad;

static void loadNextTestURL(void);

/* Screenshot series: debug-screenshot containing "series=<ms>x<count>"
 * captures every <ms> ms from each test URL's load start, <count> times
 * (NN-u<url>-s<shot>-<ms after load start>ms.ppm), e.g. to time images appearing. */
static unsigned seriesInterval, seriesCount, seriesTaken;
static guint seriesTimer;

static gboolean seriesShot(gpointer data)
{
    (void)data;
    char name[64];
    unsigned ms = (unsigned)((g_get_monotonic_time() - loadStart) / 1000);
    snprintf(name, sizeof name, "u%u-s%03u-%05ums", testURLIndex, seriesTaken + 1, ms);
    captureScreen(name);
    if (++seriesTaken >= seriesCount) {
        seriesTimer = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean testTimeout(gpointer data)
{
    (void)data;
    testTimer = 0;
    if (testWaitingForLoad) {
        SPIKE_FAIL("url", "%u/%u %s: not finished after 60 s", testURLIndex, testURLCount, testURLs[testURLIndex - 1]);
        captureScreen("timeout");
        memLog("url timeout");
        testWaitingForLoad = FALSE;
    }
    loadNextTestURL();
    return G_SOURCE_REMOVE;
}

static void loadNextTestURL(void)
{
    if (testURLIndex >= testURLCount) {
        SPIKE_RESULT("urls", testURLsLoaded == testURLCount ? "PASS" : "FAIL", "%u of %u test URLs loaded", testURLsLoaded, testURLCount);
        memLog("test URLs done");
        return;
    }
    const char* uri = testURLs[testURLIndex++];
    spike_log("test URL %u/%u: %s", testURLIndex, testURLCount, uri);
    testWaitingForLoad = TRUE;
    loadStart = g_get_monotonic_time();
    webkit_web_view_load_uri(webView, uri);
    if (seriesTimer)
        g_source_remove(seriesTimer);
    seriesTimer = 0;
    seriesTaken = 0;
    if (screenshots && seriesInterval && seriesCount)
        seriesTimer = g_timeout_add(seriesInterval, seriesShot, NULL);
    testTimer = g_timeout_add_seconds(60, testTimeout, NULL);
}

static gboolean shotAfterLoad(gpointer data)
{
    captureScreen((const char*)data);
    return G_SOURCE_REMOVE;
}

/* /data/ps5webkit/test-onload.js: evaluated in each test URL's page 2 s
 * after it loaded (log-only runs: e.g. port/webkit/test/media/yt-consent.js
 * answers YouTube's cookie dialog, which pauses the video). */
static gboolean testOnload(gpointer data)
{
    (void)data;
    gchar* script = NULL;
    gsize length = 0;
    if (g_file_get_contents("/data/ps5webkit/test-onload.js", &script, &length, NULL)) {
        webkit_web_view_evaluate_javascript(webView, script, (gssize)length, NULL, "test-onload.js", NULL, NULL, NULL);
        spike_log("test-onload.js evaluated (%zu bytes)", (size_t)length);
        g_free(script);
    }
    return G_SOURCE_REMOVE;
}

static void testURLFinished(gboolean ok, const char* uri, const char* detail)
{
    if (!testWaitingForLoad)
        return;
    testWaitingForLoad = FALSE;
    if (testTimer)
        g_source_remove(testTimer);
    double seconds = (g_get_monotonic_time() - loadStart) / 1e6;
    if (ok)
        testURLsLoaded++;
    SPIKE_RESULT("url", ok ? "PASS" : "FAIL", "%u/%u %s %s in %.1f s: %s; flexible %.1f MB", testURLIndex, testURLCount,
        uri, ok ? "loaded" : "failed", seconds, detail, flexibleMB());
    char milestone[64];
    snprintf(milestone, sizeof milestone, "after test URL %u", testURLIndex);
    memLog(milestone);
    /* Screenshot names: the page's host, e.g. "03-en.wikipedia.org-2s". */
    static char names[16][3][96];
    const char* host = strstr(uri, "://") ? strstr(uri, "://") + 3 : uri;
    size_t hostLength = strcspn(host, "/?#:");
    unsigned slot = testURLIndex % 16;
    snprintf(names[slot][0], sizeof names[slot][0], "%.*s-2s", (int)(hostLength < 80 ? hostLength : 80), host);
    snprintf(names[slot][1], sizeof names[slot][1], "%.*s-10s", (int)(hostLength < 80 ? hostLength : 80), host);
    g_timeout_add(2000, shotAfterLoad, names[slot][0]);
    g_timeout_add(2000, testOnload, NULL);
    g_timeout_add(10000, shotAfterLoad, names[slot][1]);
    ps5wk_vr_selftest_schedule(8);
    unsigned dwell = testURLIndex - 1 < 256 && testDwell[testURLIndex - 1] ? testDwell[testURLIndex - 1] : 12;
    if (dwell >= 34) { /* long stays (video): one more at +30 s, in steady playback */
        snprintf(names[slot][2], sizeof names[slot][2], "%.*s-30s", (int)(hostLength < 80 ? hostLength : 80), host);
        g_timeout_add(30000, shotAfterLoad, names[slot][2]);
    }
    testTimer = g_timeout_add_seconds(dwell, testTimeout, NULL);
}

static gboolean readTestURLs(void)
{
    gchar* contents = NULL;
    if (!g_file_get_contents("/data/ps5webkit/test-urls.txt", &contents, NULL, NULL))
        return FALSE;
    GPtrArray* list = g_ptr_array_new();
    char** lines = g_strsplit(contents, "\n", -1);
    for (char** line = lines; *line; line++) {
        g_strstrip(*line);
        if (**line && **line != '#') {
            char* space = strpbrk(*line, " \t");
            if (space) {
                *space = 0;
                if (list->len < 256)
                    testDwell[list->len] = (unsigned)atoi(space + 1);
            }
            g_ptr_array_add(list, g_strdup(*line));
        }
    }
    g_strfreev(lines);
    g_free(contents);
    testURLCount = list->len;
    g_ptr_array_add(list, NULL);
    testURLs = (char**)g_ptr_array_free(list, FALSE);
    spike_log("test URL list: %u URLs", testURLCount);
    return testURLCount > 0;
}

/* The WebProcess memory limit (apps/common/webmem.h): over it, the next
 * top-level navigation ends the WebProcess and loads the address in a new
 * one. memRestartURI is set from then until the new WebProcess commits.
 * "Restart page" in Settings does the same for the page shown
 * (settingsRestart), so that a new WebProcess reads child.env. */
static char* memRestartURI;
static gboolean memRestarting;
static gboolean settingsRestart;
/* The address the restart loads (until it commits) and how often its new
 * WebProcess ended before that (a failed launch). */
static char* restartTargetURI;
static int restartFailures;

static gboolean memRestart(gpointer data)
{
    (void)data;
    if (!webView || !memRestartURI)
        return G_SOURCE_REMOVE;
    memRestarting = TRUE;
    ps5wk_vr_stop(FALSE, "WebProcess restart");
    /* Hidden live tabs share the WebProcess: their pages go first. */
    ps5wk_browser_discard_hidden("the WebProcess restarts");
    /* No provisional load left for WebKit to continue in a process of its
     * own (it launched one that the load below then didn't use). */
    webkit_web_view_stop_loading(webView);
    webkit_web_view_terminate_web_process(webView); /* web-process-terminated, then the load below */
    ps5wk_settings_applied(PS5WK_SETTINGS_PAGE);
    spike_log("[mem] WebProcess ended (%s); loading %s in a new one", settingsRestart ? "Settings: restart page" : "memory limit", memRestartURI);
    char* uri = memRestartURI;
    memRestartURI = NULL;
    g_free(restartTargetURI);
    restartTargetURI = g_strdup(uri);
    restartFailures = 0;
    webkit_web_view_load_uri(webView, uri);
    g_free(uri);
    return G_SOURCE_REMOVE;
}

/* What is under the pointer: a field that gets focus right after a click
 * opens the system keyboard only when the click was on an editable node, so
 * a button whose click handler focuses a field doesn't reopen it
 * (wpe_display_ps5_set_pointer_over_editable). WebKit hit tests on every
 * pointer move, and the display sends one at the pointer before each click,
 * so the latest result is what the click lands on. */
static void onMouseTargetChanged(WebKitWebView* view, WebKitHitTestResult* hit, guint modifiers, gpointer data)
{
    (void)modifiers;
    (void)data;
    if (view != webView)
        return;
    wpe_display_ps5_set_pointer_over_editable(WPE_DISPLAY_PS5(display), webkit_hit_test_result_context_is_editable(hit));
}

static void onLoadChanged(WebKitWebView* view, WebKitLoadEvent event, gpointer data)
{
    (void)data;
    static const char* names[] = { "started", "redirected", "committed", "finished" };
    if (view != webView) {
        /* A hidden live tab (browserui.c): nothing here is about it. */
        spike_log("load %s (hidden tab): %s", names[event], webkit_web_view_get_uri(view));
        ps5wk_browser_load_changed(view, event);
        return;
    }
    spike_log("load %s: %s", names[event], webkit_web_view_get_uri(view));
    /* Tabs, history, the address bar (browserui.c). */
    ps5wk_browser_load_changed(view, event);
    /* The ad blocker's filter on or off for this page (allowed sites). */
    if (event != WEBKIT_LOAD_FINISHED)
        ps5wk_adblock_navigation(webkit_web_view_get_uri(view));
    int webPid = 0;
    double committed = 0, limit = 0;
    if (event == WEBKIT_LOAD_STARTED && !memRestartURI && !memRestarting && !windDownStart && webkit_web_view_get_uri(view)
        && ps5wk_webmem_over(&webPid, &committed, &limit)) {
        spike_log("[mem] WebProcess (pid %d) has %.0f MB of libpas memory committed, limit %.0f MB (PS5WK_MEM_RESTART_MB): restarting it for %s",
            webPid, committed, limit, webkit_web_view_get_uri(view));
        ps5wk_webmem_forget();
        memRestartURI = g_strdup(webkit_web_view_get_uri(view));
        g_idle_add(memRestart, NULL);
        return;
    }
    if (event == WEBKIT_LOAD_COMMITTED && memRestarting) {
        memRestarting = FALSE;
        g_clear_pointer(&restartTargetURI, g_free);
        if (settingsRestart) {
            settingsRestart = FALSE;
            SPIKE_PASS("page-restart", "new WebProcess committed %s", webkit_web_view_get_uri(view));
            GDateTime* now = g_date_time_new_now_local();
            char* time = g_date_time_format(now, "%H:%M:%S");
            ps5wk_settings_set_status("The page restarted at %s with the settings on disk.", time);
            g_free(time);
            g_date_time_unref(now);
        } else
            SPIKE_PASS("mem-restart", "new WebProcess committed %s", webkit_web_view_get_uri(view));
    }
    if (event == WEBKIT_LOAD_COMMITTED) {
        ps5wk_vr_stop(FALSE, "another page");
        setShownURI(webkit_web_view_get_uri(view));
    }
    if (event == WEBKIT_LOAD_FINISHED) {
        const char* title = webkit_web_view_get_title(view) ? webkit_web_view_get_title(view) : "";
        SPIKE_PASS("load", "%s \"%s\" (%.1f s after start)", webkit_web_view_get_uri(view), title, (g_get_monotonic_time() - loadStart) / 1e6);
        memLog("load finished");
        if (!testURLs)
            g_timeout_add(2000, shotAfterLoad, (gpointer) "loaded");
        char detail[300];
        snprintf(detail, sizeof detail, "title \"%s\"", title);
        testURLFinished(TRUE, webkit_web_view_get_uri(view), detail);
    }
}

static gboolean onLoadFailed(WebKitWebView* view, WebKitLoadEvent event, const char* uri, GError* error, gpointer data)
{
    (void)event;
    (void)data;
    if (view != webView)
        return FALSE;
    if (memRestartURI || memRestarting) {
        spike_log("[mem] load of %s ended by the WebProcess restart (%s)", uri, error->message);
        return FALSE;
    }
    SPIKE_FAIL("load", "%s: %s", uri, error->message);
    testURLFinished(FALSE, uri, error->message);
    return FALSE;
}

static gboolean memTimer(gpointer data)
{
    (void)data;
    memLog("every 60 s");
    return G_SOURCE_CONTINUE;
}

/* A page whose WebProcess keeps crashing would reload forever: after the
 * second crash on the same address within 30 s, show an error page instead. */
static char* lastCrashURI;
static gint64 lastCrashTime;

static void onWebProcessTerminated(WebKitWebView* view, WebKitWebProcessTerminationReason reason, gpointer data)
{
    (void)data;
    const char* uri = webkit_web_view_get_uri(view);
    gint64 now = g_get_monotonic_time();
    ps5wk_vr_stop(FALSE, "the page's process ended");
    if (windDownStart) {
        /* The wind-down ended it: a reload here started another 130 MB
         * upload to elfldr just before the SIGKILL, and a title killed
         * mid-upload leaves a payload that never ends (CONSOLE-HEALTH.md). */
        spike_log("wind-down: the WebProcess ended (reason %d); no reload", reason);
        return;
    }
    /* The one WebProcess serves every tab: a hidden live tab goes back to
     * its record (browserui.c), only the shown page reloads. */
    if (ps5wk_browser_view_terminated(view)) {
        spike_log("[tabs] a hidden tab's page ended with the WebProcess (reason %d)", reason);
        return;
    }
    /* Any page, any reason but ours: more than 6 ends in 2 minutes is a
     * WebProcess that dies at start (2026-11-02: ~80 crashed in a row, the
     * error page's process crashing too, until the title was killed in the
     * middle of a launch). Stop launching; the page stays as it is. */
    static gint64 recentEnds[6];
    static unsigned recentEndCount;
    if (reason != WEBKIT_WEB_PROCESS_TERMINATED_BY_API) {
        gint64 oldest = recentEnds[recentEndCount % G_N_ELEMENTS(recentEnds)];
        recentEnds[recentEndCount++ % G_N_ELEMENTS(recentEnds)] = now;
        if (recentEndCount > G_N_ELEMENTS(recentEnds) && now - oldest < 120 * G_USEC_PER_SEC) {
            SPIKE_FAIL("webprocess", "terminated (reason %d), the %u. time, %u times in %.0f s: not starting another (restart the browser)",
                reason, recentEndCount, (unsigned)G_N_ELEMENTS(recentEnds) + 1, (now - oldest) / 1e6);
            return;
        }
    }
    if (memRestarting && reason == WEBKIT_WEB_PROCESS_TERMINATED_BY_API) {
        spike_log("[mem] WebProcess restart: the old one is gone");
        return;
    }
    gboolean again = uri && lastCrashURI && !strcmp(uri, lastCrashURI) && now - lastCrashTime < 30 * G_USEC_PER_SEC;
    g_free(lastCrashURI);
    lastCrashURI = g_strdup(uri ? uri : "");
    lastCrashTime = now;
    ps5wk_settings_applied(PS5WK_SETTINGS_PAGE); /* the next WebProcess reads child.env */
    if (memRestarting && restartTargetURI && ++restartFailures <= 3) {
        /* The new WebProcess of a restart ended before the page committed
         * (its launch failed: WPEProcessManagerPS5 then closes its IPC
         * socket). Nothing committed to reload: load the address again. */
        SPIKE_FAIL("webprocess", "the restarted page's process ended before its first load (reason %d, %d. time); loading %s in another",
            reason, restartFailures, restartTargetURI);
        char* address = g_strdup(restartTargetURI);
        webkit_web_view_load_uri(view, address);
        g_free(address);
        return;
    }
    if (memRestarting && restartTargetURI) {
        /* Three new processes in a row did not start: the error page. */
        g_free(lastCrashURI);
        lastCrashURI = g_strdup(restartTargetURI);
        again = TRUE;
    }
    if (!again) {
        SPIKE_FAIL("webprocess", "terminated (reason %d); reloading", reason);
        webkit_web_view_reload(view);
        return;
    }
    memRestarting = FALSE; /* the error page below is not the restarted page */
    settingsRestart = FALSE;
    g_clear_pointer(&restartTargetURI, g_free);
    SPIKE_FAIL("webprocess", "terminated (reason %d) again on %s within 30 s; showing an error page", reason, lastCrashURI);
    char* escaped = g_markup_escape_text(lastCrashURI, -1);
    char* html = g_strdup_printf("<!doctype html><html><head><meta charset=\"utf-8\"><title>Page crashed</title>"
        "<style>body{font-family:sans-serif;background:#01001a;color:#f4f2ff;margin:64px;font-size:24px}"
        "a{color:#c7a8ff}a:focus{outline:3px solid #ff5cd6;outline-offset:4px}code{color:#b9b5e0;word-break:break-all}</style></head><body>"
        "<h1>This page keeps crashing</h1><p>The browser stopped reloading it after two crashes.</p>"
        "<p><code>%s</code></p><p><a href=\"%s\">Try again</a> &middot; <a href=\"file:///data/ps5webkit/start.html\">Start page</a></p>"
        "<p>O goes back.</p></body></html>", escaped, escaped);
    /* Not a failed load of the page itself, so Back still works. */
    webkit_web_view_load_alternate_html(view, html, lastCrashURI, NULL);
    g_free(html);
    g_free(escaped);
    /* The error page itself must not count as the page crashing again. */
    lastCrashTime = 0;
}

char* ps5wk_home_url(void)
{
    return homeURL();
}

static char* homeURL(void)
{
    gchar* contents = NULL;
    char* path = g_build_filename(ps5wk_profile_dir(), "home-url.txt", NULL); /* settings.c: the user's */
    gboolean found = g_file_get_contents(path, &contents, NULL, NULL);
    g_free(path);
    if (found) {
        g_strstrip(contents);
        if (*contents)
            return contents;
        g_free(contents);
    }
    return g_strdup(PS5WK_HOME_URL);
}

static int fileExists(const char* path);

/* ---- Send a link from another device (README: "Send a link from another
 * device") ----
 * A small HTTP server on TCP PS5WK_PUSH_PORT, on its own thread:
 *   GET /open?url=<encoded>, POST /open (body: the URL, or url=<encoded>)
 *       load it here; "OK loading <url>" (JSON for Accept: application/json,
 *       a "Sent" page for browsers, which closes itself in a pop-up)
 *   GET /?url=<encoded>  the same, for the bookmarklet's pop-up
 *   GET /                a page to type a link into, and the bookmarklet
 *   GET /status          {"ok":true,"uri":"<the page shown>"}
 *   POST /cookies        Netscape cookies.txt lines (tools/ps5open --cookies,
 *       from Chrome) added to the cookie store. Knob:
 *       /data/ps5webkit/push-cookie-key.txt: a key the X-PS5-Key header must match
 *   GET /settings, GET /settings.json, POST /settings, POST /restart,
 *       POST /clear  the settings page and its API (settingsServe below)
 *   GET /link.json  (the PS5 itself only) this server's address on the LAN
 *       now, for the start page's and Settings' QR code: {"localOnly",
 *       "pin", "url", "qr": rows of "0"/"1"}; no url without an address or
 *       with push-off
 *   GET /logo.png   Parallax's logo (/app0/ui), for these pages
 *   GET /downloads/<name>  a finished download (only names in the list,
 *       inside /data/ps5webkit/downloads), with Range support, streamed on
 *       a thread of its own (the server goes on answering); the PIN as for
 *       /open. GET /downloads.json: the finished downloads
 * The control socket takes "open <url>" too. Bare hosts get https://, other
 * words a search; only http, https and file:///data/ps5webkit/ URLs load.
 * Loading a page also leaves View in VR (onLoadChanged).
 * Knobs: /data/ps5webkit/push-off (a file): the server listens on
 * 127.0.0.1 only (the in-app Settings page still opens; the LAN can't).
 * /data/ps5webkit/push-pin.txt: a PIN that requests must carry (pin=...).
 * A request is read within 3 s and 64 KB, one at a time: a slow or
 * oversized client gets an error, never a stuck title. Nothing here waits
 * for the main loop (the browser itself loads /settings from this thread). */
#define PS5WK_PUSH_PORT 9040
#define PS5WK_PUSH_MAX 65536 /* a site's cookies fit */
#define PS5WK_COOKIE_KEY "/data/ps5webkit/push-cookie-key.txt"
#define PS5WK_PENDING_URL "/data/ps5webkit/open-url.txt"

static GMutex shownLock;
static char* shownURI; /* the committed page, for /status (any thread) */
static char* settingsReturnURI; /* the page R3 left for Settings (shownLock) */
static char* pushPIN;
static gboolean pushLocalOnly; /* push-off: 127.0.0.1 only */
static WebKitCookieManager* cookieManager;

static void setShownURI(const char* uri)
{
    g_mutex_lock(&shownLock);
    g_free(shownURI);
    shownURI = g_strdup(uri ? uri : "");
    g_mutex_unlock(&shownLock);
}

static char* copyShownURI(void)
{
    g_mutex_lock(&shownLock);
    char* uri = g_strdup(shownURI ? shownURI : "");
    g_mutex_unlock(&shownLock);
    return uri;
}

/* Any thread. A new URI to load, or NULL and *error. */
static char* pushNormalize(const char* text, const char** error)
{
    char* input = g_strstrip(g_strdup(text ? text : ""));
    char* uri = NULL;
    *error = NULL;
    if (!*input)
        *error = "no URL";
    else if (strlen(input) > 8000)
        *error = "URL too long";
    for (const char* p = input; !*error && *p; p++) {
        if ((unsigned char)*p < 0x20 || *p == 0x7f)
            *error = "control characters in the URL";
    }
    if (!*error)
        uri = uriFromInput(input);
    g_free(input);
    if (!uri) {
        if (!*error)
            *error = "no URL";
        return NULL;
    }
    if (strstr(uri, "://") && strchr(uri, ' ')) { /* pasted with spaces */
        GString* fixed = g_string_new(NULL);
        for (const char* p = uri; *p; p++) {
            if (*p == ' ')
                g_string_append(fixed, "%20");
            else
                g_string_append_c(fixed, *p);
        }
        g_free(uri);
        uri = g_string_free(fixed, FALSE);
    }
    if (!g_ascii_strncasecmp(uri, "http://", 7) || !g_ascii_strncasecmp(uri, "https://", 8)) {
        GUri* parsed = g_uri_parse(uri, G_URI_FLAGS_ENCODED | G_URI_FLAGS_PARSE_RELAXED, NULL);
        if (!parsed || !g_uri_get_host(parsed) || !*g_uri_get_host(parsed))
            *error = "not a valid http(s) URL";
        if (parsed)
            g_uri_unref(parsed);
    } else if (!strncmp(uri, "file:///data/ps5webkit/", 23)) {
        if (strstr(uri, "..") || strcasestr(uri, "%2e"))
            *error = "no .. in file URLs";
    } else {
        *error = "only http, https and file:///data/ps5webkit/ URLs";
    }
    if (*error) {
        g_free(uri);
        return NULL;
    }
    return uri;
}

static gboolean pushOpenOnMain(gpointer data)
{
    const char* uri = data;
    if (!webView || windDownStart)
        return G_SOURCE_REMOVE;
    if (ps5wk_vr_active())
        ps5wk_vr_stop(FALSE, "a link was sent");
    ps5wk_browser_close_menu();
    spike_log("push: loading %s", uri);
    loadStart = g_get_monotonic_time();
    webkit_web_view_load_uri(webView, uri);
    return G_SOURCE_REMOVE;
}

/* Any thread: check text and load it on the main loop. NULL and *uriOut
 * (free it) when it is on its way, else the reason. */
static const char* pushRequest(const char* text, const char* from, char** uriOut)
{
    const char* error = NULL;
    *uriOut = NULL;
    if (windDownStart)
        return "the browser is closing";
    char* uri = pushNormalize(text, &error);
    if (!uri) {
        spike_log("push from %s refused (%s): %.200s", from, error, text ? text : "");
        return error;
    }
    spike_log("push from %s: %s", from, uri);
    *uriOut = g_strdup(uri);
    g_idle_add_full(G_PRIORITY_DEFAULT, pushOpenOnMain, uri, g_free);
    return NULL;
}

static void cookieAdded(GObject* manager, GAsyncResult* result, gpointer data)
{
    char* name = data;
    GError* error = NULL;
    if (!webkit_cookie_manager_add_cookie_finish(WEBKIT_COOKIE_MANAGER(manager), result, &error)) {
        spike_log("push: cookie %s not added: %s", name, error ? error->message : "?");
        g_clear_error(&error);
    }
    g_free(name);
}

static gboolean pushCookiesOnMain(gpointer data)
{
    GPtrArray* cookies = data;
    for (guint i = 0; cookieManager && i < cookies->len; i++) {
        SoupCookie* cookie = g_ptr_array_index(cookies, i);
        webkit_cookie_manager_add_cookie(cookieManager, cookie, NULL, cookieAdded,
            g_strdup_printf("%s on %s", soup_cookie_get_name(cookie), soup_cookie_get_domain(cookie)));
    }
    return G_SOURCE_REMOVE;
}

/* Any thread: cookies.txt lines (domain, subdomains flag, path, secure,
 * expiry in Unix seconds or 0 for a session cookie, name, value, and
 * optionally SameSite None/Lax/Strict; "#HttpOnly_" before the domain) to
 * the cookie store, on the main loop. The number queued; *domains: the
 * distinct domains, for the log. */
static unsigned pushCookies(const char* text, char** domains)
{
    GPtrArray* cookies = g_ptr_array_new_with_free_func((GDestroyNotify)soup_cookie_free);
    GString* seen = g_string_new(NULL);
    char** lines = g_strsplit(text, "\n", -1);
    for (char** line = lines; *line; line++) {
        g_strchomp(*line);
        gboolean httpOnly = g_str_has_prefix(*line, "#HttpOnly_");
        const char* entry = httpOnly ? *line + 10 : *line;
        if (!*entry || *entry == '#')
            continue;
        char** f = g_strsplit(entry, "\t", 9);
        guint n = g_strv_length(f);
        if (n >= 7 && *f[0] && *f[5]) {
            gint64 expires = g_ascii_strtoll(f[4], NULL, 10);
            gint64 now = g_get_real_time() / G_USEC_PER_SEC;
            if (!expires || expires > now) {
                SoupCookie* cookie = soup_cookie_new(f[5], f[6], f[0], *f[2] ? f[2] : "/", -1);
                soup_cookie_set_secure(cookie, !g_ascii_strcasecmp(f[3], "TRUE"));
                soup_cookie_set_http_only(cookie, httpOnly);
                if (expires) {
                    GDateTime* when = g_date_time_new_from_unix_utc(expires);
                    soup_cookie_set_expires(cookie, when);
                    g_date_time_unref(when);
                }
                if (n >= 8)
                    soup_cookie_set_same_site_policy(cookie, !g_ascii_strcasecmp(f[7], "Strict") ? SOUP_SAME_SITE_POLICY_STRICT
                        : !g_ascii_strcasecmp(f[7], "Lax") ? SOUP_SAME_SITE_POLICY_LAX : SOUP_SAME_SITE_POLICY_NONE);
                g_ptr_array_add(cookies, cookie);
                char* key = g_strdup_printf(" %s ", f[0]);
                if (!strstr(seen->str, key) && seen->len < 400)
                    g_string_append_printf(seen, "%s%s", seen->len ? "" : " ", key + 1);
                g_free(key);
            }
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    unsigned count = cookies->len;
    *domains = g_strstrip(g_string_free(seen, FALSE));
    if (count)
        g_idle_add_full(G_PRIORITY_DEFAULT, pushCookiesOnMain, cookies, (GDestroyNotify)g_ptr_array_unref);
    else
        g_ptr_array_unref(cookies);
    return count;
}

/* The start page address: a launch argument with "://" (tools/launch.sh
 * PPSA99020 <url>), else PS5WK_PENDING_URL (tools/ps5open puts long URLs
 * there before launching; read once, deleted, ignored after 10 minutes). */
static char* startURLFromLaunch(int argc, char** argv)
{
    /* Read and delete the file in any case, so that it can't open later. */
    gchar* contents = NULL;
    gboolean stale = FALSE;
    if (g_file_get_contents(PS5WK_PENDING_URL, &contents, NULL, NULL)) {
        struct stat st;
        stale = !stat(PS5WK_PENDING_URL, &st) && st.st_mtime > 1000000000 && time(NULL) - st.st_mtime > 600;
        unlink(PS5WK_PENDING_URL);
    }
    for (int i = 0; argv && i < argc; i++) {
        spike_log("launch argument %d: %.300s", i, argv[i] ? argv[i] : "(null)");
        if (argv[i] && strstr(argv[i], "://")) {
            const char* error = NULL;
            char* uri = pushNormalize(argv[i], &error);
            if (uri) {
                g_free(contents);
                return uri;
            }
            spike_log("launch argument %d refused: %s", i, error);
        }
    }
    if (!contents)
        return NULL;
    char* uri = NULL;
    const char* error = NULL;
    if (stale)
        spike_log("%s: older than 10 minutes, ignored", PS5WK_PENDING_URL);
    else if (!(uri = pushNormalize(contents, &error)))
        spike_log("%s refused: %s", PS5WK_PENDING_URL, error);
    else
        spike_log("start URL from %s", PS5WK_PENDING_URL);
    g_free(contents);
    return uri;
}

/* application/x-www-form-urlencoded value of key, or NULL. */
static char* formValue(const char* form, const char* key)
{
    size_t keyLength = strlen(key);
    for (const char* p = form; p && *p;) {
        size_t length = strcspn(p, "&");
        if (length > keyLength && !strncmp(p, key, keyLength) && p[keyLength] == '=') {
            char* raw = g_strndup(p + keyLength + 1, length - keyLength - 1);
            for (char* c = raw; *c; c++) {
                if (*c == '+')
                    *c = ' ';
            }
            char* value = g_uri_unescape_string(raw, NULL);
            g_free(raw);
            return value;
        }
        p += length;
        if (*p)
            p++;
    }
    return NULL;
}

static char* jsonString(const char* s)
{
    GString* out = g_string_new("\"");
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        if (*p == '"' || *p == '\\')
            g_string_append_printf(out, "\\%c", *p);
        else if (*p < 0x20)
            g_string_append_printf(out, "\\u%04x", *p);
        else
            g_string_append_c(out, (char)*p);
    }
    g_string_append_c(out, '"');
    return g_string_free(out, FALSE);
}

/* cors: let pages of any origin call it (not the settings: see settingsServe). */
static void pushReplyCORS(int client, int status, const char* type, const char* body, gboolean cors)
{
    const char* reason = status == 200 ? "OK" : status == 204 ? "No Content" : status == 400 ? "Bad Request"
        : status == 403 ? "Forbidden" : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed"
        : status == 408 ? "Request Timeout" : status == 413 ? "Payload Too Large" : "Service Unavailable";
    size_t length = body ? strlen(body) : 0;
    char* head = g_strdup_printf("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%s"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n", status, reason, type, length,
        cors ? "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, X-PS5-PIN, X-PS5-Key\r\nAccess-Control-Allow-Private-Network: true\r\n" : "");
    send(client, head, strlen(head), 0);
    if (length)
        send(client, body, length, 0);
    g_free(head);
}

static void pushReply(int client, int status, const char* type, const char* body)
{
    pushReplyCORS(client, status, type, body, TRUE);
}

/* Sends a file's bytes (the logo). */
static void pushReplyBytes(int client, const char* type, const char* data, size_t length)
{
    char* head = g_strdup_printf("HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: max-age=3600\r\nConnection: close\r\n\r\n", type, length);
    send(client, head, strlen(head), 0);
    for (size_t sent = 0; sent < length;) {
        ssize_t n = send(client, data + sent, length - sent, 0);
        if (n <= 0)
            break;
        sent += (size_t)n;
    }
    g_free(head);
}

/* This PS5's address on the LAN: the source address of a route out (a
 * UDP socket connected to a public address sends nothing), else the
 * address the last LAN request came to. Push thread. */
static char pushLastLocal[INET_ADDRSTRLEN];

static gboolean lanAddress(char* out, size_t size)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        struct sockaddr_in to, local;
        memset(&to, 0, sizeof to);
        to.sin_family = AF_INET;
        to.sin_port = htons(53);
        to.sin_addr.s_addr = htonl(0x08080808);
        socklen_t length = sizeof local;
        gboolean ok = !connect(fd, (struct sockaddr*)&to, sizeof to) && !getsockname(fd, (struct sockaddr*)&local, &length)
            && local.sin_addr.s_addr != htonl(INADDR_ANY) && local.sin_addr.s_addr != htonl(INADDR_LOOPBACK)
            && inet_ntop(AF_INET, &local.sin_addr, out, (socklen_t)size);
        close(fd);
        if (ok)
            return TRUE;
    }
    if (*pushLastLocal) {
        g_strlcpy(out, pushLastLocal, size);
        return TRUE;
    }
    return FALSE;
}

/* What a phone needs to reach this server, with its QR code (see GET /link.json). */
static char* linkJSON(void)
{
    GString* out = g_string_new(NULL);
    g_string_append_printf(out, "{\"localOnly\":%s,\"pin\":%s", pushLocalOnly ? "true" : "false", pushPIN ? "true" : "false");
    char ip[INET_ADDRSTRLEN];
    if (!pushLocalOnly && lanAddress(ip, sizeof ip)) {
        char url[64];
        snprintf(url, sizeof url, "http://%s:%d/", ip, PS5WK_PUSH_PORT);
        uint8_t modules[QR_MAX_SIZE * QR_MAX_SIZE];
        int size = qr_encode(url, strlen(url), modules);
        g_string_append_printf(out, ",\"url\":\"%s\",\"qr\":[", url);
        for (int y = 0; y < size; y++) {
            g_string_append(out, y ? ",\"" : "\"");
            for (int x = 0; x < size; x++)
                g_string_append_c(out, modules[y * size + x] ? '1' : '0');
            g_string_append_c(out, '"');
        }
        g_string_append_c(out, ']');
    }
    g_string_append_c(out, '}');
    return g_string_free(out, FALSE);
}

/* The link server's pages (phones and computers): Parallax's palette, a
 * light variant by day, the dark one at night (the device's setting). */
static const char pushPageStyle[] =
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><link rel=\"icon\" href=\"/logo.png\">"
    "<style>:root{color-scheme:light dark;--bg:#f7f5ff;--fg:#14113d;--muted:#5a5584;--card:#fff;--line:#dcd6f7;--accent:#6d0efd;"
    "--focus:#d10aa6;--glow:rgba(109,14,253,.12)}"
    "@media(prefers-color-scheme:dark){:root{--bg:#07061f;--fg:#f4f2ff;--muted:#b9b5e0;--card:#110f35;--line:#2b2766;--accent:#a77bff;"
    "--focus:#ff5cd6;--glow:rgba(109,14,253,.32)}}"
    "*{box-sizing:border-box}"
    "body{margin:0;min-height:100vh;color:var(--fg);font:17px/1.45 -apple-system,system-ui,\"Noto Sans\",sans-serif;"
    "background:radial-gradient(520px 320px at 100% -6%,var(--glow),transparent 70%) no-repeat,var(--bg)}"
    "main{max-width:34rem;margin:0 auto;padding:20px 16px 32px}"
    ".brand{display:flex;align-items:center;gap:12px;margin:4px 0 22px}.brand img{width:48px;height:48px;margin:-4px}"
    ".brand b{display:block;font-size:1.3rem;letter-spacing:-.01em;line-height:1.1}.brand span{color:var(--muted);font-size:.92rem}"
    "h1{font-size:1.35rem;margin:0 0 14px}h2{font-size:1.02rem;margin:26px 0 6px}"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:16px}"
    "form{display:flex;flex-direction:column;gap:10px}input{font:inherit;padding:13px 14px;border:1px solid var(--line);"
    "border-radius:12px;background:var(--bg);color:var(--fg);min-width:0}input:focus{outline:2px solid var(--focus);outline-offset:1px;border-color:transparent}"
    "button{font:inherit;font-weight:700;padding:13px;border:0;border-radius:12px;color:#fff;cursor:pointer;"
    "background:linear-gradient(90deg,#6d0efd,#a50ccf)}button:focus-visible{outline:2px solid var(--focus);outline-offset:2px}"
    "p,li{color:var(--muted)}code{word-break:break-all;color:var(--fg)}a{color:var(--accent)}"
    "a.bm{display:inline-block;padding:9px 16px;border:1.5px dashed var(--accent);border-radius:999px;color:var(--accent);"
    "text-decoration:none;font-weight:700}#status{color:var(--fg);margin:12px 0 0}#status:empty{display:none}"
    ".dls{padding:6px 16px}.dl{display:flex;flex-direction:column;gap:2px;padding:12px 0;border-top:1px solid var(--line)}.dl:first-child{border-top:0}"
    ".dlname{font-weight:700;color:var(--accent);word-break:break-all;text-decoration:none}.dlname:hover{text-decoration:underline}"
    ".dlmeta{color:var(--muted);font-size:.9rem}.dlsha{color:var(--muted);font:12px/1.4 ui-monospace,Menlo,monospace;word-break:break-all}</style>";

static const char pushBrand[] =
    "<div class=\"brand\"><img src=\"/logo.png\" alt=\"\" onerror=\"this.remove()\"><div><b>Parallax</b><span>Browser on your PS5</span></div></div>";

/* The page's Downloads section: the finished downloads (the list as JSON
 * in the page, drawn by its script with textContent), each a link to
 * /downloads/<name>. With a PIN and none given, a line that says so. */
static char* pushDownloadsSection(const char* pin, gboolean pinOK)
{
    if (!pinOK)
        return g_strdup("<h2>Downloads</h2><p>Open this page with <code>?pin=&lt;PIN&gt;</code> to see the files downloaded on the PS5.</p>");
    char* list = ps5wk_downloads_finished_json();
    /* inside a <script>: no "</" can end it early */
    GString* safe = g_string_new(NULL);
    for (const char* c = list; *c; c++) {
        if (c[0] == '<' && c[1] == '/')
            g_string_append(safe, "<\\/");
        else
            g_string_append_c(safe, *c);
    }
    char* pinJS = pin ? jsonString(pin) : g_strdup("null");
    char* section = g_strdup_printf("<h2>Downloads</h2><div class=\"card dls\" id=\"dls\"></div>"
        "<script>(function(){var list=%s,pin=%s,box=document.getElementById('dls');"
        "function size(b){return b>=1073741824?(b/1073741824).toFixed(2)+' GB':b>=1048576?(b/1048576).toFixed(1)+' MB':b>=1024?Math.round(b/1024)+' KB':b+' B'}"
        "if(!list.length){var p=document.createElement('p');p.textContent='Nothing yet. Files you download in the PS5 browser come here, to save on this device.';box.appendChild(p);return}"
        "list.forEach(function(d){var row=document.createElement('div');row.className='dl';"
        "var a=document.createElement('a');a.className='dlname';a.href='/downloads/'+encodeURIComponent(d.file)+(pin?'?pin='+encodeURIComponent(pin):'');"
        "a.setAttribute('download',d.file);a.textContent=d.file;row.appendChild(a);"
        "var m=document.createElement('span');m.className='dlmeta';m.textContent=size(d.size)+' \\u00b7 '+new Date(d.finished*1000).toLocaleString();row.appendChild(m);"
        "if(d.sha256){var h=document.createElement('code');h.className='dlsha';h.textContent='SHA-256 '+d.sha256;row.appendChild(h)}"
        "box.appendChild(row)})})()</script>", safe->str, pinJS);
    g_free(pinJS);
    g_string_free(safe, TRUE);
    g_free(list);
    return section;
}

/* The page with the text box; host: what the bookmarklet calls. */
static char* pushIndexPage(const char* host, const char* pin, gboolean pinOK)
{
    char* downloads = pushDownloadsSection(pin, pinOK);
    char* shown = copyShownURI();
    char* shownHTML = g_markup_escape_text(shown, -1);
    char* pinPart = pin ? g_strdup_printf("+'&pin=%s'", pin) : g_strdup("");
    char* bookmarklet = g_strdup_printf("javascript:(function(){var u='http://%s/open?url='+encodeURIComponent(location.href)%s;"
        "var w=window.open(u,'ps5open','width=420,height=260');if(!w)location.href=u;})()", host, pinPart);
    char* bookmarkletHTML = g_markup_escape_text(bookmarklet, -1);
    char* page = g_strdup_printf("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>Send to PS5</title>%s</head><body><main>%s"
        "<div class=\"card\"><h1>Send a link to the PS5</h1>"
        "<form id=\"f\" method=\"post\" action=\"/open\">"
        "<input name=\"url\" id=\"url\" placeholder=\"Address or search\" autocapitalize=\"off\" autocorrect=\"off\" spellcheck=\"false\" inputmode=\"url\" autofocus required>"
        "%s<button>Open on PS5</button></form><p id=\"status\"></p></div>"
        "<p>Showing now: <code>%s</code></p>"
        "<h2>Bookmarklet</h2><p>Drag this to the bookmarks bar, then click it on any page to open that page on the PS5:</p>"
        "<p><a class=\"bm\" href=\"%s\">Send to PS5</a></p>"
        "<p>From a terminal: <code>curl 'http://%s/open?url=example.com'</code></p>%s"
        "<script>document.getElementById('f').onsubmit=function(e){e.preventDefault();var s=document.getElementById('status');"
        "s.textContent='Sending…';fetch('/open',{method:'POST',headers:{'Accept':'application/json'},body:new URLSearchParams(new FormData(this))})"
        ".then(function(r){return r.json()}).then(function(j){s.textContent=j.ok?'Opening '+j.url:'Not sent: '+j.error})"
        ".catch(function(){s.textContent='The PS5 did not answer'})}</script></main></body></html>",
        pushPageStyle, pushBrand, pushPIN ? "<input name=\"pin\" placeholder=\"PIN\" inputmode=\"numeric\" autocomplete=\"off\">" : "",
        shownHTML, bookmarkletHTML, host, downloads);
    g_free(downloads);
    g_free(shown);
    g_free(shownHTML);
    g_free(pinPart);
    g_free(bookmarklet);
    g_free(bookmarkletHTML);
    return page;
}

/* The bookmarklet's pop-up (420x260) and the form's answer without script. */
static char* pushResultPage(const char* uri, const char* error)
{
    char* text = g_markup_escape_text(uri ? uri : error, -1);
    char* page = g_strdup_printf("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>%s</title>%s"
        "<style>main{padding-top:16px}.brand{margin-bottom:12px}h1{margin-bottom:6px}</style></head><body><main>%s"
        "<h1>%s</h1><p><code>%s</code></p><p><a href=\"/\">Send another link</a></p>"
        "<script>if(%s&&window.opener)setTimeout(function(){window.close()},1200)</script></main></body></html>",
        uri ? "Sent to PS5" : "Not sent", pushPageStyle, pushBrand, uri ? "Opening on the PS5" : "Not sent", text, uri ? "true" : "false");
    g_free(text);
    return page;
}

/* A header's value (case-insensitive name), copied into out. */
static gboolean headerValue(const char* headers, const char* name, char* out, size_t size)
{
    size_t nameLength = strlen(name);
    for (const char* line = headers; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        if (!g_ascii_strncasecmp(line, name, nameLength) && line[nameLength] == ':') {
            const char* value = line + nameLength + 1;
            while (*value == ' ' || *value == '\t')
                value++;
            size_t length = strcspn(value, "\r\n");
            if (length >= size)
                length = size - 1;
            memcpy(out, value, length);
            out[length] = 0;
            return TRUE;
        }
    }
    return FALSE;
}

/* ---- Settings (README: "Settings"; the table is in settings.c) ----
 *   GET /settings        the page (the PS5 opens it with R3: toggleSettings)
 *   GET /settings.json   every setting: value, default, the value in use,
 *       what applies it ("page", "browser", "none"), whether it waits for
 *       that restart; the page shown; the last action's result
 *   POST /settings       JSON {"jit":"dfg","sound":false} or form fields
 *       jit=dfg&sound=0 (ids, or the child.env/file names); replies with
 *       the settings and "message"
 *   POST /restart?scope=page|browser   page: a new WebProcess for the page
 *       shown (reads child.env); browser: the title starts again
 *   POST /restart?scope=crash   test only: SIGKILL to the WebProcess, which
 *       then looks like a crash to WebKit (the page reloads as after one)
 *   POST /clear?what=cookies|cache|all WebKit's website data
 *   POST /adblock?do=update            the ad blocker's lists, now (adblock.c)
 *   POST /adblock?do=allow|block&site=<host>   ads allowed on a site (and
 *       its subdomains) or blocked there again; the page reloads if it is
 *       on that site
 * The PIN applies as for /open, except to the PS5 itself (127.0.0.1). No
 * CORS headers, and requests with an Origin header must come from the
 * address they were sent to, by IP or a local name: other web pages
 * (a form posted from a site, DNS rebinding) can't change settings. */
#define PS5WK_TITLE_ID "PPSA99020"
#define PS5WK_LAUNCHER_PORT 9031

/* title_exit.c (titles only): a title ID there is launched again after the
 * SIGKILL that ends the title. */
extern char spike_title_relaunch[16] __attribute__((weak));

static gboolean isSettingsURI(const char* uri)
{
    return uri && g_str_has_prefix(uri, PS5WK_SETTINGS_URL);
}

/* Main loop: R3 opens Settings, and from there goes back to the page. */
static void toggleSettings(void)
{
    if (ps5wk_vr_active() || windDownStart)
        return;
    const char* current = webkit_web_view_get_uri(webView);
    if (isSettingsURI(current)) {
        spike_log("settings: R3, back to the page");
        if (webkit_web_view_can_go_back(webView))
            webkit_web_view_go_back(webView);
        else {
            char* home = homeURL();
            webkit_web_view_load_uri(webView, home);
            g_free(home);
        }
        return;
    }
    g_mutex_lock(&shownLock);
    g_free(settingsReturnURI);
    settingsReturnURI = g_strdup(current && *current ? current : "");
    g_mutex_unlock(&shownLock);
    spike_log("settings: R3, opening %s%s", PS5WK_SETTINGS_URL, pushLocalOnly ? " (link server on 127.0.0.1 only)" : "");
    loadStart = g_get_monotonic_time();
    webkit_web_view_load_uri(webView, PS5WK_SETTINGS_URL);
}

/* Every tab's view gets these (browserui.c makes the views of live tabs). */
void ps5wk_connect_web_view(WebKitWebView* view)
{
    g_signal_connect(webkit_web_view_get_wpe_view(view), "event", G_CALLBACK(onViewEvent), NULL);
    g_signal_connect(view, "load-changed", G_CALLBACK(onLoadChanged), NULL);
    g_signal_connect(view, "load-failed", G_CALLBACK(onLoadFailed), NULL);
    g_signal_connect(view, "web-process-terminated", G_CALLBACK(onWebProcessTerminated), NULL);
    g_signal_connect(view, "mouse-target-changed", G_CALLBACK(onMouseTargetChanged), NULL);
    ps5wk_adblock_connect_view(view); /* per-site cosmetic filters and scriptlets */
}

/* Another tab shown (browserui.c): everything here that acts on "the page"
 * follows its view. */
void ps5wk_set_active_web_view(WebKitWebView* view)
{
    webView = view;
    ps5wk_adblock_set_web_view(view);
    ps5wk_vr_set_web_view(view);
    setShownURI(webkit_web_view_get_uri(view));
}

/* The browser menu's L3/R3 and Settings links: Settings, unless shown. */
void ps5wk_open_settings(void)
{
    if (!isSettingsURI(webkit_web_view_get_uri(webView)))
        toggleSettings();
}

/* host: the Host header; origin: the Origin header or "". */
static gboolean settingsRequestAllowed(const char* host, const char* origin)
{
    if (!*host)
        return !*origin;
    char name[128];
    snprintf(name, sizeof name, "%s", host);
    name[strcspn(name, ":")] = 0;
    struct in_addr address;
    gboolean local = inet_pton(AF_INET, name, &address) == 1 || !g_ascii_strcasecmp(name, "localhost") || !strchr(name, '.');
    static const char* localSuffixes[] = { ".local", ".lan", ".home", ".internal", ".home.arpa" };
    for (unsigned i = 0; !local && i < G_N_ELEMENTS(localSuffixes); i++)
        local = g_str_has_suffix(name, localSuffixes[i]);
    if (!local)
        return FALSE;
    if (!*origin)
        return TRUE; /* curl, scripts */
    char expected[160];
    snprintf(expected, sizeof expected, "http://%s", host);
    return !g_ascii_strcasecmp(origin, expected);
}

static gboolean restartPageOnMain(gpointer data)
{
    (void)data;
    if (!webView || windDownStart || memRestartURI || memRestarting)
        return G_SOURCE_REMOVE;
    const char* uri = webkit_web_view_get_uri(webView);
    memRestartURI = uri && *uri && strcmp(uri, "about:blank") ? g_strdup(uri) : homeURL();
    settingsRestart = TRUE;
    memRestart(NULL);
    return G_SOURCE_REMOVE;
}

static void dataCleared(GObject* manager, GAsyncResult* result, gpointer data)
{
    const char* what = data;
    GError* error = NULL;
    ps5wk_profile_storage_changed();
    if (webkit_website_data_manager_clear_finish(WEBKIT_WEBSITE_DATA_MANAGER(manager), result, &error)) {
        spike_log("settings: %s cleared", what);
        ps5wk_settings_set_status("%s cleared.", what);
    } else {
        spike_log("settings: %s not cleared: %s", what, error ? error->message : "?");
        ps5wk_settings_set_status("%s not cleared: %s", what, error ? error->message : "unknown error");
        g_clear_error(&error);
    }
}

static gboolean clearDownloadsOnMain(gpointer data)
{
    (void)data;
    ps5wk_downloads_clear_finished();
    ps5wk_profile_storage_changed();
    ps5wk_settings_set_status("Finished downloads cleared.");
    return G_SOURCE_REMOVE;
}

enum { CLEAR_COOKIES, CLEAR_ALL, CLEAR_CACHE };

static gboolean clearDataOnMain(gpointer data)
{
    int what = GPOINTER_TO_INT(data);
    if (!networkSession || windDownStart)
        return G_SOURCE_REMOVE;
    static const WebKitWebsiteDataTypes types[] = { WEBKIT_WEBSITE_DATA_COOKIES, WEBKIT_WEBSITE_DATA_ALL,
        WEBKIT_WEBSITE_DATA_DISK_CACHE | WEBKIT_WEBSITE_DATA_MEMORY_CACHE };
    static const char* const names[] = { "Cookies", "All website data", "The webpage cache" };
    /* Timespan 0: everything, however old. */
    webkit_website_data_manager_clear(webkit_network_session_get_website_data_manager(networkSession),
        types[what], 0, NULL, dataCleared, (gpointer)names[what]);
    return G_SOURCE_REMOVE;
}

/* One command to the launcher payload; its reply line in reply. */
static gboolean launcherAsk(const char* command, char* reply, size_t size)
{
    reply[0] = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return FALSE;
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons(PS5WK_LAUNCHER_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timeval timeout = { .tv_sec = 3 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    ssize_t n = -1;
    if (!connect(fd, (struct sockaddr*)&address, sizeof address) && send(fd, command, strlen(command), 0) == (ssize_t)strlen(command))
        n = recv(fd, reply, size - 1, 0);
    close(fd);
    reply[n > 0 ? n : 0] = 0;
    reply[strcspn(reply, "\r\n")] = 0;
    return n > 0;
}

/* "Restart browser": the launcher payload (tools/launcher) starts titles.
 * It must be there and see this title running; then the page to come back
 * to goes in PS5WK_PENDING_URL, the title winds down as for "quit", and
 * title_exit.c sends the launcher "kill -9" and "launch PPSA99020" on one
 * connection (it reads the second line once the kill is done). NULL, or
 * why not. Never on another title: the launcher kills the running app,
 * which is this one. */
static const char* restartBrowser(const char* from)
{
    if (!spike_title_relaunch)
        return "only the PS5 title can restart itself";
    if (windDownStart)
        return "the browser is already closing";
    char reply[160];
    if (!launcherAsk("status\n", reply, sizeof reply))
        return "the launcher payload isn't running (tools/launch.sh sends it): close the browser and start it again from the home screen";
    if (strncmp(reply, "OK ", 3) || !strstr(reply, PS5WK_TITLE_ID)) {
        spike_log("settings: restart refused, launcher status \"%s\"", reply);
        return "the launcher doesn't see the browser running";
    }
    g_mutex_lock(&shownLock);
    const char* back = shownURI && !isSettingsURI(shownURI) ? shownURI : settingsReturnURI;
    const char* error = NULL;
    char* uri = back && *back ? pushNormalize(back, &error) : NULL;
    g_mutex_unlock(&shownLock);
    if (uri) {
        int fd = open(PS5WK_PENDING_URL, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            (void)!write(fd, uri, strlen(uri));
            close(fd);
        }
    }
    spike_log("settings: browser restart asked by %s; launcher says \"%s\"; coming back to %s", from, reply, uri ? uri : "the home page");
    g_free(uri);
    snprintf(spike_title_relaunch, sizeof spike_title_relaunch, "%s", PS5WK_TITLE_ID);
    g_main_context_invoke(NULL, beginWindDown, NULL);
    return NULL;
}

static void settingsReplyJSON(int client, int status, gboolean ok, const char* message)
{
    char* quoted = jsonString(message);
    char* reply = g_strdup_printf("{\"ok\":%s,\"%s\":%s}\n", ok ? "true" : "false", ok ? "message" : "error", quoted);
    pushReplyCORS(client, status, "application/json", reply, FALSE);
    g_free(reply);
    g_free(quoted);
}

static char* settingsState(const char* message)
{
    char* shown = copyShownURI();
    g_mutex_lock(&shownLock);
    char* back = g_strdup(settingsReturnURI ? settingsReturnURI : "");
    g_mutex_unlock(&shownLock);
    char* shownJSON = jsonString(shown);
    char* backJSON = jsonString(back);
    char* messageJSON = message ? jsonString(message) : NULL;
    /* The ad blocker's site buttons are for the page Settings was opened
     * from (R3), or the page shown (another device). */
    char* adblock = ps5wk_adblock_json(isSettingsURI(shown) ? back : shown);
    /* For the "Send from your phone" card (on the PS5 only). */
    char* link = linkJSON();
    char* downloads = ps5wk_downloads_summary_json();
    char* storage = ps5wk_profile_storage_json();
    char* extra = g_strdup_printf("\"started\":%lld,\"uri\":%s,\"returnUri\":%s,\"pin\":%s,\"localOnly\":%s,\"link\":%s,\"downloads\":%s,"
        "\"storage\":%s,%s%s%s%s",
        (long long)startedAt, shownJSON, backJSON, pushPIN ? "true" : "false", pushLocalOnly ? "true" : "false", link, downloads, storage,
        message ? "\"message\":" : "", message ? messageJSON : "", message ? "," : "", adblock);
    g_free(storage);
    g_free(downloads);
    g_free(link);
    g_free(adblock);
    char* state = ps5wk_settings_json(extra);
    g_free(extra);
    g_free(messageJSON);
    g_free(backJSON);
    g_free(shownJSON);
    g_free(back);
    g_free(shown);
    return state;
}

/* Push thread: the routes above. Replies at once: what needs the main loop
 * is queued there, and its result shows in /settings.json ("status"). */
static void settingsServe(int client, const char* from, gboolean loopback, gboolean get, gboolean post, const char* path,
    const char* query, const char* body, const char* type, const char* host, const char* origin, gboolean pinOK)
{
    if (!strcmp(path, "/settings") && get) {
        pushReplyCORS(client, 200, "text/html; charset=utf-8", ps5wk_settings_page, FALSE);
        return;
    }
    gboolean known = (!strcmp(path, "/settings.json") && get)
        || (post && (!strcmp(path, "/settings") || !strcmp(path, "/restart") || !strcmp(path, "/clear") || !strcmp(path, "/adblock")));
    if (!known) {
        settingsReplyJSON(client, !strcmp(path, "/settings.json") || !strcmp(path, "/restart") || !strcmp(path, "/clear")
            || !strcmp(path, "/settings") || !strcmp(path, "/adblock") ? 405 : 404, FALSE, "method not allowed or no such page");
        return;
    }
    if (!pinOK && !loopback) {
        settingsReplyJSON(client, 403, FALSE, "wrong or missing PIN");
        return;
    }
    if (!settingsRequestAllowed(host, origin)) {
        spike_log("settings: refused a request from %s (Host \"%s\", Origin \"%s\")", from, host, origin);
        settingsReplyJSON(client, 403, FALSE, "refused: open the settings at the PS5's IP address (http://<ip>:9040/settings)");
        return;
    }
    if (windDownStart && post) {
        settingsReplyJSON(client, 503, FALSE, "the browser is closing");
        return;
    }
    if (!strcmp(path, "/settings.json")) {
        char* state = settingsState(NULL);
        pushReplyCORS(client, 200, "application/json", state, FALSE);
        g_free(state);
    } else if (!strcmp(path, "/settings")) {
        gboolean json = strstr(type, "json") || *body == '{';
        char* message = NULL;
        int changed = 0;
        gboolean ok = ps5wk_settings_update(body, json, &message, &changed);
        spike_log("settings from %s: %s (%.300s)", from, message, body);
        ps5wk_adblock_setting_changed(); /* applies at once (no restart) */
        applyPerfOverlaySetting();
        if (ok || changed) {
            char* state = settingsState(message);
            pushReplyCORS(client, ok ? 200 : 503, "application/json", state, FALSE);
            g_free(state);
        } else
            settingsReplyJSON(client, 400, FALSE, message);
        g_free(message);
    } else if (!strcmp(path, "/adblock")) {
        char* action = query ? formValue(query, "do") : NULL;
        if (!action)
            action = formValue(body, "do");
        char* site = query ? formValue(query, "site") : NULL;
        if (!site)
            site = formValue(body, "site");
        const char* error = NULL;
        char* done = NULL;
        if (action && !strcmp(action, "update")) {
            error = ps5wk_adblock_update(from);
            done = g_strdup("Checking for a new list.");
        } else if (action && (!strcmp(action, "allow") || !strcmp(action, "block"))) {
            gboolean allow = !strcmp(action, "allow");
            error = site ? ps5wk_adblock_set_allowed(site, allow, TRUE) : "site=<host> missing";
            done = g_strdup_printf(allow ? "Ads allowed on %s." : "Ads blocked on %s again.", site ? site : "");
        } else
            error = "do=update, do=allow&site=<host> or do=block&site=<host>";
        spike_log("settings: adblock %s%s%s from %s: %s", action ? action : "?", site ? " " : "", site ? site : "", from, error ? error : "ok");
        if (error)
            settingsReplyJSON(client, 400, FALSE, error);
        else {
            char* state = settingsState(done);
            pushReplyCORS(client, 200, "application/json", state, FALSE);
            g_free(state);
        }
        g_free(done);
        g_free(action);
        g_free(site);
    } else {
        char* scope = query ? formValue(query, !strcmp(path, "/restart") ? "scope" : "what") : NULL;
        if (!scope)
            scope = formValue(body, !strcmp(path, "/restart") ? "scope" : "what");
        if (!strcmp(path, "/restart") && scope && (!strcmp(scope, "page") || !strcmp(scope, "web"))) {
            spike_log("settings: page restart asked by %s", from);
            ps5wk_settings_set_status("Restarting the page…");
            g_idle_add(restartPageOnMain, NULL);
            settingsReplyJSON(client, 200, TRUE, "Restarting the page.");
        } else if (!strcmp(path, "/restart") && scope && !strcmp(scope, "crash")) {
            /* Crash-recovery tests (COMPOSITING-PLAN.md, M6): the WebProcess
             * pid from its memory status file (written every 5 s). */
            int webPid = 0;
            double committed = 0, limit = 0;
            ps5wk_webmem_over(&webPid, &committed, &limit);
            int rc = webPid > 0 ? kill(webPid, SIGKILL) : -1;
            spike_log("settings: crash test asked by %s: kill(%d, SIGKILL) = %d (errno %d)", from, webPid, rc, rc ? errno : 0);
            settingsReplyJSON(client, rc ? 503 : 200, !rc, rc ? "No WebProcess to kill." : "WebProcess killed.");
        } else if (!strcmp(path, "/restart") && scope && !strcmp(scope, "browser")) {
            const char* error = restartBrowser(from);
            if (!error)
                ps5wk_settings_set_status("Restarting the browser…");
            settingsReplyJSON(client, error ? 503 : 200, !error, error ? error : "Restarting the browser.");
        } else if (!strcmp(path, "/clear") && scope && (!strcmp(scope, "cookies") || !strcmp(scope, "all") || !strcmp(scope, "cache"))) {
            int what = !strcmp(scope, "all") ? CLEAR_ALL : !strcmp(scope, "cache") ? CLEAR_CACHE : CLEAR_COOKIES;
            static const char* const names[] = { "cookies", "all website data", "the webpage cache" };
            spike_log("settings: clearing %s, asked by %s", names[what], from);
            g_idle_add(clearDataOnMain, GINT_TO_POINTER(what));
            settingsReplyJSON(client, 200, TRUE, what == CLEAR_ALL ? "Clearing all website data." : what == CLEAR_CACHE ? "Clearing the webpage cache." : "Clearing cookies.");
        } else if (!strcmp(path, "/clear") && scope && !strcmp(scope, "downloads")) {
            spike_log("settings: clearing finished downloads, asked by %s", from);
            g_idle_add(clearDownloadsOnMain, NULL);
            settingsReplyJSON(client, 200, TRUE, "Deleting the finished downloads.");
        } else
            settingsReplyJSON(client, 400, FALSE, !strcmp(path, "/restart") ? "scope=page or scope=browser" : "what=cookies, what=cache, what=all or what=downloads");
        g_free(scope);
    }
}

/* ---- Finished downloads to another device (GET /downloads/<name>) ----
 * A thread per transfer (at most 4), so a 2 GB file to a phone doesn't stop
 * the link server (Settings, /open) for minutes. Range: bytes=a-b, a- and
 * -n (one range); 416 outside the file. */
#define PS5WK_FILE_SENDERS 4
static gint fileSenders;

typedef struct {
    int client;
    int fd;
    gint64 size, start, length;
    gboolean partial, head;
    char* name;
    char* from;
} FileSend;

static void* fileSendThread(void* data)
{
    FileSend* job = data;
    struct timeval timeout = { .tv_sec = 30 };
    setsockopt(job->client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    char* escaped = g_uri_escape_string(job->name, NULL, FALSE);
    char* range = job->partial ? g_strdup_printf("Content-Range: bytes %" G_GINT64_FORMAT "-%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT "\r\n", job->start,
        job->start + job->length - 1, job->size) : g_strdup("");
    char* head = g_strdup_printf("HTTP/1.1 %s\r\nContent-Type: application/octet-stream\r\nContent-Length: %" G_GINT64_FORMAT "\r\n%s"
        "Accept-Ranges: bytes\r\nContent-Disposition: attachment; filename*=UTF-8''%s\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
        job->partial ? "206 Partial Content" : "200 OK", job->length, range, escaped);
    g_free(range);
    gboolean ok = send(job->client, head, strlen(head), 0) == (ssize_t)strlen(head);
    gint64 sent = 0, startTime = g_get_monotonic_time();
    if (ok && !job->head && lseek(job->fd, job->start, SEEK_SET) == job->start) {
        const size_t size = 256 * 1024;
        char* buffer = g_malloc(size);
        while (sent < job->length) {
            ssize_t n = read(job->fd, buffer, (size_t)MIN((gint64)size, job->length - sent));
            if (n <= 0)
                break;
            ssize_t done = 0;
            while (done < n) {
                ssize_t w = send(job->client, buffer + done, (size_t)(n - done), 0);
                if (w <= 0)
                    break;
                done += w;
            }
            if (done < n)
                break;
            sent += n;
        }
        g_free(buffer);
    }
    double seconds = (g_get_monotonic_time() - startTime) / 1e6;
    spike_log("push: %s to %s: %" G_GINT64_FORMAT " of %" G_GINT64_FORMAT " bytes%s in %.1f s (%.1f MB/s)", job->name, job->from, sent, job->head ? 0 : job->length,
        job->partial ? " (a range)" : "", seconds, seconds > 0 ? sent / 1048576.0 / seconds : 0);
    g_free(head);
    g_free(escaped);
    close(job->fd);
    close(job->client);
    g_free(job->name);
    g_free(job->from);
    g_free(job);
    g_atomic_int_add(&fileSenders, -1);
    return NULL;
}

/* "bytes=a-b", "bytes=a-", "bytes=-n": the range in a file of size bytes;
 * FALSE when it can't be served (416). */
static gboolean parseByteRange(const char* value, gint64 size, gint64* start, gint64* length)
{
    if (g_ascii_strncasecmp(value, "bytes=", 6) || strchr(value, ','))
        return FALSE;
    const char* p = value + 6;
    char* end = NULL;
    if (*p == '-') {
        gint64 n = g_ascii_strtoll(p + 1, &end, 10);
        if (end == p + 1 || n <= 0)
            return FALSE;
        *start = n >= size ? 0 : size - n;
    } else {
        *start = g_ascii_strtoll(p, &end, 10);
        if (end == p || *end != '-' || *start >= size)
            return FALSE;
        p = end + 1;
        if (*p) {
            gint64 last = g_ascii_strtoll(p, &end, 10);
            if (end == p || last < *start)
                return FALSE;
            *length = MIN(last, size - 1) - *start + 1;
            return TRUE;
        }
    }
    *length = size - *start;
    return *length > 0;
}

/* TRUE: the client socket was handed to a sending thread. */
static gboolean serveDownloadFile(int client, const char* from, const char* encodedName, const char* rangeHeader, gboolean head)
{
    char* name = g_uri_unescape_string(encodedName, NULL);
    char* path = name ? ps5wk_downloads_finished_path(name) : NULL;
    int fd = path ? open(path, O_RDONLY | O_NOFOLLOW) : -1;
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        spike_log("push: %s asked for /downloads/%s: no such finished download", from, encodedName);
        pushReply(client, 404, "text/plain", "ERR no such finished download\n");
        if (fd >= 0)
            close(fd);
        g_free(name);
        g_free(path);
        return FALSE;
    }
    FileSend* job = g_new0(FileSend, 1);
    job->client = client;
    job->fd = fd;
    job->size = st.st_size;
    job->start = 0;
    job->length = st.st_size;
    job->head = head;
    job->name = name;
    job->from = g_strdup(from);
    g_free(path);
    if (rangeHeader && *rangeHeader) {
        if (!parseByteRange(rangeHeader, job->size, &job->start, &job->length)) {
            char* reply = g_strdup_printf("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%" G_GINT64_FORMAT "\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n", job->size);
            send(client, reply, strlen(reply), 0);
            g_free(reply);
            close(fd);
            g_free(job->name);
            g_free(job->from);
            g_free(job);
            return FALSE;
        }
        job->partial = TRUE;
    }
    if (g_atomic_int_add(&fileSenders, 1) >= PS5WK_FILE_SENDERS) {
        g_atomic_int_add(&fileSenders, -1);
        pushReply(client, 503, "text/plain", "ERR too many downloads at once; try again\n");
        close(fd);
        g_free(job->name);
        g_free(job->from);
        g_free(job);
        return FALSE;
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, fileSendThread, job)) {
        g_atomic_int_add(&fileSenders, -1);
        pushReply(client, 503, "text/plain", "ERR no thread\n");
        close(fd);
        g_free(job->name);
        g_free(job->from);
        g_free(job);
        return FALSE;
    }
    pthread_detach(thread);
    return TRUE;
}

/* TRUE: the client socket is someone else's now (a file transfer). */
static gboolean pushServe(int client, const char* from, gboolean loopback)
{
    char* request = g_malloc(PS5WK_PUSH_MAX + 1);
    size_t used = 0, headerEnd = 0, contentLength = 0;
    gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
    int status = 0;
    for (;;) {
        if (headerEnd && used >= headerEnd + contentLength)
            break;
        if (used >= PS5WK_PUSH_MAX) {
            status = 413;
            break;
        }
        int wait = (int)((deadline - g_get_monotonic_time()) / 1000);
        struct pollfd p = { .fd = client, .events = POLLIN };
        if (wait <= 0 || poll(&p, 1, wait) <= 0) {
            status = 408;
            break;
        }
        ssize_t n = recv(client, request + used, PS5WK_PUSH_MAX - used, 0);
        if (n <= 0) {
            status = 400;
            break;
        }
        used += (size_t)n;
        request[used] = 0;
        if (!headerEnd) {
            char* end = strstr(request, "\r\n\r\n");
            if (end) {
                headerEnd = (size_t)(end - request) + 4;
                char value[32];
                if (headerValue(request, "Content-Length", value, sizeof value))
                    contentLength = strtoul(value, NULL, 10);
                if (contentLength > PS5WK_PUSH_MAX - headerEnd) {
                    status = 413;
                    break;
                }
            }
        }
    }
    if (status) {
        if (status != 400 || used)
            pushReply(client, status, "text/plain", status == 408 ? "ERR request too slow\n" : status == 413 ? "ERR request too large\n" : "ERR bad request\n");
        g_free(request);
        return FALSE;
    }
    request[headerEnd - 2] = 0; /* headers end with one \r\n */
    const char* body = request + headerEnd;
    request[headerEnd + contentLength] = 0;

    char method[8] = "", target[2048] = "";
    if (sscanf(request, "%7s %2047s", method, target) != 2) {
        pushReply(client, 400, "text/plain", "ERR bad request\n");
        g_free(request);
        return FALSE;
    }
    const char* headers = strchr(request, '\n') ? strchr(request, '\n') + 1 : "";
    char accept[256] = "", type[128] = "", host[128] = "", pinHeader[64] = "", keyHeader[128] = "", origin[160] = "";
    headerValue(headers, "Accept", accept, sizeof accept);
    headerValue(headers, "Origin", origin, sizeof origin);
    headerValue(headers, "X-PS5-Key", keyHeader, sizeof keyHeader);
    headerValue(headers, "Content-Type", type, sizeof type);
    headerValue(headers, "Host", host, sizeof host);
    headerValue(headers, "X-PS5-PIN", pinHeader, sizeof pinHeader);
    char rangeHeader[128] = "";
    headerValue(headers, "Range", rangeHeader, sizeof rangeHeader);
    char* query = strchr(target, '?');
    if (query)
        *query++ = 0;
    const char* path = target;
    gboolean settingsPath = !strcmp(path, "/settings") || !strcmp(path, "/settings.json") || !strcmp(path, "/restart") || !strcmp(path, "/clear")
        || !strcmp(path, "/adblock");
    gboolean browserPath = !strcmp(path, "/browser") || g_str_has_prefix(path, "/browser/");

    if (!strcmp(method, "OPTIONS")) {
        /* No CORS for the settings: a page elsewhere can't send them JSON. */
        pushReplyCORS(client, 204, "text/plain", NULL, !settingsPath && !browserPath);
        g_free(request);
        return FALSE;
    }
    gboolean get = !strcmp(method, "GET") || !strcmp(method, "HEAD");
    gboolean post = !strcmp(method, "POST");
    char* url = query ? formValue(query, "url") : NULL;
    char* pin = query ? formValue(query, "pin") : NULL;
    gboolean form = post && (strstr(type, "application/x-www-form-urlencoded") || !strncmp(body, "url=", 4));
    if (post && !url && form)
        url = formValue(body, "url");
    if (post && !url) /* also curl -d <url>, which says form-urlencoded */
        url = g_strdup(body);
    if (post && form && !pin)
        pin = formValue(body, "pin");
    if (!pin && *pinHeader)
        pin = g_strdup(pinHeader);
    gboolean pinOK = !pushPIN || (pin && !strcmp(pin, pushPIN));
    gboolean html = strstr(accept, "text/html") != NULL;
    gboolean json = strstr(accept, "application/json") || (query && strstr(query, "format=json"));

    if (settingsPath) {
        settingsServe(client, from, loopback, get, post, path, query, body, type, host, origin, pinOK);
    } else if (browserPath) {
        /* Tabs, bookmarks, history and simulated menu keys (browserui.c):
         * the settings' rules (no CORS, same Host and Origin, the PIN). */
        if (!pinOK && !loopback)
            settingsReplyJSON(client, 403, FALSE, "wrong or missing PIN");
        else if (!settingsRequestAllowed(host, origin)) {
            spike_log("browser: refused a request from %s (Host \"%s\", Origin \"%s\")", from, host, origin);
            settingsReplyJSON(client, 403, FALSE, "refused");
        } else if (windDownStart)
            settingsReplyJSON(client, 503, FALSE, "the browser is closing");
        else {
            int code = 200;
            char* reply = ps5wk_browser_api(method, path, query, body, &code);
            if (post)
                spike_log("browser API from %s: %s %s%s%s", from, method, path, query ? "?" : "", query ? query : "");
            pushReplyCORS(client, code, "application/json", reply, FALSE);
            g_free(reply);
        }
    } else if (g_str_has_prefix(path, "/downloads/") && get) {
        /* A finished download: the PIN as for /open (not for the PS5 itself). */
        if (!pinOK && !loopback)
            pushReply(client, 403, "text/plain", "ERR wrong or missing PIN\n");
        else if (serveDownloadFile(client, from, path + strlen("/downloads/"), rangeHeader, !strcmp(method, "HEAD"))) {
            g_free(url);
            g_free(pin);
            g_free(request);
            return TRUE;
        }
    } else if (!strcmp(path, "/downloads.json") && get) {
        if (!pinOK && !loopback)
            pushReply(client, 403, "application/json", "{\"ok\":false,\"error\":\"wrong or missing PIN\"}\n");
        else {
            char* list = ps5wk_downloads_finished_json();
            char* reply = g_strdup_printf("{\"ok\":true,\"downloads\":%s}\n", list);
            pushReply(client, 200, "application/json", reply);
            g_free(reply);
            g_free(list);
        }
    } else if (!strcmp(path, "/status") && get) {
        char* shown = copyShownURI();
        char* quoted = jsonString(shown);
        char* reply = g_strdup_printf("{\"ok\":true,\"uri\":%s}\n", quoted);
        pushReply(client, 200, "application/json", reply);
        g_free(reply);
        g_free(quoted);
        g_free(shown);
    } else if (!strcmp(path, "/cookies") && post) {
        /* Open by default (home network); with a key file, requests must
         * carry it in X-PS5-Key (not the PIN: cookies log into accounts). */
        gchar* key = NULL;
        if (g_file_get_contents(PS5WK_COOKIE_KEY, &key, NULL, NULL))
            g_strstrip(key);
        if (key && *key && strcmp(key, keyHeader)) {
            spike_log("push from %s: cookies refused (wrong key)", from);
            pushReply(client, 403, "text/plain", "ERR wrong or missing cookie key (X-PS5-Key)\n");
        } else if (windDownStart || !cookieManager) {
            pushReply(client, 503, "text/plain", "ERR the browser is closing\n");
        } else {
            char* domains = NULL;
            unsigned count = pushCookies(body, &domains);
            spike_log("push from %s: %u cookies for %s", from, count, *domains ? domains : "(none)");
            char* reply = g_strdup_printf("OK %u cookies added\n", count);
            pushReply(client, count ? 200 : 400, "text/plain", count ? reply : "ERR no usable cookies\n");
            g_free(reply);
            g_free(domains);
        }
        g_free(key);
    } else if ((!strcmp(path, "/open") && (get || post)) || (!strcmp(path, "/") && ((get && url) || post))) {
        char* uri = NULL;
        const char* error = !pinOK ? "wrong or missing PIN" : NULL;
        if (!error)
            error = pushRequest(url, from, &uri);
        int code = !error ? 200 : !pinOK ? 403 : windDownStart ? 503 : 400;
        char* reply;
        if (json && !html) {
            char* quoted = jsonString(uri ? uri : error);
            reply = g_strdup_printf("{\"ok\":%s,\"%s\":%s}\n", uri ? "true" : "false", uri ? "url" : "error", quoted);
            g_free(quoted);
        } else if (html) {
            reply = pushResultPage(uri, error);
        } else {
            reply = g_strdup_printf("%s %s\n", uri ? "OK loading" : "ERR", uri ? uri : error);
        }
        pushReply(client, code, json && !html ? "application/json" : html ? "text/html; charset=utf-8" : "text/plain; charset=utf-8", reply);
        g_free(reply);
        g_free(uri);
    } else if (!strcmp(path, "/link.json") && get && loopback) {
        /* The start page (a file: page: CORS) and Settings on the PS5. */
        char* reply = linkJSON();
        pushReply(client, 200, "application/json", reply);
        g_free(reply);
    } else if (!strcmp(path, "/logo.png") && get) {
        gchar* data = NULL;
        gsize length = 0;
        if (g_file_get_contents("/app0/ui/parallax-logo.png", &data, &length, NULL)
            || g_file_get_contents("/data/ps5webkit/parallax-logo.png", &data, &length, NULL))
            pushReplyBytes(client, "image/png", data, length);
        else
            pushReply(client, 404, "text/plain", "ERR no logo\n");
        g_free(data);
    } else if ((!strcmp(path, "/") || !strcmp(path, "/index.html")) && get) {
        /* The bookmarklet calls the address this page was reached by. */
        gboolean hostOK = *host != 0;
        for (const char* c = host; *c; c++) {
            if (!g_ascii_isalnum(*c) && *c != '.' && *c != ':' && *c != '-')
                hostOK = FALSE;
        }
        if (!hostOK) {
            struct sockaddr_in local;
            socklen_t length = sizeof local;
            char ip[INET_ADDRSTRLEN] = "0.0.0.0";
            if (!getsockname(client, (struct sockaddr*)&local, &length))
                inet_ntop(AF_INET, &local.sin_addr, ip, sizeof ip);
            snprintf(host, sizeof host, "%s:%d", ip, PS5WK_PUSH_PORT);
        }
        char* page = pushIndexPage(host, pushPIN && pinOK ? pushPIN : NULL, pinOK);
        pushReply(client, 200, "text/html; charset=utf-8", page);
        g_free(page);
    } else if (!strcmp(path, "/") || !strcmp(path, "/open") || !strcmp(path, "/status") || !strcmp(path, "/cookies")) {
        pushReply(client, 405, "text/plain", "ERR method not allowed\n");
    } else {
        pushReply(client, 404, "text/plain", "ERR not found (/, /open?url=..., /status, /settings)\n");
    }
    g_free(url);
    g_free(pin);
    g_free(request);
    return FALSE;
}

static void* pushThread(void* unused)
{
    (void)unused;
    int fd = -1;
    for (unsigned attempt = 0; fd < 0; attempt++) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        struct sockaddr_in address;
        memset(&address, 0, sizeof address);
        address.sin_family = AF_INET;
        address.sin_port = htons(PS5WK_PUSH_PORT);
        address.sin_addr.s_addr = htonl(pushLocalOnly ? INADDR_LOOPBACK : INADDR_ANY);
        if (fd >= 0)
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (fd >= 0 && !bind(fd, (struct sockaddr*)&address, sizeof address) && !listen(fd, 8))
            break;
        int err = errno;
        if (attempt % 30 == 0)
            spike_log("push: port %d: %s (attempt %u; trying again every 2 s)", PS5WK_PUSH_PORT, strerror(err), attempt + 1);
        if (fd >= 0)
            close(fd);
        fd = -1;
        sleep(2);
    }
    if (pushLocalOnly)
        spike_log("push: listening on 127.0.0.1:%d only (push-off: Settings with R3, nothing from the LAN)", PS5WK_PUSH_PORT);
    else
        spike_log("push: listening on TCP %d (http://<this PS5>:%d/)%s", PS5WK_PUSH_PORT, PS5WK_PUSH_PORT, pushPIN ? ", PIN required" : "");
    for (;;) {
        struct sockaddr_in peer;
        socklen_t peerLength = sizeof peer;
        int client = accept(fd, (struct sockaddr*)&peer, &peerLength);
        if (client < 0) {
            usleep(100 * 1000);
            continue;
        }
        struct timeval timeout = { .tv_sec = 3 };
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
        char from[INET_ADDRSTRLEN + 8] = "http ?";
        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip))
            snprintf(from, sizeof from, "http %s", ip);
        if (peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
            /* the address this request came to: this PS5's on the LAN */
            struct sockaddr_in local;
            socklen_t localLength = sizeof local;
            if (!getsockname(client, (struct sockaddr*)&local, &localLength))
                inet_ntop(AF_INET, &local.sin_addr, pushLastLocal, sizeof pushLastLocal);
        }
        if (!pushServe(client, from, peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK)))
            close(client);
    }
    return NULL;
}

static void pushStart(void)
{
    if (fileExists("/data/ps5webkit/push-off")) {
        spike_log("push: /data/ps5webkit/push-off: 127.0.0.1 only");
        pushLocalOnly = TRUE;
    }
    gchar* pin = NULL;
    if (g_file_get_contents("/data/ps5webkit/push-pin.txt", &pin, NULL, NULL)) {
        g_strstrip(pin);
        if (*pin)
            pushPIN = pin;
        else
            g_free(pin);
    }
    pthread_t thread;
    if (!pthread_create(&thread, NULL, pushThread, NULL))
        pthread_detach(thread);
    else
        spike_log("push: no thread");
}

/* Titles differ from payloads in which socket calls work (Spike 8): log
 * what WebKit's IPC needs (socketpair types, close-on-exec, pipe2). */
int __real_fcntl(int, int, ...) __attribute__((weak));
static void probeSockets(void)
{
    if (!__real_fcntl)
        return;
    static const struct { int type; const char* name; } types[] = {
        { SOCK_STREAM, "stream" }, { SOCK_SEQPACKET, "seqpacket" }, { SOCK_DGRAM, "dgram" },
    };
    char line[1024];
    int n = 0;
    for (unsigned i = 0; i < sizeof types / sizeof types[0]; i++) {
        int fds[2] = { -1, -1 };
        errno = 0;
        int rc = socketpair(AF_UNIX, types[i].type, 0, fds);
        int err = errno;
        int cloexec = -2;
        if (!rc) {
            int fl = __real_fcntl(fds[0], F_GETFL);
            int fle = errno;
            int sfl = __real_fcntl(fds[0], F_SETFL, O_NONBLOCK | (fl > 0 ? fl : 0));
            int sfle = errno;
            int one = 1;
            int nb = ioctl(fds[1], FIONBIO, &one);
            char byte;
            errno = 0;
            int dw = (int)recv(fds[1], &byte, 1, MSG_DONTWAIT);
            n += snprintf(line + n, sizeof line - n, "[recv MSG_DONTWAIT on empty: %d/%d] ", dw, errno);
            n += snprintf(line + n, sizeof line - n, "[%s: F_GETFL %d/%d F_SETFL %d/%d FIONBIO %d] ", types[i].name, fl, fle, sfl, sfle, nb);
            cloexec = fcntl(fds[0], F_SETFD, FD_CLOEXEC);
            close(fds[0]);
            close(fds[1]);
        }
        n += snprintf(line + n, sizeof line - n, "socketpair %s rc %d errno %d (F_SETFD %d); ", types[i].name, rc, err, cloexec);
    }
    int p[2];
    errno = 0;
    int rc = pipe(p);
    n += snprintf(line + n, sizeof line - n, "pipe rc %d errno %d", rc, errno);
    spike_log("probe: %s", line);
    if (!rc) {
        n = 0;
#define PROBE(expr) do { errno = 0; int r_ = (expr); n += snprintf(line + n, sizeof line - n, "%s = %d (errno %d); ", #expr, r_, errno); } while (0)
        PROBE(fcntl(p[0], F_GETFD));
        PROBE(fcntl(p[0], F_SETFD, FD_CLOEXEC));
        PROBE(fcntl(p[0], F_GETFL));
        PROBE(fcntl(p[0], F_SETFL, O_NONBLOCK));
        PROBE(fcntl(p[0], F_GETFL));
        int one = 1;
        PROBE(ioctl(p[1], FIONBIO, &one));
        int d = fcntl(p[0], F_DUPFD, 3);
        n += snprintf(line + n, sizeof line - n, "F_DUPFD -> %d; ", d);
        if (d >= 0)
            close(d);
        close(p[0]);
        close(p[1]);
        int q[2];
        PROBE(d = dup(q[0] = 0));
        if (d > 0)
            close(d);
        PROBE(pipe2(q, O_CLOEXEC));
        if (!pipe2(q, O_NONBLOCK)) {
            close(q[0]);
            close(q[1]);
        }
#undef PROBE
        spike_log("probe: %s", line);
    }
}

/* open(), not access(): access() reports existing files as missing in a title. */
static int fileExists(const char* path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

/* titlevdec.c: hardware video decoding (Videodec2) for the WebProcesses. */
void ps5wk_title_vdec_preload(void) __attribute__((weak));
void ps5wk_title_vdec_install(void) __attribute__((weak));
/* titlelog.c: the log queue no thread waits on (CONSOLE-HEALTH.md, "Logging"). */
void ps5wk_titlelog_install(const char* dir);

/* The sampling profiler in the title (apps/common/profiler.c; WebGL's
 * ANGLE and GL driver run here, spikes/11-webgl M4): on while
 * /data/ps5webkit/title-profile exists, its first line the PS5WK_PROFILE
 * mode (empty: 1, CPU time per thread). Profiles go to
 * /data/ps5webkit/prof/title-<pid>.prof (test/prof/prof.py). Every thread
 * made through titlemem.c's pthread_create wrapper is in its table, those
 * made before this call too; threads with their own stacks (libc's path)
 * and the system libraries' own threads are not sampled. */
void ps5wk_profiler_start(const char* tag) __attribute__((weak));
static void startTitleProfiler(void)
{
    if (!ps5wk_profiler_start)
        return;
    FILE* file = fopen("/data/ps5webkit/title-profile", "r");
    if (!file)
        return;
    char mode[32] = "";
    if (!fgets(mode, sizeof mode, file))
        mode[0] = 0;
    fclose(file);
    mode[strcspn(mode, " \t\r\n")] = 0;
    setenv("PS5WK_PROFILE", mode[0] ? mode : "1", 1);
    ps5wk_profiler_start("title");
    spike_log("[prof] title profiler started, mode %s (/data/ps5webkit/title-profile)", mode[0] ? mode : "1");
}

int main(int argc, char** argv)
{
    /* Before the unjail in spike_title_init (PS5VR: libSceVideodec2 does
     * not load after it). */
    if (ps5wk_title_vdec_preload)
        ps5wk_title_vdec_preload();
    ps5wk_titlelog_install("/data/ps5webkit");
    spike_title_init("ps5browser", LOG_HOST, LOG_PORT);
    loadStart = g_get_monotonic_time();
    startedAt = g_get_real_time() / G_USEC_PER_SEC;
    signal(SIGPIPE, SIG_IGN);
    // WebKit, GLib and the child payloads (relayed) log to stderr: titlelog.c
    // made it a pipe into its queue. Only without it (spike_log_fd: the old
    // blocking connection) is stderr pointed at the log socket.
    if (spike_log_fd >= 0 && dup2(spike_log_fd, 2) != 2) {
        /* A title can't dup2 (EPERM; fd 2 can't be reused either): point
         * libc's stderr stream at the log socket instead. */
        int err = errno;
        FILE* log = fdopen(spike_log_fd, "w");
        if (log)
            stderr = log;
        spike_log("dup2(log, stderr) failed (errno %d): stderr is %s", err, log ? "an fdopen() stream on the log socket" : "lost");
    }
    setvbuf(stderr, NULL, _IOLBF, 0);
    g_log_set_default_handler(logHandler, NULL);
    spike_log("ps5browser: WPE WebKit %u.%u.%u, pid %d, build %s", webkit_get_major_version(), webkit_get_minor_version(),
        webkit_get_micro_version(), (int)getpid(), ps5wk_title_build_id ? ps5wk_title_build_id() : "(payload)");
    spike_notify("Parallax Browser starting");
    memLog("main");
    startTitleProfiler();
    if (ps5wk_title_check_imports) {
        static char names[1024];
        unsigned total = 0;
        unsigned missing = ps5wk_title_check_imports(names, sizeof names, &total);
        spike_log("imports: %u of %u NULL%s%s", missing, total, missing ? ": " : "", names);
    }
    probeSockets();
    if (ps5wk_title_vdec_install)
        ps5wk_title_vdec_install();

    mkdir("/data/ps5webkit", 0777);
    mkdir("/data/ps5webkit/home", 0777);
    setenv("HOME", "/data/ps5webkit/home", 0);
    setenv("XDG_RUNTIME_DIR", "/data/ps5webkit/home", 0);
    setenv("GIO_USE_VFS", "local", 0);
    setenv("ICU_DATA", "/data/ps5webkit/share/icu", 0);
    setenv("XDG_DATA_DIRS", "/data/ps5webkit/share", 0); /* share/mime/globs2 */
    setenv("GSETTINGS_BACKEND", "memory", 0);
    /* Temporary files (g_get_tmp_dir; WebKit's content filter compiler
     * writes its NFAs there): there is no /tmp for a title. */
    mkdir("/data/ps5webkit/home/tmp", 0777);
    setenv("TMPDIR", "/data/ps5webkit/home/tmp", 0);
    g_io_openssl_load(NULL);
    /* JavaScriptCore in this process runs the ad blocker's list converter
     * (adblock.c) and nothing else: no JIT in a title. Read when WebKit
     * initializes JSC (the web context below); the WebProcesses do not see
     * this environment. */
    setenv("JSC_useJIT", "false", 0);
    /* Whose data: before anything reads it (README: "Users and storage"). */
    ps5wk_profile_init();
    /* The values in use until something restarts (the settings page). */
    ps5wk_settings_init("/data/ps5webkit", ps5wk_profile_dir(), pushNormalize);

    /* open(), not access(): access() said no for an existing file here. */
    int flag = open("/data/ps5webkit/debug-screenshot", O_RDONLY);
    if (flag >= 0)
        close(flag);
    screenshots = flag >= 0 || g_getenv("PS5WK_SCREENSHOT");
    if (flag >= 0) {
        gchar* knob = NULL;
        if (g_file_get_contents("/data/ps5webkit/debug-screenshot", &knob, NULL, NULL) && strstr(knob, "series=")
            && sscanf(strstr(knob, "series=") + 7, "%ux%u", &seriesInterval, &seriesCount) == 2)
            spike_log("debug screenshots: a series every %u ms, %u per test URL", seriesInterval, seriesCount);
        g_free(knob);
    }
    if (screenshots) {
        mkdir(SHOT_DIR, 0777);
        GDir* dir = g_dir_open(SHOT_DIR, 0, NULL);
        for (const char* name; dir && (name = g_dir_read_name(dir));) {
            char* path = g_build_filename(SHOT_DIR, name, NULL);
            unlink(path);
            g_free(path);
        }
        if (dir)
            g_dir_close(dir);
        setenv("PS5WK_SCREENSHOT_DIR", SHOT_DIR, 1);
        spike_log("debug screenshots on: %s", SHOT_DIR);
    }

    GError* error = NULL;
    display = wpe_display_ps5_new();
    wpe_display_ps5_set_child_elf(WPE_DISPLAY_PS5(display), "/app0/ps5wk-child.elf");
    if (!wpe_display_connect(display, &error)) {
        SPIKE_FAIL("display", "%s", error ? error->message : "?");
        return 1;
    }
    wpe_display_set_primary(display);
    SPIKE_PASS("display", "PS5 display connected");
    memLog("display connected");
    /* The WebProcesses' audio (ps5audiosink) is played here with sceAudioOut
     * (apps/common/ps5audioserver.c). /data/ps5webkit/audio-null (a file):
     * consume it silently instead, for log-only runs. */
    /* Log-only runs: /data/ps5webkit/audio-null (no sceAudioOut) or
     * /data/ps5webkit/audio-silent (sceAudioOut at volume 0). */
    int audioMode = fileExists("/data/ps5webkit/audio-null") ? PS5WK_AUDIO_NULL
        : fileExists("/data/ps5webkit/audio-silent") ? PS5WK_AUDIO_SILENT : PS5WK_AUDIO_SCE;
    ps5wk_audio_server_start(audioMode);
    int selftest = fileExists("/data/ps5webkit/audio-selftest");
    spike_log("audio: server started (%s); self-test file: %s",
        audioMode == PS5WK_AUDIO_NULL ? "null output" : audioMode == PS5WK_AUDIO_SILENT ? "sceAudioOut, volume 0" : "sceAudioOut",
        selftest ? "present" : "absent");
    if (selftest)
        ps5wk_audio_server_selftest();

    /* Every WebProcess is a payload that costs ~300 of the system's 8192 CPU
     * page tables (README: findings). WebKit kept each page's process alive
     * (back/forward cache, process cache): 6 payloads after 3 sites. Without
     * them only the current page's process and one prewarmed one live. */
    webkit_web_context_set_cache_model(webkit_web_context_get_default(), WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER);
    WebKitSettings* settings = webkit_settings_new_with_settings(
        "enable-page-cache", FALSE,
        "enable-spatial-navigation", TRUE,
        /* Page console messages and uncaught exceptions go to the WebProcess
         * stdout, which the title relays to its log ([webN] lines). */
        "enable-write-console-messages-to-stdout", TRUE,
        "user-agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15",
        NULL);
    /* screen.orientation: Safari on the Mac has it (WebKit turns it on for
     * Apple ports only) and pages that see the Safari user agent call
     * screen.orientation.addEventListener without checking; the throw stopped
     * their scripts before videos and animations started. */
    WebKitFeatureList* features = webkit_settings_get_all_features();
    WebKitFeature* orientation = webkit_feature_list_find(features, "ScreenOrientationAPI");
    if (orientation)
        webkit_settings_set_feature_enabled(settings, orientation, TRUE);
    spike_log("settings: Screen Orientation API %s", orientation ? "on" : "not found");
    /* Hidden pages (live background tabs, browserui.c): timers throttled and
     * CSS animations suspended, as WebKit does for hidden pages on its other
     * ports (both off for WPE by default). */
    static const char* const hiddenPageFeatures[] = { "HiddenPageDOMTimerThrottling", "HiddenPageCSSAnimationSuspension" };
    for (unsigned i = 0; i < G_N_ELEMENTS(hiddenPageFeatures); i++) {
        WebKitFeature* feature = webkit_feature_list_find(features, hiddenPageFeatures[i]);
        if (feature)
            webkit_settings_set_feature_enabled(settings, feature, TRUE);
        spike_log("settings: %s %s", hiddenPageFeatures[i], feature ? "on" : "not found");
    }
    webkit_feature_list_unref(features);
    char* websiteData = g_build_filename(ps5wk_profile_dir(), "browser", "data", NULL);
    char* websiteCache = g_build_filename(ps5wk_profile_dir(), "browser", "cache", NULL);
    char* cookieFile = g_build_filename(websiteData, "cookies.sqlite", NULL);
    WebKitNetworkSession* session = webkit_network_session_new(websiteData, websiteCache);
    networkSession = session;
    /* Without this WPE keeps cookies in memory only: every site logged out
     * and YouTube's consent dialog back after each restart. */
    cookieManager = webkit_network_session_get_cookie_manager(session);
    webkit_cookie_manager_set_persistent_storage(cookieManager, cookieFile, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    g_free(cookieFile);
    g_free(websiteCache);
    g_free(websiteData);
    /* A TV browser: media may start without a gesture (YouTube, players
     * that autoplay; the pad's X is not always on the video). */
    WebKitWebsitePolicies* policies = webkit_website_policies_new_with_policies("autoplay", WEBKIT_AUTOPLAY_ALLOW, NULL);
    webView = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "display", display,
        "settings", settings,
        "network-session", session,
        "website-policies", policies,
        NULL));
    ps5wk_connect_web_view(webView);
    ps5wk_vr_init(display, webView);
    /* After View in VR: the browser's button handler chains it. */
    ps5wk_browser_init(display, webView);
    ps5wk_downloads_init(WPE_DISPLAY_PS5(display), session);
    ps5wk_adblock_init(webView);
    wpe_display_ps5_set_perf_overlay_callback(WPE_DISPLAY_PS5(display), onPerfOverlayToggled, NULL);

    memLog("web view created");
    g_timeout_add_seconds(60, memTimer, NULL);

    char* start = startURLFromLaunch(argc, argv);
    char* home = homeURL();
    if (readTestURLs()) {
        if (start)
            spike_log("test URL list wins over the start URL %s", start);
        ps5wk_browser_start(NULL, home, TRUE);
        loadNextTestURL();
    } else {
        /* The saved tabs come back; a start link opens in a new tab. */
        loadStart = g_get_monotonic_time();
        ps5wk_browser_start(start, home, FALSE);
    }
    g_free(home);

    g_free(start);
    pthread_t control;
    if (!pthread_create(&control, NULL, controlThread, NULL))
        pthread_detach(control);
    pushStart();
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    mainLoop = loop;
    g_main_loop_run(loop);
    /* No child upload may be cut off by the SIGKILL that follows main. */
    wpe_process_manager_ps5_quiesce(5000);
    ps5wk_vr_stop(TRUE, "main loop ended");
    ps5wk_audio_server_stop();
    /* Restart browser, after the wind-down: title_exit.c launches the title
     * again only if the launcher still sees this title in front. */
    if (spike_title_relaunch && spike_title_relaunch[0]) {
        char reply[160];
        if (!launcherAsk("status\n", reply, sizeof reply) || strncmp(reply, "OK ", 3) || !strstr(reply, PS5WK_TITLE_ID)) {
            spike_log("settings: no restart after all (launcher status \"%s\")", reply);
            spike_title_relaunch[0] = 0;
        }
    }
    return 0;
}
