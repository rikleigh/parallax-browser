// Sampling profiler for payloads (ps5wk-child: the WebProcess and the
// NetworkProcess). PS5WK_PROFILE in /data/ps5webkit/child.env (or
// ps5wk-shot's childenv=) turns it on:
//
//   PS5WK_PROFILE=1 (or cpu)  every thread, weighted by CPU time: a helper
//                             thread sends SIGPROF (pthread_kill) to the
//                             process's threads every 2 ms; the handler reads
//                             the thread's own CPU clock and records a sample
//                             weighted by the CPU used since its last one.
//                             Threads that were idle are signalled less and
//                             less often (down to about once a second).
//   PS5WK_PROFILE=wall        the process's WebKit main thread only, every 2 ms
//                             of wall time, idle or not (what it waits on)
//   PS5WK_PROFILE=threads     every thread every tick (default 20 ms), idle or
//                             not, weighted by wall time: where each thread
//                             waits (hangs and lock cycles between threads)
//   PS5WK_PROFILE=itimer      setitimer(ITIMER_PROF): the kernel signals the
//                             thread that used the CPU. Never run on the
//                             console: opt-in only.
//   PS5WK_PROFILE_US=<n>      tick in microseconds (default 2000)
//   PS5WK_PROFILE_WINDOW_MS=<n>  aggregation window (default 1000)
//
// On the PS5 (13.60) pthread_getcpuclockid() + clock_gettime() of another
// thread returns the *calling* thread's CPU time, so a thread's CPU can only
// be read by the thread itself: hence the reading in the handler.
// CLOCK_THREAD_CPUTIME_ID for the calling thread is right.
//
// The handler unwinds the interrupted stack with the executable's own DWARF
// CFI (ps5unwind.c), follows rbp through JSC's LLInt and JIT frames and
// through other modules (libkernel, libc), and writes the frames to a
// lock-free ring. A writer thread folds identical stacks per window and
// appends them to /data/ps5webkit/prof/<tag>-<pid>.prof:
//
//   # ps5prof 3 tag=... pid=... mode=... interval_us=... marker=<hex> ...
//   W <ms> <process cpu ms> <samples> <dropped> <jit start>-<jit end> <JIT counters>
//   T <thread> <generation> <cpu ms, last seen> <samples> <name>
//   S <thread> <count> <weight us> <pc> <pc> ...     (innermost first)
//
// pcs: hex offsets from the executable's start (llvm-symbolizer on the
// unstripped ELF), "j<tier>" for JIT code (0 thunks, inline caches and the
// regexp JIT, 1 Baseline, 2 DFG, 3 FTL; "j" unknown), "x<hex>" for
// addresses in other modules (libkernel, libc).
// port/webkit/test/prof/prof-run.sh runs pages, prof.py summarizes.
#include "profiler.h"
#include "ps5unwind.h"

#include <errno.h>
#include <pthread.h>
#include <pthread_np.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

extern char __ehdr_start;
extern char __eh_frame_hdr_start __attribute__((weak));
extern char __eh_frame_start __attribute__((weak));
extern char __eh_frame_end __attribute__((weak));
// LLInt's code (labels in LowLevelInterpreter.asm, hidden in the archive).
extern void llintPCRangeStart(void) __attribute__((weak, visibility("hidden")));
extern void llintPCRangeEnd(void) __attribute__((weak, visibility("hidden")));
// JSC's executable memory and JIT counters (WebKit patches 0030 and 0035).
void ps5wk_jit_memory(void** start, void** end, size_t* allocated, size_t* committed) __attribute__((weak));
void ps5wk_jit_counts(unsigned long long* counts, size_t n) __attribute__((weak));
int ps5wk_jit_tier(const void* pc) __attribute__((weak));
int pthread_getname_np(pthread_t, char*, size_t) __attribute__((weak));

#define MAX_FRAMES 128
#define RING (1u << 13)
#define THREADS 1024
#define SCAN_WORDS 64
#define OTHER_MODULE_FRAMES 12
#define MAX_BACKOFF 9 // idle threads: every 2^9 ticks (~1 s at 2 ms)

typedef struct {
    _Atomic unsigned seq;
    unsigned short n;
    unsigned short flags;
    uint32_t weightUS;
    uintptr_t thread;
    uintptr_t pc[MAX_FRAMES];
} slot_t;

typedef struct {
    _Atomic uintptr_t key; // pthread_t
    _Atomic int ready; // lo/hi valid
    uintptr_t lo, hi;
    int exited;
    int own; // the profiler's threads: never signalled
    unsigned generation;
    _Atomic int64_t lastCPUNS; // the thread's CPU at its last signal (-1: not read yet)
    _Atomic int skip; // ticks to leave out (idle backoff)
    _Atomic int idleRuns;
    double reportedCPU;
    unsigned samples, reportedSamples;
    char name[32];
} thread_t;

static int mode; // 0 off, 1 itimer, 2 cpu (signalled, self-timed), 3 wall, 4 threads (all, wall time)
static unsigned intervalUS = 2000, windowMS = 1000;
static slot_t* ring;
static _Atomic unsigned head, tail;
static _Atomic unsigned long long dropped, samplesTaken, signalsSent, idleSignals;
static ps5unwind_info_t unwindInfo;
static int unwindReady;
static uintptr_t base, textStart, textEnd, llintStart, llintEnd;
static _Atomic uintptr_t jitStart, jitEnd;
static pthread_t wallTarget;
static thread_t threads[THREADS];
static char path[160];
static double startMS;

static double nowMS(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int64_t selfCPUNS(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts))
        return -1;
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static unsigned threadHash(uintptr_t key)
{
    key ^= key >> 17;
    key *= 0x9e3779b97f4a7c15ull;
    return (unsigned)(key >> 40) & (THREADS - 1);
}

// Async-signal-safe lookup.
static thread_t* findThread(uintptr_t key)
{
    unsigned h = threadHash(key);
    for (unsigned i = 0; i < THREADS; i++) {
        thread_t* t = &threads[(h + i) & (THREADS - 1)];
        uintptr_t k = atomic_load_explicit(&t->key, memory_order_acquire);
        if (k == key)
            return t;
        if (!k)
            return NULL;
    }
    return NULL;
}

static thread_t* insertThread(uintptr_t key)
{
    unsigned h = threadHash(key);
    for (unsigned i = 0; i < THREADS; i++) {
        thread_t* t = &threads[(h + i) & (THREADS - 1)];
        uintptr_t k = atomic_load_explicit(&t->key, memory_order_acquire);
        if (k == key)
            return t;
        if (!k) {
            uintptr_t expected = 0;
            if (atomic_compare_exchange_strong(&t->key, &expected, key))
                return t;
            if (expected == key)
                return t;
        }
    }
    return NULL;
}

// In the thread itself: stack bounds and CPU so far.
static void fillThread(thread_t* t)
{
    atomic_store_explicit(&t->ready, 0, memory_order_release);
    pthread_t self = pthread_self();
    pthread_attr_t attr;
    void* stackBase = NULL;
    size_t stackSize = 0;
    if (!pthread_attr_init(&attr)) {
        if (!pthread_attr_get_np(self, &attr))
            pthread_attr_getstack(&attr, &stackBase, &stackSize);
        pthread_attr_destroy(&attr);
    }
    t->exited = 0;
    t->own = 0;
    t->generation++;
    t->name[0] = 0;
    t->reportedCPU = -1;
    atomic_store(&t->lastCPUNS, selfCPUNS());
    atomic_store(&t->skip, 0);
    atomic_store(&t->idleRuns, 0);
    if (stackBase && stackSize) {
        t->lo = (uintptr_t)stackBase;
        t->hi = (uintptr_t)stackBase + stackSize;
        atomic_store_explicit(&t->ready, 1, memory_order_release);
    }
}

// Called by the crash guard's pthread_create trampoline in each new thread
// (crashguard.c), before and after the thread function.
void ps5wk_profiler_thread(int starting)
{
    pthread_t self = pthread_self();
    if (starting) {
        thread_t* t = insertThread((uintptr_t)self);
        if (t)
            fillThread(t);
        return;
    }
    thread_t* t = findThread((uintptr_t)self);
    if (!t)
        return;
    atomic_store(&t->lastCPUNS, selfCPUNS());
    atomic_store_explicit(&t->ready, 0, memory_order_release);
    t->exited = 1;
}

static int inText(uintptr_t pc)
{
    return pc >= textStart && pc < textEnd;
}

static int inJIT(uintptr_t pc)
{
    return pc >= atomic_load_explicit(&jitStart, memory_order_relaxed) && pc < atomic_load_explicit(&jitEnd, memory_order_relaxed);
}

static int inLLInt(uintptr_t pc)
{
    return pc >= llintStart && pc < llintEnd;
}

// A return address: the bytes before it are a call instruction.
static int afterCall(uintptr_t address)
{
    if (address < textStart + 8 || address >= textEnd)
        return 0;
    const unsigned char* b = (const unsigned char*)address;
    if (b[-5] == 0xe8)
        return 1;
    if (b[-2] == 0xff && (b[-1] & 0x38) == 0x10)
        return 1;
    if (b[-3] == 0xff && (b[-2] & 0x38) == 0x10)
        return 1;
    if (b[-6] == 0xff && (b[-5] & 0x38) == 0x10)
        return 1;
    if (b[-7] == 0xff && (b[-6] & 0x38) == 0x10)
        return 1;
    if (b[-4] == 0xff && (b[-3] & 0x38) == 0x10)
        return 1;
    return 0;
}

static int readWord(uintptr_t address, uintptr_t lo, uintptr_t hi, uintptr_t* out)
{
    if ((address & 7) || address < lo || address + 8 > hi)
        return -1;
    *out = *(const uintptr_t*)address;
    return 0;
}

// rbp chain: JSC's LLInt and JIT frames (rbp is the call frame register),
// code without CFI, and Sony's modules (built with frame pointers).
static int framePointerStep(ps5unwind_regs_t* r, uintptr_t lo, uintptr_t hi)
{
    uintptr_t fp = r->fp, ra, next;
    if (fp < r->sp || readWord(fp + 8, lo, hi, &ra) || readWord(fp, lo, hi, &next))
        return -1;
    if (next && next <= fp)
        next = 0; // the chain must go up the stack; 0 stops the next step
    r->pc = ra;
    r->sp = fp + 16;
    r->fp = next;
    return 0;
}

static unsigned unwind(uintptr_t pc, uintptr_t sp, uintptr_t fp, uintptr_t lo, uintptr_t hi, uintptr_t* out)
{
    unsigned n = 0;
    out[n++] = pc;
    if (!unwindReady)
        return n;
    ps5unwind_regs_t r = { pc, sp, fp };
    int exact = 1, otherFrames = 0;
    while (n < MAX_FRAMES) {
        int ok = -1;
        uintptr_t oldSP = r.sp;
        int other = !inText(r.pc) && !inJIT(r.pc);
        if (!other && !inLLInt(r.pc))
            ok = ps5unwind_step(&unwindInfo, &r, exact, lo, hi);
        if (ok)
            ok = framePointerStep(&r, lo, hi);
        if (ok && other && exact) {
            // A leaf in another module without a frame (a system call stub,
            // memcpy): the first return address into this executable.
            r.sp = oldSP;
            for (unsigned i = 0; i < SCAN_WORDS; i++) {
                uintptr_t word;
                if (readWord(r.sp + i * 8, lo, hi, &word))
                    break;
                if (afterCall(word)) {
                    r.pc = word;
                    r.sp = r.sp + i * 8 + 8;
                    ok = 0;
                    break;
                }
            }
        }
        if (ok || r.sp <= oldSP || !r.pc)
            break;
        if (!inText(r.pc) && !inJIT(r.pc)) {
            // Up through another module's frames for a while (its callbacks
            // into this executable, e.g. the thread entry), then stop.
            if (++otherFrames > OTHER_MODULE_FRAMES)
                break;
        } else
            otherFrames = 0;
        out[n++] = r.pc;
        exact = 0;
    }
    // Trailing frames in other modules (libkernel's thread start) say nothing.
    while (n > 1 && !inText(out[n - 1]) && !inJIT(out[n - 1]))
        n--;
    return n;
}

// The interrupted frame of a signal context, unwound. Async-signal-safe.
static unsigned contextUnwind(const void* context, thread_t* t, uintptr_t* out)
{
    // PS5 ucontext: FreeBSD's mcontext at +0x40 (crashguard.c): rip +0xe0,
    // rsp +0xf8, rbp +0x88.
    const uintptr_t* words = (const uintptr_t*)context;
    uintptr_t pc = words[0xe0 / 8], sp = words[0xf8 / 8], fp = words[0x88 / 8];
    uintptr_t lo = sp, hi = (sp + 4096) & ~(uintptr_t)4095; // without known bounds: sp's page
    if (t && atomic_load_explicit(&t->ready, memory_order_acquire) && sp >= t->lo && sp < t->hi)
        hi = t->hi;
    return unwind(pc, sp, fp, lo, hi, out);
}

static void onProf(int sig, siginfo_t* info, void* context)
{
    (void)sig;
    (void)info;
    int savedErrno = errno;
    uintptr_t self = (uintptr_t)pthread_self();
    thread_t* t = findThread(self);
    uint32_t weightUS = intervalUS;
    if (mode == 2) {
        // Weighted by the CPU this thread used since its last signal; a
        // thread that barely ran is left alone for longer each time.
        int64_t now = selfCPUNS();
        if (!t || now < 0) {
            errno = savedErrno;
            return;
        }
        int64_t last = atomic_exchange(&t->lastCPUNS, now);
        int64_t used = last >= 0 ? now - last : 0;
        if (used < (int64_t)intervalUS * 250) { // under a quarter of a tick
            int runs = atomic_load(&t->idleRuns);
            if (runs < MAX_BACKOFF)
                atomic_store(&t->idleRuns, ++runs);
            atomic_store(&t->skip, (1 << runs) - 1);
            atomic_fetch_add_explicit(&idleSignals, 1, memory_order_relaxed);
            errno = savedErrno;
            return;
        }
        atomic_store(&t->idleRuns, 0);
        atomic_store(&t->skip, 0);
        weightUS = (uint32_t)(used / 1000 > 0xfffffff ? 0xfffffff : used / 1000);
    }
    unsigned h = atomic_load_explicit(&head, memory_order_relaxed);
    for (;;) {
        unsigned tl = atomic_load_explicit(&tail, memory_order_acquire);
        if (h - tl >= RING) {
            atomic_fetch_add_explicit(&dropped, 1, memory_order_relaxed);
            errno = savedErrno;
            return;
        }
        if (atomic_compare_exchange_weak_explicit(&head, &h, h + 1, memory_order_acq_rel, memory_order_relaxed))
            break;
    }
    slot_t* s = &ring[h & (RING - 1)];
    s->thread = self;
    s->weightUS = weightUS;
    s->n = (unsigned short)contextUnwind(context, t, s->pc);
    s->flags = 0;
    atomic_fetch_add_explicit(&samplesTaken, 1, memory_order_relaxed);
    atomic_store_explicit(&s->seq, h + 1, memory_order_release);
    errno = savedErrno;
}

// Folding: identical (thread, stack) pairs per window.
typedef struct {
    uint64_t hash;
    unsigned count;
    uint64_t weightUS;
    unsigned short n;
    short thread;
    uintptr_t pc[MAX_FRAMES];
} folded_t;

#define FOLD (1u << 13)
static folded_t* fold;
static unsigned folded;

static uint64_t stackHash(int thread, const uintptr_t* pc, unsigned n)
{
    uint64_t h = 1469598103934665603ull ^ (uint64_t)thread;
    for (unsigned i = 0; i < n; i++) {
        h ^= pc[i];
        h *= 1099511628211ull;
    }
    return h | 1;
}

static void addToFold(int thread, const uintptr_t* pc, unsigned n, uint32_t weightUS)
{
    uint64_t h = stackHash(thread, pc, n);
    for (unsigned i = 0; i < FOLD; i++) {
        folded_t* e = &fold[(h + i) & (FOLD - 1)];
        if (!e->hash) {
            e->hash = h;
            e->count = 1;
            e->weightUS = weightUS;
            e->n = (unsigned short)n;
            e->thread = (short)thread;
            memcpy(e->pc, pc, n * sizeof *pc);
            folded++;
            return;
        }
        if (e->hash == h && e->thread == thread && e->n == n && !memcmp(e->pc, pc, n * sizeof *pc)) {
            e->count++;
            e->weightUS += weightUS;
            return;
        }
    }
}

static void drain(void)
{
    unsigned t = atomic_load_explicit(&tail, memory_order_relaxed);
    for (;;) {
        slot_t* s = &ring[t & (RING - 1)];
        if (atomic_load_explicit(&s->seq, memory_order_acquire) != t + 1)
            break;
        thread_t* th = findThread(s->thread);
        if (!th) {
            th = insertThread(s->thread); // a thread the trampoline did not see
            if (th) {
                th->generation++;
                atomic_store(&th->lastCPUNS, -1);
            }
        }
        int index = th ? (int)(th - threads) : -1;
        if (th)
            th->samples++;
        unsigned n = s->n > MAX_FRAMES ? MAX_FRAMES : s->n;
        addToFold(index, s->pc, n, s->weightUS);
        atomic_store_explicit(&tail, ++t, memory_order_release);
        if (folded > FOLD * 3 / 4)
            break;
    }
}

// The tier that made the code at a JIT pc (patch 0035 searches its last 64k
// allocations), cached per pc: 0 thunks/ICs/regexp, 1 Baseline, 2 DFG, 3 FTL.
#define TIER_CACHE 8192
static uintptr_t tierPC[TIER_CACHE];
static signed char tierValue[TIER_CACHE];

static int jitTier(uintptr_t pc)
{
    if (!ps5wk_jit_tier)
        return -1;
    unsigned h = (unsigned)((pc * 0x9e3779b97f4a7c15ull) >> 51) & (TIER_CACHE - 1);
    if (tierPC[h] == pc)
        return tierValue[h];
    int tier = ps5wk_jit_tier((const void*)pc);
    tierPC[h] = pc;
    tierValue[h] = (signed char)tier;
    return tier;
}

static void writeWindow(FILE* f)
{
    double now = nowMS();
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    double processMS = usage.ru_utime.tv_sec * 1e3 + usage.ru_utime.tv_usec / 1e3 + usage.ru_stime.tv_sec * 1e3 + usage.ru_stime.tv_usec / 1e3;
    char counts[256] = "";
    if (ps5wk_jit_counts) {
        unsigned long long c[16] = { 0 };
        ps5wk_jit_counts(c, 16);
        int used = 0;
        for (int i = 0; i < 16 && used < (int)sizeof counts - 24; i++)
            used += snprintf(counts + used, sizeof counts - used, " %llu", c[i]);
    }
    // W: window end (ms since the start), process CPU ms, samples, drops,
    // signals sent and signals to idle threads so far, the JIT pool,
    // ps5wk_jit_counts (see ExecutableAllocator.cpp).
    fprintf(f, "W %.0f %.0f %llu %llu %llu %llu %lx-%lx%s\n", now - startMS, processMS, (unsigned long long)atomic_load(&samplesTaken),
        (unsigned long long)atomic_load(&dropped), (unsigned long long)atomic_load(&signalsSent), (unsigned long long)atomic_load(&idleSignals),
        (unsigned long)atomic_load(&jitStart), (unsigned long)atomic_load(&jitEnd), counts);
    for (unsigned i = 0; i < THREADS; i++) {
        thread_t* t = &threads[i];
        if (!atomic_load(&t->key) || t->own)
            continue;
        int64_t cpu = atomic_load(&t->lastCPUNS);
        double cpuMS = cpu >= 0 ? cpu / 1e6 : -1;
        if (cpuMS == t->reportedCPU && t->samples == t->reportedSamples && t->name[0])
            continue;
        if (!t->exited && pthread_getname_np)
            pthread_getname_np((pthread_t)atomic_load(&t->key), t->name, sizeof t->name);
        for (char* c = t->name; *c; c++)
            if (*c == ' ' || *c == '\n')
                *c = '_';
        fprintf(f, "T %u %u %.1f %u %s\n", i, t->generation, cpuMS, t->samples, t->name[0] ? t->name : "-");
        t->reportedCPU = cpuMS;
        t->reportedSamples = t->samples;
    }
    uintptr_t js = atomic_load(&jitStart), je = atomic_load(&jitEnd);
    for (unsigned i = 0; i < FOLD; i++) {
        folded_t* e = &fold[i];
        if (!e->hash)
            continue;
        fprintf(f, "S %d %u %llu", e->thread, e->count, (unsigned long long)e->weightUS);
        for (unsigned k = 0; k < e->n; k++) {
            uintptr_t pc = e->pc[k];
            if (pc >= textStart && pc < textEnd)
                fprintf(f, " %lx", (unsigned long)(pc - base));
            else if (pc >= js && pc < je) {
                int tier = jitTier(pc);
                if (tier >= 0)
                    fprintf(f, " j%d", tier);
                else
                    fputs(" j", f);
            } else
                fprintf(f, " x%lx", (unsigned long)pc);
        }
        fputc('\n', f);
        e->hash = 0;
    }
    folded = 0;
    static unsigned windows;
    if (++windows % 30 == 0) // code is freed and reused: forget old answers
        memset(tierPC, 0, sizeof tierPC);
}

static void markOwnThread(void)
{
    thread_t* t = insertThread((uintptr_t)pthread_self());
    if (t)
        t->own = 1;
}

static void startTimer(void);

static void* writerThread(void* data)
{
    (void)data;
    markOwnThread();
    double lastWindow = nowMS();
    double checkAt = lastWindow + 3000;
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    double cpuAtStart = usage.ru_utime.tv_sec * 1e3 + usage.ru_utime.tv_usec / 1e3;
    for (;;) {
        struct timespec tick = { 0, 50 * 1000 * 1000 };
        nanosleep(&tick, NULL);
        if (ps5wk_jit_memory) { // JSC maps its pool when the first page needs it
            void *start = NULL, *end = NULL;
            ps5wk_jit_memory(&start, &end, NULL, NULL);
            atomic_store(&jitStart, (uintptr_t)start);
            atomic_store(&jitEnd, (uintptr_t)end);
        }
        drain();
        double now = nowMS();
        if (mode == 1 && checkAt && now >= checkAt) {
            checkAt = 0;
            getrusage(RUSAGE_SELF, &usage);
            double cpu = usage.ru_utime.tv_sec * 1e3 + usage.ru_utime.tv_usec / 1e3;
            if (!atomic_load(&samplesTaken) && cpu - cpuAtStart > 100) {
                struct itimerval off;
                memset(&off, 0, sizeof off);
                setitimer(ITIMER_PROF, &off, NULL);
                fprintf(stderr, "[profiler] no SIGPROF from the itimer after %.0f ms of CPU: signalling threads instead\n", cpu - cpuAtStart);
                mode = 2;
                startTimer();
            }
        }
        if (now - lastWindow >= windowMS || folded > FOLD * 3 / 4) {
            FILE* f = fopen(path, "a");
            if (f) {
                writeWindow(f);
                fclose(f);
            }
            lastWindow = now;
        }
    }
    return NULL;
}

// Wall mode: one thread, every tick. CPU mode: every thread not in its idle
// backoff (the handler decides whether the tick becomes a sample). Threads
// mode: every thread, every tick (skip stays 0: the handler never sets it).
static void* timerThread(void* data)
{
    (void)data;
    markOwnThread();
    struct timespec tick = { intervalUS / 1000000, (long)(intervalUS % 1000000) * 1000 };
    for (;;) {
        nanosleep(&tick, NULL);
        if (mode == 3) {
            pthread_kill(wallTarget, SIGPROF);
            atomic_fetch_add_explicit(&signalsSent, 1, memory_order_relaxed);
            continue;
        }
        if (mode != 2 && mode != 4)
            return NULL;
        for (unsigned i = 0; i < THREADS; i++) {
            thread_t* t = &threads[i];
            uintptr_t key = atomic_load_explicit(&t->key, memory_order_acquire);
            if (!key || t->exited || t->own || !atomic_load_explicit(&t->ready, memory_order_acquire))
                continue;
            int skip = atomic_load_explicit(&t->skip, memory_order_relaxed);
            if (skip > 0) {
                atomic_store_explicit(&t->skip, skip - 1, memory_order_relaxed);
                continue;
            }
            if (!pthread_kill((pthread_t)key, SIGPROF))
                atomic_fetch_add_explicit(&signalsSent, 1, memory_order_relaxed);
            else
                t->exited = 1;
        }
    }
    return NULL;
}

static void startTimer(void)
{
    pthread_t thread;
    if (!pthread_create(&thread, NULL, timerThread, NULL))
        pthread_detach(thread);
}

// Backtraces for the main-thread watchdog (watchdog.c), with or without
// PS5WK_PROFILE: the same unwinder, thread table and frame notation.
int ps5wk_backtrace_init(void)
{
    static int done;
    if (done)
        return unwindReady ? 0 : -1;
    done = 1;
    base = (uintptr_t)&__ehdr_start;
    if (&__eh_frame_hdr_start && !ps5unwind_init(&unwindInfo, &__eh_frame_hdr_start, &__eh_frame_start ? &__eh_frame_start : NULL,
            &__eh_frame_end ? &__eh_frame_end : NULL))
        unwindReady = 1;
    textStart = unwindReady ? unwindInfo.textStart : base;
    textEnd = unwindReady ? unwindInfo.textEnd : base;
    if (llintPCRangeStart && llintPCRangeEnd) {
        llintStart = (uintptr_t)llintPCRangeStart;
        llintEnd = (uintptr_t)llintPCRangeEnd;
    }
    return unwindReady ? 0 : -1;
}

unsigned ps5wk_backtrace_context(const void* ucontext, uintptr_t* out)
{
    return contextUnwind(ucontext, findThread((uintptr_t)pthread_self()), out);
}

void ps5wk_backtrace_refresh_jit(void)
{
    if (ps5wk_jit_memory) {
        void *start = NULL, *end = NULL;
        ps5wk_jit_memory(&start, &end, NULL, NULL);
        atomic_store(&jitStart, (uintptr_t)start);
        atomic_store(&jitEnd, (uintptr_t)end);
    }
}

// " <hex>" without the C library (the watchdog formats while other threads
// may hold any libc lock).
static size_t appendHex(char* buffer, size_t used, size_t size, const char* prefix, uintptr_t value)
{
    char digits[2 * sizeof value];
    unsigned n = 0;
    do {
        digits[n++] = "0123456789abcdef"[value & 15];
        value >>= 4;
    } while (value && n < sizeof digits);
    for (const char* p = prefix; *p && used + 1 < size; p++)
        buffer[used++] = *p;
    while (n && used + 1 < size)
        buffer[used++] = digits[--n];
    buffer[used] = 0;
    return used;
}

size_t ps5wk_backtrace_format(char* buffer, size_t size, const uintptr_t* pcs, unsigned n)
{
    size_t used = 0;
    if (!size)
        return 0;
    buffer[0] = 0;
    uintptr_t js = atomic_load(&jitStart), je = atomic_load(&jitEnd);
    for (unsigned k = 0; k < n && used + 24 < size; k++) {
        uintptr_t pc = pcs[k];
        if (pc >= textStart && pc < textEnd)
            used = appendHex(buffer, used, size, " ", pc - base);
        else if (pc >= js && pc < je) {
            // Uncached: the profiler's cache belongs to its writer thread.
            int tier = ps5wk_jit_tier ? ps5wk_jit_tier((const void*)pc) : -1;
            buffer[used++] = ' ';
            buffer[used++] = 'j';
            if (tier >= 0 && tier <= 9)
                buffer[used++] = (char)('0' + tier);
            buffer[used] = 0;
        } else
            used = appendHex(buffer, used, size, " x", pc);
    }
    return used;
}

unsigned ps5wk_profiler_threads(uintptr_t* keys, unsigned* indices, unsigned max)
{
    unsigned n = 0;
    for (unsigned i = 0; i < THREADS && n < max; i++) {
        thread_t* t = &threads[i];
        uintptr_t key = atomic_load_explicit(&t->key, memory_order_acquire);
        if (!key || t->exited || t->own || !atomic_load_explicit(&t->ready, memory_order_acquire))
            continue;
        keys[n] = key;
        indices[n] = i;
        n++;
    }
    return n;
}

int ps5wk_profiler_thread_index(uintptr_t key)
{
    thread_t* t = findThread(key);
    return t ? (int)(t - threads) : -1;
}

void ps5wk_profiler_own_thread(void)
{
    markOwnThread();
}

void ps5wk_profiler_register_self(void)
{
    thread_t* self = insertThread((uintptr_t)pthread_self());
    if (self && !atomic_load(&self->ready))
        fillThread(self);
}

void ps5wk_profiler_start(const char* tag)
{
    const char* on = getenv("PS5WK_PROFILE");
    if (!on || !*on || !strcmp(on, "0") || mode)
        return;
    if (!strcmp(on, "wall"))
        mode = 3;
    else if (!strcmp(on, "threads"))
        mode = 4;
    else if (!strcmp(on, "itimer"))
        mode = 1;
    else
        mode = 2; // 1, cpu
    intervalUS = mode == 1 ? 1000 : mode == 4 ? 20000 : 2000;
    if (getenv("PS5WK_PROFILE_US") && atoi(getenv("PS5WK_PROFILE_US")) >= 500)
        intervalUS = (unsigned)atoi(getenv("PS5WK_PROFILE_US"));
    if (getenv("PS5WK_PROFILE_WINDOW_MS") && atoi(getenv("PS5WK_PROFILE_WINDOW_MS")) >= 100)
        windowMS = (unsigned)atoi(getenv("PS5WK_PROFILE_WINDOW_MS"));
    ps5wk_backtrace_init();
    ring = (slot_t*)mmap(NULL, sizeof(slot_t) * RING, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    fold = (folded_t*)mmap(NULL, sizeof(folded_t) * FOLD, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (ring == MAP_FAILED || fold == MAP_FAILED) {
        fprintf(stderr, "[profiler] cannot map the sample buffers\n");
        mode = 0;
        return;
    }
    // The calling thread (WebKit's main thread) and its stack.
    wallTarget = pthread_self();
    ps5wk_profiler_register_self();
    startMS = nowMS();
    const char* dir = getenv("PS5WK_PROFILE_DIR");
    if (!dir || !*dir)
        dir = "/data/ps5webkit/prof";
    mkdir(dir, 0777);
    snprintf(path, sizeof path, "%s/%s-%d.prof", dir, tag, (int)getpid());
    FILE* f = fopen(path, "w");
    if (f) {
        fprintf(f, "# ps5prof 3 tag=%s pid=%d start_ms=%.0f mode=%s interval_us=%u window_ms=%u marker=%lx text=%lx-%lx llint=%lx-%lx unwind=%s\n", tag,
            (int)getpid(), startMS, mode == 1 ? "itimer" : mode == 2 ? "cpu" : mode == 4 ? "threads" : "wall", intervalUS, windowMS,
            (unsigned long)((uintptr_t)ps5wk_profiler_start - base), (unsigned long)(textStart - base), (unsigned long)(textEnd - base),
            (unsigned long)(llintStart ? llintStart - base : 0), (unsigned long)(llintEnd ? llintEnd - base : 0), unwindReady ? "cfi" : "none");
        fclose(f);
    }
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = onProf;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPROF, &action, NULL);
    pthread_t writer;
    if (!pthread_create(&writer, NULL, writerThread, NULL))
        pthread_detach(writer);
    if (mode == 1) {
        fprintf(stderr, "[profiler] setitimer(ITIMER_PROF, %u us)\n", intervalUS);
        struct itimerval timer;
        memset(&timer, 0, sizeof timer);
        timer.it_interval.tv_sec = timer.it_value.tv_sec = intervalUS / 1000000;
        timer.it_interval.tv_usec = timer.it_value.tv_usec = intervalUS % 1000000;
        if (setitimer(ITIMER_PROF, &timer, NULL)) {
            fprintf(stderr, "[profiler] setitimer failed (errno %d): signalling threads instead\n", errno);
            mode = 2;
        }
    }
    if (mode != 1)
        startTimer();
    fprintf(stderr, "[profiler] %s sampling, tick %u us (%s unwinding, %zu FDEs) -> %s\n",
        mode == 1 ? "CPU-time (itimer)" : mode == 2 ? "CPU-weighted (threads signalled, self-timed)" : mode == 4 ? "all threads, wall-time" : "main-thread wall-time", intervalUS,
        unwindReady ? "CFI" : "no", unwindInfo.count, path);
}
