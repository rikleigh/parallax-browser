# WPE WebKit for the PS5 (Milestones 1 and 2)

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

Milestone 2 step 1 (2026-10-28): `<video>`/`<audio>` and MSE on GStreamer,
software decoders in the WebProcess, audio played by the title (see
*Media* below).

No WebGL, WebXR, GPU process, Web Audio, WebRTC, speech, geolocation,
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
| 0013-ps5-payload-exit-and-cache-without-hard-links | `AuxiliaryProcess::didClose` exits at once (`_exit`) in PS5 payloads: the GLib teardown after the run loop stopped aborted (SIGABRT) in every swapped-out WebProcess. The network cache copies a blob when `link()` fails (EOPNOTSUPP on `/data`) instead of leaving the record without its body (`Failed to create hard link`) |
| 0014-ps5-exit-on-ui-close-and-no-forced-compositing | payloads `_exit(0)` on the IPC thread as soon as the connection to the UI process closes (end of stream or HUP; 0013 only covered `didClose` on the main thread, and swapped-out processes still aborted); `RenderLayerCompositor::enableCompositingMode` is a no-op when accelerated compositing is off: `document.startViewTransition()` (YouTube watch pages) forced compositing on and `NonCompositedFrameRenderer::graphicsLayerFactory()` RELEASE_ASSERTed (SIGABRT, every watch page) |

| 0015-media-build-fixes-video-without-web-audio | ENABLE_VIDEO with Web Audio off: `platform/audio/gstreamer` on the include path (AudioDecoder.cpp includes it under USE(GSTREAMER)); no WEBCORE_EXPORT on a member of the exported `MediaSourcePrivate` (dllexport inside dllexport is an error with this toolchain); libpas uses libc's `backtrace()` on FreeBSD (its own stub clashed with libc.a's once GStreamer pulled `backtrace.o` in) |
| 0016-ps5-software-video-paint-and-media-policy | PS5 only (`PS5_PAYLOAD_SDK`): AV1 not advertised (no hardware AV1; `PS5WK_MEDIA_AV1=1` advertises it), `PS5WK_MEDIA_VP9=0` hides VP9; avdec `max-threads` 4 (`PS5WK_DECODER_THREADS`), `max-errors` -1 (a reference-less frame after a flush doesn't end playback); playsink's `videoconvert` on 4 threads; video painting: the frame is scaled to the device-pixel size with GstVideoConverter (4 threads, linear) and wrapped zero-copy in an SkImage cached for the repaints of the same frame (Skia's raster bilinear scaling of 1080p cost ~30 ms per frame, ImageGStreamer copied the frame on every paint, and `convertSampleToImage` ran a conversion pipeline even when the format already matched); non-composited rendering updates longer than 6 ms are followed by an equal idle gap (NonCompositedFrameRenderer), so JavaScript timers and MSE appends still run while video plays |

0011 also carries: WTF FreeBSD sources, VIDEO=OFF build fixes (media sources,
TextExtraction, AudioEncoder/PlatformRawAudioData), Skia `malloc_np.h`, DNS
includes, WKAPICast PlayStation guard, `EGL_DEFAULT_DISPLAY` as an int on PS5,
xdgmime globals renamed (clash with GIO's copy in a static link), CPU-only
WPE preferences on PS5 (no hardware acceleration, no compositing), and the
128 MB JSC structure heap on PS5.

The memmem shim and `<sys/sysinfo.h>` from Spike 4 are in `shim/`
(force-included `ps5shim.h`, `libps5shim.a`).

## Media (Milestone 2, step 1)

`<video>`, `<audio>` and MSE play on the static GStreamer from port/deps
(`USE_GSTREAMER_FULL=ON`, `USE_GSTREAMER_GL=OFF`, `ENABLE_VIDEO=ON`,
`ENABLE_MEDIA_SOURCE=ON`, Web Audio off). `apps/common/media-stubs.h` (the
Milestone 1 placeholder media interfaces) is gone.

### Decision: decode in the WebProcess, play audio in the title

- **Video: software decoding in the WebProcess payload** (avdec_h264,
  avdec_vp9, avdec_h265 from FFmpeg 7.1.1; dav1d for AV1, not advertised),
  painted through WebKit's normal non-composited video path into the page
  bitmap. The hardware decoder only works in a title (Spike 9) and its frames
  would have to reach the page by hole punching, which needs compositing in
  the title; that is step 2. Software decoding was hours of work, not days,
  and covers ≤1080p (measured below).
- **Audio: through the title.** An elfldr payload *can* open sceAudioOut
  (`test/audio/audioprobe.c`: main port of the system user 255, S16 48 kHz,
  2000 ms of audio consumed in 2013 ms; other user ids 0x80260011), and the
  first build played from the WebProcess (`ps5audiosink`, direct mode). But
  **the audio daemon never releases what payload processes took**: after
  eight payload processes had used sceAudioOut (2026-10-28: the probe, the
  gst smoke test and six WebProcesses, some closing their port, some not),
  `sceAudioOutInit` returned 0x80260002 in the ninth, and then blocked
  forever (IPC wait `IpmWSv0`) in every new process. A title launch an hour
  later hung in the launcher (`IpmWEf0`) and the console rebooted (11:44).
  **Never use sceAudioOut from a payload.**
  A title is an application the system cleans up after: 12 launch/SIGKILL
  cycles of PPSA99020 in a row, each opening a port and leaving it open when
  killed (`/data/ps5webkit/audio-selftest`), all reopened it (handle
  0x20000000, Init 0).
- So the WebProcess's `ps5audiosink` (port/deps/gst/gstps5audiosink.c,
  autoaudiosink picks it at rank PRIMARY) ships S16 48 kHz PCM over a unix
  socket, `/system_tmp/ps5wk/audio-<UIProcess pid>.sock`
  (`PS5WK_AUDIO_SOCKET`, set by ps5wk-child from its launch path), to the
  audio server in the UIProcess (`apps/common/ps5audioserver.c`): one
  connection and one sceAudioOut port per stream, 256-frame grains, a
  "frames played" count sent back per grain for the sink's `delay()` (A/V
  sync), a flush/seek closes the connection (queued audio dropped). Blocking
  in sceAudioOutOutput paces the WebProcess's pipeline clock through the
  socket (about 4 grains, 21 ms, buffered on each side). The title closes
  its ports when main returns (`ps5wk_audio_server_stop`); a SIGKILL leaves
  that to the system (checked above). ps5wk-shot runs the same server with a
  paced null output, so harness runs never touch sceAudioOut.
- Long-term (FEASIBILITY §1a) the hardware decoder element lives in the
  title with hole-punch video; audio already lives there.

### Knobs

| | |
| --- | --- |
| `/data/ps5webkit/audio-null` | title: consume audio silently (no sceAudioOut) |
| `/data/ps5webkit/audio-silent` | title: real sceAudioOut ports at volume 0 (log-only runs) |
| `/data/ps5webkit/audio-selftest` | title: open a port at start, 0.1 s of silence, leave it open (`RESULT ps5browser audio-selftest`) |
| `child.env`: `PS5WK_DECODER_THREADS`, `PS5WK_MEDIA_VP9=0`, `PS5WK_MEDIA_AV1=1`, `GST_DEBUG=...` | WebProcess decoding, codec advertising, GStreamer logs |
| `child.env`: `PS5WK_PROFILE=1` | sampling profiler of the WebProcess main thread (`apps/common/profiler.c`, 2 ms SIGPROF; `test/prof.sh` fetches and symbolizes) |
| `child.env`: `PS5WK_CPU_LOG=0` | turn off the `[cpu] webN: x cores` lines (every 5 s while above 0.3 cores) |
| `WEBKIT_GST_VIDEO_DECODING_LIMIT` | set by the child to 1920x1080@60: MSE/canPlayType refuse more, so YouTube stays ≤1080p |

The children also set `WPE_SHELL_MEDIA_DISK_CACHE_PATH=/data/ps5webkit/home/cache/media`
(progressive downloads buffer on disk; WebKit's default `/var/tmp` doesn't
exist for payloads: "Could not create temp file").

### Test media and pages

`test/media/make-media.sh [--upload]` makes the clips with FFmpeg on the Mac
(test patterns with a running clock, a white flash and a 1760 Hz beep at the
start of every second: A/V sync by eye and ear; nothing third-party is
committed) and uploads them with `video.html` (file:///data/ps5webkit/video.html,
tile "Video test" on the start page; `?src=<url>` plays a URL, the stats line
and `VIDEOSTATS` console lines give frames, drops and fps).
`test/media/yt-stats.js` (ps5wk-shot `js=`) dumps the first `<video>`,
YouTube's stats for nerds and `MediaSource.isTypeSupported` answers;
`test/media/yt-consent.js` (ps5wk-shot `onload=`, new) answers YouTube's
cookie dialog, which pauses the video in a fresh session, and logs
`YTSTATS` every 3 s.

### Measured (ps5wk-shot, 1920x1080 viewport, video element 1280x720, 2026-10-28)

| clip | fps shown | dropped | WebProcess CPU |
| --- | --- | --- | --- |
| local 720p30 H.264 + AAC (1:1, before the paint fixes) | 28.5 | 0 | 1.44 cores |
| local 1080p30 H.264 + AAC | 29.9 | 0 | 0.91–1.05 cores |
| local 1080p60 H.264 + AAC | 59.9 | 0 | 1.59–1.95 cores |
| local 1080p30 VP9 + Opus | 29.9 | 0 | 1.03 cores |
| Sintel trailer 720p24 H.264 + AAC over HTTPS (moov at the end, range requests) | 24 | 0 | 0.55 cores |
| YouTube "Never Gonna Give You Up": MSE VP9 720p25 (itag 247) + AAC (140) | 25 | 0 | 0.87 cores |
| YouTube Big Buck Bunny 60 fps: VP9 720p60 (302) chosen | plays, JS starved before the throttle | | 1.2–1.7 cores |

Before the paint fixes 1080p30 managed ~19 fps and starved the page's
JavaScript completely (events and timers ran only after the video ended):
the profile showed 81 % of the main thread in Skia's `bilerp_clamp_8888`.
YouTube asks `isTypeSupported` and gets: avc1 yes, vp9/vp09 yes, av01 no,
VP9 at 3840x2160 no, at 1920x1080@60 yes; mp4a yes; `audio/webm; opus` no
(not needed so far: YouTube sent AAC).

**In the title (log-only run, 2026-10-28 11:58, audio-silent):** the local
1080p30 MP4 played with its audio going through the title's sceAudioOut
(32.8 s of audio in 34.3 s, handle 0x20000000 at volume 0) at **20–23 fps**
(0 dropped, WebProcess 0.46 cores; the title's vsync-paced frame
completion plus the rendering throttle is the suspect for the gap to 30), and
the title's flexible memory was 299 MB free (eboot 129 MB with GStreamer,
was 109 MB). The YouTube URL after it did not report "load finished" within
60 s, and the launcher's kill then got no reply: open.

Still open: the TV test (picture and sound judged by the owner), title
fps, YouTube 60 fps under its overlays (every video frame repaints the
player's gradient/blur layers on the CPU; hole punching removes that), Opus
in WebM for MSE, the 24 s without video frames once on YouTube after the
consent dialog (audio and clock ran on).

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
| `PS5Sce.h` | weak declarations of libScePad and libSceUserService (no SDK headers); the libSceImeDialog function table that the title fills (`apps/title/titleime.c`) |

Pad map: D-pad = arrow keys (spatial navigation between links in the
browser); X = click at the pointer when it is shown (it shows for 8 s after
the left stick moves), otherwise Enter; O = Back; left stick = pointer;
right stick = scroll under the pointer; Options = URL/search prompt;
Triangle = keyboard for the focused field (it also opens by itself when X
focuses a field); Square = reload; L1/R1 = previous/next field; L2/R2 = zoom.

GL goes through libepoxy; in the title, `--wrap=dlopen/dlsym` and
`apps/title/dlshim.c` resolve epoxy's lookups to the statically linked
ps5-opengl (EGL 1.4 from its symbol table, GL from `eglGetProcAddress`).

## System keyboard (libSceImeDialog)

The system loads libScePad and libSceUserService for every title, but not
libSceImeDialog.sprx: listed as needed in eboot.bin, its imports stayed NULL.
`apps/title/titleime.c` loads it at start the way Spike 9 loads its decoder:
`sceSysmoduleLoadModule(0x96)` (the ImeDialog id in 13.60's libSceSysmodule
table, as on the PS4), `sceKernelLoadStartModule` of
`/<sandbox word>/common/lib/libSceImeDialog.sprx` for the handle, then
`sceKernelDlsym` (refused, as for every system library so far) and the
13.60 export offsets in `apps/title/ime_exports_1360.h` (anchor string
`checkImeDialogInitParam` checked first). libSceCommonDialog is bound the
same way and `sceCommonDialogInitialize` returns 0. Log on the console:

    ime: sysmodule 0x96 -> 0x00000000; load /<word>/common/lib/libSceImeDialog.sprx -> 0xe4
    ime: libSceImeDialog.sprx: 5 of 5 bound (0 by name, base 800e68000)
    ime: libSceCommonDialog.sprx: 1 of 1 bound (0 by name, base 800e48000)

`sceImeDialogInit` takes the PS4 `SceImeDialogParam` (96 bytes; the
disassembly copies 0x60 bytes and reads `type` at +4 and `option` at +0x20).

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

- **YouTube stayed on its loading skeleton because of `ENABLE_VIDEO=OFF`.**
  With page console messages on (`enable-write-console-messages-to-stdout`,
  `[webN] CONSOLE ...` lines in the title log and the shot output), the
  main bundle (kevlar_base) threw `ReferenceError: Can't find variable:
  HTMLVideoElement`: TypeScript decorator metadata names the class while
  components are defined, so the whole module aborted. A document-start user
  script (`apps/common/media-stubs.h`) defines placeholder
  `HTMLMediaElement`/`HTMLVideoElement`/`HTMLAudioElement` interfaces; with
  it the home page renders its app (cookie-consent dialog, guide, masthead)
  in `ps5wk-shot`. `<video>` stays `HTMLUnknownElement`, so playback is
  still Milestone 2. A watch page (`/watch?v=...`) loaded (title "Me at the
  zoo - YouTube") but its WebProcess died 1.7 s later (termination reason 0,
  no output); the console powered off shortly afterwards.
  `run-shot.sh` takes `SHOT_ARGS="settle=<s> js=<file on the console>"` to
  watch a page longer and dump page state.

- **file:// pages showed as source**: no shared-mime-info on the console,
  so GIO and WebKit's xdgmime had no extension map. A hand-written
  `share/mime/globs2` (port/deps `mime` step) plus `XDG_DATA_DIRS=/data/ps5webkit/share`
  in the children, the title and the shot fixes it:
  `file:///data/ps5webkit/start.html` loads as `text/html` in `ps5wk-shot`.
  The title's home page is that file again (`PS5WK_HOME_URL`; the console's
  `home-url.txt` now names it too).

- **Crash guard in the payloads** (`apps/common/crashguard.c`, ps5wk-child
  and ps5wk-shot): SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT/SIGTRAP/SIGSYS
  handlers on an alternate signal stack (every thread gets one through
  `--wrap=pthread_create`) print one report and `_exit(70)`, so a crashing
  payload no longer reaches the system crash reporter (suspected in two
  console power-offs after WebProcess crashes):

      [crash web5 pid 145] signal 11 (SIGSEGV) code 1 addr 0x... pc 0x... (elf +0x...) sp 0x... thread 0x...
      [crash web5 pid 145] stack: +0x... +0x...   (stack words after a call instruction, innermost first)

  `llvm-addr2line -f -C -e toolchain/wpe-webkit/build/bin/ps5wk-child 0x<elf offset>`.
  Do not read the payload's own ELF header: elfldr maps the first PT_LOAD
  (file offset 0x4000) at the image base, so `__ehdr_start` is the base but
  no header is there (the first build did, and faulted before its handlers
  were installed). The PS5 kernel's ucontext is not the SDK header's:
  FreeBSD's mcontext sits at +0x40 instead of +0x10 (`test/sigctx/sigctx.c`:
  rip +0xe0, rsp +0xf8, rbp +0x88); with the header's layout the first
  reports had `sp 0x0` and a wrong pc. `abort()` (all WebKit RELEASE_ASSERTs
  here) shows as SIGABRT code 0, pc in libkernel's raise, `addr` = pc + 2;
  the stack scan then gives the WebKit frames.
- **Payload threads had 64 KiB stacks.** The payload libc's default thread
  stack is 64 KiB (`test/stack/stack.c`: default attr 64 KiB, 16 KiB guard),
  and WTF leaves most thread types to the platform default, Web Workers'
  JavaScript threads included (JSC alone reserves 192 KiB of stack zones;
  only PLATFORM(PLAYSTATION) asks for 512 KiB). The crash guard's
  `pthread_create` wrapper now gives every thread without its own stack at
  least 2 MiB (FreeBSD's amd64 default). Prime suspect for the silent
  WebProcess death on YouTube watch pages (not re-tested yet). The title's
  own wrapper (`titlemem.c`) still uses the requested size.

- **YouTube in the title, TV test 2026-10-28**: the page failed with
  `Error receiving data: Connection reset by peer` three times, while the
  swapped-out start-page WebProcess aborted (SIGABRT, caught by the crash
  guard) and the NetworkProcess logged `Failed to create hard link` for
  every cache write. `ps5wk-shot` with the title's own session directories
  (`SHOT_ARGS="data=/data/ps5webkit/browser/data cache=/data/ps5webkit/browser/cache"`)
  loaded YouTube fine, so the cache contents and cookies (in memory only) were
  not the cause on their own. With patch 0013 (clean exit, cache copies)
  a log-only title run of start.html -> YouTube -> example.com -> YouTube
  loaded all four with no reset, no abort and no hard-link errors. The reset
  was not reproduced, so its cause stays open (most likely the aborting
  process or the record without a body). `new Audio()` is stubbed as well
  (`ReferenceError: Can't find variable: Audio` from voice search).

- **YouTube watch pages crashed every WebProcess** 4–7 s after loading
  (TV test 2026-10-28, a reload loop). Symbolized crash guard stack:
  `jsDocumentPrototypeFunction_startViewTransition` ->
  `Document::setActiveViewTransition` -> `RenderLayerCompositor::enableCompositingMode`
  -> `FrameRenderer::graphicsLayerFactory()` RELEASE_ASSERT (we paint without
  compositing). Fixed in 0014; `ps5wk-shot` keeps the watch page up for 30 s
  showing the player's "Your browser can't play this video". The title
  shows an error page instead of reloading after a second WebProcess crash
  on the same address within 30 s.
  Still open: `Error receiving data: Bad file descriptor` (and the earlier
  `Connection reset by peer`) on YouTube's main document in the title, both
  at the moment a swapped-out WebProcess died and the NetworkProcess saw its
  connection close; not reproduced in the harness or log-only title runs
  since those processes exit cleanly.

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
