# WPE WebKit for the PS5 (Milestone 1)

WebKit 15486a0178, `PORT=WPE`, built with ps5-payload-sdk for a jailbroken PS5
on 13.60. Goal of Milestone 1 (FEASIBILITY §1a, §4.8): browse on the TV with
pages painted on the CPU.

- **Title PPSA99020 = UIProcess** (`apps/title/ps5browser.c`) with the PS5 WPE
  Platform backend (`wpe-ps5/`): the page on the TV through ps5-opengl's EGL
  window surface, DualSense input, the system keyboard (libSceImeDialog).
- **Payload `ps5wk-child.elf` = WebProcess and NetworkProcess** (one
  multi-call ELF, `apps/child/ps5wk-child.cpp`), started by the title through
  the local elfldr with Spike 8's reversed rendezvous (`wpe-ps5/PS5LaunchProtocol.h`).
  8 GB of flexible memory, JIT off (asm LLInt), Skia on the CPU, frames sent
  to the UIProcess as shared memory (WebKit's `RenderTargetSHMImage` path
  with `hardware-acceleration-policy = never`, no GL in payloads).
- **Payload `ps5wk-shot.elf`**: a UIProcess on WPE's headless display, for
  payload-only first light (loads a URL, reports a checksum and a thumbnail).

No WebGL, WebXR, GPU process, GStreamer/media, WebRTC, speech, geolocation,
sandbox, introspection, docs, journald or libbacktrace.

## Build

```sh
port/webkit/build.sh                     # src shim configure build
port/webkit/build.sh src                 # clone + patches (FRESH_SRC=1 re-applies)
port/webkit/build.sh configure           # cmake (log: toolchain/wpe-webkit/configure.log)
NINJA_TARGETS="ps5wk-child ps5wk-shot ps5browser" port/webkit/build.sh build
port/webkit/build.sh export-patches      # after editing the clone: commits -> patches/
port/webkit/apps/title/package.sh        # title folder -> toolchain/wpe-webkit/titles/PPSA99020
port/webkit/test/run-shot.sh [url]       # payload-only first light on the console
```

- Source: `toolchain/wpe-webkit/src`, a clone of `ref/WebKit` that uses its
  object store through `alternates` (ref/WebKit is never written), sparse
  (Source/ without libwebrtc). Branch `ps5` = 15486a0178 + one commit per
  patch; `export-patches` writes them back to `patches/`.
- Build: `toolchain/wpe-webkit/build`, Release, `-O2 -gline-tables-only`
  (`DEBUG_FLAGS=-g0` for the smallest build), ninja, all cores.
- Toolchain: `prospero-clang` 18 + libc++ 19.1.7 (`toolchain/libcxx19`),
  dependencies from `toolchain/wpe-sysroot` (port/deps, its
  `ps5-wpe.cmake` toolchain file and `pkg-config-ps5`), ICU 76.1.
- `FAKE_DEPS=1`: placeholder libepoxy/libxkbcommon (headers that `#error`)
  so configure completes and JSC/WTF/Skia build while port/deps finishes
  them. Not for a real build.
- WebKit is a static library (`WebKit_LIBRARY_TYPE STATIC` on PS5); the
  executables in `apps/` link it. The title relinks ps5browser's link line
  with PS5VR's native-app CRT and ps5-opengl (`apps/title/package.sh`).

## Patches (`patches/`, applied in order by `build.sh src`)

| patch | what |
| --- | --- |
| 0001-cmake-cross-compile-from-macOS-and-PS5-define | skip WebKitXcodeSDK and the compiler include-dir probe when cross-compiling from macOS; API tests off; `-DPS5_PAYLOAD_SDK=1` from OptionsJSCOnly and OptionsWPE when the SDK toolchain sets `PS5` |
| 0002-ps5-payload-sdk-is-FreeBSD-not-PLATFORM-PLAYSTATION | `__SCE__` no longer selects bmalloc/libpas PLAYSTATION; libpas on for FreeBSD x86_64; 16 KiB page ceiling |
| 0003-freebsd-portability-fixes | libpas FreeBSD monotonic clock, `<malloc.h>` guard, bytecode-cache version without `dladdr` |
| 0004-clang18-suppress-attribute-on-function | `[[clang::suppress]]` on a function is clang 19+ |
| 0005-jsc-shell-ps5-entry-point | `jsc` shell main for payloads (argv/stdin), for the JSC test payload |
| 0006-libpas-ps5-16k-pages | libpas commit/decommit within 16 KiB pages (found on the console in Spike 4) |
| 0010-wpe-platform-ps5-backend-and-process-manager | `ENABLE_WPE_PLATFORM_PS5`: builds `wpe/ps5` (a symlink to `port/webkit/wpe-ps5`) into WPEPlatform and WebKit; enables `WPEProcessManager` (Android-only upstream) for PS5 in WPEPlatform and ProcessLauncherGLib; WebKit as a static library; `apps/` added when `PS5_PORT_DIR` is set |
| 0011-wpe-on-freebsd-portability | WPE on a FreeBSD userland: WTF picks the generic memory footprint and no Linux-only sources (as PlatformGTK does), plus later fixes found while building |

| 0012-ipc-unix-log-failures | diagnostics: log every failure path in `Platform/IPC/unix/ConnectionUnix.cpp` |

0011 also carries: WTF FreeBSD sources, VIDEO=OFF build fixes (media sources,
TextExtraction, AudioEncoder/PlatformRawAudioData), Skia `malloc_np.h`, DNS
includes, WKAPICast PlayStation guard, `EGL_DEFAULT_DISPLAY` as an int on PS5,
xdgmime globals renamed (clash with GIO's copy in a static link), CPU-only
WPE preferences on PS5 (no hardware acceleration, no compositing), and the
128 MB JSC structure heap on PS5.

The memmem shim and `<sys/sysinfo.h>` from Spike 4 are in `shim/`
(force-included `ps5shim.h`, `libps5shim.a`).

## JSC `Date` (FEASIBILITY §8 item 5)

Not a clock bug. WTF's wall clock on this libc is
`clock_gettime(CLOCK_REALTIME)` (`WTF/wtf/CurrentTime.cpp`, generic branch),
and it is right on the console: Spike 4's unmodified `jsc.elf` prints
`new Date().toISOString() = 2026-10-27T23:45:41.311Z` (the Mac read 23:27 GMT
at upload; the run includes a 20 s upload). The "1970" in Spike 4 came from
`js/hello.js`, which prints `new Date(0)`. The payload time zone is UTC
(`getTimezoneOffset() = 0`, `Intl` tz `UTC`); the children set `TZ=UTC`.
Showing local time needs the console's time-zone setting (libSceSystemService)
later. Console log: `port/webkit/test/date.js`, results below.

## PS5 WPE Platform backend (`wpe-ps5/`)

| file | |
| --- | --- |
| `WPEDisplayPS5` | connects EGL (ps5-opengl, one window surface, GL 4.6 core), one 1080p `WPEScreen`, the pad (8 ms poll on the main loop), the IME dialog, and installs `WPEProcessManagerPS5` as the default process manager |
| `WPEToplevelPS5` | always fullscreen, the surface size |
| `WPEViewPS5` | `render_buffer`: copies the damaged rectangles of the WebProcess's SHM buffer into a BGRA texture, presents, then reports the buffer rendered and released |
| `PS5Renderer` | the GL side: page texture, round pointer sprite, swap (vsync) |
| `WPEInputMethodContextPS5` | the system keyboard for focused fields |
| `WPEProcessManagerPS5` | children as elfldr payloads (below) |
| `PS5Sce.h` | weak declarations of libScePad, libSceImeDialog, libSceUserService (no SDK headers) |

Pad map: D-pad = arrow keys (spatial navigation between links in the
browser); X = click at the pointer when it is shown (it shows for 8 s after
the left stick moves), otherwise Enter; O = Back; left stick = pointer;
right stick = scroll under the pointer; Options = URL/search prompt;
Triangle = keyboard for the focused field (it also opens by itself when X
focuses a field); Square = reload; L1/R1 = previous/next field; L2/R2 = zoom.

GL goes through libepoxy; in the title, `--wrap=dlopen/dlsym` and
`apps/title/dlshim.c` resolve epoxy's lookups to the statically linked
ps5-opengl (EGL 1.4 from its symbol table, GL from `eglGetProcAddress`).

## Process launching

`WPEProcessManagerPS5` (WebKit's `WPEProcessManager` hook):

1. `launch()` dups WebKit's client socket, starts a thread that streams
   `ps5wk-child.elf` to 127.0.0.1:9021 followed by
   `PS5WK-LAUNCH 1 <web|network> <id> /system_tmp/ps5wk/ui-<pid>.sock`,
   and returns at once (WebKit's messages wait in the socketpair).
2. The payload reads that line from stdin (elfldr hands it the rest of the
   stream), creates `/system_tmp/ps5wk` (0777) and connects to the title's
   socket; the title binds it as soon as the directory exists.
3. hello → welcome + the IPC fd (SCM_RIGHTS); the child calls
   `WebKit::WebProcessMain` / `NetworkProcessMain` with
   `<name> <id> <fd>` on a 64 MB-stack thread.
4. The child's stdout/stderr come back on the elfldr connection; the title
   relays them to its log, prefixed `[web<id>]` / `[network<id>]`.

## Findings on the console (2026-10-28)

- **CPU page tables are a system-wide pool of 8192 pages** (`VM Stats ...
  page table CPU n/8192` in the kernel log), and the system already uses
  ~4550. Address-space reservations cost page tables on the PS5 even when
  untouched, and JSC reserves a 4 GB structure heap per process (8 GB
  while aligning). Three WebKit payloads exhausted the pool (`[KERNEL] system
  page table exhausted`): elfldr then failed to map new payloads
  (`elfldr.c:190:pt_mmap: Cannot allocate memory`) and libpas's 128 MB
  reservation failed (SIGILL in `pas_compact_heap_reservation`). Fixed in
  0011: a 128 MB structure heap, as Sony's PlayStation port uses. Watch that
  counter whenever something reserves address space.
- **elfldr takes one payload at a time.** Two concurrent uploads left the
  second payload with unapplied relocations (SIGSEGV in a static
  initializer). `WPEProcessManagerPS5` serializes uploads, waits for the
  previous payload's first output line and retries "Error spawning payload".
  Serialized, a 93 MB upload takes ~0.3 s and a child is connected
  0.7–1.3 s after `launch()`.
- **ICU data is not linked in** (stub data library) and is read from
  `/data/ps5webkit/share/icu/icudt76l.dat` (`ICU_DATA`), which keeps the
  child at 93 MB instead of 125 MB.
- The payload wall clock is right (see JSC `Date` above).

## Status and timings (checkpoint 2026-10-28 ~02:00)

### Builds (Mac, 14 cores, Release, -O2 -gline-tables-only)

| | |
| --- | --- |
| clean full build (`rm -rf build`, configure + ninja of ps5wk-child, ps5wk-shot, ps5browser, 8365 steps) | **747 s wall** (configure 37 s, ninja 710 s) |
| incremental relink after a one-file change | 7–31 s |
| disk | build ~4.8 GB, source clone ~0.85 GB (objects shared with ref/WebKit) |

All three executables link: `ps5wk-child` (WebProcess+NetworkProcess payload,
93 MB stripped, ICU data external), `ps5wk-shot` (headless UIProcess payload),
`ps5browser` (UIProcess, linked as a payload ELF; `apps/title/package.sh`
relinks it as the title, not yet run).

### Console (payload-only, `test/run-shot.sh`)

Latest run (example.com):

    RESULT ps5wk-shot setup PASS UIProcess up at 0.0 s, loading https://example.com/
    RESULT ps5wk-shot load FAIL https://example.com/: WebKit encountered an internal error (domain WebKitErrorDomain code 300)
    RESULT ps5wk-shot network PASS first response committed at 1.7 s
    RESULT ps5wk-shot paint FAIL no frame painted within 40 s (load finished)

Working: elfldr launch of both children from a payload UIProcess (upload
~0.3 s, connected 0.7–1.3 s after launch), the reversed rendezvous with
SCM_RIGHTS, WebProcess and NetworkProcess initialisation, NetworkProcess TLS +
HTTP/2: with `SHOT_DEBUG=Network,Loading,Process` it logs `GET / HTTP/2` to
example.com and `didReceiveResponse: (httpStatusCode=200, MIMEType=text/html`.

**Blocker:** right after the NetworkProcess sends
`WebResourceLoader::DidReceiveResponse` to the WebProcess, the
WebProcess<->NetworkProcess IPC connection closes: the WebProcess logs
`WebProcess::networkProcessConnectionClosed: NetworkProcess closed its
connection (Crashed)` and the NetworkProcess logs
`NetworkConnectionToWebProcess::didClose: WebProcess closed its connection`.
Then `WebLoaderStrategy::networkProcessCrashed` fails the load (internal error
code 300, `WebLoaderStrategy.cpp(693)`), so nothing is painted. Neither
process crashes (klog: both exit 0 when the UIProcess quits).
The injected-bundle warning (`Error loading the injected bundle
.../libWPEInjectedBundle.so`) is harmless (no dlopen; only the
WebProcessExtension API needs it).

Ruled out / done:
- not page tables, not elfldr (fixed, see Findings);
- SCM_RIGHTS over SOCK_SEQPACKET socketpairs works on the console
  (`test/seqpacket`: stream/seqpacket/dgram all PASS, passed twice);
- patch 0012 logs every failure path in `Platform/IPC/unix/ConnectionUnix.cpp`
  (malformed message, out-of-line body map failure, MSG_CTRUNC, ECONNRESET,
  full read buffer). **None fired** in the last run, so the close does not
  come from those paths.

Hypothesis: the close comes from the GLib read monitor
(`Connection::m_readSocketMonitor`, a `GSocketMonitor` on the IPC socket,
`Connection.h` USE(GLIB)) reporting G_IO_HUP/G_IO_ERR, or from the sender
side. deps notes that glib's ppoll is broken on PS5 and disabled; a GSource
condition on a SOCK_SEQPACKET socket may report HUP/ERR wrongly on this
kernel. DidReceiveResponse is probably the first large message on that
connection: it carries a CertificateInfo, so its body goes out of line as a
SharedMemory fd.

Next steps:
1. Find where `m_readSocketMonitor` is started (grep `m_readSocketMonitor`
   in Platform/IPC, probably `Connection.cpp`/`ConnectionUnix.cpp` under
   USE(GLIB)) and log the GIOCondition that leads to `connectionDidClose()`;
   also log in `sendOutputMessage` failures (EPIPE is not treated as close
   on non-Linux).
2. Test on the console: a 2-process (or 2-thread) GSocket/GSource watch on a
   SOCK_SEQPACKET socketpair, sending a message with an fd attachment, and
   print the conditions seen (extend `test/seqpacket`).
3. If it is the monitor: patch it on PS5 (ignore HUP when data is readable,
   or poll with a plain fd watch), or make IPC use SOCK_STREAM on PS5
   (`SOCKET_TYPE` in ConnectionUnix.cpp; the message framing would need
   checking).
4. Then rerun `test/run-shot.sh` for paint, then package and hand over the
   title (`apps/title/package.sh`).

Debug knobs: `/data/ps5webkit/child.env` (KEY=VALUE lines read by the
child, e.g. `WEBKIT_DEBUG=Network,Loading,Process`); `SHOT_DEBUG=...` for
`run-shot.sh` sets WEBKIT_DEBUG in the UIProcess, which the NetworkProcess
inherits through its creation parameters; `ENABLE_RELEASE_LOG=ON` prints
RELEASE_LOG to stderr. The kernel log (`logs/klog-*.log`) shows crashes with
registers; symbolize with
`llvm-addr2line -e toolchain/wpe-webkit/build/bin/ps5wk-child <rip - 0x200000000>`.
