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
#include "WPEPS5Private.h"
#include <arpa/inet.h>
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

// Connects to elfldr and sends the ELF plus the header line. Returns the
// connection (the payload's stdout) or -1.
static int sendToElfldr(const UploadJob& job, const char* tag)
{
    gint64 t0 = g_get_monotonic_time();
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
        if (s >= 0)
            close(s);
        close(elf);
        return -1;
    }
    static char chunk[1 << 20];
    size_t total = 0;
    bool ok = true;
    for (;;) {
        ssize_t n = read(elf, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        if (!writeAll(s, chunk, static_cast<size_t>(n))) {
            ok = false;
            break;
        }
        total += static_cast<size_t>(n);
    }
    close(elf);
    ok = ok && writeAll(s, job.header.c_str(), job.header.length());
    PS5_LOG("[ps5-launch %s] sent %zu bytes + header to elfldr in %.0f ms%s", tag, total,
        (g_get_monotonic_time() - t0) / 1000.0, ok ? "" : " (write failed)");
    if (!ok) {
        close(s);
        return -1;
    }
    return s;
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
        s = sendToElfldr(*job, tag);
        if (s < 0)
            break;
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
        return nullptr;
    }

    // The payload's stdout/stderr come back on this connection: relay them
    // to our log, line by line, until the payload exits.
    for (;;) {
        line[used] = 0;
        char* start = line;
        for (char* nl; (nl = strchr(start, '\n')); start = nl + 1) {
            *nl = 0;
            fprintf(stderr, "[%s] %s\n", tag, start);
        }
        used = static_cast<size_t>(line + used - start);
        memmove(line, start, used);
        if (used == sizeof(line) - 1) {
            line[used] = 0;
            fprintf(stderr, "[%s] %s\n", tag, line);
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
        fprintf(stderr, "[%s] %s\n", tag, line);
    }
    fprintf(stderr, "[%s] (output closed after %.1f s)\n", tag, (g_get_monotonic_time() - t0) / 1e6);
    close(s);
    return nullptr;
}

// ---------------------------------------------------------- the rendezvous

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

static void handleConnection(WPEProcessManagerPS5* manager, int client)
{
    auto* priv = manager->priv;
    ps5wk_hello hello { };
    struct pollfd p { client, POLLIN, 0 };
    if (poll(&p, 1, 2000) != 1 || recv(client, &hello, sizeof(hello), MSG_WAITALL) != sizeof(hello) || hello.magic != PS5WK_HELLO_MAGIC) {
        g_warning("[ps5-launch] rendezvous: bad or missing hello (errno %d)", errno);
        close(client);
        return;
    }

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
// after a few hundred. Remove those whose process is gone.
static void removeStaleRuntimeFiles()
{
    DIR* dir = opendir(PS5WK_LAUNCH_DIR);
    if (!dir)
        return;
    unsigned removed = 0;
    while (struct dirent* entry = readdir(dir)) {
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
        // Only a process that is known to be gone (EPERM: alive, another uid).
        if (pid == getpid() || !kill(pid, 0) || errno != ESRCH)
            continue;
        std::string path = std::string(PS5WK_LAUNCH_DIR "/") + name;
        if (!unlink(path.c_str()))
            removed++;
    }
    closedir(dir);
    if (removed)
        PS5_LOG("[ps5-launch] removed %u stale files from %s", removed, PS5WK_LAUNCH_DIR);
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
    priv->listenWatch = g_unix_fd_add(fd, G_IO_IN, [](int fd, GIOCondition, gpointer userData) -> gboolean {
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
    struct stat st;
    if (!stat(PS5WK_LAUNCH_DIR, &st) && bindListener(manager))
        return;
    priv->waitDirTicks = 0;
    priv->waitDirTimer = g_timeout_add(100, [](gpointer userData) -> gboolean {
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
    priv->launches.add(handle, WTF::move(launch));
    g_thread_unref(g_thread_new("ps5-launch", uploadThread, job));
    return handle;
}

static void wpeProcessManagerPS5Terminate(WPEProcessManager* processManager, guint64 handle)
{
    auto* priv = WPE_PROCESS_MANAGER_PS5(processManager)->priv;
    auto launch = priv->launches.take(handle);
    if (!launch)
        return;
    if (launch->ipcFD >= 0)
        close(launch->ipcFD);
    // Closing WebKit's end of the IPC socket already makes the child exit;
    // SIGKILL in case it is stuck (may be refused across title/payload).
    if (launch->pid > 0 && kill(launch->pid, SIGKILL))
        PS5_LOG("[ps5-launch] kill(%d) errno %d", launch->pid, errno);
}

static void wpeProcessManagerPS5Dispose(GObject* object)
{
    auto* priv = WPE_PROCESS_MANAGER_PS5(object)->priv;
    if (priv->listenWatch) {
        g_source_remove(priv->listenWatch);
        priv->listenWatch = 0;
    }
    if (priv->waitDirTimer) {
        g_source_remove(priv->waitDirTimer);
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

void wpe_process_manager_ps5_set_child_elf(WPEProcessManager* manager, const char* path)
{
    g_return_if_fail(WPE_IS_PROCESS_MANAGER_PS5(manager));
    WPE_PROCESS_MANAGER_PS5(manager)->priv->childELF = path && *path ? path : "/app0/ps5wk-child.elf";
}
