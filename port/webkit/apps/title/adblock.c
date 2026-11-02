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
#define AB_ALLOW_FILE "/data/ps5webkit/adblock-allow.txt"
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

/* The converter: the copy in /data/ps5webkit/adblock (tests) or the title's. */
static const char* converterPath(void)
{
    return fileExists(AB_DIR "/dnr-to-webkit.js") ? AB_DIR "/dnr-to-webkit.js" : AB_BUNDLED "/dnr-to-webkit.js";
}

static void removeJobFiles(void)
{
    unlink(AB_WORK "/job.ini");
    unlink(AB_WORK "/result.ini");
    unlink(AB_WORK "/bundled-rules.json");
    unlink(AB_WORK "/dnr-to-webkit.js");
    unlink(AB_WORK "/update-rules.json"); /* left by a child that was ended */
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
        ok = copyFile(converter, AB_WORK "/dnr-to-webkit.js");
        failure = ok ? NULL : "the converter could not be copied";
        g_key_file_set_string(file, "job", "converter", AB_WORK "/dnr-to-webkit.js");
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
