/* psvr2 - see psvr2.h. Derived from PS5VR's app/src/app_vr.c
 * (GPL-3.0-or-later); structure offsets and call sequences are PS5VR's
 * findings on firmware 13.60. */
#include "psvr2.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* -- system library entry points ---------------------------------------- */
typedef struct { uint64_t size, align; } size_align_t;

#define HMD2_FUNCS(X)                                                                         \
    X(int, sceHmd2Initialize, (const void *param))                                           \
    X(int, sceHmd2Open, (int user_id, int type, int index, const void *param))                \
    X(int, sceHmd2GetDeviceInformation, (void *info))                                         \
    X(int, sceHmd2GetFieldOfViewWithoutHandle, (void *fov))                                   \
    X(size_align_t, sceHmd2ReprojectionQueryBufferSizeAlign, (void))                          \
    X(size_align_t, sceHmd2ReprojectionQueryDisplayBufferSizeAlign, (void))                   \
    X(int, sceHmd2ReprojectionInitialize, (const void *param, const void *reserved))          \
    X(int, sceHmd2ReprojectionEnableVrMode, (uint64_t mode))                                  \
    X(int, sceHmd2ReprojectionDisableVrMode, (void))                                          \
    X(int, sceHmd2ReprojectionBeginFrame, (uint64_t frame))                                   \
    X(int, sceHmd2ReprojectionGetPredictedDisplayTime, (uint64_t frame, uint64_t *time))      \
    X(int, sceHmd2ReprojectionSetParam, (uint32_t buffer, uint64_t frame, const void *param,  \
                                         const void *reserved))                               \
    X(int, sceHmd2ReprojectionSetRenderConfig, (uint64_t half_rate, uint64_t r0, uint64_t r1)) \
    X(int, sceHmd2ReprojectionTerminate, (void))                                              \
    X(int, sceHmd2Close, (int handle))                                                        \
    X(int, sceHmd2Terminate, (void))
#define VRTRACKER2_FUNCS(X)                                                                   \
    X(int, sceVrTracker2QueryMemory, (const void *param, void *out))                          \
    X(int, sceVrTracker2Initialize, (const void *param))                                      \
    X(int, sceVrTracker2RegisterDevice, (int type, int handle))                               \
    X(int, sceVrTracker2GetResult, (const void *param, void *result))                         \
    X(int, sceVrTracker2UnregisterDevice, (int handle))                                       \
    X(int, sceVrTracker2Finalize, (void))
#define DECLARE_PTR(ret, name, args) static ret(*name) args;
HMD2_FUNCS(DECLARE_PTR)
VRTRACKER2_FUNCS(DECLARE_PTR)

#define SYSMODULE_HMD2       0x12a
#define SYSMODULE_VRTRACKER2 0x12b
#define VR_MODE_120HZ        0x2000f
#define VR_QUEUE_FULL        ((int)0x8A72001A)
#define VO_BUSY              ((int)0x80290009)
#define FRAME_US             16667 /* 120 Hz output, half-rate rendering */

int sceSysmoduleLoadModule(uint32_t id);
const char *sceKernelGetFsSandboxRandomWord(void);
int sceKernelLoadStartModule(const char *path, size_t argc, const void *argv, uint32_t flags,
                             void *opt, int *res);
int sceKernelDlsym(int handle, const char *name, void **addr);
int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align, int type,
                                  int64_t *offset);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t offset,
                             size_t align);
int sceKernelMunmap(void *addr, size_t len);
int sceKernelReleaseDirectMemory(int64_t start, size_t len);
int sceKernelUsleep(unsigned usec);
uint64_t sceKernelGetProcessTime(void);

typedef struct { void *data, *metadata, *reserved0, *reserved1; } vo_buffer_t;
typedef struct { uint8_t reserved[80]; } vo_attribute_t;
int sceVideoOutOpen(int user_id, int bus_type, int index, const void *param);
int sceVideoOutClose(int handle);
void sceVideoOutSetBufferAttribute2(void *attr, uint64_t format, uint32_t tiling, uint32_t width,
                                    uint32_t height, uint64_t option, uint32_t reserved0,
                                    uint64_t reserved1);
int sceVideoOutRegisterBuffers2(int handle, int set_index, int start_index, void *buffers,
                                int count, void *attr, int category, void *option);
int sceVideoOutUnregisterBuffers(int handle, int set_index);
int sceVideoOutSubmitFlip(int handle, int index, int flip_mode, int64_t flip_arg);
int sceVideoOutConfigureOutput(int handle, uint64_t mode, const void *options, const void *r0,
                               uint64_t r1);

/* -- logging ------------------------------------------------------------- */
static psvr2_log_fn s_log;

void psvr2_set_logger(psvr2_log_fn fn)
{
    s_log = fn;
}

__attribute__((format(printf, 1, 2))) static void logf_(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (s_log)
        s_log(line);
    else
        printf("%s\n", line);
}

/* -- binding by name, then by per-firmware export offset ------------------ */
typedef struct { const char *name; uint32_t offset; } export_sig_t;
typedef struct { uint32_t va; const char *text; } export_anchor_t;
#include "exports/vr_exports_1360.h"
/* More firmware: add exports/vr_exports_<fw>.h (tools/gen_export_table.py)
 * and a row in kTables below. Tables are tried until an anchor matches. */

typedef struct {
    const export_sig_t *hmd2;
    size_t hmd2_n;
    const export_anchor_t *hmd2_anchor;
    const export_sig_t *tracker;
    size_t tracker_n;
    const export_anchor_t *tracker_anchor;
} fw_table_t;
#define COUNT(a) (sizeof(a) / sizeof *(a))
static const fw_table_t kTables[] = {
    {klibSceHmd2Exports, COUNT(klibSceHmd2Exports), &klibSceHmd2Anchor, klibSceVrTracker2Exports,
     COUNT(klibSceVrTracker2Exports), &klibSceVrTracker2Anchor},
};

typedef struct { const char *name; void **slot; } slot_t;
#define SLOT(ret, name, args) {#name, (void **)&name},
static slot_t kHmd2Slots[] = {HMD2_FUNCS(SLOT)};
static slot_t kTrackerSlots[] = {VRTRACKER2_FUNCS(SLOT)};

typedef struct {
    uint64_t size;
    char name[256];
    struct { uint64_t addr; uint32_t size; int32_t prot; } seg[4];
    uint32_t nseg;
    uint8_t fingerprint[20];
} module_info_t;
int sceKernelGetModuleInfo(int handle, module_info_t *info);

static int load_sysmodule(uint32_t id, const char *name)
{
    int rc = sceSysmoduleLoadModule(id);
    /* A title sees system libraries under its sandbox's random-word
     * directory; an elfldr payload runs with the real root. */
    const char *word = sceKernelGetFsSandboxRandomWord();
    char paths[2][128];
    snprintf(paths[0], sizeof paths[0], "/%s/common/lib/%s.sprx", word ? word : "system", name);
    snprintf(paths[1], sizeof paths[1], "/system/common/lib/%s.sprx", name);
    int h = -1;
    for (int i = 0; i < 2 && h < 0; i++) {
        int res = 0;
        h = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, &res);
        logf_("psvr2: sysmodule 0x%x -> 0x%08x; load %s -> 0x%x", id, rc, paths[i], h);
    }
    return h;
}

static uintptr_t module_base(int h, const char *file, const export_anchor_t *anchor)
{
    module_info_t mi;
    memset(&mi, 0, sizeof mi);
    mi.size = sizeof mi;
    if (sceKernelGetModuleInfo(h, &mi) != 0 || strcmp(mi.name, file) != 0)
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

static int resolve(int h, const char *file, slot_t *slots, size_t n, int tracker)
{
    int ok = 1;
    uintptr_t base = 0;
    const fw_table_t *table = NULL;
    for (size_t i = 0; i < n; i++) {
        if (sceKernelDlsym(h, slots[i].name, slots[i].slot) == 0 && *slots[i].slot)
            continue;
        if (!table) {
            for (size_t t = 0; t < COUNT(kTables) && !base; t++) {
                base = module_base(h, file, tracker ? kTables[t].tracker_anchor
                                                    : kTables[t].hmd2_anchor);
                if (base)
                    table = &kTables[t];
            }
            logf_("psvr2: %s by export offsets, base %p%s", file, (void *)base,
                  base ? "" : " (no table matches this firmware)");
            if (!table)
                return -1;
        }
        const export_sig_t *sig = tracker ? table->tracker : table->hmd2;
        const size_t sig_n = tracker ? table->tracker_n : table->hmd2_n;
        *slots[i].slot = NULL;
        for (size_t j = 0; j < sig_n; j++)
            if (!strcmp(sig[j].name, slots[i].name))
                *slots[i].slot = (void *)(base + sig[j].offset);
        if (!*slots[i].slot) {
            logf_("psvr2: missing %s", slots[i].name);
            ok = 0;
        }
    }
    return ok ? 0 : -1;
}

void *psvr2_direct_alloc(uint64_t size, uint64_t align)
{
    size = (size + 0xffff) & ~(uint64_t)0xffff;
    if (align < 0x10000)
        align = 0x10000;
    int64_t off = 0;
    void *va = NULL;
    if (sceKernelAllocateDirectMemory(0, (int64_t)16 << 30, size, align, 12 /* WB_ONION */, &off) != 0 ||
        sceKernelMapDirectMemory(&va, size, 0x33, 0, off, align) != 0) {
        logf_("psvr2: direct alloc 0x%llx failed", (unsigned long long)size);
        return NULL;
    }
    memset(va, 0, size);
    return va;
}

/* Direct memory psvr2_open takes for the tracker and the reprojection: given
 * back in psvr2_close (a title that enters and leaves VR many times would
 * otherwise lose ~130 MB each time). */
#define MAX_OWNED 4
static struct { void *va; int64_t off; uint64_t size; } s_owned[MAX_OWNED];

static void *owned_alloc(uint64_t size, uint64_t align)
{
    int slot = -1;
    for (int i = 0; i < MAX_OWNED && slot < 0; i++)
        if (!s_owned[i].va)
            slot = i;
    if (slot < 0)
        return NULL;
    size = (size + 0xffff) & ~(uint64_t)0xffff;
    if (align < 0x10000)
        align = 0x10000;
    int64_t off = 0;
    void *va = NULL;
    if (sceKernelAllocateDirectMemory(0, (int64_t)16 << 30, size, align, 12 /* WB_ONION */, &off) != 0)
        return NULL;
    if (sceKernelMapDirectMemory(&va, size, 0x33, 0, off, align) != 0) {
        sceKernelReleaseDirectMemory(off, size);
        logf_("psvr2: direct alloc 0x%llx failed", (unsigned long long)size);
        return NULL;
    }
    memset(va, 0, size);
    s_owned[slot].va = va;
    s_owned[slot].off = off;
    s_owned[slot].size = size;
    return va;
}

static uint64_t owned_free_all(void)
{
    uint64_t bytes = 0;
    for (int i = 0; i < MAX_OWNED; i++) {
        if (!s_owned[i].va)
            continue;
        sceKernelMunmap(s_owned[i].va, s_owned[i].size);
        sceKernelReleaseDirectMemory(s_owned[i].off, s_owned[i].size);
        bytes += s_owned[i].size;
        s_owned[i].va = NULL;
    }
    return bytes;
}

/* -- state ---------------------------------------------------------------- */
typedef struct { void *layers; uint32_t count; uint32_t pad; } layer_set_t;

static struct {
    int preloaded, open, active;
    int hmd_init, tracker_init, tracker_registered, reprojection_init;
    int hmd, vh;
    float fov[4];
    uint32_t eye_w, eye_h;
    int count;
    uint8_t layers[PSVR2_MAX_BUFFERS][0x130] __attribute__((aligned(16)));
    layer_set_t sets[PSVR2_MAX_BUFFERS];
    uint64_t frame, next_begin;
    unsigned submits, errors;
    int require_headset, no_headset;
    uint32_t dev_status;
    int dev_worn, dev_lens;
} g = {.hmd = -1, .vh = -1};

void psvr2_require_headset(int on)
{
    g.require_headset = on != 0;
}

int psvr2_device_status(uint32_t *status, int *worn, int *lens_mm)
{
    int rc = 0;
    if (g.open) {
        uint8_t info[64];
        memset(info, 0, sizeof info);
        rc = sceHmd2GetDeviceInformation(info);
        if (rc == 0) {
            g.dev_status = *(uint32_t *)info;
            g.dev_worn = info[20];
            g.dev_lens = info[21];
        }
    }
    if (status)
        *status = g.dev_status;
    if (worn)
        *worn = g.dev_worn;
    if (lens_mm)
        *lens_mm = g.dev_lens;
    return rc;
}

int psvr2_preload(void)
{
    if (g.preloaded)
        return g.preloaded > 0 ? 0 : -1;
    g.preloaded = -1;
    int h = load_sysmodule(SYSMODULE_HMD2, "libSceHmd2");
    if (h < 0 || resolve(h, "libSceHmd2.sprx", kHmd2Slots, COUNT(kHmd2Slots), 0) != 0)
        return -1;
    h = load_sysmodule(SYSMODULE_VRTRACKER2, "libSceVrTracker2");
    if (h < 0 || resolve(h, "libSceVrTracker2.sprx", kTrackerSlots, COUNT(kTrackerSlots), 1) != 0)
        return -1;
    g.preloaded = 1;
    return 0;
}

static void teardown(void);

/* Every failure after sceHmd2Initialize undoes what was set up (teardown),
 * so a failed open (no headset) leaves nothing behind. */
static int open_(int user_id)
{
    uint8_t init[16] = {0};
    int rc = sceHmd2Initialize(init);
    logf_("psvr2: sceHmd2Initialize -> 0x%08x", rc);
    if (rc != 0)
        return -1;
    g.hmd_init = 1;
    float fov[16];
    memset(fov, 0, sizeof fov);
    rc = sceHmd2GetFieldOfViewWithoutHandle(fov);
    memcpy(g.fov, fov, sizeof g.fov);
    logf_("psvr2: FOV rc 0x%08x out %.4f in %.4f up %.4f down %.4f", rc, (double)fov[0],
          (double)fov[1], (double)fov[2], (double)fov[3]);
    if (rc != 0 || g.fov[0] <= 0.0f || g.fov[2] <= 0.0f)
        return -1;
    /* A payload is not an application of user_id, and Hmd2 may refuse it
     * (0x8a720009 "invalid userId"); try the system user ids too. */
    const int kUsers[] = {user_id, 0xff, 0xfe, 0};
    for (size_t u = 0; u < COUNT(kUsers); u++) {
        g.hmd = sceHmd2Open(kUsers[u], 0, 0, NULL);
        logf_("psvr2: sceHmd2Open(user 0x%x) -> 0x%08x", kUsers[u], g.hmd);
        if (g.hmd >= 0)
            break;
    }
    uint8_t info[64];
    memset(info, 0, sizeof info);
    rc = sceHmd2GetDeviceInformation(info);
    g.dev_status = *(uint32_t *)info;
    g.dev_worn = info[20];
    g.dev_lens = info[21];
    logf_("psvr2: sceHmd2Open -> 0x%08x; status %u worn %u lens %u mm", g.hmd, *(uint32_t *)info,
          info[20], info[21]);
    if (g.hmd < 0)
        return -1;
    /* Without a headset Hmd2 still opens (status 2, lens 0 mm), and so do the
     * tracker, the reprojection and VR mode: the title then sat in VR mode
     * with nothing to show it in and the pad gone to the system. Connected,
     * worn or not, reads status 0. */
    if (g.require_headset && (rc != 0 || g.dev_status != 0)) {
        logf_("psvr2: no PS VR2 headset (device information 0x%08x, status %u): not entering VR", rc,
              g.dev_status);
        g.no_headset = 1;
        return -1;
    }

    uint32_t qparam[8] = {0x20, 0};
    uint32_t qout[8] = {0};
    if (sceVrTracker2QueryMemory(qparam, qout) != 0)
        return -1;
    logf_("psvr2: tracker wants %u KB direct memory (align 0x%x)", qout[1] >> 10, qout[2]);
    void *twork = owned_alloc(qout[1], qout[2]);
    if (!twork)
        return -1;
    uint8_t ip[0x48];
    memset(ip, 0, sizeof ip);
    *(uint32_t *)(ip + 0x00) = 0x48;
    *(int32_t *)(ip + 0x08) = 700;
    *(int32_t *)(ip + 0x0c) = 700;
    *(uint64_t *)(ip + 0x10) = 0x1f00;
    *(uint64_t *)(ip + 0x18) = 0x1f00;
    *(void **)(ip + 0x20) = twork;
    *(uint32_t *)(ip + 0x28) = qout[1];
    *(uint32_t *)(ip + 0x2c) = qout[2];
    rc = sceVrTracker2Initialize(ip);
    g.tracker_init = rc == 0;
    int rc2 = rc == 0 ? sceVrTracker2RegisterDevice(0, g.hmd) : -1;
    g.tracker_registered = rc2 == 0;
    logf_("psvr2: tracker init 0x%08x register 0x%08x", rc, rc2);
    if (rc != 0 || rc2 != 0)
        return -1;

    size_align_t wb = sceHmd2ReprojectionQueryBufferSizeAlign();
    size_align_t db = sceHmd2ReprojectionQueryDisplayBufferSizeAlign();
    logf_("psvr2: reprojection wants %llu KB work + %llu KB display buffer",
          (unsigned long long)(wb.size >> 10), (unsigned long long)(db.size >> 10));
    void *work = owned_alloc(wb.size, 0x10000);
    void *disp = owned_alloc(db.size, 0x10000);
    if (!work || !disp)
        return -1;
    uint8_t rp[72];
    memset(rp, 0, sizeof rp);
    *(void **)(rp + 0x00) = work;
    *(void **)(rp + 0x08) = disp;
    *(uint32_t *)(rp + 0x10) = 256;
    *(int32_t *)(rp + 0x14) = 0x1f00;
    *(uint32_t *)(rp + 0x18) = 1;
    rc = sceHmd2ReprojectionInitialize(rp, NULL);
    logf_("psvr2: sceHmd2ReprojectionInitialize -> 0x%08x", rc);
    if (rc != 0)
        return -1;
    g.reprojection_init = 1;
    return 0;
}

int psvr2_open(int user_id)
{
    if (g.open)
        return 0;
    if (psvr2_preload() != 0)
        return -1;
    g.no_headset = 0;
    if (open_(user_id) != 0) {
        teardown();
        return g.no_headset ? PSVR2_NO_HEADSET : -1;
    }
    g.open = 1;
    return 0;
}

int psvr2_probe(int user_id, uint32_t *status, int *worn)
{
    *status = 0;
    *worn = 0;
    if (g.open || g.hmd_init)
        return -1;
    if (psvr2_preload() != 0)
        return -2;
    uint8_t init[16] = {0};
    int rc = sceHmd2Initialize(init);
    if (rc != 0) {
        logf_("psvr2: probe: sceHmd2Initialize -> 0x%08x", rc);
        return rc;
    }
    int h = sceHmd2Open(user_id, 0, 0, NULL);
    uint8_t info[64];
    memset(info, 0, sizeof info);
    int rc2 = sceHmd2GetDeviceInformation(info);
    *status = *(uint32_t *)info;
    *worn = info[20];
    int rc3 = h >= 0 ? sceHmd2Close(h) : 0;
    int rc4 = sceHmd2Terminate();
    logf_("psvr2: probe: open 0x%08x, device info 0x%08x (status %u worn %u lens %u mm), close 0x%08x, terminate 0x%08x",
          h, rc2, *status, info[20], info[21], rc3, rc4);
    return h >= 0 ? 0 : h;
}

void psvr2_fov(float tan[4])
{
    memcpy(tan, g.fov, sizeof g.fov);
}

void psvr2_eye_tangents(int eye, float overscan, float tan[4])
{
    /* Left eye: out is to the left. Right eye: in is to the left. */
    tan[0] = (eye ? g.fov[1] : g.fov[0]) * overscan;
    tan[1] = (eye ? g.fov[0] : g.fov[1]) * overscan;
    tan[2] = g.fov[2] * overscan;
    tan[3] = g.fov[3] * overscan;
}

int psvr2_worn(void)
{
    uint8_t info[64];
    memset(info, 0, sizeof info);
    return g.open && sceHmd2GetDeviceInformation(info) == 0 ? info[20] != 0 : 0;
}

int psvr2_start(const uint32_t tsharp[][8], int count, uint32_t eye_w, uint32_t eye_h)
{
    if (!g.open || g.active || count < 1 || count > PSVR2_MAX_BUFFERS)
        return -1;
    g.eye_w = eye_w;
    g.eye_h = eye_h;
    g.count = count;
    int rc = sceHmd2ReprojectionEnableVrMode(VR_MODE_120HZ);
    g.vh = sceVideoOutOpen(0xff, 32, 0, NULL); /* bus 32: Hmd2's VR port */
    logf_("psvr2: EnableVrMode -> 0x%08x, VR port 0x%08x", rc, g.vh);
    if (g.vh < 0) {
        sceHmd2ReprojectionDisableVrMode();
        return -1;
    }
    vo_buffer_t bufs[PSVR2_MAX_BUFFERS];
    memset(bufs, 0, sizeof bufs);
    memset(g.layers, 0, sizeof g.layers);
    for (int b = 0; b < count; b++) {
        *(uint32_t *)g.layers[b] = 0x10; /* the same texture for both eyes */
        memcpy(g.layers[b] + 0x04, tsharp[b], 32);
        memcpy(g.layers[b] + 0x24, tsharp[b], 32);
        g.sets[b].layers = g.layers[b];
        g.sets[b].count = 1;
        bufs[b].data = &g.sets[b];
    }
    vo_attribute_t attr;
    memset(&attr, 0, sizeof attr);
    sceVideoOutSetBufferAttribute2(&attr, 0x8000000000000000ull, 0, 2 * eye_w, eye_h, 0, 0, 0);
    rc = sceVideoOutRegisterBuffers2(g.vh, 0, 0, bufs, count, &attr, 0x10, NULL);
    logf_("psvr2: RegisterBuffers2 -> 0x%08x", rc);
    if (rc != 0) {
        sceVideoOutClose(g.vh);
        g.vh = -1;
        sceHmd2ReprojectionDisableVrMode();
        return -1;
    }
    int c1 = sceVideoOutConfigureOutput(g.vh, VR_MODE_120HZ, NULL, NULL, 0);
    int c2 = sceHmd2ReprojectionSetRenderConfig(1, 0, 0);
    logf_("psvr2: 120 Hz output 0x%08x, half-rate render config 0x%08x", c1, c2);
    g.frame = 0;
    g.next_begin = 0;
    g.submits = g.errors = 0;
    g.active = 1;
    return 0;
}

/* Tracker result: +4 tracked; device pose at +0x60 (position) / +0x70
 * (quaternion); eye quaternions at +0xb0 / +0xf0 (PS5VR). The eye blocks
 * mirror the device block's layout, so +0xa0 / +0xe0 are read as eye
 * positions and accepted only if they look like a plausible IPD. */
static int pose(uint64_t time, psvr2_frame_t *f)
{
    static uint8_t res[0x400];
    uint8_t gp[0x20];
    memset(gp, 0, sizeof gp);
    *(uint32_t *)(gp + 0x00) = 0x20;
    *(int32_t *)(gp + 0x04) = g.hmd;
    *(uint64_t *)(gp + 0x10) = time;
    memset(res, 0, sizeof res);
    if (sceVrTracker2GetResult(gp, res) != 0)
        return 0;
    memcpy(f->head.pos, res + 0x60, 12);
    memcpy(f->head.quat, res + 0x70, 16);
    memcpy(f->eye[0].quat, res + 0xb0, 16);
    memcpy(f->eye[1].quat, res + 0xf0, 16);
    memcpy(f->eye[0].pos, res + 0xa0, 12);
    memcpy(f->eye[1].pos, res + 0xe0, 12);
    float d2 = 0;
    for (int i = 0; i < 3; i++) {
        const float d = f->eye[0].pos[i] - f->eye[1].pos[i];
        d2 += d * d;
    }
    f->eye_pos_valid = d2 > 0.045f * 0.045f && d2 < 0.08f * 0.08f;
    return *(uint32_t *)(res + 4) != 0;
}

int psvr2_begin_frame(psvr2_frame_t *f)
{
    if (!g.active)
        return -1;
    /* A fixed 60 Hz schedule (the compositor shows each frame twice). */
    uint64_t now = sceKernelGetProcessTime();
    if (g.next_begin > now && g.next_begin - now <= FRAME_US) {
        sceKernelUsleep((unsigned)(g.next_begin - now));
        now = g.next_begin;
    }
    g.next_begin = (g.next_begin && now < g.next_begin + FRAME_US) ? g.next_begin + FRAME_US
                                                                   : now + FRAME_US;
    const uint64_t frame = g.frame + 1;
    int rc = sceHmd2ReprojectionBeginFrame(frame);
    for (int tries = 0; rc == VR_QUEUE_FULL && tries < 40; tries++) {
        sceKernelUsleep(1000);
        rc = sceHmd2ReprojectionBeginFrame(frame);
    }
    if (rc != 0) {
        if (g.errors++ < 20)
            logf_("psvr2: BeginFrame(%llu) -> 0x%08x", (unsigned long long)frame, rc);
        return -1;
    }
    g.frame = frame;
    memset(f, 0, sizeof *f);
    f->frame = frame;
    f->head.quat[3] = f->eye[0].quat[3] = f->eye[1].quat[3] = 1.0f;
    sceHmd2ReprojectionGetPredictedDisplayTime(frame, &f->display_time);
    f->tracked = pose(f->display_time, f);
    return 0;
}

int psvr2_submit(int index, const psvr2_frame_t *f, const float tan[2][4])
{
    if (!g.active || index < 0 || index >= g.count)
        return -1;
    static uint8_t lp[0x148] __attribute__((aligned(16)));
    struct { void *layers; int32_t count; int32_t pad; void *ext; } param = {lp, 1, 0, NULL};
    const uint32_t ew = g.eye_w, eh = g.eye_h, tw = 2 * ew, th = eh;
    memset(lp, 0, sizeof lp);
    uint32_t *u = (uint32_t *)lp;
    u[0x00 / 4] = 0x10;
    u[0x08 / 4] = tw, u[0x0c / 4] = th, u[0x10 / 4] = tw, u[0x14 / 4] = th;
    u[0x18 / 4] = 0x92, u[0x1c / 4] = 0x00fff000, u[0x20 / 4] = 0x05500000; /* sampler */
    u[0x28 / 4] = ew, u[0x2c / 4] = eh;                                     /* left half */
    u[0x38 / 4] = ew, u[0x3c / 4] = eh, u[0x40 / 4] = ew;                   /* right half */
    memcpy(lp + 0x48, tan[0], 16);
    memcpy(lp + 0x58, tan[1], 16);
    memcpy(lp + 0x68, f->head.pos, 12);
    memcpy(lp + 0x74, f->head.quat, 16);
    *(uint64_t *)(lp + 0x88) = f->display_time;
    *(uint64_t *)(lp + 0x90) = f->frame;
    u[0x98 / 4] = 100;
    u[0xa0 / 4] = ew, u[0xa4 / 4] = eh, u[0xa8 / 4] = ew, u[0xac / 4] = eh;
    int r1 = sceHmd2ReprojectionSetParam((uint32_t)index, f->frame, &param, NULL);
    int r2 = sceVideoOutSubmitFlip(g.vh, index, 2, (int64_t)f->frame);
    if ((r1 || r2) && g.errors++ < 20)
        logf_("psvr2: frame %llu setparam 0x%08x flip 0x%08x", (unsigned long long)f->frame, r1, r2);
    g.submits++;
    return r1 || r2 ? -1 : 0;
}

void psvr2_stop(void)
{
    if (!g.active)
        return;
    g.active = 0;
    /* Leave VR mode first: the reprojection keeps sampling the last buffer. */
    int rc3 = sceHmd2ReprojectionDisableVrMode();
    int rc = 0;
    for (int i = 0; i < 50; i++) {
        rc = sceVideoOutUnregisterBuffers(g.vh, 0);
        if (rc != VO_BUSY)
            break;
        sceKernelUsleep(10000);
    }
    int rc2 = sceVideoOutClose(g.vh);
    g.vh = -1;
    logf_("psvr2: stopped after %u frames (unregister 0x%08x close 0x%08x disable 0x%08x)",
          g.submits, rc, rc2, rc3);
}

/* Must run before exit: the tracker library's destructor faults on a live
 * tracker (PS5VR). Undoes whatever open_() got to, in reverse order. */
static void teardown(void)
{
    int r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0;
    if (g.tracker_registered)
        r1 = sceVrTracker2UnregisterDevice(g.hmd);
    if (g.tracker_init)
        r2 = sceVrTracker2Finalize();
    if (g.reprojection_init)
        r3 = sceHmd2ReprojectionTerminate();
    if (g.hmd >= 0)
        r4 = sceHmd2Close(g.hmd);
    if (g.hmd_init)
        r5 = sceHmd2Terminate();
    const int was = g.hmd_init;
    g.tracker_registered = g.tracker_init = g.reprojection_init = g.hmd_init = 0;
    g.hmd = -1;
    g.open = 0;
    const uint64_t freed = owned_free_all();
    if (was)
        logf_("psvr2: closed (tracker 0x%08x 0x%08x, reprojection 0x%08x, hmd 0x%08x 0x%08x; %llu MB direct memory released)",
              r1, r2, r3, r4, r5, (unsigned long long)(freed >> 20));
}

void psvr2_close(void)
{
    psvr2_stop();
    teardown();
}

/* -- leaving the title -------------------------------------------------------- */
/* Returning from main leaves the title to the system, which kills it
 * (0xa0d0c00f, a "debug error" when the user closes it). PS5VR asks ShellUI to
 * close the app instead. libSceSystemService exports this from a library the
 * import table cannot name, so it is bound like the VR functions. */
int psvr2_exit_app(void)
{
    static const char file[] = "libSceSystemService.sprx";
    const char *word = sceKernelGetFsSandboxRandomWord();
    char paths[2][128];
    snprintf(paths[0], sizeof paths[0], "/%s/common/lib/%s", word ? word : "system", file);
    snprintf(paths[1], sizeof paths[1], "/system/common/lib/%s", file);
    int h = -1;
    for (int i = 0; i < 2 && h < 0; i++) {
        int res = 0;
        h = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, &res);
    }
    int (*exit_app)(void) = NULL;
    if (h >= 0 && (sceKernelDlsym(h, "sceShellCoreUtilExitApp", (void **)&exit_app) != 0 ||
                   !exit_app)) {
        const uintptr_t base = module_base(h, file, &klibSceSystemServiceAnchor);
        exit_app = base ? (int (*)(void))(base + klibSceSystemServiceExports[0].offset) : NULL;
    }
    logf_("psvr2: %s -> 0x%x, sceShellCoreUtilExitApp %p", file, h, (void *)exit_app);
    return exit_app ? exit_app() : -1;
}

/* -- T# --------------------------------------------------------------------- */
enum {
    GFX10_FORMAT_8_8_8_8_UNORM = 56,
    SQ_SEL_X = 4, SQ_SEL_Y = 5, SQ_SEL_Z = 6, SQ_SEL_W = 7,
    SQ_RSRC_IMG_2D = 9,
};

/* PS5VR's eng_agc_writer.c build_tsharp_2d_internal, RGBA8 only. */
int psvr2_build_tsharp_rgba8(uint32_t out[8], uint64_t gpu_address, uint32_t width,
                             uint32_t height, uint32_t pitch_bytes, uint32_t sw_mode)
{
    const uint32_t bpp = 4;
    const uint64_t align = sw_mode ? 0xffffu : 0xffu;
    if (!out || !gpu_address || (gpu_address & align) || !width || !height || width > 16384u ||
        height > 16384u || pitch_bytes < width * bpp || pitch_bytes % bpp)
        return -1;
    const uint32_t wm1 = width - 1u, pm1 = pitch_bytes / bpp - 1u;
    memset(out, 0, 8 * sizeof(uint32_t));
    out[0] = (uint32_t)(gpu_address >> 8);
    out[1] = (uint32_t)(gpu_address >> 40) | (GFX10_FORMAT_8_8_8_8_UNORM << 20) | ((wm1 & 3u) << 30);
    out[2] = ((wm1 >> 2) & 0xfffu) | ((height - 1u) << 14);
    out[3] = SQ_SEL_X | (SQ_SEL_Y << 3) | (SQ_SEL_Z << 6) | (SQ_SEL_W << 9) |
             ((uint32_t)SQ_RSRC_IMG_2D << 28) | (sw_mode << 20);
    if (!sw_mode && pitch_bytes / bpp != width)
        out[4] = (pm1 & 0x1fffu) | (((pm1 >> 13) & 1u) << 13);
    out[5] = 7u << 20;
    return 0;
}
