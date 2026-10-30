/*
 * ps5locks.c - pthread mutexes, condition variables and rwlocks on futex
 * words, for the payloads and the title (linked with --wrap=pthread_mutex_lock,
 * ...; apps/CMakeLists.txt).
 *
 * Why: the console's pthread mutex hands a contended lock over at 4-20 us
 * (test/lockbench: 17 ns uncontended, 4.2/13.4/20.8 us per lock with 2/4/6
 * threads), so every GMutex, GCond, std::mutex and WTF ParkingLot park in
 * glib, GStreamer, libsoup and WebKit convoys as soon as two threads meet.
 * libpas's own lock (a futex on _umtx_op) does not, and these are the same
 * kind of lock: the unlocking thread can take the lock straight back
 * (no hand-over), a waiter spins briefly, then sleeps in
 * _umtx_op(UMTX_OP_WAIT_UINT_PRIVATE) and is woken by UMTX_OP_WAKE_PRIVATE.
 *
 * Representation (pthread_mutex_t, pthread_cond_t and pthread_rwlock_t are
 * one pointer on this libc, and their static initializers are NULL, or 1 for
 * PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP): the 8 bytes are used directly as
 * two 32-bit words, w[0] at the lower address.
 *
 *   mutex   w[0] kind: 0 or 1 = plain (normal, default, errorcheck, adaptive),
 *                4 = plain and process-shared; w[1] the futex: 0 free,
 *                1 locked, 2 locked with sleepers.
 *           recursive: the 8 bytes are a pointer to a malloc'd rmutex | 2.
 *   cond    w[0] sequence (futex), w[1] clock (bits 0-7), process-shared
 *           (bit 8), sleeping waiters (bits 16-31).
 *   rwlock  w[0] readers (bits 0-29), sleepers (bit 30), writer (bit 31);
 *           w[1] process-shared (bit 0). Readers are preferred (a thread
 *           that already reads can read again while a writer waits).
 *   mutexattr / condattr: our own encoding (magic in the upper half), so no
 *           attribute getter of the console's libc is needed.
 *
 * Every function that touches these objects must be wrapped, or it would
 * read our words as a libc pointer: apps/common/check-lock-imports.sh fails
 * the build when the executable imports one that isn't.
 *
 * Semantics kept: recursive mutexes (owner + count, EPERM for a non-owner's
 * unlock), trylock, timed waits against the condition variable's clock
 * (pthread_condattr_setclock: glib's GCond uses CLOCK_MONOTONIC) and
 * CLOCK_REALTIME for mutex/rwlock timed locks, process-shared objects
 * (non-private futex ops). Not kept: errorcheck mutexes behave as normal
 * ones (this libc's PTHREAD_MUTEX_DEFAULT is ERRORCHECK, so the two can't be
 * told apart: a relock deadlocks instead of returning EDEADLK, an unlock of
 * an unlocked mutex is a no-op); priority protocols and robust mutexes
 * (nothing here imports their attribute calls).
 *
 * Off switch: if PS5LOCKS_OFF_FILE exists when the first wrapped call is
 * made (before main, one thread), every call goes to the console's libc for
 * the life of the process (A/B tests).
 */
#include "ps5locks.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PS5LOCKS_OFF_FILE
#define PS5LOCKS_OFF_FILE "/data/ps5webkit/no-fastlock"
#endif

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* -- futex ------------------------------------------------------------------ */

#ifdef PS5LOCKS_HOST_TEST
/* macOS, for logic tests on the Mac (test/lockbench/host-test.c). */
extern int __ulock_wait(uint32_t operation, void* addr, uint64_t value, uint32_t timeout_us);
extern int __ulock_wake(uint32_t operation, void* addr, uint64_t wake_value);
#define UL_COMPARE_AND_WAIT 1
#define ULF_WAKE_ALL 0x00000100

static int futex_wait(uint32_t* addr, uint32_t value, const struct timespec* relative, int shared)
{
    (void)shared;
    uint32_t us = 0;
    if (relative) {
        uint64_t t = (uint64_t)relative->tv_sec * 1000000u + (uint64_t)relative->tv_nsec / 1000u;
        us = t == 0 ? 1 : t > 0xffffffffu ? 0xffffffffu : (uint32_t)t;
    }
    int r = __ulock_wait(UL_COMPARE_AND_WAIT, addr, value, us);
    if (r < 0 && errno == ETIMEDOUT)
        return ETIMEDOUT;
    return 0;
}

static void futex_wake(uint32_t* addr, int count, int shared)
{
    (void)shared;
    __ulock_wake(UL_COMPARE_AND_WAIT | (count > 1 ? ULF_WAKE_ALL : 0), addr, 0);
}

static inline void cpu_relax(void) { __asm__ __volatile__("yield"); }
#else
#include <sys/types.h>
#include <sys/umtx.h>

/* Waits while *addr == value, at most relative (NULL: forever). 0 when
 * woken (or the value had changed, or a signal: callers loop), ETIMEDOUT. */
static int futex_wait(uint32_t* addr, uint32_t value, const struct timespec* relative, int shared)
{
    int saved = errno;
    /* A relative timeout: uaddr is the size of the struct, uaddr2 points to it. */
    int r = _umtx_op(addr, shared ? UMTX_OP_WAIT_UINT : UMTX_OP_WAIT_UINT_PRIVATE, value,
        relative ? (void*)sizeof(*relative) : NULL, (void*)relative);
    int result = r == -1 && errno == ETIMEDOUT ? ETIMEDOUT : 0;
    errno = saved;
    return result;
}

static void futex_wake(uint32_t* addr, int count, int shared)
{
    int saved = errno;
    _umtx_op(addr, shared ? UMTX_OP_WAKE : UMTX_OP_WAKE_PRIVATE, (u_long)count, NULL, NULL);
    errno = saved;
}

static inline void cpu_relax(void) { __builtin_ia32_pause(); }
#endif

static inline uint32_t load(uint32_t* p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
static inline uint32_t xchg(uint32_t* p, uint32_t v) { return __atomic_exchange_n(p, v, __ATOMIC_ACQ_REL); }
static inline int cas(uint32_t* p, uint32_t* expected, uint32_t desired)
{
    return __atomic_compare_exchange_n(p, expected, desired, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

/* -- mode and statistics ---------------------------------------------------- */

int ps5locks_spin = 100;
static int mode; /* 0 undecided, 1 fast, 2 the console's libc */
static uint64_t mutexParks, condWaits, rwlockParks, recursiveMutexes;

static __attribute__((noinline)) int decide(void)
{
#ifdef PS5LOCKS_HOST_TEST
    int m = 1;
#else
    int saved = errno;
    int m = access(PS5LOCKS_OFF_FILE, F_OK) == 0 ? 2 : 1;
    errno = saved;
#endif
    int expected = 0;
    __atomic_compare_exchange_n(&mode, &expected, m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return __atomic_load_n(&mode, __ATOMIC_ACQUIRE) == 1;
}

static inline int fast(void)
{
    int m = __atomic_load_n(&mode, __ATOMIC_RELAXED);
    if (LIKELY(m == 1))
        return 1;
    if (m == 2)
        return 0;
    return decide();
}

static inline void count(uint64_t* counter)
{
    __atomic_fetch_add(counter, 1, __ATOMIC_RELAXED);
}

void ps5locks_stats(ps5locks_stats_t* out)
{
    out->fast = fast();
    out->spin = ps5locks_spin;
    out->mutex_parks = __atomic_load_n(&mutexParks, __ATOMIC_RELAXED);
    out->cond_waits = __atomic_load_n(&condWaits, __ATOMIC_RELAXED);
    out->rwlock_parks = __atomic_load_n(&rwlockParks, __ATOMIC_RELAXED);
    out->recursive_mutexes = __atomic_load_n(&recursiveMutexes, __ATOMIC_RELAXED);
}

/* The console's functions (--wrap). */
#ifdef PS5LOCKS_HOST_TEST
#define REAL(name, ...) (abort(), 0)
#else
#define REAL(name, ...) __real_##name(__VA_ARGS__)
int __real_pthread_mutex_init(pthread_mutex_t*, const pthread_mutexattr_t*);
int __real_pthread_mutex_destroy(pthread_mutex_t*);
int __real_pthread_mutex_lock(pthread_mutex_t*);
int __real_pthread_mutex_trylock(pthread_mutex_t*);
int __real_pthread_mutex_timedlock(pthread_mutex_t*, const struct timespec*);
int __real_pthread_mutex_unlock(pthread_mutex_t*);
int __real_pthread_mutexattr_init(pthread_mutexattr_t*);
int __real_pthread_mutexattr_destroy(pthread_mutexattr_t*);
int __real_pthread_mutexattr_settype(pthread_mutexattr_t*, int);
int __real_pthread_mutexattr_gettype(const pthread_mutexattr_t*, int*);
int __real_pthread_cond_init(pthread_cond_t*, const pthread_condattr_t*);
int __real_pthread_cond_destroy(pthread_cond_t*);
int __real_pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
int __real_pthread_cond_timedwait(pthread_cond_t*, pthread_mutex_t*, const struct timespec*);
int __real_pthread_cond_signal(pthread_cond_t*);
int __real_pthread_cond_broadcast(pthread_cond_t*);
int __real_pthread_condattr_init(pthread_condattr_t*);
int __real_pthread_condattr_destroy(pthread_condattr_t*);
int __real_pthread_condattr_setclock(pthread_condattr_t*, clockid_t);
int __real_pthread_condattr_getclock(const pthread_condattr_t*, clockid_t*);
int __real_pthread_rwlock_init(pthread_rwlock_t*, const pthread_rwlockattr_t*);
int __real_pthread_rwlock_destroy(pthread_rwlock_t*);
int __real_pthread_rwlock_rdlock(pthread_rwlock_t*);
int __real_pthread_rwlock_tryrdlock(pthread_rwlock_t*);
int __real_pthread_rwlock_timedrdlock(pthread_rwlock_t*, const struct timespec*);
int __real_pthread_rwlock_wrlock(pthread_rwlock_t*);
int __real_pthread_rwlock_trywrlock(pthread_rwlock_t*);
int __real_pthread_rwlock_timedwrlock(pthread_rwlock_t*, const struct timespec*);
int __real_pthread_rwlock_unlock(pthread_rwlock_t*);
#endif

/* -- time ------------------------------------------------------------------- */

static int valid_time(const struct timespec* t)
{
    return t && t->tv_nsec >= 0 && t->tv_nsec < 1000000000L;
}

/* relative = absolute - now on clock; 0 if that is not in the future. */
static int relative_from_absolute(clockid_t clock, const struct timespec* absolute, struct timespec* relative)
{
    struct timespec now;
    clock_gettime(clock, &now);
    relative->tv_sec = absolute->tv_sec - now.tv_sec;
    relative->tv_nsec = absolute->tv_nsec - now.tv_nsec;
    if (relative->tv_nsec < 0) {
        relative->tv_nsec += 1000000000L;
        relative->tv_sec--;
    }
    return relative->tv_sec > 0 || (relative->tv_sec == 0 && relative->tv_nsec > 0);
}

/* -- plain lock on one futex word ------------------------------------------- */

static inline int plain_trylock(uint32_t* f)
{
    uint32_t expected = 0;
    return cas(f, &expected, 1);
}

/* absolute: NULL = no timeout (CLOCK_REALTIME otherwise). */
static __attribute__((noinline)) int plain_lock_slow(uint32_t* f, int shared, const struct timespec* absolute)
{
    for (int i = 0; i < ps5locks_spin; i++) {
        if (load(f) == 0 && plain_trylock(f))
            return 0;
        cpu_relax();
    }
    while (xchg(f, 2) != 0) {
        struct timespec relative;
        if (absolute && !relative_from_absolute(CLOCK_REALTIME, absolute, &relative))
            return ETIMEDOUT;
        count(&mutexParks);
        futex_wait(f, 2, absolute ? &relative : NULL, shared);
    }
    return 0;
}

static inline void plain_lock(uint32_t* f, int shared)
{
    if (LIKELY(plain_trylock(f)))
        return;
    plain_lock_slow(f, shared, NULL);
}

/* After a condition wait: there may be other sleepers, so lock as 2. */
static void plain_lock_contended(uint32_t* f, int shared)
{
    while (xchg(f, 2) != 0) {
        count(&mutexParks);
        futex_wait(f, 2, NULL, shared);
    }
}

static inline void plain_unlock(uint32_t* f, int shared)
{
    if (UNLIKELY(xchg(f, 0) == 2))
        futex_wake(f, 1, shared);
}

/* -- mutex ------------------------------------------------------------------ */

typedef struct {
    uint32_t futex;
    uint32_t count;  /* only the owner reads or writes it */
    uintptr_t owner; /* pthread_self() of the owner, 0 when free */
    int shared;
} rmutex_t;

static inline uintptr_t self_id(void) { return (uintptr_t)pthread_self(); }
static inline uintptr_t owner_of(rmutex_t* r) { return __atomic_load_n(&r->owner, __ATOMIC_RELAXED); }
static inline void set_owner(rmutex_t* r, uintptr_t owner) { __atomic_store_n(&r->owner, owner, __ATOMIC_RELAXED); }

#define KIND_SHARED 4u
#define KIND_RECURSIVE 2u

#define ATTR_MAGIC 0x50533541u /* our attribute encoding, upper half */

static inline uint64_t word(const void* object)
{
    return __atomic_load_n((const uint64_t*)object, __ATOMIC_RELAXED);
}

static inline rmutex_t* rmutex_of(uint64_t w)
{
    return (rmutex_t*)(uintptr_t)(w & ~(uint64_t)3);
}

int __wrap_pthread_mutexattr_init(pthread_mutexattr_t* attr)
{
    if (!fast())
        return REAL(pthread_mutexattr_init, attr);
    *(uint64_t*)attr = (uint64_t)ATTR_MAGIC << 32 | (uint32_t)PTHREAD_MUTEX_DEFAULT;
    return 0;
}

int __wrap_pthread_mutexattr_destroy(pthread_mutexattr_t* attr)
{
    if (!fast())
        return REAL(pthread_mutexattr_destroy, attr);
    *(uint64_t*)attr = 0;
    return 0;
}

int __wrap_pthread_mutexattr_settype(pthread_mutexattr_t* attr, int type)
{
    if (!fast())
        return REAL(pthread_mutexattr_settype, attr, type);
    uint64_t w = *(uint64_t*)attr;
    if ((uint32_t)(w >> 32) != ATTR_MAGIC || type < 0 || type > 255)
        return EINVAL;
    *(uint64_t*)attr = (w & ~(uint64_t)0xff) | (uint32_t)type;
    return 0;
}

int __wrap_pthread_mutexattr_gettype(const pthread_mutexattr_t* attr, int* type)
{
    if (!fast())
        return REAL(pthread_mutexattr_gettype, attr, type);
    uint64_t w = *(const uint64_t*)attr;
    if ((uint32_t)(w >> 32) != ATTR_MAGIC)
        return EINVAL;
    *type = (int)(w & 0xff);
    return 0;
}

int __wrap_pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr)
{
    if (!fast())
        return REAL(pthread_mutex_init, mutex, attr);
    int type = PTHREAD_MUTEX_DEFAULT, shared = 0;
    if (attr) {
        uint64_t w = *(const uint64_t*)attr;
        if ((uint32_t)(w >> 32) == ATTR_MAGIC) {
            type = (int)(w & 0xff);
            shared = (int)(w >> 8 & 1);
        }
    }
    if (type == PTHREAD_MUTEX_RECURSIVE) {
        rmutex_t* r = (rmutex_t*)calloc(1, sizeof(rmutex_t));
        if (!r)
            return ENOMEM;
        r->shared = shared;
        count(&recursiveMutexes);
        __atomic_store_n((uint64_t*)mutex, (uint64_t)(uintptr_t)r | KIND_RECURSIVE, __ATOMIC_RELEASE);
        return 0;
    }
    __atomic_store_n((uint64_t*)mutex, shared ? KIND_SHARED : 0, __ATOMIC_RELEASE);
    return 0;
}

int __wrap_pthread_mutex_destroy(pthread_mutex_t* mutex)
{
    if (!fast())
        return REAL(pthread_mutex_destroy, mutex);
    uint64_t w = word(mutex);
    if (w & KIND_RECURSIVE) {
        rmutex_t* r = rmutex_of(w);
        if (owner_of(r))
            return EBUSY;
        free(r);
    } else if (w >> 32)
        return EBUSY;
    __atomic_store_n((uint64_t*)mutex, 0, __ATOMIC_RELEASE);
    return 0;
}

static int rmutex_lock(rmutex_t* r, const struct timespec* absolute, int try)
{
    uintptr_t self = self_id();
    if (owner_of(r) == self) {
        if (r->count == UINT32_MAX)
            return EAGAIN;
        r->count++;
        return 0;
    }
    if (!plain_trylock(&r->futex)) {
        if (try)
            return EBUSY;
        int e = plain_lock_slow(&r->futex, r->shared, absolute);
        if (e)
            return e;
    }
    set_owner(r, self);
    r->count = 1;
    return 0;
}

static int rmutex_unlock(rmutex_t* r)
{
    if (owner_of(r) != self_id())
        return EPERM;
    if (--r->count == 0) {
        set_owner(r, 0);
        plain_unlock(&r->futex, r->shared);
    }
    return 0;
}

int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex)
{
    if (UNLIKELY(!fast()))
        return REAL(pthread_mutex_lock, mutex);
    uint64_t w = word(mutex);
    if (UNLIKELY(w & KIND_RECURSIVE))
        return rmutex_lock(rmutex_of(w), NULL, 0);
    uint32_t* f = (uint32_t*)mutex + 1;
    if (LIKELY(plain_trylock(f)))
        return 0;
    return plain_lock_slow(f, (int)(w & KIND_SHARED), NULL);
}

int __wrap_pthread_mutex_trylock(pthread_mutex_t* mutex)
{
    if (UNLIKELY(!fast()))
        return REAL(pthread_mutex_trylock, mutex);
    uint64_t w = word(mutex);
    if (UNLIKELY(w & KIND_RECURSIVE))
        return rmutex_lock(rmutex_of(w), NULL, 1);
    return plain_trylock((uint32_t*)mutex + 1) ? 0 : EBUSY;
}

int __wrap_pthread_mutex_timedlock(pthread_mutex_t* mutex, const struct timespec* absolute)
{
    if (UNLIKELY(!fast()))
        return REAL(pthread_mutex_timedlock, mutex, absolute);
    uint64_t w = word(mutex);
    if (w & KIND_RECURSIVE) {
        rmutex_t* r = rmutex_of(w);
        if (owner_of(r) != self_id() && !valid_time(absolute))
            return EINVAL;
        return rmutex_lock(r, absolute, 0);
    }
    uint32_t* f = (uint32_t*)mutex + 1;
    if (plain_trylock(f))
        return 0;
    if (!valid_time(absolute))
        return EINVAL;
    return plain_lock_slow(f, (int)(w & KIND_SHARED), absolute);
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t* mutex)
{
    if (UNLIKELY(!fast()))
        return REAL(pthread_mutex_unlock, mutex);
    uint64_t w = word(mutex);
    if (UNLIKELY(w & KIND_RECURSIVE))
        return rmutex_unlock(rmutex_of(w));
    plain_unlock((uint32_t*)mutex + 1, (int)(w & KIND_SHARED));
    return 0;
}

/* -- condition variable ----------------------------------------------------- */

#define COND_SHARED 0x100u
#define COND_WAITER 0x10000u

int __wrap_pthread_condattr_init(pthread_condattr_t* attr)
{
    if (!fast())
        return REAL(pthread_condattr_init, attr);
    *(uint64_t*)attr = (uint64_t)ATTR_MAGIC << 32 | (uint32_t)CLOCK_REALTIME;
    return 0;
}

int __wrap_pthread_condattr_destroy(pthread_condattr_t* attr)
{
    if (!fast())
        return REAL(pthread_condattr_destroy, attr);
    *(uint64_t*)attr = 0;
    return 0;
}

int __wrap_pthread_condattr_setclock(pthread_condattr_t* attr, clockid_t clock)
{
    if (!fast())
        return REAL(pthread_condattr_setclock, attr, clock);
    uint64_t w = *(uint64_t*)attr;
    if ((uint32_t)(w >> 32) != ATTR_MAGIC || (int)clock < 0 || (int)clock > 255)
        return EINVAL;
    struct timespec probe;
    if (clock_gettime(clock, &probe))
        return EINVAL;
    *(uint64_t*)attr = (w & ~(uint64_t)0xff) | (uint32_t)clock;
    return 0;
}

int __wrap_pthread_condattr_getclock(const pthread_condattr_t* attr, clockid_t* clock)
{
    if (!fast())
        return REAL(pthread_condattr_getclock, attr, clock);
    uint64_t w = *(const uint64_t*)attr;
    if ((uint32_t)(w >> 32) != ATTR_MAGIC)
        return EINVAL;
    *clock = (clockid_t)(w & 0xff);
    return 0;
}

int __wrap_pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr)
{
    if (!fast())
        return REAL(pthread_cond_init, cond, attr);
    uint32_t flags = CLOCK_REALTIME;
    if (attr) {
        uint64_t w = *(const uint64_t*)attr;
        if ((uint32_t)(w >> 32) == ATTR_MAGIC)
            flags = (uint32_t)(w & 0x1ff);
    }
    uint32_t* w = (uint32_t*)cond;
    __atomic_store_n(&w[0], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&w[1], flags, __ATOMIC_RELEASE);
    return 0;
}

int __wrap_pthread_cond_destroy(pthread_cond_t* cond)
{
    if (!fast())
        return REAL(pthread_cond_destroy, cond);
    /* Nothing to free; the words are left alone (a waiter that is just
     * leaving still decrements the count). */
    return 0;
}

static int cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* absolute)
{
    uint32_t* w = (uint32_t*)cond;
    uint32_t flags = __atomic_load_n(&w[1], __ATOMIC_RELAXED);
    int shared = (flags & COND_SHARED) != 0;
    clockid_t clock = (clockid_t)(flags & 0xff);
    if (absolute && !valid_time(absolute))
        return EINVAL;

    uint64_t m = word(mutex);
    rmutex_t* r = (m & KIND_RECURSIVE) ? rmutex_of(m) : NULL;
    if (r && owner_of(r) != self_id())
        return EPERM;

    count(&condWaits);
    __atomic_fetch_add(&w[1], COND_WAITER, __ATOMIC_SEQ_CST);
    uint32_t sequence = __atomic_load_n(&w[0], __ATOMIC_SEQ_CST);

    uint32_t saved = 0;
    if (r) {
        saved = r->count;
        r->count = 0;
        set_owner(r, 0);
        plain_unlock(&r->futex, r->shared);
    } else
        plain_unlock((uint32_t*)mutex + 1, (int)(m & KIND_SHARED));

    int result = 0;
    if (absolute) {
        struct timespec relative;
        if (!relative_from_absolute(clock, absolute, &relative))
            result = ETIMEDOUT;
        else
            result = futex_wait(&w[0], sequence, &relative, shared);
    } else
        futex_wait(&w[0], sequence, NULL, shared);
    __atomic_fetch_sub(&w[1], COND_WAITER, __ATOMIC_SEQ_CST);

    if (r) {
        plain_lock_contended(&r->futex, r->shared);
        set_owner(r, self_id());
        r->count = saved;
    } else
        plain_lock_contended((uint32_t*)mutex + 1, (int)(m & KIND_SHARED));
    return result;
}

int __wrap_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex)
{
    if (UNLIKELY(!fast()))
        return REAL(pthread_cond_wait, cond, mutex);
    return cond_wait(cond, mutex, NULL);
}

int __wrap_pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* absolute)
{
    if (UNLIKELY(!fast()))
        return REAL(pthread_cond_timedwait, cond, mutex, absolute);
    if (!absolute)
        return EINVAL;
    return cond_wait(cond, mutex, absolute);
}

static int cond_wake(pthread_cond_t* cond, int all)
{
    uint32_t* w = (uint32_t*)cond;
    __atomic_fetch_add(&w[0], 1, __ATOMIC_SEQ_CST);
    uint32_t flags = __atomic_load_n(&w[1], __ATOMIC_SEQ_CST);
    if (flags >= COND_WAITER)
        futex_wake(&w[0], all ? INT_MAX : 1, (flags & COND_SHARED) != 0);
    return 0;
}

int __wrap_pthread_cond_signal(pthread_cond_t* cond)
{
    if (UNLIKELY(!fast()))
        return REAL(pthread_cond_signal, cond);
    return cond_wake(cond, 0);
}

int __wrap_pthread_cond_broadcast(pthread_cond_t* cond)
{
    if (UNLIKELY(!fast()))
        return REAL(pthread_cond_broadcast, cond);
    return cond_wake(cond, 1);
}

/* -- rwlock ----------------------------------------------------------------- */

#define RW_WRITER 0x80000000u
#define RW_SLEEPERS 0x40000000u
#define RW_READERS 0x3fffffffu

int __wrap_pthread_rwlock_init(pthread_rwlock_t* lock, const pthread_rwlockattr_t* attr)
{
    if (!fast())
        return REAL(pthread_rwlock_init, lock, attr);
    (void)attr; /* nothing here sets rwlock attributes */
    __atomic_store_n((uint64_t*)lock, 0, __ATOMIC_RELEASE);
    return 0;
}

int __wrap_pthread_rwlock_destroy(pthread_rwlock_t* lock)
{
    if (!fast())
        return REAL(pthread_rwlock_destroy, lock);
    if (load((uint32_t*)lock) & (RW_WRITER | RW_READERS))
        return EBUSY;
    return 0;
}

/* Sleeps until the state changes from s (setting the sleepers bit first). 0
 * to try again, ETIMEDOUT. */
static int rw_sleep(uint32_t* state, uint32_t s, int shared, const struct timespec* absolute)
{
    if (!(s & RW_SLEEPERS)) {
        if (!cas(state, &s, s | RW_SLEEPERS))
            return 0;
        s |= RW_SLEEPERS;
    }
    struct timespec relative;
    if (absolute && !relative_from_absolute(CLOCK_REALTIME, absolute, &relative))
        return ETIMEDOUT;
    count(&rwlockParks);
    futex_wait(state, s, absolute ? &relative : NULL, shared);
    return 0;
}

static int rw_rdlock(pthread_rwlock_t* lock, int try, const struct timespec* absolute)
{
    uint32_t* state = (uint32_t*)lock;
    int shared = (int)(load(state + 1) & 1);
    for (int spins = 0;;) {
        uint32_t s = load(state);
        if (!(s & RW_WRITER)) {
            if ((s & RW_READERS) == RW_READERS)
                return EAGAIN;
            if (cas(state, &s, s + 1))
                return 0;
            continue;
        }
        if (try)
            return EBUSY;
        if (absolute && !valid_time(absolute))
            return EINVAL;
        if (spins++ < ps5locks_spin) {
            cpu_relax();
            continue;
        }
        if (rw_sleep(state, s, shared, absolute))
            return ETIMEDOUT;
    }
}

static int rw_wrlock(pthread_rwlock_t* lock, int try, const struct timespec* absolute)
{
    uint32_t* state = (uint32_t*)lock;
    int shared = (int)(load(state + 1) & 1);
    for (int spins = 0;;) {
        uint32_t s = load(state);
        if (!(s & ~RW_SLEEPERS)) {
            if (cas(state, &s, s | RW_WRITER))
                return 0;
            continue;
        }
        if (try)
            return EBUSY;
        if (absolute && !valid_time(absolute))
            return EINVAL;
        if (spins++ < ps5locks_spin) {
            cpu_relax();
            continue;
        }
        if (rw_sleep(state, s, shared, absolute))
            return ETIMEDOUT;
    }
}

int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t* lock)
{
    if (UNLIKELY(!fast()))
        return REAL(pthread_rwlock_rdlock, lock);
    return rw_rdlock(lock, 0, NULL);
}

int __wrap_pthread_rwlock_tryrdlock(pthread_rwlock_t* lock)
{
    if (UNLIKELY(!fast()))
        return REAL(pthread_rwlock_tryrdlock, lock);
    return rw_rdlock(lock, 1, NULL);
}

int __wrap_pthread_rwlock_timedrdlock(pthread_rwlock_t* lock, const struct timespec* absolute)
{
    if (UNLIKELY(!fast()))
        return REAL(pthread_rwlock_timedrdlock, lock, absolute);
    return rw_rdlock(lock, 0, absolute);
}

int __wrap_pthread_rwlock_wrlock(pthread_rwlock_t* lock)
{
    if (UNLIKELY(!fast()))
        return REAL(pthread_rwlock_wrlock, lock);
    return rw_wrlock(lock, 0, NULL);
}

int __wrap_pthread_rwlock_trywrlock(pthread_rwlock_t* lock)
{
    if (UNLIKELY(!fast()))
        return REAL(pthread_rwlock_trywrlock, lock);
    return rw_wrlock(lock, 1, NULL);
}

int __wrap_pthread_rwlock_timedwrlock(pthread_rwlock_t* lock, const struct timespec* absolute)
{
    if (UNLIKELY(!fast()))
        return REAL(pthread_rwlock_timedwrlock, lock, absolute);
    return rw_wrlock(lock, 0, absolute);
}

int __wrap_pthread_rwlock_unlock(pthread_rwlock_t* lock)
{
    if (UNLIKELY(!fast()))
        return REAL(pthread_rwlock_unlock, lock);
    uint32_t* state = (uint32_t*)lock;
    int shared = (int)(load(state + 1) & 1);
    uint32_t s = load(state);
    if (s & RW_WRITER) {
        /* Readers can't enter while the writer holds it: all that can change
         * meanwhile is the sleepers bit. */
        if (xchg(state, 0) & RW_SLEEPERS)
            futex_wake(state, INT_MAX, shared);
        return 0;
    }
    if (!(s & RW_READERS))
        return EPERM;
    s = __atomic_sub_fetch(state, 1, __ATOMIC_ACQ_REL);
    /* The last reader out wakes the sleepers (writers). A reader that came in
     * meanwhile makes the cas fail and wakes them when it leaves. */
    while (!(s & RW_READERS) && (s & RW_SLEEPERS)) {
        if (cas(state, &s, s & ~RW_SLEEPERS)) {
            futex_wake(state, INT_MAX, shared);
            break;
        }
    }
    return 0;
}
