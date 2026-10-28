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

#include <glib.h>
#include <pthread.h>
#include <stdint.h>

struct PS5VideoStream;
struct PS5VideoFrame;

// Presents CPU-painted page buffers and hole-punch video on the TV through
// ps5-opengl: one EGL window surface (a second one fails with EGL_BAD_ALLOC
// on 13.60, Spike 9) and one desktop GL 4.6 core context, both owned by a
// render thread, so neither the swap (paced to vsync) nor copies block the
// UI main loop.
//
// Each frame, back to front: the videos (YUV frames from PS5VideoServer,
// converted to RGBA on the GPU, at the rectangles the page punched), the
// page (premultiplied BGRA, blended: transparent where the page punched a
// hole for a <video>), the pad pointer.
//
// The page lives in a persistent, coherent GL buffer that the shader reads
// directly, so a page update is a memcpy of the damaged rows (~1 ms for a
// full 1080p page) instead of glTexSubImage2D (34 ms: ps5-opengl tiles on
// the CPU at ~210 MB/s). /data/ps5webkit/page-texture selects the old
// texture path.
class PS5Renderer {
public:
    static PS5Renderer& singleton();

    // Starts the render thread and waits for EGL/GL to come up on it.
    bool initialize(char* error, unsigned errorSize);
    bool isInitialized() const { return m_initialized; }

    void* eglDisplay() const { return m_eglDisplay; }
    int width() const { return m_width; }
    int height() const { return m_height; }

    struct Rect { int x, y, width, height; };
    // Main thread: queue a premultiplied BGRA page buffer (no damage rects =
    // all of it). data must stay valid until done(userData) is called, on
    // the render thread, once the damaged rows are copied; the frame with
    // it is presented right after.
    void submitPage(const uint8_t* data, int width, int height, unsigned stride, const Rect* damage, unsigned damageCount,
        void (*done)(void*), void* userData);

    // Main thread: pointer state; schedules a frame.
    void present(bool pointerVisible, double pointerX, double pointerY);

    // Any thread: something changed (video frames arrived).
    void wake();

    // Debug screen readback: the next frame is read back (glReadPixels),
    // halved (1920x1080 -> 960x540, box filter), flipped top-down and
    // written to path as a binary PPM (P6, RGB).
    void requestCapture(const char* path);

    // CPU time of the render thread so far (seconds), or -1.
    double renderThreadCPUSeconds() const;

private:
    PS5Renderer() = default;
    static void* threadEntry(void*);
    void threadMain();
    bool initializeGL(char* error, unsigned errorSize);
    bool createPrograms(char* error, unsigned errorSize);
    void renderFrame();
    void copyPage();
    void handleVideo(PS5VideoStream&, const PS5VideoFrame*, const uint8_t*);
    void drawScene();
    void writeCapture(char* path);

    bool m_initialized { false };
    void* m_eglDisplay { nullptr };
    void* m_eglSurface { nullptr };
    void* m_eglContext { nullptr };
    int m_width { 0 };
    int m_height { 0 };

    pthread_t m_thread { };
    GMutex m_lock;
    GCond m_cond;
    bool m_initDone { false };
    bool m_initOK { false };
    char m_initError[512] { };

    // Requests from other threads (guarded by m_lock).
    struct PageJob {
        bool pending { false };
        const uint8_t* data { nullptr };
        int width { 0 }, height { 0 };
        unsigned stride { 0 };
        Rect* damage { nullptr };
        unsigned damageCount { 0 };
        void (*done)(void*) { nullptr };
        void* userData { nullptr };
    } m_pageJob;
    bool m_frameRequested { false };
    bool m_pointerVisible { false };
    double m_pointerX { 0 }, m_pointerY { 0 };
    char* m_capturePath { nullptr };

    // Render thread only.
    PageJob m_job;
    bool m_drawPointer { false };
    double m_drawPointerX { 0 }, m_drawPointerY { 0 };
    bool m_videoActive { false };
    bool m_pageAsTexture { false };
    unsigned m_program { 0 };
    int m_uRect { -1 }, m_uTexture { -1 }, m_uOpaque { -1 };
    unsigned m_pageProgram { 0 };
    int m_uPageRect { -1 }, m_uPageNDC { -1 }, m_uPageSize { -1 }, m_uPageStride { -1 }, m_uScreenHeight { -1 };
    unsigned m_convertProgram { 0 };
    int m_uFormat { -1 }, m_uStrides { -1 }, m_uOffsets { -1 }, m_uColor { -1 };
    unsigned m_vao { 0 };
    unsigned m_pageTexture { 0 };
    unsigned m_pageBuffer { 0 };
    uint8_t* m_pageMap { nullptr };
    size_t m_pageBufferBytes { 0 };
    int m_pageWidth { 0 };
    int m_pageHeight { 0 };
    unsigned m_pointerTexture { 0 };
    struct VideoDraw { unsigned texture; int x, y, width, height; };
    VideoDraw m_videoDraws[8];
    unsigned m_videoDrawCount { 0 };

    // vsync estimate (CLOCK_MONOTONIC us)
    int64_t m_lastSwapEnd { 0 };
    int64_t m_vsyncGrid { 0 };
    double m_periodUS { 16683.0 };
};
