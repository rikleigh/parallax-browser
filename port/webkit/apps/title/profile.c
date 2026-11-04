/*
 * The PS5 user's own browser data (see profile.h; README: "Users and
 * storage"). One folder per PS5 user, named by the user ID the system gives
 * the title (sceUserServiceGetInitialUser: the user who started it):
 *
 *   /data/ps5webkit/users/<id, 8 hex digits>/
 *     name.txt                        the user's name, for someone looking over FTP
 *     bookmarks.json history.json session.json downloads.json   (browserdata.c)
 *     downloads/                      (downloads.c)
 *     browser/data browser/cache      WebKit's website data (cookies.sqlite,
 *                                     local storage, IndexedDB...) and HTTP cache
 *     home-url.txt                    the Home page setting (settings.c)
 *     adblock-allow.txt               sites where ads are allowed (adblock.c)
 *
 * Before users, all of that was in /data/ps5webkit itself. The first start
 * that makes /data/ps5webkit/users moves it into that user's folder (a
 * rename each, nothing copied); users who come later start empty. Without a
 * user (the system call failing) the folder is users/default.
 *
 * Main thread for init; the getters after it from any thread.
 */
#define SPIKE_NAME "ps5browser"
#include "common/spike.h"
#include "browser.h" /* ps5wk_json_string */
#include "profile.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

int sceUserServiceInitialize(const void* params) __attribute__((weak));
int sceUserServiceGetInitialUser(int32_t* user) __attribute__((weak));
int sceUserServiceGetUserName(int32_t user, char* name, size_t size) __attribute__((weak));

#define USERS_DIR PS5WK_DATA_DIR "/users"

static char* profileDir;
static char* downloadsDir;
static char* userName;

/* What moves from /data/ps5webkit into the first user's folder. */
static const char* const legacyItems[] = {
    "bookmarks.json", "history.json", "session.json", "downloads.json", "downloads",
    "browser/data", "browser/cache", "home-url.txt", "adblock-allow.txt",
};

static void makeDir(const char* path)
{
    if (mkdir(path, 0777) && errno != EEXIST)
        spike_log("[user] mkdir %s failed (errno %d)", path, errno);
    chmod(path, 0777); /* FTP can manage it too */
}

static void moveLegacyData(void)
{
    char* browserDir = g_build_filename(profileDir, "browser", NULL);
    makeDir(browserDir);
    g_free(browserDir);
    unsigned moved = 0;
    for (unsigned i = 0; i < G_N_ELEMENTS(legacyItems); i++) {
        char* from = g_build_filename(PS5WK_DATA_DIR, legacyItems[i], NULL);
        char* to = g_build_filename(profileDir, legacyItems[i], NULL);
        struct stat st;
        if (!lstat(from, &st) && lstat(to, &st)) {
            if (rename(from, to))
                spike_log("[user] could not move %s to %s (errno %d)", from, to, errno);
            else
                moved++;
        }
        g_free(to);
        g_free(from);
    }
    spike_log("[user] first start with users: %u item%s of the shared data moved to %s", moved, moved == 1 ? "" : "s", profileDir);
}

void ps5wk_profile_init(void)
{
    int32_t user = -1;
    int rc = -1;
    if (sceUserServiceInitialize)
        sceUserServiceInitialize(NULL); /* again later (the pad): that call just fails */
    if (sceUserServiceGetInitialUser)
        rc = sceUserServiceGetInitialUser(&user);
    char name[64] = { 0 };
    int nameRC = -1;
    if (!rc && user >= 0 && sceUserServiceGetUserName)
        nameRC = sceUserServiceGetUserName(user, name, sizeof name - 1);

    makeDir(PS5WK_DATA_DIR);
    gboolean firstWithUsers = mkdir(USERS_DIR, 0777) == 0;
    chmod(USERS_DIR, 0777);
    if (!rc && user >= 0)
        profileDir = g_strdup_printf(USERS_DIR "/%08x", (unsigned)user);
    else
        profileDir = g_strdup(USERS_DIR "/default");
    makeDir(profileDir);
    userName = g_strdup(*name ? name : !rc && user >= 0 ? "PS5 user" : "Everyone");
    spike_log("[user] %s (user 0x%08x, rc 0x%08x; name rc 0x%08x%s): data in %s", userName, (unsigned)user, (unsigned)rc, (unsigned)nameRC,
        sceUserServiceGetUserName ? "" : ", not linked", profileDir);
    if (firstWithUsers)
        moveLegacyData();

    downloadsDir = g_build_filename(profileDir, "downloads", NULL);
    char* namePath = g_build_filename(profileDir, "name.txt", NULL);
    char* line = g_strdup_printf("%s\n", userName);
    g_file_set_contents(namePath, line, -1, NULL);
    chmod(namePath, 0666);
    g_free(line);
    g_free(namePath);
}

const char* ps5wk_profile_dir(void)
{
    return profileDir ? profileDir : USERS_DIR "/default";
}

const char* ps5wk_profile_downloads_dir(void)
{
    return downloadsDir ? downloadsDir : USERS_DIR "/default/downloads";
}

const char* ps5wk_profile_name(void)
{
    return userName ? userName : "Everyone";
}

/* ---------------------------------------------------------------- storage */

typedef struct {
    guint64 bytes, files;
} Usage;

/* Every file under path (not following links); skip: a name not to enter
 * at the top level. */
static void addUsage(Usage* usage, const char* path, const char* skip)
{
    struct stat st;
    if (lstat(path, &st))
        return;
    if (!S_ISDIR(st.st_mode)) {
        usage->bytes += (guint64)st.st_size;
        usage->files++;
        return;
    }
    GDir* dir = g_dir_open(path, 0, NULL);
    if (!dir)
        return;
    for (const char* name; (name = g_dir_read_name(dir));) {
        if (skip && !strcmp(name, skip))
            continue;
        char* child = g_build_filename(path, name, NULL);
        addUsage(usage, child, NULL);
        g_free(child);
    }
    g_dir_close(dir);
}

static Usage usageOf(const char* name)
{
    Usage usage = { 0, 0 };
    char* path = g_build_filename(profileDir, name, NULL);
    addUsage(&usage, path, NULL);
    g_free(path);
    return usage;
}

static GMutex storageLock;
static char* storageJSON;
static gint64 storageTime;

static void appendItem(GString* out, const char* id, const char* label, Usage usage)
{
    g_string_append_printf(out, "%s{\"id\":\"%s\",\"label\":", out->str[out->len - 1] == '[' ? "" : ",", id);
    ps5wk_json_string(out, label);
    g_string_append_printf(out, ",\"bytes\":%" G_GUINT64_FORMAT ",\"files\":%" G_GUINT64_FORMAT "}", usage.bytes, usage.files);
}

static char* countStorage(void)
{
    gint64 start = g_get_monotonic_time();
    Usage cache = usageOf("browser/cache"), site = usageOf("browser/data"), downloads = usageOf("downloads");
    Usage lists = { 0, 0 };
    static const char* const listFiles[] = { "bookmarks.json", "history.json", "session.json", "downloads.json", "home-url.txt", "adblock-allow.txt" };
    for (unsigned i = 0; i < G_N_ELEMENTS(listFiles); i++) {
        Usage u = usageOf(listFiles[i]);
        lists.bytes += u.bytes;
        lists.files += u.files;
    }
    /* The other users' folders, and everything outside users/. */
    Usage others = { 0, 0 }, shared = { 0, 0 };
    unsigned otherCount = 0;
    const char* mine = strrchr(profileDir, '/') + 1;
    GDir* dir = g_dir_open(USERS_DIR, 0, NULL);
    for (const char* name; dir && (name = g_dir_read_name(dir));) {
        if (!strcmp(name, mine))
            continue;
        char* path = g_build_filename(USERS_DIR, name, NULL);
        struct stat st;
        if (!lstat(path, &st) && S_ISDIR(st.st_mode)) {
            addUsage(&others, path, NULL);
            otherCount++;
        }
        g_free(path);
    }
    if (dir)
        g_dir_close(dir);
    addUsage(&shared, PS5WK_DATA_DIR, "users");

    struct statvfs fs;
    long long freeBytes = !statvfs(PS5WK_DATA_DIR, &fs) ? (long long)fs.f_bavail * (long long)fs.f_frsize : -1;
    GString* out = g_string_new("{\"user\":");
    ps5wk_json_string(out, ps5wk_profile_name());
    g_string_append(out, ",\"folder\":");
    ps5wk_json_string(out, profileDir);
    g_string_append_printf(out, ",\"free\":%lld,\"otherUsers\":%u,\"items\":[", freeBytes, otherCount);
    appendItem(out, "cache", "Webpage cache", cache);
    appendItem(out, "site", "Cookies and site data", site);
    appendItem(out, "downloads", "Downloads", downloads);
    appendItem(out, "lists", "History, bookmarks and tabs", lists);
    g_string_append(out, "],\"shared\":[");
    appendItem(out, "browser", "Browser files (fonts, ad-block lists, settings)", shared);
    appendItem(out, "others", "Other users", others);
    g_string_append(out, "]}");
    spike_log("[user] storage counted in %.0f ms: cache %.1f MB (%" G_GUINT64_FORMAT " files), site data %.1f MB, downloads %.1f MB, "
        "shared %.1f MB, %u other user%s %.1f MB", (g_get_monotonic_time() - start) / 1000.0, cache.bytes / 1e6, cache.files,
        site.bytes / 1e6, downloads.bytes / 1e6, shared.bytes / 1e6, otherCount, otherCount == 1 ? "" : "s", others.bytes / 1e6);
    return g_string_free(out, FALSE);
}

char* ps5wk_profile_storage_json(void)
{
    g_mutex_lock(&storageLock);
    gint64 now = g_get_monotonic_time();
    if (!storageJSON || now - storageTime > 5 * G_USEC_PER_SEC) {
        g_free(storageJSON);
        storageJSON = countStorage();
        storageTime = g_get_monotonic_time();
    }
    char* json = g_strdup(storageJSON);
    g_mutex_unlock(&storageLock);
    return json;
}

void ps5wk_profile_storage_changed(void)
{
    g_mutex_lock(&storageLock);
    storageTime = 0;
    g_mutex_unlock(&storageLock);
}
