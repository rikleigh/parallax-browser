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

#ifndef WPEDisplayPS5_h
#define WPEDisplayPS5_h

#if !defined(__WPE_PS5_H_INSIDE__) && !defined(BUILDING_WEBKIT)
#error "Only <wpe/ps5/wpe-ps5.h> can be included directly."
#endif

#include <glib-object.h>
#include <wpe/wpe-platform.h>

G_BEGIN_DECLS

#define WPE_TYPE_DISPLAY_PS5 (wpe_display_ps5_get_type())
WPE_API G_DECLARE_FINAL_TYPE (WPEDisplayPS5, wpe_display_ps5, WPE, DISPLAY_PS5, WPEDisplay)

/**
 * WPEDisplayPS5TextCallback:
 * @text: (nullable): the text entered, or %NULL if the dialog was cancelled
 * @user_data: data passed to wpe_display_ps5_request_text()
 */
typedef void (* WPEDisplayPS5TextCallback) (const char *text, gpointer user_data);

WPE_API WPEDisplay *wpe_display_ps5_new          (void);

/* Opens the system on-screen keyboard (libSceImeDialog). @url selects the URL
 * keyboard layout and the "Go" label. Returns FALSE if a dialog is already
 * open or the dialog could not be started. */
WPE_API gboolean    wpe_display_ps5_request_text (WPEDisplayPS5            *display,
                                                  const char               *title,
                                                  const char               *initial_text,
                                                  gboolean                  url,
                                                  WPEDisplayPS5TextCallback callback,
                                                  gpointer                  user_data);

/* Path of the child payload ELF (WebProcess/NetworkProcess) the process
 * manager sends to elfldr, and the Mac log sink handed to children. */
WPE_API void        wpe_display_ps5_set_child_elf (WPEDisplayPS5 *display,
                                                   const char    *path);
WPE_API void        wpe_display_ps5_set_log_sink  (WPEDisplayPS5 *display,
                                                   const char    *host,
                                                   int            port);

/* Debug: the next frame on the TV is read back, halved (960x540 for
 * 1080p), and written to @path as a binary PPM; logs "[wpe-ps5] SHOT ...". */
WPE_API void        wpe_display_ps5_capture_screen (WPEDisplayPS5 *display,
                                                    const char    *path);

G_END_DECLS

#endif /* WPEDisplayPS5_h */
