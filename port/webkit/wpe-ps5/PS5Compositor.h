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

#include <stdint.h>

// The GPU compositor's seam in PS5Renderer (port/webkit/COMPOSITING-PLAN.md,
// "Render loop"). Plain C++: WPEPlatform never sees WebKit types. The WC
// build's UIProcess (WebKit/UIProcess/wpe/ps5/PS5WCHost.cpp) implements it
// with WCScene/TextureMapper; the CPU build has none.
//
// attach() runs on the main thread; every other call on the render thread
// with PS5Renderer's GL context current.

struct PS5CompositorContext {
    void* eglDisplay;
    void* eglConfig;
    void* eglContext;
    void* eglSurface;
    int width, height; // the TV surface
    // Any thread: asks the render thread for a frame (applyPending, paint,
    // swap, didPresent) as soon as it is idle.
    void (*requestFrame)();
    // Render thread, during paint(): the RGBA texture (row 0 = top, opaque)
    // the newest frame of the video stream whose sink sent `key` in its
    // HELLO was converted into, or 0 (no such stream, no frame yet); frames
    // counts the frames converted into it so far. A stream drawn this way
    // is not drawn under the page (hole punch is the CPU path's).
    unsigned (*videoTexture)(uint64_t key, int* width, int* height, uint32_t* frames);
    // WebGL's video textures (M3, spikes/11-webgl). Any thread but the
    // render thread, with a GL context of the compositor's share group
    // current that nothing else uses meanwhile: copies the newest converted
    // frame of the stream `key` into `texture` (GL_TEXTURE_2D, RGBA8, at
    // least videoFrameSize), upside down if flipY. The copy waits for the
    // conversion on the GPU and the next conversion waits for the copy.
    // Returns a GL sync object after the copy (the caller waits on it in the
    // context that reads the texture, then deletes it), or null: no frame.
    // GPU=radv, in a WebGL context: (void*)1, copied in that context, with
    // no fence (WebGL 1's has none).
    void* (*copyVideoFrame)(uint64_t key, unsigned texture, bool flipY);
    // Any thread: the size of the stream's newest converted frame; false: none.
    bool (*videoFrameSize)(uint64_t key, int* width, int* height);
};

// Counters for the performance overlay (PS5PerfOverlay.cpp). Totals since
// set-up, never reset (the overlay takes differences between snapshots),
// then values as they are now.
struct PS5CompositorPerf {
    uint64_t updates; // WebProcess updates applied
    uint64_t updateApplyUS;
    uint64_t paints; // scene paints
    uint64_t paintUS;
    uint64_t presents; // frames that reached the TV with the page's scene in them
    uint64_t tileUploads, tileUploadBytes, tileUploadUS;
    uint64_t scrollFrames; // presented frames that moved the shown page
    uint64_t scrollLatencies, scrollLatencyUS; // pad wheel event -> the frame that moved the page
    uint64_t wheelEvents; // pad wheel events
    uint64_t clampedEvents; // async scrolling stopped at the painted edge
    double scrollLatencyMaxMS; // since the previous snapshot
    unsigned pages, layers, videoLayers;
    double glTextureMB;
    bool coverageReduced; // over the GL texture cap: tiles for the visible rect only
};

class PS5CompositorClient {
public:
    virtual ~PS5CompositorClient() = default;

    // Main thread, from PS5Renderer::initialize() before the render thread
    // starts (WebKit may not be initialized yet: store, don't call into it).
    // requestFrame: any thread, wakes the render thread (see below).
    virtual void attach(void (*requestFrame)()) = 0;
    // Polled by the render thread before each frame: set up now? The client
    // asks (then requestFrame) once WebKit is up, e.g. when the first page
    // wants to know whether it can composite.
    virtual bool wantsSetUp() = 0;
    // Once, on request. False: no compositing for good (the client answers
    // its queued and future updates without showing them).
    virtual bool setUp(const PS5CompositorContext&) = 0;
    // Queued updates waiting to be applied?
    virtual bool hasPending() = 0;
    // Applies queued layer-tree updates in order (tile uploads included).
    virtual void applyPending() = 0;
    // Paints the scene if it changed. Returns the scene texture (GL_TEXTURE_2D,
    // premultiplied RGBA, row 0 = the top of the page) and its size, or 0 when
    // there is nothing composited to show (PS5Renderer then draws the CPU page).
    // displayUS: when the frame is expected on screen (CLOCK_MONOTONIC us).
    virtual unsigned paint(int64_t displayUS, int* width, int* height) = 0;
    // Right after the swap (or headset submit) that shows what was applied so
    // far: the GPU is done with that frame's uploads; replies may go out.
    virtual void didPresent() = 0;
    // The overlay's counters (after setUp() succeeded); false: none.
    virtual bool perfSnapshot(PS5CompositorPerf&) { return false; }
};

#if defined(PS5WK_WC) && PS5WK_WC
// The WC build's compositor (PS5WCHost.cpp). Main thread, once.
PS5CompositorClient* ps5wkWCCompositorClient();
#endif

// The title's own GL: PS5Renderer's context, which draws the compositor's
// scene, the CPU page, video and the browser's UI. The strings as read on
// the render thread when the context was made, for webkit://gpu (WebKit's
// WebKitProtocolHandler.cpp). Any thread; the strings stay valid. False
// until the renderer is up and where there is none (ps5wk-shot).
struct PS5RendererGLInfo {
    const char* renderer; // GL_RENDERER
    const char* vendor; // GL_VENDOR
    const char* version; // GL_VERSION
    const char* shadingLanguageVersion; // GL_SHADING_LANGUAGE_VERSION
    const char* eglVersion; // EGL_VERSION
    const char* eglVendor; // EGL_VENDOR
};
bool ps5RendererGLInfo(PS5RendererGLInfo&);
