// Spike 12 P1: Dawn (Vulkan backend on RADV) in a title beside ps5-opengl.
// WGSL triangle and compute through Tint, MapAsync readback, and the frame
// shown through the compositor's path: CopyTextureToBuffer, MapAsync, one
// memcpy into the GL persistent SSBO, the SSBO -> texture pass (P0c's
// fallback form: Dawn has no host-pointer import).
#include <dawn/native/DawnNative.h>
#include <webgpu/webgpu_cpp.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

extern "C" {
#include "shared.h"
void p1_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void p1_result(const char* test, int pass, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
double p1_now_ms(void);
void p1_mem_line(const char* when);
void* vk_icdGetInstanceProcAddr(void* instance, const char* name);

// port/deps/patches/dawn: the title's drivers.
void* dawn_ps5_vk_get_instance_proc_addr(void) { return reinterpret_cast<void*>(&vk_icdGetInstanceProcAddr); }
void* dawn_ps5_egl_get_proc_address(const char*) { return nullptr; } // Route V: no GL backend
void dawn_run(void);
}

namespace {

std::string Str(wgpu::StringView s) { return s.data ? std::string(s.data, s.length) : std::string(); }

int g_errors;
bool g_lost;
wgpu::Instance g_instance;

// Runs Dawn's callbacks until `done`; false after timeoutMs.
bool Pump(const bool& done, double timeoutMs = 5000) {
    const double t0 = p1_now_ms();
    while (!done) {
        g_instance.ProcessEvents();
        if (done)
            break;
        if (p1_now_ms() - t0 > timeoutMs)
            return false;
        usleep(50);
    }
    return true;
}

bool MapRead(const wgpu::Buffer& buffer, uint64_t size) {
    bool done = false, ok = false;
    buffer.MapAsync(wgpu::MapMode::Read, 0, size, wgpu::CallbackMode::AllowProcessEvents,
                    [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
                        ok = status == wgpu::MapAsyncStatus::Success;
                        done = true;
                    });
    return Pump(done) && ok;
}

wgpu::ShaderModule Module(const wgpu::Device& device, const char* code) {
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = code;
    wgpu::ShaderModuleDescriptor desc{};
    desc.nextInChain = &wgsl;
    return device.CreateShaderModule(&desc);
}

const char* kTriangle = R"(
struct Params { colour : vec4f }
@group(0) @binding(0) var<uniform> params : Params;
@vertex fn vs(@builtin(vertex_index) i : u32) -> @builtin(position) vec4f {
    var p = array<vec2f, 3>(vec2f(0.0, 0.8), vec2f(0.8, -0.8), vec2f(-0.8, -0.8));
    return vec4f(p[i], 0.0, 1.0);
}
@fragment fn fs() -> @location(0) vec4f { return params.colour; }
)";

const char* kCompute = R"(
@group(0) @binding(0) var<storage, read_write> values : array<u32>;
@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id : vec3u) {
    values[id.x] = id.x * 3u + 7u;
}
)";

// Something nearer a real page's shader, for a compile time and the stack:
// loops, a struct, matrices, builtins, a texture and a sampler.
const char* kBig = R"(
struct Light { position : vec3f, colour : vec3f, range : f32 }
struct Scene { viewProj : mat4x4f, eye : vec3f, time : f32, lights : array<Light, 8>, count : u32 }
@group(0) @binding(0) var<uniform> scene : Scene;
@group(0) @binding(1) var albedo : texture_2d<f32>;
@group(0) @binding(2) var samp : sampler;
struct VOut { @builtin(position) pos : vec4f, @location(0) world : vec3f, @location(1) normal : vec3f, @location(2) uv : vec2f }
fn hash(p : vec3f) -> f32 { return fract(sin(dot(p, vec3f(12.9898, 78.233, 37.719))) * 43758.5453); }
fn noise(p : vec3f) -> f32 {
    let i = floor(p); let f = fract(p); let u = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash(i), hash(i + vec3f(1, 0, 0)), u.x), mix(hash(i + vec3f(0, 1, 0)), hash(i + vec3f(1, 1, 0)), u.x), u.y),
               mix(mix(hash(i + vec3f(0, 0, 1)), hash(i + vec3f(1, 0, 1)), u.x), mix(hash(i + vec3f(0, 1, 1)), hash(i + vec3f(1, 1, 1)), u.x), u.y), u.z);
}
fn fbm(p : vec3f) -> f32 { var v = 0.0; var a = 0.5; var q = p; for (var i = 0; i < 5; i++) { v += a * noise(q); q = q * 2.02 + vec3f(1.7); a *= 0.5; } return v; }
@vertex fn vs(@location(0) position : vec3f, @location(1) normal : vec3f, @location(2) uv : vec2f, @builtin(instance_index) inst : u32) -> VOut {
    var o : VOut;
    let wobble = vec3f(sin(scene.time + f32(inst)), cos(scene.time * 0.7), 0.0) * 0.1;
    o.world = position + wobble; o.normal = normal; o.uv = uv;
    o.pos = scene.viewProj * vec4f(o.world, 1.0);
    return o;
}
@fragment fn fs(v : VOut) -> @location(0) vec4f {
    let base = textureSample(albedo, samp, v.uv).rgb * (0.6 + 0.4 * fbm(v.world * 4.0));
    let n = normalize(v.normal); let view = normalize(scene.eye - v.world);
    var lit = vec3f(0.03) * base;
    for (var i = 0u; i < min(scene.count, 8u); i++) {
        let l = scene.lights[i]; let d = l.position - v.world; let dist = length(d); let dir = d / max(dist, 1e-4);
        let att = clamp(1.0 - dist / l.range, 0.0, 1.0);
        let h = normalize(dir + view);
        lit += (base * max(dot(n, dir), 0.0) + pow(max(dot(n, h), 0.0), 32.0)) * l.colour * att * att;
    }
    return vec4f(pow(lit / (lit + vec3f(1.0)), vec3f(1.0 / 2.2)), 1.0);
}
)";

bool Near(const uint8_t* px, int r, int g, int b) {
    return abs(px[0] - r) <= 2 && abs(px[1] - g) <= 2 && abs(px[2] - b) <= 2;
}

} // namespace

void dawn_run(void) {
    p1_mem_line("before Dawn");
    double t0 = p1_now_ms();
    wgpu::InstanceDescriptor instanceDesc{};
    dawn::native::Instance native(&instanceDesc);
    g_instance = wgpu::Instance(native.Get());

    dawn::native::Adapter chosen;
    wgpu::AdapterInfo info{};
    wgpu::RequestAdapterOptions options{};
    options.featureLevel = wgpu::FeatureLevel::Core;
    options.backendType = wgpu::BackendType::Vulkan;
    for (dawn::native::Adapter& a : native.EnumerateAdapters(&options)) {
        wgpu::AdapterInfo i{};
        wgpu::Adapter(a.Get()).GetInfo(&i);
        p1_log("adapter: backend %d type %d \"%s\" \"%s\"", int(i.backendType), int(i.adapterType), Str(i.device).c_str(), Str(i.description).c_str());
        if (i.backendType == wgpu::BackendType::Vulkan && !chosen) {
            chosen = a;
            wgpu::Adapter(a.Get()).GetInfo(&info);
        }
    }
    if (!chosen) {
        p1_result("dawn-adapter", 0, "no core Vulkan adapter (see stderr lines in the log)");
        return;
    }
    wgpu::Adapter adapter(chosen.Get());
    wgpu::SupportedFeatures features{};
    adapter.GetFeatures(&features);
    std::string names;
    bool f16 = false, bc = false, ts = false, f32filter = false;
    for (size_t i = 0; i < features.featureCount; i++) {
        f16 |= features.features[i] == wgpu::FeatureName::ShaderF16;
        bc |= features.features[i] == wgpu::FeatureName::TextureCompressionBC;
        ts |= features.features[i] == wgpu::FeatureName::TimestampQuery;
        f32filter |= features.features[i] == wgpu::FeatureName::Float32Filterable;
    }
    wgpu::Limits limits{};
    adapter.GetLimits(&limits);
    p1_result("dawn-adapter", 1, "\"%s\" (%s) core, %zu features (shader-f16 %d, texture-compression-bc %d, timestamp-query %d, float32-filterable %d); "
              "maxTextureDimension2D %u, maxBufferSize %llu MB, maxStorageBufferBindingSize %llu MB, maxComputeWorkgroupsPerDimension %u; %.0f ms",
              Str(info.device).c_str(), Str(info.description).c_str(), features.featureCount, f16, bc, ts, f32filter, limits.maxTextureDimension2D,
              (unsigned long long)(limits.maxBufferSize >> 20), (unsigned long long)(limits.maxStorageBufferBindingSize >> 20),
              limits.maxComputeWorkgroupsPerDimension, p1_now_ms() - t0);

    t0 = p1_now_ms();
    wgpu::DeviceDescriptor deviceDesc{};
    deviceDesc.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
        p1_log("dawn device error %d: %s", int(type), Str(message).c_str());
        g_errors++;
    });
    deviceDesc.SetDeviceLostCallback(wgpu::CallbackMode::AllowProcessEvents, [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
        p1_log("dawn device lost (%d): %s", int(reason), Str(message).c_str());
        g_lost = true;
    });
    wgpu::Device device = wgpu::Device::Acquire(chosen.CreateDevice(&deviceDesc));
    if (!device) {
        p1_result("dawn-device", 0, "CreateDevice returned null");
        return;
    }
    wgpu::Queue queue = device.GetQueue();
    p1_result("dawn-device", 1, "device in %.0f ms", p1_now_ms() - t0);
    p1_mem_line("after Dawn device");

    // -- triangle ---------------------------------------------------------------
    const uint32_t W = BRIDGE_W, H = BRIDGE_H;
    t0 = p1_now_ms();
    wgpu::ShaderModule triModule = Module(device, kTriangle);
    const double moduleMs = p1_now_ms() - t0;
    wgpu::ColorTargetState target{};
    target.format = wgpu::TextureFormat::RGBA8Unorm;
    wgpu::FragmentState fragment{};
    fragment.module = triModule;
    fragment.entryPoint = "fs";
    fragment.targetCount = 1;
    fragment.targets = &target;
    wgpu::RenderPipelineDescriptor rpd{};
    rpd.vertex.module = triModule;
    rpd.vertex.entryPoint = "vs";
    rpd.fragment = &fragment;
    t0 = p1_now_ms();
    wgpu::RenderPipeline pipeline = device.CreateRenderPipeline(&rpd);
    const double pipelineMs = p1_now_ms() - t0;

    wgpu::TextureDescriptor td{};
    td.size = {W, H, 1};
    td.format = wgpu::TextureFormat::RGBA8Unorm;
    td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
    wgpu::Texture texture = device.CreateTexture(&td);
    wgpu::TextureView view = texture.CreateView();
    wgpu::BufferDescriptor ubd{};
    ubd.size = 16;
    ubd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer uniform = device.CreateBuffer(&ubd);
    wgpu::BindGroupEntry bge{};
    bge.binding = 0;
    bge.buffer = uniform;
    bge.size = 16;
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = pipeline.GetBindGroupLayout(0);
    bgd.entryCount = 1;
    bgd.entries = &bge;
    wgpu::BindGroup bindGroup = device.CreateBindGroup(&bgd);
    wgpu::BufferDescriptor rbd{};
    rbd.size = BRIDGE_BYTES;
    rbd.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer readback = device.CreateBuffer(&rbd);

    auto frame = [&](int n, int rgb[3]) {
        rgb[0] = (n * 8) & 255;
        rgb[1] = 255 - ((n * 5) & 255);
        rgb[2] = 64;
        const float colour[4] = {rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f, 1};
        queue.WriteBuffer(uniform, 0, colour, sizeof colour);
        wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
        wgpu::RenderPassColorAttachment att{};
        att.view = view;
        att.loadOp = wgpu::LoadOp::Clear;
        att.storeOp = wgpu::StoreOp::Store;
        att.clearValue = {0, 0, 1, 1};
        wgpu::RenderPassDescriptor rp{};
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &att;
        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rp);
        pass.SetPipeline(pipeline);
        pass.SetBindGroup(0, bindGroup);
        pass.Draw(3);
        pass.End();
        wgpu::TexelCopyTextureInfo src{};
        src.texture = texture;
        wgpu::TexelCopyBufferInfo dst{};
        dst.buffer = readback;
        dst.layout.bytesPerRow = W * 4;
        dst.layout.rowsPerImage = H;
        wgpu::Extent3D extent = {W, H, 1};
        encoder.CopyTextureToBuffer(&src, &dst, &extent);
        wgpu::CommandBuffer commands = encoder.Finish();
        queue.Submit(1, &commands);
    };

    int rgb[3];
    t0 = p1_now_ms();
    frame(1, rgb);
    bool mapped = MapRead(readback, BRIDGE_BYTES);
    const double firstFrameMs = p1_now_ms() - t0;
    if (mapped) {
        const uint8_t* px = static_cast<const uint8_t*>(readback.GetConstMappedRange(0, BRIDGE_BYTES));
        const uint8_t* centre = px + (size_t(H / 2) * W + W / 2) * 4;
        const uint8_t* corner = px + (size_t(2) * W + 2) * 4;
        const bool ok = Near(centre, rgb[0], rgb[1], rgb[2]) && Near(corner, 0, 0, 255) && !g_errors;
        p1_result("dawn-triangle", ok, "1920x1080 WGSL triangle: centre %u,%u,%u (want %d,%d,%d) corner %u,%u,%u; module %.1f ms, pipeline %.1f ms, "
                  "first frame + readback %.1f ms, %d device errors", centre[0], centre[1], centre[2], rgb[0], rgb[1], rgb[2], corner[0], corner[1], corner[2],
                  moduleMs, pipelineMs, firstFrameMs, g_errors);
        readback.Unmap();
    } else
        p1_result("dawn-triangle", 0, "MapAsync did not succeed (%d device errors, lost %d)", g_errors, g_lost);

    // -- compute ----------------------------------------------------------------
    {
        enum { VALUES = 4096 };
        t0 = p1_now_ms();
        wgpu::ComputePipelineDescriptor cpd{};
        cpd.compute.module = Module(device, kCompute);
        cpd.compute.entryPoint = "main";
        wgpu::ComputePipeline cp = device.CreateComputePipeline(&cpd);
        const double cpMs = p1_now_ms() - t0;
        wgpu::BufferDescriptor sbd{};
        sbd.size = VALUES * 4;
        sbd.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc;
        wgpu::Buffer storage = device.CreateBuffer(&sbd);
        wgpu::BufferDescriptor mbd{};
        mbd.size = VALUES * 4;
        mbd.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
        wgpu::Buffer out = device.CreateBuffer(&mbd);
        wgpu::BindGroupEntry e{};
        e.binding = 0;
        e.buffer = storage;
        e.size = VALUES * 4;
        wgpu::BindGroupDescriptor d{};
        d.layout = cp.GetBindGroupLayout(0);
        d.entryCount = 1;
        d.entries = &e;
        wgpu::BindGroup bg = device.CreateBindGroup(&d);
        t0 = p1_now_ms();
        wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
        wgpu::ComputePassEncoder pass = encoder.BeginComputePass();
        pass.SetPipeline(cp);
        pass.SetBindGroup(0, bg);
        pass.DispatchWorkgroups(VALUES / 64);
        pass.End();
        encoder.CopyBufferToBuffer(storage, 0, out, 0, VALUES * 4);
        wgpu::CommandBuffer commands = encoder.Finish();
        queue.Submit(1, &commands);
        unsigned bad = 0;
        const bool ok = MapRead(out, VALUES * 4);
        if (ok) {
            const uint32_t* v = static_cast<const uint32_t*>(out.GetConstMappedRange(0, VALUES * 4));
            for (unsigned i = 0; i < VALUES; i++)
                bad += v[i] != i * 3u + 7u;
            out.Unmap();
        }
        p1_result("dawn-compute", ok && !bad && !g_errors, "%d values, %u wrong; module + pipeline %.1f ms, dispatch + copy + MapAsync %.2f ms, %d device errors",
                  VALUES, bad, cpMs, p1_now_ms() - t0, g_errors);
    }

    // -- a larger shader: compile time ---------------------------------------------
    {
        const int before = g_errors;
        t0 = p1_now_ms();
        wgpu::ShaderModule big = Module(device, kBig);
        const double modMs = p1_now_ms() - t0;
        wgpu::VertexAttribute attrs[3] = {};
        attrs[0].format = wgpu::VertexFormat::Float32x3; attrs[0].offset = 0; attrs[0].shaderLocation = 0;
        attrs[1].format = wgpu::VertexFormat::Float32x3; attrs[1].offset = 12; attrs[1].shaderLocation = 1;
        attrs[2].format = wgpu::VertexFormat::Float32x2; attrs[2].offset = 24; attrs[2].shaderLocation = 2;
        wgpu::VertexBufferLayout vbl{};
        vbl.arrayStride = 32;
        vbl.attributeCount = 3;
        vbl.attributes = attrs;
        wgpu::ColorTargetState t{};
        t.format = wgpu::TextureFormat::RGBA8Unorm;
        wgpu::FragmentState f{};
        f.module = big;
        f.entryPoint = "fs";
        f.targetCount = 1;
        f.targets = &t;
        wgpu::RenderPipelineDescriptor d{};
        d.vertex.module = big;
        d.vertex.entryPoint = "vs";
        d.vertex.bufferCount = 1;
        d.vertex.buffers = &vbl;
        d.fragment = &f;
        t0 = p1_now_ms();
        wgpu::RenderPipeline p = device.CreateRenderPipeline(&d);
        g_instance.ProcessEvents();
        const double pipeMs = p1_now_ms() - t0;
        p1_result("dawn-shader-compile", p && g_errors == before, "a lit, textured, 5-octave-noise shader (vs + fs): WGSL module %.1f ms, pipeline (Tint to SPIR-V, then RADV) %.1f ms cold",
                  modMs, pipeMs);
    }
    p1_mem_line("after pipelines");

    // -- frames to the compositor's path ---------------------------------------------
    g_gl.want_bridge = 1;
    for (int i = 0; i < 300 && !g_gl.bridge_state; i++)
        usleep(10 * 1000);
    if (g_gl.bridge_state != 1) {
        p1_result("dawn-present", 0, "GL persistent buffer: state %d", int(g_gl.bridge_state));
        return;
    }
    g_gl.offset_words = 0;
    auto present = [&](int n, double* dawnMs, double* copyMs) {
        int c[3];
        double a = p1_now_ms();
        frame(n, c);
        if (!MapRead(readback, BRIDGE_BYTES))
            return false;
        double b = p1_now_ms();
        memcpy(g_gl.map, readback.GetConstMappedRange(0, BRIDGE_BYTES), BRIDGE_BYTES);
        readback.Unmap();
        if (dawnMs) *dawnMs += b - a;
        if (copyMs) *copyMs += p1_now_ms() - b;
        g_gl.posted = n;
        for (int i = 0; i < 20000 && g_gl.drawn != n; i++)
            usleep(100);
        memcpy(rgb, c, sizeof c);
        return g_gl.drawn == n;
    };
    bool ok = true;
    g_gl.verify = 1;
    for (int n = 2; n <= 9 && ok; n++)
        ok = present(n, nullptr, nullptr) && Near(g_gl.centre, rgb[0], rgb[1], rgb[2]) && Near(g_gl.corner, 0, 0, 255);
    g_gl.verify = 0;
    if (!ok) {
        p1_result("dawn-present", 0, "GL texture centre %u,%u,%u want %d,%d,%d corner %u,%u,%u", g_gl.centre[0], g_gl.centre[1], g_gl.centre[2],
                  rgb[0], rgb[1], rgb[2], g_gl.corner[0], g_gl.corner[1], g_gl.corner[2]);
        return;
    }
    g_gl.pass_ms_sum = 0;
    g_gl.pass_count = 0;
    const int N = 600;
    double dawnMs = 0, copyMs = 0;
    const unsigned long glFrames = g_gl.frames;
    const double all0 = p1_now_ms();
    for (int n = 0; n < N && ok; n++)
        ok = present(100 + n, &dawnMs, &copyMs);
    const double all = p1_now_ms() - all0;
    p1_result("dawn-present", ok && !g_errors && !g_lost, "8 frames right in the GL texture; %d frames of 1920x1080: Dawn draw + CopyTextureToBuffer + MapAsync %.2f ms, "
              "memcpy to the GL buffer %.2f ms, GL SSBO pass %.2f ms CPU: %.2f ms a frame on the CPU; %.1f frames/s end to end, GL presented %lu; %d device errors",
              N, dawnMs / N, copyMs / N, g_gl.pass_count ? g_gl.pass_ms_sum / g_gl.pass_count : 0,
              (dawnMs + copyMs) / N + (g_gl.pass_count ? g_gl.pass_ms_sum / g_gl.pass_count : 0), N * 1000.0 / all, g_gl.frames - glFrames, g_errors);
    p1_mem_line("after 600 frames");

    device.Destroy();
    g_instance.ProcessEvents();
    g_instance = nullptr;
}
