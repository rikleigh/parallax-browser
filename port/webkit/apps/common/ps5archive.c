/* ps5archive.c: unpacking downloaded archives with libarchive; see
 * ps5archive.h.
 *
 * Every archive is hostile. ps5archive_extract reads the whole directory
 * first (inspect) and refuses before anything is written; then it reads
 * the archive again and checks that each entry is the one it inspected, in
 * the same order, with no more data than its header said. Files and folders
 * are made one part at a time with *at() calls from an open dest, so no
 * link on the disk is followed, and files only with O_EXCL. */
#include "ps5archive.h"

#include <archive.h>
#include <archive_entry.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define WRITE_CHUNK (4u * 1024 * 1024) /* archive_read_data fills it, then one write() */
#define READ_BLOCK (1024 * 1024)       /* libarchive's reads from the volumes */
#define SOURCE_BLOCK (64 * 1024)       /* and from a source (a server: only what it needs) */
#define MAX_PATH_LENGTH 1000           /* relative, under dest */
#define MAX_PART_LENGTH 255
#define MAX_VOLUMES 9999
#define MAX_NAME_BYTES (32u << 20) /* every path, kept between the passes */
#define PROGRESS_NS 100000000ll /* at most ~10 calls a second */

#define DEFAULT_MAX_ENTRIES 200000u
#define DEFAULT_MAX_BYTES (512ull << 30)
#define DEFAULT_FREE_MARGIN (64ull << 20)

typedef struct {
    char* path;    /* cleaned: "a/b/c", no "." parts, '/' only */
    int64_t size;  /* -1: not in the header */
    int directory;
} Entry;

typedef struct {
    Entry* entries;
    unsigned count;
    unsigned capacity;
} EntryList;

static void fail(char* error, size_t size, const char* format, ...)
{
    if (!error || !size)
        return;
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
}

/* A name short enough for an error on the TV. */
static const char* shown(const char* name, char* out, size_t size)
{
    size_t length = strlen(name);
    if (length < size) {
        memcpy(out, name, length + 1);
        return out;
    }
    /* "...the end of it": the file name is the useful part */
    snprintf(out, size, "...%s", name + length - (size - 4));
    return out;
}

static const char* baseName(const char* path)
{
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void formatSize(char* out, size_t size, uint64_t bytes)
{
    if (bytes >= (1ull << 30))
        snprintf(out, size, "%.1f GB", (double)bytes / (double)(1ull << 30));
    else if (bytes >= (1ull << 20))
        snprintf(out, size, "%.0f MB", (double)bytes / (double)(1ull << 20));
    else
        snprintf(out, size, "%llu KB", (unsigned long long)((bytes + 1023) >> 10));
}

static int64_t nowNs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec;
}

static int endsWithNoCase(const char* s, size_t length, const char* suffix)
{
    size_t n = strlen(suffix);
    return length >= n && !strncasecmp(s + length - n, suffix, n);
}

static int allDigits(const char* s, size_t length)
{
    if (!length)
        return 0;
    for (size_t i = 0; i < length; i++) {
        if (!isdigit((unsigned char)s[i]))
            return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- volumes */

typedef enum {
    SET_SINGLE,
    SET_NUMBERED, /* name.7z.001, name.zip.001: plain byte splits */
    SET_PART,     /* name.part1.rar, name.part01.rar */
    SET_OLD_RAR,  /* name.rar, name.r00, name.r01.. */
    SET_ZIP_SPLIT /* name.z01, name.z02.., name.zip (last) */
} SetKind;

typedef struct {
    SetKind kind;
    char dir[1024];   /* "" or ending in '/' */
    char stem[512];   /* the name without the set's suffix */
    char ext[8];      /* SET_NUMBERED: "7z" or "zip" as written */
    unsigned number;  /* the part given (SET_SINGLE: 0) */
    unsigned width;   /* digits in its number */
} SetName;

/* Works on the name's last part only; dir keeps the rest. */
static void parseSetName(const char* path, SetName* s)
{
    memset(s, 0, sizeof *s);
    const char* name = baseName(path);
    size_t dirLength = (size_t)(name - path);
    if (dirLength >= sizeof s->dir)
        dirLength = sizeof s->dir - 1;
    memcpy(s->dir, path, dirLength);
    s->dir[dirLength] = 0;
    size_t length = strlen(name);
    snprintf(s->stem, sizeof s->stem, "%s", name);

    /* name.7z.001 / name.zip.001 */
    const char* dot = strrchr(name, '.');
    if (dot && allDigits(dot + 1, strlen(dot + 1)) && strlen(dot + 1) <= 4) {
        size_t stemEnd = (size_t)(dot - name);
        const char* exts[] = { ".7z", ".zip" };
        for (unsigned i = 0; i < 2; i++) {
            size_t n = strlen(exts[i]);
            if (stemEnd > n && !strncasecmp(name + stemEnd - n, exts[i], n)) {
                s->kind = SET_NUMBERED;
                snprintf(s->stem, sizeof s->stem, "%.*s", (int)(stemEnd - n), name);
                snprintf(s->ext, sizeof s->ext, "%.*s", (int)(n - 1), name + stemEnd - n + 1);
                s->number = (unsigned)strtoul(dot + 1, NULL, 10);
                s->width = (unsigned)strlen(dot + 1);
                return;
            }
        }
    }
    /* name.partN.rar */
    if (endsWithNoCase(name, length, ".rar")) {
        size_t end = length - 4, start = end;
        while (start > 0 && isdigit((unsigned char)name[start - 1]))
            start--;
        if (start < end && end - start <= 4 && start >= 5 && !strncasecmp(name + start - 5, ".part", 5)) {
            s->kind = SET_PART;
            snprintf(s->stem, sizeof s->stem, "%.*s", (int)(start - 5), name);
            s->number = (unsigned)strtoul(name + start, NULL, 10);
            s->width = (unsigned)(end - start);
            return;
        }
        snprintf(s->stem, sizeof s->stem, "%.*s", (int)end, name);
        s->kind = SET_OLD_RAR; /* if name.r00 is there; see ps5archive_volumes */
        s->number = 0;
        return;
    }
    /* name.r00 (the volume after name.rar), name.z01 */
    if (length > 4 && name[length - 4] == '.' && isdigit((unsigned char)name[length - 2])
        && isdigit((unsigned char)name[length - 1])) {
        char c = (char)tolower((unsigned char)name[length - 3]);
        if (c == 'r' || c == 'z') {
            s->kind = c == 'r' ? SET_OLD_RAR : SET_ZIP_SPLIT;
            snprintf(s->stem, sizeof s->stem, "%.*s", (int)(length - 4), name);
            s->number = (unsigned)atoi(name + length - 2) + (c == 'r' ? 2 : 0); /* as volumeNumber */
            s->width = 2;
            return;
        }
    }
    if (endsWithNoCase(name, length, ".zip")) {
        snprintf(s->stem, sizeof s->stem, "%.*s", (int)(length - 4), name);
        s->kind = SET_ZIP_SPLIT; /* if name.z01 is there */
        s->number = 0;
        return;
    }
    s->kind = SET_SINGLE;
}

bool ps5archive_is_archive(const char* path)
{
    const char* name = baseName(path);
    size_t length = strlen(name);
    static const char* const suffixes[] = { ".zip", ".7z", ".rar", ".tar", ".tgz", ".tar.gz", ".tbz", ".tbz2",
        ".tar.bz2", ".txz", ".tar.xz", ".tzst", ".tar.zst", ".tar.lzma" };
    for (unsigned i = 0; i < sizeof suffixes / sizeof *suffixes; i++) {
        if (length > strlen(suffixes[i]) && endsWithNoCase(name, length, suffixes[i]))
            return true;
    }
    SetName s;
    parseSetName(name, &s);
    return s.kind == SET_NUMBERED || (s.kind == SET_OLD_RAR && s.number) || (s.kind == SET_ZIP_SPLIT && s.number);
}

void ps5archive_free_list(char** list)
{
    if (!list)
        return;
    for (char** p = list; *p; p++)
        free(*p);
    free(list);
}

/* The number of the volume called name in the set (0: not in it). The
 * stem must match exactly, the rest without case. Old-style RAR: name.rar
 * is 1, name.r00 is 2. Split ZIP: name.z01 is 1.., name.zip is 0 here (it's
 * the last, placed after the others). */
static unsigned volumeNumber(const SetName* s, const char* name, unsigned* width)
{
    size_t stem = strlen(s->stem), length = strlen(name);
    if (length <= stem || memcmp(name, s->stem, stem))
        return 0;
    const char* rest = name + stem;
    size_t restLength = length - stem;
    switch (s->kind) {
    case SET_NUMBERED: {
        size_t e = strlen(s->ext);
        if (restLength < e + 3 || rest[0] != '.' || strncasecmp(rest + 1, s->ext, e) || rest[e + 1] != '.')
            return 0;
        const char* digits = rest + e + 2;
        if (!allDigits(digits, strlen(digits)) || strlen(digits) > 4)
            return 0;
        *width = (unsigned)strlen(digits);
        return (unsigned)strtoul(digits, NULL, 10);
    }
    case SET_PART: {
        if (restLength < 10 || strncasecmp(rest, ".part", 5) || !endsWithNoCase(rest, restLength, ".rar"))
            return 0;
        size_t n = restLength - 9;
        if (!allDigits(rest + 5, n) || n > 4)
            return 0;
        *width = (unsigned)n;
        return (unsigned)strtoul(rest + 5, NULL, 10);
    }
    case SET_OLD_RAR:
        if (restLength == 4 && !strcasecmp(rest, ".rar"))
            return 1;
        if (restLength == 4 && rest[0] == '.' && tolower((unsigned char)rest[1]) == 'r' && allDigits(rest + 2, 2))
            return (unsigned)atoi(rest + 2) + 2;
        return 0;
    case SET_ZIP_SPLIT:
        if (restLength == 4 && rest[0] == '.' && tolower((unsigned char)rest[1]) == 'z' && allDigits(rest + 2, 2))
            return (unsigned)atoi(rest + 2);
        return 0;
    default:
        return 0;
    }
}

static int readTail(const char* path, unsigned char* buffer, size_t want, size_t* got, uint64_t* fileSize)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st)) {
        close(fd);
        return -1;
    }
    *fileSize = (uint64_t)st.st_size;
    size_t n = (uint64_t)st.st_size < want ? (size_t)st.st_size : want;
    ssize_t r = pread(fd, buffer, n, st.st_size - (off_t)n);
    close(fd);
    if (r != (ssize_t)n)
        return -1;
    *got = n;
    return 0;
}

static int readHead(const char* path, unsigned char* buffer, size_t want)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t r = pread(fd, buffer, want, 0);
    close(fd);
    return r == (ssize_t)want ? 0 : -1;
}

static uint32_t crc32Plain(const unsigned char* p, size_t n)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static uint64_t le64(const unsigned char* p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

/* Whether a RAR volume says another one follows: the end-of-archive block
 * near its end (volumes made to a fixed size have zeros after it), RAR5
 * end-of-archive flag 1, RAR4 ENDARC_NEXT_VOLUME. 1 more, 0 last, -1 can't
 * tell. */
static int rarHasNext(const char* path)
{
    unsigned char tail[64];
    size_t got;
    uint64_t size;
    if (readTail(path, tail, sizeof tail, &got, &size) || got < 8)
        return -1;
    for (size_t i = got - 7; i-- > 0;) {
        const unsigned char* h = tail + i;
        /* RAR5: CRC32, then the vints size 3, type 5, flags, end flags */
        if (i + 8 <= got && h[4] == 3 && h[5] == 5) {
            uint32_t crc = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
            if (crc == crc32Plain(h + 4, 4))
                return h[7] & 1;
        }
        /* RAR4: HEAD_CRC (the low 16 bits of the CRC32 from HEAD_TYPE),
         * HEAD_TYPE 0x7b, HEAD_FLAGS, HEAD_SIZE (7, or up to 20 with a data
         * CRC, a volume number and reserved bytes) */
        size_t headSize = h[5] | (size_t)h[6] << 8;
        if (h[2] == 0x7b && headSize >= 7 && headSize <= 20 && i + headSize <= got) {
            unsigned crc = h[0] | h[1] << 8;
            if ((crc32Plain(h + 2, headSize - 2) & 0xffff) == crc)
                return (h[3] | h[4] << 8) & 1;
        }
    }
    return -1;
}

/* The ZIP end of central directory record in the last 64 KB of path: 1
 * found (*disk: its "number of this disk"), 0 not there although the file
 * is big enough to hold it, -1 can't tell. */
static int zipEnd(const char* path, unsigned* disk)
{
    enum { TAIL = 65535 + 22 };
    unsigned char* tail = malloc(TAIL); /* not on the stack: job threads */
    size_t got;
    uint64_t size;
    int result = -1;
    if (tail && !readTail(path, tail, TAIL, &got, &size) && got >= 22) {
        result = got == TAIL ? 0 : -1;
        for (size_t i = got - 22 + 1; i-- > 0;) {
            if (tail[i] == 'P' && tail[i + 1] == 'K' && tail[i + 2] == 5 && tail[i + 3] == 6) {
                *disk = tail[i + 4] | tail[i + 5] << 8;
                result = 1;
                break;
            }
        }
    }
    free(tail);
    return result;
}

static void missingPart(char* error, size_t size, unsigned number, unsigned total, const char* name)
{
    char n[96];
    shown(name, n, sizeof n);
    if (total)
        fail(error, size, "Part %u of %u is missing: %s", number, total, n);
    else
        fail(error, size, "Part %u is missing: %s", number, n);
}

/* The name the set's part number would have (for the error). */
static void partName(const SetName* s, unsigned number, unsigned width, char* out, size_t size)
{
    switch (s->kind) {
    case SET_NUMBERED:
        snprintf(out, size, "%s.%s.%0*u", s->stem, s->ext, (int)width, number);
        break;
    case SET_PART:
        snprintf(out, size, "%s.part%0*u.rar", s->stem, (int)width, number);
        break;
    case SET_OLD_RAR:
        if (number == 1)
            snprintf(out, size, "%s.rar", s->stem);
        else
            snprintf(out, size, "%s.r%02u", s->stem, number - 2);
        break;
    case SET_ZIP_SPLIT:
        snprintf(out, size, "%s.z%02u", s->stem, number);
        break;
    default:
        snprintf(out, size, "%s", s->stem);
    }
}

char** ps5archive_volumes(const char* path, char* error, size_t errorSize)
{
    SetName s;
    parseSetName(path, &s);
    struct stat st;

    char** names = NULL; /* [number] = the name in dir, 1-based */
    unsigned last = 0;
    char zipLast[512] = ""; /* SET_ZIP_SPLIT: name.zip as it's written */
    if (s.kind != SET_SINGLE) {
        DIR* d = opendir(s.dir[0] ? s.dir : ".");
        if (!d) {
            fail(error, errorSize, "The folder can't be read (errno %d)", errno);
            return NULL;
        }
        names = calloc(MAX_VOLUMES + 2, sizeof *names);
        if (!names) {
            closedir(d);
            fail(error, errorSize, "Out of memory");
            return NULL;
        }
        struct dirent* de;
        while ((de = readdir(d))) {
            unsigned width = 0, n = volumeNumber(&s, de->d_name, &width);
            if (s.kind == SET_ZIP_SPLIT && !strncmp(de->d_name, s.stem, strlen(s.stem))
                && !strcasecmp(de->d_name + strlen(s.stem), ".zip"))
                snprintf(zipLast, sizeof zipLast, "%s", de->d_name);
            if (!n || n > MAX_VOLUMES)
                continue;
            /* two spellings of one number: keep the given one's width */
            if (names[n] && (s.width && width != s.width))
                continue;
            free(names[n]);
            names[n] = strdup(de->d_name);
            if (n > last)
                last = n;
        }
        closedir(d);
        /* name.rar with no name.r00, name.zip with no name.z01: one file */
        if ((s.kind == SET_OLD_RAR || s.kind == SET_ZIP_SPLIT) && !s.number && last <= (s.kind == SET_OLD_RAR ? 1u : 0u)) {
            for (unsigned i = 0; i <= MAX_VOLUMES; i++)
                free(names[i]);
            free(names);
            names = NULL;
            s.kind = SET_SINGLE;
        }
    }
    if (s.kind == SET_SINGLE) {
        if (stat(path, &st) || !S_ISREG(st.st_mode)) {
            char n[96];
            fail(error, errorSize, "The file is missing: %s", shown(baseName(path), n, sizeof n));
            return NULL;
        }
        char** list = calloc(2, sizeof *list);
        if (!list || !(list[0] = strdup(path))) {
            free(list);
            fail(error, errorSize, "Out of memory");
            return NULL;
        }
        return list;
    }

    /* How many parts the set has, where the archive says so. */
    unsigned total = 0;
    unsigned width = s.width ? s.width : (s.kind == SET_NUMBERED ? 3 : s.kind == SET_PART ? 1 : 2);
    char full[2048];
    if (s.kind == SET_NUMBERED && names[1] && !strcasecmp(s.ext, "7z")) {
        /* 7z start header: the next header's offset and size give the
         * archive's length; the parts are all the first one's size */
        unsigned char head[32];
        snprintf(full, sizeof full, "%s%s", s.dir, names[1]);
        if (!readHead(full, head, sizeof head) && !memcmp(head, "7z\xbc\xaf\x27\x1c", 6) && !stat(full, &st) && st.st_size > 0) {
            uint64_t length = 32 + le64(head + 12) + le64(head + 20);
            uint64_t parts = (length + (uint64_t)st.st_size - 1) / (uint64_t)st.st_size;
            if (parts >= last && parts <= MAX_VOLUMES)
                total = (unsigned)parts;
        }
    }
    if (s.kind == SET_NUMBERED && names[last] && !strcasecmp(s.ext, "zip")) {
        /* a byte split of a ZIP ends with its end of central directory; a
         * last part big enough to hold it whole but without it isn't the
         * last */
        unsigned disk;
        snprintf(full, sizeof full, "%s%s", s.dir, names[last]);
        if (!zipEnd(full, &disk)) {
            char n[600];
            partName(&s, last + 1, width, n, sizeof n);
            missingPart(error, errorSize, last + 1, 0, n);
            goto failed;
        }
    }
    if (s.kind == SET_ZIP_SPLIT) {
        if (!zipLast[0]) {
            char n[600];
            snprintf(n, sizeof n, "%s.zip", s.stem);
            char m[96];
            fail(error, errorSize, "The last part is missing: %s", shown(n, m, sizeof m));
            goto failed;
        }
        /* end of central directory: "number of this disk" is the parts
         * before name.zip */
        unsigned disk;
        snprintf(full, sizeof full, "%s%s", s.dir, zipLast);
        if (zipEnd(full, &disk) == 1 && disk >= last && disk < MAX_VOLUMES)
            total = disk + 1;
        if (total)
            last = total - 1;
    } else if (total > last)
        last = total;
    if (s.kind != SET_ZIP_SPLIT && total == 0 && last) {
        /* RAR: the last volume found says whether more follow */
        if ((s.kind == SET_PART || s.kind == SET_OLD_RAR) && names[last]) {
            snprintf(full, sizeof full, "%s%s", s.dir, names[last]);
            if (rarHasNext(full) == 1) {
                char n[600];
                partName(&s, last + 1, width, n, sizeof n);
                missingPart(error, errorSize, last + 1, 0, n);
                goto failed;
            }
        }
    }
    for (unsigned i = 1; i <= last; i++) {
        if (!names[i]) {
            char n[600];
            partName(&s, i, width, n, sizeof n);
            missingPart(error, errorSize, i, s.kind == SET_ZIP_SPLIT ? last + 1 : last, n);
            goto failed;
        }
    }
    if (!last && s.kind != SET_ZIP_SPLIT) {
        char n[96];
        fail(error, errorSize, "The file is missing: %s", shown(baseName(path), n, sizeof n));
        goto failed;
    }

    unsigned count = last + (s.kind == SET_ZIP_SPLIT ? 1 : 0);
    char** list = calloc(count + 1, sizeof *list);
    if (!list)
        goto oom;
    for (unsigned i = 1; i <= count; i++) {
        const char* name = i <= last ? names[i] : zipLast;
        size_t n = strlen(s.dir) + strlen(name) + 1;
        if (!(list[i - 1] = malloc(n))) {
            ps5archive_free_list(list);
            goto oom;
        }
        snprintf(list[i - 1], n, "%s%s", s.dir, name);
    }
    for (unsigned i = 0; i <= MAX_VOLUMES; i++)
        free(names[i]);
    free(names);
    return list;
oom:
    fail(error, errorSize, "Out of memory");
failed:
    for (unsigned i = 0; i <= MAX_VOLUMES; i++)
        free(names[i]);
    free(names);
    return NULL;
}

/* ---------------------------------------------------------------- entries */

/* The entry's path made safe, or why not: no leading '/' or '\', no drive
 * letter, no ".." part, no control character; '\' is a separator (archives
 * made on Windows), "" and "." parts are dropped, bytes that aren't UTF-8
 * become '_'. 0 ok; 1 the root itself ("./"); -1 refused. */
static int cleanPath(const char* raw, char* out, size_t size, char* error, size_t errorSize)
{
    char n[96];
    if (!raw || !raw[0]) {
        fail(error, errorSize, "An entry in the archive has no name");
        return -1;
    }
    if (raw[0] == '/' || raw[0] == '\\' || (isalpha((unsigned char)raw[0]) && raw[1] == ':')) {
        fail(error, errorSize, "Unsafe path in the archive: %s", shown(raw, n, sizeof n));
        return -1;
    }
    size_t length = strlen(raw);
    if (length >= 4096) {
        fail(error, errorSize, "A name in the archive is too long");
        return -1;
    }
    size_t o = 0;
    size_t i = 0;
    while (i <= length) {
        size_t start = i;
        while (i < length && raw[i] != '/' && raw[i] != '\\')
            i++;
        size_t part = i - start;
        i++; /* past the separator (or the end) */
        if (!part || (part == 1 && raw[start] == '.'))
            continue;
        if (part == 2 && raw[start] == '.' && raw[start + 1] == '.') {
            fail(error, errorSize, "Unsafe path in the archive: %s", shown(raw, n, sizeof n));
            return -1;
        }
        if (part > MAX_PART_LENGTH) {
            fail(error, errorSize, "A name in the archive is too long: %s", shown(raw, n, sizeof n));
            return -1;
        }
        if (o + part + 2 > size || o + part + 1 > MAX_PATH_LENGTH) {
            fail(error, errorSize, "A path in the archive is too long: %s", shown(raw, n, sizeof n));
            return -1;
        }
        if (o)
            out[o++] = '/';
        for (size_t k = start; k < start + part;) {
            unsigned char c = (unsigned char)raw[k];
            if (c < 0x20 || c == 0x7f) {
                fail(error, errorSize, "A name in the archive has control characters");
                return -1;
            }
            /* one UTF-8 sequence, or '_' for a byte that doesn't start one */
            unsigned need = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 9;
            int valid = need < 9 && c != 0xc0 && c != 0xc1 && c <= 0xf4 && k + need < start + part;
            for (unsigned m = 1; valid && m <= need; m++)
                valid = k + m < start + part && ((unsigned char)raw[k + m] & 0xc0) == 0x80;
            if (!valid) {
                out[o++] = '_';
                k++;
                continue;
            }
            memcpy(out + o, raw + k, need + 1);
            o += need + 1;
            k += need + 1;
        }
    }
    out[o] = 0;
    if (!o)
        return 1;
    /* "." and ".." can't come back from '_' replacements: '_' isn't '.' */
    return 0;
}

static int compareEntries(const void* a, const void* b)
{
    return strcmp(((const Entry*)a)->path, ((const Entry*)b)->path);
}

static void freeEntries(EntryList* list)
{
    for (unsigned i = 0; i < list->count; i++)
        free(list->entries[i].path);
    free(list->entries);
    memset(list, 0, sizeof *list);
}

static int addEntry(EntryList* list, const char* path, int64_t size, int directory)
{
    if (list->count == list->capacity) {
        unsigned capacity = list->capacity ? list->capacity * 2 : 256;
        Entry* entries = realloc(list->entries, capacity * sizeof *entries);
        if (!entries)
            return -1;
        list->entries = entries;
        list->capacity = capacity;
    }
    Entry* e = &list->entries[list->count];
    if (!(e->path = strdup(path)))
        return -1;
    e->size = size;
    e->directory = directory;
    list->count++;
    return 0;
}

static void limitsOrDefaults(const PS5ArchiveLimits* in, PS5ArchiveLimits* out)
{
    memset(out, 0, sizeof *out);
    if (in)
        *out = *in;
    if (!out->max_entries)
        out->max_entries = DEFAULT_MAX_ENTRIES;
    if (!out->max_bytes)
        out->max_bytes = DEFAULT_MAX_BYTES;
    if (!out->free_margin)
        out->free_margin = DEFAULT_FREE_MARGIN;
}

static struct archive* newReader(char* error, size_t errorSize)
{
    struct archive* a = archive_read_new();
    if (!a) {
        fail(error, errorSize, "Out of memory");
        return NULL;
    }
    /* Only what downloads hold: fewer parsers facing hostile input. */
    archive_read_support_format_zip(a);
    archive_read_support_format_7zip(a);
    archive_read_support_format_rar(a);
    archive_read_support_format_rar5(a);
    archive_read_support_format_tar(a);
    archive_read_support_filter_gzip(a);
    archive_read_support_filter_bzip2(a);
    archive_read_support_filter_xz(a);
    archive_read_support_filter_lzma(a);
    archive_read_support_filter_zstd(a);
    return a;
}

static void openFailed(struct archive* a, char* error, size_t errorSize)
{
    const char* why = archive_error_string(a);
    if (why && strstr(why, "nrecognized"))
        fail(error, errorSize, "This isn't an archive that can be unpacked");
    else
        fail(error, errorSize, "The archive can't be opened (%s)", why ? why : "unknown error");
    archive_read_free(a);
}

static struct archive* openSet(char** volumes, char* error, size_t errorSize)
{
    struct archive* a = newReader(error, errorSize);
    if (!a)
        return NULL;
    if (archive_read_open_filenames(a, (const char**)volumes, READ_BLOCK) != ARCHIVE_OK) {
        openFailed(a, error, errorSize);
        return NULL;
    }
    return a;
}

/* A PS5ArchiveSource as libarchive's client: reads of READ_BLOCK at the
 * position, seeks and skips that only move it (so the formats that can
 * seek, ZIP and 7-Zip, read their directory at the end, and RAR skips
 * each file's data). */
typedef struct {
    const PS5ArchiveSource* source;
    uint64_t at;
    char* buffer;
} SourceClient;

static la_ssize_t sourceRead(struct archive* a, void* data, const void** out)
{
    SourceClient* c = data;
    int64_t n = c->source->read_at(c->source->data, c->at, c->buffer, SOURCE_BLOCK);
    if (n < 0) {
        archive_set_error(a, EIO, "%s", c->source->error && c->source->error(c->source->data) ? c->source->error(c->source->data) : "read failed");
        return -1;
    }
    c->at += (uint64_t)n;
    *out = c->buffer;
    return (la_ssize_t)n;
}

static la_int64_t sourceSkip(struct archive* a, void* data, la_int64_t request)
{
    (void)a;
    SourceClient* c = data;
    uint64_t left = c->source->size > c->at ? c->source->size - c->at : 0;
    la_int64_t n = request < 0 ? 0 : (uint64_t)request > left ? (la_int64_t)left : request;
    c->at += (uint64_t)n;
    return n;
}

static la_int64_t sourceSeek(struct archive* a, void* data, la_int64_t offset, int whence)
{
    SourceClient* c = data;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)c->at : (int64_t)c->source->size;
    int64_t to = base + offset;
    if (to < 0 || (uint64_t)to > c->source->size) {
        archive_set_error(a, EINVAL, "seek outside the file");
        return ARCHIVE_FATAL;
    }
    c->at = (uint64_t)to;
    return to;
}

static struct archive* openSource(const PS5ArchiveSource* source, SourceClient* client, char* error, size_t errorSize)
{
    client->source = source;
    client->at = 0;
    client->buffer = malloc(READ_BLOCK);
    struct archive* a = client->buffer ? newReader(error, errorSize) : NULL;
    if (!a) {
        if (!client->buffer)
            fail(error, errorSize, "Out of memory");
        free(client->buffer);
        client->buffer = NULL;
        return NULL;
    }
    archive_read_set_read_callback(a, sourceRead);
    archive_read_set_skip_callback(a, sourceSkip);
    archive_read_set_seek_callback(a, sourceSeek);
    archive_read_set_callback_data(a, client);
    if (archive_read_open1(a) != ARCHIVE_OK) {
        openFailed(a, error, errorSize);
        free(client->buffer);
        client->buffer = NULL;
        return NULL;
    }
    return a;
}

static int isEncryptionError(struct archive* a)
{
    if (archive_read_has_encrypted_entries(a) > 0)
        return 1;
    const char* why = archive_error_string(a);
    return why && (strstr(why, "ncrypt") || strstr(why, "assword"));
}

/* A failed libarchive read: refused (-1) if it's encryption, else -2. */
static int readFailure(struct archive* a, char* error, size_t errorSize)
{
    if (isEncryptionError(a)) {
        fail(error, errorSize, "Encrypted archives aren't supported");
        return -1;
    }
    const char* why = archive_error_string(a);
    if (why && strstr(why, "nrecognized"))
        fail(error, errorSize, "This isn't an archive that can be unpacked");
    else if (why && strstr(why, "nsupported"))
        fail(error, errorSize, "The archive uses something that isn't supported (%s)", why);
    else
        fail(error, errorSize, "The archive is damaged or incomplete (%s)", why ? why : "read error");
    return -2;
}

static void formatName(struct archive* a, char* out, size_t size)
{
    int format = archive_format(a) & ARCHIVE_FORMAT_BASE_MASK;
    const char* base = format == ARCHIVE_FORMAT_ZIP ? "ZIP"
        : format == ARCHIVE_FORMAT_7ZIP             ? "7-Zip"
        : format == ARCHIVE_FORMAT_RAR              ? "RAR"
        : format == ARCHIVE_FORMAT_RAR_V5           ? "RAR5"
        : format == ARCHIVE_FORMAT_TAR              ? "tar"
                                                    : NULL;
    char other[32];
    if (!base) {
        snprintf(other, sizeof other, "%s", archive_format_name(a) ? archive_format_name(a) : "?");
        base = other;
    }
    const char* filter = "";
    switch (archive_filter_code(a, 0)) {
    case ARCHIVE_FILTER_GZIP:
        filter = ".gz";
        break;
    case ARCHIVE_FILTER_BZIP2:
        filter = ".bz2";
        break;
    case ARCHIVE_FILTER_XZ:
        filter = ".xz";
        break;
    case ARCHIVE_FILTER_LZMA:
        filter = ".lzma";
        break;
    case ARCHIVE_FILTER_ZSTD:
        filter = ".zst";
        break;
    default:
        break;
    }
    snprintf(out, size, "%s%s", base, filter);
}

/* The entry's type and links: 0 a file or folder (*directory says which). */
/* An entry's kind: filetype is libarchive's AE_IF* (also for the ZIP
 * reader below); link: it names a link target. 0 (directory set) or -1. */
static int checkKind(unsigned filetype, int link, const char* path, int* directory, char* error, size_t errorSize)
{
    char n[96];
    if (link || filetype == AE_IFLNK) {
        fail(error, errorSize, "Links aren't supported: %s", shown(path, n, sizeof n));
        return -1;
    }
    if (filetype == AE_IFREG || filetype == AE_IFDIR) {
        *directory = filetype == AE_IFDIR;
        return 0;
    }
    fail(error, errorSize, "Special files aren't supported: %s", shown(path, n, sizeof n));
    return -1;
}

/* What inspecting has seen so far. */
typedef struct {
    const PS5ArchiveLimits* limits;
    PS5ArchiveInfo* info;
    EntryList* list;
    size_t nameBytes;
    char* error;
    size_t errorSize;
} Inspect;

/* One entry's header: checked (path, kind, limits) and added to the list.
 * size < 0: not known (a folder's is ignored). 0, -1 refused, -2 memory. */
static int takeEntry(Inspect* s, const char* raw, unsigned filetype, int link, int64_t size)
{
    char path[MAX_PATH_LENGTH + 8];
    char n[96];
    int clean = cleanPath(raw, path, sizeof path, s->error, s->errorSize);
    if (clean < 0)
        return -1;
    int directory = 0;
    if (checkKind(filetype, link, clean ? "./" : path, &directory, s->error, s->errorSize))
        return -1;
    if (clean == 1) {
        if (!directory) {
            fail(s->error, s->errorSize, "Unsafe path in the archive: %s", shown(raw, n, sizeof n));
            return -1;
        }
        return 0; /* "./": dest itself */
    }
    PS5ArchiveInfo* info = s->info;
    if (++info->entries > s->limits->max_entries) {
        fail(s->error, s->errorSize, "The archive has more than %u files", s->limits->max_entries);
        return -1;
    }
    if (directory)
        size = -1;
    else if (size >= 0)
        info->unpacked += (uint64_t)size;
    else
        info->sizes_known = false;
    if (info->unpacked > s->limits->max_bytes) {
        char text[32];
        formatSize(text, sizeof text, s->limits->max_bytes);
        fail(s->error, s->errorSize, "The archive unpacks to more than %s", text);
        return -1;
    }
    s->nameBytes += strlen(path) + 1;
    if (s->nameBytes > MAX_NAME_BYTES) {
        fail(s->error, s->errorSize, "The archive's list of files is too big");
        return -1;
    }
    if (addEntry(s->list, path, size, directory)) {
        fail(s->error, s->errorSize, "Out of memory");
        return -2;
    }
    return 0;
}

static int finishInspect(PS5ArchiveInfo* info, EntryList* list, char* error, size_t errorSize);

/* Reads every header of the set into list (cleaned paths, in archive
 * order) and fills info. 0, -1, -2 as ps5archive_inspect. Frees a. */
static int inspectArchive(struct archive* a, const PS5ArchiveLimits* limits, PS5ArchiveInfo* info, EntryList* list, char* error,
    size_t errorSize)
{
    info->sizes_known = true;
    Inspect s = { limits, info, list, 0, error, errorSize };
    int result = 0;
    for (;;) {
        struct archive_entry* entry;
        int r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF)
            break;
        if (r < ARCHIVE_WARN) {
            result = readFailure(a, error, errorSize);
            info->encrypted = result == -1;
            break;
        }
        if (archive_entry_is_encrypted(entry) || archive_read_has_encrypted_entries(a) > 0) {
            info->encrypted = true;
            fail(error, errorSize, "Encrypted archives aren't supported");
            result = -1;
            break;
        }
        const char* raw = archive_entry_pathname_utf8(entry);
        if (!raw)
            raw = archive_entry_pathname(entry);
        int64_t size = archive_entry_size_is_set(entry) && archive_entry_size(entry) >= 0 ? archive_entry_size(entry) : -1;
        result = takeEntry(&s, raw, archive_entry_filetype(entry), archive_entry_hardlink(entry) || archive_entry_symlink(entry), size);
        if (result)
            break;
    }
    if (!result) {
        formatName(a, info->format, sizeof info->format);
        result = finishInspect(info, list, error, errorSize);
    }
    archive_read_free(a);
    return result;
}

/* After the headers: not empty, no duplicates, the top folder, the apps. */
static int finishInspect(PS5ArchiveInfo* info, EntryList* list, char* error, size_t errorSize)
{
    int result = 0;
    char n[96];
    Entry* sorted = NULL;
    if (!info->entries) {
        fail(error, errorSize, "The archive is empty");
        result = -1;
        goto done;
    }

    /* Duplicates, and a file that's also a folder ("a" and "a/b"). */
    sorted = malloc(list->count * sizeof *sorted);
    if (!sorted) {
        fail(error, errorSize, "Out of memory");
        result = -2;
        goto done;
    }
    memcpy(sorted, list->entries, list->count * sizeof *sorted);
    qsort(sorted, list->count, sizeof *sorted, compareEntries);
    for (unsigned i = 0; i < list->count && !result; i++) {
        const Entry* e = &sorted[i];
        if (i && !strcmp(sorted[i - 1].path, e->path) && !(sorted[i - 1].directory && e->directory)) {
            fail(error, errorSize, "The archive has %s twice", shown(e->path, n, sizeof n));
            result = -1;
            break;
        }
        if (e->directory)
            continue;
        /* the first path after e that starts with "e/" */
        size_t length = strlen(e->path);
        unsigned lo = i + 1, hi = list->count;
        while (lo < hi) {
            unsigned mid = lo + (hi - lo) / 2;
            int c = strncmp(sorted[mid].path, e->path, length);
            if (c < 0 || (c == 0 && (unsigned char)sorted[mid].path[length] < '/'))
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < list->count && !strncmp(sorted[lo].path, e->path, length) && sorted[lo].path[length] == '/') {
            fail(error, errorSize, "The archive has %s as a file and a folder", shown(e->path, n, sizeof n));
            result = -1;
        }
    }
    if (result)
        goto done;

    /* The one top folder, and a homebrew title (the shallowest folder with
     * both sce_sys/param.json and eboot.bin). */
    const char* first = list->entries[0].path;
    size_t topLength = strcspn(first, "/");
    int oneTop = topLength < sizeof info->top_folder - 1;
    int titleDepth = -1;
    for (unsigned i = 0; i < list->count; i++) {
        const Entry* e = &list->entries[i];
        if (oneTop && (strncmp(e->path, first, topLength) || (e->path[topLength] != '/' && e->path[topLength])
                          || (!e->path[topLength] && !e->directory)))
            oneTop = 0;
        if (e->directory)
            continue;
        size_t length = strlen(e->path);
        static const char param[] = "sce_sys/param.json";
        size_t p = sizeof param - 1;
        if (length < p || strcmp(e->path + length - p, param) || (length > p && e->path[length - p - 1] != '/'))
            continue;
        size_t dirLength = length > p ? length - p - 1 : 0;
        char eboot[MAX_PATH_LENGTH + 16];
        snprintf(eboot, sizeof eboot, "%.*s%seboot.bin", (int)dirLength, e->path, dirLength ? "/" : "");
        Entry key = { .path = eboot };
        const Entry* found = bsearch(&key, sorted, list->count, sizeof *sorted, compareEntries);
        if (!found || found->directory)
            continue;
        int depth = 0;
        for (size_t k = 0; k < dirLength; k++)
            depth += e->path[k] == '/';
        depth += dirLength > 0;
        char dir[MAX_PATH_LENGTH + 8];
        snprintf(dir, sizeof dir, "%.*s", (int)dirLength, e->path);
        info->titles++;
        if (titleDepth < 0 || depth < titleDepth || (depth == titleDepth && strcmp(dir, info->title_dir) < 0)) {
            titleDepth = depth;
            info->has_title = true;
            snprintf(info->title_dir, sizeof info->title_dir, "%s", dir);
        }
    }
    if (oneTop)
        snprintf(info->top_folder, sizeof info->top_folder, "%.*s/", (int)topLength, first);
done:
    free(sorted);
    return result;
}

static int inspectSet(char** volumes, const PS5ArchiveLimits* limits, PS5ArchiveInfo* info, EntryList* list, char* error,
    size_t errorSize)
{
    memset(info, 0, sizeof *info);
    for (char** v = volumes; *v; v++)
        info->volumes++;
    struct archive* a = openSet(volumes, error, errorSize);
    if (!a)
        return -2;
    return inspectArchive(a, limits, info, list, error, errorSize);
}

int ps5archive_inspect(const char* path, const PS5ArchiveLimits* limits, PS5ArchiveInfo* info, char* error,
    size_t errorSize)
{
    PS5ArchiveLimits l;
    limitsOrDefaults(limits, &l);
    PS5ArchiveInfo scratch;
    if (!info)
        info = &scratch;
    memset(info, 0, sizeof *info);
    char** volumes = ps5archive_volumes(path, error, errorSize);
    if (!volumes)
        return -2;
    EntryList list = { 0 };
    int r = inspectSet(volumes, &l, info, &list, error, errorSize);
    freeEntries(&list);
    ps5archive_free_list(volumes);
    return r;
}

/* ---------------------------------------------------------------- ZIP's directory, read directly (a source) */

/* libarchive's seekable ZIP reader reads every file's local header too: on
 * a server, a request per file. The end record and the central directory
 * say everything inspecting needs, in one or two reads; param.json is then
 * read from its local header (stored or deflated). Anything this doesn't
 * handle (a split ZIP, data before the archive, another method) returns 1
 * and libarchive reads it instead. */

#define ZIP_TAIL (65535u + 22u)
#define ZIP_DIR_MAX (64u << 20)
#define ZIP_ENTRY_MAX (8u << 20) /* compressed bytes read for one file */

static uint16_t zip16(const unsigned char* p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t zip32(const unsigned char* p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int sourceReadAll(const PS5ArchiveSource* src, uint64_t offset, void* buffer, size_t length, char* error, size_t errorSize)
{
    int64_t n = src->read_at(src->data, offset, buffer, length);
    if (n == (int64_t)length)
        return 0;
    const char* why = n < 0 && src->error ? src->error(src->data) : NULL;
    fail(error, errorSize, "%s", why ? why : n < 0 ? "read failed" : "The archive is damaged or incomplete (cut short)");
    return -2;
}

/* The central directory into *dir (malloc'd). 0; 1 not a ZIP this reads;
 * -2 a read failed. */
static int zipDirectory(const PS5ArchiveSource* src, unsigned char** dir, uint64_t* dirSize, uint64_t* count, char* error, size_t errorSize)
{
    *dir = NULL;
    if (src->size < 22)
        return 1;
    size_t tailLength = (size_t)(src->size < ZIP_TAIL ? src->size : ZIP_TAIL);
    uint64_t tailAt = src->size - tailLength;
    unsigned char* tail = malloc(tailLength);
    if (!tail) {
        fail(error, errorSize, "Out of memory");
        return -2;
    }
    int r = sourceReadAll(src, tailAt, tail, tailLength, error, errorSize);
    if (r) {
        free(tail);
        return r;
    }
    /* the end record: the last "PK\5\6" whose comment ends the file */
    const unsigned char* end = NULL;
    for (size_t i = tailLength - 22 + 1; i-- > 0;) {
        const unsigned char* p = tail + i;
        if (p[0] == 'P' && p[1] == 'K' && p[2] == 5 && p[3] == 6 && i + 22 + zip16(p + 20) == tailLength) {
            end = p;
            break;
        }
    }
    if (!end || zip16(end + 4) || zip16(end + 6) || zip16(end + 8) != zip16(end + 10)) {
        free(tail);
        return 1; /* not a ZIP, or one of several disks */
    }
    uint64_t entries = zip16(end + 10), size = zip32(end + 12), offset = zip32(end + 16);
    if (entries == 0xFFFF || size == 0xFFFFFFFFu || offset == 0xFFFFFFFFu) {
        /* ZIP64: the locator just before, then its end record */
        size_t at = (size_t)(end - tail);
        if (at < 20 || memcmp(end - 20, "PK\6\7", 4) || zip32(end - 20 + 4) || zip32(end - 20 + 16) > 1) {
            free(tail);
            return 1;
        }
        uint64_t recordAt = (uint64_t)zip32(end - 20 + 8) | (uint64_t)zip32(end - 20 + 12) << 32;
        unsigned char record[56];
        if (recordAt + sizeof record > src->size) {
            free(tail);
            return 1;
        }
        r = sourceReadAll(src, recordAt, record, sizeof record, error, errorSize);
        if (r) {
            free(tail);
            return r;
        }
        if (memcmp(record, "PK\6\6", 4) || zip32(record + 16) || zip32(record + 20)) {
            free(tail);
            return 1;
        }
        entries = (uint64_t)zip32(record + 32) | (uint64_t)zip32(record + 36) << 32;
        size = (uint64_t)zip32(record + 40) | (uint64_t)zip32(record + 44) << 32;
        offset = (uint64_t)zip32(record + 48) | (uint64_t)zip32(record + 52) << 32;
    }
    free(tail);
    if (offset + size > src->size || size > ZIP_DIR_MAX || (entries && size < 46 * entries))
        return 1;
    *dir = malloc(size ? size : 1);
    if (!*dir) {
        fail(error, errorSize, "Out of memory");
        return -2;
    }
    r = sourceReadAll(src, offset, *dir, (size_t)size, error, errorSize);
    if (r) {
        free(*dir);
        *dir = NULL;
        return r;
    }
    if (size >= 4 && memcmp(*dir, "PK\1\2", 4)) {
        free(*dir);
        *dir = NULL;
        return 1; /* data before the archive moves every offset */
    }
    *dirSize = size;
    *count = entries;
    return 0;
}

typedef struct {
    char name[MAX_PATH_LENGTH + 8]; /* as stored (NUL added) */
    unsigned filetype;
    int link;
    int encrypted;
    unsigned method;
    uint64_t compressed, size, local;
} ZipEntry;

/* The entry at *at in dir (moves *at past it): 0, or 1 when the directory
 * isn't what this reads. */
static int zipNext(const unsigned char* dir, uint64_t dirSize, uint64_t* at, ZipEntry* e)
{
    if (*at + 46 > dirSize || memcmp(dir + *at, "PK\1\2", 4))
        return 1;
    const unsigned char* p = dir + *at;
    unsigned madeBy = zip16(p + 4), flags = zip16(p + 8);
    unsigned nameLength = zip16(p + 28), extraLength = zip16(p + 30), commentLength = zip16(p + 32);
    uint32_t external = zip32(p + 38);
    if (*at + 46 + nameLength + extraLength + commentLength > dirSize || nameLength == 0 || nameLength > MAX_PATH_LENGTH)
        return 1;
    memset(e, 0, sizeof *e);
    memcpy(e->name, p + 46, nameLength);
    e->name[nameLength] = 0;
    if (memchr(e->name, 0, nameLength))
        return 1;
    e->method = zip16(p + 10);
    e->encrypted = (flags & 1) || e->method == 99;
    e->compressed = zip32(p + 20);
    e->size = zip32(p + 24);
    e->local = zip32(p + 42);
    /* ZIP64 sizes and offset, in that order, for the fields that are full */
    const unsigned char* x = p + 46 + nameLength;
    const unsigned char* xEnd = x + extraLength;
    while (x + 4 <= xEnd) {
        unsigned id = zip16(x), length = zip16(x + 2);
        const unsigned char* v = x + 4;
        if (v + length > xEnd)
            break;
        if (id == 1) {
            const unsigned char* w = v;
            uint64_t* fields[] = { &e->size, &e->compressed, &e->local };
            for (unsigned i = 0; i < 3; i++) {
                if (*fields[i] != 0xFFFFFFFFu)
                    continue;
                if (w + 8 > v + length)
                    return 1;
                *fields[i] = (uint64_t)zip32(w) | (uint64_t)zip32(w + 4) << 32;
                w += 8;
            }
        }
        x = v + length;
    }
    /* the kind: a Unix mode if the maker was Unix and gave one, else the
     * name's trailing '/' (or DOS's folder bit) */
    unsigned mode = madeBy >> 8 == 3 ? external >> 16 : 0;
    int slash = e->name[nameLength - 1] == '/' || e->name[nameLength - 1] == '\\';
    if (mode & 0170000) {
        unsigned type = mode & 0170000;
        e->filetype = type == 0100000 ? AE_IFREG : type == 0040000 ? AE_IFDIR : type == 0120000 ? AE_IFLNK : AE_IFIFO;
        if (e->filetype == AE_IFREG && slash)
            e->filetype = AE_IFDIR;
    } else
        e->filetype = slash || (external & 0x10) ? AE_IFDIR : AE_IFREG;
    *at += 46 + nameLength + extraLength + commentLength;
    return 0;
}

/* 0, -1, -2 as inspect; 1 libarchive should read it. */
static int zipInspect(const PS5ArchiveSource* src, const PS5ArchiveLimits* limits, PS5ArchiveInfo* info, EntryList* list, char* error,
    size_t errorSize)
{
    unsigned char* dir;
    uint64_t dirSize = 0, count = 0;
    int r = zipDirectory(src, &dir, &dirSize, &count, error, errorSize);
    if (r)
        return r;
    info->sizes_known = true;
    Inspect s = { limits, info, list, 0, error, errorSize };
    uint64_t at = 0;
    ZipEntry e;
    for (uint64_t i = 0; i < count && !r; i++) {
        if (zipNext(dir, dirSize, &at, &e)) {
            r = 1;
            break;
        }
        if (e.encrypted) {
            info->encrypted = true;
            fail(error, errorSize, "Encrypted archives aren't supported");
            r = -1;
            break;
        }
        r = takeEntry(&s, e.name, e.filetype, 0, (int64_t)e.size);
    }
    free(dir);
    if (r == 1) {
        /* start again with libarchive */
        freeEntries(list);
        memset(list, 0, sizeof *list);
        unsigned volumes = info->volumes;
        memset(info, 0, sizeof *info);
        info->volumes = volumes;
        return 1;
    }
    if (r)
        return r;
    snprintf(info->format, sizeof info->format, "ZIP");
    return finishInspect(info, list, error, errorSize);
}

/* The file called name: 0, -1 not there or too big, -2 read failure, 1
 * libarchive should read it. */
static int zipReadEntry(const PS5ArchiveSource* src, const char* name, char* buffer, size_t size, size_t* length, char* error,
    size_t errorSize)
{
    unsigned char* dir;
    uint64_t dirSize = 0, count = 0;
    int r = zipDirectory(src, &dir, &dirSize, &count, error, errorSize);
    if (r)
        return r;
    uint64_t at = 0;
    ZipEntry e;
    char path[MAX_PATH_LENGTH + 8];
    char scratch[256];
    int found = 0;
    for (uint64_t i = 0; i < count && !found; i++) {
        if (zipNext(dir, dirSize, &at, &e)) {
            free(dir);
            return 1;
        }
        found = e.filetype == AE_IFREG && cleanPath(e.name, path, sizeof path, scratch, sizeof scratch) == 0 && !strcmp(path, name);
    }
    free(dir);
    if (!found) {
        fail(error, errorSize, "%s isn't in the archive", name);
        return -1;
    }
    if (e.encrypted) {
        fail(error, errorSize, "Encrypted archives aren't supported");
        return -1;
    }
    if (e.size > size - 1 || e.compressed > ZIP_ENTRY_MAX) {
        fail(error, errorSize, "%s is too big", name);
        return -1;
    }
    if (e.method != 0 && e.method != 8)
        return 1;
    unsigned char header[30];
    if (e.local + sizeof header > src->size)
        return 1;
    r = sourceReadAll(src, e.local, header, sizeof header, error, errorSize);
    if (r)
        return r;
    if (memcmp(header, "PK\3\4", 4))
        return 1;
    uint64_t dataAt = e.local + 30 + zip16(header + 26) + zip16(header + 28);
    if (dataAt + e.compressed > src->size)
        return 1;
    unsigned char* data = malloc(e.compressed ? e.compressed : 1);
    if (!data) {
        fail(error, errorSize, "Out of memory");
        return -2;
    }
    r = sourceReadAll(src, dataAt, data, (size_t)e.compressed, error, errorSize);
    if (r) {
        free(data);
        return r;
    }
    if (e.method == 0) {
        if (e.compressed != e.size) {
            free(data);
            return 1;
        }
        memcpy(buffer, data, (size_t)e.size);
    } else {
        z_stream z;
        memset(&z, 0, sizeof z);
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK) {
            free(data);
            fail(error, errorSize, "Out of memory");
            return -2;
        }
        z.next_in = data;
        z.avail_in = (uInt)e.compressed;
        z.next_out = (Bytef*)buffer;
        z.avail_out = (uInt)(size - 1);
        int zr = inflate(&z, Z_FINISH);
        uint64_t out = z.total_out;
        inflateEnd(&z);
        if (zr != Z_STREAM_END || out != e.size) {
            free(data);
            fail(error, errorSize, "The archive is damaged or incomplete (%s)", name);
            return -2;
        }
    }
    free(data);
    buffer[e.size] = 0;
    if (length)
        *length = (size_t)e.size;
    return 0;
}

int ps5archive_inspect_source(const PS5ArchiveSource* source, const PS5ArchiveLimits* limits, PS5ArchiveInfo* info, char* error,
    size_t errorSize)
{
    PS5ArchiveLimits l;
    limitsOrDefaults(limits, &l);
    PS5ArchiveInfo scratch;
    if (!info)
        info = &scratch;
    memset(info, 0, sizeof *info);
    info->volumes = 1;
    EntryList list = { 0 };
    int r = zipInspect(source, &l, info, &list, error, errorSize);
    if (r != 1) {
        freeEntries(&list);
        return r;
    }
    SourceClient client;
    struct archive* a = openSource(source, &client, error, errorSize);
    if (!a)
        return -2;
    r = inspectArchive(a, &l, info, &list, error, errorSize);
    freeEntries(&list);
    free(client.buffer);
    return r;
}

/* Reads entry name of an open archive into buffer (see ps5archive_read_entry). */
static int readEntry(struct archive* a, const char* name, char* buffer, size_t size, size_t* length, char* error, size_t errorSize)
{
    char path[MAX_PATH_LENGTH + 8];
    char scratch[256];
    for (;;) {
        struct archive_entry* entry;
        int r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF) {
            fail(error, errorSize, "%s isn't in the archive", name);
            return -1;
        }
        if (r < ARCHIVE_WARN)
            return readFailure(a, error, errorSize);
        if (archive_entry_is_encrypted(entry)) {
            fail(error, errorSize, "Encrypted archives aren't supported");
            return -1;
        }
        const char* raw = archive_entry_pathname_utf8(entry);
        if (!raw)
            raw = archive_entry_pathname(entry);
        if (cleanPath(raw, path, sizeof path, scratch, sizeof scratch) != 0 || strcmp(path, name)
            || archive_entry_filetype(entry) != AE_IFREG)
            continue;
        if (archive_entry_size_is_set(entry) && archive_entry_size(entry) > (la_int64_t)size - 1) {
            fail(error, errorSize, "%s is too big", name);
            return -1;
        }
        size_t n = 0;
        for (;;) {
            la_ssize_t got = archive_read_data(a, buffer + n, size - 1 - n);
            if (got < 0)
                return readFailure(a, error, errorSize);
            if (got == 0)
                break;
            n += (size_t)got;
            if (n == size - 1) {
                char extra;
                if (archive_read_data(a, &extra, 1) > 0) {
                    fail(error, errorSize, "%s is too big", name);
                    return -1;
                }
                break;
            }
        }
        buffer[n] = 0;
        if (length)
            *length = n;
        return 0;
    }
}

int ps5archive_read_entry(const char* path, const PS5ArchiveSource* source, const char* name, char* buffer, size_t size,
    size_t* length, char* error, size_t errorSize)
{
    if (!buffer || size < 2 || !name || (!path && !source)) {
        fail(error, errorSize, "Nothing to read");
        return -1;
    }
    buffer[0] = 0;
    if (length)
        *length = 0;
    if (source) {
        int r = zipReadEntry(source, name, buffer, size, length, error, errorSize);
        if (r != 1)
            return r;
        buffer[0] = 0;
        if (error && errorSize)
            error[0] = 0;
        SourceClient client;
        struct archive* a = openSource(source, &client, error, errorSize);
        if (!a)
            return -2;
        r = readEntry(a, name, buffer, size, length, error, errorSize);
        archive_read_free(a);
        free(client.buffer);
        return r;
    }
    char** volumes = ps5archive_volumes(path, error, errorSize);
    if (!volumes)
        return -2;
    struct archive* a = openSet(volumes, error, errorSize);
    ps5archive_free_list(volumes);
    if (!a)
        return -2;
    int r = readEntry(a, name, buffer, size, length, error, errorSize);
    archive_read_free(a);
    return r;
}

/* ---------------------------------------------------------------- writing */

typedef struct {
    int root;          /* dest, open */
    char parent[MAX_PATH_LENGTH + 8];
    int parentFd;      /* the folder parent names (-1 none; root's own fd for "") */
} Folders;

static int writeAll(int fd, const void* buffer, size_t length)
{
    size_t done = 0;
    while (done < length) {
        ssize_t n = write(fd, (const char*)buffer + done, length - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

/* Opens the folder relative names under dest, making each missing part
 * (0777). A part that is a link or a file on the disk fails (ELOOP or
 * ENOTDIR). Returns an fd the caller doesn't close (Folders keeps it). */
static int openFolder(Folders* f, const char* relative, char* error, size_t errorSize)
{
    if (f->parentFd >= 0 && !strcmp(f->parent, relative))
        return f->parentFd;
    if (f->parentFd >= 0 && f->parentFd != f->root)
        close(f->parentFd);
    f->parentFd = -1;
    f->parent[0] = 0;
    int fd = f->root;
    char part[MAX_PART_LENGTH + 1];
    const char* p = relative;
    char n[96];
    while (*p) {
        size_t length = strcspn(p, "/");
        memcpy(part, p, length);
        part[length] = 0;
        /* The SDK's mkdirat returns an errno as a positive result (13.60;
         * openat and fstatat too, which ps5compat's --wrap fixes:
         * port/deps/compat/atfuncs.c). */
        int r = mkdirat(fd, part, 0777);
        int err = !r ? 0 : r > 0 ? r : errno;
        int made = !err;
        if (err && err != EEXIST) {
            fail(error, errorSize, "A folder couldn't be made: %s (errno %d)", shown(relative, n, sizeof n), err);
            goto failed;
        }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) {
            struct stat st;
            if (!fstatat(fd, part, &st, AT_SYMLINK_NOFOLLOW) && S_ISLNK(st.st_mode))
                fail(error, errorSize, "A folder on the drive is a link: %s", shown(part, n, sizeof n));
            else if (!fstatat(fd, part, &st, AT_SYMLINK_NOFOLLOW) && !S_ISDIR(st.st_mode))
                fail(error, errorSize, "A file is in the way of a folder: %s", shown(part, n, sizeof n));
            else
                fail(error, errorSize, "A folder couldn't be opened: %s (errno %d)", shown(part, n, sizeof n), errno);
            goto failed;
        }
        if (made)
            fchmod(next, 0777);
        if (fd != f->root)
            close(fd);
        fd = next;
        p += length;
        if (*p == '/')
            p++;
    }
    f->parentFd = fd;
    snprintf(f->parent, sizeof f->parent, "%s", relative);
    return fd;
failed:
    if (fd != f->root)
        close(fd);
    return -1;
}

/* mkdir -p for an absolute or relative dest (made 0777). */
static int makeDest(const char* dest, char* error, size_t errorSize)
{
    char path[2048];
    int n = snprintf(path, sizeof path, "%s", dest);
    if (n <= 0 || (size_t)n >= sizeof path) {
        fail(error, errorSize, "The destination path is too long");
        return -1;
    }
    for (char* p = path + 1;; p++) {
        if (*p && *p != '/')
            continue;
        char saved = *p;
        *p = 0;
        if (!mkdir(path, 0777))
            chmod(path, 0777);
        else if (errno != EEXIST) {
            fail(error, errorSize, "The destination folder couldn't be made (errno %d)", errno);
            return -1;
        }
        *p = saved;
        if (!saved)
            break;
    }
    return 0;
}

int ps5archive_extract(const char* path, const char* dest, const PS5ArchiveLimits* limits,
    ps5archive_progress_fn progress, void* data, volatile int* cancel, char* error, size_t errorSize)
{
    PS5ArchiveLimits l;
    limitsOrDefaults(limits, &l);
    PS5ArchiveInfo info;
    char** volumes = ps5archive_volumes(path, error, errorSize);
    if (!volumes)
        return -2;
    EntryList list = { 0 };
    struct archive* a = NULL;
    char* buffer = NULL;
    Folders folders = { .root = -1, .parentFd = -1 };
    int out = -1;
    char n[96];
    int result = inspectSet(volumes, &l, &info, &list, error, errorSize);
    if (result)
        goto done;
    result = -2;
    if (cancel && *cancel) {
        fail(error, errorSize, "Cancelled");
        result = -3;
        goto done;
    }

    if (makeDest(dest, error, errorSize))
        goto done;
    folders.root = open(dest, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (folders.root < 0) {
        fail(error, errorSize, "The destination folder couldn't be opened (errno %d)", errno);
        goto done;
    }
    struct statvfs vfs;
    if (!fstatvfs(folders.root, &vfs)) {
        uint64_t avail = (uint64_t)vfs.f_bavail * (uint64_t)(vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize);
        uint64_t need = info.unpacked + l.free_margin;
        if (need < info.unpacked)
            need = UINT64_MAX;
        if (need > avail) {
            char s[32], f[32];
            formatSize(s, sizeof s, need);
            formatSize(f, sizeof f, avail);
            fail(error, errorSize, "Not enough space: needs %s, %s free", s, f);
            goto done;
        }
    }

    buffer = malloc(WRITE_CHUNK);
    if (!buffer) {
        fail(error, errorSize, "Out of memory");
        goto done;
    }
    a = openSet(volumes, error, errorSize);
    if (!a)
        goto done;

    uint64_t unpacked = 0, total = info.unpacked;
    int64_t lastProgress = 0;
    unsigned index = 0;
    char clean[MAX_PATH_LENGTH + 8];
    char folder[MAX_PATH_LENGTH + 8];
    if (progress) {
        progress(0, total, "", data);
        lastProgress = nowNs();
    }
    for (;;) {
        if (cancel && *cancel) {
            fail(error, errorSize, "Cancelled");
            result = -3;
            goto done;
        }
        struct archive_entry* entry;
        int r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF)
            break;
        if (r < ARCHIVE_WARN) {
            readFailure(a, error, errorSize);
            goto done; /* -2: files may have been written */
        }
        const char* raw = archive_entry_pathname_utf8(entry);
        if (!raw)
            raw = archive_entry_pathname(entry);
        int c = cleanPath(raw, clean, sizeof clean, error, errorSize);
        if (c == 1)
            continue;
        int directory = 0;
        /* The second reading must give what the first did. */
        if (c < 0 || index >= list.count || strcmp(clean, list.entries[index].path)
            || checkKind(archive_entry_filetype(entry), archive_entry_hardlink(entry) || archive_entry_symlink(entry), clean, &directory, error,
                errorSize)
            || directory != list.entries[index].directory
            || archive_entry_is_encrypted(entry)) {
            fail(error, errorSize, "The archive changed while it was unpacked");
            goto done;
        }
        const Entry* e = &list.entries[index++];
        if (directory) {
            if (openFolder(&folders, e->path, error, errorSize) < 0)
                goto done;
            continue;
        }
        const char* slash = strrchr(e->path, '/');
        snprintf(folder, sizeof folder, "%.*s", slash ? (int)(slash - e->path) : 0, e->path);
        int parent = openFolder(&folders, folder, error, errorSize);
        if (parent < 0)
            goto done;
        const char* name = slash ? slash + 1 : e->path;
        out = openat(parent, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0777);
        if (out < 0) {
            if (errno == EEXIST)
                fail(error, errorSize, "%s already exists", shown(e->path, n, sizeof n));
            else
                fail(error, errorSize, "%s couldn't be made (errno %d)", shown(e->path, n, sizeof n), errno);
            goto done;
        }
        fchmod(out, 0777);
        uint64_t written = 0;
        for (;;) {
            if (cancel && *cancel) {
                fail(error, errorSize, "Cancelled");
                result = -3;
                goto done;
            }
            la_ssize_t got = archive_read_data(a, buffer, WRITE_CHUNK);
            if (got == 0)
                break;
            if (got < 0) {
                readFailure(a, error, errorSize);
                goto done;
            }
            written += (uint64_t)got;
            unpacked += (uint64_t)got;
            if ((e->size >= 0 && written > (uint64_t)e->size) || unpacked > l.max_bytes) {
                fail(error, errorSize, "The archive is damaged: %s is bigger than it says", shown(e->path, n, sizeof n));
                goto done;
            }
            if (writeAll(out, buffer, (size_t)got)) {
                if (errno == ENOSPC)
                    fail(error, errorSize, "Not enough space on the drive");
                else
                    fail(error, errorSize, "%s couldn't be written (errno %d)", shown(e->path, n, sizeof n), errno);
                goto done;
            }
            int64_t now = progress ? nowNs() : 0;
            if (progress && now - lastProgress >= PROGRESS_NS) {
                progress(unpacked, unpacked > total ? unpacked : total, e->path, data);
                lastProgress = now;
            }
        }
        /* a sparse file's hole at its end is a size, not data */
        if (e->size >= 0 && written < (uint64_t)e->size && archive_entry_sparse_count(entry) > 0
            && !ftruncate(out, (off_t)e->size)) {
            unpacked += (uint64_t)e->size - written;
            written = (uint64_t)e->size;
        }
        if (e->size >= 0 && written != (uint64_t)e->size) {
            fail(error, errorSize, "The archive is damaged: %s is shorter than it says", shown(e->path, n, sizeof n));
            goto done;
        }
        if (close(out)) {
            out = -1;
            fail(error, errorSize, "%s couldn't be written (errno %d)", shown(e->path, n, sizeof n), errno);
            goto done;
        }
        out = -1;
    }
    if (index != list.count) {
        fail(error, errorSize, "The archive changed while it was unpacked");
        goto done;
    }
    if (progress)
        progress(unpacked, unpacked > total ? unpacked : total, "", data);
    result = 0;
done:
    if (out >= 0)
        close(out);
    if (folders.parentFd >= 0 && folders.parentFd != folders.root)
        close(folders.parentFd);
    if (folders.root >= 0)
        close(folders.root);
    if (a)
        archive_read_free(a);
    free(buffer);
    freeEntries(&list);
    ps5archive_free_list(volumes);
    return result;
}
