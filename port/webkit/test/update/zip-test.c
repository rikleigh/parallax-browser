/* zip-test.c: ps5zip_extract on the Mac (zip-test.sh builds and runs it).
 * zip-test <archive> <prefix> <dest>: prints "OK" or "REFUSED <why>". */
#include <stdio.h>
#include "../../apps/common/ps5zip.h"

int main(int argc, char** argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: zip-test <archive> <prefix> <dest>\n");
        return 2;
    }
    char error[512] = "";
    int r = ps5zip_extract(argv[1], argv[2], argv[3], 20000, 2ull << 30, NULL, NULL, error, sizeof error);
    if (r)
        printf("REFUSED %s\n", error);
    else
        printf("OK\n");
    return r ? 1 : 0;
}
