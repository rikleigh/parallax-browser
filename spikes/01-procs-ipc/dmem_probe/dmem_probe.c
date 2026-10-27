/* Spike 1 follow-up: why can't a payload allocate direct memory?
 * Tries sizes, memory types and the allocation entry points, logging the
 * return codes, and frees everything it gets. */
#define SPIKE_NAME "01-dmem-probe"
#include "../../common/spike.h"

#include <stdint.h>

int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align, int type,
                                  int64_t *offset);
int sceKernelAllocateMainDirectMemory(size_t len, size_t align, int type, int64_t *offset);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t offset,
                             size_t align);
int sceKernelReleaseDirectMemory(int64_t start, size_t len);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t align, int64_t *start_out,
                                       size_t *size_out);
int sceKernelMapFlexibleMemory(void **addr, size_t len, int prot, int flags);
int sceKernelReleaseFlexibleMemory(void *addr, size_t len);

static void try_alloc(const char *how, size_t len, int type, int main_api)
{
    int64_t off = -1;
    int rc = main_api ? sceKernelAllocateMainDirectMemory(len, 0x10000, type, &off)
                      : sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(), len,
                                                      0x10000, type, &off);
    int mrc = -1;
    void *va = NULL;
    if (rc == 0) {
        mrc = sceKernelMapDirectMemory(&va, len, 0x33, 0, off, 0x10000);
        if (mrc == 0)
            ((volatile uint8_t *)va)[0] = 1;
        sceKernelReleaseDirectMemory(off, len);
    }
    spike_log("  %-6s %6zu KB type %2d: alloc 0x%08x off 0x%llx map 0x%08x va %p", how, len >> 10,
              type, rc, (unsigned long long)off, mrc, va);
    if (rc == 0 && mrc == 0)
        SPIKE_PASS("dmem", "%s %zu KB type %d works", how, len >> 10, type);
}

int main(void)
{
    int64_t s = 0;
    size_t avail = 0;
    int rc = sceKernelAvailableDirectMemorySize(0, (int64_t)sceKernelGetDirectMemorySize(), 0x10000,
                                                &s, &avail);
    spike_log("direct memory: total %zu MB, available rc 0x%08x start 0x%llx size %zu MB",
              sceKernelGetDirectMemorySize() >> 20, rc, (unsigned long long)s, avail >> 20);
    static const int kTypes[] = {0, 3, 10, 12};
    static const size_t kSizes[] = {64u << 10, 1u << 20, 64u << 20};
    for (size_t t = 0; t < sizeof kTypes / sizeof *kTypes; t++)
        for (size_t z = 0; z < sizeof kSizes / sizeof *kSizes; z++) {
            try_alloc("any", kSizes[z], kTypes[t], 0);
            try_alloc("main", kSizes[z], kTypes[t], 1);
        }
    /* Budget: the largest single allocation (binary search, 64 KB steps),
     * then how much fits in total as 1 MB pieces. */
    size_t lo = 0, hi = 512u << 20;
    while (hi - lo > 0x10000) {
        size_t mid = ((lo + hi) / 2) & ~(size_t)0xffff;
        int64_t off = -1;
        if (sceKernelAllocateDirectMemory(0, 0, mid, 0x10000, 12, &off) == 0) {
            sceKernelReleaseDirectMemory(off, mid);
            lo = mid;
        } else {
            hi = mid;
        }
    }
    int64_t offs[512];
    unsigned n = 0;
    for (; n < 512; n++)
        if (sceKernelAllocateDirectMemory(0, 0, 1u << 20, 0x10000, 12, &offs[n]) != 0)
            break;
    for (unsigned i = 0; i < n; i++)
        sceKernelReleaseDirectMemory(offs[i], 1u << 20);
    SPIKE_RESULT("dmem-budget", "INFO", "largest single allocation %zu KB, total in 1 MB pieces %u MB",
                 lo >> 10, n);
    void *fx = NULL;
    rc = sceKernelMapFlexibleMemory(&fx, 1u << 20, 3, 0);
    spike_log("flexible map 1 MB: 0x%08x %p", rc, fx);
    if (rc == 0)
        sceKernelReleaseFlexibleMemory(fx, 1u << 20);
    spike_log("done");
    return 0;
}
