/*
 * The download manager's page (parallax://downloads, /dlm on the link
 * server): see dlm.c.
 */
#pragma once

#include <glib.h>

extern const char ps5wk_dlm_page_html[];

/* A route thread: TRUE when path is the page's (/dlm, /dlm.json, /dlm/fs),
 * with *status, *type and *reply (g_free) filled. The caller checks the PIN
 * first (the PS5's own page needs none). */
gboolean ps5wk_dlm_serve(const char* method, const char* path, const char* query, const char* body, int* status, const char** type, char** reply);
