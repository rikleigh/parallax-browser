# Spike 8: title ↔ payload IPC

The architecture in FEASIBILITY §4.4c puts WebKit's UIProcess and GPU process
in a registered title and the WebProcess/NetworkProcess in elfldr payloads.
This spike checks the link between them.

- `client.c` is the title (PPSA99009). It launches `server.elf` itself
  through the local elfldr (127.0.0.1:9021), the way WebKit's ProcessLauncher
  would, then runs sessions over each transport. It exits through
  `sceShellCoreUtilExitApp` (`psvr2_exit_app()`).
- `server.c` is the payload, the stand-in for a WebProcess. It creates a named
  shm `/ps5wk-ipc` (mode 0666), a unix socket `/system_tmp/ps5wk-ipc.sock`
  (mode 0777), loopback TCP on port 19500, and a 0777 directory
  `/system_tmp/ps5wk` for the title's own socket.
- `ipc_proto.h` holds the 64-byte message format, the shm pattern helpers and
  the in-process round-trip baseline.

Each session does a handshake, then anonymous shm fds in both directions
(unix only), the named shm, 20,000 round trips of 64 B, and a 64 MB bulk
transfer.

## Run

    spikes/08-title-ipc/build-title.sh
    tools/upload-title.sh spikes/08-title-ipc/build/titles/PPSA99009
    # PS5Upload: Games -> Game files -> PPSA99009 -> Play
    # results arrive at tools/logserver.sh

To run the server by hand, `make -C spikes/08-title-ipc send` before
launching. The title then skips `spawn-from-title`.

## Results (13.60, 2026-10-27, two runs)

`evidence/2026-10-27/08-title-ipc.log` (kept locally, not published)

| Test | Result |
| --- | --- |
| spawn-from-title | **PASS**: server listening 1.6 s / 5.2 s after the upload; it has the payload's 8 GB of flexible memory |
| unix-connect (title → payload socket) | **FAIL, EPERM**: the title may not connect to a socket owned by the payload, whatever its mode |
| unix-back (title listens in `/system_tmp/ps5wk`, payload connects) | **PASS**: shm fds passed **both ways** with SCM_RIGHTS; RTT 131 µs; 112 MB/s |
| tcp (loopback) | **PASS**: RTT 186–249 µs; 347–360 MB/s |
| named shm across processes | **PASS** both ways (run 2's tcp FAIL was a test bug, since fixed: the pattern check sampled the earlier session's note) |
| exit-app | **PASS**: the call returns 0 and the title closes with no "debug error" (checked on the TV). Now in every title: `spikes/common/title_exit.c` |

In-process round-trip baselines (64 B, socketpair / loopback TCP):

- title: **18 µs / 46 µs**
- payload: **73 µs / 116 µs**

The payload's own threads are about 4× slower than the title's. Payload
scheduling (priority or core set) explains most of the cross-process
latency, not the title/payload boundary.

## What it means for the port

- The **rendezvous is reversed**: the title (UIProcess) binds the socket in a
  directory a payload has prepared, and each child process connects to it.
  WebKit's launcher normally hands the child a socketpair end. A PS5
  ProcessLauncher instead passes the child the socket path (or a per-launch
  token) on spawn and accepts the connection. After that, `IPC::Connection`
  on Unix sockets with SCM_RIGHTS works unchanged.
- One payload has to create `/system_tmp/ps5wk` (mode 0777) before the title
  can listen. The first payload the title spawns can do it. A
  "launcher" payload that stays resident is another option.
- Latency: WebKit's sync IPC pays about 130 µs per round trip. WebGL goes
  through the GPU process's shared-memory stream (StreamConnection), so it
  mostly avoids this. Raising payload thread priority is the first thing to
  try.
- Spawning takes seconds, so WebKit's process prewarming matters.
