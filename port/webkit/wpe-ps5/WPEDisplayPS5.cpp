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

#include "config.h"
#include "WPEDisplayPS5.h"

#include "GRefPtrWPE.h"

#include "PS5PerfOverlay.h"
#include "PS5Renderer.h"
#include <pthread.h>
#include <sys/resource.h>
#include <time.h>
#include "PS5Sce.h"
#include "WPEExtensions.h"
#include "WPEPS5Private.h"
#include <cmath>
#include <cstring>
#include <gio/gio.h>
#include <wtf/glib/GRefPtr.h>
#include <wtf/glib/RunLoopSourcePriority.h>
#include <wtf/glib/GUniquePtr.h>
#include <wtf/glib/WTFGType.h>
#include <string>

/**
 * WPEDisplayPS5:
 *
 * The PS5 [class@WPEPlatform.Display]: one fullscreen view on the TV through
 * ps5-opengl's EGL window surface, DualSense input mapped to keyboard and
 * pointer events, the system keyboard (libSceImeDialog) as input method, and
 * auxiliary processes launched as elfldr payloads (WPEProcessManagerPS5).
 *
 * Pad mapping:
 *   D-pad            arrow keys (auto-repeat)
 *   Cross (X)        click at the pointer when it is shown, else Return
 *   Circle (O)       Back (XF86Back; the browser goes back)
 *   Triangle         on-screen keyboard for the focused field
 *   Square           Reload (XF86Reload)
 *   L1 / R1          Shift+Tab / Tab (previous / next focusable element)
 *   L2 / R2          ZoomOut / ZoomIn (XF86ZoomOut/In)
 *   Options          Menu (the browser opens its URL prompt)
 *   R3               Tools (XF86Tools; the browser opens its Settings page)
 *   L3               HomePage (XF86HomePage; the browser loads its home page)
 *   left stick       moves the drawn pointer
 *   right stick      scrolls under the pointer
 *   (the application's button handler sees presses first; ps5browser takes
 *   the touchpad button for "View in VR", and the whole pad while in VR)
 */

static constexpr unsigned padIntervalMS = 8;
static constexpr double repeatDelay = 0.40;
static constexpr double repeatInterval = 0.06;
static constexpr double pointerShownFor = 8.0; // seconds after the last stick move
static constexpr int stickDeadZone = 14; // of 127

// WPEScreen is abstract; the TV is one fixed screen.
G_DECLARE_FINAL_TYPE(WPEScreenPS5, wpe_screen_ps5, WPE, SCREEN_PS5, WPEScreen)
#define WPE_TYPE_SCREEN_PS5 (wpe_screen_ps5_get_type())
struct _WPEScreenPS5Private { };
WEBKIT_DEFINE_FINAL_TYPE(WPEScreenPS5, wpe_screen_ps5, WPE_TYPE_SCREEN, WPEScreen)
static void wpe_screen_ps5_class_init(WPEScreenPS5Class*) { }

struct PendingText {
    WPEDisplayPS5TextCallback callback { nullptr };
    gpointer userData { nullptr };
    char16_t buffer[2049] { };
    char16_t title[128] { };
    char16_t placeholder[2] { };
};

struct _WPEDisplayPS5Private {
    GRefPtr<WPEScreen> screen;
    WPEView* activeView { nullptr };
    WPEInputMethodContext* focusedInputContext { nullptr };

    int32_t userID { -1 };
    int32_t pad { -1 };
    uint32_t buttons { 0 };
    uint32_t ignoredButtons { 0 };
    uint32_t repeatingKey { 0 };
    double repeatNext { 0 };
    double lastPress { -1000 };
    GRefPtr<GSource> padSource;

    double pointerX { 0 };
    double pointerY { 0 };
    double pointerMovedAt { -1000 };
    bool pointerDown { false };
    bool pointerWasVisible { false };

    GRefPtr<GSource> redrawSource;

    std::unique_ptr<PendingText> pendingText;
    PS5ImeFunctions ime { };

    // The application's button handler; the pad is captured (only the
    // handler gets it) while the video plays in the headset.
    WPEDisplayPS5ButtonHandler buttonHandler { nullptr };
    gpointer buttonHandlerData { nullptr };
    uint32_t swallowedButtons { 0 };
    bool padCaptured { false };
    bool stickChord { false }; // L3 + R3 went down together: their own keys are not sent
    WPEDisplayPS5PerfOverlayCallback perfOverlayCallback { nullptr };
    gpointer perfOverlayCallbackData { nullptr };
    WPEDisplayPS5VRCallback vrCallback { nullptr };
    gpointer vrCallbackData { nullptr };
    bool vrStarting { false };

    GRefPtr<WPEProcessManager> processManager;
    std::string childELF;
    std::string logHost;
    int logPort { 0 };
};
WEBKIT_DEFINE_FINAL_TYPE_WITH_CODE(WPEDisplayPS5, wpe_display_ps5, WPE_TYPE_DISPLAY, WPEDisplay,
    wpeEnsureExtensionPointsRegistered();
    g_io_extension_point_implement(WPE_DISPLAY_EXTENSION_POINT_NAME, g_define_type_id, "ps5", 100))

static gint64 s_mainPollUS; // time the main loop spent waiting in poll (main thread)

int64_t ps5wkMainLoopPollUS()
{
    return s_mainPollUS;
}

static double nowSeconds()
{
    return g_get_monotonic_time() / 1e6;
}

static guint32 eventTime()
{
    return static_cast<guint32>(g_get_monotonic_time() / 1000);
}

// ---------------------------------------------------------------- redraw

static void redraw(WPEDisplayPS5* display)
{
    auto* priv = display->priv;
    bool visible = nowSeconds() - priv->pointerMovedAt < pointerShownFor;
    priv->pointerWasVisible = visible;
    PS5Renderer::singleton().present(visible, priv->pointerX, priv->pointerY);
}

void wpeDisplayPS5ScheduleRedraw(WPEDisplayPS5* display)
{
    auto* priv = display->priv;
    if (priv->redrawSource)
        return;
    priv->redrawSource = adoptGRef(g_idle_source_new());
    g_source_set_priority(priv->redrawSource.get(), RunLoopSourcePriority::RunLoopDispatcher);
    g_source_set_name(priv->redrawSource.get(), "WPE PS5 redraw");
    g_source_set_callback(priv->redrawSource.get(), [](gpointer userData) -> gboolean {
        auto* display = WPE_DISPLAY_PS5(userData);
        display->priv->redrawSource = nullptr;
        redraw(display);
        return G_SOURCE_REMOVE;
    }, display, nullptr);
    g_source_attach(priv->redrawSource.get(), g_main_context_get_thread_default());
}

void wpeDisplayPS5SetActiveView(WPEDisplayPS5* display, WPEView* view)
{
    display->priv->activeView = view;
}

WPEView* wpeDisplayPS5GetActiveView(WPEDisplayPS5* display)
{
    return display->priv->activeView;
}

void wpeDisplayPS5SetFocusedInputContext(WPEDisplayPS5* display, WPEInputMethodContext* context)
{
    display->priv->focusedInputContext = context;
}

void wpeDisplayPS5InputContextDestroyed(WPEDisplayPS5* display, WPEInputMethodContext* context)
{
    if (display->priv->focusedInputContext == context)
        display->priv->focusedInputContext = nullptr;
}

double wpeDisplayPS5SecondsSinceLastPress(WPEDisplayPS5* display)
{
    return nowSeconds() - display->priv->lastPress;
}

// ------------------------------------------------------------- IME dialog

static void utf8ToUTF16(const char* text, char16_t* out, size_t capacity)
{
    size_t n = 0;
    if (text) {
        glong written = 0;
        GUniquePtr<gunichar2> utf16(g_utf8_to_utf16(text, -1, nullptr, &written, nullptr));
        if (utf16) {
            for (; n < static_cast<size_t>(written) && n + 1 < capacity; n++)
                out[n] = static_cast<char16_t>(utf16.get()[n]);
        }
    }
    out[n] = 0;
}

static void finishText(WPEDisplayPS5* display, const char* text)
{
    auto pending = WTF::move(display->priv->pendingText);
    if (pending && pending->callback)
        pending->callback(text, pending->userData);
}

static void pollTextDialog(WPEDisplayPS5* display)
{
    auto* priv = display->priv;
    if (!priv->pendingText || !priv->ime.getStatus)
        return;
    int status = priv->ime.getStatus();
    if (status == PS5_IME_DIALOG_STATUS_RUNNING)
        return;

    PS5ImeDialogResult result { };
    int rc = status == PS5_IME_DIALOG_STATUS_FINISHED ? priv->ime.getResult(&result) : -1;
    priv->ime.term();
    if (rc || result.endStatus) {
        PS5_LOG("IME dialog closed: status %d rc 0x%x end %d", status, rc, result.endStatus);
        finishText(display, nullptr);
        return;
    }
    size_t length = 0;
    while (length < G_N_ELEMENTS(priv->pendingText->buffer) && priv->pendingText->buffer[length])
        length++;
    GUniquePtr<char> utf8(g_utf16_to_utf8(reinterpret_cast<const gunichar2*>(priv->pendingText->buffer), length, nullptr, nullptr, nullptr));
    finishText(display, utf8 ? utf8.get() : "");
}

gboolean wpe_display_ps5_request_text(WPEDisplayPS5* display, const char* title, const char* initialText, gboolean url, WPEDisplayPS5TextCallback callback, gpointer userData)
{
    g_return_val_if_fail(WPE_IS_DISPLAY_PS5(display), FALSE);
    auto* priv = display->priv;
    if (priv->pendingText || !priv->ime.init) {
        PS5_LOG("request_text: %s", priv->pendingText ? "a dialog is already open" : "libSceImeDialog not loaded");
        return FALSE;
    }

    auto pending = std::make_unique<PendingText>();
    pending->callback = callback;
    pending->userData = userData;
    utf8ToUTF16(initialText, pending->buffer, G_N_ELEMENTS(pending->buffer));
    utf8ToUTF16(title ? title : "", pending->title, G_N_ELEMENTS(pending->title));

    PS5ImeDialogParam param { };
    param.userID = priv->userID;
    param.type = url ? PS5_IME_TYPE_URL : PS5_IME_TYPE_DEFAULT;
    param.enterLabel = url ? PS5_IME_ENTER_LABEL_GO : PS5_IME_ENTER_LABEL_DEFAULT;
    param.option = url ? PS5_IME_OPTION_NO_AUTO_CAPITALIZATION : 0;
    param.maxTextLength = G_N_ELEMENTS(pending->buffer) - 1;
    param.inputTextBuffer = pending->buffer;
    param.posX = PS5Renderer::singleton().width() / 2.0f;
    param.posY = PS5Renderer::singleton().height() / 2.0f;
    param.horizontalAlignment = 1; // centre
    param.verticalAlignment = 1;
    param.placeholder = pending->placeholder;
    param.title = pending->title;

    // The keyboard's own pointer follows the touchpad, and a touchpad that
    // reports a touch (a resting finger, or none at all) pins the selection
    // so the D-pad can't move it: keys are picked with the D-pad and X only.
    PS5ImeParamExtended extended { };
    extended.option = PS5_IME_EXT_OPTION_DISABLE_POINTER;
    int rc = priv->ime.init(&param, &extended);
    if (rc) {
        PS5_LOG("sceImeDialogInit with the pointer disabled failed (0x%08x); trying without", rc);
        rc = priv->ime.init(&param, nullptr);
    }
    if (rc) {
        PS5_LOG("sceImeDialogInit failed: 0x%08x", rc);
        return FALSE;
    }
    PS5_LOG("IME dialog open (%s, %zu chars)", url ? "URL" : "text", strlen(initialText ? initialText : ""));
    priv->pendingText = WTF::move(pending);
    return TRUE;
}

// ---------------------------------------------------------------- the pad

static void sendKey(WPEView* view, guint keyval, guint keycode, WPEModifiers modifiers, bool down)
{
    GRefPtr<WPEEvent> event = adoptGRef(wpe_event_keyboard_new(down ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP,
        view, WPE_INPUT_SOURCE_KEYBOARD, eventTime(), modifiers, keycode, keyval));
    wpe_view_event(view, event.get());
}

static void tapKey(WPEView* view, guint keyval, guint keycode, WPEModifiers modifiers = static_cast<WPEModifiers>(0))
{
    sendKey(view, keyval, keycode, modifiers, true);
    sendKey(view, keyval, keycode, modifiers, false);
}

struct KeyMapping {
    uint32_t button;
    guint keyval;
    guint keycode; // evdev + 8
    WPEModifiers modifiers;
    bool repeats;
};

static const KeyMapping keyMappings[] = {
    { PS5_PAD_UP, WPE_KEY_Up, 111, static_cast<WPEModifiers>(0), true },
    { PS5_PAD_DOWN, WPE_KEY_Down, 116, static_cast<WPEModifiers>(0), true },
    { PS5_PAD_LEFT, WPE_KEY_Left, 113, static_cast<WPEModifiers>(0), true },
    { PS5_PAD_RIGHT, WPE_KEY_Right, 114, static_cast<WPEModifiers>(0), true },
    { PS5_PAD_CIRCLE, WPE_KEY_Back, 166, static_cast<WPEModifiers>(0), false },
    { PS5_PAD_SQUARE, WPE_KEY_Reload, 189, static_cast<WPEModifiers>(0), false },
    { PS5_PAD_L1, WPE_KEY_ISO_Left_Tab, 23, WPE_MODIFIER_KEYBOARD_SHIFT, true },
    { PS5_PAD_R1, WPE_KEY_Tab, 23, static_cast<WPEModifiers>(0), true },
    { PS5_PAD_L2, WPE_KEY_ZoomOut, 0, static_cast<WPEModifiers>(0), false },
    { PS5_PAD_R2, WPE_KEY_ZoomIn, 0, static_cast<WPEModifiers>(0), false },
    { PS5_PAD_OPTIONS, WPE_KEY_Menu, 135, static_cast<WPEModifiers>(0), false },
    { PS5_PAD_R3, WPE_KEY_Tools, 0, static_cast<WPEModifiers>(0), false },
    { PS5_PAD_L3, WPE_KEY_HomePage, 180, static_cast<WPEModifiers>(0), false },
};

static double stickValue(uint8_t raw)
{
    int v = static_cast<int>(raw) - 128;
    if (std::abs(v) < stickDeadZone)
        return 0;
    double x = (std::abs(v) - stickDeadZone) / static_cast<double>(127 - stickDeadZone);
    x = std::fmin(1.0, x);
    return (v < 0 ? -1 : 1) * x * x; // quadratic: fine control near the centre
}

static void openPad(WPEDisplayPS5* display)
{
    auto* priv = display->priv;
    if (!scePadInit || !scePadOpen || !sceUserServiceInitialize) {
        PS5_LOG("pad: libScePad/libSceUserService not linked, no pad input");
        return;
    }
    sceUserServiceInitialize(nullptr);
    int rc = scePadInit();
    int32_t user = -1;
    if (sceUserServiceGetInitialUser)
        sceUserServiceGetInitialUser(&user);
    priv->userID = user;
    priv->pad = scePadOpen(user, 0, 0, nullptr);
    PS5_LOG("pad: init 0x%x, user %d, open -> %d", rc, user, priv->pad);
    PS5PadData data { };
    if (priv->pad >= 0 && scePadReadState(priv->pad, &data) == 0)
        priv->ignoredButtons = data.buttons; // held at start: ignore until released
}

// Test knob: /data/ps5webkit/pad-scroll-test makes the right stick scroll
// as the owner's pad does (one precise wheel event per pad tick), in cycles:
// `secs` s down at `ry` (the stick value after padTick's curve, 0-1),
// `secs` s up, `pause` s still, from `delay` s after the first pad tick with
// a view; `shots` captures per direction
// (evenly spaced, mid-scroll) to /data/ps5webkit/shots/pad-NNN-down|up.ppm.
// Content: "delay=15 secs=4 ry=0.5 pause=4 shots=3" (any subset). Also
// rx=<0-1> (horizontal: "down" scrolls right), px=/py=<0-1> (where the events
// land, as a fraction of the view; default the centre, as padTick without a
// pointer), hold=<s> (still between down and up), rest=<s> (one capture
// that long into each hold and pause, after any snap: pad-NNN-rest.ppm).
struct PadScrollTest {
    bool on { false };
    double delay { 15 }, secs { 4 }, ry { 0.5 }, rx { 0 }, pause { 4 };
    double px { 0.5 }, py { 0.5 }, rest { 0 }, hold { 0 };
    int shots { 3 };
    double start { 0 };
    int lastShot { -1 };
    int lastRestShot { -1 };
    unsigned shotCount { 0 };
};
static PadScrollTest s_padScrollTest;

static void padScrollTestInit()
{
    GUniqueOutPtr<char> contents;
    if (!g_file_get_contents("/data/ps5webkit/pad-scroll-test", &contents.outPtr(), nullptr, nullptr))
        return;
    auto& test = s_padScrollTest;
    test.on = true;
    for (char* token = strtok(contents.get(), " \t\r\n"); token; token = strtok(nullptr, " \t\r\n")) {
        double value;
        char key[16];
        if (sscanf(token, "%15[a-z]=%lf", key, &value) != 2)
            continue;
        if (!strcmp(key, "delay"))
            test.delay = value;
        else if (!strcmp(key, "secs"))
            test.secs = value;
        else if (!strcmp(key, "ry"))
            test.ry = value;
        else if (!strcmp(key, "rx"))
            test.rx = value;
        else if (!strcmp(key, "px"))
            test.px = value;
        else if (!strcmp(key, "py"))
            test.py = value;
        else if (!strcmp(key, "rest"))
            test.rest = value;
        else if (!strcmp(key, "hold"))
            test.hold = value;
        else if (!strcmp(key, "pause"))
            test.pause = value;
        else if (!strcmp(key, "shots"))
            test.shots = static_cast<int>(value);
    }
    PS5_LOG("pad-scroll-test: from %.0f s, %.1f s down / up at rx %.2f ry %.2f (at %.2f, %.2f of the view), %.1f s hold, %.1f s pause, %d shots per direction, rest shot %.1f s",
        test.delay, test.secs, test.rx, test.ry, test.px, test.py, test.hold, test.pause, test.shots, test.rest);
}

static void padScrollTestTick(WPEDisplayPS5* display)
{
    auto& test = s_padScrollTest;
    WPEView* view = display->priv->activeView;
    if (!test.on || !view)
        return;
    double t = nowSeconds();
    if (!test.start)
        test.start = t + test.delay;
    // down, hold (still), up, pause (still)
    double cycle = 2 * test.secs + test.hold + test.pause;
    double at = t - test.start;
    if (at < 0 || cycle <= 0)
        return;
    double phase = std::fmod(at, cycle);
    bool inHold = phase >= test.secs && phase < test.secs + test.hold;
    bool inPause = phase >= 2 * test.secs + test.hold;
    if (inHold || inPause) {
        double still = inHold ? phase - test.secs : phase - 2 * test.secs - test.hold;
        int restSlot = static_cast<int>(at / cycle) * 2 + (inPause ? 1 : 0);
        if (test.rest > 0 && still >= test.rest && restSlot != test.lastRestShot) {
            test.lastRestShot = restSlot;
            char path[128];
            snprintf(path, sizeof path, "/data/ps5webkit/shots/pad-%03u-rest.ppm", ++test.shotCount);
            wpe_display_ps5_capture_screen(display, path);
        }
        return;
    }
    bool down = phase < test.secs;
    double ry = down ? test.ry : -test.ry;
    double rx = down ? test.rx : -test.rx;
    const double speed = 14.0; // as padTick
    double x = wpe_view_get_width(view) * test.px, y = wpe_view_get_height(view) * test.py;
    GRefPtr<WPEEvent> event = adoptGRef(wpe_event_scroll_new(view, WPE_INPUT_SOURCE_MOUSE, eventTime(),
        static_cast<WPEModifiers>(0), -rx * speed, -ry * speed, TRUE, FALSE, x, y));
    wpe_view_event(view, event.get());
    if (test.shots > 0) {
        double within = down ? phase : phase - test.secs - test.hold;
        int k = static_cast<int>(within / (test.secs / (test.shots + 1)));
        int slot = (static_cast<int>(at / cycle) * 2 + (down ? 0 : 1)) * (test.shots + 1) + k;
        if (k >= 1 && k <= test.shots && slot != test.lastShot) {
            test.lastShot = slot;
            char path[128];
            snprintf(path, sizeof path, "/data/ps5webkit/shots/pad-%03u-%s.ppm", ++test.shotCount, down ? "down" : "up");
            wpe_display_ps5_capture_screen(display, path);
        }
    }
}

static void padTick(WPEDisplayPS5* display)
{
    auto* priv = display->priv;
    pollTextDialog(display);
    padScrollTestTick(display);

    WPEView* view = priv->activeView;
    if (priv->pad < 0 || !view)
        return;

    PS5PadData data { };
    if (scePadReadState(priv->pad, &data))
        return;

    // While the on-screen keyboard is up, the pad belongs to it.
    if (priv->pendingText) {
        priv->buttons = data.buttons;
        priv->ignoredButtons = data.buttons;
        priv->repeatingKey = 0;
        return;
    }

    uint32_t now = data.buttons;
    priv->ignoredButtons &= now;
    now &= ~priv->ignoredButtons;
    uint32_t pressed = now & ~priv->buttons;
    uint32_t released = priv->buttons & ~now;
    priv->buttons = now;
    const double t = nowSeconds();
    if (pressed)
        priv->lastPress = t;

    // The application first (View in VR); what it takes, the page never sees.
    if (uint32_t swallowedReleases = released & priv->swallowedButtons) {
        released &= ~swallowedReleases;
        priv->swallowedButtons &= ~swallowedReleases;
    }
    if (pressed && priv->buttonHandler) {
        bool wasCaptured = priv->padCaptured;
        bool taken = priv->buttonHandler(display, pressed, priv->buttonHandlerData);
        if (priv->padCaptured != wasCaptured || priv->activeView != view)
            return; // the handler changed the pad's owner (held buttons are ignored until released)
        if (taken) {
            priv->swallowedButtons |= pressed;
            pressed = 0;
        }
    }
    if (priv->padCaptured) {
        priv->repeatingKey = 0;
        return;
    }

    // Left stick: pointer.
    double sx = stickValue(data.sticks[0]), sy = stickValue(data.sticks[1]);
    int width = wpe_view_get_width(view), height = wpe_view_get_height(view);
    bool pointerVisible = t - priv->pointerMovedAt < pointerShownFor;
    if (sx || sy) {
        if (!pointerVisible && priv->pointerX == 0 && priv->pointerY == 0) {
            priv->pointerX = width / 2.0;
            priv->pointerY = height / 2.0;
        }
        const double speed = 22.0; // px per tick at full deflection
        double dx = sx * speed, dy = sy * speed;
        priv->pointerX = std::fmin(std::fmax(priv->pointerX + dx, 0), width - 1);
        priv->pointerY = std::fmin(std::fmax(priv->pointerY + dy, 0), height - 1);
        priv->pointerMovedAt = t;
        auto modifiers = priv->pointerDown ? WPE_MODIFIER_POINTER_BUTTON1 : static_cast<WPEModifiers>(0);
        GRefPtr<WPEEvent> event = adoptGRef(wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, view, WPE_INPUT_SOURCE_MOUSE,
            eventTime(), modifiers, priv->pointerX, priv->pointerY, dx, dy));
        wpe_view_event(view, event.get());
        wpeDisplayPS5ScheduleRedraw(display);
        pointerVisible = true;
    } else if (priv->pointerWasVisible && !pointerVisible)
        wpeDisplayPS5ScheduleRedraw(display); // hide it

    // Right stick: scroll under the pointer (or the centre).
    double rx = stickValue(data.sticks[2]), ry = stickValue(data.sticks[3]);
    if (rx || ry) {
        const double speed = 14.0; // WebKit multiplies precise deltas by 2.5
        double px = pointerVisible ? priv->pointerX : width / 2.0;
        double py = pointerVisible ? priv->pointerY : height / 2.0;
        GRefPtr<WPEEvent> event = adoptGRef(wpe_event_scroll_new(view, WPE_INPUT_SOURCE_MOUSE, eventTime(),
            static_cast<WPEModifiers>(0), -rx * speed, -ry * speed, TRUE, FALSE, px, py));
        wpe_view_event(view, event.get());
    }

    // Cross: click when the pointer is shown, otherwise Return.
    if (pressed & PS5_PAD_CROSS) {
        if (pointerVisible) {
            priv->pointerDown = true;
            guint count = wpe_view_compute_press_count(view, priv->pointerX, priv->pointerY, WPE_BUTTON_PRIMARY, eventTime());
            GRefPtr<WPEEvent> event = adoptGRef(wpe_event_pointer_button_new(WPE_EVENT_POINTER_DOWN, view, WPE_INPUT_SOURCE_MOUSE,
                eventTime(), WPE_MODIFIER_POINTER_BUTTON1, WPE_BUTTON_PRIMARY, priv->pointerX, priv->pointerY, count));
            wpe_view_event(view, event.get());
            priv->pointerMovedAt = t;
        } else
            sendKey(view, WPE_KEY_Return, 36, static_cast<WPEModifiers>(0), true);
    }
    if (released & PS5_PAD_CROSS) {
        if (priv->pointerDown) {
            priv->pointerDown = false;
            GRefPtr<WPEEvent> event = adoptGRef(wpe_event_pointer_button_new(WPE_EVENT_POINTER_UP, view, WPE_INPUT_SOURCE_MOUSE,
                eventTime(), static_cast<WPEModifiers>(0), WPE_BUTTON_PRIMARY, priv->pointerX, priv->pointerY, 0));
            wpe_view_event(view, event.get());
        } else
            sendKey(view, WPE_KEY_Return, 36, static_cast<WPEModifiers>(0), false);
    }

    // L3 + R3 (both sticks pressed in): the performance overlay on or off.
    // L3's and R3's own keys (home page, Settings) go out when the button
    // comes back up instead of when it goes down, and not after the chord.
    constexpr uint32_t stickButtons = PS5_PAD_L3 | PS5_PAD_R3;
    if ((pressed & stickButtons) && (now & stickButtons) == stickButtons) {
        priv->stickChord = true;
        bool on = !PS5Renderer::singleton().perfOverlay();
        PS5Renderer::singleton().setPerfOverlay(on);
        PS5Renderer::singleton().showMessage(on ? "Performance overlay on (L3 + R3)" : "Performance overlay off", 1.5);
        PS5_LOG("pad: L3 + R3, performance overlay %s", on ? "on" : "off");
        if (priv->perfOverlayCallback)
            priv->perfOverlayCallback(display, on, priv->perfOverlayCallbackData);
    }
    if (uint32_t stickReleased = released & stickButtons) {
        for (const auto& mapping : keyMappings) {
            if (!priv->stickChord && (stickReleased & mapping.button)) {
                sendKey(view, mapping.keyval, mapping.keycode, mapping.modifiers, true);
                sendKey(view, mapping.keyval, mapping.keycode, mapping.modifiers, false);
            }
        }
        if (!(now & stickButtons))
            priv->stickChord = false;
    }
    pressed &= ~stickButtons;
    released &= ~stickButtons;

    // Triangle: the on-screen keyboard for the focused field.
    if ((pressed & PS5_PAD_TRIANGLE) && priv->focusedInputContext)
        wpeInputMethodContextPS5OpenKeyboard(priv->focusedInputContext);

    for (const auto& mapping : keyMappings) {
        if (pressed & mapping.button) {
            sendKey(view, mapping.keyval, mapping.keycode, mapping.modifiers, true);
            if (mapping.repeats) {
                priv->repeatingKey = mapping.button;
                priv->repeatNext = t + repeatDelay;
            }
        }
        if (released & mapping.button) {
            sendKey(view, mapping.keyval, mapping.keycode, mapping.modifiers, false);
            if (priv->repeatingKey == mapping.button)
                priv->repeatingKey = 0;
        }
    }

    // Auto-repeat of the last held direction/tab key.
    if (priv->repeatingKey && (now & priv->repeatingKey) && t >= priv->repeatNext) {
        for (const auto& mapping : keyMappings) {
            if (mapping.button == priv->repeatingKey) {
                sendKey(view, mapping.keyval, mapping.keycode, mapping.modifiers, true);
                break;
            }
        }
        priv->repeatNext = t + repeatInterval;
    }
}

// ---------------------------------------------------------------- WPEDisplay

static void wpeDisplayPS5Dispose(GObject* object)
{
    auto* priv = WPE_DISPLAY_PS5(object)->priv;
    if (priv->padSource) {
        g_source_destroy(priv->padSource.get());
        priv->padSource = nullptr;
    }
    if (priv->redrawSource) {
        g_source_destroy(priv->redrawSource.get());
        priv->redrawSource = nullptr;
    }
    if (priv->pad >= 0 && scePadClose)
        scePadClose(priv->pad);
    priv->pad = -1;
    G_OBJECT_CLASS(wpe_display_ps5_parent_class)->dispose(object);
}

static gboolean wpeDisplayPS5Connect(WPEDisplay* display, GError** error)
{
    auto* displayPS5 = WPE_DISPLAY_PS5(display);
    auto* priv = displayPS5->priv;

    char message[512] = { };
    if (!PS5Renderer::singleton().initialize(message, sizeof(message))) {
        g_set_error(error, WPE_DISPLAY_ERROR, WPE_DISPLAY_ERROR_CONNECTION_FAILED, "PS5: %s", message);
        return FALSE;
    }

    const int width = PS5Renderer::singleton().width();
    const int height = PS5Renderer::singleton().height();
    priv->screen = adoptGRef(WPE_SCREEN(g_object_new(WPE_TYPE_SCREEN_PS5, "id", 1, nullptr)));
    wpe_screen_set_position(priv->screen.get(), 0, 0);
    wpe_screen_set_size(priv->screen.get(), width, height);
    // A 1080p TV seen from a couch: report a large panel so CSS mm make sense.
    wpe_screen_set_physical_size(priv->screen.get(), 1210, 680);
    wpe_screen_set_scale(priv->screen.get(), 1.0);
    wpe_screen_set_refresh_rate(priv->screen.get(), 60000);
    wpe_display_screen_added(display, priv->screen.get());

    wpe_display_set_available_input_devices(display, static_cast<WPEAvailableInputDevices>(WPE_AVAILABLE_INPUT_DEVICE_MOUSE | WPE_AVAILABLE_INPUT_DEVICE_KEYBOARD));

    openPad(displayPS5);
    padScrollTestInit();
    // The system keyboard: loaded at run time by the title (PS5Sce.h).
    if (ps5wk_title_load_ime && !ps5wk_title_load_ime(&priv->ime)) {
        int rc = priv->ime.commonDialogInitialize ? priv->ime.commonDialogInitialize() : -1;
        PS5_LOG("ime: libSceImeDialog bound; sceCommonDialogInitialize -> 0x%x", rc);
    } else {
        priv->ime = { };
        PS5_LOG("ime: no system keyboard (libSceImeDialog not bound)");
    }

    priv->padSource = adoptGRef(g_timeout_source_new(padIntervalMS));
    g_source_set_priority(priv->padSource.get(), RunLoopSourcePriority::RunLoopTimer);
    g_source_set_name(priv->padSource.get(), "WPE PS5 pad");
    g_source_set_callback(priv->padSource.get(), [](gpointer userData) -> gboolean {
        padTick(WPE_DISPLAY_PS5(userData));
        return G_SOURCE_CONTINUE;
    }, display, nullptr);
    g_source_attach(priv->padSource.get(), g_main_context_get_thread_default());

    // Title load for the TV-test logs, every 5 s while it is above 2 %:
    // main (UI) thread busy = wall time outside the main loop's poll (a
    // poll function that times itself); process CPU from getrusage; thread
    // CPU clocks where the system has them (-1 otherwise).
    g_main_context_set_poll_func(g_main_context_default(), [](GPollFD* fds, guint n, gint timeout) -> gint {
        gint64 start = g_get_monotonic_time();
        gint result = g_poll(fds, n, timeout);
        s_mainPollUS += g_get_monotonic_time() - start;
        return result;
    });
    g_timeout_add_seconds(5, [](gpointer) -> gboolean {
        static gint64 lastTime, lastPoll;
        static double lastProcess = -1, lastRender = -1;
        struct rusage usage;
        double process = !getrusage(RUSAGE_SELF, &usage)
            ? usage.ru_utime.tv_sec + usage.ru_stime.tv_sec + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6 : -1;
        double render = PS5Renderer::singleton().renderThreadCPUSeconds();
        gint64 now = g_get_monotonic_time(), poll = s_mainPollUS;
        if (lastTime) {
            double seconds = (now - lastTime) / 1e6;
            double mainBusy = 1.0 - (poll - lastPoll) / 1e6 / seconds;
            double processShare = process >= 0 && lastProcess >= 0 ? (process - lastProcess) / seconds : -1;
            double renderShare = render >= 0 && lastRender >= 0 ? (render - lastRender) / seconds : -1;
            if (mainBusy > 0.02 || processShare > 0.05)
                PS5_LOG("[cpu] title: main thread busy %.0f %% (outside poll), process CPU %.2f cores, render thread CPU %.0f %%",
                    100 * mainBusy, processShare, 100 * renderShare);
        }
        lastTime = now;
        lastPoll = poll;
        lastProcess = process;
        lastRender = render;
        return G_SOURCE_CONTINUE;
    }, nullptr);

    // WebProcess and NetworkProcess are elfldr payloads (FEASIBILITY §4.8).
    priv->processManager = adoptGRef(wpe_process_manager_ps5_new(priv->childELF.c_str()));
    wpe_process_manager_set_default(priv->processManager.get());

    PS5_LOG("display connected: %dx%d, pad %d", width, height, priv->pad);
    return TRUE;
}

static WPEView* wpeDisplayPS5CreateView(WPEDisplay* display)
{
    return WPE_VIEW(g_object_new(WPE_TYPE_VIEW_PS5, "display", display, nullptr));
}

static WPEToplevel* wpeDisplayPS5CreateToplevel(WPEDisplay* display, guint)
{
    return WPE_TOPLEVEL(g_object_new(WPE_TYPE_TOPLEVEL_PS5, "display", display, "max-views", 1, nullptr));
}

static gpointer wpeDisplayPS5GetEGLDisplay(WPEDisplay*, GError** error)
{
    if (auto* eglDisplay = PS5Renderer::singleton().eglDisplay())
        return eglDisplay;
    g_set_error_literal(error, WPE_EGL_ERROR, WPE_EGL_ERROR_NOT_AVAILABLE, "PS5: EGL not initialized");
    return nullptr;
}

static guint wpeDisplayPS5GetNScreens(WPEDisplay* display)
{
    return WPE_DISPLAY_PS5(display)->priv->screen ? 1 : 0;
}

static WPEScreen* wpeDisplayPS5GetScreen(WPEDisplay* display, guint index)
{
    return index ? nullptr : WPE_DISPLAY_PS5(display)->priv->screen.get();
}

static WPEInputMethodContext* wpeDisplayPS5CreateInputMethodContext(WPEDisplay*, WPEView* view)
{
    return wpeInputMethodContextPS5New(view);
}

static void wpe_display_ps5_class_init(WPEDisplayPS5Class* displayPS5Class)
{
    GObjectClass* objectClass = G_OBJECT_CLASS(displayPS5Class);
    objectClass->dispose = wpeDisplayPS5Dispose;

    WPEDisplayClass* displayClass = WPE_DISPLAY_CLASS(displayPS5Class);
    displayClass->connect = wpeDisplayPS5Connect;
    displayClass->create_view = wpeDisplayPS5CreateView;
    displayClass->create_toplevel = wpeDisplayPS5CreateToplevel;
    displayClass->get_egl_display = wpeDisplayPS5GetEGLDisplay;
    displayClass->get_n_screens = wpeDisplayPS5GetNScreens;
    displayClass->get_screen = wpeDisplayPS5GetScreen;
    displayClass->create_input_method_context = wpeDisplayPS5CreateInputMethodContext;
}

/**
 * wpe_display_ps5_new:
 *
 * Create a new #WPEDisplayPS5.
 *
 * Returns: (transfer full): a #WPEDisplay
 */
WPEDisplay* wpe_display_ps5_new(void)
{
    return WPE_DISPLAY(g_object_new(WPE_TYPE_DISPLAY_PS5, nullptr));
}

void wpe_display_ps5_set_child_elf(WPEDisplayPS5* display, const char* path)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    display->priv->childELF = path ? path : "";
    if (display->priv->processManager)
        wpe_process_manager_ps5_set_child_elf(display->priv->processManager.get(), path);
}

void wpe_display_ps5_set_log_sink(WPEDisplayPS5* display, const char* host, int port)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    display->priv->logHost = host ? host : "";
    display->priv->logPort = port;
}

void wpe_display_ps5_capture_screen(WPEDisplayPS5* display, const char* path)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    g_return_if_fail(path);
    PS5Renderer::singleton().requestCapture(path);
    wpeDisplayPS5ScheduleRedraw(display);
}

// ---------------------------------------------------------------- the pad for the application, View in VR

void wpe_display_ps5_set_button_handler(WPEDisplayPS5* display, WPEDisplayPS5ButtonHandler handler, gpointer userData)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    display->priv->buttonHandler = handler;
    display->priv->buttonHandlerData = userData;
}

// Buttons held while the owner changes are ignored until released, so
// neither side sees half a press.
static void setPadCaptured(WPEDisplayPS5* display, bool captured)
{
    auto* priv = display->priv;
    if (priv->padCaptured == captured)
        return;
    priv->padCaptured = captured;
    priv->ignoredButtons |= priv->buttons;
    priv->buttons = 0;
    priv->swallowedButtons = 0;
    priv->repeatingKey = 0;
    if (priv->pointerDown && priv->activeView) {
        priv->pointerDown = false;
        GRefPtr<WPEEvent> event = adoptGRef(wpe_event_pointer_button_new(WPE_EVENT_POINTER_UP, priv->activeView, WPE_INPUT_SOURCE_MOUSE,
            eventTime(), static_cast<WPEModifiers>(0), WPE_BUTTON_PRIMARY, priv->pointerX, priv->pointerY, 0));
        wpe_view_event(priv->activeView, event.get());
    }
    if (!captured) {
        priv->pointerMovedAt = -1000; // pointer hidden until the stick moves again
        wpeDisplayPS5ScheduleRedraw(display);
    }
    PS5_LOG("pad %s", captured ? "captured (View in VR)" : "back to the page");
}

void wpe_display_ps5_set_perf_overlay(WPEDisplayPS5* display, gboolean enabled)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    PS5Renderer::singleton().setPerfOverlay(enabled);
}

gboolean wpe_display_ps5_get_perf_overlay(WPEDisplayPS5* display)
{
    g_return_val_if_fail(WPE_IS_DISPLAY_PS5(display), FALSE);
    return PS5Renderer::singleton().perfOverlay();
}

void wpe_display_ps5_set_perf_overlay_callback(WPEDisplayPS5* display, WPEDisplayPS5PerfOverlayCallback callback, gpointer userData)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    display->priv->perfOverlayCallback = callback;
    display->priv->perfOverlayCallbackData = userData;
}

void wpe_display_ps5_set_perf_counter(WPEDisplayPS5*, const char* name, guint64 value)
{
    ps5PerfSetCounter(name, value);
}

void wpe_display_ps5_show_message(WPEDisplayPS5* display, const char* text, double seconds)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    PS5Renderer::singleton().showMessage(text, seconds);
}

void wpe_display_ps5_vr_start(WPEDisplayPS5* display, WPEDisplayPS5VRProjection projection, double fisheyeDegrees,
    WPEDisplayPS5VRCallback callback, gpointer userData)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    auto* priv = display->priv;
    if (priv->vrStarting)
        return;
    priv->vrCallback = callback;
    priv->vrCallbackData = userData;
    priv->vrStarting = true;
    // Capture the pad at once: presses while the headset comes up must not
    // reach the page.
    setPadCaptured(display, true);
    PS5Renderer::singleton().startVR(projection, static_cast<float>(fisheyeDegrees),
        [](bool ok, const char* message, void* data) {
            auto* display = WPE_DISPLAY_PS5(data);
            auto* priv = display->priv;
            priv->vrStarting = false;
            if (!ok)
                setPadCaptured(display, false);
            PS5_LOG("View in VR: %s%s%s", ok ? "headset up" : "not started", message && *message ? ": " : "", message ? message : "");
            if (priv->vrCallback)
                priv->vrCallback(display, ok, message, priv->vrCallbackData);
        },
        [](const char* why, void* data) {
            auto* display = WPE_DISPLAY_PS5(data);
            auto* priv = display->priv;
            setPadCaptured(display, false);
            PS5_LOG("View in VR ended: %s", why ? why : "");
            if (priv->vrCallback)
                priv->vrCallback(display, FALSE, why, priv->vrCallbackData);
        },
        display);
}

void wpe_display_ps5_vr_stop(WPEDisplayPS5* display, gboolean wait)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    PS5Renderer::singleton().stopVR(wait);
    setPadCaptured(display, false);
}

gboolean wpe_display_ps5_vr_is_active(WPEDisplayPS5* display)
{
    g_return_val_if_fail(WPE_IS_DISPLAY_PS5(display), FALSE);
    return PS5Renderer::singleton().vrActive();
}

void wpe_display_ps5_vr_set_projection(WPEDisplayPS5* display, WPEDisplayPS5VRProjection projection, double fisheyeDegrees)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    PS5Renderer::singleton().setVRProjection(projection, static_cast<float>(fisheyeDegrees));
}

void wpe_display_ps5_vr_recenter(WPEDisplayPS5* display)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    PS5Renderer::singleton().recenterVR();
}

void wpe_display_ps5_vr_set_text(WPEDisplayPS5* display, const char* hint, double seconds, const char* tvText)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    PS5Renderer::singleton().setVRText(hint, seconds, tvText);
}

void wpe_display_ps5_vr_sim_look(WPEDisplayPS5* display, double yaw, double pitch)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    PS5Renderer::singleton().setVRSimLook(static_cast<float>(yaw), static_cast<float>(pitch));
}

struct ThumbnailsRequest {
    WPEDisplayPS5* display;
    WPEDisplayPS5ThumbnailsCallback callback;
    gpointer userData;
};

void wpe_display_ps5_video_thumbnails(WPEDisplayPS5* display, guint frames, WPEDisplayPS5ThumbnailsCallback callback, gpointer userData)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    auto* request = new ThumbnailsRequest { WPE_DISPLAY_PS5(g_object_ref(display)), callback, userData };
    PS5Renderer::singleton().requestVideoThumbnails(frames,
        [](const uint8_t* luma, unsigned count, int width, int height, void* data) {
            auto* request = static_cast<ThumbnailsRequest*>(data);
            if (request->callback)
                request->callback(request->display, luma, count, PS5Renderer::kThumbnailSize, width, height, request->userData);
            g_object_unref(request->display);
            delete request;
        },
        request);
}
