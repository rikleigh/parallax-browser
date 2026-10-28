/* memprobe: which calls give a payload's anonymous memory back to the PS5
 * kernel? libpas decommits by madvise (FreeBSD: MADV_FREE) or by mapping
 * fresh pages over the range; on PS5 pages are 16 KiB. For each method,
 * on a 64 MB anonymous mapping: touch every page, give the range (or a
 * part) back, and read the system's free flexible memory before and after
 * (sceKernelAvailableFlexibleMemorySize), plus whether the contents read
 * back as zero and whether the pages can be touched again.
 *   prospero-clang -O2 -o memprobe.elf memprobe.c   (env.sh, then the SDK's prospero.sh)
 *   cat memprobe.elf | socat -t 20 - TCP:$PS5_HOST:$PS5_PORT   (output comes back) */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

int sceKernelAvailableFlexibleMemorySize(size_t *size);

#define MB (1024.0 * 1024.0)
#define SIZE ((size_t)64 << 20)

static double flexible(void)
{
    size_t free = 0;
    sceKernelAvailableFlexibleMemorySize(&free);
    return free / MB;
}

/* The system's free pages (vm.stats.vm.v_free_count), MB; -1 if refused. */
static double systemFree(void)
{
    unsigned count = 0;
    size_t len = sizeof count;
    if (sysctlbyname("vm.stats.vm.v_free_count", &count, &len, NULL, 0))
        return -1;
    return count * (double)getpagesize() / MB;
}

/* Resident MB of [p, p + n) (mincore), -1 if it fails. */
static double resident(char *p, size_t n)
{
    static unsigned char vec[SIZE / 4096];
    if (mincore(p, n, (char *)vec))
        return -1;
    size_t pages = 0, page = getpagesize();
    for (size_t i = 0; i < n / page; i++)
        pages += vec[i] & 1;
    return pages * (double)page / MB;
}

static void touch(char *p, size_t n)
{
    for (size_t i = 0; i < n; i += 4096)
        p[i] = (char)(1 + (i >> 12));
}

static int zeroes(const char *p, size_t n)
{
    for (size_t i = 0; i < n; i += 4096)
        if (p[i])
            return 0;
    return 1;
}

enum { M_FREE, M_DONTNEED, M_FIXED, M_MUNMAP, M_PROTNONE, M_FREE_4K };
static const char *names[] = { "madvise(MADV_FREE)", "madvise(MADV_DONTNEED)", "mmap(MAP_FIXED) over", "munmap", "mprotect(PROT_NONE)",
                               "madvise(MADV_FREE) on 4 KiB granules" };

static void probe(int method)
{
    double f0 = flexible(), s0 = systemFree();
    char *p = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        printf("%s: mmap failed (errno %d)\n", names[method], errno);
        return;
    }
    double f1 = flexible();
    touch(p, SIZE);
    double f2 = flexible(), s2 = systemFree(), r2 = resident(p, SIZE);
    /* Give back the middle half, 16 KiB aligned (what libpas would). */
    char *q = p + SIZE / 4;
    size_t n = SIZE / 2;
    int rc = 0;
    switch (method) {
    case M_FREE: rc = madvise(q, n, MADV_FREE); break;
    case M_DONTNEED: rc = madvise(q, n, MADV_DONTNEED); break;
    case M_FIXED: rc = mmap(q, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == q ? 0 : -1; break;
    case M_MUNMAP: rc = munmap(q, n); break;
    case M_PROTNONE: rc = mprotect(q, n, PROT_NONE); break;
    case M_FREE_4K:
        /* every other 4 KiB granule: what 0006 had to avoid */
        for (size_t i = 0; i < n && !rc; i += 8192)
            rc = madvise(q + i, 4096, MADV_FREE);
        break;
    }
    int err = rc ? errno : 0;
    double f3 = flexible();
    /* MADV_FREE frees lazily on some kernels: a little later, and after
     * other memory is taken. */
    usleep(200 * 1000);
    double f4 = flexible(), s4 = systemFree();
    double r4 = method == M_MUNMAP ? -1 : resident(q, n);
    int zero = -1;
    double f5 = f4;
    if (method == M_PROTNONE)
        mprotect(q, n, PROT_READ | PROT_WRITE);
    if (method != M_MUNMAP) {
        zero = zeroes(q, n);
        touch(q, n);
        f5 = flexible();
    }
    printf("%-38s rc %d errno %d | flexible free MB: before %.1f, mapped %+.1f, touched %+.1f, given back %+.1f (+0.2 s %+.1f) of 32 MB, "
           "contents %s, touched again %+.1f\n",
        names[method], rc, err, f0, f1 - f0, f2 - f1, f3 - f2, f4 - f2,
        zero < 0 ? "-" : zero ? "zero" : "kept", f5 - f4);
    printf("    system free MB %.1f -> touched %+.1f -> given back %+.1f; resident: all %.1f MB after touching, the given-back 32 MB: %.1f MB after\n",
        s0, s2 - s0, s4 - s2, r2, r4);
    if (method == M_MUNMAP) {
        munmap(p, SIZE / 4);
        munmap(p + SIZE * 3 / 4, SIZE / 4);
    } else
        munmap(p, SIZE);
    printf("    after munmap of all: %+.1f vs before\n", flexible() - f0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("memprobe: page size %d, flexible free %.1f MB\n", getpagesize(), flexible());
    for (int m = M_FREE; m <= M_FREE_4K; m++)
        probe(m);
    printf("RESULT webkit-memprobe PASS done\n");
    return 0;
}
