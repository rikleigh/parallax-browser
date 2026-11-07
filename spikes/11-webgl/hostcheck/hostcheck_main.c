/* hostcheck_main.c - runs angle_tests.cpp (HOST_CHECK) on the Mac under
 * Rosetta, over Spike 2's host build of ps5-opengl's EGL facade, the real
 * ps5 Gallium screen and Mesa 26.2. No GPU: display, configs, ES contexts,
 * ANGLE's caps/feature detection and shader translation + compilation only. */
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

void angle_tests_run(void);
void share_probe_run(void);

/* psbc (the PS5 shader compiler) is not built for the host; Mesa only stores
 * the options pointer at screen creation (as in spike 2's hostcheck.c). */
static _Alignas(64) unsigned char nir_options[8][8192];
const void *psbc_get_nir_options(int stage) { return nir_options[stage & 7]; }
void psbc_free_output(void *output) { (void)output; }
void psbc_free_tessellation_output(void *output) { (void)output; }

static int failures;

void spike11_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

void spike11_result(const char *test, int pass, const char *fmt, ...)
{
    va_list ap;
    printf("RESULT 11-webgl-host %s %s ", test, pass ? "PASS" : "FAIL");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    if (!pass)
        failures++;
}

double spike11_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

double spike11_cpu_ms(void) { return spike11_now_ms(); }

int main(void)
{
    angle_tests_run();
    share_probe_run();
    printf("%d failure(s)\n", failures);
    return failures != 0;
}
