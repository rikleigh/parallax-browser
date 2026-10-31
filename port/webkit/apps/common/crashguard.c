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
#include <sys/mman.h>
#include <unistd.h>

// The image's address range, from symbols the linker defines: __ehdr_start
// is the load base (vaddr 0; the ELF header itself is not mapped there by
// elfldr, so it must not be read), __init_array_start is in .data, past
// .text and .rodata. Do not parse the ELF header: the first payload built
// that way faulted in ps5wk_crash_guard_install, before the handlers existed.
extern char __ehdr_start;
extern char __init_array_start;
static uintptr_t textStart, textEnd;

// JSC's executable memory (WebKit patch 0030), so a pc in JIT code is named.
void ps5wk_jit_memory(void** start, void** end, size_t* allocated, size_t* committed) __attribute__((weak));

#define ALT_STACK_SIZE (128 * 1024)
#define SCAN_WORDS 4096
#define MAX_CANDIDATES 24

static char guardTag[32] = "?";
static volatile sig_atomic_t inHandler;

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
    (void)ignored;
}

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
    if (inHandler)
        _exit(71);
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

    // Stack scan, printed in chunks so a fault while scanning loses little.
    if (sp && !(sp & 7)) {
        startLine(&line);
        put(&line, "stack:");
        int found = 0, inLine = 0;
        const uintptr_t* word = (const uintptr_t*)sp;
        for (int i = 0; i < SCAN_WORDS && found < MAX_CANDIDATES; i++) {
            uintptr_t value = word[i];
            if (!afterCall(value))
                continue;
            put(&line, " +");
            putHex(&line, value - base);
            found++;
            if (++inLine == 8) {
                flush(&line);
                startLine(&line);
                put(&line, "stack:");
                inLine = 0;
            }
        }
        if (inLine || !found) {
            if (!found)
                put(&line, " (no .text addresses)");
            flush(&line);
        }
    }
    _exit(70);
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
