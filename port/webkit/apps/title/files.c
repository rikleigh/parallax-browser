/*
 * The menu's Files list: see files.h.
 *
 * At the top, the drives (the service's "roots"), the favourite folders and
 * this user's Downloads. In a folder, the first row is the folder itself
 * (X: its options: a new folder, favourite, where downloads go); then its
 * folders and files (the service's "ls": folders first). X on a folder
 * opens it (R2: its options), X on a file or an app folder shows what can
 * be done with it, Square deletes (a second press), O goes up.
 *
 * Moving, copying and choosing a download's folder work by going to the
 * folder: the first row then says "Move <name> here" and X does it.
 *
 * Listing is asynchronous (ps5wk_dlservice_request_async); a reply for a
 * place no longer shown is dropped (generation).
 */
#define SPIKE_NAME "ps5browser"
#include "common/spike.h"
#include "files.h"
#include "browser.h"
#include "dlservice.h"
#include "downloads.h"
#include "minijson.h"
#include "profile.h"

#include <stdarg.h>
#include <string.h>

static WPEDisplayPS5* display;
static char* place;    /* the folder shown; NULL: the drives */
static MJ* listing;    /* "roots" (an array) or "ls" (an object) for place */
static gboolean listed; /* listing is place's */
static gboolean loading;
static char* loadError;
static unsigned generation;
static char* focusName; /* the entry to focus once listed */

enum { MODE_NONE, MODE_MOVE, MODE_COPY, MODE_PICK, MODE_DEFAULT }; /* default: the folder new downloads go to */
static int mode;
static char* modeSrc;   /* move, copy: what goes */
static char* modeName;  /* its name, or the download's file name */
static unsigned modeDownload; /* pick: the download's id */

enum { R_NONE, R_DRIVE, R_FAVORITE, R_DOWNLOADS, R_PLACE, R_UP, R_ENTRY }; /* in a folder: R_PLACE, R_UP, then R_ENTRY */
typedef struct {
    int type;
    const MJ* item; /* R_DRIVE, R_ENTRY */
    char* path;     /* g_free */
} RowRef;

/* the dialog's row (copied: the listing may change under it) */
static struct {
    int type;
    char* path;
    char* name;
    gboolean dir, app, archive, used;
    gboolean icon; /* an app folder: sce_sys/icon0.png is there */
    gint64 size;
    /* an archive: what it holds (the service's "peek" on the drive) */
    unsigned peekID;
    gboolean peeking;
    MJ* peek;
    char* peekError;
} chosen;
static unsigned nextPeek = 1;

/* The last archive looked at, so opening its dialog again doesn't start
 * another look (the service runs only a few at once, and can't stop one). */
static struct {
    char* path;
    gint64 size;
    unsigned id;
    gboolean pending;
    gint64 asked; /* monotonic us: a look with no answer after a while is asked again */
    char* json;  /* the answer, or NULL */
    char* error; /* why not */
} lastPeek;

enum {
    CH_OPEN = 1,
    CH_INSTALL,
    CH_IMAGE,
    CH_EXTRACT,
    CH_EXTRACT_INSTALL,
    CH_EXTRACT_IMAGE,
    CH_MOVE,
    CH_COPY,
    CH_RENAME,
    CH_DELETE,
    CH_NEW_FOLDER,
    CH_FAVORITE,
    CH_DEFAULT,
    CH_UP,
};

/* ---------------------------------------------------------------- helpers */

static void say(const char* format, ...) G_GNUC_PRINTF(1, 2);
static void say(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    char* text = g_strdup_vprintf(format, args);
    va_end(args);
    if (display)
        wpe_display_ps5_show_message(display, text, 4);
    g_free(text);
}

static void formatBytes(char* out, size_t size, double bytes)
{
    if (bytes < 0)
        snprintf(out, size, "?");
    else if (bytes >= 1e12)
        snprintf(out, size, "%.2f TB", bytes / 1099511627776.0);
    else if (bytes >= 1073741824.0)
        snprintf(out, size, "%.2f GB", bytes / 1073741824.0);
    else if (bytes >= 104857600.0)
        snprintf(out, size, "%.0f MB", bytes / 1048576.0);
    else if (bytes >= 1048576.0)
        snprintf(out, size, "%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1024)
        snprintf(out, size, "%.0f KB", bytes / 1024.0);
    else
        snprintf(out, size, "%.0f B", bytes);
}

static void formatTime(char* out, size_t size, gint64 seconds)
{
    GDateTime* time = seconds > 0 ? g_date_time_new_from_unix_local(seconds) : NULL;
    char* text = time ? g_date_time_format(time, "%d %b %H:%M") : NULL;
    snprintf(out, size, "%s", text ? text : "");
    g_free(text);
    if (time)
        g_date_time_unref(time);
}

static char* withArrows(const char* prefix, const char* rest)
{
    char** parts = g_strsplit(rest, "/", -1);
    GString* out = g_string_new(prefix);
    for (char** p = parts; *p; p++) {
        if (**p)
            g_string_append_printf(out, " › %s", *p);
    }
    g_strfreev(parts);
    return g_string_free(out, FALSE);
}

char* ps5wk_files_place_name(const char* path)
{
    if (!path || !*path)
        return g_strdup("Drives");
    const char* user = ps5wk_profile_downloads_dir();
    size_t ul = strlen(user);
    if (!strncmp(path, user, ul) && (!path[ul] || path[ul] == '/'))
        return withArrows("Downloads", path + ul);
    if (!strncmp(path, "/data", 5) && (!path[5] || path[5] == '/'))
        return withArrows("Internal storage", path + 5);
    if ((!strncmp(path, "/mnt/usb", 8) || !strncmp(path, "/mnt/ext", 8)) && g_ascii_isdigit(path[8]) && (!path[9] || path[9] == '/')) {
        char drive[32];
        snprintf(drive, sizeof drive, "%s %d", path[5] == 'u' ? "USB drive" : "Extended storage", path[8] - '0' + 1);
        return withArrows(drive, path + 9);
    }
    return g_strdup(path);
}

/* A drive's top folder (O from it goes to the drives). */
static gboolean isDriveTop(const char* path)
{
    if (!strcmp(path, "/data"))
        return TRUE;
    return (!strncmp(path, "/mnt/usb", 8) || !strncmp(path, "/mnt/ext", 8)) && g_ascii_isdigit(path[8]) && !path[9];
}

static const char* manageV(const char* action, va_list args)
{
    GHashTable* params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(params, g_strdup("do"), g_strdup(action));
    for (const char* key; (key = va_arg(args, const char*));) {
        const char* value = va_arg(args, const char*);
        if (value)
            g_hash_table_insert(params, g_strdup(key), g_strdup(value));
    }
    const char* error = ps5wk_downloads_manage(params);
    g_hash_table_unref(params);
    return error;
}

/* downloads.c's action: NULL or why not (said). */
static const char* manage(const char* action, ...) G_GNUC_NULL_TERMINATED;
static const char* manage(const char* action, ...)
{
    va_list args;
    va_start(args, action);
    const char* error = manageV(action, args);
    va_end(args);
    if (error)
        say("%s", error);
    return error;
}

static unsigned favoriteCount(void)
{
    char** favorites = ps5wk_downloads_favorites();
    return favorites ? g_strv_length(favorites) : 0;
}

static gboolean isFavorite(const char* path)
{
    char** favorites = ps5wk_downloads_favorites();
    for (unsigned i = 0; path && favorites && favorites[i]; i++) {
        if (!strcmp(favorites[i], path))
            return TRUE;
    }
    return FALSE;
}

static const MJ* entries(void)
{
    return place && listed ? mj_get(listing, "entries") : NULL;
}

static unsigned driveCount(void)
{
    return !place && listed ? mj_length(listing) : 0;
}

static void rowFree(RowRef* r)
{
    g_free(r->path);
    r->path = NULL;
}

static gboolean rowAt(unsigned index, RowRef* out)
{
    memset(out, 0, sizeof *out);
    if (!place) {
        unsigned drives = driveCount(), favorites = favoriteCount();
        if (index < drives) {
            out->type = R_DRIVE;
            out->item = mj_at(listing, index);
            const char* path = mj_string(out->item, "path");
            out->path = g_strdup(path ? path : "");
        } else if (index < drives + favorites) {
            out->type = R_FAVORITE;
            out->path = g_strdup(ps5wk_downloads_favorites()[index - drives]);
        } else if (index == drives + favorites) {
            out->type = R_DOWNLOADS;
            out->path = g_strdup(ps5wk_profile_downloads_dir());
        } else
            return FALSE;
        return TRUE;
    }
    if (index == 0) {
        out->type = R_PLACE;
        out->path = g_strdup(place);
        return TRUE;
    }
    if (index == 1) {
        out->type = R_UP;
        out->path = g_strdup(place);
        return TRUE;
    }
    const MJ* item = mj_at(entries(), index - 2);
    const char* name = mj_string(item, "name");
    if (!item || !name)
        return FALSE;
    out->type = R_ENTRY;
    out->item = item;
    out->path = g_build_filename(place, name, NULL);
    return TRUE;
}

/* ---------------------------------------------------------------- listing */

static int indexOfName(const char* name)
{
    const MJ* list = entries();
    for (unsigned i = 0; name && i < mj_length(list); i++) {
        if (!g_strcmp0(mj_string(mj_at(list, i), "name"), name))
            return (int)i + 2;
    }
    return -1;
}

static void load(void);

/* The drives couldn't be listed (the service starting, busy): tried again a
 * few times, and each time the list is shown. */
static guint retryTimer;
static unsigned retries;

static gboolean retryRoots(gpointer data)
{
    (void)data;
    retryTimer = 0;
    if (!place && !listed && !loading)
        load();
    return G_SOURCE_REMOVE;
}

const char* ps5wk_files_error(void)
{
    static char text[300];
    if (place || listed || !loadError)
        return NULL;
    snprintf(text, sizeof text, "%s%s", loadError, retryTimer || loading ? " · trying again" : " · go to another list and back to try again");
    return text;
}

static void onListed(const char* json, const char* error, gpointer user)
{
    if (GPOINTER_TO_UINT(user) != generation)
        return; /* another place since */
    loading = FALSE;
    MJ* value = json ? mj_parse(json) : NULL;
    g_clear_pointer(&loadError, g_free);
    if (!value && !error)
        error = "The answer couldn't be read";
    if (value && (place ? mj_kind(value) != MJ_OBJECT : mj_kind(value) != MJ_ARRAY)) {
        mj_free(value);
        value = NULL;
        error = "The answer couldn't be read";
    }
    if (value) {
        mj_free(listing);
        listing = value;
        listed = TRUE;
    } else {
        loadError = g_strdup(error);
        spike_log("[files] %s: %s", place ? place : "drives", error);
        if (!place && !listed && !retryTimer && retries < 5) {
            retries++;
            retryTimer = g_timeout_add_seconds(3, retryRoots, NULL);
        }
    }
    if (value && !place)
        retries = 0;
    int focus = -1;
    if (focusName) {
        focus = indexOfName(focusName);
        g_clear_pointer(&focusName, g_free);
    }
    ps5wk_browser_files_changed(focus);
}

static void load(void)
{
    generation++;
    loading = TRUE;
    if (place)
        ps5wk_dlservice_request_async(8000, onListed, GUINT_TO_POINTER(generation), "ls", "path", place, NULL);
    else
        ps5wk_dlservice_request_async(8000, onListed, GUINT_TO_POINTER(generation), "roots", NULL);
}

/* Shows folder (NULL: the drives), name focused once listed. */
static void navigate(const char* folder, const char* name)
{
    char* copy = g_strdup(folder);
    g_free(place);
    place = copy;
    mj_free(listing);
    listing = NULL;
    listed = FALSE;
    g_clear_pointer(&loadError, g_free);
    g_free(focusName);
    focusName = g_strdup(name);
    load();
    ps5wk_browser_files_changed(0);
}

void ps5wk_files_shown(void)
{
    retries = 0;
    if (!loading)
        load();
}

void ps5wk_files_refresh(void)
{
    if (place)
        load();
}

void ps5wk_files_open(const char* folder, const char* name)
{
    navigate(folder, name);
}

static void clearMode(void)
{
    mode = MODE_NONE;
    g_clear_pointer(&modeSrc, g_free);
    g_clear_pointer(&modeName, g_free);
    modeDownload = 0;
}

void ps5wk_files_pick_folder(unsigned download, const char* fileName)
{
    clearMode();
    mode = MODE_PICK;
    modeDownload = download;
    modeName = g_strdup(fileName ? fileName : "the download");
    if (!place)
        navigate(NULL, NULL);
    else
        ps5wk_browser_files_changed(0);
}

void ps5wk_files_pick_default(const char* folder)
{
    clearMode();
    mode = MODE_DEFAULT;
    modeName = g_strdup("new downloads");
    navigate(folder, NULL);
}

void ps5wk_files_pick_cancel(unsigned download)
{
    if (mode == MODE_PICK && modeDownload == download) {
        clearMode();
        ps5wk_browser_files_changed(-1);
    }
}

/* Whether place takes what the mode carries. */
static gboolean modeTarget(void)
{
    if (!place || mode == MODE_NONE)
        return FALSE;
    if (mode == MODE_DEFAULT)
        return g_strcmp0(place, ps5wk_downloads_default_folder()) != 0;
    if (mode == MODE_PICK)
        return TRUE;
    char* parent = g_path_get_dirname(modeSrc);
    size_t sl = strlen(modeSrc);
    gboolean ok = strcmp(place, parent) && strcmp(place, modeSrc) && !(!strncmp(place, modeSrc, sl) && place[sl] == '/');
    g_free(parent);
    return ok;
}

/* ---------------------------------------------------------------- rows */

unsigned ps5wk_files_count(void)
{
    if (!place)
        return listed ? driveCount() + favoriteCount() + 1 : 0;
    return 2 + mj_length(entries());
}

static void entryKind(const MJ* e, const char* name, char* out, size_t size)
{
    if (mj_bool(e, "dir")) {
        snprintf(out, size, "%s", mj_bool(e, "app") ? "APP" : "DIR");
        return;
    }
    const char* dot = strrchr(name, '.');
    if (!dot || dot == name || !dot[1] || strlen(dot + 1) > 5) {
        snprintf(out, size, "FILE");
        return;
    }
    if (!g_ascii_strcasecmp(dot + 1, "exfat"))
        snprintf(out, size, "IMG");
    else {
        char* upper = g_ascii_strup(dot + 1, -1);
        snprintf(out, size, "%.4s", upper);
        g_free(upper);
    }
}

void ps5wk_files_row(unsigned index, BUIRow* r)
{
    RowRef ref;
    if (!rowAt(index, &ref))
        return;
    r->progress = -1;
    static char title[300];
    char size[32], total[32], when[48];
    switch (ref.type) {
    case R_DRIVE: {
        const char* kind = mj_string(ref.item, "kind");
        snprintf(r->kind, sizeof r->kind, "%s", !g_strcmp0(kind, "usb") ? "USB" : !g_strcmp0(kind, "ext") ? "EXT" : "SSD");
        const char* label = mj_string(ref.item, "label");
        snprintf(title, sizeof title, "%s", label ? label : ref.path);
        r->title = title;
        formatBytes(size, sizeof size, mj_number(ref.item, "free", -1));
        formatBytes(total, sizeof total, mj_number(ref.item, "size", -1));
        const char* fs = mj_string(ref.item, "fs");
        snprintf(r->detail, sizeof r->detail, "%s free of %s%s%s%s", size, total, fs ? " · " : "", fs ? fs : "",
            mj_bool(ref.item, "writable") ? "" : " · read only");
        break;
    }
    case R_FAVORITE: {
        snprintf(r->kind, sizeof r->kind, "★");
        char* name = ps5wk_files_place_name(ref.path);
        snprintf(title, sizeof title, "%s", name);
        g_free(name);
        r->title = title;
        snprintf(r->detail, sizeof r->detail, "%s", ref.path);
        snprintf(r->state, sizeof r->state, "Favourite");
        if (!g_strcmp0(ref.path, ps5wk_downloads_default_folder())) {
            snprintf(r->state, sizeof r->state, "Download folder");
            r->tone = BUI_TONE_DONE;
        }
        break;
    }
    case R_DOWNLOADS:
        snprintf(r->kind, sizeof r->kind, "DL");
        r->title = "Downloads";
        snprintf(r->detail, sizeof r->detail, "This PS5 user's downloads folder");
        if (!g_strcmp0(ref.path, ps5wk_downloads_default_folder())) {
            snprintf(r->state, sizeof r->state, "Download folder");
            r->tone = BUI_TONE_DONE;
        }
        break;
    case R_PLACE: {
        char* name = ps5wk_files_place_name(place);
        if (mode != MODE_NONE) {
            const char* verb = mode == MODE_MOVE ? "move" : mode == MODE_COPY ? "copy" : "save";
            if (modeTarget()) {
                r->action = TRUE;
                snprintf(r->kind, sizeof r->kind, "↓"); /* into here */
                if (mode == MODE_DEFAULT)
                    snprintf(title, sizeof title, "Use this folder for new downloads");
                else
                    snprintf(title, sizeof title, "%c%s %s here", g_ascii_toupper(verb[0]), verb + 1, modeName);
                r->tone = BUI_TONE_ACTIVE;
            } else if (mode == MODE_DEFAULT) {
                /* already the download folder */
                snprintf(r->kind, sizeof r->kind, "DIR");
                snprintf(title, sizeof title, "New downloads go here");
                snprintf(r->state, sizeof r->state, "Download folder");
                r->tone = BUI_TONE_DONE;
            } else {
                /* the folder it's in, or one inside it */
                snprintf(r->kind, sizeof r->kind, "DIR");
                snprintf(title, sizeof title, "Open another folder to %s %s into", verb, modeName);
                r->tone = BUI_TONE_WARN;
            }
            /* where: once */
            snprintf(r->detail, sizeof r->detail, "%s", loading && !listed ? "Reading the folder…" : name);
        } else {
            snprintf(r->kind, sizeof r->kind, "DIR");
            snprintf(title, sizeof title, "%s", name);
            if (loadError)
                snprintf(r->detail, sizeof r->detail, "%s", loadError);
            else if (!listed)
                snprintf(r->detail, sizeof r->detail, "Reading the folder…");
            else {
                unsigned n = mj_length(entries());
                double free = mj_number(listing, "free", -1);
                formatBytes(size, sizeof size, free);
                snprintf(r->detail, sizeof r->detail, "%s%s%u item%s", free >= 0 ? size : "", free >= 0 ? " free · " : "", n, n == 1 ? "" : "s");
            }
            if (loadError)
                r->tone = BUI_TONE_WARN;
            const char* folder = ps5wk_downloads_default_folder();
            if (!g_strcmp0(folder, place) && !loadError) {
                snprintf(r->state, sizeof r->state, "Download folder");
                r->tone = BUI_TONE_DONE;
            } else if (isFavorite(place))
                snprintf(r->state, sizeof r->state, "★ Favourite");
        }
        g_free(name);
        r->title = title;
        break;
    }
    case R_UP: {
        snprintf(r->kind, sizeof r->kind, "↑");
        gboolean top = isDriveTop(place) || !strchr(place + 1, '/');
        char* parent = top ? NULL : g_path_get_dirname(place);
        char* name = top ? NULL : ps5wk_files_place_name(parent);
        snprintf(title, sizeof title, "Up to %s", top ? "all drives" : name);
        r->title = title;
        if (parent)
            snprintf(r->detail, sizeof r->detail, "%s", parent);
        g_free(parent);
        g_free(name);
        break;
    }
    case R_ENTRY: {
        const char* name = mj_string(ref.item, "name");
        snprintf(title, sizeof title, "%s", name);
        r->title = title;
        entryKind(ref.item, name, r->kind, sizeof r->kind);
        formatTime(when, sizeof when, (gint64)mj_number(ref.item, "mtime", 0));
        if (mj_bool(ref.item, "dir"))
            snprintf(r->detail, sizeof r->detail, "%s · %s", mj_bool(ref.item, "app") ? "App folder" : "Folder", when);
        else {
            formatBytes(size, sizeof size, mj_number(ref.item, "size", -1));
            snprintf(r->detail, sizeof r->detail, "%s · %s", size, when);
        }
        if (ps5wk_downloads_in_use(ref.path)) {
            snprintf(r->state, sizeof r->state, "In use");
            r->tone = BUI_TONE_ACTIVE;
        } else if (mj_bool(ref.item, "dir") && !g_strcmp0(ref.path, ps5wk_downloads_default_folder())) {
            snprintf(r->state, sizeof r->state, "Download folder");
            r->tone = BUI_TONE_DONE;
        } else if (mode == MODE_NONE && mj_bool(ref.item, "app"))
            snprintf(r->state, sizeof r->state, "Can be installed");
        break;
    }
    }
    rowFree(&ref);
}

void ps5wk_files_hints(unsigned index, const char** cross, const char** square, const char** r2, const char** circle)
{
    *cross = *square = *r2 = *circle = NULL;
    RowRef ref;
    if (!rowAt(index, &ref))
        return;
    if (place)
        *circle = "Up";
    switch (ref.type) {
    case R_DRIVE:
    case R_DOWNLOADS:
        *cross = "Open";
        break;
    case R_FAVORITE:
        *cross = "Open";
        *square = "Remove the favourite";
        break;
    case R_PLACE:
        if (mode != MODE_NONE) {
            if (modeTarget())
                *cross = mode == MODE_MOVE ? "Move here" : mode == MODE_COPY ? "Copy here" : mode == MODE_DEFAULT ? "Use this folder" : "Save here";
            *square = mode == MODE_PICK ? "Choose later" : "Cancel";
        } else
            *cross = "Folder options";
        break;
    case R_UP:
        *cross = "Open";
        break;
    case R_ENTRY: {
        gboolean dir = mj_bool(ref.item, "dir"), app = mj_bool(ref.item, "app");
        if (mode != MODE_NONE) {
            if (dir)
                *cross = "Open";
            break;
        }
        if (dir && !app) {
            *cross = "Open";
            *r2 = "Options";
        } else if (app) {
            *cross = "Options";
            *r2 = "Open";
        } else
            *cross = "Options";
        if (!ps5wk_downloads_in_use(ref.path))
            *square = "Delete";
        break;
    }
    }
    rowFree(&ref);
}

/* ---------------------------------------------------------------- doing */

static int doMode(void)
{
    if (!modeTarget())
        return PS5WK_FILES_NOTHING;
    if (mode == MODE_PICK) {
        char id[16];
        snprintf(id, sizeof id, "%u", modeDownload);
        if (manage("save", "id", id, "dir", place, NULL))
            return PS5WK_FILES_NOTHING;
        clearMode();
        return PS5WK_FILES_MENU_CLOSE;
    }
    if (mode == MODE_DEFAULT) {
        /* chosen from Settings: back to it */
        if (manage("settings", "folder", place, NULL))
            return PS5WK_FILES_NOTHING;
        char* where = ps5wk_files_place_name(place);
        say("New downloads go to %s", where);
        g_free(where);
        clearMode();
        return PS5WK_FILES_MENU_CLOSE;
    }
    const char* kind = mode == MODE_MOVE ? "move" : "copy";
    char* src = g_strdup(modeSrc);
    char* name = g_strdup(modeName);
    if (!manage("task", "kind", kind, "path", src, "dest", place, NULL)) {
        char* where = ps5wk_files_place_name(place);
        say("%s %s to %s\nThe progress is in Downloads", mode == MODE_MOVE ? "Moving" : "Copying", name, where);
        g_free(where);
        clearMode();
    }
    g_free(src);
    g_free(name);
    return PS5WK_FILES_DONE;
}

int ps5wk_files_activate(unsigned index, gboolean alternative)
{
    RowRef ref;
    if (!rowAt(index, &ref))
        return PS5WK_FILES_NOTHING;
    int result = PS5WK_FILES_NOTHING;
    switch (ref.type) {
    case R_DRIVE:
    case R_FAVORITE:
    case R_DOWNLOADS:
        navigate(ref.path, NULL);
        result = PS5WK_FILES_DONE;
        break;
    case R_PLACE:
        result = mode != MODE_NONE ? doMode() : PS5WK_FILES_CHOICES;
        break;
    case R_UP:
        ps5wk_files_up();
        result = PS5WK_FILES_DONE;
        break;
    case R_ENTRY: {
        gboolean dir = mj_bool(ref.item, "dir"), app = mj_bool(ref.item, "app");
        if (mode != MODE_NONE) {
            if (dir) {
                navigate(ref.path, NULL);
                result = PS5WK_FILES_DONE;
            }
        } else if (dir && (app ? alternative : !alternative)) {
            navigate(ref.path, NULL);
            result = PS5WK_FILES_DONE;
        } else
            result = PS5WK_FILES_CHOICES;
        break;
    }
    }
    rowFree(&ref);
    return result;
}

char* ps5wk_files_row_key(unsigned index)
{
    RowRef ref;
    if (!rowAt(index, &ref))
        return NULL;
    char* key = g_strdup_printf("%d:%s", ref.type, ref.path ? ref.path : "");
    rowFree(&ref);
    return key;
}

gboolean ps5wk_files_square_needs_confirm(unsigned index)
{
    RowRef ref;
    gboolean confirm = rowAt(index, &ref) && ref.type == R_ENTRY && mode == MODE_NONE && !ps5wk_downloads_in_use(ref.path);
    rowFree(&ref);
    return confirm;
}

static void startDelete(const char* path)
{
    char* base = g_path_get_basename(path);
    if (!manage("task", "kind", "delete", "path", path, NULL))
        say("Deleting %s", base);
    g_free(base);
}

void ps5wk_files_square(unsigned index)
{
    RowRef ref;
    if (!rowAt(index, &ref))
        return;
    if (ref.type == R_FAVORITE) {
        manage("favorite", "path", ref.path, "on", "0", NULL);
        ps5wk_browser_files_changed(-1);
    } else if (ref.type == R_PLACE && mode != MODE_NONE) {
        if (mode == MODE_PICK)
            say("%s waits in Downloads: X on it chooses the folder", modeName);
        clearMode();
        ps5wk_browser_files_changed(-1);
    } else if (ref.type == R_ENTRY && mode == MODE_NONE && !ps5wk_downloads_in_use(ref.path))
        startDelete(ref.path);
    rowFree(&ref);
}

gboolean ps5wk_files_up(void)
{
    if (!place)
        return FALSE;
    if (isDriveTop(place) || !strchr(place + 1, '/')) {
        navigate(NULL, NULL);
        return TRUE;
    }
    char* parent = g_path_get_dirname(place);
    char* name = g_path_get_basename(place);
    navigate(parent, name);
    g_free(parent);
    g_free(name);
    return TRUE;
}

/* ---------------------------------------------------------------- the dialog */

static void clearChosen(void)
{
    g_clear_pointer(&chosen.path, g_free);
    g_clear_pointer(&chosen.name, g_free);
    mj_free(chosen.peek);
    g_free(chosen.peekError);
    memset(&chosen, 0, sizeof chosen);
}

/* What the look found, into the dialog. */
static void takePeek(const char* json, const char* error)
{
    chosen.peeking = FALSE;
    MJ* value = json ? mj_parse(json) : NULL;
    if (value && mj_kind(value) == MJ_OBJECT)
        chosen.peek = value;
    else {
        mj_free(value);
        chosen.peekError = g_strdup(error && *error ? error : "It couldn't be read");
    }
}

void ps5wk_files_peek_ended(const char* id, GHashTable* params)
{
    unsigned n = (unsigned)atoi(id + 1);
    const char* ok = g_hash_table_lookup(params, "ok");
    const char* json = ok && !strcmp(ok, "1") ? g_hash_table_lookup(params, "json") : NULL;
    const char* error = g_hash_table_lookup(params, "error");
    if (lastPeek.pending && n == lastPeek.id) {
        lastPeek.pending = FALSE;
        lastPeek.json = g_strdup(json);
        lastPeek.error = g_strdup(error);
    }
    if (!chosen.peeking || n != chosen.peekID)
        return; /* another dialog since */
    takePeek(json, error);
    ps5wk_browser_files_changed(-1);
}

/* Look inside chosen's archive, or take what the last look at it found. */
static void peekChosen(void)
{
    chosen.peeking = TRUE;
    if (lastPeek.path && !strcmp(lastPeek.path, chosen.path) && lastPeek.size == chosen.size) {
        if (lastPeek.pending && g_get_monotonic_time() - lastPeek.asked < 120 * G_USEC_PER_SEC) {
            chosen.peekID = lastPeek.id; /* the look already asked for */
            return;
        }
        if (!lastPeek.pending && lastPeek.json) {
            takePeek(lastPeek.json, lastPeek.error);
            return;
        }
        /* it failed: look again (the drive may have been busy) */
    }
    g_free(lastPeek.path);
    g_free(lastPeek.json);
    g_free(lastPeek.error);
    lastPeek.path = g_strdup(chosen.path);
    lastPeek.size = chosen.size;
    lastPeek.id = chosen.peekID = nextPeek++;
    lastPeek.pending = TRUE;
    lastPeek.asked = g_get_monotonic_time();
    lastPeek.json = lastPeek.error = NULL;
    char id[16];
    snprintf(id, sizeof id, "f%u", chosen.peekID);
    ps5wk_dlservice_send("peek", "id", id, "path", chosen.path, NULL);
}

gboolean ps5wk_files_choices_open(unsigned index)
{
    clearChosen();
    RowRef ref;
    if (!rowAt(index, &ref))
        return FALSE;
    if (ref.type != R_PLACE && ref.type != R_ENTRY) {
        rowFree(&ref);
        return FALSE;
    }
    chosen.type = ref.type;
    chosen.path = ref.path; /* kept */
    chosen.name = g_path_get_basename(ref.path);
    if (ref.type == R_ENTRY) {
        chosen.dir = mj_bool(ref.item, "dir");
        chosen.app = mj_bool(ref.item, "app");
        chosen.icon = mj_bool(ref.item, "icon");
        chosen.archive = mj_bool(ref.item, "archive");
        chosen.size = (gint64)mj_number(ref.item, "size", -1);
        if (chosen.archive)
            peekChosen(); /* what it holds decides what's offered */
    }
    return TRUE;
}

static void addChoice(BUIChoices* out, int id, int tone, gboolean confirm, const char* label, const char* detail)
{
    if (out->count >= BUI_CHOICES_MAX)
        return;
    BUIChoice* c = &out->choices[out->count++];
    c->id = id;
    c->tone = tone;
    c->confirm = confirm;
    snprintf(c->label, sizeof c->label, "%s", label);
    snprintf(c->detail, sizeof c->detail, "%s", detail ? detail : "");
}

/* "name.part1.rar", "name.tar.gz": the folder an archive unpacks into. */
static char* unpackFolder(const char* path)
{
    char* stem = g_strdup(path);
    static const char* const tails[] = { ".tar.gz", ".tar.xz", ".tar.bz2", ".tgz" };
    gboolean cut = FALSE;
    for (unsigned i = 0; i < G_N_ELEMENTS(tails) && !cut; i++) {
        if (g_str_has_suffix(stem, tails[i])) {
            stem[strlen(stem) - strlen(tails[i])] = 0;
            cut = TRUE;
        }
    }
    for (int pass = 0; pass < 3 && !cut; pass++) {
        char* dot = strrchr(stem, '.');
        char* slash = strrchr(stem, '/');
        if (!dot || (slash && dot < slash))
            break;
        gboolean number = strlen(dot + 1) >= 2 && strspn(dot + 1, "0123456789") == strlen(dot + 1);
        gboolean part = !g_ascii_strncasecmp(dot + 1, "part", 4) && strspn(dot + 5, "0123456789") == strlen(dot + 5);
        *dot = 0;
        if (!number && !part)
            break;
    }
    return stem;
}

gboolean ps5wk_files_choices(BUIChoices* out)
{
    memset(out, 0, sizeof *out);
    out->armed = -1;
    if (!chosen.path || !place)
        return FALSE;
    char* where = ps5wk_files_place_name(place);
    if (chosen.type == R_PLACE) {
        if (strcmp(chosen.path, place)) {
            g_free(where);
            return FALSE;
        }
        snprintf(out->title, sizeof out->title, "%s", where);
        snprintf(out->lines[out->lineCount++], sizeof out->lines[0], "%s", place);
        addChoice(out, CH_NEW_FOLDER, BUI_TONE_PLAIN, FALSE, "New folder…", NULL);
        gboolean favorite = isFavorite(place);
        addChoice(out, CH_FAVORITE, BUI_TONE_PLAIN, FALSE, favorite ? "Remove from favourites" : "★ Add to favourites",
            favorite ? NULL : "Favourites are at the top of Files, and offered when a download asks where to go");
        const char* folder = ps5wk_downloads_default_folder();
        if (g_strcmp0(folder, place)) {
            char* now = ps5wk_files_place_name(folder);
            char detail[200];
            snprintf(detail, sizeof detail, "New downloads go to %s now", now);
            addChoice(out, CH_DEFAULT, BUI_TONE_PLAIN, FALSE, "Save new downloads here", detail);
            g_free(now);
        }
        addChoice(out, CH_UP, BUI_TONE_PLAIN, FALSE, isDriveTop(place) ? "Back to the drives" : "Up a folder", NULL);
        g_free(where);
        return TRUE;
    }
    /* an entry: still in the folder shown */
    char* folder = g_path_get_dirname(chosen.path);
    gboolean here = !strcmp(folder, place) && indexOfName(chosen.name) > 0;
    g_free(folder);
    if (!here) {
        g_free(where);
        return FALSE;
    }
    chosen.used = ps5wk_downloads_in_use(chosen.path);
    snprintf(out->title, sizeof out->title, "%s", chosen.name);
    char size[32] = "";
    if (!chosen.dir)
        formatBytes(size, sizeof size, (double)chosen.size);
    snprintf(out->lines[out->lineCount++], sizeof out->lines[0], "%s%s%s", chosen.app ? "App folder" : chosen.dir ? "Folder" : size, " · in ",
        where);
    if (chosen.used) {
        out->lineTones[out->lineCount] = BUI_TONE_WARN;
        snprintf(out->lines[out->lineCount++], sizeof out->lines[0], "A download or a job is using it: it can't be moved, renamed or deleted now");
    }
    if (chosen.app) {
        addChoice(out, CH_INSTALL, BUI_TONE_ACTIVE, FALSE, "Install the app", "You're asked before an installed version is replaced");
        gboolean known = FALSE;
        const char* mounter = ps5wk_downloads_image_mounter(&known);
        if (chosen.icon && known && !mounter) {
            if (out->lineCount < 3) {
                out->lineTones[out->lineCount] = BUI_TONE_WARN;
                snprintf(out->lines[out->lineCount++], sizeof out->lines[0], "%s", PS5WK_NO_IMAGE_MOUNTER);
            }
        } else if (chosen.icon) {
            char detail[160];
            snprintf(detail, sizeof detail, "<ID>.exfat in the drive's homebrew folder, for %s; checked once made", mounter ? mounter : "ShadowMountPlus");
            addChoice(out, CH_IMAGE, BUI_TONE_PLAIN, FALSE, "Make an exFAT image", detail);
        } else if (out->lineCount < 3) {
            out->lineTones[out->lineCount] = BUI_TONE_WARN;
            snprintf(out->lines[out->lineCount++], sizeof out->lines[0], "No icon (sce_sys/icon0.png), so no exFAT image: ShadowMountPlus won't install an app without one");
        }
        addChoice(out, CH_OPEN, BUI_TONE_PLAIN, FALSE, "Open the folder", NULL);
    } else if (chosen.dir) {
        addChoice(out, CH_OPEN, BUI_TONE_PLAIN, FALSE, "Open", NULL);
    } else if (chosen.archive) {
        /* what it holds: an image only of exactly one app */
        const MJ* p = chosen.peek;
        const char* id = mj_string(p, "titleId");
        const char* refused = mj_string(p, "refused");
        int titles = (int)mj_number(p, "titles", 0);
        if (chosen.peeking)
            snprintf(out->lines[out->lineCount++], sizeof out->lines[0], "Looking inside…");
        else if (refused) {
            out->lineTones[out->lineCount] = BUI_TONE_WARN;
            snprintf(out->lines[out->lineCount++], sizeof out->lines[0], "It can't be unpacked: %s", refused);
        } else if (titles > 1)
            snprintf(out->lines[out->lineCount++], sizeof out->lines[0], "It holds %d apps: unpack it, then install each from Files", titles);
        else if (id) {
            const char* name = mj_string(p, "name");
            const char* version = mj_string(p, "version");
            const char* installed = mj_string(p, "installed");
            out->lineTones[out->lineCount] = BUI_TONE_ACTIVE;
            snprintf(out->lines[out->lineCount++], sizeof out->lines[0], "It holds an app: %s · %s%s%s", name ? name : id, id,
                version ? " · version " : "", version ? version : "");
            if (installed && *installed && out->lineCount < 3) {
                out->lineTones[out->lineCount] = BUI_TONE_WARN;
                snprintf(out->lines[out->lineCount++], sizeof out->lines[0], "%s",
                    strcmp(installed, "?") ? (version && !strcmp(installed, version) ? "This version is installed already" : "Another version is installed")
                                           : "A version of it is installed");
            }
        } else if (p)
            snprintf(out->lines[out->lineCount++], sizeof out->lines[0], "It holds files, not an app");
        else if (chosen.peekError)
            snprintf(out->lines[out->lineCount++], sizeof out->lines[0], "What it holds isn't known: %s", chosen.peekError);
        if (!refused) {
            char* into = unpackFolder(chosen.path);
            char* intoName = ps5wk_files_place_name(into);
            char detail[200];
            snprintf(detail, sizeof detail, "Into %s", intoName);
            gboolean app = id && titles == 1;
            gboolean several = titles > 1;
            if (p && !id && titles == 0)
                addChoice(out, CH_EXTRACT, BUI_TONE_ACTIVE, FALSE, "Unpack", detail);
            else {
                if (!several)
                    addChoice(out, CH_EXTRACT_INSTALL, app ? BUI_TONE_ACTIVE : BUI_TONE_PLAIN, FALSE, "Unpack and install the app",
                        "You're asked before an installed version is replaced");
                addChoice(out, CH_EXTRACT, several ? BUI_TONE_ACTIVE : BUI_TONE_PLAIN, FALSE, "Unpack", detail);
            }
            if (app && !mj_bool(p, "icon")) {
                if (out->lineCount < 3) {
                    out->lineTones[out->lineCount] = BUI_TONE_WARN;
                    snprintf(out->lines[out->lineCount++], sizeof out->lines[0], "No icon (sce_sys/icon0.png), so no exFAT image: ShadowMountPlus won't install an app without one");
                }
            } else if (app) {
                gboolean known = FALSE;
                const char* mounter = ps5wk_downloads_image_mounter(&known);
                if (known && !mounter) {
                    if (out->lineCount < 3) {
                        out->lineTones[out->lineCount] = BUI_TONE_WARN;
                        snprintf(out->lines[out->lineCount++], sizeof out->lines[0], "%s", PS5WK_NO_IMAGE_MOUNTER);
                    }
                } else {
                    char image[160];
                    snprintf(image, sizeof image, "%s.exfat in the drive's homebrew folder, for %s; checked once made", id,
                        mounter ? mounter : "ShadowMountPlus");
                    addChoice(out, CH_EXTRACT_IMAGE, BUI_TONE_PLAIN, FALSE, "Unpack and make an exFAT image", image);
                }
            }
            g_free(into);
            g_free(intoName);
        }
    }
    if (!chosen.used) {
        addChoice(out, CH_MOVE, BUI_TONE_PLAIN, FALSE, "Move…", "Then open the folder it goes into");
        addChoice(out, CH_COPY, BUI_TONE_PLAIN, FALSE, "Copy…", "Then open the folder it goes into");
        addChoice(out, CH_RENAME, BUI_TONE_PLAIN, FALSE, "Rename…", NULL);
        addChoice(out, CH_DELETE, BUI_TONE_WARN, TRUE, chosen.dir ? "Delete the folder and everything in it" : "Delete", NULL);
    }
    g_free(where);
    return TRUE;
}

static void onFolderMade(const char* json, const char* error, gpointer user)
{
    char* name = user;
    if (!json)
        say("Couldn't make the folder: %s", error ? error : "?");
    else if (place) {
        g_free(focusName);
        focusName = g_strdup(name);
        load();
    }
    g_free(name);
}

static void onNewFolderName(const char* text, gpointer data)
{
    char* folder = data;
    if (text && *text && place && !strcmp(folder, place))
        ps5wk_dlservice_request_async(15000, onFolderMade, g_strdup(text), "mkdir", "path", folder, "name", text, NULL);
    g_free(folder);
}

static void onRenamed(const char* json, const char* error, gpointer user)
{
    char* name = user;
    if (!json)
        say("Couldn't rename it: %s", error ? error : "?");
    else if (place) {
        g_free(focusName);
        focusName = g_strdup(name);
        load();
    }
    g_free(name);
}

static void onRenameText(const char* text, gpointer data)
{
    char* path = data;
    char* old = g_path_get_basename(path);
    if (text && *text && strcmp(text, old)) {
        if (ps5wk_downloads_in_use(path))
            say("A download or a job is using it");
        else {
            spike_log("[files] rename %s to %s", path, text);
            ps5wk_dlservice_request_async(15000, onRenamed, g_strdup(text), "rename", "path", path, "name", text, NULL);
        }
    }
    g_free(old);
    g_free(path);
}

int ps5wk_files_choose(int id)
{
    if (!chosen.path)
        return BUI_CHOSEN_CLOSE;
    char* path = g_strdup(chosen.path);
    char* name = g_strdup(chosen.name);
    int result = BUI_CHOSEN_CLOSE;
    const char* kind = NULL;
    switch (id) {
    case CH_OPEN:
        navigate(path, NULL);
        break;
    case CH_UP:
        ps5wk_files_up();
        break;
    case CH_INSTALL:
        kind = "install";
        break;
    case CH_IMAGE:
        kind = "image";
        break;
    case CH_EXTRACT:
        kind = "extract";
        break;
    case CH_EXTRACT_INSTALL:
        kind = "extract-install";
        break;
    case CH_EXTRACT_IMAGE:
        kind = "extract-image";
        break;
    case CH_MOVE:
    case CH_COPY:
        clearMode();
        mode = id == CH_MOVE ? MODE_MOVE : MODE_COPY;
        modeSrc = g_strdup(path);
        modeName = g_strdup(name);
        say("Open the folder %s goes into, then X on the top row", name);
        ps5wk_browser_files_changed(0);
        break;
    case CH_RENAME: {
        char* data = g_strdup(path);
        if (!wpe_display_ps5_request_text(display, "New name", name, FALSE, onRenameText, data)) {
            g_free(data);
            say("The keyboard couldn't open");
        }
        break;
    }
    case CH_DELETE:
        startDelete(path);
        break;
    case CH_NEW_FOLDER:
        if (place) {
            char* data = g_strdup(place);
            if (!wpe_display_ps5_request_text(display, "Folder name", "", FALSE, onNewFolderName, data)) {
                g_free(data);
                say("The keyboard couldn't open");
            }
        }
        break;
    case CH_FAVORITE: {
        gboolean on = !isFavorite(path);
        if (!manage("favorite", "path", path, "on", on ? "1" : "0", NULL))
            say(on ? "Added to favourites" : "Removed from favourites");
        ps5wk_browser_files_changed(-1);
        break;
    }
    case CH_DEFAULT:
        if (!manage("settings", "folder", path, NULL)) {
            char* where = ps5wk_files_place_name(path);
            say("New downloads go to %s", where);
            g_free(where);
        }
        ps5wk_browser_files_changed(-1);
        break;
    default:
        result = BUI_CHOSEN_STAY;
        break;
    }
    if (kind && !manage("task", "kind", kind, "path", path, NULL)) {
        static const struct { const char *kind, *what; } words[] = { { "install", "Installing" }, { "image", "Making an image of" },
            { "extract", "Unpacking" }, { "extract-install", "Unpacking and installing" }, { "extract-image", "Unpacking" } };
        const char* what = "Working on";
        for (unsigned i = 0; i < G_N_ELEMENTS(words); i++) {
            if (!strcmp(words[i].kind, kind))
                what = words[i].what;
        }
        say("%s %s\nThe progress is in Downloads", what, name);
    }
    g_free(path);
    g_free(name);
    return result;
}

void ps5wk_files_init(WPEDisplayPS5* d)
{
    display = d;
}
