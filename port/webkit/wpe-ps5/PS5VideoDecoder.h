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

/*
 * Hardware video decoding in the UIProcess (Milestone 2 step 2b): the
 * decoder side of PS5VideoServer. A WebProcess's ps5vdecproxy element opens
 * a decoder connection (PS5VideoProtocol.h, role DECODER) and sends access
 * units; a decoder thread here decodes them ahead of time, straight into
 * output buffers the renderer can read (GL buffers in the title), and
 * answers each picture with PICTURE (display order). The WebProcess's sink
 * then names the picture in a REMOTE FRAME, the consumer draws it from the
 * output buffer and the slot goes back to the decoder once it is presented.
 *
 * The decoding itself is a backend the executable installs: Videodec2 in
 * the title (apps/title/titlevdec.c, Spike 9's vdec2), or a fake in
 * ps5wk-shot that returns empty pictures in display order (exercises the
 * WebProcess side in payload-only runs). Without a backend HELLO_ACK
 * advertises nothing and the WebProcess decodes in software.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ps5wk_vdec_picture {
    int valid;            /* a picture came out of this call */
    int error;            /* the decoder flagged it */
    uint32_t width;       /* coded width (luma samples) */
    uint32_t height;      /* coded height (rows before the chroma plane) */
    uint32_t pitch_bytes; /* bytes per row */
    void* buffer;         /* where the picture starts (inside the frame passed) */
    int has_pts;
    uint64_t pts;         /* the pts passed with its access unit */
};

struct ps5wk_vdec_backend {
    const char* name;
    unsigned codecs; /* PS5WK_VCODEC_* */
    int max_width, max_height;
    /* Brings up a decoder; *frame_bytes / *frame_align: what an output buffer needs. */
    void* (*open)(unsigned codec, unsigned profile, unsigned bit_depth, int width, int height,
        size_t* frame_bytes, size_t* frame_align, char* error, size_t error_size);
    /* One access unit, the picture (if any) into frame. pts_ns: the buffer PTS (the fake reorders by it). */
    int (*decode)(void* decoder, const void* au, size_t size, uint64_t pts, int64_t pts_ns, void* frame, struct ps5wk_vdec_picture*);
    /* One picture left in the decoder's pipeline into frame; valid 0 when empty. */
    int (*flush)(void* decoder, void* frame, struct ps5wk_vdec_picture*);
    int (*reset)(void* decoder);
    void (*close)(void* decoder);
    /* Optional: what the backend did at load time, for the log. */
    const char* (*report)(void);
};

/* Installs the backend (before the video server starts). */
void ps5wk_video_set_decoder_backend(const struct ps5wk_vdec_backend*);

/* ps5wk-shot: the fake backend (PS5WK_FAKE_VDEC=1). */
const struct ps5wk_vdec_backend* ps5wk_video_fake_decoder_backend(void);

#ifdef __cplusplus
}

#include "PS5VideoProtocol.h"

struct PS5VideoPicture {
    unsigned glBuffer { 0 };      // GL buffer name (0 for memory the renderer can't sample)
    const uint8_t* map { nullptr };
    size_t bufferBytes { 0 };
    uint32_t offset { 0 };        // picture start within the buffer
    uint32_t pitch { 0 };         // bytes per row
    uint32_t codedHeight { 0 };   // rows of luma before the chroma plane
    uint32_t bitDepth { 8 };      // 10: 16-bit samples, value in the low bits
};

namespace PS5VideoDecoder {

// What HELLO_ACK advertises.
unsigned codecs();
void maxSize(int& width, int& height);
bool available();

// A decoder connection (after its HELLO): runs until the socket closes.
void serve(int fd, int pid, int streamID);

// Consumer side (PS5VideoServer): a REMOTE frame's picture. pin() keeps it
// (and its buffer) for drawing; release() gives that picture's slot back
// (wasPinned: it was presented; older pictures of that decoder still held
// then were lost on the way and go back too, a safety net). The WebProcess
// gives back what it doesn't hand to the title (PRELEASE: the proxy's drops
// and every placeholder buffer freed unrendered).
bool pin(uint32_t decoder, uint32_t token, PS5VideoPicture&);
void release(uint32_t decoder, uint32_t token, bool wasPinned);

// Output buffers: GL buffers from the renderer in the title (allocated on
// its thread); plain memory without one.
struct BufferProvider {
    bool (*allocate)(size_t bytes, unsigned* glBuffer, uint8_t** map);
    void (*free)(unsigned glBuffer, uint8_t* map, size_t bytes);
};
void setBufferProvider(const BufferProvider&);

// The decoder budget now (the performance overlay): open decoders and their
// direct memory against the limits.
struct Budget {
    unsigned decoders, maxDecoders;
    double directMB, maxDirectMB;
    uint64_t refused;
};
void budget(Budget&);

} // namespace PS5VideoDecoder
#endif
