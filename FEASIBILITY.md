# Feasibility: WebKit + WebXR on a jailbroken PS5 with PS VR2

_Status: **console phase under way: the key gate (Spike 3) passed on
2026-10-27; title ↔ payload IPC (Spike 8) passed the same day.**_

Everything below marked **verified** was built or checked on the Mac.
Everything marked **pending** needs a run on the console, and the spike that
answers it is named. Sources are community GitHub projects from
Sept–Oct 2026. Several are days old and partly AI-written, so treat their
claims as unverified until a spike confirms them.

## 1. Verdict so far

**Go, with a revised architecture.** The key gate (GL → PS VR2, Spike 3)
passed zero-copy at 60 fps; JavaScriptCore runs (Spike 4); multi-process
works between payloads (Spike 1) and between the title and payloads it
launches (Spike 8); GLES 3.2 works (Spike 2). The measured payload/title
split (§4.4b) points to a GPU-process design (§4.4c). Earlier
host-side findings:

- **There's now a second GPU route.** pacbrew's Mesa port (RADV Vulkan +
  zink + a Mesa EGL "videoout" platform, GLES built in) claims to work in
  elfldr payloads. That would fix ps5-opengl's three gaps for WebKit: no
  GLES, no EGLImage, and title-only. It's unvalidated on hardware (§4.2).
- **The PS VR2 hand-off is a texture descriptor, not a graphics API.**
  Hmd2 takes a GFX10 T# over GPU memory. ps5-opengl's driver already builds
  T#s internally, so a zero-copy GL → headset path is a small driver patch,
  not an AGC rewrite (§4.3).
- **Sony's JIT API is reachable.** `sceKernelJitCreateSharedMemory` and
  friends (what the system browser uses) are exported to payloads. JIT may
  be possible later, not just the LLInt interpreter (§4.4).

The decisive unknowns are all console runs: **Spike 3** (does GL-rendered
memory reach the headset), **Spike 1** (multi-process from a title, and
cross-process direct memory), and **Spike 2** (which GPU stack works in
which process).

## 2. Engine choice: WebKit, not Chromium

Decided: WebKit (WPE port). Chromium is reconsidered only if Spike 1 shows
no multi-process model at all.

| | WebKit (WPE) | Chromium |
| --- | --- | --- |
| Port cost | CMake (`prospero-cmake` exists), FreeBSD support and `OS(PLAYSTATION)` paths upstream. Sony ships WebKit on PS4/PS5 | GN + its own toolchain, new `target_os`. The FreeBSD port alone carries 1000+ patches |
| WebXR backend | OpenXR on EGL/GLES, Unix-like, so close to our case | OpenXR on Windows (D3D11) and Android only, as far as known |
| WebGL | ANGLE | ANGLE (same) |
| No multi-process | no supported single-process mode, so a large patch | `--single-process` / `--in-process-gpu` exist |
| JS without JIT | JSC LLInt/CLoop (Spike 4) | V8 `--jitless` (mature) |

## 3. Spike results

| Spike | Host phase | Console phase | Gate |
| --- | --- | --- | --- |
| 0 baseline | **verified:** SDK + libc++ built (LLVM 18.1.8), hello_world + `sysinfo.elf` build | **PASS** (2026-10-27): FW 13.60 (kernel 0x13600007), payload authid 0x480000001000000e, direct 12.4 GB / flexible 8.1 GB, 16 threads, Hmd2/VrTracker2/VrHand + hand models present | **go** |
| 1 procs/IPC | **verified:** builds; split into groups `safe`/`spawn`/`jit`/`fork`/`title` | **Payload:** threads, `fork`, **fd passing (SCM_RIGHTS)**, named shm, Unix socket in `/system_tmp` (not `/data`, `/user`), 6 GB anonymous memory, **all 3 JIT probes** PASS; **kernel panic** after a forked child with shared mappings exited; direct-memory budget only ~55 MB. **Title (unjailed):** threads, named shm PASS; **no directory accepts an AF_UNIX socket** (`/system_tmp` EACCES); **flexible memory 251 MB**, direct ≥2 GB; JIT: RWX FAIL, **RW→RX `mprotect` PASS**, `sceKernelJit*` FAIL (0x80020001). **`spawn` (payload): PASS:** child started through the local elfldr in 375 ms, rendezvous over `/system_tmp`, shm + pipe fds passed both ways, child has 8 GB flexible memory. **`dmem-share` FAIL** (0x8002000d, EACCES): no cross-process direct-memory mapping by API | **go** (payload↔payload; title↔payload: Spike 8) |
| 2 GL/EGL | **verified:** ps5-opengl from source + patches (GLES 3.2, T# export); pacbrew Mesa built | **PASS (title, ps5-opengl+patches):** EGL "OpenGL OpenGL_ES"; GL 4.6 core, 199 ext; surfaceless; FBO render; **GLES 3.2 context + `#version 300 es` rendering**; shared context on a 2nd thread (needs a big stack); T# export (SW_64KB_R_X). **FAIL:** EGLImage (expected); WebGL-like load **132.7 ms/frame for 2000 small draws** (8 fps). **2b:** the cost is CPU time in the driver; with patch 0003 (strips batched), profiling off and async preparation it is **8 µs per draw, 17 ms for 2000** (§4.2c). pacbrew Mesa not run yet | **go** (perf risk reduced) |
| 3 GL → PS VR2 | **verified:** `psvr2` backend, `hmd2_probe.elf` (3a), `gl_hmd2` title (3b) | **3a (payload): FAIL by design.** Libraries load, 13.60 offset tables match, Hmd2 initialises and reports FOV, but `sceHmd2Open` refuses every user id from a non-application process (0x8a720009). **3b (title): PASS.** Zero-copy: GL 4.6 renders into ps5-opengl textures (SW_64KB_R_X) exported as T#s; 3600 frames at 60.0 fps (120 Hz out), render 1.13 ms + hand-off 0.06 ms, 0 submit errors, 3600/3600 tracked, tracker eye positions valid. Visual check: world-locked, correct stereo depth, no judder noticed; floor grid right in front looked slightly off (likely aliasing of 1-px lines and an assumed floor height, not the pipeline) | **go** |
| 4 JSC | **verified:** `jsc` links (JSCOnly, JIT off, asm LLInt, bmalloc/libpas, ICU 76 static; 36 MB slim). 6 patches + `memmem` shim; needs libc++ 19 | **PASS** (payload): correctness **30/30** (regex, typed arrays, BigInt, generators, stack-overflow RangeError, ICU Intl, GC churn); bench **3.41 s** vs 1.04–1.10 s on M4 Max (same build): richards 596, deltablue 592, numeric 890, strings 424, alloc 906 ms. Fixes needed on device: `-DLD_PACK_RELATIVE_RELOCS=OFF` (SDK loader ignores DT_RELR), libpas commit/decommit on 16 KiB pages (patch 0006). Open: `Date` reads 1970 (wall clock source) | **go** |
| 8 title ↔ payload IPC | **verified:** builds as title PPSA99009 with the server payload embedded | **PASS** ([README](spikes/08-title-ipc/README.md)): **the title spawns the payload through the local elfldr** (listening after 1.6–5.2 s); title → payload unix `connect` **EPERM**; **reversed works: the title listens in a 0777 directory the payload made (`/system_tmp/ps5wk`), the payload connects, shm fds pass both ways with SCM_RIGHTS**; named shm both ways; loopback TCP. RTT 131 µs (unix) / 186–249 µs (tcp); in-process baseline 18 µs in the title vs 73 µs in a payload; 112 MB/s (unix), 350 MB/s (tcp). `sceShellCoreUtilExitApp` returns 0 and the title closes **with no debug error** (user-checked) | **go** |
| 5 OpenXR runtime | **verified:** `libopenxr_psvr2` covers all ~50 calls WebKit makes (+ the rest of 1.0 core); host smoke test replaying WebKit's sequence **18/18 PASS** via dlopen, static link and the official Khronos loader (1.1.65); PS5 backend cross-compiles (`libopenxr_psvr2.a`) | pending (needs Spike 3b first) | — |
| 6 MiniBrowser | design ([README](spikes/06-wpe-minibrowser/README.md)) | — | — |
| 7 WebXR e2e | design ([README](spikes/07-webxr-e2e/README.md)) | — | — |

Console logs for every run above are in
[`evidence/2026-10-27/`](evidence/2026-10-27/): `00-sysinfo`, `01-*` (incl.
the run that panicked), `03a-hmd2-probe-payload`, `titles-02-03b` (Spikes 2
and 3b over TCP), `04-jsc-*` and `08-title-ipc`.

### What it took to get Spike 3b running (worth knowing for the port)

1. **Title `param.json` must declare PS VR2.** With `attribute`/`attribute3`
   = 0, `sceHmd2Initialize` returns 0x8a72001f. With PS5VR's values
   (0x62000000 / 0x04080c40) it works. The registered copy under
   `/user/app/<TITLE>/sce_sys/` must be updated too.
2. **Link `libkernel` first.** Built on macOS, the glob that orders the SDK
   stub libraries put `libkernel_web` first; `stat()` then bound to it and the
   console's loader left the import NULL (SIGSEGV at address 0 in `main`).
   Fixed in `package-title.sh`.
3. **Titles unjail through etaHEN** (`/download0/etahen_jailbreak` drop file)
   to reach `/data`; after that TCP logging to the Mac also works.
4. **Hmd2 needs an application process.** Payloads can load and initialise
   it but can't open the headset, so the XR side of the browser is a title.
5. A title's direct-memory needs for VR: tracker 32 MB + reprojection
   ~97 MB + eye buffers. A payload's direct budget is ~55 MB.
6. **Threads that compile shaders need large stacks.** Mesa's
   `nir_opt_varyings` overflowed the PS5's default pthread stack on a second
   GL thread (Spike 2 `shared-thread`, SIGSEGV just below the stack). WebKit's
   compositor/WebGL threads must be created with multi-MB stacks on PS5.
7. The kernel log (`klogsrv`, port 3232) is the reliable crash channel:
   it showed the NULL-import crash with registers.

## 4. Findings

### 4.1 ps5-opengl (Mesa 26.2 + a custom `ps5` Gallium driver on AGC)

- **Desktop GL only.** The EGL facade (`src/egl/ps5_egl.c`) rejects
  `EGL_OPENGL_ES_API` (`eglBindAPI`), reports `EGL_CLIENT_APIS "OpenGL"`,
  and Mesa is configured with `-Dgles2=disabled`. The state tracker can make
  ES contexts, so enabling GLES is a build flag plus a facade patch (Spike 2
  agent). The fallback is ANGLE's GL backend.
- **No EGLImage, no DMA-buf, no fences as fds.** It does have shared
  contexts, pbuffers and `EGL_KHR_surfaceless_context`.
- **Title-only:** "launch it as a registered title; do not send it to an
  ELF loader." Whether a WebKit child process could use GL at all is
  unknown.
- **Supply risk:** the upstream (blackbearreloaded/ps5-opengl) and its
  native-app boilerplate were **deleted from GitHub** during this study. We
  build from the `mihawk-99` fork. The only prebuilt SDK left is from an
  unknown 0-star account, so it's not used.
- **Good news for Spike 3:** textures are direct memory (`struct
  ps5_resource { data, stride, level_offset, direct_start … }`), and the
  driver builds GFX10 image descriptors itself. A T# export entry point is a
  small patch.
- It passes the GL 4.6 CTS on firmware 6.02 per its own report; this hasn't
  been checked on newer firmware.

### 4.2 pacbrew Mesa (ps5-payload-dev/pacbrew-repo, Oct 2026)

- RADV with a PS5 winsys on AGC (no DRM), zink, an **EGL "videoout"
  platform** (pbuffers like surfaceless, window surfaces on
  `VK_KHR_display` over VideoOut), `-Dgles2=enabled`, and shared libraries.
  Patch 0028: "a payload … keeps [the shader cache] in /data", so it's
  **designed to run in payloads**.
- It would give WebKit what it expects: Mesa EGL with EGLImage, GLES 3.x,
  and a Vulkan device (ANGLE's best-supported backend, and WebGPU later).
- **Risk:** 28 patches written over a few days (many co-authored by an AI
  assistant), with no on-device validation recorded in the repo. Spike 2
  route B tests it with the same `gl_egl` program.
- _Build and capability findings: pending (agent)._

### 4.2b Spike 2 builds: what they settled

- **ps5-opengl + patches** (`spikes/02-gl-egl/ps5-opengl/`): GLES 3.2 / GLSL
  ES 3.20, 144 extensions (checked with a host harness that runs the real
  driver caps code). No `OES_texture_float_linear`: deliberately left off to
  keep GL behaviour as validated. The texture export returns the driver's own
  T# after a full GPU wait. Still no EGLImage (Mesa advertises
  `GL_OES_EGL_image`, but it fails) and no EGL fences, which ANGLE's EGL
  backend may want. Fixed 256 MB heap and 256 MB render arena: WebKit will
  need more.
- **pacbrew Mesa** (`spikes/02-gl-egl/pacbrew-mesa/`): EGL with
  `KHR_image_base`, `gl_texture_2D_image`, `fence_sync`/`wait_sync`,
  surfaceless; expected GL 4.6 + ES 3.2 via zink. **No external memory fds of
  any kind** (`has_external_fd` false): GPU sharing works only within one
  process. ANGLE-on-Vulkan would probably initialise (Vulkan 1.4), but its
  output couldn't be shared with a separate compositor, so ANGLE-on-GL(ES)
  over the same EGL, in one process, is the simpler path. The PKGBUILD itself
  calls the patches unreviewed "AI slop"; the repo has no on-device logs.
- **Consequence:** neither stack shares GPU memory across processes through
  its API. Cross-process zero-copy rests on Spike 1 `dmem-share`; otherwise
  WebGL, compositing and XR live in one process (Spike 7 options B/C).
- **Payload direct-memory budget is ~55 MB** (single allocation 54 MB,
  measured), against 12.4 GB for the system. Headset work memory plus eye
  buffers is a few tens of MB, so GPU-heavy processes (WebGL, XR) almost
  certainly have to be titles or get a raised budget (R3).

### 4.2c Draw-call cost (Spike 2b): measured, found, fixed in part

Spike 2's `webgl-load` ran 2000 small draws in 132 ms. `gl_draw_bench`
(title, `build-bench.sh`) splits that cost; logs in
[`evidence/2026-10-27/02b-draw-bench.log`](evidence/2026-10-27/02b-draw-bench.log).

| 2000 draws (strip + 2 `glUniform4f`), 1080p target | per draw | frame |
| --- | --- | --- |
| stock ps5-opengl | 72 µs | 144 ms |
| + patch 0003 (strips, lines, points batched) | 26.6 µs | 53.6 ms |
| + profiling compiled out (`gles-noprof`) | 19.7 µs | 39.7 ms |
| + `PS5_ASYNC_NATIVE_PREP=1` (`gles-async`) | **8.1 µs** | **17.2 ms** |

- **All of it is CPU time in the driver.** Issue time equals thread CPU time,
  `glFinish` waits ~0–1 ms, and the cost does not change with the size of
  the target. A single instanced draw of all 2000 quads takes 0.1 ms. The GPU
  is not the limit. `glUniform*` is cheap (0.2 µs).
- **Cause 1 (fixed by patch 0003):** the driver only batched
  `GL_TRIANGLES`. Every strip draw allocated GPU memory, submitted alone and
  spin-waited for the GPU (`ps5_screen.c` `ps5_multidraw_eligible`). Each
  batched draw carries its own command stream and primitive type, so strips,
  lines and points batch safely. Fans still take the synchronous path.
- **Cause 2:** every draw re-encodes all state and builds a fresh command
  buffer (no dirty tracking). The async build hands that to two pinned worker
  threads, which spin while waiting, so it costs two CPU cores. The real fix
  is state caching in the driver: port work, not a spike.
- **Cause 3:** per-draw profiling (rdtsc scopes, atomics, printf bursts) is
  on by default in ps5-opengl's build (`PS5_DRAW_PROFILE=1`): ~7 µs per draw.
- `glBufferSubData` on a UBO in flight costs 30 µs per draw in every build,
  because it waits for retire. Per-draw uniforms belong in `glUniform*` or
  `glBindBufferRange` (8 µs).
- **glthread works** once `PS5_GLTHREAD=1` is set before `eglInitialize`
  (the screen reads it there, and each context when created). The calling
  thread then spends 0.1 ms per frame on 2000 draws, and the driver work
  (the same 17–54 ms) moves to Mesa's own thread. Throughput does not change.
  For WebKit's GPU process it frees the thread that decodes IPC.
- What this means for WebXR: at 8 µs, 250 draws take 2.4 ms and 1000 draws
  8.8 ms. Three.js renders each eye separately, so a 150-object scene costs
  about 2.5 ms per frame. That fits the 60 fps (reprojected to 120 Hz) budget
  Spike 3b ran at. Heavy scenes stay CPU-bound until the driver caches state.

### 4.3 The PS VR2 path (from PS5VR's source)

- Hmd2 reprojection at 120 Hz with half-rate rendering (60 fps). A type 0x10
  layer per frame: one side-by-side eye buffer used for both eyes, per-eye
  FOV tangents, the device pose and the predicted display time.
  `sceVideoOutSubmitFlip` on **VideoOut bus 32**.
- Buffers are registered as **32-byte GFX10 T#s** (RGBA8/RGB10A2,
  SW_64KB_R_X). The compositor samples memory owned by the **registering
  process**. That constraint drives the WebXR process design (Spike 7).
- `libSceHmd2`/`libSceVrTracker2`/`libSceVrHand` hide some functions from
  dlsym. They're reached at **per-firmware offsets**, guarded by anchor
  strings. Only 13.60 has a table in source (a 12.70 binary release
  exists). Other firmware needs tables generated from that console's own
  decrypted libraries. **The test console runs 13.60, so the shipped table
  applies.** It's also the last firmware Relapse supports: don't update it.
- Sense controllers: `sceVrTracker2RegisterDevice(1|2, pad handle)` +
  `GetResult` (PS5VR). Hands: `libSceVrHand` returns 26 OpenXR-ordered
  joints, but needs elevated credentials for the headset cameras (PS5VR's
  helper payload). This maps directly onto `XR_EXT_hand_tracking`.
- The original PS5VR repo is gone too. We work from the `kavera001-dev`
  fork.

### 4.4 JIT

The SDK's libkernel stubs export `sceKernelJitCreateSharedMemory`,
`sceKernelJitCreateAliasOfSharedMemory` and `sceKernelJitMapSharedMemory`
(also in `libkernel_web`, the system browser's variant). Spike 1 probes them
together with RWX and RW→RX `mprotect`. If `jit-sce` passes, JSC's JIT
needs an `ExecutableAllocator` backend with separate W and X mappings.
Upstream WebKit already supports that model for other platforms. Until
then: LLInt, no JIT (Spike 4).

### 4.4b Payload vs title: measured on 13.60

| | elfldr payload | registered title (unjailed via etaHEN) |
| --- | --- | --- |
| Headset (Hmd2 open) | no: "invalid userId" | **yes** (with VR attributes in `param.json`) |
| ps5-opengl | no (title-only by design) | **yes** |
| Flexible (CPU) memory | ~8 GB | **~251 MB** |
| Direct (GPU) memory | **~55 MB** | ≥2 GB |
| AF_UNIX socket at a path | `/system_tmp` | **bind only in a 0777 directory a payload created** (`/system_tmp/ps5wk`); `connect` to a payload's socket EPERM |
| Launch a payload | via elfldr | **via the local elfldr** (127.0.0.1:9021), 1.6–5.2 s |
| Socket RTT within the process | 73 µs | 18 µs |
| JIT | RWX, RW→RX, `sceKernelJit*` all work | **RW→RX `mprotect` only** |

Consequences: the browser's UI/XR/GPU side must be a title, and its large
heaps (JS, DOM) have to live in direct memory (a libpas/bmalloc backend over
`sceKernelAllocateDirectMemory`, as Sony's own port does), or in payload
child processes with their 8 GB of flexible memory. JSC's JIT would need a
mprotect-flip executable allocator in a title. IPC rendezvous is reversed
(Spike 8): the title listens, the payloads connect.

### 4.4c The process architecture this points to

Put together, the measurements suggest WebKit's **GPU-process model**, split
along the PS5's payload/title line:

- **Title = UIProcess + GPU process + XR.** It owns the headset, ps5-opengl,
  the compositor and all WebGL rendering (WebKit's remote WebGL in the GPU
  process), with ≥2 GB of direct memory. Eye buffers and page buffers never
  leave this process, which fits both "Hmd2 samples memory of the registering
  process" and "no cross-process direct memory".
- **Payloads = WebProcess(es) + NetworkProcess**, spawned through elfldr: 8 GB
  of flexible memory for JS/DOM heaps, and the full JIT toolbox (RWX,
  `sceKernelJit*`) for JSC. WebGL calls are serialised to the GPU process over
  IPC, as on Apple platforms.
- **Title ↔ payload IPC: proven (Spike 8).** The title spawns payloads via
  the local elfldr. It listens on a unix socket in a 0777 directory that a
  payload created, and the payloads connect (the other direction is EPERM).
  From then on SCM_RIGHTS and shared memory work as WebKit expects. So a PS5
  ProcessLauncher needs a reversed rendezvous, but `IPC::Connection` itself
  can stay unchanged.
- **Still to prove:** WPE's GPU-process WebGL path must be enabled and work
  (upstream status to check).

### 4.5 Process model

- elfldr starts payloads by `sceKernelSpawn`ing `SceSpZeroConf` and
  injecting the ELF over ptrace with kernel R/W. Payload stdout goes back to
  the sender.
- WebKit's launchers hand the child one end of a socketpair. Sony's own port
  does this through a private `PlayStation::launchProcess`, and libwpe has
  a pluggable process provider. A PS5 provider = spawn via elfldr + pass the
  fd over a rendezvous socket (exactly Spike 1's `elfldr-spawn`).
- WPE can show pages without DMA-buf: `AcceleratedSurface::RenderTargetSHMImage`
  reads GL frames back into shared memory. That's the first-light path for
  Spike 6.

### 4.6 WebKit's WebXR expectations (read from source)

UIProcess OpenXR backend: `XR_KHR_opengl_es_enable` +
`XR_MNDX_egl_enable` (`XrGraphicsBindingEGLMNDX`), GLES swapchains
(`GL_RGBA8` preferred), DMA-buf export of swapchain images to the
WebProcess, and an EGL fence fd back. About 50 `xr*` entry points (listed
in the Spike 5 notes). Optional extensions: hand tracking, local floor,
composition layers, Android raycast/trackables.

### 4.7 OpenXR runtime (Spike 5) and what WebKit still needs

The runtime is the easy half. WebKit's `OpenXRGraphicsBindingOpenGLES` can't
create its display or export swapchain images on PS5 (it wants GBM, a
surfaceless EGL with DMA-buf export, or Android), and its frame fences are
fds. That's the patch Spike 7 has to write. Smaller items: a flip blit per
frame (~0.1 ms), WebXR sees 60 Hz (the panel is at 120 Hz through
reprojection), the floor height is assumed (1.6 m below the starting head
pose), WebKit reports two phantom controllers when none are active, and
WebKit checks for `XR_EXT_local_floor` without enabling it. Those last two
are upstream WebKit bugs.

## 5. Risk register

| # | Risk | Likelihood | Impact | Mitigation / deciding spike |
| --- | --- | --- | --- | --- |
| R1a | Kernel panic when a forked child with shared-memory mappings exits (seen on 13.60, run 1). WebKit children map shared memory constantly | observed once | high if it also hits non-forked children | Bisect (`fork` group: inherited SHM_ANON vs received fd); `spawn` group checks the elfldr-child case; avoid fork |
| R1 | No multi-process from a title (no spawn, no fd passing) | **retired** (Spike 8) | — | The title spawns via elfldr; reversed unix rendezvous carries SCM_RIGHTS |
| R13 | IPC latency: ~130 µs per round trip title ↔ payload (payload threads alone are 4× slower than the title's), against 10–20 µs on desktop | observed | medium: WebKit's sync IPC (layout queries, input) adds up | Payload thread priority/affinity; shared-memory streams (WebKit's StreamConnection) for hot paths; prewarm processes (spawn takes seconds) |
| R2 | GL memory can't reach the Hmd2 layer (needs tiling, or a process-ownership problem) | medium | blocking for WebXR | Spike 3a (linear T#), 3b (zero-copy); AGC/GL blit into SW_64KB_R_X as plan B |
| R3 | GPU only usable in the title process | medium (ps5-opengl: likely) | high: WebGL must run where XR runs (Spike 7 options B/C) | Spike 2 on both stacks in a payload; pacbrew Mesa claims payload support |
| R4 | No cross-process GPU memory | medium | medium: XR via option B/C, pages via SHM readback | Spike 1 `dmem-share` |
| R5 | pacbrew Mesa / ps5-opengl immature or abandoned (upstreams already deleted) | high | medium | Keep both routes; vendor sources and patches; pin hashes |
| R6 | Per-firmware fragility (export tables, SCE stubs, Relapse limited to ≤13.60) | high | medium | Anchor-checked tables, one generator script, document the supported firmware |
| R7 | JIT unavailable, so slow JS | medium | medium for WebXR (three.js frame budgets) | Spike 1 JIT probes, Spike 4 perf numbers |
| R11 | GL draw-call overhead makes busy WebGL/three.js scenes CPU-bound | medium (reduced) | medium for WebXR content | Measured and partly fixed (§4.2c): 72 → 8 µs per draw with patch 0003, profiling off and async preparation. Left: driver state caching (port work), compare pacbrew Mesa/zink. glthread takes the work off the calling thread |
| R12 | Clean exit/suspend: titles were killed by the system on close (0xa0d0c00f, "debug error") | **fixed** | low (UX) | Cause: ps5-opengl's `catchReturnFromMain` sleeps forever after `main`. `spikes/common/title_exit.c` replaces it with `sceShellCoreUtilExitApp` (as PS5VR): Spike 8 closes with no error (2026-10-27). Still to do: handle suspend/resume events in the browser |
| R8 | Memory budget (WebKit + JSC + GPU + 60 MB hand-tracking work memory) | medium | medium | Spike 1 `mem-flex`/`mem-direct` |
| R9 | Dependency porting (glib, libsoup, ICU, Skia deps) is larger than estimated | high | schedule | pacbrew covers ~60%; budget it explicitly (Spike 6) |
| R10 | Tethered jailbreak: re-run after every reboot | certain | UX only | Document it; autoload scripts |

## 6. Licensing

- ps5-payload-sdk, PS5VR (and `spikes/common/psvr2`, which is derived from
  it), and ps5-opengl are **GPL-3.0-or-later**. pacbrew's Mesa is MIT (plus
  its patches). WebKit is LGPL-2.1 (WebCore/JSC) and BSD (WebKit2, WTF).
  ANGLE and Skia are BSD.
- A combined binary linking GPL-3 parts makes the whole distribution
  GPL-3. LGPL-2.1+ and BSD code may be combined into it. That's fine for
  homebrew if the full corresponding source is published.
- **Never distribute** Sony `.sprx`/`.prx`, firmware, keys or decrypted
  system libraries. Per-firmware export tables (offsets + anchor strings)
  follow PS5VR's practice. Users generate them from their own console.

## 7. Recommendation and estimate

_To be finalised after the console runs._ Provisional, for one experienced
engineer:

| Phase | Effort |
| --- | --- |
| Spikes 0–3 on device, fixes | 1–2 weeks |
| JSC + dependency ports (glib, libsoup, ICU, …) | 4–8 weeks |
| WPE build + PS5 WPE Platform backend + process provider | 4–6 weeks |
| OpenXR runtime on psvr2 + controllers (+ hands) | 2–4 weeks |
| WebXR texture path (Spike 7 option A/B) + polish | 3–6 weeks |
| **Total to an end-to-end WebXR demo** | **~4–6 months** |

Add 2–3 months if R1 forces an in-process WebKit.

## 8. What to run on the console (in order)

1. `make -C spikes/00-baseline hello send`. Install PS5VR and play a video
   in the headset.
2. `make -C spikes/01-procs-ipc send`.
3. `make -C spikes/03-gl-hmd2 send` (3a: wear the headset).
4. Spike 2: the pacbrew Mesa payload (`spikes/02-gl-egl/pacbrew-mesa/deploy.sh`)
   and the ps5-opengl title (with `WITH_PROCS_TESTS`).
5. Spike 3b title. Then Spike 4 `jsc`.
