/*
 * ps5install.h: a downloaded homebrew title (a folder with sce_sys/param.json
 * and eboot.bin) added to the console's home screen, in the download service
 * (apps/child/dl-job.c).
 *
 * The folder goes to <drive>/homebrew/<title ID> on the drive it is on
 * (a rename, so no copy), then it is registered the way tools/launcher
 * does it on 13.60 (read off PS5Upload): /user/app/<ID>/sce_sys gets the
 * title's param.json and pictures, /user/app/<ID>/mount.lnk names the
 * folder (etaHEN mounts it when the title starts), and
 * sceAppInstUtilAppInstallAll makes the system register it (13.60 has no
 * AppInstallTitleDir; it is used where it exists).
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
    char dir[1024];    /* the folder with sce_sys/param.json and eboot.bin */
} PS5TitleInfo;

/* The shallowest title folder in dir (dir itself, or up to depth levels
 * below). 0 found; -1 none (error says so), -2 a param.json without a
 * valid titleId. */
int ps5install_find(const char* dir, unsigned depth, PS5TitleInfo* out, char* error, size_t error_size);

/* Fills out from a param.json's text (out->dir left empty). 0, or -2
 * without a valid titleId. */
int ps5install_parse_param(const char* text, PS5TitleInfo* out);

/* Whether title_id is installed: its param.json in /user/appmeta (what the
 * system registered), /user/app/<ID>/sce_sys, /system_data/priv/appmeta;
 * user_app (a test's folder) instead: only <user_app>/<ID>/sce_sys. out
 * gets its name and version, dir the param.json read. */
bool ps5install_installed(const char* user_app, const char* title_id, PS5TitleInfo* out);

typedef void (*ps5install_log_fn)(const char* line, void* data);

typedef struct {
    const char* user_app;  /* NULL: "/user/app" (tests: a temporary folder) */
    bool do_register;      /* call sceAppInstUtil (PS5 only; ignored elsewhere) */
    ps5install_log_fn log; /* NULL: none */
    void* data;
} PS5InstallOptions;

/* Moves title->dir to <drive>/homebrew/<ID> (an existing copy there is
 * replaced: renamed aside first, deleted once the new one is in place, put
 * back if the move fails) and registers it. installed gets the folder.
 * 0; -1 refused (error says why); -2 failed (files are in place, the
 * registration didn't work: error says so). */
int ps5install_title(const PS5TitleInfo* title, const PS5InstallOptions* options, char* installed, size_t installed_size,
    char* error, size_t error_size);

/* The drive a path is on: "/data", "/mnt/usb0", "/mnt/ext1"..., or NULL. */
const char* ps5install_drive_of(const char* path, char* out, size_t size);

/* Deletes a folder and everything in it (no links followed). 0 or -1. */
int ps5install_remove_tree(const char* path);

#ifdef __cplusplus
}
#endif

#endif
