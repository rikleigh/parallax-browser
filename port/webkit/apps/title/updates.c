/*
 * updates.c: the browser's own updates (port/webkit/UPDATES.md).
 *
 * Each start, 10 s in, the update job (ps5wk-child's "update" mode,
 * apps/child/update-job.c) asks GitHub for the newest release. If it's
 * newer, the update page (parallax://update, below) opens over the tab
 * shown and offers it; nothing is downloaded without the user's yes. When
 * ProsperoStore manages the install, the page says to update it there.
 * Update: the job downloads and unpacks it into a staging folder; Restart
 * now (or the next start) installs it through the restart helper
 * (apps/relaunch/ps5wk-relaunch.c), which swaps the files once the title is
 * gone and starts it again.
 *
 * Files (UPDATE_WORK): job.ini and result.ini (the job's), staged.txt (the
 * staging folder of a release that is ready; the job writes it last),
 * last-install.txt (the helper's outcome, read once at the next start).
 * Logs: [update] lines, and the job's [update-job] lines.
 */
#define SPIKE_NAME "ps5browser"

#include "common/spike.h"
#include "browser.h"
#include "../common/ps5zip.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <wpe/webkit.h>
#include "WPEDisplayPS5.h"
#include "WPEProcessManagerPS5.h"

#define UPDATE_WORK "/data/ps5webkit/update"
#define UPDATE_SOURCE "https://api.github.com/repos/rikleigh/parallax-browser/releases?per_page=10"
/* Test use only: a URL in place of GitHub's API, and a folder in place of
 * ProsperoStore's (UPDATES.md, "Testing"). */
#define UPDATE_SOURCE_TEST "/data/ps5webkit/update-source"
#define UPDATE_STORE_ROOT "/data/prosperostore"
#define UPDATE_STORE_ROOT_TEST "/data/ps5webkit/update-store-root"
/* Test use only: the user's yes to Update and Restart now. */
#define UPDATE_TEST_YES "/data/ps5webkit/update-test-yes"
#define UPDATE_CHECK_DELAY_S 10
#define UPDATE_TITLE "PPSA99207"

typedef enum {
    U_IDLE,
    U_CHECKING,
    U_UP_TO_DATE,
    U_AVAILABLE,
    U_DOWNLOADING,
    U_READY,
    U_INSTALLING,
    U_FAILED,
} UpdateState;

static const char* const stateNames[] = { "idle", "checking", "up-to-date", "available", "downloading", "ready", "installing", "failed" };

/* What the route threads read: under lock. */
static GMutex lock;
static UpdateState state;
static char running[16];    /* contentVersion, 01.002.000 */
static char runningShown[16]; /* 0.2.0 */
static char* latest;         /* 0.3.0 */
static char* tag;
static char* page;
static char* notes;
static char* failure;        /* why the last step failed */
static char* installProblem; /* why this install can't update itself */
static char* status;         /* the job's last status line */
static char* staging;        /* the release that is ready */
static char* lastInstall;    /* the helper's outcome at this start, once */
static gboolean managed, busy;
static guint64 size, progressDone, progressTotal;
static char progressPhase[16];
static gint64 checkedAt;
static gboolean failedDownload; /* the failure was a download's: Try again downloads */

/* Main loop only. */
static WPEDisplayPS5* display;
static gboolean jobRunning;
static gboolean openWhenFound; /* the start's check, or Check for updates */
static gboolean offered;       /* the page opened by itself this run */
static gboolean manualCheck;   /* Check for updates: the page opens whatever it finds */

static void setString(char** field, const char* value)
{
    g_free(*field);
    *field = value && *value ? g_strdup(value) : NULL;
}

static void message(const char* text, int seconds)
{
    if (display)
        wpe_display_ps5_show_message(display, text, seconds);
}

/* The running version, from the title's own param.json. */
static void readRunningVersion(void)
{
    gchar* text = NULL;
    if (g_file_get_contents("/app0/sce_sys/param.json", &text, NULL, NULL)) {
        const char* key = strstr(text, "\"contentVersion\"");
        const char* quote = key ? strchr(key + 16, '"') : NULL;
        if (quote && strlen(quote + 1) >= 10 && quote[11] == '"')
            snprintf(running, sizeof running, "%.10s", quote + 1);
    }
    g_free(text);
    int a, b, c;
    if (sscanf(running, "%2d.%3d.%3d", &a, &b, &c) == 3 && a >= 1)
        snprintf(runningShown, sizeof runningShown, "%d.%d.%d", a - 1, b, c);
    else
        snprintf(runningShown, sizeof runningShown, "%s", *running ? running : "?");
}

static char* fileLine(const char* path)
{
    gchar* text = NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL))
        return NULL;
    text[strcspn(text, "\r\n")] = 0;
    if (!*text) {
        g_free(text);
        return NULL;
    }
    return text;
}

/* key=value from ready.txt. */
static char* readyValue(const char* text, const char* key)
{
    size_t length = strlen(key);
    for (const char* line = text; line && *line;) {
        if (!strncmp(line, key, length) && line[length] == '=')
            return g_strndup(line + length + 1, strcspn(line + length + 1, "\r\n"));
        const char* next = strchr(line, '\n');
        line = next ? next + 1 : NULL;
    }
    return NULL;
}

/* ---- the job ---- */

static gboolean jobFinishedOnMain(gpointer data);
static const char* startJob(const char* mode);
static const char* install(const char* from);

/* The relay thread: one line of the job's output. */
static void jobOutput(const char* line, gpointer data)
{
    (void)data;
    static const char statusPrefix[] = "[update-job] status: ";
    static const char progressPrefix[] = "[update-job] progress: ";
    if (!strncmp(line, statusPrefix, sizeof statusPrefix - 1)) {
        g_mutex_lock(&lock);
        setString(&status, line + sizeof statusPrefix - 1);
        g_mutex_unlock(&lock);
    } else if (!strncmp(line, progressPrefix, sizeof progressPrefix - 1)) {
        char phase[16];
        unsigned long long done, total;
        if (sscanf(line + sizeof progressPrefix - 1, "%15s %llu %llu", phase, &done, &total) == 3) {
            g_mutex_lock(&lock);
            snprintf(progressPhase, sizeof progressPhase, "%s", phase);
            progressDone = done;
            progressTotal = total;
            g_mutex_unlock(&lock);
        }
    }
}

typedef struct {
    char* mode;
    gboolean launched;
} Job;

/* The relay thread: the job's output closed (it ended), or it never ran. */
static void jobEnded(gboolean launched, gpointer data)
{
    Job* job = data;
    job->launched = launched;
    g_idle_add(jobFinishedOnMain, job);
}

static gboolean openPageOnMain(gpointer data)
{
    (void)data;
    if (!ps5wk_browser_open_page(PS5WK_UPDATE_URL))
        g_timeout_add_seconds(2, openPageOnMain, NULL); /* a tab switch: try again */
    return G_SOURCE_REMOVE;
}

static gboolean jobFinishedOnMain(gpointer data)
{
    Job* job = data;
    jobRunning = FALSE;
    GKeyFile* result = g_key_file_new();
    gboolean read = g_key_file_load_from_file(result, UPDATE_WORK "/result.ini", G_KEY_FILE_NONE, NULL);
    gboolean ok = read && g_key_file_get_boolean(result, "result", "ok", NULL);
    char* error = read ? g_key_file_get_string(result, "result", "error", NULL) : NULL;
    gboolean download = !strcmp(job->mode, "download");
    gboolean available = FALSE;
    g_mutex_lock(&lock);
    if (read) {
        char* value;
#define TAKE(field, key) value = g_key_file_get_string(result, "result", key, NULL); if (value || !download) setString(&field, value); g_free(value)
        TAKE(latest, "version");
        TAKE(tag, "tag");
        TAKE(page, "page");
        TAKE(notes, "notes");
#undef TAKE
        value = g_key_file_get_string(result, "result", "install-problem", NULL);
        if (value || !download)
            setString(&installProblem, value);
        g_free(value);
        managed = g_key_file_get_boolean(result, "result", "managed", NULL);
        busy = g_key_file_get_boolean(result, "result", "busy", NULL);
        size = g_key_file_get_uint64(result, "result", "size", NULL);
        available = g_key_file_get_boolean(result, "result", "available", NULL);
    }
    setString(&status, NULL);
    if (!download)
        checkedAt = time(NULL);
    if (ok && download) {
        char* where = g_key_file_get_string(result, "result", "staging", NULL);
        setString(&staging, where);
        g_free(where);
        state = U_READY;
        failedDownload = FALSE;
    } else if (ok)
        state = available ? U_AVAILABLE : U_UP_TO_DATE;
    else {
        state = U_FAILED;
        failedDownload = download;
        setString(&failure, error ? error : !job->launched ? "the update job couldn't start" : "the update job ended without a result");
    }
    UpdateState now = state;
    char* shownVersion = g_strdup(latest);
    gboolean canInstall = !managed && !installProblem;
    g_mutex_unlock(&lock);
    spike_log("[update] %s %s: %s%s%s", job->mode, ok ? "done" : "failed", stateNames[now], error ? ": " : "", error ? error : "");
    int testFd = open(UPDATE_TEST_YES, O_RDONLY); /* not access(): a title's says no (titlesys.c) */
    gboolean testYes = testFd >= 0;
    if (testFd >= 0)
        close(testFd);
    if (now == U_READY) {
        char* text = g_strdup_printf("Parallax Browser %s is ready: restart to install it, or it installs the next time the browser opens", shownVersion);
        message(text, 6);
        g_free(text);
        if (testYes) {
            spike_log("[update] %s: Restart now", UPDATE_TEST_YES);
            const char* error = install("the test's yes");
            if (error)
                spike_log("[update] install: %s", error);
        }
    } else if (now == U_AVAILABLE && canInstall && testYes) {
        spike_log("[update] %s: Update", UPDATE_TEST_YES);
        g_idle_add(openPageOnMain, NULL);
        const char* error = startJob("download");
        if (error)
            spike_log("[update] download: %s", error);
    } else if (now == U_AVAILABLE && openWhenFound && !offered) {
        offered = TRUE;
        /* The page offers what this browser can install; a store install
         * gets a message (the store may list the release a day later). */
        if (canInstall || manualCheck) {
            spike_log("[update] %s is available: the update page opens", shownVersion);
            g_idle_add(openPageOnMain, NULL);
        } else {
            spike_log("[update] %s is available; %s", shownVersion, managed ? "ProsperoStore manages this install" : "this install can't update itself");
            char* text = g_strdup_printf(managed ? "Parallax Browser %s is out: update it in ProsperoStore" : "Parallax Browser %s is out: see Settings, Updates", shownVersion);
            message(text, 6);
            g_free(text);
        }
    } else if (now == U_FAILED && download)
        message("The update couldn't be downloaded: see the update page", 5);
    openWhenFound = FALSE;
    manualCheck = FALSE;
    g_free(shownVersion);
    g_free(error);
    g_key_file_unref(result);
    g_free(job->mode);
    g_free(job);
    return G_SOURCE_REMOVE;
}

/* Main loop: NULL, or why not. */
static const char* startJob(const char* mode)
{
    if (jobRunning)
        return "an update check or download is already running";
    if (!*running)
        return "the browser's version isn't known";
    WPEProcessManager* manager = wpe_process_manager_get_default();
    if (!manager)
        return "the browser isn't ready yet";
    g_mkdir_with_parents(UPDATE_WORK, 0777);
    chmod(UPDATE_WORK, 0777); /* the job runs under another uid */
    unlink(UPDATE_WORK "/result.ini");
    char* source = fileLine(UPDATE_SOURCE_TEST);
    char* storeRoot = fileLine(UPDATE_STORE_ROOT_TEST);
    GKeyFile* file = g_key_file_new();
    g_key_file_set_string(file, "job", "mode", mode);
    g_key_file_set_string(file, "job", "title", UPDATE_TITLE);
    g_key_file_set_string(file, "job", "running", running);
    g_key_file_set_string(file, "job", "source", source ? source : UPDATE_SOURCE);
    g_key_file_set_boolean(file, "job", "test-source", source != NULL);
    g_key_file_set_string(file, "job", "work", UPDATE_WORK);
    g_key_file_set_string(file, "job", "store-root", storeRoot ? storeRoot : UPDATE_STORE_ROOT);
    g_key_file_set_string(file, "job", "result", UPDATE_WORK "/result.ini");
    gsize length = 0;
    char* text = g_key_file_to_data(file, &length, NULL);
    gboolean written = g_file_set_contents(UPDATE_WORK "/job.ini", text, (gssize)length, NULL);
    chmod(UPDATE_WORK "/job.ini", 0666);
    g_free(text);
    g_key_file_unref(file);
    if (source || storeRoot)
        spike_log("[update] test settings: source %s, ProsperoStore folder %s", source ? source : "GitHub", storeRoot ? storeRoot : UPDATE_STORE_ROOT);
    g_free(source);
    g_free(storeRoot);
    if (!written)
        return "the job file couldn't be written";
    Job* job = g_new0(Job, 1);
    job->mode = g_strdup(mode);
    g_mutex_lock(&lock);
    state = !strcmp(mode, "download") ? U_DOWNLOADING : U_CHECKING;
    progressDone = progressTotal = 0;
    progressPhase[0] = 0;
    setString(&failure, NULL);
    g_mutex_unlock(&lock);
    jobRunning = TRUE;
    if (!wpe_process_manager_ps5_run_task(manager, "update", UPDATE_WORK "/job.ini", jobOutput, jobEnded, job)) {
        jobRunning = FALSE;
        g_mutex_lock(&lock);
        state = U_FAILED;
        setString(&failure, "the update job couldn't start");
        g_mutex_unlock(&lock);
        g_free(job->mode);
        g_free(job);
        return "the update job couldn't start";
    }
    spike_log("[update] %s job started (running %s)", mode, runningShown);
    return NULL;
}

/* Main loop: the staged release goes in through the restart helper. */
static const char* install(const char* from)
{
    g_mutex_lock(&lock);
    char* where = state == U_READY && staging ? g_strdup(staging) : NULL;
    char* version = g_strdup(latest);
    g_mutex_unlock(&lock);
    if (!where) {
        g_free(version);
        return "no update is ready";
    }
    const char* error = ps5wk_restart_for_update(where, from);
    spike_log("[update] installing %s from %s (%s): %s", version ? version : "?", where, from, error ? error : "the browser closes");
    if (!error) {
        g_mutex_lock(&lock);
        state = U_INSTALLING;
        g_mutex_unlock(&lock);
        char* text = g_strdup_printf("Updating to %s…", version ? version : "the new version");
        message(text, 10);
        g_free(text);
    }
    g_free(where);
    g_free(version);
    return error;
}

static gboolean checkOnMain(gpointer data)
{
    (void)data;
    openWhenFound = TRUE;
    const char* error = startJob("check");
    if (error) {
        spike_log("[update] the start's check didn't run: %s", error);
        openWhenFound = FALSE;
    }
    return G_SOURCE_REMOVE;
}

static gboolean installAtStartOnMain(gpointer data)
{
    (void)data;
    const char* error = install("the start (an update the user said yes to)");
    if (error) {
        g_mutex_lock(&lock);
        state = U_READY; /* Restart now on the update page can try again */
        g_mutex_unlock(&lock);
        g_timeout_add_seconds(UPDATE_CHECK_DELAY_S, checkOnMain, NULL);
    }
    return G_SOURCE_REMOVE;
}

static gpointer removeTreeThread(gpointer data)
{
    char* path = data;
    if (ps5zip_remove_tree(path))
        spike_log("[update] %s couldn't be removed (errno %d)", path, errno);
    g_free(path);
    return NULL;
}

/* ---- the routes (any thread) ---- */

typedef struct {
    char* action;
    char* from;
} Action;

static gboolean actionOnMain(gpointer data)
{
    Action* a = data;
    const char* error = NULL;
    if (!strcmp(a->action, "check")) {
        openWhenFound = TRUE;
        manualCheck = TRUE;
        offered = FALSE;
        error = startJob("check");
    } else if (!strcmp(a->action, "download"))
        error = startJob("download");
    else if (!strcmp(a->action, "install"))
        error = install(a->from);
    if (error) {
        spike_log("[update] %s from %s: %s", a->action, a->from, error);
        if (!strcmp(a->action, "install")) {
            g_mutex_lock(&lock);
            setString(&failure, error);
            g_mutex_unlock(&lock);
        }
    }
    g_free(a->action);
    g_free(a->from);
    g_free(a);
    return G_SOURCE_REMOVE;
}

const char* ps5wk_updates_action(const char* action, const char* from)
{
    if (!action || (strcmp(action, "check") && strcmp(action, "download") && strcmp(action, "install") && strcmp(action, "later")))
        return "do=check, do=download, do=install or do=later";
    g_mutex_lock(&lock);
    UpdateState now = state;
    gboolean storeManaged = managed;
    gboolean problem = installProblem != NULL;
    g_mutex_unlock(&lock);
    if (!strcmp(action, "later")) {
        spike_log("[update] later, from %s", from);
        return NULL;
    }
    if (!strcmp(action, "download") && (storeManaged || problem))
        return storeManaged ? "ProsperoStore manages this install: update it in ProsperoStore" : "this install can't update itself";
    if (!strcmp(action, "download") && now != U_AVAILABLE && !(now == U_FAILED && failedDownload))
        return "no update to download";
    if (!strcmp(action, "install") && now != U_READY)
        return "no update is ready";
    if (now == U_CHECKING || now == U_DOWNLOADING || now == U_INSTALLING)
        return "busy: wait a moment";
    Action* a = g_new0(Action, 1);
    a->action = g_strdup(action);
    a->from = g_strdup(from);
    g_idle_add(actionOnMain, a);
    return NULL;
}

char* ps5wk_updates_json(void)
{
    GString* out = g_string_new("{\"running\":");
    g_mutex_lock(&lock);
    ps5wk_json_string(out, runningShown);
    g_string_append(out, ",\"state\":");
    ps5wk_json_string(out, stateNames[state]);
#define FIELD(name, value) do { g_string_append(out, ",\"" name "\":"); if (value) ps5wk_json_string(out, value); else g_string_append(out, "null"); } while (0)
    FIELD("latest", latest);
    FIELD("tag", tag);
    FIELD("page", page);
    FIELD("notes", notes);
    FIELD("error", failure);
    FIELD("installProblem", installProblem);
    FIELD("status", status);
    FIELD("lastInstall", lastInstall);
#undef FIELD
    g_string_append_printf(out, ",\"managed\":%s,\"busy\":%s,\"size\":%llu,\"phase\":", managed ? "true" : "false", busy ? "true" : "false",
        (unsigned long long)size);
    ps5wk_json_string(out, progressPhase);
    g_string_append_printf(out, ",\"done\":%llu,\"total\":%llu,\"checkedAt\":%lld,\"retryDownload\":%s}", (unsigned long long)progressDone,
        (unsigned long long)progressTotal, (long long)checkedAt, failedDownload ? "true" : "false");
    g_mutex_unlock(&lock);
    return g_string_free(out, FALSE);
}

/* ---- start ---- */

void ps5wk_updates_init(WPEDisplayPS5* displayIn)
{
    display = displayIn;
    readRunningVersion();
    /* The helper's outcome, from the restart that installed (or didn't). */
    char* outcome = fileLine(UPDATE_WORK "/last-install.txt");
    unlink(UPDATE_WORK "/last-install.txt");
    if (outcome) {
        spike_log("[update] the last install: %s (running %s)", outcome, runningShown);
        char* text = !strncmp(outcome, "ok ", 3) ? g_strdup_printf("Updated to %s", runningShown)
                                                 : g_strdup_printf("The update couldn't be installed: %s", outcome + (strncmp(outcome, "failed ", 7) ? 0 : 7));
        message(text, 6);
        g_mutex_lock(&lock);
        setString(&lastInstall, text);
        g_mutex_unlock(&lock);
        g_free(text);
        g_free(outcome);
    }
    /* A release the user said yes to, ready: it installs now. */
    char* where = fileLine(UPDATE_WORK "/staged.txt");
    if (where) {
        char* readyPath = g_build_filename(where, "ready.txt", NULL);
        gchar* ready = NULL;
        char *version = NULL, *contentVersion = NULL, *from = NULL, *readyPage = NULL, *readyTag = NULL;
        if (g_file_get_contents(readyPath, &ready, NULL, NULL)) {
            version = readyValue(ready, "version");
            contentVersion = readyValue(ready, "content-version");
            from = readyValue(ready, "running");
            readyPage = readyValue(ready, "page");
            readyTag = readyValue(ready, "tag");
        }
        gboolean usable = version && contentVersion && from && !strcmp(from, running) && strcmp(contentVersion, running) > 0;
        spike_log("[update] staged in %s: %s (%s, from %s): %s", where, version ? version : "?", contentVersion ? contentVersion : "?",
            from ? from : "?", usable ? "installing it now" : "not usable: removed");
        if (usable) {
            g_mutex_lock(&lock);
            state = U_READY;
            setString(&staging, where);
            setString(&latest, version);
            setString(&tag, readyTag);
            setString(&page, readyPage);
            g_mutex_unlock(&lock);
            g_timeout_add(1500, installAtStartOnMain, NULL);
        } else {
            unlink(UPDATE_WORK "/staged.txt");
            g_thread_unref(g_thread_new("update-clean", removeTreeThread, g_strdup(where)));
        }
        g_free(version);
        g_free(contentVersion);
        g_free(from);
        g_free(readyPage);
        g_free(readyTag);
        g_free(ready);
        g_free(readyPath);
        g_free(where);
        if (usable)
            return;
    }
    g_timeout_add_seconds(UPDATE_CHECK_DELAY_S, checkOnMain, NULL);
}

/* ---- the update page (GET /update; parallax://update) ---- */

const char ps5wk_update_page_html[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>Parallax Browser update</title>\n"
    "<script>if(location.protocol=='parallax:'||location.hostname=='127.0.0.1'||location.hostname=='localhost')document.documentElement.className='tv'</script>\n"
    "<style>\n"
    ":root{color-scheme:light dark;--bg:#f7f5ff;--fg:#14113d;--muted:#5a5584;--card:#fff;--line:#dcd6f7;--accent:#6d0efd;--on:#fff;--warn:#a04b00;--focus:#d10aa6}\n"
    "@media(prefers-color-scheme:dark){:root{--bg:#07061f;--fg:#f4f2ff;--muted:#b9b5e0;--card:#110f35;--line:#2b2766;--accent:#a77bff;--on:#01001a;--warn:#ffb648;--focus:#ff5cd6}}\n"
    ":root.tv{color-scheme:dark;--bg:#07061f;--fg:#f4f2ff;--muted:#b9b5e0;--card:#110f35;--line:#2b2766;--accent:#a77bff;--on:#01001a;--warn:#ffb648;--focus:#ff5cd6}\n"
    "*{box-sizing:border-box}\n"
    "html{font-size:clamp(16px,1.25vw,26px)}\n"
    "body{margin:0;background:var(--bg);color:var(--fg);font:1rem/1.45 -apple-system,system-ui,sans-serif}\n"
    "main{max-width:56rem;margin:0 auto;padding:1.2rem 16px 3rem}\n"
    "header{display:flex;align-items:center;gap:.8rem;flex-wrap:wrap}\n"
    "h1{font-size:1.6rem;margin:0;flex:1}\n"
    "h1 span{display:block;font-size:.8rem;font-weight:600;color:var(--muted);letter-spacing:.02em}\n"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:.8rem;padding:.9rem 1rem;margin:.8rem 0}\n"
    ".big{font-size:1.25rem;font-weight:600;margin:.2rem 0 .4rem}\n"
    ".help{color:var(--muted);margin:.25rem 0 .6rem}\n"
    ".seg{display:flex;flex-wrap:wrap;gap:.5rem;margin-top:.6rem}\n"
    "button,input{font:inherit;color:inherit}\n"
    "button{min-height:2.6rem;padding:.45rem 1rem;border-radius:.6rem;border:2px solid var(--line);background:transparent;cursor:pointer}\n"
    "button.primary{background:var(--accent);border-color:var(--accent);color:var(--on);font-weight:600}\n"
    "button:focus,input:focus{outline:5px solid var(--focus);outline-offset:3px}\n"
    "input{min-height:2.6rem;padding:.45rem .8rem;border:2px solid var(--line);border-radius:.6rem;background:var(--bg);width:8rem}\n"
    ".bar{height:.8rem;border-radius:.4rem;background:var(--line);overflow:hidden;margin:.6rem 0}\n"
    ".bar div{height:100%;width:0;background:var(--accent)}\n"
    "#notes{white-space:pre-wrap;overflow-wrap:anywhere;max-height:45vh;overflow:auto;margin:.6rem 0 0;padding-top:.6rem;border-top:1px solid var(--line)}\n"
    ".warn{color:var(--warn)}\n"
    "</style></head><body><main>\n"
    "<header><h1><span>Parallax Browser</span>Update</h1><button id=\"back\" hidden>Back to the page</button></header>\n"
    "<div id=\"pinbox\" class=\"card\" hidden><p>This PS5 asks for the link server's PIN.</p><div class=\"seg\"><input id=\"pin\" inputmode=\"numeric\" autocomplete=\"off\" placeholder=\"PIN\"><button id=\"pinok\">OK</button></div></div>\n"
    "<div class=\"card\" id=\"card\"><p class=\"big\" id=\"head\">Looking for a newer version…</p><p class=\"help\" id=\"text\"></p>\n"
    "<div class=\"bar\" id=\"bar\" hidden><div id=\"fill\"></div></div>\n"
    "<div class=\"seg\" id=\"buttons\"></div><div id=\"notes\" hidden></div></div>\n"
    "<p class=\"help\" id=\"version\"></p>\n"
    "</main><script>\n"
    "var U=null,pin='',tv=document.documentElement.className=='tv',focusId=null;\n"
    "try{pin=sessionStorage.getItem('ps5pin')||''}catch(e){}\n"
    "var query=new URLSearchParams(location.search);if(query.get('pin'))pin=query.get('pin');\n"
    "function $(id){return document.getElementById(id)}\n"
    "function headers(form){var h={'Accept':'application/json'};if(pin)h['X-PS5-PIN']=pin;if(form)h['Content-Type']='application/x-www-form-urlencoded';return h}\n"
    "function post(path,body){return fetch(path,{method:'POST',headers:headers(true),body:body,cache:'no-store'}).then(function(r){return r.json().catch(function(){return{ok:r.ok}})})}\n"
    "function closePage(){if(!tv){history.back();return}post('/browser/page','action=close').then(function(j){if(j.ok===false)history.back()},function(){history.back()})}\n"
    "function act(what){return post('/update','do='+what).then(function(j){if(j.ok===false&&j.error){$('text').textContent=j.error}load()})}\n"
    "function openTab(url){if(!tv){window.open(url,'_blank');return}post('/browser/tab','action=new&url='+encodeURIComponent(url))}\n"
    "function mb(n){return Math.round(n/1048576)+' MB'}\n"
    "function button(text,primary,run){var b=document.createElement('button');b.textContent=text;if(primary)b.className='primary';b.onclick=run;$('buttons').appendChild(b);return b}\n"
    "function render(){\n"
    "  var u=U,head='',text='',busy=false;$('buttons').textContent='';\n"
    "  $('version').textContent='This is Parallax Browser '+u.running+'.'+(u.checkedAt?' Last checked '+new Date(u.checkedAt*1000).toLocaleString()+'.':'');\n"
    "  var notes=(u.state=='available'||u.state=='failed')&&u.notes;$('notes').hidden=!notes;$('notes').textContent=notes||'';\n"
    "  var whatsNew=function(){if(u.page)button('What\\u2019s new',false,function(){openTab(u.page)})};\n"
    "  if(u.state=='checking'||u.state=='idle'){head='Looking for a newer version…';busy=true}\n"
    "  else if(u.state=='up-to-date'){head='Parallax Browser is up to date.';text=u.lastInstall||'';button('Check again',false,function(){act('check')});button('Close',false,closePage)}\n"
    "  else if(u.state=='available'&&u.managed){head='Version '+u.latest+' is available.';text='ProsperoStore installed the browser and keeps it up to date: update it in ProsperoStore. It can list a new version a day or so after its release.';whatsNew();button('Close',true,closePage)}\n"
    "  else if(u.state=='available'&&u.installProblem){head='Version '+u.latest+' is available.';text='The browser can\\u2019t update itself here: '+u.installProblem+'.';whatsNew();button('Close',true,closePage)}\n"
    "  else if(u.state=='available'){head='Version '+u.latest+' is available.';text='The download is '+mb(u.size)+'. You can keep browsing while it downloads; the browser asks before it restarts.';button('Update',true,function(){act('download')});whatsNew();button('Later',false,function(){post('/update','do=later');closePage()})}\n"
    "  else if(u.state=='downloading'){var unpack=u.phase=='unpack',pc=u.total?Math.floor(u.done*100/u.total):0;head=(unpack?'Unpacking ':'Downloading ')+u.latest+(u.total?': '+pc+'%':'')+'…';\n"
    "    text=(unpack?'':(u.total?mb(u.done)+' of '+mb(u.total)+'. ':''))+'You can go back to the page: the browser says when it\\u2019s ready.';$('fill').style.width=pc+'%';busy=true;button('Back to the page',false,closePage)}\n"
    "  else if(u.state=='ready'){head=u.latest+' is ready.';text='Restart now to install it, or it installs the next time the browser opens. Your tabs come back.'+(u.error?' ('+u.error+')':'');button('Restart now',true,function(){act('install')});button('Next time',false,function(){post('/update','do=later');closePage()})}\n"
    "  else if(u.state=='installing'){head='Updating to '+u.latest+'…';text='The browser closes and opens again on this page\\u2019s tab.';busy=true}\n"
    "  else{head=u.retryDownload?'The update couldn\\u2019t be downloaded.':'The browser couldn\\u2019t check for updates.';text=u.error||'';$('text').className='help warn';\n"
    "    button('Try again',true,function(){act(u.retryDownload?'download':'check')});button('Close',false,closePage)}\n"
    "  if(u.state!='failed')$('text').className='help';\n"
    "  $('head').textContent=head;$('text').textContent=text;$('bar').hidden=u.state!='downloading';\n"
    "  var first=$('buttons').querySelector('button');if(tv&&first&&focusId!=u.state){focusId=u.state;first.focus()}\n"
    "  return busy;\n"
    "}\n"
    "function load(){fetch('/update.json',{headers:headers(false),cache:'no-store'}).then(function(r){if(r.status==403){$('pinbox').hidden=false;return null}return r.json()}).then(function(j){\n"
    "  if(!j)return;U=j;$('pinbox').hidden=true;if(render())setTimeout(load,1000)}).catch(function(){$('head').textContent='The PS5 did not answer.';setTimeout(load,3000)})}\n"
    "$('back').hidden=!tv;$('back').onclick=closePage;\n"
    "$('pinok').onclick=function(){pin=$('pin').value.trim();try{sessionStorage.setItem('ps5pin',pin)}catch(e){}load()};\n"
    "load();\n"
    "</script></body></html>\n";
