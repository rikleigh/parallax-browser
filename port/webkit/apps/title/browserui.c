/*
 * Browser basics (ps5browser): tabs, the address bar and its menu, and
 * session restore. See browser.h and port/webkit/README.md ("Tabs,
 * bookmarks, history").
 *
 * Tabs share the one WebKitWebView (one WebProcess): only the active tab
 * is a live page. A background tab is its address, title, scroll position
 * and WebKit's serialized session state (its back/forward list). Switching
 * reads the shown page's scroll position (JavaScript), saves its session
 * state, restores the other tab's state and goes to its current entry (a
 * history navigation: the HTTP cache serves what it has, then the network),
 * then scrolls back to where it was. A page that came from a form (POST) is
 * not sent again: a short page offers to load the address instead. A new
 * tab gets a fresh back/forward list (an empty about:blank first entry that
 * Back skips). At most MAX_TABS tabs; memory stays that of one page.
 *
 * The UI is drawn by the title (wpe_display_ps5_set_ui_layer, a CPU canvas
 * shown over the page by PS5Renderer, the same with GPU compositing on or
 * off, not in the headset):
 *   - while a page loads, a bar at the top with its title, address and
 *     progress, gone 1.5 s after the load ends
 *   - Options: the menu. The bar becomes buttons (Back, Forward, Reload or
 *     Stop, Home, the address, Bookmark, New tab) over a panel with three
 *     lists: Tabs, Bookmarks, History. The pad is in UI mode while it is
 *     open (wpe_display_ps5_set_ui_mode): D-pad moves the yellow focus, X
 *     picks, the left stick's pointer and X work too, the right stick
 *     scrolls the list, L1/R1 (or left/right in a list) switch lists,
 *     Square removes (closes a tab, deletes a bookmark or history entry),
 *     R2 opens a bookmark or history entry in a new tab, Triangle (or X on
 *     the address) brings up the system keyboard for an address or search
 *     words, O or Options closes the menu.
 *
 * Main thread, except ps5wk_browser_api (the link server's thread), which
 * runs its work on the main loop and waits for it.
 */
#define SPIKE_NAME "ps5browser"
#include "common/spike.h"
#include "browser.h"
#include "browserdraw.h"
#include "viewinvr.h"
#include "../common/webmem.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_TABS 12
#define STATE_MAX_BYTES (512 * 1024) /* a bigger saved state: address and scroll only */
#define SESSION_DELAY_MS 2000
#define BAR_HIDE_MS 1500
#define DATA_DIR "/data/ps5webkit"
#define NO_RESTORE_FLAG DATA_DIR "/no-session-restore"
#define NO_LOAD_BAR_FLAG DATA_DIR "/no-load-bar"
#define SCROLL_WORLD "ps5wk"

/* ps5browser.c */
char* ps5wk_uri_from_input(const char* text);
char* ps5wk_home_url(void);
void ps5wk_open_settings(void);
gboolean ps5wk_vr_buttons(guint32 pressed);

typedef struct {
    char* url;
    char* title;
    GBytes* state; /* the saved back/forward list (a discarded tab's way back; refreshed when a live tab is hidden) */
    double scrollX, scrollY;
    gboolean fromForm; /* the page shown is a form's (POST) result */
    WebKitWebView* view; /* live: its own web view (the active tab always has one), else NULL */
    gint64 lastUsed;     /* monotonic us: last shown (LRU of live tabs) */
} Tab;

static WPEDisplayPS5* display;
static WebKitWebView* webView; /* the active tab's view */
static GPtrArray* tabs; /* Tab* */
static int active;
static gboolean sessionFrozen; /* a test run, or winding down: no session writes */
static guint sessionTimer;
static gboolean restoring;      /* a tab's page is coming back: no form resubmission */
static gboolean pendingScrollSet;
static double pendingScrollX, pendingScrollY;
static gboolean switching; /* waiting for the scroll position before a switch */
static char* postURI;      /* the last main-frame navigation that sent a form (POST) */
static char* formPageURI;
static char* formPlaceholderURI; /* the placeholder for a form result, until it commits */
static unsigned liveLimit = 2;   /* live tabs (views), /data/ps5webkit/live-tabs */
static gboolean memoryTight;     /* over the pressure line: one live tab until well under it */
static guint memoryTimer;
static gboolean showFormPage(gpointer data);

/* ---------------------------------------------------------------- tabs */

static Tab* tabNew(const char* url, const char* title)
{
    Tab* tab = g_new0(Tab, 1);
    tab->url = g_strdup(url ? url : "");
    tab->title = g_strdup(title ? title : "");
    return tab;
}

static void tabFree(gpointer data)
{
    Tab* tab = data;
    g_free(tab->url);
    g_free(tab->title);
    if (tab->state)
        g_bytes_unref(tab->state);
    g_free(tab);
}

static Tab* activeTab(void)
{
    return tabs && active >= 0 && active < (int)tabs->len ? g_ptr_array_index(tabs, active) : NULL;
}

static void setString(char** field, const char* value)
{
    if (value && (!*field || strcmp(*field, value))) {
        g_free(*field);
        *field = g_strdup(value);
    }
}

/* A one-entry back/forward list (WebKit's session state format, version 3):
 * the new tab's empty first entry, or a saved tab without a state. */
static GVariant* nothing(const char* type)
{
    return g_variant_new_maybe(G_VARIANT_TYPE(type), NULL);
}

/* Built value by value: g_variant_new()'s varargs for maybe types of
 * tuples and integers are easy to get wrong (a crash). */
static GBytes* craftState(const char* url, const char* title, double scrollX, double scrollY)
{
    static gint64 sequence;
    if (!sequence)
        sequence = g_get_real_time();
    if (!url || !g_utf8_validate(url, -1, NULL))
        url = "about:blank";
    if (!title || !g_utf8_validate(title, -1, NULL))
        title = "";
    GVariant* frame[12] = {
        g_variant_new_string(url), g_variant_new_string(url), g_variant_new_string(""), g_variant_new_string(""),
        g_variant_new_array(G_VARIANT_TYPE_STRING, NULL, 0),
        nothing("ay"),
        g_variant_new_int64(sequence + 1), g_variant_new_int64(sequence + 2),
        g_variant_new("(ii)", (gint32)scrollX, (gint32)scrollY),
        g_variant_new_double(1.0),
        nothing("(sa(uaysxmxmds))"),
        g_variant_new_array(G_VARIANT_TYPE_VARIANT, NULL, 0),
    };
    sequence += 2;
    GVariant* item[4] = { g_variant_new_string(title ? title : ""), g_variant_new_tuple(frame, 12), nothing("t"), g_variant_new_uint32(2) /* NotAllow */ };
    GVariant* entry = g_variant_new_tuple(item, 4);
    GVariant* top[3] = { g_variant_new_uint16(3), g_variant_new_array(NULL, &entry, 1), g_variant_new_maybe(NULL, g_variant_new_uint32(0)) };
    GVariant* state = g_variant_ref_sink(g_variant_new_tuple(top, 3));
    GBytes* bytes = g_variant_get_data_as_bytes(state);
    g_variant_unref(state);
    return bytes;
}

static gboolean restoreState(WebKitWebView* view, GBytes* bytes)
{
    WebKitWebViewSessionState* state = bytes ? webkit_web_view_session_state_new(bytes) : NULL;
    if (!state)
        return FALSE;
    webkit_web_view_restore_session_state(view, state);
    webkit_web_view_session_state_unref(state);
    return TRUE;
}

/* A view's back/forward list, serialized (NULL when too big). */
static GBytes* captureState(WebKitWebView* view)
{
    WebKitWebViewSessionState* state = webkit_web_view_get_session_state(view);
    if (!state)
        return NULL;
    GBytes* bytes = webkit_web_view_session_state_serialize(state);
    webkit_web_view_session_state_unref(state);
    if (bytes && g_bytes_get_size(bytes) > STATE_MAX_BYTES) {
        spike_log("browser: session state of %s is %zu KB: keeping its address only", webkit_web_view_get_uri(view),
            g_bytes_get_size(bytes) / 1024);
        g_bytes_unref(bytes);
        bytes = NULL;
    }
    return bytes;
}

static void scheduleSessionSave(void);
static void redraw(void);
static void showBar(void);
static void connectView(WebKitWebView* view);

/* The page in a view becomes this tab's (a fresh list: about:blank, which
 * Back skips, then the page). */
static void loadFresh(WebKitWebView* view, const char* url)
{
    GBytes* blank = craftState("about:blank", "", 0, 0);
    restoreState(view, blank);
    g_bytes_unref(blank);
    if (view == webView) {
        restoring = FALSE;
        pendingScrollSet = FALSE;
    }
    webkit_web_view_load_uri(view, url);
}

/* Loads the active tab's page into its view from what was saved of it. */
static void activate(void)
{
    Tab* tab = activeTab();
    if (!tab || !tab->view)
        return;
    WebKitWebView* view = tab->view;
    GBytes* bytes = tab->state ? g_bytes_ref(tab->state) : craftState(tab->url, tab->title, tab->scrollX, tab->scrollY);
    gboolean ok = restoreState(view, bytes);
    g_bytes_unref(bytes);
    WebKitBackForwardListItem* item = ok ? webkit_back_forward_list_get_current_item(webkit_web_view_get_back_forward_list(view)) : NULL;
    pendingScrollSet = tab->scrollX > 1 || tab->scrollY > 1;
    pendingScrollX = tab->scrollX;
    pendingScrollY = tab->scrollY;
    restoring = TRUE;
    if (tab->fromForm) {
        /* Its page came from a form: never sent again by itself. */
        spike_log("browser: tab %d: %s came from a form: not sent again, offering to load the address", active + 1, tab->url);
        restoring = FALSE;
        pendingScrollSet = FALSE;
        g_free(formPageURI);
        formPageURI = g_strdup(tab->url);
        showFormPage(NULL);
    } else if (item && !g_str_equal(webkit_back_forward_list_item_get_uri(item), "about:blank")) {
        spike_log("browser: tab %d: back to %s (scroll %.0f,%.0f%s)", active + 1, webkit_back_forward_list_item_get_uri(item),
            tab->scrollX, tab->scrollY, tab->state ? "" : ", no saved list");
        webkit_web_view_go_to_back_forward_list_item(view, item);
    } else {
        spike_log("browser: tab %d: loading %s (no saved entry)", active + 1, tab->url);
        loadFresh(view, *tab->url ? tab->url : "about:blank");
    }
}

/* What a tab's view shows now into its record (address, title, list). */
static void saveTab(Tab* tab)
{
    if (!tab || !tab->view)
        return;
    const char* uri = webkit_web_view_get_uri(tab->view);
    if (uri && *uri && strcmp(uri, "about:blank"))
        setString(&tab->url, uri);
    const char* title = webkit_web_view_get_title(tab->view);
    if (title && *title)
        setString(&tab->title, title);
    if (tab->state)
        g_bytes_unref(tab->state);
    tab->state = captureState(tab->view);
}

static void saveLive(void)
{
    saveTab(activeTab());
}

/* ---- live tabs: one web view each, all in the one WebProcess ---- */

static unsigned liveCount(void)
{
    unsigned n = 0;
    for (unsigned i = 0; tabs && i < tabs->len; i++)
        n += ((Tab*)g_ptr_array_index(tabs, i))->view != NULL;
    return n;
}

static unsigned liveAllowed(void)
{
    return memoryTight ? 1 : liveLimit;
}

/* Which tabs keep their pages, after every change (switch, new tab, close,
 * discard): "[tabs] live (2 of 2 allowed): 1 shown, 3 hidden 12 s ago". */
static void logLiveTabs(const char* why)
{
    if (!tabs)
        return;
    GString* line = g_string_new(NULL);
    gint64 now = g_get_monotonic_time();
    for (unsigned i = 0; i < tabs->len; i++) {
        Tab* tab = g_ptr_array_index(tabs, i);
        if (!tab->view)
            continue;
        if ((int)i == active)
            g_string_append_printf(line, "%s%u shown", line->len ? ", " : "", i + 1);
        else
            g_string_append_printf(line, "%s%u hidden %.0f s ago", line->len ? ", " : "", i + 1, (now - tab->lastUsed) / 1e6);
    }
    unsigned live = liveCount();
    spike_log("[tabs] live (%u of %u allowed%s) after %s: %s; %u discarded of %u tabs", live, liveAllowed(), memoryTight ? ", memory tight" : "", why,
        line->len ? line->str : "none", tabs->len - live, tabs->len);
    g_string_free(line, TRUE);
}

/* The display, the title's other parts (ps5browser.c: load handlers, VR,
 * the ad blocker) and the UI follow the active tab's view. */
void ps5wk_set_active_web_view(WebKitWebView* view);

static void showView(WebKitWebView* view)
{
    webView = view;
    wpe_display_ps5_set_active_view(display, webkit_web_view_get_wpe_view(view));
    ps5wk_set_active_web_view(view);
}

/* A new hidden web view sharing the first one's WebProcess (patch 0018:
 * one WebProcess for everything), settings, content filters and policies. */
static WebKitWebView* newView(void)
{
    /* From now on the display maps only the view made active. */
    wpe_display_ps5_set_active_view(display, webkit_web_view_get_wpe_view(webView));
    WebKitWebView* view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "related-view", webView,
        "settings", webkit_web_view_get_settings(webView),
        "user-content-manager", webkit_web_view_get_user_content_manager(webView),
        "website-policies", webkit_web_view_get_website_policies(webView),
        NULL));
    connectView(view);
    spike_log("[tabs] new web view %p (%u live)", view, liveCount() + 1);
    return view;
}

static void viewGone(gpointer data, GObject* gone)
{
    (void)data;
    spike_log("[tabs] web view %p gone", (void*)gone);
}

static void destroyView(WebKitWebView* view)
{
    if (!view)
        return;
    spike_log("[tabs] web view %p closing", view);
    g_signal_handlers_disconnect_matched(view, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, &tabs);
    g_object_weak_ref(G_OBJECT(view), viewGone, NULL);
    g_object_unref(view);
}

/* A hidden live tab gives up its view: back to its record (address, title,
 * scroll, list), its page goes. Returns the view (still alive) when keep. */
static WebKitWebView* discardTab(Tab* tab, const char* why, gboolean keep)
{
    if (!tab || !tab->view || tab == activeTab())
        return NULL;
    saveTab(tab);
    WebKitWebView* view = tab->view;
    tab->view = NULL;
    int index = -1;
    for (unsigned i = 0; i < tabs->len; i++) {
        if (g_ptr_array_index(tabs, i) == tab)
            index = (int)i;
    }
    spike_log("[tabs] tab %d discarded (%s): %s, scroll %.0f,%.0f%s", index + 1, why, tab->url, tab->scrollX, tab->scrollY,
        keep ? "; its view is reused" : "");
    if (keep)
        return view;
    destroyView(view);
    return NULL;
}

/* The least recently used hidden live tab. */
static Tab* lruHidden(void)
{
    Tab* lru = NULL;
    for (unsigned i = 0; i < tabs->len; i++) {
        Tab* tab = g_ptr_array_index(tabs, i);
        if (tab->view && (int)i != active && (!lru || tab->lastUsed < lru->lastUsed))
            lru = tab;
    }
    return lru;
}

/* A view for a tab that is about to be shown (the active tab, still the
 * old one, keeps its view): a new one while under the limit, else the
 * least recently used hidden tab's (that tab is discarded). With one live
 * tab allowed, the old active tab's own view (it is discarded). */
static WebKitWebView* viewForNewlyShownTab(Tab* old)
{
    while (liveCount() > liveAllowed() && lruHidden())
        discardTab(lruHidden(), "over the live-tab limit", FALSE);
    if (liveCount() < liveAllowed())
        return newView();
    if (lruHidden())
        return discardTab(lruHidden(), "least recently used", TRUE);
    /* One live tab: the old tab gives its view. */
    if (old && old->view) {
        saveTab(old);
        WebKitWebView* view = old->view;
        old->view = NULL;
        return view;
    }
    return newView();
}

/* Hidden live tabs all go (memory, a WebProcess that ended). */
static void discardHidden(const char* why)
{
    for (unsigned i = 0; tabs && i < tabs->len; i++) {
        Tab* tab = g_ptr_array_index(tabs, i);
        if (tab->view && (int)i != active)
            discardTab(tab, why, FALSE);
    }
}

void ps5wk_browser_discard_hidden(const char* why)
{
    discardHidden(why);
    logLiveTabs(why);
    redraw();
}

typedef void (*ScrollDone)(gpointer data);
typedef struct {
    ScrollDone done;
    gpointer data;
    guint timeout;
    gboolean finished;
    Tab* tab;
} ScrollCapture;

static void scrollCaptureFinish(ScrollCapture* capture)
{
    if (capture->finished)
        return;
    capture->finished = TRUE;
    if (capture->timeout)
        g_source_remove(capture->timeout);
    capture->timeout = 0;
    if (capture->done)
        capture->done(capture->data);
}

static gboolean tabExists(Tab* tab)
{
    for (unsigned i = 0; tabs && i < tabs->len; i++) {
        if (g_ptr_array_index(tabs, i) == tab)
            return TRUE;
    }
    return FALSE;
}

static void scrollCaptured(GObject* object, GAsyncResult* result, gpointer data)
{
    ScrollCapture* capture = data;
    JSCValue* value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(object), result, NULL);
    /* Also after the timeout: while the tab still has this view (a hidden
     * live tab), the answer is its page's. */
    if (value && tabExists(capture->tab) && capture->tab->view == WEBKIT_WEB_VIEW(object)) {
        char* text = jsc_value_to_string(value);
        double x = 0, y = 0;
        if (text && sscanf(text, "%lf,%lf", &x, &y) == 2) {
            capture->tab->scrollX = x;
            capture->tab->scrollY = y;
        }
        g_free(text);
    }
    if (value)
        g_object_unref(value);
    scrollCaptureFinish(capture);
    g_free(capture);
}

static gboolean scrollCaptureTimeout(gpointer data)
{
    ScrollCapture* capture = data;
    capture->timeout = 0;
    scrollCaptureFinish(capture); /* freed when the script answers (it always does, at the latest when the page goes) */
    return G_SOURCE_REMOVE;
}

/* The live tab's scroll position into its Tab, then done(data) (at most
 * ~400 ms later: a busy page answers late); done NULL: nothing waits. */
static void captureScroll(ScrollDone done, gpointer data)
{
    ScrollCapture* capture = g_new0(ScrollCapture, 1);
    capture->done = done;
    capture->data = data;
    capture->tab = activeTab();
    capture->timeout = g_timeout_add(400, scrollCaptureTimeout, capture);
    webkit_web_view_evaluate_javascript(webView, "Math.round(scrollX)+','+Math.round(scrollY)", -1, SCROLL_WORLD, NULL, NULL,
        scrollCaptured, capture);
}

static void switchNow(gpointer data)
{
    int target = GPOINTER_TO_INT(data);
    switching = FALSE;
    if (target < 0 || target >= (int)tabs->len || target == active) {
        redraw();
        return;
    }
    gint64 start = g_get_monotonic_time();
    ps5wk_vr_stop(FALSE, "tab switch");
    Tab* old = activeTab();
    saveLive(); /* its record stays fresh while it is a hidden live tab */
    if (old)
        old->lastUsed = start; /* shown until now (the LRU order of hidden live tabs) */
    Tab* tab = g_ptr_array_index(tabs, target);
    gboolean live = tab->view != NULL;
    if (!live)
        tab->view = viewForNewlyShownTab(old);
    active = target;
    tab->lastUsed = g_get_monotonic_time();
    showView(tab->view);
    restoring = FALSE;
    pendingScrollSet = FALSE;
    if (live)
        spike_log("[tabs] tab %d shown live in %.1f ms (%u live): %s", target + 1, (g_get_monotonic_time() - start) / 1000.0, liveCount(),
            webkit_web_view_get_uri(tab->view) ? webkit_web_view_get_uri(tab->view) : tab->url);
    else
        activate();
    logLiveTabs(live ? "a switch to a live tab" : "a switch to a discarded tab");
    scheduleSessionSave();
    redraw();
}

/* The tab shown keeps its view (and its page) when another is shown: its
 * scroll position can come later, nothing waits for it (a busy page
 * answered up to 400 ms late, which every switch waited for). */
static gboolean shownTabStaysLive(void)
{
    return liveAllowed() >= 2;
}

static void switchTo(int index)
{
    if (!tabs || index < 0 || index >= (int)tabs->len || index == active || switching)
        return;
    if (shownTabStaysLive()) {
        captureScroll(NULL, NULL);
        switchNow(GINT_TO_POINTER(index));
        return;
    }
    switching = TRUE;
    captureScroll(switchNow, GINT_TO_POINTER(index));
}

typedef struct {
    char* url;
} NewTabJob;

static void newTabNow(gpointer data)
{
    NewTabJob* job = data;
    switching = FALSE;
    ps5wk_vr_stop(FALSE, "new tab");
    Tab* old = activeTab();
    saveLive();
    if (old)
        old->lastUsed = g_get_monotonic_time();
    WebKitWebView* view = viewForNewlyShownTab(old);
    int at = tabs->len ? active + 1 : 0;
    Tab* tab = tabNew(job->url, "");
    tab->view = view;
    tab->lastUsed = g_get_monotonic_time();
    g_ptr_array_insert(tabs, at, tab);
    active = at;
    spike_log("browser: new tab %d of %u: %s (%u live)", active + 1, tabs->len, job->url, liveCount());
    showView(view);
    loadFresh(view, job->url);
    logLiveTabs("a new tab");
    scheduleSessionSave();
    redraw();
    g_free(job->url);
    g_free(job);
}

/* Opens url (NULL: the home page) in a new tab after the active one. */
static gboolean openInNewTab(const char* url)
{
    if (tabs->len >= MAX_TABS) {
        char text[96];
        snprintf(text, sizeof text, "%d tabs is the most: close one first (Options, then Square)", MAX_TABS);
        wpe_display_ps5_show_message(display, text, 3);
        spike_log("browser: new tab refused (%u tabs open)", tabs->len);
        return FALSE;
    }
    if (switching)
        return FALSE;
    NewTabJob* job = g_new0(NewTabJob, 1);
    job->url = url ? g_strdup(url) : ps5wk_home_url();
    if (shownTabStaysLive()) {
        captureScroll(NULL, NULL);
        newTabNow(job);
        return TRUE;
    }
    switching = TRUE;
    captureScroll(newTabNow, job);
    return TRUE;
}

static void closeTab(int index)
{
    if (!tabs || index < 0 || index >= (int)tabs->len || switching)
        return;
    Tab* closing = g_ptr_array_index(tabs, index);
    spike_log("browser: closing tab %d of %u (%s%s)", index + 1, tabs->len, closing->url, closing->view ? ", live" : "");
    if (tabs->len == 1) {
        /* The last tab: it becomes a new tab on the home page. */
        char* home = ps5wk_home_url();
        setString(&closing->url, home);
        setString(&closing->title, "");
        closing->scrollX = closing->scrollY = 0;
        closing->fromForm = FALSE;
        g_clear_pointer(&closing->state, g_bytes_unref);
        loadFresh(closing->view, home);
        g_free(home);
    } else if (index == active) {
        ps5wk_vr_stop(FALSE, "tab closed");
        WebKitWebView* freed = closing->view;
        closing->view = NULL;
        g_ptr_array_remove_index(tabs, (unsigned)index);
        active = index < (int)tabs->len ? index : (int)tabs->len - 1;
        Tab* tab = activeTab();
        tab->lastUsed = g_get_monotonic_time();
        if (tab->view) {
            /* Live: shown at once; the closed tab's view goes. */
            showView(tab->view);
            destroyView(freed);
        } else {
            /* The closed tab's view takes the next tab's page. */
            tab->view = freed;
            activate();
        }
    } else {
        WebKitWebView* freed = closing->view;
        closing->view = NULL;
        g_ptr_array_remove_index(tabs, (unsigned)index);
        if (index < active)
            active--;
        destroyView(freed);
    }
    logLiveTabs("a tab closed");
    scheduleSessionSave();
    redraw();
}

/* Memory: hidden live tabs go when the WebProcess's libpas memory nears its
 * limit (60% of PS5WK_MEM_RESTART_MB, default 3 GB: the limit itself restarts
 * the WebProcess, every page in it) or the title's free flexible memory runs
 * low; no new live tabs until well under (45%). */
size_t ps5wk_title_flexible_free(void) __attribute__((weak));

static gboolean memoryCheck(gpointer data)
{
    (void)data;
    int webPid = 0;
    double committed = 0, limit = 0;
    ps5wk_webmem_over(&webPid, &committed, &limit);
    double flexibleMB = ps5wk_title_flexible_free ? ps5wk_title_flexible_free() / 1048576.0 : -1;
    gboolean webHigh = limit > 0 && committed > limit * 0.6;
    gboolean titleLow = flexibleMB >= 0 && flexibleMB < 96;
    if ((webHigh || titleLow) && liveCount() > 1) {
        char why[160];
        snprintf(why, sizeof why, "memory: WebProcess %.0f of %.0f MB, title %.0f MB free", committed, limit, flexibleMB);
        discardHidden(why);
        logLiveTabs("memory pressure");
        redraw();
    }
    gboolean tight = webHigh || titleLow || (memoryTight && limit > 0 && committed > limit * 0.45);
    if (tight != memoryTight) {
        memoryTight = tight;
        spike_log("[tabs] memory %s: %u live tab%s allowed (WebProcess %.0f of %.0f MB, title %.0f MB free)", tight ? "tight" : "fine again",
            liveAllowed(), liveAllowed() == 1 ? "" : "s", committed, limit, flexibleMB);
    }
    return G_SOURCE_CONTINUE;
}

/* GPU compositing: the title's GL textures over their cap (~1 s average)
 * with hidden live tabs' scenes among them (patches 0072, 0073): the least
 * recently used one is discarded before the shown page loses tile coverage. Weak: the
 * CPU-only build has no WC host. */
void ps5wk_wc_set_hidden_pages_callback(void (*callback)(double mb)) __attribute__((weak));

static void onGLPressure(double mb)
{
    char why[96];
    snprintf(why, sizeof why, "GL textures %.0f MB, over the cap", mb);
    /* The least recently used one; the host asks again 5 s later if the
     * textures are still over. */
    if (lruHidden())
        discardTab(lruHidden(), why, FALSE);
    logLiveTabs("GL texture pressure");
    redraw();
}

/* ---------------------------------------------------------------- session */

static void sessionWrite(gpointer data)
{
    (void)data;
    if (sessionFrozen && !GPOINTER_TO_INT(data))
        return;
    GPtrArray* list = g_ptr_array_new_with_free_func(ps5wk_session_tab_free);
    for (unsigned i = 0; i < tabs->len; i++) {
        Tab* tab = g_ptr_array_index(tabs, i);
        PS5WKSessionTab* saved = g_new0(PS5WKSessionTab, 1);
        saved->url = g_strdup(tab->url);
        saved->title = g_strdup(tab->title);
        saved->scrollX = tab->scrollX;
        saved->scrollY = tab->scrollY;
        saved->fromForm = tab->fromForm;
        if (tab->view) {
            const char* uri = webkit_web_view_get_uri(tab->view);
            if (uri && *uri && strcmp(uri, "about:blank")) {
                g_free(saved->url);
                saved->url = g_strdup(uri);
            }
            saved->state = captureState(tab->view);
        } else if (tab->state)
            saved->state = g_bytes_ref(tab->state);
        g_ptr_array_add(list, saved);
    }
    ps5wk_session_save(list, active);
    g_ptr_array_unref(list);
}

static void sessionSaveWithScroll(gpointer data)
{
    sessionWrite(data);
}

static gboolean sessionTimerFired(gpointer data)
{
    (void)data;
    sessionTimer = 0;
    if (!sessionFrozen && !switching)
        captureScroll(sessionSaveWithScroll, GINT_TO_POINTER(0));
    else if (switching)
        scheduleSessionSave();
    return G_SOURCE_REMOVE;
}

static void scheduleSessionSave(void)
{
    if (sessionFrozen || sessionTimer)
        return;
    sessionTimer = g_timeout_add(SESSION_DELAY_MS, sessionTimerFired, NULL);
}

void ps5wk_browser_save(void)
{
    if (!tabs || sessionFrozen)
        return;
    if (sessionTimer) {
        g_source_remove(sessionTimer);
        sessionTimer = 0;
    }
    sessionWrite(GINT_TO_POINTER(1));
    ps5wk_bdata_flush();
    sessionFrozen = TRUE; /* what follows (about:blank of the wind-down) is not the session */
    spike_log("browser: session saved (%u tabs, tab %d shown)", tabs->len, active + 1);
}

/* ---------------------------------------------------------------- drawing */

/* browserdraw.c draws (the palette and sizes in brand.h); here the state it
 * draws from. */

static WPEDisplayPS5Canvas* menuCanvas;
static WPEDisplayPS5Canvas* barCanvas;
static gboolean menuOpen;
static gboolean barShown;
static guint barHideTimer;
static guint redrawSource;
static int zone = ZONE_TOOLBAR;
static int toolbarFocus = TB_ADDRESS;
static int section = SEC_TABS;
static int listFocus;
static int listTop;
static double scrollAccumulator;
static gboolean clearArmed; /* "Clear history" pressed once */
static int screenW = 1920, screenH = 1080;
static gint64 lastBarDraw;
/* What drawing the menu costs (main thread), logged when it closes. */
static unsigned drawCount;
static gint64 drawTotalUS, drawMaxUS;

#define ROW_H bui_row_height()

static int rowCount(int sec)
{
    switch (sec) {
    case SEC_TABS:
        return (int)tabs->len + (tabs->len < MAX_TABS ? 1 : 0);
    case SEC_BOOKMARKS:
        return (int)ps5wk_bookmarks()->len;
    case SEC_HISTORY:
        return ps5wk_history()->len ? (int)ps5wk_history()->len + 1 : 0; /* row 0: clear history */
    }
    return 0;
}

static gboolean canGoBack(void)
{
    WebKitBackForwardList* list = webkit_web_view_get_back_forward_list(webView);
    WebKitBackForwardListItem* back = webkit_back_forward_list_get_back_item(list);
    return back && strcmp(webkit_back_forward_list_item_get_uri(back), "about:blank");
}

static const char* shownURI(void)
{
    const char* uri = webkit_web_view_get_uri(webView);
    if (!uri || !*uri || !strcmp(uri, "about:blank")) {
        Tab* tab = activeTab();
        uri = tab ? tab->url : "";
    }
    return uri;
}

static const char* shownTitle(void)
{
    const char* title = webkit_web_view_get_title(webView);
    if (!title || !*title) {
        Tab* tab = activeTab();
        title = tab && *tab->title ? tab->title : "";
    }
    return title;
}

/* A row of the list shown (BUIState.row). */
static void listRow(int row, BUIRow* r, void* data)
{
    (void)data;
    if (section == SEC_TABS) {
        if (row >= (int)tabs->len) {
            r->title = "New tab";
            r->url = "Opens the home page";
            r->action = TRUE;
            return;
        }
        Tab* tab = g_ptr_array_index(tabs, row);
        r->current = row == active;
        r->title = r->current ? shownTitle() : tab->title;
        r->url = r->current ? shownURI() : tab->url;
        if (!*r->title)
            r->title = r->url;
        r->number = row + 1;
        /* a hidden tab whose page is still live: back at once */
        if (!r->current && tab->view)
            snprintf(r->note, sizeof r->note, "kept live");
    } else if (section == SEC_BOOKMARKS) {
        PS5WKEntry* e = g_ptr_array_index(ps5wk_bookmarks(), row);
        r->title = *e->title ? e->title : e->url;
        r->url = e->url;
    } else {
        if (row == 0) {
            r->title = clearArmed ? "Press X again to clear all history" : "Clear all history";
            r->url = "Bookmarks and open tabs stay";
            r->action = TRUE;
            r->armed = clearArmed;
            return;
        }
        PS5WKEntry* e = g_ptr_array_index(ps5wk_history(), row - 1);
        r->title = *e->title ? e->title : e->url;
        r->url = e->url;
        GDateTime* time = g_date_time_new_from_unix_local(e->time);
        char* when = time ? g_date_time_format(time, "%d %b %H:%M") : NULL;
        snprintf(r->note, sizeof r->note, "%s", when ? when : "");
        g_free(when);
        if (time)
            g_date_time_unref(time);
    }
}

static void fillState(BUIState* s)
{
    memset(s, 0, sizeof *s);
    s->screenW = screenW;
    s->screenH = screenH;
    s->loading = webkit_web_view_is_loading(webView);
    s->progress = webkit_web_view_get_estimated_load_progress(webView);
    s->canBack = canGoBack();
    s->canForward = webkit_web_view_can_go_forward(webView);
    s->canNewTab = tabs->len < MAX_TABS;
    s->starred = ps5wk_bookmark_find(shownURI()) >= 0;
    s->title = shownTitle();
    s->uri = shownURI();
    s->tabCount = tabs->len;
    s->maxTabs = MAX_TABS;
    s->zone = zone;
    s->toolbarFocus = toolbarFocus;
    s->section = section;
    s->bookmarkCount = ps5wk_bookmarks()->len;
    s->historyCount = ps5wk_history()->len;
    s->rowCount = rowCount(section);
    s->listTop = listTop;
    s->listFocus = listFocus;
    s->row = listRow;
}

static int visibleRows(void)
{
    return bui_visible_rows(screenH);
}

static void keepFocusVisible(void)
{
    int rows = rowCount(section), visible = visibleRows();
    if (listFocus >= rows)
        listFocus = rows - 1;
    if (listFocus < 0)
        listFocus = 0;
    if (listFocus < listTop)
        listTop = listFocus;
    if (listFocus >= listTop + visible)
        listTop = listFocus - visible + 1;
    if (listTop > rows - visible)
        listTop = rows - visible;
    if (listTop < 0)
        listTop = 0;
}

static void redrawNow(void)
{
    if (menuOpen) {
        if (!menuCanvas)
            menuCanvas = wpe_display_ps5_canvas_new(screenW, screenH);
        if (!menuCanvas)
            return;
        BUIState state;
        fillState(&state);
        gint64 start = g_get_monotonic_time();
        bui_draw_menu(menuCanvas, &state);
        gint64 drawn = g_get_monotonic_time();
        wpe_display_ps5_set_ui_layer(display, menuCanvas, 0, 0);
        gint64 us = g_get_monotonic_time() - start;
        drawCount++;
        drawTotalUS += us;
        if (us > drawMaxUS)
            drawMaxUS = us;
        if (drawCount == 1)
            spike_log("browser: menu drawn in %.1f ms (+ %.1f ms to hand it over)", (drawn - start) / 1000.0, (us - (drawn - start)) / 1000.0);
        return;
    }
    if (menuCanvas) {
        wpe_display_ps5_canvas_free(menuCanvas);
        menuCanvas = NULL;
    }
    if (barShown && !ps5wk_vr_active()) {
        if (!barCanvas)
            barCanvas = wpe_display_ps5_canvas_new(screenW, bui_bar_height());
        if (!barCanvas)
            return;
        BUIState state;
        fillState(&state);
        wpe_display_ps5_canvas_clear(barCanvas, 0);
        bui_draw_bar(barCanvas, &state, FALSE);
        wpe_display_ps5_set_ui_layer(display, barCanvas, 0, 0);
        lastBarDraw = g_get_monotonic_time();
        return;
    }
    if (barCanvas) {
        wpe_display_ps5_canvas_free(barCanvas);
        barCanvas = NULL;
    }
    wpe_display_ps5_set_ui_layer(display, NULL, 0, 0);
}

static gboolean redrawIdle(gpointer data)
{
    (void)data;
    redrawSource = 0;
    redrawNow();
    return G_SOURCE_REMOVE;
}

static void redraw(void)
{
    if (!redrawSource)
        redrawSource = g_idle_add(redrawIdle, NULL);
}

/* The bar while loading: at most ~12 redraws a second for progress. */
static gboolean barRedrawDue(gpointer data)
{
    (void)data;
    redrawSource = 0;
    redrawNow();
    return G_SOURCE_REMOVE;
}

static void progressRedraw(void)
{
    if (redrawSource || (!menuOpen && !barShown))
        return;
    gint64 since = (g_get_monotonic_time() - lastBarDraw) / 1000;
    if (menuOpen || since >= 80)
        redraw();
    else
        redrawSource = g_timeout_add((guint)(80 - since), barRedrawDue, NULL);
}

static gboolean barHide(gpointer data)
{
    (void)data;
    barHideTimer = 0;
    barShown = FALSE;
    redraw();
    return G_SOURCE_REMOVE;
}

static gboolean loadBarWanted(void)
{
    static int wanted = -1;
    if (wanted < 0) {
        int fd = open(NO_LOAD_BAR_FLAG, O_RDONLY);
        wanted = fd < 0;
        if (fd >= 0)
            close(fd);
    }
    return wanted;
}

static void showBar(void)
{
    if (!loadBarWanted() || ps5wk_vr_active())
        return;
    if (barHideTimer) {
        g_source_remove(barHideTimer);
        barHideTimer = 0;
    }
    if (!barShown) {
        barShown = TRUE;
        redraw();
    }
}

static void hideBarSoon(void)
{
    if (barShown && !barHideTimer)
        barHideTimer = g_timeout_add(BAR_HIDE_MS, barHide, NULL);
}

/* ---------------------------------------------------------------- the menu */

static void setMenu(gboolean open)
{
    if (menuOpen == open)
        return;
    menuOpen = open;
    wpe_display_ps5_set_ui_mode(display, open);
    if (open) {
        zone = ZONE_TOOLBAR;
        toolbarFocus = TB_ADDRESS;
        clearArmed = FALSE;
        section = SEC_TABS;
        listFocus = active;
        listTop = 0;
        keepFocusVisible();
        drawCount = 0;
        drawTotalUS = drawMaxUS = 0;
        spike_log("browser: menu open");
    } else if (drawCount)
        spike_log("browser: menu closed (drawn %u times, %.1f ms on average, %.1f ms at most)", drawCount,
            drawTotalUS / 1000.0 / drawCount, drawMaxUS / 1000.0);
    else
        spike_log("browser: menu closed");
    redraw();
}

void ps5wk_browser_close_menu(void)
{
    setMenu(FALSE);
}

gboolean ps5wk_browser_menu_open(void)
{
    return menuOpen;
}

void ps5wk_browser_toggle_menu(void)
{
    if (ps5wk_vr_active())
        return;
    setMenu(!menuOpen);
}

static void loadInTab(const char* uri)
{
    spike_log("browser: loading %s in tab %d", uri, active + 1);
    restoring = FALSE;
    pendingScrollSet = FALSE;
    webkit_web_view_load_uri(webView, uri);
}

static void onAddressEntered(const char* text, gpointer data)
{
    (void)data;
    if (!text) {
        spike_log("browser: address keyboard cancelled");
        return;
    }
    char* uri = ps5wk_uri_from_input(text);
    if (!uri)
        return;
    setMenu(FALSE);
    loadInTab(uri);
    g_free(uri);
}

static void editAddress(void)
{
    const char* current = shownURI();
    if (!strncmp(current, "file:", 5) || !strncmp(current, "about:", 6) || !strncmp(current, "data:", 5))
        current = "";
    if (!wpe_display_ps5_request_text(display, "Address or search", current, TRUE, onAddressEntered, NULL))
        spike_log("browser: address: no system keyboard");
}

static void toolbarActivate(int index)
{
    switch (index) {
    case TB_BACK:
        if (canGoBack()) {
            webkit_web_view_go_back(webView);
            setMenu(FALSE);
        }
        break;
    case TB_FORWARD:
        if (webkit_web_view_can_go_forward(webView)) {
            webkit_web_view_go_forward(webView);
            setMenu(FALSE);
        }
        break;
    case TB_RELOAD:
        if (webkit_web_view_is_loading(webView))
            webkit_web_view_stop_loading(webView);
        else {
            webkit_web_view_reload(webView);
            setMenu(FALSE);
        }
        break;
    case TB_HOME: {
        char* home = ps5wk_home_url();
        setMenu(FALSE);
        loadInTab(home);
        g_free(home);
        break;
    }
    case TB_ADDRESS:
        editAddress();
        break;
    case TB_STAR: {
        const char* uri = shownURI();
        if (!*uri || !strncmp(uri, "about:", 6))
            break;
        gboolean on = ps5wk_bookmark_toggle(uri, shownTitle());
        spike_log("browser: bookmark %s: %s", on ? "added" : "removed", uri);
        wpe_display_ps5_show_message(display, on ? "Bookmarked" : "Bookmark removed", 1.5);
        redraw();
        break;
    }
    case TB_NEWTAB:
        if (openInNewTab(NULL))
            setMenu(FALSE);
        break;
    }
}

static const char* rowURL(int row)
{
    if (section == SEC_BOOKMARKS && row >= 0 && row < (int)ps5wk_bookmarks()->len)
        return ((PS5WKEntry*)g_ptr_array_index(ps5wk_bookmarks(), row))->url;
    if (section == SEC_HISTORY && row >= 1 && row <= (int)ps5wk_history()->len)
        return ((PS5WKEntry*)g_ptr_array_index(ps5wk_history(), row - 1))->url;
    return NULL;
}

static void rowActivate(int row, gboolean newTab)
{
    if (section == SEC_TABS) {
        if (row >= (int)tabs->len) {
            if (openInNewTab(NULL))
                setMenu(FALSE);
        } else {
            setMenu(FALSE);
            switchTo(row);
        }
        return;
    }
    if (section == SEC_HISTORY && row == 0) {
        if (!clearArmed) {
            clearArmed = TRUE;
        } else {
            ps5wk_history_clear();
            clearArmed = FALSE;
            spike_log("browser: history cleared");
            wpe_display_ps5_show_message(display, "History cleared", 1.5);
        }
        redraw();
        return;
    }
    const char* url = rowURL(row);
    if (!url)
        return;
    char* copy = g_strdup(url);
    if (newTab) {
        if (openInNewTab(copy))
            setMenu(FALSE);
    } else {
        setMenu(FALSE);
        loadInTab(copy);
    }
    g_free(copy);
}

static void rowRemove(int row)
{
    if (section == SEC_TABS) {
        if (row < (int)tabs->len)
            closeTab(row);
    } else if (section == SEC_BOOKMARKS) {
        if (row < (int)ps5wk_bookmarks()->len) {
            spike_log("browser: bookmark removed: %s", rowURL(row));
            ps5wk_bookmark_remove((unsigned)row);
        }
    } else if (row >= 1) {
        ps5wk_history_remove((unsigned)(row - 1));
    }
    keepFocusVisible();
    redraw();
}

static void setSection(int s)
{
    section = (s + SEC_COUNT) % SEC_COUNT;
    listFocus = section == SEC_TABS ? active : 0;
    listTop = 0;
    clearArmed = FALSE;
    keepFocusVisible();
    if (zone == ZONE_LIST && !rowCount(section))
        zone = ZONE_TOOLBAR;
}

static const BUIHot* hotAt(double x, double y)
{
    return bui_hot_at(x, y);
}

/* Focus follows the pointer. */
static gboolean focusHot(const BUIHot* hot)
{
    if (!hot)
        return FALSE;
    if (hot->kind == HOT_TOOLBAR) {
        if (zone == ZONE_TOOLBAR && toolbarFocus == hot->index)
            return FALSE;
        zone = ZONE_TOOLBAR;
        toolbarFocus = hot->index;
    } else if (hot->kind == HOT_ROW) {
        if (zone == ZONE_LIST && listFocus == hot->index)
            return FALSE;
        zone = ZONE_LIST;
        listFocus = hot->index;
    } else
        return FALSE;
    return TRUE;
}

static void onUIPointer(WPEDisplayPS5* d, double x, double y, double scrollY, gpointer data)
{
    (void)d;
    (void)data;
    if (!menuOpen)
        return;
    if (scrollY) {
        scrollAccumulator += scrollY;
        int steps = (int)(scrollAccumulator / ROW_H * 2);
        if (steps) {
            scrollAccumulator = 0;
            int rows = rowCount(section);
            listTop += steps;
            if (listTop > rows - visibleRows())
                listTop = rows - visibleRows();
            if (listTop < 0)
                listTop = 0;
            if (zone == ZONE_LIST) {
                if (listFocus < listTop)
                    listFocus = listTop;
                if (listFocus >= listTop + visibleRows())
                    listFocus = listTop + visibleRows() - 1;
            }
            redraw();
        }
        return;
    }
    if (focusHot(hotAt(x, y)))
        redraw();
}

/* The pad while the menu is open (UI mode: every press comes here). */
static void menuButtons(guint32 pressed)
{
    if (pressed & WPE_DISPLAY_PS5_BUTTON_CROSS) {
        double x, y;
        if (wpe_display_ps5_get_pointer(display, &x, &y)) {
            const BUIHot* hot = hotAt(x, y);
            if (!hot)
                return;
            if (hot->kind == HOT_SECTION) {
                setSection(hot->index);
                redraw();
                return;
            }
            focusHot(hot);
        }
        if (zone == ZONE_TOOLBAR)
            toolbarActivate(toolbarFocus);
        else
            rowActivate(listFocus, FALSE);
        return;
    }
    if (pressed & (WPE_DISPLAY_PS5_BUTTON_CIRCLE | WPE_DISPLAY_PS5_BUTTON_OPTIONS)) {
        setMenu(FALSE);
        return;
    }
    if (pressed & WPE_DISPLAY_PS5_BUTTON_TRIANGLE) {
        zone = ZONE_TOOLBAR;
        toolbarFocus = TB_ADDRESS;
        redraw();
        editAddress();
        return;
    }
    if (pressed & WPE_DISPLAY_PS5_BUTTON_TOUCHPAD) {
        setMenu(FALSE);
        ps5wk_vr_toggle();
        return;
    }
    if (pressed & WPE_DISPLAY_PS5_BUTTON_L3) {
        toolbarActivate(TB_HOME);
        return;
    }
    if (pressed & WPE_DISPLAY_PS5_BUTTON_R3) {
        setMenu(FALSE);
        ps5wk_open_settings();
        return;
    }
    if (pressed & WPE_DISPLAY_PS5_BUTTON_SQUARE) {
        if (zone == ZONE_LIST)
            rowRemove(listFocus);
        return;
    }
    if (pressed & WPE_DISPLAY_PS5_BUTTON_R2) {
        if (zone == ZONE_LIST && section != SEC_TABS)
            rowActivate(listFocus, TRUE);
        return;
    }
    if (pressed & (WPE_DISPLAY_PS5_BUTTON_L1 | WPE_DISPLAY_PS5_BUTTON_R1)) {
        setSection(section + ((pressed & WPE_DISPLAY_PS5_BUTTON_R1) ? 1 : -1));
        redraw();
        return;
    }
    if (zone == ZONE_TOOLBAR) {
        if (pressed & WPE_DISPLAY_PS5_BUTTON_LEFT)
            toolbarFocus = toolbarFocus > 0 ? toolbarFocus - 1 : 0;
        else if (pressed & WPE_DISPLAY_PS5_BUTTON_RIGHT)
            toolbarFocus = toolbarFocus < TB_COUNT - 1 ? toolbarFocus + 1 : TB_COUNT - 1;
        else if ((pressed & WPE_DISPLAY_PS5_BUTTON_DOWN) && rowCount(section)) {
            zone = ZONE_LIST;
            keepFocusVisible();
        }
    } else {
        if (pressed & WPE_DISPLAY_PS5_BUTTON_UP) {
            if (listFocus > 0)
                listFocus--;
            else
                zone = ZONE_TOOLBAR;
        } else if (pressed & WPE_DISPLAY_PS5_BUTTON_DOWN) {
            if (listFocus < rowCount(section) - 1)
                listFocus++;
        } else if (pressed & (WPE_DISPLAY_PS5_BUTTON_LEFT | WPE_DISPLAY_PS5_BUTTON_RIGHT)) {
            setSection(section + ((pressed & WPE_DISPLAY_PS5_BUTTON_RIGHT) ? 1 : -1));
            if (rowCount(section))
                zone = ZONE_LIST;
        }
        if (section == SEC_HISTORY && listFocus != 0)
            clearArmed = FALSE;
        keepFocusVisible();
    }
    redraw();
}

/* The display's button handler: the menu, then View in VR, then Options. */
static gboolean onButtons(WPEDisplayPS5* d, guint32 pressed, gpointer data)
{
    (void)d;
    (void)data;
    if (menuOpen) {
        menuButtons(pressed);
        return TRUE;
    }
    if (ps5wk_vr_buttons(pressed))
        return TRUE;
    if (pressed & WPE_DISPLAY_PS5_BUTTON_OPTIONS) {
        ps5wk_browser_toggle_menu();
        return TRUE;
    }
    return FALSE;
}

/* ---------------------------------------------------------------- the web view */

void ps5wk_browser_go_back(void)
{
    if (canGoBack())
        webkit_web_view_go_back(webView);
}

static void applyPendingScroll(void)
{
    if (!pendingScrollSet)
        return;
    pendingScrollSet = FALSE;
    /* Now, and again while the page is still at the top (it may still be
     * growing) for 3 s; never once it has been scrolled. */
    char* script = g_strdup_printf(
        "(function(x,y){function go(){if(Math.abs(scrollY-y)>4||Math.abs(scrollX-x)>4)scrollTo(x,y);}go();"
        "var n=0,t=setInterval(function(){if(++n>6){clearInterval(t);return;}if(scrollY<2&&y>2)go();},500);})(%.0f,%.0f)",
        pendingScrollX, pendingScrollY);
    webkit_web_view_evaluate_javascript(webView, script, -1, SCROLL_WORLD, NULL, NULL, NULL, NULL);
    spike_log("browser: tab %d: scrolled back to %.0f,%.0f", active + 1, pendingScrollX, pendingScrollY);
    g_free(script);
}

static Tab* tabForView(WebKitWebView* view)
{
    for (unsigned i = 0; tabs && i < tabs->len; i++) {
        Tab* tab = g_ptr_array_index(tabs, i);
        if (tab->view == view)
            return tab;
    }
    return NULL;
}

void ps5wk_browser_load_changed(WebKitWebView* view, WebKitLoadEvent event)
{
    const char* uri = webkit_web_view_get_uri(view);
    if (view != webView) {
        /* A hidden live tab loading by itself (a page's own redirect or
         * reload): its record follows, nothing is shown. */
        Tab* tab = tabForView(view);
        if (tab && event == WEBKIT_LOAD_COMMITTED && uri && *uri && strcmp(uri, "about:blank"))
            setString(&tab->url, uri);
        return;
    }
    switch (event) {
    case WEBKIT_LOAD_STARTED:
        showBar();
        break;
    case WEBKIT_LOAD_COMMITTED: {
        Tab* tab = activeTab();
        if (tab && uri && *uri && strcmp(uri, "about:blank"))
            setString(&tab->url, uri);
        /* A form's result (not a redirect after it): a tab switch must not
         * load it again. */
        if (tab)
            tab->fromForm = uri && ((postURI && !strcmp(postURI, uri)) || (formPlaceholderURI && !strcmp(formPlaceholderURI, uri)));
        g_clear_pointer(&postURI, g_free);
        g_clear_pointer(&formPlaceholderURI, g_free);
        ps5wk_history_visit(uri, webkit_web_view_get_title(view), TRUE);
        scheduleSessionSave();
        break;
    }
    case WEBKIT_LOAD_FINISHED:
        restoring = FALSE;
        applyPendingScroll();
        hideBarSoon();
        scheduleSessionSave();
        break;
    default:
        break;
    }
    redraw();
}

/* The WebProcess ended: every view's page went with it. A hidden live tab
 * goes back to its record (its page comes back when it is picked);
 * ps5browser.c reloads the active one as before. */
static guint discardEndedSource;

static gboolean discardEndedViews(gpointer data)
{
    (void)data;
    discardEndedSource = 0;
    discardHidden("its WebProcess ended");
    redraw();
    return G_SOURCE_REMOVE;
}

/* TRUE: a hidden live tab's view (ps5browser.c then leaves it alone). */
gboolean ps5wk_browser_view_terminated(WebKitWebView* view)
{
    if (view == webView || !tabForView(view))
        return FALSE;
    if (!discardEndedSource)
        discardEndedSource = g_idle_add(discardEndedViews, NULL);
    return TRUE;
}

static void onTitleChanged(GObject* object, GParamSpec* pspec, gpointer data)
{
    (void)pspec;
    (void)data;
    WebKitWebView* view = WEBKIT_WEB_VIEW(object);
    const char* title = webkit_web_view_get_title(view);
    Tab* tab = tabForView(view);
    if (tab && title && *title)
        setString(&tab->title, title);
    if (view != webView)
        return;
    ps5wk_history_set_title(webkit_web_view_get_uri(view), title);
    if (menuOpen || barShown)
        redraw();
}

static void onURIChanged(GObject* object, GParamSpec* pspec, gpointer data)
{
    (void)pspec;
    (void)data;
    WebKitWebView* view = WEBKIT_WEB_VIEW(object);
    /* Same-document navigations (pushState, fragments): no load events. */
    if (webkit_web_view_is_loading(view))
        return;
    const char* uri = webkit_web_view_get_uri(view);
    Tab* tab = tabForView(view);
    if (tab && uri && *uri && strcmp(uri, "about:blank"))
        setString(&tab->url, uri);
    if (view != webView)
        return;
    ps5wk_history_visit(uri, webkit_web_view_get_title(view), TRUE);
    scheduleSessionSave();
    if (menuOpen || barShown)
        redraw();
}

static void onProgress(GObject* object, GParamSpec* pspec, gpointer data)
{
    (void)pspec;
    (void)data;
    if (WEBKIT_WEB_VIEW(object) == webView)
        progressRedraw();
}

/* target=_blank links and window.open: a new tab (a page's own pop-ups,
 * without a click, are dropped). */
static WebKitWebView* onCreate(WebKitWebView* view, WebKitNavigationAction* action, gpointer data)
{
    (void)data;
    if (view != webView)
        return NULL; /* a hidden tab opens nothing */
    WebKitURIRequest* request = webkit_navigation_action_get_request(action);
    const char* uri = request ? webkit_uri_request_get_uri(request) : NULL;
    if (!uri || !*uri || !strcmp(uri, "about:blank")) {
        spike_log("browser: new window without an address: ignored");
        return NULL;
    }
    if (!webkit_navigation_action_is_user_gesture(action)) {
        spike_log("browser: pop-up without a click blocked: %s", uri);
        return NULL;
    }
    if (strncmp(uri, "http://", 7) && strncmp(uri, "https://", 8)) {
        spike_log("browser: new window for %s: not http(s), ignored", uri);
        return NULL;
    }
    spike_log("browser: link for a new window: new tab for %s", uri);
    if (openInNewTab(uri))
        wpe_display_ps5_show_message(display, "Opened in a new tab (Options: tabs)", 2);
    return NULL;
}

static gboolean showFormPage(gpointer data)
{
    (void)data;
    if (!formPageURI)
        return G_SOURCE_REMOVE;
    char* escaped = g_markup_escape_text(formPageURI, -1);
    char* html = g_strdup_printf("<!doctype html><html><head><meta charset=\"utf-8\"><title>%s</title>"
        "<style>body{font-family:sans-serif;background:#01001a;color:#f4f2ff;margin:64px;font-size:26px}"
        "a{color:#f4f2ff;display:inline-block;padding:18px 28px;background:#161440;border-radius:16px;text-decoration:none}"
        "a:focus,a:hover{outline:3px solid #ff5cd6;box-shadow:0 0 24px rgba(109,14,253,.5)}code{color:#b9b5e0;word-break:break-all}</style></head><body>"
        "<h1>This page came from a form</h1><p>Showing it again would send the form again, so the browser didn't.</p>"
        "<p><code>%s</code></p><p><a href=\"%s\" autofocus>Load the address (without the form)</a></p>"
        "<p>O goes back.</p></body></html>", escaped, escaped, escaped);
    webkit_web_view_load_alternate_html(webView, html, formPageURI, NULL);
    g_free(html);
    g_free(escaped);
    /* The placeholder stands for the form result (a later switch shows it again). */
    g_free(formPlaceholderURI);
    formPlaceholderURI = formPageURI;
    formPageURI = NULL;
    return G_SOURCE_REMOVE;
}

/* Notes which pages are form (POST) results, and a tab coming back never
 * sends a form again. */
static gboolean onDecidePolicy(WebKitWebView* view, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer data)
{
    (void)data;
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION || view != webView)
        return FALSE;
    WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    WebKitURIRequest* request = webkit_navigation_action_get_request(action);
    const char* method = request ? webkit_uri_request_get_http_method(request) : NULL;
    if (!restoring) {
        if (method && !g_ascii_strcasecmp(method, "POST")) {
            g_free(postURI);
            postURI = g_strdup(webkit_uri_request_get_uri(request));
        }
        return FALSE;
    }
    gboolean resubmit = webkit_navigation_action_get_navigation_type(action) == WEBKIT_NAVIGATION_TYPE_FORM_RESUBMITTED
        || (method && !g_ascii_strcasecmp(method, "POST"));
    if (!resubmit)
        return FALSE;
    const char* uri = webkit_uri_request_get_uri(request);
    spike_log("browser: tab %d: %s came from a form: not sent again, offering to load the address", active + 1, uri);
    webkit_policy_decision_ignore(decision);
    restoring = FALSE;
    pendingScrollSet = FALSE;
    g_free(formPageURI);
    formPageURI = g_strdup(uri);
    g_idle_add(showFormPage, NULL);
    return TRUE;
}

/* ---------------------------------------------------------------- start */

/* ps5browser.c: its handlers on a new view (load events, crashes, keys). */
void ps5wk_connect_web_view(WebKitWebView* view);

/* The browser's own handlers on a view (data &tabs: destroyView takes them
 * off by it). */
static void connectView(WebKitWebView* view)
{
    g_signal_connect(view, "notify::title", G_CALLBACK(onTitleChanged), &tabs);
    g_signal_connect(view, "notify::uri", G_CALLBACK(onURIChanged), &tabs);
    g_signal_connect(view, "notify::estimated-load-progress", G_CALLBACK(onProgress), &tabs);
    g_signal_connect(view, "notify::is-loading", G_CALLBACK(onProgress), &tabs);
    g_signal_connect(view, "create", G_CALLBACK(onCreate), &tabs);
    g_signal_connect(view, "decide-policy", G_CALLBACK(onDecidePolicy), &tabs);
    if (view != webView)
        ps5wk_connect_web_view(view);
}

static unsigned readLiveLimit(void)
{
    unsigned limit = 2;
    gchar* text = NULL;
    if (g_file_get_contents(DATA_DIR "/live-tabs", &text, NULL, NULL)) {
        int value = atoi(g_strstrip(text));
        if (value >= 1 && value <= 4)
            limit = (unsigned)value;
        g_free(text);
    }
    return limit;
}

void ps5wk_browser_init(WPEDisplay* d, WebKitWebView* view)
{
    display = WPE_DISPLAY_PS5(d);
    webView = view;
    tabs = g_ptr_array_new_with_free_func(tabFree);
    liveLimit = readLiveLimit();
    spike_log("[tabs] up to %u live tab%s (/data/ps5webkit/live-tabs: 1-4, default 2)", liveLimit, liveLimit == 1 ? "" : "s");
    memoryTimer = g_timeout_add_seconds(5, memoryCheck, NULL);
    if (ps5wk_wc_set_hidden_pages_callback)
        ps5wk_wc_set_hidden_pages_callback(onGLPressure);
    ps5wk_bdata_init(DATA_DIR);
    WPEView* wpeView = webkit_web_view_get_wpe_view(view);
    if (wpeView && wpe_view_get_width(wpeView) > 0) {
        screenW = wpe_view_get_width(wpeView);
        screenH = wpe_view_get_height(wpeView);
    }
    wpe_display_ps5_set_button_handler(display, onButtons, NULL);
    wpe_display_ps5_set_ui_pointer_callback(display, onUIPointer, NULL);
    connectView(view);
}

static gboolean fileExists(const char* path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return FALSE;
    close(fd);
    return TRUE;
}

void ps5wk_browser_start(const char* startURI, const char* homeURI, gboolean testRun)
{
    if (testRun) {
        /* Test URL lists drive the view themselves; the owner's session stays. */
        sessionFrozen = TRUE;
        Tab* only = tabNew(homeURI, "");
        only->view = webView;
        g_ptr_array_add(tabs, only);
        active = 0;
        spike_log("browser: test run: one tab, the saved session is neither restored nor written");
        return;
    }
    int savedActive = 0;
    GPtrArray* saved = fileExists(NO_RESTORE_FLAG) ? NULL : ps5wk_session_load(&savedActive);
    for (unsigned i = 0; saved && i < saved->len && tabs->len < MAX_TABS; i++) {
        PS5WKSessionTab* s = g_ptr_array_index(saved, i);
        Tab* tab = tabNew(s->url, s->title);
        tab->scrollX = s->scrollX;
        tab->scrollY = s->scrollY;
        tab->fromForm = s->fromForm;
        tab->state = s->state ? g_bytes_ref(s->state) : NULL;
        g_ptr_array_add(tabs, tab);
    }
    if (saved)
        g_ptr_array_unref(saved);
    if (savedActive >= (int)tabs->len)
        savedActive = 0;
    if (!tabs->len) {
        const char* uri = startURI ? startURI : homeURI;
        Tab* first = tabNew(uri, "");
        first->view = webView;
        first->lastUsed = g_get_monotonic_time();
        g_ptr_array_add(tabs, first);
        active = 0;
        spike_log("browser: no saved tabs; loading %s", uri);
        webkit_web_view_load_uri(webView, uri);
        return;
    }
    active = savedActive;
    Tab* shown = activeTab();
    shown->view = webView; /* the others come back when picked */
    shown->lastUsed = g_get_monotonic_time();
    spike_log("browser: %u saved tab%s, tab %d shown (%s)", tabs->len, tabs->len == 1 ? "" : "s", active + 1, shown->url);
    if (startURI && strcmp(startURI, shown->url) && tabs->len < MAX_TABS) {
        /* A link sent while the browser was closed (ps5open, Restart browser
         * with another page): a new tab after the saved ones. */
        Tab* tab = tabNew(startURI, "");
        g_ptr_array_add(tabs, tab);
        shown->view = NULL;
        tab->view = webView;
        tab->lastUsed = g_get_monotonic_time();
        active = (int)tabs->len - 1;
        spike_log("browser: start link in a new tab %d: %s", active + 1, startURI);
        loadFresh(webView, startURI);
    } else if (startURI && strcmp(startURI, shown->url)) {
        spike_log("browser: start link in the shown tab (%d tabs is the most): %s", MAX_TABS, startURI);
        loadFresh(webView, startURI);
    } else
        activate();
    scheduleSessionSave();
}

/* ---------------------------------------------------------------- /browser API */

static char* stateJSON(void)
{
    GString* out = g_string_new("{\"ok\":true,\"tabs\":[");
    for (unsigned i = 0; i < tabs->len; i++) {
        Tab* tab = g_ptr_array_index(tabs, i);
        gboolean live = (int)i == active;
        g_string_append(out, i ? ",{\"live\":" : "{\"live\":");
        g_string_append(out, tab->view ? "true" : "false");
        g_string_append(out, ",\"url\":");
        ps5wk_json_string(out, live ? shownURI() : tab->url);
        g_string_append(out, ",\"title\":");
        ps5wk_json_string(out, live ? shownTitle() : tab->title);
        g_string_append_printf(out, ",\"active\":%s,\"scrollX\":%.0f,\"scrollY\":%.0f,\"savedState\":%zu}", live ? "true" : "false",
            tab->scrollX, tab->scrollY, tab->state ? g_bytes_get_size(tab->state) : (size_t)0);
    }
    WebKitBackForwardList* list = webkit_web_view_get_back_forward_list(webView);
    g_string_append_printf(out, "],\"active\":%d,\"maxTabs\":%d,\"menu\":%s,\"bar\":%s,\"section\":%d,\"zone\":%d,\"toolbarFocus\":%d,"
        "\"listFocus\":%d,\"canGoBack\":%s,\"canGoForward\":%s,\"backForwardLength\":%u,\"loading\":%s,\"progress\":%.2f,"
        "\"liveLimit\":%u,\"liveAllowed\":%u,\"liveCount\":%u,\"memoryTight\":%s,\"bookmarks\":",
        active, MAX_TABS, menuOpen ? "true" : "false", barShown ? "true" : "false", section, zone, toolbarFocus, listFocus,
        canGoBack() ? "true" : "false", webkit_web_view_can_go_forward(webView) ? "true" : "false",
        webkit_back_forward_list_get_length(list), webkit_web_view_is_loading(webView) ? "true" : "false",
        webkit_web_view_get_estimated_load_progress(webView), liveLimit, liveAllowed(), liveCount(), memoryTight ? "true" : "false");
    ps5wk_entries_json(out, ps5wk_bookmarks(), 500);
    g_string_append_printf(out, ",\"historyLength\":%u,\"history\":", ps5wk_history()->len);
    ps5wk_entries_json(out, ps5wk_history(), 50);
    g_string_append(out, "}\n");
    return g_string_free(out, FALSE);
}

static const struct {
    const char* name;
    guint32 button;
} buttonNames[] = {
    { "options", WPE_DISPLAY_PS5_BUTTON_OPTIONS }, { "up", WPE_DISPLAY_PS5_BUTTON_UP }, { "down", WPE_DISPLAY_PS5_BUTTON_DOWN },
    { "left", WPE_DISPLAY_PS5_BUTTON_LEFT }, { "right", WPE_DISPLAY_PS5_BUTTON_RIGHT }, { "cross", WPE_DISPLAY_PS5_BUTTON_CROSS },
    { "x", WPE_DISPLAY_PS5_BUTTON_CROSS }, { "circle", WPE_DISPLAY_PS5_BUTTON_CIRCLE }, { "o", WPE_DISPLAY_PS5_BUTTON_CIRCLE },
    { "square", WPE_DISPLAY_PS5_BUTTON_SQUARE }, { "triangle", WPE_DISPLAY_PS5_BUTTON_TRIANGLE }, { "l1", WPE_DISPLAY_PS5_BUTTON_L1 },
    { "r1", WPE_DISPLAY_PS5_BUTTON_R1 }, { "l2", WPE_DISPLAY_PS5_BUTTON_L2 }, { "r2", WPE_DISPLAY_PS5_BUTTON_R2 },
    { "l3", WPE_DISPLAY_PS5_BUTTON_L3 }, { "r3", WPE_DISPLAY_PS5_BUTTON_R3 },
};

typedef struct {
    char* method;
    char* path;
    char* query;
    char* body;
    char* reply;
    int status;
    gboolean done;
    GMutex lock;
    GCond cond;
} ApiCall;

static char* param(const char* query, const char* body, const char* key)
{
    for (int pass = 0; pass < 2; pass++) {
        const char* source = pass ? body : query;
        if (!source)
            continue;
        size_t keyLength = strlen(key);
        for (const char* p = source; p && *p;) {
            const char* end = strchr(p, '&');
            size_t length = end ? (size_t)(end - p) : strlen(p);
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
            p = end ? end + 1 : NULL;
        }
    }
    return NULL;
}

static char* apiRun(ApiCall* call, int* status)
{
    *status = 200;
    const char* path = call->path;
    gboolean post = !strcmp(call->method, "POST");
    if (!strcmp(path, "/browser") || !strcmp(path, "/browser/state")) {
        return stateJSON();
    }
    if (!post) {
        *status = 405;
        return g_strdup("{\"ok\":false,\"error\":\"POST\"}\n");
    }
    char* action = param(call->query, call->body, "action");
    char* url = param(call->query, call->body, "url");
    char* index = param(call->query, call->body, "index");
    char* name = param(call->query, call->body, "name");
    char* text = param(call->query, call->body, "text");
    char* error = NULL;
    int n = index ? atoi(index) - 1 : -1; /* 1-based in the API, as in the menu */
    if (!strcmp(path, "/browser/tab")) {
        if (action && !strcmp(action, "new")) {
            char* uri = url ? ps5wk_uri_from_input(url) : NULL;
            if (!openInNewTab(uri))
                error = g_strdup("tab not opened (limit, or a switch in progress)");
            g_free(uri);
        } else if (action && !strcmp(action, "switch"))
            n >= 0 && n < (int)tabs->len ? switchTo(n) : (void)(error = g_strdup("index: 1..tabs"));
        else if (action && !strcmp(action, "close"))
            n >= 0 && n < (int)tabs->len ? closeTab(n) : (void)(error = g_strdup("index: 1..tabs"));
        else
            error = g_strdup("action=new|switch|close");
    } else if (!strcmp(path, "/browser/bookmark")) {
        const char* uri = url ? url : shownURI();
        ps5wk_bookmark_toggle(uri, url ? url : shownTitle());
        redraw();
    } else if (!strcmp(path, "/browser/history")) {
        if (action && !strcmp(action, "clear"))
            ps5wk_history_clear();
        else if (action && !strcmp(action, "remove") && n >= 0)
            ps5wk_history_remove((unsigned)n);
        else
            error = g_strdup("action=clear|remove&index=N");
        redraw();
    } else if (!strcmp(path, "/browser/key")) {
        guint32 button = 0;
        for (unsigned i = 0; name && i < G_N_ELEMENTS(buttonNames); i++) {
            if (!strcmp(name, buttonNames[i].name))
                button = buttonNames[i].button;
        }
        if (!button)
            error = g_strdup("name=options|up|down|left|right|x|o|square|triangle|l1|r1|r2|l3|r3");
        else {
            spike_log("browser: simulated %s", name);
            if (!onButtons(display, button, NULL))
                error = g_strdup("not taken by the browser (the page would get it)");
        }
    } else if (!strcmp(path, "/browser/address")) {
        if (!text)
            error = g_strdup("text=<address or search words>");
        else
            onAddressEntered(text, NULL);
    } else if (!strcmp(path, "/browser/save")) {
        gboolean frozen = sessionFrozen;
        sessionWrite(GINT_TO_POINTER(1));
        ps5wk_bdata_flush();
        sessionFrozen = frozen;
    } else if (!strcmp(path, "/browser/shot")) {
        mkdir(DATA_DIR "/shots", 0777);
        char* file = g_strdup_printf(DATA_DIR "/shots/ui-%s.ppm", name && *name ? name : "shot");
        for (char* c = file + strlen(DATA_DIR "/shots/"); *c; c++) {
            if (!g_ascii_isalnum(*c) && *c != '-' && *c != '.' && *c != '_')
                *c = '_';
        }
        wpe_display_ps5_capture_screen(display, file);
        g_free(file);
    } else {
        *status = 404;
        error = g_strdup("not found (/browser/state, tab, bookmark, history, key, address, save, shot)");
    }
    char* reply;
    if (error) {
        if (*status == 200)
            *status = 400;
        GString* out = g_string_new("{\"ok\":false,\"error\":");
        ps5wk_json_string(out, error);
        g_string_append(out, "}\n");
        reply = g_string_free(out, FALSE);
    } else
        reply = g_strdup("{\"ok\":true}\n");
    g_free(action);
    g_free(url);
    g_free(index);
    g_free(name);
    g_free(text);
    g_free(error);
    return reply;
}

static gboolean apiOnMain(gpointer data)
{
    ApiCall* call = data;
    int status = 500;
    char* reply = tabs ? apiRun(call, &status) : g_strdup("{\"ok\":false,\"error\":\"starting\"}\n");
    g_mutex_lock(&call->lock);
    call->reply = reply;
    call->status = status;
    call->done = TRUE;
    g_cond_signal(&call->cond);
    g_mutex_unlock(&call->lock);
    return G_SOURCE_REMOVE;
}

char* ps5wk_browser_api(const char* method, const char* path, const char* query, const char* body, int* status)
{
    ApiCall* call = g_new0(ApiCall, 1);
    call->method = g_strdup(method);
    call->path = g_strdup(path);
    call->query = g_strdup(query ? query : "");
    call->body = g_strdup(body ? body : "");
    g_mutex_init(&call->lock);
    g_cond_init(&call->cond);
    g_main_context_invoke(NULL, apiOnMain, call);
    gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
    g_mutex_lock(&call->lock);
    while (!call->done && g_cond_wait_until(&call->cond, &call->lock, deadline))
        ;
    gboolean done = call->done;
    char* reply = call->reply;
    *status = done ? call->status : 503;
    g_mutex_unlock(&call->lock);
    if (!done) {
        /* The main loop is busy: the call still runs later and leaks its
         * small struct (rare, and better than a use after free). */
        return g_strdup("{\"ok\":false,\"error\":\"the browser is busy\"}\n");
    }
    g_free(call->method);
    g_free(call->path);
    g_free(call->query);
    g_free(call->body);
    g_mutex_clear(&call->lock);
    g_cond_clear(&call->cond);
    g_free(call);
    return reply;
}
