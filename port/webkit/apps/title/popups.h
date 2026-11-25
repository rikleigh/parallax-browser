/*
 * Pop-ups (popups.c; README: "Pop-ups"): a page that opens a new window
 * without a press gets none and the browser says so, unless its site is on
 * the allow list (Settings, Ads & privacy). The asking itself is
 * browserui.c's (the web view's "create" signal); this is the list and what
 * was last blocked.
 */
#pragma once

#include <glib.h>

/* Once the PS5 user's folder is known (profile.c): reads the list. */
void ps5wk_popups_init(void);

/* Any thread: may the page at uri open windows without a press? */
gboolean ps5wk_popups_allowed(const char* uri);

/* Main loop: the page at uri wanted a window for popup and got none.
 * Returns how many that makes on this page (1: the first). */
unsigned ps5wk_popups_blocked(const char* uri, const char* popup);

/* Main loop: the page shown is a new one (a committed load): nothing has
 * been blocked on it yet. */
void ps5wk_popups_page(const char* uri);

/* Any thread: a pop-up was blocked on the page at uri, which is still the
 * one shown (Settings then opens on its pop-ups row). */
gboolean ps5wk_popups_blocked_on(const char* uri);

/* Any thread: pop-ups allowed on host and its subdomains, or blocked there
 * again. NULL, or why not. */
const char* ps5wk_popups_set_allowed(const char* host, gboolean allowed);

/* Private browsing: from now on the list is a copy that is dropped when it
 * ends (FALSE), as the per-site settings are; what was blocked goes too. */
void ps5wk_popups_set_private(gboolean enabled);

/* Any thread: the state for /settings.json, as one JSON member
 * ("popups":{...},) with uri the page the site row is for. savedOnly: the
 * list as saved, without the private session's changes (another device
 * while private browsing is on). */
char* ps5wk_popups_json(const char* uri, gboolean savedOnly);
