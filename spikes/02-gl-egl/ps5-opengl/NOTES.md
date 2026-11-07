# ps5-opengl: SDK build on macOS, GLES enablement, texture export for PS VR2

Source: `ref/ps5-opengl` (mihawk-99 fork, commit `ec06d8c`, GPL-3.0-or-later).
Everything here was built and checked on an arm64 Mac with no console. Nothing
has run on PS5 hardware.

## Results

| | Result |
| --- | --- |
| Stock SDK (`toolchain/ps5-opengl-stock`) | Builds from source in about 2.5 min (`build.sh stock --clean`). 19 archives plus 2 AGC import stubs. `verify_gl46_link_surface`: 657/657 GL 4.6 core commands exported. Manifest verified. |
| GLES SDK (`toolchain/ps5-opengl-gles`) | Builds the same way (`build.sh gles --clean`) with `patches/0001` and `0002`. Mesa uses `-Dgles2=enabled`. Adds `GLES2/` and `GLES3/` headers and `ps5_egl_export.h`. A GLES 3 consumer links with `-z defs` (no unresolved symbols) and packages as a title. |
| GLES version | **OpenGL ES 3.2** (`"OpenGL ES 3.2 Mesa 26.2.0"`, GLSL ES 3.20, 144 extensions) once the patch adds two driver fixes. Without them, Mesa caps this driver at **ES 2.0**, and ES context creation fails in any case (details below). |
| Tiling | Single-level 2D RGBA8, SRGB8_A8, R8, RG8 and RGBA16F textures that are both render target and sampler use **SW_64KB_R_X** (SW_MODE 27). That is the native layout, 64 KiB aligned, the same as PS5VR's eye buffers. Everything else is linear. Renderable linear textures also get a tiled render-staging copy. |
| Stretch: export | Done: `eglExportTextureDescriptorPS5` (`EGL_PS5_texture_descriptor_export`). Checked on host only. |
| Packaging | `package-title.sh` replaces the deleted boilerplate using PS5VR's vendored native-app tooling. It produces `eboot.bin` (fake-signed; integrity valid), `sce_sys/param.json` and `sce_module/libc.prx`. |

## Route taken

I built natively on macOS arm64 and used neither Docker nor sudo. The cross
compiler is our ps5-payload-sdk master (Homebrew LLVM 18.1.8). Mesa, PSBC and
the runtime need only `PS5_PAYLOAD_SDK`, so the deleted
`ps5-native-app-boilerplate` matters only for **title packaging**.
`package-title.sh` covers that part (see below). The whole build of one
variant, from export through fetch, PSBC, Mesa, runtime and install, takes
about 2.5 min on 14 cores. That is fast enough that an amd64 container is not
worth it.

`build.sh [stock|gles|all] [--clean]`:
1. Runs `git archive ec06d8c` from `ref/ps5-opengl` into
   `build/<variant>/` (gitignored), then applies `patches/*.patch` for the
   gles variant. The tree is a throwaway git repo, so `git -C build/gles diff`
   shows the patch.
2. Runs upstream `tools/fetch-sources.py`. It fetches pinned commits into the
   variant's `third_party/`, verifies the Mesa 26.2.0 tarball sha256
   (`efd4bb08…`) and checks that the PSBC patched tree equals
   `21ce3d77…`. All of these checks passed.
3. Generates the PSBC sources. This is upstream `build-opengnm-psbc.sh` minus
   the host CLI binary, which only Linux-only host tests use.
4. Runs upstream `build-opengnm-psbc-ps5.sh`, `build-mesa-ps5.sh` (with
   `mesa-ps5.patch`, cross file `$PS5_PAYLOAD_SDK/toolchain/prospero.ini`) and
   `install-ps5-opengl-gl46.sh <prefix>`, all unmodified for stock.
5. Checks the link surface and the manifest, and that EGL_CLIENT_APIS in
   `ps5_egl.o` is "OpenGL" or "OpenGL OpenGL_ES". For gles it also link-probes
   `examples/gles3_probe.c`.

Host prerequisites: Homebrew `llvm@18`, `bison` (keg-only; **installed during
this work** with `brew install bison`), `coreutils`, `bash`, `ninja`, plus
Python 3.13. `build.sh` creates a venv with meson 1.10.1, mako, pyyaml and
packaging.

### Problems hit and fixes (all in build.sh, no upstream edits needed for stock)
- macOS bison is 2.3, but Mesa needs >2.3 (glcpp grammar). Fix: put
  `/opt/homebrew/opt/bison/bin` first on PATH.
- The upstream scripts are written for Linux: `realpath -m`,
  `sha256sum --check --status`, `install`, `mapfile` (bash 4+) and `ar -M`
  (thin-archive materialisation). Fix: coreutils `gnubin` and Homebrew bash on
  PATH, and an `ar` → `llvm-ar` shim (also `nm` → `llvm-nm`).
- Meson found Homebrew python3.14 before the venv. It still selected the venv
  python, because only that one has mako. No action was needed.
- These upstream checks are **not** run because they fail for reasons unrelated
  to the build:
  - `make test-compiler`: the host compile needs `clang-18` and `<endian.h>`,
    and is Linux-only.
  - `tests/ps5/verify_gl33_capability_audit.py`: stale source-text assertion
    (`tiled_size + PS5_RENDER_ALIGNMENT - 1u` no longer exists in the fork's
    `ps5_screen.c`). This is upstream drift, not a build problem.
- `rm -rf` of a build tree occasionally hit "Directory not empty" (Spotlight).
  `build.sh` retries once.
- The payload SDK's `prospero-clang` rejects `-fuse-ld`. The libc++ archives
  carry a `pthread` dependent-library specifier, so a bare `ld.lld` link needs
  `-L$SDK/target/lib` (PS5VR works around this with its own `libpthread.a`).

### Toolchain caveat
The upstream release was built with payload SDK **v0.42** and Clang 21.1.8 for
Mesa/PSBC, plus Clang 18 for the title glue. Ours is SDK master with LLVM 18
for everything. It compiles warning-clean under the same `-Werror` runtime
flags. Even so, the binaries are new and none has a hardware receipt (upstream
itself says source reproducibility is not binary identity). Treat the first
console run of either variant as a bring-up.

### Replacing the deleted boilerplate
From `tools/build-native-test-app.sh`, the boilerplate supplied four things:
1. The payload SDK v0.42. Ours replaces it.
2. A CRT/startup plus `tooling/native/ps5-pie.ld`.
3. The converter and fake-signer (`sce_module_writer.cpp` etc.). ps5-opengl
   patches its libc heap_size to 256 MiB.
4. `libc.prx` and the folder-title layout.

`ref/PS5VR/app/tools/native-app/` is a vendored copy of that same boilerplate
tooling (file headers say "ps5-native-app-boilerplate"; the heap line ps5-opengl
seds is there verbatim). PS5VR's `ps5-pie.ld` is the base script with the
`__eh_frame*` PROVIDEs inlined, which is exactly what ps5-opengl's wrapper
`native-app/ps5-pie.ld` adds. PS5VR's loader constants are hardware-validated:
module SDK 0x02000009, companion 0x08050001, fSELF magic 0x1D3D154F.
`package-title.sh` therefore:
- builds `ps5-native-tool` with Apple clang++ and system zlib, applying the
  same 256 MiB heap patch ps5-opengl applies;
- links PS5VR `app_crt.cpp` plus ps5-opengl `native-app/runtime_shims.c`
  (log to `/download0/ps5-opengl.log`, `catchReturnFromMain`, the glapi TLS
  init stub) and `app_heap.c` with ps5-opengl's `--wrap=malloc…` set;
- runs `ps5-native-tool link` then `self --sign`, and assembles the folder
  title with ps5-opengl's `param.json` (new titleId and contentId).

Not reproduced: the boilerplate's `Makefile` and `assets/`, `runtime/` and
`sce_sys/` art. These are cosmetic or unknown. Whether the boilerplate runtime
did more than the CRT is the main **packaging risk** (see Risks).

## GLES enablement (`patches/0001-gles2-contexts.patch`)

What the patch changes:
- **`build-mesa-ps5.sh`**: `-Dgles2=enabled` (gles1 stays disabled). The
  archive set is unchanged. `libglapi_bridge.a` then also exports the ES-only
  `glBlendBarrier` and `glPrimitiveBoundingBox`.
- **`ps5_egl.c`**:
  - `eglBindAPI` accepts `EGL_OPENGL_ES_API`.
  - `EGL_CLIENT_APIS` reports `"OpenGL OpenGL_ES"`.
  - Both configs report `EGL_RENDERABLE_TYPE = OPENGL|ES2|ES3` (0x4c), and
    `eglChooseConfig` accepts those bits.
  - `eglChooseConfig` now also accepts the other standard attributes
    (`EGL_COLOR_BUFFER_TYPE`, `BIND_TO_TEXTURE_RGB[A]`, `LUMINANCE/ALPHA_MASK_SIZE`,
    `MIN/MAX_SWAP_INTERVAL`, `NATIVE_VISUAL_TYPE`, `TRANSPARENT_*_VALUE`,
    `MAX_PBUFFER_*`, `MATCH_NATIVE_PIXMAP`). Before, they returned
    EGL_BAD_ATTRIBUTE, which would trip ANGLE-style config queries.
  - `eglCreateContext` with ES bound reads `EGL_CONTEXT_CLIENT_VERSION` /
    `MAJOR_VERSION_KHR` (same enum, default 1) and `MINOR_VERSION_KHR`. 2.0 and
    3.0–3.2 succeed and create an `API_OPENGLES2` st context at the highest ES
    version (Mesa semantics). 1.x, 2.1, 4.x, or anything above Mesa's ES
    version fails with EGL_BAD_MATCH. A profile mask or the forward-compatible
    flag fails with EGL_BAD_ATTRIBUTE (KHR_create_context).
  - `EGL_CONTEXT_CLIENT_TYPE` reports the context's real API.
  - EGL_CONFORMANT stays OpenGL-only, because no ES CTS has run on real ES
    contexts.
- **`ps5_screen.c`** (the two fixes that make ES actually work):
  1. `ps5_context_create` rejected every flag outside its whitelist. Mesa
     passes `PIPE_CONTEXT_NO_LOD_BIAS` for **every** ES context, so ES context
     creation always failed (EGL_BAD_ALLOC) even with gles2 compiled in. The
     fix accepts the flag as a hint.
  2. Caps: `texture_half_float_linear = true` and
     `fragment_shader_derivatives = true`.
     - Without the first, `compute_version_es2()` returns **20**:
       OES_texture_half_float_linear is the *only* missing ES 3.0 input. All
       ES 3.1 and 3.2 inputs are already present, including ASTC via the
       compressed fallback, KHR_blend_equation_advanced via fbfetch,
       geometry/tessellation, and OES_texture_buffer/cube_map_array.
     - FP16 filtering is already required of desktop GL 3.0+ and exercised by
       the GL CTS. These two caps only feed ES-only extension flags in
       `st_extensions.c`.
     - `texture_float_linear` is deliberately left unset. Mesa also uses it to
       force NEAREST sampling of FP32 textures in *all* APIs
       (`ForceFloat32TexNearest`), and changing it would alter validated GL
       behaviour. Consequence: no `GL_OES_texture_float_linear` for WebGL.
- **ps5_egl.o build flags**: `-DHAVE_OPENGL=1 -DHAVE_OPENGL_ES_1=0
  -DHAVE_OPENGL_ES_2=1`, matching Mesa's global pre_args.
- **`install-ps5-opengl-core33.sh`**: also installs `GLES2/` and `GLES3/`.
- **`generate-gl46-entrypoints.py`**: also wraps ES 3.2-only commands if the
  bridge lacks them (with gles2 on, it already has them), and adds an `OES`
  stub suffix.

### How the version was determined: `hostcheck/`
`hostcheck/run.sh` builds an x86_64 macOS binary and runs it under Rosetta.
The binary combines:
- the **real** `ps5_screen.c` (caps, format table, resource layout and
  descriptor code), compiled with the exact defines from
  `ps5-opengl-core33.mk`;
- the patched `ps5_egl.c`;
- the same patched Mesa 26.2 (host build with `-Dgles2=enabled`);
- direct memory faked with mmap, with AGC/PSBC entry points as aborting stubs
  and weak hooks left NULL, as on a console without the backend.

No GPU work is submitted. It prints the inputs to `compute_version_es2`
(`hostcheck/es_diag.c`) and exercises EGL, contexts and the export. Results:

- **Stock caps:** `_mesa_get_version(API_OPENGLES2) = 20`. The only `NO` is
  OES_texture_half_float_linear. ES 2.0 context creation fails with
  EGL_BAD_ALLOC (the NO_LOD_BIAS issue).
- **Patched:** `es2=32`. ES 2.0, 3.0, 3.1 and 3.2 requests all give
  `"OpenGL ES 3.2 Mesa 26.2.0"` / `"OpenGL ES GLSL ES 3.20"` with 144
  extensions. These include:
  - EXT_color_buffer_float and _half_float
  - KHR_blend_equation_advanced, KHR_robustness
  - KHR_texture_compression_astc_ldr
  - OES_copy_image, OES_draw_buffers_indexed, OES_geometry_shader,
    OES_tessellation_shader
  - OES_texture_buffer, OES_texture_cube_map_array, OES_texture_stencil8
  - EXT_texture_filter_anisotropic, EXT_texture_format_BGRA8888

  Missing: OES_texture_float_linear (see above).
- **GL is unchanged:** a GL 4.6 core context still reports
  `"4.6 (Core Profile) Mesa 26.2.0"`, matching upstream's hardware reports.
  This cross-checks that the harness reproduces the console's caps.

About the README claim that the "OpenGL ES 3.0/3.1 tests under OpenGL 4.5
pass": per `validation/2026-10-01-gl46-conformance/run.json`, those are the
`gl45-es3-main` / `gl45-es31-main` CTS case lists, run on **desktop GL 4.5
contexts** through ARB_ES3/ES3_1_compatibility (`--deqp-gl-config-id=2`).
They show that ES 3.x feature semantics work on this driver. They never
created an ES context, which was impossible before this patch.

## Linear vs tiled (`ps5_screen.c`, `ps5_linear_sampled_layout` and the descriptor builders)
- **Tiled, native (SW_MODE 27 = SW_64KB_R_X)** applies to a `PIPE_TEXTURE_2D`
  that meets all of these:
  - format R8G8B8A8_UNORM, R8G8B8A8_SRGB, R8_UNORM, R8G8_UNORM or
    R16G16B16A16_FLOAT;
  - `last_level == 0`, `array_size == 1`, no MSAA;
  - bind ⊇ RENDER_TARGET|SAMPLER_VIEW;
  - size ≤ 16384.

  The data is 64 KiB aligned (`PS5_COLOR_TARGET_ALIGNMENT`), and the footprint
  is whole 64 KiB tiles (`ps5_tiled_color_surface_size`; for 4 bpp the tile is
  128×128 px). Rendering and sampling use the same memory, with no copies. The
  T# word 3 is `0x91b00fac`, giving TYPE=2D and SW_MODE=27. The scanout/window
  targets (B8G8R8A8, RENDER_TARGET|DISPLAY_TARGET) and RT-only resources are
  tiled too.
- **Tiled depth** uses SW_64KB_Z_X (24), word 3 `0x918…`.
- **Linear** covers everything else: mip chains, arrays, cubes, 3D, other
  formats such as RGB10_A2 and BGRA, and sampled-only textures. Rows are padded
  to 256 bytes and mips are stored in reverse order. Linear textures that are
  also render targets get a hidden **tiled render-staging copy**
  (`render_staging_offset/size`): draws render into the staging copy, which is
  copied back to linear.
- **MSAA4 colour** uses a tiled layout with its own descriptor (`0xe1b2…`).
- **Forcing SW_64KB_R_X** for a GL render target:
  - create it with `glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8 |
    GL_SRGB8_ALPHA8 | GL_RGBA16F | GL_R8 | GL_RG8, w, h)`;
  - don't call `glGenerateMipmap` and don't use mip levels > 0;
  - don't use texture arrays, rectangle textures or BGRA.

  Mesa gives colour-renderable textures RENDER_TARGET|SAMPLER_VIEW binds, so
  these land in the native layout automatically. Immutable storage keeps the
  address stable. Adding RGB10_A2, the 10-bit VR format that PS5VR also builds,
  would mean adding it to `ps5_linear_sampled_layout`'s list. It has the same
  4-byte tiling as RGBA8, but that is untested.

## Stretch: `eglExportTextureDescriptorPS5` (`patches/0002-egl-export-texture-descriptor.patch`)
```c
#include <ps5_egl_export.h>   /* installed in the gles prefix */
EGLBoolean eglExportTextureDescriptorPS5(EGLDisplay, EGLContext ctx, GLuint texture,
    uint32_t tsharp[8], uint64_t *gpu_address, uint32_t *width, uint32_t *height,
    uint32_t *pitch /* texels */, uint32_t *swizzle_mode /* 0 linear, 27 SW_64KB_R_X */);
```
It is also reachable through `eglGetProcAddress` and advertised as
`EGL_PS5_texture_descriptor_export`.

What it does:
- `ctx` must be current on the calling thread (EGL_BAD_ACCESS otherwise).
- It finishes glthread, looks up the texture (2D or RECTANGLE), finalizes it,
  then `st_context_flush(ST_FLUSH_WAIT)`, so all queued GL work has retired on
  the GPU.
- It then calls the new driver helper `ps5_resource_export_descriptor()`. That
  helper returns the driver's **own** sampled T# (the same
  `ps5_resource_image_descriptor` path its shaders use), the GPU VA,
  width/height, pitch, and SW_MODE decoded from word 3.

It refuses (EGL_BAD_MATCH) anything whose memory isn't the image itself: MSAA,
depth/stencil, display targets, and anything with render or depth staging (in
practice, every renderable texture outside the native list).

Host-checked results, on the real descriptor code:

| Texture | Result |
| --- | --- |
| RGBA8 2048×1024 | va 64 KiB aligned, SW_MODE 27, T# `011d0000 c3800000 80ffc1ff 91b00fac 0 00400000 0 0` |
| SRGB8_A8 1000×600 | SW 27, pitch 1024 |
| RGBA16F 512×512 | SW 27, format 71 |
| 3-level RGBA8, RGB10_A2, DEPTH32F | refused |

Compared with PS5VR's `eng_agc_build_tsharp_render_target` for the same
address and size, words 0, 1, 3, 4, 6 and 7 are identical: address, format
(56 or 71), size, dst_sel XYZW, TYPE 2D and SW_64KB_R_X. The differences are:
- word 2 bit 31: ps5-opengl sets it, PS5VR does not;
- word 5: ps5-opengl `0x00400000`, PS5VR `0x00700000` (bits 20–21);
- for sRGB, ps5-opengl uses the 8_8_8_8_SRGB format, where PS5VR's
  eye-buffer builder is UNORM-only.

Both are hardware-validated in their own uses (ps5-opengl sampling; PS5VR Hmd2
layers). If the compositor proves picky, rebuild the T# with PS5VR's builder
from `gpu_address/width/height`. The outputs are exactly its inputs.

Why zero-copy is plausible:
- ps5-opengl allocates with `sceKernelAllocateDirectMemory(type 12 =
  WB_ONION)` + `sceKernelMapDirectMemory(prot 0x33)`. That is identical to
  PS5VR's `direct_alloc` for eye buffers.
- The driver programs `resource->data` (the CPU VA) unchanged into every
  descriptor and colour target, so GPU VA == CPU VA.

Consumer pattern (from `app_vr.c`):
1. Allocate N immutable RGBA8 textures sized like PS5VR's eye buffers
   (2×EYE_W × EYE_H, eye width 128-aligned).
2. Export each one once.
3. Copy the T# into the layer struct at +0x04 and +0x24.
4. `sceVideoOutRegisterBuffers2`.
5. Per frame: render with GL, `eglExportTextureDescriptorPS5` (or just
   `glFinish`), `sceHmd2ReprojectionBeginFrame`, then submit.

Caveats:
- Export does a full GPU wait. The cheaper path is a fence: per-frame export
  isn't needed, because the address is fixed.
- The VideoOut flip/compositor runs on a separate queue from ps5-opengl's AGC
  submissions. Only the CPU-side wait orders them.
- Deleting or reallocating the texture while it is registered is the caller's
  bug.

## Linking a consumer against the SDK
Compile:

```sh
. env.sh
prospero-clang -DGL_GLEXT_PROTOTYPES=1 -I$PS5WK_ROOT/toolchain/ps5-opengl-gles/include -c app.c
```

Headers: `EGL/`, `GL/`, `GLES2/`, `GLES3/`, `KHR/`, `ps5_opengl_display.h`
and `ps5_egl_export.h`.

Libraries, as in `lib/pkgconfig/ps5-opengl.pc` and
`share/ps5-opengl/ps5-opengl.mk`:

```
-Wl,-u,ps5_agc_gate2_run -L$P/lib -Wl,--start-group -lPS5OpenGL -Wl,--end-group \
  $P/lib/libSceAgc.so $P/lib/libSceAgcDriver.so -lSceVideoOut -lkernel_web -lSceSystemService \
  -lc++ -lc++abi -lunwind
```

`libPS5OpenGL.a` is a GROUP linker script over the 19 archives. `-u
ps5_agc_gate2_run` pulls in the AGC backend; without it the EGL window path
has no presenter. A CMake config (`lib/cmake/PS5OpenGL`) also exists.
`build.sh` link-probes exactly this with `-z defs`. The result
(`build/gles-probe/gles3_probe.elf`) is 52 MB unstripped, with DT_NEEDED
libSceAgc.prx, libSceAgcDriver.prx, libSceVideoOut, libkernel_web,
libSceSystemService, libSceLibcInternal and libSceNet.

## Packaging as a registered title
Do not use elfldr; the app must be a registered folder title.

```sh
spikes/02-gl-egl/ps5-opengl/package-title.sh [-p <sdk prefix>] [-t PPSA99006] [-n "name"] \
    -o <outdir> app.o [more.o ...]
```

The output is `<outdir>/<TITLE>/{eboot.bin, sce_sys/param.json,
sce_module/libc.prx}`. For example, `build/titles/PPSA99006` for
`examples/gles3_probe.c` is 22 MB, and the `self --inspect` integrity check is
valid.

To install: upload the folder to `/data/homebrew/<TITLE>`, register it once as
a folder app, then launch the title. Later iterations only replace files. The
log is `/download0/ps5-opengl.log`.

## Risks / open items
- **Hardware unproven:** a new toolchain (SDK master/LLVM 18 vs v0.42/Clang
  21), new packaging glue, and the GLES path have never run on hardware.
  First test: launch the `gles3_probe` title and read
  `/download0/ps5-opengl.log` for `GL_VERSION`, the export line and 120
  swapped frames.
- **Packaging fidelity:**
  - PS5VR's CRT and heap patch are the same boilerplate lineage, but the
    boilerplate `Makefile`/`runtime/` contents are unknown.
  - `catchReturnFromMain` from ps5-opengl's shims parks the process after
    `main` returns, by design.
  - The `libc.prx` comes from PS5VR's vendored runtime. It is a console module:
    keep it out of git, which `.gitignore` already does.
- **No EGLImage / dma-buf:** sharing between processes or with WebKit's
  compositor has to go through same-process GL textures or this PS5 export.
  Mesa still advertises GL_OES_EGL_image, but `glEGLImageTarget*` just returns
  a GL error because the frontend has no `get_egl_image`.
- **ES-specific behaviour is CTS-untested:** precision qualifiers,
  ES-only extensions, and `ForceFloat32TexNearest` (FP32 textures are always
  point-sampled, so OES_texture_float_linear is absent).
- **WebGL/ANGLE fit:**
  - WebGL 2 needs ES 3.0; we get 3.2.
  - ANGLE's EGL/GLES backend expects things this facade lacks:
    `EGL_KHR_image_base`, `EGL_KHR_fence_sync` / `EGL_ANDROID_native_fence_sync`
    (optional), and a sane `eglGetProcAddress` for core functions (present).
  - Only one window surface (the TV, fixed size), and pbuffers are limited to
    scanout size. WebKit offscreen rendering should use FBOs on a surfaceless
    or pbuffer context.
- **Export:**
  - Word 2 bit 31 and word 5 bits 20–21 differ from PS5VR's Hmd2 T#.
  - There is no async fence API.
  - RGB10_A2 eye buffers are not native-tiled.
- **Memory:** the 256 MiB render arena (`PS5_RENDER_ARENA_BYTES`) plus the
  256 MiB patched libc heap are both fixed. WebKit will want a bigger heap
  (`package-title.sh` heap patch).
- **GPL-3.0:** linking these static archives makes the consumer a combined
  GPL-3 work.

Not used: the third-party "ps5-opengl-sdk-1.0.2.tar.gz" (JMUtechnologies) was
neither downloaded nor consulted. Everything here is built from the pinned
public sources.

## Files
- `build.sh`: reproducible build of both variants.
- `package-title.sh`: link and package a folder title.
- `patches/0001-gles2-contexts.patch`: GLES enablement (EGL facade, driver,
  Mesa option, headers).
- `patches/0002-egl-export-texture-descriptor.patch`: export API, driver
  helper and public header.
- `hostcheck/`: Rosetta harness. Run `hostcheck/run.sh` after `build.sh gles`.
  Its log is `build/hostcheck/run.log`.
- `examples/gles3_probe.c`: minimal ES 3 plus export consumer, used for the
  link and package probes.
- `build/` (gitignored): variant trees, logs, venv, title output.

## Patch 0004: GPU blits into sRGB textures (2026-11-07, WebGL M4)

`patches/0004-srgb-gpu-blit-through-linear-view.patch`: `ps5_blit_gpu_color`
also takes an RGBA8 source into an `R8G8B8A8_SRGB` texture when the blit asks
for the destination's linear format (GL_FRAMEBUFFER_SRGB off), drawing
through an `R8G8B8A8_UNORM` surface of it. Before, that blit took the CPU
fallback: the title's copy of a 1080p video frame into an SRGB8_ALPHA8 WebGL
texture cost tens of ms (spikes/11-webgl, *M3 formats*). Built for trial with
`PS5_OPENGL_PREFIX=$PS5WK_ROOT/toolchain/ps5-opengl-gles-noprof-m4 build.sh
gles-noprof` (the new `PS5_OPENGL_PREFIX` installs a variant elsewhere, so
the browser's own `toolchain/ps5-opengl-gles-noprof` stays as it was). Not
run on the console yet. The `build/gles` tree has it applied (by hand, `git
apply`); a `--clean` build applies it with the others.


## Patch 0005: AGC register defaults fetched once (2026-11-07)

`append_target_state` takes `sceAgcGetRegisterDefaults()`'s table, and every
native draw (`ps5_agc_gate2_run`) called it again. Under the title profiler
(`busy.html?n=1000`, WebGL 2) that call was 28% of the title's CPU, all at
one address in the system library. `runtime_register_defaults` keeps the
pointer after the first call, calls the function again every 4096th use and,
if the pointer ever changes, logs `[ps5-agc] register defaults moved` and
stops caching. 1000 draws per frame went from 32.7 to 47.0 fps (30 -> 21 µs
per draw on the WebGL thread). The rest of the measured split is in
`spikes/11-webgl/README.md` (M4).

## Patch 0006: no restart conversion when no index is the restart index (2026-11-07)

WebGL 2 has primitive restart on for every indexed draw, and `ps5_draw_vbo`
sent every such draw through a `util_primconvert` context made and freed per
draw, its indices rewritten and uploaded, and it never reached the deferred
batch. `ps5_draws_contain_restart_index` scans the draw's indices in the
buffer's CPU copy (`resource->data`); when none equals `restart_index` the
draw goes on with `primitive_restart` off (user indices, out-of-range draws
and indirect draws convert as before). `busy.html` (WebGL 2): 1000 cubes
47.0 -> 54.7 fps, 2000 cubes 24.5 -> 29.9 (16 µs per draw on the WebGL
thread); textured 52.7 and 28.6; GL error 0.

## Patch 0007: a four-entry MRT register cache for every large table (2026-11-07)

`ps5_agc_set_cx_mrt` cached its rewritten register table in one entry, and
only for tables holding both 0x318 and 0x2d5 (the initial graphics table).
After 0006 the prepared draws' per-draw table lacks them, so it was rewritten
on every draw (8% of the title's CPU at 1000 draws per frame), and the WebGL
thread alternates two large tables (101 and 85 records), which evicted each
other. The rewrite depends only on the input records and the backend draw
state, so the cache now holds four entries for any table of 16 or more
records (keyed on all input bytes and on the draw state after the call:
with `PS5_DYNAMIC_SCANOUT` the call fills in an unset depth size).
`busy.html`: 1000 cubes 54.7 -> 59.8 fps (the 60 Hz cap), 2000 cubes 29.9
-> 32.9 (~15 µs per draw); GL error 0.

Note: `PS5_RUNTIME_QUIET` makes `printf` a no-op in `ps5_screen.c` and
`ps5_agc_runtime_backend.c`, and the title logs stderr, not stdout:
diagnostics go to `fprintf(stderr, ...)` (0005's warning does).

## Patch 0008: GPU depth/stencil clears for 4x MSAA (2026-11-07)

`ps5_clear_gpu_depth_stencil` (the blitter's depth quad) refused
multisampled targets, so every clear of a 4x depth/stencil buffer (the
default framebuffer of an antialiased WebGL canvas, as three.js makes it)
drained the GPU queue and filled and flushed the tiled depth and stencil
planes on the CPU: three.js's `webgl_geometry_cube` spent ~2.2 ms per frame
in `glClear`. The GPU path now admits 4x targets with 4x storage, sizing the
depth and stencil planes for 4 samples. The blitter draws with multisample
rasterization off, which on this hardware (as on radeonsi) gives each covered
pixel all its samples; the 4x colour clears already relied on it.
`tests/ps5/test_gpu_clear_state.py` covers the new bounds (on macOS run it
with `-no-pie` removed). On the console:
`port/webkit/test/webgl/msaa-clear.html` (every sample primed, cleared,
probed, resolved; also passes in desktop Chrome).
Result: all 43 cases PASS on the console; three.js `webgl_geometry_cube`
2.2 -> 0.24 ms per frame on the WebGL thread at 60 fps; `busy.html`
unchanged (1000 cubes 59.8, 2000 32.9 fps).

## Patch 0009: prepared draws clear and flush only what the GPU reads (2026-11-07)

Every prepared draw zeroed its 16 KiB state pages (`memory + 0x4000`) and
clflushed all of them (8% and 8% of the title's CPU at 1000 draws per frame
after 0006). The GPU reads those pages only through the indirect register
loads in the draw's stream (`0x63`/`0x64`/`0x9f`, each reading exactly its
count of records; the packets `validate_indirect_register_tables` checks)
and writes only the completion marker at 0x6ff0, and every table is built
from scratch per draw. So the clear now zeroes the marker only, and the
publish walks the finished stream and flushes each loaded table and the
marker line; a stream it cannot read, or a table outside the 64 KiB work
block, flushes the 16 KiB whole as before. The ranges are flushed with
`clflushopt` unfenced and fenced once per draw: a first version flushing each
range with `util_flush_inval_range` (two `mfence`s per call, and the fence
waits for every flush in flight) gained nothing, the publish going from 8% to
14% of the WebGL thread with a sixth of the lines. Host test:
`tests/ps5/test_prepared_shader_pairs.py`. `busy.html`: 2000 cubes 32.9 ->
37.7 fps (~12.6 µs per draw), 1000 cubes 15.8 -> 13.8 ms per frame on the
WebGL thread; `msaa-clear.html`, `webgl.html` and three.js's cube unchanged.

## Patch 0010: one fence per CPU flush (2026-11-07)

`ps5_flush_gpu_data` (Gallium) and the runtime's `flush_gpu_data` used
`util_flush_inval_range`: `clflushopt` over the range, `mfence`, the last line
flushed again, `mfence` (the repeat is a workaround for Atom's clflush
ordering). On this AMD CPU `clflushopt` is ordered after older stores to its
line, so the range and one `mfence` publish it; each draw makes several of
these flushes (descriptor and constant tables, backing stores).
