/* spike.h - shared logging and result reporting for the ps5webkit spikes.
 *
 * Payloads sent through elfldr get their stdout/stderr wired to the socket
 * that delivered them, so `tools/send.sh` shows the log on the Mac directly.
 * Registered titles (GL, Hmd2) have no such socket: call spike_log_connect()
 * with the Mac's address and everything printed through spike_log() also goes
 * to `tools/logserver.sh`.
 *
 * Every test result is one machine-readable line:
 *   RESULT <spike> <test> PASS|FAIL|SKIP <detail>
 * tools/results.sh collects them from saved logs into FEASIBILITY tables.
 */
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef SPIKE_NAME
#define SPIKE_NAME "spike"
#endif

/* Weak, not static: one set of sinks shared by every file of a spike (a
 * static copy per translation unit left Spike 1's tests, linked into the
 * Spike 2 title, logging nowhere but stdout). */
__attribute__((weak)) int spike_log_fd = -1;
__attribute__((weak)) int spike_log_file_fd = -1; /* titles: /data/ps5webkit/<title>.log */

/* Optional second sink: a TCP connection to tools/logserver.sh. */
static inline int spike_log_connect(const char *host, int port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1)
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    spike_log_fd = fd;
    return 0;
}

static inline double spike_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

__attribute__((format(printf, 1, 2))) static inline void spike_log(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof line - 2)
        n = (int)sizeof line - 2;
    line[n++] = '\n';
    fwrite(line, 1, (size_t)n, stdout);
    fflush(stdout);
    if (spike_log_file_fd >= 0 && write(spike_log_file_fd, line, (size_t)n) < 0) {
        close(spike_log_file_fd);
        spike_log_file_fd = -1;
    }
    if (spike_log_fd >= 0 && send(spike_log_fd, line, (size_t)n, 0) < 0) {
        close(spike_log_fd);
        spike_log_fd = -1;
    }
}

#define SPIKE_RESULT(test, status, ...)                                               \
    do {                                                                              \
        char _d[768];                                                                 \
        snprintf(_d, sizeof _d, __VA_ARGS__);                                         \
        spike_log("RESULT %s %s %s %s", SPIKE_NAME, (test), (status), _d);            \
    } while (0)

#define SPIKE_PASS(test, ...) SPIKE_RESULT(test, "PASS", __VA_ARGS__)
#define SPIKE_FAIL(test, ...) SPIKE_RESULT(test, "FAIL", __VA_ARGS__)
#define SPIKE_SKIP(test, ...) SPIKE_RESULT(test, "SKIP", __VA_ARGS__)

/* A popup on the TV: useful to see that a title started at all. */
typedef struct {
    char useless1[45];
    char message[3075];
} spike_notify_request_t;
int sceKernelSendNotificationRequest(int, spike_notify_request_t *, size_t, int) __attribute__((weak));

static inline void spike_notify(const char *msg)
{
    spike_notify_request_t req;
    memset(&req, 0, sizeof req);
    strncpy(req.message, msg, sizeof req.message - 1);
    if (sceKernelSendNotificationRequest)
        sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}
