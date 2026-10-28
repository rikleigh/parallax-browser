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
 *   HELLO     magic, version, pid
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
 */

#include <stdint.h>

#define PS5WK_VIDEO_SOCKET_FORMAT "/system_tmp/ps5wk/video-%d.sock" /* %d = UIProcess pid */
#define PS5WK_VIDEO_MAGIC 0x56355350u /* "PS5V" */
#define PS5WK_VIDEO_VERSION 1
#define PS5WK_VIDEO_MAX_SLOTS 16

enum {
    PS5WK_VMSG_HELLO = 1,
    PS5WK_VMSG_SHM = 2,
    PS5WK_VMSG_FRAME = 3,
    PS5WK_VMSG_GEOMETRY = 4,
    PS5WK_VMSG_FLUSH = 5,
    PS5WK_VMSG_RELEASE = 16,
};

enum {
    PS5WK_VFMT_I420 = 1, /* Y, then U, then V (4:2:0) */
    PS5WK_VFMT_NV12 = 2, /* Y, then interleaved UV (4:2:0) */
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
    uint32_t reserved[3];
    int64_t target_us;    /* FRAME */
    int64_t pts_ns;       /* FRAME */
};

#ifdef __cplusplus
static_assert(sizeof(struct ps5wk_video_msg) == 128, "ps5wk_video_msg is 128 bytes");
#else
_Static_assert(sizeof(struct ps5wk_video_msg) == 128, "ps5wk_video_msg is 128 bytes");
#endif
