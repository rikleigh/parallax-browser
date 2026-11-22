/* Driver for install-test.sh: find <dir> [depth], count <dir> [depth], install <dir> <user_app>,
 * param <param.json> (ps5install_parse_param: "OK id|name|version|contentId|category"). */
#include "../../../apps/common/ps5install.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv)
{
    char error[256] = "";
    PS5TitleInfo t;
    if (argc >= 3 && !strcmp(argv[1], "find")) {
        int r = ps5install_find(argv[2], argc > 3 ? (unsigned)atoi(argv[3]) : 2, &t, error, sizeof error);
        if (r) { printf("ERR %d %s\n", r, error); return 0; }
        printf("OK %s|%s|%s|%s\n", t.title_id, t.name, t.version, t.dir);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "param")) {
        static char text[256 * 1024];
        FILE* f = fopen(argv[2], "rb");
        size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
        if (f)
            fclose(f);
        text[n] = 0;
        int r = ps5install_parse_param(text, &t);
        if (r) { printf("ERR %d\n", r); return 0; }
        printf("OK %s|%s|%s|%s|%s\n", t.title_id, t.name, t.version, t.content_id, t.category);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "count")) {
        printf("COUNT %d\n", ps5install_count(argv[2], argc > 3 ? (unsigned)atoi(argv[3]) : 3));
        return 0;
    }
    if (argc == 4 && (!strcmp(argv[1], "install") || !strcmp(argv[1], "move"))) {
        if (ps5install_find(argv[2], 2, &t, error, sizeof error)) { printf("ERR find %s\n", error); return 0; }
        PS5InstallOptions o = { .user_app = argv[3], .move_only = !strcmp(argv[1], "move") };
        char installed[1024];
        int r = ps5install_title(&t, &o, installed, sizeof installed, error, sizeof error);
        if (r) printf("ERR %d %s\n", r, error); else printf("OK %s\n", installed);
        return 0;
    }
    return 2;
}
