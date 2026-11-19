/* Spike 15 test titles: ANGLE's Vulkan backend on RADV, no ps5-opengl.
 *
 * 15a (default, PPSA99021): angle_vk_tests.cpp. 15c (-DSPIKE15_C,
 * PPSA99022): vr_tsharp.cpp, an Hmd2 eye buffer from a RADV VkImage. 15d
 * (-DSPIKE15_D, PPSA99023): video_15d.cpp, Videodec2 into ANGLE buffers. 15e
 * (-DSPIKE15_E, PPSA99024): vk_share_15e.cpp, images shared between VkDevices.
 * Logs go to /data/ps5webkit-spike/<15a|15c>.log (fetch over FTP) and the
 * kernel log; no TCP log sink, so nothing waits on the Mac.
 */
#if defined(SPIKE15_E)
#define SPIKE_NAME "15e-vk-share"
#define SPIKE15_TAG "15e"
#define SPIKE15_WHAT "images shared between ANGLE's VkDevice and a second one"
void vk_share_tests_run(void);
#elif defined(SPIKE15_D)
#define SPIKE_NAME "15d-video"
#define SPIKE15_TAG "15d"
#define SPIKE15_WHAT "hardware-decoded video into RADV memory, converted on ANGLE"
void video_tests_run(int load_rc);
int vdec2_load(void);
#elif defined(SPIKE15_C)
#define SPIKE_NAME "15c-vr-tsharp"
#define SPIKE15_TAG "15c"
#define SPIKE15_WHAT "an Hmd2 eye buffer from a RADV VkImage"
void vr_tsharp_tests_run(int preload_rc);
#else
#define SPIKE_NAME "15a-angle-vk"
#define SPIKE15_TAG "15a"
#define SPIKE15_WHAT "ANGLE on Vulkan on RADV, no ps5-opengl"
void angle_vk_tests_run(void);
#endif
#include "../../common/title.h"
#include "../../common/psvr2/psvr2.h"

#include "crashguard.h"

#include <stdarg.h>
#include <stdlib.h>

/* The crash guard's weak hooks; the packager wants every symbol defined. */
void ps5wk_jit_memory(void **start, void **end, size_t *allocated, size_t *committed)
{
    (void)allocated;
    (void)committed;
    *start = *end = NULL;
}
void ps5wk_profiler_thread(int starting) { (void)starting; }
/* title_exit.c's weak hook for a restart helper: this title has none. */
int spike_title_exit_helper(void) { return -1; }

/* C-linkage helpers for the C++ side (spike.h is header-only C). */
void spike15_log(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    spike_log("%s", buf);
}

void spike15_result(const char *test, int pass, const char *fmt, ...)
{
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    spike_log("RESULT %s %s %s %s", SPIKE_NAME, test, pass ? "PASS" : "FAIL", buf);
}

double spike15_now_ms(void) { return spike_now_ms(); }

double spike15_cpu_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return -1;
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

int main(void)
{
    char line[160];
    snprintf(line, sizeof line, "[ps5webkit] spike" SPIKE15_TAG " starting, pid %d\n", (int)getpid());
    spike_klog(line);
    spike_title_check_imports();
#if defined(SPIKE15_D)
    /* Before the unjail: the module won't load after it (Spike 9, PS5VR). */
    const int preload_rc = vdec2_load();
#elif defined(SPIKE15_C)
    /* Early, while the sandbox's library path is how Hmd2 is reached. */
    const int preload_rc = psvr2_preload();
#endif
    const int unjailed = spike_title_unjail();
    if (unjailed) {
        mkdir("/data/ps5webkit-spike", 0777);
        spike_log_file_fd = open("/data/ps5webkit-spike/" SPIKE15_TAG ".log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    /* The browser's crash report (signal, pc as an ELF offset, stack scan)
     * goes to stderr: send it, and anything RADV prints, to the log. */
    if (spike_log_file_fd >= 0) {
        setvbuf(stdout, NULL, _IONBF, 0);
        dup2(spike_log_file_fd, 1);
        dup2(spike_log_file_fd, 2);
    }
    /* No crash guard: its stack scan reads code bytes (afterCall), and a
     * title's text is execute-only, so the guard itself faults
     * (SYSTEM_XO_VIOLATION) and the kernel then reports the guard's
     * registers instead of the first fault's. Without it the kernel's own
     * report (klogsrv) has the faulting thread's registers and backtrace. */
    (void)ps5wk_crash_guard_install;
    /* RADV's on-disk shader cache defaults to /app0/radv-shader-cache; the
     * spike measures cold pipelines, as spike 12 did. */
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
    snprintf(line, sizeof line, "[ps5webkit] spike" SPIKE15_TAG " unjailed %d log fd %d\n", unjailed,
             spike_log_file_fd);
    spike_klog(line);
    spike_notify("ps5webkit: spike " SPIKE15_TAG " running");
    spike_log("ps5webkit spike " SPIKE15_TAG ": " SPIKE15_WHAT " (pid %d, unjailed %d)", (int)getpid(), unjailed);
#if defined(SPIKE15_E)
    vk_share_tests_run();
#elif defined(SPIKE15_D)
    video_tests_run(preload_rc);
#elif defined(SPIKE15_C)
    vr_tsharp_tests_run(preload_rc);
#else
    angle_vk_tests_run();
#endif
    spike_log("done");
    spike_notify("ps5webkit: spike " SPIKE15_TAG " done");
    return 0;
}
