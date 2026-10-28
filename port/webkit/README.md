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

| 0012-ipc-unix-log-failures | diagnostics: log why each IPC connection closes, in `Platform/IPC/glib/ConnectionGLib.cpp` (the file GLib ports use) |

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

## Status and timings (2026-10-28)

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

First light 2026-10-28: the WebProcess payload paints pages with Skia on the
CPU and hands them over as SHM buffers (thumbnail of the first one:
`test/first-light-example.com.png`).

    RESULT ps5wk-shot setup PASS UIProcess up at 0.0 s, loading https://example.com/
    RESULT ps5wk-shot network PASS first response committed at 1.4 s
    RESULT ps5wk-shot load PASS https://example.com/ finished at 1.4 s, title "Example Domain"
    RESULT ps5wk-shot paint FAIL 1280x720, 77 frames, first at 1.4 s, fnv64 b8e5138fc7b8a468, 100.00% non-white pixels
      (the page rendered correctly; the check counted the light-grey background as "non-white" and was fixed afterwards)

    RESULT ps5wk-shot setup PASS UIProcess up at 0.0 s, loading https://en.wikipedia.org/wiki/PlayStation_5
    RESULT ps5wk-shot network PASS first response committed at 1.5 s
    RESULT ps5wk-shot load PASS https://en.wikipedia.org/wiki/PlayStation_5 finished at 3.5 s, title "PlayStation 5 - Wikipedia"
    RESULT ps5wk-shot paint PASS 1280x720, 346 frames, first at 1.4 s, fnv64 4bdf213cb91b1482, 99.97% of pixels differ from the background

Both children launch through elfldr from a payload UIProcess (upload ~0.3 s,
connected 0.7–1.3 s after launch). The NetworkProcess runs TLS (OpenSSL) and
HTTP/2, and fontconfig/Noto (CJK included), images, CSS and JS all work.

What blocked it last, for the record: WebProcess<->NetworkProcess IPC
closed at the first out-of-line message. The cause was
**`fcntl(fd, F_DUPFD_CLOEXEC, 0)` returning 0 (fd 0, the payload's stdin
socket) in libkernel**, so `WTF::dupCloseOnExec` sent a socket in place of
shared memory (`test/seqpacket/dupfd.c`). Fixed with `__wrap_fcntl` in
`shim/ps5shim.c` (`-Wl,--wrap=fcntl`); requested in port/deps/REQUESTS.md
for ps5compat, since glib uses it too. Patch 0012 makes every
IPC-connection close path in `Platform/IPC/glib/ConnectionGLib.cpp` log why.

Other probes in `test/` (payloads, RESULT lines on stdout):
`jitpool` (2048 MB of JIT shm in one payload, no limit hit),
`seqpacket/seqpacket` (SCM_RIGHTS over stream/seqpacket/dgram PASS),
`seqpacket/shmfd` (SHM_ANON and named shm fds pass and map), `seqpacket/dupfd`
(the F_DUPFD_CLOEXEC bug), `seqpacket/reserve` (RW or PROT_NONE
reservations do not reduce `sceKernelAvailableFlexibleMemorySize`).

### Title PPSA99020 on the TV (2026-10-28, night run)

**The title loads and shows pages on the TV.** Verified from the log and a
screen readback (nobody watching the TV): example.com, Wikipedia and
DuckDuckGo Lite, the right way up and in the right colours.

    RESULT ps5browser display PASS PS5 display connected
    RESULT ps5browser load PASS https://example.com/ "Example Domain" (2.5 s after start)
    RESULT ps5browser url PASS 1/3 https://example.com/ loaded in 2.5 s: title "Example Domain"; flexible 334.0 MB
    RESULT ps5browser load PASS https://en.wikipedia.org/wiki/PlayStation_5 "PlayStation 5 - Wikipedia" (2.5 s after start)
    RESULT ps5browser url PASS 2/3 https://en.wikipedia.org/wiki/PlayStation_5 loaded in 2.5 s: title "PlayStation 5 - Wikipedia"; flexible 334.0 MB
    RESULT ps5browser load PASS https://lite.duckduckgo.com/lite?q=ps5 "ps5 at DuckDuckGo" (0.9 s after start)
    RESULT ps5browser url PASS 3/3 https://lite.duckduckgo.com/lite?q=ps5 loaded in 1.0 s: title "ps5 at DuckDuckGo"; flexible 334.0 MB
    RESULT ps5browser urls PASS 3 of 3 test URLs loaded
    [wpe-ps5] pad: init 0x0, user 285379873, open -> 53085952

Screen readbacks (960x540, halved from the 1080p frame on the TV):
`test/title-01-example.com-2s.png`, `test/title-03-en.wikipedia.org-2s.png`,
`test/title-05-lite.duckduckgo.com-2s.png` (+ the `-10s` ones, and
`title-first-frame.png`, the blank first frame). Load times count from
`webkit_web_view_load_uri`; a new WebProcess payload for the first page adds
~2 s before that (upload ~0.35 s, connected 1–2.3 s after launch).

Flexible memory (`sceKernelAvailableFlexibleMemorySize`, `[mem]` lines):

| milestone | flexible free | direct memory in use |
| --- | --- | --- |
| main (image already charged: 440 -> 335 MB) | 335.0 MB | heap 2 MB |
| display connected (EGL/GL up) | 334.0 MB | heap 12.8 MB |
| web view created | 334.0 MB | heap 22.8 MB, anon mmaps 256 MB (libpas 128 MB compact reservation, JSC 128 MB structure heap), 3 thread stacks |
| after example.com / Wikipedia / DDG | 334.0 / 334.0 / 334.0 MB | heap 26.8 MB (peak 34.7), anon mmaps 264–272 MB, 9–15 thread stacks (1.1–1.9 MB) |

Everything the UIProcess allocates is in direct memory; flexible memory
stays at 334 MB through three page loads (~190 MB more than Spike 10's
budget needs for the 8K decoder at depth 2).

#### Title vs payload: what broke, and the fixes

| symptom | cause | fix |
| --- | --- | --- |
| `[GLib ERROR] Creating pipes for GWakeup: Invalid argument` | `pipe2(O_CLOEXEC)` EINVAL in a title | `__wrap_pipe2` (`shim/ps5shim.c`): pipe() + flags by hand |
| `pad: libScePad/libSceUserService not linked` | only weak references to the SCE functions: imports came out NULL | strong references in `apps/title/dlshim.c`; the pad opens for the logged-in user |
| SIGILL in libpas (`pas_bootstrap_free_heap...`, `pas_immortal_heap...`) at `webkit_settings_new` | anonymous mmaps (libpas's 128 MB compact heap reservation) failed: flexible memory | `apps/title/titlemem.c`: anonymous RW mmaps -> direct memory (MAP_ALIGNED honoured, partial munmap, MAP_FIXED re-zeroing); Spike 10's `dheap.c` for malloc instead of ps5-opengl's 128 MB `app_heap.c`; thread stacks in direct memory (`--wrap=pthread_create/join/exit`); bmalloc through the system heap (`Malloc=1`) |
| `JSC::StructureMemoryManager` RELEASE_ASSERT | the structure heap wants 128 MB alignment (`MAP_ALIGNED(27)`) | honoured by the mmap wrapper |
| `IPC::createPlatformConnection` RELEASE_ASSERT | **`fcntl(F_GETFD/F_SETFD/F_DUPFD)` return EINVAL in a title** | `__wrap_fcntl`: FD flags report success (no exec in a title), F_DUPFD falls back to dup |
| SYSTEM_ILLEGAL_FUNCTION_CALL in `dup` | **libc.a's `syscalls.o` makes raw syscalls, which a title may not**, and once linked it overrides libkernel's exports (dup) | the title links `libc-title.a` (libc.a without syscalls.o); `apps/title/titlesys.c` provides what libkernel lacks (openat/fstatat/unlinkat on AT_FDCWD or absolute paths, eaccess, utimensat, chown family, closefrom, statfs/getfsstat ENOSYS, readlink/link/symlink) |
| `dup(ipc fd) errno 1` | **`dup()` is EPERM in a title** (dup2 too) | dup through SCM_RIGHTS to ourselves (`scmDup`, `--wrap=dup,dup2`) |
| SIGSEGV at address 0 in `currentExecutablePath` | `libScePosixForWebKit.so`'s stub exports `__wrap_readlink`, `__wrap_link`, `__wrap_symlink` (and `__wrap_mmap`); lld bound them instead of libps5compat's and they were NULL at run time | that stub is no longer linked |
| first page never finished loading (UI main loop blocked) | **`F_SETFL O_NONBLOCK` and FIONBIO on sockets are EACCES in a title**: GSocket's fds stayed blocking | emulated O_NONBLOCK: `__wrap_fcntl` remembers it, `--wrap=recvmsg,sendmsg,recv,send,recvfrom,sendto` add MSG_DONTWAIT, `--wrap=close` forgets it |
| nothing on stderr | `dup2(log, 2)` refused | `stderr = fdopen(log socket)` in `ps5browser.c` |

Probes in the title log at every start: `imports: N of M NULL` (every
function the title imports, from `apps/title/importcheck.c`, generated by
`package.sh`), and `probe:` lines (socketpair types, fcntl, FIONBIO,
MSG_DONTWAIT, dup, pipe2). Pipes behave (F_SETFL works on them); sockets
don't.

#### Process count and page tables

Each WebProcess payload costs ~300 of the system-wide 8192 CPU page tables
(`VM Stats ... page table CPU` in the kernel log: 4650 with nothing running,
5256 with the title + NetworkProcess + one WebProcess, 6155 after three
sites). WebKit kept every visited site's process (back/forward cache and
process cache) plus a prewarmed one, so 6 payloads were alive after three
navigations. `ps5browser.c` now sets `enable-page-cache` FALSE and the
DOCUMENT_BROWSER cache model, which should leave the current page's process
and one prewarmed process. **Built, not yet verified on the console** (the
console went offline during that run, below).

Children ended by aborting: when the UIProcess closed their IPC socket,
`exit()` ran static destructors and crashed in `WebCore::Font::~Font`
(ThreadGlobalData, WTFCrash), so every ended page process produced a crash
report for a ~600 MB payload. `apps/child/ps5wk-child.cpp` now `_exit`s
after WebKit returns. **Built, not yet verified.**

#### Packaging

`apps/title/package.sh` -> `toolchain/wpe-webkit/titles/PPSA99020` (200 MB:
`eboot.bin` 109 MB + `ps5wk-child.elf` 90 MB + param.json + libc.prx).
Title link specifics: `ps5-pie-webkit.ld` (WTF/JSC's page-aligned writable
sections moved into the data segment, or the converter fails with "LLVM
layout leaves no room for PS5 process parameters"); weak references that no
stub provides are defined as 0 (`_ZTH...` thread_local hooks, LSan hooks,
`__cxa_thread_atexit_impl`, `sceCommonDialogInitialize`, `__syscall`); the
SCE libraries the backend uses are linked without --as-needed;
ps5-opengl's `mkstemps` shim renamed (clashes with libc.a); libc.a without
`syscalls.o`, no `libScePosixForWebKit.so`; and the wraps:
malloc family + `aligned_alloc`/`memalign` (dheap), `mmap/munmap/madvise`,
`pthread_create/join/exit` (titlemem), `dlopen/dlsym` (dlshim, epoxy),
`fcntl/pipe2/dup/dup2`, `recvmsg/sendmsg/recv/send/recvfrom/sendto/close`
(ps5shim). Then a second link with `importcheck.o`.

Runtime needs on the console (already there): `/data/ps5webkit` runtime tree
from port/deps (certs, fonts, fonts.conf, xkb) and
`/data/ps5webkit/share/icu/icudt76l.dat` (uploaded by `test/run-shot.sh`).
Optional: `/data/ps5webkit/home-url.txt` (start page),
`/data/ps5webkit/child.env` (`WEBKIT_DEBUG=...` for the children; very
verbose).

#### Debug knobs (title)

- `/data/ps5webkit/debug-screenshot` (a file, any content) or
  `PS5WK_SCREENSHOT=1`: the first page frame, and 2 s and 10 s after each
  test URL finishes, are read back (`glReadPixels` of the composed back
  buffer before the swap, ~100 ms), halved and flipped to top-down, and
  written to `/data/ps5webkit/shots/NN-<host>-2s.ppm`; the log says
  `[wpe-ps5] SHOT <path> ... mean, % non-black`. `test/fetch-shots.sh`
  fetches them over FTP and writes `test/title-*.png`.
  `wpe_display_ps5_capture_screen()` is the API.
- `/data/ps5webkit/test-urls.txt`: one URL per line; loaded in turn instead
  of the home page, each with RESULT lines (load time, flexible memory) and
  screenshots, 12 s apart; 60 s timeout per URL.
- `[mem]` lines at main, display, web view, every load and every 60 s.
- Crash triage: `toolchain/wpe-webkit/titles/ps5browser-symbols.elf`
  (refreshed by `package.sh`), `llvm-addr2line -f -C -i -e ... <rip - 0x400000>`.
  `TITLE_LDFLAGS="--trace-symbol=x" package.sh` traces the title link.

Not yet checked: the ImeDialog struct layout (keyboard), the pad map in
use (only the pad opening is verified), spatial navigation with the D-pad,
fonts for Arabic/Hebrew/Devanagari (example.com's second paragraph shows
boxes: no Noto font for that script in `/data/ps5webkit/fonts`).

#### Console state at the end of the night run (03:01 GMT)

The console dropped off the network (no ping, no ports) a few seconds into
the run that tested the page-cache/cache-model change, ~3 h 50 min after the
owner's last pad input, so most likely rest mode. The kernel log's last
lines were a system app starting (NPXS40112) and
`mDBG: Waiting for debug subsystem`. Earlier in the night a system app
(NPXS40112, SceSpZeroConf) had crashed once while several aborted child
payloads were being crash-reported; whether that is related is unknown.

### Next steps

1. Verify the two unverified fixes above on the console (page-table count
   staying flat while browsing; children exiting without crash reports):
   `tools/launch.sh PPSA99020` with `test-urls.txt` and watch
   `page table CPU` in the kernel log and `(output closed after ...)` lines.
2. Owner on the TV: pad map, Options -> keyboard -> URL, X on links,
   scrolling, zoom (see the morning checklist in the session report).
3. Move the ps5shim fcntl/pipe2/dup wrappers into libps5compat (port/deps)
   if glib-only programs need them too; today every WebKit executable links
   ps5shim with `--wrap=fcntl,pipe2` and the title adds the rest.
4. Fonts for more scripts; process prewarming at start; TZ from the system
   settings.

Debug knobs: `/data/ps5webkit/child.env` (KEY=VALUE lines read by the
child, e.g. `WEBKIT_DEBUG=Network,Loading,Process`); `SHOT_DEBUG=...` for
`run-shot.sh` sets WEBKIT_DEBUG in the UIProcess, which the NetworkProcess
inherits through its creation parameters; `ENABLE_RELEASE_LOG=ON` prints
RELEASE_LOG to stderr. The kernel log (`logs/klog-*.log`) shows crashes with
registers; symbolize with
`llvm-addr2line -e toolchain/wpe-webkit/build/bin/ps5wk-child <rip - 0x200000000>`.
