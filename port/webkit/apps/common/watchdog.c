// Main-thread watchdog for ps5wk-child (the WebProcess and the
// NetworkProcess), on by default: PS5WK_WATCHDOG_S=<seconds> sets the stall
// threshold (default 3), 0 turns it off.
//
// A GLib timeout on the main context (G_PRIORITY_HIGH, every 250 ms) marks
// each turn of WebKit's main run loop with the time and the main thread's own
// CPU clock. Two helper threads:
//
// - the monitor wakes once a second. When the main thread has not turned for
//   the threshold, it writes the "stalled" line at once, before anything
//   that could block, and asks the reporter for stacks. Every ~10 s while
//   the stall lasts: "still stalled" (and the main thread's stack again).
//   Then a line when it turns. It also watches the reporter: a report step
//   that takes more than 3 s (a signal that is not delivered, a thread name
//   behind a libthr lock) is named in a line of its own.
// - the reporter signals each thread (SIGVTALRM, unused by WebKit, JSC, GLib
//   and the profiler's SIGPROF); the handler unwinds the interrupted stack
//   with the profiler's unwinder (profiler.c, ps5unwind.c) into a static
//   buffer. The main thread first, then every other thread, once per stall.
//
//   [watchdog web3] main thread stalled: no run loop turn for 3.2 s (pid 812); stacks follow, also in /data/ps5webkit/prof/watchdog-web3-812.txt
//   [watchdog web3] main thread: its CPU +3.1 s since its last turn (busy)
//   [watchdog web3] bt main t17 cpu 41234 ms: 1a2b3c 1a2b00 j2 j1 x8001234 ...
//   [watchdog web3] bt t3 JSC_Heap_Collector cpu 812 ms: ...
//   [watchdog web3] bt t9 WebCore_Scrolling cpu 2 ms: same as t3
//   [watchdog web3] 16 other threads (9 distinct stacks, 1 silent)
//   [watchdog web3] main thread still stalled after 13.2 s
//   [watchdog web3] bt main t17 cpu 51210 ms: ...
//   [watchdog web3] main thread recovered: stalled 16.4 s, its CPU +16.2 s (busy)
//
// Nothing on the report path takes a lock of the C library or allocates:
// lines are formatted here (no snprintf), stacks go into static buffers
// that are touched at start (as are the helpers' stacks: no page fault
// needs the VM map while the process is stuck), and output is written with
// system calls. stderr is the elfldr connection to the title (a socket): if
// it does not take a line within ~0.5 s (the title not reading, the socket
// full: every thread that logs would be stuck there), the line is dropped
// from stderr and the rest goes only to the file. Every stall is also
// appended to <PS5WK_PROFILE_DIR or /data/ps5webkit/prof>/watchdog-<tag>-<pid>.txt.
// Touching <that dir>/watchdog-dump (e.g. an FTP upload) makes every
// running child write the stacks of all its threads once, stalled or not:
// for a process whose run loop still turns but does no work.
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
// thread, one wake-up and one stat() a second of the monitor; the reporter
// sleeps in read().
#include "watchdog.h"
#include "profiler.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
#define STEP_WARN_MS 3000
#define STDERR_WAIT_MS 500
#define MAX_THREADS 512
#define OTHER_FRAMES 48
#define HELPER_STACK (512 * 1024)
#define TOUCHED_STACK (64 * 1024)

enum { IDLE, REQUESTED, WRITING, DONE };

static char tag[32];
static int64_t thresholdMS;
static pthread_t mainThread;
static _Atomic int running;
static _Atomic int64_t beatMS; // 0 until the run loop turns the first time
static _Atomic int64_t beatCPUNS;

// One request at a time (the reporter thread is the only requester).
static _Atomic int replyState;
static _Atomic uintptr_t replyTarget;
static uintptr_t replyPC[PS5WK_BACKTRACE_MAX];
static unsigned replyN;
static int64_t replyCPUNS;

// -------------------------------------------------------------- output

static char filePath[192];
static char dumpPath[192];
static _Atomic int fileFD = -1;
static _Atomic int stderrState; // 0 takes output, 1 stopped taking it, 2 not a socket (plain write)

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

static void pauseMS(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };
    while (nanosleep(&ts, &ts) && errno == EINTR) { }
}

// A line under construction: no C library, no allocation.
typedef struct {
    char* p;
    size_t n, cap;
} line_t;

static void put(line_t* l, const char* s)
{
    while (s && *s && l->n + 2 < l->cap)
        l->p[l->n++] = *s++;
}

static void putUnsigned(line_t* l, uint64_t v)
{
    char d[24];
    unsigned n = 0;
    do {
        d[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n && l->n + 2 < l->cap)
        l->p[l->n++] = d[--n];
}

static void putInt(line_t* l, int64_t v)
{
    if (v < 0) {
        put(l, "-");
        putUnsigned(l, (uint64_t)(-(v + 1)) + 1);
    } else
        putUnsigned(l, (uint64_t)v);
}

// "12.3"
static void putSeconds(line_t* l, int64_t ms)
{
    if (ms < 0)
        ms = 0;
    putUnsigned(l, (uint64_t)(ms / 1000));
    put(l, ".");
    putUnsigned(l, (uint64_t)(ms % 1000 / 100));
}

static void begin(line_t* l, char* buffer, size_t size)
{
    l->p = buffer;
    l->n = 0;
    l->cap = size;
    put(l, "[watchdog ");
    put(l, tag);
    put(l, "] ");
}

static void writeFile(const char* s, size_t n)
{
    int fd = atomic_load(&fileFD);
    if (fd >= 0) {
        ssize_t ignored = write(fd, s, n);
        (void)ignored;
    }
}

static void noteStderrStopped(void)
{
    char buffer[256];
    line_t note;
    begin(&note, buffer, sizeof buffer);
    put(&note, "stderr (the log socket to the title) is not taking output: the title is not reading it; lines go to this file only until it does");
    note.p[note.n++] = '\n';
    writeFile(note.p, note.n);
}

// The line to the file and to stderr. Never waits more than ~STDERR_WAIT_MS
// for stderr (once it stopped taking output: not at all). Pieces of at most
// STDERR_PIECE bytes, each sent only when poll() says the socket has room
// (its send low-water mark, 2 KB): no send can block even if MSG_DONTWAIT
// were not honoured.
#define STDERR_PIECE 1024
static void finish(line_t* l)
{
    l->p[l->n++] = '\n';
    const char* s = l->p;
    size_t n = l->n;
    writeFile(s, n);
    int state = atomic_load(&stderrState);
    if (state == 2) {
        ssize_t ignored = write(2, s, n);
        (void)ignored;
        return;
    }
    size_t sent = 0;
    int waited = 0, maxWait = state == 1 ? 0 : STDERR_WAIT_MS;
    while (sent < n) {
        struct pollfd p = { 2, POLLOUT, 0 };
        int ready = poll(&p, 1, 0);
        if (ready < 0 && errno == EINTR)
            continue;
        ssize_t w = -1;
        int error = EAGAIN;
        if (ready > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL)))
            error = EPIPE;
        else if (ready > 0) {
            size_t piece = n - sent < STDERR_PIECE ? n - sent : STDERR_PIECE;
            w = send(2, s + sent, piece, MSG_DONTWAIT);
            error = w < 0 ? errno : 0;
        }
        if (w > 0) {
            sent += (size_t)w;
            continue;
        }
        if (error == EINTR)
            continue;
        if (error == ENOTSOCK) {
            atomic_store(&stderrState, 2);
            ssize_t ignored = write(2, s + sent, n - sent);
            (void)ignored;
            return;
        }
        if ((error == EAGAIN || error == EWOULDBLOCK || error == ENOBUFS) && waited < maxWait) {
            pauseMS(20);
            waited += 20;
            continue;
        }
        if (atomic_exchange(&stderrState, 1) != 1)
            noteStderrStopped();
        return;
    }
    if (state == 1 && atomic_exchange(&stderrState, 0) == 1) {
        char buffer[128];
        line_t note;
        begin(&note, buffer, sizeof buffer);
        put(&note, "stderr takes output again");
        finish(&note);
    }
}

// The file of this process's reports, created at the first one.
static void openFile(void)
{
    if (atomic_load(&fileFD) >= 0 || !filePath[0])
        return;
    int fd = open(filePath, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;
    int expected = -1;
    if (!atomic_compare_exchange_strong(&fileFD, &expected, fd)) {
        close(fd);
        return;
    }
    if (atomic_load(&stderrState) == 1)
        noteStderrStopped();
}

// -------------------------------------------------------------- capture

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

// What the reporter is doing, for the monitor (a step that never ends is
// named in the log: that is where the report itself is stuck).
enum { STEP_IDLE, STEP_SIGNAL, STEP_REPLY, STEP_NAME, STEP_OUTPUT };
static const char* const stepNames[] = { "idle", "pthread_kill (libthr's thread list lock?)", "waiting for the signal handler (the thread does not run it)",
    "pthread_getname_np (libthr lock?)", "writing a line (stderr or the file)" };
static _Atomic int step;
static _Atomic int stepThread;
static _Atomic int64_t stepSinceMS;
static _Atomic int stepWarned;

static void setStep(int value, int thread)
{
    atomic_store(&stepThread, thread);
    atomic_store(&stepSinceMS, nowMS());
    atomic_store(&stepWarned, 0);
    atomic_store(&step, value);
}

// The thread's stack, from its own signal handler. 0, or -1 (no such
// thread) / -2 (no answer within REPLY_WAIT_MS: signals blocked, or it is
// stopped in the kernel).
static int capture(uintptr_t thread, int index, uintptr_t* pcs, unsigned* n, int64_t* cpuNS)
{
    atomic_store(&replyTarget, thread);
    atomic_store(&replyState, REQUESTED);
    setStep(STEP_SIGNAL, index);
    int killed = pthread_kill((pthread_t)thread, WATCHDOG_SIGNAL);
    setStep(STEP_REPLY, index);
    if (killed) {
        atomic_store(&replyState, IDLE);
        atomic_store(&replyTarget, 0);
        return -1;
    }
    for (int i = 0; i < REPLY_WAIT_MS && atomic_load_explicit(&replyState, memory_order_acquire) != DONE; i++)
        pauseMS(1);
    int expected = REQUESTED;
    if (atomic_compare_exchange_strong(&replyState, &expected, IDLE)) {
        atomic_store(&replyTarget, 0);
        return -2; // a late signal finds the state IDLE and does nothing
    }
    for (int i = 0; i < 2000 && atomic_load_explicit(&replyState, memory_order_acquire) != DONE; i++)
        pauseMS(1); // the handler is writing: it finishes in microseconds
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

static void putThreadName(line_t* l, uintptr_t key, int index)
{
    char name[32];
    name[0] = 0;
    setStep(STEP_NAME, index);
    if (pthread_getname_np)
        pthread_getname_np((pthread_t)key, name, sizeof name);
    setStep(STEP_OUTPUT, index);
    name[sizeof name - 1] = 0;
    if (!name[0])
        name[0] = '-', name[1] = 0;
    for (char* c = name; *c; c++)
        if (*c == ' ' || *c == '\n')
            *c = '_';
    put(l, name);
}

// "its CPU +3.1 s<middle> (busy)": the main thread's CPU since its last turn.
static void putCPU(line_t* l, int64_t cpuNS, int64_t lastBeatCPU, int64_t wallMS, const char* middle)
{
    if (cpuNS < 0 || lastBeatCPU < 0) {
        put(l, "its CPU unknown");
        put(l, middle);
        return;
    }
    int64_t usedMS = (cpuNS - lastBeatCPU) / 1000000;
    put(l, "its CPU +");
    putSeconds(l, usedMS);
    put(l, " s");
    put(l, middle);
    put(l, usedMS * 2 > wallMS ? " (busy)" : " (waiting)");
}

static char reportLine[4096];
static char frames[3072];
static uintptr_t mainPCs[PS5WK_BACKTRACE_MAX];
static uintptr_t otherPCs[PS5WK_BACKTRACE_MAX];
static uintptr_t keys[MAX_THREADS];
static unsigned indices[MAX_THREADS];
static uint64_t hashes[MAX_THREADS];

// The main thread's stack. stallBeat/stallBeatCPU: the last turn (0: not stalled).
static void reportMain(int64_t stallBeat, int64_t stallBeatCPU)
{
    unsigned n = 0;
    int64_t cpuNS = -1;
    int index = ps5wk_profiler_thread_index((uintptr_t)mainThread);
    ps5wk_backtrace_refresh_jit();
    int rc = capture((uintptr_t)mainThread, index, mainPCs, &n, &cpuNS);
    setStep(STEP_OUTPUT, index);
    line_t l;
    if (stallBeat) {
        begin(&l, reportLine, sizeof reportLine);
        put(&l, "main thread: ");
        putCPU(&l, rc ? -1 : cpuNS, stallBeatCPU, nowMS() - stallBeat, " since its last turn");
        finish(&l);
    }
    begin(&l, reportLine, sizeof reportLine);
    put(&l, "bt main t");
    putInt(&l, index);
    if (rc) {
        put(&l, rc == -1 ? ": thread gone" : ": no answer to the signal");
        finish(&l);
        return;
    }
    put(&l, " cpu ");
    putInt(&l, cpuNS / 1000000);
    put(&l, " ms:");
    ps5wk_backtrace_format(frames, sizeof frames, mainPCs, n);
    put(&l, frames);
    finish(&l);
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
    unsigned count = ps5wk_profiler_threads(keys, indices, MAX_THREADS);
    unsigned others = 0, printed = 0, silent = 0;
    line_t l;
    for (unsigned i = 0; i < count; i++) {
        hashes[i] = 0;
        if (keys[i] == (uintptr_t)mainThread)
            continue;
        unsigned n = 0;
        int64_t cpuNS = -1;
        int rc = capture(keys[i], (int)indices[i], otherPCs, &n, &cpuNS);
        if (rc == -1)
            continue;
        others++;
        begin(&l, reportLine, sizeof reportLine);
        put(&l, "bt t");
        putUnsigned(&l, indices[i]);
        put(&l, " ");
        putThreadName(&l, keys[i], (int)indices[i]);
        if (rc) {
            put(&l, ": no answer to the signal");
            finish(&l);
            silent++;
            continue;
        }
        if (n > OTHER_FRAMES)
            n = OTHER_FRAMES;
        hashes[i] = framesHash(otherPCs, n);
        unsigned same = i;
        for (unsigned k = 0; k < i; k++)
            if (hashes[k] == hashes[i]) {
                same = k;
                break;
            }
        put(&l, " cpu ");
        putInt(&l, cpuNS / 1000000);
        if (same != i) {
            put(&l, " ms: same as t");
            putUnsigned(&l, indices[same]);
            finish(&l);
            continue;
        }
        put(&l, " ms:");
        ps5wk_backtrace_format(frames, sizeof frames, otherPCs, n);
        put(&l, frames);
        finish(&l);
        printed++;
    }
    begin(&l, reportLine, sizeof reportLine);
    putUnsigned(&l, others);
    put(&l, " other threads (");
    putUnsigned(&l, printed);
    put(&l, " distinct stacks, ");
    putUnsigned(&l, silent);
    put(&l, " silent)");
    finish(&l);
}

// ------------------------------------------------------------- threads

// Requests from the monitor to the reporter, one byte each on a socket pair.
enum { REQUEST_FIRST = 'F', REQUEST_MAIN = 'M', REQUEST_DUMP = 'D' };
static int requestFD[2] = { -1, -1 };
static _Atomic int64_t requestBeat, requestBeatCPU;

static void request(char what, int64_t stallBeat, int64_t stallBeatCPU)
{
    atomic_store(&requestBeat, stallBeat);
    atomic_store(&requestBeatCPU, stallBeatCPU);
    ssize_t ignored = send(requestFD[0], &what, 1, MSG_DONTWAIT);
    (void)ignored;
}

// Faults in the first TOUCHED_STACK bytes of the calling thread's stack now.
static void __attribute__((noinline)) touchStack(void)
{
    volatile char pad[TOUCHED_STACK];
    for (size_t i = 0; i < sizeof pad; i += 1024)
        pad[i] = 0;
    pad[sizeof pad - 1] = 0;
}

static void* reporterThread(void* data)
{
    (void)data;
    ps5wk_profiler_own_thread();
    touchStack();
    for (;;) {
        char what = 0;
        ssize_t got = read(requestFD[1], &what, 1);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0 || !atomic_load(&running))
            return NULL;
        int64_t stallBeat = atomic_load(&requestBeat), stallBeatCPU = atomic_load(&requestBeatCPU);
        reportMain(what == REQUEST_DUMP ? 0 : stallBeat, stallBeatCPU);
        if (what != REQUEST_MAIN)
            reportOthers();
        setStep(STEP_IDLE, -1);
    }
}

static void checkReporter(int64_t now)
{
    int current = atomic_load(&step);
    if (current == STEP_IDLE || atomic_load(&stepWarned))
        return;
    int64_t since = atomic_load(&stepSinceMS);
    if (now - since < STEP_WARN_MS)
        return;
    atomic_store(&stepWarned, 1);
    char buffer[384];
    line_t l;
    begin(&l, buffer, sizeof buffer);
    put(&l, "report held up for ");
    putSeconds(&l, now - since);
    put(&l, " s in ");
    put(&l, stepNames[current]);
    put(&l, ", thread t");
    putInt(&l, atomic_load(&stepThread));
    finish(&l);
}

// watchdog-dump touched since the last look: a report of every thread.
static int64_t dumpSeen = -1;
static int dumpAsked(void)
{
    struct stat st;
    int64_t stamp = stat(dumpPath, &st) ? 0 : (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
    if (dumpSeen < 0) {
        dumpSeen = stamp;
        return 0;
    }
    if (stamp == dumpSeen)
        return 0;
    dumpSeen = stamp;
    return stamp != 0;
}

static void* monitorThread(void* data)
{
    (void)data;
    ps5wk_profiler_own_thread();
    touchStack();
    char buffer[512];
    line_t l;
    int64_t lastCheck = nowMS(), ignoreBefore = 0;
    int stalled = 0;
    int64_t stallBeat = 0, stallBeatCPU = -1, nextReport = 0;
    dumpAsked();
    for (;;) {
        pauseMS(1000); // signals (the profiler's SIGPROF) cut sleeps short: resumed
        if (!atomic_load(&running))
            return NULL;
        int64_t now = nowMS();
        // We did not run for seconds ourselves: the whole process was
        // stopped (rest mode, a debugger). Not a stall of the main thread.
        if (now - lastCheck > 3000)
            ignoreBefore = now;
        lastCheck = now;
        checkReporter(now);
        if (dumpAsked()) {
            openFile();
            begin(&l, buffer, sizeof buffer);
            put(&l, "stacks of every thread asked for (");
            put(&l, dumpPath);
            put(&l, "), pid ");
            putInt(&l, getpid());
            put(&l, ": they follow, also in ");
            put(&l, filePath);
            finish(&l);
            request(REQUEST_DUMP, 0, -1);
        }
        int64_t beatAt = atomic_load_explicit(&beatMS, memory_order_acquire);
        if (!beatAt)
            continue; // the run loop has not started yet
        if (stalled) {
            if (beatAt != stallBeat) {
                int64_t afterCPU = atomic_load_explicit(&beatCPUNS, memory_order_relaxed);
                begin(&l, buffer, sizeof buffer);
                put(&l, "main thread recovered: stalled ");
                putSeconds(&l, beatAt - stallBeat);
                put(&l, " s, ");
                putCPU(&l, afterCPU, stallBeatCPU, beatAt - stallBeat, "");
                finish(&l);
                stalled = 0;
                continue;
            }
            if (now >= nextReport) {
                begin(&l, buffer, sizeof buffer);
                put(&l, "main thread still stalled after ");
                putSeconds(&l, now - stallBeat);
                put(&l, " s");
                finish(&l);
                if (atomic_load(&step) == STEP_IDLE)
                    request(REQUEST_MAIN, stallBeat, stallBeatCPU);
                nextReport += REPEAT_MS;
            }
            continue;
        }
        int64_t since = now - (beatAt > ignoreBefore ? beatAt : ignoreBefore);
        if (since >= thresholdMS) {
            stalled = 1;
            stallBeat = beatAt;
            stallBeatCPU = atomic_load_explicit(&beatCPUNS, memory_order_relaxed);
            openFile();
            // First, before anything that could block.
            begin(&l, buffer, sizeof buffer);
            put(&l, "main thread stalled: no run loop turn for ");
            putSeconds(&l, now - beatAt);
            put(&l, " s (pid ");
            putInt(&l, getpid());
            put(&l, "); stacks follow, also in ");
            put(&l, filePath);
            finish(&l);
            request(REQUEST_FIRST, stallBeat, stallBeatCPU);
            nextReport = now + REPEAT_MS;
        }
    }
}

static int startHelper(void* (*function)(void*))
{
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, HELPER_STACK);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    int rc = pthread_create(&thread, &attributes, function, NULL);
    pthread_attr_destroy(&attributes);
    return rc;
}

static void startLine(const char* text)
{
    char buffer[512];
    line_t l;
    begin(&l, buffer, sizeof buffer);
    put(&l, text);
    finish(&l);
}

void ps5wk_watchdog_start(const char* processTag)
{
    const char* knob = getenv("PS5WK_WATCHDOG_S");
    int s = knob && *knob ? atoi(knob) : 3;
    snprintf(tag, sizeof tag, "%s", processTag ? processTag : "?");
    if (s <= 0) {
        startLine("off (PS5WK_WATCHDOG_S=0)");
        return;
    }
    thresholdMS = (int64_t)s * 1000;
    mainThread = pthread_self();
    const char* dir = getenv("PS5WK_PROFILE_DIR");
    if (!dir || !*dir)
        dir = "/data/ps5webkit/prof";
    mkdir(dir, 0777);
    snprintf(filePath, sizeof filePath, "%s/watchdog-%s-%d.txt", dir, tag, (int)getpid());
    snprintf(dumpPath, sizeof dumpPath, "%s/watchdog-dump", dir);
    // The report path's buffers, faulted in now.
    memset(reportLine, 0, sizeof reportLine);
    memset(frames, 0, sizeof frames);
    memset(keys, 0, sizeof keys);
    memset(indices, 0, sizeof indices);
    memset(hashes, 0, sizeof hashes);
    memset(mainPCs, 0, sizeof mainPCs);
    memset(otherPCs, 0, sizeof otherPCs);
    memset(replyPC, 0, sizeof replyPC);
    ps5wk_profiler_register_self();
    int unwinding = ps5wk_backtrace_init() == 0;
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = onSignal;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(WATCHDOG_SIGNAL, &action, NULL)) {
        startLine("sigaction failed: off");
        return;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, requestFD)) {
        startLine("no socket pair: off");
        return;
    }
    fcntl(requestFD[0], F_SETFD, FD_CLOEXEC);
    fcntl(requestFD[1], F_SETFD, FD_CLOEXEC);
    // On the default main context: WebKit's main RunLoop (RunLoopGLib) runs it.
    g_timeout_add_full(PS5WK_G_PRIORITY_HIGH, BEAT_MS, beat, NULL, NULL);
    atomic_store(&running, 1);
    if (startHelper(reporterThread) || startHelper(monitorThread)) {
        atomic_store(&running, 0);
        startLine("no thread: off");
        return;
    }
    char text[480];
    snprintf(text, sizeof text, "on: reports a main thread that does not turn for %d s (PS5WK_WATCHDOG_S; %s unwinding); reports also in %s, "
        "touch %s for the stacks of every thread", s, unwinding ? "CFI" : "no", filePath, dumpPath);
    startLine(text);
}

void ps5wk_watchdog_stop(void)
{
    atomic_store(&running, 0);
}
