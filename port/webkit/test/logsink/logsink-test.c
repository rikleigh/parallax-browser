/*
 * logsink-test.c - the title's log (apps/title/titlelog.c) on the Mac,
 * against a log server that stops reading (run.sh drives it).
 *
 *   logsink-test queue|direct|private <port> <seconds> <dir>
 *
 * queue:  titlelog.c, as in the title (a log-sink file in <dir> applies).
 * direct: the old path, spike_log()'s blocking send() and stderr on the
 *         same socket, for comparison.
 *
 * Five threads log like the title's do: three through spike_log ("[app msg]
 * tN seq S"), one through stderr ("[wpe-ps5] stderr seq S") and one as the
 * relay of a child's output ("[web7] seq S"), 400 lines/s each. Every second
 * the slowest call of each kind is printed on the report stream (fd 3 if
 * open, else the original stdout).
 *
 * private: titlelog.c, private browsing switched on and off around stderr
 *         fragments, whole lines and relayed lines all marked PRIVMARK
 *         (none may reach the log), with unfinished stderr lines on both
 *         sides of each switch.
 */
#include "common/title.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>

void (*ps5wk_log_line_hook)(const char* line, size_t n) = 0; /* WPEProcessManagerPS5.cpp */
void ps5wk_titlelog_install(const char* dir);
void ps5wk_log_private(int on);

static FILE* report;
static _Atomic long long worstUS[3]; /* spike_log, stderr, relay */
static _Atomic long calls;
static volatile int running = 1;

static long long nowUS(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void noteWorst(int kind, long long us)
{
    long long seen = atomic_load(&worstUS[kind]);
    while (us > seen && !atomic_compare_exchange_weak(&worstUS[kind], &seen, us)) { }
    atomic_fetch_add(&calls, 1);
}

static const char padding[] = "padding padding padding padding padding padding padding padding padding";

static void* logger(void* data)
{
    int id = (int)(intptr_t)data; /* 0-2 spike_log, 3 stderr, 4 relay */
    for (long seq = 1; running; seq++) {
        long long t0 = nowUS();
        if (id < 3)
            spike_log("[app msg] t%d seq %ld %s", id, seq, padding);
        else if (id == 3)
            fprintf(stderr, "[wpe-ps5] stderr seq %ld %s\n", seq, padding);
        else {
            char line[256];
            int n = snprintf(line, sizeof line, "[web7] seq %ld %s\n", seq, padding);
            if (ps5wk_log_line_hook)
                ps5wk_log_line_hook(line, (size_t)n);
            else
                fputs(line, stderr);
        }
        noteWorst(id < 3 ? 0 : id - 2, nowUS() - t0);
        usleep(2500);
    }
    return NULL;
}

/* Unfinished stderr lines reach the pipe only when flushed (stderr is
 * line-buffered into it), as a library's raw write(2) would. */
static void errPart(const char* text)
{
    fputs(text, stderr);
    fflush(stderr);
}

static int privateRun(void)
{
    for (int round = 0; round < 3; round++) {
        errPart("[wpe-ps5] before-private fragment");
        ps5wk_log_private(1);
        errPart(" PRIVMARK-a ends the line from before\n");
        errPart("[wpe-ps5] PRIVMARK-b unfinished");
        spike_log("PRIVMARK-c a whole line");
        if (ps5wk_log_line_hook)
            ps5wk_log_line_hook("[web3] PRIVMARK-d relayed\n", strlen("[web3] PRIVMARK-d relayed\n"));
        errPart(" PRIVMARK-e\n[wpe-ps5] PRIVMARK-f unfinished at the end");
        usleep(50000); /* the stderr thread may take part of it meanwhile */
        ps5wk_log_private(0);
        errPart(" after-private tail\n");
        spike_log("after-private line %d", round);
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s queue|direct|private <port> <seconds> <dir>\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    int priv = !strcmp(argv[1], "private");
    int queue = priv || !strcmp(argv[1], "queue");
    int port = atoi(argv[2]), seconds = atoi(argv[3]);
    const char* dir = argv[4];
    int reportFD = fcntl(3, F_GETFD) >= 0 ? 3 : dup(1);
    report = fdopen(reportFD, "w");
    setvbuf(report, NULL, _IOLBF, 0);
    if (!freopen("/dev/null", "w", stdout)) /* spike_log's stdout copy (direct) */
        return 1;

    char path[512];
    snprintf(path, sizeof path, "%s/logsink-test.log", dir);
    spike_log_file_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (queue) {
        ps5wk_titlelog_install(dir);
        if (spike_title_log_start_hook("logsink-test", "127.0.0.1", port)) {
            fprintf(report, "titlelog start failed\n");
            return 1;
        }
    } else {
        if (spike_log_connect("127.0.0.1", port)) {
            fprintf(report, "connect failed\n");
            return 1;
        }
        FILE* log = fdopen(spike_log_fd, "w"); /* as the title did */
        setvbuf(log, NULL, _IOLBF, 0);
        stderr = log;
    }
    if (priv) {
        privateRun();
        spike_log("logsink-test done");
        fprintf(report, "flush: %s\n", spike_log_flush(2000) ? "done" : "timed out");
        return 0;
    }
    spike_log("logsink-test %s: 5 threads, 400 lines/s each, %d s", argv[1], seconds);

    pthread_t threads[5];
    for (int i = 0; i < 5; i++)
        pthread_create(&threads[i], NULL, logger, (void*)(intptr_t)i);
    long long start = nowUS();
    for (int s = 1; s <= seconds; s++) {
        while (nowUS() - start < s * 1000000LL)
            usleep(10000);
        long long w0 = atomic_exchange(&worstUS[0], 0), w1 = atomic_exchange(&worstUS[1], 0), w2 = atomic_exchange(&worstUS[2], 0);
        long n = atomic_exchange(&calls, 0);
        long long worst = w0 > w1 ? (w0 > w2 ? w0 : w2) : (w1 > w2 ? w1 : w2);
        fprintf(report, "t=%2ds calls %5ld  slowest call %8.3f ms (spike_log %.3f, stderr %.3f, relay %.3f)\n", s, n, worst / 1000.0,
            w0 / 1000.0, w1 / 1000.0, w2 / 1000.0);
    }
    running = 0;
    for (int i = 0; i < 5; i++)
        pthread_join(threads[i], NULL);
    spike_log("logsink-test done");
    fprintf(report, "flush: %s\n", spike_log_flush(2000) ? "done" : "timed out");
    return 0;
}
