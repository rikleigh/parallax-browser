/*
 * ps5launchers.c: see ps5launchers.h.
 */
#include "ps5launchers.h"

#include "../title/minijson.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <netinet/in.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__PROSPERO__)
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#endif

#define SMP_PORT 10101
#define KRAKEN_PORT 8097
#define REPLY_MAX (4u << 20)

static const char* prefixed(const PS5LaunchersOptions* o, const char* path, char* out, size_t size)
{
    snprintf(out, size, "%s%s", o && o->root ? o->root : "", path);
    return out;
}

static bool isFileAt(const PS5LaunchersOptions* o, const char* path)
{
    char p[1024];
    struct stat st;
    return !stat(prefixed(o, path, p, sizeof p), &st) && S_ISREG(st.st_mode);
}

static bool isDirAt(const PS5LaunchersOptions* o, const char* path)
{
    char p[1024];
    struct stat st;
    return !stat(prefixed(o, path, p, sizeof p), &st) && S_ISDIR(st.st_mode);
}

static int timeoutOf(const PS5LaunchersOptions* o)
{
    return o && o->timeout_ms > 0 ? o->timeout_ms : 1000;
}

/* ---- loopback HTTP ---- */

static gint64 nowMs(void)
{
    return g_get_monotonic_time() / 1000;
}

static bool waitFor(int fd, short events, gint64 deadline)
{
    for (;;) {
        gint64 left = deadline - nowMs();
        if (left <= 0)
            return false;
        struct pollfd p = { .fd = fd, .events = events };
        int r = poll(&p, 1, (int)left);
        if (r > 0)
            return true;
        if (r < 0 && errno == EINTR)
            continue;
        return false;
    }
}

/* Dechunks body in place (Transfer-Encoding: chunked); its new length, or
 * -1 if it is cut short. */
static long dechunk(char* body, size_t length)
{
    size_t in = 0, out = 0;
    for (;;) {
        char* end = NULL;
        if (in >= length)
            return -1;
        unsigned long n = strtoul(body + in, &end, 16);
        char* crlf = g_strstr_len(body + in, (gssize)(length - in), "\r\n");
        if (!crlf || end == body + in)
            return -1;
        in = (size_t)(crlf - body) + 2;
        if (!n)
            break;
        if (n > length - in)
            return -1;
        memmove(body + out, body + in, n);
        out += n;
        in += n + 2;
    }
    body[out] = 0;
    return (long)out;
}

/* One request to 127.0.0.1:port; the reply's status (and its body in
 * *body, NUL-terminated, g_free() it), or -1 (nothing answered). */
static int httpRequest(int port, const char* method, const char* path, const char* json, int timeoutMs, char** body)
{
    if (body)
        *body = NULL;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    gint64 deadline = nowMs() + timeoutMs;
    if (connect(fd, (struct sockaddr*)&a, sizeof a)) {
        int error = 0;
        socklen_t len = sizeof error;
        if (errno != EINPROGRESS || !waitFor(fd, POLLOUT, deadline) || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) || error) {
            close(fd);
            return -1;
        }
    }
    GString* req = g_string_new(NULL);
    g_string_append_printf(req, "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\nUser-Agent: parallax-browser\r\n", method, path,
        port);
    if (json)
        g_string_append_printf(req, "Content-Type: application/json\r\nContent-Length: %zu\r\n", strlen(json));
    else if (!strcmp(method, "POST"))
        g_string_append(req, "Content-Length: 0\r\n");
    g_string_append(req, "\r\n");
    if (json)
        g_string_append(req, json);
    size_t sent = 0;
    while (sent < req->len) {
        ssize_t w = send(fd, req->str + sent, req->len - sent, 0);
        if (w > 0) {
            sent += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EINTR) && waitFor(fd, POLLOUT, deadline))
            continue;
        break;
    }
    bool sentAll = sent == req->len;
    g_string_free(req, TRUE);
    if (!sentAll) {
        close(fd);
        return -1;
    }
    GString* reply = g_string_new(NULL);
    long want = -1; /* the whole reply's length, once the headers say */
    bool chunked = false;
    size_t headerEnd = 0;
    char* buf = g_malloc(16384); /* the service's threads have small stacks */
    for (;;) {
        if (want >= 0 && reply->len >= (size_t)want)
            break;
        if (!waitFor(fd, POLLIN, deadline))
            break;
        ssize_t r = recv(fd, buf, 16384, 0);
        if (r < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (r <= 0)
            break;
        g_string_append_len(reply, buf, r);
        if (reply->len > REPLY_MAX)
            break;
        if (!headerEnd) {
            char* end = strstr(reply->str, "\r\n\r\n");
            if (end) {
                headerEnd = (size_t)(end - reply->str) + 4;
                char* headers = g_ascii_strdown(reply->str, (gssize)headerEnd);
                char* cl = strstr(headers, "\r\ncontent-length:");
                if (cl)
                    want = (long)headerEnd + atol(cl + 17);
                char* te = strstr(headers, "\r\ntransfer-encoding:");
                if (te) {
                    char* eol = strstr(te + 2, "\r\n");
                    char* ch = strstr(te, "chunked");
                    chunked = ch && (!eol || ch < eol);
                }
                g_free(headers);
            }
        }
    }
    close(fd);
    g_free(buf);
    int status = -1;
    if (headerEnd && (!strncmp(reply->str, "HTTP/1.1 ", 9) || !strncmp(reply->str, "HTTP/1.0 ", 9)))
        status = atoi(reply->str + 9);
    if (status > 0 && body) {
        size_t length = reply->len - headerEnd;
        if (want >= 0 && (size_t)want - headerEnd < length)
            length = (size_t)want - headerEnd;
        char* text = g_strndup(reply->str + headerEnd, length);
        if (chunked && dechunk(text, length) < 0)
            text[0] = 0;
        *body = text;
    }
    g_string_free(reply, TRUE);
    return status;
}

/* POST json; the parsed reply if it came with status 200, else NULL. */
static MJ* postJSON(int port, const char* path, const char* json, int timeoutMs)
{
    char* body = NULL;
    int status = httpRequest(port, "POST", path, json, timeoutMs, &body);
    MJ* value = status == 200 && body ? mj_parse(body) : NULL;
    g_free(body);
    return value;
}

static MJ* getJSON(int port, const char* path, int timeoutMs)
{
    char* body = NULL;
    int status = httpRequest(port, "GET", path, NULL, timeoutMs, &body);
    MJ* value = status == 200 && body ? mj_parse(body) : NULL;
    g_free(body);
    return value;
}

static char* jsonQuote(const char* text)
{
    GString* out = g_string_new("\"");
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
        if (*p == '"' || *p == '\\')
            g_string_append_printf(out, "\\%c", *p);
        else if (*p < 0x20)
            g_string_append_printf(out, "\\u%04x", *p);
        else
            g_string_append_c(out, (char)*p);
    }
    g_string_append_c(out, '"');
    return g_string_free(out, FALSE);
}

/* ---- detection ---- */

/* ShadowMountPlus's [api] port and enabled from its config.ini. */
static void readSmpConfig(const PS5LaunchersOptions* o, int* port, bool* enabled)
{
    char path[1024];
    char* text = NULL;
    *port = SMP_PORT;
    *enabled = true;
    if (!g_file_get_contents(prefixed(o, "/data/shadowmount/config.ini", path, sizeof path), &text, NULL, NULL))
        return;
    char** lines = g_strsplit(text, "\n", -1);
    bool api = false;
    for (char** l = lines; *l; l++) {
        char* line = g_strstrip(*l);
        if (*line == '[') {
            api = !g_ascii_strncasecmp(line, "[api]", 5);
            continue;
        }
        char* eq = strchr(line, '=');
        if (!api || *line == '#' || *line == ';' || !eq)
            continue;
        *eq = 0;
        char* key = g_strstrip(line);
        char* value = g_strstrip(eq + 1);
        char* hash = strpbrk(value, "#;");
        if (hash)
            *hash = 0;
        g_strstrip(value);
        if (!g_ascii_strcasecmp(key, "port") && atoi(value) > 0 && atoi(value) < 65536)
            *port = atoi(value);
        else if (!g_ascii_strcasecmp(key, "enabled"))
            *enabled = !(!g_ascii_strcasecmp(value, "off") || !g_ascii_strcasecmp(value, "false") || !g_ascii_strcasecmp(value, "no")
                || !strcmp(value, "0"));
    }
    g_strfreev(lines);
    g_free(text);
}

/* A process (or its thread) called name runs. */
static bool processRuns(const char* name)
{
#if defined(__PROSPERO__)
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0 };
    size_t len = 0;
    if (sysctl(mib, 4, NULL, &len, NULL, 0) || !len)
        return false;
    len += len / 4;
    char* buf = malloc(len);
    if (!buf)
        return false;
    bool found = false;
    if (!sysctl(mib, 4, buf, &len, NULL, 0)) {
        for (size_t off = 0; off + sizeof(int) <= len;) {
            struct kinfo_proc* k = (struct kinfo_proc*)(buf + off);
            size_t need = offsetof(struct kinfo_proc, ki_tdname) + sizeof k->ki_tdname;
            if (offsetof(struct kinfo_proc, ki_comm) + sizeof k->ki_comm > need)
                need = offsetof(struct kinfo_proc, ki_comm) + sizeof k->ki_comm;
            if (k->ki_structsize <= 0 || (size_t)k->ki_structsize > len - off || (size_t)k->ki_structsize < need)
                break;
            if (!strncmp(k->ki_tdname, name, sizeof k->ki_tdname) || !strncmp(k->ki_comm, name, sizeof k->ki_comm)) {
                found = true;
                break;
            }
            off += (size_t)k->ki_structsize;
        }
    }
    free(buf);
    return found;
#else
    (void)name;
    return false;
#endif
}

static void copyRoots(const MJ* list, char roots[][256], unsigned* count)
{
    *count = 0;
    for (unsigned i = 0; i < mj_length(list) && *count < PS5LAUNCHERS_ROOTS; i++) {
        const char* path = mj_text(mj_at(list, i));
        if (!path || *path != '/')
            continue;
        snprintf(roots[*count], 256, "%s", path);
        size_t n = strlen(roots[*count]);
        while (n > 1 && roots[*count][n - 1] == '/')
            roots[*count][--n] = 0;
        (*count)++;
    }
}

/* GET path answers 200 with a cache manifest (Kraken Mount's and
 * TITAN-CORE's appcache): -1 nothing answered, 0 no, 1 yes. */
static int manifest(int port, const char* path, int timeout)
{
    char* body = NULL;
    int status = httpRequest(port, "GET", path, NULL, timeout, &body);
    int yes = status < 0 ? -1 : status == 200 && body && !strncmp(body, "CACHE MANIFEST", 14);
    g_free(body);
    return yes;
}

void ps5launchers_detect(PS5Launchers* out, const PS5LaunchersOptions* o)
{
    memset(out, 0, sizeof *out);
    int timeout = timeoutOf(o);
    bool enabled = true;
    readSmpConfig(o, &out->smp_port, &enabled);
    if (o && o->smp_port > 0)
        out->smp_port = o->smp_port;
    if (enabled) {
        MJ* v = postJSON(out->smp_port, "/api/v1/version", "{}", timeout);
        if (v && mj_number(v, "status", -1) == 0) {
            out->smp = out->smp_api = true;
            snprintf(out->smp_version, sizeof out->smp_version, "%s", mj_string(v, "shadowmount_version") ? mj_string(v, "shadowmount_version") : "");
        }
        mj_free(v);
    }
    if (out->smp_api) {
        MJ* s = postJSON(out->smp_port, "/api/v1/settings", "{}", timeout);
        copyRoots(mj_get(s, "scan_paths"), out->smp_roots, &out->smp_root_count);
        mj_free(s);
    } else if (isFileAt(o, "/data/.kstuff_noautomount") && isDirAt(o, "/data/shadowmount") && processRuns("shadowmountplus.elf"))
        out->smp = true; /* its API is off: it still scans */

    out->kraken_port = o && o->kraken_port > 0 ? o->kraken_port : KRAKEN_PORT;
    int titan = manifest(out->kraken_port, "/titan.appcache", timeout);
    if (titan == 1)
        out->titan = true;
    else if (titan == 0 && manifest(out->kraken_port, "/kraken.appcache", timeout) == 1)
        out->kraken = true; /* (nothing answered: not asked twice) */
    if (out->titan || out->kraken) {
        MJ* s = getJSON(out->kraken_port, "/api/settings", timeout);
        copyRoots(mj_get(s, "scanpath"), out->kraken_roots, &out->kraken_root_count);
        mj_free(s);
        /* its scan waits while an app it manages runs (this browser, in
         * /data/homebrew, when it started first) */
        MJ* state = getJSON(out->kraken_port, "/api/state", timeout);
        out->kraken_paused = mj_bool(state, "paused");
        mj_free(state);
    }
    out->kstuff_automount = !isFileAt(o, "/data/.kstuff_noautomount");
    out->etahen = isDirAt(o, "/data/etaHEN");
    const char* found = ps5launchers_scanner(out);
    snprintf(out->found, sizeof out->found, "%s", found ? found : "");
    snprintf(out->images, sizeof out->images, "%s", out->titan ? "TITAN-CORE" : out->smp ? "ShadowMountPlus" : "");
}

void ps5launchers_force(PS5Launchers* l, const char* which)
{
    if (!which || !*which || !strcmp(which, "auto"))
        return;
    bool smp = !strcmp(which, "smp"), kraken = !strcmp(which, "kraken"), titan = !strcmp(which, "titan");
    if (!smp && !kraken && !titan && strcmp(which, "self"))
        return;
    l->forced = true;
    /* TITAN-CORE: its own ShadowMountPlus as detected */
    l->smp = smp || (titan && l->smp);
    l->smp_api = (smp || titan) && l->smp_api;
    if (!kraken)
        l->kraken_paused = l->kraken_paused && titan;
    l->kraken = kraken;
    l->titan = titan;
}

const char* ps5launchers_scanner(const PS5Launchers* l)
{
    if (l->titan)
        return "TITAN-CORE";
    if (l->smp)
        return "ShadowMountPlus";
    if (l->kraken)
        return "Kraken Mount";
    return NULL;
}

const char* ps5launchers_image_mounter(const PS5Launchers* l)
{
    return l->images[0] ? l->images : NULL;
}

bool ps5launchers_waits(const PS5Launchers* l)
{
    /* TITAN-CORE's ShadowMountPlus doesn't wait */
    return l->kraken && l->kraken_paused;
}

/* Whether parent is one of the defaults (/data/homebrew, /data/etaHEN/games,
 * /mnt/usbN|extN/{homebrew,etaHEN/games} and those drives' roots). */
static bool defaultRoot(const char* parent)
{
    if (!strcmp(parent, "/data/homebrew") || !strcmp(parent, "/data/etaHEN/games") || !strcmp(parent, "/user/data/homebrew")
        || !strcmp(parent, "/user/data/etaHEN/games"))
        return true;
    for (int ext = 0; ext < 2; ext++) {
        unsigned last = ext ? 1 : 7;
        for (unsigned n = 0; n <= last; n++) {
            char drive[32];
            snprintf(drive, sizeof drive, "/mnt/%s%u", ext ? "ext" : "usb", n);
            size_t len = strlen(drive);
            if (strncmp(parent, drive, len))
                continue;
            const char* rest = parent + len;
            if (!*rest || !strcmp(rest, "/homebrew") || !strcmp(rest, "/etaHEN/games"))
                return true;
        }
    }
    return false;
}

/* /mnt/usb0 and the like (Kraken Mount looks only at the root itself) */
static bool driveRoot(const char* path)
{
    return !strncmp(path, "/mnt/", 5) && !strchr(path + 5, '/');
}

static bool inRoots(const char* parent, const char roots[][256], unsigned count)
{
    if (!count)
        return defaultRoot(parent);
    for (unsigned i = 0; i < count; i++) {
        if (!strcmp(parent, roots[i]))
            return true;
    }
    return false;
}

static char* parentOf(const char* folder)
{
    char* parent = g_path_get_dirname(folder);
    size_t n = strlen(parent);
    while (n > 1 && parent[n - 1] == '/')
        parent[--n] = 0;
    return parent;
}

/* Kraken Mount also takes a game one folder below a root (not below a
 * drive's root). */
static bool krakenScans(const PS5Launchers* l, const char* parent)
{
    if (inRoots(parent, l->kraken_roots, l->kraken_root_count))
        return true;
    if (!l->kraken_root_count)
        return false;
    char* grand = parentOf(parent);
    bool ok = !driveRoot(grand) && inRoots(grand, l->kraken_roots, l->kraken_root_count);
    g_free(grand);
    return ok;
}

bool ps5launchers_scans(const PS5Launchers* l, const char* folder, char* why, size_t size)
{
    char* parent = parentOf(folder);
    bool ok = false;
    /* in ps5launchers_scanner's order */
    if (l->titan) {
        /* its ShadowMountPlus takes it (added to its list), else TITAN
         * from its own folders */
        ok = l->smp_api || krakenScans(l, parent);
        if (!ok)
            snprintf(why, size, "TITAN-CORE doesn't look in %s (its scanpath setting): add it there", parent);
    } else if (l->smp) {
        /* outside its folders it is added to its list (with its API) */
        ok = l->smp_api || inRoots(parent, l->smp_roots, l->smp_root_count);
        if (!ok)
            snprintf(why, size, "ShadowMountPlus doesn't look in %s, and its API is off", parent);
    } else if (l->kraken) {
        ok = krakenScans(l, parent);
        if (!ok)
            snprintf(why, size, "Kraken Mount doesn't look in %s (its scanpath setting): add it there", parent);
    } else
        snprintf(why, size, "No homebrew launcher is running");
    g_free(parent);
    return ok;
}

/* ShadowMountPlus's entry for title_id in its game list, or NULL (owned by
 * *list: mj_free it). */
static const MJ* smpGame(const PS5Launchers* l, const char* title_id, int timeout, MJ** list)
{
    *list = l->smp_api ? postJSON(l->smp_port, "/api/v1/games", "{\"include_size\":false}", timeout) : NULL;
    const MJ* games = mj_get(*list, "games");
    for (unsigned i = 0; i < mj_length(games); i++) {
        const MJ* g = mj_at(games, i);
        if (!g_strcmp0(mj_string(g, "title_id"), title_id))
            return g;
    }
    return NULL;
}

static const MJ* krakenGame(const PS5Launchers* l, const char* title_id, const char* folder, int timeout, MJ** state)
{
    *state = l->kraken || l->titan ? getJSON(l->kraken_port, "/api/state", timeout) : NULL;
    const MJ* games = mj_get(*state, "games");
    const MJ* any = NULL;
    for (unsigned i = 0; i < mj_length(games); i++) {
        const MJ* g = mj_at(games, i);
        if (g_strcmp0(mj_string(g, "title_id"), title_id))
            continue;
        if (folder && !g_strcmp0(mj_string(g, "path"), folder))
            return g;
        if (!any && g_strcmp0(mj_string(g, "status"), "duplicate"))
            any = g;
    }
    return folder ? NULL : any;
}

static char* readLink(const PS5LaunchersOptions* o, const char* title_id, const char* name)
{
    char rel[128], path[1024];
    snprintf(rel, sizeof rel, "/user/app/%s/%s", title_id, name);
    char* text = NULL;
    if (!g_file_get_contents(prefixed(o, rel, path, sizeof path), &text, NULL, NULL))
        return NULL;
    char* nl = strchr(text, '\n'); /* mount_img.lnk: one layer a line */
    if (nl)
        *nl = 0;
    g_strstrip(text);
    if (!*text) {
        g_free(text);
        return NULL;
    }
    return text;
}

bool ps5launchers_source(const PS5Launchers* l, const char* title_id, bool links, const PS5LaunchersOptions* o, char* out, size_t size)
{
    int timeout = timeoutOf(o);
    MJ* list = NULL;
    const MJ* g = smpGame(l, title_id, timeout, &list);
    const char* path = mj_string(g, "path");
    bool found = path && *path;
    if (found)
        snprintf(out, size, "%s", path);
    mj_free(list);
    if (!found) {
        g = krakenGame(l, title_id, NULL, timeout, &list);
        path = mj_string(g, "path");
        found = path && *path;
        if (found)
            snprintf(out, size, "%s", path);
        mj_free(list);
    }
    for (int i = 0; links && !found && i < 2; i++) {
        char* link = readLink(o, title_id, i ? "mount.lnk" : "mount_img.lnk");
        found = link != NULL;
        if (found)
            snprintf(out, size, "%s", link);
        g_free(link);
    }
    return found;
}

int ps5launchers_before_replace(const PS5Launchers* l, const char* title_id, const PS5LaunchersOptions* o, char* error, size_t size)
{
    /* ShadowMountPlus mounts a folder app when it starts and releases it
     * after (1.7: "runtime layers released"), so the folder can be swapped
     * unless it is mounted now (the app runs). Its unmount answers 409 for
     * any title while the browser is in front. */
    MJ* list = NULL;
    const MJ* g = smpGame(l, title_id, timeoutOf(o), &list);
    bool mounted = mj_bool(g, "mounted");
    mj_free(list);
    if (!mounted)
        return 0;
    snprintf(error, size, "ShadowMountPlus has it mounted (it's running): close the app first");
    return -1;
}

int ps5launchers_announce(const PS5Launchers* l, const char* folder, const char* title_id, bool replaced,
    const PS5LaunchersOptions* o, char* error, size_t size)
{
    int timeout = timeoutOf(o);
    if (!ps5launchers_scans(l, folder, error, size))
        return -1;
    char* parent = parentOf(folder);
    if (l->smp_api) {
        char* json;
        const char* route;
        bool manual = false;
        if (inRoots(parent, l->smp_roots, l->smp_root_count)) {
            route = "/api/v1/scan";
            json = g_strdup("{\"reset_attempts\":true}");
        } else {
            char* quoted = jsonQuote(folder);
            route = "/api/v1/manual/add";
            json = g_strdup_printf("{\"path\":%s}", quoted);
            g_free(quoted);
            manual = true;
        }
        MJ* r;
        int answered = 1;
        if (manual) {
            /* its error replies (4xx, with "error") say why: kept */
            char* body = NULL;
            int status = httpRequest(l->smp_port, "POST", route, json, timeout, &body);
            answered = status > 0;
            r = body ? mj_parse(body) : NULL;
            g_free(body);
        } else
            r = postJSON(l->smp_port, route, json, timeout);
        if (manual && (!r || mj_number(r, "status", -1) != 0)) {
            /* not in a folder it scans: if it didn't add it, nothing finds it */
            const char* message = mj_string(r, "error");
            snprintf(error, size, "ShadowMountPlus didn't add %s to its list (%s%s%s)", folder, !answered ? "no answer" : "it refused",
                message && *message ? ": " : "", message && *message ? message : "");
            mj_free(r);
            g_free(json);
            g_free(parent);
            return -1;
        }
        /* for the log: whether it queued a scan, or holds it (scan_deferred_reason) */
        const char* reason = mj_string(r, "scan_deferred_reason");
        snprintf(error, size, "ShadowMountPlus %s: %s%s%s%s", route + 8, !r ? "no answer" : mj_bool(r, "scan_queued") ? "scan queued" : "no scan queued",
            mj_bool(r, "scan_deferred") ? ", deferred" : "", reason && *reason ? ": " : "", reason && *reason ? reason : "");
        mj_free(r);
        g_free(json);
    }
    if (l->titan || l->kraken) {
        int status = httpRequest(l->kraken_port, "POST", "/api/rescan", NULL, timeout, NULL);
        if (!l->smp_api)
            snprintf(error, size, "%s rescan: %d%s", l->titan ? "TITAN-CORE" : "Kraken Mount", status, l->kraken_paused ? " (it waits: paused)" : "");
        if (replaced) {
            char path[64];
            snprintf(path, sizeof path, "/api/rebuild/%s", title_id);
            httpRequest(l->kraken_port, "POST", path, NULL, timeout, NULL);
        }
    }
    g_free(parent);
    return 0; /* (not asked: its API is off, or the setting named it) it scans by itself; error says what it answered */
}

int ps5launchers_app_ready(const PS5Launchers* l, const char* folder, const char* title_id, const PS5LaunchersOptions* o,
    char* error, size_t size)
{
    int timeout = timeoutOf(o);
    if (l->smp_api) {
        /* registered from this folder (mount.lnk is written before the
         * registration, which can still fail; "mounted" only while it runs) */
        MJ* list = NULL;
        const MJ* g = smpGame(l, title_id, timeout, &list);
        bool ready = g && !g_strcmp0(mj_string(g, "path"), folder) && mj_bool(g, "installed");
        mj_free(list);
        if (ready)
            return 1;
    } else if (l->smp) {
        char* link = readLink(o, title_id, "mount.lnk");
        bool same = link && !strcmp(link, folder);
        g_free(link);
        if (same)
            return 1;
    }
    if (!l->titan && !l->kraken)
        return 0;
    MJ* state = NULL;
    const MJ* g = krakenGame(l, title_id, folder, timeout, &state);
    const char* status = mj_string(g, "status");
    int ready = 0;
    if (!g_strcmp0(status, "ready"))
        ready = 1;
    else if (!g_strcmp0(status, "error") || !g_strcmp0(status, "duplicate")) {
        const char* why = mj_string(g, "error");
        snprintf(error, size, "%s", why && *why ? why : !g_strcmp0(status, "duplicate") ? "Another copy of it is in its library" : "It failed");
        ready = -1;
    }
    mj_free(state);
    return ready;
}

char* ps5launchers_json(const PS5Launchers* l)
{
    const char* scanner = ps5launchers_scanner(l);
    GString* out = g_string_new("{\"scanner\":");
    g_string_append(out, scanner ? "\"" : "null");
    if (scanner)
        g_string_append_printf(out, "%s\"", scanner);
    g_string_append_printf(out, ",\"found\":\"%s\",\"images\":", l->found);
    g_string_append(out, l->images[0] ? "\"" : "null");
    if (l->images[0])
        g_string_append_printf(out, "%s\"", l->images);
    g_string_append_printf(out,
        ",\"smp\":%s,\"smpApi\":%s,\"kraken\":%s,\"titan\":%s,\"waits\":%s,\"kstuff\":%s,\"etahen\":%s,\"forced\":%s}",
        l->smp ? "true" : "false", l->smp_api ? "true" : "false", l->kraken ? "true" : "false", l->titan ? "true" : "false",
        ps5launchers_waits(l) ? "true" : "false", l->kstuff_automount ? "true" : "false", l->etahen ? "true" : "false",
        l->forced ? "true" : "false");
    return g_string_free(out, FALSE);
}

int ps5launchers_mounts_ufs2(const PS5Launchers* l, char* why, size_t size)
{
    if (!l->smp) {
        snprintf(why, size, "ShadowMountPlus isn't running: it (or TITAN-CORE, which starts it) mounts .ffpkg images");
        return -1;
    }
    /* "1.7", "1.7beta12", "v1.6-3-gabc123": the first two numbers */
    const char* v = l->smp_version;
    while (*v && !(*v >= '0' && *v <= '9'))
        v++;
    if (!*v)
        return -1; /* not known */
    unsigned major = 0, minor = 0;
    if (sscanf(v, "%u.%u", &major, &minor) < 1)
        return -1;
    if (major > 1 || (major == 1 && minor >= 7))
        return 1;
    snprintf(why, size, "ShadowMountPlus %s doesn't mount .ffpkg images: they need 1.7 or later (or make an exFAT image)", l->smp_version);
    return 0;
}

bool ps5launchers_scans_itself(const PS5Launchers* l, const char* path)
{
    char* parent = parentOf(path);
    bool ok = l->smp && inRoots(parent, l->smp_roots, l->smp_root_count);
    g_free(parent);
    return ok;
}

int ps5launchers_manual_list_add(const PS5LaunchersOptions* o, const char* path, char* error, size_t size)
{
    char list[1024];
    prefixed(o, "/data/shadowmount/manual.lst", list, sizeof list);
    char* text = NULL;
    g_file_get_contents(list, &text, NULL, NULL);
    bool there = false;
    char** lines = g_strsplit(text ? text : "", "\n", -1);
    for (char** line = lines; *line && !there; line++) {
        g_strstrip(*line);
        there = !strcmp(*line, path);
    }
    g_strfreev(lines);
    int r = 0;
    if (!there) {
        FILE* f = fopen(list, "a");
        bool newline = text && *text && text[strlen(text) - 1] != '\n';
        if (!f || (newline && fputc('\n', f) == EOF) || fprintf(f, "%s\n", path) < 0) {
            snprintf(error, size, "Couldn't add it to ShadowMountPlus's list (%s, errno %d)", list, errno);
            r = -1;
        }
        if (f && fclose(f) && !r) {
            snprintf(error, size, "Couldn't add it to ShadowMountPlus's list (%s, errno %d)", list, errno);
            r = -1;
        }
    }
    g_free(text);
    return r;
}
