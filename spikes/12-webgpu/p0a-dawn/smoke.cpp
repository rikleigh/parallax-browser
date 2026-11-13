// Spike 12 P0a: link smoke test for the cross-built Dawn (libwebgpu_dawn.a).
//
// Creates a dawn::native::Instance, enumerates adapters (with the null driver
// hooks below, only Dawn's null backend answers), creates a device on the
// null adapter and puts two WGSL modules through Tint: one valid (then used
// in a compute pipeline), one with an error (expects a Tint diagnostic).
//
// For the PS5 the pass criterion is that this links with no undefined
// symbols (build.sh); the same source runs on the macOS host (build.sh host).
#include <dawn/native/DawnNative.h>
#include <webgpu/webgpu_cpp.h>

#include <cstdio>
#include <string>

// The host program's side of port/deps/patches/dawn: no drivers here.
#if defined(__PROSPERO__)
extern "C" void* dawn_ps5_vk_get_instance_proc_addr(void) {
    return nullptr;
}
extern "C" void* dawn_ps5_egl_get_proc_address(const char*) {
    return nullptr;
}
#endif

namespace {

std::string Str(wgpu::StringView s) {
    return s.data ? std::string(s.data, s.length) : std::string();
}

const char* BackendName(wgpu::BackendType t) {
    switch (t) {
        case wgpu::BackendType::Null: return "Null";
        case wgpu::BackendType::Vulkan: return "Vulkan";
        case wgpu::BackendType::OpenGLES: return "OpenGLES";
        case wgpu::BackendType::OpenGL: return "OpenGL";
        case wgpu::BackendType::Metal: return "Metal";
        default: return "other";
    }
}

int g_errors = 0;       // uncaptured device errors
int g_diagnostics = 0;  // Tint messages of the last GetCompilationInfo

wgpu::ShaderModule Compile(const wgpu::Instance& instance, const wgpu::Device& device,
                           const char* label, const char* code) {
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = code;
    wgpu::ShaderModuleDescriptor desc{};
    desc.nextInChain = &wgsl;
    desc.label = label;
    wgpu::ShaderModule module = device.CreateShaderModule(&desc);
    g_diagnostics = 0;
    module.GetCompilationInfo(
        wgpu::CallbackMode::AllowProcessEvents,
        [label](wgpu::CompilationInfoRequestStatus status, const wgpu::CompilationInfo* info) {
            if (status != wgpu::CompilationInfoRequestStatus::Success || !info) {
                return;
            }
            for (size_t i = 0; i < info->messageCount; i++) {
                const wgpu::CompilationMessage& m = info->messages[i];
                printf("  tint [%s] %u:%u %s\n", label, unsigned(m.lineNum), unsigned(m.linePos),
                       Str(m.message).c_str());
                g_diagnostics++;
            }
        });
    instance.ProcessEvents();
    return module;
}

}  // namespace

int main() {
    wgpu::InstanceDescriptor instanceDesc{};
    dawn::native::Instance native(&instanceDesc);
    wgpu::Instance instance(native.Get());

    // Every backend's discovery runs: Vulkan and OpenGL ES fail here (no
    // driver) and Dawn carries on.
    dawn::native::Adapter nullAdapter;
    for (wgpu::FeatureLevel level : {wgpu::FeatureLevel::Core, wgpu::FeatureLevel::Compatibility}) {
        wgpu::RequestAdapterOptions options{};
        options.featureLevel = level;
        for (dawn::native::Adapter& a : native.EnumerateAdapters(&options)) {
            wgpu::AdapterInfo info{};
            wgpu::Adapter(a.Get()).GetInfo(&info);
            printf("adapter (%s): backend=%s device='%s'\n",
                   level == wgpu::FeatureLevel::Core ? "core" : "compat",
                   BackendName(info.backendType), Str(info.device).c_str());
            if (info.backendType == wgpu::BackendType::Null && !nullAdapter) {
                nullAdapter = a;
            }
        }
    }
    if (!nullAdapter) {
        printf("FAIL: no null adapter\n");
        return 1;
    }

    wgpu::DeviceDescriptor deviceDesc{};
    deviceDesc.SetUncapturedErrorCallback(
        [](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message) {
            printf("  device error: %s\n", Str(message).c_str());
            g_errors++;
        });
    wgpu::Device device = wgpu::Device::Acquire(nullAdapter.CreateDevice(&deviceDesc));
    if (!device) {
        printf("FAIL: no device\n");
        return 1;
    }

    wgpu::ShaderModule good = Compile(instance, device, "good", R"(
        @group(0) @binding(0) var<storage, read_write> data : array<u32>;
        @compute @workgroup_size(4)
        fn main(@builtin(global_invocation_id) id : vec3u) {
            data[id.x] = data[id.x] * 2u + u32(sqrt(f32(id.x)));
        })");
    int goodDiagnostics = g_diagnostics, goodErrors = g_errors;

    wgpu::ComputePipelineDescriptor pipelineDesc{};
    pipelineDesc.compute.module = good;
    pipelineDesc.compute.entryPoint = "main";
    wgpu::ComputePipeline pipeline = device.CreateComputePipeline(&pipelineDesc);
    instance.ProcessEvents();
    int pipelineErrors = g_errors - goodErrors;

    Compile(instance, device, "bad", "fn f() -> u32 { return 1.5; }");
    int badDiagnostics = g_diagnostics;

    bool ok = goodDiagnostics == 0 && goodErrors == 0 && pipelineErrors == 0 && pipeline &&
              badDiagnostics > 0;
    printf("good module: %d diagnostics, %d errors; pipeline: %d errors; bad module: %d diagnostics\n",
           goodDiagnostics, goodErrors, pipelineErrors, badDiagnostics);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
