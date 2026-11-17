/* ps5wk-relaunch.c: starts the browser title again (Settings' Restart
 * browser; with pages in the browser process also the page settings and
 * Safer browsing). An elfldr payload that the title sends itself
 * (wpe_process_manager_ps5_run_task_elf, apps/title/ps5browser.c
 * restartBrowser), so it needs no launcher payload or network port.
 *
 * Why a payload: a title that has used the GPU and ends itself shows a
 * system error (spikes/common/title_exit.c), and a title can't start
 * another app. A payload can SIGKILL the title (nothing on screen) and start
 * it with sceLncUtilLaunchApp, as tools/launcher does on 13.60.
 *
 * Steps:
 *  1. stdin has the launch header (PS5LaunchProtocol.h):
 *       PS5WK-LAUNCH 1 relaunch 0 <title id>:<title pid>:<go file>
 *  2. Prints "[relaunch] ready" (back to the title on the elfldr
 *     connection). The title then winds down: tabs saved, media stopped,
 *     downloads paused, the page it will come back to written down.
 *  3. When the title has wound down it creates <go file>
 *     (spike_title_exit_helper in ps5browser.c), and waits.
 *  4. This SIGKILLs the title, waits until it is gone, and starts it again.
 * If <go file> doesn't appear within 60 s, or the title goes by itself, it
 * does nothing. The log: /data/ps5webkit/relaunch.log (the title's
 * connection is gone after step 4).
 *
 * An update (port/webkit/UPDATES.md, "Install"): the header's type is
 * "update" and its argument <title id>:<title pid>:<go file>:<staging>,
 * where <staging> is the folder the update job unpacked the release into
 * (<staging>/<title id>, <staging>/ready.txt). Between steps 4's kill and
 * start, once the title is gone, this checks again that ProsperoStore
 * doesn't manage the install and that the install holds the version that
 * was running, then moves each top-level entry of the install that the
 * release has into <staging>/../previous and the release's in (renames;
 * undone if one fails), and refreshes the system's copies of sce_sys. The
 * outcome goes to /data/ps5webkit/update/last-install.txt for the title.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#define LOG_PATH "/data/ps5webkit/relaunch.log"

typedef struct {
    uint32_t size;
    uint32_t user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint32_t check_flag;
} lnc_app_param_t;

int sceUserServiceInitialize(void*);
int sceUserServiceGetForegroundUser(int32_t*);
int sceLncUtilLaunchApp(const char* title_id, const char** argv, lnc_app_param_t* param);
int sceLncUtilGetAppIdOfRunningBigApp(void);

/* sceKernelGetAppInfo fills this; only app_id and title_id are used
 * (tools/launcher/launcher.c). */
typedef struct {
    uint32_t app_id;
    uint64_t unknown1;
    char title_id[14];
    char unknown2[0x3c];
} app_info_t;
int sceKernelGetAppInfo(pid_t pid, app_info_t* info);

#define UPDATE_DIR "/data/ps5webkit/update"
#define UPDATE_RESULT UPDATE_DIR "/last-install.txt"
#define STORE_ROOT "/data/prosperostore"
#define STORE_ROOT_TEST "/data/ps5webkit/update-store-root"

static FILE* logFile;
static int titleConnected = 1;

static void say(const char* fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (titleConnected) {
        printf("[relaunch] %s\n", line);
        fflush(stdout);
    }
    if (logFile) {
        fprintf(logFile, "%ld %s\n", (long)time(NULL), line);
        fflush(logFile);
    }
}

/* The app id of pid when it is still title, else 0. */
static uint32_t appOf(pid_t pid, const char* title)
{
    app_info_t info;
    memset(&info, 0, sizeof info);
    if (sceKernelGetAppInfo(pid, &info) || strncmp(info.title_id, title, 9))
        return 0;
    return info.app_id;
}

/* tools/launcher's launch(): refused calls come back as negative SCE
 * errors; try again with system authids. */
static int launch(const char* title)
{
    static const uint64_t retryAuthids[] = {
        0x4800000000010003ull, /* elfldr's ptrace authid */
        0x4801000000000013ull,
        0x4800000000000010ull,
    };
    int32_t user = 0;
    if (sceUserServiceGetForegroundUser(&user) != 0 || user <= 0)
        user = -1;
    lnc_app_param_t p;
    memset(&p, 0, sizeof p);
    p.size = sizeof p;
    p.user_id = (uint32_t)user;
    uint64_t own = kernel_get_ucred_authid(getpid());
    int r = sceLncUtilLaunchApp(title, NULL, &p);
    for (size_t i = 0; r < 0 && i < sizeof retryAuthids / sizeof *retryAuthids; i++) {
        kernel_set_ucred_authid(getpid(), retryAuthids[i]);
        r = sceLncUtilLaunchApp(title, NULL, &p);
        kernel_set_ucred_authid(getpid(), own);
    }
    return r;
}

/* ---- updates ---- */

static int readSmall(const char* path, char* buffer, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buffer, size - 1);
    close(fd);
    if (n < 0 || (size_t)n >= size - 1)
        return -1;
    buffer[n] = 0;
    return 0;
}

/* key=value lines (ready.txt). */
static int lineValue(const char* text, const char* key, char* out, size_t size)
{
    size_t length = strlen(key);
    for (const char* line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        if (!strncmp(line, key, length) && line[length] == '=') {
            const char* value = line + length + 1;
            size_t n = strcspn(value, "\r\n");
            if (n >= size)
                return -1;
            memcpy(out, value, n);
            out[n] = 0;
            return 0;
        }
    }
    return -1;
}

/* "key": "value" in a small JSON object with plain string values (param.json,
 * ProsperoStore's receipt and journal): enough for the fields compared. */
static int jsonValue(const char* text, const char* key, char* out, size_t size)
{
    char quoted[64];
    snprintf(quoted, sizeof quoted, "\"%s\"", key);
    const char* p = strstr(text, quoted);
    if (!p)
        return -1;
    p += strlen(quoted);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p++ != ':')
        return -1;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p++ != '"')
        return -1;
    size_t n = 0;
    while (p[n] && p[n] != '"' && p[n] != '\\')
        n++;
    if (p[n] != '"' || n >= size)
        return -1;
    memcpy(out, p, n);
    out[n] = 0;
    return 0;
}

static int paramOf(const char* folder, char* title, size_t titleSize, char* version, size_t versionSize)
{
    char path[512], text[16384];
    snprintf(path, sizeof path, "%s/sce_sys/param.json", folder);
    return readSmall(path, text, sizeof text) || jsonValue(text, "titleId", title, titleSize) || jsonValue(text, "contentVersion", version, versionSize) ? -1 : 0;
}

static int removeTree(const char* path)
{
    struct stat st;
    if (lstat(path, &st))
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    DIR* dir = opendir(path);
    if (!dir)
        return -1;
    int result = 0;
    struct dirent* d;
    while ((d = readdir(dir))) {
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
            continue;
        char child[1024];
        snprintf(child, sizeof child, "%s/%s", path, d->d_name);
        if (removeTree(child))
            result = -1;
    }
    closedir(dir);
    return rmdir(path) ? -1 : result;
}

static int copyFile(const char* from, const char* to)
{
    int in = open(from, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (out < 0) {
        close(in);
        return -1;
    }
    static char buffer[64 * 1024];
    int ok = 1;
    for (;;) {
        ssize_t n = read(in, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = !n;
            break;
        }
        if (write(out, buffer, (size_t)n) != n) {
            ok = 0;
            break;
        }
    }
    close(in);
    return close(out) || !ok ? -1 : 0;
}

/* ShadowMountPlus copies an app's sce_sys when it first registers it; the
 * home screen shows those copies. Best effort, as ProsperoStore does. */
static void refreshRegistered(const char* title, const char* install)
{
    char source[512], app[256], folders[2][256];
    snprintf(source, sizeof source, "%s/sce_sys", install);
    snprintf(app, sizeof app, "/user/app/%s", title);
    snprintf(folders[0], sizeof folders[0], "/user/appmeta/%s", title);
    snprintf(folders[1], sizeof folders[1], "%s/sce_sys", app);
    int copied = 0, failed = 0;
    for (int f = 0; f < 2; f++) {
        struct stat st;
        if (stat(folders[f], &st) || !S_ISDIR(st.st_mode))
            continue;
        DIR* dir = opendir(source);
        struct dirent* d;
        while (dir && (d = readdir(dir))) {
            char from[768], to[768];
            snprintf(from, sizeof from, "%s/%s", source, d->d_name);
            if (d->d_name[0] == '.' || stat(from, &st) || !S_ISREG(st.st_mode))
                continue;
            snprintf(to, sizeof to, "%s/%s", folders[f], d->d_name);
            if (copyFile(from, to))
                failed++;
            else
                copied++;
        }
        if (dir)
            closedir(dir);
    }
    char icon[512], from[600];
    struct stat st;
    snprintf(icon, sizeof icon, "%s/icon0.png", app);
    snprintf(from, sizeof from, "%s/icon0.png", source);
    if (!stat(icon, &st) && copyFile(from, icon))
        failed++;
    say("update: the system's copies of sce_sys refreshed (%d files, %d failed)", copied, failed);
}

static void installResult(const char* text)
{
    int fd = open(UPDATE_RESULT, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        (void)!write(fd, text, strlen(text));
        close(fd);
    }
}

/* NULL, or why the update must not be installed now. */
static const char* updateRefusal(const char* title, const char* install, const char* running)
{
    static char why[256];
    char storeRoot[256] = STORE_ROOT, text[16384], value[1024];
    if (!readSmall(STORE_ROOT_TEST, storeRoot, sizeof storeRoot))
        storeRoot[strcspn(storeRoot, "\r\n")] = 0;
    char id[16], version[16];
    if (paramOf(install, id, sizeof id, version, sizeof version) || strcmp(id, title))
        return "the installed folder no longer holds the browser";
    if (strcmp(version, running)) {
        snprintf(why, sizeof why, "the installed version changed (%s, not %s)", version, running);
        return why;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/journal.json", storeRoot);
    if (!readSmall(path, text, sizeof text) && !jsonValue(text, "titleId", value, sizeof value) && !strcmp(value, title))
        return "ProsperoStore has an operation on the browser in progress";
    snprintf(path, sizeof path, "%s/receipts/%s.json", storeRoot, title);
    char location[1024], receiptVersion[16];
    if (!readSmall(path, text, sizeof text) && !jsonValue(text, "titleId", value, sizeof value) && !strcmp(value, title)
        && !jsonValue(text, "location", location, sizeof location) && !jsonValue(text, "contentVersion", receiptVersion, sizeof receiptVersion)) {
        char target[1100];
        snprintf(target, sizeof target, "%s/%s", location, title);
        if (!strcmp(target, install) && !strcmp(receiptVersion, version))
            return "ProsperoStore manages this install";
    }
    return NULL;
}

#define MAX_ENTRIES 256

/* Once the title is gone: the release's top-level entries in place of the
 * install's. 0, or -1 (nothing changed, or every move undone). */
static int installUpdate(const char* title, const char* staging)
{
    char readyPath[512], ready[4096];
    char version[32], contentVersion[16], running[16], install[512];
    snprintf(readyPath, sizeof readyPath, "%s/ready.txt", staging);
    if (readSmall(readyPath, ready, sizeof ready) || lineValue(ready, "version", version, sizeof version)
        || lineValue(ready, "content-version", contentVersion, sizeof contentVersion) || lineValue(ready, "running", running, sizeof running)
        || lineValue(ready, "install", install, sizeof install)) {
        say("update: %s is missing or incomplete: not installed", readyPath);
        installResult("failed the update's files are incomplete\n");
        return -1;
    }
    char app[600], id[16], stagedVersion[16];
    snprintf(app, sizeof app, "%s/%s", staging, title);
    const char* refusal = updateRefusal(title, install, running);
    if (!refusal && (paramOf(app, id, sizeof id, stagedVersion, sizeof stagedVersion) || strcmp(id, title) || strcmp(stagedVersion, contentVersion)))
        refusal = "the unpacked release isn't the expected version";
    if (refusal) {
        say("update to %s not installed: %s", version, refusal);
        char text[320];
        snprintf(text, sizeof text, "failed %s\n", refusal);
        installResult(text);
        removeTree(staging);
        unlink(UPDATE_DIR "/staged.txt");
        return -1;
    }
    /* <staging>/../previous: the old files, kept until the next update. */
    char previous[600];
    snprintf(previous, sizeof previous, "%s", staging);
    char* slash = strrchr(previous, '/');
    if (!slash) {
        installResult("failed the staging folder is odd\n");
        return -1;
    }
    snprintf(slash, sizeof previous - (size_t)(slash - previous), "/previous");
    removeTree(previous);
    if (mkdir(previous, 0777) && errno != EEXIST) {
        say("update: %s couldn't be made (errno %d)", previous, errno);
        installResult("failed the folder for the old version couldn't be made\n");
        return -1;
    }
    static char names[MAX_ENTRIES][256];
    int count = 0;
    DIR* dir = opendir(app);
    struct dirent* d;
    while (dir && (d = readdir(dir))) {
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
            continue;
        if (count == MAX_ENTRIES || strlen(d->d_name) >= sizeof names[0]) {
            closedir(dir);
            say("update: too many or too long entries in %s", app);
            installResult("failed the release has too many top-level entries\n");
            return -1;
        }
        snprintf(names[count++], sizeof names[0], "%s", d->d_name);
    }
    if (dir)
        closedir(dir);
    if (!count) {
        installResult("failed the release is empty\n");
        return -1;
    }
    static char movedOut[MAX_ENTRIES], movedIn[MAX_ENTRIES];
    memset(movedOut, 0, sizeof movedOut);
    memset(movedIn, 0, sizeof movedIn);
    int failedAt = -1, failure = 0;
    for (int i = 0; i < count && failedAt < 0; i++) {
        char from[1024], to[1024], old[1024];
        snprintf(to, sizeof to, "%s/%s", install, names[i]);
        snprintf(old, sizeof old, "%s/%s", previous, names[i]);
        snprintf(from, sizeof from, "%s/%s", app, names[i]);
        struct stat st;
        if (!lstat(to, &st)) {
            if (rename(to, old)) {
                failedAt = i;
                failure = errno;
                break;
            }
            movedOut[i] = 1;
        }
        if (rename(from, to)) {
            failedAt = i;
            failure = errno;
            break;
        }
        movedIn[i] = 1;
    }
    if (failedAt >= 0) {
        say("update: moving %s failed (errno %d): undoing", names[failedAt], failure);
        for (int i = failedAt; i >= 0; i--) {
            char from[1024], to[1024], old[1024];
            snprintf(to, sizeof to, "%s/%s", install, names[i]);
            snprintf(old, sizeof old, "%s/%s", previous, names[i]);
            snprintf(from, sizeof from, "%s/%s", app, names[i]);
            if (movedIn[i] && rename(to, from))
                say("update: undo: %s couldn't go back (errno %d)", to, errno);
            if (movedOut[i] && rename(old, to))
                say("update: undo: %s couldn't come back (errno %d)", old, errno);
        }
        char text[256];
        snprintf(text, sizeof text, "failed a file couldn't be moved (errno %d)\n", failure);
        installResult(text);
        removeTree(staging);
        unlink(UPDATE_DIR "/staged.txt");
        return -1;
    }
    chmod(install, 0777);
    say("update: %s installed in %s (%d entries; the old ones in %s)", version, install, count, previous);
    refreshRegistered(title, install);
    char text[96];
    snprintf(text, sizeof text, "ok %s\n", version);
    installResult(text);
    removeTree(staging);
    unlink(UPDATE_DIR "/staged.txt");
    return 0;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    logFile = fopen(LOG_PATH, "a");
    char line[512];
    if (!fgets(line, sizeof line, stdin)) {
        say("no launch header on stdin: the browser title starts this payload");
        return 1;
    }
    char magic[32], type[16], argument[300];
    int version = 0;
    unsigned long long processIdentifier = 0;
    char title[16] = "";
    int pid = 0;
    char go[256] = "", staging[256] = "";
    int update = 0;
    if (sscanf(line, "%31s %d %15s %llu %299s", magic, &version, type, &processIdentifier, argument) != 5
        || strcmp(magic, "PS5WK-LAUNCH") || (strcmp(type, "relaunch") && strcmp(type, "update"))
        || ((update = !strcmp(type, "update"))
                ? sscanf(argument, "%9[A-Z0-9]:%d:%255[^:]:%255s", title, &pid, go, staging) != 4 || staging[0] != '/'
                : sscanf(argument, "%9[A-Z0-9]:%d:%255s", title, &pid, go) != 3)
        || strncmp(title, "PPSA", 4) || pid <= 0) {
        say("bad launch header: \"%.*s\"", (int)strcspn(line, "\n"), line);
        return 1;
    }
    sceUserServiceInitialize(NULL);
    uint32_t app = appOf(pid, title);
    if (!app) {
        say("%s is not pid %d: nothing to restart", title, pid);
        return 1;
    }
    unlink(go);
    say("ready: %s pid %d (app 0x%x); waiting for %s%s%s", title, pid, app, go, update ? "; then the update in " : "", update ? staging : "");

    struct stat st;
    int waited = 0;
    for (; waited < 60000; waited += 50) {
        if (!stat(go, &st))
            break;
        if (!appOf(pid, title)) {
            say("%s went by itself after %d ms: not starting it again", title, waited);
            return 0;
        }
        usleep(50 * 1000);
    }
    if (waited >= 60000) {
        say("%s didn't ask within 60 s: not restarting it", title);
        return 1;
    }
    unlink(go);
    titleConnected = 0; /* its end of stdout goes with it */
    if (kill(pid, SIGKILL)) {
        say("SIGKILL to %s pid %d failed: %s", title, pid, strerror(errno));
        return 1;
    }
    /* Gone: the process, and the system's idea of the app in front. What
     * the system reports meanwhile goes to the log ("big app" ids). */
    int gone = -1, last = (int)app, noApp = -1;
    char trail[160] = "";
    size_t o = 0;
    for (int t = 0; t < 10000; t += 50) {
        int running = sceLncUtilGetAppIdOfRunningBigApp();
        if (running != last && o < sizeof trail) {
            o += snprintf(trail + o, sizeof trail - o, " %d ms: 0x%x", t, (unsigned)running);
            last = running;
        }
        if (gone < 0 && !appOf(pid, title) && running != (int)app)
            gone = t;
        if (gone >= 0 && running <= 0 && running != (int)app) {
            noApp = t;
            break;
        }
        usleep(50 * 1000);
    }
    if (gone < 0) {
        say("%s pid %d still there 10 s after SIGKILL", title, pid);
        return 1;
    }
    /* Starting it straight after the kill showed "Cannot start the game or
     * app" while the system still dealt with the old one (it started all
     * the same). Wait a moment more: /data/ps5webkit/relaunch-delay-ms
     * (default 1000) sets how long. */
    if (update)
        installUpdate(title, staging); /* the old version starts again if it fails */
    int delay = 1000;
    FILE* f = fopen("/data/ps5webkit/relaunch-delay-ms", "r");
    if (f) {
        if (fscanf(f, "%d", &delay) != 1 || delay < 0 || delay > 10000)
            delay = 1000;
        fclose(f);
    }
    say("%s gone after %d ms, no app in front after %d ms (big app:%s); starting it in %d ms", title, gone, noApp, o ? trail : " unchanged", delay);
    usleep((useconds_t)delay * 1000);
    int r = -1;
    for (int attempt = 1; attempt <= 10; attempt++) {
        r = launch(title);
        if (r >= 0) {
            say("%s started again (app 0x%x, attempt %d)", title, (unsigned)r, attempt);
            return 0;
        }
        usleep(500 * 1000);
    }
    say("starting %s again failed: 0x%08x", title, (unsigned)r);
    return 1;
}
