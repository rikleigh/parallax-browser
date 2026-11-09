/*
 * adblock.c: the ad blocker (port/webkit/ADBLOCK.md).
 *
 * uBlock Origin Lite's default network rulesets, converted to WebKit
 * content-blocker JSON by port/webkit/adblock/dnr-to-webkit.js, compiled by
 * WebKitUserContentFilterStore and put on the web view's user content
 * manager as one filter.
 *
 * The list:
 *   /app0/adblock/adblock-rules.json + adblock-info.ini  bundled with the title
 *     (tools/ubol-convert.js), so blocking works offline and on first start
 *   /data/ps5webkit/adblock/adblock-rules.json + adblock-info.ini  the last
 *     update downloaded on the console
 *   The newer of the two (uBO Lite version, then converter version) is the
 *   source. Compiled into /data/ps5webkit/browser/contentfilters under
 *   "ubol-a" or "ubol-b" (alternately: a new list is compiled next to the
 *   one in use, which stays until the new one is ready);
 *   /data/ps5webkit/adblock/compiled.ini says which one and from what.
 *   Later starts only load it (no compile) while the source is unchanged.
 *
 * Compiling and updating happen in a child process (ps5wk-child's
 * "adblock" mode, apps/child/adblock-job.c), so the memory they take (the
 * compile peaks at several hundred MB) goes back when it exits; the title
 * only loads the compiled result. The update (Settings "Update lists now",
 * or by itself once a day, 2 min after start at the earliest and never while
 * a page loads): the child fetches uBO Lite's manifest.json from
 * github.com/uBlockOrigin/uBOL-home, and if its version is newer than the
 * list in use, the default rulesets at that release tag, runs
 * dnr-to-webkit.js in JavaScriptCore, compiles the result into the slot not
 * in use and saves the list to /data/ps5webkit/adblock. Any failure keeps
 * the list in use.
 *
 * Off switches: the "Ad blocking" setting (/data/ps5webkit/adblock-off) and
 * the per-site allow list (adblock-allow.txt in the PS5 user's folder,
 * /data/ps5webkit/users/<id> (profile.c), one site per
 * line: a host and its subdomains). WebKit applies each filter on its own
 * (an exception in a second filter cannot undo a block in this one), so the
 * filter is taken off the user content manager while an allowed site is
 * shown and put back for the next page (ps5wk_adblock_navigation, from
 * load-changed). Logs: [adblock] lines.
 */
#define SPIKE_NAME "ps5browser"
#include "common/spike.h"
#include "adblock.h"
#include "adblock-scripting.h"
#include "settings.h"
#include "profile.h"
#include "viewinvr.h"
#include "WPEProcessManagerPS5.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>

#define AB_DIR "/data/ps5webkit/adblock"
#define AB_STORE "/data/ps5webkit/browser/contentfilters"
#define AB_BUNDLED "/app0/adblock"
#define AB_OFF_FLAG "/data/ps5webkit/adblock-off"
#define AB_ALLOW_NAME "adblock-allow.txt" /* in the PS5 user's folder (profile.c) */
#define AB_NO_UPDATE_FLAG "/data/ps5webkit/adblock-noupdate"
#define AB_FIRST_CHECK_S 120
#define AB_RECHECK_S (30 * 60)
#define AB_CHECK_INTERVAL_S (24 * 3600)

void ps5wk_title_mem_log(const char* milestone) __attribute__((weak));
static void memLog(const char* milestone)
{
    if (ps5wk_title_mem_log)
        ps5wk_title_mem_log(milestone);
}

typedef struct {
    char* version;  /* uBO Lite's, e.g. 2026.1026.1812 */
    int converter;  /* dnr-to-webkit.js VERSION */
    long rules;     /* WebKit rules */
    long dnrRules;
    long skipped;
    gint64 generated; /* Unix seconds */
    char* sha256;   /* of the rules JSON */
    char* origin;   /* bundled | update */
    char* rulesets;
} ListInfo;

static void listInfoClear(ListInfo* info)
{
    g_free(info->version);
    g_free(info->sha256);
    g_free(info->origin);
    g_free(info->rulesets);
    memset(info, 0, sizeof *info);
}

static void listInfoCopy(ListInfo* to, const ListInfo* from)
{
    listInfoClear(to);
    *to = *from;
    to->version = g_strdup(from->version);
    to->sha256 = g_strdup(from->sha256);
    to->origin = g_strdup(from->origin);
    to->rulesets = g_strdup(from->rulesets);
}

/* ---- state ----
 * Main loop: webView..filterOn (and jobRunning below). Under abLock (read by
 * the link server thread): enabled, sites, status, active info, updating,
 * compiling, lastCheck. */
static WebKitWebView* webView;
static WebKitUserContentManager* contentManager;
static WebKitUserContentFilterStore* store;
static WebKitUserContentFilter* activeFilter;
static char* activeID;
static gboolean filterOn;
static gboolean compiling;

static GMutex abLock;
static gboolean enabled = TRUE;
static GPtrArray* sites; /* char*, sorted */
static char* status;
static ListInfo active;
static gint64 activeCompiledAt;
static gboolean updating;
static gint64 lastCheck;
static gint64 lastCheckResult; /* when the last check finished */

static void setStatus(const char* format, ...) G_GNUC_PRINTF(1, 2);
static void setStatus(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    char* text = g_strdup_vprintf(format, args);
    va_end(args);
    g_mutex_lock(&abLock);
    g_free(status);
    status = text;
    g_mutex_unlock(&abLock);
}

/* open(), not access(): access() reports existing files as missing in a title. */
static gboolean fileExists(const char* path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return FALSE;
    close(fd);
    return TRUE;
}

static gboolean writeAtomically(const char* path, const char* contents, gsize length)
{
    char* temporary = g_strdup_printf("%s.tmp", path);
    gboolean ok = FALSE;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        ok = TRUE;
        for (gsize done = 0; ok && done < length;) {
            ssize_t n = write(fd, contents + done, length - done);
            ok = n > 0;
            done += n > 0 ? (gsize)n : 0;
        }
        fchmod(fd, 0666);
        fsync(fd);
        ok = !close(fd) && ok;
        if (ok)
            ok = !rename(temporary, path);
        if (!ok)
            unlink(temporary);
    }
    g_free(temporary);
    return ok;
}

static double msSince(gint64 start)
{
    return (g_get_monotonic_time() - start) / 1000.0;
}

/* -1, 0, 1: dotted numbers (2026.1026.1812). */
static int compareVersions(const char* a, const char* b)
{
    if (!a || !b)
        return a ? 1 : b ? -1 : 0;
    char** x = g_strsplit(a, ".", -1);
    char** y = g_strsplit(b, ".", -1);
    int result = 0;
    for (int i = 0; !result && (x[i] || y[i]); i++) {
        long u = x[i] ? strtol(x[i], NULL, 10) : -1, v = y[i] ? strtol(y[i], NULL, 10) : -1;
        result = u < v ? -1 : u > v;
    }
    g_strfreev(x);
    g_strfreev(y);
    return result;
}

/* (version, converter) */
static int compareLists(const ListInfo* a, const ListInfo* b)
{
    int c = compareVersions(a->version, b->version);
    return c ? c : a->converter < b->converter ? -1 : a->converter > b->converter;
}

static gboolean readInfo(const char* path, ListInfo* info)
{
    GKeyFile* file = g_key_file_new();
    gboolean ok = g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL);
    memset(info, 0, sizeof *info);
    if (ok) {
        info->version = g_key_file_get_string(file, "list", "version", NULL);
        info->sha256 = g_key_file_get_string(file, "list", "sha256", NULL);
        info->origin = g_key_file_get_string(file, "list", "origin", NULL);
        info->rulesets = g_key_file_get_string(file, "list", "rulesets", NULL);
        info->converter = g_key_file_get_integer(file, "list", "converter", NULL);
        info->rules = (long)g_key_file_get_int64(file, "list", "rules", NULL);
        info->dnrRules = (long)g_key_file_get_int64(file, "list", "dnr-rules", NULL);
        info->skipped = (long)g_key_file_get_int64(file, "list", "skipped", NULL);
        info->generated = g_key_file_get_int64(file, "list", "generated", NULL);
        ok = info->version && info->sha256;
        if (!ok)
            listInfoClear(info);
    }
    g_key_file_unref(file);
    return ok;
}

static void infoToKeyFile(GKeyFile* file, const ListInfo* info)
{
    g_key_file_set_string(file, "list", "source", "uBlock Origin Lite (github.com/uBlockOrigin/uBOL-home, GPL-3.0)");
    g_key_file_set_string(file, "list", "version", info->version ? info->version : "");
    g_key_file_set_string(file, "list", "rulesets", info->rulesets ? info->rulesets : "");
    g_key_file_set_integer(file, "list", "converter", info->converter);
    g_key_file_set_int64(file, "list", "rules", info->rules);
    g_key_file_set_int64(file, "list", "dnr-rules", info->dnrRules);
    g_key_file_set_int64(file, "list", "skipped", info->skipped);
    g_key_file_set_int64(file, "list", "generated", info->generated);
    g_key_file_set_string(file, "list", "sha256", info->sha256 ? info->sha256 : "");
    g_key_file_set_string(file, "list", "origin", info->origin ? info->origin : "");
}

static gboolean writeKeyFile(GKeyFile* file, const char* path)
{
    gsize length = 0;
    char* text = g_key_file_to_data(file, &length, NULL);
    gboolean ok = text && writeAtomically(path, text, length);
    g_free(text);
    return ok;
}

/* ---- sites (the allow list) ---- */

static int compareStrings(gconstpointer a, gconstpointer b)
{
    return strcmp(*(char* const*)a, *(char* const*)b);
}

/* A site as stored: lowercase host without "www.", or NULL. */
static char* cleanSite(const char* text)
{
    if (!text)
        return NULL;
    char* site = g_ascii_strdown(text, -1);
    g_strstrip(site);
    if (g_str_has_prefix(site, "www."))
        memmove(site, site + 4, strlen(site + 4) + 1);
    gboolean ok = *site && strlen(site) < 254 && strchr(site, '.');
    for (const char* c = site; ok && *c; c++)
        ok = g_ascii_isalnum(*c) || *c == '.' || *c == '-' || *c == '_';
    if (!ok) {
        g_free(site);
        return NULL;
    }
    return site;
}

char* ps5wk_adblock_site_for_uri(const char* uri)
{
    if (!uri || (g_ascii_strncasecmp(uri, "http://", 7) && g_ascii_strncasecmp(uri, "https://", 8)))
        return NULL;
    GUri* parsed = g_uri_parse(uri, G_URI_FLAGS_ENCODED | G_URI_FLAGS_PARSE_RELAXED, NULL);
    char* site = parsed && g_uri_get_host(parsed) ? cleanSite(g_uri_get_host(parsed)) : NULL;
    if (parsed)
        g_uri_unref(parsed);
    return site;
}

static char* hostForURI(const char* uri)
{
    if (!uri || (g_ascii_strncasecmp(uri, "http://", 7) && g_ascii_strncasecmp(uri, "https://", 8)))
        return NULL;
    GUri* parsed = g_uri_parse(uri, G_URI_FLAGS_ENCODED | G_URI_FLAGS_PARSE_RELAXED, NULL);
    char* host = parsed && g_uri_get_host(parsed) ? g_ascii_strdown(g_uri_get_host(parsed), -1) : NULL;
    if (parsed)
        g_uri_unref(parsed);
    return host;
}

/* abLock held. The allow-list entry covering host, or NULL. */
static const char* siteEntryFor(const char* host)
{
    if (!host || !sites)
        return NULL;
    size_t hostLength = strlen(host);
    for (guint i = 0; i < sites->len; i++) {
        const char* site = g_ptr_array_index(sites, i);
        size_t length = strlen(site);
        if (!strcmp(host, site) || (hostLength > length && host[hostLength - length - 1] == '.' && !strcmp(host + hostLength - length, site)))
            return site;
    }
    return NULL;
}

static char* allowFile(void)
{
    return g_build_filename(ps5wk_profile_dir(), AB_ALLOW_NAME, NULL);
}

static void readSites(void)
{
    gchar* contents = NULL;
    GPtrArray* list = g_ptr_array_new_with_free_func(g_free);
    char* path = allowFile();
    gboolean found = g_file_get_contents(path, &contents, NULL, NULL);
    g_free(path);
    if (found) {
        char** lines = g_strsplit(contents, "\n", -1);
        for (char** line = lines; *line; line++) {
            char* site = **line == '#' ? NULL : cleanSite(*line);
            if (site)
                g_ptr_array_add(list, site);
        }
        g_strfreev(lines);
        g_free(contents);
    }
    g_ptr_array_sort(list, compareStrings);
    g_mutex_lock(&abLock);
    if (sites)
        g_ptr_array_unref(sites);
    sites = list;
    g_mutex_unlock(&abLock);
}

/* abLock held. */
static gboolean writeSites(void)
{
    GString* text = g_string_new("# Sites where the ad blocker is off (Settings), one per line: a host and its subdomains.\n");
    for (guint i = 0; i < sites->len; i++)
        g_string_append_printf(text, "%s\n", (char*)g_ptr_array_index(sites, i));
    char* path = allowFile();
    gboolean ok = writeAtomically(path, text->str, text->len);
    g_free(path);
    g_string_free(text, TRUE);
    return ok;
}

/* ---- the filter on the web view ---- */

static char* lastURI; /* main loop: the page the filter was last set for */

static gboolean isSettingsPage(const char* uri)
{
    return uri && g_str_has_prefix(uri, "parallax:");
}

/* Main loop. The filter on or off for the page at uri (NULL: the last one). */
static void applyFilter(const char* uri, const char* why)
{
    if (uri && !isSettingsPage(uri) && (!g_ascii_strncasecmp(uri, "http://", 7) || !g_ascii_strncasecmp(uri, "https://", 8))) {
        g_free(lastURI);
        lastURI = g_strdup(uri);
    } else if (uri)
        return; /* the settings page, file: and about: pages keep the state */
    if (!contentManager || !activeFilter)
        return;
    char* host = hostForURI(lastURI);
    g_mutex_lock(&abLock);
    const char* entry = siteEntryFor(host);
    char* allowedBy = entry ? g_strdup(entry) : NULL;
    gboolean want = enabled && !allowedBy;
    gboolean on = enabled;
    g_mutex_unlock(&abLock);
    if (want != filterOn) {
        if (want)
            webkit_user_content_manager_add_filter(contentManager, activeFilter);
        else
            webkit_user_content_manager_remove_filter(contentManager, activeFilter);
        filterOn = want;
        spike_log("[adblock] filter %s for %s (%s%s%s%s)", want ? "on" : "off", host ? host : "(no page)", why,
            !on ? "; ad blocking is off in Settings" : "", allowedBy ? "; allowed site " : "", allowedBy ? allowedBy : "");
    }
    g_free(allowedBy);
    g_free(host);
}

void ps5wk_adblock_navigation(const char* uri)
{
    applyFilter(uri, "navigation");
}

/* ---- phase 2: per-site cosmetic filters and scriptlets ----
 *
 * uBO Lite's per-site data (adblock-scripting.txt next to the list it came
 * with, made by ubol-scripting.js) is looked up for the host of every frame
 * about to load: decide-policy (navigation action) comes before the frame's
 * request, and the user content sent from here reaches the WebProcess
 * before the policy answer, so it is there at document start. For each
 * host (cached, AB_SITE_CACHE hosts): a user style sheet with its cosmetic
 * filters, a main-world user script with its scriptlet calls and an
 * isolated-world one ("ubol", uBO Lite's ISOLATED scriptlets and procedural
 * filters), each with an allow list of that host only, all frames, document
 * start. The scriptlets every site gets ("*") are one script for all
 * http(s) frames, with the hosts that have their own script (which includes
 * them, minus the host's exceptions) on its block list. Off with the
 * setting, and on allowed sites (all of it is removed while one is shown,
 * as the filter is). */

#define AB_SCRIPTING_FILE "adblock-scripting.txt"
#define AB_SCRIPTING_DEBUG_FLAG "/data/ps5webkit/adblock-scripting-debug" /* a call that throws is logged */
#define AB_SCRIPTING_OFF_FLAG "/data/ps5webkit/adblock-scripting-off"     /* test knob: phase 1 only (read at start) */
#define AB_SITE_CACHE 32
#define AB_ISOLATED_WORLD "ubol"

typedef struct {
    char* host;
    char* top;                       /* the top-level host its "site>>" calls are for, or NULL */
    WebKitUserScript* script[2];
    WebKitUserStyleSheet* css;
    gboolean blocksGeneric[2];       /* the generic script must not run on this host */
    gint64 used;
} SiteEntry;

static AbSiteData* siteData;
static char* siteDataSource;         /* the directory it came from */
static GHashTable* siteEntries;      /* host -> SiteEntry */
static WebKitUserScript* genericScript[2];
static char* genericSource[2];
static gboolean scriptingOn;
static gboolean scriptingDisabled;   /* AB_SCRIPTING_OFF_FLAG */
static guint siteLookups;
static double siteLookupMS;          /* the slowest */
/* Under abLock (/settings.json reads them on the link server's thread). */
static char* lastSite;               /* the last top-level host set up */
static guint lastCalls[2], lastCSS, lastProcedurals;
static AbSiteDataInfo siteInfo;      /* version points into siteData */
static char* siteInfoVersion;
static guint sitesSetUp;

static void siteEntryFree(gpointer data)
{
    SiteEntry* entry = data;
    for (int w = 0; w < 2; w++) {
        if (entry->script[w]) {
            webkit_user_content_manager_remove_script(contentManager, entry->script[w]);
            webkit_user_script_unref(entry->script[w]);
        }
    }
    if (entry->css) {
        webkit_user_content_manager_remove_style_sheet(contentManager, entry->css);
        webkit_user_style_sheet_unref(entry->css);
    }
    g_free(entry->host);
    g_free(entry->top);
    g_free(entry);
}

static gboolean scriptableHost(const char* host)
{
    if (!host || !*host || strlen(host) > 253)
        return FALSE;
    for (const char* c = host; *c; c++) {
        if (!g_ascii_isalnum(*c) && *c != '.' && *c != '-' && *c != '_')
            return FALSE;
    }
    return TRUE;
}

/* http://host/ and https://host/, any path and port (UserContentURLPattern). */
static void addHostPatterns(GPtrArray* patterns, const char* host)
{
    g_ptr_array_add(patterns, g_strdup_printf("http://%s/*", host));
    g_ptr_array_add(patterns, g_strdup_printf("https://%s/*", host));
}

static WebKitUserScript* newScript(const char* source, int world, const char* const* allow, const char* const* block)
{
    if (world == AB_WORLD_ISOLATED)
        return webkit_user_script_new_for_world(source, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
            AB_ISOLATED_WORLD, allow, block);
    return webkit_user_script_new(source, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, allow, block);
}

/* The generic scripts again, with the block list as the cache is now (the
 * new one goes on before the old one comes off). */
static void genericRebuild(void)
{
    static const char* const allow[] = { "http://*/*", "https://*/*", NULL };
    for (int w = 0; w < 2; w++) {
        WebKitUserScript* old = genericScript[w];
        genericScript[w] = NULL;
        if (genericSource[w]) {
            GPtrArray* block = g_ptr_array_new_with_free_func(g_free);
            GHashTableIter iter;
            gpointer value;
            g_hash_table_iter_init(&iter, siteEntries);
            while (g_hash_table_iter_next(&iter, NULL, &value)) {
                SiteEntry* entry = value;
                if (entry->blocksGeneric[w])
                    addHostPatterns(block, entry->host);
            }
            g_ptr_array_add(block, NULL);
            genericScript[w] = newScript(genericSource[w], w, allow, block->len > 1 ? (const char* const*)block->pdata : NULL);
            webkit_user_content_manager_add_script(contentManager, genericScript[w]);
            g_ptr_array_unref(block);
        }
        if (old) {
            webkit_user_content_manager_remove_script(contentManager, old);
            webkit_user_script_unref(old);
        }
    }
}

/* Main loop: none of it on any page from now on. */
static void scriptingOff(const char* why)
{
    if (!scriptingOn)
        return;
    guint hosts = siteEntries ? g_hash_table_size(siteEntries) : 0;
    if (siteEntries)
        g_hash_table_remove_all(siteEntries);
    for (int w = 0; w < 2; w++) {
        if (genericScript[w]) {
            webkit_user_content_manager_remove_script(contentManager, genericScript[w]);
            webkit_user_script_unref(genericScript[w]);
            genericScript[w] = NULL;
        }
        g_clear_pointer(&genericSource[w], g_free);
    }
    scriptingOn = FALSE;
    g_mutex_lock(&abLock);
    sitesSetUp = 0;
    g_clear_pointer(&lastSite, g_free);
    g_mutex_unlock(&abLock);
    spike_log("[adblock] cosmetic filters and scriptlets off (%s); %u site(s) removed", why, hosts);
}

static gboolean scriptingOnNow(void)
{
    if (scriptingOn)
        return TRUE;
    if (!siteData || !contentManager)
        return FALSE;
    if (!siteEntries)
        siteEntries = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, siteEntryFree);
    AbSiteScripts generic;
    ab_site_data_generic(siteData, &generic);
    for (int w = 0; w < 2; w++) {
        genericSource[w] = generic.script[w];
        generic.script[w] = NULL;
    }
    ab_site_scripts_clear(&generic);
    scriptingOn = TRUE;
    genericRebuild();
    spike_log("[adblock] cosmetic filters and scriptlets on: generic scripts %.1f KB (main) + %.1f KB (isolated)",
        genericSource[0] ? strlen(genericSource[0]) / 1024.0 : 0, genericSource[1] ? strlen(genericSource[1]) / 1024.0 : 0);
    return TRUE;
}

/* Whether an entry that kept the generic scripts off its host went. */
static gboolean evictOldest(const char* keep)
{
    gboolean blocked = FALSE;
    while (g_hash_table_size(siteEntries) > AB_SITE_CACHE) {
        SiteEntry* oldest = NULL;
        GHashTableIter iter;
        gpointer value;
        g_hash_table_iter_init(&iter, siteEntries);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            SiteEntry* entry = value;
            if ((!oldest || entry->used < oldest->used) && strcmp(entry->host, keep))
                oldest = entry;
        }
        if (!oldest)
            break;
        blocked = blocked || oldest->blocksGeneric[0] || oldest->blocksGeneric[1];
        g_hash_table_remove(siteEntries, oldest->host);
    }
    return blocked;
}

/* Main loop: user content for frames at host below the page at topHost. */
static void siteSetUp(const char* host, const char* topHost, gboolean mainFrame)
{
    const char* variant = strcmp(host, topHost) && ab_site_data_has_ancestor_rules(siteData, topHost) ? topHost : NULL;
    SiteEntry* entry = g_hash_table_lookup(siteEntries, host);
    if (entry && !g_strcmp0(entry->top, variant)) {
        entry->used = g_get_monotonic_time();
        return;
    }
    gboolean blockedBefore[2] = { entry && entry->blocksGeneric[0], entry && entry->blocksGeneric[1] };
    if (entry)
        g_hash_table_remove(siteEntries, host);
    gint64 start = g_get_monotonic_time();
    AbSiteScripts scripts;
    ab_site_data_for_host(siteData, host, variant ? topHost : NULL, &scripts);
    double ms = msSince(start);
    siteLookups++;
    if (ms > siteLookupMS)
        siteLookupMS = ms;
    entry = g_new0(SiteEntry, 1);
    entry->host = g_strdup(host);
    entry->top = scripts.usesTop ? g_strdup(variant) : NULL;
    entry->used = g_get_monotonic_time();
    GPtrArray* allow = g_ptr_array_new_with_free_func(g_free);
    addHostPatterns(allow, host);
    g_ptr_array_add(allow, NULL);
    const char* const* allowList = (const char* const*)allow->pdata;
    for (int w = 0; w < 2; w++) {
        entry->blocksGeneric[w] = !scripts.generic[w];
        if (scripts.script[w]) {
            entry->script[w] = newScript(scripts.script[w], w, allowList, NULL);
            webkit_user_content_manager_add_script(contentManager, entry->script[w]);
        }
    }
    if (scripts.css) {
        entry->css = webkit_user_style_sheet_new(scripts.css, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_STYLE_LEVEL_USER, allowList, NULL);
        webkit_user_content_manager_add_style_sheet(contentManager, entry->css);
    }
    g_ptr_array_unref(allow);
    g_hash_table_insert(siteEntries, entry->host, entry);
    gboolean evictedBlocking = evictOldest(host);
    if (evictedBlocking || blockedBefore[0] != entry->blocksGeneric[0] || blockedBefore[1] != entry->blocksGeneric[1])
        genericRebuild();
    g_mutex_lock(&abLock);
    sitesSetUp = g_hash_table_size(siteEntries);
    if (mainFrame) {
        g_free(lastSite);
        lastSite = g_strdup(host);
        lastCalls[0] = scripts.calls[0];
        lastCalls[1] = scripts.calls[1];
        lastCSS = scripts.cssRules;
        lastProcedurals = scripts.procedurals;
    }
    g_mutex_unlock(&abLock);
    if (scripts.script[0] || scripts.script[1] || scripts.css || mainFrame)
        spike_log("[adblock] site %s%s%s (%s): scriptlets %u main + %u isolated, %u CSS rules, %u procedural; scripts %.1f + %.1f KB, CSS %.1f KB; lookup %.2f ms",
            host, variant ? " under " : "", variant ? variant : "", mainFrame ? "page" : "frame", scripts.calls[0], scripts.calls[1],
            scripts.cssRules, scripts.procedurals, scripts.script[0] ? strlen(scripts.script[0]) / 1024.0 : 0,
            scripts.script[1] ? strlen(scripts.script[1]) / 1024.0 : 0, scripts.css ? strlen(scripts.css) / 1024.0 : 0, ms);
    ab_site_scripts_clear(&scripts);
}

static gboolean onDecidePolicyScripting(WebKitWebView* view, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer data)
{
    (void)data;
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION || !siteData || scriptingDisabled)
        return FALSE;
    WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    WebKitURIRequest* request = action ? webkit_navigation_action_get_request(action) : NULL;
    const char* uri = request ? webkit_uri_request_get_uri(request) : NULL;
    WebKitFrameInfo* frame = webkit_navigation_action_get_target_frame_info(action);
    gboolean mainFrame = !frame || webkit_frame_info_is_main_frame(frame);
    char* host = hostForURI(uri);
    const char* topURI = mainFrame ? uri : webkit_web_view_get_uri(view);
    char* topHost = mainFrame ? g_strdup(host) : hostForURI(topURI);
    if (!host || !topHost || isSettingsPage(topURI) || !scriptableHost(host) || !scriptableHost(topHost)) {
        g_free(host);
        g_free(topHost);
        return FALSE;
    }
    g_mutex_lock(&abLock);
    gboolean on = enabled;
    const char* allowedBy = siteEntryFor(topHost);
    char* allowed = allowedBy ? g_strdup(allowedBy) : NULL;
    g_mutex_unlock(&abLock);
    if (!on)
        scriptingOff("ad blocking is off in Settings");
    else if (allowed) {
        char* why = g_strdup_printf("allowed site %s shown", allowed);
        scriptingOff(why);
        g_free(why);
    } else if (scriptingOnNow())
        siteSetUp(host, topHost, mainFrame);
    g_free(allowed);
    g_free(host);
    g_free(topHost);
    return FALSE;
}

void ps5wk_adblock_connect_view(WebKitWebView* view)
{
    g_signal_connect(view, "decide-policy", G_CALLBACK(onDecidePolicyScripting), NULL);
}

/* Main loop: the site data that came with the list in dir (bundled or
 * downloaded). What was set up from the old data goes. */
static void loadSiteData(const char* dir, const char* why)
{
    char* path = g_build_filename(dir, AB_SCRIPTING_FILE, NULL);
    gint64 start = g_get_monotonic_time();
    char* error = NULL;
    AbSiteData* data = ab_site_data_load(path, &error);
    double ms = msSince(start);
    if (!data) {
        spike_log("[adblock] no cosmetic filters or scriptlets from %s (%s): %s%s", path, why, error ? error : "?",
            siteData ? "; keeping the ones in use" : "");
        g_free(error);
        g_free(path);
        return;
    }
    ab_site_data_set_debug(fileExists(AB_SCRIPTING_DEBUG_FLAG));
    scriptingOff("new site data");
    ab_site_data_free(siteData);
    siteData = data;
    g_free(siteDataSource);
    siteDataSource = g_strdup(dir);
    AbSiteDataInfo info;
    ab_site_data_info(data, &info);
    g_mutex_lock(&abLock);
    siteInfo = info;
    g_free(siteInfoVersion);
    siteInfoVersion = g_strdup(info.version);
    siteInfo.version = NULL;
    g_mutex_unlock(&abLock);
    spike_log("[adblock] site data %s (uBO Lite %s, %s): %u hosts, scriptlet calls %u main + %u isolated, %u cosmetic filters (%u procedural), library %u + %u declarations; %.1f MB + %.1f MB index, read in %.0f ms",
        path, info.version, why, info.hosts, info.calls[0], info.calls[1], info.cosmetics, info.procedurals, info.libraryItems[0],
        info.libraryItems[1], info.bytes / 1048576.0, info.indexBytes / 1048576.0, ms);
    memLog("adblock: site data loaded");
    g_free(path);
}

/* Main loop: reload the page shown if it is an http(s) page (not Settings),
 * and, with site set, on that site. */
static void reloadShownPage(const char* site, const char* why)
{
    const char* uri = webView ? webkit_web_view_get_uri(webView) : NULL;
    if (!uri || isSettingsPage(uri) || ps5wk_vr_active())
        return;
    char* host = hostForURI(uri);
    gboolean match = host && (!site || !strcmp(host, site) || (g_str_has_suffix(host, site) && strlen(host) > strlen(site) && host[strlen(host) - strlen(site) - 1] == '.'));
    if (match) {
        spike_log("[adblock] reloading %s (%s)", uri, why);
        webkit_web_view_reload(webView);
    }
    g_free(host);
}

static gboolean settingChangedOnMain(gpointer data)
{
    gboolean on = GPOINTER_TO_INT(data);
    spike_log("[adblock] ad blocking turned %s in Settings", on ? "on" : "off");
    applyFilter(NULL, "setting");
    scriptingOff("ad blocking setting changed"); /* set up again by the next page */
    reloadShownPage(NULL, "ad blocking setting changed");
    return G_SOURCE_REMOVE;
}

void ps5wk_adblock_setting_changed(void)
{
    /* The state now (the settings reply shows it), the filter on the main loop. */
    gboolean on = !fileExists(AB_OFF_FLAG);
    g_mutex_lock(&abLock);
    gboolean changed = on != enabled;
    enabled = on;
    g_mutex_unlock(&abLock);
    if (changed)
        g_idle_add(settingChangedOnMain, GINT_TO_POINTER(on));
}

typedef struct {
    char* site;
    gboolean reload;
} SiteChange;

static gboolean siteChangedOnMain(gpointer data)
{
    SiteChange* change = data;
    applyFilter(NULL, "allow list changed");
    scriptingOff("allow list changed"); /* set up again by the next page */
    if (change->reload)
        reloadShownPage(change->site, "allow list changed");
    g_free(change->site);
    g_free(change);
    return G_SOURCE_REMOVE;
}

const char* ps5wk_adblock_set_allowed(const char* host, gboolean allowed, gboolean reload)
{
    char* site = cleanSite(host);
    if (!site)
        return "not a site (a host name such as example.com)";
    g_mutex_lock(&abLock);
    guint index = 0;
    gboolean present = FALSE;
    for (; index < sites->len && !present; index++)
        present = !strcmp(g_ptr_array_index(sites, index), site);
    if (present)
        index--;
    gboolean changed = present != allowed;
    if (changed && allowed) {
        g_ptr_array_add(sites, g_strdup(site));
        g_ptr_array_sort(sites, compareStrings);
    } else if (changed)
        g_ptr_array_remove_index(sites, index);
    gboolean ok = !changed || writeSites();
    g_mutex_unlock(&abLock);
    spike_log("[adblock] %s %s%s", allowed ? "ads allowed on" : "ads blocked again on", site, !changed ? " (no change)" : ok ? "" : " (could not save the list)");
    if (!ok) {
        g_free(site);
        return "could not save the list of sites where ads are allowed";
    }
    SiteChange* change = g_new0(SiteChange, 1);
    change->site = site;
    change->reload = reload && changed;
    g_idle_add(siteChangedOnMain, change);
    return NULL;
}

/* ---- loading and compiling ----
 *
 * The compile and the update run in a child process (ps5wk-child in its
 * "adblock" mode, apps/child/adblock-job.c), started through elfldr like the
 * WebProcesses (wpe_process_manager_ps5_run_task): WebKit's compile of this
 * list peaks at several hundred MB, and in the title's heap (dlmalloc over
 * direct memory) ~420 MB of it stayed mapped until the browser restarted.
 * The child writes the compiled list into the store under the slot not in
 * use, in WebKit's own format, and the title only loads it (ms). One job at
 * a time; a job that fails, crashes or never reports leaves the list in use.
 *
 * The job's files: AB_WORK/job.ini (what to do), AB_WORK/result.ini
 * (written last by the child), AB_WORK/bundled-rules.json and
 * dnr-to-webkit.js (copies from /app0, which a payload can't see), AB_WORK/tmp
 * (the compiler's temporary files; the child clears it). */

#define AB_WORK AB_DIR "/work"

static gboolean jobRunning; /* main loop */

typedef struct {
    char* mode;       /* compile | update */
    char* from;       /* why, for the log */
    gboolean force;
    ListInfo info;    /* compile: the list being compiled; update: the one in use */
    char* source;     /* compile: the rules as the title sees them */
    char* identifier; /* the slot it goes to */
    gint64 started;
    gboolean launched;
} Job;

static void jobFree(Job* job)
{
    g_free(job->mode);
    g_free(job->from);
    listInfoClear(&job->info);
    g_free(job->source);
    g_free(job->identifier);
    g_free(job);
}

static void storeRemoved(GObject* object, GAsyncResult* result, gpointer data)
{
    char* identifier = data;
    GError* error = NULL;
    if (!webkit_user_content_filter_store_remove_finish(WEBKIT_USER_CONTENT_FILTER_STORE(object), result, &error)) {
        spike_log("[adblock] old compiled list %s not removed: %s", identifier, error ? error->message : "?");
        g_clear_error(&error);
    }
    g_free(identifier);
}

/* Main loop: filter (compiled or loaded as identifier from info) is the one
 * to use now; the previous one goes. */
static void useFilter(WebKitUserContentFilter* filter, const char* identifier, const ListInfo* info, gint64 compiledAt)
{
    WebKitUserContentFilter* old = activeFilter;
    char* oldID = activeID;
    gboolean wasOn = filterOn;
    activeFilter = filter;
    activeID = g_strdup(identifier);
    g_mutex_lock(&abLock);
    listInfoCopy(&active, info);
    activeCompiledAt = compiledAt;
    g_mutex_unlock(&abLock);
    if (old && wasOn) {
        /* The new one first: never a moment without blocking. */
        webkit_user_content_manager_add_filter(contentManager, filter);
        webkit_user_content_manager_remove_filter(contentManager, old);
    } else {
        filterOn = FALSE;
        applyFilter(NULL, "list ready");
    }
    if (old)
        webkit_user_content_filter_unref(old);
    if (oldID && strcmp(oldID, identifier))
        webkit_user_content_filter_store_remove(store, oldID, NULL, storeRemoved, g_strdup(oldID));
    g_free(oldID);
}

/* Streams from to to (0666): no copy of the file in the heap. */
static gboolean copyFile(const char* from, const char* to)
{
    static char buffer[256 * 1024]; /* the job's preparation thread only (one job at a time) */
    int in = open(from, O_RDONLY);
    if (in < 0)
        return FALSE;
    char* temporary = g_strdup_printf("%s.tmp", to);
    int out = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    gboolean ok = out >= 0;
    while (ok) {
        ssize_t n = read(in, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = !n;
            break;
        }
        for (ssize_t done = 0; ok && done < n;) {
            ssize_t w = write(out, buffer + done, (size_t)(n - done));
            ok = w > 0;
            done += w > 0 ? w : 0;
        }
    }
    close(in);
    if (out >= 0) {
        fchmod(out, 0666);
        ok = !close(out) && ok;
    }
    if (ok)
        ok = !rename(temporary, to);
    if (!ok)
        unlink(temporary);
    g_free(temporary);
    return ok;
}

/* The converters: the copy in /data/ps5webkit/adblock (tests) or the title's. */
static const char* converterPath(void)
{
    return fileExists(AB_DIR "/dnr-to-webkit.js") ? AB_DIR "/dnr-to-webkit.js" : AB_BUNDLED "/dnr-to-webkit.js";
}

static const char* scriptingConverterPath(void)
{
    return fileExists(AB_DIR "/ubol-scripting.js") ? AB_DIR "/ubol-scripting.js" : AB_BUNDLED "/ubol-scripting.js";
}

static void removeJobFiles(void)
{
    unlink(AB_WORK "/job.ini");
    unlink(AB_WORK "/result.ini");
    unlink(AB_WORK "/bundled-rules.json");
    unlink(AB_WORK "/dnr-to-webkit.js");
    unlink(AB_WORK "/ubol-scripting.js");
    unlink(AB_WORK "/update-rules.json"); /* left by a child that was ended */
    unlink(AB_WORK "/update-scripting.txt");
}

static void jobLaunch(Job* job);
static gboolean jobFinishedOnMain(gpointer data);

static gboolean jobLaunchOnMain(gpointer data)
{
    jobLaunch(data);
    return G_SOURCE_REMOVE;
}

/* The relay thread: one line of the child's output. */
static void jobOutput(const char* line, gpointer data)
{
    (void)data;
    static const char statusPrefix[] = "[adblock-job] status: ";
    if (!strncmp(line, statusPrefix, sizeof statusPrefix - 1))
        setStatus("%s", line + sizeof statusPrefix - 1);
    else if (!strcmp(line, "[adblock-job] phase: compiling")) {
        g_mutex_lock(&abLock);
        updating = FALSE;
        compiling = TRUE;
        g_mutex_unlock(&abLock);
        memLog("adblock: child compiling");
    }
}

/* The relay thread: the child's output closed (it ended), or it never ran. */
static void jobEnded(gboolean launched, gpointer data)
{
    Job* job = data;
    job->launched = launched;
    g_idle_add(jobFinishedOnMain, job);
}

/* A thread: the files the child needs, then the launch on the main loop. */
static void* jobPrepare(void* data)
{
    Job* job = data;
    g_mkdir_with_parents(AB_WORK, 0777);
    g_mkdir_with_parents(AB_STORE, 0777);
    /* The child runs under another uid. */
    chmod(AB_DIR, 0777);
    chmod(AB_WORK, 0777);
    chmod(AB_STORE, 0777);
    removeJobFiles();
    GKeyFile* file = g_key_file_new();
    g_key_file_set_string(file, "job", "mode", job->mode);
    g_key_file_set_string(file, "job", "from", job->from);
    g_key_file_set_string(file, "job", "store", AB_STORE);
    g_key_file_set_string(file, "job", "identifier", job->identifier);
    g_key_file_set_string(file, "job", "work", AB_WORK);
    g_key_file_set_string(file, "job", "result", AB_WORK "/result.ini");
    gboolean ok = TRUE;
    const char* failure = NULL;
    if (!strcmp(job->mode, "compile")) {
        const char* source = job->source;
        if (g_str_has_prefix(source, AB_BUNDLED "/")) { /* /app0 is the title's alone */
            gint64 start = g_get_monotonic_time();
            ok = copyFile(source, AB_WORK "/bundled-rules.json");
            spike_log("[adblock] copied %s for the child in %.0f ms%s", source, msSince(start), ok ? "" : " (FAILED)");
            source = AB_WORK "/bundled-rules.json";
            failure = ok ? NULL : "the bundled list could not be copied";
        }
        g_key_file_set_string(file, "job", "source", source);
        g_key_file_set_string(file, "job", "version", job->info.version ? job->info.version : "");
    } else {
        const char* converter = converterPath();
        ok = copyFile(converter, AB_WORK "/dnr-to-webkit.js") && copyFile(scriptingConverterPath(), AB_WORK "/ubol-scripting.js");
        failure = ok ? NULL : "the converters could not be copied";
        g_key_file_set_string(file, "job", "converter", AB_WORK "/dnr-to-webkit.js");
        g_key_file_set_string(file, "job", "scripting-converter", AB_WORK "/ubol-scripting.js");
        g_key_file_set_string(file, "job", "dest", AB_DIR);
        g_key_file_set_boolean(file, "job", "force", job->force);
        g_key_file_set_string(file, "job", "current-version", job->info.version ? job->info.version : "");
        g_key_file_set_integer(file, "job", "current-converter", job->info.converter);
    }
    if (ok && !writeKeyFile(file, AB_WORK "/job.ini")) {
        ok = FALSE;
        failure = "the job file could not be written";
    }
    g_key_file_unref(file);
    if (!ok) {
        spike_log("[adblock] %s job not started: %s", job->mode, failure);
        setStatus("The list work could not start (%s); the list in use stays.", failure);
        job->launched = FALSE;
        job->started = 0;
        g_idle_add(jobFinishedOnMain, job);
        return NULL;
    }
    g_idle_add(jobLaunchOnMain, job);
    return NULL;
}

static void jobLaunch(Job* job)
{
    WPEProcessManager* manager = wpe_process_manager_get_default();
    if (!manager
        || !wpe_process_manager_ps5_run_task(manager, "adblock", AB_WORK "/job.ini", jobOutput, jobEnded, job)) {
        spike_log("[adblock] %s job not started: no PS5 process manager", job->mode);
        job->launched = FALSE;
        jobFinishedOnMain(job);
    }
}

/* Main loop: a compile or an update in the child. NULL, or why not. */
static const char* startJob(const char* mode, const char* from, gboolean force, const ListInfo* info, const char* source)
{
    if (jobRunning)
        return !strcmp(mode, "update") ? "the list is being updated or compiled" : "another compile is running";
    Job* job = g_new0(Job, 1);
    job->mode = g_strdup(mode);
    job->from = g_strdup(from);
    job->force = force;
    listInfoCopy(&job->info, info);
    job->source = g_strdup(source);
    job->identifier = g_strdup(activeID && !strcmp(activeID, "ubol-a") ? "ubol-b" : "ubol-a");
    job->started = g_get_monotonic_time();
    jobRunning = TRUE;
    gboolean update = !strcmp(mode, "update");
    g_mutex_lock(&abLock);
    updating = update;
    compiling = !update;
    g_mutex_unlock(&abLock);
    struct stat st;
    if (update)
        spike_log("[adblock] update started (%s) in a child process; list in use %s (converter %d); result goes to %s", from,
            info->version ? info->version : "none", info->converter, job->identifier);
    else
        spike_log("[adblock] compiling %s (uBO Lite %s, %s, %ld rules, %.1f MB) as %s in a child process: %s", source, info->version,
            info->origin, info->rules, stat(source, &st) ? -1.0 : st.st_size / 1048576.0, job->identifier, from);
    memLog(update ? "adblock: update start" : "adblock: compile start");
    setStatus(update ? "Checking for a new list…" : "Compiling the list %s…", info->version ? info->version : "");
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 256 * 1024);
    pthread_t thread;
    int rc = pthread_create(&thread, &attributes, jobPrepare, job);
    pthread_attr_destroy(&attributes);
    if (rc) {
        job->launched = FALSE;
        job->started = 0;
        jobFinishedOnMain(job);
        return "no thread for the job";
    }
    pthread_detach(thread);
    return NULL;
}

typedef struct {
    ListInfo info;
    char* identifier;
    gint64 started;
    double jobSeconds;
    double compileSeconds;
} NewListLoad;

/* Main loop: the child's compiled list, loaded (the 6 ms path). */
static void newListLoaded(GObject* object, GAsyncResult* result, gpointer data)
{
    NewListLoad* load = data;
    GError* error = NULL;
    WebKitUserContentFilter* filter = webkit_user_content_filter_store_load_finish(WEBKIT_USER_CONTENT_FILTER_STORE(object), result, &error);
    double ms = msSince(load->started);
    if (filter) {
        gint64 now = g_get_real_time() / G_USEC_PER_SEC;
        spike_log("[adblock] loaded the new list %s (uBO Lite %s, %s, converter %d, %ld rules) in %.0f ms; the child took %.1f s (compile %.1f s)",
            load->identifier, load->info.version, load->info.origin, load->info.converter, load->info.rules, ms, load->jobSeconds, load->compileSeconds);
        GKeyFile* file = g_key_file_new();
        infoToKeyFile(file, &load->info);
        g_key_file_set_string(file, "compiled", "identifier", load->identifier);
        g_key_file_set_int64(file, "compiled", "at", now);
        g_key_file_set_int64(file, "compiled", "ms", (gint64)(load->compileSeconds * 1000));
        if (!writeKeyFile(file, AB_DIR "/compiled.ini"))
            spike_log("[adblock] %s/compiled.ini not written", AB_DIR);
        g_key_file_unref(file);
        useFilter(filter, load->identifier, &load->info, now);
        setStatus("Using uBO Lite %s (%ld rules), compiled in %.1f s.", load->info.version, load->info.rules, load->compileSeconds);
    } else {
        spike_log("[adblock] the new list %s was compiled but did not load: %s; keeping %s", load->identifier,
            error ? error->message : "?", activeID ? activeID : "no list");
        setStatus("The list %s was compiled but could not be loaded (%s); the list in use stays.", load->info.version,
            error ? error->message : "unknown error");
        g_clear_error(&error);
    }
    memLog("adblock: new list in use");
    listInfoClear(&load->info);
    g_free(load->identifier);
    g_free(load);
}

static gboolean jobFinishedOnMain(gpointer data)
{
    Job* job = data;
    gboolean update = !strcmp(job->mode, "update");
    double seconds = job->started ? msSince(job->started) / 1000 : 0;
    GKeyFile* result = g_key_file_new();
    gboolean haveResult = job->launched && g_key_file_load_from_file(result, AB_WORK "/result.ini", G_KEY_FILE_NONE, NULL);
    char* outcome = haveResult ? g_key_file_get_string(result, "result", "status", NULL) : NULL;
    char* message = haveResult ? g_key_file_get_string(result, "result", "message", NULL) : NULL;
    double compileSeconds = haveResult ? g_key_file_get_int64(result, "result", "compile-ms", NULL) / 1000.0 : 0;
    long childPeak = haveResult ? (long)g_key_file_get_int64(result, "result", "peak-rss-mb", NULL) : -1;
    removeJobFiles();
    memLog("adblock: child ended");
    spike_log("[adblock] %s job (%s) ended after %.1f s: %s%s%s; child peak rss %ld MB", job->mode, job->from, seconds,
        !job->launched ? "the child did not start" : outcome ? outcome : "no result (the child failed or was ended)",
        message && *message ? ": " : "", message ? message : "", childPeak);
    gboolean newList = outcome && !strcmp(outcome, "ok");
    ListInfo info = { 0 };
    if (newList && update && !readInfo(AB_DIR "/adblock-info.ini", &info)) {
        newList = FALSE;
        g_free(message);
        message = g_strdup("The new list's adblock-info.ini is missing; the list in use stays.");
    } else if (newList && !update)
        listInfoCopy(&info, &job->info);
    if (newList) {
        NewListLoad* load = g_new0(NewListLoad, 1);
        load->info = info;
        load->identifier = g_strdup(job->identifier);
        load->started = g_get_monotonic_time();
        load->jobSeconds = seconds;
        load->compileSeconds = compileSeconds;
        webkit_user_content_filter_store_load(store, load->identifier, NULL, newListLoaded, load);
        if (update)
            loadSiteData(AB_DIR, "updated list"); /* saved before the list's info file */
    } else if (outcome && !strcmp(outcome, "uptodate"))
        setStatus("%s", message && *message ? message : "The list is up to date.");
    else if (!job->launched)
        setStatus("The list work could not start (the child process did not run); the list in use stays.");
    else if (outcome)
        setStatus("%s", message && *message ? message : "The list work failed; the list in use stays.");
    else
        setStatus("The list work ended without a result after %.0f s (the child process failed); the list in use stays.", seconds);
    g_mutex_lock(&abLock);
    if (update)
        lastCheckResult = g_get_real_time() / G_USEC_PER_SEC;
    updating = FALSE;
    compiling = FALSE;
    g_mutex_unlock(&abLock);
    jobRunning = FALSE;
    g_free(outcome);
    g_free(message);
    g_key_file_unref(result);
    jobFree(job);
    return G_SOURCE_REMOVE;
}

static void compileList(const char* path, const ListInfo* info, const char* why)
{
    const char* busy = startJob("compile", why, FALSE, info, path);
    if (busy)
        spike_log("[adblock] compile of %s skipped: %s", path, busy);
}

/* The list to use: the newer of the bundled and the downloaded one. */
static gboolean chooseSource(ListInfo* info, char** path)
{
    ListInfo bundled, downloaded;
    gboolean haveBundled = readInfo(AB_BUNDLED "/adblock-info.ini", &bundled) && fileExists(AB_BUNDLED "/adblock-rules.json");
    gboolean haveDownloaded = readInfo(AB_DIR "/adblock-info.ini", &downloaded) && fileExists(AB_DIR "/adblock-rules.json");
    if (haveBundled && (!haveDownloaded || compareLists(&bundled, &downloaded) > 0)) {
        *info = bundled;
        *path = g_strdup(AB_BUNDLED "/adblock-rules.json");
        if (haveDownloaded)
            listInfoClear(&downloaded);
        return TRUE;
    }
    if (haveBundled)
        listInfoClear(&bundled);
    if (haveDownloaded) {
        *info = downloaded;
        *path = g_strdup(AB_DIR "/adblock-rules.json");
        return TRUE;
    }
    memset(info, 0, sizeof *info);
    *path = NULL;
    return FALSE;
}

typedef struct {
    ListInfo compiled;
    gint64 compiledAt;
    char* identifier;
    gint64 started;
} LoadJob;

static void loaded(GObject* object, GAsyncResult* result, gpointer data)
{
    LoadJob* job = data;
    GError* error = NULL;
    WebKitUserContentFilter* filter = webkit_user_content_filter_store_load_finish(WEBKIT_USER_CONTENT_FILTER_STORE(object), result, &error);
    double ms = msSince(job->started);
    memLog("adblock: loaded");
    ListInfo source;
    char* path = NULL;
    gboolean haveSource = chooseSource(&source, &path);
    if (filter) {
        spike_log("[adblock] loaded compiled list %s (uBO Lite %s, %s, %ld rules) in %.0f ms", job->identifier, job->compiled.version,
            job->compiled.origin, job->compiled.rules, ms);
        useFilter(filter, job->identifier, &job->compiled, job->compiledAt);
        setStatus("Using uBO Lite %s (%ld rules).", job->compiled.version, job->compiled.rules);
        if (haveSource && g_strcmp0(source.sha256, job->compiled.sha256) && compareLists(&source, &job->compiled) >= 0)
            compileList(path, &source, "the source list changed");
    } else {
        spike_log("[adblock] compiled list %s not loaded after %.0f ms: %s", job->identifier, ms, error ? error->message : "?");
        g_clear_error(&error);
        if (haveSource)
            compileList(path, &source, "no compiled list to load");
    }
    if (haveSource)
        listInfoClear(&source);
    g_free(path);
    listInfoClear(&job->compiled);
    g_free(job->identifier);
    g_free(job);
}

/* ---- the update (in the child too) ---- */

static void writeLastCheck(gint64 when)
{
    char text[32];
    int length = snprintf(text, sizeof text, "%lld\n", (long long)when);
    writeAtomically(AB_DIR "/last-check", text, (gsize)length);
}

/* Main loop. */
static const char* startUpdate(gboolean force, const char* from)
{
    if (jobRunning)
        return "the list is being updated or compiled";
    g_mutex_lock(&abLock);
    lastCheck = g_get_real_time() / G_USEC_PER_SEC;
    ListInfo current = { 0 };
    listInfoCopy(&current, &active);
    g_mutex_unlock(&abLock);
    writeLastCheck(lastCheck);
    const char* why = startJob("update", from, force, &current, NULL);
    listInfoClear(&current);
    return why;
}

static gboolean startUpdateOnMain(gpointer data)
{
    const char* why = startUpdate(TRUE, data);
    if (why && !jobRunning) {
        g_mutex_lock(&abLock);
        updating = FALSE;
        g_mutex_unlock(&abLock);
    }
    if (why) {
        spike_log("[adblock] update (%s) not started: %s", (char*)data, why);
        setStatus("Not now: %s.", why);
    }
    g_free(data);
    return G_SOURCE_REMOVE;
}

const char* ps5wk_adblock_update(const char* from)
{
    g_mutex_lock(&abLock);
    gboolean busy = updating || compiling;
    if (!busy)
        updating = TRUE; /* the reply already says so; startJob confirms it */
    g_mutex_unlock(&abLock);
    if (busy)
        return "the list is being updated or compiled";
    g_idle_add(startUpdateOnMain, g_strdup(from));
    return NULL;
}

/* Main loop timer: the daily check, when the browser is idle. */
static gboolean autoCheck(gpointer data)
{
    if (fileExists(AB_NO_UPDATE_FLAG))
        return data ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    g_mutex_lock(&abLock);
    gboolean due = now - lastCheck >= AB_CHECK_INTERVAL_S || now < lastCheck;
    g_mutex_unlock(&abLock);
    if (!due || jobRunning)
        return data ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
    if (webkit_web_view_is_loading(webView) || ps5wk_vr_active()) {
        g_timeout_add_seconds(30, autoCheck, GINT_TO_POINTER(1)); /* once, soon */
        return data ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
    }
    startUpdate(FALSE, "daily check");
    return data ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

static gboolean firstCheck(gpointer data)
{
    (void)data;
    autoCheck(NULL);
    g_timeout_add_seconds(AB_RECHECK_S, autoCheck, NULL);
    return G_SOURCE_REMOVE;
}

/* The compiled list not in use (keep: the slot compiled.ini names). A title
 * can't list directories (EPERM), so only known names: WebKit's own
 * temporary files (ContentRuleList-XXXXXX, SerializedNFA-XXXXXX in TMPDIR)
 * are deleted by WebKit itself (remove() in titlesys.c). */
static void removeUnusedSlot(const char* keep)
{
    const char* slots[] = { "ubol-a", "ubol-b" };
    for (unsigned i = 0; i < G_N_ELEMENTS(slots); i++) {
        if (keep && !strcmp(keep, slots[i]))
            continue;
        char* path = g_strdup_printf(AB_STORE "/ContentRuleList-%s", slots[i]);
        if (!unlink(path))
            spike_log("[adblock] removed the unused compiled list %s", path);
        g_free(path);
    }
}

/* The page shown changed (the browser's tabs share the content manager). */
void ps5wk_adblock_set_web_view(WebKitWebView* view)
{
    webView = view;
}

void ps5wk_adblock_init(WebKitWebView* view)
{
    webView = view;
    contentManager = webkit_web_view_get_user_content_manager(view);
    mkdir(AB_DIR, 0777);
    removeJobFiles(); /* of a job the title did not see end (the next child clears its temporary files) */
    store = webkit_user_content_filter_store_new(AB_STORE);
    readSites();
    enabled = !fileExists(AB_OFF_FLAG);
    gchar* last = NULL;
    if (g_file_get_contents(AB_DIR "/last-check", &last, NULL, NULL))
        lastCheck = g_ascii_strtoll(last, NULL, 10);
    g_free(last);
    spike_log("[adblock] %s; %u allowed site(s); store %s", enabled ? "on" : "off in Settings", sites->len, AB_STORE);
    scriptingDisabled = fileExists(AB_SCRIPTING_OFF_FLAG);
    if (scriptingDisabled)
        spike_log("[adblock] per-site cosmetic filters and scriptlets off (%s)", AB_SCRIPTING_OFF_FLAG);
    /* Phase 2's site data: the one that came with the list in use. A list
     * without it (downloaded before phase 2) gets the bundled site data and
     * an update soon. */
    {
        ListInfo source;
        char* path = NULL;
        if (chooseSource(&source, &path)) {
            char* dir = g_path_get_dirname(path);
            loadSiteData(dir, !strcmp(dir, AB_BUNDLED) ? "bundled list" : "downloaded list");
            if (!siteData && strcmp(dir, AB_BUNDLED)) {
                loadSiteData(AB_BUNDLED, "the downloaded list has none");
                lastCheck = 0;
            }
            g_free(dir);
            listInfoClear(&source);
        }
        g_free(path);
    }

    GKeyFile* file = g_key_file_new();
    char* identifier = NULL;
    if (g_key_file_load_from_file(file, AB_DIR "/compiled.ini", G_KEY_FILE_NONE, NULL))
        identifier = g_key_file_get_string(file, "compiled", "identifier", NULL);
    removeUnusedSlot(identifier);
    g_clear_pointer(&identifier, g_free);
    LoadJob* job = g_new0(LoadJob, 1);
    if ((identifier = g_key_file_get_string(file, "compiled", "identifier", NULL))
        && readInfo(AB_DIR "/compiled.ini", &job->compiled)) {
        job->compiledAt = g_key_file_get_int64(file, "compiled", "at", NULL);
        job->identifier = identifier;
        job->started = g_get_monotonic_time();
        activeID = g_strdup(identifier); /* the next compile goes to the other slot */
        webkit_user_content_filter_store_load(store, identifier, NULL, loaded, job);
    } else {
        g_free(identifier);
        g_free(job);
        ListInfo source;
        char* path = NULL;
        if (chooseSource(&source, &path)) {
            compileList(path, &source, "first start");
            listInfoClear(&source);
        } else {
            spike_log("[adblock] no list: neither %s nor %s has one", AB_BUNDLED, AB_DIR);
            setStatus("No list yet: press Update lists now.");
        }
        g_free(path);
    }
    g_key_file_unref(file);
    g_timeout_add_seconds(AB_FIRST_CHECK_S, firstCheck, NULL);
}

/* ---- /settings.json ---- */

static void appendJSONString(GString* out, const char* s)
{
    g_string_append_c(out, '"');
    for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\')
            g_string_append_printf(out, "\\%c", *p);
        else if (*p < 0x20)
            g_string_append_printf(out, "\\u%04x", *p);
        else
            g_string_append_c(out, (char)*p);
    }
    g_string_append_c(out, '"');
}

char* ps5wk_adblock_json(const char* uri)
{
    char* site = ps5wk_adblock_site_for_uri(uri);
    char* host = hostForURI(uri);
    GString* out = g_string_new("\"adblock\":{");
    g_mutex_lock(&abLock);
    const char* entry = siteEntryFor(host);
    g_string_append_printf(out, "\"enabled\":%s,\"filterOn\":%s,\"updating\":%s,\"compiling\":%s,\"lastCheck\":%lld,\"site\":",
        enabled ? "true" : "false", filterOn ? "true" : "false", updating ? "true" : "false", compiling ? "true" : "false",
        (long long)lastCheck);
    appendJSONString(out, site);
    g_string_append(out, ",\"siteAllowedBy\":");
    appendJSONString(out, entry);
    g_string_append(out, ",\"sites\":[");
    for (guint i = 0; sites && i < sites->len; i++) {
        if (i)
            g_string_append_c(out, ',');
        appendJSONString(out, g_ptr_array_index(sites, i));
    }
    g_string_append(out, "],\"list\":{\"version\":");
    appendJSONString(out, active.version);
    g_string_append_printf(out, ",\"rules\":%ld,\"dnrRules\":%ld,\"skipped\":%ld,\"generated\":%lld,\"compiled\":%lld,\"origin\":",
        active.rules, active.dnrRules, active.skipped, (long long)active.generated, (long long)activeCompiledAt);
    appendJSONString(out, active.origin);
    g_string_append(out, ",\"rulesets\":");
    appendJSONString(out, active.rulesets);
    g_string_append(out, "},\"scripting\":{\"version\":");
    appendJSONString(out, siteInfoVersion);
    g_string_append_printf(out, ",\"hosts\":%u,\"calls\":%u,\"isolatedCalls\":%u,\"cosmetics\":%u,\"procedurals\":%u,\"bytes\":%zu,\"sitesSetUp\":%u,\"site\":",
        siteInfo.hosts, siteInfo.calls[0], siteInfo.calls[1], siteInfo.cosmetics, siteInfo.procedurals, siteInfo.bytes + siteInfo.indexBytes, sitesSetUp);
    appendJSONString(out, lastSite);
    g_string_append_printf(out, ",\"siteCalls\":%u,\"siteIsolatedCalls\":%u,\"siteCSS\":%u,\"siteProcedurals\":%u}", lastCalls[0], lastCalls[1], lastCSS, lastProcedurals);
    g_string_append(out, ",\"status\":");
    appendJSONString(out, status);
    g_mutex_unlock(&abLock);
    g_string_append(out, "},");
    g_free(site);
    g_free(host);
    return g_string_free(out, FALSE);
}
