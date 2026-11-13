/* probe.h - what glprobe.c needs from its two hosts (main_title.c on the
 * console, main_host.c in the Mac harness). */
#pragma once

/* One line to the log. */
void probe_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* One "RESULT <suite> <test> <status> <detail>" line; status is PASS, FAIL or
 * SKIP. Called through glprobe.c's R()/result helpers only. */
void probe_emit(const char *test, const char *status, const char *detail);

double probe_now_ms(void);

/* 0: the launch argument left this group out (skip=a,b or only=a,b). */
int probe_selected(const char *group);

/* Runs one group of tests. results: the space-separated RESULT names the
 * group must report. needs_gpu: the group reaches shader codegen or the GPU.
 * Console: calls fn. Host: forks, so an aborting platform stub ends only this
 * group; a needs_gpu group's results all become SKIP host. */
void probe_run_group(const char *group, int needs_gpu, const char *results, void (*fn)(void));

/* The whole probe: contexts, then every group. */
void glprobe_run(void);
