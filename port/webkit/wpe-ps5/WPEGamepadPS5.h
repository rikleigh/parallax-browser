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
#pragma once

// The Gamepad API on PS5 (README: "Gamepad mode"): the DualSense the title
// reads as a WPEGamepad, standard mapping. WebKit's GamepadProviderWPE makes
// the manager (wpe_display_create_gamepad_manager) when a page first asks
// for gamepads and drops it when none does; the display feeds the device
// from its pad read while the page has the pad. Main thread only.

#include "WPEDisplayPS5.h"
#include <cstdint>

struct PS5PadData;

// The manager, with its one device (the user's DualSense).
WPEGamepadManager* wpeGamepadManagerPS5New(WPEDisplayPS5*);

// The device's state: @held = PS5_PAD_* bits, @data = the pad read (sticks,
// triggers). NULL data with held 0: everything released and centred (the
// page lost the pad). Only changes are sent on.
void wpeGamepadPS5SetState(WPEGamepad*, uint32_t held, const PS5PadData*);

// Display side (WPEDisplayPS5.cpp): the device exists / is gone, WebKit
// started or stopped reading it, and the motors.
void wpeDisplayPS5GamepadCreated(WPEDisplayPS5*, WPEGamepad*);
void wpeDisplayPS5GamepadGone(WPEDisplayPS5*, WPEGamepad*);
void wpeDisplayPS5GamepadMonitoring(WPEDisplayPS5*, bool monitoring);
bool wpeDisplayPS5Rumble(WPEDisplayPS5*, double strong, double weak);
