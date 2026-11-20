/*
 * The runtime files (README: "Installing"): fonts, CA certificates, ICU,
 * XKB and MIME data, the start page and the logo, which the title and its
 * payload children read from /data/ps5webkit. They ship inside the title
 * folder (/app0/runtime, packaged by package.sh), because a store installs
 * and updates only that folder, and the payloads can't see /app0: so the
 * title copies them out when it starts.
 *
 *   /app0/runtime/runtime-version   a hash of the bundled files (package.sh)
 *   /app0/runtime/runtime-files.txt the files, one relative path a line
 *   /data/ps5webkit/runtime-version the one installed
 *
 * The title can open files in /app0 but not list its folders (EPERM, seen
 * on the console 2026-11-10), hence the list. Same stamp: nothing to do.
 * Otherwise every listed file is copied over
 * (to <name>.tmp, then renamed), and the stamp is written last, so a copy
 * cut short (power, a full disk) is redone at the next start. Only the
 * bundled paths are written: users/, settings, flags, downloads and the
 * other files in /data/ps5webkit are never touched, and files an older
 * runtime had are left where they are.
 *
 * Every folder and file is open to all (0777, 0666), like the rest of
 * /data/ps5webkit: the title runs as its own uid, and FTP on the console,
 * which runs as another and gets no root override, couldn't delete what
 * was in a 0755 folder the title made (GitHub issue 2). Folders an older
 * title made 0755 are opened at every start, up to date or not.
 *
 * Main thread, right after the unjail, before anything reads the files.
 */
#define SPIKE_NAME "ps5browser"
#include "common/spike.h"
#include "runtime.h"
#include "WPEProcessManagerPS5.h"

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RUNTIME_FROM "/app0/runtime"
#define RUNTIME_TO "/data/ps5webkit"
#define RUNTIME_STAMP "runtime-version"
#define RUNTIME_LIST "runtime-files.txt"

typedef struct {
    unsigned files;
    guint64 bytes;
    int error; /* errno of the first failure */
    gboolean inData; /* it was in /data/ps5webkit (not reading /app0) */
    char failed[256];
} CopyState;

static void failWhere(CopyState* state, const char* what, const char* path, gboolean inData)
{
    if (state->error)
        return;
    state->error = errno ? errno : EIO;
    state->inData = inData;
    snprintf(state->failed, sizeof state->failed, "%s %s", what, path);
}

/* A failure writing in /data/ps5webkit. */
static void fail(CopyState* state, const char* what, const char* path)
{
    failWhere(state, what, path, TRUE);
}

/* A failure reading the title's own files: no repair of /data helps. */
static void failSource(CopyState* state, const char* what, const char* path)
{
    failWhere(state, what, path, FALSE);
}

static gboolean refusedInData(const CopyState* state)
{
    return state->inData && (state->error == EACCES || state->error == EPERM);
}

static void copyFile(const char* from, const char* to, CopyState* state)
{
    static char buffer[1 << 20];
    int in = open(from, O_RDONLY);
    if (in < 0) {
        failSource(state, "open", from);
        return;
    }
    char* tmp = g_strconcat(to, ".tmp", NULL);
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) {
        fail(state, "create", tmp);
        close(in);
        g_free(tmp);
        return;
    }
    fchmod(out, 0666);
    gboolean ok = TRUE;
    for (;;) {
        ssize_t n = read(in, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            failSource(state, "read", from);
            ok = FALSE;
            break;
        }
        if (!n)
            break;
        for (ssize_t done = 0; done < n;) {
            ssize_t w = write(out, buffer + done, (size_t)(n - done));
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0) {
                fail(state, "write", tmp);
                ok = FALSE;
                break;
            }
            done += w;
        }
        if (!ok)
            break;
        state->bytes += (guint64)n;
    }
    close(in);
    if (close(out) && ok) {
        fail(state, "close", tmp);
        ok = FALSE;
    }
    if (ok && rename(tmp, to)) {
        fail(state, "rename to", to);
        ok = FALSE;
    }
    if (!ok)
        unlink(tmp);
    else
        state->files++;
    g_free(tmp);
}

/* runtime-files.txt as lines, stripped, or NULL. */
static char** readList(void)
{
    char* listPath = g_build_filename(RUNTIME_FROM, RUNTIME_LIST, NULL);
    char* list = NULL;
    gboolean read = g_file_get_contents(listPath, &list, NULL, NULL);
    g_free(listPath);
    if (!read)
        return NULL;
    char** lines = g_strsplit(list, "\n", -1);
    g_free(list);
    for (char** line = lines; *line; line++)
        g_strstrip(*line);
    return lines;
}

/* Relative paths inside the runtime only. */
static gboolean listedPathOK(const char* name)
{
    return name[0] != '/' && !strstr(name, "..");
}

/* The folders of relative (a path under RUNTIME_TO), each made if missing
 * and opened to all; done holds the ones already seen. */
static int openFolders(const char* relative, GHashTable* done)
{
    char* path = g_build_filename(RUNTIME_TO, relative, NULL);
    size_t base = strlen(RUNTIME_TO) + 1;
    int result = 0;
    for (char* cut = path + base;; cut++) {
        if (*cut && *cut != '/')
            continue;
        char saved = *cut;
        *cut = 0;
        if (!g_hash_table_contains(done, path)) {
            if (mkdir(path, 0777) && errno != EEXIST) {
                result = errno;
                *cut = saved;
                break;
            }
            chmod(path, 0777);
            g_hash_table_add(done, g_strdup(path));
        }
        *cut = saved;
        if (!saved)
            break;
    }
    g_free(path);
    errno = result;
    return result ? -1 : 0;
}

/* Every listed file's folders, and the fontconfig cache's, made and opened
 * to all. Fontconfig writes its cache there; FTP copies and zips drop empty
 * folders. */
static void openAllFolders(char** lines, CopyState* state)
{
    GHashTable* done = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (char** line = lines; *line && !state->error; line++) {
        const char* name = *line;
        if (!*name || !listedPathOK(name))
            continue;
        char* parent = g_path_get_dirname(name);
        if (strcmp(parent, ".") && openFolders(parent, done))
            fail(state, "mkdir", parent);
        g_free(parent);
    }
    if (!state->error && openFolders("cache/fontconfig", done))
        fail(state, "mkdir", RUNTIME_TO "/cache/fontconfig");
    g_hash_table_destroy(done);
}

/* Every file runtime-files.txt names, from RUNTIME_FROM to RUNTIME_TO. */
static void copyListed(char** lines, CopyState* state)
{
    for (char** line = lines; *line && !state->error; line++) {
        const char* name = *line;
        if (!*name)
            continue;
        if (!listedPathOK(name)) {
            errno = EINVAL;
            failSource(state, "refused", name);
            break;
        }
        char* source = g_build_filename(RUNTIME_FROM, name, NULL);
        char* target = g_build_filename(RUNTIME_TO, name, NULL);
        copyFile(source, target, state);
        g_free(source);
        g_free(target);
    }
}

static char* readStamp(const char* dir)
{
    char* path = g_build_filename(dir, RUNTIME_STAMP, NULL);
    char* contents = NULL;
    if (g_file_get_contents(path, &contents, NULL, NULL))
        g_strstrip(contents);
    g_free(path);
    return contents;
}

int ps5wk_runtime_install(char* error, size_t errorSize, gboolean* dataRefused)
{
    if (error && errorSize)
        *error = 0;
    if (dataRefused)
        *dataRefused = FALSE;
    char* bundled = readStamp(RUNTIME_FROM);
    if (!bundled) {
        /* A development title packaged with NO_RUNTIME=1: the files were
         * copied to the console by hand. */
        spike_log("runtime: none in the title (%s), using %s as it is", RUNTIME_FROM, RUNTIME_TO);
        return 0;
    }
    char* installed = readStamp(RUNTIME_TO);
    char** lines = readList();
    if (installed && !strcmp(installed, bundled)) {
        /* Folders an older title made 0755 (issue 2). */
        CopyState opened = { 0 };
        if (lines)
            openAllFolders(lines, &opened);
        spike_log("runtime: %s is up to date (%s)%s%s", RUNTIME_TO, bundled, opened.error ? "; couldn't open " : "",
            opened.error ? opened.failed : "");
        if (dataRefused)
            *dataRefused = refusedInData(&opened);
        g_strfreev(lines);
        g_free(installed);
        g_free(bundled);
        return 0;
    }
    spike_log("runtime: installing %s into %s (bundled %s, installed %s)", RUNTIME_FROM, RUNTIME_TO, bundled,
        installed ? installed : "none");
    gint64 start = g_get_monotonic_time();
    CopyState state = { 0 };
    if (!lines)
        failSource(&state, "read", RUNTIME_FROM "/" RUNTIME_LIST);
    else if (g_mkdir_with_parents(RUNTIME_TO, 0777))
        fail(&state, "mkdir", RUNTIME_TO);
    else {
        openAllFolders(lines, &state);
        copyListed(lines, &state);
    }
    if (!state.error) {
        char* from = g_build_filename(RUNTIME_FROM, RUNTIME_STAMP, NULL);
        char* to = g_build_filename(RUNTIME_TO, RUNTIME_STAMP, NULL);
        copyFile(from, to, &state);
        g_free(from);
        g_free(to);
    }
    double ms = (g_get_monotonic_time() - start) / 1000.0;
    int result;
    if (state.error) {
        spike_log("runtime: install FAILED after %u files: %s (errno %d); the next start tries again", state.files, state.failed, state.error);
        if (error && errorSize)
            snprintf(error, errorSize, "%s (errno %d)", state.failed, state.error);
        result = -1;
    } else {
        spike_log("runtime: installed %u files, %.1f MB, in %.0f ms", state.files, state.bytes / 1048576.0, ms);
        result = (int)state.files;
    }
    g_strfreev(lines);
    g_free(installed);
    g_free(bundled);
    if (dataRefused)
        *dataRefused = result < 0 && refusedInData(&state);
    return result;
}

/* GitHub issue 3: the title runs as uid 1, its payloads as uid 0, and a
 * folder a payload made at the default umask (before 0.2.3) is 0755 and
 * root's: the title can't write in it. Seen as "couldn't install its
 * files", no icons (the bar's are font glyphs) and no ad blocker (it saves
 * its lists in adblock/ and browser/contentfilters). A title can't chmod
 * what isn't its own, and access() can't be trusted in a title
 * (titlesys.c), so: a test file in each folder the title writes in, and if
 * one is refused, the restart helper (a payload, root) opens the tree. */

static const char* const writtenFolders[] = {
    "", "home", "home/cache", "home/tmp", "cache", "cache/fontconfig", "fonts", "adblock", "adblock/work", "browser",
    "browser/contentfilters", "update", "users",
};

gboolean ps5wk_data_locked(char* which, size_t whichSize)
{
    for (size_t i = 0; i < G_N_ELEMENTS(writtenFolders); i++) {
        char* folder = g_build_filename(RUNTIME_TO, writtenFolders[i], NULL);
        char* test = g_strdup_printf("%s/.write-test-%d", folder, (int)getpid());
        int fd = open(test, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        int error = fd < 0 ? errno : 0;
        if (fd >= 0) {
            close(fd);
            unlink(test);
        }
        g_free(test);
        /* A folder that isn't there yet is made by the title itself. */
        if (error == EACCES || error == EPERM) {
            if (which && whichSize)
                snprintf(which, whichSize, "%s (errno %d)", folder, error);
            g_free(folder);
            return TRUE;
        }
        g_free(folder);
    }
    return FALSE;
}

#define REPAIR_ELF "/app0/ps5wk-relaunch.elf"
#define REPAIR_TIMEOUT_S 30

/* One repair a run (the title's start): static, so a helper that answers
 * after the wait gave up writes into something that is still there. */
static struct {
    GMutex lock;
    GCond cond;
    gboolean ended, done;
    unsigned changed, failed;
} repairWait;

static void repairOutput(const char* line, gpointer data)
{
    (void)data;
    unsigned changed, failed;
    const char* done = strstr(line, "repair done ");
    if (!done || sscanf(done, "repair done %u %u", &changed, &failed) != 2)
        return;
    g_mutex_lock(&repairWait.lock);
    repairWait.done = TRUE;
    repairWait.changed = changed;
    repairWait.failed = failed;
    g_cond_broadcast(&repairWait.cond); /* the result is in: no need to wait for the connection to close */
    g_mutex_unlock(&repairWait.lock);
}

static void repairEnded(gboolean launched, gpointer data)
{
    (void)launched;
    (void)data;
    g_mutex_lock(&repairWait.lock);
    repairWait.ended = TRUE;
    g_cond_broadcast(&repairWait.cond);
    g_mutex_unlock(&repairWait.lock);
}

int ps5wk_data_repair(char* error, size_t errorSize)
{
    if (error && errorSize)
        *error = 0;
    int fd = open(REPAIR_ELF, O_RDONLY);
    if (fd < 0) {
        if (error && errorSize)
            snprintf(error, errorSize, "the restart helper is missing");
        return -1;
    }
    close(fd);
    /* Before the display connects: a manager of its own, for this task. */
    WPEProcessManager* manager = wpe_process_manager_ps5_new(NULL);
    gint64 start = g_get_monotonic_time();
    if (!wpe_process_manager_ps5_run_task_elf(manager, REPAIR_ELF, "repair", RUNTIME_TO, repairOutput, repairEnded, NULL)) {
        g_object_unref(manager);
        if (error && errorSize)
            snprintf(error, errorSize, "the restart helper couldn't be started");
        return -1;
    }
    gint64 end = start + REPAIR_TIMEOUT_S * G_USEC_PER_SEC;
    g_mutex_lock(&repairWait.lock);
    while (!repairWait.done && !repairWait.ended && g_cond_wait_until(&repairWait.cond, &repairWait.lock, end))
        ;
    gboolean ended = repairWait.ended, done = repairWait.done;
    unsigned changed = repairWait.changed, failed = repairWait.failed;
    g_mutex_unlock(&repairWait.lock);
    g_object_unref(manager); /* the task holds its own reference */
    spike_log("runtime: repair %s after %.0f ms: %u opened to all, %u failed", done ? "done" : ended ? "ended without a result" : "TIMED OUT",
        (g_get_monotonic_time() - start) / 1000.0, changed, failed);
    if (!done) {
        if (error && errorSize)
            snprintf(error, errorSize, ended ? "the restart helper didn't run" : "the restart helper didn't answer in %d s", REPAIR_TIMEOUT_S);
        return -1;
    }
    return (int)changed;
}
