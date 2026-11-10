/*
 * The runtime files, from the title folder into /data/ps5webkit (runtime.c).
 */
#pragma once

#include <stddef.h>

/* Copies /app0/runtime into /data/ps5webkit when its stamp differs from the
 * installed one. Returns the number of files copied (0: nothing to do, or no
 * runtime in the title), or -1 with a short reason in error. */
int ps5wk_runtime_install(char* error, size_t errorSize);
