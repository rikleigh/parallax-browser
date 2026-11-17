# Parallax Browser

A homebrew web browser for a jailbroken PS5, built on WPE WebKit. It can
play the video on a page in the PS VR2 headset.

<p align="center"><img src="port/webkit/runtime/parallax-logo.png" width="160" alt="Parallax Browser logo"></p>

> **Experimental.** It has been tested on one console, on firmware 13.60,
> with a DualSense and a PS VR2. WebXR isn't supported yet.

## Features

- **View in VR.** Press R3, the right stick, on a page that's playing a
  video to watch it in the PS VR2. It handles 180° and 360° video,
  side-by-side and top-and-bottom 3D, and fisheye. The browser detects the
  layout from the page and the picture, and centres the video where you
  look.
- WPE WebKit with GPU compositing and async scrolling. JavaScriptCore and
  WebAssembly run with the JIT.
- WebGL 1 and 2 on the PS5's GPU, including video drawn into WebGL.
- Hardware video decoding on the console's decoder, for H.264, HEVC and
  VP9 up to 8K, including HLS streams.
- Audio in AAC, MP3, Opus, Vorbis in Ogg and WebM, and FLAC, with the Web
  Audio API.
- Gamepad mode for browser games. The page gets the controller through the
  Gamepad API, with rumble, or you map keys and a mouse to the buttons for
  each site. Hold △ to turn it on, Options to leave it.
- Tabs, bookmarks, history and a start page. Recent tabs stay loaded.
- Private browsing keeps no history, and drops cookies and site data when
  you leave it.
- A profile for each PS5 user. Each user has their own cookies, history,
  bookmarks, tabs and downloads, and Settings shows how much storage each
  one uses.
- Downloads with pause, resume and SHA-256 checksums. They survive a
  browser restart.
- Ad blocking with the uBlock Origin Lite rule lists, which include network
  rules, cosmetic filters and scriptlets. It has no code for particular
  sites, and you can turn it off for each site.
- A link server. Open `http://<ps5-ip>:9040/` on a phone or computer, or
  scan the QR code on the start page or in Settings, to send links to the
  PS5, change settings and fetch downloads. See [Security](#security).
- Each page runs in its own process, so a page that crashes closes only
  itself. "Pages in the browser process (experimental)" in Settings runs
  pages inside the browser instead:
  - Pro: pages get more of the PS5's CPU, and games and heavy pages run up
    to twice as fast.
  - Con: busy pages make the menu, the pointer and clicks pause now and
    then. Scrolling carries on through the pauses.
  - Con: a page that crashes closes the whole browser.
  - Con: pages are not kept apart from the browser.
- Settings, at `parallax://settings`, for Safer browsing, the home page,
  the search engine, video, memory, ad blocking, the link server and
  clearing data. Google is the default search engine. Pressing □ on the
  menu's Home button also makes the current page the home page.
- The system on-screen keyboard for text fields.
- The touchpad works as a mouse.

## Controls (DualSense)

| Button | On a page |
| --- | --- |
| Left stick | Move the pointer |
| Right stick | Scroll |
| Touchpad | Works like a laptop's. One finger moves the pointer, two fingers scroll, and a press clicks. Press and hold to drag. |
| X | Click, or Enter when the pointer is hidden |
| O | Back |
| D-pad | Move between links |
| L1 | Previous form field |
| R1 | Next form field |
| L2 | Zoom out |
| R2 | Zoom in |
| Triangle | Keyboard |
| Square | Reload |
| Options | The menu, with the address field, tabs, bookmarks, history and downloads |
| R3 | View in VR |
| L3 | Home page |
| L3 and R3 together | Performance overlay |

To open Settings, press Options, then choose ⚙.

The bar (Back, Forward, the address, Bookmark, Settings and the rest) stays
above the page. Move the pointer up onto a button and press X to use it
without opening the menu. To hide the bar, choose the full screen button (the
four corners). The browser remembers that choice. It also hides the bar by
itself in gamepad mode and while a video is in full screen.

| Button | In VR |
| --- | --- |
| X | Play or pause |
| D-pad left | Skip back 30 seconds. Hold to keep going. |
| D-pad right | Skip forward 30 seconds. Hold to keep going. |
| Right stick, left or right | Scrub along the seek bar |
| Triangle | Next projection |
| Square | Recentre on where you look |
| O | Cancel the seek you're choosing |
| O, Options or R3 | Back to the TV. The video keeps playing. |

Any button shows the player panel in the headset, with the seek bar, the
time and the controls. The panel stays while the video is paused. A skip
or scrub moves a marker on the bar first, and the video seeks when you
stop.

## Requirements

- A PS5 on a jailbreakable firmware, with kstuff and etaHEN, elfldr on port
  9021 and ftpsrv on port 2121. Development used firmware 13.60. The
  jailbreak is tethered, so run it again after every reboot.
- A way to register homebrew titles from `/data/homebrew`, such as etaHEN
  or ShadowMount+. The title ID is `PPSA99207`.
- For View in VR, a PS VR2, paired and set up in the system settings.

## Installing

Once Parallax Browser is listed on ProsperoStore, you can install it from
the store. To install it by hand:

1. Download the zip from [Releases](https://github.com/rikleigh/parallax-browser/releases)
   and unpack it.
2. Copy its `PPSA99207` folder to `/data/homebrew` on the console over FTP,
   so that it is `/data/homebrew/PPSA99207`.
3. Register `/data/homebrew/PPSA99207` with your homebrew tool, etaHEN or
   ShadowMount+. Parallax Browser then appears on the home screen.

On its first start, the browser copies its runtime files from the title
into `/data/ps5webkit`. These are the fonts, certificates, Unicode data
and the start page.

To update, replace the `PPSA99207` folder with the new one, or update from
the store. The next start brings the runtime files up to date. Each PS5
user's bookmarks, history, settings and downloads live in
`/data/ps5webkit/users`. Updates never touch that folder, so your data
stays.

Until 2026-11-10 the title ID was `PPSA99020`, which another app in the
homebrew catalog uses. If you installed a build from before then, remove
that title with your homebrew tool. Your data in `/data/ps5webkit` stays.

## Security

- **The link server is open to your local network by default.** Anyone on
  the same network can send links to the PS5, see and control its tabs,
  change settings and fetch downloads. They can already do as much through
  elfldr and FTP, which the jailbreak leaves open.

  To limit the link server, put a PIN in `/data/ps5webkit/push-pin.txt`,
  which the browser reads when it starts. Or turn the link server off in
  Settings, which still opens on the PS5. The link server refuses to
  import cookies unless you set up a PIN or
  `/data/ps5webkit/push-cookie-key.txt`.

  Pages in the browser can't reach the link server. While private browsing
  is on, the link server doesn't say which pages are open. It still keeps
  and lists downloads, as in normal browsing.
- **The browser has no sandbox, and a malicious page could take over the
  console.** Web pages run in a process the browser starts like any other
  payload, or, with the experimental "Pages in the browser process" on,
  inside the browser itself. Either way they run as root, outside the
  system's jail, with the jailbreak's kernel access, and can reach elfldr. A web page
  that exploits a bug in WebKit, most often in the JavaScript JIT, would
  get all of it. That means every user's cookies and logged-in sessions,
  the console's files and account data, and the kernel until the next
  reboot. It could also install itself to run again after the next
  jailbreak, and attack other devices on your network.

  No exploit for this browser is known. WebKit fixes security bugs every
  month, though, and each release of this browser keeps the WebKit
  version it shipped with.

  Browse sites you trust and keep ad blocking on. For any other site, turn
  on Safer browsing with the shield button in the menu, or in Settings. It
  turns off the JavaScript JIT, WebAssembly and WebGL, where most exploits
  start, and runs pages in a separate process (the browser restarts to
  switch). Scripts then run several times slower, and some games and 3D
  sites won't work. Don't sign in to accounts that matter on the PS5, and
  don't import cookies from another browser.

  If the console behaves oddly after you visit a site, reboot it before
  you run the jailbreak again. Then check `/data/homebrew` and etaHEN's
  autoload for files you didn't put there.
- **Pages can't reach the jailbreak's own services.** FTP, elfldr and
  similar tools on the console take commands from anyone who connects, so
  a page could otherwise send them commands. The browser refuses every
  connection from web content to the PS5 itself, on its loopback address
  or its own network address. Other devices on your network stay
  reachable, so use the jailbreak only on a home network you trust.

## Building

The build cross-compiles WebKit and about 30 libraries for the PS5, on a
Mac with Apple silicon. The steps are:

1. Run `brew install llvm@18 socat cmake ninja wget nasm bison glib gperf`,
   then `. ./env.sh`. Put the console's address in `PS5_HOST` and the
   Mac's in `PS5WK_LOG_HOST`, in `env.local.sh`, which git ignores.
2. Clone the reference checkouts into `ref/`, which git also ignores. The
   scripts expect exactly these folder names. Each fetch takes only the
   commit the build uses. `ps5-opengl` and `WebKit` must be at these
   commits, because the build scripts check them out by hash. A plain
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

   Then build the SDK into `toolchain/ps5-payload-sdk`, as in
   [Setup for the spikes](#setup-for-the-spikes).
3. `spikes/04-jsc/build.sh` builds libc++ 19 and ICU.
   `spikes/02-gl-egl/ps5-opengl/build.sh gles` builds ps5-opengl with
   OpenGL ES and the browser's patches. Next,
   `PS5_OPENGL_PREFIX=$PWD/toolchain/ps5-opengl-gles-noprof-m8 spikes/02-gl-egl/ps5-opengl/build.sh gles-noprof`
   installs the build that the browser links against. It is the same
   build with the per-draw profiling compiled out, and `package.sh` looks
   for it in that folder. Last, `port/deps/build.sh all` builds the
   libraries into `toolchain/wpe-sysroot`. See
   [port/deps/README.md](port/deps/README.md).
4. `port/webkit/build.sh` applies the patches in `port/webkit/patches/` to
   WebKit and builds it. Then `port/webkit/apps/title/package.sh` writes
   the title folder. See [port/webkit/README.md](port/webkit/README.md#build).
5. `tools/upload-title.sh toolchain/wpe-webkit/titles/PPSA99207` copies the
   title to the console, and `tools/launch.sh` registers, starts and stops
   it. The title folder carries the runtime files in `runtime/` and
   installs them into `/data/ps5webkit` when it starts. Once the console
   has the runtime files, `NO_RUNTIME=1` packages the title without them,
   which makes uploads faster.

`tools/logserver.sh` collects the browser's log on the Mac.

`tools/make-release.sh <version>` builds the release zip from the WebGPU
build (`WEBGPU=1 port/webkit/build.sh`, in `build-webgpu`). It refuses a
working tree with uncommitted changes, since the zip names the commit. The
zip has the `PPSA99207` folder at the top, as the homebrew catalog
requires, and the build inside has no log address. For every release, raise
`contentVersion` in `port/webkit/apps/title/sce_sys/param.json` so that
consoles see the new build as an update. The format is
`01.<minor>.<patch>`, so 0.1.0 is `01.001.000`.

## Repository layout

| Path | What it is |
| --- | --- |
| `port/webkit/` | The browser. It has the WebKit patches, the PS5 platform backend in `wpe-ps5/`, the title app in `apps/title/`, the ad blocker, the tests and the development notes. See its [README](port/webkit/README.md). |
| `port/deps/` | Cross-builds of the libraries WebKit needs, and their patches |
| `spikes/` | The feasibility experiments the project started from |
| `tools/` | Tools to upload, launch, log and convert |
| [FEASIBILITY.md](FEASIBILITY.md) | The original feasibility study and risk register |

## Setup for the spikes

Clone `ref/ps5-payload-sdk` as in step 2 above, then run:

```sh
make -C ref/ps5-payload-sdk DESTDIR=$PWD/toolchain/ps5-payload-sdk install
TMPDIR=$PWD/build ref/ps5-payload-sdk/libcxx.sh    # libc++ for C++ code
```

To run a payload spike, use `make -C spikes/NN-name send`. Its output
comes back over the elfldr socket into `logs/`.

## Legal

This is an independent homebrew project, not affiliated with or endorsed
by Sony Interactive Entertainment. "PlayStation", "PS5" and "PS VR2" are
trademarks of Sony Interactive Entertainment Inc. The repository contains
no Sony system files, firmware or keys. Code that uses system libraries
resolves them on the console at run time.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE). The WebKit patches are under
WebKit's own licences, LGPL-2.1 and BSD.
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) lists the third-party
components and their licences.
