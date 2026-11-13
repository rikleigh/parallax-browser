// Spike 12 P2: the smallest user of the Dawn WebGPU backend, for the link check. It is linked
// for the PS5 and never run: there is no Vulkan driver behind the hooks below.
#include "config.h"

#include <WebGPU/WebGPUCpp.h>
#include <wtf/CompletionHandler.h>
#include <wtf/Function.h>

extern "C" {
// port/deps/patches/dawn: the title supplies the drivers. None here.
void* dawn_ps5_vk_get_instance_proc_addr(void) { return nullptr; }
void* dawn_ps5_egl_get_proc_address(const char*) { return nullptr; }
}

int main()
{
    RefPtr instance = WebGPU::createInstance({ });
    if (!instance)
        return 1;

    int result = 2;
    instance->requestAdapter({ }, [&](RefPtr<WebGPU::Adapter>&& adapter) {
        if (!adapter)
            return;
        adapter->requestDevice({ }, [&](RefPtr<WebGPU::Device>&& device) {
            result = device ? 0 : 3;
        });
    });

    RefPtr presentationContext = instance->createPresentationContext({ });
    return presentationContext ? result : 4;
}
