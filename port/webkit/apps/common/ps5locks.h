/*
 * ps5locks.h - pthread mutexes, condition variables and rwlocks on futex
 * words (_umtx_op), linked in with --wrap. See ps5locks.c.
 */
#ifndef PS5LOCKS_H
#define PS5LOCKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int fast;                  /* 1: futex locks, 0: the console's libc (the off file exists) */
    int spin;                  /* spin iterations before a waiter sleeps */
    uint64_t mutex_parks;      /* futex waits on a mutex */
    uint64_t cond_waits;       /* condition variable waits */
    uint64_t rwlock_parks;     /* futex waits on an rwlock */
    uint64_t recursive_mutexes; /* recursive mutexes made (each one a small malloc) */
} ps5locks_stats_t;

void ps5locks_stats(ps5locks_stats_t* out);

/* Spin iterations before sleeping (default 100; lockbench sets others). */
extern int ps5locks_spin;

#ifdef __cplusplus
}
#endif

#endif
