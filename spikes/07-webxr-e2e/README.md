# Spike 7: end-to-end WebXR

**Goal:** `https://immersive-web.github.io/webxr-samples/` (immersive-vr) and
a three.js VR example enter an immersive session and render correctly in
the PS VR2, with head tracking and controllers.

**Status:** design only. It depends on Spikes 3, 5 and 6.

## How WPE's WebXR moves a frame today

1. The UIProcess runs OpenXR on its own thread (`PlatformXROpenXR.cpp`). It
   creates swapchains in its own EGL context and exports each swapchain
   image to the WebProcess as a **DMA-buf** (`exportOpenXRTextureDMABuf`,
   or via GBM/AHardwareBuffer), as `PlatformXR::FrameData::ExternalTexture
   { fds, strides, offsets, fourcc, modifier }`.
2. The WebProcess imports it as an EGLImage, and WebGL renders into it
   (`WebXROpaqueFramebuffer`).
3. The WebProcess sends back a fence fd (`FrameData::fenceFD`). The
   UIProcess server-waits on it (`waitFrameFence`) and calls `xrEndFrame`.

None of this exists on PS5: there's no DMA-buf, no EGL native fence fds, and
(on ps5-opengl) no EGLImage.

## The PS5 constraint that decides the design

The PS VR2 compositor samples eye buffers by GPU address through
**VideoOut buffers registered by the submitting process** (Spike 3). So the
process that calls `psvr2_start`/`psvr2_submit` must own (or have mapped)
the memory WebGL rendered into.

## Options, in order of preference

| | Design | Needs | WebKit patch size |
| --- | --- | --- | --- |
| A | **Shared direct memory.** The UIProcess allocates swapchain images in direct memory and passes `{dmem offset, size, layout}` in a PS5 variant of `ExternalTexture`. The WebProcess maps the same pages and wraps them as a GL texture (driver import, the reverse of the T# export). Fence: a shared-memory sequence number, or a GL fence plus a short CPU wait | Spike 1 `dmem-share` PASS, plus a ps5-opengl/Mesa "import texture from direct memory" entry point | small: a `#if PLATFORM(PS5)` branch in `OpenXRGraphicsBindingOpenGLES` and `WebXROpaqueFramebuffer` |
| B | **XR in the WebProcess.** Run the OpenXR session (our runtime) in the WebProcess, next to WebGL, with the UIProcess only coordinating. WebKit's early GTK WebXR prototype did this | GPU and Hmd2 access from the child process (Spike 1 + 3a in a payload) | medium: move `PlatformXR` device ownership behind IPC |
| C | **Single process** (in-process launcher, Spike 6) | nothing new for XR. Same-process textures, shared contexts (Spike 2 `shared-thread`) | XR small, launcher large |
| D | Readback through shared memory | always possible | small, but ~2 × 1536² × 4 B per frame at 60 fps (~1 GB/s). Expect it to miss frames: last resort |

## Validation

- `webxr-samples/immersive-vr-session.html`, `input-selection.html`,
  `controller-state.html`, and the three.js `webxr_vr_ballshooter` example.
- Pass: the session starts from the page's button, stays at 60 fps rendered
  / 120 Hz displayed with no judder, stereo is correct, Sense controllers
  track with select/squeeze events, and exiting returns to the 2D page.
