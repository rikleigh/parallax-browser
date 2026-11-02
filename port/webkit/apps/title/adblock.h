/*
 * The ad blocker (adblock.c; port/webkit/ADBLOCK.md): uBlock Origin Lite's
 * network rules as a WebKit content filter, the per-site allow list and
 * the on-console list update.
 */
#pragma once

#include <glib.h>
#include <wpe/webkit.h>

/* Main loop, once the web view exists: loads (or compiles) the list and
 * schedules the background update check. */
void ps5wk_adblock_init(WebKitWebView* webView);

/* Main loop: the page at uri is about to load (or has committed): the
 * filter goes on or off for it (setting, allow list). */
void ps5wk_adblock_navigation(const char* uri);

/* Any thread: re-read the setting (Settings changed it) and apply it. */
void ps5wk_adblock_setting_changed(void);

/* Any thread: allow (or block again) ads on host. NULL, or why not. When
 * reload is set, the page shown reloads if it is on that host. */
const char* ps5wk_adblock_set_allowed(const char* host, gboolean allowed, gboolean reload);

/* Any thread: start a list update now (from Settings). NULL, or why not. */
const char* ps5wk_adblock_update(const char* from);

/* Any thread: the state for /settings.json, as one JSON member
 * ("adblock":{...},) with uri the page the site buttons are for. */
char* ps5wk_adblock_json(const char* uri);

/* The host of an http(s) URI without "www.", or NULL (free it). */
char* ps5wk_adblock_site_for_uri(const char* uri);
