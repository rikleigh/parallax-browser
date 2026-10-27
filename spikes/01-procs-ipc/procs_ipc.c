/* Spike 1 - processes, IPC, memory and JIT memory.
 *
 * WebKit runs as a UIProcess plus WebProcess/NetworkProcess children that
 * talk over Unix sockets, pass fds with SCM_RIGHTS and share memory. This
 * payload checks each piece on the console:
 *
 *   threads        many pthreads with large stacks
 *   fork           plain fork() + pipe + waitpid
 *   fork-fdpass    socketpair + SCM_RIGHTS across fork
 *   shm-anon       shm_open(SHM_ANON) shared across fork
 *   shm-named      named POSIX shm (cross-process rendezvous by name)
 *   unix-path      AF_UNIX socket bound to a filesystem path
 *   elfldr-spawn   spawn an embedded child ELF through the local elfldr,
 *                  rendezvous over the path socket, pass shm both ways
 *   dmem-share     the child maps the parent's direct memory by offset
 *                  (cross-process GPU memory: eye buffers, page buffers)
 *   mem-flex       how much anonymous memory one process can commit
 *   mem-direct     how much direct (GPU-visible) memory it can map
 *   jit-rwx        RWX anonymous mapping
 *   jit-mprotect   RW then mprotect(RX)
 *   jit-sce        Sony's sceKernelJit* shared-memory double mapping (what
 *                  the system browser uses)
 *
 * Built with -DPROCS_IPC_LIBRARY the same tests run from inside a
 * registered title (procs_ipc_run_all), which is where WebKit would live.
 *
 * The gate: a WebKit-style multi-process launcher is possible if either
 * fork-fdpass or elfldr-spawn passes. Otherwise the in-process fallback is
 * needed (major risk; see FEASIBILITY.md).
 */
#ifndef SPIKE_NAME
#define SPIKE_NAME "01-procs-ipc"
#endif
#include "../common/spike.h"
#include "ipc_common.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>

int sceKernelAvailableFlexibleMemorySize(size_t *);
int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align, int type,
                                  int64_t *offset);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t offset,
                             size_t align);
int sceKernelReleaseDirectMemory(int64_t start, size_t len);
int sceKernelJitCreateSharedMemory(const char *name, size_t len, int max_prot, int *fd);
int sceKernelJitCreateAliasOfSharedMemory(int fd, int max_prot, int *alias_fd);

/* child.elf, linked in by the Makefile (child_blob.S). */
extern const uint8_t child_elf_start[], child_elf_end[];

static int g_fork_ok;

/* -- threads --------------------------------------------------------------- */
static void *thread_main(void *arg)
{
    volatile char stack_use[256 * 1024];
    stack_use[0] = 1;
    stack_use[sizeof stack_use - 1] = 1;
    return (void *)((intptr_t)arg + stack_use[0]);
}

static __attribute__((unused)) void test_threads(void)
{
    enum { N = 32 };
    pthread_t t[N];
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1u << 20);
    int made = 0, joined = 0;
    for (; made < N; made++)
        if (pthread_create(&t[made], &attr, thread_main, (void *)(intptr_t)made) != 0)
            break;
    for (int i = 0; i < made; i++) {
        void *r;
        if (pthread_join(t[i], &r) == 0 && (intptr_t)r == i + 1)
            joined++;
    }
    if (joined == N)
        SPIKE_PASS("threads", "%d threads with 1 MB stacks", N);
    else
        SPIKE_FAIL("threads", "created %d joined %d of %d", made, joined, N);
}

/* -- fork ------------------------------------------------------------------- */
static int wait_child(pid_t pid, int timeout_ms, int *status)
{
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        pid_t r = waitpid(pid, status, WNOHANG);
        if (r == pid)
            return 0;
        if (r < 0)
            return -1;
        usleep(10000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, status, 0);
    return -1;
}

static __attribute__((unused)) void test_fork(void)
{
    int p[2];
    if (pipe(p) != 0) {
        SPIKE_FAIL("fork", "pipe errno %d", errno);
        return;
    }
    const double t0 = spike_now_ms();
    pid_t pid = fork();
    if (pid == 0) {
        const char m = 'k';
        (void)write(p[1], &m, 1);
        _exit(7);
    }
    if (pid < 0) {
        SPIKE_FAIL("fork", "fork errno %d (%s)", errno, strerror(errno));
        close(p[0]);
        close(p[1]);
        return;
    }
    char m = 0;
    int st = 0;
    const int got = wait_readable(p[0], 3000) == 0 && read(p[0], &m, 1) == 1 && m == 'k';
    const int reaped = wait_child(pid, 3000, &st) == 0;
    close(p[0]);
    close(p[1]);
    if (got && reaped && WIFEXITED(st) && WEXITSTATUS(st) == 7) {
        g_fork_ok = 1;
        SPIKE_PASS("fork", "child %d ran and exited, %.1f ms", (int)pid, spike_now_ms() - t0);
    } else {
        SPIKE_FAIL("fork", "child %d: message %d reaped %d status 0x%x", (int)pid, got, reaped, st);
    }
}

static int make_anon_shm(size_t size, uint8_t **map)
{
    int fd = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }
    *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (*map == MAP_FAILED) {
        close(fd);
        return -1;
    }
    return fd;
}

static __attribute__((unused)) void test_fork_fdpass_and_shm(void)
{
    if (!g_fork_ok) {
        SPIKE_SKIP("fork-fdpass", "fork unavailable");
        SPIKE_SKIP("shm-anon", "tested across fork; fork unavailable");
        return;
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        SPIKE_FAIL("fork-fdpass", "socketpair errno %d", errno);
        return;
    }
    uint8_t *map = NULL;
    int shm = make_anon_shm(SHM_SIZE, &map);
    if (shm < 0)
        SPIKE_FAIL("shm-anon", "shm_open(SHM_ANON)/ftruncate/mmap errno %d", errno);

    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        /* Receive a fresh shm fd over the socket and map it independently. */
        char tag[4];
        int fd = recv_fd(sv[1], tag, sizeof tag);
        int ok = 0;
        if (fd >= 0) {
            uint8_t *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (m != MAP_FAILED) {
                ok = m[0] == 0xab;
                m[1] = 0xcd;
            }
        }
        if (map)
            map[0] = 0x5a; /* the inherited SHM_ANON mapping */
        (void)write(sv[1], &ok, sizeof ok);
        _exit(0);
    }
    close(sv[1]);
    uint8_t *m2 = NULL;
    int fd2 = make_anon_shm(4096, &m2);
    int ok = 0;
    if (fd2 >= 0) {
        m2[0] = 0xab;
        if (send_fd(sv[0], fd2, "shm", 4) == 0 && wait_readable(sv[0], 3000) == 0 &&
            read(sv[0], &ok, sizeof ok) == sizeof ok && ok && m2[1] == 0xcd)
            SPIKE_PASS("fork-fdpass", "shm fd passed with SCM_RIGHTS, both sides see writes");
        else
            SPIKE_FAIL("fork-fdpass", "child ok %d, parent sees 0x%02x", ok, m2[1]);
    } else {
        SPIKE_FAIL("fork-fdpass", "could not make the shm to pass (errno %d)", errno);
    }
    int st;
    wait_child(pid, 3000, &st);
    if (shm >= 0) {
        if (map[0] == 0x5a)
            SPIKE_PASS("shm-anon", "SHM_ANON mapping shared across fork");
        else
            SPIKE_FAIL("shm-anon", "child write not visible (0x%02x)", map[0]);
        munmap(map, SHM_SIZE);
        close(shm);
    }
    close(sv[0]);
}

static __attribute__((unused)) void test_shm_named(void)
{
    const char *name = "/ps5webkit-spike01";
    shm_unlink(name);
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        SPIKE_FAIL("shm-named", "shm_open create errno %d (%s)", errno, strerror(errno));
        return;
    }
    int fd2 = shm_open(name, O_RDWR, 0600);
    int ok = fd2 >= 0 && ftruncate(fd, 65536) == 0;
    if (ok) {
        uint8_t *a = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        uint8_t *b = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0);
        ok = a != MAP_FAILED && b != MAP_FAILED;
        if (ok) {
            a[100] = 0x42;
            ok = b[100] == 0x42;
        }
    }
    if (ok)
        SPIKE_PASS("shm-named", "create, reopen by name, shared mapping");
    else
        SPIKE_FAIL("shm-named", "reopen fd %d errno %d", fd2, errno);
    shm_unlink(name);
    close(fd);
    if (fd2 >= 0)
        close(fd2);
}

/* -- path socket + elfldr spawn --------------------------------------------- */
static int listen_path_socket(char *path_out, size_t size)
{
    mkdir("/data/ps5webkit", 0777);
    for (size_t i = 0; i < sizeof kSockDirs / sizeof *kSockDirs; i++) {
        struct sockaddr_un a;
        memset(&a, 0, sizeof a);
        a.sun_family = AF_UNIX;
        sock_path(a.sun_path, sizeof a.sun_path, kSockDirs[i]);
        unlink(a.sun_path);
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s < 0)
            continue;
        if (bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, 4) == 0) {
            snprintf(path_out, size, "%s", a.sun_path);
            return s;
        }
        spike_log("  bind %s: errno %d", a.sun_path, errno);
        close(s);
    }
    return -1;
}

/* Upload an ELF to the local elfldr; returns the connection, which carries
 * the child's stdout. */
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

static void drain_child_output(int s)
{
    char buf[512];
    while (wait_readable(s, 50) == 0) {
        ssize_t n = read(s, buf, sizeof buf - 1);
        if (n <= 0)
            break;
        buf[n] = 0;
        for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n"))
            spike_log("  | %s", line);
    }
}

static __attribute__((unused)) void test_unix_path_and_spawn(int spawn)
{
    char path[108];
    int ls = listen_path_socket(path, sizeof path);
    if (ls < 0) {
        SPIKE_FAIL("unix-path", "no writable directory for an AF_UNIX socket");
        SPIKE_SKIP("elfldr-spawn", "needs unix-path");
        return;
    }
    /* Self-connect first: proves the path socket and SCM_RIGHTS in-process. */
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    int self_ok = c >= 0 && connect(c, (struct sockaddr *)&a, sizeof a) == 0;
    int acc = self_ok ? accept(ls, NULL, NULL) : -1;
    if (acc >= 0) {
        int p[2];
        char tag[4];
        self_ok = pipe(p) == 0 && send_fd(c, p[1], "self", 4) == 0;
        int got = self_ok ? recv_fd(acc, tag, sizeof tag) : -1;
        self_ok = got >= 0 && write(got, "x", 1) == 1;
        if (got >= 0)
            close(got);
        close(acc);
    }
    if (c >= 0)
        close(c);
    if (self_ok)
        SPIKE_PASS("unix-path", "%s, SCM_RIGHTS over it", path);
    else
        SPIKE_FAIL("unix-path", "%s: self connect/fd pass failed errno %d", path, errno);

    if (!spawn) {
        close(ls);
        unlink(path);
        return;
    }
    /* Spawn the child through elfldr. */
    const size_t len = (size_t)(child_elf_end - child_elf_start);
    const double t0 = spike_now_ms();
    int out = elfldr_send(child_elf_start, len);
    if (out < 0) {
        SPIKE_FAIL("elfldr-spawn", "cannot reach elfldr on 127.0.0.1:9021 (errno %d)", errno);
        close(ls);
        unlink(path);
        return;
    }
    if (wait_readable(ls, 10000) != 0) {
        drain_child_output(out);
        SPIKE_FAIL("elfldr-spawn", "child never connected to %s within 10 s", path);
        close(out);
        close(ls);
        unlink(path);
        return;
    }
    int cs = accept(ls, NULL, NULL);
    const double t_connect = spike_now_ms() - t0;

    uint8_t *map = NULL;
    int shm = make_anon_shm(SHM_SIZE, &map);
    int ok = shm >= 0;
    shm_report_t *r = (shm_report_t *)map;
    int64_t doff = -1;
    volatile uint8_t *dmem = NULL;
    if (ok) {
        memset(map, 0, sizeof(shm_report_t));
        for (size_t i = sizeof(shm_report_t); i < SHM_SIZE; i++)
            map[i] = (uint8_t)(i & 0xff);
        void *va = NULL;
        if (sceKernelAllocateDirectMemory(0, (int64_t)16 << 30, DMEM_SIZE, 0x10000, 12, &doff) == 0 &&
            sceKernelMapDirectMemory(&va, DMEM_SIZE, 0x33, 0, doff, 0x10000) == 0) {
            dmem = va;
            dmem[0] = 0xa5;
            dmem[1] = 0;
        } else {
            doff = -1;
        }
        r->dmem_offset = doff;
        r->dmem_size = doff >= 0 ? DMEM_SIZE : 0;
        ok = send_fd(cs, shm, "shm", 4) == 0;
    }
    for (int i = 0; ok && i < 300 && __atomic_load_n(&r->magic, __ATOMIC_ACQUIRE) != SHM_MAGIC; i++)
        usleep(10000);
    int reverse = -1;
    char tag[8] = {0}, msg[64] = {0};
    if (ok && wait_readable(cs, 3000) == 0)
        reverse = recv_fd(cs, tag, sizeof tag);
    if (reverse >= 0 && wait_readable(reverse, 2000) == 0)
        (void)read(reverse, msg, sizeof msg - 1);
    close(cs); /* lets the child finish */
    drain_child_output(out);

    if (ok && r->magic == SHM_MAGIC && r->pattern_ok && reverse >= 0 && msg[0])
        SPIKE_PASS("elfldr-spawn",
                   "child pid %d connected in %.0f ms; shm both ways; child flex %llu MB; "
                   "reverse pipe: \"%s\"",
                   r->child_pid, t_connect, (unsigned long long)r->child_flex_mb, msg);
    else
        SPIKE_FAIL("elfldr-spawn", "shm %d magic 0x%x pattern %u reverse fd %d msg \"%s\"", ok,
                   map ? r->magic : 0, map ? r->pattern_ok : 0, reverse, msg);
    if (doff < 0)
        SPIKE_SKIP("dmem-share", "parent could not allocate direct memory");
    else if (ok && r->magic == SHM_MAGIC && r->dmem_result == 1 && dmem[1] == 0x5a)
        SPIKE_PASS("dmem-share", "child mapped the parent's direct memory at offset 0x%llx; "
                   "writes visible both ways", (unsigned long long)doff);
    else
        SPIKE_FAIL("dmem-share", "child result %d (1 ok, 2 wrong data, <0 map error), parent sees "
                   "0x%02x", map ? r->dmem_result : 0, dmem ? dmem[1] : 0);
    if (doff >= 0)
        sceKernelReleaseDirectMemory(doff, DMEM_SIZE);
    if (shm >= 0) {
        munmap(map, SHM_SIZE);
        close(shm);
    }
    close(out);
    close(ls);
    unlink(path);
}

/* -- memory ----------------------------------------------------------------- */
#define MEM_CHUNK  (256u << 20)
#define MEM_CAP_MB 6144u /* stop here even if more is available */

static __attribute__((unused)) void test_mem_flex(void)
{
    size_t before = 0;
    sceKernelAvailableFlexibleMemorySize(&before);
    void *chunks[MEM_CAP_MB / 256];
    /* Stay 1 GB clear of what the system says is free: running a console
     * out of memory is not the question here. */
    size_t limit = sizeof chunks / sizeof *chunks;
    if (before >> 20 < 1024u + 256u)
        limit = 0;
    else if ((before >> 20) - 1024u < MEM_CAP_MB)
        limit = ((before >> 20) - 1024u) / 256u;
    unsigned n = 0;
    for (; n < limit; n++) {
        uint8_t *p = mmap(NULL, MEM_CHUNK, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED)
            break;
        for (size_t off = 0; off < MEM_CHUNK; off += 4096) /* commit it */
            p[off] = 1;
        chunks[n] = p;
    }
    for (unsigned i = 0; i < n; i++)
        munmap(chunks[i], MEM_CHUNK);
    SPIKE_PASS("mem-flex", "committed %u MB anonymous (limit %u MB, errno %d at stop); "
               "flexible available beforehand %zu MB",
               n * 256u, (unsigned)(limit * 256u), n >= limit ? 0 : errno, before >> 20);
}

static __attribute__((unused)) void test_mem_direct(void)
{
    int64_t offs[16];
    unsigned n = 0;
    for (; n < 8; n++) { /* 8 x 256 MB = 2 GB cap */
        int64_t off = 0;
        void *va = NULL;
        if (sceKernelAllocateDirectMemory(0, (int64_t)16 << 30, MEM_CHUNK, 0x10000, 12, &off) != 0)
            break;
        if (sceKernelMapDirectMemory(&va, MEM_CHUNK, 0x33, 0, off, 0x10000) != 0) {
            sceKernelReleaseDirectMemory(off, MEM_CHUNK);
            break;
        }
        offs[n] = off;
    }
    for (unsigned i = 0; i < n; i++)
        sceKernelReleaseDirectMemory(offs[i], MEM_CHUNK);
    if (n)
        SPIKE_PASS("mem-direct", "mapped %u MB of direct memory (cap 2048 MB)", n * 256u);
    else
        SPIKE_FAIL("mem-direct", "no direct memory could be allocated");
}

/* -- JIT memory --------------------------------------------------------------- */
static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_fault;

static void on_fault(int sig)
{
    g_fault = sig;
    siglongjmp(g_jmp, 1);
}

/* mov eax, 42; ret */
static const uint8_t kCode[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};

/* Runs code at x (already holding kCode); 42 on success, -1 on a fault. */
static int try_call(void *x)
{
    struct sigaction sa, old_segv, old_bus;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fault;
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);
    int r = -1;
    g_fault = 0;
    if (sigsetjmp(g_jmp, 1) == 0)
        r = ((int (*)(void))x)();
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);
    return r;
}

static __attribute__((unused)) void test_jit(void)
{
    const size_t sz = 0x4000;

    uint8_t *p = mmap(NULL, sz, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        SPIKE_FAIL("jit-rwx", "mmap RWX errno %d", errno);
    } else {
        memcpy(p, kCode, sizeof kCode);
        int r = try_call(p);
        if (r == 42)
            SPIKE_PASS("jit-rwx", "RWX anonymous page executes");
        else
            SPIKE_FAIL("jit-rwx", "mapped but executing faulted (signal %d)", (int)g_fault);
        munmap(p, sz);
    }

    p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p != MAP_FAILED) {
        memcpy(p, kCode, sizeof kCode);
        if (mprotect(p, sz, PROT_READ | PROT_EXEC) != 0)
            SPIKE_FAIL("jit-mprotect", "mprotect RX errno %d", errno);
        else if (try_call(p) == 42)
            SPIKE_PASS("jit-mprotect", "RW -> RX via mprotect executes");
        else
            SPIKE_FAIL("jit-mprotect", "mprotect ok but executing faulted (signal %d)", (int)g_fault);
        munmap(p, sz);
    }

    int xfd = -1, wfd = -1;
    int rc = sceKernelJitCreateSharedMemory("ps5webkit-jit", sz, PROT_READ | PROT_WRITE | PROT_EXEC,
                                            &xfd);
    if (rc != 0) {
        SPIKE_FAIL("jit-sce", "sceKernelJitCreateSharedMemory 0x%08x", rc);
        return;
    }
    rc = sceKernelJitCreateAliasOfSharedMemory(xfd, PROT_READ | PROT_WRITE, &wfd);
    if (rc != 0) {
        SPIKE_FAIL("jit-sce", "sceKernelJitCreateAliasOfSharedMemory 0x%08x", rc);
        close(xfd);
        return;
    }
    uint8_t *w = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, wfd, 0);
    uint8_t *x = mmap(NULL, sz, PROT_READ | PROT_EXEC, MAP_SHARED, xfd, 0);
    if (w == MAP_FAILED || x == MAP_FAILED) {
        SPIKE_FAIL("jit-sce", "mapping the alias pair failed (w %p x %p errno %d)", (void *)w,
                   (void *)x, errno);
    } else {
        memcpy(w, kCode, sizeof kCode);
        if (try_call(x) == 42)
            SPIKE_PASS("jit-sce", "W^X double mapping: write via alias, execute via the other");
        else
            SPIKE_FAIL("jit-sce", "mapped but executing faulted (signal %d)", (int)g_fault);
    }
    if (w != MAP_FAILED)
        munmap(w, sz);
    if (x != MAP_FAILED)
        munmap(x, sz);
    close(wfd);
    close(xfd);
}

/* Groups, chosen at build time (elfldr passes no argv). The first console
 * run panicked the kernel right after fork-fdpass: a forked child holding
 * shared-memory mappings exited. So risky tests run in their own payloads:
 *   1 safe   no child processes, no JIT
 *   2 spawn  elfldr-spawn + dmem-share (a child maps shared memory, exits)
 *   3 jit    JIT memory probes
 *   4 fork   fork tests incl. fork-fdpass/shm-anon (PANICKED on 13.60)
 *   5 title  safe + jit, for the run from inside a registered title
 */
#ifndef SPIKE01_GROUP
#define SPIKE01_GROUP 1
#endif

#define RUN(test, call)                                                  \
    do {                                                                 \
        spike_log("start %s", test);                                     \
        call;                                                            \
    } while (0)

void procs_ipc_run_all(void);
void procs_ipc_run_all(void)
{
    spike_log("ps5webkit spike 01 group %d (pid %d)", SPIKE01_GROUP, (int)getpid());
#if SPIKE01_GROUP == 1
    RUN("threads", test_threads());
    RUN("shm-named", test_shm_named());
    RUN("unix-path", test_unix_path_and_spawn(0));
    RUN("mem-flex", test_mem_flex());
    RUN("mem-direct", test_mem_direct());
#elif SPIKE01_GROUP == 2
    RUN("elfldr-spawn", test_unix_path_and_spawn(1));
#elif SPIKE01_GROUP == 3
    RUN("jit", test_jit());
#elif SPIKE01_GROUP == 5
    RUN("threads", test_threads());
    RUN("shm-named", test_shm_named());
    RUN("unix-path", test_unix_path_and_spawn(0));
    RUN("mem-flex", test_mem_flex());
    RUN("mem-direct", test_mem_direct());
    RUN("jit", test_jit());
#elif SPIKE01_GROUP == 4
    RUN("fork", test_fork());
    RUN("fork-fdpass", test_fork_fdpass_and_shm());
#endif
}

#ifndef PROCS_IPC_LIBRARY
int main(void)
{
    procs_ipc_run_all();
    spike_notify("ps5webkit: spike 01 done");
    spike_log("done");
    return 0;
}
#endif
