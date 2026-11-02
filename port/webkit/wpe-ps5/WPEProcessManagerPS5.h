/*
 * Copyright (C) 2026 ps5webkit contributors
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.
 */

#ifndef WPEProcessManagerPS5_h
#define WPEProcessManagerPS5_h

#include <glib-object.h>
#include <wpe/wpe-platform.h>

G_BEGIN_DECLS

/* Launches WebKit's WebProcess/NetworkProcess as elfldr payloads from
 * @child_elf (default /app0/ps5wk-child.elf). WPEDisplayPS5 installs one as
 * the default process manager when it connects; other UIProcesses (the
 * headless test payload) install one with wpe_process_manager_set_default(). */
WPE_API WPEProcessManager *wpe_process_manager_ps5_new           (const char        *child_elf);
WPE_API void               wpe_process_manager_ps5_set_child_elf (WPEProcessManager *manager,
                                                                  const char        *child_elf);

/* A task payload: @child_elf started in another mode (header type @task,
 * one @argument without spaces), with no WebKit process. @output gets each
 * line the payload prints, @ended once at the end (@launched FALSE: it never
 * ran), both on a thread of their own. Used by the title's ad blocker
 * (apps/title/adblock.c) to compile its list in a process whose memory goes
 * back when it exits. */
typedef void (*WPEProcessManagerPS5TaskOutput) (const char *line, gpointer user_data);
typedef void (*WPEProcessManagerPS5TaskEnded)  (gboolean launched, gpointer user_data);
WPE_API gboolean wpe_process_manager_ps5_run_task (WPEProcessManager             *manager,
                                                   const char                    *task,
                                                   const char                    *argument,
                                                   WPEProcessManagerPS5TaskOutput output,
                                                   WPEProcessManagerPS5TaskEnded  ended,
                                                   gpointer                       user_data);

G_END_DECLS

#endif /* WPEProcessManagerPS5_h */
