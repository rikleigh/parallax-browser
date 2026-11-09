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
#include "WPEGamepadPS5.h"

#include "PS5Sce.h"
#include "WPEPS5Private.h"
#include <cmath>
#include <wtf/glib/GRefPtr.h>
#include <wtf/glib/WTFGType.h>

// What pages see as the gamepad's id: Chrome's form for a DualSense, which
// games' controller tables know (button glyphs, the standard layout).
static constexpr const char* kGamepadName = "DualSense Wireless Controller (STANDARD GAMEPAD Vendor: 054c Product: 0ce6)";

// ---------------------------------------------------------------- the device

G_DECLARE_FINAL_TYPE(WPEGamepadPS5, wpe_gamepad_ps5, WPE, GAMEPAD_PS5, WPEGamepad)
#define WPE_TYPE_GAMEPAD_PS5 (wpe_gamepad_ps5_get_type())

struct _WPEGamepadPS5Private {
    WPEDisplayPS5* display { nullptr }; // weak: the display outlives WebKit's manager
    uint32_t held { 0 };
    uint8_t axes[4] { 128, 128, 128, 128 }; // raw, as last sent
    uint8_t triggers[2] { 0, 0 };
    bool rumbling { false };
};
WEBKIT_DEFINE_FINAL_TYPE(WPEGamepadPS5, wpe_gamepad_ps5, WPE_TYPE_GAMEPAD, WPEGamepad)

// The standard mapping's buttons (WPEGamepadButton is in that order). L2/R2
// go as analog values; the touchpad button stands in for Create (Select,
// button 8), which the system keeps for itself.
struct ButtonMapping {
    uint32_t bit;
    WPEGamepadButton button;
};
static const ButtonMapping kButtons[] = {
    { PS5_PAD_CROSS, WPE_GAMEPAD_BUTTON_RIGHT_CLUSTER_BOTTOM },
    { PS5_PAD_CIRCLE, WPE_GAMEPAD_BUTTON_RIGHT_CLUSTER_RIGHT },
    { PS5_PAD_SQUARE, WPE_GAMEPAD_BUTTON_RIGHT_CLUSTER_LEFT },
    { PS5_PAD_TRIANGLE, WPE_GAMEPAD_BUTTON_RIGHT_CLUSTER_TOP },
    { PS5_PAD_L1, WPE_GAMEPAD_BUTTON_LEFT_SHOULDER_FRONT },
    { PS5_PAD_R1, WPE_GAMEPAD_BUTTON_RIGHT_SHOULDER_FRONT },
    { PS5_PAD_TOUCHPAD, WPE_GAMEPAD_BUTTON_CENTER_CLUSTER_LEFT },
    { PS5_PAD_OPTIONS, WPE_GAMEPAD_BUTTON_CENTER_CLUSTER_RIGHT },
    { PS5_PAD_L3, WPE_GAMEPAD_BUTTON_LEFT_THUMB },
    { PS5_PAD_R3, WPE_GAMEPAD_BUTTON_RIGHT_THUMB },
    { PS5_PAD_UP, WPE_GAMEPAD_BUTTON_LEFT_CLUSTER_TOP },
    { PS5_PAD_DOWN, WPE_GAMEPAD_BUTTON_LEFT_CLUSTER_BOTTOM },
    { PS5_PAD_LEFT, WPE_GAMEPAD_BUTTON_LEFT_CLUSTER_LEFT },
    { PS5_PAD_RIGHT, WPE_GAMEPAD_BUTTON_LEFT_CLUSTER_RIGHT },
};

static double axisValue(uint8_t raw)
{
    // 0..255, 128 the centre: -1..1 (the page applies its own dead zone).
    return std::fmax(-1.0, std::fmin(1.0, (static_cast<int>(raw) - 128) / 127.0));
}

void wpeGamepadPS5SetState(WPEGamepad* gamepad, uint32_t held, const PS5PadData* data)
{
    auto* priv = WPE_GAMEPAD_PS5(gamepad)->priv;
    uint32_t changed = held ^ priv->held;
    priv->held = held;
    for (const auto& mapping : kButtons) {
        if (changed & mapping.bit)
            wpe_gamepad_button_event(gamepad, mapping.button, !!(held & mapping.bit));
    }

    // The triggers' analog values (a light touch shows before the button's
    // bit); the bit with no analog reading counts as fully pressed.
    const uint32_t triggerBits[2] = { PS5_PAD_L2, PS5_PAD_R2 };
    const WPEGamepadButton triggerButtons[2] = { WPE_GAMEPAD_BUTTON_LEFT_SHOULDER_BACK, WPE_GAMEPAD_BUTTON_RIGHT_SHOULDER_BACK };
    for (int i = 0; i < 2; i++) {
        uint8_t value = data ? data->analog[i] : 0;
        if ((held & triggerBits[i]) && !value)
            value = 255;
        if (value != priv->triggers[i]) {
            priv->triggers[i] = value;
            wpe_gamepad_button_value_event(gamepad, triggerButtons[i], value / 255.0);
        }
    }

    const WPEGamepadAxis axes[4] = { WPE_GAMEPAD_AXIS_LEFT_X, WPE_GAMEPAD_AXIS_LEFT_Y, WPE_GAMEPAD_AXIS_RIGHT_X, WPE_GAMEPAD_AXIS_RIGHT_Y };
    for (int i = 0; i < 4; i++) {
        uint8_t raw = data ? data->sticks[i] : 128;
        if (raw != priv->axes[i]) {
            priv->axes[i] = raw;
            wpe_gamepad_axis_event(gamepad, axes[i], axisValue(raw));
        }
    }
}

static void wpeGamepadPS5StartInputMonitor(WPEGamepad* gamepad)
{
    if (auto* display = WPE_GAMEPAD_PS5(gamepad)->priv->display)
        wpeDisplayPS5GamepadMonitoring(display, true);
}

static void wpeGamepadPS5StopInputMonitor(WPEGamepad* gamepad)
{
    auto* priv = WPE_GAMEPAD_PS5(gamepad)->priv;
    if (!priv->display)
        return;
    if (priv->rumbling) {
        wpeDisplayPS5Rumble(priv->display, 0, 0);
        priv->rumbling = false;
    }
    wpeDisplayPS5GamepadMonitoring(priv->display, false);
}

static gboolean wpeGamepadPS5HasRumble(WPEGamepad*)
{
    return scePadSetVibration != nullptr;
}

// WebKit times the effect itself (PlatformGamepadWPE stops it with 0, 0 when
// its duration ends), so @duration is not needed here.
static gboolean wpeGamepadPS5Rumble(WPEGamepad* gamepad, gdouble strong, gdouble weak, guint)
{
    auto* priv = WPE_GAMEPAD_PS5(gamepad)->priv;
    if (!priv->display)
        return FALSE;
    priv->rumbling = strong > 0 || weak > 0;
    return wpeDisplayPS5Rumble(priv->display, strong, weak);
}

static void wpeGamepadPS5Dispose(GObject* object)
{
    auto* priv = WPE_GAMEPAD_PS5(object)->priv;
    if (priv->display) {
        if (priv->rumbling)
            wpeDisplayPS5Rumble(priv->display, 0, 0);
        wpeDisplayPS5GamepadGone(priv->display, WPE_GAMEPAD(object));
        priv->display = nullptr;
    }
    G_OBJECT_CLASS(wpe_gamepad_ps5_parent_class)->dispose(object);
}

static void wpe_gamepad_ps5_class_init(WPEGamepadPS5Class* klass)
{
    G_OBJECT_CLASS(klass)->dispose = wpeGamepadPS5Dispose;
    WPEGamepadClass* gamepadClass = WPE_GAMEPAD_CLASS(klass);
    gamepadClass->start_input_monitor = wpeGamepadPS5StartInputMonitor;
    gamepadClass->stop_input_monitor = wpeGamepadPS5StopInputMonitor;
    gamepadClass->has_rumble = wpeGamepadPS5HasRumble;
    gamepadClass->rumble = wpeGamepadPS5Rumble;
}

// ---------------------------------------------------------------- the manager

G_DECLARE_FINAL_TYPE(WPEGamepadManagerPS5, wpe_gamepad_manager_ps5, WPE, GAMEPAD_MANAGER_PS5, WPEGamepadManager)
#define WPE_TYPE_GAMEPAD_MANAGER_PS5 (wpe_gamepad_manager_ps5_get_type())

struct _WPEGamepadManagerPS5Private {
    GRefPtr<WPEGamepad> gamepad;
};
WEBKIT_DEFINE_FINAL_TYPE(WPEGamepadManagerPS5, wpe_gamepad_manager_ps5, WPE_TYPE_GAMEPAD_MANAGER, WPEGamepadManager)

static void wpeGamepadManagerPS5Dispose(GObject* object)
{
    auto* priv = WPE_GAMEPAD_MANAGER_PS5(object)->priv;
    if (priv->gamepad) {
        // The display stops feeding it now, not when the last reference goes.
        g_object_run_dispose(G_OBJECT(priv->gamepad.get()));
        priv->gamepad = nullptr;
    }
    G_OBJECT_CLASS(wpe_gamepad_manager_ps5_parent_class)->dispose(object);
}

static void wpe_gamepad_manager_ps5_class_init(WPEGamepadManagerPS5Class* klass)
{
    G_OBJECT_CLASS(klass)->dispose = wpeGamepadManagerPS5Dispose;
}

WPEGamepadManager* wpeGamepadManagerPS5New(WPEDisplayPS5* display)
{
    auto* manager = WPE_GAMEPAD_MANAGER_PS5(g_object_new(WPE_TYPE_GAMEPAD_MANAGER_PS5, nullptr));
    auto* gamepad = WPE_GAMEPAD(g_object_new(WPE_TYPE_GAMEPAD_PS5, "name", kGamepadName, nullptr));
    WPE_GAMEPAD_PS5(gamepad)->priv->display = display;
    manager->priv->gamepad = adoptGRef(gamepad);
    wpe_gamepad_manager_add_device(WPE_GAMEPAD_MANAGER(manager), gamepad);
    wpeDisplayPS5GamepadCreated(display, gamepad);
    PS5_LOG("gamepad: a page asked for gamepads (the DualSense is gamepad 0)");
    return WPE_GAMEPAD_MANAGER(manager);
}
