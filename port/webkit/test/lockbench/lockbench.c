/*
 * lockbench: payload threads vs malloc/free and locks, the console's libc
 * against ps5locks (futex locks) and ps5malloc (libpas).
 *
 * Built with WebKit (libpas comes from its objects), apps/CMakeLists.txt:
 *   NINJA_TARGETS=ps5wk-lockbench port/webkit/build.sh build
 *   socat -t 300 - TCP:$PS5_HOST:9021 < toolchain/wpe-webkit/build/bin/ps5wk-lockbench
 * Optional argument line on stdin is not read; everything runs, ~1-2 min.
 *
 * For 1, 2, 4 and 6 threads (OPS operations per thread):
 *   malloc/free (libc)      __real_malloc/__real_free, the console's heap
 *   malloc/free (libpas)    malloc/free through ps5malloc
 *   mutex (libc)            __real_pthread_mutex_lock/unlock on one mutex
 *   mutex (futex sN)        ps5locks, spinning N times before sleeping
 *   rwlock read (libc/futex) read lock/unlock, 1 in 64 a write lock
 *   semaphore mutex, ALU    as before (the table in FEASIBILITY §9)
 * Then two threads handing a token back and forth through a condition
 * variable (round trips/s), and a 4+4 producer/consumer queue (items/s),
 * each with libc and futex locks.
 *
 * The first version (no ps5locks, libc only; lockbench.elf) measured, per
 * thread: malloc/free 0.30/47.8/106/162 us, mutex 17.5 ns/4.2/13.4/20.8 us,
 * semaphore 0.97/1.08/1.28/1.74 us, ALU 2.5 ns.
 */
#include "../../apps/common/ps5locks.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define OPS 200000

void* __real_malloc(size_t);
void __real_free(void*);
int __real_pthread_mutex_lock(pthread_mutex_t*);
int __real_pthread_mutex_unlock(pthread_mutex_t*);
int __real_pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
int __real_pthread_cond_signal(pthread_cond_t*);
int __real_pthread_rwlock_rdlock(pthread_rwlock_t*);
int __real_pthread_rwlock_wrlock(pthread_rwlock_t*);
int __real_pthread_rwlock_unlock(pthread_rwlock_t*);

/* ps5malloc's ownership bitmap needs every anonymous mapping. */
void ps5malloc_note_map(void*, size_t, int);
void ps5malloc_note_unmap(void*, size_t);
void* __real_mmap(void*, size_t, int, int, int, off_t);
int __real_munmap(void*, size_t);
void* __wrap_mmap(void* a, size_t l, int p, int f, int fd, off_t o)
{
    void* r = __real_mmap(a, l, p, f, fd, o);
    if (r != MAP_FAILED && (f & MAP_ANON))
        ps5malloc_note_map(r, l, f);
    return r;
}
int __wrap_munmap(void* a, size_t l)
{
    ps5malloc_note_unmap(a, l);
    return __real_munmap(a, l);
}

static pthread_mutex_t libcMutex = PTHREAD_MUTEX_INITIALIZER, futexMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t libcRw = PTHREAD_RWLOCK_INITIALIZER, futexRw = PTHREAD_RWLOCK_INITIALIZER;
static sem_t sem;
static volatile unsigned long sink;
static int mode;

enum { M_MALLOC_LIBC, M_MALLOC_PAS, M_MUTEX_LIBC, M_MUTEX_S0, M_MUTEX_S100, M_MUTEX_S1000, M_RW_LIBC, M_RW_FUTEX, M_SEM, M_ALU, M_COUNT };
static const char* names[] = { "malloc (libc)", "malloc (libpas)", "mutex (libc)", "mutex (futex s0)", "mutex (futex s100)",
    "mutex (futex s1000)", "rwlock (libc)", "rwlock (futex)", "semaphore", "alu" };

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void* worker(void* arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u;
    unsigned long local = 0;
    for (int i = 0; i < OPS; i++) {
        seed = seed * 1103515245 + 12345;
        switch (mode) {
        case M_MALLOC_LIBC: {
            void* p = __real_malloc(16 + (seed >> 20) % 512);
            local += (uintptr_t)p;
            __real_free(p);
            break;
        }
        case M_MALLOC_PAS: {
            void* p = malloc(16 + (seed >> 20) % 512);
            local += (uintptr_t)p;
            free(p);
            break;
        }
        case M_MUTEX_LIBC:
            __real_pthread_mutex_lock(&libcMutex);
            local++;
            sink++;
            __real_pthread_mutex_unlock(&libcMutex);
            break;
        case M_MUTEX_S0:
        case M_MUTEX_S100:
        case M_MUTEX_S1000:
            pthread_mutex_lock(&futexMutex);
            local++;
            sink++;
            pthread_mutex_unlock(&futexMutex);
            break;
        case M_RW_LIBC:
            if ((seed >> 24) % 64 == 0) {
                __real_pthread_rwlock_wrlock(&libcRw);
                sink++;
            } else {
                __real_pthread_rwlock_rdlock(&libcRw);
                local += sink;
            }
            __real_pthread_rwlock_unlock(&libcRw);
            break;
        case M_RW_FUTEX:
            if ((seed >> 24) % 64 == 0) {
                pthread_rwlock_wrlock(&futexRw);
                sink++;
            } else {
                pthread_rwlock_rdlock(&futexRw);
                local += sink;
            }
            pthread_rwlock_unlock(&futexRw);
            break;
        case M_SEM:
            sem_wait(&sem);
            local++;
            sink++;
            sem_post(&sem);
            break;
        case M_ALU:
            for (int j = 0; j < 20; j++)
                local = local * 31 + j;
            break;
        }
    }
    sink += local;
    return NULL;
}

/* Condition variables: ping-pong and a bounded queue, libc or futex. */
static int useLibc;
static pthread_mutex_t cm = PTHREAD_MUTEX_INITIALIZER, cmLibc = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c1 = PTHREAD_COND_INITIALIZER, c2 = PTHREAD_COND_INITIALIZER;
static pthread_cond_t c1Libc = PTHREAD_COND_INITIALIZER, c2Libc = PTHREAD_COND_INITIALIZER;
static int token, queueCount, queueDone;
#define ROUNDS 20000
#define QITEMS 50000

static void lockC(void) { if (useLibc) __real_pthread_mutex_lock(&cmLibc); else pthread_mutex_lock(&cm); }
static void unlockC(void) { if (useLibc) __real_pthread_mutex_unlock(&cmLibc); else pthread_mutex_unlock(&cm); }
static void waitC(int which) { if (useLibc) __real_pthread_cond_wait(which ? &c2Libc : &c1Libc, &cmLibc); else pthread_cond_wait(which ? &c2 : &c1, &cm); }
static void signalC(int which) { if (useLibc) __real_pthread_cond_signal(which ? &c2Libc : &c1Libc); else pthread_cond_signal(which ? &c2 : &c1); }

static void* pingPong(void* arg)
{
    int me = (int)(intptr_t)arg;
    for (int i = 0; i < ROUNDS; i++) {
        lockC();
        while (token != me)
            waitC(me);
        token = !me;
        signalC(!me);
        unlockC();
    }
    return NULL;
}

static void* producer(void* arg)
{
    (void)arg;
    for (int i = 0; i < QITEMS; i++) {
        lockC();
        while (queueCount >= 64)
            waitC(1);
        queueCount++;
        signalC(0);
        unlockC();
    }
    return NULL;
}

static void* consumer(void* arg)
{
    (void)arg;
    for (;;) {
        lockC();
        while (queueCount == 0 && queueDone < 4 * QITEMS)
            waitC(0);
        if (queueDone >= 4 * QITEMS) {
            signalC(0);
            unlockC();
            return NULL;
        }
        queueCount--;
        if (++queueDone == 4 * QITEMS)
            signalC(0);
        signalC(1);
        unlockC();
    }
}

static double run(void* (*f)(void*), int n)
{
    pthread_t t[8];
    double start = now();
    for (int i = 0; i < n; i++)
        pthread_create(&t[i], NULL, f, (void*)(intptr_t)(i + 1));
    for (int i = 0; i < n; i++)
        pthread_join(t[i], NULL);
    return now() - start;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    sem_init(&sem, 0, 1);
    printf("RESULT lockbench start: %d ops per thread, page size %d\n", OPS, getpagesize());
    for (mode = 0; mode < M_COUNT; mode++) {
        ps5locks_spin = mode == M_MUTEX_S0 ? 0 : mode == M_MUTEX_S1000 ? 1000 : 100;
        for (int n = 1; n <= 6; n += (n == 1 ? 1 : 2)) {
            ps5locks_stats_t before, after;
            ps5locks_stats(&before);
            double s = run(worker, n);
            ps5locks_stats(&after);
            printf("RESULT lockbench %-20s %d threads: %8.1f ms, %8.1f ns/op/thread, %7.2f Mops/s total, %llu parks\n", names[mode], n,
                s * 1e3, s * 1e9 / OPS, n * OPS / s / 1e6,
                (unsigned long long)(after.mutex_parks + after.rwlock_parks - before.mutex_parks - before.rwlock_parks));
        }
    }
    ps5locks_spin = 100;
    for (useLibc = 1; useLibc >= 0; useLibc--) {
        token = 0;
        pthread_t a, b;
        double start = now();
        pthread_create(&a, NULL, pingPong, (void*)0);
        pthread_create(&b, NULL, pingPong, (void*)1);
        pthread_join(a, NULL);
        pthread_join(b, NULL);
        double s = now() - start;
        printf("RESULT lockbench cond ping-pong (%s): %d round trips in %.1f ms, %.2f us each\n", useLibc ? "libc" : "futex",
            ROUNDS, s * 1e3, s * 1e6 / ROUNDS);
    }
    for (useLibc = 1; useLibc >= 0; useLibc--) {
        queueCount = queueDone = 0;
        pthread_t t[8];
        double start = now();
        for (int i = 0; i < 4; i++)
            pthread_create(&t[i], NULL, producer, NULL);
        for (int i = 4; i < 8; i++)
            pthread_create(&t[i], NULL, consumer, NULL);
        for (int i = 0; i < 8; i++)
            pthread_join(t[i], NULL);
        double s = now() - start;
        printf("RESULT lockbench cond queue 4+4 (%s): %d items in %.1f ms, %.2f M items/s\n", useLibc ? "libc" : "futex",
            4 * QITEMS, s * 1e3, 4 * QITEMS / s / 1e6);
    }
    printf("RESULT lockbench done\n");
    return 0;
}
