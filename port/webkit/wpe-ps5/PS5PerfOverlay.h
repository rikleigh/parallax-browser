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

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

class PS5CompositorClient;

// The performance overlay (port/webkit/OVERLAY.md): live numbers in a
// corner of the TV, drawn by PS5Renderer on its render thread over
// everything else, text refreshed twice a second, a frame-time graph
// redrawn every frame. Text: a glyph atlas (FreeType, NotoSansMono-Bold)
// made once; the panel, the glyphs and the graph are quads in one
// persistently mapped vertex buffer, one draw call.
//
// Where the numbers come from: the render loop's own timings (noteFrame),
// the GPU compositor's counters (PS5CompositorClient::perfSnapshot), a
// sampler thread (process and thread CPU clocks, flexible and direct
// memory, TCP byte counters, the decoder budget), the WebProcess's and
// NetworkProcess's "[perf]" lines (apps/child/ps5wk-child.cpp, one per
// second, kept out of the log by the output relay) and their "[render]"
// lines, and named counters other parts of the title set (e.g. the ad
// blocker's "blocked").

// Any thread (the child output relays): one line of a payload's output.
// True: an overlay line, not for the log.
bool ps5PerfNoteChildLine(const char* tag, const char* line);
// The payload's output closed (it exited).
void ps5PerfChildEnded(const char* tag);
// Any thread: a named counter (a total since start) shown on the overlay
// with its rate; up to 6 names.
void ps5PerfSetCounter(const char* name, uint64_t value);
// Main thread: the main thread's CPU sampler (a GLib timeout) on or off.
void ps5PerfMainThreadSampler(bool on);
// WPEDisplayPS5.cpp: time the main loop has spent waiting in poll (us).
int64_t ps5wkMainLoopPollUS();

// One presented frame (render thread, after its swap), CLOCK_MONOTONIC us.
struct PS5PerfFrame {
    int64_t start, applied, consumed, painted, drawn, swapStart, swapEnd;
    double uploadMS; // CPU path: the page copy (0: no new page image)
    bool composited; // the GPU compositor's scene was drawn
};

// What the renderer knows when the overlay draws.
struct PS5PerfState {
    PS5CompositorClient* compositor; // set up and ready, else null
    bool compositorBuild; // a compositor exists (WC build)
    bool composited; // this frame shows the composited scene
    int pageWidth, pageHeight;
    size_t pageBufferBytes;
    uint64_t videoFramesConverted; // total
    unsigned vao; // the renderer's vertex array (bound again after the draw)
};

class PS5PerfOverlay {
public:
    // Render thread, GL current. options: the flag file's words (pos=tl|tr|
    // bl|br, size=<px at 1080p>, graph=0).
    PS5PerfOverlay(pthread_t renderThread, pthread_t mainThread, const char* options);
    ~PS5PerfOverlay();

    void setActive(bool); // starts or parks the sampler thread
    void noteFrame(const PS5PerfFrame&);
    // One frame's GPU time, a few frames late (GPU=radv: PS5Renderer's timer queries).
    void noteGPUTime(double ms);
    // Draws over the current framebuffer (0, viewport = the TV).
    void draw(int width, int height, const PS5PerfState&);
    // When the text is due again (the render loop wakes for it).
    int64_t nextUpdateUS() const { return m_nextUpdateUS; }

    struct Impl;

private:
    Impl* m_impl;
    int64_t m_nextUpdateUS { 0 };
};
