/* ps5ufs2.c: a folder as one UFS2 image file (ShadowMountPlus's .ffpkg),
 * and reading one back; see ps5ufs2.h.
 *
 * The writer is ported from PS5 Dump Forge (https://github.com/quer3q/ps5-dump-forge,
 * crates/ps5-dump-forge-ufs2: lib.rs, geom.rs, ondisk.rs, bmap.rs, tree.rs),
 * Copyright (c) the PS5 Dump Forge authors, GPL-3.0 (this project is
 * GPL-3.0 too; THIRD-PARTY-NOTICES.md). Its geometry arithmetic is in turn
 * derived from FreeBSD sbin/newfs/mkfs.c:
 *   Copyright (c) 2002 Networks Associates Technology, Inc. All rights reserved.
 *   Copyright (c) 1980, 1989, 1993 The Regents of the University of California.
 *   (BSD-3-Clause; see the FreeBSD source for the full notice.)
 * Field offsets are those of FreeBSD struct fs, struct cg (sys/ufs/ffs/fs.h)
 * and struct ufs2_dinode (sys/ufs/ufs/dinode.h).
 *
 * What it writes (dump-forge's layout, unchanged, so the same folder gives
 * the same bytes as dump-forge's writer):
 *   - newfs -O 2 -b 65536 -f 65536 -m 0 -S 4096 -i <density>: one fragment
 *     per block, no soft updates, no journal, no check hashes. The density
 *     is SMP's recommended -i: the image's data over (entries + 2048),
 *     rounded down to 4 KiB, clamped to 64..256 KiB.
 *   - The raw filesystem from byte 0 (no partition table); the primary
 *     superblock at 64 KiB.
 *   - Inodes numbered depth first from the root (inode 2), each directory's
 *     children sorted by their names' bytes.
 *   - Every file and directory one contiguous run of blocks, with its
 *     indirect blocks in front of the blocks they map (ffs_balloc's order),
 *     in inode order from the first data block.
 *   - 1024 free blocks (64 MiB) and 2048 free inodes kept for writable
 *     (image_rw=) mounts; the image is the smallest newfs size that holds
 *     the tree and those.
 *   - Fixed times (1000000000), so the same tree gives the same image.
 * Everything is computed before the first byte is written, and the image
 * is written front to back: each cylinder group's metadata just before the
 * first data block after it.
 *
 * The reader (verify, ps5ufs2_read_file) is written from the on-disk
 * format, not from the writer's layout: block and fragment sizes, inode
 * places and block pointers from the superblock and the inodes.
 */
#include "ps5ufs2.h"

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

/* ---------------------------------------------------------------- geometry (geom.rs) */

#define BSIZE 65536ull
#define SBLKNO 2ull
#define CBLKNO 3ull
#define IBLKNO 4ull
#define INOPB (BSIZE / 256)
#define NINDIR (BSIZE / 8)
#define CONTIGSUMSIZE 16ull
#define CG_HDR 168ull
#define CGSIZEFUDGE 8ull
#define MINCYLGRPS 4ull
#define CSUM_SIZE 16ull

#define FS_UFS2_MAGIC 0x19540119u
#define CG_MAGIC 0x00090255u
#define SBLOCK_UFS2 65536ull
#define SBLOCKSIZE 8192u
#define VOLNAME_AT 680u
#define VOLNAME_LEN 32u
#define FS_FLAGS_UPDATED 0x80u
#define FS_OPTSPACE 1
#define FIXED_TIME 1000000000ll

#define IFMT 0170000u
#define IFDIR 0040000u
#define IFREG 0100000u

#define ROOTINO 2ull
#define SPARE_INODES 2048ull
#define SPARE_BLOCKS 1024ull
#define NDADDR 12ull
#define NIADDR 3
#define UFS_LINK_MAX 65500ull
#define UFS_MAXNAMLEN 255u
#define UFS_MAXPATHLEN 1023u
#define MAXDIRSIZE 0x7fffffffull
#define UFS_DIRBLKSIZ 512u
#define DT_DIR 4
#define DT_REG 8

#define CHUNK (8ull * 1024 * 1024) /* file data is read and written in runs of at most this */
#define OUT_BUFFER (1u << 20)
#define DEFAULT_MARGIN (64ull << 20)

typedef struct {
    uint64_t size; /* fs_size, in blocks (= fragments) */
    uint64_t density;
    uint64_t fpg, ipg, ncg;
    uint64_t dblkno;   /* first data block of every group */
    uint64_t csblocks; /* blocks of the summary area at the start of group 0's data */
} Geom;

static uint64_t divCeil(uint64_t a, uint64_t b)
{
    return a / b + (a % b != 0);
}

static uint64_t roundUp(uint64_t a, uint64_t b)
{
    return divCeil(a, b) * b;
}

/* CGSIZE(fs) for UFS2 with cluster maps. */
static uint64_t cgsize(uint64_t fpg, uint64_t ipg)
{
    return CG_HDR + divCeil(ipg, 8) + divCeil(fpg, 8) + 4 + CONTIGSUMSIZE * 4 + divCeil(fpg, 8);
}

static uint64_t ipgFor(uint64_t fpg, uint64_t fpi)
{
    return roundUp(divCeil(fpg, fpi), INOPB);
}

/* newfs's choice of fpg/ipg for size blocks at density bytes an inode.
 * 0, or -1 when no layout of that size exists. */
static int geomNew(Geom* g, uint64_t size, uint64_t density)
{
    if (size < 8)
        return -1;
    uint64_t fpi = density / BSIZE ? density / BSIZE : 1;
    const uint64_t maxinum = (1ull << 32) - INOPB;
    if (fpi < 1 + size / maxinum)
        return -1;
    uint64_t minfpg = fpi * INOPB < size ? fpi * INOPB : size;
    uint64_t ipg = INOPB;
    uint64_t fpg = IBLKNO + ipg / INOPB > minfpg ? IBLKNO + ipg / INOPB : minfpg;
    ipg = ipgFor(fpg, fpi);
    fpg = IBLKNO + ipg / INOPB > minfpg ? IBLKNO + ipg / INOPB : minfpg;
    ipg = ipgFor(fpg, fpi);
    if (cgsize(fpg, ipg) >= BSIZE - CGSIZEFUDGE)
        return -1;
    /* newfs grows the group a block at a time until its map fills a block
     * or there would be fewer than MINCYLGRPS groups: both stops found
     * directly (the map only grows with the group) */
    const uint64_t limit = BSIZE - CGSIZEFUDGE;
    uint64_t few = size / MINCYLGRPS + 1;
    if (few < fpg)
        few = fpg;
    uint64_t lo = fpg, hi = few;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (cgsize(mid, ipgFor(mid, fpi)) >= limit)
            hi = mid;
        else
            lo = mid + 1;
    }
    fpg = lo == few || cgsize(lo, ipgFor(lo, fpi)) == limit ? lo : lo - 1;
    ipg = ipgFor(fpg, fpi);
    /* newfs only demands that the last group hold its own metadata */
    uint64_t lastminfpg = IBLKNO + ipg / INOPB;
    for (;;) {
        uint64_t rem = size % fpg;
        if (size < lastminfpg)
            return -1;
        if (rem >= lastminfpg || rem == 0)
            break;
        fpg--;
        ipg = ipgFor(fpg, fpi);
    }
    uint64_t ncg = divCeil(size, fpg);
    if (ncg * ipg > maxinum)
        return -1;
    g->size = size;
    g->density = density;
    g->fpg = fpg;
    g->ipg = ipg;
    g->ncg = ncg;
    g->dblkno = IBLKNO + ipg / INOPB;
    g->csblocks = divCeil(ncg * CSUM_SIZE, BSIZE);
    return 0;
}

/* Blocks in group c (only the last one can be short). */
static uint64_t cgBlocks(const Geom* g, uint64_t c)
{
    uint64_t left = g->size - c * g->fpg;
    return left < g->fpg ? left : g->fpg;
}

/* dump-forge's rule on top of newfs's: every group, the last included,
 * holds its metadata and at least one data block, and group 0 the
 * summary area too. */
static int lastGroupOk(const Geom* g)
{
    return cgBlocks(g, 0) > g->dblkno + g->csblocks && cgBlocks(g, g->ncg - 1) > g->dblkno;
}

static uint64_t dataCapacity(const Geom* g)
{
    return g->size - g->ncg * g->dblkno - g->csblocks;
}

static uint64_t inodeCapacity(const Geom* g)
{
    return g->ncg * g->ipg;
}

/* Data blocks of group c this writer allocates from. */
static uint64_t cgData(const Geom* g, uint64_t c)
{
    return cgBlocks(g, c) - g->dblkno - (c ? 0 : g->csblocks);
}

static uint64_t cgDataStart(const Geom* g, uint64_t c)
{
    return c * g->fpg + g->dblkno + (c ? 0 : g->csblocks);
}

/* Physical block of the i-th allocatable block, and how many allocatable
 * blocks follow it contiguously in the same group (itself included). */
static uint64_t locate(const Geom* g, uint64_t i, uint64_t* contiguous)
{
    uint64_t d0 = cgData(g, 0);
    if (i < d0) {
        if (contiguous)
            *contiguous = d0 - i;
        return cgDataStart(g, 0) + i;
    }
    uint64_t full = g->fpg - g->dblkno;
    uint64_t rest = i - d0;
    uint64_t c = 1 + rest / full, off = rest % full;
    if (contiguous)
        *contiguous = cgData(g, c) - off;
    return cgDataStart(g, c) + off;
}

/* The allocatable-block range [*start, *end) in group c. */
static void cgDataRange(const Geom* g, uint64_t c, uint64_t* start, uint64_t* end)
{
    *start = c ? cgData(g, 0) + (c - 1) * (g->fpg - g->dblkno) : 0;
    *end = *start + cgData(g, c);
}

/* fs_dsize, as newfs computes it. */
static uint64_t dsize(const Geom* g)
{
    return g->size - SBLKNO - g->ncg * (g->dblkno - SBLKNO) - g->csblocks;
}

/* SMP's recommended -i: bytes the image holds over (entries + 2048),
 * rounded down to 4 KiB, 64..256 KiB. */
static uint64_t densityFor(uint64_t bytes, uint64_t entries)
{
    uint64_t d = bytes / (entries + 2048) / 4096 * 4096;
    return d < 64 * 1024 ? 64 * 1024 : d > 256 * 1024 ? 256 * 1024 : d;
}

/* ---------------------------------------------------------------- block maps (bmap.rs) */

/* Data blocks one indirect tree of level maps: NINDIR^level. */
static uint64_t span(unsigned level)
{
    uint64_t s = 1;
    while (level--)
        s *= NINDIR;
    return s;
}

/* Slots of a full tree of level (indirect blocks included). */
static uint64_t fullSlots(unsigned level)
{
    uint64_t s = 1;
    for (unsigned i = 0; i < level; i++)
        s = 1 + NINDIR * s;
    return s;
}

/* Slots of a tree of level mapping its first k data blocks. */
static uint64_t treeSlots(unsigned level, uint64_t k)
{
    if (!level)
        return 1;
    uint64_t per = span(level - 1);
    uint64_t full = k / per, rem = k % per;
    return 1 + full * fullSlots(level - 1) + (rem ? treeSlots(level - 1, rem) : 0);
}

static uint64_t maxBlocks(void)
{
    return NDADDR + span(1) + span(2) + span(3);
}

/* All slots (data + indirect blocks) of a file of n data blocks; 0 with
 * *ok false past maxBlocks. */
static uint64_t totalSlots(uint64_t n, int* ok)
{
    *ok = n <= maxBlocks();
    if (!*ok)
        return 0;
    uint64_t left = n, direct = n < NDADDR ? n : NDADDR, total = direct;
    left -= direct;
    for (unsigned level = 1; level <= NIADDR && left; level++) {
        uint64_t take = left < span(level) ? left : span(level);
        total += treeSlots(level, take);
        left -= take;
    }
    return total;
}

/* Slot of each top-level indirect block of a file of n data blocks
 * (UINT64_MAX: none). */
static void indirectRoots(uint64_t n, uint64_t out[NIADDR])
{
    uint64_t slot = NDADDR, covered = NDADDR;
    for (unsigned i = 0; i < NIADDR; i++) {
        out[i] = UINT64_MAX;
        if (n <= covered)
            continue;
        out[i] = slot;
        slot += fullSlots(i + 1);
        covered += span(i + 1);
    }
}

/* The slot sequence of a file, in disk order, as calls: data runs
 * (slot, lbn, count) and indirect blocks (slot, level, k). */
typedef struct {
    int (*data)(void* user, uint64_t slot, uint64_t lbn, uint64_t count);
    int (*indirect)(void* user, uint64_t slot, unsigned level, uint64_t k);
    void* user;
} SegWalk;

static int walkTree(const SegWalk* w, unsigned level, uint64_t k, uint64_t* slot, uint64_t* lbn)
{
    int r = w->indirect(w->user, *slot, level, k);
    if (r)
        return r;
    ++*slot;
    if (level == 1) {
        r = w->data(w->user, *slot, *lbn, k);
        *slot += k;
        *lbn += k;
        return r;
    }
    uint64_t per = span(level - 1);
    for (uint64_t left = k; left;) {
        uint64_t t = left < per ? left : per;
        if ((r = walkTree(w, level - 1, t, slot, lbn)))
            return r;
        left -= t;
    }
    return 0;
}

static int walkSegments(const SegWalk* w, uint64_t n)
{
    uint64_t direct = n < NDADDR ? n : NDADDR;
    int r;
    if (direct && (r = w->data(w->user, 0, 0, direct)))
        return r;
    uint64_t slot = direct, lbn = direct, left = n - direct;
    for (unsigned level = 1; level <= NIADDR && left; level++) {
        uint64_t take = left < span(level) ? left : span(level);
        if ((r = walkTree(w, level, take, &slot, &lbn)))
            return r;
        left -= take;
    }
    return 0;
}

/* ---------------------------------------------------------------- the tree (tree.rs) */

typedef struct {
    char* path;       /* relative to the source; "" for the root */
    const char* name; /* in path, after its last '/' */
    int dir;
    uint64_t size;    /* a file's bytes; a directory's (a multiple of UFS_DIRBLKSIZ) */
    size_t parent;
    size_t firstChild, childCount; /* in nodes, sorted by name bytes */
    uint32_t subdirs, depth;
    uint64_t ino;     /* depth-first order from ROOTINO */
    uint64_t start, slots; /* data index of slot 0; data + indirect blocks */
} Node;

typedef struct {
    Node* nodes;
    size_t count, capacity;
    size_t* order; /* node indices in inode order */
    Geom geom;
    uint64_t imageSize, fileBytes, usedBlocks;
    uint32_t fsId;
    unsigned char volname[VOLNAME_LEN];
    char* error;
    size_t errorSize;
    int failure; /* a failure that isn't the source's: -2, not -1 */
} Layout;

static void fail(Layout* l, int local, const char* format, ...)
{
    if (local)
        l->failure = 1;
    if (!l->error || !l->errorSize)
        return;
    va_list args;
    va_start(args, format);
    vsnprintf(l->error, l->errorSize, format, args);
    va_end(args);
}

static char* joinPath(const char* a, const char* b)
{
    size_t la = strlen(a), lb = strlen(b);
    char* path = malloc(la + lb + 2);
    if (!path)
        return NULL;
    memcpy(path, a, la);
    size_t at = la;
    if (la && lb)
        path[at++] = '/';
    memcpy(path + at, b, lb + 1);
    return path;
}

static Node* addNode(Layout* l)
{
    if (l->count == l->capacity) {
        size_t capacity = l->capacity ? l->capacity * 2 : 256;
        Node* grown = realloc(l->nodes, capacity * sizeof *grown);
        if (!grown)
            return NULL;
        l->nodes = grown;
        l->capacity = capacity;
    }
    Node* n = &l->nodes[l->count++];
    memset(n, 0, sizeof *n);
    return n;
}

static void freeLayout(Layout* l)
{
    for (size_t i = 0; i < l->count; i++)
        free(l->nodes[i].path);
    free(l->nodes);
    free(l->order);
    l->nodes = NULL;
    l->order = NULL;
    l->count = 0;
}

static int compareBytes(const void* a, const void* b)
{
    return strcmp(*(char* const*)a, *(char* const*)b); /* strcmp compares as unsigned char */
}

/* DIRSIZ: an entry's header (8 bytes), its name and a NUL, rounded up to 4. */
static unsigned dirsiz(size_t namelen)
{
    return (unsigned)((8 + namelen + 1 + 3) & ~(size_t)3);
}

/* One folder's entries as child nodes, sorted by name bytes. */
static int listFolder(Layout* l, const char* src, size_t index)
{
    char* folder = joinPath(src, l->nodes[index].path);
    if (!folder) {
        fail(l, 1, "out of memory");
        return -1;
    }
    DIR* dir = opendir(folder);
    if (!dir) {
        fail(l, 1, "%s couldn't be read (errno %d)", folder, errno);
        free(folder);
        return -1;
    }
    char** names = NULL;
    size_t count = 0, capacity = 0;
    int result = -1;
    struct dirent* entry;
    errno = 0;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 64;
            char** grown = realloc(names, capacity * sizeof *grown);
            if (!grown) {
                fail(l, 1, "out of memory");
                goto done;
            }
            names = grown;
        }
        if (!(names[count] = strdup(entry->d_name))) {
            fail(l, 1, "out of memory");
            goto done;
        }
        count++;
        errno = 0;
    }
    if (errno) {
        fail(l, 1, "%s couldn't be read (errno %d)", folder, errno);
        goto done;
    }
    if (count)
        qsort(names, count, sizeof *names, compareBytes);
    l->nodes[index].firstChild = l->count;
    l->nodes[index].childCount = count;
    uint32_t subdirs = 0;
    for (size_t i = 0; i < count; i++) {
        size_t nameLength = strlen(names[i]);
        if (nameLength > UFS_MAXNAMLEN) {
            fail(l, 0, "%s/%s: a name longer than 255 bytes", l->nodes[index].path, names[i]);
            goto done;
        }
        char* path = joinPath(l->nodes[index].path, names[i]);
        char* full = path ? joinPath(src, path) : NULL;
        Node* node = full ? addNode(l) : NULL;
        if (!node) {
            free(path);
            free(full);
            fail(l, 1, "out of memory");
            goto done;
        }
        node->path = path;
        node->name = path + strlen(path) - nameLength;
        node->parent = index;
        node->depth = l->nodes[index].depth + 1;
        struct stat st;
        int bad = lstat(full, &st);
        free(full);
        if (bad) {
            fail(l, 1, "%s couldn't be read (errno %d)", path, errno);
            goto done;
        }
        if (strlen(path) > UFS_MAXPATHLEN) {
            fail(l, 0, "%s: a path longer than %u bytes", path, UFS_MAXPATHLEN);
            goto done;
        }
        if (S_ISLNK(st.st_mode)) {
            fail(l, 0, "%s is a link", path);
            goto done;
        }
        if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
            fail(l, 0, "%s is a special file", path);
            goto done;
        }
        node->dir = S_ISDIR(st.st_mode);
        node->size = node->dir ? 0 : (uint64_t)st.st_size;
        subdirs += node->dir;
    }
    if (2ull + subdirs > UFS_LINK_MAX) {
        fail(l, 0, "%s holds %u folders; UFS allows %llu", index ? l->nodes[index].path : "the folder", subdirs,
            (unsigned long long)(UFS_LINK_MAX - 2));
        goto done;
    }
    l->nodes[index].subdirs = subdirs;
    result = 0;
done:
    for (size_t i = 0; i < count; i++)
        free(names[i]);
    free(names);
    closedir(dir);
    free(folder);
    return result;
}

/* The bytes a directory's entries take: ".", "..", then its children,
 * packed into UFS_DIRBLKSIZ chunks, none straddling one. */
static uint64_t directorySize(const Layout* l, size_t index)
{
    const Node* n = &l->nodes[index];
    uint64_t chunks = 1;
    unsigned used = 0;
    for (size_t i = 0; i < 2 + n->childCount; i++) {
        size_t length = i == 0 ? 1 : i == 1 ? 2 : strlen(l->nodes[n->firstChild + i - 2].name);
        unsigned need = dirsiz(length);
        if (used + need > UFS_DIRBLKSIZ) {
            chunks++;
            used = 0;
        }
        used += need;
    }
    return chunks * UFS_DIRBLKSIZ;
}

static void put16(unsigned char* p, uint16_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char* p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static void put64(unsigned char* p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

/* A directory's bytes (directorySize long, zeroed by the caller): the
 * last entry of each chunk takes the chunk's leftover space. */
static void directoryBytes(const Layout* l, size_t index, unsigned char* out)
{
    const Node* n = &l->nodes[index];
    size_t chunk = 0, used = 0, last = SIZE_MAX;
    for (size_t i = 0; i < 2 + n->childCount; i++) {
        const char* name;
        uint64_t ino;
        unsigned char type = DT_DIR;
        if (i == 0) {
            name = ".";
            ino = n->ino;
        } else if (i == 1) {
            name = "..";
            ino = l->nodes[n->parent].ino;
        } else {
            const Node* child = &l->nodes[n->firstChild + i - 2];
            name = child->name;
            ino = child->ino;
            type = child->dir ? DT_DIR : DT_REG;
        }
        size_t length = strlen(name);
        unsigned need = dirsiz(length);
        if (used + need > UFS_DIRBLKSIZ) {
            if (last != SIZE_MAX)
                put16(out + last + 4, (uint16_t)(chunk + UFS_DIRBLKSIZ - last));
            chunk += UFS_DIRBLKSIZ;
            used = 0;
        }
        size_t at = chunk + used;
        put32(out + at, (uint32_t)ino);
        put16(out + at + 4, (uint16_t)need);
        out[at + 6] = type;
        out[at + 7] = (unsigned char)length;
        memcpy(out + at + 8, name, length);
        used += need;
        last = at;
    }
    if (last != SIZE_MAX)
        put16(out + last + 4, (uint16_t)(chunk + UFS_DIRBLKSIZ - last));
}

/* The folder breadth first (each folder's children next to each other),
 * then numbered depth first: the inode order, which is also the order
 * the files' blocks go in. */
static int listTree(Layout* l, const char* src)
{
    struct stat st;
    if (stat(src, &st)) {
        fail(l, errno != ENOENT && errno != ENOTDIR, "%s couldn't be read (errno %d)", src, errno);
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fail(l, 0, "%s isn't a folder", src);
        return -1;
    }
    Node* root = addNode(l);
    if (!root || !(root->path = strdup(""))) {
        fail(l, 1, "out of memory");
        return -1;
    }
    root->name = root->path;
    root->dir = 1;
    for (size_t i = 0; i < l->count; i++) {
        if (l->nodes[i].dir && listFolder(l, src, i))
            return -1;
    }
    if (l->count > (1ull << 32) - ROOTINO - SPARE_INODES) {
        fail(l, 0, "too many files for a UFS2 image");
        return -1;
    }
    /* depth first, children in name order: a stack of node indices */
    l->order = malloc(l->count * sizeof *l->order);
    size_t* stack = malloc(l->count * sizeof *stack);
    if (!l->order || !stack) {
        free(stack);
        fail(l, 1, "out of memory");
        return -1;
    }
    size_t top = 0, next = 0;
    stack[top++] = 0;
    while (top) {
        size_t i = stack[--top];
        l->nodes[i].ino = ROOTINO + next;
        l->order[next++] = i;
        const Node* n = &l->nodes[i];
        if (n->dir) {
            for (size_t c = n->childCount; c-- > 0;)
                stack[top++] = n->firstChild + c;
        }
    }
    free(stack);
    for (size_t i = 0; i < l->count; i++) {
        if (!l->nodes[i].dir)
            continue;
        uint64_t size = directorySize(l, i);
        if (size > MAXDIRSIZE) {
            fail(l, 0, "%s holds too many entries for one UFS folder", i ? l->nodes[i].path : "the folder");
            return -1;
        }
        l->nodes[i].size = size;
    }
    return 0;
}

/* The smallest newfs layout from start blocks up that holds needBlocks
 * data blocks and needInodes inodes with a sound last group. Capacity
 * isn't monotonic in the size, so every size is tried. */
static int sizeImage(Layout* l, uint64_t start, uint64_t density, uint64_t needBlocks, uint64_t needInodes)
{
    for (uint64_t size = start; size < (1ull << 48); size++) {
        Geom g;
        if (geomNew(&g, size, density))
            continue;
        if (lastGroupOk(&g) && dataCapacity(&g) >= needBlocks && inodeCapacity(&g) >= needInodes) {
            l->geom = g;
            return 0;
        }
    }
    fail(l, 0, "no UFS2 layout holds this folder");
    return -1;
}

static int prepare(Layout* l, const char* src, const PS5Ufs2Options* options)
{
    if (options && options->maker) {
        size_t n = strlen(options->maker);
        int ok = n < VOLNAME_LEN;
        for (size_t i = 0; ok && i < n; i++)
            ok = options->maker[i] >= 0x20 && options->maker[i] < 0x7f;
        if (!ok) {
            fail(l, 0, "the maker's mark must be printable ASCII, at most %u bytes", VOLNAME_LEN - 1);
            return -1;
        }
        memcpy(l->volname, options->maker, n);
    }
    if (listTree(l, src))
        return -1;
    uint64_t next = 0;
    for (size_t k = 0; k < l->count; k++) {
        Node* n = &l->nodes[l->order[k]];
        int ok;
        n->start = next;
        n->slots = totalSlots(divCeil(n->size, BSIZE), &ok);
        if (!ok) {
            fail(l, 0, "%s is larger than UFS2 can address with 64 KiB blocks", n->path);
            return -1;
        }
        next += n->slots;
        if (!n->dir)
            l->fileBytes += n->size;
    }
    l->usedBlocks = next;
    uint64_t objects = l->count;
    uint64_t dataBlocks = next + SPARE_BLOCKS;
    uint64_t needBlocks = dataBlocks + divCeil(options ? options->free_bytes : 0, BSIZE);
    uint64_t needInodes = ROOTINO + objects + SPARE_INODES;
    uint64_t density = densityFor(dataBlocks * BSIZE, objects);
    if (sizeImage(l, needBlocks > 16 ? needBlocks : 16, density, needBlocks, needInodes))
        return -1;
    l->imageSize = l->geom.size * BSIZE;
    /* a volume id from the shape of the tree, as dump-forge makes it */
    uint32_t h = 0x811c9dc5u;
    uint64_t values[4] = { l->imageSize, objects, l->fileBytes, l->usedBlocks };
    for (int v = 0; v < 4; v++) {
        for (int b = 0; b < 8; b++)
            h = (h ^ (uint8_t)(values[v] >> (8 * b))) * 0x01000193u;
    }
    l->fsId = h;
    return 0;
}

int64_t ps5ufs2_measure(const char* src, const PS5Ufs2Options* options, char* error, size_t error_size)
{
    Layout l = { .error = error, .errorSize = error_size };
    if (!src) {
        fail(&l, 0, "no folder");
        return -1;
    }
    int64_t size = prepare(&l, src, options) ? -1 : (int64_t)l.imageSize;
    freeLayout(&l);
    return size;
}

/* ---------------------------------------------------------------- the on-disk structures (ondisk.rs) */

typedef struct {
    uint64_t ndir, nbfree, nifree;
} Csum;

typedef struct {
    Csum cs;
    uint64_t inodes, blocks; /* in use in the group */
} GroupUse;

static uint64_t maxFileSize(void)
{
    uint64_t m = BSIZE * 12 - 1, per = BSIZE;
    for (int i = 0; i < 3; i++) {
        per *= NINDIR;
        m += per;
    }
    return m;
}

/* The superblock (8192 bytes); actual: the byte offset this copy goes at. */
static void superblock(const Layout* l, const Csum* total, uint64_t actual, unsigned char* b)
{
    const Geom* g = &l->geom;
    memset(b, 0, SBLOCKSIZE);
#define S32(at, v) put32(b + (at), (uint32_t)(int32_t)(int64_t)(v))
    S32(8, SBLKNO);
    S32(12, CBLKNO);
    S32(16, IBLKNO);
    S32(20, g->dblkno);
    S32(44, g->ncg);
    S32(48, BSIZE);
    S32(52, BSIZE);
    S32(56, 1);   /* fs_frag */
    S32(60, 0);   /* fs_minfree */
    S32(72, ~(int64_t)(BSIZE - 1)); /* fs_bmask */
    S32(76, ~(int64_t)(BSIZE - 1)); /* fs_fmask */
    S32(80, 16);  /* fs_bshift */
    S32(84, 16);  /* fs_fshift */
    S32(88, CONTIGSUMSIZE); /* fs_maxcontig */
    S32(92, NINDIR);        /* fs_maxbpg */
    S32(96, 0);   /* fs_fragshift */
    S32(100, 7);  /* fs_fsbtodb */
    S32(104, SBLOCKSIZE);
    S32(116, NINDIR);
    S32(120, INOPB);
    S32(128, FS_OPTSPACE);
    S32(144, FIXED_TIME); /* fs_id[0] */
    put32(b + 148, l->fsId);
    S32(156, g->csblocks * BSIZE); /* fs_cssize */
    S32(160, BSIZE);               /* fs_cgsize */
    S32(184, g->ipg);
    S32(188, g->fpg);
    b[209] = 1; /* fs_clean */
    memcpy(b + VOLNAME_AT, l->volname, VOLNAME_LEN);
    b[211] = FS_FLAGS_UPDATED;
    S32(860, BSIZE); /* fs_maxbsize */
    put64(b + 872, g->size); /* fs_providersize */
    put64(b + 992, actual);  /* fs_sblockactualloc */
    put64(b + 1000, SBLOCK_UFS2);
    put64(b + 1008, total->ndir);
    put64(b + 1016, total->nbfree);
    put64(b + 1024, total->nifree);
    put64(b + 1072, (uint64_t)FIXED_TIME);
    put64(b + 1080, g->size);
    put64(b + 1088, dsize(g));
    put64(b + 1096, g->dblkno); /* fs_csaddr */
    put32(b + 1196, 16384);     /* fs_avgfilesize */
    put32(b + 1200, 64);        /* fs_avgfpdir */
    S32(1316, CONTIGSUMSIZE);   /* fs_contigsumsize */
    S32(1320, 120);             /* fs_maxsymlinklen */
    put64(b + 1328, maxFileSize());
    put64(b + 1336, BSIZE - 1); /* fs_qbmask */
    put64(b + 1344, BSIZE - 1); /* fs_qfmask */
    put32(b + 1372, FS_UFS2_MAGIC);
#undef S32
}

/* struct fsrecovery: the 20 bytes before the superblock, for fsck. */
static void fsrecovery(const Geom* g, unsigned char b[20])
{
    put32(b, FS_UFS2_MAGIC);
    put32(b + 4, 7);
    put32(b + 8, (uint32_t)SBLKNO);
    put32(b + 12, (uint32_t)g->fpg);
    put32(b + 16, (uint32_t)g->ncg);
}

static void setBit(unsigned char* b, uint64_t base, uint64_t bit)
{
    b[base + bit / 8] |= (unsigned char)(1u << (bit % 8));
}

/* The cylinder group header and its maps, one block. */
static void cylinderGroup(const Geom* g, uint64_t c, const GroupUse* u, unsigned char* b)
{
    memset(b, 0, BSIZE);
    uint64_t ndblk = cgBlocks(g, c);
    uint64_t iusedoff = CG_HDR;
    uint64_t freeoff = iusedoff + divCeil(g->ipg, 8);
    uint64_t nextfree = freeoff + divCeil(g->fpg, 8);
    uint64_t clustersumoff = roundUp(nextfree, 4) - 4;
    uint64_t clusteroff = clustersumoff + (CONTIGSUMSIZE + 1) * 4;
    uint64_t nextfreeoff = clusteroff + divCeil(g->fpg, 8);
    put32(b + 4, CG_MAGIC);
    put32(b + 12, (uint32_t)c);
    put32(b + 20, (uint32_t)ndblk);
    put32(b + 24, (uint32_t)u->cs.ndir);
    put32(b + 28, (uint32_t)u->cs.nbfree);
    put32(b + 32, (uint32_t)u->cs.nifree);
    put32(b + 92, (uint32_t)iusedoff);
    put32(b + 96, (uint32_t)freeoff);
    put32(b + 100, (uint32_t)nextfreeoff);
    put32(b + 104, (uint32_t)clustersumoff);
    put32(b + 108, (uint32_t)clusteroff);
    put32(b + 112, (uint32_t)ndblk); /* cg_nclusterblks */
    put32(b + 116, (uint32_t)g->ipg); /* cg_niblk */
    /* cg_initediblk: what is in use, rounded to an inode block, at least two */
    uint64_t inited = roundUp(u->inodes, INOPB);
    if (inited < 2 * INOPB)
        inited = 2 * INOPB;
    if (inited > g->ipg)
        inited = g->ipg;
    put32(b + 120, (uint32_t)inited);
    put64(b + 136, (uint64_t)FIXED_TIME);
    for (uint64_t i = 0; i < u->inodes; i++)
        setBit(b, iusedoff, i);
    /* free: in groups after the first the blocks in front of the backup
     * superblock, then everything past the allocated data */
    uint64_t firstFree = cgDataStart(g, c) - c * g->fpg + u->blocks;
    uint64_t ranges[2][2] = { { 0, c ? SBLKNO : 0 }, { firstFree, ndblk } };
    uint32_t sum[CONTIGSUMSIZE + 1] = { 0 };
    for (int r = 0; r < 2; r++) {
        for (uint64_t d = ranges[r][0]; d < ranges[r][1]; d++) {
            setBit(b, freeoff, d);
            setBit(b, clusteroff, d);
        }
        if (ranges[r][1] > ranges[r][0]) {
            uint64_t run = ranges[r][1] - ranges[r][0];
            sum[run < CONTIGSUMSIZE ? run : CONTIGSUMSIZE]++;
        }
    }
    /* entry 0 is never used (cg_clustersumoff points a word early) */
    for (unsigned i = 1; i <= CONTIGSUMSIZE; i++)
        put32(b + clustersumoff + 4 * i, sum[i]);
}

/* ---------------------------------------------------------------- writing */

typedef struct {
    int fd;
    unsigned char* buffer; /* OUT_BUFFER */
    size_t fill;
    uint64_t start; /* where buffer goes */
    uint64_t pos;   /* the end of what was put so far */
} Output;

static int writeAt(int fd, const void* bytes, size_t length, uint64_t offset)
{
    size_t done = 0;
    while (done < length) {
        ssize_t n = pwrite(fd, (const char*)bytes + done, length - done, (off_t)(offset + done));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static int flushOutput(Output* o)
{
    if (!o->fill)
        return 0;
    if (writeAt(o->fd, o->buffer, o->fill, o->start))
        return -1;
    o->start += o->fill;
    o->fill = 0;
    return 0;
}

/* Puts bytes at at, never before what was put already. -1: errno (EINVAL:
 * the writer went back, a bug). */
static int put(Output* o, uint64_t at, const void* bytes, size_t length)
{
    if (at < o->pos) {
        errno = EINVAL;
        return -1;
    }
    if (at != o->start + o->fill) {
        if (flushOutput(o))
            return -1;
        o->start = at;
    }
    if (length >= OUT_BUFFER) {
        if (flushOutput(o) || writeAt(o->fd, bytes, length, at))
            return -1;
        o->start = at + length;
    } else {
        while (length) {
            size_t room = OUT_BUFFER - o->fill;
            size_t n = length < room ? length : room;
            memcpy(o->buffer + o->fill, bytes, n);
            o->fill += n;
            bytes = (const char*)bytes + n;
            length -= n;
            if (o->fill == OUT_BUFFER && flushOutput(o))
                return -1;
        }
    }
    return 0;
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

typedef struct {
    Layout* l;
    Output out;
    const char* src;
    GroupUse* use;
    Csum total;
    uint64_t nextGroup;
    unsigned char* block;  /* BSIZE: indirect, inode, cg blocks */
    unsigned char* chunk;  /* CHUNK: file data */
    unsigned char* dirBuffer;
    size_t dirBufferSize;
    const Node* node;      /* the one being written */
    int fd;                /* its file */
    uint64_t done;
    ps5ufs2_progress_fn progress;
    void* data;
    double lastReport;
    volatile int* cancel;
} Writer;

static int putAt(Writer* w, uint64_t at, const void* bytes, size_t length)
{
    if (put(&w->out, at, bytes, length)) {
        if (errno == EINVAL)
            fail(w->l, 1, "internal error: the UFS2 writer went back to byte %llu", (unsigned long long)at);
        else
            fail(w->l, 1, "the image couldn't be written (errno %d)", errno);
        return -2;
    }
    if (at + length > w->out.pos)
        w->out.pos = at + length;
    return 0;
}

static int cancelled(Writer* w)
{
    if (w->cancel && *w->cancel) {
        fail(w->l, 0, "cancelled");
        return 1;
    }
    return 0;
}

static void dinode(const Layout* l, const Node* n, unsigned char* b)
{
    const Geom* g = &l->geom;
    uint64_t nblocks = divCeil(n->size, BSIZE);
    memset(b, 0, 256);
    put16(b, (uint16_t)((n->dir ? IFDIR : IFREG) | 0777));
    put16(b + 2, (uint16_t)(n->dir ? 2 + n->subdirs : 1));
    put64(b + 16, n->size);
    put64(b + 24, n->slots * (BSIZE / 512));
    for (int at = 32; at <= 56; at += 8)
        put64(b + at, (uint64_t)FIXED_TIME); /* atime, mtime, ctime, birthtime */
    for (uint64_t i = 0; i < nblocks && i < NDADDR; i++)
        put64(b + 112 + 8 * i, locate(g, n->start + i, NULL));
    uint64_t roots[NIADDR];
    indirectRoots(nblocks, roots);
    for (int i = 0; i < NIADDR; i++)
        put64(b + 208 + 8 * i, roots[i] == UINT64_MAX ? 0 : locate(g, n->start + roots[i], NULL));
    put32(b + 240, n->dir ? n->depth : 0);
}

/* The metadata of group c: backup superblock, cg block, inodes; in group 0
 * also the recovery block, the primary superblock and the summary area. */
static int groupMeta(Writer* w, uint64_t c)
{
    const Layout* l = w->l;
    const Geom* g = &l->geom;
    uint64_t base = c * g->fpg * BSIZE;
    unsigned char sb[SBLOCKSIZE];
    int r;
    if (!c) {
        unsigned char rec[20];
        fsrecovery(g, rec);
        if ((r = putAt(w, SBLOCK_UFS2 - 20, rec, sizeof rec)))
            return r;
        superblock(l, &w->total, SBLOCK_UFS2, sb);
        if ((r = putAt(w, SBLOCK_UFS2, sb, sizeof sb)))
            return r;
    }
    uint64_t at = base + SBLKNO * BSIZE;
    superblock(l, &w->total, at, sb);
    if ((r = putAt(w, at, sb, sizeof sb)))
        return r;
    const GroupUse* u = &w->use[c];
    cylinderGroup(g, c, u, w->block);
    if ((r = putAt(w, base + CBLKNO * BSIZE, w->block, BSIZE)))
        return r;
    for (uint64_t blk = 0; blk < divCeil(u->inodes, INOPB); blk++) {
        memset(w->block, 0, BSIZE);
        uint64_t inBlock = u->inodes - blk * INOPB < INOPB ? u->inodes - blk * INOPB : INOPB;
        for (uint64_t j = 0; j < inBlock; j++) {
            uint64_t ino = c * g->ipg + blk * INOPB + j;
            if (ino < ROOTINO)
                continue;
            dinode(l, &l->nodes[l->order[ino - ROOTINO]], w->block + j * 256);
        }
        if ((r = putAt(w, base + (IBLKNO + blk) * BSIZE, w->block, BSIZE)))
            return r;
    }
    if (!c) {
        size_t bytes = (size_t)(g->csblocks * BSIZE);
        unsigned char* sum = calloc(1, bytes);
        if (!sum) {
            fail(w->l, 1, "out of memory");
            return -2;
        }
        for (uint64_t i = 0; i < g->ncg; i++) {
            put32(sum + i * 16, (uint32_t)w->use[i].cs.ndir);
            put32(sum + i * 16 + 4, (uint32_t)w->use[i].cs.nbfree);
            put32(sum + i * 16 + 8, (uint32_t)w->use[i].cs.nifree);
        }
        r = putAt(w, g->dblkno * BSIZE, sum, bytes);
        free(sum);
        if (r)
            return r;
    }
    return 0;
}

/* The metadata of every group that starts at or before block. */
static int metaBefore(Writer* w, uint64_t block)
{
    const Geom* g = &w->l->geom;
    while (w->nextGroup < g->ncg && w->nextGroup * g->fpg <= block) {
        if (cancelled(w))
            return -3;
        int r = groupMeta(w, w->nextGroup);
        if (r)
            return r;
        w->nextGroup++;
    }
    return 0;
}

static int dataAt(Writer* w, uint64_t block, const void* bytes, size_t length)
{
    int r = metaBefore(w, block);
    return r ? r : putAt(w, block * BSIZE, bytes, length);
}

static int onIndirect(void* user, uint64_t slot, unsigned level, uint64_t k)
{
    Writer* w = user;
    const Geom* g = &w->l->geom;
    const Node* n = w->node;
    memset(w->block, 0, BSIZE);
    uint64_t per = span(level - 1), step = fullSlots(level - 1);
    for (uint64_t j = 0; j < divCeil(k, per); j++)
        put64(w->block + j * 8, locate(g, n->start + slot + 1 + j * step, NULL));
    return dataAt(w, locate(g, n->start + slot, NULL), w->block, BSIZE);
}

static int onData(void* user, uint64_t slot, uint64_t lbn, uint64_t count)
{
    Writer* w = user;
    const Geom* g = &w->l->geom;
    const Node* n = w->node;
    for (uint64_t b = 0; b < count;) {
        if (cancelled(w))
            return -3;
        uint64_t contiguous;
        uint64_t p = locate(g, n->start + slot + b, &contiguous);
        uint64_t blocks = count - b;
        if (blocks > contiguous)
            blocks = contiguous;
        if (blocks > CHUNK / BSIZE)
            blocks = CHUNK / BSIZE;
        uint64_t off = (lbn + b) * BSIZE;
        size_t length = (size_t)(blocks * BSIZE < n->size - off ? blocks * BSIZE : n->size - off);
        int r;
        if (n->dir) {
            r = dataAt(w, p, w->dirBuffer + off, length);
        } else {
            for (size_t got = 0; got < length;) {
                ssize_t m = pread(w->fd, w->chunk + got, length - got, (off_t)(off + got));
                if (m < 0 && errno == EINTR)
                    continue;
                if (m <= 0) {
                    fail(w->l, 1, m ? "%s couldn't be read (errno %d)" : "%s got shorter while it was copied", n->path, errno);
                    return -2;
                }
                got += (size_t)m;
            }
            r = dataAt(w, p, w->chunk, length);
            w->done += length;
            double t = now();
            if (w->progress && t - w->lastReport >= 0.1) {
                w->lastReport = t;
                w->progress(w->done, w->l->fileBytes, n->path, w->data);
            }
        }
        if (r)
            return r;
        b += blocks;
    }
    return 0;
}

static int writeNode(Writer* w, const Node* n)
{
    uint64_t nblocks = divCeil(n->size, BSIZE);
    if (!nblocks)
        return 0;
    w->node = n;
    w->fd = -1;
    if (n->dir) {
        if (n->size > w->dirBufferSize) {
            unsigned char* grown = realloc(w->dirBuffer, (size_t)n->size);
            if (!grown) {
                fail(w->l, 1, "out of memory");
                return -2;
            }
            w->dirBuffer = grown;
            w->dirBufferSize = (size_t)n->size;
        }
        memset(w->dirBuffer, 0, (size_t)n->size);
        directoryBytes(w->l, (size_t)(n - w->l->nodes), w->dirBuffer);
    } else {
        char* full = joinPath(w->src, n->path);
        if (!full) {
            fail(w->l, 1, "out of memory");
            return -2;
        }
        w->fd = open(full, O_RDONLY);
        free(full);
        struct stat st;
        if (w->fd < 0 || fstat(w->fd, &st)) {
            fail(w->l, 1, "%s couldn't be opened (errno %d)", n->path, errno);
            if (w->fd >= 0)
                close(w->fd);
            return -2;
        }
        if ((uint64_t)st.st_size != n->size) {
            close(w->fd);
            fail(w->l, 1, "%s changed while the image was made", n->path);
            return -2;
        }
    }
    SegWalk walk = { onData, onIndirect, w };
    int r = walkSegments(&walk, nblocks);
    if (w->fd >= 0)
        close(w->fd);
    w->fd = -1;
    return r;
}

static int writeImage(Writer* w)
{
    Layout* l = w->l;
    const Geom* g = &l->geom;
    if (!(w->use = calloc(g->ncg, sizeof *w->use))) {
        fail(l, 1, "out of memory");
        return -2;
    }
    uint64_t inodesUsed = ROOTINO + l->count;
    for (uint64_t c = 0; c < g->ncg; c++) {
        GroupUse* u = &w->use[c];
        uint64_t inodes = inodesUsed > c * g->ipg ? inodesUsed - c * g->ipg : 0;
        u->inodes = inodes < g->ipg ? inodes : g->ipg;
        uint64_t start, end;
        cgDataRange(g, c, &start, &end);
        uint64_t used = l->usedBlocks < start ? start : l->usedBlocks > end ? end : l->usedBlocks;
        u->blocks = used - start;
        u->cs.nbfree = (c ? SBLKNO : 0) + cgData(g, c) - u->blocks;
        u->cs.nifree = g->ipg - u->inodes;
    }
    for (size_t i = 0; i < l->count; i++) {
        if (l->nodes[i].dir)
            w->use[l->nodes[i].ino / g->ipg].cs.ndir++;
    }
    for (uint64_t c = 0; c < g->ncg; c++) {
        w->total.ndir += w->use[c].cs.ndir;
        w->total.nbfree += w->use[c].cs.nbfree;
        w->total.nifree += w->use[c].cs.nifree;
    }
    if (w->progress)
        w->progress(0, l->fileBytes, "", w->data);
    for (size_t k = 0; k < l->count; k++) {
        if (cancelled(w))
            return -3;
        int r = writeNode(w, &l->nodes[l->order[k]]);
        if (r)
            return r;
    }
    int r = metaBefore(w, UINT64_MAX);
    if (r)
        return r;
    if (w->out.pos > l->imageSize) {
        fail(l, 1, "internal error: the UFS2 writer reached byte %llu of a %llu-byte image", (unsigned long long)w->out.pos,
            (unsigned long long)l->imageSize);
        return -2;
    }
    if (w->out.pos < l->imageSize) {
        unsigned char zero = 0;
        if ((r = putAt(w, l->imageSize - 1, &zero, 1)))
            return r;
    }
    if (flushOutput(&w->out)) {
        fail(l, 1, "the image couldn't be written (errno %d)", errno);
        return -2;
    }
    if (w->progress)
        w->progress(l->fileBytes, l->fileBytes, "", w->data);
    return cancelled(w) ? -3 : 0;
}

/* image's folder is src or a folder in it. */
static int insideFolder(const char* image, const char* src)
{
    char* folder = strdup(image);
    if (!folder)
        return 0;
    char* slash = strrchr(folder, '/');
    if (!slash)
        strcpy(folder, ".");
    else if (slash == folder)
        folder[1] = 0;
    else
        *slash = 0;
    char* a = realpath(folder, NULL);
    char* b = realpath(src, NULL);
    free(folder);
    size_t n = b ? strlen(b) : 0;
    int inside = a && b && !strncmp(a, b, n) && (a[n] == 0 || a[n] == '/' || (n && b[n - 1] == '/'));
    free(a);
    free(b);
    return inside;
}

int ps5ufs2_build(const char* src, const char* image, const PS5Ufs2Options* options, ps5ufs2_progress_fn progress, void* data,
    volatile int* cancel, char* error, size_t error_size)
{
    Layout l = { .error = error, .errorSize = error_size };
    if (!src || !image || !*image) {
        fail(&l, 0, "no folder or image");
        return -1;
    }
    struct stat st;
    if (!lstat(image, &st)) {
        fail(&l, 0, "%s already exists", image);
        return -1;
    }
    if (insideFolder(image, src)) {
        fail(&l, 0, "the image can't go inside the folder it is made from");
        return -1;
    }
    if (prepare(&l, src, options)) {
        freeLayout(&l);
        return l.failure ? -2 : -1;
    }
    int result = -2;
    char* part = malloc(strlen(image) + 6);
    char* folder = strdup(image);
    Writer w = { .l = &l, .src = src, .fd = -1, .progress = progress, .data = data, .cancel = cancel };
    w.out.fd = -1;
    if (!part || !folder || !(w.out.buffer = malloc(OUT_BUFFER)) || !(w.block = malloc(BSIZE)) || !(w.chunk = malloc(CHUNK))) {
        fail(&l, 1, "out of memory");
        goto done;
    }
    sprintf(part, "%s.part", image);
    char* slash = strrchr(folder, '/');
    if (!slash)
        strcpy(folder, ".");
    else if (slash == folder)
        folder[1] = 0;
    else
        *slash = 0;
    struct statvfs fs;
    uint64_t margin = options && options->free_margin ? options->free_margin : DEFAULT_MARGIN;
    if (statvfs(folder, &fs)) {
        fail(&l, 1, "%s couldn't be checked for space (errno %d)", folder, errno);
        goto done;
    }
    uint64_t space = (uint64_t)fs.f_bavail * (fs.f_frsize ? fs.f_frsize : fs.f_bsize);
    if (space < l.imageSize + margin) {
        fail(&l, 1, "not enough space: the image needs %llu MB and %llu MB more left free, %llu MB are free",
            (unsigned long long)(l.imageSize >> 20), (unsigned long long)(margin >> 20), (unsigned long long)(space >> 20));
        goto done;
    }
    w.out.fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (w.out.fd < 0) {
        fail(&l, 1, "%s couldn't be made (errno %d)", part, errno);
        goto done;
    }
    result = writeImage(&w);
    if (close(w.out.fd) && !result) {
        fail(&l, 1, "the image couldn't be written (errno %d)", errno);
        result = -2;
    }
    w.out.fd = -1;
    if (!result) {
        if (!lstat(image, &st)) {
            fail(&l, 0, "%s already exists", image);
            result = -1;
        } else if (rename(part, image)) {
            fail(&l, 1, "%s couldn't be renamed (errno %d)", part, errno);
            result = -2;
        }
    }
    if (result)
        unlink(part);
done:
    if (w.out.fd >= 0)
        close(w.out.fd);
    free(w.out.buffer);
    free(w.block);
    free(w.chunk);
    free(w.dirBuffer);
    free(w.use);
    free(part);
    free(folder);
    freeLayout(&l);
    return result;
}

/* ---------------------------------------------------------------- reading an image */

/* Any UFS2 volume: the sizes from its superblock. */
typedef struct {
    int fd;
    uint64_t fileSize;
    uint32_t bsize, fsize, frag, ipg, fpg, iblkno, inopb, nindir, ncg;
    uint64_t size; /* fragments */
    /* the indirect blocks read last, one a level */
    uint64_t cached[3];
    unsigned char* cache[3];
    char* error;
    size_t errorSize;
} Ufs;

static int ufsFail(Ufs* u, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    if (u->error && u->errorSize)
        vsnprintf(u->error, u->errorSize, format, args);
    va_end(args);
    return -1;
}

static uint32_t get32(const unsigned char* p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const unsigned char* p)
{
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
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

static int isPowerOfTwo(uint32_t v)
{
    return v && !(v & (v - 1));
}

static void ufsClose(Ufs* u)
{
    if (u->fd >= 0)
        close(u->fd);
    u->fd = -1;
    for (int i = 0; i < 3; i++) {
        free(u->cache[i]);
        u->cache[i] = NULL;
    }
}

/* 0; -1 not UFS2 (error says why). */
static int ufsOpen(Ufs* u, const char* image)
{
    u->fd = open(image, O_RDONLY);
    struct stat st;
    if (u->fd < 0 || fstat(u->fd, &st))
        return ufsFail(u, "%s couldn't be opened (errno %d)", image, errno);
    u->fileSize = (uint64_t)st.st_size;
    unsigned char sb[1376];
    if (u->fileSize < SBLOCK_UFS2 + sizeof sb || readAt(u->fd, sb, sizeof sb, SBLOCK_UFS2))
        return ufsFail(u, "not a UFS2 image (too short)");
    if (get32(sb + 1372) != FS_UFS2_MAGIC)
        return ufsFail(u, "not a UFS2 image");
    u->bsize = get32(sb + 48);
    u->fsize = get32(sb + 52);
    u->frag = get32(sb + 56);
    u->ncg = get32(sb + 44);
    u->ipg = get32(sb + 184);
    u->fpg = get32(sb + 188);
    u->iblkno = get32(sb + 16);
    u->inopb = get32(sb + 120);
    u->nindir = get32(sb + 116);
    u->size = get64(sb + 1080);
    if (!isPowerOfTwo(u->bsize) || !isPowerOfTwo(u->fsize) || u->bsize < 4096 || u->bsize > (1u << 20) || u->fsize > u->bsize
        || u->frag != u->bsize / u->fsize || u->inopb != u->bsize / 256 || u->nindir != u->bsize / 8 || !u->ipg || !u->fpg || !u->ncg
        || u->ipg % u->inopb || (uint64_t)u->ncg * u->ipg > (1ull << 32))
        return ufsFail(u, "a UFS2 superblock with an unexpected geometry");
    if (u->size * u->fsize > u->fileSize)
        return ufsFail(u, "the image is shorter than its filesystem (%llu of %llu bytes)", (unsigned long long)u->fileSize,
            (unsigned long long)(u->size * u->fsize));
    for (int i = 0; i < 3; i++) {
        u->cached[i] = 0;
        if (!(u->cache[i] = malloc(u->bsize)))
            return ufsFail(u, "out of memory");
    }
    return 0;
}

/* Inode ino's 256 bytes. */
static int readInode(Ufs* u, uint64_t ino, unsigned char d[256])
{
    if (ino < ROOTINO || ino >= (uint64_t)u->ncg * u->ipg)
        return ufsFail(u, "an inode number out of range (%llu)", (unsigned long long)ino);
    uint64_t c = ino / u->ipg, in = ino % u->ipg;
    uint64_t frag = c * u->fpg + u->iblkno + in / u->inopb * u->frag;
    uint64_t offset = frag * u->fsize + in % u->inopb * 256;
    if (offset + 256 > u->fileSize || readAt(u->fd, d, 256, offset))
        return ufsFail(u, "inode %llu couldn't be read", (unsigned long long)ino);
    return 0;
}

/* Pointer index of a block read through the cache at level. */
static int pointer(Ufs* u, int level, uint64_t block, uint64_t index, uint64_t* out)
{
    if (!block) {
        *out = 0;
        return 0;
    }
    if (block >= u->size)
        return ufsFail(u, "a block pointer past the volume's end");
    if (u->cached[level] != block) {
        if (readAt(u->fd, u->cache[level], u->bsize, block * u->fsize))
            return ufsFail(u, "an indirect block couldn't be read");
        u->cached[level] = block;
    }
    *out = get64(u->cache[level] + index * 8);
    return 0;
}

/* The fragment address of logical block lbn of the inode d (0: a hole). */
static int blockOf(Ufs* u, const unsigned char* d, uint64_t lbn, uint64_t* out)
{
    uint64_t n = u->nindir;
    if (lbn < NDADDR) {
        *out = get64(d + 112 + 8 * lbn);
        return 0;
    }
    lbn -= NDADDR;
    if (lbn < n)
        return pointer(u, 0, get64(d + 208), lbn, out);
    lbn -= n;
    uint64_t p;
    if (lbn < n * n)
        return pointer(u, 1, get64(d + 216), lbn / n, &p) || pointer(u, 0, p, lbn % n, out) ? -1 : 0;
    lbn -= n * n;
    if (lbn / n / n >= n)
        return ufsFail(u, "a file longer than its block map");
    uint64_t q;
    return pointer(u, 2, get64(d + 224), lbn / n / n, &p) || pointer(u, 1, p, lbn / n % n, &q) || pointer(u, 0, q, lbn % n, out) ? -1 : 0;
}

/* length bytes at offset of the inode d's data. */
static int readData(Ufs* u, const unsigned char* d, uint64_t offset, void* buffer, size_t length)
{
    uint64_t size = get64(d + 16);
    if (offset + length > size)
        return ufsFail(u, "a read past a file's end");
    unsigned char* out = buffer;
    while (length) {
        uint64_t lbn = offset / u->bsize, in = offset % u->bsize;
        size_t n = u->bsize - in < length ? (size_t)(u->bsize - in) : length;
        uint64_t frag;
        if (blockOf(u, d, lbn, &frag))
            return -1;
        if (!frag)
            memset(out, 0, n);
        else if (frag >= u->size || frag * u->fsize + in + n > u->fileSize)
            return ufsFail(u, "a block pointer past the volume's end");
        else if (readAt(u->fd, out, n, frag * u->fsize + in))
            return ufsFail(u, "the image couldn't be read (errno %d)", errno);
        out += n;
        offset += n;
        length -= n;
    }
    return 0;
}

static unsigned inodeMode(const unsigned char* d)
{
    return (unsigned)(d[0] | d[1] << 8) & IFMT;
}

/* A directory's bytes (malloc'ed) with its length. */
static unsigned char* readDirectory(Ufs* u, const unsigned char* d, size_t* length)
{
    uint64_t size = get64(d + 16);
    if (inodeMode(d) != IFDIR) {
        ufsFail(u, "not a folder");
        return NULL;
    }
    if (size % UFS_DIRBLKSIZ || size > (64u << 20)) {
        ufsFail(u, "a folder's size is wrong (%llu bytes)", (unsigned long long)size);
        return NULL;
    }
    unsigned char* bytes = malloc(size ? (size_t)size : 1);
    if (!bytes) {
        ufsFail(u, "out of memory");
        return NULL;
    }
    if (readData(u, d, 0, bytes, (size_t)size)) {
        free(bytes);
        return NULL;
    }
    *length = (size_t)size;
    return bytes;
}

/* The next entry of a directory at *at: its inode (0 unused), type and
 * name. 1 an entry, 0 the end, -1 damaged. */
static int nextEntry(Ufs* u, const unsigned char* bytes, size_t length, size_t* at, uint32_t* ino, unsigned* type, char name[256])
{
    while (*at < length) {
        const unsigned char* e = bytes + *at;
        size_t chunkEnd = (*at / UFS_DIRBLKSIZ + 1) * UFS_DIRBLKSIZ;
        if (chunkEnd > length)
            chunkEnd = length; /* a folder whose size isn't whole 512-byte chunks */
        if (*at + 8 > chunkEnd) /* the header must be there before it's read */
            return ufsFail(u, "a damaged folder entry");
        unsigned reclen = (unsigned)(e[4] | e[5] << 8);
        unsigned namlen = e[7];
        if (reclen < 8 || reclen % 4 || *at + reclen > chunkEnd || 8u + namlen > reclen)
            return ufsFail(u, "a damaged folder entry");
        *at += reclen;
        *ino = get32(e);
        if (!*ino)
            continue;
        *type = e[6];
        memcpy(name, e + 8, namlen);
        name[namlen] = 0;
        return 1;
    }
    return 0;
}

/* The inode called name in the directory d. 1 found, 0 not, -1 damaged. */
static int findIn(Ufs* u, const unsigned char* d, const char* name, uint64_t* ino)
{
    size_t length;
    unsigned char* bytes = readDirectory(u, d, &length);
    if (!bytes)
        return -1;
    size_t at = 0;
    uint32_t found;
    unsigned type;
    char entry[256];
    int r;
    while ((r = nextEntry(u, bytes, length, &at, &found, &type, entry)) == 1) {
        if (!strcmp(entry, name)) {
            *ino = found;
            break;
        }
    }
    free(bytes);
    return r;
}

bool ps5ufs2_is_ufs2(const char* image)
{
    int fd = open(image, O_RDONLY);
    if (fd < 0)
        return false;
    unsigned char magic[4];
    bool is = !readAt(fd, magic, 4, SBLOCK_UFS2 + 1372) && get32(magic) == FS_UFS2_MAGIC;
    close(fd);
    return is;
}

int ps5ufs2_read_file(const char* image, const char* path, void* buffer, size_t size, uint64_t* length, char* error,
    size_t error_size)
{
    Ufs u = { .fd = -1, .error = error, .errorSize = error_size };
    if (error && error_size)
        error[0] = 0;
    int result = -1;
    unsigned char d[256];
    if (ufsOpen(&u, image) || readInode(&u, ROOTINO, d)) {
        result = u.fd >= 0 && u.fileSize >= SBLOCK_UFS2 + 1376 && ps5ufs2_is_ufs2(image) ? -2 : -1;
        goto done;
    }
    char part[256];
    for (const char* p = path; *p;) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        size_t n = strcspn(p, "/");
        if (n >= sizeof part) {
            ufsFail(&u, "%s isn't in the image", path);
            goto done;
        }
        memcpy(part, p, n);
        part[n] = 0;
        p += n;
        uint64_t ino = 0;
        int r = findIn(&u, d, part, &ino);
        if (r < 0) {
            result = -2;
            goto done;
        }
        if (!r) {
            ufsFail(&u, "%s isn't in the image", path);
            goto done;
        }
        if (readInode(&u, ino, d)) {
            result = -2;
            goto done;
        }
    }
    if (inodeMode(d) != IFREG) {
        ufsFail(&u, "%s isn't a file in the image", path);
        goto done;
    }
    uint64_t fileLength = get64(d + 16);
    if (length)
        *length = fileLength;
    size_t want = fileLength < size ? (size_t)fileLength : size;
    if (want && readData(&u, d, 0, buffer, want)) {
        result = -2;
        goto done;
    }
    result = 0;
done:
    ufsClose(&u);
    return result;
}

/* ---------------------------------------------------------------- verify */

typedef struct {
    Ufs u;
    uint64_t done, total;
    int counting;
    ps5ufs2_progress_fn progress;
    void* data;
    volatile int* cancel;
    unsigned char* buffer; /* CHUNK: the image */
    unsigned char* other;  /* CHUNK: the source */
    double lastReport;
} Check;

static int namesCompare(const void* a, const void* b)
{
    return strcmp(*(char* const*)a, *(char* const*)b);
}

static int compareFile(Check* c, const unsigned char* d, const char* path)
{
    uint64_t size = get64(d + 16);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return ufsFail(&c->u, "%s couldn't be read (errno %d)", path, errno);
    int result = 0;
    for (uint64_t at = 0; at < size && !result;) {
        if (c->cancel && *c->cancel) {
            result = ufsFail(&c->u, "Cancelled");
            break;
        }
        size_t n = size - at < CHUNK ? (size_t)(size - at) : (size_t)CHUNK;
        if (readData(&c->u, d, at, c->buffer, n))
            result = -1;
        else if (readAt(fd, c->other, n, at))
            result = ufsFail(&c->u, "%s changed or couldn't be read (errno %d)", path, errno);
        else if (memcmp(c->buffer, c->other, n))
            result = ufsFail(&c->u, "%s differs from the image's copy", path);
        at += n;
        c->done += n;
        double t = now();
        if (c->progress && t - c->lastReport >= 0.25) {
            c->lastReport = t;
            c->progress(c->done, c->total, "", c->data);
        }
    }
    close(fd);
    return result;
}

static int checkDirectory(Check* c, const unsigned char* dir, uint64_t ino, uint64_t parent, const char* src, unsigned depth)
{
    if (depth > 128)
        return ufsFail(&c->u, "folders nested too deep");
    size_t length;
    unsigned char* bytes = readDirectory(&c->u, dir, &length);
    if (!bytes)
        return -1;
    char** names = NULL;
    unsigned count = 0, capacity = 0;
    int result = 0, sawDot = 0, sawDotDot = 0;
    size_t at = 0;
    uint32_t entryIno;
    unsigned type;
    char name[256];
    int r;
    while (!result && (r = nextEntry(&c->u, bytes, length, &at, &entryIno, &type, name)) == 1) {
        if (!strcmp(name, ".")) {
            sawDot = 1;
            if (entryIno != ino)
                result = ufsFail(&c->u, "%s: its \".\" isn't itself", src);
            continue;
        }
        if (!strcmp(name, "..")) {
            sawDotDot = 1;
            if (entryIno != parent)
                result = ufsFail(&c->u, "%s: its \"..\" isn't its parent", src);
            continue;
        }
        if (strchr(name, '/') || !*name) {
            result = ufsFail(&c->u, "%s: a bad name in the image", src);
            break;
        }
        char* path = joinPath(src, name);
        unsigned char d[256];
        struct stat st;
        if (!path) {
            result = ufsFail(&c->u, "out of memory");
            break;
        }
        if (lstat(path, &st))
            result = ufsFail(&c->u, "%s is in the image but not in the folder", path);
        else if (readInode(&c->u, entryIno, d))
            result = -1;
        else if ((inodeMode(d) == IFDIR) != S_ISDIR(st.st_mode) || (inodeMode(d) != IFDIR && inodeMode(d) != IFREG))
            result = ufsFail(&c->u, "%s is a %s in the image", path, inodeMode(d) == IFDIR ? "folder" : "file");
        else if ((type == DT_DIR) != (inodeMode(d) == IFDIR))
            result = ufsFail(&c->u, "%s: its folder entry's type is wrong", path);
        else if (S_ISDIR(st.st_mode))
            result = checkDirectory(c, d, entryIno, ino, path, depth + 1);
        else if (get64(d + 16) != (uint64_t)st.st_size)
            result = ufsFail(&c->u, "%s: %llu bytes in the image, %llu in the folder", path, (unsigned long long)get64(d + 16),
                (unsigned long long)st.st_size);
        else if (c->counting)
            c->total += (uint64_t)st.st_size;
        else
            result = compareFile(c, d, path);
        free(path);
        if (!result) {
            if (count == capacity) {
                capacity = capacity ? capacity * 2 : 64;
                char** grown = realloc(names, capacity * sizeof *grown);
                if (!grown) {
                    result = ufsFail(&c->u, "out of memory");
                    break;
                }
                names = grown;
            }
            if (!(names[count] = strdup(name)))
                result = ufsFail(&c->u, "out of memory");
            else
                count++;
        }
    }
    if (!result && r < 0)
        result = -1;
    free(bytes);
    if (!result && (!sawDot || !sawDotDot))
        result = ufsFail(&c->u, "%s: its \".\" or \"..\" is missing in the image", src);
    if (!result && count > 1) {
        qsort(names, count, sizeof *names, namesCompare);
        for (unsigned i = 1; i < count && !result; i++) {
            if (!strcmp(names[i - 1], names[i]))
                result = ufsFail(&c->u, "%s/%s is in the image twice", src, names[i]);
        }
    }
    for (unsigned i = 0; i < count; i++)
        free(names[i]);
    free(names);
    if (result)
        return result;
    DIR* folder = opendir(src);
    if (!folder)
        return ufsFail(&c->u, "%s couldn't be read (errno %d)", src, errno);
    unsigned there = 0;
    struct dirent* entry;
    while ((entry = readdir(folder)))
        there += strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..");
    closedir(folder);
    if (there != count)
        return ufsFail(&c->u, "%s has %u entries, the image %u", src, there, count);
    return 0;
}

int ps5ufs2_verify(const char* image, const char* src, ps5ufs2_progress_fn progress, void* data, volatile int* cancel, char* error,
    size_t error_size)
{
    Check c = { .u = { .fd = -1, .error = error, .errorSize = error_size }, .progress = progress, .data = data, .cancel = cancel };
    if (error && error_size)
        error[0] = 0;
    int result = -1;
    unsigned char root[256];
    if (ufsOpen(&c.u, image) || readInode(&c.u, ROOTINO, root))
        goto done;
    if (inodeMode(root) != IFDIR) {
        ufsFail(&c.u, "the image's root isn't a folder");
        goto done;
    }
    if (!(c.buffer = malloc(CHUNK)) || !(c.other = malloc(CHUNK))) {
        ufsFail(&c.u, "out of memory");
        goto done;
    }
    /* the sizes first (the progress's total), then the bytes */
    c.counting = 1;
    if (checkDirectory(&c, root, ROOTINO, ROOTINO, src, 0))
        goto done;
    c.counting = 0;
    if (progress)
        progress(0, c.total, "", data);
    if (checkDirectory(&c, root, ROOTINO, ROOTINO, src, 0))
        goto done;
    if (progress)
        progress(c.done, c.total, "", data);
    result = 0;
done:
    if (result && cancel && *cancel && error && error_size)
        snprintf(error, error_size, "Cancelled");
    ufsClose(&c.u);
    free(c.buffer);
    free(c.other);
    return result ? (cancel && *cancel ? -3 : -1) : 0;
}
