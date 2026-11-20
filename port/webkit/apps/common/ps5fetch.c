/*
 * ps5fetch.c: one HTTP(S) download over several connections; see ps5fetch.h.
 *
 * Threads. ps5fetch_start makes the engine thread, which starts worker 0:
 * the probe. Once the probe has shown that ranges work, the engine starts
 * more workers, up to the allowed number, while chunks are free. Each worker
 * has a SoupSession of its own and uses libsoup's sync API
 * (soup_session_send, then g_input_stream_read into a 512 KiB buffer that
 * is written with pwrite when it is full or every 250 ms). Every quarter
 * second the engine samples the speed, cancels a request that has received
 * nothing for stall_s, starts and joins workers, and (at most once a second,
 * if anything changed) rewrites the .ps5dl. It calls `ended` once the last
 * worker has gone, and has then closed or renamed the files.
 *
 * One mutex guards the shared state. A worker takes it once per buffer
 * written (to record the range, the bytes and the time), and a few times
 * per request and per chunk; nothing slow (network, data writes, the
 * .ps5dl) happens under it.
 *
 * Which bytes are on disk is a sorted list of disjoint ranges. A chunk is
 * free, taken (by one worker) or done; a worker asks for the first range of
 * its chunk that isn't on disk, then the next, so a chunk given back after
 * a failure keeps what it got.
 *
 * Choices where the header leaves room:
 *   - The probe asks the first range that isn't on disk (chunk 0 on a new
 *     download, the first missing bytes on a resume), up to its chunk's end.
 *     A 416 or a 206 without a total makes it ask again without a Range
 *     (one connection, the whole file: a 0-byte file answers 416).
 *   - Single stream (a 200): a retry asks for the whole file again; bytes
 *     before the first one missing are read and not written again. If the
 *     validator or the length changed meanwhile, everything is written
 *     again (status.restarted). A resume that gets a 200 keeps the saved
 *     bytes only if the validator and length match (and there is a
 *     validator).
 *   - No ETag and no Last-Modified: only the total is compared. A weak
 *     ETag is compared like a strong one. No If-Range is sent: the answers
 *     are checked here.
 *   - A validator or total that changes while a download runs (not on its
 *     probe) fails it as retryable ("The file changed on the server"): the
 *     next start on the path then starts over from zero.
 *   - A 206 that starts elsewhere than asked fails the download (not
 *     retryable). A 200 to a Range asked after the probe is retried.
 *   - Failure: a worker whose request failed at least twice in a row, and
 *     when nothing has arrived on any connection for stall_s, fails the
 *     download (retryable) instead of waiting again. So a connection that
 *     stalls once gets one more try; one that stalls every time fails after
 *     about 2 x stall_s, a server that refuses connections after stall_s.
 *     No new worker is started for a chunk that a worker waiting to retry
 *     will take back (it would start the count of failures over).
 *   - Retry-After (seconds) on a 408/429/5xx lengthens that wait, to 60 s
 *     at most. Lowered connections aren't raised again.
 *   - Expired final address: a 401/403/404/410 to a request that didn't go
 *     to the original address makes the next request go there; a
 *     401/403/404/410 to one that did fails the download (not retryable).
 *     A request to the current address that was redirected makes the
 *     address it ended at the current one. So a signed address that
 *     expires many times during one download is renewed each time, but a
 *     refused original address isn't asked again.
 *   - options.url is asked first even if the .ps5dl has a newer final
 *     address. options.chunk is used for a new download; a resume keeps the
 *     chunk size in its .ps5dl.
 *   - Free space: the total minus the .part's current size, plus
 *     free_margin, must be free on the .part's drive (statvfs). Checked once
 *     the probe has the total, before the .part is opened (a refused new
 *     download leaves no files). A total that isn't known isn't checked.
 *   - The .part is never extended with ftruncate, only shortened: to 0 when
 *     a resume starts over, to the total when it is longer at the end.
 *   - The .ps5dl is written only once the .part has been opened (or if it
 *     was there before). It is replaced by writing <path>.part.ps5dl.tmp
 *     and renaming it, without fsync; the .part gets an fsync when a
 *     download pauses or fails.
 *   - Bytes on disk the .ps5dl lists beyond the .part's size are dropped.
 *     A .ps5dl that lists bytes but no .part: a new download (restarted).
 *   - The saved bytes are already all there (a stop between the last byte
 *     and the rename): no request, the .part is renamed.
 *   - user_agent NULL: libsoup's own ("libsoup/3.x"). Accept-Encoding:
 *     identity, and the session has no content decoder, so a server that
 *     compresses anyway gets its bytes saved as sent.
 *   - Cookies: "#HttpOnly_" lines are read (HttpOnly cookies); a domain is
 *     given a leading '.' when includeSubdomains is TRUE and loses it when
 *     FALSE; expired ones are skipped. A Secure one is SameSite=None, the
 *     others Lax (libsoup drops a SameSite=None cookie that isn't Secure);
 *     each request is its own site for cookies and a top-level navigation,
 *     so both kinds go with it and with its redirects.
 *   - ps5fetch_cancel after `ended` (PAUSED or FAILED) deletes the .part
 *     and .ps5dl and makes the state CANCELLED, without a second `ended`.
 *     ps5fetch_free on a running download stops it (PAUSED) first.
 */
#include "ps5fetch.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <glib.h>
#include <libsoup/soup.h>

#define MAX_WORKERS 16
#define MIN_CHUNK (1ull << 20)
#define DEFAULT_CHUNK (8ull << 20)
#define DEFAULT_STALL_S 45
#define DEFAULT_MARGIN (16ull << 20)
#define BUFFER_SIZE (512 * 1024)
#define FLUSH_US 250000
#define TICK_US 250000
#define SPEED_SAMPLES 9 /* a sample per tick: 2 s */
#define STATE_EVERY_US G_USEC_PER_SEC
#define RETRY_AFTER_MAX_S 60
#define STATE_GROUP "ps5dl"
#define MAX_TOTAL (16ll << 40) /* 16 TiB: a bigger size is a broken or hostile answer */

static const unsigned backoffSeconds[] = { 1, 2, 4, 8, 15 };

typedef struct {
    guint64 start, end; /* [start, end) */
} Span;

enum { CHUNK_FREE, CHUNK_TAKEN, CHUNK_DONE };
enum { WORKER_NONE, WORKER_RUNNING, WORKER_EXITED };
enum { ASK_NONE, ASK_PAUSE, ASK_CANCEL };

/* What a request led to: the worker's next step. */
typedef enum {
    NEXT_AGAIN, /* the next request at once */
    NEXT_RETRY, /* the chunk back to the queue, wait, then ask again */
    NEXT_LOWER, /* fewer connections: the chunk back, this worker ends */
    NEXT_END,   /* the download is over for this worker (done, stopped, failed) */
} Next;

typedef struct {
    gboolean probe;
    gboolean whole;     /* no Range: the whole file from byte 0 */
    guint64 start, end; /* with a Range: [start, end) */
    char* url;
    unsigned generation;
} Job;

typedef struct {
    PS5Fetch* fetch;
    int state;
    GThread* thread;
    GCancellable* request; /* the request in flight (a reference), else NULL */
    gint64 lastData;       /* monotonic µs: the request's start or its last data */
    gboolean stalled;      /* the engine cancelled the request: nothing for stall_s */
    gboolean receiving;
    gint64 chunk; /* the chunk this worker holds, -1: none */
    unsigned failures; /* requests that failed in a row */
    unsigned retryAfter;
} Worker;

struct PS5Fetch {
    char* originalUrl;
    char* userAgent;
    char* referer;
    char* path;
    char* partPath;
    char* statePath;
    GPtrArray* cookies; /* SoupCookie*, copied into each worker's jar */
    unsigned stallS;
    guint64 margin;
    ps5fetch_ended_fn ended;
    void* data;
    GThread* engine;

    GMutex lock;
    GCond wake; /* the engine, and workers waiting to retry */

    /* Everything below: under lock. */
    PS5FetchState state;
    int ask;
    gboolean failed;
    gboolean retryable;
    char error[256];
    char problem[200]; /* the last retryable problem, for the stall's message */
    char* url;         /* asked now: the final address once known */
    unsigned urlGeneration;
    char* finalUrl;
    gboolean probed;
    gboolean probeWhole;
    gboolean ranges;
    gboolean complete;
    gboolean restarted;
    gboolean checkSaved; /* a resume: the probe's total and validator must match */
    gboolean keepState;  /* the .ps5dl is worth writing (the .part exists) */
    gint64 total;
    char* etag;
    char* lastModified;
    char* contentType;
    int httpStatus;
    guint64 chunk;
    GArray* done; /* Span: sorted, disjoint, not touching */
    guint64 received;
    guint8* chunks;
    guint64 chunkCount;
    guint64 freeChunks;
    guint64 lowestFree;
    unsigned allowed;
    unsigned waiting; /* workers waiting to retry */
    Worker workers[MAX_WORKERS];
    int fd;
    guint64 arrived; /* from the network, for the speed */
    gint64 lastByte; /* monotonic µs: the last data on any connection */
    gboolean dirty;  /* the .ps5dl is behind */
    double speed;
    gint64 sampleTime[SPEED_SAMPLES];
    guint64 sampleBytes[SPEED_SAMPLES];
    unsigned samples;
};

/* ---- byte ranges ---- */

static Span* spans(GArray* done)
{
    return (Span*)(void*)done->data;
}

/* The first span that ends at or after `at` (that `at` may touch). */
static guint firstSpanFrom(GArray* done, guint64 at)
{
    guint low = 0, high = done->len;
    while (low < high) {
        guint middle = (low + high) / 2;
        if (spans(done)[middle].end < at)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

/* Adds [start, end); returns how many of those bytes are new. */
static guint64 addSpan(GArray* done, guint64 start, guint64 end)
{
    if (start >= end)
        return 0;
    guint i = firstSpanFrom(done, start);
    guint j = i;
    guint64 low = start, high = end, covered = 0;
    while (j < done->len && spans(done)[j].start <= end) {
        Span s = spans(done)[j];
        guint64 a = MAX(s.start, start), b = MIN(s.end, end);
        if (b > a)
            covered += b - a;
        low = MIN(low, s.start);
        high = MAX(high, s.end);
        j++;
    }
    Span merged = { low, high };
    if (j == i)
        g_array_insert_val(done, i, merged);
    else {
        spans(done)[i] = merged;
        if (j > i + 1)
            g_array_remove_range(done, i + 1, j - i - 1);
    }
    return end - start - covered;
}

/* The first bytes in [low, high) that aren't on disk: TRUE and [*start, *end). */
static gboolean firstGap(GArray* done, guint64 low, guint64 high, guint64* start, guint64* end)
{
    guint64 at = low;
    guint i = firstSpanFrom(done, at + 1);
    if (i < done->len && spans(done)[i].start <= at) {
        at = spans(done)[i].end;
        i++;
    }
    if (at >= high)
        return FALSE;
    *start = at;
    *end = i < done->len && spans(done)[i].start < high ? spans(done)[i].start : high;
    return TRUE;
}

static guint64 spanBytes(GArray* done)
{
    guint64 sum = 0;
    for (guint i = 0; i < done->len; i++)
        sum += spans(done)[i].end - spans(done)[i].start;
    return sum;
}

/* ---- small things ---- */

static const char* nonEmpty(const char* text)
{
    return text && *text ? text : NULL;
}

/* The validator is the ETag, else Last-Modified; none matches none. */
static gboolean sameValidator(const char* etagA, const char* modifiedA, const char* etagB, const char* modifiedB)
{
    if (nonEmpty(etagA) || nonEmpty(etagB))
        return nonEmpty(etagA) && nonEmpty(etagB) && !strcmp(etagA, etagB);
    return !g_strcmp0(nonEmpty(modifiedA), nonEmpty(modifiedB));
}

static void replaceString(char** slot, const char* value)
{
    if (g_strcmp0(*slot, value)) {
        g_free(*slot);
        *slot = g_strdup(value);
    }
}

static gboolean writeAt(int fd, const char* buffer, size_t length, guint64 offset)
{
    size_t done = 0;
    while (done < length) {
        ssize_t n = pwrite(fd, buffer + done, length - done, (off_t)(offset + done));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (!n)
                errno = EIO;
            return FALSE;
        }
        done += (size_t)n;
    }
    return TRUE;
}

static gboolean writeFile(const char* path, const char* text, size_t length)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0)
        return FALSE;
    gboolean ok = writeAt(fd, text, length, 0);
    if (close(fd))
        ok = FALSE;
    return ok;
}

/* Under lock. The first failure wins; stopping makes failures moot. */
static void fail(PS5Fetch* f, gboolean retryable, const char* format, ...) G_GNUC_PRINTF(3, 4);
static void fail(PS5Fetch* f, gboolean retryable, const char* format, ...)
{
    if (f->failed || f->ask)
        return;
    va_list args;
    va_start(args, format);
    vsnprintf(f->error, sizeof f->error, format, args);
    va_end(args);
    f->failed = TRUE;
    f->retryable = retryable;
    g_cond_broadcast(&f->wake);
}

/* Under lock: a problem a retry may get past. */
static void setProblem(PS5Fetch* f, const char* format, ...) G_GNUC_PRINTF(2, 3);
static void setProblem(PS5Fetch* f, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(f->problem, sizeof f->problem, format, args);
    va_end(args);
}

/* ---- the .ps5dl ---- */

typedef struct {
    gint64 total;
    char* etag;
    char* lastModified;
    char* finalUrl;
    guint64 chunk;
    gboolean ranges;
    GArray* done;
    guint64 listed; /* the bytes the .ps5dl lists, before they are cut */
    gboolean part;  /* the .part is there */
} Saved;

static void clearSaved(Saved* s)
{
    g_free(s->etag);
    g_free(s->lastModified);
    g_free(s->finalUrl);
    if (s->done)
        g_array_unref(s->done);
    memset(s, 0, sizeof *s);
}

/* Reads <part>.ps5dl; the ranges are cut to the .part's size and the total. */
static gboolean loadSaved(const char* statePath, const char* partPath, Saved* s)
{
    memset(s, 0, sizeof *s);
    GKeyFile* key = g_key_file_new();
    if (!g_key_file_load_from_file(key, statePath, G_KEY_FILE_NONE, NULL) || !g_key_file_has_group(key, STATE_GROUP)) {
        g_key_file_free(key);
        return FALSE;
    }
    s->done = g_array_new(FALSE, FALSE, sizeof(Span));
    s->total = g_key_file_get_int64(key, STATE_GROUP, "total", NULL);
    if (!g_key_file_has_key(key, STATE_GROUP, "total", NULL) || s->total < -1)
        s->total = -1;
    s->etag = g_key_file_get_string(key, STATE_GROUP, "etag", NULL);
    s->lastModified = g_key_file_get_string(key, STATE_GROUP, "last_modified", NULL);
    s->finalUrl = g_key_file_get_string(key, STATE_GROUP, "final_url", NULL);
    s->chunk = g_key_file_get_uint64(key, STATE_GROUP, "chunk", NULL);
    s->ranges = g_key_file_get_boolean(key, STATE_GROUP, "ranges", NULL);
    char* text = g_key_file_get_string(key, STATE_GROUP, "done", NULL);
    g_key_file_free(key);

    struct stat st;
    s->part = !stat(partPath, &st) && S_ISREG(st.st_mode);
    guint64 limit = s->part ? (guint64)st.st_size : 0;
    if (s->total >= 0 && (guint64)s->total < limit)
        limit = (guint64)s->total;
    for (const char* p = text ? text : ""; *p;) {
        char* end;
        guint64 a = g_ascii_strtoull(p, &end, 10);
        if (end == p || *end != '-')
            break;
        p = end + 1;
        guint64 b = g_ascii_strtoull(p, &end, 10);
        if (end == p)
            break;
        p = *end == ',' ? end + 1 : end;
        if (b > a)
            s->listed += b - a;
        addSpan(s->done, a, MIN(b, limit));
    }
    g_free(text);
    return TRUE;
}

/* Under lock: the .ps5dl's text. */
static char* stateText(PS5Fetch* f, gsize* length)
{
    GKeyFile* key = g_key_file_new();
    g_key_file_set_integer(key, STATE_GROUP, "version", 1);
    g_key_file_set_string(key, STATE_GROUP, "url", f->originalUrl);
    g_key_file_set_string(key, STATE_GROUP, "final_url", f->finalUrl ? f->finalUrl : "");
    g_key_file_set_int64(key, STATE_GROUP, "total", f->total);
    g_key_file_set_string(key, STATE_GROUP, "etag", f->etag ? f->etag : "");
    g_key_file_set_string(key, STATE_GROUP, "last_modified", f->lastModified ? f->lastModified : "");
    g_key_file_set_uint64(key, STATE_GROUP, "chunk", f->chunk);
    g_key_file_set_boolean(key, STATE_GROUP, "ranges", f->ranges);
    GString* done = g_string_sized_new(32 + f->done->len * 24);
    for (guint i = 0; i < f->done->len; i++)
        g_string_append_printf(done, "%s%" G_GUINT64_FORMAT "-%" G_GUINT64_FORMAT, i ? "," : "", spans(f->done)[i].start, spans(f->done)[i].end);
    g_key_file_set_string(key, STATE_GROUP, "done", done->str);
    g_string_free(done, TRUE);
    char* text = g_key_file_to_data(key, length, NULL);
    g_key_file_free(key);
    return text;
}

static void writeState(PS5Fetch* f, char* text, gsize length)
{
    char* temporary = g_strconcat(f->statePath, ".tmp", NULL);
    if (writeFile(temporary, text, length))
        rename(temporary, f->statePath);
    else
        unlink(temporary);
    g_free(temporary);
    g_free(text);
}

/* ---- cookies ---- */

static GPtrArray* parseCookies(const char* text)
{
    GPtrArray* cookies = g_ptr_array_new_with_free_func((GDestroyNotify)soup_cookie_free);
    if (!text)
        return cookies;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    char** lines = g_strsplit(text, "\n", -1);
    for (char** line = lines; *line; line++) {
        char* l = *line;
        gboolean httpOnly = FALSE;
        g_strchomp(l);
        if (g_str_has_prefix(l, "#HttpOnly_")) {
            l += strlen("#HttpOnly_");
            httpOnly = TRUE;
        } else if (*l == '#' || !*l)
            continue;
        char** field = g_strsplit(l, "\t", 7);
        if (g_strv_length(field) == 7 && *field[0] && *field[5]) {
            gboolean subdomains = !g_ascii_strcasecmp(field[1], "TRUE");
            const char* domain = field[0][0] == '.' ? field[0] + 1 : field[0];
            char* cookieDomain = subdomains ? g_strconcat(".", domain, NULL) : g_strdup(domain);
            gint64 expires = g_ascii_strtoll(field[4], NULL, 10);
            if (expires <= 0 || expires > now) {
                SoupCookie* cookie = soup_cookie_new(field[5], field[6], cookieDomain, *field[2] ? field[2] : "/", -1);
                if (expires > 0) {
                    GDateTime* when = g_date_time_new_from_unix_utc(expires);
                    if (when) {
                        soup_cookie_set_expires(cookie, when);
                        g_date_time_unref(when);
                    }
                }
                gboolean secure = !g_ascii_strcasecmp(field[3], "TRUE");
                soup_cookie_set_secure(cookie, secure);
                soup_cookie_set_http_only(cookie, httpOnly);
                /* libsoup drops a SameSite=None cookie that isn't Secure;
                 * the others stay Lax, sent with the requests here (each is
                 * its own site and a top-level navigation) */
                if (secure)
                    soup_cookie_set_same_site_policy(cookie, SOUP_SAME_SITE_POLICY_NONE);
                g_ptr_array_add(cookies, cookie);
            }
            g_free(cookieDomain);
        }
        g_strfreev(field);
    }
    g_strfreev(lines);
    return cookies;
}

static SoupSession* sessionWith(const char* userAgent, unsigned timeoutS, GPtrArray* cookies)
{
    SoupSession* session = soup_session_new_with_options("user-agent", userAgent ? userAgent : "", "timeout", timeoutS,
        "idle-timeout", 30u, NULL);
    soup_session_remove_feature_by_type(session, SOUP_TYPE_CONTENT_DECODER);
    SoupCookieJar* jar = soup_cookie_jar_new();
    for (guint i = 0; cookies && i < cookies->len; i++)
        soup_cookie_jar_add_cookie(jar, soup_cookie_copy(g_ptr_array_index(cookies, i)));
    soup_session_add_feature(session, SOUP_SESSION_FEATURE(jar));
    g_object_unref(jar);
    return session;
}

static SoupSession* newSession(PS5Fetch* f)
{
    return sessionWith(f->userAgent, f->stallS, f->cookies);
}

/* ---- chunks (under lock) ---- */

static void makeChunks(PS5Fetch* f)
{
    g_free(f->chunks);
    guint64 total = (guint64)f->total;
    f->chunkCount = (total + f->chunk - 1) / f->chunk;
    f->chunks = g_malloc0(f->chunkCount ? f->chunkCount : 1);
    f->freeChunks = 0;
    f->lowestFree = 0;
    for (guint64 k = 0; k < f->chunkCount; k++) {
        guint64 a, b;
        if (firstGap(f->done, k * f->chunk, MIN((k + 1) * f->chunk, total), &a, &b))
            f->freeChunks++;
        else
            f->chunks[k] = CHUNK_DONE;
    }
}

static gint64 takeChunk(PS5Fetch* f)
{
    for (guint64 k = f->lowestFree; k < f->chunkCount; k++) {
        if (f->chunks[k] == CHUNK_FREE) {
            f->chunks[k] = CHUNK_TAKEN;
            f->freeChunks--;
            f->lowestFree = k + 1;
            return (gint64)k;
        }
    }
    f->lowestFree = f->chunkCount;
    return -1;
}

static void releaseChunk(PS5Fetch* f, Worker* w)
{
    if (w->chunk >= 0 && f->chunks && (guint64)w->chunk < f->chunkCount && f->chunks[w->chunk] == CHUNK_TAKEN) {
        f->chunks[w->chunk] = CHUNK_FREE;
        f->freeChunks++;
        f->lowestFree = MIN(f->lowestFree, (guint64)w->chunk);
        g_cond_broadcast(&f->wake);
    }
    w->chunk = -1;
}

static gboolean allOnDisk(PS5Fetch* f)
{
    return f->total >= 0 && f->received >= (guint64)f->total && (f->probed || f->checkSaved);
}

/* Under lock: forget the bytes on disk (a resume that can't continue). */
static void startOver(PS5Fetch* f)
{
    g_array_set_size(f->done, 0);
    f->received = 0;
    g_clear_pointer(&f->chunks, g_free);
    f->chunkCount = f->freeChunks = f->lowestFree = 0;
    f->total = -1;
    g_clear_pointer(&f->etag, g_free);
    g_clear_pointer(&f->lastModified, g_free);
    f->restarted = TRUE;
    f->checkSaved = FALSE;
    f->dirty = TRUE;
    int r = f->fd >= 0 ? ftruncate(f->fd, 0) : truncate(f->partPath, 0);
    (void)r; /* no .part yet, or the writes will say */
}

/* Under lock: the probe's total is known; is there room for the rest? */
static gboolean enoughSpace(PS5Fetch* f)
{
    if (f->total < 0)
        return TRUE;
    struct stat st;
    guint64 have = !stat(f->partPath, &st) ? (guint64)st.st_size : 0;
    guint64 need = ((guint64)f->total > have ? (guint64)f->total - have : 0) + f->margin;
    char* folder = g_path_get_dirname(f->path);
    struct statvfs fs;
    int r = statvfs(folder, &fs);
    g_free(folder);
    if (r)
        return TRUE; /* the open or the writes will say */
    guint64 available = (guint64)fs.f_bavail * (guint64)fs.f_frsize;
    if (available >= need)
        return TRUE;
    char* needText = g_format_size(need);
    char* freeText = g_format_size(available);
    fail(f, FALSE, "Not enough free space on the drive: %s needed, %s free", needText, freeText);
    g_free(needText);
    g_free(freeText);
    return FALSE;
}

static gboolean openPart(PS5Fetch* f)
{
    if (f->fd >= 0)
        return TRUE;
    f->fd = open(f->partPath, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (f->fd < 0) {
        fail(f, FALSE, "The file can't be written: %s", g_strerror(errno));
        return FALSE;
    }
    f->keepState = TRUE;
    return TRUE;
}

/* ---- a request ---- */

/* Under lock: what to ask next, or FALSE when this worker has nothing to do. */
static gboolean nextJob(PS5Fetch* f, Worker* w, Job* job)
{
    memset(job, 0, sizeof *job);
    if (!f->probed) {
        job->probe = TRUE;
        job->whole = f->probeWhole;
        if (!job->whole) {
            guint64 high = f->total >= 0 ? (guint64)f->total : G_MAXUINT64;
            if (!firstGap(f->done, 0, high, &job->start, &job->end))
                return FALSE;
            job->end = MIN(job->end, (job->start / f->chunk + 1) * f->chunk);
        }
    } else if (f->ranges) {
        for (;;) {
            if (w->chunk < 0 && (w->chunk = takeChunk(f)) < 0)
                return FALSE;
            guint64 low = (guint64)w->chunk * f->chunk;
            if (firstGap(f->done, low, MIN(low + f->chunk, (guint64)f->total), &job->start, &job->end))
                break;
            f->chunks[w->chunk] = CHUNK_DONE;
            w->chunk = -1;
            g_cond_broadcast(&f->wake);
        }
    } else
        job->whole = TRUE;
    job->url = g_strdup(f->url);
    job->generation = f->urlGeneration;
    return TRUE;
}

static gboolean othersReceiving(PS5Fetch* f, Worker* w)
{
    for (int i = 0; i < MAX_WORKERS; i++) {
        if (&f->workers[i] != w && f->workers[i].state == WORKER_RUNNING && f->workers[i].receiving)
            return TRUE;
    }
    return FALSE;
}

static Next networkProblem(PS5Fetch* f, Worker* w, GError* error)
{
    g_mutex_lock(&f->lock);
    Next next = NEXT_RETRY;
    if (f->ask || f->failed || f->complete)
        next = NEXT_END;
    else if (w->stalled || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
        setProblem(f, "a connection stalled");
    else
        setProblem(f, "%s", error ? error->message : "the connection failed");
    g_mutex_unlock(&f->lock);
    g_clear_error(&error);
    return next;
}

/* Writes what the buffer holds (bytes before skipBelow are already there). */
static gboolean flush(PS5Fetch* f, Worker* w, const char* buffer, size_t fill, guint64 at, guint64 skipBelow)
{
    size_t skip = at >= skipBelow ? 0 : (size_t)MIN(skipBelow - at, (guint64)fill);
    if (skip < fill && !writeAt(f->fd, buffer + skip, fill - skip, at + skip)) {
        int e = errno;
        g_mutex_lock(&f->lock);
        if (e == ENOSPC)
            fail(f, FALSE, "The drive is full");
        else
            fail(f, FALSE, "The file can't be written: %s", g_strerror(e));
        g_mutex_unlock(&f->lock);
        return FALSE;
    }
    gint64 now = g_get_monotonic_time();
    g_mutex_lock(&f->lock);
    if (skip < fill)
        f->received += addSpan(f->done, at + skip, at + fill);
    f->arrived += fill;
    f->lastByte = now;
    w->lastData = now;
    w->failures = 0;
    f->dirty = TRUE;
    g_mutex_unlock(&f->lock);
    return TRUE;
}

/* The body, [start, limit) of the file (limit G_MAXUINT64: to its end). */
static Next receive(PS5Fetch* f, Worker* w, GInputStream* stream, GCancellable* cancel, char* buffer, gboolean whole, guint64 start,
    guint64 limit, guint64 skipBelow)
{
    guint64 at = start;
    size_t fill = 0;
    gint64 flushed = g_get_monotonic_time();
    for (;;) {
        guint64 left = limit - (at + fill);
        if (!left)
            break;
        size_t want = BUFFER_SIZE - fill;
        if (left < want)
            want = (size_t)left;
        GError* error = NULL;
        gssize n = g_input_stream_read(stream, buffer + fill, want, cancel, &error);
        if (n > 0)
            fill += (size_t)n;
        gint64 now = g_get_monotonic_time();
        if (fill && (n <= 0 || fill == BUFFER_SIZE || now - flushed >= FLUSH_US || at + fill == limit)) {
            if (!flush(f, w, buffer, fill, at, skipBelow)) {
                g_clear_error(&error);
                return NEXT_END;
            }
            at += fill;
            fill = 0;
            flushed = now;
        }
        if (n < 0)
            return networkProblem(f, w, error);
        if (!n) {
            g_mutex_lock(&f->lock);
            Next next = NEXT_RETRY;
            if (whole && f->total < 0) {
                f->total = (gint64)at; /* no length: the end of the body is the end */
                f->complete = TRUE;
                f->dirty = TRUE;
                g_cond_broadcast(&f->wake);
                next = NEXT_END;
            } else
                setProblem(f, "the connection closed early");
            g_mutex_unlock(&f->lock);
            return next;
        }
    }
    if (!whole)
        return NEXT_AGAIN;
    g_mutex_lock(&f->lock);
    f->complete = TRUE;
    g_cond_broadcast(&f->wake);
    g_mutex_unlock(&f->lock);
    return NEXT_END;
}

/* Under lock: the answer's status and headers. NEXT_AGAIN with *body set:
 * read it, [*start, *limit), not writing what is before *skipBelow. */
static Next checkAnswer(PS5Fetch* f, Worker* w, SoupMessage* message, const Job* job, gboolean* body, guint64* start, guint64* limit,
    guint64* skipBelow)
{
    guint status = soup_message_get_status(message);
    const char* reason = soup_message_get_reason_phrase(message);
    SoupMessageHeaders* headers = soup_message_get_response_headers(message);
    *body = FALSE;
    f->httpStatus = (int)status;
    if (f->ask || f->failed || f->complete)
        return NEXT_END;

    if (status == 401 || status == 403 || status == 404 || status == 410) {
        if (job->generation != f->urlGeneration)
            return NEXT_AGAIN; /* another worker has gone back already */
        if (strcmp(job->url, f->originalUrl)) {
            replaceString(&f->url, f->originalUrl);
            f->urlGeneration++;
            return NEXT_AGAIN;
        }
        fail(f, FALSE, "The server refused: %u %s", status, reason ? reason : "");
        return NEXT_END;
    }
    if (status == 416 && job->probe && !job->whole) {
        f->probeWhole = TRUE;
        return NEXT_AGAIN;
    }
    if (status == 408 || status == 429 || (status >= 500 && status < 600)) {
        setProblem(f, "the server answered %u %s", status, reason ? reason : "");
        const char* after = soup_message_headers_get_one(headers, "Retry-After");
        w->retryAfter = after && g_ascii_isdigit(*after) ? (unsigned)MIN(g_ascii_strtoull(after, NULL, 10), RETRY_AFTER_MAX_S) : 0;
        if ((status == 429 || status == 503) && f->probed && f->ranges && f->allowed > 1 && othersReceiving(f, w)) {
            f->allowed--;
            return NEXT_LOWER;
        }
        return NEXT_RETRY;
    }
    if ((status != 200 && status != 206) || (status == 206 && job->whole)) {
        fail(f, FALSE, "The server refused: %u %s", status, reason ? reason : "");
        return NEXT_END;
    }

    goffset rangeStart = 0, rangeEnd = -1, rangeTotal = -1;
    if (status == 206 && !soup_message_headers_get_content_range(headers, &rangeStart, &rangeEnd, &rangeTotal))
        rangeTotal = -2;
    if (status == 206 && (rangeTotal < 0 || rangeEnd < rangeStart)) {
        if (job->probe) {
            f->probeWhole = TRUE; /* no total: one connection, the whole file */
            return NEXT_AGAIN;
        }
        fail(f, FALSE, "The server's answer can't be used (Content-Range: %s)",
            soup_message_headers_get_one(headers, "Content-Range") ? soup_message_headers_get_one(headers, "Content-Range") : "none");
        return NEXT_END;
    }
    if (status == 206 && (guint64)rangeStart != job->start) {
        fail(f, FALSE, "The server sent the wrong part of the file (bytes from %lld, asked from %" G_GUINT64_FORMAT ")", (long long)rangeStart,
            job->start);
        return NEXT_END;
    }
    if (status == 200 && !job->whole && !job->probe) {
        setProblem(f, "the server sent the whole file instead of a part");
        return NEXT_RETRY;
    }
    gint64 length = soup_message_headers_get_encoding(headers) == SOUP_ENCODING_CONTENT_LENGTH
        ? soup_message_headers_get_content_length(headers)
        : -1;
    gint64 total = status == 206 ? rangeTotal : length;
    if (total > MAX_TOTAL) {
        /* a hostile or broken answer: a list of chunks that size would
         * take the engine's lock (and memory) for good */
        fail(f, FALSE, "The server gave an impossible size (%" G_GINT64_FORMAT " bytes)", total);
        return NEXT_END;
    }
    const char* etag = soup_message_headers_get_one(headers, "ETag");
    const char* modified = soup_message_headers_get_one(headers, "Last-Modified");
    gboolean same = sameValidator(f->etag, f->lastModified, etag, modified) && total == f->total;

    if (job->probe) {
        if (f->checkSaved) {
            gboolean over = !same || (status == 200 && !nonEmpty(etag) && !nonEmpty(modified));
            f->checkSaved = FALSE;
            if (over) {
                startOver(f);
                if (status == 206 && job->start)
                    return NEXT_AGAIN; /* ask from byte 0 */
            }
        }
        f->total = total;
        replaceString(&f->etag, etag);
        replaceString(&f->lastModified, modified);
        f->ranges = status == 206;
        if (!enoughSpace(f))
            return NEXT_END;
        if (f->ranges)
            makeChunks(f);
        if (!openPart(f))
            return NEXT_END;
        if (f->ranges) {
            w->chunk = (gint64)(job->start / f->chunk);
            if (f->chunks[w->chunk] == CHUNK_FREE) {
                f->chunks[w->chunk] = CHUNK_TAKEN;
                f->freeChunks--;
            }
        }
        f->probed = TRUE;
        f->dirty = TRUE;
        g_cond_broadcast(&f->wake);
    } else if (!same) {
        if (!job->whole) {
            fail(f, TRUE, "The file changed on the server");
            return NEXT_END;
        }
        /* one stream from byte 0 again: all of it is written again */
        g_array_set_size(f->done, 0);
        f->received = 0;
        f->total = total;
        replaceString(&f->etag, etag);
        replaceString(&f->lastModified, modified);
        f->restarted = TRUE;
        f->dirty = TRUE;
    }

    char* final = g_uri_to_string(soup_message_get_uri(message));
    replaceString(&f->finalUrl, final);
    if (job->generation == f->urlGeneration && strcmp(final, f->url)) {
        replaceString(&f->url, final); /* redirected: ask there from now on */
        f->urlGeneration++;
    }
    g_free(final);
    const char* type = soup_message_headers_get_content_type(headers, NULL);
    if (type)
        replaceString(&f->contentType, type);

    *body = TRUE;
    if (status == 206) {
        *start = job->start;
        *limit = MIN(job->end, (guint64)rangeEnd + 1);
        *skipBelow = 0;
    } else {
        *start = 0;
        *limit = f->total >= 0 ? (guint64)f->total : G_MAXUINT64;
        *skipBelow = f->done->len && !spans(f->done)[0].start ? spans(f->done)[0].end : 0;
    }
    w->receiving = TRUE;
    return NEXT_AGAIN;
}

static Next request(PS5Fetch* f, Worker* w, SoupSession* session, char* buffer, const Job* job)
{
    SoupMessage* message = soup_message_new("GET", job->url);
    if (!message) {
        g_mutex_lock(&f->lock);
        fail(f, FALSE, "The address can't be used: %.200s", job->url);
        g_mutex_unlock(&f->lock);
        return NEXT_END;
    }
    SoupMessageHeaders* headers = soup_message_get_request_headers(message);
    soup_message_headers_replace(headers, "Accept-Encoding", "identity");
    if (f->referer)
        soup_message_headers_replace(headers, "Referer", f->referer);
    if (!job->whole)
        soup_message_headers_set_range(headers, (goffset)job->start, (goffset)job->end - 1);
    GUri* uri = soup_message_get_uri(message);
    soup_message_set_first_party(message, uri);
    soup_message_set_site_for_cookies(message, uri);
    soup_message_set_is_top_level_navigation(message, TRUE);

    GCancellable* cancel = g_cancellable_new();
    g_mutex_lock(&f->lock);
    gboolean stopping = f->ask || f->failed;
    w->request = g_object_ref(cancel);
    w->stalled = FALSE;
    w->retryAfter = 0;
    w->lastData = g_get_monotonic_time();
    g_mutex_unlock(&f->lock);
    if (stopping)
        g_cancellable_cancel(cancel);

    GError* error = NULL;
    GInputStream* stream = soup_session_send(session, message, cancel, &error);
    Next next;
    if (!stream)
        next = networkProblem(f, w, error);
    else {
        gboolean body;
        guint64 start = 0, limit = 0, skipBelow = 0;
        g_mutex_lock(&f->lock);
        next = checkAnswer(f, w, message, job, &body, &start, &limit, &skipBelow);
        g_mutex_unlock(&f->lock);
        if (body)
            next = receive(f, w, stream, cancel, buffer, job->whole || soup_message_get_status(message) == 200, start, limit, skipBelow);
        g_input_stream_close(stream, NULL, NULL);
        g_object_unref(stream);
    }
    g_object_unref(message);
    g_mutex_lock(&f->lock);
    g_clear_object(&w->request);
    w->receiving = FALSE;
    g_mutex_unlock(&f->lock);
    g_object_unref(cancel);
    return next;
}

/* Under lock, after a failed request: FALSE when the worker should end. */
static gboolean waitToRetry(PS5Fetch* f, Worker* w)
{
    gint64 now = g_get_monotonic_time();
    w->failures++;
    if (w->failures >= 2 && now - f->lastByte >= (gint64)f->stallS * G_USEC_PER_SEC) {
        fail(f, TRUE, "Nothing has arrived for %u s (%s)", f->stallS, f->problem);
        return FALSE;
    }
    unsigned seconds = backoffSeconds[MIN(w->failures, G_N_ELEMENTS(backoffSeconds)) - 1];
    seconds = MAX(seconds, w->retryAfter);
    gint64 until = now + (gint64)seconds * G_USEC_PER_SEC;
    f->waiting++;
    while (!f->ask && !f->failed && !f->complete && !(f->probed && f->ranges && allOnDisk(f))) {
        if (!g_cond_wait_until(&f->wake, &f->lock, until))
            break;
    }
    f->waiting--;
    return !f->ask && !f->failed && !f->complete;
}

static gpointer workerMain(gpointer data)
{
    Worker* w = data;
    PS5Fetch* f = w->fetch;
    SoupSession* session = newSession(f);
    char* buffer = g_malloc(BUFFER_SIZE);
    g_mutex_lock(&f->lock);
    for (;;) {
        if (f->ask || f->failed || f->complete)
            break;
        Job job;
        if (!nextJob(f, w, &job))
            break;
        g_mutex_unlock(&f->lock);
        Next next = request(f, w, session, buffer, &job);
        g_free(job.url);
        g_mutex_lock(&f->lock);
        if (next == NEXT_END)
            break;
        if (next == NEXT_LOWER)
            break;
        if (next == NEXT_RETRY) {
            releaseChunk(f, w);
            if (!waitToRetry(f, w))
                break;
        }
    }
    releaseChunk(f, w);
    g_mutex_unlock(&f->lock);
    g_object_unref(session);
    g_free(buffer);
    g_mutex_lock(&f->lock);
    w->state = WORKER_EXITED;
    g_cond_broadcast(&f->wake);
    g_mutex_unlock(&f->lock);
    return NULL;
}

/* ---- the engine ---- */

/* Under lock. */
static gboolean startWorker(PS5Fetch* f)
{
    for (int i = 0; i < MAX_WORKERS; i++) {
        Worker* w = &f->workers[i];
        if (w->state != WORKER_NONE)
            continue;
        w->state = WORKER_RUNNING;
        w->chunk = -1;
        w->failures = 0;
        w->retryAfter = 0;
        w->receiving = FALSE;
        char name[16];
        snprintf(name, sizeof name, "ps5fetch-%d", i);
        w->thread = g_thread_try_new(name, workerMain, w, NULL);
        if (w->thread)
            return TRUE;
        w->state = WORKER_NONE;
        return FALSE;
    }
    return FALSE;
}

/* Under lock: one sample a tick; the speed over the last ~2 s. */
static void sampleSpeed(PS5Fetch* f, gint64 now)
{
    if (f->samples == SPEED_SAMPLES) {
        memmove(f->sampleTime, f->sampleTime + 1, sizeof f->sampleTime[0] * (SPEED_SAMPLES - 1));
        memmove(f->sampleBytes, f->sampleBytes + 1, sizeof f->sampleBytes[0] * (SPEED_SAMPLES - 1));
        f->samples--;
    }
    f->sampleTime[f->samples] = now;
    f->sampleBytes[f->samples] = f->arrived;
    f->samples++;
    gint64 span = now - f->sampleTime[0];
    f->speed = span > 0 ? (double)(f->arrived - f->sampleBytes[0]) * G_USEC_PER_SEC / (double)span : 0;
}

static void cancelAll(GPtrArray* cancels)
{
    for (guint i = 0; i < cancels->len; i++)
        g_cancellable_cancel(g_ptr_array_index(cancels, i));
    g_ptr_array_set_size(cancels, 0);
}

/* Under lock, all workers gone: closes, renames or deletes the files. */
static PS5FetchState finish(PS5Fetch* f)
{
    gboolean complete = f->complete || allOnDisk(f);
    if (f->ask == ASK_CANCEL) {
        if (f->fd >= 0)
            close(f->fd);
        f->fd = -1;
        unlink(f->partPath);
        unlink(f->statePath);
        return PS5FETCH_CANCELLED;
    }
    if (complete && !f->failed) {
        int r = 0;
        if (f->fd >= 0) {
            struct stat st;
            if (!fstat(f->fd, &st) && (guint64)st.st_size > (guint64)f->total)
                r = ftruncate(f->fd, (off_t)f->total);
            if (close(f->fd))
                r = -1;
            f->fd = -1;
        } else {
            struct stat st;
            if (stat(f->partPath, &st))
                r = writeFile(f->partPath, "", 0) ? 0 : -1; /* 0 bytes and no .part yet */
            else if ((guint64)st.st_size > (guint64)f->total)
                r = truncate(f->partPath, (off_t)f->total);
        }
        if (!r && !rename(f->partPath, f->path)) {
            unlink(f->statePath);
            return PS5FETCH_DONE;
        }
        f->failed = TRUE;
        f->retryable = FALSE;
        snprintf(f->error, sizeof f->error, "The finished file can't be saved: %s", g_strerror(errno));
    }
    if (f->keepState) {
        gsize length;
        char* text = stateText(f, &length);
        writeState(f, text, length);
    }
    if (f->fd >= 0) {
        fsync(f->fd);
        close(f->fd);
        f->fd = -1;
    }
    return f->failed ? PS5FETCH_FAILED : PS5FETCH_PAUSED;
}

static gpointer engineMain(gpointer data)
{
    PS5Fetch* f = data;
    GPtrArray* cancels = g_ptr_array_new_with_free_func(g_object_unref);
    gboolean cancelled = FALSE;
    g_mutex_lock(&f->lock);
    if (!allOnDisk(f) && !startWorker(f))
        fail(f, TRUE, "The download couldn't start a connection");
    gint64 now = g_get_monotonic_time();
    gint64 nextTick = now, nextSave = now + STATE_EVERY_US;
    for (;;) {
        for (int i = 0; i < MAX_WORKERS; i++) {
            Worker* w = &f->workers[i];
            if (w->state != WORKER_EXITED)
                continue;
            GThread* thread = w->thread;
            w->thread = NULL;
            w->state = WORKER_NONE;
            g_mutex_unlock(&f->lock);
            g_thread_join(thread);
            g_mutex_lock(&f->lock);
        }
        unsigned alive = 0;
        for (int i = 0; i < MAX_WORKERS; i++)
            alive += f->workers[i].state != WORKER_NONE;
        gboolean ending = f->ask || f->failed || f->complete || allOnDisk(f);
        if (ending) {
            if (!cancelled) {
                cancelled = TRUE;
                for (int i = 0; i < MAX_WORKERS; i++) {
                    if (f->workers[i].request)
                        g_ptr_array_add(cancels, g_object_ref(f->workers[i].request));
                }
                g_cond_broadcast(&f->wake);
                g_mutex_unlock(&f->lock);
                cancelAll(cancels);
                g_mutex_lock(&f->lock);
            }
            if (!alive)
                break;
        } else if (f->probed && f->ranges) {
            /* not for the chunks that workers waiting to retry will take
             * back: a fresh worker would start their count of failures over */
            guint64 spare = f->freeChunks > f->waiting ? f->freeChunks - f->waiting : 0;
            guint64 more = MIN((guint64)(f->allowed > alive ? f->allowed - alive : 0), spare);
            for (; more; more--) {
                if (!startWorker(f))
                    break;
                alive++;
            }
            if (!alive)
                fail(f, TRUE, "The download couldn't start a connection");
        }

        now = g_get_monotonic_time();
        if (now >= nextTick) {
            nextTick = now + TICK_US;
            sampleSpeed(f, now);
            gint64 stallUs = (gint64)f->stallS * G_USEC_PER_SEC;
            for (int i = 0; i < MAX_WORKERS; i++) {
                Worker* w = &f->workers[i];
                if (w->state == WORKER_RUNNING && w->request && !w->stalled && now - w->lastData > stallUs) {
                    w->stalled = TRUE;
                    g_ptr_array_add(cancels, g_object_ref(w->request));
                }
            }
            if (cancels->len) {
                g_mutex_unlock(&f->lock);
                cancelAll(cancels);
                g_mutex_lock(&f->lock);
            }
        }
        if (f->dirty && f->keepState && now >= nextSave && !ending) {
            gsize length;
            char* text = stateText(f, &length);
            f->dirty = FALSE;
            nextSave = now + STATE_EVERY_US;
            g_mutex_unlock(&f->lock);
            writeState(f, text, length);
            g_mutex_lock(&f->lock);
        }
        g_cond_wait_until(&f->wake, &f->lock, nextTick);
    }
    PS5FetchState state = finish(f);
    f->state = state;
    f->speed = 0;
    g_mutex_unlock(&f->lock);
    g_ptr_array_unref(cancels);
    f->ended(f, state, f->data);
    return NULL;
}

/* ---- the API ---- */

PS5Fetch* ps5fetch_start(const PS5FetchOptions* options, ps5fetch_ended_fn ended, void* data)
{
    if (!options || !options->url || !options->path || !ended)
        return NULL;
    PS5Fetch* f = g_new0(PS5Fetch, 1);
    f->originalUrl = g_strdup(options->original_url ? options->original_url : options->url);
    f->userAgent = g_strdup(options->user_agent);
    f->referer = g_strdup(options->referer);
    f->path = g_strdup(options->path);
    f->partPath = g_strconcat(options->path, ".part", NULL);
    f->statePath = g_strconcat(options->path, ".part.ps5dl", NULL);
    f->cookies = parseCookies(options->cookies);
    f->stallS = options->stall_s ? options->stall_s : DEFAULT_STALL_S;
    f->margin = options->free_margin ? options->free_margin : DEFAULT_MARGIN;
    f->ended = ended;
    f->data = data;
    g_mutex_init(&f->lock);
    g_cond_init(&f->wake);
    f->state = PS5FETCH_RUNNING;
    f->url = g_strdup(options->url);
    f->finalUrl = g_strdup(options->url);
    f->total = -1;
    f->fd = -1;
    f->chunk = options->chunk ? MAX(options->chunk, MIN_CHUNK) : DEFAULT_CHUNK;
    f->done = g_array_new(FALSE, FALSE, sizeof(Span));
    f->allowed = CLAMP(options->connections, 1, MAX_WORKERS);
    for (int i = 0; i < MAX_WORKERS; i++) {
        f->workers[i].fetch = f;
        f->workers[i].chunk = -1;
    }
    f->lastByte = g_get_monotonic_time();

    Saved saved;
    if (loadSaved(f->statePath, f->partPath, &saved)) {
        f->keepState = TRUE;
        guint64 bytes = spanBytes(saved.done);
        if (bytes) {
            g_array_unref(f->done);
            f->done = g_array_ref(saved.done);
            f->received = bytes;
            f->total = saved.total;
            f->etag = g_strdup(nonEmpty(saved.etag));
            f->lastModified = g_strdup(nonEmpty(saved.lastModified));
            if (saved.chunk >= MIN_CHUNK)
                f->chunk = saved.chunk;
            f->checkSaved = TRUE;
        } else if (saved.listed)
            f->restarted = TRUE; /* bytes were listed but aren't there */
        clearSaved(&saved);
    }

    f->engine = g_thread_try_new("ps5fetch", engineMain, f, NULL);
    if (!f->engine) {
        f->state = PS5FETCH_FAILED;
        ps5fetch_free(f);
        return NULL;
    }
    return f;
}

void ps5fetch_status(PS5Fetch* f, PS5FetchStatus* out)
{
    memset(out, 0, sizeof *out);
    if (!f)
        return;
    g_mutex_lock(&f->lock);
    out->state = f->state;
    out->total = f->total;
    out->received = f->received;
    out->speed = f->speed;
    for (int i = 0; i < MAX_WORKERS; i++)
        out->connections += f->workers[i].state == WORKER_RUNNING && f->workers[i].receiving;
    out->ranges = f->probed && f->ranges;
    out->restarted = f->restarted;
    out->retryable = f->state == PS5FETCH_FAILED && f->retryable;
    out->http_status = f->httpStatus;
    g_strlcpy(out->final_url, f->finalUrl ? f->finalUrl : "", sizeof out->final_url);
    g_strlcpy(out->etag, f->etag ? f->etag : "", sizeof out->etag);
    g_strlcpy(out->last_modified, f->lastModified ? f->lastModified : "", sizeof out->last_modified);
    g_strlcpy(out->content_type, f->contentType ? f->contentType : "", sizeof out->content_type);
    if (f->state == PS5FETCH_FAILED)
        g_strlcpy(out->error, f->error, sizeof out->error);
    g_mutex_unlock(&f->lock);
}

static void askToStop(PS5Fetch* f, int ask)
{
    if (!f)
        return;
    g_mutex_lock(&f->lock);
    if (f->state == PS5FETCH_RUNNING) {
        if (ask > f->ask)
            f->ask = ask;
        g_cond_broadcast(&f->wake);
    } else if (ask == ASK_CANCEL && (f->state == PS5FETCH_PAUSED || f->state == PS5FETCH_FAILED)) {
        unlink(f->partPath);
        unlink(f->statePath);
        f->state = PS5FETCH_CANCELLED;
    }
    g_mutex_unlock(&f->lock);
}

void ps5fetch_stop(PS5Fetch* f)
{
    askToStop(f, ASK_PAUSE);
}

void ps5fetch_cancel(PS5Fetch* f)
{
    askToStop(f, ASK_CANCEL);
}

void ps5fetch_free(PS5Fetch* f)
{
    if (!f)
        return;
    if (f->engine) {
        askToStop(f, ASK_PAUSE);
        g_thread_join(f->engine);
    }
    g_free(f->originalUrl);
    g_free(f->userAgent);
    g_free(f->referer);
    g_free(f->path);
    g_free(f->partPath);
    g_free(f->statePath);
    g_ptr_array_unref(f->cookies);
    g_free(f->url);
    g_free(f->finalUrl);
    g_free(f->etag);
    g_free(f->lastModified);
    g_free(f->contentType);
    g_array_unref(f->done);
    g_free(f->chunks);
    g_mutex_clear(&f->lock);
    g_cond_clear(&f->wake);
    g_free(f);
}

bool ps5fetch_saved(const char* path, uint64_t* received, int64_t* total)
{
    if (!path)
        return false;
    char* partPath = g_strconcat(path, ".part", NULL);
    char* statePath = g_strconcat(path, ".part.ps5dl", NULL);
    Saved saved;
    gboolean found = loadSaved(statePath, partPath, &saved);
    if (found) {
        if (received)
            *received = spanBytes(saved.done);
        if (total)
            *total = saved.total;
        clearSaved(&saved);
    }
    g_free(partPath);
    g_free(statePath);
    return found;
}

/* ---- pieces of a file on demand (ps5fetch_ranges_*) ---- */

#define RANGES_BLOCK (64u * 1024)
#define RANGES_AHEAD_MIN 4u  /* blocks asked for at least (256 KiB) */
#define RANGES_AHEAD_MAX 16u /* reading on from the last request: up to 1 MiB */

struct PS5FetchRanges {
    char* url;
    char* referer;
    SoupSession* session;
    uint64_t size;
    gint64 deadline; /* monotonic us */
    uint64_t maxBytes;
    GHashTable* blocks; /* block number -> RANGES_BLOCK bytes (the last one shorter) */
    uint64_t lastEnd;   /* the byte after the last request's */
    unsigned ahead;
    unsigned requests;
    uint64_t bytes;
    char error[256];
};

PS5FetchRanges* ps5fetch_ranges_open(const PS5FetchRangesOptions* options)
{
    if (!options || !options->url || !options->size)
        return NULL;
    PS5FetchRanges* r = g_new0(PS5FetchRanges, 1);
    r->url = g_strdup(options->url);
    r->referer = g_strdup(options->referer);
    unsigned timeout = options->timeout_ms ? options->timeout_ms : 10000;
    r->deadline = g_get_monotonic_time() + (gint64)timeout * 1000;
    r->maxBytes = options->max_bytes ? options->max_bytes : 16u << 20;
    r->size = options->size;
    GPtrArray* cookies = parseCookies(options->cookies);
    r->session = sessionWith(options->user_agent, MAX(1u, (timeout + 999) / 1000), cookies);
    g_ptr_array_unref(cookies);
    r->blocks = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
    r->ahead = RANGES_AHEAD_MIN;
    return r;
}

static void rangesFail(PS5FetchRanges* r, const char* format, ...) G_GNUC_PRINTF(2, 3);
static void rangesFail(PS5FetchRanges* r, const char* format, ...)
{
    if (*r->error)
        return;
    va_list args;
    va_start(args, format);
    vsnprintf(r->error, sizeof r->error, format, args);
    va_end(args);
}

/* Cancels a request when the deadline passes (a server that sends a byte
 * now and then would keep libsoup's per-read timeout from firing). */
typedef struct {
    GMutex lock;
    GCond cond;
    gboolean done;
    gint64 deadline;
    GCancellable* cancel;
} Watchdog;

static gpointer watchdogMain(gpointer data)
{
    Watchdog* w = data;
    g_mutex_lock(&w->lock);
    while (!w->done) {
        if (!g_cond_wait_until(&w->cond, &w->lock, w->deadline)) {
            if (!w->done)
                g_cancellable_cancel(w->cancel);
            break;
        }
    }
    g_mutex_unlock(&w->lock);
    return NULL;
}

static uint64_t blockLength(PS5FetchRanges* r, uint64_t block)
{
    uint64_t start = block * RANGES_BLOCK;
    return MIN((uint64_t)RANGES_BLOCK, r->size - start);
}

/* Asks for blocks first.. (up to r->ahead, stopping at one already kept). */
static gboolean rangesFetch(PS5FetchRanges* r, uint64_t first)
{
    if (*r->error)
        return FALSE;
    uint64_t last = first;
    uint64_t blockCount = (r->size + RANGES_BLOCK - 1) / RANGES_BLOCK;
    unsigned ahead = first * RANGES_BLOCK == r->lastEnd ? MIN(r->ahead * 2, RANGES_AHEAD_MAX) : RANGES_AHEAD_MIN;
    while (last + 1 < blockCount && last + 1 < first + ahead) {
        gint64 key = (gint64)(last + 1);
        if (g_hash_table_contains(r->blocks, &key))
            break;
        last++;
    }
    uint64_t start = first * RANGES_BLOCK;
    uint64_t end = last * RANGES_BLOCK + blockLength(r, last); /* exclusive */
    if (r->bytes + (end - start) > r->maxBytes) {
        rangesFail(r, "More than %llu bytes would be needed", (unsigned long long)r->maxBytes);
        return FALSE;
    }
    gint64 left = r->deadline - g_get_monotonic_time();
    if (left <= 0) {
        rangesFail(r, "It took too long");
        return FALSE;
    }
    soup_session_set_timeout(r->session, (guint)MAX(1, (left + 999999) / 1000000));
    SoupMessage* message = soup_message_new("GET", r->url);
    if (!message) {
        rangesFail(r, "Bad address");
        return FALSE;
    }
    SoupMessageHeaders* headers = soup_message_get_request_headers(message);
    soup_message_headers_replace(headers, "Accept-Encoding", "identity");
    if (r->referer)
        soup_message_headers_replace(headers, "Referer", r->referer);
    soup_message_headers_set_range(headers, (goffset)start, (goffset)end - 1);
    GUri* uri = soup_message_get_uri(message);
    soup_message_set_first_party(message, uri);
    soup_message_set_site_for_cookies(message, uri);
    soup_message_set_is_top_level_navigation(message, TRUE);
    r->requests++;
    Watchdog watch = { .deadline = r->deadline, .cancel = g_cancellable_new() };
    g_mutex_init(&watch.lock);
    g_cond_init(&watch.cond);
    GThread* watcher = g_thread_new("dl-peek-watch", watchdogMain, &watch);
    GError* error = NULL;
    GInputStream* stream = soup_session_send(r->session, message, watch.cancel, &error);
    gboolean ok = FALSE;
    if (!stream) {
        rangesFail(r, "%s", g_cancellable_is_cancelled(watch.cancel) ? "It took too long" : error ? error->message : "network error");
        g_clear_error(&error);
        g_object_unref(message);
        goto finish;
    }
    guint status = soup_message_get_status(message);
    goffset rangeStart = 0, rangeEnd = 0, rangeTotal = 0;
    SoupMessageHeaders* answer = soup_message_get_response_headers(message);
    if (status != 206)
        rangesFail(r, "The server answered %u, not a range", status);
    else if (!soup_message_headers_get_content_range(answer, &rangeStart, &rangeEnd, &rangeTotal) || (uint64_t)rangeStart != start
        || (rangeTotal > 0 && (uint64_t)rangeTotal != r->size) || (uint64_t)rangeEnd + 1 < end)
        rangesFail(r, "The server sent another range than the one asked");
    else {
        guchar* data = g_malloc(end - start);
        gsize got = 0;
        if (!g_input_stream_read_all(stream, data, end - start, &got, watch.cancel, &error) || got != end - start)
            rangesFail(r, "%s", g_cancellable_is_cancelled(watch.cancel) ? "It took too long" : error ? error->message : "the answer was cut short");
        else {
            r->bytes += end - start;
            for (uint64_t b = first; b <= last; b++) {
                gint64* key = g_new(gint64, 1);
                *key = (gint64)b;
                uint64_t length = blockLength(r, b);
                g_hash_table_replace(r->blocks, key, g_memdup2(data + (b - first) * RANGES_BLOCK, length));
            }
            r->ahead = ahead;
            r->lastEnd = end;
            ok = TRUE;
        }
        g_clear_error(&error);
        g_free(data);
    }
    /* later requests go where this one ended (a signed address stays good
     * for the few seconds this takes) */
    if (ok) {
        char* final = g_uri_to_string(soup_message_get_uri(message));
        if (final) {
            g_free(r->url);
            r->url = final;
        }
    }
    g_input_stream_close(stream, NULL, NULL);
    g_object_unref(stream);
    g_object_unref(message);
finish:
    g_mutex_lock(&watch.lock);
    watch.done = TRUE;
    g_cond_signal(&watch.cond);
    g_mutex_unlock(&watch.lock);
    g_thread_join(watcher);
    g_mutex_clear(&watch.lock);
    g_cond_clear(&watch.cond);
    g_object_unref(watch.cancel);
    return ok;
}

int64_t ps5fetch_ranges_read(PS5FetchRanges* r, uint64_t offset, void* buffer, size_t length)
{
    if (!r || *r->error)
        return -1;
    if (offset >= r->size)
        return 0;
    length = (size_t)MIN((uint64_t)length, r->size - offset);
    size_t done = 0;
    while (done < length) {
        uint64_t at = offset + done;
        gint64 key = (gint64)(at / RANGES_BLOCK);
        const guchar* block = g_hash_table_lookup(r->blocks, &key);
        if (!block) {
            if (!rangesFetch(r, (uint64_t)key))
                return -1;
            block = g_hash_table_lookup(r->blocks, &key);
        }
        uint64_t within = at % RANGES_BLOCK;
        size_t n = (size_t)MIN((uint64_t)(length - done), blockLength(r, (uint64_t)key) - within);
        memcpy((char*)buffer + done, block + within, n);
        done += n;
    }
    return (int64_t)done;
}

const char* ps5fetch_ranges_error(PS5FetchRanges* r)
{
    return r && *r->error ? r->error : NULL;
}

void ps5fetch_ranges_stats(PS5FetchRanges* r, unsigned* requests, uint64_t* bytes)
{
    if (requests)
        *requests = r ? r->requests : 0;
    if (bytes)
        *bytes = r ? r->bytes : 0;
}

void ps5fetch_ranges_close(PS5FetchRanges* r)
{
    if (!r)
        return;
    g_object_unref(r->session);
    g_hash_table_unref(r->blocks);
    g_free(r->url);
    g_free(r->referer);
    g_free(r);
}
