# Spike 12 P0a: Dawn cross-build for the PS5

**Dawn cross-builds with our toolchain: PASS.**
Clang 18.1.8 (`prospero-clang`) with libc++ 19 built Dawn and Tint with no
compiler workarounds. Three small patches, all about the platform, none about
the compiler. Nothing was run on a console; the PS5 result is a link.

    port/deps/build.sh dawn                   # libwebgpu_dawn.a + headers -> toolchain/wpe-sysroot
    spikes/12-webgpu/p0a-dawn/build.sh        # link smoke.cpp as a PS5 payload ELF
    spikes/12-webgpu/p0a-dawn/build.sh host   # macOS: same patched source, null backend, run smoke
                                              # (needs the step's source tree: KEEP=1)

## What is pinned

Dawn `088a8d00efd1c70b58c549d355401d115904b648` (9 Oct 2026): the commit
`chromium/8103` (M157, the stable channel on 13 Nov 2026) branched from.

The branch head (`292f8067`, seven `[M157]` backports, three of them marked
security or use-after-free) is not on GitHub: `github.com/google/dawn` carries
`main` and date tags only, and a `dawn.googlesource.com` `+archive` tarball is
generated per request, so it has no stable sha256. The branch point is on
`main`, hence on GitHub. Before shipping, the seven backports want carrying as
patches or a later `main` commit wants pinning.

Third-party trees, at the revisions in that commit's `DEPS`, as GitHub archive
tarballs (sha256 in `step_dawn`):

| Tree | Revision | Used for |
|---|---|---|
| abseil-cpp | `7f008af1` (upstream revision of Chromium's copy `af5aede1`) | compiled in |
| SPIRV-Headers | `86f980c7` | headers |
| SPIRV-Tools | `e56740d9` | configure only: no object of it is built |
| Vulkan-Headers | `c4685086` | headers |
| Vulkan-Utility-Libraries | `90738f6e` | headers |
| EGL-Registry | `db3425b8` | `KHR/khrplatform.h`, EGL headers |
| OpenGL-Registry | `6af574a1`, `xml/gl.xml` only (the archive is 112 MB) | GL loader generator |
| Jinja2 3.1.2, MarkupSafe 2.1.5 | PyPI sdists (Chromium's copies are 3.1.2 and 0.18) | code generator, host python3 |

Not needed: glslang (only for Tint's command line tools and GLSL validator),
protobuf, GLFW, googletest, Vulkan-Loader, the Vulkan memory allocator.

## Configuration

Static, one archive (`DAWN_BUILD_MONOLITHIC_LIBRARY=STATIC`). Backends: Vulkan,
OpenGL ES, null. Off: desktop GL, D3D, Metal, X11, Wayland, GLFW, samples,
tests, node bindings, protobuf, SPIR-V validation (it is a debugging aid and
would add SPIRV-Tools' optimizer). Tint: WGSL reader, SPIR-V writer, GLSL
writer, null writer (follows the null backend); no SPIR-V reader, no WGSL, HLSL
or MSL writer, no command line tools. Dawn's own defaults for the rest:
`-std=c++20 -fno-exceptions -fno-rtti`.

Two settings in the step that are not obvious:

- `-DCMAKE_CXX_FLAGS_RELEASE=-DNDEBUG`: `cmake_pkg` empties the release flags,
  and Dawn refuses to compile with neither `NDEBUG` nor `DAWN_ENABLE_ASSERTS`.
- `-DTINT_BUILD_AS_OTHER_OS=ON`: CMake calls the target UNIX, so Tint would
  pick its Linux files (`/proc/self/exe`, termios queries, child processes).
  This option selects the do-nothing `*_other.cc` ones. No patch needed.

## Patches (3, `port/deps/patches/dawn/`)

| Patch | What it does |
|---|---|
| `0001-ps5-platform.patch` | `DAWN_PLATFORM_IS(PS5)` on the POSIX path, keyed on `__PROSPERO__` (`prospero-clang` defines `__FreeBSD__ 9`, `__PROSPERO__`, `__unix__`; not `__linux__`). `GetExecutablePath`/`GetModulePath` return nothing. CMake: no `DRMUtils.cpp` (`<sys/sysmacros.h>`), no dma-buf/opaque-fd external memory and no semaphore-fd service, so those report "unsupported" as on macOS. |
| `0002-ps5-static-vulkan-lookup.patch` | Vulkan backend: no `dlopen("libvulkan.so.1")`; `vkGetInstanceProcAddr` comes from the host hook. |
| `0003-ps5-static-egl-lookup.patch` | OpenGL ES backend: no `dlopen("libEGL.so")`; EGL and GL entry points come from the host hook. |

The hooks, which the title must define (plain undefined references, not weak:
a title that leaves one out fails to link):

    extern "C" void* dawn_ps5_vk_get_instance_proc_addr(void);
    extern "C" void* dawn_ps5_egl_get_proc_address(const char* name);

- `dawn_ps5_vk_get_instance_proc_addr` returns the driver's
  `vkGetInstanceProcAddr` as a `void*` (for a Mesa ICD, its
  `vk_icdGetInstanceProcAddr`), or null for "no Vulkan". Dawn calls it once per
  Vulkan instance and then resolves `vkCreateInstance`,
  `vkEnumerateInstanceExtensionProperties`, `vkEnumerateInstanceLayerProperties`
  and `vkEnumerateInstanceVersion` through it with a null instance.
- `dawn_ps5_egl_get_proc_address` must resolve the `egl*` names as well as the
  `gl*` ones and return null for what it lacks: the same contract as
  `angle_ps5_egl_get_proc_address` in `spikes/11-webgl/src/egl_lookup.c`, whose
  table can be reused. Dawn then uses the default display
  (`eglGetDisplay(EGL_DEFAULT_DISPLAY)`).

Dawn already had a public way to do the second one per request
(`dawn::native::opengl::RequestAdapterOptionsGetGLProc`, with a `getProc` and an
`EGLDisplay`); it still takes precedence. The patch covers the requests that do
not chain it.

None of the ANGLE port's other problems came up: Dawn has no futex code, does
not name threads, and does not use `CLOCK_MONOTONIC_RAW`.

## Installed

`toolchain/wpe-sysroot/lib/libwebgpu_dawn.a` and, under `include/`:
`webgpu/webgpu.h`, `webgpu/webgpu_cpp.h` (+ `webgpu_cpp_chained_struct.h`,
`webgpu_cpp_print.h`, `webgpu_enum_class_bitmasks.h`), `dawn/webgpu.h`,
`dawn/webgpu_cpp.h`, `dawn/dawn_proc_table.h`, `dawn/native/{DawnNative,
NullBackend,OpenGLBackend,VulkanBackend,dawn_native_export}.h`,
`dawn/platform/{DawnPlatform,dawn_platform_export}.h` (installed by hand: Dawn's
rules omit them), plus `dawn/wire/client/*` and `webgpu_upstream/*`, which
Dawn's install adds and we do not use.

The archive defines the `wgpu*` C functions directly on top of dawn_native, so
there is no `dawn_proc` and no `dawn_proc.h`: nothing to call
`dawnProcSetProcs` on. `dawn::native::GetProcs()` is still there for code that
wants a table.

Dawn's exported CMake package is deleted from the sysroot: it carries link
flags abseil probed for (`-lrt`, `EXECINFO_LIBRARY-NOTFOUND`) and an absolute
path. Link the archive by path.

Consumer link line (order matters only in that the archive follows the objects
that use it):

    prospero-clang++ -nostdlib++ -Wl,--gc-sections  objects...  \
        toolchain/wpe-sysroot/lib/libwebgpu_dawn.a \
        toolchain/libcxx19/install/lib/{libc++.a,libc++abi.a,libunwind.a}

Compile flags: `-std=c++20 -nostdinc++ -isystem toolchain/libcxx19/install/include/c++/v1
-Itoolchain/wpe-sysroot/include`. No defines are needed. The consumer need not
be built with `-fno-exceptions -fno-rtti` (the smoke test is, to match).

## Sizes and time

| | |
|---|---|
| `libwebgpu_dawn.a` | 33,647,290 bytes, 714 objects |
| `smoke.elf`, `--gc-sections` (`llvm-size`) | text 6,794,442, data 170,276, bss 28,136 |
| `smoke.elf` stripped, on disk | 7,014,400 bytes |
| Build (configure + compile + install), 14 cores | 121 s; the whole step 2 min 14 s |
| Downloads | 45 MB |
| Kept trees with `KEEP=1` | 601 MB source, 142 MB build |

The smoke ELF's text includes libc++ and the SDK's static libc; all three
backends and both Tint writers are in it (the instance registers every backend,
so `--gc-sections` cannot drop one).

## Smoke test

`smoke.cpp`: a `dawn::native::Instance`, adapter enumeration at both feature
levels, a device on the null adapter, a valid WGSL compute shader through Tint
and into a compute pipeline, and an invalid one that must produce a Tint
diagnostic. On the PS5 it defines both hooks to return null.

- PS5: compiled with `-Wall -Werror` and linked. Run: the link. The ELF's 131
  undefined symbols are libc/libm/pthread imports, each checked against the
  exports of the three stub libraries it names (`libkernel_web`,
  `libSceLibcInternal`, `libSceNet`); the one left over,
  `__cxa_thread_atexit_impl`, is libc++abi's usual weak reference. No Dawn,
  Tint, abseil, `vk*`, `egl*` or `dl*` symbol is imported. Not run.
- macOS host: the same patched source tree, null backend only, Apple clang.
  Run, and it passed:

      adapter (core): backend=Null device='Null backend'
      adapter (compat): backend=Null device='Null backend'
        device error: Error while parsing WGSL: :1:24 error: cannot convert value of type 'abstract-float' to type 'u32'
        tint [bad] 1:24 cannot convert value of type 'abstract-float' to type 'u32'
      good module: 0 diagnostics, 0 errors; pipeline: 0 errors; bad module: 1 diagnostics
      PASS

  This shows the patches leave Dawn working where they do not apply. It does
  not exercise the patched code: every change is behind `DAWN_PLATFORM_IS(PS5)`
  or `PS5` in CMake, and the host build has no Vulkan or OpenGL ES backend.

## Compiler verdict

Dawn's documentation asks for Clang 19 or newer. At this commit Clang 18.1.8
with libc++ 19.1.7 compiled everything with no source change and no flag
change for the compiler's sake: 292 warnings (289 `-Wimplicit-int-conversion`),
no errors once the three platform errors were patched. No older Dawn commit was
tried, since the first one built. Not tested: libc++ 18 (the SDK's own).

## Open problems

1. Nothing here has run on a console. Unverified at run time on the PS5:
   abseil's and Dawn's use of emulated TLS, `pipe`/`poll` (Dawn's
   `SystemEvent`), `mmap` (abseil's low level allocator), and Dawn's worker
   threads.
2. The patched Vulkan and EGL paths are compiled and linked, never executed.
3. What the drivers must offer, read from the source, not tested:
   - Vulkan: 1.1 at least (`kRequiredVulkanVersion`).
   - OpenGL ES: 3.1 at least, with `GL_EXT_color_buffer_float` below 3.2; EGL
     1.4 with `EGL_EXT_create_context_robustness` and one of
     `EGL_KHR_fence_sync`/`EGL_KHR_reusable_sync`. The GL backend only answers
     requests for `FeatureLevel::Compatibility`. Whether ps5-opengl meets this
     is P0d's question.
4. The M157 backports are missing (see "What is pinned").
5. `DynamicLib` still compiles to `dlopen`/`dlsym` (the SDK's libc has them);
   with the patches no PS5 code path calls it for a driver.
6. External image import/export (dma-buf, opaque fd, sync fd) is off, so
   `Adapter::SupportsExternalImages()` is false. Sharing a Dawn texture with
   the compositor needs another route (a readback, or a PS5 external memory
   service).
7. Size was not tuned. `TINT_ENABLE_IR_DUMPING` and
   `TINT_ENABLE_IR_VALIDATION_ASSERTS` are on by default; dropping the null
   backend would drop Tint's null writer.
