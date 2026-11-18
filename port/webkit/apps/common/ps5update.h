/* ps5update.h: what the browser's own updates (port/webkit/UPDATES.md) share
 * between the title (apps/title/updates.c), the update job
 * (apps/child/update-job.c) and the restart helper
 * (apps/relaunch/ps5wk-relaunch.c): one reading of ProsperoStore's files,
 * param.json and ready.txt, so the three can't disagree. Plain C and POSIX
 * only (the helper has no glib), so it builds for the PS5 and on the Mac
 * (its test: port/webkit/test/update/store-test.sh). */
#ifndef PS5UPDATE_H
#define PS5UPDATE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS5UPDATE_SOURCE "https://api.github.com/repos/rikleigh/parallax-browser/releases?per_page=10"
#define PS5UPDATE_STORE_ROOT "/data/prosperostore"
#define PS5UPDATE_WORK "/data/ps5webkit/update"

/* A file's contents, NUL-terminated, malloc'd; NULL with errno (EFBIG over
 * limit bytes, EINVAL with a NUL byte in it). */
char* ps5update_read(const char* path, size_t limit);

/* The string value of key at the top level of the JSON object text, its
 * escapes decoded: 1 found, 0 not there, -1 text isn't one valid JSON
 * object, key is there twice, its value isn't a string or is longer than
 * size - 1. */
int ps5update_json_string(const char* text, const char* key, char* out, size_t size);

/* key=value lines (ready.txt): 0, or -1 (not there, or too long). */
int ps5update_line_value(const char* text, const char* key, char* out, size_t size);

/* The titleId and contentVersion of <folder>/sce_sys/param.json: 0 or -1. */
int ps5update_param(const char* folder, char* title, size_t titleSize, char* version, size_t versionSize);

/* What ProsperoStore's files (under root) say about the install at folder.
 * It fails closed: a receipt or journal that is there but can't be read or
 * understood counts as the store's.
 *   managed: <root>/receipts/<title>.json names folder (location + "/" +
 *            title, as the same folder in any spelling), whatever version it
 *            records, or can't be read: the store installed the browser
 *            there, so it is updated in the store.
 *   busy:    <root>/journal.json (an operation of the store's in progress or
 *            interrupted) names title, or can't be read. */
typedef struct {
    int managed;
    int busy;
    char note[400]; /* what was found, for the log */
} ps5update_store;
void ps5update_store_check(const char* root, const char* title, const char* folder, ps5update_store* out);

/* Removes path and everything under it (no links followed). 0, or -1 with
 * errno; ENOENT counts as done. */
int ps5update_remove_tree(const char* path);

#ifdef __cplusplus
}
#endif

#endif
