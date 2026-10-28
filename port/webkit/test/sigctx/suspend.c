/*
 * suspend.c - what JSC's conservative GC reads as a suspended thread's stack
 * pointer in a PS5 payload. WTF suspends threads with pthread_kill(SIGUSR1)
 * and reads mc_rsp from &uc->uc_mcontext (SDK header: ucontext +0x10, so
 * mc_rsp at +0xc8). The PS5 kernel puts the mcontext at +0x40 (sigctx.c), so
 * the real rsp is at +0xf8 and +0xc8 is the kernel's mc_addr.
 * The worker touches fresh heap pages (page faults set the trap address) and
 * idles in a syscall, as JS threads do; every sample prints both words and
 * whether each lies inside the worker's stack.
 *   prospero-clang -O1 suspend.c -o suspend.elf -lpthread && socat - TCP:$PS5_HOST:9021 < suspend.elf
 */
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static sem_t ack;
static volatile uintptr_t wtfSp, realSp, realRbp;
static uintptr_t stackLo, stackHi;
static volatile int stop;

static void handler(int sig, siginfo_t* info, void* context)
{
    (void)sig;
    (void)info;
    const uintptr_t* w = (const uintptr_t*)context;
    wtfSp = w[0xc8 / 8];
    realSp = w[0xf8 / 8];
    realRbp = w[0x88 / 8];
    sem_post(&ack);
}

static void* worker(void* arg)
{
    (void)arg;
    size_t chunk = 64 << 20;
    char* heap = mmap(NULL, chunk, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    size_t off = 0;
    while (!stop) {
        for (int i = 0; i < 16; i++) { // fault in fresh pages
            heap[off] = 1;
            off = (off + 16384) % chunk;
        }
        if (off == 0)
            madvise(heap, chunk, MADV_FREE);
        struct timespec ts = { 0, 200000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    sem_init(&ack, 0, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigfillset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    size_t ssize = 2 << 20;
    void* stack = mmap(NULL, ssize, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    pthread_attr_setstack(&attr, stack, ssize);
    stackLo = (uintptr_t)stack;
    stackHi = stackLo + ssize;
    pthread_t t;
    pthread_create(&t, &attr, worker, NULL);
    printf("RESULT suspend worker stack [%p, %p)\n", (void*)stackLo, (void*)stackHi);

    int wtfIn = 0, wtfZero = 0, wtfAbove = 0, wtfBelow = 0, realIn = 0;
    for (int i = 0; i < 200; i++) {
        usleep(5000);
        pthread_kill(t, SIGUSR1);
        sem_wait(&ack);
        uintptr_t a = wtfSp, b = realSp;
        if (a == 0)
            wtfZero++;
        else if (a >= stackLo && a < stackHi)
            wtfIn++;
        else if (a >= stackHi)
            wtfAbove++;
        else
            wtfBelow++;
        if (b >= stackLo && b < stackHi)
            realIn++;
        if (i < 12)
            printf("  sample %d: +0xc8 (what WTF reads as rsp) %p   +0xf8 (rsp) %p   +0x88 (rbp) %p\n", i, (void*)a, (void*)b, (void*)realRbp);
    }
    stop = 1;
    pthread_join(t, NULL);
    printf("RESULT suspend 200 samples: +0xc8 in stack %d, zero %d, above stack top %d (JSC: negative size -> huge copy), below %d; +0xf8 in stack %d\n",
        wtfIn, wtfZero, wtfAbove, wtfBelow, realIn);
    return 0;
}
