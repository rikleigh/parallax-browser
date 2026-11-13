/* glp.h - shared by glprobe.c (contexts, caps, everything that needs no GPU)
 * and glprobe_gpu.c (everything that draws or dispatches). */
#pragma once

#define EGL_NO_PLATFORM_SPECIFIC_TYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "probe.h"

/* Every render target in the probe. */
#define W 32
#define H 32

/* RESULT lines. */
void R(const char *test, int pass, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void RSKIP(const char *test, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* A detail string built in steps (one per thread of execution: the probe is
 * single-threaded). */
void dreset(void);
void dcat(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *dstr(void);

int has_ext(const char *name);
void *gp(const char *name); /* eglGetProcAddress */

/* The first GL error since the last call (all errors are drained). */
GLenum glerr(void);
const char *errname(GLenum e);

GLuint mk_shader(GLenum type, const char *src, char *log, size_t n);
GLuint mk_program(const char *vs, const char *fs, char *log, size_t n);
GLuint mk_compute(const char *cs, char *log, size_t n);

/* Back to default GL state between groups. */
void reset_state(void);

/* Runs one group (probe_run_group) unless the launch argument left it out,
 * and reports as FAIL whatever it promised in results and did not report. */
void glp_group(const char *group, int needs_gpu, const char *results, void (*fn)(void));

void gpu_groups_run(void);
