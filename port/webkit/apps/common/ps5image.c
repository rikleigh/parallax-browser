/* ps5image.c: what an image or package holds; see ps5image.h.
 *
 * The package layout (CNT header and entries, the FIH wrapper and the
 * entry IDs) follows pkg-viewer (https://github.com/Loopayeh/pkg-viewer,
 * MIT; THIRD-PARTY-NOTICES.md) and psdevwiki's PKG page; the code is this
 * project's own. Everything read from the file is bounded: entry counts,
 * offsets and sizes are checked against the file before they're used.
 */
#include "ps5image.h"

#include "ps5exfat.h"
#include "ps5install.h"
#include "ps5ufs2.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define SFO_MAX (64u * 1024)
#define JSON_MAX (256u * 1024)
#define ICON_MAX (4u * 1024 * 1024)
#define ENTRIES_MAX 10000u
#define NAMES_MAX (1u << 20)

#define ID_NAMES 0x0200u
#define ID_SFO 0x1000u
#define ID_ICON 0x1200u
#define ID_JSON 0x2000u
#define ENTRY_ENCRYPTED 0x80000000u
#define PKG_RETAIL 0x80000000u

static void fail(char* error, size_t size, const char* format, ...)
{
    if (!error || !size)
        return;
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
}

static uint32_t be32(const unsigned char* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t le16(const unsigned char* p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const unsigned char* p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const unsigned char* p)
{
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

static int readAt(int fd, void* buffer, size_t length, uint64_t offset)
{
    size_t got = 0;
    while (got < length) {
        ssize_t n = pread(fd, (char*)buffer + got, length - got, (off_t)(offset + got));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        got += (size_t)n;
    }
    return 0;
}

/* Copies a string from a fixed-size field: printable ASCII and UTF-8 kept,
 * control characters dropped. */
static void copyField(char* out, size_t size, const unsigned char* from, size_t length)
{
    size_t n = 0;
    for (size_t i = 0; i < length && from[i] && n + 1 < size; i++) {
        if (from[i] >= 0x20 && from[i] != 0x7f)
            out[n++] = (char)from[i];
    }
    out[n] = 0;
}

/* "UP9000-PPSA01234_00-..." -> "PPSA01234". */
static void titleFromContentId(const char* content, char* out, size_t size)
{
    if (strlen(content) >= 16 && content[6] == '-') {
        char id[10];
        memcpy(id, content + 7, 9);
        id[9] = 0;
        if (ps5install_valid_id(id)) {
            snprintf(out, size, "%s", id);
            return;
        }
    }
    out[0] = 0;
}

/* The first title ID in a file name ("PPSA01234-app.pkg" -> "PPSA01234"). */
static void titleFromName(const char* path, char* out, size_t size)
{
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (const char* p = base; strlen(p) >= 9; p++) {
        char id[10];
        memcpy(id, p, 9);
        id[9] = 0;
        if (ps5install_valid_id(id) && (p == base || !(p[-1] >= 'A' && p[-1] <= 'Z')) && !(p[9] >= '0' && p[9] <= '9')) {
            snprintf(out, size, "%s", id);
            return;
        }
    }
    out[0] = 0;
}

/* ---------------------------------------------------------------- param.sfo */

int ps5image_parse_sfo(const unsigned char* data, size_t length, PS5ImageInfo* out)
{
    if (length < 20 || memcmp(data, "\0PSF", 4))
        return -1;
    uint32_t keys = le32(data + 8), values = le32(data + 12), count = le32(data + 16);
    if (keys > length || values > length || count > 1000 || 20 + (uint64_t)count * 16 > length)
        return -1;
    char version[32] = "", appVersion[32] = "";
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char* e = data + 20 + i * 16;
        uint32_t keyAt = keys + le16(e), format = le16(e + 2), used = le32(e + 4), max = le32(e + 8), valueAt = values + le32(e + 12);
        if (keyAt >= length || valueAt > length || max > length - valueAt || used > max)
            continue;
        const char* key = (const char*)data + keyAt;
        if (!memchr(key, 0, length - keyAt))
            continue;
        if (format != 0x0204 && format != 0x0004)
            continue; /* not a string */
        char value[256];
        copyField(value, sizeof value, data + valueAt, used);
        if (!strcmp(key, "TITLE"))
            snprintf(out->name, sizeof out->name, "%s", value);
        else if (!strcmp(key, "TITLE_ID") && ps5install_valid_id(value))
            snprintf(out->title_id, sizeof out->title_id, "%s", value);
        else if (!strcmp(key, "CONTENT_ID"))
            snprintf(out->content_id, sizeof out->content_id, "%s", value);
        else if (!strcmp(key, "CATEGORY"))
            snprintf(out->category, sizeof out->category, "%s", value);
        else if (!strcmp(key, "VERSION"))
            snprintf(version, sizeof version, "%s", value);
        else if (!strcmp(key, "APP_VER"))
            snprintf(appVersion, sizeof appVersion, "%s", value);
    }
    /* a patch's VERSION is the base game's; its own is APP_VER */
    snprintf(out->version, sizeof out->version, "%s", !strcmp(out->category, "gp") && *appVersion ? appVersion : *version ? version : appVersion);
    if (!*out->title_id && *out->content_id)
        titleFromContentId(out->content_id, out->title_id, sizeof out->title_id);
    return 0;
}

static void fromParamJson(const char* text, PS5ImageInfo* out)
{
    PS5TitleInfo t;
    if (ps5install_parse_param(text, &t))
        return;
    snprintf(out->title_id, sizeof out->title_id, "%s", t.title_id);
    snprintf(out->name, sizeof out->name, "%s", strcmp(t.name, t.title_id) ? t.name : "");
    snprintf(out->version, sizeof out->version, "%s", t.version);
    if (*t.content_id)
        snprintf(out->content_id, sizeof out->content_id, "%s", t.content_id);
    snprintf(out->category, sizeof out->category, "%s", t.category);
}

/* ---------------------------------------------------------------- packages */

typedef struct {
    uint32_t id, nameAt, flags, offset, size;
} Entry;

/* The CNT at base: its entries, and what param.sfo / param.json say. */
static int readCnt(int fd, uint64_t fileSize, uint64_t base, PS5ImageInfo* out, char* error, size_t size)
{
    unsigned char h[0x80];
    if (base + sizeof h > fileSize || readAt(fd, h, sizeof h, base)) {
        fail(error, size, "the package is cut short");
        return -1;
    }
    if (memcmp(h, "\x7f" "CNT", 4)) {
        fail(error, size, "the package has no CNT header");
        return -1;
    }
    char content[48];
    copyField(content, sizeof content, h + 0x40, 0x24);
    snprintf(out->content_id, sizeof out->content_id, "%s", content);
    if (out->kind == PS5IMAGE_PKG_PS4)
        out->fake = !(be32(h + 4) & PKG_RETAIL);
    uint32_t count = be32(h + 0x10), table = be32(h + 0x18);
    if (!count || count > ENTRIES_MAX || base + table + (uint64_t)count * 32 > fileSize) {
        fail(error, size, "the package's entry table doesn't fit the file");
        return -1;
    }
    Entry* entries = calloc(count, sizeof *entries);
    unsigned char* raw = malloc((size_t)count * 32);
    char* names = NULL;
    uint32_t namesLength = 0;
    if (!entries || !raw || readAt(fd, raw, (size_t)count * 32, base + table)) {
        free(entries);
        free(raw);
        fail(error, size, entries && raw ? "the package couldn't be read" : "out of memory");
        return -1;
    }
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char* e = raw + i * 32;
        entries[i] = (Entry) { be32(e), be32(e + 4), be32(e + 8), be32(e + 16), be32(e + 20) };
    }
    free(raw);
    for (uint32_t i = 0; i < count && !names; i++) {
        const Entry* e = &entries[i];
        if (e->id != ID_NAMES || e->flags & ENTRY_ENCRYPTED || !e->size || e->size > NAMES_MAX || base + e->offset + e->size > fileSize)
            continue;
        if ((names = malloc(e->size + 1)) && !readAt(fd, names, e->size, base + e->offset)) {
            names[e->size] = 0;
            namesLength = e->size;
        } else {
            free(names);
            names = NULL;
        }
    }
    /* each wanted entry by ID, else by name */
    static const struct {
        uint32_t id;
        const char* name;
        uint32_t max;
    } wanted[3] = { { ID_JSON, "param.json", JSON_MAX }, { ID_SFO, "param.sfo", SFO_MAX }, { ID_ICON, "icon0.png", ICON_MAX } };
    const Entry* found[3] = { NULL, NULL, NULL };
    for (uint32_t i = 0; i < count; i++) {
        const Entry* e = &entries[i];
        const char* name = names && e->nameAt < namesLength ? names + e->nameAt : "";
        for (int w = 0; w < 3; w++) {
            if (!found[w] && (e->id == wanted[w].id || !strcmp(name, wanted[w].name)))
                found[w] = e;
        }
    }
    for (int w = 0; w < 3; w++) {
        const Entry* e = found[w];
        if (!e || !e->size)
            continue;
        if (e->flags & ENTRY_ENCRYPTED) {
            out->encrypted = true;
            continue;
        }
        if (e->size > wanted[w].max || base + e->offset + e->size > fileSize)
            continue;
        if (w == 2) {
            out->icon_offset = base + e->offset;
            out->icon_size = e->size;
            out->has_icon = true;
            continue;
        }
        unsigned char* data = malloc(e->size + 1);
        if (!data || readAt(fd, data, e->size, base + e->offset)) {
            free(data);
            continue;
        }
        data[e->size] = 0;
        if (w == 0 && !*out->title_id)
            fromParamJson((const char*)data, out);
        else if (w == 1 && !*out->title_id)
            ps5image_parse_sfo(data, e->size, out);
        free(data);
    }
    free(names);
    free(entries);
    if (!*out->title_id && *out->content_id)
        titleFromContentId(out->content_id, out->title_id, sizeof out->title_id);
    return 0;
}

/* ---------------------------------------------------------------- images */

static void readImage(const char* path, PS5ImageInfo* out)
{
    int ufs = out->kind == PS5IMAGE_UFS2;
#define READ(file, buffer, size, length) \
    (ufs ? ps5ufs2_read_file(path, file, buffer, size, length, why, sizeof why) \
         : ps5exfat_read_file(path, file, buffer, size, length, why, sizeof why))
    char why[256];
    uint64_t length = 0;
    char* text = malloc(JSON_MAX + 1);
    if (!text)
        return;
    bool param = false;
    if (!READ("sce_sys/param.json", text, JSON_MAX, &length)) {
        text[length < JSON_MAX ? length : JSON_MAX] = 0;
        fromParamJson(text, out);
        param = *out->title_id != 0;
        snprintf(out->platform, sizeof out->platform, "PS5");
    } else if (!READ("sce_sys/param.sfo", text, SFO_MAX, &length) && length <= SFO_MAX) {
        param = !ps5image_parse_sfo((unsigned char*)text, (size_t)length, out) && *out->title_id;
        snprintf(out->platform, sizeof out->platform, "PS4");
    }
    free(text);
    out->app_at_root = param && !READ("eboot.bin", NULL, 0, &length);
    out->has_icon = !READ("sce_sys/icon0.png", NULL, 0, &length) && length > 8 && length <= ICON_MAX;
    out->icon_size = out->has_icon ? length : 0;
#undef READ
}

/* ---------------------------------------------------------------- the file */

int ps5image_info(const char* path, PS5ImageInfo* out, char* error, size_t size)
{
    memset(out, 0, sizeof *out);
    if (error && size)
        error[0] = 0;
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        fail(error, size, fd < 0 ? "%s couldn't be opened (errno %d)" : "%s isn't a file", path, errno);
        if (fd >= 0)
            close(fd);
        return -1;
    }
    out->size = (uint64_t)st.st_size;
    unsigned char head[0x100];
    memset(head, 0, sizeof head);
    size_t got = out->size < sizeof head ? (size_t)out->size : sizeof head;
    if (readAt(fd, head, got, 0)) {
        close(fd);
        fail(error, size, "%s couldn't be read (errno %d)", path, errno);
        return -1;
    }
    unsigned char ufsMagic[4] = { 0 };
    int result = 0;
    if (got >= 0x80 && !memcmp(head, "\x7f" "CNT", 4)) {
        out->kind = PS5IMAGE_PKG_PS4;
        out->format = "pkg";
        out->package = true;
        snprintf(out->platform, sizeof out->platform, "PS4");
        result = readCnt(fd, out->size, 0, out, error, size);
        out->label = out->fake ? "PS4 fake package" : "PS4 package (retail)";
    } else if (got >= 0x60 && !memcmp(head, "\x7f" "FIH", 4)) {
        out->kind = PS5IMAGE_PKG_PS5;
        out->format = "pkg";
        out->package = true;
        snprintf(out->platform, sizeof out->platform, "PS5");
        out->fake = head[5] != 0x80;
        uint64_t cnt = le64(head + 0x58);
        unsigned char magic[4];
        if (cnt && cnt + 0x80 > out->size) {
            fail(error, size, "the package is cut short (%llu bytes; its contents start at %llu)", (unsigned long long)out->size,
                (unsigned long long)cnt);
            result = -1;
        } else if (cnt && !readAt(fd, magic, 4, cnt) && !memcmp(magic, "\x7f" "CNT", 4))
            result = readCnt(fd, out->size, cnt, out, error, size);
        else
            out->encrypted = true; /* a retail package: nothing readable inside */
        out->label = out->encrypted && !out->fake ? "PS5 package (retail, encrypted)" : out->fake ? "PS5 fake package (fPKG)"
                                                                                                   : "PS5 package (retail)";
        if (!*out->title_id)
            titleFromName(path, out->title_id, sizeof out->title_id);
    } else if (got >= 11 && !memcmp(head + 3, "EXFAT   ", 8)) {
        unsigned char sig[2];
        if (readAt(fd, sig, 2, 510) || sig[0] != 0x55 || sig[1] != 0xaa) {
            fail(error, size, "an exFAT boot sector without its signature");
            result = -1;
        } else {
            out->kind = PS5IMAGE_EXFAT;
            out->format = "exfat";
            out->label = "exFAT image";
            readImage(path, out);
        }
    } else if (out->size >= 65536 + 1376 && !readAt(fd, ufsMagic, 4, 65536 + 1372) && le32(ufsMagic) == 0x19540119u) {
        out->kind = PS5IMAGE_UFS2;
        out->format = "ffpkg";
        out->label = "UFS2 image";
        readImage(path, out);
    } else if (got >= 16 && (le64(head) == 1 || le64(head) == 2) && le64(head + 8) == 20130315ull) {
        out->kind = PS5IMAGE_PFS;
        out->format = "pfs";
        out->label = "PFS image";
    } else {
        fail(error, size, "not an image or package this knows");
        result = -1;
    }
    close(fd);
    return result;
}

int ps5image_icon(const char* path, const char* out_png, char* error, size_t size)
{
    PS5ImageInfo info;
    if (ps5image_info(path, &info, error, size))
        return -1;
    if (!info.has_icon) {
        fail(error, size, info.encrypted ? "the package is encrypted: its icon can't be read" : "no icon0.png in it");
        return -1;
    }
    if (info.icon_size < 8 || info.icon_size > ICON_MAX) {
        fail(error, size, "its icon0.png is too big");
        return -1;
    }
    unsigned char* png = malloc((size_t)info.icon_size);
    if (!png) {
        fail(error, size, "out of memory");
        return -1;
    }
    char why[256] = "";
    uint64_t length = 0;
    int r;
    if (info.package) {
        int fd = open(path, O_RDONLY);
        r = fd < 0 || readAt(fd, png, (size_t)info.icon_size, info.icon_offset) ? -1 : 0;
        if (fd >= 0)
            close(fd);
        length = info.icon_size;
    } else if (info.kind == PS5IMAGE_UFS2)
        r = ps5ufs2_read_file(path, "sce_sys/icon0.png", png, (size_t)info.icon_size, &length, why, sizeof why);
    else
        r = ps5exfat_read_file(path, "sce_sys/icon0.png", png, (size_t)info.icon_size, &length, why, sizeof why);
    if (r || length != info.icon_size) {
        free(png);
        fail(error, size, "its icon couldn't be read%s%s", *why ? ": " : "", why);
        return -1;
    }
    if (memcmp(png, "\x89PNG\r\n\x1a\n", 8)) {
        free(png);
        fail(error, size, "its icon0.png isn't a PNG");
        return -1;
    }
    /* a name of its own: two requests for the same icon at once (the TV's
     * dialog and the phone page) mustn't write one file */
    static unsigned serial;
    size_t n = strlen(out_png) + 48;
    char* part = malloc(n);
    if (!part) {
        free(png);
        fail(error, size, "out of memory");
        return -1;
    }
    snprintf(part, n, "%s.%ld.%u.part", out_png, (long)getpid(), __atomic_add_fetch(&serial, 1, __ATOMIC_RELAXED));
    int fd = open(part, O_WRONLY | O_CREAT | O_EXCL, 0666);
    r = -1;
    if (fd >= 0) {
        size_t done = 0;
        while (done < length) {
            ssize_t w = write(fd, png + done, (size_t)length - done);
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0)
                break;
            done += (size_t)w;
        }
        r = close(fd) || done != length ? -1 : 0;
    }
    if (!r && rename(part, out_png))
        r = -1;
    if (r) {
        fail(error, size, "%s couldn't be written (errno %d)", out_png, errno);
        unlink(part);
    }
    free(part);
    free(png);
    return r;
}

/* ---------------------------------------------------------------- JSON */

typedef struct {
    char* text;
    size_t length, capacity;
    int failed;
} Text;

static void add(Text* t, const char* s, size_t n)
{
    if (t->failed)
        return;
    if (t->length + n + 1 > t->capacity) {
        size_t capacity = (t->capacity + n + 1) * 2;
        char* grown = realloc(t->text, capacity);
        if (!grown) {
            t->failed = 1;
            return;
        }
        t->text = grown;
        t->capacity = capacity;
    }
    memcpy(t->text + t->length, s, n);
    t->length += n;
    t->text[t->length] = 0;
}

static void addf(Text* t, const char* format, ...)
{
    char buffer[128];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    if (n > 0)
        add(t, buffer, (size_t)n < sizeof buffer ? (size_t)n : sizeof buffer - 1);
}

static void addString(Text* t, const char* key, const char* value)
{
    addf(t, ",\"%s\":", key);
    if (!value) {
        add(t, "null", 4);
        return;
    }
    add(t, "\"", 1);
    for (const unsigned char* p = (const unsigned char*)value; *p; p++) {
        if (*p == '"' || *p == '\\') {
            char e[2] = { '\\', (char)*p };
            add(t, e, 2);
        } else if (*p < 0x20)
            addf(t, "\\u%04x", *p);
        else
            add(t, (const char*)p, 1);
    }
    add(t, "\"", 1);
}

char* ps5image_json(const PS5ImageInfo* i)
{
    Text t = { 0 };
    add(&t, "{\"kind\":", 8);
    add(&t, "\"", 1);
    add(&t, i->format ? i->format : "", strlen(i->format ? i->format : ""));
    add(&t, "\"", 1);
    addString(&t, "label", i->label ? i->label : "");
    addString(&t, "platform", i->platform);
    addString(&t, "name", i->name);
    addString(&t, "id", i->title_id);
    addString(&t, "contentId", i->content_id);
    addString(&t, "version", i->version);
    addString(&t, "category", i->category);
    addf(&t, ",\"package\":%s,\"fake\":%s,\"encrypted\":%s,\"appAtRoot\":%s,\"icon\":%s,\"size\":%llu}", i->package ? "true" : "false",
        i->fake ? "true" : "false", i->encrypted ? "true" : "false", i->app_at_root ? "true" : "false", i->has_icon ? "true" : "false",
        (unsigned long long)i->size);
    if (t.failed) {
        free(t.text);
        return NULL;
    }
    return t.text;
}

static bool endsWith(const char* name, const char* tail)
{
    size_t n = name ? strlen(name) : 0, m = strlen(tail);
    return n > m && !strcasecmp(name + n - m, tail);
}

bool ps5image_is_image_name(const char* name)
{
    return endsWith(name, ".exfat") || endsWith(name, ".ffpkg");
}

bool ps5image_is_pkg_name(const char* name)
{
    return endsWith(name, ".pkg");
}
