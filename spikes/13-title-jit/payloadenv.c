/* payloadenv.c - the payload jsc's main, wrapped (build-jsc-title.sh links
 * the WebKit build's jsc objects again with --wrap=main and
 * --wrap=_Z12jscShellMainiPPc):
 * - main: jscenv.h's settings, the title's, then PS5Main.cpp's main;
 * - side threads from /data/jsc/payload-side.txt (jscside.h), reported at exit;
 * - jscShellMain (on PS5Main's 16 MB shell thread): pinned to the CPU in
 *   /data/jsc/payload-cpu.txt, if any, the way jsctitle.c pins the title's. */
#include "jscenv.h"
#include "jscside.h"

#include <unistd.h>

int __real_main(int argc, char** argv, char** envp);
int __real__Z12jscShellMainiPPc(int argc, char** argv);

static void report(const char* name, const char* value)
{
    fprintf(stderr, "[jsc-payload] env %s=%s\n", name, value);
}

static void reportLine(const char* line)
{
    fprintf(stderr, "[jsc-payload] %s\n", line);
}

static void stopSide(void)
{
    jscSideStop(reportLine);
}

int __wrap__Z12jscShellMainiPPc(int argc, char** argv)
{
    FILE* f = fopen("/data/jsc/payload-cpu.txt", "r");
    int cpu = -1;
    if (f) {
        if (fscanf(f, "%d", &cpu) != 1)
            cpu = -1;
        fclose(f);
    }
    if (cpu >= 0) {
        int rc = jscPinSelf(cpu);
        fprintf(stderr, "[jsc-payload] shell thread pinned to CPU %d (errno %d)\n", cpu, rc);
    }
    fprintf(stderr, "[jsc-payload] shell thread mask %#llx\n", (unsigned long long)jscSelfMask());
    return __real__Z12jscShellMainiPPc(argc, argv);
}

int __wrap_main(int argc, char** argv, char** envp)
{
    int n = jscLoadEnv(report);
    fprintf(stderr, "[jsc-payload] WebProcess JSC settings, %d lines from %s; useWasm=%s\n", n, JSC_ENV_FILE,
        getenv("JSC_useWasm"));
    char describe[256];
    if (jscSideStart("/data/jsc/payload-side.txt", describe, sizeof describe)) {
        fprintf(stderr, "[jsc-payload] side threads: %s\n", describe);
        atexit(stopSide);
    }
    int r = __real_main(argc, argv, envp);
    stopSide();
    return r;
}
