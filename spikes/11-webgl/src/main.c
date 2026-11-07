/* Spike 11 - WebGL on the PS5: ANGLE over ps5-opengl in a title.
 *
 * Runs as a registered title (PPSA99006 by default). Logs go to
 * /data/ps5webkit-spike/webgl.log (fetch over FTP) and the kernel log;
 * no TCP log sink, so nothing waits on the Mac.
 *
 *   angle-*       ANGLE's GLES front end over ps5-opengl's EGL (angle_tests.cpp)
 *   native-*      the same workload straight through ps5-opengl (native_bench.c)
 *   share-*       WebGL sharing with a GL 4.6 core context (angle_tests.cpp)
 */
#define SPIKE_NAME "11-webgl"
#include "../../common/title.h"

#include "crashguard.h"

#include <stdarg.h>
#include <stdlib.h>

void angle_tests_run(void);
void native_bench_run(void);
void share_probe_run(void);

/* The crash guard's weak hooks; the packager wants every symbol defined. */
void ps5wk_jit_memory(void **start, void **end, size_t *allocated, size_t *committed)
{
    (void)allocated;
    (void)committed;
    *start = *end = NULL;
}
void ps5wk_profiler_thread(int starting) { (void)starting; }

/* C-linkage helpers for the C++ side (spike.h is header-only C). */
void spike11_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    spike_log("%s", buf);
}

void spike11_result(const char *test, int pass, const char *fmt, ...)
{
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    spike_log("RESULT %s %s %s %s", SPIKE_NAME, test, pass ? "PASS" : "FAIL", buf);
}

double spike11_now_ms(void) { return spike_now_ms(); }

double spike11_cpu_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return -1;
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

int main(void)
{
    char line[160];
    snprintf(line, sizeof line, "[ps5webkit] spike11 starting, pid %d\n", (int)getpid());
    spike_klog(line);
    spike_title_check_imports();
    const int unjailed = spike_title_unjail();
    if (unjailed) {
        mkdir("/data/ps5webkit-spike", 0777);
        spike_log_file_fd = open("/data/ps5webkit-spike/webgl.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    /* The browser's crash report (signal, pc as an ELF offset, stack scan)
     * goes to stderr: send it, and ps5-opengl's own stdout, to the log. */
    if (spike_log_file_fd >= 0) {
        setvbuf(stdout, NULL, _IONBF, 0);
        dup2(spike_log_file_fd, 1);
        dup2(spike_log_file_fd, 2);
    }
    ps5wk_crash_guard_install("spike11");
    snprintf(line, sizeof line, "[ps5webkit] spike11 unjailed %d log fd %d\n", unjailed, spike_log_file_fd);
    spike_klog(line);
    spike_notify("ps5webkit: spike 11 (ANGLE) running");
    spike_log("ps5webkit spike 11: ANGLE over ps5-opengl (pid %d, unjailed %d)", (int)getpid(), unjailed);

    const char *only = getenv("SPIKE11_ONLY"); /* unset in a title; kept for a host build */
    if (!only || !strcmp(only, "angle"))
        angle_tests_run();
    if (!only || !strcmp(only, "native"))
        native_bench_run();
    if (!only || !strcmp(only, "share"))
        share_probe_run();

    spike_log("done");
    spike_notify("ps5webkit: spike 11 done");
    return 0;
}
