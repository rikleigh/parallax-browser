/*
 * adblock-scripting.c: uBlock Origin Lite's per-site cosmetic filters and
 * scriptlets for one frame host (port/webkit/ADBLOCK.md, phase 2). Reads
 * the site data made by port/webkit/adblock/ubol-scripting.js and builds,
 * for a host, the style sheet and the main-world / isolated-world scripts
 * with only the filters and scriptlet calls that match it. Plain GLib (no
 * WebKit): adblock.c registers the results as WebKit user content.
 */
#pragma once

#include <glib.h>

typedef struct AbSiteData AbSiteData;

/* The site data file, or NULL with *error set (free it). */
AbSiteData* ab_site_data_load(const char* path, char** error);
void ab_site_data_free(AbSiteData* data);

typedef struct {
    char* version;      /* uBO Lite's */
    int converter;      /* ubol-scripting.js VERSION */
    guint hosts;        /* host index entries */
    guint calls[2];     /* scriptlet calls: main, isolated */
    guint cosmetics;    /* per-site cosmetic filters (CSS + procedural) */
    guint procedurals;
    guint libraryItems[2];
    gsize bytes;        /* the file, kept in memory */
    gsize indexBytes;   /* the index built on it */
} AbSiteDataInfo;

/* Points into data (don't free). */
void ab_site_data_info(const AbSiteData* data, AbSiteDataInfo* info);

/* The world indexes. */
enum { AB_WORLD_MAIN = 0, AB_WORLD_ISOLATED = 1 };

typedef struct {
    char* script[2];      /* per world, or NULL: no calls for this host */
    char* css;            /* user style sheet, or NULL */
    gboolean generic[2];  /* the host's calls in that world are exactly the generic ("*") ones */
    guint calls[2];
    guint cssRules;
    guint procedurals;
    gboolean usesTop;     /* has calls that apply only below topHost (uBO Lite's "site>>" entries) */
} AbSiteScripts;

/* For a frame at host (lowercase) whose top-level page is at topHost (NULL
 * or the same: a top-level page). */
void ab_site_data_for_host(const AbSiteData* data, const char* host, const char* topHost, AbSiteScripts* out);

/* The scripts for every site: the "*" entries (no CSS). */
void ab_site_data_generic(const AbSiteData* data, AbSiteScripts* out);

/* Whether frames below topHost get calls of their own ("site>>" entries). */
gboolean ab_site_data_has_ancestor_rules(const AbSiteData* data, const char* topHost);

void ab_site_scripts_clear(AbSiteScripts* scripts);

/* Scripts built from now on log a scriptlet call that throws (console). */
void ab_site_data_set_debug(gboolean on);
