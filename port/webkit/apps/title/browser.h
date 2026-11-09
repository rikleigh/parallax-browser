/*
 * Browser basics (ps5browser): tabs, bookmarks, history, the address bar
 * and its menu, session restore. See port/webkit/README.md, "Tabs,
 * bookmarks, history".
 *
 *   browserdata.c  bookmarks.json, history.json, session.json and
 *                  start-data.js under /data/ps5webkit (JSON, written to a
 *                  temporary file and renamed)
 *   browserui.c    the tabs (one WebKitWebView, the others kept as saved
 *                  session states), the title-drawn address bar and menu,
 *                  the pad while the menu is open, the /browser API
 */
#pragma once

#include <glib.h>
#include <wpe/ps5/wpe-ps5.h>
#include <wpe/webkit.h>

/* ---- browserdata.c ---- */

typedef struct {
    char* url;
    char* title;
    gint64 time;     /* Unix seconds: added (bookmark), last visit (history) */
    unsigned visits; /* history */
} PS5WKEntry;

typedef struct {
    char* url;
    char* title;
    double scrollX, scrollY;
    gboolean fromForm; /* the page shown came from a form (POST): not loaded again by itself */
    GBytes* state; /* webkit_web_view_session_state_serialize(), or NULL */
} PS5WKSessionTab;

void ps5wk_bdata_init(const char* dir);
/* Writes what is waiting (the debounce timer's job), e.g. at the wind-down. */
void ps5wk_bdata_flush(void);

/* Newest first; the arrays and entries belong to browserdata.c. */
GPtrArray* ps5wk_bookmarks(void);
int ps5wk_bookmark_find(const char* url);
/* Adds the page, or removes it if it is there; TRUE: now bookmarked. */
gboolean ps5wk_bookmark_toggle(const char* url, const char* title);
void ps5wk_bookmark_remove(unsigned index);

GPtrArray* ps5wk_history(void);
/* A page was shown: moves it to the top (newVisit counts a visit). */
void ps5wk_history_visit(const char* url, const char* title, gboolean newVisit);
void ps5wk_history_set_title(const char* url, const char* title);
void ps5wk_history_remove(unsigned index);
void ps5wk_history_clear(void);
/* Whether a page belongs in the history (http/https only). */
gboolean ps5wk_history_wanted(const char* url);

/* The saved tabs (PS5WKSessionTab*, free with ps5wk_session_tab_free) and
 * the active one; NULL when there is no session file. */
GPtrArray* ps5wk_session_load(int* active);
/* Written at once (temporary file + rename). */
void ps5wk_session_save(GPtrArray* tabs, int active);
void ps5wk_session_tab_free(gpointer tab);

/* Per-site settings (sites.json): gamepad mode and its button mapping
 * (gamepad.c; README: "Gamepad mode"). */
enum {
    PS5WK_GAMEPAD_ASK, /* not decided: offered when the page reads the gamepad */
    PS5WK_GAMEPAD_ON,
    PS5WK_GAMEPAD_OFF, /* declined or switched off: not offered again */
};
typedef struct {
    char* host;   /* lowercase, as in the page's address */
    int gamepad;  /* PS5WK_GAMEPAD_* */
    char* keys;   /* "cross=Space circle=Escape ...", NULL/"": none (gamepad.c) */
    char* left;   /* the sticks: "gamepad", "arrows", "wasd", "pointer", "scroll" */
    char* right;
    gint64 changed; /* Unix seconds */
} PS5WKSite;

/* Most recently changed first (the private session's own list while
 * private); the entries belong to browserdata.c. */
GPtrArray* ps5wk_sites(void);
PS5WKSite* ps5wk_site_find(const char* host);
/* Finds or adds the host's entry (call ps5wk_site_changed after changing it). */
PS5WKSite* ps5wk_site_get(const char* host);
void ps5wk_site_changed(PS5WKSite* site);
gboolean ps5wk_site_remove(const char* host);
const char* ps5wk_site_gamepad_name(int gamepad);
/* Private mode: from now on site settings live in a copy that is dropped
 * when it ends (FALSE). */
void ps5wk_sites_set_private(gboolean enabled);

/* Downloads (downloads.json): the list downloads.c keeps, newest first. */
typedef enum {
    PS5WK_DL_RUNNING, /* receiving, or about to (a retry, a resume asked) */
    PS5WK_DL_PAUSED,  /* the .part file stays; Resume continues it */
    PS5WK_DL_FAILED,  /* error says why; the .part file stays (Retry) */
    PS5WK_DL_DONE,
    PS5WK_DL_CANCELLED, /* the .part file is gone */
} PS5WKDownloadState;

typedef struct {
    unsigned id;
    char* url;      /* the original address, before redirects: a resume asks it again */
    char* finalURL; /* where the data came from last (after redirects) */
    char* file;     /* the name in the downloads folder (the .part has ".part" after it) */
    char* mime;
    gint64 size;     /* total bytes, -1: not known */
    gint64 received; /* bytes in the file or its .part */
    int state;       /* PS5WKDownloadState */
    char* etag;      /* the validator for If-Range */
    char* lastModified;
    gint64 started, finished; /* Unix seconds (finished: 0 until done) */
    char* error;     /* why it failed, or a note ("started over from zero") */
    char* sha256;    /* lowercase hex, once the finished file was read */
    gboolean restarted; /* the last resume started over from zero */
} PS5WKDownloadRecord;

/* downloads.json: the records (PS5WKDownloadRecord*, free with
 * ps5wk_download_record_free), or an empty array. */
GPtrArray* ps5wk_downloads_load(void);
void ps5wk_downloads_save(GPtrArray* records);
void ps5wk_download_record_clear(PS5WKDownloadRecord* record);
void ps5wk_download_record_free(gpointer record);
const char* ps5wk_download_state_name(int state);

/* JSON helpers for browserui.c's API. */
void ps5wk_json_string(GString* out, const char* s);
void ps5wk_entries_json(GString* out, GPtrArray* entries, unsigned max);

/* ---- browserui.c ---- */

/* ps5browser.c, for browserui.c: the active tab's view changed (load
 * handlers, View in VR, the ad blocker follow it); a new tab's view gets
 * ps5browser.c's handlers. */
void ps5wk_set_active_web_view(WebKitWebView* view);
void ps5wk_connect_web_view(WebKitWebView* view);

/* After the web view is made (and View in VR's button handler installed:
 * the browser chains it). */
void ps5wk_browser_init(WPEDisplay* display, WebKitWebView* webView);
/* The first page: restores the saved tabs (unless @testRun), then opens
 * @startURI in a new tab, or the home page when nothing was restored. */
void ps5wk_browser_start(const char* startURI, const char* homeURI, gboolean testRun);
/* ps5browser.c's load-changed handler calls this first (any tab's view). */
void ps5wk_browser_load_changed(WebKitWebView* view, WebKitLoadEvent event);
/* A view's WebProcess ended: TRUE when it is a hidden live tab's (it goes
 * back to its record; nothing to reload). */
gboolean ps5wk_browser_view_terminated(WebKitWebView* view);
/* Hidden live tabs give up their pages (before the WebProcess restarts). */
void ps5wk_browser_discard_hidden(const char* why);
/* Back as the pad's O does (not into the empty first entry of a new tab;
 * a browser page tab with nothing to go back to closes). */
void ps5wk_browser_go_back(void);
/* The browser's own pages (Settings, button mapping) open in a browser page
 * tab over the tab shown: one at a time, not one of the tabs the limit
 * counts. FALSE: not now (a tab switch in progress). */
gboolean ps5wk_browser_open_page(const char* url);
/* Closes the browser page tab shown, back to the tab it was opened over
 * (also POST /browser/page action=close). FALSE: the tab shown isn't one. */
gboolean ps5wk_browser_close_page(void);
/* The mapping page for the site shown (the menu's gamepad button's □). */
void ps5wk_browser_open_gamepad_page(void);
/* Options: the menu (with the address field focused). */
void ps5wk_browser_toggle_menu(void);
gboolean ps5wk_browser_menu_open(void);
/* Closes the menu (e.g. View in VR started, the browser winds down). */
void ps5wk_browser_close_menu(void);
/* The session file now (wind-down). */
void ps5wk_browser_save(void);
/* The /browser API (link server thread); NULL path = not ours. Returns a
 * malloc'd JSON reply and sets *status. */
char* ps5wk_browser_api(const char* method, const char* path, const char* query, const char* body, int* status);

/* ---- gamepad.c (README: "Gamepad mode") ---- */

void ps5wk_gamepad_init(WPEDisplayPS5* display);
/* The page shown (a committed load, a tab switch): applies its site's mode. */
void ps5wk_gamepad_page(WebKitWebView* view, const char* uri);
/* The offer's △ and ○ while it shows (before the browser's own); TRUE: taken. */
gboolean ps5wk_gamepad_buttons(guint32 pressed);
/* The menu's gamepad button: on/off for the site shown. */
void ps5wk_gamepad_toggle(void);
/* Options held in gamepad mode: off for the site shown. */
void ps5wk_gamepad_leave(void);
gboolean ps5wk_gamepad_active(void);
/* The site shown ("file:" for local files), NULL for none. */
const char* ps5wk_gamepad_site(void);
/* /browser/gamepad: GET the state, POST site=&gamepad=&preset=&keys=&left=&right=&remove=1. */
char* ps5wk_gamepad_api(gboolean post, const char* site, const char* gamepad, const char* preset, const char* keys,
    const char* left, const char* right, gboolean remove, int* status);
/* The mapping page (GET /gamepad on the link server). */
extern const char ps5wk_gamepad_page_html[];
#define PS5WK_GAMEPAD_URL "parallax://gamepad"
#define PS5WK_SETTINGS_URL_PREFIX "parallax://settings"
#define PS5WK_START_URL "parallax://start"
/* The start page before parallax://start (ps5browser.c): saved tabs, history
 * and home-url.txt may still have it; it opens parallax://start instead. */
#define PS5WK_LEGACY_START_URL "file:///data/ps5webkit/start.html"

/* titlelog.c: while on, the log takes no lines (private browsing). */
void ps5wk_log_private(int on);

/* Private browsing (browserui.c): TRUE while the private session is shown. */
gboolean ps5wk_browser_private(void);
