/* archive-test.c: apps/common/ps5archive.c on the Mac (archive-test.sh
 * builds and runs it).
 *
 *   archive-test is <name>              "yes" / "no"
 *   archive-test volumes <path>         the parts' names, or "MISSING <why>"
 *   archive-test inspect <path> [max_entries max_bytes]
 *   archive-test extract <path> <dest> [max_entries max_bytes margin [cancel]]
 *
 * inspect and extract print "OK ...", "REFUSED <why>" (-1), "FAILED <why>"
 * (-2) or "CANCELLED <why>" (-3). extract also checks the progress calls:
 * done never goes back and never passes total, and at most ~10 a second.
 * "cancel" sets *cancel at the first progress call. */
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../../apps/common/ps5archive.h"

typedef struct {
    unsigned calls;
    uint64_t lastDone;
    int bad;
    int cancelAtFirst;
    volatile int cancel;
    struct timespec start;
} Progress;

static double seconds(const struct timespec* since)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - since->tv_sec) + (double)(now.tv_nsec - since->tv_nsec) / 1e9;
}

static void onProgress(uint64_t done, uint64_t total, const char* current, void* data)
{
    Progress* p = data;
    if (done < p->lastDone || done > total || !current)
        p->bad = 1;
    p->lastDone = done;
    p->calls++;
    if (p->cancelAtFirst)
        p->cancel = 1;
}

static const char* outcome(int r)
{
    return r == -1 ? "REFUSED" : r == -2 ? "FAILED" : r == -3 ? "CANCELLED" : "ERROR";
}

int main(int argc, char** argv)
{
    /* ps5archive gets UTF-8 names from libarchive on the PS5 (build.sh's
     * libarchive step); here that takes a UTF-8 locale. */
    setlocale(LC_CTYPE, "en_US.UTF-8");
    char error[512] = "";
    if (argc == 3 && !strcmp(argv[1], "is")) {
        printf("%s\n", ps5archive_is_archive(argv[2]) ? "yes" : "no");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "volumes")) {
        char** list = ps5archive_volumes(argv[2], error, sizeof error);
        if (!list) {
            printf("MISSING %s\n", error);
            return 1;
        }
        for (char** p = list; *p; p++) {
            const char* slash = strrchr(*p, '/');
            printf("%s%s", p == list ? "" : " ", slash ? slash + 1 : *p);
        }
        printf("\n");
        ps5archive_free_list(list);
        return 0;
    }
    PS5ArchiveLimits limits = { 0 };
    if (argc >= 3 && !strcmp(argv[1], "inspect")) {
        if (argc >= 5) {
            limits.max_entries = (unsigned)strtoul(argv[3], NULL, 10);
            limits.max_bytes = strtoull(argv[4], NULL, 10);
        }
        PS5ArchiveInfo info;
        int r = ps5archive_inspect(argv[2], &limits, &info, error, sizeof error);
        if (r) {
            printf("%s %s\n", outcome(r), error);
            return 1;
        }
        printf("OK format=%s volumes=%u entries=%u unpacked=%llu sizes_known=%d top=%s title=%s\n", info.format, info.volumes,
            info.entries, (unsigned long long)info.unpacked, info.sizes_known, info.top_folder[0] ? info.top_folder : "-",
            info.has_title ? (info.title_dir[0] ? info.title_dir : ".") : "-");
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "extract")) {
        if (argc >= 7) {
            limits.max_entries = (unsigned)strtoul(argv[4], NULL, 10);
            limits.max_bytes = strtoull(argv[5], NULL, 10);
            limits.free_margin = strtoull(argv[6], NULL, 10);
        }
        Progress p = { .cancelAtFirst = argc >= 8 && !strcmp(argv[7], "cancel") };
        clock_gettime(CLOCK_MONOTONIC, &p.start);
        int r = ps5archive_extract(argv[2], argv[3], &limits, onProgress, &p, &p.cancel, error, sizeof error);
        double took = seconds(&p.start);
        /* the first and last calls come on top of ~10 a second */
        if (p.calls > (unsigned)(took * 10) + 3)
            p.bad = 1;
        if (r) {
            printf("%s %s\n", outcome(r), error);
            return 1;
        }
        printf("OK progress=%s\n", p.bad ? "BAD" : "ok");
        return 0;
    }
    fprintf(stderr, "usage: see archive-test.c\n");
    return 2;
}
