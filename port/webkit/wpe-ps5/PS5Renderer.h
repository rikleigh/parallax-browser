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
#include <stddef.h>
#include <string>
#include <vector>

class PS5CompositorClient;
struct PS5VideoStream;
struct PS5VideoFrame;
struct PS5VRState;

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
//
// "View in VR" (PS5RendererVR.cpp): the render thread can also drive the
// PS VR2 headset (Spike 3b/9's path: Hmd2 at 120 Hz with half-rate
// rendering, VrTracker2 head pose, GL eye buffers exported as T#s). It then
// paces itself to the headset instead of the TV, draws the newest video
// frame on a sphere, dome or flat screen per eye, and leaves a notice on the
// TV. Video frames, page buffers and decoder slots flow as before.
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

    // GPU compositing (PS5Compositor.h): main thread, before the render thread
    // starts (initialize() sets the WC build's client itself). The client is
    // set up on the render thread once GL is up; while it has a frame, its
    // scene texture is drawn instead of the page buffer.
    void setCompositor(PS5CompositorClient*);
    // Any thread: the compositor has work (applyPending/paint/didPresent).
    void requestCompositorFrame();

    // Hardware decoding (PS5VideoDecoder's output buffers): a persistent,
    // coherent GL buffer made on the render thread (blocks until it is);
    // freed there at the start of the next frame.
    bool allocateVideoBuffer(size_t bytes, unsigned* name, uint8_t** map);
    void freeVideoBuffer(unsigned name);

    // ---- View in VR (main thread) ----
    // Projections, as WPEDisplayPS5.h's WPE_DISPLAY_PS5_VR_* values.
    enum VRProjection : int {
        VRFlat = 0, VR360, VR360SBS, VR360TB, VR180, VR180SBS, VR180TB, VRFisheyeSBS, VRProjectionCount
    };
    // Asks the render thread to take the playing video into the headset.
    // done(ok, message, userData) runs on the main thread once the headset
    // is up (ok) or could not be (no video, no headset: message says why).
    // ended(why, userData) runs on the main thread when VR ends by itself
    // (headset lost), not after stopVR().
    using VRDone = void (*)(bool ok, const char* message, void* userData);
    using VREnded = void (*)(const char* why, void* userData);
    void startVR(int projection, float fisheyeDegrees, VRDone, VREnded, void* userData);
    // Back to the TV. wait: block until the headset is released (wind-down;
    // at most ~3 s).
    void stopVR(bool wait);
    bool vrActive();
    void setVRProjection(int projection, float fisheyeDegrees);
    void recenterVR();
    // hint: shown in the headset for `seconds` (empty: none); tvText: the
    // notice on the TV while in VR (nullptr: keep).
    void setVRText(const char* hint, double seconds, const char* tvText);
    // Log-only tests (/data/ps5webkit/vr-sim: no headset, the eye buffers
    // are rendered with this head direction, degrees, right and up positive).
    void setVRSimLook(float yawDegrees, float pitchDegrees);
    // A short message over the page on the TV (e.g. "No headset").
    void showMessage(const char* text, double seconds);
    // Small luma pictures of the playing video (View in VR's layout guess):
    // up to `count` decoded frames at least 200 ms apart, within 1 s (fewer
    // if the video is paused: no newer frame within 400 ms ends it), each kThumbnailSize x kThumbnailSize bytes,
    // every cell the average of its part of the frame (8 x 8 taps), row 0 =
    // top. done(luma, frames, videoWidth, videoHeight, userData) runs on the
    // main thread (frames 0: no video). A new request answers a pending one
    // with no frames.
    static constexpr int kThumbnailSize = 128;
    using ThumbnailsDone = void (*)(const uint8_t* luma, unsigned frames, int width, int height, void* userData);
    void requestVideoThumbnails(unsigned count, ThumbnailsDone, void* userData);

private:
    PS5Renderer() = default;
    static void* threadEntry(void*);
    void threadMain();
    bool initializeGL(char* error, unsigned errorSize);
    bool createPrograms(char* error, unsigned errorSize);
    void renderFrame();
    void copyPage();
    void handleVideo(PS5VideoStream&, const PS5VideoFrame*, const uint8_t*);
    void serviceBufferRequests();
    void drawScene();
    void writeCapture(char* path);
    void drawPanel(unsigned texture, int width, int height, double centerX, double centerY, double scale);
    void updateMessage(int64_t now);

    // PS5RendererVR.cpp, render thread
    void vrHandleRequests();
    bool vrStart(int projection, float fisheyeDegrees, char* message, size_t messageSize);
    void vrStop(const char* why);
    void vrFrame();
    bool vrCreatePrograms();
    void vrRenderEyes(unsigned buffer, const void* frame);
    void vrDrawTV(int64_t now);
    void vrCapture(unsigned buffer, char* path);
    void vrUpdateText(int64_t now);
    void serviceThumbnails(int64_t now);
    bool takeThumbnail(unsigned texture, uint8_t* out);

    bool m_initialized { false };
    void* m_eglDisplay { nullptr };
    void* m_eglSurface { nullptr };
    void* m_eglContext { nullptr };
    void* m_eglConfig { nullptr };
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
    struct BufferRequest {
        size_t bytes { 0 };
        unsigned name { 0 };
        uint8_t* map { nullptr };
        bool done { false };
    };
    std::vector<BufferRequest*> m_bufferRequests;
    std::vector<unsigned> m_bufferFrees;
    bool m_frameRequested { false };
    bool m_compositorRequested { false };
    bool m_pointerVisible { false };
    double m_pointerX { 0 }, m_pointerY { 0 };
    char* m_capturePath { nullptr };

    // Render thread only.
    PS5CompositorClient* m_compositor { nullptr }; // set before the render thread starts
    bool m_compositorReady { false };
    bool m_compositorSetUpDone { false };
    void setUpCompositorIfWanted();
    unsigned m_sceneTexture { 0 };
    int m_sceneWidth { 0 }, m_sceneHeight { 0 };
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
    static constexpr unsigned kMaxVideoStreams = 16; // drawn or kept as sources per frame
    struct VideoDraw { unsigned texture; int x, y, width, height; uint64_t key; };
    VideoDraw m_videoDraws[kMaxVideoStreams];
    unsigned m_videoDrawCount { 0 };
    // Every stream with a converted frame, refreshed by each consume pass
    // (the TV's or the headset's): what "View in VR" can show, and what a
    // composited page's video layers draw (key: the sink's stream key).
    struct VideoSource { int stream; unsigned texture; int width, height; int64_t lastFrameUS; uint64_t key; uint32_t frames; };
    VideoSource m_videoSources[kMaxVideoStreams];
    unsigned m_videoSourceCount { 0 };
    // GPU compositing: the stream keys the scene drew as video layers this
    // frame (PS5CompositorContext::videoTexture); not drawn under the page.
    static unsigned compositorVideoTexture(uint64_t key, int* width, int* height, uint32_t* frames);
    uint64_t m_layerVideoKeys[kMaxVideoStreams] { };
    unsigned m_layerVideoKeyCount { 0 };

    // Message over the page (guarded by m_lock: m_messageText/m_messageSeconds/
    // m_messageChanged; render thread: the rest).
    std::string m_messageText;
    double m_messageSeconds { 0 };
    bool m_messageChanged { false };
    unsigned m_messageTexture { 0 };
    int m_messageWidth { 0 }, m_messageHeight { 0 };
    int64_t m_messageUntilUS { 0 };

    // View in VR requests (guarded by m_lock).
    struct VRRequest {
        bool start { false };
        int projection { 0 };
        float fisheyeDegrees { 180 };
        VRDone done { nullptr };
        VREnded ended { nullptr };
        void* userData { nullptr };
        bool simLook { false };
        float simYaw { 0 }, simPitch { 0 };
        bool stop { false };
        bool setProjection { false };
        bool recenter { false };
        bool textChanged { false };
        std::string hint;
        double hintSeconds { 0 };
        bool tvTextChanged { false };
        std::string tvText;
    } m_vrRequest;
    bool m_vrActiveShared { false }; // guarded by m_lock (vrActive(), stopVR(wait))
    struct ThumbnailRequest {
        bool pending { false };
        unsigned count { 0 };
        ThumbnailsDone done { nullptr };
        void* userData { nullptr };
    } m_thumbnailRequest; // guarded by m_lock
    struct Thumbnails { // render thread
        bool active { false };
        unsigned count { 0 }, taken { 0 };
        ThumbnailsDone done { nullptr };
        void* userData { nullptr };
        int64_t deadlineUS { 0 }, nextUS { 0 }, stallUS { 0 }, lastFrameUS { 0 };
        int width { 0 }, height { 0 };
        std::vector<uint8_t> luma;
    } m_thumbnails;
    PS5VRState* m_vr { nullptr }; // render thread

    // vsync estimate (CLOCK_MONOTONIC us)
    int64_t m_lastSwapEnd { 0 };
    int64_t m_vsyncGrid { 0 };
    double m_periodUS { 16683.0 };
};
