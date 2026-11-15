/* jscside.h - side threads beside the jsc shell (Spike 14 stage 0), the
 * same in the title (jsctitle.c) and the payload jsc (payloadenv.c).
 *
 * A file holds "<cpu>:<duty %>" words, e.g. "15:50 13:50"; cpu "-" leaves a
 * thread unpinned. Each thread repeats a 10 ms period: an integer loop for
 * duty% of it (4 independent xorshift chains: neither latency-bound nor as
 * wide as smtprobe's 8), then sleep for the rest. This is a stand-in for the
 * WebProcess's other threads in a fight: the page's main thread (19% of a
 * core), the second worker (14%), audio and the wasm compile helpers.
 *
 * jscSideStop() reports, per thread, the loop rate while spinning (Mloops
 * per spinning second: lower when the core is shared) and the duty it got. */
#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/param.h>
#include <sys/cpuset.h>
#include <errno.h>

#define JSC_SIDE_MAX 16

typedef struct {
    pthread_t tid;
    int cpu, duty, pinErr;
    double spinMs, wallMs;
    unsigned long long loops;
} jsc_side_t;

static jsc_side_t g_side[JSC_SIDE_MAX];
static int g_sideCount;
static _Atomic int g_sideRun;

static inline double jscSideNow(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* Pin the calling thread to one CPU; 0 or errno. */
static inline int jscPinSelf(int cpu)
{
    uint64_t m[2] = {1ull << cpu, 0};
    return cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, (cpuset_t*)m) ? errno : 0;
}

static inline uint64_t jscSelfMask(void)
{
    uint64_t m[2] = {0, 0};
    cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, (cpuset_t*)m);
    return m[0];
}

static void* jscSideThread(void* arg)
{
    jsc_side_t* s = arg;
    if (s->cpu >= 0)
        s->pinErr = jscPinSelf(s->cpu);
    uint64_t a = 1, b = 2, c = 3, d = 4;
    const double period = 10.0, busy = period * s->duty / 100.0;
    double start = jscSideNow();
    while (atomic_load_explicit(&g_sideRun, memory_order_relaxed)) {
        double t0 = jscSideNow(), t;
        do {
            for (int i = 0; i < 4096; i++) {
                a ^= a << 13; a ^= a >> 7; a ^= a << 17;
                b ^= b << 13; b ^= b >> 7; b ^= b << 17;
                c ^= c << 13; c ^= c >> 7; c ^= c << 17;
                d ^= d << 13; d ^= d >> 7; d ^= d << 17;
            }
            __asm__ volatile("" : "+r"(a), "+r"(b), "+r"(c), "+r"(d));
            s->loops += 4096;
            t = jscSideNow();
        } while (t - t0 < busy);
        s->spinMs += t - t0;
        if (s->duty < 100) {
            double left = period - (t - t0);
            if (left > 0.05) {
                struct timespec ts = {0, (long)(left * 1e6)};
                nanosleep(&ts, NULL);
            }
        }
    }
    s->wallMs = jscSideNow() - start;
    return NULL;
}

/* Starts the threads the file names; returns how many. describe gets a
 * one-line summary. */
static inline int jscSideStart(const char* path, char* describe, size_t size)
{
    describe[0] = 0;
    FILE* f = fopen(path, "r");
    if (!f)
        return 0;
    char text[512] = {0};
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    atomic_store(&g_sideRun, 1);
    size_t o = 0;
    char* save = NULL;
    for (char* w = strtok_r(text, " \t\r\n", &save); w && g_sideCount < JSC_SIDE_MAX; w = strtok_r(NULL, " \t\r\n", &save)) {
        char* colon = strchr(w, ':');
        if (!colon)
            continue;
        jsc_side_t* s = &g_side[g_sideCount];
        memset(s, 0, sizeof *s);
        s->cpu = w[0] == '-' ? -1 : atoi(w);
        s->duty = atoi(colon + 1);
        if (s->duty <= 0)
            continue;
        if (s->duty > 100)
            s->duty = 100;
        if (pthread_create(&s->tid, NULL, jscSideThread, s))
            continue;
        g_sideCount++;
        o += snprintf(describe + o, size > o ? size - o : 0, "%s%s:%d%%", o ? " " : "", w[0] == '-' ? "any" : w, s->duty);
    }
    return g_sideCount;
}

/* Stops the threads and writes one line per thread through report. */
static inline void jscSideStop(void (*report)(const char* line))
{
    if (!g_sideCount)
        return;
    atomic_store(&g_sideRun, 0);
    for (int i = 0; i < g_sideCount; i++) {
        jsc_side_t* s = &g_side[i];
        pthread_join(s->tid, NULL);
        char line[200], cpu[24];
        if (s->cpu < 0)
            snprintf(cpu, sizeof cpu, "any");
        else
            snprintf(cpu, sizeof cpu, "%d%s", s->cpu, s->pinErr ? " (pin failed)" : "");
        snprintf(line, sizeof line, "side %d: cpu %s, duty asked %d%%, got %.0f%%, %.1f Mloops per spinning second", i, cpu,
            s->duty, s->wallMs > 0 ? 100.0 * s->spinMs / s->wallMs : 0, s->spinMs > 0 ? s->loops / (s->spinMs * 1e3) : 0);
        report(line);
    }
    g_sideCount = 0;
}
