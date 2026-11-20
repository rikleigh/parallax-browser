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
#include "dlservice.h"
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
    /* the download service (engine PS5WK_DL_SERVICE) */
    gboolean fromPage;    /* a page's download (not a transfer the title asked for) */
    gboolean handingOff;  /* WebKit's transfer is being cancelled: the service takes over */
    WebKitNetworkSession* session; /* its cookies */
    unsigned connections; /* receiving now */
    gint64 serviceFrom;   /* bytes on disk when this transfer started */
    /* a job on the finished file, while it runs */
    char jobPhase[16];
    gint64 jobDone, jobTotal;
    /* what the archive holds (the service's "peek": JSON, see ps5peek.h),
     * for the manager's suggestion; not kept in downloads.json (the page
     * asks again for a finished one) */
    char* peek;
    char* peekError;
    gboolean peeking;
    gboolean peekedLocal; /* the finished file was looked at (not again) */
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
static char* snapManagerJSON;

/* Jobs on files that aren't downloads (the manager's file browser): an
 * archive or app folder already on a drive. Ids "t<n>" on the service. */
typedef struct {
    unsigned id;
    char* kind;  /* "extract", "install", "image" */
    char* then;  /* after "extract": "install", "image", NULL */
    char* src;
    int state;   /* PS5WK_JOB_* */
    char phase[16];
    gint64 done, total, started;
    char* error;
    char* out;
    char* titleName;
} Task;
static GPtrArray* tasks; /* Task*, newest first */
static unsigned nextTask = 1;

static void startAttempt(Download* d, gboolean fresh);
static void downloadFree(gpointer data);
static void changed(gboolean stateChanged);
static void hashNext(void);
static void serviceStart(Download* d, gboolean fresh);
static void startAfter(Download* d);
static void onServiceEvent(const char* verb, GHashTable* params, gpointer user);
static void withCookies(Download* d, gboolean peek, gboolean fresh);

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
    g_free(d->peek);
    g_free(d->peekError);
    if (d->session)
        g_object_unref(d->session);
    ps5wk_download_record_clear(&d->r);
    g_free(d);
}

static gint64 nowSeconds(void)
{
    return g_get_real_time() / G_USEC_PER_SEC;
}

/* In the user's downloads folder (where WebKit's transfers write). */
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

/* d's folder: the one chosen, else the user's downloads folder. */
static const char* folderOf(const Download* d)
{
    return d->r.dir ? d->r.dir : PS5WK_DOWNLOADS_DIR;
}

/* d's file (or its .part) wherever it is. */
static char* pathIn(const Download* d, gboolean part)
{
    return g_strdup_printf("%s/%s%s", folderOf(d), d->r.file ? d->r.file : "", part ? ".part" : "");
}

/* Whether the title itself can see d's folder (its own data on /data);
 * other drives go through the download service. */
static gboolean titleSees(const Download* d)
{
    return !d->r.dir || !strcmp(d->r.dir, PS5WK_DOWNLOADS_DIR);
}

static gint64 sizeIn(const Download* d, gboolean part)
{
    if (!d->r.file)
        return -1;
    if (titleSees(d))
        return fileSize(d->r.file, part);
    return part ? -1 : (d->r.state == PS5WK_DL_DONE ? d->r.size : -1);
}

/* Deletes d's file or .part (and the service's .ps5dl beside it). */
static void removeIn(const Download* d, gboolean part)
{
    if (!d->r.file)
        return;
    if (titleSees(d) && d->r.engine == PS5WK_DL_WEBKIT) {
        removeFile(d->r.file, part);
        return;
    }
    char* path = pathIn(d, part);
    ps5wk_dlservice_send("rm", "path", path, NULL);
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
        if (!titleSees(d))
            continue; /* the phone page serves the user's downloads folder only */
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
static void updateManagerSnapshot(void);

static void changed(gboolean stateChanged)
{
    dirty = TRUE;
    updateManagerSnapshot();
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
        /* only files the title can read (other drives: the service's) */
        if (d->r.state != PS5WK_DL_DONE || d->r.sha256 || !titleSees(d) || d->r.jobState == PS5WK_JOB_RUNNING)
            continue;
        HashJob* job = g_new0(HashJob, 1);
        job->id = d->r.id;
        job->path = pathIn(d, FALSE);
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

/* ---------------------------------------------------------------- the download manager's settings */

static struct {
    unsigned connections;   /* per download, 1..16 */
    gboolean ask;           /* where to save each download: the manager's page asks */
    char* folder;           /* the default folder; NULL: the user's downloads folder */
    gboolean deleteArchives; /* after they were unpacked */
    char** favorites;       /* folders, NULL-terminated */
    /* what was last chosen for an archive holding one app ("none",
     * "extract", "extract-install", "extract-image"); NULL: never */
    char* appAfter;
} settings = { 8, TRUE, NULL, FALSE, NULL, NULL };

static char* settingsPath(void)
{
    return g_strdup_printf("%s/downloads.ini", ps5wk_profile_dir());
}

static void loadSettings(void)
{
    GKeyFile* file = g_key_file_new();
    char* path = settingsPath();
    if (g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL)) {
        GError* error = NULL;
        int connections = g_key_file_get_integer(file, "downloads", "connections", &error);
        if (!error)
            settings.connections = (unsigned)CLAMP(connections, 1, 16);
        g_clear_error(&error);
        gboolean ask = g_key_file_get_boolean(file, "downloads", "ask", &error);
        if (!error)
            settings.ask = ask;
        g_clear_error(&error);
        settings.deleteArchives = g_key_file_get_boolean(file, "downloads", "delete-archives", NULL);
        char* folder = g_key_file_get_string(file, "downloads", "folder", NULL);
        g_free(settings.folder);
        settings.folder = folder && *folder ? folder : NULL;
        if (!settings.folder)
            g_free(folder);
        g_strfreev(settings.favorites);
        settings.favorites = g_key_file_get_string_list(file, "downloads", "favorites", NULL, NULL);
        char* appAfter = g_key_file_get_string(file, "downloads", "app-after", NULL);
        g_free(settings.appAfter);
        settings.appAfter = appAfter && *appAfter ? appAfter : NULL;
        if (!settings.appAfter)
            g_free(appAfter);
    }
    g_free(path);
    g_key_file_unref(file);
}

static void saveSettings(void)
{
    GKeyFile* file = g_key_file_new();
    g_key_file_set_integer(file, "downloads", "connections", (int)settings.connections);
    g_key_file_set_boolean(file, "downloads", "ask", settings.ask);
    g_key_file_set_boolean(file, "downloads", "delete-archives", settings.deleteArchives);
    g_key_file_set_string(file, "downloads", "folder", settings.folder ? settings.folder : "");
    if (settings.favorites)
        g_key_file_set_string_list(file, "downloads", "favorites", (const char* const*)settings.favorites, g_strv_length(settings.favorites));
    if (settings.appAfter)
        g_key_file_set_string(file, "downloads", "app-after", settings.appAfter);
    char* path = settingsPath();
    gsize length = 0;
    char* text = g_key_file_to_data(file, &length, NULL);
    if (!g_file_set_contents(path, text, (gssize)length, NULL))
        spike_log("[dl] %s couldn't be written", path);
    g_free(text);
    g_free(path);
    g_key_file_unref(file);
}

/* ---------------------------------------------------------------- the download service */

static Download* byID(unsigned id)
{
    for (unsigned i = 0; list && i < list->len; i++) {
        Download* d = g_ptr_array_index(list, i);
        if (d->r.id == id)
            return d;
    }
    return NULL;
}

static Download* byParam(GHashTable* params)
{
    const char* id = g_hash_table_lookup(params, "id");
    return id && *id ? byID((unsigned)atoi(id)) : NULL;
}

static gint64 numberParam(GHashTable* params, const char* key, gint64 fallback)
{
    const char* value = g_hash_table_lookup(params, key);
    return value && *value ? g_ascii_strtoll(value, NULL, 10) : fallback;
}

static void setString(char** field, const char* value)
{
    if (*field && value && !strcmp(*field, value))
        return;
    g_free(*field);
    *field = value && *value ? g_strdup(value) : NULL;
}

/* A page's GET download whose server takes ranges goes to the download
 * service (several connections, any drive); anything else (a form's POST,
 * a blob: or data: file, a server without ranges, where asking again could
 * fail) stays WebKit's single stream. */
static gboolean serviceWanted(Download* d, WebKitDownload* download, WebKitURIResponse* response, guint status, gint64 length)
{
    if (!d->fromPage || d->attemptResume || windingDown || status != 200 || length <= 0 || !resumable(d))
        return FALSE;
    WebKitURIRequest* request = webkit_download_get_request(download);
    const char* method = request ? webkit_uri_request_get_http_method(request) : NULL;
    if (method && strcmp(method, "GET"))
        return FALSE;
    const char* ranges = header(response, "Accept-Ranges");
    while (ranges && (*ranges == ' ' || *ranges == '\t'))
        ranges++;
    return ranges && !g_ascii_strncasecmp(ranges, "bytes", 5) && (!ranges[5] || ranges[5] == ' ' || ranges[5] == ',' || ranges[5] == '\t');
}

/* The manager's page over the tab shown (a download waits for a folder). */
static void openManager(void)
{
    /* already shown (it reloads its state every second) */
    WebKitWebView* view = ps5wk_browser_active_view();
    const char* shown = view ? webkit_web_view_get_uri(view) : NULL;
    if (shown && g_str_has_prefix(shown, "parallax://downloads"))
        return;
    if (!ps5wk_browser_open_page("parallax://downloads/"))
        notice("Choose where to save it in Downloads\n(Options, then R1: Downloads)");
}

/* ---------------------------------------------------------------- looking inside archives (the manager's suggestion) */

static gboolean isArchiveName(const char* name);

/* A later part of a set, or one that needs the others to be read: only
 * looked at once the whole set is on the drive. */
static gboolean isSetPartName(const char* name)
{
    if (!name)
        return FALSE;
    const char* dot = strrchr(name, '.');
    if (!dot)
        return FALSE;
    const char* ext = dot + 1;
    size_t length = strlen(ext);
    gboolean digits = length >= 2 && strspn(ext, "0123456789") == length;
    if (digits) /* name.7z.001, name.zip.001 */
        return TRUE;
    if ((ext[0] == 'r' || ext[0] == 'R' || ext[0] == 'z' || ext[0] == 'Z') && length == 3 && g_ascii_isdigit(ext[1]) && g_ascii_isdigit(ext[2]))
        return TRUE; /* name.r00, name.z01 */
    if (!g_ascii_strcasecmp(ext, "rar") && dot - name > 6) {
        const char* part = g_strrstr_len(name, dot - name, ".part");
        return part && part + 5 < dot && strspn(part + 5, "0123456789") == (size_t)(dot - part - 5);
    }
    return FALSE;
}

/* Asks the service what d's archive holds: on the server (before and while
 * it downloads) or, once it is done, on the drive. */
static void startPeek(Download* d, gboolean local)
{
    if (d->peeking || !isArchiveName(d->r.file))
        return;
    setString(&d->peekError, NULL);
    if (local) {
        if (d->r.state != PS5WK_DL_DONE)
            return;
        char id[16];
        snprintf(id, sizeof id, "%u", d->r.id);
        char* path = pathIn(d, FALSE);
        d->peeking = TRUE;
        d->peekedLocal = TRUE;
        ps5wk_dlservice_send("peek", "id", id, "path", path, NULL);
        g_free(path);
    } else {
        if (d->r.engine != PS5WK_DL_SERVICE || d->r.size <= 0 || isSetPartName(d->r.file))
            return;
        d->peeking = TRUE;
        withCookies(d, TRUE, FALSE);
    }
    changed(FALSE);
}

static const char* afterNoun(const char* after)
{
    return !g_strcmp0(after, "extract-install") ? "unpack and install the app"
        : !g_strcmp0(after, "extract-image")    ? "unpack and make an exFAT image"
        : !g_strcmp0(after, "extract")          ? "unpack it"
                                                : "keep the file";
}

/* "peekend": what the archive holds. With "ask" off, an archive holding one
 * app gets what was chosen for the last one (never a first time). */
static void peekEnded(Download* d, GHashTable* params)
{
    d->peeking = FALSE;
    const char* ok = g_hash_table_lookup(params, "ok");
    const char* json = g_hash_table_lookup(params, "json");
    if (ok && !strcmp(ok, "1") && json && *json == '{') {
        setString(&d->peek, json);
        setString(&d->peekError, NULL);
    } else {
        setString(&d->peekError, g_hash_table_lookup(params, "error"));
        spike_log("[dl] %u: couldn't look inside: %s", d->r.id, d->peekError ? d->peekError : "?");
    }
    gboolean oneApp = d->peek && strstr(d->peek, "\"titles\":1,") && strstr(d->peek, "\"refused\":null");
    if (oneApp && !settings.ask && !d->r.after && d->r.jobState == PS5WK_JOB_NONE && settings.appAfter && strcmp(settings.appAfter, "none")
        && (d->r.state == PS5WK_DL_RUNNING || d->r.state == PS5WK_DL_PAUSED || d->r.state == PS5WK_DL_DONE)) {
        setString(&d->r.after, settings.appAfter);
        spike_log("[dl] %u: holds an app: %s, as last time", d->r.id, settings.appAfter);
        notice("%s holds an app: the browser will %s, as last time", d->r.file, afterNoun(settings.appAfter));
        if (d->r.state == PS5WK_DL_DONE)
            startAfter(d);
    }
    changed(FALSE);
}

/* WebKit's response (headers only) became the service's download. */
static void handOff(Download* d, WebKitDownload* download, WebKitURIResponse* response, const char* suggested, gint64 length)
{
    const char* finalURL = webkit_uri_response_get_uri(response);
    char* clean = sanitize(suggested, finalURL ? finalURL : d->r.url);
    g_free(d->r.file);
    /* a name no other entry has (removing one deletes its name's .part),
     * free in the downloads folder too; the service checks other folders */
    d->r.file = uniqueName(clean, d);
    g_free(clean);
    setString(&d->r.mime, webkit_uri_response_get_mime_type(response));
    setString(&d->r.etag, header(response, "ETag"));
    setString(&d->r.lastModified, header(response, "Last-Modified"));
    d->r.size = length;
    d->r.received = 0;
    d->r.engine = PS5WK_DL_SERVICE;
    g_clear_pointer(&d->r.moveTo, g_free); /* the service writes in the folder itself */
    WebKitWebView* view = webkit_download_get_web_view(download);
    if (d->session)
        g_object_unref(d->session);
    d->session = g_object_ref(view ? webkit_web_view_get_network_session(view) : networkSession);
    const char* page = view ? webkit_web_view_get_uri(view) : NULL;
    if (page && (g_str_has_prefix(page, "http://") || g_str_has_prefix(page, "https://")) && strcmp(page, d->r.url))
        setString(&d->r.referer, page);
    d->handingOff = TRUE;
    webkit_download_cancel(download);
    ps5wk_browser_download_started(view);
    char size[32];
    formatBytes(size, sizeof size, length);
    spike_log("[dl] %u: %s, %s, ranges: to the download service (%u connections)%s", d->r.id, d->r.file, size, settings.connections,
        settings.ask ? ", asking where to save it" : "");
    d->announced = TRUE;
    if (settings.ask) {
        d->r.state = PS5WK_DL_ASKING;
        changed(TRUE);
        startPeek(d, FALSE);
        openManager();
        return;
    }
    setString(&d->r.dir, settings.folder);
    notice("Download started: %s\n(Options, then R1: Downloads)", d->r.file);
    serviceStart(d, TRUE);
    startPeek(d, FALSE);
}

/* Cookies for the service's requests: the browser's for the original and
 * the final address, as cookies.txt lines. */
typedef struct {
    unsigned id;
    unsigned pending;
    gboolean fresh;
    gboolean peek; /* for a look inside the archive, not the download */
    GString* text;
} CookieRequest;

static void appendCookies(GString* text, GList* cookies)
{
    for (GList* item = cookies; item; item = item->next) {
        SoupCookie* cookie = item->data;
        const char* domain = soup_cookie_get_domain(cookie);
        const char* name = soup_cookie_get_name(cookie);
        const char* value = soup_cookie_get_value(cookie);
        if (!domain || !name || strpbrk(name, "\t\n") || (value && strpbrk(value, "\t\n")))
            continue;
        GDateTime* expires = soup_cookie_get_expires(cookie);
        g_string_append_printf(text, "%s\t%s\t%s\t%s\t%lld\t%s\t%s\n", domain, domain[0] == '.' ? "TRUE" : "FALSE",
            soup_cookie_get_path(cookie) ? soup_cookie_get_path(cookie) : "/", soup_cookie_get_secure(cookie) ? "TRUE" : "FALSE",
            expires ? (long long)g_date_time_to_unix(expires) : 0LL, name, value ? value : "");
    }
}

static void sendFetch(Download* d, const char* cookies, gboolean fresh)
{
    if (d->r.state != PS5WK_DL_RUNNING || d->r.engine != PS5WK_DL_SERVICE)
        return;
    char id[16], conns[8];
    snprintf(id, sizeof id, "%u", d->r.id);
    snprintf(conns, sizeof conns, "%u", settings.connections);
    ps5wk_dlservice_send("fetch", "id", id, "url", d->r.url, "ua", *userAgent ? userAgent : NULL, "ref", d->r.referer, "cookies",
        cookies && *cookies ? cookies : NULL, "dir", folderOf(d), "name", d->r.file, "fresh", fresh ? "1" : "0", "conns", conns, NULL);
    spike_log("[dl] %u: %s %s to %s/%s (%u connections)", d->r.id, fresh ? "asking" : "continuing", d->r.url, folderOf(d), d->r.file,
        settings.connections);
}

static void sendPeek(Download* d, const char* cookies)
{
    char id[16], size[24];
    snprintf(id, sizeof id, "%u", d->r.id);
    snprintf(size, sizeof size, "%" G_GINT64_FORMAT, d->r.size);
    ps5wk_dlservice_send("peek", "id", id, "url", d->r.url, "size", size, "ua", *userAgent ? userAgent : NULL, "ref", d->r.referer,
        "cookies", cookies && *cookies ? cookies : NULL, NULL);
}

static void onCookies(GObject* source, GAsyncResult* result, gpointer data)
{
    CookieRequest* request = data;
    GList* cookies = webkit_cookie_manager_get_cookies_finish(WEBKIT_COOKIE_MANAGER(source), result, NULL);
    appendCookies(request->text, cookies);
    g_list_free_full(cookies, (GDestroyNotify)soup_cookie_free);
    if (--request->pending)
        return;
    Download* d = byID(request->id);
    if (d && request->peek)
        sendPeek(d, request->text->str);
    else if (d)
        sendFetch(d, request->text->str, request->fresh);
    g_string_free(request->text, TRUE);
    g_free(request);
}

static void serviceStart(Download* d, gboolean fresh)
{
    if (d->retryTimer) {
        g_source_remove(d->retryTimer);
        d->retryTimer = 0;
        d->retryAt = 0;
    }
    d->r.state = PS5WK_DL_RUNNING;
    d->pausing = d->cancelling = d->stalled = FALSE;
    d->responded = FALSE;
    d->speed = 0;
    d->connections = 0;
    d->attemptBytes = 0;
    d->sampleTime = 0;
    d->lastData = g_get_monotonic_time();
    if (fresh)
        d->r.received = 0;
    d->serviceFrom = d->r.received;
    changed(TRUE);
    withCookies(d, FALSE, fresh);
}

/* The browser's cookies for d's original and final address, then the
 * service's fetch (or peek). */
static void withCookies(Download* d, gboolean peek, gboolean fresh)
{
    WebKitNetworkSession* session = d->session ? d->session : networkSession;
    WebKitCookieManager* manager = session ? webkit_network_session_get_cookie_manager(session) : NULL;
    if (!manager) {
        if (peek)
            sendPeek(d, NULL);
        else
            sendFetch(d, NULL, fresh);
        return;
    }
    CookieRequest* request = g_new0(CookieRequest, 1);
    request->id = d->r.id;
    request->fresh = fresh;
    request->peek = peek;
    request->text = g_string_new(NULL);
    request->pending = 1;
    gboolean other = d->r.finalURL && strcmp(d->r.finalURL, d->r.url);
    if (other)
        request->pending++;
    webkit_cookie_manager_get_cookies(manager, d->r.url, NULL, onCookies, request);
    if (other)
        webkit_cookie_manager_get_cookies(manager, d->r.finalURL, NULL, onCookies, request);
}

static gboolean retryFired(gpointer data);

/* A transfer on the service ended (fetchend). */
static void serviceEnded(Download* d, GHashTable* params)
{
    const char* state = g_hash_table_lookup(params, "state");
    const char* error = g_hash_table_lookup(params, "error");
    d->speed = 0;
    d->connections = 0;
    if (!state)
        state = "failed";
    if (!strcmp(state, "done")) {
        d->r.state = PS5WK_DL_DONE;
        d->r.finished = nowSeconds();
        d->r.size = d->r.received;
        d->retries = 0;
        if (d->r.error && strncmp(d->r.error, "Started over", 12))
            setError(d, NULL);
        spike_log("[dl] %u: done: %s/%s, %" G_GINT64_FORMAT " bytes (%" G_GINT64_FORMAT " this time)", d->r.id, folderOf(d), d->r.file,
            d->r.received, d->r.received - d->serviceFrom);
        notice("Downloaded %s", d->r.file);
        changed(TRUE);
        hashNext();
        startAfter(d);
        return;
    }
    if (!strcmp(state, "paused")) {
        if (d->r.state == PS5WK_DL_RUNNING && !windingDown) {
            /* not asked by the title: the service stopped (it is ending) */
            d->r.state = PS5WK_DL_PAUSED;
            setError(d, "The download service stopped");
        }
        changed(TRUE);
        return;
    }
    if (!strcmp(state, "cancelled")) {
        if (d->r.state != PS5WK_DL_CANCELLED) {
            d->r.state = PS5WK_DL_CANCELLED;
            d->r.received = 0;
        }
        changed(TRUE);
        return;
    }
    /* failed */
    if (d->r.state != PS5WK_DL_RUNNING) {
        changed(TRUE);
        return;
    }
    const char* retryable = g_hash_table_lookup(params, "retryable");
    static const unsigned delays[] = { 2, 5, 15, 30 };
    if (retryable && !strcmp(retryable, "1") && d->retries < G_N_ELEMENTS(delays)) {
        unsigned delay = delays[d->retries++];
        setError(d, error && *error ? error : "Connection lost");
        d->retryTimer = g_timeout_add_seconds(delay, retryFired, d);
        d->retryAt = g_get_monotonic_time() + delay * G_USEC_PER_SEC;
        spike_log("[dl] %u: retry %u in %u s (%s)", d->r.id, d->retries, delay, d->r.error);
        changed(TRUE);
        return;
    }
    d->r.state = PS5WK_DL_FAILED;
    if (retryable && !strcmp(retryable, "1")) {
        char* why = g_strdup_printf("%s (tried %u times)", error && *error ? error : "Connection lost", (unsigned)G_N_ELEMENTS(delays) + 1);
        setError(d, why);
        g_free(why);
    } else
        setError(d, error && *error ? error : "Failed");
    notice("Download failed: %s\n%s", d->r.file ? d->r.file : d->r.url, d->r.error);
    changed(TRUE);
}

static void serviceProgress(Download* d, GHashTable* params)
{
    if (d->r.state != PS5WK_DL_RUNNING)
        return;
    gint64 received = numberParam(params, "recv", d->r.received);
    gint64 total = numberParam(params, "total", -1);
    gint64 now = g_get_monotonic_time();
    if (received > d->r.received) {
        d->lastData = now;
        d->attemptBytes += received - d->r.received;
        if (d->retries && d->attemptBytes >= 1024 * 1024)
            d->retries = 0; /* it works again */
    }
    d->r.received = received;
    if (total > 0)
        d->r.size = total;
    double speed = (double)numberParam(params, "speed", 0);
    d->speed = d->speed > 0 && speed > 0 ? d->speed * 0.5 + speed * 0.5 : speed;
    d->connections = (unsigned)numberParam(params, "conns", 0);
    if (numberParam(params, "status", 0) > 0 && !d->responded) {
        d->responded = TRUE;
        if (d->r.error && strncmp(d->r.error, "Started over", 12))
            setError(d, NULL);
    }
    if (numberParam(params, "restarted", 0) && !d->r.restarted) {
        d->r.restarted = TRUE;
        setError(d, "Started over from zero: the server's file changed or it can't resume");
    }
    const char* final = g_hash_table_lookup(params, "final");
    if (final && *final)
        setString(&d->r.finalURL, final);
    const char* etag = g_hash_table_lookup(params, "etag");
    if (etag && *etag)
        setString(&d->r.etag, etag);
    const char* lm = g_hash_table_lookup(params, "lm");
    if (lm && *lm)
        setString(&d->r.lastModified, lm);
    changed(FALSE);
}

static const char* jobNoun(const char* job)
{
    return !g_strcmp0(job, "extract") ? "Unpacking" : !g_strcmp0(job, "install") ? "Installing" : !g_strcmp0(job, "image") ? "Making the image"
        : !g_strcmp0(job, "move") ? "Moving" : "Working";
}

static void jobEnded(Download* d, GHashTable* params)
{
    const char* ok = g_hash_table_lookup(params, "ok");
    const char* error = g_hash_table_lookup(params, "error");
    const char* out = g_hash_table_lookup(params, "out");
    const char* titleID = g_hash_table_lookup(params, "title");
    const char* name = g_hash_table_lookup(params, "name");
    gboolean success = ok && !strcmp(ok, "1");
    d->jobPhase[0] = 0;
    d->jobDone = d->jobTotal = 0;
    d->r.jobState = success ? PS5WK_JOB_DONE : PS5WK_JOB_FAILED;
    setString(&d->r.jobError, success ? NULL : (error && *error ? error : "Failed"));
    setString(&d->r.jobOut, out);
    if (titleID && *titleID) {
        setString(&d->r.titleID, titleID);
        setString(&d->r.titleName, name);
    }
    gboolean moved = !g_strcmp0(d->r.job, "move");
    if (moved && success && out) {
        /* the file is in its folder now */
        char* folder = g_path_get_dirname(out);
        char* base = g_path_get_basename(out);
        setString(&d->r.dir, strcmp(folder, PS5WK_DOWNLOADS_DIR) ? folder : NULL);
        setString(&d->r.file, base);
        g_clear_pointer(&d->r.moveTo, g_free);
        g_free(folder);
        g_free(base);
    }
    spike_log("[dl] %u: %s %s%s%s", d->r.id, d->r.job ? d->r.job : "job", success ? "done" : "failed: ", success ? (out ? out : "") : "",
        success ? "" : d->r.jobError);
    if (!success)
        notice("%s %s failed\n%s", jobNoun(d->r.job), d->r.file ? d->r.file : "", d->r.jobError);
    else if (d->r.titleID && (!g_strcmp0(d->r.job, "install") || (!g_strcmp0(d->r.job, "extract") && !g_strcmp0(d->r.after, "extract-install"))))
        notice("Installed %s\nIt's on the home screen", d->r.titleName ? d->r.titleName : d->r.titleID);
    else if (!g_strcmp0(d->r.job, "image") || (!g_strcmp0(d->r.job, "extract") && !g_strcmp0(d->r.after, "extract-image")))
        notice("Made the image %s", out ? out : "");
    else if (!g_strcmp0(d->r.job, "extract"))
        notice("Unpacked %s\nto %s", d->r.file ? d->r.file : "", out ? out : "");
    changed(TRUE);
    if (moved && success && d->r.after)
        startAfter(d);
}

static Task* taskByID(unsigned id)
{
    for (unsigned i = 0; tasks && i < tasks->len; i++) {
        Task* t = g_ptr_array_index(tasks, i);
        if (t->id == id)
            return t;
    }
    return NULL;
}

static void taskEvent(unsigned id, const char* verb, GHashTable* params)
{
    Task* t = taskByID(id);
    if (!t)
        return;
    if (!strcmp(verb, "job")) {
        const char* phase = g_hash_table_lookup(params, "phase");
        snprintf(t->phase, sizeof t->phase, "%s", phase ? phase : "");
        t->done = numberParam(params, "done", 0);
        t->total = numberParam(params, "total", 0);
        changed(FALSE);
        return;
    }
    if (strcmp(verb, "jobend"))
        return;
    const char* ok = g_hash_table_lookup(params, "ok");
    gboolean success = ok && !strcmp(ok, "1");
    t->state = success ? PS5WK_JOB_DONE : PS5WK_JOB_FAILED;
    const char* error = g_hash_table_lookup(params, "error");
    setString(&t->error, success ? NULL : (error && *error ? error : "Failed"));
    setString(&t->out, g_hash_table_lookup(params, "out"));
    setString(&t->titleName, g_hash_table_lookup(params, "name"));
    t->phase[0] = 0;
    char* base = g_path_get_basename(t->src);
    if (!success)
        notice("%s %s failed\n%s", jobNoun(t->kind), base, t->error);
    else if (t->titleName && (!strcmp(t->kind, "install") || !g_strcmp0(t->then, "install")))
        notice("Installed %s\nIt's on the home screen", t->titleName);
    else
        notice("%s %s: done\n%s", jobNoun(t->kind), base, t->out ? t->out : "");
    g_free(base);
    changed(TRUE);
}

static void taskFree(gpointer data)
{
    Task* t = data;
    g_free(t->kind);
    g_free(t->then);
    g_free(t->src);
    g_free(t->error);
    g_free(t->out);
    g_free(t->titleName);
    g_free(t);
}

static void onServiceEvent(const char* verb, GHashTable* params, gpointer user)
{
    (void)user;
    if (!list)
        return;
    if (!strcmp(verb, "gone")) {
        /* the service ended: what ran there is retried (which starts it again) */
        for (unsigned i = 0; i < list->len; i++) {
            Download* d = g_ptr_array_index(list, i);
            if (d->r.engine == PS5WK_DL_SERVICE && d->r.state == PS5WK_DL_RUNNING && !d->retryTimer) {
                GHashTable* failed = g_hash_table_new(g_str_hash, g_str_equal);
                g_hash_table_insert(failed, "state", "failed");
                g_hash_table_insert(failed, "retryable", "1");
                g_hash_table_insert(failed, "error", "The download service stopped");
                serviceEnded(d, failed);
                g_hash_table_unref(failed);
            }
            if (d->r.jobState == PS5WK_JOB_RUNNING) {
                d->r.jobState = PS5WK_JOB_FAILED;
                setString(&d->r.jobError, "The download service stopped");
                d->jobPhase[0] = 0;
                changed(TRUE);
            }
            if (d->peeking) {
                d->peeking = FALSE;
                d->peekedLocal = FALSE;
                changed(FALSE);
            }
        }
        for (unsigned i = 0; tasks && i < tasks->len; i++) {
            Task* t = g_ptr_array_index(tasks, i);
            if (t->state == PS5WK_JOB_RUNNING) {
                t->state = PS5WK_JOB_FAILED;
                setString(&t->error, "The download service stopped");
                changed(TRUE);
            }
        }
        return;
    }
    const char* taskID = g_hash_table_lookup(params, "id");
    if (taskID && taskID[0] == 't') {
        taskEvent((unsigned)atoi(taskID + 1), verb, params);
        return;
    }
    Download* d = byParam(params);
    if (!d)
        return;
    const char* name = g_hash_table_lookup(params, "name");
    if (name && *name && !strchr(name, '/') && name[0] != '.')
        setString(&d->r.file, name);
    if (!strcmp(verb, "fetch"))
        serviceProgress(d, params);
    else if (!strcmp(verb, "fetchend")) {
        serviceProgress(d, params);
        serviceEnded(d, params);
    } else if (!strcmp(verb, "job")) {
        const char* phase = g_hash_table_lookup(params, "phase");
        snprintf(d->jobPhase, sizeof d->jobPhase, "%s", phase ? phase : "");
        d->jobDone = numberParam(params, "done", 0);
        d->jobTotal = numberParam(params, "total", 0);
        changed(FALSE);
    } else if (!strcmp(verb, "jobend"))
        jobEnded(d, params);
    else if (!strcmp(verb, "peekend"))
        peekEnded(d, params);
}

/* A job on d's finished file. kind: "extract", "install", "image", "move". */
static const char* startJob(Download* d, const char* kind, const char* then, const char* destination)
{
    if (d->r.state != PS5WK_DL_DONE || !d->r.file)
        return "not a finished download";
    if (d->r.jobState == PS5WK_JOB_RUNNING)
        return "a job is already running for it";
    char id[16];
    snprintf(id, sizeof id, "%u", d->r.id);
    char* src = pathIn(d, FALSE);
    char* dest = destination ? g_strdup(destination) : NULL;
    char* image = NULL;
    /* "name.part1.rar", "name.tar.gz", "name.7z.001": the name without its archive parts */
    char* stem = g_strdup(d->r.file);
    static const char* const tails[] = { ".tar.gz", ".tar.xz", ".tar.bz2", ".tgz" };
    gboolean cut = FALSE;
    for (unsigned i = 0; i < G_N_ELEMENTS(tails) && !cut; i++) {
        if (g_str_has_suffix(stem, tails[i]) && strlen(stem) > strlen(tails[i])) {
            stem[strlen(stem) - strlen(tails[i])] = 0;
            cut = TRUE;
        }
    }
    for (int pass = 0; pass < 3 && !cut; pass++) {
        char* dot = strrchr(stem, '.');
        if (!dot || dot == stem)
            break;
        gboolean number = strlen(dot + 1) >= 2 && strspn(dot + 1, "0123456789") == strlen(dot + 1);
        gboolean part = !g_ascii_strncasecmp(dot + 1, "part", 4) && strspn(dot + 5, "0123456789") == strlen(dot + 5);
        *dot = 0;
        if (!number && !part)
            break;
    }
    if (!strcmp(kind, "extract") && !dest)
        dest = g_strdup_printf("%s/%s", folderOf(d), stem);
    /* images: <drive>/homebrew/<TITLE_ID>.exfat, the service decides */
    if (!strcmp(kind, "extract") && !g_strcmp0(then, "image"))
        image = g_strdup("auto");
    if (!strcmp(kind, "image") && !dest)
        dest = g_strdup("auto");
    g_free(stem);
    setString(&d->r.job, kind);
    d->r.jobState = PS5WK_JOB_RUNNING;
    setString(&d->r.jobError, NULL);
    snprintf(d->jobPhase, sizeof d->jobPhase, "%s", kind);
    d->jobDone = d->jobTotal = 0;
    ps5wk_dlservice_send("job", "id", id, "kind", kind, "src", src, "dest", dest, "then", then, "image", image, "delete",
        !strcmp(kind, "extract") && settings.deleteArchives ? "1" : NULL, NULL);
    spike_log("[dl] %u: %s %s -> %s%s%s", d->r.id, kind, src, dest ? dest : "-", then ? " then " : "", then ? then : "");
    g_free(src);
    g_free(dest);
    g_free(image);
    changed(TRUE);
    return NULL;
}

/* What d asked for when it was saved: a move to its folder (a WebKit
 * download for another folder), then unpacking. */
static void startAfter(Download* d)
{
    if (d->r.moveTo && titleSees(d) && strcmp(d->r.moveTo, PS5WK_DOWNLOADS_DIR)) {
        char* dest = g_strdup_printf("%s/%s", d->r.moveTo, d->r.file);
        startJob(d, "move", NULL, dest);
        g_free(dest);
        return;
    }
    if (!d->r.after)
        return;
    if (!strcmp(d->r.after, "extract"))
        startJob(d, "extract", NULL, NULL);
    else if (!strcmp(d->r.after, "extract-install"))
        startJob(d, "extract", "install", NULL);
    else if (!strcmp(d->r.after, "extract-image"))
        startJob(d, "extract", "image", NULL);
}

/* ---------------------------------------------------------------- a transfer's signals (WebKit's) */

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
    if (response && serviceWanted(d, download, response, status, length)) {
        handOff(d, download, response, suggested, length);
        return TRUE;
    }
    d->fromPage = FALSE; /* WebKit's from now on (a resume asks WebKit again) */

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
    if (d->handingOff)
        return; /* the service has it: "finished" follows */
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
    if (d->handingOff) {
        d->handingOff = FALSE;
        endTransfer(d);
        if (d->removing)
            g_ptr_array_remove(list, d);
        changed(TRUE);
        return;
    }
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
    if (d->r.state == PS5WK_DL_DONE)
        startAfter(d);
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
    if (d->r.engine == PS5WK_DL_SERVICE) {
        serviceStart(d, fresh);
        return;
    }
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
    d->fromPage = TRUE;
    /* a WebKit transfer saves to the user's downloads folder; another
     * default folder gets the file when it is done */
    if (settings.folder && strcmp(settings.folder, PS5WK_DOWNLOADS_DIR))
        d->r.moveTo = g_strdup(settings.folder);
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
            char conns[24] = "";
            if (d->connections > 1)
                snprintf(conns, sizeof conns, " · %u connections", d->connections);
            snprintf(r->detail, sizeof r->detail, "%s%s%s%s%s%s%s%s", got, d->r.size >= 0 ? " of " : "", d->r.size >= 0 ? total : "", *speed ? " · " : "", speed,
                *eta ? " · " : "", eta, conns);
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
        if (d->r.jobState == PS5WK_JOB_RUNNING) {
            r->tone = BUI_TONE_ACTIVE;
            r->running = TRUE;
            r->progress = d->jobTotal > 0 ? CLAMP((double)d->jobDone / d->jobTotal, 0, 1) : -1;
            const char* phase = *d->jobPhase ? d->jobPhase : d->r.job;
            snprintf(r->state, sizeof r->state, "%s%s", jobNoun(phase), r->progress >= 0 ? "" : "…");
            if (r->progress >= 0)
                snprintf(r->state + strlen(r->state), sizeof r->state - strlen(r->state), " %.0f%%", r->progress * 100);
        } else if (d->r.jobState == PS5WK_JOB_FAILED) {
            r->tone = BUI_TONE_WARN;
            snprintf(r->detail, sizeof r->detail, "%s failed: %s", jobNoun(d->r.job), d->r.jobError ? d->r.jobError : "?");
        } else if (d->r.jobState == PS5WK_JOB_DONE && d->r.titleID)
            snprintf(r->state, sizeof r->state, "Installed");
        else if (d->r.jobState == PS5WK_JOB_DONE && !g_strcmp0(d->r.job, "extract"))
            snprintf(r->state, sizeof r->state, "Unpacked");
        break;
    }
    case PS5WK_DL_CANCELLED:
        r->tone = BUI_TONE_PLAIN;
        r->progress = -1;
        snprintf(r->state, sizeof r->state, "Cancelled");
        snprintf(r->detail, sizeof r->detail, "The partial file was deleted");
        break;
    case PS5WK_DL_ASKING:
        r->tone = BUI_TONE_WARN;
        r->progress = -1;
        snprintf(r->state, sizeof r->state, "Where to save?");
        snprintf(r->detail, sizeof r->detail, "%s · X chooses the folder", total);
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
        *cross = "Manage";
        *square = "Delete file";
        break;
    case PS5WK_DL_CANCELLED:
        *cross = "Download again";
        *square = "Remove";
        break;
    case PS5WK_DL_ASKING:
        *cross = "Choose folder";
        *square = "Remove";
        break;
    }
}

static char* idText(const Download* d, char* out, size_t size)
{
    snprintf(out, size, "%u", d->r.id);
    return out;
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
    if (d->r.engine == PS5WK_DL_SERVICE) {
        char id[16];
        d->pausing = TRUE;
        ps5wk_dlservice_send("stop", "id", idText(d, id, sizeof id), NULL);
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
    case PS5WK_DL_DONE:
    case PS5WK_DL_ASKING:
        openManager();
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
    if (d->r.engine == PS5WK_DL_SERVICE && d->r.state == PS5WK_DL_RUNNING) {
        char id[16];
        ps5wk_dlservice_send("cancel", "id", idText(d, id, sizeof id), NULL);
    } else if (d->r.file && d->r.state != PS5WK_DL_ASKING) {
        removeIn(d, TRUE);
        if (d->r.state == PS5WK_DL_DONE)
            removeIn(d, FALSE);
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
        } else if (d->r.engine == PS5WK_DL_SERVICE && d->r.state == PS5WK_DL_RUNNING && !d->retryTimer) {
            char id[16];
            d->cancelling = TRUE;
            ps5wk_dlservice_send("cancel", "id", idText(d, id, sizeof id), NULL);
        } else if (d->r.file)
            removeIn(d, TRUE);
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
    snprintf(path, sizeof path, "%s/%s%s", folderOf(d), d->r.file ? d->r.file : "?",
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

/* ---------------------------------------------------------------- the download manager's page (dlm.c) */

static gboolean isArchiveName(const char* name)
{
    static const char* const tails[] = { ".zip", ".7z", ".rar", ".tar", ".tgz", ".tar.gz", ".tar.xz", ".tar.bz2", ".001" };
    size_t length = name ? strlen(name) : 0;
    for (unsigned i = 0; i < G_N_ELEMENTS(tails); i++) {
        size_t tail = strlen(tails[i]);
        if (length > tail && !g_ascii_strcasecmp(name + length - tail, tails[i]))
            return TRUE;
    }
    return FALSE;
}

static void jsonKey(GString* out, const char* key, const char* value)
{
    g_string_append_printf(out, ",\"%s\":", key);
    if (value)
        ps5wk_json_string(out, value);
    else
        g_string_append(out, "null");
}

/* Main loop: the whole state for the page, kept for route threads. */
static void updateManagerSnapshot(void)
{
    GString* out = g_string_new("{\"ok\":true,\"settings\":{");
    g_string_append_printf(out, "\"connections\":%u,\"ask\":%s,\"deleteArchives\":%s", settings.connections, settings.ask ? "true" : "false",
        settings.deleteArchives ? "true" : "false");
    jsonKey(out, "folder", settings.folder ? settings.folder : PS5WK_DOWNLOADS_DIR);
    jsonKey(out, "userFolder", PS5WK_DOWNLOADS_DIR);
    jsonKey(out, "appAfter", settings.appAfter);
    g_string_append(out, ",\"favorites\":[");
    for (unsigned i = 0; settings.favorites && settings.favorites[i]; i++) {
        if (i)
            g_string_append_c(out, ',');
        ps5wk_json_string(out, settings.favorites[i]);
    }
    g_string_append(out, "]},\"downloads\":[");
    gint64 now = g_get_monotonic_time();
    for (unsigned i = 0; list && i < list->len; i++) {
        Download* d = g_ptr_array_index(list, i);
        char host[128];
        g_string_append_printf(out, "%s{\"id\":%u,\"state\":\"%s\"", i ? "," : "", d->r.id, ps5wk_download_state_name(d->r.state));
        jsonKey(out, "file", d->r.file);
        jsonKey(out, "dir", folderOf(d));
        jsonKey(out, "url", d->r.url);
        jsonKey(out, "host", hostOf(d->r.url, host, sizeof host));
        g_string_append_printf(out, ",\"size\":%" G_GINT64_FORMAT ",\"received\":%" G_GINT64_FORMAT ",\"speed\":%.0f,\"conns\":%u,\"service\":%s",
            d->r.size, d->r.received, d->r.state == PS5WK_DL_RUNNING ? d->speed : 0, d->connections, d->r.engine == PS5WK_DL_SERVICE ? "true" : "false");
        g_string_append_printf(out, ",\"connecting\":%s,\"retryIn\":%.0f,\"archive\":%s,\"started\":%" G_GINT64_FORMAT ",\"finished\":%" G_GINT64_FORMAT,
            d->r.state == PS5WK_DL_RUNNING && !d->responded ? "true" : "false", d->retryTimer ? MAX(0, (d->retryAt - now) / 1e6) : 0.0,
            isArchiveName(d->r.file) ? "true" : "false", d->r.started, d->r.finished);
        jsonKey(out, "error", d->r.error);
        jsonKey(out, "after", d->r.after);
        jsonKey(out, "job", d->r.job);
        g_string_append_printf(out, ",\"jobState\":%d,\"jobDone\":%" G_GINT64_FORMAT ",\"jobTotal\":%" G_GINT64_FORMAT, d->r.jobState, d->jobDone, d->jobTotal);
        jsonKey(out, "jobPhase", *d->jobPhase ? d->jobPhase : NULL);
        jsonKey(out, "jobError", d->r.jobError);
        jsonKey(out, "jobOut", d->r.jobOut);
        jsonKey(out, "titleId", d->r.titleID);
        jsonKey(out, "titleName", d->r.titleName);
        jsonKey(out, "sha256", d->r.sha256);
        g_string_append_printf(out, ",\"peeking\":%s,\"peekedLocal\":%s,\"peek\":%s", d->peeking ? "true" : "false",
            d->peekedLocal ? "true" : "false", d->peek ? d->peek : "null");
        jsonKey(out, "peekError", d->peekError);
        g_string_append_c(out, '}');
    }
    g_string_append(out, "],\"tasks\":[");
    for (unsigned i = 0; tasks && i < tasks->len; i++) {
        Task* t = g_ptr_array_index(tasks, i);
        g_string_append_printf(out, "%s{\"id\":\"t%u\",\"state\":%d,\"done\":%" G_GINT64_FORMAT ",\"total\":%" G_GINT64_FORMAT, i ? "," : "", t->id,
            t->state, t->done, t->total);
        jsonKey(out, "kind", t->kind);
        jsonKey(out, "then", t->then);
        jsonKey(out, "src", t->src);
        jsonKey(out, "phase", *t->phase ? t->phase : NULL);
        jsonKey(out, "error", t->error);
        jsonKey(out, "out", t->out);
        jsonKey(out, "titleName", t->titleName);
        g_string_append_c(out, '}');
    }
    g_string_append(out, "]}\n");
    g_mutex_lock(&snapLock);
    g_free(snapManagerJSON);
    snapManagerJSON = g_string_free(out, FALSE);
    g_mutex_unlock(&snapLock);
}

char* ps5wk_downloads_manager_json(void)
{
    g_mutex_lock(&snapLock);
    char* json = g_strdup(snapManagerJSON ? snapManagerJSON : "{\"ok\":false,\"error\":\"starting\"}\n");
    g_mutex_unlock(&snapLock);
    return json;
}

static gboolean folderAllowed(const char* dir)
{
    if (!dir || (strncmp(dir, "/data/", 6) && strncmp(dir, "/mnt/usb", 8) && strncmp(dir, "/mnt/ext", 8)))
        return FALSE;
    return !strstr(dir, "/../") && !g_str_has_suffix(dir, "/..") && !strstr(dir, "/./") && !strstr(dir, "//") && strlen(dir) < 900;
}

static const char* startTask(const char* kind, const char* then, const char* src)
{
    if (!folderAllowed(src))
        return "That file isn't in a place the browser can use";
    if (!tasks)
        tasks = g_ptr_array_new_with_free_func(taskFree);
    Task* t = g_new0(Task, 1);
    t->id = nextTask++;
    t->kind = g_strdup(kind);
    t->then = g_strdup(then);
    t->src = g_strdup(src);
    t->state = PS5WK_JOB_RUNNING;
    t->started = nowSeconds();
    snprintf(t->phase, sizeof t->phase, "%s", kind);
    g_ptr_array_insert(tasks, 0, t);
    while (tasks->len > 30)
        g_ptr_array_remove_index(tasks, tasks->len - 1);
    char id[16];
    snprintf(id, sizeof id, "t%u", t->id);
    char* dest = NULL;
    char* image = NULL;
    if (!strcmp(kind, "extract")) {
        /* next to the archive, its name without the archive's extensions */
        char* stem = g_strdup(src);
        static const char* const tails[] = { ".tar.gz", ".tar.xz", ".tar.bz2", ".tgz" };
        gboolean cut = FALSE;
        for (unsigned i = 0; i < G_N_ELEMENTS(tails) && !cut; i++) {
            if (g_str_has_suffix(stem, tails[i])) {
                stem[strlen(stem) - strlen(tails[i])] = 0;
                cut = TRUE;
            }
        }
        for (int pass = 0; pass < 3 && !cut; pass++) {
            char* dot = strrchr(stem, '.');
            char* slash = strrchr(stem, '/');
            if (!dot || (slash && dot < slash))
                break;
            gboolean number = strlen(dot + 1) >= 2 && strspn(dot + 1, "0123456789") == strlen(dot + 1);
            gboolean part = !g_ascii_strncasecmp(dot + 1, "part", 4) && strspn(dot + 5, "0123456789") == strlen(dot + 5);
            *dot = 0;
            if (!number && !part)
                break;
        }
        dest = stem;
        if (!g_strcmp0(then, "image"))
            image = g_strdup("auto");
    } else if (!strcmp(kind, "image"))
        dest = g_strdup("auto");
    ps5wk_dlservice_send("job", "id", id, "kind", kind, "src", src, "dest", dest, "then", then, "image", image, "delete",
        !strcmp(kind, "extract") && settings.deleteArchives ? "1" : NULL, NULL);
    spike_log("[dl] task %s: %s %s%s%s", id, kind, src, then ? " then " : "", then ? then : "");
    g_free(dest);
    g_free(image);
    changed(TRUE);
    return NULL;
}

static const char* textParam(GHashTable* params, const char* key)
{
    const char* value = g_hash_table_lookup(params, key);
    return value && *value ? value : NULL;
}

/* Main loop: one of the page's actions (do=...). NULL or why not. */
const char* ps5wk_downloads_manage(GHashTable* params)
{
    const char* action = textParam(params, "do");
    if (!action)
        return "do=<action> missing";
    const char* idText = textParam(params, "id");
    Download* d = idText ? byID((unsigned)atoi(idText)) : NULL;

    if (!strcmp(action, "settings")) {
        const char* connections = textParam(params, "connections");
        if (connections)
            settings.connections = (unsigned)CLAMP(atoi(connections), 1, 16);
        const char* ask = textParam(params, "ask");
        if (ask)
            settings.ask = !strcmp(ask, "1") || !strcmp(ask, "true");
        const char* del = textParam(params, "deleteArchives");
        if (del)
            settings.deleteArchives = !strcmp(del, "1") || !strcmp(del, "true");
        /* "": forget it */
        const char* appAfter = g_hash_table_lookup(params, "appAfter");
        if (appAfter) {
            if (*appAfter && strcmp(appAfter, "none") && strcmp(appAfter, "extract") && strcmp(appAfter, "extract-install")
                && strcmp(appAfter, "extract-image"))
                return "appAfter=none|extract|extract-install|extract-image or nothing";
            setString(&settings.appAfter, appAfter);
        }
        const char* folder = textParam(params, "folder");
        if (folder) {
            if (!folderAllowed(folder))
                return "That folder isn't one the browser can use";
            setString(&settings.folder, strcmp(folder, PS5WK_DOWNLOADS_DIR) ? folder : NULL);
        }
        saveSettings();
        changed(FALSE);
        return NULL;
    }
    if (!strcmp(action, "favorite")) {
        const char* path = textParam(params, "path");
        const char* on = textParam(params, "on");
        if (!folderAllowed(path))
            return "That folder isn't one the browser can use";
        GPtrArray* kept = g_ptr_array_new();
        for (unsigned i = 0; settings.favorites && settings.favorites[i]; i++) {
            if (strcmp(settings.favorites[i], path))
                g_ptr_array_add(kept, g_strdup(settings.favorites[i]));
        }
        if (on && !strcmp(on, "1") && kept->len < 20)
            g_ptr_array_add(kept, g_strdup(path));
        g_ptr_array_add(kept, NULL);
        g_strfreev(settings.favorites);
        settings.favorites = (char**)g_ptr_array_free(kept, FALSE);
        saveSettings();
        changed(FALSE);
        return NULL;
    }
    if (!strcmp(action, "task")) {
        const char* kind = textParam(params, "kind");
        const char* path = textParam(params, "path");
        if (!kind || !path)
            return "kind and path";
        if (!strcmp(kind, "extract") || !strcmp(kind, "install") || !strcmp(kind, "image"))
            return startTask(kind, NULL, path);
        if (!strcmp(kind, "extract-install"))
            return startTask("extract", "install", path);
        if (!strcmp(kind, "extract-image"))
            return startTask("extract", "image", path);
        return "kind=extract|extract-install|extract-image|install|image";
    }
    if (!strcmp(action, "taskstop") || !strcmp(action, "taskremove")) {
        const char* id = textParam(params, "id");
        Task* t = id && id[0] == 't' ? taskByID((unsigned)atoi(id + 1)) : NULL;
        if (!t)
            return "no such task";
        if (!strcmp(action, "taskstop") && t->state == PS5WK_JOB_RUNNING)
            ps5wk_dlservice_send("jobstop", "id", id, NULL);
        else if (!strcmp(action, "taskremove") && t->state != PS5WK_JOB_RUNNING)
            g_ptr_array_remove(tasks, t);
        changed(FALSE);
        return NULL;
    }
    if (!d)
        return "no such download";
    unsigned index = 0;
    g_ptr_array_find(list, d, &index);
    if (!strcmp(action, "save")) {
        if (d->r.state != PS5WK_DL_ASKING)
            return "it isn't waiting for a folder";
        const char* dir = textParam(params, "dir");
        if (!folderAllowed(dir))
            return "That folder isn't one the browser can use";
        const char* after = textParam(params, "after");
        setString(&d->r.dir, strcmp(dir, PS5WK_DOWNLOADS_DIR) ? dir : NULL);
        setString(&d->r.after, after && strcmp(after, "none") ? after : NULL);
        const char* remember = textParam(params, "remember");
        if (remember && !strcmp(remember, "1"))
            setString(&settings.folder, strcmp(dir, PS5WK_DOWNLOADS_DIR) ? dir : NULL);
        /* chosen for an archive holding one app: suggested for the next */
        const char* app = textParam(params, "app");
        if (app && !strcmp(app, "1"))
            setString(&settings.appAfter, after ? after : "none");
        if ((remember && !strcmp(remember, "1")) || (app && !strcmp(app, "1")))
            saveSettings();
        spike_log("[dl] %u: save to %s%s%s", d->r.id, folderOf(d), d->r.after ? ", then " : "", d->r.after ? d->r.after : "");
        notice("Download started: %s", d->r.file);
        serviceStart(d, TRUE);
        return NULL;
    }
    if (!strcmp(action, "peek")) {
        /* a finished archive: what it holds (once; the page asks) */
        if (d->r.state != PS5WK_DL_DONE)
            return "only finished downloads";
        if (!d->peeking && !d->peekedLocal)
            startPeek(d, TRUE);
        return NULL;
    }
    if (!strcmp(action, "after")) {
        /* what happens when it is done (for one still running) */
        const char* after = textParam(params, "after");
        setString(&d->r.after, after && strcmp(after, "none") ? after : NULL);
        changed(TRUE);
        return NULL;
    }
    if (!strcmp(action, "pause") || !strcmp(action, "resume") || !strcmp(action, "cancel") || !strcmp(action, "remove"))
        return ps5wk_downloads_action(action, (int)index, NULL);
    if (!strcmp(action, "delete")) {
        if (d->r.state == PS5WK_DL_RUNNING)
            return "pause or cancel it first";
        if (d->r.jobState == PS5WK_JOB_RUNNING)
            return "a job is running on it";
        removeEntry(d);
        changed(TRUE);
        return NULL;
    }
    if (!strcmp(action, "forget")) {
        /* the entry goes, the file stays */
        if (d->r.state != PS5WK_DL_DONE && d->r.state != PS5WK_DL_CANCELLED && d->r.state != PS5WK_DL_FAILED)
            return "only finished, failed or cancelled ones";
        if (d->download || d->r.jobState == PS5WK_JOB_RUNNING)
            return "it is busy";
        g_ptr_array_remove(list, d);
        changed(TRUE);
        return NULL;
    }
    if (!strcmp(action, "extract"))
        return startJob(d, "extract", NULL, NULL);
    if (!strcmp(action, "extract-install"))
        return startJob(d, "extract", "install", NULL);
    if (!strcmp(action, "extract-image"))
        return startJob(d, "extract", "image", NULL);
    if (!strcmp(action, "jobstop")) {
        if (d->r.jobState != PS5WK_JOB_RUNNING)
            return "no job is running";
        ps5wk_dlservice_send("jobstop", "id", idText, NULL);
        return NULL;
    }
    return "no such action";
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
            d->r.size, d->r.received, sizeIn(d, d->r.state != PS5WK_DL_DONE), d->speed, d->download || (d->r.engine == PS5WK_DL_SERVICE && d->responded && d->r.state == PS5WK_DL_RUNNING) ? "true" : "false",
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
        if (d->r.file && d->r.engine == PS5WK_DL_WEBKIT) {
            gint64 onDisk = fileSize(d->r.file, TRUE);
            if (onDisk >= 0)
                d->r.received = onDisk;
        }
        d->r.state = PS5WK_DL_PAUSED;
        setError(d, "Paused when the browser closed");
        paused++;
    }
    /* the service stops its transfers (their .ps5dl stay) when the title's
     * connection to it closes */
    ps5wk_dlservice_wind_down();
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
    loadSettings();
    ps5wk_dlservice_init(onServiceEvent, NULL);
    GPtrArray* records = ps5wk_downloads_load();
    unsigned paused = 0, dropped = 0;
    for (unsigned i = 0; i < records->len; i++) {
        PS5WKDownloadRecord* r = g_ptr_array_index(records, i);
        gboolean visible = !r->dir || !strcmp(r->dir, PS5WK_DOWNLOADS_DIR);
        if (r->state == PS5WK_DL_DONE && visible && fileSize(r->file, FALSE) < 0) {
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
        if ((dl->r.state == PS5WK_DL_PAUSED || dl->r.state == PS5WK_DL_FAILED) && dl->r.engine == PS5WK_DL_WEBKIT) {
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
    updateManagerSnapshot();
    hashNext();
}
