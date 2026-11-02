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

/* JSON helpers for browserui.c's API. */
void ps5wk_json_string(GString* out, const char* s);
void ps5wk_entries_json(GString* out, GPtrArray* entries, unsigned max);

/* ---- browserui.c ---- */

/* After the web view is made (and View in VR's button handler installed:
 * the browser chains it). */
void ps5wk_browser_init(WPEDisplay* display, WebKitWebView* webView);
/* The first page: restores the saved tabs (unless @testRun), then opens
 * @startURI in a new tab, or the home page when nothing was restored. */
void ps5wk_browser_start(const char* startURI, const char* homeURI, gboolean testRun);
/* ps5browser.c's load-changed handler calls this first. */
void ps5wk_browser_load_changed(WebKitLoadEvent event);
/* Back as the pad's O does (not into the empty first entry of a new tab). */
void ps5wk_browser_go_back(void);
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
