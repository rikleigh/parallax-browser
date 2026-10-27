/* au - access units out of the containers Spike 9 feeds the decoder.
 *
 *   h264, hevc  raw Annex-B elementary streams, split into access units at
 *               the boundaries H.264 7.4.1.2.3 / HEVC 7.4.2.4.4 define
 *   ivf         VP9 in IVF; superframes split into their frames
 *   mp4         fragmented MP4 (moov + moof/mdat pairs, what MSE gets):
 *               avc1/avc3, hvc1/hev1 or vp09; length-prefixed NAL units are
 *               rewritten to Annex-B and the avcC/hvcC parameter sets put in
 *               front of keyframes that don't carry their own (what
 *               GStreamer's h264parse/h265parse do before a byte-stream
 *               decoder)
 *
 * The whole file is in memory; each call writes one AU into the caller's
 * buffer (the decoder's input slot), so every source costs one copy.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define AU_MAX_SAMPLES 4096

enum { AU_H264 = 1, AU_HEVC, AU_VP9 };

typedef struct {
    const uint8_t *data;
    size_t size, pos;
    int codec;     /* AU_* */
    int container; /* 0 annexb, 1 ivf, 2 mp4 */
    /* mp4 */
    int width, height;    /* from the sample entry */
    int nal_len;          /* length prefix bytes */
    uint8_t ps[2048];     /* parameter sets, Annex-B */
    size_t ps_len;
    uint32_t sample_off[AU_MAX_SAMPLES], sample_size[AU_MAX_SAMPLES];
    int nsamples, sample;
    size_t first_moof;
    /* vp9 superframes */
    const uint8_t *sub[8];
    uint32_t sub_size[8];
    int nsub, isub;
    /* stats */
    int fragments;
} au_src_t;

/* kind: "h264", "hevc", "ivf" or "mp4". 0 on success. */
int au_open(au_src_t *s, const uint8_t *data, size_t size, const char *kind);
/* Next AU into dst. Returns its size, 0 at the end, -1 on a parse error or
 * if it doesn't fit. *key: a random-access point. *hidden: a VP9 frame that
 * is decoded but not shown (show_frame = 0). */
long au_next(au_src_t *s, uint8_t *dst, size_t cap, int *key, int *hidden);
void au_rewind(au_src_t *s);
