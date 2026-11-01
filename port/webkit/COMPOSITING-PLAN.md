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

## M1 results (2026-11-01)

Log-only PPSA99020 runs of the WC build (`build-wc`), compositing switch
off then on, `test-urls.txt` + `debug-screenshot` (960x540 captures of the
composed back buffer at +2 s and +10 s). Diff = pixels whose largest channel
difference is over 24 (and over 64), CPU path vs WC:

| page | > 24 | > 64 | what differs |
| --- | --- | --- | --- |
| example.com | 0.00 % | 0.00 % | nothing |
| `test/paint/static.html` (sticky header, shadows, rotated/translucent cards, SVG) | 0.00-0.02 % | 0.00 % | antialiasing only |
| BBC News | 0.01 % (same stories); 1.9 % in a later run | 0.00 / 1.5 % | a story changed between runs |
| Wikipedia article | 22 % | 10 % | the donation banner was served to one run only; the rest is identical |
| MDN docs page (sticky header and sidebar) | 5.6-7.6 % | 5.4-6.9 % | different ads; layout identical |
| `test/paint/layers.html` (new: fixed header/badge, sticky headings, 2D/3D/will-change transforms, opacity over stripes, overflow scroller, rounded clip, filters, blend) | 1.07 % | 0.63 % | see below |

`layers.html`: the `perspective` + `rotateY` box is drawn in 3D by WC (the
CPU path flattens it: WC is right); `scale(1.15) translateZ(0)` text is a
little softer (WC paints the layer at 1x and scales the texture; no
contents scale for transformed layers yet); the overflow scroller's text
sits ~1 px lower (subpixel offset of the composited scrolled contents).
Opacity and alpha over stripes, rounded clips, blur/grayscale/drop-shadow
and multiply blending match.

After scrolling to 1520 px in 40 px steps (`scroll-to` capture at +10 s),
`layers.html` and Wikipedia are identical to the CPU path (0.00 %); MDN
differs only by its ad-dependent content offset.

**Bugs found and fixed** (branch `ps5-wc`):
- Scrollbars were missing in WC mode (page and overflow): the coordinated
  graphics scrollbar controller leaves composited scrollbar layers to a
  scrolling thread that WC doesn't have. `WebChromeClient::ensureScrollbarsController`
  uses the generic controller under DrawingAreaWC, and `ScrollbarThemeAdwaita::paint`
  paints when the controller draws into scrollbar layers.
- A layer that stopped and restarted drawing showed nothing (the scene drops
  its backing store meanwhile): `GraphicsLayerWC::setDrawsContent(true)`
  repaints all tiles.
- The host repainted the scene for updates that changed nothing composited
  (BBC sends ~60 such updates/s): skipped now (`[wc] ... N unchanged`).
- Sticky headings were one scroll step behind after the M2 change below
  (layer positions update incrementally; the sticky subtree is now marked).

**Memory** (title, same pages): flexible memory 283.0 MB free in both modes
on every page (unchanged). Direct memory 598.8-608.5 MB in use on the CPU
path, 635.7-666.8 MB in WC mode (+37 to +68 MB). GL textures (`[wc]` line)
16-43 MB per page during the screenshot round, peak 64.7 MB during the
scroll round with M2's coverage margin (Wikipedia/MDN).

**Switch:** with `/data/ps5webkit/compositing` absent the same title runs the
CPU path (`[render] painting on 4 threads`, no `[wc]` line), checked after
the WC runs too.

## M2 results (2026-11-01)

Changes (`toolchain/wpe-webkit/src-wc`, branch `ps5-wc`, commits 057ebfbb87..1b74f03f8a):
- **Coverage margin** (`GraphicsLayerWC.cpp` `updateCoverageRect`, `WCTileGrid::setCoverageRect(coverage, keep)`):
  tiles are made max(one tile row, half the visible height) above and below
  what is visible and kept one more tile row beyond that. Layer resizes keep
  the tiles whose rect didn't change (both sides; explicit removals for the
  others). A tile keeps up to 8 dirty parts and paints them separately when
  that saves half of the bounding rect.
- **rAF pacing** (`DrawingAreaWC.cpp` `DisplayRefreshMonitorWC`): a
  DisplayRefreshMonitor fired by each update's reply (sent after the swap
  that shows it) or, with nothing in flight, by a timer at most at 60 Hz.
- **No repaint of fixed/sticky content per scroll step** (`LocalFrameView::setViewportConstrainedObjectsNeedLayout`,
  `scrollContentsFastPath`; PS5 and composited scrolling only): every scroll
  step moves the layout viewport, which relaid out every fixed and sticky
  renderer, and a relayout repaints all of it (LayoutRepainter: self needs
  layout = full repaint). That repainted the composited fixed header and
  every sticky heading on each step, and on Wikipedia the whole sticky
  columns (4.7 Mpx per step). Sticky renderers now get a layer position
  update (which repaints a moved non-composited one at its old and new
  place), composited fixed ones an out-of-flow movement layout.
- `[render]`/`[render1]` lines for WC in the CPU path's format
  (`PS5WK_RENDER_LOG=2`), `PS5WK_WC_LAYER_LOG=1` per-layer paints with their
  invalidations, `[wc] ... unchanged`.

**Measured** (title, log-only, 1920x1080, `test-onload.js` =
`test/paint/scroll-raf.js`: a rAF counter, then 5 s down and 5 s up at
40 px per 16 ms tick; `PS5WK_RENDER_LOG=2`; summary by
`test/paint/title-scroll-summary.py`). Per rendering update during the
scroll; presents = updates shown per second (WC: `[wc]` presents; CPU:
`[present]` page uploads):

| page | path | updates/s | main thread ms/update (avg / p95) | Mpx painted/update (avg / p95) | presents/s | rAF/s while scrolling (avg / max) |
| --- | --- | --- | --- | --- | --- | --- |
| `layers.html` | CPU | 26.2 | 15.5 / 39.8 | 0.84 / 2.07 | 19.3 | 27.0 / 59.9 |
| | WC | 58.6 | 2.4 / 6.3 | 0.107 / 0.20 | 58.6 | 58.5 / 60.0 |
| Wikipedia article | CPU | 99.3 | 4.4 / 9.2 | 1.08 / 2.07 | 54.5 | 94.7 / 108.8 |
| | WC | 56.6 | 6.6 / 26.3 | 0.67 / 4.64 | 56.6 | 56.7 / 59.9 |
| MDN docs page | CPU | 39.4 | 12.4 / 15.5 | 2.04 / 2.07 | 44.0 | 45.5 / 97.7 |
| | WC | 59.9 | 10.2 / 14.1 | 0.30 / 0.35 | 60.2 | 59.8 / 60.3 |
| BBC News | CPU | 39.8 | 17.0 / 22.6 | 2.03 / 2.07 | 26.4 | 39.3 / 55.6 |
| | WC | 49.3 | 6.7 / 13.4 | 0.18 / 1.01 | 48.9 | 49.9 / 60.2 |

Before the fixed/sticky fix (coverage margin and pacing only) WC painted
4.7 Mpx per step on Wikipedia and 5.5 on MDN (21-29 presents/s).

- **Tile-row crossings:** steps painting ≥ 0.5 Mpx were 15 of 586
  (`layers.html`, 2.0 Mpx, 29 ms main thread), 10 of 599 (MDN, 1.2 Mpx,
  15 ms), 34 of 493 (BBC, 2.3 Mpx, 33 ms), 69 of 566 (Wikipedia, 3.1 Mpx,
  20 ms; mostly the page's own invalidations). Serial tile painting costs
  6-14 ms per Mpx on the main thread, so a crossing step misses a frame:
  that is M5 (record once, replay tiles on 0032's pool).
- Other steps: 0.03-0.33 Mpx and 1.7-10 ms. What is left is non-composited
  sticky content that moves (MDN's sidebar, Wikipedia's columns: old and new
  place repainted) and the scrollbar layer (0.02 Mpx): M3 (composited sticky).
- **Pacing:** rAF ≤ 60/s everywhere in WC (CPU path 95/s average, 109/s max
  on Wikipedia). Presents 57-60/s except BBC (49/s: main thread busy with
  the page's script, ~4 ms per update before painting, 100 ms spikes).
  `frame done` (update sent until presented) 10-15 ms.
- Pass targets: ~0 Mpx/step except crossings: yes, apart from moving
  non-composited sticky content (M3); ≥ 58 presents/s: `layers.html`,
  MDN yes, Wikipedia 56.6, BBC 49 (script-bound); rAF ≤ 60/s: yes.

**Console finding:** `/system_tmp/ps5wk` fills with the runtime sockets
and `webmem-<pid>` files of killed titles (4 per launch); after ~14
launches children can't start (ENOSPC on `listen`). The title's cleanup
logs `cannot list /system_tmp/ps5wk: errno 1` (the directory is
`drwxrwxrwx root`, the stale sockets are owned by uid 1, the webmem files by
uid 0). Cleaned over FTP during this round.

**Left for later:** M3 (sticky composited without coordinated scrolling,
transform/opacity animations), M5 (tile painting off the main thread;
crossing steps take 15-33 ms), contents scale for scaled layers (soft text
under `scale()`), video layer (M4), VR with compositing untested.

## M5 results (2026-11-01)

Changes (`toolchain/wpe-webkit/src-wc`, branch `ps5-wc`, commits 8af4f0f1d2, 2f640b9488):
- **Record once, replay on a pool** (`WebProcess/WebPage/wc/WCTilePainter.{h,cpp}`,
  one per `DrawingAreaWC`; `GraphicsLayerWC.cpp` `WCTiledBacking::paintAndFlush`
  hands it the layer's dirty parts). Per layer the dirty parts are recorded
  once on the main thread into an SkPicture (R-tree; one
  `paintGraphicsLayerContents` per rect of the parts' region, clipped), then
  each part is replayed into its slot in bands of at least 64 rows (16k px)
  on a `WorkerPool` of `PS5WK_PAINT_THREADS` (default 4) threads. Default
  asynchronous: the main thread goes back to the run loop and the update is
  sent when the last band is done (`DrawingAreaWC::sendUpdatesWC`; one update
  in flight as before). `PS5WK_WC_PAINT_ASYNC=0`: the main thread takes bands
  too and waits. Layers with under 64k dirty pixels (`PS5WK_WC_RECORD_MIN_PX`)
  are painted directly into their slots. The recording context is deferred
  (`GraphicsContextSkia` got an `IsDeferred` argument): ImageBuffers
  (canvases, scratch buffers) are recorded as copy-on-write snapshots, not
  their live pixels.
- **Shared-memory tile arena** (`WCTileArena` in `WCTilePainter.cpp`): per
  WebProcess, 8 MB `SharedMemory` chunks (`PS5WK_WC_ARENA_CHUNK_MB`), up to
  64 MB (`PS5WK_WC_ARENA_MB`); first-fit 64-byte aligned slots; chunks beyond
  the first are freed after 5 s empty and unused. A chunk goes once to each
  drawing-area proxy (`WCUpdateInfo::tileArenaChunks`, read-only handle,
  mapped once in the title and kept; freed ones in `removedTileArenaChunks`).
  Tiles carry a `WCTileArenaSlot` (chunk, offset, stride, size;
  `WCBackingStore.h`), checked against the chunk size by
  `DrawingAreaProxyWCPS5::resolveTileArena` and uploaded without a copy
  (`WCBackingStore::createNativeImage`, `WCScene.cpp`). Slots of an update are
  free again when its reply arrives (after the present, so after the upload).
  A part that finds no slot is painted the old way.
- `PS5WK_WC_TILE_PAINT=0` restores the old path (a new ImageBuffer and
  SharedMemory per part, serial). `[render1]` lines add `record`, `raster`,
  `shm` (SharedMemory allocations) and `old way`; `[render] wc tiles:` every
  5 s; `[wc] tile arena:` lines when chunks come and go.

**Measured** (title, log-only, M2's method: `scroll-raf.js`, 5 s down and up
at 40 px per 16 ms, `PS5WK_RENDER_LOG=2`; all three modes in one console
session on the same binary, then the async default again on the final
build; "crossing" = steps painting ≥ 0.5 Mpx; main thread = the whole
rendering update on the WebProcess main thread):

| page | mode | main ms/step avg / p95 | crossing steps: n, Mpx, main ms avg / p95 | presents/s | shm allocations/s (scroll; page load) |
| --- | --- | --- | --- | --- | --- |
| `layers.html` | old | 1.1 / 1.3 | 14, 2.0, 25.9 / 27.3 | 58.6 | 121.5; 551 |
| | sync | 0.8 / 1.2 | 14, 2.0, 16.7 / 17.5 | 60.0 | 0.1; 5 |
| | async | 0.5 / 1.1 (0.5 / 1.0) | 14, 2.0, 1.2 / 2.6 (1.2 / 3.2) | 60.1 (60.0) | 0.1; 5 |
| Wikipedia article | old | 3.6 / 21.9 | 67, 2.9, 16.2 / 31.0 | 58.6 | 358.8; 811 |
| | sync | 2.3 / 11.6 | 71, 2.8, 9.0 / 16.4 | 60.0 | 0.0; 2 |
| | async | 1.7 / 4.7 (1.7 / 4.9) | 66, 2.7, 4.3 / 6.7 (4.4 / 6.9) | 60.1 (60.0) | 0.0; 2 |
| MDN docs page | old | 5.5 / 7.3 | 11, 1.0, 11.3 / 14.2 | 60.0 | 189.6; 984 |
| | sync | 5.3 / 6.9 | 11, 1.0, 8.6 / 10.6 | 60.0 | 0.0; 2 |
| | async | 5.2 / 6.9 (5.3 / 6.7) | 11, 1.0, 7.0 / 8.0 (6.9 / 8.1) | 60.0 (60.0) | 0.0; 2 |
| BBC News | old | 6.8 / 17.8 | 35, 2.1, 28.2 / 89.4 | 25.3 | 120.0; 356 |
| | sync | 4.9 / 11.5 | 35, 2.3, 15.1 / 40.9 | 26.3 | 0.1; 2 |
| | async | 3.6 / 9.4 (3.1 / 5.0) | 31, 2.3, 9.4 / 26.3 (8.7 / 23.0) | 27.0 (50.2) | 0.1; 2 |

(Bracketed: the async default on the final build, a separate session.)
- Crossing steps no longer miss frames: the main thread spends 1-9 ms on
  them asynchronously (recording 0.4-5 ms of it; the rest is style, layout,
  script), against 11-28 ms the old way. Pixels painted per step are
  unchanged (same tiles, same invalidations).
- What the pool costs, wall clock per update (async): 1.9-2.6 ms per Mpx on
  Wikipedia and MDN, 7 ms per Mpx on `layers.html` (2 Mpx in 14 ms: its
  45° repeating-gradient page background is expensive per pixel and scales
  badly over threads, 25 ms serial against 16 ms on 4); BBC's page load
  batches took up to 60 ms. That time is not on the main thread and
  overlaps the previous frame's present; presents stayed at 60/s on the
  three pages that reached 60 before, and `frame done` (send to present,
  15-16 ms) did not change.
- BBC News presented 25-27/s in every mode in the first session (`frame done`
  30-45 ms in all three, i.e. on the title side, not the tiles) and 50/s in
  the second; M2 had 49/s. Not from M5 (same in the old mode); worth a look
  with M3's changes (animations, composited sticky) in the build.
- SharedMemory allocations: 112-366/s while scrolling and 356-984 per page
  load the old way (one shm object, fd and two mappings per tile part); now
  2-5 per page load (chunk growth: `layers.html` grows to 5 chunks, 40 MB,
  for its first paint) and none while scrolling.

**Correctness** (`debug-screenshot` captures, 960x540, same build): new vs
old WC path: 0.00 % pixels differ on example.com, `static.html`,
`layers.html` and Wikipedia at +2 s and +10 s. CPU path vs new WC: same as
M1 (`layers.html` 1.07 % > 24 / 0.63 % > 64, the 3D box and scaled text;
example.com and Wikipedia 0.00 %; `static.html` 0.16 % > 24 / 0.00 % > 64,
antialiasing at rows 307-332, identical between the old and new WC paths).
After scrolling to 1500 px (`scroll-to.js`, +10 s): `layers.html` and
Wikipedia 0.00 %.

**Memory and page tables:** the arena keeps one 8 MB chunk per WebProcess
(about 4-5 page tables in the WebProcess and as many in the title), up to
40 MB for ~5 s after a page's first paint. The old path mapped 1 MB per tile
part in both processes for the life of each update. The kernel's VM Stats
(sampled only on change) can't separate either: page tables 5010-5030 idle
before and after the runs, peaks 6309-6335 during runs with both paths;
swap out 444-446 MB, no growth across the second session. Direct memory in
the title is the same in both modes (`[mem]` 635.7-653.8 MB).

**Left:** the gradient-heavy replay scaling (`layers.html`); recordings
still reference ImageBuffer snapshots only, other live data (e.g. software
video frames painted into tiles) is held by reference; the arena never
shrinks below one chunk while a page is open; no cap on how long an async
batch may take before the update goes (a 60 ms batch delays that frame).

## M3 results (2026-11-01)

Changes (`toolchain/wpe-webkit/src-wc`, branch `ps5-wc`):
- **Composited sticky without coordinated scrolling** (4e0fe1c13e,
  `RenderLayerCompositor::requiresCompositingForPosition`, PS5 only): a
  sticky element with no other overflow ancestor gets its own layer when its
  frame scrolls composited (the RenderView's layer draws content), as sticky
  elements in composited overflow scrollers already did. M2's per-step
  layer position + geometry update then moves the layer on the main thread
  instead of repainting the element at its old and new place.
  `PS5WK_WC_STICKY=0` (child.env) = old behaviour. Fixed elements were
  already composited (`acceleratedCompositingForFixedPosition` follows AC in
  `DrawingAreaWC::updatePreferences`; fixed elements always form a stacking
  context; only out-of-view, empty or non-view-container ones stay painted).
- **Accelerated transform/opacity animations** (fea93260ac and follow-up):
  `GraphicsLayerWC::addAnimation/pauseAnimation/removeAnimation/
  transformRelatedPropertyDidChange` for opacity, translate, rotate, scale,
  transform (forwards, rate 1, as GraphicsLayerCoordinated; filters stay on
  the main thread). A layer's animations go whole in `WCLayerUpdateInfo`
  (`WCLayerChange::Animations`, `WCAnimations`: keyframes with timing
  functions, duration, iterations, direction, fill, start time, pause state,
  the non-animated transform parts; ~140 lines incl. serialization).
  `WCScene::apply` builds TextureMapperAnimations, `WCScene::paint` runs them
  at the frame's display time. `PS5WCHost` paints every frame while they
  run; such a frame waits (until 6 ms before it is due) for the update the
  WebProcess sends after its last reply, otherwise rendering updates drop to
  30/s (measured: 30.0 updates/s and rAF 30/s before that wait). Layers under
  a running transform animation are tiled whole (≤ 4 Mpx) so they don't move
  out of their tiles. `PS5WK_WC_ANIMATIONS=0` = main-thread animations.
  Size: ~330 lines in all.
- Tests (`port/webkit/test/paint/`): `sticky.html` (sticky header, sticky
  sidebar with its own overflow, sticky headings, sticky table header in an
  overflow scroller that follows the page scroll, fixed footer),
  `animations.html` (`?auto`: CSS animations, a CSS transition, Web
  Animations, a rAF-driven box, a 2600 px strip sliding in a 1340 px clip;
  6 s idle, 6 s with the main thread blocked 100 ms every 300 ms, 6 s idle),
  `scroll-to.js` (stop at 1520 px for captures), `shot-diff.py` (CPU vs WC
  captures), `title-anim-summary.py`.

**Sticky, measured** (title, log-only, same build, `scroll-raf.js` as in M2,
`PS5WK_RENDER_LOG=2`; off = `PS5WK_WC_STICKY=0`). Mpx painted per rendering
update during the 10 s scroll:

| page | sticky | Mpx/update avg | p50 | updates ≥ 0.5 Mpx | main thread ms avg / p95 | layers | GL textures median / max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `sticky.html` | off | 0.190 | 0.180 | 42 of 599 | 4.3 / 17.0 | 41-43 | 34.5 / 49.8 MB |
| | on | 0.103 | 0.030 | 43 of 600 | 2.1 / 12.8 | 49 | 35.6 / 49.8 MB |
| Wikipedia article | off | 0.640 | 0.410 | 64 of 569 | 6.4 / 24.9 | 10 | 30.5 / 34.2 MB |
| | on | 0.360 | 0.020 | 68 of 587 | 3.1 / 21.4 | 16 | 28.2 / 35.7 MB |
| MDN docs page | off | 0.296 | 0.320 | 10 of 597 | 10.0 / 14.1 | 81-94 | 32.3 / 38.1 MB |
| | on | 0.041 | 0.020 | 11 of 600 | 5.5 / 7.4 | 99 | 33.4 / 39.2 MB |

- Ordinary steps now paint 0.02-0.03 Mpx: the scrollbar layer (0.02) and,
  on `sticky.html`, the footer's own scrollY counter. What is left of the
  average is tile-row crossings (the ≥ 0.5 Mpx steps, unchanged: M5).
- Presents 58.8-60/s and rAF ≤ 60/s with sticky on (Wikipedia 56.9 → 58.8).
- No layer explosion: +6 to +18 layers, GL textures within ±2 MB.
- **Correctness:** CPU path vs WC (sticky on), `scroll-to.js` (stopped at
  1520 px), captures at +2 s and +10 s, `shot-diff.py`: `sticky.html`
  0.00 % at both (sticky header, sidebar, headings, the table header inside
  the scrolled overflow box and the fixed footer all in place),
  `layers.html` 0.00 % after the scroll (1.07 % before it: M1's 3D box),
  MDN 0.00 % after the scroll (ad bar before it), Wikipedia 20-22 %: the
  donation banner was served to the CPU run only and shifted the article;
  the sticky contents column and appearance panel are identical.

**Animations, measured** (`animations.html?auto`, `title-anim-summary.py`;
main thread = `PS5WK_WC_ANIMATIONS=0`, i.e. M2's state: the main thread
steps the animations and sets layer transform/opacity each frame). Per
phase; "busy" = the main thread blocked 100 ms every 300 ms:

| animations | phase | rendering updates/s | main thread ms/update avg / p95 | Mpx/update | scene paints/s (animation-only) | longest gap between painted frames | page rAF/s, longest rAF gap |
| --- | --- | --- | --- | --- | --- | --- | --- |
| main thread | idle | 59.7 | 1.6 / 2.1 | 0.028 | 59.7 (0) | (one paint per update) | 59.7, 54 ms |
| | busy | 46.7 | 1.6 / 2.3 | 0.024 | 47.2 (0) | ~117 ms (every 300 ms) | 46.7, 116 ms |
| accelerated (first version) | idle | 30.0 | 1.5 / 2.3 | 0.035 | 59.7 (29.8) | 17.7-19.7 ms | 30.0, 54 ms |
| | busy | 23.3 | 1.4 / 2.2 | 0.027 | 60.0 (36.5) | 17.6-18.9 ms | 23.3, 118 ms |
| accelerated (final) | idle | 59.5 | 1.5 / 2.0 | 0.028 | 59.5 (0) | 16.8 ms after the first second | 59.5, 59 ms |
| | busy | 40.7 | 1.4 / 2.1 | 0.024 | 60.0 (19.2) | **16.8 ms** | 40.7, 117 ms |
| | idle2 | 60.0 | 1.4 / 2.0 | 0.023 | 60.0 (0) | 16.8 ms | 60.0, 18 ms |

- Main-thread animations already repaint nothing in WC: they have their own
  layers and each frame only changes layer properties. The 0.023-0.028 Mpx
  per update is the page's rAF-driven box (a 2D `style.transform` without a
  layer, repainted at old and new place), on both paths.
- What acceleration buys is smoothness under load: with the main thread
  blocked, CSS animations, the transition and the Web Animation keep moving
  at 60 frames/s without a missed vsync (longest gap between painted frames
  16.8 ms), where the main-thread path freezes them for ~117 ms every 300 ms.
  The rAF-driven box still freezes (only the main thread can move it).
- "First version": the host painted animation frames right after each swap,
  before the WebProcess's next update arrived, so every update waited one
  extra frame (30 updates/s). The final version waits for it (≤ 6 ms before
  the frame is due): 59.5-60 updates/s again, "waited for" in 56-60 of 60
  frames/s when idle. Animation time = the frame's display time.
- The longest gap in the first second (34-73 ms) is the first use of new
  shader programs and the strip's tiles; captures (`debug-screenshot`) add a
  ~120 ms stall each, so animation runs are made without them.
- Correctness: WC captures of `animations.html` show every box and the
  sliding strip fully drawn where it has moved into what was clipped at the
  start (whole-layer tiling for animated layers).
- Memory: 19-23 layers, GL textures 16.1-17.4 MB on this page.

**Left for later / plan changes:**
- Scrolling is still main-thread: composited sticky and fixed layers move
  in each scroll step's rendering update (no repaint, but a commit per
  step). Async scrolling in the scene (scroll offset + sticky/fixed
  constraints applied in `WCScene`) is the next step if JS-heavy pages make
  scrolling stutter; the animation path (scene-side state, a frame waits
  for the update) is the model for it.
- Sticky elements inside overflow that is not a composited scroller (e.g.
  `overflow: hidden` ancestors), and in frames that don't scroll
  composited, stay painted (they don't move with the page scroll anyway).
- Not accelerated: filter animations, reverse directions and playback
  rates ≠ 1 (as GraphicsLayerCoordinated), animated layers over 4 Mpx
  (normal coverage), scroll-driven animations.
- Tile-row crossings (≥ 0.5 Mpx steps, 15-34 ms) dominate what scrolling
  still paints: M5.
- The title abort seen in the owner's TV test after these builds was the
  BitmapTexturePool release timer (fixed separately, 7d09449da9).
