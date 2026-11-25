/*
 * The PS5 user's own browser data (profile.c; README: "Users and storage"):
 * /data/ps5webkit/users/<user id>/ holds what is theirs (cookies and site
 * data, the cache, history, bookmarks, tabs, downloads, the home page, the
 * sites where ads or pop-ups are allowed). Everything else in /data/ps5webkit (fonts,
 * the ad-block lists, child.env and the other switches) is shared.
 */
#pragma once

#include <glib.h>

#define PS5WK_DATA_DIR "/data/ps5webkit"

/* Before anything reads user data: asks the system who started the title,
 * makes their folder, and on the first start with users moves the data that
 * was in /data/ps5webkit (one browser for everyone) into it. */
void ps5wk_profile_init(void);

const char* ps5wk_profile_dir(void);           /* /data/ps5webkit/users/<id> */
const char* ps5wk_profile_downloads_dir(void); /* .../downloads */
const char* ps5wk_profile_name(void);          /* the PS5 user's name */

/* What the browser keeps on the console, for the settings page:
 * "storage":{...}. Any thread; sizes are counted at most every few seconds. */
char* ps5wk_profile_storage_json(void);
/* Count again at the next request (after clearing something). */
void ps5wk_profile_storage_changed(void);
