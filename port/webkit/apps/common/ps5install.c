/*
 * ps5install.c: see ps5install.h.
 *
 * param.json is read with a small scanner, not a JSON parser: only three
 * strings are needed (titleId, contentVersion, and the default language's
 * titleName), and a title whose param.json the scanner can't read would not
 * start anyway. A title ID is four capital letters and five digits.
 */
#include "ps5install.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__PROSPERO__)
#include <ps5/kernel.h>
int sceKernelLoadStartModule(const char* path, size_t argc, const void* argv, uint32_t flags, void* opt, int* res);
#endif

#define PARAM_MAX (256 * 1024)

static void say(const PS5InstallOptions* options, const char* format, ...)
{
    if (!options || !options->log)
        return;
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    options->log(line, options->data);
}

static void fail(char* error, size_t size, const char* format, ...)
{
    if (!error || !size)
        return;
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
}

static bool isFile(const char* path)
{
    struct stat st;
    return !lstat(path, &st) && S_ISREG(st.st_mode);
}

static bool isDir(const char* path)
{
    struct stat st;
    return !lstat(path, &st) && S_ISDIR(st.st_mode);
}

/* ---- param.json ---- */

static char* readSmall(const char* path)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return NULL;
    char* text = malloc(PARAM_MAX + 1);
    size_t n = 0;
    while (text && n < PARAM_MAX) {
        ssize_t got = read(fd, text + n, PARAM_MAX - n);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        n += (size_t)got;
    }
    close(fd);
    if (text)
        text[n] = 0;
    return text;
}

/* The string value after "key": at or after from (escapes \" \\ \/ kept
 * simple; \uXXXX left as is). NULL if none. */
static const char* stringAfter(const char* from, const char* key, char* out, size_t size)
{
    char quoted[80];
    snprintf(quoted, sizeof quoted, "\"%s\"", key);
    for (const char* p = strstr(from, quoted); p; p = strstr(p + 1, quoted)) {
        const char* q = p + strlen(quoted);
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
            q++;
        if (*q != ':')
            continue;
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
            q++;
        if (*q != '"')
            continue;
        q++;
        size_t n = 0;
        while (*q && *q != '"' && n + 1 < size) {
            if (*q == '\\' && q[1]) {
                q++;
                out[n++] = *q == 'n' ? ' ' : *q;
            } else
                out[n++] = *q;
            q++;
        }
        out[n] = 0;
        return q;
    }
    return NULL;
}

static bool validTitleID(const char* id)
{
    if (strlen(id) != 9)
        return false;
    for (int i = 0; i < 4; i++)
        if (id[i] < 'A' || id[i] > 'Z')
            return false;
    for (int i = 4; i < 9; i++)
        if (id[i] < '0' || id[i] > '9')
            return false;
    return true;
}

/* 0, -1 not a title folder, -2 a param.json without a valid titleId. */
static int probe(const char* dir, PS5TitleInfo* out)
{
    char param[1200], eboot[1200];
    snprintf(param, sizeof param, "%s/sce_sys/param.json", dir);
    snprintf(eboot, sizeof eboot, "%s/eboot.bin", dir);
    if (!isFile(param) || !isFile(eboot))
        return -1;
    char* text = readSmall(param);
    if (!text)
        return -2;
    int r = ps5install_parse_param(text, out);
    free(text);
    snprintf(out->dir, sizeof out->dir, "%s", dir);
    return r;
}

int ps5install_parse_param(const char* text, PS5TitleInfo* out)
{
    memset(out, 0, sizeof *out);
    if (!text || !stringAfter(text, "titleId", out->title_id, sizeof out->title_id) || !validTitleID(out->title_id)) {
        out->title_id[0] = 0;
        return -2;
    }
    stringAfter(text, "contentVersion", out->version, sizeof out->version);
    char language[32] = "";
    stringAfter(text, "defaultLanguage", language, sizeof language);
    if (*language) {
        char key[40];
        snprintf(key, sizeof key, "\"%s\"", language);
        /* the language's object ("en-US": {...}), not defaultLanguage's value */
        const char* block = NULL;
        for (const char* p = strstr(text, key); p && !block; p = strstr(p + 1, key)) {
            const char* q = p + strlen(key);
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
                q++;
            if (*q++ != ':')
                continue;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
                q++;
            if (*q == '{')
                block = q;
        }
        /* its titleName, if it comes before that object closes */
        if (block) {
            const char* end = strchr(block, '}');
            char name[256];
            const char* after = stringAfter(block, "titleName", name, sizeof name);
            if (after && (!end || after <= end + 1))
                snprintf(out->name, sizeof out->name, "%s", name);
        }
    }
    if (!*out->name)
        stringAfter(text, "titleName", out->name, sizeof out->name);
    if (!*out->name)
        snprintf(out->name, sizeof out->name, "%s", out->title_id);
    return 0;
}

/* /user/app/<ID> holds an installed app, not what an install that failed
 * left: ShadowMountPlus, refused at the icon, leaves sce_sys/param.json
 * alone there (2026-11-21). An installed one has its mount link (kstuff's
 * mount.lnk, ShadowMountPlus's mount_img.lnk) or its icon. */
static bool userAppComplete(const char* user_app, const char* title_id)
{
    static const char* const signs[] = { "mount.lnk", "mount_img.lnk", "icon0.png", "sce_sys/icon0.png" };
    for (unsigned i = 0; i < sizeof signs / sizeof *signs; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s/%s", user_app, title_id, signs[i]);
        if (isFile(path))
            return true;
    }
    return false;
}

bool ps5install_installed(const char* user_app, const char* title_id, PS5TitleInfo* out)
{
    if (!title_id || !validTitleID(title_id))
        return false;
    /* the system's copy of what it registered (the version the home screen
     * shows; on the console /user/app/<ID>/sce_sys can be older, and
     * /system_data/priv/appmeta older still), then ours and the launchers' */
    char paths[3][1200];
    unsigned n = 0;
    if (!user_app)
        snprintf(paths[n++], sizeof paths[0], "/user/appmeta/%s/param.json", title_id);
    snprintf(paths[n++], sizeof paths[0], "%s/%s/sce_sys/param.json", user_app ? user_app : "/user/app", title_id);
    if (!user_app)
        snprintf(paths[n++], sizeof paths[0], "/system_data/priv/appmeta/%s/param.json", title_id);
    const unsigned userApp = user_app ? 0 : 1; /* paths[userApp] is in /user/app */
    for (unsigned i = 0; i < n; i++) {
        if (!isFile(paths[i]))
            continue;
        if (i == userApp && !userAppComplete(user_app ? user_app : "/user/app", title_id))
            continue; /* a failed install's leftover */
        char* text = readSmall(paths[i]);
        PS5TitleInfo found;
        int r = text ? ps5install_parse_param(text, &found) : -2;
        free(text);
        if (r || strcmp(found.title_id, title_id))
            continue;
        if (out) {
            *out = found;
            snprintf(out->dir, sizeof out->dir, "%s", paths[i]);
        }
        return true;
    }
    return false;
}

/* Breadth first, so the shallowest title wins. */
typedef char FindPath[1024];
#define FIND_WIDTH 64

int ps5install_find(const char* dir, unsigned depth, PS5TitleInfo* out, char* error, size_t error_size)
{
    /* on the heap: a worker thread's stack is small on the PS5 */
    FindPath* level = calloc(FIND_WIDTH, sizeof(FindPath));
    FindPath* next = calloc(FIND_WIDTH, sizeof(FindPath));
    if (!level || !next) {
        free(level);
        free(next);
        fail(error, error_size, "Out of memory");
        return -1;
    }
    unsigned count = 0, bad = 0;
    snprintf(level[count++], sizeof level[0], "%s", dir);
    for (unsigned d = 0; d <= depth && count; d++) {
        unsigned nextCount = 0;
        for (unsigned i = 0; i < count; i++) {
            int r = probe(level[i], out);
            if (!r) {
                free(level);
                free(next);
                return 0;
            }
            if (r == -2)
                bad++;
            if (d == depth)
                continue;
            DIR* folder = opendir(level[i]);
            if (!folder)
                continue;
            struct dirent* entry;
            while ((entry = readdir(folder)) && nextCount < FIND_WIDTH) {
                if (entry->d_name[0] == '.' || !strcmp(entry->d_name, "sce_sys"))
                    continue;
                char path[1024];
                if ((size_t)snprintf(path, sizeof path, "%s/%s", level[i], entry->d_name) >= sizeof path || !isDir(path))
                    continue;
                snprintf(next[nextCount++], sizeof next[0], "%s", path);
            }
            closedir(folder);
        }
        memcpy(level, next, sizeof next[0] * nextCount);
        count = nextCount;
    }
    free(level);
    free(next);
    if (bad)
        fail(error, error_size, "The title's param.json has no valid title ID");
    else
        fail(error, error_size, "No app here: no folder with sce_sys/param.json and eboot.bin");
    return bad ? -2 : -1;
}

/* Wider than find's: a count cut short would be wrong (-2 then). */
#define COUNT_WIDTH 256

int ps5install_count(const char* dir, unsigned depth)
{
    FindPath* level = calloc(COUNT_WIDTH, sizeof(FindPath));
    FindPath* next = calloc(COUNT_WIDTH, sizeof(FindPath));
    if (!level || !next) {
        free(level);
        free(next);
        return -1;
    }
    unsigned count = 0, titles = 0;
    int cut = 0;
    snprintf(level[count++], sizeof level[0], "%s", dir);
    for (unsigned d = 0; d <= depth && count && !cut; d++) {
        unsigned nextCount = 0;
        for (unsigned i = 0; i < count; i++) {
            PS5TitleInfo info;
            if (!probe(level[i], &info)) {
                titles++; /* an app's own folders aren't searched */
                continue;
            }
            if (d == depth)
                continue;
            DIR* folder = opendir(level[i]);
            if (!folder)
                continue;
            struct dirent* entry;
            while ((entry = readdir(folder))) {
                if (entry->d_name[0] == '.' || !strcmp(entry->d_name, "sce_sys"))
                    continue;
                char path[1024];
                if ((size_t)snprintf(path, sizeof path, "%s/%s", level[i], entry->d_name) >= sizeof path || !isDir(path))
                    continue;
                if (nextCount == COUNT_WIDTH) {
                    cut = 1;
                    break;
                }
                snprintf(next[nextCount++], sizeof next[0], "%s", path);
            }
            closedir(folder);
        }
        memcpy(level, next, sizeof next[0] * nextCount);
        count = nextCount;
    }
    free(level);
    free(next);
    if (cut)
        return -2;
    return (int)titles;
}

/* ---- files ---- */

const char* ps5install_drive_of(const char* path, char* out, size_t size)
{
    if (!strncmp(path, "/data/", 6) || !strcmp(path, "/data")) {
        snprintf(out, size, "/data");
        return out;
    }
    static const char* const prefixes[] = { "/mnt/usb", "/mnt/ext" };
    for (unsigned i = 0; i < 2; i++) {
        size_t n = strlen(prefixes[i]);
        if (!strncmp(path, prefixes[i], n) && path[n] >= '0' && path[n] <= '9' && (path[n + 1] == '/' || !path[n + 1])) {
            snprintf(out, size, "%.*s", (int)n + 1, path);
            return out;
        }
    }
#if !defined(__PROSPERO__)
    /* tests on the Mac: the drive is the environment's PS5INSTALL_DRIVE */
    const char* test = getenv("PS5INSTALL_DRIVE");
    if (test && !strncmp(path, test, strlen(test))) {
        snprintf(out, size, "%s", test);
        return out;
    }
#endif
    return NULL;
}

int ps5install_remove_tree(const char* path)
{
    struct stat st;
    if (lstat(path, &st))
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    DIR* folder = opendir(path);
    if (!folder)
        return -1;
    int r = 0;
    struct dirent* entry;
    while ((entry = readdir(folder))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char child[1200];
        if ((size_t)snprintf(child, sizeof child, "%s/%s", path, entry->d_name) >= sizeof child) {
            r = -1;
            continue;
        }
        if (ps5install_remove_tree(child))
            r = -1;
    }
    closedir(folder);
    if (rmdir(path))
        r = -1;
    return r;
}

static int copyFile(const char* from, const char* to)
{
    int in = open(from, O_RDONLY | O_NOFOLLOW);
    if (in < 0)
        return -1;
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buffer[64 * 1024];
    int r = 0;
    for (;;) {
        ssize_t n = read(in, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            r = -1;
        if (n <= 0)
            break;
        for (ssize_t done = 0; done < n;) {
            ssize_t w = write(out, buffer + done, (size_t)(n - done));
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0) {
                r = -1;
                break;
            }
            done += w;
        }
        if (r)
            break;
    }
    close(in);
    if (close(out))
        r = -1;
    return r;
}

/* ---- registration (PS5) ---- */

#if defined(__PROSPERO__)
/* libSceAppInstUtil isn't loaded in payloads: loaded on first use, its
 * functions found by the kernel-side lookup (sceKernelDlsym refuses them),
 * as tools/launcher/launcher.c does. */
static int (*appInstInit)(void);
static int (*appInstTitleDir)(const char* titleID, const char* installPath, void* reserved);
static int (*appInstAll)(void* reserved);

static int loadAppInstUtil(const PS5InstallOptions* options)
{
    static int loaded = 1; /* 1: not tried */
    if (loaded <= 0)
        return loaded;
    static const char* const paths[] = { "/system/common/lib/libSceAppInstUtil.sprx", "/system/priv/lib/libSceAppInstUtil.sprx" };
    for (size_t i = 0; i < sizeof paths / sizeof *paths && !appInstTitleDir && !appInstAll; i++) {
        int res = 0;
        int handle = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, &res);
        if (handle < 0) {
            say(options, "loading %s: 0x%08x", paths[i], (unsigned)handle);
            continue;
        }
        void* init = (void*)kernel_dynlib_dlsym(getpid(), (uint32_t)handle, "sceAppInstUtilInitialize");
        void* dir = (void*)kernel_dynlib_dlsym(getpid(), (uint32_t)handle, "sceAppInstUtilAppInstallTitleDir");
        void* all = (void*)kernel_dynlib_dlsym(getpid(), (uint32_t)handle, "sceAppInstUtilAppInstallAll");
        if (init && !appInstInit)
            appInstInit = (int (*)(void))init;
        if (dir)
            appInstTitleDir = (int (*)(const char*, const char*, void*))dir;
        if (all)
            appInstAll = (int (*)(void*))all;
    }
    if (!appInstInit || (!appInstTitleDir && !appInstAll)) {
        loaded = -1;
        return loaded;
    }
    int r = appInstInit();
    say(options, "sceAppInstUtilInitialize: 0x%08x", (unsigned)r);
    loaded = r ? r : 0;
    return loaded;
}
#endif

static int registerTitle(const char* titleID, const char* folder, const PS5InstallOptions* options, char* error, size_t error_size)
{
    const char* userApp = options && options->user_app ? options->user_app : "/user/app";
    char path[1200], to[1200];
    snprintf(path, sizeof path, "%s/%s", userApp, titleID);
    mkdir(path, 0777);
    snprintf(path, sizeof path, "%s/%s/sce_sys", userApp, titleID);
    mkdir(path, 0777);
    if (!isDir(path)) {
        fail(error, error_size, "Couldn't make %s (errno %d)", path, errno);
        return -2;
    }
    static const char* const files[] = { "param.json", "icon0.png", "pic0.png", "pic1.png", "icon0.dds", "pic0.dds", "pic1.dds" };
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        snprintf(path, sizeof path, "%s/sce_sys/%s", folder, files[i]);
        snprintf(to, sizeof to, "%s/%s/sce_sys/%s", userApp, titleID, files[i]);
        if (!isFile(path))
            continue;
        if (copyFile(path, to) && i == 0) {
            fail(error, error_size, "Couldn't copy param.json to %s (errno %d)", to, errno);
            return -2;
        }
    }
    snprintf(path, sizeof path, "%s/%s/mount.lnk", userApp, titleID);
    FILE* link = fopen(path, "w");
    if (!link || fputs(folder, link) < 0 || fclose(link)) {
        fail(error, error_size, "Couldn't write %s (errno %d)", path, errno);
        return -2;
    }
#if defined(__PROSPERO__)
    if (options && options->do_register) {
        int r = loadAppInstUtil(options);
        if (r) {
            fail(error, error_size, "The system's installer couldn't be loaded (0x%08x)", (unsigned)r);
            return -2;
        }
        const char* how = appInstTitleDir ? "AppInstallTitleDir" : "AppInstallAll";
        r = appInstTitleDir ? appInstTitleDir(titleID, "/user/app/", NULL) : appInstAll(NULL);
        say(options, "sceAppInstUtil%s(%s): 0x%08x", how, titleID, (unsigned)r);
        if (r) {
            fail(error, error_size, "The system's installer refused it (sceAppInstUtil%s 0x%08x)", how, (unsigned)r);
            return -2;
        }
    }
#endif
    return 0;
}

int ps5install_target(const PS5TitleInfo* title, char* out, size_t size)
{
    char drive[512];
    if (!ps5install_drive_of(title->dir, drive, sizeof drive))
        return -1;
    snprintf(out, size, "%s/homebrew/%s", drive, title->title_id);
    return 0;
}

int ps5install_title(const PS5TitleInfo* title, const PS5InstallOptions* options, char* installed, size_t installed_size,
    char* error, size_t error_size)
{
    char drive[512];
    if (!ps5install_drive_of(title->dir, drive, sizeof drive)) {
        fail(error, error_size, "Apps can be installed from internal storage, M.2 or USB drives only");
        return -1;
    }
    char homebrew[600], target[640];
    snprintf(homebrew, sizeof homebrew, "%s/homebrew", drive);
    snprintf(target, sizeof target, "%s/%s", homebrew, title->title_id);
    if (strcmp(title->dir, target)) {
        mkdir(homebrew, 0777);
        if (!strncmp(target, title->dir, strlen(title->dir)) && target[strlen(title->dir)] == '/') {
            fail(error, error_size, "The app's folder holds its own install folder");
            return -1;
        }
        char aside[700] = "";
        if (isDir(target) || isFile(target)) {
            snprintf(aside, sizeof aside, "%s.old-%ld", target, (long)time(NULL));
            if (rename(target, aside)) {
                fail(error, error_size, "Couldn't move the installed copy aside (errno %d)", errno);
                return -1;
            }
            say(options, "the installed copy went to %s", aside);
        }
        if (rename(title->dir, target)) {
            int e = errno;
            if (*aside)
                rename(aside, target);
            fail(error, error_size, e == EXDEV ? "The app and %s are on different drives" : "Couldn't move the app to %s (errno %d)", target, e);
            return -1;
        }
        say(options, "moved %s to %s", title->dir, target);
        if (*aside && ps5install_remove_tree(aside))
            say(options, "the old copy %s couldn't be deleted entirely", aside);
    }
    if (installed && installed_size)
        snprintf(installed, installed_size, "%s", target);
    if (options && options->move_only) {
        say(options, "moved %s (%s %s) to %s, for the launcher to register", title->title_id, title->name, title->version, target);
        return 0;
    }
    int r = registerTitle(title->title_id, target, options, error, error_size);
    if (!r)
        say(options, "installed %s (%s %s) from %s", title->title_id, title->name, title->version, target);
    return r;
}
