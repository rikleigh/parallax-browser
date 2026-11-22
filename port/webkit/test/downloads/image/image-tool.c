/* image-tool: ps5ufs2.c and ps5image.c from the command line, for
 * image-test.sh.
 *   image-tool info <file>                  ps5image_json, or "ERR <why>"
 *   image-tool icon <file> <out.png>        "OK <bytes>" or "ERR <why>"
 *   image-tool read <image> <path>          the file's bytes (exFAT or UFS2), or "ERR <why>" (exit 1)
 *   image-tool ffpkg-measure <folder>       bytes
 *   image-tool ffpkg-build <folder> <image> [cancel-at-call]
 *                                           "OK <seconds>s calls=<n> last=<done>/<total>", "REFUSED <why>",
 *                                           "FAILED <why>", "CANCELLED <why>"
 *   image-tool ffpkg-verify <image> <folder> [cancel-at-call]
 *                                           "OK last=<done>/<total>", "DIFF <why>", "CANCELLED"
 */
#include "../../../apps/common/ps5image.h"
#include "../../../apps/common/ps5exfat.h"
#include "../../../apps/common/ps5ufs2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static unsigned calls;
static uint64_t lastDone, lastTotal, cancelAfter;
static volatile int cancelFlag;

static void onProgress(uint64_t done, uint64_t total, const char* current, void* data)
{
    (void)current;
    (void)data;
    calls++;
    lastDone = done;
    lastTotal = total;
    if (cancelAfter && calls >= cancelAfter)
        cancelFlag = 1;
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char** argv)
{
    char error[1024] = "";
    if (argc < 3)
        return 2;
    const char* verb = argv[1];
    if (!strcmp(verb, "info")) {
        PS5ImageInfo info;
        if (ps5image_info(argv[2], &info, error, sizeof error)) {
            printf("ERR %s\n", error);
            return 1;
        }
        char* json = ps5image_json(&info);
        printf("%s\n", json);
        free(json);
        return 0;
    }
    if (!strcmp(verb, "icon") && argc >= 4) {
        if (ps5image_icon(argv[2], argv[3], error, sizeof error)) {
            printf("ERR %s\n", error);
            return 1;
        }
        struct stat st;
        stat(argv[3], &st);
        printf("OK %lld\n", (long long)st.st_size);
        return 0;
    }
    if (!strcmp(verb, "read") && argc >= 4) {
        static unsigned char buffer[16 << 20];
        uint64_t length = 0;
        int r = ps5ufs2_is_ufs2(argv[2]) ? ps5ufs2_read_file(argv[2], argv[3], buffer, sizeof buffer, &length, error, sizeof error)
                                         : ps5exfat_read_file(argv[2], argv[3], buffer, sizeof buffer, &length, error, sizeof error);
        if (r) {
            printf("ERR %d %s\n", r, error);
            return 1;
        }
        fwrite(buffer, 1, length < sizeof buffer ? (size_t)length : sizeof buffer, stdout);
        return 0;
    }
    if (!strcmp(verb, "ffpkg-measure")) {
        int64_t n = ps5ufs2_measure(argv[2], NULL, error, sizeof error);
        if (n < 0) {
            printf("ERR %s\n", error);
            return 1;
        }
        printf("%lld\n", (long long)n);
        return 0;
    }
    if (!strcmp(verb, "ffpkg-build") && argc >= 4) {
        cancelAfter = argc > 4 ? strtoull(argv[4], NULL, 10) : 0;
        double t = now();
        int r = ps5ufs2_build(argv[2], argv[3], NULL, onProgress, NULL, &cancelFlag, error, sizeof error);
        if (!r)
            printf("OK %.2fs calls=%u last=%llu/%llu\n", now() - t, calls, (unsigned long long)lastDone, (unsigned long long)lastTotal);
        else
            printf("%s %s\n", r == -1 ? "REFUSED" : r == -3 ? "CANCELLED" : "FAILED", error);
        return r ? 1 : 0;
    }
    if (!strcmp(verb, "ffpkg-verify") && argc >= 4) {
        cancelAfter = argc > 4 ? strtoull(argv[4], NULL, 10) : 0;
        int r = ps5ufs2_verify(argv[2], argv[3], onProgress, NULL, &cancelFlag, error, sizeof error);
        if (!r)
            printf("OK last=%llu/%llu\n", (unsigned long long)lastDone, (unsigned long long)lastTotal);
        else
            printf("%s %s\n", r == -3 ? "CANCELLED" : "DIFF", error);
        return r ? 1 : 0;
    }
    return 2;
}
