# Spike 14 stage 2: the fight (2026-11-15)

**Result: fights run 1.7–1.9× faster with the WebProcess in the title.**
In-title: median 48 frames/s, mean 47.6. Payload: median 25, mean 28.2.
The slowest tenth of seconds went from 22 to 44 frames/s. The pass line was
1.15× the payload's median.

Setup:
- One build, PPSA99209 (build `20261115-113730-91f31930c3`). Only the switch
  `/data/ps5webkit/inprocess-web` changed, read at title start.
- Six runs alternating on/off, on the owner's chosen matchup and stage, two
  rounds each.
- `tools/fps-bench.py`, first 90 fight seconds of each run (run 5: 87). Run
  1 was taken at 120 s, and its last 15 s were the next opponent, so only its
  first 90 count.
- No profiler, probe or extra logging.
- CSVs: `logs/bench-s2-*.csv`. Summary:
  `evidence/2026-11-15/14-webprocess-in-title-stage2.log`.

| Run | WebProcess | Mean | Median | p10 | p90 |
| --- | --- | --- | --- | --- | --- |
| 1 | in the title | 48.2 | 49 | 45 | 51 |
| 2 | payload | 27.8 | 25 | 21 | 40 |
| 3 | in the title | 48.0 | 47 | 44 | 54 |
| 4 | payload | 27.9 | 25 | 22 | 40 |
| 5 | in the title | 46.6 | 47 | 43 | 53 |
| 6 | payload | 28.8 | 27 | 22 | 39 |
| **all, in the title** (267 s) | | **47.6** | **48** | **44** | **53** |
| **all, payload** (270 s) | | **28.2** | **25** | **22** | **40** |

Ratios: mean 1.69×, median 1.92×.

Notes:
- The runs agree closely within each side, so the matchup was fixed well.
  This matchup is slow in a payload: median 25, against 30–31 in earlier
  sessions' mixed fights.
- Stage 0 predicted 1.41× for the emulator thread alone. The fight gained
  more, so the payload probably lost more to the shared SMT sibling and the
  thin CPU 13 than the synthetic side threads modelled.
- Fights are still under 60. The emulator thread is still the limit, now on
  a full core (not profiled in the title yet).
- The owner: "it runs great. It feels full speed to me."

- Sound, from the owner: "sound is much better. There is still a slight
  stutter but this is likely because the game is not running at 60fps". The
  page's AudioWorklet reads a ring the emulator fills, so the remaining
  stutter fits an emulator still short of real time (48 of 60 frames/s).

Not measured yet (stage 2's other pass items):
- main-loop stalls over 100 ms in the title (the UI shares the thread);
- a CPU profile of the in-title fight and thread placement.

## Toward 60 (2026-11-15)

Profile of an in-title fight (title CPU profiler, stage 2 matchup,
`logs/prof/title-106.prof`, last 56 s; 49.3 frames/s while profiled):

| Thread | Share of a core |
| --- | --- |
| emulator worker (`WebCore: Worker`) | **99.1%**: 74% wasm OMG, 23.5% wasm BBQ in functions over 100 KB |
| title main thread (UI and the page) | 37% (about half of it page JS) |
| emulator's second worker | 24% |
| compositor | 13% |
| Dawn (`WebGPU_work_queue`) | 9% |

The emulator worker is still the limit, now on a core of its own.

`JSC_maximumOMGCandidateCost=2000000` (default 100000; set with a line in
`/data/ps5webkit/inprocess-web`) lets JSC optimize the big functions:
- profiled (`title-112.prof`): BBQ 0%, OMG 78% plus 18% in the over-100 KB
  functions; the worker at **93.7%** of a core;
- frame rates, same matchup: 49.5 (median 49, p10 47) unprofiled, 50.7
  (median 50, **p10 49**) profiled. Stage 2 in-title runs: 46.6–48.2, p10
  43–45.
- About +2–5% and steadier; the owner: "almost felt too fast". PS2 game
  logic runs per frame, so the game now runs close to its real speed. An
  earlier session saw no gain from this setting in a payload, where the
  thread shared its core.

Left: the worker has about 6% headroom, so something besides it holds a
little back (waits on the second worker or frame handoff?). 60 needs about
1.15–1.2× more from the worker thread.

The worker's waits (profiler `threads` mode, `title-115.prof`): the main
emulator worker waits about 6% of the time, all in `Atomics.wait` (wasm
`memoryAtomicWait32`): the emulator's own hand-offs between its two
workers. The second worker waits about 70% of the time.

Thread placement (`apps/common/affinity.c`, the owner's uncommitted code,
compiled into the title package when present, `PS5WK_AFFINITY` from the
switch file): the emulator worker alone on CPU 3, CPU 2 (its sibling)
empty, every other title thread on 0, 1 and 4-12.
- First try: the hot slot stayed on a compile thread, so the worker never
  moved.
- Second: the worker pinned at 0.99 cores. Median 50, mean 48.7, including
  7 s at exactly 30 in one round; about 50 without them.
- No gain over the OMG setting alone (49.5–50.7): the title's scheduler
  already kept the worker's core free (as stage 0's b5 found).

| Step | Fight median | Emulator worker |
| --- | --- | --- |
| payload | 25 | shared core |
| in the title | 47–48 | 99% of a core |
| + `JSC_maximumOMGCandidateCost=2000000` | 49–50, p10 47–49 | 94–99% |
| + worker pinned alone | 50 | 99% |

About 50 is the ceiling from the browser side. The emulator's main thread
is saturated on its own core, in code that is all at JSC's top wasm tier.
60 needs about 1.2× from that one thread: from the emulator itself, or from
JSC's code for its hottest functions (per-function profiling first).

### Where the worker's time goes, by WebAssembly function

`JSC_maximumOMGCandidateCost=2000000` is now the in-title default
(`apps/title/ps5browser.c` setUpInTitleWeb); the placement hook is gone.

The profiler now writes JIT frames as `j<tier>:<address>`, and
`prof.py --jitdump jit-*.dump` names them from JSC's JITDump
(`JSC_useJITDump=true`, `JSC_jitDumpDirectory=/data/ps5webkit/prof` in the
switch file); `--code-dir` writes the hottest functions' machine code with
per-instruction sample shares. JITDump in the title needed WTF's main
thread made before `JSC::initialize()` (`WebKit2Initialize.cpp`, PS5 only):
the thunks JSC compiles at start-up started PerfLog's WorkQueue thread,
which took WTF thread uid 1 and failed `initializeMainThread`'s assertion.

Profile `title-106.prof` (two rounds, profiler on), the emulator's main
worker, inclusive:
- `ps2::vu::run` (the VU0 interpreter, called per instruction from the
  recompiled EE code, `gen::unit_*`): 36%, 32% self. Each call sets up a
  384-byte frame in linear memory and copies vector registers; the hot spots
  are loads and stores, not arithmetic.
- `ps2::vu::maddabc` and its lambda: 8.5% self; `lower()` (called from
  `run` per instruction): 4% self.
- `gen::unit_*` (recompiled EE blocks): about 10% self.
- `usleep` and futex waits from `on_vblank`: about 4%.

JSC's code for these is ordinary for interpreter code (fast memory, no
bounds checks; spills around calls); no single codegen fault stands out.

Inlining larger callees (`wasmInliningMaximumWasmCalleeSize=4000`,
`LargeFunctionGrowthFactor=2.5`, `Budget=60000`, `MaximumCount=200`):
median 50, mean 49.8, p10 47 over 94 fight seconds: no change.

So about 50 stays the browser-side ceiling. The largest single item is the
emulator running VU0 macro instructions one call at a time through its
interpreter; recompiling them along with the EE blocks (as it already does
for VU1 microprograms, the `vu1_*` modules) is the emulator author's lever.
