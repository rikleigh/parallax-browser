/*
 * ps5peek.h: what an archive holds, before or after it is downloaded (the
 * download manager's suggestion: keep it, unpack it, install the app in
 * it). In the download service (apps/child/dl-job.c, "peek").
 *
 * On a server it reads only the parts that list the files (ps5fetch_ranges:
 * Range requests), within max_bytes and timeout_ms: a ZIP's or 7-Zip's
 * directory is at the end (a few requests); RAR keeps a header before each
 * file (one request per file); a tar.gz, a RAR set's later parts and the
 * files inside 7-Zip's solid blocks mostly don't fit. On a drive it reads
 * the whole set (ps5archive_inspect).
 *
 * The app: the folder with sce_sys/param.json and eboot.bin (as
 * ps5archive_inspect finds it), its param.json read for the title ID, name
 * and version; if that can't be read, a title ID in the folder's or the
 * archive's name ("guessed"). Then whether that ID is installed, and which
 * version (ps5install_installed).
 */
#ifndef PS5PEEK_H
#define PS5PEEK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* a server's file ... */
    const char* url;
    uint64_t size;
    const char* user_agent;
    const char* referer;
    const char* cookies; /* cookies.txt lines */
    unsigned timeout_ms; /* 0: 10000 */
    uint64_t max_bytes;  /* 0: 24 MiB */
    /* ... or a drive's (url NULL) */
    const char* path;
    /* where installed apps are looked for; NULL: the console's (tests: a folder) */
    const char* user_app;
} PS5PeekOptions;

/* JSON: {"format","entries","unpacked","sizesKnown","titles","titleDir",
 * "titleId","name","version","guessed","installed","refused","remote",
 * "requests","fetched","ms"} (free with free()). "refused": why it can't be
 * unpacked (encrypted, unsafe paths, over the limits), else null. NULL when
 * it couldn't be read (error says why: not an archive, too slow, a server
 * that won't answer ranges). */
char* ps5peek_json(const PS5PeekOptions* options, char* error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
