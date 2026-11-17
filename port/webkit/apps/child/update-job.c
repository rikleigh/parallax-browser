/*
 * update-job.c: the browser's own updates, in ps5wk-child as a task payload
 * of its own (port/webkit/UPDATES.md).
 *
 * The title (apps/title/updates.c) writes a job file and starts the child
 * ELF with the header "PS5WK-LAUNCH 1 update 0 <job file>"
 * (wpe_process_manager_ps5_run_task). This process then
 *   check:    finds the installed folder, reads ProsperoStore's receipt and
 *             journal for it, and asks GitHub for the newest release;
 *   download: the same, then downloads the release's zip into a staging
 *             folder on the install's drive, checks its size and SHA-256,
 *             unpacks it (apps/common/ps5zip.c), checks the unpacked
 *             param.json and writes ready.txt, then <work>/staged.txt;
 * and writes a result file last. The title reads it when this process's
 * output closes; no result means failure. The swap itself is the restart
 * helper's (apps/relaunch/ps5wk-relaunch.c), once the title is gone.
 *
 * A payload, not the title: the console slows a title's own file writes to
 * about 2 MB/s after a few hundred megabytes, and the browser unpacks to
 * about 470 MB. Output: "[update-job] ..." lines, relayed to the title's
 * log; "[update-job] status: <text>" and "[update-job] progress: <phase>
 * <done> <total>" are read by the title.
 */
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
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <wpe/webkit.h>

#include "../common/ps5zip.h"

#define UPDATE_JOB_LIMIT_S (60 * 60)
#define UPDATE_ZIP_ENTRIES 20000
#define UPDATE_UNPACKED_LIMIT (3ull << 30)
#define UPDATE_ZIP_LIMIT (2ull << 30)

int ps5wk_update_job(const char* jobPath);

#define JOBLOG(...) do { fprintf(stderr, "[update-job] " __VA_ARGS__); fputc('\n', stderr); } while (0)

static void status(const char* format, ...) G_GNUC_PRINTF(1, 2);
static void status(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    char* text = g_strdup_vprintf(format, args);
    va_end(args);
    for (char* c = text; *c; c++) {
        if (*c == '\n' || *c == '\r')
            *c = ' ';
    }
    fprintf(stderr, "[update-job] status: %s\n", text);
    g_free(text);
}

static void progressLine(const char* phase, guint64 done, guint64 total)
{
    fprintf(stderr, "[update-job] progress: %s %llu %llu\n", phase, (unsigned long long)done, (unsigned long long)total);
}

/* ---- versions ----
 * A release's tag is vA.B.C; its contentVersion is 0(A+1).BBB.CCC (0.2.0 is
 * 01.002.000). */
typedef struct {
    int major, minor, patch;
} Version;

static gboolean parseTag(const char* tag, Version* v)
{
    if (!tag)
        return FALSE;
    if (*tag == 'v' || *tag == 'V')
        tag++;
    char extra;
    return sscanf(tag, "%d.%d.%d%c", &v->major, &v->minor, &v->patch, &extra) == 3 && v->major >= 0 && v->major < 98
        && v->minor >= 0 && v->minor < 1000 && v->patch >= 0 && v->patch < 1000;
}

static gboolean parseContentVersion(const char* text, Version* v)
{
    int a, b, c;
    char extra;
    if (!text || strlen(text) != 10 || sscanf(text, "%2d.%3d.%3d%c", &a, &b, &c, &extra) != 3 || a < 1)
        return FALSE;
    v->major = a - 1;
    v->minor = b;
    v->patch = c;
    return TRUE;
}

static char* contentVersionOf(const Version* v)
{
    return g_strdup_printf("%02d.%03d.%03d", v->major + 1, v->minor, v->patch);
}

static int compareVersions(const Version* a, const Version* b)
{
    if (a->major != b->major)
        return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor)
        return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch)
        return a->patch < b->patch ? -1 : 1;
    return 0;
}

/* ---- JSON (JavaScriptCore) ---- */

static JSCContext* js;

static JSCValue* parseJSON(const char* text)
{
    if (!js)
        js = jsc_context_new();
    JSCValue* value = jsc_value_new_from_json(js, text);
    if (jsc_context_get_exception(js)) {
        jsc_context_clear_exception(js);
        g_clear_object(&value);
    }
    return value;
}

/* obj[key] as a string, or NULL. */
static char* jsonString(JSCValue* object, const char* key)
{
    if (!object || !jsc_value_is_object(object))
        return NULL;
    JSCValue* v = jsc_value_object_get_property(object, key);
    char* s = v && jsc_value_is_string(v) ? jsc_value_to_string(v) : NULL;
    g_clear_object(&v);
    return s;
}

static double jsonNumber(JSCValue* object, const char* key)
{
    JSCValue* v = jsc_value_object_get_property(object, key);
    double d = v && jsc_value_is_number(v) ? jsc_value_to_double(v) : -1;
    g_clear_object(&v);
    return d;
}

static gboolean jsonBool(JSCValue* object, const char* key)
{
    JSCValue* v = jsc_value_object_get_property(object, key);
    gboolean b = v && jsc_value_is_boolean(v) && jsc_value_to_boolean(v);
    g_clear_object(&v);
    return b;
}

static char* readSmall(const char* path, gsize limit)
{
    gchar* text = NULL;
    gsize length = 0;
    if (!g_file_get_contents(path, &text, &length, NULL))
        return NULL;
    if (length > limit) {
        g_free(text);
        return NULL;
    }
    return text;
}

/* The title ID and contentVersion folder's param.json declares. */
static gboolean readParam(const char* folder, char** title, char** version)
{
    char* path = g_build_filename(folder, "sce_sys", "param.json", NULL);
    char* text = readSmall(path, 64 * 1024);
    g_free(path);
    JSCValue* param = text ? parseJSON(text) : NULL;
    g_free(text);
    *title = jsonString(param, "titleId");
    *version = jsonString(param, "contentVersion");
    g_clear_object(&param);
    if (*title && *version)
        return TRUE;
    g_clear_pointer(title, g_free);
    g_clear_pointer(version, g_free);
    return FALSE;
}

/* ---- the installed folder ----
 * The places ShadowMountPlus scans, as ProsperoStore's self-update kit
 * looks: the homebrew and etaHEN folders of each drive, and the external
 * drives' roots. */
typedef struct {
    char* folder;   /* the installed app, e.g. /data/homebrew/PPSA99207 */
    char* location; /* the folder holding it, e.g. /data/homebrew */
    char* version;  /* its contentVersion */
} Install;

static void installClear(Install* install)
{
    g_free(install->folder);
    g_free(install->location);
    g_free(install->version);
    memset(install, 0, sizeof *install);
}

static GPtrArray* installRoots(void)
{
    GPtrArray* roots = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(roots, g_strdup("/data/homebrew"));
    g_ptr_array_add(roots, g_strdup("/data/etaHEN/games"));
    const char* drives[] = { "/mnt/ext0", "/mnt/ext1", "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3", "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7" };
    for (unsigned i = 0; i < G_N_ELEMENTS(drives); i++) {
        g_ptr_array_add(roots, g_strdup_printf("%s/homebrew", drives[i]));
        g_ptr_array_add(roots, g_strdup_printf("%s/etaHEN/games", drives[i]));
        g_ptr_array_add(roots, g_strdup(drives[i]));
    }
    return roots;
}

/* The one installed folder of title at version: TRUE, or FALSE with why. */
static gboolean findInstall(const char* title, const char* version, Install* out, char** why)
{
    GPtrArray* roots = installRoots();
    GPtrArray* found = g_ptr_array_new_with_free_func(g_free);
    unsigned atVersion = 0, other = 0;
    for (unsigned r = 0; r < roots->len; r++) {
        const char* root = g_ptr_array_index(roots, r);
        GDir* dir = g_dir_open(root, 0, NULL);
        if (!dir)
            continue;
        const char* name;
        unsigned looked = 0;
        while ((name = g_dir_read_name(dir)) && looked++ < 4096) {
            char* folder = g_build_filename(root, name, NULL);
            struct stat st;
            char *id = NULL, *v = NULL;
            if (!lstat(folder, &st) && S_ISDIR(st.st_mode) && readParam(folder, &id, &v) && !strcmp(id, title)) {
                gboolean known = FALSE; /* the same folder through two roots */
                for (unsigned i = 0; i < found->len; i++)
                    known = known || !strcmp(g_ptr_array_index(found, i), folder);
                if (!known) {
                    JOBLOG("installed copy: %s (%s)", folder, v);
                    if (!strcmp(v, version)) {
                        atVersion++;
                        if (!out->folder) {
                            out->folder = g_strdup(folder);
                            out->location = g_strdup(root);
                            out->version = g_strdup(v);
                        }
                    } else
                        other++;
                    g_ptr_array_add(found, g_strdup(folder));
                }
            }
            g_free(id);
            g_free(v);
            g_free(folder);
        }
        g_dir_close(dir);
    }
    g_ptr_array_unref(found);
    g_ptr_array_unref(roots);
    if (atVersion == 1)
        return TRUE;
    installClear(out);
    if (!atVersion)
        *why = g_strdup(other ? "the installed folder holds another version than the one running: update by hand"
                              : "the browser's folder wasn't found (installed as an image, or in an unusual place): update by hand");
    else
        *why = g_strdup_printf("the browser is installed %u times: remove the extra copies, or update by hand", atVersion);
    return FALSE;
}

/* ---- ProsperoStore (read only) ----
 * It manages an app when <root>/receipts/<title>.json names the folder that
 * holds it and the version that is there (its managed() check); a journal
 * naming the app means an operation on it is in progress or interrupted. */
static void storeState(const char* storeRoot, const char* title, const Install* install, gboolean* managed, gboolean* busy)
{
    *managed = *busy = FALSE;
    char* path = g_strdup_printf("%s/receipts/%s.json", storeRoot, title);
    char* text = readSmall(path, 16 * 1024);
    JSCValue* receipt = text ? parseJSON(text) : NULL;
    char* id = jsonString(receipt, "titleId");
    char* location = jsonString(receipt, "location");
    char* version = jsonString(receipt, "contentVersion");
    if (receipt) {
        char* target = location ? g_strdup_printf("%s/%s", location, title) : NULL;
        *managed = id && !strcmp(id, title) && target && !strcmp(target, install->folder) && version
            && !strcmp(version, install->version);
        JOBLOG("ProsperoStore receipt %s: %s, %s, %s: %s", path, id ? id : "?", location ? location : "?", version ? version : "?",
            *managed ? "it manages this install" : "not this install (the store sees it as installed outside it)");
        g_free(target);
    }
    g_free(id);
    g_free(location);
    g_free(version);
    g_clear_object(&receipt);
    g_free(text);
    g_free(path);

    path = g_strdup_printf("%s/journal.json", storeRoot);
    text = readSmall(path, 64 * 1024);
    JSCValue* journal = text ? parseJSON(text) : NULL;
    id = jsonString(journal, "titleId");
    *busy = id && !strcmp(id, title);
    if (text)
        JOBLOG("ProsperoStore journal: %s%s", id ? id : "?", *busy ? " (this app: an operation in progress)" : "");
    g_free(id);
    g_clear_object(&journal);
    g_free(text);
    g_free(path);
}

/* ---- GitHub ---- */

static SoupSession* session;

static SoupSession* soupSession(void)
{
    if (!session)
        session = soup_session_new_with_options("timeout", 60, "idle-timeout", 60, "user-agent", "ParallaxBrowser-update", NULL);
    return session;
}

typedef struct {
    Version version;
    char* tag;
    char* url;
    guint64 size;
    char* sha256;
    char* page;
    char* notes;
} Release;

static void releaseClear(Release* r)
{
    g_free(r->tag);
    g_free(r->url);
    g_free(r->sha256);
    g_free(r->page);
    g_free(r->notes);
    memset(r, 0, sizeof *r);
}

/* The newest published release with its zip (drafts skipped, pre-releases
 * counted: the catalog's rule). TRUE, or FALSE with why. */
static gboolean newestRelease(const char* source, Release* out, char** why)
{
    SoupMessage* message = soup_message_new("GET", source);
    if (!message) {
        *why = g_strdup_printf("bad address %s", source);
        return FALSE;
    }
    soup_message_headers_append(soup_message_get_request_headers(message), "Accept", "application/vnd.github+json");
    GError* error = NULL;
    GBytes* body = NULL;
    for (int attempt = 1; attempt <= 3 && !body; attempt++) {
        g_clear_error(&error);
        body = soup_session_send_and_read(soupSession(), message, NULL, &error);
        guint code = soup_message_get_status(message);
        if (body && code != 200) {
            g_bytes_unref(body);
            body = NULL;
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, code == 403 || code == 429 ? "GitHub is limiting requests (HTTP %u)" : "HTTP %u", code);
            if (code >= 400 && code < 500)
                break;
        }
        if (!body && attempt < 3) {
            JOBLOG("asking %s failed (%s), attempt %d; again", source, error ? error->message : "?", attempt);
            g_usleep(attempt * G_USEC_PER_SEC);
            g_object_unref(message);
            message = soup_message_new("GET", source);
            soup_message_headers_append(soup_message_get_request_headers(message), "Accept", "application/vnd.github+json");
        }
    }
    g_object_unref(message);
    if (!body) {
        *why = g_strdup_printf("couldn't reach GitHub: %s", error ? error->message : "failed");
        g_clear_error(&error);
        return FALSE;
    }
    gsize length = 0;
    const char* data = g_bytes_get_data(body, &length);
    char* text = g_strndup(data, length);
    g_bytes_unref(body);
    JSCValue* releases = parseJSON(text);
    g_free(text);
    if (!releases || !jsc_value_is_array(releases)) {
        g_clear_object(&releases);
        *why = g_strdup("GitHub's answer wasn't a list of releases");
        return FALSE;
    }
    double count = jsonNumber(releases, "length");
    gboolean any = FALSE;
    for (unsigned i = 0; i < count && i < 100; i++) {
        JSCValue* release = jsc_value_object_get_property_at_index(releases, i);
        char* tag = jsonString(release, "tag_name");
        Version v;
        if (!release || jsonBool(release, "draft") || !parseTag(tag, &v) || (any && compareVersions(&v, &out->version) <= 0)) {
            g_free(tag);
            g_clear_object(&release);
            continue;
        }
        char* wanted = g_strdup_printf("parallax-browser-%d.%d.%d.zip", v.major, v.minor, v.patch);
        JSCValue* assets = jsc_value_object_get_property(release, "assets");
        double assetCount = assets && jsc_value_is_array(assets) ? jsonNumber(assets, "length") : 0;
        for (unsigned a = 0; a < assetCount && a < 100; a++) {
            JSCValue* asset = jsc_value_object_get_property_at_index(assets, a);
            char* name = jsonString(asset, "name");
            char* digest = jsonString(asset, "digest");
            char* url = jsonString(asset, "browser_download_url");
            double size = jsonNumber(asset, "size");
            if (name && !strcmp(name, wanted) && digest && g_str_has_prefix(digest, "sha256:") && strlen(digest) == 7 + 64 && url && size > 0) {
                releaseClear(out);
                out->version = v;
                out->tag = g_strdup(tag);
                out->url = g_strdup(url);
                out->size = (guint64)size;
                out->sha256 = g_ascii_strdown(digest + 7, -1);
                out->page = jsonString(release, "html_url");
                char* notes = jsonString(release, "body");
                if (notes && strlen(notes) > 8000)
                    notes[8000] = 0; /* made whole UTF-8 below */
                out->notes = notes;
                any = TRUE;
            } else if (name && !strcmp(name, wanted))
                JOBLOG("%s: %s has no SHA-256 digest from GitHub; passed over", tag, name);
            g_free(name);
            g_free(digest);
            g_free(url);
            g_clear_object(&asset);
        }
        if (!any || strcmp(out->tag, tag))
            JOBLOG("%s: no %s with a digest; passed over", tag, wanted);
        g_clear_object(&assets);
        g_free(wanted);
        g_free(tag);
        g_clear_object(&release);
    }
    g_clear_object(&releases);
    if (!any) {
        *why = g_strdup("no release with the browser's zip was found");
        return FALSE;
    }
    /* Release notes cut at 8000 bytes: back to a whole UTF-8 character. */
    if (out->notes) {
        const gchar* end = NULL;
        if (!g_utf8_validate(out->notes, -1, &end))
            *(gchar*)end = 0;
    }
    return TRUE;
}

/* ---- download ---- */

static gboolean allowedDownload(GUri* uri, gboolean test)
{
    if (!uri)
        return FALSE;
    if (test)
        return TRUE;
    const char* scheme = g_uri_get_scheme(uri);
    const char* host = g_uri_get_host(uri);
    return scheme && !g_ascii_strcasecmp(scheme, "https") && host
        && (!g_ascii_strcasecmp(host, "github.com") || g_str_has_suffix(host, ".githubusercontent.com"));
}

/* url into path, its size and SHA-256 checked: TRUE, or FALSE with why. */
static gboolean downloadRelease(const Release* release, const char* path, gboolean test, char** why)
{
    GUri* first = g_uri_parse(release->url, G_URI_FLAGS_NONE, NULL);
    gboolean allowed = allowedDownload(first, test);
    if (first)
        g_uri_unref(first);
    if (!allowed) {
        *why = g_strdup_printf("the release's address isn't on GitHub: %s", release->url);
        return FALSE;
    }
    SoupMessage* message = soup_message_new("GET", release->url);
    GError* error = NULL;
    GInputStream* stream = message ? soup_session_send(soupSession(), message, NULL, &error) : NULL;
    guint code = message ? soup_message_get_status(message) : 0;
    if (stream && (code != 200 || !allowedDownload(soup_message_get_uri(message), test))) {
        *why = code != 200 ? g_strdup_printf("the download failed: HTTP %u", code)
                           : g_strdup("the download was sent somewhere other than GitHub");
        g_object_unref(stream);
        g_object_unref(message);
        return FALSE;
    }
    if (!stream) {
        *why = g_strdup_printf("the download failed: %s", error ? error->message : "no answer");
        g_clear_error(&error);
        g_clear_object(&message);
        return FALSE;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        *why = g_strdup_printf("the download couldn't be saved (errno %d)", errno);
        g_object_unref(stream);
        g_object_unref(message);
        return FALSE;
    }
    GChecksum* sum = g_checksum_new(G_CHECKSUM_SHA256);
    static char buffer[256 * 1024];
    guint64 got = 0;
    gint64 shown = 0;
    gboolean ok = TRUE;
    for (;;) {
        gssize n = g_input_stream_read(stream, buffer, sizeof buffer, NULL, &error);
        if (n < 0) {
            *why = g_strdup_printf("the download stopped: %s", error ? error->message : "read failed");
            g_clear_error(&error);
            ok = FALSE;
            break;
        }
        if (!n)
            break;
        got += (guint64)n;
        if (got > release->size || got > UPDATE_ZIP_LIMIT) {
            *why = g_strdup("the download is bigger than the release says");
            ok = FALSE;
            break;
        }
        g_checksum_update(sum, (const guchar*)buffer, (gsize)n);
        for (gssize done = 0; done < n;) {
            ssize_t w = write(fd, buffer + done, (size_t)(n - done));
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0) {
                *why = g_strdup_printf("the download couldn't be saved (errno %d)", errno);
                ok = FALSE;
                break;
            }
            done += w;
        }
        if (!ok)
            break;
        gint64 now = g_get_monotonic_time();
        if (now - shown > 500000 || got == release->size) {
            progressLine("download", got, release->size);
            shown = now;
        }
    }
    g_input_stream_close(stream, NULL, NULL);
    g_object_unref(stream);
    g_object_unref(message);
    if (close(fd) && ok) {
        *why = g_strdup_printf("the download couldn't be saved (errno %d)", errno);
        ok = FALSE;
    }
    if (ok && got != release->size) {
        *why = g_strdup_printf("the download ended early (%llu of %llu bytes)", (unsigned long long)got, (unsigned long long)release->size);
        ok = FALSE;
    }
    if (ok && strcmp(g_checksum_get_string(sum), release->sha256)) {
        *why = g_strdup("the download's SHA-256 doesn't match the release's: it was refused");
        JOBLOG("SHA-256 %s, the release says %s", g_checksum_get_string(sum), release->sha256);
        ok = FALSE;
    }
    g_checksum_free(sum);
    return ok;
}

static void unpackProgress(uint64_t done, uint64_t total, void* data)
{
    gint64* shown = data;
    gint64 now = g_get_monotonic_time();
    if (now - *shown > 500000 || done == total) {
        progressLine("unpack", done, total);
        *shown = now;
    }
}

/* The staging work folder on the install's drive: the job's work folder on
 * /data, <drive>/ps5webkit-update elsewhere. Neither is a folder
 * ShadowMountPlus scans. */
static char* workFor(const Install* install, const char* dataWork)
{
    if (g_str_has_prefix(install->folder, "/data/"))
        return g_strdup(dataWork);
    char** parts = g_strsplit(install->folder, "/", 4); /* "", "mnt", "usb0", rest */
    char* work = parts[1] && parts[2] ? g_strdup_printf("/%s/%s/ps5webkit-update", parts[1], parts[2]) : NULL;
    g_strfreev(parts);
    return work;
}

static gboolean writeText(const char* path, const char* text)
{
    char* temporary = g_strdup_printf("%s.tmp", path);
    gboolean ok = g_file_set_contents(temporary, text, -1, NULL) && !chmod(temporary, 0666) && !rename(temporary, path);
    if (!ok)
        unlink(temporary);
    g_free(temporary);
    return ok;
}

/* ---- the job ---- */

int ps5wk_update_job(const char* jobPath)
{
    alarm(UPDATE_JOB_LIMIT_S);
    GKeyFile* job = g_key_file_new();
    if (!g_key_file_load_from_file(job, jobPath, G_KEY_FILE_NONE, NULL)) {
        JOBLOG("no job file %s", jobPath);
        return 1;
    }
    char* mode = g_key_file_get_string(job, "job", "mode", NULL);
    char* title = g_key_file_get_string(job, "job", "title", NULL);
    char* running = g_key_file_get_string(job, "job", "running", NULL);
    char* source = g_key_file_get_string(job, "job", "source", NULL);
    char* work = g_key_file_get_string(job, "job", "work", NULL);
    char* storeRoot = g_key_file_get_string(job, "job", "store-root", NULL);
    char* resultPath = g_key_file_get_string(job, "job", "result", NULL);
    gboolean test = g_key_file_get_boolean(job, "job", "test-source", NULL);
    g_key_file_unref(job);
    if (!mode || !title || !running || !source || !work || !storeRoot || !resultPath) {
        JOBLOG("incomplete job file %s", jobPath);
        return 1;
    }
    JOBLOG("%s for %s %s (source %s%s)", mode, title, running, source, test ? ", a test source" : "");

    GKeyFile* result = g_key_file_new();
    g_key_file_set_string(result, "result", "mode", mode);
    char* why = NULL;
    Install install = { 0 };
    Release release = { 0 };
    Version runningVersion;
    gboolean managed = FALSE, busy = FALSE, available = FALSE;
    gboolean downloading = !strcmp(mode, "download");

    if (!parseContentVersion(running, &runningVersion))
        why = g_strdup_printf("the running version %s isn't understood", running);
    /* The install: needed to download; for a check, only to say whether the
     * store manages it. */
    gboolean installed = !why && findInstall(title, running, &install, &why);
    if (installed) {
        storeState(storeRoot, title, &install, &managed, &busy);
        g_key_file_set_string(result, "result", "install", install.folder);
        g_key_file_set_string(result, "result", "location", install.location);
    } else if (!downloading && why) {
        g_key_file_set_string(result, "result", "install-problem", why);
        g_clear_pointer(&why, g_free);
    }
    g_key_file_set_boolean(result, "result", "managed", managed);
    g_key_file_set_boolean(result, "result", "busy", busy);
    if (busy && !why)
        why = g_strdup("ProsperoStore has an operation on the browser in progress: finish it in the store first");

    if (!why) {
        status("Looking for a newer version…");
        if (newestRelease(source, &release, &why)) {
            available = compareVersions(&release.version, &runningVersion) > 0;
            char* version = g_strdup_printf("%d.%d.%d", release.version.major, release.version.minor, release.version.patch);
            JOBLOG("newest release %s (%s, %llu bytes, sha256 %s): %s", release.tag, version, (unsigned long long)release.size,
                release.sha256, available ? "newer" : "not newer");
            g_key_file_set_string(result, "result", "tag", release.tag);
            g_key_file_set_string(result, "result", "version", version);
            g_key_file_set_uint64(result, "result", "size", release.size);
            g_key_file_set_string(result, "result", "page", release.page ? release.page : "");
            g_key_file_set_string(result, "result", "notes", release.notes ? release.notes : "");
            g_free(version);
        }
    }
    g_key_file_set_boolean(result, "result", "available", available);

    if (downloading && !why) {
        if (managed)
            why = g_strdup("ProsperoStore manages this install: update it in ProsperoStore");
        else if (!available)
            why = g_strdup("there's no newer version");
    }
    if (downloading && !why) {
        char* stagingWork = workFor(&install, work);
        char* staging = stagingWork ? g_build_filename(stagingWork, "staging", NULL) : NULL;
        char* zip = staging ? g_build_filename(staging, "release.zip", NULL) : NULL;
        char* app = staging ? g_build_filename(staging, title, NULL) : NULL;
        char* expected = contentVersionOf(&release.version);
        struct statvfs fs;
        if (!staging)
            why = g_strdup_printf("no work folder for %s", install.folder);
        else if (ps5zip_remove_tree(staging)) /* an earlier attempt's leftovers */
            why = g_strdup_printf("the old staging folder %s couldn't be removed (errno %d)", staging, errno);
        else if (g_mkdir_with_parents(staging, 0777) || chmod(stagingWork, 0777) || chmod(staging, 0777))
            why = g_strdup_printf("the staging folder %s couldn't be made (errno %d)", staging, errno);
        else if (!statvfs(staging, &fs) && (guint64)fs.f_bavail * fs.f_frsize < release.size * 4)
            why = g_strdup_printf("not enough free space: %llu MB needed, %llu MB free", (unsigned long long)(release.size * 4 >> 20),
                (unsigned long long)((guint64)fs.f_bavail * fs.f_frsize >> 20));
        if (!why) {
            status("Downloading %s…", release.tag);
            JOBLOG("downloading %s to %s", release.url, zip);
            gint64 start = g_get_monotonic_time();
            if (downloadRelease(&release, zip, test, &why))
                JOBLOG("downloaded and checked in %.1f s", (g_get_monotonic_time() - start) / 1e6);
        }
        if (!why) {
            status("Unpacking %s…", release.tag);
            char prefix[32];
            snprintf(prefix, sizeof prefix, "%s/", title);
            char error[512] = "";
            gint64 shown = 0, start = g_get_monotonic_time();
            if (g_mkdir(app, 0777) || chmod(app, 0777))
                why = g_strdup_printf("the unpack folder couldn't be made (errno %d)", errno);
            else if (ps5zip_extract(zip, prefix, app, UPDATE_ZIP_ENTRIES, UPDATE_UNPACKED_LIMIT, unpackProgress, &shown, error, sizeof error))
                why = g_strdup_printf("the release's zip was refused: %s", error);
            else
                JOBLOG("unpacked in %.1f s", (g_get_monotonic_time() - start) / 1e6);
        }
        if (!why) {
            char *id = NULL, *version = NULL;
            char* eboot = g_build_filename(app, "eboot.bin", NULL);
            if (!readParam(app, &id, &version))
                why = g_strdup("the release has no readable sce_sys/param.json");
            else if (strcmp(id, title))
                why = g_strdup_printf("the release is for %s, not %s", id, title);
            else if (strcmp(version, expected))
                why = g_strdup_printf("the release says version %s, its tag %s", version, expected);
            else if (!g_file_test(eboot, G_FILE_TEST_IS_REGULAR))
                why = g_strdup("the release has no eboot.bin");
            g_free(eboot);
            g_free(id);
            g_free(version);
        }
        if (zip)
            unlink(zip);
        if (!why) {
            char* readyPath = g_build_filename(staging, "ready.txt", NULL);
            char* ready = g_strdup_printf("version=%d.%d.%d\ntag=%s\ncontent-version=%s\nrunning=%s\ninstall=%s\nlocation=%s\npage=%s\n",
                release.version.major, release.version.minor, release.version.patch, release.tag, expected, running, install.folder,
                install.location, release.page ? release.page : "");
            char* stagedPath = g_build_filename(work, "staged.txt", NULL);
            char* staged = g_strdup_printf("%s\n", staging);
            if (!writeText(readyPath, ready) || !writeText(stagedPath, staged))
                why = g_strdup("the update couldn't be recorded");
            else {
                JOBLOG("ready: %s in %s", release.tag, staging);
                g_key_file_set_string(result, "result", "staging", staging);
            }
            g_free(staged);
            g_free(stagedPath);
            g_free(ready);
            g_free(readyPath);
        }
        if (why && staging)
            ps5zip_remove_tree(staging);
        g_free(expected);
        g_free(app);
        g_free(zip);
        g_free(staging);
        g_free(stagingWork);
    }

    g_key_file_set_boolean(result, "result", "ok", !why);
    if (why) {
        JOBLOG("%s failed: %s", mode, why);
        g_key_file_set_string(result, "result", "error", why);
    }
    gsize length = 0;
    char* text = g_key_file_to_data(result, &length, NULL);
    gboolean written = text && writeText(resultPath, text);
    if (!written)
        JOBLOG("the result couldn't be written to %s", resultPath);
    g_free(text);
    g_key_file_unref(result);
    releaseClear(&release);
    installClear(&install);
    g_free(why);
    g_free(mode);
    g_free(title);
    g_free(running);
    g_free(source);
    g_free(work);
    g_free(storeRoot);
    g_free(resultPath);
    return written ? 0 : 1;
}
