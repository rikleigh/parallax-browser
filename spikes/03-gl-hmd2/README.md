# Spike 3: GL → PS VR2 (the key go/no-go)

**Gate:** stable, head-tracked stereo in the headset from a GL-rendered
texture. If GL memory can't reach the Hmd2 layer, the project needs an AGC
blit path or is blocked.

## What's known (from reading PS5VR)

- Frames reach the headset as a **type 0x10 layer** through Hmd2
  reprojection: `sceHmd2ReprojectionSetParam` followed by
  `sceVideoOutSubmitFlip` on VideoOut **bus 32** (Hmd2's VR port). Output is
  120 Hz with half-rate rendering, so the app renders at 60 fps. One
  side-by-side eye buffer is used for both eyes, with per-eye viewports, FOV
  tangents, the device pose and the predicted display time.
- Buffers are registered as **32-byte GFX10 texture descriptors (T#)** over
  direct memory. The compositor samples whatever GPU memory a T# points at,
  so any renderer whose memory we can address can feed it. PS5VR and Sony
  titles use RGBA8 or RGB10A2 with **SW_64KB_R_X** tiling.
- `spikes/common/psvr2/` is that sequence as a small backend: preload/bind,
  open, start(T#s), begin_frame (pose), submit, stop, close.

## 3a: `hmd2_probe.elf` (payload, no GL)

CPU-drawn test card in **linear** buffers. It answers two questions before GL
is involved:

1. Does the headset path work from an **elfldr payload**? PS5VR only runs as
   a title. A payload means fast iteration, and possibly a WebKit
   child-process-friendly XR process.
2. Does the compositor accept **linear** textures? If yes, any GL texture we
   can locate can be submitted. If not, we need SW_64KB_R_X: a GPU blit into
   a tiled buffer, or a driver that renders tiled.

```sh
. ./env.sh && make -C spikes/03-gl-hmd2 send     # wear the headset
```

Expected: a red-tinted left eye, a blue-tinted right eye, a white grid, and
a green bar sweeping once a second. Record the visual result in FEASIBILITY.md.

## 3b: `gl_hmd2` (title, ps5-opengl)

A GL 4.6 scene (cube ring, floor grid) rendered per eye with the tracker's
pose for the predicted display time. It uses one of two hand-offs:

- **zerocopy:** GL textures exported as T#s through
  `eglExportTextureDescriptorPS5`, a ps5-opengl patch that lives in
  `spikes/02-gl-egl/ps5-opengl/`. This is the hand-off a WebXR swapchain
  needs.
- **copy:** the fallback on stock ps5-opengl. `glReadPixels` into a linear
  direct-memory buffer, submitted as a linear T#. It proves the pipeline even
  though it costs a readback.

Logged: path, render ms, hand-off ms, missed frame slots, submit errors,
tracking. **Pass** means >55 fps rendered with fewer than 2% of frames over
budget, plus a visual check: world-locked cubes, correct stereo depth, no
judder when turning.

Build and packaging depend on the ps5-opengl build (Spike 2), so see
`spikes/02-gl-egl/README.md` for the title packaging steps.

## Notes and risks

- **Eye positions:** PS5VR only reads eye *orientations*, since video sits at
  infinity. psvr2 reads the same tracker result's `+0xa0`/`+0xe0` as eye
  *positions* by symmetry with the device block, and accepts them only when
  their separation is a plausible IPD. Otherwise it uses head ± 31.5 mm.
  3a logs both, so check the log.
- **Row order:** both paths render with a flipped projection so that memory
  row 0 is the top of the image, which is what the compositor expects.
- **Firmware:** psvr2 binds by name first, then by per-firmware offsets
  guarded by anchor strings. Only the 13.60 table ships, and other firmware
  fails safely at `preload`.

## Results

_Pending console runs: 3a first, then 3b._
