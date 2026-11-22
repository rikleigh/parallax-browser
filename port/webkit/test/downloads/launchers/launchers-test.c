/* The driver for launchers-test.sh: apps/common/ps5launchers.c on the Mac.
 *   t detect ROOT SMPPORT KPORT [FORCE]
 *   t scans ROOT SMPPORT KPORT FOLDER [FORCE]
 *   t announce ROOT SMPPORT KPORT FOLDER ID REPLACED [FORCE]
 *   t ready ROOT SMPPORT KPORT FOLDER ID [FORCE]
 *   t source ROOT SMPPORT KPORT ID
 *   t unmount ROOT SMPPORT KPORT ID
 */
#include "../../../apps/common/ps5launchers.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv)
{
    if (argc < 5)
        return 2;
    PS5LaunchersOptions o = { .root = argv[2], .smp_port = atoi(argv[3]), .kraken_port = atoi(argv[4]), .timeout_ms = 800 };
    const char* cmd = argv[1];
    int forceAt = !strcmp(cmd, "detect") ? 5 : !strcmp(cmd, "scans") ? 6 : !strcmp(cmd, "announce") ? 8 : !strcmp(cmd, "ready") ? 7 : 99;
    PS5Launchers l;
    ps5launchers_detect(&l, &o);
    if (argc > forceAt)
        ps5launchers_force(&l, argv[forceAt]);
    char why[256] = "";
    if (!strcmp(cmd, "detect")) {
        char* json = ps5launchers_json(&l);
        printf("%s", json);
        g_free(json);
        for (unsigned i = 0; i < l.smp_root_count; i++)
            printf(" smp:%s", l.smp_roots[i]);
        for (unsigned i = 0; i < l.kraken_root_count; i++)
            printf(" k:%s", l.kraken_roots[i]);
        printf("\n");
    } else if (!strcmp(cmd, "scans") && argc > 5) {
        bool ok = ps5launchers_scans(&l, argv[5], why, sizeof why);
        printf("%s %s\n", ok ? "YES" : "NO", why);
    } else if (!strcmp(cmd, "announce") && argc > 7) {
        int r = ps5launchers_announce(&l, argv[5], argv[6], !strcmp(argv[7], "1"), &o, why, sizeof why);
        printf("%d %s\n", r, why);
    } else if (!strcmp(cmd, "ready") && argc > 6) {
        int r = ps5launchers_app_ready(&l, argv[5], argv[6], &o, why, sizeof why);
        printf("%d %s\n", r, why);
    } else if (!strcmp(cmd, "source") && argc > 5) {
        char from[1024];
        bool found = ps5launchers_source(&l, argv[5], true, &o, from, sizeof from);
        printf("%s\n", found ? from : "-");
    } else if (!strcmp(cmd, "unmount") && argc > 5) {
        int r = ps5launchers_before_replace(&l, argv[5], &o, why, sizeof why);
        printf("%d %s\n", r, why);
    } else
        return 2;
    return 0;
}
