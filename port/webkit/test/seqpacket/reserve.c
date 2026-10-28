/* reserve: what does an address-space reservation cost a PS5 process?
 * Compares sceKernelAvailableFlexibleMemorySize before/after mmap of 512 MB
 * RW anon (what WTF/libpas do) vs PROT_NONE, then mprotect+touch 16 MB. */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
int sceKernelAvailableFlexibleMemorySize(size_t *);
static size_t avail(void) { size_t s = 0; sceKernelAvailableFlexibleMemorySize(&s); return s >> 20; }
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    size_t a0 = avail();
    void *rw = mmap(NULL, 512u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    size_t a1 = avail();
    void *none = mmap(NULL, 512u << 20, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    size_t a2 = avail();
    int mp = none != MAP_FAILED ? mprotect(none, 16u << 20, PROT_READ | PROT_WRITE) : -1;
    size_t a3 = avail();
    if (!mp) memset(none, 1, 16u << 20);
    size_t a4 = avail();
    if (rw != MAP_FAILED) memset(rw, 1, 16u << 20);
    size_t a5 = avail();
    printf("RESULT webkit-reserve INFO avail %zu MB; after 512 MB RW mmap %zu (%s); after 512 MB PROT_NONE %zu (%s); mprotect 16 MB RW %d -> %zu; touched %zu; touched 16 MB of RW %zu\n",
           a0, a1, rw == MAP_FAILED ? "failed" : "ok", a2, none == MAP_FAILED ? "failed" : "ok", mp, a3, a4, a5);
    return 0;
}
