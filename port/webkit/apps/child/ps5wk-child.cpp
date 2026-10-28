/*
 * ps5wk-child: WebKit's WebProcess and NetworkProcess as one PS5 payload.
 *
 * The UIProcess (the browser title, or the ps5wk-shot payload) sends this ELF
 * to the local elfldr followed by a header line, and hands over the IPC
 * socket through the reversed rendezvous (wpe-ps5/PS5LaunchProtocol.h).
 * Then this calls WebKit's process entry point with the argv WebKit's
 * launcher would have used: "<name> <process identifier> <ipc fd>".
 *
 * stdout/stderr are the elfldr connection, i.e. they go back to whoever
 * launched us (the title relays them to its TCP log).
 */
#include "PS5LaunchProtocol.h"
#include "PS5VideoProtocol.h"
#include "../common/crashguard.h"
#include "../common/profiler.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace WebKit {
// Source/WebKit/WebProcess/WebProcessMain.h, NetworkProcess/NetworkProcessMain.h
int WebProcessMain(int argc, char** argv);
int NetworkProcessMain(int argc, char** argv);
}

extern "C" {
int sceKernelAvailableFlexibleMemorySize(size_t*) __attribute__((weak));
// glib-networking's OpenSSL TLS backend, linked statically (GIO module
// registration that a shared build would do by scanning GIO_MODULE_DIR).
void g_io_openssl_load(void*);
// libpas (bmalloc's heap): bytes it has mapped (pas_page_malloc), and of
// those, the bytes decommitted and not used again (patch 0031: whole 16 KiB
// pages given back to the kernel; 0 with PS5WK_PAS_DECOMMIT=0).
extern size_t pas_page_malloc_num_allocated_bytes __attribute__((weak));
extern intptr_t pas_page_malloc_ps5_decommitted_bytes __attribute__((weak));
extern size_t pas_compact_heap_reservation_size __attribute__((weak));
extern size_t pas_compact_heap_reservation_bump __attribute__((weak));
// JSC's executable memory pool (WebKit patch 0030; absent without the JIT).
void ps5wk_jit_memory(void** start, void** end, size_t* allocated, size_t* committed) __attribute__((weak));
}

static double nowMS()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

#define LOG(...) do { fprintf(stderr, "[ps5wk-child %d] ", (int)getpid()); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

// The header line elfldr leaves on stdin after the ELF.
static bool readHeader(char* line, size_t size, int timeoutMS)
{
    size_t used = 0;
    double end = nowMS() + timeoutMS;
    while (used + 1 < size) {
        int left = static_cast<int>(end - nowMS());
        struct pollfd p { 0, POLLIN, 0 };
        if (left <= 0 || poll(&p, 1, left) != 1)
            return false;
        char c;
        ssize_t n = read(0, &c, 1);
        if (n <= 0)
            return false;
        if (c == '\n')
            break;
        line[used++] = c;
    }
    line[used] = 0;
    return true;
}

static int rendezvous(const char* path, unsigned long long processIdentifier)
{
    mkdir(PS5WK_LAUNCH_DIR, 0777);
    chmod(PS5WK_LAUNCH_DIR, 0777); // the title runs under another uid

    struct sockaddr_un address { };
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    int s = -1;
    double start = nowMS();
    while (nowMS() - start < 60000) {
        s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s >= 0 && !connect(s, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)))
            break;
        if (s >= 0)
            close(s);
        s = -1;
        usleep(50 * 1000);
    }
    if (s < 0) {
        LOG("rendezvous: cannot connect to %s (errno %d)", path, errno);
        return -1;
    }

    ps5wk_hello hello { PS5WK_HELLO_MAGIC, processIdentifier, static_cast<int32_t>(getpid()), PS5WK_LAUNCH_VERSION };
    if (send(s, &hello, sizeof(hello), 0) != sizeof(hello)) {
        LOG("rendezvous: hello failed (errno %d)", errno);
        close(s);
        return -1;
    }

    ps5wk_welcome welcome { };
    struct iovec iov { &welcome, sizeof(welcome) };
    char control[CMSG_SPACE(sizeof(int))] { };
    struct msghdr message { };
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    ssize_t n = recvmsg(s, &message, MSG_WAITALL);
    int fd = -1;
    for (struct cmsghdr* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS)
            memcpy(&fd, CMSG_DATA(header), sizeof(int));
    }
    close(s);
    if (n != sizeof(welcome) || welcome.magic != PS5WK_WELCOME_MAGIC || welcome.status || fd < 0) {
        LOG("rendezvous: no IPC fd (n %zd, status %d, fd %d, errno %d)", n, welcome.status, fd, errno);
        if (fd >= 0)
            close(fd);
        return -1;
    }
    LOG("rendezvous: connected to %s after %.0f ms, IPC fd %d", path, nowMS() - start, fd);
    return fd;
}

static void setupEnvironment(const char* launchPath)
{
    // elfldr payloads start with an empty environment.
    mkdir("/data/ps5webkit", 0777);
    mkdir("/data/ps5webkit/home", 0777);
    setenv("HOME", "/data/ps5webkit/home", 0);
    setenv("XDG_CACHE_HOME", "/data/ps5webkit/home/cache", 0);
    setenv("XDG_DATA_HOME", "/data/ps5webkit/home/data", 0);
    setenv("XDG_CONFIG_HOME", "/data/ps5webkit/home/config", 0);
    setenv("XDG_RUNTIME_DIR", "/system_tmp/ps5wk", 0);
    setenv("TMPDIR", "/system_tmp/ps5wk", 0);
    setenv("GIO_USE_VFS", "local", 0);
    setenv("GSETTINGS_BACKEND", "memory", 0);
    setenv("TZ", "UTC", 0);
    setenv("ICU_DATA", "/data/ps5webkit/share/icu", 0); // icudt76l.dat (ICU stub data linked)
    // MIME types by file extension (share/mime/globs2; no shared-mime-info on
    // the console): file:// pages, MIMETypeRegistry, GIO content types.
    setenv("XDG_DATA_DIRS", "/data/ps5webkit/share", 0);
    // Media: WEBKIT_GST_VIDEO_DECODING_LIMIT (what MSE/canPlayType accept)
    // is set in main() once the title says whether it decodes in hardware.
    // GST_DEBUG etc. can be added through child.env.
    // Progressive media is buffered on disk (GStreamer downloadbuffer); the
    // default /var/tmp does not exist for payloads.
    mkdir("/data/ps5webkit/home/cache", 0777);
    mkdir("/data/ps5webkit/home/cache/media", 0777);
    setenv("WPE_SHELL_MEDIA_DISK_CACHE_PATH", "/data/ps5webkit/home/cache/media", 0);
    // Optional overrides for debugging, one KEY=VALUE per line (e.g.
    // WEBKIT_DEBUG=Network,Loading,IPC). Read last, so they win:
    // child.env for every child, then child-<UIProcess pid>.env for the
    // children of one UIProcess only (ps5wk-shot childenv=..., so harness
    // runs don't touch the browser title's children).
    auto readEnvFile = [](const char* file) {
        FILE* f = fopen(file, "r");
        if (!f)
            return;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            char* eq = strchr(line, '=');
            if (line[0] == '#' || !eq)
                continue;
            *eq = 0;
            setenv(line, eq + 1, 1);
            LOG("env %s=%s (%s)", line, eq + 1, file);
        }
        fclose(f);
    };
    readEnvFile("/data/ps5webkit/child.env");
    int uiPid = 0;
    if (const char* slash = strrchr(launchPath, '/'); slash && sscanf(slash + 1, "ui-%d.sock", &uiPid) == 1) {
        char file[64];
        snprintf(file, sizeof(file), "/data/ps5webkit/child-%d.env", uiPid);
        readEnvFile(file);
    }
}

// JavaScriptCore's JIT, opt-in on PS5 (WebKit patch 0030: a title can't
// execute what it maps RWX; payloads can). On for WebProcesses unless
// PS5WK_JIT says otherwise (child.env, or ps5wk-shot childenv=):
//   PS5WK_JIT=0         LLInt only (the interpreter, as before the JIT)
//   PS5WK_JIT=baseline  LLInt + Baseline JIT (+ the regexp JIT)
//   PS5WK_JIT=dfg       + DFG
//   PS5WK_JIT=1 / ftl   + FTL (all tiers, the default)
// JSC_* variables (JSC options, e.g. JSC_useFTLJIT=false) in child.env win.
static void configureJIT()
{
    const char* knob = getenv("PS5WK_JIT");
    const char* mode = knob && *knob ? knob : "1";
    bool off = !strcmp(mode, "0") || !strcmp(mode, "off");
    setenv("JSC_useJIT", off ? "false" : "true", 0);
    if (!strcmp(mode, "baseline"))
        setenv("JSC_useDFGJIT", "false", 0);
    if (!strcmp(mode, "baseline") || !strcmp(mode, "dfg"))
        setenv("JSC_useFTLJIT", "false", 0);
    // One compiler thread per tier (JSC's default is up to 4 on 16 CPUs):
    // YouTube's heap at 20 s was 772 MB instead of 885 MB (libpas never
    // returns pages on PS5), no measurable slowdown.
    setenv("JSC_numberOfBaselineCompilerThreads", "1", 0);
    setenv("JSC_numberOfDFGCompilerThreads", "1", 0);
    setenv("JSC_numberOfFTLCompilerThreads", "1", 0);
    const char* dfg = getenv("JSC_useDFGJIT");
    const char* ftl = getenv("JSC_useFTLJIT");
    LOG("JIT: %s (PS5WK_JIT=%s; JSC_useJIT=%s%s%s%s%s)", off ? "off, LLInt only" : "on", mode, getenv("JSC_useJIT"),
        dfg ? ", JSC_useDFGJIT=" : "", dfg ? dfg : "", ftl ? ", JSC_useFTLJIT=" : "", ftl ? ftl : "");
}

// Asks the title's video server what it decodes in hardware (HELLO with
// the QUERY role; wpe-ps5/PS5VideoProtocol.h).
static void queryTitleDecoder(unsigned& codecs, int& maxWidth, int& maxHeight)
{
    codecs = 0;
    maxWidth = maxHeight = 0;
    const char* path = getenv("PS5WK_VIDEO_SOCKET");
    const char* knob = getenv("PS5WK_HWDEC");
    if (!path || (knob && !strcmp(knob, "0")))
        return;
    struct sockaddr_un address { };
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0)
        return;
    if (!connect(s, reinterpret_cast<struct sockaddr*>(&address), sizeof(address))) {
        ps5wk_video_msg msg { };
        msg.type = PS5WK_VMSG_HELLO;
        msg.magic = PS5WK_VIDEO_MAGIC;
        msg.version = PS5WK_VIDEO_VERSION;
        msg.pid = getpid();
        msg.role = PS5WK_VROLE_QUERY;
        if (send(s, &msg, sizeof(msg), 0) == sizeof(msg)) {
            struct pollfd p { s, POLLIN, 0 };
            if (poll(&p, 1, 2000) == 1 && recv(s, &msg, sizeof(msg), MSG_WAITALL) == sizeof(msg) && msg.type == PS5WK_VMSG_HELLO_ACK) {
                codecs = msg.role;
                maxWidth = static_cast<int>(msg.width);
                maxHeight = static_cast<int>(msg.height);
            }
        }
    }
    close(s);
}

struct MainArgs {
    bool network;
    int argc;
    char* argv[4];
    int result;
};

// CPU use of this process (user + system time per wall second, i.e. cores
// busy), logged every 5 s while it is above 0.3 cores: shows what software
// video decoding and painting cost (Milestone 2). PS5WK_CPU_LOG=0 turns it
// off, PS5WK_CPU_LOG=all logs every 5 s whatever the load.
static void* cpuLogger(void* data)
{
    const char* tag = static_cast<const char*>(data);
    struct rusage last;
    getrusage(RUSAGE_SELF, &last);
    double lastWall = nowMS();
    for (;;) {
        struct timespec ts { 5, 0 };
        nanosleep(&ts, nullptr);
        struct rusage now;
        getrusage(RUSAGE_SELF, &now);
        double wall = nowMS();
        auto ms = [](const struct timeval& tv) { return tv.tv_sec * 1e3 + tv.tv_usec / 1e3; };
        double user = ms(now.ru_utime) - ms(last.ru_utime), sys = ms(now.ru_stime) - ms(last.ru_stime);
        double cores = (user + sys) / (wall - lastWall);
        static const bool always = getenv("PS5WK_CPU_LOG") && !strcmp(getenv("PS5WK_CPU_LOG"), "all");
        if (cores > 0.3 || always)
            LOG("[cpu] %s: %.2f cores (user %.2f, sys %.2f) over %.1f s", tag, cores, user / (wall - lastWall), sys / (wall - lastWall), (wall - lastWall) / 1e3);
        last = now;
        lastWall = wall;
    }
    return nullptr;
}


// Every mmap/munmap of the payload goes through these (--wrap=mmap,munmap in
// apps/CMakeLists.txt): live mappings for the [mem] line, and one log line
// per failed mmap with its size, alignment, protection, flags, errno and
// caller (libpas, the JSC structure heap and glib all map here). The log is
// write(2) of a stack buffer: no stdio or malloc inside mmap.
extern "C" void* __real_mmap(void*, size_t, int, int, int, off_t);
extern "C" int __real_munmap(void*, size_t);
static std::atomic<uint64_t> mmapCalls, mmapFailures, munmapCalls;
static std::atomic<int64_t> mmapLiveBytes, mmapLiveCount;
static std::atomic<uint64_t> mmapPeakBytes, mmapLargest;

// Mappings of 1 MB or more (libpas's, the JIT pool, the structure heap...),
// for the resident count (mincore) in the [mem] line: lock-free slots.
#define BIG_MAPPINGS 1024
static std::atomic<uintptr_t> bigStart[BIG_MAPPINGS];
static size_t bigLength[BIG_MAPPINGS];

static void rememberBig(void* address, size_t length)
{
    for (int i = 0; i < BIG_MAPPINGS; i++) {
        uintptr_t expected = 0;
        bigLength[i] = length;
        if (bigStart[i].compare_exchange_strong(expected, (uintptr_t)address))
            return;
    }
}

static void forgetBig(void* address, size_t length)
{
    for (int i = 0; i < BIG_MAPPINGS; i++) {
        if (bigStart[i].load(std::memory_order_relaxed) == (uintptr_t)address && bigLength[i] == length) {
            bigStart[i].store(0);
            return;
        }
    }
}

// Resident bytes in the big mappings, by mincore over 1 MB pieces (pieces
// that are no longer mapped are skipped).
static double residentBigMB()
{
    static unsigned char vector[(1 << 20) / 4096];
    size_t page = getpagesize(), resident = 0;
    for (int i = 0; i < BIG_MAPPINGS; i++) {
        uintptr_t start = bigStart[i].load(std::memory_order_relaxed);
        if (!start)
            continue;
        size_t length = bigLength[i];
        for (size_t offset = 0; offset < length; offset += 1 << 20) {
            size_t piece = length - offset < (1u << 20) ? length - offset : 1u << 20;
            if (mincore((void*)(start + offset), piece, (char*)vector))
                continue;
            for (size_t k = 0; k < (piece + page - 1) / page; k++)
                resident += vector[k] & 1;
        }
    }
    return resident * (double)page / 1048576.0;
}

extern "C" void* __wrap_mmap(void* address, size_t length, int prot, int flags, int fd, off_t offset)
{
    void* result = __real_mmap(address, length, prot, flags, fd, offset);
    mmapCalls.fetch_add(1, std::memory_order_relaxed);
    if (result == MAP_FAILED) {
        int savedErrno = errno;
        uint64_t n = mmapFailures.fetch_add(1, std::memory_order_relaxed);
        if (n < 32) {
            char line[256];
            int len = snprintf(line, sizeof(line), "[ps5wk-child %d] [mem] mmap FAILED: %zu bytes (%.1f MB) at %p, align 2^%d, prot %#x, flags %#x, fd %d: errno %d (caller %p)\n",
                (int)getpid(), length, length / 1048576.0, address, (flags >> 24) & 0xff, prot, flags & 0xffffff, fd, savedErrno, __builtin_return_address(0));
            if (len > 0)
                write(2, line, len < (int)sizeof(line) ? len : (int)sizeof(line) - 1);
        }
        errno = savedErrno;
        return result;
    }
    if (!(flags & MAP_FIXED) || !address) {
        if (length >= (1u << 20))
            rememberBig(result, length);
        int64_t live = mmapLiveBytes.fetch_add(length, std::memory_order_relaxed) + length;
        mmapLiveCount.fetch_add(1, std::memory_order_relaxed);
        uint64_t peak = mmapPeakBytes.load(std::memory_order_relaxed);
        while (live > 0 && (uint64_t)live > peak && !mmapPeakBytes.compare_exchange_weak(peak, live, std::memory_order_relaxed)) { }
        uint64_t largest = mmapLargest.load(std::memory_order_relaxed);
        while (length > largest && !mmapLargest.compare_exchange_weak(largest, length, std::memory_order_relaxed)) { }
    }
    return result;
}

extern "C" int __wrap_munmap(void* address, size_t length)
{
    int result = __real_munmap(address, length);
    munmapCalls.fetch_add(1, std::memory_order_relaxed);
    if (!result && length >= (1u << 20))
        forgetBig(address, length);
    if (!result) {
        mmapLiveBytes.fetch_sub(length, std::memory_order_relaxed);
        mmapLiveCount.fetch_sub(1, std::memory_order_relaxed);
    }
    return result;
}

// Resident set size of this process (kern.proc.pid), or -1 (payloads get
// EPERM/ENOENT for it on 13.60; then only getrusage's peak is printed).
static long residentKB()
{
    struct kinfo_proc info;
    size_t size = sizeof(info);
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)getpid() };
    if (sysctl(mib, 4, &info, &size, nullptr, 0) || size < sizeof(info))
        return -1;
    return info.ki_rssize * (long)(getpagesize() / 1024);
}

// The memory watchdog: one [mem] line per minute (PS5WK_MEM_LOG=<seconds>
// changes the period, 0 turns it off), for watching long sessions.
static size_t startFlexible;
static void* memLogger(void* data)
{
    const char* tag = static_cast<const char*>(data);
    const char* period = getenv("PS5WK_MEM_LOG");
    int seconds = period ? atoi(period) : 60;
    double start = nowMS();
    // The memory limit (WebProcess): every 5 s, libpas's committed memory
    // (mapped minus given back) against PS5WK_MEM_RESTART_MB (default 3072, 0
    // = off), written to PS5WK_MEM_STATUS for the UIProcess, which restarts
    // this WebProcess at the next top-level navigation when it says "over 1".
    const char* statusPath = getenv("PS5WK_MEM_STATUS");
    const char* limitText = getenv("PS5WK_MEM_RESTART_MB");
    double limitMB = limitText ? atof(limitText) : 3072;
    bool over = false;
    for (unsigned tick = 1;; tick++) {
        struct timespec ts { 5, 0 };
        nanosleep(&ts, nullptr);
        double mapped = &pas_page_malloc_num_allocated_bytes ? pas_page_malloc_num_allocated_bytes / 1048576.0 : 0;
        double givenBack = &pas_page_malloc_ps5_decommitted_bytes && pas_page_malloc_ps5_decommitted_bytes > 0
            ? pas_page_malloc_ps5_decommitted_bytes / 1048576.0 : 0;
        double committed = mapped > givenBack ? mapped - givenBack : 0;
        if (statusPath) {
            bool nowOver = limitMB > 0 && committed >= limitMB;
            if (nowOver && !over)
                LOG("[mem] %s: libpas committed %.0f MB, at or above the limit of %.0f MB (PS5WK_MEM_RESTART_MB): the browser restarts this WebProcess at the next navigation",
                    tag, committed, limitMB);
            over = nowOver;
            char line[160];
            int length = snprintf(line, sizeof line, "pid %d committed %.0f limit %.0f over %d\n", (int)getpid(), committed, limitMB, over ? 1 : 0);
            char temporary[200];
            snprintf(temporary, sizeof temporary, "%s.tmp", statusPath);
            int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd >= 0) {
                write(fd, line, length);
                fchmod(fd, 0666);
                close(fd);
                rename(temporary, statusPath);
            }
        }
        if (seconds <= 0 || (tick * 5) % (unsigned)seconds >= 5)
            continue;
        size_t flexible = 0;
        if (sceKernelAvailableFlexibleMemorySize)
            sceKernelAvailableFlexibleMemorySize(&flexible);
        struct rusage usage;
        getrusage(RUSAGE_SELF, &usage);
        long rss = residentKB();
        double pas = &pas_page_malloc_num_allocated_bytes ? pas_page_malloc_num_allocated_bytes / 1048576.0 : -1;
        double resident = residentBigMB();
        double compact = &pas_compact_heap_reservation_bump ? pas_compact_heap_reservation_bump / 1048576.0 : -1;
        double compactSize = &pas_compact_heap_reservation_size ? pas_compact_heap_reservation_size / 1048576.0 : -1;
        char jitText[96] = "";
        if (ps5wk_jit_memory) {
            void *jitStart = nullptr, *jitEnd = nullptr;
            size_t jitAllocated = 0, jitCommitted = 0;
            ps5wk_jit_memory(&jitStart, &jitEnd, &jitAllocated, &jitCommitted);
            if (jitStart)
                snprintf(jitText, sizeof jitText, "; JIT code %.1f MB (committed %.1f of %.0f MB)", jitAllocated / 1048576.0,
                    jitCommitted / 1048576.0, ((char*)jitEnd - (char*)jitStart) / 1048576.0);
            else
                snprintf(jitText, sizeof jitText, "; JIT off");
        }
        char rssText[48];
        if (rss >= 0)
            snprintf(rssText, sizeof rssText, "rss %.1f MB, peak", rss / 1024.0);
        else
            snprintf(rssText, sizeof rssText, "peak rss");
        LOG("[mem] %s at %.0f s: %s %.1f MB; flexible free %.1f MB (%+.1f since start); libpas pages %.1f MB (%.1f given back, %.1f committed), resident in big mappings %.1f MB, compact %.1f of %.0f MB; mmaps %lld live, %.1f MB (peak %.1f, largest %.1f), %llu calls, %llu munmaps, %llu failed%s",
            tag, (nowMS() - start) / 1e3, rssText, usage.ru_maxrss / 1024.0,
            flexible / 1048576.0, ((double)flexible - (double)startFlexible) / 1048576.0, pas, givenBack, committed, resident, compact, compactSize,
            (long long)mmapLiveCount.load(), mmapLiveBytes.load() / 1048576.0, mmapPeakBytes.load() / 1048576.0, mmapLargest.load() / 1048576.0,
            (unsigned long long)mmapCalls.load(), (unsigned long long)munmapCalls.load(), (unsigned long long)mmapFailures.load(), jitText);
    }
    return nullptr;
}

static void* runWebKit(void* data)
{
    auto* args = static_cast<MainArgs*>(data);
    if (!args->network)
        ps5wk_profiler_start("web");
    args->result = args->network ? WebKit::NetworkProcessMain(args->argc, args->argv) : WebKit::WebProcessMain(args->argc, args->argv);
    return nullptr;
}

int main(int, char**)
{
    signal(SIGPIPE, SIG_IGN);
    ps5wk_crash_guard_install("child");
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);

    char line[512];
    if (!readHeader(line, sizeof(line), 20000)) {
        LOG("no launch header on stdin; this payload is started by the PS5 WebKit UIProcess (see PS5LaunchProtocol.h)");
        return 1;
    }
    char magic[32], type[16], path[256];
    int version = 0;
    unsigned long long processIdentifier = 0;
    if (sscanf(line, "%31s %d %15s %llu %255s", magic, &version, type, &processIdentifier, path) != 5
        || strcmp(magic, PS5WK_LAUNCH_HEADER) || version != PS5WK_LAUNCH_VERSION) {
        LOG("bad launch header: \"%s\"", line);
        return 1;
    }
    bool network = !strcmp(type, "network");
    char tag[32];
    snprintf(tag, sizeof(tag), "%s%llu", network ? "network" : "web", processIdentifier);
    ps5wk_crash_guard_install(tag);
    size_t flexible = 0;
    if (sceKernelAvailableFlexibleMemorySize)
        sceKernelAvailableFlexibleMemorySize(&flexible);
    startFlexible = flexible;
    LOG("%s process %llu starting (parent %d, flexible memory %zu MB)", type, processIdentifier, (int)getppid(), flexible >> 20);

    int fd = rendezvous(path, processIdentifier);
    if (fd < 0)
        return 1;

    setupEnvironment(path);
    if (!network)
        configureJIT();
    // Audio goes to the UIProcess's audio server (apps/common/ps5audioserver.c):
    // <dir>/ui-<pid>.sock -> <dir>/audio-<pid>.sock; hole-punch video frames
    // to its video server (wpe-ps5/PS5VideoServer.cpp): <dir>/video-<pid>.sock.
    if (!network) {
        char audioPath[256];
        snprintf(audioPath, sizeof(audioPath), "%s", path);
        if (char* slash = strrchr(audioPath, '/')) {
            int uiPid = 0;
            if (sscanf(slash + 1, "ui-%d.sock", &uiPid) == 1) {
                snprintf(slash + 1, sizeof(audioPath) - (slash + 1 - audioPath), "audio-%d.sock", uiPid);
                setenv("PS5WK_AUDIO_SOCKET", audioPath, 0);
                snprintf(slash + 1, sizeof(audioPath) - (slash + 1 - audioPath), "video-%d.sock", uiPid);
                setenv("PS5WK_VIDEO_SOCKET", audioPath, 0);
                // The memory limit's status for the UIProcess (memLogger).
                snprintf(slash + 1, sizeof(audioPath) - (slash + 1 - audioPath), "webmem-%d", uiPid);
                setenv("PS5WK_MEM_STATUS", audioPath, 1);
            }
        }
    }
    g_io_openssl_load(nullptr);
    if (!network) {
        // Hardware decoding in the title (Milestone 2 step 2b): advertise up
        // to 8K (the hardware does HEVC/VP9 that large; H.264 above 4K falls
        // back to software); otherwise 1080p60, the software decoders' limit
        // (YouTube then stays at or below 1080p). child.env can override.
        unsigned codecs = 0;
        int maxWidth = 0, maxHeight = 0;
        queryTitleDecoder(codecs, maxWidth, maxHeight);
        if (codecs && maxWidth >= 3840)
            setenv("WEBKIT_GST_VIDEO_DECODING_LIMIT", maxWidth >= 7680 && maxHeight >= 4320 ? "7680x4320@60" : "3840x2160@60", 0);
        else
            setenv("WEBKIT_GST_VIDEO_DECODING_LIMIT", "1920x1080@60", 0);
        LOG("video decoding: %s (codecs 0x%x, up to %dx%d); MSE/canPlayType limit %s", codecs ? "hardware in the title" : "software",
            codecs, maxWidth, maxHeight, getenv("WEBKIT_GST_VIDEO_DECODING_LIMIT"));
    }

    static char idArg[32], fdArg[16];
    snprintf(idArg, sizeof(idArg), "%llu", processIdentifier);
    snprintf(fdArg, sizeof(fdArg), "%d", fd);
    static MainArgs args { network, 3, { const_cast<char*>(network ? "WPENetworkProcess" : "WPEWebProcess"), idArg, fdArg, nullptr }, 0 };

    // WebKit's main thread is whichever thread initializes it (WTF generic
    // MainThread); run it on a thread with a known, large stack for JSC.
    static char cpuTag[32];
    snprintf(cpuTag, sizeof(cpuTag), "%s", tag);
    const char* cpuLog = getenv("PS5WK_CPU_LOG");
    if (!network && !(cpuLog && !strcmp(cpuLog, "0"))) {
        pthread_t cpuThread;
        if (!pthread_create(&cpuThread, nullptr, cpuLogger, cpuTag))
            pthread_detach(cpuThread);
    }
    {
        pthread_t memThread;
        if (!pthread_create(&memThread, nullptr, memLogger, cpuTag))
            pthread_detach(memThread);
    }

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 64 << 20);
    pthread_t thread;
    if (pthread_create(&thread, &attributes, runWebKit, &args)) {
        LOG("pthread_create failed, running on the main thread");
        runWebKit(&args);
    } else
        pthread_join(thread, nullptr);
    LOG("%s process %llu exited with %d", type, processIdentifier, args.result);
    // _exit, not exit: static destructors crashed (WebCore::Font::~Font ->
    // ThreadGlobalData, WTFCrash) every time a page's process ended, and each
    // crash made the system write a crash report for a ~600 MB payload.
    fflush(stdout);
    fflush(stderr);
    _exit(args.result);
}
