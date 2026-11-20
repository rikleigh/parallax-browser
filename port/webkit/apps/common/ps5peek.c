/*
 * ps5peek.c: what an archive holds; see ps5peek.h.
 */
#include "ps5peek.h"

#include "ps5archive.h"
#include "ps5fetch.h"
#include "ps5install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#define DEFAULT_TIMEOUT_MS 10000
#define DEFAULT_MAX_BYTES (24u << 20)
#define PARAM_MAX (256u * 1024)

static int64_t rangesReadAt(void* data, uint64_t offset, void* buffer, size_t length)
{
    return ps5fetch_ranges_read(data, offset, buffer, length);
}

static const char* rangesError(void* data)
{
    return ps5fetch_ranges_error(data);
}

/* The first "ABCD12345" in text standing on its own (a title ID in a
 * folder's or file's name). */
static gboolean titleIDIn(const char* text, char* out, size_t size)
{
    for (const char* p = text; p && *p; p++) {
        int ok = 1;
        for (int i = 0; i < 9 && ok; i++)
            ok = i < 4 ? (p[i] >= 'A' && p[i] <= 'Z') : (p[i] >= '0' && p[i] <= '9');
        if (ok && !(p[9] >= '0' && p[9] <= '9') && (p == text || !(p[-1] >= 'A' && p[-1] <= 'Z'))) {
            snprintf(out, size, "%.9s", p);
            return TRUE;
        }
    }
    return FALSE;
}

static void jsonString(GString* out, const char* text)
{
    if (!text) {
        g_string_append(out, "null");
        return;
    }
    g_string_append_c(out, '"');
    for (const unsigned char* c = (const unsigned char*)text; *c; c++) {
        if (*c == '"' || *c == '\\')
            g_string_append_printf(out, "\\%c", *c);
        else if (*c < 0x20)
            g_string_append_printf(out, "\\u%04x", *c);
        else
            g_string_append_c(out, (char)*c);
    }
    g_string_append_c(out, '"');
}

/* The last part of a path or an address (no query). */
static char* baseName(const char* where)
{
    const char* end = where + strcspn(where, "?#");
    const char* start = end;
    while (start > where && start[-1] != '/')
        start--;
    return g_strndup(start, (gsize)(end - start));
}

char* ps5peek_json(const PS5PeekOptions* options, char* error, size_t errorSize)
{
    if (error && errorSize)
        error[0] = 0;
    if (!options || (!options->url && !options->path)) {
        snprintf(error, errorSize, "Nothing to look at");
        return NULL;
    }
    gint64 started = g_get_monotonic_time();
    PS5ArchiveInfo info;
    memset(&info, 0, sizeof info);
    char why[256] = "";
    PS5FetchRanges* ranges = NULL;
    PS5ArchiveSource source = { 0 };
    int r;
    if (options->url) {
        PS5FetchRangesOptions o = { 0 };
        o.url = options->url;
        o.user_agent = options->user_agent;
        o.referer = options->referer;
        o.cookies = options->cookies;
        o.size = options->size;
        o.timeout_ms = options->timeout_ms ? options->timeout_ms : DEFAULT_TIMEOUT_MS;
        o.max_bytes = options->max_bytes ? options->max_bytes : DEFAULT_MAX_BYTES;
        ranges = ps5fetch_ranges_open(&o);
        source.data = ranges;
        source.size = options->size;
        source.read_at = rangesReadAt;
        source.error = rangesError;
        if (!ranges)
            snprintf(why, sizeof why, "No address or size");
        r = ranges ? ps5archive_inspect_source(&source, NULL, &info, why, sizeof why) : -2;
    } else
        r = ps5archive_inspect(options->path, NULL, &info, why, sizeof why);
    if (r == -2) {
        /* why the server's reads failed says more than libarchive's "read failed" */
        const char* network = ps5fetch_ranges_error(ranges);
        snprintf(error, errorSize, "%s", network ? network : why);
        ps5fetch_ranges_close(ranges);
        return NULL;
    }

    PS5TitleInfo title;
    memset(&title, 0, sizeof title);
    gboolean guessed = FALSE;
    if (!r && info.has_title) {
        char name[600];
        snprintf(name, sizeof name, "%s%ssce_sys/param.json", info.title_dir, *info.title_dir ? "/" : "");
        char* text = g_malloc(PARAM_MAX);
        char problem[256] = "";
        if (ps5archive_read_entry(options->url ? NULL : options->path, options->url ? &source : NULL, name, text, PARAM_MAX, NULL, problem,
                sizeof problem)
            || ps5install_parse_param(text, &title))
            memset(&title, 0, sizeof title);
        g_free(text);
        if (!*title.title_id) {
            char* base = baseName(options->url ? options->url : options->path);
            guessed = titleIDIn(info.title_dir, title.title_id, sizeof title.title_id)
                || titleIDIn(base, title.title_id, sizeof title.title_id);
            g_free(base);
        }
    }
    PS5TitleInfo installed;
    gboolean isInstalled = *title.title_id && ps5install_installed(options->user_app, title.title_id, &installed);
    unsigned requests = 0;
    uint64_t fetched = 0;
    ps5fetch_ranges_stats(ranges, &requests, &fetched);
    ps5fetch_ranges_close(ranges);

    GString* json = g_string_new("{\"format\":");
    jsonString(json, info.format);
    g_string_append_printf(json, ",\"entries\":%u,\"unpacked\":%llu,\"sizesKnown\":%s,\"titles\":%u,\"titleDir\":", info.entries,
        (unsigned long long)info.unpacked, info.sizes_known ? "true" : "false", info.titles);
    jsonString(json, info.has_title ? info.title_dir : NULL);
    g_string_append(json, ",\"titleId\":");
    jsonString(json, *title.title_id ? title.title_id : NULL);
    g_string_append(json, ",\"name\":");
    jsonString(json, *title.name ? title.name : NULL);
    g_string_append(json, ",\"version\":");
    jsonString(json, *title.version ? title.version : NULL);
    g_string_append_printf(json, ",\"guessed\":%s,\"installed\":", guessed ? "true" : "false");
    jsonString(json, isInstalled ? (*installed.version ? installed.version : "?") : NULL);
    g_string_append(json, ",\"refused\":");
    jsonString(json, r == -1 ? why : NULL);
    g_string_append_printf(json, ",\"remote\":%s,\"requests\":%u,\"fetched\":%llu,\"ms\":%lld}", options->url ? "true" : "false", requests,
        (unsigned long long)fetched, (long long)((g_get_monotonic_time() - started) / 1000));
    char* out = strdup(json->str);
    g_string_free(json, TRUE);
    return out;
}
