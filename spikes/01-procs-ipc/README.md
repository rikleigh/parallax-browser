# Spike 1: processes, IPC, memory, JIT memory

**Gate:** WebKit's multi-process model is possible. That means a UIProcess
can start a child process, hand it a connected socket (`SCM_RIGHTS`), and
share memory with it. If not, WebKit needs an in-process launcher that runs
the Web/Network process mains on threads. That's a major cost and risk (see
FEASIBILITY.md, R1, which also covers the Chromium alternative).

## Why these tests

- WebKit's launchers create a socketpair and give the child one end.
  `ProcessLauncherGLib` uses `g_spawn`. `ProcessLauncherPlayStation`, Sony's
  own port, passes the client fd to its private `PlayStation::launchProcess`.
  libwpe also has a pluggable process provider (`wpe_process_launch`), which
  is our insertion point.
- On a jailbroken PS5, the known way to start an arbitrary ELF in a new
  process is what elfldr does: `sceKernelSpawn` a `SceSpZeroConf` process,
  then inject the ELF over ptrace using kernel R/W. The **`elfldr-spawn`**
  test does exactly what a PS5 process provider would. It uploads the child
  ELF to the local elfldr (127.0.0.1:9021). The child connects to a
  rendezvous Unix socket, gets its fd over `SCM_RIGHTS`, and both sides then
  share memory. If that works, `ProcessProviderPS5` is
  "elfldr_send(child) + rendezvous", with elfldr's spawn code linked in later
  instead of going through the socket.

## Tests (`RESULT 01-procs-ipc <test> ...`)

| Test | What |
| --- | --- |
| threads | 32 pthreads with 1 MB stacks |
| fork | `fork()` + pipe + `waitpid` |
| fork-fdpass | socketpair + `SCM_RIGHTS` across fork, both sides map the passed shm |
| shm-anon | `shm_open(SHM_ANON)` shared across fork |
| shm-named | named POSIX shm |
| unix-path | `AF_UNIX` socket at a filesystem path, `SCM_RIGHTS` over it |
| elfldr-spawn | embedded `child.elf` spawned through the local elfldr, rendezvous, shm both ways, a pipe fd passed back |
| dmem-share | the child maps the parent's direct (GPU-visible) memory by its offset. This is the zero-copy route for WebProcess → XR/compositor buffers (Spike 7) |
| mem-flex | anonymous memory one process can commit (capped at 6 GB, then freed) |
| mem-direct | direct (GPU-visible) memory one process can map (capped at 4 GB) |
| jit-rwx | RWX anonymous page executes |
| jit-mprotect | RW → RX `mprotect` executes |
| jit-sce | Sony's `sceKernelJitCreateSharedMemory` + alias double mapping (W^X). This is what the system browser's JIT uses |

The JIT probes recover from SIGSEGV/SIGBUS and run last. The
`sceKernelJit*` signatures come from PS4 homebrew usage and are **unverified**
on PS5. If `jit-sce` fails with an error code rather than a fault, the
signature is the first suspect.

## Run

```sh
. ./env.sh
make -C spikes/01-procs-ipc send
```

Run it again from **title context**, since WebKit ships as a title and a
title's sandbox may forbid what a payload is allowed. Spike 2's title, built
with `-DWITH_PROCS_TESTS`, links `procs_ipc.c` with `-DPROCS_IPC_LIBRARY`
and runs the same tests, reporting them under `02-gl-egl`'s log.

## Results

_Pending a console run._
