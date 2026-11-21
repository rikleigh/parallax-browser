/*
 * files-drive: apps/common/ps5files.c from the command line (files-test.sh).
 *   files-drive move|copy SRC DEST [--margin BYTES] [--cancel-after BYTES]
 * Prints "OK [note]", "REFUSED <why>", "FAILED <why>" or "CANCELLED <why>".
 */
#include "../../../apps/common/ps5files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int cancel;
static unsigned long long cancelAfter;

static void onProgress(uint64_t done, uint64_t total, const char* current, void* data)
{
    (void)total;
    (void)current;
    (void)data;
    if (cancelAfter && done >= cancelAfter)
        cancel = 1;
}

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: files-drive move|copy SRC DEST [--margin N] [--cancel-after N]\n");
        return 2;
    }
    unsigned long long margin = 0;
    for (int i = 4; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--margin"))
            margin = strtoull(argv[i + 1], NULL, 10);
        else if (!strcmp(argv[i], "--cancel-after"))
            cancelAfter = strtoull(argv[i + 1], NULL, 10);
    }
    char error[512] = "";
    int r = ps5files_transfer(argv[2], argv[3], !strcmp(argv[1], "move"), margin, onProgress, NULL, &cancel, error, sizeof error);
    const char* what = r == 0 ? "OK" : r == -1 ? "REFUSED" : r == -2 ? "FAILED" : "CANCELLED";
    printf("%s%s%s\n", what, *error ? " " : "", error);
    return 0;
}
