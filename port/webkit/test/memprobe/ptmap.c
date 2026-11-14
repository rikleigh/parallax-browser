/* ptmap: which process's mappings account for the console's CPU page tables?
 * For every process: its VM map (kern.proc.vmmap), the number of distinct
 * 2 MB slots its entries cover, and its large entries. */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
int sceKernelGetPageTableStats(int*, int*, int*, int*);
#define SLOT 21
int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    int ct = -1, ca = -1, gt = -1, ga = -1;
    int r = sceKernelGetPageTableStats(&ct, &ca, &gt, &ga);
    printf("PTMAP stats: returned %#x; CPU used %d of %d; GPU used %d of %d; sizeof kinfo_vmentry %zu kinfo_proc %zu\n",
        r, ct - ca, ct, gt - ga, gt, sizeof(struct kinfo_vmentry), sizeof(struct kinfo_proc));
    long totalSlots = 0;
    static char buf[4 << 20];
    for (int pid = 1; pid < 3000; pid++) {
        int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, pid };
        size_t len = sizeof(buf);
        if (sysctl(mib, 4, buf, &len, NULL, 0) < 0) {
            if (errno != ESRCH) printf("PTMAP pid %d: vmmap errno %d\n", pid, errno);
            continue;
        }
        char name[64] = "?";
        struct kinfo_proc kp; size_t kl = sizeof(kp);
        int mib2[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };
        if (!sysctl(mib2, 4, &kp, &kl, NULL, 0)) snprintf(name, sizeof(name), "%s", kp.ki_comm);
        long slots = 0, slotsNone = 0, entries = 0; uint64_t bytes = 0, bytesNone = 0, lastSlot = ~0ull;
        int firstSize = 0;
        for (char* p = buf; p < buf + len;) {
            struct kinfo_vmentry* e = (struct kinfo_vmentry*)p;
            if (e->kve_structsize <= 0) break;
            if (!firstSize) firstSize = e->kve_structsize;
            entries++;
            uint64_t s = e->kve_start, en = e->kve_end;
            bytes += en - s;
            int none = !(e->kve_protection & 7);
            if (none) bytesNone += en - s;
            uint64_t a = s >> SLOT, b = (en - 1) >> SLOT;
            if (a == lastSlot) a++;
            if (b >= a || a == lastSlot + 1) {
                if (b >= a) { slots += b - a + 1; if (none) slotsNone += b - a + 1; lastSlot = b; }
            }
            p += e->kve_structsize;
        }
        totalSlots += slots;
        printf("PTMAP pid %d %s: %ld entries (structsize %d), %llu MB mapped (%llu MB no-access), %ld slots (%ld from no-access entries)\n",
            pid, name, entries, firstSize, (unsigned long long)(bytes >> 20), (unsigned long long)(bytesNone >> 20), slots, slotsNone);
        if (slots >= 64) {
            /* slots by entry size class and by protection */
            static const uint64_t lim[] = { 2u << 20, 8u << 20, 64u << 20, 512u << 20, ~0ull };
            long cls[5][2] = { { 0 } }; uint64_t last = ~0ull;
            for (char* p = buf; p < buf + len;) {
                struct kinfo_vmentry* e = (struct kinfo_vmentry*)p;
                if (e->kve_structsize <= 0) break;
                uint64_t sz = e->kve_end - e->kve_start, a = e->kve_start >> SLOT, b = (e->kve_end - 1) >> SLOT;
                int c = 0; while (sz >= lim[c]) c++;
                if (a == last) a++;
                if (b >= a) { cls[c][!(e->kve_protection & 7)] += b - a + 1; last = b; }
                p += e->kve_structsize;
            }
            printf("PTMAP  pid %d slots by entry size (accessible/no-access): <2MB %ld/%ld, 2-8MB %ld/%ld, 8-64MB %ld/%ld, 64-512MB %ld/%ld, >=512MB %ld/%ld\n", pid,
                cls[0][0], cls[0][1], cls[1][0], cls[1][1], cls[2][0], cls[2][1], cls[3][0], cls[3][1], cls[4][0], cls[4][1]);
        }
        if (slots >= 64)
            for (char* p = buf; p < buf + len;) {
                struct kinfo_vmentry* e = (struct kinfo_vmentry*)p;
                if (e->kve_structsize <= 0) break;
                uint64_t sz = e->kve_end - e->kve_start;
                if (sz >= (16u << 20))
                    printf("PTMAP   %d %010llx-%010llx %6llu MB prot %d type %d flags %#x %.*s\n", pid,
                        (unsigned long long)e->kve_start, (unsigned long long)e->kve_end, (unsigned long long)(sz >> 20),
                        e->kve_protection, e->kve_type, e->kve_flags,
                        (int)(e->kve_structsize - offsetof(struct kinfo_vmentry, kve_path)), e->kve_path);
                p += e->kve_structsize;
            }
    }
    printf("PTMAP total slots over all processes: %ld (CPU page tables used: %d)\n", totalSlots, ct - ca);
    fflush(stdout);
    return 0;
}
