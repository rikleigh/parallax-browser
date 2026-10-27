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
#include "WPEViewPS5.h"

#include "PS5Renderer.h"
#include "WPEPS5Private.h"
#include <wtf/glib/GRefPtr.h>
#include <wtf/glib/RunLoopSourcePriority.h>
#include <wtf/glib/WTFGType.h>

/**
 * WPEViewPS5:
 *
 * The PS5 [class@WPEPlatform.View]. It takes the WebProcess's shared-memory
 * buffers (pages are painted with Skia on the CPU, FEASIBILITY §4.8), copies
 * the damaged rectangles into a GL texture and presents it on the TV. The
 * buffer is released as soon as it has been copied, so the WebProcess can
 * paint the next frame while this one is on screen.
 */
struct _WPEViewPS5Private {
    GRefPtr<WPEBuffer> renderedBuffer;
    GRefPtr<GSource> frameSource;
    GRefPtr<GSource> focusSource;
    unsigned frames { 0 };
    gint64 firstFrameTime { 0 };
};
WEBKIT_DEFINE_FINAL_TYPE(WPEViewPS5, wpe_view_ps5, WPE_TYPE_VIEW, WPEView)

static void wpeViewPS5Constructed(GObject* object)
{
    G_OBJECT_CLASS(wpe_view_ps5_parent_class)->constructed(object);

    auto* view = WPE_VIEW(object);
    g_signal_connect(view, "notify::toplevel", G_CALLBACK(+[](WPEView* view, GParamSpec*, gpointer) {
        auto* priv = WPE_VIEW_PS5(view)->priv;
        auto* display = WPE_DISPLAY_PS5(wpe_view_get_display(view));
        auto* toplevel = wpe_view_get_toplevel(view);
        if (!toplevel) {
            if (priv->focusSource) {
                g_source_destroy(priv->focusSource.get());
                priv->focusSource = nullptr;
            }
            if (wpeDisplayPS5GetActiveView(display) == view)
                wpeDisplayPS5SetActiveView(display, nullptr);
            wpe_view_focus_out(view);
            wpe_view_unmap(view);
            return;
        }

        int width, height;
        wpe_toplevel_get_size(toplevel, &width, &height);
        if (width && height)
            wpe_view_resized(view, width, height);
        wpe_view_map(view);
        wpeDisplayPS5SetActiveView(display, view);

        // One fullscreen view owns the pad: give it keyboard focus on the next
        // main loop iteration (as the headless view does), so WebKit sees a
        // focus change.
        if (priv->focusSource)
            return;
        priv->focusSource = adoptGRef(g_idle_source_new());
        g_source_set_priority(priv->focusSource.get(), RunLoopSourcePriority::RunLoopDispatcher);
        g_source_set_name(priv->focusSource.get(), "WPE PS5 view focus");
        g_source_set_callback(priv->focusSource.get(), [](gpointer userData) -> gboolean {
            auto* view = WPE_VIEW(userData);
            WPE_VIEW_PS5(view)->priv->focusSource = nullptr;
            if (wpe_view_get_mapped(view))
                wpe_view_focus_in(view);
            return G_SOURCE_REMOVE;
        }, view, nullptr);
        g_source_attach(priv->focusSource.get(), g_main_context_get_thread_default());
    }), nullptr);
}

static void wpeViewPS5Dispose(GObject* object)
{
    auto* priv = WPE_VIEW_PS5(object)->priv;
    if (priv->frameSource) {
        g_source_destroy(priv->frameSource.get());
        priv->frameSource = nullptr;
    }
    if (priv->focusSource) {
        g_source_destroy(priv->focusSource.get());
        priv->focusSource = nullptr;
    }
    G_OBJECT_CLASS(wpe_view_ps5_parent_class)->dispose(object);
}

static gboolean wpeViewPS5RenderBuffer(WPEView* view, WPEBuffer* buffer, const WPERectangle* damageRects, guint damageCount, GError** error)
{
    auto* priv = WPE_VIEW_PS5(view)->priv;
    if (!WPE_IS_BUFFER_SHM(buffer)) {
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "PS5 view only renders shared-memory buffers");
        return FALSE;
    }

    auto* shm = WPE_BUFFER_SHM(buffer);
    gsize size = 0;
    const auto* data = static_cast<const uint8_t*>(g_bytes_get_data(wpe_buffer_shm_get_data(shm), &size));
    static_assert(sizeof(WPERectangle) == sizeof(PS5Renderer::Rect));
    PS5Renderer::singleton().uploadPage(data, wpe_buffer_get_width(buffer), wpe_buffer_get_height(buffer),
        wpe_buffer_shm_get_stride(shm), reinterpret_cast<const PS5Renderer::Rect*>(damageRects), damageCount);

    // Present now; eglSwapBuffers paces this to the TV's refresh.
    auto* display = WPE_DISPLAY_PS5(wpe_view_get_display(view));
    wpeDisplayPS5ScheduleRedraw(display);

    if (!priv->frames++) {
        priv->firstFrameTime = g_get_monotonic_time();
        PS5_LOG("first page frame: %dx%d stride %u, %u damage rects", wpe_buffer_get_width(buffer),
            wpe_buffer_get_height(buffer), wpe_buffer_shm_get_stride(shm), damageCount);
    } else if (!(priv->frames % 600)) {
        double seconds = (g_get_monotonic_time() - priv->firstFrameTime) / 1e6;
        PS5_LOG("%u page frames in %.1f s", priv->frames, seconds);
    }

    // The pixels are copied: tell WebKit the frame is done and hand the
    // buffer back, from the main loop (as the headless view does).
    priv->renderedBuffer = buffer;
    if (!priv->frameSource) {
        priv->frameSource = adoptGRef(g_idle_source_new());
        g_source_set_priority(priv->frameSource.get(), RunLoopSourcePriority::RunLoopDispatcher);
        g_source_set_name(priv->frameSource.get(), "WPE PS5 frame done");
        g_source_set_callback(priv->frameSource.get(), [](gpointer userData) -> gboolean {
            auto* view = WPE_VIEW(userData);
            auto* priv = WPE_VIEW_PS5(view)->priv;
            priv->frameSource = nullptr;
            if (auto buffer = WTF::move(priv->renderedBuffer)) {
                wpe_view_buffer_rendered(view, buffer.get());
                wpe_view_buffer_released(view, buffer.get());
            }
            return G_SOURCE_REMOVE;
        }, view, nullptr);
        g_source_attach(priv->frameSource.get(), g_main_context_get_thread_default());
    }
    return TRUE;
}

static gboolean wpeViewPS5CanBeMapped(WPEView*)
{
    return TRUE;
}

static void wpe_view_ps5_class_init(WPEViewPS5Class* viewPS5Class)
{
    GObjectClass* objectClass = G_OBJECT_CLASS(viewPS5Class);
    objectClass->constructed = wpeViewPS5Constructed;
    objectClass->dispose = wpeViewPS5Dispose;

    WPEViewClass* viewClass = WPE_VIEW_CLASS(viewPS5Class);
    viewClass->render_buffer = wpeViewPS5RenderBuffer;
    viewClass->can_be_mapped = wpeViewPS5CanBeMapped;
}
