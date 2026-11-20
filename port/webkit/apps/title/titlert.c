/*
 * titlert.c - stdout and stderr into a file, in GPU=radv builds.
 *
 * GPU=gl builds get this from ps5-opengl's native-app/runtime_shims.c
 * (/download0/ps5-opengl.log), with __assert, openlog, popen and pclose,
 * which libc.a has as well. GPU=radv builds don't link ps5-opengl
 * (HANDOVER-radv.md Phase 5), so the title keeps only the log: what is
 * printed before titlelog.c takes stderr over, and stdout after.
 */
#include <stdio.h>

__attribute__((constructor)) static void titleOpenLog(void)
{
    static const char path[] = "/download0/ps5browser.log";
    /* Emptied, then both streams append, so neither writes over the other. */
    FILE* stream = freopen(path, "w", stdout);
    if (stream)
        stream = freopen(path, "a", stdout);
    if (stream)
        setvbuf(stream, NULL, _IONBF, 0);
    stream = freopen(path, "a", stderr);
    if (stream)
        setvbuf(stream, NULL, _IONBF, 0);
}
