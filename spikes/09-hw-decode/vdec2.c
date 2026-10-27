/* vdec2 - see vdec2.h. Derived from PS5VR's eng_vdec_native.c and
 * sce_videodec2.h (GPL-3.0-or-later). */
#include "vdec2.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -- ABI (PS5VR sce_videodec2.h) ------------------------------------------ */
typedef struct {
    uint64_t size;
    uint32_t resource_type; /* 1 = compute queue */
    uint32_t codec_type;
    uint32_t profile;
    uint32_t max_level;
    int32_t max_width;
    int32_t max_height;
    int32_t max_dpb_frames; /* -1 = decoder sizes its own DPB */
    uint32_t pipeline_depth;
    uint64_t compute_queue;
    uint64_t cpu_affinity;
    int32_t cpu_priority;
    uint32_t optimize_progressive;
    uint32_t check_memory_type;
    uint32_t reserved;
} cfg_info_t;

typedef struct {
    uint64_t size;
    uint64_t cpu_size; /* flexible memory, prot 0x03 */
    void *cpu;
    uint64_t gpu_size; /* direct memory */
    void *gpu;
    uint64_t cpu_gpu_size;
    void *cpu_gpu;
    uint64_t max_frame_size; /* one output picture */
    uint32_t frame_alignment;
    uint32_t reserved;
} mem_info_t;

typedef struct {
    uint64_t size;
    uint16_t pipe_id, queue_id;
    uint8_t check_memory_type, reserved0;
    uint16_t reserved1;
} compute_cfg_t;

typedef struct {
    uint64_t size;
    uint64_t cpu_gpu_size;
    void *cpu_gpu;
} compute_mem_t;

typedef struct {
    uint64_t size;
    void *au;
    uint64_t au_size;
    uint64_t pts, dts, attached;
} input_t;

typedef struct {
    uint64_t size;
    void *buffer;
    uint64_t buffer_size;
    uint32_t accepted, reserved;
} frame_t;

typedef struct {
    uint64_t size;
    uint8_t valid, error, picture_count, padding;
    uint32_t codec, width, pitch, height, reserved;
    void *buffer;
    uint64_t buffer_size;
    uint32_t frame_format, pitch_bytes;
} output_t;

#define VDEC2_FUNCS(X)                                                                        \
    X(int32_t, sceVideodec2QueryComputeMemoryInfo, (compute_mem_t *))                        \
    X(int32_t, sceVideodec2AllocateComputeQueue, (const compute_cfg_t *, const compute_mem_t *, \
                                                  void **))                                  \
    X(int32_t, sceVideodec2ReleaseComputeQueue, (void *))                                    \
    X(int32_t, sceVideodec2QueryDecoderMemoryInfo, (const cfg_info_t *, mem_info_t *))       \
    X(int32_t, sceVideodec2CreateDecoder, (const cfg_info_t *, const mem_info_t *, void **))  \
    X(int32_t, sceVideodec2DeleteDecoder, (void *))                                          \
    X(int32_t, sceVideodec2Reset, (void *))                                                  \
    X(int32_t, sceVideodec2Decode, (void *, input_t *, frame_t *, output_t *))               \
    X(int32_t, sceVideodec2Flush, (void *, frame_t *, output_t *))
#define DECLARE_PTR(ret, name, args) static ret(*name) args;
VDEC2_FUNCS(DECLARE_PTR)

#define SYSMODULE_VIDEODEC2 0xCF

int sceSysmoduleLoadModule(uint32_t id);
const char *sceKernelGetFsSandboxRandomWord(void);
int sceKernelLoadStartModule(const char *path, size_t argc, const void *argv, uint32_t flags,
                             void *opt, int *res);
int sceKernelDlsym(int handle, const char *name, void **addr);
int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align, int type,
                                  int64_t *offset);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t offset,
                             size_t align);
int sceKernelReleaseDirectMemory(int64_t start, size_t len);
int sceKernelMapNamedFlexibleMemory(void **addr, size_t len, int prot, int flags, const char *name);
int sceKernelReleaseFlexibleMemory(void *addr, size_t len);
int sceKernelMunmap(void *addr, size_t len);

/* -- logging ---------------------------------------------------------------- */
static vdec2_log_fn s_log;
void vdec2_set_logger(vdec2_log_fn fn)
{
    s_log = fn;
}

__attribute__((format(printf, 1, 2))) static void logf_(const char *fmt, ...)
{
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (s_log)
        s_log(line);
    else
        printf("%s\n", line);
}

/* -- loading: by name, then by export offset ------------------------------- */
typedef struct { const char *name; uint32_t offset; } export_sig_t;
typedef struct { uint32_t va; const char *text; } export_anchor_t;
#include "exports/videodec2_exports_1360.h"
#define COUNT(a) (sizeof(a) / sizeof *(a))

typedef struct {
    uint64_t size;
    char name[256];
    struct { uint64_t addr; uint32_t size; int32_t prot; } seg[4];
    uint32_t nseg;
    uint8_t fingerprint[20];
} module_info_t;
int sceKernelGetModuleInfo(int handle, module_info_t *info);

typedef struct { const char *name; void **slot; } slot_t;
#define SLOT(ret, name, args) {#name, (void **)&name},
static slot_t kSlots[] = {VDEC2_FUNCS(SLOT)};

static char s_report[512];
static int s_loaded;

static uintptr_t module_base_if_anchor(int h, const export_anchor_t *anchor)
{
    module_info_t mi;
    memset(&mi, 0, sizeof mi);
    mi.size = sizeof mi;
    if (sceKernelGetModuleInfo(h, &mi) != 0)
        return 0;
    uintptr_t base = 0;
    for (unsigned i = 0; i < mi.nseg && i < 4; i++)
        if (!base || mi.seg[i].addr < base)
            base = mi.seg[i].addr;
    const uintptr_t want = base + anchor->va;
    const size_t len = strlen(anchor->text) + 1;
    for (unsigned i = 0; i < mi.nseg && i < 4; i++) {
        const uintptr_t lo = mi.seg[i].addr, hi = lo + mi.seg[i].size;
        if (want >= lo && want + len <= hi && (mi.seg[i].prot & 1))
            return memcmp((const void *)want, anchor->text, len) == 0 ? base : 0;
    }
    return 0;
}

int vdec2_load(void)
{
    if (s_loaded)
        return 0;
    const int sm = sceSysmoduleLoadModule(SYSMODULE_VIDEODEC2);
    const char *word = sceKernelGetFsSandboxRandomWord();
    char paths[2][128];
    snprintf(paths[0], sizeof paths[0], "/%s/common/lib/libSceVideodec2.sprx", word ? word : "system");
    snprintf(paths[1], sizeof paths[1], "/system/common/lib/libSceVideodec2.sprx");
    int h = -1, which = -1;
    for (int i = 0; i < 2 && h < 0; i++) {
        int res = 0;
        h = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, &res);
        which = i;
    }
    int by_name = 0, by_offset = 0, missing = 0;
    uintptr_t base = 0;
    for (size_t i = 0; i < COUNT(kSlots); i++) {
        *kSlots[i].slot = NULL;
        if (h >= 0 && sceKernelDlsym(h, kSlots[i].name, kSlots[i].slot) == 0 && *kSlots[i].slot) {
            by_name++;
            continue;
        }
        if (!base && h >= 0)
            base = module_base_if_anchor(h, &klibSceVideodec2Anchor);
        for (size_t j = 0; base && j < COUNT(klibSceVideodec2Exports); j++)
            if (!strcmp(klibSceVideodec2Exports[j].name, kSlots[i].name))
                *kSlots[i].slot = (void *)(base + klibSceVideodec2Exports[j].offset);
        if (*kSlots[i].slot)
            by_offset++;
        else
            missing++;
    }
    snprintf(s_report, sizeof s_report,
             "sysmodule 0x%x -> 0x%08x; load %s -> 0x%x; %d bound by name, %d by 13.60 offsets "
             "(anchor %s), %d missing",
             SYSMODULE_VIDEODEC2, (unsigned)sm, paths[which], (unsigned)h, by_name, by_offset,
             base ? "matched" : "not used/no match", missing);
    if (missing)
        return h < 0 ? h : -1;
    s_loaded = 1;
    return 0;
}

const char *vdec2_load_report(void)
{
    return s_report[0] ? s_report : "vdec2_load not called";
}

/* -- memory ------------------------------------------------------------------ */
typedef struct { void *addr; int64_t start; size_t size; } dmem_t;

static int dmem_alloc(dmem_t *d, size_t size, size_t align)
{
    d->addr = NULL;
    d->start = -1;
    d->size = size;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, align, 12,
                                           &d->start);
    if (rc == 0)
        rc = sceKernelMapDirectMemory(&d->addr, size, 0x33, 0, d->start, align);
    return rc;
}

static void dmem_free(dmem_t *d)
{
    if (d->addr)
        sceKernelMunmap(d->addr, d->size);
    if (d->start >= 0)
        sceKernelReleaseDirectMemory(d->start, d->size);
    d->addr = NULL;
    d->start = -1;
}

void *vdec2_direct_alloc(size_t size, size_t align)
{
    dmem_t d;
    size = (size + align - 1) & ~(align - 1);
    return dmem_alloc(&d, size, align) == 0 ? d.addr : NULL;
}

static size_t align16k(uint64_t v)
{
    return (size_t)((v + 0x3fff) & ~(uint64_t)0x3fff);
}

/* -- decoder ----------------------------------------------------------------- */
#define INPUT_SLOTS 4
#define MAX_SLOTS 16

struct vdec2 {
    void *queue, *decoder;
    dmem_t compute, gpu, cpu_gpu, input, frames;
    void *cpu;
    size_t cpu_size;
    cfg_info_t cfg;
    mem_info_t mem;
    size_t frame_size;
    int nslots;
    void *slot[MAX_SLOTS];
    unsigned in_ring, out_ring;
};

/* A level that covers the picture: AVC level x10, HEVC general_level_idc
 * (x30), VP9 x10. PS5VR: 4K HEVC needs 153 (5.1); above 8,912,896 luma
 * samples (5.x's limit) it is 183 (6.1). */
static uint32_t level_for(uint32_t codec, int w, int h)
{
    const int64_t px = (int64_t)w * h;
    if (codec == VDEC2_CODEC_AVC)
        return px <= 2228224 ? 41 : px <= 8912896 ? 52 : 62;
    if (codec == VDEC2_CODEC_HEVC)
        return px <= 2228224 ? 123 : px <= 8912896 ? 153 : 183;
    return px <= 2228224 ? 41 : px <= 8912896 ? 51 : 61;
}

static void picture_from(const output_t *o, vdec2_picture_t *p, int slot)
{
    memset(p, 0, sizeof *p);
    p->valid = o->valid && o->picture_count;
    p->error = o->error;
    p->width = o->width;
    p->height = o->height;
    p->pitch = o->pitch;
    p->pitch_bytes = o->pitch_bytes ? o->pitch_bytes : o->pitch;
    p->frame_format = o->frame_format;
    p->buffer = o->buffer;
    p->buffer_size = o->buffer_size;
    p->slot = slot;
}

vdec2_t *vdec2_open(const vdec2_config_t *c, vdec2_info_t *info, const char **stage, int *rc_out)
{
    int rc = -1;
    *stage = "load";
    if (!s_loaded && vdec2_load() != 0) {
        *rc_out = -1;
        return NULL;
    }
    vdec2_t *v = calloc(1, sizeof *v);
    v->compute.start = v->gpu.start = v->cpu_gpu.start = v->input.start = v->frames.start = -1;
    memset(info, 0, sizeof *info);

#define STEP(name, call)            \
    do {                            \
        *stage = (name);            \
        if ((rc = (call)) != 0)     \
            goto fail;              \
    } while (0)

    compute_mem_t cm = {.size = sizeof cm};
    compute_cfg_t cc = {.size = sizeof cc};
    STEP("QueryComputeMemoryInfo", sceVideodec2QueryComputeMemoryInfo(&cm));
    info->compute_bytes = cm.cpu_gpu_size;
    STEP("alloc(compute)", dmem_alloc(&v->compute, align16k(cm.cpu_gpu_size), 0x4000));
    cm.cpu_gpu = v->compute.addr;
    cm.cpu_gpu_size = v->compute.size;
    STEP("AllocateComputeQueue", sceVideodec2AllocateComputeQueue(&cc, &cm, &v->queue));

    const int w = (c->width + 63) & ~63, h = (c->height + 63) & ~63;
    cfg_info_t *cfg = &v->cfg;
    cfg->size = sizeof *cfg;
    cfg->resource_type = 1;
    cfg->codec_type = c->codec;
    cfg->profile = c->profile;
    cfg->max_level = c->level ? c->level : level_for(c->codec, w, h);
    cfg->max_width = w;
    cfg->max_height = h;
    cfg->max_dpb_frames = -1;
    cfg->pipeline_depth = (uint32_t)(c->pipeline_depth > 0 ? c->pipeline_depth : 4);
    cfg->compute_queue = (uint64_t)(uintptr_t)v->queue;
    cfg->cpu_affinity = 0x3f;
    cfg->cpu_priority = 700;
    cfg->optimize_progressive = 1;
    info->level = cfg->max_level;

    mem_info_t *mem = &v->mem;
    mem->size = sizeof *mem;
    STEP("QueryDecoderMemoryInfo", sceVideodec2QueryDecoderMemoryInfo(cfg, mem));
    info->cpu_bytes = mem->cpu_size;
    info->gpu_bytes = mem->gpu_size;
    info->cpu_gpu_bytes = mem->cpu_gpu_size;
    info->frame_bytes = mem->max_frame_size;
    info->frame_alignment = mem->frame_alignment;

    v->cpu_size = align16k(mem->cpu_size);
    STEP("MapNamedFlexibleMemory",
         sceKernelMapNamedFlexibleMemory(&mem->cpu, v->cpu_size, 0x03, 0, "Spike09Vdec2"));
    v->cpu = mem->cpu;
    STEP("alloc(gpu)", dmem_alloc(&v->gpu, align16k(mem->gpu_size), 0x4000));
    mem->gpu = v->gpu.addr;
    mem->gpu_size = v->gpu.size;
    if (mem->cpu_gpu_size) {
        STEP("alloc(cpu_gpu)", dmem_alloc(&v->cpu_gpu, align16k(mem->cpu_gpu_size), 0x4000));
        mem->cpu_gpu = v->cpu_gpu.addr;
        mem->cpu_gpu_size = v->cpu_gpu.size;
    }
    STEP("alloc(input)", dmem_alloc(&v->input, (size_t)VDEC2_INPUT_SLOT_BYTES * INPUT_SLOTS, 0x4000));
    v->frame_size = align16k(mem->max_frame_size);
    v->nslots = c->frame_slots > 0 ? c->frame_slots : 8;
    if (v->nslots > MAX_SLOTS)
        v->nslots = MAX_SLOTS;
    if (c->external_slots) {
        for (int i = 0; i < v->nslots; i++)
            v->slot[i] = c->external_slots[i];
    } else {
        STEP("alloc(frames)", dmem_alloc(&v->frames, v->frame_size * (size_t)v->nslots, 0x10000));
        for (int i = 0; i < v->nslots; i++)
            v->slot[i] = (uint8_t *)v->frames.addr + (size_t)i * v->frame_size;
    }
    STEP("CreateDecoder", sceVideodec2CreateDecoder(cfg, mem, &v->decoder));
    STEP("Reset", sceVideodec2Reset(v->decoder));
    *stage = "ok";
    *rc_out = 0;
    return v;
fail:
    logf_("vdec2: bring-up %ux%u codec %u failed at %s rc=0x%08x", (unsigned)c->width,
          (unsigned)c->height, (unsigned)c->codec, *stage, (unsigned)rc);
    *rc_out = rc;
    vdec2_close(v);
    return NULL;
#undef STEP
}

size_t vdec2_frame_size(const vdec2_t *v)
{
    return v->frame_size;
}

int vdec2_decode(vdec2_t *v, const void *au, size_t size, uint64_t pts, vdec2_picture_t *pic)
{
    memset(pic, 0, sizeof *pic);
    if (size > VDEC2_INPUT_SLOT_BYTES)
        return -1;
    uint8_t *in_mem = (uint8_t *)v->input.addr + (size_t)(v->in_ring++ % INPUT_SLOTS) * VDEC2_INPUT_SLOT_BYTES;
    if (au != in_mem)
        memcpy(in_mem, au, size);
    const int slot = (int)(v->out_ring++ % (unsigned)v->nslots);
    input_t in = {.size = sizeof in, .au = in_mem, .au_size = size, .pts = pts, .dts = UINT64_MAX};
    frame_t fb = {.size = sizeof fb, .buffer = v->slot[slot], .buffer_size = v->frame_size};
    output_t out = {.size = sizeof out};
    const int rc = sceVideodec2Decode(v->decoder, &in, &fb, &out);
    picture_from(&out, pic, slot);
    return rc;
}

void *vdec2_input_slot(vdec2_t *v)
{
    return (uint8_t *)v->input.addr + (size_t)(v->in_ring % INPUT_SLOTS) * VDEC2_INPUT_SLOT_BYTES;
}

int vdec2_flush(vdec2_t *v, vdec2_picture_t *pic)
{
    const int slot = (int)(v->out_ring++ % (unsigned)v->nslots);
    frame_t fb = {.size = sizeof fb, .buffer = v->slot[slot], .buffer_size = v->frame_size};
    output_t out = {.size = sizeof out};
    const int rc = sceVideodec2Flush(v->decoder, &fb, &out);
    picture_from(&out, pic, slot);
    return rc;
}

int vdec2_renew(vdec2_t *v)
{
    if (v->decoder)
        sceVideodec2DeleteDecoder(v->decoder);
    v->decoder = NULL;
    mem_info_t mem = v->mem;
    int rc = sceVideodec2CreateDecoder(&v->cfg, &mem, &v->decoder);
    return rc ? rc : sceVideodec2Reset(v->decoder);
}

void vdec2_close(vdec2_t *v)
{
    if (!v)
        return;
    if (v->decoder)
        sceVideodec2DeleteDecoder(v->decoder);
    dmem_free(&v->frames);
    dmem_free(&v->input);
    dmem_free(&v->cpu_gpu);
    dmem_free(&v->gpu);
    if (v->cpu) {
        sceKernelReleaseFlexibleMemory(v->cpu, v->cpu_size);
        sceKernelMunmap(v->cpu, v->cpu_size);
    }
    if (v->queue)
        sceVideodec2ReleaseComputeQueue(v->queue);
    dmem_free(&v->compute);
    free(v);
}
