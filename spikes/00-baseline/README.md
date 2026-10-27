# Spike 0: environment baseline

**Gate:** hello_world runs, `sysinfo` reports a supported firmware with the
PS VR2 libraries present, and PS5VR shows video in the headset on this
console.

## Host side (done)

- ps5-payload-sdk (master, 2026-10-21) built with Homebrew LLVM 18.1.8 into
  `toolchain/ps5-payload-sdk`. libc++/libc++abi/libunwind are built with the
  SDK's `libcxx.sh`.
- The SDK's `samples/hello_world` and `sysinfo.elf` build cleanly.

## On the console

```sh
. ./env.sh
make -C spikes/00-baseline hello    # a "Hello, world!" notification on the TV
make -C spikes/00-baseline send     # sysinfo: firmware, creds, memory, VR libs
```

`sysinfo` prints `RESULT 00-baseline <test> PASS|FAIL ...` lines for:
`firmware`, `process`, `memory`, `cpus`, `hmd2-lib`, `vrtracker2-lib`,
`vrhand-lib`, `hand-models` and `homebrew-dir`.

### PS VR2 check with PS5VR

Install PS5VR the way its README describes (title folder `PPSA99177` to
`/data/homebrew`, plus `ps5vr-helper.elf` to elfldr), then play any video in
the headset.

- The original repo (theghostonline/PS5VR) is gone. The surviving fork
  `kavera001-dev/PS5VR` has source with a 13.60 export table, plus a binary
  release `v1.3.0-fw12.70-v2` that its README implies supports 12.70. Use the
  release that matches the console's firmware.
- If the console isn't on 13.60 (or 12.70 with that release), PS5VR needs an
  export table generated from **this console's own** decrypted
  `libSceHmd2.sprx`, `libSceVrTracker2.sprx` and `libSceVrHand.sprx`.
  Decrypt them on the console with ps5-self-pager, then run
  `tools/gen_export_table.py` (vendored from PS5VR; see
  `ref/PS5VR/docs/firmware-support.md`). Add the table to
  `spikes/common/psvr2/exports/` and to `kTables` in `psvr2.c`. Never commit
  the `.sprx` files.

### SCE stubs for this firmware

The SDK ships stubs for 34 system libraries, including libSceVideoOut,
libScePad, libSceImeDialog, libSceUserService and libSceSysmodule. That's
enough for every spike so far. Hmd2 and VrTracker2 are bound at runtime by
name or offset (psvr2), so they need no stubs. Generate more only when a
spike needs them:

```sh
ln -s /path/to/decrypted/libSceXYZ.sprx ref/ps5-payload-sdk/sce_stubs/
pip3 install pyelftools
make -C ref/ps5-payload-sdk/sce_stubs stubs
make -C ref/ps5-payload-sdk DESTDIR=$PWD/toolchain/ps5-payload-sdk install
```

## Results

_Pending a console run. Paste `tools/results.sh logs/sysinfo-*.log` here and
into FEASIBILITY.md._
