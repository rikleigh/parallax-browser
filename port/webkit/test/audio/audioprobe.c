/*
 * audioprobe.c - can an elfldr payload play sound through sceAudioOut?
 * (Milestone 2: decides whether GStreamer's audio sink can live in the
 * WebProcess payload or must ship PCM to the title.)
 *
 *   . ./env.sh && make -C port/webkit/test/audio && tools/send.sh port/webkit/test/audio/audioprobe.elf
 *
 * Opens the main port (S16 stereo, 48 kHz, 256 frames) for several user ids
 * and feeds it 2 s of silence (TONE=1 build: a quiet 440 Hz tone), timing
 * each sceAudioOutOutput. A port that plays blocks ~5.33 ms per buffer once
 * its queue is full; one that drops data returns at once.
 * RESULT lines on stdout.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef TONE
#define TONE 0
#endif

int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t userId, int32_t type, int32_t index, uint32_t len, uint32_t freq, uint32_t param);
int sceAudioOutOutput(int32_t handle, const void* ptr);
int sceAudioOutClose(int32_t handle);
int sceAudioOutSetVolume(int32_t handle, int32_t flag, int32_t* vol);
int sceUserServiceInitialize(void* params);
int sceUserServiceGetForegroundUser(int32_t* user);
int sceUserServiceGetInitialUser(int32_t* user);

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

#define FRAMES 256
#define RATE 48000

static int16_t buf[FRAMES * 2];

static void try_port(const char* what, int32_t user)
{
    int h = sceAudioOutOpen(user, 0 /* MAIN */, 0, FRAMES, RATE, 1 /* S16 stereo */);
    if (h < 0) {
        printf("RESULT audioprobe open-%s FAIL user %d: 0x%08x\n", what, user, (unsigned)h);
        return;
    }
    int32_t vol[2] = { 32768, 32768 };
    int vrc = sceAudioOutSetVolume(h, 3, vol);
    double t0 = now_ms(), worst = 0, last = t0;
    int n = (2 * RATE) / FRAMES, bad = 0, blocked = 0;
    unsigned phase = 0;
    for (int i = 0; i < n; i++) {
        for (int f = 0; f < FRAMES; f++) {
            int16_t s = TONE ? (int16_t)(1500 * sin(2 * M_PI * 440.0 * (phase++) / RATE)) : 0;
            buf[2 * f] = buf[2 * f + 1] = s;
        }
        int rc = sceAudioOutOutput(h, buf);
        double t = now_ms();
        if (rc < 0)
            bad++;
        if (t - last > 2.0)
            blocked++;
        if (i > 8 && t - last > worst)
            worst = t - last;
        last = t;
    }
    sceAudioOutOutput(h, NULL); /* wait for the queue to drain */
    double total = now_ms() - t0;
    printf("RESULT audioprobe open-%s %s user %d: handle 0x%x, volume rc 0x%x, %d buffers (%.0f ms of audio) took %.0f ms, %d calls blocked >2 ms, worst %.2f ms, %d errors\n",
        what, (total > 1500 && bad == 0) ? "PASS" : "FAIL", user, h, (unsigned)vrc, n, n * FRAMES * 1000.0 / RATE, total, blocked, worst, bad);
    sceAudioOutClose(h);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int rc = sceAudioOutInit();
    printf("RESULT audioprobe init %s 0x%08x (tone %d)\n", (rc == 0 || rc == (int)0x8026000e) ? "PASS" : "FAIL", (unsigned)rc, TONE);
    int urc = sceUserServiceInitialize(NULL);
    int32_t fg = -1, init = -1;
    int frc = sceUserServiceGetForegroundUser(&fg);
    int irc = sceUserServiceGetInitialUser(&init);
    printf("RESULT audioprobe users INFO init 0x%x, foreground %d (0x%x), initial %d (0x%x)\n", (unsigned)urc, fg, (unsigned)frc, init, (unsigned)irc);
    try_port("system", 255);
    if (fg >= 0)
        try_port("foreground", fg);
    if (init >= 0 && init != fg)
        try_port("initial", init);
    printf("RESULT audioprobe done\n");
    return 0;
}
