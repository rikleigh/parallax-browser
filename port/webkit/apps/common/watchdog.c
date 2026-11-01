// Main-thread watchdog for ps5wk-child (the WebProcess and the
// NetworkProcess), on by default: PS5WK_WATCHDOG_S=<seconds> sets the stall
// threshold (default 3), 0 turns it off.
//
// A GLib timeout on the main context (G_PRIORITY_HIGH, every 250 ms) marks
// each turn of WebKit's main run loop with the time and the main thread's own
// CPU clock. A helper thread wakes once a second; when the main thread has
// not turned for the threshold, it signals it (SIGVTALRM, unused by WebKit,
// JSC, GLib and the profiler's SIGPROF), and the handler unwinds the
// interrupted stack with the profiler's unwinder (profiler.c, ps5unwind.c)
// into a static buffer. Then the other threads, once per stall. Every ~10 s
// while the stall lasts: the main thread again. Then a line when it turns.
//
//   [watchdog web3] main thread stalled: no run loop turn for 3.2 s, its CPU +3.1 s (busy); backtraces follow
//   [watchdog web3] bt main t17 cpu 41234 ms: 1a2b3c 1a2b00 j2 j1 x8001234 ...
//   [watchdog web3] bt t3 JSC_Heap_Collector cpu 812 ms: ...
//   [watchdog web3] bt t9 WebCore_Scrolling: same as t3
//   [watchdog web3] main thread still stalled after 13.2 s, its CPU +13.0 s (busy)
//   [watchdog web3] bt main t17 cpu 51210 ms: ...
//   [watchdog web3] main thread recovered: stalled 16.4 s, its CPU +16.2 s (busy)
//
// Frames are the profiler's notation (profiler.c): hex offsets in the
// unstripped ELF (llvm-addr2line -f -C -i -e .../ps5wk-child 0x<offset>; for
// the frames after the first, the offset is a return address: the call is
// at offset - 1), "j<tier>" JIT code (0 thunks/ICs/regexp, 1 Baseline,
// 2 DFG, 3 FTL), "x<hex>" absolute addresses in other modules (libkernel,
// libc: a system call or a wait). "busy": the main thread used more than
// half of the stall's wall time on the CPU (JavaScript, layout); otherwise
// it is waiting (a lock, a sync IPC reply, I/O).
//
// Cost while nothing is wrong: 4 timer dispatches a second on the main
// thread, one wake-up a second of the helper. The helper takes no WebKit or
// GLib lock (the stalled main thread may hold one) and writes with write(2).
#include "watchdog.h"
#include "profiler.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// GLib is linked statically; this target has no GLib headers.
typedef int (*ps5wk_gsource_func)(void*);
unsigned g_timeout_add_full(int priority, unsigned interval, ps5wk_gsource_func function, void* data, void (*notify)(void*));
#define PS5WK_G_PRIORITY_HIGH (-100)

int pthread_getname_np(pthread_t, char*, size_t) __attribute__((weak));

#define WATCHDOG_SIGNAL SIGVTALRM
#define BEAT_MS 250
#define REPEAT_MS 10000
#define REPLY_WAIT_MS 200
#define MAX_THREADS 512
#define OTHER_FRAMES 48

enum { IDLE, REQUESTED, WRITING, DONE };

static char tag[32];
static int64_t thresholdMS;
static pthread_t mainThread;
static _Atomic int running;
static _Atomic int64_t beatMS; // 0 until the run loop turns the first time
static _Atomic int64_t beatCPUNS;

// One request at a time (the helper thread is the only requester).
static _Atomic int replyState;
static _Atomic uintptr_t replyTarget;
static uintptr_t replyPC[PS5WK_BACKTRACE_MAX];
static unsigned replyN;
static int64_t replyCPUNS;

static int64_t nowMS(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t selfCPUNS(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts))
        return -1;
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static int beat(void* data)
{
    (void)data;
    atomic_store_explicit(&beatCPUNS, selfCPUNS(), memory_order_relaxed);
    atomic_store_explicit(&beatMS, nowMS(), memory_order_release);
    return 1; // G_SOURCE_CONTINUE
}

static void onSignal(int sig, siginfo_t* info, void* context)
{
    (void)sig;
    (void)info;
    int savedErrno = errno;
    if ((uintptr_t)pthread_self() == atomic_load(&replyTarget)) {
        int expected = REQUESTED;
        if (atomic_compare_exchange_strong(&replyState, &expected, WRITING)) {
            replyCPUNS = selfCPUNS();
            replyN = ps5wk_backtrace_context(context, replyPC);
            atomic_store_explicit(&replyState, DONE, memory_order_release);
        }
    }
    errno = savedErrno;
}

static void pause1ms(void)
{
    struct timespec ts = { 0, 1000000 };
    nanosleep(&ts, NULL);
}

// The thread's stack, from its own signal handler. 0, or -1 (no such
// thread) / -2 (no answer within REPLY_WAIT_MS: signals blocked, or it is
// stopped in the kernel).
static int capture(uintptr_t thread, uintptr_t* pcs, unsigned* n, int64_t* cpuNS)
{
    atomic_store(&replyTarget, thread);
    atomic_store(&replyState, REQUESTED);
    if (pthread_kill((pthread_t)thread, WATCHDOG_SIGNAL)) {
        atomic_store(&replyState, IDLE);
        atomic_store(&replyTarget, 0);
        return -1;
    }
    for (int i = 0; i < REPLY_WAIT_MS && atomic_load_explicit(&replyState, memory_order_acquire) != DONE; i++)
        pause1ms();
    int expected = REQUESTED;
    if (atomic_compare_exchange_strong(&replyState, &expected, IDLE)) {
        atomic_store(&replyTarget, 0);
        return -2; // a late signal finds the state IDLE and does nothing
    }
    for (int i = 0; i < 2000 && atomic_load_explicit(&replyState, memory_order_acquire) != DONE; i++)
        pause1ms(); // the handler is writing: it finishes in microseconds
    if (atomic_load_explicit(&replyState, memory_order_acquire) != DONE)
        return -2;
    unsigned count = replyN > PS5WK_BACKTRACE_MAX ? PS5WK_BACKTRACE_MAX : replyN;
    memcpy(pcs, replyPC, count * sizeof *pcs);
    *n = count;
    *cpuNS = replyCPUNS;
    atomic_store(&replyTarget, 0);
    atomic_store(&replyState, IDLE);
    return 0;
}

static void writeLine(const char* format, ...) __attribute__((format(printf, 1, 2)));

static void writeLine(const char* format, ...)
{
    char line[4096];
    int used = snprintf(line, sizeof line, "[watchdog %s] ", tag);
    va_list args;
    va_start(args, format);
    int more = vsnprintf(line + used, sizeof line - used - 1, format, args);
    va_end(args);
    if (more < 0)
        more = 0;
    used += more;
    if (used > (int)sizeof line - 2)
        used = (int)sizeof line - 2;
    line[used++] = '\n';
    ssize_t ignored = write(2, line, (size_t)used);
    (void)ignored;
}

// "12.3"
static const char* seconds(char* buffer, size_t size, int64_t ms)
{
    if (ms < 0)
        ms = 0;
    snprintf(buffer, size, "%lld.%lld", (long long)(ms / 1000), (long long)(ms % 1000 / 100));
    return buffer;
}

static void threadName(uintptr_t key, char* name, size_t size)
{
    name[0] = 0;
    if (pthread_getname_np)
        pthread_getname_np((pthread_t)key, name, size);
    if (!name[0])
        snprintf(name, size, "-");
    for (char* c = name; *c; c++)
        if (*c == ' ' || *c == '\n')
            *c = '_';
}

// The main thread's CPU since its last turn and "busy"/"waiting".
static void cpuText(char* buffer, size_t size, int64_t cpuNS, int64_t lastBeatCPU, int64_t wallMS)
{
    if (cpuNS < 0 || lastBeatCPU < 0) {
        snprintf(buffer, size, "its CPU unknown");
        return;
    }
    int64_t usedMS = (cpuNS - lastBeatCPU) / 1000000;
    char s[32];
    snprintf(buffer, size, "its CPU +%s s (%s)", seconds(s, sizeof s, usedMS), usedMS * 2 > wallMS ? "busy" : "waiting");
}

static void reportMain(int64_t stalledMS, int64_t lastBeatCPU, int first)
{
    uintptr_t pcs[PS5WK_BACKTRACE_MAX];
    unsigned n = 0;
    int64_t cpuNS = -1;
    ps5wk_backtrace_refresh_jit();
    int rc = capture((uintptr_t)mainThread, pcs, &n, &cpuNS);
    char s[32], cpu[80];
    cpuText(cpu, sizeof cpu, rc ? -1 : cpuNS, lastBeatCPU, stalledMS);
    if (first)
        writeLine("main thread stalled: no run loop turn for %s s, %s; backtraces follow", seconds(s, sizeof s, stalledMS), cpu);
    else
        writeLine("main thread still stalled after %s s, %s", seconds(s, sizeof s, stalledMS), cpu);
    int index = ps5wk_profiler_thread_index((uintptr_t)mainThread);
    if (rc) {
        writeLine("bt main t%d: %s", index, rc == -1 ? "thread gone" : "no answer to the signal");
        return;
    }
    char frames[3072];
    ps5wk_backtrace_format(frames, sizeof frames, pcs, n);
    writeLine("bt main t%d cpu %lld ms:%s", index, (long long)(cpuNS / 1000000), frames);
}

static uint64_t framesHash(const uintptr_t* pcs, unsigned n)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned i = 0; i < n; i++) {
        h ^= pcs[i];
        h *= 1099511628211ull;
    }
    return h | 1;
}

static void reportOthers(void)
{
    static uintptr_t keys[MAX_THREADS];
    static unsigned indices[MAX_THREADS];
    static uint64_t hashes[MAX_THREADS];
    unsigned count = ps5wk_profiler_threads(keys, indices, MAX_THREADS);
    unsigned others = 0, printed = 0, silent = 0;
    for (unsigned i = 0; i < count; i++) {
        hashes[i] = 0;
        if (keys[i] == (uintptr_t)mainThread)
            continue;
        uintptr_t pcs[PS5WK_BACKTRACE_MAX];
        unsigned n = 0;
        int64_t cpuNS = -1;
        char name[32];
        int rc = capture(keys[i], pcs, &n, &cpuNS);
        if (rc == -1)
            continue;
        others++;
        threadName(keys[i], name, sizeof name);
        if (rc) {
            writeLine("bt t%u %s: no answer to the signal", indices[i], name);
            silent++;
            continue;
        }
        if (n > OTHER_FRAMES)
            n = OTHER_FRAMES;
        hashes[i] = framesHash(pcs, n);
        unsigned same = i;
        for (unsigned k = 0; k < i; k++)
            if (hashes[k] == hashes[i]) {
                same = k;
                break;
            }
        if (same != i) {
            writeLine("bt t%u %s cpu %lld ms: same as t%u", indices[i], name, (long long)(cpuNS / 1000000), indices[same]);
            continue;
        }
        char frames[2048];
        ps5wk_backtrace_format(frames, sizeof frames, pcs, n);
        writeLine("bt t%u %s cpu %lld ms:%s", indices[i], name, (long long)(cpuNS / 1000000), frames);
        printed++;
    }
    writeLine("%u other threads (%u distinct stacks, %u silent)", others, printed, silent);
}

static void* watchdogThread(void* data)
{
    (void)data;
    ps5wk_profiler_own_thread();
    int64_t lastCheck = nowMS(), ignoreBefore = 0;
    int stalled = 0;
    int64_t stallBeat = 0, stallBeatCPU = -1, nextReport = 0;
    for (;;) {
        struct timespec ts = { 1, 0 };
        while (nanosleep(&ts, &ts) && errno == EINTR) { } // signals (the profiler's SIGPROF) cut sleeps short
        if (!atomic_load(&running))
            return NULL;
        int64_t now = nowMS();
        // We did not run for seconds ourselves: the whole process was
        // stopped (rest mode, a debugger). Not a stall of the main thread.
        if (now - lastCheck > 3000)
            ignoreBefore = now;
        lastCheck = now;
        int64_t beatAt = atomic_load_explicit(&beatMS, memory_order_acquire);
        if (!beatAt)
            continue; // the run loop has not started yet
        if (stalled) {
            char s[32], cpu[80];
            if (beatAt != stallBeat) {
                int64_t afterCPU = atomic_load_explicit(&beatCPUNS, memory_order_relaxed);
                cpuText(cpu, sizeof cpu, afterCPU, stallBeatCPU, beatAt - stallBeat);
                writeLine("main thread recovered: stalled %s s, %s", seconds(s, sizeof s, beatAt - stallBeat), cpu);
                stalled = 0;
                continue;
            }
            if (now >= nextReport) {
                reportMain(now - stallBeat, stallBeatCPU, 0);
                nextReport += REPEAT_MS;
            }
            continue;
        }
        int64_t since = now - (beatAt > ignoreBefore ? beatAt : ignoreBefore);
        if (since >= thresholdMS) {
            stalled = 1;
            stallBeat = beatAt;
            stallBeatCPU = atomic_load_explicit(&beatCPUNS, memory_order_relaxed);
            reportMain(now - beatAt, stallBeatCPU, 1);
            reportOthers();
            nextReport = now + REPEAT_MS;
        }
    }
}

void ps5wk_watchdog_start(const char* processTag)
{
    const char* knob = getenv("PS5WK_WATCHDOG_S");
    int s = knob && *knob ? atoi(knob) : 3;
    snprintf(tag, sizeof tag, "%s", processTag ? processTag : "?");
    if (s <= 0) {
        writeLine("off (PS5WK_WATCHDOG_S=0)");
        return;
    }
    thresholdMS = (int64_t)s * 1000;
    mainThread = pthread_self();
    ps5wk_profiler_register_self();
    int unwinding = ps5wk_backtrace_init() == 0;
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = onSignal;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(WATCHDOG_SIGNAL, &action, NULL)) {
        writeLine("sigaction failed (errno %d): off", errno);
        return;
    }
    // On the default main context: WebKit's main RunLoop (RunLoopGLib) runs it.
    g_timeout_add_full(PS5WK_G_PRIORITY_HIGH, BEAT_MS, beat, NULL, NULL);
    atomic_store(&running, 1);
    pthread_t thread;
    if (pthread_create(&thread, NULL, watchdogThread, NULL)) {
        atomic_store(&running, 0);
        writeLine("no thread: off");
        return;
    }
    pthread_detach(thread);
    writeLine("on: reports a main thread that does not turn for %d s (PS5WK_WATCHDOG_S; %s unwinding)", s, unwinding ? "CFI" : "no");
}

void ps5wk_watchdog_stop(void)
{
    atomic_store(&running, 0);
}
