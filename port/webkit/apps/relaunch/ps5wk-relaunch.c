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
 */
#include <errno.h>
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
    char go[256] = "";
    if (sscanf(line, "%31s %d %15s %llu %299s", magic, &version, type, &processIdentifier, argument) != 5
        || strcmp(magic, "PS5WK-LAUNCH") || strcmp(type, "relaunch")
        || sscanf(argument, "%9[A-Z0-9]:%d:%255s", title, &pid, go) != 3 || strncmp(title, "PPSA", 4) || pid <= 0) {
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
    say("ready: %s pid %d (app 0x%x); waiting for %s", title, pid, app, go);

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
