/*
 * ps.c - list processes (pid, ppid, state, name) on the console, and
 * optionally SIGKILL some: a line on stdin "kill <pid> [<pid>...]" or
 * "killname <name>" (e.g. a payload hung in a system call).
 *   { cat ps.elf; echo "killname audioprobe"; } | socat -t 10 - TCP:$PS5_HOST:9021
 */
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>

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
    printf("kinfo_proc size %zu (header %zu), %zu bytes\n", ((struct kinfo_proc*)buf)->ki_structsize, sizeof(struct kinfo_proc), len);
    for (size_t off = 0; off < len;) {
        struct kinfo_proc* k = (struct kinfo_proc*)(buf + off);
        if (k->ki_structsize <= 0)
            break;
        printf("pid %5d ppid %5d stat %d %-20s %s\n", k->ki_pid, k->ki_ppid, k->ki_stat, k->ki_comm, k->ki_wmesg);
        if (killName[0] && strstr(k->ki_comm, killName) && k->ki_pid != getpid())
            printf("  kill %d -> %d\n", k->ki_pid, kill(k->ki_pid, SIGKILL));
        off += k->ki_structsize;
    }
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
