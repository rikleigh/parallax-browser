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

// The view that receives pad input (the last one mapped, or the one the
// application chose: wpe_display_ps5_set_active_view).
void wpeDisplayPS5SetActiveView(WPEDisplayPS5*, WPEView*);
WPEView* wpeDisplayPS5GetActiveView(WPEDisplayPS5*);
// True once the application chooses the active view itself: views that are
// not active stay unmapped.
bool wpeDisplayPS5ChoosesActiveView(WPEDisplayPS5*);
// WPEViewPS5: the view became the active one (map, focus, next frame whole)
// or stopped being it (focus out, unmap).
void wpeViewPS5SetActive(WPEView*, bool active);
// The size every page view has: the screen's width, its height less
// PS5Renderer::pageTop() (the application's bar above the page).
void wpeViewPS5PageSize(int* width, int* height);
// Main thread: the page area changed size; every view (and its toplevel)
// is resized to it.
void wpeViewPS5ResizeAll(void);

// Coalesced redraw (pointer moved, page updated).
void wpeDisplayPS5ScheduleRedraw(WPEDisplayPS5*);

// The focused editable field: the IME context registers itself so that a
// pad press can open the on-screen keyboard for it.
void wpeDisplayPS5SetFocusedInputContext(WPEDisplayPS5*, WPEInputMethodContext*);
void wpeDisplayPS5InputContextDestroyed(WPEDisplayPS5*, WPEInputMethodContext*);

// Whether a field that just got focus should open the keyboard: the focus
// followed (within 1 s) a click with the pointer over an editable node, or a
// D-pad / L1 / R1 press that moved the focus. Focus a page sets by itself
// (autofocus, or a button's click handler focusing a field) does not. @why
// gets a short reason for the log (NULL when no press was recent).
bool wpeDisplayPS5FocusFollowsPressOnField(WPEDisplayPS5*, const char** why);

// The system keyboard with the given enter label (PS5_IME_ENTER_LABEL_*),
// capitalizing automatically or not; wpe_display_ps5_request_text without
// the check.
// The focused field's caret moved (a scroll) while the keyboard is open:
// the page lift follows it.
void wpeDisplayPS5KeyboardFieldMoved(WPEDisplayPS5*, int fieldY, int fieldHeight);
bool wpeDisplayPS5RequestText(WPEDisplayPS5*, const char* title, const char* initialText, bool url, int32_t enterLabel, bool autoCapitalize, bool password, bool noLearning, int fieldY, int fieldHeight, WPEDisplayPS5TextCallback, gpointer userData);

// Return pressed and released on the active view (real key events: keydown,
// keypress, keyup), as the pad's X does when the pointer is hidden.
void wpeDisplayPS5SendReturn(WPEDisplayPS5*);

// IME context (WPEInputMethodContextPS5.cpp)
WPEInputMethodContext* wpeInputMethodContextPS5New(WPEView*);
void wpeInputMethodContextPS5OpenKeyboard(WPEInputMethodContext*);

// Process manager (WPEProcessManagerPS5.cpp)
#include "WPEProcessManagerPS5.h"

// PS5 logging: g_message goes to stderr; the title redirects stderr to the
// TCP log. Use for anything the owner may need to read from the Mac.
#define PS5_LOG(...) g_message("[wpe-ps5] " __VA_ARGS__)
