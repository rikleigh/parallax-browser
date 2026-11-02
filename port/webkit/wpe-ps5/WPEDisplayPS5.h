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

/* ---- The pad, for the application ---- */

/* DualSense buttons (bits of the pressed mask below). */
typedef enum {
    WPE_DISPLAY_PS5_BUTTON_L3 = 0x00000002,
    WPE_DISPLAY_PS5_BUTTON_R3 = 0x00000004,
    WPE_DISPLAY_PS5_BUTTON_OPTIONS = 0x00000008,
    WPE_DISPLAY_PS5_BUTTON_UP = 0x00000010,
    WPE_DISPLAY_PS5_BUTTON_RIGHT = 0x00000020,
    WPE_DISPLAY_PS5_BUTTON_DOWN = 0x00000040,
    WPE_DISPLAY_PS5_BUTTON_LEFT = 0x00000080,
    WPE_DISPLAY_PS5_BUTTON_L2 = 0x00000100,
    WPE_DISPLAY_PS5_BUTTON_R2 = 0x00000200,
    WPE_DISPLAY_PS5_BUTTON_L1 = 0x00000400,
    WPE_DISPLAY_PS5_BUTTON_R1 = 0x00000800,
    WPE_DISPLAY_PS5_BUTTON_TRIANGLE = 0x00001000,
    WPE_DISPLAY_PS5_BUTTON_CIRCLE = 0x00002000,
    WPE_DISPLAY_PS5_BUTTON_CROSS = 0x00004000,
    WPE_DISPLAY_PS5_BUTTON_SQUARE = 0x00008000,
    WPE_DISPLAY_PS5_BUTTON_TOUCHPAD = 0x00100000,
} WPEDisplayPS5Button;

/* Called on the main thread with the buttons that went down, before the
 * page gets them. Return TRUE to take them: the page then sees neither the
 * press nor the release. */
typedef gboolean (* WPEDisplayPS5ButtonHandler) (WPEDisplayPS5 *display,
                                                 guint32        pressed,
                                                 gpointer       user_data);
WPE_API void        wpe_display_ps5_set_button_handler (WPEDisplayPS5             *display,
                                                        WPEDisplayPS5ButtonHandler handler,
                                                        gpointer                   user_data);

/* A short message over the page on the TV (bottom centre), for @seconds. */
WPE_API void        wpe_display_ps5_show_message (WPEDisplayPS5 *display,
                                                  const char    *text,
                                                  double         seconds);

/* What is under the pointer, from the application's hit tests (e.g.
 * WebKitWebView::mouse-target-changed and
 * webkit_hit_test_result_context_is_editable()). A field that gets focus
 * right after a click opens the system keyboard only if the click was over
 * an editable node, so a button whose click handler focuses a field does not
 * reopen it. Until this is called, any click counts. */
WPE_API void        wpe_display_ps5_set_pointer_over_editable (WPEDisplayPS5 *display,
                                                               gboolean       editable);

/* ---- Performance overlay (port/webkit/OVERLAY.md) ---- */

/* Live numbers in a corner of the TV (frames, CPU, memory, compositing,
 * scrolling, video, network). Takes effect at once. */
WPE_API void        wpe_display_ps5_set_perf_overlay (WPEDisplayPS5 *display,
                                                      gboolean       enabled);
WPE_API gboolean    wpe_display_ps5_get_perf_overlay (WPEDisplayPS5 *display);
/* Called on the main thread when the pad shortcut (L3 + R3) turned the
 * overlay on or off (e.g. to remember it). */
typedef void (* WPEDisplayPS5PerfOverlayCallback) (WPEDisplayPS5 *display,
                                                   gboolean       enabled,
                                                   gpointer       user_data);
WPE_API void        wpe_display_ps5_set_perf_overlay_callback (WPEDisplayPS5                   *display,
                                                               WPEDisplayPS5PerfOverlayCallback callback,
                                                               gpointer                         user_data);
/* A named counter (a total since start, e.g. "blocked" for the ad blocker)
 * shown on the overlay with its rate; up to 6 names. Any thread; @display
 * may be NULL. */
WPE_API void        wpe_display_ps5_set_perf_counter (WPEDisplayPS5 *display,
                                                      const char    *name,
                                                      guint64        value);

/* ---- View in VR: the playing video in the PS VR2 headset ---- */

typedef enum {
    WPE_DISPLAY_PS5_VR_FLAT = 0,     /* a big virtual screen */
    WPE_DISPLAY_PS5_VR_360,          /* equirectangular 360, mono */
    WPE_DISPLAY_PS5_VR_360_SBS,      /* 360, left eye left half */
    WPE_DISPLAY_PS5_VR_360_TB,       /* 360, left eye top half */
    WPE_DISPLAY_PS5_VR_180,          /* equirectangular 180, mono */
    WPE_DISPLAY_PS5_VR_180_SBS,
    WPE_DISPLAY_PS5_VR_180_TB,
    WPE_DISPLAY_PS5_VR_FISHEYE_SBS,  /* equidistant fisheye per eye (MKX200 etc.) */
    WPE_DISPLAY_PS5_VR_N_PROJECTIONS
} WPEDisplayPS5VRProjection;

/* active TRUE: the headset shows the video. FALSE: it could not start, or
 * VR ended by itself (headset lost); @message says why. Not called after
 * wpe_display_ps5_vr_stop(). */
typedef void (* WPEDisplayPS5VRCallback) (WPEDisplayPS5 *display,
                                          gboolean       active,
                                          const char    *message,
                                          gpointer       user_data);

/* Takes the playing video into the headset (asynchronous: @callback says
 * how it went). While VR is active the pad goes only to the button handler
 * (no pointer, scrolling or keys for the page). /data/ps5webkit/vr-sim:
 * simulated, no headset is touched (log-only tests). */
WPE_API void        wpe_display_ps5_vr_start (WPEDisplayPS5            *display,
                                              WPEDisplayPS5VRProjection projection,
                                              double                    fisheye_degrees,
                                              WPEDisplayPS5VRCallback   callback,
                                              gpointer                  user_data);
/* Back to the TV. @wait: block until the headset is released (wind-down). */
WPE_API void        wpe_display_ps5_vr_stop  (WPEDisplayPS5 *display,
                                              gboolean       wait);
WPE_API gboolean    wpe_display_ps5_vr_is_active (WPEDisplayPS5 *display);
WPE_API void        wpe_display_ps5_vr_set_projection (WPEDisplayPS5            *display,
                                                       WPEDisplayPS5VRProjection projection,
                                                       double                    fisheye_degrees);
/* The video's front goes where the head looks now. */
WPE_API void        wpe_display_ps5_vr_recenter (WPEDisplayPS5 *display);
/* @hint: shown in the headset for @seconds ("" or 0 s: none); @tv_text: the
 * notice on the TV while in VR (NULL: unchanged). '\n' separates lines. */
WPE_API void        wpe_display_ps5_vr_set_text (WPEDisplayPS5 *display,
                                                 const char    *hint,
                                                 double         seconds,
                                                 const char    *tv_text);
/* Small luma pictures of the playing video, for guessing its layout: up
 * to @frames decoded frames about 200 ms apart (fewer when it is paused),
 * each @size x @size bytes (128), every byte the mean luma of its part of
 * the picture, row 0 = top; @luma is NULL and @frames 0 without a video.
 * The callback runs on the main thread; a new request answers a pending
 * one with no frames. */
typedef void (* WPEDisplayPS5ThumbnailsCallback) (WPEDisplayPS5 *display,
                                                  const guint8  *luma,
                                                  guint          frames,
                                                  guint          size,
                                                  int            video_width,
                                                  int            video_height,
                                                  gpointer       user_data);
WPE_API void        wpe_display_ps5_video_thumbnails (WPEDisplayPS5                  *display,
                                                      guint                           frames,
                                                      WPEDisplayPS5ThumbnailsCallback callback,
                                                      gpointer                        user_data);
/* Simulation only: the head direction (degrees, right and up positive). */
WPE_API void        wpe_display_ps5_vr_sim_look (WPEDisplayPS5 *display,
                                                 double         yaw,
                                                 double         pitch);

G_END_DECLS

#endif /* WPEDisplayPS5_h */
