/*
 * peek-drive: apps/common/ps5peek.c from the command line (peek-test.sh).
 *   peek-drive --url URL --size N [--cookies FILE] [--max-bytes N] [--timeout MS] [--user-app DIR]
 *   peek-drive --path FILE [--user-app DIR]
 * Prints the JSON, or "ERR <why>".
 */
#include "../../../apps/common/ps5peek.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* readFile(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    static char text[65536];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    return text;
}

int main(int argc, char** argv)
{
    PS5PeekOptions o = { 0 };
    for (int i = 1; i + 1 < argc; i += 2) {
        const char* k = argv[i];
        const char* v = argv[i + 1];
        if (!strcmp(k, "--url"))
            o.url = v;
        else if (!strcmp(k, "--size"))
            o.size = strtoull(v, NULL, 10);
        else if (!strcmp(k, "--path"))
            o.path = v;
        else if (!strcmp(k, "--cookies"))
            o.cookies = readFile(v);
        else if (!strcmp(k, "--max-bytes"))
            o.max_bytes = strtoull(v, NULL, 10);
        else if (!strcmp(k, "--timeout"))
            o.timeout_ms = (unsigned)strtoul(v, NULL, 10);
        else if (!strcmp(k, "--user-app"))
            o.user_app = v;
        else {
            fprintf(stderr, "unknown option %s\n", k);
            return 2;
        }
    }
    o.user_agent = "peek-drive";
    char error[256];
    char* json = ps5peek_json(&o, error, sizeof error);
    if (json)
        printf("%s\n", json);
    else
        printf("ERR %s\n", error);
    free(json);
    return 0;
}
