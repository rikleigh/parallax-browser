# Spike 14: the WebProcess inside the title (fights 1.7–1.9× faster in the title)

**Stage 0 (2026-11-15, [stage0.md](stage0.md)).**
- **Gain:** with the fight's shape, `wasm-big` runs at 5.19–5.25 Msteps/s in
  the title and 3.69–3.72 in the payload, **1.41×**. About 1.23× comes from
  the core itself and about 1.14× from not sharing it with an SMT sibling.
- **Route A is closed:** launching a second title replaces the first.
- **Next:** stage 1 (route B).

**Stage 2 (2026-11-15, [stage2.md](stage2.md)).** With the WebProcess in the
title, fights run at a median of 48 frames/s against 25 as a payload (mean
47.6 against 28.2). Same build and matchup, three runs each. Stage 1 (route
B) works behind `/data/ps5webkit/inprocess-web` and passes its checks
([stage1.md](stage1.md)): page loads, tabs, back/forward, 16 minutes of
fights without a crash, switch off unchanged.

**Stage 1, item 2 measured (2026-11-15, [stage1.md](stage1.md)).** In a
fight the whole title uses 0.20 of a core and its main thread 1.1%, so
sharing the main thread costs little CPU. Latency (long page tasks delaying
the UI) is what remains. Next: item 1, the launch.

**Question:** would the PS2 emulator's fights run faster if the WebProcess
ran in the browser title, and can WebKit be made to run it there?

The fight is limited by one thread, the emulator's `WebCore: Worker`, at
0.87–0.90 of a core (`HANDOVER-fps.md`). As an elfldr payload, the
WebProcess gets CPUs 9, 11, 13, 14 and 15 (mask `0xea00`). While a title
runs, 9 and 11 get no time and 13 about a third, so the whole WebProcess
lives on one physical core (14 and 15 are SMT siblings) plus scraps. The
title has CPUs 0–12. A payload cannot widen its mask (EPERM), and changing
the kernel's scheduling is out of scope (below). Running in the title is the
remaining way to get those CPUs, and it is what ProsperoEden does.

## What is already known

- **JSC with the JIT works in a title** (Spike 13, stage 1): 30/30
  correctness, stress checksums equal to LLInt in every tier, VM traps, and
  `wasm-big` with the right checksum. The 256 MB JIT pool is direct memory
  mapped RW and then `sceKernelMprotect` 0x07. No WebKit change was needed.
- **Single-thread speed barely changes.** `wasm-big` runs at 5.24 Msteps/s in
  the title and 4.95 in a payload (+6%) on an idle console. Beside 11–12 busy
  threads both drop to 4.31–4.35. JS (`bench.js`) is 15% faster in the title.
  These runs had the payload thread's SMT sibling **idle**. In a fight it is
  not: the page's main thread (19%), the second worker (14%), the audio thread
  and the wasm compile helpers (8%) share CPUs 14/15/13 with the emulator.
  smtprobe measured a busy sibling at 1.1× (latency-bound code) to 1.9× (wide
  integer code) slower. **What the title would buy is mainly a core the
  emulator does not share, and nothing has measured that yet.** Stage 0 does.
- **WebKit already runs one auxiliary process in the title.** Patch 0081
  (`PS5GPUProcessInTitle.cpp`, `AuxiliaryProcess::ps5InitializeInTitle`):
  - `GPUProcess::singleton()` is created in the title without its process
    start-up and runs on the title's main run loop.
  - WebProcesses reach it over ordinary IPC connections.
  - Its parent connection is an in-process pair whose far end drops messages.
  WebGPU (Dawn on RADV) also runs there, on RemoteGPU's work-queue thread.
  This is the pattern stage 1 extends.
- **How WebProcesses start now:** patch 0010 adds the WPE platform process
  manager. The title sends `ps5wk-child.elf` to elfldr. The child connects to
  a rendezvous socket, gets its IPC fd, sets up its environment
  (`setupEnvironment`, `configureJIT` in `apps/child/ps5wk-child.cpp`), and
  runs `WebProcessMain` on a pthread (`runWebKit`).
- **Title restrictions found so far:**
  - `exit()` raises SIGSYS.
  - `dup`/`dup2` are EPERM; `fcntl` FD flags are EINVAL; O_NONBLOCK on
    sockets is EACCES. The browser title already wraps all of these.
  - RWX at map time faults when executed.
  - Busy threads are not time-sliced against each other (Spike 13: 13 busy
    threads starved main and the log thread for hours).
- **Title memory:** about 440 MB of flexible memory. `titlemem.c` puts heap,
  anonymous RW mappings and stacks in direct memory. PROT_NONE reservations go
  to the real `mmap`, so pages committed into them later would be flexible
  memory.
- **Fork from a title is untested.** The title-context run of Spike 1 did
  only threads, shm, memory and JIT (`evidence/2026-10-27/titles-02-03b.log`).

## Out of scope

- **Kernel or scheduler changes** (widening the payload mask, re-pinning
  processes): not part of this route and not to be attempted.
- More than one WebProcess in the title (`WebProcess::singleton()` is one per
  process).
- The NetworkProcess, which stays a payload.
- Shipping. The in-title WebProcess stays behind a switch. Losing crash and
  security isolation needs its own decision in `RELEASE-REVIEW.md`.

## Stage 0: is it worth it? (about 1 day, no WebKit change)

Uses Spike 13's title, `jscenv.h`, `payloadenv.c`, `wasm-big.js` and
`run.sh`.

**0a. The fight's shape in a payload.**
- Add to `payloadenv.c`:
  - pin the shell thread from `/data/jsc/cpu.txt`, set on the main thread
    before PS5Main starts it, so the shell inherits it;
  - busy threads from `/data/jsc/payload-load.txt` (`<cpu>:<duty %>` per
    thread: a loop that spins for that share of each 10 ms and sleeps the
    rest).
- With a busy title running, as in a fight (Spike 13's load title, 12
  threads), run `wasm-big` in the payload on CPU 14 with:
  - nothing else running;
  - CPU 15 at 50% and at 100%;
  - CPU 15 at 50% and CPU 13 at 50% (close to the profile's other threads).

**0b. The title equivalent.** Run `wasm-big` in the title with the same
number of side threads at the same duty, unpinned, and pinned so that the
jsc thread's sibling is idle. The CPU pairs come from smtprobe run in the
title (APIC IDs ÷ 2).

**0c. Two titles at once** (a probe for route A, below; about 2 hours).
With the browser title or a spike title running, start a second spike title
without stopping the first. Record:
- whether both stay running;
- each one's CPU mask;
- whether the one not in the foreground still gets CPU time (a busy loop's
  rate);
- whether the two can share an `AF_UNIX` socket under `/system_tmp/ps5wk`
  with `SCM_RIGHTS`, and direct memory.

**Gate:**
- gain = title rate (0b) ÷ worst fight-like payload rate (0a);
- fights need about 1.7–2× to reach 60 frames/s;
- **gain below 1.15×: stop.** The route cannot lift the fights much. The
  remaining lever is the emulator's own hot functions (the other open item in
  `HANDOVER-fps.md`);
- **gain 1.15× or more:**
  - route A if 0c passes: the WebProcess as a second title, keeping WebKit's
    process model and isolation; the change is the process manager launching a
    title instead of a payload;
  - otherwise route B, stage 1 below.

## Stage 1 (route B): one WebProcess in the browser title (about 2–4 weeks, uncertain)

Behind a switch: `/data/ps5webkit/inprocess-web`, or a key in `child.env`.

1. **Launch.** Add `PS5WebProcessInTitle` (`UIProcess/wpe/ps5/`), modelled on
   0081:
   - when the process manager would launch a WebProcess, make an IPC
     connection pair;
   - finish the launch on the UI side with the title's pid and the server end;
   - start `WebProcess::singleton()` with the client end.
   Give it `AuxiliaryProcess::initialize`'s per-process parts but none of its
   process-wide set-up: WTF, JSC, GLib and logging are already started by the
   UIProcess. Do not call `RunLoop::run()`, because the title's main loop is
   already running.
2. **The main thread.** WebKit has one main thread per process
   (`isMainThread()`, `RunLoop::main()`). So the WebProcess's main thread has
   to be the title's: page layout, JS timers and events (19% of a core in
   fights) share a run loop with the UI and compositor. This is the largest
   design risk. Measure what the title's main thread does during a fight
   before starting (the profiler, or the run loop's own timing).
   **Measured: 1.1% of a core in a fight (`stage1.md`).**
3. **One WebProcess.** Use `usesSingleWebProcess`. Also turn off process
   swaps on navigation, the prewarmed process and site isolation, or send only
   the first WebProcess to the title and keep later ones as payloads.
4. **Process-wide state the two sides would share.** Each item needs a
   decision:
   - JSC options: patch 0030 turns the JIT off in titles; take
     `configureJIT`'s values instead;
   - the JIT pool: move Spike 13's `__wrap_mmap` RW→RWX path into
     `titlemem.c`;
   - signal handlers: JSC's VM traps and wasm faults vs the title's
     `crashguard.c`, and their chaining order;
   - `exit()`/`terminate()` paths (SIGSYS);
   - the environment from `setupEnvironment` (GStreamer, XDG, ICU);
   - `ps5wk-child.cpp`'s mmap tracking and loggers;
   - memory-pressure handling, which in a title acts on the whole browser;
   - `Process::identifier()`, which both sides share.
5. **WebAssembly memories.** The emulator reserves about 2 GiB plus guards
   (patches 0129/0130). In the title, pages committed into a PROT_NONE
   reservation would be flexible memory (about 400 MB). They need direct
   memory instead: `sceKernelReserveVirtualRange` and
   `sceKernelMapDirectMemory` as the memory grows, clear of RADV's fixed
   ranges (`[0x2_0000_0000, 0x3_0000_0000)`, `[0x40_0000_0000, 0x80_0000_0000)`).
   Fast memory's signal path in a title is untested.
6. **Threads.** Unsliced busy threads in a title (Spike 13) mean a page's
   workers could starve the UI, audio or GPU threads. Give WebProcess threads
   a lower priority than those, or keep them off fixed CPUs, through
   `titlemem.c`'s `pthread_create` wrapper. ProsperoEden keeps its emulator
   threads off the GPU threads' cores and their SMT siblings (+17%).
7. **Media.** Audio and video are in the WebProcess today. In the title, the
   hardware decoder becomes directly usable (Spike 9). Keep the current sinks
   first.

**Pass:**
- a static page and the emulator page load and run in the in-title
  WebProcess;
- closing the tab and navigating back and forward work;
- no crash in 10 minutes of fights;
- the switch off gives today's build unchanged.

## Stage 2: the fight (about 2–3 days)

- `tools/fps-bench.py` on the same matchup, payload WebProcess vs in-title,
  same build, at least 3 runs each.
- A CPU-weighted profile of the in-title fight: the worker's share of a core,
  and the main thread's queueing.
- Thread placement: the emulator's worker on a core whose sibling is idle,
  and off Dawn's and the compositor's cores.

**Pass:** fights at a clearly higher frame rate (≥ 1.15× the payload's median)
with clean sound, and the UI stays responsive (no main-loop stalls over
100 ms in the log).

## Risks

- **The shared main thread** (stage 1.2) could cost more than the CPUs gain,
  and it puts UI jank in the page's hands.
- **Crash and security isolation are gone.** A page crash, or a WebProcess
  memory kill, ends the browser.
- **Hidden process-wide assumptions** in WebKit beyond the list in 1.4, which
  typically show up as RELEASE_ASSERTs at run time. 0081 met some of them
  for the GPU process, a much smaller surface.
- **Memory:** the WebProcess's heap and the emulator's memory in the title's
  direct-memory budget, beside the GPU side, the decoder and Dawn.
- **Upstream drift:** a PS5-only launch path in `WebProcessProxy`/
  `ProcessLauncher` and in `AuxiliaryProcess`.
- **The gain may be modest** even with a free core: the single-thread
  difference is 6%, and the rest depends on the SMT numbers stage 0 measures.

## Files

| file | role |
|---|---|
| `stage0.md` | the 0a/0b/0c numbers and the gate decision (done) |
| `stage1.md` | stage 1: the title's threads in a fight (item 2), the in-title launch and every fix (item 1) |
| `stage2.md` | stage 2: fight frame rates, in the title vs payload |
| `spikes/13-title-jit/payloadenv.c`, `jscside.h`, `run.sh` (steps `s0a` `s0b` `s0c`) | stage 0: shell-thread pinning, duty-cycled side threads, the title's APIC map (done) |
| `tools/launcher` `launch-also`/`apps`/`killtitle` | 0c: a second title (PPSA99018) beside a running one (done) |
| `port/webkit/patches/01xx-ps5-webprocess-in-title.patch` | stage 1, only after the gate |
