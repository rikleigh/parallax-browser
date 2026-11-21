/*
 * ps5files.c: moving and copying between drives; see ps5files.h.
 */
#include "ps5files.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define COPY_BLOCK (4u * 1024 * 1024)
#define MAX_ENTRIES 1000000u
#define PATH_BYTES 1200
#define DEFAULT_MARGIN (64ull << 20)
#define PROGRESS_NS 100000000ll

typedef struct {
    uint64_t done, total;
    unsigned entries;
    dev_t device;      /* src's drive: a folder on another one inside it is refused */
    int sync;          /* fsync each file (a move: the source goes after) */
    int madeTop;       /* this call made <dest>.part: only then is it ours to delete */
    ps5files_progress_fn progress;
    void* data;
    volatile int* cancel;
    int64_t lastProgress;
    char* error;
    size_t errorSize;
    char* buffer;
} Copy;

static void fail(char* error, size_t size, const char* format, ...)
{
    if (!error || !size)
        return;
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
}

static int64_t nowNs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec;
}

static const char* baseName(const char* path)
{
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int join(char* out, const char* a, const char* b)
{
    return (size_t)snprintf(out, PATH_BYTES, "%s/%s", a, b) >= PATH_BYTES ? -1 : 0;
}

int ps5files_remove_tree(const char* path)
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
        char child[PATH_BYTES];
        if (join(child, path, entry->d_name)) {
            r = -1;
            continue;
        }
        if (ps5files_remove_tree(child))
            r = -1;
    }
    closedir(folder);
    if (rmdir(path))
        r = -1;
    return r;
}

/* Counts src's bytes; refuses links and special files. 0 or -1. */
static int measure(Copy* c, const char* path)
{
    struct stat st;
    if (c->cancel && *c->cancel)
        return -3;
    if (lstat(path, &st)) {
        fail(c->error, c->errorSize, "Couldn't read %s (errno %d)", baseName(path), errno);
        return -1;
    }
    if (!c->entries)
        c->device = st.st_dev;
    else if (st.st_dev != c->device) {
        fail(c->error, c->errorSize, "%s is another drive", baseName(path));
        return -1;
    }
    if (++c->entries > MAX_ENTRIES) {
        fail(c->error, c->errorSize, "More than %u files", MAX_ENTRIES);
        return -1;
    }
    if (S_ISREG(st.st_mode)) {
        c->total += (uint64_t)st.st_size;
        return 0;
    }
    if (S_ISLNK(st.st_mode)) {
        fail(c->error, c->errorSize, "Links aren't moved or copied: %s", baseName(path));
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fail(c->error, c->errorSize, "Special files aren't moved or copied: %s", baseName(path));
        return -1;
    }
    DIR* folder = opendir(path);
    if (!folder) {
        fail(c->error, c->errorSize, "Couldn't open %s (errno %d)", baseName(path), errno);
        return -1;
    }
    int r = 0;
    struct dirent* entry;
    while (!r && (entry = readdir(folder))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char child[PATH_BYTES];
        if (join(child, path, entry->d_name)) {
            fail(c->error, c->errorSize, "A path is too long: %s", entry->d_name);
            r = -1;
        } else
            r = measure(c, child);
    }
    closedir(folder);
    return r;
}

static void report(Copy* c, const char* current, int force)
{
    if (!c->progress)
        return;
    int64_t now = nowNs();
    if (!force && now - c->lastProgress < PROGRESS_NS)
        return;
    c->lastProgress = now;
    c->progress(c->done, c->total, current, c->data);
}

static int copyFile(Copy* c, const char* from, const char* to, int top)
{
    int in = open(from, O_RDONLY | O_NOFOLLOW);
    if (in < 0) {
        fail(c->error, c->errorSize, "Couldn't read %s (errno %d)", baseName(from), errno);
        return -2;
    }
    int out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0777);
    if (out < 0) {
        fail(c->error, c->errorSize, "Couldn't write %s (errno %d)", baseName(to), errno);
        close(in);
        return -2;
    }
    if (top)
        c->madeTop = 1;
    fchmod(out, 0777);
    int r = 0;
    for (;;) {
        if (c->cancel && *c->cancel) {
            r = -3;
            break;
        }
        ssize_t n = read(in, c->buffer, COPY_BLOCK);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            fail(c->error, c->errorSize, "Couldn't read %s (errno %d)", baseName(from), errno);
            r = -2;
            break;
        }
        if (n == 0)
            break;
        for (ssize_t written = 0; written < n;) {
            ssize_t w = write(out, c->buffer + written, (size_t)(n - written));
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0) {
                fail(c->error, c->errorSize, "Couldn't write %s (errno %d)", baseName(to), w < 0 ? errno : ENOSPC);
                r = -2;
                break;
            }
            written += w;
        }
        if (r)
            break;
        c->done += (uint64_t)n;
        report(c, baseName(from), 0);
    }
    close(in);
    if (!r && c->sync && fsync(out)) {
        fail(c->error, c->errorSize, "Couldn't write %s (errno %d)", baseName(to), errno);
        r = -2;
    }
    if (close(out) && !r) {
        fail(c->error, c->errorSize, "Couldn't write %s (errno %d)", baseName(to), errno);
        r = -2;
    }
    if (!r)
        report(c, baseName(from), 1); /* each file's end, whatever the time */
    return r;
}

static int copyTree(Copy* c, const char* from, const char* to, int top)
{
    struct stat st;
    if (lstat(from, &st)) {
        fail(c->error, c->errorSize, "Couldn't read %s (errno %d)", baseName(from), errno);
        return -2;
    }
    if (S_ISREG(st.st_mode))
        return copyFile(c, from, to, top);
    if (!S_ISDIR(st.st_mode)) {
        /* it changed since it was listed */
        fail(c->error, c->errorSize, "%s changed while it was copied", baseName(from));
        return -2;
    }
    if (mkdir(to, 0777)) {
        fail(c->error, c->errorSize, "Couldn't make the folder %s (errno %d)", baseName(to), errno);
        return -2;
    }
    if (top)
        c->madeTop = 1;
    chmod(to, 0777);
    DIR* folder = opendir(from);
    if (!folder) {
        fail(c->error, c->errorSize, "Couldn't open %s (errno %d)", baseName(from), errno);
        return -2;
    }
    int r = 0;
    struct dirent* entry;
    while (!r && (entry = readdir(folder))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char a[PATH_BYTES], b[PATH_BYTES];
        if (join(a, from, entry->d_name) || join(b, to, entry->d_name)) {
            fail(c->error, c->errorSize, "A path is too long: %s", entry->d_name);
            r = -2;
        } else
            r = copyTree(c, a, b, 0);
    }
    closedir(folder);
    return r;
}

/* Whether everything in src is in copy, the same kind and size (a move
 * deletes src only then: something written into it during the copy stays). */
static int sameTree(const char* src, const char* copy)
{
    struct stat a, b;
    if (lstat(src, &a) || lstat(copy, &b))
        return 0;
    if (S_ISREG(a.st_mode))
        return S_ISREG(b.st_mode) && a.st_size == b.st_size;
    if (!S_ISDIR(a.st_mode) || !S_ISDIR(b.st_mode))
        return 0;
    DIR* folder = opendir(src);
    if (!folder)
        return 0;
    int same = 1;
    struct dirent* entry;
    while (same && (entry = readdir(folder))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char x[PATH_BYTES], y[PATH_BYTES];
        same = !join(x, src, entry->d_name) && !join(y, copy, entry->d_name) && sameTree(x, y);
    }
    closedir(folder);
    return same;
}

/* Whether dest's folder is src or inside it, by identity (another spelling
 * of the name on a drive that ignores case is the same folder). */
static int insideItself(const char* src, const char* dest)
{
    struct stat s;
    if (stat(src, &s) || !S_ISDIR(s.st_mode))
        return 0;
    char path[PATH_BYTES];
    snprintf(path, sizeof path, "%s", dest);
    for (;;) {
        char* slash = strrchr(path, '/');
        if (!slash || slash == path)
            return 0;
        *slash = 0;
        struct stat p;
        if (!stat(path, &p) && p.st_dev == s.st_dev && p.st_ino == s.st_ino)
            return 1;
    }
}

/* Trailing slashes off, into out. */
static void trimmed(const char* path, char* out, size_t size)
{
    snprintf(out, size, "%s", path);
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/')
        out[--n] = 0;
}

int ps5files_transfer(const char* srcPath, const char* destPath, bool move, uint64_t freeMargin, ps5files_progress_fn progress,
    void* data, volatile int* cancel, char* error, size_t errorSize)
{
    if (error && errorSize)
        error[0] = 0;
    char src[PATH_BYTES], dest[PATH_BYTES], part[PATH_BYTES + 8];
    if (!srcPath || !destPath || strlen(srcPath) >= sizeof src - 8 || strlen(destPath) >= sizeof dest - 8) {
        fail(error, errorSize, "No source or destination");
        return -1;
    }
    trimmed(srcPath, src, sizeof src);
    trimmed(destPath, dest, sizeof dest);
    size_t srcLength = strlen(src);
    if (!strcmp(src, dest) || (!strncmp(dest, src, srcLength) && dest[srcLength] == '/')) {
        fail(error, errorSize, "It can't go into itself");
        return -1;
    }
    struct stat st;
    if (lstat(src, &st)) {
        fail(error, errorSize, "%s isn't there", baseName(src));
        return -1;
    }
    if (insideItself(src, dest)) {
        fail(error, errorSize, "It can't go into itself");
        return -1;
    }
    if (!lstat(dest, &st)) {
        fail(error, errorSize, "There is already something called %s there", baseName(dest));
        return -1;
    }
    snprintf(part, sizeof part, "%s.part", dest);
    if (!lstat(part, &st)) {
        fail(error, errorSize, "There is already something called %s there", baseName(part));
        return -1;
    }
    if (move) {
        if (!rename(src, dest))
            return 0;
        if (errno != EXDEV) {
            fail(error, errorSize, "Couldn't move it (errno %d)", errno);
            return -2;
        }
    }

    Copy c = { 0 };
    c.sync = move;
    c.progress = progress;
    c.data = data;
    c.cancel = cancel;
    c.error = error;
    c.errorSize = errorSize;
    int measured = measure(&c, src);
    if (measured == -3) {
        fail(error, errorSize, "Cancelled");
        return -3;
    }
    if (measured)
        return -1;
    char folder[PATH_BYTES];
    snprintf(folder, sizeof folder, "%s", dest);
    char* slash = strrchr(folder, '/');
    if (slash && slash != folder)
        *slash = 0;
    else if (slash)
        slash[1] = 0;
    struct statvfs fs;
    uint64_t margin = freeMargin ? freeMargin : DEFAULT_MARGIN;
    if (!statvfs(folder, &fs) && (uint64_t)fs.f_bavail * (uint64_t)fs.f_frsize < c.total + margin) {
        fail(error, errorSize, "Not enough space there: %llu MB needed, %llu MB free", (unsigned long long)((c.total + margin) >> 20),
            (unsigned long long)(((uint64_t)fs.f_bavail * (uint64_t)fs.f_frsize) >> 20));
        return -1;
    }
    c.buffer = malloc(COPY_BLOCK);
    if (!c.buffer) {
        fail(error, errorSize, "Out of memory");
        return -2;
    }
    report(&c, "", 1);
    int r = copyTree(&c, src, part, 1);
    free(c.buffer);
    if (r == -3)
        fail(error, errorSize, "Cancelled");
    /* something called dest that appeared meanwhile isn't replaced */
    if (!r && !lstat(dest, &st)) {
        fail(error, errorSize, "Something called %s appeared there meanwhile", baseName(dest));
        r = -1;
    }
    if (!r && rename(part, dest)) {
        fail(error, errorSize, "Couldn't finish %s (errno %d)", baseName(dest), errno);
        r = -2;
    }
    if (r) {
        /* only a .part this call made (another's of the same name stays) */
        if (c.madeTop)
            ps5files_remove_tree(part);
        return r;
    }
    report(&c, "", 1);
    if (move) {
        if (!sameTree(src, dest))
            fail(error, errorSize, "Copied, but the original changed meanwhile: it was kept");
        else if (ps5files_remove_tree(src))
            fail(error, errorSize, "Copied, but the original couldn't all be deleted");
    }
    return 0;
}
