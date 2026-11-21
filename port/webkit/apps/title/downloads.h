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

/* The menu's Downloads list: the downloads (newest first), then the jobs
 * on files from Files, then unfinished downloads found in the download
 * folders that the list lost. */
unsigned ps5wk_downloads_count(void);
void ps5wk_downloads_row(unsigned index, BUIRow* row);
/* A download finished in the last few seconds (its notice says Options
 * opens it): its index, once. */
gboolean ps5wk_downloads_take_recent(unsigned* index);
/* The list came into view (it looks for lost unfinished downloads). */
void ps5wk_downloads_shown(void);
/* What X, Square and R2 do for a row (footer hints): NULL = nothing. */
void ps5wk_downloads_hints(unsigned index, const char** cross, const char** square, const char** r2);
/* X: pause, resume, retry, continue, stop a job; TRUE: open the row's
 * dialog instead (a finished download, one waiting for a folder). */
gboolean ps5wk_downloads_primary(unsigned index);
/* Square: TRUE when it needs a second press (a file is deleted). */
/* O on the dialog: BUI_CHOSEN_CLOSE (only the dialog closes), or
 * BUI_CHOSEN_MENU_CLOSE when that cancelled a download waiting for a folder. */
int ps5wk_downloads_choices_back(void);
/* The row the open dialog is about, where it is now (FALSE: gone). */
gboolean ps5wk_downloads_choices_index(unsigned* index);
/* What a row is, kept while rows come and go above it (g_free; NULL: no row). */
char* ps5wk_downloads_row_key(unsigned index);
gboolean ps5wk_downloads_square_needs_confirm(unsigned index);
/* Square: cancel (running, paused: the .part goes), or delete the entry and
 * its file; clear a job; delete an unfinished download's files. */
void ps5wk_downloads_square(unsigned index);
/* The details card (strings valid until the next call; count 0: none). */
void ps5wk_downloads_details(unsigned index, BUIDetails* details);
/* The dialog for a row: open (FALSE: none), fill (FALSE: its row is gone
 * or changed), a choice (returns BUI_CHOSEN_*). */
gboolean ps5wk_downloads_choices_open(unsigned index);
gboolean ps5wk_downloads_choices(BUIChoices* out);
int ps5wk_downloads_choose(int id);
/* Files: the favourite folders (NULL-terminated, or NULL; downloads.c's)
 * and the folder new downloads go to. */
char** ps5wk_downloads_favorites(void);
const char* ps5wk_downloads_default_folder(void);
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
/* Any thread: whether a download, a job or a task uses path (or path is a
 * folder holding what one uses): Files leaves it alone then. */
gboolean ps5wk_downloads_in_use(const char* path);
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
