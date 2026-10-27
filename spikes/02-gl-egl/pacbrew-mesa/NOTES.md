# Spike 2b: pacbrew's Mesa 26.2 (RADV + zink + EGL "videoout") as an elfldr payload

Date: 2026-10-27. Mac-only: everything below was built and read, nothing ran on a console.

| | Result |
|---|---|
| Mesa build | **Built.** libEGL, libGL, libGLESv2, libgallium, libvulkan (= RADV), 44.3 MB in total |
| `gl_egl_mesa.elf` | **Links** (151 KB), against `libEGL.so.1` + `libGL.so.1`. All 60 GL/EGL imports resolve. |
| Runs on PS5? | **Unknown.** Not tested on a console. `deploy.sh` is ready to use. |
| Time | SDK + libcxx: about 1 min. Mesa (host `mesa_clc` + target): 2 min 40 s, using `-j12` in the container |

## 1. Route

- **Container:** `Dockerfile` uses `ubuntu:24.04`, native linux/arm64 under Docker Desktop, with no emulation.
  - Ubuntu 24.04 rather than Debian: pacbrew's CI (`.github/workflows/ps5-payload-libs.yml`) runs on `ubuntu-latest`. Its packages match the CI's exactly: clang-18, lld-18, llvm-18-dev, libclang(-cpp)18-dev, libclc-18-dev, libllvmspirvlib-18-dev, glslang-tools, spirv-tools, python3-mako/yaml/packaging, makepkg 6.0.2, pacman-package-manager, and meson from pip (1.12.1; the CI asks for >= 1.4).
  - The PS5 SDK is a set of bash wrappers around the host clang, so it does not care about the host CPU.
- **Mount:** `build.sh` mounts the repo at **the same absolute path** inside the container (`-v $ROOT:$ROOT`). Every path baked into `.pc` files, meson cross files and the SDK is then valid on the Mac too.
- **Steps** (`build.sh`, run on the Mac; it re-runs itself inside the container):
  1. **SDK.** A private copy of `ref/ps5-payload-sdk` is built (`ref/` holds the Mac build's `.o` files, so it is copied and `make clean` is run first). It is installed with `make DESTDIR=toolchain/pacbrew-sysroot install`. The Mac's `toolchain/ps5-payload-sdk` is not touched.
  2. **libcxx 18.1.8.** Same flags as `pacbrew-repo/libcxx/PKGBUILD` (== `sdk/libcxx.sh`). The tarball's sha256 is checked. Only the runtimes subtrees are extracted, with tests and benchmarks off, because the full llvm-project tree is about 1.5 GB and the Mac started with 5.6 GB free.
  3. **`makepkg -d -f -C`** on private copies of the pacbrew PKGBUILDs for `openlibm zlib zstd mesa` (Mesa's `makedepends`). The sha256 sums are checked; Mesa and all 28 patches passed. Each package is then unpacked into the sysroot as `pacman -U` would do it.
     - `expat` and `vulkan-headers` are not needed: Mesa is configured with `-Dexpat=disabled` and uses its own `include/vulkan`.
  4. **Payload.** `prospero-clang gl_egl.c mesa_env.c -lEGL -lGL -lpthread`.
- **Sysroot = SDK.** `toolchain/pacbrew-sysroot/` is a complete ps5-payload-sdk install (`bin/`, `toolchain/`, `target/`), with the pacbrew packages under `target/user/homebrew` (pacbrew's `PS5_HBROOT` layout). pacbrew installs into `$PS5_PAYLOAD_SDK/target`, so the SDK and the sysroot are one tree. That is why there is no separate `toolchain/ps5-payload-sdk-linux`.
  - The tree is usable from the Mac as well: `PS5_PAYLOAD_SDK=toolchain/pacbrew-sysroot LLVM_CONFIG=/opt/homebrew/opt/llvm@18/bin/llvm-config`.
- **Scratch:** `toolchain/pacbrew-build/` (gitignored, 1 GB) holds downloads, logs and the makepkg trees. `KEEP_SRC=1` keeps the patched Mesa tree that the line references in section 3 point into. It can be deleted; `docker rmi ps5wk-pacbrew-mesa` frees the 2.3 GB image.

Run it again with `spikes/02-gl-egl/pacbrew-mesa/build.sh` (everything), or with `build.sh pkgs`, `build.sh payload`, `build.sh sdk`, `build.sh libcxx` or `build.sh pkg-<name>`.

## 2. Problems hit and fixes

1. **Host flags would leak into the PS5 compiler.** The distro `makepkg.conf` on arm64 has `CARCH=aarch64` and host CFLAGS/LDFLAGS, which would reach `prospero-clang`. On pacbrew's x86_64 CI they happen to be harmless. Fix: a generated `makepkg.conf` with `CFLAGS=-O2 -pipe`, empty LDFLAGS, `!strip !debug !lto`, and `PKGEXT=.pkg.tar.gz`.
2. **`bsdtar` exited 1 while unpacking packages** ("Can't restore time" on `Users/`). The bind mount's parent directories belong to root in the container. Fix: `bsdtar -xm --no-same-owner`.
3. **`ref/ps5-payload-sdk` contains Mac-built objects.** Building there would have changed another agent's tree. Fix: build from a copy.
4. **Disk space.** The Mac had about 5.6 GB free at the start. Fix: partial llvm-project extraction, and build trees deleted after packaging unless `KEEP_SRC=1`.
5. **Harmless noise:**
   - makepkg prints "failed to initialize alpm library / could not create database". It only affects `.BUILDINFO`, because there is no pacman database in the container.
   - Mesa prints 5,925 warnings, almost all `-Wgeneric` qualifier warnings from openlibm's tgmath `_Generic`.
6. **No Mesa source changes were needed.** The PKGBUILD's `prepare()`/`build()`/`package()` ran unmodified.

### What was built (`toolchain/pacbrew-sysroot/target/user/homebrew/lib`)

| File | Size | SONAME | DT_NEEDED |
|---|---:|---|---|
| libEGL.so.1.0.0 | 482,704 | libEGL.so.1 | libgallium-26.2.0.so, libkernel_web.sprx, libSceLibcInternal.sprx |
| libGL.so.1.2.0 (patch 0026) | 497,104 | libGL.so.1 | libgallium-26.2.0.so |
| libGLESv2.so.2.0.0 | 115,064 | libGLESv2.so.2 | libgallium-26.2.0.so |
| libgallium-26.2.0.so (zink, DRI, glapi) | 22,073,920 | libgallium-26.2.0.so | libvulkan.so.1, libkernel_web.sprx, libSceLibcInternal.sprx |
| libvulkan.so.1.4.354 (RADV, patch 0021) | 21,146,720 | libvulkan.so.1 | libSceAgc.sprx, libSceAgcDriver.sprx, libSceVideoOut.sprx, libkernel_web.sprx, libSceLibcInternal.sprx |

- zlib, zstd and libc++ are static and already linked in, so they are not deployed.
- No debug info. `llvm-strip` would save only about 2.6 MB, so the files are deployed unstripped and keep their symtab for klog.
- No `PT_TLS`: `-femulated-tls`.
- Relocations are only GLOB_DAT, RELATIVE and 64, all of which the SDK's `crt/rtld_so.c:497-517` handles.
- Every *strong* undefined symbol resolves against the SDK stubs or the other Mesa libraries. About 5,900 weak undefineds are left (the RADV layer tables: `annotate_*`, `threaded_*`, `sqtt_*`, ...); `rtld_so.c:203` ignores unresolved weak symbols.
- The configuration summary matches the PKGBUILD:
  - EGL platforms: `videoout surfaceless`; native platform: videoout.
  - DRI platform: pseudo-drm.
  - No libdrm, so `HAVE_LIBDRM` is undefined in `egl_dri2.c`'s compile line.

### `gl_egl_mesa.elf`

- **Source:** `spikes/02-gl-egl/src/gl_egl.c` is compiled **unmodified**.
  - `-DLOG_HOST="127.0.0.1"`: in a payload, stdout already returns over the elfldr socket. With loopback, the second log sink fails at once instead of timing out against a LAN address.
  - The extra file `mesa_env.c` is a constructor that sets `EGL_LOG_LEVEL=debug`, `MESA_LOG_LEVEL=info` and `MESA_SHADER_CACHE_DIR=/data/radv-shader-cache` without overwriting existing values. elfldr gives a payload no environment, and Mesa's stderr diagnostics are what we will need on the first run. `-DPS5WK_MESA_QUIET` drops the logging.
  - The SDK's crt runs the payload's `.init_array` before `main` (`crt/crt.c:126`).
- **Behaviour under zink:** desktop GL 4.6 core is created first, then ES 3. Zink exposes both, so the existing test flow applies as is.
  - `egl-image` should PASS: `eglCreateImageKHR` and `glEGLImageTargetTexture2DOES` exist, and `eglExportDMABUFImageMESA` will be NULL.
  - `export-tsharp` will SKIP.
- **Library lookup:** DT_NEEDED is `libEGL.so.1` and `libGL.so.1` (plain sonames, not `/data/...` paths as in `samples/hello_so`). The SDK's rtld (`crt/rtld.c:159-249`, `__rtld_find_file`) tries these locations in order:
  1. `/system{,_ex}/{priv,common}{,_ex}/lib`
  2. `$LD_LIBRARY_PATH`
  3. **`/user/homebrew/lib`**
  4. the cwd

  So the libraries go to `/user/homebrew/lib` under their soname.

## 3. Capabilities, read from the patches and the patched source

**Where the references point:**
- **Patch lines** are into `ref/pacbrew-repo/mesa/00NN-*.patch`.
- **"tree:"** lines are into the patched Mesa tree at `toolchain/pacbrew-build/pkgs/mesa/src/mesa-26.2.0/`, which exists only after `KEEP_SRC=1 build.sh pkgs`.

**Reading only:** these are what the code would advertise. Several values depend on runtime Vulkan feature bits.

### EGL (videoout platform, patch 0025)

**Platform**
- **Display and surfaces.** `eglGetDisplay(EGL_DEFAULT_DISPLAY)` is the videoout display (0025 L7-13; native platform videoout). There is **no `EGL_*_platform_videoout` extension or enum**: videoout is reachable only as the default/native display.
  - Window surfaces: any non-zero native window. They are kopper swapchains on VK_KHR_display at a fixed 3840x2160 (0025 L169-171, L237-240). Formats are BGRA8888/BGRX8888 (L351-355).
  - Pbuffer configs: also RGBA16F, RGB10A2 and RGB565 (L357-372). Pbuffers work as on surfaceless.
- **Driver.** `create_image = dri2_create_image_khr` (0025 L339), which makes EGLImages from GL textures and renderbuffers work. The init forces zink+kopper on a software EGL device with `fd = -1` (L403-449).
- **Client extensions** (tree: `src/egl/main/eglglobals.c:60-97`):
  - `EGL_EXT_client_extensions`, `EGL_EXT_device_base/enumeration/query`, `EGL_EXT_platform_base`, `EGL_KHR_client_get_all_proc_addresses`, `EGL_KHR_debug`
  - `EGL_EXT_platform_device`, `EGL_EXT_explicit_device`, `EGL_MESA_platform_surfaceless`
  - No x11/wayland/gbm.
- **Surfaceless platform.** `EGL_PLATFORM_SURFACELESS_MESA` (what WPE uses for headless) has no DRM to probe and no swrast. `eglapi.c:697-712` then retries with `Options.Zink`, so it should come up on zink too. This is untested; `MESA_LOADER_DRIVER_OVERRIDE=zink` forces it.

**Display extensions** (tree: `src/egl/drivers/dri2/egl_dri2.c:660-740`, unchanged by the patches; the line numbers are upstream's)

| Extension | Status | Why (reference) |
|---|---|---|
| EGL_KHR_image_base, KHR_gl_texture_2D_image, KHR_gl_texture_cubemap_image, KHR_gl_renderbuffer_image | **yes** | Unconditional (L726-729) |
| EGL_KHR_gl_texture_3D_image | yes | If max 3D levels != 0 (L731) |
| EGL_KHR_surfaceless_context, KHR_no_config_context, KHR_create_context(_no_error) | **yes** | L660-663 |
| EGL_KHR_fence_sync, KHR_wait_sync, KHR_reusable_sync, KHR_cl_event2 | **yes** | L703-713 |
| EGL_ANDROID_native_fence_sync | **no** | Needs `__DRI_FENCE_CAP_NATIVE_FD` (L706). zink sets it only with `VK_KHR_external_semaphore_fd` (tree: `zink_screen.c:788-790`), and the PS5 RADV does not report that (below). |
| EGL_MESA_image_dma_buf_export, EXT_image_dma_buf_import(_modifiers) | **no** | Inside `#ifdef HAVE_LIBDRM` (L715-722), and there is no libdrm. Even with libdrm, zink's dmabuf cap needs `KHR_external_memory_fd` and `EXT_external_memory_dma_buf` (tree: `zink_screen.c:1159-1165`). |
| EGL_ANDROID_blob_cache | yes | zink has a disk cache (L710) |
| EGL_MESA_gl_interop, MESA_query_driver, EXT_pixel_format_float, EXT_config_select_group, KHR_context_flush_control | yes | Unconditional |
| EGL_KHR_gl_colorspace | likely | If BGRA8 sRGB is renderable (L686-690) |
| EGL_IMG_context_priority | low/medium | RADV PS5 `global_priority_mask` = LOW and MEDIUM (tree: `radv_ps5_winsys.c:437`) |
| EGL_EXT_create_context_robustness, KHR_partial_update | runtime | zink caps |

### GL / GLES version through zink on the PS5's GFX1013

- **Desktop GL.** zink's GLSL level is fixed at 460 (tree: `zink_screen.c:987-988`). The GL/ES version Mesa computes depends on the extensions zink derives from Vulkan features.
- **Features the PS5 RADV reports** (tree: `radv_physical_device.c`):
  - `geometryShader`, `tessellationShader` (L1058-1059). These are implemented as compute: 0004, 0010.
  - `EXT_transform_feedback` (L995)
  - `KHR_dynamic_rendering_local_read` (L769). This gives zink's fbfetch, and with it `KHR_blend_equation_advanced`.
  - `EXT_line_rasterization`, `EXT_provoking_vertex`, `EXT_non_seamless_cube_map`, `KHR/EXT_robustness2`, `EXT_custom_border_color`, `EXT_extended_dynamic_state{,2,3}`, `EXT_vertex_input_dynamic_state`, `EXT_graphics_pipeline_library`, `EXT_shader_object`
  - Vulkan API version 1.4 (tree: `radv_instance.h:22`; `radv_physical_device.c:1861`)
- **ES 3.2** (tree: `src/mesa/main/version.c:530-552`) also needs:
  - `KHR_texture_compression_astc_ldr`. The GPU has no ASTC and RADV's ASTC emulation is off without drirc, but Mesa's state tracker CPU-decodes ASTC to RGBA8 because `allow_compressed_fallback` defaults to **true** (tree: `driinfo_gallium.h:40`, `st_extensions.c:979-983, 1195`).
  - `KHR_blend_equation_advanced`, through fbfetch above.
- **What the patches turn off does not affect GL/ES versions** (0003 L631-694, 0002 L505-516):
  - barycentrics, VRS, ray tracing, perf queries, capture/replay
  - device-generated commands, accelerated integer dot product
  - flat scratch, CS wave id and NGG per-primitive parameters (compiler internals)
- **Expectation:** `4.6 (Core Profile) Mesa 26.2.0` and `OpenGL ES 3.2 Mesa 26.2.0`, renderer `zink Vulkan 1.4(PlayStation 5 GPU (RADV ...))`. This is the same as upstream zink on RADV Navi1x. The risk is correctness, not the advertised version: GS and tessellation run as compute, and 0004 calls part of it "WIP".

### Vulkan: external memory and sharing across processes

- **`has_external_fd` is false on PS5.** The PS5 winsys sets `info->has_external_fd = false` ("GPU memory and the queue's syncs have no file descriptor to share"; 0001 L2966-2968; tree: `radv_ps5_winsys.c:439`). It is never set true by a later patch.
- **What that gates off** (0001 L778-802, L662-692; tree: `radv_physical_device.c:772-776, 918, 932`):
  - `VK_KHR_external_memory_fd`, `KHR_external_semaphore_fd`, `KHR_external_fence_fd`
  - `EXT_external_memory_dma_buf`, `EXT_image_drm_format_modifier`
  - the OPAQUE_FD/DMA_BUF entries in `GetPhysicalDeviceExternal{Image,Buffer}Properties`
  - **No fd type at all**, neither opaque nor sync_fd nor dma-buf. The winsys's sync type has only binary/CPU features, no import/export, and timelines are emulated (tree: `radv_ps5_sync.c:171-173`).
- **Kept:**
  - `KHR_external_memory` / `external_semaphore` / `external_fence` (the base extensions without handle types)
  - **`VK_EXT_external_memory_host`**: `has_userptr = true` (0009 L163; host-pointer import, 0009 L160-180)
  - `EXT_queue_family_foreign`
  - zink turns `external_memory_host` into `resource_from_user_memory` (tree: `zink_screen.c:791`), which gives `GL_AMD_pinned_memory`.
- **Consequence for WebKit:**
  - **Within one process** (one EGLDisplay): shared contexts and EGLImages from GL textures plus `KHR_fence_sync`/`wait_sync` work. That is the realistic texture-sharing path (WebGL to compositor to WebXR).
  - **Across processes** (WebProcess/GPUProcess) there is **no** fd mechanism: no dma-buf, no opaque fd, no sync_fd. The only cross-process route this driver allows is CPU-visible shared memory imported with `external_memory_host` / `AMD_pinned_memory` on each side. On the PS5's unified memory that is a GPU copy, not zero-copy of tiled images. The PS5-specific options are a single GPU process, or new winsys code that exports AGC memory.
- **Other relevant Vulkan facts:**
  - One queue family on the graphics ring plus a compute/transfer family on the same ring (0008)
  - Sparse binding (0005)
  - `conformanceVersion` 0.0.0.0 (0005 L497)
  - Instance extensions: `VK_KHR_display` on VideoOut, `VK_EXT_headless_surface` (0011 L77-86; tree: `radv_instance.c:242, 251`)
  - `libvulkan.so.1` *is* the driver and exports `vk[A-Z]*` and `vk_icd*` (0021 L146; core 1.0-1.4 plus surface, swapchain, display and display_swapchain, L199-204). There is no loader and there are no layers.

## 4. Could ANGLE's Vulkan backend run on this RADV? (reading only)

- **Minimum version: met.** ANGLE needs Vulkan 1.1 (`ref/WebKit/Source/ThirdParty/ANGLE/src/common/vulkan/vulkan_icd.h:26`; checked at `vk_renderer.cpp:2603, 2735`). RADV reports 1.4.
- **Loading: compatible by design.** ANGLE loads `libvulkan.so.1` and resolves through `vkGetInstanceProcAddr`, which 0021 provides.
- **Display backends exist for both available surfaces:**
  - `linux/display/DisplayVkSimple.cpp:51` (VK_KHR_display)
  - `linux/headless/DisplayVkHeadless.cpp:81` (VK_EXT_headless_surface)
  - `linux/DisplayVkOffscreen.cpp`
- **Feature needs look covered.** ANGLE's ES 3.0/3.1 needs are core 1.1 features RADV reports: independentBlend, robustBufferAccess, compute, `EXT_transform_feedback` (with a vertex-store emulation fallback), subgroup basic. The extensions ANGLE prefers are present too: GPL / pipeline library, dynamic state 1/2/3, vertex-input dynamic state, custom border color, provoking vertex, depth clip control, primitive topology list restart, primitives generated query, shader stencil export, memory budget, host image copy, swapchain_maintenance1, incremental present (tree lines in section 3).
  - Missing ones are optional in ANGLE: fragment shading rate, barycentrics, and all external fd extensions. They are pushed only when supported (`vk_renderer.cpp:3983-4000`).
  - ANGLE's EGL dma-buf import is gated on `supportsExternalMemoryDmaBuf` (`DisplayVk.cpp:560`), so it would be off.
- **Verdict:** ANGLE-on-Vulkan should initialize and could reach ES 3.0 (WebGL 2), probably ES 3.1. This is untested.
- **The blocker is interop, not ANGLE:**
  - ANGLE-Vulkan has its own VkDevice. Handing its WebGL output to a zink/GLES compositor (a second VkDevice), or to another process, needs external memory fds, which this RADV does not have.
  - The workarounds are a host-memory copy through `EXT_external_memory_host` / `AMD_pinned_memory`, or making ANGLE and the compositor share one device (not supported by stock ANGLE + zink).
- **Simpler and lower risk for WebKit:** ANGLE on its GL/GLES backend over this Mesa EGL (zink), in-process with the compositor, sharing through EGLImage/shared contexts. That costs one more translation layer (GLES to zink to Vulkan). It is still better than ps5-opengl's position (title-only, no GLES).

## 5. Deploying

**Prerequisites on the console:** elfldr on 9021 and ftpsrv on 2121.

```sh
PS5_HOST=192.168.x.y spikes/02-gl-egl/pacbrew-mesa/deploy.sh               # upload + run
PS5_HOST=... deploy.sh --upload-only | --send-only | --dry-run
```

**Uploads** (42.3 MiB; `curl -T`, uploaded under each soname because FTP has no symlinks):

```
curl -sS --ftp-create-dirs -T .../libEGL.so.1.0.0        ftp://$PS5_HOST:2121/user/homebrew/lib/libEGL.so.1
curl -sS --ftp-create-dirs -T .../libGL.so.1.2.0         ftp://$PS5_HOST:2121/user/homebrew/lib/libGL.so.1
curl -sS --ftp-create-dirs -T .../libGLESv2.so.2.0.0     ftp://$PS5_HOST:2121/user/homebrew/lib/libGLESv2.so.2   # not needed by this payload
curl -sS --ftp-create-dirs -T .../libgallium-26.2.0.so   ftp://$PS5_HOST:2121/user/homebrew/lib/libgallium-26.2.0.so
curl -sS --ftp-create-dirs -T .../libvulkan.so.1.4.354   ftp://$PS5_HOST:2121/user/homebrew/lib/libvulkan.so.1
curl -sS -o /dev/null -Q "*MKD /data/radv-shader-cache" ftp://$PS5_HOST:2121/
tools/send.sh spikes/02-gl-egl/pacbrew-mesa/gl_egl_mesa.elf    # socat to :9021; log saved under logs/
```

- **`/user/homebrew/lib` is pacbrew's shared prefix.** If something else installs a different `libEGL.so.1` or `libvulkan.so.1` there, it collides. `$LD_LIBRARY_PATH` cannot help, because elfldr passes no environment and rtld resolves DT_NEEDED before any constructor runs. The workaround is relinking with absolute sonames.
- **Diagnosing failures:** look at the elfldr socket output (EGL debug log, `wsi/videoout:` and `radv/ps5` stderr lines from 0019) and at klog (`klogsrv`) for rtld errors such as "unable to load".

## 6. Risks and open questions

- **Provenance.** The PKGBUILD says it plainly (L11-14): "These patches are AI slop generated by claude.ai. It seems to work, but comments in the code are not to be trusted …". Patches 0017-0028 are `Co-Authored-By: Claude Opus 5.5`. 0001-0016 are squashes from mihawk-99/PS5_Mesa, dated 2026-10-07/08. The checked-out repo's HEAD is from 2026-10-11.
- **Test claims cannot be verified here.** The comments cite on-device evidence 46 times ("RADV smoke title", dEQP-VK case names, PS5_Vulkan / ps5vk measurements). That suggests the upstream fork was run on hardware, but the pacbrew repo has no test logs. The repo's CI (`ps5-payload-libs.yml`) only **compiles** every package on tag push or manual dispatch and uploads a tarball. Nothing is executed and nothing touches a console. Our build proves the same thing and no more: it compiles and links.
- **Payload versus title.** Patch 0028 is the only payload-specific change: the shader cache goes to `/data` when `/app0` is not writable. Its message says payloads work, but nothing else in the stack was adapted for them. Open questions:
  - Do AGC (`sceAgcInit`, direct memory, the submission ring) and `sceVideoOutOpen(0xff /*system user*/, 0, 0)` (0011 L317, L549) work from elfldr's process?
  - Does VideoOut fight ShellUI for the screen? Pbuffer-only use (this spike) never opens VideoOut, unless zink/kopper touches the display at screen creation.
  - 0019's stderr messages will say why if the device cannot be set up.
- **Loader.** The SDK rtld relocates by symbol-name lookup, one lookup for each of about 40k relocations in 44 MB of libraries. Expect a noticeable load time. The code segments are mapped RWE (the SDK's linker script).
- **Thread-local dispatch.** Patch 0023 replaces TLS GL dispatch with `_mesa_glapi_get_dispatch()` calls on the PS5 (0023 L186) after a `PRX_NOT_RESOLVED_FUNCTION`. That is a small per-call cost, and it is evidence that a GL call through libGLESv2 once failed on a device.
- **Emulation.** Geometry shaders, tessellation and transform feedback run as compute (0004, 0007, 0010); 0004 marks part of it WIP. WebGL 2 does not need GS/tess, but zink uses GS internally for some primitive emulation (quads, line stipple, and so on).
- **No cross-process GPU sharing** (section 3). This is the main architectural constraint for WebKit's multi-process model on this stack.
- **Pinned versions.** Mesa is pinned at 26.2.0 plus 28 patches (sha256-pinned). Any pacbrew bump changes `radv-build-id` and the shader-cache key, which is harmless.
