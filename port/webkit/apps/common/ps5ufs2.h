/*
 * ps5ufs2.h: a UFS2 image made from a folder (ShadowMountPlus's .ffpkg),
 * and reading one back. The download service (apps/child/dl-job.c) makes
 * these next to exFAT images (ps5exfat.h); ps5image.c reads an app's
 * param.json and icon out of one.
 *
 * The writer is a port of PS5 Dump Forge's crates/ps5-dump-forge-ufs2
 * (quer3q/ps5-dump-forge, GPL-3.0; see ps5ufs2.c and THIRD-PARTY-NOTICES.md):
 * the one geometry ShadowMountPlus recommends, what
 * `newfs -O 2 -b 65536 -f 65536 -m 0 -S 4096 -i <density>` formats, filled
 * the way a one-pass makefs would. Every file is one contiguous run of
 * 64 KiB blocks, and the image is written front to back once. Writing
 * <image>.part, renamed when complete.
 *
 * For ShadowMountPlus (1.7 and later): src is the game's root folder
 * (eboot.bin, sce_sys/param.json), whose contents become the image root;
 * name the image <TITLE_ID>.ffpkg and put it where its scan finds it
 * (/data/homebrew/, /mnt/usbN/homebrew/...) or add it by path.
 *
 * The reader takes any UFS2 image (block and fragment sizes from its
 * superblock), read-only.
 *
 * POSIX only, so it builds for the PS5 and on the Mac (its test:
 * port/webkit/test/downloads/image-test.sh).
 */
#ifndef PS5UFS2_H
#define PS5UFS2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* maker;     /* fs_volname: printable ASCII, at most 31 bytes; NULL: none */
    uint64_t free_bytes;   /* free space beyond the 64 MiB spare, for writable mounts; 0: none */
    uint64_t free_margin;  /* left free on the image's drive; 0: 64 MiB */
} PS5Ufs2Options;

/* done and total are bytes of file data copied (or compared). */
typedef void (*ps5ufs2_progress_fn)(uint64_t done, uint64_t total, const char* current, void* data);

/* How big the image of src would be (bytes), or -1 with error filled. */
int64_t ps5ufs2_measure(const char* src, const PS5Ufs2Options* options, char* error, size_t error_size);

/* Writes image (a new file; an existing one is refused) holding src's
 * files and folders. Refuses links and special files. *cancel nonzero
 * stops between blocks. 0; -1 refused (what src holds, an existing image);
 * -2 failed (reading, writing, no space); -3 cancelled. On -2 and -3 the
 * .part is deleted. */
int ps5ufs2_build(const char* src, const char* image, const PS5Ufs2Options* options, ps5ufs2_progress_fn progress, void* data,
    volatile int* cancel, char* error, size_t error_size);

/* Reads image back as UFS2 (superblock, inodes, block maps, directories,
 * independently of the writer) and compares it with src: the same names,
 * kinds and sizes both ways, and every file's bytes. 0 the same; -1 not
 * (error says what); -3 cancelled. Progress: bytes compared. */
int ps5ufs2_verify(const char* image, const char* src, ps5ufs2_progress_fn progress, void* data, volatile int* cancel, char* error,
    size_t error_size);

/* Whether the file starts like a UFS2 volume (its superblock's magic at
 * 64 KiB + 1372). */
bool ps5ufs2_is_ufs2(const char* image);

/* One file of image ("sce_sys/param.json": from the root, case matters):
 * its first size bytes into buffer (buffer may be NULL with size 0: only
 * whether it is there), its length into *length. 0; -1 not there, not a
 * file, or not a UFS2 image (error says which); -2 the image couldn't be
 * read. */
int ps5ufs2_read_file(const char* image, const char* path, void* buffer, size_t size, uint64_t* length, char* error,
    size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
