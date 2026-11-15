# Spike 14 stage 0: results (2026-11-15, 13.60)

**Gate: passed (1.41×). Route A is closed (two titles cannot run at once), so
the next step is route B, stage 1.**

Runs: `spikes/13-title-jit/results/20261115-055259` (`summary.txt` and one log
per run). Summary: `evidence/2026-11-15/14-webprocess-in-title-stage0.log`.
Tools: `STEPS="install s0a s0b s0c" spikes/13-title-jit/run.sh`.

Every run used the same WPE build's JSC, with the WebProcess's JSC settings
(`jscenv.h`). It ran `wasm-big.js` (one 142 KB function, a `br_table`
interpreter loop) for 30 s, and every run printed the check
`fa681e86-f0492106`. The rate is the mean of the last 10 one-second samples.

Side threads (`jscside.h`) stand in for the fight's other WebProcess threads
(main thread 19%, second worker 14%, audio, compile helpers). Each spins 4
xorshift chains for its duty share of every 10 ms and sleeps the rest.

## 0a: the payload `jsc`, shell thread pinned to CPU 14

| run | title | side threads | Msteps/s |
|---|---|---|---|
| a0 | none | none | 2.85 |
| a1 | idle (PPSA99017, 0 threads) | none | 4.24 |
| a2 | idle | 15 at 50% | 3.76 |
| a3 | idle | 15 at 100% | 3.33 |
| a4 | idle | 15 and 13 at 50% | **3.72** |
| a5 | busy (12 threads on 1–12) | 15 and 13 at 50% | **3.69** |

- A side thread on CPU 13 asked for 50% got 66% of wall time spinning, at 135–141
  Mloops per spinning second, against 430 on CPU 15. That is a third of the
  speed, as found before: CPU 13 is shared with something while a title runs.
- **a0 is unexplained.** With no title running, CPU 14 gave 2.85, steady
  from t=3 s. Spike 13's unpinned no-title run gave 4.95. Something else
  probably uses 14 (or its core) when no game runs. This doesn't affect
  fights, which always have a title running.
- The busy title (a5) costs the payload almost nothing beyond the idle one
  (a4). What hurts is the sibling: 15 at 100% costs 21% (a1→a3), and the
  fight's shape costs 12% (a1→a4).

## 0b: the title `jsc`, no other title running

The title's APIC map (CPUID leaf 1, `cpu:apic`):
`0:15 1:14 2:13 3:12 4:11 5:10 6:9 7:8 8:7 9:6 10:5 11:4 12:3`. So the cores
are {0,1} {2,3} {4,5} … {10,11}, and **12 shares a core with payload CPU
13** (APIC 3 and 2). In payload terms the full list is APIC = 15 − CPU.

| run | jsc | side threads | Msteps/s |
|---|---|---|---|
| b1 | CPU 3 | none | 5.20 |
| b2 | CPU 3 | sibling 2 at 100% | 4.15 |
| b3 | CPU 3 | 2 (sibling) and 1 at 50%: the payload's placement | 4.68 |
| b4 | CPU 3 | 1 and 4 at 50%: other cores | **5.19** |
| b5 | unpinned | two unpinned at 50% | **5.25** |

- A busy sibling costs the same in the title (b1→b2, −20%) as in the payload
  (a1→a3, −21%).
- Left alone (b5), the title's scheduler kept the side threads off jsc's core:
  5.25, as fast as jsc alone.
- Side threads on title cores spun at 502–515 Mloops/s, against 430 on
  payload CPU 15 with the same code. Even with nothing on its sibling, the
  payload's core runs about 20% slower (a1 4.24 against b1 5.20). The cause
  isn't measured: clocks, or system threads on cores 14/15.

## Gain

Title, fight's shape (b4 5.19, b5 5.25) ÷ payload, fight's shape (a4 3.72,
a5 3.69) = **1.40–1.42×**.
- About 1.23× of that comes from the core itself (b1 ÷ a1).
- About 1.14× comes from not sharing it (a1 ÷ a4).

The gate was 1.15×, against the 1.7–2× fights need for 60 frames/s. If fights
scale with the emulator thread, 28–35 frames/s would become about 40–50.

Caveats:
- `wasm-big` is a stand-in for the emulator's hot code.
- The side threads are synthetic. The real ones float over 13/14/15 rather
  than being pinned.
- The real WebProcess's emulator worker is not pinned to 14, so the payload
  side may be better or worse than a4.

Stage 2's frame-rate A/B is the real test.

## 0c: two titles at once

PPSA99017 was running (one load thread, a beat every 5 s). Then
`tools/launch.sh --also PPSA99018`, a second registered copy of the same
title.
- The launch succeeded, but only with a fallback system authid
  (`0x4801000000000013`): the launcher's own authid was refused while a title
  ran.
- **PPSA99017 was gone within 5 s.** It logged no beat after its start, and
  the app list showed only PPSA99018 from then on.
- PPSA99018 ran at full speed (578 M loops/s per load thread) until it was
  killed at 60 s.

So launching a second title replaces the first. Route A (the WebProcess as
its own title) is not possible this way. Other app kinds (mini-apps, system
services) were not tried.

## Notes

- `tools/launcher` gained `launch-also`, `apps` and `killtitle`, used by
  `tools/launch.sh --also`, `--apps` and `--killtitle`. `killtitle` takes
  PPSA99xxx titles only.
- PPSA99018 ("ps5webkit spike 13 (PPSA99018)") is now registered on the
  console. `tools/launch.sh --uninstall PPSA99018` removes it.
- For stage 1, thread placement inside the title is worth up to 11% by
  itself: compare b3 and b4. Keep the emulator worker's sibling idle.
