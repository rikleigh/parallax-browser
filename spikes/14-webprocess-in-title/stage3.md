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

## Left as it is

- A page that crashes the in-title WebProcess ends the browser (no
  "page crashed, reload" page): start it again from the home screen. Not
  tested yet.
- The page memory limit (`PS5WK_MEM_RESTART_MB`) only acts on payload
  WebProcesses; in the title, the title's own memory is the limit.
- Security: payload WebProcesses already run as root outside the jail with
  the jailbreak's kernel access (README, Security), so pages in the title
  reach no more than before.
