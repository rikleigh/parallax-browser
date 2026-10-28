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

#include "PS5VideoDecoder.h"
#include "PS5VideoProtocol.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <deque>
#include <functional>
#include <glib.h>

// Hole-punch video in the UIProcess (Milestone 2 step 2a): one stream per
// WebProcess ps5videosink (PS5VideoProtocol.h). Receiver threads queue the
// frames the sinks announce; the consumer (PS5Renderer's render thread, or
// a null consumer in ps5wk-shot) picks the frame due at each vsync and
// releases the slots.
//
// Step 2b (Videodec2 in the title) adds a second kind of frame source:
// PS5VideoDecoder decodes access units from the WebProcess's ps5vdecproxy
// straight into the renderer's GL buffers; the sink then sends REMOTE frames
// that name a decoded picture instead of a shared-memory slot. The consumer
// side (frame selection by time, geometry, compositing) stays.

struct PS5VideoFrame {
    uint32_t slot { 0 };
    uint32_t generation { 0 };
    uint32_t format { 0 };
    uint32_t width { 0 };
    uint32_t height { 0 };
    uint32_t colorimetry { 0 };
    uint32_t stride[3] { };
    uint32_t offset[3] { };
    int64_t targetUS { 0 }; // CLOCK_MONOTONIC, 0 = at once
    int64_t ptsNS { -1 };
    // REMOTE frames (format PS5WK_VFMT_REMOTE): a title decoder's picture,
    // pinned while the consumer draws it.
    bool remote { false };
    uint32_t decoder { 0 };
    uint32_t token { 0 };
    PS5VideoPicture picture;
};

struct PS5VideoGeometry {
    bool visible { false };
    int x { 0 }, y { 0 }, width { 0 }, height { 0 }; // page-buffer pixels
};

struct PS5VideoStream {
    int id { 0 };
    int pid { 0 };
    int fd { -1 };

    // Guarded by lock.
    GMutex lock;
    uint8_t* shm { nullptr };
    size_t shmBytes { 0 };
    uint32_t slots { 0 };
    uint32_t slotBytes { 0 };
    uint32_t generation { 0 };
    std::deque<PS5VideoFrame> queue;
    bool hasPending { false };
    PS5VideoGeometry pending;
    int64_t pendingSinceUS { 0 };
    bool closed { false };

    // Consumer thread only.
    PS5VideoGeometry geometry; // committed: drawn with the page it came with
    bool hasFrame { false };   // a frame was consumed (the consumer keeps showing it)
    int64_t lastFrameUS { 0 };
    uint32_t received { 0 }, displayed { 0 }, dropped { 0 }, late { 0 };
    void* consumerData { nullptr }; // the renderer's GL objects for this stream
    // stats window
    int64_t windowStartUS { 0 };
    uint32_t windowReceived { 0 }, windowDisplayed { 0 }, windowDropped { 0 };
    double windowLateMS { 0 };
    // Cadence: vsyncs each frame stayed on screen (consume() runs once per
    // presented frame), as a histogram (1, 2, 3, 4+) and the last 16 holds.
    unsigned holdVsyncs { 0 };
    unsigned holdHistogram[4] { };
    char holdPattern[17] { };
    unsigned holdPatternLength { 0 };
    int64_t lastGeometryLogUS { 0 };

    PS5VideoStream() { g_mutex_init(&lock); }
    ~PS5VideoStream() { g_mutex_clear(&lock); }
};

namespace PS5VideoServer {

// Listens on PS5WK_VIDEO_SOCKET_FORMAT for this process; wake() is called
// (from receiver threads) whenever a frame or a geometry change arrives.
void start(void (*wake)());

// One consumer pass, at each presented frame:
// - commitGeometry: a new page buffer is going on screen now, so pending
//   geometry becomes current (it is also committed after 100 ms anyway);
// - for each stream, the newest frame due by displayUS + slackUS is handed
//   to consume(stream, frame, data) with the stream's shared memory mapped
//   (older due frames are dropped), then its slot is released;
// - consume(stream, nullptr, nullptr) for streams without a new frame, and
//   with stream.closed set once before a closed stream is deleted (free
//   consumerData then).
// Returns true while any stream has frames queued or showed one in the
// last 500 ms (keep presenting every vsync).
using Consumer = std::function<void(PS5VideoStream&, const PS5VideoFrame*, const uint8_t*)>;
bool consume(int64_t displayUS, int64_t slackUS, bool commitGeometry, const Consumer&);

// After the frame drawn in consume() is on screen (the swap returned, so the
// GPU is done with it): REMOTE pictures drawn go back to their decoders.
void presented();

// Earliest time (CLOCK_MONOTONIC us) something is due: a queued frame or a
// pending geometry's 100 ms commit; INT64_MAX if nothing.
int64_t nextDeadline();

unsigned streamCount();

} // namespace PS5VideoServer

extern "C" {
#endif

// ps5wk-shot (no display): consume frames on a 60 Hz timer and log what
// arrives, so payload-only runs exercise the WebProcess side.
void ps5wk_video_server_start_null(void);

#ifdef __cplusplus
}
#endif
