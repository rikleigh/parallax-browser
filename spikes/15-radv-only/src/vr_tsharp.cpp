// vr_tsharp.cpp - spike 15c: an Hmd2 eye buffer from a RADV VkImage, drawn
// through ANGLE's GL on Vulkan, in a title with no ps5-opengl.
//
// Today (ps5-opengl) PS5RendererVR exports its three GL eye textures as T#s
// through eglExportTextureDescriptorPS5 (our ps5-opengl patch 0002); Hmd2's
// reprojection samples them in SW_64KB_R_X. On RADV, as WebKit's ANGLE would
// do it:
//
//   1. ANGLE's own VkDevice (EGL_EXT_device_query + EGL_ANGLE_device_vulkan);
//   2. a VkImage on it laid out as the display scans out: Mesa's internal
//      wsi_image_create_info { scanout = true }, which PS5_Mesa's RADV turns
//      into the display's fixed swizzle, SW_64KB_R_X, with DCC off
//      (radv_image.c, has_fixed_display_swizzle). No change to the fork;
//   3. memory from RADV, mapped: a PS5 RADV buffer has one address for the
//      CPU and the GPU (winsys/ps5/radv_ps5_bo.c), so the mapping is the
//      address the T# carries;
//   4. the image as a GL texture (EGL_ANGLE_vulkan_image: EGL_VULKAN_IMAGE_ANGLE
//      and glAcquireTexturesANGLE / glReleaseTexturesANGLE around each frame,
//      the WebXR hand-off);
//   5. the T# from psvr2_build_tsharp_rgba8(address, 27 = SW_64KB_R_X).
//
// Checks:
//   vr-device       ANGLE's VkInstance/VkPhysicalDevice/VkDevice
//   vr-image        the scanout image: size, alignment, memory type, address
//   vr-egl-image    the VkImage as an ES texture + complete FBO
//   vr-layout       a coordinate-coded pattern drawn with GL, read from the raw
//                   memory through the console-measured RGBA8 SW_64KB_R_X map
//                   (ref/PS5_Vulkan driver/ps5vk_image.c,
//                   ps5vk_tiled_texel_offset): every texel where the map puts
//                   it, and the row order (which way is up for Hmd2)
//   vr-tsharp       the three eye buffers' T#s
//   vr-headset      with a PS VR2 connected: a test card through Hmd2
//                   at 60 frames/s for 60 s (the owner looks); without one, skipped
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

#include "common/psvr2/psvr2.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
void spike15_log(const char *fmt, ...);
void spike15_result(const char *test, int pass, const char *fmt, ...);
double spike15_now_ms(void);
void vr_tsharp_tests_run(int preload_rc);
// RADV's ICD entry point (radv-local.o keeps it global).
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance, const char *);
int sceUserServiceInitialize(void *);
int sceUserServiceGetInitialUser(int *);
}

namespace {

// Mesa's private image create info (src/vulkan/util/vk_internal_exts.h).
constexpr VkStructureType kWsiImageCreateInfoMesa = (VkStructureType)1000001002;
struct WsiImageCreateInfo {
    VkStructureType sType;
    const void *pNext;
    bool scanout;
    bool blit_src;
};

constexpr uint32_t kEye = 2048;  // PS5RendererVR's kEyeSize
constexpr uint32_t kW = 2 * kEye, kH = kEye;
constexpr uint32_t kSwizzle64KbRX = 27;
constexpr int kBuffers = PSVR2_MAX_BUFFERS;

EGLDisplay g_dpy = EGL_NO_DISPLAY;

struct Vk {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties memProps = nullptr;
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements imageReqs = nullptr;
    PFN_vkAllocateMemory allocate = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindImageMemory bind = nullptr;
    PFN_vkMapMemory map = nullptr;
} vk;

struct EyeBuffer {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageCreateInfo info = {};
    uint8_t *cpu = nullptr;  // = the GPU address
    VkDeviceSize size = 0;
    EGLImageKHR egl = EGL_NO_IMAGE_KHR;
    GLuint tex = 0, fbo = 0;
    GLenum layout = GL_NONE;  // the image's layout while ANGLE doesn't hold it
    uint32_t tsharp[8] = {};
};

// --- the console-measured RGBA8 SW_64KB_R_X map -----------------------------
// ps5vk_tiled_texel_offset's 4-byte, one-sample case (ref/PS5_Vulkan): 128x128
// texel tiles of 64 KiB in row-major order, an XOR map inside a tile.
uint64_t rx_offset(uint32_t x, uint32_t y, uint32_t width)
{
    const uint32_t sx = x & 127u, sy = y & 127u;
    const uint64_t swz = ((sx << 2) & 0x0cu) ^ ((sx << 5) & 0x380u) ^ ((sx << 4) & 0x400u) ^ ((sx << 6) & 0x800u) ^
                         ((sx << 9) & 0xa000u) ^ ((sy << 4) & 0x70u) ^ ((sy << 5) & 0xf00u) ^ ((sy << 9) & 0x1000u) ^
                         ((sy << 8) & 0x4000u);
    const uint64_t tiles_per_row = (width + 127u) / 128u;
    return ((uint64_t)(y >> 7) * tiles_per_row + (x >> 7)) * 0x10000u + swz;
}

// The pattern: texel (x, y) in GL window coordinates (y = gl_FragCoord.y).
void expect(uint32_t x, uint32_t y, uint8_t out[4])
{
    out[0] = (uint8_t)(x & 255);
    out[1] = (uint8_t)(y & 255);
    out[2] = (uint8_t)(((x >> 8) & 15) | (((y >> 8) & 15) << 4));
    out[3] = 255;
}

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

GLuint compile(GLenum type, const char *src, char *log, size_t n)
{
    GLuint s = GL_CreateShader(type);
    GL_ShaderSource(s, 1, &src, nullptr);
    GL_CompileShader(s);
    GLint ok = 0;
    GL_GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GL_GetShaderInfoLog(s, (GLsizei)n, nullptr, log);
        return 0;
    }
    return s;
}

GLuint program(const char *vs, const char *fs, char *log, size_t n)
{
    log[0] = 0;
    GLuint v = compile(GL_VERTEX_SHADER, vs, log, n), f = v ? compile(GL_FRAGMENT_SHADER, fs, log, n) : 0;
    if (!v || !f)
        return 0;
    GLuint p = GL_CreateProgram();
    GL_AttachShader(p, v);
    GL_AttachShader(p, f);
    GL_LinkProgram(p);
    GLint ok = 0;
    GL_GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GL_GetProgramInfoLog(p, (GLsizei)n, nullptr, log);
        return 0;
    }
    return p;
}

// --- set-up ------------------------------------------------------------------

bool angle_up()
{
    const EGLAttrib attrs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE,
        EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE, EGL_PLATFORM_VULKAN_DISPLAY_MODE_SIMPLE_ANGLE,
        EGL_NONE,
    };
    g_dpy = EGL_GetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, attrs);
    EGLint major = 0, minor = 0;
    if (g_dpy == EGL_NO_DISPLAY || !EGL_Initialize(g_dpy, &major, &minor)) {
        spike15_result("vr-device", 0, "ANGLE display: 0x%04x", EGL_GetError());
        return false;
    }
    // A config-less ES 3.0 context, surfaceless: the eye buffers are FBOs.
    const EGLint ctxAttrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    EGLContext ctx = EGL_CreateContext(g_dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxAttrs);
    if (ctx == EGL_NO_CONTEXT || !EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        spike15_result("vr-device", 0, "ES 3.0 context: 0x%04x", EGL_GetError());
        return false;
    }
    const char *ext = (const char *)GL_GetString(GL_EXTENSIONS);
    if (!ext || !strstr(ext, "GL_ANGLE_vulkan_image") || !strstr(ext, "GL_OES_EGL_image")) {
        spike15_result("vr-device", 0, "GL_ANGLE_vulkan_image %d, GL_OES_EGL_image %d",
                       ext && strstr(ext, "GL_ANGLE_vulkan_image"), ext && strstr(ext, "GL_OES_EGL_image"));
        return false;
    }

    EGLAttrib dev = 0, inst = 0, phys = 0, device = 0;
    if (!EGL_QueryDisplayAttribEXT(g_dpy, EGL_DEVICE_EXT, &dev) ||
        !EGL_QueryDeviceAttribEXT((EGLDeviceEXT)dev, EGL_VULKAN_INSTANCE_ANGLE, &inst) ||
        !EGL_QueryDeviceAttribEXT((EGLDeviceEXT)dev, EGL_VULKAN_PHYSICAL_DEVICE_ANGLE, &phys) ||
        !EGL_QueryDeviceAttribEXT((EGLDeviceEXT)dev, EGL_VULKAN_DEVICE_ANGLE, &device)) {
        spike15_result("vr-device", 0, "EGL device query: 0x%04x", EGL_GetError());
        return false;
    }
    vk.instance = (VkInstance)inst;
    vk.phys = (VkPhysicalDevice)phys;
    vk.device = (VkDevice)device;
    vk.gdpa = (PFN_vkGetDeviceProcAddr)vk_icdGetInstanceProcAddr(vk.instance, "vkGetDeviceProcAddr");
    vk.memProps = (PFN_vkGetPhysicalDeviceMemoryProperties)vk_icdGetInstanceProcAddr(
        vk.instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!vk.gdpa || !vk.memProps) {
        spike15_result("vr-device", 0, "no vkGetDeviceProcAddr / memory properties");
        return false;
    }
#define DEV(field, name) vk.field = (PFN_##name)vk.gdpa(vk.device, #name)
    DEV(createImage, vkCreateImage);
    DEV(destroyImage, vkDestroyImage);
    DEV(imageReqs, vkGetImageMemoryRequirements);
    DEV(allocate, vkAllocateMemory);
    DEV(freeMemory, vkFreeMemory);
    DEV(bind, vkBindImageMemory);
    DEV(map, vkMapMemory);
#undef DEV
    const bool ok = vk.createImage && vk.destroyImage && vk.imageReqs && vk.allocate && vk.freeMemory && vk.bind &&
                    vk.map;
    spike15_result("vr-device", ok, "ANGLE's VkInstance %p, VkPhysicalDevice %p, VkDevice %p; GL \"%s\"",
                   (void *)vk.instance, (void *)vk.phys, (void *)vk.device, GL_GetString(GL_VERSION));
    return ok;
}

// The scanout image in RADV memory, mapped.
bool make_image(EyeBuffer *b, int index, bool log)
{
    static const WsiImageCreateInfo scanout = {kWsiImageCreateInfoMesa, nullptr, true, false};
    b->info = {};
    b->info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    b->info.pNext = &scanout;
    b->info.imageType = VK_IMAGE_TYPE_2D;
    b->info.format = VK_FORMAT_R8G8B8A8_UNORM;
    b->info.extent = {kW, kH, 1};
    b->info.mipLevels = 1;
    b->info.arrayLayers = 1;
    b->info.samples = VK_SAMPLE_COUNT_1_BIT;
    b->info.tiling = VK_IMAGE_TILING_OPTIMAL;
    b->info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    b->info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    b->info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = vk.createImage(vk.device, &b->info, nullptr, &b->image);
    if (r != VK_SUCCESS) {
        spike15_result("vr-image", 0, "vkCreateImage %d", (int)r);
        return false;
    }
    VkMemoryRequirements req;
    vk.imageReqs(vk.device, b->image, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vk.memProps(vk.phys, &mp);
    // Host-visible and coherent (the address must be mappable), device-local
    // if there is such a type.
    int type = -1;
    const VkMemoryPropertyFlags need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (int pass = 0; pass < 2 && type < 0; pass++)
        for (uint32_t i = 0; i < mp.memoryTypeCount && type < 0; i++) {
            const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
            if ((req.memoryTypeBits & (1u << i)) && (f & need) == need &&
                (pass == 1 || (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
                type = (int)i;
        }
    if (type < 0) {
        spike15_result("vr-image", 0, "no host-visible memory type in 0x%x", req.memoryTypeBits);
        return false;
    }
    VkMemoryDedicatedAllocateInfo dedicated = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = b->image;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)type;
    r = vk.allocate(vk.device, &ai, nullptr, &b->memory);
    if (r == VK_SUCCESS)
        r = vk.bind(vk.device, b->image, b->memory, 0);
    void *p = nullptr;
    if (r == VK_SUCCESS)
        r = vk.map(vk.device, b->memory, 0, VK_WHOLE_SIZE, 0, &p);
    if (r != VK_SUCCESS) {
        spike15_result("vr-image", 0, "allocate/bind/map %d", (int)r);
        return false;
    }
    b->cpu = (uint8_t *)p;
    b->size = req.size;
    const uint64_t tiled = (uint64_t)((kW + 127) / 128) * ((kH + 127) / 128) * 0x10000;
    const bool ok = req.size == tiled && ((uintptr_t)p & 0xffff) == 0;
    if (log || !ok)
        spike15_result("vr-image", ok,
                       "buffer %d: %ux%u RGBA8 scanout image, %llu bytes (a 64 KiB tile grid is %llu), alignment %llu, "
                       "memory type %d flags 0x%x heap %u, address %p",
                       index, kW, kH, (unsigned long long)req.size, (unsigned long long)tiled,
                       (unsigned long long)req.alignment, type, mp.memoryTypes[type].propertyFlags,
                       mp.memoryTypes[type].heapIndex, p);
    return ok;
}

bool make_gl(EyeBuffer *b, int index, bool log)
{
    const uint64_t info = (uint64_t)(uintptr_t)&b->info;
    const EGLint attrs[] = {
        EGL_VULKAN_IMAGE_CREATE_INFO_HI_ANGLE, (EGLint)(info >> 32),
        EGL_VULKAN_IMAGE_CREATE_INFO_LO_ANGLE, (EGLint)(info & 0xffffffffu),
        EGL_NONE,
    };
    b->egl = EGL_CreateImageKHR(g_dpy, EGL_NO_CONTEXT, EGL_VULKAN_IMAGE_ANGLE, (EGLClientBuffer)&b->image, attrs);
    if (b->egl == EGL_NO_IMAGE_KHR) {
        spike15_result("vr-egl-image", 0, "buffer %d: eglCreateImageKHR(EGL_VULKAN_IMAGE_ANGLE) 0x%04x", index,
                       EGL_GetError());
        return false;
    }
    GL_GenTextures(1, &b->tex);
    GL_BindTexture(GL_TEXTURE_2D, b->tex);
    GL_EGLImageTargetTexture2DOES(GL_TEXTURE_2D, b->egl);
    GL_GenFramebuffers(1, &b->fbo);
    GL_BindFramebuffer(GL_FRAMEBUFFER, b->fbo);
    GL_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b->tex, 0);
    const GLenum st = GL_CheckFramebufferStatus(GL_FRAMEBUFFER);
    const GLenum e = GL_GetError();
    const bool ok = st == GL_FRAMEBUFFER_COMPLETE && e == GL_NO_ERROR;
    if (log || !ok)
        spike15_result("vr-egl-image", ok, "buffer %d: EGL image -> texture %u, FBO status 0x%04x, GL error 0x%04x",
                       index, b->tex, st, e);
    return ok;
}

// ANGLE holds the image between these (the WebXR hand-off).
void acquire(EyeBuffer *b) { GL_AcquireTexturesANGLE(1, &b->tex, &b->layout); }
void release(EyeBuffer *b) { GL_ReleaseTexturesANGLE(1, &b->tex, &b->layout); }

// --- the layout check --------------------------------------------------------

void test_layout(EyeBuffer *b)
{
    char log[512];
    GLuint prog = program(kVsFull, kFsPattern, log, sizeof log);
    if (!prog) {
        spike15_result("vr-layout", 0, "pattern shader: %s", log);
        return;
    }
    GLuint vao = 0;
    GL_GenVertexArrays(1, &vao);
    GL_BindVertexArray(vao);
    acquire(b);
    GL_BindFramebuffer(GL_FRAMEBUFFER, b->fbo);
    GL_Viewport(0, 0, kW, kH);
    GL_UseProgram(prog);
    const double t0 = spike15_now_ms();
    GL_DrawArrays(GL_TRIANGLES, 0, 3);
    // A readPixels makes ANGLE wait for the draw with a host-read barrier,
    // which writes the GPU's L2 back: the raw memory is then current. Its
    // value also checks GL's own view of texel (5, 3).
    uint8_t viaGl[4] = {};
    GL_ReadPixels(5, 3, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, viaGl);
    release(b);
    GL_Finish();
    const double t1 = spike15_now_ms();
    const GLenum e = GL_GetError();

    // Every texel through the measured map, rows as drawn (GL y = memory
    // row) and flipped; and, for contrast, a linear reading.
    uint64_t bad = 0, badFlip = 0, badLinear = 0;
    uint32_t firstBad[2] = {0, 0};
    uint8_t got[4] = {}, want[4];
    for (uint32_t y = 0; y < kH; y++)
        for (uint32_t x = 0; x < kW; x++) {
            const uint8_t *px = b->cpu + rx_offset(x, y, kW);
            expect(x, y, want);
            if (memcmp(px, want, 4) != 0) {
                if (!bad++) {
                    firstBad[0] = x;
                    firstBad[1] = y;
                    memcpy(got, px, 4);
                }
            }
            if (((x | y) & 7) == 0) {  // sampled: these are only to compare
                expect(x, kH - 1 - y, want);
                badFlip += memcmp(px, want, 4) != 0;
                expect(x, y, want);
                badLinear += memcmp(b->cpu + ((uint64_t)y * kW + x) * 4, want, 4) != 0;
            }
        }
    const double t2 = spike15_now_ms();
    const uint64_t sampled = (uint64_t)(kW / 8) * (kH / 8);
    spike15_result("vr-layout", bad == 0 && e == GL_NO_ERROR,
                   "%ux%u coordinate pattern drawn by GL, read raw through the measured SW_64KB_R_X map: %llu of %llu "
                   "texels wrong%s; flipped rows: %llu of %llu sampled wrong; linear: %llu of %llu wrong. GL readPixels "
                   "(5,3) = %u,%u,%u (want 5,3,0); draw+wait %.1f ms, CPU check %.0f ms",
                   kW, kH, (unsigned long long)bad, (unsigned long long)kW * kH, bad ? "" : " (all where the map puts them)",
                   (unsigned long long)badFlip, (unsigned long long)sampled, (unsigned long long)badLinear,
                   (unsigned long long)sampled, viaGl[0], viaGl[1], viaGl[2], t1 - t0, t2 - t1);
    if (bad)
        spike15_log("  first wrong texel (%u,%u): got %u,%u,%u,%u", firstBad[0], firstBad[1], got[0], got[1], got[2],
                    got[3]);
    if (!bad)
        spike15_log("  memory row 0 is GL row 0 (gl_FragCoord.y = 0.5): a frame for Hmd2 has its top at GL y = 0, "
                    "as Spike 3b's flip_y did on ps5-opengl");
    GL_DeleteProgram(prog);
    GL_DeleteVertexArrays(1, &vao);
}

// --- the headset ---------------------------------------------------------------

// A head-locked test card, drawn in angles (tangents) rather than pixels so
// both eyes agree and it fuses into one picture: the eyes' buffers cover
// different angles (each sees further outward than inward), so a shape at
// the same pixels in both would be seen twice. Scissored clears, no shader.
// Up = memory row 0 = GL y 0.
//   - dark grey, a white grid every 0.25 (tangent);
//   - a yellow band above the centre: up, and channel order (swapped red and
//     blue would make it cyan);
//   - one green bar sweeping left to right once a second;
//   - a red square lower left in the left eye's buffer only, a blue square
//     lower right in the right eye's only: close one eye and one goes.
void draw_card(uint64_t frame, const float tan[2][4])
{
    GL_Enable(GL_SCISSOR_TEST);
    for (int eye = 0; eye < 2; eye++) {
        const float L = tan[eye][0], R = tan[eye][1], U = tan[eye][2], D = tan[eye][3];
        const int x0 = eye * (int)kEye;
        // A rectangle given in tangents (x right, y up) -> eye pixels.
        auto box = [&](float ax0, float ay0, float ax1, float ay1, float r, float g, float b) {
            const int px0 = (int)((ax0 + L) / (L + R) * kEye), px1 = (int)((ax1 + L) / (L + R) * kEye);
            const int py0 = (int)((U - ay1) / (U + D) * kEye), py1 = (int)((U - ay0) / (U + D) * kEye);
            GL_Scissor(x0 + px0, py0, px1 - px0 > 1 ? px1 - px0 : 2, py1 - py0 > 1 ? py1 - py0 : 2);
            GL_ClearColor(r, g, b, 1);
            GL_Clear(GL_COLOR_BUFFER_BIT);
        };
        GL_Scissor(x0, 0, kEye, kEye);
        GL_ClearColor(0.12f, 0.12f, 0.12f, 1);
        GL_Clear(GL_COLOR_BUFFER_BIT);
        for (int i = -4; i <= 4; i++) {
            box(i * 0.25f - 0.004f, -1.0f, i * 0.25f + 0.004f, 1.0f, 0.8f, 0.8f, 0.8f);
            box(-1.0f, i * 0.25f - 0.004f, 1.0f, i * 0.25f + 0.004f, 0.8f, 0.8f, 0.8f);
        }
        box(-0.6f, 0.55f, 0.6f, 0.70f, 1.0f, 0.85f, 0.0f);
        const float bx = -0.8f + 1.6f * (float)(frame % 60) / 59.0f;
        box(bx - 0.03f, -0.3f, bx + 0.03f, 0.3f, 0.1f, 0.9f, 0.2f);
        if (eye == 0)
            box(-0.5f, -0.6f, -0.3f, -0.4f, 1.0f, 0.1f, 0.1f);
        else
            box(0.3f, -0.6f, 0.5f, -0.4f, 0.15f, 0.3f, 1.0f);
    }
    GL_Disable(GL_SCISSOR_TEST);
}

void test_headset(EyeBuffer *bufs, int preload_rc)
{
    if (preload_rc != 0) {
        spike15_result("vr-headset", 0, "psvr2_preload %d: Hmd2 not bound on this firmware", preload_rc);
        return;
    }
    psvr2_set_logger([](const char *line) { spike15_log("  psvr2: %s", line); });
    psvr2_require_headset(1);
    int user = -1;
    sceUserServiceInitialize(nullptr);
    sceUserServiceGetInitialUser(&user);
    const int rc = psvr2_open(user);
    if (rc == PSVR2_NO_HEADSET) {
        spike15_log("RESULT 15c-vr-tsharp vr-headset SKIP no PS VR2 connected (Hmd2 device status not 0)");
        return;
    }
    if (rc != 0) {
        spike15_result("vr-headset", 0, "psvr2_open %d (user 0x%x)", rc, user);
        return;
    }
    uint32_t t[kBuffers][8];
    for (int i = 0; i < kBuffers; i++)
        memcpy(t[i], bufs[i].tsharp, sizeof t[i]);
    if (psvr2_start(t, kBuffers, kEye, kEye) != 0) {
        spike15_result("vr-headset", 0, "psvr2_start refused the RADV T#s");
        psvr2_close();
        return;
    }
    float tan[2][4];
    psvr2_eye_tangents(0, 1.0f, tan[0]);
    psvr2_eye_tangents(1, 1.0f, tan[1]);
    GL_Viewport(0, 0, kW, kH);
    unsigned frames = 0, errors = 0, slow = 0, tracked = 0;
    double render = 0, worst = 0;
    const double t0 = spike15_now_ms();
    while (spike15_now_ms() - t0 < 60000.0) {  // time to set VR up first
        psvr2_frame_t f;
        if (psvr2_begin_frame(&f) != 0) {
            errors++;
            continue;
        }
        tracked += f.tracked != 0;
        EyeBuffer *b = &bufs[f.frame % kBuffers];
        const double r0 = spike15_now_ms();
        acquire(b);
        GL_BindFramebuffer(GL_FRAMEBUFFER, b->fbo);
        draw_card(f.frame, tan);
        release(b);
        GL_Finish();  // Hmd2 must not sample a half-drawn buffer
        const double r1 = spike15_now_ms();
        if (psvr2_submit((int)(f.frame % kBuffers), &f, tan) != 0)
            errors++;
        render += r1 - r0;
        worst = r1 - r0 > worst ? r1 - r0 : worst;
        slow += r1 - r0 > 16.0;
        frames++;
    }
    const double secs = (spike15_now_ms() - t0) / 1000.0;
    psvr2_close();
    const double fps = frames / secs;
    spike15_result("vr-headset", fps > 55 && errors == 0,
                   "%u frames in %.1f s (%.1f frames/s), %u tracked, %u submit/begin errors; draw+finish %.2f ms "
                   "average, worst %.2f, %u over 16 ms. Owner: see the README for what the headset should show",
                   frames, secs, fps, tracked, errors, frames ? render / frames : 0.0, worst, slow);
}

}  // namespace

void vr_tsharp_tests_run(int preload_rc)
{
    if (!angle_up())
        return;
    EyeBuffer bufs[kBuffers];
    for (int i = 0; i < kBuffers; i++)
        if (!make_image(&bufs[i], i, i == 0) || !make_gl(&bufs[i], i, i == 0))
            return;
    test_layout(&bufs[0]);
    char line[256] = "";
    bool ok = true;
    for (int i = 0; i < kBuffers; i++) {
        // The builder wants a pitch of at least a row even when the swizzle
        // implies it.
        ok &= psvr2_build_tsharp_rgba8(bufs[i].tsharp, (uint64_t)(uintptr_t)bufs[i].cpu, kW, kH, kW * 4,
                                       kSwizzle64KbRX) == 0;
        const uint32_t *t = bufs[i].tsharp;
        snprintf(line, sizeof line, "%08x %08x %08x %08x %08x %08x %08x %08x", t[0], t[1], t[2], t[3], t[4], t[5],
                 t[6], t[7]);
        spike15_log("  buffer %d at %p: T# %s", i, (void *)bufs[i].cpu, line);
    }
    spike15_result("vr-tsharp", ok, "%d T#s, %ux%u RGBA8 SW_64KB_R_X over RADV memory", kBuffers, kW, kH);
    test_headset(bufs, preload_rc);
    EGL_MakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    EGL_Terminate(g_dpy);
    spike15_log("vr tests done");
}
