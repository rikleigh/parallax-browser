/* shotrelay: start a payload that is already on the console's disk, so a
 * harness run does not send the 129 MB ps5wk-shot ELF over the network
 * each time (run-shot.sh SHOT_REMOTE=<path>). Reads one line from stdin,
 * "<elf path> <argument line>", sends the file and then the argument line
 * to the local elfldr (127.0.0.1:9021, as the process manager sends the
 * children), and copies what the payload prints back to stdout until it
 * closes the connection.
 *   prospero-clang -O2 -o shotrelay.elf shotrelay.c   (env.sh, then the SDK's prospero.sh)
 *   { cat shotrelay.elf; echo "/data/ps5webkit/prof/ps5wk-shot.elf url=..."; } | socat - TCP:$PS5_HOST:9021 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int writeAll(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int main(void)
{
    static char line[4096], buffer[1 << 16];
    size_t len = 0;
    signal(SIGPIPE, SIG_IGN); /* a closed connection is a write error, not a kill */
    /* One line, byte by byte: nothing after it may be consumed. */
    while (len + 1 < sizeof line) {
        ssize_t r = read(0, line + len, 1);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0 || line[len] == '\n')
            break;
        len++;
    }
    line[len] = 0;
    char *args = strchr(line, ' ');
    if (args)
        *args++ = 0;
    int file = open(line, O_RDONLY);
    if (file < 0) {
        printf("shotrelay: cannot open %s: errno %d\n", line, errno);
        return 1;
    }
    int s = -1;
    for (int attempt = 0; attempt < 50; attempt++) { /* elfldr refuses loopback connections for a moment sometimes */
        s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons(9021);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (!connect(s, (struct sockaddr *)&a, sizeof a))
            break;
        close(s);
        s = -1;
        usleep(200000);
    }
    if (s < 0) {
        printf("shotrelay: cannot reach elfldr on 127.0.0.1:9021: errno %d\n", errno);
        close(file);
        return 1;
    }
    ssize_t r;
    size_t sent = 0;
    while ((r = read(file, buffer, sizeof buffer)) > 0) {
        if (writeAll(s, buffer, (size_t)r)) {
            printf("shotrelay: send failed after %zu bytes: errno %d\n", sent, errno);
            close(file);
            close(s);
            return 1;
        }
        sent += (size_t)r;
    }
    close(file);
    if (r < 0) {
        printf("shotrelay: read failed after %zu bytes: errno %d\n", sent, errno);
        close(s);
        return 1;
    }
    if (args) {
        writeAll(s, args, strlen(args));
        writeAll(s, "\n", 1);
    }
    printf("shotrelay: started %s (%zu bytes) from the local elfldr\n", line, sent);
    fflush(stdout);
    while ((r = read(s, buffer, sizeof buffer)) != 0) {
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (writeAll(1, buffer, (size_t)r))
            break; /* the Mac went away; the payload ends on its own timeout */
    }
    close(s);
    return 0;
}
