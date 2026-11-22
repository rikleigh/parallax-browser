/* ps5pkginstall.c: a package served on 127.0.0.1 and handed to the
 * console's installer; see ps5pkginstall.h.
 *
 * The server's behaviour (one thread a connection, at most 16 at once and
 * 503 past that, keep-alive, HEAD, one byte range a request, the fixed
 * Last-Modified, the query the console adds ignored, done once every byte
 * was served and then 2 s quiet) follows PS5 Dump Forge's
 * crates/ps5-dump-forge-core/src/install.rs; the installer call its
 * ps5/installer.c (both GPL-3.0, quer3q/ps5-dump-forge; THIRD-PARTY-NOTICES.md).
 * The code is this project's own. A <name>.crc beside the package is
 * answered 404: the console installs without one.
 */
#include "ps5pkginstall.h"

#include "ps5install.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__PROSPERO__)
#include <ps5/kernel.h>
int sceKernelLoadStartModule(const char* path, size_t argc, const void* argv, uint32_t flags, void* opt, int* res);
#endif

#define URL_MAX 127
#define MAX_CONNS 16
#define HEAD_MAX (8 * 1024)
#define SEND_CHUNK (1u << 20)
#define IDLE_MS 30000 /* a kept-alive connection with no request for this long is closed */
#define STRANGER_MS 5000 /* ... one that never asked for the package: this long */
#define RANGES_MAX 4096
#define LAST_MODIFIED "Wed, 01 Jan 2025 00:00:00 GMT"

typedef struct {
    uint64_t start, end;
} Span;

typedef struct {
    const PS5PkgInstallOptions* o;
    int listener;
    char prefix[48];      /* "/<token>/" */
    char urlName[96];     /* "<name>.pkg" */
    char crcName[96];     /* "<name>.crc" */
    uint64_t total;
    dev_t dev;
    ino_t ino;
    time_t mtime;
    pthread_mutex_t lock;
    pthread_cond_t idle;  /* conns reached 0 */
    Span spans[RANGES_MAX]; /* bytes served, merged, sorted */
    unsigned spanCount;
    uint64_t covered;
    unsigned conns;
    unsigned requests;    /* for the package (the token's path) */
    unsigned seen;        /* any */
    double last;          /* the last request for the package, or byte of it sent */
    char failed[256];     /* the package changed or couldn't be read */
    volatile int stop;
} Server;

typedef struct {
    Server* s;
    int fd;
} Conn;

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static void logf_(const Server* s, const char* format, ...)
{
    if (!s->o->log)
        return;
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    s->o->log(line, s->o->data);
}

static void fail(char* error, size_t size, const char* format, ...)
{
    if (!error || !size)
        return;
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
}

/* Adds [start, end) to the bytes served. Lock held. */
static void cover(Server* s, uint64_t start, uint64_t end)
{
    if (start >= end)
        return;
    unsigned i = 0;
    while (i < s->spanCount && s->spans[i].end < start)
        i++;
    unsigned j = i;
    uint64_t lo = start, hi = end;
    while (j < s->spanCount && s->spans[j].start <= end) {
        if (s->spans[j].start < lo)
            lo = s->spans[j].start;
        if (s->spans[j].end > hi)
            hi = s->spans[j].end;
        j++;
    }
    if (i == j && s->spanCount == RANGES_MAX)
        return; /* too scattered to track; the next ones merge them */
    /* spans[i..j) become one */
    unsigned removed = j - i;
    if (removed == 0) {
        memmove(&s->spans[i + 1], &s->spans[i], (s->spanCount - i) * sizeof(Span));
        s->spanCount++;
    } else if (removed > 1) {
        memmove(&s->spans[i + 1], &s->spans[j], (s->spanCount - j) * sizeof(Span));
        s->spanCount -= removed - 1;
    }
    s->spans[i].start = lo;
    s->spans[i].end = hi;
    uint64_t c = 0;
    for (unsigned k = 0; k < s->spanCount; k++)
        c += s->spans[k].end - s->spans[k].start;
    s->covered = c;
}

static int sendAll(int fd, const void* bytes, size_t length, const Server* s)
{
    const char* p = bytes;
    while (length) {
        if (s->stop)
            return -1;
        struct pollfd w = { fd, POLLOUT, 0 };
        int r = poll(&w, 1, 200);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            return -1;
        if (!r)
            continue;
        ssize_t n = send(fd, p, length, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (n <= 0)
            return -1;
        p += n;
        length -= (size_t)n;
    }
    return 0;
}

static int respond(int fd, const Server* s, const char* status, uint64_t length, const char* extra, int keep)
{
    char head[512];
    int n = snprintf(head, sizeof head,
        "HTTP/1.1 %s\r\nContent-Type: application/octet-stream\r\nContent-Length: %llu\r\nAccept-Ranges: bytes\r\n"
        "Last-Modified: " LAST_MODIFIED "\r\n%sConnection: %s\r\n\r\n",
        status, (unsigned long long)length, extra ? extra : "", keep ? "keep-alive" : "close");
    return sendAll(fd, head, (size_t)n, s);
}

/* A header's value in head (lines after the first), or NULL. */
static int header(const char* head, const char* name, char* out, size_t size)
{
    size_t n = strlen(name);
    for (const char* line = strstr(head, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
        const char* p = line + 2;
        if (!strncasecmp(p, name, n) && p[n] == ':') {
            p += n + 1;
            while (*p == ' ' || *p == '\t')
                p++;
            size_t len = strcspn(p, "\r\n");
            if (len >= size)
                len = size - 1;
            memcpy(out, p, len);
            out[len] = 0;
            return 1;
        }
    }
    return 0;
}

/* What "bytes=..." asks of total: 1 a part (*start..*end inclusive), 0 the
 * whole, -1 unsatisfiable. */
static int parseRange(const char* value, uint64_t total, uint64_t* start, uint64_t* end)
{
    if (strncasecmp(value, "bytes=", 6) || strchr(value, ','))
        return 0;
    const char* p = value + 6;
    char* after;
    if (*p == '-') {
        unsigned long long n = strtoull(p + 1, &after, 10);
        if (after == p + 1 || !n)
            return 0;
        if (n > total)
            n = total;
        *start = total - n;
        *end = total - 1;
        return 1;
    }
    unsigned long long a = strtoull(p, &after, 10);
    if (after == p || *after != '-')
        return 0;
    p = after + 1;
    unsigned long long b = total ? total - 1 : 0;
    if (*p) {
        b = strtoull(p, &after, 10);
        if (after == p)
            return 0;
    }
    if (a >= total)
        return -1;
    if (b < a)
        return 0;
    *start = a;
    *end = b < total ? b : total - 1;
    return 1;
}

/* Sends bytes [start, end] of the package. */
static int body(Server* s, int fd, int* file, uint64_t start, uint64_t end, unsigned char* buffer)
{
    if (*file < 0) {
        *file = open(s->o->path, O_RDONLY);
        struct stat st;
        if (*file < 0 || fstat(*file, &st) || st.st_dev != s->dev || st.st_ino != s->ino || (uint64_t)st.st_size != s->total
            || st.st_mtime != s->mtime) {
            pthread_mutex_lock(&s->lock);
            if (!*s->failed)
                snprintf(s->failed, sizeof s->failed, "The package changed or went away while the console was copying it");
            pthread_mutex_unlock(&s->lock);
            return -1;
        }
    }
    for (uint64_t at = start; at <= end;) {
        size_t want = end - at + 1 < SEND_CHUNK ? (size_t)(end - at + 1) : SEND_CHUNK;
        size_t got = 0;
        while (got < want) {
            ssize_t n = pread(*file, buffer + got, want - got, (off_t)(at + got));
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                pthread_mutex_lock(&s->lock);
                if (!*s->failed)
                    snprintf(s->failed, sizeof s->failed, "The package couldn't be read (errno %d)", n ? errno : EIO);
                pthread_mutex_unlock(&s->lock);
                return -1;
            }
            got += (size_t)n;
        }
        if (sendAll(fd, buffer, want, s))
            return -1;
        pthread_mutex_lock(&s->lock);
        cover(s, at, at + want);
        s->last = now();
        pthread_mutex_unlock(&s->lock);
        at += want;
    }
    return 0;
}

/* Answers one request (head: NUL-terminated, without the blank line). 1
 * the connection may carry another, 0 close it. *valid: it asked for the
 * package (the token's path). Only those count as the console's activity:
 * a page in the browser can reach 127.0.0.1 too, and mustn't keep the job
 * from settling or stalling out. */
static int answer(Server* s, int fd, const char* head, int* file, unsigned char* buffer, int* valid)
{
    char method[8] = "", target[1024] = "", version[16] = "";
    if (sscanf(head, "%7s %1023s %15s", method, target, version) != 3) {
        respond(fd, s, "400 Bad Request", 0, NULL, 0);
        return 0;
    }
    char connection[32] = "";
    header(head, "Connection", connection, sizeof connection);
    int keep = strcmp(version, "HTTP/1.0") ? strcasecmp(connection, "close") != 0 : !strcasecmp(connection, "keep-alive");
    int headOnly = !strcmp(method, "HEAD");
    pthread_mutex_lock(&s->lock);
    unsigned n = ++s->seen;
    pthread_mutex_unlock(&s->lock);
    char range[128] = "";
    int ranged = header(head, "Range", range, sizeof range);
    if (n <= 8)
        logf_(s, "served: %s %s range=%s", method, target, ranged ? range : "-");
    if (!headOnly && strcmp(method, "GET")) {
        respond(fd, s, "405 Method Not Allowed", 0, NULL, 0);
        return 0;
    }
    /* the console adds a query (?product=...&serverIpAddr=...): the path decides */
    char* query = strchr(target, '?');
    if (query)
        *query = 0;
    size_t pl = strlen(s->prefix);
    const char* name = strncmp(target, s->prefix, pl) ? NULL : target + pl;
    if (!name || strcmp(name, s->urlName)) {
        if (name && !strcmp(name, s->crcName))
            logf_(s, "served: %s -> 404 (no .crc: the console installs without one)", target);
        else
            logf_(s, "served: %s -> 404 (not the package's address)", target);
        respond(fd, s, "404 Not Found", 0, NULL, 0);
        return 0;
    }
    *valid = 1;
    pthread_mutex_lock(&s->lock);
    s->last = now();
    s->requests++;
    pthread_mutex_unlock(&s->lock);
    uint64_t start = 0, end = s->total - 1;
    int part = ranged ? parseRange(range, s->total, &start, &end) : 0;
    char extra[128];
    if (part < 0) {
        snprintf(extra, sizeof extra, "Content-Range: bytes */%llu\r\n", (unsigned long long)s->total);
        respond(fd, s, "416 Range Not Satisfiable", 0, extra, keep);
        return keep;
    }
    if (part) {
        snprintf(extra, sizeof extra, "Content-Range: bytes %llu-%llu/%llu\r\n", (unsigned long long)start, (unsigned long long)end,
            (unsigned long long)s->total);
        if (respond(fd, s, "206 Partial Content", end - start + 1, extra, keep))
            return 0;
    } else if (respond(fd, s, "200 OK", s->total, NULL, keep))
        return 0;
    if (headOnly)
        return keep;
    return body(s, fd, file, start, end, buffer) ? 0 : keep;
}

static void* connThread(void* data)
{
    Conn* c = data;
    Server* s = c->s;
    int fd = c->fd;
    free(c);
    int file = -1;
    unsigned char* buffer = malloc(SEND_CHUNK);
    char* head = malloc(HEAD_MAX + 1);
    size_t fill = 0;
    double idleSince = now();
    int valid = 0; /* it asked for the package once */
    while (buffer && head && !s->stop) {
        char* end = NULL;
        head[fill] = 0;
        if ((end = strstr(head, "\r\n\r\n"))) {
            *end = 0;
            size_t used = (size_t)(end - head) + 4;
            int keep = answer(s, fd, head, &file, buffer, &valid);
            memmove(head, head + used, fill - used);
            fill -= used;
            idleSince = now();
            if (!keep)
                break;
            continue;
        }
        if (fill >= HEAD_MAX) {
            respond(fd, s, "431 Request Header Fields Too Large", 0, NULL, 0);
            break;
        }
        struct pollfd r = { fd, POLLIN, 0 };
        int p = poll(&r, 1, 200);
        if (p < 0 && errno == EINTR)
            continue;
        if (p < 0)
            break;
        if (!p) {
            if ((now() - idleSince) * 1000 > (valid ? IDLE_MS : STRANGER_MS))
                break;
            continue;
        }
        ssize_t n = recv(fd, head + fill, HEAD_MAX - fill, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (n <= 0)
            break;
        fill += (size_t)n;
    }
    if (file >= 0)
        close(file);
    free(buffer);
    free(head);
    close(fd);
    pthread_mutex_lock(&s->lock);
    if (!--s->conns)
        pthread_cond_broadcast(&s->idle);
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

static void* acceptThread(void* data)
{
    Server* s = data;
    while (!s->stop) {
        struct pollfd p = { s->listener, POLLIN, 0 };
        int r = poll(&p, 1, 200);
        if (r <= 0)
            continue;
        int fd = accept(s->listener, NULL, NULL);
        if (fd < 0) {
            struct timespec t = { 0, 100 * 1000 * 1000 }; /* EMFILE and the like: don't spin */
            nanosleep(&t, NULL);
            continue;
        }
        /* non-blocking: a console that stops reading can't hold a send()
         * past Stop or the stall limit (sendAll polls and checks stop) */
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            continue;
        }
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        int yes = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
        pthread_mutex_lock(&s->lock);
        int full = s->conns >= MAX_CONNS;
        if (!full)
            s->conns++;
        pthread_mutex_unlock(&s->lock);
        if (full) {
            static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(fd, busy, sizeof busy - 1, 0);
            close(fd);
            continue;
        }
        Conn* c = malloc(sizeof *c);
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        if (c) {
            c->s = s;
            c->fd = fd;
        }
        if (!c || pthread_create(&t, &attr, connThread, c)) {
            free(c);
            close(fd);
            pthread_mutex_lock(&s->lock);
            if (!--s->conns)
                pthread_cond_broadcast(&s->idle);
            pthread_mutex_unlock(&s->lock);
        }
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

/* ---------------------------------------------------------------- the console's installer */

#if defined(__PROSPERO__)
#define SYSTEM_INSTALL_AUTHID 0x4801000000000013ULL
#define SHELLCORE_AUTHID 0x3800000000000010ULL
#define REFUSED_PREFLIGHT 0x80B22404u

/* The published layout is six pointers, but the installer reads the 8
 * bytes after icon_url: nonzero sends a plain install down the patch path
 * (0x80B2116F). Zeroed first. */
typedef struct {
    const char* uri;
    const char* ex_uri;
    const char* playgo_scenario_id;
    const char* content_id;
    const char* content_name;
    const char* icon_url;
    uint64_t after_icon_url;
    uint64_t reserved[7];
} MetaInfo;
_Static_assert(sizeof(MetaInfo) == 0x70, "MetaInfo is 0x70 bytes");

typedef struct {
    char content_id[0x30];
    int content_type;
    int content_platform;
} PkgInfo;

typedef struct {
    char languages[30][8];
    char playgo_scenario_ids[64][3];
    char content_ids[64][0x30];
    unsigned char tail[6480];
} PlayGoInfo;

/* Static: PlayGoInfo is about 10 KiB, more than a worker's stack should
 * hold. Used under ps5install_appinst_lock. */
static MetaInfo meta;
static PkgInfo pkg;
static PlayGoInfo playgo;

static int (*appInstInit)(void);
static int (*installByPackage)(MetaInfo*, PkgInfo*, PlayGoInfo*);

typedef struct {
    uint64_t authid;
    uint8_t caps[16];
    uint8_t attrs[32];
    int haveCaps, haveAttrs;
} Creds;

/* ps5upload's installer credentials: the SYSTEM authid, every capability,
 * attrs 0x80. With the authid alone the installer refuses at once. */
static Creds credsAcquire(const Server* s)
{
    pid_t pid = getpid();
    Creds saved;
    memset(&saved, 0, sizeof saved);
    saved.authid = kernel_get_ucred_authid(pid);
    saved.haveCaps = kernel_get_ucred_caps(pid, saved.caps) == 0;
    saved.haveAttrs = kernel_get_ucred_attrs(pid, saved.attrs) == 0;
    if (kernel_set_ucred_authid(pid, SYSTEM_INSTALL_AUTHID) != 0)
        logf_(s, "install: can't swap the authid");
    uint8_t caps[16];
    memset(caps, 0xff, sizeof caps);
    if (kernel_set_ucred_caps(pid, caps) != 0)
        logf_(s, "install: can't raise the capabilities");
    uint8_t attrs[32];
    memset(attrs, 0, sizeof attrs);
    attrs[0] = 0x80;
    if (kernel_set_ucred_attrs(pid, attrs) != 0)
        logf_(s, "install: can't set the attrs");
    return saved;
}

static void credsRelease(const Server* s, const Creds* saved)
{
    pid_t pid = getpid();
    if (saved->haveCaps)
        kernel_set_ucred_caps(pid, saved->caps);
    if (saved->haveAttrs)
        kernel_set_ucred_attrs(pid, saved->attrs);
    if (!saved->authid)
        return;
    for (int i = 0; i < 3; i++) {
        kernel_set_ucred_authid(pid, saved->authid);
        if (kernel_get_ucred_authid(pid) == saved->authid)
            return;
    }
    logf_(s, "install: can't restore the authid 0x%llx", (unsigned long long)saved->authid);
}

static int consoleInstall(const Server* s, const char* url, const char* name, unsigned* code)
{
    *code = 0;
    if (!installByPackage) {
        static const char* const paths[] = { "/system/common/lib/libSceAppInstUtil.sprx", "/system/priv/lib/libSceAppInstUtil.sprx" };
        for (size_t i = 0; i < sizeof paths / sizeof *paths && !installByPackage; i++) {
            int res = 0;
            int handle = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, &res);
            if (handle < 0)
                continue;
            appInstInit = (int (*)(void))kernel_dynlib_dlsym(getpid(), (uint32_t)handle, "sceAppInstUtilInitialize");
            installByPackage = (int (*)(MetaInfo*, PkgInfo*, PlayGoInfo*))kernel_dynlib_dlsym(getpid(), (uint32_t)handle,
                "sceAppInstUtilInstallByPackage");
        }
        if (!installByPackage || !appInstInit) {
            installByPackage = NULL;
            logf_(s, "install: sceAppInstUtilInstallByPackage not found");
            return -2;
        }
    }
    ps5install_appinst_lock();
    Creds saved = credsAcquire(s);
    /* after the credentials: initialized as SYSTEM, once for the service
     * (a registration may have done it already), never terminated (as
     * ps5upload does); a failure is logged and the install tried anyway */
    int rc = ps5install_appinst_initialize(appInstInit);
    if (rc)
        logf_(s, "install: sceAppInstUtilInitialize: 0x%08x", (unsigned)rc);
    memset(&meta, 0, sizeof meta);
    memset(&pkg, 0, sizeof pkg);
    memset(&playgo, 0, sizeof playgo);
    meta.uri = url;
    meta.ex_uri = "";
    meta.playgo_scenario_id = "";
    meta.content_id = "";
    meta.content_name = name;
    meta.icon_url = "";
    rc = installByPackage(&meta, &pkg, &playgo);
    logf_(s, "install: InstallByPackage %s: 0x%08x", url, (unsigned)rc);
    if ((unsigned)rc == REFUSED_PREFLIGHT && kernel_set_ucred_authid(getpid(), SHELLCORE_AUTHID) == 0) {
        rc = installByPackage(&meta, &pkg, &playgo);
        logf_(s, "install: as ShellCore: 0x%08x", (unsigned)rc);
    }
    credsRelease(s, &saved);
    ps5install_appinst_unlock();
    if (!rc)
        return 0;
    *code = (unsigned)rc;
    return -1;
}
#endif

/* ---------------------------------------------------------------- the job */

const char* ps5pkginstall_hint(unsigned code)
{
    switch (code) {
    case 0x80B2116Fu:
        return "the console took it for a patch: install its base game first, or the package is broken";
    case 0x80A30004u:
    case 0x80A3000Au:
        return "install the base game first";
    case 0x80A3000Fu:
        return "it is for a different game (its title ID doesn't match)";
    case 0x80A30011u:
        return "the base game is the wrong version for this patch";
    case 0x80A30019u:
        return "the patch package is invalid or damaged";
    case 0x80A30017u:
        return "this add-on needs its game installed";
    case 0x80A3000Du:
        return "it needs a newer system software than this console's";
    case 0x80A3000Cu:
        return "close the game first";
    case 0x80A30002u:
        return "not enough free space";
    case 0x80A30008u:
    case 0x80A30009u:
        return "the package is damaged or the wrong kind";
    case 0x80020016u:
        return "no license: a fake package needs the console's fPKG patches";
    case 0x80B22404u:
        return "the installer refused the request before fetching anything";
    default:
        return NULL;
    }
}

/* 0, or -1 if there's no randomness (then nothing is served: a guessable
 * address would let a page in the browser fetch the package). */
static int randomToken(char out[17])
{
    unsigned char bytes[8] = { 0 };
    int fd = open("/dev/urandom", O_RDONLY);
    int ok = fd >= 0 && read(fd, bytes, sizeof bytes) == (ssize_t)sizeof bytes;
    if (fd >= 0)
        close(fd);
    if (!ok)
        return -1;
    for (int i = 0; i < 8; i++)
        snprintf(out + 2 * i, 3, "%02x", bytes[i]);
    return 0;
}

/* The package's name in the URL: its file name's letters, digits, - and
 * _ (at most 48), else "package". */
static void urlName(const char* path, char* out, size_t size, const char* tail)
{
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t n = 0;
    for (const char* p = base; *p && n < 48 && n + 1 < size; p++) {
        if (!strcasecmp(p, ".pkg"))
            break;
        char c = *p;
        out[n++] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ? c : '_';
    }
    if (!n)
        n = (size_t)snprintf(out, size, "package");
    snprintf(out + n, size - n, "%s", tail);
}

int ps5pkginstall_run(const PS5PkgInstallOptions* o, unsigned* code, char* error, size_t error_size)
{
    *code = 0;
    if (error && error_size)
        error[0] = 0;
    struct stat st;
    if (!o || !o->path || stat(o->path, &st) || !S_ISREG(st.st_mode) || !st.st_size) {
        fail(error, error_size, "The package isn't there (or is empty)");
        return -2;
    }
#if !defined(__PROSPERO__)
    if (!o->installer) {
        fail(error, error_size, "Packages are installed on the console only");
        return -1;
    }
#endif
    Server* s = calloc(1, sizeof *s);
    if (!s) {
        fail(error, error_size, "Out of memory");
        return -2;
    }
    s->o = o;
    s->total = (uint64_t)st.st_size;
    s->dev = st.st_dev;
    s->ino = st.st_ino;
    s->mtime = st.st_mtime;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->idle, NULL);
    char token[17];
    if (randomToken(token)) {
        fail(error, error_size, "The package couldn't be served (no /dev/urandom)");
        pthread_mutex_destroy(&s->lock);
        pthread_cond_destroy(&s->idle);
        free(s);
        return -2;
    }
    snprintf(s->prefix, sizeof s->prefix, "/%s/", token);
    urlName(o->path, s->urlName, sizeof s->urlName, ".pkg");
    urlName(o->path, s->crcName, sizeof s->crcName, ".crc");
    s->listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = { 0 };
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof address;
    int result = -2;
    pthread_t acceptor;
    int accepting = 0;
    if (s->listener < 0 || bind(s->listener, (struct sockaddr*)&address, sizeof address) || listen(s->listener, 32)
        || getsockname(s->listener, (struct sockaddr*)&address, &length)) {
        fail(error, error_size, "The package couldn't be served (errno %d)", errno);
        goto done;
    }
    if (pthread_create(&acceptor, NULL, acceptThread, s)) {
        fail(error, error_size, "The package couldn't be served (no thread)");
        goto done;
    }
    accepting = 1;
    char url[256];
    snprintf(url, sizeof url, "http://127.0.0.1:%u%s%s", (unsigned)ntohs(address.sin_port), s->prefix, s->urlName);
    if (strlen(url) > URL_MAX) {
        fail(error, error_size, "The package's address is too long for the installer");
        result = -1;
        goto done;
    }
    if (o->on_url)
        o->on_url(url, o->data);
    const char* name = o->name && *o->name ? o->name : s->urlName;
    pthread_mutex_lock(&s->lock);
    s->last = now();
    pthread_mutex_unlock(&s->lock);
    int accepted;
#if defined(__PROSPERO__)
    accepted = o->installer ? o->installer(url, name, code, o->installer_data) : consoleInstall(s, url, name, code);
#else
    accepted = o->installer(url, name, code, o->installer_data);
#endif
    if (accepted == -2) {
        fail(error, error_size, "The console's installer couldn't be loaded");
        goto done;
    }
    if (accepted) {
        const char* hint = ps5pkginstall_hint(*code);
        fail(error, error_size, "The console refused the package (0x%08X%s%s)", *code, hint ? ": " : "", hint ? hint : "");
        result = -1;
        goto done;
    }
    logf_(s, "install: accepted; serving %llu bytes at %s", (unsigned long long)s->total, url);
    double stall = (o->stall_ms ? o->stall_ms : 5 * 60 * 1000) / 1000.0;
    double settle = (o->settle_ms ? o->settle_ms : 2000) / 1000.0;
    for (;;) {
        struct timespec t = { 0, 250 * 1000 * 1000 };
        nanosleep(&t, NULL);
        pthread_mutex_lock(&s->lock);
        uint64_t covered = s->covered;
        double quiet = now() - s->last;
        unsigned conns = s->conns;
        char failed[256];
        snprintf(failed, sizeof failed, "%s", s->failed);
        pthread_mutex_unlock(&s->lock);
        if (o->progress)
            o->progress(covered, s->total, o->data);
        if (*failed) {
            fail(error, error_size, "%s", failed);
            break;
        }
        if (o->cancel && *o->cancel) {
            fail(error, error_size, "Cancelled: if the console still lists the download, delete it there");
            result = -3;
            break;
        }
        if (covered >= s->total && quiet >= settle && !conns) {
            result = 0;
            break;
        }
        if (covered >= s->total && quiet >= settle * 5) {
            result = 0; /* a kept-alive connection idling */
            break;
        }
        if (quiet >= stall) {
            fail(error, error_size, "The console stopped copying the package (%llu of %llu MB; nothing asked for %.0f %s)",
                (unsigned long long)(covered >> 20), (unsigned long long)(s->total >> 20), stall >= 60 ? stall / 60 : stall,
                stall >= 60 ? "minutes" : "s");
            break;
        }
    }
    logf_(s, "install: %s after %u requests", result ? "stopped" : "every byte served", s->requests);
done:
    s->stop = 1;
    if (accepting)
        pthread_join(acceptor, NULL);
    if (s->listener >= 0)
        close(s->listener);
    pthread_mutex_lock(&s->lock);
    while (s->conns)
        pthread_cond_wait(&s->idle, &s->lock);
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->idle);
    free(s);
    return result;
}
