/* rwxprobe: what does a bigger JIT pool cost a payload? JSC reserves its
 * executable pool as one anonymous RWX mapping (OSAllocatorPOSIX: mmap
 * PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANON) and touches pages as
 * it fills them. For each size: map it, hold it untouched, touch the first
 * 48 MB (what a heavy page's JIT code uses), hold, unmap. The output names
 * the phase and the time (time()), to be matched with the kernel log's
 * "VM Stats ... page table CPU n/8192" lines (klogsrv).
 *   prospero-clang -O2 -o rwxprobe.elf rwxprobe.c   (env.sh, then the SDK's prospero.sh)
 *   cat rwxprobe.elf | socat -t 400 - TCP:$PS5_HOST:$PS5_PORT   (output comes back) */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MB ((size_t)1 << 20)
#define HOLD 20

static void phase(const char *what, size_t size)
{
    printf("RWXPROBE %ld %s %zu MB\n", (long)time(NULL), what, size / MB);
    fflush(stdout);
}

int main(void)
{
    static const size_t sizes[] = { 128 * MB, 256 * MB, 512 * MB, 1024 * MB };
    setvbuf(stdout, NULL, _IOLBF, 0);
    phase("baseline", 0);
    sleep(HOLD);
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        size_t size = sizes[i];
        unsigned char *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) {
            phase("mmap-failed", size);
            continue;
        }
        phase("mapped-untouched", size);
        sleep(HOLD);
        for (size_t o = 0; o < 48 * MB; o += 16384)
            p[o] = 0xc3; /* ret */
        phase("touched-48MB", size);
        sleep(HOLD);
        munmap(p, size);
        phase("unmapped", size);
        sleep(HOLD);
    }
    phase("done", 0);
    return 0;
}
