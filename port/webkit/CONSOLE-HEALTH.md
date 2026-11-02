# Console health: payloads left behind by the title (2026-11-02)

After long sessions with many title launches, the console got slower until
it was restarted: the 130 MB child uploads to elfldr took longer, and
WebProcess raster on the same page cost about twice as much. The cause we
found is ours: **a title killed while it streamed a child to elfldr leaves
a payload that never runs and never ends**. Each one stays until the console
restarts, costs ~180 of the 8192 system page tables and ~150 MB of swap, and
every later launch and page got slower. Killing the title in the middle of
such an upload can also **panic the kernel**: it did once on purpose
(18:20:17, below), and two earlier console crashes and the 10-28 wedge came
during big elfldr sends or a kill.

Every normal stop raced this: the wind-down ends the WebProcess, the title
took that as a crash and reloaded the page, which started a new upload just
before the launcher's SIGKILL. Fixed in the title, the process manager and
the child (below). Verified on the console with normal stop cycles.

## Evidence (title log, kernel log)

Data: the title log (`tools/logserver.sh`, 931k lines over 2026-11-01/02,
286 launches, reboots where the title pid drops back to ~101) and the
klogsrv capture with Mac timestamps (SceShellCore's `VM Stats` and `FMEM`
lines every few seconds: page tables, swap, and every process's memory,
payloads included).

**One payload that never went away.** The kernel's FMEM list showed a
`payload.elf` of 356.8 MB address space (146 MB resident at first, 2.9 MB
later: swapped out, it never ran) from 2026-11-02 02:55:52 until the capture
ended at 10:22, through more than 100 title launches. 356.8 MB is a ps5wk-child
before WebKit starts (a running WebProcess has ~977 MB); a child that is
running its own code gives up after 20 s (no header) or 60 s (no title).
At that moment:

- 02:53-02:55 a build whose WebProcess crashed at start: the title relaunched
  it ~80 times (`payload.elf calls exit() exit_value=46`, i.e. the crash
  guard's 70, once a second), the error page's process crashing too.
- 02:55:51 the title (pid 675) was killed in the middle of that loop. Its
  last launch had no "is payload pid" line, and one pid more was used than
  the launches account for (756): the payload elfldr made of the cut-off
  stream.

| idle snapshots (no title running) | page tables (CPU) | swap out |
| --- | --- | --- |
| 02:41-02:52, before | 5006-5041 | 527-548 MB |
| 02:55:52, the payload appears | 5185-5190 | 525 MB |
| 03:07 onwards (its pages pushed out by the next runs) | 5180-5223 | 660-679 MB, until 10:22 |

**Later runs were slower** (same build family, same console session):

| period | elfldr send of the 130 MB child, median (p10-p90) | scrolljank `?busy=30` raster, ms per Mpx |
| --- | --- | --- |
| 11-02 before 02:55 | 230 ms (202-316), n 56 | 2.46 |
| 02:55 payload .. 10:41 | 305 ms (237-359), n 222 | 4.2-5.2 (25 runs) |
| after the title crashed at ~10:41 (one more pid unaccounted for) | 800 ms (616-942), n 32 | - |
| after the owner's restart (11:30-17:50) | 226 ms (208-272), n 158 | 2.35-2.40 (3 runs) |
| after the fix, fresh boot (6 cycles) | ~290 ms (227-414) | - |

The ~10:41 step: the title itself crashed (SIGBUS in the ad blocker's
JavaScriptCore update on the title's main thread; pids 1196-1209 all crashed
that way between 10:32 and 10:41, a separate bug) with a child launch
counted but no pid line. The kernel capture had stopped at 10:23, so the
second leftover is inferred from the pid count and the send times, not seen.
The raster column has one run before 02:55, and the build changed then too;
the jump back after the restart is on later builds. Upload throughput of the
title's GL tiles (`[wc]` lines) depends on tile size (0.09 ms for small
tiles, 0.16 ms for 0.6 MB ones), so "ms per upload" across pages is not a
health measure. MB per ms on the same page is: it is in the cycle tool.

**Not explained:** in the afternoon session (after the restart, no orphan
signature: steady sends, every pid accounted for by children and ad blocker
tasks), tile-upload throughput on the apple page fell from ~3.0 to ~1.2
MB/ms between launches 8 and 63 while raster on that page got faster.
Several builds from the other work landed in between, so this may be code,
not the console. It was not measured with a fixed build.

**Pids.** Each launch uses: title N, N+1 (the system's NPXS40093 dialog
process), then one per child (network, web, adblock tasks, WebProcess
restarts). The cycle tool's own ps payload adds one. A gap beyond that is
a process nobody logged.

## The race at every stop

`launch.sh --stop` -> launcher "stop" -> title wind-down: `about:blank`,
audio streams ended, `webkit_web_view_terminate_web_process`, main loop
quit, main returns, `title_exit.c` asks the launcher for the SIGKILL.
`onWebProcessTerminated` saw reason 2 (by API) and called
`webkit_web_view_reload`. In 416 of the 511 title runs in the log, the stop
shows `terminated (reason 2); reloading` and `launching web process 5` with
no pid. Usually the SIGKILL came before elfldr made a process. When it came
during the stream, elfldr got a cut-off ELF. The reference elfldr
(`ref/elfldr/socksrv.c`) spawns whatever it read once the connection ends,
if it starts with the ELF magic. The console's elfldr (it takes a header
line after the ELF) is another build; how it handles a cut-off stream is not
known, the payload above is what came of one.

## Kernel panic (2026-11-02 18:20:17, do not repeat)

To make an orphan on purpose: title on the start page, `POST
/restart?scope=crash` (the title SIGKILLs its WebProcess and reloads,
`launching web process 5`), 0.15 s later `launch.sh --kill`. The kernel log
in that second: `onSessionKilled pid=128` (the title), `Fatal trap 12: page
fault while in kernel mode`, then the console went off; the owner restarted
it. Treat **any kill of the title while a child upload or spawn is in
flight** as able to take the console down. Do not use `launch.sh --kill`
or crash-test-then-kill sequences.

Earlier unexplained events fit this: the README's two console crashes
(one during a 129 MB ps5wk-shot send to elfldr, one overnight after stalled
129 MB sends) and the 2026-10-28 wedge (`launch.sh --kill` without a reply
right after web5/web6 uploads of 4.5 s). The README's swap note: the step
of ~150 MB on 11-02 was the orphan; the growth on 11-01 (0 -> 382 MB over 9
runs, with no payloads left over after any run) is system processes pushed
out while a WebProcess holds 0.7-1 GB, as noted there.

## Fixes

- **No reload during the wind-down** (`apps/title/ps5browser.c`):
  `onWebProcessTerminated` returns when the wind-down has begun and logs
  `wind-down: the WebProcess ended (reason 2); no reload`.
- **Quiesce before the SIGKILL** (`wpe_process_manager_ps5_quiesce`,
  `wpe-ps5/WPEProcessManagerPS5.cpp`): after the main loop, no upload
  starts any more and the title waits (5 s at most) for one being sent;
  logs `quiesced: ...; N send(s) in flight at the call, none left after X ms`.
- **Crash-loop brake** (title): more than 6 WebProcess ends in 2 minutes
  (crashes, not ours) and the title stops starting new ones (`not starting
  another (restart the browser)`). Before, a WebProcess that died at start
  was relaunched ~80 times.
- **Children give up sooner** (`apps/child/ps5wk-child.cpp`): 10 s of
  ECONNREFUSED on the title's socket (its title is gone) ends the rendezvous
  instead of 60 s.
- **Title-start check** (child): the NetworkProcess lists processes and
  logs `[health] WARNING: N big payload(s) older than this title ...` for
  any `payload.elf` with more than 200 MB of address space started before
  the title (or `[health] no payloads left over ...`). It does not kill
  them: a ps5wk-shot run is a big payload too.

Not covered: a title that crashes (the SIGBUS of 10:32-10:41) or a forced kill
after the launcher's `STOP_WAIT` while an upload is in flight. The quiesce
keeps a normal stop well inside `STOP_WAIT` (10 s).

## Measuring it

- `port/webkit/test/ps/ps.elf` (`ps.c`, 111 KB, through elfldr from the
  Mac): every process with state, flags, tracer, address space, resident
  size, CPU time, threads, age, wait channel, and the `payload.elf` count.
  The console's `kinfo_proc` is 1096 bytes (the SDK header's 1088). Fields up
  to `ki_wmesg` read right.
- `port/webkit/test/health/cycles.sh N [dwell] [url]` (hold console.lock;
  `LOG=` the logserver output, optional `KLOG=` a klogsrv capture): per
  cycle a ps snapshot (payloads other than ps itself, with pid, state,
  size, age), the newest VM Stats, launch, dwell, `--stop`, then
  `parse-run.py` on that run's part of the log: children launched and
  connected, elfldr send ms, tile-upload MB/ms and small-tile ms, raster ms
  per Mpx, wind-down reload or not, the quiesce line.

Before the fix (fresh boot, 18:18-18:19, two cycles): each run launched 3
children and logged `RELOADED` at stop. After the fix (18:25-18:28, six
cycles, start page, 25 s):

| cycle | children launched / connected | sends (ms) | payload.elf after (launcher only) | idle page tables | wind-down |
| --- | --- | --- | --- | --- | --- |
| 1-6 | 2 / 2 each | 227-414 | 1 (pid 101) every time | 4960-4969 | no reload; quiesce: 0 in flight |

Pids stepped by exactly 5 per cycle (title, dialog, 2 children, ps).

## What to do

- Stop the title with `launch.sh --stop` (or the PS button). Never
  `launch.sh --kill` while it may be launching a child.
- If the title log shows `[health] WARNING`, or child sends stay above
  ~500 ms (`sent ... bytes + header to elfldr in N ms`), restart the console
  and re-run the jailbreak: a leftover payload can't be removed by us
  safely, and it slows everything until a restart.
- The ad blocker's update in the title crashed with SIGBUS (signal 10 in
  `[adblock-debug]`) four times on 11-02 between 10:32 and 10:41. Each title
  crash is a chance to cut an upload.

## Logging (2026-11-02: the title froze when the network went away)

The PS5 lost its network (no ARP reply, no ping) and the browser's picture
froze at that moment; the PS button still closed the title. The title's log
was the cause: `spike_log()` sent each line to `tools/logserver.sh` with a
blocking `send()`, and stderr (WebKit, GLib, the relayed `[webN]` lines of
the children) was an `fdopen()` stream on the same socket. With nobody
acknowledging, the socket's send buffer filled and every thread that logged
stopped in `send()`: the render thread at its next `[present]` line (every
5 s while frames go out), the main thread, the relay threads, and behind
them the children, whose stderr (the elfldr connection) nobody read. Shown
on the Mac with the old code (`test/logsink/run.sh`, case `direct`): with
the log server stopped, logging calls took 4-7 s each and no thread logged
for 10 s.

**Now** (`apps/title/titlelog.c`; spike.h's `spike_log_sink`, title.h's
`spike_title_log_start_hook`, `ps5wk_log_line_hook` in
WPEProcessManagerPS5.cpp):

- `spike_log`, the relay and stderr (now a pipe, drained by a `log-stderr`
  thread) put whole lines into a 512 KB ring under a mutex held for a
  memcpy. A full ring drops the line and counts it:
  `[log] N lines dropped: the log queue was full`.
- A `log-send` thread writes every line to `/data/ps5webkit/ps5browser.log`
  (now with stderr and the children's lines too; at 32 MB it becomes
  `ps5browser.log.1` and a new file starts) and to TCP through a 512 KB
  buffer: non-blocking connect (5 s timeout) and `send(MSG_DONTWAIT)` when
  poll says there is room. Lines that do not fit are dropped from TCP only
  and noted once it takes lines again: `[log] N lines dropped over X s: the
  log server was not taking them (they are in the file log)`. A connection
  that takes nothing for 30 s is reset (RST, so its leftovers never arrive
  after the new connection's lines); reconnects back off 0.25 s to 4 s:
  `[log] connection to the log server ... lost (...)`, `[log] connected to
  ... again after X s (lost: ...)`.
- At start: `[log] non-blocking log: TCP host:port (buffer 512 KB, ...)`;
  the title line says `tcp log to ... queued (non-blocking sender)`.
- The title's exit (`title_exit.c`, before asking for the SIGKILL) and
  `abort()` (`titleabort.c`) wait up to 0.5 s / 1 s for the queue to go out.
- Children: stderr writes wait 0.5 s at most (`SO_SNDTIMEO` on the elfldr
  connection, 256 KB buffer); the line is then cut or lost. Each child logs
  `stderr: a write waits 0.5 s at most (buffer 256 KB)`. Untested with a
  title that stops reading (the relay no longer does).
- Line order per source and the `[app msg]`, `[webN]` prefixes are as
  before; the log server and the parsing scripts are unchanged.

**Test knob:** `/data/ps5webkit/log-sink`, one line `[host]:port
[queue=<KB>] [stall=<s>]`, read at title start: the TCP log goes there
(e.g. `test/logsink/listener.pl -a 0.0.0.0 -r 8192 9998`, which can be
stopped with `kill -STOP` without touching `tools/logserver.sh`). Delete it
after a test; the start line shows it while it is there.

**Results.** Mac (`test/logsink/run.sh`: 5 threads, 2000 lines/s,
listener stopped 10 s or killed and restarted): slowest logging call 3-7 ms
(macOS scheduling) against 4-7 s for the old code; the file log complete;
in the listener's copy every thread in order, each gap right after a drop
note. Console (animated page plus a `console.log` every 100 ms, listener
on :9998, 16 KB TCP buffer): stopped for 25 s, 45 s and (with `stall=10`)
45 s, then killed for 10 s and restarted. The file log's `[present]` lines
show 300 frames (59.9/s) in every 5 s window through all of it (30 and 17
windows in the two runs); the listener got `[log] 290 lines dropped over
20.5 s`, the reset and reconnect notes, and the page's numbered lines with
one gap, right after the note. A normal `--stop` still delivers `closing
the title through the launcher (SIGKILL)`.

**What to know:**
- After a network outage, look in `/data/ps5webkit/ps5browser.log` (FTP)
  for what the log server missed.
- A connection reset after 30 s without progress can leave half a line in
  the log server's copy (the part the old connection had delivered),
  joined to the first line of the new one.
- Lines in the kernel's send buffer of a reset connection are lost without
  a count; the file has them.
