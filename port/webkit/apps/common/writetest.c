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
    "pwrite", "reopen", "prealloc", "overwrite", "pace", "passes", "mb2", "between", "sleep", "keep", "vfs", "rctl", "uid", "thread", NULL };

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
    char sync[16], between[16];
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
