/* jscenv.h - JSC settings shared by the title (jsctitle.c) and the payload
 * jsc (payloadenv.c), so both runs start with the same options: the
 * WebProcess's settings (ps5wk-child.cpp configureJIT) as JSC_* variables,
 * then /data/jsc/env.txt "NAME=value" lines (# comments) over them.
 *
 * Environment, not command-line options: JSC applies the environment before
 * it resolves dependent options, the command line after. "--useWasm=1" on
 * the command line, with wasm off by default on PS5 (Options.cpp), turns
 * wasm on after IPInt, BBQ, the wasm compiler threads and the rest were
 * already switched off with it ("INCOHERENT OPTIONS"). */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSC_ENV_FILE "/data/jsc/env.txt"

/* Calls report(name, value) for each line of the file; returns how many. */
static inline int jscLoadEnv(void (*report)(const char* name, const char* value))
{
    setenv("JSC_useJIT", "true", 0);
    setenv("JSC_useWasm", "true", 0);
    setenv("JSC_numberOfBaselineCompilerThreads", "1", 0);
    setenv("JSC_numberOfDFGCompilerThreads", "1", 0);
    setenv("JSC_numberOfFTLCompilerThreads", "1", 0);
    setenv("JSC_jitMemoryReservationSize", "268435456", 0);
    setenv("ICU_DATA", "/data/ps5webkit/share/icu", 0);
    FILE* f = fopen(JSC_ENV_FILE, "r");
    if (!f)
        return 0;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char* eq = strchr(line, '=');
        if (!eq || line[0] == '#')
            continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
        if (report)
            report(line, eq + 1);
        n++;
    }
    fclose(f);
    return n;
}
