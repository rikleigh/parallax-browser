/* ptstats: can a payload read the console's page table pool? Compare with the
 * kernel log's "page table CPU n/8192 GPU n/8192" (klogsrv). Maps 512 MB in
 * between to see the numbers move (+256 expected if they are system-wide). */
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
int sceKernelGetPageTableStats(int* cpuTotal, int* cpuAvailable, int* gpuTotal, int* gpuAvailable);
static void show(const char* when)
{
    int ct = -1, ca = -1, gt = -1, ga = -1;
    int r = sceKernelGetPageTableStats(&ct, &ca, &gt, &ga);
    printf("PTSTATS %ld %s: returned %#x; CPU total %d available %d (used %d); GPU total %d available %d (used %d)\n",
        (long)time(NULL), when, r, ct, ca, ct - ca, gt, ga, gt - ga);
}
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    show("start");
    sleep(12);
    void* p = mmap(NULL, (size_t)512 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    show(p == MAP_FAILED ? "mmap failed" : "512 MB mapped");
    sleep(12);
    show("512 MB mapped, 12 s later");
    if (p != MAP_FAILED) munmap(p, (size_t)512 << 20);
    show("unmapped");
    return 0;
}
