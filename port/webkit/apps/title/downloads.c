/*
 * Downloads (ps5browser). See downloads.h and port/webkit/README.md
 * ("Downloads").
 *
 * A response becomes a download when it is an attachment, when the main
 * frame gets a type the browser can't show (.zip, .elf, .pkg, octet-stream)
 * or when the link had a download attribute (browserui.c's decide-policy
 * asks ps5wk_downloads_response_policy). WebKit's NetworkProcess writes it
 * to /data/ps5webkit/downloads/<name>.part and renames it to <name> when it
 * is complete (patch 0075), so a big file never sits in memory; the title
 * only sees the signals of its WebKitDownload.
 *
 * Pause cancels the transfer and keeps the .part file. Resume (and every
 * automatic retry) asks the ORIGINAL address again (signed CDN addresses a
 * link redirects to expire) with "Range: bytes=N-", N the size of the .part
 * file, and If-Range with the validator the first response had (a strong
 * ETag, else Last-Modified): the NetworkProcess appends a 206 that starts at
 * N, refuses a 206 that starts anywhere else, and starts over on a 200 (no
 * range support, or the file changed), which the list then says. Before
 * any data is written (decide-destination) the title also checks the
 * free space against the size (statfs) and, for a 206, that its start and
 * total match what is on disk and what the first response said.
 *
 * Network errors and stalls (no data for STALL_S) are retried 4 times
 * (2, 5, 15, 30 s); data arriving resets the count. Downloads don't belong
 * to a tab: closing or discarding the tab leaves them running. The list is
 * saved every SAVE_S while something runs, on every change of state and at
 * the wind-down; one still marked running when the title starts again (it
 * was killed or crashed) shows as paused, and Resume continues it from
 * its .part file. A finished file is read on a worker thread for its
 * SHA-256, shown in the details.
 */
#define SPIKE_NAME "ps5browser"
#include "common/spike.h"
#include "browser.h"
#include "downloads.h"

#include <errno.h>
#include <fcntl.h>
#include <libsoup/soup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define LIST_MAX 200
#define SAVE_S 5
#define STALL_S 45
#define FREE_MARGIN (16 * 1024 * 1024LL) /* left free on /data after a download */
#define NAME_MAX_BYTES 180

typedef struct {
    PS5WKDownloadRecord r; /* first: what downloads.json keeps */
    WebKitDownload* download; /* the transfer now, or NULL */
    gint64 attemptFrom;       /* bytes on disk when this transfer started (its Range) */
    gint64 attemptBytes;      /* received in this transfer */
    gboolean attemptResume;   /* this transfer asked for a range */
    gboolean responded;       /* decide-destination ran (data may follow) */
    gboolean failedNow;       /* "failed" came for this transfer ("finished" follows) */
    /* why the transfer is being cancelled */
    gboolean pausing, cancelling, stalled, removing, restartFromZero;
    char* refused; /* decide-destination refused it: the message */
    gboolean announced; /* "Download started" shown */
    /* automatic retries */
    unsigned retries;
    guint retryTimer;
    gint64 retryAt; /* monotonic us */
    /* speed */
    gint64 sampleTime, sampleBytes, lastData;
    double speed; /* bytes/s */
    gboolean hashing;
} Download;

static WPEDisplayPS5* display;
static WebKitNetworkSession* networkSession;
static GPtrArray* list; /* Download*, newest first */
static unsigned nextID = 1;
static gboolean dirty;
static guint saveTimer, tickTimer;
static gboolean windingDown;
static char userAgent[256];

/* The link server's view (any thread). */
static GMutex snapLock;
static char* snapFinishedJSON;
static char* snapSummaryJSON;
static GPtrArray* snapFinishedNames; /* char* */

static void startAttempt(Download* d, gboolean fresh);
static void downloadFree(gpointer data);
static void changed(gboolean stateChanged);
static void hashNext(void);

/* ---------------------------------------------------------------- helpers */

static void downloadFree(gpointer data)
{
    Download* d = data;
    if (d->retryTimer)
        g_source_remove(d->retryTimer);
    if (d->download) {
        g_signal_handlers_disconnect_matched(d->download, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, d);
        g_object_unref(d->download);
    }
    g_free(d->refused);
    ps5wk_download_record_clear(&d->r);
    g_free(d);
}

static gint64 nowSeconds(void)
{
    return g_get_real_time() / G_USEC_PER_SEC;
}

static char* pathOf(const char* file, gboolean part)
{
    return g_strdup_printf("%s/%s%s", PS5WK_DOWNLOADS_DIR, file, part ? ".part" : "");
}

static gint64 fileSize(const char* file, gboolean part)
{
    char* path = pathOf(file, part);
    struct stat st;
    gint64 size = stat(path, &st) ? -1 : (gint64)st.st_size;
    g_free(path);
    return size;
}

static void removeFile(const char* file, gboolean part)
{
    char* path = pathOf(file, part);
    if (unlink(path) && errno != ENOENT)
        spike_log("[dl] could not delete %s (errno %d)", path, errno);
    g_free(path);
}

/* The downloads folder's free space: statfs where the console allows it,
 * else what the NetworkProcess (a payload, which may call it) writes every
 * 5 s to /system_tmp/ps5wk/datafree-<this pid> (ps5wk-child.cpp). -1: not
 * known. Settings' Storage card uses it too (/data is one file system). */
gint64 ps5wk_downloads_free_space(void)
{
    struct statfs fs;
    if (!statfs(PS5WK_DOWNLOADS_DIR, &fs))
        return (gint64)fs.f_bavail * (gint64)fs.f_bsize;
    char path[64];
    snprintf(path, sizeof path, "/system_tmp/ps5wk/datafree-%d", (int)getpid());
    gint64 bytes = -1;
    FILE* file = fopen(path, "r");
    if (file) {
        long long value = 0;
        if (fscanf(file, "free %lld", &value) == 1)
            bytes = value;
        fclose(file);
    }
    static gboolean logged;
    if (!logged && bytes < 0)
        spike_log("[dl] free space of %s not known yet (statfs: errno %d; %s: none)", PS5WK_DOWNLOADS_DIR, errno, path);
    logged = logged || bytes < 0;
    return bytes;
}

static void setError(Download* d, const char* text)
{
    g_free(d->r.error);
    d->r.error = text && *text ? g_strdup(text) : NULL;
}

static void formatBytes(char* out, size_t size, gint64 bytes)
{
    if (bytes < 0)
        snprintf(out, size, "?");
    else if (bytes >= 1024LL * 1024 * 1024)
        snprintf(out, size, "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 100LL * 1024 * 1024)
        snprintf(out, size, "%.0f MB", bytes / (1024.0 * 1024));
    else if (bytes >= 1024LL * 1024)
        snprintf(out, size, "%.1f MB", bytes / (1024.0 * 1024));
    else if (bytes >= 1024)
        snprintf(out, size, "%.0f KB", bytes / 1024.0);
    else
        snprintf(out, size, "%lld B", (long long)bytes);
}

static void formatETA(char* out, size_t size, double seconds)
{
    if (seconds < 60)
        snprintf(out, size, "%.0f s left", seconds < 1 ? 1 : seconds);
    else if (seconds < 3600)
        snprintf(out, size, "%.0f min left", seconds / 60);
    else
        snprintf(out, size, "%d h %d min left", (int)(seconds / 3600), (int)(seconds / 60) % 60);
}

static const char* hostOf(const char* url, char* out, size_t size)
{
    const char* p = strstr(url ? url : "", "://");
    p = p ? p + 3 : (url ? url : "");
    if (!strncmp(p, "www.", 4))
        p += 4;
    size_t n = strcspn(p, "/:?#");
    snprintf(out, size, "%.*s", (int)MIN(n, size - 1), p);
    return out;
}

static gboolean resumable(const Download* d)
{
    return d->r.url && (g_str_has_prefix(d->r.url, "http://") || g_str_has_prefix(d->r.url, "https://"));
}

static Download* byDownload(WebKitDownload* download)
{
    for (unsigned i = 0; list && i < list->len; i++) {
        Download* d = g_ptr_array_index(list, i);
        if (d->download == download)
            return d;
    }
    return NULL;
}

static Download* at(int index)
{
    return list && index >= 0 && index < (int)list->len ? g_ptr_array_index(list, index) : NULL;
}

static void notice(const char* format, ...) G_GNUC_PRINTF(1, 2);
static void notice(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    char* text = g_strdup_vprintf(format, args);
    va_end(args);
    if (display)
        wpe_display_ps5_show_message(display, text, 3);
    g_free(text);
}

/* A name for the folder: no path, no control characters, no leading dots;
 * at most NAME_MAX_BYTES bytes of UTF-8. */
static char* sanitize(const char* suggested, const char* url)
{
    char* name = NULL;
    if (suggested && *suggested)
        name = g_strdup(suggested);
    else if (url) {
        /* the address's last path segment */
        char* path = g_strdup(url);
        path[strcspn(path, "?#")] = 0;
        const char* slash = strrchr(path, '/');
        char* segment = slash && slash[1] ? g_uri_unescape_string(slash + 1, NULL) : NULL;
        name = segment ? segment : g_strdup("");
        g_free(path);
    } else
        name = g_strdup("");
    if (!g_utf8_validate(name, -1, NULL)) {
        char* valid = g_utf8_make_valid(name, -1);
        g_free(name);
        name = valid;
    }
    for (unsigned char* c = (unsigned char*)name; *c; c++) {
        if (*c < 0x20 || *c == 0x7f || strchr("/\\:*?\"<>|", *c))
            *c = '_';
    }
    char* start = name;
    while (*start == '.' || *start == ' ')
        start++;
    char* clean = g_strdup(start);
    g_free(name);
    size_t length = strlen(clean);
    while (length && (clean[length - 1] == ' ' || clean[length - 1] == '.'))
        clean[--length] = 0;
    if (length > NAME_MAX_BYTES) {
        /* keep the extension, cut the stem on a character boundary */
        const char* dot = strrchr(clean, '.');
        size_t extension = dot && strlen(dot) <= 12 ? strlen(dot) : 0;
        const char* cut = g_utf8_find_prev_char(clean, clean + NAME_MAX_BYTES - extension + 1);
        size_t keep = cut ? (size_t)(cut - clean) : 0;
        char* shorter = g_strdup_printf("%.*s%s", (int)keep, clean, extension ? dot : "");
        g_free(clean);
        clean = shorter;
    }
    if (!*clean) {
        g_free(clean);
        clean = g_strdup("download");
    }
    return clean;
}

static gboolean nameTaken(const char* name, const Download* except)
{
    for (unsigned i = 0; i < list->len; i++) {
        const Download* d = g_ptr_array_index(list, i);
        if (d != except && d->r.file && !strcmp(d->r.file, name))
            return TRUE;
    }
    return fileSize(name, FALSE) >= 0 || fileSize(name, TRUE) >= 0;
}

/* "name.zip", then "name (1).zip", "name (2).zip"... (".tar.gz" stays whole). */
static char* uniqueName(const char* name, const Download* except)
{
    if (!nameTaken(name, except))
        return g_strdup(name);
    const char* extension = NULL;
    static const char* const compound[] = { ".tar.gz", ".tar.xz", ".tar.bz2", ".tar.zst" };
    for (unsigned i = 0; i < G_N_ELEMENTS(compound) && !extension; i++) {
        if (g_str_has_suffix(name, compound[i]) && strlen(name) > strlen(compound[i]))
            extension = name + strlen(name) - strlen(compound[i]);
    }
    if (!extension) {
        extension = strrchr(name, '.');
        if (extension == name)
            extension = NULL;
    }
    if (!extension)
        extension = name + strlen(name);
    for (unsigned n = 1; n < 10000; n++) {
        char* candidate = g_strdup_printf("%.*s (%u)%s", (int)(extension - name), name, n, extension);
        if (!nameTaken(candidate, except))
            return candidate;
        g_free(candidate);
    }
    return g_strdup_printf("download-%" G_GINT64_FORMAT, g_get_real_time());
}

static const char* header(WebKitURIResponse* response, const char* name)
{
    SoupMessageHeaders* headers = response ? webkit_uri_response_get_http_headers(response) : NULL;
    return headers ? soup_message_headers_get_one(headers, name) : NULL;
}

/* "bytes S-E/T": S and T (T -1 for "*"). */
static gboolean parseContentRange(const char* value, gint64* start, gint64* total)
{
    if (!value || g_ascii_strncasecmp(value, "bytes ", 6))
        return FALSE;
    char* end = NULL;
    *start = g_ascii_strtoll(value + 6, &end, 10);
    if (end == value + 6 || *end != '-')
        return FALSE;
    const char* slash = strchr(end, '/');
    if (!slash)
        return FALSE;
    *total = slash[1] == '*' ? -1 : g_ascii_strtoll(slash + 1, NULL, 10);
    return TRUE;
}

/* ---------------------------------------------------------------- the list's view for other threads */

static void updateSnapshot(void)
{
    GString* finished = g_string_new("[");
    GPtrArray* names = g_ptr_array_new_with_free_func(g_free);
    unsigned active = 0, done = 0;
    for (unsigned i = 0; i < list->len; i++) {
        Download* d = g_ptr_array_index(list, i);
        if (d->r.state == PS5WK_DL_RUNNING)
            active++;
        if (d->r.state != PS5WK_DL_DONE)
            continue;
        done++;
        g_string_append(finished, names->len ? ",{\"file\":" : "{\"file\":");
        ps5wk_json_string(finished, d->r.file);
        g_string_append_printf(finished, ",\"size\":%" G_GINT64_FORMAT ",\"sha256\":", d->r.size);
        ps5wk_json_string(finished, d->r.sha256 ? d->r.sha256 : "");
        g_string_append_printf(finished, ",\"finished\":%" G_GINT64_FORMAT ",\"url\":", d->r.finished);
        ps5wk_json_string(finished, d->r.url);
        g_string_append_c(finished, '}');
        g_ptr_array_add(names, g_strdup(d->r.file));
    }
    g_string_append_c(finished, ']');
    char* summary = g_strdup_printf("\"finished\":%u,\"active\":%u,\"total\":%u", done, active, list->len);
    g_mutex_lock(&snapLock);
    g_free(snapFinishedJSON);
    snapFinishedJSON = g_string_free(finished, FALSE);
    g_free(snapSummaryJSON);
    snapSummaryJSON = summary;
    if (snapFinishedNames)
        g_ptr_array_unref(snapFinishedNames);
    snapFinishedNames = names;
    g_mutex_unlock(&snapLock);
}

char* ps5wk_downloads_finished_json(void)
{
    g_mutex_lock(&snapLock);
    char* json = g_strdup(snapFinishedJSON ? snapFinishedJSON : "[]");
    g_mutex_unlock(&snapLock);
    return json;
}

char* ps5wk_downloads_summary_json(void)
{
    g_mutex_lock(&snapLock);
    char* counts = g_strdup(snapSummaryJSON ? snapSummaryJSON : "\"finished\":0,\"active\":0,\"total\":0");
    g_mutex_unlock(&snapLock);
    /* the free space now (any thread: a file read) */
    char* json = g_strdup_printf("{\"folder\":\"%s\",\"free\":%" G_GINT64_FORMAT ",%s}", PS5WK_DOWNLOADS_DIR, ps5wk_downloads_free_space(), counts);
    g_free(counts);
    return json;
}

char* ps5wk_downloads_finished_path(const char* name)
{
    if (!name || !*name || name[0] == '.' || strchr(name, '/') || strchr(name, '\\'))
        return NULL;
    char* path = NULL;
    g_mutex_lock(&snapLock);
    for (unsigned i = 0; snapFinishedNames && i < snapFinishedNames->len; i++) {
        if (!strcmp(g_ptr_array_index(snapFinishedNames, i), name))
            path = pathOf(name, FALSE);
    }
    g_mutex_unlock(&snapLock);
    return path;
}

/* ---------------------------------------------------------------- saving */

static void saveNow(void)
{
    if (saveTimer) {
        g_source_remove(saveTimer);
        saveTimer = 0;
    }
    dirty = FALSE;
    GPtrArray* records = g_ptr_array_new(); /* borrowed */
    for (unsigned i = 0; i < list->len; i++)
        g_ptr_array_add(records, &((Download*)g_ptr_array_index(list, i))->r);
    ps5wk_downloads_save(records);
    g_ptr_array_unref(records);
}

static gboolean saveLater(gpointer data)
{
    (void)data;
    saveTimer = 0;
    saveNow();
    return G_SOURCE_REMOVE;
}

/* stateChanged: written at once, the link server's view refreshed;
 * else (progress) within SAVE_S. */
static void changed(gboolean stateChanged)
{
    dirty = TRUE;
    if (stateChanged) {
        saveNow();
        updateSnapshot();
    } else if (!saveTimer)
        saveTimer = g_timeout_add_seconds(SAVE_S, saveLater, NULL);
    ps5wk_browser_downloads_changed();
}

/* ---------------------------------------------------------------- SHA-256 (a worker thread) */

typedef struct {
    unsigned id;
    char* path;
    char* hex;
    char* error;
    double seconds;
    gint64 bytes;
} HashJob;

static gboolean hashRunning;

static gboolean hashDone(gpointer data)
{
    HashJob* job = data;
    hashRunning = FALSE;
    for (unsigned i = 0; list && i < list->len; i++) {
        Download* d = g_ptr_array_index(list, i);
        if (d->r.id != job->id)
            continue;
        d->hashing = FALSE;
        if (job->hex) {
            g_free(d->r.sha256);
            d->r.sha256 = g_strdup(job->hex);
        }
        spike_log("[dl] %u: SHA-256 %s (%.1f MB in %.1f s, %.0f MB/s)%s%s", job->id, job->hex ? job->hex : "not computed", job->bytes / 1048576.0,
            job->seconds, job->seconds > 0 ? job->bytes / 1048576.0 / job->seconds : 0, job->error ? ": " : "", job->error ? job->error : "");
        changed(TRUE);
    }
    g_free(job->path);
    g_free(job->hex);
    g_free(job->error);
    g_free(job);
    hashNext();
    return G_SOURCE_REMOVE;
}

static gpointer hashThread(gpointer data)
{
    HashJob* job = data;
    gint64 start = g_get_monotonic_time();
    int fd = open(job->path, O_RDONLY);
    if (fd < 0)
        job->error = g_strdup_printf("open failed (errno %d)", errno);
    else {
        GChecksum* checksum = g_checksum_new(G_CHECKSUM_SHA256);
        const size_t size = 1024 * 1024;
        guchar* buffer = g_malloc(size);
        for (;;) {
            ssize_t n = read(fd, buffer, size);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0) {
                job->error = g_strdup_printf("read failed (errno %d)", errno);
                break;
            }
            if (!n)
                break;
            g_checksum_update(checksum, buffer, n);
            job->bytes += n;
        }
        close(fd);
        if (!job->error)
            job->hex = g_strdup(g_checksum_get_string(checksum));
        g_checksum_free(checksum);
        g_free(buffer);
    }
    job->seconds = (g_get_monotonic_time() - start) / 1e6;
    g_idle_add(hashDone, job);
    return NULL;
}

/* One file at a time: the oldest finished download without a checksum. */
static void hashNext(void)
{
    if (hashRunning || windingDown)
        return;
    for (unsigned i = list->len; i-- > 0;) {
        Download* d = g_ptr_array_index(list, i);
        if (d->r.state != PS5WK_DL_DONE || d->r.sha256)
            continue;
        HashJob* job = g_new0(HashJob, 1);
        job->id = d->r.id;
        job->path = pathOf(d->r.file, FALSE);
        d->hashing = TRUE;
        hashRunning = TRUE;
        GThread* thread = g_thread_try_new("ps5wk-sha256", hashThread, job, NULL);
        if (thread)
            g_thread_unref(thread);
        else {
            hashRunning = FALSE;
            d->hashing = FALSE;
            g_free(job->path);
            g_free(job);
        }
        return;
    }
}

/* ---------------------------------------------------------------- a transfer's signals */

static void endTransfer(Download* d)
{
    if (!d->download)
        return;
    g_signal_handlers_disconnect_matched(d->download, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, d);
    g_object_unref(d->download);
    d->download = NULL;
}

static gboolean onDecideDestination(WebKitDownload* download, const char* suggested, gpointer data)
{
    Download* d = data;
    WebKitURIResponse* response = webkit_download_get_response(download);
    guint status = response ? webkit_uri_response_get_status_code(response) : 0;
    const char* finalURL = response ? webkit_uri_response_get_uri(response) : NULL;
    const char* lengthText = header(response, "Content-Length");
    gint64 length = lengthText ? g_ascii_strtoll(lengthText, NULL, 10) : -1;
    gint64 rangeStart = -1, rangeTotal = -1;
    gboolean partial = status == 206 && parseContentRange(header(response, "Content-Range"), &rangeStart, &rangeTotal);
    d->responded = TRUE;
    if (finalURL) {
        g_free(d->r.finalURL);
        d->r.finalURL = g_strdup(finalURL);
    }

    if (!d->r.file) {
        /* a new download: its name, and what a resume checks against */
        char* clean = sanitize(suggested, finalURL ? finalURL : d->r.url);
        d->r.file = uniqueName(clean, d);
        g_free(clean);
        g_free(d->r.mime);
        d->r.mime = g_strdup(response ? webkit_uri_response_get_mime_type(response) : NULL);
    }

    if (d->attemptResume) {
        if (partial && rangeStart == d->attemptFrom && (rangeTotal < 0 || d->r.size < 0 || rangeTotal == d->r.size)) {
            spike_log("[dl] %u: resumed at %" G_GINT64_FORMAT " bytes (%s), %s", d->r.id, d->attemptFrom, header(response, "Content-Range"),
                finalURL ? finalURL : "");
            d->r.restarted = FALSE;
            setError(d, NULL); /* a retry's reason, an old note */
        } else if (partial) {
            /* a range of another file, or not where the .part ends: never joined */
            spike_log("[dl] %u: the server sent %s for %" G_GINT64_FORMAT " bytes on disk (total %" G_GINT64_FORMAT " before): starting over",
                d->r.id, header(response, "Content-Range"), d->attemptFrom, d->r.size);
            d->restartFromZero = TRUE;
            webkit_download_cancel(download);
            return TRUE;
        } else {
            /* 200: no ranges here, or If-Range saw another validator */
            gboolean validator = d->r.etag || d->r.lastModified;
            spike_log("[dl] %u: HTTP %u for a resume at %" G_GINT64_FORMAT " bytes: starting over from zero (%s)", d->r.id, status, d->attemptFrom,
                validator ? "no range support, or the file changed" : "no range support");
            d->r.restarted = TRUE;
            d->attemptFrom = 0;
            d->r.received = 0;
            setError(d, validator ? "Started over from zero: the server's file changed or it can't resume"
                                  : "Started over from zero: the server can't resume");
        }
    }
    if (!d->attemptResume || !partial) {
        /* the whole file: its size and validator */
        d->r.size = length;
        g_free(d->r.etag);
        g_free(d->r.lastModified);
        d->r.etag = g_strdup(header(response, "ETag"));
        d->r.lastModified = g_strdup(header(response, "Last-Modified"));
    } else if (rangeTotal >= 0)
        d->r.size = rangeTotal;

    /* room for what is still to come */
    gint64 needed = partial ? (rangeTotal >= 0 ? rangeTotal - rangeStart : length) : length;
    gint64 free = ps5wk_downloads_free_space();
    if (needed > 0 && free >= 0 && needed + FREE_MARGIN > free) {
        char a[32], b[32];
        formatBytes(a, sizeof a, needed);
        formatBytes(b, sizeof b, free);
        g_free(d->refused);
        d->refused = g_strdup_printf("Not enough space: needs %s, %s free", a, b);
        spike_log("[dl] %u: %s: refused (%s)", d->r.id, d->r.file, d->refused);
        webkit_download_cancel(download);
        return TRUE;
    }

    char* path = pathOf(d->r.file, FALSE);
    webkit_download_set_allow_overwrite(download, TRUE);
    webkit_download_set_destination(download, path);
    char size[32];
    formatBytes(size, sizeof size, d->r.size);
    spike_log("[dl] %u: HTTP %u, %s%s, to %s.part (%s; ETag %s, Last-Modified %s)", d->r.id, status, size, partial ? " (a range)" : "",
        path, d->r.mime ? d->r.mime : "?", d->r.etag ? d->r.etag : "-", d->r.lastModified ? d->r.lastModified : "-");
    g_free(path);
    ps5wk_browser_download_started(webkit_download_get_web_view(download));
    if (!d->announced) {
        d->announced = TRUE;
        notice("Download started: %s\n(Options, then R1: Downloads)", d->r.file);
    }
    d->lastData = g_get_monotonic_time();
    changed(TRUE);
    return TRUE;
}

static void onReceivedData(WebKitDownload* download, guint64 length, gpointer data)
{
    (void)download;
    Download* d = data;
    gint64 now = g_get_monotonic_time();
    d->r.received += (gint64)length;
    d->attemptBytes += (gint64)length;
    d->lastData = now;
    if (d->retries && d->attemptBytes >= 1024 * 1024)
        d->retries = 0; /* it works again */
    if (!d->sampleTime) {
        d->sampleTime = now;
        d->sampleBytes = d->r.received;
    } else if (now - d->sampleTime >= 500 * 1000) {
        double rate = (d->r.received - d->sampleBytes) / ((now - d->sampleTime) / 1e6);
        d->speed = d->speed > 0 ? d->speed * 0.6 + rate * 0.4 : rate;
        d->sampleTime = now;
        d->sampleBytes = d->r.received;
        changed(FALSE);
    }
}

static gboolean retryFired(gpointer data)
{
    Download* d = data;
    d->retryTimer = 0;
    d->retryAt = 0;
    spike_log("[dl] %u: retry %u", d->r.id, d->retries);
    startAttempt(d, FALSE);
    return G_SOURCE_REMOVE;
}

static void onFailed(WebKitDownload* download, GError* error, gpointer data)
{
    Download* d = data;
    WebKitURIResponse* response = webkit_download_get_response(download);
    guint status = response ? webkit_uri_response_get_status_code(response) : 0;
    d->failedNow = TRUE;
    gboolean cancelled = g_error_matches(error, WEBKIT_DOWNLOAD_ERROR, WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER);
    spike_log("[dl] %u: transfer ended: %s (domain %s, code %d, HTTP %u; %" G_GINT64_FORMAT " bytes this time, %" G_GINT64_FORMAT " on disk)",
        d->r.id, error ? error->message : "?", error ? g_quark_to_string(error->domain) : "?", error ? error->code : 0, status, d->attemptBytes,
        d->r.file ? fileSize(d->r.file, TRUE) : -1);
    if (windingDown)
        return;
    if (d->removing)
        return;
    if (d->pausing) {
        d->r.state = PS5WK_DL_PAUSED;
        return;
    }
    if (d->cancelling) {
        d->r.state = PS5WK_DL_CANCELLED;
        if (d->r.file)
            removeFile(d->r.file, TRUE);
        d->r.received = 0;
        return;
    }
    if (d->refused) {
        d->r.state = PS5WK_DL_FAILED;
        setError(d, d->refused);
        g_clear_pointer(&d->refused, g_free);
        notice("Download failed: %s\n%s", d->r.file ? d->r.file : "", d->r.error);
        return;
    }
    if (d->restartFromZero || status == 416) {
        /* mismatched range, or the server says the range is past its file */
        if (d->r.file)
            removeFile(d->r.file, TRUE);
        d->r.received = 0;
        d->r.restarted = TRUE;
        setError(d, "Started over from zero: the server's file did not match the partial one");
        d->restartFromZero = FALSE;
        d->r.state = PS5WK_DL_RUNNING;
        d->retryTimer = g_timeout_add(500, retryFired, d); /* after this transfer's "finished" */
        d->retryAt = g_get_monotonic_time() + 500 * 1000;
        return;
    }
    /* HTTP errors that a retry won't fix */
    gboolean httpFinal = status >= 400 && status != 408 && status != 429 && status < 500;
    gboolean destination = g_error_matches(error, WEBKIT_DOWNLOAD_ERROR, WEBKIT_DOWNLOAD_ERROR_DESTINATION);
    if (httpFinal || destination || !resumable(d) || (cancelled && !d->stalled)) {
        d->r.state = PS5WK_DL_FAILED;
        char* why = httpFinal ? g_strdup_printf("HTTP %u %s", status, response && *error->message ? error->message : "")
                              : g_strdup(error && *error->message ? error->message : "Failed");
        g_strstrip(why);
        setError(d, why);
        g_free(why);
        notice("Download failed: %s\n%s", d->r.file ? d->r.file : d->r.url, d->r.error);
        return;
    }
    static const unsigned delays[] = { 2, 5, 15, 30 };
    if (d->retries < G_N_ELEMENTS(delays)) {
        unsigned delay = delays[d->retries++];
        setError(d, d->stalled ? "No data for 45 s" : error && *error->message ? error->message : "Connection lost");
        d->r.state = PS5WK_DL_RUNNING;
        d->retryTimer = g_timeout_add_seconds(delay, retryFired, d);
        d->retryAt = g_get_monotonic_time() + delay * G_USEC_PER_SEC;
        spike_log("[dl] %u: retry %u in %u s", d->r.id, d->retries, delay);
        return;
    }
    d->r.state = PS5WK_DL_FAILED;
    char* why = g_strdup_printf("%s (tried %u times)", d->stalled ? "No data for 45 s" : error && *error->message ? error->message : "Connection lost",
        (unsigned)G_N_ELEMENTS(delays) + 1);
    setError(d, why);
    g_free(why);
    notice("Download failed: %s\n%s. X in Downloads retries.", d->r.file ? d->r.file : d->r.url, d->r.error);
}

static void onFinished(WebKitDownload* download, gpointer data)
{
    Download* d = data;
    gboolean failed = d->failedNow;
    if (!failed && !windingDown && d->r.size > 0 && fileSize(d->r.file, FALSE) >= 0 && fileSize(d->r.file, FALSE) < d->r.size && resumable(d)) {
        /* shorter than the server said (a connection that ended early and
         * looked like the end): back to .part and on from there */
        gint64 onDisk = fileSize(d->r.file, FALSE);
        char* done = pathOf(d->r.file, FALSE);
        char* part = pathOf(d->r.file, TRUE);
        gboolean back = !rename(done, part);
        g_free(done);
        g_free(part);
        spike_log("[dl] %u: finished with %" G_GINT64_FORMAT " of %" G_GINT64_FORMAT " bytes: %s", d->r.id, onDisk, d->r.size,
            back ? "resuming from the .part" : "could not rename it back");
        if (back && ++d->retries > 4) {
            failed = TRUE;
            d->r.received = onDisk;
            d->r.state = PS5WK_DL_FAILED;
            setError(d, "The connection closed early (tried 5 times)");
        } else if (back) {
            d->r.received = onDisk;
            failed = TRUE;
            setError(d, "The connection closed early");
            d->r.state = PS5WK_DL_RUNNING;
            d->retryTimer = g_timeout_add_seconds(2, retryFired, d);
            d->retryAt = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
        }
    }
    if (!failed && !windingDown) {
        gint64 onDisk = fileSize(d->r.file, FALSE);
        d->r.state = PS5WK_DL_DONE;
        d->r.finished = nowSeconds();
        if (onDisk >= 0)
            d->r.received = onDisk;
        if (d->r.size >= 0 && onDisk >= 0 && onDisk != d->r.size)
            spike_log("[dl] %u: finished with %" G_GINT64_FORMAT " bytes, the server said %" G_GINT64_FORMAT, d->r.id, onDisk, d->r.size);
        d->r.size = d->r.received;
        if (d->r.error && strncmp(d->r.error, "Started over", 12))
            setError(d, NULL);
        d->retries = 0;
        spike_log("[dl] %u: done: %s, %" G_GINT64_FORMAT " bytes (%" G_GINT64_FORMAT " this time, from %" G_GINT64_FORMAT ")", d->r.id, d->r.file,
            d->r.received, d->attemptBytes, d->attemptFrom);
        notice("Downloaded %s", d->r.file);
    }
    d->pausing = d->cancelling = d->stalled = FALSE;
    d->speed = 0;
    endTransfer(d);
    (void)download;
    if (d->removing) {
        g_ptr_array_remove(list, d);
        changed(TRUE);
        return;
    }
    changed(TRUE);
    hashNext();
}

static void connectTransfer(Download* d, WebKitDownload* download)
{
    d->download = download;
    d->responded = d->failedNow = FALSE;
    d->attemptBytes = 0;
    d->sampleTime = 0;
    d->speed = 0;
    d->lastData = g_get_monotonic_time();
    g_signal_connect(download, "decide-destination", G_CALLBACK(onDecideDestination), d);
    g_signal_connect(download, "received-data", G_CALLBACK(onReceivedData), d);
    g_signal_connect(download, "failed", G_CALLBACK(onFailed), d);
    g_signal_connect(download, "finished", G_CALLBACK(onFinished), d);
}

/* A transfer for d: from what the .part file holds (a Range request for the
 * original address), or the whole file (fresh, or nothing on disk). */
static void startAttempt(Download* d, gboolean fresh)
{
    WebKitWebView* view = ps5wk_browser_active_view();
    if (!view || d->download)
        return;
    if (d->retryTimer) {
        g_source_remove(d->retryTimer);
        d->retryTimer = 0;
        d->retryAt = 0;
    }
    gint64 onDisk = d->r.file && !fresh ? fileSize(d->r.file, TRUE) : -1;
    if (fresh && d->r.file)
        removeFile(d->r.file, TRUE);
    WebKitURIRequest* request = webkit_uri_request_new(d->r.url);
    SoupMessageHeaders* headers = webkit_uri_request_get_http_headers(request);
    d->attemptResume = onDisk > 0 && resumable(d) && headers;
    d->attemptFrom = d->attemptResume ? onDisk : 0;
    d->r.received = d->attemptFrom;
    if (d->attemptResume) {
        char range[48];
        snprintf(range, sizeof range, "bytes=%" G_GINT64_FORMAT "-", onDisk);
        soup_message_headers_replace(headers, "Range", range);
        /* If-Range wants a strong validator: an ETag that isn't W/, else the date */
        const char* validator = d->r.etag && strncmp(d->r.etag, "W/", 2) ? d->r.etag : d->r.lastModified;
        if (validator)
            soup_message_headers_replace(headers, "If-Range", validator);
        spike_log("[dl] %u: asking %s again from byte %" G_GINT64_FORMAT " (If-Range %s)", d->r.id, d->r.url, onDisk, validator ? validator : "none");
    } else
        spike_log("[dl] %u: asking %s for the whole file", d->r.id, d->r.url);
    if (headers && *userAgent && !soup_message_headers_get_one(headers, "User-Agent"))
        soup_message_headers_replace(headers, "User-Agent", userAgent);
    d->r.state = PS5WK_DL_RUNNING;
    d->pausing = d->cancelling = d->stalled = d->restartFromZero = FALSE;
    d->announced = TRUE; /* the notice is for new downloads */
    WebKitDownload* download = webkit_web_view_download_request(view, request);
    g_object_unref(request);
    if (!download) {
        d->r.state = PS5WK_DL_FAILED;
        setError(d, "WebKit did not start the download");
        changed(TRUE);
        return;
    }
    connectTransfer(d, download); /* takes the reference */
    changed(TRUE);
}

/* Every download in the session: one the title asked for (a resume) is
 * already known; any other is new (a page's link). */
static void onDownloadStarted(WebKitNetworkSession* session, WebKitDownload* download, gpointer data)
{
    (void)session;
    (void)data;
    if (byDownload(download))
        return;
    Download* d = g_new0(Download, 1);
    d->r.id = nextID++;
    d->r.url = webkit_download_dup_original_uri(download);
    if (!d->r.url || !*d->r.url) {
        g_free(d->r.url);
        WebKitURIRequest* request = webkit_download_get_request(download);
        d->r.url = g_strdup(request ? webkit_uri_request_get_uri(request) : "");
    }
    if (strlen(d->r.url) > 4096) {
        /* a data: address made by a page: not kept whole (and never asked again) */
        char* cut = g_strdup_printf("%.64s…", d->r.url);
        g_free(d->r.url);
        d->r.url = cut;
    }
    d->r.size = -1;
    d->r.state = PS5WK_DL_RUNNING;
    d->r.started = nowSeconds();
    WebKitURIRequest* request = webkit_download_get_request(download);
    spike_log("[dl] %u: download started: %s (request %s)", d->r.id, d->r.url, request ? webkit_uri_request_get_uri(request) : "?");
    g_ptr_array_insert(list, 0, d);
    while (list->len > LIST_MAX) {
        /* the oldest finished entry (its file stays) */
        gboolean dropped = FALSE;
        for (unsigned i = list->len; i-- > 0 && !dropped;) {
            Download* old = g_ptr_array_index(list, i);
            if (!old->download && old->r.state != PS5WK_DL_RUNNING) {
                g_ptr_array_remove_index(list, i);
                dropped = TRUE;
            }
        }
        if (!dropped)
            break;
    }
    connectTransfer(d, g_object_ref(download));
    changed(TRUE);
}

/* Stalls: no data for STALL_S on a transfer that had its answer. */
static gboolean tick(gpointer data)
{
    (void)data;
    gint64 now = g_get_monotonic_time();
    gboolean any = FALSE;
    for (unsigned i = 0; i < list->len; i++) {
        Download* d = g_ptr_array_index(list, i);
        if (d->download && !d->pausing && !d->cancelling && !d->stalled && now - d->lastData > STALL_S * G_USEC_PER_SEC) {
            spike_log("[dl] %u: no data for %d s: retrying", d->r.id, STALL_S);
            d->stalled = TRUE;
            webkit_download_cancel(d->download);
        }
        if (d->download && d->speed > 0 && now - d->lastData > 2 * G_USEC_PER_SEC)
            d->speed = 0; /* nothing arriving: no speed, no ETA */
        any = any || d->r.state == PS5WK_DL_RUNNING;
    }
    if (any)
        ps5wk_browser_downloads_changed(); /* retry countdowns */
    return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------- policy */

gboolean ps5wk_downloads_response_policy(WebKitWebView* view, WebKitResponsePolicyDecision* decision)
{
    (void)view;
    WebKitURIResponse* response = webkit_response_policy_decision_get_response(decision);
    guint status = webkit_uri_response_get_status_code(response);
    const char* disposition = header(response, "Content-Disposition");
    while (disposition && (*disposition == ' ' || *disposition == '\t'))
        disposition++;
    gboolean attachment = disposition && !g_ascii_strncasecmp(disposition, "attachment", 10);
    gboolean attribute = webkit_response_policy_decision_has_download_attribute(decision);
    gboolean mainResource = webkit_response_policy_decision_is_main_frame_main_resource(decision);
    gboolean showable = webkit_response_policy_decision_is_mime_type_supported(decision);
    const char* reason = attachment ? "attachment" : attribute ? "download attribute" : NULL;
    if (!reason && mainResource && !showable && status != 204 && status != 205 && status < 300)
        reason = "a type the browser can't show";
    if (!reason)
        return FALSE;
    spike_log("[dl] response for %s (%s, HTTP %u): a download (%s)", webkit_uri_response_get_uri(response),
        webkit_uri_response_get_mime_type(response) ? webkit_uri_response_get_mime_type(response) : "?", status, reason);
    webkit_policy_decision_download(WEBKIT_POLICY_DECISION(decision));
    return TRUE;
}

/* ---------------------------------------------------------------- the menu's list */

unsigned ps5wk_downloads_count(void)
{
    return list ? list->len : 0;
}

static const char* kindOf(const Download* d, char* out, size_t size)
{
    const char* file = d->r.file ? d->r.file : "";
    const char* dot = strrchr(file, '.');
    if (dot && dot != file && strlen(dot + 1) >= 1 && strlen(dot + 1) <= 4) {
        size_t n = 0;
        for (const char* c = dot + 1; *c && n + 1 < size; c++) {
            if (!g_ascii_isalnum(*c))
                break;
            out[n++] = g_ascii_toupper(*c);
        }
        out[n] = 0;
        if (n)
            return out;
    }
    snprintf(out, size, "FILE");
    return out;
}

void ps5wk_downloads_row(unsigned index, BUIRow* r)
{
    Download* d = at((int)index);
    if (!d)
        return;
    static char host[128];
    r->title = d->r.file ? d->r.file : d->r.url;
    r->url = hostOf(d->r.url, host, sizeof host);
    kindOf(d, r->kind, sizeof r->kind);
    char got[32], total[32];
    formatBytes(got, sizeof got, d->r.received);
    formatBytes(total, sizeof total, d->r.size);
    r->progress = d->r.size > 0 ? CLAMP((double)d->r.received / d->r.size, 0, 1) : -1;
    gint64 now = g_get_monotonic_time();
    switch (d->r.state) {
    case PS5WK_DL_RUNNING:
        r->tone = BUI_TONE_ACTIVE;
        if (d->retryTimer) {
            snprintf(r->state, sizeof r->state, "Retrying in %.0f s", MAX(0, (d->retryAt - now) / 1e6));
            snprintf(r->detail, sizeof r->detail, "%s%s%s · %s", got, d->r.size >= 0 ? " of " : "", d->r.size >= 0 ? total : "",
                d->r.error ? d->r.error : "");
            r->tone = BUI_TONE_WARN;
        } else if (!d->responded) {
            snprintf(r->state, sizeof r->state, "Connecting…");
            snprintf(r->detail, sizeof r->detail, "%s%s%s", got, d->r.size >= 0 ? " of " : "", d->r.size >= 0 ? total : "");
        } else {
            r->running = TRUE;
            char speed[32] = "", eta[32] = "";
            if (d->speed > 1) {
                formatBytes(speed, sizeof speed, (gint64)d->speed);
                strcat(speed, "/s");
                if (d->r.size > 0 && d->r.size > d->r.received)
                    formatETA(eta, sizeof eta, (d->r.size - d->r.received) / d->speed);
            }
            snprintf(r->detail, sizeof r->detail, "%s%s%s%s%s%s%s", got, d->r.size >= 0 ? " of " : "", d->r.size >= 0 ? total : "", *speed ? " · " : "", speed,
                *eta ? " · " : "", eta);
            if (d->r.restarted) {
                snprintf(r->state, sizeof r->state, "Restarted from zero");
                r->tone = BUI_TONE_WARN;
            } else if (r->progress >= 0)
                snprintf(r->state, sizeof r->state, "%.0f%%", r->progress * 100);
        }
        break;
    case PS5WK_DL_PAUSED:
        r->tone = BUI_TONE_PLAIN;
        snprintf(r->state, sizeof r->state, "Paused");
        snprintf(r->detail, sizeof r->detail, "%s%s%s%s%s", got, d->r.size >= 0 ? " of " : "", d->r.size >= 0 ? total : "", d->r.error ? " · " : "",
            d->r.error ? d->r.error : "");
        break;
    case PS5WK_DL_FAILED:
        r->tone = BUI_TONE_WARN;
        snprintf(r->state, sizeof r->state, "Failed");
        snprintf(r->detail, sizeof r->detail, "%s%s%s", d->r.error ? d->r.error : "Failed", d->r.received > 0 ? " · " : "", d->r.received > 0 ? got : "");
        break;
    case PS5WK_DL_DONE: {
        r->tone = BUI_TONE_DONE;
        r->progress = -1;
        snprintf(r->state, sizeof r->state, "Done");
        GDateTime* time = g_date_time_new_from_unix_local(d->r.finished);
        char* when = time ? g_date_time_format(time, "%d %b %H:%M") : NULL;
        snprintf(r->detail, sizeof r->detail, "%s · %s%s%s", total, when ? when : "", d->r.restarted ? " · restarted from zero once" : "",
            d->hashing ? " · SHA-256…" : "");
        g_free(when);
        if (time)
            g_date_time_unref(time);
        break;
    }
    case PS5WK_DL_CANCELLED:
        r->tone = BUI_TONE_PLAIN;
        r->progress = -1;
        snprintf(r->state, sizeof r->state, "Cancelled");
        snprintf(r->detail, sizeof r->detail, "The partial file was deleted");
        break;
    }
}

void ps5wk_downloads_hints(unsigned index, const char** cross, const char** square)
{
    Download* d = at((int)index);
    *cross = *square = NULL;
    if (!d)
        return;
    switch (d->r.state) {
    case PS5WK_DL_RUNNING:
        *cross = "Pause";
        *square = "Cancel";
        break;
    case PS5WK_DL_PAUSED:
        *cross = "Resume";
        *square = "Cancel";
        break;
    case PS5WK_DL_FAILED:
        *cross = "Retry";
        *square = d->r.received > 0 ? "Cancel" : "Remove";
        break;
    case PS5WK_DL_DONE:
        *cross = "Details";
        *square = "Delete file";
        break;
    case PS5WK_DL_CANCELLED:
        *cross = "Download again";
        *square = "Remove";
        break;
    }
}

static void pauseDownload(Download* d)
{
    if (d->retryTimer) {
        g_source_remove(d->retryTimer);
        d->retryTimer = 0;
        d->retryAt = 0;
    }
    setError(d, NULL);
    if (d->download) {
        d->pausing = TRUE;
        webkit_download_cancel(d->download);
    }
    d->r.state = PS5WK_DL_PAUSED;
    d->speed = 0;
    spike_log("[dl] %u: paused at %" G_GINT64_FORMAT " bytes", d->r.id, d->r.received);
    changed(TRUE);
}

static void resumeDownload(Download* d)
{
    d->retries = 0;
    setError(d, NULL);
    if (!resumable(d)) {
        setError(d, "This kind of address can't be asked again");
        d->r.state = PS5WK_DL_FAILED;
        changed(TRUE);
        return;
    }
    startAttempt(d, FALSE);
}

void ps5wk_downloads_primary(unsigned index)
{
    Download* d = at((int)index);
    if (!d)
        return;
    switch (d->r.state) {
    case PS5WK_DL_RUNNING:
        pauseDownload(d);
        break;
    case PS5WK_DL_PAUSED:
    case PS5WK_DL_FAILED:
        resumeDownload(d);
        break;
    case PS5WK_DL_CANCELLED:
        d->r.restarted = FALSE;
        d->r.received = 0;
        d->retries = 0;
        setError(d, NULL);
        startAttempt(d, TRUE);
        break;
    default:
        break;
    }
}

gboolean ps5wk_downloads_square_needs_confirm(unsigned index)
{
    Download* d = at((int)index);
    return d && d->r.state == PS5WK_DL_DONE;
}

static void removeEntry(Download* d)
{
    if (d->retryTimer) {
        g_source_remove(d->retryTimer);
        d->retryTimer = 0;
    }
    if (d->r.file) {
        removeFile(d->r.file, TRUE);
        if (d->r.state == PS5WK_DL_DONE)
            removeFile(d->r.file, FALSE);
    }
    spike_log("[dl] %u: removed (%s%s)", d->r.id, d->r.file ? d->r.file : d->r.url, d->r.state == PS5WK_DL_DONE ? ", file deleted" : "");
    if (d->download) {
        /* gone when its transfer ends ("finished") */
        d->removing = TRUE;
        webkit_download_cancel(d->download);
        return;
    }
    g_ptr_array_remove(list, d);
}

void ps5wk_downloads_square(unsigned index)
{
    Download* d = at((int)index);
    if (!d)
        return;
    if (d->r.state == PS5WK_DL_RUNNING || d->r.state == PS5WK_DL_PAUSED || (d->r.state == PS5WK_DL_FAILED && d->r.received > 0)) {
        if (d->retryTimer) {
            g_source_remove(d->retryTimer);
            d->retryTimer = 0;
        }
        spike_log("[dl] %u: cancelled at %" G_GINT64_FORMAT " bytes", d->r.id, d->r.received);
        setError(d, NULL);
        if (d->download) {
            d->cancelling = TRUE;
            webkit_download_cancel(d->download);
        } else if (d->r.file)
            removeFile(d->r.file, TRUE);
        d->r.state = PS5WK_DL_CANCELLED;
        d->r.received = 0;
    } else
        removeEntry(d);
    changed(TRUE);
}

void ps5wk_downloads_details(unsigned index, BUIDetails* out)
{
    static char path[512], size[96], sha[80], validator[160], times[128], state[200];
    Download* d = at((int)index);
    memset(out, 0, sizeof *out);
    if (!d)
        return;
    out->title = d->r.file ? d->r.file : "Download";
    int n = 0;
    out->labels[n] = "Address";
    out->values[n++] = d->r.url;
    if (d->r.finalURL && strcmp(d->r.finalURL, d->r.url)) {
        out->labels[n] = "Came from";
        out->values[n++] = d->r.finalURL;
    }
    snprintf(path, sizeof path, "%s/%s%s", PS5WK_DOWNLOADS_DIR, d->r.file ? d->r.file : "?",
        d->r.state == PS5WK_DL_DONE || d->r.state == PS5WK_DL_CANCELLED ? "" : ".part");
    out->labels[n] = "File";
    out->values[n++] = path;
    char got[32], total[32];
    formatBytes(got, sizeof got, d->r.received);
    formatBytes(total, sizeof total, d->r.size);
    if (d->r.state == PS5WK_DL_DONE)
        snprintf(size, sizeof size, "%s (%" G_GINT64_FORMAT " bytes)", total, d->r.size);
    else
        snprintf(size, sizeof size, "%s of %s (%" G_GINT64_FORMAT " bytes on disk)", got, total, d->r.received);
    out->labels[n] = "Size";
    out->values[n++] = size;
    snprintf(sha, sizeof sha, "%s", d->r.sha256 ? d->r.sha256 : d->hashing ? "computing…" : d->r.state == PS5WK_DL_DONE ? "not computed" : "when it is done");
    out->labels[n] = "SHA-256";
    out->values[n++] = sha;
    if (d->r.etag || d->r.lastModified) {
        snprintf(validator, sizeof validator, "%s%s%s", d->r.etag ? d->r.etag : "", d->r.etag && d->r.lastModified ? "  ·  " : "",
            d->r.lastModified ? d->r.lastModified : "");
        out->labels[n] = "Validator";
        out->values[n++] = validator;
    }
    GDateTime* a = g_date_time_new_from_unix_local(d->r.started);
    GDateTime* b = d->r.finished ? g_date_time_new_from_unix_local(d->r.finished) : NULL;
    char* as = a ? g_date_time_format(a, "%d %b %Y %H:%M:%S") : g_strdup("?");
    char* bs = b ? g_date_time_format(b, "%H:%M:%S") : NULL;
    snprintf(times, sizeof times, "%s%s%s", as, bs ? ", done " : "", bs ? bs : "");
    g_free(as);
    g_free(bs);
    if (a)
        g_date_time_unref(a);
    if (b)
        g_date_time_unref(b);
    out->labels[n] = "Started";
    out->values[n++] = times;
    snprintf(state, sizeof state, "%s%s%s", ps5wk_download_state_name(d->r.state), d->r.error ? ": " : "", d->r.error ? d->r.error : "");
    state[0] = g_ascii_toupper(state[0]);
    out->labels[n] = "State";
    out->values[n++] = state;
    out->count = n;
}

unsigned ps5wk_downloads_active(double* progress)
{
    unsigned active = 0;
    gint64 got = 0, total = 0;
    gboolean unknown = FALSE;
    for (unsigned i = 0; list && i < list->len; i++) {
        Download* d = g_ptr_array_index(list, i);
        if (d->r.state != PS5WK_DL_RUNNING)
            continue;
        active++;
        if (d->r.size > 0) {
            got += d->r.received;
            total += d->r.size;
        } else
            unknown = TRUE;
    }
    if (progress)
        *progress = active && !unknown && total > 0 ? CLAMP((double)got / total, 0, 1) : -1;
    return active;
}

/* ---------------------------------------------------------------- API, settings, wind-down */

void ps5wk_downloads_json(GString* out)
{
    g_string_append_c(out, '[');
    gint64 now = g_get_monotonic_time();
    for (unsigned i = 0; list && i < list->len; i++) {
        Download* d = g_ptr_array_index(list, i);
        g_string_append_printf(out, "%s{\"id\":%u,\"state\":\"%s\",\"file\":", i ? "," : "", d->r.id, ps5wk_download_state_name(d->r.state));
        ps5wk_json_string(out, d->r.file);
        g_string_append(out, ",\"url\":");
        ps5wk_json_string(out, d->r.url);
        g_string_append(out, ",\"finalUrl\":");
        ps5wk_json_string(out, d->r.finalURL);
        g_string_append_printf(out, ",\"size\":%" G_GINT64_FORMAT ",\"received\":%" G_GINT64_FORMAT ",\"onDisk\":%" G_GINT64_FORMAT
            ",\"speed\":%.0f,\"transfer\":%s,\"retries\":%u,\"retryIn\":%.1f,\"restarted\":%s,\"error\":",
            d->r.size, d->r.received, d->r.file ? fileSize(d->r.file, d->r.state != PS5WK_DL_DONE) : -1, d->speed, d->download ? "true" : "false",
            d->retries, d->retryTimer ? (d->retryAt - now) / 1e6 : 0.0, d->r.restarted ? "true" : "false");
        ps5wk_json_string(out, d->r.error);
        g_string_append(out, ",\"etag\":");
        ps5wk_json_string(out, d->r.etag);
        g_string_append(out, ",\"sha256\":");
        ps5wk_json_string(out, d->r.sha256);
        g_string_append_printf(out, ",\"hashing\":%s}", d->hashing ? "true" : "false");
    }
    g_string_append_c(out, ']');
}

const char* ps5wk_downloads_action(const char* action, int index, const char* url)
{
    if (!action)
        return "action=pause|resume|cancel|remove|x|square|start&url=...";
    if (!strcmp(action, "start")) {
        WebKitWebView* view = ps5wk_browser_active_view();
        if (!url || !*url || !view)
            return "url=<address>";
        WebKitDownload* download = webkit_web_view_download_uri(view, url);
        if (download)
            g_object_unref(download); /* download-started takes it */
        return NULL;
    }
    Download* d = at(index);
    if (!d)
        return "index: 1..downloads";
    if (!strcmp(action, "pause")) {
        if (d->r.state != PS5WK_DL_RUNNING)
            return "not running";
        pauseDownload(d);
    } else if (!strcmp(action, "resume") || !strcmp(action, "retry")) {
        if (d->r.state != PS5WK_DL_PAUSED && d->r.state != PS5WK_DL_FAILED)
            return "not paused or failed";
        resumeDownload(d);
    } else if (!strcmp(action, "x"))
        ps5wk_downloads_primary((unsigned)index);
    else if (!strcmp(action, "cancel") || !strcmp(action, "square") || !strcmp(action, "remove"))
        ps5wk_downloads_square((unsigned)index);
    else
        return "action=pause|resume|cancel|remove|x|square|start&url=...";
    return NULL;
}

void ps5wk_downloads_clear_finished(void)
{
    unsigned n = 0;
    for (unsigned i = list->len; i-- > 0;) {
        Download* d = g_ptr_array_index(list, i);
        if ((d->r.state == PS5WK_DL_DONE || d->r.state == PS5WK_DL_CANCELLED) && !d->download) {
            removeEntry(d);
            n++;
        }
    }
    spike_log("[dl] cleared %u finished downloads", n);
    changed(TRUE);
}

void ps5wk_downloads_wind_down(void)
{
    if (!list)
        return;
    windingDown = TRUE;
    unsigned paused = 0;
    for (unsigned i = 0; i < list->len; i++) {
        Download* d = g_ptr_array_index(list, i);
        if (d->r.state != PS5WK_DL_RUNNING)
            continue;
        if (d->r.file) {
            gint64 onDisk = fileSize(d->r.file, TRUE);
            if (onDisk >= 0)
                d->r.received = onDisk;
        }
        d->r.state = PS5WK_DL_PAUSED;
        setError(d, "Paused when the browser closed");
        paused++;
    }
    saveNow();
    spike_log("[dl] wind-down: %u download%s saved as paused", paused, paused == 1 ? "" : "s");
}

/* ---------------------------------------------------------------- start */

void ps5wk_downloads_watch_session(WebKitNetworkSession* session)
{
    g_signal_connect(session, "download-started", G_CALLBACK(onDownloadStarted), NULL);
}

void ps5wk_downloads_init(WPEDisplayPS5* d, WebKitNetworkSession* session)
{
    display = d;
    networkSession = session;
    if (mkdir(PS5WK_DOWNLOADS_DIR, 0777) && errno != EEXIST)
        spike_log("[dl] mkdir %s failed (errno %d)", PS5WK_DOWNLOADS_DIR, errno);
    chmod(PS5WK_DOWNLOADS_DIR, 0777);
    WebKitWebView* view = ps5wk_browser_active_view();
    if (view)
        g_snprintf(userAgent, sizeof userAgent, "%s", webkit_settings_get_user_agent(webkit_web_view_get_settings(view)));
    list = g_ptr_array_new_with_free_func(downloadFree);
    GPtrArray* records = ps5wk_downloads_load();
    unsigned paused = 0, dropped = 0;
    for (unsigned i = 0; i < records->len; i++) {
        PS5WKDownloadRecord* r = g_ptr_array_index(records, i);
        if (r->state == PS5WK_DL_DONE && fileSize(r->file, FALSE) < 0) {
            /* deleted from the folder (FTP): not offered any more */
            dropped++;
            continue;
        }
        Download* dl = g_new0(Download, 1);
        dl->r = *r;
        memset(r, 0, sizeof *r); /* moved */
        if (dl->r.state == PS5WK_DL_RUNNING) {
            /* the browser was closed, or ended, while it ran */
            dl->r.state = PS5WK_DL_PAUSED;
            if (!dl->r.error)
                dl->r.error = g_strdup("Paused when the browser closed");
            paused++;
        }
        if (dl->r.state == PS5WK_DL_PAUSED || dl->r.state == PS5WK_DL_FAILED) {
            gint64 onDisk = fileSize(dl->r.file, TRUE);
            dl->r.received = onDisk > 0 ? onDisk : 0;
        }
        if (dl->r.id >= nextID)
            nextID = dl->r.id + 1;
        g_ptr_array_add(list, dl);
    }
    g_ptr_array_unref(records);
    spike_log("[dl] %u downloads in the list (%u paused at start, %u gone from the folder) in %s", list->len, paused, dropped, PS5WK_DOWNLOADS_DIR);
    g_signal_connect(session, "download-started", G_CALLBACK(onDownloadStarted), NULL);
    tickTimer = g_timeout_add_seconds(1, tick, NULL);
    if (paused || dropped)
        saveNow();
    updateSnapshot();
    hashNext();
}
