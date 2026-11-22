/*
 * ps5pkginstall.h: a .pkg on a drive handed to the console's installer, in
 * the download service (apps/child/dl-job.c). PS4 packages and PS5 fake
 * packages (fPKG) both go this way.
 *
 * The installer (sceAppInstUtilInstallByPackage) takes a package by URL,
 * says yes or no at once, then pulls it over HTTP on its own threads:
 * Range requests on several connections, keep-alive, HEAD, sometimes a
 * <name>.crc beside it. So the package is served on 127.0.0.1, on a port
 * the system picks, under a random token
 * (http://127.0.0.1:<port>/<token>/<name>.pkg, at most 127 bytes: the
 * installer's URL field), until every byte has been served once and
 * nothing more is asked for 2 s; nothing asked for 5 minutes ends it as
 * stalled. The service has to keep running till then.
 *
 * The call, its structures and credentials follow PS5 Dump Forge's
 * ps5/installer.c (quer3q/ps5-dump-forge, GPL-3.0), itself after
 * ps5upload's installer: the SYSTEM authid 0x4801000000000013 with every
 * capability and attrs 0x80 for the call, ShellCore's 0x3800000000000010
 * when the SYSTEM one is refused at preflight (0x80B22404), the 0x70-byte
 * MetaInfo zeroed but for the URL and the name. Installing a fake package
 * also needs the console's fPKG patches (kstuff with fpkg enabled).
 *
 * POSIX sockets and threads; the installer call only on the PS5
 * (__PROSPERO__). Elsewhere a test passes its own installer
 * (port/webkit/test/downloads/pkginstall-test.sh).
 */
#ifndef PS5PKGINSTALL_H
#define PS5PKGINSTALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* url accepted: 0; refused: -1 with the installer's code in *code. */
typedef int (*ps5pkginstall_installer_fn)(const char* url, const char* name, unsigned* code, void* data);
typedef void (*ps5pkginstall_progress_fn)(uint64_t served, uint64_t total, void* data);
typedef void (*ps5pkginstall_log_fn)(const char* line, void* data);

typedef struct {
    const char* path;        /* the package */
    const char* name;        /* what the console's download list shows; NULL: the file's name */
    ps5pkginstall_installer_fn installer; /* NULL: the console's (PS5 only) */
    void* installer_data;
    void (*on_url)(const char* url, void* data); /* tests: the URL, once the server listens */
    ps5pkginstall_progress_fn progress; /* bytes served at least once (every 250 ms) */
    ps5pkginstall_log_fn log;
    void* data;              /* for on_url, progress and log */
    volatile int* cancel;
    unsigned stall_ms;       /* nothing asked for this long: stalled; 0: 5 minutes */
    unsigned settle_ms;      /* every byte served and nothing asked for this long: done; 0: 2 s */
} PS5PkgInstallOptions;

/* Serves the package and hands it to the installer. 0 every byte was
 * served (the console has it; it installs it from its download list); -1
 * refused (*code: the installer's answer, error says what it means); -2
 * failed (the file changed, the console stopped asking); -3 cancelled. */
int ps5pkginstall_run(const PS5PkgInstallOptions* options, unsigned* code, char* error, size_t error_size);

/* What an installer code means, in a few words; NULL: not known. */
const char* ps5pkginstall_hint(unsigned code);

#ifdef __cplusplus
}
#endif

#endif
