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
#include "common/title.h"

#include <signal.h>
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
#define PS5WK_HOME_URL "https://example.com/"
#endif

void g_io_openssl_load(void*);

static WebKitWebView* webView;
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
    case WPE_KEY_Menu:
        wpe_display_ps5_request_text(WPE_DISPLAY_PS5(display), "Address or search", webkit_web_view_get_uri(webView), TRUE, onURLEntered, NULL);
        return TRUE;
    default:
        return FALSE;
    }
}

static void onLoadChanged(WebKitWebView* view, WebKitLoadEvent event, gpointer data)
{
    (void)data;
    static const char* names[] = { "started", "redirected", "committed", "finished" };
    spike_log("load %s: %s", names[event], webkit_web_view_get_uri(view));
    if (event == WEBKIT_LOAD_FINISHED)
        SPIKE_PASS("load", "%s \"%s\"", webkit_web_view_get_uri(view), webkit_web_view_get_title(view) ? webkit_web_view_get_title(view) : "");
}

static gboolean onLoadFailed(WebKitWebView* view, WebKitLoadEvent event, const char* uri, GError* error, gpointer data)
{
    (void)view;
    (void)event;
    (void)data;
    SPIKE_FAIL("load", "%s: %s", uri, error->message);
    return FALSE;
}

static void onWebProcessTerminated(WebKitWebView* view, WebKitWebProcessTerminationReason reason, gpointer data)
{
    (void)data;
    SPIKE_FAIL("webprocess", "terminated (reason %d); reloading", reason);
    webkit_web_view_reload(view);
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

int main(void)
{
    spike_title_init("ps5browser", LOG_HOST, LOG_PORT);
    signal(SIGPIPE, SIG_IGN);
    // WebKit, GLib and the child payloads (relayed) log to stderr.
    if (spike_log_fd >= 0)
        dup2(spike_log_fd, 2);
    setvbuf(stderr, NULL, _IOLBF, 0);
    g_log_set_default_handler(logHandler, NULL);
    spike_log("ps5browser: WPE WebKit %u.%u.%u, pid %d", webkit_get_major_version(), webkit_get_minor_version(),
        webkit_get_micro_version(), (int)getpid());
    spike_notify("ps5webkit browser starting");

    mkdir("/data/ps5webkit", 0777);
    mkdir("/data/ps5webkit/home", 0777);
    setenv("HOME", "/data/ps5webkit/home", 0);
    setenv("XDG_RUNTIME_DIR", "/data/ps5webkit/home", 0);
    setenv("GIO_USE_VFS", "local", 0);
    setenv("ICU_DATA", "/data/ps5webkit/share/icu", 0);
    setenv("GSETTINGS_BACKEND", "memory", 0);
    g_io_openssl_load(NULL);

    GError* error = NULL;
    display = wpe_display_ps5_new();
    wpe_display_ps5_set_child_elf(WPE_DISPLAY_PS5(display), "/app0/ps5wk-child.elf");
    if (!wpe_display_connect(display, &error)) {
        SPIKE_FAIL("display", "%s", error ? error->message : "?");
        return 1;
    }
    wpe_display_set_primary(display);
    SPIKE_PASS("display", "PS5 display connected");

    WebKitSettings* settings = webkit_settings_new_with_settings(
        "enable-spatial-navigation", TRUE,
        "user-agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15",
        NULL);
    WebKitNetworkSession* session = webkit_network_session_new("/data/ps5webkit/browser/data", "/data/ps5webkit/browser/cache");
    webView = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "display", display,
        "settings", settings,
        "network-session", session,
        NULL));
    WPEView* view = webkit_web_view_get_wpe_view(webView);
    g_signal_connect(view, "event", G_CALLBACK(onViewEvent), NULL);
    g_signal_connect(webView, "load-changed", G_CALLBACK(onLoadChanged), NULL);
    g_signal_connect(webView, "load-failed", G_CALLBACK(onLoadFailed), NULL);
    g_signal_connect(webView, "web-process-terminated", G_CALLBACK(onWebProcessTerminated), NULL);

    char* home = homeURL();
    spike_log("loading %s", home);
    webkit_web_view_load_uri(webView, home);
    g_free(home);

    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
