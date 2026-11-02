/* title_exit.c - how every spike title ends. Link it into each title.
 *
 * The title CRT (PS5VR's app_crt.cpp) calls catchReturnFromMain() once main
 * returns. Its default does nothing, and the system then kills the title
 * (0xa0d0c00f, a "debug error" when it closes). This strong definition
 * overrides the CRT's weak one.
 *
 * Watched on 13.60 (2026-10-27): a title that has used the GPU and ends
 * itself shows a debug error, whether it asks ShellUI to close it
 * (sceShellCoreUtilExitApp: ShellUI suspends it first, which crashes it,
 * reason 0xc), calls exit() or calls _exit() (also reason 0xc). Only a
 * SIGKILL from another process ends it with nothing on screen. So the title
 * asks the launcher payload (tools/launcher, TCP 9031) to SIGKILL it.
 *
 * Without the launcher: sceShellCoreUtilExitApp closes a GPU-free title
 * cleanly (Spike 8); GPU titles then leave through exit(), with the error.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "title.h"
#include "psvr2/psvr2.h"

#define LAUNCHER_PORT 9031

/* A title that wants to start again (ps5browser's "Restart browser") puts
 * its title ID here. The launcher then gets "launch <ID>" after the kill,
 * on the same connection: it reads that line once the kill is done (the
 * title is gone by then; the launcher's reply just goes nowhere). */
char spike_title_relaunch[16];

/* Returns only if the launcher couldn't be asked or didn't act in time. */
static void ask_launcher_to_kill_us(void)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(LAUNCHER_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
        char cmd[48] = "kill -9\n";
        if (spike_title_relaunch[0])
            snprintf(cmd, sizeof cmd, "kill -9\nlaunch %.15s\n", spike_title_relaunch);
        spike_log("closing the title through the launcher (SIGKILL)%s%s", spike_title_relaunch[0] ? ", then starting " : "",
                  spike_title_relaunch);
        /* A queued log (ps5browser's titlelog.c): out before the kill. */
        fflush(stderr);
        spike_log_flush(500);
        if (write(fd, cmd, strlen(cmd)) == (ssize_t)strlen(cmd))
            for (int i = 0; i < 30; i++)
                usleep(100 * 1000);
    }
    close(fd);
}

void catchReturnFromMain(int status);
void catchReturnFromMain(int status)
{
    spike_log("main returned %d", status);
    ask_launcher_to_kill_us();
    spike_log("no launcher; closing the title through sceShellCoreUtilExitApp");
    if (psvr2_exit_app() == 0)
        for (int i = 0; i < 20; i++)
            usleep(100 * 1000); /* ShellUI ends a GPU-free title in this time */
    spike_log("ShellUI did not close the title; leaving through exit()");
}
