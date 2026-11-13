/* Spike 12 P0d - the GL probe as a registered title.
 *
 * Runs the probe once at start-up and exits (spikes/common/title_exit.c asks
 * the launcher payload to close the title). Every result is one line,
 *   RESULT 12-p0d-glprobe <test> PASS|FAIL|SKIP <detail>
 * written to /data/ps5webkit-spike/glprobe.log (fetch over FTP, as spike 11's
 * webgl.log) and, when tools/logserver.sh is listening on the Mac, to it as
 * well (the connection is tried for 2 s and then given up, so nothing waits
 * on the Mac).
 *
 * Launch argument (tools/launch.sh <TITLE_ID> <arg>), to get past a group
 * that takes the title down:
 *   skip=<group>[,<group>...]   leave these groups out
 *   only=<group>[,<group>...]   run only these
 * Group names are the "== <group>" lines of the log.
 */
#define SPIKE_NAME "12-p0d-glprobe"
#include "../../../common/title.h"

#include "crashguard.h"
#include "probe.h"

#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>

#ifndef LOG_HOST
#define LOG_HOST ""
#endif
#ifndef LOG_PORT
#define LOG_PORT 9999
#endif

/* The crash guard's weak hooks; the packager wants every symbol defined. */
void ps5wk_jit_memory(void **start, void **end, size_t *allocated, size_t *committed)
{
    (void)allocated;
    (void)committed;
    *start = *end = NULL;
}
void ps5wk_profiler_thread(int starting) { (void)starting; }

static int g_file = -1, g_tcp = -1;
static int g_pass, g_fail, g_skip;
static char g_only[256], g_skip_list[256];

static void put(const char *line, size_t n)
{
    if (g_file >= 0 && write(g_file, line, n) < 0) {
        close(g_file);
        g_file = -1;
    }
    if (g_tcp >= 0 && send(g_tcp, line, n, 0) < 0) {
        close(g_tcp);
        g_tcp = -1;
    }
}

void probe_log(const char *fmt, ...)
{
    char line[1100];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof line - 2)
        n = (int)sizeof line - 2;
    line[n++] = '\n';
    put(line, (size_t)n);
}

void probe_emit(const char *test, const char *status, const char *detail)
{
    if (!strcmp(status, "PASS"))
        g_pass++;
    else if (!strcmp(status, "FAIL"))
        g_fail++;
    else
        g_skip++;
    probe_log("RESULT %s %s %s %s", SPIKE_NAME, test, status, detail);
}

double probe_now_ms(void) { return spike_now_ms(); }

static int listed(const char *list, const char *group)
{
    char padded[300], want[96];
    snprintf(padded, sizeof padded, ",%s,", list);
    snprintf(want, sizeof want, ",%s,", group);
    return strstr(padded, want) != NULL;
}

int probe_selected(const char *group)
{
    if (g_only[0])
        return listed(g_only, group);
    return !listed(g_skip_list, group);
}

void probe_run_group(const char *group, int needs_gpu, const char *results, void (*fn)(void))
{
    (void)needs_gpu;
    (void)results;
    char line[160];
    /* Before the group starts: if the title dies in it, this is the last
     * line of the log and of the kernel log. */
    probe_log("== %s", group);
    snprintf(line, sizeof line, "[ps5webkit] glprobe group %s\n", group);
    spike_klog(line);
    const double t0 = spike_now_ms();
    fn();
    probe_log("   (%s: %.0f ms)", group, spike_now_ms() - t0);
}

/* tools/logserver.sh on the Mac, if it is there: 2 s at most. */
static int log_connect(const char *host, int port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (!host[0] || inet_pton(AF_INET, host, &a.sin_addr) != 1)
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int r = connect(fd, (struct sockaddr *)&a, sizeof a);
    if (r != 0 && errno == EINPROGRESS) {
        struct pollfd p = {fd, POLLOUT, 0};
        int error = 0;
        socklen_t len = sizeof error;
        if (poll(&p, 1, 2000) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0)
            r = 0;
    }
    if (r != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, flags);
    return fd;
}

int main(int argc, char **argv)
{
    char line[200];
    snprintf(line, sizeof line, "[ps5webkit] glprobe starting, pid %d\n", (int)getpid());
    spike_klog(line);
    spike_title_check_imports();
    const int unjailed = spike_title_unjail();
    if (unjailed) {
        mkdir("/data/ps5webkit-spike", 0777);
        g_file = open("/data/ps5webkit-spike/glprobe.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    /* The crash report (stderr) and the driver's own messages (stdout) go to
     * the same file. */
    if (g_file >= 0) {
        setvbuf(stdout, NULL, _IONBF, 0);
        dup2(g_file, 1);
        dup2(g_file, 2);
    }
    ps5wk_crash_guard_install("glprobe");
    g_tcp = log_connect(LOG_HOST, LOG_PORT);
    snprintf(line, sizeof line, "[ps5webkit] glprobe unjailed %d file %d tcp %d\n", unjailed, g_file, g_tcp);
    spike_klog(line);
    spike_notify("ps5webkit: GL probe (spike 12 P0d) running");
    probe_log("ps5webkit spike 12 P0d: GL probe for Dawn's GLES backend (pid %d, unjailed %d, file log %s, tcp log to %s:%d %s)",
              (int)getpid(), unjailed, g_file >= 0 ? "on" : "off", LOG_HOST, LOG_PORT, g_tcp >= 0 ? "connected" : "not connected");

    for (int i = 0; argv && i < argc; i++) {
        if (!argv[i])
            continue;
        probe_log("launch argument %d: %.200s", i, argv[i]);
        if (!strncmp(argv[i], "skip=", 5))
            snprintf(g_skip_list, sizeof g_skip_list, "%s", argv[i] + 5);
        if (!strncmp(argv[i], "only=", 5))
            snprintf(g_only, sizeof g_only, "%s", argv[i] + 5);
    }

    const double t0 = spike_now_ms();
    glprobe_run();
    probe_log("done: %d PASS, %d FAIL, %d SKIP in %.0f ms", g_pass, g_fail, g_skip, spike_now_ms() - t0);
    spike_notify("ps5webkit: GL probe done");
    if (g_tcp >= 0) {
        shutdown(g_tcp, SHUT_WR);
        usleep(200 * 1000);
        close(g_tcp);
        g_tcp = -1;
    }
    return 0;
}
