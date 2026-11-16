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
/* While blocked (private browsing), every capture above is refused, and one
 * already asked for is dropped. */
WPE_API void        wpe_display_ps5_set_captures_blocked (WPEDisplayPS5 *display,
                                                          gboolean       blocked);

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
    /* Not a button: Options held for 1 s in gamepad mode (the press itself
     * went to the page). Only the button handler sees it. */
    WPE_DISPLAY_PS5_BUTTON_OPTIONS_HOLD = 0x01000000,
    /* Not a button: Triangle held for 1 s on a page outside gamepad mode
     * (the press itself went to the page). Only the button handler sees it. */
    WPE_DISPLAY_PS5_BUTTON_TRIANGLE_HOLD = 0x02000000,
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
/* While View in VR has the pad: called at every pad read (about 120 a
 * second) with the buttons held and the right stick (-1..1, right and down
 * positive, 0 in the dead zone), for held buttons and scrubbing. Presses
 * still go to the button handler first. Main thread. */
typedef void (* WPEDisplayPS5CapturedPadCallback) (WPEDisplayPS5 *display,
                                                   guint32        held,
                                                   double         right_x,
                                                   double         right_y,
                                                   gpointer       user_data);
WPE_API void        wpe_display_ps5_set_captured_pad_callback (WPEDisplayPS5                   *display,
                                                               WPEDisplayPS5CapturedPadCallback callback,
                                                               gpointer                         user_data);

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

/* ---- Gamepad mode and the Gamepad API (README: "Gamepad mode") ---- */

/* The Gamepad API is always there: pages that ask see the DualSense as a
 * standard gamepad while the page has the pad (not while the application's
 * UI, the keyboard or View in VR has it). Gamepad mode also stops the pad
 * meaning anything to the browser: no pointer, scrolling, Back, zoom or
 * focus moves; each button sends only its key below (if any) and the
 * sticks do what they are set to. The button handler sees nothing but
 * WPE_DISPLAY_PS5_BUTTON_OPTIONS_HOLD. Outside it, Triangle held on the
 * page is WPE_DISPLAY_PS5_BUTTON_TRIANGLE_HOLD (the application turns it
 * on with that). */
typedef enum {
    WPE_DISPLAY_PS5_STICK_GAMEPAD = 0, /* the Gamepad API only */
    WPE_DISPLAY_PS5_STICK_ARROWS,      /* the arrow keys (pushed past half way) */
    WPE_DISPLAY_PS5_STICK_WASD,        /* W A S D */
    WPE_DISPLAY_PS5_STICK_POINTER,     /* the pointer, as outside gamepad mode */
    WPE_DISPLAY_PS5_STICK_SCROLL,      /* scrolls, as the right stick outside gamepad mode */
} WPEDisplayPS5StickAction;

typedef struct {
    guint32 button;  /* one WPEDisplayPS5Button bit */
    guint   keyval;  /* WPE_KEY_*; WPE_KEY_Pointer_Button1: a click at the pointer */
    guint   keycode; /* the hardware key code (evdev + 8), for KeyboardEvent.code */
} WPEDisplayPS5ButtonKey;

/* @keys (copied; several may name one button) and the sticks apply until
 * the next call. Keys held when it changes (or when the pad goes to the
 * application's UI, the keyboard or View in VR) are released first. */
WPE_API void        wpe_display_ps5_set_gamepad_mode (WPEDisplayPS5                *display,
                                                      gboolean                      enabled,
                                                      const WPEDisplayPS5ButtonKey *keys,
                                                      guint                         n_keys,
                                                      WPEDisplayPS5StickAction      left_stick,
                                                      WPEDisplayPS5StickAction      right_stick);
WPE_API gboolean    wpe_display_ps5_get_gamepad_mode (WPEDisplayPS5 *display);

/* WebKit started (@in_use TRUE) or stopped reading the gamepad for the page
 * shown: a page asked for gamepads (navigator.getGamepads(), a
 * gamepadconnected listener). Main thread. */
typedef void (* WPEDisplayPS5GamepadCallback) (WPEDisplayPS5 *display,
                                               gboolean       in_use,
                                               gpointer       user_data);
WPE_API void        wpe_display_ps5_set_gamepad_callback (WPEDisplayPS5               *display,
                                                          WPEDisplayPS5GamepadCallback callback,
                                                          gpointer                     user_data);
WPE_API gboolean    wpe_display_ps5_gamepad_in_use (WPEDisplayPS5 *display);

/* ---- The application's own UI (the browser's address bar and menu) ---- */

/* A CPU image (premultiplied BGRA) the application draws its UI into:
 * rectangles and UTF-8 text with the Noto fonts in /data/ps5webkit/fonts
 * (symbol, CJK and script fallbacks; no shaping). Main thread. Colours are
 * 0xAARRGGBB, not premultiplied, blended over what is there. */
typedef struct _WPEDisplayPS5Canvas WPEDisplayPS5Canvas;
WPE_API WPEDisplayPS5Canvas *wpe_display_ps5_canvas_new        (int                  width,
                                                                int                  height);
WPE_API void                 wpe_display_ps5_canvas_free       (WPEDisplayPS5Canvas *canvas);
WPE_API int                  wpe_display_ps5_canvas_get_width  (WPEDisplayPS5Canvas *canvas);
WPE_API int                  wpe_display_ps5_canvas_get_height (WPEDisplayPS5Canvas *canvas);
/* Every pixel set to @argb (0 = transparent). */
WPE_API void                 wpe_display_ps5_canvas_clear      (WPEDisplayPS5Canvas *canvas,
                                                                guint32              argb);
/* A rectangle with corners rounded by @radius px. */
WPE_API void                 wpe_display_ps5_canvas_fill       (WPEDisplayPS5Canvas *canvas,
                                                                int                  x,
                                                                int                  y,
                                                                int                  width,
                                                                int                  height,
                                                                int                  radius,
                                                                guint32              argb);
/* The same with a gradient from @from to @to, left to right (or top to
 * bottom when @vertical). */
WPE_API void                 wpe_display_ps5_canvas_fill_gradient (WPEDisplayPS5Canvas *canvas,
                                                                int                  x,
                                                                int                  y,
                                                                int                  width,
                                                                int                  height,
                                                                int                  radius,
                                                                guint32              from,
                                                                guint32              to,
                                                                gboolean             vertical);
/* The rounded rectangle's outline, @line_width px inside its edge, with a
 * gradient as above (@from == @to: one colour). */
WPE_API void                 wpe_display_ps5_canvas_stroke     (WPEDisplayPS5Canvas *canvas,
                                                                int                  x,
                                                                int                  y,
                                                                int                  width,
                                                                int                  height,
                                                                int                  radius,
                                                                int                  line_width,
                                                                guint32              from,
                                                                guint32              to,
                                                                gboolean             vertical);
/* A soft glow around the rounded rectangle (not inside it), fading out
 * over @spread px; colours left to right as above. */
WPE_API void                 wpe_display_ps5_canvas_glow       (WPEDisplayPS5Canvas *canvas,
                                                                int                  x,
                                                                int                  y,
                                                                int                  width,
                                                                int                  height,
                                                                int                  radius,
                                                                int                  spread,
                                                                guint32              from,
                                                                guint32              to);
/* A premultiplied image (0xAARRGGBB words, @width x @height, rows top
 * down) blended at (@x, @y) with @alpha (0-255). */
WPE_API void                 wpe_display_ps5_canvas_image      (WPEDisplayPS5Canvas *canvas,
                                                                int                  x,
                                                                int                  y,
                                                                int                  width,
                                                                int                  height,
                                                                const guint32       *pixels,
                                                                guint                alpha);
/* One line of text, its baseline at @baseline, @size px; cut with an
 * ellipsis to @max_width when that is > 0. Returns the width drawn. */
WPE_API int                  wpe_display_ps5_canvas_text       (WPEDisplayPS5Canvas *canvas,
                                                                int                  x,
                                                                int                  baseline,
                                                                int                  max_width,
                                                                int                  size,
                                                                gboolean             bold,
                                                                guint32              argb,
                                                                const char          *utf8);
WPE_API int                  wpe_display_ps5_canvas_text_width (WPEDisplayPS5Canvas *canvas,
                                                                int                  size,
                                                                gboolean             bold,
                                                                const char          *utf8);
/* Shows a copy of @canvas over the page at (@x, @y) in TV pixels, under the
 * pointer, messages and the performance overlay, with GPU compositing on or
 * off; NULL removes it. Not shown while the video plays in the headset. */
WPE_API void                 wpe_display_ps5_set_ui_layer      (WPEDisplayPS5       *display,
                                                                WPEDisplayPS5Canvas *canvas,
                                                                int                  x,
                                                                int                  y);

/* UI mode: the pad belongs to the application's own UI. Every button goes
 * to the button handler (D-pad, L1/R1, L2/R2 also as auto-repeated presses
 * while held), the left stick moves the pointer without page events, and
 * pointer moves and right-stick scrolling go to the UI pointer callback.
 * The page sees nothing; buttons held across the switch are ignored until
 * released. View in VR's capture takes precedence. */
WPE_API void                 wpe_display_ps5_set_ui_mode       (WPEDisplayPS5       *display,
                                                                gboolean             enabled);
WPE_API gboolean             wpe_display_ps5_get_ui_mode       (WPEDisplayPS5       *display);
/* Several views (the browser's live tabs): only the active view is mapped
 * (page visible: WebKit paints it, runs its timers and rAF at full rate),
 * has the keyboard focus, is shown and gets the pad; the others are
 * unmapped (page hidden). Once this has been called, a new view stays
 * unmapped until it is made active. NULL is not allowed. */
WPE_API void                 wpe_display_ps5_set_active_view   (WPEDisplayPS5       *display,
                                                                WPEView             *view);

/* The pointer position in TV pixels; returns whether it is shown (it shows
 * for a few seconds after the left stick moves). */
WPE_API gboolean             wpe_display_ps5_get_pointer       (WPEDisplayPS5       *display,
                                                                double              *x,
                                                                double              *y);
/* In UI mode: the pointer moved (@x, @y), or the right stick scrolled
 * (@scroll_y px, down positive; 0 for a move). Main thread. */
typedef void (* WPEDisplayPS5UIPointerCallback) (WPEDisplayPS5 *display,
                                                 double         x,
                                                 double         y,
                                                 double         scroll_y,
                                                 gpointer       user_data);
WPE_API void                 wpe_display_ps5_set_ui_pointer_callback (WPEDisplayPS5                 *display,
                                                                      WPEDisplayPS5UIPointerCallback callback,
                                                                      gpointer                       user_data);

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
/* The same with the application's own drawing (copied): @panel in the
 * headset for @seconds (NULL or 0 s: none), head-locked below the view's
 * centre, 1240 px about 50 degrees wide; @tv: the TV's card while in VR,
 * centred on black (NULL: unchanged). */
WPE_API void        wpe_display_ps5_vr_set_panel (WPEDisplayPS5       *display,
                                                  WPEDisplayPS5Canvas *panel,
                                                  double               seconds,
                                                  WPEDisplayPS5Canvas *tv);
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
