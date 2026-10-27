/* Spike 8 server - the WebProcess side of the title <-> payload IPC test.
 *
 * Runs as an elfldr payload (full JIT, 8 GB flexible memory), normally
 * spawned by the Spike 8 title through the local elfldr, as WebKit's
 * UIProcess would launch a WebProcess. Can also be sent by hand
 * (`make send`) before starting the title.
 *
 * Listens on the unix path socket and on loopback TCP (ipc_proto.h), serves
 * sessions one at a time and exits on MSG_QUIT or after 5 minutes.
 */
#define SPIKE_NAME "08-title-ipc-server"
#include "../common/spike.h"
#include "ipc_proto.h"

#include <sys/stat.h>

int sceKernelAvailableFlexibleMemorySize(size_t *);

#define SERVER_LIFETIME_MS (5 * 60 * 1000)

static int listen_unix(void)
{
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", IPC_SOCK_PATH);
    unlink(a.sun_path);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 4) != 0) {
        spike_log("[srv] bind %s: errno %d", a.sun_path, errno);
        close(s);
        return -1;
    }
    /* A title runs under another uid: connect() needs write permission. */
    if (chmod(a.sun_path, 0777) != 0)
        spike_log("[srv] chmod %s: errno %d", a.sun_path, errno);
    return s;
}

static int listen_tcp(void)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(IPC_TCP_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 4) != 0) {
        spike_log("[srv] bind 127.0.0.1:%d: errno %d", IPC_TCP_PORT, errno);
        close(s);
        return -1;
    }
    return s;
}

static int serve(int s, const char *transport, int is_unix, const uint8_t *named);

/* The title listens, the payload connects; then an ordinary session. */
static int connect_back(int ctl, const char *path, const uint8_t *named)
{
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    int err = c < 0 ? errno : 0;
    if (c >= 0 && connect(c, (struct sockaddr *)&a, sizeof a) != 0) {
        err = errno;
        close(c);
        c = -1;
    }
    spike_log("[srv] connect back to %s: %s (errno %d)", path, c >= 0 ? "connected" : "failed", err);
    msg_t ack = ipc_msg(MSG_CONNECT_BACK, (uint32_t)err, 0, NULL);
    ipc_send(ctl, &ack, -1);
    if (c < 0)
        return 0;
    const int quit = serve(c, "unix-back", 1, named);
    close(c);
    return quit;
}

/* One session. Returns 1 when the client asked the server to stop. */
static int serve(int s, const char *transport, int is_unix, const uint8_t *named)
{
    msg_t m;
    int quit = 0;
    if (ipc_recv(s, &m, NULL, 5000) != 0) {
        spike_log("[srv] %s: no hello", transport);
        return 0;
    }
    if (m.type == MSG_QUIT || m.type == MSG_BYE) /* readiness probe or stop */
        return m.type == MSG_QUIT;
    if (m.type == MSG_CONNECT_BACK)
        return connect_back(s, m.text, named);
    spike_log("[srv] %s: client pid %d connected", transport, m.pid);

    int anon_fd = -1;
    uint8_t *anon = is_unix ? ipc_anon_shm(&anon_fd, SEED_SERVER) : NULL;
    msg_t w = ipc_msg(MSG_WELCOME, anon != NULL, IPC_SHM_SIZE, IPC_SHM_NAME);
    if (ipc_send(s, &w, anon_fd) != 0) {
        spike_log("[srv] %s: welcome send errno %d", transport, errno);
        goto out;
    }

    int back_fd = -1;
    if (ipc_recv(s, &m, is_unix ? &back_fd : NULL, 10000) != 0 || m.type != MSG_SHM_DONE) {
        spike_log("[srv] %s: no shm reply", transport);
        goto out;
    }
    const size_t note_len = strnlen(m.text, sizeof m.text);
    uint32_t bits = 0;
    if (anon && note_len && !memcmp(anon + IPC_REPLY_OFF, m.text, note_len))
        bits |= SHM_OK_ANON;
    if (named && note_len && !memcmp(named + IPC_REPLY_OFF, m.text, note_len))
        bits |= SHM_OK_NAMED;
    if (back_fd >= 0) {
        uint8_t *p = ipc_map(back_fd, IPC_SHM_SIZE);
        if (p && ipc_check(p, IPC_SHM_SIZE, SEED_CLIENT))
            bits |= SHM_OK_REVERSE;
        if (p)
            munmap(p, IPC_SHM_SIZE);
        close(back_fd);
    }
    spike_log("[srv] %s: client note \"%s\", shm bits anon %d named %d reverse %d", transport,
              m.text, !!(bits & SHM_OK_ANON), !!(bits & SHM_OK_NAMED), !!(bits & SHM_OK_REVERSE));
    msg_t r = ipc_msg(MSG_SHM_RESULT, bits, 0, NULL);
    if (ipc_send(s, &r, -1) != 0)
        goto out;

    static uint8_t bulk[64 << 10];
    unsigned pings = 0;
    for (;;) {
        if (ipc_recv(s, &m, NULL, 10000) != 0)
            break;
        if (m.type == MSG_PING) {
            pings++;
            if (ipc_write_all(s, &m, sizeof m) != 0)
                break;
        } else if (m.type == MSG_BULK) {
            uint64_t got = 0;
            while (got < m.value) {
                size_t want = m.value - got < sizeof bulk ? (size_t)(m.value - got) : sizeof bulk;
                if (wait_readable(s, 10000) != 0)
                    break;
                ssize_t n = recv(s, bulk, want, 0);
                if (n <= 0)
                    break;
                got += (uint64_t)n;
            }
            msg_t a = ipc_msg(MSG_BULK, 0, got, NULL);
            if (ipc_write_all(s, &a, sizeof a) != 0 || got != m.value)
                break;
        } else {
            spike_log("[srv] %s: session done (%u pings)%s", transport, pings,
                      m.type == MSG_QUIT ? ", told to quit" : "");
            quit = m.type == MSG_QUIT;
            goto out;
        }
    }
    spike_log("[srv] %s: session ended early (%u pings, errno %d)", transport, pings, errno);
out:
    if (anon)
        munmap(anon, IPC_SHM_SIZE);
    if (anon_fd >= 0)
        close(anon_fd);
    return quit;
}

int main(void)
{
#ifdef LOG_HOST
    spike_log_connect(LOG_HOST, LOG_PORT);
#endif
    size_t flex = 0;
    sceKernelAvailableFlexibleMemorySize(&flex);
    spike_log("[srv] ps5webkit spike 08 server: pid %d, parent %d, uid %d, flexible memory %zu MB",
              (int)getpid(), (int)getppid(), (int)getuid(), flex >> 20);

    /* The rendezvous by name: created world-accessible so another uid can
     * open it. */
    shm_unlink(IPC_SHM_NAME);
    uint8_t *named = NULL;
    int nfd = shm_open(IPC_SHM_NAME, O_RDWR | O_CREAT, 0666);
    if (nfd >= 0) {
        fchmod(nfd, 0666);
        if (ftruncate(nfd, IPC_SHM_SIZE) == 0 && (named = ipc_map(nfd, IPC_SHM_SIZE)))
            ipc_fill(named, IPC_SHM_SIZE, SEED_NAMED);
    }
    if (named)
        SPIKE_PASS("named-shm", "%s, %u MB, mode 0666", IPC_SHM_NAME, IPC_SHM_SIZE >> 20);
    else
        SPIKE_FAIL("named-shm", "%s: errno %d", IPC_SHM_NAME, errno);

    ipc_rtt_baseline("[srv]", 20000);

    mkdir(IPC_BACK_DIR, 0777);
    if (chmod(IPC_BACK_DIR, 0777) != 0)
        spike_log("[srv] mkdir/chmod %s: errno %d", IPC_BACK_DIR, errno);

    int ul = listen_unix();
    if (ul >= 0)
        SPIKE_PASS("unix-listen", "%s, mode 0777", IPC_SOCK_PATH);
    else
        SPIKE_FAIL("unix-listen", "%s", IPC_SOCK_PATH);
    int tl = listen_tcp();
    if (tl >= 0)
        SPIKE_PASS("tcp-listen", "127.0.0.1:%d", IPC_TCP_PORT);
    else
        SPIKE_FAIL("tcp-listen", "127.0.0.1:%d", IPC_TCP_PORT);
    spike_log("[srv] ready");

    const double deadline = spike_now_ms() + SERVER_LIFETIME_MS;
    int quit = 0;
    while (!quit && (ul >= 0 || tl >= 0) && spike_now_ms() < deadline) {
        struct pollfd p[2] = {{ul, POLLIN, 0}, {tl, POLLIN, 0}};
        if (poll(p, 2, 1000) <= 0)
            continue;
        for (int i = 0; i < 2 && !quit; i++) {
            if (!(p[i].revents & POLLIN))
                continue;
            int c = accept(p[i].fd, NULL, NULL);
            if (c < 0)
                continue;
            if (i == 1)
                ipc_nodelay(c);
            quit = serve(c, i == 0 ? "unix" : "tcp", i == 0, named);
            close(c);
        }
    }
    spike_log("[srv] stopping (%s)", quit ? "quit" : "timeout");
    if (ul >= 0) {
        close(ul);
        unlink(IPC_SOCK_PATH);
    }
    if (tl >= 0)
        close(tl);
    if (named)
        munmap(named, IPC_SHM_SIZE);
    if (nfd >= 0)
        close(nfd);
    shm_unlink(IPC_SHM_NAME);
    return 0;
}
