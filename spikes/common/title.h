/* title.h - start-up for spikes that run as registered titles.
 *
 * A title starts jailed in its own sandbox: no /data, and (seen on the first
 * console run) no log connection to the Mac either. etaHEN (and
 * PS5-Lapy-JB-Daemon) unjail a title on request: write {"PID":"<pid>"} to
 * /download0/etahen_jailbreak; the daemon polls the sandboxes every 250 ms,
 * raises the process's credentials, points its root at the real root and
 * deletes the file. Protocol and timing from PS5VR's eng_jailbreak.c.
 *
 * spike_title_init() does that, then mirrors spike_log() into
 * /data/ps5webkit/<name>.log (fetch with FTP) and tries the TCP log sink.
 */
#pragma once

#include "spike.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#define SPIKE_JB_FILE "/download0/etahen_jailbreak"

/* open(O_DIRECTORY), not stat(): PS5VR's probe, and stat() is the import
 * that first came back NULL in a title on 13.60. */
static inline int spike_title_unjailed(void)
{
    int fd = open("/data", O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

/* Kernel log sink (klogsrv streams it to the Mac): works before /data and
 * the network do, so start-up problems are visible. */
int sceKernelDebugOutText(int channel, const char *text) __attribute__((weak));
static inline void spike_klog(const char *line)
{
    if (sceKernelDebugOutText)
        sceKernelDebugOutText(0, line);
}

/* An import the console's loader cannot resolve is left NULL and crashes
 * when called. Log any of these that are. */
#define SPIKE_IMPORT(fn) {#fn, (const void *)(fn)}
static inline void spike_title_check_imports(void)
{
    static const struct { const char *name; const void *fn; } k[] = {
        SPIKE_IMPORT(open), SPIKE_IMPORT(close), SPIKE_IMPORT(write), SPIKE_IMPORT(read),
        SPIKE_IMPORT(mkdir), SPIKE_IMPORT(unlink), SPIKE_IMPORT(usleep), SPIKE_IMPORT(socket),
        SPIKE_IMPORT(connect), SPIKE_IMPORT(send), SPIKE_IMPORT(inet_pton), SPIKE_IMPORT(getpid),
        SPIKE_IMPORT(clock_gettime), SPIKE_IMPORT(sceKernelSendNotificationRequest),
    };
    char line[256];
    int missing = 0;
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) {
        if (!k[i].fn) {
            snprintf(line, sizeof line, "[ps5webkit] import %s is NULL\n", k[i].name);
            spike_klog(line);
            missing++;
        }
    }
    snprintf(line, sizeof line, "[ps5webkit] import check: %d NULL\n", missing);
    spike_klog(line);
}

/* Returns 1 if the title now sees the real root. */
static inline int spike_title_unjail(void)
{
    if (spike_title_unjailed())
        return 1;
    for (int attempt = 0; attempt < 3; attempt++) {
        int fd = open(SPIKE_JB_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            return 0;
        char body[48];
        int n = snprintf(body, sizeof body, "{\"PID\":\"%d\"}", (int)getpid());
        int ok = write(fd, body, (size_t)n) == n;
        close(fd);
        if (!ok)
            return 0;
        for (int i = 0; i < 30; i++) { /* 3 s per attempt */
            usleep(100 * 1000);
            if (spike_title_unjailed())
                return 1;
        }
    }
    return 0;
}

/* A title can start its own log instead of the blocking TCP connection
 * below (ps5browser: titlelog.c, a queue and a sender thread no caller
 * waits on). Called after the unjail and the file log; 0: it took over. */
__attribute__((weak)) int (*spike_title_log_start_hook)(const char *name, const char *log_host, int log_port) = 0;

static inline void spike_title_init(const char *name, const char *log_host, int log_port)
{
    char line[160];
    snprintf(line, sizeof line, "[ps5webkit] %s starting, pid %d\n", name, (int)getpid());
    spike_klog(line);
    spike_title_check_imports();
    const int unjailed = spike_title_unjail();
    if (unjailed) {
        mkdir("/data/ps5webkit", 0777);
        char path[128];
        snprintf(path, sizeof path, "/data/ps5webkit/%s.log", name);
        spike_log_file_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    int tcp = -1;
    const char *how = "FAILED";
    if (spike_title_log_start_hook && spike_title_log_start_hook(name, log_host, log_port) == 0)
        tcp = 0, how = "queued (non-blocking sender)";
    else if ((tcp = spike_log_connect(log_host, log_port)) == 0)
        how = "connected";
    snprintf(line, sizeof line, "[ps5webkit] %s unjailed %d tcp %d\n", name, unjailed, tcp);
    spike_klog(line);
    spike_log("title %s pid %d: unjailed %d, file log %s, tcp log to %s:%d %s (errno %d)", name,
              (int)getpid(), unjailed, spike_log_file_fd >= 0 ? "on" : "off", log_host, log_port,
              how, tcp == 0 ? 0 : errno);
}
