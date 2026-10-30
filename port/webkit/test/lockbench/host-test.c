/*
 * Logic tests for apps/common/ps5locks.c and ps5malloc.c on the Mac (macOS
 * __ulock_wait/__ulock_wake stand in for _umtx_op; libpas is a stub over the
 * system malloc). Not a performance test: real numbers come from lockbench
 * on the console.
 *
 *   port/webkit/test/lockbench/host-test.sh
 */
#define PS5LOCKS_HOST_TEST 1
#include "../../apps/common/ps5locks.c"

#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>

/* ps5malloc.c's dependencies, stubbed. */
static int pasAllocs, pasFrees;
void* __real_malloc(size_t n) { return malloc(n); }
void* __real_calloc(size_t n, size_t s) { return calloc(n, s); }
void* __real_realloc(void* p, size_t n) { return realloc(p, n); }
static int realFrees;
void __real_free(void* p) { __atomic_fetch_add(&realFrees, 1, __ATOMIC_RELAXED); free(p); }
int __real_posix_memalign(void** o, size_t a, size_t n) { return posix_memalign(o, a, n); }
void* __real_aligned_alloc(size_t a, size_t n) { return aligned_alloc(a, n); }
size_t __real_malloc_usable_size(const void* p) { (void)p; return 0; }
void* __real_mmap(void* a, size_t l, int p, int f, int fd, off_t o) { return mmap(a, l, p, f, fd, o); }
int __real_munmap(void* a, size_t l) { return munmap(a, l); }
#define mode pm_mode
#define decide pm_decide
#define fallback pm_fallback
#define aligned pm_aligned
#include "../../apps/common/ps5malloc.c"
#undef mode
#undef decide
void* wtf_try_aligned_malloc(size_t alignment, size_t size)
{
    __atomic_fetch_add(&pasAllocs, 1, __ATOMIC_RELAXED);
    void* p = NULL;
    return posix_memalign(&p, alignment, size) ? NULL : p;
}
void wtf_free(void* p) { __atomic_fetch_add(&pasFrees, 1, __ATOMIC_RELAXED); free(p); }
void* bmalloc_try_realloc(void* old, size_t size) { return realloc(old, size); }
size_t bmalloc_get_allocation_size(void* p) { (void)p; return 0; }

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

typedef uint64_t lock_word; /* what pthread_*_t is on the console */
#define M(x) ((pthread_mutex_t*)&(x))
#define C(x) ((pthread_cond_t*)&(x))
#define RW(x) ((pthread_rwlock_t*)&(x))

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static pthread_t start(void* (*f)(void*), void* arg)
{
    pthread_t t;
    CHECK(!pthread_create(&t, NULL, f, arg));
    return t;
}

/* 1. plain mutexes ---------------------------------------------------------- */
static lock_word mutexA, mutexB = 1; /* NULL and PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP */
static volatile long counterA, counterB;
#define N 200000

static void* bump(void* arg)
{
    (void)arg;
    for (int i = 0; i < N; i++) {
        __wrap_pthread_mutex_lock(M(mutexA));
        counterA++;
        __wrap_pthread_mutex_unlock(M(mutexA));
        __wrap_pthread_mutex_lock(M(mutexB));
        counterB++;
        __wrap_pthread_mutex_unlock(M(mutexB));
    }
    return NULL;
}

static void test_mutex(void)
{
    pthread_t t[8];
    double s = now();
    for (int i = 0; i < 8; i++)
        t[i] = start(bump, NULL);
    for (int i = 0; i < 8; i++)
        pthread_join(t[i], NULL);
    CHECK(counterA == 8L * N && counterB == 8L * N);
    CHECK(__wrap_pthread_mutex_trylock(M(mutexA)) == 0);
    CHECK(__wrap_pthread_mutex_trylock(M(mutexA)) == EBUSY);
    CHECK(__wrap_pthread_mutex_destroy(M(mutexA)) == EBUSY);
    CHECK(__wrap_pthread_mutex_unlock(M(mutexA)) == 0);
    CHECK(__wrap_pthread_mutex_destroy(M(mutexA)) == 0);
    printf("mutex: 8 threads x %d x 2 locks OK (%.0f ms, %llu parks)\n", N, (now() - s) * 1e3, (unsigned long long)mutexParks);
}

/* 2. timed lock ----------------------------------------------------------- */
static lock_word timedMutex;

static void* holdTimed(void* arg)
{
    (void)arg;
    __wrap_pthread_mutex_lock(M(timedMutex));
    usleep(300000);
    __wrap_pthread_mutex_unlock(M(timedMutex));
    return NULL;
}

static void test_timedlock(void)
{
    pthread_t t = start(holdTimed, NULL);
    usleep(50000);
    struct timespec abs;
    clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_nsec += 100000000;
    if (abs.tv_nsec >= 1000000000) { abs.tv_nsec -= 1000000000; abs.tv_sec++; }
    double s = now();
    CHECK(__wrap_pthread_mutex_timedlock(M(timedMutex), &abs) == ETIMEDOUT);
    double waited = now() - s;
    CHECK(waited > 0.08 && waited < 0.25);
    clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_sec += 2;
    CHECK(__wrap_pthread_mutex_timedlock(M(timedMutex), &abs) == 0);
    CHECK(__wrap_pthread_mutex_unlock(M(timedMutex)) == 0);
    pthread_join(t, NULL);
    printf("timedlock: timed out after %.0f ms, then got it OK\n", waited * 1e3);
}

/* 3. recursive mutexes ---------------------------------------------------- */
static lock_word recursive;
static volatile long recursiveCounter;

static void* bumpRecursive(void* arg)
{
    (void)arg;
    for (int i = 0; i < N / 4; i++) {
        __wrap_pthread_mutex_lock(M(recursive));
        __wrap_pthread_mutex_lock(M(recursive));
        recursiveCounter++;
        __wrap_pthread_mutex_unlock(M(recursive));
        recursiveCounter++;
        __wrap_pthread_mutex_unlock(M(recursive));
    }
    return NULL;
}

static void* tryOther(void* arg)
{
    (void)arg;
    CHECK(__wrap_pthread_mutex_trylock(M(recursive)) == EBUSY);
    CHECK(__wrap_pthread_mutex_unlock(M(recursive)) == EPERM);
    return NULL;
}

static void test_recursive(void)
{
    lock_word attr;
    CHECK(!__wrap_pthread_mutexattr_init((pthread_mutexattr_t*)&attr));
    CHECK(!__wrap_pthread_mutexattr_settype((pthread_mutexattr_t*)&attr, PTHREAD_MUTEX_RECURSIVE));
    int type = -1;
    CHECK(!__wrap_pthread_mutexattr_gettype((pthread_mutexattr_t*)&attr, &type) && type == PTHREAD_MUTEX_RECURSIVE);
    CHECK(!__wrap_pthread_mutex_init(M(recursive), (pthread_mutexattr_t*)&attr));
    CHECK(!__wrap_pthread_mutexattr_destroy((pthread_mutexattr_t*)&attr));
    CHECK(recursive & KIND_RECURSIVE);
    CHECK(__wrap_pthread_mutex_lock(M(recursive)) == 0);
    CHECK(__wrap_pthread_mutex_trylock(M(recursive)) == 0);
    pthread_join(start(tryOther, NULL), NULL);
    CHECK(__wrap_pthread_mutex_unlock(M(recursive)) == 0);
    CHECK(__wrap_pthread_mutex_destroy(M(recursive)) == EBUSY);
    CHECK(__wrap_pthread_mutex_unlock(M(recursive)) == 0);
    CHECK(__wrap_pthread_mutex_unlock(M(recursive)) == EPERM);
    pthread_t t[6];
    for (int i = 0; i < 6; i++)
        t[i] = start(bumpRecursive, NULL);
    for (int i = 0; i < 6; i++)
        pthread_join(t[i], NULL);
    CHECK(recursiveCounter == 6L * (N / 4) * 2);
    CHECK(__wrap_pthread_mutex_destroy(M(recursive)) == 0 && recursive == 0);
    printf("recursive: nesting, EBUSY/EPERM, 6 threads OK\n");
}

/* 4. condition variables: a bounded queue --------------------------------- */
#define QSIZE 16
#define ITEMS 100000
static lock_word qMutex, qNotEmpty, qNotFull, qRecursive;
static int queue[QSIZE], qHead, qCount;
static long consumed, consumedSum;
static int useRecursive, useBroadcast;

static pthread_mutex_t* qm(void) { return useRecursive ? M(qRecursive) : M(qMutex); }

static void* producer(void* arg)
{
    int base = (int)(intptr_t)arg;
    for (int i = 0; i < ITEMS; i++) {
        __wrap_pthread_mutex_lock(qm());
        while (qCount == QSIZE)
            __wrap_pthread_cond_wait(C(qNotFull), qm());
        queue[(qHead + qCount++) % QSIZE] = base + i;
        if (useBroadcast)
            __wrap_pthread_cond_broadcast(C(qNotEmpty));
        else
            __wrap_pthread_cond_signal(C(qNotEmpty));
        __wrap_pthread_mutex_unlock(qm());
    }
    return NULL;
}

static void* consumer(void* arg)
{
    (void)arg;
    for (;;) {
        __wrap_pthread_mutex_lock(qm());
        while (qCount == 0 && consumed < 4L * ITEMS)
            __wrap_pthread_cond_wait(C(qNotEmpty), qm());
        if (consumed >= 4L * ITEMS) {
            __wrap_pthread_mutex_unlock(qm());
            return NULL;
        }
        int v = queue[qHead];
        qHead = (qHead + 1) % QSIZE;
        qCount--;
        consumed++;
        consumedSum += v;
        if (consumed == 4L * ITEMS)
            __wrap_pthread_cond_broadcast(C(qNotEmpty));
        if (useBroadcast)
            __wrap_pthread_cond_broadcast(C(qNotFull));
        else
            __wrap_pthread_cond_signal(C(qNotFull));
        __wrap_pthread_mutex_unlock(qm());
    }
}

static void run_queue(const char* name)
{
    qHead = qCount = 0;
    consumed = consumedSum = 0;
    double s = now();
    pthread_t p[4], c[4];
    for (int i = 0; i < 4; i++)
        p[i] = start(producer, (void*)(intptr_t)(i * ITEMS));
    for (int i = 0; i < 4; i++)
        c[i] = start(consumer, NULL);
    for (int i = 0; i < 4; i++)
        pthread_join(p[i], NULL);
    for (int i = 0; i < 4; i++)
        pthread_join(c[i], NULL);
    long n = 4L * ITEMS;
    CHECK(consumed == n && consumedSum == n * (n - 1) / 2);
    printf("cond queue (%s): 4 producers, 4 consumers, %ld items OK (%.0f ms)\n", name, n, (now() - s) * 1e3);
}

static void test_cond(void)
{
    run_queue("signal");
    useBroadcast = 1;
    run_queue("broadcast");
    useBroadcast = 0;
    lock_word attr;
    __wrap_pthread_mutexattr_init((pthread_mutexattr_t*)&attr);
    __wrap_pthread_mutexattr_settype((pthread_mutexattr_t*)&attr, PTHREAD_MUTEX_RECURSIVE);
    __wrap_pthread_mutex_init(M(qRecursive), (pthread_mutexattr_t*)&attr);
    useRecursive = 1;
    run_queue("recursive mutex");
    useRecursive = 0;
}

/* 5. timed waits on each clock -------------------------------------------- */
static void test_timedwait(void)
{
    for (int pass = 0; pass < 2; pass++) {
        clockid_t clock = pass ? CLOCK_MONOTONIC : CLOCK_REALTIME;
        lock_word cond = 0, mutex = 0, attr;
        CHECK(!__wrap_pthread_condattr_init((pthread_condattr_t*)&attr));
        CHECK(!__wrap_pthread_condattr_setclock((pthread_condattr_t*)&attr, clock));
        clockid_t got = -1;
        CHECK(!__wrap_pthread_condattr_getclock((pthread_condattr_t*)&attr, &got) && got == clock);
        CHECK(!__wrap_pthread_cond_init(C(cond), (pthread_condattr_t*)&attr));
        struct timespec abs;
        clock_gettime(clock, &abs);
        abs.tv_nsec += 80000000;
        if (abs.tv_nsec >= 1000000000) { abs.tv_nsec -= 1000000000; abs.tv_sec++; }
        __wrap_pthread_mutex_lock(M(mutex));
        double s = now();
        int r;
        do
            r = __wrap_pthread_cond_timedwait(C(cond), M(mutex), &abs);
        while (r == 0);
        double waited = now() - s;
        CHECK(r == ETIMEDOUT && waited > 0.07 && waited < 0.2);
        CHECK(__wrap_pthread_mutex_trylock(M(mutex)) == EBUSY); /* still held */
        __wrap_pthread_mutex_unlock(M(mutex));
        /* An absolute time in the past times out at once. */
        abs.tv_sec -= 10;
        __wrap_pthread_mutex_lock(M(mutex));
        CHECK(__wrap_pthread_cond_timedwait(C(cond), M(mutex), &abs) == ETIMEDOUT);
        abs.tv_nsec = 2000000000;
        CHECK(__wrap_pthread_cond_timedwait(C(cond), M(mutex), &abs) == EINVAL);
        __wrap_pthread_mutex_unlock(M(mutex));
        printf("timedwait (%s): ETIMEDOUT after %.0f ms OK\n", pass ? "monotonic" : "realtime", waited * 1e3);
    }
}

/* 6. rwlocks -------------------------------------------------------------- */
static lock_word rw;
static volatile long rwA, rwB, rwReads;
static volatile int rwStop;

static void* rwWriter(void* arg)
{
    (void)arg;
    for (int i = 0; i < N / 4; i++) {
        __wrap_pthread_rwlock_wrlock(RW(rw));
        rwA++;
        rwB++;
        __wrap_pthread_rwlock_unlock(RW(rw));
    }
    return NULL;
}

static void* rwReader(void* arg)
{
    (void)arg;
    while (!__atomic_load_n(&rwStop, __ATOMIC_ACQUIRE)) {
        __wrap_pthread_rwlock_rdlock(RW(rw));
        CHECK(rwA == rwB);
        __atomic_fetch_add(&rwReads, 1, __ATOMIC_RELAXED);
        __wrap_pthread_rwlock_unlock(RW(rw));
    }
    return NULL;
}

static void* rwWriteOnce(void* arg)
{
    (void)arg;
    __wrap_pthread_rwlock_wrlock(RW(rw));
    rwA++;
    rwB++;
    __wrap_pthread_rwlock_unlock(RW(rw));
    return NULL;
}

static void test_rwlock(void)
{
    double s = now();
    pthread_t w[3], r[4];
    for (int i = 0; i < 4; i++)
        r[i] = start(rwReader, NULL);
    for (int i = 0; i < 3; i++)
        w[i] = start(rwWriter, NULL);
    for (int i = 0; i < 3; i++)
        pthread_join(w[i], NULL);
    __atomic_store_n(&rwStop, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < 4; i++)
        pthread_join(r[i], NULL);
    CHECK(rwA == 3L * (N / 4) && rwB == rwA);
    /* Read again while a writer waits: no deadlock (readers preferred). */
    CHECK(__wrap_pthread_rwlock_rdlock(RW(rw)) == 0);
    pthread_t t = start(rwWriteOnce, NULL);
    usleep(50000);
    CHECK(__wrap_pthread_rwlock_tryrdlock(RW(rw)) == 0);
    CHECK(__wrap_pthread_rwlock_trywrlock(RW(rw)) == EBUSY);
    __wrap_pthread_rwlock_unlock(RW(rw));
    __wrap_pthread_rwlock_unlock(RW(rw));
    pthread_join(t, NULL);
    CHECK(rwA == 3L * (N / 4) + 1);
    CHECK(__wrap_pthread_rwlock_trywrlock(RW(rw)) == 0);
    CHECK(__wrap_pthread_rwlock_tryrdlock(RW(rw)) == EBUSY);
    struct timespec abs;
    clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_nsec += 50000000;
    if (abs.tv_nsec >= 1000000000) { abs.tv_nsec -= 1000000000; abs.tv_sec++; }
    CHECK(__wrap_pthread_rwlock_timedrdlock(RW(rw), &abs) == ETIMEDOUT);
    __wrap_pthread_rwlock_unlock(RW(rw));
    CHECK(rw == 0);
    printf("rwlock: 3 writers, 4 readers (%ld reads), reread under a waiting writer, timed OK (%.0f ms, %llu parks)\n",
        rwReads, (now() - s) * 1e3, (unsigned long long)rwlockParks);
}

/* 7. ps5malloc's ownership bitmap and emulated TLS ------------------------ */
static __emutls_control tlsControl = { sizeof(long), sizeof(long), { 0 }, NULL };
static long tlsInit = 42;
static __emutls_control tlsControl2 = { sizeof(long), 64, { 0 }, &tlsInit };

static void* tlsThread(void* arg)
{
    long* a = (long*)__emutls_get_address(&tlsControl);
    long* b = (long*)__emutls_get_address(&tlsControl2);
    CHECK(*a == 0 && *b == 42 && ((uintptr_t)b & 63) == 0);
    *a = (long)(intptr_t)arg;
    usleep(10000);
    CHECK(*(long*)__emutls_get_address(&tlsControl) == (long)(intptr_t)arg);
    return NULL;
}

static void test_malloc(void)
{
    size_t length = 64 << 20;
    char* map = (char*)mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(map != MAP_FAILED);
    CHECK(!ps5malloc_owns(map));
    ps5malloc_note_map(map, length, MAP_ANON);
    CHECK(ps5malloc_owns(map) && ps5malloc_owns(map + length - 1) && ps5malloc_owns(map + 12345678));
    CHECK(!ps5malloc_owns(map + length) && !ps5malloc_owns(map - 1));
    /* A partial unmap in the middle, on page boundaries. */
    ps5malloc_note_unmap(map + (16 << 20), 16 << 20);
    CHECK(ps5malloc_owns(map + (16 << 20) - 1) && !ps5malloc_owns(map + (16 << 20)));
    CHECK(!ps5malloc_owns(map + (32 << 20) - 1) && ps5malloc_owns(map + (32 << 20)));
    /* Unaligned length rounds up to whole pages. */
    ps5malloc_note_unmap(map, 100);
    CHECK(!ps5malloc_owns(map + 16383) && ps5malloc_owns(map + 16384));
    /* Not anonymous: not marked. */
    ps5malloc_note_map(map, 16384, 0);
    CHECK(!ps5malloc_owns(map));
    /* A mapping across a 2 GiB chunk boundary. */
    uintptr_t edge = ((uintptr_t)map + ((uintptr_t)1 << 31)) & ~(((uintptr_t)1 << 31) - 1);
    ps5malloc_note_map((void*)(edge - 32768), 65536, MAP_ANON);
    CHECK(ps5malloc_owns((void*)(edge - 32768)) && ps5malloc_owns((void*)(edge + 32767)) && !ps5malloc_owns((void*)(edge + 32768)));
    ps5malloc_note_unmap((void*)(edge - 32768), 65536);
    CHECK(!ps5malloc_owns((void*)(edge - 1)) && !ps5malloc_owns((void*)edge));
    munmap(map, length);
    ps5malloc_note_unmap(map, length);
    CHECK(!ps5malloc_owns(map + (40 << 20)));

    /* free(): a block outside our mappings goes to the console's free. */
    pm_mode = 1;
    void* foreign = malloc(100);
    int before = realFrees;
    __wrap_free(foreign);
    CHECK(realFrees == before + 1 && foreignFrees == 1);
    void* p = __wrap_malloc(10);
    CHECK(pasAllocs == 1);
    (void)p; /* the stub's block isn't in a marked mapping: not freed here */
    void* q = NULL;
    CHECK(__wrap_posix_memalign(&q, 4096, 100) == 0 && ((uintptr_t)q & 4095) == 0);
    CHECK(__wrap_posix_memalign(&q, 3, 100) == EINVAL);
    CHECK(__wrap_calloc((size_t)1 << 62, 8) == NULL && errno == ENOMEM);

    pthread_t t[4];
    for (int i = 0; i < 4; i++)
        t[i] = start(tlsThread, (void*)(intptr_t)(i + 1));
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    CHECK(tlsControl.object.index == 1 && tlsControl2.object.index == 2);
    printf("ps5malloc: bitmap (partial unmaps, rounding, 2 GiB edge), foreign free, emulated TLS OK\n");
}

int main(void)
{
    test_mutex();
    test_timedlock();
    test_recursive();
    test_cond();
    test_timedwait();
    test_rwlock();
    test_malloc();
    ps5locks_stats_t st;
    ps5locks_stats(&st);
    printf("stats: fast %d, spin %d, mutex parks %llu, cond waits %llu, rwlock parks %llu, recursive %llu\n", st.fast, st.spin,
        (unsigned long long)st.mutex_parks, (unsigned long long)st.cond_waits, (unsigned long long)st.rwlock_parks,
        (unsigned long long)st.recursive_mutexes);
    printf("RESULT host-test PASS\n");
    return 0;
}
