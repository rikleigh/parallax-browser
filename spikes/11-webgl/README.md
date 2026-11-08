# Spike 11: WebGL on the PS5 (ANGLE over ps5-opengl, and the other ways)

_2026-11-07. Branch `spike/webgl`. Owner's question: can Parallax Browser get
full WebGL? Part 1 builds ANGLE for the PS5 and runs it over ps5-opengl in a
test title. Part 2 assesses every other route and ranks them._

## Part 1 result: ANGLE on ps5-opengl draws on the console at native speed

| Step | Status | Evidence |
| --- | --- | --- |
| ANGLE (WebKit's vendored copy, 2.1.29096) cross-compiled for the PS5 title with its GL backend over EGL | **PASS** (Mac) | `build/angle-ps5/lib/libangle_ps5.a`, 17 MB, 306 objects, 3 small patches ([patches/](patches/)) |
| Test title linked against ANGLE + ps5-opengl (gles-async) + libc++ 19, packaged | **PASS** (Mac) | `build/titles/PPSA99006/eboot.bin`, 26 MB, fake-signed, integrity valid |
| ANGLE over ps5-opengl's real EGL facade + driver caps + Mesa (Spike 2's Rosetta harness, no GPU) | **PASS** (Mac host) | [results/hostcheck-2026-11-07.log](results/hostcheck-2026-11-07.log): EGL 1.5 ANGLE display, surfaceless, ES 3.0 context, 110 extensions, ESSL 300 es / 100 compiled |
| Triangle, FBO, texture, readPixels, fence, draw-call timing on the console | **PASS** (PS5, 2026-11-07) | [results/console-2026-11-07.log](results/console-2026-11-07.log): all 13 ANGLE checks and 4 native checks pass; per draw 8.3 µs through ANGLE vs 8.2 µs native |

So piece 1 is a **go**: ANGLE needs nothing from ps5-opengl that it does not
have, once three things are shimmed (below), and on the console it draws
correctly with no measurable overhead.

### Console run (2026-11-07)

| Check | ANGLE | Native ps5-opengl |
| --- | --- | --- |
| Display / context | EGL 1.5, ES 3.0, 110 extensions, surfaceless | EGL 1.4, ES 3.2 |
| ESSL 300 es program | compiled + linked in 4.9 ms | |
| Triangle into an RGBA8 FBO, read back | right colours, GL error 0 | |
| Texture sampling (64x64 checker) | right colours | |
| `glFenceSync` / `glClientWaitSync` | signalled at once | |
| `#version 100` (WebGL 1) shader | right colours | |
| 2000 draws, one frame | 17.54 ms, **8.3 µs per draw** | 17.48 ms, **8.2 µs per draw** |
| 1920x1080 RGBA `texSubImage2D` + finish | 34.5 ms | 34.3 ms |
| 256x256 `readPixels` | 1.05 ms | 1.05 ms |

ANGLE's validation and state tracking cost about 0.1 µs per draw here, well
under the 20 µs that would have pointed at redundant state. The draw cost is
ps5-opengl's own (about 8 µs, as in Spike 2b), so a WebGL page gets the same
draw budget as native GLES on this driver: about 2000 draws per 60 Hz frame.
The 1080p upload (35 ms) is slow, as it is natively; it matters for video
textures and big canvases, which is why Part 2 copies video on the GPU.

### Share group (2026-11-07): works, with two rules for the port

`share_probe_run` ([src/angle_tests.cpp](src/angle_tests.cpp), native side in
[src/share_native.c](src/share_native.c)) sets up sharing the way WebKit does
(`PlatformDisplay::angleSharingGLContext()` and `GraphicsContextGLEGL`)
against a GL 4.6 core context made like the browser's compositor context.
Console log: [results/console-share-2026-11-07.log](results/console-share-2026-11-07.log).

| Check | Result |
| --- | --- |
| GL 4.6 core context, `EGL_OPENGL_API` bound (as `PS5Renderer::initializeGL`) | PASS |
| ES 3.2 context created with the core context as its share context | PASS |
| `EGL_EXTERNAL_CONTEXT_ANGLE` around that ES context | PASS |
| WebGL 2 context (WebKit's attributes, virtualization group 0) sharing with it | PASS: native ES 3.2 context |
| Red triangle drawn through ANGLE; ANGLE texture 1 is native texture 9 | PASS |
| `glFenceSync` in ANGLE's native context, waited on in the core context | PASS |
| Core context reads native texture 9: red centre, blue corner | PASS |
| Core context `glCopyImageSubData`s its own texture into it (the video path); ANGLE reads it back | PASS |

So the compositor can show what WebGL draws, and give WebGL its video frames,
with no copies beyond the one for video, no EGLImage and no DMA-buf. Two
things failed first (both reproduced on the Mac under the host harness, where
lldb gave the backtrace) and are rules for the port:

1. **Wrap an ES context, not the 4.6 core context.** ANGLE is built without
   `ANGLE_ENABLE_GL_DESKTOP_BACKEND`, so a renderer around a desktop GL
   context loads no GL functions and calls a null `genTextures` in
   `GenerateCaps` (the first console run died there with no report). The
   host makes an ES 3.2 "anchor" context with the compositor's context as its
   share context and wraps that; share groups are transitive. (Building the
   desktop backend in would also do, at the cost of a larger ANGLE.)
2. **Bind `EGL_OPENGL_ES_API` before creating a WebGL context.** ANGLE binds
   ES once, when the display is set up, then creates each virtualization
   group's native context with the thread's bound API. On the compositor's
   thread that is `EGL_OPENGL_API`, so the "ES" context comes out desktop GL
   and crashes as in 1 (`SPIKE11_SHARE_GL_BOUND=1` on the host shows it). The
   host binds ES around the call, or ANGLE's `DisplayEGL::initializeContext`
   is patched to.

Also seen: creating the WebGL context leaves no native context current
(ANGLE restores what it thinks was current), so the host makes the
compositor's context current again afterwards. The probe reads ANGLE's
native texture name through ANGLE's internals
([src/angle_native.cpp](src/angle_native.cpp)); the port needs the same, as
a small exported function.

This run used the browser's driver build (`gles`), where a draw costs 27 µs
through ANGLE and natively alike (the 8 µs above is `gles-async`; FEASIBILITY
§4.2c has the builds and why the async one costs two cores). WebGL in the
browser gets whichever build the browser ships.

### Host check (what passed)

Spike 2's `hostcheck` links the patched `ps5_egl.c`, the real
`ps5_screen.c` (caps, formats, descriptors) and Mesa 26.2 for x86_64 macOS,
with AGC and the PS5 shader compiler as aborting stubs. Spike 11 links the
same objects with a host build of the same patched ANGLE
([hostcheck/run.sh](hostcheck/run.sh)) and runs `angle_tests.cpp` in
`HOST_CHECK` mode (everything up to shader compilation; linking a program
would reach the PSBC stub). Output, trimmed:

```
RESULT 11-webgl-host angle-display PASS EGL 1.5 vendor "Google Inc. (PS5 homebrew)" version "1.5 (ANGLE 2.1.29096 git hash: e9f64dda0c5b)" apis "OpenGL_ES" (68 ms)
  ANGLE display extensions: EGL_EXT_create_context_robustness EGL_KHR_create_context ... EGL_KHR_surfaceless_context ... EGL_KHR_no_config_context EGL_KHR_create_context_no_error EGL_KHR_reusable_sync ...
RESULT 11-webgl-host angle-config PASS config id 2 renderable 0x4d, surfaceless 1
RESULT 11-webgl-host angle-context-es3 PASS GL "OpenGL ES 3.0 (ANGLE 2.1.29096 ...)" renderer "ANGLE (PS5 homebrew, PS5 AGC, OpenGL ES 3.2 Mesa 26.2.0)" GLSL "OpenGL ES GLSL ES 3.00 (ANGLE ...)", 110 extensions, surfaceless (1 ms)
  WebGL-relevant: +OES_texture_float -OES_texture_float_linear +OES_texture_half_float +EXT_color_buffer_float +OES_element_index_uint +OES_standard_derivatives +EXT_texture_filter_anisotropic +OES_vertex_array_object +ANGLE_instanced_arrays +EXT_sRGB +OES_EGL_image +OES_EGL_image_external +KHR_debug +ANGLE_framebuffer_blit +ANGLE_framebuffer_multisample +EXT_disjoint_timer_query +OES_texture_npot +OES_rgb8_rgba8 +EXT_texture_compression_s3tc
  limits: max texture 16384, renderbuffer 16384, vertex texture units 16, vertex attribs 16
RESULT 11-webgl-host angle-shader-es3-compile PASS ANGLE translation + Mesa front end: vs 1 fs 2
RESULT 11-webgl-host angle-shader-tex-compile PASS ANGLE translation + Mesa front end: vs 3 fs 4
RESULT 11-webgl-host angle-context-es2 PASS GL "OpenGL ES 3.0 (ANGLE ...)" GLSL "OpenGL ES GLSL ES 3.00 (ANGLE ...)"
RESULT 11-webgl-host angle-shader-es2-compile PASS ANGLE translation + Mesa front end: vs 1 fs 2
0 failure(s)
```

Read-outs:

- ANGLE's `DisplayEGL` negotiates with ps5-opengl's facade as it is: EGL
  "1.4", `EGL_KHR_create_context`, `EGL_KHR_surfaceless_context` (so no mock
  pbuffer), `EGL_KHR_no_config_context`, `EGL_EXT_create_context_robustness`
  (ANGLE adds the robustness attributes; the facade accepts them). It binds
  `EGL_OPENGL_ES_API` and creates an ES 3.2 native context (patch 0001 of
  Spike 2), then caps the client at **ES 3.0**: enough for WebGL 2 (which
  needs ES 3.0), and the same as most ANGLE-on-GLES devices.
- WebGL 1 and 2 both get what WebKit asks for: `OES_texture_float`,
  `OES_texture_half_float`, `EXT_color_buffer_float`, VAOs, instancing,
  derivatives, S3TC, anisotropic filtering, timer queries. Missing:
  `OES_texture_float_linear` (left off in ps5-opengl on purpose, Spike 2
  notes), so WebGL's `OES_texture_float_linear` extension will be absent;
  three.js copes.
- `OES_EGL_image` is advertised through from Mesa but does not work on
  ps5-opengl (Spike 2). Nothing in a WebGL page can reach it; WebKit's own
  use of it is the video-texture path (Part 2).
- The ES 2.0 request returns an ES 3.0 context: ANGLE's
  `EGL_ANGLE_create_context_backwards_compatible` default. WebKit asks for
  the exact version it wants, so this is cosmetic.

### What ANGLE needed from the platform

| Need | ps5-opengl has it? | Done |
| --- | --- | --- |
| EGL >= 1.4, `eglGetDisplay(EGL_DEFAULT_DISPLAY)`, `eglBindAPI(ES)` | yes (Spike 2 patch 0001 for ES) | nothing |
| `eglGetProcAddress` that returns the `egl*` entry points (ANGLE loads them from the EGL library it `dlopen`s: `libEGL.so.1`) | **no**: the facade's `eglGetProcAddress` only knows the GL entry points, and there is nothing to `dlopen` in a static title | [patches/0001](patches/0001-ps5-static-egl-lookup.patch): `FunctionsEGLDL` asks a host-supplied `angle_ps5_egl_get_proc_address()` ([src/egl_lookup.c](src/egl_lookup.c), a table of the 34 `egl*` symbols, then `eglGetProcAddress` for GL) |
| `eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS)` (client extensions) | returns NULL | nothing: ANGLE ignores the result at that point |
| Surfaceless contexts or a pbuffer config | surfaceless | nothing |
| `EGL_KHR_fence_sync` / `EGL_KHR_wait_sync` | **no** | nothing: ANGLE's `SyncGL` uses GL sync objects (`glFenceSync`), which Mesa has; only `eglCreateSyncKHR` from WebKit's own code would miss it, and WebKit uses it on the DMA-buf path only |
| `EGL_KHR_image_base` (EGLImage) | **no** (and `GL_OES_EGL_image` fails) | nothing for WebGL itself; needed later for video textures unless done with a shared context (Part 2) |
| GL entry points through `eglGetProcAddress` | yes (`_mesa_glapi_get_proc_address`) | nothing |
| `linux/futex.h` for `angle::SimpleMutex` | no (FreeBSD kernel) | [patches/0002](patches/0002-ps5-no-futex.patch): the `std::mutex` fallback under `ANGLE_PLATFORM_PS5` |
| `CLOCK_MONOTONIC_RAW`, `pthread_setname_np` | no (FreeBSD libc: `CLOCK_MONOTONIC`, `pthread_set_name_np`) | [patches/0003](patches/0003-ps5-system-utils.patch) |
| `extern thread_local` across objects (`gl::gCurrentValidContext`) | the title converter refuses the TLS init symbol `_ZTH...` | `ANGLE_USE_STATIC_THREAD_LOCAL_VARIABLES` (ANGLE's own option: accessor functions over function-local TLS) |
| `dlopen`, `dladdr`, `/proc/self/exe` | compile fine; fail at run time | nothing: ANGLE only uses them for diagnostics and module paths |
| Symbol clash with ps5-opengl's `gl*`/`egl*` in the same executable | would clash | the build leaves out ANGLE's unprefixed shells (`libGLESv2_autogen.cpp`, libEGL) and exports only `GL_*`/`EGL_*`, which is what WebKit's `GraphicsContextGLANGLE` calls anyway |
| zlib (program cache compression) | from `toolchain/wpe-sysroot` | linked |

Nothing in `toolchain/wpe-webkit/src` was changed. ANGLE is copied to
`build/angle-ps5/src` (git-ignored), patched there, and built with its own
CMake project ([angle/CMakeLists.txt](angle/CMakeLists.txt)) that reuses
ANGLE's `GLESv2.cmake`, `GL.cmake`, `Compiler.cmake` and `linux.cmake` source
lists with the same selection as `PlatformWPE.cmake` (`is_linux`), minus
X11/Wayland/GBM. Build: ~5 min at `-j8`.

### Build and run

```sh
. ./env.sh
spikes/11-webgl/build-angle.sh            # ANGLE -> build/angle-ps5/lib/libangle_ps5.a
spikes/11-webgl/build-title.sh            # title PPSA99006 (ps5-opengl gles-async)
spikes/11-webgl/hostcheck/run.sh          # the Mac host check (needs spike 2's hostcheck once)
# console (needs PPSA99006 registered; the owner runs --register):
export PS5WK_OWN_TITLES="PPSA99020 PPSA99006 PPSA99010"
tools/upload-title.sh spikes/11-webgl/build/titles/PPSA99006
tools/launch.sh PPSA99006
sleep 40; curl "ftp://$PS5_HOST:2121/data/ps5webkit-spike/webgl.log"
tools/launch.sh --stop
```

`package-title.sh` (Spike 2) gained `PS5_CXXLIB` so a title can link
`toolchain/libcxx19` instead of the SDK's libc++ 18 (ANGLE is built against
libc++ 19, like WebKit).

What the title does ([src/angle_tests.cpp](src/angle_tests.cpp),
[src/native_bench.c](src/native_bench.c)): the host-check steps, then a
256x256 RGBA8 FBO, a red triangle on blue with a `readPixels` check, a 64x64
checker `texImage2D` sampled and checked, `fenceSync`/`clientWaitSync`, an ES
2.0 context with `#version 100` shaders, then timings: 2000 strip draws with
two `uniform4f` each (Spike 2b's "uniform-draw" case) through ANGLE and
natively through ps5-opengl on the same display, a 1920x1080 RGBA
`texSubImage2D` (the cost of a video frame as a CPU texture) and a 256x256
`readPixels`. Every result is a `RESULT 11-webgl <test> PASS|FAIL ...` line
in `/data/ps5webkit-spike/webgl.log`. Expected: native ~8 µs per draw
(Spike 2b, gles-async); ANGLE adds its validation and state tracking, which
on desktop Linux costs about 1-3 µs per draw; a figure well above 20 µs
would point at ANGLE re-sending state the driver then re-encodes (ps5-opengl
has no dirty tracking, §4.2c).

## Part 2: the other ways, ranked

Sources: WebKit 15486a0178 (read for this spike), FEASIBILITY §4.2-4.8, the
Spike 1/2/8/10 results, the reference checkouts in `ref/` (PS5VR,
PS5_Vulkan, pacbrew-repo) and public material on ANGLE, SwiftShader,
llvmpipe and WebGL players. Effort is in agent sessions with the console
loop included.

### 1. Remote WebGL hosted in the title (recommended)

How: `ENABLE_WEBGL` and `UseGPUProcessForWebGL` on. The WebProcess keeps
WebKit's `RemoteGraphicsContextGLProxy` as it is: every WebGL call goes
into an IPC stream (a shared-memory ring with a semaphore wake-up). The
receiver, `RemoteGraphicsContextGL`, runs on a work queue inside the title
(the UIProcess) on ANGLE over ps5-opengl (Part 1). Each frame,
`prepareForDisplay` hands the drawing buffer's texture to the WC scene that
`PS5WCHost` already paints on the title's render thread.

What the WebKit source says it takes:

1. **`IPC::Semaphore` on FreeBSD: small (~150 lines).**
   `Platform/IPC/unix/IPCSemaphoreUnix.cpp` is Linux-only (memfd + futex);
   on other systems `signal()` is a no-op and `wait()` returns false, so the
   stream work queue would spin and every client wait would time out. Port:
   the same fd-based design over `shm_open(SHM_ANON)` and `_umtx_op`
   (`UMTX_OP_WAIT_UINT` / `UMTX_OP_WAKE`), which libthr itself uses here.
   The rest of the stream path (`SharedMemory` on SHM_ANON, socketpair with
   SCM_RIGHTS) already works (Spike 8, the WC tiles).
2. **Build switches: medium.** `ENABLE_GPU_PROCESS` depends on `USE_GBM` on
   WPE (`OptionsWPE.cmake:163`); ANGLE's GL backend needs Spike 11's three
   patches (carried as patch files like the others);
   `shouldUseRemoteRenderingForWebGL()` is already true on WPE without GBM.
   Media, canvas and DOM remote rendering stay off.
3. **A host in the title instead of a GPU process: medium.**
   `WebProcessProxy::createGPUProcessConnection` is the cut-in point: give
   the socket to an in-title object that answers `CreateRenderingBackend`
   (stub), `CreateGraphicsContextGL` / `ReleaseGraphicsContextGL` and owns a
   `RemoteSharedResourceCache`. `RemoteGraphicsContextGL` takes a small host
   interface instead of `GPUConnectionToWebProcess&` (which wants a
   `GPUProcess&`). None of `AuxiliaryProcess::initialize`'s process-global
   state is needed; the `isInGPUProcess()` asserts are debug-only. R14
   shrinks from "run a GPUProcess in-process" to "host one object type".
4. **The WC content layer: the largest piece.** Upstream's
   `RemoteGraphicsContextGLWC` uses `GraphicsContextGLEGL` and
   `TextureMapperGCGLPlatformLayer`, which are compiled out under
   `USE_COORDINATED_GRAPHICS` (always on for WPE) and have never been built
   here. The fit is `GraphicsContextGLCoordinated`: it yields a
   `CoordinatedPlatformLayerBufferRGB(nativeTextureID, size, GLFence)`, with
   no EGLImage, no DMA-buf and a `glFenceSync` fence. Needed: a delegate that
   catches `setDisplayBuffer`, a `TextureMapperPlatformLayer` wrapper that
   `WCScene::update` can `setContentsLayer` through `WCContentBufferManager`
   (already linked by `WC.cmake`), the scene keyed by the WebProcess
   identifier (PS5WCHost uses the title's own), and a lock or a single
   thread for the GL work queue and the render thread (upstream runs the
   scene and WebGL on one queue).
5. **Share group: tested, works** (see Part 1, with the two rules found
   there). ANGLE's contexts must share GL object names
   with the title's compositor context (`PlatformDisplay::angleSharingGLContext()`
   wraps it with `EGL_EXTERNAL_CONTEXT_ANGLE`). The title's context is GL
   4.6 core created with `EGL_NO_CONTEXT` (PS5Renderer.cpp:420); ANGLE makes
   GLES 3.x contexts. A GLES-to-GL-core share is normal in Mesa but untested
   on ps5-opengl (Spike 2 shared GL with GL). If it fails, the fallback is a
   copy (`glCopyImageSubData` into a compositor texture, well under 0.1 ms
   at 1080p) at the cost of one extra texture per canvas.

Unlocks: WebGL 2 (ES 3.0 through ANGLE; three.js r163+ is WebGL2-only and
throws without it, A-Frame stays blank), hardware speed, zero-copy video
textures (§Video below), and WebXR later in the same process (FEASIBILITY
§4.8 stays valid).

Risks: draw cost (ANGLE's state manager on top of a driver
without dirty tracking, §4.2c; the built title measures it); the sync WebGL
calls (`getError`, `readPixels`, `getParameter`, `finish`) are round trips
at ~131 µs each (R13), though WebKit's proxy batches and three.js issues
few per frame; title memory (ANGLE adds ~10 MB of image, charged to
flexible memory 1:1, R8); a WebGL crash takes the title down, not a page.

Effort: 2-4 days of sessions to a three.js page on the TV, about one more
for video textures.

Cheapest experiments: (1) the console run of the Spike 11 title (PPSA99006
must be registered first); (2) add to it an ANGLE context created with
`EGL_EXTERNAL_CONTEXT_ANGLE` against a native GL 4.6 core context, draw
through ANGLE into a texture, sample it from the core context and check a
pixel. That settles item 5 in an afternoon.

### 2. Software WebGL in the WebProcess (fallback, or a first step)

How: Mesa llvmpipe behind a surfaceless EGL in the WebProcess, with ANGLE's
GL backend on top (exactly Chrome's `--use-angle=gl` on llvmpipe) and
WebKit's in-process `GraphicsContextGLCoordinated`. WC mode cannot show an
in-process WebGL layer (`GraphicsLayerWC.cpp:1134` assumes a
`WCPlatformLayerGCGL`), but WebKit already has the CPU path:
`GraphicsContextGLANGLE::readRenderingResultsForPainting()` reads the
drawing buffer into a `PixelBuffer` and the canvas is painted like an
image into the tiles. At 1080p that is ~8 MB of readback plus the Skia
draw, about 5 ms of CPU per frame, far less than the rasteriser itself.

What it takes: pacbrew's Mesa has no software rasteriser
(`-Dgallium-drivers=zink -Dllvm=disabled`, `ref/pacbrew-repo/mesa/PKGBUILD`),
so: bump pacbrew's LLVM target package (14.0.6; Mesa 26 wants newer), build
Mesa with `-Dllvm=enabled -Dgallium-drivers=llvmpipe` for the payload, make
the surfaceless EGL device pick llvmpipe, then the same ANGLE patches and
`ENABLE_WEBGL` wiring as option 1 (which it shares). LLVM's JIT needs RWX,
which payloads have. Expect ~40-60 MB more payload image.

SwiftShader (ANGLE on Vulkan) is the other software route: it has no BSD
platform branch, WebKit's ANGLE CMake has no Vulkan backend, and Chrome
removed it from the default path in 137 as "effectively unusable" for
most content; public numbers put llvmpipe ahead of it. Not worth it.

Unlocks: WebGL 2 everywhere a page asks, so WebGL-only players run at all.
Performance estimate (unverified): a textured 360 sphere at 1080p 20-40
fps on 8 Zen 2 cores, a mid-weight lit three.js scene 5-15 fps.

Limits: video textures stop at what the WebProcess has (software-decoded
frames, 1080p max); hardware-decoded frames never leave the title, so
4K60 360 video through WebGL is out; no WebXR.

Effort: medium (1-2 days of sessions), no title changes. It shares the
`ENABLE_WEBGL` and ANGLE work with option 1, so doing it first is not
wasted, but it is not the owner's goal.

### 3. ANGLE on Vulkan (RADV) in the title

`ref/PS5_Vulkan` has RADV (Mesa 26.2 with a PS5 winsys on AGC) at Vulkan 1.4
running the CTS, vkQuake and RetroArch as titles. ANGLE's Vulkan backend is
its best-supported one. But WebKit's ANGLE build has no Vulkan backend, the
title's compositor would have to move from ps5-opengl to RADV/zink too (two
drivers in one process cannot share memory), and pacbrew's RADV reports no
external-memory fds at all (§4.2b). A later option if ps5-opengl's draw
cost or share group turn out to be the limit. Effort: large.

### 4. A separate GPU process, or GPU use from payloads

Not pursued beyond what FEASIBILITY already records. Sony's port launches
`/app0/*Process.self` through a private `<process-launcher.h>`
(`ProcessLauncherPlayStation.cpp`); the homebrew SDK has no stub for it, so
this stays a lead for the owner. Every homebrew GPU stack in `ref/`
(ps5-opengl, PS5_Vulkan's RADV, PS5VR's AGC renderer) runs as a title;
pacbrew's Mesa is packaged for payloads but has no on-device record
(`spikes/02-gl-egl/pacbrew-mesa/deploy.sh` is the ready, small experiment,
FEASIBILITY §8 item 2). Even if a payload could render, its output would
still have to come back as a CPU bitmap (no cross-process GPU memory, R4),
which makes it option 2 with a faster rasteriser, not option 1. Ideas that
need kernel-level changes were left as research notes only, per the rules.

### 5. The WebGL page's JavaScript in the title

A WebProcess inside the title would put JSC, the DOM and WebGL in one
process with the GPU. It needs an in-process WebProcess mode WebKit does
not have (a bigger version of R14), the title's JIT is `mprotect`-flip
only (patch 0030's JIT uses an RWX pool), the page-table budget and the
title's flexible memory are shared with the UI, and a page crash ends the
browser. Only worth it for WebXR frame latency, and option 1 already gives
WebXR the same process for its swapchain. Rank: last.

### Video textures under each option

| Option | `texImage2D(video)` for hardware-decoded frames |
| --- | --- |
| 1 (title host) | Same process and share group as the decoder: a stream message `CopyTextureFromVideoStream(streamKey, texture)` blits the title's converted RGBA video texture (`PS5CompositorContext::videoTexture`, patch 0051) into ANGLE's texture (`glCopyImageSubData`, 33 MB at 4K, trivial on the GPU) with a fence or a render-thread hand-off. No EGLImage needed. Today the remote path on non-Cocoa platforms falls back to `videoFrameToNativeImage`, which patch 0020 returns empty for remote samples, so hardware video would upload black until this is built. Medium. |
| 2 (software, WebProcess) | Frames would have to come back from the title as CPU NV12 (4K: 12 MB per frame, ~750 MB/s each way at 60 fps) and be converted on the CPU; software-decoded frames (1080p max) work through the slow fallback. Realistic ceiling 1080p30. |
| 3 (RADV in title) | As option 1, once the decoder's buffers are on the same driver. |
| 4, 5 | As 2, or as 1 for the in-title WebProcess. |

For VR tube sites this matters less than it looks: "View in VR" already
projects the decoded frame in the title without WebGL, and YouTube, Vimeo,
Delight XR and DeoVR's web player show a flat video when WebGL is off.
What WebGL fixes is the custom three.js/A-Frame players that show nothing
without it, plus maps and the occasional WebGL2-only site viewer.

### Ranking

| Rank | Option | Unlocks | Effort | Main risk |
| --- | --- | --- | --- | --- |
| 1 | Remote WebGL hosted in the title on ANGLE/ps5-opengl | WebGL 2 at hardware speed, zero-copy video textures, WebXR later | large (2-4 days + 1 for video) | draw cost; sync IPC |
| 2 | llvmpipe + ANGLE in the WebProcess | WebGL 2 everywhere, software speed | medium (1-2 days) | too slow for 3D scenes; no hardware video |
| 3 | ANGLE on RADV in the title | as 1 with a better backend | large | moves the whole compositor |
| 4 | separate GPU process / payload GPU | process isolation | unknown | unknown rights; output still a bitmap |
| 5 | WebProcess in the title | as 1 | very large | JIT, memory, isolation |

### Recommendation and milestones

**M1 on the console (2026-11-07).** The WebGL build, packaged as PPSA99006
with the noprof driver, on `file:///data/ps5webkit/webgl.html`: the title
logs `GPU process side started`, `GPU connection for WebProcess 4`, `ANGLE
context for WebGL 1` / `2` (max texture 16384) and `first frame finished`;
the page reads both triangles back green. The canvases stay blank (M2).
Three things the first runs showed, now fixed:

- 0082: `WebChromeClient::createGraphicsContextGL` opened a GL display in the
  WebProcess before going remote; the payload has no libEGL and libepoxy
  aborted. On PS5 WebGL there is remote only.
- 0082: `BitmapTexturePool` kept its main-thread timer off on PS5 only
  without the GPU process. With it on, removing the last scene stopped the
  timer from the render thread and the title aborted. (In WebCore,
  `ENABLE(WC_GPU_PROCESS)` isn't defined; the PS5 condition there is WC on
  PS5.) The console dropped off the network about a minute after that abort.
- 0083: `shouldUseRemoteRenderingForWebGL()` is false with coordinated
  graphics and no GBM; on PS5 it follows the preference.

**M2 on the console (2026-11-07).** How the frames reach the screen:

- The title's WebGL sharing context (the one ANGLE wraps) is an ES context
  made with the compositor's GL 4.6 core context as its share context
  (`PS5GPUProcessInTitle.cpp`, share-group rule 1); the work queue binds ES
  before each context (rule 2). Log: `WebGL shares GL names with the
  compositor`.
- Each context is a `GraphicsContextGLCoordinated` subclass whose display
  delegate hands every finished frame (native texture, size, GL fence) to a
  `PS5WebGLLayer` (`GPUProcess/graphics/ps5/PS5WebGLLayers.h`), registered by
  (WebProcess, `RemoteGraphicsContextGLIdentifier`).
- In the WebProcess the proxy's delegate only carries that identifier;
  `GraphicsLayerWC` sends it as `webGLLayerKey` in the layer update and
  `WCScene` sets the found layer as the contents layer, as for video. The
  scene now knows its page's WebProcess (`DrawingAreaProxyWCPS5` passes it).
- Buffers and their fences are only destroyed on the render thread
  (`collectGarbage()` each frame); a context going away stops its layer
  drawing before its textures are deleted.

Seen on the TV: both spin canvases turning at 59.9-60 fps, and three.js's
textured cube "very smooth". threejs.org's `webgl_animation_keyframes`
stays blank: its model is Draco-compressed and three.js's Draco decoder
needs WebAssembly, which this build doesn't have (the page logs `DRACOLoader:
WebAssembly is required`), not a WebGL fault.

**M3 on the console (2026-11-07).** `texImage2D(TEXTURE_2D, 0, RGBA[8],
RGBA, UNSIGNED_BYTE, video)` for a video the title decodes in hardware:

- WebCore asks the context (`GraphicsContextGL::ps5CopyTextureFromVideoStream`)
  with the player's ps5videosink stream key; the proxy sends
  `CopyTextureFromVideoStreamPS5(key, texture, flipY)` in the stream, in
  order with the page's other calls.
- In the title (`RemoteGraphicsContextGLPS5.cpp`) a small ANGLE entry point
  (`ANGLE_PS5PrepareExternalTexture2D`, `src/libANGLE/renderer/gl/ps5/`)
  defines level 0 as the frame's size in RGBA8, marks it initialized (robust
  resource init would otherwise clear the copy at first use) and returns the
  native texture. A separate native ES context of the share group
  (`PS5WebGLVideoCopy.cpp`) is made current just for the copy, so ANGLE's
  native context keeps the state ANGLE thinks it has; ANGLE's context then
  waits for the copy on the GPU.
- PS5Renderer publishes each stream's newest converted frame with a fence
  after the conversion; `copyVideoFrame` waits for it on the GPU, blits
  (flipped for `UNPACK_FLIP_Y`), and leaves a fence the next conversion into
  the texture waits for. No CPU waits on either side.
- WebKit's GStreamer player suspends muted video that isn't visible in the
  viewport, and a `<video>` never put in a document counts as invisible: it
  stopped at its first frame. On PS5 a detached video now counts as visible
  (`HTMLMediaElement::playerViewportVisibility`); muted videos in the page
  scrolled away are still suspended (one hardware decoder).

Seen on the TV: `test/webgl/video.html` (1080p H.264, detached and in the
page) upright with its clock running at 60 fps; `test/webgl/pano.html`, the
4K 360 grid, upright, clock running, 60 fps.

**M3 formats (2026-11-07, afternoon).** The copy now takes what players
upload: `texImage2D` as RGBA/RGBA8, RGB/RGB8, SRGB8_ALPHA8 (WebGL 2) and
EXT_sRGB's SRGB_ALPHA_EXT (WebGL 1), and `texSubImage2D` of the whole frame
at 0,0 into an existing level 0 of one of those formats at least the
frame's size (`texStorage2D` textures, power-of-two textures). The ANGLE
entry point takes the page's internal format, format and type and defines
level 0 through `glTexImage2D`'s own validation; a copy into part of a
level clears the rest first (robust init), as `texSubImage2D` does. The
blit writes sRGB textures through their linear view (`GL_FRAMEBUFFER_SRGB`
off in the copy context: EXT_sRGB_write_control), so they get the frame's
bytes as `texImage2D` stores them. `test/webgl/video-formats.html` checks
each against the RGBA8 copy by reading the textures back through a
framebuffer (stored bytes) with the video paused: on the console, WebGL 2,
1080p H.264, two rounds, every case **PASS** with a maximum difference of 0
(RGBA8, RGBA, RGB8, RGB, SRGB8_ALPHA8, RGBA8 and SRGB8_ALPHA8
`texStorage2D` + `texSubImage2D`, 2048x2048 `texStorage2D` + `texSubImage2D`,
flipY off = flipY on upside down), no GL error. The WebGL 1 run (`?gl=1`)
hasn't run yet.

The same run found the sRGB copy slow: with an RGBA8 and an SRGB8_ALPHA8
texture filled from the video every frame the page ran at ~13 fps, the
WebGL thread at 76 ms of CPU per frame (`video.html`'s single RGBA8 copy
holds 60 fps). ps5-opengl's GPU blit (`ps5_blit_gpu_color`) wants the
resource's own format on both sides; a linear view of an sRGB texture took
the CPU fallback. `spikes/02-gl-egl/ps5-opengl/patches/0004` lets it draw
through the linear view (RGBA8 linear and sRGB share the native tiled
layout, as the driver's clear path notes). Built into
`toolchain/ps5-opengl-gles-noprof-m4` (the browser's own driver build is
unchanged) and packaged as PPSA99006 in `toolchain/wpe-webkit/titles-webgl-m4`;
not run yet (the console went off the network, below). RGB8 has no RGBX
format in this driver, so Mesa should store it as RGBA8 and take the GPU
path already; the `?show=` switch of `video-formats.html` times each half.

**Console incident (2026-11-07 14:30).** After the formats run ended
cleanly (wind-down, `main returned 0`), the next launch of PPSA99006 could
not create `/system_tmp/ps5wk` or its sockets: `errno 28` (ENOSPC) on
`ui-`, `video-` and `audio-<pid>.sock`. The children never connected, and
about two minutes later the console dropped off the network (ping: host
down). Nothing in the first run wrote to `/system_tmp` that the log shows;
what filled it is not known yet (to look at when the console is back:
`/system_tmp` listing over FTP, before launching anything).
It happened again at 19:39 after ~15 launches since the reboot (the same
ENOSPC on `ui-<pid>.sock`, then the console crashed during `--stop`).
Cause, found by listing `/system_tmp/ps5wk` after each launch: every run
leaves `datafree-<UIProcess pid>` (the child's free-space status for the
title, `downloads.c`), and `removeStaleRuntimeFiles` in `ps5wk-child.cpp`,
which already clears the other pid-named files of dead titles for exactly
this ENOSPC (a small tmpfs, ~14 launches), did not know that name. Fixed:
`datafree-<pid>` is in its list (the next launch logged `stale files:
removed 6`, the old `datafree-` files gone). Builds without the fix,
PPSA99020's among them, still leave one file per launch until a reboot.

**WebAssembly (2026-11-07).** On in WebProcesses with the JIT
(ps5wk-child; `PS5WK_WASM=0` in child.env turns it off), bounds-checked
memories. `test/wasm/wasm.html` passes (compile 9 ms; a 1M-step loop 0.41 ms
in wasm, 0.53 ms in JS; memory grown to 64 MB) and threejs.org's
`webgl_animation_keyframes`, blank before (Draco needs WebAssembly), shows its
animated scene.

Take option 1. Option 2 only if the owner wants WebGL-only players to run
at any speed before the title work is done; it shares M1 below.

| Milestone | Work | Done when |
| --- | --- | --- |
| M0 | **Done 2026-11-07.** Register PPSA99006, run the Spike 11 title; add the share-group probe (ANGLE external context sharing with GL 4.6 core) | `angle-triangle PASS`, 8.3 µs per draw; all `share-*` PASS |
| M1 | **Done 2026-11-07** (patches 0077–0083). `ENABLE_WEBGL` + ANGLE in the WPE build (patches carried in `port/webkit/patches`), `ENABLE_GPU_PROCESS` without GBM, `IPC::Semaphore` on `_umtx_op`, ANGLE linked into the title. Also the title host: WebKit's `GPUProcess` made in the title (`PS5GPUProcessInTitle.cpp`), each context a `GraphicsContextGLCoordinated` on the `RemoteGraphicsContextGL` work queue | a page creates a WebGL context and the stream connects (log only). `test/webgl/webgl.html`: WebGL 1 and 2 contexts, a triangle in each read back 0,255,0,255, no GL error, ~50 ms each |
| M2 | **Done 2026-11-07** (patch 0084). The coordinated buffer as a WC content layer, scene keyed per WebProcess | a WebGL triangle page and a three.js demo on the TV at 60 fps. `test/webgl/spin.html` (a WebGL 2 and a WebGL 1 canvas redrawn every frame) and three.js's `webgl_geometry_cube` example: both shown, 60 fps, compositor paint 0.7 ms |
| M3 | **Partly done 2026-11-07** (patch 0085, formats: uncommitted). `CopyTextureFromVideoStreamPS5` for hardware frames; software frames via the existing fallback | a three.js 360 player plays a 4K clip: **done** (`test/webgl/pano.html`, three.js r160, `vrtest_360_mono.mp4` 3840x1920 HEVC, 60 fps, upright, clock running); a VR tube site's own WebGL player: for the owner to check (not tested here); sRGB, RGB and `texSubImage2D` video textures: **correct** (`test/webgl/video-formats.html`, all PASS), the sRGB copy fast with driver patch 0004 (`?show=srgb` 60 fps, 0.35 ms per frame on the WebGL thread); WebGL 1 formats (`?gl=1`, incl. `SRGB_ALPHA_EXT`): all PASS, 60 fps; the software-frame fallback not run yet |
| M4 | **Started 2026-11-07** (uncommitted; profiled, first driver fix run). Cost: ANGLE/driver state caching (§4.2c), glthread, the texture cap with WebGL buffers, memory lines for the host. Done so far: the measurements (below) and `test/webgl/busy.html`; glthread as a knob; WebGL drawing buffers in the cap and the `[wc]`/`[mem]` lines; the title profiler. Profiled `busy.html`; driver patch 0005 (register defaults fetched once): 1000 cubes 32.7 -> 47.0 fps, 2000 16.8 -> 24.5; patch 0006 (no restart conversion when no index is the restart index): 54.7 and 29.9 (textured 52.7, 28.6); patch 0007 (four-entry MRT register cache): 59.8 and 32.9, so busy scenes stay above 30 fps to ~2000 draws per frame. Patch 0008 (GPU depth/stencil clears for 4x MSAA): three.js's antialiased cube 2.2 -> 0.24 ms per frame on the WebGL thread. Patch 0009 (prepared draws clear only the completion marker and flush only the register tables their stream loads, fenced once) and 0010 (one fence per CPU flush, not two): 2000 cubes 32.9 -> 37.7 -> 38.8 fps (~12.5 µs per draw). Patch 0011 (descriptor clear only the slots the shader lists, not 4 KiB per stage): 41.1 fps (~12 µs per draw). Patch 0012 (new descriptor snapshots skip the arena's clear): 42.9 fps (~11 µs per draw). Patch 0013 (inline equality for the per-draw cache checks, not libc `memcmp`): 46.4 fps (~10 µs per draw). 0014 (repeated draws skip unchanged state): 56.3 fps (textured 53.2). 0015 (batched draws fence their descriptor tables once): 59.8 fps (textured 56.3). 0016 + WebKit 0087 (Mesa's VAO fast path; ANGLE's VAO bind without a map search): 3000 cubes 39.5 -> 40.9 fps (textured 36.6 -> 37.8). `[mem]`: the title flat over 6 min of the sweep (direct memory 750-759 MB, no trend); web4's peak RSS +20 MB in 5 min (the page allocates every frame; a longer run to see it level). Next: the rest of the measured list below | busy scenes stay above 30 fps; `[mem]` flat |
| later | WebXR on the same process (Spike 5 runtime + §4.8) | — |

**M4 so far (2026-11-07):**

- `[webgl] title: <n> frames/s; the WebGL thread <ms> CPU per frame (max),
  <%> of a core; <n> contexts, drawing buffers ~<MB>` every 5 s while
  frames come (the thread CPU time between `prepareForDisplay`s: stream
  decoding, ANGLE and the driver for every context on that thread). The
  formats run above gave the first figures.
- WebGL drawing buffers (3 RGBA8 textures per context; the default
  framebuffer's depth, stencil and multisample buffers are GL objects WebCore
  makes like the page's own, not counted) in the `[wc]` stats (`WebGL <n>
  contexts (<n> made), <n> layers shown, drawing buffers ~<MB>`), in the
  title's `[mem]` line, and in the GL texture cap's total. Over the cap a
  page showing WebGL loses tile coverage as before but never goes to the
  CPU path (its canvases would go blank).
- glthread for the title's contexts while `/data/ps5webkit/gl-thread`
  exists (`PS5_GLTHREAD=1` before `eglInitialize`; logged). Off by default.
- The sampling profiler (`apps/common/profiler.c`) in the title while
  `/data/ps5webkit/title-profile` exists (its first line the
  `PS5WK_PROFILE` mode, empty = CPU time per thread); every thread made
  through titlemem's `pthread_create` wrapper is sampled (WebKit's WebGL
  work queue among them). Output `/data/ps5webkit/prof/title-<pid>.prof`;
  `test/prof/prof.py --elf <the package's llvm-pie.elf>` names the frames.
- What the draw path does (from reading ps5-opengl's draw path; the
  measured split is above). Each draw rebuilds its native command stream from scratch
  (`ps5_agc_gate2_run`, `ps5_agc_native_runtime.c` ~3573-4994): a 16 KiB
  work block memset and then clflushed, both shader packages parsed again
  and compared byte for byte against the cache, AGC's default register
  table scanned, ~180 graphics/viewport registers (16 viewports always),
  an L2 ACQUIRE_MEM, two RELEASE_MEMs, a walk over the stream to validate
  it, and a `getenv` and `sceKernelGetDirectMemorySize` call. On the
  Gallium side (`ps5_screen.c`): no dirty bits at all
  (`framebuffer_valid`/`viewport_valid` are validity flags);
  `ps5_encode_graphics_state` over 16 viewports, vertex layout and shader
  variant lookups, texture descriptors rebuilt and re-validated, each
  stage's 4 KiB constant block memset, rebuilt and clflushed
  (`ps5_prepare_constant`, including the whole bound range of every
  non-copied UBO), the eligibility loops, a CPU scan of every index for
  min/max, and ~10 atomic retains per draw. The likely order: clflush
  traffic first, then the per-draw register tables and command build,
  then package parse/compare and the work memset, then Gallium's
  re-encoding. The fix the notes call "state caching": dirty bits in the
  bind/set hooks, cached encodings on the context, and a prepared native
  work block per (shader pair, framebuffer, state) where a draw only
  writes user data, CB0 and the draw packet.
- **Measured** (`busy.html?n=1000`, WebGL 2, title profiler, 52 s; the
  WebGL work queue 94% of a core, 30 µs per draw; the page's JS ~1 µs per
  object). Of the title's CPU: `sceAgcGetRegisterDefaults`, called by
  every draw's `append_target_state`, **28%**, a single address in the
  system library (patch 0005 caches the pointer, checked again every 4096
  uses: 30 -> 21 µs per draw); clflush (`util_flush_inval_range`,
  `util_clflushopt_range`) ~14%; the kernel submit
  (`ps5_agc_gate2_batch_submit` -> `runtime_batch_api.submit`) 8.5%;
  `ps5_agc_set_cx_mrt_uncached` 5%; the 16 KiB work memset
  (`runtime_prepared_work_clear`) 4% and the 4 KiB constant memset
  (`ps5_prepare_constant`) 3%; primitive restart (always on in WebGL 2:
  every indexed draw goes through a `util_primconvert` context made and
  freed per draw, its indices rewritten and uploaded) ~4% plus its share
  of the flushes. ANGLE and IPC decode together under 10%. Sweep before
  and after 0005 (fps; WebGL thread ms per frame): 250: 59.9 (8.6) ->
  59.9 (6.2); 500: 59.8 (15.8) -> 59.9 (11.0); 1000: 32.7 (30.4) -> 47.0
  (20.9); 2000: 16.8 (59.2) -> 24.5 (39.4). GL error 0 throughout.
  With patch 0006 as well (restart conversion skipped when no index is
  the restart index): 500: 59.9 (10.1); 1000: 54.7 (17.9); 2000: 29.9
  (32.6); `?tex=1` 52.7 and 28.6 at 1000 and 2000.
  After 0006 the kernel submit fell from 8.5% to 0.7% (the draws reach the
  deferred batch). Profile after 0006: system code 35% (`gate2_run`'s
  16 KiB work memset 8%, `ps5_render_arena_allocate` 4.6%, the constant
  memset 4.3%), clflush 18% (`runtime_prepared_work_publish` 8%,
  `ps5_draw_vbo_locked` 6%), `ps5_agc_set_cx_mrt_uncached` 8% (fixed by
  0007: 1000 59.8 fps, 2000 32.9 fps, ~15 µs per draw).
- threejs.org `webgl_geometry_cube` (antialias, 1080p): 60 fps but 2.2 ms
  per frame on the WebGL thread, nearly all in `glClear`: the 4x MSAA
  depth/stencil clear takes the CPU path (`ps5_clear_words`, ~14%, plus
  ~14% clflush), after draining the GPU queue (spinning on completion,
  ~40%). Driver patch 0008 lets the GPU depth/stencil clear (the blitter's
  depth quad) take 4x targets: 0.24 ms per frame (60 fps, the picture right on
  the TV). `test/webgl/msaa-clear.html` checks every sample is cleared (each
  sample primed by a draw, cleared, probed, resolved): 4x and 1x
  renderbuffers of five depth formats and an antialiased canvas; depth,
  stencil, both, colour+depth+stencil, scissored: all 43 PASS on the console
  (and in desktop Chrome). `?bench=1` (1080p 4x, clear + 200 draws): 59.3 fps,
  3.15 ms per frame. `busy.html` unchanged (1000 59.8, 2000 32.9 fps).
- The per-draw work memset and flush (patch 0009): the 16 KiB of state pages
  were zeroed and clflushed on every draw. The GPU reads them only through
  the indirect register loads in the stream, so only those tables (and the
  completion marker) are flushed now, ~1-2 KiB. The first try flushed each
  table with `util_flush_inval_range` and gained nothing: it fences twice per
  call and the fence waits for every flush in flight, so the cost is per
  call, not per line (the publish rose from 8% to 14% of the WebGL thread).
  Fenced once per draw: 2000 cubes 37.7 fps; 0010 (one fence per flush
  everywhere): 38.8 fps. `msaa-clear.html` 0 failures, `webgl.html` PASS,
  video formats 20 PASS, three.js's cube right on the TV.
- The per-draw descriptor clear (patch 0011): constant preparation zeroed the
  whole 4 KiB descriptor prefix of each stage's recycled storage. Only the
  slots in the shader's binding metadata are cleared now (each writer
  already rewrites its own), with the whole clear kept for geometry and
  tessellation layouts. 2000 cubes 38.8 -> 41.1 fps (textured 39.0);
  `ps5_prepare_constant` 6% -> under 1% of the WebGL thread. `msaa-clear.html`
  0 failures, `webgl.html` PASS, video formats 23 PASS, the cube right.
- New descriptor snapshots (patch 0012): with 8 batches in flight and a
  256-per-stage reuse cache, most of the 3 x 16 KiB snapshots per draw at
  2000 draws are new arena allocations, each zeroed. Their readable bytes are
  rewritten before use (as for a recycled one), so they skip the clear: 2000
  cubes 41.1 -> 42.9 fps (textured 40.5). Same checks pass.
- Per-draw cache checks (patch 0013): the register-table, shader-pair and
  shader-variant caches compared bytes with the system libc `memcmp` (an
  out-of-module call, ~7% of the WebGL thread at 2000 draws). An inline SSE2
  equality check: 2000 cubes 42.9 -> 46.4 fps (textured 43.6). Same checks
  pass, the cube right.
- Repeated draws (patch 0014): a batched draw whose state equals the draw
  before it in the same command group emits only its own commands (user
  data, the draw, its completion), not the render-target, shader, depth,
  blend and viewport register tables rebuilt per draw (~10% of the WebGL
  thread). Groups are now decided as draws are queued, so a repeat knows it
  follows its predecessor in one stream. 2000 cubes 46.4 -> 56.3 fps
  (textured 53.2). Same checks pass, the cube right.
- Descriptor-table fences (patch 0015): a batched draw fenced each of its
  three descriptor tables; a fence waits for every older flush, so the three
  were ~13% of the WebGL thread. Now the first two flush unfenced and the
  last table's fence covers all three. 2000 cubes 56.3 -> 59.8 fps (textured
  53.2 -> 56.3). A version that left all three to the runtime's later fence
  stalled the video formats page once (cause unknown); this one passed it 3
  of 3.
- VAO binds (driver patch 0016, WebKit patch 0087): every draw binds its own
  VAO. Mesa rescanned the VAO each time to merge attributes sharing a buffer,
  because the ps5 screen never allowed the fast path most Gallium drivers use
  (one vertex buffer per attribute, the same addresses); now it does. ANGLE's
  GL backend looked up the VAO's state in a `std::map` on each bind though
  the vertex array already holds it; it passes it now. ~2.6% of the WebGL
  thread: 3000 cubes 39.5 -> 40.9 fps (textured 36.6 -> 37.8); 2000 was
  already at 60. ANGLE's own `VertexArray::onBind` (~5.5%: re-observing
  buffers, sizes, element limits) is still there. Same checks pass, the cube
  right.
- `test/webgl/busy.html`: lit cubes drawn as three.js draws meshes (per
  object a VAO bind, two matrix uniforms, a colour, optionally a texture,
  `drawElements` of 36), 100 to 2000 objects, 5 s each, one `RESULT
  webgl-busy n=<count> <fps> fps, JS <ms> per frame ...` line per step.

