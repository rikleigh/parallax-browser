/* ps5zip.c: unpacking a release zip; see ps5zip.h. */
#include "ps5zip.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define CHUNK (256 * 1024)
#define CD_LIMIT (64u * 1024 * 1024)

typedef struct {
    const char* name; /* into the central directory; not NUL-terminated */
    unsigned nameLength;
    unsigned method;
    uint32_t crc;
    uint64_t compressed;
    uint64_t size;
    uint64_t offset;
    int directory;
} Entry;

static void fail(char* error, size_t size, const char* format, ...)
{
    if (!error || !size)
        return;
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
}

static unsigned u16(const unsigned char* p)
{
    return p[0] | (unsigned)p[1] << 8;
}

static uint32_t u32(const unsigned char* p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int readAt(int fd, void* buffer, size_t length, uint64_t offset)
{
    size_t done = 0;
    while (done < length) {
        ssize_t n = pread(fd, (char*)buffer + done, length - done, (off_t)(offset + done));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

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

/* A relative path of plain parts: no leading '/', no "", "." or ".." part,
 * no backslash or control character. A trailing '/' (a folder) is allowed. */
static int safeName(const char* name, unsigned length)
{
    if (!length || length > 1024 || name[0] == '/')
        return 0;
    unsigned start = 0;
    for (unsigned i = 0; i <= length; i++) {
        if (i < length) {
            unsigned char c = (unsigned char)name[i];
            if (c < 0x20 || c == 0x7f || c == '\\' || c == ':')
                return 0;
            if (c != '/')
                continue;
        }
        unsigned part = i - start;
        if (i == length && part == 0 && length && name[length - 1] == '/')
            break; /* the folder's trailing slash */
        if (part == 0 || (part == 1 && name[start] == '.') || (part == 2 && name[start] == '.' && name[start + 1] == '.'))
            return 0;
        start = i + 1;
    }
    return 1;
}

static int compareEntries(const void* a, const void* b)
{
    const Entry* x = a;
    const Entry* y = b;
    unsigned n = x->nameLength < y->nameLength ? x->nameLength : y->nameLength;
    int c = memcmp(x->name, y->name, n);
    return c ? c : (int)x->nameLength - (int)y->nameLength;
}

/* mkdir -p for the parts of relative (under dest), 0777 each. */
static int makeFolders(const char* dest, const char* relative, int includeLast)
{
    char path[2048];
    int n = snprintf(path, sizeof path, "%s/%s", dest, relative);
    if (n < 0 || (size_t)n >= sizeof path)
        return -1;
    size_t base = strlen(dest) + 1;
    size_t end = strlen(path);
    if (!includeLast) {
        char* slash = strrchr(path + base, '/');
        if (!slash)
            return 0;
        end = (size_t)(slash - path);
    }
    for (size_t i = base; i <= end; i++) {
        if (i < end && path[i] != '/')
            continue;
        char saved = path[i];
        path[i] = 0;
        if (path[base] && mkdir(path, 0777) && errno != EEXIST) {
            path[i] = saved;
            return -1;
        }
        struct stat st;
        if (path[base] && (lstat(path, &st) || !S_ISDIR(st.st_mode))) {
            path[i] = saved;
            return -1;
        }
        if (path[base])
            chmod(path, 0777);
        path[i] = saved;
    }
    return 0;
}

static int extractOne(int fd, const Entry* e, uint64_t dataEnd, const char* dest, const char* relative, unsigned char* in,
    unsigned char* out, uint64_t* done, uint64_t total, ps5zip_progress_fn progress, void* data, char* error, size_t errorSize)
{
    unsigned char local[30];
    if (readAt(fd, local, sizeof local, e->offset) || u32(local) != 0x04034b50) {
        fail(error, errorSize, "%s: no local header", relative);
        return -1;
    }
    uint64_t start = e->offset + 30 + u16(local + 26) + u16(local + 28);
    if (start + e->compressed > dataEnd || start + e->compressed < start) {
        fail(error, errorSize, "%s: its data runs past the archive's files", relative);
        return -1;
    }
    if (makeFolders(dest, relative, 0)) {
        fail(error, errorSize, "%s: its folder couldn't be made (errno %d)", relative, errno);
        return -1;
    }
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", dest, relative);
    int outFd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0777);
    if (outFd < 0) {
        fail(error, errorSize, "%s couldn't be created (errno %d)", relative, errno);
        return -1;
    }
    fchmod(outFd, 0777);
    uLong crc = crc32(0, Z_NULL, 0);
    uint64_t written = 0, read = 0;
    int ok = 1;
    z_stream z;
    memset(&z, 0, sizeof z);
    if (e->method == 8 && inflateInit2(&z, -MAX_WBITS) != Z_OK) {
        close(outFd);
        fail(error, errorSize, "%s: inflate couldn't start", relative);
        return -1;
    }
    int ended = 0;
    while (ok && !ended) {
        size_t want = e->compressed - read > CHUNK ? CHUNK : (size_t)(e->compressed - read);
        if (want && readAt(fd, in, want, start + read)) {
            ok = 0;
            fail(error, errorSize, "%s: read failed", relative);
            break;
        }
        read += want;
        if (e->method == 0) {
            if (writeAll(outFd, in, want)) {
                ok = 0;
                fail(error, errorSize, "%s: write failed (errno %d)", relative, errno);
                break;
            }
            crc = crc32(crc, in, (uInt)want);
            written += want;
            ended = read == e->compressed;
        } else {
            z.next_in = in;
            z.avail_in = (uInt)want;
            do {
                z.next_out = out;
                z.avail_out = CHUNK;
                int r = inflate(&z, Z_NO_FLUSH);
                if (r == Z_BUF_ERROR && !z.avail_in)
                    r = Z_OK; /* it needs more input */
                if (r != Z_OK && r != Z_STREAM_END) {
                    ok = 0;
                    fail(error, errorSize, "%s: bad deflate data (%d)", relative, r);
                    break;
                }
                size_t got = CHUNK - z.avail_out;
                if (written + got > e->size) {
                    ok = 0;
                    fail(error, errorSize, "%s: bigger than the archive says", relative);
                    break;
                }
                if (got && writeAll(outFd, out, got)) {
                    ok = 0;
                    fail(error, errorSize, "%s: write failed (errno %d)", relative, errno);
                    break;
                }
                crc = crc32(crc, out, (uInt)got);
                written += got;
                if (r == Z_STREAM_END) {
                    ended = 1;
                    break;
                }
                if (!got && !z.avail_in && read == e->compressed) {
                    ok = 0;
                    fail(error, errorSize, "%s: deflate data ends early", relative);
                    break;
                }
            } while (z.avail_out == 0 || z.avail_in);
        }
        if (progress && want)
            progress(*done + written, total, data);
    }
    if (e->method == 8)
        inflateEnd(&z);
    if (close(outFd) && ok) {
        ok = 0;
        fail(error, errorSize, "%s: close failed (errno %d)", relative, errno);
    }
    if (ok && (written != e->size || crc != e->crc)) {
        ok = 0;
        fail(error, errorSize, "%s: size or CRC doesn't match", relative);
    }
    *done += written;
    return ok ? 0 : -1;
}

int ps5zip_extract(const char* archive, const char* prefix, const char* dest, unsigned maxEntries, uint64_t maxBytes,
    ps5zip_progress_fn progress, void* data, char* error, size_t errorSize)
{
    int fd = open(archive, O_RDONLY);
    if (fd < 0) {
        fail(error, errorSize, "the archive couldn't be opened (errno %d)", errno);
        return -1;
    }
    struct stat st;
    unsigned char* cd = NULL;
    Entry* entries = NULL;
    unsigned char* in = NULL;
    unsigned char* out = NULL;
    int result = -1;
    if (fstat(fd, &st) || st.st_size < 22) {
        fail(error, errorSize, "not a zip archive");
        goto done;
    }
    /* The end of central directory record: in the last 64 KB + 22. */
    uint64_t size = (uint64_t)st.st_size;
    size_t tailLength = size < 65557 ? (size_t)size : 65557;
    unsigned char* tail = malloc(tailLength);
    if (!tail || readAt(fd, tail, tailLength, size - tailLength)) {
        free(tail);
        fail(error, errorSize, "the archive couldn't be read");
        goto done;
    }
    long eocd = -1;
    for (long i = (long)tailLength - 22; i >= 0; i--) {
        if (u32(tail + i) == 0x06054b50 && (size_t)i + 22 + u16(tail + i + 20) == tailLength) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        free(tail);
        fail(error, errorSize, "not a zip archive (no end record)");
        goto done;
    }
    unsigned count = u16(tail + eocd + 10);
    uint32_t cdSize = u32(tail + eocd + 12), cdOffset = u32(tail + eocd + 16);
    int multiDisk = u16(tail + eocd + 4) || u16(tail + eocd + 6) || u16(tail + eocd + 8) != count;
    free(tail);
    if (multiDisk || count == 0xffff || cdSize == 0xffffffffu || cdOffset == 0xffffffffu) {
        fail(error, errorSize, "split or zip64 archives aren't supported");
        goto done;
    }
    if (!count || count > maxEntries) {
        fail(error, errorSize, "the archive has %u entries (at most %u)", count, maxEntries);
        goto done;
    }
    if (cdSize > CD_LIMIT || (uint64_t)cdOffset + cdSize > size) {
        fail(error, errorSize, "the archive's directory is damaged");
        goto done;
    }
    cd = malloc(cdSize ? cdSize : 1);
    entries = calloc(count, sizeof *entries);
    if (!cd || !entries || readAt(fd, cd, cdSize, cdOffset)) {
        fail(error, errorSize, "the archive's directory couldn't be read");
        goto done;
    }
    size_t prefixLength = strlen(prefix);
    uint64_t total = 0;
    size_t p = 0;
    for (unsigned i = 0; i < count; i++) {
        if (p + 46 > cdSize || u32(cd + p) != 0x02014b50) {
            fail(error, errorSize, "the archive's directory is damaged (entry %u)", i);
            goto done;
        }
        const unsigned char* h = cd + p;
        unsigned nameLength = u16(h + 28), extraLength = u16(h + 30), commentLength = u16(h + 32);
        if (p + 46 + nameLength + extraLength + commentLength > cdSize) {
            fail(error, errorSize, "the archive's directory is damaged (entry %u)", i);
            goto done;
        }
        Entry* e = &entries[i];
        e->name = (const char*)h + 46;
        e->nameLength = nameLength;
        unsigned madeBy = u16(h + 4), flags = u16(h + 8);
        e->method = u16(h + 10);
        e->crc = u32(h + 16);
        e->compressed = u32(h + 20);
        e->size = u32(h + 24);
        e->offset = u32(h + 42);
        uint32_t external = u32(h + 38);
        e->directory = nameLength && e->name[nameLength - 1] == '/';
        p += 46 + nameLength + extraLength + commentLength;
        char shown[160];
        snprintf(shown, sizeof shown, "%.*s", (int)(nameLength < 120 ? nameLength : 120), e->name);
        if (flags & 0x41) {
            fail(error, errorSize, "%s is encrypted", shown);
            goto done;
        }
        if (e->method != 0 && e->method != 8) {
            fail(error, errorSize, "%s uses compression method %u", shown, e->method);
            goto done;
        }
        if (e->compressed == 0xffffffffu || e->size == 0xffffffffu || e->offset == 0xffffffffu) {
            fail(error, errorSize, "%s needs zip64", shown);
            goto done;
        }
        if (!safeName(e->name, nameLength)) {
            fail(error, errorSize, "%s isn't a safe path", shown);
            goto done;
        }
        if (nameLength < prefixLength || memcmp(e->name, prefix, prefixLength)) {
            fail(error, errorSize, "%s isn't inside %s", shown, prefix);
            goto done;
        }
        /* Unix attributes: only plain files and folders. */
        if ((madeBy >> 8) == 3) {
            unsigned type = (external >> 16) & 0170000;
            if (type && type != 0100000 && type != 0040000) {
                fail(error, errorSize, "%s is a link or a special file", shown);
                goto done;
            }
            if ((type == 0040000) != e->directory && type) {
                fail(error, errorSize, "%s: its type and name disagree", shown);
                goto done;
            }
        }
        if (e->directory && (e->size || e->compressed)) {
            fail(error, errorSize, "%s: a folder with data", shown);
            goto done;
        }
        if (e->method == 0 && e->compressed != e->size) {
            fail(error, errorSize, "%s: stored with two sizes", shown);
            goto done;
        }
        if (e->offset >= cdOffset) {
            fail(error, errorSize, "%s: its data is past the directory", shown);
            goto done;
        }
        total += e->size;
        if (total > maxBytes) {
            fail(error, errorSize, "the archive unpacks to more than %llu MB", (unsigned long long)(maxBytes >> 20));
            goto done;
        }
    }
    /* Duplicates (a folder and its trailing-slash twin count once each). */
    Entry* sorted = malloc(count * sizeof *sorted);
    if (!sorted) {
        fail(error, errorSize, "out of memory");
        goto done;
    }
    memcpy(sorted, entries, count * sizeof *sorted);
    qsort(sorted, count, sizeof *sorted, compareEntries);
    for (unsigned i = 1; i < count; i++) {
        if (!compareEntries(&sorted[i - 1], &sorted[i])) {
            fail(error, errorSize, "%.*s is in the archive twice", (int)(sorted[i].nameLength < 120 ? sorted[i].nameLength : 120), sorted[i].name);
            free(sorted);
            goto done;
        }
    }
    free(sorted);

    in = malloc(CHUNK);
    out = malloc(CHUNK);
    if (!in || !out) {
        fail(error, errorSize, "out of memory");
        goto done;
    }
    uint64_t written = 0;
    for (unsigned i = 0; i < count; i++) {
        const Entry* e = &entries[i];
        char relative[1100];
        snprintf(relative, sizeof relative, "%.*s", (int)(e->nameLength - prefixLength), e->name + prefixLength);
        size_t length = strlen(relative);
        if (length && relative[length - 1] == '/')
            relative[--length] = 0;
        if (!length)
            continue; /* the prefix folder itself */
        if (e->directory) {
            if (makeFolders(dest, relative, 1)) {
                fail(error, errorSize, "%s: the folder couldn't be made (errno %d)", relative, errno);
                goto done;
            }
            continue;
        }
        if (extractOne(fd, e, cdOffset, dest, relative, in, out, &written, total, progress, data, error, errorSize))
            goto done;
    }
    result = 0;
done:
    free(in);
    free(out);
    free(entries);
    free(cd);
    close(fd);
    return result;
}

int ps5zip_remove_tree(const char* path)
{
    struct stat st;
    if (lstat(path, &st))
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    DIR* dir = opendir(path);
    if (!dir)
        return -1;
    int result = 0;
    struct dirent* d;
    while ((d = readdir(dir))) {
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
            continue;
        char child[2048];
        int n = snprintf(child, sizeof child, "%s/%s", path, d->d_name);
        if (n < 0 || (size_t)n >= sizeof child || ps5zip_remove_tree(child))
            result = -1;
    }
    closedir(dir);
    if (rmdir(path))
        result = -1;
    return result;
}
