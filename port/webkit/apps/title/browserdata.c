/*
 * Browser data (ps5browser): bookmarks, history and the saved tabs, as JSON
 * files in the PS5 user's folder (/data/ps5webkit/users/<id>, profile.c),
 * plus /data/ps5webkit/start-data.js for the start page. See
 * browser.h and port/webkit/README.md ("Tabs, bookmarks, history").
 *
 *   bookmarks.json  {"version":1,"bookmarks":[{"url","title","added"}]}, newest first
 *   history.json    {"version":1,"history":[{"url","title","last","visits"}]},
 *                   newest first, at most HISTORY_MAX pages (http/https only)
 *   session.json    {"version":1,"active":N,"tabs":[{"url","title","scrollX",
 *                   "scrollY","form","state"}]}; state: WebKit's serialized
 *                   session state of the tab (its back/forward list), base64;
 *                   form 1: the page shown is a form's (POST) result
 *   downloads.json  {"version":1,"downloads":[{"id","url","finalUrl","file","mime",
 *                   "size","received","state","etag","lastModified","started",
 *                   "finished","error","sha256","restarted"}]}, newest first
 *                   (downloads.c keeps the list and decides when to write it)
 *   sites.json      {"version":1,"sites":[{"host","gamepad","keys","left","right",
 *                   "changed"}]}, most recently changed first: per-site
 *                   settings (gamepad mode and its button mapping; README:
 *                   "Gamepad mode"). In private mode changes go to a list in
 *                   memory that goes with the private session.
 *   start-data.js   window.ps5wkData = {bookmarks:[...], top:[...]}: what the
 *                   start page (a file: page, the only kind that can load it)
 *                   shows; web pages can't read file: URLs
 *
 * Every file is written to <name>.tmp, synced and renamed over the old one,
 * so a crash or power cut leaves the old or the new file, never half of one.
 * Changes are written a few seconds later (one write for a burst of
 * changes) and at the wind-down. Main thread only. No sync, no telemetry.
 */
#define SPIKE_NAME "ps5browser"
#include "common/spike.h"
#include "browser.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Next to start.html (a file: page loads it by a relative URL), not in the
 * user's folder: written at every start for the user who started it. */
#define START_DATA "/data/ps5webkit/start-data.js"
#define HISTORY_MAX 1000
#define BOOKMARKS_MAX 500
#define START_BOOKMARKS 24
#define START_TOP 12
#define BOOKMARKS_DELAY_S 1
#define HISTORY_DELAY_S 10

static char* dataDir;
static GPtrArray* bookmarks; /* PS5WKEntry*, newest first */
static GPtrArray* history;   /* PS5WKEntry*, newest first */
static GPtrArray* sites;        /* PS5WKSite*, most recently changed first */
static GPtrArray* privateSites; /* the same while private, or NULL */
static gboolean bookmarksDirty, historyDirty, sitesDirty;
static guint saveTimer;

/* ---------------------------------------------------------------- JSON */

void ps5wk_json_string(GString* out, const char* s)
{
    g_string_append_c(out, '"');
    for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\')
            g_string_append_printf(out, "\\%c", *p);
        else if (*p < 0x20 || *p == 0x7f)
            g_string_append_printf(out, "\\u%04x", *p);
        else if (p[0] == 0xe2 && p[1] == 0x80 && (p[2] == 0xa8 || p[2] == 0xa9)) {
            /* U+2028/2029: valid JSON, but not valid in a JS string literal (start-data.js) */
            g_string_append_printf(out, "\\u%04x", p[2] == 0xa8 ? 0x2028 : 0x2029);
            p += 2;
        } else
            g_string_append_c(out, (char)*p);
    }
    g_string_append_c(out, '"');
}

typedef enum { J_NULL, J_BOOL, J_NUMBER, J_STRING, J_ARRAY, J_OBJECT } JType;
typedef struct JNode {
    JType type;
    double number;
    char* string;
    GPtrArray* items; /* JNode* (array values, object values) */
    GPtrArray* keys;  /* char* (object keys) */
} JNode;

static void jfree(gpointer data)
{
    JNode* node = data;
    if (!node)
        return;
    g_free(node->string);
    if (node->items)
        g_ptr_array_unref(node->items);
    if (node->keys)
        g_ptr_array_unref(node->keys);
    g_free(node);
}

static void jspace(const char** p)
{
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r')
        (*p)++;
}

static char* jparseString(const char** p)
{
    if (**p != '"')
        return NULL;
    (*p)++;
    GString* out = g_string_new(NULL);
    while (**p && **p != '"') {
        if (**p != '\\') {
            g_string_append_c(out, *(*p)++);
            continue;
        }
        (*p)++;
        char c = *(*p)++;
        switch (c) {
        case 'n': g_string_append_c(out, '\n'); break;
        case 't': g_string_append_c(out, '\t'); break;
        case 'r': g_string_append_c(out, '\r'); break;
        case 'b': g_string_append_c(out, '\b'); break;
        case 'f': g_string_append_c(out, '\f'); break;
        case 'u': {
            unsigned cp = 0;
            for (int i = 0; i < 4; i++) {
                if (!g_ascii_isxdigit(**p)) {
                    g_string_free(out, TRUE);
                    return NULL;
                }
                cp = cp * 16 + g_ascii_xdigit_value(*(*p)++);
            }
            if (cp >= 0xd800 && cp < 0xdc00 && (*p)[0] == '\\' && (*p)[1] == 'u') {
                unsigned low = 0;
                gboolean ok = TRUE;
                for (int i = 2; i < 6; i++)
                    ok = ok && g_ascii_isxdigit((*p)[i]);
                if (ok) {
                    for (int i = 2; i < 6; i++)
                        low = low * 16 + g_ascii_xdigit_value((*p)[i]);
                    if (low >= 0xdc00 && low < 0xe000) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                        *p += 6;
                    }
                }
            }
            if (cp && !(cp >= 0xd800 && cp < 0xe000))
                g_string_append_unichar(out, cp);
            break;
        }
        case 0:
            g_string_free(out, TRUE);
            return NULL;
        default: g_string_append_c(out, c); break;
        }
    }
    if (**p != '"') {
        g_string_free(out, TRUE);
        return NULL;
    }
    (*p)++;
    return g_string_free(out, FALSE);
}

static JNode* jparse(const char** p, int depth)
{
    jspace(p);
    if (depth > 16)
        return NULL;
    JNode* node = g_new0(JNode, 1);
    if (**p == '{' || **p == '[') {
        gboolean object = **p == '{';
        node->type = object ? J_OBJECT : J_ARRAY;
        node->items = g_ptr_array_new_with_free_func(jfree);
        if (object)
            node->keys = g_ptr_array_new_with_free_func(g_free);
        (*p)++;
        jspace(p);
        if (**p == (object ? '}' : ']')) {
            (*p)++;
            return node;
        }
        for (;;) {
            jspace(p);
            if (object) {
                char* key = jparseString(p);
                jspace(p);
                if (!key || **p != ':') {
                    g_free(key);
                    break;
                }
                (*p)++;
                g_ptr_array_add(node->keys, key);
            }
            JNode* value = jparse(p, depth + 1);
            if (!value) {
                if (object)
                    g_ptr_array_remove_index(node->keys, node->keys->len - 1);
                break;
            }
            g_ptr_array_add(node->items, value);
            jspace(p);
            if (**p == ',') {
                (*p)++;
                continue;
            }
            if (**p == (object ? '}' : ']')) {
                (*p)++;
                return node;
            }
            break;
        }
        jfree(node);
        return NULL;
    }
    if (**p == '"') {
        node->type = J_STRING;
        node->string = jparseString(p);
        if (!node->string) {
            jfree(node);
            return NULL;
        }
        return node;
    }
    if (!strncmp(*p, "true", 4) || !strncmp(*p, "false", 5)) {
        node->type = J_BOOL;
        node->number = **p == 't';
        *p += **p == 't' ? 4 : 5;
        return node;
    }
    if (!strncmp(*p, "null", 4)) {
        *p += 4;
        return node;
    }
    char* end = NULL;
    node->number = g_ascii_strtod(*p, &end);
    if (end == *p) {
        jfree(node);
        return NULL;
    }
    node->type = J_NUMBER;
    *p = end;
    return node;
}

static JNode* jget(JNode* object, const char* key)
{
    if (!object || object->type != J_OBJECT)
        return NULL;
    for (unsigned i = 0; i < object->keys->len; i++) {
        if (!strcmp(g_ptr_array_index(object->keys, i), key))
            return g_ptr_array_index(object->items, i);
    }
    return NULL;
}

static const char* jstr(JNode* object, const char* key)
{
    JNode* node = jget(object, key);
    return node && node->type == J_STRING ? node->string : NULL;
}

static double jnum(JNode* object, const char* key, double fallback)
{
    JNode* node = jget(object, key);
    return node && node->type == J_NUMBER ? node->number : fallback;
}

static JNode* readJSONFile(const char* name)
{
    char* path = g_build_filename(dataDir, name, NULL);
    gchar* text = NULL;
    gsize length = 0;
    JNode* root = NULL;
    if (g_file_get_contents(path, &text, &length, NULL)) {
        const char* p = text;
        root = jparse(&p, 0);
        if (!root || root->type != J_OBJECT) {
            spike_log("browser: %s is not a JSON object (%zu bytes): ignored", path, (size_t)length);
            jfree(root);
            root = NULL;
        }
    }
    g_free(text);
    g_free(path);
    return root;
}

static gboolean writeFileAtomically(const char* name, const char* contents, gsize length)
{
    char* path = g_path_is_absolute(name) ? g_strdup(name) : g_build_filename(dataDir, name, NULL);
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
    if (!ok)
        spike_log("browser: writing %s failed (errno %d)", path, errno);
    g_free(temporary);
    g_free(path);
    return ok;
}

/* ---------------------------------------------------------------- entries */

static void entryFree(gpointer data)
{
    PS5WKEntry* entry = data;
    g_free(entry->url);
    g_free(entry->title);
    g_free(entry);
}

static PS5WKEntry* entryNew(const char* url, const char* title, gint64 time, unsigned visits)
{
    PS5WKEntry* entry = g_new0(PS5WKEntry, 1);
    entry->url = g_strdup(url ? url : "");
    entry->title = g_strdup(title ? title : "");
    entry->time = time;
    entry->visits = visits;
    return entry;
}

static gint64 nowSeconds(void)
{
    return g_get_real_time() / G_USEC_PER_SEC;
}

void ps5wk_entries_json(GString* out, GPtrArray* entries, unsigned max)
{
    g_string_append_c(out, '[');
    for (unsigned i = 0; entries && i < entries->len && i < max; i++) {
        PS5WKEntry* entry = g_ptr_array_index(entries, i);
        g_string_append(out, i ? ",{\"url\":" : "{\"url\":");
        ps5wk_json_string(out, entry->url);
        g_string_append(out, ",\"title\":");
        ps5wk_json_string(out, entry->title);
        g_string_append_printf(out, ",\"time\":%" G_GINT64_FORMAT ",\"visits\":%u}", entry->time, entry->visits);
    }
    g_string_append_c(out, ']');
}

static void loadEntries(GPtrArray* into, const char* file, const char* listKey, const char* timeKey, unsigned max)
{
    JNode* root = readJSONFile(file);
    JNode* list = jget(root, listKey);
    for (unsigned i = 0; list && list->type == J_ARRAY && i < list->items->len && into->len < max; i++) {
        JNode* item = g_ptr_array_index(list->items, i);
        const char* url = jstr(item, "url");
        if (!url || !*url)
            continue;
        g_ptr_array_add(into, entryNew(url, jstr(item, "title"), (gint64)jnum(item, timeKey, 0), (unsigned)jnum(item, "visits", 0)));
    }
    jfree(root);
}

/* ---------------------------------------------------------------- saving */

static int compareVisits(gconstpointer a, gconstpointer b)
{
    const PS5WKEntry* x = *(PS5WKEntry* const*)a;
    const PS5WKEntry* y = *(PS5WKEntry* const*)b;
    if (x->visits != y->visits)
        return x->visits > y->visits ? -1 : 1;
    return x->time > y->time ? -1 : x->time < y->time;
}

/* For the start page: bookmarks and the most visited pages. */
static void writeStartData(void)
{
    GPtrArray* top = g_ptr_array_new();
    for (unsigned i = 0; i < history->len; i++) {
        PS5WKEntry* entry = g_ptr_array_index(history, i);
        if (entry->visits >= 2)
            g_ptr_array_add(top, entry);
    }
    g_ptr_array_sort(top, compareVisits);
    GString* out = g_string_new("/* written by ps5browser (browserdata.c) */\nwindow.ps5wkData = {\"bookmarks\":");
    ps5wk_entries_json(out, bookmarks, START_BOOKMARKS);
    g_string_append(out, ",\"top\":");
    ps5wk_entries_json(out, top, START_TOP);
    g_string_append(out, "};\n");
    writeFileAtomically(START_DATA, out->str, out->len);
    g_string_free(out, TRUE);
    g_ptr_array_unref(top);
}

static void saveNow(void)
{
    if (saveTimer) {
        g_source_remove(saveTimer);
        saveTimer = 0;
    }
    if (sitesDirty) {
        GString* out = g_string_new("{\"version\":1,\"sites\":[");
        for (unsigned i = 0; i < sites->len; i++) {
            PS5WKSite* site = g_ptr_array_index(sites, i);
            g_string_append(out, i ? ",\n{\"host\":" : "\n{\"host\":");
            ps5wk_json_string(out, site->host);
            g_string_append(out, ",\"gamepad\":");
            ps5wk_json_string(out, ps5wk_site_gamepad_name(site->gamepad));
            g_string_append(out, ",\"keys\":");
            ps5wk_json_string(out, site->keys ? site->keys : "");
            g_string_append(out, ",\"left\":");
            ps5wk_json_string(out, site->left ? site->left : "");
            g_string_append(out, ",\"right\":");
            ps5wk_json_string(out, site->right ? site->right : "");
            g_string_append_printf(out, ",\"changed\":%" G_GINT64_FORMAT "}", site->changed);
        }
        g_string_append(out, "]}\n");
        if (writeFileAtomically("sites.json", out->str, out->len))
            sitesDirty = FALSE;
        g_string_free(out, TRUE);
    }
    gboolean any = bookmarksDirty || historyDirty;
    if (bookmarksDirty) {
        GString* out = g_string_new("{\"version\":1,\"bookmarks\":[");
        for (unsigned i = 0; i < bookmarks->len; i++) {
            PS5WKEntry* entry = g_ptr_array_index(bookmarks, i);
            g_string_append(out, i ? ",\n{\"url\":" : "\n{\"url\":");
            ps5wk_json_string(out, entry->url);
            g_string_append(out, ",\"title\":");
            ps5wk_json_string(out, entry->title);
            g_string_append_printf(out, ",\"added\":%" G_GINT64_FORMAT "}", entry->time);
        }
        g_string_append(out, "]}\n");
        if (writeFileAtomically("bookmarks.json", out->str, out->len))
            bookmarksDirty = FALSE;
        g_string_free(out, TRUE);
    }
    if (historyDirty) {
        GString* out = g_string_new("{\"version\":1,\"history\":[");
        for (unsigned i = 0; i < history->len; i++) {
            PS5WKEntry* entry = g_ptr_array_index(history, i);
            g_string_append(out, i ? ",\n{\"url\":" : "\n{\"url\":");
            ps5wk_json_string(out, entry->url);
            g_string_append(out, ",\"title\":");
            ps5wk_json_string(out, entry->title);
            g_string_append_printf(out, ",\"last\":%" G_GINT64_FORMAT ",\"visits\":%u}", entry->time, entry->visits);
        }
        g_string_append(out, "]}\n");
        if (writeFileAtomically("history.json", out->str, out->len))
            historyDirty = FALSE;
        g_string_free(out, TRUE);
    }
    if (any)
        writeStartData();
}

static gboolean saveLater(gpointer data)
{
    (void)data;
    saveTimer = 0;
    saveNow();
    return G_SOURCE_REMOVE;
}

static void scheduleSave(unsigned seconds)
{
    if (saveTimer)
        return;
    saveTimer = g_timeout_add_seconds(seconds, saveLater, NULL);
}

void ps5wk_bdata_flush(void)
{
    saveNow();
}

static void siteFree(gpointer data);
static void loadSites(void);

void ps5wk_bdata_init(const char* dir)
{
    dataDir = g_strdup(dir);
    bookmarks = g_ptr_array_new_with_free_func(entryFree);
    history = g_ptr_array_new_with_free_func(entryFree);
    loadEntries(bookmarks, "bookmarks.json", "bookmarks", "added", BOOKMARKS_MAX);
    loadEntries(history, "history.json", "history", "last", HISTORY_MAX);
    sites = g_ptr_array_new_with_free_func(siteFree);
    loadSites();
    spike_log("browser: %u bookmarks, %u history entries, %u sites with settings", bookmarks->len, history->len, sites->len);
    writeStartData(); /* the last user's until now */
}

/* ---------------------------------------------------------------- sites */

#define SITES_MAX 300

static void siteFree(gpointer data)
{
    PS5WKSite* site = data;
    g_free(site->host);
    g_free(site->keys);
    g_free(site->left);
    g_free(site->right);
    g_free(site);
}

static PS5WKSite* siteCopy(const PS5WKSite* from, const char* host)
{
    PS5WKSite* site = g_new0(PS5WKSite, 1);
    site->host = g_strdup(host);
    if (from) {
        site->gamepad = from->gamepad;
        site->keys = g_strdup(from->keys);
        site->left = g_strdup(from->left);
        site->right = g_strdup(from->right);
        site->changed = from->changed;
    }
    return site;
}

const char* ps5wk_site_gamepad_name(int gamepad)
{
    return gamepad == PS5WK_GAMEPAD_ON ? "on" : gamepad == PS5WK_GAMEPAD_OFF ? "off" : "ask";
}

static int gamepadFromName(const char* name)
{
    return !g_strcmp0(name, "on") ? PS5WK_GAMEPAD_ON : !g_strcmp0(name, "off") ? PS5WK_GAMEPAD_OFF : PS5WK_GAMEPAD_ASK;
}

static void loadSites(void)
{
    JNode* root = readJSONFile("sites.json");
    JNode* list = jget(root, "sites");
    for (unsigned i = 0; list && list->type == J_ARRAY && i < list->items->len && sites->len < SITES_MAX; i++) {
        JNode* item = g_ptr_array_index(list->items, i);
        const char* host = jstr(item, "host");
        if (!host || !*host || ps5wk_site_find(host))
            continue;
        PS5WKSite* site = siteCopy(NULL, host);
        site->gamepad = gamepadFromName(jstr(item, "gamepad"));
        site->keys = g_strdup(jstr(item, "keys"));
        site->left = g_strdup(jstr(item, "left"));
        site->right = g_strdup(jstr(item, "right"));
        site->changed = (gint64)jnum(item, "changed", 0);
        g_ptr_array_add(sites, site);
    }
    jfree(root);
}

GPtrArray* ps5wk_sites(void)
{
    return privateSites ? privateSites : sites;
}

PS5WKSite* ps5wk_site_find(const char* host)
{
    GPtrArray* list = ps5wk_sites();
    for (unsigned i = 0; host && i < list->len; i++) {
        PS5WKSite* site = g_ptr_array_index(list, i);
        if (!g_ascii_strcasecmp(site->host, host))
            return site;
    }
    return NULL;
}

PS5WKSite* ps5wk_site_get(const char* host)
{
    g_return_val_if_fail(host && *host, NULL);
    PS5WKSite* site = ps5wk_site_find(host);
    if (!site) {
        site = siteCopy(NULL, host);
        g_ptr_array_insert(ps5wk_sites(), 0, site);
    }
    return site;
}

void ps5wk_site_changed(PS5WKSite* site)
{
    GPtrArray* list = ps5wk_sites();
    site->changed = nowSeconds();
    unsigned index;
    if (g_ptr_array_find(list, site, &index) && index) {
        g_ptr_array_steal_index(list, index);
        g_ptr_array_insert(list, 0, site);
    }
    while (list->len > SITES_MAX)
        g_ptr_array_remove_index(list, list->len - 1);
    if (list == sites) {
        sitesDirty = TRUE;
        scheduleSave(BOOKMARKS_DELAY_S);
    }
}

gboolean ps5wk_site_remove(const char* host)
{
    PS5WKSite* site = ps5wk_site_find(host);
    if (!site)
        return FALSE;
    GPtrArray* list = ps5wk_sites();
    g_ptr_array_remove(list, site);
    if (list == sites) {
        sitesDirty = TRUE;
        scheduleSave(BOOKMARKS_DELAY_S);
    }
    return TRUE;
}

void ps5wk_sites_set_private(gboolean enabled)
{
    if (!enabled) {
        if (privateSites)
            g_ptr_array_unref(privateSites);
        privateSites = NULL;
        return;
    }
    if (privateSites)
        return;
    /* A copy of the saved settings: what the private session changes goes
     * with it. */
    privateSites = g_ptr_array_new_with_free_func(siteFree);
    for (unsigned i = 0; i < sites->len; i++) {
        PS5WKSite* site = g_ptr_array_index(sites, i);
        g_ptr_array_add(privateSites, siteCopy(site, site->host));
    }
}

/* ---------------------------------------------------------------- bookmarks */

GPtrArray* ps5wk_bookmarks(void)
{
    return bookmarks;
}

int ps5wk_bookmark_find(const char* url)
{
    for (unsigned i = 0; url && i < bookmarks->len; i++) {
        if (!strcmp(((PS5WKEntry*)g_ptr_array_index(bookmarks, i))->url, url))
            return (int)i;
    }
    return -1;
}

gboolean ps5wk_bookmark_toggle(const char* url, const char* title)
{
    if (!url || !*url)
        return FALSE;
    int index = ps5wk_bookmark_find(url);
    bookmarksDirty = TRUE;
    scheduleSave(BOOKMARKS_DELAY_S);
    if (index >= 0) {
        g_ptr_array_remove_index(bookmarks, (unsigned)index);
        return FALSE;
    }
    if (bookmarks->len >= BOOKMARKS_MAX)
        g_ptr_array_remove_index(bookmarks, bookmarks->len - 1);
    g_ptr_array_insert(bookmarks, 0, entryNew(url, title && *title ? title : url, nowSeconds(), 0));
    return TRUE;
}

void ps5wk_bookmark_remove(unsigned index)
{
    if (index >= bookmarks->len)
        return;
    g_ptr_array_remove_index(bookmarks, index);
    bookmarksDirty = TRUE;
    scheduleSave(BOOKMARKS_DELAY_S);
}

/* ---------------------------------------------------------------- history */

GPtrArray* ps5wk_history(void)
{
    return history;
}

gboolean ps5wk_history_wanted(const char* url)
{
    if (!url || strlen(url) > 4096)
        return FALSE;
    if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://"))
        return FALSE;
    /* The title's own pages (Settings) */
    return !g_str_has_prefix(url, "http://127.0.0.1:9040/") && !g_str_has_prefix(url, "http://localhost:9040/");
}

void ps5wk_history_visit(const char* url, const char* title, gboolean newVisit)
{
    if (!ps5wk_history_wanted(url))
        return;
    PS5WKEntry* entry = NULL;
    for (unsigned i = 0; i < history->len; i++) {
        PS5WKEntry* e = g_ptr_array_index(history, i);
        if (!strcmp(e->url, url)) {
            entry = g_ptr_array_steal_index(history, i);
            break;
        }
    }
    if (!entry)
        entry = entryNew(url, title, 0, 0);
    if (title && *title) {
        g_free(entry->title);
        entry->title = g_strdup(title);
    }
    entry->time = nowSeconds();
    if (newVisit || !entry->visits)
        entry->visits++;
    g_ptr_array_insert(history, 0, entry);
    while (history->len > HISTORY_MAX)
        g_ptr_array_remove_index(history, history->len - 1);
    historyDirty = TRUE;
    scheduleSave(HISTORY_DELAY_S);
}

void ps5wk_history_set_title(const char* url, const char* title)
{
    if (!url || !title || !*title)
        return;
    for (unsigned i = 0; i < history->len && i < 8; i++) {
        PS5WKEntry* e = g_ptr_array_index(history, i);
        if (!strcmp(e->url, url)) {
            if (strcmp(e->title, title)) {
                g_free(e->title);
                e->title = g_strdup(title);
                historyDirty = TRUE;
                scheduleSave(HISTORY_DELAY_S);
            }
            return;
        }
    }
}

void ps5wk_history_remove(unsigned index)
{
    if (index >= history->len)
        return;
    g_ptr_array_remove_index(history, index);
    historyDirty = TRUE;
    scheduleSave(BOOKMARKS_DELAY_S);
}

void ps5wk_history_clear(void)
{
    g_ptr_array_set_size(history, 0);
    historyDirty = TRUE;
    scheduleSave(BOOKMARKS_DELAY_S);
}

/* ---------------------------------------------------------------- session */

void ps5wk_session_tab_free(gpointer data)
{
    PS5WKSessionTab* tab = data;
    if (!tab)
        return;
    g_free(tab->url);
    g_free(tab->title);
    if (tab->state)
        g_bytes_unref(tab->state);
    g_free(tab);
}

GPtrArray* ps5wk_session_load(int* active)
{
    JNode* root = readJSONFile("session.json");
    if (!root)
        return NULL;
    GPtrArray* tabs = g_ptr_array_new_with_free_func(ps5wk_session_tab_free);
    JNode* list = jget(root, "tabs");
    for (unsigned i = 0; list && list->type == J_ARRAY && i < list->items->len; i++) {
        JNode* item = g_ptr_array_index(list->items, i);
        const char* url = jstr(item, "url");
        if (!url || !*url)
            continue;
        PS5WKSessionTab* tab = g_new0(PS5WKSessionTab, 1);
        tab->url = g_strdup(url);
        tab->title = g_strdup(jstr(item, "title") ? jstr(item, "title") : "");
        tab->scrollX = jnum(item, "scrollX", 0);
        tab->scrollY = jnum(item, "scrollY", 0);
        tab->fromForm = jnum(item, "form", 0) != 0;
        const char* state = jstr(item, "state");
        if (state && *state) {
            gsize length = 0;
            guchar* bytes = g_base64_decode(state, &length);
            if (bytes && length)
                tab->state = g_bytes_new_take(bytes, length);
            else
                g_free(bytes);
        }
        g_ptr_array_add(tabs, tab);
    }
    *active = (int)jnum(root, "active", 0);
    if (*active < 0 || *active >= (int)tabs->len)
        *active = 0;
    jfree(root);
    return tabs;
}

void ps5wk_session_save(GPtrArray* tabs, int active)
{
    GString* out = g_string_new(NULL);
    g_string_append_printf(out, "{\"version\":1,\"active\":%d,\"tabs\":[", active);
    for (unsigned i = 0; tabs && i < tabs->len; i++) {
        PS5WKSessionTab* tab = g_ptr_array_index(tabs, i);
        g_string_append(out, i ? ",\n{\"url\":" : "\n{\"url\":");
        ps5wk_json_string(out, tab->url);
        g_string_append(out, ",\"title\":");
        ps5wk_json_string(out, tab->title);
        g_string_append_printf(out, ",\"scrollX\":%.0f,\"scrollY\":%.0f,\"form\":%d,\"state\":\"", tab->scrollX, tab->scrollY, tab->fromForm ? 1 : 0);
        if (tab->state) {
            gsize length = 0;
            const guchar* data = g_bytes_get_data(tab->state, &length);
            char* base64 = g_base64_encode(data, length);
            g_string_append(out, base64);
            g_free(base64);
        }
        g_string_append(out, "\"}");
    }
    g_string_append(out, "]}\n");
    writeFileAtomically("session.json", out->str, out->len);
    g_string_free(out, TRUE);
}

/* ---------------------------------------------------------------- downloads */

static const char* const downloadStates[] = { "running", "paused", "failed", "done", "cancelled" };

const char* ps5wk_download_state_name(int state)
{
    return state >= 0 && state < (int)G_N_ELEMENTS(downloadStates) ? downloadStates[state] : "failed";
}

void ps5wk_download_record_clear(PS5WKDownloadRecord* r)
{
    g_free(r->url);
    g_free(r->finalURL);
    g_free(r->file);
    g_free(r->mime);
    g_free(r->etag);
    g_free(r->lastModified);
    g_free(r->error);
    g_free(r->sha256);
    memset(r, 0, sizeof *r);
}

void ps5wk_download_record_free(gpointer data)
{
    if (!data)
        return;
    ps5wk_download_record_clear(data);
    g_free(data);
}

static char* jdup(JNode* item, const char* key)
{
    const char* value = jstr(item, key);
    return value && *value ? g_strdup(value) : NULL;
}

GPtrArray* ps5wk_downloads_load(void)
{
    GPtrArray* records = g_ptr_array_new_with_free_func(ps5wk_download_record_free);
    JNode* root = readJSONFile("downloads.json");
    JNode* list = jget(root, "downloads");
    for (unsigned i = 0; list && list->type == J_ARRAY && i < list->items->len; i++) {
        JNode* item = g_ptr_array_index(list->items, i);
        const char* file = jstr(item, "file");
        const char* url = jstr(item, "url");
        /* a name in the folder, nothing else */
        if (!file || !*file || !url || strchr(file, '/') || file[0] == '.')
            continue;
        PS5WKDownloadRecord* r = g_new0(PS5WKDownloadRecord, 1);
        r->id = (unsigned)jnum(item, "id", 0);
        r->url = g_strdup(url);
        r->finalURL = jdup(item, "finalUrl");
        r->file = g_strdup(file);
        r->mime = jdup(item, "mime");
        r->size = (gint64)jnum(item, "size", -1);
        r->received = (gint64)jnum(item, "received", 0);
        r->state = PS5WK_DL_FAILED;
        const char* state = jstr(item, "state");
        for (unsigned s = 0; state && s < G_N_ELEMENTS(downloadStates); s++) {
            if (!strcmp(state, downloadStates[s]))
                r->state = (int)s;
        }
        r->etag = jdup(item, "etag");
        r->lastModified = jdup(item, "lastModified");
        r->started = (gint64)jnum(item, "started", 0);
        r->finished = (gint64)jnum(item, "finished", 0);
        r->error = jdup(item, "error");
        r->sha256 = jdup(item, "sha256");
        r->restarted = jnum(item, "restarted", 0) != 0;
        g_ptr_array_add(records, r);
    }
    jfree(root);
    return records;
}

static void jsonMember(GString* out, const char* key, const char* value)
{
    g_string_append_printf(out, ",\"%s\":", key);
    ps5wk_json_string(out, value ? value : "");
}

void ps5wk_downloads_save(GPtrArray* records)
{
    GString* out = g_string_new("{\"version\":1,\"downloads\":[");
    for (unsigned i = 0; records && i < records->len; i++) {
        PS5WKDownloadRecord* r = g_ptr_array_index(records, i);
        g_string_append_printf(out, "%s{\"id\":%u", i ? ",\n" : "\n", r->id);
        jsonMember(out, "url", r->url);
        jsonMember(out, "finalUrl", r->finalURL);
        jsonMember(out, "file", r->file);
        jsonMember(out, "mime", r->mime);
        g_string_append_printf(out, ",\"size\":%" G_GINT64_FORMAT ",\"received\":%" G_GINT64_FORMAT ",\"state\":\"%s\"", r->size, r->received,
            ps5wk_download_state_name(r->state));
        jsonMember(out, "etag", r->etag);
        jsonMember(out, "lastModified", r->lastModified);
        g_string_append_printf(out, ",\"started\":%" G_GINT64_FORMAT ",\"finished\":%" G_GINT64_FORMAT, r->started, r->finished);
        jsonMember(out, "error", r->error);
        jsonMember(out, "sha256", r->sha256);
        g_string_append_printf(out, ",\"restarted\":%d}", r->restarted ? 1 : 0);
    }
    g_string_append(out, "]}\n");
    writeFileAtomically("downloads.json", out->str, out->len);
    g_string_free(out, TRUE);
}
