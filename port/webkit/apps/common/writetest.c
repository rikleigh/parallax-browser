// The disk write test: N MB to one file with plain write(), timed per 64 MB,
// one thing varied at a time. Development builds only (PS5WK_DEBUG_BUILD):
// the title runs it for POST /debug/write-test (apps/title/ps5browser.c) and
// ps5wk-child as the task payload "writetest", so the title process and a
// payload process can be compared on the same file system.
//
// Why: a worker's OPFS sync-access-handle writes, with pages in the browser
// process, fall from about 235 MB/s to about 2 MB/s once the title has
// written about 1.3 GB, while a payload (the NetworkProcess, a WebProcess
// payload) keeps about 200 MB/s (README "Storage": B8, 2026-11-26).
//
// A test is "key=value&key=value" (values percent-encoded):
//   dir=<folder>      where the file goes (default /data/ps5webkit). The file
//                     is always ps5wk-write-test[-<tag>].bin, so no other file
//                     can be overwritten. dir=null: /dev/null.
//   tag=<letters>     part of the file's name (two tests at once)
//   mb=<n>            MB per pass (default 3072, at most 32768)
//   chunk=<KB>        bytes per write() in KB (default 65536, 4 to 262144)
//   report=<MB>       a line per this many MB (default 64)
//   direct=1          open with O_DIRECT          osync=1   open with O_SYNC
//   sync=fsync|fdatasync   called every syncmb=<MB> (default: report's)
//   fadvise=1         posix_fadvise(DONTNEED) on what was written, at each
//                     sync point or report (a payload only: a title has no
//                     posix_fadvise, it is a raw system call in libc.a)
//   pwrite=1          pwrite() at the offset, not write()
//   reopen=<MB>       close and open the file again every this many MB
//   prealloc=1        ftruncate() to the full size first
//   overwrite=1       no O_TRUNC: write over the file a keep=1 test left
//   pace=<ms>         sleep after every write()
//   passes=<n>        the whole write again (default 1), mb2=<n> MB in the
//                     passes after the first (default mb's), and between passes
//   between=read|delete|sync|none   read the file back, delete it, fsync it
//   sleep=<s>         then wait
//   keep=1            leave the file at the end (default: deleted)
//   vfs=0             no vfs.* sysctls in the lines
//   rctl=<pid>        racct usage and rctl rules of a process (a payload only;
//                     0: this process), before and after
//   uid=<n>           setuid(n) first (a payload only, which is root: the
//                     title's uid is in its "start" line)
// thread= and proc= are the caller's (where the test runs).
//
// gap=<test> runs one of these instead of the speed test (2026-11-26: OPFS
// bytes that were never written read back as other data):
//   gap=1        files with a gap: a head written, then a write past the end
//                (lseek and write(), pwrite(), from a second descriptor,
//                after fsync), ftruncate() to a larger size, a small write
//                into a hole; the gap is read back and its bytes counted:
//                zeros, the marker below, the test's own earlier files ('H'
//                and 'W' bytes) or other data. passes=<n> repeats them.
//   gap=marker   only writes a marker file of marker=<MB> (default 256):
//                16-byte records "PS5WK-MARKER" and a number (fsync first
//                with sync=fsync), then deletes it, so its blocks are free
//                for the next file. With gap=1, marker=<MB> does this first
//                in the same process; sleep=<s> waits after it.
//   gap=mtime    when the modification time of a file settles: a write,
//                fsync and close as a page's sync access handle does, then
//                stat() every 10 ms for 3 s (WebKit's File checks it to the
//                second).
//
// A line per report: MB/s, the time in write() and its longest call, the time
// in fsync, and of the writing thread its CPU time, block output operations
// and context switches (getrusage): a thread held back by the kernel sleeps
// (no CPU time), one that flushes itself counts block operations.
#ifdef PS5WK_DEBUG_BUILD
#include "writetest.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define FILE_NAME "ps5wk-write-test"

#ifdef PS5WK_WRITETEST_SYSCALLS /* a payload: libc.a's raw system calls */
int rctl_get_racct(const char* in, size_t inLength, char* out, size_t outLength);
int rctl_get_rules(const char* in, size_t inLength, char* out, size_t outLength);
#endif

const char* const ps5wk_write_test_keys[] = { "dir", "tag", "mb", "chunk", "report", "direct", "osync", "sync", "syncmb", "fadvise",
    "pwrite", "reopen", "prealloc", "overwrite", "pace", "passes", "mb2", "between", "sleep", "keep", "vfs", "rctl", "uid", "thread", "gap",
    "marker", NULL };

typedef struct {
    PS5WKWriteTestSay say;
    void* data;
    const char* process;
    char path[512];
    char dir[400];
    unsigned long long mb, mb2, chunkKB, reportMB, syncMB, reopenMB;
    unsigned passes, paceMS, sleepS;
    int direct, osync, fadvise, usePwrite, prealloc, overwrite, keep, vfs, null;
    long rctl, uid;
    char sync[16], between[16], gap[16], tag[32];
    unsigned long long markerMB;
} Test;

static void sayf(const Test* t, const char* format, ...) __attribute__((format(printf, 2, 3)));
static void sayf(const Test* t, const char* format, ...)
{
    char line[900];
    int n = snprintf(line, sizeof line, "[writetest] %s %d: ", t->process, (int)getpid());
    va_list args;
    va_start(args, format);
    vsnprintf(line + n, sizeof line - (size_t)n, format, args);
    va_end(args);
    t->say(line, t->data);
}

// The percent-decoded value of key in spec: 1 if it is there.
static int value(const char* spec, const char* key, char* out, size_t size)
{
    size_t keyLength = strlen(key);
    for (const char* p = spec; p && *p;) {
        size_t length = strcspn(p, "&");
        if (length > keyLength && !strncmp(p, key, keyLength) && p[keyLength] == '=') {
            size_t o = 0;
            for (const char* c = p + keyLength + 1; c < p + length && o + 1 < size; c++) {
                if (*c == '%' && c + 2 < p + length && isxdigit((unsigned char)c[1]) && isxdigit((unsigned char)c[2])) {
                    char hex[3] = { c[1], c[2], 0 };
                    out[o++] = (char)strtol(hex, NULL, 16);
                    c += 2;
                } else
                    out[o++] = *c == '+' ? ' ' : *c;
            }
            out[o] = 0;
            return 1;
        }
        p += length;
        if (*p)
            p++;
    }
    return 0;
}

static unsigned long long number(const char* spec, const char* key, unsigned long long fallback, unsigned long long low, unsigned long long high)
{
    char text[32];
    if (!value(spec, key, text, sizeof text) || !isdigit((unsigned char)text[0]))
        return fallback;
    unsigned long long n = strtoull(text, NULL, 10);
    return n < low ? low : n > high ? high : n;
}

static void parse(Test* t, const char* spec)
{
    if (!value(spec, "dir", t->dir, sizeof t->dir) || !t->dir[0])
        snprintf(t->dir, sizeof t->dir, "/data/ps5webkit");
    while (strlen(t->dir) > 1 && t->dir[strlen(t->dir) - 1] == '/')
        t->dir[strlen(t->dir) - 1] = 0;
    t->null = !strcmp(t->dir, "null");
    char tag[32] = "";
    value(spec, "tag", tag, sizeof tag);
    for (char* c = tag; *c; c++) {
        if (!isalnum((unsigned char)*c))
            *c = '_';
    }
    snprintf(t->tag, sizeof t->tag, "%s", tag);
    if (t->null)
        snprintf(t->path, sizeof t->path, "/dev/null");
    else
        snprintf(t->path, sizeof t->path, "%s/" FILE_NAME "%s%s.bin", t->dir, *tag ? "-" : "", tag);
    t->mb = number(spec, "mb", 3072, 1, 32768);
    t->mb2 = number(spec, "mb2", t->mb, 1, 32768);
    t->chunkKB = number(spec, "chunk", 65536, 4, 262144);
    t->reportMB = number(spec, "report", 64, 1, 32768);
    t->syncMB = number(spec, "syncmb", t->reportMB, 1, 32768);
    t->reopenMB = number(spec, "reopen", 0, 0, 32768);
    t->passes = (unsigned)number(spec, "passes", 1, 1, 100);
    t->paceMS = (unsigned)number(spec, "pace", 0, 0, 60000);
    t->sleepS = (unsigned)number(spec, "sleep", 0, 0, 3600);
    t->direct = (int)number(spec, "direct", 0, 0, 1);
    t->osync = (int)number(spec, "osync", 0, 0, 1);
    t->fadvise = (int)number(spec, "fadvise", 0, 0, 1);
    t->usePwrite = (int)number(spec, "pwrite", 0, 0, 1);
    t->prealloc = (int)number(spec, "prealloc", 0, 0, 1);
    t->overwrite = (int)number(spec, "overwrite", 0, 0, 1);
    t->keep = (int)number(spec, "keep", 0, 0, 1);
    t->vfs = (int)number(spec, "vfs", 1, 0, 1);
    t->rctl = value(spec, "rctl", tag, sizeof tag) ? strtol(tag, NULL, 10) : -1;
    t->uid = value(spec, "uid", tag, sizeof tag) ? strtol(tag, NULL, 10) : -1;
    if (!value(spec, "sync", t->sync, sizeof t->sync) || (strcmp(t->sync, "fsync") && strcmp(t->sync, "fdatasync")))
        snprintf(t->sync, sizeof t->sync, "none");
    if (!value(spec, "between", t->between, sizeof t->between))
        snprintf(t->between, sizeof t->between, "none");
    if (!value(spec, "gap", t->gap, sizeof t->gap))
        t->gap[0] = 0;
    t->markerMB = number(spec, "marker", !strcmp(t->gap, "marker") ? 256 : 0, 0, 8192);
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

typedef struct {
    double cpu;
    long blocksOut, voluntary, involuntary;
    int who;
} Usage;

// Of this thread; of the process where the kernel has no per-thread usage.
static Usage usage(void)
{
    struct rusage r;
    Usage u = { 0, 0, 0, 0, RUSAGE_THREAD };
    if (getrusage(RUSAGE_THREAD, &r)) {
        u.who = RUSAGE_SELF;
        if (getrusage(RUSAGE_SELF, &r))
            return u;
    }
    u.cpu = (double)r.ru_utime.tv_sec + (double)r.ru_utime.tv_usec / 1e6 + (double)r.ru_stime.tv_sec + (double)r.ru_stime.tv_usec / 1e6;
    u.blocksOut = r.ru_oublock;
    u.voluntary = r.ru_nvcsw;
    u.involuntary = r.ru_nivcsw;
    return u;
}

// "name value" of the buffer cache's counters, in out: how many could be read.
static unsigned vfsState(const Test* t, char* out, size_t size, int limits)
{
    static const char* const counters[] = { "vfs.numdirtybuffers", "vfs.runningbufspace", "vfs.bufspace", NULL };
    static const char* const all[] = { "vfs.numdirtybuffers", "vfs.lodirtybuffers", "vfs.hidirtybuffers", "vfs.dirtybufthresh",
        "vfs.runningbufspace", "vfs.lorunningspace", "vfs.hirunningspace", "vfs.bufspace", "vfs.maxbufspace", "vfs.hibufspace",
        "vfs.write_behind", "vfs.ffs.doasyncfree", "kern.maxvnodes", NULL };
    size_t o = 0;
    unsigned read = 0;
    out[0] = 0;
    if (!t->vfs)
        return 0;
    for (const char* const* name = limits ? all : counters; *name && o + 48 < size; name++) {
        long long number = 0;
        size_t length = sizeof number;
        if (sysctlbyname(*name, &number, &length, NULL, 0))
            o += (size_t)snprintf(out + o, size - o, "%s%s errno %d", o ? ", " : "", *name, errno);
        else {
            o += (size_t)snprintf(out + o, size - o, "%s%s %lld", o ? ", " : "", *name, length == 4 ? (long long)*(int*)&number : number);
            read++;
        }
    }
    return read;
}

static void rctlState(const Test* t, const char* when)
{
    if (t->rctl < 0)
        return;
#ifdef PS5WK_WRITETEST_SYSCALLS
    char filter[64], out[2048];
    snprintf(filter, sizeof filter, "process:%ld", t->rctl ? t->rctl : (long)getpid());
    out[0] = 0;
    if (rctl_get_racct(filter, strlen(filter) + 1, out, sizeof out))
        sayf(t, "racct of %s %s: errno %d (%s)", filter, when, errno, strerror(errno));
    else
        sayf(t, "racct of %s %s: %.1800s", filter, when, out);
    snprintf(filter, sizeof filter, "process:%ld:", t->rctl ? t->rctl : (long)getpid());
    out[0] = 0;
    if (rctl_get_rules(filter, strlen(filter) + 1, out, sizeof out))
        sayf(t, "rctl rules of %s %s: errno %d (%s)", filter, when, errno, strerror(errno));
    else
        sayf(t, "rctl rules of %s %s: \"%.1800s\"", filter, when, out);
#else
    sayf(t, "rctl=%ld %s: not in this process (a title has no rctl calls): use proc=payload", t->rctl, when);
#endif
}

static int openFile(const Test* t, int truncate)
{
    int flags = O_WRONLY | O_CREAT | (truncate ? O_TRUNC : 0) | (t->direct ? O_DIRECT : 0) | (t->osync ? O_SYNC : 0);
    return open(t->path, t->null ? O_WRONLY : flags, 0666);
}

// One pass over the file. 0: every byte of it was written.
static int writePass(const Test* t, unsigned pass, unsigned long long mb, uint8_t* chunk, size_t chunkBytes, double* seconds)
{
    const unsigned long long total = mb << 20, reportBytes = t->reportMB << 20, syncBytes = t->syncMB << 20, reopenBytes = t->reopenMB << 20;
    int fd = openFile(t, !t->overwrite);
    if (fd < 0) {
        sayf(t, "pass %u: open %s failed: errno %d (%s)", pass, t->path, errno, strerror(errno));
        return -1;
    }
    if (t->prealloc && !t->null && ftruncate(fd, (off_t)total))
        sayf(t, "pass %u: ftruncate to %llu MB failed: errno %d (%s)", pass, mb, errno, strerror(errno));
    const int syncing = strcmp(t->sync, "none") != 0;
    unsigned long long written = 0, reported = 0, synced = 0, reopened = 0;
    double start = now(), intervalStart = start, writeTime = 0, longest = 0, syncTime = 0, slowestRate = 0, fastestRate = 0;
    unsigned calls = 0, shortWrites = 0;
    Usage before = usage();
    int result = 0;
    while (written < total) {
        size_t want = total - written < chunkBytes ? (size_t)(total - written) : chunkBytes;
        memcpy(chunk, &written, sizeof written); // no two chunks alike
        double t0 = now();
        ssize_t n = t->usePwrite ? pwrite(fd, chunk, want, (off_t)written) : write(fd, chunk, want);
        double took = now() - t0;
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            sayf(t, "pass %u: write of %zu bytes at %llu MB failed: %zd, errno %d (%s)", pass, want, written >> 20, n, errno, strerror(errno));
            result = -1;
            break;
        }
        shortWrites += (size_t)n < want;
        written += (unsigned long long)n;
        writeTime += took;
        longest = took > longest ? took : longest;
        calls++;
        if (syncing && (written - synced >= syncBytes || written == total)) {
            t0 = now();
            int rc = !strcmp(t->sync, "fsync") ? fsync(fd) : fdatasync(fd);
            syncTime += now() - t0;
            if (rc)
                sayf(t, "pass %u: %s at %llu MB failed: errno %d (%s)", pass, t->sync, written >> 20, errno, strerror(errno));
            synced = written;
        }
        if (t->fadvise && (synced == written || written - reported >= reportBytes || written == total)) {
#ifdef PS5WK_WRITETEST_SYSCALLS
            int rc = posix_fadvise(fd, 0, (off_t)written, POSIX_FADV_DONTNEED);
            if (rc && written <= chunkBytes)
                sayf(t, "pass %u: posix_fadvise(DONTNEED) failed: %d (%s)", pass, rc, strerror(rc));
#else
            if (written <= chunkBytes)
                sayf(t, "pass %u: fadvise=1 ignored: a title has no posix_fadvise (use proc=payload)", pass);
#endif
        }
        if (reopenBytes && written - reopened >= reopenBytes && written < total) {
            close(fd);
            fd = openFile(t, 0);
            if (fd < 0) {
                sayf(t, "pass %u: opening %s again at %llu MB failed: errno %d (%s)", pass, t->path, written >> 20, errno, strerror(errno));
                result = -1;
                break;
            }
            if (!t->usePwrite)
                lseek(fd, (off_t)written, SEEK_SET);
            reopened = written;
        }
        if (written - reported >= reportBytes || written == total) {
            double end = now(), interval = end - intervalStart;
            Usage after = usage();
            double rate = (double)(written - reported) / 1048576.0 / (interval > 0 ? interval : 1e-9);
            slowestRate = !slowestRate || rate < slowestRate ? rate : slowestRate;
            fastestRate = rate > fastestRate ? rate : fastestRate;
            char vfs[400];
            vfsState(t, vfs, sizeof vfs, 0);
            sayf(t, "pass %u: %5llu MB: %7.1f MB/s (%llu MB in %.3f s: write() %.3f s in %u calls, longest %.3f s; %s %.3f s); %s CPU %.3f s, "
                "%ld block writes, %ld waits, %ld preempted%s%s",
                pass, written >> 20, rate, (written - reported) >> 20, interval, writeTime, calls, longest, t->sync, syncTime,
                after.who == RUSAGE_THREAD ? "thread" : "process", after.cpu - before.cpu, after.blocksOut - before.blocksOut,
                after.voluntary - before.voluntary, after.involuntary - before.involuntary, *vfs ? "; " : "", vfs);
            reported = written;
            intervalStart = now();
            writeTime = longest = syncTime = 0;
            calls = 0;
            before = usage();
        }
        if (t->paceMS)
            usleep(t->paceMS * 1000);
    }
    double t0 = now();
    int rc = close(fd);
    double closeTime = now() - t0;
    *seconds = now() - start;
    struct stat st;
    memset(&st, 0, sizeof st);
    int statted = !stat(t->path, &st);
    sayf(t, "pass %u %s: %llu of %llu MB in %.1f s, %.1f MB/s overall (fastest %.1f, slowest %.1f per %llu MB); close %.3f s%s; %u short writes; "
        "file %lld bytes, %lld blocks of 512%s",
        pass, result ? "FAILED" : "done", written >> 20, mb, *seconds, (double)written / 1048576.0 / (*seconds > 0 ? *seconds : 1e-9),
        fastestRate, slowestRate, t->reportMB, closeTime, rc ? " (failed)" : "", shortWrites, statted ? (long long)st.st_size : -1LL,
        statted ? (long long)st.st_blocks : -1LL, t->null ? " (/dev/null)" : "");
    return result;
}

static void readBack(const Test* t, uint8_t* chunk, size_t chunkBytes)
{
    int fd = open(t->path, O_RDONLY | (t->direct ? O_DIRECT : 0));
    if (fd < 0) {
        sayf(t, "read back: open failed: errno %d (%s)", errno, strerror(errno));
        return;
    }
    unsigned long long total = 0;
    double start = now();
    for (ssize_t n; (n = read(fd, chunk, chunkBytes)) > 0;)
        total += (unsigned long long)n;
    double seconds = now() - start;
    close(fd);
    sayf(t, "read back: %llu MB in %.1f s, %.1f MB/s", total >> 20, seconds, (double)total / 1048576.0 / (seconds > 0 ? seconds : 1e-9));
}

// The gap tests (gap=, at the top).
static const char markerText[12] = { 'P', 'S', '5', 'W', 'K', '-', 'M', 'A', 'R', 'K', 'E', 'R' };

// at < 0: write() at the descriptor's offset.
static int writeAll(int fd, const uint8_t* p, size_t n, long long at)
{
    while (n) {
        ssize_t w = at >= 0 ? pwrite(fd, p, n, (off_t)at) : write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
        if (at >= 0)
            at += w;
    }
    return 0;
}

static void gapMarker(const Test* t, uint8_t* buffer, size_t bufferBytes)
{
    char path[512];
    snprintf(path, sizeof path, "%s/" FILE_NAME "-marker.bin", t->dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        sayf(t, "gap marker: open %s failed: errno %d (%s)", path, errno, strerror(errno));
        return;
    }
    const unsigned long long total = t->markerMB << 20;
    unsigned long long written = 0;
    uint32_t record = 0;
    double start = now();
    int failed = 0;
    while (written < total) {
        size_t n = bufferBytes < total - written ? bufferBytes : (size_t)(total - written);
        for (size_t i = 0; i + 16 <= n; i += 16, record++) {
            memcpy(buffer + i, markerText, sizeof markerText);
            memcpy(buffer + i + 12, &record, 4);
        }
        if (writeAll(fd, buffer, n, -1)) {
            failed = 1;
            break;
        }
        written += n;
    }
    // sync=fsync: on the disk first. Without it the file is deleted before
    // its blocks are committed, and they are free again at once (with it,
    // later: the next files don't get them).
    const int syncing = strcmp(t->sync, "none") != 0;
    int synced = !syncing || !fsync(fd);
    close(fd);
    int removed = !unlink(path);
    sayf(t, "gap marker: %llu MB of \"PS5WK-MARKER\" records%s to %s in %.1f s, fsync %s, %s", written >> 20, failed ? " (the write FAILED)" : "", path,
        now() - start, !syncing ? "not asked" : synced ? "ok" : "FAILED", removed ? "deleted: its blocks are free now" : "NOT deleted");
}

typedef struct {
    unsigned long long length, nonZero, markerSlots, markerBytes, own, other;
    long long first;
    uint8_t firstBytes[8];
    char blocks[160];
} GapCount;

// Counts the bytes of data (read at fileOffset) that are not zero: in
// 16-byte records of the marker (as they lie in a file and so in a disk
// block), the test's own 'H' and 'W', or anything else.
static void gapCount(const uint8_t* data, size_t length, unsigned long long fileOffset, GapCount* c)
{
    unsigned long long block = ~0ull, inBlock = 0;
    size_t o = strlen(c->blocks);
    for (size_t i = 0; i < length;) {
        size_t slotEnd = (size_t)(((fileOffset + i) | 15) + 1 - fileOffset);
        if (slotEnd > length)
            slotEnd = length;
        int marker = slotEnd - i == 16 && !((fileOffset + i) & 15) && !memcmp(data + i, markerText, sizeof markerText);
        c->markerSlots += marker;
        for (; i < slotEnd; i++) {
            if (!data[i])
                continue;
            unsigned long long b = (fileOffset + i) >> 16;
            if (b != block) {
                if (inBlock && o + 24 < sizeof c->blocks)
                    o += (size_t)snprintf(c->blocks + o, sizeof c->blocks - o, "%s%llu:%llu", o ? " " : "", block, inBlock);
                block = b;
                inBlock = 0;
            }
            inBlock++;
            if (!c->nonZero++) {
                c->first = (long long)(fileOffset + i);
                memcpy(c->firstBytes, data + i, length - i < 8 ? length - i : 8);
            }
            if (marker)
                c->markerBytes++;
            else if (data[i] == 'H' || data[i] == 'W')
                c->own++;
            else
                c->other++;
        }
    }
    if (inBlock && o + 24 < sizeof c->blocks)
        snprintf(c->blocks + o, sizeof c->blocks - o, "%s%llu:%llu", o ? " " : "", block, inBlock);
    c->length += length;
}

static int gapRead(int fd, uint8_t* buffer, unsigned long long from, unsigned long long to, GapCount* c)
{
    while (from < to) {
        size_t want = (size_t)(to - from);
        ssize_t n = pread(fd, buffer, want, (off_t)from);
        if (n <= 0)
            return -1;
        gapCount(buffer, (size_t)n, from, c);
        from += (unsigned long long)n;
    }
    return 0;
}

static void gapDescribe(const GapCount* c, char* out, size_t size)
{
    if (!c->nonZero) {
        snprintf(out, size, "all %llu bytes zero", c->length);
        return;
    }
    snprintf(out, size, "%llu of %llu bytes NOT zero, from %lld (per 64 KB block: %s): marker %llu bytes in %llu records, the test's own H/W %llu, other %llu; "
        "the first %02x %02x %02x %02x %02x %02x %02x %02x",
        c->nonZero, c->length, c->first, c->blocks, c->markerBytes, c->markerSlots, c->own, c->other, c->firstBytes[0], c->firstBytes[1], c->firstBytes[2],
        c->firstBytes[3], c->firstBytes[4], c->firstBytes[5], c->firstBytes[6], c->firstBytes[7]);
}

enum { GapSeekWrite, GapPwrite, GapSecondDescriptor, GapAfterFsync, GapTruncate, GapTruncateThenSmall, GapOwnMarker, GapHows };
static const char* const gapHowNames[] = { "lseek+write", "pwrite", "pwrite from a 2nd descriptor", "fsync+close, then pwrite", "ftruncate",
    "ftruncate, then 10 bytes into the hole", "8 MB of marker (fsync with sync=), ftruncate(0), the head, then pwrite" };

// One file: head bytes of 'H', then the gap made as how says, read back
// through the same descriptor and through a new one. 1: some gap byte is not
// zero.
static int gapCase(const Test* t, unsigned pass, int how, unsigned long long head, unsigned long long at, uint8_t* buffer, uint8_t* readBuffer,
    unsigned* markerCases)
{
    static const size_t writeBytes = 65536, smallBytes = 10, smallAt = 100;
    char path[512];
    snprintf(path, sizeof path, "%s/" FILE_NAME "-gap%s.bin", t->dir, t->tag);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        sayf(t, "gap: open %s failed: errno %d (%s)", path, errno, strerror(errno));
        return 0;
    }
    int failed = 0;
    if (how == GapOwnMarker) {
        // The file's own earlier bytes where the gap will be: marker records
        // over its first 8 MB, on the disk (fsync), then freed.
        for (size_t i = 0; i + 16 <= (8u << 20); i += 16) {
            uint32_t record = (uint32_t)(i / 16);
            memcpy(buffer + i, markerText, sizeof markerText);
            memcpy(buffer + i + 12, &record, 4);
        }
        failed = writeAll(fd, buffer, 8u << 20, -1) || (strcmp(t->sync, "none") && fsync(fd)) || ftruncate(fd, 0) || lseek(fd, 0, SEEK_SET) != 0;
    }
    memset(buffer, 'H', (size_t)head);
    failed = failed || writeAll(fd, buffer, (size_t)head, -1);
    memset(buffer, 'W', writeBytes);
    unsigned long long end = at + writeBytes;
    if (!failed) {
        switch (how) {
        case GapSeekWrite:
            failed = lseek(fd, (off_t)at, SEEK_SET) != (off_t)at || writeAll(fd, buffer, writeBytes, -1);
            break;
        case GapPwrite:
        case GapOwnMarker:
            failed = writeAll(fd, buffer, writeBytes, (long long)at);
            break;
        case GapSecondDescriptor: {
            int second = open(path, O_RDWR);
            failed = second < 0 || writeAll(second, buffer, writeBytes, (long long)at);
            if (second >= 0)
                close(second);
            break;
        }
        case GapAfterFsync:
            failed = fsync(fd);
            close(fd);
            fd = open(path, O_RDWR);
            failed = failed || fd < 0 || writeAll(fd, buffer, writeBytes, (long long)at);
            break;
        case GapTruncate:
            failed = ftruncate(fd, (off_t)end);
            break;
        case GapTruncateThenSmall:
            failed = ftruncate(fd, (off_t)end) || writeAll(fd, buffer, smallBytes, (long long)(at + smallAt));
            break;
        }
    }
    if (failed || fd < 0) {
        sayf(t, "gap pass %u, %s, head %llu, at %llu: a call FAILED: errno %d (%s)", pass, gapHowNames[how], head, at, errno, strerror(errno));
        if (fd >= 0)
            close(fd);
        unlink(path);
        return 0;
    }
    // What was never written: [head, at), or for an ftruncate [head, end)
    // less the small write.
    GapCount counts[2];
    for (int round = 0; round < 2; round++) {
        GapCount* c = &counts[round];
        memset(c, 0, sizeof *c);
        if (round) {
            close(fd);
            fd = open(path, O_RDONLY);
            if (fd < 0)
                break;
        }
        if (how == GapTruncate)
            gapRead(fd, readBuffer, head, end, c);
        else if (how == GapTruncateThenSmall) {
            gapRead(fd, readBuffer, head, at + smallAt, c);
            gapRead(fd, readBuffer, at + smallAt + smallBytes, end, c);
        } else
            gapRead(fd, readBuffer, head, at, c);
    }
    struct stat st;
    memset(&st, 0, sizeof st);
    fstat(fd, &st);
    if (fd >= 0)
        close(fd);
    unlink(path);
    char first[700], second[400];
    gapDescribe(&counts[0], first, sizeof first);
    if (counts[1].nonZero == counts[0].nonZero && counts[1].first == counts[0].first && !memcmp(counts[1].firstBytes, counts[0].firstBytes, 8))
        snprintf(second, sizeof second, "the same through a new descriptor");
    else
        gapDescribe(&counts[1], second, sizeof second);
    sayf(t, "gap pass %u, %s, head %llu, at %llu (file %lld bytes, %lld blocks of 512): %s; %s", pass, gapHowNames[how], head, at, (long long)st.st_size,
        (long long)st.st_blocks, first, second);
    if (counts[0].markerSlots || counts[1].markerSlots)
        (*markerCases)++;
    return counts[0].nonZero || counts[1].nonZero;
}

static void gapMtime(const Test* t, uint8_t* buffer)
{
    static const char* const names[] = { "1 MB write, fsync, close", "1 MB write, close", "ftruncate to 1 MB, 1 MB pwrite at 4 MB, fsync, close" };
    char path[512];
    snprintf(path, sizeof path, "%s/" FILE_NAME "-mtime.bin", t->dir);
    memset(buffer, 'M', 1 << 20);
    for (int variant = 0; variant < 3; variant++) {
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            sayf(t, "gap mtime: open %s failed: errno %d (%s)", path, errno, strerror(errno));
            return;
        }
        int failed = writeAll(fd, buffer, 1 << 20, -1);
        if (variant == 2)
            failed = failed || ftruncate(fd, 1 << 20) || writeAll(fd, buffer, 1 << 20, 4 << 20);
        if (variant != 1)
            failed = failed || fsync(fd);
        close(fd);
        double closed = now();
        struct stat last, st;
        memset(&last, 0, sizeof last);
        stat(path, &last);
        sayf(t, "gap mtime, %s%s: right after: mtime %lld.%09ld, ctime %lld.%09ld, size %lld", names[variant], failed ? " (a call FAILED)" : "",
            (long long)last.st_mtim.tv_sec, (long)last.st_mtim.tv_nsec, (long long)last.st_ctim.tv_sec, (long)last.st_ctim.tv_nsec, (long long)last.st_size);
        unsigned changes = 0, secondChanges = 0;
        double lastChange = 0;
        while (now() - closed < 3.0) {
            usleep(10000);
            if (stat(path, &st))
                continue;
            if (st.st_mtim.tv_sec == last.st_mtim.tv_sec && st.st_mtim.tv_nsec == last.st_mtim.tv_nsec && st.st_size == last.st_size
                && st.st_ctim.tv_sec == last.st_ctim.tv_sec && st.st_ctim.tv_nsec == last.st_ctim.tv_nsec)
                continue;
            changes++;
            secondChanges += st.st_mtim.tv_sec != last.st_mtim.tv_sec;
            lastChange = now() - closed;
            if (changes <= 6)
                sayf(t, "gap mtime, %s: +%.3f s: mtime %lld.%09ld, ctime %lld.%09ld, size %lld", names[variant], lastChange, (long long)st.st_mtim.tv_sec,
                    (long)st.st_mtim.tv_nsec, (long long)st.st_ctim.tv_sec, (long)st.st_ctim.tv_nsec, (long long)st.st_size);
            last = st;
        }
        sayf(t, "gap mtime, %s: %u changes in 3 s after close (%u of the second), the last at +%.3f s", names[variant], changes, secondChanges, lastChange);
        unlink(path);
    }
}

static int gapTests(Test* t)
{
    static const size_t bufferBytes = 8 << 20;
    uint8_t* buffer = mmap(NULL, bufferBytes * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (buffer == MAP_FAILED) {
        sayf(t, "gap: FAILED: no buffer (errno %d)", errno);
        return -1;
    }
    sayf(t, "gap start: gap=%s in %s, marker %llu MB, %u pass(es); uid %d", t->gap, t->dir, t->markerMB, t->passes, (int)getuid());
    if (t->markerMB)
        gapMarker(t, buffer, bufferBytes);
    if (t->markerMB && t->sleepS) {
        sayf(t, "gap: sleeping %u s", t->sleepS);
        sleep(t->sleepS);
    }
    unsigned cases = 0, dirty = 0, markerCases = 0;
    if (!strcmp(t->gap, "mtime"))
        gapMtime(t, buffer);
    else if (strcmp(t->gap, "marker")) {
        // head, at: the page's (opfs-sync-rw.html: the file ended at 2 MB +
        // 400 KB, the write began at 3 MB + 5), a small file, an end on a
        // block's boundary, and a gap inside one block.
        static const unsigned long long shapes[][2] = { { 2506752, 3145733 }, { 16384, 1048581 }, { 65536, 1048581 }, { 2506752, 2506852 } };
        for (unsigned pass = 1; pass <= t->passes; pass++) {
            for (unsigned s = 0; s < sizeof shapes / sizeof shapes[0]; s++) {
                for (int how = 0; how < GapHows; how++) {
                    cases++;
                    dirty += (unsigned)gapCase(t, pass, how, shapes[s][0], shapes[s][1], buffer, buffer + bufferBytes, &markerCases);
                }
            }
        }
    }
    munmap(buffer, bufferBytes * 2);
    sayf(t, "END gap: %u case(s), %u with bytes that are not zero, %u of them with the marker", cases, dirty, markerCases);
    return 0;
}

int ps5wk_write_test(const char* spec, const char* process, PS5WKWriteTestSay say, void* data)
{
    Test test;
    memset(&test, 0, sizeof test);
    Test* t = &test;
    t->say = say;
    t->data = data;
    t->process = process;
    parse(t, spec ? spec : "");
    const size_t chunkBytes = (size_t)t->chunkKB << 10;
    if (t->uid >= 0) {
#ifdef PS5WK_WRITETEST_SYSCALLS
        int rc = setuid((uid_t)t->uid);
        sayf(t, "setuid(%ld): %d (errno %d), now uid %d, euid %d", t->uid, rc, rc ? errno : 0, (int)getuid(), (int)geteuid());
#else
        sayf(t, "uid=%ld ignored: only a payload changes its uid (use proc=payload)", t->uid);
#endif
    }
    sayf(t, "start: %s, %llu MB x %u (then %llu MB) in %llu KB %s, a line per %llu MB; uid %d; open%s%s%s%s; sync %s every %llu MB%s; reopen every %llu MB; "
        "pace %u ms; between passes %s, sleep %u s; (%s)",
        t->path, t->mb, t->passes, t->mb2, t->chunkKB, t->usePwrite ? "pwrite()s" : "write()s", t->reportMB, (int)getuid(),
        t->overwrite ? "" : " O_TRUNC", t->direct ? " O_DIRECT" : "", t->osync ? " O_SYNC" : "", t->prealloc ? ", ftruncate first" : "", t->sync,
        t->syncMB, t->fadvise ? ", fadvise(DONTNEED)" : "", t->reopenMB, t->paceMS, t->between, t->sleepS, spec ? spec : "");
    if (!t->null) {
        struct statfs fs;
        memset(&fs, 0, sizeof fs);
        if (statfs(t->dir, &fs))
            sayf(t, "statfs %s: errno %d (%s)", t->dir, errno, strerror(errno));
        else
            sayf(t, "file system of %s: %s on %s from %s, flags 0x%llx, block %llu, I/O size %llu, %llu MB free of %llu", t->dir, fs.f_fstypename,
                fs.f_mntonname, fs.f_mntfromname, (unsigned long long)fs.f_flags, (unsigned long long)fs.f_bsize, (unsigned long long)fs.f_iosize,
                (unsigned long long)fs.f_bavail * fs.f_bsize >> 20, (unsigned long long)fs.f_blocks * fs.f_bsize >> 20);
    }
    if (t->gap[0] && !t->null)
        return gapTests(t);
    char vfs[900];
    if (t->vfs && !vfsState(t, vfs, sizeof vfs, 1)) {
        sayf(t, "buffer cache: no vfs.* sysctl can be read in this process (%.200s, ...): left out of the lines", vfs);
        t->vfs = 0;
    } else if (t->vfs) {
        sayf(t, "buffer cache: %s", vfs);
        if (!vfsState(t, vfs, sizeof vfs, 0))
            t->vfs = 0; // none of the counters the lines would carry
    }
    rctlState(t, "before");
    // Page-aligned (O_DIRECT), not from malloc: a title's anonymous mappings
    // are direct memory (titlemem.c).
    uint8_t* chunk = mmap(NULL, chunkBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (chunk == MAP_FAILED) {
        sayf(t, "FAILED: no %llu KB for the chunk (errno %d)", t->chunkKB, errno);
        return -1;
    }
    uint64_t x = 0x9e3779b97f4a7c15ull; // not zeros: nothing to compress or skip
    for (size_t i = 0; i + 8 <= chunkBytes; i += 8) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        memcpy(chunk + i, &x, 8);
    }
    int result = 0;
    double all = 0;
    for (unsigned pass = 1; pass <= t->passes; pass++) {
        double seconds = 0;
        if (writePass(t, pass, pass == 1 ? t->mb : t->mb2, chunk, chunkBytes, &seconds))
            result = -1;
        all += seconds;
        if (pass == t->passes)
            break;
        if (!strcmp(t->between, "read"))
            readBack(t, chunk, chunkBytes);
        else if (!strcmp(t->between, "delete") && !t->null) {
            double t0 = now();
            int rc = unlink(t->path);
            sayf(t, "between passes: unlink %s in %.3f s", rc ? "FAILED" : "done", now() - t0);
        } else if (!strcmp(t->between, "sync") && !t->null) {
            int fd = open(t->path, O_WRONLY);
            double t0 = now();
            int rc = fd >= 0 ? fsync(fd) : -1;
            sayf(t, "between passes: fsync %s in %.3f s", rc ? "FAILED" : "done", now() - t0);
            if (fd >= 0)
                close(fd);
        }
        if (t->sleepS) {
            sayf(t, "between passes: sleeping %u s", t->sleepS);
            sleep(t->sleepS);
        }
    }
    rctlState(t, "after");
    vfsState(t, vfs, sizeof vfs, 0);
    if (!t->keep && !t->null) {
        double t0 = now();
        int rc = unlink(t->path);
        sayf(t, "unlink %s in %.3f s", rc ? "FAILED" : "done", now() - t0);
    }
    munmap(chunk, chunkBytes);
    sayf(t, "END %s: %llu MB x %u in %.1f s%s%s", result ? "FAILED" : "ok", t->mb, t->passes, all, *vfs ? "; " : "", vfs);
    return result;
}

double ps5wk_write_test_probe(const char* spec)
{
    Test test;
    memset(&test, 0, sizeof test);
    parse(&test, spec ? spec : "");
    if (test.null)
        snprintf(test.dir, sizeof test.dir, "/data/ps5webkit");
    char temporary[512], path[512], block[4096];
    snprintf(path, sizeof path, "%s/" FILE_NAME "-probe.txt", test.dir);
    snprintf(temporary, sizeof temporary, "%s.tmp", path);
    memset(block, 'p', sizeof block);
    double start = now();
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return -1;
    int ok = write(fd, block, sizeof block) == (ssize_t)sizeof block;
    ok = !fsync(fd) && ok;
    ok = !close(fd) && ok;
    ok = !rename(temporary, path) && ok;
    double seconds = now() - start;
    unlink(path);
    return ok ? seconds : -1;
}
#endif
