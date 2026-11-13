/* Spike 12 P0b/P0c - RADV beside ps5-opengl in one title.
 * Title PPSA99012. RESULT lines go to tools/logserver.sh and
 * /data/ps5webkit/12-webgpu-p0b.log. */
#define SPIKE_NAME "12-webgpu-p0b"
#include "common/title.h"
#include "crashguard.h"
#include "shared.h"

#include <pthread.h>
#include <stdlib.h>

#ifndef LOG_HOST
#define LOG_HOST "192.168.1.100"
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

int main(void)
{
    spike_title_init(SPIKE_NAME, LOG_HOST, LOG_PORT);
    if (spike_log_file_fd >= 0) { /* the crash report and both drivers' stderr */
        setvbuf(stdout, NULL, _IONBF, 0);
        dup2(spike_log_file_fd, 2);
    }
    ps5wk_crash_guard_install("spike12");
    spike_notify("ps5webkit: spike 12 (RADV beside ps5-opengl) running");
    /* No shader cache on disk for the spike: timings are cold compiles. */
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);

    pthread_t gl;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2u << 20);
    pthread_create(&gl, &attr, gl_thread_main, NULL);
    for (int i = 0; i < 1000 && !g_gl.state; i++)
        usleep(10 * 1000);
    if (g_gl.state != 1) {
        SPIKE_FAIL("gl-present", "%s", g_gl.state ? g_gl.info : "GL thread did not start in 10 s");
        return 1;
    }
    usleep(1000 * 1000);
    SPIKE_PASS("gl-present", "%s; %lu frames in the first second", g_gl.info, (unsigned long)g_gl.frames);

    vk_run();

    /* Leave the picture up for a look at the TV, then go. */
    const unsigned long a = g_gl.frames;
    usleep(3000 * 1000);
    spike_log("GL presented %lu frames in the last 3 s; %lu GL errors, %lu failed swaps in all", g_gl.frames - a,
              (unsigned long)g_gl.gl_errors, (unsigned long)g_gl.swap_failures);
    g_gl.stop = 1;
    pthread_join(gl, NULL);
    spike_log("done");
    spike_notify("ps5webkit: spike 12 done");
    return 0;
}
