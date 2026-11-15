# Spike 14 stage 1 (route B): notes and measurements

## 1.2 The title's main thread in a fight (2026-11-15)

**Result: the title is almost idle during a fight. Its main thread uses 1.1%
of a core, so sharing it with the WebProcess's main thread (19%) costs little
CPU.**

Run:
- PPSA99208, build `20261114-230100-91f31930c3`, with the title profiler in
  CPU mode (`/data/ps5webkit/title-profile` = `1`; removed after the run).
- `tools/fps-bench.py --label title-main-prof`: 150 fight seconds, mean
  33.9 frames/s, median 30, p10 28, p90 48. That is the same as the earlier
  runs (median 30–31), so the title profiler did not visibly slow the fight.
- Profile: `logs/prof/title-350.prof`, fight window 995–1145 s.
  - Summary: `logs/prof/title-350-fight.txt`, from
    `prof.py --elf toolchain/wpe-webkit/titles/debug/20261114-230100-91f31930c3/ps5browser.elf --from 995000 --to 1145000`.
  - Evidence: `evidence/2026-11-15/14-webprocess-in-title-stage1-main.log`.

| Title thread | Share of a core | What it does |
| --- | --- | --- |
| whole process (`getrusage`, every thread) | **0.20** | |
| `PS5Renderer::threadMain` (compositor) | 0.093 | 45% waiting for the present (`ps5_agc_gate2_wait_present`), 27% `SceneHost::paint`, 16% the WebGL layer latch (texture upload) |
| `WebGPU_work_queue` (Dawn, in-title GPU process) | 0.071 | stream IPC dispatch, `Queue::Submit`, the canvas present and readback |
| main (`eboot.bin`: UI, glib main loop) | **0.011** | the display connection's callback, the UI menu redraw (`bui_draw_menu`), gamepad snapshots |
| IPC `ReceiveQueue` ×3 | 0.004 each | |

- Between 900 and 960 s, before the page was opened, the whole title used
  0.01 of a core.
- The process total (0.20) is `getrusage(RUSAGE_SELF)`, so it includes
  threads the profiler does not sample (system libraries' own threads).
- Thread placement: about 0.2 of a core's work on 13 CPUs, so the title has
  plenty of free cores. Stage 2's placement only needs to keep the emulator
  worker off the cores (and SMT siblings) of the renderer and the WebGPU
  queue.
- Not measured: how the run loop is timed (queueing, stalls). With 1.1% busy,
  a wall-mode run would mostly show idle time. It becomes worth running once
  the page's work is on this thread (stage 2's "no main-loop stalls over
  100 ms").

### What this means for item 2 (one main thread)

- **CPU:** the page's main-thread work (19% of a core in the payload) would
  join 1.1%. It is not the large risk the README feared. The risk that
  remains is latency: a long page task (layout, a JS timer) delays UI input
  and the menu. The compositor (`PS5Renderer`) and Dawn have their own
  threads, so frames do not wait for the page.
- **Sync IPC between the two sides on one thread.** From reading
  `Source/WebKit/Platform/IPC/Connection.cpp`:
  - `SyncMessageState` is one per dispatcher (the main run loop), shared by
    every connection on it.
  - While a thread waits for a sync reply, incoming sync messages for any
    connection on that dispatcher are dispatched in the wait loop
    (`SyncMessageState::processIncomingMessage`; "If we don't we'll end up
    with a deadlock where both sync message senders are stuck waiting for a
    reply").
  - So a sync send from the in-title WebProcess to the UIProcess should be
    handled nested on the same thread rather than deadlock. Not yet
    confirmed at run time. Messages sent without
    `DispatchMessageEvenWhenWaitingForSyncReply`, and handlers that wait for
    the main run loop to turn, are the cases to watch in item 1.

## 1.1 Launch (2026-11-15, in progress)

Switch: `/data/ps5webkit/inprocess-web` (a file). Without it the build
behaves as before.

**How it starts.** WebKit's UI side is unchanged.
- `ProcessLauncher` makes the socket pair and passes the client end to the
  PS5 process manager.
- With the switch on, the manager's `launch()` (`wpe-ps5/WPEProcessManagerPS5.cpp`)
  takes the first WebProcess itself. On the main loop, after `launch()` has
  returned, it calls `ps5wk_web_process_in_title_start(fd, id)` instead of
  sending `ps5wk-child.elf` to elfldr. Later WebProcesses are payloads.
- `PS5WebProcessInTitle.cpp` (new, WebKit) does what `WebProcessMainWPE`
  does, minus everything already started (WTF, JSC, GLib, the run loop).
  It then calls `AuxiliaryProcess::ps5InitializeWebProcessInTitle`.
- The PS5 build already runs one WebProcess with no process swaps, no
  process cache and no prewarming (`WebKitWebContext.cpp`), so every page
  goes to it (item 3 needs nothing).

**Process-wide state, as decided:**

| What | In the title |
| --- | --- |
| Process type (`isInWebProcess()`) | set to WebContent for the whole title at start. UI-side uses outside asserts are Apple-only. GStreamer's `ensureGStreamerInitialized` needs it. |
| `Process::identifier()` | set to the WebProcess's. The UI side uses it only for `PS5WCHost`'s own layer ids. Object identifiers share one counter, so no clash. |
| `SessionID` / `WebPageProxyIdentifier` generation protection | not enabled: the UI makes them for every new page. With it, the next new tab would hit a RELEASE_ASSERT. |
| Exit on connection close (`ps5ExitProcessWhenClosed`, `crashAfter10Seconds`) | off. `didClose`, `stopRunLoop`, `WebProcess::stopRunLoop` only log. `stopRunLoop` would stop the title's loop and destroy the GPU side's display. The WebProcess stays stopped; it is one per title. |
| JSC options | `ps5wk-child.cpp configureJIT`'s values, set by the title before WebKit starts JSC: JIT, WebAssembly, one compiler thread per tier, 256 MB pool. Safer browsing turns them off. They then hold for the title's own JSC too (the ad-block converter). |
| JIT pool | `titlemem.c`: anonymous `PROT_EXEC` maps go to direct memory read-write, then `sceKernelMprotect` (Spike 13's path). Only with the switch. |
| Environment | audio and video socket paths (`audio-<pid>.sock`, `video-<pid>.sock`), HLS, 1080p60 decode limit, media cache. Not `child.env`, and not the child's loggers. |
| Script dialogs | the title has no handler, so WPE's default closes them at once: no wait on the shared thread. |

**IPC on one main thread.** The WebProcess's sync messages to the UI arrive
on the same thread that waits for their replies. WebKit already dispatches
incoming sync messages inside the wait. That isn't enough when the UI
handler finishes with another process's reply.
`GetNetworkProcessConnection` is the first such message: the UI asks the
NetworkProcess, and its async reply would wait for the main loop, which is
blocked in the wait. So, with an in-title WebProcess, every other
connection on the main run loop has all its messages dispatched inside such
waits, in order (`Connection.cpp`, `SyncMessageState::processIncomingMessage`).
Any UI handler that waits for a timer or a main-loop turn before replying
would still block. The wait logs
`[web-in-title] the title's main thread still waits for the reply to <message>`
every 2 s, so such cases show up by name.

UI-side `waitForAndDispatchImmediately` on the in-title connection can't be
answered while the UI waits. On WPE: `DidCreateSubframe` (0 s) and
`ReceivedMainResourceResponseWithCertificateInfo` (0 s). The coordinated
graphics drawing area (500 ms) is not used on PS5.

### First runs (2026-11-15, PPSA99209 "Parallax web-in-title test")

A separate test title, so the installed WebGPU test (PPSA99208) is
untouched. Same WebGPU build, plus the changes above.

| Run | What happened | Fix |
| --- | --- | --- |
| 1 | The WebProcess started in the title (0.6 ms) and the JIT pool mapped (256 MB, `sceKernelMprotect` 0x7). Then it hung in `GetNetworkProcessConnection`: the NetworkProcess payload never got its IPC fd, because the process manager's rendezvous (listener, hello) ran on the main loop, which was the thread waiting. | With a WebProcess in the title, the rendezvous runs on its own thread with its own GLib context; the launches table is locked (`WPEProcessManagerPS5.cpp`). |
| 2 | The restored emulator tab loaded in 2.3 s, then nothing more happened (black screen). | Run 6. |
| 3 | `example.com` loaded (1.5 s) and the title stayed up, but the page painted without text. "Fontconfig error: Cannot load default config file". | `access()` says no for existing files in a title; `titlesys.c` checks again with `open()` (`--wrap=access`, only with the switch). |
| 4 | Still no text, no error. Fontconfig had the config and the folder but **0 fonts**: `opendir("/data/ps5webkit/fonts")` is EPERM in a title (stat, open and FreeType work). | `PS5WebProcessInTitle.cpp` adds each `fonts/` file from `/app0/runtime/runtime-files.txt` with `FcConfigAppFontAddFile`: 29 files, 47 faces, **1.3 s** on the main thread at start (2 entries failed). |
| 5 | `example.com` in the title is identical to the payload's (Latin, Arabic, CJK, Cyrillic): `port/webkit/test/title-inweb-fonts-02-example.com-10s.png` vs `title-payload-02-example.com-10s.png`. | |

Also for debugging:
- stdout goes to the log in in-title mode (a FILE on fd 2; `dup2` onto fd 1
  is EBUSY in the title), so page console messages are logged as for
  payloads.
- `KEY=VALUE` lines in `/data/ps5webkit/inprocess-web` are set in the
  title's environment, as `child.env` is for payloads.

| 6 | The emulator page (wall profiler on): black screen, but the main thread was idle (1% of a core), not stuck. A JS probe (`test-onload.js`) found WebGPU's adapter, IndexedDB, CacheStorage and fetch all working, and `crossOriginIsolated` true. But the document held 3 of the page's 4 inline scripts and no the emulator's main script: **25,192 of 46,436 characters**, cut in the middle of the third script, while a `fetch` of the same URL got all of it. | See below. |

**Run 6's cause, from the code (not yet confirmed by a run).** The first IPC
fix dispatched every message of every main-loop connection except the
in-title one inside sync waits. That includes the WebProcess side's own
`NetworkProcessConnection`. An inline script's sync call to the
NetworkProcess (cookies, storage) then had the main resource's
`DidReceiveData`/`DidFinishResourceLoad` dispatched inside it. The
document finished early, cut where the parser was. WebKit never dispatches
those inside a sync wait. Narrowed: only the UI side's connections to other
processes (`AuxiliaryProcessProxy`, `Connection::ps5MarkUIProcessSide`) get
dispatch-all, which is what `GetNetworkProcessConnection` needed.

| 7 | With the narrowed rule: the whole document (46,466 characters, 5 scripts); the emulator's main script ran, then `RangeError: Out of memory` creating the emulator's 256 MB memory. JSC reserves it with `sceKernelReserveVirtualRange` (fine in a title) and commits with `mmap(MAP_FIXED)`, which in a title is flexible memory (about 211 MB free). | Item 5: JSC calls `ps5wk_title_commit_direct`/`ps5wk_title_release_direct` (weak, defined only by the title's `titlemem.c`): direct memory mapped at the address with `sceKernelMapDirectMemory(MAP_FIXED)`, zeroed, and in titlemem's table. `[mem] wasm commit 1: 256.0 MB of direct memory at 8000000000`, then `[wasm] fast memory of 256 MB at 8000000000 (shared)`. |
| 8 | The runtime never initialized (`runDependencies` stuck at 1, 2 pthread workers unused). A one-line test worker failed with "Maximum call stack size exceeded"; in a payload it works. The stack bounds JSC reads are right (40 threads checked: `pthread_attr_get_np` matches the stack given). But WebKit asks no stack size for JavaScript threads on PS5, so they get libc's 64 KiB, less than JSC's reserved zone. In payloads the crash guard raises every such thread to 2 MiB. | `titlemem.c`: threads without their own stack get at least 2 MiB with the switch on, as the crash guard does. The test worker ran; `onRuntimeInitialized` and `beginGame` fired. |
| 9 | WebGPU: `requestAdapter()` resolves to null in the title (an adapter and a device in a payload). `RemoteGPUProxy::create` blocks the main thread in `waitUntilInitialized()` for the GPU side's `WasCreated`, which the GPU side sends from its `CreateGPU` handler on the same main thread: the wait times out, the proxy is lost, its stream closes ("end of stream on socket 114"). | The in-title GPU side's connection is marked like the UI's (`GPUConnectionToWebProcess`), and in the title a main-thread `waitForMessage` of a WebProcess-side connection waits in 2 ms slices, dispatching the main run loop's queued UI and GPU side messages between them (`Connection.cpp`). |
| 10 | **The emulator runs in the title on WebGPU**: Dawn started, the 1920x1080 canvas presents, the probe gets an adapter and a device, `onRuntimeInitialized` and `beginGame`. Menus at 60 canvas frames/s. The title's main thread is 12-13% busy outside poll (UI plus the page), the title 1.0 core in all. | |

Open:
- fight frame rates (stage 2);
- the 1.3 s font add at start (a fontconfig cache written by a payload would
  avoid it);
- a leftover NetworkProcess payload from run 1 (pid 357, 371 MB) is still
  alive on the console.

## Pass checks (2026-11-15, after a console reboot)

PPSA99209 build `20261115-113730-91f31930c3`, switch on. The owner drove
the controller. Log from line 1464187 of that day's title log.

| Check | Result |
| --- | --- |
| A static page and the emulator page load and run in the in-title WebProcess | Pass: `example.com`, three.js WebGPU retargeting, two emulator pages, all in WebProcess 4 (the title). |
| Closing the tab, back and forward | Pass. Back and forward between the emulator page and other pages reloaded the game. A new tab, then closing it, then closing every tab and opening a new one (B3). At B3, WebKit launched a payload WebProcess (5) for the new tab's start page, closed it 49 s later, and kept using the in-title WebProcess 4. That process was never terminated. |
| No crash in 10 minutes of fights | Pass: 973 fight seconds (16 min), several scenes and characters. Mean 42.2 frames/s, median 45, p10 30, p90 52 (stage 2's fixed matchup: median 48). No crash, no sync-wait line, no watchdog report. |
| The switch off gives today's build unchanged | Pass. Review of patch 0133 and the title changes: every change is gated on the in-title connection, `s_ps5WebProcessInTitle`, the process manager's hook, `ps5wk_title_mem_exec` or `ps5wk_title_access_fix`. The always-on parts have no effect: the connection marker flag; `--wrap=access` returns the real call's result unless the fix flag is set. Stage 2's three switch-off runs played normally at the payload's rates. |

**Stage 1 (route B) passes.**

Seen on the way (2026-11-15, a switch-off run, so not this change): the
console stopped giving the controller to anything. The title logged
`pad: ... intercepted by the system` once and got no input after it; the PS
button did nothing either. The title kept running normally for 8 more
minutes (main thread 0–1%, memory flat). A reboot cleared it, and the
leftover NetworkProcess payload from run 1 with it.
In the pass-check session the owner left the emulator paused for a while and
resumed a fight (when the freeze had shown before): no freeze. The pad went
to the system and back as normal (`intercepted by the system`, then `not
intercepted`).
