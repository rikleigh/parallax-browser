// Shared helper of the WebGPU test pages in this directory (plain script, no
// modules: the pages load it with <script src="webgpu-test.js">). Everything
// hangs off the global T.
//
//   T.run('adapter', async () => { ... })   sets the suite (webgpu-adapter),
//                                           runs the page, prints the summary
//   T.test('case', async () => 'detail')    one case: PASS with the returned
//                                           detail, FAIL with what it threw
//   T.step(promise, 'name')                 a 10 s timeout on an awaited step
//   T.getAdapter() / T.getDevice()          null after one clear FAIL line
//                                           (no navigator.gpu, null adapter)
//
// Log lines: RESULT webgpu-<page> <case> PASS|FAIL|SKIP <why>, then
// RESULT webgpu-<page> summary PASS|FAIL <passed>/<cases>. Lines that are not
// results are logged as [webgpu-<page>] <text>.
'use strict';
const T = (() => {
  let suite = 'webgpu-unnamed';
  let out = null;
  const results = [];
  let skipped = 0;
  let summarised = false;
  const uncaptured = [];   // messages of uncapturederror events on devices made by getDevice

  function append(text, className) {
    if (!out) out = document.getElementById('out');
    if (!out) return;
    const div = document.createElement('div');
    div.textContent = text;
    if (className) div.className = className;
    out.appendChild(div);
  }

  function oneLine(text) {
    return String(text === undefined || text === null ? '' : text).replace(/\s+/g, ' ').trim();
  }

  function report(name, ok, why) {
    results.push(!!ok);
    const detail = oneLine(why);
    const line = 'RESULT ' + suite + ' ' + name + ' ' + (ok ? 'PASS' : 'FAIL') + (detail ? ' ' + detail : '');
    console.log(line);
    append(line, ok ? '' : 'fail');
  }

  function skip(name, why) {
    skipped++;
    const line = 'RESULT ' + suite + ' ' + name + ' SKIP ' + oneLine(why);
    console.log(line);
    append(line, 'skip');
  }

  function info(text) {
    const line = '[' + suite + '] ' + oneLine(text);
    console.log(line);
    append(line, 'info');
  }

  function summary() {
    if (summarised) return;
    summarised = true;
    const passed = results.filter(Boolean).length;
    const ok = passed === results.length;
    const line = 'RESULT ' + suite + ' summary ' + (ok ? 'PASS' : 'FAIL') + ' ' + passed + '/' + results.length +
      (skipped ? ' (' + skipped + ' skipped)' : '');
    console.log(line);
    append(line, ok ? '' : 'fail');
  }

  // Rejects with the step's name if the promise has not settled in time, so
  // a page never waits for ever on a promise the implementation drops.
  function step(promise, name, ms = 10000) {
    let timer;
    const timeout = new Promise((resolve, reject) => {
      timer = setTimeout(() => reject(new Error('timeout after ' + ms / 1000 + ' s: ' + name)), ms);
    });
    return Promise.race([Promise.resolve(promise), timeout]).finally(() => clearTimeout(timer));
  }

  function describe(e) {
    if (e && e.name && e.message !== undefined) return e.name + ': ' + e.message;
    return String(e);
  }

  // One case. fn returns the PASS detail (or nothing) and throws to FAIL.
  // With a device, validation errors raised while the case runs fail it too
  // (a validation error scope around the case).
  async function test(name, fn, device) {
    let scoped = false;
    try {
      if (device) { device.pushErrorScope('validation'); scoped = true; }
      const detail = await step(fn(), 'case ' + name, 60000);
      if (scoped) {
        scoped = false;
        const error = await step(device.popErrorScope(), 'popErrorScope');
        if (error) throw new Error('validation error: ' + error.message);
      }
      report(name, true, detail);
      return true;
    } catch (e) {
      if (scoped) device.popErrorScope().catch(() => {});
      report(name, false, describe(e));
      return false;
    }
  }

  function assert(condition, why) {
    if (!condition) throw new Error(why);
  }

  async function run(name, fn) {
    suite = 'webgpu-' + name;
    addEventListener('error', e => report('uncaught-exception', false, e.message));
    addEventListener('unhandledrejection', e => report('unhandled-rejection', false, describe(e.reason)));
    try {
      await fn();
    } catch (e) {
      report('exception', false, describe(e));
    }
    summary();
  }

  async function getAdapter(options, caseName = 'init') {
    if (!navigator.gpu) {
      report(caseName, false, 'no navigator.gpu' + (isSecureContext ? '' : ' (not a secure context)'));
      return null;
    }
    let adapter;
    try {
      adapter = await step(navigator.gpu.requestAdapter(options), 'requestAdapter');
    } catch (e) {
      report(caseName, false, 'requestAdapter: ' + describe(e));
      return null;
    }
    if (!adapter) {
      report(caseName, false, 'requestAdapter returned null');
      return null;
    }
    return adapter;
  }

  function watchDevice(device, label) {
    device.addEventListener('uncapturederror', e => {
      const message = (label ? label + ': ' : '') + (e.error && e.error.message);
      uncaptured.push(message);
      info('uncapturederror ' + message);
    });
    device.lost.then(lost => {
      if (lost.reason !== 'destroyed') info('device lost (' + lost.reason + '): ' + lost.message);
    });
  }

  // { adapter, device, format } or null after a FAIL line.
  async function getDevice(descriptor, adapterOptions, caseName = 'init') {
    const adapter = await getAdapter(adapterOptions, caseName);
    if (!adapter) return null;
    let device;
    try {
      device = await step(adapter.requestDevice(descriptor), 'requestDevice');
    } catch (e) {
      report(caseName, false, 'requestDevice: ' + describe(e));
      return null;
    }
    if (!device) {
      report(caseName, false, 'requestDevice returned ' + device);
      return null;
    }
    watchDevice(device, caseName === 'init' ? '' : caseName);
    return { adapter, device, format: navigator.gpu.getPreferredCanvasFormat() };
  }

  // Runs fn inside an error scope and returns the GPUError it raised (or null).
  async function errorScope(device, filter, fn) {
    device.pushErrorScope(filter);
    let thrown = null;
    try {
      await fn();
    } catch (e) {
      thrown = e;
    }
    const error = await step(device.popErrorScope(), 'popErrorScope(' + filter + ')');
    if (thrown) throw thrown;
    return error;
  }

  function errorKind(error) {
    if (!error) return 'none';
    if (typeof GPUValidationError !== 'undefined' && error instanceof GPUValidationError) return 'validation';
    if (typeof GPUOutOfMemoryError !== 'undefined' && error instanceof GPUOutOfMemoryError) return 'out-of-memory';
    if (typeof GPUInternalError !== 'undefined' && error instanceof GPUInternalError) return 'internal';
    return error.constructor ? error.constructor.name : typeof error;
  }

  function halfToFloat(h) {
    const sign = h & 0x8000 ? -1 : 1, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
    if (exponent === 0) return sign * Math.pow(2, -14) * (mantissa / 1024);
    if (exponent === 31) return mantissa ? NaN : sign * Infinity;
    return sign * Math.pow(2, exponent - 15) * (1 + mantissa / 1024);
  }

  // Copies one mip level of an rgba8unorm, bgra8unorm or rgba16float texture
  // (usage COPY_SRC) into a buffer, maps it and returns { width, height, data }
  // with tightly packed RGBA: bytes for the 8-bit formats, floats for
  // rgba16float.
  async function readTexture(device, texture, mipLevel = 0) {
    const width = Math.max(1, texture.width >> mipLevel), height = Math.max(1, texture.height >> mipLevel);
    const format = texture.format;
    if (!['rgba8unorm', 'bgra8unorm', 'rgba8unorm-srgb', 'bgra8unorm-srgb', 'rgba16float'].includes(format))
      throw new Error('readTexture: format ' + format + ' not handled');
    const bytesPerPixel = format === 'rgba16float' ? 8 : 4;
    const bytesPerRow = Math.ceil(width * bytesPerPixel / 256) * 256;
    const buffer = device.createBuffer({ size: bytesPerRow * height, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
    const encoder = device.createCommandEncoder();
    encoder.copyTextureToBuffer({ texture, mipLevel }, { buffer, bytesPerRow, rowsPerImage: height }, { width, height });
    device.queue.submit([encoder.finish()]);
    await step(buffer.mapAsync(GPUMapMode.READ), 'mapAsync (texture readback)');
    const mapped = buffer.getMappedRange();
    let data;
    if (bytesPerPixel === 4) {
      const source = new Uint8Array(mapped);
      data = new Uint8Array(width * height * 4);
      for (let y = 0; y < height; y++)
        data.set(source.subarray(y * bytesPerRow, y * bytesPerRow + width * 4), y * width * 4);
      if (format.startsWith('bgra')) {
        for (let i = 0; i < data.length; i += 4) {
          const b = data[i];
          data[i] = data[i + 2];
          data[i + 2] = b;
        }
      }
    } else {
      const source = new Uint16Array(mapped);
      data = new Float32Array(width * height * 4);
      for (let y = 0; y < height; y++)
        for (let i = 0; i < width * 4; i++)
          data[y * width * 4 + i] = halfToFloat(source[y * bytesPerRow / 2 + i]);
    }
    buffer.unmap();
    buffer.destroy();
    return { width, height, data };
  }

  // What a canvas (or image, bitmap, video) shows, as ImageData, through a 2D
  // canvas's drawImage.
  function snapshot(source, width, height) {
    const canvas = document.createElement('canvas');
    canvas.width = width;
    canvas.height = height;
    const context = canvas.getContext('2d', { willReadFrequently: true });
    context.drawImage(source, 0, 0, width, height);
    return context.getImageData(0, 0, width, height);
  }

  function pixel(image, x, y) {
    const i = (y * image.width + x) * 4;
    return [image.data[i], image.data[i + 1], image.data[i + 2], image.data[i + 3]];
  }

  function near(got, want, tolerance = 2) {
    for (let i = 0; i < want.length; i++)
      if (!(Math.abs(got[i] - want[i]) <= tolerance)) return false;
    return true;
  }

  function show(values) {
    return Array.from(values).map(v => Number.isInteger(v) ? v : v.toFixed(3)).join(',');
  }

  // Throws unless the pixel at (x, y) is `want` within the tolerance (2 of
  // 255 for bytes; give a float tolerance for float images).
  function expectPixel(image, x, y, want, label, tolerance = 2) {
    const got = pixel(image, x, y);
    if (!near(got, want, tolerance))
      throw new Error((label ? label + ' ' : '') + 'pixel (' + x + ',' + y + ') is ' + show(got) + ', expected ' + show(want));
  }

  // A 2x2 pattern used by the canvas and copy pages: one colour per
  // quadrant, all different, so a flip or a channel swap shows.
  const QUADRANTS = {
    topLeft: [255, 0, 0, 255], topRight: [0, 255, 0, 255],
    bottomLeft: [0, 0, 255, 255], bottomRight: [255, 255, 255, 255],
  };

  // '' if the image shows the quadrant pattern (flipped: top and bottom
  // swapped), otherwise what was wrong.
  function quadrantsMismatch(image, flipped = false, tolerance = 2) {
    const w = image.width, h = image.height;
    const left = w >> 2, right = w - 1 - (w >> 2), top = h >> 2, bottom = h - 1 - (h >> 2);
    const want = [
      ['top left', left, top, flipped ? QUADRANTS.bottomLeft : QUADRANTS.topLeft],
      ['top right', right, top, flipped ? QUADRANTS.bottomRight : QUADRANTS.topRight],
      ['bottom left', left, bottom, flipped ? QUADRANTS.topLeft : QUADRANTS.bottomLeft],
      ['bottom right', right, bottom, flipped ? QUADRANTS.topRight : QUADRANTS.bottomRight],
    ];
    const wrong = [];
    for (const [name, x, y, colour] of want) {
      const got = pixel(image, x, y);
      if (!near(got, colour, tolerance)) wrong.push(name + ' ' + show(got) + ' expected ' + show(colour));
    }
    return wrong.join('; ');
  }

  // Paints the quadrant pattern into a 2D context.
  function paintQuadrants(context, width, height) {
    const css = c => 'rgb(' + c[0] + ',' + c[1] + ',' + c[2] + ')';
    const w = width / 2, h = height / 2;
    context.fillStyle = css(QUADRANTS.topLeft); context.fillRect(0, 0, w, h);
    context.fillStyle = css(QUADRANTS.topRight); context.fillRect(w, 0, w, h);
    context.fillStyle = css(QUADRANTS.bottomLeft); context.fillRect(0, h, w, h);
    context.fillStyle = css(QUADRANTS.bottomRight); context.fillRect(w, h, w, h);
  }

  function nextFrame() {
    return step(new Promise(resolve => requestAnimationFrame(resolve)), 'requestAnimationFrame', 10000);
  }

  // Calls draw(timeMs, frameIndex) on every animation frame and keeps going.
  // .measured resolves after `frames` frames with { fps, jsMs } (frames per
  // second between the first and the last of them, mean time inside draw),
  // and rejects if draw throws or the frames have not come within `ms`.
  function animate(draw, frames, ms = 20000) {
    const handle = { frames: 0, stopped: false, stop() { handle.stopped = true; } };
    handle.measured = new Promise((resolve, reject) => {
      let first = 0, js = 0;
      const timer = setTimeout(() => reject(new Error('timeout after ' + ms / 1000 + ' s: ' + handle.frames + ' of ' + frames + ' frames drawn')), ms);
      function tick(now) {
        if (handle.stopped) return;
        const before = performance.now();
        try {
          draw(now, handle.frames);
        } catch (e) {
          handle.stopped = true;
          clearTimeout(timer);
          info('draw threw in frame ' + handle.frames + ': ' + describe(e));
          reject(e);
          return;
        }
        const after = performance.now();
        if (handle.frames === 0) first = before;
        if (handle.frames < frames) js += after - before;
        if (handle.frames === frames) {
          clearTimeout(timer);
          resolve({ fps: frames * 1000 / (before - first), jsMs: js / frames });
        }
        handle.frames++;
        requestAnimationFrame(tick);
      }
      requestAnimationFrame(tick);
    });
    return handle;
  }

  // The default limits of the WebGPU specification (a device made without
  // requiredLimits has exactly these). 'min' marks the two alignment limits,
  // where smaller is better.
  const DEFAULT_LIMITS = {
    maxTextureDimension1D: 8192, maxTextureDimension2D: 8192, maxTextureDimension3D: 2048,
    maxTextureArrayLayers: 256, maxBindGroups: 4, maxBindGroupsPlusVertexBuffers: 24,
    maxBindingsPerBindGroup: 1000, maxDynamicUniformBuffersPerPipelineLayout: 8,
    maxDynamicStorageBuffersPerPipelineLayout: 4, maxSampledTexturesPerShaderStage: 16,
    maxSamplersPerShaderStage: 16, maxStorageBuffersPerShaderStage: 8,
    maxStorageTexturesPerShaderStage: 4, maxUniformBuffersPerShaderStage: 12,
    maxUniformBufferBindingSize: 65536, maxStorageBufferBindingSize: 134217728,
    minUniformBufferOffsetAlignment: 256, minStorageBufferOffsetAlignment: 256,
    maxVertexBuffers: 8, maxBufferSize: 268435456, maxVertexAttributes: 16,
    maxVertexBufferArrayStride: 2048, maxInterStageShaderVariables: 16, maxColorAttachments: 8,
    maxColorAttachmentBytesPerSample: 32, maxComputeWorkgroupStorageSize: 16384,
    maxComputeInvocationsPerWorkgroup: 256, maxComputeWorkgroupSizeX: 256,
    maxComputeWorkgroupSizeY: 256, maxComputeWorkgroupSizeZ: 64,
    maxComputeWorkgroupsPerDimension: 65535,
  };
  const isMinLimit = name => name.startsWith('min');

  function limitNames(limits) {
    const names = [];
    for (const name in limits)
      if (typeof limits[name] === 'number') names.push(name);
    return names;
  }

  return {
    run, test, step, report, skip, info, summary, assert, describe,
    getAdapter, getDevice, watchDevice, errorScope, errorKind, uncaptured,
    readTexture, snapshot, pixel, near, show, expectPixel,
    QUADRANTS, quadrantsMismatch, paintQuadrants,
    nextFrame, animate, DEFAULT_LIMITS, isMinLimit, limitNames,
  };
})();
