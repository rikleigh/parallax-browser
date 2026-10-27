# ps5webkit

Feasibility study: a homebrew WebKit browser on a jailbroken PS5 that shows
immersive **WebXR** content on a **PS VR2**.

The result so far, plus the risk register and recommendation, is in
**[FEASIBILITY.md](FEASIBILITY.md)**. This tree holds throwaway spikes. Each
one tests a single high-risk unknown and has a go/no-go gate.

| Spike | Question | Runs as |
| --- | --- | --- |
| [00-baseline](spikes/00-baseline) | Toolchain, elfldr, firmware, PS VR2 libraries present | payload |
| [01-procs-ipc](spikes/01-procs-ipc) | Can WebKit's multi-process model work? fork, fd passing, shm, elfldr spawn, memory, JIT memory | payload (+ title via 02) |
| [02-gl-egl](spikes/02-gl-egl) | GLES contexts and offscreen rendering, on ps5-opengl and on pacbrew Mesa (RADV/zink) | title / payload |
| [03-gl-hmd2](spikes/03-gl-hmd2) | **Key gate:** GL-rendered, head-tracked stereo in the PS VR2 | payload (3a), title (3b) |
| [04-jsc](spikes/04-jsc) | JavaScriptCore cross-compiled, JIT off, on device | payload |
| [05-openxr-runtime](spikes/05-openxr-runtime) | A minimal OpenXR runtime over PS VR2 that WebKit's backend accepts | host test + PS5 lib |
| [06-wpe-minibrowser](spikes/06-wpe-minibrowser) | WPE WebKit with a PS5 WPE Platform backend on the TV | title |
| [07-webxr-e2e](spikes/07-webxr-e2e) | immersive-vr WebXR samples in the headset | title |

## Setup (macOS, Apple silicon)

```sh
brew install llvm@18 socat cmake ninja wget
. ./env.sh          # PS5_PAYLOAD_SDK, LLVM_CONFIG, PATH, PS5_HOST
```

`env.sh` expects the payload SDK built into `toolchain/ps5-payload-sdk`. To
build it:

```sh
git clone --depth 1 https://github.com/ps5-payload-dev/sdk ref/ps5-payload-sdk
make -C ref/ps5-payload-sdk DESTDIR=$PWD/toolchain/ps5-payload-sdk install
TMPDIR=$PWD/build ref/ps5-payload-sdk/libcxx.sh    # libc++ for C++ spikes
```

Put the console's address in `env.local.sh` (gitignored):

```sh
export PS5_HOST=192.168.1.50      # the PS5
export PS5WK_LOG_HOST=192.168.1.20 # this Mac, for title logs
```

## Console prerequisites

- A jailbreak-capable firmware (Relapse covers 7.00–13.60), kstuff, etaHEN,
  and **elfldr on port 9021**. The jailbreak is tethered, so re-run it after
  every reboot.
- ftpsrv (port 2121) for copying files, and ShadowMount+ or etaHEN to
  register homebrew titles from `/data/homebrew`.
- A PS VR2, paired and set up in the system settings.

## Running

- **Payloads:** `make -C spikes/NN-name send`. The payload's stdout comes back
  over the elfldr socket and is saved to `logs/`.
- **Titles:** start `tools/logserver.sh` on the Mac, install the title folder,
  then launch it from the home screen. Logs arrive on the Mac.
- `tools/results.sh` turns the `RESULT` lines in `logs/` into the tables
  that go into FEASIBILITY.md.

## Layout and rules

- `ref/` holds third-party checkouts and `toolchain/` holds local builds. Both
  are gitignored.
- `spikes/common/` has the shared logging (`spike.h`), the GL helpers, and
  `psvr2/`, a minimal PS VR2 output backend derived from PS5VR (GPL-3).
- Never commit or redistribute Sony files: `.sprx`/`.prx`/`.self`, firmware,
  keys, or decrypted system libraries. `.gitignore` blocks the common
  extensions. Per-firmware export tables hold offsets only, as PS5VR's do.
- Licensing: the spikes link GPL-3 code (payload SDK, PS5VR-derived psvr2,
  ps5-opengl), so treat this tree as GPL-3.0-or-later. See FEASIBILITY.md for
  how that combines with WebKit's LGPL/BSD.
