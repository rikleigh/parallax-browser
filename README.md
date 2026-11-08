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
- **A profile for each PS5 user:** cookies, history, bookmarks, tabs and
  downloads are kept apart, and Settings shows how much storage each uses.
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
| D-pad left / right | Skip back / forward 30 s (hold to keep going) |
| Right stick left / right | Scrub along the seek bar |
| Triangle | Next projection |
| Square | Recentre on where you look |
| O | Cancel a seek you're choosing |
| O, Options or touchpad | Back to the TV (playback continues) |

Any button shows the player panel in the headset: the seek bar, the time
and the controls. It stays while the video is paused. A skip or scrub
moves a marker on the bar first; the video seeks when you stop.

## Requirements

- A PS5 on a jailbreakable firmware (developed on 13.60) with kstuff and
  etaHEN, elfldr on port 9021 and ftpsrv on port 2121. The jailbreak is
  tethered, so run it again after every reboot.
- A way to register homebrew titles from `/data/homebrew` (etaHEN or
  ShadowMount+). The title ID is `PPSA99020`.
- For View in VR: a PS VR2, paired and set up in the system settings.

## Installing

1. Download the zip from [Releases](https://github.com/rikleigh/parallax-browser/releases)
   and unpack it.
2. Copy its `data` folder onto the console's `/data` over FTP, merging
   folders. That puts the title in `/data/homebrew/PPSA99020` and its runtime
   files (fonts, certificates, Unicode data, the start page) in
   `/data/ps5webkit`.
3. Register `/data/homebrew/PPSA99020` with your homebrew tool. Parallax
   Browser then appears on the home screen.

To update, copy the new `data` folder over the old one. Each PS5 user's
bookmarks, history, settings and downloads live in `/data/ps5webkit/users`,
which the zip doesn't contain, so they're kept.

## Building

Building is involved: WebKit and about 30 libraries are cross-compiled for
the PS5 on macOS (Apple silicon). The steps, roughly:

1. `brew install llvm@18 socat cmake ninja wget nasm bison glib gperf`, then
   `. ./env.sh`. Put the console's and Mac's addresses in `env.local.sh`
   (git-ignored): `PS5_HOST` and `PS5WK_LOG_HOST`.
2. Clone the reference checkouts into `ref/` (git-ignored). The scripts
   expect exactly these folder names. Each fetch takes only the commit the
   build was tested with; `ps5-opengl` and `WebKit` must be at these
   commits, since the build scripts check them out by hash. A plain
   `git clone --depth 1` would get the latest commit instead, and a full
   WebKit clone is very large.

   ```sh
   refclone() {  # folder, URL, commit
       git init -q "ref/$1" &&
       git -C "ref/$1" fetch -q --depth 1 "$2" "$3" &&
       git -C "ref/$1" checkout -q FETCH_HEAD
   }
   refclone ps5-payload-sdk https://github.com/ps5-payload-dev/sdk a7685ce8e97ce1f8a4f38253a41d54241c9124f1
   refclone PS5VR https://github.com/kavera001-dev/PS5VR 110f9a795eec413d174571f65ce2fe4233c49424
   refclone ps5-opengl https://github.com/mihawk-99/ps5-opengl ec06d8c2da0359e274e409046f0a9586965e4010
   refclone pacbrew-repo https://github.com/ps5-payload-dev/pacbrew-repo 722187f0ee246c0d7d57401c8a97de1c7f4e2ff0
   refclone WebKit https://github.com/WebKit/WebKit 15486a017817cad93f25fdffeb013278866970fc
   ```

   Then build the SDK into `toolchain/ps5-payload-sdk` (see
   [Setup](#setup-for-the-spikes)).
3. `spikes/04-jsc/build.sh` builds libc++ 19 and ICU, and
   `spikes/02-gl-egl/ps5-opengl/build.sh gles` builds ps5-opengl with
   OpenGL ES, and `build.sh gles-noprof` then installs the build the browser
   links (the same with its per-draw profiling compiled out). Then
   `port/deps/build.sh all` builds the libraries into
   `toolchain/wpe-sysroot` ([port/deps/README.md](port/deps/README.md)).
4. `port/webkit/build.sh` applies the patches in `port/webkit/patches/` to
   WebKit and builds it. `port/webkit/apps/title/package.sh` then writes the
   title folder ([port/webkit/README.md](port/webkit/README.md#build)).
5. `tools/upload-title.sh toolchain/wpe-webkit/titles/PPSA99020` copies it
   to the console. `tools/launch.sh` registers, starts and stops it. The
   runtime files need copying to `/data/ps5webkit` once: take the
   `data/ps5webkit` folder from a zip made by `tools/make-release.sh`.

`tools/logserver.sh` collects the browser's log on the Mac.
`tools/make-release.sh <version>` builds the release zip: the title with no
log address built in, and the runtime files.

## Repository layout

| Path | What it is |
| --- | --- |
| `port/webkit/` | The browser: the WebKit patches, the PS5 platform backend (`wpe-ps5/`), the title app (`apps/title/`), the ad blocker, tests and the development notes ([README](port/webkit/README.md)) |
| `port/deps/` | Cross-builds of the libraries WebKit needs, plus their patches |
| `spikes/` | The feasibility experiments the project started from |
| `tools/` | Upload, launch, logging and conversion tools |
| [FEASIBILITY.md](FEASIBILITY.md) | The original feasibility study and risk register |

## Setup for the spikes

With `ref/ps5-payload-sdk` cloned as in step 2 above:

```sh
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
