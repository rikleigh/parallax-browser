# Performance overlay

Live numbers in a corner of the TV while browsing: frames, CPU, memory,
GPU compositing, async scrolling, video, network. Drawn by the title on
PS5Renderer's render thread over everything else (page, videos, pointer,
messages), in both the CPU path and the GPU-compositing (WC) path. TV only:
while "View in VR" runs, the render loop drives the headset and the TV
shows its notice; the overlay comes back with the TV.

## Turning it on

- Settings (R3) > Browser > **Performance overlay** (default off): applies
  at once, remembered across starts. It is the flag file below
  (`settings.c`, `ps5browser.c` applies a change live and keeps the file in
  step with the pad shortcut).
- `/data/ps5webkit/perf-overlay` (a file) turns it on at start. Its words
  are options: `pos=tl|tr|bl|br` (corner, default `tr`), `size=<px>` (font
  size at 1080p, default 22), `graph=0` (no frame-time graph).
- Pad: **L3 + R3** (press both sticks in) toggles it. A short message says
  which. L3 (home page) and R3 (Settings) now act when the button comes
  back up instead of when it goes down, and not at all after the chord.
- API (`WPEDisplayPS5.h`): `wpe_display_ps5_set_perf_overlay()`,
  `wpe_display_ps5_get_perf_overlay()`,
  `wpe_display_ps5_set_perf_overlay_callback()` (the chord tells the
  application, to remember the choice) and
  `wpe_display_ps5_set_perf_counter(display, name, total)`: a named counter
  shown on its own line with its rate, for e.g. the ad blocker's "blocked"
  (up to 6 names, any thread, `display` may be NULL).

## What it shows

Text refreshed twice a second; each number covers the half second since
the previous refresh unless it says otherwise.

| Line | Meaning |
|---|---|
| `FPS` | frames presented per second (swaps). With the overlay on, an idle page still gets 2 frames/s (the text refresh). |
| `page` | new page images shown per second: scene paints (WC) or page copies (CPU path) |
| `scroll` | presented frames that moved the shown page, per second (WC; `-` on the CPU path) |
| `miss` | frame intervals over 25 ms inside a run of frames (gaps over 100 ms are idle, not misses) |
| `frame avg/max` | interval between presented frames (ms), runs only |
| `work a/b` | render-thread work per frame before its draws, avg/max (ms): `apply` (page copy / compositor updates) + `video` (decoded frames consumed and converted) + `scene` (compositor paint) |
| `draw` | the frame's draws (ms); includes waiting for the previous frame's flip, so it fills the rest of the vsync |
| `render a/b` | everything before the swap (work + draw), avg/max, as the `[present]` log line's "render" |
| `swap` | `eglSwapBuffers` avg/max (ms) |
| `GPU a/b` | GPU=radv: the GPU time of the render thread's GL work per frame, from its start to the swap (the compositor's scene, the UI, video conversion, the overlay), avg/max (ms), from `EXT_disjoint_timer_query` a few frames late. Not WebGL's or WebGPU's own work, which other contexts submit. GPU=gl: `n/a`, see below |
| `CPU main/render/title` | the title's main (UI) thread and render thread (% of a core; each thread's own `getrusage(RUSAGE_THREAD)`: the thread CPU clocks, `pthread_getcpuclockid`, read ~0 on the PS5, so the `[cpu] title` log line's "render thread CPU" is not meaningful), the whole title process (cores) |
| `web/net` | WebProcess(es) and NetworkProcess CPU (cores; `+` = more than one WebProcess) |
| `MEM flex/direct` | the title's free flexible memory and the direct memory in use (MB) |
| `web rss/pas`, `net` | WebProcess resident and libpas committed memory, NetworkProcess resident (MB) |
| `WC` | `on` (this frame shows the composited scene), `idle` (compositor up, nothing composited shown), or `off` (CPU build, or this page paints on the CPU); pages, layers, GL texture MB (`CAP`: over the GL texture cap, tiles for the visible rect only) |
| `tiles`, `paint` | tile uploads per second and MB/s, scene paint ms (WC) |
| `PAGE` | CPU path: page copies per second, ms per copy, page size |
| `WP` | WebProcess updates applied per second and title-side apply ms (WC); `upd a/b ms n/s`: the WebProcess's own update time avg/p95 and rate, from its `[render]` line (5 s window) |
| `SCRL` | pad wheel event -> the presented frame that moved the page, avg and max (ms); wheel steps stopped at the painted edge (`clamp`) |
| `VID` | open hardware decoders / budget, their direct memory / budget (MB), decoded frames converted per second |
| `NET` | bytes in/out (MB/s, system-wide): TCP payload (`net.inet.tcp.stats`, `(TCP)`) or, where that sysctl is refused, the network interfaces' counters; read by the title, else by the NetworkProcess |
| (counters) | named counters, e.g. `blocked 123 (+2/s)` |
| `OVL` | the overlay's own render-thread time per frame (avg, max) and the last text refresh (ms) |

The graph under the text: one bar per presented frame, newest on the right,
the bar's height the interval to the previous frame (green within 20.8 ms,
yellow within 37.5, red above; grey ticks for idle gaps), with the render
thread's time for that frame in blue inside it; lines at 16.7 and 33.3 ms,
the top is 50 ms.

## Not measurable

- **GPU time per frame** (GPU=gl; GPU=radv measures it): ps5-opengl's timer queries (`GL_TIME_ELAPSED`,
  `GL_TIMESTAMP`) are CPU clocks taken at begin/end
  (`ref/ps5-opengl/src/gallium/ps5/ps5_screen.c`, `ps5_begin_query`), not
  GPU timestamps, so they measure submission, not GPU work. The GPU's share
  shows indirectly in `draw` (its first draw into the window waits for the
  previous frame's flip) and in `frame` intervals above 16.7 ms while
  `render` is short.
- **Network requests per second**: nothing in the title sees requests
  cheaply; bytes per second stand in. A counter can be added
  with `wpe_display_ps5_set_perf_counter()` once something counts them.

## How it works

- `wpe-ps5/PS5PerfOverlay.{h,cpp}`: the overlay. A glyph atlas
  (NotoSansMono-Bold from `/data/ps5webkit/fonts`, printable ASCII, made
  once with FreeType, ~1 ms) and one solid cell; the panel, the glyphs and
  the graph are quads in a persistently mapped vertex buffer (two halves,
  alternating per frame), drawn with one `glDrawArrays`. Text vertices are
  rebuilt twice a second, the graph every frame.
- A sampler thread (four times a second, only while the overlay is on):
  process CPU, flexible and direct memory, TCP counters, the decoder budget
  (`PS5VideoDecoder::budget`). The render thread reads its own
  `RUSAGE_THREAD` at each text refresh, the main thread from a 250 ms GLib
  timeout that runs only while the overlay is on.
- `PS5Renderer`: frame timings (`noteFrame` after each swap), the overlay
  drawn last in `drawScene`, and the idle render loop wakes for the text
  refresh while the overlay is on.
- GPU compositor (`PS5CompositorClient::perfSnapshot`, src-wc
  `PS5WCHost.cpp`): totals since set-up (updates, paints, tile uploads,
  frames that moved the page, pad-to-present latency, clamped wheel steps),
  pages, layers, GL texture MB. The overlay takes differences.
- Children (`apps/child/ps5wk-child.cpp`): a `[perf] cpu <cores> rss <MB>
  pas <MB>` line every second on their output, the NetworkProcess's with
  ` tcp|if <bytes in> <bytes out>` (`PS5WK_PERF_LINES=0` turns them off;
  rss is -1 where `kern.proc` is refused, and the overlay then shows libpas
  only); the title's output relay (`WPEProcessManagerPS5.cpp`) hands every
  line to `ps5PerfNoteChildLine()`, which keeps `[perf]` lines out of the
  log and reads the WebProcess's `[render]` lines.
