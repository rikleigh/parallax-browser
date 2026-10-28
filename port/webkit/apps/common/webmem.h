/* The WebProcess memory limit, UIProcess side (browser title, ps5wk-shot).
 *
 * libpas memory the WebProcess has committed only grows on PS5 when pages
 * can't be given back, and one WebProcess serves every navigation (patch
 * 0018). ps5wk-child's memory thread compares libpas's committed memory
 * with PS5WK_MEM_RESTART_MB (child.env, default 3072, 0 = off) every 5 s
 * and writes PS5WK_LAUNCH_DIR/webmem-<UIProcess pid>:
 *     pid <web pid> committed <MB> limit <MB> over <0|1>
 * When it says over 1, the UIProcess ends the WebProcess at the next
 * top-level navigation and loads that address in a new one (one payload
 * at a time, so the page-table budget is the same as before). */
#ifndef PS5WK_WEBMEM_H
#define PS5WK_WEBMEM_H

#include <stdio.h>
#include <unistd.h>

#define PS5WK_WEBMEM_STATUS_FORMAT "/system_tmp/ps5wk/webmem-%d"

/* 1 when the WebProcess said it is over its limit; its numbers in the out
 * parameters. */
static inline int ps5wk_webmem_over(int* webPid, double* committedMB, double* limitMB)
{
    char path[64];
    snprintf(path, sizeof path, PS5WK_WEBMEM_STATUS_FORMAT, (int)getpid());
    FILE* file = fopen(path, "r");
    if (!file)
        return 0;
    int over = 0;
    if (fscanf(file, "pid %d committed %lf limit %lf over %d", webPid, committedMB, limitMB, &over) != 4)
        over = 0;
    fclose(file);
    return over == 1;
}

/* The status of a WebProcess that is being replaced (the new one writes its
 * own within 5 s). */
static inline void ps5wk_webmem_forget(void)
{
    char path[64];
    snprintf(path, sizeof path, PS5WK_WEBMEM_STATUS_FORMAT, (int)getpid());
    unlink(path);
}

#endif
