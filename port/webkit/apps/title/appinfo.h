/*
 * What an image, package or app folder on a drive holds, and its icon, for
 * the Downloads and Files dialogs (downloads.c, files.c); the phone page
 * (dlm.c) asks the service itself. The download service reads them (op "titleinfo" and
 * "icon": apps/child/dl-job.c, apps/common/ps5image.h); this keeps the
 * last few, and decodes the icon (libpng) scaled to a square of
 * PS5WK_APPINFO_ICON pixels, premultiplied, for the canvas
 * (BUIChoices.picture).
 */
#pragma once

#include "minijson.h"

#include <glib.h>

#define PS5WK_APPINFO_ICON 112

typedef struct {
    gboolean pending;     /* the service hasn't answered yet */
    MJ* info;             /* its titleinfo answer (ps5image_json, with "installed"), or NULL */
    char* error;          /* ... or why not */
    guint32* icon;        /* PS5WK_APPINFO_ICON squared, 0xAARRGGBB premultiplied; NULL: none (yet) */
    gboolean iconPending;
} PS5WKAppInfo;

/* Main loop. path's entry (size: the file's, so a changed file is asked
 * again; an error is asked again after a few seconds); the first call asks
 * the service, and done runs (main loop) each time something arrives.
 * Never NULL. */
const PS5WKAppInfo* ps5wk_appinfo(const char* path, gint64 size, void (*done)(void));

/* Main loop. Drops path's entry (a job installed, moved or replaced it):
 * the next ps5wk_appinfo asks again; an answer still on its way is
 * dropped. */
void ps5wk_appinfo_forget(const char* path);

/* "Name · ID · version" from an info ("" pieces left out); g_free. */
char* ps5wk_appinfo_summary(const MJ* info);
