// Sampling profiler for one thread of a payload (PS5WK_PROFILE=1 in
// /data/ps5webkit/child.env profiles each WebProcess's main thread).
//
// A helper thread sends SIGPROF to the target thread every 2 ms; the handler
// records the interrupted pc and the first return addresses found on its
// stack (a scan for words that follow a call instruction, as the crash guard
// does: no frame pointers in this build). Samples are appended to
// /data/ps5webkit/prof/<tag>-<pid>.txt every 5 s, one line per sample:
// "pc caller1 caller2 ...", ELF offsets in hex (llvm-addr2line on the
// unstripped ps5wk-child). port/webkit/test/prof.sh fetches and summarizes.
#include "profiler.h"

#include <pthread.h>
#include <pthread_np.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern char __ehdr_start;
extern char __init_array_start;

#define DEPTH 6
#define RING 8192
#define SCAN_WORDS 512

typedef struct {
    uintptr_t pc[DEPTH + 1];
} sample_t;

static sample_t ring[RING];
static volatile unsigned head, tail;
static pthread_t target;
static uintptr_t textStart, textEnd;
static uintptr_t stackTop; // scan bound: the target thread's stack end
static char path[128];

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

static void onProf(int sig, siginfo_t* info, void* context)
{
    (void)sig;
    (void)info;
    unsigned h = head;
    if (h - tail >= RING)
        return; // full: drop
    sample_t* s = &ring[h % RING];
    memset(s, 0, sizeof *s);
    // PS5 ucontext: rip at +0xe0, rsp at +0xf8 (see crashguard.c).
    const uintptr_t* words = (const uintptr_t*)context;
    s->pc[0] = words[0xe0 / 8];
    uintptr_t sp = words[0xf8 / 8];
    int n = 1;
    if (sp && !(sp & 7)) {
        const uintptr_t* w = (const uintptr_t*)sp;
        int words = SCAN_WORDS;
        if (stackTop > sp && (stackTop - sp) / 8 < (uintptr_t)words)
            words = (int)((stackTop - sp) / 8);
        else if (stackTop <= sp)
            words = 0;
        for (int i = 0; i < words && n <= DEPTH; i++)
            if (afterCall(w[i]))
                s->pc[n++] = w[i];
    }
    __atomic_store_n(&head, h + 1, __ATOMIC_RELEASE);
}

static void flushSamples(void)
{
    FILE* f = fopen(path, "a");
    if (!f)
        return;
    uintptr_t base = (uintptr_t)&__ehdr_start;
    unsigned h = __atomic_load_n(&head, __ATOMIC_ACQUIRE);
    while (tail != h) {
        sample_t* s = &ring[tail % RING];
        for (int i = 0; i <= DEPTH && s->pc[i]; i++) {
            uintptr_t pc = s->pc[i];
            fprintf(f, i ? " %lx" : "%lx", (unsigned long)(pc >= textStart && pc < textEnd ? pc - base : 0));
        }
        fputc('\n', f);
        tail++;
    }
    fclose(f);
}

static void* profilerThread(void* data)
{
    (void)data;
    struct timespec tick = { 0, 2 * 1000 * 1000 };
    int ticks = 0;
    for (;;) {
        nanosleep(&tick, NULL);
        pthread_kill(target, SIGPROF);
        if (++ticks % 2500 == 0) // ~5 s
            flushSamples();
    }
    return NULL;
}

void ps5wk_profiler_start(const char* tag)
{
    const char* on = getenv("PS5WK_PROFILE");
    if (!on || strcmp(on, "1"))
        return;
    textStart = (uintptr_t)&__ehdr_start;
    textEnd = (uintptr_t)&__init_array_start;
    target = pthread_self();
    pthread_attr_t attr;
    void* stackBase = NULL;
    size_t stackSize = 0;
    if (!pthread_attr_init(&attr) && !pthread_attr_get_np(target, &attr))
        pthread_attr_getstack(&attr, &stackBase, &stackSize);
    stackTop = (uintptr_t)stackBase + stackSize;
    mkdir("/data/ps5webkit/prof", 0777);
    snprintf(path, sizeof path, "/data/ps5webkit/prof/%s-%d.txt", tag, (int)getpid());
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = onProf;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPROF, &action, NULL);
    pthread_t thread;
    if (!pthread_create(&thread, NULL, profilerThread, NULL))
        pthread_detach(thread);
    fprintf(stderr, "[profiler] sampling thread %p (stack %p + %zu) every 2 ms -> %s\n", (void*)target, stackBase, stackSize, path);
}
