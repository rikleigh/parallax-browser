/* dmem.h - the libkernel memory calls Spike 10 uses, and the direct-memory
 * page source under its heap (dheap.c).
 *
 * The SDK declares none of these; the prototypes match libkernel's exports
 * (and the way PS5VR, ps5-opengl and Spike 9 call them).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* sceKernelMapDirectMemory / ReserveVirtualRange flags */
#define DMEM_MAP_FIXED        0x10
#define DMEM_MAP_NO_OVERWRITE 0x80
/* protections: CPU r/w/x in the low bits, GPU r/w in 0x10/0x20 */
#define DMEM_PROT_CPU_RW 0x03
#define DMEM_PROT_CPU_RX 0x05
#define DMEM_PROT_CPU_GPU_RW 0xf2 /* prospero-win's / ps5-xash3d's fallback */
#define DMEM_TYPE_WB 12           /* "type 0x0c": write-back, the decoder's and ps5-opengl's */
#define DMEM_PAGE 0x4000          /* 16 KiB CPU pages */
#define DMEM_UNIT 0x10000         /* direct-memory allocation unit used here (64 KiB) */

int sceKernelReserveVirtualRange(void **addr, size_t len, int flags, size_t align);
int sceKernelAllocateMainDirectMemory(size_t len, size_t align, int type, int64_t *phys);
int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align, int type,
                                  int64_t *phys);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t phys,
                             size_t align);
int sceKernelMprotect(const void *addr, size_t len, int prot);
int sceKernelMunmap(void *addr, size_t len);
int sceKernelReleaseDirectMemory(int64_t phys, size_t len);
int sceKernelAvailableFlexibleMemorySize(size_t *size);
int sceKernelConfiguredFlexibleMemorySize(size_t *size);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t align, int64_t *out_start,
                                       size_t *out_size);

/* Snapshot helpers for the log. */
size_t dmem_flexible_free(void);
size_t dmem_direct_free(void); /* largest free direct block */

/* -- the heap (dheap.c) ---------------------------------------------------- */

/* The heap's range: reserved at the first allocation (before main). */
#define DHEAP_HINT  ((uintptr_t)0x2000000000ull)
#define DHEAP_RANGE ((size_t)16 << 30)
#define DHEAP_ARENAS 8

typedef struct {
    uintptr_t base;
    size_t range;
    int init_rc;            /* 0, or the call that refused (see init_step) */
    const char *init_step;
    int prot;               /* the protection the commits use (0x03 or 0xf2) */
    int map0_rc;            /* sceKernelMapDirectMemory rc with 0x03 on the first commit */
    int alloc_main;         /* 1 AllocateMainDirectMemory, 0 AllocateDirectMemory */
    size_t mapped, peak;    /* direct memory mapped for the heap now / at most */
    unsigned segments;
    unsigned long long maps, unmaps, refused_unmaps, map_failures;
    unsigned long long libc_fallbacks; /* allocations libc served instead */
    unsigned arenas;
} dheap_stats_t;

void dheap_stats(dheap_stats_t *st);
int dheap_owns(const void *p);
/* Give fully free segments back (mspace_trim on every arena). */
void dheap_trim(void);

/* The real libc allocator, for the throughput comparison. */
void *__real_malloc(size_t size);
void __real_free(void *p);

#ifdef __cplusplus
}
#endif
