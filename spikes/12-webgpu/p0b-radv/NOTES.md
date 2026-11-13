# Spike 12, P0b and P0c: RADV beside ps5-opengl, and the frame bridge

_2026-11-13. Console run on 13.60, title PPSA99012._

**Both pass.** RADV (PS5_Mesa, Vulkan 1.4) runs in the same title as
ps5-opengl while GL presents every vblank, and a Vulkan frame reaches a GL
texture with no CPU copy. Route V is open.

## Results (console, [results/console-2026-11-13.log](results/console-2026-11-13.log))

| Check | Result |
| --- | --- |
| GL 4.6 core context on the window surface, presenting (as `PS5Renderer`) | PASS, 60 frames/s before, during and after the Vulkan work; 0 GL errors, 0 failed swaps |
| `vkCreateInstance`, physical device | PASS, 1 ms. "PlayStation 5 GPU (RADV NAVI21)", Vulkan 1.4.354, 219 device extensions |
| `vkCreateDevice`, one graphics + compute queue | PASS, under 1 ms |
| Triangle into a 1920x1080 RGBA8 image, copied to a buffer, read on the CPU | PASS, exact colours. Pipeline 2.8 ms (cold), draw + copy + fence 0.09 ms |
| Compute dispatch writing 4096 values to a storage buffer | PASS, all right. Pipeline 0.3 ms, dispatch + fence 0.03 ms |
| **P0c fallback:** Vulkan staging buffer, `memcpy` into a GL persistent SSBO, the video path's SSBO-to-texture pass | PASS. memcpy of 7.9 MB 0.57 ms; GL pass 0.54 ms CPU; 120 frames at 59.9 frames/s (GL's vblank) |
| **P0c zero copy:** the GL buffer's mapping imported with `VK_EXT_external_memory_host`, `vkCmdCopyImageToBuffer` straight into it | PASS. 8 frames checked by GL readback, exact. 300 frames: Vulkan side 0.07 ms a frame (max 0.08), GL pass 0.54 ms CPU, 59.9 frames/s |
| Memory | Flexible memory free: 272.0 MB throughout (nothing RADV allocates at run time is flexible). Direct memory: 45 MB for everything above (8 MB image, 8 MB staging, 12 MB GL buffer, driver arenas). No large reservation appeared: the 256 GiB "region" in the winsys is an address plan, not a mapping |

What this settles from the plan's list of unknowns:

- **Two AGC users in one title: fine.** Both drivers call `sceAgcInit` and
  submit on their own; neither noticed the other in 7 s of interleaved
  submissions. (Not covered: hours of soak, or a GPU fault in one of them.)
- **Two static Mesa 26.2 copies in one link: solved at build time** (below).
- **RADV's direct-memory use: small and on demand.**
- **The bridge works both ways**, and the zero-copy form needs nothing new in
  either driver. The GL mapping came back 16 KiB aligned (0x2097a8000), which
  is the import alignment RADV asks for; the title keeps 4 MiB of slack and an
  offset uniform in case it does not.

The Vulkan fence is enough to order the two drivers: GL draws from the buffer
only after `vkWaitForFences`, and every checked frame was complete.

## How it is built

`port/deps/radv/build-radv.sh` (run by `port/deps/build.sh radv`) builds `libvulkan_radeon.a` (269 MB with debug info) from
`ref/PS5_Mesa` at `01d70579` (the revision `ref/PS5_Vulkan` pins), following
`ref/PS5_Vulkan/tools/build-radv.sh release` but with **our** payload SDK and
libc++ 19. The only thing taken from the PS5_Vulkan SDK fork is its
`platform/include/ps5platform/*.h` (declarations of AGC, kernel and VideoOut
functions); `libps5platform.a` is not linked. First build 3 min.

Host tools for Mesa's OpenCL kernels (`mesa_clc`, `vtn_bindgen2`) need
Homebrew `libclc` and `spirv-llvm-translator` (**installed during this work**)
with Homebrew `llvm` (23), not the `llvm@18` that `env.sh` puts in
`LLVM_CONFIG`.

`build-title.sh` then:

1. Links the archive into one relocatable object (`ld.lld -r --whole-archive`;
   the SDK's `prospero-lld` wrapper adds a link script that breaks `-r`).
2. Defines the 6383 weak entry points nothing provides (`radv_Foo` where RADV
   uses `vk_common_Foo`) as absolute 0 inside that object. The title converter
   refuses undefined symbols, weak or not.
3. Makes every symbol local except `vk_icdGetInstanceProcAddr`,
   `vk_icdNegotiateLoaderICDInterfaceVersion` and
   `vk_icdGetPhysicalDeviceProcAddr` (`llvm-objcopy --keep-global-symbols`).
   RADV and ps5-opengl share 4163 global names (NIR, util, `ac_*`, ACO, blake3);
   after this they never meet.
4. Links the title with [package-title.sh](package-title.sh), Spike 2's script
   with one change: the payload SDK's `libSceAgcDriver.so` stub (116 exports)
   instead of ps5-opengl's (5), because RADV imports
   `sceAgcDriverSetHsOffchipParam`. The SDK's stub is a superset.

What RADV needed from the title beyond the SDK's libraries: `__cpu_model`
([src/cpu_model.c](src/cpu_model.c); addrlib asks `__builtin_cpu_supports("avx2")`)
and `open_memstream`, which ps5-opengl's `ps5_mesa_shims.c` already defines.
No `--wrap`, no `--defsym`, none of PS5_Vulkan's libc substitutions.

Sizes: the RADV object is 22.6 MB text + 0.5 MB data before `--gc-sections`;
the title's eboot is 39.8 MB against 25.8 MB for Spike 11's (ANGLE + GL), so
RADV costs roughly 20 MB of title image, which is flexible memory.

## For the port

- The browser's `package.sh` needs steps 1-3 for RADV and the SDK's AGC driver
  stub; nothing else in its link line changes.
- The browser's `titlemem.c` wraps `mmap`; RADV takes its memory with
  `sceKernelAllocateDirectMemory`/`sceKernelMapDirectMemory` directly, so the
  two do not interact. Not run yet in that title.
- RADV sets `MESA_SHADER_CACHE_DIR=/app0/radv-shader-cache` unless the title
  sets it first. The spike disabled the cache; the browser should point it at
  its own data folder.
- Thread stacks: PS5_Vulkan wraps `pthread_create` to give 2 MiB direct-memory
  stacks. The spike ran RADV on the main thread and it was enough for these
  shaders; real WGSL through Dawn needs the multi-MB stack the plan gives
  `RemoteGPU`'s thread.
- Not tested: device loss, a GPU fault, memory exhaustion, more than one
  device, long runs.

## Run it

```
port/deps/build.sh radv
spikes/12-webgpu/p0b-radv/build-title.sh
PS5WK_OWN_TITLES="PPSA99207 PPSA99012" tools/launch.sh --install spikes/12-webgpu/p0b-radv/build/titles/PPSA99012
curl ftp://$PS5_HOST:2121/data/ps5webkit/12-webgpu-p0b.log
```

The TV shows the Vulkan frame (blue with a triangle that changes colour) inset
in a GL clear that pulses green, for about 10 s.
