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
 * Hole-punch video, Milestone 2 step 2a: wire format between the
 * WebProcess's ps5videosink (port/deps/gst/gstps5videosink.c, which keeps a
 * copy of these definitions: change both) and the title's video server
 * (PS5VideoServer.cpp).
 *
 * One unix stream connection per video sink, to
 * /system_tmp/ps5wk/video-<UIProcess pid>.sock (PS5WK_VIDEO_SOCKET in the
 * WebProcess). Every message is one struct ps5wk_video_msg (128 bytes).
 *
 * WebProcess -> title:
 *   HELLO     magic, version, pid; sinks: pts_ns = the sink's stream key
 *             (random, never 0; 0 from sinks before GPU compositing): a
 *             composited page names its video layer's stream by it
 *             (GraphicsLayerWC -> WCScene -> PS5Renderer::videoTexture)
 *   SHM       slots, slot_bytes, generation; the shared memory fd rides
 *             along (SCM_RIGHTS). A new generation replaces the old one:
 *             frames of older generations are dropped.
 *   FRAME     slot, generation, format, width, height, stride[], offset[],
 *             colorimetry, target_us (CLOCK_MONOTONIC microseconds at which
 *             the frame should be on screen; 0 = as soon as possible, for
 *             the preroll frame), pts_ns (for logs)
 *   GEOMETRY  visible, rect[] = the video's rectangle in page-buffer pixels
 *             (x, y, width, height), as punched into the page. The title
 *             applies it with the next page buffer (the hole and the video
 *             move together), or after 100 ms without one.
 *   FLUSH     drop every queued frame (seek, flush); the frame on screen
 *             stays
 * title -> WebProcess:
 *   RELEASE   slot, generation: the title is done with that slot (copied
 *             or dropped); displayed/dropped = the title's counts so far
 *
 * Closing the connection removes the video.
 *
 * Version 2 (step 2b, hardware decoding in the title): the title answers
 * every HELLO with HELLO_ACK (what it can decode). A connection whose HELLO
 * has role = PS5WK_VROLE_DECODER belongs to a ps5vdecproxy element
 * (port/deps/gst/gstps5vdecproxy.c) instead of a sink:
 *
 * WebProcess -> title (decoder connection):
 *   DOPEN     codec, profile, bit_depth, width, height: bring up Videodec2;
 *             answered by DOPEN_ACK (status 0 = decoding in hardware;
 *             decoder = its id, for the sink's REMOTE frames)
 *   AU        one access unit: payload_bytes of Annex-B H.264/HEVC or a VP9
 *             frame/superframe follow the message; key = the proxy's frame
 *             number (comes back in PICTURE), pts_ns = the buffer PTS,
 *             epoch, flags (PS5WK_VAU_KEYFRAME)
 *   DFLUSH    epoch = the new epoch: drop queued AUs and decoded pictures,
 *             reset the decoder (seek)
 *   DDRAIN    epoch: decode what is queued and empty the decoder's
 *             pipeline, then DRAINED (end of stream)
 *   PRELEASE  token: the WebProcess gives that picture back: the proxy
 *             won't output it (its frame is gone, clipped or late), or its
 *             placeholder buffer was freed before the sink handed it to the
 *             title (dropped, flushed or never rendered, wherever in the
 *             pipeline). A picture sent in PICTURE is the WebProcess's until
 *             then, or until the sink names it in a FRAME, a DFLUSH or the
 *             connection closes: the title never takes it back by itself.
 * WebProcess -> title (sink connection):
 *   RDROP     decoder, token: a placeholder the sink dropped without
 *             rendering (no longer sent: the buffer's PRELEASE covers it)
 * title -> WebProcess (decoder connection):
 *   PICTURE   key, token, epoch, width/height (coded), pitch, bit_depth: a
 *             decoded picture, in display order. status != 0: no picture
 *             for that key (VP9 hidden frame, decode error)
 *   DRAINED   epoch
 *
 * The proxy outputs a placeholder buffer per picture
 * (video/x-raw(memory:PS5Remote), struct ps5wk_remote_picture in its memory);
 * ps5videosink sends it as a FRAME with format PS5WK_VFMT_REMOTE,
 * decoder = the decoder id, token = the picture. The title draws the
 * decoded picture straight from the decoder's output buffer and gives the
 * slot back to the decoder after the frame is presented (or dropped).
 */

#include <stdint.h>

#define PS5WK_VIDEO_SOCKET_FORMAT "/system_tmp/ps5wk/video-%d.sock" /* %d = UIProcess pid */
#define PS5WK_VIDEO_MAGIC 0x56355350u /* "PS5V" */
#define PS5WK_VIDEO_VERSION 2
#define PS5WK_VIDEO_MAX_SLOTS 16

enum {
    PS5WK_VMSG_HELLO = 1,
    PS5WK_VMSG_SHM = 2,
    PS5WK_VMSG_FRAME = 3,
    PS5WK_VMSG_GEOMETRY = 4,
    PS5WK_VMSG_FLUSH = 5,
    PS5WK_VMSG_DOPEN = 6,
    PS5WK_VMSG_AU = 7,
    PS5WK_VMSG_DFLUSH = 8,
    PS5WK_VMSG_DDRAIN = 9,
    PS5WK_VMSG_PRELEASE = 10,
    PS5WK_VMSG_RDROP = 11, /* sink: a REMOTE picture reached the sink but was dropped (late): decoder, token */
    PS5WK_VMSG_RELEASE = 16,
    PS5WK_VMSG_HELLO_ACK = 17,
    PS5WK_VMSG_DOPEN_ACK = 18,
    PS5WK_VMSG_PICTURE = 19,
    PS5WK_VMSG_DRAINED = 20,
};

enum {
    PS5WK_VROLE_SINK = 0,
    PS5WK_VROLE_DECODER = 1,
    PS5WK_VROLE_QUERY = 2, /* HELLO_ACK, then the title closes the connection */
};

/* HELLO_ACK codecs (bits), DOPEN codec (one of them) */
#define PS5WK_VCODEC_H264 1u
#define PS5WK_VCODEC_HEVC 2u
#define PS5WK_VCODEC_VP9 4u
#define PS5WK_VCODEC_10BIT 0x100u /* HELLO_ACK: 10-bit HEVC/VP9 too */
#define PS5WK_VAU_KEYFRAME 1u

/* The placeholder buffer's memory (ps5vdecproxy -> ps5videosink, inside the
 * WebProcess only). handed_over: set (atomically) by the sink once a FRAME
 * naming the picture reached the title, which then gives it back itself;
 * when the memory is freed without it, the proxy sends PRELEASE. */
#define PS5WK_REMOTE_MAGIC 0x52355350u /* "PS5R" */
struct ps5wk_remote_picture {
    uint32_t magic;
    uint32_t decoder;
    uint32_t token;
    uint32_t epoch;
    int32_t handed_over;
};

enum {
    PS5WK_VFMT_I420 = 1, /* Y, then U, then V (4:2:0) */
    PS5WK_VFMT_NV12 = 2, /* Y, then interleaved UV (4:2:0) */
    PS5WK_VFMT_REMOTE = 3, /* FRAME: a picture in a title decoder's buffer */
    PS5WK_VFMT_P010LSB = 4, /* title-internal: NV12 layout, 16-bit samples, 10 bits in the low bits (Videodec2) */
};

/* colorimetry: bit 0 = BT.709 matrix (else BT.601), bit 1 = full range */
#define PS5WK_VCOLOR_BT709 1u
#define PS5WK_VCOLOR_FULL_RANGE 2u

struct ps5wk_video_msg {
    uint32_t type;
    uint32_t magic;       /* HELLO */
    uint32_t version;     /* HELLO */
    int32_t pid;          /* HELLO */
    uint32_t slot;        /* FRAME, RELEASE */
    uint32_t slots;       /* SHM */
    uint32_t slot_bytes;  /* SHM */
    uint32_t generation;  /* SHM, FRAME, RELEASE */
    uint32_t format;      /* FRAME */
    uint32_t width;       /* FRAME */
    uint32_t height;      /* FRAME */
    uint32_t visible;     /* GEOMETRY */
    int32_t rect[4];      /* GEOMETRY: x, y, width, height */
    uint32_t colorimetry; /* FRAME */
    uint32_t stride[3];   /* FRAME: bytes per row of each plane */
    uint32_t offset[3];   /* FRAME: plane offsets within the slot */
    uint32_t displayed;   /* RELEASE */
    uint32_t dropped;     /* RELEASE */
    uint32_t role;        /* HELLO: PS5WK_VROLE_*; DOPEN, HELLO_ACK: codec(s) */
    uint32_t status;      /* DOPEN_ACK, PICTURE: 0 = ok */
    uint32_t payload_bytes; /* AU: bytes that follow the message */
    int64_t target_us;    /* FRAME */
    int64_t pts_ns;       /* FRAME, AU, PICTURE */
};

/* Version 2 aliases (same storage, decoder messages):
 *   decoder  = slots   (DOPEN_ACK, FRAME with PS5WK_VFMT_REMOTE)
 *   token    = slot    (PICTURE, PRELEASE, FRAME with PS5WK_VFMT_REMOTE)
 *   epoch    = generation (AU, DFLUSH, DDRAIN, PICTURE, DRAINED)
 *   key      = stride[2] (AU, PICTURE)
 *   flags    = visible (AU)
 *   profile  = offset[0], bit_depth = offset[1] (DOPEN, PICTURE)
 *   pitch    = stride[0] (PICTURE: bytes per row) */
#define ps5wk_decoder slots
#define ps5wk_token slot
#define ps5wk_epoch generation
#define ps5wk_key stride[2]
#define ps5wk_flags visible
#define ps5wk_profile offset[0]
#define ps5wk_bit_depth offset[1]
#define ps5wk_pitch stride[0]

#ifdef __cplusplus
static_assert(sizeof(struct ps5wk_video_msg) == 128, "ps5wk_video_msg is 128 bytes");
#else
_Static_assert(sizeof(struct ps5wk_video_msg) == 128, "ps5wk_video_msg is 128 bytes");
#endif
