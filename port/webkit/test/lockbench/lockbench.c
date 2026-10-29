/*
 * lockbench: payload threads vs malloc/free and mutexes (why CPU page painting
 * on several threads was slower than on one).
 *   prospero-clang -O2 -o lockbench.elf lockbench.c   (env.sh, then the SDK's prospero.sh)
 *   socat -t 60 - TCP:$PS5_HOST:9021 < lockbench.elf
 * For 1, 2, 4 and 6 threads: malloc/free pairs, pthread mutex lock/unlock on
 * one shared mutex, semaphore (sem_wait/sem_post) as a mutex, and pure ALU work.
 */
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define OPS 400000
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static sem_t sem;
static volatile unsigned long sink;
static int mode;

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void* worker(void* arg)
{
    unsigned seed = (unsigned)(unsigned long)arg * 2654435761u;
    unsigned long local = 0;
    for (int i = 0; i < OPS; i++) {
        switch (mode) {
        case 0: {
            seed = seed * 1103515245 + 12345;
            void* p = malloc(16 + (seed >> 20) % 512);
            local += (unsigned long)p;
            free(p);
            break;
        }
        case 1:
            pthread_mutex_lock(&mutex);
            local++;
            sink++;
            pthread_mutex_unlock(&mutex);
            break;
        case 2:
            sem_wait(&sem);
            local++;
            sink++;
            sem_post(&sem);
            break;
        case 3:
            for (int j = 0; j < 20; j++)
                local = local * 31 + j;
            break;
        }
    }
    sink += local;
    return NULL;
}

int main(void)
{
    static const char* names[] = { "malloc/free", "pthread mutex", "semaphore mutex", "alu" };
    sem_init(&sem, 0, 1);
    for (mode = 0; mode < 4; mode++) {
        for (int n = 1; n <= 6; n += (n == 1 ? 1 : 2)) {
            pthread_t t[8];
            double start = now();
            for (int i = 0; i < n; i++)
                pthread_create(&t[i], NULL, worker, (void*)(unsigned long)(i + 1));
            for (int i = 0; i < n; i++)
                pthread_join(t[i], NULL);
            double s = now() - start;
            printf("RESULT lockbench %-16s %d threads: %7.1f ms, %6.1f ns/op/thread, %6.2f Mops/s total\n", names[mode], n, s * 1e3, s * 1e9 / OPS, n * OPS / s / 1e6);
            fflush(stdout);
        }
    }
    return 0;
}
