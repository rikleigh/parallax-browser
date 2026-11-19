/*
 * The runtime files, from the title folder into /data/ps5webkit (runtime.c).
 */
#pragma once

#include <glib.h>
#include <stddef.h>

/* Copies /app0/runtime into /data/ps5webkit when its stamp differs from the
 * installed one. Returns the number of files copied (0: nothing to do, or no
 * runtime in the title), or -1 with a short reason in error. dataRefused:
 * TRUE when /data/ps5webkit refused the title (EACCES/EPERM there, not in
 * /app0), which ps5wk_data_repair can fix. */
int ps5wk_runtime_install(char* error, size_t errorSize, gboolean* dataRefused);

/* TRUE when a folder in /data/ps5webkit that the title writes in refuses
 * it (EACCES/EPERM; GitHub issue 3), with the folder in which. */
gboolean ps5wk_data_locked(char* which, size_t whichSize);

/* Opens /data/ps5webkit to all through the restart helper (a payload: root),
 * waiting for it (30 s at most). Returns the number of entries changed, or
 * -1 with a short reason in error. Main thread, before the display. */
int ps5wk_data_repair(char* error, size_t errorSize);
