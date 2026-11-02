/*
 * ps.c - list processes on the console, and optionally SIGKILL some: a line
 * on stdin "kill <pid> [<pid>...]" or "killname <name>" (e.g. a payload hung
 * in a system call).
 *   { cat ps.elf; echo "killname audioprobe"; } | socat -t 10 - TCP:$PS5_HOST:9021
 *   prospero-clang -O2 -o ps.elf ps.c   (env.sh, then the SDK's prospero.sh)
 *
 * One line per process: pid, ppid, state (S* letter), P_* flags, tracer pid,
 * virtual and resident size, CPU time, threads, age, ki_comm, thread name
 * and wait channel. Payloads that elfldr starts are all named payload.elf
 * (their parent is elfldr); see port/webkit/CONSOLE-HEALTH.md.
 */
#include <sys/types.h>
#include <sys/param.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/user.h>
#include <unistd.h>

static char stateLetter(char stat)
{
    switch (stat) {
    case SIDL: return 'I';
    case SRUN: return 'R';
    case SSLEEP: return 'S';
    case SSTOP: return 'T';
    case SZOMB: return 'Z';
    case SWAIT: return 'W';
    case SLOCK: return 'L';
    default: return '?';
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    char line[256] = "";
    struct pollfd p = { 0, POLLIN, 0 };
    if (poll(&p, 1, 1500) == 1) {
        ssize_t n = read(0, line, sizeof line - 1);
        line[n > 0 ? n : 0] = 0;
    }
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0 };
    size_t len = 0;
    if (sysctl(mib, 4, NULL, &len, NULL, 0)) {
        perror("sysctl size");
        return 1;
    }
    len += 64 * 1024;
    char* buf = malloc(len);
    if (sysctl(mib, 4, buf, &len, NULL, 0)) {
        perror("sysctl");
        return 1;
    }
    char killName[64] = "";
    if (!strncmp(line, "killname ", 9))
        sscanf(line + 9, "%63s", killName);
    struct timeval now;
    gettimeofday(&now, NULL);
    long pageKB = getpagesize() / 1024;
    printf("kinfo_proc size %d (header %zu), %zu bytes, page %ld KB\n", ((struct kinfo_proc*)buf)->ki_structsize, sizeof(struct kinfo_proc), len, pageKB);
    printf("%5s %5s %s %8s %5s %8s %7s %8s %4s %7s %-20s %-20s %s\n", "pid", "ppid", "S", "flag", "trc", "vsz MB", "rss MB", "cpu s", "thr", "age s", "comm", "tdname", "wmesg");
    int payloads = 0;
    double payloadVsz = 0;
    for (size_t off = 0; off < len;) {
        struct kinfo_proc* k = (struct kinfo_proc*)(buf + off);
        if (k->ki_structsize <= 0)
            break;
        double vsz = k->ki_size / 1048576.0, rss = k->ki_rssize * pageKB / 1024.0;
        long age = k->ki_start.tv_sec ? (long)(now.tv_sec - k->ki_start.tv_sec) : -1;
        printf("pid %5d %5d %c %8lx %5d %8.1f %7.1f %8.2f %4d %7ld %-20s %-20s %s\n", k->ki_pid, k->ki_ppid, stateLetter(k->ki_stat),
            (unsigned long)k->ki_flag, k->ki_tracer, vsz, rss, k->ki_runtime / 1e6, k->ki_numthreads, age, k->ki_comm, k->ki_tdname, k->ki_wmesg);
        if (!strcmp(k->ki_comm, "payload.elf")) {
            payloads++;
            payloadVsz += vsz;
        }
        if (killName[0] && strstr(k->ki_comm, killName) && k->ki_pid != getpid())
            printf("  kill %d -> %d\n", k->ki_pid, kill(k->ki_pid, SIGKILL));
        off += k->ki_structsize;
    }
    printf("payload.elf processes: %d (this one included), %.1f MB virtual\n", payloads, payloadVsz);
    if (!strncmp(line, "kill ", 5)) {
        char* s = line + 5;
        for (char* t = strtok(s, " \n"); t; t = strtok(NULL, " \n")) {
            int pid = atoi(t);
            if (pid > 1)
                printf("kill %d -> %d\n", pid, kill(pid, SIGKILL));
        }
    }
    return 0;
}
