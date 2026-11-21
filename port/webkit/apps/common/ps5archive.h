/*
 * ps5archive.h: unpacking downloaded archives (ZIP, 7z, RAR4/RAR5, tar and
 * its compressed forms, including split and multipart sets) with
 * libarchive, in the download service (apps/child/dl-job.c).
 *
 * libarchive, zlib and POSIX only, so it builds for the PS5 (port/deps:
 * libarchive with zlib, bzip2, liblzma) and on the Mac (its tests:
 * port/webkit/test/downloads/archive-test.sh, peek-test.sh).
 *
 * Safety: the archive is untrusted. Before anything is written the whole
 * directory of entries is read (ps5archive_inspect) and the archive is
 * refused if any entry has an absolute path, a ".." part, a link (symbolic
 * or hard), a device, FIFO or socket, or is encrypted; or if it holds more
 * than limits.max_entries entries or more than limits.max_bytes unpacked.
 * Entries are written with O_NOFOLLOW under dest only, files 0777 and
 * folders 0777 (the console won't start an app with less). Unpacked sizes
 * are checked against the free space first (statvfs).
 */
#ifndef PS5ARCHIVE_H
#define PS5ARCHIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned max_entries; /* 0: 200000 */
    uint64_t max_bytes;   /* 0: 512 GiB */
    uint64_t free_margin; /* left free on dest's drive; 0: 64 MiB */
} PS5ArchiveLimits;

typedef struct {
    char format[32];       /* "ZIP", "7-Zip", "RAR", "RAR5", "tar.gz"... (libarchive's name, shortened) */
    unsigned volumes;      /* parts in the set (1 for a single file) */
    unsigned entries;
    uint64_t unpacked;     /* bytes, the sum of the entries' sizes */
    bool encrypted;        /* refused: passwords aren't supported */
    bool sizes_known;      /* every entry had a size in its header */
    /* The one folder everything is in ("PPSA01234-app/"), else "". */
    char top_folder[256];
    /* A homebrew title in the archive: the folder (relative, "" = the root)
     * holding sce_sys/param.json and eboot.bin, else title_dir[0] == 0 and
     * has_title false. */
    bool has_title;
    char title_dir[512];
    unsigned titles;       /* how many such folders */
    bool title_icon;       /* title_dir has sce_sys/icon0.png (ShadowMountPlus
                            * won't install an image's app without one) */
} PS5ArchiveInfo;

/* An archive read through a function instead of from files (the download
 * manager's look inside one still on a server: ps5fetch_ranges, see
 * apps/child/dl-job.c). One file, no split sets. */
typedef struct {
    void* data;
    uint64_t size;
    /* length bytes at offset (fewer only at the end): the number, or -1 */
    int64_t (*read_at)(void* data, uint64_t offset, void* buffer, size_t length);
    /* why the last read failed, or NULL (may itself be NULL) */
    const char* (*error)(void* data);
} PS5ArchiveSource;

/* done and total are unpacked bytes; current the entry being written. */
typedef void (*ps5archive_progress_fn)(uint64_t done, uint64_t total, const char* current, void* data);

/* Whether path's name looks like an archive this unpacks (by extension,
 * including .7z.001, .zip.001, .part1.rar, .rar, .r00). */
bool ps5archive_is_archive(const char* path);

/* The set path belongs to, in order, as a NULL-terminated array of paths
 * (free with ps5archive_free_list). Any part of the set may be given; the
 * first is found from it (name.part01.rar, name.rar + name.r00.., name.7z.001,
 * name.zip.001 / name.z01..). Missing parts: NULL, error says which. */
char** ps5archive_volumes(const char* path, char* error, size_t error_size);
void ps5archive_free_list(char** list);

/* Reads every entry's header (no data written). 0; -1 refused (error says
 * why: encrypted, unsafe path, over the limits); -2 unreadable (not an
 * archive, a part missing, damaged). */
int ps5archive_inspect(const char* path, const PS5ArchiveLimits* limits, PS5ArchiveInfo* info, char* error,
    size_t error_size);
/* The same for a source (read_at is asked only for the parts the format
 * needs: ZIP's and 7-Zip's directory at the end, RAR's headers between the
 * files; a tar.gz is read through). */
int ps5archive_inspect_source(const PS5ArchiveSource* source, const PS5ArchiveLimits* limits, PS5ArchiveInfo* info,
    char* error, size_t error_size);

/* The file called name (a path as inspect sees it: "PPSA01234/sce_sys/
 * param.json") read into buffer, at most size - 1 bytes, NUL-terminated;
 * from the set path belongs to, or from source if it isn't NULL. 0; -1 not
 * there, too big, encrypted; -2 unreadable. */
int ps5archive_read_entry(const char* path, const PS5ArchiveSource* source, const char* name, char* buffer, size_t size,
    size_t* length, char* error, size_t error_size);

/* Unpacks the set path belongs to into dest (made if missing). Inspects
 * first (as above), then checks the free space. *cancel (if not NULL) set
 * to nonzero stops between blocks. 0; -1 refused; -2 failed (reading,
 * writing, no space); -3 cancelled. On -2 and -3 what was written stays
 * (the caller removes dest if it made it). */
int ps5archive_extract(const char* path, const char* dest, const PS5ArchiveLimits* limits,
    ps5archive_progress_fn progress, void* data, volatile int* cancel, char* error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
