/* Spike 12 P1 - Dawn on RADV in a title, shown through the compositor's path.
 * Title PPSA99012. Log: /data/ps5webkit/12-webgpu-p1.log and tools/logserver.sh. */
#define SPIKE_NAME "12-webgpu-p1"
#include "common/title.h"
#include "crashguard.h"
#include "shared.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>

#ifndef LOG_HOST
#define LOG_HOST "192.168.1.100"
#endif
#ifndef LOG_PORT
#define LOG_PORT 9999
#endif

int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t align, int64_t *out_start, size_t *out_size);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableFlexibleMemorySize(size_t *out);
void dawn_run(void);

void ps5wk_jit_memory(void **start, void **end, size_t *allocated, size_t *committed)
{
    (void)allocated;
    (void)committed;
    *start = *end = NULL;
}
void ps5wk_profiler_thread(int starting) { (void)starting; }

/* Dawn's DynamicLib is linked but never used on the PS5 (the drivers come
 * through port/deps/patches/dawn's hooks); libc.a's dl* need __dlclose and
 * friends, which a title cannot import. The browser wraps these too (dlshim.c). */
void *__wrap_dlopen(const char *path, int mode) { (void)path; (void)mode; return NULL; }
void *__wrap_dlsym(void *handle, const char *name) { (void)handle; (void)name; return NULL; }
int __wrap_dlclose(void *handle) { (void)handle; return 0; }

void p1_log(const char *fmt, ...)
{
    char buf[1000];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    spike_log("%s", buf);
}

void p1_result(const char *test, int pass, const char *fmt, ...)
{
    char buf[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    spike_log("RESULT %s %s %s %s", SPIKE_NAME, test, pass ? "PASS" : "FAIL", buf);
}

double p1_now_ms(void) { return spike_now_ms(); }

void p1_mem_line(const char *when)
{
    int64_t start = 0;
    size_t avail = 0, flex = 0;
    const size_t pool = sceKernelGetDirectMemorySize();
    sceKernelAvailableDirectMemorySize(0, (int64_t)pool, 0x4000, &start, &avail);
    sceKernelAvailableFlexibleMemorySize(&flex);
    spike_log("[mem] %-22s largest free direct block %.1f MB, flexible free %.1f MB", when, avail / 1048576.0, flex / 1048576.0);
}

/* Dawn on its own thread, as RemoteGPU's work queue will be, on a stack we
 * can measure: filled with a pattern, scanned afterwards. */
#define DAWN_STACK (8u << 20)
static void *dawn_thread(void *arg)
{
    (void)arg;
    dawn_run();
    return NULL;
}

int main(void)
{
    spike_title_init(SPIKE_NAME, LOG_HOST, LOG_PORT);
    if (spike_log_file_fd >= 0) {
        setvbuf(stdout, NULL, _IONBF, 0);
        dup2(spike_log_file_fd, 2);
    }
    ps5wk_crash_guard_install("spike12p1");
    spike_notify("ps5webkit: spike 12 P1 (Dawn on RADV) running");
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);

    pthread_t gl, dawn;
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
    usleep(500 * 1000);
    SPIKE_PASS("gl-present", "%s", g_gl.info);

    uint8_t *stack = NULL;
    if (posix_memalign((void **)&stack, 16384, DAWN_STACK) != 0)
        return 1;
    memset(stack, 0xA5, DAWN_STACK);
    pthread_attr_setstack(&attr, stack, DAWN_STACK);
    pthread_create(&dawn, &attr, dawn_thread, NULL);
    pthread_join(dawn, NULL);
    size_t untouched = 0;
    while (untouched < DAWN_STACK && stack[untouched] == 0xA5)
        untouched++;
    SPIKE_PASS("dawn-stack", "high-water %.0f KiB of an %u MiB stack (WGSL compile, Tint, RADV's compiler, all on Dawn's thread)",
               (DAWN_STACK - untouched) / 1024.0, DAWN_STACK >> 20);

    const unsigned long a = g_gl.frames;
    usleep(3000 * 1000);
    spike_log("GL presented %lu frames in the last 3 s; %lu GL errors, %lu failed swaps in all", g_gl.frames - a,
              (unsigned long)g_gl.gl_errors, (unsigned long)g_gl.swap_failures);
    g_gl.stop = 1;
    pthread_join(gl, NULL);
    spike_log("done");
    return 0;
}
