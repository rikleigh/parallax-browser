/* launcher.c - start and stop registered titles from the Mac, so agents can
 * run build -> launch -> read log -> fix without anyone pressing Play.
 *
 * An elfldr payload that stays resident and listens on TCP LAUNCHER_PORT.
 * One command per line, one reply line per command ("OK ..." or "ERR ..."):
 *
 *   status                 the running big app, if any: "OK <appid> <TITLEID>"
 *   launch <TITLEID> [arg] start a registered title (fails if one is running)
 *   kill                   SIGKILL the running big app, wait until it is gone
 *   stop                   ask the browser title to wind down by itself (control
 *                          socket, no system calls); launch.sh then polls status
 *                          and only kills it if it is still there
 *   register <TITLEID> [dir] register a title uploaded to /data/homebrew/<ID>
 *   uninstall <TITLEID>    remove a spike title from the app list (spikes only)
 *   quit                   stop the launcher
 *
 * tools/launch.sh is the client. It sends this payload first if nothing
 * answers on the port.
 *
 * Each command runs on a worker thread. If it hasn't answered after 15 s
 * the client gets "ERR <cmd> blocked for 15 s in <stage>" (the system call
 * it is stuck in) and the launcher keeps serving other connections
 * (2026-10-28: the launcher hung inside a kill request and only accepted
 * connections from then on).
 *
 * On 13.60 (checked 2026-10-27) sceLncUtilLaunchApp works with the payload's
 * own authid; the system authids below are only a fallback. Stopping is by
 * signal (see kill_running).
 */
#include <errno.h>
#include <pthread.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
int sceLncUtilGetAppIdOfRunningBigApp(void);
int sceLncUtilGetAppTitleId(uint32_t app_id, char *title_id);
int sceKernelLoadStartModule(const char *path, size_t argc, const void *argv, uint32_t flags,
                             void *opt, int *res);

static void say(int fd, const char *fmt, ...);

/* libSceAppInstUtil isn't loaded in payloads, so elfldr can't bind it at
 * load time ("Unable to resolve"): load it on first use. */
static int (*app_inst_init)(void);
static int (*app_inst_title_dir)(const char *title_id, const char *install_path, void *reserved);
static int (*app_inst_all)(void *reserved);
static int (*app_inst_uninstall)(const char *title_id);

static int load_app_inst_util(void)
{
    if (app_inst_title_dir || app_inst_all)
        return 0;
    /* 13.60's copy has no AppInstallTitleDir (in any process). It has
     * AppInstallAll, which makes ShellCore rescan /user/app and register every
     * title directory it finds (its log: "AppInstallTitleDirMain <ID>"), as
     * happens when PS5Upload starts a title. Look in each copy. sceKernelDlsym refuses these symbols (0x80020003); the
     * kernel-side lookup reads the export table, as elfldr does. */
    static const char *paths[] = {
        "/system/common/lib/libSceAppInstUtil.sprx",
        "/system/priv/lib/libSceAppInstUtil.sprx",
    };
    for (size_t i = 0; i < sizeof paths / sizeof *paths && !app_inst_title_dir && !app_inst_all; i++) {
        int res = 0;
        int h = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, &res);
        if (h < 0) {
            say(-1, "  load %s -> 0x%08x", paths[i], (unsigned)h);
            continue;
        }
        void *init = (void *)kernel_dynlib_dlsym(getpid(), (uint32_t)h, "sceAppInstUtilInitialize");
        void *dir = (void *)kernel_dynlib_dlsym(getpid(), (uint32_t)h, "sceAppInstUtilAppInstallTitleDir");
        say(-1, "  %s (0x%x): Initialize %p, AppInstallTitleDir %p", paths[i], h, init, dir);
        if (init && !app_inst_init)
            app_inst_init = (int (*)(void))init;
        void *all = (void *)kernel_dynlib_dlsym(getpid(), (uint32_t)h, "sceAppInstUtilAppInstallAll");
        say(-1, "  AppInstallAll %p", all);
        if (dir)
            app_inst_title_dir = (int (*)(const char *, const char *, void *))dir;
        if (all)
            app_inst_all = (int (*)(void *))all;
        void *un = (void *)kernel_dynlib_dlsym(getpid(), (uint32_t)h, "sceAppInstUtilAppUnInstall");
        say(-1, "  AppUnInstall %p", un);
        if (un && !app_inst_uninstall)
            app_inst_uninstall = (int (*)(const char *))un;
    }
    if (!app_inst_init || (!app_inst_title_dir && !app_inst_all))
        return -1;
    int r = app_inst_init();
    say(-1, "  sceAppInstUtilInitialize -> 0x%08x", (unsigned)r);
    return r;
}

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
    size_t n = strlen(line);
    if (fd >= 0) { /* the client first: the log is on /data */
        line[n] = '\n';
        (void)!write(fd, line, n + 1);
        line[n] = 0;
    }
    if (log_file) {
        fprintf(log_file, "%ld %s\n", (long)time(NULL), line);
        fflush(log_file);
    }
}

/* What the current command is doing, for "blocked in" replies. */
static const char *volatile stage = "idle";

static int running_app(char title_id[16])
{
    stage = "sceLncUtilGetAppIdOfRunningBigApp";
    int app_id = sceLncUtilGetAppIdOfRunningBigApp();
    title_id[0] = 0;
    stage = "sceLncUtilGetAppTitleId";
    if (app_id > 0 && app_id != -1 && sceLncUtilGetAppTitleId((uint32_t)app_id, title_id) != 0)
        strcpy(title_id, "?");
    stage = "after running_app";
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
    stage = "sceKernelGetAppInfo scan";
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

/* The app takes a moment to go; launching before then fails. */
static int wait_gone(int app_id, int ms)
{
    for (int t = 0; t < ms; t += 100) {
        char now[16];
        if (running_app(now) != app_id)
            return t;
        usleep(100 * 1000);
    }
    return -1;
}

/* On 13.60 every kill API refuses a payload: sceLncUtilKillApp and
 * ForceKillApp return 0x80940004 with any authid, sceSystemServiceKillApp
 * blocks 10 s and fails. SIGKILL works: the system logs an app crash
 * (0xa0d0c00d) but shows nothing (watched 2026-10-27). SIGTERM and any exit
 * a GPU title makes itself show a debug error, so SIGKILL is the only way. */
static int kill_running(int fd)
{
    char tid[16], ptid[16];
    int app_id = running_app(tid);
    if (app_id <= 0) {
        say(fd, "OK nothing running");
        return 0;
    }
    pid_t pid = app_pid(app_id, ptid);
    if (pid < 0) {
        say(fd, "ERR kill %s (appid 0x%x): no process found", tid, app_id);
        return -1;
    }
    stage = "kill(SIGKILL)";
    if (kill(pid, SIGKILL) != 0) {
        say(fd, "ERR kill %s: SIGKILL pid %d: %s", tid, pid, strerror(errno));
        return -1;
    }
    stage = "waiting for the killed app to go";
    int t = wait_gone(app_id, 10000);
    if (t < 0) {
        say(fd, "ERR %s still running 10 s after SIGKILL", tid);
        return -1;
    }
    say(fd, "OK killed %s after %d ms", tid, t);
    return 0;
}

static int copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    if (!in)
        return -1;
    FILE *out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    char buf[16384];
    size_t n;
    int r = 0;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n)
            r = -1;
    fclose(in);
    if (fclose(out) != 0)
        r = -1;
    return r;
}

/* Register a title whose files are in src (default /data/homebrew/<ID>) the
 * way PS5Upload's Play does (read off the console): /user/app/<ID>/ holds
 * sce_sys/param.json and a mount.lnk naming src, which etaHEN mounts at
 * launch; then the system's installer adds it to the app list. */
static void register_title(int fd, const char *tid, const char *src_arg)
{
    char src[256], path[320], to[320];
    if (src_arg)
        snprintf(src, sizeof src, "%s", src_arg);
    else
        snprintf(src, sizeof src, "/data/homebrew/%s", tid);

    struct stat st;
    snprintf(path, sizeof path, "%s/eboot.bin", src);
    if (stat(path, &st) != 0) {
        say(fd, "ERR register %s: no %s", tid, path);
        return;
    }
    snprintf(path, sizeof path, "/user/app/%s", tid);
    mkdir(path, 0777);
    snprintf(path, sizeof path, "/user/app/%s/sce_sys", tid);
    mkdir(path, 0777);
    static const char *files[] = { "param.json", "icon0.png", "pic0.png", "pic1.png" };
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        snprintf(path, sizeof path, "%s/sce_sys/%s", src, files[i]);
        snprintf(to, sizeof to, "/user/app/%s/sce_sys/%s", tid, files[i]);
        if (copy_file(path, to) != 0 && i == 0) {
            say(fd, "ERR register %s: copy %s: %s", tid, path, strerror(errno));
            return;
        }
    }
    snprintf(path, sizeof path, "/user/app/%s/mount.lnk", tid);
    FILE *lnk = fopen(path, "w");
    if (!lnk || fputs(src, lnk) < 0 || fclose(lnk) != 0) {
        say(fd, "ERR register %s: write %s: %s", tid, path, strerror(errno));
        return;
    }
    int r = load_app_inst_util();
    if (r != 0) {
        say(fd, "ERR register %s: loading libSceAppInstUtil: 0x%08x", tid, (unsigned)r);
        return;
    }
    const char *how = "AppInstallTitleDir";
    if (app_inst_title_dir) {
        r = app_inst_title_dir(tid, "/user/app/", NULL);
    } else {
        how = "AppInstallAll";
        r = app_inst_all(NULL);
    }
    if (r != 0)
        say(fd, "ERR register %s: sceAppInstUtil%s 0x%08x (files are in place)", tid, how, (unsigned)r);
    else
        say(fd, "OK registered %s from %s (%s)", tid, src, how);
}

/* Only our spike titles can be uninstalled: never the owner's titles, and
 * not the browser (PPSA99020) either. */
static const char *const uninstallable[] = {
    "PPSA99006", "PPSA99007", "PPSA99009", "PPSA99010", "PPSA99011",
    "PPSA99012", "PPSA99013", "PPSA99014", "PPSA99015",
};

static void uninstall_title(int fd, const char *tid)
{
    int ok = 0;
    for (size_t i = 0; i < sizeof uninstallable / sizeof *uninstallable; i++)
        ok |= !strcmp(tid, uninstallable[i]);
    if (!ok) {
        say(fd, "ERR uninstall %s: not a spike title", tid);
        return;
    }
    char running[16];
    if (running_app(running) > 0 && !strcmp(running, tid)) {
        say(fd, "ERR uninstall %s: it is running", tid);
        return;
    }
    int r = load_app_inst_util();
    if (r != 0 || !app_inst_uninstall) {
        say(fd, "ERR uninstall %s: no sceAppInstUtilAppUnInstall (0x%08x)", tid, (unsigned)r);
        return;
    }
    r = app_inst_uninstall(tid);
    if (r != 0)
        say(fd, "ERR uninstall %s: sceAppInstUtilAppUnInstall 0x%08x", tid, (unsigned)r);
    else
        say(fd, "OK uninstalled %s", tid);
}

/* Ask the browser title (port/webkit/apps/title/ps5browser.c) to wind down
 * by itself: stop media, close its audio ports and sockets, then ask us for
 * the SIGKILL. Only a unix socket in /system_tmp, no system service calls,
 * so it works even when those are slow. */
#define BROWSER_CONTROL_SOCKET "/system_tmp/ps5wk/ps5browser-control.sock"
static void stop_title(int fd)
{
    stage = "connect to the title's control socket";
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, BROWSER_CONTROL_SOCKET, sizeof a.sun_path - 1);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        say(fd, "ERR stop: %s: %s", BROWSER_CONTROL_SOCKET, strerror(errno));
        if (s >= 0)
            close(s);
        return;
    }
    struct timeval tv = { 3, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char reply[128] = "";
    stage = "waiting for the title's answer";
    ssize_t n = -1;
    if (write(s, "quit\n", 5) == 5)
        n = read(s, reply, sizeof reply - 1);
    close(s);
    reply[n > 0 ? n : 0] = 0;
    reply[strcspn(reply, "\r\n")] = 0;
    if (n > 0 && !strncmp(reply, "OK", 2))
        say(fd, "OK stop: %s", reply);
    else
        say(fd, "ERR stop: no answer from the title (%s)", n < 0 ? strerror(errno) : reply);
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
        stage = "sceLncUtilLaunchApp";
        int r = launch(a1, a2, &used);
        if (r < 0)
            say(fd, "ERR launch %s: 0x%08x", a1, (unsigned)r);
        else
            say(fd, "OK launched %s appid 0x%x (authid 0x%016llx%s)", a1, r,
                (unsigned long long)used, used == own_authid ? ", own" : "");
    } else if (!strcmp(cmd, "stop")) {
        stop_title(fd);
    } else if (!strcmp(cmd, "kill")) {
        kill_running(fd); /* "kill -9" is accepted: SIGKILL is the only way */
    } else if (!strcmp(cmd, "register")) {
        if (!a1 || strlen(a1) != 9)
            say(fd, "ERR usage: register <TITLEID> [srcdir]");
        else
            register_title(fd, a1, a2);
    } else if (!strcmp(cmd, "uninstall")) {
        if (!a1 || strlen(a1) != 9)
            say(fd, "ERR usage: uninstall <TITLEID>");
        else
            uninstall_title(fd, a1);
    } else if (!strcmp(cmd, "quit")) {
        say(fd, "OK bye");
        return 1;
    } else {
        say(fd, "ERR unknown command %s (status, launch, stop, kill, register, uninstall, quit)", cmd);
    }
    return 0;
}

typedef struct {
    int fd;
    char line[256];
    int done, stop, abandoned;
} job_t;

static void *run_job(void *data)
{
    job_t *job = data;
    int stop = command(job->fd, job->line);
    stage = "idle";
    if (__atomic_load_n(&job->abandoned, __ATOMIC_ACQUIRE)) {
        say(-1, "a command that blocked for more than 15 s returned");
        close(job->fd);
        free(job);
        return NULL;
    }
    job->stop = stop;
    __atomic_store_n(&job->done, 1, __ATOMIC_RELEASE);
    return NULL;
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
        int abandoned = 0;
        while (!stop && !abandoned && in && fgets(line, sizeof line, in)) {
            say(-1, "> %.*s", (int)strcspn(line, "\r\n"), line);
            job_t *job = calloc(1, sizeof *job);
            pthread_t t;
            if (job) {
                job->fd = c;
                memcpy(job->line, line, sizeof job->line);
            }
            if (!job || pthread_create(&t, NULL, run_job, job) != 0) {
                stop = command(c, line); /* no thread: run it here */
                free(job);
                continue;
            }
            pthread_detach(t);
            for (int ms = 0; ms < 15000 && !__atomic_load_n(&job->done, __ATOMIC_ACQUIRE); ms += 20)
                usleep(20 * 1000);
            if (__atomic_load_n(&job->done, __ATOMIC_ACQUIRE)) {
                stop = job->stop;
                free(job);
            } else {
                /* Leave the connection to the worker (it closes it if it
                 * ever returns) and serve the next client. */
                char msg[200];
                int n = snprintf(msg, sizeof msg, "ERR %.20s blocked for 15 s in %s\n", line, stage);
                for (int i = 0; i < n; i++)
                    if (msg[i] == '\n' && i < n - 1)
                        msg[i] = ' ';
                (void)!write(c, msg, (size_t)n);
                __atomic_store_n(&job->abandoned, 1, __ATOMIC_RELEASE);
                abandoned = 1;
            }
        }
        if (abandoned)
            continue; /* the worker owns c now (and `in` leaks with it) */
        if (in)
            fclose(in);
        else
            close(c);
    }
    close(s);
    return 0;
}
