/*
 * heapprof.c - which code holds the title's malloc memory.
 *
 * The title's malloc is dheap.c (dlmalloc over direct memory). With pages in
 * the browser process it also serves the WebProcess's libraries (Dawn,
 * RADV, ANGLE, Skia, GStreamer, glib...); WebKit's own heap is libpas, in
 * titlemem.c's anonymous mmaps, and is not seen here.
 *
 * On while /data/ps5webkit/heap-profile exists when the title starts; its
 * first line is the sampling interval in KB (default 512). About one
 * allocation per interval of bytes allocated is sampled, with its stack
 * (profiler.c's unwinder, no allocation inside malloc), and kept until it
 * is freed. Every 60 s (and at wind-down) the live samples, folded by stack,
 * go to /data/ps5webkit/prof/heap-<pid>-<n>.txt; the newest 30 stay.
 * tools/heapprof.py symbolizes them and compares two.
 *
 * An allocation of s bytes is sampled when the running byte count crosses a
 * multiple of the interval I: probability about min(1, s / I), counted as
 * max(s, I) bytes, so the totals estimate live bytes per stack.
 *
 * File format:
 *   # ps5heap 1 pid=<pid> n=<snapshot> ms=<since start> interval=<bytes> in_use=<bytes> mapped=<bytes>
 *     live=<samples> sampled=<total samples> dropped=<table full> marker=<ps5wk_profiler_start's offset>
 *   S <estimated bytes> <samples> <pc> <pc> ...   (innermost first, the allocator's frames left out)
 */
#define SPIKE_NAME "ps5browser"
#include "common/spike.h"
#include "dmem.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern void (*volatile dheap_hook_alloc)(void* p, size_t usable);
extern void (*volatile dheap_hook_free)(void* p, size_t usable);
int ps5wk_backtrace_init(void) __attribute__((weak));
unsigned ps5wk_backtrace_here(uintptr_t* out, unsigned max) __attribute__((weak));
size_t ps5wk_backtrace_format_pc(char* buffer, size_t size, uintptr_t pc) __attribute__((weak));
void ps5wk_backtrace_refresh_jit(void) __attribute__((weak));
void ps5wk_profiler_register_self(void) __attribute__((weak));
void ps5wk_profiler_start(const char* tag) __attribute__((weak));

#define FRAMES 28 /* kept per sample, after the allocator's own */
#define SKIP 3 /* ps5wk_backtrace_here, onAlloc, dheap.c's note_alloc */
#define SLOTS (1u << 17) /* live samples at most (a power of two) */
#define FILTER (1u << 20) /* counters for the free path's quick "not sampled" */
#define KEEP 30

typedef struct {
    uintptr_t p; /* 0: empty */
    uint64_t bytes; /* what the sample stands for: max(size, interval) */
    unsigned char n;
    uintptr_t pc[FRAMES];
} rec_t;

static rec_t* table;
static _Atomic unsigned char* filter;
/* Made with pthread_mutex_init before the hooks are set: libthr's static
 * initializer allocates the mutex at its first lock, which inside malloc
 * would come back here. */
static pthread_mutex_t lock;
static unsigned live;
static uint64_t interval = 512 << 10;
static _Atomic uint64_t counted;
static _Atomic unsigned long long sampled, dropped;
static double startMs;

static unsigned slotOf(uintptr_t p)
{
    uint64_t h = (uint64_t)p * 0x9e3779b97f4a7c15ull;
    return (unsigned)(h >> 40) & (SLOTS - 1);
}

static unsigned filterOf(uintptr_t p)
{
    uint64_t h = ((uint64_t)p >> 4) * 0xff51afd7ed558ccdull;
    return (unsigned)(h >> 32) & (FILTER - 1);
}

static void onAlloc(void* p, size_t usable)
{
    uint64_t before = atomic_fetch_add_explicit(&counted, usable, memory_order_relaxed);
    if (before / interval == (before + usable) / interval)
        return;
    uintptr_t frames[FRAMES + SKIP];
    unsigned n = ps5wk_backtrace_here(frames, FRAMES + SKIP);
    unsigned skip = n > SKIP ? SKIP : 0;
    atomic_fetch_add_explicit(&sampled, 1, memory_order_relaxed);
    pthread_mutex_lock(&lock);
    if (live >= SLOTS - SLOTS / 8) {
        pthread_mutex_unlock(&lock);
        atomic_fetch_add_explicit(&dropped, 1, memory_order_relaxed);
        return;
    }
    unsigned i = slotOf((uintptr_t)p);
    while (table[i].p && table[i].p != (uintptr_t)p)
        i = (i + 1) & (SLOTS - 1);
    rec_t* r = &table[i];
    if (!r->p) {
        live++;
        atomic_fetch_add_explicit(&filter[filterOf((uintptr_t)p)], 1, memory_order_relaxed);
    }
    r->p = (uintptr_t)p;
    r->bytes = usable > interval ? usable : interval;
    r->n = (unsigned char)(n - skip);
    memcpy(r->pc, frames + skip, (n - skip) * sizeof(uintptr_t));
    pthread_mutex_unlock(&lock);
}

/* Linear probing with backward-shift deletion: no tombstones. */
static void onFree(void* p, size_t usable)
{
    (void)usable;
    unsigned f = filterOf((uintptr_t)p);
    if (!atomic_load_explicit(&filter[f], memory_order_relaxed))
        return;
    pthread_mutex_lock(&lock);
    unsigned i = slotOf((uintptr_t)p);
    while (table[i].p && table[i].p != (uintptr_t)p)
        i = (i + 1) & (SLOTS - 1);
    if (!table[i].p) {
        pthread_mutex_unlock(&lock);
        return;
    }
    atomic_fetch_sub_explicit(&filter[f], 1, memory_order_relaxed);
    live--;
    unsigned hole = i;
    for (unsigned j = (i + 1) & (SLOTS - 1); table[j].p; j = (j + 1) & (SLOTS - 1)) {
        unsigned home = slotOf(table[j].p);
        /* j's entry may move into the hole if its home is not in (hole, j]. */
        int between = hole <= j ? (home > hole && home <= j) : (home > hole || home <= j);
        if (!between) {
            table[hole] = table[j];
            hole = j;
        }
    }
    table[hole].p = 0;
    pthread_mutex_unlock(&lock);
}

/* ---- snapshots ---------------------------------------------------------- */

typedef struct {
    uint64_t hash, bytes;
    unsigned count;
    unsigned index; /* a record with this stack, in the copy */
} fold_t;

static uint64_t stackHash(const rec_t* r)
{
    uint64_t h = 1469598103934665603ull ^ r->n;
    for (unsigned k = 0; k < r->n; k++)
        h = (h ^ r->pc[k]) * 1099511628211ull;
    return h ? h : 1;
}

static int byBytes(const void* a, const void* b)
{
    const fold_t *x = a, *y = b;
    return x->bytes < y->bytes ? 1 : x->bytes > y->bytes ? -1 : 0;
}

static unsigned snapshots;

static void writeSnapshot(const char* why)
{
    static rec_t* copy;
    static fold_t* folds;
    if (!copy) {
        copy = mmap(NULL, sizeof(rec_t) * SLOTS, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        folds = mmap(NULL, sizeof(fold_t) * SLOTS, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (copy == MAP_FAILED || folds == MAP_FAILED) {
            copy = NULL;
            spike_log("[heapprof] no memory for a snapshot");
            return;
        }
    }
    if (ps5wk_backtrace_refresh_jit)
        ps5wk_backtrace_refresh_jit();
    unsigned n = 0;
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < SLOTS; i++)
        if (table[i].p)
            copy[n++] = table[i];
    pthread_mutex_unlock(&lock);

    /* Fold by stack: open addressing over folds[]. */
    memset(folds, 0, sizeof(fold_t) * SLOTS);
    unsigned distinct = 0;
    uint64_t total = 0;
    for (unsigned i = 0; i < n; i++) {
        uint64_t h = stackHash(&copy[i]);
        unsigned j = (unsigned)(h >> 20) & (SLOTS - 1);
        while (folds[j].hash && folds[j].hash != h)
            j = (j + 1) & (SLOTS - 1);
        if (!folds[j].hash) {
            folds[j].hash = h;
            folds[j].index = i;
            distinct++;
        }
        folds[j].bytes += copy[i].bytes;
        folds[j].count++;
        total += copy[i].bytes;
    }
    unsigned m = 0;
    for (unsigned j = 0; j < SLOTS; j++)
        if (folds[j].hash)
            folds[m++] = folds[j];
    qsort(folds, m, sizeof *folds, byBytes);

    dheap_stats_t st;
    dheap_stats(&st);
    unsigned number = ++snapshots;
    const char* dir = "/data/ps5webkit/prof";
    mkdir(dir, 0777);
    char path[160], tmp[168];
    snprintf(path, sizeof path, "%s/heap-%d-%03u.txt", dir, (int)getpid(), number);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f) {
        spike_log("[heapprof] cannot write %s", tmp);
        return;
    }
    char pcText[40] = "0";
    if (ps5wk_profiler_start) /* its offset, as the profiler's marker: tools/heapprof.py checks the ELF */
        ps5wk_backtrace_format_pc(pcText, sizeof pcText, (uintptr_t)ps5wk_profiler_start);
    fprintf(f, "# ps5heap 1 pid=%d n=%u ms=%.0f why=%s interval=%llu in_use=%zu mapped=%zu live=%u sampled=%llu dropped=%llu marker=%s\n",
        (int)getpid(), number, spike_now_ms() - startMs, why, (unsigned long long)interval, dheap_in_use(), st.mapped, n,
        (unsigned long long)atomic_load(&sampled), (unsigned long long)atomic_load(&dropped), pcText);
    for (unsigned j = 0; j < m; j++) {
        const rec_t* r = &copy[folds[j].index];
        fprintf(f, "S %llu %u", (unsigned long long)folds[j].bytes, folds[j].count);
        for (unsigned k = 0; k < r->n; k++) {
            ps5wk_backtrace_format_pc(pcText, sizeof pcText, r->pc[k]);
            fprintf(f, " %s", pcText);
        }
        fputc('\n', f);
    }
    fclose(f);
    rename(tmp, path);
    if (number > KEEP) {
        snprintf(path, sizeof path, "%s/heap-%d-%03u.txt", dir, (int)getpid(), number - KEEP);
        unlink(path);
    }
    spike_log("[heapprof] snapshot %u (%s): in use %.1f MB of %.1f MB mapped; %u live samples ~ %.1f MB in %u stacks; %llu sampled, %llu dropped",
        number, why, dheap_in_use() / 1048576.0, st.mapped / 1048576.0, n, total / 1048576.0, distinct,
        (unsigned long long)atomic_load(&sampled), (unsigned long long)atomic_load(&dropped));
}

static void* snapshotThread(void* arg)
{
    (void)arg;
    /* The title profiler's signals cut sleep() short (4259 snapshots in a
     * 5-minute run): wait until the minute has really passed. */
    double next = spike_now_ms() + 60000;
    for (;;) {
        double left = next - spike_now_ms();
        if (left > 0) {
            usleep(left > 1000 ? 1000000 : (useconds_t)(left * 1000));
            continue;
        }
        next += 60000;
        writeSnapshot("60 s");
    }
    return NULL;
}

void ps5wk_heapprof_snapshot(const char* why)
{
    if (table)
        writeSnapshot(why);
}

/* From the title's main thread, early. */
void ps5wk_heapprof_start(void)
{
    FILE* file = fopen("/data/ps5webkit/heap-profile", "r");
    if (!file)
        return;
    char line[32] = "";
    if (fgets(line, sizeof line, file) && atoi(line) > 0)
        interval = (uint64_t)atoi(line) << 10;
    fclose(file);
    if (!ps5wk_backtrace_init || !ps5wk_backtrace_here || ps5wk_backtrace_init()) {
        spike_log("[heapprof] no unwinder: off");
        return;
    }
    table = mmap(NULL, sizeof(rec_t) * SLOTS, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    filter = mmap(NULL, FILTER, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (table == MAP_FAILED || filter == MAP_FAILED) {
        table = NULL;
        spike_log("[heapprof] no memory for the table: off");
        return;
    }
    if (pthread_mutex_init(&lock, NULL)) {
        table = NULL;
        spike_log("[heapprof] no mutex: off");
        return;
    }
    /* The main thread's stack bounds, for its walks (others are in the
     * thread table through titlemem.c's pthread_create wrapper). */
    if (ps5wk_profiler_register_self)
        ps5wk_profiler_register_self();
    startMs = spike_now_ms();
    atomic_store(&counted, 0);
    dheap_hook_free = onFree;
    dheap_hook_alloc = onAlloc;
    pthread_t thread;
    pthread_create(&thread, NULL, snapshotThread, NULL);
    pthread_detach(thread);
    spike_log("[heapprof] on: one sample per %llu KB allocated, %u slots (%.1f MB), snapshots every 60 s to /data/ps5webkit/prof/heap-%d-*.txt",
        (unsigned long long)(interval >> 10), SLOTS, sizeof(rec_t) * SLOTS / 1048576.0, (int)getpid());
}
