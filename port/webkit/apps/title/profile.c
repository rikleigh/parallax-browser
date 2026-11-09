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
 *     search.txt                      the Search engine setting (settings.c)
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
#include "downloads.h" /* ps5wk_downloads_free_space */
#include "profile.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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
    gint64 bytes, files; /* bytes < 0: not counted yet */
} Usage;

/* The folders are counted by the NetworkProcess: the title can open a
 * folder but not list it (getdirentries: EPERM), a payload can. Settings
 * writes a request, the NetworkProcess answers within a second
 * (answerStorageRequest in ps5wk-child.cpp; /system_tmp/ps5wk/storage-want-
 * <pid>, storage-<pid>), and settings.json says "counting" until an answer
 * newer than the last change comes, which the page polls for. Single files
 * (the lists) are counted here: lstat works. */
enum { FOLDER_CACHE, FOLDER_SITE, FOLDER_DOWNLOADS, FOLDER_OTHERS, FOLDER_SHARED, FOLDER_COUNT };
static const char* const folderIds[FOLDER_COUNT] = { "cache", "site", "downloads", "others", "shared" };

static GMutex storageLock;
static char* storageJSON;
static gint64 storageTime;
static unsigned requestGen, changedGen, answerGen; /* storageLock */
static Usage folders[FOLDER_COUNT];
static unsigned otherCount;

static char* storagePath(const char* name)
{
    return g_strdup_printf("/system_tmp/ps5wk/%s-%d", name, (int)getpid());
}

/* The NetworkProcess's answer, if newer than the one we have. */
static void readAnswer(void)
{
    char* path = storagePath("storage");
    char* text = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL)) {
        unsigned gen = 0;
        if (sscanf(text, "gen %u", &gen) == 1 && gen > answerGen) {
            answerGen = gen;
            char** lines = g_strsplit(text, "\n", -1);
            for (char** line = lines; *line; line++) {
                char id[16];
                long long bytes, files;
                unsigned count;
                if (sscanf(*line, "%15s %lld %lld %u", id, &bytes, &files, &count) != 4)
                    continue;
                for (int i = 0; i < FOLDER_COUNT; i++) {
                    if (strcmp(id, folderIds[i]))
                        continue;
                    folders[i].bytes = bytes;
                    folders[i].files = files;
                    if (i == FOLDER_OTHERS)
                        otherCount = count;
                }
            }
            g_strfreev(lines);
        }
        g_free(text);
    }
    g_free(path);
}

/* A new request, unless one is waiting. */
static void requestCount(void)
{
    char* path = storagePath("storage-want");
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        const char* mine = strrchr(profileDir, '/') + 1;
        GString* request = g_string_new(NULL);
        g_string_append_printf(request, "gen %u\n", ++requestGen);
        g_string_append_printf(request, "cache\t-\t%s/browser/cache\n", profileDir);
        g_string_append_printf(request, "site\t-\t%s/browser/data\n", profileDir);
        g_string_append_printf(request, "downloads\t-\t%s/downloads\n", profileDir);
        g_string_append_printf(request, "others\t%s\t%s\n", mine, USERS_DIR);
        g_string_append_printf(request, "shared\tusers\t%s\n", PS5WK_DATA_DIR);
        char* temporary = g_strconcat(path, ".tmp", NULL);
        if (g_file_set_contents(temporary, request->str, request->len, NULL))
            rename(temporary, path);
        g_free(temporary);
        g_string_free(request, TRUE);
    }
    g_free(path);
}

static void appendItem(GString* out, const char* id, const char* label, Usage usage)
{
    g_string_append_printf(out, "%s{\"id\":\"%s\",\"label\":", out->str[out->len - 1] == '[' ? "" : ",", id);
    ps5wk_json_string(out, label);
    g_string_append_printf(out, ",\"bytes\":%" G_GINT64_FORMAT ",\"files\":%" G_GINT64_FORMAT "}", usage.bytes, usage.files);
}

/* storageLock held. */
static char* storageState(gboolean* counting)
{
    readAnswer();
    requestCount();
    *counting = answerGen <= changedGen;
    Usage lists = { 0, 0 };
    static const char* const listFiles[] = { "bookmarks.json", "history.json", "session.json", "downloads.json", "home-url.txt", "search.txt", "adblock-allow.txt" };
    for (unsigned i = 0; i < G_N_ELEMENTS(listFiles); i++) {
        char* path = g_build_filename(profileDir, listFiles[i], NULL);
        struct stat st;
        if (!lstat(path, &st) && S_ISREG(st.st_mode)) {
            lists.bytes += st.st_size;
            lists.files++;
        }
        g_free(path);
    }
    Usage shown[FOLDER_COUNT];
    for (int i = 0; i < FOLDER_COUNT; i++)
        shown[i] = *counting ? (Usage) { -1, 0 } : folders[i];

    /* Not statvfs: the title's system libraries don't have it (the browser
     * closed when Settings opened). */
    long long freeBytes = ps5wk_downloads_free_space();
    GString* out = g_string_new("{\"user\":");
    ps5wk_json_string(out, ps5wk_profile_name());
    g_string_append(out, ",\"folder\":");
    ps5wk_json_string(out, profileDir);
    g_string_append_printf(out, ",\"free\":%lld,\"counting\":%s,\"otherUsers\":%u,\"items\":[", freeBytes, *counting ? "true" : "false", otherCount);
    appendItem(out, "cache", "Webpage cache", shown[FOLDER_CACHE]);
    appendItem(out, "site", "Cookies and site data", shown[FOLDER_SITE]);
    appendItem(out, "downloads", "Downloads", shown[FOLDER_DOWNLOADS]);
    appendItem(out, "lists", "History, bookmarks and tabs", lists);
    g_string_append(out, "],\"shared\":[");
    appendItem(out, "browser", "Browser files (fonts, ad-block lists, settings)", shown[FOLDER_SHARED]);
    appendItem(out, "others", "Other users", shown[FOLDER_OTHERS]);
    g_string_append(out, "]}");
    if (!*counting)
        spike_log("[user] storage (answer %u): cache %.1f MB (%" G_GINT64_FORMAT " files), site data %.1f MB, downloads %.1f MB, "
            "shared %.1f MB, %u other user%s %.1f MB", answerGen, folders[FOLDER_CACHE].bytes / 1e6, folders[FOLDER_CACHE].files,
            folders[FOLDER_SITE].bytes / 1e6, folders[FOLDER_DOWNLOADS].bytes / 1e6, folders[FOLDER_SHARED].bytes / 1e6, otherCount,
            otherCount == 1 ? "" : "s", folders[FOLDER_OTHERS].bytes / 1e6);
    return g_string_free(out, FALSE);
}

char* ps5wk_profile_storage_json(void)
{
    g_mutex_lock(&storageLock);
    gint64 now = g_get_monotonic_time();
    if (!storageJSON || now - storageTime > 5 * G_USEC_PER_SEC) {
        g_free(storageJSON);
        gboolean counting;
        storageJSON = storageState(&counting);
        storageTime = counting ? 0 : g_get_monotonic_time(); /* the page polls until the answer */
    }
    char* json = g_strdup(storageJSON);
    g_mutex_unlock(&storageLock);
    return json;
}

void ps5wk_profile_storage_changed(void)
{
    g_mutex_lock(&storageLock);
    storageTime = 0;
    changedGen = requestGen; /* only a request made after this counts */
    g_mutex_unlock(&storageLock);
}
