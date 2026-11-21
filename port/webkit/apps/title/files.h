/*
 * The menu's Files list (ps5browser): the console's drives and folders
 * through the download service (dl-job.c's "roots" and "ls"), with what
 * can be done to what is there: open a folder, install an app folder,
 * unpack an archive, make an exFAT image, move or copy to another folder,
 * rename, delete, a new folder, favourites, the folder new downloads go
 * to. The jobs run as downloads.c's tasks (shown in the Downloads list).
 * See port/webkit/README.md ("Downloads").
 *
 * Main thread.
 */
#pragma once

#include "browserdraw.h"

#include <glib.h>
#include <wpe/ps5/wpe-ps5.h>

void ps5wk_files_init(WPEDisplayPS5* display);

/* The list came into view: read the place shown again. */
void ps5wk_files_shown(void);
/* A job changed files (a move, a copy, a delete ended): read the place
 * again if it is shown. */
void ps5wk_files_refresh(void);

unsigned ps5wk_files_count(void);
/* The drives couldn't be listed: why, and that it is tried again (NULL: no error). */
const char* ps5wk_files_error(void);
void ps5wk_files_row(unsigned index, BUIRow* row);
/* What X, Square, R2 and O do for a row (NULL: nothing; O NULL: close). */
void ps5wk_files_hints(unsigned index, const char** cross, const char** square, const char** r2, const char** circle);

/* What X (or R2) did. */
enum {
    PS5WK_FILES_NOTHING,
    PS5WK_FILES_CHOICES,      /* open the dialog for the row (ps5wk_files_choices_open) */
    PS5WK_FILES_DONE,         /* done (a folder opened, a job started) */
    PS5WK_FILES_TO_DOWNLOADS, /* back to the Downloads list */
    PS5WK_FILES_MENU_CLOSE,   /* the download waiting for a folder has one: back to the page */
};
int ps5wk_files_activate(unsigned index, gboolean alternative);
/* What a row is, kept while the listing changes (g_free; NULL: no row). */
char* ps5wk_files_row_key(unsigned index);
gboolean ps5wk_files_square_needs_confirm(unsigned index);
void ps5wk_files_square(unsigned index);
/* O: up a folder; FALSE at the drives (the menu closes). */
gboolean ps5wk_files_up(void);

/* The dialog for a row: open (FALSE: none), fill (FALSE: its row is gone),
 * a choice (returns BUI_CHOSEN_*). */
gboolean ps5wk_files_choices_open(unsigned index);
gboolean ps5wk_files_choices(BUIChoices* out);
int ps5wk_files_choose(int id);

/* Files at folder with name focused (Show in Files). */
void ps5wk_files_open(const char* folder, const char* name);
/* Files to choose the folder of the download waiting for one (its id). */
void ps5wk_files_pick_folder(unsigned download, const char* fileName);
/* The download waiting for a folder went (saved elsewhere, removed). */
void ps5wk_files_pick_cancel(unsigned download);

/* downloads.c: the service's "peekend" for a Files look (id "f<n>"). */
void ps5wk_files_peek_ended(const char* id, GHashTable* params);

/* A place for people: "Downloads", "Downloads › games", "USB drive 1 ›
 * homebrew", "Internal storage › x" (g_free). */
char* ps5wk_files_place_name(const char* path);
