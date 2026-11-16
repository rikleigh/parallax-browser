# Spike 14, stage 3: pages in the browser process by default (2026-11-16)

Pages run in the browser title (PPSA99207, Parallax Browser) unless
Settings' "Pages in the browser process" is off
(`/data/ps5webkit/no-inprocess-web`) or Safer browsing is on. Both of those
use payload WebProcesses as before. `/data/ps5webkit/inprocess-web` is no
longer the switch; its KEY=VALUE lines still go to the in-title WebProcess
(debugging).

## What changed for it to be the default

| Gap | Change |
| --- | --- |
| A script that never returns froze the browser's UI and the controller (they share the title's main thread). | Patch 0135: JSC's watchdog on the in-title WebProcess's main-thread VM, 10 s of CPU time per entry (`PS5WK_SCRIPT_LIMIT_S`, 0 off). The script ends with an uncatchable termination; the page goes on. |
| WebKit ending the in-title WebProcess (Restart page, Safer browsing) left it alive with its pages, scripts and workers. | Patch 0135: when the UI process closes its connection, the in-title WebProcess closes its pages. In the stage 3 run the tab was discarded first (ps5browser.c), which closed the page the normal way. |
| The in-title WebProcess ignored the page settings (`child.env`: JIT level, WebAssembly, painting threads...): only payloads read it. | `setUpInTitleWeb` reads `child.env` (then `inprocess-web`) and maps `PS5WK_JIT`/`PS5WK_WASM` as `ps5wk-child.cpp` configureJIT does, before JSC starts. |
| One in-title WebProcess per browser run, so "Restart page" left pages in a payload (25 instead of 48 frames/s) until the browser restarted. | In this mode the page settings are browser-restart settings: Settings shows Restart browser only, `/restart?scope=page` and the shield button restart the browser, which comes back to the page shown. |
| Restart browser needed the developer launcher (tools/launcher). | `apps/relaunch/ps5wk-relaunch.c`: a 100 KB payload the title sends to elfldr (`wpe_process_manager_ps5_run_task_elf`). It says "ready", the title winds down and creates a go file once main returns (`spike_title_exit_helper`, title_exit.c), and the helper SIGKILLs the title, waits until the system has no app in front, waits 1 s more and starts it again. Log: `/data/ps5webkit/relaunch.log`. |
| Main-loop stalls were not measured. | A 50 ms timer in the title: stalls over 250 ms logged at once, `[stall] 60 s: ...` every minute. |
| HLS video (`<video src=...m3u8>`, GStreamer's hlsdemux2, which downloads by itself in the WebProcess) took ~15 s per segment in the title: long loads, playback stopping every few seconds. In a title, inet sockets stay blocking: `fcntl(F_SETFL, O_NONBLOCK)` and `SOCK_NONBLOCK` are taken (F_GETFL shows the flag) but a read with no data waits, and `ioctl(FIONBIO)` is EACCES. GLib's reads then waited until the server closed the connection. | `shim/ps5shim.c` (title only): `fcntl(F_SETFL)` and `socket(SOCK_NONBLOCK)` on a socket also set Sony's `SO_NBIO` (0x1200), which does switch it; where that is refused, the existing `MSG_DONTWAIT` emulation. `package.sh` wraps `socket` for the title. Found with a start-up self-test in the title (wake-up latency of pipes, sockets and GLib contexts; libsoup requests; each way of asking for non-blocking), since removed. |
| The in-title WebProcess capped video at 1080p (`WEBKIT_GST_VIDEO_DECODING_LIMIT`); payloads ask the title's decoder and allow up to 8K. | `setUpInTitleWeb` sets the limit from the hardware decoder (`ps5wk_title_vdec_max_size`, titlevdec.c), as `ps5wk-child.cpp` does. |
| The browser froze for 15 s at a time (twice in a few hours of video pages): a page's WebGL sync call (`GetErrors`) waited for the WebGL thread until IPC's timeout. Stream sync calls wait on the stream's semaphore, where patch 0133's dispatch of the main run loop's UI and GPU side messages didn't reach. | Patch 0136: that wait goes in 2 ms slices on the title's main thread and dispatches those messages between them (logged: `a WebGL/WebGPU sync call waited for N message(s) of the title's main thread`). A thread logs `[stall] the main loop has not turned for N s` with the stream call the main thread is in and what each WebGL/WebGPU stream thread is doing (`ps5wk_stream_threads_report`). |
| A crash ended the title without a word. | `apps/title/titlecrash.c`: the payloads' crash guard in the title (without its pthread_create wrapper, titlemem.c has one); the report also goes to `/data/ps5webkit/crash-title.txt`, written from the handler. |
| libpas zero-fills and decommits by mapping fresh pages over its own (`mmap(MAP_FIXED)`). Ranges outside titlemem.c's table went to the real mmap, flexible memory: ~530 MB of such mappings in a few minutes of video pages, flexible memory down to 56 MB (15 MB after hours), video decoders refused for memory (slow software decoding), and the title ended by libpas's `PAS_ASSERT` in `pas_page_malloc_zero_fill` once none was left. | titlemem.c: anonymous read-write `MAP_FIXED` mappings outside the table, in 16 KiB pages up to 64 MB, get direct memory at the address (like WebAssembly's commits); a `MAP_FIXED` mapping of another kind over the table's ranges (a `PROT_NONE` guard) takes them out of the table first. The `[mem]` line counts both kinds. |
| Adaptive (HLS) streams start below 4K and switch up a few seconds in; each switch closed the decoder and asked for a bigger one, which the budget refused with pages in the title (the old decoder's memory came back only after the drain): playback stopped and started again. | PS5VideoDecoder.cpp: H.264 above 1080p gets a 3840x2160 decoder from the start when it fits (`h264headroom=0` in `vdec-budget`: its own size), so the switch keeps it; the H.264 estimate above 1080p comes from Spike 9's 4K figure. titlevdec.c: pipeline depth 2 above 1080p (it was above 4K). |
| Scrolling a feed that adds posts as it goes felt sluggish: the page's scripts, style and layout (0.3-1.2 s at a time, about 25 times a minute) run on the title's main thread, which also reads the pad, so the page stopped dead while they ran. | Stall samples: while the main loop has not turned for 120 ms, the stall thread samples the main thread's stack every 20 ms (the profiler's unwinder); stalls over 250 ms go to `/data/ps5webkit/prof/stalls-title-<pid>.txt` (`test/prof/stall-sym.py` sums them up by function; `/data/ps5webkit/no-stall-samples` turns it off). 43 of 44 were the page's own work (scripts building posts, style, layout). Patch 0137 + WPEDisplayPS5.cpp: while padTick has not run for 50 ms, a pad thread reads the right stick and queues its delta for the render thread's frame-locked scrolling (up to the painted edge, as a payload page does). Same 150 s scroll test: 100k px before, 112k px with it, 118k px with pages in a payload. The other stalls remain (menu, pointer, clicks wait). |

## Checks (owner, PPSA99207, no launcher payload running)

- **Runaway script** (`port/webkit/test/browser/script-loop.html`, a loop
  that never ends 10 s after load): the browser froze, then
  `a page script ran ... past its time limit: stopped it`, the page said
  "stopped the loop after 10.0 s" and its counter went on; the menu and
  controller worked. Stall log: 9972 ms.
- **Normal browsing**, about 15 minutes (the start page, a video site,
  an emulator page): per minute at most 19 stalls over 100 ms, the longest
  925 ms, during the busiest first minutes; later minutes none. Fights at
  a median of 48 frames/s.
- **Restart page** (the build before the restart change): the old page
  was closed; the game reloaded in a payload. Not wanted, hence the change
  above.
- **Restart browser through the helper**, four times (JIT off, JIT back
  on, Safer browsing on, off): each came back by itself, the helper's
  first launch attempt worked. The first helper started the title 200 ms
  after the kill, and the system showed "Cannot start the game or app"
  each time (the browser still started). Waiting until no app is in front
  plus 1 s (`/data/ps5webkit/relaunch-delay-ms`) removed it.

- **Video pages after the fixes above** (2026-11-16): four VR videos (HLS,
  2160x1080 then 3840x1920) and the start page in between, WebGPU on and
  off: no crash, every decoder in hardware (none refused), every
  `MAP_FIXED` outside the table in direct memory (none in flexible), the
  title's flexible memory at least 185 MB free (56 MB before), the longest
  stall 0.9 s. Direct memory in use went from about 1 GB to 1.9 GB:
  not watched over a long session yet.

## Left as it is

- A page that crashes the in-title WebProcess ends the browser (no
  "page crashed, reload" page): start it again from the home screen. Not
  tested yet.
- The page memory limit (`PS5WK_MEM_RESTART_MB`) only acts on payload
  WebProcesses; in the title, the title's own memory is the limit.
- Security: payload WebProcesses already run as root outside the jail with
  the jailbreak's kernel access (README, Security), so pages in the title
  reach no more than before.
