/* exfat-tool.c: ps5exfat on the Mac (exfat-test.sh builds and runs it).
 *   exfat-tool measure <folder> [cluster]: prints the image size
 *   exfat-tool build <folder> <image> [cluster] [label] [cancel-at-call]
 * build prints "OK <bytes> <seconds> <MB/s> calls=<n> last=<done>/<total>",
 * "REFUSED <why>" (-1), "FAILED <why>" (-2) or "CANCELLED <why>" (-3). */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include "../../../apps/common/ps5exfat.h"

typedef struct {
    unsigned calls;
    uint64_t done, total;
    unsigned cancelAt; /* sets *cancel at this progress call; 0 never */
    volatile int* cancel;
} Seen;

static void progress(uint64_t done, uint64_t total, const char* current, void* data)
{
    (void)current;
    Seen* seen = data;
    seen->calls++;
    seen->done = done;
    seen->total = total;
    if (seen->cancelAt && seen->calls >= seen->cancelAt)
        *seen->cancel = 1;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: exfat-tool measure <folder> [cluster] | build <folder> <image> [cluster] [label] [cancel-at-call]\n");
        return 2;
    }
    char error[512] = "";
    PS5ExfatOptions options = { 0 };
    if (argv[1][0] == 'm') {
        options.cluster_size = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 0;
        int64_t size = ps5exfat_measure(argv[2], &options, error, sizeof error);
        if (size < 0) {
            printf("REFUSED %s\n", error);
            return 1;
        }
        printf("%lld\n", (long long)size);
        return 0;
    }
    if (argc < 4)
        return 2;
    options.cluster_size = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) : 0;
    options.label = argc > 5 && argv[5][0] ? argv[5] : NULL;
    volatile int cancel = 0;
    Seen seen = { 0, 0, 0, argc > 6 ? (unsigned)strtoul(argv[6], NULL, 0) : 0, &cancel };
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    int r = ps5exfat_build(argv[2], argv[3], &options, progress, &seen, &cancel, error, sizeof error);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double seconds = (double)(b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    if (r) {
        printf("%s %s\n", r == -1 ? "REFUSED" : r == -2 ? "FAILED" : "CANCELLED", error);
        return 1;
    }
    struct stat st;
    stat(argv[3], &st);
    printf("OK %lld %.2f %.0f calls=%u last=%llu/%llu\n", (long long)st.st_size, seconds,
        seen.total / 1e6 / (seconds > 0 ? seconds : 1), seen.calls, (unsigned long long)seen.done, (unsigned long long)seen.total);
    return 0;
}
