/*
 * ps5clock.c: clock_gettime() for CLOCK_MONOTONIC and CLOCK_REALTIME from
 * the TSC, in ps5wk-child (--wrap=clock_gettime, apps/CMakeLists.txt).
 *
 * Why: the console's clock_gettime is a system call, ~1.1 us per call (the
 * WebProcess's "[probe] clock_gettime" line), and a WebProcess reads the
 * clock all the time: WebKit's timers and MonotonicTime::now, libpas's epoch
 * in its page decisions, GLib's main loop, performance.now(), Date.now().
 * That was 2-3% of the WebProcess main thread's CPU in Speedometer 3.1
 * (README: "Speedometer").
 *
 * How: for the first 250 ms the kernel clock is read as before while the
 * TSC is timed against it; then monotonic time is an anchor plus TSC ticks
 * times a rate. Every 2 s one caller reads the kernel clock again, refines
 * the rate over the whole time since the start, and steers the next 2 s
 * (by at most 0.2%) to absorb any difference, so the clock stays continuous,
 * monotonic and within microseconds of the kernel's; a difference over
 * 50 ms (the process was suspended) is jumped forward at once. Realtime is
 * monotonic time plus the kernel's realtime-minus-monotonic offset, re-read
 * at each check (a clock change shows within 2 s). Other clocks (thread CPU
 * time...) go to the kernel.
 *
 * Off switch: /data/ps5webkit/no-tsc-clock (checked at the first call): the
 * kernel clock for the life of the process.
 */
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

int __real_clock_gettime(clockid_t, struct timespec*);

#define PS5CLOCK_OFF_FILE "/data/ps5webkit/no-tsc-clock"
#define CALIBRATE_NS 250000000LL
#define CHECK_NS 2000000000LL
#define MAX_STEER 0.002
#define JUMP_NS 50000000LL

enum { STATE_NEW, STATE_CALIBRATING, STATE_TSC, STATE_OFF };

typedef struct {
    uint64_t tsc;       /* anchor: TSC ticks */
    int64_t ns;         /* anchor: monotonic ns */
    uint64_t mult;      /* ns per tick << 32, steered, for the first steerTicks */
    uint64_t baseMult;  /* ns per tick << 32 after that */
    uint64_t steerTicks;
    int64_t realOffset; /* realtime - monotonic, ns */
} Params;

static _Atomic int state = STATE_NEW;
static _Atomic unsigned sequence; /* odd while params change */
static Params params;
static _Atomic int updating;
static _Atomic uint64_t nextCheckTsc;
static uint64_t startTsc; /* calibration start (written before startNs) */
static _Atomic int64_t startNs;
static double ticksPerNs;

/* lfence on both sides: rdtsc is not ordered with the loads of the
 * sequence around it otherwise (a counter read after the check would pair
 * with old parameters and could go back by the steering difference). */
static inline uint64_t readTsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static inline int64_t toNs(const struct timespec* ts)
{
    return (int64_t)ts->tv_sec * 1000000000LL + ts->tv_nsec;
}

static inline void fromNs(int64_t ns, struct timespec* ts)
{
    ts->tv_sec = (time_t)(ns / 1000000000LL);
    ts->tv_nsec = (long)(ns % 1000000000LL);
}

static inline int64_t nsAt(const Params* p, uint64_t tsc)
{
    int64_t ticks = (int64_t)(tsc - p->tsc);
    if (ticks < 0) /* another core's TSC a few ticks behind the anchor */
        ticks = 0;
    uint64_t steered = (uint64_t)ticks < p->steerTicks ? (uint64_t)ticks : p->steerTicks;
    unsigned __int128 scaled = (unsigned __int128)steered * p->mult + (unsigned __int128)((uint64_t)ticks - steered) * p->baseMult;
    return p->ns + (int64_t)(scaled >> 32);
}

/* The kernel's monotonic time with the TSC read around it (its midpoint). */
static int64_t kernelPair(uint64_t* tsc)
{
    struct timespec ts;
    uint64_t before = readTsc();
    __real_clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t after = readTsc();
    *tsc = before + (after - before) / 2;
    return toNs(&ts);
}

static int64_t realOffsetNow(int64_t monotonicNs)
{
    struct timespec ts;
    __real_clock_gettime(CLOCK_REALTIME, &ts);
    return toNs(&ts) - monotonicNs;
}

/* Publishes next, anchored at the TSC read here, inside the change: a
 * caller that read the TSC after it retries with the new parameters, one
 * that read it before keeps the old ones at a time before the anchor, so
 * the clock never goes back. kernelNs/kernelTsc: a jump to the kernel's
 * time (> 0), else the old parameters' time at the anchor. */
static void publish(Params* next, int64_t kernelNs, uint64_t kernelTsc)
{
    atomic_fetch_add_explicit(&sequence, 1, memory_order_acq_rel);
    atomic_thread_fence(memory_order_seq_cst);
    uint64_t tsc = readTsc();
    next->tsc = tsc;
    if (kernelNs)
        next->ns = kernelNs + (int64_t)((double)(tsc - kernelTsc) / ticksPerNs);
    else
        next->ns = nsAt(&params, tsc);
    params = *next;
    atomic_fetch_add_explicit(&sequence, 1, memory_order_release);
}

static void readParams(Params* out, uint64_t* tsc)
{
    for (;;) {
        unsigned before = atomic_load_explicit(&sequence, memory_order_acquire);
        if (before & 1)
            continue;
        *out = params;
        *tsc = readTsc();
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&sequence, memory_order_relaxed) == before)
            return;
    }
}

static uint64_t multFor(double nsPerTick)
{
    return (uint64_t)(nsPerTick * 4294967296.0);
}

/* The calibration is done: start the TSC clock. */
static void startTscClock(void)
{
    uint64_t tsc;
    int64_t ns = kernelPair(&tsc);
    ticksPerNs = (double)(tsc - startTsc) / (double)(ns - atomic_load(&startNs));
    if (!(ticksPerNs > 0.05 && ticksPerNs < 20)) { /* 50 MHz-20 GHz: else something is wrong */
        atomic_store(&state, STATE_OFF);
        return;
    }
    uint64_t mult = multFor(1.0 / ticksPerNs);
    Params p = { tsc, ns, mult, mult, 0, realOffsetNow(ns) };
    publish(&p, ns, tsc);
    atomic_store_explicit(&nextCheckTsc, tsc + (uint64_t)(ticksPerNs * CHECK_NS), memory_order_relaxed);
    atomic_store_explicit(&state, STATE_TSC, memory_order_release);
}

/* Every CHECK_NS: refine the rate, steer towards the kernel clock. */
static void check(void)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong(&updating, &expected, 1))
        return;
    uint64_t tsc;
    int64_t kernelNs = kernelPair(&tsc);
    Params current, next;
    uint64_t unused;
    readParams(&current, &unused);
    int64_t ourNs = nsAt(&current, tsc);
    double rate = (double)(tsc - startTsc) / (double)(kernelNs - atomic_load(&startNs));
    if (rate > 0.05 && rate < 20)
        ticksPerNs = rate;
    int64_t difference = kernelNs - ourNs; /* > 0: behind the kernel */
    double steer = (double)difference / (double)CHECK_NS;
    if (steer > MAX_STEER)
        steer = MAX_STEER;
    if (steer < -MAX_STEER)
        steer = -MAX_STEER;
    next.baseMult = multFor(1.0 / ticksPerNs);
    next.mult = difference > JUMP_NS ? next.baseMult : multFor((1.0 / ticksPerNs) * (1.0 + steer));
    next.steerTicks = (uint64_t)(ticksPerNs * CHECK_NS);
    next.realOffset = realOffsetNow(kernelNs);
    publish(&next, difference > JUMP_NS ? kernelNs : 0, tsc);
    atomic_store_explicit(&nextCheckTsc, tsc + (uint64_t)(ticksPerNs * CHECK_NS), memory_order_relaxed);
    atomic_store(&updating, 0);
}

static int kernelClock(clockid_t clock, struct timespec* ts)
{
    int s = atomic_load_explicit(&state, memory_order_acquire);
    if (s == STATE_NEW) {
        int expected = STATE_NEW;
        if (atomic_compare_exchange_strong(&state, &expected, STATE_CALIBRATING)) {
            if (!access(PS5CLOCK_OFF_FILE, F_OK)) {
                atomic_store(&state, STATE_OFF);
            } else {
                uint64_t tsc;
                int64_t ns = kernelPair(&tsc);
                startTsc = tsc;
                atomic_store_explicit(&startNs, ns, memory_order_release);
            }
        }
    } else if (s == STATE_CALIBRATING && clock == CLOCK_MONOTONIC) {
        struct timespec now;
        __real_clock_gettime(CLOCK_MONOTONIC, &now);
        int expected = 0;
        int64_t start = atomic_load_explicit(&startNs, memory_order_acquire);
        if (start && toNs(&now) - start >= CALIBRATE_NS && atomic_compare_exchange_strong(&updating, &expected, 1)) {
            startTscClock();
            atomic_store(&updating, 0);
        }
    }
    return __real_clock_gettime(clock, ts);
}

int __wrap_clock_gettime(clockid_t clock, struct timespec* ts)
{
    int realtime;
    switch (clock) {
    case CLOCK_MONOTONIC:
#ifdef CLOCK_MONOTONIC_PRECISE
    case CLOCK_MONOTONIC_PRECISE:
#endif
#ifdef CLOCK_MONOTONIC_FAST
    case CLOCK_MONOTONIC_FAST:
#endif
        realtime = 0;
        break;
    case CLOCK_REALTIME:
#ifdef CLOCK_REALTIME_PRECISE
    case CLOCK_REALTIME_PRECISE:
#endif
#ifdef CLOCK_REALTIME_FAST
    case CLOCK_REALTIME_FAST:
#endif
        realtime = 1;
        break;
    default:
        return __real_clock_gettime(clock, ts);
    }
    if (!ts || atomic_load_explicit(&state, memory_order_acquire) != STATE_TSC)
        return kernelClock(clock, ts);
    Params p;
    uint64_t tsc;
    readParams(&p, &tsc);
    if (tsc >= atomic_load_explicit(&nextCheckTsc, memory_order_relaxed)) {
        check();
        readParams(&p, &tsc);
    }
    int64_t ns = nsAt(&p, tsc);
    fromNs(realtime ? ns + p.realOffset : ns, ts);
    return 0;
}
