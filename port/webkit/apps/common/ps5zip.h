/* ps5zip.h: unpacking a release zip (port/webkit/UPDATES.md, "Unpack").
 * zlib and POSIX only, so it builds for the PS5 and on the Mac (its test:
 * port/webkit/test/update/zip-test.sh). */
#ifndef PS5ZIP_H
#define PS5ZIP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* done and total are uncompressed bytes. */
typedef void (*ps5zip_progress_fn)(uint64_t done, uint64_t total, void* data);

/* Unpacks archive into dest (an existing, empty folder). Every entry must
 * be under prefix (e.g. "PPSA99207/"), which is taken off: prefix's files
 * land in dest. Before anything is written, the whole directory is checked:
 * no absolute or parent paths, no backslashes, no links or special files,
 * no encryption, only stored and deflated entries, no duplicates, at most
 * maxEntries entries and maxBytes bytes unpacked, no zip64. Each file's size
 * and CRC are checked as it is written. Files and folders get mode 0777 (the
 * console won't start an app with less). 0, or -1 with error filled; a
 * failure part way leaves what was written (the caller removes dest). */
int ps5zip_extract(const char* archive, const char* prefix, const char* dest, unsigned maxEntries, uint64_t maxBytes,
    ps5zip_progress_fn progress, void* data, char* error, size_t errorSize);

/* Removes path and everything under it (no links followed). 0, or -1 with
 * errno; ENOENT counts as done. */
int ps5zip_remove_tree(const char* path);

#ifdef __cplusplus
}
#endif

#endif
