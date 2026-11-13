/* Spike 12 P0b/P0c: what the GL thread and the Vulkan side share. */
#pragma once
#ifdef __cplusplus /* P1's Dawn side */
#include <atomic>
using std::atomic_int;
using std::atomic_uint;
using std::atomic_ulong;
#else
#include <stdatomic.h>
#endif
#include <stddef.h>
#include <stdint.h>

#define BRIDGE_W 1920
#define BRIDGE_H 1080
#define BRIDGE_BYTES ((size_t)BRIDGE_W * BRIDGE_H * 4)

struct gl_shared {
    atomic_int stop;
    atomic_int state;            /* 0 starting, 1 presenting, -1 failed */
    atomic_ulong frames;         /* eglSwapBuffers that returned */
    atomic_ulong gl_errors;      /* glGetError() != 0, counted once a frame */
    atomic_ulong swap_failures;
    char info[256];              /* GL_VERSION / failure text */

    /* The bridge: a persistent SSBO whose mapping Vulkan imports. */
    atomic_int want_bridge;      /* main -> GL: make the buffer */
    atomic_int bridge_state;     /* 0 none, 1 ready, -1 failed */
    uint8_t *map;                /* glMapBufferRange result */
    size_t map_bytes;
    atomic_uint offset_words;    /* where the frame starts in the buffer, in uints */
    atomic_int posted;           /* main -> GL: frame number written by Vulkan */
    atomic_int drawn;            /* GL -> main: last frame turned into the texture */
    atomic_int verify;           /* main -> GL: read the texture's centre and corner back */
    uint8_t centre[4], corner[4];
    double pass_ms;              /* CPU ms of the last SSBO -> texture pass (with glFinish when verify) */
    double pass_ms_sum;
    int pass_count;
};

extern struct gl_shared g_gl;
void *gl_thread_main(void *arg);
void vk_run(void);
