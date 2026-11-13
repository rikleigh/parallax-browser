/* main_host.c - runs the probe on the Mac under Rosetta, over Spike 2's host
 * build of ps5-opengl's EGL facade, the real ps5 Gallium screen and Mesa
 * 26.2 (see spikes/02-gl-egl/ps5-opengl/hostcheck). No GPU and no PS5 shader
 * compiler there: AGC and PSBC entry points are stubs that abort.
 *
 * Every group runs in a forked child, so a stub that aborts ends that group
 * only. A group that needs the GPU is still tried (the log shows how far it
 * got), but all its results are reported as SKIP host: whatever it printed
 * says nothing about the console.
 */
#include "../src/probe.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PROBE_SUITE
#define PROBE_SUITE "12-p0d-glprobe-host"
#endif

/* refcheck.sh builds this file with PROBE_REF: the same probe over Mesa's
 * software rasteriser instead of the ps5 screen, to test the probe itself.
 * There every group really runs, so its results are reported as they are. */
#ifdef PROBE_REF
#define GPU_RESULTS_COUNT 1
#else
#define GPU_RESULTS_COUNT 0
#endif

/* psbc (the PS5 shader compiler) is not built for the host; Mesa only stores
 * the options pointer at screen creation (as in spike 2's hostcheck.c). */
static _Alignas(64) unsigned char nir_options[8][8192];
const void *psbc_get_nir_options(int stage) { return nir_options[stage & 7]; }
void psbc_free_output(void *output) { (void)output; }
void psbc_free_tessellation_output(void *output) { (void)output; }

static int g_pass, g_fail, g_skip;

void probe_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

void probe_emit(const char *test, const char *status, const char *detail)
{
    printf("RESULT %s %s %s %s\n", PROBE_SUITE, test, status, detail);
    fflush(stdout);
}

double probe_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

int probe_selected(const char *group)
{
    const char *only = getenv("PROBE_ONLY"), *skip = getenv("PROBE_SKIP");
    char list[512];
    if (only) {
        snprintf(list, sizeof list, ",%s,", only);
        char want[96];
        snprintf(want, sizeof want, ",%s,", group);
        return strstr(list, want) != NULL;
    }
    if (skip) {
        snprintf(list, sizeof list, ",%s,", skip);
        char want[96];
        snprintf(want, sizeof want, ",%s,", group);
        return strstr(list, want) == NULL;
    }
    return 1;
}

static void count(const char *status)
{
    if (!strcmp(status, "PASS"))
        g_pass++;
    else if (!strcmp(status, "FAIL"))
        g_fail++;
    else
        g_skip++;
}

void probe_run_group(const char *group, int needs_gpu, const char *results, void (*fn)(void))
{
    int fds[2];
    if (GPU_RESULTS_COUNT)
        needs_gpu = 0;
    printf("== %s%s\n", group, needs_gpu ? " (needs the GPU: tried, reported as SKIP host)" : "");
    fflush(stdout);
    if (pipe(fds) != 0) {
        perror("pipe");
        exit(2);
    }
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[1]);
        fn();
        fflush(stdout);
        _exit(0);
    }
    close(fds[1]);

    char seen[48][64], stub[96] = "", assertion[400] = "", line[2048];
    int nseen = 0;
    FILE *in = fdopen(fds[0], "r");
    while (fgets(line, sizeof line, in)) {
        char suite[64], test[64], status[16];
        int at = 0;
        line[strcspn(line, "\n")] = 0;
        if (sscanf(line, "RESULT %63s %63s %15s %n", suite, test, status, &at) == 3) {
            if (nseen < 48)
                snprintf(seen[nseen++], sizeof seen[0], "%s", test);
            if (needs_gpu) {
                count("SKIP");
                printf("RESULT %s %s SKIP host: no GPU or PS5 shader compiler here. The host run said %s: %s\n", suite, test, status, line + at);
            } else {
                count(status);
                printf("%s\n", line);
            }
        } else {
            sscanf(line, "hostcheck: platform stub %95s reached", stub);
            if (!strncmp(line, "Assertion failed", 16))
                snprintf(assertion, sizeof assertion, "%s", line);
            if (strncmp(line, "[ps5-", 5) != 0) /* the driver's own chatter */
                printf("%s\n", line);
        }
    }
    fclose(in);
    int status = 0;
    waitpid(pid, &status, 0);
    const int died = !WIFEXITED(status) || WEXITSTATUS(status) != 0;

    /* Whatever the group did not get to. */
    char names[512];
    snprintf(names, sizeof names, "%s", results);
    for (char *save = NULL, *t = strtok_r(names, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
        int found = 0;
        for (int i = 0; i < nseen; i++)
            found |= !strcmp(seen[i], t);
        if (found)
            continue;
        char why[480];
        if (assertion[0])
            snprintf(why, sizeof why, "the group died in Mesa's debug build: %s", assertion);
        else if (stub[0])
            snprintf(why, sizeof why, "the group stopped at the platform stub %s", stub);
        else if (WIFSIGNALED(status))
            snprintf(why, sizeof why, "the group died with signal %d", WTERMSIG(status));
        else
            snprintf(why, sizeof why, "the group ended without reporting it");
        if (needs_gpu) {
            count("SKIP");
            printf("RESULT %s %s SKIP host: no GPU or PS5 shader compiler here (%s)\n", PROBE_SUITE, t, why);
        } else {
            count("FAIL");
            printf("RESULT %s %s FAIL %s\n", PROBE_SUITE, t, why);
        }
    }
    if (died && !needs_gpu)
        printf("   (group %s: child %s %d%s%s)\n", group, WIFSIGNALED(status) ? "signal" : "exit",
               WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status), stub[0] ? ", stub " : "", stub);
    fflush(stdout);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* The two context results come straight from the parent. */
    printf("ps5webkit P0d GL probe, host harness (suite %s)\n", PROBE_SUITE);
    glprobe_run();
    printf("%d PASS, %d FAIL, %d SKIP in groups (plus the two ctx-* lines above)\n", g_pass, g_fail, g_skip);
    return 0;
}
