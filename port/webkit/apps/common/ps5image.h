/*
 * ps5image.h: what a downloaded image or package holds, for the download
 * service (apps/child/dl-job.c): the app's name, title ID, version and
 * icon, out of an exFAT image (.exfat), a UFS2 image (.ffpkg), a PS4
 * package (CNT) or a PS5 package (FIH, with its CNT inside).
 *
 * The format comes from the file's first bytes, never its name:
 *   "\x7FCNT" at 0              a PS4 package (fake: pkg_type's bit 31 clear)
 *   "\x7FFIH" at 0              a PS5 package; its CNT at the u64 at 0x58
 *                               (none there: a retail package, encrypted);
 *                               byte 5 0x80: signed by Sony (retail)
 *   "EXFAT   " at 3, 55 AA      exFAT
 *   0x19540119 at 65536+1372    UFS2
 *   PFS (version 1 or 2, magic 20130315 at 8): known, not read
 * In a package: the big-endian header (entry count 0x10, entry table 0x18,
 * content ID 0x40, pkg_type 0x04) and its 32-byte entries, found by ID
 * (0x1000 param.sfo, 0x2000 param.json, 0x1200 icon0.png) or by name
 * (the 0x0200 name table). Layout from pkg-viewer (Loopayeh, MIT) and
 * psdevwiki's PKG page. In an image: sce_sys/param.json (else
 * sce_sys/param.sfo) and sce_sys/icon0.png at its root.
 *
 * POSIX only, so it builds for the PS5 and on the Mac (its test:
 * port/webkit/test/downloads/image-test.sh).
 */
#ifndef PS5IMAGE_H
#define PS5IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PS5IMAGE_UNKNOWN = 0,
    PS5IMAGE_EXFAT,
    PS5IMAGE_UFS2,
    PS5IMAGE_PFS,
    PS5IMAGE_PKG_PS4,
    PS5IMAGE_PKG_PS5,
} PS5ImageKind;

typedef struct {
    PS5ImageKind kind;
    const char* format;   /* "exfat", "ffpkg", "pfs", "pkg" */
    const char* label;    /* "exFAT image", "UFS2 image", "PS4 fake package"... */
    char platform[8];     /* "PS5", "PS4", "" (not known) */
    char title_id[16];
    char content_id[48];
    char name[256];       /* the title's name; else "" */
    char version[32];     /* "01.000.000" (PS5), "01.00" (PS4; a patch: APP_VER) */
    char category[16];    /* PS4: CATEGORY ("gd" a game, "gp" a patch, "ac" add-on); PS5: applicationCategoryType */
    bool package;         /* a .pkg (PS4 or PS5) */
    bool fake;            /* a package not signed by Sony (installs with the fPKG patches) */
    bool encrypted;       /* a package whose entries can't be read (retail): only the header's ID */
    bool app_at_root;     /* an image with sce_sys/param.json (or param.sfo) and eboot.bin at its root */
    bool has_icon;
    uint64_t icon_offset; /* a package: icon0.png's place in the file */
    uint64_t icon_size;
    uint64_t size;        /* the file's */
} PS5ImageInfo;

/* What path holds. 0 (out filled as far as it can be read: a package
 * whose param.sfo is encrypted still has its content ID); -1 not a format
 * this knows, or unreadable (error says which). */
int ps5image_info(const char* path, PS5ImageInfo* out, char* error, size_t error_size);

/* Copies the app's icon0.png out of path (an image or a package) to
 * out_png (written to out_png.part first, then renamed). The bytes must
 * start like a PNG and be at most 4 MiB. 0, or -1 (error says why). */
int ps5image_icon(const char* path, const char* out_png, char* error, size_t error_size);

/* Reads a param.sfo's TITLE, TITLE_ID, CONTENT_ID, CATEGORY and
 * VERSION/APP_VER into out. 0, or -1 not a param.sfo. */
int ps5image_parse_sfo(const unsigned char* data, size_t length, PS5ImageInfo* out);

/* {"kind":"pkg","label":…,"platform":…,"name":…,"id":…,"contentId":…,
 * "version":…,"category":…,"fake":…,"encrypted":…,"appAtRoot":…,
 * "icon":…,"size":…}; free() it. */
char* ps5image_json(const PS5ImageInfo* info);

/* Whether name ends in an image extension the console's mounters know
 * (.exfat, .ffpkg) or is a package (.pkg); case ignored. */
bool ps5image_is_image_name(const char* name);
bool ps5image_is_pkg_name(const char* name);

#ifdef __cplusplus
}
#endif

#endif
