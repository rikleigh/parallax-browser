/*
 * fetch-drive.c: runs one download through apps/common/ps5fetch.c on the
 * Mac, for fetch-test.sh.
 *
 *   fetch-drive [--conns N] [--max-conns N] [--chunk BYTES] [--stall S] [--margin BYTES]
 *               [--orig URL] [--cookies FILE] [--ua TEXT] [--referer URL]
 *               [--stop-at BYTES] [--cancel-at BYTES] [--timeout S] URL PATH
 *   fetch-drive --saved PATH
 *
 * --stop-at / --cancel-at: ps5fetch_stop / ps5fetch_cancel once that many
 * bytes are on disk. Progress goes to stderr every half second; the result
 * is one line on stdout, key=value separated by spaces (error last):
 *   state=done received=N total=N got=N secs=S speed=B/s maxconns=N
 *   lateconns=N allowed=N ranges=0|1 restarted=0|1 retryable=0|1 status=N
 *   final=URL error=TEXT
 * got: bytes that came on disk during this run; speed: got / secs;
 * lateconns: the most connections seen in the second half of the bytes;
 * allowed: the connections the engine used at the end (grown or lowered).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "../../../apps/common/ps5fetch.h"

static gint endedFlag;

static void ended(PS5Fetch* fetch, PS5FetchState state, void* data)
{
    (void)fetch;
    (void)state;
    (void)data;
    g_atomic_int_set(&endedFlag, 1);
}

static const char* stateName(PS5FetchState state)
{
    switch (state) {
    case PS5FETCH_RUNNING:
        return "running";
    case PS5FETCH_DONE:
        return "done";
    case PS5FETCH_PAUSED:
        return "paused";
    case PS5FETCH_FAILED:
        return "failed";
    case PS5FETCH_CANCELLED:
        return "cancelled";
    }
    return "?";
}

int main(int argc, char** argv)
{
    PS5FetchOptions o = { 0 };
    o.connections = 1;
    guint64 stopAt = 0, cancelAt = 0;
    double timeout = 300;
    char* cookies = NULL;
    const char* positional[2] = { NULL, NULL };
    int npositional = 0;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--saved") && v) {
            uint64_t received = 0;
            int64_t total = -1;
            bool saved = ps5fetch_saved(v, &received, &total);
            printf("saved=%d received=%llu total=%lld\n", saved, (unsigned long long)received, (long long)total);
            return 0;
        }
        if (a[0] == '-' && a[1] == '-' && v) {
            i++;
            if (!strcmp(a, "--conns"))
                o.connections = (unsigned)atoi(v);
            else if (!strcmp(a, "--max-conns"))
                o.max_connections = (unsigned)atoi(v);
            else if (!strcmp(a, "--chunk"))
                o.chunk = g_ascii_strtoull(v, NULL, 10);
            else if (!strcmp(a, "--stall"))
                o.stall_s = (unsigned)atoi(v);
            else if (!strcmp(a, "--margin"))
                o.free_margin = g_ascii_strtoull(v, NULL, 10);
            else if (!strcmp(a, "--orig"))
                o.original_url = v;
            else if (!strcmp(a, "--ua"))
                o.user_agent = v;
            else if (!strcmp(a, "--referer"))
                o.referer = v;
            else if (!strcmp(a, "--cookies")) {
                if (!g_file_get_contents(v, &cookies, NULL, NULL)) {
                    fprintf(stderr, "can't read %s\n", v);
                    return 2;
                }
                o.cookies = cookies;
            } else if (!strcmp(a, "--stop-at"))
                stopAt = g_ascii_strtoull(v, NULL, 10);
            else if (!strcmp(a, "--cancel-at"))
                cancelAt = g_ascii_strtoull(v, NULL, 10);
            else if (!strcmp(a, "--timeout"))
                timeout = atof(v);
            else {
                fprintf(stderr, "unknown option %s\n", a);
                return 2;
            }
        } else if (npositional < 2)
            positional[npositional++] = a;
    }
    if (npositional != 2) {
        fprintf(stderr, "usage: fetch-drive [options] URL PATH | --saved PATH\n");
        return 2;
    }
    o.url = positional[0];
    o.path = positional[1];

    gint64 t0 = g_get_monotonic_time();
    PS5Fetch* fetch = ps5fetch_start(&o, ended, NULL);
    if (!fetch) {
        printf("state=nostart\n");
        return 1;
    }
    PS5FetchStatus s;
    ps5fetch_status(fetch, &s);
    guint64 first = s.received;
    unsigned maxConns = 0, lateConns = 0;
    gboolean asked = FALSE;
    gint64 shown = 0;
    while (!g_atomic_int_get(&endedFlag)) {
        g_usleep(20000);
        ps5fetch_status(fetch, &s);
        gint64 now = g_get_monotonic_time();
        maxConns = MAX(maxConns, s.connections);
        if (s.total > 0 && s.received * 2 >= (guint64)s.total)
            lateConns = MAX(lateConns, s.connections);
        if (!asked && stopAt && s.received >= stopAt) {
            ps5fetch_stop(fetch);
            asked = TRUE;
        }
        if (!asked && cancelAt && s.received >= cancelAt) {
            ps5fetch_cancel(fetch);
            asked = TRUE;
        }
        if (!asked && (now - t0) / 1e6 > timeout) {
            fprintf(stderr, "fetch-drive: timed out, stopping\n");
            ps5fetch_stop(fetch);
            asked = TRUE;
        }
        if (now - shown >= 500000) {
            shown = now;
            fprintf(stderr, "  %5.1f s  %llu/%lld  %.2f MB/s  %u conns (of %u)  status %d\n", (now - t0) / 1e6, (unsigned long long)s.received,
                (long long)s.total, s.speed / 1048576, s.connections, s.allowed, s.http_status);
        }
    }
    double secs = (g_get_monotonic_time() - t0) / 1e6;
    ps5fetch_status(fetch, &s);
    ps5fetch_free(fetch);
    guint64 got = s.received >= first ? s.received - first : s.received;
    printf("state=%s received=%llu total=%lld got=%llu secs=%.2f speed=%.0f maxconns=%u lateconns=%u allowed=%u ranges=%d restarted=%d retryable=%d "
           "status=%d final=%s error=%s\n",
        stateName(s.state), (unsigned long long)s.received, (long long)s.total, (unsigned long long)got, secs, secs > 0 ? got / secs : 0,
        maxConns, lateConns, s.allowed, s.ranges, s.restarted, s.retryable, s.http_status, s.final_url, s.error);
    g_free(cookies);
    return 0;
}
