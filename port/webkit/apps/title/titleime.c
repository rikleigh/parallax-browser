/*
 * titleime.c - load the system keyboard (libSceImeDialog) into the browser
 * title and bind its functions.
 *
 * The title imports pad and user service from libraries the system loads
 * for every title, but libSceImeDialog.sprx is not one of them: its imports
 * stay NULL even though eboot.bin lists it as needed. So it is loaded at
 * run time, as Spike 9 and the PSVR2 code load theirs:
 *
 *  1. sceSysmoduleLoadModule(0x96) (the ImeDialog id in 13.60's
 *     libSceSysmodule table, the same as on the PS4);
 *  2. sceKernelLoadStartModule(<sandbox word>/common/lib/libSceImeDialog.sprx)
 *     for a handle (returns the loaded module's handle);
 *  3. sceKernelDlsym by name, and where that is refused (it is for every
 *     system library tried so far), 13.60 export offsets from the module
 *     base, used only when an anchor string in the module's read-only data
 *     matches (ime_exports_1360.h), so other firmware fails safely.
 *
 * libSceCommonDialog (sceCommonDialogInitialize) is bound the same way; it is
 * optional.
 *
 * Title only (package.sh); WPEDisplayPS5 calls ps5wk_title_load_ime() when
 * it is linked in (a weak reference, NULL in payloads).
 */
#define SPIKE_NAME "ps5browser"
#include "common/spike.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int sceSysmoduleLoadModule(uint32_t id);
const char* sceKernelGetFsSandboxRandomWord(void);
int sceKernelLoadStartModule(const char* path, size_t argc, const void* argv, uint32_t flags, void* opt, int* res);
int sceKernelDlsym(int handle, const char* name, void** addr);

typedef struct {
    uint64_t size;
    char name[256];
    struct {
        uint64_t addr;
        uint32_t size;
        int32_t prot;
    } seg[4];
    uint32_t nseg;
    uint8_t fingerprint[20];
} module_info_t;
int sceKernelGetModuleInfo(int handle, module_info_t* info);

typedef struct {
    const char* name;
    uint32_t offset;
} export_sig_t;
typedef struct {
    uint32_t va;
    const char* text;
} export_anchor_t;
#include "ime_exports_1360.h"

/* Same layout as PS5Sce.h's PS5ImeFunctions. */
struct ps5wk_ime_functions {
    int (*init)(const void* param, const void* extended);
    int (*get_status)(void);
    int (*get_result)(void* result);
    int (*term)(void);
    int (*abort)(void);
    int (*common_dialog_initialize)(void);
};

#define SYSMODULE_IME_DIALOG 0x96
#define COUNT(a) (sizeof(a) / sizeof *(a))

static int load_module(uint32_t sysmodule, const char* name)
{
    int rc = sysmodule ? sceSysmoduleLoadModule(sysmodule) : 0;
    const char* word = sceKernelGetFsSandboxRandomWord();
    char paths[2][128];
    snprintf(paths[0], sizeof paths[0], "/%s/common/lib/%s.sprx", word ? word : "system", name);
    snprintf(paths[1], sizeof paths[1], "/system/common/lib/%s.sprx", name);
    int h = -1;
    for (int i = 0; i < 2 && h < 0; i++) {
        int res = 0;
        h = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, &res);
        spike_log("ime: sysmodule 0x%x -> 0x%08x; load %s -> 0x%x", sysmodule, (unsigned)rc, paths[i], (unsigned)h);
    }
    return h;
}

/* The module's base if its anchor string is where the table says. */
static uintptr_t module_base(int h, const char* file, const export_anchor_t* anchor)
{
    module_info_t mi;
    memset(&mi, 0, sizeof mi);
    mi.size = sizeof mi;
    if (sceKernelGetModuleInfo(h, &mi) != 0 || strcmp(mi.name, file) != 0) {
        spike_log("ime: module info for %s: name \"%s\"", file, mi.name);
        return 0;
    }
    uintptr_t base = 0;
    for (unsigned i = 0; i < mi.nseg && i < 4; i++)
        if (!base || mi.seg[i].addr < base)
            base = mi.seg[i].addr;
    const uintptr_t want = base + anchor->va;
    const size_t len = strlen(anchor->text) + 1;
    for (unsigned i = 0; i < mi.nseg && i < 4; i++) {
        const uintptr_t lo = mi.seg[i].addr, hi = lo + mi.seg[i].size;
        if (want >= lo && want + len <= hi && (mi.seg[i].prot & 1))
            return memcmp((const void*)want, anchor->text, len) == 0 ? base : 0;
    }
    return 0;
}

/* Binds each name to *slots[i]: by sceKernelDlsym, else by offset. Returns
 * how many were bound. */
static int bind_exports(int h, const char* file, const char* const* names, void** slots, size_t n,
    const export_sig_t* sigs, size_t nsigs, const export_anchor_t* anchor)
{
    int bound = 0, byName = 0;
    uintptr_t base = 0;
    int triedBase = 0;
    for (size_t i = 0; i < n; i++) {
        slots[i] = NULL;
        if (sceKernelDlsym(h, names[i], &slots[i]) == 0 && slots[i]) {
            bound++;
            byName++;
            continue;
        }
        slots[i] = NULL;
        if (!triedBase) {
            triedBase = 1;
            base = module_base(h, file, anchor);
        }
        if (!base)
            continue;
        for (size_t j = 0; j < nsigs; j++) {
            if (!strcmp(sigs[j].name, names[i])) {
                slots[i] = (void*)(base + sigs[j].offset);
                bound++;
            }
        }
    }
    spike_log("ime: %s: %d of %zu bound (%d by name, base %p%s)", file, bound, n, byName, (void*)base,
        triedBase && !base ? ", anchor does not match: not 13.60?" : "");
    return bound;
}

/* Returns 0 when init, get_status, get_result and term are all bound. */
int ps5wk_title_load_ime(struct ps5wk_ime_functions* out)
{
    memset(out, 0, sizeof *out);
    int h = load_module(SYSMODULE_IME_DIALOG, "libSceImeDialog");
    if (h >= 0) {
        static const char* const names[] = { "sceImeDialogInit", "sceImeDialogGetStatus", "sceImeDialogGetResult",
            "sceImeDialogTerm", "sceImeDialogAbort" };
        void* slots[COUNT(names)];
        bind_exports(h, "libSceImeDialog.sprx", names, slots, COUNT(names), klibSceImeDialogExports,
            COUNT(klibSceImeDialogExports), &klibSceImeDialogAnchor);
        out->init = (int (*)(const void*, const void*))slots[0];
        out->get_status = (int (*)(void))slots[1];
        out->get_result = (int (*)(void*))slots[2];
        out->term = (int (*)(void))slots[3];
        out->abort = (int (*)(void))slots[4];
    }
    int c = load_module(0, "libSceCommonDialog");
    if (c >= 0) {
        static const char* const names[] = { "sceCommonDialogInitialize" };
        void* slot = NULL;
        bind_exports(c, "libSceCommonDialog.sprx", names, &slot, 1, klibSceCommonDialogExports,
            COUNT(klibSceCommonDialogExports), &klibSceCommonDialogAnchor);
        out->common_dialog_initialize = (int (*)(void))slot;
    }
    return out->init && out->get_status && out->get_result && out->term ? 0 : -1;
}
