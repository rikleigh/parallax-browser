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

#include "PS5Sce.h"
#include "WPEPS5Private.h"
#include <wtf/glib/GUniquePtr.h>
#include <wtf/glib/WTFGType.h>
#include <string>

// Text input through the system keyboard (libSceImeDialog). WebKit focuses
// this context when an editable element gets focus. The keyboard opens:
//  - right away if the focus followed a click with the pointer over an
//    editable node (X on a field) or a D-pad / L1 / R1 press that moved the
//    focus; not when a page focuses a field by itself, on load or in a
//    button's click handler (a search button that refocuses its field);
//  - on Triangle while a field is focused;
//  - on X over the field that already has focus (no focus change follows;
//    say a form's validation focused it after Go).
// It capitalizes automatically only where the page asks (autocapitalize) or
// in a multi-line field.
// The dialog starts with the field's text around the cursor; on "OK" that
// text is replaced (delete-surrounding + committed), and in a single-line
// field Return follows, as a phone keyboard's Go key submits the form (the
// key events reach the WebProcess after the committed text, on the same
// connection, so the page sees the new value first). The dialog's OK button
// then says Search in a search field and Go in other single-line fields; a
// textarea or contenteditable element (WPE_INPUT_HINT_MULTILINE) keeps the
// default label and gets no Return. Cancel changes nothing.

G_DECLARE_FINAL_TYPE(WPEInputMethodContextPS5, wpe_input_method_context_ps5, WPE, INPUT_METHOD_CONTEXT_PS5, WPEInputMethodContext)
#define WPE_TYPE_INPUT_METHOD_CONTEXT_PS5 (wpe_input_method_context_ps5_get_type())

struct _WPEInputMethodContextPS5Private {
    bool focused { false };
    std::string surrounding;
    guint cursorIndex { 0 };
    bool waiting { false };
    bool enterSubmits { false }; // the open dialog's OK sends Return after the text
};
WEBKIT_DEFINE_FINAL_TYPE(WPEInputMethodContextPS5, wpe_input_method_context_ps5, WPE_TYPE_INPUT_METHOD_CONTEXT, WPEInputMethodContext)

static WPEDisplayPS5* displayFor(WPEInputMethodContext* context)
{
    auto* display = wpe_input_method_context_get_display(context);
    return display && WPE_IS_DISPLAY_PS5(display) ? WPE_DISPLAY_PS5(display) : nullptr;
}

static glong utf16Length(const char* text, gssize bytes)
{
    glong items = 0;
    GUniquePtr<gunichar2> utf16(g_utf8_to_utf16(text, bytes, nullptr, &items, nullptr));
    return utf16 ? items : 0;
}

void wpeInputMethodContextPS5OpenKeyboard(WPEInputMethodContext* context)
{
    auto* priv = WPE_INPUT_METHOD_CONTEXT_PS5(context)->priv;
    auto* display = displayFor(context);
    if (!display || priv->waiting)
        return;

    auto purpose = wpe_input_method_context_get_input_purpose(context);
    bool url = purpose == WPE_INPUT_PURPOSE_URL;
    const char* title = purpose == WPE_INPUT_PURPOSE_PASSWORD ? "Password" : url ? "Address" : purpose == WPE_INPUT_PURPOSE_SEARCH ? "Search" : "Text";
    const char* initial = purpose == WPE_INPUT_PURPOSE_PASSWORD ? "" : priv->surrounding.c_str();
    // Single line: no multiline hint (patch 0071 sets it for textarea and
    // contenteditable) and no new line in the text around the cursor.
    bool multiline = (wpe_input_method_context_get_input_hints(context) & WPE_INPUT_HINT_MULTILINE)
        || priv->surrounding.find('\n') != std::string::npos;
    priv->enterSubmits = !multiline;
    int32_t enterLabel = multiline ? PS5_IME_ENTER_LABEL_DEFAULT : purpose == WPE_INPUT_PURPOSE_SEARCH ? PS5_IME_ENTER_LABEL_SEARCH : PS5_IME_ENTER_LABEL_GO;
    // No automatic capitals in form fields (user names, passwords, e-mail
    // addresses, searches): only where the page asks for them (an
    // autocapitalize attribute: WebKit's uppercase hints), or in a textarea
    // or contenteditable element that does not ask for lowercase.
    auto hints = wpe_input_method_context_get_input_hints(context);
    constexpr auto uppercase = static_cast<WPEInputHints>(WPE_INPUT_HINT_UPPERCASE_CHARS | WPE_INPUT_HINT_UPPERCASE_WORDS | WPE_INPUT_HINT_UPPERCASE_SENTENCES);
    bool autoCapitalize = !url && purpose != WPE_INPUT_PURPOSE_PASSWORD
        && ((hints & uppercase) || (multiline && !(hints & WPE_INPUT_HINT_LOWERCASE)));

    g_object_ref(context);
    priv->waiting = wpeDisplayPS5RequestText(display, title, initial, url, enterLabel, autoCapitalize, [](const char* text, gpointer userData) {
        auto* context = WPE_INPUT_METHOD_CONTEXT(userData);
        auto* priv = WPE_INPUT_METHOD_CONTEXT_PS5(context)->priv;
        priv->waiting = false;
        if (text && priv->focused) {
            bool replace = wpe_input_method_context_get_input_purpose(context) != WPE_INPUT_PURPOSE_PASSWORD;
            if (replace && priv->surrounding.length()) {
                // Replace the whole surrounding text with the dialog's result.
                glong before = utf16Length(priv->surrounding.c_str(), priv->cursorIndex);
                glong total = utf16Length(priv->surrounding.c_str(), priv->surrounding.length());
                g_signal_emit_by_name(context, "delete-surrounding", static_cast<int>(-before), static_cast<guint>(total));
            }
            g_signal_emit_by_name(context, "committed", text);
            PS5_LOG("IME: committed %zu bytes%s", strlen(text), priv->enterSubmits ? ", then Return" : "");
            if (priv->enterSubmits) {
                if (auto* display = displayFor(context))
                    wpeDisplayPS5SendReturn(display);
            }
        }
        g_object_unref(context);
    }, context);
    if (!priv->waiting)
        g_object_unref(context);
}

static void wpeInputMethodContextPS5Dispose(GObject* object)
{
    auto* context = WPE_INPUT_METHOD_CONTEXT(object);
    if (auto* display = displayFor(context))
        wpeDisplayPS5InputContextDestroyed(display, context);
    G_OBJECT_CLASS(wpe_input_method_context_ps5_parent_class)->dispose(object);
}

static void wpeInputMethodContextPS5GetPreeditString(WPEInputMethodContext*, gchar** text, GList** underlines, guint* cursorOffset)
{
    if (text)
        *text = g_strdup("");
    if (underlines)
        *underlines = nullptr;
    if (cursorOffset)
        *cursorOffset = 0;
}

static gboolean wpeInputMethodContextPS5FilterKeyEvent(WPEInputMethodContext*, WPEEvent*)
{
    return FALSE;
}

static void wpeInputMethodContextPS5FocusIn(WPEInputMethodContext* context)
{
    auto* priv = WPE_INPUT_METHOD_CONTEXT_PS5(context)->priv;
    priv->focused = true;
    auto* display = displayFor(context);
    if (!display)
        return;
    wpeDisplayPS5SetFocusedInputContext(display, context);
    const char* why = nullptr;
    bool open = wpeDisplayPS5FocusFollowsPressOnField(display, &why);
    if (why)
        PS5_LOG("IME: field focused after a %s: keyboard %s", why, open ? "opens" : "stays closed (Triangle opens it)");
    // Defer: WebKit sends the surrounding text right after focus.
    if (open) {
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, [](gpointer userData) -> gboolean {
            auto* context = WPE_INPUT_METHOD_CONTEXT(userData);
            if (WPE_INPUT_METHOD_CONTEXT_PS5(context)->priv->focused)
                wpeInputMethodContextPS5OpenKeyboard(context);
            return G_SOURCE_REMOVE;
        }, g_object_ref(context), g_object_unref);
    }
}

static void wpeInputMethodContextPS5FocusOut(WPEInputMethodContext* context)
{
    auto* priv = WPE_INPUT_METHOD_CONTEXT_PS5(context)->priv;
    priv->focused = false;
    if (auto* display = displayFor(context))
        wpeDisplayPS5InputContextDestroyed(display, context);
}

static void wpeInputMethodContextPS5SetSurrounding(WPEInputMethodContext* context, const gchar* text, guint length, guint cursorIndex, guint)
{
    auto* priv = WPE_INPUT_METHOD_CONTEXT_PS5(context)->priv;
    priv->surrounding.assign(text ? text : "", text ? length : 0);
    priv->cursorIndex = std::min<guint>(cursorIndex, length);
}

static void wpeInputMethodContextPS5Reset(WPEInputMethodContext*)
{
}

static void wpe_input_method_context_ps5_class_init(WPEInputMethodContextPS5Class* klass)
{
    GObjectClass* objectClass = G_OBJECT_CLASS(klass);
    objectClass->dispose = wpeInputMethodContextPS5Dispose;

    WPEInputMethodContextClass* imClass = WPE_INPUT_METHOD_CONTEXT_CLASS(klass);
    imClass->get_preedit_string = wpeInputMethodContextPS5GetPreeditString;
    imClass->filter_key_event = wpeInputMethodContextPS5FilterKeyEvent;
    imClass->focus_in = wpeInputMethodContextPS5FocusIn;
    imClass->focus_out = wpeInputMethodContextPS5FocusOut;
    imClass->set_surrounding = wpeInputMethodContextPS5SetSurrounding;
    imClass->reset = wpeInputMethodContextPS5Reset;
}

WPEInputMethodContext* wpeInputMethodContextPS5New(WPEView* view)
{
    return WPE_INPUT_METHOD_CONTEXT(g_object_new(WPE_TYPE_INPUT_METHOD_CONTEXT_PS5, "view", view, nullptr));
}
