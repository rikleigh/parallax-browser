// vk_share_15e.cpp - spike 15e: images shared between two VkDevices in one
// process on PS5_Mesa's RADV: ANGLE's, and a second one standing in for
// Dawn's (its own VkInstance and VkDevice, with VK_EXT_external_memory_host,
// the one extension Dawn would need). No ps5-opengl.
//
// What's known before running (README, 15e):
//   - neither ANGLE nor Dawn takes an existing VkDevice;
//   - the PS5 winsys exports no memory fds (has_external_fd = false), so no
//     VK_KHR_external_memory_fd / _semaphore_fd: Dawn's and ANGLE's usual
//     interop (opaque fd, GL_EXT_memory_object_fd) has nothing to stand on;
//   - but one RADV in one process has one GPU address space: a RADV buffer's
//     GPU address is its CPU address, and a host-pointer import maps the same
//     memory at the same address (radv_ps5_buffer_from_ptr) and never takes
//     it back. So a second device can import ANGLE's memory by pointer and
//     bind an identical VkImage to it. RADV lays an image out from its create
//     info alone, but external handle types change it (no DCC, no HTILE,
//     "shareable"), so both sides carry VkExternalMemoryImageCreateInfo.
//
// Checks (1920x1080 RGBA8, the coordinate pattern of 15c, every texel):
//   share-caps        what the second device offers (fd extensions, host
//                     import and its alignment)
//   share-a-to-b      ANGLE draws into its image with GL; the second device
//                     reads its alias of the same memory (copy to a buffer):
//                     video or a WebGL canvas into WebGPU
//   share-b-to-a      the second device writes its alias (copy from a
//                     buffer); ANGLE samples its image: a WebGPU canvas into
//                     the compositor
//   (both for "same" create info, and "control": ANGLE's image without the
//    external-memory struct, which should not match)
//   share-buffer      the linear fallback: the second device copies its
//                     image into a buffer aliasing an ANGLE GL buffer
//                     (Dawn's CopyTextureToBuffer into host-mapped memory),
//                     then ANGLE uploads it from the buffer on the GPU
//                     (PIXEL_UNPACK); per-frame cost of the two copies
#define EGL_NO_PLATFORM_SPECIFIC_TYPES 1
#define GL_GLES_PROTOTYPES 0
#define EGL_EGL_PROTOTYPES 0
#define VK_NO_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <vulkan/vulkan_core.h>

#include "libGLESv2/entry_points_egl_autogen.h"
#include "libGLESv2/entry_points_egl_ext_autogen.h"
#include "libGLESv2/entry_points_gles_2_0_autogen.h"
#include "libGLESv2/entry_points_gles_3_0_autogen.h"
#include "libGLESv2/entry_points_gles_ext_autogen.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

extern "C" {
void spike15_log(const char *fmt, ...);
void spike15_result(const char *test, int pass, const char *fmt, ...);
double spike15_now_ms(void);
void vk_share_tests_run(void);
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance, const char *);
}

namespace {

constexpr uint32_t kW = 1920, kH = 1080;
constexpr VkDeviceSize kPage = 0x10000;

EGLDisplay g_dpy = EGL_NO_DISPLAY;

// Vulkan functions, per device (both are RADV; the instance decides).
struct Fns {
#define FN(n) PFN_vk##n n = nullptr;
#define VK_ALL(X)                                                                                          \
    X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties2) X(GetPhysicalDeviceMemoryProperties)       \
    X(EnumerateDeviceExtensionProperties) X(GetPhysicalDeviceQueueFamilyProperties) X(CreateDevice)        \
    X(GetPhysicalDeviceExternalBufferProperties) X(GetDeviceQueue) X(CreateCommandPool)                    \
    X(AllocateCommandBuffers) X(CreateFence) X(ResetFences) X(WaitForFences) X(BeginCommandBuffer)         \
    X(EndCommandBuffer) X(ResetCommandBuffer) X(QueueSubmit) X(CreateImage) X(DestroyImage)                \
    X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory) X(CreateBuffer)       \
    X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) X(CmdPipelineBarrier) \
    X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) X(GetMemoryHostPointerPropertiesEXT)
    VK_ALL(FN)
#undef FN
    void load(VkInstance inst)
    {
#define LD(n) n = (PFN_vk##n)vk_icdGetInstanceProcAddr(inst, "vk" #n);
        VK_ALL(LD)
#undef LD
    }
};

struct Dev {
    Fns f;
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mp = {};
    // second device only
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDeviceSize hostAlign = 0;
} A, B;

int mem_type(const Dev &d, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < d.mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (d.mp.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

void expect(uint32_t x, uint32_t y, uint8_t out[4])
{
    out[0] = (uint8_t)(x & 255);
    out[1] = (uint8_t)(y & 255);
    out[2] = (uint8_t)(((x >> 8) & 15) | (((y >> 8) & 15) << 4));
    out[3] = 255;
}

uint64_t count_bad(const uint8_t *px, uint32_t rowBytes, uint32_t *fx, uint32_t *fy)
{
    uint64_t bad = 0;
    uint8_t want[4];
    for (uint32_t y = 0; y < kH; y++)
        for (uint32_t x = 0; x < kW; x++) {
            expect(x, y, want);
            if (memcmp(px + (size_t)y * rowBytes + x * 4, want, 4) != 0 && !bad++) {
                *fx = x;
                *fy = y;
            }
        }
    return bad;
}

// --- GL helpers ----------------------------------------------------------------

const char *kVsFull =
    "#version 300 es\n"
    "void main() { vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1) * 4.0 - 1.0;\n"
    "  gl_Position = vec4(p, 0.0, 1.0); }\n";
const char *kFsPattern =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "out vec4 o;\n"
    "void main() { ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  int b = ((p.x >> 8) & 15) | (((p.y >> 8) & 15) << 4);\n"
    "  o = vec4(float(p.x & 255), float(p.y & 255), float(b), 255.0) / 255.0; }\n";
GLuint g_pattern = 0, g_vao = 0;

GLuint compile(GLenum type, const char *src)
{
    GLuint s = GL_CreateShader(type);
    GL_ShaderSource(s, 1, &src, nullptr);
    GL_CompileShader(s);
    return s;
}

VkImageLayout vk_layout(GLenum l)
{
    switch (l) {
    case GL_LAYOUT_GENERAL_EXT: return VK_IMAGE_LAYOUT_GENERAL;
    case GL_LAYOUT_COLOR_ATTACHMENT_EXT: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case GL_LAYOUT_SHADER_READ_ONLY_EXT: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case GL_LAYOUT_TRANSFER_SRC_EXT: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case GL_LAYOUT_TRANSFER_DST_EXT: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    default: return VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

// --- set-up ----------------------------------------------------------------------

bool angle_up()
{
    const EGLAttrib attrs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE,
        EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE, EGL_PLATFORM_VULKAN_DISPLAY_MODE_SIMPLE_ANGLE,
        EGL_NONE,
    };
    g_dpy = EGL_GetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, attrs);
    const EGLint ctxAttrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    EGLContext ctx = EGL_NO_CONTEXT;
    if (g_dpy != EGL_NO_DISPLAY && EGL_Initialize(g_dpy, nullptr, nullptr))
        ctx = EGL_CreateContext(g_dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxAttrs);
    if (ctx == EGL_NO_CONTEXT || !EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        spike15_result("share-caps", 0, "ANGLE context: 0x%04x", EGL_GetError());
        return false;
    }
    EGLAttrib dev = 0, inst = 0, phys = 0, device = 0;
    if (!EGL_QueryDisplayAttribEXT(g_dpy, EGL_DEVICE_EXT, &dev) ||
        !EGL_QueryDeviceAttribEXT((EGLDeviceEXT)dev, EGL_VULKAN_INSTANCE_ANGLE, &inst) ||
        !EGL_QueryDeviceAttribEXT((EGLDeviceEXT)dev, EGL_VULKAN_PHYSICAL_DEVICE_ANGLE, &phys) ||
        !EGL_QueryDeviceAttribEXT((EGLDeviceEXT)dev, EGL_VULKAN_DEVICE_ANGLE, &device)) {
        spike15_result("share-caps", 0, "EGL device query: 0x%04x", EGL_GetError());
        return false;
    }
    A.inst = (VkInstance)inst;
    A.phys = (VkPhysicalDevice)phys;
    A.dev = (VkDevice)device;
    A.f.load(A.inst);
    A.f.GetPhysicalDeviceMemoryProperties(A.phys, &A.mp);
    GLuint vs = compile(GL_VERTEX_SHADER, kVsFull), fs = compile(GL_FRAGMENT_SHADER, kFsPattern);
    g_pattern = GL_CreateProgram();
    GL_AttachShader(g_pattern, vs);
    GL_AttachShader(g_pattern, fs);
    GL_LinkProgram(g_pattern);
    GL_GenVertexArrays(1, &g_vao);
    GLint ok = 0;
    GL_GetProgramiv(g_pattern, GL_LINK_STATUS, &ok);
    return ok != 0;
}

bool second_device_up()
{
    auto create = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(nullptr, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "spike15e-dawn-stand-in";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkResult r = create ? create(&ici, nullptr, &B.inst) : VK_ERROR_INITIALIZATION_FAILED;
    if (r != VK_SUCCESS) {
        spike15_result("share-caps", 0, "second vkCreateInstance %d", (int)r);
        return false;
    }
    B.f.load(B.inst);
    uint32_t n = 1;
    B.f.EnumeratePhysicalDevices(B.inst, &n, &B.phys);
    B.f.GetPhysicalDeviceMemoryProperties(B.phys, &B.mp);
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &hp};
    B.f.GetPhysicalDeviceProperties2(B.phys, &props);
    B.hostAlign = hp.minImportedHostPointerAlignment;
    static VkExtensionProperties exts[512];
    uint32_t next = 512;
    B.f.EnumerateDeviceExtensionProperties(B.phys, nullptr, &next, exts);
    auto has = [&](const char *name) {
        for (uint32_t i = 0; i < next; i++)
            if (!strcmp(exts[i].extensionName, name))
                return true;
        return false;
    };
    const bool host = has(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    // Can a buffer's memory be exported as an opaque fd at all?
    VkPhysicalDeviceExternalBufferInfo ebi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
    ebi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    ebi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExternalBufferProperties ebp = {VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    B.f.GetPhysicalDeviceExternalBufferProperties(B.phys, &ebi, &ebp);

    static VkQueueFamilyProperties qf[8];
    uint32_t nq = 8;
    B.f.GetPhysicalDeviceQueueFamilyProperties(B.phys, &nq, qf);
    uint32_t fam = UINT32_MAX;
    for (uint32_t i = 0; i < nq && fam == UINT32_MAX; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            fam = i;
    const float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = fam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char *devExts[] = {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = host ? 1 : 0;
    dci.ppEnabledExtensionNames = devExts;
    r = fam == UINT32_MAX ? VK_ERROR_INITIALIZATION_FAILED : B.f.CreateDevice(B.phys, &dci, nullptr, &B.dev);
    if (r == VK_SUCCESS) {
        B.f.GetDeviceQueue(B.dev, fam, 0, &B.queue);
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = fam;
        B.f.CreateCommandPool(B.dev, &pci, nullptr, &B.pool);
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = B.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        B.f.AllocateCommandBuffers(B.dev, &cai, &B.cmd);
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        B.f.CreateFence(B.dev, &fci, nullptr, &B.fence);
    }
    const bool ok = r == VK_SUCCESS && host && B.hostAlign && B.hostAlign <= kPage;
    spike15_result("share-caps", ok,
                   "second VkInstance + VkDevice on the same RADV (%d); %u device extensions: "
                   "VK_KHR_external_memory_fd %d, VK_KHR_external_semaphore_fd %d, VK_KHR_external_fence_fd %d, "
                   "VK_EXT_external_memory_host %d (alignment %llu); opaque-fd buffer export features 0x%x",
                   (int)r, next, has("VK_KHR_external_memory_fd"), has("VK_KHR_external_semaphore_fd"),
                   has("VK_KHR_external_fence_fd"), host, (unsigned long long)B.hostAlign,
                   ebp.externalMemoryProperties.externalMemoryFeatures);
    return ok;
}

// --- the second device's work -------------------------------------------------------

struct BBuffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint8_t *map = nullptr;
};

// A host-visible buffer of B's own, or (at != nullptr) B's import of `at`.
bool b_buffer(BBuffer *o, VkDeviceSize bytes, VkBufferUsageFlags usage, void *at)
{
    VkExternalMemoryBufferCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, at ? &ext : nullptr};
    bci.size = bytes;
    bci.usage = usage;
    if (B.f.CreateBuffer(B.dev, &bci, nullptr, &o->buf) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    B.f.GetBufferMemoryRequirements(B.dev, o->buf, &req);
    VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = at;
    uint32_t bits = req.memoryTypeBits;
    if (at) {
        VkMemoryHostPointerPropertiesEXT hpp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (B.f.GetMemoryHostPointerPropertiesEXT(B.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, at,
                                                  &hpp) != VK_SUCCESS)
            return false;
        bits &= hpp.memoryTypeBits;
    }
    const int type = mem_type(B, bits, at ? 0 : VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, at ? &imp : nullptr};
    ai.allocationSize = at ? bytes : req.size;
    ai.memoryTypeIndex = (uint32_t)type;
    if (type < 0 || B.f.AllocateMemory(B.dev, &ai, nullptr, &o->mem) != VK_SUCCESS ||
        B.f.BindBufferMemory(B.dev, o->buf, o->mem, 0) != VK_SUCCESS)
        return false;
    if (!at) {
        void *p = nullptr;
        if (B.f.MapMemory(B.dev, o->mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS)
            return false;
        o->map = (uint8_t *)p;
    }
    return true;
}

// B's alias of memory at `at` (bytes), as an image made from `ci`.
VkImage b_alias(const VkImageCreateInfo &ci, void *at, VkDeviceSize bytes, VkDeviceMemory *mem, VkDeviceSize *reqSize)
{
    VkImage img = VK_NULL_HANDLE;
    if (B.f.CreateImage(B.dev, &ci, nullptr, &img) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkMemoryRequirements req;
    B.f.GetImageMemoryRequirements(B.dev, img, &req);
    *reqSize = req.size;
    VkMemoryHostPointerPropertiesEXT hpp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    B.f.GetMemoryHostPointerPropertiesEXT(B.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, at, &hpp);
    VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = at;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp};
    ai.allocationSize = bytes;
    const int type = mem_type(B, req.memoryTypeBits & hpp.memoryTypeBits, 0);
    ai.memoryTypeIndex = (uint32_t)type;
    if (type < 0 || req.size > bytes || B.f.AllocateMemory(B.dev, &ai, nullptr, mem) != VK_SUCCESS ||
        B.f.BindImageMemory(B.dev, img, *mem, 0) != VK_SUCCESS) {
        B.f.DestroyImage(B.dev, img, nullptr);
        return VK_NULL_HANDLE;
    }
    return img;
}

void barrier(VkImage img, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    B.f.CmdPipelineBarrier(B.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &b);
}

void b_begin()
{
    B.f.ResetCommandBuffer(B.cmd, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    B.f.BeginCommandBuffer(B.cmd, &bi);
}

// Submit and wait on the CPU: the two devices have no shared semaphores.
double b_submit_wait()
{
    B.f.EndCommandBuffer(B.cmd);
    const double t0 = spike15_now_ms();
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &B.cmd;
    B.f.ResetFences(B.dev, 1, &B.fence);
    B.f.QueueSubmit(B.queue, 1, &si, B.fence);
    B.f.WaitForFences(B.dev, 1, &B.fence, VK_TRUE, UINT64_MAX);
    return spike15_now_ms() - t0;
}

VkBufferImageCopy region()
{
    VkBufferImageCopy r = {};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {kW, kH, 1};
    return r;
}

// --- ANGLE's image ------------------------------------------------------------------

struct AImage {
    VkImageCreateInfo ci = {};
    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize bytes = 0, reqSize = 0;
    uint8_t *ptr = nullptr;
    EGLImageKHR egl = EGL_NO_IMAGE_KHR;
    GLuint tex = 0, fbo = 0;
    GLenum layout = GL_NONE;
};

void image_info(VkImageCreateInfo *ci, const void *next)
{
    *ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, next};
    ci->imageType = VK_IMAGE_TYPE_2D;
    ci->format = VK_FORMAT_R8G8B8A8_UNORM;
    ci->extent = {kW, kH, 1};
    ci->mipLevels = 1;
    ci->arrayLayers = 1;
    ci->samples = VK_SAMPLE_COUNT_1_BIT;
    ci->tiling = VK_IMAGE_TILING_OPTIMAL;
    ci->usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

// On ANGLE's device, in mapped memory (whose address is its GPU address),
// then wrapped for GL. `external`: with the external-memory struct the
// second device's alias must carry.
bool a_image(AImage *a, bool external)
{
    a->ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    image_info(&a->ci, external ? &a->ext : nullptr);
    if (A.f.CreateImage(A.dev, &a->ci, nullptr, &a->img) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    A.f.GetImageMemoryRequirements(A.dev, a->img, &req);
    a->reqSize = req.size;
    a->bytes = (req.size + kPage - 1) & ~(kPage - 1);  // whole pages, to import
    const int type = mem_type(A, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = a->img;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded};
    ai.allocationSize = a->bytes;
    ai.memoryTypeIndex = (uint32_t)type;
    void *p = nullptr;
    if (type < 0 || A.f.AllocateMemory(A.dev, &ai, nullptr, &a->mem) != VK_SUCCESS ||
        A.f.BindImageMemory(A.dev, a->img, a->mem, 0) != VK_SUCCESS ||
        A.f.MapMemory(A.dev, a->mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS)
        return false;
    a->ptr = (uint8_t *)p;
    const uint64_t info = (uint64_t)(uintptr_t)&a->ci;
    const EGLint attrs[] = {EGL_VULKAN_IMAGE_CREATE_INFO_HI_ANGLE, (EGLint)(info >> 32),
                            EGL_VULKAN_IMAGE_CREATE_INFO_LO_ANGLE, (EGLint)(info & 0xffffffffu), EGL_NONE};
    a->egl = EGL_CreateImageKHR(g_dpy, EGL_NO_CONTEXT, EGL_VULKAN_IMAGE_ANGLE, (EGLClientBuffer)&a->img, attrs);
    if (a->egl == EGL_NO_IMAGE_KHR)
        return false;
    GL_GenTextures(1, &a->tex);
    GL_BindTexture(GL_TEXTURE_2D, a->tex);
    GL_EGLImageTargetTexture2DOES(GL_TEXTURE_2D, a->egl);
    GL_GenFramebuffers(1, &a->fbo);
    GL_BindFramebuffer(GL_FRAMEBUFFER, a->fbo);
    GL_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a->tex, 0);
    return GL_CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

void a_free(AImage *a)
{
    GL_DeleteFramebuffers(1, &a->fbo);
    GL_DeleteTextures(1, &a->tex);
    EGL_DestroyImageKHR(g_dpy, a->egl);
    GL_Finish();
    A.f.DestroyImage(A.dev, a->img, nullptr);
    A.f.FreeMemory(A.dev, a->mem, nullptr);
}

// A control is reported, not judged.
void report(const char *name, bool control, bool pass, const char *fmt, ...)
{
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (control)
        spike15_log("RESULT 15e-vk-share %s INFO %s", name, buf);
    else
        spike15_result(name, pass, "%s", buf);
}

// --- the checks -------------------------------------------------------------------

// ANGLE draws, B reads its alias.
void test_a_to_b(const char *variant, bool aExternal)
{
    char name[48];
    snprintf(name, sizeof name, "share-a-to-b-%s", variant);
    AImage a;
    if (!a_image(&a, aExternal)) {
        spike15_result(name, 0, "ANGLE's image: 0x%04x", EGL_GetError());
        return;
    }
    GL_AcquireTexturesANGLE(1, &a.tex, &a.layout);
    GL_BindFramebuffer(GL_FRAMEBUFFER, a.fbo);
    GL_Viewport(0, 0, kW, kH);
    GL_UseProgram(g_pattern);
    GL_BindVertexArray(g_vao);
    GL_DrawArrays(GL_TRIANGLES, 0, 3);
    GL_ReleaseTexturesANGLE(1, &a.tex, &a.layout);
    GL_Finish();

    VkImageCreateInfo ci;
    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    image_info(&ci, &ext);
    VkDeviceMemory bmem = VK_NULL_HANDLE;
    VkDeviceSize bReq = 0;
    VkImage bimg = b_alias(ci, a.ptr, a.bytes, &bmem, &bReq);
    BBuffer out;
    if (!bimg || !b_buffer(&out, (VkDeviceSize)kW * kH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, nullptr)) {
        spike15_result(name, 0, "B's alias of %p (%llu bytes, B wants %llu): failed", (void *)a.ptr,
                       (unsigned long long)a.bytes, (unsigned long long)bReq);
        return;
    }
    memset(out.map, 0x55, (size_t)kW * kH * 4);
    b_begin();
    barrier(bimg, vk_layout(a.layout), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkBufferImageCopy rg = region();
    B.f.CmdCopyImageToBuffer(B.cmd, bimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.buf, 1, &rg);
    const double ms = b_submit_wait();
    uint32_t fx = 0, fy = 0;
    const uint64_t bad = count_bad(out.map, kW * 4, &fx, &fy);
    const bool control = !aExternal;
    report(name, control, bad == 0,
                   "ANGLE drew a %ux%u pattern (released in GL layout 0x%04x); the second device read its alias "
                   "(ANGLE's image %llu bytes, B's %llu): %llu of %u texels wrong%s; B's copy + wait %.2f ms%s",
                   kW, kH, a.layout, (unsigned long long)a.reqSize, (unsigned long long)bReq,
                   (unsigned long long)bad, kW * kH, bad ? "" : " (all right)", ms,
                   control ? " [control: ANGLE's image made without the external struct]" : "");
    if (bad)
        spike15_log("  first wrong texel (%u,%u)", fx, fy);
    B.f.DestroyImage(B.dev, bimg, nullptr);
    B.f.FreeMemory(B.dev, bmem, nullptr);
    B.f.DestroyBuffer(B.dev, out.buf, nullptr);
    B.f.FreeMemory(B.dev, out.mem, nullptr);
    a_free(&a);
}

// B writes its alias, ANGLE samples (reads) its image.
void test_b_to_a(const char *variant, bool aExternal)
{
    char name[48];
    snprintf(name, sizeof name, "share-b-to-a-%s", variant);
    AImage a;
    if (!a_image(&a, aExternal)) {
        spike15_result(name, 0, "ANGLE's image: 0x%04x", EGL_GetError());
        return;
    }
    VkImageCreateInfo ci;
    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    image_info(&ci, &ext);
    VkDeviceMemory bmem = VK_NULL_HANDLE;
    VkDeviceSize bReq = 0;
    VkImage bimg = b_alias(ci, a.ptr, a.bytes, &bmem, &bReq);
    BBuffer src;
    if (!bimg || !b_buffer(&src, (VkDeviceSize)kW * kH * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, nullptr)) {
        spike15_result(name, 0, "B's alias of %p: failed", (void *)a.ptr);
        return;
    }
    for (uint32_t y = 0; y < kH; y++)
        for (uint32_t x = 0; x < kW; x++)
            expect(x, y, src.map + ((size_t)y * kW + x) * 4);
    b_begin();
    barrier(bimg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkBufferImageCopy rg = region();
    B.f.CmdCopyBufferToImage(B.cmd, src.buf, bimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    barrier(bimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const double ms = b_submit_wait();

    // ANGLE takes it over in the layout B left it in.
    GLenum layout = GL_LAYOUT_SHADER_READ_ONLY_EXT;
    GL_AcquireTexturesANGLE(1, &a.tex, &layout);
    std::vector<uint8_t> px((size_t)kW * kH * 4, 0x55);
    GL_BindFramebuffer(GL_FRAMEBUFFER, a.fbo);
    GL_ReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    GL_ReleaseTexturesANGLE(1, &a.tex, &a.layout);
    const GLenum e = GL_GetError();
    uint32_t fx = 0, fy = 0;
    const uint64_t bad = count_bad(px.data(), kW * 4, &fx, &fy);
    const bool control = !aExternal;
    report(name, control, bad == 0 && e == GL_NO_ERROR,
                   "the second device wrote a %ux%u pattern into its alias (copy + wait %.2f ms); ANGLE read its "
                   "image: %llu of %u texels wrong%s, GL error 0x%x%s",
                   kW, kH, ms, (unsigned long long)bad, kW * kH, bad ? "" : " (all right)", e,
                   control ? " [control]" : "");
    if (bad)
        spike15_log("  first wrong texel (%u,%u)", fx, fy);
    B.f.DestroyImage(B.dev, bimg, nullptr);
    B.f.FreeMemory(B.dev, bmem, nullptr);
    B.f.DestroyBuffer(B.dev, src.buf, nullptr);
    B.f.FreeMemory(B.dev, src.mem, nullptr);
    a_free(&a);
}

// The linear fallback, per frame: B copies its image into a buffer aliasing
// an ANGLE GL buffer; ANGLE uploads that buffer into a texture on the GPU.
void test_buffer()
{
    const VkDeviceSize bytes = (VkDeviceSize)kW * kH * 4;
    const VkDeviceSize span = ((bytes + kPage - 1) & ~(kPage - 1)) + kPage;
    GLuint glbuf = 0;
    GL_GenBuffers(1, &glbuf);
    GL_BindBuffer(GL_PIXEL_UNPACK_BUFFER, glbuf);
    const GLbitfield fl = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;
    GL_BufferStorageEXT(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)span, nullptr, fl);
    uint8_t *map = (uint8_t *)GL_MapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr)span, fl);
    if (!map) {
        spike15_result("share-buffer", 0, "ANGLE buffer map: GL error 0x%x", GL_GetError());
        return;
    }
    uint8_t *at = (uint8_t *)(((uintptr_t)map + kPage - 1) & ~(uintptr_t)(kPage - 1));
    const size_t off = (size_t)(at - map);
    BBuffer alias, src;
    // B's own image with the pattern, as Dawn's canvas texture would be.
    VkImageCreateInfo ci;
    image_info(&ci, nullptr);
    VkImage bimg = VK_NULL_HANDLE;
    VkDeviceMemory imem = VK_NULL_HANDLE;
    bool ok = b_buffer(&alias, (bytes + kPage - 1) & ~(kPage - 1), VK_BUFFER_USAGE_TRANSFER_DST_BIT, at) &&
              b_buffer(&src, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, nullptr) &&
              B.f.CreateImage(B.dev, &ci, nullptr, &bimg) == VK_SUCCESS;
    if (ok) {
        VkMemoryRequirements req;
        B.f.GetImageMemoryRequirements(B.dev, bimg, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = (uint32_t)mem_type(B, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ok = B.f.AllocateMemory(B.dev, &ai, nullptr, &imem) == VK_SUCCESS &&
             B.f.BindImageMemory(B.dev, bimg, imem, 0) == VK_SUCCESS;
    }
    if (!ok) {
        spike15_result("share-buffer", 0, "B's buffers or image: failed (ANGLE buffer at %p)", (void *)map);
        return;
    }
    for (uint32_t y = 0; y < kH; y++)
        for (uint32_t x = 0; x < kW; x++)
            expect(x, y, src.map + ((size_t)y * kW + x) * 4);
    const VkBufferImageCopy rg = region();
    b_begin();
    barrier(bimg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    B.f.CmdCopyBufferToImage(B.cmd, src.buf, bimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    barrier(bimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    b_submit_wait();

    GLuint tex = 0, fbo = 0;
    GL_GenTextures(1, &tex);
    GL_BindTexture(GL_TEXTURE_2D, tex);
    GL_TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kW, kH);
    GL_GenFramebuffers(1, &fbo);
    GL_BindFramebuffer(GL_FRAMEBUFFER, fbo);
    GL_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    const int kFrames = 30;
    double bms = 0, ams = 0;
    for (int f = 0; f < kFrames; f++) {
        memset(at, 0x55, 64);
        b_begin();
        B.f.CmdCopyImageToBuffer(B.cmd, bimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, alias.buf, 1, &rg);
        bms += b_submit_wait();
        const double t0 = spike15_now_ms();
        GL_BindBuffer(GL_PIXEL_UNPACK_BUFFER, glbuf);
        GL_BindTexture(GL_TEXTURE_2D, tex);
        GL_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, (const void *)(uintptr_t)off);
        GL_Finish();
        ams += spike15_now_ms() - t0;
    }
    GL_BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    std::vector<uint8_t> px((size_t)kW * kH * 4, 0x55);
    GL_BindFramebuffer(GL_FRAMEBUFFER, fbo);
    GL_ReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    const GLenum e = GL_GetError();
    uint32_t fx = 0, fy = 0;
    const uint64_t bad = count_bad(px.data(), kW * 4, &fx, &fy);
    spike15_result("share-buffer", bad == 0 && e == GL_NO_ERROR,
                   "%ux%u, %d frames: the second device's image copied into its import of an ANGLE GL buffer "
                   "(%p, +%zu) %.2f ms, then ANGLE's upload from that buffer (PIXEL_UNPACK) %.2f ms, a frame "
                   "(each to completion); %llu of %u texels wrong; GL error 0x%x",
                   kW, kH, kFrames, (void *)map, off, bms / kFrames, ams / kFrames, (unsigned long long)bad, kW * kH,
                   e);
    if (bad)
        spike15_log("  first wrong texel (%u,%u)", fx, fy);
    GL_DeleteFramebuffers(1, &fbo);
    GL_DeleteTextures(1, &tex);
    GL_DeleteBuffers(1, &glbuf);
    B.f.DestroyImage(B.dev, bimg, nullptr);
    B.f.FreeMemory(B.dev, imem, nullptr);
    B.f.DestroyBuffer(B.dev, alias.buf, nullptr);
    B.f.FreeMemory(B.dev, alias.mem, nullptr);
    B.f.DestroyBuffer(B.dev, src.buf, nullptr);
    B.f.FreeMemory(B.dev, src.mem, nullptr);
}

}  // namespace

void vk_share_tests_run(void)
{
    if (!angle_up() || !second_device_up())
        return;
    test_a_to_b("same", true);
    test_b_to_a("same", true);
    test_a_to_b("control", false);
    test_b_to_a("control", false);
    test_buffer();
    EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    EGL_Terminate(g_dpy);
    spike15_log("share tests done");
}
