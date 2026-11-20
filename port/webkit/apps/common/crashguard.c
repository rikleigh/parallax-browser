/*
 * crashguard.c - see crashguard.h.
 *
 * The report, one write() per line, async-signal-safe formatting only:
 *
 *   [crash <tag> pid <pid>] signal 11 (SIGSEGV) code 1 addr 0x... pc 0x... (elf +0x...) sp 0x... thread 0x...
 *   [crash <tag> pid <pid>] stack: +0x... +0x... ...      (stack words that follow a call instruction)
 *
 * "elf +0x..." are addresses in the unstripped ELF (lld puts __ehdr_start at
 * the image base): llvm-addr2line -f -C -e toolchain/wpe-webkit/build/bin/ps5wk-child 0x...
 * The stack line is a scan, not an unwind (no frame pointers at -O2): it
 * lists return-address candidates, innermost first, some of them stale.
 *
 * SA_NODEFER: a fault inside the handler re-enters it and exits at once
 * (_exit(71)) rather than being fatal with the signal blocked.
 */
#include "crashguard.h"

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

// The image's address range, from symbols the linker defines: __ehdr_start
// is the load base (vaddr 0; the ELF header itself is not mapped there by
// elfldr, so it must not be read), __init_array_start is in .data, past
// .text and .rodata. Do not parse the ELF header: the first payload built
// that way faulted in ps5wk_crash_guard_install, before the handlers existed.
extern char __ehdr_start;
extern char __init_array_start;
// The end of .text (lld defines it when referenced): the scan's bound when
// code can't be read. Weak: 0 where the linker doesn't.
extern char _etext __attribute__((weak));
static uintptr_t textStart, textEnd, codeEnd;
// Whether the bytes of .text can be read. A title's code is mapped
// execute-only: afterCall's look at the call instruction faulted (the
// 2026-11-20 crash test), so there the scan lists every stack word that
// points into .text, return addresses or not.
static int codeReadable;

// JSC's executable memory (WebKit patch 0030), so a pc in JIT code is named.
void ps5wk_jit_memory(void** start, void** end, size_t* allocated, size_t* committed) __attribute__((weak));

#define ALT_STACK_SIZE (128 * 1024)
#define SCAN_WORDS 4096
#define MAX_CANDIDATES 24

static char guardTag[32] = "?";
static int reportFd = -1; /* ps5wk_crash_guard_report_file */
static volatile sig_atomic_t inHandler;

// The title's log (titlelog.c), when this is linked into it. In the title fd 2
// is not the log: a title cannot dup2 over it, so titlelog points libc's stderr
// stream at its pipe instead, and write(2, ...) went nowhere. The report is
// kept here as written and handed over before _exit; the hook only try-locks
// and waits a bounded time.
int ps5wk_crash_log_report(const char* text, size_t n) __attribute__((weak));
static char reportText[2048];
static size_t reportLength, reportSent;
// The stack scan stops at the first page it cannot read instead of faulting
// there (2026-11-20: a fault in the scan killed the title outright, the
// nested handler never ran, and nothing but the first line was written).
// mincore() doesn't do: a guard page is mapped, with no access. A write()
// from the page into a pipe fails with EFAULT instead (probePipe, made by
// ps5wk_crash_guard_install, non-blocking; each probe's byte is read back).
static uintptr_t pageSize = 4096;
static int probePipe[2] = { -1, -1 };

typedef struct {
    char text[512];
    size_t length;
} line_t;

static void put(line_t* line, const char* s)
{
    while (*s && line->length + 1 < sizeof line->text)
        line->text[line->length++] = *s++;
}

static void putHex(line_t* line, uintptr_t value)
{
    char digits[2 + 16 + 1];
    int n = 0;
    char tmp[16];
    do {
        tmp[n++] = "0123456789abcdef"[value & 15];
        value >>= 4;
    } while (value && n < 16);
    int i = 0;
    digits[i++] = '0';
    digits[i++] = 'x';
    while (n)
        digits[i++] = tmp[--n];
    digits[i] = 0;
    put(line, digits);
}

static void putDec(line_t* line, long value)
{
    char tmp[24];
    int n = 0;
    int negative = value < 0;
    unsigned long v = negative ? -(unsigned long)value : (unsigned long)value;
    do {
        tmp[n++] = '0' + v % 10;
        v /= 10;
    } while (v && n < 22);
    char out[24];
    int i = 0;
    if (negative)
        out[i++] = '-';
    while (n)
        out[i++] = tmp[--n];
    out[i] = 0;
    put(line, out);
}

static void startLine(line_t* line)
{
    line->length = 0;
    put(line, "[crash ");
    put(line, guardTag);
    put(line, " pid ");
    putDec(line, getpid());
    put(line, "] ");
}

static void flush(line_t* line)
{
    put(line, "\n");
    ssize_t ignored = write(2, line->text, line->length);
    if (reportFd >= 0)
        ignored = write(reportFd, line->text, line->length);
    (void)ignored;
    for (size_t i = 0; i < line->length && reportLength < sizeof reportText; i++)
        reportText[reportLength++] = line->text[i];
}

// What the log hasn't had yet, to the log.
static void sendReport(void)
{
    if (ps5wk_crash_log_report && reportLength > reportSent) {
        size_t from = reportSent;
        reportSent = reportLength;
        ps5wk_crash_log_report(reportText + from, reportLength - from);
    }
}

static void finish(int code)
{
    sendReport();
    _exit(code);
}

// Whether the page holding address can be read. Without the pipe, only sp's
// own page counts as known.
static int pageReadable(uintptr_t address)
{
    if (probePipe[1] < 0)
        return 0;
    const uintptr_t page = address & ~(pageSize - 1);
    if (write(probePipe[1], (const void*)page, 1) != 1)
        return 0;
    char byte;
    ssize_t ignored = read(probePipe[0], &byte, 1);
    (void)ignored;
    return 1;
}

// The stack line being built: a fault while scanning (past the top of the
// thread's stack) re-enters the handler, which prints what was found so far.
static line_t scanLine;
static volatile sig_atomic_t scanning;

static const char* signalName(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL (WebKit CRASH()/RELEASE_ASSERT is ud2)";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    case SIGTRAP: return "SIGTRAP";
    case SIGSYS: return "SIGSYS";
    default: return "?";
    }
}

static int inText(uintptr_t address)
{
    return address >= textStart && address < textEnd;
}

// Whether the bytes before address end with a call instruction, i.e. it can
// be a return address (filters string and data pointers out of the scan).
static int afterCall(uintptr_t address)
{
    if (address < textStart + 8 || !inText(address))
        return 0;
    if (!codeReadable)
        return address < codeEnd;
    // The 7 bytes before it may be on a page that isn't mapped (the image
    // base's page, where the ELF header would be, or a gap between segments).
    if (!pageReadable(address - 7) || !pageReadable(address - 1))
        return 0;
    const unsigned char* b = (const unsigned char*)address;
    if (b[-5] == 0xe8)
        return 1; // call rel32
    if (b[-2] == 0xff && (b[-1] & 0x38) == 0x10)
        return 1; // call *reg / *(reg)
    if (b[-3] == 0xff && (b[-2] & 0x38) == 0x10)
        return 1; // call *disp8(reg) / sib
    if (b[-6] == 0xff && (b[-5] & 0x38) == 0x10)
        return 1; // call *disp32(reg) / *rip+disp32
    if (b[-7] == 0xff && (b[-6] & 0x38) == 0x10)
        return 1; // call *disp32(sib)
    if (b[-4] == 0xff && (b[-3] & 0x38) == 0x10)
        return 1; // call *disp8(sib)
    return 0;
}

static void handler(int sig, siginfo_t* info, void* context)
{
    if (inHandler) {
        // A fault in the report itself. Once: the partial stack line, and
        // the report to the log; a fault in that ends here.
        if (inHandler > 1)
            _exit(71);
        inHandler = 2;
        if (scanning) {
            scanning = 0;
            put(&scanLine, " (scan stopped: a fault reading the stack)");
            flush(&scanLine);
        }
        finish(71);
    }
    inHandler = 1;

    // The PS5 kernel's ucontext is not the SDK header's: FreeBSD's mcontext
    // starts at +0x40, not +0x10 (test/sigctx/sigctx.c on 13.60: rip +0xe0,
    // rsp +0xf8, rbp +0x88). For SIGABRT from abort(), pc is libkernel's
    // raise and si_addr is pc + 2.
    const uintptr_t* words = (const uintptr_t*)context;
    uintptr_t pc = words ? words[0xe0 / 8] : 0;
    uintptr_t sp = words ? words[0xf8 / 8] : 0;
    uintptr_t fp = words ? words[0x88 / 8] : 0;
    uintptr_t base = (uintptr_t)&__ehdr_start;

    line_t line;
    startLine(&line);
    put(&line, "signal ");
    putDec(&line, sig);
    put(&line, " (");
    put(&line, signalName(sig));
    put(&line, ") code ");
    putDec(&line, info ? info->si_code : 0);
    put(&line, " addr ");
    putHex(&line, info ? (uintptr_t)info->si_addr : 0);
    put(&line, " pc ");
    putHex(&line, pc);
    if (inText(pc)) {
        put(&line, " (elf +");
        putHex(&line, pc - base);
        put(&line, ")");
    } else if (ps5wk_jit_memory) {
        void *jitStart = 0, *jitEnd = 0;
        ps5wk_jit_memory(&jitStart, &jitEnd, 0, 0);
        if (pc >= (uintptr_t)jitStart && pc < (uintptr_t)jitEnd) {
            put(&line, " (JIT code +");
            putHex(&line, pc - (uintptr_t)jitStart);
            put(&line, ")");
        }
    }
    put(&line, " sp ");
    putHex(&line, sp);
    put(&line, " fp ");
    putHex(&line, fp);
    put(&line, " thread ");
    putHex(&line, (uintptr_t)pthread_self());
    flush(&line);
    // The first line to the log now: the scan below can still go wrong.
    sendReport();

    // Stack scan, printed in chunks so a fault while scanning loses little.
    if (sp && !(sp & 7)) {
        startLine(&scanLine);
        put(&scanLine, codeReadable ? "stack:" : "stack (code not readable: every .text value):");
        int found = 0, inLine = 0;
        scanning = 1;
        // sp's own page too (a stack overflow faults on it).
        if (probePipe[1] >= 0 && !pageReadable(sp)) {
            put(&scanLine, " (sp's page can't be read)");
            found = SCAN_WORDS; // no scan; the line is flushed below
        }
        const uintptr_t* word = (const uintptr_t*)sp;
        for (int i = 0; i < SCAN_WORDS && found < MAX_CANDIDATES; i++) {
            uintptr_t at = (uintptr_t)&word[i];
            if (!(at & (pageSize - 1)) && !pageReadable(at)) {
                put(&scanLine, " (end of the mapped stack)");
                break;
            }
            uintptr_t value = word[i];
            if (!afterCall(value))
                continue;
            put(&scanLine, " +");
            putHex(&scanLine, value - base);
            found++;
            if (++inLine == 8) {
                flush(&scanLine);
                startLine(&scanLine);
                put(&scanLine, codeReadable ? "stack:" : "stack (code not readable: every .text value):");
                inLine = 0;
            }
        }
        scanning = 0;
        if (found == SCAN_WORDS)
            found = 0, inLine = 1;
        if (inLine || !found) {
            if (!found)
                put(&scanLine, " (no .text addresses)");
            flush(&scanLine);
        }
    }
    finish(70);
}

void ps5wk_crash_guard_thread(void)
{
    stack_t current;
    if (!sigaltstack(NULL, &current) && !(current.ss_flags & SS_DISABLE) && current.ss_sp)
        return;
    void* memory = mmap(NULL, ALT_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED)
        return;
    stack_t stack;
    memset(&stack, 0, sizeof stack);
    stack.ss_sp = memory;
    stack.ss_size = ALT_STACK_SIZE;
    if (sigaltstack(&stack, NULL))
        munmap(memory, ALT_STACK_SIZE);
}

void ps5wk_crash_guard_report_file(const char* path)
{
    if (reportFd < 0 && path)
        reportFd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
}

void ps5wk_crash_guard_install(const char* tag)
{
    if (tag) {
        size_t n = strlen(tag);
        if (n >= sizeof guardTag)
            n = sizeof guardTag - 1;
        memcpy(guardTag, tag, n);
        guardTag[n] = 0;
    }
    textStart = (uintptr_t)&__ehdr_start;
    textEnd = (uintptr_t)&__init_array_start;
    long page = sysconf(_SC_PAGESIZE);
    if (page >= 4096 && !(page & (page - 1)))
        pageSize = (uintptr_t)page;
    if (probePipe[0] < 0 && !pipe(probePipe)) {
        for (int i = 0; i < 2; i++) {
            fcntl(probePipe[i], F_SETFL, fcntl(probePipe[i], F_GETFL) | O_NONBLOCK);
            fcntl(probePipe[i], F_SETFD, FD_CLOEXEC);
        }
    }
    codeEnd = (uintptr_t)&_etext > textStart && (uintptr_t)&_etext <= textEnd ? (uintptr_t)&_etext : textEnd;
    // This function's own bytes: readable or not, as all of .text is.
    codeReadable = probePipe[1] >= 0 && pageReadable((uintptr_t)&ps5wk_crash_guard_install);
    ps5wk_crash_guard_thread();
    static const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS };
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++) {
        struct sigaction action;
        memset(&action, 0, sizeof action);
        action.sa_sigaction = handler;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
        sigemptyset(&action.sa_mask);
        sigaction(signals[i], &action, NULL);
    }
}

#ifndef PS5WK_CRASHGUARD_NO_THREAD_WRAP
// -Wl,--wrap=pthread_create: every new thread gets its alternate signal stack
// first. The stack is freed when the thread function returns (not on
// pthread_exit, which leaks 128 KiB).
int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);

typedef struct {
    void* (*start)(void*);
    void* argument;
} thread_start_t;

// profiler.c (ps5wk-child): the thread's stack bounds and CPU clock.
void ps5wk_profiler_thread(int starting) __attribute__((weak));

static void* threadTrampoline(void* data)
{
    thread_start_t start = *(thread_start_t*)data;
    free(data);
    ps5wk_crash_guard_thread();
    if (ps5wk_profiler_thread)
        ps5wk_profiler_thread(1);
    void* result = start.start(start.argument);
    if (ps5wk_profiler_thread)
        ps5wk_profiler_thread(0);
    stack_t current;
    if (!sigaltstack(NULL, &current) && !(current.ss_flags & SS_DISABLE) && current.ss_sp && !(current.ss_flags & SS_ONSTACK)) {
        stack_t disable;
        memset(&disable, 0, sizeof disable);
        disable.ss_flags = SS_DISABLE;
        if (!sigaltstack(&disable, NULL))
            munmap(current.ss_sp, current.ss_size);
    }
    return result;
}

// The payload libc's default thread stack is 64 KiB (test/stack/stack.c on
// 13.60), and WTF leaves the size to the platform for most thread types
// (Workers' JavaScript threads included; JSC alone reserves 192 KiB of
// stack zones). Threads that would get less than MIN_THREAD_STACK and did
// not bring their own stack get MIN_THREAD_STACK, FreeBSD's amd64 default.
#define MIN_THREAD_STACK (2u << 20)

static int widenedAttributes(const pthread_attr_t* attributes, pthread_attr_t* out)
{
    size_t size = 0;
    if (attributes) {
        void* own = NULL;
        size_t ownSize = 0;
        if (pthread_attr_getstack(attributes, &own, &ownSize) == 0 && own)
            return 0;
        if (pthread_attr_getstacksize(attributes, &size) != 0 || size >= MIN_THREAD_STACK)
            return 0;
    }
    if (pthread_attr_init(out) != 0)
        return 0;
    if (attributes) {
        int v;
        size_t guard;
        struct sched_param sp;
        if (pthread_attr_getdetachstate(attributes, &v) == 0)
            pthread_attr_setdetachstate(out, v);
        if (pthread_attr_getguardsize(attributes, &guard) == 0)
            pthread_attr_setguardsize(out, guard);
        if (pthread_attr_getinheritsched(attributes, &v) == 0)
            pthread_attr_setinheritsched(out, v);
        if (pthread_attr_getschedpolicy(attributes, &v) == 0)
            pthread_attr_setschedpolicy(out, v);
        if (pthread_attr_getschedparam(attributes, &sp) == 0)
            pthread_attr_setschedparam(out, &sp);
        if (pthread_attr_getscope(attributes, &v) == 0)
            pthread_attr_setscope(out, v);
    }
    if (pthread_attr_setstacksize(out, MIN_THREAD_STACK) != 0) {
        pthread_attr_destroy(out);
        return 0;
    }
    return 1;
}

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attributes, void* (*start)(void*), void* argument)
{
    pthread_attr_t widened;
    int useWidened = widenedAttributes(attributes, &widened);
    const pthread_attr_t* effective = useWidened ? &widened : attributes;
    thread_start_t* data = (thread_start_t*)malloc(sizeof *data);
    int rc;
    if (!data)
        rc = __real_pthread_create(thread, effective, start, argument);
    else {
        data->start = start;
        data->argument = argument;
        rc = __real_pthread_create(thread, effective, threadTrampoline, data);
        if (rc)
            free(data);
    }
    if (useWidened)
        pthread_attr_destroy(&widened);
    return rc;
}
#endif /* PS5WK_CRASHGUARD_NO_THREAD_WRAP */
