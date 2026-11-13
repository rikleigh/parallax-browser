# Spike 12: WebGPU on the PS5 (Dawn on RADV)

_2026-11-13. Owner's question: can Parallax Browser get WebGPU, for rendering
and for compute/ML? Plan: backend library Dawn; the GPU route under it (Vulkan
on RADV, or GLES on ps5-opengl) decided by cheap spikes before any WebKit work._

## Gates

| Gate | Decision | Why |
| --- | --- | --- |
| **1: which route** | **Route V: Dawn's Vulkan backend on RADV** (PS5_Mesa), beside ps5-opengl in the title | P0a, P0b and P0c all pass. Route V gives core WebGPU; Route G would be compatibility mode only. No Clang bump is needed for Dawn |
| **2: Dawn in a title** | **Go** | P1: core adapter, WGSL triangle and compute correct, frames to a GL texture for 1.3 ms of CPU at 1080p. Nobody has looked at the TV picture yet; "correct" is by GL readback |

## The spikes (all run on the console, 13.60, 2026-11-13)

| | What | Result | Notes |
| --- | --- | --- | --- |
| P0a | Dawn cross-built for the title (`port/deps/build.sh dawn`) | PASS | [p0a-dawn/NOTES.md](p0a-dawn/NOTES.md). Clang 18 + libc++ 19, 3 patches, 33.6 MB archive, about 6 MB linked |
| P0b | RADV in one title with ps5-opengl; triangle and compute while GL presents | PASS | [p0b-radv/NOTES.md](p0b-radv/NOTES.md). The two Mesas are kept apart by linking RADV as one object with three global symbols |
| P0c | A Vulkan frame into a GL texture | PASS | Same notes. Zero copy by importing the GL buffer's mapping (0.07 ms); or one memcpy (0.57 ms) |
| P0d | What Dawn's GLES backend would need from ps5-opengl (Route G) | 90 of 93 PASS | [p0d-glprobe/NOTES.md](p0d-glprobe/NOTES.md). Compute works there too; the 3 failures are EGL facade gaps a shim covers. Kept as the fallback |
| P1 | Dawn on RADV in a title, shown through the compositor's kind of path | PASS | [p1-dawn-title/NOTES.md](p1-dawn-title/NOTES.md) |

## In the browser (P2 to P6 in part, on the console 2026-11-13)

`WEBGPU=1 port/webkit/build.sh` builds WebKit with `ENABLE_WEBGPU` and
`USE_DAWN` into `toolchain/wpe-webkit/build-webgpu`; `package.sh` with that
build dir links RADV in. WebKit side, patches 0109 to 0124:

- `Source/WebGPU/WebGPUDawn/`: WebKit's `WebGPUCpp.h` implemented over Dawn
  (29 files). It runs in the title, on RemoteGPU's work queue thread; the
  WebProcess ELF has no Dawn in it.
- Canvas: `GPUCanvasContextPS5`; each presented frame comes out of Dawn as
  pixels (`CopyTextureToBuffer` + `MapAsync`) and becomes a texture in the
  title's WC scene through the WebGL layer registry (`PS5WebGPUCanvas`).
  Readback (`toDataURL`, `drawImage`) is a sync message with shared memory.
- Setting: "WebGPU (experimental)", off by default, off with Safer browsing
  (`/data/ps5webkit/webgpu-on`). Switching it on takes effect in pages that
  are already open (0114).
- Memory budget (0120): buffers and textures are counted, 1536 MB for the
  title (`/data/ps5webkit/webgpu-budget-mb` to change it); over it the page
  gets a `GPUOutOfMemoryError` and the driver is not asked. The `[mem]` line
  reports use, peak and refusals. A canvas's readback buffers count too, and
  the frames all canvases hold on the CPU are bounded at 768 MB (0121; over
  either a frame is not shown). Those two bounds have not been driven to
  their limits on the console.

Test pages: `port/webkit/test/webgpu/` (`run-console.sh` runs them on the
console). Console,
`port/webkit/test/webgpu/results/console-2026-11-13-b.log`:

| Page | Result |
| --- | --- |
| adapter, device, compute, offscreen-readback, errors, buffer-map | 11/11, 29/29, 6/6, 7/7, 11/11, 11/11 |
| triangle, cube, resize, two-canvases, todataurl | 3/3, 3/3, 10/10, 7/7, 11/11; 60 frames/s at every size up to 1920x1080, three canvases together included |
| copy-external-image | 16/16: canvas, ImageBitmap, OffscreenCanvas, `<img>` and ImageData sources (0117) |
| video-texture | 5/5 (`results/console-2026-11-13-e.log`): `copyExternalImageToTexture` from a `<video>` (0123) and `importExternalTexture` (0124), both from the title's decoded frames, read back with a compute pass (1.2 ms for 1920x1080; `glReadPixels` took 34 ms). An external texture imported every frame from a 1080p video draws at 38.5 frames/s |
| ml-transformers (transformers.js 3.8.1 on ONNX Runtime Web, all-MiniLM-L6-v2) | 4/4: WebGPU fp32 matches WebAssembly to 1.00000, fp16 (`shader-f16`) to 0.99998; a warm run takes 38 ms on WebGPU, 106 ms on one-thread WebAssembly |
| WebGL regression (`webgl/readpixels.html`, WebGPU on) | 29/29 |

Real content: three.js r-current examples from threejs.org run on
`WebGPURenderer` at 60 frames/s at 1920x1080 with no page errors:
`webgpu_instance_mesh`, `webgpu_loader_gltf`, `webgpu_compute_particles`,
`webgpu_animation_retargeting`, `webgpu_animation_retargeting_readyplayer`,
`webgpu_materials`, `webgpu_postprocessing_bloom`. The owner looked at the
first few on the TV.

Found on the way:

- RemoteGPU's work queue thread had a 64 KiB stack and Dawn's device creation
  overflowed it (the title ended with no report). It has 8 MiB now.
- An opaque canvas's frames missed TextureMapper's fast upload path
  (`glTexSubImage2D`, 35 ms at 1080p: 29 frames/s). Fixed (0112).
- On a non-Apple port `importExternalTexture` returned a proxy for an object
  the GPU side never made; the next message failed that side's checks, the
  connection closed, and the page hung on a full stream buffer. Fixed for
  this case (0113), and in general (0119): the title now ends a WebProcess
  whose message its GPU side refuses, and the browser reloads the page (twice
  at most).
- The setting changed in Settings never reached the GPU side of a WebProcess
  that was already running (no GPUProcessProxy in the title): three.js fell
  back to WebGL with WebGPU "on". Fixed (0114).
- A pipeline stage without `entryPoint` reached Dawn as the name "": three.js's
  mipmap pipelines were all invalid and textured models came out black.
  Fixed (0115).
- **Black flicker of layers above the canvas** (panels, a sidebar; about once
  a second), and now and then broken geometry in the canvas. The two drivers
  share one GPU, and each sets the GPU's state in one submission and relies
  on it in the next ones of the same frame (ps5-opengl) or queue submit
  (RADV). A submission of the other driver that fell in between ran those
  with the wrong state: in the bad frames some layers were drawn without
  blending, so their transparent parts overwrote what was behind them
  (the compositor's GL calls were the same as in good frames). Fixed with
  one GPU submission lock in the title (`titleradv.c`): Dawn holds it around
  `vkQueueSubmit` (Dawn patch 0004), the render thread around a frame's GL
  work, not across its waits (`PS5Renderer.cpp`, WebKit 0122). Console: 0
  black frames in 3000 with the lock, 18 in 1200 without; 60 frames/s. Not
  covered: the WebGL thread's GL (a page with WebGL and WebGPU at once) and
  the swap's own submission. Reusing canvas textures (0116) only made it
  rarer by moving the timing; it stays, as it saves a texture a frame.
- ONNX Runtime Web's WebAssembly memory is shared and declares 4 GiB, which
  JSC reserves at once; a payload can map about 5.4 GiB. The WebProcess could
  then not map a 45 MB tensor (`getMappedRange()` came back empty) and
  crashed encoding a 16 MB IPC message. WebAssembly memories are 2 GiB at
  most now (0118; this is in every build, not only WebGPU ones).
- From a `file:` page, fetches from huggingface.co failed "due to access
  control checks" most of the time (the server's headers are right). Not
  looked into; the ML page takes its model from `serve-models.py` instead.

Since the lock: Dawn carries the M157 branch's six backports that touch code
built here (`port/deps/patches/dawn/0005` to `0010`); the test pages give the
same results with them (`results/console-2026-11-13-d.log`). The default
build (no WebGPU) with patches 0109 to 0122 runs on the console: WebGL
regression 29/29, a three.js WebGL example at 60 frames/s, no `navigator.gpu`.

## Costs measured

- Title image (flexible memory): +25 MB for RADV and Dawn together
  (eboot 168 MB to 193 MB).
- Run time: direct memory only, about 45-60 MB for a 1080p canvas's worth of
  buffers and the drivers' arenas. The title's direct memory was 0.8 to
  1.2 GB with a three.js example open, 2.7 GB at the peak of the ML page.

## Open

- Video in WebGPU works for hardware-decoded `<video>` elements only (the
  ones with a stream in the title). A WebCodecs `VideoFrame`, or a video
  without such a stream, gives InvalidStateError from `importExternalTexture`
  and black from a copy. An imported 1080p video holds 38.5 frames/s, not 60
  (the frame goes GPU to CPU to GPU: 8 MB each way).
- `<img>` sources are drawn premultiplied and read back: colour under partial
  alpha loses precision and is gone where alpha is 0.
- Not done from P6: an on-disk pipeline cache (RADV's own is off in the
  title, it defaults to a folder a title cannot write; Dawn's caching
  interface is not implemented), the WebGPU CTS, and a review of the
  title-side code against a hostile WebProcess (known: layer keys can be made
  to collide, which loses a canvas, not the title; texture sizes in the
  budget are estimates).
- Device loss has not been provoked on the console. A GPU fault in either
  driver still ends the title.
- A buffer unmapped after writing sends its bytes inline in an IPC message up
  to WebKit's usual threshold; with little address space left that
  allocation is fatal for the WebProcess (seen once, before 0118).
- Only one model has run (90 MB). Nothing larger, no image model, no
  ONNX Runtime Web without transformers.js.
