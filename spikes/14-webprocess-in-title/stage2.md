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
