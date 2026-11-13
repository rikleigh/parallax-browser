// Spike 12 P2: the behaviour of Dawn that Source/WebGPU/WebGPUDawn relies on, checked on the host
// against the same Dawn source built with the null backend (spikes/12-webgpu/p0a-dawn/build.sh host).
// It does not test the backend itself, which needs WTF built for the host.
#include <dawn/native/DawnNative.h>
#include <webgpu/webgpu_cpp.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures;
#define CHECK(condition, what) do { bool ok = (condition); printf("%s  %s\n", ok ? "ok  " : "FAIL", what); g_failures += !ok; } while (0)

struct ErrorLog {
    std::vector<wgpu::ErrorType> uncaptured;
    int lost = 0;
    wgpu::DeviceLostReason lostReason {};
};

int main()
{
    wgpu::InstanceDescriptor instanceDescriptor;
    auto native = std::make_unique<dawn::native::Instance>(&instanceDescriptor);
    wgpu::Instance instance(native->Get());

    wgpu::RequestAdapterOptions options;
    options.featureLevel = wgpu::FeatureLevel::Core;
    options.backendType = wgpu::BackendType::Null;
    auto adapters = native->EnumerateAdapters(&options);
    CHECK(!adapters.empty(), "null adapter");
    if (adapters.empty())
        return 1;
    wgpu::Adapter adapter(adapters[0].Get());

    ErrorLog log;
    wgpu::DeviceDescriptor deviceDescriptor;
    deviceDescriptor.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView, ErrorLog* log) {
        log->uncaptured.push_back(type);
    }, &log);
    deviceDescriptor.SetDeviceLostCallback(wgpu::CallbackMode::AllowProcessEvents, [&log](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView) {
        log.lost++;
        log.lostReason = reason;
    });
    wgpu::Device device = adapter.CreateDevice(&deviceDescriptor);
    CHECK(!!device, "wgpu::Adapter::CreateDevice is synchronous");

    // An uncaptured error is reported inside the call that makes it.
    wgpu::BufferDescriptor bad;
    bad.size = 16;
    // The bit that an Invalid flag of WebGPUCpp.h is converted to.
    bad.usage = wgpu::BufferUsage::Vertex | static_cast<wgpu::BufferUsage>(1ull << 31);
    wgpu::Buffer errorBuffer = device.CreateBuffer(&bad);
    CHECK(!!errorBuffer, "an invalid descriptor gives a non-null error buffer");
    CHECK(log.uncaptured.size() == 1 && log.uncaptured[0] == wgpu::ErrorType::Validation, "an unknown usage bit is a validation error, reported synchronously");

    log.uncaptured.clear();
    wgpu::TextureDescriptor badTexture;
    badTexture.size = { 4, 4, 1 };
    badTexture.format = wgpu::TextureFormat::RGBA8Unorm;
    badTexture.usage = wgpu::TextureUsage::CopyDst | static_cast<wgpu::TextureUsage>(1ull << 31);
    device.CreateTexture(&badTexture);
    CHECK(log.uncaptured.size() == 1, "an unknown texture usage bit is a validation error");
    log.uncaptured.clear();
    {
        wgpu::ShaderSourceWGSL source;
        source.code = "@vertex fn vs() -> @builtin(position) vec4f { return vec4f(); } @fragment fn fs() -> @location(0) vec4f { return vec4f(); }";
        wgpu::ShaderModuleDescriptor moduleDescriptor;
        moduleDescriptor.nextInChain = &source;
        wgpu::ShaderModule module = device.CreateShaderModule(&moduleDescriptor);
        wgpu::ColorTargetState target;
        target.format = wgpu::TextureFormat::RGBA8Unorm;
        wgpu::FragmentState fragment;
        fragment.module = module;
        fragment.targetCount = 1;
        fragment.targets = &target;
        wgpu::RenderPipelineDescriptor descriptor;
        descriptor.vertex.module = module;
        descriptor.fragment = &fragment;
        // A vertex buffer slot and a color target slot that are not used.
        wgpu::VertexBufferLayout unusedBuffer;
        descriptor.vertex.bufferCount = 1;
        descriptor.vertex.buffers = &unusedBuffer;
        device.CreateRenderPipeline(&descriptor);
        CHECK(log.uncaptured.empty(), "a render pipeline with default states, undefined entry points and an unused vertex buffer slot is valid");
        target.writeMask = wgpu::ColorWriteMask::Red | static_cast<wgpu::ColorWriteMask>(1ull << 31);
        device.CreateRenderPipeline(&descriptor);
        CHECK(log.uncaptured.size() == 1, "an unknown color write bit is a validation error");
    }

    // The scopes of pauseAllErrorReporting() swallow errors above a scope of the page.
    log.uncaptured.clear();
    int popOrder = 0, pageScopeOrder = 0;
    wgpu::ErrorType pageScopeType = wgpu::ErrorType::Unknown;
    device.PushErrorScope(wgpu::ErrorFilter::Validation); // the page
    device.PushErrorScope(wgpu::ErrorFilter::Validation); // pause
    device.PushErrorScope(wgpu::ErrorFilter::OutOfMemory); // pause
    device.CreateBuffer(&bad);
    wgpu::CommandEncoder invalidEncoder = device.CreateCommandEncoder();
    wgpu::RenderPassDescriptor emptyPass;
    wgpu::RenderPassEncoder invalidPass = invalidEncoder.BeginRenderPass(&emptyPass);
    wgpu::ComputePassEncoder invalidComputePass = invalidEncoder.BeginComputePass();
    wgpu::CommandBuffer invalidCommands = invalidEncoder.Finish();
    CHECK(invalidPass && invalidComputePass && invalidCommands, "the invalid objects GPUDevice makes are non-null");
    invalidPass.End();
    invalidComputePass.End();
    device.GetQueue().Submit(1, &invalidCommands);
    bool popCalledInline = false;
    for (int i = 0; i < 2; ++i)
        device.PopErrorScope(wgpu::CallbackMode::AllowProcessEvents, [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType, wgpu::StringView) { ++popOrder; });
    popCalledInline = popOrder;
    device.PopErrorScope(wgpu::CallbackMode::AllowProcessEvents, [&](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView) {
        pageScopeOrder = ++popOrder;
        pageScopeType = status == wgpu::PopErrorScopeStatus::Success ? type : wgpu::ErrorType::Unknown;
    });
    CHECK(!popCalledInline && !popOrder, "PopErrorScope does not call back inside the call");
    instance.ProcessEvents();
    CHECK(popOrder == 3 && pageScopeOrder == 3, "ProcessEvents calls the callbacks, in the order of the calls");
    CHECK(pageScopeType == wgpu::ErrorType::NoError, "the page's scope caught nothing while reporting was paused");
    CHECK(log.uncaptured.empty(), "and nothing was uncaptured");

    // No scope: an error status.
    wgpu::PopErrorScopeStatus emptyStatus = wgpu::PopErrorScopeStatus::Success;
    device.PopErrorScope(wgpu::CallbackMode::AllowProcessEvents, [&](wgpu::PopErrorScopeStatus status, wgpu::ErrorType, wgpu::StringView) { emptyStatus = status; });
    instance.ProcessEvents();
    CHECK(emptyStatus == wgpu::PopErrorScopeStatus::Error, "PopErrorScope with no scope has the status Error");

    // InjectError is an ordinary validation error.
    device.PushErrorScope(wgpu::ErrorFilter::Validation);
    device.InjectError(wgpu::ErrorType::Validation, "injected");
    std::string injected;
    device.PopErrorScope(wgpu::CallbackMode::AllowProcessEvents, [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView message) {
        if (type == wgpu::ErrorType::Validation)
            injected = std::string(std::string_view(message));
    });
    instance.ProcessEvents();
    CHECK(injected.find("injected") != std::string::npos, "InjectError goes to the error scopes");

    // Mapping: mapped at creation, ranges, and a mapping that does not start at zero.
    wgpu::BufferDescriptor mappedDescriptor;
    mappedDescriptor.size = 64;
    mappedDescriptor.usage = wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc;
    mappedDescriptor.mappedAtCreation = true;
    wgpu::Buffer buffer = device.CreateBuffer(&mappedDescriptor);
    void* whole = buffer.GetMappedRange(0, 64);
    void* again = buffer.GetMappedRange(0, 64);
    void* part = buffer.GetMappedRange(8, 16);
    CHECK(whole && again == whole && part == static_cast<char*>(whole) + 8, "GetMappedRange may be called for overlapping ranges, and they alias");
    CHECK(!buffer.GetMappedRange(4, 8) && !buffer.GetMappedRange(8, 6), "GetMappedRange wants an offset of 8n and a size of 4n");
    memset(whole, 0x5a, 64);
    buffer.Unmap();
    buffer.Unmap();
    CHECK(log.uncaptured.empty(), "Unmap of an unmapped buffer is not an error");

    bool mapped = false, mapCalledInline = false;
    buffer.MapAsync(wgpu::MapMode::Write, 16, 32, wgpu::CallbackMode::AllowProcessEvents, [&](wgpu::MapAsyncStatus status, wgpu::StringView) { mapped = status == wgpu::MapAsyncStatus::Success; });
    mapCalledInline = mapped;
    for (int i = 0; i < 100 && !mapped; ++i)
        instance.ProcessEvents();
    CHECK(mapped && !mapCalledInline, "MapAsync completes from ProcessEvents");
    CHECK(!buffer.GetMappedRange(0, 16) && !buffer.GetMappedRange(40, 16), "only the mapped range can be had");
    auto* partial = static_cast<unsigned char*>(buffer.GetMappedRange(16, 32));
    CHECK(partial && partial[0] == 0x5a && partial[31] == 0x5a, "a write mapping has the contents of the buffer");
    CHECK(!buffer.GetConstMappedRange(0, 8) && buffer.GetConstMappedRange(16, 8), "GetConstMappedRange works on a write mapping, in its range");

    // A second mapAsync while mapped: a validation error and an error status, no state change.
    wgpu::MapAsyncStatus secondStatus = wgpu::MapAsyncStatus::Success;
    buffer.MapAsync(wgpu::MapMode::Write, 0, 8, wgpu::CallbackMode::AllowProcessEvents, [&](wgpu::MapAsyncStatus status, wgpu::StringView) { secondStatus = status; });
    instance.ProcessEvents();
    CHECK(secondStatus != wgpu::MapAsyncStatus::Success && log.uncaptured.size() == 1, "mapAsync on a mapped buffer fails with a validation error");
    CHECK(!!buffer.GetMappedRange(16, 32), "and the first mapping stands");
    log.uncaptured.clear();
    buffer.Unmap();

    // Read mapping: no writable range.
    wgpu::BufferDescriptor readDescriptor;
    readDescriptor.size = 64;
    readDescriptor.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer readBuffer = device.CreateBuffer(&readDescriptor);
    bool readMapped = false;
    readBuffer.MapAsync(wgpu::MapMode::Read, 0, wgpu::kWholeMapSize, wgpu::CallbackMode::AllowProcessEvents, [&](wgpu::MapAsyncStatus status, wgpu::StringView) { readMapped = status == wgpu::MapAsyncStatus::Success; });
    for (int i = 0; i < 100 && !readMapped; ++i)
        instance.ProcessEvents();
    CHECK(readMapped && !readBuffer.GetMappedRange(0, 64) && readBuffer.GetConstMappedRange(0, 64), "a read mapping has only a const range; kWholeMapSize maps the rest of the buffer");
    readBuffer.Unmap();

    // A pending mapping that unmap() aborts.
    wgpu::MapAsyncStatus abortedStatus = wgpu::MapAsyncStatus::Success;
    readBuffer.MapAsync(wgpu::MapMode::Read, 0, 64, wgpu::CallbackMode::AllowProcessEvents, [&](wgpu::MapAsyncStatus status, wgpu::StringView) { abortedStatus = status; });
    readBuffer.Unmap();
    instance.ProcessEvents();
    CHECK(abortedStatus == wgpu::MapAsyncStatus::Aborted || abortedStatus == wgpu::MapAsyncStatus::Success, "unmap() during a pending mapping calls back (aborted, or the mapping had completed)");

    // Auto layouts: a bind group layout of one pipeline cannot be put in a pipeline layout.
    wgpu::ShaderSourceWGSL wgsl;
    wgsl.code = "@group(0) @binding(0) var<uniform> u : vec4f; @compute @workgroup_size(1) fn main() { _ = u; }";
    wgpu::ShaderModuleDescriptor moduleDescriptor;
    moduleDescriptor.nextInChain = &wgsl;
    wgpu::ShaderModule module = device.CreateShaderModule(&moduleDescriptor);
    wgpu::ComputePipelineDescriptor pipelineDescriptor;
    pipelineDescriptor.compute.module = module; // No entry point: the only one of the stage.
    wgpu::ComputePipeline pipeline = device.CreateComputePipeline(&pipelineDescriptor);
    CHECK(log.uncaptured.empty(), "a pipeline with an undefined entry point and an auto layout is valid");
    wgpu::BindGroupLayout autoLayout = pipeline.GetBindGroupLayout(0);
    wgpu::PipelineLayoutDescriptor layoutDescriptor;
    layoutDescriptor.bindGroupLayoutCount = 1;
    layoutDescriptor.bindGroupLayouts = &autoLayout;
    wgpu::PipelineLayout explicitLayout = device.CreatePipelineLayout(&layoutDescriptor);
    CHECK(explicitLayout && log.uncaptured.size() == 1, "a pipeline layout cannot be made of another pipeline's auto bind group layouts");
    log.uncaptured.clear();

    bool asyncDone = false, asyncInline = false;
    device.CreateComputePipelineAsync(&pipelineDescriptor, wgpu::CallbackMode::AllowProcessEvents, [&](wgpu::CreatePipelineAsyncStatus status, wgpu::ComputePipeline created, wgpu::StringView) {
        asyncDone = status == wgpu::CreatePipelineAsyncStatus::Success && created;
    });
    asyncInline = asyncDone;
    for (int i = 0; i < 100000 && !asyncDone; ++i)
        instance.ProcessEvents();
    CHECK(asyncDone && !asyncInline, "CreateComputePipelineAsync completes from ProcessEvents");

    // Compilation messages: positions in UTF-16 are chained.
    wgpu::ShaderSourceWGSL badWGSL;
    badWGSL.code = "// \xc3\xa9\xc3\xa9\nfn f() { \xc3\xa9 }";
    wgpu::ShaderModuleDescriptor badModuleDescriptor;
    badModuleDescriptor.nextInChain = &badWGSL;
    device.PushErrorScope(wgpu::ErrorFilter::Validation);
    wgpu::ShaderModule badModule = device.CreateShaderModule(&badModuleDescriptor);
    device.PopErrorScope(wgpu::CallbackMode::AllowProcessEvents, [](wgpu::PopErrorScopeStatus, wgpu::ErrorType, wgpu::StringView) { });
    bool hasMessage = false, hasUTF16 = false;
    uint64_t lineNum = 0, linePos = 0, linePos16 = 0;
    badModule.GetCompilationInfo(wgpu::CallbackMode::AllowProcessEvents, [&](wgpu::CompilationInfoRequestStatus status, const wgpu::CompilationInfo* info) {
        if (status != wgpu::CompilationInfoRequestStatus::Success || !info || !info->messageCount)
            return;
        hasMessage = true;
        lineNum = info->messages[0].lineNum;
        linePos = info->messages[0].linePos;
        for (auto* chained = info->messages[0].nextInChain; chained; chained = chained->nextInChain) {
            if (chained->sType == wgpu::SType::DawnCompilationMessageUtf16) {
                hasUTF16 = true;
                linePos16 = static_cast<const wgpu::DawnCompilationMessageUtf16*>(chained)->linePos;
            }
        }
    });
    instance.ProcessEvents();
    printf("      compilation message: line %llu, position %llu (UTF-8), %llu (UTF-16)\n", (unsigned long long)lineNum, (unsigned long long)linePos, (unsigned long long)linePos16);
    // The line has one two-byte character before the position.
    CHECK(hasMessage && hasUTF16 && lineNum == 2 && linePos16 > 1 && linePos == linePos16 + 1, "GetCompilationInfo: one-based line and position in UTF-8, with a chained DawnCompilationMessageUtf16");

    // Teardown as ~InstanceImpl does it: destroy the device, and the callbacks become ready.
    bool workDone = false, lateMap = false, lateMapCalled = false;
    device.GetQueue().OnSubmittedWorkDone(wgpu::CallbackMode::AllowProcessEvents, [&](wgpu::QueueWorkDoneStatus, wgpu::StringView) { workDone = true; });
    readBuffer.MapAsync(wgpu::MapMode::Read, 0, 64, wgpu::CallbackMode::AllowProcessEvents, [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
        lateMapCalled = true;
        lateMap = status == wgpu::MapAsyncStatus::Success;
    });
    CHECK(!log.lost, "no device lost callback so far");
    device.Destroy();
    CHECK(!log.lost, "Destroy() does not call the device lost callback inline");
    for (int i = 0; i < 10; ++i)
        instance.ProcessEvents();
    CHECK(log.lost == 1 && log.lostReason == wgpu::DeviceLostReason::Destroyed, "ProcessEvents reports the loss, with the reason Destroyed");
    CHECK(workDone && lateMapCalled, "the callbacks of a destroyed device are called by ProcessEvents");
    (void)lateMap;

    // After the loss: no uncaptured errors, and PopErrorScope succeeds with no error.
    log.uncaptured.clear();
    device.CreateBuffer(&bad);
    wgpu::PopErrorScopeStatus lostPopStatus = wgpu::PopErrorScopeStatus::Error;
    wgpu::ErrorType lostPopType = wgpu::ErrorType::Unknown;
    device.PopErrorScope(wgpu::CallbackMode::AllowProcessEvents, [&](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView) {
        lostPopStatus = status;
        lostPopType = type;
    });
    instance.ProcessEvents();
    CHECK(log.uncaptured.empty(), "a lost device reports no uncaptured errors");
    CHECK(lostPopStatus == wgpu::PopErrorScopeStatus::Success && lostPopType == wgpu::ErrorType::NoError, "PopErrorScope on a lost device is a scope with no error");

    printf("%d failed\n", g_failures);
    return g_failures ? 1 : 0;
}
