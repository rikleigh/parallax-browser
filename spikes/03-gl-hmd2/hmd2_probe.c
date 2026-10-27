/* Spike 3a - the headset path without GL.
 *
 * Opens the PS VR2 through psvr2 (PS5VR's sequence) and shows a CPU-drawn
 * side-by-side test card for SECONDS seconds. Two questions, before GL is
 * involved at all:
 *   1. Does Hmd2/VrTracker2/VideoOut-bus-32 work from an elfldr payload, or
 *      only from a registered title (as PS5VR runs)? Payload = fast iteration.
 *   2. Does the compositor accept a LINEAR texture (sw_mode 0)? Sony titles
 *      and PS5VR hand it SW_64KB_R_X. If linear works, any GL texture whose
 *      memory we can address can be submitted; if not, a tiling step is needed.
 *
 * What you should see in the headset: left eye red-tinted, right eye
 * blue-tinted, a white grid, a green bar that sweeps once a second. The card
 * is head-locked (each frame is submitted with the pose it was "rendered"
 * at), so it follows the head. Logged: per-call results, frames submitted.
 */
#define SPIKE_NAME "03a-hmd2-probe"
#include "../common/spike.h"
#include "../common/psvr2/psvr2.h"

#include <stdint.h>

#ifndef SECONDS
#define SECONDS 20
#endif
#ifndef EYE
#define EYE 768 /* small: the CPU draws every pixel, and a payload has ~55 MB of direct memory */
#endif

int sceUserServiceInitialize(void *);
int sceUserServiceGetInitialUser(int *);
int sceUserServiceGetForegroundUser(int *);
typedef struct { int user_id[4]; } user_list_t;
int sceUserServiceGetLoginUserIdList(user_list_t *);

/* GetInitialUser fails in a payload (no app launch context): fall back to
 * the foreground user, then the first logged-in user. */
static int find_user(void)
{
    int user = -1;
    int r0 = sceUserServiceInitialize(NULL);
    int r1 = sceUserServiceGetInitialUser(&user);
    if (r1 == 0 && user >= 0)
        return user;
    int r2 = sceUserServiceGetForegroundUser(&user);
    if (r2 == 0 && user >= 0)
        return user;
    user_list_t l;
    memset(&l, 0xff, sizeof l);
    int r3 = sceUserServiceGetLoginUserIdList(&l);
    spike_log("user service: init 0x%08x initial 0x%08x foreground 0x%08x list 0x%08x [%x %x %x %x]",
              r0, r1, r2, r3, l.user_id[0], l.user_id[1], l.user_id[2], l.user_id[3]);
    for (int i = 0; i < 4; i++)
        if (l.user_id[i] >= 0)
            return l.user_id[i];
    return -1;
}

static void log_line(const char *s)
{
    spike_log("%s", s);
}

static void draw(uint32_t *px, uint32_t pitch_px, uint32_t frame)
{
    const uint32_t w = 2 * EYE, h = EYE, bar = (frame % 60) * (w / 60);
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = px + (size_t)y * pitch_px;
        for (uint32_t x = 0; x < w; x++) {
            const int right = x >= EYE;
            const uint32_t ex = x - (right ? EYE : 0);
            uint32_t c = right ? 0xff602010u : 0xff102060u; /* ABGR: blue / red tint */
            if (ex % 128 < 3 || y % 128 < 3)
                c = 0xffffffffu;
            if (y > h / 2 - 16 && y < h / 2 + 16 && x >= bar && x < bar + w / 60)
                c = 0xff00ff00u;
            row[x] = c;
        }
    }
}

int main(void)
{
    spike_log("ps5webkit spike 03a: Hmd2 probe, linear CPU-drawn buffers (pid %d)", (int)getpid());
    psvr2_set_logger(log_line);

    if (psvr2_preload() != 0) {
        SPIKE_FAIL("preload", "Hmd2/VrTracker2 could not be bound on this firmware");
        return 1;
    }
    SPIKE_PASS("preload", "libraries bound");

    const int user = find_user();
    if (psvr2_open(user) != 0) {
        SPIKE_FAIL("open", "Hmd2/tracker/reprojection setup failed for user 0x%x", user);
        return 1;
    }
    float fov[4];
    psvr2_fov(fov);
    SPIKE_PASS("open", "user 0x%x, FOV tangents out %.3f in %.3f up %.3f down %.3f, worn %d", user,
               (double)fov[0], (double)fov[1], (double)fov[2], (double)fov[3], psvr2_worn());

    /* Linear RGBA8, rows padded to 256 texels. */
    const uint32_t pitch_px = (2 * EYE + 255) & ~255u;
    const uint64_t one = ((uint64_t)pitch_px * 4 * EYE + 0xffff) & ~0xffffull;
    spike_log("eye buffers: %u x %llu KB", PSVR2_MAX_BUFFERS, (unsigned long long)(one >> 10));
    uint8_t *mem = psvr2_direct_alloc(one * PSVR2_MAX_BUFFERS, 0x10000);
    if (!mem) {
        SPIKE_FAIL("start", "no direct memory for eye buffers");
        psvr2_close();
        return 1;
    }
    uint32_t t[PSVR2_MAX_BUFFERS][8];
    for (int b = 0; b < PSVR2_MAX_BUFFERS; b++) {
        draw((uint32_t *)(mem + b * one), pitch_px, 0);
        psvr2_build_tsharp_rgba8(t[b], (uint64_t)(uintptr_t)(mem + b * one), 2 * EYE, EYE,
                                 pitch_px * 4, 0);
    }
    if (psvr2_start(t, PSVR2_MAX_BUFFERS, EYE, EYE) != 0) {
        SPIKE_FAIL("start", "VR mode / buffer registration refused a linear T#");
        psvr2_close();
        return 1;
    }
    SPIKE_PASS("start", "VR mode on, %d linear %ux%u buffers registered", PSVR2_MAX_BUFFERS,
               2 * EYE, EYE);

    float tan[2][4];
    psvr2_eye_tangents(0, 1.0f, tan[0]);
    psvr2_eye_tangents(1, 1.0f, tan[1]);
    unsigned ok = 0, bad = 0, tracked = 0, eye_pos = 0;
    double draw_ms = 0;
    const double t0 = spike_now_ms();
    for (uint32_t i = 0; spike_now_ms() - t0 < SECONDS * 1000.0; i++) {
        psvr2_frame_t f;
        if (psvr2_begin_frame(&f) != 0) {
            bad++;
            continue;
        }
        const int b = (int)(f.frame % PSVR2_MAX_BUFFERS);
        const double d0 = spike_now_ms();
        draw((uint32_t *)(mem + b * one), pitch_px, i);
        draw_ms += spike_now_ms() - d0;
        tracked += f.tracked != 0;
        eye_pos += f.eye_pos_valid != 0;
        if (i == 30)
            spike_log("pose: head (%.3f %.3f %.3f) eyes (%.3f %.3f %.3f) (%.3f %.3f %.3f) valid %d",
                      (double)f.head.pos[0], (double)f.head.pos[1], (double)f.head.pos[2],
                      (double)f.eye[0].pos[0], (double)f.eye[0].pos[1], (double)f.eye[0].pos[2],
                      (double)f.eye[1].pos[0], (double)f.eye[1].pos[1], (double)f.eye[1].pos[2],
                      f.eye_pos_valid);
        if (psvr2_submit(b, &f, tan) == 0)
            ok++;
        else
            bad++;
    }
    const double secs = (spike_now_ms() - t0) / 1000.0;
    psvr2_close();

    if (ok > 0 && bad == 0)
        SPIKE_PASS("submit", "%u frames in %.1f s (%.1f fps), CPU draw %.2f ms/frame", ok, secs,
                   ok / secs, ok ? draw_ms / ok : 0.0);
    else
        SPIKE_FAIL("submit", "%u ok, %u failed in %.1f s", ok, bad, secs);
    SPIKE_RESULT("tracking", tracked ? "PASS" : "FAIL", "%u/%u frames tracked, eye positions valid %u",
                 tracked, ok + bad, eye_pos);
    spike_log("VISUAL CHECK: did the headset show the red/blue test card with a sweeping green bar?"
              " Record the answer in FEASIBILITY.md (03a linear-T# visual).");
    spike_notify("ps5webkit: spike 03a done");
    return 0;
}
