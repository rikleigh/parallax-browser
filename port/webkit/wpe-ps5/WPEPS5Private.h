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

#include "WPEDisplayPS5.h"
#include "WPEToplevelPS5.h"
#include "WPEViewPS5.h"

// Internal glue between the PS5 display, view, input method and process
// manager. Main thread only.

// The view that receives pad input (the last one mapped).
void wpeDisplayPS5SetActiveView(WPEDisplayPS5*, WPEView*);
WPEView* wpeDisplayPS5GetActiveView(WPEDisplayPS5*);

// Coalesced redraw (pointer moved, page updated).
void wpeDisplayPS5ScheduleRedraw(WPEDisplayPS5*);

// The focused editable field: the IME context registers itself so that a
// pad press can open the on-screen keyboard for it.
void wpeDisplayPS5SetFocusedInputContext(WPEDisplayPS5*, WPEInputMethodContext*);
void wpeDisplayPS5InputContextDestroyed(WPEDisplayPS5*, WPEInputMethodContext*);

// Seconds since the last pad press: focus that follows a press opens the
// keyboard, focus a page sets by itself (autofocus) does not.
double wpeDisplayPS5SecondsSinceLastPress(WPEDisplayPS5*);

// IME context (WPEInputMethodContextPS5.cpp)
WPEInputMethodContext* wpeInputMethodContextPS5New(WPEView*);
void wpeInputMethodContextPS5OpenKeyboard(WPEInputMethodContext*);

// Process manager (WPEProcessManagerPS5.cpp)
#include "WPEProcessManagerPS5.h"

// PS5 logging: g_message goes to stderr; the title redirects stderr to the
// TCP log. Use for anything the owner may need to read from the Mac.
#define PS5_LOG(...) g_message("[wpe-ps5] " __VA_ARGS__)
