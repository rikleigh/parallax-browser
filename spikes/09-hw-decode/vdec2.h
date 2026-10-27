/* vdec2 - Sony's hardware video decoder (libSceVideodec2) for the spikes.
 *
 * libSceVideodec2 is the low-level decoder games use: it takes one access
 * unit at a time (Annex-B H.264/HEVC, or one VP9 frame) and writes the
 * decoded picture as linear NV12 (P010 for 10-bit) into a frame buffer the
 * caller owns, in GPU-visible direct memory. That is the shape a GStreamer
 * decoder element needs.
 *
 * The SDK has no stubs for it, so it is loaded at run time: sysmodule 0xCF,
 * then each function by name (sceKernelDlsym), falling back to per-firmware
 * export offsets (exports/videodec2_exports_<fw>.h, tools/gen_export_table.py)
 * guarded by an anchor string, the way spikes/common/psvr2 binds Hmd2.
 *
 * ABI (struct layouts, call sequence, memory types): PS5VR's
 * app/engine/media/include/sce/sce_videodec2.h and eng_vdec_native.c
 * (GPL-3.0-or-later), which took them from ProsperoLight. This file is
 * derived from those.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* codec_type values: the service's own tags, not sequential. */
#define VDEC2_CODEC_AVC  1u
#define VDEC2_CODEC_HEVC 974921u  /* 0x000EE049 */
#define VDEC2_CODEC_VP9  2382845u /* 0x00245BFD */

typedef void (*vdec2_log_fn)(const char *line);
void vdec2_set_logger(vdec2_log_fn fn);

/* Load the module and bind its functions. Call it early in a title: PS5VR
 * found sceSysmoduleLoadModule(0xCF) fails (ESDKVERSION) after the etaHEN
 * unjail swaps credentials. Returns 0, or the first failing rc. */
int vdec2_load(void);
/* What vdec2_load did, for the log (sysmodule rc, handle, how bound). */
const char *vdec2_load_report(void);

typedef struct {
    uint32_t codec;     /* VDEC2_CODEC_* */
    uint32_t profile;   /* AVC profile_idc (100 High); HEVC 1 Main, 2 Main10; VP9 0 or 2 */
    uint32_t level;     /* 0 = derived from the size */
    int width, height;  /* largest coded picture (padded to 64 here) */
    int frame_slots;    /* output ring; 0 = 8 */
    int pipeline_depth; /* 0 = 4 (PS5VR's value for 8-bit) */
    /* Optional: caller-owned output slots (e.g. a GL buffer's mapping), each
     * at least the decoder's max_frame_size. NULL = allocate direct memory. */
    void *const *external_slots;
} vdec2_config_t;

typedef struct {
    /* sizes the decoder asked for */
    uint64_t compute_bytes, cpu_bytes, gpu_bytes, cpu_gpu_bytes, frame_bytes;
    uint32_t frame_alignment;
    uint32_t level;
} vdec2_info_t;

typedef struct {
    int valid;          /* a picture came out of this call */
    int error;          /* the decoder flagged the picture */
    uint32_t width, height;     /* luma; height is the coded (padded) height */
    uint32_t pitch;             /* luma row pitch in samples */
    uint32_t pitch_bytes;       /* row pitch in bytes (2 x pitch for P010) */
    uint32_t frame_format;
    void *buffer;               /* Y plane; UV follows at pitch_bytes * height */
    uint64_t buffer_size;
    int slot;                   /* which output slot holds it */
} vdec2_picture_t;

typedef struct vdec2 vdec2_t;

/* Bring up a decoder (compute queue, memory, decoder, reset). On failure
 * returns NULL with the failing step and rc in *stage / *rc. */
vdec2_t *vdec2_open(const vdec2_config_t *cfg, vdec2_info_t *info, const char **stage, int *rc);
/* Decode one access unit (copied into the decoder's input ring first).
 * Returns the call's rc; *pic says whether a picture came out. */
int vdec2_decode(vdec2_t *v, const void *au, size_t size, uint64_t pts, vdec2_picture_t *pic);
/* Drain one buffered picture (end of stream). rc 0 and pic->valid 0: empty. */
int vdec2_flush(vdec2_t *v, vdec2_picture_t *pic);
/* A fresh decoder object on the same memory (what a seek needs, per PS5VR). */
int vdec2_renew(vdec2_t *v);
void vdec2_close(vdec2_t *v);
/* Bytes per output slot (max_frame_size, 16 KiB aligned). */
size_t vdec2_frame_size(const vdec2_t *v);
/* The input ring slot the next vdec2_decode uses: build the AU there and
 * pass this pointer to skip the copy. */
void *vdec2_input_slot(vdec2_t *v);
/* Input ring slot size: the largest access unit vdec2_decode accepts. */
#define VDEC2_INPUT_SLOT_BYTES (8u << 20)

/* The bring-up step in progress (for a watchdog), and whether vdec2_open
 * logs each step as it enters it. */
extern volatile const char *vdec2_stage;
extern int vdec2_verbose;

/* GPU-visible direct memory (WB onion, CPU r/w), as the decoder and
 * ps5-opengl both allocate it. NULL on failure. */
void *vdec2_direct_alloc(size_t size, size_t align);

#ifdef __cplusplus
}
#endif
