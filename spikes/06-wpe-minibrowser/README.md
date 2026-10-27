# Spike 6: WPE MiniBrowser on the TV

**Goal:** WPE WebKit (`PORT=WPE`, `ENABLE_WEBGL=ON`, `ENABLE_WEBXR=ON`, JIT
off) built with ps5-payload-sdk, with a custom WPE Platform backend. It loads
pages on the TV and navigates with the DualSense.

**Status:** design only. Spikes 1–4 decide which variant gets built. Nothing
here should be started before the Spike 3 gate passes.

## Build shape

- Toolchain: `prospero-cmake` (ps5-payload-sdk), static libc++. Start from
  the Spike 4 JSC configuration and patches (OS detection, ICU, malloc).
- WebKit options: `-DPORT=WPE -DENABLE_WPE_PLATFORM=ON -DENABLE_WPE_LEGACY_API=OFF
  -DENABLE_JIT=OFF -DENABLE_WEBGL=ON -DENABLE_WEBXR=ON -DUSE_OPENXR=ON
  -DUSE_GBM=OFF -DUSE_LIBDRM=OFF -DENABLE_GAMEPAD=ON -DUSE_SKIA=ON`. Turn off
  everything else we can (media/GStreamer, WebRTC, speech, geolocation,
  bubblewrap, introspection, docs).
- **Dependencies:** this is the biggest effort item in the port. The
  pacbrew repo already has zlib, zstd, libpng, libjpeg-turbo, libwebp,
  freetype, harfbuzz, fontconfig, expat, libxml2, sqlite, libpsl, curl,
  openssl/libressl, libiconv, libdeflate and mesa. **Missing:** glib (+gio),
  libsoup3, ICU, libepoxy, brotli/woff2, lcms2, libavif (optional),
  libtasn1/gnutls or glib-networking with OpenSSL, nghttp2, and Skia (built
  in-tree by WebKit). glib and libsoup are the hard ones: GIO's process and
  socket assumptions, glib-networking's TLS backend, and threads.
- GPU stack: either **ps5-opengl** (title-only, desktop GL, which needs our
  GLES patch or ANGLE-on-GL) or **pacbrew Mesa** (RADV + zink + EGL
  videoout, GLES built in, works in payloads). Spike 2 picks the one to use.
  WebKit reaches EGL/GLES through libepoxy (an EGL-only build) and ANGLE for
  WebGL.

## The PS5 WPE Platform backend (`WPEPlatform/wpe/ps5/`)

Modelled on `WPEPlatform/wpe/headless/` (~550 lines), which is the closest
upstream backend:

| Class | PS5 implementation |
| --- | --- |
| `WPEDisplayPS5` | owns the EGL display and the VideoOut scanout (ps5-opengl window surface, or the Mesa videoout platform), screen size/refresh (1080p/4K, 60/120 Hz), and the buffer formats it can import |
| `WPEViewPS5` / `WPEToplevelPS5` | one fullscreen view. `render_buffer`: import the WebProcess's buffer and draw it to the scanout |
| Buffers | **v1:** `WPEBufferSHM` (stock). The WebProcess renders with GL and reads back into shared memory (`AcceleratedSurface::RenderTargetSHMImage`), and the UIProcess uploads and draws. It costs one 1080p readback per frame, about 8 MB. **v2:** a PS5 buffer type over shared direct memory (if Spike 1 `dmem-share` passes), or single-process rendering |
| Input | `libScePad` → `WPEEvent` keyboard/pointer emulation (D-pad focus navigation, left stick as pointer, triggers scroll), Gamepad API via `WPEGamepadManager` |
| Text input | `WPEInputMethodContext` backed by `libSceImeDialog` (stub already in the SDK) |
| Clipboard, cursor | no-op / drawn pointer sprite |

## Process launching (`ProcessProviderPS5`)

WebKit asks for a WebProcess and a NetworkProcess, each with one end of a
socketpair:

- If Spike 1's `elfldr-spawn` passes (in title context too), the provider
  sends the child ELF (WPEWebProcess / WPENetworkProcess built as payloads,
  or one multi-call binary) to elfldr. A rendezvous Unix socket hands over
  the socketpair end with `SCM_RIGHTS`, and a tiny `main` builds the argv
  WebKit expects (`<identifier> <fd>`). Sony's port does the same thing with
  its private launcher (`ProcessLauncherPlayStation.cpp`).
- If only `fork` works, use a fork-server (fork before threads, then
  `exec`-less `WebProcessMain` in the child).
- If neither works, **in-process mode**: run `WebProcessMain` and
  `NetworkProcessMain` on threads in the UIProcess with an in-process
  `IPC::Connection` over a socketpair. Modern WebKit doesn't support this.
  WebKit keeps per-process singletons, and the GPU/JS heaps would need to
  coexist. Budget it as a large patch set (FEASIBILITY R1).

## Exit criteria

`https://example.com` and a JS-heavy page render on the TV. Pad navigation
and the on-screen keyboard work. Memory and frame times are logged for 10
minutes of browsing.
