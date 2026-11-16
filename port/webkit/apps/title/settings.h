/*
 * The browser's settings (settings.c; README: "Settings"), served by the
 * link server as /settings, /settings.json and POST /settings.
 */
#pragma once

#include <glib.h>

/* What applies a setting. */
enum {
    PS5WK_SETTINGS_PAGE = 1,    /* a new WebProcess (restart the page) */
    PS5WK_SETTINGS_BROWSER = 2, /* the title starting again */
};

/* Checks a URL typed for the home page: a new string, or NULL and *error. */
typedef char* (*PS5WKSettingsNormalize)(const char* text, const char** error);

/* dir: where child.env and the flag files live (/data/ps5webkit); user: the
 * PS5 user's folder, for the files that are theirs (the Home page). Records
 * the values found as the ones in use. */
void ps5wk_settings_init(const char* dir, const char* user, PS5WKSettingsNormalize normalize);

/* The values now on disk are in use for these scopes (a new WebProcess
 * started: PS5WK_SETTINGS_PAGE). */
void ps5wk_settings_applied(int scopes);
/* Pages in the browser process: page settings wait for a browser restart. */
void ps5wk_settings_set_pages_in_browser(gboolean on);

/* The settings as JSON: {"ok":true,<extra>"settings":[...],...}. extra:
 * more members, each followed by a comma, or NULL. */
char* ps5wk_settings_json(const char* extra);

/* Applies a request body (JSON object or form fields, id=value; ids or the
 * child.env/file names). *message says what happened; *changed: the scopes
 * of the settings that changed. FALSE on a bad request (nothing changed
 * then) or when something could not be written. */
gboolean ps5wk_settings_update(const char* body, gboolean json, char** message, int* changed);

/* The search engine setting's URL, the words (escaped) go after it:
 * "https://www.google.com/search?q=" by default. Any thread. */
const char* ps5wk_settings_search_url(void);
/* The result of the last action (clearing data, restarting), for the page. */
void ps5wk_settings_set_status(const char* format, ...) G_GNUC_PRINTF(1, 2);

/* The settings page (static: its script reads /settings.json). */
extern const char ps5wk_settings_page[];

/* The page the browser menu's Settings button (⚙) opens. */
#define PS5WK_SETTINGS_URL "parallax://settings"
