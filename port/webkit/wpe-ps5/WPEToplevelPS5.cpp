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
#include "WPEToplevelPS5.h"

#include "PS5Renderer.h"
#include "WPEPS5Private.h"
#include <wtf/glib/WTFGType.h>

/**
 * WPEToplevelPS5:
 *
 * The PS5 [class@WPEPlatform.Toplevel]: always fullscreen on the TV, the size
 * of the EGL window surface. Resize requests are ignored.
 */
struct _WPEToplevelPS5Private {
};
WEBKIT_DEFINE_FINAL_TYPE(WPEToplevelPS5, wpe_toplevel_ps5, WPE_TYPE_TOPLEVEL, WPEToplevel)

static void wpeToplevelPS5Constructed(GObject* object)
{
    G_OBJECT_CLASS(wpe_toplevel_ps5_parent_class)->constructed(object);

    auto* toplevel = WPE_TOPLEVEL(object);
    int width = PS5Renderer::singleton().width();
    int height = PS5Renderer::singleton().height();
    wpe_toplevel_resized(toplevel, width ? width : 1920, height ? height : 1080);
    wpe_toplevel_state_changed(toplevel, static_cast<WPEToplevelState>(WPE_TOPLEVEL_STATE_ACTIVE | WPE_TOPLEVEL_STATE_FULLSCREEN));
}

static WPEScreen* wpeToplevelPS5GetScreen(WPEToplevel* toplevel)
{
    auto* display = wpe_toplevel_get_display(toplevel);
    return display ? wpe_display_get_screen(display, 0) : nullptr;
}

static gboolean wpeToplevelPS5Resize(WPEToplevel*, int, int)
{
    return FALSE;
}

static gboolean wpeToplevelPS5SetFullscreen(WPEToplevel*, gboolean fullscreen)
{
    return fullscreen;
}

static void wpe_toplevel_ps5_class_init(WPEToplevelPS5Class* toplevelPS5Class)
{
    GObjectClass* objectClass = G_OBJECT_CLASS(toplevelPS5Class);
    objectClass->constructed = wpeToplevelPS5Constructed;

    WPEToplevelClass* toplevelClass = WPE_TOPLEVEL_CLASS(toplevelPS5Class);
    toplevelClass->get_screen = wpeToplevelPS5GetScreen;
    toplevelClass->resize = wpeToplevelPS5Resize;
    toplevelClass->set_fullscreen = wpeToplevelPS5SetFullscreen;
}
