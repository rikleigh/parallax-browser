# WebGPU test pages

Self-checking pages for WebGPU in the browser (Dawn over Vulkan on the
console). Each page runs when it loads, with no input, and logs one line per
case to the console and to the page:

    RESULT webgpu-<page> <case> PASS|FAIL|SKIP <why>
    RESULT webgpu-<page> summary PASS|FAIL <passed>/<cases>

Failures are red on the page. Every awaited step has a 10 s timeout that
fails the case with the step's name, so a page always reaches its summary.
With WebGPU off, every page logs one FAIL (`no navigator.gpu`, or
`requestAdapter returned null`) and its summary. `webgpu-test.js` is the
shared helper; `index.html` links the pages.

## Pages

| Page | What it checks |
| --- | --- |
| `adapter.html` | `navigator.gpu`, `requestAdapter()` with each option, `adapter.info`, the feature list, every limit (logged, and at least the default), preferred canvas format, WGSL language features |
| `device.html` | `requestDevice()`, default features and limits, each adapter feature requested alone, bad feature and over-limit requests reject, a second `requestDevice()` on one adapter, `destroy()` and `device.lost`, labels, `onSubmittedWorkDone()` |
| `compute.html` | Compute pipelines writing storage buffers, read back with `mapAsync`: a fill, two bind groups with `dispatchWorkgroupsIndirect`, a 64x64 matrix product, `createComputePipelineAsync`; timings logged |
| `offscreen-readback.html` | Rendering into textures and `copyTextureToBuffer`: triangle, 4x MSAA resolve, depth test, `writeTexture` + sampling, `writeBuffer` + `copyBufferToBuffer`, rgba16float, mip levels and views |
| `errors.html` | Error scopes (validation, out-of-memory, nested), `uncapturederror`, `getCompilationInfo()` line numbers, `GPUPipelineError`, calls on a destroyed buffer; the browser's memory budget (128 MiB buffers until one is refused, and that destroying gives the memory back) |
| `buffer-map.html` | Mapping as ML libraries use it: `mappedAtCreation`, partial and disjoint ranges, typed views, `mapAsync` for writing and reading, round trips of 1, 16 and 64 MiB, `requestAdapter()` again after a device |
| `triangle.html` | A 640x360 canvas with a turning triangle; frame rate after 30 frames; one frame read back through `drawImage` |
| `cube.html` | A textured, depth-tested, indexed cube with a uniform buffer written every frame; frame rate after 120 frames |
| `resize.html` | `canvas.width/height` through 640x360, 1280x720, 1920x1080, 16x16 and back; `unconfigure()`/`configure()`; rgba16float with extended tone mapping; a second device |
| `two-canvases.html` | Two canvases on one device and a third on a second device, animated together; a canvas removed from the document and re-added |
| `todataurl.html` | `toDataURL`, `toBlob`, `createImageBitmap` and `drawImage` of a WebGPU canvas, in the task that drew and three frames later; alphaMode opaque and premultiplied |
| `copy-external-image.html` | `copyExternalImageToTexture` from a 2D canvas, ImageBitmap, OffscreenCanvas, HTMLImageElement and ImageData, with `flipY` and `premultipliedAlpha` both ways |
| `video-texture.html` | A playing video through `importExternalTexture` (120 frames), `copyExternalImageToTexture` and `requestVideoFrameCallback` |
| `ml-transformers.html` | transformers.js (ONNX Runtime Web) with a 90 MB sentence-embedding model: WebAssembly as the reference, then WebGPU fp32 and fp16 compared with it; times logged. Needs the network, see below |

The comment at the top of each page lists its cases and what should be on
screen.

## On the desktop

WebGPU needs a secure context; `localhost` is one.

    cd port/webkit/test/webgpu && python3 -m http.server 8793

Then open `http://localhost:8793/index.html` in Chrome and read the page, or
the console for the RESULT lines.

`video-texture.html` takes its clip from `?src=<url>` and otherwise from
`../media/sync-1080p30-h264.mp4`, which is where the console has it (see
below). The repository holds no media: `port/webkit/test/media/make-media.sh`
makes the clips in `toolchain/wpe-webkit/media/`. On the desktop either serve
a directory that has this directory as `webgpu/` and the clips as `media/`,
or serve the clips with `tools/serve-media.py` (it allows cross-origin use)
and open `video-texture.html?src=http://localhost:8771/sync-1080p30-h264.mp4`.
With no clip the page logs `RESULT webgpu-video-texture video SKIP no video
source` and stops.

### Expected in desktop Chrome

Every case passes: `results/desktop-chrome-2026-11-13.log` is a full run
(Chrome 157, macOS, Metal). The parts that vary:

- `device.html` has one `feature-<name>` case per adapter feature, so its
  count depends on the GPU (35/35 there).
- `adapter.html` `request-adapter-fallback` passes with or without a
  fallback adapter and says which.
- `device.html` `second-request-device`: Chrome rejects (the adapter is
  used up), which is what the specification says.
- `errors.html` `mapped-at-creation-size`: Chrome throws a RangeError, as
  the specification says; `scope-out-of-memory`: Chrome reports the 2^50
  byte buffer as a validation error (over `maxBufferSize`).
- `todataurl.html` `later-frame-*`: Chrome still gives the pattern from
  `toDataURL` and `toBlob` and a cleared canvas from `createImageBitmap` and
  `drawImage`. Both are accepted.
- Frame rates and timings are logged, not judged.

## On the console

Upload the directory to `/data/ps5webkit/webgpu/` and open
`file:///data/ps5webkit/webgpu/index.html`, or serve it from the Mac and open
it through the link server.

Upload is FTP to the console's ftpsrv, the way
`port/webkit/test/media/make-media.sh --upload` and `tools/upload-title.sh`
do it (`. ./env.sh` sets `PS5_HOST`):

    FTP="ftp://$PS5_HOST:${PS5_FTP_PORT:-2121}"
    for f in port/webkit/test/webgpu/*.html port/webkit/test/webgpu/*.js; do
        curl -s --ftp-create-dirs -T "$f" "$FTP/data/ps5webkit/webgpu/$(basename "$f")"
    done

Open a page from the Mac with `tools/ps5open` (it sends the link to the
browser's link server, `http://$PS5_HOST:9040/`):

    tools/ps5open file:///data/ps5webkit/webgpu/index.html

To serve instead of uploading, run `python3 -m http.server` as above and open
`http://<mac>:8793/index.html` with `tools/ps5open`. Over plain http from
another machine the page is not a secure context, and a browser that follows
the specification then has no `navigator.gpu`: the pages say so (`no
navigator.gpu (not a secure context)`). Use the uploaded `file://` copy if
that happens.

`run-console.sh` opens pages one after another in the running browser and
prints their RESULT lines from the title's log (`TITLE` names the title that
must be running, `WAIT` the seconds a page gets, `QUERY` is added to the URL).

`ml-transformers.html` loads its library from cdn.jsdelivr.net and its model
from huggingface.co. From a `file:` page the model's fetches failed most of
the time on the console (reported as access control checks), so serve the
model from the Mac instead: put the hub's files under
`<dir>/Xenova/all-MiniLM-L6-v2/resolve/main/` (`config.json`,
`tokenizer.json`, `tokenizer_config.json`, `onnx/model.onnx`,
`onnx/model_fp16.onnx`), then

    port/webkit/test/webgpu/serve-models.py <dir> &
    WAIT=300 QUERY="models=http://<mac>:8765/" port/webkit/test/webgpu/run-console.sh ml-transformers

The video clip for `video-texture.html` is
`/data/ps5webkit/media/sync-1080p30-h264.mp4`, which
`port/webkit/test/media/make-media.sh --upload` puts there.

### What may fail early on a Dawn/Vulkan port

These lean on parts a first WebGPU bring-up often lacks; a FAIL here does not
mean the core is broken. `adapter.html`, `device.html`, `compute.html`,
`offscreen-readback.html` and `errors.html` need no canvas and no
presentation, so they are the ones to get green first.

- Canvas readback (`drawImage`, `toDataURL`, `toBlob`, `createImageBitmap` of
  a WebGPU canvas): all of `todataurl.html`, the pixel checks in
  `resize.html` and `two-canvases.html`, `first-frame-readback` in
  `triangle.html`, `centre-pixel` in `cube.html`. The `animation` cases of
  `triangle.html` and `cube.html` do not depend on it.
- `resize.html` `format-rgba16float`: a float swap chain and
  `toneMapping: extended`.
- `resize.html` `size-*`: the drawing buffer following `canvas.width/height`
  without a new `configure()` call (the failure message says whether
  reconfiguring would have helped).
- `video-texture.html`: `importExternalTexture` and `texture_external`
  (Dawn's multi-planar external textures), video frames reaching
  `copyExternalImageToTexture`, `requestVideoFrameCallback`. A video that
  does not play is a SKIP, not a FAIL.
- `copy-external-image.html`: each source kind is its own path;
  `offscreen-canvas-*` is a SKIP where there is no `OffscreenCanvas`.
- `two-canvases.html` and `resize.html` `device-change` need a second
  adapter and device in one page.
- `errors.html`: `getCompilationInfo()` line numbers, `GPUPipelineError`, and
  `uncaptured-error` depend on how the port carries Dawn's messages back to
  the page.
- `device.html` `default-limits` wants a default device to report exactly
  the specification's defaults, not the adapter's limits.
- From `file://`, a video or image in another directory may count as another
  origin, in which case `copyExternalImageToTexture` and
  `importExternalTexture` throw a SecurityError.
