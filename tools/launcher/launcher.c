/* launcher.c - start and stop registered titles from the Mac, so agents can
 * run build -> launch -> read log -> fix without anyone pressing Play.
 *
 * An elfldr payload that stays resident and listens on TCP LAUNCHER_PORT.
 * One command per line, one reply line per command ("OK ..." or "ERR ..."):
 *
 *   status                 the running big app, if any: "OK <appid> <TITLEID>"
 *   launch <TITLEID> [arg] start a registered title (fails if one is running)
 *   kill                   stop the running big app and wait until it is gone
 *   quit                   stop the launcher
 *
 * tools/launch.sh is the client. It sends this payload first if nothing
 * answers on the port. Titles still have to be registered once (PS5Upload).
 *
 * On 13.60 (checked 2026-10-27) sceLncUtilLaunchApp works with the payload's
 * own authid; the system authids below are only a fallback. No kill API
 * accepts a payload, so kill sends SIGKILL to the title's process, which the
 * system accepts at once and lets the next launch go ahead.
 */
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#define LAUNCHER_PORT 9031
#define LOG_PATH "/data/ps5webkit/launcher.log"

typedef struct {
    uint32_t size;
    uint32_t user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint32_t check_flag;
} lnc_app_param_t;

int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(int32_t *);
int sceLncUtilLaunchApp(const char *title_id, const char **argv, lnc_app_param_t *param);
int sceLncUtilKillApp(uint32_t app_id);
int sceLncUtilGetAppIdOfRunningBigApp(void);
int sceLncUtilGetAppTitleId(uint32_t app_id, char *title_id);

static FILE *log_file;
static uint64_t own_authid;

/* Goes to the launcher log on the console and to the client, if any. */
static void say(int fd, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (log_file) {
        fprintf(log_file, "%ld %s\n", (long)time(NULL), line);
        fflush(log_file);
    }
    if (fd >= 0) {
        size_t n = strlen(line);
        line[n] = '\n';
        (void)!write(fd, line, n + 1);
    }
}

static int running_app(char title_id[16])
{
    int app_id = sceLncUtilGetAppIdOfRunningBigApp();
    title_id[0] = 0;
    if (app_id > 0 && app_id != -1 && sceLncUtilGetAppTitleId((uint32_t)app_id, title_id) != 0)
        strcpy(title_id, "?");
    return app_id;
}

/* Refused calls come back as negative SCE errors; try again with system
 * authids. Returns the call's result; *used gets the authid that worked. */
static const uint64_t retry_authids[] = {
    0x4800000000010003ull, /* elfldr's ptrace authid */
    0x4801000000000013ull,
    0x4800000000000010ull,
};

static int launch(const char *title_id, const char *arg, uint64_t *used)
{
    int32_t user = 0;
    if (sceUserServiceGetForegroundUser(&user) != 0 || user <= 0)
        user = -1;

    const char *argv[2] = { arg, NULL };
    lnc_app_param_t p;
    memset(&p, 0, sizeof p);
    p.size = sizeof p;
    p.user_id = (uint32_t)user;

    *used = own_authid;
    int r = sceLncUtilLaunchApp(title_id, arg ? argv : NULL, &p);
    for (size_t i = 0; r < 0 && i < sizeof retry_authids / sizeof *retry_authids; i++) {
        kernel_set_ucred_authid(getpid(), retry_authids[i]);
        r = sceLncUtilLaunchApp(title_id, arg ? argv : NULL, &p);
        *used = retry_authids[i];
        kernel_set_ucred_authid(getpid(), own_authid);
    }
    return r;
}

/* sceKernelGetAppInfo fills this; only app_id and title_id are used. */
typedef struct {
    uint32_t app_id;
    uint64_t unknown1;
    char title_id[14];
    char unknown2[0x3c];
} app_info_t;
int sceKernelGetAppInfo(pid_t pid, app_info_t *info);

/* PS5 pids are small; probing each is simpler than relying on the kernel's
 * kinfo_proc layout matching the SDK's. */
static pid_t app_pid(int app_id, char title_id[16])
{
    for (pid_t pid = 1; pid < 8192; pid++) {
        app_info_t info;
        memset(&info, 0, sizeof info);
        if (sceKernelGetAppInfo(pid, &info) == 0 && info.app_id == (uint32_t)app_id) {
            memcpy(title_id, info.title_id, 9);
            title_id[9] = 0;
            return pid;
        }
    }
    return -1;
}

/* On 13.60 every kill API refuses a payload: sceLncUtilKillApp and
 * ForceKillApp return 0x80940004 with any authid, sceSystemServiceKillApp
 * blocks 10 s and fails. So ask once politely, then SIGKILL the process. */
static int kill_running(int fd)
{
    char tid[16];
    int app_id = running_app(tid);
    if (app_id <= 0) {
        say(fd, "OK nothing running");
        return 0;
    }
    const char *how = "sceLncUtilKillApp";
    int r = sceLncUtilKillApp((uint32_t)app_id);
    if (r < 0) {
        char ptid[16];
        pid_t pid = app_pid(app_id, ptid);
        if (pid < 0) {
            say(fd, "ERR kill %s (appid 0x%x): 0x%08x, and no process found", tid, app_id, (unsigned)r);
            return -1;
        }
        if (kill(pid, SIGKILL) != 0) {
            say(fd, "ERR kill %s: SIGKILL pid %d: %s", tid, pid, strerror(errno));
            return -1;
        }
        how = "SIGKILL";
        say(-1, "  sceLncUtilKillApp 0x%08x; SIGKILL pid %d (%s)", (unsigned)r, pid, ptid);
    }
    /* The app takes a moment to go; launching before then fails. */
    for (int i = 0; i < 100; i++) {
        char now[16];
        if (running_app(now) != app_id) {
            say(fd, "OK killed %s after %d ms (%s)", tid, i * 100, how);
            return 0;
        }
        usleep(100 * 1000);
    }
    say(fd, "ERR %s still running 10 s after kill", tid);
    return -1;
}

/* Returns 1 to stop the launcher. */
static int command(int fd, char *line)
{
    char *cmd = strtok(line, " \t\r\n");
    char *a1 = strtok(NULL, " \t\r\n");
    char *a2 = strtok(NULL, "\r\n");
    char tid[16];

    if (!cmd)
        return 0;
    if (!strcmp(cmd, "status")) {
        int app_id = running_app(tid);
        if (app_id > 0)
            say(fd, "OK 0x%x %s", app_id, tid);
        else
            say(fd, "OK none");
    } else if (!strcmp(cmd, "launch")) {
        if (!a1 || strlen(a1) != 9) {
            say(fd, "ERR usage: launch <TITLEID> [arg]");
            return 0;
        }
        int app_id = running_app(tid);
        if (app_id > 0) {
            say(fd, "ERR busy 0x%x %s", app_id, tid);
            return 0;
        }
        uint64_t used;
        int r = launch(a1, a2, &used);
        if (r < 0)
            say(fd, "ERR launch %s: 0x%08x", a1, (unsigned)r);
        else
            say(fd, "OK launched %s appid 0x%x (authid 0x%016llx%s)", a1, r,
                (unsigned long long)used, used == own_authid ? ", own" : "");
    } else if (!strcmp(cmd, "kill")) {
        kill_running(fd);
    } else if (!strcmp(cmd, "quit")) {
        say(fd, "OK bye");
        return 1;
    } else {
        say(fd, "ERR unknown command %s (status, launch, kill, quit)", cmd);
    }
    return 0;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    log_file = fopen(LOG_PATH, "a");
    own_authid = kernel_get_ucred_authid(getpid());
    sceUserServiceInitialize(NULL);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(LAUNCHER_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 4) != 0) {
        /* Most likely a launcher is already running. */
        say(-1, "launcher pid %d: port %d: %s; exiting", getpid(), LAUNCHER_PORT, strerror(errno));
        return 1;
    }
    say(-1, "launcher pid %d listening on %d (authid 0x%016llx)", getpid(), LAUNCHER_PORT,
        (unsigned long long)own_authid);

    for (int stop = 0; !stop;) {
        int c = accept(s, NULL, NULL);
        if (c < 0)
            continue;
        FILE *in = fdopen(c, "r");
        char line[256];
        while (!stop && in && fgets(line, sizeof line, in)) {
            say(-1, "> %.*s", (int)strcspn(line, "\r\n"), line);
            stop = command(c, line);
        }
        if (in)
            fclose(in);
        else
            close(c);
    }
    close(s);
    return 0;
}
