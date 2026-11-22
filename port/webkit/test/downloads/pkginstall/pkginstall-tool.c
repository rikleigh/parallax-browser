/* pkginstall-tool: ps5pkginstall_run on the Mac, with a stand-in for the
 * console's installer, for pkginstall-test.sh.
 *   pkginstall-tool <pkg> <url-file> [refuse-code-hex] [stall-ms] [cancel-file]
 * Writes the URL to url-file once the server listens; the stand-in accepts
 * (or refuses with the code). The script then fetches with curl. Prints
 * "RESULT <r> code=<hex> served=<n>/<total> error=<text>" and the log.
 */
#include "../../../apps/common/ps5pkginstall.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char* urlFile;
static const char* cancelFile;
static unsigned refuse;
static uint64_t served, total;
static volatile int cancelFlag;

static int installer(const char* url, const char* name, unsigned* code, void* data)
{
    (void)data;
    printf("installer: %s (%s)\n", url, name);
    if (refuse) {
        *code = refuse;
        return -1;
    }
    return 0;
}

static void onUrl(const char* url, void* data)
{
    (void)data;
    char part[1024];
    snprintf(part, sizeof part, "%s.part", urlFile);
    FILE* f = fopen(part, "w");
    fputs(url, f);
    fclose(f);
    rename(part, urlFile);
}

static void onProgress(uint64_t done, uint64_t all, void* data)
{
    (void)data;
    served = done;
    total = all;
    if (cancelFile && !access(cancelFile, F_OK))
        cancelFlag = 1;
}

static void onLog(const char* line, void* data)
{
    (void)data;
    printf("log: %s\n", line);
}

int main(int argc, char** argv)
{
    if (argc < 3)
        return 2;
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    urlFile = argv[2];
    refuse = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 16) : 0;
    cancelFile = argc > 5 ? argv[5] : NULL;
    PS5PkgInstallOptions o = { 0 };
    o.path = argv[1];
    o.name = "Test package";
    o.installer = installer;
    o.on_url = onUrl;
    o.progress = onProgress;
    o.log = onLog;
    o.cancel = &cancelFlag;
    o.stall_ms = argc > 4 ? (unsigned)atoi(argv[4]) : 20000;
    o.settle_ms = 500;
    unsigned code = 0;
    char error[512] = "";
    int r = ps5pkginstall_run(&o, &code, error, sizeof error);
    printf("RESULT %d code=%08X served=%llu/%llu error=%s\n", r, code, (unsigned long long)served, (unsigned long long)total, error);
    return 0;
}
