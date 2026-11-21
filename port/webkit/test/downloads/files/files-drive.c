/*
 * files-drive: apps/common/ps5files.c from the command line (files-test.sh).
 *   files-drive move|copy SRC DEST [--margin BYTES] [--cancel-after BYTES]
 *               [--create-at-start PATH]   (a file, or a folder if PATH ends
 *               with '/', made when copying starts: after the checks, as
 *               another job or a download would)
 *               [--create-at-end PATH]     (a file made once everything is
 *               copied, before a move deletes the original)
 * Prints "OK [note]", "REFUSED <why>", "FAILED <why>" or "CANCELLED <why>".
 */
#include "../../../apps/common/ps5files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile int cancel;
static unsigned long long cancelAfter;
static const char* createAtStart;
static const char* createAtEnd; /* at the last report (everything copied, before the original goes) */

static void onProgress(uint64_t done, uint64_t total, const char* current, void* data)
{
    (void)data;
    if (cancelAfter && done >= cancelAfter)
        cancel = 1;
    if (createAtEnd && done == total && done > 0 && current && !*current) {
        int fd = open(createAtEnd, O_WRONLY | O_CREAT, 0666);
        if (fd >= 0)
            close(fd);
        createAtEnd = NULL;
    }
    if (createAtStart) {
        size_t n = strlen(createAtStart);
        if (createAtStart[n - 1] == '/')
            mkdir(createAtStart, 0777);
        else {
            int fd = open(createAtStart, O_WRONLY | O_CREAT, 0666);
            if (fd >= 0) {
                if (write(fd, "planted\n", 8) != 8)
                    perror("write");
                close(fd);
            }
        }
        createAtStart = NULL;
    }
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
        else if (!strcmp(argv[i], "--create-at-start"))
            createAtStart = argv[i + 1];
        else if (!strcmp(argv[i], "--create-at-end"))
            createAtEnd = argv[i + 1];
    }
    char error[512] = "";
    int r = ps5files_transfer(argv[2], argv[3], !strcmp(argv[1], "move"), margin, onProgress, NULL, &cancel, error, sizeof error);
    const char* what = r == 0 ? "OK" : r == -1 ? "REFUSED" : r == -2 ? "FAILED" : "CANCELLED";
    printf("%s%s%s\n", what, *error ? " " : "", error);
    return 0;
}
