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
 *   O: back, Options: type a URL or search, Square: reload,
 *   L2/R2: zoom out/in, L1/R1: previous/next field, Triangle: keyboard.
 *
 * Logs go to the Mac's tools/logserver.sh (LOG_HOST:LOG_PORT) and to
 * /data/ps5webkit/ps5browser.log.
 */
#define SPIKE_NAME "ps5browser"
#include "../common/ps5audioserver.h"
#include "common/title.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <sys/filio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <wpe/ps5/wpe-ps5.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>

#ifndef LOG_HOST
#define LOG_HOST "192.168.1.20"
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
    spike_log("wind-down: quit requested; %d audio stream(s) open; loading about:blank", ps5wk_audio_server_active_streams());
    if (webView)
        webkit_web_view_load_uri(webView, "about:blank");
    g_timeout_add(50, windDownStep, NULL);
    return G_SOURCE_REMOVE;
}

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
        char line[64] = "";
        ssize_t n = recv(client, line, sizeof line - 1, 0);
        line[n > 0 ? n : 0] = 0;
        if (!strncmp(line, "quit", 4)) {
            static const char reply[] = "OK winding down\n";
            send(client, reply, sizeof reply - 1, 0);
            g_main_context_invoke(NULL, beginWindDown, NULL);
        } else {
            static const char reply[] = "ERR unknown command (quit)\n";
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

static void onURLEntered(const char* text, gpointer data)
{
    (void)data;
    if (!text) {
        spike_log("URL prompt cancelled");
        return;
    }
    char* uri = uriFromInput(text);
    if (uri) {
        spike_log("loading %s", uri);
        webkit_web_view_load_uri(webView, uri);
        g_free(uri);
    }
}

static gboolean onViewEvent(WPEView* view, WPEEvent* event, gpointer data)
{
    (void)view;
    (void)data;
    if (wpe_event_get_event_type(event) != WPE_EVENT_KEYBOARD_KEY_DOWN)
        return FALSE;
    switch (wpe_event_keyboard_get_keyval(event)) {
    case WPE_KEY_Back:
        if (webkit_web_view_can_go_back(webView))
            webkit_web_view_go_back(webView);
        return TRUE;
    case WPE_KEY_Reload:
        webkit_web_view_reload(webView);
        return TRUE;
    case WPE_KEY_ZoomIn:
    case WPE_KEY_ZoomOut:
        zoom *= wpe_event_keyboard_get_keyval(event) == WPE_KEY_ZoomIn ? 1.15 : 1 / 1.15;
        zoom = zoom < 0.3 ? 0.3 : zoom > 4.0 ? 4.0 : zoom;
        webkit_web_view_set_zoom_level(webView, zoom);
        spike_log("zoom %.2f", zoom);
        return TRUE;
    case WPE_KEY_Menu: {
        /* Start from the current address, except for the built-in pages. */
        const char* current = webkit_web_view_get_uri(webView);
        if (!current || !strncmp(current, "data:", 5) || !strncmp(current, "file:", 5) || !strncmp(current, "about:", 6))
            current = "";
        if (!wpe_display_ps5_request_text(WPE_DISPLAY_PS5(display), "Address or search", current, TRUE, onURLEntered, NULL))
            spike_log("URL prompt: no system keyboard");
        return TRUE;
    }
    default:
        return FALSE;
    }
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

/* Test URL list: /data/ps5webkit/test-urls.txt, one URL per line (# comments).
 * Each is loaded in turn; after it finishes: a screenshot at +2 s and +10 s,
 * load time and memory in the log, then the next one at +12 s. */
static char** testURLs;
static unsigned testURLCount, testURLIndex, testURLsLoaded;
static gint64 loadStart;
static guint testTimer;
static gboolean testWaitingForLoad;

static void loadNextTestURL(void);

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
    static char names[16][2][96];
    const char* host = strstr(uri, "://") ? strstr(uri, "://") + 3 : uri;
    size_t hostLength = strcspn(host, "/?#:");
    unsigned slot = testURLIndex % 16;
    snprintf(names[slot][0], sizeof names[slot][0], "%.*s-2s", (int)(hostLength < 80 ? hostLength : 80), host);
    snprintf(names[slot][1], sizeof names[slot][1], "%.*s-10s", (int)(hostLength < 80 ? hostLength : 80), host);
    g_timeout_add(2000, shotAfterLoad, names[slot][0]);
    g_timeout_add(2000, testOnload, NULL);
    g_timeout_add(10000, shotAfterLoad, names[slot][1]);
    testTimer = g_timeout_add(12000, testTimeout, NULL);
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
        if (**line && **line != '#')
            g_ptr_array_add(list, g_strdup(*line));
    }
    g_strfreev(lines);
    g_free(contents);
    testURLCount = list->len;
    g_ptr_array_add(list, NULL);
    testURLs = (char**)g_ptr_array_free(list, FALSE);
    spike_log("test URL list: %u URLs", testURLCount);
    return testURLCount > 0;
}

static void onLoadChanged(WebKitWebView* view, WebKitLoadEvent event, gpointer data)
{
    (void)data;
    static const char* names[] = { "started", "redirected", "committed", "finished" };
    spike_log("load %s: %s", names[event], webkit_web_view_get_uri(view));
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
    (void)view;
    (void)event;
    (void)data;
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
    gboolean again = uri && lastCrashURI && !strcmp(uri, lastCrashURI) && now - lastCrashTime < 30 * G_USEC_PER_SEC;
    g_free(lastCrashURI);
    lastCrashURI = g_strdup(uri ? uri : "");
    lastCrashTime = now;
    if (!again) {
        SPIKE_FAIL("webprocess", "terminated (reason %d); reloading", reason);
        webkit_web_view_reload(view);
        return;
    }
    SPIKE_FAIL("webprocess", "terminated (reason %d) again on %s within 30 s; showing an error page", reason, lastCrashURI);
    char* escaped = g_markup_escape_text(lastCrashURI, -1);
    char* html = g_strdup_printf("<!doctype html><html><head><meta charset=\"utf-8\"><title>Page crashed</title>"
        "<style>body{font-family:sans-serif;background:#10141c;color:#e8ecf2;margin:64px;font-size:24px}"
        "a{color:#9cc7ff}code{color:#9aa4b2;word-break:break-all}</style></head><body>"
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

static char* homeURL(void)
{
    gchar* contents = NULL;
    if (g_file_get_contents("/data/ps5webkit/home-url.txt", &contents, NULL, NULL)) {
        g_strstrip(contents);
        if (*contents)
            return contents;
        g_free(contents);
    }
    return g_strdup(PS5WK_HOME_URL);
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

int main(void)
{
    spike_title_init("ps5browser", LOG_HOST, LOG_PORT);
    loadStart = g_get_monotonic_time();
    signal(SIGPIPE, SIG_IGN);
    // WebKit, GLib and the child payloads (relayed) log to stderr.
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
    spike_log("ps5browser: WPE WebKit %u.%u.%u, pid %d", webkit_get_major_version(), webkit_get_minor_version(),
        webkit_get_micro_version(), (int)getpid());
    spike_notify("ps5webkit browser starting");
    memLog("main");
    if (ps5wk_title_check_imports) {
        static char names[1024];
        unsigned total = 0;
        unsigned missing = ps5wk_title_check_imports(names, sizeof names, &total);
        spike_log("imports: %u of %u NULL%s%s", missing, total, missing ? ": " : "", names);
    }
    probeSockets();

    mkdir("/data/ps5webkit", 0777);
    mkdir("/data/ps5webkit/home", 0777);
    setenv("HOME", "/data/ps5webkit/home", 0);
    setenv("XDG_RUNTIME_DIR", "/data/ps5webkit/home", 0);
    setenv("GIO_USE_VFS", "local", 0);
    setenv("ICU_DATA", "/data/ps5webkit/share/icu", 0);
    setenv("XDG_DATA_DIRS", "/data/ps5webkit/share", 0); /* share/mime/globs2 */
    setenv("GSETTINGS_BACKEND", "memory", 0);
    g_io_openssl_load(NULL);

    /* open(), not access(): access() said no for an existing file here. */
    int flag = open("/data/ps5webkit/debug-screenshot", O_RDONLY);
    if (flag >= 0)
        close(flag);
    screenshots = flag >= 0 || g_getenv("PS5WK_SCREENSHOT");
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
    WebKitNetworkSession* session = webkit_network_session_new("/data/ps5webkit/browser/data", "/data/ps5webkit/browser/cache");
    /* A TV browser: media may start without a gesture (YouTube, players
     * that autoplay; the pad's X is not always on the video). */
    WebKitWebsitePolicies* policies = webkit_website_policies_new_with_policies("autoplay", WEBKIT_AUTOPLAY_ALLOW, NULL);
    webView = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "display", display,
        "settings", settings,
        "network-session", session,
        "website-policies", policies,
        NULL));
    WPEView* view = webkit_web_view_get_wpe_view(webView);
    g_signal_connect(view, "event", G_CALLBACK(onViewEvent), NULL);
    g_signal_connect(webView, "load-changed", G_CALLBACK(onLoadChanged), NULL);
    g_signal_connect(webView, "load-failed", G_CALLBACK(onLoadFailed), NULL);
    g_signal_connect(webView, "web-process-terminated", G_CALLBACK(onWebProcessTerminated), NULL);

    memLog("web view created");
    g_timeout_add_seconds(60, memTimer, NULL);

    if (readTestURLs()) {
        loadNextTestURL();
    } else {
        char* home = homeURL();
        spike_log("loading %s", home);
        loadStart = g_get_monotonic_time();
        webkit_web_view_load_uri(webView, home);
        g_free(home);
    }

    pthread_t control;
    if (!pthread_create(&control, NULL, controlThread, NULL))
        pthread_detach(control);
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    mainLoop = loop;
    g_main_loop_run(loop);
    ps5wk_audio_server_stop();
    return 0;
}
