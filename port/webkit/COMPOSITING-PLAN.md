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

## M4 results (2026-11-01)

Video in WC mode, and View in VR with compositing on. Before: a composited
`<video>` showed nothing in WC mode (its contents layer was stubbed out).

**Design as built** (src-wc commits 02a3f8847b, 0e6aee371a; outer repo
8f838d5, 2078f47):
- **No contents display delegate.** Under coordinated graphics the video
  hookup is `GraphicsLayer::setContentsToMediaPlayer` (RenderLayerBacking
  calls it for a composited RenderVideo), not a delegate, so
  `GraphicsLayerWC::setContentsToMediaPlayer` (`GraphicsLayerWC.cpp:573`,
  PS5 only) asks the player for its stream key through a new PS5 virtual,
  `MediaPlayerPrivateInterface::ps5VideoStreamKey()` (`MediaPlayerPrivate.h:308`),
  and sends it with the layer's PlatformLayer change
  (`WCLayerUpdateInfo::PlatformLayerChanges::videoStreamKey`, `WCUpdateInfo.h:159`,
  serialized). Other contents (`setContentsToPlatformLayer`) clear it.
- **Stream key:** `ps5videosink` has a random, nonzero 64-bit `stream-key`
  property, fixed for the sink's life, sent in every HELLO as `pts_ns`
  (`gstps5videosink.c:258`; `PS5VideoProtocol.h`). Older sinks send 0 and
  an older title ignores the field: protocol version unchanged, both builds
  keep working (the main CPU build was rebuilt with the new archive).
  `MediaPlayerPrivateGStreamer` reads the property when it makes the sink
  and calls `renderingModeChanged()` so the layer asks again
  (`MediaPlayerPrivateGStreamer.cpp:5017`).
- **No hole punch with compositing:** with accelerated compositing
  `RenderVideo` doesn't paint a video at all; `paint()` (snapshots,
  flattening only) now paints the frame instead of punching a hole
  (`:4737`), so the sink sends no GEOMETRY in WC mode. Presented-frame
  counting (0036) also counts with compositing on while the sink is
  ps5videosink (`:4358`; it only counted when not accelerated, which WC mode
  is, so requestVideoFrameCallback would have stopped).
- **Title:** the video server stores the key per stream
  (`PS5VideoServer.cpp:169`); `PS5Renderer::compositorVideoTexture`
  (`PS5Renderer.cpp:950`, given to the compositor as
  `PS5CompositorContext::videoTexture`, `PS5Compositor.h:45`) returns the
  stream's converted RGBA texture (already converted each due frame in
  `handleVideo`) and a frame count; keys drawn as layers are skipped in the
  "videos under the page" draw (`:1016`). Streams per frame raised from 8
  to 16.
- **Scene:** a layer with a key gets a `WCScene::VideoLayer`
  (`WCScene.cpp:68`), a TextureMapperPlatformLayer whose
  `paintToTextureMapper` draws the texture with `drawTexture(GLuint)`: the
  layer's transform, opacity, rounded/contents clip and filters apply as
  for any contents layer; no frame yet = nothing (the element's background
  shows). `SceneHost::paint` repaints when `videoLayersNeedPaint()` (a video
  layer's stream has a frame, or lost its texture, since the last paint),
  without a WCUpdateInfo (`PS5WCHost.cpp:598`); `[wc] ... N video layers,
  M paints for video frames only` once a second.
- **View in VR:** nothing to change. The VR loop consumes and converts
  frames itself and calls the compositor's applyPending/didPresent at the
  headset rate; leaving VR repaints the scene at once (updates applied
  during VR set it dirty; a new video frame does too). `PS5RendererVR.cpp`
  untouched.

**Tests** (log-only PPSA99020 runs, same binary, `debug-screenshot`,
`video.html` with the synthetic clips; `?test=transform` and `?test=fill`
are new, VIDEOSTATS now carries requestVideoFrameCallback counts):

| clip (hardware-decoded unless noted) | CPU path: shown / dropped (title) | WC: shown / dropped | rVFC callbacks / presentedFrames at the last VIDEOSTATS, CPU vs WC |
| --- | --- | --- | --- |
| H.264 1080p30, `?test=geometry` (40 s) | 1205 / 9 | 1194 / 9 | 1015/1018 vs 1007/1017 |
| H.264 1080p30, `?test=transform` (34 s) | 1034 / 8 | 1035 / 8 | |
| HEVC 2160p30 (25 s) | 767 / 5 | 766 / 5 | 742/746 vs 739/745 |
| VP9 1080p30 (25 s) | 767 / 6 | 768 / 6 | 744/747 vs 739/747 |
| H.264 1080p60 (30 s) | 1785 / 15 | 1785 / 15 | 1758/1800 vs 1710/1800 |
| software (`no-hwdec`): H.264 1080p30 geometry / VP9 1080p30 | | 1195 / 10, 743 / 7 | readyState 3-4 throughout |

- **Geometry** (screenshots, CPU vs WC, pixels > 24 / > 64 apart): rounded
  clip with the label over it and scrolling 0.20 / 0.12 % (+2 s) and
  0.22 / 0.13 % (+30 s; the stats text); 4.45 % at +10 s, where the CPU
  path still had the video at its pre-scroll place (hole-punch geometry
  lags a scroll by a frame or more; WC moves video and page together).
  Turned + scaled + 60 % opacity over stripes: WC shows the video correctly
  blended and turned; the CPU path shows *nothing* there (a rotated video
  can't be punched, and a hardware-decoded frame can't be painted into the
  page), hence 28 %. HEVC/VP9/1080p60 0.00 % at +2 s, 0-2 % at +10 s (the
  running clock differs by a frame). Software decoding in WC vs hardware
  in WC: 0.06-1.3 %.
- **Feed pages:** `feed-players.html` (8 players, MSE and progressive,
  suspend/resume, loops) in WC: ran to the end (`FP DONE`), 0 `FP SPIN`
  (spinner over a playing video), 8 video layers, 0 pictures lost without a
  release, no take-backs (not compared with a CPU run of the page this
  round). `test/paint/wcfeed.html`
  (60 posts, a muted looping video in each, scrolled for minutes): 60 video
  layers, 492 layers, 58-59 updates/s while scrolling; 170 decoders opened
  and 168 closed as videos went off screen (direct memory 5.1 GB at load
  with everything starting at once, then 1.45 and 0.87 GB; three videos fell
  back to software when flexible memory ran short at load). The WebProcess
  aborted after 135 s in GStreamer's playsink (`gst_play_sink_do_reconfigure:
  assertion failed: (it)` after "Padname sink_0 is not unique in element
  streamsynchronizer"): the same abort is in earlier runs of the same page
  by other rounds and in a run on 2026-10-31, before this work, so it is a
  playsink race with many pipelines, not the video layer. Open.
- **View in VR with compositing on** (`vr-sim` + `vr-guesstest`, 20 enter
  and leave cycles in two runs: 1080p H.264, VP9, 4K HEVC, 1080p60): every
  guess/remembered/forgotten check PASS; the WebProcess kept sending 48-61
  updates/s and the video kept decoding (VIDEOSTATS every second, 30
  frames/s) while in VR, with up to 6 frames dropped at an entry; the scene
  painted again within the first second after each exit (36 paints that
  second; no WebProcess update needed). Title direct memory
  after each URL flat over 12 cycles (754 -> 796.4/796.5 MB for H.264,
  1095.2-1095.4 for 4K HEVC; 688.9 MB at the end, as the run before),
  0 pictures lost without a release; page tables 6158-6162 during the run,
  5012 after; swap flat at 452.9 MB.
- **Fullscreen:** `requestFullscreen()` needs a user gesture ("transient
  activation"), which log-only runs can't give; `?test=fill` (the video
  fixed over the whole viewport, as the fullscreen style does) is the
  stand-in: HEVC 2160p30 over the whole 1920x1080 view, 857 frames, 6
  dropped, and with the page otherwise still the scene repainted for video
  frames only (`30 paints for video frames only` per second, no updates).
  The real fullscreen API is untested.

**Not done / open:** the playsink abort above; canvas `drawImage` of a
hardware-decoded video still draws nothing (unchanged); HDR still shown with
the BT.709 matrix; a video layer is repainted only when its stream converts
a frame, so a layer whose stream exceeds 16 per frame stays empty.

## WC title abort while scrolling a video feed (2026-11-01)

The owner's TV test of WC mode (a heavy feed page with many autoplaying
videos, scrolled for a while) killed the title: kernel log "A user thread
receives a fatal signal / abort is called", thread `eboot.bin` (the main
thread), nothing in the title log.

**Symbolizing:** the crashed eboot (downloaded from the console) is a plain
ELF, code at file offset 0x4000 for vaddr 0, and the kernel's backtrace
values are load addresses: the title's text is mapped at 0x400000
(`xotext: 0000000000400000` in the same report), so subtract 0x400000.
Byte windows before each return address in the crashed eboot matched the
next build's symbols uniquely:

    0x40017b  _start
    0x40b59f  main
    0x555b3c9 g_main_loop_run / g_main_context_iterate / g_main_context_dispatch
    0x315b590 WTF::RunLoop timer source (RunLoopGLib.cpp:94/122)
    0x44d0840 WebCore::BitmapTexturePool::releaseUnusedTexturesTimerFired()
    0x44c8597 WebCore::PlatformDisplay::sharedDisplay(): RELEASE_ASSERT(s_sharedDisplay) -> abort()

**Cause:** BitmapTexturePool (TextureMapper's intermediate surfaces: a
composited layer with opacity < 1 whose children overlap, masks, filters,
3D) has an 80 MB limit (`BITMAP_TEXTURE_POOL_MAX_SIZE_IN_MB`). S1b made
`scheduleReleaseUnusedTextures` return on PS5 (the title's render thread
releases on every WCScene update), but `enterLimitExceededModeIfNeeded`
started the main-thread timer itself (`startOneShot(0_s)`). The timer ran
on the title's main thread with no GL context current and asked
`PlatformDisplay::sharedDisplay()` for a sharing context; the title has no
shared display: RELEASE_ASSERT, abort. A surface is sized to the visible
part of its layer, so while scrolling every step allocates new sizes and
the pool fills within seconds on pages with translucent composited cards.
Not the animation or tile-arena work, and not an allocation failure.

**Fix** (`src-wc` 7d09449da9, `BitmapTexturePool.cpp`): no main-thread timer
on PS5 when the limit is exceeded either (the per-update release on the
render thread uses the 50 ms limit-mode tolerance), and the timer path uses
`sharedDisplayIfExists()`. c290e972ee logs `[wc] texture pool over its 80 MB
limit` (first time and every 100th).

**Reproduction** (`test/paint/wcfeed.html`, a self-scrolling feed of 60
translucent composited cards with synthetic videos, 5 min, served by
`tools/serve-media.py --page`): the pool went over 80 MB within seconds of
the first scroll step (82.6 MB in 49 textures) and 700 times in 5 min; the
fixed title ran the 5 minutes at 57-58 rAF/s (492 layers, GL textures
96 MB median, 137 MB max). Before the fix the first of these would have
aborted.

**Tooling** (outer repo bc85844):
- `package.sh` gives each package a build id (time + source commit), which
  the title logs at start (`ps5browser: ... build <id>`), and keeps the
  unstripped `ps5browser.elf` and `ps5wk-child` (debug sections
  zlib-compressed, ~340 MB each) with `build.txt` (eboot/child sha1, source
  commit, number of uncommitted files) and the uncommitted source diff in
  `toolchain/wpe-webkit/titles/debug/<id>/`; the last 5 are kept and
  `titles/ps5browser-symbols.elf` links to the newest.
  `llvm-addr2line -f -C -i -e titles/debug/<id>/ps5browser.elf 0x<addr - 0x400000>`.
- `--wrap=abort` (`apps/title/titleabort.c`): an abort in the title
  (RELEASE_ASSERT/CRASH, libc++ hardening after its stderr message,
  g_error after logHandler's line) first logs
  `[abort] abort() called on thread 0x... (main): backtrace 0x... (self 0x...)`
  to the title log, its file log and the kernel log (frame-pointer walk,
  load addresses as in the kernel's report; no stdio, no malloc).
- `test/paint/scroll-long.js`: a 4-minute scroll for `test-onload.js`.

**Page tables and swap, WC vs CPU** (log-only runs, same build
20261101-162413-c290e972ee, compositing flag on/off; kernel VM Stats; idle
before the runs 5010-5030 CPU page tables):

| scenario | mode | page tables max | swap out | title direct memory | WebProcess mmaps at end | scrolled |
| --- | --- | --- | --- | --- | --- | --- |
| feed page from the TV test, `scroll-long.js` 4 min | WC | 6673 | 452 -> 811 | 731-885 MB | 1603 MB (148 live) | 124k px in 182 s, rAF 20-25/s |
| | CPU | 6540 | 582 -> 582 | up to 2661 MB | 1320 MB (117 live) | 58k px in 182 s, rAF 8-15/s |
| `wcfeed.html` 5 min | WC | 6636 | 442 -> 444 | 6623 MB at load, then 1006-1486 | 1005 MB | 57-58 rAF/s |
| | CPU | 7209 (a WebProcess crash and reload during the run) | 443 -> 838 | 7679 MB, flexible 8 MB free | 1468 MB (peak 1908) | 8-42 rAF/s |

- Page tables follow the WebProcess's mappings (libpas growth as the
  feed grows: on the feed page +371 MB of WebProcess mmaps over 4 min
  against +332 page tables), not the title: the title's anonymous mmaps stay
  at 264 MB and GL textures at 50-147 MB in WC mode; the tile arena keeps 8
  MB chunks. WC scrolled twice as far in the same time (more feed loaded),
  so its slightly higher peak is more page, not more cost per page. The
  owner's 7284 had a WebProcess with 1782 MB in 169 mappings (peak 1853 MB)
  plus the console's own load; it was not an allocation failure (the abort
  was the assertion above).
- Decoder direct memory does not show in the page-table count (6.6 GB of
  direct memory at the 60-video load, page tables 6636).
- **Video decoders (not WC):** every VP9 stream gets a 3840x2160 decoder
  (`PS5VideoDecoder.cpp:1128`, "VP9 is cheap at 4K"): 535 MB direct memory
  and 12 x 12 MB output buffers for a 480x854 clip; H.264 ones are opened
  at 1920x1088 (70 MB). A page that starts 60 muted autoplay videos opens a
  decoder for each before WebKit suspends the off-screen ones: 6.6-7.7 GB of
  direct memory, flexible memory down to 5-8 MB, then
  `MapNamedFlexibleMemory rc=0x8002000c` and software decoding. Worth a cap
  (decoders per WebProcess, or the VP9 size from the stream) in M6.

**WebProcess abort in GStreamer's playsink (not WC, seen in both modes):**
with many videos started and suspended at once (`wcfeed.html` in CPU mode
here, in WC mode in M4's run, and once on 2026-10-31) the WebProcess
logged `Padname src_0 / sink_0 is not unique in element streamsynchronizerN`
and aborted in `gst_play_sink_do_reconfigure` (`gstplaysink.c:3625`,
assertion `it`; crash guard stack: g_assertion_message_expr <-
gst_play_sink_do_reconfigure <- sinkpad_blocked_cb). streamsynchronizer
names its request pads from a counter that `new_pad` increments only after
dropping and retaking its lock to add the pads, and that READY->NULL resets
to 0 even while a pad requested during the state change is still there
(WebKit sets suspended videos' pipelines to NULL from the main thread while
the streaming thread reconfigures playsink). The next request then reuses
`sink_0`, adding the pad fails and frees it, and playsink asserts on it.
Patch `port/deps/patches/gst-plugins-base/0001-streamsynchronizer-unique-pad-names.patch`:
the number is reserved before the lock is dropped, skips names that exist,
and the counter is reset only when no stream is left. Unpatched, the CPU
run of `wcfeed.html` aborted twice in 5 min (17:05 and 17:07); patched
(build 20261101-174417-0e6aee371a), 5 min in CPU mode without an abort.

**Open: WebProcess stall with 60 videos in WC mode** (same build, which
also has M4's video layers): 11 s into `wcfeed.html` the title had opened
20 decoders (mostly 3840x2160 VP9 ones, see above), direct memory 10.5 GB,
flexible memory 0-13 MB; further decoders failed (`MapNamedFlexibleMemory
rc=0x8002000c`, 3 s each before falling back to software), the WebProcess
held 590 mappings (1816 MB, 2555 MB resident in big mappings, page tables
7201) and from then on sent no update and logged nothing but its 60 s
`[mem]` line for the rest of the run. Title and console recovered on stop
(page tables 5013). Not seen in the same scenario with the earlier build
(c290e972ee: 398 decoders opened and closed over 5 min). Needs a thread
dump of the stalled WebProcess; a decoder cap would avoid the state.

## M6 results (2026-11-01)

Robustness: decoder budget, GL texture cap, WebProcess crash recovery,
settings, long session. Commits: outer repo 4268463 (decoder budget, VP9
sizing), ddd34f8 (crash-test endpoint); `src-wc` b9007fb2c3 (suspend after
preroll), 900ba6f6ad (GL cap, scene teardown). Builds tested
20261101-175542 / -180658 / -182441 (-0e6aee371a source base), all with the
deps archive of 17:31 (gstreamer-full with the streamsynchronizer patch).

**Decoder admission** (both modes; `port/webkit/wpe-ps5/PS5VideoDecoder.cpp`,
"budget" section and `openHardware`):
- At most 8 hardware decoders at once; their memory (decoder + output
  buffers, measured around each open, opens serialized under `openLock`)
  within 2048 MB of direct memory; an open must leave 64 MB of flexible
  memory free (`admit`). Over budget, the DOPEN waits up to 0.5 s for
  decoders to close (off-screen videos being suspended), then is refused
  (status 5): that pipeline run decodes in software. decodebin3 drops its
  decoder at PAUSED -> READY (`gst_decodebin3_reset`), so a suspended video
  that comes back asks the title again.
- VP9 decoders are sized to the stream (at least 1920x1088); 4K headroom
  only for streams above 1080p while it fits in half the budget. Measured
  over the runs below: VP9 1920x1088 208 MB direct (median of 389 opens,
  p10-p90 143-282), 6 MB flexible, against 535 + 32 + 12 x 12 MB at
  3840x2160; H.264 1920x1088 107 MB (745 opens, 57-180; the spread is
  frees landing in the measuring window), 24 MB flexible.
- `/data/ps5webkit/vdec-budget` overrides (`decoders= direct= reserve=
  wait= vp9headroom=0/1/2`); open/close lines log each decoder's cost and
  `budget N decoders, X MB (peak ..., refused, waited, VP9 sized to the stream)`.
- Why not take decoders back from paused or hidden videos: ca31aee removed
  the slot take-back because the pictures still belong to the WebProcess
  (preroll, playsink's queue); closing a decoder under a live pipeline
  would need the proxy to reopen it and wait for a keyframe. Instead the
  pipelines themselves let go (below), and the budget bounds the rest.
- **WebKit side** (`MediaPlayerPrivateGStreamer::updateStates` ->
  `ps5SuspendAfterPreroll`): a muted video made off screen was marked
  suspended while its pipeline was still NULL, and its preroll then went
  ahead (only PLAYING is held back while suspended), so every off-screen
  video of a feed kept a decoder open, paused. Once prerolled (metadata,
  first frame, ready states delivered) it now goes to its suspend state (NULL;
  MSE players keep PAUSED as before) and resumes like a video that scrolled
  away. `PS5WK_MEDIA_SUSPEND_PREROLLED=0` = old behaviour.

`wcfeed.html?mins=2` (60 posts with muted looping synthetic videos, 2 min
self-scroll), log-only. "old" = the same binary with
`vdec-budget: decoders=999 direct=999999 reserve=0 wait=0 vp9headroom=2`
and `PS5WK_MEDIA_SUSPEND_PREROLLED=0`. Whether all videos start at once at
the load varies from run to run (WCFEED's playing count at 10 s: 9-12 in
some runs, 55-58 in others); the 10.5 GB / 5.1 GB "before" figures are the
burst case from earlier rounds:

| run | mode | decoders opened (VP9 4K) | most open at once | refused -> software | title direct max | flexible min | suspended after preroll | page tables max | rAF |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| crash agent, build 0e6aee371a (burst) | WC | | 20 | (opens failing, 3 s each) | 10.5 GB | 0-13 MB | | 7201 | WebProcess stalled |
| M4 round (burst) | WC | 170 | | 3 (flexible exhausted) | 5.1 GB | | | 6636 | 57-58 |
| A: old, same binary (no burst) | WC | 162 (55) | 6 (2154 MB) | 0 | 2515 MB | 158 MB | 0 | 6293 | 58-59 |
| B: suspend + budget, 4K VP9 while roomy | WC | 201 (66) | 6 (1013 MB) | 0 | 1587 MB | 184 MB | 58 | 6268 | 58-59 |
| C: final (burst at load: 58 playing at 10 s) | WC | 178 (0) | 8 (1493 MB) | 34 | 1728 MB | 85 MB | 57 | 6764 | 20 at 10 s, then 58-59 |
| D: final | CPU | 158 (0) | 8 (1682 MB) | 27 | 1241 MB | 159 MB | 55 | 6647 | 23 at 10 s, then 40-42 |
| G: final (burst; first 60 s, cap test run) | WC | 76 (0) | 8 (1462 MB) | 15 | 1826 MB | 160 MB | 37 | 6529 | 40 at 20 s, then 57-59 |

- The budget holds: never more than 8 decoders or ~1.7 GB of decoder memory;
  flexible memory stayed above the 64 MB reserve (measured minimum 85 MB;
  65 MB in the budget's own view, opens in flight subtracted); no
  `MapNamedFlexibleMemory` failure and no stall in any run. Earlier CPU runs
  of the page reached 7679 MB (plan section above).
- Refusals happen only in the load burst (prerolls of off-screen videos,
  which then go to NULL); the visible videos played hardware-decoded at
  30 frames/s (runs C, D: `[video]` stats windows 28.8-29.6 shown/s, 0-3
  dropped).
  Software prerolls cost CPU for a few seconds at the load (rAF 20-40 in
  the first 10-20 s of the burst runs).
- Page tables follow the WebProcess (6268-6764 in every run, swap flat at
  459-464 MB); decoder direct memory does not show there.

**GL texture cap** (WC; `PS5WCHost.cpp` `SceneHost::checkGLMemory`, called
each presented frame; `DrawingAreaProxyWCPS5::glMemoryAction`;
`GraphicsLayerWC::setTileCoverageReduced`): decisions go by a ~1 s average of
`BitmapTextureUploaderPS5::textureBytes()` (the texture pool's
intermediate surfaces swing 49-101 MB from one second to the next on
`wcfeed.html`). Above 128 MB: logged (every 30 s). Above the 160 MB cap:
`DrawingArea::SetTileCoverageReducedWC(true)` to the shown page's
WebProcess: tiles for the visible rect only, no margin, no whole-layer tiling
of animated layers; back to normal after 20 s under 96 MB. Still over the
cap 5 s later, or over 240 MB: the page's WebProcess is ended and the page
reloads (the title's web-process-terminated handler) in a new one that
paints on the CPU, for that WebProcess's life. `/data/ps5webkit/wc-gl-cap`
(MB) moves the cap. Checked with the cap at 100 MB on `wcfeed.html` (its GL
textures run at 94-97 MB median, 136 MB max): first version (instantaneous
value) reduced coverage at 104 MB and fell back to the CPU 5 s later on a
pool spike (100.8 MB), the reload painted on 4 threads, `[wc]` lines gone,
rAF 30; with the average, coverage was reduced at 100.5 MB and the page
stayed composited (no fallback in the remaining 40 s). Reduced coverage
saves little on this page: its textures are mostly the pool's surfaces for
the 60 translucent cards (the pool went over its own 80 MB limit), not
tiles. At the default 160 MB cap no run reached the cap (max 137 MB).

Page tables: title GL textures and the tile arena's shm chunks don't show
as a separate cost: page tables 6264-6764 in every WC and CPU run, the same
spread as M5 (the WebProcess's mappings dominate).

**WebProcess crash recovery** (WC, `wcfeed.html`, `POST /restart?scope=crash`
= SIGKILL to the WebProcess, then `POST /restart?scope=page`):
- crash: `RESULT ps5browser webprocess FAIL terminated (reason 0); reloading`,
  a new drawing area at once, `[wc] scene 19 removed: 492 layers, 60 video
  layers; GL textures 100.7 -> 58.0 MB, 0 scenes left`; every decoder of the
  dead WebProcess released (`budget 0 decoders, 0 MB`); the reloaded page
  composited again (492 layers, 58 rAF/s, videos playing) within ~3 s.
- page restart: `scene 24 removed: ... GL textures 125.4 -> 83.5 MB`,
  `RESULT ps5browser page-restart PASS`, composited again.
- The 58 / 83.5 MB left with no scene were BitmapTexturePool's free
  surfaces (a global pool, released only by scene updates). The host now
  empties the pool when its last scene goes: after the fix the same crash
  went `64.4 -> 7.9 MB` (the 8.3 MB scene texture; also on the CPU
  fallback reload: `100.8 -> 7.9 MB`).
- Title direct memory after the reloads: 993.7 MB (crash), 877.2 MB (page
  restart), as in steady play of the page; no stale video layers: the new
  scene's video layers are the new page's own (9, then 60 as its posts
  were made).

**Settings** (the `compositing` item, `apps/title/settings.c`, ba6c52c):
README.md is not touched here (uncommitted hunks of another round); its
Settings table should get this row, after *Offer VP9 video*:

| Setting | Stored as | Applies |
| --- | --- | --- |
| GPU compositing, experimental (default off; WC builds only) | `/data/ps5webkit/compositing` when on (opt-in flag file) | page restart |

and `compositing` in the list of setting ids (after `media_vp9`), with
this note under the table: *GPU compositing* puts pages together on the
GPU in layers (WebKit's WC compositor in the title, this file). WebKit reads
the flag whenever a page gets a new WebProcess, so *Restart page* applies it
(the log shows `[wc] page N: WC compositor`); without it, or in a build
without the compositor, pages paint on the CPU. A page whose GL textures
stay over the 160 MB cap gets a new WebProcess that paints on the CPU
(`[wc] ... this page goes to the CPU path`); the next WebProcess composites
again. Test knobs, not in Settings: `/data/ps5webkit/wc-gl-cap`,
`/data/ps5webkit/vdec-budget`, `PS5WK_MEDIA_SUSPEND_PREROLLED` (child.env),
`POST /restart?scope=crash`.

**Long session** (WC, build 20261101-182441, 18:29-19:12, 43 min, one
title and one WebProcess throughout; `test-urls.txt` of 40 loads: neutral
public pages (Wikipedia, MDN, BBC News, docs.python.org, w3.org, gnu.org,
DuckDuckGo lite, example.com/org) and the synthetic pages (`wcfeed.html`
x4, `feed.html` x2, `feed-players.html` x2, `video.html` with H.264 1080p30
/ 1080p60, VP9, HEVC 2160p fill, `sticky.html`, `animations.html?auto`,
`layers.html`, `static.html`); `test-onload.js` scrolled every page down
for 20 s and up for 10 s):
- No crash: no title abort, no WebProcess crash or restart, no playsink
  abort, no CPU fallback, no coverage reduction (one known `g_bytes_get_data`
  GLib-CRITICAL on BBC News, seen 64 times in earlier logs). The 4 URL
  "failures" are the harness: two of the four `wcfeed.html` loads never
  report load-finished within 60 s (as in earlier rounds) and are counted
  twice.
- Title direct memory after each load: 655-704 MB on every neutral page
  from the first (example.com 635.8, Wikipedia 644.4) to the last
  (DuckDuckGo 663.7, `animations.html` 658.4); media pages 741-1078 MB
  while their decoders are open, back to the baseline at the next page.
  Flexible memory 277 MB free after every neutral page (283 at the start;
  202 MB at worst with a page's decoders open).
- GL textures: median 15-55 MB per page, max 137.4 MB (`wcfeed.html`),
  under the 160 MB cap throughout (the ~1 s average never reached the
  128 MB log threshold).
- Decoders: 604 opened, 605 closed, at most 7 at once (965 MB), 0 refused;
  0 VP9 decoders at 4K; 189 videos suspended after an off-screen preroll.
  `[video]` stats: 440 windows over 86 streams, 26.3 frames shown/s on
  average (videos scrolled in and out mid-window), 127 dropped in all.
- Page tables 5016 at the start, 6380-6531 through the session (6531 max,
  +150 over the session, following the WebProcess's mappings: 45 -> 207 live,
  713 -> 1112 MB, libpas committed 228 -> 482-541 MB as the WebProcess
  served 40 pages), 5013 after the title stopped; swap 460.3 -> 456.7 MB.
  Pass: no crash, title memory flat, GL under the cap; the WebProcess's own
  growth is the known libpas behaviour (the 3 GB memory restart covers it).
- Scrolling: page rAF while scrolled 47.6-60.3/s (median 59.9).

**What is left:**
- A decoder of a paused or hidden MSE player stays open (WebKit keeps MSE
  pipelines PAUSED when suspended); on a long MSE feed they still count
  against the budget, and newly visible videos then decode in software.
  Taking such a decoder back needs the proxy to give it up and reopen at
  the next keyframe (a protocol change in `port/deps/gst`).
- The proxy treats a refusal as "software"; there is no "wait for a
  decoder" or poster-only state, so a refused visible video plays in
  software until its next suspend/resume.
- Reduced tile coverage saves little on pages whose GL memory is
  TextureMapper's intermediate surfaces (many translucent composited
  layers); the pool's own 80 MB limit is the lever there.
- The CPU fallback lasts for the WebProcess's life (one WebProcess serves
  every navigation), not for one page.
- The owner's TV check of the decoder budget on real feeds.

## TV test 2 fixes (2026-11-01)

The owner's second TV session in WC mode (build 20261101-191505, `tv22.log`)
reported scrolling that "starts off feeling quicker but starts to feel
slow" and a lock-up on a video feed after which other sites felt slow.

### Frame pacing

**What the TV numbers were.** The `[present]`, `[render] wc` and `[wc]`
lines on the MDN home page read 21-32 updates/s and 25-31 presents/s, but
the first two are 5 s windows and the stick was released and moved again
within them: the 1 s `[wc]` lines of the same minutes reach 52-57 updates/s
in seconds of steady scrolling, and `frame done` of 12-16 ms only happens
when updates come one per vsync (an update that finds no flip pending is
presented in 2-4 ms: 4.2 ms in the session's idle window). Pages with video
or many layers were really slower: YouTube 8-44 updates/s with scene paints
of 13-27 ms (624 layers), reddit 8-44 with the WebProcess busy.

**Instrumentation** (kept; all cheap):
- `[present] render: apply, video, scene, draw` after each `[present]` line
  (PS5Renderer: where the render thread's time goes; draw includes the wait
  for the previous swap's flip).
- `[wc] ... update in the title: queued, applied to swap, swap to reply`:
  an update from the main thread receiving it to its reply running on the
  main thread (the reply hop is 0.01-0.02 ms; queued 0.3-0.8 ms on light
  pages, 2-7 ms on reddit).
- `[render] wc ... reply to next update, waiting at the reply, started by
  the idle timer; tile wait` (DrawingAreaWC: the WebProcess's part of a
  cycle). `[render1]` (PS5WK_RENDER_LOG=2) adds timestamped reply/start/sent.
- `/data/ps5webkit/wc-frame-log`: a `[frame]` line per render-thread frame
  (started N ms after the last swap, apply, video, scene, draw, swap) and per
  presented update (queued at, applied, swap); `test/paint/interval.html`, a
  timer-driven box.

**Findings** (log-only runs, `pad-scroll-test` = the owner's stick as wheel
events every 8 ms):
- The swap returns at once; the next frame's first draw waits for that
  swap's flip: 11-13 ms when the frame starts right after the swap, 0.1-0.2
  ms when it starts 20 ms or more later (`[frame]` lines). So the title
  paces updates to the display by itself.
- With an update waiting at every reply (stick held, or a rAF loop) the old
  code already ran at 60/s on MDN, Wikipedia, `layers.html` (and 57 on BBC).
- Frames that ran without an update (video playing, the pointer, M3's
  accelerated animations) started right after the swap or 10 ms after it
  with video, before the WebProcess's update for that vsync arrived; the
  update then waited for the next frame, and a scene that took more than
  ~6 ms to paint missed its vsync because the video loop started 10 ms
  late. M3 had fixed this only while animating.
- DrawingAreaWC's idle refresh timer fired a full frame after a reply even
  when a refresh was asked for just after it (an update requested by a
  wheel event or timer that came in then).
- TextureMapper drew every tile of every layer each frame, including WC's
  coverage margin and a feed's layers below the fold.

**Fixes** (src-wc 95e259c204, 80573f3544, 8f6597b8f5; outer repo 7ddf170):
- DrawingAreaWC: the first refresh asked for after a reply fires at once
  (once per reply; `PS5WK_WC_REFRESH_NOW=0` = old).
- PS5WCHost: any frame that applied no update while one is expected waits
  for it until 6 ms before the frame is due (`/data/ps5webkit/wc-wait-old`
  = M3 behaviour; the same flag restores the 10 ms below).
- PS5Renderer: with video, frames start 2 ms after the swap (was 10 ms).
- TextureMapperSparseBackingStore: tiles outside the clip bounds are not
  drawn (affine transforms; `/data/ps5webkit/wc-no-cull` draws all).
- Not done: answering an update when it is applied instead of after the
  swap (pipelining). With the reply after the swap the WebProcess has most
  of a frame, and rAF ticks stay one per vsync; an early reply can tick rAF
  twice within a few ms when a frame starts late.

**Measured**, same binary, flags for "old" (stick at 0.3, 4 s down/up, 2 s
still; one-vsync share = consecutive presented updates one vsync apart
within bursts; updates/s = 1 s `[wc]` windows with updates):

| page | old: one-vsync / updates/s / scene ms | new: one-vsync / updates/s / scene ms |
| --- | --- | --- |
| MDN home | 99 % / 47.6 / 2.0 | 99 % / 49.7 / 1.5 |
| YouTube home (232 layers) | 84 % / 23.3 / 9.1 | 97 % / 49.2 / 7.6 |
| reddit front page | 94 % / 34.2 / 6.6 | 83 % / 33.3 / 7.3 (content differs per load; WebProcess update 3.9 ms, frame done 16-17 ms in both) |

With the stick held and a rAF counter on the page (`test-onload.js`; an
update is then waiting at every reply), old vs new on the same pages:

| page | updates/s old / new | frame done ms old / new | scene paint ms old / new | rAF/s new (max) |
| --- | --- | --- | --- | --- |
| MDN docs page | 59.9 / 59.9 | 13.9 / 13.9 | 3.2 / 2.3 | 59.9 (60.2) |
| Wikipedia article | 59.9 / 59.9 | 14.5 / 14.5 | 2.3 / 1.5 | 59.9 (60.0) |
| BBC News | 57.3 / 57.1 | 14.5 / 14.5 | 6.6 / 5.7 | 57.7 (60.0) |
| reddit front page | 21.4 / 20.4 | 60.3 / 29.3 | 5.0 / 5.6 | 23.1 (60.0) |
| `layers.html` | 59.9 / 59.9 | 15.8 / 15.8 | 5.4 / 3.6 | 59.9 (60.0) |

- reddit is bound by its WebProcess (rendering updates of 16-18 ms with
  script, tile wait 10-16 ms, the reply queued behind main-thread work);
  the title's part of an update is 18 ms (queued 7, applied to swap 10).
- `wcfeed.html` (492 layers, 55 videos playing): 57-59 updates/s, scene
  paint 9.6 ms; `test/paint/interval.html` 50 ticks and 50 frames shown/s
  in both modes.

### The video feed lock-up

**What the log shows.** In `tv22.log` the WebProcess sent no update for
about 75 s on reddit (the `[render] wc` window of 79.7 s holds 154 updates,
which must have come in bursts at both ends; the `[wc]` window of 77.7 s
starts with 44), then 41 updates in the second before the owner navigated
away. The missing `[cpu] web4` lines mean only that the WebProcess used
less than 0.3 cores (`ps5wk-child.cpp` logs only above that), i.e. it was
waiting, not computing. A main-thread timer (`MediaSource::streamingTimerFired`)
logged at about the start of the gap, the navigation's `load started`
came from the WebProcess, and its media teardown ran at once after it.

**The video path was not holding it.** The hypothesis that the title only
consumes frames of streams whose layer is drawn is wrong for this code:
`PS5VideoServer::consume` runs over every stream in every render-thread
frame (`PS5Renderer::threadMain`), the loop wakes for queued frames
(`nextDeadline`), every chosen frame is converted in `handleVideo` whatever
draws it, and pinned pictures go back after the swap (`presented`). The
held slots in the log belong to the WebProcess: decoder 10 (an MSE player
with audio) made 14 pictures; the proxy read 5 (4 given back unrendered,
1 prerolled into the sink: "stop after 1 frames sent"), the other 9 were
unread on the socket because the decoder's streaming thread sat in
`finish_frame` pushing into the prerolled, PAUSED sink, and the title's
audio stream for that player played 0.0 s in 84.8 s. That is a player
that prerolled and never started playing (the same pattern as YouTube's
preview player at line 431 and reddit's decoder 2: 1 frame sent, 5
pictures, 4 given back), which the decoder's "every output slot held"
line describes; it blocks nothing on the WebProcess main thread (the
title's serve loop never stops reading, the proxy's waits end on flush,
stop and a 9 s stall timeout).

**Reproduction** (log-only, new build, `PS5WK_PROFILE=threads` in
child.env to have where every thread waits if it happened): reddit front
page scrolled for 4 min (6 s down, 6 s up, 3 s still), `wcfeed.html` (60
s: the harness moves on after 60 s because the page never reports its load
finished) and `test/media/feed.html` (whole timeline, `FEED DONE`, 0
stuck): updates never stopped (1 s `[wc]` windows 7-60 updates on reddit,
57-59 on `wcfeed.html`), no WebProcess crash, decoders opened and closed
(206 decoder lines on reddit, budget back to 0 at the end). The stall did
not reproduce, so there is no thread dump of it. The earlier "WebProcess
stall with 60 videos" was the flexible-memory exhaustion state that M6's
decoder budget now prevents.

**Afterwards.** In `tv22.log` everything was released at the navigation
(decoder budget back to 0 decoders / 0 MB, title direct memory 671 MB,
flexible 281 MB, GL textures 24 MB on the next page). The pages after it
ran as the ones before it (old.reddit 16 updates/s, MDN 25-32/s in 5 s
windows; YouTube and BBC before the stall 17-40/s): the same 5 s-window
numbers as above, not a degraded state. In run 9 MDN after the two media
pages ran at 60 updates/s in scrolling seconds with 1.5 ms scene paints,
as on a fresh title.

**Open:** what kept the WebProcess from updating for ~75 s on the TV is
unknown (waiting, not busy; not the video path by the evidence above).
If it comes back: `PS5WK_PROFILE=threads` in child.env shows where every
thread waits; a main-thread watchdog in ps5wk-child that logs the main
thread's stack after N s without a run-loop turn would catch it in a
normal session.

### Sticky header one scroll step behind (WC)

Owner: "the sticky header isn't quite right: when scrolling I can see the
content behind the header and sometimes it gets pushed slightly off screen".

**Cause** (`src-wc` b235085eb2 and edfee43cea, `LocalFrameView.cpp`, PS5 only): on a
main-thread scroll, `ScrollView::scrollTo` runs `completeUpdatesAfterScrollTo`
(layer positions, `scrollContents`, the OnScroll compositing update), and only
then `LocalFrameView::scrollTo` calls `scrollPositionChanged`, which moves the
scrolled contents layer, the layout viewport (`updateLayoutViewport`) and,
through M2's `setViewportConstrainedObjectsNeedLayout`, the sticky offsets.
Upstream that function relaid out every sticky renderer, so the rendering
update ran a layout and an AfterLayout compositing update with fresh
offsets. M2 replaced the relayout with a layer position walk, which leaves no
layout behind: the only compositing update of the step was the OnScroll one,
which ran before the offsets changed. A composited sticky layer (M3) was
therefore placed with the previous step's offset while the page moved by
the current one: one wheel event (7-35 px with the pad's 8 ms ticks) up off
the screen while scrolling down, page content showing above it while
scrolling up, and left there when scrolling stopped at the end of the page.
Pages hid it when anything else caused a layout each step (a fixed element:
M2 gives composited fixed elements an out-of-flow movement layout; scroll
listeners that change the DOM), which is why `sticky.html` (fixed footer,
scrollY counter) and the docs pages never showed it, and why the stopped
captures of M3 matched.

**Fix:** while a `LocalFrameView::scrollTo` of a composited-scrolling frame
runs, `updateCompositingLayersAfterScrolling` is held back and run once at
the end, after the layout viewport and layer positions are current (one
OnScroll update per step, as before, just later; with a fixed element the
update bails because a layout is pending, and the AfterLayout update does it).
`PS5WK_WC_STICKY_SYNC=0` (child.env) = old order. Follow-up (edfee43cea):
when that update bails for a pending layout, the root is marked for a
requirements traversal so the AfterLayout update still recomputes overlap
for the new scroll position, as the OnScroll update always did.

**Test knob:** `/data/ps5webkit/pad-scroll-test` (`WPEDisplayPS5.cpp`) scrolls
like the owner's right stick (one precise wheel event per 8 ms pad tick) in
cycles and captures mid-scroll (`pad-NNN-down|up.ppm`), e.g.
`delay=9 secs=2.5 ry=0.2 pause=1 shots=4`. `sticky.html?quiet` has no scroll
listener.

**Measured** (pad-scroll-test, 960x540 captures, header strip matched against
a capture at rest; offsets in capture rows = 2 px):

| page, pad deflection | old order | fixed |
| --- | --- | --- |
| docs front page (sticky header, nothing else changes per step), ry 0.2 (7 px per event, ~875 px/s) | 45 of 45 mid-scroll captures off: -4 rows scrolling down (header pushed up 8 px), +3/+4 scrolling up (page above it) | 45 of 45 at 0 |
| same, ry 0.8 (28 px per event), stopped at the end of the page | header 18 px off the top until the next scroll | at 0 |
| docs article page, ry 0.2 | 60 of 60 at 0 (a layout each step hides it) | 55 of 55 at 0 |
| `sticky.html?quiet`, ry 0.8 | header, headings, fixed footer at 0 (the fixed footer's layout hides it) | at 0 |

Fixed elements were never off (they get a layout per step).

### Thumbnails slower to show in WC mode (not reproduced)

Owner: "the video thumbnails on youtube are now taking much longer to load
than they did without gpu". `tv22.log` had 232 layers on the video site's
home page growing to 624-643 after scrolling, `raster` up to 221 ms/s and
2-40 updates/s.

**Measured** (build 20261101-202621 and -205943, the same binary for both
paths, compositing flag on/off; `debug-screenshot` with the new
`series=<ms>x<count>` content, captures from each test URL's load start;
"filled" = share of the content area that is image-like):

| page | WC: thumbnails visible | CPU path |
| --- | --- | --- |
| `test/paint/images.html` (120 synthetic 640x360 JPEGs, `loading=lazy`), first load (includes the WebProcess start) | 9 of 12 at 1.93 s, 12 at 2.18 s | 6 at 2.0 s, 7 at 2.26-2.76 s, 12 at 3.01 s |
| same, `?layers` (a composited layer per card), images in the HTTP cache | 12 of 12 in the first capture (250 ms) | same |
| a channel's videos grid | filled 19 % at 4.5 s, 41 % at 4.76 s | 40 % at 4.26 s |
| a search results page | 13 % at 4.26 s, 38 % at 4.51 s | 24 % at 4.76 s (more at 5 s) |
| channel grid scrolled by script (24 px per 16 ms tick from load + 2 s) | every row drawn in the mid-scroll captures; got 4960 px down in 6 s | every row drawn; 3928 px |
| search page, same scroll | same | same |

The thumbnails show up together with the rest of the page in both
modes, within ±0.5 s, and WC got further in the scrolled runs. The
`[render]` lines put the time in the WebProcess's style/layout/script
during the load on both paths (search page: 53-60 ms per rendering update
for the first 5 s, WC and CPU alike), not in tiles or decoding. The
console's profile gets an empty home page ("Try searching"), so the
owner's home feed with recommendations couldn't be loaded; the feed-like
pages above were used instead.

Ruled out on the way, each by measurement with an A/B build: the
asynchronous decode that a layer's first paint asks for
(`RenderLayerBacking::paintContents` adds `DefaultAsynchronousImageDecode`
while `repaintCount() == 0`; with that off the scrolled-feed timeline was
the same); composited sticky elements (`PS5WK_WC_STICKY=0`: still 819 and
775 layers on the two pages); and limiting the overlap extent of sticky
and fixed layers to one view, since overlap is recomputed on each main-thread
scroll step anyway (903 layers instead of 961). Those knobs were not kept.

**What the layers are** (`PS5WK_WC_LAYER_LOG=1`, search page): most of the
775-961 layers are the site's touch-feedback overlays (the
`ytSpecTouchFeedbackShapeStroke/Fill` and `yt-interaction` stroke/fill
divs, two or three in every button and card) plus the guide entries. They
cost scene paint time in the title: 7.4-7.6 ms per frame with the
scissor-box tile culling (8f6597b8f5), 10.8 ms before it. That is
render-thread time per frame, not a delay for images. Why those divs are
composited (overlap with a composited ancestor or sibling, or the site's
own `will-change`) wasn't worked out.

**Open:** the owner's case is not reproduced. Things to check on the TV:
whether the slow thumbnails are on the home feed only (recommendations,
signed in) or also on search and channel pages; whether it gets better with
the pacing fixes above (the old ~30 updates/s cap delayed every new image by up to a frame
as well); a `series=500x30` capture on the TV build would time it.

**Tools added:** `test/paint/images.html` (`?lazy=0`, `?io` (IntersectionObserver
lazy loading), `?layers`, `?n=`, `?v=` (cache-busting); logs `IMG <i> loaded`,
`IMG-VIEWPORT`, `IMG-SLOTS`), `test/paint/make-images.py <dir>` (synthetic
thumbnails, served with `tools/serve-media.py --port 8775 --page
port/webkit/test/paint/images.html <dir>`), the screenshot series
(`ps5browser.c`). Note: the title's clean-up of `/data/ps5webkit/shots` at
start leaves files behind (old captures were still there in later runs), so
name lists should come from the run's log.

### Main-thread watchdog (ps5wk-child)

The TV session also had a WebProcess that sent no updates for ~75 s while
mostly idle and then recovered by itself; it never reproduced in log-only
runs. To catch it next time in normal browsing, both child processes now
run a watchdog (`apps/common/watchdog.c`, on by default):

- **Heartbeat:** a GLib timeout on the default main context (WebKit's main
  RunLoop), `G_PRIORITY_HIGH`, every 250 ms, stores the time and the main
  thread's own CPU clock.
- **Watchdog thread:** wakes once a second. When the main thread has not
  turned for `PS5WK_WATCHDOG_S` seconds (child.env; default 3, `0` off), it
  sends the main thread `SIGVTALRM` (unused by WebKit, JSC, GLib and the
  profiler's `SIGPROF`); the handler unwinds the interrupted stack with the
  profiler's unwinder (CFI + rbp chains through LLInt/JIT, `profiler.c`
  `ps5wk_backtrace_context`) into a static buffer and reads its own CPU
  clock. Then every other thread the crash guard's trampoline registered,
  once per stall. Every ~10 s while it lasts: the main thread again. Then a
  "recovered" line. Works with `PS5WK_PROFILE` off or on (checked with
  `threads`). If the whole process was stopped (rest mode: the watchdog
  itself did not run for > 3 s), that time is not counted.
- **Cost:** 4 timer dispatches/s on the main thread and 1 wake-up/s of the
  helper; idle WebProcess 0.00 cores with it on and off (`PS5WK_CPU_LOG=all`,
  60 s each). The watchdog takes no WebKit or GLib lock (the main thread may
  hold one) and writes its lines with `write(2)`.

**The lines** (title log, prefixed `[web<N>]`/`[network<N>]`):

```
[watchdog web4] on: reports a main thread that does not turn for 3 s (PS5WK_WATCHDOG_S; CFI unwinding)
[watchdog web4] main thread stalled: no run loop turn for 3.9 s, its CPU +3.8 s (busy); backtraces follow
[watchdog web4] bt main t756 cpu 3895 ms: j3 f7b2c4 30295fe 318083d 16d02e2 ... 2d38cc5 2c1e5a 2d57 4c1e
[watchdog web4] bt t95 JIT_Worklist_Helper_Thread cpu 3 ms: x800001dfc 2d3cfcb 2caa9df e5f8 2c82f2a ...
[watchdog web4] bt t324 PS5Paint cpu 1 ms: same as t262
[watchdog web4] bt t437 gmain: no answer to the signal
[watchdog web4] 16 other threads (9 distinct stacks, 1 silent)
[watchdog web4] main thread still stalled after 14.2 s, its CPU +13.8 s (busy)
[watchdog web4] bt main t194 cpu 13924 ms: j3 f7b2c4 ...
[watchdog web4] main thread recovered: stalled 15.0 s, its CPU +14.6 s (busy)
```

- `its CPU +x s`: what the main thread used since its last turn. **busy**
  (over half the wall time: JavaScript, layout, painting) or **waiting** (a
  lock, a sync IPC reply, I/O).
- Frames, innermost first, in the profiler's notation: hex offsets in the
  unstripped ELF (`toolchain/wpe-webkit/titles/debug/<build>/ps5wk-child`
  for the title that ran), `j<tier>` JIT code (0 thunks/ICs/regexp,
  1 Baseline, 2 DFG, 3 FTL), `x<hex>` absolute addresses in other modules
  (libkernel/libc: the system call it waits in). `t<n>` is the thread's slot
  in the profiler's table; `cpu` that thread's CPU so far.
- `gmain` (GLib's worker) blocks all signals and never answers (costs
  200 ms of the report).
- **Symbolize:** `port/webkit/test/prof/watchdog-sym.py [--elf <ps5wk-child>] <log>`
  (default ELF: the newest in `titles/debug/`) prints each `bt` line with
  its frames (inlined frames expanded, `--no-inlines` for one line per
  frame). By hand: `llvm-addr2line -f -C -i -e <ps5wk-child> 0x<offset>`,
  with offset - 1 for every frame after the first (return addresses).

**Checked on the console** (title, build 20261101-213700, log-only):
`test/paint/stall.html` served by `tools/serve-media.py --port 8776 --page
port/webkit/test/paint/stall.html` (which now also answers `/slow?ms=<n>`
late):

- `?busy` (a 6 s JavaScript loop from a `setTimeout`): report at 3.9 s,
  `its CPU +3.8 s (busy)`, main thread `j3` (FTL code) <- `vmEntryToJavaScript`
  <- `JSC::Interpreter::executeCall` <- `JSC::profiledCall` <-
  `WebCore::ScheduledAction::execute` <- `DOMTimer::fired` <-
  `ThreadTimers::sharedTimerFiredInternal` <- `RunLoop` timer <-
  `g_main_context_dispatch` <- `RunLoop::run`; recovered: stalled 6.0 s.
- `?xhr` (a synchronous XHR answered after 6 s): report at 3.9 s,
  `its CPU +0.0 s (waiting)`, main thread in libkernel <-
  `ThreadCondition::timedWait` <- `ParkingLot::parkConditionallyImpl` <-
  `BinarySemaphore::waitUntil` <- `IPC::Connection::waitForSyncReply` <-
  `sendSync<NetworkConnectionToWebProcess::PerformSynchronousLoad>` <-
  `WebLoaderStrategy::loadResourceSynchronously` <- ... <-
  `XMLHttpRequest::send` <- `jsXMLHttpRequestPrototypeFunction_send` <- LLInt
  <- the same timer path; recovered: stalled 6.1 s. The NetworkProcess's
  main thread kept turning (no report there).
- `?busy&ms=15000` with `PS5WK_PROFILE=threads`: report at 3.9 s, the
  10 s repeat at 14.2 s with the same stack, recovered at 15.0 s; the
  profiler's file was written as usual.
- Ordinary browsing (Wikipedia, Hacker News, GitHub, BBC News, each
  scrolled down and up by `scroll.js`): no watchdog lines besides "on".

## Async scrolling: design (2026-11-02)

Written from a source read only (no console, no build). Goal: on pages
whose WebProcess main thread is busy (last TV log: rendering updates 3-16 ms
avg, p95 41-49 ms, spikes 100-250 ms; 10-30 scroll steps/s while the title
could present 60), the pad scrolls at display rate and the WebProcess catches
up later, with tiles painted ahead through the coverage margin.

### Where scrolling is today (WC)

- **No scrolling coordinator at all.** `Page::scrollingCoordinator()` needs
  `scrollingCoordinatorEnabled`, which `WebPage.cpp:1024-1027` sets only
  if `threadedScrollingEnabled` and `DrawingArea::supportsAsyncScrolling()`
  (false by default, `DrawingArea.h:125`; DrawingAreaWC doesn't override it);
  `DrawingAreaProxyWCPS5::configurePreferencesForNewPage` and
  `WebPreferencesWPE.cpp:54` force threaded scrolling off. Upstream WC
  (Windows; PlayStation's async scrolling is its coordinated
  `LayerTreeHostPlayStation`, not WC) has none either.
- Scrolling is ScrollView on the WebProcess main thread. The root moves via
  `RenderLayerCompositor::updateScrollLayerPosition` (scrolled contents
  layer position = -scroll position), overflow scrollers via the scroll
  container's bounds origin (`RenderLayerBacking::updateScrollOffset`);
  sticky/fixed layers are moved by M2/M3's per-step layer position update.
- Input: `padTick` (`WPEDisplayPS5.cpp`, title main thread, every 8 ms)
  turns the right stick into precise wheel events (14 px x deflection, x2.5
  in WebKit: 35 px per tick = 4375 px/s at full deflection, 875 px/s at ry
  0.2). `WebPageProxy::handleWheelEvent` sends them to the EventDispatcher
  (`shouldSendWheelEventsToEventDispatcher`), which has no scrolling tree and
  hops to the main thread; the reply comes from there, and
  `WheelEventCoalescer` merges everything that arrives meanwhile into the next
  event. On a busy main thread that is the 10-30 large steps/s. The D-pad is
  arrow keys (KeyboardScrollingAnimator, main thread).
- Build (`build-wc`): `ENABLE_ASYNC_SCROLLING=ON`, `ENABLE_SMOOTH_SCROLLING=ON`,
  `ENABLE_SCROLLING_THREAD` on (GLib + coordinated graphics,
  `PlatformEnableGlib.h:66`). `ScrollingCoordinatorCoordinated` and
  `ThreadedScrollingTree` are compiled but unused. `UI_SIDE_COMPOSITING` is
  Cocoa only, and the ScrollingStateTree serializers
  (`Shared/RemoteLayerTree/RemoteScrollingCoordinatorTransaction.serialization.in`
  + `.cpp`) are built only for Cocoa (`DerivedSources.make:907`,
  `SourcesCocoa.txt:255`), although the .in file is plain C++ types.
- Passive by default: wheel listeners on window/document/html/body are
  passive (`Quirks::shouldMakeEventListenerPassive`,
  `passiveWheelListenersAsDefaultOnDocument`), so most pages only block
  scrolling in regions with element-level non-passive listeners.

### Options

**(a) WebProcess scrolling thread** (`ScrollingCoordinatorCoordinated` +
`ThreadedScrollingTree`, wheel events already reach the EventDispatcher
thread). Not usable as is: every node class targets `CoordinatedPlatformLayer`
(`ScrollingStateNodeCoordinated.cpp` downcasts each GraphicsLayer to
GraphicsLayerCoordinated: a crash with GraphicsLayerWC), and the tree assumes
a compositor in the same process. For WC it needs WC node classes writing
thread-safe per-layer scroll positions, plus a second WebProcess -> title
update stream ("positions only", from the scrolling thread) next to
DrawingAreaWC's one-update-in-flight pipeline, with reconciliation when a
main-thread update carries older positions, and a display-refresh source on
the scrolling thread for scroll animations. Latency: pad -> title main ->
IPC -> EventDispatcher -> scrolling thread -> IPC -> title main -> render
thread. The scrolling thread also competes with the main thread and the 4
tile threads for the WebProcess's cores.

**(b) UI-side scrolling tree in the title** (Cocoa's RemoteScrollingCoordinator
/ RemoteScrollingTree model, which exists because the compositor lives in the
UI process, as ours does). The WebProcess keeps a non-threaded
`AsyncScrollingCoordinator` that builds the ScrollingStateTree (frame,
overflow, fixed, sticky, positioned and overflow-proxy nodes with
constraints, scroll limits, layout viewport, event tracking regions,
synchronous scrolling reasons, snap offsets, requested and keyboard scrolls)
and sends its commit with each UpdateWC; layers are named by
PlatformLayerIdentifier, which WC already uses. The title holds a WebCore
`ScrollingTree` per scene: wheel events are handled in the title,
WebCore's generic node code computes the scrolled contents position, overflow
bounds origins and sticky/fixed positions, the render thread applies them to
the WCScene's layers just before painting, and the title tells the
WebProcess where it scrolled (`ScrollUpdate`, applied with
`AsyncScrollingCoordinator::applyScrollUpdate`, which moves the frame view,
fires scroll events and runs IntersectionObserver at its next rendering update).

**(c) Main-frame offset only in the title** (scroll layer id, position and
limits sent with UpdateWC, fixed layers counter-moved). Cheapest first step,
but sticky constraints are only computed when a scrolling coordinator exists
(`RenderLayerCompositor` builds them for scrolling nodes), and programmatic
scroll reconciliation, wheel regions and later overflow scrollers would be
reinvented outside WebKit's tested code.

**Recommendation: (b), staged so its first async step is (c)'s scope (main
frame only) built on the full tree.** Reasons:
- Latency: zero IPC on the scroll path. A pad tick changes the tree on the
  title main thread (microseconds, under the tree lock) and the next render
  thread frame shows it: the same latency as the drawn pointer.
- The title owns both the pad and the vsync. Only here can the stick be
  integrated per frame at the frame's display time (125 Hz pad ticks over
  60 Hz frames give 2 or 3 ticks per frame: steps of 70/105 px at full
  deflection, visible judder even at 60 fps), and only here does the scroll
  position at present time meet what tiles the scene has (clamp at the painted
  edge, below).
- Reuse: WebCore's ScrollingTree, its node bases (`ScrollingTreeStickyNode::computeConstrainingRectAndAnchorLayerPosition`,
  `ScrollingTreeFixedNode`, latching, `ScrollingEffectsController` for
  snapping, keyboard and smooth programmatic scrolls), the state tree and its
  generated serializers. New code is the WC glue: a WebProcess coordinator
  (~300 lines after `RemoteScrollingCoordinator.mm`), title tree + 6 node
  classes + delegate (~1000, after the Coordinated ones, which are 2.3k
  lines incl. scrollbars), host/scene hooks (~300).
- The WC pipeline stays as it is: one update in flight, reply after the
  swap, rAF paced by presents. Scroll frames are title-only frames like M3's
  animation frames and M4's video frames.
- VR keeps working without change: the VR loop runs `applyPending` (scroll
  commits are applied there) and `didPresent`, never paints, and the pad is
  captured by VR (no scroll input); positions are applied at the next TV paint.

### Design (b)

**WebProcess** (`WebProcess/WebPage/wc/WCScrollingCoordinator.{h,cpp}`, new):
- `AsyncScrollingCoordinator` subclass, made by
  `WebChromeClient::createScrollingCoordinator` when the page's drawing area
  is DrawingAreaWC (run-time choice; `ScrollingCoordinatorCoordinated`
  otherwise). `coordinatesScrollingForFrameView` true for composited frames;
  `scheduleTreeStateCommit` -> `DrawingAreaWC::triggerRenderingUpdate`.
- On by preference: the title sets `threadedScrollingEnabled` for WC pages
  when the flag is on (`configurePreferencesForNewPage`), and
  `DrawingAreaWC::supportsAsyncScrolling()` returns true. `registerScrollingTree`
  stays a no-op (no EventDispatcher tree).
- `DrawingAreaWC::sendUpdateAC`: after the layer flush,
  `commitTreeStateForRootFrameID(frame, LayerRepresentation::PlatformLayerIDRepresentation)`
  goes into the UpdateWC message as an optional transaction (reusing
  `RemoteScrollingCoordinatorTransaction`: guard
  `ENABLE(UI_SIDE_COMPOSITING) || USE(GRAPHICS_LAYER_WC)`, the .cpp and the
  .serialization.in added to the WPE WC build). With it: the coverage rect
  of the frame's tiled contents layer (document coordinates; M5 sends an
  update only when all its tiles are painted, so coverage = painted).
- New message `DrawingArea::ScrollUpdatesWC(Vector<ScrollUpdate>) -> ()`:
  applies the newest update per node (`applyScrollUpdate(..., ScrollType::User)`,
  `ScrollingLayerPositionAction::Sync`: `GraphicsLayerWC::syncPosition`/
  `syncBoundsOrigin` keep the WebProcess's layer tree current without
  fighting the title), replies at once. Requested (programmatic) scrolls and
  their responses follow `RemoteScrollingCoordinator::scrollUpdateForNode`.
- Coverage ahead (AS3): GraphicsLayerWC gets the scroll velocity from the
  updates (px/s, smoothed) and extends the margin in the scroll direction,
  below.

**Title** (`UIProcess/wpe/ps5/`):
- `PS5WCScrollingTree` (ScrollingTree subclass, one per scene, owned by
  DrawingAreaProxyWCPS5, shared with SceneHost) and node classes
  `ScrollingTree{FrameScrolling,OverflowScrolling,Fixed,Sticky,Positioned,OverflowScrollProxy}NodeWC`
  that keep layer ids (`layer()` returns null) and write their results into
  a `ScrollingLayerPositions` table (layer id -> position or bounds origin,
  in GraphicsLayer's top-left convention, alignment offset subtracted as
  `ScrollingTreeStickyNodeCoordinated::applyLayerPositions` does).
- Threads: wheel events on the title main thread; commits, animation ticks
  (`serviceScrollAnimations(displayTime)`) and `applyLayerPositions` on the
  render thread; all under the tree's own lock, never while holding
  SceneHost's queue lock. Commits are applied in `applyPending` right after
  the WCUpdateInfo of the same message, so they run in VR too.
- `WebPageProxy::handleWheelEvent`: PS5 WC branch (as `PLATFORM(MAC)`'s
  scrolling-coordinator branch) -> `DrawingAreaProxyWCPS5::handleWheelEvent`:
  `determineWheelEventProcessing` (event tracking regions, synchronous
  reasons) + latching + `handleWheelEvent` on the tree, then
  `continueWheelEventHandling(result)`: handled in the title and nothing
  blocking -> completed at once (the coalescer never waits for the
  WebProcess), DOM `wheel` listeners still get it as NonBlockingDOMEventDispatch
  via `WebPage::HandleWheelEvent` (`shouldSendWheelEventsToEventDispatcher`
  false while the tree is on); in a blocking region or with synchronous
  reasons -> SynchronousScrolling + BlockingDOMEventDispatch to the WebProcess as
  today (a preventDefault blocks it; the main-thread scroll comes back as a
  state tree position).
- Hit testing (which node scrolls at the point; pointer or centre):
  root node until AS4; then a WCScene helper walking layer geometry
  (positions, transforms, masksToBounds clips) for scroll container layers.
  Event regions aren't sent in WC; a pointer-events:none overlay over a
  scroller can pick the wrong node (accepted, measured).
- SceneHost::paint: tick animations, apply layer positions
  (`WCScene::setScrollingLayerPositions`, re-applied after every `apply()` so
  the title's positions win over a WebProcess update carrying older ones),
  mark the scene dirty when anything moved. `didPresent` posts the presented
  positions to the main thread, which sends `ScrollUpdatesWC` with at most one
  message in flight (the next goes when the WebProcess acks: a stalled main
  thread gets one coalesced update, not 15).
- Pacing: the "frame without an update waits for the expected one" rule
  (TV test 2) waits until 6 ms before the frame is due; while the title is
  scrolling it waits only until `displayUS - max(6 ms, smoothed scene paint + 2 ms)`
  so a heavy scene doesn't miss the vsync waiting for a late WebProcess.
- Lifetime: the tree goes with its scene (crash, page restart, GL-cap
  fallback); a new drawing area starts with a full commit.

**Behaviour that must stay right:**
- Sticky/fixed: positions come from WebCore's constraint math every frame
  in the title, so MDN-style sticky headers are exact during async scroll
  (no "one step behind"). M3's PS5 sticky compositing and b235085eb2's
  scroll order are for main-thread scrolling only; with a coordinator the
  upstream paths (`isAsyncScrollableStickyLayer`, `hasCoordinatedScrolling`)
  apply, and the PS5 changes in `RenderLayerCompositor.cpp` /
  `LocalFrameView.cpp` must step aside then (check each).
- Overflow scrollers: main-thread (sent to the WebProcess) until AS4, then async with
  latching and scroll chaining from the tree.
- Scroll-linked JS: scroll events and IntersectionObserver run when the
  WebProcess applies the position, as in every async-scrolling browser.
  JS-positioned effects (parallax, hide-on-scroll headers) lag while the main
  thread is behind; CSS sticky/fixed don't. Lazy images load when it catches
  up. No per-site handling; the flag turns it all off.
- preventDefault: blocking regions go synchronous (above). Root listeners are
  passive by default, so a page-wide blocking region needs an element-level
  non-passive listener.
- Keyboard (D-pad) and smooth programmatic scrolls: the key still goes to the
  main thread first (it may be prevented); the animation then runs in the
  title (`RequestedKeyboardScrollData`, `RequestedScrollData` animated),
  ticked per frame on the render thread.
- Snapping: snap offsets are in the state tree; ScrollingEffectsController's
  discrete-snap timer runs on the title main RunLoop.
- Synchronous reasons (e.g. `background-attachment: fixed`, non-composited
  viewport-constrained objects): that frame scrolls on the main thread as
  today.
- Scrollbars: the WebProcess still paints the thumb (M1's generic
  controller), so it lags the content while the main thread is behind. A
  title-drawn thumb for the main frame is an option later.
- GL cap: ReduceCoverage (no margin) leaves nothing painted ahead, so the
  clamp below holds async scroll at the visible rect: correct, WebProcess-paced.

### Coverage and outrunning the tiles

Today: margin max(512, half the visible height) = 540 px above and below
at 1080p, tiles aligned to 512, kept one more row: 540-1051 px painted
ahead. Time until a still WebProcess is outrun:

| stick | px/s | painted ahead lasts | 250 ms stall overruns by |
| --- | --- | --- | --- |
| ry 0.2 | 875 | >= 0.62 s | 0 |
| ry 0.5 | 2190 | >= 0.25 s | up to 7 px |
| ry 0.8 | 3500 | >= 0.15 s | up to 335 px |
| full | 4375 | >= 0.12 s | up to 554 px |

- **Velocity-aware margin (AS3):** ahead = clamp(v x 0.35 s, 540, 2 x view
  height) in the scroll direction, behind = one tile row while v > 1000 px/s;
  0.35 s covers the TV log's p95 plus a 250 ms stall plus one async tile
  batch. Only for the frame's contents layer and tiled layers over 1 Mpx
  under the scrolled node (not for every card of a feed). After a stall
  the WebProcess needs one rendering update plus the new rows' tiles
  (~1 Mpx per 512 px row at 1920 wide, 2-7 ms on the pool) to catch up.
- **Memory:** a 512 px row of the 1920-wide root is 4 tiles = 4 MB of GL
  texture. Today ~8 rows (~32 MB); at full speed 512 behind + 1080 + 1530
  ahead + kept rows ~10 rows (~40 MB), i.e. +8 MB on the root, temporary.
  Typical pages run 15-55 MB median, the worst seen 137 MB (`wcfeed.html`,
  mostly texture-pool surfaces) against the 128 MB log / 160 MB cap; the
  extension is dropped when the 1 s average is over 128 MB.
- **Outrunning: clamp by default.** The title never scrolls the frame past
  the painted coverage rect it got with the last applied update (async
  scroll stops at the edge until the WebProcess catches up: never worse than
  today, never blank). Option `checkerboard=<px>` in the flag file allows up
  to that many px past it, drawn in the frame's background colour, for an A/B
  on the TV. Other layers' missing tiles show their background (rare: each
  has its own margin). Both counted in the log.

### Milestones

Flag file `/data/ps5webkit/wc-async-scroll` (WC builds, read for each new
WebProcess like `compositing`, so Restart page applies it; tokens:
`overflow=0|1 stick=events|frame checkerboard=<px> lookahead=<ms>`).
Absent = today's behaviour exactly.

| | what | pass (console, log-only unless noted) | effort |
| --- | --- | --- | --- |
| AS0 | Baseline instrumentation + test page; no behaviour change. WCUpdateInfo carries the newest wheel event time the rendering update consumed; the title logs pad -> present latency, scroll frames/s, WebProcess updates/s | numbers for `scrolljank.html` (below) at busy 0/30, spikes, ry 0.2/0.5/1.0, and the TV log's heavy feed pages | 1 day |
| AS1 | WCScrollingCoordinator + transaction with UpdateWC + title tree applying layer positions; all wheel events still go to the WebProcess (synchronous) | captures equal AS0's (`shot-diff.py` 0 %) on `sticky.html`, `layers.html`, `static.html`, docs pages; MDN sticky header 0 rows off mid-scroll (pad-scroll-test); updates/s and main-thread ms not worse; tree dump (node counts by type) per page | 4-6 days (serializer build risk) |
| AS2 | Async main-frame scrolling in the title: wheel handling in DrawingAreaProxyWCPS5, ScrollUpdatesWC, clamp at the painted edge, overflow and blocking regions to the WebProcess | `scrolljank.html?busy=30`: >= 58 scroll frames/s with the WebProcess at ~25 updates/s; pad -> present p95 <= 35 ms (vs AS0); sticky/fixed 0 rows off in mid-scroll captures; SJ scroll events keep coming; lazy images all load; `?prevent` region doesn't scroll; VR enter/leave cycles (`vr-sim` + `vr-guesstest`) PASS | 4-5 days |
| AS3 | Velocity-aware coverage, `checkerboard=` option, GL-cap interplay | clamped frames and px per second at ry 0.5/1.0 with 100/250 ms spikes (AS2 vs AS3); GL textures under the cap on `wcfeed.html` while scrolling | 2-3 days |
| AS4 | Overflow scrollers and iframes async (WCScene hit test, overflow/positioned/proxy nodes, latching, chaining at the limits) | `sticky.html` sidebar and table header, `layers.html` overflow box, scrolljank's scroller: captures equal AS1's, async share of wheel events logged | 3-4 days |
| AS5 | Frame-locked stick (`stick=frame`): padTick publishes the stick velocity, the render thread advances the latched node by v x (display time - last display time); wheel events still go to DOM listeners, non-blocking. Keyboard/programmatic smooth scrolls ticked per frame | per-frame scroll step stddev ~0 while held (`[frame]` lines get `scroll dy`); D-pad scroll smooth under `?busy=30` once started | 2-3 days |
| AS6 | Scene paint for scroll frames on many-layer pages (skip off-screen layer subtrees, not only tiles); optional title-drawn main-frame thumb; TV check, then default on | `scrolljank.html?layers=2500`: scene paint <= 8 ms (YouTube-like pages paid ~18 ms per frame for 2772 layers, which caps any title-side scrolling at 30-55 fps) | 2-4 days |

**Log lines** (title, 1 s windows while scrolling, `[ascroll]`): frames
that moved (title-only / with an update), wheel events async vs sent to
the WebProcess (blocking region, synchronous reasons, sub-scroller, off),
lead over the WebProcess (title position - position in its last update:
max/avg px), clamped frames and px, checkerboard px, pad -> present latency
avg/p95/max (wheel event time to the swap that first shows it), ScrollUpdates
sent/acked and the WebProcess's apply delay. WebProcess: `[render] wc scroll:`
updates applied, coalesced. `title-ascroll-summary.py` (after
`title-scroll-summary.py`) turns them into the tables above.

**Test page** `test/paint/scrolljank.html` (synthetic content only): 200
posts of text, CSS-gradient "thumbnails" (each with a distinct stripe, so
missing tiles show as background in captures), translucent `will-change`
cards, a sticky page header, sticky section headings, a fixed bottom bar, an
`overflow: auto` box with its own sticky header, a scroll-snap strip, lazy
images (IntersectionObserver, `make-images.py` thumbnails), a region with a
non-passive wheel listener. Knobs: `?busy=<ms>` (busy-wait in every rAF:
style/JS-heavy main thread), `?spike=<ms>&every=<ms>` (long tasks),
`?layers=<n>` (tiny composited layers, scene-paint stress), `?parallax`
(scroll-linked transform, expected to lag), `?prevent` (the region calls
preventDefault). Logs `SJ` lines: scroll events/s, scrollY per rAF, long
tasks, image loads, preventDefault count. Driven by `pad-scroll-test`
(ry 0.2/0.5/1.0, mid-scroll captures; header strip and stripe checks as in
TV test 2) and `scroll-raf.js`.

### Patches

- WebCore: `rendering/RenderLayerCompositor.cpp` and `page/LocalFrameView.cpp`
  (PS5 main-thread-scrolling changes only without coordinated scrolling);
  `platform/graphics/texmap/TextureMapperLayer.{h,cpp}` (combined transform
  for hit testing and subtree culling, AS4/AS6). ScrollingTree code itself
  unchanged if possible (a null `layer()` must be safe in the paths we use).
- WebKit: `Shared/RemoteLayerTree/RemoteScrollingCoordinatorTransaction.{h,cpp}`
  (guard), `CMakeLists.txt` / `PlatformWPE.cmake` / `Platform/WC.cmake`
  (serialization + sources), `WebProcess/WebCoreSupport/WebChromeClient.cpp`,
  `WebProcess/WebPage/wc/{DrawingAreaWC,GraphicsLayerWC}.{h,cpp}`,
  `WCUpdateInfo.h`/`.serialization.in`, `WebProcess/WebPage/DrawingArea.messages.in`,
  `UIProcess/DrawingAreaProxy.{h,messages.in}`, `UIProcess/WebPageProxy.cpp`
  (wheel branch), `GPUProcess/graphics/wc/WCScene.{h,cpp}`,
  `UIProcess/wpe/ps5/{DrawingAreaProxyWCPS5,PS5WCHost}.cpp`. New:
  `WebProcess/WebPage/wc/WCScrollingCoordinator.{h,cpp}`,
  `UIProcess/wpe/ps5/PS5WCScrollingTree.{h,cpp}`, `PS5WCScrollingNodes.{h,cpp}`.
  About 2-2.5k lines.
- Outer repo: `wpe-ps5/WPEDisplayPS5.cpp` and `PS5Compositor.h` (AS5 stick
  velocity to the render thread), test page and summary script; a Settings
  item once it is the default.

Total about 3-4 weeks of work at the plan's usual pace.

### Risks, in order

1. **WebProcess behaviour once a scrolling coordinator exists:** coordinated
   scrolling switches paths in LocalFrameView/RenderLayerCompositor (layer
   positions, scrollbars, event regions on the main thread), next to our M2/M3
   PS5 changes. AS1 exists to find this with the title still main-thread paced.
2. **Serializers outside Cocoa:** the generated ScrollingStateTree coders
   and `RemoteScrollingCoordinatorTransaction.cpp` may pull in Cocoa-only
   types; fallback is a smaller hand-written WC transaction of the same nodes.
3. **Scene paint cost** on many-layer pages bounds title-side scrolling,
   whatever the WebProcess does (AS6).
4. **Sticky/fixed exactness** (anchor/alignment conventions between
   GraphicsLayerWC, the constraints and TextureMapperLayer): checked by
   mid-scroll captures on every milestone.
5. Threading in the title (tree lock vs SceneHost lock; main thread wheel
   handling vs render thread commits and ticks).
6. Scroll-linked JS lag and clamp-vs-checkerboard feel: product calls for
   the owner on the TV, both switchable.
7. Coverage memory under the GL cap (bounded, dropped over 128 MB).

### Async scrolling: results (2026-11-02)

Built as designed, with AS1-AS3 in one step (src-wc 8b7e3617ac): the
title's tree handled wheel events from the first working build, so there is
no separate "positions only" stage. Everything is behind
`/data/ps5webkit/wc-async-scroll` (read for each new WebProcess: Restart page
applies it). Without the flag the title sets threaded scrolling off as
before, no scrolling coordinator is made, and the only differences are two
fields in UpdateWC that nothing reads unless `/data/ps5webkit/wc-scroll-log`
is there.

**What runs where**
- WebProcess (`WebProcess/WebPage/wc/WCScrollingCoordinator.{h,cpp}`): a
  non-threaded AsyncScrollingCoordinator, made by WebChromeClient for
  DrawingAreaWC pages when threaded scrolling is on. Its state tree commit
  goes with each main frame UpdateWC as a
  `RemoteScrollingCoordinatorTransaction` (that file and its
  `.serialization.in` are now built for WC; `ScrollbarHoverState`'s GLib
  fields are `[NotSerialized]`). It applies title scroll updates as
  RemoteScrollingCoordinator does, including the request/response ordering for
  programmatic scrolls. Its own `setScrollPinningBehavior` is a no-op
  (AsyncScrollingCoordinator's version needs a local tree; the first build
  crashed there at page creation).
- Title (`UIProcess/wpe/ps5/PS5WCScrollingTree.{h,cpp}`): WebCore's
  ScrollingTree with WC nodes (frame, overflow, fixed, sticky, positioned,
  overflow proxy) that write layer positions and bounds origins into a
  table. Each frame the render thread services scroll animations at the
  frame's display time, runs `applyLayerPositions` (the tree only marks
  positions dirty on a scroll), and sets the table on the WCScene
  (`WCScene::setScrollingLayerPosition`, re-applied after every update that
  resets them). The first measured build skipped `applyLayerPositions`, so the
  root's scroll position moved but no layer did, and its numbers were thrown
  away. Commits are applied in `applyPending`, so the VR loop applies them too.
- Wheel routing (`DrawingAreaProxyWCPS5::handleWheelEvent`, from
  `WebPageProxy::handleWheelEvent`):
  - handled in the title unless the point is over a sub-scroller (overflow or
    iframe container rects, computed on the render thread after each paint),
    the root has synchronous scrolling reasons, or the event tracking region
    says a blocking listener is there;
  - those cases go to the WebProcess first (`WebPage::HandleWheelEvent`, not
    the EventDispatcher), and its default handling names the node, which the
    title's tree then scrolls (`handleWheelEventAfterDefaultHandling`);
  - title-handled events complete at once for the coalescer, and go to the
    page's passive listeners as non-blocking DOM events (one in flight,
    newest kept).
  - With WHEEL_EVENT_REGIONS on (GLib), blocking listeners live in per-layer
    event regions that WC doesn't send, so PS5 adds the bounds of nodes with
    non-passive wheel listeners to the frame's synchronous region
    (`Document::ps5AbsoluteRegionForNonPassiveWheelEventTargets`). Root
    listeners stay passive by default.
- Painted edge (AS3): the WebProcess gets the title's root scroll velocity
  with each `ScrollUpdatesWC` and makes tiles ahead in that direction
  (velocity x 0.35 s, at most two visible heights,
  `PS5WK_WC_LOOKAHEAD_MS`). Each update reports how far the frame's tiles
  reach above and below the viewport, and the title clamps a wheel event's
  delta so it never scrolls past that (`checkerboard=<px>` in the flag
  file lets it go that much further). Reduced coverage (GL cap) means 0:
  scrolling then waits for the WebProcess, as before.

**Measured** (log-only PPSA99020, build 20261102-032153-edfee43cea, same
binary for both modes, flag on/off; `test/paint/scrolljank.html`
uploaded to the console; `pad-scroll-test` = the stick as wheel events every
8 ms, 4 s down / 4 s up / 1 s still. The fast row uses ry 1.0: 35 px per
event, 4375 px/s. "Moving frames/s" = presented frames that moved the page,
in seconds with at least 20 pad events. "pad -> present" = oldest pad event
not yet shown until the swap of the first frame that moved the page: avg of
per-second averages / worst per-second p95 / max. `title-ascroll-summary.py`):

| scrolljank page | mode | moving frames/s | pad -> present ms | WebProcess updates/s | lead over the WebProcess, max | clamped |
| --- | --- | --- | --- | --- | --- | --- |
| idle main thread | off | 58.4 | 13.3 / 17.4 / 24.1 | 60.0 | | |
| | async | 59.4 | 12.8 / 17.3 / 30.4 | 60.0 | 54 px | 0 |
| `?busy=30` (30 ms busy-wait per rAF) | off | 25.6 | 31.9 / 40.2 / 44.8 | 28.2 | | |
| | async | **54.3** | **13.8 / 32.8 / 36.2** | 18.7 | 216 px | 0 |
| `?busy=8&spike=200&every=700` | off | 38.3 | 20.9 / 211 / 222 | 44.6 | | |
| | async | **49.6** | **13.7 / 33.0 / 104** | 24.9 | 540 px | 0 |
| `?busy=30&spike=250&every=1000`, ry 1.0 | off | 18.2 | 45.9 / 332 / 332 | 20.6 | | |
| | async | **47.6** | **13.7 / 94.1 / 248** | 14.0 | 1505 px | 62 events (2117 px) |

- Presents ran at 60/s through the scrolling seconds with async on (54/s in
  5 s windows that include the still second). Title scene paint stayed at
  2.3-2.6 ms on this page (89 layers).
- The WebProcess's own update rate drops with async on (busy=30: 28 to 19/s).
  It now also applies title scroll positions, and the page has a wheel
  listener, so every title-handled event is also dispatched to the DOM (a
  hit test each). The page still moves at display rate. Worth checking
  whether one DOM wheel event per frame would do.
- In the fast row the lead reached 1505 px (velocity-extended coverage), and
  62 events were shortened at the painted edge: the page stopped briefly at
  the edge of painted tiles instead of showing unpainted area.
- Sticky and fixed: 6 of 6 mid-scroll captures (`?busy=30`, ry 0.5) have the
  sticky header (rows 0-31 red, 32-33 border) and the fixed bar (rows 516-539)
  exactly where they are at rest; section headings and lazily loaded
  images in place.
- `?prevent` (a non-passive listener that calls preventDefault, spanning the
  column at the scroll point): once it reached the scroll point, 1484 events
  went to the WebProcess first, 1459 were prevented, and the page stayed
  put. Before that, 303 events scrolled in the title.
- Sub-scrollers at the scroll point (`?inline`, earlier runs): the events go
  to the WebProcess, the overflow box scrolls, and the page around it
  doesn't (same as without the flag).
- Without the flag on the same binary: WC runs match the AS0 baseline. With
  the compositing flag absent the CPU path paints on 4 threads and scrolls
  as before (`[render] painting on 4 threads`, 24 scroll events/s at busy=30).

**Not done yet / next:**
- VR: the VR loop applies commits (`applyPending`) and never paints. No
  enter/leave cycle has been run with the flag on (`vr-sim` +
  `vr-guesstest`).
- Overflow boxes and iframes scroll async only after the WebProcess's
  answer (AS4: title hit test plus their nodes, which already exist in the
  title's tree).
- Frame-locked stick (AS5): pad ticks at 125 Hz over 60 Hz frames still
  give 2 or 3 ticks per frame.
- Scene paint culling for many-layer pages (AS6).
- Real-site checks on the TV (a heavy feed, the MDN sticky header).
- Measurement caveats: the numbers are from the synthetic page only. A
  console session by another agent overlapped some earlier runs (series
  screenshots every 250 ms), and those runs were discarded.

## Music section thumbnails (2026-11-02)

Owner: in GPU mode, thumbnails on the video site's Music section (the music
channel's page, which signed-out sessions can reach) load very slowly, while
they now come in fast everywhere else on the site.

**Cause: mostly the network, in both modes.** On that page each playlist
card's cover is a 1200x1200 PNG (served as `maxresdefault.jpg`,
`image/png`) of 0.66-2.57 MB (20 sampled). The page script chooses it as
`src` for a 314x176 box (no `srcset`), and the same image is drawn a second
time as the card's blurred stack backdrop. One screen holds ~20 covers, and
the page sets `src` on ~10-30 more just off screen: ~30 MB per screen. Other
pages use 480x360 JPEGs (~20-50 KB). The console's link measured ~5 MB/s:
a 20 MB download in 3.9-4.0 s, 20 x 100 KB in parallel in 0.26-0.31 s,
LAN ping 18-75 ms, FTP 2-7 MB/s. The Mac on the same network got 19 MB/s.
The soup session's limits (256 connections, 6 per host) are Chrome's.
With a cold HTTP cache (cache directory moved aside for each run; JS
timings of every visible `img` from navigation start; scroll steps of 0.8
viewport every 3-4 s):

| run | first screen: all visible covers loaded | step 1 | step 2 | step 3 |
| --- | --- | --- | --- | --- |
| GPU (WC), cold | 8.5 s | 3.7 s | 3.3 s | 1.0 s |
| GPU (WC), cold, earlier run | 7.9 s | 3.9 s | > 4 s | 2.5 s |
| CPU path, cold | 10.7 s | > 5 s | > 4.4 s | 2.1 s |
| CPU path, covers in the HTTP cache | 5.2 s | 0.8 s | 0.9 s | 0.7 s |

An earlier "CPU is fast" result came from a CPU run that came right after
a GPU run, with the covers already in the HTTP cache. Covers complete one
after another, 0.3-0.5 s apart, at the link's rate. Once loaded, they show
within 0.25-0.8 s in both modes (250 ms captures).

**Not the GL texture cap:** GL textures were 71-97 MB on this page
(624-641 layers), under the 128 MB log line and the 160 MB cap. The
128/160 MB events in the TV log were on other pages of the site.

**What the main thread does on this page** (sampling profiler, 45 s: load,
4 scroll steps, 5 jumps back to loaded content; WC vs CPU, same script):

| | WC | CPU path |
| --- | --- | --- |
| main thread CPU | 18.5 s | 26.8 s |
| PNG/JPEG decode in painting (synchronous) | 1.30 s | 1.84 s |
| CSS `filter: blur()` of the backdrops, in software while recording | 0.57 s | 6.5 s (all filters 9.8 s) |
| recording (WC tiles / CPU frame) | 2.8 s | 16.5 s |
| `computeCompositingRequirements` | 3.9 s (21 %) | 0 |

- The memory cache is 96 MB (`DocumentBrowser`, RAM >= 2 GB). A cover costs
  ~1.5 MB encoded + 5.8 MB decoded, so the encoded covers alone exceed it.
  The decoded data of any cover not drawn for 1 s and not in the viewport is
  pruned: 11-25 large frames destroyed per 5 s while scrolling. Each revisit
  decodes again, synchronously, 40-60 ms per cover (0.4-0.8 s of main
  thread per 5 s window). Raising the capacity was considered but not done:
  WebProcess memory is the tightest resource (1.3 GB RSS seen on the TV).
- Scroll jumps back to loaded content (250 ms captures): the CPU path shows
  them in 0.25-0.5 s; WC in 0.5-1.25 s, plus stalls of 2.5-3 s in some of
  them (below).

**Fixed: painting waited on an image's background decode** (src-wc
730265f633, `BitmapImageSource::frameAtIndexCacheIfNeeded`, both modes).
Upstream decodes large off-screen images (e.g. in WC's coverage margin) on
the image's decoding queue. `ScalableImageDecoder` holds its lock for the
whole decode. Painting that image meanwhile (`RenderImage::paintReplaced`
-> `currentFrameIsComplete` -> `cacheMetadataAtIndex` ->
`frameHasAlphaAtIndex`) blocked the main thread on that lock until the
decode ended. A 1 s watchdog (`PS5WK_WATCHDOG_S=1`) caught a 2.5 s stall of
the whole update this way, the main thread "waiting". Now the metadata
read is skipped while a decode of that image is in flight. The frame counts
as not decoded yet, and the decode's completion caches the metadata and
repaints its waiting clients. `PS5WK_DECODE_METADATA_WAIT=1` (child.env)
waits, as upstream. With it: 12-28 skipped reads per 5 s while loading,
and covers drawn normally after jumps (captures checked). The A/B runs are
noisy (network, page content changes per load); the evidence is the
backtrace. The same commit adds a `[render] image decodes` line (every 5 s
while images are decoded): main-thread decodes with ms and large-image
count, asynchronous requests and results, large frames destroyed,
metadata reads skipped.

**Tried and dropped:** decoding large images asynchronously when painting
WC tiles (instead of synchronously in the recording). After a jump, covers
stayed gray for more than 5 s: decodes completed (25 per 5 s), but the
frames were destroyed and asked for again. The two clients per cover (main
image and blurred backdrop, different draw sizes) and the cache pruning
above are the likely reasons. Not committed.

**Open:**
- A second kind of 2.5-3 s stall on some jumps (2 of ~6 runs, both modes of
  the knob). The main thread does not answer the watchdog's signal ("its
  CPU unknown", then "+0.7 s (waiting)"). In 2 of 3 cases it came right
  after a new 8 MB tile-arena chunk (`SharedMemory::allocate`: `shm_open`
  + `mmap`, then the title maps it). Not resolved. Next step: time
  `addChunk` and the title's map, or pre-allocate the arena chunks.
- The software blur of the backdrops (0.57 s in WC, 6.5 s on the CPU
  path) runs on the main thread because filters are applied when recorded.
  Moving it to the replay threads (a Skia image filter in the picture)
  would be a bigger change.
- Visible-first image priority and HTTP/2 to the image host were not
  looked into. At ~5 MB/s, a screen of these covers takes ~6 s whatever
  the order.

**The 2772-layer / 18 ms scene paint** in the TV log is a different page
of the site. On this page: 624-641 layers, scene paint 11-15 ms per frame
(home page: 7.6 ms). It costs frame rate, not thumbnail time (covers are
gated by the network). TextureMapper already skips invisible layers
(opacity 0, empty). The WebProcess side is `computeCompositingRequirements`
at 21 % of the main thread. No cheap win found; the touch-feedback overlay
layers (see "Thumbnails slower to show") are the candidate.

**For the owner, on the TV:** open the Music section once with GPU
compositing off and once with it on. Both should be about as slow while
the covers download (several seconds per screen on this link), and both
quick when revisited within the HTTP cache's life. A wired connection
would help here: the console's link measured ~5 MB/s, LAN ping 18-75 ms.

### Async scrolling: second round (2026-11-02, src-wc e8f6923ada)

New flag file tokens: `/data/ps5webkit/wc-async-scroll` content, e.g.
`stickframe=1 overflow=1`. Early replies are on by default.

- **Early replies** (default; `earlyreply=0` turns them off). With async
  scrolling the title paints every frame of a scroll itself, so the
  WebProcess's update found a frame already in progress. Its reply came after
  the following swap: `frame done` 24 ms against 4 ms without the flag, and
  `?busy=30` updates fell from 28 to 19/s. The cause was not the DOM wheel
  dispatch: `?nowheel` (no wheel listener) ran the same 18.8/s. Updates of an
  async-scrolling scene are now answered when applied, since their tiles have
  been copied out of the arena by then. Result: `frame done` 12.6 ms,
  22.6 updates/s.
- **Frame-locked stick (AS5, `stickframe=1`).** Title-handled pad wheel events
  become a velocity (smoothed, per axis) that the render thread integrates
  over each frame's display-time interval, within what has been queued, all
  of it once the stick stops for 30 ms, catching up if more than 3 frames'
  worth waits. The painted-edge clamp applies per frame.
- **Async sub-scrollers (AS4, `overflow=1`).** The innermost overflow or
  iframe container rect (surface rects from the last paint) under the point
  scrolls in the title, chaining to its ancestors at its limits
  (`EventTargeting::Propagate`), unless that node scrolls synchronously.
  Without the token the WebProcess picks the node, as before.

Same method as the first round (build 20261102-035852-730265f633 unless
noted; "step sd" = standard deviation of the root's per-frame scroll step, ry
0.5 ≈ 34 px per frame):

| scrolljank page | mode | moving frames/s | pad -> present ms | step sd | WebProcess updates/s |
| --- | --- | --- | --- | --- | --- |
| `?busy=30` | off | 25.9 | 32.0 / 70.1 / 70.1 | | 28.2 |
| | async, earlyreply=0 | 56.6 | 13.4 / 32.6 / 34.1 | 13.8 px | 18.7 |
| | async (early replies) | 57.8 | 12.1 / 17.2 / 32.6 | 5.9 px | 22.6 |
| | async + stickframe=1 | **59.7** | **12.0 / 17.3 / 19.5** | **3.7 px** | 22.6 |
| idle main thread (build 20261102-033639, no early replies) | async | 59.5 | 13.0 / 17.2 / 25.1 | 7.0 px | 60.0 |
| | async + stickframe=1 | 59.6 | 12.6 / 17.1 / 27.0 | 2.0 px | 60.0 |
| `?busy=8&spike=200&every=700` | async + stickframe=1 | 53.8 | 12.9 / 17.2 / 179 | 5.6 px | 42.9 |

**Real pages** (build 20261102-032153; flag on/off, ry 0.5, 40 s from load):
- reddit front page (the TV log's heavy feed): moving frames/s 5.8 to
  **41.2**, pad -> present 317 to **28 ms** avg. The WebProcess ran 5-8
  updates/s while the page loaded and settled. 1332 wheel events were
  shortened at the painted edge (lead up to 1476 px): the page stopped at the
  edge of painted tiles rather than showing unpainted area.
- MDN reference page with sticky header, breadcrumbs, sticky sidebar and
  table of contents: 6 mid-scroll captures with the flag on and off at the
  same moments. The header and breadcrumb rows (0-48 of 540) are identical
  in every pair; below them only the scroll offset differs (the async run is
  a few px further on). Sidebar and TOC are in place.

**VR with the flag on** (`vr-sim` + `vr-selftest`, video.html with the 1080p
H.264 clip, async + stickframe): `RESULT ps5browser vr PASS headset shows the
video`, 57.6 headset frames/s, the WebProcess kept updating at 56.8/s
(commits applied in the VR loop), no crash. The self-test's full enter/leave
sequence didn't finish within the 60 s dwell in either run (no FAIL line;
same as before this work).

**AS4 check** (build 20261102-034351, `?inline&busy=30`, the overflow box spans the column under
the scroll point, `overflow=1`): 2833 of 2833 wheel events handled in the
title (against 350 sent to the WebProcess without the token). Captures show
the box scrolled (rows 4-11) with its sticky inner header in place and the
page held still around it. Its scrollbar thumb is the WebProcess's (it lags).

**Flag absent on the final build:** WC `?busy=30` 25.9 moving frames/s,
32 ms, the same as the AS0 baseline. With compositing off the CPU path paints
on 4 threads and scrolls (23 scroll events/s).

**Left:** AS6 (scene paint culling for many-layer pages); a title-drawn
scrollbar thumb; AS4 hit testing ignores pointer-events/z-order (a fixed
element over a scroller still routes to the scroller); the owner's TV check.
Suggested TV setting: `/data/ps5webkit/wc-async-scroll` with content
`stickframe=1` (add `overflow=1` to try async overflow boxes), with
`/data/ps5webkit/compositing` on. Then Restart page.

### Scene paint for many-layer pages, tile-arena timing, scrollbar (2026-11-02 night, src-wc 513c01b0c4)

**Where the scene paint time goes.** I added a per-second `[wc] scene per
paint` line (`/data/ps5webkit/wc-scene-stats`): prepare and paint ms, layers
visited, invisible, off screen, blending, surfaces, tiles drawn and culled,
draws, and time in textured quads and in `glDrawArrays`. On
`scrolljank.html?layers=2800&cluster=2500` (2879 layers, about 1100 of them
small composited dots on screen), a paint took 82-96 ms. Of that, 81 ms was
inside `glDrawArrays`: 1142 draws at ~71 us of render-thread CPU each.
Visiting the 2879 layers was cheap: with the dots spread over the whole feed
(74 draws), the paint was 6.3 ms, nearly all of it the 74 draws. Overlap
computations, intermediate surfaces and transforms (prepare 1 ms) don't
matter here. On a video site's search page: 1249 layers, 287 invisible,
106 draws, 8.3 ms.

**Why a draw cost 71 us.** The PS5 GL driver can defer and batch draws.
Its eligibility check (`ps5_multidraw_eligible`) only accepts GL_TRIANGLES,
and its batch summary showed 335k rejects for "input" against 4k eligible.
TextureMapper drew every quad as a GL_TRIANGLE_FAN, so each draw was
prepared and submitted on its own. Caching GL state (program, blend, vertex
buffer, unchanged uniforms) changed nothing measurable (39.6 against 40.5
ms) and was dropped again.

**Fix:** quads are drawn as two triangles (6 vertices), so the driver
batches them. A/B with `/data/ps5webkit/wc-fan-draws`:

| page | draws per paint | paint before | paint after |
| --- | --- | --- | --- |
| scrolljank, 2800 layers clustered | 1142 | 82-96 ms | 39-41 ms (~29 us per draw) |
| scrolljank, 2800 layers spread | 74 | 6.3 ms | ~2 ms |
| video site search page (1249 layers) | ~100 | 8.3 ms | 3.9 ms |
| a heavy feed page (79 layers) | 56 | | 2.1 ms |

Captures (`debug-screenshot`, +10 s, no scrolling) of `layers.html`,
`static.html` and the 2800-dot page are pixel-identical between triangles
and fans. This change is not behind the async flag. It changes how every
WC scene is drawn, with `wc-fan-draws` as the way back. The remaining
~29 us per draw is driver work per draw: descriptors and texture
publication. Going lower needs fewer draws (an atlas for small layers) or
driver work. A 2800-layer page still paints at ~40 ms, so it scrolls at
~25 frames/s even in the title.

**Tile-arena stall** (from the music-section thumbnails round). The chunk
path is now timed in the log. On the console, `SharedMemory::allocate` of
an 8 MB chunk took 0.02-0.03 ms on the WebProcess main thread, and the
title's map took 0.2-0.9 ms. A jump test (`test-onload.js` jumping to far
places every 1.5 s, 24 jumps, a 600-post page, `PS5WK_WATCHDOG_S=1`) grew
the arena to 4 chunks and released them again, twice, with no watchdog
report. The allocation is not the stall. A guess that fits "the main
thread doesn't answer the signal, then waiting": the page faults on
swapped-out memory under the 1.3 GB WebProcess of that site. I haven't
checked it; the kernel VM stats around a stall would show it. A spare chunk
made on a background thread exists but is off (`PS5WK_WC_ARENA_SPARE=1`),
since it doesn't help and would add 8 MB.

**Scrollbar during async scrolling.** The WebProcess paints the frame's
overlay scrollbar thumb at its own scroll position. While the title is more
than 24 px ahead, that layer is hidden (opacity override in the scene), and
it comes back once the WebProcess has caught up. `thumb=0` in the flag file
keeps it. Measured: the scrollbar was hidden and shown once per scroll
burst, and was absent from mid-scroll captures. Adwaita's overlay thumb is
faint anyway (9 px of it in one of 4 captures without the change).

**VR self-test with async scrolling on** (dwell 170 s, `vr-sim` +
`vr-selftest`, async + stickframe): `RESULT ps5browser vr-selftest PASS 15
eye-buffer captures, 8 projections, controls, 4 entries in 72 s; VR off
now`.

**Final checks on this build:** flag absent in WC mode (`?busy=30`: 25.6
moving frames/s, 30 ms pad -> present, as the baseline); CPU path (no
compositing flag) paints on 4 threads and scrolls; async with stickframe on
the 2800-layer page: 45 moving frames/s (bound by the 40 ms scene paint);
search page 52.6 moving frames/s with 3.8 ms scene paints.

### Horizontal carousels under async scrolling (2026-11-02, src-wc b8033dea3f)

Owner, on a big product page with the flag at `stickframe=1`: the
horizontal carousels (overflow-x scrollers with `scroll-snap-type: x
mandatory` and script "paddle" buttons) didn't always sit centred, and
sideways stick scrolling didn't work well. His log had seconds of ~120
wheel events "in the title", 0 sent to the WebProcess and nothing moving.

New test page `test/paint/carousel.html` (synthetic cards; carousels with
snap-align center + paddles calling `scrollBy({smooth})`, snap-align start +
`scrollTo({smooth})`, and no snapping; `?busy=`, `?auto=<ms>` presses the
paddles; `CAR` lines give where each carousel settles and how far the nearest
card is from its snap position). `pad-scroll-test` gained `rx=` (sideways),
`px=`/`py=` (where the events land), `hold=` (still time between the two
directions) and `rest=` (a capture that long into each still period).

**Causes, all async-only** (the same page with the flag absent, WC or CPU,
snaps the paddle scrolls to the cards):
1. **Sub-scroller rects slid with their content.** The title routes the
   stick by the rect of each overflow node's container layer
   (`WCScene::layerRectOnSurface`). A TextureMapperLayer's surface transform
   includes -boundsOrigin, so once a carousel had scrolled by its width the
   rect no longer held the point: the next events went to the root, which
   can't scroll sideways, and were dropped (frame-locked steps nothing took).
   Reproduced on the test page: one second of scrolling (54 events to the
   WebProcess, carousel at 990 px), then 120 events/s that moved nothing.
   Fix: the box is mapped at its bounds origin.
2. **Nothing ended a stick gesture.** The pad's precise wheel events have no
   end phase, so ScrollingEffectsController never snapped (and kept "user
   scroll in progress"). The carousel stopped wherever the stick let go
   (test page: 262 px off). Fix: 100 ms after the last wheel event the tree
   gives the last node a wheel event scrolled an end-of-gesture event, if that
   node snaps; WebKit then animates it to the nearest snap position (no
   fling: kinetic scrolling is skipped for snapping nodes). Stick input stops
   a running animated scroll.
3. **Paddle `scrollBy` landed between cards.** For scrolls by a delta the
   WebProcess sends the unsnapped delta (`AnimatedDeltaUpdate`), and the tree
   just added it: 664 instead of the 728 snap position (64 px off-centre,
   every press). Fix: the tree snaps delta destinations directionally, as the
   WebProcess's ScrollAnimator does.
4. Also: events the root can't take (nothing the title knows of under the
   point, root pinned that way) now go to the WebProcess, whose hit test can
   find a scroller (`unconsumed=0` restores the old drop); events a carousel
   can't take (vertical over it) stay with the root in the title
   (frame-locked) instead of a WebProcess round trip.

New flag tokens (defaults on): `snap=0`, `unconsumed=0`. The `[ascroll]`
line adds: root could not take, frame steps nothing took, sub-scroller moves
in the title, snaps after wheel.

**Results** (log-only PPSA99020, build 20261102-085929; `stickframe=1`
unless noted; stick 1.5 s right, 1.5 s still, 1.5 s left, 2 s still):

| test | events | where it settled |
| --- | --- | --- |
| test page, old behaviour (`unconsumed=0 snap=0`, before the rect fix) | 54 to the WebProcess, then ~120/s "in the title", 0 moved | 990 px, card 3 262 px off |
| test page, ov0 (default) | 1566 to the WebProcess, 0 dropped | 9/9 settles snapped (off 0.0) |
| test page, `overflow=1` | 1579 in the title, 0 dropped | 9/9 snapped |
| test page `?busy=30`, ov0 | 655 to the WebProcess (coalesced) | 6/6 snapped |
| test page `?busy=30`, `overflow=1` | 1250 in the title | 7/7 snapped |
| test page paddles (`?auto=1500`), before fix 3 | | centre carousel 664/1328/1992: 64 px off every time |
| same, after | | 728/1392/2056: snapped, as WC without the flag and CPU |
| product page, gallery at the view centre, rx 0.5 | 3638 to the WebProcess, 5477 gallery moves in the title, 21 snaps | full travel both ways (0 and 3152) |
| product page, rx 0.2, ov0 | 2447 to the WebProcess, 14 snaps | 1432 / 0, card edge on the gutter line (captures) |
| product page, rx 0.2, `overflow=1` | 2462 in the title, 14 snaps | same positions |
| product page, its paddle buttons (clicked from script) | | 0 / 716 / 1432 / 2148, the same snap positions |

Vertical regression check, `scrolljank.html?busy=30` ry 0.5 with
`stickframe=1`: 52.6 moving frames/s, pad -> present 12.2 ms avg (3115 events
in the title, 0 to the WebProcess). Flag absent: no scrolling tree, SJ 25
scroll events/s as before.

**`overflow=1`:** on these pages it behaves the same as the default, and on
a busy main thread it delivers every stick event to the carousel in the title
(1250 against 655 coalesced ones through the WebProcess). Its known gap is
unchanged: the title picks the innermost scroller rect under the point and
ignores z-order and pointer-events (an overlay above a scroller routes to the
scroller). It is a good candidate for the default; not flipped.

Not done: an axis lock for diagonal stick input (a mostly vertical push with
a small sideways part is taken by a carousel under the point, in every mode);
lazily loaded gallery images at the far end showed black in some mid-run
captures of the product page (not compared with the flag off).
