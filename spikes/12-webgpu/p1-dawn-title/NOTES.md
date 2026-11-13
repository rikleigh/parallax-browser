# Spike 12, P1: Dawn in a title

_2026-11-13. Console run on 13.60, title PPSA99012._

**Pass.** Dawn's Vulkan backend runs on RADV in the same title as ps5-opengl
and offers **core** WebGPU. A WGSL triangle and a compute shader give the
right results through `MapAsync`, and frames reach the compositor's kind of GL
texture at 60 frames/s for 1.3 ms of CPU each.

## Results ([results/console-2026-11-13.log](results/console-2026-11-13.log))

| Check | Result |
| --- | --- |
| Adapter | "PlayStation 5 GPU (RADV NAVI21)", core feature level, 35 features, among them `shader-f16`, `texture-compression-bc`, `timestamp-query`, `float32-filterable`. `maxTextureDimension2D` 16384, `maxBufferSize` 4 GB |
| Device | under 1 ms |
| WGSL triangle, 1920x1080, `CopyTextureToBuffer` + `MapAsync` | exact colours. Shader module 0.2 ms, pipeline 3.4 ms cold, first frame and readback 1.1 ms |
| WGSL compute, 4096 values | all right. Module and pipeline 1.0 ms, dispatch + copy + map 0.32 ms |
| A larger shader (8 lights, 5-octave noise, texture; vertex + fragment) | module 0.9 ms, pipeline 12.0 ms cold (Tint to SPIR-V, then RADV's compiler; RADV's disk cache off) |
| 600 frames to the GL texture | first 8 checked by GL readback, exact. Per frame: Dawn draw + copy + `MapAsync` 0.23 ms, `memcpy` of 7.9 MB into the GL buffer 0.57 ms, GL SSBO pass 0.54 ms: **1.34 ms of CPU**. 59.9 frames/s (GL's vblank), 0 device errors, 0 GL errors |
| Stack | high-water 248 KiB on Dawn's thread, shader compiles included |
| Memory | flexible free 266 MB against 272 MB in P0b's title: Dawn costs about 6 MB of title image. Direct memory +57 MB for everything the test made |

"Shown" here means read back from the GL texture the compositor would draw;
the picture is also on the TV (a blue frame with a triangle that changes
colour, inset in a pulsing green clear) but nobody has looked at it yet.

## How frames get to GL

Dawn has no host-pointer import, so this is P0c's fallback form: Dawn copies
the texture to a `MapRead` buffer, the title `memcpy`s the mapping into the GL
persistent SSBO, and GL runs the video path's SSBO-to-texture pass. The copy
is 0.57 ms at 1080p. The zero-copy form from P0c (0.07 ms) would need Dawn's
Vulkan handles (`dawn::native::vulkan`), and is not worth it at this cost.

## What the title needed for Dawn

- `dawn_ps5_vk_get_instance_proc_addr()` returning RADV's
  `vk_icdGetInstanceProcAddr`; it works with no loader.
- `--wrap=dlopen,dlsym,dlclose`: Dawn's `DynamicLib` is linked though unused,
  and libc.a's `dlclose` needs `__dlclose`, which a title cannot import. The
  browser already wraps `dlopen` and `dlsym` (`dlshim.c`); it needs `dlclose` too.
- `--defsym=__cxa_thread_atexit_impl=0` (weak in libc++abi), as the browser's
  `package.sh` already does for weak symbols.
- Everything P0b needed for RADV.

Not covered: device loss from a driver fault, out-of-memory, several devices,
canvas resize, long runs.
