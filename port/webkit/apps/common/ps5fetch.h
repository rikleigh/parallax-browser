/*
 * ps5fetch.h: one HTTP(S) download over several connections (the download
 * service, apps/child/dl-job.c; port/webkit/README.md "Downloads").
 *
 * glib, libsoup 3 and POSIX only, so it builds for the PS5 (ps5wk-child) and
 * on the Mac (its test: port/webkit/test/downloads/fetch-test.sh).
 *
 * How a download runs
 *   The file is cut into chunks (PS5FetchOptions.chunk bytes, at least
 *   1 MiB). Up to `connections` worker threads, each with a SoupSession of
 *   its own, take the lowest chunk no one has yet and ask for it with
 *   "Range: bytes=a-b", then the next one. Work therefore stays near the
 *   front of the file: on a drive that can't hold a sparse file (exFAT
 *   USB), a write far past the end would make the system fill the gap with
 *   zeros first.
 *
 *   The first request (the probe) asks for chunk 0 with a Range. A 206 with
 *   a Content-Range total means ranges work: the other workers start. A 200
 *   means they don't: the probe's own body is the whole file, read by that
 *   one connection (no Range is asked again). A 206 that doesn't start where
 *   it was asked, a total that differs from the first one, or a validator
 *   (ETag, else Last-Modified) that changed is refused, so mismatched data
 *   is never joined; on a resume the file starts over from zero instead
 *   (status.restarted).
 *
 *   Data goes to <path>.part (pwrite at its offset); which bytes are on
 *   disk is kept in <path>.part.ps5dl (a GKeyFile: the address, the final
 *   address, the total, the validator, the chunk size and the finished
 *   byte ranges), written at most once a second and when a download stops.
 *   A download started again on the same path continues from it. When the
 *   last byte is on disk, the .part is renamed to <path> and the .ps5dl
 *   deleted.
 *
 *   A request that fails (a network error, no data for `stall_s`, 5xx,
 *   408, 429) is tried again by its worker after 1, 2, 4, 8, 15 s; its
 *   chunk goes back to the queue meanwhile, keeping what it got. 429 or 503
 *   while other connections work lowers the number of connections by one
 *   (some hosts allow few at once). A 401/403/404/410 on the final address
 *   (a signed CDN address that expired) asks the original address once
 *   again (redirects followed, the new final address used from then on).
 *   The download fails when nothing has arrived on any connection for
 *   `stall_s` after those retries, on another 4xx, or on a disk error.
 *
 * Every function is thread-safe. Callbacks run on an engine thread.
 */
#ifndef PS5FETCH_H
#define PS5FETCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PS5Fetch PS5Fetch;

typedef struct {
    const char* url;          /* asked first: the address the data came from (after redirects) */
    const char* original_url; /* the address the download started from; NULL = url */
    const char* user_agent;   /* NULL: libsoup's */
    const char* referer;      /* NULL: none */
    /* The browser's cookies for these addresses, in cookies.txt (Netscape)
     * form, one per line: domain \t includeSubdomains(TRUE/FALSE) \t path \t
     * secure(TRUE/FALSE) \t expires(unix s, 0 = session) \t name \t value.
     * NULL: none. Kept in a SoupCookieJar per worker. */
    const char* cookies;
    const char* path;         /* the finished file; data in <path>.part */
    unsigned connections;     /* 1..16 */
    uint64_t chunk;           /* bytes per request; 0: 8 MiB */
    unsigned stall_s;         /* 0: 45 */
    uint64_t free_margin;     /* left free on the drive; 0: 16 MiB */
} PS5FetchOptions;

typedef enum {
    PS5FETCH_RUNNING,
    PS5FETCH_DONE,
    PS5FETCH_PAUSED,    /* ps5fetch_stop: <path>.part and .ps5dl stay */
    PS5FETCH_FAILED,    /* error says why; .part and .ps5dl stay */
    PS5FETCH_CANCELLED, /* ps5fetch_cancel: .part and .ps5dl are deleted */
} PS5FetchState;

typedef struct {
    PS5FetchState state;
    int64_t total;          /* -1 while not known */
    uint64_t received;      /* bytes on disk */
    double speed;           /* bytes/s over the last ~2 s, all connections */
    unsigned connections;   /* receiving now */
    bool ranges;            /* the server answers Range (known after the probe) */
    bool restarted;         /* a resume had to start over from zero */
    bool retryable;         /* FAILED: a network error or stall (worth a later retry) */
    int http_status;        /* the last response's (0: none yet) */
    char final_url[2048];
    char etag[256];
    char last_modified[64];
    char content_type[128];
    char error[256];        /* FAILED: why, for people ("The server refused: 403 Forbidden") */
} PS5FetchStatus;

/* ended: once, when the state leaves RUNNING (on an engine thread). */
typedef void (*ps5fetch_ended_fn)(PS5Fetch* fetch, PS5FetchState state, void* data);

/* Starts (or continues, if <path>.part.ps5dl is there) a download. Copies
 * what it needs from options. NULL only if threads can't be made. */
PS5Fetch* ps5fetch_start(const PS5FetchOptions* options, ps5fetch_ended_fn ended, void* data);
/* A snapshot (any thread). */
void ps5fetch_status(PS5Fetch* fetch, PS5FetchStatus* out);
/* Stop and keep the .part and .ps5dl (PAUSED). Returns at once; `ended`
 * follows when the workers have stopped. */
void ps5fetch_stop(PS5Fetch* fetch);
/* Stop and delete the .part and .ps5dl (CANCELLED). */
void ps5fetch_cancel(PS5Fetch* fetch);
/* Waits for the workers (after `ended`) and frees. Not from `ended`. */
void ps5fetch_free(PS5Fetch* fetch);

/* What a later ps5fetch_start on path would continue from: the bytes on
 * disk and the total from <path>.part.ps5dl. false if there is none. */
bool ps5fetch_saved(const char* path, uint64_t* received, int64_t* total);

/* Pieces of a file on a server that answers Range, read on demand (the
 * download manager's look inside an archive before it is downloaded:
 * apps/child/dl-job.c "peek"). Blocking: the calling thread's own requests,
 * one at a time, kept in 64 KiB blocks (a read near the last one asks
 * further ahead, up to 1 MiB). Fails, and stays failed, once more than
 * max_bytes were asked for in all, timeout_ms has passed since the open,
 * or the server answers anything but a 206 for the range asked. */
typedef struct PS5FetchRanges PS5FetchRanges;

typedef struct {
    const char* url;        /* asked first; the address it redirects to is used after */
    const char* user_agent; /* NULL: libsoup's */
    const char* referer;
    const char* cookies;    /* cookies.txt lines, as PS5FetchOptions */
    uint64_t size;          /* the file's (from the download's response) */
    unsigned timeout_ms;    /* 0: 10000 */
    uint64_t max_bytes;     /* 0: 16 MiB */
} PS5FetchRangesOptions;

PS5FetchRanges* ps5fetch_ranges_open(const PS5FetchRangesOptions* options);
/* length bytes at offset into buffer (fewer only at the end of the file):
 * the number read, or -1 (ps5fetch_ranges_error says why). */
int64_t ps5fetch_ranges_read(PS5FetchRanges* ranges, uint64_t offset, void* buffer, size_t length);
const char* ps5fetch_ranges_error(PS5FetchRanges* ranges);
void ps5fetch_ranges_stats(PS5FetchRanges* ranges, unsigned* requests, uint64_t* bytes);
void ps5fetch_ranges_close(PS5FetchRanges* ranges);

#ifdef __cplusplus
}
#endif

#endif
