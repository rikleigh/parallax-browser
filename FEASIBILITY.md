# Feasibility: WebKit + WebXR on a jailbroken PS5 with PS VR2

_Status: **study complete (2026-10-27). Verdict: go. The goal is now a
browser that plays VR video from tube sites, in the page and in the
headset, rather than WebXR (§1a); estimate ~4–7 days of agent sessions, bound by console debugging (§7).**_

_Port status (2026-10-28): **Milestone 1 reached.** The browser title
PPSA99020 browses on the TV; Milestone 2 (video) has started: see §9._

Everything below marked **verified** was built or checked on the Mac.
Everything marked **pending** needs a run on the console, and the spike that
answers it is named. Sources are community GitHub projects from
Sept–Oct 2026. Several are days old and partly AI-written, so treat their
claims as unverified until a spike confirms them.

## 1. Verdict so far

**Go.** Every platform question the plan set out to answer has a console
result:

- **GL → PS VR2 works zero-copy** (Spike 3b): GL 4.6 renders into textures
  that Hmd2 samples directly, 60 fps reprojected to 120 Hz, head-tracked.
- **Multi-process works** between payloads (Spike 1) and between a title and
  the payloads it launches (Spike 8). The rendezvous is reversed (the title
  listens), then fd passing and shared memory work as WebKit expects.
- **GLES 3.2 works** in a title on patched ps5-opengl (Spike 2). Draw cost is
  down from 72 to 8 µs per draw (Spike 2b), which is enough for modest
  three.js scenes.
- **JavaScriptCore runs** (Spike 4), about 3× slower than an M4 Max with
  the interpreter. Payloads can map JIT memory, so the WebProcess can get a
  JIT later.

The architecture that fits these results (§4.4c, §4.8):

- **One title process: UIProcess + GPU process + OpenXR runtime.** It owns
  the headset, GL, WebGL and compositing, so WebXR frames never leave the
  process.
- **Payloads: WebProcess(es) and NetworkProcess**, launched through elfldr.
  They have 8 GB of memory and the JIT. Pages are painted on the CPU and sent
  as shared bitmaps (WebKit's WC mode), so payloads need no GL.

What remains is port work, not unknowns. The largest new items are running
WebKit's GPU process inside the UIProcess (a patch WebKit doesn't have) and
a direct-memory heap for the title (§4.8). The estimate is in §7.

## 1a. Revised goal (2026-10-27): browse, and watch VR video in the headset

The owner's actual aim is a web browser, plus the option to watch a 360° or
180° video found while browsing in the PS VR2. WebXR isn't needed. That
removes the hardest parts of the plan above:

- **No WebGL, so no ANGLE and no GPU process.** Pages are painted on the CPU
  in the WebProcess (payload) and handed to the title as shared bitmaps,
  which upstream WPE supports (`HardwareAccelerationEnabled=false`, §4.8).
  R14 (GPU process inside the UIProcess) and R15 (ANGLE texture sharing) go
  away, and R8 eases because only the UIProcess lives in the title.
- **No WebXR, no OpenXR runtime, no XR patches to WebKit.** Spike 5 stays on
  the shelf.

What it adds:

- **Video plays inside pages.** The owner's sources are tube-style sites
  that stream VR video (2026-10-27). They play it with their own JavaScript
  players, which fetch HLS/DASH segments and feed them to Media Source
  Extensions (hls.js, dash.js, Shaka, video.js), or give Safari an `.m3u8`
  directly. The URLs are signed and short-lived and are hidden behind that
  JavaScript, so lifting a link out of the page and playing it elsewhere
  would mean per-site extraction that breaks often. Instead, WebKit's own
  media stack plays the video, so every site works the way it does in
  Safari:
  - **GStreamer**, WPE's only media backend, ported like the other
    dependencies. MSE and native HLS (`adaptivedemux2`) come with it.
  - **A decoder element of our own** wrapping Sony's hardware decoder
    (`libSceVideodec2`; Spike 9 measured H.264, HEVC 8/10-bit up to 8K and
    VP9 from a title, decoding into GL buffer memory), with FFmpeg/dav1d
    (`gst-libav`) as the software fallback, for AV1 in particular, which the
    hardware lacks.
  - **Hole-punch video**, as WPE set-top boxes do: the page leaves a
    transparent hole where the `<video>` is, and the title draws the decoded
    frame under the page bitmap. Decoded frames never touch the CPU, which
    matters because pages are painted on the CPU.
- **"View in VR":** a button (and pad shortcut) takes the `<video>` that is
  playing into the headset. The player keeps running; the title just draws
  the same decoded frame on a sphere (360°) or dome (180°), mono or
  side-by-side/top-bottom 3D, through Spike 3b's zero-copy path. It works
  whatever player the site uses, because it starts from the decoded frame,
  not the site's player. The projection is guessed from the page and the
  stream URL (`180`, `360`, `SBS`/`LR`, `TB`, `fisheye`, `MKX200`, which these
  sites put in titles and filenames), and the pad changes it. Back returns
  to the page with playback continuing.
- **The owner's sources, checked 2026-10-27:** VR video sites and
  YouTube 360/180. 4K is enough; 6K–8K is a bonus.
  - **VR site:** plain progressive MP4 (no MSE needed), signed URLs. HEVC goes
    up to 7680×3840 (one sample: HEVC Main 8-bit, 29.97 fps, ~35 Mbit/s,
    AAC-LC stereo), with 5760 and 4320 versions too. H.264 stops at 1080p,
    so VR video above 1080p means HEVC. Its public JSON feed
    (`/video/id/<n>`), a format other VR sites also publish for the
    site's app, gives every source and the exact projection (`viewAngle`,
    `stereoMode`). "View in VR" uses it when a site has it, and can then
    pick a sharper source than the page player.
  - **YouTube:** MSE with VP9 (WebM) or AV1 (MP4) at 3840×2160; H.264 stops at
    1080p and there's no HEVC. So YouTube in 4K needs VP9 or AV1 decoding.
    The projection comes from the player data, and YouTube may repack 360
    frames (3840×2160, not 2:1), which "View in VR" must unpack.
  - **Audio:** AAC on both, plus Opus on YouTube. Both are cheap in
    software.
- **Out of reach:** DRM-protected streams (no Widevine or PlayReady for
  homebrew). Free tube sites rarely use DRM; some paid VR sites do.
- **Site players that need WebGL or WebXR** (R17). These players usually
  hide their VR button when `navigator.xr` is missing and show the flat
  video, which is all we need. Players that refuse to play at all without
  WebGL would bring WebGL back into scope.
- **Browsing in the headset, two ways:**
  - **Free:** PS VR2's Cinematic Mode shows a non-VR title on a large
    virtual screen. The owner confirmed it shows the home screen and our
    titles (2026-10-27).
  - **Later:** our own VR panel mode, which draws the page on a curved
    panel in the headset at higher resolution, with Sense-controller
    pointing. It needs its own VR keyboard.

## 2. Engine choice: WebKit, not Chromium

Decided: WebKit (WPE port). Chromium was to be reconsidered only if there
was no multi-process model at all. Spikes 1 and 8 found one, so WebKit stays.
Chromium's one advantage is that it can run its GPU work inside the browser
process out of the box, while WebKit needs a patch (§4.8).

| | WebKit (WPE) | Chromium |
| --- | --- | --- |
| Port cost | CMake (`prospero-cmake` exists), FreeBSD support and `OS(PLAYSTATION)` paths upstream. Sony ships WebKit on PS4/PS5 | GN + its own toolchain, new `target_os`. The FreeBSD port alone carries 1000+ patches |
| WebXR backend | OpenXR on EGL/GLES, Unix-like, so close to our case | OpenXR on Windows (D3D11) and Android only, as far as known |
| WebGL | ANGLE | ANGLE (same) |
| No multi-process | no supported single-process mode, so a large patch (not needed: Spike 8) | `--single-process` / `--in-process-gpu` exist |
| GPU work in the browser process | patch: GPU process started inside the UIProcess (§4.8) | `--in-process-gpu` |
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
| 9 hardware video decode | **verified:** `libSceVideodec2` bound through a 13.60 export table (dlsym refuses it); AUs from Annex-B, IVF and fMP4; zero-copy GL path | **PASS (title)** ([README](spikes/09-hw-decode/README.md)): VR clip **8K HEVC 93 fps**, 6K 130, 4K 90; H.264 4K 87; VP9 4K 84, 5.7K 68, Profile 2 88; HEVC Main10 8K 59, 8192x4096 60 (80 Mbit/s noisy clips); 0 errors. Linear NV12 (10-bit: LSB-aligned 16-bit) written into **GL buffer storage**, converted on the GPU in 3.2 ms at 8K with no CPU copy (readback matches). 8K decoder takes 223 MB flexible memory (139 MB at depth 2, 79 fps). **Payload: blocks in the first call.** No AV1 in the decoder; dav1d in software: 4K 92 fps, 8K 34. FFmpeg HEVC in software: 4K 46–51, 8K 20 (8 threads; 16 threads collapse). Headset run plays (0 submit errors) but **visual check open** | **go** |
| 10 title direct-memory heap | **verified:** `dheap.c` (dlmalloc 2.8.6, MIT-0) over a 16 GiB `sceKernelReserveVirtualRange`, commits via `sceKernelAllocateMainDirectMemory` type 12 + `sceKernelMapDirectMemory` prot 0x03 | **PASS (title PPSA99015)** ([README](spikes/10-title-dmem/README.md)): 4 GB of direct memory committed with **flexible memory unchanged (440.0 MB)**; malloc, C++ new, glib all land in it; 8-thread stress 1.5 GB live, 0 bad, 21 M alloc/free pairs/s vs libc's 0.04 M/s; **8K HEVC decoder at depth 4 next to a 1.5 GB heap: 92.3 fps, 0 errors**; 64 direct-memory thread stacks cost no flexible memory (libc stacks: 128 MB, never returned); RW→RX `sceKernelMprotect` JIT works. **The title image costs flexible memory 1:1** (100 MB padding: 440 → 340 MB at the first constructor); title text is execute-only | **go** (R8) |
| 11 WebGL (ANGLE on ps5-opengl) | **verified (Mac):** ANGLE 2.1.29096 (WebKit's copy) cross-compiled for the title with its GL backend over EGL (3 small patches: a static `egl*` lookup, no futex, FreeBSD clocks); test title PPSA99006 built; **host check PASS** under Spike 2's Rosetta harness (real EGL facade + driver caps + Mesa): EGL 1.5 ANGLE display, surfaceless, ES 3.0 contexts, 110 extensions, ESSL 300 es/100 compiled | **verified (PS5, 2026-11-07):** triangle, FBO, texture sampling, GL fences and a WebGL 1 shader all draw correctly through ANGLE; 8.3 µs per draw vs 8.2 µs native, so ANGLE adds about 0.1 µs; 1080p upload 35 ms either way ([log](spikes/11-webgl/results/console-2026-11-07.log)). Share-group probe not yet run | **go** |
| 5 OpenXR runtime | **verified:** `libopenxr_psvr2` covers all ~50 calls WebKit makes (+ the rest of 1.0 core); host smoke test replaying WebKit's sequence **18/18 PASS** via dlopen, static link and the official Khronos loader (1.1.65); PS5 backend cross-compiles (`libopenxr_psvr2.a`) | not run; the study closed without it. Its PS5 backend is Spike 3b's code path, which passed. The first port task | — (not a gate) |
| 6 MiniBrowser | design ([README](spikes/06-wpe-minibrowser/README.md)); superseded in part by §4.8 (WC mode, CPU-painted WebProcess, no GL in payloads) | — | port work |
| 7 WebXR e2e | design ([README](spikes/07-webxr-e2e/README.md)); §4.8 picks the single-title option (OpenXR and WebGL in one process) | — | port work |

Console logs for every run above are in
`evidence/2026-10-27/` (raw console logs, kept locally and not published): `00-sysinfo`, `01-*` (incl.
the run that panicked), `02b-draw-bench`, `03a-hmd2-probe-payload`,
`titles-02-03b` (Spikes 2 and 3b over TCP), `04-jsc-*`, `08-title-ipc` and `09-*`.

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
- Build and capability findings: §4.2b. It has not been run on the console.

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
`evidence/2026-10-27/02b-draw-bench.log` (kept locally).

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
| sceAudioOut | opens and plays, but **never released**: ~8 processes exhaust the audio daemon (Init 0x80260002, then blocks system-wide) | **yes**, freed when the title is killed |

Flexible memory, two readings: Spike 1 committed 251 MB of anonymous memory in a
title; Spike 9's title (also ps5-opengl) reports 419 MB from
`sceKernelAvailableFlexibleMemorySize` at start, and an 8K HEVC decoder maps
223 MB of it (139 MB at pipeline depth 2).

**Title vs payload libc behaviour (found getting the browser title up, 2026-10-28;
full table in [port/webkit/README.md](port/webkit/README.md)):** in a title,
`pipe2(O_CLOEXEC)` fails with EINVAL, `fcntl` F_GETFD/F_SETFD/F_DUPFD fail with EINVAL,
`dup`/`dup2` are EPERM, sockets can't be made non-blocking (F_SETFL and FIONBIO are
EACCES), and the SDK libc's raw syscalls crash the title (SYSTEM_ILLEGAL_FUNCTION_CALL).
All of these are worked around in `port/webkit/shim` and `apps/title` (pipe()+fcntl,
SCM_RIGHTS to itself for dup, MSG_DONTWAIT per call, libkernel instead of raw syscalls).
Anonymous mmaps, malloc and thread stacks go to direct memory (Spike 10), so the browser
title holds steady at 334 MB of flexible memory free while browsing.
Payloads also cost system-wide CPU page tables (8192 in total, ~4550 used by the
system, ~300 per WebKit payload), which limits how many WebProcesses can stay alive.

**Audio from payloads (found 2026-10-28, Milestone 2): don't.** A payload can
open sceAudioOut (main port, system user 255) and it plays in real time, but
the system audio daemon never releases what a payload process took: after
about eight payload processes had used it, `sceAudioOutInit` returned
0x80260002 and then blocked (IPC wait) in every new process, system-wide.
A title launch later hung in the launcher and the console rebooted. Titles
are cleaned up: 12 launch/SIGKILL cycles of the browser title, each leaving
a port open, all reopened one. So audio is played by the title, fed by the
payloads over a unix socket (port/webkit/README.md, *Media*).

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
- **Revised by §4.8:** the GPU process can't be a separate title, so it has to
  run inside the UIProcess (a patch), with compositing moved into it (WC mode).

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

### 4.8 WebKit's GPU process on PS5 (read from source, WebKit 15486a0178)

- **Upstream WPE builds the GPU process but doesn't use it for WebGL.**
  `ENABLE_GPU_PROCESS_BY_DEFAULT` is Cocoa-only, so `UseGPUProcessForWebGL`
  and `…ForCanvasRendering` default to false on WPE. WebGL and the threaded
  compositor run with GL **in the WebProcess** (a payload: no ps5-opengl,
  ~55 MB direct memory).
- **WPE's GPU process requires GBM** (`OptionsWPE.cmake:155`). Its WebGL
  backend (`RemoteGraphicsContextGLGBM.cpp`) hands frames to the WebProcess
  compositor as DMA-buf + fence fds. The PS5 has no cross-process GPU memory
  (Spike 1 `dmem-share`), so this path can't work as it is.
- **The fit is WebKit's "WC" mode (`USE_GRAPHICS_LAYER_WC`, `Platform/WC.cmake`),
  used by WinCairo and by Sony's own port when its GPU process is on.** The
  WebProcess paints tiles on the CPU into shared bitmaps and sends the layer
  tree. The GPU process composites with TextureMapper GL, WebGL included, in
  one process (`WCScene.cpp`), and returns the frame as a bitmap. The
  WebProcess needs no GL. WPE also has a CPU-only page path
  (`HardwareAccelerationEnabled=false`, `NonCompositedFrameRenderer`) for
  first light.
- **The GPU process has to share the title with the UIProcess.** Hmd2
  (OpenXR, in the UIProcess) and GL both need the title, and a title can't
  start a second title. WebKit has no in-process mode for it.
  `AuxiliaryProcess::initialize` sets process-global state: the process type,
  `Process::setIdentifier`, `SessionID`/`WebPageProxyIdentifier` generation
  protection (which would break the UIProcess), and `terminate()` stops the
  main run loop. So the patch is an in-process start of `GPUProcess` on the
  UI main run loop that skips those. The heavy work already runs on its own
  stream work queues. There are 8 `isInGPUProcess()`-style checks to review.
  Chromium has this built in (`--in-process-gpu`); this is the one point where
  Chromium has the edge (§2).
- **WebXR in that process stays zero-copy:** the OpenXR swapchain and the
  WebGL context live in the same process. The remote WebGL path already
  forwards `CreateExternalImage`. What's left is a PS5 `ExternalImageSource`
  (an in-process texture instead of DMA-buf fds), plus a way for ANGLE's
  context to use a native ps5-opengl texture. That means EGLImage, or a T#
  import added to the driver.
- **Lead, untested:** Sony's port launches `/app0/*Process.self` as
  app-local children of the app (`ProcessLauncherPlayStation.cpp`,
  `PlayStation::launchProcess`). If a homebrew title can reach the system call
  underneath, and those children keep app rights (AGC, title memory), the GPU
  process could stay a separate process.
- **Title memory:** with the UIProcess and the GPU process in the title, its
  251 MB of flexible memory is tight. Heaps need a direct-memory backend
  (risk R8).

### 4.9 WebGL (Spike 11, 2026-11-07)

Full write-up: [spikes/11-webgl/README.md](spikes/11-webgl/README.md).

- **ANGLE runs on ps5-opengl.** WebKit's vendored ANGLE builds for the
  title with its GL-over-EGL backend; ps5-opengl's facade gives it what it
  asks for (EGL 1.4, `KHR_create_context`, surfaceless contexts, ES 3.2
  native contexts from Spike 2's patch) once three things are shimmed:
  the `egl*` entry points (the facade's `eglGetProcAddress` only knows GL,
  and a static title has nothing to `dlopen`), no `linux/futex.h`, and
  FreeBSD clock/thread-name names. No EGLImage and no EGL fences are needed
  for WebGL itself (ANGLE uses GL sync objects). ANGLE caps the client at
  **ES 3.0**, which is WebGL 2. Checked on the Mac against the real EGL
  facade, driver caps and Mesa, then on the console: the triangle, FBO,
  texture and fence checks pass, and a draw costs 8.3 µs through ANGLE
  against 8.2 µs native, so ANGLE's overhead is negligible.
- **The route (ranked in the spike):** WebKit's remote WebGL with the
  receiver hosted in the title: `IPC::Semaphore` on `_umtx_op` (small),
  `ENABLE_GPU_PROCESS` without GBM and ANGLE in the build (medium), a host
  object in place of `GPUConnectionToWebProcess` (medium: no process-global
  state is needed, so R14 shrinks), and the drawing buffer into `WCScene`
  through `GraphicsContextGLCoordinated`'s texture + `glFenceSync` buffer
  (the large piece: upstream's WC WebGL layer is compiled out under
  coordinated graphics). Video textures come from the title's own video
  texture by a GPU copy in the same share group. First risk to test: ANGLE's
  GLES contexts sharing with the title's GL 4.6 core context on ps5-opengl.
  R15 therefore no longer needs EGLImage.
- **Fallback:** llvmpipe + ANGLE in the WebProcess, painted through
  WebKit's readPixels path (medium; software speed; no hardware video).
  A separate GPU process or GPU use from payloads is not a route today (every
  homebrew GPU stack runs as a title; Sony's launcher is a private API).
- **Why it matters now:** three.js r163+ and A-Frame are WebGL2-only and
  fail outright without a context; most VR video sites show flat video
  instead, and "View in VR" covers those.

## 5. Risk register

| # | Risk | Likelihood | Impact | Mitigation / deciding spike |
| --- | --- | --- | --- | --- |
| R1a | Kernel panic when a forked child with shared-memory mappings exits (seen on 13.60, run 1). WebKit children map shared memory constantly | observed once | high if it also hits non-forked children | Bisect (`fork` group: inherited SHM_ANON vs received fd); `spawn` group checks the elfldr-child case; avoid fork |
| R1 | No multi-process from a title (no spawn, no fd passing) | **retired** (Spike 8) | — | The title spawns via elfldr; reversed unix rendezvous carries SCM_RIGHTS |
| R13 | IPC latency: ~130 µs per round trip title ↔ payload (payload threads alone are 4× slower than the title's), against 10–20 µs on desktop | observed | medium: WebKit's sync IPC (layout queries, input) adds up | Payload thread priority/affinity; shared-memory streams (WebKit's StreamConnection) for hot paths; prewarm processes (spawn takes seconds) |
| R2 | GL memory can't reach the Hmd2 layer (needs tiling, or a process-ownership problem) | **retired** (Spike 3b) | — | Zero-copy via T# export, 3600/3600 frames |
| R3 | GPU only usable in the title process | **accepted** (ps5-opengl is title-only; payload direct memory ~55 MB) | designed around | All GL in the title (§4.8); pacbrew Mesa in a payload is still untested |
| R4 | No cross-process GPU memory | **observed** (Spike 1 `dmem-share` EACCES) | designed around | WC mode: the WebProcess sends CPU-painted bitmaps; WebGL, compositing and XR share one process (§4.8) |
| R16 | Video decoding for VR video (§1a): the owner's sources need HEVC (VR site, 4K–8K) and VP9 or AV1 (YouTube, 4K) | **retired** (Spike 9) | low | A title reaches `libSceVideodec2`: 8K HEVC from a real VR site clip at 93 fps, 4K H.264/VP9 at 84–87, 10-bit too, decoded straight into GL buffers (no CPU copy). Left: AV1 is software only (dav1d 4K 92 fps); an 8K decoder costs 140–220 MB of the title's ~420 MB flexible memory; 8K60 needs a decoder thread (the single-threaded spike reached 47 fps). The owner checked TV and headset: correct, smooth, 60 fps footage smooth; 30 fps footage shows jitter in nearby people, a property of the source ([Spike 9](spikes/09-hw-decode/README.md)) |
| R17 | Tube sites' players misbehave in our browser: they need WebGL to play at all, or serve a lesser stream or an "unsupported browser" page to an unknown user agent | medium | medium: per site | Check the owner's sites on a desktop browser with WebGL and WebXR off; send a Safari user agent; WebGL (R14/R15) only if a site the owner needs demands it |
| R18 | GStreamer port (core, base, good with `adaptivedemux2`, our decoder element, hole-punch sink) is larger than the other dependencies | medium | schedule | It shares glib with WPE; plugins are built static and only the few needed; set-top-box WPE ports have done hole-punch on similar hardware |
| R14 | GPU process inside the UIProcess (WebXR goal only; not needed for §1a): WebKit has no in-process mode; process-global init (process type, identifier generation, run-loop stop) clashes | certain (it's required) | high: schedule, plus a downstream patch to carry | In-process `GPUProcess` start that skips process-global init (§4.8). Alternative: Sony-style app-local child processes, if reachable (lead, untested) |
| R15 | (WebXR goal only) WebGL (ANGLE) can't use the OpenXR runtime's native ps5-opengl textures in the same process; ANGLE port to the PS5 target | medium | high for WebXR | Add EGLImage or a T# import to ps5-opengl (we carry its patches); ANGLE GL backend on Mesa GL 4.6 / GLES 3.2 |
| R5 | pacbrew Mesa / ps5-opengl immature or abandoned (upstreams already deleted) | high | medium | Keep both routes; vendor sources and patches; pin hashes |
| R6 | Per-firmware fragility (export tables, SCE stubs, Relapse limited to ≤13.60) | high | medium | Anchor-checked tables, one generator script, document the supported firmware |
| R7 | JIT unavailable, so slow JS | **retired** (2026-10-28) | medium for WebXR (three.js frame budgets) | JS runs in payloads, where RWX, RW→RX and `sceKernelJit*` all work (Spike 1). The JIT now runs in the WebProcess (WebKit patch 0030: Baseline, DFG, FTL and the regexp JIT on an RWX pool; the title stays on the LLInt): Spike 4's bench 3.06 s -> 0.44 s on the console, 7.6x as a page. Costs ~15 MB of image per executable and 200–500 MB more WebProcess heap ([port/webkit/README.md](port/webkit/README.md), *JIT*) |
| R11 | GL draw-call overhead makes busy WebGL/three.js scenes CPU-bound | medium (reduced) | medium for WebXR content | Measured and partly fixed (§4.2c): 72 → 8 µs per draw with patch 0003, profiling off and async preparation. Left: driver state caching (port work), compare pacbrew Mesa/zink. glthread takes the work off the calling thread |
| R12 | Clean exit/suspend: titles were killed by the system on close (0xa0d0c00f, "debug error") | **worked around** | low (UX) | First cause: ps5-opengl's `catchReturnFromMain` sleeps forever after `main`. Second, watched on the TV: any GPU title that ends itself (`sceShellCoreUtilExitApp`, `exit`, `_exit`) gets a debug error, because ShellUI's suspend-before-kill crashes it (reason 0xc). Only a SIGKILL from another process shows nothing, so `title_exit.c` asks the launcher payload to SIGKILL the title ([tools/launcher](tools/launcher/README.md)). Still to do: handle suspend/resume (rest mode) in the browser |
| R8 | Memory budget: the title has ~440 MB of flexible memory (13.60), and the UIProcess, Sony's video decoder (223 MB for 8K at depth 4, 139 MB at depth 2) and the title image itself (1:1, ~112 MB for today's browser ELF) all draw on it | medium | high | **Mostly retired by Spike 10:** malloc, libpas/bmalloc pages and thread stacks go to direct memory (11.7 GB, flexible untouched, measured). What's left: keep the image small; budget the decoder at depth 2 for 8K if the image stays ~110 MB (~190 MB headroom) |
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

**Recommendation: go**, for the revised goal in §1a, targeting firmware
13.60 only (R6). The title is the UIProcess plus the VR video player; the
payloads are the WebProcess (CPU-painted pages) and the NetworkProcess. The
WebXR architecture (§4.4c, §4.8) stays documented in case WebXR is wanted
later.

**The work is done by Claude agents with the owner at the console**, so the
estimate is not in engineer-months. For calibration, the provisional plan put
Spikes 0–3 at 1–2 weeks. With agents they took a few hours, dozens of console
runs included: the whole study, Spikes 0–8, ran from about 16:00 one
afternoon. Writing code and porting dependencies are not the limit. Three
things are:

1. **The console loop.** The launcher (below) now registers, starts and stops
   titles without the owner. Crashes can still need a reboot and a re-run of
   the jailbreak (R1a, R10), and rest mode stops payloads until the console
   wakes. A large WebKit binary will crash far more often than the spikes
   did, with only the kernel log and the TCP log to debug it.
2. **Build time.** Measured in the port: a clean WPE build takes 747 s on
   the Mac (configure 37 s, ninja 710 s), and relinks after a change take
   8–31 s. Build time turned out not to matter.
3. **Checks only a person can do:** the TV picture and the headset.

| Phase | Agent work | What sets the pace |
| --- | --- | --- |
| Hardware-decode spike (R16): **done** (Spike 9), TV and headset checked by the owner | — | — |
| Dependency ports (glib, libsoup, Skia, HarfBuzz, FreeType, fontconfig, ICU, …) (R9); no ANGLE | large, but parallel and Mac-only | build time |
| JSC fixes (`Date`, plus Spike 4's patches carried over) | small | one console run |
| WPE build + PS5 WPE Platform backend (VideoOut, DualSense, IME, fonts), CPU-painted pages | medium | console crash-and-fix loop |
| Process launcher: elfldr spawn, reversed rendezvous, prewarming | small (Spike 8 code) | console loop |
| **Milestone 1: browsing on the TV, and in the headset through Cinematic Mode** | | **~1–3 days of sessions** |
| Media stack: GStreamer port, MSE and HLS, our hardware-decoder element, audio out, hole-punch video in pages (R18) | medium–large | console loop, TV checks on real sites |
| "View in VR": the playing `<video>`'s frames on a sphere/dome, 3D formats, projection guess, pad controls, switching into and out of VR | small–medium (Spike 3b path) | headset checks |
| Owner's sites checked one by one: user agent, player quirks (R17) | small | owner at the TV |
| **Milestone 2: VR video from tube sites, in the page and in the headset** | | **~2–4 more days of sessions** |
| VR panel mode: the page on a curved panel, Sense-controller pointing, VR keyboard | small–medium | headset checks |

So it is roughly **4–7 days of sessions to Milestone 2**, plus about a day
for the VR panel mode, with wide error bars. The spikes were small programs;
the browser is a multi-million-line program whose bugs will show up mostly
as crashes on the console. R16 decides how well high-resolution VR video
plays, not whether the project works.

**Biggest speed-up, now built: agents launch titles themselves.**
[`tools/launch.sh`](tools/launcher/README.md) registers a title
(`sceAppInstUtilAppInstallAll` after writing `/user/app/<ID>`), starts it
(`sceLncUtilLaunchApp`) and stops it with SIGKILL (no kill API accepts a
payload on 13.60, and only SIGKILL avoids a debug error). The
build → upload → register → launch → read-log → fix loop no longer needs the
owner. Only the TV and headset checks still need a person.

## 8. Optional console runs before the port

None of these changes the verdict. Each settles a detail of the port plan
cheaply.

1. **Spike 5 on the console:** `hello_xr`-style check of `libopenxr_psvr2`
   in a title.
2. **pacbrew Mesa in a payload** (`spikes/02-gl-egl/pacbrew-mesa/deploy.sh`).
   If it works within the payload's memory budget, R3 softens.
3. **Sony-style app-local child processes:** find the system call behind
   `PlayStation::launchProcess`, and check whether such a child can use AGC
   and the title's memory. If it can, R14 goes away.
4. **Payload thread priority/affinity** against Spike 8's 131 µs round trip
   (R13).
5. **JSC `Date`:** fix the wall-clock source and re-run Spike 4.
6. Re-run Spike 2b to capture the driver profile breakdown (the log now uses
   `freopen`).

The spike READMEs list the build and run commands for each spike.

## 9. Port progress

**2026-10-28, Milestone 1 (browsing on the TV): reached**, one day into the
port.

- **Dependencies** (`port/deps/`): a static sysroot with glib, libsoup
  (HTTP/2), OpenSSL, ICU, HarfBuzz, FreeType, fontconfig, libxml2, SQLite and
  the rest; plus GStreamer, FFmpeg decoders and dav1d for Milestone 2. The
  smoke tests pass on the console (Milestone 1: 18/18, Milestone 2: 9/9).
- **WebKit** (`port/webkit/`): WPE at 15486a0178 with a PS5 WPE Platform
  backend (VideoOut, DualSense, process manager), the WebProcess and
  NetworkProcess as one elfldr payload, and the UIProcess as the title.
  The JIT was off at first; the WebProcess now runs JavaScriptCore's JIT (patch 0030, R7).
- **Checked by the owner on the TV:** Wikipedia, BBC News, GitHub, Hacker
  News and DuckDuckGo load and render; pointer, click, back and zoom work on
  the pad. Old WebProcesses exit after cross-site navigation, and the title's
  flexible memory stays flat (334 MB free).
- **What it took** is in §4.4b: titles get a different libc from payloads
  (pipe2, fcntl, dup2, non-blocking sockets and mmap all behave differently),
  page tables are limited system-wide, and the title needs Spike 10's
  direct-memory heap for malloc, thread stacks and anonymous mmaps.
- **Finished the same day, checked on the TV:** the on-screen keyboard
  (libSceImeDialog loaded at run time by 13.60 export offset) for the
  address bar, search and text fields; YouTube's pages (search, results,
  watch page without playback) after placeholder media interfaces and a fix
  for forced compositing; file:// pages; Noto fonts for more scripts and
  colour emoji.
- **Crashes no longer take the console down.** Two console hangs this morning
  followed WebProcess crashes. The payloads now catch fatal signals, log a
  symbolizable stack and `_exit`, so the system crash reporter never sees
  them; the console then survived about ten crashes in a row. Payload
  threads also default to a 64 KiB stack, now raised to 2 MiB.
- **Milestone 2 has started:** video and audio in pages through GStreamer.
  - **Step 1** (2026-10-28): software decoding in the WebProcess, frames
    painted into the page, audio played by the title. YouTube played with
    sound on the TV, but at ~7 fps under its overlays.
  - **Step 2a, hole-punch video** (2026-10-28; port/webkit/README.md,
    *Hole-punch video*): the page punches a transparent hole, the WebProcess's
    `ps5videosink` hands each software-decoded frame to the title through
    shared memory with the time it is due (from the audio clock), and the
    title's new render thread converts it on the GPU and draws it under the
    page at the next vsync. The page itself now goes to the GPU as a memcpy
    into a persistent GL buffer (1 ms for a full page instead of 14–36 ms).
    Measured: local 1080p30/60 and YouTube 720p60/1080p60 at full frame
    rate, WebProcess CPU down by half to two thirds, title under 0.1 cores.
    WebKit's MSE queue was deepened to 1 s, which removed YouTube's 20–35 s
    video stalls after a pause. Waiting for the owner's TV check.
  - **TV check of step 2a** (2026-10-28 evening): YouTube and local video
    smooth, fullscreen, seeking and switching videos fine. One WebProcess
    crash after 150 s: JSC's GC read suspended threads' stack pointers at the
    SDK's ucontext offset, not the PS5 kernel's (+0x40), so it mostly
    skipped their stacks and sometimes copied a wrapped "stack" size
    (out-of-memory panic). Fixed (WebKit patch 0019); WebProcesses now log
    a memory line per minute.
  - **Step 2b, hardware decoding** (2026-10-28, port/webkit/README.md,
    *Step 2b*): a proxy decoder element in the WebProcess sends access
    units to the title, Videodec2 decodes them straight into the GL buffers
    the compositor reads, and the sink names the decoded picture instead of
    sending pixels. Measured in the title: YouTube VP9 2160p60 at 59.5
    frames/s, local HEVC 4K (8- and 10-bit) and 7680x3840 at 30 fps, the
    WebProcess at 0.05–0.07 cores for local clips; 8K HEVC takes 139 MB of
    the title's ~300 MB of flexible memory. Software decoding stays the
    fallback (AV1, other profiles, no title).
  - **TV check of step 2b** (2026-10-28 evening): the local HEVC 4K, 4K
    10-bit and 8K clips played smoothly. YouTube 4K60 first stuck after
    ~5 s: frames dropped as late (by GstVideoDecoder's QoS and by
    GstBaseSink before `prepare()`) never gave their decoder slots back, so
    all 12 were held. Fixed (e6afe3e): the proxy drops late pictures itself
    and releases them, the sink no longer drops hardware frames. On the
    re-check YouTube VP9 2160p60 played ~100 s at 59–60 frames/s with 0
    slot take-backs; the owner saw a few stutters only while the page was
    loading (280 late frames, mostly then), which the JIT should help.
- **Next, running in parallel:** "View in VR" (the playing video in the
  headset) and the JSC JIT in the WebProcess (R7).
- **View in VR** (2026-10-28, port/webkit/README.md, *View in VR*): the
  touchpad takes the playing video into the headset; the title's render
  thread drives Hmd2/VrTracker2 (Spike 3b/9's path) and draws the same
  decoded frames (hardware or software) as 360/180 mono, SBS or TB,
  fisheye or a flat screen, guessed from the page and stream addresses;
  the pad plays/pauses, seeks, recentres and cycles the projection through
  the page's video element; O returns to the TV. Log-only runs with a
  simulated headset passed (projections checked from eye-buffer captures,
  repeated enter/leave, slots and memory flat); waiting for the owner's
  headset check.
- **Evening fixes (2026-10-28):** the TV "no sound" was the TV being off
  (the sound test played with and without the PS VR2 param.json attributes;
  Hmd2 itself only checks attribute3 bit 0x400). BBC News's H.264 video
  failed in Videodec2 after a mid-stream resolution switch: the decoder is
  now drained and reset at a new SPS. WebProcess memory no longer only grows:
  libpas gives decommitted pages back (patch 0031: 300-420 MB of a ~1.1 GB
  heap returned at any time in a 48-load session), and a memory limit
  (`PS5WK_MEM_RESTART_MB`, default 3 GB) restarts the WebProcess at the next
  navigation. Details in port/webkit/README.md.
- **Send a link from another device** (2026-10-28; port/webkit/README.md,
  *Send a link from another device*): the title serves a link server on TCP
  9040. A page with a text box, a bookmarklet, `tools/ps5open [url]` (with no
  URL it sends the front Safari/Chrome tab, and starts the browser with the
  link if it isn't running) and a Share-menu shortcut all load a link in
  the browser.
- **Faster CPU painting (2026-10-29, patch 0032, port/webkit/README.md,
  *CPU painting*):** "slow HTML rendering" was the WebProcess painting each
  frame on its main thread alone. Frames are now recorded and replayed in
  bands on 4 threads, scrolls move the last frame's pixels and paint only
  the uncovered strip, overlapping damage is painted once, and a full-frame
  self-copy per frame is gone. Found on the way: the console's libc malloc
  and pthread mutexes collapse when threads contend (a malloc/free pair 0.3 us
  alone, 48-162 us with 2-6 threads; `test/lockbench`), which made extra
  painting threads slower than one; Skia's heap and C++ `new` in the child now
  use libpas. Scrolling a 1080p page in the harness: a local test page 10 ->
  85 updates/s (50 -> 3 ms each), BBC News 23 -> 41/s, Wikipedia 35 -> 95/s
  (one thread vs four), GitHub 43 -> 55/s. The title ran YouTube (hardware
  video, hole punch), BBC News, Wikipedia, GitHub and Hacker News with the
  new child; BBC's idle full-view repaints now take 16-19 ms. glib/GStreamer
  still use libc malloc and pthread mutexes (next candidate), and the title's
  heap locks with a pthread mutex too. Knobs: `PS5WK_PAINT_THREADS`,
  `PS5WK_SCROLL_COPY`, `PS5WK_PAINT_ASYNC`, `PS5WK_RENDER_LOG`. Waiting for the
  owner's TV check (scrolling, video, View in VR).
- **Settings (2026-10-29, port/webkit/README.md, *Settings*):** one settings
  page, opened with R3 on the PS5 or at `http://<ps5>:9040/settings` from a
  Mac or phone. It covers the JIT tiers, painting threads, fast scrolling,
  the memory limit, decommit, hole punch, VP9, hardware decoding, sound,
  the link server and the home page, and can clear cookies or all website
  data. It rewrites child.env while keeping the other lines, and applies
  changes by restarting the page (a new WebProcess) or the whole browser
  (wind-down, then the launcher's kill and launch on one connection).
  Checked on the console by log; R3 and the D-pad are waiting for the
  owner's TV check.
- **VR site hover previews froze the page (2026-10-30, port/webkit/README.md,
  *Hover previews*):** small H.264 High previews (380x222 level 2.2) got no
  picture from Videodec2, and removing one hung the WebProcess. Videodec2
  holds a level-sized DPB (16 frames at that size) plus 4 pipeline frames
  before its first picture, while the proxy stopped at 16 frames in flight
  and waited forever, on the streaming thread the main thread's teardown
  then waited for. Now 32 in flight, waits that end on a flush or stop, and
  a decode error after 9 s of silence from the title. Built, not yet checked
  on the console: `preview.html` with synthetic previews.
- **HLS in `<video>` (2026-10-30, port/webkit/README.md, *HLS streams*):**
  a site's player set an HLS master playlist as a `<source>`, which failed
  at once: WebKit only takes HLS with `WEBKIT_GST_ENABLE_HLS_SUPPORT=1`, and
  demotes adaptivedemux2's `hlsdemux2` because it downloads by itself. The
  children set the variable and patch 0033 keeps hlsdemux2 (the WebProcess
  payload has its own networking; the page's User-Agent is passed on, not
  its cookies). The legacy hlsdemux was tried first: playbin3 refuses it.
  Checked in the title with synthetic streams (TS H.264 and fMP4 HEVC, two
  variants each, `make-media.sh`): both played to the end through the
  hardware decoder with the switch up, 0 decode errors, audio to the title.
- **4K60 H.264 HLS froze for good (2026-10-30, port/webkit/README.md,
  *HLS streams*).** Videodec2's error states are sticky (13.60
  disassembly): after `0x811d0304`/`0x811d0300` every Decode fails at once
  until a Reset. The title never reset a decoder on errors, so the picture
  stopped while the audio went on. What triggered it (synthetic repro
  `hls-vr/mixed.m3u8`): tsdemux dropped the packet that starts a PES after
  an MPEG-TS continuity jump between segments, so the new segment's IDR
  with its SPS/PPS never reached the decoder. Fixes: the title resets a
  stuck decoder and resumes at a keyframe (retrying a failed keyframe
  once); a gst-plugins-bad patch keeps the PES start after a jump; the
  proxy no longer doubles parameter sets. Checked log-only in the title:
  `mixed.m3u8` went from 27 errors to 0 (2514 of 2514 pictures), and
  hlsdemux2's ABR switches, local H.264/HEVC/VP9 played without errors.
  The TV check on the site is still to do.
- **Payload malloc, mutex contention and networking (2026-10-30, checked on
  the console by log; port/webkit/README.md, *Locks, malloc and
  networking*).** `port/webkit/test/lockbench` (a payload; 400k operations per
  thread, the console's libc):

  | threads | malloc/free pair | pthread mutex lock/unlock | sem_wait/sem_post | ALU |
  | --- | --- | --- | --- | --- |
  | 1 | 0.30 us | 17.5 ns | 0.97 us | 2.5 ns |
  | 2 | 47.8 us | 4.2 us | 1.08 us | 1.7 ns |
  | 4 | 106 us | 13.4 us | 1.28 us | 2.6 ns |
  | 6 | 162 us | 20.8 us | 1.74 us | 4.4 ns |

  Total malloc throughput falls from 3.3 to 0.04 million pairs/s as soon as
  two threads allocate, and a contended pthread mutex hands over at ~4-20 us
  per lock; semaphores scale. What changed:
  - **ps5locks** (`apps/common/ps5locks.c`, ps5wk-child and the title):
    every `pthread_mutex_*`/`pthread_cond_*`/`pthread_rwlock_*` (and the
    attribute calls) wrapped onto futex words on `_umtx_op`, the call libpas's
    lock already uses on the console: no hand-over convoy, spin 100 then
    sleep. Covers GMutex/GCond/GRecMutex/GRWLock, libc++ `std::mutex`, WTF's
    Mutex/ThreadCondition (ParkingLot's park path), and in the title Spike
    10's dlmalloc arenas (there are 8, each behind a pthread mutex, not one
    global lock) and its render/decoder/audio threads. A build check fails
    if an unwrapped mutex/cond/rwlock function is imported.
  - **ps5malloc** (`apps/common/ps5malloc.c`, ps5wk-child): malloc and its
    family on libpas (fastMalloc's heap). Ownership by address: a bitmap of
    the 16 KiB pages of every anonymous mapping made through the child's
    `__wrap_mmap` (libpas's only source; the console's malloc never maps
    through it); anything else (`strdup`, `vasprintf`, `getcwd(NULL)`,
    `realpath(NULL)` from the console's libc) is freed by the console's libc.
    The child defines `__emutls_get_address` on the console's malloc
    (libpas's thread cache is an emulated `__thread`, and compiler-rt's
    emutls mallocs: it would recurse).
  - Off switches, decided before main: `/data/ps5webkit/no-fastlock`,
    `no-fastlock-title`, `no-pasmalloc`. Counters (parks, cond waits, foreign
    frees) in the child's `[mem]` line.
  - **Networking** (read from the code, survey in the README): the
    NetworkProcess is its own payload; libsoup with HTTP/2, 6 per host;
    WebKit's DNS cache 60 s; TLS via glib-networking/OpenSSL with session
    resumption; disk cache under `browser/cache`. Patch 0034 fixes three
    things found: the disk cache wrote every body over 16 KiB twice (0013's
    copy when `link()` fails) and never shrank (the copies were counted
    nowhere); DocumentBrowser's disk cache was 75 MB (now 1 GB with 16 GB
    free); DNS prefetch answers went into a cache map libsoup's typed
    IPv4/IPv6 lookups never read.
  - **Measured on the console** (`ps5wk-lockbench`, per operation per
    thread, 1/2/4/6 threads): malloc/free 279 ns/46.7/107/162 us with the
    console's libc, 46/45/53/90 ns with ps5malloc (6 threads: 0.04 -> 67 M
    pairs/s); a contended mutex 4.3/13.6/21.0 us -> 89/364/588 ns; condition
    ping-pong 8.5 -> 3.7 us; a 4+4 producer/consumer queue 0.13 -> 2.85 M
    items/s. `_umtx_op` works in payloads and in the title.
  - **End to end no measurable change** in these runs (painting was already
    on libpas): paint-ab scroll rates within run-to-run noise; YouTube 4K60
    in the title 16 vs 12 dropped of ~3700 frames, same CPU; a 48-load long
    session passed (0 failures, no crash, 0 foreign frees once `strdup` and
    `vasprintf` were wrapped, peak RSS 923 MB). libpas committed is now
    790-1035 MB in that session (670-890 before) because it holds glib and
    GStreamer memory too. Patch 0034 checked: no more `Blobs/` copies (up to
    12.9 MB per site before), warm loads served from the records' blobs.
  - Logic tests on the Mac: `test/lockbench/host-test.sh` (clean under
    ThreadSanitizer and UBSan).
  - Still open: per-connection `SSL_CTX_new`, 8 KB reads per main-loop
    callback, the WebProcess's separate libsoup for hlsdemux2; whether spin
    0 or 100 is better for real workloads (spin 0 wins the 4-6 thread
    microbenchmark, spin 100 the 2-thread one).
- **Where the time goes (2026-10-31, profiled on the console by log;
  port/webkit/README.md, *Profiling*).** A sampling profiler for both
  children (`PS5WK_PROFILE=1`: every thread, weighted by its own CPU clock,
  DWARF-CFI unwinding in the signal handler, folded stacks per second;
  `test/prof/prof-run.sh`, `prof.py`), and JIT counters plus per-tier
  tagging of JIT code (patch 0035). Findings, biggest first:
  - **JavaScript** is 55-60% of a WebProcess main thread that is 40-80% busy
    for 15-25 s on YouTube and news sites (YouTube search: 14 s of main
    thread CPU). JIT code itself is 10-19 points; the C++ runtime, the parser
    and the LLInt are the rest. The tiers work (YouTube: 3573 Baseline, 2279
    DFG, 77 FTL compiles, 0 failures, 0 refused allocations), but our 128 MB
    executable pool raises every tier-up threshold x1.8-1.9 on YouTube
    (JSC's memory-pressure multiplier; ~x1.05 with upstream's 1 GB).
  - **CPU painting** limits scrolling on pages with fixed/sticky content:
    BBC News repaints 2.07 Mpx per step, paint 25-28 ms of 35-43 ms per
    update (11-22 updates/s); bilinear image scaling is 7% of the main thread,
    copying the last frame forward 1-3 ms per update. The **bitmap transfer
    is not the problem**: the title uploads a full 8 MB frame in ~1 ms and
    uses 0.05-0.17 cores. GPU painting would pay off for paint-bound
    scrolling (BBC-like pages), not for script-bound loads (YouTube).
  - **NetworkProcess**: libsoup's HSTS store writes SQLite on the network
    main thread for every `Strict-Transport-Security` response (each moves
    the expiry): ~3.3 s of wall time (12%) in a The Verge load.
  - rAF runs at 110-158/s while scrolling (updates aren't paced to 60 Hz).
  - Owner's reports: no reload loops (YouTube: two top-level loads, the
    rest in-page; the title logs the previous URI on "load started"; The
    Verge was article-then-back; reddit a login redirect). Apple's product
    page uses its own scroll-driven JS (no CSS scroll timelines), sticky
    sections and scrubbed videos; in the harness it chose its full
    experience but its videos never got a source, so pinned video sections
    stay still. To be checked in the title.
  - Tooling fixes: elfldr briefly refuses loopback connections (ECONNREFUSED)
    under back-to-back launches; the process manager now retries for 10 s.
    On 13.60 another thread's CPU clock reads as the caller's.
  - Next (not done): 60 Hz pacing (hours), composited fixed/sticky layers or
    GPU painting for scrolling (days to weeks). The JIT pool and the HSTS
    writes were done on 2026-11-01 (next item).
- **JIT pool and HSTS writes (2026-11-01, harness only; README, *JIT pool
  and HSTS writes*).**
  - **HSTS**: a libsoup deps patch writes the HSTS database only on real
    policy changes (new host, removal, includeSubDomains, expiry moved by
    more than a day or half the max-age), and keeps the in-memory policy
    current. HSTS code on the NetworkProcess main thread during a The Verge
    load: 26% of its CPU before (1 run; 12.5% of wall time in a wall-clock
    run), 0.0-0.6% after (7 runs). Storing and http->https upgrades after a
    restart are not checked on the console yet (`test/net/hsts-test.sh`).
  - **JIT pool**: 256 MB per WebProcess (was 128 MB;
    `/data/ps5webkit/no-bigjitpool` restores it). A reservation costs one
    CPU page table per 2 MB at mmap time, touched or not (+64 for 256 MB,
    +192 for 512 MB, against 128 MB), and no real memory beyond the code
    written. Over 2 runs per size on YouTube search and channel pages and
    The Verge, the tier-up multiplier fell (channel x1.49 -> x1.22 -> x1.10,
    The Verge x1.22 -> x1.10 -> x1.05 for 128/256/512 MB), but main-thread
    CPU, load times and memory moved only within run-to-run noise. 256 MB
    takes most of the threshold inflation away for a third of 512 MB's
    page-table cost. Low confidence; tier-up threshold options not tried.
  - **Console health, open**: two console crashes are unexplained (one
    during a 129 MB ps5wk-shot send to elfldr, one overnight after stalled
    129 MB sends while the link ran at 0.1-0.5 MB/s). Since then: swap
    grows with our runs and doesn't come back when idle (0 -> 382 MB over 9
    runs, 418 MB after 18; idle system RSS 1194 -> ~600 MB), the prime
    suspect; elfldr keeps ~145 of 160 MB FMEM after taking a 129 MB ELF;
    FTP downloads run at ~2.5 MB/s right after another transfer and 8 MB/s
    after an idle gap (a timing effect, also on a fresh boot); page tables
    and the kernel's BUDGET_FD_FILE count return to their floor after every
    run. The harness now uploads ps5wk-shot once and starts it from the
    console's disk (`test/shotrelay`), so no large payload is sent from the
    Mac to elfldr.
- **Feed pages: decoders stalled by paused and hidden videos (2026-11-01,
  checked on the console by log; port/webkit/README.md, *Feeds with several
  videos*).** On a feed with several autoplaying videos some spun forever or
  crawled, and the title took back 6 decoded pictures every 1.5-2 s (34
  times over 24 decoders in the owner's session, 0 decode errors). The
  pictures were legitimately held: a paused, prerolled or hidden player
  (WebKit suspends muted videos outside the viewport: non-MSE to NULL, MSE
  to PAUSED with its decoder) keeps its sink's preroll, playsink's queue and
  decoded-but-unread pictures, and the title's 1.5 s safety net took back
  pictures the WebProcess still owned, so resumed players showed a frozen
  start and dropped 19-65 frames while the decoder threw runs of pictures
  away. Now a picture is the WebProcess's until the sink hands it to the
  title or it is given back, and every unrendered placeholder buffer gives
  its picture back when GStreamer frees it (wherever that happens), so the
  timeout is gone; releases free exactly one picture; the proxy's reply
  waits survive signals. A synthetic feed page (`test/media/feed.html`,
  seven players: MSE and progressive, paused, seeking, scrolled, removed):
  0 take-backs (10 and 15 before), 0 frames dropped by players resumed from
  a paused preroll (19, 25, 19 before), 0 pictures lost without a release.
  Found by the same page, still open: the WebProcess main thread hung in
  WebKit's resume-from-suspend seek (qtdemux's stream lock) in all three
  runs served over http, before and after the fix and with software
  decoding (not in the owner's session); progressive A/V clips drop ~41
  frames when they first play. The HSTS console check was not run: FTP
  downloads fell to 1-2.3 MB/s even after an idle gap (7-8 normally), so
  console work stopped there.
- **Feed pages: spinners over playing videos, and the resume freeze
  (2026-11-01, patch 0036, checked on the console by log;
  port/webkit/README.md, *Feeds: spinners over playing videos*).** With
  decoding healthy, some feed videos still kept the page's spinner up
  while they played. A synthetic feed (`test/media/feed-players.html`: MSE
  and progressive players, muted autoplay through the attribute and
  play(), IntersectionObserver play/pause, six spinner models computed
  from standard signals) found four faults in what the page reads:
  `requestVideoFrameCallback` never called back (frames were only counted
  on the accelerated path, not with the hole-punch sink or CPU painting);
  `readyState` stuck at 2 while playing after a loop, a seek or WebKit's
  off-screen suspend/resume (a buffering change held back during an async
  state change was dropped, and an old one was "delayed" again by every
  later state change), so `waiting` came and `playing` never did; looping
  MSE videos stuck in `seeking` (the MSE player tore its pipeline down
  under the loop's seek); and the page freeze over http, which a new
  all-threads wall-time profile (`PS5WK_PROFILE=threads`) showed to be a
  deadlock: seamless looping's non-flushing seek waits for qtdemux's pull
  loop, which waits for bytes that only the blocked main thread delivers
  (upstream's guard against it only works with playbin2). Fixes: count
  frames in `triggerRepaint`; apply held-back buffering changes when the
  state change completes, hold back only new ones; keep the MSE pipeline
  when the ended notification started a seek; no seamless looping for
  http(s) progressive media. The `PageConsentRequired` lines were not the
  policy (the title allows autoplay, muted or not; no play() promise was
  rejected): WebKit prints them when the `autoplay` attribute path has
  nothing to do (no attribute, play() already called, already playing);
  now printed only for a real wait. Before/after on the test page:
  spinner-over-playing-video events 22 -> 1 (progressive WebM reports empty
  `buffered`, open), readyState-2-while-playing samples 20 -> 0, `seeking`
  / `seeked` 6/2 -> 11/11, PageConsentRequired lines 19 -> 0, and the page
  ran to the end in 3 runs instead of freezing at the first resume. Console
  steady over the round: swap out 386-392 MB (no growth), page tables back
  to 4972 after every run.
