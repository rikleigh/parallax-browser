/* jsctitle.c - Spike 13 stage 1: the jsc shell as a registered title, with
 * the JIT on (README.md).
 *
 * The WPE build's jsc objects (jsc.cpp's jscShellMain, JSC, WTF, libpas)
 * are relinked as a title by build-jsc-title.sh, with the browser title's
 * memory layer (titlemem.c + Spike 10's dheap.c: heap, anonymous mmaps and
 * thread stacks in direct memory). This file is the title's main:
 *
 * - argv from /data/jsc/args.txt (Spike 4's layout; one run per launch:
 *   JSC's options are fixed once per process), e.g.
 *     --useWasmFaultSignalHandler=0 /data/jsc/wasm-big.js
 * - stdout and stderr go through a pipe to spike_log, so jsc's output
 *   reaches tools/logserver.sh and /data/ps5webkit/spike13-jsc.log;
 * - the WebProcess's JSC settings (ps5wk-child.cpp configureJIT): JIT on,
 *   one compiler thread per tier, a 256 MB executable pool, WebAssembly on.
 *   JSC_* variables in /data/jsc/env.txt (NAME=value per line) win;
 * - JIT memory: a title can't run code from a mapping made executable at
 *   map time, but it can once the mapping is made RWX with mprotect (stage
 *   0). JSC's pool is one anonymous RWX mmap (OSAllocatorPOSIX), so
 *   __wrap_mmap here maps executable requests RW through titlemem.c
 *   (direct memory; titlemem.c's own __wrap_mmap is renamed
 *   titlemem_mmap at compile time) and then sceKernelMprotect's them to
 *   the protection asked for.
 *
 * The title ends when jscShellMain returns (title_exit.c).
 */
#define SPIKE_NAME "13-title-jit"
#include "common/title.h"
#include "dmem.h"
#include "jscenv.h"
#include "jscside.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pthread_np.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/ucontext.h>
#include <signal.h>

int jscShellMain(int argc, char** argv) __asm__("_Z12jscShellMainiPPc"); /* C++ (jsc.cpp) */
void* titlemem_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off);
int main(void);
extern const char __ehdr_start[];
int pthread_getname_np(pthread_t, char*, size_t) __attribute__((weak));

#ifndef TITLE_ID
#define TITLE_ID "PPSA99017"
#endif
#define ARGS_FILE "/data/jsc/args.txt"
#define STACK_SIZE ((size_t)16 << 20)
#define PAGE ((size_t)DMEM_PAGE)

/* ---- executable mappings ------------------------------------------------ */

static _Atomic unsigned long execMaps, execFailures;

void* __wrap_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off)
{
    const int anon = (flags & MAP_ANON) && fd == -1 && !(flags & MAP_SHARED);
    if (!(prot & PROT_EXEC) || !anon || (flags & MAP_FIXED))
        return titlemem_mmap(addr, len, prot, flags, fd, off);
    void* p = titlemem_mmap(addr, len, prot & ~PROT_EXEC, flags, fd, off);
    if (p == MAP_FAILED)
        return p;
    const size_t size = (len + PAGE - 1) & ~(PAGE - 1);
    int kprot = ((prot & PROT_READ) ? 1 : 0) | ((prot & PROT_WRITE) ? 2 : 0) | 4;
    int rc = sceKernelMprotect(p, size, kprot);
    if (rc) {
        atomic_fetch_add(&execFailures, 1);
        spike_log("[jsc-title] executable mmap of %.1f MB at %p: sceKernelMprotect(0x%x) rc 0x%08x", size / 1048576.0, p,
            kprot, (unsigned)rc);
        munmap(p, len);
        errno = ENOMEM;
        return MAP_FAILED;
    }
    atomic_fetch_add(&execMaps, 1);
    spike_log("[jsc-title] executable mmap of %.1f MB (prot %d) -> %p, made 0x%x with sceKernelMprotect", size / 1048576.0,
        prot, p, kprot);
    return p;
}

/* ---- stdout/stderr -> spike_log ---------------------------------------- */

static void* outputThread(void* arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[4096];
    size_t have = 0;
    for (;;) {
        ssize_t n = read(fd, buf + have, sizeof buf - 1 - have);
        if (n <= 0)
            break;
        have += (size_t)n;
        size_t start = 0;
        for (size_t i = 0; i < have; i++) {
            if (buf[i] == '\n') {
                buf[i] = 0;
                spike_log("%s", buf + start);
                start = i + 1;
            }
        }
        if (start == 0 && have == sizeof buf - 1) { /* a long line: send what there is */
            buf[have] = 0;
            spike_log("%s", buf);
            have = 0;
        } else {
            memmove(buf, buf + start, have - start);
            have -= start;
        }
    }
    if (have) {
        buf[have] = 0;
        spike_log("%s", buf);
    }
    return NULL;
}

static int g_outWrite = -1;
static pthread_mutex_t g_sinkLock = PTHREAD_MUTEX_INITIALIZER;

/* spike_log writes to stdout too, which is the pipe from here on: this sink
 * writes to the log file and the log server only. */
static void logSink(const char* line, size_t n)
{
    pthread_mutex_lock(&g_sinkLock);
    if (spike_log_file_fd >= 0 && write(spike_log_file_fd, line, n) < 0) {
        close(spike_log_file_fd);
        spike_log_file_fd = -1;
    }
    if (spike_log_fd >= 0 && send(spike_log_fd, line, n, 0) < 0) {
        close(spike_log_fd);
        spike_log_fd = -1;
    }
    pthread_mutex_unlock(&g_sinkLock);
}

static void redirectOutput(void)
{
    spike_log_sink = logSink;
    int fds[2];
    if (pipe(fds)) {
        spike_log("[jsc-title] pipe errno %d: jsc output is lost", errno);
        return;
    }
    pthread_t t;
    pthread_create(&t, NULL, outputThread, (void*)(intptr_t)fds[0]);
    pthread_detach(t);
    fflush(stdout);
    fflush(stderr);
    /* dup2 onto 1 and 2 is EPERM in a title (and libc's stdout is fd 11):
     * point libc's streams at the pipe instead, as titlelog.c does. */
    g_outWrite = fds[1];
    FILE* out = fdopen(fds[1], "w");
    FILE* err = fdopen(dup(fds[1]), "w");
    if (out) {
        setvbuf(out, NULL, _IOLBF, 0);
        stdout = out;
    }
    if (err) {
        setvbuf(err, NULL, _IONBF, 0);
        stderr = err;
    }
    spike_log("[jsc-title] stdout %s, stderr %s: into the log", out ? "redirected" : "NOT redirected",
        err ? "redirected" : "NOT redirected");
    printf("[jsc-title] stdout reaches the log\n");
    fprintf(stderr, "[jsc-title] stderr reaches the log\n");
    fflush(stdout);
}

static void flushOutput(void)
{
    fflush(stdout);
    fflush(stderr);
    /* Let the reader drain the pipe. */
    usleep(300000);
    spike_log_flush(2000);
}

/* ---- argv and environment ---------------------------------------------- */

static char* readFile(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f)
        return NULL;
    size_t cap = 1 << 14, len = 0;
    char* s = malloc(cap);
    for (;;) {
        if (len + 4096 + 1 > cap)
            s = realloc(s, cap *= 2);
        size_t n = fread(s + len, 1, cap - len - 1, f);
        if (!n)
            break;
        len += n;
    }
    fclose(f);
    s[len] = 0;
    return s;
}

static void reportEnv(const char* name, const char* value)
{
    spike_log("[jsc-title] env %s=%s", name, value);
}

static void loadEnv(void)
{
    jscLoadEnv(reportEnv); /* jscenv.h: the same settings as the payload jsc */
}

/* ---- exit -----------------------------------------------------------------
 * jsc ends with exit() (jscExit -> WTF::exitProcess), which a title may not
 * call (SIGSYS, code SI_KERNEL, from libkernel). build-jsc-title.sh links
 * with --wrap=exit: on the shell thread, exit ends that thread with the
 * status, and main reports it and returns (title_exit.c ends the title). */
static pthread_t g_shellThread;
static _Atomic int g_exitStatus = -1000;
void __real_exit(int status) __attribute__((noreturn));

void __wrap_exit(int status)
{
    if (g_shellThread && pthread_equal(pthread_self(), g_shellThread)) {
        atomic_store(&g_exitStatus, status);
        fflush(stdout);
        fflush(stderr);
        pthread_exit(NULL);
    }
    spike_log("[jsc-title] exit(%d) from another thread: the title ends here", status);
    fflush(stdout);
    fflush(stderr);
    usleep(300000);
    spike_log_flush(1000);
    __real_exit(status);
}

/* ---- the shell thread --------------------------------------------------- */

typedef struct {
    int argc;
    char** argv;
    int result;
} shell_t;

static uint64_t threadMask(void)
{
    uint64_t m[2] = {0, 0};
    cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, (cpuset_t*)m);
    return m[0];
}

static void* shellThread(void* p)
{
    shell_t* s = p;
    pthread_set_name_np(pthread_self(), "jsc");
    /* /data/jsc/cpu.txt: a CPU number to pin the shell's thread to. */
    char* cpu = readFile("/data/jsc/cpu.txt");
    if (cpu && *cpu >= '0' && *cpu <= '9') {
        int c = atoi(cpu);
        uint64_t m[2] = {1ull << c, 0};
        int rc = cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, (cpuset_t*)m) ? errno : 0;
        spike_log("[jsc-title] shell thread pinned to CPU %d (errno %d)", c, rc);
    }
    free(cpu);
    spike_log("[jsc-title] shell thread mask %#llx", (unsigned long long)threadMask());
    g_shellThread = pthread_self();
    s->result = jscShellMain(s->argc, s->argv);
    atomic_store(&g_exitStatus, s->result);
    return NULL;
}

/* ---- load: busy threads on the title's CPUs ----------------------------
 * /data/jsc/load.txt "<threads> <seconds>": that many threads run an
 * integer loop (no memory traffic) for that long, or until jsc ends. A
 * stand-in for a game keeping the title's CPUs busy, for the payload runs. */
static _Atomic int g_loadRun;
static _Atomic unsigned long long g_loadLoops;

/* Each load thread is pinned to one of CPUs 1-12, leaving CPU 0 to main,
 * the output thread and jsc: the title scheduler does not share a CPU
 * between busy threads (13 unpinned load threads on CPUs 0-12 stopped main
 * and the log for good). */
static void* loadThread(void* arg)
{
    int c = 1 + (int)(intptr_t)arg % 12;
    uint64_t m[2] = {1ull << c, 0};
    cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, (cpuset_t*)m);
    pthread_set_name_np(pthread_self(), "load");
    uint64_t x = 88172645463325252ull;
    while (atomic_load_explicit(&g_loadRun, memory_order_relaxed)) {
        for (int i = 0; i < 1000000; i++) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            __asm__ volatile("" : "+r"(x));
        }
        atomic_fetch_add_explicit(&g_loadLoops, 1, memory_order_relaxed);
    }
    return NULL;
}

static int g_loadThreads;
static double g_loadSeconds;
static pthread_t g_loadTids[64];

static void startLoad(void)
{
    char* text = readFile("/data/jsc/load.txt");
    if (!text)
        return;
    g_loadThreads = atoi(text);
    char* sp = strchr(text, ' ');
    g_loadSeconds = sp ? atof(sp + 1) : 0; /* "<threads>" alone: only while jsc runs */
    free(text);
    if (g_loadThreads <= 0) {
        g_loadThreads = 0;
        return;
    }
    if (g_loadThreads > 64)
        g_loadThreads = 64;
    atomic_store(&g_loadRun, 1);
    for (int i = 0; i < g_loadThreads; i++)
        pthread_create(&g_loadTids[i], NULL, loadThread, (void*)(intptr_t)i);
    spike_log("[jsc-title] load: %d busy threads (CPUs 1-%d) for up to %.0f s", g_loadThreads, g_loadThreads < 12 ? g_loadThreads : 12, g_loadSeconds);
}

static void stopLoad(double ranMs)
{
    if (g_loadThreads <= 0)
        return;
    atomic_store(&g_loadRun, 0);
    for (int i = 0; i < g_loadThreads; i++)
        pthread_join(g_loadTids[i], NULL);
    spike_log("[jsc-title] load: stopped after %.0f ms, %.1f M loops per thread-second", ranMs,
        atomic_load(&g_loadLoops) / (ranMs / 1000.0) / g_loadThreads);
    g_loadThreads = 0;
}

/* ---- crash report --------------------------------------------------------
 * The title has no kernel log stream here: on a fatal signal, log the
 * signal, the address, rip/rsp and rip relative to main (symbolize with
 * llvm-addr2line on build/jsc-link/llvm-pie.elf: <main's address in the
 * ELF> + delta), then die as usual. JSC's own handlers chain to this one. */
static void onCrash(int sig, siginfo_t* info, void* ctx)
{
    /* The PS5 kernel's ucontext is not the SDK header's (crashguard.c,
     * test/sigctx on 13.60): rip +0xe0, rsp +0xf8, rbp +0x88. */
    const uintptr_t* words = ctx;
    uintptr_t rip = words ? words[0xe0 / 8] : 0, rsp = words ? words[0xf8 / 8] : 0;
    uintptr_t rbp = words ? words[0x88 / 8] : 0;
    char name[32] = "?";
    if (pthread_getname_np)
        pthread_getname_np(pthread_self(), name, sizeof name);
    spike_log("[jsc-title] CRASH signal %d code %d addr %p in thread %s: rip %#lx (elf +%#lx) rsp %#lx rbp %#lx", sig,
        info->si_code, info->si_addr, name, (unsigned long)rip, (unsigned long)(rip - (uintptr_t)&__ehdr_start), (unsigned long)rsp,
        (unsigned long)rbp);
    /* Frame-pointer walk, as far as it looks sane. */
    for (int i = 0; i < 12 && rbp && !(rbp & 7) && rbp > rsp && rbp - rsp < ((size_t)64 << 20); i++) {
        uintptr_t ret = ((uintptr_t*)rbp)[1];
        spike_log("[jsc-title]   frame %d: %#lx (elf +%#lx)", i, (unsigned long)ret, (unsigned long)(ret - (uintptr_t)&__ehdr_start));
        uintptr_t next = ((uintptr_t*)rbp)[0];
        if (next <= rbp)
            break;
        rbp = next;
    }
    spike_log_flush(1000);
    usleep(300000);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void installCrashHandler(void)
{
    static char altstack[64 * 1024];
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = onCrash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGSYS};
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        sigaction(sigs[i], &sa, NULL);
}

static void sideReport(const char* line)
{
    spike_log("[jsc-title] %s", line);
}

/* Initial APIC ID of each CPU in the main thread's mask (CPUID leaf 1):
 * CPUs whose APIC ID / 2 is the same share a core (smtprobe.c). */
static void logApicMap(void)
{
    uint64_t mask = threadMask();
    char line[400];
    size_t o = 0;
    for (int c = 0; c < 16; c++) {
        if (!(mask & (1ull << c)) || jscPinSelf(c))
            continue;
        usleep(1000);
        unsigned a, b, cx, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(cx), "=d"(d) : "a"(1), "c"(0));
        o += snprintf(line + o, sizeof line - o, " %d:%u", c, b >> 24);
    }
    uint64_t m[2] = {mask, 0};
    cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, (cpuset_t*)m);
    spike_log("[jsc-title] cpu:apic%s", line);
}

static double flexMB(void)
{
    size_t s = 0;
    sceKernelAvailableFlexibleMemorySize(&s);
    return s / 1048576.0;
}

int main(void)
{
    spike_title_init("spike13-jsc", LOG_HOST, LOG_PORT);
    spike_log("ps5webkit spike 13 stage 1 (title): jsc with the JIT, pid %d, flexible %.1f MB, main mask %#llx", (int)getpid(),
        flexMB(), (unsigned long long)threadMask());
    redirectOutput();
    installCrashHandler();
    spike_log("[jsc-title] main at %p, title %s", (void*)main, TITLE_ID);
    logApicMap();
    loadEnv();

    static char* argv[256];
    int argc = 0;
    argv[argc++] = "jsc";
    char* text = readFile(ARGS_FILE);
    double l0 = spike_now_ms();
    startLoad();
    if (!text || !strncmp(text, "none", 4)) {
        if (g_loadThreads > 0 || g_loadSeconds > 0) {
            /* A load title (or, with 0 threads, an idle one) for g_loadSeconds,
             * with a beat every 5 s: the load threads' rate shows whether this
             * title still gets CPU time (Spike 14 0c, two titles at once). */
            double beat = l0;
            unsigned long long lastLoops = 0;
            spike_log("[jsc-title %s] staying %.0f s with %d load threads", TITLE_ID, g_loadSeconds, g_loadThreads);
            while (spike_now_ms() - l0 < g_loadSeconds * 1000) {
                usleep(100000);
                double now = spike_now_ms();
                if (now - beat >= 5000) {
                    unsigned long long loops = atomic_load(&g_loadLoops);
                    spike_log("[jsc-title %s] beat t=%.0f s: %.1f M loops/s per load thread", TITLE_ID, (now - l0) / 1000,
                        g_loadThreads ? (loops - lastLoops) / ((now - beat) / 1000.0) / g_loadThreads : 0.0);
                    lastLoops = loops;
                    beat = now;
                }
            }
            stopLoad(spike_now_ms() - l0);
        } else
            spike_log("[jsc-title] no %s and no load: nothing to run", ARGS_FILE);
        spike_log("[jsc-title] done");
        flushOutput();
        return 0;
    }
    char* save = NULL;
    for (char* tok = strtok_r(text, " \t\r\n", &save); tok && argc < 255; tok = strtok_r(NULL, " \t\r\n", &save))
        argv[argc++] = tok;
    argv[argc] = NULL;
    char line[900];
    size_t o = 0;
    for (int i = 1; i < argc && o < sizeof line - 2; i++)
        o += snprintf(line + o, sizeof line - o, "%s ", argv[i]);
    spike_log("[jsc-title] jsc %s", line);

    char describe[256];
    if (jscSideStart("/data/jsc/side.txt", describe, sizeof describe))
        spike_log("[jsc-title] side threads: %s", describe);

    shell_t s = {argc, argv, 1};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, STACK_SIZE);
    pthread_t t;
    double t0 = spike_now_ms();
    if (pthread_create(&t, &attr, shellThread, &s)) {
        spike_log("[jsc-title] pthread_create failed");
        flushOutput();
        return 0;
    }
    pthread_attr_destroy(&attr);
    pthread_join(t, NULL);
    jscSideStop(sideReport);
    stopLoad(spike_now_ms() - l0);
    fflush(stdout);
    fflush(stderr);
    usleep(200000);
    spike_log("[jsc-title] exit %d after %.0f ms; executable mmaps %lu, failed %lu; flexible %.1f MB", atomic_load(&g_exitStatus),
        spike_now_ms() - t0, atomic_load(&execMaps), atomic_load(&execFailures), flexMB());
    spike_log("[jsc-title] done");
    flushOutput();
    return 0;
}
