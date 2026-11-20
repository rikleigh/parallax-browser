# Spike 15: every GPU user on RADV (no ps5-opengl)

_Started 2026-11-18, from `HANDOVER-radv.md` (local note). The owner wants the
compositor, browser UI, video, View in VR, WebGL and WebGPU all on RADV
(Mesa's Vulkan driver, `ref/PS5_Mesa`), and ps5-opengl out of the build.
Spike 15 picks the route with test titles before any browser work._

| | Question | Status |
| --- | --- | --- |
| 15a | ANGLE's Vulkan backend cross-built for the title, on RADV | **PASS** on the console and on the TV (owner checked), compositor-shaped load included (below) |
| 15b | Zink on RADV with a GL 4.6 core context | not run: Route A answered every question (Gate 1 below) |
| 15c | A T# for Hmd2 from a RADV `VkImage` | **PASS**, in the headset too (owner checked): layout checked texel by texel, 60 frames/s through Hmd2 for 60 s |
| 15d | Video into RADV memory, YUV to RGBA on RADV | **PASS** on the console and on the TV (owner checked): Videodec2 decodes into ANGLE's buffers, zero copy; conversion checked against the CPU; 3 to 4 times faster than on ps5-opengl |
| 15e | Sharing images between `VkDevice`s (Dawn's and ANGLE's) | **PASS** on the console, raw Vulkan: tiled images shared zero-copy both ways by host-pointer aliasing; the linear fallback works too. Dawn itself needs a patch (below) |

## 15a: ANGLE on Vulkan on RADV, in a title with no ps5-opengl

Console runs on 13.60, title PPSA99021, 2026-11-18:
[results/console-15a-2026-11-18.log](results/console-15a-2026-11-18.log),
then, with the 1080p swapchain fix and the compositor load,
[results/console-15a-compositor-2026-11-18.log](results/console-15a-compositor-2026-11-18.log)
(17 checks, all PASS).
The checks and workloads are Spike 11's, so the right-hand column is the
same code through ANGLE's GL backend over ps5-opengl
(`spikes/11-webgl/results/console-2026-11-07.log`, the `gles-async` build).

| Check | ANGLE on RADV (Vulkan) | ANGLE over ps5-opengl (Spike 11) |
| --- | --- | --- |
| Display | EGL 1.5 in 7 ms | EGL 1.5 |
| Context | **ES 3.1**, 175 extensions; renderer "ANGLE (AMD, Vulkan 1.4.354 (PlayStation 5 GPU (RADV NAVI21)), radv-26.2.0)" | ES 3.0, 110 extensions |
| WebGL extensions (Spike 11's list of 19) | all 19, `OES_texture_float_linear` included | 18 (no `OES_texture_float_linear`) |
| ES 3.1 for the compositor's video shader (Route A) | 64 SSBO bindings, compute | n/a |
| ESSL 300 es compile + link | 1.5 to 5.5 ms (SPIR-V; pipelines built on ANGLE's worker threads) | 4.9 ms |
| Triangle, texture, fence, `#version 100` (WebGL 1) | PASS, exact colours | PASS |
| 2000 strip draws + 2 `uniform4f` each, one frame, offscreen | **0.93 ms; 0.2 µs CPU per draw** | 17.54 ms; 8.3 µs per draw |
| 1920x1080 RGBA `texSubImage2D` + finish | **0.61 ms** | 34.5 ms |
| 256x256 `readPixels` | 0.06 ms | 1.05 ms |
| Window surface on `VK_KHR_display` (VideoOut), swap interval 1, 600 frames | **60.1 frames/s**, worst interval 16.9 ms, none over 20 ms | not measured in Spike 11 |
| The same with 2000 draws a frame | **59.9 frames/s**, none over 20 ms; 0.54 ms CPU to issue a frame (0.3 µs a draw) | |
| Compositor-shaped 1080p frame, offscreen (306 layers, 6 shader variants; 42 program switches, 182 texture binds, 80 scissor changes, 4 tile uploads a frame) | **0.68 ms a frame** = 0.28 ms CPU (0.9 µs a layer) + 0.39 ms GPU wait; worst 1.06 ms | not measured |
| The same frame on the TV, 600 frames | **60.1 frames/s**, worst interval 17.1 ms, none over 20 ms; 0.37 ms CPU a frame | |
| Compiling + linking the 6 variants | 115 ms (about 19 ms each; first frame then 1.7 ms) | |
| Title image (eboot) | 24.8 MB: ANGLE + RADV | 26 MB: ANGLE + ps5-opengl |

Read-outs:

- **Per-draw cost and uploads.** Both are well inside Gate 1's "at least as
  good as today". The draw bench changes only uniforms between draws, which
  ANGLE turns into dynamic offsets. Texture binds, blend changes and program
  switches, which the compositor does all the time, are not measured yet.
  The upload is a staging copy on the GPU: there's no CPU detiling as in
  ps5-opengl (`COMPOSITING-PLAN.md`, 210 MB/s).
- **Present.** In the present cases, "issue" is wall time, and it holds the
  wait for the next swapchain image: ANGLE acquires at the first draw of a
  frame. So the CPU columns are the cost, and the swap itself took 0.04 to
  0.4 ms.
- **Swapchain size (fixed).** The first runs' window surface came out
  3840x2160 although the native window asked for 1920x1080:
  `WindowSurfaceVkSimple` took its size from the surface's `currentExtent`,
  which RADV reports as the display mode's (the TV is 4K), while taking any
  size from 1x1 up to it. Patch 0003 sizes ANGLE's simple surface by the
  native window (clamped to RADV's min and max), so the swapchain is
  1920x1080, a size VideoOut shows without a blit. The fix is in ANGLE, not
  RADV, because RADV's answer is what upstream Mesa's display WSI gives.
- **The compositor's shape costs little.** The 306-layer frame changes
  program, texture, blend and scissor state the way TextureMapper does, and
  issues in 0.28 ms of CPU. The first frame took 1.7 ms, so turning blend
  and scissor on and off added no pipeline compiles at draw time.
- **Shader compiles are where the stalls are.** ANGLE builds each program's
  pipeline when it links it, so the 6 variants cost 115 ms there (about 19 ms
  each, with RADV's disk cache off in this spike) and the first frame is
  1.7 ms. In the browser, a variant linked the first time a page needs it is
  a dropped frame or so. To do in the port: link TextureMapper's common
  variants at startup, and look at RADV's disk cache on the console
  (PS5_Mesa's `disk-cache-on-console` change, which this RADV has).
- **Window configs.** RADV's VideoOut WSI offers `B8G8R8A8` surface formats
  only, and ANGLE refuses (`EGL_BAD_MATCH`) a window surface on an RGBA8
  config. The BGRA8 config works (config id 17 here).

### What it took

The build, `build-angle-vk.sh`:

- WebKit's ANGLE copy (`toolchain/wpe-webkit/src-webgl/Source/ThirdParty/ANGLE`,
  2.1.29096) has every Vulkan backend source and the pre-generated internal
  shaders, but no `Vulkan.cmake`. [angle/gen-vulkan-cmake.py](angle/gen-vulkan-cmake.py)
  writes one from `vulkan_backend.gni`. ANGLE's own `gni-to-cmake.py` needs
  `ply`.
- The dependencies:
  - Vulkan headers come from RADV's install.
  - SPIRV-Headers, and SPIRV-Tools' headers (only those), come from Dawn's
    checkout.
  - VulkanMemoryAllocator is fetched at ANGLE's DEPS pin (`7e55b01`, from
    Chromium's mirror: GitHub doesn't serve that commit) and compiled with
    VMA's GN defines.
- [angle/CMakeLists.txt](angle/CMakeLists.txt): the Vulkan backend with the
  "simple" `VK_KHR_display` display, volk, and no GL backend. Two notes:
  - The ESSL translator calls the atan emulator, which ANGLE lists with the
    GLSL output. Only that one file is added.
  - `angle_spirv_utils.cpp` (SPIR-V validation, used from asserts only)
    becomes a stub, [src/ps5_spirv_utils.cpp](src/ps5_spirv_utils.cpp).
- Patches:
  - [0001](patches/0001-ps5-static-vulkan-lookup.patch): ANGLE takes RADV's
    `vk_icdGetInstanceProcAddr` from the host program instead of
    `dlopen("libvulkan.so.1")`, like Dawn's patch 0002.
  - [0002](patches/0002-vk-helpers-row-depth-skip-bytes.patch): WebKit's
    `changes.diff` gives one Vulkan caller of `computeRowDepthSkipBytes` a
    signature its header doesn't have. WebKit never builds the Vulkan
    backend, so nothing caught it.
  - [0003](patches/0003-ps5-simple-surface-window-size.patch): the window
    surface takes the native window's size (above).
- The archive is 22.6 MB, built in about 5 minutes at `-j10`.

The title, `build-title-15a.sh` and [package-title.sh](package-title.sh).
The packager is P0b's with ps5-opengl taken out: the SDK's `libSceAgc.so`
and `libSceAgcDriver.so` stubs, and libc without `syscalls.o`, as the
browser links it. What the title then has to supply itself is in
[src/radv_title.c](src/radv_title.c):

- `open_memstream`, which ps5-opengl's shims used to provide (RADV uses it
  for diagnostics only);
- `__cpu_model`, and `posix_fallocate`, `fstatat` and `readlink` (which
  `syscalls.o` provided, and which libkernel doesn't export to titles);
- `dlopen`, `dlsym`, `dlclose` and `dladdr` as "nothing to load", because
  libc's `dlfcn.o` imports `__dladdr`/`__dlclose`, which a title can't;
- `angle_ps5_vk_get_instance_proc_addr`.

### Found on the way (for the port)

- **Thread stacks.** The first runs died in RADV's `nir_opt_varyings` on
  `ANGLE-Worker`, writing below a 64 KiB stack (SIGSEGV, "user write data,
  page not present"). ANGLE compiles pipelines on `std::thread` workers that
  ask for no stack size, and a title's default is 64 KiB. The fix here is
  the crash guard's `__wrap_pthread_create`, which gives 2 MiB. In the
  browser, `titlemem.c` raises small stacks to 2 MiB only when a WebProcess
  runs in the title (`ps5wk_title_mem_exec`), so ANGLE's workers in the
  title need the same.
- **The crash guard can't report a crash in a title.** Its stack scan
  (`afterCall` in `port/webkit/apps/common/crashguard.c`) reads the code
  bytes before each candidate return address. A title's text is
  execute-only, so the guard itself died with `SYSTEM_XO_VIOLATION`, and the
  kernel then reported the guard's registers instead of the first fault's.
  The spike doesn't install the guard; the kernel's report (klogsrv) gave
  the real fault. The browser title links the same guard. Not changed here.
- Reading kernel crash reports: the ELF is linked at 0 and loaded at
  0x400000. Subtract that from `rip` and the backtrace before symbolizing
  against `build/title-<ID>/llvm-pie.elf`.

### Not done yet in 15a

- The owner hasn't looked at the compositor frame on the TV yet: for 10 s
  after the pulsing colour, scrolling checkered tiles under moving panels,
  some rounded, some grey or sepia, with small coloured squares on top.
- Shader compile time with RADV's disk cache on, and across runs.
- Sharing with WebGL contexts in one share group, EGL images from
  `VkImage`s (`EGL_VULKAN_IMAGE_ANGLE`), and timer queries.
- Memory (direct and flexible) and a long run.

## 15c: an Hmd2 eye buffer from a RADV `VkImage`

Console runs on 13.60, title PPSA99022, 2026-11-19: without a headset,
[results/console-15c-2026-11-19.log](results/console-15c-2026-11-19.log), and
with the PS VR2,
[results/console-15c-headset-2026-11-19.log](results/console-15c-headset-2026-11-19.log).
Source: [src/vr_tsharp.cpp](src/vr_tsharp.cpp), built by `build-title-15c.sh`.

Today `PS5RendererVR` hands Hmd2 its three GL eye textures as T#s through
`eglExportTextureDescriptorPS5` (ps5-opengl patch 0002), in `SW_64KB_R_X`.
15c does the same with WebKit's future stack, ANGLE's GL on RADV, and **no
change to PS5_Mesa or ANGLE**:

1. ANGLE's own `VkDevice`, from `EGL_EXT_device_query` and
   `EGL_ANGLE_device_vulkan`.
2. On it, a `VkImage` chained with Mesa's internal
   `wsi_image_create_info { scanout = true }` (the struct RADV's own
   swapchain uses). PS5_Mesa's RADV lays a scanout image out in the
   display's fixed swizzle, `SW_64KB_R_X`, with DCC off
   (`radv_image.c`, `has_fixed_display_swizzle`), which is the mode Hmd2
   takes.
3. Memory for it from RADV, host-visible, mapped. A PS5 RADV buffer has the
   same address for the CPU and the GPU (`winsys/ps5/radv_ps5_bo.c`), so the
   mapping *is* the address the T# carries.
4. The image as a GL texture and FBO through `EGL_ANGLE_vulkan_image`
   (`EGL_VULKAN_IMAGE_ANGLE`), with `glAcquireTexturesANGLE` and
   `glReleaseTexturesANGLE` around each frame (the WebXR hand-off).
5. The T# from `psvr2_build_tsharp_rgba8(address, ..., 27)`.

| Check | Result |
| --- | --- |
| ANGLE's `VkInstance`/`VkPhysicalDevice`/`VkDevice` | PASS |
| The scanout image, 4096x2048 RGBA8 (3 of them, as `PS5RendererVR`'s 2048 eyes side by side) | 33,554,432 bytes, exactly a grid of 64 KiB tiles; 64 KiB aligned; memory type 3 (device-local, host-visible, coherent) |
| As a GL texture + FBO | PASS (complete) |
| **Layout**: a coordinate-coded pattern drawn by GL, read raw from memory through the console-measured RGBA8 `SW_64KB_R_X` map (PS5_Vulkan's `ps5vk_tiled_texel_offset`) | **0 of 8,388,608 texels wrong**. Read with rows flipped, or as linear, every sampled texel is wrong, so the check discriminates |
| Row order | Memory row 0 is GL row 0 (`gl_FragCoord.y` 0.5). A frame for Hmd2 has its top at GL y = 0, as Spike 3b's `flip_y` did on ps5-opengl |
| T#s | built for the three buffers (e.g. `40004000 c3800000 01ffc3ff 91b00fac 0 00700000 0 0` at `0x4000400000`) |
| Hmd2 | `sceHmd2Initialize`, `sceHmd2Open`, the tracker, reprojection, VR mode and `RegisterBuffers2` all accept the RADV T#s |
| **In the headset**, 60 s | **3601 frames, 60.0 frames/s, all tracked, 0 submit errors, none over 16 ms**; drawing the card + `glFinish` 0.22 ms a frame. The owner saw the card as drawn (below) |

Read-outs:

- **The browser's VR path has an answer on Route A** without a fork hook:
  the eye buffers become `VkImage`s that `PS5RendererVR` creates on ANGLE's
  device, instead of GL textures it exports. The Mesa-internal `sType`
  (`1000001002`) is the one private piece. It is stable in Mesa (the WSI
  code depends on it), but it is not API: if it ever goes, a one-line
  PS5_Mesa extension struct replaces it.
- **Cache coherency for the raw read.** The check reads the memory after a
  `glReadPixels`, whose host-read barrier makes RADV write the GPU's L2
  back. Hmd2 samples through the GPU, and on ps5-opengl a `glFinish` a
  frame was enough (Spike 3b). Whether it is on RADV is what the headset
  run shows: stale tiles or flicker would mean it isn't.
- **Hmd2 refuses a title without PS VR2 attributes** (`0x8a72001f` from
  `sceHmd2Initialize`), as Spike 3b found. `build-title-15c.sh` copies
  PS5VR's into `param.json`, as the browser's `package.sh` does.

The headset check (owner, 2026-11-19). The card is head-locked and drawn
in angles (tangents), so both eyes agree on it: a white grid on dark grey,
one green bar sweeping left to right once a second, a yellow band above the
centre, a red square lower left in the left eye's buffer only and a blue one
lower right in the right eye's only. The owner saw all of it as drawn,
closing each eye in turn: so the image is clean (layout), up is up (rows),
yellow is yellow (channel order) and each eye gets its own half. Also no
flicker or stale tiles, so a `glFinish` a frame is enough for Hmd2 on RADV,
as on ps5-opengl.

A first card drew its shapes at the same *pixels* in both eyes, and the
owner saw two bars: each eye's buffer covers different angles (1.84 outward,
0.95 inward, as tangents), so the same pixel is a different direction in
each eye. The browser already places its head-locked hint in angles
(`PS5RendererVR`'s shader: `tan_lrud`, `hintRect`), so nothing changes there
when it moves to RADV.

Not done yet in 15c:

- The TV swapchain and Hmd2 at the same time (the Risks list's display
  ownership).

## 15d: hardware-decoded video into RADV memory, converted on ANGLE

Console run on 13.60, title PPSA99023, 2026-11-19:
[results/console-15d-2026-11-19.log](results/console-15d-2026-11-19.log).
Source: [src/video_15d.cpp](src/video_15d.cpp), with Spike 9's `vdec2.c` and
`au.c`; built by `build-title-15d.sh`.

Today Videodec2 decodes into ps5-opengl GL buffers, and `PS5Renderer`
converts each picture with a `#version 430 core` fragment shader that reads
the buffer as an SSBO (`PS5VideoGL`; Spike 9). 15d does the same on Route A,
again with **no import and no copy**:

- The decoder's 8 output slots are ANGLE GL buffers with persistent,
  coherent mappings (`GL_EXT_buffer_storage`). A PS5 RADV buffer is direct
  memory type 12, mapped CPU+GPU read/write at one address
  (`winsys/ps5/radv_ps5_platform.c`): the decoder's own type, and what
  ps5-opengl used. So the mapping goes to `vdec2_open` as it is, and the
  handover's `VK_EXT_external_memory_host` import isn't needed (ANGLE only
  enables that extension for OpenCL anyway).
- The conversion is the browser's shader, unchanged but for
  `#version 310 es` and precision qualifiers (ES 3.1 on ANGLE has 64
  fragment SSBO blocks, 1023 MB each, offset alignment 4). A compute variant
  writes an `rgba8` image.
- Each picture is shown letterboxed on the TV.

| Clip | Pictures (10 s, at vsync) | vs CPU conversion, 64 points on 2 pictures (max channel difference) | Fragment pass: CPU to issue / GPU / to `glFinish` | Compute pass: GPU / to `glFinish` |
| --- | --- | --- | --- | --- |
| H.264 1920x1080 (fMP4) | 600, 59.9/s, 0 errors | fragment 1, compute 0 | 0.027 / 0.089 / **0.17 ms** | 0.037 / 0.11 ms |
| HEVC 3840x2160 (fMP4) | 598, 59.8/s, 0 errors | fragment 1, compute 0 | 0.039 / 0.139 / **0.24 ms** | 0.141 / 0.21 ms |
| VP9 Profile 2 3840x2160, 10-bit (IVF) | 571, 57.0/s, 0 errors | fragment 1, compute 0 | 0.054 / 0.245 / **0.42 ms** | 0.482 / 0.56 ms |

Every picture lay inside the slot handed to the decoder, and the luma
spread over a grid was 21 to 221, so the decoder really wrote into RADV
memory. GPU times are from `EXT_disjoint_timer_query`; "to `glFinish`" is
wall time on the CPU and bounds them from above.

Against today:

- **The renderer's own number** is "copy+convert 0.47 ms per 1080p frame"
  (`port/webkit/README.md`): CPU wall time to issue, no `glFinish`. The same
  measure here is 0.027 ms at 1080p.
- **To completion**, Spike 9 timed ps5-opengl's pass at 0.81 to 0.89 ms for
  4320x2160 HEVC and 1.97 ms for 4K VP9. On RADV a 4K picture is converted
  and finished in 0.24 ms, and a 4K 10-bit one in 0.42 ms.
- **The VP9 10-bit rate** (57/s) is the decoder's: Spike 9 measured 54.1/s
  for 4K VP9 on ps5-opengl. Presenting waited 7 ms a frame here, against
  16 ms for the other clips, so the GPU side had time to spare.

The owner watched a second run on the TV (2026-11-19): all three clips
looked right (colours, no blocks, tearing or flicker).

Read-outs:

- **Fragment or compute.** For 8-bit they cost about the same; for 10-bit
  the fragment pass is twice as fast. Keep the fragment pass, as the browser
  has it, which also keeps the "texture row 0 is the picture's top" rule.
- **The fragment pass is 1 off the CPU at some points, the compute pass
  exact.** Both run the same arithmetic, so it's in how the float result
  becomes 8 bits (the colour target's conversion against `imageStore`'s);
  not looked into further. One step in 255 doesn't matter for video.
- **For the port:** `PS5VideoGL` keeps its shape. `glBufferStorage` becomes
  `glBufferStorageEXT`, and the decoder buffer provider allocates ANGLE
  buffers instead of ps5-opengl ones. The `VK_EXT_external_memory_host` route
  stays a fallback for memory that ANGLE didn't allocate.

Not done yet in 15d:

- 8K and HEVC Main10, and several streams at once (memory, not
  conversion, is what limits those: Spike 9).
- Video into WebGL (patch 0085, a blit into ANGLE's texture today) and into
  WebGPU (15e).

## 15e: images shared between two `VkDevice`s in one process

Console run on 13.60, title PPSA99024, 2026-11-19:
[results/console-15e-2026-11-19.log](results/console-15e-2026-11-19.log).
Source: [src/vk_share_15e.cpp](src/vk_share_15e.cpp), built by
`build-title-15e.sh`.

The handover's questions, answered from the code first:

- **Do Dawn and ANGLE take an existing `VkInstance`/`VkDevice`?** No.
  Neither has a way in (Dawn's `VulkanBackend.h` only hands its own out;
  ANGLE has no such display attribute). Each makes its own device.
- **Does PS5_Mesa's RADV export memory?** No. The PS5 winsys sets
  `has_external_fd = false` (`buffer_get_fd` fails), so no
  `VK_KHR_external_memory_fd`, `_semaphore_fd` or `_fence_fd`, and the
  console agrees (opaque-fd export features 0). Dawn's and ANGLE's usual
  interop (opaque fd / dma-buf, `GL_EXT_memory_object_fd`) has nothing to
  stand on.
- **But** one RADV in one process has one GPU address space. A RADV
  buffer's GPU address is its CPU address, and a second device's
  host-pointer import (`VK_EXT_external_memory_host`, 16 KiB alignment)
  maps the same memory at the same address
  (`radv_ps5_buffer_from_ptr`) and never takes it back when freed. RADV
  lays an image out from its create info alone. So a second device can
  alias ANGLE's image memory with an identical `VkImage`: zero copy, for
  tiled images too, not only the linear buffers the handover expected.

The test: ANGLE's device, and a second `VkInstance`/`VkDevice` on the same
RADV standing in for Dawn's, with only `VK_EXT_external_memory_host`
enabled (what Dawn would need). The image is 1920x1080 RGBA8, carrying
15c's coordinate pattern, and every texel is checked.

| Check | Result |
| --- | --- |
| A to B: ANGLE draws into its image with GL; the second device copies its alias out | **0 of 2,073,600 texels wrong**; the second device's copy + wait 0.08 ms |
| B to A: the second device copies the pattern into its alias; ANGLE reads its image (taken over with `glAcquireTexturesANGLE` in the layout B left) | **0 of 2,073,600 wrong**; B's copy + wait 0.14 ms |
| Control: the same with ANGLE's image made *without* the external-memory struct | 518,400 and 2,073,600 wrong: ANGLE's image is then 8,896,512 bytes against 8,847,360 (DCC metadata) |
| Linear fallback: the second device's own image copied into its import of an ANGLE GL buffer (Dawn's `CopyTextureToBuffer` into host-mapped memory), then ANGLE's upload from that buffer on the GPU (`PIXEL_UNPACK`) | 0 wrong; **0.05 + 0.10 ms a frame** at 1080p, each to completion |

The rule the control shows: RADV changes an image's layout when it has
external handle types (no DCC, no HTILE, "shareable": `radv_image.c`). So
both devices must create the image with
`VkExternalMemoryImageCreateInfo { HOST_ALLOCATION }`. ANGLE's side does
that because the browser creates that `VkImage` itself and hands it to
ANGLE (`EGL_VULKAN_IMAGE_ANGLE`, as 15c). ANGLE's device doesn't enable
`VK_EXT_external_memory_host`, so naming that handle type there is
outside the spec; RADV accepts it, and a one-line ANGLE patch (enable the
extension, which ANGLE already does for OpenCL) would make it proper.

Synchronisation: the devices share no semaphores, so the hand-off is a CPU
wait (the producer's fence or `glFinish`). Where Phase 4 puts that wait
(on the compositor's thread or Dawn's) is a design point. Both devices are on the same GPU and L2, and that was
enough here in both directions.

What it means for Phase 4 (WebGPU):

- **Canvas present (0110/0132):** the compositor (ANGLE) allocates the
  canvas's swap images as above. Dawn renders into its alias, and the
  compositor samples its own image. There is no `CopyTextureToBuffer` +
  `MapAsync`, and no CPU copy.
- **Video into WebGPU (0123/0124):** the converted RGBA video texture
  (15d) is made the same way, and Dawn imports its alias as an external
  texture. That drops today's GPU to CPU to GPU trip (38.5 frames/s for
  1080p).
- **Dawn needs a patch for this:** a host-pointer kind of
  `ExternalImageDescriptorVk` (import the memory with
  `VkImportMemoryHostPointerInfoEXT`, create the image from the given
  create info, no semaphores), next to the opaque-fd and dma-buf ones in
  `src/dawn/native/vulkan/external_memory/`. If that's more than wanted,
  the linear fallback needs only Dawn's `HostMappedPointer` feature turned
  on for this device (stock Dawn switches it off for AMD:
  `PhysicalDeviceVk.cpp`), at 0.15 ms a frame for 1080p.
- An alternative is a PS5_Mesa change: in-process "opaque fds" for
  buffers (a handle table in the winsys), so that stock Dawn's and ANGLE's
  opaque-fd paths work. It's more code in the fork, and semaphores would
  still be missing.

## Gate 1: Route A or Route B

The handover: "Go with A when 15a's per-draw cost and present rate are at
least as good as today's, and 15c to 15e have an answer for it."

| Question | Route A (ANGLE on Vulkan on RADV) |
| --- | --- |
| Per-draw cost | 0.2 µs CPU a draw, against 8.3 µs today (ps5-opengl through ANGLE's GL backend) |
| Uploads | 1080p `texSubImage2D` 0.61 ms, against 34.5 ms |
| Present | 60 frames/s on the TV, with 2000 draws a frame and with a 306-layer compositor frame (0.37 ms CPU) |
| VR (15c) | Hmd2 takes RADV images' T#s, no fork change; 60 frames/s for 60 s in the headset, checked by the owner |
| Video (15d) | Videodec2 decodes into ANGLE's buffers, zero copy; 4K converted in 0.24 ms, against 0.81 to 0.89 ms; checked on the TV |
| WebGPU sharing (15e) | Zero-copy aliasing of tiled images both ways; needs a Dawn patch (or the 0.15 ms linear fallback) |
| Risks seen | Shader compiles at link time (about 19 ms each); ANGLE's workers need big stacks; the crash guard can't run in a title (XO) |

**All of 15a and 15c to 15e have an answer on Route A**, and every number
is better than today's. 15b (Zink) was not run; it would only matter if
Route A had failed somewhere.

**Decision (owner, 2026-11-19): Route A.** Next are Phase 0's baselines on
the release build, then Phase 2.

Left open for Phase 2 (none of them blocks the route):

- The TV swapchain and Hmd2 at the same time (display ownership).
- Shader compile stalls: link the compositor's variants at startup, and
  look at RADV's disk cache.
- Memory over a long run, and GPU faults (none provoked).

## Phase 0: the release build's numbers (the A/B baseline)

Release browser 0.2.2 (PPSA99207, ps5-opengl), console 13.60, 2026-11-19:
[results/phase0-20261119-013048/](results/phase0-20261119-013048/), made by
`phase0/run.sh` (summary: `phase0/summary.py <dir>`). Every page is local.
Each case starts the browser as a test run, with its page as the only test
URL, so the owner's saved session is neither read nor written. The
compositor cases scroll with `pad-scroll-test` (`delay=8 secs=4 ry=0.5
pause=2`, the right stick at half deflection) and log `wc-scene-stats`.

| Case | Frames/s best (median of 5 s windows) | Render ms | Scene paint ms, median (max) | Draws per paint | Tile upload ms each | Video convert ms |
| --- | --- | --- | --- | --- | --- | --- |
| `static.html` (sticky header, cards) | 59.9 (42.5, with pauses) | 1.22 | 0.25 (14.5, the first paint) | 20 | 0.136 | |
| `wcfeed.html` (long feed with videos, 1 min) | 60.0 (59.9) | 2.68 | 1.34 (3.07) | 52 | 0.129 | 0.21 |
| `scrolljank.html?layers=2800&cluster=2500` | 59.6 (51.4) | 13.49 | 10.0 (13.6) | 844 | 0.066 | |
| 1080p30 H.264 video | 60.0 (59.9) | 1.04 | | | | 0.29 |
| 2160p30 HEVC video | 59.9 (59.9) | 1.35 | | | | 0.80 |
| 2160p30 HEVC 10-bit video | 60.0 (59.9) | 2.11 | | | | 2.26 |

- **2800 layers:** the median scene paint depends on where the scroll is
  (5.3 ms and 426 draws in a first run, 10.0 ms and 844 draws here); the
  maximum, 13.6-13.9 ms, held in both. That is ~16 µs per draw.
- **WebGL:** `readpixels.html` 29/29. `busy.html` (three.js's way of drawing
  meshes) 59.8 / 59.9 / 59.9 / 60.0 / 59.3 frames/s at 100 / 250 / 500 /
  1000 / 2000 objects, JS 0.28 / 0.47 / 0.87 / 1.50 / 2.36 ms per frame.
  `webgl/video.html` (1080p video into a texture every frame) 60.0
  frames/s, 120 uploads per 2 s, GL error 0. The three.js example pages
  need the network (a CDN), so they are not in the baseline.
- **WebGPU:** all 14 pages pass (adapter 11/11, device 29/29, compute 6/6,
  offscreen-readback 7/7, errors 11/11, buffer-map 11/11, triangle 3/3,
  cube 3/3, resize 10/10, two-canvases 7/7, todataurl 11/11,
  copy-external-image 16/16, video-texture 5/5, webgl-and-webgpu 5/5).
  The canvas pages run at 59.9-60.0 frames/s, `video-texture` included.
- **VR** (`vr-sim` + `vr-selftest` on the 1080p video page): self-test PASS
  (15 eye-buffer captures, 8 projections, controls, 4 entries in 71 s).
  Eye buffers 4096x2048 x3, 4 samples; 56.5 headset frames/s simulated
  (3115 in 55 s), render 1.70 ms avg per headset frame, 0 begin or submit
  errors.
- **Overlay GPU time:** not measurable today (ps5-opengl's timer queries
  are CPU clocks, `OVERLAY.md`), so there is no baseline.
- **Memory** (the title's `[mem]` lines): direct memory 630-790 MB in
  use on the plain pages, 1.0-1.2 GB with 4K video, 2.0 GB on the video
  feed; flexible memory free stays at 101-242 MB (lowest on the feed).
  **Seen, not explained:** in the WebGPU run (one browser start, the
  pages opened one after another) direct memory went from 723 MB to 3778
  MB at `buffer-map` (round trips up to 64 MiB) and kept rising to 4274
  MB by the last page. It was not given back after the pages were left.
  Look at this again on RADV-only builds (Phase 4).
- **Owner check (2026-11-19), all good:** on the TV, the 4K HEVC test
  clip (running clock, a flash and a beep each second) played smoothly and
  in sync, 30 frames/s with none dropped. In the headset, View in VR on
  the 1080p clip: steady, fixed in space, smooth. Log: 60.0 headset
  frames/s once up, render 0.86-0.99 ms avg, 30 video frames/s shown, 0
  begin or submit errors; headset up in 723 ms, released in 1218 ms.

## Phase 2: what the title's GL needs on ANGLE (GLES)

Read from the sources on 2026-11-19 (line numbers at `2e7d311`). The browser's
own UI is drawn on the CPU (`browserdraw.c` into `PS5UICanvas`'s premultiplied
BGRA pixels; toasts and the VR panel through FreeType in `PS5Text.cpp`) and
handed to `PS5Renderer`, which uploads and draws it. Only the renderer, the VR
renderer and the perf overlay make GL calls. What changes on ES 3.x:

- **Context:** `eglBindAPI(EGL_OPENGL_API)`, `EGL_OPENGL_BIT` and a 4.6 core
  profile (`PS5Renderer.cpp:441-451`) become an ES 3.1 context on ANGLE's
  display.
- **Shaders:** every one is `#version 430 core` with no precision qualifiers
  (`PS5Renderer.cpp:84, 95, 110, 126, 143, 154`; `PS5RendererVR.cpp:145, 157,
  1158`; `PS5PerfOverlay.cpp:529, 544`). They need `#version 310 es` and
  `precision` lines. The page, UI and convert shaders read std430 SSBOs in the
  fragment stage: ES 3.1 allows it when `GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS`
  is above 0 (15d's convert shader ran this way on ANGLE).
- **Persistent mapped buffers:** `glBufferStorage` for the decoder slots
  (`:399-404`), the page buffer (`:925-929`), the video stream buffers
  (`:1189-1193`), the UI buffer (`:1508-1512`) and the overlay's vertices
  (`PS5PerfOverlay.cpp:682-684`) need `GL_EXT_buffer_storage`, which ANGLE on
  RADV has (15a, 15d).
- **BGRA uploads:** `glTexImage2D(GL_RGBA8, ..., GL_BGRA)` (UI fallback
  `:1538`, page fallback `:971, 974, 983`) is invalid on ES. Use
  `GL_BGRA_EXT` as the internal format too (`EXT_texture_format_BGRA8888`),
  or swizzle in the shader.
- **sRGB:** `glEnable/glDisable(GL_FRAMEBUFFER_SRGB)` (`:1128, 1137`) needs
  `EXT_sRGB_write_control`.
- **Fine on ES 3:** `glBlitFramebuffer` with a Y flip (`:1136`, formats must
  match), fence syncs (`:1064-1143`), `GL_UNPACK_ROW_LENGTH`, RGBA readbacks,
  `glTexStorage2D(GL_RGBA8)`, premultiplied blending.
- **ps5-opengl private:** `ps5_agc_gate2_wait_present` (`:46`, `:797`), the
  submission lock (`:42-66`) and `eglExportTextureDescriptorPS5`
  (`PS5RendererVR.cpp:677, 692`), which 15c's T# path replaces.
- **Overlay GPU time:** there are no timer queries in the code; `OVERLAY.md`
  shows `GPU n/a` because ps5-opengl's queries are CPU clocks. On ANGLE,
  `EXT_disjoint_timer_query` gives real GPU time (15d timed its conversion with it).

## Phase 2 progress (2026-11-19 night)

**Build switch.** `GPU=radv port/webkit/build.sh` builds the browser with
the title rendering on ANGLE's Vulkan backend over RADV:

- Its own worktree `toolchain/wpe-webkit/src-radv` (branch `ps5-radv`,
  made from `ps5-webgl` at `242db6ea24`) and build dir `build-radv`. The
  release tree and build are untouched.
- `GPU=radv port/webkit/build.sh export-patches` writes the branch's own
  commits to `port/webkit/patches-radv/`, and a fresh `src-radv` applies
  them.
- VulkanMemoryAllocator is fetched once into `toolchain/wpe-deps/src/vma`
  (ANGLE's DEPS revision).
- `TITLE=PPSA99208 NAME="Parallax RADV test" BLD=toolchain/wpe-webkit/build-radv
  port/webkit/apps/title/package.sh` packages it as the browser test title.

**WebKit's ANGLE gets the Vulkan backend** (`PlatformWPE.cmake`, when
`PS5WK_GPU_RADV`), next to the GL backend WebGL still uses: 15a's source
lists (`Vulkan.cmake` generated from `vulkan_backend.gni`), 15a's three
ANGLE patches, and 15a's SPIR-V stand-in. One ANGLE in the title, so no
symbol clash.

**The title on ANGLE** (all behind `PS5WK_GPU_RADV`; the release build
compiles as before):

- `dlshim.c`: epoxy's `dlsym` of libEGL/libGLESv2 answers with ANGLE's
  `EGL_GetProcAddress`. ANGLE's GL backend keeps reaching ps5-opengl through
  `angle_ps5_egl_get_proc_address`.
- `PS5Renderer::initializeANGLE`: ANGLE's Vulkan display in simple mode
  (`VK_KHR_display` on VideoOut), the first 8888 ES 3 window config that
  RADV's B8G8R8A8 surface takes, a 1920x1080 native window (patch 0003), an
  ES 3.1 context. No `ps5_agc_gate2_wait_present` (ANGLE's swapchain
  acquire waits instead).
- `PS5GLES.h`: the `#version 430 core` shaders become `#version 310 es`
  with default precisions (renderer, VR, overlay, GL bench), and BGRA
  uploads use `GL_BGRA_EXT` as the internal format.
- View in VR: the eye buffers are RADV scanout images on ANGLE's
  `VkDevice` (15c), wrapped with `EGL_VULKAN_IMAGE_ANGLE`, held by ANGLE for
  the whole session (`glAcquireTexturesANGLE` once), with T#s from
  `psvr2_build_tsharp_rgba8`. The frame's existing `glFinish` comes before
  each submit.
- The WC compositor's TextureMapper adopts the renderer's context as
  before; on an ES context it uses its ES shaders.

**Console results (2026-11-19, 13.60, PPSA99208 "Parallax RADV test",
build `20261119-033406-242db6ea24`):** the title starts on ANGLE
(`ANGLE (AMD, Vulkan 1.4.354 (PlayStation 5 GPU (RADV NAVI21)), radv-26.2.0);
OpenGL ES 3.1`; display up in 7-25 ms, surface and context in ~125 ms), the
WC compositor comes up on it (8 TextureMapper programs in 24 ms, GLSL 300
es, first paint 59 ms), and every Phase 0 case runs. Logs:
[results/radv-20261119-033838/](results/radv-20261119-033838/), against
the release build's [results/phase0-20261119-013048/](results/phase0-20261119-013048/):

| Case | Frames/s best (median), release → radv | Scene paint ms median (max) | Tile upload ms each | Video convert ms |
| --- | --- | --- | --- | --- |
| `static.html`, pad scroll | 59.9 (42.5) → 59.9 (42.0) | 0.25 (14.5) → 0.17 (11.1) | 0.136 → 0.036 | |
| `wcfeed.html` | 60.0 (59.9) → 59.9 (59.9) | 1.34 (3.07) → 0.81 (2.19) | 0.129 → 0.041 | 0.21 → 0.05 |
| 2800 layers, pad scroll | 59.6 (51.4) → 59.9 (59.9) | 10.0 (13.6) → 4.5 (9.0) | 0.066 → 0.025 | |
| 1080p H.264 | 60.0 (59.9) → 59.9 (59.9) | | | 0.29 → 0.03 |
| 2160p HEVC | 59.9 (59.9) → 59.9 (59.9) | | | 0.80 → 0.03 |
| 2160p HEVC 10-bit | 60.0 (59.9) → 59.9 (59.9) | | | 2.26 → 0.03 |
| VR (`vr-sim` + self-test) | 56.5 → 60.0 headset frames/s, render 1.70 → 0.93-1.01 ms | | | |

- **WebGL** (Phase 3, done here too): `readpixels.html` 29/29 (29/29).
  `busy.html` at 100 / 250 / 500 / 1000 / 2000 objects: 59.9-60.3
  frames/s throughout, JS 0.24 / 0.57 / 0.66 / 1.13 / 2.12 ms per frame
  (release 0.28 / 0.47 / 0.87 / 1.50 / 2.36). `webgl/video.html` 59.4-59.9
  frames/s (60.0), centre pixel 0,5,108,255 as on the release build.
- **WebGPU:** all 14 pages pass, as on the release build;
  `video-texture` 5/5 at 59.9 frames/s, `webgl-and-webgpu` 5/5.
- **View in VR:** self-test PASS on RADV eye buffers (15 captures, 8
  projections, controls, 4 entries in 71 s), eye buffers at a 64 KiB
  aligned RADV address, `sw_mode 27`.
- **Video convert** is the renderer's `copy+convert` timer, the same code
  in both builds: the render thread's time to copy a frame into its GL
  buffer (frames that aren't already there) and to issue the YUV-to-RGBA
  draw, not GPU time (15d measured that on ANGLE: 0.17-0.42 ms). Issuing
  the draw cost 0.3-2.3 ms of CPU on ps5-opengl.
- **"Render" in the `[present]` line** is ~16 ms on RADV against 1-3 ms:
  it now includes ANGLE's wait for the next swapchain image, which ANGLE
  does at the first draw into the window (the `draw` part). ps5-opengl did
  the same wait in `ps5_agc_gate2_wait_present`, outside the measured time.
  It's vsync pacing, not work: every case presents at 59.9.
- **Memory:** direct memory 452 MB on the plain pages (630 on the release
  build), 1.5 GB on the feed (2.0). After WebGPU's `buffer-map` page (3.6
  GB, as on the release build) it went back to 0.5-0.7 GB on the next
  pages; on the release build it stayed at 4.0-4.3 GB (Phase 0's open
  question).

**Fixed on the way** (all in `patches-radv/` or behind `PS5WK_GPU_RADV`):

- WebGL contexts failed with `EGL_BAD_ATTRIBUTE`: WebKit asks every ANGLE
  display for `EGL_CONTEXT_VIRTUALIZATION_GROUP_ANGLE`, which only ANGLE's
  GL backend has. It now asks only when the display lists
  `EGL_ANGLE_context_virtualization` (`GraphicsContextGLEGL.cpp`; the
  PS5 build also logs which set-up step fails, with the EGL error).
- WebGL video textures crashed the title in ANGLE's Vulkan command
  recording (null allocator in `SecondaryCommandBuffer::setVertexInput`):
  WebKit builds ANGLE without its share-context lock, and the title uses
  one share group from several threads. `ANGLE_ENABLE_SHARE_CONTEXT_LOCK=1`
  (ANGLE's own default outside Chromium) for the radv build fixed it. The
  WebGL video copy also runs in WebGL's own context now (no switch to a
  helper context), keeping WebGL's framebuffer bindings and scissor test;
  that change alone didn't fix the crash.
- WebGPU's video read made its context current on
  `eglGetDisplay(EGL_DEFAULT_DISPLAY)`, which on radv is a different ANGLE
  display: it now uses the shared display's own handle (the same handle as
  before on the release build).
- The ANGLE and VR code reach RADV through weak symbols, so the WebProcess
  payload (no RADV) still links.

**Owner TV check (2026-11-19 morning)** found two more, both fixed:

- The system's splash screen stayed over the browser: ps5-opengl hides it
  when its window surface is made (`ps5_egl.c`), and on radv nothing did.
  `PS5Renderer::initializeANGLE` now calls
  `sceSystemServiceHideSplashScreen` after ANGLE's window surface.
- A WebGPU cube turned in jumps, about once a second: the title's GPU
  submission lock was held through ANGLE's swapchain acquire (inside the
  frame's first draw), so Dawn's canvas readbacks waited about 976 ms each
  (`webgpu-latency-log`). RADV's PS5 winsys already puts every submission
  of the process on one queue under its own lock, and there is no second
  driver on radv, so the lock is a no-op there (`titleradv.c`): readbacks
  60/s, 0.2 ms each; the owner saw the cube turn smoothly.
- **Rerun on this build (2026-11-20, the 07:55 package with both fixes):**
  [results/radv-20261120-013718/](results/radv-20261120-013718/), all
  pass, no crash or device loss. WebGL: `readpixels` 29/29, `busy`
  59.7-60.2 frames/s at 100 to 2000 objects (JS 0.31 / 0.38 / 0.64 / 1.10
  / 2.07 ms), `webgl/video` 59.9-60.0 frames/s, GL error 0. WebGPU: all 14
  pages pass with the same counts; triangle 63.2, two-canvases 59.9,
  cube, resize and `todataurl` 59.9, `video-texture` 60.3 (59.9),
  `webgl-and-webgpu` 59.9 frames/s. Direct memory 452-707 MB, 3.6 GB at
  `buffer-map`, back to ~0.5 GB on the next page. (The summary's present
  rate for short pages, e.g. triangle's 6.8/s, is a 5 s stats window
  that ends after the page did; the pages' own counters are the ones
  quoted.)
- **Owner headset check (2026-11-20), View in VR on PPSA99208:** the 1080p
  clip, flat (guessed right), "looks fine".
  [results/radv-vr-headset-20261120.log](results/radv-vr-headset-20261120.log):
  headset up in 711 ms on RADV eye buffers (`sw_mode 27`), 60.0-60.2
  headset frames/s once up, render 0.58-0.60 ms avg (release 0.86-0.99),
  29.4-29.9 video frames/s shown, 0 begin or submit errors, all frames
  tracked; recentre at entry, two right-stick seeks, back to the TV with
  O (released in 1204 ms, the TV at 59.9 frames/s after). An 18.8 s
  session: the projections (△), play/pause (X) and recentre (□) weren't
  pressed in it (`vr-selftest` covers those on this build).
- Pages, scrolling, both videos, WebGL and WebGL video all ran at 59.9
  frames/s on the TV run, except the 2800-layer page scrolling itself by
  script (`&auto=6`): about 52 frames/s, held back by the WebProcess (18 ms
  per update). Not yet compared with the release build the same way.

**Left for later:**

- **Regression: WebGPU video textures** (owner on the TV, 2026-11-20: the
  picture and clock are right, "the framerate isn't smooth";
  [results/radv-webgpu-video-texture-tv-20261120.log](results/radv-webgpu-video-texture-tv-20261120.log)).
  `video-texture` passes 5/5 on every radv run, but its own counters:
  `importExternalTexture` 47.0-50.3 frames/s with 3.4-4.8 ms of JS a
  frame (release 60.0, 0.30 ms); `copyExternalImageToTexture` 2.0-3.3 s
  (release 36 ms). The title's read of each video frame for Dawn
  (`PS5WebGPUVideoRead.cpp`, compute pass + `glMapBufferRange`, under
  `readerLock`) takes ~32 ms on ANGLE/RADV against 1.2 ms on ps5-opengl,
  so the canvas gets at most ~30 fresh frames/s. **Timed** (2026-11-20,
  a build with per-step timers, 120-read averages,
  [results/radv-webgpu-videoread-timing-20261120.log](results/radv-webgpu-videoread-timing-20261120.log)):
  `eglMakeCurrent` 4-14 ms (waiting on ANGLE's share-context lock, which
  the compositor's thread holds while ANGLE waits for the next swapchain
  image; the lock is on in the radv build since the WebGL video crash
  fix) and the `memcpy` out of the mapped 8 MB buffer 15.1-15.4 ms
  (~530 MB/s: the mapping is uncached; the RADV memory type seen in 15c
  is flags 0x7, no `HOST_CACHED`). The copy, dispatch, flush, map and
  release are ~0.3 ms together. Not a GPU wait, so double-buffering the
  readback wouldn't help. Phase 4 (Dawn takes the converted frame as a
  `VkImage`, 15e) removes the read and both costs; the same two costs
  apply to any other CPU readback on radv (Dawn's `MapAsync`,
  `glReadPixels`), so check those in Phase 4 too. The summary's 60 present/s for this page hid it: read
  the pages' own RESULT lines. **Fixed in Phase 4** (below): 60.0
  frames/s, 0.30 ms of JS; the copy 27 ms.
- **Owner TV pages (2026-11-19):** the six pages looked fine (owner,
  2026-11-20).
- **Long session (2026-11-20):** `wcfeed.html?mins=30` on PPSA99208 (49
  videos playing):
  [results/radv-longfeed-20261120-022551.log](results/radv-longfeed-20261120-022551.log).
  All 325 five-second windows at 59.9 frames/s or more, rAF 59.9-60.0;
  direct memory 600-1030 MB with the scroll position and no growth (893
  MB at the end), flexible free 186-234 MB, WebProcess RSS 258-268 MB; no
  crash, abort or device loss. Black frames weren't counted (no tool for
  it outside Spike 12's two-driver test).
- **Smoke and browser features (2026-11-20, PPSA99208, no TV):** a site
  over the network, `parallax://settings` and `parallax://update` 3/3;
  Safer browsing on (JIT and WebAssembly off in the new WebProcess, WebGL
  and WebGPU off: `readpixels` gets no context) and off again (29/29);
  WebGPU off (`navigator.gpu` absent) and on (adapter 11/11); pages in
  the browser process (`web_in_title=1`: the WebProcess in the title,
  hardware video, WebGL 29/29, WebGPU adapter 11/11); the GL texture cap
  at 40 MB on the feed (coverage reduced, then the page reloaded on the
  CPU path, 30 rAF/s, as designed). Settings were put back to their
  defaults (all were at the default before).
- **Crash missed on the night, found 2026-11-20 morning:** the live-tabs
  attempt (video page, then `webgl-readpixels` pushed into the same tab)
  killed the title (pid 182, build 20261120-030636, the Phase 2/3 build):
  `crash-title.txt` has SIGSEGV at address 0x0, pc elf +0x52811. The log
  stops just after `[webgl] title: ANGLE context 581 for WebGL 2` with no
  crash line (the handler wrote the file but the log didn't get it), so
  the run script's crash count said 0 and its "not running" was taken
  for a slow `--status`. That build's ELF wasn't kept; against the Phase
  4 ELF the pc falls in `WTF::WordLock::unlock`, which may not hold for
  the older build. Not seen in any Phase 4 run (WebGPU 14 pages, WebGL
  29/29, compositor, video, VR, the TV pages below). **Replayed 5 times
  on the Phase 4 build (2026-11-20 09:23):** fresh title on the 1080p
  video page, 8 s, then `webgl-readpixels` and `webgpu/adapter` pushed
  into the same tab: the title alive after every round, WebGL 29/29 and
  adapter 11/11 each time, `crash-title.txt` unchanged. Not reproduced.
  Log: `results/radv-vidgl-repro-20261120.log`. **The crash's place**:
  package.sh had kept that build's ELF
  (`titles-radv-wip/debug/20261120-030636-e6f88ff0af/`): elf +0x52811 is
  `__wrap_pthread_mutex_lock` (`apps/common/ps5locks.c:302`, reading the
  mutex's word), so something locked a null `pthread_mutex_t*`. Who did
  isn't known: the report had no stack line (below).
- **Crash reports fixed (2026-11-20).** Why that crash left only one line:
  (1) the crash guard wrote with `write(2, ...)`, and in the title fd 2
  isn't the log (titlelog points libc's `stderr` stream at its pipe; a
  title can't dup2 over fd 2), so nothing reached the log; (2) the stack
  scan faulted and killed the process: the call-instruction check read
  code bytes, and a title's code is execute-only. Now
  (`apps/common/crashguard.c`, `apps/title/titlelog.c`): the report is
  also handed to the log through `ps5wk_crash_log_report` (trylock for at
  most 0.5 s, then a flush of at most 1 s), the first line before the
  scan; the scan checks each page before reading it (a `write()` from it
  into a pipe, EFAULT instead of a fault) and stops at the end of the
  mapped stack; where code can't be read it lists every stack word in
  `.text` (`stack (code not readable: every .text value)`), unfiltered; a
  fault in the report prints what it had. Test knob:
  `/data/ps5webkit/crash-title-test` makes the title fault 10 s after
  start, three calls deep (delete it after). Checked: the report in the
  log and in `crash-title.txt`, the stack line symbolizing to
  `crashTestDepth` x3, `crashTestThread`, `stack_trampoline`
  (`results/radv-crashreport3-20261120-102453/`). These files are shared
  with the release build: it gets the same when next built.
- **Left for the owner:** *Restart browser* (it restarts the running
  title since 2026-11-20: it used to pass PPSA99207 to the restart helper
  whatever was running, and the helper refused PPSA99208, "PPSA99207 is not
  pid N"; checked through the API: the helper restarted PPSA99208 in about
  1.5 s), the update path from 0.2.2 (it means installing the radv build as
  PPSA99207: updates still name PPSA99207 only), and live tabs (a test run has
  one tab, so they need a normal run, with the owner's session).
- **Follow-ups (2026-11-20)**
  ([results/radv-followups-20261120-135314/](results/radv-followups-20261120-135314/)):
  - **Overlay GPU time:** the render thread times each frame's GL work,
    from its start to the swap, with `EXT_disjoint_timer_query` (ANGLE's,
    from RADV's 100 MHz timestamps), four queries in a ring, read a few
    frames late, none while the overlay is off. The overlay's `GPU` shows
    avg/max (`shots/02--10s.png`: `GPU 0.4/0.7` on a WebGL 1 video page).
  - **One frame per task for WebGPU video copies** (0144): two
    `copyExternalImageToTexture(video)` calls in one task could copy
    different frames (the title took the stream's newest for each). The
    WebProcess gives each copy the task's token (until the end of its
    microtask checkpoint); RemoteQueue keeps the shared image of the first
    copy and makes the task's other copies of that stream from it, Dawn
    reporting the layout it leaves the image in for the next. New case
    `video-scaled.html` `same-frame`: 68 tasks of three copies, each of one
    frame.
  - **The crash report's code bound:** `_etext` came out 0 (the title's own
    linker script didn't define it, and `package.sh` nulled the weak
    reference), so the scan's bound was `__init_array_start`, past
    `.rodata`. `ps5-pie-webkit.ld` defines it now, at the end of `.text`.
    The deliberate fault's report is as before (its stack had nothing
    between the two bounds) and symbolizes to `crashTestDepth` x3,
    `crashTestThread`, `stack_trampoline`.
  - **WebGL 1 video in the runner:** `webgl/video.html?gl=1`, case
    `webgl1-video` (59.9 frames/s), next to `webgl-video`.
- sRGB WebGL textures fed from video got the frame encoded twice on radv
  (a blit into sRGB encodes, and ANGLE has no `EXT_sRGB_write_control`
  here). Fixed 2026-11-20 with the WebGL 1 fix below: every WebGL context
  now copies with `CHROMIUM_copy_texture`.
- Phase 4: done (below); the owner's TV check passed (2026-11-20).
- Phase 5: done (below): the radv build links no ps5-opengl. The 0.3.0
  release is still to do.
- Long runs, memory over hours, GPU faults: not tested.
- **WebGL 1 video textures aborted the title (fixed 2026-11-20).** The
  owner's test on a VR video site: the title aborted when the video
  started, before View in VR. The page draws its player with WebGL 1. On
  radv the video copy runs in the page's own context (0140), and ANGLE
  makes a WebGL 1 context OpenGL ES 2.0, which has no fences and no
  `glBlitFramebuffer`. epoxy aborted at the copy's `glWaitSync` ("No
  provider of glWaitSync found"). Every earlier video check was WebGL 2.
  Now, in an ES 2.0 context, `PS5Renderer::copyVideoFrame` copies with
  `glCopySubTextureCHROMIUM` (it can flip; `ANGLE_framebuffer_blit`
  can't), with no fences: the frame's write, the copy and the next write
  are flushed in turn under one lock, onto ANGLE's one queue. It returns
  `(void*)1` for that, and 0143 has WebKit delete no fence then. Checked
  ([results/radv-webgl1video-20261120-122646/](results/radv-webgl1video-20261120-122646/)):
  `webgl/video-formats.html?gl=1` passes every case in both rounds (RGBA,
  RGB, `SRGB_ALPHA_EXT`, sub-image uploads, flipY off), WebGL 2's run is as
  before, and `readpixels` is 29/29. The WebGL 2 sRGB cases still failed
  (the double encoding above).
  Then every WebGL context, 1 or 2 (`ANGLE_webgl_compatibility`), took the
  same copy, which stores the frame's bytes as they are in any format;
  WebGPU's reader context still blits, into RGBA8 and BGRA8 only
  ([results/radv-webglsrgb-20261120-130657/](results/radv-webglsrgb-20261120-130657/)):
  `video-formats.html` passes every case in WebGL 1 and 2, sRGB included
  (`SRGB8_ALPHA8`, max difference 0, was 73), `readpixels` 29/29, the
  runner's WebGL cases at 59.9 frames/s, and every WebGPU page passes
  (`video-texture` at 60.0 frames/s, its first second's 49 dropped video
  frames as in Phase 4 and 5).

## Phase 4: WebGPU without the CPU round trips (2026-11-20)

On PPSA99208, the same runner as Phase 0
([results/radv-phase4-20261120-a/](results/radv-phase4-20261120-a/): WebGPU
and WebGL; [results/radv-phase4-20261120-b/](results/radv-phase4-20261120-b/):
static, feed, 2800 layers, the three videos, VR simulation). WebKit's side is
`patches-radv/0141-ps5-radv-webgpu-shared-images.patch`; Dawn's is
`port/deps/patches/dawn-radv/`.

**What changed:**

- **Shared images** (`PS5SharedImages.cpp`). The title makes RGBA8/BGRA8
  `VkImage`s on ANGLE's `VkDevice`, with
  `VkExternalMemoryImageCreateInfo { HOST_ALLOCATION }`, in host-visible
  memory. Each is wrapped for GL with `EGL_VULKAN_IMAGE_ANGLE`. Dawn
  aliases one with an image made from the same create info, over a
  host-pointer import of the same memory (15e). Each side hands the image
  over after a CPU wait for its own work: `glFinish` or a GL fence on the
  title's side, `OnSubmittedWorkDone` on Dawn's. Dawn keeps its aliases
  per device (16, newest kept), by an image id that is never reused.
- **Video into WebGPU (0123/0124).** For `importExternalTexture(video)`, GL
  copies the stream's converted frame into a free shared image on
  RemoteGPU's work queue (the same blit as WebGL's video copy). Dawn's
  alias becomes the external texture's plane. The image goes back once the
  texture has expired and Dawn's work with it is done. A process has at
  most 8 such images (192 MB); past that, or when Dawn can't alias one,
  the old readback path runs. `copyExternalImageToTexture(video)` doesn't
  bring pixels to the WebProcess any more. It sends
  `CopyVideoStreamFrameToTexturePS5` on the stream, and the title runs
  Dawn's `CopyTextureForBrowser` from the shared image into the
  destination, flipped when asked. That covers rgba8/bgra8 (and their
  sRGB forms), rgb10a2 and rgba16float, with no source origin; the rest
  go the pixel way.
- **Canvas present (0110/0132).** A presented frame of an 8-bit format is
  copied on the GPU (`CopyTextureToTexture`) into one of the canvas's
  shared images (at most 4). The WC scene draws that image's GL texture.
  GL takes the image when the frame is latched, and lets it go with a
  fence when the frame is replaced; the image is free for Dawn again
  once the fence has passed. There's no `CopyTextureToBuffer` or
  `MapAsync`, and no CPU copy. rgba16float canvases still go the pixel
  way, which converts them. The canvas doesn't render straight into the
  compositor's image: the page reuses its three render buffers while the
  compositor may still show one, so a copy on the GPU (about 8 MB at
  1080p) keeps the two apart.
- **Readback** (`toDataURL`, `drawImage`, `createImageBitmap`) is
  unchanged: Dawn reads the render buffer (`readPixels`). The Web
  Inspector's read of the last shown frame (`currentFrame` false) has no
  CPU copy to use now, so it falls back to that same read. Any CPU
  readback on radv still costs the uncached mapping (~530 MB/s; the PS5
  winsys has no `HOST_CACHED` type).
- **The compositor takes its swapchain image before its frame, outside
  ANGLE's locks** (`PS5Renderer.cpp`: `eglPrepareSwapBuffersANGLE`,
  which acquires in an unlocked tail call, where ps5-opengl's flip wait
  was). Before, the acquire ran in the frame's first draw, holding the
  share group's lock for most of a frame. That was the 4-14 ms
  `eglMakeCurrent` wait in Dawn's video reads. WebGL waited for it too:
  `webgl-and-webgpu` went from 5.56 to 2.93 ms of JS a frame (release
  3.09). The `[present]` line's render time is now the frame's work
  (0.4-0.5 ms on the plain pages, 16 ms before), as it is on the release
  build.
- **The GPU submit lock is out of the radv build.** Its Dawn leaves 0004
  out (`dawn-radv/0002` reverts it), and `PS5Renderer.cpp`'s
  `GPUSubmitLock`, WebGL's batch lock and flush (0122,
  `RemoteGraphicsContextGLPS5.cpp`) and the video read's lock are
  compiled out. `titleradv.c` keeps its two empty functions, because
  `ps5browser.c` turns WebGPU on when they exist.

**How Dawn's change stays off the release build.** `port/deps/build.sh
dawn-radv` unpacks Dawn again into `src/dawn-radv`. It applies
`patches/dawn` and then `patches/dawn-radv` (0001: a new
`dawn/native/PS5HostPointerTexture.h` and
`vulkan/PS5HostPointerTextureVk.cpp`, new files only; 0002: no lock), and
installs into `toolchain/wpe-deps/install/dawn-radv`. Only GPU=radv
`port/webkit/build.sh` links that prefix (`-DPS5WK_DAWN_ROOT`, used by
WebGPUDawn's CMakeLists on the `ps5-radv` branch). `step_dawn`, the
sysroot's `libwebgpu_dawn.a` (13 Nov), `build-webgpu` and the PPSA99207
packages weren't touched.

**Console, before (radv, [radv-20261120-013718](results/radv-20261120-013718/))
and after, against the release build ([phase0-20261119-013048](results/phase0-20261119-013048/));
the pages' own RESULT lines:**

| Page | Release | radv before | radv after |
| --- | --- | --- | --- |
| `video-texture`: `importExternalTexture` every frame | 60.0 fps, 0.30 ms JS | 50.3 fps, 3.44 ms (47.0, 4.80 on 11-19) | **60.0 fps, 0.30 ms** |
| `video-texture`: `copyExternalImageToTexture` + readback, 1080p | 36.0 ms | 1967 ms (3300) | **27.0 ms** |
| `video-texture`: 10 `requestVideoFrameCallback`s | 1877 ms, media time 1.967 | 330 ms, 4.000 | 1888 ms, 1.967 |
| triangle, 30 frames | 60.4 fps, 0.13 ms | 63.2, 0.17 | 60.7, 0.20 |
| cube, 120 frames | 60.0, 0.28 | 59.9, 0.28 | 60.0, 0.24 |
| resize, 320x180 to 1920x1080 | 60.1-62.5 (1920x1080: 60.9) | 59.9-62.6 (61.0) | 60.0-61.9 (61.1) |
| two-canvases, three canvases | 59.9, 0.27 | 59.9, 0.28 | 59.9, 0.27 |
| `webgl-and-webgpu`, 900 frames | 60.0, 3.09 ms | 59.9, 5.56 | 59.8, 2.93 |
| `todataurl` | 11/11 | 11/11 | 11/11 |

- **All 14 WebGPU pages pass** with the release build's counts: adapter
  11/11, device 29/29, compute 6/6, offscreen-readback 7/7, errors 11/11,
  buffer-map 11/11, triangle 3/3, cube 3/3, resize 10/10, two-canvases
  7/7, todataurl 11/11, copy-external-image 16/16, video-texture 5/5,
  webgl-and-webgpu 5/5. There was no uncaptured error, device loss or
  crash. The title logs "first frame ... in an image shared with the
  compositor" for every 8-bit canvas, and "video frame 1920x1080 copied
  for a WebGPU texture, no CPU copy" (1.97 ms for the first copy,
  `glFinish` included).
- **`video-frame-callback` and the video's own counters now match the
  release build exactly:** 19.0 shown/s while the page starts and 49
  dropped at the clip's start, as on release; the clip time during the
  animation is 0.04 to 0.09 s (release 0.04 to 0.08). The radv
  "before" numbers there (330 ms) were an artefact of its slow imports,
  not a better result.
- **WebGL:** `readpixels` 29/29. `busy` 59.7-60.4 frames/s at 100 to
  2000 objects (JS 0.34 / 0.56 / 1.06 / 1.80 / 3.17 ms; one frame of 45
  ms at 250). `webgl/video` 59.9-60.0 frames/s, centre pixel 0,5,108,255,
  GL error 0. The `busy` JS times are higher than before (0.31-2.07 ms) in
  this one run; not looked into. The frame rate held.
- **Compositor, video and VR on the same build** (the swapchain change
  touches every frame): static 59.9 (42.9), feed 60.0 (59.9), 2800 layers
  60.0 (59.9) with scene paint 7.54 (9.36) ms, the three videos 59.9
  (59.9) with convert 0.03-0.04 ms, `vr-sim` + `vr-selftest` PASS (15
  captures, 8 projections, 4 entries in 71 s). Render time 0.41-0.45 ms
  except the feed (1.61) and the 2800 layers (9.46).
- **Memory:** direct memory 452-707 MB on the WebGPU pages, 3.6 GB at
  `buffer-map` and back after, as before; 452-1232 MB on the compositor
  cases. A 1080p shared image is 8.3 MB (whole 64 KiB granules).

**Not done or not checked:**

- ~~The TV~~: **owner check, 2026-11-20, all good** on PPSA99208:
  `video-texture` smooth with the right colours (5/5, 60.0 fps, 0.30 ms
  JS, copy 25 ms), `cube` smooth (3/3, 60.1 fps), `two-canvases` each
  canvas its own picture, detach and reattach fine (7/7), `resize` (10/10;
  the soft, stepped triangle the owner saw was case 5, 16x16 shown at
  640x360, as expected). No crash. Log:
  `results/radv-phase4-tv-20261120.log`.
- Rendering straight into the compositor's image (no GPU copy) needs the
  canvas's render buffers to come from the pool. Not done; the copy costs
  the GPU about 8 MB of bandwidth a frame at 1080p.
- ~~A video in WebGPU whose decoded size isn't the copy size~~ **fixed
  2026-11-20:** the page sends the element's size (videoWidth x
  videoHeight) with the copy, and the title scales the frame to it on the
  GPU (into the frame-size read texture, then a linear blit into the
  shared image) where the decoded size differs, as the pixel way scaled
  it on the CPU. New page `webgpu/video-scaled.html` (5/5 on PPSA99208):
  a 1440x1080 clip with 4:3 pixels (`media/anamorphic-1440x1080-sar4x3-h264.mp4`,
  1920x1080 on screen) copied and imported with its red-to-blue edge at
  x 960 of 1920, the scaled copy 5.5-7 ms; `switch-h264.mp4` (in-band
  size changes, the element 396x704 throughout) copied 79 times, every
  copy covering the texture. `video-texture` 5/5, `copy-external-image`
  16/16, `cube`, WebGL 29/29 after it. Logs: `results/radv-sizefix*-20261120-*`.
- `switch-h264.mp4` decoded with 80 errors, 220 AUs skipped before a
  keyframe and 10 resets (300 of 600 pictures) on **both** builds: the
  release browser (PPSA99207, build 20261119-094042) and radv the same,
  so not radv's. **Fixed 2026-11-20** (`wpe-ps5/PS5VideoDecoder.cpp`,
  shared with the release build): the 2026-11-09 small H.264 decoders
  (its own 400x704) met the in-band switch to 544x960; such an SPS now
  reopens the decoder in place. 600/600 pictures, 0 errors; see
  port/webkit/README.md ("Broken again by the small H.264 decoders").
  `video-scaled` 5/5 after it (its switch case now really decodes at
  544x960 and 1280x720 into a 396x704 element, so the scaled copy runs).
  Logs: `results/radv-vdecgrow-20261120-103823/`,
  `results/radv-videoscaled-20261120-104518/`.
- The `busy` JS rise above, and the GPU cost of the per-frame copies, have
  not been looked into.

## Phase 5: no ps5-opengl (2026-11-20)

The `GPU=radv` title links no ps5-opengl. Build `20261120-112401-938bd6e031`
on PPSA99208, with the Phase 0 runner and the browser features
([results/radv-phase5-20261120-112438/](results/radv-phase5-20261120-112438/)).

**What changed:**

- **ANGLE** is built with its Vulkan backend only
  (`patches-radv/0142-ps5-radv-angle-vulkan-only.patch`, `PlatformWPE.cmake`).
  Its GL backend would have loaded a native GL through `libEGL`, which
  `dlshim.c` now answers with ANGLE itself. `PS5ExternalTexture.cpp` builds
  without `TextureGL` (on Vulkan it already returned the texture's name).
- **`package.sh`** reads `PS5WK_GPU_RADV` from the build's cache before
  anything else. Then it skips `libPS5OpenGL.a`, `-u ps5_agc_gate2_run`,
  `egl_symbols.inc` and ps5-opengl's `runtime_shims.c`, and takes the SDK's
  `libSceAgc` and `libSceAgcDriver` stubs. The `GPU=gl` path is as before
  (checked: the release's `build-webgpu` still packages).
- **What ps5-opengl was quietly providing:**
  - `open_memstream`, for RADV's `u_memstream` (hang reports, debug
    dumps). The SDK's libc has none, nor funopen or fmemopen. ps5-opengl's
    version fails with ENOSYS, and so does the one in `titleradv.c` now:
    RADV never had a memory stream in the release either.
  - A strong import of `sceSystemServiceHideSplashScreen`. With only
    `PS5Renderer.cpp`'s weak reference, `libSceSystemService` wasn't a
    needed library and the converter found no stub for it. It is in
    `dlshim.c`'s list of strong imports now.
  - stdout and stderr into a file before the title log starts:
    `titlert.c` (`/download0/ps5browser.log`). libc.a has the `__assert`,
    `openlog`, `popen` and `pclose` that `runtime_shims.c` also defined.
- **`build-radv.sh`** makes its own meson venv (`build/radv-ps5/venv`)
  instead of using Spike 2's. A `--clean` build with it gave an archive
  byte-identical to the one before.
- **`radv-object.sh`** is kept as it is: the `GPU=gl` WebGPU build still
  needs it, and with only the ICD's entry points global, RADV's own zlib
  (its meson fallback: `inflate`, `deflate`, `crc32`, `adler32`) can't
  meet the title's.

**Size:** `ps5browser.elf` text 190.4 MB → 169.1 MB, bss 26.2 → 23.5 MB.
The eboot is 167 MiB (the release's, with ps5-opengl and RADV both in it, is 185 MiB).
The smaller image shows as free flexible memory: the lowest free flexible
memory is 23 MB higher in every runner case (static 239 → 262 MB, video
215 → 238 MB).

**Results:** the same as Phase 4.

| case | Phase 4 | Phase 5 |
| --- | --- | --- |
| static, present/s best (median) | 59.9 (42.9) | 59.9 (43.0) |
| feed, render ms | 1.61 | 1.65 |
| layers2800, render / scene paint ms | 9.46 / 7.54 | 9.75 / 7.38 |
| video 1080p / 2160p / 2160p10, present/s | 59.9 / 59.9 / 59.9 | 59.9 / 59.9 / 59.9 |
| webgl-busy, webgl-video | 59.9, 59.9 | 59.9, 59.9 |
| webgpu-cube, webgpu-video-texture | 59.9, 60.2 | 59.9, 60.3 |
| vr-sim | 59.9 (47.4) | 59.9 (47.4) |

- WebGL `readpixels` 29/29. Every WebGPU page passes (adapter 11/11,
  device 29/29, compute 6/6, offscreen-readback 7/7, errors 11/11,
  buffer-map 11/11, triangle 3/3, cube 3/3, resize 10/10, two-canvases 7/7,
  todataurl 11/11, copy-external-image 16/16, video-texture 5/5,
  webgl-and-webgpu 5/5).
- The VR self-test passes (15 eye-buffer captures, 8 projections).
- The feed's peak direct memory was 1612 MB (Phase 4: 1232). Across
  earlier runs it has been between 1232 and 1990 MB; it depends on when the
  15 decoders peak together.
- Features: the smoke run, Safer browsing on and off, WebGPU off and on,
  pages in the browser process, the GL texture cap's fallback to the CPU,
  three live tabs, and the video-WebGL-WebGPU sequence 5 times. No crash,
  abort or device lost; `crash-title.txt` unchanged.

**Release 0.3.0 (2026-11-20).** `make-release.sh` packages `build-radv`,
and `contentVersion` is 01.003.000. The update from a 0.2.2 build to the
0.3.0 zip passed on the console (`port/webkit/UPDATES.md`, "0.2.2 to 0.3.0"):
the update installed, 0.3.0 started, and the settings, update and plain
pages rendered ([results/release-030-update-20261120-162711/](results/release-030-update-20261120-162711/)).

**8K HEVC and HLS before the release (2026-11-20).** The runner's new
`media` cases ran on the 0.3.0 code (PPSA99208, the dev build of `fc80cec`)
and on the 0.2.3 release (PPSA99207), one after the other:
[results/release-030-media-20261120-165335/](results/release-030-media-20261120-165335/).
The HLS streams came from `tools/serve-media.py` on the Mac.

| case | frames shown (dropped), 0.3.0 / 0.2.3 | decoder, both builds | render ms, 0.3.0 / 0.2.3 | copy+convert ms (CPU) | direct MB max | flexible MB min |
| --- | --- | --- | --- | --- | --- | --- |
| `video-8k` (7680x3840 HEVC, 30 s) | 893 (0) / 893 (0) | 900/900 pictures, 0 errors | 0.76 / 3.10 | 0.05 / 3.02 | 1950 / 1926 | 121 / 101 |
| `hls-ts` (H.264, 640x360 up to 1280x720) | 1177 (6) / 1179 (5) | 1200/1200, 0 errors | 0.34 / 0.97 | 0.03 / 0.28 | 534 / 714 | 246 / 226 |
| `hls-fmp4` (HEVC, 960x540 up to 1920x1080) | 1178 (5) / 1176 (6) | 1200/1200, 0 errors | 0.35 / 1.01 | 0.03 / 0.27 | 579 / 745 | 239 / 219 |
| `hls-vr` (H.264 2160x1080 up to 3840x1920 60 fps) | 2748 (4) / 2701 (2) | 2760/2760, 0 errors | 0.53 / 1.79 | 0.04 / 0.81 | 878 / 920 | 199 / 179 |
| `hls-vr` `mixed.m3u8` (switching every few segments) | 2426 (6) / 2424 (3) | 2514/2514, 0 errors, 5 format changes, 0 resets | 0.51 / 1.76 | 0.04 / 0.81 | 878 / 920 | 199 / 179 |
| `vr-sim-8k` (View in VR on the 8K clip) | self-test PASS on both (15 captures, 8 projections) | 0 errors | 0.78 / 2.10 | 0.06 / 3.03 | 2087 / 2100 | 121 / 101 |

The TV composited 59.9-60.0 frames/s in every case on both builds, with no
crash, abort or device loss. Both builds play every case the same way. The
render and convert times are CPU times: on ANGLE the work is recorded and
then runs on the GPU, so they show the CPU freed, not a faster GPU. This
covers the checklist's 8K and HLS item. The three.js example pages (they
need the network) haven't been run on RADV; `busy.html`, which draws as
three.js does, has.

## Run it

```sh
. ./env.sh
port/deps/build.sh radv                       # once; RADV and its headers
spikes/15-radv-only/build-angle-vk.sh         # -> build/angle-vk-ps5/lib/libangle_vk_ps5.a
spikes/15-radv-only/build-title-15a.sh        # -> build/titles/PPSA99021
PS5WK_OWN_TITLES="PPSA99207 PPSA99021" tools/launch.sh --install spikes/15-radv-only/build/titles/PPSA99021
curl "ftp://$PS5_HOST:2121/data/ps5webkit-spike/15a.log"
# 15c
spikes/15-radv-only/build-title-15c.sh        # -> build/titles/PPSA99022
PS5WK_OWN_TITLES="PPSA99207 PPSA99022" tools/launch.sh --install spikes/15-radv-only/build/titles/PPSA99022
curl "ftp://$PS5_HOST:2121/data/ps5webkit-spike/15c.log"
# 15d: the clips (Spike 9's) go to the spike folder, not /data/ps5webkit
spikes/15-radv-only/build-title-15d.sh        # -> build/titles/PPSA99023
for f in h264_1080p.mp4 hevc_4k.mp4 vp9p2_4k.ivf; do
    curl -sS --ftp-create-dirs -T spikes/09-hw-decode/media/$f "ftp://$PS5_HOST:2121/data/ps5webkit-spike/15d/$f"
done
PS5WK_OWN_TITLES="PPSA99207 PPSA99023" tools/launch.sh --install spikes/15-radv-only/build/titles/PPSA99023
curl "ftp://$PS5_HOST:2121/data/ps5webkit-spike/15d.log"
# 15e
spikes/15-radv-only/build-title-15e.sh        # -> build/titles/PPSA99024
PS5WK_OWN_TITLES="PPSA99207 PPSA99024" tools/launch.sh --install spikes/15-radv-only/build/titles/PPSA99024
curl "ftp://$PS5_HOST:2121/data/ps5webkit-spike/15e.log"
```

Phase 0 and the radv build (Phase 2/3):

```sh
spikes/15-radv-only/phase0/run.sh                       # release build (PPSA99207); PS5_PIN for the WebGPU pages
port/deps/build.sh dawn-radv                            # once (Phase 4): the radv build's own Dawn
GPU=radv NINJA_TARGETS="ps5wk-child ps5browser" port/webkit/build.sh src shim configure build
TITLE=PPSA99208 NAME="Parallax RADV test" BLD=toolchain/wpe-webkit/build-radv \
    OUT=toolchain/wpe-webkit/titles-radv port/webkit/apps/title/package.sh
# a test URL first, so the install's launch is a test run (no session restore)
PS5WK_OWN_TITLES="PPSA99207 PPSA99208" tools/launch.sh --install toolchain/wpe-webkit/titles-radv/PPSA99208
TITLE=PPSA99208 spikes/15-radv-only/phase0/run.sh       # the same cases on it
```

For a crash, connect to klogsrv (`nc $PS5_HOST 3232`, after sending
`ref/klogsrv/klogsrv-ps5.elf` to the ELF loader) before launching.
