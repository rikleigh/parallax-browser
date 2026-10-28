/*
 * View in VR (ps5browser): the <video> playing in the page, in the PS VR2
 * headset. See viewinvr.c.
 */
#pragma once

#include <wpe/ps5/wpe-ps5.h>
#include <wpe/webkit.h>

/* Installs the pad shortcut (touchpad button) on the display. */
void ps5wk_vr_init(WPEDisplay* display, WebKitWebView* webView);
/* Enter or leave VR, as the touchpad does. */
void ps5wk_vr_toggle(void);
/* Leave VR; wait: until the headset is released (wind-down). */
void ps5wk_vr_stop(gboolean wait, const char* why);
gboolean ps5wk_vr_active(void);
/* Log-only test sequence (/data/ps5webkit/vr-selftest with vr-sim): started
 * after a test page loaded. */
void ps5wk_vr_selftest_schedule(unsigned delaySeconds);
