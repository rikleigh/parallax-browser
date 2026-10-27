/* Spike 0 - environment baseline.
 *
 * Sent through elfldr; prints what the later spikes depend on: firmware,
 * process credentials, memory budgets, and whether the PS VR2 system
 * libraries PS5VR binds to are present on this console.
 */
#define SPIKE_NAME "00-baseline"
#include "../common/spike.h"

#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

#include <ps5/kernel.h>

typedef struct {
    size_t size;
    char version_string[0x1c];
    uint32_t version;
} sw_version_t;
int sceKernelGetSystemSwVersion(sw_version_t *);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableFlexibleMemorySize(size_t *);
const char *sceKernelGetFsSandboxRandomWord(void);

static void report_file(const char *test, const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0)
        SPIKE_PASS(test, "%s (%lld bytes)", path, (long long)st.st_size);
    else
        SPIKE_FAIL(test, "%s missing", path);
}

int main(void)
{
    spike_log("ps5webkit spike 00: baseline");

    sw_version_t sw;
    memset(&sw, 0, sizeof sw);
    sw.size = sizeof sw;
    if (sceKernelGetSystemSwVersion(&sw) == 0)
        SPIKE_PASS("firmware", "%s (0x%08x), kernel says 0x%08x", sw.version_string, sw.version,
                   kernel_get_fw_version());
    else
        SPIKE_FAIL("firmware", "sceKernelGetSystemSwVersion failed; kernel says 0x%08x",
                   kernel_get_fw_version());

    const pid_t pid = getpid();
    uint8_t caps[16];
    kernel_get_ucred_caps(pid, caps);
    SPIKE_PASS("process", "pid %d authid 0x%016llx caps %02x%02x%02x%02x...%02x", (int)pid,
               (unsigned long long)kernel_get_ucred_authid(pid), caps[0], caps[1], caps[2],
               caps[3], caps[15]);

    size_t flex = 0;
    sceKernelAvailableFlexibleMemorySize(&flex);
    SPIKE_PASS("memory", "direct %zu MB, flexible available %zu MB",
               sceKernelGetDirectMemorySize() >> 20, flex >> 20);

    int ncpu = 0;
    size_t len = sizeof ncpu;
    if (sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0) == 0)
        SPIKE_PASS("cpus", "%d", ncpu);

    /* PS5VR binds these by name or by per-firmware export offsets. */
    report_file("hmd2-lib", "/system/common/lib/libSceHmd2.sprx");
    report_file("vrtracker2-lib", "/system/common/lib/libSceVrTracker2.sprx");
    report_file("vrhand-lib", "/system/common/lib/libSceVrHand.sprx");
    report_file("hand-models", "/system/priv/vr_hand_ro/handPose_00.bin");

    /* ps5-opengl and PS5VR run as registered titles from /data/homebrew. */
    struct stat st;
    if (stat("/data/homebrew", &st) == 0)
        SPIKE_PASS("homebrew-dir", "/data/homebrew exists");
    else
        SPIKE_SKIP("homebrew-dir", "/data/homebrew missing (ShadowMount+/etaHEN title dir)");

    spike_notify("ps5webkit: spike 00 done");
    spike_log("done");
    return 0;
}
