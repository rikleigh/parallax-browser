# GPU compositing (then WebGL): plan

Written 2026-11-01 from a source read of WebKit 30c37bf34f (branch `ps5`).
Paths are relative to `toolchain/wpe-webkit/src` unless they start with `port/`.

## Decision

- **Host: WebKit's WC compositor (`WCScene`, `WCSceneContext`, TextureMapper)
  runs inside the title's UIProcess, on PS5Renderer's render thread, in
  PS5Renderer's own GL context.** Layer-tree updates (`WCUpdateInfo`) travel
  over the existing WebProcess↔UIProcess connection as one new async message
  to the drawing-area proxy. No GPUProcess, `ENABLE_GPU_PROCESS` stays OFF.
- An in-process GPUProcess (FEASIBILITY §4.8, R14) waits until WebGL. Everything
  built now carries over; only the transport changes (~150 lines).
- **No bitmap readback.** `WCScene::update` is split into `apply()` and
  `paint()`; the scene paints into an FBO texture that `PS5Renderer::drawScene`
  draws where the page buffer goes today (under pointer and messages).
- **Vsync pacing.** `DrawingAreaWC` sends one update and waits for the reply.
  The title replies only after the swap that shows the frame, and a
  DisplayRefreshMonitor fired from `displayDidRefresh` drives rAF (today rAF
  runs at 110–158/s while scrolling).
- **Video becomes a composited layer** drawn from PS5VideoServer's converted
  frames, keyed by a stream key the sink sends in HELLO. Hole punch stays for
  the CPU fallback.
- **CPU fallback:** AC/ForceCompositing follow a runtime switch (settings item
  `compositing`, flag file `/data/ps5webkit/no-compositing`); the UI proxy and
  `DrawingArea::create` both choose at runtime. Automatic fallback if the scene
  context fails.

## Facts it rests on

- WC is ~4.1k lines. The scene half (`WCScene`, `WCSceneContext`,
  `WCContentBufferManager`, `WCRemoteFrameHostLayerManager`) has no GPUProcess
  dependency. GPU_PROCESS-only: `RemoteWCLayerTreeHost.cpp` (:52, :66),
  `RemoteWCLayerTreeHostProxy.cpp:58`, `DrawingAreaWC.cpp:288,372`
  (`flushImageBuffers`, `WebPage.h:2084`).
- `WCScene::update` (`WCScene.cpp:99-301`): apply changes, upload tiles via
  `TextureMapperSparseBackingStore` → `BitmapTexture::updateContents`
  (`glTexSubImage2D`, `BitmapTexture.cpp:370`), paint, then readback
  (:278-296) or swap (:299).
- Tiles 512² (`WCTileGrid.cpp:89`); coverage = visible rect + 20 px
  (`GraphicsLayerWC.cpp:539`), no prepaint; each dirty tile painted serially on
  the main thread into a new ImageBuffer/SharedMemory (`GraphicsLayerWC.cpp:68`).
- No compositor animations (`GraphicsLayerWC` lacks `addAnimation`); no async
  scrolling (`DrawingArea.h:125`); sticky isn't composited without coordinated
  scrolling (`RenderLayerCompositor.cpp:4169-4174`, `:4294`).
- TextureMapper vs ps5-opengl: emits `#version 300 es` shaders for GL ≥ 3
  (`TextureMapperShaderProgram.cpp:730-747`); needs `GLContext::current()`;
  reads `glGetString(GL_EXTENSIONS)` (NULL in core, `GLContext.cpp:641`);
  `glVertexAttribPointer` without a VAO (`TextureMapper.cpp:863`);
  `GLContext::create` forces `EGL_OPENGL_ES_API` (`GLContext.cpp:268`) while
  PS5Renderer uses desktop GL 4.6 core (`PS5Renderer.cpp:363-373`). Adopt the
  existing context with `GLContext(GLDisplay&, EGLContext, EGLSurface, EGLConfig)`
  (`GLContext.h:84`) and never destroy it.
- `glTexSubImage2D` on ps5-opengl detiles on the CPU at ~210 MB/s ≈ 5 ms per
  512² tile: stock uploads are too slow (spike S1a).
- Render thread stack is 1 MiB (`PS5Renderer.cpp:266`); shader compiles need
  ~8 MiB.

## Build conflicts (USE_GRAPHICS_LAYER_WC + USE_COORDINATED_GRAPHICS + USE_SKIA on WPE)

1. `USE_TEXTURE_MAPPER` on for PS5 (`WebCore/PlatformWPE.cmake:21-23`, option at `OptionsWPE.cmake:137`).
2. `include(Platform/WC.cmake)` in `WebKit/PlatformWPE.cmake` (cf. `PlatformPlayStation.cmake:157-159`).
3. `TextureMapperTile.cpp` only built without COORDINATED (`TextureMapper.cmake:41-44`) but needed by `TextureMapperSparseBackingStore.cpp:76-81`.
4. `RemoteWCLayerTreeHost*`, `RemoteGraphicsContextGLWC.cpp` (`WC.cmake:1-20,30-32`) only with GPU_PROCESS.
5. `DrawingAreaProxyWC.cpp`, `BackingStoreSkia.cpp` (`WC.cmake:43-47`) can't build on WPE (`BackingStore.h:28`, `DrawingAreaProxy.h:66,179-180`): use a PS5 proxy.
6. `UpdateInfo` guard (`Shared/UpdateInfo.h:28`, `UpdateInfo.serialization.in:23`): add `|| USE(GRAPHICS_LAYER_WC)`.
7. `Messages::DrawingAreaProxy::Update` missing on WPE (`DrawingAreaProxy.messages.in:32-35`; sent at `DrawingAreaWC.cpp:300,390`): `#if` out `sendUpdateNonAC`.
8. `flushImageBuffers` (`DrawingAreaWC.cpp:288,372`) under `#if ENABLE(GPU_PROCESS)`.
9. `DrawingAreaWC` abstract under COORDINATED (`DrawingArea.h:172-176` vs `DrawingAreaWC.h:62-64`).
10. `DrawingArea.cpp:77-80` picks WC at compile time: make it a runtime choice.
11. `PageClient::usesOffscreenRendering` (`PageClient.h:864-866`) and `WebPageProxy::viewWidget()` (`WebPageProxy.h:1543-1545`, used at `WebPageProxy.cpp:16150`) need WPE definitions.
12. `WebPageProxy.cpp:19021` unchecked `downcast<DrawingAreaProxyCoordinatedGraphics>`: guard with `is<>`.
13. `DrawingAreaWC::updatePreferences` (:125): force compositing on PS5.
14. `GLContext::glExtensions()` (`GLContext.cpp:641`): use `glGetStringi` in core profiles.
15. WebGL phase only: `PlatformLayer` typedefs (`PlatformLayer.h:34-55`), `RemoteGraphicsContextGLProxyWC.cpp:77`, `GraphicsLayerWC.cpp:631`, `TextureMapperGCGLPlatformLayer.h:22`, `GraphicsContextGLEGL.cpp:250,297,345`.

## Transport

- `UpdateWC(struct WebKit::WCUpdateInfo) -> (bool)` in
  `DrawingAreaProxy.messages.in` under `USE(GRAPHICS_LAYER_WC) && !ENABLE(GPU_PROCESS)`
  (header in the `messages.py:804-805` table). `DrawingAreaWC` sends it or uses
  `RemoteWCLayerTreeHostProxy` behind an interface.
- UIProcess: `DrawingAreaProxyWCPS5` (`sizeDidChange` → `UpdateGeometryWC`,
  `DrawingArea.messages.in:56`; `updateWC` queues work for the host).
  `PageClientImpl::createDrawingAreaProxy` (:109-111) picks it when AC is on
  (cf. `win/PageClientImpl.cpp:57-63`). Host code under `UIProcess/wpe/ps5/`.
- wpe-ps5: `PS5Renderer::setCompositor(PS5CompositorClient*)` with
  `applyPending()`, `paint(displayUS) -> GLuint`, `didPresent()`.
- `ps5wk-shot` gets a GL-free null host (counts layers/tiles/Mpx, replies at 60 Hz).

## Render loop (PS5Renderer)

1. Apply queued `WCUpdateInfo`s in order; tile uploads via the PS5 upload path.
2. `PS5VideoServer::consume` as today.
3. If dirty / video layer / animations: bind a VAO, `TextureMapper::beginPainting(FlipY::No, sceneTexture)`, paint root, `endPainting`.
4. `drawScene()` (`PS5Renderer.cpp:868-957`): hole-punch videos (fallback only), scene texture (premultiplied) instead of the page SSBO, message, pointer.
5. `eglSwapBuffers`, then complete the replies included in that frame.
- View in VR: `vrFrame()` also applies updates and completes replies at the headset rate, skipping scene paint.

## Memory

GL textures come from direct memory. Typical 1080p page: root 8–20 MB, layers
5–30 MB, BitmapTexturePool 8 MB each (released per update), scene texture
8.3 MB. Plan 40–70 MB, log above 128 MB, hard cap 160 MB → CPU fallback.
Check room next to Videodec2 buffers (same allocator).

## Existing patches

0011 (prefs): AC/ForceCompositing follow the switch, threaded scrolling stays
off. 0014, 0016, 0032: fallback only (0032's band painting is reused in M5).
0017: punch/GEOMETRY only when non-composited; sink sends a stream key. 0020,
0036: check presented-frame counting with a video layer. 0018: fine.

## Spikes and milestones

- **S1a** tile upload microbench (standalone GL title): `glTexSubImage2D` vs
  `GL_PIXEL_UNPACK_BUFFER` vs memcpy into a persistent SSBO + FBO draw. Pass: ≤ 0.5 ms per 512² tile.
- **S1b** TextureMapper on PS5Renderer's context: compile all programs, draw a test tree, measure compile/paint time and GL errors. Fallback: shared GLES 3.2 context.
- **S2** WC build spike: the build fixes above, both ELFs link, size change recorded.
- **S3** (WebGL time): `ENABLE_GPU_PROCESS=ON` without GBM, price option A.
- **M1** static page composites (patches 0040 build, 0041 UI host, 0042 texmap). Pass: captures of 5 pages match the CPU path; `[wc]` log line; switch returns to CPU.
- **M2** scrolling without repaint + 60 Hz pacing (coverage margin, DisplayRefreshMonitor, reply after swap). Pass: ~0 Mpx/step painted except tile-row crossings; ≥ 58 presents/s; rAF ≤ 60/s.
- **M3** sticky/fixed + transform/opacity animations with 0 repaint.
- **M4** video layer + View in VR.
- **M5** tile painting cost: record once, replay per tile on 0032's pool; persistent shm tile arena.
- **M6** robustness: memory cap, WebProcess crash recovery, settings item, long session; owner's TV check.
- **W0/W1** WebGL: in-process GPUProcess, ANGLE on ps5-opengl, then WebGL 1/2. Then WebXR.

Rough total for compositing (S1–M6): 4–6 weeks.

## Risks, in order

1. Tile upload throughput (S1a). 2. TextureMapper on a desktop core context (S1b).
3. Serial main-thread tile painting, shm per tile (M2/M5). 4. Video delegate in a
COORDINATED build (M4). 5. Sticky/animations still on the main thread (M3;
upgrade path: WC accelerated animations). 6. Layer-explosion memory (M6).

## S1a results (2026-11-01)

Bench mode in the title (`wpe-ps5/PS5GLBench.cpp`, flag file
`/data/ps5webkit/glbench`; its content may name methods, empty = all): on the
render thread in PS5Renderer's context at start, before the first page. 41
distinct premultiplied BGRA 512² sources (41 MB, so out of cache), batches of
1/4/12/40 tiles per frame, then every tile drawn 1:1 into a 1080p target;
wall clock (thread CPU clocks read 0 in a title). Correctness: first and
last tile of each batch drawn nearest into 512² and read back whole: every
method matched exactly. Per tile, the same at every batch size (±2 %):

| method | upload CPU | draw CPU | GPU wait | direct memory per tile |
| --- | --- | --- | --- | --- |
| `glTexSubImage2D` GL_RGBA, 2D RGBA8 (stock path) | 4.27 ms | 0.05 ms | ~0 | 1.0 MB |
| same, GL_BGRA | 4.61 ms | 0.05 ms | ~0 | 1.0 MB |
| persistent PBO + `glTexSubImage2D` GL_RGBA / GL_BGRA | 5.04 / 5.40 ms | 0.05 ms | ~0 | 1.0 MB + staging |
| **linear 2D** (`glTexStorage2D` 2 levels) + `glTexSubImage2D` GL_RGBA, R/B `GL_TEXTURE_SWIZZLE` | **0.134 ms** | 0.05–0.08 ms | ~0 | 2.25 MB |
| linear 2D + GL_BGRA (Mesa swizzles on the CPU) | 0.41 ms | 0.05–0.08 ms | ~0 | 2.25 MB |
| **memcpy into a persistent buffer slice + FBO draw** (`unpackUnorm4x8`) | **0.195 ms** | 0.03 ms | ~0 | 1.0 MB + staging |
| same with non-temporal stores | 0.191 ms | 0.03 ms | ~0 | 1.0 MB + staging |
| memcpy into a buffer sampled as a `GL_RGBA8` buffer texture | 0.085 ms | 0.045 ms | ~0 | 1.0 MB (the buffer) |

- Raw 1 MiB memcpy into a mapped GL buffer: 0.082 ms (~12.8 GB/s); clflush
  of 1 MiB: 0.02 ms. GPU waits (glFinish after upload and after the draw)
  are 0.00–0.02 ms per tile: the GPU is not the limit anywhere.
- Why the stock path is slow: Mesa's memcpy path (`texture_subdata`) maps
  the texture; a 2D RGBA8 texture that is both sampler and render target is
  tiled (`SW_64KB_R_X`, `ps5_linear_sampled_layout`), so the map is a
  staging copy that `ps5_transfer_unmap` scatters one texel at a time
  (`ps5_tiled_color_offset` per pixel) and then flushes the whole allocation.
- PBO is not a GPU path here: `try_pbo_upload` does not engage, the PBO is
  read on the CPU and lands in the same per-texel scatter (slower by the
  extra copy).
- Linear layout: a texture with a mip chain (or RECT/array/3D) is linear;
  `glTexSubImage2D` of matching bytes is then a row memcpy. It costs 2.25 MB
  per tile: 1 MB linear + 0.25 MB unused level 1 + a 1 MB tiled
  "render staging" copy the driver allocates for every renderable linear
  texture (Mesa marks all RGBA8 textures renderable). A driver patch that
  allocates the staging copy on first render would bring it to 1.25 MB.
- Recommendation for `BitmapTexture::updateContents` on PS5: memcpy the
  dirty rect into a slot of a persistent, coherent staging buffer (ring,
  e.g. 16 MB, reused after the swap that waits for the GPU) and draw it into
  the ordinary tiled RGBA8 texture with an FBO pass that swaps B/R
  (`glBindBufferRange` the slot only: the driver flushes the whole bound
  range on every draw). Tiles stay normal render targets (BitmapTexturePool,
  copies, filters keep working), 1 MB each, no TextureMapper shader change.
  The linear-texture path (glTexStorage2D with 2 levels + swizzle) is the
  simpler fallback if 2.25 MB per tile is acceptable or the driver is patched.
