/* Spike 12 P0b/P0c, Vulkan side: RADV (PS5_Mesa) in the same title as
 * ps5-opengl, while the GL thread presents.
 *   P0b: headless device, triangle into an offscreen image, compute dispatch.
 *   P0c: the frame handed to GL through a GL persistent buffer imported by
 *        host pointer (VK_EXT_external_memory_host), and the fallback (a
 *        Vulkan staging buffer and a memcpy into the GL mapping).
 * No loader: the entry points come from the ICD's vk_icdGetInstanceProcAddr. */
#define SPIKE_NAME "12-webgpu-p0b"
#include "common/spike.h"
#include "shared.h"

#include <stdlib.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "tri_vert.h"
#include "tri_frag.h"
#include "fill_comp.h"

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t align, int64_t *out_start, size_t *out_size);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableFlexibleMemorySize(size_t *out);

#define VK_FUNCS(F)                                                                                   \
    F(EnumeratePhysicalDevices) F(GetPhysicalDeviceProperties2) F(GetPhysicalDeviceMemoryProperties)  \
    F(GetPhysicalDeviceQueueFamilyProperties) F(EnumerateDeviceExtensionProperties) F(CreateDevice)   \
    F(GetDeviceQueue) F(CreateCommandPool) F(AllocateCommandBuffers) F(CreateImage)                   \
    F(GetImageMemoryRequirements) F(AllocateMemory) F(BindImageMemory) F(CreateImageView)             \
    F(CreateBuffer) F(GetBufferMemoryRequirements) F(BindBufferMemory) F(MapMemory)                   \
    F(CreateShaderModule) F(CreatePipelineLayout) F(CreateGraphicsPipelines) F(CreateComputePipelines)\
    F(CreateDescriptorSetLayout) F(CreateDescriptorPool) F(AllocateDescriptorSets)                    \
    F(UpdateDescriptorSets) F(BeginCommandBuffer) F(EndCommandBuffer) F(ResetCommandBuffer)           \
    F(CmdBeginRendering) F(CmdEndRendering) F(CmdBindPipeline) F(CmdSetViewport) F(CmdSetScissor)     \
    F(CmdPushConstants) F(CmdDraw) F(CmdPipelineBarrier) F(CmdCopyImageToBuffer)                      \
    F(CmdBindDescriptorSets) F(CmdDispatch) F(QueueSubmit) F(CreateFence) F(WaitForFences)            \
    F(ResetFences) F(DeviceWaitIdle) F(GetPhysicalDeviceFeatures2)
#define DECLARE(n) static PFN_vk##n vk##n;
VK_FUNCS(DECLARE)
static PFN_vkCreateInstance vkCreateInstance;
static PFN_vkGetMemoryHostPointerPropertiesEXT vkGetMemoryHostPointerPropertiesEXT;

static VkInstance inst;
static VkPhysicalDevice pdev;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkCommandBuffer cmd;
static VkFence fence;
static VkPhysicalDeviceMemoryProperties memprops;

static void mem_line(const char *when)
{
    int64_t start = 0;
    size_t avail = 0, flex = 0;
    const size_t pool = sceKernelGetDirectMemorySize();
    sceKernelAvailableDirectMemorySize(0, (int64_t)pool, 0x4000, &start, &avail);
    sceKernelAvailableFlexibleMemorySize(&flex);
    spike_log("[mem] %-28s direct pool %.0f MB, largest free block %.1f MB, flexible free %.1f MB", when,
              pool / 1048576.0, avail / 1048576.0, flex / 1048576.0);
}

static int mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static VkResult submit_and_wait(double *ms)
{
    const double t0 = spike_now_ms();
    const VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    VkResult r = vkQueueSubmit(queue, 1, &si, fence);
    if (r == VK_SUCCESS)
        r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
    vkResetFences(dev, 1, &fence);
    if (ms)
        *ms = spike_now_ms() - t0;
    return r;
}

static VkResult make_buffer(VkDeviceSize bytes, VkBufferUsageFlags usage, VkBuffer *buf, void **map)
{
    const VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .usage = usage };
    VkResult r = vkCreateBuffer(dev, &bi, NULL, buf);
    if (r)
        return r;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, *buf, &mr);
    const int type = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)type };
    VkDeviceMemory m;
    if ((r = vkAllocateMemory(dev, &ai, NULL, &m)) || (r = vkBindBufferMemory(dev, *buf, m, 0)))
        return r;
    return vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, map);
}

static VkShaderModule shader(const uint32_t *code, size_t bytes)
{
    const VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code };
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, NULL, &m);
    return m;
}

/* The offscreen target and its pipeline. */
static VkImage image;
static VkImageView view;
static VkPipeline tri_pipeline;
static VkPipelineLayout tri_layout;

static void record_frame(const float colour[4], VkBuffer dst, VkDeviceSize dst_offset)
{
    vkResetCommandBuffer(cmd, 0);
    const VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
                               .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    const VkRenderingAttachmentInfo att = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view,
                                            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                            .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.color.float32 = { 0, 0, 1, 1 } };
    const VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { BRIDGE_W, BRIDGE_H } },
                                 .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &att };
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp = { 0, 0, BRIDGE_W, BRIDGE_H, 0, 1 };
    const VkRect2D sc = { { 0, 0 }, { BRIDGE_W, BRIDGE_H } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tri_pipeline);
    vkCmdPushConstants(cmd, tri_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, colour);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    const VkBufferImageCopy region = { .bufferOffset = dst_offset, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                       .imageExtent = { BRIDGE_W, BRIDGE_H, 1 } };
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
    vkEndCommandBuffer(cmd);
}

static int near(const uint8_t *px, int r, int g, int b)
{
    return abs(px[0] - r) <= 2 && abs(px[1] - g) <= 2 && abs(px[2] - b) <= 2;
}

static void frame_colour(int n, float c[4], int rgb[3])
{
    rgb[0] = (n * 8) & 255;
    rgb[1] = 255 - ((n * 5) & 255);
    rgb[2] = 64;
    c[0] = rgb[0] / 255.0f;
    c[1] = rgb[1] / 255.0f;
    c[2] = rgb[2] / 255.0f;
    c[3] = 1;
}

static int wait_gl_drawn(int n, int timeout_ms)
{
    for (int i = 0; i < timeout_ms * 10 && g_gl.drawn != n; i++)
        usleep(100);
    return g_gl.drawn == n;
}

void vk_run(void)
{
    VkResult r;
    mem_line("before vkCreateInstance");
    unsigned long gl0 = g_gl.frames;
    double t0 = spike_now_ms();

    vkCreateInstance = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
    if (!vkCreateInstance) {
        SPIKE_FAIL("vk-instance", "vk_icdGetInstanceProcAddr(vkCreateInstance) is NULL");
        return;
    }
    const VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "spike12", .apiVersion = VK_API_VERSION_1_3 };
    const VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    if ((r = vkCreateInstance(&ici, NULL, &inst))) {
        SPIKE_FAIL("vk-instance", "vkCreateInstance %d", r);
        return;
    }
#define LOAD(n) vk##n = (PFN_vk##n)vk_icdGetInstanceProcAddr(inst, "vk" #n);
    VK_FUNCS(LOAD)
    LOAD(GetMemoryHostPointerPropertiesEXT)
    uint32_t n = 1;
    r = vkEnumeratePhysicalDevices(inst, &n, &pdev);
    if (r < 0 || n < 1) {
        SPIKE_FAIL("vk-instance", "vkEnumeratePhysicalDevices %d, %u devices", r, n);
        return;
    }
    SPIKE_PASS("vk-instance", "instance and physical device in %.0f ms; GL presented %lu frames meanwhile", spike_now_ms() - t0, g_gl.frames - gl0);
    mem_line("after vkCreateInstance");

    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostprops = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
    VkPhysicalDeviceProperties2 props = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hostprops };
    vkGetPhysicalDeviceProperties2(pdev, &props);
    vkGetPhysicalDeviceMemoryProperties(pdev, &memprops);
    static VkExtensionProperties exts[512];
    uint32_t next = 512;
    vkEnumerateDeviceExtensionProperties(pdev, NULL, &next, exts);
    int have_host = 0;
    for (uint32_t i = 0; i < next; i++)
        have_host |= !strcmp(exts[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    static VkQueueFamilyProperties qf[8];
    uint32_t nq = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nq, qf);
    qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq && qfam == UINT32_MAX; i++)
        if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            qfam = i;
    char heaps[160] = "";
    for (uint32_t i = 0; i < memprops.memoryHeapCount; i++)
        snprintf(heaps + strlen(heaps), sizeof heaps - strlen(heaps), "%s%.0f MB%s", i ? ", " : "",
                 memprops.memoryHeaps[i].size / 1048576.0, memprops.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " (device)" : "");

    t0 = spike_now_ms();
    const float prio = 1;
    const VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE };
    const char *dev_exts[] = { VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME };
    const VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                                     .enabledExtensionCount = have_host ? 1 : 0, .ppEnabledExtensionNames = dev_exts };
    if (qfam == UINT32_MAX || (r = vkCreateDevice(pdev, &dci, NULL, &dev))) {
        SPIKE_FAIL("vk-device", "queue family %u, vkCreateDevice %d", qfam, r);
        return;
    }
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    const VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam };
    VkCommandPool pool;
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    const VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    vkAllocateCommandBuffers(dev, &cai, &cmd);
    const VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vkCreateFence(dev, &fci, NULL, &fence);
    SPIKE_PASS("vk-device", "\"%s\" Vulkan %u.%u.%u, %u extensions, heaps: %s; queue family %u of %u; device in %.0f ms; "
               "VK_EXT_external_memory_host %s (alignment %llu)", props.properties.deviceName,
               VK_VERSION_MAJOR(props.properties.apiVersion), VK_VERSION_MINOR(props.properties.apiVersion),
               VK_VERSION_PATCH(props.properties.apiVersion), next, heaps, qfam, nq, spike_now_ms() - t0,
               have_host ? "yes" : "NO", (unsigned long long)hostprops.minImportedHostPointerAlignment);
    mem_line("after vkCreateDevice");

    /* -- triangle into an offscreen image, read back ------------------------ */
    const VkImageCreateInfo imi = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                    .extent = { BRIDGE_W, BRIDGE_H, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                    .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
    VkMemoryRequirements mr;
    VkDeviceMemory imem;
    r = vkCreateImage(dev, &imi, NULL, &image);
    if (!r) {
        vkGetImageMemoryRequirements(dev, image, &mr);
        const VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                          .memoryTypeIndex = (uint32_t)mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
        r = vkAllocateMemory(dev, &ai, NULL, &imem);
    }
    if (!r)
        r = vkBindImageMemory(dev, image, imem, 0);
    const VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                        .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    if (!r)
        r = vkCreateImageView(dev, &vci, NULL, &view);
    VkBuffer staging = VK_NULL_HANDLE;
    void *staging_map = NULL;
    if (!r)
        r = make_buffer(BRIDGE_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &staging, &staging_map);
    if (r) {
        SPIKE_FAIL("vk-triangle", "target or staging buffer: %d", r);
        return;
    }
    t0 = spike_now_ms();
    const VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
    const VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    vkCreatePipelineLayout(dev, &plci, NULL, &tri_layout);
    const VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = shader(tri_vert, sizeof tri_vert), .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = shader(tri_frag, sizeof tri_frag), .pName = "main" },
    };
    const VkPipelineVertexInputStateCreateInfo vis = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    const VkPipelineInputAssemblyStateCreateInfo ias = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    const VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    const VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
                                                        .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
    const VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    const VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
    const VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    const VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    const VkPipelineRenderingCreateInfo prci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt };
    const VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &prci, .stageCount = 2, .pStages = stages,
                                                .pVertexInputState = &vis, .pInputAssemblyState = &ias, .pViewportState = &vps, .pRasterizationState = &rs,
                                                .pMultisampleState = &ms, .pColorBlendState = &cbs, .pDynamicState = &ds, .layout = tri_layout };
    if ((r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &tri_pipeline))) {
        SPIKE_FAIL("vk-triangle", "vkCreateGraphicsPipelines %d", r);
        return;
    }
    const double pipeline_ms = spike_now_ms() - t0;
    float colour[4];
    int rgb[3];
    frame_colour(1, colour, rgb);
    double frame_ms = 0;
    record_frame(colour, staging, 0);
    r = submit_and_wait(&frame_ms);
    const uint8_t *px = staging_map;
    const uint8_t *centre = px + ((size_t)(BRIDGE_H / 2) * BRIDGE_W + BRIDGE_W / 2) * 4, *corner = px + ((size_t)2 * BRIDGE_W + 2) * 4;
    if (r == VK_SUCCESS && near(centre, rgb[0], rgb[1], rgb[2]) && near(corner, 0, 0, 255))
        SPIKE_PASS("vk-triangle", "1920x1080 RGBA8: centre %u,%u,%u (want %d,%d,%d) corner %u,%u,%u; pipeline %.1f ms, draw + copy to buffer + fence %.2f ms",
                   centre[0], centre[1], centre[2], rgb[0], rgb[1], rgb[2], corner[0], corner[1], corner[2], pipeline_ms, frame_ms);
    else
        SPIKE_FAIL("vk-triangle", "result %d centre %u,%u,%u,%u (want %d,%d,%d) corner %u,%u,%u,%u", r, centre[0], centre[1], centre[2], centre[3],
                   rgb[0], rgb[1], rgb[2], corner[0], corner[1], corner[2], corner[3]);

    /* -- compute dispatch ---------------------------------------------------- */
    enum { VALUES = 4096 };
    VkBuffer cbuf;
    void *cmap = NULL;
    r = make_buffer(VALUES * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &cbuf, &cmap);
    const VkDescriptorSetLayoutBinding dslb = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    const VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &dslb };
    VkDescriptorSetLayout dsl;
    vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl);
    const VkPipelineLayoutCreateInfo cplci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
    VkPipelineLayout clayout;
    vkCreatePipelineLayout(dev, &cplci, NULL, &clayout);
    const VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    const VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps };
    VkDescriptorPool dpool;
    vkCreateDescriptorPool(dev, &dpci, NULL, &dpool);
    const VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet dset;
    vkAllocateDescriptorSets(dev, &dsai, &dset);
    const VkDescriptorBufferInfo dbi = { cbuf, 0, VK_WHOLE_SIZE };
    const VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi };
    vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);
    t0 = spike_now_ms();
    const VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = clayout,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader(fill_comp, sizeof fill_comp), .pName = "main" } };
    VkPipeline cpipe = VK_NULL_HANDLE;
    if (!r)
        r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &cpipe);
    const double cpipe_ms = spike_now_ms() - t0;
    double dispatch_ms = 0;
    unsigned bad = 0, first_bad = 0;
    if (!r) {
        memset(cmap, 0, VALUES * 4);
        vkResetCommandBuffer(cmd, 0);
        const VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(cmd, &bi);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cpipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, clayout, 0, 1, &dset, 0, NULL);
        vkCmdDispatch(cmd, VALUES / 64, 1, 1);
        vkEndCommandBuffer(cmd);
        r = submit_and_wait(&dispatch_ms);
        const uint32_t *v = cmap;
        for (unsigned i = 0; i < VALUES; i++)
            if (v[i] != i * 3u + 7u && !bad++)
                first_bad = i;
    }
    if (r == VK_SUCCESS && !bad)
        SPIKE_PASS("vk-compute", "%d values written by the shader, all right; pipeline %.1f ms, dispatch + fence %.2f ms", VALUES, cpipe_ms, dispatch_ms);
    else
        SPIKE_FAIL("vk-compute", "result %d, %u wrong values (first at %u)", r, bad, first_bad);
    mem_line("after triangle and compute");

    /* -- did GL keep presenting? --------------------------------------------- */
    {
        const unsigned long a = g_gl.frames;
        usleep(500 * 1000);
        const unsigned long b = g_gl.frames;
        if (b - a >= 20 && !g_gl.gl_errors && !g_gl.swap_failures)
            SPIKE_PASS("gl-alive", "GL presented %lu frames in 0.5 s after the Vulkan work (%lu since start), no GL errors", b - a, b);
        else
            SPIKE_FAIL("gl-alive", "%lu frames in 0.5 s, %lu GL errors, %lu failed swaps", b - a, (unsigned long)g_gl.gl_errors, (unsigned long)g_gl.swap_failures);
    }

    /* -- P0c: the frame to GL ------------------------------------------------ */
    g_gl.want_bridge = 1;
    for (int i = 0; i < 300 && !g_gl.bridge_state; i++)
        usleep(10 * 1000);
    if (g_gl.bridge_state != 1) {
        SPIKE_FAIL("bridge-gl-buffer", "GL persistent buffer: state %d map %p", (int)g_gl.bridge_state, (void *)g_gl.map);
        return;
    }
    SPIKE_PASS("bridge-gl-buffer", "GL persistent SSBO of %.1f MB mapped at %p", g_gl.map_bytes / 1048576.0, (void *)g_gl.map);

    /* Fallback first (it cannot fault): staging buffer, then memcpy into the GL mapping. */
    {
        g_gl.offset_words = 0;
        g_gl.verify = 1;
        int ok = 1, shown[3] = { 0 };
        uint8_t got[4] = { 0 };
        for (int f = 1; f <= 4 && ok; f++) {
            frame_colour(f, colour, rgb);
            record_frame(colour, staging, 0);
            ok = submit_and_wait(NULL) == VK_SUCCESS;
            memcpy(g_gl.map, staging_map, BRIDGE_BYTES);
            g_gl.posted = f;
            ok = ok && wait_gl_drawn(f, 2000) && near(g_gl.centre, rgb[0], rgb[1], rgb[2]) && near(g_gl.corner, 0, 0, 255);
            memcpy(got, g_gl.centre, 4);
            memcpy(shown, rgb, sizeof shown);
        }
        g_gl.verify = 0;
        double vk_sum = 0, cpy_sum = 0, cpy_max = 0;
        enum { N = 120 };
        const unsigned long glf = g_gl.frames;
        const double all0 = spike_now_ms();
        for (int f = 0; f < N && ok; f++) {
            double vk_ms = 0;
            frame_colour(100 + f, colour, rgb);
            record_frame(colour, staging, 0);
            ok = submit_and_wait(&vk_ms) == VK_SUCCESS;
            const double c0 = spike_now_ms();
            memcpy(g_gl.map, staging_map, BRIDGE_BYTES);
            const double c = spike_now_ms() - c0;
            g_gl.posted = 100 + f;
            ok = ok && wait_gl_drawn(100 + f, 2000);
            vk_sum += vk_ms;
            cpy_sum += c;
            if (c > cpy_max)
                cpy_max = c;
        }
        const double all = spike_now_ms() - all0;
        if (ok)
            SPIKE_PASS("bridge-memcpy", "4 frames right in the GL texture; %d frames: Vulkan draw + copy + fence %.2f ms, memcpy of %.1f MB %.2f ms (max %.2f), "
                       "GL pass %.2f ms CPU; %.1f frames/s end to end, GL presented %lu", N, vk_sum / N, BRIDGE_BYTES / 1048576.0, cpy_sum / N, cpy_max,
                       g_gl.pass_count ? g_gl.pass_ms_sum / g_gl.pass_count : 0, N * 1000.0 / all, g_gl.frames - glf);
        else
            SPIKE_FAIL("bridge-memcpy", "GL texture centre %u,%u,%u want %d,%d,%d (drawn %d posted %d)", got[0], got[1], got[2], shown[0], shown[1], shown[2],
                       (int)g_gl.drawn, (int)g_gl.posted);
    }

    /* Zero copy: import the GL mapping. */
    if (!have_host || !vkGetMemoryHostPointerPropertiesEXT) {
        SPIKE_FAIL("bridge-import", "VK_EXT_external_memory_host is not offered");
        return;
    }
    const VkDeviceSize align = hostprops.minImportedHostPointerAlignment;
    uint8_t *at = (uint8_t *)(((uintptr_t)g_gl.map + align - 1) & ~(uintptr_t)(align - 1));
    const VkDeviceSize import_bytes = (BRIDGE_BYTES + align - 1) & ~(align - 1);
    if (at + import_bytes > g_gl.map + g_gl.map_bytes) {
        SPIKE_FAIL("bridge-import", "no aligned range of %llu bytes in the mapping (alignment %llu)", (unsigned long long)import_bytes, (unsigned long long)align);
        return;
    }
    VkMemoryHostPointerPropertiesEXT hpp = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    r = vkGetMemoryHostPointerPropertiesEXT(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, at, &hpp);
    spike_log("bridge: importing %p (+%td in the GL mapping), %llu bytes, memory types 0x%x (result %d)", (void *)at, at - g_gl.map,
              (unsigned long long)import_bytes, hpp.memoryTypeBits, r);
    spike_log_flush(200);
    const VkExternalMemoryBufferCreateInfo embci = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
    const VkBufferCreateInfo bbi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &embci, .size = import_bytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer bridge = VK_NULL_HANDLE;
    VkDeviceMemory bmem = VK_NULL_HANDLE;
    if (!r)
        r = vkCreateBuffer(dev, &bbi, NULL, &bridge);
    int type = -1;
    if (!r) {
        vkGetBufferMemoryRequirements(dev, bridge, &mr);
        type = mem_type(mr.memoryTypeBits & hpp.memoryTypeBits, 0);
        const VkImportMemoryHostPointerInfoEXT imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                                       .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = at };
        const VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = import_bytes, .memoryTypeIndex = (uint32_t)type };
        r = type < 0 ? VK_ERROR_FEATURE_NOT_PRESENT : vkAllocateMemory(dev, &ai, NULL, &bmem);
    }
    if (!r)
        r = vkBindBufferMemory(dev, bridge, bmem, 0);
    if (r) {
        SPIKE_FAIL("bridge-import", "import of the GL mapping: %d (memory type %d of 0x%x)", r, type, hpp.memoryTypeBits);
        return;
    }
    SPIKE_PASS("bridge-import", "GL mapping %p imported as Vulkan memory (type %d), %llu bytes at +%td", (void *)at, type, (unsigned long long)import_bytes, at - g_gl.map);
    spike_log_flush(200);
    {
        memset(g_gl.map, 0x55, g_gl.map_bytes); /* so a frame that never arrives is seen */
        g_gl.offset_words = (unsigned)((at - g_gl.map) / 4);
        g_gl.verify = 1;
        int ok = 1, shown[3] = { 0 };
        uint8_t got[4] = { 0 };
        for (int f = 301; f <= 308 && ok; f++) {
            frame_colour(f, colour, rgb);
            record_frame(colour, bridge, 0);
            ok = submit_and_wait(NULL) == VK_SUCCESS;
            g_gl.posted = f;
            ok = ok && wait_gl_drawn(f, 2000) && near(g_gl.centre, rgb[0], rgb[1], rgb[2]) && near(g_gl.corner, 0, 0, 255);
            memcpy(got, g_gl.centre, 4);
            memcpy(shown, rgb, sizeof shown);
        }
        g_gl.verify = 0;
        g_gl.pass_ms_sum = 0;
        g_gl.pass_count = 0;
        enum { N = 300 };
        double vk_sum = 0, vk_max = 0;
        const unsigned long glf = g_gl.frames;
        const double all0 = spike_now_ms();
        for (int f = 0; f < N && ok; f++) {
            double vk_ms = 0;
            frame_colour(400 + f, colour, rgb);
            record_frame(colour, bridge, 0);
            ok = submit_and_wait(&vk_ms) == VK_SUCCESS;
            g_gl.posted = 400 + f;
            ok = ok && wait_gl_drawn(400 + f, 2000);
            vk_sum += vk_ms;
            if (vk_ms > vk_max)
                vk_max = vk_ms;
        }
        const double all = spike_now_ms() - all0;
        if (ok)
            SPIKE_PASS("bridge-zero-copy", "8 frames right in the GL texture; %d frames: Vulkan draw + vkCmdCopyImageToBuffer into the GL buffer + fence %.2f ms (max %.2f), "
                       "GL SSBO pass %.2f ms CPU; %.1f frames/s end to end, GL presented %lu, GL errors %lu", N, vk_sum / N, vk_max,
                       g_gl.pass_count ? g_gl.pass_ms_sum / g_gl.pass_count : 0, N * 1000.0 / all, g_gl.frames - glf, (unsigned long)g_gl.gl_errors);
        else
            SPIKE_FAIL("bridge-zero-copy", "GL texture centre %u,%u,%u want %d,%d,%d corner %u,%u,%u (drawn %d posted %d)", got[0], got[1], got[2],
                       shown[0], shown[1], shown[2], g_gl.corner[0], g_gl.corner[1], g_gl.corner[2], (int)g_gl.drawn, (int)g_gl.posted);
    }
    mem_line("after the bridge");
    vkDeviceWaitIdle(dev);
}
