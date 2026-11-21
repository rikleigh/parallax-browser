/*
 * ps5files.h: moving and copying files and folders between drives, for
 * the download manager's Files tab (the download service,
 * apps/child/dl-job.c, jobs "move" and "copy").
 *
 * POSIX only, so it builds for the PS5 and on the Mac (its test:
 * port/webkit/test/downloads/files-test.sh).
 *
 * src is a file or a folder; dest the new path (not there yet: an
 * existing one is refused, nothing is overwritten). A move on the same
 * drive is one rename. Otherwise src is listed first (links and special
 * files are refused before anything is written, the bytes are counted and
 * checked against the free space on dest's drive), then copied into
 * <dest>.part, which is renamed to dest once complete; a move then
 * deletes src. Files and folders are made 0777 (the console won't start
 * an app with less). dest inside src, or src itself, is refused.
 */
#ifndef PS5FILES_H
#define PS5FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* done and total are bytes copied; current the file being copied. */
typedef void (*ps5files_progress_fn)(uint64_t done, uint64_t total, const char* current, void* data);

/* 0; -1 refused (what src holds, dest there, no space); -2 failed
 * (reading, writing); -3 cancelled (*cancel nonzero, checked between
 * blocks). On -2 and -3 the .part is deleted and src is untouched; a move
 * whose copy is complete but whose src couldn't all be deleted returns 0
 * with error filled ("copied, but ..."). free_margin: left free on dest's
 * drive (0: 64 MiB). */
int ps5files_transfer(const char* src, const char* dest, bool move, uint64_t free_margin, ps5files_progress_fn progress,
    void* data, volatile int* cancel, char* error, size_t error_size);

/* Deletes a file, or a folder and everything in it (no links followed).
 * 0 or -1. */
int ps5files_remove_tree(const char* path);

#ifdef __cplusplus
}
#endif

#endif
