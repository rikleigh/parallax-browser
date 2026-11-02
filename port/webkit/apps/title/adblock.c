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
 * The update (Settings "Update lists now", or by itself once a day, 2 min
 * after start at the earliest and never while a page loads): a thread
 * fetches (as WebKit downloads, through the NetworkProcess: no TLS in the
 * title) uBO Lite's manifest.json from github.com/uBlockOrigin/uBOL-home,
 * and if its version is newer than the list in use, the default rulesets at
 * that release tag, runs dnr-to-webkit.js in a JavaScriptCore context of its
 * own (no JIT in the title: JSC_useJIT=false, ps5browser.c), writes the
 * list to /data/ps5webkit/adblock, and the main loop compiles it. Any
 * failure keeps the list in use.
 *
 * Off switches: the "Ad blocking" setting (/data/ps5webkit/adblock-off) and
 * the per-site allow list (/data/ps5webkit/adblock-allow.txt, one site per
 * line: a host and its subdomains). WebKit applies each filter on its own
 * (an exception in a second filter cannot undo a block in this one), so the
 * filter is taken off the user content manager while an allowed site is
 * shown and put back for the next page (ps5wk_adblock_navigation, from
 * load-changed). Logs: [adblock] lines.
 */
#define SPIKE_NAME "ps5browser"
#include "common/spike.h"
#include "adblock.h"
#include "settings.h"
#include "viewinvr.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>

#define AB_DIR "/data/ps5webkit/adblock"
#define AB_STORE "/data/ps5webkit/browser/contentfilters"
#define AB_BUNDLED "/app0/adblock"
#define AB_OFF_FLAG "/data/ps5webkit/adblock-off"
#define AB_ALLOW_FILE "/data/ps5webkit/adblock-allow.txt"
#define AB_NO_UPDATE_FLAG "/data/ps5webkit/adblock-noupdate"
#define AB_REPO "https://raw.githubusercontent.com/uBlockOrigin/uBOL-home"
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
 * Main loop: webView..compiling. Under abLock (read by the link server
 * thread): enabled, sites, status, active info, updating, lastCheck. */
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

static void readSites(void)
{
    gchar* contents = NULL;
    GPtrArray* list = g_ptr_array_new_with_free_func(g_free);
    if (g_file_get_contents(AB_ALLOW_FILE, &contents, NULL, NULL)) {
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
    gboolean ok = writeAtomically(AB_ALLOW_FILE, text->str, text->len);
    g_string_free(text, TRUE);
    return ok;
}

/* ---- the filter on the web view ---- */

static char* lastURI; /* main loop: the page the filter was last set for */

static gboolean isSettingsPage(const char* uri)
{
    return uri && g_str_has_prefix(uri, "http://127.0.0.1:9040/");
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
        return "could not save " AB_ALLOW_FILE;
    }
    SiteChange* change = g_new0(SiteChange, 1);
    change->site = site;
    change->reload = reload && changed;
    g_idle_add(siteChangedOnMain, change);
    return NULL;
}

/* ---- loading and compiling ---- */

static void startUpdateCompile(void);
static gboolean pendingUpdateCompile;

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

typedef struct {
    ListInfo info;
    char* identifier;
    char* path;
    gint64 started;
} CompileJob;

static void compileJobFree(CompileJob* job)
{
    listInfoClear(&job->info);
    g_free(job->identifier);
    g_free(job->path);
    g_free(job);
}

static void compiled(GObject* object, GAsyncResult* result, gpointer data)
{
    CompileJob* job = data;
    GError* error = NULL;
    WebKitUserContentFilter* filter = webkit_user_content_filter_store_save_from_file_finish(WEBKIT_USER_CONTENT_FILTER_STORE(object), result, &error);
    double ms = msSince(job->started);
    compiling = FALSE;
    memLog("adblock: compiled");
    if (!filter) {
        spike_log("[adblock] compiling %s (uBO Lite %s, %ld rules) FAILED after %.0f ms: %s; keeping %s", job->path, job->info.version,
            job->info.rules, ms, error ? error->message : "?", activeID ? activeID : "no list");
        setStatus("The list %s could not be compiled (%s); the list in use stays.", job->info.version, error ? error->message : "unknown error");
        g_clear_error(&error);
        compileJobFree(job);
        return;
    }
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    spike_log("[adblock] compiled uBO Lite %s (%s, converter %d): %ld rules in %.0f ms as %s", job->info.version, job->info.origin,
        job->info.converter, job->info.rules, ms, job->identifier);
    GKeyFile* file = g_key_file_new();
    infoToKeyFile(file, &job->info);
    g_key_file_set_string(file, "compiled", "identifier", job->identifier);
    g_key_file_set_int64(file, "compiled", "at", now);
    g_key_file_set_int64(file, "compiled", "ms", (gint64)ms);
    if (!writeKeyFile(file, AB_DIR "/compiled.ini"))
        spike_log("[adblock] %s/compiled.ini not written", AB_DIR);
    g_key_file_unref(file);
    useFilter(filter, job->identifier, &job->info, now);
    setStatus("Using uBO Lite %s (%ld rules), compiled in %.1f s.", job->info.version, job->info.rules, ms / 1000);
    compileJobFree(job);
    if (pendingUpdateCompile) {
        pendingUpdateCompile = FALSE;
        startUpdateCompile();
    }
}

/* Main loop: compile the rules at path (described by info). */
static void compileList(const char* path, const ListInfo* info, const char* why)
{
    if (compiling) {
        spike_log("[adblock] compile of %s skipped: another compile is running", path);
        return;
    }
    CompileJob* job = g_new0(CompileJob, 1);
    listInfoCopy(&job->info, info);
    job->identifier = g_strdup(activeID && !strcmp(activeID, "ubol-a") ? "ubol-b" : "ubol-a");
    job->path = g_strdup(path);
    job->started = g_get_monotonic_time();
    compiling = TRUE;
    struct stat st;
    spike_log("[adblock] compiling %s (uBO Lite %s, %s, %ld rules, %.1f MB) as %s: %s", path, info->version, info->origin,
        info->rules, stat(path, &st) ? -1.0 : st.st_size / 1048576.0, job->identifier, why);
    memLog("adblock: compile start");
    setStatus("Compiling the list %s…", info->version);
    GFile* file = g_file_new_for_path(path);
    webkit_user_content_filter_store_save_from_file(store, job->identifier, file, NULL, compiled, job);
    g_object_unref(file);
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

/* ---- the update ---- */

typedef struct {
    gboolean force;
    char* from;
    ListInfo current; /* the list in use when it started */
    /* results */
    gboolean wrote;   /* a new list is in AB_DIR */
    char* message;
    ListInfo info;
} UpdateJob;

/* Downloads go through WebKit (webkit_network_session_download_uri: the
 * NetworkProcess fetches, as for any page) rather than libsoup in this
 * process: TLS in the title ended it (2026-11-02, no log; OpenSSL there has
 * never worked). The update thread waits for each one. */
typedef struct {
    gint refs;
    char* url;
    char* path;
    GMutex lock;
    GCond cond;
    gboolean done;
    guint status;
    char* error;
    WebKitDownload* download; /* main loop */
} DownloadRequest;

static void downloadRequestUnref(DownloadRequest* request)
{
    if (!g_atomic_int_dec_and_test(&request->refs))
        return;
    g_free(request->url);
    g_free(request->path);
    g_free(request->error);
    g_mutex_clear(&request->lock);
    g_cond_clear(&request->cond);
    g_free(request);
}

static gboolean downloadDecideDestination(WebKitDownload* download, const char* suggested, gpointer data)
{
    (void)suggested;
    DownloadRequest* request = data;
    webkit_download_set_allow_overwrite(download, TRUE);
    webkit_download_set_destination(download, request->path);
    return TRUE;
}

static void downloadFailed(WebKitDownload* download, GError* error, gpointer data)
{
    (void)download;
    DownloadRequest* request = data;
    g_mutex_lock(&request->lock);
    if (!request->error)
        request->error = g_strdup(error ? error->message : "failed");
    g_mutex_unlock(&request->lock);
}

static void downloadFinished(WebKitDownload* download, gpointer data)
{
    DownloadRequest* request = data;
    WebKitURIResponse* response = webkit_download_get_response(download);
    g_mutex_lock(&request->lock);
    request->status = response ? webkit_uri_response_get_status_code(response) : 0;
    request->done = TRUE;
    g_cond_signal(&request->cond);
    g_mutex_unlock(&request->lock);
    g_signal_handlers_disconnect_by_data(download, request);
    request->download = NULL;
    g_object_unref(download);
    downloadRequestUnref(request);
}

static gboolean downloadStart(gpointer data)
{
    DownloadRequest* request = data;
    WebKitNetworkSession* session = webView ? webkit_web_view_get_network_session(webView) : NULL;
    if (!session) {
        downloadFailed(NULL, NULL, request);
        g_mutex_lock(&request->lock);
        request->done = TRUE;
        g_cond_signal(&request->cond);
        g_mutex_unlock(&request->lock);
        downloadRequestUnref(request);
        return G_SOURCE_REMOVE;
    }
    request->download = webkit_network_session_download_uri(session, request->url);
    g_signal_connect(request->download, "decide-destination", G_CALLBACK(downloadDecideDestination), request);
    g_signal_connect(request->download, "failed", G_CALLBACK(downloadFailed), request);
    g_signal_connect(request->download, "finished", G_CALLBACK(downloadFinished), request);
    return G_SOURCE_REMOVE;
}

static gboolean downloadCancel(gpointer data)
{
    DownloadRequest* request = data;
    if (request->download)
        webkit_download_cancel(request->download); /* "failed", "finished" */
    downloadRequestUnref(request);
    return G_SOURCE_REMOVE;
}

/* Update thread: url's body, or NULL (*missing for a 404 when optional). */
static GBytes* fetch(const char* url, gboolean optional, gboolean* missing, GError** error)
{
    static unsigned serial;
    DownloadRequest* request = g_new0(DownloadRequest, 1);
    request->refs = 2; /* this thread, the main loop */
    request->url = g_strdup(url);
    request->path = g_strdup_printf(AB_DIR "/download-%u.tmp", ++serial);
    g_mutex_init(&request->lock);
    g_cond_init(&request->cond);
    g_idle_add(downloadStart, request);
    gint64 deadline = g_get_monotonic_time() + 120 * G_USEC_PER_SEC;
    g_mutex_lock(&request->lock);
    while (!request->done) {
        if (!g_cond_wait_until(&request->cond, &request->lock, deadline)) {
            if (!request->error)
                request->error = g_strdup("no answer within 120 s");
            g_atomic_int_inc(&request->refs);
            g_idle_add(downloadCancel, request);
            deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
            if (!g_cond_wait_until(&request->cond, &request->lock, deadline))
                break;
        }
    }
    gboolean done = request->done;
    guint status = request->status;
    char* failure = g_strdup(request->error);
    g_mutex_unlock(&request->lock);
    GBytes* body = NULL;
    if (done && !failure && status == 200) {
        gchar* contents = NULL;
        gsize length = 0;
        if (g_file_get_contents(request->path, &contents, &length, error))
            body = g_bytes_new_take(contents, length);
    } else if (done && status == 404 && optional && missing) /* WebKit: "failed", Not Found */
        *missing = TRUE;
    else {
        char code[32];
        snprintf(code, sizeof code, "HTTP %u", status);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s for %s", failure ? failure : status ? code : "no response", url);
    }
    unlink(request->path);
    g_free(failure);
    downloadRequestUnref(request);
    return body;
}

/* The converter: the copy in /data/ps5webkit/adblock (tests) or the title's. */
static char* converterPath(void)
{
    if (fileExists(AB_DIR "/dnr-to-webkit.js"))
        return g_strdup(AB_DIR "/dnr-to-webkit.js");
    return g_strdup(AB_BUNDLED "/dnr-to-webkit.js");
}

static char* jscError(JSCContext* context)
{
    JSCException* exception = jsc_context_get_exception(context);
    if (!exception)
        return NULL;
    char* text = jsc_exception_report(exception);
    jsc_context_clear_exception(context);
    return text;
}

/* Long value of "key":<number> in a flat JSON object (the stats line). */
static long jsonNumber(const char* json, const char* key)
{
    char* pattern = g_strdup_printf("\"%s\":", key);
    const char* at = strstr(json, pattern);
    long value = at ? strtol(at + strlen(pattern), NULL, 10) : -1;
    g_free(pattern);
    return value;
}


/* The update thread: download, convert, write. */
static void runUpdate(UpdateJob* job)
{
    gint64 started = g_get_monotonic_time();
    GError* error = NULL;
    JSCVirtualMachine* vm = NULL;
    JSCContext* js = NULL;
    char* converted = NULL;
    GString* fetchedList = g_string_new(NULL);
    size_t fetchedBytes = 0;

    GBytes* manifest = fetch(AB_REPO "/main/chromium/manifest.json", FALSE, NULL, &error);
    if (!manifest) {
        job->message = g_strdup_printf("Could not check for a new list: %s", error ? error->message : "download failed");
        goto done;
    }
    double fetchManifestMS = msSince(started);

    /* JavaScriptCore with the converter (also reads the manifest). */
    gint64 jsStart = g_get_monotonic_time();
    char* scriptPath = converterPath();
    gchar* script = NULL;
    gsize scriptLength = 0;
    if (!g_file_get_contents(scriptPath, &script, &scriptLength, &error)) {
        job->message = g_strdup_printf("The converter %s is missing: %s", scriptPath, error ? error->message : "?");
        g_free(scriptPath);
        goto done;
    }
    vm = jsc_virtual_machine_new();
    js = jsc_context_new_with_virtual_machine(vm);
    JSCValue* ignored = jsc_context_evaluate_with_source_uri(js, script, (gssize)scriptLength, scriptPath, 1);
    g_clear_object(&ignored);
    g_free(script);
    char* jsFailure = jscError(js);
    if (jsFailure) {
        job->message = g_strdup_printf("The converter %s did not load: %s", scriptPath, jsFailure);
        g_free(jsFailure);
        g_free(scriptPath);
        goto done;
    }
    g_free(scriptPath);
    JSCValue* converter = jsc_context_get_value(js, "DNRToWebKit");
    JSCValue* manifestText = jsc_value_new_string_from_bytes(js, manifest);
    JSCValue* manifestInfo = jsc_value_object_invoke_method(converter, "rulesetsFromManifest", JSC_TYPE_VALUE, manifestText, G_TYPE_NONE);
    g_object_unref(manifestText);
    char* manifestJSON = manifestInfo && jsc_value_is_string(manifestInfo) ? jsc_value_to_string(manifestInfo) : NULL;
    g_clear_object(&manifestInfo);
    jsFailure = jscError(js);
    if (!manifestJSON || jsFailure) {
        job->message = g_strdup_printf("uBO Lite's manifest was not understood: %s", jsFailure ? jsFailure : "no answer");
        g_free(jsFailure);
        g_free(manifestJSON);
        g_object_unref(converter);
        goto done;
    }
    JSCValue* parsed = jsc_value_new_from_json(js, manifestJSON);
    g_free(manifestJSON);
    JSCValue* versionValue = jsc_value_object_get_property(parsed, "version");
    JSCValue* idsValue = jsc_value_object_get_property(parsed, "rulesets");
    char* version = jsc_value_to_string(versionValue);
    JSCValue* lengthValue = jsc_value_object_get_property(idsValue, "length");
    int count = jsc_value_to_int32(lengthValue);
    g_object_unref(lengthValue);
    GPtrArray* ids = g_ptr_array_new_with_free_func(g_free);
    for (int i = 0; i < count; i++) {
        JSCValue* item = jsc_value_object_get_property_at_index(idsValue, (guint)i);
        g_ptr_array_add(ids, jsc_value_to_string(item));
        g_object_unref(item);
    }
    g_object_unref(versionValue);
    g_object_unref(idsValue);
    g_object_unref(parsed);

    ListInfo candidate = { 0 };
    candidate.version = g_strdup(version);
    JSCValue* converterVersion = jsc_value_object_get_property(converter, "VERSION");
    candidate.converter = jsc_value_to_int32(converterVersion);
    g_object_unref(converterVersion);
    gboolean newer = compareLists(&candidate, &job->current) > 0;
    spike_log("[adblock] update check (%s): uBO Lite %s on GitHub (converter %d), in use %s (converter %d)%s; manifest in %.0f ms, JavaScriptCore and converter ready in %.0f ms",
        job->from, version, candidate.converter, job->current.version ? job->current.version : "none", job->current.converter,
        newer ? "" : job->force ? ": not newer, converting anyway (asked from Settings)" : ": up to date", fetchManifestMS, msSince(jsStart));
    if (!newer && !job->force) {
        job->message = g_strdup_printf("The list is up to date (uBO Lite %s).", job->current.version ? job->current.version : version);
        listInfoClear(&candidate);
        g_free(version);
        g_ptr_array_unref(ids);
        g_object_unref(converter);
        goto done;
    }

    /* The rulesets at that release's tag. */
    gint64 downloadStart = g_get_monotonic_time();
    JSCValue* inputs = jsc_value_new_object(js, NULL, NULL);
    for (guint i = 0; i < ids->len && !job->message; i++) {
        const char* id = g_ptr_array_index(ids, i);
        for (int regex = 0; regex < 2 && !job->message; regex++) {
            char* url = g_strdup_printf(AB_REPO "/%s/chromium/rulesets/%s/%s.json", version, regex ? "regex" : "main", id);
            gboolean missing = FALSE;
            GBytes* body = fetch(url, regex, &missing, &error);
            if (body) {
                char* key = g_strdup_printf("%s%s", id, regex ? ".regex" : "");
                JSCValue* text = jsc_value_new_string_from_bytes(js, body);
                jsc_value_object_set_property(inputs, key, text);
                g_object_unref(text);
                fetchedBytes += g_bytes_get_size(body);
                g_string_append_printf(fetchedList, "%s%s", fetchedList->len ? ", " : "", key);
                g_free(key);
                g_bytes_unref(body);
            } else if (!missing)
                job->message = g_strdup_printf("Could not download the list: %s", error ? error->message : url);
            g_clear_error(&error);
            g_free(url);
        }
    }
    double downloadMS = msSince(downloadStart);
    if (job->message) {
        g_object_unref(inputs);
        listInfoClear(&candidate);
        g_free(version);
        g_ptr_array_unref(ids);
        g_object_unref(converter);
        goto done;
    }
    spike_log("[adblock] downloaded uBO Lite %s: %s (%.1f MB) in %.0f ms", version, fetchedList->str, fetchedBytes / 1048576.0, downloadMS);
    memLog("adblock: downloaded");

    gint64 convertStart = g_get_monotonic_time();
    JSCValue* output = jsc_value_object_invoke_method(converter, "convertJSON", JSC_TYPE_VALUE, inputs, G_TYPE_NONE);
    g_object_unref(inputs);
    g_object_unref(converter);
    jsFailure = jscError(js);
    if (!jsFailure && output && jsc_value_is_string(output))
        converted = jsc_value_to_string(output);
    g_clear_object(&output);
    double convertMS = msSince(convertStart);
    memLog("adblock: converted (JSC context alive)");
    g_clear_object(&js);
    g_clear_object(&vm);
    memLog("adblock: JSC context released");
    if (!converted) {
        job->message = g_strdup_printf("The conversion failed: %s", jsFailure ? jsFailure : "no result");
        spike_log("[adblock] conversion of uBO Lite %s failed after %.0f ms: %s", version, convertMS, jsFailure ? jsFailure : "no result");
        g_free(jsFailure);
        listInfoClear(&candidate);
        g_free(version);
        g_ptr_array_unref(ids);
        goto done;
    }
    char* newline = strchr(converted, '\n');
    long rules = newline ? jsonNumber(converted, "output") : -1;
    if (!newline || rules <= 0 || newline[1] != '[') {
        job->message = g_strdup("The conversion gave no rules.");
        listInfoClear(&candidate);
        g_free(version);
        g_ptr_array_unref(ids);
        goto done;
    }
    *newline = 0;
    const char* rulesText = newline + 1;
    size_t rulesLength = strlen(rulesText);
    spike_log("[adblock] converted uBO Lite %s in %.0f ms: %s; %.1f MB of WebKit JSON", version, convertMS, converted, rulesLength / 1048576.0);

    candidate.rules = rules;
    candidate.dnrRules = jsonNumber(converted, "input");
    candidate.skipped = jsonNumber(converted, "skipped");
    candidate.generated = g_get_real_time() / G_USEC_PER_SEC;
    candidate.origin = g_strdup("update");
    candidate.sha256 = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar*)rulesText, rulesLength);
    GString* joined = g_string_new(NULL);
    for (guint i = 0; i < ids->len; i++)
        g_string_append_printf(joined, "%s%s", i ? ";" : "", (char*)g_ptr_array_index(ids, i));
    candidate.rulesets = g_string_free(joined, FALSE);
    g_ptr_array_unref(ids);
    g_free(version);

    /* The rules first, the info last: an info file always describes the
     * rules next to it. A failed write leaves the previous pair (the info
     * is removed first, so a half pair is never chosen). */
    gint64 writeStart = g_get_monotonic_time();
    unlink(AB_DIR "/adblock-info.ini");
    GKeyFile* file = g_key_file_new();
    infoToKeyFile(file, &candidate);
    gboolean ok = writeAtomically(AB_DIR "/adblock-rules.json", rulesText, rulesLength) && writeKeyFile(file, AB_DIR "/adblock-info.ini");
    g_key_file_unref(file);
    if (!ok) {
        job->message = g_strdup_printf("The new list could not be saved in %s (%s).", AB_DIR, g_strerror(errno));
        listInfoClear(&candidate);
        goto done;
    }
    spike_log("[adblock] saved uBO Lite %s (%ld rules) in %.0f ms; update took %.1f s so far", candidate.version, candidate.rules,
        msSince(writeStart), msSince(started) / 1000);
    job->info = candidate;
    job->wrote = TRUE;

done:
    g_clear_error(&error);
    g_clear_object(&js);
    g_clear_object(&vm);
    if (manifest)
        g_bytes_unref(manifest);
    g_free(converted);
    g_string_free(fetchedList, TRUE);
}

static gboolean updateFinished(gpointer data)
{
    UpdateJob* job = data;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    g_mutex_lock(&abLock);
    updating = FALSE;
    lastCheckResult = now;
    g_mutex_unlock(&abLock);
    if (job->wrote) {
        setStatus("Downloaded uBO Lite %s (%ld rules); compiling…", job->info.version, job->info.rules);
        if (compiling)
            pendingUpdateCompile = TRUE;
        else
            startUpdateCompile();
    } else {
        spike_log("[adblock] update (%s): %s", job->from, job->message ? job->message : "nothing to do");
        setStatus("%s", job->message ? job->message : "Nothing to do.");
    }
    listInfoClear(&job->current);
    listInfoClear(&job->info);
    g_free(job->from);
    g_free(job->message);
    g_free(job);
    return G_SOURCE_REMOVE;
}

static void* updateThread(void* data)
{
    runUpdate(data);
    g_idle_add(updateFinished, data);
    return NULL;
}

/* Main loop: compile what the update saved. */
static void startUpdateCompile(void)
{
    ListInfo info;
    if (!readInfo(AB_DIR "/adblock-info.ini", &info))
        return;
    compileList(AB_DIR "/adblock-rules.json", &info, "update");
    listInfoClear(&info);
}

static void writeLastCheck(gint64 when)
{
    char text[32];
    int length = snprintf(text, sizeof text, "%lld\n", (long long)when);
    writeAtomically(AB_DIR "/last-check", text, (gsize)length);
}

/* Main loop. */
static const char* startUpdate(gboolean force, const char* from)
{
    g_mutex_lock(&abLock);
    if (updating) {
        g_mutex_unlock(&abLock);
        return "an update is already running";
    }
    updating = TRUE;
    lastCheck = g_get_real_time() / G_USEC_PER_SEC;
    UpdateJob* job = g_new0(UpdateJob, 1);
    job->force = force;
    job->from = g_strdup(from);
    listInfoCopy(&job->current, &active);
    g_mutex_unlock(&abLock);
    writeLastCheck(lastCheck);
    setStatus("Checking for a new list…");
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 4 * 1024 * 1024);
    pthread_t thread;
    int rc = pthread_create(&thread, &attributes, updateThread, job);
    pthread_attr_destroy(&attributes);
    if (rc) {
        g_mutex_lock(&abLock);
        updating = FALSE;
        g_mutex_unlock(&abLock);
        listInfoClear(&job->current);
        g_free(job->from);
        g_free(job);
        setStatus("The update could not start (no thread).");
        return "no thread for the update";
    }
    pthread_detach(thread);
    spike_log("[adblock] update started (%s)", from);
    return NULL;
}

static gboolean startUpdateOnMain(gpointer data)
{
    startUpdate(TRUE, data);
    g_free(data);
    return G_SOURCE_REMOVE;
}

const char* ps5wk_adblock_update(const char* from)
{
    g_mutex_lock(&abLock);
    gboolean busy = updating;
    g_mutex_unlock(&abLock);
    if (busy)
        return "an update is already running";
    g_idle_add(startUpdateOnMain, g_strdup(from));
    return NULL;
}

/* Main loop timer: the daily check, when the browser is idle. */
static gboolean autoCheck(gpointer data)
{
    (void)data;
    if (fileExists(AB_NO_UPDATE_FLAG))
        return G_SOURCE_CONTINUE;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    g_mutex_lock(&abLock);
    gboolean due = !updating && (now - lastCheck >= AB_CHECK_INTERVAL_S || now < lastCheck);
    g_mutex_unlock(&abLock);
    if (!due || compiling)
        return G_SOURCE_CONTINUE;
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

void ps5wk_adblock_init(WebKitWebView* view)
{
    webView = view;
    contentManager = webkit_web_view_get_user_content_manager(view);
    mkdir(AB_DIR, 0777);
    store = webkit_user_content_filter_store_new(AB_STORE);
    readSites();
    enabled = !fileExists(AB_OFF_FLAG);
    gchar* last = NULL;
    if (g_file_get_contents(AB_DIR "/last-check", &last, NULL, NULL))
        lastCheck = g_ascii_strtoll(last, NULL, 10);
    g_free(last);
    spike_log("[adblock] %s; %u allowed site(s); store %s", enabled ? "on" : "off in Settings", sites->len, AB_STORE);

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
    g_string_append(out, "},\"status\":");
    appendJSONString(out, status);
    g_mutex_unlock(&abLock);
    g_string_append(out, "},");
    g_free(site);
    g_free(host);
    return g_string_free(out, FALSE);
}
