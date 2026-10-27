# Spike 5: libopenxr_psvr2, a minimal OpenXR runtime on PS VR2

WebKit's OpenXR backend (`Source/WebKit/UIProcess/XR/openxr`) is left as it is.
This spike adds a small OpenXR runtime that implements what that backend
calls and runs on `common/psvr2` (Hmd2 reprojection + VrTracker2). It is plain
C11, uses one global lock, and has two backends behind `runtime/backend.h`:

| backend | where | head pose | frames | swapchain images | submit |
|---|---|---|---|---|---|
| `backend_mock.c` | host / headless | ±30° yaw sweep (0.1 Hz) + 1 cm bob, standing 1.65 m above a known floor | 60 Hz monotonic clock, same schedule as `psvr2_begin_frame` | fake GL names (1000, 1001, ...) | validates and counts |
| `backend_psvr2.c` | PS5 (compiles, not run) | `psvr2_begin_frame` poses | `psvr2_begin_frame` (60 Hz slots, 120 Hz out) | `glTexStorage2D` textures in the app's EGL context | flipping blit into 3 present buffers, then `psvr2_submit` |

OpenXR headers: KhronosGroup/OpenXR-SDK tag **release-1.1.65** (commit
`24a1f2b92d212501a2b1b8d30ffd53abf55fcfa8`), copied to
`toolchain/khronos/include/openxr/` (`openxr.h`, `openxr_platform.h`,
`openxr_platform_defines.h`, `openxr_reflection.h`,
`openxr_loader_negotiation.h`; see `SOURCE.txt` there).

## Build and run

```
make                 # host dylib + static lib + tests (mock backend)
make test            # dlopen + xrNegotiateLoaderRuntimeInterface (300 frames), then static link (120 frames)
make loader-test LOADER_DIR=<dir of libopenxr_loader.dylib>   # through the official Khronos loader
. ../../env.sh && make ps5   # prospero-clang: build/ps5/libopenxr_psvr2.a (runtime + both backends + common/psvr2)
```

Logs go to `logs/`. Collect them with
`tools/results.sh spikes/05-openxr-runtime/logs/*.log`. Environment variables:
`XR_PSVR2_LOG=0..3` sets verbosity; `XR_PSVR2_BACKEND=mock` selects the mock
backend in the PS5 build (useful for WebKit bring-up without a headset);
`XR_PSVR2_MOCK_CONTROLLERS=1` makes the mock report two active controllers.

## Results (host, 2026-10-27)

All 18 checks pass in all three modes: dlopen + negotiate, static link, and
the official loader. ASan/UBSan is clean.

```
RESULT 05-openxr-runtime negotiate PASS xrNegotiateLoaderRuntimeInterface: interface 1, API 1.1.65; interface 99 refused
RESULT 05-openxr-runtime extensions PASS 4: XR_KHR_opengl_es_enable XR_MNDX_egl_enable XR_EXT_local_floor XR_EXT_hand_tracking
RESULT 05-openxr-runtime instance PASS libopenxr_psvr2 (mock) 0.1.0, API 1.1, 3 extensions enabled
RESULT 05-openxr-runtime system PASS "PS VR2 (mock)" vendor 0x054c, orientation 1 position 1 hands 0, 1 view config(s), 2 views rec 1408x1536 (WebKit recommendedResolution 2816x1536), max layers 16, blend OPAQUE
RESULT 05-openxr-runtime session PASS XrGraphicsBindingEGLMNDX (null EGL handles), GLES 3.0-3.2
RESULT 05-openxr-runtime input-setup PASS 2 action sets x (2 pose + 21 button + 2 axis actions), simple_controller bindings, attached
RESULT 05-openxr-runtime swapchain PASS formats RGBA8+SRGB8_ALPHA8 listed, 2816x1536 RGBA8 x3 images (GL names 1000..1002)
RESULT 05-openxr-runtime frame-timing PASS 300 frames, 59.99 fps, xrWaitFrame interval mean 16.66 ms (min 8.47 max 21.63), 0 late >25 ms, predictedDisplayTime off the 16.667 ms grid 0, slots skipped 0, shouldRender=false 0
RESULT 05-openxr-runtime pose-continuity PASS max per-frame step 0.52 mm / 0.314 deg, yaw swept -2.2..29.1 deg
RESULT 05-openxr-runtime views-fov PASS IPD 64.0 mm, L fov l/r/u/d -56.2/49.0/54.1/-56.2 R -49.0/56.2/54.1/-56.2 deg, local=origin*viewer mismatches 0, ipd 0, symmetric-fov 0
RESULT 05-openxr-runtime swapchain-cycling PASS 300 frames, image index round-robin over 3, 0 out of order; submitted layout SBS_EXACT
RESULT 05-openxr-runtime spaces PASS VIEW/LOCAL + floor via LOCAL_FLOOR: floor 1.652 m below LOCAL
RESULT 05-openxr-runtime layouts PASS SBS one swapchain 2816x1536 (WebKit) ok; two swapchains 1408x1536 -> 0 (TWO_SWAPCHAINS); scaled SBS 2112x1152 -> 0 (SBS_SCALED)
RESULT 05-openxr-runtime input-inactive PASS 12600 action-state queries, 0 active; aim/grip located 0/0; profile ""
RESULT 05-openxr-runtime state-machine PASS IDLE>READY>SYNCHRONIZED>VISIBLE>FOCUSED>VISIBLE>SYNCHRONIZED>STOPPING>IDLE>EXITING
RESULT 05-openxr-runtime errors PASS 38/38 expected XrResult codes
RESULT 05-openxr-runtime input-plumbing PASS XR_PSVR2_MOCK_CONTROLLERS=1: 1 profile event(s), profile "/interaction_profiles/khr/simple_controller", select transitions 3 (changedSinceLastSync 6), aim/grip located 180/180 of 180, 180 active of 3780 action states (unbound stay inactive)
RESULT 05-openxr-runtime spaces-api1.0 PASS apiVersion 1.0, XR_EXT_local_floor not enabled (as WebKit): floor via STAGE->LOCAL offset, 1.651 m below LOCAL
```

On the host, the frame-interval spread (8 to 22 ms around a 16.67 ms mean)
comes from macOS timer slack. The schedule stays on its fixed 60 Hz grid: no
predicted display time fell off the grid. The test raises its own QoS the way
WebKit's XR WorkQueue does. Without that, macOS coalesces sleeps by up to 12 ms.

**Official loader:** Homebrew has no `openxr-sdk` formula. The OpenXR-SDK
release-1.1.65 loader builds on macOS with plain CMake
(`-DDYNAMIC_LOADER=ON -DBUILD_TESTS=OFF`, about a minute). With
`XR_RUNTIME_JSON=openxr_psvr2_host.json`, the loader negotiates with the dylib
and the whole WebKit sequence runs through it, all checks passing (the loader
adds its own `XR_EXT_debug_utils`). The stale-handle checks are skipped in this
mode: passing destroyed handles through the loader is undefined behaviour, and
its dispatch lookup crashes on them. hello_xr was not tried: it needs a
graphics plugin we cannot satisfy headless.

**PS5:** `make ps5` builds all 7 objects (runtime, both backends,
`common/psvr2/psvr2.c`) with prospero-clang 18 at `-Wall -Wextra -Werror`, into
`build/ps5/libopenxr_psvr2.a` (ELF x86-64). Its external symbols are: libc,
pthread and libm; GL/EGL (ps5-opengl); `sceKernel*`, `sceUserService*`,
`sceVideoOut*` and `sceSysmoduleLoadModule`. It has not been linked into a
payload or run.

## Implemented subset compared with WebKit's calls

"WebKit" is where the call is made in `UIProcess/XR/openxr`.

| Function | WebKit | Status |
|---|---|---|
| xrGetInstanceProcAddr | OpenXRExtensions::loadMethods | Implemented. Gates extension entry points on enabled extensions; NULL instance is allowed only for the 3 global functions. |
| xrEnumerateInstanceExtensionProperties | OpenXRExtensions() | Implemented, with the two-call idiom. |
| xrCreateInstance / xrDestroyInstance | createInstance / cleanup | Implemented. Accepts API 1.0 and 1.1 (WebKit passes `XR_CURRENT_API_VERSION`). One instance at a time. |
| xrGetSystem / xrGetSystemProperties | initializeSystem / systemProperties | Implemented. HMD only. Opens the headset here. Fills the `XrSystemHandTrackingPropertiesEXT` chain with `supportsHandTracking = false`. |
| xrEnumerateViewConfigurations / ...Views | collectViewConfigurations | Implemented. PRIMARY_STEREO, recommended size from the tangents (square pixels at 1536 rows). |
| xrEnumerateEnvironmentBlendModes | initializeBlendModes | Implemented. OPAQUE only. |
| xrGetOpenGLESGraphicsRequirementsKHR | initializeForSession | Implemented. GLES 3.0 to 3.2. Required before xrCreateSession (otherwise `GRAPHICS_REQUIREMENTS_CALL_MISSING`). |
| xrCreateSession / xrDestroySession | createSessionIfNeeded / cleanup | Implemented. Needs `XrGraphicsBindingEGLMNDX` (KHR_opengl_es + MNDX_egl enabled). The mock accepts NULL EGL handles; psvr2 requires a display and a context. |
| xrBeginSession / xrEndSession / xrRequestExitSession | handleSessionStateChange / endSessionIfExists | Implemented. Full state machine: IDLE → READY at create, SYNCHRONIZED at begin, VISIBLE → FOCUSED after the first xrEndFrame (FOCUSED ↔ VISIBLE follows `psvr2_worn()`). Exit request walks FOCUSED → VISIBLE → SYNCHRONIZED → STOPPING; xrEndSession then gives IDLE → EXITING. |
| xrPollEvent | pollEvents | Implemented. Per-instance ring of 32 events, `EVENTS_LOST`. Events: session state, interaction profile changed. |
| xrEnumerateReferenceSpaces / xrCreateReferenceSpace / xrDestroySpace | createReferenceSpacesIfNeeded | Implemented. VIEW, LOCAL, STAGE; LOCAL_FLOOR when XR_EXT_local_floor is enabled or API ≥ 1.1. |
| xrLocateSpace (+ 1.1 xrLocateSpaces) | populateFrameData, input poses | Implemented. Relative poses with intersected location flags. Velocity flags are always 0. |
| xrLocateViews | populateFrameData (×2: LOCAL and VIEW) | Implemented. Per-eye poses from the backend; asymmetric FOV = atan of the psvr2 tangents. |
| xrWaitFrame / xrBeginFrame / xrEndFrame | beginFrame / endFrame | Implemented. Paced by the backend; `shouldRender` is true in VISIBLE/FOCUSED. Returns `FRAME_DISCARDED` and `CALL_ORDER_INVALID` per the spec. xrEndFrame validates every layer before submitting. |
| xrEnumerateSwapchainFormats | collectSwapchainFormatsIfNeeded | Implemented. Returns GL_RGBA8, GL_SRGB8_ALPHA8, GL_DEPTH24_STENCIL8, GL_DEPTH_COMPONENT24. |
| xrCreateSwapchain / xrDestroySwapchain / xrEnumerateSwapchainImages | OpenXRSwapchain::create, enumerateSwapchainImages | Implemented. 3 images (1 if static); 2D, 1 sample, 1 layer, 1 face; up to 16384 px. Uses `XrSwapchainImageOpenGLESKHR`. |
| xrAcquire / xrWait / xrReleaseSwapchainImage | OpenXRSwapchain::acquireImage / releaseImage | Implemented. FIFO round-robin; `CALL_ORDER_INVALID` on misuse. |
| xrStringToPath / xrPathToString | OpenXRInput(Source) | Implemented. Per-instance interning with path well-formedness checks. |
| xrCreateActionSet / xrDestroyActionSet / xrCreateAction | OpenXRInputSource::initialize | Implemented, with name and localized-name validation and subaction paths. |
| xrCreateActionSpace | createActionSpace | Implemented. Pose comes from the bound grip or aim source of an active controller. |
| xrSuggestInteractionProfileBindings | OpenXRInput::initialize | Implemented. simple_controller is validated and used; other profiles defined in the spec are accepted and ignored. |
| xrAttachSessionActionSets | OpenXRInput::initialize | Implemented. Resolves the bindings. |
| xrSyncActions | collectInputSources | Implemented. Polls the backend controllers and returns `SESSION_NOT_FOCUSED` when not focused. |
| xrGetActionStateBoolean / Float / Vector2f | collectButton / collectAxis | Implemented, with `isActive`, `changedSinceLastSync`, `lastChangeTime` and subaction filtering. |
| xrGetCurrentInteractionProfile | updateInteractionProfile | Implemented. Returns simple_controller while that hand's controller is active, otherwise XR_NULL_PATH. |
| xrCreateHandTrackerEXT / xrLocateHandJointsEXT / xrDestroyHandTrackerEXT | OpenXRInputSource (WEBXR_HANDS) | **Stub.** The extension is advertised, the system reports no support, and create returns `FEATURE_UNSUPPORTED`. |
| (rest of 1.0 core) | not called by WebKit | Implemented so the loader's dispatch table is complete: xrGetInstanceProperties, xrResultToString, xrStructureTypeToString, xrEnumerateApiLayerProperties, xrGetViewConfigurationProperties, xrGetReferenceSpaceBoundsRect (`SPACE_BOUNDS_UNAVAILABLE`), xrGetActionStatePose, xrDestroyAction, xrEnumerateBoundSourcesForAction, xrGetInputSourceLocalizedName, xrApply/StopHapticFeedback (no-op). |
| xrGetRuntimeStatsPSVR2 | private | Frame, submit and layout counters, used by the smoke test. |

**Extensions: what WebKit needs and what we offer.**

- **Required:** `XR_KHR_opengl_es_enable` (always requested) and
  `XR_MNDX_egl_enable` (non-Android: `initializeForSession` fails without it).
  Both are implemented.
- **Optional, offered:**
  - `XR_EXT_hand_tracking`: a stub. WebKit RELEASE_ASSERTs that all three
    functions resolve whenever the extension is listed; they do.
  - `XR_EXT_local_floor`: offered. WebKit checks only that it is *supported*
    and never enables it. On API 1.0 we therefore list LOCAL_FLOOR only if the
    extension is enabled, and WebKit falls back to STAGE → LOCAL+offset, which
    the `spaces-api1.0` check covers. On API 1.1 LOCAL_FLOOR is core, and WebKit
    uses it directly.
- **Optional, not offered:**
  - `XR_EXT_hand_interaction` (and with it WebKit's `hand_interaction_ext`
    profile and pinch/poke spaces).
  - `XR_KHR_composition_layer_{equirect2,cylinder,cube}`: WEBXR_LAYERS only.
  - `XR_MSFT_unbounded_reference_space`.
  - `XR_ANDROID_trackables` and `XR_ANDROID_raycast`: hit test.

**Interaction profiles:** `OpenXRInputMappings.h` maps only
`/interaction_profiles/khr/simple_controller` (trigger → `/input/select/click`,
plus grip and aim poses) and `ext/hand_interaction_ext`. The runtime drives
state from simple_controller.

**Stubbed or simplified:**
- Quad layers are validated but not composited, and only the first projection
  layer is shown; the rest are counted as `layers_ignored`.
- Haptics are a no-op.
- Hand tracking reports no support.
- Velocities are never valid.
- No recentering events and no LOSS_PENDING.
- Texture-array, cube and multisample swapchains return `FEATURE_UNSUPPORTED`.
- `xrGetReferenceSpaceBoundsRect` returns `SPACE_BOUNDS_UNAVAILABLE`.
- Action spaces whose action set was destroyed stop locating (the spec keeps
  them alive; WebKit destroys them in the same breath).

## How the PS5 backend maps frames and swapchains onto psvr2

- **xrGetSystem:** `psvr2_preload()`, `sceUserServiceGetInitialUser`, then
  `psvr2_open(user)`. Render tangents come from `psvr2_eye_tangents(eye, 1.15)`
  (Spike 3b's overscan). The recommended view is
  `align16(1536·(l+r)/(u+d)) × 1536`. The tracker's floor height is unknown, so
  STAGE assumes the head starts 1.6 m above the floor (as Spike 3b's world does).
- **xrCreateSession:** stores the `XrGraphicsBindingEGLMNDX` display and
  context. Each GL call site makes that context current if it is not already
  (WebKit's render queue already has it current), and restores the previous one.
- **xrBeginSession:** creates three present buffers of `(2·eye_w) × eye_h`
  RGBA8, using the same hand-off decision as Spike 3b:
  - **zerocopy** if `eglExportTextureDescriptorPS5` resolves (through the
    binding's `getProcAddress`, then `eglGetProcAddress`): GL textures exported
    as T#s.
  - **copy** otherwise: linear direct memory (`psvr2_direct_alloc`), linear T#s
    from `psvr2_build_tsharp_rgba8`, and a GL staging texture.

  Then `psvr2_start(T#s, 3, eye_w, eye_h)`, which enters 120 Hz VR mode with
  half-rate rendering. VR mode is entered at session begin, independent of
  WebKit's swapchains, so tracking and pacing work before the first layer
  arrives.
- **xrWaitFrame:** `psvr2_begin_frame`, which waits for the 60 Hz slot, begins
  the Hmd2 frame and fetches the pose at the predicted display time. Frames go
  into a ring keyed by display time. `XrTime` is `sceKernelGetProcessTime()` µs
  × 1000, the same clock psvr2 uses. `predictedDisplayPeriod` is 16.67 ms.
- **Swapchains:** `glGenTextures` + `glTexStorage2D(format, w, h)` in the app's
  context. The images are the GL names, which WebKit exports to the web process.
- **xrEndFrame:** takes the first projection layer. For each eye, one
  `glBlitFramebuffer` copies the released image's `imageRect` into that eye's
  half of present buffer `frame % 3`, **flipped vertically**: GL's row 0 is the
  bottom, while Hmd2 reads memory row 0 as the top (Spike 3b renders with
  `flip_y` for the same reason). The blit uses LINEAR filtering when the sizes
  differ, and runs with scissor and `GL_FRAMEBUFFER_SRGB` disabled so the bytes
  are copied raw. The zerocopy path ends with `glFinish`; the copy path does a
  `glReadPixels` into the linear buffer. Then
  `psvr2_submit(b, frame-for-displayTime, tangents-from-layer-fov)`.

  A frame with no projection layer re-submits the last buffer with the pose it
  was rendered with (the reprojection corrects it), or black buffer 0. This
  keeps one submit per `psvr2_begin_frame`.

**Cost of the blit.** WebKit's layout is one 2816×1536 swapchain with
side-by-side halves (`SBS_EXACT`). It always needs the flip blit: about 17 MB
read plus 17 MB written per frame, roughly 0.1 ms of PS5 bandwidth. Two
swapchains and scaled SBS (framebufferScaleFactor ≠ 1) take the same path at
the same cost. The copy fallback adds a 17 MB `glReadPixels`; Spike 3b measures
it. Skipping the blit (handing WebKit's swapchain images to Hmd2 directly)
would need a flipped UV rect in the Hmd2 layer. The +0x28.. rect fields in
`psvr2_submit` might allow negative heights, but that is unknown.

## WebKit expectations that are hard to meet on PS5 / PS VR2

1. **Texture export to the web process (biggest gap, WebKit side).**
   `OpenXRGraphicsBindingOpenGLES::initializeDisplay` needs one of: an Android
   EGL; `EGL_KHR_platform_gbm`; or `EGL_MESA_platform_surfaceless` with
   `MESA_image_dma_buf_export`. `exportTexture2D` needs dma-buf or GBM. On PS5
   (ps5-opengl EGL) none of these exist. The XR device would not even be
   reported, and `startFrame` would return no layer data. Spike 6/7 needs a PS5
   path in the binding: for example, share swapchain textures through
   `eglExportTextureDescriptorPS5` handles over direct memory, or render
   in-process. Frame fences (`waitFrameFence` imports an FD) need an equivalent
   too. The runtime itself is not affected: it creates plain GL textures in
   whatever context WebKit passes.
2. **Image orientation:** see the blit cost above. It is cheap, but it rules
   out zero-copy straight from WebKit's swapchain.
3. **Frame rate:** WebKit sees a 60 Hz `predictedDisplayPeriod`, which is the
   app cadence psvr2 schedules. The panel runs at 120 Hz and the reprojection
   shows each frame twice. WebXR has no say in this (`supportedFrameRates` is
   not exposed by WebKit's OpenXR path). Full-rate 120 Hz rendering would need
   `sceHmd2ReprojectionSetRenderConfig(0, …)` in psvr2 and is out of scope.
4. **Pose at arbitrary times:** psvr2 exposes only the pose of the frame it
   began. `VrTracker2GetResult` with a time stays internal to `psvr2.c`. Every
   `xrLocate*` therefore uses the latest frame's pose. WebKit locates only at
   `predictedDisplayTime`, so this is fine in practice.
5. **sRGB:** WebKit always picks GL_RGBA8 and writes display-referred values;
   Hmd2 samples a UNORM 8888 T#. A raw copy is correct, and SRGB8_ALPHA8 is
   copied raw as well. Whether Hmd2 expects gamma-encoded input is Spike 3b's
   visual check.
6. **Phantom controllers (WebKit quirk):** `OpenXRInputSource::collectButton`
   treats `XR_SUCCEEDED(xrGetActionState*)` as "has a value" and never looks at
   `isActive`. With no controllers, WebKit still reports two tracked-pointer
   input sources with identity poses and no profiles. The same happens on any
   conformant runtime. Fix it in WebKit (check `isActive`, or skip sources whose
   interaction profile is NULL), not by bending the runtime.
7. **Desktop GL vs GLES:** WPE builds the GLES binding
   (`XR_USE_GRAPHICS_API_OPENGL_ES`). ps5-opengl is desktop GL 4.6 core. The
   runtime only calls entry points common to GL 4.6 and GLES 3.0, plus
   `GL_FRAMEBUFFER_SRGB` (desktop only; on GLES the error is cleared). The
   GLES flavour of ps5-opengl is another agent's work in
   `02-gl-egl/ps5-opengl/build/gles`.

## Controllers and hand tracking: what is still needed

The action, binding and state plumbing is complete and tested
(`input-plumbing`, using synthetic controllers). Only `ps_controller()` in
`backend_psvr2.c` is empty. It needs PS VR2 Sense data, which psvr2 does not
expose yet. From PS5VR `app/src/app_vr.c` (around lines 325–420):

- Open the pads: `scePadOpen(user, 3 /*left*/ or 4 /*right*/, 0, NULL)`.
- Register them with the tracker: `sceVrTracker2RegisterDevice(2, left_pad)`
  and `sceVrTracker2RegisterDevice(1, right_pad)` (0 is the headset).
- Pose: `sceVrTracker2GetResult` with the pad handle in the param block (+4)
  and the time (+0x10). Tracked flag at +4, position at +0x60, quaternion at
  +0x70, in the same tracking space as the head.
- Buttons: `scePadVrControllerReadState(pad, st[0x68])`. Buttons at +0, stick
  bytes at +4/+5, connected flag at +0x40.
- Mapping to `rt_controller_t`: trigger → `select` / `trigger` (WebXR trigger,
  `generic-button`); options → `menu`; grip = tracker pose; aim = grip × a fixed
  forward/down offset (to be tuned on hardware).
- Thumbsticks and face buttons can reach WebXR only after WebKit adds a mapping
  in `OpenXRInputMappings.h` for a richer profile (for example
  `oculus/touch_controller`, or a PS VR2 Sense profile), plus runtime bindings
  for it.
- Haptics: `scePadSetVibration*` for the pads, `sceHmd2SetVibration` for the
  headset.
- Hand tracking: PS5VR drives `libSceVrHand` from the headset cameras. Wiring
  it would mean `supportsHandTracking = true` and filling 26 XR_EXT_hand_tracking
  joints per hand from it.

## Suggested changes to common/psvr2 (not made here)

1. `psvr2_pose_at(uint64_t time_us, psvr2_frame_t *out)`: expose the existing
   static `pose()` query for arbitrary times, so xrLocateSpace is exact
   (expectation 4 above).
2. `psvr2_controllers_open(user)` and
   `psvr2_controller(side, time_us, pose, buttons, stick)`, per the previous
   section.
3. Optionally, a flag or UV rect on `psvr2_submit` for a vertically flipped
   source, if the Hmd2 layer supports it. That would allow zero-copy straight
   from GL swapchains, without the blit.
4. A way to read the tracker's floor height, if VrTracker2 has one, to replace
   the 1.6 m assumption.

## How WebKit links it on the PS5

There is no loader, and everything is one process: WebKit's OpenXR
coordinator lives in the UI process, which is also the registered title that
may open Hmd2.

- `build/ps5/libopenxr_psvr2.a` **exports the core `xr*` symbols directly**
  (`XRRT_EXPORT_CORE=1`, the default), so WebKit's direct calls (`xrCreateInstance`,
  `xrWaitFrame`, ...) link against it. Extension functions come through
  `xrGetInstanceProcAddr` exactly as WebKit already fetches them. The static
  smoke test (`xr_smoke_static`) proves this configuration on the host.
  `xrNegotiateLoaderRuntimeInterface` is also exported, for loader use on
  desktop. Do not link a real `libopenxr_loader` into the same binary: its
  symbols would clash.
- **CMake:** WPE's `OptionsWPE.cmake` does `find_package(OpenXR REQUIRED CONFIG)`
  and defines `XR_USE_PLATFORM_EGL` and `XR_USE_GRAPHICS_API_OPENGL_ES`. Pass
  `-DOpenXR_DIR=<repo>/spikes/05-openxr-runtime/cmake`. That
  `OpenXRConfig.cmake` defines `OpenXR::openxr_loader` as the static archive,
  with headers from `toolchain/khronos/include` and `OpenXR_VERSION` 1.1.65.
  The final link must also provide ps5-opengl (GL/EGL), `SceVideoOut`,
  `SceSysmodule`, `SceUserService` and `kernel`.
- **Startup:** call `xr_psvr2_preload()` (declared in `runtime/openxr_psvr2.h`)
  at the top of the UI process's `main()`. Hmd2 and VrTracker2 must load while
  the sandbox library path still resolves. This is a one-line WebKit/launcher
  patch. If the app skips it, `xrGetSystem` calls `psvr2_preload()` itself.
  That works only if it is still early enough, and psvr2 caches the outcome, so
  a late failure is final.
- `psvr2_close()` runs in `xrDestroyInstance`, as VrTracker2's destructor
  requires. WebKit destroys the instance in `stopWhenIdle` and in
  `~OpenXRCoordinator`. If the process can exit without either, add an
  `atexit` hook.

## Files

- `runtime/xr_internal.h`, `xr_math.h`: object model, handle table, lock, pose
  maths.
- `runtime/runtime.c`: instance, system, paths, events, dispatch, loader
  negotiation, extension stubs.
- `runtime/session.c`: session state machine, spaces, views, frame loop, layer
  validation and layout classification.
- `runtime/swapchain.c`: formats, swapchains, acquire/wait/release.
- `runtime/input.c`: action sets, actions, bindings, sync and state, profiles,
  haptics.
- `runtime/backend.h`, `backend_mock.c`, `backend_psvr2.c`: device backends.
- `runtime/openxr_psvr2.h`: public extras (`xrGetRuntimeStatsPSVR2`,
  `xr_psvr2_preload`).
- `test/xr_smoke.c`: the replay of WebKit's call sequence (dlopen, static and
  loader modes).
- `openxr_psvr2_host.json`: host runtime manifest for `XR_RUNTIME_JSON`.
- `cmake/OpenXRConfig.cmake`: lets WebKit's `find_package(OpenXR CONFIG)` find
  this runtime.
- `Makefile`, `logs/`.
