/*
 * ps5install.h: a downloaded homebrew title (a folder with sce_sys/param.json
 * and eboot.bin) added to the console's home screen, in the download service
 * (apps/child/dl-job.c).
 *
 * The folder goes to <drive>/homebrew/<title ID> on the drive it is on
 * (a rename, so no copy). When a homebrew launcher runs (ShadowMountPlus,
 * Kraken Mount, TITAN-CORE: ps5launchers.h), that is all (move_only): it
 * scans that folder and registers the app itself. Otherwise it is
 * registered the way tools/launcher does it on 13.60: /user/app/<ID>/sce_sys
 * gets the title's param.json and pictures, /user/app/<ID>/mount.lnk names
 * the folder (no newline: kstuff-lite nullfs-mounts it on
 * /system_ex/app/<ID> when kstuff starts or a USB drive is plugged in;
 * etaHEN doesn't read it), and sceAppInstUtilAppInstallAll makes the
 * system register it (AppInstallTitleDir where it exists: not from 12.00
 * on, where ShadowMountPlus and Kraken Mount don't find it either).
 *
 * POSIX only; the registration only on the PS5 (__PROSPERO__), so the rest
 * builds and is tested on the Mac (port/webkit/test/downloads/install-test.sh).
 */
#ifndef PS5INSTALL_H
#define PS5INSTALL_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char title_id[16]; /* "PPSA01234" */
    char name[256];    /* the default language's titleName, else the ID */
    char version[32];  /* contentVersion ("01.000.000"), else "" */
    char content_id[48]; /* contentId ("UP9000-PPSA01234_00-..."), else "" */
    char category[16]; /* applicationCategoryType as text ("0" a game, "65536" an app), else "" */
    char dir[1024];    /* the folder with sce_sys/param.json and eboot.bin */
} PS5TitleInfo;

/* The shallowest title folder in dir (dir itself, or up to depth levels
 * below). 0 found; -1 none (error says so), -2 a param.json without a
 * valid titleId. */
int ps5install_find(const char* dir, unsigned depth, PS5TitleInfo* out, char* error, size_t error_size);

/* How many title folders dir holds (dir itself, or up to depth levels
 * below; an app's own folders aren't searched), -1 out of memory, -2 too
 * many folders on one level to search them all. */
int ps5install_count(const char* dir, unsigned depth);

/* The first empty folder in an app's folder dir, relative ("game"; the
 * names on each level sorted): a folder the app may expect files added to
 * (Hydroid's game/ for the APK), which an image (read-only) can't take.
 * 1 found (in out), 0 none, -1 too many entries to look through or an
 * error. */
int ps5install_empty_folder(const char* dir, char* out, size_t size);

/* Fills out from a param.json's text (out->dir left empty; a UTF-8 byte
 * order mark skipped). 0, or -2 without a valid titleId. */
int ps5install_parse_param(const char* text, PS5TitleInfo* out);

/* Whether id is a title ID ("PPSA01234", "CUSA01234": 4 capitals, 5 digits). */
bool ps5install_valid_id(const char* id);

/* Whether title_id is installed: its param.json in /user/appmeta (what the
 * system registered), /user/app/<ID>/sce_sys, /system_data/priv/appmeta;
 * user_app (a test's folder) instead: only <user_app>/<ID>/sce_sys. out
 * gets its name and version, dir the param.json read. */
bool ps5install_installed(const char* user_app, const char* title_id, PS5TitleInfo* out);

typedef void (*ps5install_log_fn)(const char* line, void* data);

typedef struct {
    const char* user_app;  /* NULL: "/user/app" (tests: a temporary folder) */
    bool do_register;      /* call sceAppInstUtil (PS5 only; ignored elsewhere) */
    bool move_only;        /* only move it to <drive>/homebrew/<ID>: a launcher registers it */
    ps5install_log_fn log; /* NULL: none */
    void* data;
} PS5InstallOptions;

/* Moves title->dir to <drive>/homebrew/<ID> (an existing copy there is
 * replaced: renamed aside first, deleted once the new one is in place, put
 * back if the move fails) and registers it (not with move_only). installed
 * gets the folder.
 * 0; -1 refused (error says why); -2 failed (files are in place, the
 * registration didn't work: error says so). */
int ps5install_title(const PS5TitleInfo* title, const PS5InstallOptions* options, char* installed, size_t installed_size,
    char* error, size_t error_size);

/* Where ps5install_title puts title (title->dir's drive's
 * homebrew/<ID>): 0, or -1 (not on a drive apps install from). */
int ps5install_target(const PS5TitleInfo* title, char* out, size_t size);

/* The drive a path is on: "/data", "/mnt/usb0", "/mnt/ext1"..., or NULL. */
const char* ps5install_drive_of(const char* path, char* out, size_t size);

/* Held around every call into the system's installer (sceAppInstUtil):
 * registrations here and package installs (ps5pkginstall.c). */
void ps5install_appinst_lock(void);
void ps5install_appinst_unlock(void);

/* Lock held: sceAppInstUtilInitialize (init) once for the whole service,
 * whichever caller comes first. 0 when it has been initialized (now or
 * before), else what init returned; a failure is tried again next time. */
int ps5install_appinst_initialize(int (*init)(void));

/* Deletes a folder and everything in it (no links followed). 0 or -1. */
int ps5install_remove_tree(const char* path);

#ifdef __cplusplus
}
#endif

#endif
