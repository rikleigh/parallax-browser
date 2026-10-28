/*
 * ps5wk-shot: payload-side first light for WPE WebKit on the PS5.
 *
 * A UIProcess in an elfldr payload, on WPE's headless display, with the PS5
 * process manager: it starts the NetworkProcess and the WebProcess (the
 * ps5wk-child payload, /data/ps5webkit/ps5wk-child.elf by default) through
 * the local elfldr, loads a URL with CPU painting (hardware acceleration
 * off), and when the page has loaded and painted reports:
 *
 *   RESULT ps5wk-shot <test> PASS|FAIL <detail>
 *   THUMB <base64 of a binary PPM thumbnail>     (decode: tools in port/webkit/test)
 *
 * Stdin carries the arguments (elfldr passes none for raw uploads): one line
 * "url=<url> child=<path> timeout=<s> width=<w> height=<h> settle=<s>
 * ua=safari|default js=<file>", any order, all optional. settle: seconds to
 * keep running after the load finished (default 3); ua=safari uses the
 * browser title's user agent; js: a script file on the console evaluated in
 * the page just before the report (its result is printed). Page console
 * messages are printed by the WebProcess ([webN] CONSOLE lines).
 * Output goes back on the elfldr connection.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wpe/headless/wpe-headless.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#include <WPEProcessManagerPS5.h>
#include "../common/media-stubs.h"
#include "../common/crashguard.h"

void g_io_openssl_load(void*);

static char url[1024] = "https://example.com/";
static char child[256] = "/data/ps5webkit/ps5wk-child.elf";
static int timeoutSeconds = 120;
static int width = 1280, height = 720;
static int settleSeconds = 3;
static char userAgent[16] = "safari";
static char jsFile[256];
#define SAFARI_UA "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15"

static GMainLoop* loop;
static WebKitWebView* webView;
static GBytes* lastPixels;
static int lastWidth, lastHeight, lastStride;
static unsigned framesRendered;
static gint64 startTime, loadFinishedTime, firstFrameTime;
static gboolean loadFailed;
static int exitCode = 1;

#define RESULT(test, ok, ...) do { printf("RESULT ps5wk-shot %s %s ", test, (ok) ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static double secondsSinceStart(void)
{
    return (g_get_monotonic_time() - startTime) / 1e6;
}

static void readArguments(void)
{
    struct pollfd p = { 0, POLLIN, 0 };
    if (poll(&p, 1, 2000) != 1)
        return;
    char line[2048];
    ssize_t n = read(0, line, sizeof(line) - 1);
    if (n <= 0)
        return;
    line[n] = 0;
    char* save = NULL;
    for (char* token = strtok_r(line, " \t\r\n", &save); token; token = strtok_r(NULL, " \t\r\n", &save)) {
        if (!strncmp(token, "url=", 4))
            g_strlcpy(url, token + 4, sizeof(url));
        else if (!strncmp(token, "child=", 6))
            g_strlcpy(child, token + 6, sizeof(child));
        else if (!strncmp(token, "timeout=", 8))
            timeoutSeconds = atoi(token + 8);
        else if (!strncmp(token, "width=", 6))
            width = atoi(token + 6);
        else if (!strncmp(token, "height=", 7))
            height = atoi(token + 7);
        else if (!strncmp(token, "settle=", 7))
            settleSeconds = atoi(token + 7);
        else if (!strncmp(token, "ua=", 3))
            g_strlcpy(userAgent, token + 3, sizeof(userAgent));
        else if (!strncmp(token, "js=", 3))
            g_strlcpy(jsFile, token + 3, sizeof(jsFile));
        else if (!strncmp(token, "debug=", 6))
            setenv("WEBKIT_DEBUG", token + 6, 1); // also reaches the NetworkProcess (creation parameters)
    }
}

static void onBufferRendered(WPEView* view, WPEBuffer* buffer, gpointer data)
{
    (void)view;
    (void)data;
    GError* error = NULL;
    GBytes* pixels = wpe_buffer_import_to_pixels(buffer, &error);
    if (!pixels) {
        printf("buffer-rendered: import_to_pixels failed: %s\n", error ? error->message : "?");
        g_clear_error(&error);
        return;
    }
    if (!framesRendered++) {
        firstFrameTime = g_get_monotonic_time();
        printf("first frame at %.1f s: %dx%d (%s)\n", secondsSinceStart(), wpe_buffer_get_width(buffer), wpe_buffer_get_height(buffer),
            WPE_IS_BUFFER_SHM(buffer) ? "SHM" : "other");
    }
    if (lastPixels)
        g_bytes_unref(lastPixels);
    lastPixels = g_bytes_ref(pixels);
    lastWidth = wpe_buffer_get_width(buffer);
    lastHeight = wpe_buffer_get_height(buffer);
    lastStride = WPE_IS_BUFFER_SHM(buffer) ? (int)wpe_buffer_shm_get_stride(WPE_BUFFER_SHM(buffer)) : lastWidth * 4;
}

static void report(void)
{
    if (!lastPixels) {
        RESULT("paint", FALSE, "no frame painted within %d s (load %s)", timeoutSeconds, loadFinishedTime ? "finished" : "not finished");
        return;
    }
    gsize size = 0;
    const guint8* p = g_bytes_get_data(lastPixels, &size);
    guint64 hash = 1469598103934665603ull;
    guint64 nonWhite = 0, total = (guint64)lastWidth * lastHeight;
    for (int y = 0; y < lastHeight; y++) {
        const guint8* row = p + (size_t)y * lastStride;
        for (int x = 0; x < lastWidth; x++) {
            const guint8* px = row + x * 4;
            for (int c = 0; c < 4; c++)
                hash = (hash ^ px[c]) * 1099511628211ull;
            if (px[0] != p[0] || px[1] != p[1] || px[2] != p[2])
                nonWhite++; // differs from the top-left pixel (the background)
        }
    }
    gboolean painted = nonWhite * 1000 > total && nonWhite < total; // > 0.1% not background
    RESULT("paint", painted, "%dx%d, %u frames, first at %.1f s, fnv64 %016llx, %.2f%% of pixels differ from the background", lastWidth, lastHeight,
        framesRendered, (firstFrameTime - startTime) / 1e6, (unsigned long long)hash, total ? 100.0 * nonWhite / total : 0.0);

    // Thumbnail: box-filtered to 320 px wide, binary PPM, base64 in lines.
    int tw = 320, th = lastHeight * 320 / (lastWidth ? lastWidth : 1);
    if (th < 1)
        th = 1;
    char header[32];
    int hl = snprintf(header, sizeof(header), "P6\n%d %d\n255\n", tw, th);
    gsize ppmSize = hl + (gsize)tw * th * 3;
    guint8* ppm = g_malloc(ppmSize);
    memcpy(ppm, header, hl);
    guint8* out = ppm + hl;
    for (int ty = 0; ty < th; ty++) {
        for (int tx = 0; tx < tw; tx++) {
            int x0 = tx * lastWidth / tw, x1 = (tx + 1) * lastWidth / tw;
            int y0 = ty * lastHeight / th, y1 = (ty + 1) * lastHeight / th;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    const guint8* px = p + (size_t)y * lastStride + x * 4; // BGRA
                    b += px[0];
                    g += px[1];
                    r += px[2];
                    n++;
                }
            n = n ? n : 1;
            *out++ = r / n;
            *out++ = g / n;
            *out++ = b / n;
        }
    }
    gchar* b64 = g_base64_encode(ppm, ppmSize);
    size_t len = strlen(b64);
    for (size_t off = 0; off < len; off += 4000)
        printf("THUMB %.*s\n", (int)(len - off < 4000 ? len - off : 4000), b64 + off);
    printf("THUMB-END %dx%d\n", tw, th);
    fflush(stdout);
    g_free(b64);
    g_free(ppm);
    exitCode = painted ? 0 : 1;
}

static gboolean onTimeout(gpointer data)
{
    (void)data;
    printf("timeout after %d s\n", timeoutSeconds);
    report();
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static void finish(void)
{
    report();
    g_main_loop_quit(loop);
}

static void onScriptDone(GObject* object, GAsyncResult* result, gpointer data)
{
    (void)data;
    GError* error = NULL;
    JSCValue* value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(object), result, &error);
    if (value) {
        char* text = jsc_value_to_string(value);
        printf("JS %s\n", text ? text : "(null)");
        g_free(text);
        g_object_unref(value);
    } else {
        printf("JS error: %s\n", error ? error->message : "?");
        g_clear_error(&error);
    }
    finish();
}

static gboolean onSettled(gpointer data)
{
    (void)data;
    gchar* script = NULL;
    gsize length = 0;
    if (*jsFile && g_file_get_contents(jsFile, &script, &length, NULL)) {
        webkit_web_view_evaluate_javascript(webView, script, (gssize)length, NULL, NULL, NULL, onScriptDone, NULL);
        g_free(script);
        return G_SOURCE_REMOVE;
    }
    finish();
    return G_SOURCE_REMOVE;
}

static void onLoadChanged(WebKitWebView* view, WebKitLoadEvent event, gpointer data)
{
    (void)data;
    static const char* names[] = { "started", "redirected", "committed", "finished" };
    printf("load %s at %.1f s: %s\n", names[event], secondsSinceStart(), webkit_web_view_get_uri(view));
    if (event == WEBKIT_LOAD_COMMITTED)
        RESULT("network", TRUE, "first response committed at %.1f s", secondsSinceStart());
    if (event == WEBKIT_LOAD_FINISHED) {
        loadFinishedTime = g_get_monotonic_time();
        if (!loadFailed)
            RESULT("load", TRUE, "%s finished at %.1f s, title \"%s\"", url, secondsSinceStart(), webkit_web_view_get_title(view) ?: "");
        g_timeout_add_seconds(settleSeconds, onSettled, NULL); // let it paint
    }
}

static gboolean onLoadFailed(WebKitWebView* view, WebKitLoadEvent event, const char* failingURI, GError* error, gpointer data)
{
    (void)view;
    (void)event;
    (void)data;
    loadFailed = TRUE;
    RESULT("load", FALSE, "%s: %s (domain %s code %d)", failingURI, error->message, g_quark_to_string(error->domain), error->code);
    return FALSE;
}

static void onWebProcessTerminated(WebKitWebView* view, WebKitWebProcessTerminationReason reason, gpointer data)
{
    (void)view;
    (void)data;
    RESULT("webprocess", FALSE, "terminated (reason %d) at %.1f s", reason, secondsSinceStart());
    report();
    g_main_loop_quit(loop);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    ps5wk_crash_guard_install("shot");
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    startTime = g_get_monotonic_time();
    readArguments();
    printf("ps5wk-shot: pid %d, url %s, child %s, %dx%d, timeout %d s\n", (int)getpid(), url, child, width, height, timeoutSeconds);

    struct stat st;
    if (stat(child, &st)) {
        RESULT("setup", FALSE, "child ELF %s missing (errno %d): upload it with port/webkit/test/run-shot.sh", child, errno);
        return 1;
    }
    mkdir("/data/ps5webkit", 0777);
    mkdir("/data/ps5webkit/home", 0777);
    mkdir("/system_tmp/ps5wk", 0777);
    chmod("/system_tmp/ps5wk", 0777);
    setenv("HOME", "/data/ps5webkit/home", 0);
    setenv("XDG_RUNTIME_DIR", "/system_tmp/ps5wk", 0);
    setenv("GIO_USE_VFS", "local", 0);
    setenv("ICU_DATA", "/data/ps5webkit/share/icu", 0);
    setenv("XDG_DATA_DIRS", "/data/ps5webkit/share", 0); /* share/mime/globs2 */
    setenv("GSETTINGS_BACKEND", "memory", 0);
    g_io_openssl_load(NULL);

    WPEProcessManager* manager = wpe_process_manager_ps5_new(child);
    wpe_process_manager_set_default(manager);

    GError* error = NULL;
    WPEDisplay* display = wpe_display_headless_new();
    if (!wpe_display_connect(display, &error)) {
        RESULT("setup", FALSE, "headless display: %s", error ? error->message : "?");
        return 1;
    }

    WebKitSettings* settings = webkit_settings_new_with_settings(
        "enable-developer-extras", FALSE,
        "enable-write-console-messages-to-stdout", TRUE,
        NULL);
    if (!strcmp(userAgent, "safari"))
        webkit_settings_set_user_agent(settings, SAFARI_UA);
    WebKitNetworkSession* session = webkit_network_session_new("/data/ps5webkit/shot/data", "/data/ps5webkit/shot/cache");
    webView = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "display", display,
        "settings", settings,
        "network-session", session,
        NULL));

    WPEView* view = webkit_web_view_get_wpe_view(webView);
    WPEToplevel* toplevel = wpe_view_get_toplevel(view);
    if (toplevel)
        wpe_toplevel_resize(toplevel, width, height);
    else
        printf("note: the headless view has no toplevel\n");
    if (!g_getenv("PS5WK_NO_MEDIA_STUBS")) {
        WebKitUserScript* stubs = webkit_user_script_new(ps5wkMediaStubsScript, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
        webkit_user_content_manager_add_script(webkit_web_view_get_user_content_manager(webView), stubs);
        webkit_user_script_unref(stubs);
    }
    g_signal_connect(view, "buffer-rendered", G_CALLBACK(onBufferRendered), NULL);
    g_signal_connect(webView, "load-changed", G_CALLBACK(onLoadChanged), NULL);
    g_signal_connect(webView, "load-failed", G_CALLBACK(onLoadFailed), NULL);
    g_signal_connect(webView, "web-process-terminated", G_CALLBACK(onWebProcessTerminated), NULL);

    RESULT("setup", TRUE, "UIProcess up at %.1f s, loading %s", secondsSinceStart(), url);
    webkit_web_view_load_uri(webView, url);

    loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add_seconds(timeoutSeconds, onTimeout, NULL);
    g_main_loop_run(loop);
    printf("ps5wk-shot done (%d) after %.1f s\n", exitCode, secondsSinceStart());
    fflush(stdout);
    _exit(exitCode); // children follow when their IPC sockets close
}
