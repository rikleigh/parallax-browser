# Spike 12 P0d: does ps5-opengl offer what Dawn's GLES backend needs?

Route G of the WebGPU evaluation is Dawn's OpenGL ES backend (WebGPU
compatibility mode) on the GL driver the browser already ships
(`toolchain/ps5-opengl-gles-noprof-m8`). This probe asks the driver for
everything that backend needs, on an ES 3.2 context created like spike 11's
"anchor" (in the share group of a GL 4.6 core context like the compositor's).

**State on 2026-11-13: host run done, console title built and packaged, not
yet run on the console.** Nothing here has touched the console.

Three levels of evidence are kept apart below:

- **host**: run on the Mac in the Rosetta harness: the real patched EGL facade,
  the real `ps5_screen.c` (caps, limits, formats) and Mesa 26.2. No GPU, no PS5
  shader compiler. `host/run.sh` checks that the driver tree it compiles is the
  one `gles-noprof-m8` was built from (same PS5 objects: yes).
- **refcheck**: the same probe over Mesa's software rasteriser (softpipe)
  behind the same facade. It tests the *probe* (shaders, expected values, call
  sequences), not the PS5 driver.
- **source**: read in Dawn at `bc34d95bda12` (2026-11-12,
  `src/dawn/native/opengl/`) or in the driver tree; not run.

## Verdict so far (preliminary)

Route G is **not blocked by anything found on the host**, and needs **no
driver patch and probably no Dawn patch to get an adapter**: one small EGL
shim in the embedder (below). What is settled and what is not:

| | |
| --- | --- |
| Settled (host) | ES 3.2 / GLSL ES 3.20; a Dawn-style context (ES 3.1 asked for, no config, no share context, surfaceless, robust access + lose-context-on-reset) is created and reports the reset strategy asked for; all 358 ES 2.0-3.2 commands resolve through `eglGetProcAddress`; all 25 limits Dawn derives meet the compatibility-mode defaults; every WebGPU render-attachment format is a complete framebuffer; BC, ETC2 and ASTC are all advertised; Tint-shaped compute and render shaders pass Mesa's front end. |
| Missing (host) | **`EGL_KHR_fence_sync` / `EGL_KHR_reusable_sync`**: Dawn refuses the adapter without one, and its queue waits with EGL sync objects only. **`egl*` entry points through `eglGetProcAddress`**: 0 of 33 (same finding as spike 11). Both are covered by the shim. |
| Found (host) | **4x multisample `GL_BGRA8_EXT`** trips an assertion in Mesa's debug build (`st_texture_create`): undefined on the console's release build. Every other 4x format is fine. |
| Limitation (source) | Dawn's GL backend offers **compatibility mode only**: a page that calls `requestAdapter()` without `featureLevel: "compatibility"` gets no adapter from it. This is the main cost of Route G and no shim changes it. |
| Unknown until the console run | Everything that reaches the PS5 shader compiler or the GPU: compute at all (`glDispatchCompute`, SSBO, image load/store, shared memory, atomics, indirect dispatch), indirect draws, `glCopyImageSubData`, MSAA resolve, float and depth/stencil targets, compressed formats when sampled, and the two timings. 66 results. |

The driver source has a full compute path (`ps5_launch_grid`,
`ps5_create_compute_state`; `caps->compute = true`) and upstream reports GL 4.6
conformance runs on hardware, so the expectation is that most of the 66 pass;
the paths with explicit reject branches in the driver (compute texture
sampling, indirect dispatch, shared memory) are the ones to look at first.

## Host results (`results/host-2026-11-13.log`)

93 results: 23 PASS, 4 FAIL, 66 SKIP host.

| Test | Result | Detail |
| --- | --- | --- |
| ctx-core46 | PASS | `4.6 (Core Profile) Mesa 26.2.0`, surfaceless |
| ctx-anchor-es32 | PASS | `OpenGL ES 3.2 Mesa 26.2.0` with the core context as share context; this is the probe's context |
| egl-display | PASS | EGL 1.4, apis `OpenGL OpenGL_ES`, client extensions NULL. Display extensions: `EGL_KHR_create_context EGL_KHR_create_context_no_error EGL_EXT_create_context_robustness EGL_KHR_no_config_context EGL_KHR_surfaceless_context EGL_PS5_texture_descriptor_export` |
| egl-dawn-required | **FAIL** | EGL >= 1.4 yes, OpenGL_ES yes, create_context_robustness yes, **fence_sync or reusable_sync NO** |
| egl-sync | **FAIL** | no sync extension, and none of `eglCreateSync[KHR]`, `eglClientWaitSync[KHR]`, `eglWaitSync[KHR]`, `eglCreateImageKHR` resolve |
| egl-getproc-egl | **FAIL** | 0 of 33 EGL 1.0-1.4 entry points through the facade's `eglGetProcAddress` (it only knows `gl*`) |
| ctx-dawn-es31 | PASS | ES 3.1 asked for, `EGL_NO_CONFIG`, no share context, surfaceless: an ES 3.2 context (GLSL ES 3.20, 144 extensions) |
| ctx-dawn-robust | PASS | `EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT` + `LOSE_CONTEXT_ON_RESET_EXT` accepted; `GL_CONTEXT_FLAGS` 0x4, strategy `LOSE_CONTEXT_ON_RESET`, `glGetGraphicsResetStatus` NO_ERROR. The EGL 1.5 attribute values (0x31B2/0x31BD) are refused, which is right for EGL 1.4: Dawn uses the EXT ones there |
| reset-status | PASS | anchor (no robustness attributes): strategy `NO_RESET_NOTIFICATION`, status NO_ERROR |
| caps-strings | PASS | renderer `PS5 AGC`, vendor `PS5 homebrew`, 144 extensions (list in the log) |
| caps-limits-texture | PASS | 2D 16384, 3D 2048, layers 2048, cube 16384, texture buffer 1048576 texels, colour attachments 8, draw buffers 8, samples 4 (colour, depth, integer), anisotropy 16 |
| caps-limits-buffers | PASS | UBO bindings 108, UBO size 65536, UBO alignment 16, 14 uniform blocks per stage; SSBO bindings 80, SSBO block size 134217728, SSBO alignment 16 |
| caps-limits-compute | PASS | group count 65535 x3, group size 1024/1024/64, invocations 1024, shared memory 32768, 16 storage blocks, 16 image uniforms, 16 texture units |
| caps-limits-stages | PASS | 16 vertex attribs, 16 bindings, stride 2048, 16 varying vectors, 16 texture units per stage (96 combined), 256 image units, 16 image uniforms and 16 storage blocks in vertex and fragment too |
| caps-webgpu-compat-limits | PASS | 25 limits as `PhysicalDeviceGL.cpp:341-430` derives them, against the compatibility tier of `Limits.cpp`: 0 below |
| gl-procs-es32 | PASS | 358 of 358; an invented name does not resolve, so the check means something |
| gl-procs-ext | PASS | 26 of 28 (absent: `glDrawArraysInstancedBaseInstanceANGLE`, `glPolygonModeNV`) |
| gl-dawn-hard-requirements | PASS | ES 3.2, so neither `EXT_color_buffer_float` nor `KHR_robustness` is needed (both present anyway) |
| gl-extensions-dawn | PASS | 43 of the 53 the backend looks at. Absent: `APPLE_texture_format_BGRA8888`, `NV_sRGB_formats`, `EXT_float_blend`, `EXT_multisampled_render_to_texture`, `AMD_gpu_shader_half_float`, `AMD_gpu_shader_int16`, `ANGLE_base_vertex_base_instance`, `OES_texture_float_linear`, `EXT_shader_framebuffer_fetch`, `NV_polygon_mode`; none is required |
| gl-dawn-features | PASS | Dawn would enable: texture-compression-bc, -astc, -etc2, dual-source-blending, unorm16 formats, anisotropic filtering, BGRA8 as `GL_BGRA8_EXT`. Not: float32-blendable, shader-f16, msaa-render-to-single-sampled, indirect-first-instance |
| shader-compile-compute | PASS | three compute shaders through `glCompileShader` (front end only): SSBO + UBO + `length()`; image load/store rgba8/r32f/rgba16f/rgba32ui; samplers + `shared` + `barrier()` + `atomicAdd` at 320 es |
| shader-compile-render | PASS | UBO, fragment SSBO, shadow and stencil samplers, MRT, `gl_FragDepth`, discard |
| formats-color-rt | PASS | all 30 complete (R8..RGBA32F, integer formats, RGB10_A2, R11F_G11F_B10F, norm16, RGBA8_SNORM, BGRA8_EXT); RGB9_E5 is not renderable, as on any ES |
| formats-depth-stencil-rt | PASS | D16, D24, D24S8, D32F, D32F_S8, S8: complete as textures, single-sample and 4x |
| formats-msaa | PASS | 4x `glTexStorage2DMultisample` attachment complete for RGBA8, SRGB8_A8, RGBA16F, RGBA32F, R8, RG8, RGB10_A2, RGBA8UI, D24S8, D32F |
| formats-compressed-list | PASS | 51 formats: 12 S3TC/RGTC/BPTC, 10 ETC2/EAC, 28 ASTC LDR |
| formats-msaa-bgra8 | **FAIL** | `glTexStorage2DMultisample(4, GL_BGRA8_EXT)`: `Assertion failed: screen->is_format_supported(screen, format, target, 0, 0, PIPE_BIND_SAMPLER_VIEW)` in `st_texture_create` (Mesa debug build) |
| 66 others | SKIP host | every group that links a program: the PS5 shader compiler stub (`psbc_compile_nir`) is reached at the first `glLinkProgram` |

### Refcheck (`results/refcheck-softpipe-2026-11-13.log`)

The same probe over softpipe, every group really running: 83 PASS, 10 FAIL.
The 10: the three EGL results above (same facade), `formats-msaa-bgra8`, and
six multisample results (softpipe has `GL_MAX_SAMPLES` 1). So **every
console-only test except the five in the `msaa` group has passed on a real GL
implementation**: compute SSBO/UBO/images/shared/atomics, indirect draws and
dispatch, copy-image, PBOs, float and depth/stencil targets, sRGB, BGRA,
instancing and base vertex, MRT, primitive restart, texture buffers, the nine
hand-made compressed blocks, clip distances, timer queries. A FAIL on the
console in those is the driver's. The `msaa` group's expectations (edge
pixels intermediate, sample-shading mean 128, sample mask 0x5 gives 128) are
unverified.

## Dawn's GLES backend: requirement checklist

Dawn `bc34d95bda12`, paths under `src/dawn/native/opengl/`. "have (host)" was
checked by running; "console" means the probe has a test that only the
console can run.

| Requirement | Dawn source | ps5-opengl | Shim / patch |
| --- | --- | --- | --- |
| Compatibility mode only: core adapters are never offered | `BackendGL.cpp:71-74`, `PhysicalDeviceGL.cpp:579-581` | n/a | none possible: Route G is `featureLevel: "compatibility"` content only |
| The embedder can pass its own `getProc` and `EGLDisplay` instead of Dawn loading `libEGL.so` | `BackendGL.cpp:128-132`, `include/dawn/native/OpenGLBackend.h:54-59` (`RequestAdapterOptionsGetGLProc`) | n/a (static title: nothing to `dlopen`) | this is where the shim goes |
| Every `egl*` entry point through that `getProc` (23 at load, 10 more for EGL 1.1-1.4) | `EGLFunctions.cpp:117-206` | **missing** (host): 0 of 33 through `eglGetProcAddress` | shim: a table like spike 11's `egl_lookup.c` (34 symbols), then `eglGetProcAddress` for `gl*` |
| EGL >= 1.4 | `DisplayEGL.cpp:113-116` | have (host): 1.4 | |
| `EGL_CLIENT_APIS` contains `OpenGL_ES` | `DisplayEGL.cpp:121-128` | have (host) | |
| `EGL_EXT_create_context_robustness` | `BackendGL.cpp:94-96` | have (host) | |
| `EGL_KHR_fence_sync` or `EGL_KHR_reusable_sync` | `BackendGL.cpp:97-101`; used by `QueueGL.cpp:72-78, 241-260`, `UtilsEGL.cpp:93-116, 171-187` | **missing** (host) | shim: `eglCreateSyncKHR` / `eglDestroySyncKHR` / `eglClientWaitSyncKHR` / `eglGetSyncAttribKHR` over `glFenceSync` / `glClientWaitSync` (Dawn creates and waits with its own context current), and an `eglQueryString` that adds `EGL_KHR_fence_sync`. About 80 lines; could equally be patch 0021 in `ps5_egl.c`. GL fences themselves: console (`sync-gl-fence`; spike 11 saw them work across the share group) |
| Queue never uses GL sync objects | `QueueGL.cpp` (no `glFenceSync` anywhere in the backend) | n/a | so the EGL sync shim is not optional |
| `EGL_KHR_create_context` (else ES 2.0 is asked for and the version checked after) | `ContextEGL.cpp:160-177` | have (host) | |
| ES 3.1 context request | `ContextEGL.cpp:163-164` | have (host): returns ES 3.2 | |
| `EGL_KHR_no_config_context` (else an `EGL_WINDOW_BIT` config with `EGL_CONFORMANT` ES2) | `ContextEGL.cpp:141-150`, `DisplayEGL.cpp:168-170` | have (host) | |
| `EGL_KHR_surfaceless_context` (else a 1x1 pbuffer) | `ContextEGL.cpp:226-247` | have (host) | |
| Robust access + `LOSE_CONTEXT_ON_RESET`, EXT attribute values below EGL 1.5 | `ContextEGL.cpp:179-192`, `PhysicalDeviceGL.cpp:560` | have (host): accepted, strategy reported back | |
| Share context: always `EGL_NO_CONTEXT`; no option | `ContextEGL.cpp:220-221` | n/a | shim: an `eglCreateContext` wrapper that passes the anchor (or compositor) context as the share context; or a one-line Dawn patch |
| Wrapping an embedder GL texture needs the `ANGLETextureSharing` feature, i.e. `EGL_ANGLE_display_texture_share_group`, and passes `EGL_DISPLAY_TEXTURE_SHARE_GROUP_ANGLE` at context creation | `DeviceGL.cpp:432-447`, `PhysicalDeviceGL.cpp:265-267`, `ContextEGL.cpp:194-197`, `OpenGLBackend.h:77-85` | missing (host; the facade refuses unknown attributes) | shim: advertise the extension and strip the attribute in the `eglCreateContext` wrapper (real sharing then comes from the share context above); or relax the check in Dawn. Unverified: not built |
| `eglBindAPI(EGL_OPENGL_ES_API)` on the calling thread | `ContextEGL.cpp:152` | have (host) | the compositor thread keeps `EGL_OPENGL_API` bound (spike 11): rebind after Dawn calls, or in the wrapper |
| Dawn leaves its context current after a call unless `GLAllowContextOnMultiThreads` / `GLDefer` | `ContextEGL.cpp:413-430`, `PhysicalDeviceGL.cpp:566-567` | n/a | embedder makes its own context current again (as with ANGLE in spike 11) or sets the toggle |
| OpenGL ES >= 3.1 | `PhysicalDeviceGL.cpp:162` | have (host): 3.2 | |
| Below ES 3.2: `GL_EXT_color_buffer_float` | `PhysicalDeviceGL.cpp:163-165`, `:462` | not needed (3.2); present | |
| `glGetGraphicsResetStatus` (3.2 core, or KHR/EXT robustness) | `PhysicalDeviceGL.cpp:174-176`, `OpenGLFunctions.cpp:50-67`; called on every submit, `UtilsGL.cpp:324-331` | have (host): resolves, returns NO_ERROR | |
| Every ES core command up to the context's version through `getProc` | `generator/templates/opengl/OpenGLFunctionsBase.cpp:44-53` | have (host): 358 of 358 | note: `LoadProc` (`:35-41`) tests the address of the member, not the pointer loaded, so a missing command would not be reported; none is missing here |
| GLSL the context's own version (`#version 320 es`) | `ShaderModuleGL.cpp:423-424` | front end: have (host). Codegen: console | |
| Limits read at adapter creation | `PhysicalDeviceGL.cpp:341-430` | have (host): all queries succeed, none below the compatibility defaults | |
| `glDispatchCompute` then `glMemoryBarrier(GL_ALL_BARRIER_BITS)`; `glDispatchComputeIndirect` | `CommandBufferGL.cpp:1250-1251, 1266-1267` | console (`compute-*`, `indirect-dispatch`, `memory-barrier-texture`, `time-compute-*`) | |
| `glDrawArraysInstanced`, `glDrawElementsInstanced`; base vertex and first instance folded into attribute offsets | `CommandBufferGL.cpp:249-276, 1473, 1495` | console (`draw-instanced-divisor`) | Dawn does not call the ES 3.2 `*BaseVertex` entry points or `EXT_base_instance`; both are probed anyway |
| `glDrawArraysIndirect`, `glDrawElementsIndirect` | `CommandBufferGL.cpp:1520, 1551` | console (`indirect-draw-*`) | |
| `GL_PRIMITIVE_RESTART_FIXED_INDEX` | `CommandBufferGL.cpp:1487, 1545` | console (`primitive-restart`) | |
| `glMapBufferRange` for every buffer map | `BufferGL.cpp:224, 261, 265, 307` | console (`compute-ssbo-map`) | |
| Texture-to-buffer: `glReadPixels` into `GL_PIXEL_PACK_BUFFER`; buffer-to-texture from `GL_PIXEL_UNPACK_BUFFER` | `CommandBufferGL.cpp:890, 943` | console (`readpixels-pbo`, `texsubimage-pbo`) | on ES most non-RGBA8 readbacks go through compute blits instead (`PhysicalDeviceGL.cpp:474-503`), which leans on compute again |
| `glCopyImageSubData` for texture-to-texture | `CommandBufferGL.cpp:1128`, `UtilsGL.cpp:77-89` | console (`copy-image-*`; spike 11 saw the 2D case work) | |
| `glTexStorage2DMultisample`, resolve by `glBlitFramebuffer` | `TextureGL.cpp:173`, `CommandBufferGL.cpp:645-670` | completeness: have (host). Drawing and resolve: console (`msaa-resolve-*`) | **4x `GL_BGRA8_EXT`: see "found" above** |
| `glSampleMaski`, `GL_SAMPLE_ALPHA_TO_COVERAGE`, `GL_SAMPLE_MASK` always enabled | `RenderPipelineGL.cpp:347-351`, `DeviceGL.cpp:206` | console (`sample-mask`, `alpha-to-coverage`, `sample-shading`) | |
| `glClearBufferfi/fv/iv`, `glDrawBuffers`, `glInvalidateFramebuffer` | `CommandBufferGL.cpp:1362, 1443-1449, 1660` | console (`depth-stencil-*`, `mrt-4`) | |
| Stencil sampled through `GL_DEPTH_STENCIL_TEXTURE_MODE`; `GL_STENCIL_INDEX8` textures (3.2) | `CommandBufferGL.cpp:459-464`, `GLFormat.cpp:43-49` | completeness: have (host). Sampling: console (`depth-stencil-*`) | |
| BGRA8 as `GL_BGRA8_EXT` with immutable storage: `GL_EXT_texture_format_BGRA8888` + `GL_EXT_texture_storage` | `GLFormat.cpp:33-58` | extensions and FBO completeness: have (host). Upload, sampling, read: console (`bgra8`) | BGRA readback is a compute blit unless `GL_EXT_read_format_bgra` (present: `PhysicalDeviceGL.cpp:452, 488`) |
| No `GL_FRAMEBUFFER_SRGB` on ES: sRGB attachments always encode | `DeviceGL.cpp:200-205` | console (`srgb-framebuffer`) | |
| Optional features by extension (BC, ASTC, ETC2, dual-source blending, norm16, ...) | `PhysicalDeviceGL.cpp:205-325` | extension presence: have (host). Decoding when sampled: console (`compressed-*`) | |
| Debug output when backend validation is on (3.2 core) | `DeviceGL.cpp:171-195` | commands resolve (host) | |
| No texture views on ES (`glTextureView` is desktop-only in Dawn) | `TextureGL.cpp:716` | n/a | a compatibility-mode restriction, not ours |
| Texture buffers, timer queries, `EXT_base_instance`, clip distances | not used by the GLES backend at this commit | present (host); console tests exist | informational |

## Console run (not done: another task has the console)

Title: `spikes/12-webgpu/p0d-glprobe/build/titles/PPSA99016` (22 MB, built
against `toolchain/ps5-opengl-gles-noprof-m8`, fake-signed, integrity valid).
PPSA99016 is new: `--install` registers it on first use (spike 11's note says
the owner does the registering). The title draws nothing on screen, runs the
probe once, writes `/data/ps5webkit-spike/glprobe.log`, and asks the launcher
to close it. Only one title can run at a time: check `--status` first.

```sh
. ./env.sh
export PS5WK_OWN_TITLES="PPSA99207 PPSA99016"
tools/launch.sh --status                         # nothing of someone else's running?
tools/logserver.sh &                             # optional: a TCP copy into logs/title-*.log
tools/launch.sh --install spikes/12-webgpu/p0d-glprobe/build/titles/PPSA99016   # upload, register if new, launch
sleep 60
curl -s "ftp://$PS5_HOST:2121/data/ps5webkit-spike/glprobe.log" \
    -o spikes/12-webgpu/p0d-glprobe/results/console-$(date +%F).log
tail -1 spikes/12-webgpu/p0d-glprobe/results/console-$(date +%F).log   # "done: N PASS, N FAIL, N SKIP ..." or the title died
tools/results.sh spikes/12-webgpu/p0d-glprobe/results/console-*.log    # the RESULT lines as a table
tools/launch.sh --status                         # the title closes itself; if it is still there: tools/launch.sh --kill
```

Later runs of an already-installed build: `tools/upload-title.sh <dir>` then
`tools/launch.sh PPSA99016`.

If the log ends without a `done:` line, the last `== <group>` line names the
group that took the title down (the crash guard's report follows it, and the
kernel log has `[ps5webkit] glprobe group <group>`). Run the rest without it:

```sh
tools/launch.sh PPSA99016 skip=compute-image            # or skip=a,b  /  only=a,b
```

Groups, in order: `egl ctx-dawn caps procs ext shader-frontend formats`
(these ran on the host), then `sanity compute-ssbo compute-image indirect
compute-more copy-image sync ubo draws restart pbo float-rt depth srgb bgra
msaa texbuffer compressed clip timer timing`, and last `bgra-msaa` (the
assertion case: last because nobody knows what the release build does with it).

Rebuild: `spikes/12-webgpu/p0d-glprobe/build-title.sh [VARIANT [TITLE_ID]]`
(the Mac's address for the TCP log is compiled in from `env.sh`).

## What the console run decides

1. **Compute at all** (`compute-ssbo-map` first, then the `compute-*` and
   `indirect-dispatch` results). Dawn's GLES backend uses compute not only for
   compute pipelines but for most texture-to-buffer copies on ES, so a hole
   here is a hole in ordinary rendering content too.
2. **Timings**: `time-compute-first-compile` (the cost of one compute pipeline)
   and `time-compute-dispatch-1000` (per-dispatch cost with the
   `glMemoryBarrier` Dawn adds after each).
3. **`formats-msaa-bgra8`**: if the release build fails cleanly (GL error),
   Dawn reports a texture-creation error for multisampled `bgra8unorm` and the
   embedder should prefer `rgba8unorm` as the canvas format; if it corrupts or
   crashes, the driver needs a fix (the format `st` picks for 4x BGRA8 is not
   one `ps5_is_format_supported` accepts as a sampler view).
4. The rest of the 66 are regressions-in-waiting for specific WebGPU features;
   each failing one maps to a row of the checklist.

## Files

| | |
| --- | --- |
| `src/glprobe.c` | contexts, EGL, caps, limits, entry points, formats, shader front end |
| `src/glprobe_gpu.c` | everything that links, draws or dispatches |
| `src/es32_procs.inc` | the 358 ES 2.0-3.2 commands, generated by `gen-procs.py` from Mesa's `gl.xml` |
| `src/main_title.c`, `build-title.sh` | the console title (PPSA99016) |
| `host/main_host.c`, `host/run.sh` | the Mac harness run (each group in a forked child) |
| `host/ref_glue.c`, `host/refcheck.sh` | the probe over softpipe, to test the probe |
| `results/` | `host-2026-11-13.log`, `refcheck-softpipe-2026-11-13.log` |

## Console run (2026-11-13, added by the lead after the probe was written)

[results/console-2026-11-13.log](results/console-2026-11-13.log): **90 PASS, 3 FAIL, 0 SKIP** in 244 ms; the title
closed itself. The three failures are the EGL facade gaps already seen on the host (`egl-dawn-required`,
`egl-sync`, `egl-getproc-egl`), all covered by the embedder shim proposed above. Everything that needed the GPU
passed, compute included: `compute-ssbo-map` right, first compute compile + link 0.9 ms, 13.8 µs per dispatch.
`bgra-msaa` did not take the title down.

So Route G is a workable fallback (compatibility mode only). Gate 1 chose Route V, which also passed
(../p0b-radv, ../p1-dawn-title) and is the only one that offers core WebGPU.
