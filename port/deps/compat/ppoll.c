/* ppoll(2) on PS5 payloads returns a positive errno-like value without
 * filling revents (13.60). Wrapped (-Wl,--wrap=ppoll) as poll() with the
 * signal mask applied around it (not atomic, which no caller here relies
 * on). glib is also configured not to use ppoll. */
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

int __wrap_ppoll(struct pollfd fds[], nfds_t nfds, const struct timespec *ts, const sigset_t *mask)
{
    int timeout = -1;
    sigset_t old;
    if (ts) {
        long long ms = (long long)ts->tv_sec * 1000 + (ts->tv_nsec + 999999) / 1000000;
        timeout = ms > INT_MAX ? INT_MAX : (int)ms;
    }
    if (mask)
        pthread_sigmask(SIG_SETMASK, mask, &old);
    int r = poll(fds, nfds, timeout);
    if (mask)
        pthread_sigmask(SIG_SETMASK, &old, NULL);
    return r;
}
