/*
 * ps5exfat.h: an exFAT image made from a folder (the download service,
 * apps/child/dl-job.c): a game or app folder becomes one file that the
 * console's image mounters can mount. The image's layout (where the folder
 * goes, the name and extension, cluster size) follows what those mounters
 * expect: see ps5exfat.c's header comment.
 *
 * POSIX only, so it builds for the PS5 and on the Mac (its test:
 * port/webkit/test/downloads/exfat-test.sh, checked with fsck_exfat and by
 * mounting the image with hdiutil).
 *
 * The image is written front to back in one pass: the folder is listed
 * first (sizes known), every file and folder gets contiguous clusters
 * (also chained in the FAT, NoFatChain clear: what the console-tested
 * writers do; see ps5exfat.c), then the boot region, the FAT, the
 * allocation bitmap, the up-case table and the directories are written,
 * then each file's data in order. Writing <image>.part, renamed when
 * complete.
 *
 * For ShadowMountPlus: src is the game's root folder (eboot.bin,
 * sce_sys/param.json), whose contents become the image root; name the
 * image <TITLE_ID>.exfat and put it in /data/homebrew/ (or
 * /data/etaHEN/games/, /mnt/usbN/homebrew/); leave cluster_size 0 (64 KiB).
 */
#ifndef PS5EXFAT_H
#define PS5EXFAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* label;      /* volume label (UTF-8, at most 11 characters kept); NULL: none */
    uint32_t cluster_size;  /* bytes, a power of two 4 KiB..32 MiB; 0: chosen from the total size */
    uint64_t free_margin;   /* left free on the image's drive; 0: 64 MiB */
} PS5ExfatOptions;

/* done and total are bytes of file data copied. */
typedef void (*ps5exfat_progress_fn)(uint64_t done, uint64_t total, const char* current, void* data);

/* How big the image of src would be (bytes), or -1 with error filled. */
int64_t ps5exfat_measure(const char* src, const PS5ExfatOptions* options, char* error, size_t error_size);

/* Writes image (a new file; an existing one is refused) holding src's
 * files and folders. Refuses links and special files. *cancel nonzero
 * stops between blocks. 0; -1 refused (what src holds, an existing image);
 * -2 failed (reading, writing, no space); -3 cancelled. On -2 and -3 the
 * .part is deleted. */
int ps5exfat_build(const char* src, const char* image, const PS5ExfatOptions* options,
    ps5exfat_progress_fn progress, void* data, volatile int* cancel, char* error, size_t error_size);

/* Reads image back as exFAT (the boot region and its checksum, the FAT
 * chains, every entry set's checksum, the up-case table) and compares it
 * with src: the same names, kinds and sizes both ways, and every file's
 * bytes. 0 the same; -1 not (error says what); -3 cancelled. Progress:
 * bytes compared. */
int ps5exfat_verify(const char* image, const char* src, ps5exfat_progress_fn progress, void* data, volatile int* cancel, char* error,
    size_t error_size);

/* One file of image ("sce_sys/param.json": from the root; ASCII letters
 * match in either case, as exFAT names do): its first size bytes into
 * buffer (buffer may be NULL with size 0: only whether it is there), its
 * length into *length. Takes other builders' images too: any sector and
 * cluster size, and a zeroed FAT read as contiguous. 0; -1 not there, not
 * a file, or not exFAT (error says which); -2 the image is damaged. */
int ps5exfat_read_file(const char* image, const char* path, void* buffer, size_t size, uint64_t* length, char* error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
