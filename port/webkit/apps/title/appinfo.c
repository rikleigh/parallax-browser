/* appinfo.c: what an image, package or app folder holds, and its icon;
 * see appinfo.h. */
#include "appinfo.h"

#include "dlservice.h"

#include <png.h>
#include <stdlib.h>
#include <string.h>

#include "common/spike.h"

#define KEEP 16
#define ICON_MAX_SIDE 2048 /* bigger PNGs aren't decoded (icon0.png is 512) */
#define RETRY_US (5 * G_USEC_PER_SEC) /* an error is asked again after this */

typedef struct {
    char* path;
    gint64 size;
    gint64 used;     /* monotonic us: the oldest goes first */
    gint64 failedAt; /* monotonic us: when its error came */
    guint64 serial;  /* this entry's requests: a reply for an older one is dropped */
    PS5WKAppInfo a;
    GPtrArray* waiting; /* the done callbacks to run */
} Entry;

/* What a request remembers: whose it is. */
typedef struct {
    char* path;
    guint64 serial;
} Asked;

static Entry entries[KEEP];
static guint64 serials;

static Entry* find(const char* path)
{
    for (int i = 0; i < KEEP; i++) {
        if (entries[i].path && !strcmp(entries[i].path, path))
            return &entries[i];
    }
    return NULL;
}

static gpointer asked(const Entry* e)
{
    Asked* a = g_new(Asked, 1);
    a->path = g_strdup(e->path);
    a->serial = e->serial;
    return a;
}

/* The entry a reply is for (and frees what the request remembered), or
 * NULL: forgotten, evicted or asked again since. */
static Entry* answered(gpointer user)
{
    Asked* a = user;
    Entry* e = find(a->path);
    if (e && e->serial != a->serial)
        e = NULL;
    g_free(a->path);
    g_free(a);
    return e;
}

static void clearEntry(Entry* e)
{
    g_free(e->path);
    mj_free(e->a.info);
    g_free(e->a.error);
    g_free(e->a.icon);
    if (e->waiting)
        g_ptr_array_unref(e->waiting);
    memset(e, 0, sizeof *e);
}

static void notify(Entry* e)
{
    for (unsigned i = 0; e->waiting && i < e->waiting->len; i++) {
        void (*done)(void) = (void (*)(void))g_ptr_array_index(e->waiting, i);
        done();
    }
}

/* The PNG at file, scaled (a box filter) into a side x side square,
 * centred, premultiplied 0xAARRGGBB. NULL if it can't be read. */
static guint32* decodeIcon(const char* file, int side)
{
    png_image image;
    memset(&image, 0, sizeof image);
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, file))
        return NULL;
    if (!image.width || !image.height || image.width > ICON_MAX_SIDE || image.height > ICON_MAX_SIDE) {
        png_image_free(&image);
        return NULL;
    }
    image.format = PNG_FORMAT_BGRA; /* bytes B G R A: 0xAARRGGBB words, little-endian */
    guint32* full = g_try_malloc(PNG_IMAGE_SIZE(image));
    if (!full || !png_image_finish_read(&image, NULL, full, 0, NULL)) {
        g_free(full);
        png_image_free(&image);
        return NULL;
    }
    const int w = (int)image.width, h = (int)image.height;
    const int longest = w > h ? w : h;
    const int ow = w * side / longest, oh = h * side / longest;
    guint32* out = g_new0(guint32, (gsize)side * side);
    for (int y = 0; y < oh; y++) {
        const int y0 = y * h / oh, y1 = (y + 1) * h / oh > y0 ? (y + 1) * h / oh : y0 + 1;
        for (int x = 0; x < ow; x++) {
            const int x0 = x * w / ow, x1 = (x + 1) * w / ow > x0 ? (x + 1) * w / ow : x0 + 1;
            guint64 a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int sy = y0; sy < y1; sy++) {
                for (int sx = x0; sx < x1; sx++) {
                    const guint32 p = full[(gsize)sy * w + sx];
                    const guint32 pa = p >> 24;
                    a += pa;
                    r += ((p >> 16) & 0xff) * pa;
                    g += ((p >> 8) & 0xff) * pa;
                    b += (p & 0xff) * pa;
                    n++;
                }
            }
            /* premultiplied: each colour times its alpha, averaged */
            const guint32 oa = (guint32)(a / n), orr = (guint32)(r / n / 255), og = (guint32)(g / n / 255), ob = (guint32)(b / n / 255);
            out[(gsize)(y + (side - oh) / 2) * side + x + (side - ow) / 2] = oa << 24 | orr << 16 | og << 8 | ob;
        }
    }
    g_free(full);
    return out;
}

static void onIcon(const char* json, const char* error, gpointer user)
{
    Entry* e = answered(user);
    if (!e)
        return;
    e->a.iconPending = FALSE;
    MJ* value = json ? mj_parse(json) : NULL;
    const char* file = mj_string(value, "path");
    if (file) {
        g_free(e->a.icon);
        e->a.icon = decodeIcon(file, PS5WK_APPINFO_ICON);
        if (!e->a.icon)
            spike_log("[appinfo] %s: the icon couldn't be decoded", file);
    } else
        spike_log("[appinfo] %s: no icon: %s", e->path, error ? error : "?");
    mj_free(value);
    notify(e);
}

static void onInfo(const char* json, const char* error, gpointer user)
{
    Entry* e = answered(user);
    if (!e)
        return;
    e->a.pending = FALSE;
    MJ* value = json ? mj_parse(json) : NULL;
    if (value && mj_kind(value) == MJ_OBJECT)
        e->a.info = value;
    else {
        mj_free(value);
        e->a.error = g_strdup(error && *error ? error : "It couldn't be read");
        e->failedAt = g_get_monotonic_time();
    }
    if (mj_bool(e->a.info, "icon")) {
        e->a.iconPending = TRUE;
        ps5wk_dlservice_request_async(30000, onIcon, asked(e), "icon", "path", e->path, NULL);
    }
    notify(e);
}

const PS5WKAppInfo* ps5wk_appinfo(const char* path, gint64 size, void (*done)(void))
{
    Entry* e = find(path);
    const gint64 now = g_get_monotonic_time();
    GPtrArray* waiting = NULL;
    if (e && !e->a.pending && (e->size != size || (e->a.error && now - e->failedAt > RETRY_US))) {
        /* the file changed, or it failed a while ago: ask again (the
         * callbacks stay) */
        waiting = e->waiting;
        e->waiting = NULL;
        clearEntry(e);
        e = NULL;
    }
    if (!e) {
        /* a free slot, else the least recently used one not waiting for
         * an answer, else the least recently used (its answer is dropped) */
        Entry* victim = NULL;
        for (int i = 0; i < KEEP && !victim; i++) {
            if (!entries[i].path)
                victim = &entries[i];
        }
        for (int pass = 0; pass < 2 && !victim; pass++) {
            for (int i = 0; i < KEEP; i++) {
                Entry* c = &entries[i];
                if (pass == 0 && (c->a.pending || c->a.iconPending))
                    continue;
                if (!victim || c->used < victim->used)
                    victim = c;
            }
        }
        clearEntry(victim);
        e = victim;
        e->path = g_strdup(path);
        e->size = size;
        e->serial = ++serials;
        e->waiting = waiting;
        e->a.pending = TRUE;
        ps5wk_dlservice_request_async(30000, onInfo, asked(e), "titleinfo", "path", path, NULL);
    } else if (waiting)
        g_ptr_array_unref(waiting);
    e->used = g_get_monotonic_time();
    if (done) {
        if (!e->waiting)
            e->waiting = g_ptr_array_new();
        gboolean known = FALSE;
        for (unsigned i = 0; i < e->waiting->len && !known; i++)
            known = g_ptr_array_index(e->waiting, i) == (gpointer)done;
        if (!known)
            g_ptr_array_add(e->waiting, (gpointer)done);
    }
    return &e->a;
}

void ps5wk_appinfo_forget(const char* path)
{
    Entry* e = path ? find(path) : NULL;
    if (e)
        clearEntry(e); /* its serial goes too: answers on their way are dropped */
}

char* ps5wk_appinfo_summary(const MJ* info)
{
    const char* name = mj_string(info, "name");
    const char* id = mj_string(info, "id");
    const char* version = mj_string(info, "version");
    GString* out = g_string_new(NULL);
    if (name && *name)
        g_string_append(out, name);
    if (id && *id)
        g_string_append_printf(out, "%s%s", out->len ? " · " : "", id);
    if (version && *version)
        g_string_append_printf(out, "%sversion %s", out->len ? " · " : "", version);
    return g_string_free(out, FALSE);
}
