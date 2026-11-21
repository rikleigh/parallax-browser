/*
 * Downloads (ps5browser): files saved to the PS5 user's downloads folder
 * (/data/ps5webkit/users/<id>/downloads, profile.c) through
 * WebKit's download path (the NetworkProcess streams them to disk), the
 * list in downloads.json, pause, resume (patch 0075: a Range request for
 * the original address, appended to the .part file on a matching 206),
 * automatic retries, SHA-256 of finished files. See downloads.c and
 * port/webkit/README.md ("Downloads").
 *
 * Main thread, except the functions marked "any thread" (the link server).
 */
#pragma once

#include "browserdraw.h"
#include "profile.h"

#include <glib.h>
#include <wpe/ps5/wpe-ps5.h>
#include <wpe/webkit.h>

#define PS5WK_DOWNLOADS_DIR ps5wk_profile_downloads_dir()

/* At start, after browserui.c's init (it gives the active view). */
void ps5wk_downloads_init(WPEDisplayPS5* display, WebKitNetworkSession* session);
/* Another session's downloads come here too (private browsing's; the files
 * and the list are kept as for any download). */
void ps5wk_downloads_watch_session(WebKitNetworkSession* session);

/* browserui.c's decide-policy for responses: TRUE when the response was
 * made a download (attachment, a type the browser can't show in the main
 * frame, a link with a download attribute). */
gboolean ps5wk_downloads_response_policy(WebKitWebView* view, WebKitResponsePolicyDecision* decision);

/* The list, newest first (the menu's Downloads list). */
unsigned ps5wk_downloads_count(void);
void ps5wk_downloads_row(unsigned index, BUIRow* row);
/* What X and Square do for a row (footer hints): NULL = nothing. */
void ps5wk_downloads_hints(unsigned index, const char** cross, const char** square);
/* X: pause, resume, retry; a finished file's details. */
void ps5wk_downloads_primary(unsigned index);
/* Square: TRUE when it needs a second press (a finished file is deleted). */
gboolean ps5wk_downloads_square_needs_confirm(unsigned index);
/* Square: cancel (running, paused: the .part goes), or delete the entry and
 * its file. */
void ps5wk_downloads_square(unsigned index);
/* The details card (strings valid until the next call). */
void ps5wk_downloads_details(unsigned index, BUIDetails* details);
/* Receiving now, and their progress together (< 0: a size not known). */
unsigned ps5wk_downloads_active(double* progress);

/* The API (/browser/download) and tests. */
const char* ps5wk_downloads_action(const char* action, int index, const char* url);
void ps5wk_downloads_json(GString* out);

/* The download manager's page (dlm.c, parallax://downloads): its state as
 * JSON (any thread), and its actions ("do=save|pause|resume|cancel|remove|
 * delete|forget|extract|extract-install|extract-image|jobstop|after|peek|
 * settings|favorite|task|taskstop|taskremove|adopt|partial-delete", main
 * loop: NULL or why not). */
char* ps5wk_downloads_manager_json(void);
/* Any thread: the folders an unfinished download may be in (one a line),
 * for the service's "partials". */
char* ps5wk_downloads_scan_dirs(void);
const char* ps5wk_downloads_manage(GHashTable* params);

/* Settings: the folder, its free space, the finished ones. */
void ps5wk_downloads_clear_finished(void);
/* Any thread: the free space on /data in bytes, -1 if not known (the title
 * can't call statvfs: see downloads.c). */
gint64 ps5wk_downloads_free_space(void);
/* Any thread: {"folder","free","finished","active","total"} for settings.json. */
char* ps5wk_downloads_summary_json(void);
/* Any thread: the finished downloads for the link server's page:
 * [{"file","size","sha256","finished"}]. */
char* ps5wk_downloads_finished_json(void);
/* Any thread: the path of a finished download called @name (only names in
 * the list), or NULL. */
char* ps5wk_downloads_finished_path(const char* name);

/* The wind-down: what still runs is saved as paused. */
void ps5wk_downloads_wind_down(void);

/* browserui.c: the list or the ring changed (it redraws when shown). */
void ps5wk_browser_downloads_changed(void);
/* browserui.c: a download from @view is under way (a tab opened only for
 * it can close now). */
void ps5wk_browser_download_started(WebKitWebView* view);
/* browserui.c: the active tab's view (resumes are asked through it). */
WebKitWebView* ps5wk_browser_active_view(void);
