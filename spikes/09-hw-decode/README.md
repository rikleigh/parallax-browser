# Spike 9: hardware video decode (R16)

**Question:** can a homebrew title on 13.60 use Sony's hardware video decoder
for the browser's VR video (H.264/HEVC/VP9 access units from MSE's fMP4
segments, decoded frames drawn with ps5-opengl under the page and in the
headset), and how fast is it?

**Answer (measured on the console, 2026-10-27):** yes, from a registered
title. `libSceVideodec2` decodes:
- **8K HEVC from the owner's VR sample (7680x3840, 35 Mbit/s) at 93 fps**;
- 4K H.264 at 87 fps;
- 4K–5.7K VP9 at 68–88 fps;
- HEVC Main10 and 8192x4096.

It writes linear NV12 (16-bit LSB-aligned for 10-bit) into memory the caller
supplies, and that memory can be a GL buffer, so the title's GL converts it
to RGBA8 on the GPU with **no CPU copy**: 3.2 ms at 8K, checked by readback.
An elfldr payload can't use it: it blocks in the first call.

The owner checked the TV and headset output by eye: correct and smooth
(details under *Headset and TV* below).

## What was built

| File | What |
| --- | --- |
| `vdec2.[ch]` | Loads `libSceVideodec2` (sysmodule 0xCF) and binds 9 functions; brings up a decoder (compute queue, memory, CreateDecoder, Reset); decode, flush, renew, close. Optional caller-owned output slots. ABI from PS5VR's `sce_videodec2.h` / `eng_vdec_native.c` (GPL; they took it from ProsperoLight) |
| `exports/videodec2_exports_1360.h` | 13.60 export offsets + anchor (`tools/gen_export_table.py` on the console's own decrypted library, fetched with ftpsrv, which decrypts SELFs on the fly; the `.sprx` stays in gitignored `ref/sprx-1360/`) |
| `au.[ch]` | Access units out of Annex-B H.264/HEVC, IVF (VP9, superframes split) and **fragmented MP4** (avcC/hvcC → Annex-B, parameter sets before keyframes, as h264parse/h265parse do). Host check: `hostcheck/au_test.c` matches ffprobe's frame counts on every clip, and its output decodes cleanly in ffmpeg |
| `hw_decode.c` | Title (PPSA99014, `build-title.sh`) and payload (`make`, `-DPAYLOAD`). Job list `/data/ps5webkit/hwdec/jobs.txt` with modes `bench`, `gl`, `vr360`, `vr180` |
| `run.sh jobs/<list>.txt [--payload]` | Uploads the clips a list names, installs the list, launches the title (or sends the payload) |
| `make-clips.sh` | Synthetic clips on the Mac: side-by-side testsrc2 halves with the frame number burned in and temporal noise, so the encoder spends tube bitrates (40–80 Mbit/s); fMP4 + elementary stream |
| `fetch-sample.sh v.json` | 30 s cuts of the owner's VR sample (HEVC 4320x2160, 5760x2880, 7680x3840; ~35 Mbit/s, 360° mono). **Local testing only:** `media/` is gitignored and the signed URLs are not kept |
| `sw/` | Software fallback: `build-ffmpeg.sh` (FFmpeg 7.1.1 minimal + dav1d 1.5.1, both with x86 SIMD, into `toolchain/ffmpeg-sw`), `sw_decode.c` payload, `run.sh` |

## Loading (title and payload)

| Call | Title | Payload |
| --- | --- | --- |
| `sceSysmoduleLoadModule(0xCF)` (before the etaHEN unjail, per PS5VR) | 0 | 0 |
| `sceKernelLoadStartModule(<sandbox>/common/lib/libSceVideodec2.sprx)` | handle 0xd1 | handle 0x22 (`/system/...`) |
| `sceKernelDlsym` by name, 9 functions | **all refused** | all refused |
| 13.60 export offsets (anchor `SCEVIDEODEC2` at 0x8880 matched) | **9/9 bound** | 9/9 bound |
| `sceVideodec2QueryComputeMemoryInfo` | 0 | **blocks forever** (whole process; even a watchdog thread never ran) |
| `sceSysmoduleLoadModule(0x88)` (libSceAudiodec, for AAC) | 0 | 0 |

The decoder exports 21 functions. 15 are named by the candidate list:
`MapMemory`, `MapDirectMemory`, `QueryComputeMemoryInfo`,
`AllocateComputeQueue`, `ReleaseComputeQueue`, `QueryDecoderMemoryInfo`,
`CreateDecoder`, `Decode`, `Flush`, `GetPictureInfo`,
`Get{Avc,Hevc,Vp9}PictureInfo`, `Reset` and `DeleteDecoder`. Codec tags:
AVC 1, HEVC 974921, VP9 2382845. **There is no AV1**: no `GetAv1PictureInfo`
alongside the AVC/HEVC/VP9 ones (inferred: the PS5's video block has no AV1
decode).

Call sequence (all returned 0 in the title):
1. `QueryComputeMemoryInfo`.
2. Allocate direct memory (type 12, prot 0x33), then `AllocateComputeQueue`.
3. `QueryDecoderMemoryInfo`.
4. `MapNamedFlexibleMemory` for the CPU part, plus three direct allocations.
5. `CreateDecoder`, then `Reset`.
6. `Decode` once per access unit, with an output slot each time.
7. `Flush` at the end.

Bring-up takes 63–111 ms for HEVC and 3.5–5 ms for VP9.

## Throughput (title, decode only, as fast as it goes)

Decode is called back to back on one thread, the access unit is copied into
the decoder's input ring, and every output picture is counted. "Changed after
return": three pictures per clip were hashed when `Decode` returned, then
again 50 ms later, and none had changed, so a returned picture is complete.

| Clip | Codec | Size | Bitrate | fps | Avg call (ms) | Flexible memory (MB) |
| --- | --- | --- | --- | --- | --- | --- |
| **VR clip 8K, fMP4** | HEVC Main | 7680x3840 | 35 Mbit/s | **93.4** | 10.6 | 223 |
| VR clip 8K, Annex-B | HEVC Main | 7680x3840 | 35 Mbit/s | 93.3 | 10.5 | 223 |
| VR clip 8K, pipeline depth 2 | HEVC Main | 7680x3840 | 35 Mbit/s | 79.4 | 12.4 | 139 |
| VR clip 8K, pipeline depth 1 | HEVC Main | 7680x3840 | 35 Mbit/s | 49.2 | 20.1 | 97 |
| VR clip 6K | HEVC Main | 5760x2880 | ~32 Mbit/s | 130.4 | 7.5 | 153 |
| VR clip 4K | HEVC Main | 4320x2160 | ~23 Mbit/s | 90.5 | 11.0 | 95 |
| synthetic 8K (noise) | HEVC Main | 7680x3840 | 80 Mbit/s | 62.1 | 15.8 | 223 |
| synthetic 8K (noise) | HEVC Main10 | 7680x3840 | 80 Mbit/s | 59.3 | 16.5 | 223 |
| synthetic | HEVC Main | 8192x4096 | 80 Mbit/s | 60.1 | 16.3 | 251 |
| synthetic | HEVC Main | 5760x2880 | 60 Mbit/s | 82.5 | 11.8 | 153 |
| synthetic | HEVC Main | 3840x2160 | 40 Mbit/s | 127.6 | 7.6 | 75 |
| synthetic | H.264 High | 3840x2160 | 50 Mbit/s | 86.8 | 11.2 | 71 |
| synthetic | H.264 High | 1920x1080 | 12 Mbit/s | 269.8 | 3.5 | 24 |
| synthetic | VP9 Profile 0 | 3840x2160 | 30 Mbit/s | 84.2 (IVF), 83.5 (fMP4) | 11.6 | 13 |
| synthetic | VP9 Profile 0 | 5760x2880 | 40 Mbit/s | 67.9 | 14.3 | 22 |
| synthetic | VP9 Profile 2 (10-bit) | 3840x2160 | 30 Mbit/s | 88.3 | 10.2 | 14 |

There were 0 decode errors and 0 pictures flagged in every row (the full
numbers are in `evidence/2026-10-27/09-hw-decode-title.log`). Speed depends
on bitrate: the real 35 Mbit/s 8K stream decodes 1.5× faster than the noisy
80 Mbit/s one. Every pass mark is met with a wide margin (4K30 minimum, 8K30
bonus). PS5VR's note "8K60 peaks at 45–50 fps" matches depth 1 here; depth 4
nearly doubles it.

Pictures checked on the Mac: each `bench` job writes picture 45 as a 1/8
scale PPM, converted on the CPU. VR clip 8K picture 45 is identical to
ffmpeg's frame 45, and the synthetic clips show the burned-in "45" in both
eyes for H.264, HEVC 8-bit and 10-bit, and VP9 Profile 0.

## Memory (what the decoder asks for)

| Stream | Flexible (MB) | Direct `gpu` + `cpu_gpu` (MB) | One output frame (MB) |
| --- | --- | --- | --- |
| H.264 1080p | 24 | 70 | 3.2 |
| H.264 4K | 71 | 292 | 12 |
| HEVC 4K | 75 | 337 | 12 |
| HEVC 5760x2880 | 153 | 960 | 24 |
| HEVC 7680x3840 (depth 4 / 2 / 1) | 223 / 139 / 97 | 1169 / 941 / 827 | 42 |
| HEVC Main10 7680x3840 | 223 | 1788 | 84 |
| HEVC 8192x4096 | 251 | 1329 | 48 |
| VP9 4K / 5.7K / 4K Profile 2 | 13 / 22 / 14 | 535 / 1012 / 838 | 12 / 24 / 24 |

Plus 4.6 MB for the compute queue, a 32 MB input ring (4 × 8 MB; the
largest AU seen was 4.3 MB), and the output slots (8 here).

**Flexible memory is the constraint.** The title starts with 419 MB of
flexible memory available. An 8K HEVC decoder at depth 4 leaves 66 MB, and
the browser's UIProcess heap competes for the same pool. Depth 2 (139 MB,
79 fps) is the likely setting for 8K. HEVC at 4K and VP9 at any size are
cheap. Direct memory (11.7 GB free) is not a concern.

## Output format and the way into GL

- **Layout:** linear, row pitch = width rounded up to 64 for HEVC at 4320
  (4352) and 5760 (5888); 2048 for 1080p H.264. Height is the coded height
  (1088 at 1080p, 2176 at 4320x2160). Y plane, then interleaved UV at
  `pitch_bytes × height`. `frame_format` is 0 for both depths.
- **10-bit:** 16-bit samples, `pitch_bytes = 2 × pitch`, with **10-bit values
  in the low bits** (Y samples 0x0131, 0x0154...). That is not P010, which is
  MSB-aligned, so shaders divide by 1023.
- **Where it lands:** in the output slot passed to `Decode`. Any GPU-visible
  direct memory works, including **the persistent mapping of a GL buffer**:
  ps5-opengl allocates buffers as direct memory type 12 / prot 0x33 (the
  decoder's own type), and the GPU VA equals the CPU VA.

`gl` job: 8 GL buffers (`glBufferStorage` with persistent and coherent
mapping) are handed to `vdec2_open` as output slots. Per picture:
1. A fragment pass reads NV12 or 10-bit from the slot as an SSBO (max 128 MB,
   enough for 8K 10-bit).
2. It writes an immutable RGBA8 texture, which ps5-opengl lays out as
   SW_64KB_R_X and which exports as a T# (logged), the same hand-off Spike 3b
   used for Hmd2.
3. That texture is drawn letterboxed on the TV.

| Clip | fps (TV vsync 60) | NV12→RGBA (ms) | Readback |
| --- | --- | --- | --- |
| VR clip 4320x2160 HEVC | 59.7 | 0.81–0.89 | matches the CPU-converted picture |
| VR clip 7680x3840 HEVC | 59.4 | 3.24 | correct |
| HEVC Main10 7680x3840 | 57.7 | 4.88 | correct ("45") |
| VP9 3840x2160 | 54.1 | 1.97 | correct ("45") |

**Zero copy into GL is verified** (readback, no person needed). For
comparison, a CPU `memcpy` of one decoded frame into a GL buffer costs
0.96 ms at 4K and 2.9 ms at 8K (5.95 ms for 8K 10-bit), so even the copy
fallback would be affordable.

## Headset (`vr360` / `vr180`) and TV: checked by the owner

The RGBA8 frame is drawn on a sphere (360 mono) or a front dome (180
side-by-side) per eye, sampling along each pixel's view ray with the
tracker's eye orientation. It goes into 3 exported eye buffers through
`spikes/common/psvr2`, with Hmd2 at 120 Hz and half-rate rendering, and is
paced to the video's frame rate. Job lines can set the eye buffer size, the
samples per pixel (1, or 4 on a rotated grid) and the frame rate.

**Owner's checks (2026-10-27, evening):**

| Run | Seen | Log |
| --- | --- | --- |
| TV, `jobs/gl8k.txt` (VR clip 8K, 8K 10-bit bars, VP9 4K, VR clip 4K) | "looked fine": colours, no blocks, no tearing | 54–60 fps at vsync, 0 errors |
| Headset, `jobs/vr.txt` (VR clip 8K 360 night street, synthetic 8K 180 SBS) | all correct: surrounds you, upright, world-locked, 3D fused, smooth. The street scene "looked low bitrate" | 1804 / 1202 headset frames, every one tracked, 0 late |
| Headset, `jobs/vr-sharp.txt` (daylight VR clip 8K 360, 47 Mbit/s): eye 1536 ×1, 2048 ×4, 2560 ×4 | "hard to tell" apart; all three the same. Nearby people jittered in all three | render 0.6 / 1.2 / 1.7 ms; the headset accepts 2560 per eye |
| Headset, `jobs/vr-60fps.txt` (daylight VR clip 360 at 60 fps, 10-bit): 4320x2160, then 7680x3840 | "much better", both passes OK | 4K: 59.1 fps. 8K: **46.8 fps**, the single-threaded loop can't keep up |

What this settles:
- **"Low bitrate" is the source.** The night street clip decoded by ffmpeg
  on the Mac at full resolution shows the same smearing and blotches (a
  consumer 360 camera at night, 35 Mbit/s). The decoder reproduces it.
- **Jitter in nearby people is 30 fps footage**, not our pacing: the frame
  timing in the file is regular, camera motion looked smooth, and nearby
  people move far between frames with little motion blur. The same kind of
  footage at 60 fps looked smooth.
- **Sharpness is limited by the panel and the source**, not the eye buffer
  above 1536. The browser's player uses 2048 per eye with 4 samples per
  pixel (1.2 ms), which removes aliasing at little cost.
- **8K60 needs decoding on its own thread.** The spike decodes, converts and
  renders on one thread (about 12 + 5 + 1.3 ms per 8K frame), so it reaches
  47 fps; the decoder alone runs at 80–93 fps at 8K. The player must decode
  ahead into a queue and pick frames by timestamp against the display clock
  (which also drops or repeats frames to stay in sync instead of drifting,
  as the 4K60 pass did).

## Software fallback (payload, FFmpeg 7.1.1 + dav1d 1.5.1, AVX2)

Decode only, frames counted, no conversion.

| Clip | Decoder | 8 threads (fps) | Other thread counts (fps) |
| --- | --- | --- | --- |
| VR clip 4320x2160 HEVC | ffmpeg hevc | 50.8 | 16 threads: 32.3 |
| synthetic 4K HEVC 40 Mbit/s | ffmpeg hevc | 46.1 | 16 threads: 29.8 |
| synthetic 4K H.264 50 Mbit/s | ffmpeg h264 | 75.0 | 16 threads: 48.4 |
| synthetic 4K VP9 30 Mbit/s | ffmpeg vp9 | 35.3 | 16 threads: 26.2 |
| synthetic 4K AV1 | dav1d | 91.9 | — |
| VR clip 5760x2880 HEVC | ffmpeg hevc | 31.2 | — |
| synthetic 5.7K VP9 | ffmpeg vp9 | 27.4 | 16 threads: 5.6 |
| VR clip 7680x3840 HEVC | ffmpeg hevc | 20.1 | 6 threads: 20.1; 4: 14.1; 12: 12.6; 16: 0.9 |
| synthetic 8K AV1 | dav1d | 33.7 | 6 threads: 33.9; 12: 33.1 |

**Use at most 8 decoder threads.** More threads collapse throughput (8K HEVC
fell to 0.9 fps at 16), probably because applications get fewer cores than
the 16 hardware threads. Note that two hung payload processes from the
payload test (pids 200 and 201) were still on the console during these runs;
if they spin, they take cores too. In software, 4K of any codec plays at
30 fps, and AV1 even at 8K (the synthetic clip is easy; real 8K AV1 will be
slower). 8K HEVC does not (20 fps).

## Consequences for the browser

- A GStreamer decoder element over `vdec2` is the route for H.264, HEVC
  (8-bit and 10-bit) and VP9 (Profile 0 and 2). Each AU in, one picture out,
  in display order, complete on return. The element advertises those and
  output into GL buffer memory the title owns.
- **AV1 has no hardware route.** dav1d is fast enough for 4K and probably
  for 8K, so `av01` could be advertised with a software decoder. If it isn't
  advertised, YouTube falls back to VP9, which tops out at 4K for most
  videos (8K is AV1-only).
- **VP9 alt-ref (show_frame = 0) frames return a picture**, which must be
  dropped. With Profile 2 the picture numbering was still off by one after
  dropping (picture 45 showed "46"), so the element should pair pictures to
  frames by pts with `sceVideodec2GetPictureInfo` (exported, not yet
  called), not by call order.
- The decoder lives in the title (it needs an application process). That
  matches the plan: the UIProcess/GPU side is the title, and decoded frames
  never leave it.
- Budget about 140 MB of flexible memory for an 8K HEVC decoder (depth 2), and
  reuse one decoder per codec, as PS5VR does.

## Reproduce

```sh
. ./env.sh
spikes/09-hw-decode/make-clips.sh                    # synthetic clips (media/, gitignored)
spikes/09-hw-decode/fetch-sample.sh <v.json>          # owner's VR sample (local only)
spikes/09-hw-decode/build-title.sh && make -C spikes/09-hw-decode
spikes/09-hw-decode/run.sh spikes/09-hw-decode/jobs/all.txt     # decode benchmarks (no display needed)
spikes/09-hw-decode/run.sh spikes/09-hw-decode/jobs/gl8k.txt    # TV: zero-copy GL (owner watches the TV)
spikes/09-hw-decode/run.sh spikes/09-hw-decode/jobs/vr.txt      # headset (owner wears it)
spikes/09-hw-decode/sw/build-ffmpeg.sh && make -C spikes/09-hw-decode/sw && spikes/09-hw-decode/sw/run.sh 8
```
