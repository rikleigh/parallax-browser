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
 *
 * Closing (O held in the browser): the header's type is "close", its
 * argument as for a restart. Steps 1 to 4 without the start: the title has
 * wound down and is SIGKILLed, which closes it with nothing on screen.
 *
 * A repair (GitHub issue 3): the header's type is "repair" and its argument
 * /data/ps5webkit (nothing else is accepted). The title runs as uid 1 and
 * the payloads as uid 0; a folder a payload made at the default umask is
 * 0755 root's, and the title can't write in it (the runtime's fonts, the ad
 * blocker's lists). This opens every folder under it to all (|0777) and
 * every file (|0666), and ends with
 *   [relaunch] repair done <changed> <failed>
 * Each entry is opened with O_NOFOLLOW and changed through its descriptor
 * (fchmod, fdopendir, openat): links, and files with other hard links, are
 * left as they are, and nothing outside the folder is changed.
 * The title runs it at start when it finds a folder it can't write in
 * (runtime.c, ps5wk_data_repair), then copies its files again.
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

#include "../common/ps5update.h"

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

#define UPDATE_RESULT PS5UPDATE_WORK "/last-install.txt"
#ifdef PS5WK_UPDATE_TESTING
/* A development build only: a folder in place of ProsperoStore's
 * (UPDATES.md, "Testing"). */
#define STORE_ROOT_TEST "/data/ps5webkit/update-store-root"
#endif

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

/* NULL, or why the update must not be installed now: the job's checks
 * again (ps5update.c, the same code), now that the title is gone. */
static const char* updateRefusal(const char* title, const char* install, const char* running)
{
    static char why[256];
    char storeRoot[256] = PS5UPDATE_STORE_ROOT;
#ifdef STORE_ROOT_TEST
    char* test = ps5update_read(STORE_ROOT_TEST, sizeof storeRoot - 1);
    if (test) {
        test[strcspn(test, "\r\n")] = 0;
        if (*test)
            snprintf(storeRoot, sizeof storeRoot, "%s", test);
        free(test);
    }
#endif
    char id[16], version[16];
    if (ps5update_param(install, id, sizeof id, version, sizeof version) || strcmp(id, title))
        return "the installed folder no longer holds the browser";
    if (strcmp(version, running)) {
        snprintf(why, sizeof why, "the installed version changed (%s, not %s)", version, running);
        return why;
    }
    ps5update_store store;
    ps5update_store_check(storeRoot, title, install, &store);
    say("update: ProsperoStore (%s): %s", storeRoot, store.note);
    if (store.busy)
        return "ProsperoStore has an operation on the browser in progress";
    if (store.managed)
        return "ProsperoStore manages this install";
    return NULL;
}

#define MAX_ENTRIES 256

/* Once the title is gone: the release's top-level entries in place of the
 * install's. NULL (installed), or why not (nothing changed, or every move
 * undone). version gets the release's version. */
static const char* installSteps(const char* title, const char* staging, char* version, size_t versionSize)
{
    static char why[320];
    char readyPath[512], contentVersion[16], running[16], install[512];
    snprintf(readyPath, sizeof readyPath, "%s/ready.txt", staging);
    char* ready = ps5update_read(readyPath, 4096);
    int complete = ready && !ps5update_line_value(ready, "version", version, versionSize)
        && !ps5update_line_value(ready, "content-version", contentVersion, sizeof contentVersion)
        && !ps5update_line_value(ready, "running", running, sizeof running) && !ps5update_line_value(ready, "install", install, sizeof install);
    free(ready);
    if (!complete) {
        say("update: %s is missing or incomplete", readyPath);
        return "the update's files are incomplete";
    }
    char app[600], id[16], stagedVersion[16];
    snprintf(app, sizeof app, "%s/%s", staging, title);
    const char* refusal = updateRefusal(title, install, running);
    if (refusal)
        return refusal;
    if (ps5update_param(app, id, sizeof id, stagedVersion, sizeof stagedVersion) || strcmp(id, title) || strcmp(stagedVersion, contentVersion))
        return "the unpacked release isn't the expected version";
    /* <staging>/../previous: the old files, kept until the next update. */
    char previous[600];
    snprintf(previous, sizeof previous, "%s", staging);
    char* slash = strrchr(previous, '/');
    if (!slash || slash == previous)
        return "the staging folder is odd";
    snprintf(slash, sizeof previous - (size_t)(slash - previous), "/previous");
    ps5update_remove_tree(previous);
    if (mkdir(previous, 0777) && errno != EEXIST) {
        snprintf(why, sizeof why, "the folder for the old version couldn't be made (errno %d)", errno);
        return why;
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
            return "the release has too many top-level entries";
        }
        snprintf(names[count++], sizeof names[0], "%s", d->d_name);
    }
    if (dir)
        closedir(dir);
    if (!count)
        return "the release is empty";
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
        snprintf(why, sizeof why, "a file couldn't be moved (errno %d)", failure);
        return why;
    }
    chmod(install, 0777);
    say("update: %s installed in %s (%d entries; the old ones in %s)", version, install, count, previous);
    refreshRegistered(title, install);
    return NULL;
}

/* Every outcome, installed or not, ends here: the result for the title's
 * next start, and the staged release gone (staging and staged.txt), so a
 * failure is never tried again at each start. */
static int installUpdate(const char* title, const char* staging)
{
    char version[32] = "?";
    const char* why = installSteps(title, staging, version, sizeof version);
    char text[400];
    if (why) {
        say("update to %s not installed: %s", version, why);
        snprintf(text, sizeof text, "failed %s\n", why);
    } else
        snprintf(text, sizeof text, "ok %s\n", version);
    installResult(text);
    unlink(PS5UPDATE_WORK "/staged.txt");
    if (ps5update_remove_tree(staging))
        say("update: %s couldn't be removed (errno %d)", staging, errno);
    return why ? -1 : 0;
}

#define REPAIR_ROOT "/data/ps5webkit"
#define REPAIR_DEPTH 32

typedef struct {
    unsigned changed, failed, seen, skipped;
    char firstFailure[200];
} RepairState;

static void repairFailed(RepairState* state, const char* what, const char* path)
{
    if (!state->failed++)
        snprintf(state->firstFailure, sizeof state->firstFailure, "%s %s (errno %d)", what, path, errno);
}

/* fd's mode with want added. Through the open file, never the path: a link
 * swapped in after the check can't send this anywhere else. */
static void openUp(int fd, const struct stat* st, mode_t want, const char* path, RepairState* state)
{
    if ((st->st_mode & want) == want)
        return;
    if (fchmod(fd, (st->st_mode & 07777) | want)) {
        repairFailed(state, "fchmod", path);
        return;
    }
    state->changed++;
}

/* name in the folder parent (path: for the log only). O_NOFOLLOW: a link
 * fails to open (ELOOP) and is left as it is, with what it points to.
 * O_NONBLOCK: a FIFO doesn't wait for a writer. */
static void repairEntry(int parent, const char* name, char* path, size_t size, int depth, RepairState* state)
{
    int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ELOOP || errno == EMLINK || errno == EOPNOTSUPP || errno == ENXIO)
            state->skipped++; /* a link, a socket */
        else
            repairFailed(state, "open", path);
        return;
    }
    struct stat st;
    if (fstat(fd, &st)) {
        repairFailed(state, "fstat", path);
        close(fd);
        return;
    }
    state->seen++;
    if (S_ISREG(st.st_mode)) {
        /* A file also linked from outside the tree keeps its mode. */
        if (st.st_nlink > 1)
            state->skipped++;
        else
            openUp(fd, &st, 0666, path, state);
        close(fd);
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        state->skipped++;
        close(fd);
        return;
    }
    openUp(fd, &st, 0777, path, state);
    if (depth >= REPAIR_DEPTH) {
        repairFailed(state, "too deep:", path);
        close(fd);
        return;
    }
    DIR* dir = fdopendir(fd); /* owns fd from here */
    if (!dir) {
        repairFailed(state, "fdopendir", path);
        close(fd);
        return;
    }
    size_t length = strlen(path);
    struct dirent* d;
    while ((d = readdir(dir))) {
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
            continue;
        if (length + 1 + strlen(d->d_name) + 1 > size) {
            errno = ENAMETOOLONG;
            repairFailed(state, "path", path);
            continue;
        }
        path[length] = '/';
        strcpy(path + length + 1, d->d_name);
        repairEntry(dirfd(dir), d->d_name, path, size, depth + 1, state);
        path[length] = 0;
    }
    closedir(dir);
}

static int repair(const char* root)
{
    if (strcmp(root, REPAIR_ROOT)) {
        say("repair refused: only %s", REPAIR_ROOT);
        return 1;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    static char path[1024];
    snprintf(path, sizeof path, "%s", root);
    RepairState state;
    memset(&state, 0, sizeof state);
    /* The root itself: absolute, and not a link either. */
    repairEntry(AT_FDCWD, root, path, sizeof path, 0, &state);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    say("repair: %u entries in %s, %u opened to all, %u left as they are (links, hard links, sockets), %u failed%s%s, in %ld ms",
        state.seen, root, state.changed, state.skipped, state.failed, state.failed ? ", first: " : "", state.firstFailure,
        (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
    say("repair done %u %u", state.changed, state.failed);
    return state.failed ? 1 : 0;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    /* What this makes in /data/ps5webkit stays open to the title (uid 1;
     * GitHub issue 3). */
    umask(0);
    logFile = fopen(LOG_PATH, "a");
    if (logFile)
        fchmod(fileno(logFile), 0666);
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
    int update = 0, closeOnly = 0;
    if (sscanf(line, "%31s %d %15s %llu %299s", magic, &version, type, &processIdentifier, argument) == 5
        && !strcmp(magic, "PS5WK-LAUNCH") && !strcmp(type, "repair"))
        return repair(argument);
    if (sscanf(line, "%31s %d %15s %llu %299s", magic, &version, type, &processIdentifier, argument) != 5
        || strcmp(magic, "PS5WK-LAUNCH") || (strcmp(type, "relaunch") && strcmp(type, "update") && strcmp(type, "close"))
        || ((update = !strcmp(type, "update"))
                ? sscanf(argument, "%9[A-Z0-9]:%d:%255[^:]:%255s", title, &pid, go, staging) != 4 || staging[0] != '/'
                : sscanf(argument, "%9[A-Z0-9]:%d:%255s", title, &pid, go) != 3)
        || strncmp(title, "PPSA", 4) || pid <= 0) {
        say("bad launch header: \"%.*s\"", (int)strcspn(line, "\n"), line);
        return 1;
    }
    closeOnly = !strcmp(type, "close");
    sceUserServiceInitialize(NULL);
    uint32_t app = appOf(pid, title);
    if (!app) {
        say("%s is not pid %d: nothing to %s", title, pid, closeOnly ? "close" : "restart");
        return 1;
    }
    unlink(go);
    say("ready: %s pid %d (app 0x%x); waiting for %s%s%s", title, pid, app, go,
        update ? "; then the update in " : closeOnly ? "; then it is closed, not started again" : "", update ? staging : "");

    struct stat st;
    int waited = 0;
    for (; waited < 60000; waited += 50) {
        if (!stat(go, &st))
            break;
        if (!appOf(pid, title)) {
            say("%s went by itself after %d ms: %s", title, waited, closeOnly ? "nothing to close" : "not starting it again");
            return 0;
        }
        usleep(50 * 1000);
    }
    if (waited >= 60000) {
        say("%s didn't ask within 60 s: not %s it", title, closeOnly ? "closing" : "restarting");
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
    if (closeOnly) {
        say("%s closed: gone after %d ms, no app in front after %d ms (big app:%s)", title, gone, noApp, o ? trail : " unchanged");
        return 0;
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
