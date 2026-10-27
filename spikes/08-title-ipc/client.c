/* Spike 8 - title <-> payload IPC, the link the GPU-process architecture
 * rests on (FEASIBILITY §4.4b): the title (UIProcess + GPU process: headset,
 * GL, ≥2 GB direct memory) launches and talks to payloads (WebProcess,
 * NetworkProcess: 8 GB flexible memory, full JIT).
 *
 *   spawn-from-title   the title hands the embedded server.elf to the local
 *                      elfldr (127.0.0.1:9021); skipped when a server is
 *                      already running (`make send`)
 *   unix-*             connect to the payload's /system_tmp socket; shm fds
 *                      both ways with SCM_RIGHTS; named shm; latency; bulk
 *   tcp-*              the same over loopback TCP, shm by name only
 *   exit-app           leave through sceShellCoreUtilExitApp, so closing the
 *                      title no longer shows a debug error
 */
#define SPIKE_NAME "08-title-ipc"
#include "../common/title.h"
#include "../common/psvr2/psvr2.h"
#include "ipc_proto.h"

#include <signal.h>

#ifndef LOG_HOST
#define LOG_HOST "192.168.1.20"
#define LOG_PORT 9999
#endif

#define PINGS      20000
#define BULK_BYTES (64u << 20)

/* server.elf, linked in by build-title.sh (server_blob.S). */
extern const uint8_t server_elf_start[], server_elf_end[];

static int connect_tcp(void)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(IPC_TCP_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        close(s);
        return -1;
    }
    ipc_nodelay(s);
    return s;
}

static int connect_unix(void)
{
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", IPC_SOCK_PATH);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        int e = errno;
        close(s);
        errno = e;
        return -1;
    }
    return s;
}

/* Upload an ELF to the local elfldr (as procs_ipc.c); the returned
 * connection carries the payload's stdout. */
static int elfldr_send(const uint8_t *elf, size_t len)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(9021);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        if (s >= 0)
            close(s);
        return -1;
    }
    for (size_t off = 0; off < len;) {
        ssize_t n = write(s, elf + off, len - off);
        if (n <= 0) {
            close(s);
            return -1;
        }
        off += (size_t)n;
    }
    shutdown(s, SHUT_WR); /* elfldr reads to EOF, then spawns */
    return s;
}

/* Poll until the server accepts on TCP; it creates both listeners first. */
static int wait_for_server(int timeout_ms)
{
    const double end = spike_now_ms() + timeout_ms;
    do {
        int s = connect_tcp();
        if (s >= 0) {
            msg_t bye = ipc_msg(MSG_BYE, 0, 0, NULL);
            ipc_send(s, &bye, -1);
            close(s);
            return 0;
        }
        usleep(50 * 1000);
    } while (spike_now_ms() < end);
    return -1;
}

static void result(const char *t, const char *test, int ok, const char *detail)
{
    char name[48];
    snprintf(name, sizeof name, "%s-%s", t, test);
    if (ok)
        SPIKE_PASS(name, "%s", detail);
    else
        SPIKE_FAIL(name, "%s", detail);
}

/* One session over an open connection. */
static void session(int s, const char *t, int is_unix)
{
    char d[200];
    msg_t hello = ipc_msg(MSG_HELLO, 0, 0, t), m;
    int fd = -1;
    if (ipc_send(s, &hello, -1) != 0 || ipc_recv(s, &m, is_unix ? &fd : NULL, 5000) != 0 ||
        m.type != MSG_WELCOME) {
        snprintf(d, sizeof d, "no welcome from the server (errno %d)", errno);
        result(t, "handshake", 0, d);
        return;
    }
    spike_log("%s: server pid %d, named shm %s", t, m.pid, m.text);

    char note[40];
    snprintf(note, sizeof note, "title %d via %s", (int)getpid(), t);
    const size_t note_len = strlen(note) + 1;

    /* The server's anonymous shm, passed as an fd. */
    int anon_ok = 0;
    if (is_unix) {
        uint8_t *p = fd >= 0 ? ipc_map(fd, IPC_SHM_SIZE) : NULL;
        anon_ok = p && ipc_check(p, IPC_SHM_SIZE, SEED_SERVER);
        if (p) {
            memcpy(p + IPC_REPLY_OFF, note, note_len);
            munmap(p, IPC_SHM_SIZE);
        }
        if (fd >= 0)
            close(fd);
        spike_log("%s: received fd %d, pattern %s", t, fd, anon_ok ? "ok" : "wrong/unmapped");
    }

    /* The server's named shm. */
    int named_local = 0;
    int nfd = shm_open(m.text, O_RDWR, 0);
    if (nfd >= 0) {
        uint8_t *p = ipc_map(nfd, IPC_SHM_SIZE);
        named_local = p && ipc_check(p, IPC_SHM_SIZE, SEED_NAMED);
        if (p) {
            memcpy(p + IPC_REPLY_OFF, note, note_len);
            munmap(p, IPC_SHM_SIZE);
        }
        close(nfd);
    }
    const int named_errno = nfd < 0 ? errno : 0;

    /* Our own shm, passed back. */
    int back_fd = -1;
    uint8_t *back = is_unix ? ipc_anon_shm(&back_fd, SEED_CLIENT) : NULL;
    msg_t done = ipc_msg(MSG_SHM_DONE, 0, 0, note);
    if (ipc_send(s, &done, back_fd) != 0 || ipc_recv(s, &m, NULL, 5000) != 0 ||
        m.type != MSG_SHM_RESULT) {
        snprintf(d, sizeof d, "no shm result from the server (errno %d)", errno);
        result(t, "handshake", 0, d);
        return;
    }
    if (back) {
        munmap(back, IPC_SHM_SIZE);
        close(back_fd);
    }
    if (is_unix) {
        snprintf(d, sizeof d, "server's shm fd: pattern %s, our note %s", anon_ok ? "ok" : "bad",
                 (m.arg & SHM_OK_ANON) ? "seen by the server" : "not seen");
        result(t, "fdpass-in", anon_ok && (m.arg & SHM_OK_ANON), d);
        snprintf(d, sizeof d, "title's shm fd mapped by the server, pattern %s",
                 (m.arg & SHM_OK_REVERSE) ? "ok" : "bad");
        result(t, "fdpass-out", !!(m.arg & SHM_OK_REVERSE), d);
    }
    if (nfd < 0)
        snprintf(d, sizeof d, "shm_open(%s) errno %d (%s)", IPC_SHM_NAME, named_errno,
                 strerror(named_errno));
    else
        snprintf(d, sizeof d, "%s: pattern %s, our note %s", IPC_SHM_NAME,
                 named_local ? "ok" : "bad", (m.arg & SHM_OK_NAMED) ? "seen by the server" : "not seen");
    result(t, "named-shm", named_local && (m.arg & SHM_OK_NAMED), d);

    /* Latency: WebKit IPC is many small messages. */
    double worst = 0;
    int n = 0;
    const double t0 = spike_now_ms();
    for (; n < PINGS; n++) {
        msg_t ping = ipc_msg(MSG_PING, (uint32_t)n, 0, NULL), echo;
        const double a = spike_now_ms();
        if (ipc_write_all(s, &ping, sizeof ping) != 0 || ipc_read_all(s, &echo, sizeof echo, 2000) != 0 ||
            echo.arg != (uint32_t)n)
            break;
        const double dt = spike_now_ms() - a;
        if (dt > worst)
            worst = dt;
    }
    const double total = spike_now_ms() - t0;
    snprintf(d, sizeof d, "%d/%d round trips of 64 B: avg %.1f us, worst %.2f ms", n, PINGS,
             n ? total * 1e3 / n : 0.0, worst);
    result(t, "rtt", n == PINGS, d);

    /* Throughput: what a socket-only page-buffer path would get. */
    static uint8_t chunk[64 << 10];
    memset(chunk, 0xa5, sizeof chunk);
    msg_t bulk = ipc_msg(MSG_BULK, 0, BULK_BYTES, NULL), ack;
    const double b0 = spike_now_ms();
    int ok = ipc_write_all(s, &bulk, sizeof bulk) == 0;
    for (uint32_t sent = 0; ok && sent < BULK_BYTES; sent += sizeof chunk)
        ok = ipc_write_all(s, chunk, sizeof chunk) == 0;
    ok = ok && ipc_read_all(s, &ack, sizeof ack, 10000) == 0 && ack.value == BULK_BYTES;
    const double bms = spike_now_ms() - b0;
    snprintf(d, sizeof d, "%u MB in %.1f ms: %.0f MB/s", BULK_BYTES >> 20, bms,
             bms > 0 ? (BULK_BYTES >> 20) * 1e3 / bms : 0.0);
    result(t, "bulk", ok, d);

    msg_t bye = ipc_msg(MSG_BYE, 0, 0, NULL);
    ipc_send(s, &bye, -1);
}

static void unix_back(void)
{
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", IPC_BACK_PATH);
    unlink(a.sun_path);
    int l = socket(AF_UNIX, SOCK_STREAM, 0);
    if (l < 0 || bind(l, (struct sockaddr *)&a, sizeof a) != 0 || listen(l, 1) != 0) {
        SPIKE_FAIL("unix-back-listen", "%s errno %d (%s)", IPC_BACK_PATH, errno, strerror(errno));
        if (l >= 0)
            close(l);
        return;
    }
    chmod(a.sun_path, 0777);
    SPIKE_PASS("unix-back-listen", "%s", IPC_BACK_PATH);
    int ctl = connect_tcp();
    msg_t req = ipc_msg(MSG_CONNECT_BACK, 0, 0, IPC_BACK_PATH), ack;
    if (ctl < 0 || ipc_send(ctl, &req, -1) != 0 || ipc_recv(ctl, &ack, NULL, 5000) != 0) {
        SPIKE_FAIL("unix-back-connect", "no answer over tcp");
    } else if (ack.arg != 0) {
        SPIKE_FAIL("unix-back-connect", "payload's connect errno %u (%s)", ack.arg,
                   strerror((int)ack.arg));
    } else if (wait_readable(l, 3000) != 0) {
        SPIKE_FAIL("unix-back-connect", "payload connected but accept() saw nothing");
    } else {
        int s = accept(l, NULL, NULL);
        if (s < 0) {
            SPIKE_FAIL("unix-back-connect", "accept errno %d (%s)", errno, strerror(errno));
        } else {
            SPIKE_PASS("unix-back-connect", "payload connected to the title's socket");
            session(s, "unix-back", 1);
            close(s);
        }
    }
    if (ctl >= 0)
        close(ctl);
    close(l);
    unlink(IPC_BACK_PATH);
}

static void log_line(const char *line)
{
    spike_log("%s", line);
}

static void drain_server_output(int s)
{
    char buf[512];
    while (s >= 0 && wait_readable(s, 50) == 0) {
        ssize_t n = read(s, buf, sizeof buf - 1);
        if (n <= 0)
            break;
        buf[n] = 0;
        for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n"))
            spike_log("  | %s", line);
    }
}

int main(void)
{
    spike_title_init("spike08", LOG_HOST, LOG_PORT);
    spike_log("ps5webkit spike 08: title <-> payload IPC (pid %d, uid %d)", (int)getpid(),
              (int)getuid());
    spike_notify("ps5webkit spike 08: title <-> payload IPC");
    ipc_rtt_baseline("title", 20000);

    /* 1. Launch the WebProcess stand-in, as WebKit's ProcessLauncher would. */
    int out = -1;
    if (wait_for_server(0) == 0) {
        SPIKE_SKIP("spawn-from-title", "a server is already running");
    } else {
        const double t0 = spike_now_ms();
        out = elfldr_send(server_elf_start, (size_t)(server_elf_end - server_elf_start));
        if (out < 0) {
            SPIKE_FAIL("spawn-from-title", "connect/upload to elfldr 127.0.0.1:9021 errno %d", errno);
            spike_log("waiting 120 s for a server sent by hand: make -C spikes/08-title-ipc send");
            wait_for_server(120 * 1000);
        } else if (wait_for_server(10 * 1000) == 0) {
            SPIKE_PASS("spawn-from-title", "server.elf (%zu KB) through elfldr, listening after %.0f ms",
                       (size_t)(server_elf_end - server_elf_start) >> 10, spike_now_ms() - t0);
        } else {
            SPIKE_FAIL("spawn-from-title", "elfldr took the ELF but nothing listened within 10 s");
        }
    }
    drain_server_output(out);

    /* 2. unix */
    struct stat st;
    if (stat(IPC_SOCK_PATH, &st) == 0)
        spike_log("unix: %s mode %o uid %d", IPC_SOCK_PATH, (unsigned)(st.st_mode & 07777),
                  (int)st.st_uid);
    else
        spike_log("unix: stat %s errno %d", IPC_SOCK_PATH, errno);
    int s = connect_unix();
    if (s < 0) {
        SPIKE_FAIL("unix-connect", "%s errno %d (%s)", IPC_SOCK_PATH, errno, strerror(errno));
    } else {
        SPIKE_PASS("unix-connect", "%s", IPC_SOCK_PATH);
        session(s, "unix", 1);
        close(s);
    }
    drain_server_output(out);

    /* 2b. unix, reversed: the title listens, the payload connects */
    unix_back();
    drain_server_output(out);

    /* 3. tcp */
    s = connect_tcp();
    if (s < 0) {
        SPIKE_FAIL("tcp-connect", "127.0.0.1:%d errno %d", IPC_TCP_PORT, errno);
    } else {
        SPIKE_PASS("tcp-connect", "127.0.0.1:%d", IPC_TCP_PORT);
        session(s, "tcp", 0);
        close(s);
    }

    /* 4. stop the server */
    s = connect_tcp();
    if (s < 0)
        s = connect_unix();
    if (s >= 0) {
        msg_t quit = ipc_msg(MSG_QUIT, 0, 0, NULL);
        ipc_send(s, &quit, -1);
        close(s);
    }
    usleep(300 * 1000);
    drain_server_output(out);
    if (out >= 0)
        close(out);

    /* 5. leave cleanly */
    spike_log("done; closing the title through sceShellCoreUtilExitApp");
    psvr2_set_logger(log_line);
    const int rc = psvr2_exit_app();
    if (rc == 0)
        SPIKE_PASS("exit-app", "sceShellCoreUtilExitApp asked ShellUI to close the title");
    else
        SPIKE_FAIL("exit-app", "sceShellCoreUtilExitApp -> 0x%08x", (unsigned)rc);
    for (int i = 0; rc == 0 && i < 100; i++)
        usleep(100 * 1000); /* ShellUI ends the process */
    spike_log("still running after exit-app; ending the process");
    kill(getpid(), SIGKILL);
    return 0;
}
