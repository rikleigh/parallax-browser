# Spike 13: JavaScriptCore's JIT inside a title (stages 0 and 1 done)

## Stage 0 result (2026-11-15, title PPSA99017, 13.60)

**A title can run JIT code three ways. RWX works after all: what fails is
asking for execute permission at map time, not RWX itself.**

- **RWX via `mprotect` works.** Map RW, then `mprotect` (anonymous memory) or
  `sceKernelMprotect` 0x07 (direct memory) to RWX. The code runs, it can be
  rewritten in place and it runs again (43, then 101). Spike 1's title
  failure was the map-time case only: `mmap(…PROT_EXEC…)` returns success
  but the page faults when run, again here. Patch 0030's comment ("a title
  can map it but faults executing it") is right only for map time.
- **Two views of one direct-memory allocation work (W^X).** The same
  `phys` is mapped twice with `sceKernelMapDirectMemory`: RW, and RW then
  `sceKernelMprotect` RX (RX at map time: 0x80020016). The RX view isn't
  writable and the RW view doesn't run code. While one thread ran the code
  4.8 million times, another rewrote its immediate 10,000 times through the
  RW view: all 10,000 were seen (mean 1.8 µs, max 21 µs), no fault.
- **Shared memory (`shm_open`, anonymous or named) does not work.** The RX
  view maps without error, at map time, and faults when run, as RWX at map
  time does. A 256 MB shm RX view failed with ENOMEM. `memfd_create` is not
  in the SDK.
- **Flipping RX↔RW costs 57 µs per round trip** on one 16 KiB page (66 µs
  with 4 threads running code on other pages). A thread running code on
  the page while it flips faults (1992 faults in 2000 flips), and every
  fault was retried from the signal handler without a wrong result. It
  works, but it is slow and isn't needed now.
- **A 256 MB dual-mapped direct-memory pool costs no flexible memory**: 442
  MB before, with both views mapped, with 64 MB written, and after release.
- **CPUs:** the title's mask is 0x1fff (CPUs 0–12); 13–15 refuse with
  EPERM. The 8-chain loop takes 26 ms on CPUs 0–11 and 30 ms on 12 (best of
  3). `test/sched/smtprobe` measured 28–31 ms per payload CPU with no title
  running (median of 5, so not quite the same statistic). Per core, a title
  CPU is about as fast as a payload one. What a title gains is 12 CPUs
  that nothing else takes, against a payload's 5 of which 2–3 are lost
  whenever a title runs.

Log: `evidence/2026-11-15/13-title-jit-stage0.log`.

**Gate decision.** Stage 1 can start with the smallest change: patch 0030's
RWX pool, mapped RW and then `mprotect`ed to RWX, in direct memory. JSC
then runs exactly as it does in the WebProcess. The W^X alternative is
`SEPARATED_WX_HEAP` on a dual direct-memory mapping, which passed every
check. Keep it for a later hardening step, not for the speed measurement.

**Question:** can JSC run with its JIT (Baseline, DFG, FTL, and WebAssembly
BBQ/OMG) inside a registered title process? And how fast is a
WebAssembly-heavy thread there, compared with a payload?

**Why it matters:** the PS2 emulator page (HANDOVER-fps.md) is limited by
one WebAssembly worker thread. Today that thread runs in the WebProcess, a
payload. Payloads only get the system CPUs (mask 0xea00), and while a title
runs, three of them are no use: CPUs 9 and 11 get no time and 13 about a
third. A title gets the game CPUs (0x1fff). ProsperoEden gets its speed by
running its JIT inside its title. If this spike passes, the next question is
whether the WebProcess can live in the title. If it fails, that route is
closed, and the reason gets written down.

**Scope:** JSC alone, in a test title. No WebKit process-model change, no
browser change, and no change to the system or the kernel. Only the APIs a
title already has are used.

## Stage 1 result (2026-11-15, title PPSA99017, 13.60)

**The WPE build's `jsc` runs as a title with the JIT on and gives the same
results as the interpreter. Its WebAssembly runs 6% faster than in a payload
on an idle console, and at the same rate when 11–12 CPUs are busy.** No
WebKit change was needed. Results are in `results/20261115-003132` (stress,
traps), `results/20261115-052940` (pinned) and `results/20261115-053534`
(bench, wasm, wasm-load). Summary: `evidence/2026-11-15/13-title-jit-stage1.log`.

| test | title | payload | |
|---|---|---|---|
| `correctness.js`, JIT / `--useJIT=0` | 30/30, 30/30 | (Spike 4) | |
| `jit-stress.js` per tier: JIT, DFG only, LLInt, eager | 16/16 each; 10 checksums per JIT tier equal to LLInt | | |
| `jit-watchdog.js` (VM traps in DFG/FTL code) | loop stopped at 3.2 s (`--watchdog=3000`), exit 3 | | |
| `bench.js` total | 348–352 ms | 412–417 ms | title 15% faster |
| `wasm-big.js`, nothing else running | 5.24 Msteps/s | 4.95 | title +6% |
| `wasm-big.js`, title jsc pinned to CPU 3 | 5.20 | | as unpinned |
| `wasm-big.js` beside a busy title (payload: 12 load threads on CPUs 1–12; title: 11 on CPUs 1–11 in the same title) | 4.31 | 4.35 | the same |

The rates are means of the last 10 one-second samples; the per-second lines
are in the logs, and each run's best and all-run mean are within 0.2 of these.
Every `wasm-big` run printed the check `fa681e86-f0492106`, as macOS does in
every tier. 142,635 bytes of function; the tier was not recorded (on macOS it
stays in BBQ, see stage 0 notes).

How it is built (no `patches/`):
- `build-jsc-title.sh` links ninja's `bin/jsc` objects minus `PS5Main.cpp.o`
  with `jsctitle.c` as main, `titlemem.c`/`dheap.c` (direct memory) and
  `libc-title.a`.
- The JIT pool: `jsctitle.c`'s `__wrap_mmap` maps any anonymous executable
  request RW through `titlemem` and then `sceKernelMprotect` 0x07. Only one
  happens: the 256 MB pool. Flexible memory stays at 403–407 MB (343 MB while
  `wasm-big`'s 32 MB memory and code are live).
- JSC options are the WebProcess's (`jscenv.h`) as `JSC_*` environment in
  both processes. The payload `jsc` is the same objects relinked with
  `payloadenv.c` wrapping main. Command-line `--useWasm=1` does not work on
  PS5: wasm is off by default (Options.cpp), the dependent options are
  switched off with it, and the command line comes too late to bring them back
  ("INCOHERENT OPTIONS").

Title-specific fixes found on the way (all in `jsctitle.c`):
- `exit()` raises SIGSYS in a title: `--wrap=exit` ends the shell thread
  instead, and main returns.
- `dup2` onto fds 1/2 is EPERM and libc's stdout is fd 11: `stdout`/`stderr`
  are reassigned to `fdopen`ed pipes read by a log thread.
- The crash handler reads rip/rsp/rbp at the kernel's ucontext offsets
  (+0xe0/+0xf8/+0x88, `crashguard.c`); the ELF loads 0x400000 up.
- **Busy title threads are not time-sliced against each other (as observed).**
  13 unpinned busy threads on the title's 13 CPUs stopped main and the log
  thread for 4.5 hours: the title never logged again and had to be killed.
  The load threads are now pinned to CPUs 1–12. A game that keeps all its
  CPUs busy would starve its own other threads this way. That fits the
  Prospero scheduler giving fixed-priority threads no quantum, which is not
  proven here.

Against the pass line:
- correctness, stress and traps: **pass**.
- No fault in 10 minutes: no fault in about 5 minutes of `wasm-big` and the
  stress runs in the title; a single 10-minute soak was not run.
- `wasm-big` in the title ≥ the payload's best case: **pass** (+6%; the
  payload was not pinned to CPU 14).
- "Clearly beats the payload while a title is busy": **not met** as tested.
  Under 11–12 busy threads both lose 12–18%, by the same amount, although the
  payload's thread runs outside the loaded CPUs. That points at something
  console-wide (clock or power under an all-core load, or the shared caches)
  rather than at which CPUs the payload gets. Not proven.

What this means for the in-title WebProcess route:
- The emulator's wasm thread itself would gain about 6% (JS 15%) from being
  in a title: the same code runs faster, cause not measured (direct memory,
  CPU placement or clocks are the candidates).
- It would not escape a console-wide slowdown from a busy title.
- The single-thread test does not measure what the route is really for: the
  WebProcess's other threads (main thread, GC, compilers, audio) getting 13
  CPUs instead of the payload's CPUs 13–15. That needs the WebProcess in the
  title (next spike), or a payload test with those threads busy.

## What is already known

| fact | source |
|---|---|
| Title: mapping RWX anonymous memory "works", but executing it faults (SIGSEGV) | Spike 1, title context (`evidence/2026-10-27/titles-02-03b.log:116`) |
| Title: RW, then `mprotect` to RX, executes | Spike 1 (anonymous memory); Spike 10 (direct memory, `sceKernelMprotect` 0x05) |
| Title: `sceKernelJitCreateSharedMemory` fails, 0x80020001 | Spike 1 |
| Title: `mprotect` to RWX returns 0; whether it then executes was not tested | Spike 10 |
| Title: mapping direct memory with execute permission is refused (0x80020016); `mprotect` grants it | Spike 10 |
| Title: its text is execute-only (reading it gives `SYSTEM_XO_VIOLATION`) | Spike 10 |
| Title: ~440 MB flexible memory, GBs of direct memory; anonymous mmaps, malloc and stacks already go to direct memory | Spike 10, `port/webkit/apps/title/titlemem.c` |
| Title: `shm_open` works (named shm PASS) | Spike 1 |
| Payload: RWX, RW→RX and `sceKernelJit*` all work; the JIT is on through patch 0030 (RWX pool, 256 MB) | Spike 1, patch 0030 |
| The title already links JSC (the UIProcess), with `useJIT=false` | patch 0030 `Options.cpp` |
| ProsperoEden JITs in its title with two views of one memory object, RW and RX, never RWX | HANDOVER-fps.md addendum |

JSC has two W^X write paths that this spike can start from. Both are off for
this port:

- **`ENABLE_SEPARATED_WX_HEAP`**. Every JIT write goes through
  `g_jscConfig.jitWriteSeparateHeaps(offset, src, n)` into a second,
  writable mapping of the pool. Only Darwin creates that mapping
  (`initializeSeparatedWXHeaps`, which uses `mach_vm_remap`). The C writer,
  `genericWriteToJITRegion`, is portable. On x86_64 the port needs a
  non-Darwin `initializeSeparatedWXHeaps` built on whatever dual mapping
  stage 0 finds. This is the preferred scheme.
- **`ENABLE_MPROTECT_RX_TO_RWX`**. Each write does `startWriting`/`finishWriting`,
  which flip the touched pages to RWX and back, with per-page writer counts.
  In a title RWX probably doesn't execute (Spike 1), so this would become
  RX→RW→RX. Then another thread running code on a page being patched would
  fault. This is the fallback, and stage 0 measures its cost.

## Stage 0: memory probes (a small C title, about 1 day)

A new test title, **PPSA99017** (unused), packaged the way
`spikes/10-title-dmem/build-title.sh` does it. It logs `RESULT 13-title-jit
<probe> PASS|FAIL …` lines to the log server. Each probe catches
SIGSEGV/SIGBUS, as Spike 1's do.

| probe | what it checks |
|---|---|
| `rwx-exec` | `mprotect` to RWX, on anonymous and on direct memory: does the code execute? (Spike 10 left this open) |
| `dual-shm` | `shm_open(SHM_ANON)` + `ftruncate`, mapped twice: view A RW, view B RW and then `mprotect` RX (or mapped RX directly, if allowed). Write through A, call through B, patch through A, call again |
| `dual-memfd` | the same with `memfd_create`, if the libc has it (ProsperoEden's route) |
| `dual-dmem` | one direct-memory allocation mapped at two addresses with `sceKernelMapDirectMemory`, RW and RW→RX |
| `cross-modify` | for whichever dual mapping passes: thread 1 runs a loop in view B while thread 2 rewrites a jump target through view A 10,000 times; no fault, and the new code is seen |
| `flip-cost` | RX→RW→RX `mprotect` round trips on a 16 KiB page: µs each, single-threaded and with 4 threads executing on other pages of the pool |
| `pool-size` | reserve and dual-map a 256 MB pool (patch 0030's size); flexible memory before and after |
| `cpu` | the `smtprobe` WIDE loop on each title CPU, for the CPU-speed baseline of stage 1 |

**Gate:** if a dual mapping passes `cross-modify`, stage 1 uses
`SEPARATED_WX_HEAP`. If only RW↔RX flipping works, stage 1 uses the
`MPROTECT` path plus a fault handler that waits for the writer and retries.
Treat that path as a second-best answer: note the flip cost and expect
races. If neither works, the spike ends here, and the in-title route is
closed for JIT code.

## Stage 1: the `jsc` shell as a title (about 3–6 days)

1. **Build.** The WPE build already makes `jsc` (`toolchain/wpe-webkit/build-webgpu/bin/jsc`,
   a payload). Link the same JSC objects into a title, following
   `port/webkit/apps/title/package.sh`:
   - libc++ 19;
   - `libc-title.a` (no raw syscalls);
   - `titlemem.c`'s `--wrap`s, so heap, anonymous mmaps and stacks are direct memory;
   - the SELF converter.

   Patches are local to this spike, the way Spike 4 kept its own copy, and
   go in `spikes/13-title-jit/patches/`:
   - `Options.cpp`: allow `useJIT` in this title (patch 0030 forces it off for titles);
   - `ExecutableAllocator.cpp` / `OSAllocator`: the JIT pool reserved and
     committed RW in direct memory, then `sceKernelMprotect` 0x07 (RWX), the
     stage 0 result. JSC needs no write scheme beyond what patch 0030 uses.
2. **Input/output.** A title gets no stdin from elfldr. Scripts come from
   `/data/jsc/*.js` and an `args.txt` (Spike 4's `JSC_MODE=uri` layout).
   Output goes to the title log server, as the other spike titles do.
3. **Tests**, reusing Spike 4's `js/` and `jit-check.sh`'s steps:
   - `correctness.js` with the JIT and with `--useJIT=0`;
   - `jit-stress.js` per tier (JIT, `--useFTLJIT=0`, LLInt, eager
     thresholds), with checksums compared against the LLInt run;
   - `jit-watchdog.js` (VM traps in JIT code);
   - `bench.js`, compared with the payload number (0.44 s, R7).
4. **A WebAssembly kernel shaped like the emulator.** This needs a new
   `js/wasm-big.js`, which builds its module in JS since there is no
   emcc/wat2wasm here. It should have one function of over 100 KB: an
   interpreter-style `br_table` loop over a 32 MB linear memory. That is
   the shape of the emulator's hot functions (22% of its time is in OMG
   functions over 100 KB). It prints its iterations per second each second
   for 60 s, and the tier the function is in. Run it:
   - in the **payload** `jsc` on CPU 14, with the browser title closed;
   - in the **payload** `jsc` on CPU 14, while a title is running and busy
     (the PPSA99017 `cpu` probe looping);
   - in the **title** `jsc`, unpinned and pinned to one game CPU, while the
     same load runs.

   Use the same WebKit revision and the same JSC options for all three.
   Fast memory stays off at first (the title has no reserved-range code
   yet), so the payload runs need `--useWasmFastMemory=0` too.

**Pass:**
- correctness 30/30;
- `jit-stress` checksums equal to LLInt in every tier;
- no fault in 10 minutes of `jit-stress` + `wasm-big`;
- `wasm-big` in the title at least matches the payload's best case (CPU 14,
  title closed), and clearly beats the payload while a title is busy.

The last line is what the in-title route would buy the emulator. Record it
per second, not only as a mean. FTL/OMG compile threads also run in the
title, so the run must say how many there were and where they ran.

## Out of scope (next spike, only if this one passes)

- Hosting the WebProcess in the title. WebKit has no supported
  single-process mode, so this needs its own feasibility study: the
  process launcher, the IPC connection in one process, and the lost crash
  isolation.
- WebAssembly fast memory in a title: patches 0129/0130's reserved ranges
  on direct memory, and the 2 GiB memory the emulator asks for.
- Budgets: the title image grows by the JIT tiers (~15 MB per executable,
  R7), and flexible memory must stay enough for the video decoder.

## Risks

- **Concurrent repatching.** JSC rewrites code while other threads run it
  (inline caches, jump replacement). With a dual mapping that is fine, as
  on Darwin. With RX↔RW flipping it is not: a thread executing the page
  while it is RW faults. This is why the gate prefers a dual mapping.
- **Where shared/dual memory is charged.** If shm memory counts against the
  title's flexible memory, a 256 MB pool is over half of it. In that case
  `dual-dmem`, or a smaller pool (`JSC_jitMemoryReservationSize`), is needed.
- **Execute-only text.** JSC never reads its own image's code, but any
  disassembler or code-dump option (`dumpDisassembly`, `dumpJITMemoryPath`)
  that does would fault. Keep them off.
- **Signals.** The GC's thread suspension (SIGUSR1) and the wasm fault
  handler have only run in payloads so far. A title's signal setup may
  differ (the browser title already handles SIGSEGV for its crash log).
- **Results don't transfer one to one.** The emulator also does WebGPU,
  audio and DOM work. `wasm-big` stands in only for its hot thread.

## Files

| file | role |
|---|---|
| `probe.c`, `build-probe.sh` | stage 0 title PPSA99017 (done; `tools/launch.sh --install build/titles/PPSA99017`) |
| `build-jsc-title.sh` | stage 1: JSC + shell objects → title, packaged as PPSA99017 (replacing the probe) |
| `jsctitle.c` | stage 1 title main: JIT pool mmap, output, exit, crash report, load threads, `/data/jsc/{args,cpu,load,env}.txt` |
| `jscenv.h`, `payloadenv.c` | the WebProcess's JSC settings, shared by the title and the relinked payload `jsc` |
| `js/wasm-big.js` | the emulator-shaped WebAssembly kernel (also runs in the payload `jsc`) |
| `results/` | stage 1 runs (`summary.txt` and one log per run) |
| `run.sh` | uploads `/data/jsc`, launches the title, collects the log |
