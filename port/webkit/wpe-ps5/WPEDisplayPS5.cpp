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
#include <atomic>
#include <pthread.h>
#include <sys/resource.h>
#include <time.h>
#include "PS5Sce.h"
#include "WPEGamepadPS5.h"
#include "WPEExtensions.h"
#include "WPEPS5Private.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <gio/gio.h>
#include <wtf/glib/GRefPtr.h>
#include <wtf/glib/RunLoopSourcePriority.h>
#include <wtf/glib/GUniquePtr.h>
#include <wtf/glib/WTFGType.h>
#include <string>
#include <vector>

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
 *   Cross (X)        click at the pointer when it is shown, else Return;
 *                    over a link the application can open, the click is
 *                    sent at the release, and held 0.6 s it is the
 *                    application's instead (the link in a new tab)
 *   Circle (O)       Back (XF86Back; the browser goes back)
 *   Triangle         on-screen keyboard for the focused field (it also
 *                    opens by itself when a click on a field, or the D-pad
 *                    or L1/R1, focuses one)
 *   Square           Reload (XF86Reload)
 *   L1 / R1          Shift+Tab / Tab (previous / next focusable element)
 *   L2 / R2          ZoomOut / ZoomIn (XF86ZoomOut/In)
 *   Options          Menu (the browser opens its URL prompt)
 *   R3               Tools (XF86Tools; the browser's View in VR)
 *   L3               HomePage (XF86HomePage; the browser loads its home page)
 *   left stick       moves the drawn pointer
 *   right stick      scrolls under the pointer
 *   touchpad         a laptop's touchpad: one finger moves the pointer, two
 *                    scroll under it, a press clicks (and drags while held)
 *   (the application's button handler sees presses first; ps5browser takes
 *   the whole pad while in VR, and Options for its menu, which puts the pad
 *   in UI mode while open; in UI mode the touchpad moves the UI's pointer
 *   and scrolls it, and its press goes to the handler; gamepad mode leaves
 *   the touchpad to the page)
 */

static constexpr unsigned padIntervalMS = 8;
static constexpr double kSystemHeldEndsVR = 10.0; // seconds
static constexpr double repeatDelay = 0.40;
static constexpr double repeatInterval = 0.06;
static constexpr double pointerShownFor = 8.0; // seconds after the last stick move
static constexpr int stickDeadZone = 14; // of 127
static constexpr double optionsHoldFor = 1.0; // gamepad mode: Options held this long is the browser's
static constexpr double triangleHoldFor = 1.0; // a page outside gamepad mode: Triangle held this long is the browser's
static constexpr double crossHoldFor = 0.6; // the pointer over a link: Cross held this long is the browser's (a new tab)
// The touchpad: a press or release shifts the finger a little; no pointer
// moves for this long after one, so a click lands where the pointer was.
static constexpr double touchClickStill = 0.10; // seconds
static constexpr double touchpadWidth = 1920; // a DualSense's touch x is 0-1919

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
    // The pad's stall thread (padStallThread): when padTick last ran, and
    // whether it was on a page with the right stick scrolling it.
    std::atomic<double> padTickAt { 0 };
    std::atomic<bool> padTickScrolls { false };
    GMutex padReadLock { }; // scePadReadState: padTick or the stall thread
    uint32_t buttons { 0 };
    uint32_t ignoredButtons { 0 };
    uint32_t repeatingKey { 0 };
    double repeatNext { 0 };
    double lastPress { -1000 };
    // What the last press was, for the keyboard: focus that follows a click
    // on a text field or a move of the focus (D-pad, L1/R1) opens it, focus
    // a page moves by itself (on load, or in a button's click handler) does
    // not.
    enum class PressKind : uint8_t { Other, PointerClick, FocusMove };
    PressKind lastPressKind { PressKind::Other };
    // When an editable field last got focus: a click on a field that does
    // not move the focus (the field already has it, say after a form's
    // validation focused it) opens the keyboard itself.
    double focusedAt { -1000 };
    // When the keyboard's Go sent Return: Return in a form field moves the
    // focus to the next field (WebKit patch 0091), and the keyboard opens
    // there, as on a phone.
    double goSentAt { -1000 };
    // The open keyboard's top edge on screen, for the page lift (see
    // keyboardLiftFor); the lift now applied.
    float keyboardTop { 0 };
    int keyboardLift { 0 };
    GRefPtr<GSource> padSource;

    double pointerX { 0 };
    double pointerY { 0 };
    double pointerMovedAt { -1000 };
    bool pointerDown { false };
    uint32_t pointerDownBy { 0 }; // the button holding the click: Cross or the touchpad
    bool returnDown { false }; // Cross sent Return down (the pointer was hidden)
    bool pointerWasVisible { false };
    // The touchpad's fingers at the last read (how many, the first one's
    // id, where they were on average), to move by what changed; the
    // touchpad button's state, for touchClickStill.
    uint8_t touchCount { 0 };
    uint8_t touchID { 0 };
    double touchX { 0 };
    double touchY { 0 };
    bool touchPressed { false };
    double touchStillUntil { 0 };
    // Whether the pointer is over an editable node, from the application's
    // hit tests (wpe_display_ps5_set_pointer_over_editable); until it
    // reports one, any click counts as a click on a field.
    bool pointerOverEditable { false };
    bool pointerTargetReported { false };
    // Whether the pointer is over a link the application can open
    // (wpe_display_ps5_set_pointer_over_link), and when Cross went down
    // over one (-1: not waiting). Until Cross comes back up (a click, sent
    // whole then) or crossHoldFor passes (the application's: the link in a
    // new tab), the page has seen nothing of the press.
    bool pointerOverLink { false };
    double crossLinkDownAt { -1 };

    GRefPtr<GSource> redrawSource;

    std::unique_ptr<PendingText> pendingText;
    PS5ImeFunctions ime { };

    // The application's button handler; the pad is captured (only the
    // handler gets it) while the video plays in the headset.
    WPEDisplayPS5ButtonHandler buttonHandler { nullptr };
    gpointer buttonHandlerData { nullptr };
    WPEDisplayPS5CapturedPadCallback capturedPadCallback { nullptr };
    gpointer capturedPadCallbackData { nullptr };
    uint32_t swallowedButtons { 0 };
    bool padCaptured { false };
    // What the pad read says about the system (logged on change): the read's
    // result, the intercepted bit (the system's UI has the pad), connected.
    // While captured, the system holding the pad for kSystemHeldEndsVR with
    // no press reaching View in VR ends VR (the title must never be left in
    // VR mode with no way out).
    int padReadResult { 0 };
    bool padIntercepted { false };
    int padConnected { -1 };
    unsigned padStateLogs { 0 };
    double systemHeldSince { -1 };
    double capturedPressAt { 0 };
    bool systemHeldEndedVR { false };
    bool stickChord { false }; // L3 + R3 went down together: their own keys are not sent
    // UI mode (wpe_display_ps5_set_ui_mode): the application's own UI has
    // the pad; D-pad/L1/R1/L2/R2 presses repeat while held.
    bool uiMode { false };
    uint32_t uiRepeatButton { 0 };
    double uiRepeatNext { 0 };
    bool choosesActiveView { false }; // wpe_display_ps5_set_active_view was called
    WPEDisplayPS5UIPointerCallback uiPointerCallback { nullptr };
    gpointer uiPointerCallbackData { nullptr };
    WPEDisplayPS5PerfOverlayCallback perfOverlayCallback { nullptr };
    gpointer perfOverlayCallbackData { nullptr };
    WPEDisplayPS5VRCallback vrCallback { nullptr };
    gpointer vrCallbackData { nullptr };
    bool vrStarting { false };

    // The Gamepad API's device (WPEGamepadPS5.cpp; owned by WebKit's
    // manager, which exists while a page uses gamepads) and whether WebKit
    // reads it now.
    WPEGamepad* gamepad { nullptr };
    bool gamepadMonitoring { false };
    WPEDisplayPS5GamepadCallback gamepadCallback { nullptr };
    gpointer gamepadCallbackData { nullptr };
    bool vibrationModeSet { false };
    unsigned rumbleLogs { 0 };
    // Gamepad mode (wpe_display_ps5_set_gamepad_mode): the keys the buttons
    // send, the sticks, and what is down now.
    bool gamepadMode { false };
    std::vector<WPEDisplayPS5ButtonKey> gamepadKeys;
    WPEDisplayPS5StickAction stickActions[2] { WPE_DISPLAY_PS5_STICK_GAMEPAD, WPE_DISPLAY_PS5_STICK_GAMEPAD };
    uint32_t gamepadKeysDown { 0 }; // buttons whose keys went down
    uint8_t stickKeysDown[2] { 0, 0 }; // per stick: up, down, left, right
    double optionsDownAt { -1 };
    bool optionsHoldSent { false };
    // Outside gamepad mode: Triangle held on the page (it turns gamepad mode on).
    double triangleDownAt { -1 };
    bool triangleHoldSent { false };

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
    // The page's pointer is in view coordinates, the UI's in the screen's.
    double top = priv->uiMode ? 0 : PS5Renderer::singleton().pageTop();
    PS5Renderer::singleton().present(visible, priv->pointerX, priv->pointerY + top);
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

bool wpeDisplayPS5ChoosesActiveView(WPEDisplayPS5* display)
{
    return display->priv->choosesActiveView;
}

WPEView* wpeDisplayPS5GetActiveView(WPEDisplayPS5* display)
{
    return display->priv->activeView;
}

void wpeDisplayPS5SetFocusedInputContext(WPEDisplayPS5* display, WPEInputMethodContext* context)
{
    display->priv->focusedInputContext = context;
    display->priv->focusedAt = nowSeconds();
}

// A click on an editable node opens the keyboard through the focus change it
// causes (wpeDisplayPS5FocusFollowsPressOnField). A click on the field that
// already has focus causes none, so after the click, once any focus change
// would have arrived, the keyboard opens for that field if focus stayed put.
// Should a focus change arrive later still, the dialog's text is dropped
// rather than typed elsewhere (the context checks it is still focused).
static void openKeyboardIfFocusStayed(WPEDisplayPS5* display, double clickAt)
{
    struct Click {
        GRefPtr<WPEDisplayPS5> display;
        GRefPtr<WPEInputMethodContext> context;
        double at;
    };
    auto* priv = display->priv;
    if (!priv->focusedInputContext || !priv->pointerOverEditable || !priv->pointerTargetReported || priv->pendingText)
        return;
    auto* click = new Click { display, priv->focusedInputContext, clickAt };
    g_timeout_add_full(G_PRIORITY_DEFAULT, 200, [](gpointer data) -> gboolean {
        auto* click = static_cast<Click*>(data);
        auto* priv = click->display->priv;
        if (priv->focusedInputContext == click->context.get() && priv->focusedAt < click->at && !priv->pendingText) {
            PS5_LOG("IME: click on the focused field: keyboard opens");
            wpeInputMethodContextPS5OpenKeyboard(click->context.get());
        }
        return G_SOURCE_REMOVE;
    }, click, [](gpointer data) { delete static_cast<Click*>(data); });
}

void wpeDisplayPS5InputContextDestroyed(WPEDisplayPS5* display, WPEInputMethodContext* context)
{
    if (display->priv->focusedInputContext == context)
        display->priv->focusedInputContext = nullptr;
}

bool wpeDisplayPS5FocusFollowsPressOnField(WPEDisplayPS5* display, const char** why)
{
    auto* priv = display->priv;
    using PressKind = _WPEDisplayPS5Private::PressKind;
    const char* reason = nullptr; // no recent press: the page focused the field by itself
    bool open = false;
    if (nowSeconds() - priv->goSentAt < 1.0) {
        open = true;
        reason = "Go on the keyboard (the next field, or one the page chose)";
    } else if (nowSeconds() - priv->lastPress >= 1.0)
        ;
    else if (priv->lastPressKind == PressKind::FocusMove) {
        open = true;
        reason = "D-pad or L1/R1 press (focus moved)";
    } else if (priv->lastPressKind == PressKind::PointerClick) {
        open = priv->pointerOverEditable || !priv->pointerTargetReported;
        reason = !priv->pointerTargetReported ? "click (no hit test)" : open ? "click on a field" : "click outside a field";
    } else
        reason = "press other than a click or a focus move";
    if (why)
        *why = reason;
    return open;
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
    priv->keyboardLift = 0;
    PS5Renderer::singleton().setKeyboardLift(0); // the page back where it was
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
    return wpeDisplayPS5RequestText(display, title, initialText, url, url ? PS5_IME_ENTER_LABEL_GO : PS5_IME_ENTER_LABEL_DEFAULT, !url, false, false, 0, 0, callback, userData);
}

gboolean wpe_display_ps5_request_password(WPEDisplayPS5* display, const char* title, WPEDisplayPS5TextCallback callback, gpointer userData)
{
    g_return_val_if_fail(WPE_IS_DISPLAY_PS5(display), FALSE);
    return wpeDisplayPS5RequestText(display, title, "", false, PS5_IME_ENTER_LABEL_DEFAULT, false, true, true, 0, 0, callback, userData);
}

// How far to draw the page higher so the field being typed into (its
// caret line, in view coordinates) sits above the open keyboard. The view
// fills the screen below the application's bar (PS5Renderer::pageTop) at
// scale 1. Only a field wholly on screen lifts the page
// (the page image holds what is on screen and nothing below it), never by
// more than the keyboard's height and the field's, and never so far that the field's top
// goes under the bar; anything else (a caret position before a scroll that
// reveals the field: patch 0095 sends it again after) leaves the page put.
static int keyboardLiftFor(WPEDisplayPS5* display, int fieldY, int fieldHeight, const char** why)
{
    auto* priv = display->priv;
    const float screenHeight = PS5Renderer::singleton().height();
    const float margin = 32 * screenHeight / 1080.0f; // the gap above the keyboard
    int viewHeight = priv->activeView ? wpe_view_get_height(priv->activeView) : 0;
    if (fieldHeight <= 0 || viewHeight <= 0 || priv->keyboardTop <= 0)
        return 0;
    const float top = PS5Renderer::singleton().pageTop();
    float viewToScreen = (screenHeight - top) / viewHeight;
    // The caret's line, with room for the field's border and padding.
    float fieldTop = top + (fieldY - fieldHeight * 0.5f) * viewToScreen;
    float fieldBottom = top + (fieldY + fieldHeight * 1.5f) * viewToScreen;
    if (fieldY < 0 || top + (fieldY + fieldHeight) * viewToScreen > screenHeight) {
        if (why)
            *why = " (caret off screen: no lift)";
        return 0;
    }
    if (fieldBottom <= priv->keyboardTop - margin)
        return 0;
    float wanted = fieldBottom - (priv->keyboardTop - margin);
    float keepTopVisible = std::max(fieldTop - margin - top, 0.0f);
    // At most the keyboard's height and room for the field: enough for a
    // field at the very bottom of the screen to clear it.
    float most = screenHeight - priv->keyboardTop + margin + (fieldBottom - fieldTop);
    return static_cast<int>(std::min({ wanted, keepTopVisible, most }));
}

void wpeDisplayPS5KeyboardFieldMoved(WPEDisplayPS5* display, int fieldY, int fieldHeight)
{
    auto* priv = display->priv;
    if (!priv->pendingText)
        return;
    int lift = keyboardLiftFor(display, fieldY, fieldHeight, nullptr);
    if (lift == priv->keyboardLift)
        return;
    PS5_LOG("IME: the field moved (caret y %d h %d): page lifted %d px (was %d)", fieldY, fieldHeight, lift, priv->keyboardLift);
    priv->keyboardLift = lift;
    PS5Renderer::singleton().setKeyboardLift(lift);
}

bool wpeDisplayPS5RequestText(WPEDisplayPS5* display, const char* title, const char* initialText, bool url, int32_t enterLabel, bool autoCapitalize, bool password, bool noLearning, int fieldY, int fieldHeight, WPEDisplayPS5TextCallback callback, gpointer userData)
{
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
    param.type = password ? PS5_IME_TYPE_BASIC_LATIN : url ? PS5_IME_TYPE_URL : PS5_IME_TYPE_DEFAULT;
    param.enterLabel = enterLabel;
    param.option = (autoCapitalize ? 0 : PS5_IME_OPTION_NO_AUTO_CAPITALIZATION) | (password ? PS5_IME_OPTION_PASSWORD : 0)
        | (password || noLearning ? PS5_IME_OPTION_NO_LEARNING : 0);
    param.maxTextLength = G_N_ELEMENTS(pending->buffer) - 1;
    param.inputTextBuffer = pending->buffer;
    param.placeholder = pending->placeholder;
    param.title = pending->title;
    // The keyboard opens at the bottom of the screen, as on a phone, and when
    // it would cover the field being typed into, the page is drawn higher
    // (PS5Renderer::setKeyboardLift) until it closes: no scrolling, so a
    // field at the end of a page or in a fixed box rises too, and the page
    // never sees it. The view fills the screen below the application's bar
    // at scale 1. The keyboard's size comes from the system
    // (1920x1080 coordinates), else a guess of 45% of the screen.
    const float screenHeight = PS5Renderer::singleton().height();
    const float toScreen = screenHeight / 1080.0f;
    param.posX = 960;
    param.posY = 1080 - 16;
    param.horizontalAlignment = 1; // centre
    param.verticalAlignment = 2; // 0 top, 1 centre, 2 bottom
    uint32_t panelWidth = 0, panelHeight = 0;
    int sizeRC = priv->ime.getPanelSize ? priv->ime.getPanelSize(&param, &panelWidth, &panelHeight) : -1;
    bool sizeKnown = !sizeRC && panelHeight > 100 && panelHeight < 1080;
    float keyboardTop = sizeKnown ? (1080 - 16 - static_cast<float>(panelHeight)) * toScreen : screenHeight * 0.55f;
    priv->keyboardTop = keyboardTop;
    int viewHeight = priv->activeView ? wpe_view_get_height(priv->activeView) : 0;
    const char* why = nullptr;
    int lift = keyboardLiftFor(display, fieldY, fieldHeight, &why);
    char placement[200];
    snprintf(placement, sizeof placement, "bottom, %s %ux%u%s, caret y %d h %d in a view %d high, page lifted %d px",
        sizeKnown ? "panel" : "panel guessed; system said", panelWidth, panelHeight, sizeRC ? " (no size call)" : "",
        fieldY, fieldHeight, viewHeight, lift);
    if (why)
        strncat(placement, why, sizeof placement - strlen(placement) - 1);

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
    if (rc && (param.option & PS5_IME_OPTION_NO_LEARNING)) {
        // Should the system refuse the no-learning option: without it.
        PS5_LOG("sceImeDialogInit with no learning failed (0x%08x); trying without", rc);
        param.option &= ~PS5_IME_OPTION_NO_LEARNING;
        rc = priv->ime.init(&param, &extended);
        if (rc)
            rc = priv->ime.init(&param, nullptr);
    }
    if (rc) {
        PS5_LOG("sceImeDialogInit failed: 0x%08x", rc);
        return FALSE;
    }
    priv->keyboardLift = lift;
    PS5Renderer::singleton().setKeyboardLift(lift);
    // Never the text itself (a password, or what a page holds): its length.
    PS5_LOG("IME dialog open (%s%s, %zu chars, keyboard at the %s, enter label %s, %s)", password ? "password" : url ? "URL" : "text",
        param.option & PS5_IME_OPTION_NO_LEARNING ? ", no learning" : "", strlen(initialText ? initialText : ""), placement,
        enterLabel == PS5_IME_ENTER_LABEL_SEARCH ? "Search" : enterLabel == PS5_IME_ENTER_LABEL_GO ? "Go" : enterLabel == PS5_IME_ENTER_LABEL_SEND ? "Send" : "default",
        autoCapitalize ? "capitals on" : "no capitals");
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

void wpeDisplayPS5SendReturn(WPEDisplayPS5* display)
{
    if (auto* view = display->priv->activeView) {
        display->priv->goSentAt = nowSeconds();
        tapKey(view, WPE_KEY_Return, 36);
    }
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

// Moves the page's pointer by (dx, dy) view pixels and shows it. It also
// goes up over the application's bar above the page (y < 0, down to
// -PS5Renderer::pageTop()): the page then sees it leave (one move above
// its top), and the application's UI pointer callback hears where it is,
// in screen coordinates, while it is there and once as it comes back.
static void movePointer(WPEDisplayPS5* display, WPEView* view, double dx, double dy, double t)
{
    auto* priv = display->priv;
    int width = wpe_view_get_width(view), height = wpe_view_get_height(view);
    const int top = PS5Renderer::singleton().pageTop();
    bool pointerVisible = t - priv->pointerMovedAt < pointerShownFor;
    if (!pointerVisible && priv->pointerX == 0 && priv->pointerY == 0) {
        priv->pointerX = width / 2.0;
        priv->pointerY = height / 2.0;
    }
    const bool wasOverBar = priv->pointerY < 0;
    priv->pointerX = std::fmin(std::fmax(priv->pointerX + dx, 0), width - 1);
    // Not over the bar while a click holds the page's button (a drag).
    priv->pointerY = std::fmin(std::fmax(priv->pointerY + dy, priv->pointerDown ? 0 : -top), height - 1);
    priv->pointerMovedAt = t;
    const bool overBar = priv->pointerY < 0;
    if (!overBar || !wasOverBar) {
        auto modifiers = priv->pointerDown ? WPE_MODIFIER_POINTER_BUTTON1 : static_cast<WPEModifiers>(0);
        GRefPtr<WPEEvent> event = adoptGRef(wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, view, WPE_INPUT_SOURCE_MOUSE,
            eventTime(), modifiers, priv->pointerX, overBar ? -1 : priv->pointerY, dx, dy));
        wpe_view_event(view, event.get());
    }
    if ((overBar || wasOverBar) && priv->uiPointerCallback)
        priv->uiPointerCallback(display, priv->pointerX, priv->pointerY + top, 0, priv->uiPointerCallbackData);
    wpeDisplayPS5ScheduleRedraw(display);
}

// The left stick's pointer (page pointer events): moves it by the stick's
// deflection; returns whether the pointer is shown now (and hides it when
// its time is up).
static bool stickPointer(WPEDisplayPS5* display, WPEView* view, double sx, double sy, double t)
{
    auto* priv = display->priv;
    if (sx || sy) {
        const double speed = 22.0; // px per tick at full deflection
        movePointer(display, view, sx * speed, sy * speed, t);
        return true;
    }
    bool pointerVisible = t - priv->pointerMovedAt < pointerShownFor;
    if (priv->pointerWasVisible && !pointerVisible)
        wpeDisplayPS5ScheduleRedraw(display); // hide it
    return pointerVisible;
}

// What the touchpad's fingers did since the last read: how many there are
// (0-2) and how far their average moved, in touchpad units. Nothing moves
// on the read a finger lands or lifts (the count or the first finger's id
// changed), nor for touchClickStill after the touchpad's press or release.
// Called on every read, whoever has the pad, so it never jumps.
struct TouchMove {
    int fingers { 0 };
    double dx { 0 };
    double dy { 0 };
};

static TouchMove readTouch(WPEDisplayPS5* display, const PS5PadData& data, double t)
{
    auto* priv = display->priv;
    bool pressed = data.buttons & PS5_PAD_TOUCHPAD;
    if (pressed != priv->touchPressed) {
        priv->touchPressed = pressed;
        priv->touchStillUntil = t + touchClickStill;
    }
    int count = std::min<int>(data.touchCount, 2);
    double x = 0, y = 0;
    for (int i = 0; i < count; i++) {
        x += data.touch[i].x / static_cast<double>(count);
        y += data.touch[i].y / static_cast<double>(count);
    }
    uint8_t id = count ? data.touch[0].id : 0;
    TouchMove move { count, 0, 0 };
    if (count && count == priv->touchCount && id == priv->touchID && t >= priv->touchStillUntil) {
        move.dx = x - priv->touchX;
        move.dy = y - priv->touchY;
    }
    priv->touchCount = count;
    priv->touchID = id;
    priv->touchX = x;
    priv->touchY = y;
    return move;
}

// One finger's move in pointer pixels for a screen @width wide: about one
// pixel per touchpad unit when slow, up to 2.5 when fast (as a laptop's
// touchpad accelerates), so a slow stroke places the pointer precisely and
// a quick flick crosses the screen.
static double touchGain(const TouchMove& move, int width)
{
    double distance = std::hypot(move.dx, move.dy); // per 8 ms read
    return std::fmin(2.5, 0.8 + distance * 0.03) * width / touchpadWidth;
}

// The touchpad on a page: one finger moves the pointer, two scroll under
// it (the page follows the fingers, as on a laptop). Returns whether the
// pointer moved.
static bool touchPointer(WPEDisplayPS5* display, WPEView* view, const TouchMove& move, double t)
{
    if (!move.dx && !move.dy)
        return false;
    int width = wpe_view_get_width(view);
    if (move.fingers == 1) {
        double gain = touchGain(move, width);
        movePointer(display, view, move.dx * gain, move.dy * gain, t);
        return true;
    }
    auto* priv = display->priv;
    bool pointerVisible = t - priv->pointerMovedAt < pointerShownFor;
    double px = pointerVisible ? priv->pointerX : width / 2.0;
    double py = pointerVisible ? std::fmax(priv->pointerY, 0) : wpe_view_get_height(view) / 2.0;
    const double k = 0.5 * width / touchpadWidth; // WebKit multiplies precise deltas by 2.5
    GRefPtr<WPEEvent> event = adoptGRef(wpe_event_scroll_new(view, WPE_INPUT_SOURCE_MOUSE, eventTime(),
        static_cast<WPEModifiers>(0), move.dx * k, move.dy * k, TRUE, FALSE, px, py));
    wpe_view_event(view, event.get());
    return false;
}

// A click at the pointer (Cross while the pointer is shown, or the
// touchpad's press), held by @button until its release.
static void pointerClickDown(WPEDisplayPS5* display, WPEView* view, uint32_t button, double t)
{
    auto* priv = display->priv;
    if (priv->pointerX == 0 && priv->pointerY == 0) {
        priv->pointerX = wpe_view_get_width(view) / 2.0;
        priv->pointerY = wpe_view_get_height(view) / 2.0;
    }
    // A move to where the pointer already is first: the page hit tests it
    // and the application learns what is under the pointer now (the page
    // may have scrolled or changed under a still pointer). That answer
    // reaches the main thread before any focus change the click causes,
    // which decides whether the keyboard opens
    // (wpeDisplayPS5FocusFollowsPressOnField).
    GRefPtr<WPEEvent> move = adoptGRef(wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, view, WPE_INPUT_SOURCE_MOUSE,
        eventTime(), static_cast<WPEModifiers>(0), priv->pointerX, priv->pointerY, 0, 0));
    wpe_view_event(view, move.get());
    priv->lastPressKind = _WPEDisplayPS5Private::PressKind::PointerClick;
    priv->pointerDown = true;
    priv->pointerDownBy = button;
    guint count = wpe_view_compute_press_count(view, priv->pointerX, priv->pointerY, WPE_BUTTON_PRIMARY, eventTime());
    GRefPtr<WPEEvent> event = adoptGRef(wpe_event_pointer_button_new(WPE_EVENT_POINTER_DOWN, view, WPE_INPUT_SOURCE_MOUSE,
        eventTime(), WPE_MODIFIER_POINTER_BUTTON1, WPE_BUTTON_PRIMARY, priv->pointerX, priv->pointerY, count));
    wpe_view_event(view, event.get());
    priv->pointerMovedAt = t;
    wpeDisplayPS5ScheduleRedraw(display);
}

// The right stick's scrolling, under the pointer (or the centre).
static void stickScroll(WPEDisplayPS5* display, WPEView* view, double rx, double ry, bool pointerVisible)
{
    if (!rx && !ry)
        return;
    auto* priv = display->priv;
    const double speed = 14.0; // WebKit multiplies precise deltas by 2.5
    double px = pointerVisible ? priv->pointerX : wpe_view_get_width(view) / 2.0;
    double py = pointerVisible ? std::fmax(priv->pointerY, 0) : wpe_view_get_height(view) / 2.0;
    GRefPtr<WPEEvent> event = adoptGRef(wpe_event_scroll_new(view, WPE_INPUT_SOURCE_MOUSE, eventTime(),
        static_cast<WPEModifiers>(0), -rx * speed, -ry * speed, TRUE, FALSE, px, py));
    wpe_view_event(view, event.get());
}

// ---------------------------------------------------------------- gamepad mode

struct DirectionKey {
    guint keyval;
    guint keycode;
};
// up, down, left, right
static const DirectionKey kArrowKeys[4] = { { WPE_KEY_Up, 111 }, { WPE_KEY_Down, 116 }, { WPE_KEY_Left, 113 }, { WPE_KEY_Right, 114 } };
static const DirectionKey kWASDKeys[4] = { { WPE_KEY_w, 25 }, { WPE_KEY_s, 39 }, { WPE_KEY_a, 38 }, { WPE_KEY_d, 40 } };

static const DirectionKey* directionKeys(WPEDisplayPS5StickAction action)
{
    return action == WPE_DISPLAY_PS5_STICK_ARROWS ? kArrowKeys : action == WPE_DISPLAY_PS5_STICK_WASD ? kWASDKeys : nullptr;
}

// A mouse click at the pointer (a button mapped to WPE_KEY_Pointer_Button1).
static void pointerButton(WPEDisplayPS5* display, WPEView* view, bool down)
{
    auto* priv = display->priv;
    if (down == priv->pointerDown)
        return;
    priv->pointerDown = down;
    if (down) {
        if (priv->pointerX == 0 && priv->pointerY == 0) {
            priv->pointerX = wpe_view_get_width(view) / 2.0;
            priv->pointerY = wpe_view_get_height(view) / 2.0;
        }
        guint count = wpe_view_compute_press_count(view, priv->pointerX, priv->pointerY, WPE_BUTTON_PRIMARY, eventTime());
        GRefPtr<WPEEvent> event = adoptGRef(wpe_event_pointer_button_new(WPE_EVENT_POINTER_DOWN, view, WPE_INPUT_SOURCE_MOUSE,
            eventTime(), WPE_MODIFIER_POINTER_BUTTON1, WPE_BUTTON_PRIMARY, priv->pointerX, priv->pointerY, count));
        wpe_view_event(view, event.get());
    } else {
        GRefPtr<WPEEvent> event = adoptGRef(wpe_event_pointer_button_new(WPE_EVENT_POINTER_UP, view, WPE_INPUT_SOURCE_MOUSE,
            eventTime(), static_cast<WPEModifiers>(0), WPE_BUTTON_PRIMARY, priv->pointerX, priv->pointerY, 0));
        wpe_view_event(view, event.get());
    }
}

static void gamepadKey(WPEDisplayPS5* display, WPEView* view, const WPEDisplayPS5ButtonKey& key, bool down)
{
    if (key.keyval == WPE_KEY_Pointer_Button1)
        pointerButton(display, view, down);
    else
        sendKey(view, key.keyval, key.keycode, static_cast<WPEModifiers>(0), down);
}

// What gamepad mode holds down goes up: the page loses the pad, the view or
// the mapping changes.
static void releaseGamepadInput(WPEDisplayPS5* display, WPEView* view)
{
    auto* priv = display->priv;
    if (view) {
        for (const auto& key : priv->gamepadKeys) {
            if (priv->gamepadKeysDown & key.button)
                gamepadKey(display, view, key, false);
        }
        for (int stick = 0; stick < 2; stick++) {
            const auto* keys = directionKeys(priv->stickActions[stick]);
            for (int d = 0; keys && d < 4; d++) {
                if (priv->stickKeysDown[stick] & (1 << d))
                    sendKey(view, keys[d].keyval, keys[d].keycode, static_cast<WPEModifiers>(0), false);
            }
        }
    }
    priv->gamepadKeysDown = 0;
    priv->stickKeysDown[0] = priv->stickKeysDown[1] = 0;
    priv->optionsDownAt = -1;
}

// Gamepad mode with the page holding the pad: the mapped keys, the sticks,
// and Options held for the application.
static void gamepadModeTick(WPEDisplayPS5* display, WPEView* view, const PS5PadData& data, uint32_t now, uint32_t pressed, uint32_t released, double t)
{
    auto* priv = display->priv;
    if (pressed & PS5_PAD_OPTIONS) {
        priv->optionsDownAt = t;
        priv->optionsHoldSent = false;
    }
    if (!(now & PS5_PAD_OPTIONS))
        priv->optionsDownAt = -1;
    else if (priv->optionsDownAt >= 0 && !priv->optionsHoldSent && t - priv->optionsDownAt >= optionsHoldFor && priv->buttonHandler) {
        priv->optionsHoldSent = true;
        PS5_LOG("pad: Options held in gamepad mode");
        bool wasUI = priv->uiMode, wasCaptured = priv->padCaptured, wasGamepadMode = priv->gamepadMode;
        priv->buttonHandler(display, WPE_DISPLAY_PS5_BUTTON_OPTIONS_HOLD, priv->buttonHandlerData);
        if (priv->uiMode != wasUI || priv->padCaptured != wasCaptured || priv->activeView != view || priv->gamepadMode != wasGamepadMode)
            return;
    }

    uint32_t down = pressed & ~priv->gamepadKeysDown;
    uint32_t up = released & priv->gamepadKeysDown;
    for (const auto& key : priv->gamepadKeys) {
        if (down & key.button) {
            gamepadKey(display, view, key, true);
            priv->gamepadKeysDown |= key.button;
        }
        if (up & key.button)
            gamepadKey(display, view, key, false);
    }
    priv->gamepadKeysDown &= ~up;

    bool pointerVisible = t - priv->pointerMovedAt < pointerShownFor;
    bool pointerStick = false;
    for (int stick = 0; stick < 2; stick++) {
        uint8_t rawX = data.sticks[stick * 2], rawY = data.sticks[stick * 2 + 1];
        auto action = priv->stickActions[stick];
        if (const auto* keys = directionKeys(action)) {
            // Past half way: the key goes down; back under 0.3: up.
            double x = (static_cast<int>(rawX) - 128) / 127.0, y = (static_cast<int>(rawY) - 128) / 127.0;
            const double amount[4] = { -y, y, -x, x };
            for (int d = 0; d < 4; d++) {
                bool isDown = priv->stickKeysDown[stick] & (1 << d);
                bool wanted = amount[d] > (isDown ? 0.3 : 0.5);
                if (wanted != isDown) {
                    sendKey(view, keys[d].keyval, keys[d].keycode, static_cast<WPEModifiers>(0), wanted);
                    priv->stickKeysDown[stick] ^= 1 << d;
                }
            }
        } else if (action == WPE_DISPLAY_PS5_STICK_POINTER) {
            pointerStick = true;
            pointerVisible = stickPointer(display, view, stickValue(rawX), stickValue(rawY), t);
        } else if (action == WPE_DISPLAY_PS5_STICK_SCROLL)
            stickScroll(display, view, stickValue(rawX), stickValue(rawY), pointerVisible);
    }
    if (!pointerStick)
        stickPointer(display, view, 0, 0, t); // only hides it when its time is up
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
// that long into each hold and pause, after any snap: pad-NNN-rest.ppm),
// upry=<0-1> (the stick in the "up" half; default ry; 0: still, so the cycle
// only goes down, for infinite feeds).
struct PadScrollTest {
    bool on { false };
    double delay { 15 }, secs { 4 }, ry { 0.5 }, rx { 0 }, pause { 4 };
    double px { 0.5 }, py { 0.5 }, rest { 0 }, hold { 0 }, upry { -1 };
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
        else if (!strcmp(key, "upry"))
            test.upry = value;
        else if (!strcmp(key, "shots"))
            test.shots = static_cast<int>(value);
    }
    PS5_LOG("pad-scroll-test: from %.0f s, %.1f s down / up at rx %.2f ry %.2f (up %.2f) (at %.2f, %.2f of the view), %.1f s hold, %.1f s pause, %d shots per direction, rest shot %.1f s",
        test.delay, test.secs, test.rx, test.ry, test.upry >= 0 ? test.upry : test.ry, test.px, test.py, test.hold, test.pause, test.shots, test.rest);
}

// The test's stick at time t (the values after padTick's curve); false
// while it is still. Also read by the pad's stall thread (padStallThread).
static bool padScrollTestStick(double t, double* rx, double* ry)
{
    auto& test = s_padScrollTest;
    double cycle = 2 * test.secs + test.hold + test.pause;
    double at = t - test.start;
    if (!test.on || !test.start || at < 0 || cycle <= 0)
        return false;
    double phase = std::fmod(at, cycle);
    if ((phase >= test.secs && phase < test.secs + test.hold) || phase >= 2 * test.secs + test.hold)
        return false;
    bool down = phase < test.secs;
    double base = test.ry > 0 ? test.ry : test.rx; // upry scales the up half of either axis
    double upScale = test.upry >= 0 && base > 0 ? test.upry / base : 1;
    *ry = down ? test.ry : -test.ry * upScale;
    *rx = down ? test.rx : -test.rx * upScale;
    return *rx || *ry;
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
    double rx, ry;
    if (!padScrollTestStick(t, &rx, &ry))
        return; // still in the up half (upry=0)
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

// The right stick while the main thread is stuck. Pages in the title run
// their scripts, style and layout on the title's main thread, which also
// runs padTick: while a page keeps it busy (a feed adding posts, 0.3-1.2 s
// at a time) no stick event reached the scrolling tree and the page stopped
// dead, where a page in a payload kept scrolling into what was already
// painted. This thread wakes every pad interval; once padTick has not run
// for 50 ms and its last run was on a page with the stick scrolling it, it
// reads the pad itself and hands the stick's delta, as padTick's stickScroll
// would send it, to the page's scrolling tree on the render thread
// (ps5wk_title_stall_scroll, WebKit: only while the stick was scrolling that
// page frame-locked within the last second; clamped at the painted edge
// there). Nothing else of the pad is read here; padTick goes on as before
// when the main thread is back. One log line per stall it scrolled in.
extern "C" bool ps5wk_title_stall_scroll(double wpeDeltaX, double wpeDeltaY) __attribute__((weak));

static void* padStallThread(void* data)
{
    auto* display = static_cast<WPEDisplayPS5*>(data);
    auto* priv = display->priv;
    double lastSent = 0, stallFrom = 0;
    unsigned events = 0;
    double pixels = 0;
    for (;;) {
        g_usleep(padIntervalMS * 1000);
        double t = nowSeconds();
        double tickAt = priv->padTickAt.load(std::memory_order_relaxed);
        if (t - tickAt < 0.05 || !priv->padTickScrolls.load(std::memory_order_relaxed)) {
            if (events)
                PS5_LOG("pad: the main thread was stuck %.0f ms; the stick scrolled the page from the pad's thread meanwhile (%u steps, %.0f px)",
                    (tickAt - stallFrom) * 1000, events, pixels);
            events = 0;
            pixels = 0;
            lastSent = 0;
            continue;
        }
        if (!events && !lastSent)
            stallFrom = tickAt;
        double rx = 0, ry = 0;
        if (s_padScrollTest.on) {
            if (!padScrollTestStick(t, &rx, &ry))
                rx = ry = 0;
        } else {
            PS5PadData pad { };
            g_mutex_lock(&priv->padReadLock);
            int readResult = priv->pad >= 0 ? scePadReadState(priv->pad, &pad) : -1;
            g_mutex_unlock(&priv->padReadLock);
            if (readResult || (pad.buttons & PS5_PAD_INTERCEPTED))
                continue;
            rx = stickValue(pad.sticks[2]);
            ry = stickValue(pad.sticks[3]);
        }
        double since = lastSent ? t - lastSent : t - tickAt - 0.05 + padIntervalMS / 1000.0;
        lastSent = t;
        if (!rx && !ry)
            continue;
        // padTick sends 14 px per pad interval at full deflection.
        double scale = std::clamp(since / (padIntervalMS / 1000.0), 0.0, 6.0);
        double dx = -rx * 14.0 * scale, dy = -ry * 14.0 * scale;
        if (ps5wk_title_stall_scroll(dx, dy)) {
            events++;
            pixels += std::hypot(dx, dy) * 2.5;
        }
    }
    return nullptr;
}

static void startPadStallThread(WPEDisplayPS5* display)
{
    if (!ps5wk_title_stall_scroll || display->priv->pad < 0)
        return;
    pthread_t thread;
    if (!pthread_create(&thread, nullptr, padStallThread, display)) {
        pthread_detach(thread);
        PS5_LOG("pad: the stick also scrolls from the pad's own thread while the main thread is stuck");
    }
}

// UI mode: the pointer moves for the application's UI (no page events), the
// right stick scrolls it, and held D-pad/shoulder buttons repeat.
static void uiTick(WPEDisplayPS5* display, const PS5PadData& data, const TouchMove& touch, uint32_t now, uint32_t pressed, double t)
{
    auto* priv = display->priv;
    constexpr uint32_t repeatable = PS5_PAD_UP | PS5_PAD_DOWN | PS5_PAD_LEFT | PS5_PAD_RIGHT | PS5_PAD_L1 | PS5_PAD_R1 | PS5_PAD_L2 | PS5_PAD_R2;
    // The D-pad moves the UI's focus: the pointer goes, so that X then acts
    // on the focus and not on what the pointer was left over (until the
    // stick or the touchpad moves it again).
    constexpr uint32_t dpad = PS5_PAD_UP | PS5_PAD_DOWN | PS5_PAD_LEFT | PS5_PAD_RIGHT;
    if ((pressed & dpad) && t - priv->pointerMovedAt < pointerShownFor) {
        priv->pointerMovedAt = -1000;
        wpeDisplayPS5ScheduleRedraw(display);
    }
    if (pressed & repeatable) {
        uint32_t bits = pressed & repeatable;
        priv->uiRepeatButton = bits & (~bits + 1); // the lowest one
        priv->uiRepeatNext = t + repeatDelay;
    }
    if (priv->uiRepeatButton && !(now & priv->uiRepeatButton))
        priv->uiRepeatButton = 0;
    if (priv->uiRepeatButton && t >= priv->uiRepeatNext && priv->buttonHandler) {
        priv->uiRepeatNext = t + repeatInterval * 1.5;
        uint32_t button = priv->uiRepeatButton;
        priv->buttonHandler(display, button, priv->buttonHandlerData);
        if (!priv->uiMode)
            return;
    }

    int width = PS5Renderer::singleton().width(), height = PS5Renderer::singleton().height();
    if (priv->activeView && !priv->uiMode) { // the UI's pointer: the whole screen
        width = wpe_view_get_width(priv->activeView);
        height = wpe_view_get_height(priv->activeView);
    }
    bool pointerVisible = t - priv->pointerMovedAt < pointerShownFor;
    const double speed = 22.0;
    double dx = stickValue(data.sticks[0]) * speed, dy = stickValue(data.sticks[1]) * speed;
    if (touch.fingers == 1 && (touch.dx || touch.dy)) {
        double gain = touchGain(touch, width);
        dx += touch.dx * gain;
        dy += touch.dy * gain;
    }
    if (dx || dy) {
        if (!pointerVisible && priv->pointerX == 0 && priv->pointerY == 0) {
            priv->pointerX = width / 2.0;
            priv->pointerY = height / 2.0;
        }
        priv->pointerX = std::fmin(std::fmax(priv->pointerX + dx, 0), width - 1);
        priv->pointerY = std::fmin(std::fmax(priv->pointerY + dy, 0), height - 1);
        priv->pointerMovedAt = t;
        wpeDisplayPS5ScheduleRedraw(display);
        if (priv->uiPointerCallback)
            priv->uiPointerCallback(display, priv->pointerX, priv->pointerY, 0, priv->uiPointerCallbackData);
        if (!priv->uiMode)
            return;
    } else if (priv->pointerWasVisible && !pointerVisible)
        wpeDisplayPS5ScheduleRedraw(display);

    double scroll = stickValue(data.sticks[3]) * 14.0;
    if (touch.fingers == 2)
        scroll -= touch.dy * 0.5 * width / touchpadWidth; // the list follows the fingers
    if (scroll && priv->uiPointerCallback)
        priv->uiPointerCallback(display, priv->pointerX, priv->pointerY, scroll, priv->uiPointerCallbackData);
}

// The system's hold on the pad: logged when it changes (a read failing, the
// intercepted bit, connected), so a log shows whether presses could reach the
// title at all. While View in VR has the pad, the system holding it for
// kSystemHeldEndsVR seconds with no press getting through ends VR: on
// 2026-11-02 the title entered VR mode with the PS VR2 unplugged and no
// O/touchpad press ever reached it (nothing was logged in 466 s). The
// intercepted bit alone does not count while the headset is tracking: in VR
// mode the system sets it for the whole session (2026-11-04: from 0.1 s
// after entry until the end) while presses still reach the title, so a
// viewer who just watched for 10 s was sent back to the TV.
static void padSystemState(WPEDisplayPS5* display, int readResult, const PS5PadData& data)
{
    auto* priv = display->priv;
    const bool intercepted = !readResult && (data.buttons & PS5_PAD_INTERCEPTED);
    const int connected = readResult ? priv->padConnected : data.connected;
    if (readResult != priv->padReadResult || intercepted != priv->padIntercepted || connected != priv->padConnected) {
        if (priv->padStateLogs < 40 || !(priv->padStateLogs % 100))
            PS5_LOG("pad: read 0x%x, %s, connected %d%s (raw buttons 0x%x)", static_cast<unsigned>(readResult),
                intercepted ? "intercepted by the system" : "not intercepted", connected, priv->padCaptured ? "; View in VR has the pad" : "",
                readResult ? 0 : data.buttons);
        priv->padStateLogs++;
        priv->padReadResult = readResult;
        priv->padIntercepted = intercepted;
        priv->padConnected = connected;
    }
    const double t = nowSeconds();
    const bool held = readResult || (intercepted && priv->padCaptured && !PS5Renderer::singleton().vrTracking());
    if (!held || !priv->padCaptured) {
        priv->systemHeldSince = -1;
        if (!priv->padCaptured)
            priv->systemHeldEndedVR = false;
        return;
    }
    if (priv->systemHeldSince < 0)
        priv->systemHeldSince = t;
    if (!priv->systemHeldEndedVR && t - priv->systemHeldSince >= kSystemHeldEndsVR && t - priv->capturedPressAt >= kSystemHeldEndsVR) {
        priv->systemHeldEndedVR = true;
        PS5_LOG("pad: the system has held the pad for %.0f s in View in VR (read 0x%x, intercepted %d): back to the TV", t - priv->systemHeldSince,
            static_cast<unsigned>(readResult), intercepted);
        PS5Renderer::singleton().endVR("The system had the controller for 10 s. Back on the TV.");
    }
}

static void padTick(WPEDisplayPS5* display)
{
    auto* priv = display->priv;
    priv->padTickAt.store(nowSeconds(), std::memory_order_relaxed);
    priv->padTickScrolls.store(false, std::memory_order_relaxed);
    pollTextDialog(display);
    padScrollTestTick(display);

    WPEView* view = priv->activeView;
    if (priv->pad < 0 || !view)
        return;

    PS5PadData data { };
    g_mutex_lock(&priv->padReadLock);
    int readResult = scePadReadState(priv->pad, &data);
    g_mutex_unlock(&priv->padReadLock);
    padSystemState(display, readResult, data);
    if (readResult)
        return;
    data.buttons &= ~PS5_PAD_INTERCEPTED;
    const TouchMove touch = readTouch(display, data, nowSeconds());

    // While the on-screen keyboard is up, the pad belongs to it.
    if (priv->pendingText) {
        if (priv->gamepad)
            wpeGamepadPS5SetState(priv->gamepad, 0, nullptr);
        if (priv->gamepadMode) {
            releaseGamepadInput(display, view);
            priv->buttons = 0; // nothing below was sent in gamepad mode
        }
        // End what the page saw go down: X on a field opens the keyboard
        // while X is still held (the field takes focus on the press), so
        // its release never reached the page, which then kept the button
        // down. The next stick move was a drag that selected text in the
        // field and focused it again (a site's suggestion list came back
        // after a search). Held keys get their key-up too; buttons still
        // held when the keyboard closes stay ignored until released.
        if (priv->buttons || priv->pointerDown) {
            uint32_t held = priv->buttons & ~priv->swallowedButtons;
            if (priv->pointerDown) {
                priv->pointerDown = false;
                GRefPtr<WPEEvent> event = adoptGRef(wpe_event_pointer_button_new(WPE_EVENT_POINTER_UP, view, WPE_INPUT_SOURCE_MOUSE,
                    eventTime(), static_cast<WPEModifiers>(0), WPE_BUTTON_PRIMARY, priv->pointerX, priv->pointerY, 0));
                wpe_view_event(view, event.get());
            }
            if (priv->returnDown) {
                sendKey(view, WPE_KEY_Return, 36, static_cast<WPEModifiers>(0), false);
                priv->returnDown = false;
            }
            for (const auto& mapping : keyMappings) {
                if ((held & mapping.button) && !(mapping.button & (PS5_PAD_L3 | PS5_PAD_R3))) // L3/R3 send at release
                    sendKey(view, mapping.keyval, mapping.keycode, mapping.modifiers, false);
            }
            PS5_LOG("pad: keyboard open, released what the page saw held (buttons 0x%x)", held);
        }
        priv->buttons = 0;
        priv->swallowedButtons = 0;
        priv->stickChord = false;
        priv->ignoredButtons = data.buttons;
        priv->repeatingKey = 0;
        priv->triangleDownAt = -1;
        priv->crossLinkDownAt = -1;
        return;
    }

    uint32_t now = data.buttons;
    priv->ignoredButtons &= now;
    now &= ~priv->ignoredButtons;
    uint32_t pressed = now & ~priv->buttons;
    uint32_t released = priv->buttons & ~now;
    priv->buttons = now;
    const double t = nowSeconds();
    if (pressed) {
        priv->lastPress = t;
        priv->lastPressKind = _WPEDisplayPS5Private::PressKind::Other;
    }

    // The application first (View in VR); what it takes, the page never sees.
    if (uint32_t swallowedReleases = released & priv->swallowedButtons) {
        released &= ~swallowedReleases;
        priv->swallowedButtons &= ~swallowedReleases;
    }
    // In gamepad mode the page gets every press (Options held is checked in
    // gamepadModeTick); the application's UI and View in VR still get them.
    if (pressed && priv->buttonHandler && (!priv->gamepadMode || priv->uiMode || priv->padCaptured)) {
        bool wasCaptured = priv->padCaptured;
        if (wasCaptured)
            priv->capturedPressAt = t;
        bool wasUI = priv->uiMode;
        bool taken = priv->buttonHandler(display, pressed, priv->buttonHandlerData);
        if (priv->padCaptured != wasCaptured || priv->uiMode != wasUI || priv->activeView != view)
            return; // the handler changed the pad's owner (held buttons are ignored until released)
        if (taken) {
            priv->swallowedButtons |= pressed;
            pressed = 0;
        }
    }
    // The Gamepad API sees the pad while the page has it (not what the
    // application took), and everything released while it does not.
    const bool pageHasPad = !priv->padCaptured && !priv->uiMode;
    if (priv->gamepad)
        wpeGamepadPS5SetState(priv->gamepad, pageHasPad ? now & ~priv->swallowedButtons : 0, pageHasPad ? &data : nullptr);
    if (!pageHasPad && (priv->gamepadKeysDown || priv->stickKeysDown[0] || priv->stickKeysDown[1]))
        releaseGamepadInput(display, view);

    if (!pageHasPad || priv->gamepadMode) {
        priv->triangleDownAt = -1;
        priv->crossLinkDownAt = -1;
    }

    if (priv->padCaptured) {
        priv->repeatingKey = 0;
        if (priv->capturedPadCallback)
            priv->capturedPadCallback(display, now, stickValue(data.sticks[2]), stickValue(data.sticks[3]), priv->capturedPadCallbackData);
        return;
    }
    if (priv->uiMode) {
        uiTick(display, data, touch, now, pressed, t);
        return;
    }
    if (priv->gamepadMode) {
        gamepadModeTick(display, view, data, now, pressed, released, t);
        return;
    }

    // Triangle held on the page: the application's (gamepad mode on). Its
    // press still went to the page (the keyboard for a focused field).
    if (pressed & PS5_PAD_TRIANGLE) {
        priv->triangleDownAt = t;
        priv->triangleHoldSent = false;
    }
    if (!(now & PS5_PAD_TRIANGLE))
        priv->triangleDownAt = -1;
    else if (priv->triangleDownAt >= 0 && !priv->triangleHoldSent && t - priv->triangleDownAt >= triangleHoldFor && priv->buttonHandler) {
        priv->triangleHoldSent = true;
        PS5_LOG("pad: Triangle held on the page");
        bool wasUI = priv->uiMode, wasCaptured = priv->padCaptured, wasGamepadMode = priv->gamepadMode;
        priv->buttonHandler(display, WPE_DISPLAY_PS5_BUTTON_TRIANGLE_HOLD, priv->buttonHandlerData);
        if (priv->uiMode != wasUI || priv->padCaptured != wasCaptured || priv->activeView != view || priv->gamepadMode != wasGamepadMode)
            return;
    }

    // Left stick and the touchpad: pointer. Right stick and two fingers on
    // the touchpad: scroll under the pointer (or the centre).
    bool pointerVisible = stickPointer(display, view, stickValue(data.sticks[0]), stickValue(data.sticks[1]), t);
    if (touchPointer(display, view, touch, t))
        pointerVisible = true;
    stickScroll(display, view, stickValue(data.sticks[2]), stickValue(data.sticks[3]), pointerVisible);
    priv->padTickScrolls.store(!priv->pendingText, std::memory_order_relaxed);

    // Cross: click when the pointer is shown, otherwise Return. The
    // touchpad's press: always a click at the pointer. Whichever went down
    // first holds the click; the other is ignored until it comes back up.
    constexpr uint32_t clickButtons = PS5_PAD_CROSS | PS5_PAD_TOUCHPAD;
    // Cross waiting over a link (below) no longer held and not released
    // here: the pad changed hands meanwhile, and the press is nobody's.
    if (priv->crossLinkDownAt >= 0 && !((now | released) & PS5_PAD_CROSS))
        priv->crossLinkDownAt = -1;
    if (uint32_t extra = (priv->pointerDown || priv->crossLinkDownAt >= 0) ? pressed & clickButtons : 0) {
        priv->ignoredButtons |= extra;
        priv->buttons &= ~extra;
        pressed &= ~extra;
    }
    // Over the application's bar the button handler had the click; whatever
    // it left is not the page's. The touchpad's press clicks at the pointer
    // even when it has timed out, so over the bar it is never the page's.
    if (priv->pointerY < 0)
        pressed &= pointerVisible ? ~clickButtons : ~static_cast<uint32_t>(PS5_PAD_TOUCHPAD);
    if (pressed & PS5_PAD_TOUCHPAD)
        pointerClickDown(display, view, PS5_PAD_TOUCHPAD, t);
    else if (pressed & PS5_PAD_CROSS) {
        if (pointerVisible && priv->pointerOverLink && priv->buttonHandler) {
            // Over a link: the click waits for Cross to come back up, so
            // that held it can be the application's (the link in a new
            // tab). A move to where the pointer is, as before a click: the
            // application hears what is under it now.
            priv->crossLinkDownAt = t;
            priv->pointerMovedAt = t;
            GRefPtr<WPEEvent> move = adoptGRef(wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, view, WPE_INPUT_SOURCE_MOUSE,
                eventTime(), static_cast<WPEModifiers>(0), priv->pointerX, priv->pointerY, 0, 0));
            wpe_view_event(view, move.get());
        } else if (pointerVisible)
            pointerClickDown(display, view, PS5_PAD_CROSS, t);
        else {
            sendKey(view, WPE_KEY_Return, 36, static_cast<WPEModifiers>(0), true);
            priv->returnDown = true;
        }
    }
    if (priv->crossLinkDownAt >= 0) {
        if (released & PS5_PAD_CROSS) {
            // A short press: the click, whole (its release follows below).
            priv->crossLinkDownAt = -1;
            pointerClickDown(display, view, PS5_PAD_CROSS, t);
        } else if (!priv->pointerOverLink) {
            // The pointer left the link, or it was none after all: an
            // ordinary press from here on (it can drag).
            priv->crossLinkDownAt = -1;
            pointerClickDown(display, view, PS5_PAD_CROSS, t);
        } else if (t - priv->crossLinkDownAt >= crossHoldFor) {
            priv->crossLinkDownAt = -1;
            PS5_LOG("pad: Cross held over a link");
            bool wasUI = priv->uiMode, wasCaptured = priv->padCaptured, wasGamepadMode = priv->gamepadMode;
            bool taken = priv->buttonHandler && priv->buttonHandler(display, WPE_DISPLAY_PS5_BUTTON_CROSS_HOLD, priv->buttonHandlerData);
            if (priv->uiMode != wasUI || priv->padCaptured != wasCaptured || priv->activeView != view || priv->gamepadMode != wasGamepadMode)
                return;
            // Taken: the page never hears of this press (Cross's release
            // finds nothing down). Not taken: the click after all.
            if (!taken)
                pointerClickDown(display, view, PS5_PAD_CROSS, t);
        }
    }
    if (priv->pointerDown && (released & priv->pointerDownBy)) {
        priv->pointerDown = false;
        GRefPtr<WPEEvent> event = adoptGRef(wpe_event_pointer_button_new(WPE_EVENT_POINTER_UP, view, WPE_INPUT_SOURCE_MOUSE,
            eventTime(), static_cast<WPEModifiers>(0), WPE_BUTTON_PRIMARY, priv->pointerX, priv->pointerY, 0));
        wpe_view_event(view, event.get());
        openKeyboardIfFocusStayed(display, priv->lastPress);
    }
    if ((released & PS5_PAD_CROSS) && priv->returnDown) {
        sendKey(view, WPE_KEY_Return, 36, static_cast<WPEModifiers>(0), false);
        priv->returnDown = false;
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

    constexpr uint32_t focusMoveButtons = PS5_PAD_UP | PS5_PAD_DOWN | PS5_PAD_LEFT | PS5_PAD_RIGHT | PS5_PAD_L1 | PS5_PAD_R1;
    if ((pressed & focusMoveButtons) && priv->lastPressKind == _WPEDisplayPS5Private::PressKind::Other)
        priv->lastPressKind = _WPEDisplayPS5Private::PressKind::FocusMove;

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
    startPadStallThread(displayPS5);
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

static WPEGamepadManager* wpeDisplayPS5CreateGamepadManager(WPEDisplay* display)
{
    return wpeGamepadManagerPS5New(WPE_DISPLAY_PS5(display));
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
    displayClass->create_gamepad_manager = wpeDisplayPS5CreateGamepadManager;
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

void wpe_display_ps5_set_captures_blocked(WPEDisplayPS5* display, gboolean blocked)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    PS5Renderer::singleton().setCapturesBlocked(blocked);
}

// ---------------------------------------------------------------- the pad for the application, View in VR

void wpe_display_ps5_set_button_handler(WPEDisplayPS5* display, WPEDisplayPS5ButtonHandler handler, gpointer userData)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    display->priv->buttonHandler = handler;
    display->priv->buttonHandlerData = userData;
}

void wpe_display_ps5_set_captured_pad_callback(WPEDisplayPS5* display, WPEDisplayPS5CapturedPadCallback callback, gpointer userData)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    display->priv->capturedPadCallback = callback;
    display->priv->capturedPadCallbackData = userData;
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

void wpe_display_ps5_set_ui_mode(WPEDisplayPS5* display, gboolean enabled)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    auto* priv = display->priv;
    if (priv->uiMode == !!enabled)
        return;
    priv->uiMode = enabled;
    // The pointer stays where it is on the screen: the UI's coordinates are
    // the screen's, the page's start below the application's bar.
    if (int top = PS5Renderer::singleton().pageTop(); top && (priv->pointerX || priv->pointerY)) {
        priv->pointerY += enabled ? top : -top;
        if (!enabled && priv->activeView)
            priv->pointerY = std::fmin(std::fmax(priv->pointerY, -top), wpe_view_get_height(priv->activeView) - 1);
    }
    priv->ignoredButtons |= priv->buttons;
    priv->buttons = 0;
    priv->swallowedButtons = 0;
    priv->repeatingKey = 0;
    priv->uiRepeatButton = 0;
    if (priv->pointerDown && priv->activeView) {
        priv->pointerDown = false;
        GRefPtr<WPEEvent> event = adoptGRef(wpe_event_pointer_button_new(WPE_EVENT_POINTER_UP, priv->activeView, WPE_INPUT_SOURCE_MOUSE,
            eventTime(), static_cast<WPEModifiers>(0), WPE_BUTTON_PRIMARY, priv->pointerX, priv->pointerY, 0));
        wpe_view_event(priv->activeView, event.get());
    }
    wpeDisplayPS5ScheduleRedraw(display);
    PS5_LOG("pad %s", enabled ? "to the application's UI" : "back to the page");
}

void wpe_display_ps5_set_active_view(WPEDisplayPS5* display, WPEView* view)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    g_return_if_fail(WPE_IS_VIEW_PS5(view));
    auto* priv = display->priv;
    priv->choosesActiveView = true;
    WPEView* old = priv->activeView;
    if (old == view)
        return;
    // The old page sees the end of what it was pressed with; held buttons
    // are ignored until released.
    releaseGamepadInput(display, old);
    if (priv->pointerDown && old) {
        priv->pointerDown = false;
        GRefPtr<WPEEvent> event = adoptGRef(wpe_event_pointer_button_new(WPE_EVENT_POINTER_UP, old, WPE_INPUT_SOURCE_MOUSE,
            eventTime(), static_cast<WPEModifiers>(0), WPE_BUTTON_PRIMARY, priv->pointerX, priv->pointerY, 0));
        wpe_view_event(old, event.get());
    }
    priv->ignoredButtons |= priv->buttons;
    priv->buttons = 0;
    priv->swallowedButtons = 0;
    priv->repeatingKey = 0;
    priv->activeView = view;
    if (old)
        wpeViewPS5SetActive(old, false);
    wpeViewPS5SetActive(view, true);
    wpeDisplayPS5ScheduleRedraw(display);
    PS5_LOG("active view %p (was %p)", view, old);
}

// ---------------------------------------------------------------- gamepad mode, the Gamepad API

static const char* stickActionName(WPEDisplayPS5StickAction action)
{
    switch (action) {
    case WPE_DISPLAY_PS5_STICK_GAMEPAD: return "gamepad";
    case WPE_DISPLAY_PS5_STICK_ARROWS: return "arrows";
    case WPE_DISPLAY_PS5_STICK_WASD: return "wasd";
    case WPE_DISPLAY_PS5_STICK_POINTER: return "pointer";
    case WPE_DISPLAY_PS5_STICK_SCROLL: return "scroll";
    }
    return "?";
}

void wpe_display_ps5_set_gamepad_mode(WPEDisplayPS5* display, gboolean enabled, const WPEDisplayPS5ButtonKey* keys, guint keyCount,
    WPEDisplayPS5StickAction leftStick, WPEDisplayPS5StickAction rightStick)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    g_return_if_fail(keys || !keyCount);
    auto* priv = display->priv;
    releaseGamepadInput(display, priv->activeView);
    if (priv->pointerDown && priv->activeView)
        pointerButton(display, priv->activeView, false);
    bool changed = priv->gamepadMode != !!enabled;
    priv->gamepadMode = enabled;
    priv->gamepadKeys.assign(keys, keys + keyCount);
    priv->stickActions[0] = leftStick;
    priv->stickActions[1] = rightStick;
    if (changed) {
        // Neither side sees half a press.
        priv->ignoredButtons |= priv->buttons;
        priv->buttons = 0;
        priv->swallowedButtons = 0;
        priv->repeatingKey = 0;
        priv->stickChord = false;
    }
    PS5_LOG("gamepad mode %s: %u key(s), left stick %s, right stick %s", enabled ? "on" : "off", keyCount,
        stickActionName(leftStick), stickActionName(rightStick));
}

gboolean wpe_display_ps5_get_gamepad_mode(WPEDisplayPS5* display)
{
    g_return_val_if_fail(WPE_IS_DISPLAY_PS5(display), FALSE);
    return display->priv->gamepadMode;
}

void wpe_display_ps5_set_gamepad_callback(WPEDisplayPS5* display, WPEDisplayPS5GamepadCallback callback, gpointer userData)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    display->priv->gamepadCallback = callback;
    display->priv->gamepadCallbackData = userData;
}

gboolean wpe_display_ps5_gamepad_in_use(WPEDisplayPS5* display)
{
    g_return_val_if_fail(WPE_IS_DISPLAY_PS5(display), FALSE);
    return display->priv->gamepad && display->priv->gamepadMonitoring;
}

void wpeDisplayPS5GamepadCreated(WPEDisplayPS5* display, WPEGamepad* gamepad)
{
    display->priv->gamepad = gamepad;
}

void wpeDisplayPS5GamepadMonitoring(WPEDisplayPS5* display, bool monitoring)
{
    auto* priv = display->priv;
    if (priv->gamepadMonitoring == monitoring)
        return;
    priv->gamepadMonitoring = monitoring;
    PS5_LOG("gamepad: %s", monitoring ? "the page reads the gamepad" : "no page reads the gamepad");
    if (priv->gamepadCallback)
        priv->gamepadCallback(display, monitoring, priv->gamepadCallbackData);
}

void wpeDisplayPS5GamepadGone(WPEDisplayPS5* display, WPEGamepad* gamepad)
{
    auto* priv = display->priv;
    if (priv->gamepad != gamepad)
        return;
    wpeDisplayPS5GamepadMonitoring(display, false);
    priv->gamepad = nullptr;
    PS5_LOG("gamepad: no page uses gamepads now");
}

bool wpeDisplayPS5Rumble(WPEDisplayPS5* display, double strong, double weak)
{
    auto* priv = display->priv;
    if (priv->pad < 0 || !scePadSetVibration)
        return false;
    if (!priv->vibrationModeSet && scePadSetVibrationMode) {
        priv->vibrationModeSet = true;
        int rc = scePadSetVibrationMode(priv->pad, 2);
        PS5_LOG("gamepad: vibration mode compatible -> 0x%x", static_cast<unsigned>(rc));
    }
    const uint8_t motors[2] = {
        static_cast<uint8_t>(std::lround(std::fmin(1.0, std::fmax(0.0, strong)) * 255)),
        static_cast<uint8_t>(std::lround(std::fmin(1.0, std::fmax(0.0, weak)) * 255)),
    };
    int rc = scePadSetVibration(priv->pad, motors);
    if (rc || priv->rumbleLogs < 10) {
        priv->rumbleLogs++;
        PS5_LOG("gamepad: rumble %u/%u -> 0x%x", motors[0], motors[1], static_cast<unsigned>(rc));
    }
    return !rc;
}

void wpe_display_ps5_set_page_top(WPEDisplayPS5* display, int pixels)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    auto* priv = display->priv;
    auto& renderer = PS5Renderer::singleton();
    int before = renderer.pageTop();
    renderer.setPageTop(pixels);
    if (renderer.pageTop() == before)
        return;
    PS5_LOG("page area: %d px below the top (was %d)", renderer.pageTop(), before);
    wpeViewPS5ResizeAll();
    if (!priv->uiMode && priv->activeView && (priv->pointerX || priv->pointerY))
        priv->pointerY = std::fmin(std::fmax(priv->pointerY, -renderer.pageTop()), wpe_view_get_height(priv->activeView) - 1);
    wpeDisplayPS5ScheduleRedraw(display);
}

int wpe_display_ps5_get_page_top(WPEDisplayPS5* display)
{
    g_return_val_if_fail(WPE_IS_DISPLAY_PS5(display), 0);
    return PS5Renderer::singleton().pageTop();
}

gboolean wpe_display_ps5_get_ui_mode(WPEDisplayPS5* display)
{
    g_return_val_if_fail(WPE_IS_DISPLAY_PS5(display), FALSE);
    return display->priv->uiMode;
}

gboolean wpe_display_ps5_get_pointer(WPEDisplayPS5* display, double* x, double* y)
{
    g_return_val_if_fail(WPE_IS_DISPLAY_PS5(display), FALSE);
    auto* priv = display->priv;
    if (x)
        *x = priv->pointerX;
    if (y)
        *y = priv->pointerY;
    return nowSeconds() - priv->pointerMovedAt < pointerShownFor;
}

void wpe_display_ps5_set_ui_pointer_callback(WPEDisplayPS5* display, WPEDisplayPS5UIPointerCallback callback, gpointer userData)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    display->priv->uiPointerCallback = callback;
    display->priv->uiPointerCallbackData = userData;
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

void wpe_display_ps5_set_pointer_over_editable(WPEDisplayPS5* display, gboolean editable)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    display->priv->pointerTargetReported = true;
    display->priv->pointerOverEditable = editable;
}

void wpe_display_ps5_set_pointer_over_link(WPEDisplayPS5* display, gboolean link)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    display->priv->pointerOverLink = link;
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
