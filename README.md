# Parallax Browser

A homebrew web browser for a jailbroken PS5, built on WPE WebKit, that can
play the video on a page in the **PS VR2** headset.

<p align="center"><img src="port/webkit/runtime/parallax-logo.png" width="160" alt="Parallax Browser logo"></p>

> **Status: experimental.** Tested on one console, firmware 13.60, with a
> DualSense and a PS VR2. Expect rough edges. WebGL and WebXR aren't
> supported yet.

## Features

- **WPE WebKit** with GPU compositing, async scrolling, and JavaScriptCore
  with the JIT.
- **Hardware video decoding** through the console's decoder: H.264, HEVC
  and VP9, up to 8K, including HLS streams.
- **View in VR:** press the touchpad on a page that's playing a video to watch
  it in the PS VR2: 180° or 360°, side-by-side or top/bottom 3D, or fisheye.
  The layout is detected from the page and the picture, and the video is
  centred where you look.
- **Tabs** (recent tabs stay loaded), **bookmarks**, **history** and a start page.
- **Downloads** with pause and resume, which survive a browser restart, and
  SHA-256 checksums.
- **Ad blocking** with the uBlock Origin Lite rule lists: network rules,
  cosmetic filters and scriptlets, with no site-specific code. It can be
  switched off per site.
- **Link server:** open `http://<ps5-ip>:9040/` on a phone or computer, or
  scan the QR code in the menu, to send links to the PS5, change settings
  and fetch downloads.
- The system on-screen keyboard for text fields.

## Controls (DualSense)

| Button | On a page |
| --- | --- |
| Left stick / right stick | Move the pointer / scroll |
| X | Click (Enter when the pointer is hidden) |
| O | Back |
| D-pad | Move between links |
| L1 / R1 | Previous / next form field |
| L2 / R2 | Zoom out / in |
| Triangle | Keyboard |
| Square | Reload |
| Options | Menu: address bar, tabs, bookmarks, history, downloads |
| R3 / L3 | Settings / home page |
| Touchpad button | View in VR |

| Button | In VR |
| --- | --- |
| X | Play / pause |
| D-pad left / right | Seek -10 s / +10 s |
| Triangle | Next projection |
| Square | Recentre on where you look |
| O, Options or touchpad | Back to the TV (playback continues) |

## Requirements

- A PS5 on a jailbreakable firmware (developed on 13.60) with kstuff and
  etaHEN, elfldr on port 9021 and ftpsrv on port 2121. The jailbreak is
  tethered, so run it again after every reboot.
- A way to register homebrew titles from `/data/homebrew` (etaHEN or
  ShadowMount+). The title ID is `PPSA99020`.
- For View in VR: a PS VR2, paired and set up in the system settings.

Prebuilt releases aren't available yet. To build from source, see below.

## Building

Building is involved: WebKit and about 30 libraries are cross-compiled for
the PS5 on macOS (Apple silicon). The steps, roughly:

1. `brew install llvm@18 socat cmake ninja wget nasm bison glib gperf`, then
   `. ./env.sh`. Put the console's and Mac's addresses in `env.local.sh`
   (git-ignored): `PS5_HOST` and `PS5WK_LOG_HOST`.
2. Clone the reference checkouts into `ref/` (git-ignored):
   [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk),
   [PS5VR](https://github.com/kavera001-dev/PS5VR),
   [ps5-opengl](https://github.com/mihawk-99/ps5-opengl) at `ec06d8c`,
   [pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo) and
   [WebKit](https://github.com/WebKit/WebKit) at commit `15486a0178`. Build
   the SDK into `toolchain/ps5-payload-sdk` (see [Setup](#setup-for-the-spikes)).
3. `spikes/04-jsc/build.sh` builds libc++ 19 and ICU, and
   `spikes/02-gl-egl/ps5-opengl/build.sh gles` builds ps5-opengl with
   OpenGL ES. Then `port/deps/build.sh all` builds the libraries into
   `toolchain/wpe-sysroot` ([port/deps/README.md](port/deps/README.md)).
4. `port/webkit/build.sh` applies the patches in `port/webkit/patches/` to
   WebKit and builds it. `port/webkit/apps/title/package.sh` then writes the
   title folder ([port/webkit/README.md](port/webkit/README.md#build)).
5. `tools/upload-title.sh toolchain/wpe-webkit/titles/PPSA99020` copies it
   to the console. `tools/launch.sh` registers, starts and stops it.

`tools/logserver.sh` collects the browser's log on the Mac.

## Repository layout

| Path | What it is |
| --- | --- |
| `port/webkit/` | The browser: the WebKit patches, the PS5 platform backend (`wpe-ps5/`), the title app (`apps/title/`), the ad blocker, tests and the development notes ([README](port/webkit/README.md)) |
| `port/deps/` | Cross-builds of the libraries WebKit needs, plus their patches |
| `spikes/` | The feasibility experiments the project started from |
| `tools/` | Upload, launch, logging and conversion tools |
| [FEASIBILITY.md](FEASIBILITY.md) | The original feasibility study and risk register |

## Setup for the spikes

```sh
git clone --depth 1 https://github.com/ps5-payload-dev/sdk ref/ps5-payload-sdk
make -C ref/ps5-payload-sdk DESTDIR=$PWD/toolchain/ps5-payload-sdk install
TMPDIR=$PWD/build ref/ps5-payload-sdk/libcxx.sh    # libc++ for C++ code
```

Payload spikes run with `make -C spikes/NN-name send`. Their output comes
back over the elfldr socket into `logs/`.

## Legal

This is an independent homebrew project, not affiliated with or endorsed
by Sony Interactive Entertainment. "PlayStation", "PS5" and "PS VR2" are
trademarks of Sony Interactive Entertainment Inc. The repository contains no
Sony system files, firmware or keys. Code that uses system libraries
resolves them on the console at run time.

## License

GPL-3.0-or-later ([LICENSE](LICENSE)). The WebKit patches are under
WebKit's own licences (LGPL-2.1 / BSD). Third-party components are listed
in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
