/*
 * dl-job.c: the download service, in ps5wk-child as a task payload of its
 * own (port/webkit/README.md "Downloads").
 *
 * The title (apps/title/dlservice.c) writes a job file with a token and
 * starts the child ELF with "PS5WK-LAUNCH 1 dl 0 <job file>". This process
 * listens on 127.0.0.1 (a port the system picks), prints
 * "[dl-job] port: <n>", and serves one connection: the title's, which
 * must say "hello token=<token>" first. It ends when the title closes the
 * task's connection (stdin), as the other jobs do; the downloads still
 * running are stopped first, so each one's .part and .ps5dl stay and the
 * next service continues them.
 *
 * What it does, all off the title (the console slows a title's own file
 * writes to about 2 MB/s after a few hundred megabytes, and a title can't
 * see every drive):
 *   - downloads over several connections (apps/common/ps5fetch.c), to any
 *     drive;
 *   - jobs on finished files: unpacking archives (ps5archive.c), installing
 *     homebrew apps (ps5install.c), images (an app's folder as exFAT,
 *     ps5exfat.c, or UFS2 .ffpkg, ps5ufs2.c), installing a downloaded
 *     image (moved to the drive's homebrew folder or given to
 *     ShadowMountPlus by path) or package (.pkg, served to the console's
 *     installer: ps5pkginstall.c), moving files between drives;
 *   - file operations for the download manager's page: the drives, a
 *     folder's entries, new folders, deleting, renaming, a free name.
 *
 * Lines both ways are "<verb> <key>=<value>&...", values percent-encoded:
 *
 *   title -> service
 *     hello    token
 *     fetch    id url orig ua ref cookies path conns chunk
 *     stop     id                      (pause: .part and .ps5dl stay)
 *     cancel   id                      (.part and .ps5dl deleted)
 *     job      id kind=extract|install|image|move|copy|delete src dest [delete=1]
 *              [then=install|image] [image=<path>] [format=exfat|ffpkg] [replace=1]
 *              [password]
 *              (an install that would replace an installed app is refused
 *              without replace=1: jobend says installed=<version>; an
 *              install's src may be an image or a .pkg file: ps5image.h;
 *              an archive with a password needs password: jobend says
 *              password=needed, or wrong for one that isn't it. Never
 *              logged or kept.)
 *     jobstop  id
 *     rm       path                    (a file or .part, and its .part.ps5dl)
 *     req      n op=roots|ls|mkdir|delete|rename|unique|saved|partials|partial|imageapp|titleinfo|icon
 *              path [to] [name] [dirs] [title] (partials: the downloads with a
 *              .part.ps5dl in the folders in dirs, one per line; imageapp:
 *              {installed} for the image at path, whether ShadowMountPlus has
 *              installed title from it; titleinfo: what the image, package
 *              or app folder at path holds (ps5image_json); icon: its
 *              icon0.png copied to {path} in the service's icon cache)
 *     peek     id (url size [ua] [ref] [cookies] | path)
 *              what an archive holds, on a server (Range requests for
 *              the parts that list its files) or on a drive
 *
 *   service -> title
 *     fetch    id state recv total speed conns ranges status final etag lm type
 *              (every PROGRESS_MS while it runs)
 *     fetchend id state recv total error retryable restarted status final etag lm type
 *     job      id phase done total current
 *     jobend   id ok error out [title titlename version] [installed] [by waits replaced] [password]
 *              [registered] [pkg=1 code] (pkg: a package the console's installer
 *              took, code: its answer when it refused)
 *     rep      n ok json | error
 *     peekend  id ok json | error
 *              json: {format, entries, unpacked, sizesKnown, titles,
 *              titleDir, titleId, name, version, guessed, installed,
 *              refused, requests, fetched, ms}
 *
 * Paths are only accepted under /data, /mnt/usbN and /mnt/extN (no "."
 * or ".." parts); the browser's own data under /data/ps5webkit can't be
 * deleted or renamed, except its users' downloads folders.
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <glib.h>

#include "../common/ps5archive.h"
#include "../common/ps5exfat.h"
#include "../common/ps5fetch.h"
#include "../common/ps5files.h"
#include "../common/ps5image.h"
#include "../common/ps5install.h"
#include "../common/ps5launchers.h"
#include "../common/ps5peek.h"
#include "../common/ps5pkginstall.h"
#include "../common/ps5ufs2.h"

int ps5wk_dl_job(const char* jobPath);

#define JOBLOG(...) do { fprintf(stderr, "[dl-job] " __VA_ARGS__); fputc('\n', stderr); } while (0)

#define PROGRESS_MS 500
#define JOB_PROGRESS_MS 250
#define LINE_MAX_BYTES (8 * 1024 * 1024) /* longer lines are dropped (logged) */
#define JOBS_AT_ONCE 2

static char* token;
static int client = -1;
static GMutex sendLock; /* client and writes to it */

/* ---------------------------------------------------------------- lines */

static void appendParam(GString* line, const char* key, const char* value)
{
    if (line->str[line->len - 1] != ' ')
        g_string_append_c(line, '&');
    g_string_append(line, key);
    g_string_append_c(line, '=');
    if (!value)
        return;
    /* names from archives and servers may not be UTF-8: the title's parser
     * would drop the whole line */
    if (g_utf8_validate(value, -1, NULL))
        g_string_append_uri_escaped(line, value, NULL, FALSE);
    else {
        char* valid = g_utf8_make_valid(value, -1);
        g_string_append_uri_escaped(line, valid, NULL, FALSE);
        g_free(valid);
    }
}

static void appendNumber(GString* line, const char* key, long long value)
{
    char text[32];
    snprintf(text, sizeof text, "%lld", value);
    appendParam(line, key, text);
}

/* Sends line (no newline yet) to the title, if it is there; frees it. */
static void sendLine(GString* line)
{
    g_string_append_c(line, '\n');
    g_mutex_lock(&sendLock);
    if (client >= 0) {
        const char* p = line->str;
        size_t left = line->len;
        while (left) {
            ssize_t n = send(client, p, left, 0);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            p += n;
            left -= (size_t)n;
        }
    }
    g_mutex_unlock(&sendLock);
    g_string_free(line, TRUE);
}

static GString* startLine(const char* verb)
{
    GString* line = g_string_new(verb);
    g_string_append_c(line, ' ');
    return line;
}

static void jsonString(GString* out, const char* text)
{
    g_string_append_c(out, '"');
    for (const unsigned char* c = (const unsigned char*)(text ? text : ""); *c; c++) {
        if (*c == '"' || *c == '\\')
            g_string_append_printf(out, "\\%c", *c);
        else if (*c < 0x20)
            g_string_append_printf(out, "\\u%04x", *c);
        else
            g_string_append_c(out, (char)*c);
    }
    g_string_append_c(out, '"');
}

/* ---------------------------------------------------------------- paths */

static gboolean isDrivePrefix(const char* path, size_t* length)
{
    if (!strncmp(path, "/data", 5) && (path[5] == '/' || !path[5])) {
        *length = 5;
        return TRUE;
    }
    if ((!strncmp(path, "/mnt/usb", 8) || !strncmp(path, "/mnt/ext", 8)) && path[8] >= '0' && path[8] <= '9'
        && (path[9] == '/' || !path[9])) {
        *length = 9;
        return TRUE;
    }
    return FALSE;
}

/* Under a drive, absolute, no "." or ".." parts, no "//". */
static gboolean allowedPath(const char* path)
{
    size_t root = 0;
    if (!path || !isDrivePrefix(path, &root) || strlen(path) > 1000)
        return FALSE;
    for (const char* p = path; *p; p++) {
        if (*p != '/')
            continue;
        if (p[1] == '/' || (p[1] == '.' && (p[2] == '/' || !p[2])) || (p[1] == '.' && p[2] == '.' && (p[3] == '/' || !p[3])))
            return FALSE;
    }
    return TRUE;
}

/* What may be deleted or renamed: not a drive itself, not the browser's own
 * data (but its users' downloads folders and what is in them). */
static gboolean changeablePath(const char* path)
{
    size_t root = 0;
    if (!allowedPath(path) || !isDrivePrefix(path, &root) || !path[root] || !path[root + 1])
        return FALSE;
    if (!strncmp(path, "/data/ps5webkit", 15) && (path[15] == '/' || !path[15])) {
        const char* users = "/data/ps5webkit/users/";
        if (strncmp(path, users, strlen(users)))
            return FALSE;
        const char* rest = strchr(path + strlen(users), '/'); /* after <id> */
        return rest && !strncmp(rest, "/downloads/", 11) && rest[11];
    }
    return TRUE;
}

/* A folder new files may go in: one the browser can use, but not its own
 * data under /data/ps5webkit (only the users' downloads folders there). */
static gboolean writableFolder(const char* dir)
{
    if (!allowedPath(dir))
        return FALSE;
    if (!strncmp(dir, "/data/ps5webkit", 15) && (dir[15] == '/' || !dir[15])) {
        const char* users = "/data/ps5webkit/users/";
        if (strncmp(dir, users, strlen(users)))
            return FALSE;
        const char* rest = strchr(dir + strlen(users), '/'); /* after <id> */
        return rest && !strncmp(rest, "/downloads", 10) && (!rest[10] || rest[10] == '/');
    }
    return TRUE;
}

/* A new file or folder at path: its folder writable, the name itself not
 * the folder (a drive's root can't be replaced). */
static gboolean writablePath(const char* path)
{
    if (!path || !allowedPath(path))
        return FALSE;
    char* folder = g_path_get_dirname(path);
    gboolean ok = writableFolder(folder);
    g_free(folder);
    return ok;
}

static gboolean isRegularFile(const char* path)
{
    struct stat st;
    return !lstat(path, &st) && S_ISREG(st.st_mode);
}

static gboolean exists(const char* path)
{
    struct stat st;
    return !lstat(path, &st);
}

/* ---------------------------------------------------------------- downloads */

typedef struct {
    char* id;
    char* name; /* the file's name in its folder */
    char* path; /* the file's (its .part is <path>.part): names in use */
    PS5Fetch* fetch; /* NULL while starting */
    gboolean starting; /* ps5fetch_start hasn't returned yet: not the ticker's */
    gboolean stopping; /* asked to stop or cancel */
    gboolean ended; /* its ended callback ran: freed by the ticker */
    /* a "fetch" for the same id that came while this one was stopping
     * (pause, then resume at once): started by the ticker once this one
     * has ended, and this one's "fetchend" isn't sent */
    GHashTable* again;
} Fetch;

static GMutex fetchLock;
static GHashTable* fetches; /* id -> Fetch* */

static void appendStatus(GString* line, const PS5FetchStatus* s)
{
    static const char* const states[] = { "running", "done", "paused", "failed", "cancelled" };
    appendParam(line, "state", states[s->state]);
    appendNumber(line, "recv", (long long)s->received);
    appendNumber(line, "total", (long long)s->total);
    appendNumber(line, "speed", (long long)s->speed);
    appendNumber(line, "conns", s->connections);
    appendNumber(line, "allowed", s->allowed);
    appendNumber(line, "ranges", s->ranges);
    appendNumber(line, "restarted", s->restarted);
    appendNumber(line, "status", s->http_status);
    appendParam(line, "final", s->final_url);
    appendParam(line, "etag", s->etag);
    appendParam(line, "lm", s->last_modified);
    appendParam(line, "type", s->content_type);
}

static void fetchEnded(PS5Fetch* fetch, PS5FetchState state, void* data)
{
    Fetch* f = data;
    PS5FetchStatus s;
    ps5fetch_status(fetch, &s);
    GString* line = startLine("fetchend");
    appendParam(line, "id", f->id);
    appendParam(line, "name", f->name);
    appendStatus(line, &s);
    appendParam(line, "error", s.error);
    appendNumber(line, "retryable", s.retryable);
    JOBLOG("download %s ended: state %d, %llu of %lld bytes%s%s", f->id, (int)state, (unsigned long long)s.received, (long long)s.total,
        *s.error ? ": " : "", s.error);
    g_mutex_lock(&fetchLock);
    f->ended = TRUE;
    gboolean again = f->again != NULL;
    g_mutex_unlock(&fetchLock);
    if (again)
        g_string_free(line, TRUE); /* started again: the title waits for that one */
    else
        sendLine(line);
}

static void fetchFree(gpointer data)
{
    Fetch* f = data;
    if (f->fetch)
        ps5fetch_free(f->fetch);
    if (f->again)
        g_hash_table_unref(f->again);
    g_free(f->id);
    g_free(f->name);
    g_free(f->path);
    g_free(f);
}

static void startFetch(GHashTable* p);

/* Every PROGRESS_MS: the running downloads' progress; ended ones freed. */
static gpointer tickerThread(gpointer data)
{
    (void)data;
    for (;;) {
        g_usleep(PROGRESS_MS * 1000);
        GPtrArray* done = g_ptr_array_new_with_free_func(fetchFree);
        GPtrArray* lines = g_ptr_array_new();
        GPtrArray* again = g_ptr_array_new_with_free_func((GDestroyNotify)g_hash_table_unref);
        g_mutex_lock(&fetchLock);
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, fetches);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            Fetch* f = value;
            if (f->starting)
                continue;
            if (f->ended) {
                g_hash_table_iter_steal(&iter);
                if (f->again) {
                    g_ptr_array_add(again, f->again);
                    f->again = NULL;
                }
                g_ptr_array_add(done, f);
                continue;
            }
            PS5FetchStatus s;
            ps5fetch_status(f->fetch, &s);
            if (s.state != PS5FETCH_RUNNING)
                continue;
            GString* line = startLine("fetch");
            appendParam(line, "id", f->id);
            appendParam(line, "name", f->name);
            appendStatus(line, &s);
            g_ptr_array_add(lines, line);
        }
        g_mutex_unlock(&fetchLock);
        for (unsigned i = 0; i < lines->len; i++)
            sendLine(g_ptr_array_index(lines, i));
        g_ptr_array_unref(lines);
        g_ptr_array_unref(done); /* waits for their workers: outside the lock */
        for (unsigned i = 0; i < again->len; i++)
            startFetch(g_ptr_array_index(again, i));
        g_ptr_array_unref(again);
    }
    return NULL;
}

static const char* param(GHashTable* p, const char* key)
{
    const char* value = g_hash_table_lookup(p, key);
    return value && *value ? value : NULL;
}

static gboolean validName(const char* name);
static char* uniqueName(const char* folder, const char* name);

/* Whether path (or its .part) is in use: on the disk, or by a download
 * here that hasn't made its .part yet. fetchLock held. */
static gboolean pathTaken(const char* path)
{
    char* part = g_strdup_printf("%s.part", path);
    gboolean taken = exists(path) || exists(part);
    g_free(part);
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, fetches);
    while (!taken && g_hash_table_iter_next(&iter, &key, &value)) {
        const Fetch* f = value;
        taken = !f->ended && f->path && !strcmp(f->path, path);
    }
    return taken;
}

static void refuseFetch(const char* id, const char* why)
{
    GString* line = startLine("fetchend");
    appendParam(line, "id", id ? id : "");
    appendParam(line, "state", "failed");
    appendParam(line, "error", why);
    sendLine(line);
}

static void startFetch(GHashTable* p)
{
    const char* id = param(p, "id");
    const char* url = param(p, "url");
    const char* dir = param(p, "dir");
    const char* given = param(p, "name");
    const char* fresh = param(p, "fresh");
    if (!id || !url || !dir || !writableFolder(dir) || !validName(given) || g_str_has_suffix(given, ".part") || g_str_has_suffix(given, ".ps5dl")) {
        refuseFetch(id, "The download service refused the address or the folder");
        return;
    }
    g_mkdir_with_parents(dir, 0777);

    g_mutex_lock(&fetchLock);
    Fetch* old = g_hash_table_lookup(fetches, id);
    if (old && !old->ended && old->stopping) {
        /* paused and resumed at once: after this one has stopped */
        if (old->again)
            g_hash_table_unref(old->again);
        old->again = g_hash_table_ref(p);
        g_mutex_unlock(&fetchLock);
        JOBLOG("download %s: starts again once it has stopped", id);
        return;
    }
    if (old && !old->ended) {
        g_mutex_unlock(&fetchLock);
        JOBLOG("download %s is already running", id);
        return;
    }
    if (old)
        g_hash_table_steal(fetches, id); /* ended, not freed yet: ours now */
    /* a fresh download gets a name free in the folder, also among the
     * downloads starting here (a continued one keeps its own: its .part
     * is there) */
    char* name = fresh && !strcmp(fresh, "1") ? uniqueName(dir, given) : g_strdup(given);
    Fetch* f = g_new0(Fetch, 1);
    f->id = g_strdup(id);
    f->name = name;
    f->path = g_build_filename(dir, name, NULL);
    f->starting = TRUE;
    g_hash_table_insert(fetches, f->id, f);
    g_mutex_unlock(&fetchLock);
    if (old)
        fetchFree(old); /* waits for its workers: outside the lock */

    PS5FetchOptions o = { 0 };
    o.url = url;
    o.original_url = param(p, "orig");
    o.user_agent = param(p, "ua");
    o.referer = param(p, "ref");
    o.cookies = param(p, "cookies");
    o.path = f->path;
    const char* conns = param(p, "conns");
    o.connections = conns ? (unsigned)CLAMP(atoi(conns), 1, 16) : 8;
    /* more while they make it faster, up to this (ps5fetch.h) */
    const char* maxConns = param(p, "maxconns");
    o.max_connections = maxConns ? (unsigned)CLAMP(atoi(maxConns), 0, 32) : 0;
    const char* chunk = param(p, "chunk");
    o.chunk = chunk ? g_ascii_strtoull(chunk, NULL, 10) : 0;
    JOBLOG("download %s: %s to %s, %u connections (up to %u)", id, url, f->path, o.connections, MAX(o.connections, o.max_connections));
    PS5Fetch* fetch = ps5fetch_start(&o, fetchEnded, f);
    g_mutex_lock(&fetchLock);
    f->fetch = fetch;
    f->starting = FALSE;
    if (!fetch)
        f->ended = TRUE;
    g_mutex_unlock(&fetchLock);
    if (!fetch) {
        GString* line = startLine("fetchend");
        appendParam(line, "id", id);
        appendParam(line, "state", "failed");
        appendParam(line, "error", "The download service couldn't start the download");
        appendNumber(line, "retryable", 1);
        sendLine(line);
    }
}

static void stopFetch(const char* id, gboolean cancel)
{
    if (!id)
        return;
    g_mutex_lock(&fetchLock);
    Fetch* f = g_hash_table_lookup(fetches, id);
    if (f && f->again) {
        /* a start waiting for this one to stop: not any more */
        g_hash_table_unref(f->again);
        f->again = NULL;
    }
    if (f && !f->ended && f->fetch) {
        f->stopping = TRUE;
        if (cancel)
            ps5fetch_cancel(f->fetch);
        else
            ps5fetch_stop(f->fetch);
    }
    g_mutex_unlock(&fetchLock);
    if (!f) {
        /* not running here: a paused download's files */
        GString* line = startLine("fetchend");
        appendParam(line, "id", id);
        appendParam(line, "state", cancel ? "cancelled" : "paused");
        sendLine(line);
    }
}

/* ---------------------------------------------------------------- jobs */

typedef struct {
    char* id;
    char* kind;
    char* src;
    char* dest;
    char* then;
    char* image;
    char* format;     /* an image's: "exfat" (NULL too) or "ffpkg" */
    gboolean deleteSource;
    gboolean replace; /* an install may replace the app installed with the same ID */
    char* password;   /* an archive's (wiped when the job ends) */
    const char* passwordAsk; /* jobend: "needed" or "wrong" */
    char* launcher;   /* the setting: "auto", "smp", "kraken", "titan", "self" (ps5launchers_force) */
    char installed[32]; /* refused: the version installed ("?": not known) */
    char by[32];      /* installed: the launcher that registers it ("" the browser did) */
    gboolean waits;   /* ... Kraken Mount, which waits for the browser to close */
    gboolean replaced; /* ... an installed copy was replaced */
    char registered[16]; /* the browser registered it: "kstuff" (mounted when kstuff next starts),
                          * "blocked" (/data/.kstuff_noautomount stops that), "setting" (a launcher runs) */
    gboolean package; /* a .pkg given to the console's installer */
    char app[1024];   /* unpacked (nothing after): the one app's folder in it, to offer installing */
    unsigned code;    /* ... which refused it with this */
    volatile int cancel;
    gint64 lastProgress;
} Job;

static GMutex jobLock;
static GHashTable* jobs; /* id -> Job* */
static unsigned jobsRunning;
static GCond jobSlot;

static void jobProgress(Job* job, const char* phase, uint64_t done, uint64_t total, const char* current, gboolean force)
{
    gint64 now = g_get_monotonic_time();
    if (!force && now - job->lastProgress < JOB_PROGRESS_MS * 1000)
        return;
    job->lastProgress = now;
    GString* line = startLine("job");
    appendParam(line, "id", job->id);
    appendParam(line, "phase", phase);
    appendNumber(line, "done", (long long)done);
    appendNumber(line, "total", (long long)total);
    appendParam(line, "current", current);
    sendLine(line);
}

static void onExtractProgress(uint64_t done, uint64_t total, const char* current, void* data)
{
    jobProgress(data, "extract", done, total, current, FALSE);
}

static void onImageProgress(uint64_t done, uint64_t total, const char* current, void* data)
{
    jobProgress(data, "image", done, total, current, FALSE);
}

static void onVerifyProgress(uint64_t done, uint64_t total, const char* current, void* data)
{
    jobProgress(data, "verify", done, total, current, FALSE);
}

static void autoImagePath(const char* source, const PS5TitleInfo* title, const char* format, char* out, size_t size);

static gboolean isUfs2(const Job* job)
{
    return job->format && !strcmp(job->format, "ffpkg");
}

/* ShadowMountPlus copies sce_sys/icon0.png when it installs an image's app,
 * and gives up on an app without one (seen 2026-11-21: "Failed to copy
 * staged icon", then "retry limit reached (2/2)" until it restarts). */
#define NO_ICON "The app has no icon (sce_sys/icon0.png): ShadowMountPlus won't install an image of it"

/* An image is made of exactly one app (a folder with sce_sys/param.json
 * and eboot.bin): anything else wouldn't mount as a game. FALSE with why
 * in error. */
static gboolean oneAppIn(const char* dir, char* error, size_t size)
{
    int n = ps5install_count(dir, 3);
    if (n == 1)
        return TRUE;
    char param[1024], eboot[1024];
    struct stat st;
    snprintf(param, sizeof param, "%s/sce_sys/param.json", dir);
    snprintf(eboot, sizeof eboot, "%s/eboot.bin", dir);
    if (n == -1)
        snprintf(error, size, "Out of memory");
    else if (n < 0)
        snprintf(error, size, "Too many folders to look through for the app: make the image of the app's own folder");
    else if (!n && !stat(param, &st) && !stat(eboot, &st))
        snprintf(error, size, "This app's sce_sys/param.json has no title ID that can be read, so it can't be made an image");
    else if (!n)
        snprintf(error, size, "No PS5 app here (a folder with sce_sys/param.json and eboot.bin): an image is only made of one");
    else
        snprintf(error, size, "%d apps here: make the image of one app's folder", n);
    return FALSE;
}

/* The same for an archive, before it is unpacked (when it can be listed). */
static gboolean archiveOneApp(const char* archive, const char* password, char* error, size_t size)
{
    PS5ArchiveInfo info;
    char why[256];
    PS5ArchiveLimits limits = { .password = password };
    if (ps5archive_inspect(archive, &limits, &info, why, sizeof why))
        return TRUE; /* not listed: checked once unpacked */
    if (info.titles == 1 && !info.title_icon) {
        snprintf(error, size, "%s", NO_ICON);
        return FALSE;
    }
    if (info.titles == 1)
        return TRUE;
    if (!info.titles)
        snprintf(error, size, "It holds no PS5 app (sce_sys/param.json and eboot.bin): an image is only made of one; unpack it instead");
    else
        snprintf(error, size, "It holds %u apps: unpack it, then make an image of each app's folder from Files", info.titles);
    return FALSE;
}

/* Builds image (exFAT, or UFS2 for format=ffpkg) from the app at source
 * and reads it back to compare (ps5exfat_verify, ps5ufs2_verify); a bad
 * image is deleted. TRUE: out says where. */
static gboolean buildImage(Job* job, const char* source, const PS5TitleInfo* title, const char* where, char* out, size_t outSize,
    char* error, size_t size)
{
    char image[1024];
    if (!strcmp(where, "auto"))
        autoImagePath(source, title, job->format, image, sizeof image);
    else
        snprintf(image, sizeof image, "%s", where);
    if (isUfs2(job)) {
        /* refused only when a ShadowMountPlus that runs is known to be too
         * old: the image may be meant for another console, or a later one */
        PS5Launchers* l = g_new0(PS5Launchers, 1);
        ps5launchers_detect(l, NULL);
        char why[256] = "";
        int mounts = ps5launchers_mounts_ufs2(l, why, sizeof why);
        g_free(l);
        if (!mounts) {
            snprintf(error, size, "%s", why);
            return FALSE;
        }
    }
    char icon[1024];
    struct stat st;
    snprintf(icon, sizeof icon, "%s/sce_sys/icon0.png", source);
    if (stat(icon, &st) || !S_ISREG(st.st_mode)) {
        snprintf(error, size, "%s", NO_ICON);
        return FALSE;
    }
    int v;
    char why[512];
    if (isUfs2(job)) {
        if (ps5ufs2_build(source, image, NULL, onImageProgress, job, &job->cancel, error, size))
            return FALSE;
        jobProgress(job, "verify", 0, 0, "", TRUE);
        v = ps5ufs2_verify(image, source, onVerifyProgress, job, &job->cancel, why, sizeof why);
    } else {
        PS5ExfatOptions o = { .label = title->title_id };
        if (ps5exfat_build(source, image, &o, onImageProgress, job, &job->cancel, error, size))
            return FALSE;
        jobProgress(job, "verify", 0, 0, "", TRUE);
        v = ps5exfat_verify(image, source, onVerifyProgress, job, &job->cancel, why, sizeof why);
    }
    if (v) {
        unlink(image);
        if (v == -3)
            snprintf(error, size, "Cancelled");
        else
            snprintf(error, size, "The image didn't match the folder when read back (%s): it was deleted", why);
        return FALSE;
    }
    snprintf(out, outSize, "%s", image);
    return TRUE;
}

static void onInstallLog(const char* line, void* data)
{
    (void)data;
    JOBLOG("install: %s", line);
}

static void jobEnd(Job* job, gboolean ok, const char* error, const char* out, const PS5TitleInfo* title)
{
    GString* line = startLine("jobend");
    appendParam(line, "id", job->id);
    appendNumber(line, "ok", ok);
    appendParam(line, "error", error);
    appendParam(line, "out", out);
    if (title) {
        appendParam(line, "title", title->title_id);
        appendParam(line, "titlename", title->name);
        appendParam(line, "version", title->version);
    }
    if (*job->installed)
        appendParam(line, "installed", job->installed); /* refused: replacing needs replace=1 */
    if (ok && *job->by) {
        appendParam(line, "by", job->by); /* that launcher registers it: "appready" says when */
        if (job->waits)
            appendNumber(line, "waits", 1);
        if (job->replaced)
            appendNumber(line, "replaced", 1);
    }
    if (ok && *job->registered)
        appendParam(line, "registered", job->registered);
    if (ok && *job->app)
        appendParam(line, "app", job->app);
    if (!ok && job->passwordAsk)
        appendParam(line, "password", job->passwordAsk);
    if (job->package) {
        appendNumber(line, "pkg", 1);
        if (job->code) {
            char code[16];
            snprintf(code, sizeof code, "0x%08X", job->code);
            appendParam(line, "code", code);
        }
    }
    JOBLOG("job %s (%s) %s%s%s", job->id, job->kind, ok ? "done" : "failed", error ? ": " : "", error ? error : "");
    sendLine(line);
}

static void onTransferProgress(uint64_t done, uint64_t total, const char* current, void* data)
{
    Job* job = data;
    jobProgress(job, job->kind, done, total, current, FALSE);
}

static void deleteVolumes(const char* archive)
{
    char error[256];
    char** parts = ps5archive_volumes(archive, error, sizeof error);
    for (char** p = parts; p && *p; p++) {
        if (unlink(*p))
            JOBLOG("couldn't delete %s (errno %d)", *p, errno);
    }
    ps5archive_free_list(parts);
}

/* Where an image goes when the title says "auto": ShadowMountPlus scans
 * <drive>/homebrew for <TITLE_ID>.exfat (ps5exfat.h) and .ffpkg
 * (ps5ufs2.h); without a title, next to the source as <name>.exfat. */
static void autoImagePath(const char* source, const PS5TitleInfo* title, const char* format, char* out, size_t size)
{
    const char* extension = format && !strcmp(format, "ffpkg") ? "ffpkg" : "exfat";
    char drive[64];
    if (title && ps5install_drive_of(source, drive, sizeof drive)) {
        char folder[128];
        snprintf(folder, sizeof folder, "%s/homebrew", drive);
        mkdir(folder, 0777);
        snprintf(out, size, "%s/%s.%s", folder, title->title_id, extension);
        return;
    }
    char* trimmed = g_strdup(source);
    while (strlen(trimmed) > 1 && trimmed[strlen(trimmed) - 1] == '/')
        trimmed[strlen(trimmed) - 1] = 0;
    snprintf(out, size, "%s.%s", trimmed, extension);
    g_free(trimmed);
}

static char* uniqueFolder(const char* parent, const char* name);

/* Whether title is installed (registered, or its folder in its drive's
 * homebrew folder, which an install replaces): the version in version. */
static gboolean alreadyInstalled(const PS5TitleInfo* title, const char* drivePath, char* version, size_t size)
{
    PS5TitleInfo installed;
    if (ps5install_installed(NULL, title->title_id, &installed)) {
        snprintf(version, size, "%s", *installed.version ? installed.version : "?");
        return TRUE;
    }
    char drive[64];
    if (ps5install_drive_of(drivePath, drive, sizeof drive)) {
        char* folder = g_strdup_printf("%s/homebrew/%s", drive, title->title_id);
        gboolean there = exists(folder);
        g_free(folder);
        if (there) {
            snprintf(version, size, "?");
            return TRUE;
        }
    }
    return FALSE;
}

/* The app in an archive, from its param.json (before unpacking). */
static gboolean archiveTitle(const char* archive, const char* password, PS5TitleInfo* title)
{
    PS5ArchiveInfo info;
    char error[256];
    PS5ArchiveLimits limits = { .password = password };
    if (ps5archive_inspect(archive, &limits, &info, error, sizeof error) || !info.has_title)
        return FALSE;
    char* name = g_strdup_printf("%s%ssce_sys/param.json", info.title_dir, *info.title_dir ? "/" : "");
    char* text = g_malloc(256 * 1024);
    gboolean ok = !ps5archive_read_entry(archive, NULL, password, name, text, 256 * 1024, NULL, error, sizeof error)
        && !ps5install_parse_param(text, title);
    g_free(name);
    g_free(text);
    return ok;
}

/* An install that would replace an installed app, not asked to: refused
 * (job->installed set for the title's "Replace" button). */
static gboolean refuseReplace(Job* job, const PS5TitleInfo* title, const char* drivePath, char* error, size_t size)
{
    char version[32];
    if (job->replace || !alreadyInstalled(title, drivePath, version, sizeof version))
        return FALSE;
    snprintf(job->installed, sizeof job->installed, "%s", version);
    if (strcmp(version, "?"))
        snprintf(error, size, "%s (%s) version %s is already installed", title->name, title->title_id, version);
    else
        snprintf(error, size, "%s (%s) is already installed", title->name, title->title_id);
    return TRUE;
}

/* Installs title (its folder title->dir). A homebrew launcher running
 * (ps5launchers.h): the folder only goes to <drive>/homebrew/<ID>, and the
 * launcher is asked to look (job->by says which); else it is registered
 * here (job->registered says what mounts it). 0, or ps5install_title's
 * -1/-2 (error says why). */
static int installTitle(Job* job, const PS5TitleInfo* title, char* out, size_t outSize, char* error, size_t size)
{
    PS5Launchers* l = g_new0(PS5Launchers, 1); /* 8 KB: not on this thread's stack */
    ps5launchers_detect(l, NULL);
    ps5launchers_force(l, job->launcher);
    const char* scanner = ps5launchers_scanner(l);
    JOBLOG("install %s: launcher %s%s (found: %s)", title->title_id, scanner ? scanner : "none", l->forced ? " (the setting)" : "",
        *l->found ? l->found : "none");
    PS5InstallOptions o = { .do_register = true, .log = onInstallLog };
    char target[1024], from[1024], why[256] = "";
    int r = -1;
    if (!scanner) {
        r = ps5install_title(title, &o, out, outSize, error, size);
        if (!r)
            snprintf(job->registered, sizeof job->registered, "%s", l->forced && *l->found ? "setting" : !l->kstuff_automount ? "blocked" : "kstuff");
    } else if (ps5install_target(title, target, sizeof target))
        snprintf(error, size, "Apps can be installed from internal storage, M.2 or USB drives only");
    else if (!ps5launchers_scans(l, target, why, sizeof why))
        snprintf(error, size, "%s", why);
    else if (ps5launchers_source(l, title->title_id, ps5install_installed(NULL, title->title_id, NULL), NULL, from, sizeof from)
        && strcmp(from, target) && strcmp(from, title->dir) && exists(from)) {
        /* another copy (another drive, an image) is in its library: the
         * launcher would keep that one and ignore this */
        snprintf(error, size, "Another copy of %s is at %s, and %s would keep using it: delete that copy first (in Files), then install this one",
            title->name, from, scanner);
    } else {
        gboolean replacing = exists(target);
        if (!replacing || !ps5launchers_before_replace(l, title->title_id, NULL, error, size)) {
            o.move_only = true;
            r = ps5install_title(title, &o, out, outSize, error, size);
            if (!r) {
                ps5launchers_announce(l, out, title->title_id, replacing, NULL, why, sizeof why);
                JOBLOG("install %s: %s", title->title_id, *why ? why : "(not asked: it scans by itself)");
                snprintf(job->by, sizeof job->by, "%s", scanner);
                job->waits = ps5launchers_waits(l);
                job->replaced = replacing;
            }
        }
    }
    g_free(l);
    return r;
}

/* Installs a downloaded image (.exfat, .ffpkg) holding one app at its
 * root (info: ps5image_info's). The image mounter (ShadowMountPlus, also
 * TITAN-CORE's) is needed: Kraken Mount and kstuff don't mount images.
 * When ShadowMountPlus scans the image's drive's homebrew folder, the image
 * is renamed to <drive>/homebrew/<ID>.<ext> (the same device only: a
 * rename across devices is a copy, and on the console a rename between
 * mounts can panic the kernel); otherwise it stays where it is and is given
 * to ShadowMountPlus by path (its API's manual list, else manual.lst).
 * 0, -1 refused, -2 failed (error says why); out: where the image is. */
static int installImage(Job* job, const PS5ImageInfo* info, PS5TitleInfo* title, char* out, size_t outSize, char* error, size_t size)
{
    if (!info->app_at_root || !*info->title_id) {
        snprintf(error, size, "%s", *info->title_id ? "The image's app isn't at its root (eboot.bin and sce_sys/param.json there): ShadowMountPlus wouldn't find it"
                                                     : "No app in the image (sce_sys/param.json with a title ID, and eboot.bin, at its root)");
        return -1;
    }
    if (!info->has_icon) {
        snprintf(error, size, "The image's app has no icon (sce_sys/icon0.png): ShadowMountPlus won't install it");
        return -1;
    }
    PS5Launchers* l = g_new0(PS5Launchers, 1);
    ps5launchers_detect(l, NULL);
    ps5launchers_force(l, job->launcher);
    const char* mounter = ps5launchers_image_mounter(l);
    char why[512] = "", from[1024] = "", drive[64];
    int r = -1;
    gboolean ufs2 = info->kind == PS5IMAGE_UFS2;
    const char* extension = ufs2 ? "ffpkg" : "exfat";
    if (!mounter) {
        snprintf(error, size, "Nothing that mounts images is running: start ShadowMountPlus (or TITAN-CORE, which starts it). Kraken Mount can't mount images");
        goto done;
    }
    if (ufs2 && !ps5launchers_mounts_ufs2(l, why, sizeof why)) {
        snprintf(error, size, "%s", why);
        goto done;
    }
    if (ps5launchers_source(l, title->title_id, ps5install_installed(NULL, title->title_id, NULL), NULL, from, sizeof from)
        && strcmp(from, job->src) && exists(from)) {
        char target[1100] = "";
        if (ps5install_drive_of(job->src, drive, sizeof drive))
            snprintf(target, sizeof target, "%s/homebrew/%s.%s", drive, title->title_id, extension);
        /* the copy it would replace is fine: that's what replace=1 says */
        if (!job->replace || strcmp(from, target)) {
            snprintf(error, size, "Another copy of %s is at %s, and %s would keep using it: delete that copy first (in Files), then install this one",
                title->name, from, mounter);
            goto done;
        }
    }
    snprintf(out, outSize, "%s", job->src);
    gboolean moved = FALSE, replacing = FALSE;
    if (ps5install_drive_of(job->src, drive, sizeof drive)) {
        char folder[128], target[1100];
        snprintf(folder, sizeof folder, "%s/homebrew", drive);
        snprintf(target, sizeof target, "%s/%s.%s", folder, title->title_id, extension);
        mkdir(folder, 0777);
        struct stat a, b;
        char* srcFolder = g_path_get_dirname(job->src);
        gboolean sameDevice = !stat(srcFolder, &a) && !stat(folder, &b) && a.st_dev == b.st_dev;
        g_free(srcFolder);
        if (!strcmp(target, job->src))
            moved = TRUE; /* already there */
        else if (sameDevice && ps5launchers_scans_itself(l, target)) {
            replacing = exists(target);
            if (replacing && !job->replace) {
                /* not registered (refuseReplace didn't see it), but there */
                snprintf(job->installed, sizeof job->installed, "?");
                snprintf(error, size, "An image of %s (%s) is in the homebrew folder already", title->name, title->title_id);
                goto done;
            }
            char aside[1200] = "";
            if (replacing && ps5launchers_before_replace(l, title->title_id, NULL, error, size))
                goto done;
            if (replacing) {
                snprintf(aside, sizeof aside, "%s.old-%lld", target, (long long)time(NULL));
                if (rename(target, aside)) {
                    snprintf(error, size, "Couldn't move the installed image aside (errno %d)", errno);
                    goto done;
                }
            }
            if (rename(job->src, target)) {
                snprintf(error, size, "Couldn't move the image to %s (errno %d)", folder, errno);
                if (*aside)
                    rename(aside, target);
                goto done;
            }
            if (*aside && unlink(aside))
                JOBLOG("install %s: couldn't delete %s (errno %d)", title->title_id, aside, errno);
            snprintf(out, outSize, "%s", target);
            moved = TRUE;
        }
    }
    if (!moved && !g_str_has_suffix(job->src, ufs2 ? ".ffpkg" : ".exfat")) {
        /* left in place: its name is what tells ShadowMountPlus the format */
        snprintf(error, size, "This is a%s image, but its name doesn't end in .%s: rename it, then install it", ufs2 ? " UFS2" : "n exFAT",
            extension);
        r = -1;
        goto done;
    }
    if (l->smp_api || moved) {
        /* in its folders: asked to scan; elsewhere: added to its list */
        r = ps5launchers_announce(l, out, title->title_id, replacing, NULL, why, sizeof why);
        if (r && !moved) {
            snprintf(error, size, "%s", why);
            r = -2;
            goto done;
        }
        r = 0;
    } else {
        r = ps5launchers_manual_list_add(NULL, out, error, size) ? -2 : 0;
        snprintf(why, sizeof why, "added to /data/shadowmount/manual.lst (its API is off)");
    }
    JOBLOG("install %s: image at %s, %s: %s", title->title_id, out, moved ? "moved" : "left in place", *why ? why : "-");
    snprintf(job->by, sizeof job->by, "%s", mounter);
    job->replaced = replacing;
    snprintf(title->dir, sizeof title->dir, "%s", out);
done:
    g_free(l);
    return r;
}

static void onPackageProgress(uint64_t served, uint64_t total, void* data)
{
    jobProgress(data, "install", served, total, "", FALSE);
}

static void onPackageLog(const char* line, void* data)
{
    (void)data;
    JOBLOG("%s", line);
}

/* Hands a package (.pkg) to the console's installer (ps5pkginstall.h):
 * PS4 packages and PS5 fake packages; a retail PS5 one is refused. The job
 * lasts while the console copies it (progress: the bytes it has asked for).
 * 0, -1 refused, -2 failed (error says why). */
static int installPackage(Job* job, const PS5ImageInfo* info, char* note, size_t noteSize, char* error, size_t size)
{
    if (info->kind == PS5IMAGE_PKG_PS5 && !info->fake) {
        snprintf(error, size, "This is a retail PS5 package (%s): only fake packages (fPKG) can be installed from here",
            info->encrypted ? "encrypted" : "signed by Sony");
        return -1;
    }
    job->package = TRUE;
    char name[300];
    snprintf(name, sizeof name, "%s", *info->name ? info->name : *info->title_id ? info->title_id : "Package");
    PS5PkgInstallOptions o = { 0 };
    o.path = job->src;
    o.name = name;
    o.progress = onPackageProgress;
    o.log = onPackageLog;
    o.data = job;
    o.cancel = &job->cancel;
    jobProgress(job, "install", 0, info->size, "", TRUE);
    int r = ps5pkginstall_run(&o, &job->code, error, size);
    if (r == -1 && info->fake && job->code && !ps5pkginstall_hint(job->code)) {
        size_t n = strlen(error);
        snprintf(error + n, size - n, ". A fake package installs only with the console's fPKG patches on (kstuff with fpkg enabled)");
    }
    if (!r)
        snprintf(note, noteSize, "The console has the package: it installs it from its downloads (see Notifications)");
    return r == -3 ? -2 : r;
}

/* An install whose src is a file: an image or a package. */
static int installFile(Job* job, PS5TitleInfo* title, gboolean* haveTitle, char* out, size_t outSize, char* note, size_t noteSize,
    char* error, size_t size)
{
    PS5ImageInfo info;
    char why[256] = "";
    if (ps5image_info(job->src, &info, why, sizeof why)) {
        snprintf(error, size, "Not an app, image or package that can be installed (%s)", why);
        return -1;
    }
    memset(title, 0, sizeof *title);
    snprintf(title->title_id, sizeof title->title_id, "%s", info.title_id);
    snprintf(title->name, sizeof title->name, "%s", *info.name ? info.name : info.title_id);
    snprintf(title->version, sizeof title->version, "%s", info.version);
    snprintf(title->content_id, sizeof title->content_id, "%s", info.content_id);
    snprintf(title->dir, sizeof title->dir, "%s", job->src);
    *haveTitle = *info.title_id != 0;
    if (info.package)
        return installPackage(job, &info, note, noteSize, error, size);
    if (info.kind != PS5IMAGE_EXFAT && info.kind != PS5IMAGE_UFS2) {
        snprintf(error, size, "A %s can't be installed from here: only exFAT and UFS2 (.ffpkg) images, and packages", info.label);
        return -1;
    }
    if (refuseReplace(job, title, job->src, error, size))
        return -1;
    return installImage(job, &info, title, out, outSize, error, size);
}

static gpointer jobThread(gpointer data)
{
    Job* job = data;
    g_mutex_lock(&jobLock);
    while (jobsRunning >= JOBS_AT_ONCE && !job->cancel)
        g_cond_wait(&jobSlot, &jobLock);
    jobsRunning++;
    g_mutex_unlock(&jobLock);

    char error[512] = "";
    char note[512] = ""; /* done, but something to say ("the original was kept") */
    char out[1024] = "";
    PS5TitleInfo title;
    gboolean haveTitle = FALSE;
    gboolean ok = FALSE;
    jobProgress(job, job->kind, 0, 0, "", TRUE);
    if (job->cancel)
        snprintf(error, sizeof error, "Cancelled");
    else if (!strcmp(job->kind, "extract") && job->then && !strcmp(job->then, "install") && !job->replace
        && archiveTitle(job->src, job->password, &title) && refuseReplace(job, &title, job->dest, error, sizeof error)) {
        haveTitle = TRUE; /* nothing unpacked */
    } else if (!strcmp(job->kind, "extract") && job->then && !strcmp(job->then, "image")
        && !archiveOneApp(job->src, job->password, error, sizeof error)) {
        /* nothing unpacked */
    } else if (!strcmp(job->kind, "extract")) {
        PS5ArchiveLimits limits = { .password = job->password };
        if (exists(job->dest)) {
            /* never into a folder that is there already: "name (1)" */
            char* parent = g_path_get_dirname(job->dest);
            char* base = g_path_get_basename(job->dest);
            char* free = uniqueFolder(parent, base);
            g_free(job->dest);
            job->dest = g_build_filename(parent, free, NULL);
            g_free(parent);
            g_free(base);
            g_free(free);
        }
        gboolean made = !exists(job->dest);
        int r = ps5archive_extract(job->src, job->dest, &limits, onExtractProgress, job, &job->cancel, error, sizeof error);
        if (r && made && r != -1)
            ps5install_remove_tree(job->dest);
        if (r == -4) /* the title asks for it, and sends the job again */
            job->passwordAsk = job->password && *job->password ? "wrong" : "needed";
        ok = !r;
        if (ok) {
            snprintf(out, sizeof out, "%s", job->dest);
            if (job->deleteSource)
                deleteVolumes(job->src);
        }
        if (ok && job->then && !strcmp(job->then, "install")) {
            jobProgress(job, "install", 0, 0, "", TRUE);
            char found[256] = "";
            ok = !ps5install_find(job->dest, 3, &title, found, sizeof found);
            if (!ok)
                snprintf(error, sizeof error, "Unpacked to %s, but %s", job->dest, found);
            else if (refuseReplace(job, &title, title.dir, error, sizeof error)) {
                /* the archive's param.json couldn't be read first: the
                 * unpacked folder stays, to install from Files */
                ok = FALSE;
                haveTitle = TRUE;
                snprintf(out, sizeof out, "%s", job->dest);
            } else {
                ok = !installTitle(job, &title, out, sizeof out, error, sizeof error);
                haveTitle = TRUE;
                /* the unpacked folder's rest (the title moved out of it) */
                if (ok && strcmp(job->dest, out) && strncmp(out, job->dest, strlen(job->dest)))
                    ps5install_remove_tree(job->dest);
            }
        } else if (ok && !job->then) {
            /* exactly one app in it: the title offers to install it */
            char why[256] = "";
            PS5TitleInfo app;
            if (oneAppIn(job->dest, why, sizeof why) && !ps5install_find(job->dest, 3, &app, why, sizeof why))
                snprintf(job->app, sizeof job->app, "%s", app.dir);
        } else if (ok && job->then && !strcmp(job->then, "image") && job->image) {
            /* exactly one app in what was unpacked; else it stays unpacked */
            char found[256] = "", why[256] = "";
            if (!oneAppIn(job->dest, why, sizeof why) || ps5install_find(job->dest, 3, &title, found, sizeof found)) {
                ok = FALSE;
                snprintf(error, sizeof error, "Unpacked to %s, but %s", job->dest, *why ? why : found);
                snprintf(out, sizeof out, "%s", job->dest);
            } else {
                haveTitle = TRUE;
                ok = buildImage(job, title.dir, &title, job->image, out, sizeof out, error, sizeof error);
                if (ok) {
                    /* the unpacked copy was only for the image (a big
                     * one takes a while: a step of its own) */
                    jobProgress(job, "cleanup", 0, 0, "", TRUE);
                    ps5install_remove_tree(job->dest);
                }
            }
        }
    } else if (!strcmp(job->kind, "install") && isRegularFile(job->src)) {
        ok = !installFile(job, &title, &haveTitle, out, sizeof out, note, sizeof note, error, sizeof error);
        if (ok && !*out)
            snprintf(out, sizeof out, "%s", job->src);
    } else if (!strcmp(job->kind, "install")) {
        ok = !ps5install_find(job->src, 3, &title, error, sizeof error);
        if (ok && refuseReplace(job, &title, title.dir, error, sizeof error)) {
            ok = FALSE;
            haveTitle = TRUE;
        } else if (ok) {
            ok = !installTitle(job, &title, out, sizeof out, error, sizeof error);
            haveTitle = TRUE;
        }
    } else if (!strcmp(job->kind, "image")) {
        /* exactly one app: its folder becomes the image */
        if (oneAppIn(job->src, error, sizeof error) && !ps5install_find(job->src, 3, &title, error, sizeof error)) {
            haveTitle = TRUE;
            ok = buildImage(job, title.dir, &title, job->dest, out, sizeof out, error, sizeof error);
        }
    } else if (!strcmp(job->kind, "delete")) {
        /* the file manager's Delete: a job, as a big folder takes a while */
        ok = !ps5install_remove_tree(job->src);
        if (!ok)
            snprintf(error, sizeof error, "Couldn't delete all of it (errno %d)", errno);
    } else if (!strcmp(job->kind, "move")) {
        int r = ps5files_transfer(job->src, job->dest, TRUE, 0, onTransferProgress, job, &job->cancel, error, sizeof error);
        ok = !r;
        if (ok) {
            snprintf(out, sizeof out, "%s", job->dest);
            snprintf(note, sizeof note, "%s", error); /* "Copied, but ..." */
        }
    } else if (!strcmp(job->kind, "copy")) {
        int r = ps5files_transfer(job->src, job->dest, FALSE, 0, onTransferProgress, job, &job->cancel, error, sizeof error);
        ok = !r;
        if (ok)
            snprintf(out, sizeof out, "%s", job->dest);
    } else
        snprintf(error, sizeof error, "No such job: %s", job->kind);
    if (!ok && job->cancel && !*error)
        snprintf(error, sizeof error, "Cancelled");
    jobEnd(job, ok, ok ? (*note ? note : NULL) : (*error ? error : "Failed"), *out ? out : NULL, haveTitle ? &title : NULL);

    g_mutex_lock(&jobLock);
    jobsRunning--;
    g_cond_broadcast(&jobSlot);
    g_hash_table_remove(jobs, job->id);
    g_mutex_unlock(&jobLock);
    g_free(job->id);
    g_free(job->kind);
    g_free(job->src);
    g_free(job->dest);
    g_free(job->then);
    g_free(job->image);
    g_free(job->format);
    g_free(job->launcher);
    if (job->password) {
        memset(job->password, 0, strlen(job->password));
        g_free(job->password);
    }
    g_free(job);
    return NULL;
}

/* Whether a and b are the same path or one is inside the other. */
static gboolean overlaps(const char* a, const char* b)
{
    if (!a || !b)
        return FALSE;
    size_t la = strlen(a), lb = strlen(b);
    size_t n = la < lb ? la : lb;
    return !strncmp(a, b, n) && (la == lb || (la < lb ? b[la] == '/' : a[lb] == '/'));
}

/* A running job or download using src or dest (moved, copied, deleted,
 * unpacked or written to): a second job on the same files would race it.
 * jobLock held. */
static const char* busyWith(const char* src, const char* dest)
{
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, jobs);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const Job* other = value;
        const char* theirs[] = { other->src, other->dest && strcmp(other->dest, "auto") ? other->dest : NULL };
        for (unsigned i = 0; i < G_N_ELEMENTS(theirs); i++)
            if (overlaps(theirs[i], src) || (dest && overlaps(theirs[i], dest)))
                return "Another job is using it: wait for that one";
    }
    const char* why = NULL;
    g_mutex_lock(&fetchLock);
    g_hash_table_iter_init(&iter, fetches);
    while (!why && g_hash_table_iter_next(&iter, &key, &value)) {
        const Fetch* f = value;
        if (f->ended || !f->path)
            continue;
        char* part = g_strconcat(f->path, ".part", NULL);
        if (overlaps(f->path, src) || overlaps(part, src) || (dest && (overlaps(f->path, dest) || overlaps(part, dest))))
            why = "A download is writing there";
        g_free(part);
    }
    g_mutex_unlock(&fetchLock);
    return why;
}

static void startJob(GHashTable* p)
{
    const char* id = param(p, "id");
    const char* kind = param(p, "kind");
    const char* src = param(p, "src");
    const char* dest = param(p, "dest");
    const char* image = param(p, "image");
    const char* format = param(p, "format");
    const char* refused = NULL;
    if (!id || !kind || !src || !allowedPath(src))
        refused = "The job's file isn't in a place the browser can use";
    else if (format && strcmp(format, "exfat") && strcmp(format, "ffpkg"))
        refused = "Images are exFAT or UFS2 (.ffpkg)";
    else if (!strcmp(kind, "delete") && !changeablePath(src))
        refused = "That can't be deleted from here";
    else if (strcmp(kind, "install") && strcmp(kind, "delete")
        && (!dest || (!writablePath(dest) && !(!strcmp(kind, "image") && !strcmp(dest, "auto")))))
        refused = "The job's destination isn't in a place the browser can use";
    else if (image && strcmp(image, "auto") && !writablePath(image))
        refused = "The image's place isn't one the browser can use";
    else if ((!strcmp(kind, "move") || !strcmp(kind, "copy")) && !changeablePath(src))
        refused = !strcmp(kind, "move") ? "That can't be moved" : "That can't be copied";
    g_mutex_lock(&jobLock);
    if (!refused && g_hash_table_contains(jobs, id))
        refused = "A job is already running for it";
    if (!refused)
        refused = busyWith(src, dest && strcmp(dest, "auto") ? dest : NULL);
    Job* job = NULL;
    if (!refused) {
        job = g_new0(Job, 1);
        job->id = g_strdup(id);
        job->kind = g_strdup(kind);
        job->src = g_strdup(src);
        job->dest = g_strdup(dest);
        job->then = g_strdup(param(p, "then"));
        job->image = g_strdup(image);
        job->format = g_strdup(format);
        job->deleteSource = param(p, "delete") && !strcmp(param(p, "delete"), "1");
        job->replace = param(p, "replace") && !strcmp(param(p, "replace"), "1");
        job->password = param(p, "password") ? g_strdup(param(p, "password")) : NULL;
        job->launcher = g_strdup(param(p, "launcher"));
        g_hash_table_insert(jobs, job->id, job);
    }
    g_mutex_unlock(&jobLock);
    if (refused) {
        GString* line = startLine("jobend");
        appendParam(line, "id", id ? id : "");
        appendNumber(line, "ok", 0);
        appendParam(line, "error", refused);
        sendLine(line);
        return;
    }
    JOBLOG("job %s: %s %s -> %s%s%s%s", id, kind, src, dest ? dest : "-", job->then ? " then " : "", job->then ? job->then : "",
        job->password ? " (with a password)" : "");
    GThread* thread = g_thread_try_new("dl-job", jobThread, job, NULL);
    if (thread)
        g_thread_unref(thread);
    else {
        g_mutex_lock(&jobLock);
        g_hash_table_remove(jobs, id);
        g_mutex_unlock(&jobLock);
        GString* line = startLine("jobend");
        appendParam(line, "id", id);
        appendNumber(line, "ok", 0);
        appendParam(line, "error", "The job couldn't start");
        sendLine(line);
    }
}

static void stopJob(const char* id)
{
    g_mutex_lock(&jobLock);
    Job* job = id ? g_hash_table_lookup(jobs, id) : NULL;
    if (job) {
        job->cancel = 1;
        g_cond_broadcast(&jobSlot);
    }
    g_mutex_unlock(&jobLock);
}

/* ---------------------------------------------------------------- requests */

static void driveJSON(GString* out, const char* path, const char* label, const char* kind)
{
    struct statvfs vfs;
    if (statvfs(path, &vfs) || !vfs.f_blocks)
        return;
    if (strcmp(path, "/data")) {
        /* a mount point that holds nothing mounted has /mnt's device */
        struct stat here, parent;
        if (stat(path, &here) || stat("/mnt", &parent) || here.st_dev == parent.st_dev)
            return;
    }
    if (out->len > 1)
        g_string_append_c(out, ',');
    g_string_append(out, "{\"path\":");
    jsonString(out, path);
    g_string_append(out, ",\"label\":");
    jsonString(out, label);
    g_string_append(out, ",\"kind\":");
    jsonString(out, kind);
    g_string_append_printf(out, ",\"free\":%llu,\"size\":%llu,\"writable\":%s", (unsigned long long)vfs.f_bavail * vfs.f_frsize,
        (unsigned long long)vfs.f_blocks * vfs.f_frsize, access(path, W_OK) ? "false" : "true");
    struct statfs fs;
    if (!statfs(path, &fs)) {
        g_string_append(out, ",\"fs\":");
        jsonString(out, fs.f_fstypename);
    }
    g_string_append_c(out, '}');
}

static char* rootsJSON(void)
{
    GString* out = g_string_new("[");
    driveJSON(out, "/data", "Internal storage", "internal");
    for (int i = 0; i < 2; i++) {
        char path[16], label[32];
        snprintf(path, sizeof path, "/mnt/ext%d", i);
        snprintf(label, sizeof label, "Extended storage %d", i + 1);
        driveJSON(out, path, label, "ext");
    }
    for (int i = 0; i < 8; i++) {
        char path[16], label[32];
        snprintf(path, sizeof path, "/mnt/usb%d", i);
        snprintf(label, sizeof label, "USB drive %d", i + 1);
        driveJSON(out, path, label, "usb");
    }
    g_string_append_c(out, ']');
    return g_string_free(out, FALSE);
}

static gboolean isTitleFolder(const char* path)
{
    char a[1100], b[1100];
    snprintf(a, sizeof a, "%s/sce_sys/param.json", path);
    snprintf(b, sizeof b, "%s/eboot.bin", path);
    struct stat st;
    return !stat(a, &st) && !stat(b, &st);
}

static int compareEntries(gconstpointer a, gconstpointer b)
{
    return g_utf8_collate(*(const char* const*)a, *(const char* const*)b);
}

/* A folder's entries: folders first, then files, by name. */
static char* listJSON(const char* path, char* error, size_t size)
{
    DIR* folder = opendir(path);
    if (!folder) {
        snprintf(error, size, errno == ENOENT ? "That folder isn't there" : "Couldn't open the folder (errno %d)", errno);
        return NULL;
    }
    GPtrArray* names = g_ptr_array_new_with_free_func(g_free);
    struct dirent* entry;
    while ((entry = readdir(folder)) && names->len < 5000) {
        if (entry->d_name[0] == '.' && (!entry->d_name[1] || (entry->d_name[1] == '.' && !entry->d_name[2])))
            continue;
        g_ptr_array_add(names, g_strdup(entry->d_name));
    }
    closedir(folder);
    g_ptr_array_sort(names, compareEntries);
    GString* dirs = g_string_new(NULL);
    GString* files = g_string_new(NULL);
    for (unsigned i = 0; i < names->len; i++) {
        const char* name = g_ptr_array_index(names, i);
        char* full = g_build_filename(path, name, NULL);
        struct stat st;
        if (lstat(full, &st) || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) {
            g_free(full);
            continue;
        }
        gboolean dir = S_ISDIR(st.st_mode);
        GString* out = dir ? dirs : files;
        g_string_append(out, out->len ? ",{\"name\":" : "{\"name\":");
        jsonString(out, name);
        g_string_append_printf(out, ",\"dir\":%s,\"size\":%lld,\"mtime\":%lld", dir ? "true" : "false", dir ? 0LL : (long long)st.st_size,
            (long long)st.st_mtime);
        if (dir && isTitleFolder(full)) {
            char* icon = g_build_filename(full, "sce_sys", "icon0.png", NULL);
            struct stat is;
            g_string_append_printf(out, ",\"app\":true,\"icon\":%s", !stat(icon, &is) && S_ISREG(is.st_mode) ? "true" : "false");
            g_free(icon);
        }
        if (!dir && ps5archive_is_archive(name))
            g_string_append(out, ",\"archive\":true");
        if (!dir && ps5image_is_image_name(name))
            g_string_append(out, ",\"image\":true");
        if (!dir && ps5image_is_pkg_name(name))
            g_string_append(out, ",\"pkg\":true");
        if (!dir && g_str_has_suffix(name, ".ps5dl"))
            g_string_append(out, ",\"state\":true");
        g_string_append_c(out, '}');
        g_free(full);
    }
    g_ptr_array_unref(names);
    GString* json = g_string_new("{\"path\":");
    jsonString(json, path);
    struct statvfs vfs;
    if (!statvfs(path, &vfs))
        g_string_append_printf(json, ",\"free\":%llu", (unsigned long long)vfs.f_bavail * vfs.f_frsize);
    g_string_append_printf(json, ",\"changeable\":%s,\"entries\":[%s%s%s]}", changeablePath(path) ? "true" : "false", dirs->str,
        dirs->len && files->len ? "," : "", files->str);
    g_string_free(dirs, TRUE);
    g_string_free(files, TRUE);
    return g_string_free(json, FALSE);
}

/* "name.zip", else "name (1).zip"... free in folder (neither it nor its
 * .part there). */
static char* uniqueName(const char* folder, const char* name)
{
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
    for (unsigned n = 0; n < 10000; n++) {
        char* candidate = n ? g_strdup_printf("%.*s (%u)%s", (int)(extension - name), name, n, extension) : g_strdup(name);
        char* full = g_strdup_printf("%s/%s", folder, candidate);
        gboolean taken = pathTaken(full);
        g_free(full);
        if (!taken)
            return candidate;
        g_free(candidate);
    }
    return g_strdup_printf("download-%lld", (long long)time(NULL));
}

/* "name", else "name (1)"... not there in parent. */
static char* uniqueFolder(const char* parent, const char* name)
{
    for (unsigned n = 0; n < 10000; n++) {
        char* candidate = n ? g_strdup_printf("%s (%u)", name, n) : g_strdup(name);
        char* full = g_build_filename(parent, candidate, NULL);
        gboolean taken = exists(full);
        g_free(full);
        if (!taken)
            return candidate;
        g_free(candidate);
    }
    return g_strdup_printf("%s-%lld", name, (long long)time(NULL));
}

static gboolean validName(const char* name)
{
    if (!name || !*name || name[0] == '.' || strlen(name) > 200)
        return FALSE;
    for (const unsigned char* c = (const unsigned char*)name; *c; c++) {
        if (*c < 0x20 || strchr("/\\:*?\"<>|", *c))
            return FALSE;
    }
    return TRUE;
}

/* One unfinished download at file (its .part and .part.ps5dl beside it,
 * file itself not there) as JSON into out: TRUE if it is one. A real
 * download's: a regular .part and a small regular state file (a page
 * can't plant one: downloads named *.part or *.ps5dl are refused). */
static gboolean partialJSON(const char* file, GString* out)
{
    char* dir = g_path_get_dirname(file);
    char* base = g_path_get_basename(file);
    char* partFile = g_strconcat(file, ".part", NULL);
    char* stateFile = g_strconcat(file, ".part.ps5dl", NULL);
    struct stat ps, ss;
    gboolean real = allowedPath(file) && validName(base) && !exists(file) && !lstat(partFile, &ps) && S_ISREG(ps.st_mode)
        && !lstat(stateFile, &ss) && S_ISREG(ss.st_mode) && ss.st_size < 64 * 1024;
    uint64_t received = 0;
    int64_t total = -1;
    char* url = NULL;
    if (real && ps5fetch_saved(file, &received, &total) && (url = ps5fetch_saved_url(file))) {
        g_string_append(out, "{\"path\":");
        jsonString(out, file);
        g_string_append(out, ",\"dir\":");
        jsonString(out, dir);
        g_string_append(out, ",\"name\":");
        jsonString(out, base);
        g_string_append(out, ",\"url\":");
        jsonString(out, url);
        g_string_append_printf(out, ",\"received\":%llu,\"total\":%lld,\"mtime\":%lld}", (unsigned long long)received, (long long)total,
            (long long)ss.st_mtime);
    } else
        real = FALSE;
    free(url);
    g_free(partFile);
    g_free(stateFile);
    g_free(dir);
    g_free(base);
    return real;
}

/* The unfinished downloads in dirs (one folder a line): each
 * <name>.part.ps5dl whose <name> isn't there, with what it saved and the
 * address it came from, so the title can offer to continue those its list
 * has lost. */
#define PARTIALS_DIRS_MAX 64
#define PARTIALS_PER_DIR 5000
static char* partialsJSON(const char* dirs)
{
    GString* out = g_string_new("[");
    char** list = g_strsplit(dirs ? dirs : "", "\n", PARTIALS_DIRS_MAX + 1);
    gboolean first = TRUE;
    for (unsigned i = 0; list[i] && i < PARTIALS_DIRS_MAX; i++) {
        const char* dir = list[i];
        if (!*dir || !allowedPath(dir))
            continue;
        DIR* folder = opendir(dir);
        if (!folder)
            continue;
        struct dirent* entry;
        unsigned seen = 0;
        while ((entry = readdir(folder)) && seen++ < PARTIALS_PER_DIR) {
            const char* name = entry->d_name;
            size_t length = strlen(name);
            static const char tail[] = ".part.ps5dl";
            if (length <= sizeof tail - 1 || strcmp(name + length - (sizeof tail - 1), tail))
                continue;
            char* base = g_strndup(name, length - (sizeof tail - 1));
            char* file = g_build_filename(dir, base, NULL);
            GString* one = g_string_new(NULL);
            if (partialJSON(file, one)) {
                g_string_append(out, first ? "" : ",");
                g_string_append(out, one->str);
                first = FALSE;
            }
            g_string_free(one, TRUE);
            g_free(file);
            g_free(base);
        }
        closedir(folder);
    }
    g_strfreev(list);
    g_string_append_c(out, ']');
    return g_string_free(out, FALSE);
}

/* Whether ShadowMountPlus has installed the app of the image at path:
 * it writes /user/app/<ID>/mount_img.lnk (the image's path) when it does
 * (2026-11-21, 1.7beta7), about 10-30 s after the image stops changing.
 * A link from before (the image made again in the same place) counts: the
 * app on the home screen starts from this image. */
static char* imageAppJSON(const char* image, const char* title)
{
    gboolean installed = FALSE;
    gboolean validID = title && strlen(title) == 9;
    for (int i = 0; validID && i < 9; i++)
        validID = i < 4 ? g_ascii_isupper(title[i]) : g_ascii_isdigit(title[i]);
    if (validID) {
        char link[128];
        snprintf(link, sizeof link, "/user/app/%s/mount_img.lnk", title);
        char* text = NULL;
        struct stat is;
        if (!stat(image, &is) && g_file_get_contents(link, &text, NULL, NULL)) {
            g_strstrip(text);
            installed = !strcmp(text, image);
        }
        g_free(text);
    }
    return g_strdup_printf("{\"installed\":%s}", installed ? "true" : "false");
}

/* The launchers running (ps5launchers_json), with the setting's choice
 * applied. */
static char* launchersJSON(const char* setting)
{
    PS5Launchers* l = g_new0(PS5Launchers, 1);
    ps5launchers_detect(l, NULL);
    ps5launchers_force(l, setting);
    char* json = ps5launchers_json(l);
    g_free(l);
    return json;
}

/* Whether the launcher has registered the app installed at folder (op
 * "appready", after a jobend with "by"): {"installed":…,"error":…}. */
static char* appReadyJSON(const char* folder, const char* title, const char* setting)
{
    gboolean validID = title && strlen(title) == 9;
    for (int i = 0; validID && i < 9; i++)
        validID = i < 4 ? g_ascii_isupper(title[i]) : g_ascii_isdigit(title[i]);
    char error[256] = "";
    int ready = 0;
    if (validID) {
        PS5Launchers* l = g_new0(PS5Launchers, 1);
        ps5launchers_detect(l, NULL);
        ps5launchers_force(l, setting);
        ready = ps5launchers_app_ready(l, folder, title, NULL, error, sizeof error);
        g_free(l);
    }
    GString* out = g_string_new(NULL);
    g_string_append_printf(out, "{\"installed\":%s,\"error\":", ready > 0 ? "true" : "false");
    if (ready < 0)
        jsonString(out, error);
    else
        g_string_append(out, "null");
    g_string_append_c(out, '}');
    return g_string_free(out, FALSE);
}

/* Where op "icon" puts the icons it copies out (the title reads them). */
#define ICON_CACHE "/data/ps5webkit/dl/icons"
#define ICON_MAX (4 * 1024 * 1024)

/* What the image, package or app folder at path holds (op "titleinfo"):
 * ps5image_json, plus "installed": the version installed with that ID
 * ("?" not known), or null. */
static char* titleInfoJSON(const char* path, char* error, size_t size)
{
    struct stat st;
    if (stat(path, &st)) {
        snprintf(error, size, "It isn't there");
        return NULL;
    }
    PS5ImageInfo info;
    memset(&info, 0, sizeof info);
    if (S_ISDIR(st.st_mode)) {
        PS5TitleInfo t;
        char why[256];
        if (!isTitleFolder(path) || ps5install_find(path, 0, &t, why, sizeof why)) {
            snprintf(error, size, "No app in this folder (sce_sys/param.json and eboot.bin)");
            return NULL;
        }
        info.format = "folder";
        info.label = "App folder";
        snprintf(info.platform, sizeof info.platform, "PS5");
        snprintf(info.title_id, sizeof info.title_id, "%s", t.title_id);
        snprintf(info.name, sizeof info.name, "%s", strcmp(t.name, t.title_id) ? t.name : "");
        snprintf(info.version, sizeof info.version, "%s", t.version);
        snprintf(info.content_id, sizeof info.content_id, "%s", t.content_id);
        snprintf(info.category, sizeof info.category, "%s", t.category);
        info.app_at_root = true;
        char* icon = g_build_filename(path, "sce_sys", "icon0.png", NULL);
        info.has_icon = isRegularFile(icon);
        g_free(icon);
    } else if (ps5image_info(path, &info, error, size))
        return NULL;
    char* json = ps5image_json(&info);
    if (!json) {
        snprintf(error, size, "Out of memory");
        return NULL;
    }
    GString* out = g_string_new_len(json, (gssize)strlen(json) - 1); /* without its '}' */
    free(json);
    /* a folder the app expects files added to can't take them in an image */
    char empty[512];
    if (S_ISDIR(st.st_mode) && ps5install_empty_folder(path, empty, sizeof empty) == 1) {
        g_string_append(out, ",\"emptyFolder\":");
        jsonString(out, empty);
    }
    PS5TitleInfo installed;
    if (*info.title_id && ps5install_installed(NULL, info.title_id, &installed)) {
        g_string_append(out, ",\"installed\":");
        jsonString(out, *installed.version ? installed.version : "?");
    } else
        g_string_append(out, ",\"installed\":null");
    g_string_append_c(out, '}');
    return g_string_free(out, FALSE);
}

/* The icon of the image, package or app folder at path, copied to the
 * icon cache (op "icon"): {"path": the PNG}. Named after the file and its
 * size and time, so a changed file gets a new one. */
static char* iconJSON(const char* path, char* error, size_t size)
{
    struct stat st;
    if (stat(path, &st)) {
        snprintf(error, size, "It isn't there");
        return NULL;
    }
    char* key = g_strdup_printf("%s\n%lld\n%lld", path, (long long)st.st_size, (long long)st.st_mtime);
    char* hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, key, -1);
    g_free(key);
    char out[256];
    snprintf(out, sizeof out, "%s/%.24s.png", ICON_CACHE, hash);
    g_free(hash);
    g_mkdir_with_parents(ICON_CACHE, 0777);
    if (!isRegularFile(out)) {
        if (S_ISDIR(st.st_mode)) {
            char* icon = g_build_filename(path, "sce_sys", "icon0.png", NULL);
            char* bytes = NULL;
            gsize length = 0;
            struct stat is;
            /* its size first: a huge file isn't read in to be refused */
            gboolean ok = !stat(icon, &is) && S_ISREG(is.st_mode) && is.st_size > 8 && is.st_size <= ICON_MAX
                && g_file_get_contents(icon, &bytes, &length, NULL) && length > 8 && length <= ICON_MAX
                && !memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) && g_file_set_contents(out, bytes, (gssize)length, NULL);
            g_free(icon);
            g_free(bytes);
            if (!ok) {
                snprintf(error, size, "No icon0.png that can be read in the app's sce_sys");
                return NULL;
            }
        } else if (ps5image_icon(path, out, error, size))
            return NULL;
    }
    GString* json = g_string_new("{\"path\":");
    jsonString(json, out);
    g_string_append_c(json, '}');
    return g_string_free(json, FALSE);
}

static void request(GHashTable* p)
{
    const char* n = param(p, "n");
    const char* op = param(p, "op");
    const char* path = param(p, "path");
    char error[256] = "";
    char* json = NULL;
    if (!op)
        snprintf(error, sizeof error, "op missing");
    else if (!strcmp(op, "roots"))
        json = rootsJSON();
    else if (!strcmp(op, "launchers"))
        json = launchersJSON(param(p, "launcher"));
    else if (!strcmp(op, "partials"))
        json = partialsJSON(param(p, "dirs"));
    else if (!strcmp(op, "partial")) {
        /* one, to continue it: what the title adopts comes from here, not
         * from the page */
        GString* one = g_string_new(NULL);
        if (path && partialJSON(path, one))
            json = g_string_free(one, FALSE);
        else {
            g_string_free(one, TRUE);
            snprintf(error, sizeof error, "That isn't an unfinished download (any more)");
        }
    }
    else if (!allowedPath(path))
        snprintf(error, sizeof error, "That place isn't one the browser can use");
    else if (!strcmp(op, "imageapp"))
        json = imageAppJSON(path, param(p, "title"));
    else if (!strcmp(op, "titleinfo"))
        json = titleInfoJSON(path, error, sizeof error);
    else if (!strcmp(op, "icon"))
        json = iconJSON(path, error, sizeof error);
    else if (!strcmp(op, "appready"))
        json = appReadyJSON(path, param(p, "title"), param(p, "launcher"));
    else if (!strcmp(op, "ls"))
        json = listJSON(path, error, sizeof error);
    else if (!strcmp(op, "mkdir")) {
        const char* name = param(p, "name");
        char* full = validName(name) ? g_build_filename(path, name, NULL) : NULL;
        if (!writableFolder(path))
            snprintf(error, sizeof error, "New folders can't be made there");
        else if (!full)
            snprintf(error, sizeof error, "A folder's name can't be empty or hold / \\ : * ? \" < > |");
        else if (exists(full))
            snprintf(error, sizeof error, "There is already something called %s there", name);
        else if (g_mkdir_with_parents(full, 0777))
            snprintf(error, sizeof error, "Couldn't make the folder (errno %d)", errno);
        else {
            chmod(full, 0777);
            GString* out = g_string_new("{\"path\":");
            jsonString(out, full);
            g_string_append_c(out, '}');
            json = g_string_free(out, FALSE);
        }
        g_free(full);
    } else if (!strcmp(op, "delete")) {
        if (!changeablePath(path))
            snprintf(error, sizeof error, "That can't be deleted from here");
        else if (ps5install_remove_tree(path))
            snprintf(error, sizeof error, "Couldn't delete all of it (errno %d)", errno);
        else
            json = g_strdup("{}");
    } else if (!strcmp(op, "rename")) {
        const char* name = param(p, "name");
        char* folder = g_path_get_dirname(path);
        char* full = validName(name) ? g_build_filename(folder, name, NULL) : NULL;
        if (!changeablePath(path))
            snprintf(error, sizeof error, "That can't be renamed");
        else if (!full)
            snprintf(error, sizeof error, "A name can't be empty or hold / \\ : * ? \" < > |");
        else if (exists(full))
            snprintf(error, sizeof error, "There is already something called %s there", name);
        else if (rename(path, full))
            snprintf(error, sizeof error, "Couldn't rename it (errno %d)", errno);
        else {
            GString* out = g_string_new("{\"path\":");
            jsonString(out, full);
            g_string_append_c(out, '}');
            json = g_string_free(out, FALSE);
        }
        g_free(folder);
        g_free(full);
    } else if (!strcmp(op, "unique")) {
        const char* name = param(p, "name");
        if (!validName(name))
            snprintf(error, sizeof error, "Bad name");
        else {
            g_mkdir_with_parents(path, 0777);
            g_mutex_lock(&fetchLock);
            char* free = uniqueName(path, name);
            g_mutex_unlock(&fetchLock);
            GString* out = g_string_new("{\"name\":");
            jsonString(out, free);
            g_string_append_c(out, '}');
            json = g_string_free(out, FALSE);
            g_free(free);
        }
    } else if (!strcmp(op, "saved")) {
        uint64_t received = 0;
        int64_t total = -1;
        gboolean saved = ps5fetch_saved(path, &received, &total);
        struct stat st;
        gboolean done = !stat(path, &st) && S_ISREG(st.st_mode);
        json = g_strdup_printf("{\"saved\":%s,\"received\":%llu,\"total\":%lld,\"done\":%s,\"size\":%lld}", saved ? "true" : "false",
            (unsigned long long)received, (long long)total, done ? "true" : "false", done ? (long long)st.st_size : -1LL);
    } else
        snprintf(error, sizeof error, "No such request: %s", op);
    GString* line = startLine("rep");
    appendParam(line, "n", n ? n : "");
    appendNumber(line, "ok", json != NULL);
    if (json)
        appendParam(line, "json", json);
    else
        appendParam(line, "error", error);
    g_free(json);
    sendLine(line);
}

static gpointer requestThread(gpointer data)
{
    GHashTable* p = data;
    request(p);
    g_hash_table_unref(p);
    return NULL;
}

/* "rm": a download's file, or its .part, and the .part.ps5dl beside it. */
static void removeDownloadFiles(const char* path)
{
    if (!path || !changeablePath(path)) {
        JOBLOG("rm refused: %s", path ? path : "(none)");
        return;
    }
    char* base = g_str_has_suffix(path, ".part") ? g_strndup(path, strlen(path) - 5) : g_strdup(path);
    char* part = g_strdup_printf("%s.part", base);
    char* state = g_strdup_printf("%s.part.ps5dl", base);
    const char* targets[] = { path, part, state };
    for (unsigned i = 0; i < G_N_ELEMENTS(targets); i++) {
        struct stat st;
        if (lstat(targets[i], &st) || !S_ISREG(st.st_mode))
            continue;
        if (unlink(targets[i]))
            JOBLOG("couldn't delete %s (errno %d)", targets[i], errno);
    }
    g_free(base);
    g_free(part);
    g_free(state);
}

/* ---------------------------------------------------------------- peeks */

/* A look inside an archive (ps5peek.c): what the download manager
 * suggests doing with it. A thread each, PEEKS_AT_ONCE at most. */
#define PEEKS_AT_ONCE 4

typedef struct {
    char* id;
    char* url;
    char* ua;
    char* ref;
    char* cookies;
    char* path;
    uint64_t size;
} Peek;

static gint peeksRunning;

static void peekFree(Peek* p)
{
    g_free(p->id);
    g_free(p->url);
    g_free(p->ua);
    g_free(p->ref);
    g_free(p->cookies);
    g_free(p->path);
    g_free(p);
}

static gpointer peekThread(gpointer data)
{
    Peek* p = data;
    PS5PeekOptions o = { 0 };
    o.url = p->url;
    o.size = p->size;
    o.user_agent = p->ua;
    o.referer = p->ref;
    o.cookies = p->cookies;
    o.path = p->path;
    char error[256] = "";
    char* json = ps5peek_json(&o, error, sizeof error);
    GString* line = startLine("peekend");
    appendParam(line, "id", p->id);
    appendNumber(line, "ok", json != NULL);
    if (json)
        appendParam(line, "json", json);
    else
        appendParam(line, "error", error);
    JOBLOG("peek %s: %s", p->id, json ? json : error);
    sendLine(line);
    free(json);
    peekFree(p);
    g_atomic_int_dec_and_test(&peeksRunning);
    return NULL;
}

static void startPeek(GHashTable* params)
{
    const char* id = param(params, "id");
    const char* url = param(params, "url");
    const char* path = param(params, "path");
    const char* size = param(params, "size");
    const char* refused = NULL;
    if (!id)
        return;
    if (url && !(g_str_has_prefix(url, "http://") || g_str_has_prefix(url, "https://")))
        refused = "Only http and https addresses";
    else if (url && (!size || g_ascii_strtoull(size, NULL, 10) == 0))
        refused = "The size is needed";
    else if (!url && (!path || !allowedPath(path)))
        refused = "That place isn't one the browser can use";
    else if (g_atomic_int_get(&peeksRunning) >= PEEKS_AT_ONCE)
        refused = "Busy: too many at once";
    if (refused) {
        GString* line = startLine("peekend");
        appendParam(line, "id", id);
        appendNumber(line, "ok", 0);
        appendParam(line, "error", refused);
        sendLine(line);
        return;
    }
    Peek* p = g_new0(Peek, 1);
    p->id = g_strdup(id);
    p->url = g_strdup(url);
    p->path = url ? NULL : g_strdup(path);
    p->ua = g_strdup(param(params, "ua"));
    p->ref = g_strdup(param(params, "ref"));
    p->cookies = g_strdup(param(params, "cookies"));
    p->size = url ? g_ascii_strtoull(size, NULL, 10) : 0;
    g_atomic_int_inc(&peeksRunning);
    g_thread_unref(g_thread_new("dl-peek", peekThread, p));
}

/* ---------------------------------------------------------------- the connection */

/* "k=v&k2=v2", percent-encoded: a table of new strings. Unlike
 * g_uri_parse_params, a value that isn't valid UTF-8 once decoded is
 * repaired (U+FFFD) instead of failing the whole line. */
static GHashTable* parseParams(const char* text)
{
    GHashTable* table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    char** pairs = g_strsplit(text ? text : "", "&", -1);
    for (char** pair = pairs; *pair; pair++) {
        if (!**pair)
            continue;
        char* equals = strchr(*pair, '=');
        char* key = g_uri_unescape_segment(*pair, equals ? equals : NULL, NULL);
        char* value = equals ? g_uri_unescape_string(equals + 1, NULL) : g_strdup("");
        if (key && value) {
            if (!g_utf8_validate(value, -1, NULL)) {
                char* valid = g_utf8_make_valid(value, -1);
                g_free(value);
                value = valid;
            }
            g_hash_table_replace(table, key, value);
        } else {
            g_free(key);
            g_free(value);
        }
    }
    g_strfreev(pairs);
    return table;
}

static void handleLine(char* text, gboolean* hello)
{
    char* space = strchr(text, ' ');
    if (space)
        *space = 0;
    const char* verb = text;
    GHashTable* p = parseParams(space ? space + 1 : "");
    if (!*hello) {
        *hello = !strcmp(verb, "hello") && param(p, "token") && !strcmp(param(p, "token"), token);
        if (!*hello)
            JOBLOG("a connection without the token: closed");
    } else if (!strcmp(verb, "fetch"))
        startFetch(p);
    else if (!strcmp(verb, "stop"))
        stopFetch(param(p, "id"), FALSE);
    else if (!strcmp(verb, "cancel"))
        stopFetch(param(p, "id"), TRUE);
    else if (!strcmp(verb, "job"))
        startJob(p);
    else if (!strcmp(verb, "jobstop"))
        stopJob(param(p, "id"));
    else if (!strcmp(verb, "req")) {
        /* a thread each: a big delete mustn't hold up the lines after it */
        GThread* thread = g_thread_try_new("dl-req", requestThread, g_hash_table_ref(p), NULL);
        if (thread)
            g_thread_unref(thread);
        else {
            request(p);
            g_hash_table_unref(p);
        }
    }
    else if (!strcmp(verb, "rm"))
        removeDownloadFiles(param(p, "path"));
    else if (!strcmp(verb, "peek"))
        startPeek(p);
    else
        JOBLOG("unknown line: %s", verb);
    g_hash_table_unref(p);
}

/* The connection's lines until it closes. TRUE if it was the title's
 * (it said the token). */
static gboolean serveClient(int fd)
{
    gboolean hello = FALSE;
    gboolean skipping = FALSE; /* in a line over LINE_MAX_BYTES: dropped */
    GString* pending = g_string_new(NULL);
    char buffer[16384];
    for (;;) {
        ssize_t n = recv(fd, buffer, sizeof buffer, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        g_string_append_len(pending, buffer, n);
        char* newline;
        while ((newline = memchr(pending->str, '\n', pending->len))) {
            *newline = 0;
            if (newline > pending->str && newline[-1] == '\r')
                newline[-1] = 0;
            gboolean before = hello;
            if (*pending->str && !skipping)
                handleLine(pending->str, &hello);
            skipping = FALSE;
            g_string_erase(pending, 0, newline - pending->str + 1);
            if (!hello) {
                g_string_free(pending, TRUE);
                return FALSE;
            }
            if (!before) {
                g_mutex_lock(&sendLock);
                client = fd;
                g_mutex_unlock(&sendLock);
                JOBLOG("the title is connected");
            }
        }
        if (pending->len > LINE_MAX_BYTES) {
            if (!hello)
                break;
            JOBLOG("a line over %d bytes: dropped", LINE_MAX_BYTES);
            g_string_truncate(pending, 0);
            skipping = TRUE;
        }
    }
    g_string_free(pending, TRUE);
    return hello;
}

/* Stops the downloads (their .part and .ps5dl stay) and the jobs, then
 * ends the process: the title starts a new service when it needs one. */
static void stopAllAndExit(const char* why)
{
    JOBLOG("%s: stopping the downloads and ending", why);
    g_mutex_lock(&fetchLock);
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, fetches);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        Fetch* f = value;
        if (f->again) {
            g_hash_table_unref(f->again);
            f->again = NULL;
        }
        if (!f->ended && f->fetch)
            ps5fetch_stop(f->fetch);
    }
    g_mutex_unlock(&fetchLock);
    g_mutex_lock(&jobLock);
    g_hash_table_iter_init(&iter, jobs);
    while (g_hash_table_iter_next(&iter, &key, &value))
        ((Job*)value)->cancel = 1;
    g_mutex_unlock(&jobLock);
    /* the workers write their .ps5dl as they stop: up to 3 s */
    for (int i = 0; i < 30; i++) {
        gboolean running = FALSE;
        g_mutex_lock(&fetchLock);
        g_hash_table_iter_init(&iter, fetches);
        while (g_hash_table_iter_next(&iter, &key, &value))
            running = running || !((Fetch*)value)->ended;
        g_mutex_unlock(&fetchLock);
        if (!running)
            break;
        g_usleep(100 * 1000);
    }
    fflush(stderr);
    _exit(3);
}

/* The title closed the task's connection: stop the downloads (their .part
 * and .ps5dl stay) and the jobs, then end. */
static void* stdinWatcher(void* data)
{
    (void)data;
    for (;;) {
        struct pollfd p = { 0, POLLIN, 0 };
        int n = poll(&p, 1, -1);
        if (n < 0 && errno == EINTR)
            continue;
        char c;
        if (n < 0 || (p.revents & (POLLHUP | POLLERR | POLLNVAL)) || read(0, &c, 1) <= 0)
            break;
    }
    stopAllAndExit("the title closed the task");
    return NULL;
}

int ps5wk_dl_job(const char* jobPath)
{
    signal(SIGPIPE, SIG_IGN);
    pthread_t watcher;
    if (!pthread_create(&watcher, NULL, stdinWatcher, NULL))
        pthread_detach(watcher);
    GKeyFile* file = g_key_file_new();
    if (!g_key_file_load_from_file(file, jobPath, G_KEY_FILE_NONE, NULL)) {
        JOBLOG("no job file %s", jobPath);
        return 1;
    }
    token = g_key_file_get_string(file, "job", "token", NULL);
    g_key_file_unref(file);
    unlink(jobPath); /* the token is read: no one else needs it */
    if (!token || strlen(token) < 16) {
        JOBLOG("no token in %s", jobPath);
        return 1;
    }
    fetches = g_hash_table_new(g_str_hash, g_str_equal);
    jobs = g_hash_table_new(g_str_hash, g_str_equal);

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = { 0 };
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    socklen_t length = sizeof address;
    if (listener < 0 || bind(listener, (struct sockaddr*)&address, sizeof address) || listen(listener, 2)
        || getsockname(listener, (struct sockaddr*)&address, &length)) {
        JOBLOG("no socket (errno %d)", errno);
        return 1;
    }
    GThread* ticker = g_thread_try_new("dl-progress", tickerThread, NULL, NULL);
    if (ticker)
        g_thread_unref(ticker);
    fprintf(stderr, "[dl-job] port: %u\n", (unsigned)ntohs(address.sin_port));
    fflush(stderr);
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            JOBLOG("accept failed (errno %d)", errno);
            g_usleep(200 * 1000);
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        gboolean title = serveClient(fd);
        g_mutex_lock(&sendLock);
        if (client == fd)
            client = -1;
        g_mutex_unlock(&sendLock);
        close(fd);
        /* the title takes a closed connection for an ended service and
         * starts another: this one mustn't go on writing the same files */
        if (title)
            stopAllAndExit("the title's connection closed");
    }
    return 0;
}
