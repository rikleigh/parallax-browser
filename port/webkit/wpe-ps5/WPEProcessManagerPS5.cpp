/*
 * Copyright (C) 2026 ps5webkit contributors
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.
 */

#include "config.h"

#include "PS5LaunchProtocol.h"
#include "PS5PerfOverlay.h"
#include "WPEPS5Private.h"
#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <dirent.h>
#include <climits>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Vector.h>
#include <wtf/glib/GRefPtr.h>
#include <wtf/glib/WTFGType.h>
#include <string>

// WebKit's auxiliary processes as elfldr payloads (see PS5LaunchProtocol.h).
// launch() returns at once; the ELF upload runs on a thread (the WebProcess
// ELF is large and elfldr takes 1.6-5 s to start it, Spike 8), and the IPC
// fd is handed over when the payload connects. Until then WebKit's messages
// wait in the socketpair.

G_DECLARE_FINAL_TYPE(WPEProcessManagerPS5, wpe_process_manager_ps5, WPE, PROCESS_MANAGER_PS5, WPEProcessManager)
#define WPE_TYPE_PROCESS_MANAGER_PS5 (wpe_process_manager_ps5_get_type())

struct PendingLaunch {
    guint64 handle { 0 };
    guint64 processIdentifier { 0 };
    WPEProcessType type { WPE_PROCESS_TYPE_WEB };
    int ipcFD { -1 };
    int pid { 0 };
    gint64 startTime { 0 };
    bool inTitle { false }; // the WebProcess runs in this process: never killed
};

struct _WPEProcessManagerPS5Private {
    std::string childELF;
    std::string socketPath;
    int listenFD { -1 };
    guint listenWatch { 0 };
    guint waitDirTimer { 0 };
    int waitDirTicks { 0 };
    guint64 nextHandle { 1 };
    HashMap<guint64, std::unique_ptr<PendingLaunch>> launches; // by handle
    WPEProcessManagerPS5InTitleStart inTitleStart { nullptr };
    bool inTitleUsed { false };
    // With a WebProcess in the title, the rendezvous (listener, hellos, the
    // wait for PS5WK_LAUNCH_DIR) runs on a thread of its own with this
    // context: the title's main loop can be blocked in a sync IPC wait of
    // that WebProcess, which a NetworkProcess's reply ends only once the
    // NetworkProcess has its IPC fd (2026-11-15: GetNetworkProcessConnection
    // waited forever). Null: the main loop's context, as before.
    GMainContext* rendezvousContext { nullptr };
    Lock launchesLock; // launches, when the rendezvous has its own thread
};
WEBKIT_DEFINE_FINAL_TYPE(WPEProcessManagerPS5, wpe_process_manager_ps5, WPE_TYPE_PROCESS_MANAGER, WPEProcessManager)

static const char* typeName(WPEProcessType type)
{
    switch (type) {
    case WPE_PROCESS_TYPE_WEB:
        return "web";
    case WPE_PROCESS_TYPE_NETWORK:
        return "network";
    case WPE_PROCESS_TYPE_GPU:
        return "gpu";
    }
    return "?";
}

// ---------------------------------------------------- upload + output relay

struct UploadJob {
    std::string elfPath;
    std::string header;
    std::string tag;
    WPEProcessManagerPS5* manager { nullptr }; // a reference, dropped on the main thread
    guint64 handle { 0 };
    // A task payload (wpe_process_manager_ps5_run_task): no WebKit process.
    WPEProcessManagerPS5TaskOutput taskOutput { nullptr };
    WPEProcessManagerPS5TaskEnded taskEnded { nullptr };
    gpointer taskData { nullptr };
};

static bool writeAll(int fd, const void* data, size_t size)
{
    const auto* p = static_cast<const uint8_t*>(data);
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

// elfldr takes one payload at a time: two concurrent uploads left the second
// payload with unapplied relocations (SIGSEGV in a static initializer), and
// a spawn right after another one can fail with "Error spawning payload"
// (2026-10-28). Uploads are serialized, the next one waits until the
// previous payload has printed its first line (it is running) or 10 s, and
// a refused spawn is retried.
static GMutex s_uploadLock;

// A title that dies while it streams a child to elfldr leaves elfldr with a
// cut-off ELF, and the payload elfldr makes of it never runs and never ends:
// it stays until the console restarts (2026-11-02: one after a WebProcess
// crash loop was killed, more after title crashes; each made every later
// launch and page slower, see CONSOLE-HEALTH.md). The title therefore calls
// wpe_process_manager_ps5_quiesce() before it asks for its SIGKILL: no new
// upload starts after that, and it waits for the one being sent.
static std::atomic<bool> s_quiescing { false };
static std::atomic<int> s_sending { 0 };

// The browser title's log queue (apps/title/titlelog.c) sets this: the
// relay then never waits on the log sink, and the payloads never wait on a
// relay stopped behind it (2026-11-02: a dead network froze the title in
// blocking sends). Unset (ps5wk-shot): stderr.
extern "C" void (*ps5wk_log_line_hook)(const char* line, size_t length);
void (*ps5wk_log_line_hook)(const char* line, size_t length) = nullptr;

static void logLine(const char* text)
{
    if (auto* hook = ps5wk_log_line_hook) {
        char line[2304];
        size_t length = std::min(strlen(text), sizeof(line) - 1);
        memcpy(line, text, length);
        line[length++] = '\n';
        hook(line, length);
    } else
        fprintf(stderr, "%s\n", text);
}

// One line of a payload's output into our log; the performance overlay's
// lines (PS5PerfOverlay.cpp) stay out of it.
static void relayLine(const char* tag, const char* line)
{
    if (ps5PerfNoteChildLine(tag, line))
        return;
    char text[2200];
    snprintf(text, sizeof(text), "[%s] %s", tag, line);
    logLine(text);
}

// Connects to elfldr and sends the ELF plus the header line. Returns the
// connection (the payload's stdout), or -1 (*retry: worth another attempt).
static int sendToElfldr(const UploadJob& job, const char* tag, bool* retry)
{
    gint64 t0 = g_get_monotonic_time();
    *retry = false;
    // Counted before the flag is read (both sequentially consistent), so
    // quiesce() either sees this send or this send sees its flag.
    s_sending++;
    struct SendingScope {
        ~SendingScope() { s_sending--; }
    } sendingScope;
    if (s_quiescing) {
        PS5_LOG("[ps5-launch %s] not sent: the title is ending", tag);
        return -1;
    }
    int elf = open(job.elfPath.c_str(), O_RDONLY);
    if (elf < 0) {
        g_warning("[ps5-launch %s] cannot open child ELF %s: errno %d", tag, job.elfPath.c_str(), errno);
        return -1;
    }
    struct sockaddr_in address { };
    address.sin_family = AF_INET;
    address.sin_port = htons(PS5WK_ELFLDR_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // elfldr sometimes refuses connections for a moment (ECONNREFUSED on
    // loopback while it is busy with another payload: seen in back-to-back
    // ps5wk-shot runs, 2026-10-31): retry for up to 10 s.
    int s = -1;
    for (int attempt = 1;; attempt++) {
        s = socket(AF_INET, SOCK_STREAM, 0);
        if (s >= 0 && !connect(s, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)))
            break;
        int savedErrno = errno;
        if (s >= 0)
            close(s);
        s = -1;
        if (savedErrno != ECONNREFUSED || attempt >= 40) {
            errno = savedErrno;
            break;
        }
        if (attempt == 1)
            g_message("[ps5-launch %s] elfldr refused the connection; retrying for up to 10 s", tag);
        g_usleep(250 * 1000);
    }
    if (s < 0) {
        g_warning("[ps5-launch %s] cannot reach elfldr on 127.0.0.1:%d: errno %d", tag, PS5WK_ELFLDR_PORT, errno);
        close(elf);
        *retry = true;
        return -1;
    }
    static char chunk[1 << 20];
    size_t total = 0;
    bool ok = true;
    int writeError = 0;
    for (;;) {
        ssize_t n = read(elf, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        if (!writeAll(s, chunk, static_cast<size_t>(n))) {
            writeError = errno;
            ok = false;
            break;
        }
        total += static_cast<size_t>(n);
    }
    close(elf);
    if (ok && !writeAll(s, job.header.c_str(), job.header.length())) {
        writeError = errno;
        ok = false;
    }
    PS5_LOG("[ps5-launch %s] sent %zu bytes + header to elfldr in %.0f ms%s", tag, total,
        (g_get_monotonic_time() - t0) / 1000.0, ok ? "" : " (write failed)");
    if (!ok) {
        // elfldr closed the connection under us (seen right after a
        // WebProcess was killed, 2026-11-02: 0 bytes taken, errno below).
        g_warning("[ps5-launch %s] elfldr took %zu bytes, then the write failed: errno %d", tag, total, writeError);
        close(s);
        *retry = true;
        return -1;
    }
    return s;
}

// The upload of a launch failed for good: close the IPC socket WebKit gave us
// for that process, so that WebKit sees the process end (as a crash) and
// the page asks for another one. Otherwise WebKit waits forever for a
// process that never runs and the page stays on it (2026-11-02: a page
// restart whose upload failed left the browser without a page process).
struct LaunchFailure {
    WPEProcessManagerPS5* manager;
    guint64 handle;
    std::string tag;
};

static gboolean launchFailedOnMain(gpointer data)
{
    std::unique_ptr<LaunchFailure> failure(static_cast<LaunchFailure*>(data));
    auto* priv = failure->manager->priv;
    Locker locker { priv->launchesLock };
    auto it = priv->launches.find(failure->handle);
    if (it != priv->launches.end() && it->value->ipcFD >= 0 && !it->value->pid) {
        PS5_LOG("[ps5-launch %s] the launch failed: closing its IPC socket, so WebKit takes the process as ended and starts another",
            failure->tag.c_str());
        close(it->value->ipcFD);
        it->value->ipcFD = -1;
    }
    g_object_unref(failure->manager);
    return G_SOURCE_REMOVE;
}

static gboolean unrefOnMain(gpointer data)
{
    g_object_unref(data);
    return G_SOURCE_REMOVE;
}

static gpointer uploadThread(gpointer data)
{
    std::unique_ptr<UploadJob> job(static_cast<UploadJob*>(data));
    const char* tag = job->tag.c_str();
    gint64 t0 = g_get_monotonic_time();
    char line[2048];
    size_t used = 0;
    int s = -1;

    g_mutex_lock(&s_uploadLock);
    for (int attempt = 1; attempt <= 6 && s < 0; attempt++) {
        bool retry = false;
        s = sendToElfldr(*job, tag, &retry);
        if (s < 0) {
            if (!retry || attempt == 6)
                break;
            g_warning("[ps5-launch %s] upload failed (attempt %d); retrying in %d s", tag, attempt, attempt);
            g_usleep(attempt * 1000 * 1000);
            continue;
        }
        // Wait for the payload's first output: it is running, or elfldr
        // refused the spawn.
        struct pollfd p { s, POLLIN, 0 };
        if (poll(&p, 1, 10000) != 1)
            break; // silent but maybe alive; relay whatever comes
        ssize_t n = read(s, line, sizeof(line) - 1);
        if (n <= 0) {
            close(s);
            s = -1;
            g_warning("[ps5-launch %s] payload closed its output at once (attempt %d)", tag, attempt);
            break;
        }
        line[n] = 0;
        if (strstr(line, "Error spawning payload")) {
            close(s);
            s = -1;
            g_warning("[ps5-launch %s] elfldr: Error spawning payload (attempt %d); retrying", tag, attempt);
            g_usleep(attempt * 1000 * 1000);
            continue;
        }
        used = static_cast<size_t>(n);
    }
    g_mutex_unlock(&s_uploadLock);
    if (s < 0) {
        g_warning("[ps5-launch %s] launch failed", tag);
        if (job->taskEnded) {
            job->taskEnded(FALSE, job->taskData);
            g_idle_add(unrefOnMain, job->manager);
        } else
            g_idle_add(launchFailedOnMain, new LaunchFailure { job->manager, job->handle, job->tag });
        return nullptr;
    }
    g_idle_add(unrefOnMain, job->manager);

    // The payload's stdout/stderr come back on this connection: relay them
    // to our log, line by line, until the payload exits.
    for (;;) {
        line[used] = 0;
        char* start = line;
        for (char* nl; (nl = strchr(start, '\n')); start = nl + 1) {
            *nl = 0;
            relayLine(tag, start);
            if (job->taskOutput)
                job->taskOutput(start, job->taskData);
        }
        used = static_cast<size_t>(line + used - start);
        memmove(line, start, used);
        if (used == sizeof(line) - 1) {
            line[used] = 0;
            relayLine(tag, line);
            used = 0;
        }
        ssize_t n = read(s, line + used, sizeof(line) - 1 - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        used += static_cast<size_t>(n);
    }
    if (used) {
        line[used] = 0;
        relayLine(tag, line);
    }
    char closed[96];
    snprintf(closed, sizeof(closed), "[%s] (output closed after %.1f s)", tag, (g_get_monotonic_time() - t0) / 1e6);
    logLine(closed);
    ps5PerfChildEnded(tag);
    close(s);
    if (job->taskEnded)
        job->taskEnded(TRUE, job->taskData);
    return nullptr;
}

// ---------------------------------------------------------- the rendezvous

// Sources on the rendezvous context (the main loop's, or its own thread's).
static guint rendezvousAddFD(WPEProcessManagerPS5Private* priv, int fd, GIOCondition condition, GUnixFDSourceFunc function, gpointer data)
{
    GSource* source = g_unix_fd_source_new(fd, condition);
    g_source_set_callback(source, reinterpret_cast<GSourceFunc>(function), data, nullptr);
    guint id = g_source_attach(source, priv->rendezvousContext);
    g_source_unref(source);
    return id;
}

static guint rendezvousAddTimeout(WPEProcessManagerPS5Private* priv, guint ms, GSourceFunc function, gpointer data)
{
    GSource* source = g_timeout_source_new(ms);
    g_source_set_callback(source, function, data, nullptr);
    guint id = g_source_attach(source, priv->rendezvousContext);
    g_source_unref(source);
    return id;
}

static void rendezvousRemove(WPEProcessManagerPS5Private* priv, guint id)
{
    if (GSource* source = g_main_context_find_source_by_id(priv->rendezvousContext, id))
        g_source_destroy(source);
}


static bool sendFD(int socket, const ps5wk_welcome& welcome, int fd)
{
    struct iovec iov { const_cast<ps5wk_welcome*>(&welcome), sizeof(welcome) };
    char control[CMSG_SPACE(sizeof(int))] { };
    struct msghdr message { };
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    if (fd >= 0) {
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        struct cmsghdr* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &fd, sizeof(int));
    }
    for (;;) {
        ssize_t n = sendmsg(socket, &message, 0);
        if (n < 0 && errno == EINTR)
            continue;
        return n == static_cast<ssize_t>(sizeof(welcome));
    }
}

// A child's hello, read as it arrives: the main loop never waits for a
// peer (one that sent part of a hello and then nothing used to stop the UI
// in a blocking recv). Whole within 2 s, or the connection is closed.
struct PendingHello {
    WPEProcessManagerPS5* manager; // a reference
    int fd;
    ps5wk_hello hello;
    size_t received;
    guint watch;
    guint timeout;
};

static void handleHello(WPEProcessManagerPS5*, int client, const ps5wk_hello&);

static void pendingHelloEnd(PendingHello* pending, bool closeFD)
{
    if (pending->watch)
        rendezvousRemove(pending->manager->priv, pending->watch);
    if (pending->timeout)
        rendezvousRemove(pending->manager->priv, pending->timeout);
    if (closeFD)
        close(pending->fd);
    g_object_unref(pending->manager);
    delete pending;
}

static gboolean pendingHelloTimeout(gpointer data)
{
    auto* pending = static_cast<PendingHello*>(data);
    pending->timeout = 0;
    g_warning("[ps5-launch] rendezvous: no whole hello within 2 s (%zu of %zu bytes)", pending->received, sizeof(pending->hello));
    pendingHelloEnd(pending, true);
    return G_SOURCE_REMOVE;
}

static gboolean pendingHelloReadable(int fd, GIOCondition, gpointer data)
{
    auto* pending = static_cast<PendingHello*>(data);
    ssize_t n = recv(fd, reinterpret_cast<char*>(&pending->hello) + pending->received, sizeof(pending->hello) - pending->received, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return G_SOURCE_CONTINUE;
    if (n <= 0) {
        g_warning("[ps5-launch] rendezvous: connection closed after %zu of %zu hello bytes (errno %d)", pending->received, sizeof(pending->hello), n ? errno : 0);
        pending->watch = 0;
        pendingHelloEnd(pending, true);
        return G_SOURCE_REMOVE;
    }
    pending->received += static_cast<size_t>(n);
    if (pending->received < sizeof(pending->hello))
        return G_SOURCE_CONTINUE;
    pending->watch = 0;
    handleHello(pending->manager, fd, pending->hello); // closes fd
    pendingHelloEnd(pending, false);
    return G_SOURCE_REMOVE;
}

static void handleConnection(WPEProcessManagerPS5* manager, int client)
{
    int flags = fcntl(client, F_GETFL, 0);
    if (flags < 0 || fcntl(client, F_SETFL, flags | O_NONBLOCK) < 0) {
        g_warning("[ps5-launch] rendezvous: O_NONBLOCK failed (errno %d)", errno);
        close(client);
        return;
    }
    fcntl(client, F_SETFD, FD_CLOEXEC);
    auto* pending = new PendingHello { WPE_PROCESS_MANAGER_PS5(g_object_ref(manager)), client, { }, 0, 0, 0 };
    pending->watch = rendezvousAddFD(manager->priv, client, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR), pendingHelloReadable, pending);
    pending->timeout = rendezvousAddTimeout(manager->priv, 2000, pendingHelloTimeout, pending);
}

// Main loop: a whole hello; closes client.
static void handleHello(WPEProcessManagerPS5* manager, int client, const ps5wk_hello& hello)
{
    auto* priv = manager->priv;
    if (hello.magic != PS5WK_HELLO_MAGIC) {
        g_warning("[ps5-launch] rendezvous: bad hello (magic 0x%llx)", static_cast<unsigned long long>(hello.magic));
        close(client);
        return;
    }

    Locker locker { priv->launchesLock };
    PendingLaunch* launch = nullptr;
    for (auto& entry : priv->launches.values()) {
        if (entry->processIdentifier == hello.processIdentifier && entry->ipcFD >= 0) {
            launch = entry.get();
            break;
        }
    }
    ps5wk_welcome welcome { PS5WK_WELCOME_MAGIC, launch ? 0 : -1, 0 };
    if (!launch) {
        g_warning("[ps5-launch] payload pid %d asked for unknown process %" G_GUINT64_FORMAT, hello.pid, hello.processIdentifier);
        sendFD(client, welcome, -1);
        close(client);
        return;
    }
    bool sent = sendFD(client, welcome, launch->ipcFD);
    PS5_LOG("[ps5-launch] %s process %" G_GUINT64_FORMAT " is payload pid %d, connected %.0f ms after launch; IPC fd %s",
        typeName(launch->type), launch->processIdentifier, hello.pid, (g_get_monotonic_time() - launch->startTime) / 1000.0,
        sent ? "passed" : "NOT passed");
    close(launch->ipcFD);
    launch->ipcFD = -1;
    launch->pid = hello.pid;
    close(client);
}

// The title is ended with SIGKILL (and payload harnesses often are), so the
// sockets and status files named after a pid in PS5WK_LAUNCH_DIR stay
// behind; the directory is a small tmpfs and binding failed with ENOSPC
// after a few dozen. Remove those whose process is gone. kill(pid, 0) alone
// can't tell: title pids are small and get reused by other system processes
// (and a title may get EPERM for any pid), so a pid only counts as alive if
// one of its sockets still has a listener.
static bool socketHasListener(const std::string& path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return true;
    struct sockaddr_un address { };
    address.sun_family = AF_UNIX;
    g_strlcpy(address.sun_path, path.c_str(), sizeof(address.sun_path));
    bool listening = !connect(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address))
        || (errno != ECONNREFUSED && errno != ENOENT);
    close(fd);
    return listening;
}

static void removeStaleRuntimeFiles()
{
    DIR* dir = opendir(PS5WK_LAUNCH_DIR);
    if (!dir) {
        // A title gets EPERM here; ps5wk-child cleans up instead.
        if (errno != EPERM)
            g_warning("[ps5-launch] cannot list %s: errno %d", PS5WK_LAUNCH_DIR, errno);
        return;
    }
    unsigned entries = 0;
    struct PidFiles {
        Vector<std::string> files;
        Vector<std::string> sockets;
    };
    HashMap<int, PidFiles> byPid;
    while (struct dirent* entry = readdir(dir)) {
        entries++;
        const char* name = entry->d_name;
        int pid = 0;
        const char* dash = strrchr(name, '-');
        if (!dash)
            continue;
        size_t prefix = dash - name;
        bool known = (prefix == 2 && !strncmp(name, "ui", 2)) || (prefix == 5 && !strncmp(name, "audio", 5))
            || (prefix == 5 && !strncmp(name, "video", 5)) || (prefix == 6 && !strncmp(name, "webmem", 6));
        if (!known)
            continue;
        char* end = nullptr;
        long value = strtol(dash + 1, &end, 10);
        if (end == dash + 1 || value <= 0 || value > INT_MAX)
            continue;
        if (strcmp(end, prefix == 6 ? "" : ".sock"))
            continue;
        pid = static_cast<int>(value);
        if (pid == getpid())
            continue;
        std::string path = std::string(PS5WK_LAUNCH_DIR "/") + name;
        auto& files = byPid.add(pid, PidFiles { }).iterator->value;
        if (prefix != 6)
            files.sockets.append(path);
        files.files.append(std::move(path));
    }
    closedir(dir);
    unsigned removed = 0, kept = 0, failed = 0;
    int unlinkError = 0;
    for (auto& [pid, files] : byPid) {
        // Gone for sure (ESRCH), or no socket of it answers. A pid with only
        // a status file has no UIProcess listening either.
        bool gone = (kill(pid, 0) && errno == ESRCH)
            || std::none_of(files.sockets.begin(), files.sockets.end(), socketHasListener);
        if (!gone) {
            kept += files.files.size();
            continue;
        }
        for (auto& path : files.files) {
            if (!unlink(path.c_str()))
                removed++;
            else {
                failed++;
                unlinkError = errno;
            }
        }
    }
    PS5_LOG("[ps5-launch] %s: %u entries, removed %u stale files, kept %u of live processes, %u not removed (errno %d)",
        PS5WK_LAUNCH_DIR, entries, removed, kept, failed, unlinkError);
}

static bool bindListener(WPEProcessManagerPS5* manager)
{
    removeStaleRuntimeFiles();
    auto* priv = manager->priv;
    struct sockaddr_un address { };
    address.sun_family = AF_UNIX;
    g_strlcpy(address.sun_path, priv->socketPath.c_str(), sizeof(address.sun_path));
    unlink(address.sun_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || bind(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) || listen(fd, 8)) {
        g_warning("[ps5-launch] cannot listen on %s: errno %d", address.sun_path, errno);
        if (fd >= 0)
            close(fd);
        return false;
    }
    chmod(address.sun_path, 0777); // payloads run under another uid
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    priv->listenFD = fd;
    priv->listenWatch = rendezvousAddFD(priv, fd, G_IO_IN, [](int fd, GIOCondition, gpointer userData) -> gboolean {
        int client = accept(fd, nullptr, nullptr);
        if (client >= 0)
            handleConnection(WPE_PROCESS_MANAGER_PS5(userData), client);
        return G_SOURCE_CONTINUE;
    }, manager);
    PS5_LOG("[ps5-launch] listening on %s", address.sun_path);
    return true;
}

// The first payload creates PS5WK_LAUNCH_DIR; the title can only bind in it.
static void ensureListener(WPEProcessManagerPS5* manager)
{
    auto* priv = manager->priv;
    if (priv->listenFD >= 0 || priv->waitDirTimer)
        return;
    if (priv->inTitleStart && !priv->rendezvousContext) {
        priv->rendezvousContext = g_main_context_new();
        g_thread_unref(g_thread_new("ps5-rendezvous", [](gpointer context) -> gpointer {
            GMainLoop* loop = g_main_loop_new(static_cast<GMainContext*>(context), FALSE);
            g_main_loop_run(loop);
            return nullptr;
        }, priv->rendezvousContext));
        PS5_LOG("[ps5-launch] rendezvous on its own thread (a WebProcess runs in the title)");
    }
    struct stat st;
    if (!stat(PS5WK_LAUNCH_DIR, &st) && bindListener(manager))
        return;
    priv->waitDirTicks = 0;
    priv->waitDirTimer = rendezvousAddTimeout(priv, 100, [](gpointer userData) -> gboolean {
        auto* manager = WPE_PROCESS_MANAGER_PS5(userData);
        auto* priv = manager->priv;
        struct stat st;
        if (!stat(PS5WK_LAUNCH_DIR, &st) && bindListener(manager)) {
            priv->waitDirTimer = 0;
            return G_SOURCE_REMOVE;
        }
        if (++priv->waitDirTicks > 600) {
            g_warning("[ps5-launch] %s did not appear in 60 s: no payload started?", PS5WK_LAUNCH_DIR);
            priv->waitDirTimer = 0;
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }, manager);
}

// ------------------------------------------------------- WPEProcessManager

struct InTitleStart {
    WPEProcessManagerPS5* manager;
    guint64 handle;
};

// After launch() has returned (ProcessLauncher then finishes the launch on
// the main loop): the WebProcess object in the title takes the fd.
static gboolean startInTitleOnMain(gpointer data)
{
    auto* start = static_cast<InTitleStart*>(data);
    auto* priv = start->manager->priv;
    int fd = -1;
    guint64 processIdentifier = 0;
    {
        Locker locker { priv->launchesLock };
        auto it = priv->launches.find(start->handle);
        if (it != priv->launches.end() && it->value->ipcFD >= 0) {
            fd = std::exchange(it->value->ipcFD, -1);
            processIdentifier = it->value->processIdentifier;
        }
    }
    if (fd >= 0) {
        gint64 t0 = g_get_monotonic_time();
        if (priv->inTitleStart && !priv->inTitleStart(fd, processIdentifier)) {
            PS5_LOG("[ps5-launch] web process %" G_GUINT64_FORMAT " runs in the title (pid %d); started in %.1f ms",
                processIdentifier, static_cast<int>(getpid()), (g_get_monotonic_time() - t0) / 1000.0);
        } else {
            g_warning("[ps5-launch] web process %" G_GUINT64_FORMAT ": the in-title start refused it; closing its IPC socket (the next launch is a payload)",
                processIdentifier);
            close(fd);
        }
    } else
        PS5_LOG("[ps5-launch] in-title web process: launch %" G_GUINT64_FORMAT " ended before it started", start->handle);
    g_object_unref(start->manager);
    delete start;
    return G_SOURCE_REMOVE;
}

static guint64 wpeProcessManagerPS5Launch(WPEProcessManager* processManager, WPEProcessLaunchOptions* options, GError** error)
{
    auto* manager = WPE_PROCESS_MANAGER_PS5(processManager);
    auto* priv = manager->priv;

    auto type = wpe_process_launch_options_get_process_type(options);
    if (type == WPE_PROCESS_TYPE_GPU) {
        g_set_error_literal(error, WPE_PROCESS_MANAGER_ERROR, WPE_PROCESS_MANAGER_ERROR_LAUNCH_FAILED, "PS5: no GPU process");
        return 0;
    }
    // WebKit closes its copy of the client end when launch() returns.
    int ipcFD = dup(wpe_process_launch_options_get_ipc_socket_fd(options));
    if (ipcFD < 0) {
        g_set_error(error, WPE_PROCESS_MANAGER_ERROR, WPE_PROCESS_MANAGER_ERROR_LAUNCH_FAILED, "PS5: dup(ipc fd) errno %d", errno);
        return 0;
    }
    fcntl(ipcFD, F_SETFD, FD_CLOEXEC);

    auto launch = std::make_unique<PendingLaunch>();
    launch->handle = priv->nextHandle++;
    launch->processIdentifier = wpe_process_launch_options_get_process_id(options);
    launch->type = type;
    launch->ipcFD = ipcFD;
    launch->startTime = g_get_monotonic_time();

    if (type == WPE_PROCESS_TYPE_WEB && priv->inTitleStart && !priv->inTitleUsed) {
        priv->inTitleUsed = true;
        launch->inTitle = true;
        guint64 handle = launch->handle;
        PS5_LOG("[ps5-launch] launching web process %" G_GUINT64_FORMAT " in the title (/data/ps5webkit/inprocess-web)", launch->processIdentifier);
        {
            Locker locker { priv->launchesLock };
            priv->launches.add(handle, WTF::move(launch));
        }
        g_idle_add_full(G_PRIORITY_HIGH, startInTitleOnMain, new InTitleStart { WPE_PROCESS_MANAGER_PS5(g_object_ref(manager)), handle }, nullptr);
        return handle;
    }

    auto* job = new UploadJob;
    job->elfPath = priv->childELF;
    char text[512];
    snprintf(text, sizeof(text), PS5WK_LAUNCH_HEADER " %d %s %" G_GUINT64_FORMAT " %s\n", PS5WK_LAUNCH_VERSION, typeName(type),
        launch->processIdentifier, priv->socketPath.c_str());
    job->header = text;
    snprintf(text, sizeof(text), "%s%" G_GUINT64_FORMAT, typeName(type), launch->processIdentifier);
    job->tag = text;
    PS5_LOG("[ps5-launch] launching %s process %" G_GUINT64_FORMAT " from %s", typeName(type), launch->processIdentifier, priv->childELF.c_str());

    ensureListener(manager);
    guint64 handle = launch->handle;
    job->manager = WPE_PROCESS_MANAGER_PS5(g_object_ref(manager));
    job->handle = handle;
    {
        Locker locker { priv->launchesLock };
        priv->launches.add(handle, WTF::move(launch));
    }
    g_thread_unref(g_thread_new("ps5-launch", uploadThread, job));
    return handle;
}

static void wpeProcessManagerPS5Terminate(WPEProcessManager* processManager, guint64 handle)
{
    auto* priv = WPE_PROCESS_MANAGER_PS5(processManager)->priv;
    std::unique_ptr<PendingLaunch> launch;
    {
        Locker locker { priv->launchesLock };
        launch = priv->launches.take(handle);
    }
    if (!launch)
        return;
    if (launch->ipcFD >= 0)
        close(launch->ipcFD);
    if (launch->inTitle) {
        // WebKit closed its end: the in-title WebProcess sees its connection
        // close and stays stopped (AuxiliaryProcess::didClose).
        PS5_LOG("[ps5-launch] web process %" G_GUINT64_FORMAT " (in the title) terminated by WebKit", launch->processIdentifier);
        return;
    }
    // Closing WebKit's end of the IPC socket already makes the child exit;
    // SIGKILL in case it is stuck (may be refused across title/payload).
    if (launch->pid > 0 && kill(launch->pid, SIGKILL))
        PS5_LOG("[ps5-launch] kill(%d) errno %d", launch->pid, errno);
}

static void wpeProcessManagerPS5Dispose(GObject* object)
{
    auto* priv = WPE_PROCESS_MANAGER_PS5(object)->priv;
    if (priv->listenWatch) {
        rendezvousRemove(priv, priv->listenWatch);
        priv->listenWatch = 0;
    }
    if (priv->waitDirTimer) {
        rendezvousRemove(priv, priv->waitDirTimer);
        priv->waitDirTimer = 0;
    }
    if (priv->listenFD >= 0) {
        close(priv->listenFD);
        unlink(priv->socketPath.c_str());
        priv->listenFD = -1;
    }
    G_OBJECT_CLASS(wpe_process_manager_ps5_parent_class)->dispose(object);
}

static void wpe_process_manager_ps5_class_init(WPEProcessManagerPS5Class* klass)
{
    G_OBJECT_CLASS(klass)->dispose = wpeProcessManagerPS5Dispose;
    WPEProcessManagerClass* managerClass = WPE_PROCESS_MANAGER_CLASS(klass);
    managerClass->launch = wpeProcessManagerPS5Launch;
    managerClass->terminate = wpeProcessManagerPS5Terminate;
}

WPEProcessManager* wpe_process_manager_ps5_new(const char* childELF)
{
    auto* manager = WPE_PROCESS_MANAGER_PS5(g_object_new(WPE_TYPE_PROCESS_MANAGER_PS5, nullptr));
    auto* priv = manager->priv;
    priv->childELF = childELF && *childELF ? childELF : "/app0/ps5wk-child.elf";
    char path[128];
    snprintf(path, sizeof(path), PS5WK_LAUNCH_DIR "/ui-%d.sock", static_cast<int>(getpid()));
    priv->socketPath = path;
    // A payload whose title went away gets EPIPE, not a signal; same here.
    signal(SIGPIPE, SIG_IGN);
    return WPE_PROCESS_MANAGER(manager);
}

// A task payload: the child ELF in another mode (ps5wk-child.cpp's main
// picks it by the header's type), with no WebKit process and no IPC socket.
// Header: PS5WK-LAUNCH <version> <task> 0 <argument>. Uploaded like the
// WebKit processes (serialized, retried); output relayed to the log as
// [<task>] lines and passed to output, then ended (launched or not), both on
// the relay thread. The payload ends when it likes; it should also end when
// its stdin (the connection) closes.
gboolean wpe_process_manager_ps5_run_task(WPEProcessManager* processManager, const char* task, const char* argument,
    WPEProcessManagerPS5TaskOutput output, WPEProcessManagerPS5TaskEnded ended, gpointer userData)
{
    g_return_val_if_fail(WPE_IS_PROCESS_MANAGER_PS5(processManager), FALSE);
    g_return_val_if_fail(task && *task && !strchr(task, ' ') && argument && *argument && !strchr(argument, ' '), FALSE);
    auto* manager = WPE_PROCESS_MANAGER_PS5(processManager);
    auto* job = new UploadJob;
    job->elfPath = manager->priv->childELF;
    char text[512];
    snprintf(text, sizeof(text), PS5WK_LAUNCH_HEADER " %d %s 0 %s\n", PS5WK_LAUNCH_VERSION, task, argument);
    job->header = text;
    job->tag = task;
    job->manager = WPE_PROCESS_MANAGER_PS5(g_object_ref(manager));
    job->taskOutput = output;
    job->taskEnded = ended;
    job->taskData = userData;
    PS5_LOG("[ps5-launch] launching task %s (%s) from %s", task, argument, job->elfPath.c_str());
    g_thread_unref(g_thread_new("ps5-task", uploadThread, job));
    return TRUE;
}

gboolean wpe_process_manager_ps5_quiesce(guint timeoutMS)
{
    s_quiescing = true;
    gint64 t0 = g_get_monotonic_time();
    gint64 end = t0 + static_cast<gint64>(timeoutMS) * 1000;
    int sending = s_sending;
    while (s_sending > 0 && g_get_monotonic_time() < end)
        g_usleep(5 * 1000);
    bool done = !s_sending;
    PS5_LOG("[ps5-launch] quiesced: no new payloads; %d send(s) in flight at the call, %s after %.0f ms", sending,
        done ? "none left" : "STILL SENDING", (g_get_monotonic_time() - t0) / 1000.0);
    return done;
}

void wpe_process_manager_ps5_set_web_in_title(WPEProcessManager* manager, WPEProcessManagerPS5InTitleStart start)
{
    g_return_if_fail(WPE_IS_PROCESS_MANAGER_PS5(manager));
    WPE_PROCESS_MANAGER_PS5(manager)->priv->inTitleStart = start;
}

void wpe_process_manager_ps5_set_child_elf(WPEProcessManager* manager, const char* path)
{
    g_return_if_fail(WPE_IS_PROCESS_MANAGER_PS5(manager));
    WPE_PROCESS_MANAGER_PS5(manager)->priv->childELF = path && *path ? path : "/app0/ps5wk-child.elf";
}
