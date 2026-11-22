/*
 * ps5launchers.h: the homebrew launchers running on the console, and
 * handing them an app the download service installed
 * (apps/child/dl-job.c).
 *
 * ShadowMountPlus, Kraken Mount and TITAN-CORE each scan <drive>/homebrew
 * (and other folders) every 10-15 s, and register what they find: they
 * copy the app's sce_sys to /user/app/<ID>, its metadata to
 * /user/appmeta/<ID>, nullfs-mount the folder on /system_ex/app/<ID> and
 * call sceAppInstUtilAppInstallTitleDir. When one is running, an install
 * only moves the app into a folder it scans and asks it to look, rather
 * than registering the app as well (two tools writing /user/app/<ID>).
 * Without one, kstuff-lite mounts what /user/app/<ID>/mount.lnk names, but
 * only when it starts or a USB drive is plugged in.
 *
 * Read off their sources (2026-11-21: ShadowMountPlus 1.7, Kraken Mount
 * 0.3.1, TITAN-CORE 1.0.0):
 *   ShadowMountPlus  /data/shadowmount/config.ini ([api] port, 10101;
 *                    enabled), its thread "shadowmountplus.elf", and
 *                    /data/.kstuff_noautomount while it runs. JSON API:
 *                    POST /api/v1/version, /settings (custom scan_paths),
 *                    /scan, /manual/add {"path"}, /games ({"include_size":
 *                    false}: games[] with path,
 *                    installed, mounted). It writes mount.lnk before it
 *                    registers the app, so the game list says when it has.
 *                    Mounts exFAT images. Seen on the console (2026-11-21,
 *                    1.7beta10): it holds its scans while this browser (a
 *                    PPSA title it launched) is in front, and scans as soon
 *                    as the home screen is (or the browser closes). A
 *                    second copy of a title in its library is ignored
 *                    ("Duplicate ... ignored"): the first one found stays.
 *   Kraken Mount     port 8097: GET /kraken.appcache, /api/settings
 *                    ("scanpath"), /api/state (paused; games[]: title_id,
 *                    path, status "pending" "checking" "building" "ready"
 *                    "error" "duplicate", error), POST /api/rescan,
 *                    /api/rebuild/<ID>. No exFAT images. Won't run beside
 *                    ShadowMountPlus. Its scan waits ("paused") while an app
 *                    it manages runs: this browser too, when Kraken Mount
 *                    started first (main.rs, launch.rs), so an app handed
 *                    to it is added once the browser is closed.
 *   TITAN-CORE       Kraken Mount's fork, on the same port (GET
 *                    /titan.appcache tells it apart); it starts
 *                    ShadowMountPlus too, and leaves it the titles it owns.
 * Their default scan folders: /data/homebrew, /data/etaHEN/games, and on
 * /mnt/usb0-7 and /mnt/ext0-1 homebrew, etaHEN/games and the drive's root.
 *
 * POSIX and loopback HTTP only (the process list only on the PS5), so it
 * builds and is tested on the Mac (port/webkit/test/downloads/launchers-test.sh).
 */
#ifndef PS5LAUNCHERS_H
#define PS5LAUNCHERS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS5LAUNCHERS_ROOTS 16

typedef struct {
    const char* root;  /* a prefix for the console's files (tests); NULL: none */
    int smp_port;      /* 0: from ShadowMountPlus's config.ini, else 10101 */
    int kraken_port;   /* 0: 8097 */
    int timeout_ms;    /* each request; 0: 1000 */
} PS5LaunchersOptions;

typedef struct {
    bool smp;          /* ShadowMountPlus runs (its API answered, or its thread is there) */
    bool smp_api;      /* its API answers, on smp_port */
    int smp_port;
    char smp_version[32];
    unsigned smp_root_count; /* its own scan folders (0: the defaults) */
    char smp_roots[PS5LAUNCHERS_ROOTS][256];
    bool kraken;       /* Kraken Mount answers */
    bool kraken_paused; /* ... and its scan waits for an app it manages to close */
    bool titan;        /* TITAN-CORE answers (kraken is false then) */
    int kraken_port;
    unsigned kraken_root_count; /* Kraken's or TITAN's scanpath (0: the defaults) */
    char kraken_roots[PS5LAUNCHERS_ROOTS][256];
    bool kstuff_automount; /* /data/.kstuff_noautomount isn't there */
    bool etahen;       /* /data/etaHEN is there (installed; not known to run) */
    bool forced;       /* ps5launchers_force set it, not detection */
    char found[32];    /* the scanner detection found ("" none), whatever the setting */
    char images[32];   /* what mounts exFAT images ("" none), whatever the setting */
} PS5Launchers;

/* Looks (a few loopback requests: under 2 s when something doesn't answer). */
void ps5launchers_detect(PS5Launchers* out, const PS5LaunchersOptions* options);

/* The setting's choice instead of detection: "smp", "kraken", "titan"
 * (that one counts as running, even if it didn't answer), "self" (none
 * does: the browser registers apps itself). "auto" or NULL: as detected. */
void ps5launchers_force(PS5Launchers* l, const char* which);

/* The launcher that registers app folders ("ShadowMountPlus", "TITAN-CORE",
 * "Kraken Mount"), NULL if none runs. */
const char* ps5launchers_scanner(const PS5Launchers* l);

/* What mounts exFAT images in homebrew folders (ShadowMountPlus, also
 * TITAN-CORE's), as detected; NULL: nothing. */
const char* ps5launchers_image_mounter(const PS5Launchers* l);

/* The scanner is Kraken Mount, waiting for an app it manages (this
 * browser) to close before it adds anything. */
bool ps5launchers_waits(const PS5Launchers* l);

/* Where title_id comes from: ShadowMountPlus's or Kraken Mount's library
 * entry (installed or not: a launcher keeps the first copy it found, and
 * ignores another), else (links) /user/app/<ID>/mount_img.lnk or
 * mount.lnk. FALSE: not known. */
bool ps5launchers_source(const PS5Launchers* l, const char* title_id, bool links, const PS5LaunchersOptions* options, char* out,
    size_t size);

/* Whether the scanner looks in folder's parent (folder: an app's folder).
 * FALSE: why says so. */
bool ps5launchers_scans(const PS5Launchers* l, const char* folder, char* why, size_t why_size);

/* Before an installed copy of title_id is replaced: 0, or -1 when
 * ShadowMountPlus has it mounted (it runs: error says so). It mounts a
 * folder app only while it runs, so the next start uses the new folder. */
int ps5launchers_before_replace(const PS5Launchers* l, const char* title_id, const PS5LaunchersOptions* options, char* error,
    size_t error_size);

/* Asks the scanner to look for the app now: it is at folder (title_id);
 * replaced: an installed copy was replaced (Kraken Mount rebuilds it). ShadowMountPlus
 * gets folder added to its own list when it is outside its scan folders.
 * 0 (error says what it answered, for the log); -1 (error says why: it
 * won't find it). */
int ps5launchers_announce(const PS5Launchers* l, const char* folder, const char* title_id, bool replaced,
    const PS5LaunchersOptions* options, char* error, size_t error_size);

/* Whether the app at folder (title_id) is registered by the scanner: 1 yes,
 * 0 not yet, -1 it failed (error: the launcher's reason). */
int ps5launchers_app_ready(const PS5Launchers* l, const char* folder, const char* title_id, const PS5LaunchersOptions* options,
    char* error, size_t error_size);

/* {"scanner":"…"|null (the setting applied),"found":"…" (detected),
 * "images":"…"|null,"smp":…,"smpApi":…,"kraken":…,"titan":…,"waits":…,
 * "kstuff":…,"etahen":…,"forced":…}; g_free() it. */
char* ps5launchers_json(const PS5Launchers* l);

#ifdef __cplusplus
}
#endif

#endif
