/* title_exit.c - how every spike title ends. Link it into each title.
 *
 * The title CRT (PS5VR's app_crt.cpp) calls catchReturnFromMain() once main
 * returns. Its default does nothing, and the system then kills the title
 * (0xa0d0c00f, a "debug error" when it closes). Asking ShellUI to close the
 * app ends it cleanly instead (Spike 8 on 13.60: no error). This strong
 * definition overrides the CRT's weak one.
 *
 * Titles that used the GPU are not closed by ShellUI after the request (all
 * GL spikes, 13.60), but leaving through exit() then is clean too (Spike 2:
 * no error); only the endless sleep was the problem. So wait briefly.
 */
#include "title.h"
#include "psvr2/psvr2.h"

void catchReturnFromMain(int status);
void catchReturnFromMain(int status)
{
    spike_log("main returned %d; closing the title through sceShellCoreUtilExitApp", status);
    if (psvr2_exit_app() == 0)
        for (int i = 0; i < 20; i++)
            usleep(100 * 1000); /* ShellUI ends a GPU-free title in this time */
    spike_log("ShellUI did not close the title; leaving through exit()");
}
