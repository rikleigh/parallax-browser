/*
 * Copyright (C) 2026 ps5webkit contributors
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.
 */

#include "config.h"
#include "PS5Renderer.h"
#include "PS5GLES.h"

#include "PS5Compositor.h"
#include "PS5GLBench.h"
#include "PS5PerfOverlay.h"
#include "PS5Text.h"
#include "PS5VideoDecoder.h"
#include "PS5VideoServer.h"
#include <algorithm>
#include <cmath>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// The title's GPU submission lock (apps/title/titleradv.c; WebGPU builds only):
// held around this thread's GL work so that RADV's submissions never fall
// between two of ps5-opengl's, or the other way round.
extern "C" void ps5wk_gpu_submit_lock(void) __attribute__((weak));
extern "C" int ps5wk_gpu_submit_unlock(void) __attribute__((weak));
// ps5-opengl: eglSwapBuffers queues the flip and returns; the first draw into the back buffer of
// the next frame waits for it. This is that wait, on its own.
extern "C" int ps5_agc_gate2_wait_present(void) __attribute__((weak));

namespace {
#if PS5WK_GPU_RADV
// GPU=radv: the compositor, WebGL and WebGPU are all RADV, whose PS5 winsys puts the process's
// submissions in order on one queue: no lock, and no flush of its own before the swap.
struct GPUSubmitLock {
    void release() { }
};
#else
struct GPUSubmitLock {
    GPUSubmitLock()
    {
        if (ps5wk_gpu_submit_lock)
            ps5wk_gpu_submit_lock();
    }
    ~GPUSubmitLock() { release(); }
    // After a glFlush(): what this thread drew is submitted.
    void release()
    {
        if (m_released || !ps5wk_gpu_submit_unlock)
            return;
        m_released = true;
        glFlush();
        ps5wk_gpu_submit_unlock();
    }
    bool m_released { false };
};
#endif
}

// GL is reached through libepoxy. In the title its dlopen()/dlsym() resolve
// to the statically linked ps5-opengl (port/webkit/apps/title/dlshim.c).

PS5Renderer& PS5Renderer::singleton()
{
    static PS5Renderer renderer;
    return renderer;
}

static int64_t nowUS()
{
    return g_get_monotonic_time();
}

// A quad at rect (NDC); uv (0,0) is the top-left of the texture.
static const char* kVertexShader = R"(#version 430 core
uniform vec4 rect; // x0 y0 x1 y1 in NDC
out vec2 uv;
void main()
{
    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    uv = vec2(c.x, 1.0 - c.y);
    gl_Position = vec4(mix(rect.xy, rect.zw, c), 0.0, 1.0);
}
)";

static const char* kFragmentShader = R"(#version 430 core
uniform sampler2D tex;
uniform int opaque_;
in vec2 uv;
out vec4 color;
void main()
{
    vec4 c = texture(tex, uv);
    color = opaque_ != 0 ? vec4(c.rgb, 1.0) : c;
}
)";

// The page straight from its persistent GL buffer (premultiplied BGRA rows,
// as the WebProcess painted them). pageRect: where the page sits on the
// screen in pixels, top-down.
static const char* kPageFragmentShader = R"(#version 430 core
layout(std430, binding = 1) readonly buffer Page { uint px[]; };
uniform vec4 pageRect;
uniform ivec2 pageSize;
uniform int pageStride; // in pixels
uniform int screenHeight;
out vec4 color;
void main()
{
    vec2 s = vec2(gl_FragCoord.x, float(screenHeight) - gl_FragCoord.y);
    ivec2 p = clamp(ivec2((s - pageRect.xy) * vec2(pageSize) / pageRect.zw), ivec2(0), pageSize - 1);
    color = unpackUnorm4x8(px[p.y * pageStride + p.x]).zyxw;
}
)";

// The application's UI layer: as the page, times an opacity (fades).
static const char* kUIFragmentShader = R"(#version 430 core
layout(std430, binding = 1) readonly buffer Page { uint px[]; };
uniform vec4 pageRect;
uniform ivec2 pageSize;
uniform int pageStride;
uniform int screenHeight;
uniform float opacity;
out vec4 color;
void main()
{
    vec2 s = vec2(gl_FragCoord.x, float(screenHeight) - gl_FragCoord.y);
    ivec2 p = clamp(ivec2((s - pageRect.xy) * vec2(pageSize) / pageRect.zw), ivec2(0), pageSize - 1);
    color = unpackUnorm4x8(px[p.y * pageStride + p.x]).zyxw * opacity;
}
)";

// A whole-target triangle (for the YUV -> RGBA pass).
static const char* kFullscreenVertexShader = R"(#version 430 core
void main()
{
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// I420 or NV12 from the frame's GL buffer (a copy of the WebProcess's
// shared-memory slot) to RGBA, one fragment per pixel; texture row 0 is the
// picture's top row (as with Spike 9's decoder frames).
static const char* kConvertFragmentShader = R"(#version 430 core
layout(std430, binding = 0) readonly buffer Frame { uint d[]; };
uniform int format_;   // 1 = I420, 2 = NV12, 4 = NV12 layout with 16-bit samples, 10 bits LSB-aligned (Videodec2)
uniform ivec3 strides;
uniform ivec3 offsets;
uniform int color_;    // bit 0: BT.709 (else BT.601), bit 1: full range
out vec4 o;
float b8(int a) { return float((d[a >> 2] >> ((a & 3) * 8)) & 0xffu) / 255.0; }
float b16(int a) { return float((d[a >> 2] >> ((a & 2) * 8)) & 0xffffu) / 1023.0; }
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    int cy = p.y >> 1, cx = p.x >> 1;
    float y, u, v;
    if (format_ == 4) {
        y = b16(offsets.x + p.y * strides.x + p.x * 2);
        int c = offsets.y + cy * strides.y + cx * 4;
        u = b16(c);
        v = b16(c + 2);
    } else if (format_ == 2) {
        y = b8(offsets.x + p.y * strides.x + p.x);
        int c = offsets.y + cy * strides.y + cx * 2;
        u = b8(c);
        v = b8(c + 1);
    } else {
        y = b8(offsets.x + p.y * strides.x + p.x);
        u = b8(offsets.y + cy * strides.y + cx);
        v = b8(offsets.z + cy * strides.z + cx);
    }
    if ((color_ & 2) == 0) {
        y = (y - 16.0 / 255.0) * (255.0 / 219.0);
        u = (u - 0.5) * (255.0 / 224.0);
        v = (v - 0.5) * (255.0 / 224.0);
    } else {
        u -= 0.5;
        v -= 0.5;
    }
    vec3 rgb = (color_ & 1) != 0
        ? vec3(y + 1.5748 * v, y - 0.1873 * u - 0.4681 * v, y + 1.8556 * u)
        : vec3(y + 1.402 * v, y - 0.344136 * u - 0.714136 * v, y + 1.772 * u);
    o = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

static GLuint compile(GLenum type, const char* source, char* error, unsigned errorSize)
{
    GLuint shader = glCreateShader(type);
    ps5ShaderSourceGL(shader, source);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(shader, errorSize, nullptr, error);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint link(const char* vertex, const char* fragment, char* error, unsigned errorSize)
{
    GLuint vs = compile(GL_VERTEX_SHADER, vertex, error, errorSize);
    GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, fragment, error, errorSize) : 0;
    if (!vs || !fs) {
        if (vs)
            glDeleteShader(vs);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(program, errorSize, nullptr, error);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

bool PS5Renderer::createPrograms(char* error, unsigned errorSize)
{
    m_program = link(kVertexShader, kFragmentShader, error, errorSize);
    if (!m_program)
        return false;
    m_uRect = glGetUniformLocation(m_program, "rect");
    m_uTexture = glGetUniformLocation(m_program, "tex");
    m_uOpaque = glGetUniformLocation(m_program, "opaque_");

    char pageError[512] = { };
    m_pageProgram = link(kVertexShader, kPageFragmentShader, pageError, sizeof(pageError));
    if (m_pageProgram) {
        m_uPageNDC = glGetUniformLocation(m_pageProgram, "rect");
        m_uPageRect = glGetUniformLocation(m_pageProgram, "pageRect");
        m_uPageSize = glGetUniformLocation(m_pageProgram, "pageSize");
        m_uPageStride = glGetUniformLocation(m_pageProgram, "pageStride");
        m_uScreenHeight = glGetUniformLocation(m_pageProgram, "screenHeight");
    } else {
        g_message("[wpe-ps5] page buffer shader failed (%s): page as a texture", pageError);
        m_pageAsTexture = true;
    }

    char convertError[512] = { };
    m_convertProgram = link(kFullscreenVertexShader, kConvertFragmentShader, convertError, sizeof(convertError));
    if (m_convertProgram) {
        m_uFormat = glGetUniformLocation(m_convertProgram, "format_");
        m_uStrides = glGetUniformLocation(m_convertProgram, "strides");
        m_uOffsets = glGetUniformLocation(m_convertProgram, "offsets");
        m_uColor = glGetUniformLocation(m_convertProgram, "color_");
    } else
        g_message("[wpe-ps5] YUV shader failed (%s): no hole-punch video", convertError);

    glGenVertexArrays(1, &m_vao);
    return true;
}

// A round pointer: white with a dark rim and a soft shadow, premultiplied.
static void makePointer(uint8_t* rgba, int size)
{
    const float c = (size - 1) / 2.0f;
    const float r = size * 0.36f;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float d = std::sqrt((x - c) * (x - c) + (y - c) * (y - c));
            float fill = std::fmin(1.0f, std::fmax(0.0f, r - 2.0f - d + 0.5f));
            float rim = std::fmin(1.0f, std::fmax(0.0f, r - d + 0.5f)) - fill;
            float shadow = std::fmin(1.0f, std::fmax(0.0f, (r + 3.0f - d) / 3.0f)) * 0.35f;
            float a = fill + rim + (1.0f - fill - rim) * shadow;
            float v = fill; // white fill, black rim and shadow
            uint8_t* p = rgba + 4 * (y * size + x);
            p[0] = p[1] = p[2] = static_cast<uint8_t>(v * 255.0f);
            p[3] = static_cast<uint8_t>(a * 255.0f);
        }
    }
}

static constexpr int pointerSize = 40;

static double threadCPUSeconds(pthread_t thread)
{
    clockid_t clock;
    struct timespec ts;
    if (pthread_getcpuclockid(thread, &clock) || clock_gettime(clock, &ts))
        return -1;
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

double PS5Renderer::renderThreadCPUSeconds() const
{
    return m_initialized ? threadCPUSeconds(m_thread) : -1;
}

bool PS5Renderer::initialize(char* error, unsigned errorSize)
{
    if (m_initialized)
        return true;
    g_mutex_init(&m_lock);
    g_cond_init(&m_cond);
    m_mainThread = pthread_self();
    {
        gchar* options = nullptr;
        if (g_file_get_contents("/data/ps5webkit/perf-overlay", &options, nullptr, nullptr)) {
            m_perfWanted = true;
            m_perfOptions = options;
            g_free(options);
            ps5PerfMainThreadSampler(true);
        }
    }
#if defined(PS5WK_WC) && PS5WK_WC
    if (!m_compositor)
        m_compositor = ps5wkWCCompositorClient();
#endif
    if (m_compositor)
        m_compositor->attach([] { PS5Renderer::singleton().requestCompositorFrame(); });
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    // 8 MiB: the GPU compositor compiles shaders on this thread (Mesa's GLSL
    // compiler recurses deeply; 1 MiB was enough for our own shaders only).
    pthread_attr_setstacksize(&attributes, 8 << 20);
    if (pthread_create(&m_thread, &attributes, threadEntry, this)) {
        snprintf(error, errorSize, "render thread: pthread_create failed");
        return false;
    }
    g_mutex_lock(&m_lock);
    while (!m_initDone)
        g_cond_wait(&m_cond, &m_lock);
    g_mutex_unlock(&m_lock);
    if (!m_initOK) {
        snprintf(error, errorSize, "%s", m_initError);
        return false;
    }
    m_initialized = true;
    // Hardware decoding: output buffers are GL buffers (persistent, coherent;
    // ps5-opengl backs them with GPU-visible direct memory, which Videodec2
    // writes into), made on the render thread.
    PS5VideoDecoder::setBufferProvider({
        [](size_t bytes, unsigned* name, uint8_t** map) { return PS5Renderer::singleton().allocateVideoBuffer(bytes, name, map); },
        [](unsigned name, uint8_t*, size_t) { PS5Renderer::singleton().freeVideoBuffer(name); },
    });
    PS5VideoServer::start([] { PS5Renderer::singleton().wake(); });
    return true;
}

bool PS5Renderer::allocateVideoBuffer(size_t bytes, unsigned* name, uint8_t** map)
{
    BufferRequest request;
    request.bytes = bytes;
    g_mutex_lock(&m_lock);
    m_bufferRequests.push_back(&request);
    g_cond_broadcast(&m_cond);
    while (!request.done)
        g_cond_wait(&m_cond, &m_lock);
    g_mutex_unlock(&m_lock);
    *name = request.name;
    *map = request.map;
    return request.map;
}

void PS5Renderer::freeVideoBuffer(unsigned name)
{
    g_mutex_lock(&m_lock);
    m_bufferFrees.push_back(name);
    g_cond_broadcast(&m_cond);
    g_mutex_unlock(&m_lock);
}

// Render thread, at the top of a frame (the previous swap waited for the GPU,
// so buffers freed here are no longer read).
void PS5Renderer::serviceBufferRequests()
{
    g_mutex_lock(&m_lock);
    auto requests = std::move(m_bufferRequests);
    m_bufferRequests.clear();
    auto frees = std::move(m_bufferFrees);
    m_bufferFrees.clear();
    g_mutex_unlock(&m_lock);
    for (unsigned name : frees) {
        GLuint buffer = name;
        glDeleteBuffers(1, &buffer);
    }
    for (auto* request : requests) {
        const GLbitfield flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        GLuint buffer = 0;
        glGenBuffers(1, &buffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
        glBufferStorage(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(request->bytes), nullptr, flags);
        auto* map = static_cast<uint8_t*>(glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(request->bytes), flags));
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        if (!map) {
            g_message("[wpe-ps5] decoder output buffer of %.1f MB: map failed (0x%x)", request->bytes / 1048576.0, glGetError());
            glDeleteBuffers(1, &buffer);
            buffer = 0;
        }
        g_mutex_lock(&m_lock);
        request->name = buffer;
        request->map = map;
        request->done = true;
        g_cond_broadcast(&m_cond);
        g_mutex_unlock(&m_lock);
    }
}

void* PS5Renderer::threadEntry(void* data)
{
    static_cast<PS5Renderer*>(data)->threadMain();
    return nullptr;
}

#if PS5WK_GPU_RADV
// GPU=radv: ANGLE's Vulkan backend on RADV, on ANGLE's "simple" display
// (VK_KHR_display, which PS5_Mesa's RADV puts on VideoOut), an ES 3.1
// context (spikes/15-radv-only 15a). epoxy reaches ANGLE's entry points
// (apps/title/dlshim.c).
#ifndef EGL_PLATFORM_ANGLE_ANGLE
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#endif
#ifndef EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE
#define EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE 0x3450
#endif
#ifndef EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE
#define EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE 0x348F
#endif
#ifndef EGL_PLATFORM_VULKAN_DISPLAY_MODE_SIMPLE_ANGLE
#define EGL_PLATFORM_VULKAN_DISPLAY_MODE_SIMPLE_ANGLE 0x34A4
#endif
// ps5-opengl hides the system's splash screen when its window surface is
// made (ps5_egl.c); with ANGLE nothing else does, and the splash stays over
// the title's picture.
extern "C" int sceSystemServiceHideSplashScreen(void) __attribute__((weak));
bool PS5Renderer::initializeANGLE(void*& display, void*& config, void*& surface, void*& context, char* error, unsigned errorSize)
{
    typedef EGLDisplay (*GetPlatformDisplayEXT)(EGLenum, void*, const EGLint*);
    auto getPlatformDisplay = reinterpret_cast<GetPlatformDisplayEXT>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    static const EGLint displayAttributes[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE,
        EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE, EGL_PLATFORM_VULKAN_DISPLAY_MODE_SIMPLE_ANGLE,
        EGL_NONE
    };
    int64_t start = nowUS();
    display = getPlatformDisplay ? getPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, reinterpret_cast<void*>(static_cast<intptr_t>(EGL_DEFAULT_DISPLAY)), displayAttributes) : EGL_NO_DISPLAY;
    EGLint major = 0, minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
        snprintf(error, errorSize, "ANGLE Vulkan display: %s (0x%04x)", getPlatformDisplay ? "eglInitialize failed" : "no eglGetPlatformDisplayEXT", eglGetError());
        return false;
    }
    int64_t initialized = nowUS();
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        snprintf(error, errorSize, "eglBindAPI(EGL_OPENGL_ES_API) failed (0x%04x)", eglGetError());
        return false;
    }
    // ANGLE sizes the swapchain by this (ANGLE patch 0003); VideoOut has
    // 1920x1080 and 3840x2160 buffers and blits other sizes into one.
    static struct { uint16_t width, height; } window = { 1920, 1080 };
    // RADV's VideoOut surface offers B8G8R8A8 only, and ANGLE refuses a window
    // surface whose config's format the surface lacks: try the 8888 window
    // configs until one takes.
    EGLConfig configs[64];
    EGLint count = 0, tried = 0, lastError = 0;
    eglGetConfigs(display, configs, 64, &count);
    for (EGLint i = 0; i < count && surface == EGL_NO_SURFACE; i++) {
        EGLint type = 0, renderable = 0, r = 0, g = 0, b = 0, a = 0, samples = 0;
        eglGetConfigAttrib(display, configs[i], EGL_SURFACE_TYPE, &type);
        eglGetConfigAttrib(display, configs[i], EGL_RENDERABLE_TYPE, &renderable);
        eglGetConfigAttrib(display, configs[i], EGL_RED_SIZE, &r);
        eglGetConfigAttrib(display, configs[i], EGL_GREEN_SIZE, &g);
        eglGetConfigAttrib(display, configs[i], EGL_BLUE_SIZE, &b);
        eglGetConfigAttrib(display, configs[i], EGL_ALPHA_SIZE, &a);
        eglGetConfigAttrib(display, configs[i], EGL_SAMPLES, &samples);
        if (!(type & EGL_WINDOW_BIT) || !(renderable & EGL_OPENGL_ES3_BIT) || r != 8 || g != 8 || b != 8 || a != 8 || samples > 1)
            continue;
        tried++;
        surface = eglCreateWindowSurface(display, configs[i], (EGLNativeWindowType)(uintptr_t)&window, nullptr);
        if (surface != EGL_NO_SURFACE)
            config = configs[i];
        else
            lastError = eglGetError();
    }
    if (surface == EGL_NO_SURFACE) {
        snprintf(error, errorSize, "ANGLE window surface: 0x%04x on all %d 8888 window configs (of %d)", lastError, tried, count);
        return false;
    }
    int splash = sceSystemServiceHideSplashScreen ? sceSystemServiceHideSplashScreen() : -1;
    if (splash)
        g_message("[wpe-ps5] PS5Renderer: hiding the splash screen failed (0x%08x)", static_cast<unsigned>(splash));
    static const EGLint contextAttributes[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context)) {
        snprintf(error, errorSize, "ANGLE ES 3.1 context failed (0x%04x)", eglGetError());
        return false;
    }
    g_message("[wpe-ps5] PS5Renderer: ANGLE on Vulkan, EGL %d.%d, display up in %.1f ms, surface and context in %.1f ms (%d 8888 window configs tried); %s; %s",
        major, minor, (initialized - start) / 1000.0, (nowUS() - initialized) / 1000.0, tried,
        reinterpret_cast<const char*>(glGetString(GL_RENDERER)), reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    return true;
}
#endif

bool PS5Renderer::initializeGL(char* error, unsigned errorSize)
{
    // Mesa's glthread for every context of the title (the compositor's and
    // WebGL's): ps5-opengl reads PS5_GLTHREAD when the display is initialized
    // and each context is made (FEASIBILITY §4.2c). Off unless
    // /data/ps5webkit/gl-thread exists (spikes/11-webgl M4, to measure).
    if (!getenv("PS5_GLTHREAD") && !access("/data/ps5webkit/gl-thread", F_OK)) {
        setenv("PS5_GLTHREAD", "1", 1);
        g_message("[wpe-ps5] glthread on for the title's GL contexts (/data/ps5webkit/gl-thread)");
    }
#if PS5WK_GPU_RADV
    EGLDisplay display;
    EGLConfig config = nullptr;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    if (!initializeANGLE(display, config, surface, context, error, errorSize))
        return false;
#else
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
        snprintf(error, errorSize, "eglInitialize failed (0x%04x)", eglGetError());
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        snprintf(error, errorSize, "eglBindAPI(EGL_OPENGL_API) failed (0x%04x)", eglGetError());
        return false;
    }
    static const EGLint configAttributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE
    };
    static const EGLint contextAttributes[] = {
        EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 6,
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE
    };
    EGLConfig config;
    EGLint count = 0;
    if (!eglChooseConfig(display, configAttributes, &config, 1, &count) || count != 1) {
        snprintf(error, errorSize, "eglChooseConfig failed (0x%04x)", eglGetError());
        return false;
    }
    EGLSurface surface = eglCreateWindowSurface(display, config, static_cast<EGLNativeWindowType>(0), nullptr);
    EGLContext context = surface != EGL_NO_SURFACE ? eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes) : EGL_NO_CONTEXT;
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context)) {
        snprintf(error, errorSize, "EGL window surface/context failed (0x%04x)", eglGetError());
        return false;
    }
#endif
    eglQuerySurface(display, surface, EGL_WIDTH, &m_width);
    eglQuerySurface(display, surface, EGL_HEIGHT, &m_height);
    if (m_width <= 0 || m_height <= 0) {
        m_width = 1920;
        m_height = 1080;
    }
    eglSwapInterval(display, 1);

    struct stat st;
    if (!stat("/data/ps5webkit/page-texture", &st))
        m_pageAsTexture = true;
    if (!createPrograms(error, errorSize))
        return false;

    glGenTextures(1, &m_pageTexture);
    glBindTexture(GL_TEXTURE_2D, m_pageTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    uint8_t pointer[pointerSize * pointerSize * 4];
    makePointer(pointer, pointerSize);
    glGenTextures(1, &m_pointerTexture);
    glBindTexture(GL_TEXTURE_2D, m_pointerTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pointerSize, pointerSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, pointer);

    m_eglDisplay = display;
    m_eglSurface = surface;
    m_eglContext = context;
    m_eglConfig = config;

    // First frame: black, so the TV shows the title is alive.
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(display, surface);
    g_message("[wpe-ps5] PS5Renderer: EGL %s, GL %s, %dx%d, render thread; page %s", eglQueryString(display, EGL_VERSION),
        reinterpret_cast<const char*>(glGetString(GL_VERSION)), m_width, m_height,
        m_pageAsTexture ? "as a texture (glTexSubImage2D)" : "in a persistent GL buffer");
    ps5GLBenchMaybeRun(m_width, m_height); // spike S1a, /data/ps5webkit/glbench only

    return true;
}

// Render thread: the compositor's set-up, once, when it asks (after WebKit is up).
void PS5Renderer::setUpCompositorIfWanted()
{
    if (!m_compositor || m_compositorSetUpDone || !m_compositor->wantsSetUp())
        return;
    m_compositorSetUpDone = true;
    {
        PS5CompositorContext compositorContext { m_eglDisplay, m_eglConfig, m_eglContext, m_eglSurface, m_width, m_height,
            [] { PS5Renderer::singleton().requestCompositorFrame(); }, compositorVideoTexture, copyVideoFrame, videoFrameSize };
        int64_t compositorStart = nowUS();
        m_compositorReady = m_compositor->setUp(compositorContext);
        // The compositor's GL state must not leak into ours.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindVertexArray(m_vao);
        glUseProgram(0);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glActiveTexture(GL_TEXTURE0);
        glViewport(0, 0, m_width, m_height);
        while (glGetError() != GL_NO_ERROR) { }
        g_message("[wpe-ps5] PS5Renderer: GPU compositor %s (%.1f ms)", m_compositorReady ? "ready" : "FAILED: CPU page only",
            (nowUS() - compositorStart) / 1000.0);
    }
}

void PS5Renderer::submitPage(const uint8_t* data, int width, int height, unsigned stride, const Rect* damage, unsigned damageCount,
    void (*done)(void*), void* userData)
{
    if (!m_initialized || !data || width <= 0 || height <= 0) {
        if (done)
            done(userData);
        return;
    }
    Rect* copy = damageCount ? static_cast<Rect*>(g_memdup2(damage, sizeof(Rect) * damageCount)) : nullptr;
    g_mutex_lock(&m_lock);
    // WebKit waits for each frame before it sends the next; if one were
    // still queued, wait for the render thread to take it.
    while (m_pageJob.pending)
        g_cond_wait(&m_cond, &m_lock);
    m_pageJob = { true, data, width, height, stride, copy, damageCount, done, userData };
    g_cond_broadcast(&m_cond);
    g_mutex_unlock(&m_lock);
}

void PS5Renderer::present(bool pointerVisible, double pointerX, double pointerY)
{
    if (!m_initialized)
        return;
    g_mutex_lock(&m_lock);
    m_pointerVisible = pointerVisible;
    m_pointerX = pointerX;
    m_pointerY = pointerY;
    m_frameRequested = true;
    g_cond_broadcast(&m_cond);
    g_mutex_unlock(&m_lock);
}

void PS5Renderer::setCompositor(PS5CompositorClient* client)
{
    if (!m_initialized)
        m_compositor = client;
}

void PS5Renderer::setPerfOverlay(bool on)
{
    ps5PerfMainThreadSampler(on);
    g_mutex_lock(&m_lock);
    m_perfWanted = on;
    m_frameRequested = true;
    g_cond_broadcast(&m_cond);
    g_mutex_unlock(&m_lock);
}

bool PS5Renderer::perfOverlay()
{
    g_mutex_lock(&m_lock);
    bool on = m_perfWanted;
    g_mutex_unlock(&m_lock);
    return on;
}

void PS5Renderer::setKeyboardLift(int pixels)
{
    if (m_keyboardLift.exchange(std::max(pixels, 0)) != std::max(pixels, 0))
        requestCompositorFrame();
}

void PS5Renderer::setPageTop(int pixels)
{
    pixels = std::clamp(pixels, 0, m_height / 2);
    if (m_pageTop.exchange(pixels) != pixels)
        requestCompositorFrame();
}

void PS5Renderer::requestCompositorFrame()
{
    g_mutex_lock(&m_lock);
    m_compositorRequested = true;
    g_cond_broadcast(&m_cond);
    g_mutex_unlock(&m_lock);
}

void PS5Renderer::wake()
{
    g_mutex_lock(&m_lock);
    g_cond_broadcast(&m_cond);
    g_mutex_unlock(&m_lock);
}

void PS5Renderer::requestCapture(const char* path)
{
    if (m_capturesBlocked.load()) {
        g_message("[wpe-ps5] capture refused (private browsing)");
        return;
    }
    g_mutex_lock(&m_lock);
    g_free(m_capturePath);
    m_capturePath = g_strdup(path);
    m_frameRequested = true;
    g_cond_broadcast(&m_cond);
    g_mutex_unlock(&m_lock);
}

void PS5Renderer::setCapturesBlocked(bool blocked)
{
    g_mutex_lock(&m_lock);
    m_capturesBlocked.store(blocked);
    if (blocked)
        g_clear_pointer(&m_capturePath, g_free);
    g_mutex_unlock(&m_lock);
}

// Timing, logged every 5 s while more than 25 frames went out:
// "[present] N frames (x/s), render a/b ms (avg/max, CPU time on the render
// thread before the swap), swap c/d ms, vsync interval e ms; N page uploads,
// f/g ms, MB per upload; video: N frames converted, h ms".
static struct {
    int64_t windowStart;
    unsigned frames, uploads, videoFrames, remoteFrames, intervals;
    double renderMS, renderMaxMS, swapMS, swapMaxMS, uploadMS, uploadMaxMS, uploadBytes, videoMS, videoMaxMS, intervalMS;
    // Where "render" goes: compositor updates applied, video consumed and
    // converted, scene painted, the frame drawn (its first draw into the
    // window waits for the previous frame's flip).
    double applyMS, consumeMS, sceneMS, drawMS, drawMaxMS;
} stats;

static void maybeLogStats(int64_t now)
{
    if (!stats.windowStart)
        stats.windowStart = now;
    double seconds = (now - stats.windowStart) / 1e6;
    if (seconds < 5)
        return;
    if (stats.frames > 25) {
        g_message("[wpe-ps5] [present] %u frames (%.1f/s), render %.2f ms avg %.1f max, swap %.1f ms avg %.1f max, vsync interval %.2f ms; "
                  "%u page uploads, %.1f ms avg %.1f max (%.2f MB each); %u video frames (%u hardware-decoded), copy+convert %.2f ms avg %.1f max",
            stats.frames, stats.frames / seconds, stats.renderMS / stats.frames, stats.renderMaxMS, stats.swapMS / stats.frames, stats.swapMaxMS,
            stats.intervals ? stats.intervalMS / stats.intervals : 0.0, stats.uploads, stats.uploads ? stats.uploadMS / stats.uploads : 0.0,
            stats.uploadMaxMS, stats.uploads ? stats.uploadBytes / stats.uploads / 1048576.0 : 0.0, stats.videoFrames, stats.remoteFrames,
            stats.videoFrames ? stats.videoMS / stats.videoFrames : 0.0, stats.videoMaxMS);
        g_message("[wpe-ps5] [present] render: apply %.2f ms avg, video %.2f, scene %.2f, draw %.2f avg %.1f max (incl. the wait for the previous flip)",
            stats.applyMS / stats.frames, stats.consumeMS / stats.frames, stats.sceneMS / stats.frames, stats.drawMS / stats.frames, stats.drawMaxMS);
    }
    stats = { };
    stats.windowStart = now;
}

void PS5Renderer::threadMain()
{
    char error[512] = { };
    bool ok = initializeGL(error, sizeof(error));
    g_mutex_lock(&m_lock);
    m_initOK = ok;
    snprintf(m_initError, sizeof(m_initError), "%s", error);
    m_initDone = true;
    g_cond_broadcast(&m_cond);
    g_mutex_unlock(&m_lock);
    if (!ok)
        return;

    for (;;) {
        // View in VR: the headset paces the thread (PS5RendererVR.cpp).
        vrHandleRequests();
        setUpCompositorIfWanted();
        if (m_vr) {
            vrFrame();
            // Composited pages keep going at the headset's rate (their replies
            // pace the WebProcess); the scene is painted again on the TV after.
            if (m_compositorReady) {
                g_mutex_lock(&m_lock);
                m_compositorRequested = false;
                g_mutex_unlock(&m_lock);
                GPUSubmitLock gpuSubmitLock;
                m_compositor->applyPending();
                gpuSubmitLock.release();
                m_compositor->didPresent();
            }
            continue;
        }
        char* capturePath = nullptr;
        g_mutex_lock(&m_lock);
        for (;;) {
            if (m_pageJob.pending || m_frameRequested || m_compositorRequested || m_capturePath || !m_bufferRequests.empty() || !m_bufferFrees.empty())
                break;
            if (m_vrRequest.start || m_vrRequest.stop || m_messageChanged || m_thumbnailRequest.pending || m_perfWanted != m_perfOn)
                break;
            int64_t now = nowUS();
            if (m_messageUntilUS && now >= m_messageUntilUS)
                break; // take the message down
            if (m_videoActive || m_uiFadeStartUS) {
                // Video: a frame every vsync. The swap returns at once and
                // the next frame's first draw waits for that swap's flip, so
                // the frame can start right after the swap (it waited 10 ms,
                // from when the swap itself waited: with a scene that takes
                // more than ~6 ms to paint, frames missed their vsync, and an
                // update that came in after the frame had started waited a
                // frame more). 2 ms: should a draw ever stop waiting, don't spin.
                // /data/ps5webkit/wc-wait-old: 10 ms as before (A/B).
                static const int64_t afterSwapUS = [] {
                    struct stat st;
                    return stat("/data/ps5webkit/wc-wait-old", &st) ? 2000 : 10000;
                }();
                if (!m_lastSwapEnd || now - m_lastSwapEnd >= afterSwapUS)
                    break;
                g_cond_wait_until(&m_cond, &m_lock, m_lastSwapEnd + afterSwapUS);
                continue;
            }
            // Idle: sleep until a frame or geometry change is due.
            int64_t deadline = PS5VideoServer::nextDeadline();
            if (m_messageUntilUS)
                deadline = std::min(deadline, m_messageUntilUS + 2000);
            if (m_perfOn && m_perf) // the overlay's next text update
                deadline = std::min(deadline, m_perf->nextUpdateUS() + 2000);
            if (m_thumbnails.active) // a paused video: answer at the deadline with what was taken
                deadline = std::min(deadline, std::min(m_thumbnails.deadlineUS, m_thumbnails.taken ? m_thumbnails.stallUS : INT64_MAX) + 2000);
            if (deadline <= now + 2000)
                break;
            if (deadline == INT64_MAX)
                g_cond_wait(&m_cond, &m_lock);
            else
                g_cond_wait_until(&m_cond, &m_lock, deadline - 2000);
        }
        m_job = m_pageJob;
        m_pageJob.pending = false;
        if (m_job.pending)
            g_cond_broadcast(&m_cond); // a waiting submitPage
        m_frameRequested = false;
        m_compositorRequested = false;
        m_drawPointer = m_pointerVisible;
        m_drawPointerX = m_pointerX;
        m_drawPointerY = m_pointerY;
        capturePath = m_capturePath;
        m_capturePath = nullptr;
        bool perfWanted = m_perfWanted;
        if (m_uiChanged) {
            const bool shown = m_uiRect[2] > 0 && !m_uiFadingOut, removed = m_uiPending.empty();
            if (removed && shown && uiFadeWanted()) {
                // The last image stays and fades out (drawUILayer).
                m_uiFadingOut = true;
                m_uiFadeStartUS = nowUS();
            } else {
                m_uiStaging.swap(m_uiPending);
                m_uiPending.clear();
                memcpy(m_uiRect, m_uiPendingRect, sizeof m_uiRect);
                m_uiStagingNew = true;
                m_uiFadingOut = false;
                // A layer where there was none fades in; a change to one shown doesn't.
                m_uiFadeStartUS = !removed && !shown && uiFadeWanted() ? nowUS() : 0;
            }
            m_uiChanged = false;
        }
        g_mutex_unlock(&m_lock);

        if (perfWanted != m_perfOn) {
            if (perfWanted && !m_perf)
                m_perf = new PS5PerfOverlay(pthread_self(), m_mainThread, m_perfOptions.c_str());
            if (m_perf)
                m_perf->setActive(perfWanted);
            m_perfOn = perfWanted;
#if PS5WK_GPU_RADV
            if (!perfWanted) {
                for (auto& pending : m_gpuTimerPending)
                    pending = false; // not read: a query is ended, so it can begin again
            }
#endif
        }

        // The wait for the last frame's flip, before the lock and not inside the first draw under
        // it: it is most of a frame (13.7 of 16.7 ms), and Dawn waited as long for every
        // vkQueueSubmit of a page (a WebGPU emulator: 47% of its GPU thread's time, measured).
#if !PS5WK_GPU_RADV // ANGLE waits in its swapchain acquire instead
        if (ps5wk_gpu_submit_lock && ps5_agc_gate2_wait_present)
            ps5_agc_gate2_wait_present();
#else
        // GPU=radv: ANGLE's wait for the next swapchain image, now and outside its locks
        // (eglPrepareSwapBuffersANGLE acquires in an unlocked tail call). Left to the frame's
        // first draw into the window, it was made holding the share group's lock (on in this
        // build since the WebGL video crash fix) for most of a frame, and every other thread's GL
        // call waited for it: Dawn's video reads 4-14 ms in eglMakeCurrent, measured (HANDOVER-radv.md
        // Phase 4). The same place as ps5-opengl's wait above.
        {
            using PrepareSwap = EGLBoolean (*)(EGLDisplay, EGLSurface);
            static PrepareSwap prepareSwap = reinterpret_cast<PrepareSwap>(eglGetProcAddress("eglPrepareSwapBuffersANGLE"));
            if (prepareSwap && m_eglSurface)
                prepareSwap(m_eglDisplay, m_eglSurface);
        }
#endif
        // Until the swap: its wait for the vsync is not held against Dawn.
        GPUSubmitLock gpuSubmitLock;
#if PS5WK_GPU_RADV
        const bool gpuTimer = m_perfOn && m_perf && beginGPUTimer();
#endif
        setUpCompositorIfWanted();
        serviceBufferRequests();
        int64_t start = nowUS();
        updateMessage(start);
        applyUILayer();
        bool commitGeometry = m_job.pending;
        double uploadMS = 0;
        if (m_job.pending) {
            int64_t copyStart = nowUS();
            copyPage();
            uploadMS = std::max(0.001, (nowUS() - copyStart) / 1000.0);
        }
        if (m_compositorReady)
            m_compositor->applyPending();
        int64_t applied = nowUS();

        // The frame drawn now goes on screen at the next vsync after the
        // last swap returned. Keep a vsync grid (smoothed swap-return times)
        // so frames keep a steady phase against it.
        int64_t displayUS;
        if (m_vsyncGrid && start - m_vsyncGrid < 3 * static_cast<int64_t>(m_periodUS))
            displayUS = m_vsyncGrid + static_cast<int64_t>(m_periodUS);
        else
            displayUS = start + static_cast<int64_t>(m_periodUS);
        m_videoDrawCount = 0;
        m_videoSourceCount = 0;
        if (m_convertProgram) {
            m_videoActive = PS5VideoServer::consume(displayUS, static_cast<int64_t>(m_periodUS / 2), commitGeometry,
                [this](PS5VideoStream& stream, const PS5VideoFrame* frame, const uint8_t* data) { handleVideo(stream, frame, data); });
        }
        serviceThumbnails(nowUS());
        int64_t consumed = nowUS();

        m_sceneTexture = 0;
        m_layerVideoKeyCount = 0;
        if (m_compositorReady) {
            m_sceneTexture = m_compositor->paint(displayUS, &m_sceneWidth, &m_sceneHeight);
            if (m_sceneWidth <= 0 || m_sceneHeight <= 0)
                m_sceneTexture = 0;
        }

        int64_t painted = nowUS();
        drawScene();
        int64_t drawn = nowUS();
        if (capturePath && m_capturesBlocked.load())
            g_clear_pointer(&capturePath, g_free); // private browsing began meanwhile
        if (capturePath)
            writeCapture(capturePath);
#if PS5WK_GPU_RADV
        if (gpuTimer)
            endGPUTimer();
#endif
        gpuSubmitLock.release();
        int64_t swapStart = nowUS();
        eglSwapBuffers(m_eglDisplay, m_eglSurface);
        int64_t swapEnd = nowUS();
        int64_t previousSwapEnd = m_lastSwapEnd;
        // The swap waited for the GPU: decoded pictures drawn this frame go back to their decoders.
        PS5VideoServer::presented();
        if (m_compositorReady)
            m_compositor->didPresent();

        // vsync grid: follow swap returns within a quarter period, else reset.
        if (m_lastSwapEnd) {
            double interval = swapEnd - m_lastSwapEnd;
            if (interval > 12000 && interval < 22000) {
                stats.intervals++;
                stats.intervalMS += interval / 1000.0;
                m_periodUS += (interval - m_periodUS) / 64;
                m_periodUS = std::clamp(m_periodUS, 15500.0, 17500.0);
            }
        }
        m_lastSwapEnd = swapEnd;
        if (m_vsyncGrid) {
            int64_t predicted = m_vsyncGrid;
            while (predicted + m_periodUS / 2 < swapEnd)
                predicted += static_cast<int64_t>(m_periodUS);
            int64_t error = swapEnd - predicted;
            m_vsyncGrid = std::abs(error) < m_periodUS / 4 ? predicted + error / 8 : swapEnd;
        } else
            m_vsyncGrid = swapEnd;

        if (m_job.pending) {
            g_free(m_job.damage);
            if (m_job.done)
                m_job.done(m_job.userData);
            m_job = { };
        }

        double renderMS = (swapStart - start) / 1000.0, swapMS = (swapEnd - swapStart) / 1000.0;
        stats.applyMS += (applied - start) / 1000.0;
        stats.consumeMS += (consumed - applied) / 1000.0;
        stats.sceneMS += (painted - consumed) / 1000.0;
        stats.drawMS += (drawn - painted) / 1000.0;
        stats.drawMaxMS = std::max(stats.drawMaxMS, (drawn - painted) / 1000.0);
        // /data/ps5webkit/wc-frame-log: one line per frame (frame pacing checks).
        static const bool frameLog = [] {
            struct stat st;
            return !stat("/data/ps5webkit/wc-frame-log", &st);
        }();
        if (frameLog)
            g_message("[frame] render t=%.1f: started %.2f ms after the last swap, apply %.2f, video %.2f, scene %.2f, draw %.2f, swap %.2f",
                start / 1000.0, previousSwapEnd ? (start - previousSwapEnd) / 1000.0 : -1.0, (applied - start) / 1000.0, (consumed - applied) / 1000.0,
                (painted - consumed) / 1000.0, (drawn - painted) / 1000.0, swapMS);
        if (m_perfOn && m_perf)
            m_perf->noteFrame({ start, applied, consumed, painted, drawn, swapStart, swapEnd, uploadMS, m_sceneTexture != 0 });
        stats.frames++;
        stats.renderMS += renderMS;
        stats.renderMaxMS = std::max(stats.renderMaxMS, renderMS);
        stats.swapMS += swapMS;
        stats.swapMaxMS = std::max(stats.swapMaxMS, swapMS);
        maybeLogStats(swapEnd);
    }
}

#if PS5WK_GPU_RADV
// The overlay's GPU time: EXT_disjoint_timer_query (ANGLE's, from Vulkan timestamps) around the
// render thread's GL work of a frame, from its start to the swap: the compositor's scene, the UI,
// video conversion, the overlay. Not WebGL's or WebGPU's own work, which other contexts submit.
// Read a few frames later, oldest first; nothing while the overlay is off.
bool PS5Renderer::beginGPUTimer()
{
    if (m_gpuTimerSupport < 0) {
        m_gpuTimerSupport = epoxy_has_gl_extension("GL_EXT_disjoint_timer_query");
        if (m_gpuTimerSupport)
            glGenQueriesEXT(kGPUTimers, m_gpuTimers);
        g_message("[wpe-ps5] performance overlay: GPU time %s", m_gpuTimerSupport ? "from EXT_disjoint_timer_query" : "n/a (no EXT_disjoint_timer_query)");
    }
    if (!m_gpuTimerSupport)
        return false;
    collectGPUTimers();
    if (m_gpuTimerPending[m_gpuTimerNext])
        return false; // the GPU is more than kGPUTimers frames behind: this frame isn't timed
    glBeginQueryEXT(GL_TIME_ELAPSED_EXT, m_gpuTimers[m_gpuTimerNext]);
    return true;
}

void PS5Renderer::endGPUTimer()
{
    glEndQueryEXT(GL_TIME_ELAPSED_EXT);
    m_gpuTimerPending[m_gpuTimerNext] = true;
    m_gpuTimerNext = (m_gpuTimerNext + 1) % kGPUTimers;
}

void PS5Renderer::collectGPUTimers()
{
    for (unsigned i = 0; i < kGPUTimers; i++) {
        const unsigned slot = (m_gpuTimerNext + i) % kGPUTimers;
        if (!m_gpuTimerPending[slot])
            continue;
        GLuint available = 0;
        glGetQueryObjectuivEXT(m_gpuTimers[slot], GL_QUERY_RESULT_AVAILABLE_EXT, &available);
        if (!available)
            break; // the later ones aren't either
        GLuint64 nanoseconds = 0;
        glGetQueryObjectui64vEXT(m_gpuTimers[slot], GL_QUERY_RESULT_EXT, &nanoseconds);
        m_gpuTimerPending[slot] = false;
        // A disjoint operation (a GPU clock change, a reset) makes the results meaningless.
        GLint disjoint = 0;
        glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
        if (!disjoint && m_perf)
            m_perf->noteGPUTime(nanoseconds / 1e6);
    }
}
#endif

void PS5Renderer::copyPage()
{
    int64_t start = nowUS();
    const PageJob& job = m_job;
    double bytes = 0;
    const int width = job.width, height = job.height;
    const bool resized = width != m_pageWidth || height != m_pageHeight;

    if (!m_pageAsTexture) {
        size_t needed = static_cast<size_t>(width) * height * 4;
        if (needed > m_pageBufferBytes) {
            if (m_pageBuffer)
                glDeleteBuffers(1, &m_pageBuffer);
            const GLbitfield flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
            glGenBuffers(1, &m_pageBuffer);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_pageBuffer);
            glBufferStorage(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(needed), nullptr, flags);
            m_pageMap = static_cast<uint8_t*>(glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(needed), flags));
            if (!m_pageMap) {
                g_message("[wpe-ps5] page buffer: map failed (GL error 0x%x): page as a texture", glGetError());
                glDeleteBuffers(1, &m_pageBuffer);
                m_pageBuffer = 0;
                m_pageBufferBytes = 0;
                m_pageAsTexture = true;
            } else
                m_pageBufferBytes = needed;
        }
    }

    if (!m_pageAsTexture) {
        // The previous frame's GPU work is done (the swap waits for it), so
        // the rows can be overwritten in place.
        const size_t dstStride = static_cast<size_t>(width) * 4;
        auto copyRect = [&](int x0, int y0, int x1, int y1) {
            const size_t rowBytes = static_cast<size_t>(x1 - x0) * 4;
            for (int y = y0; y < y1; y++)
                memcpy(m_pageMap + y * dstStride + x0 * 4, job.data + static_cast<size_t>(y) * job.stride + x0 * 4, rowBytes);
            bytes += static_cast<double>(rowBytes) * (y1 - y0);
        };
        if (resized || !job.damageCount) {
            if (job.stride == dstStride) {
                memcpy(m_pageMap, job.data, dstStride * height);
                bytes = static_cast<double>(dstStride) * height;
            } else
                copyRect(0, 0, width, height);
        } else {
            for (unsigned i = 0; i < job.damageCount; i++) {
                int x0 = std::max(0, job.damage[i].x), y0 = std::max(0, job.damage[i].y);
                int x1 = std::min(width, job.damage[i].x + job.damage[i].width);
                int y1 = std::min(height, job.damage[i].y + job.damage[i].height);
                if (x1 > x0 && y1 > y0)
                    copyRect(x0, y0, x1, y1);
            }
        }
    } else {
        glBindTexture(GL_TEXTURE_2D, m_pageTexture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(job.stride / 4));
        if (resized) {
            glTexImage2D(GL_TEXTURE_2D, 0, kPS5BGRAInternalFormat, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, job.data);
            bytes = 4.0 * width * height;
        } else if (!job.damageCount) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, job.data);
            bytes = 4.0 * width * height;
        } else {
            for (unsigned i = 0; i < job.damageCount; i++) {
                int x0 = std::max(0, job.damage[i].x), y0 = std::max(0, job.damage[i].y);
                int x1 = std::min(width, job.damage[i].x + job.damage[i].width);
                int y1 = std::min(height, job.damage[i].y + job.damage[i].height);
                if (x1 <= x0 || y1 <= y0)
                    continue;
                glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, x1 - x0, y1 - y0, GL_BGRA, GL_UNSIGNED_BYTE,
                    job.data + static_cast<size_t>(y0) * job.stride + static_cast<size_t>(x0) * 4);
                bytes += 4.0 * (x1 - x0) * (y1 - y0);
            }
        }
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    m_pageWidth = width;
    m_pageHeight = height;

    double ms = (nowUS() - start) / 1000.0;
    stats.uploads++;
    stats.uploadMS += ms;
    stats.uploadMaxMS = std::max(stats.uploadMaxMS, ms);
    stats.uploadBytes += bytes;
}

// The renderer's GL objects for one video stream: the frame's YUV bytes in
// a persistent GL buffer (read by the conversion shader as an SSBO) and the
// RGBA texture it is converted into. Step 2b: Videodec2 decodes straight
// into such buffers (Spike 9), and the memcpy below goes away.
struct PS5VideoGL {
    GLuint buffer { 0 };
    uint8_t* map { nullptr };
    size_t bytes { 0 };
    GLuint texture { 0 };
    GLuint fbo { 0 };
    int width { 0 }, height { 0 };
    bool valid { false };
    int64_t lastFrameUS { 0 };
    uint32_t frames { 0 }; // converted so far (video layers repaint when it moves)
};

// WebGL's copies of video frames (M3, spikes/11-webgl): the newest
// converted frame of each stream, by key, for other threads. `written`: a
// fence after the conversion (a copy waits for it on the GPU); `read`: a
// fence after the last copy (the next conversion into the texture waits for
// it). Guarded by s_sharedVideoLock; the GL calls under it only queue work.
namespace {
struct SharedVideoFrame {
    uint64_t key { 0 }; // 0: free
    GLuint texture { 0 };
    int width { 0 }, height { 0 };
    GLsync written { nullptr };
    GLsync read { nullptr };
};
GMutex s_sharedVideoLock;
SharedVideoFrame s_sharedVideo[16];

SharedVideoFrame* sharedVideoFrame(uint64_t key, bool create)
{
    SharedVideoFrame* free = nullptr;
    for (auto& entry : s_sharedVideo) {
        if (entry.key == key)
            return &entry;
        if (!entry.key && !free)
            free = &entry;
    }
    if (create && free)
        free->key = key;
    return create ? free : nullptr;
}

void clearSharedVideoFrame(SharedVideoFrame& entry)
{
    if (entry.written)
        glDeleteSync(entry.written);
    if (entry.read)
        glDeleteSync(entry.read);
    entry = { };
}
} // namespace

// Render thread: before the stream's texture is written again, the GPU waits
// for WebGL's last copy out of it.
static void waitForVideoFrameCopies(uint64_t key)
{
    if (!key)
        return;
    g_mutex_lock(&s_sharedVideoLock);
    if (auto* entry = sharedVideoFrame(key, false); entry && entry->read) {
        glWaitSync(entry->read, 0, GL_TIMEOUT_IGNORED);
        glDeleteSync(entry->read);
        entry->read = nullptr;
    }
    g_mutex_unlock(&s_sharedVideoLock);
}

// Render thread: the stream's texture holds a new frame (or the stream went: texture 0).
static void publishVideoFrame(uint64_t key, GLuint texture, int width, int height)
{
    if (!key)
        return;
    g_mutex_lock(&s_sharedVideoLock);
    if (!texture) {
        if (auto* entry = sharedVideoFrame(key, false))
            clearSharedVideoFrame(*entry);
    } else if (auto* entry = sharedVideoFrame(key, true)) {
        entry->texture = texture;
        entry->width = width;
        entry->height = height;
        if (entry->written)
            glDeleteSync(entry->written);
        entry->written = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush(); // other contexts wait on it
    }
    g_mutex_unlock(&s_sharedVideoLock);
}

bool PS5Renderer::videoFrameSize(uint64_t key, int* width, int* height)
{
    *width = *height = 0;
    if (!key)
        return false;
    g_mutex_lock(&s_sharedVideoLock);
    if (auto* entry = sharedVideoFrame(key, false); entry && entry->written) {
        *width = entry->width;
        *height = entry->height;
    }
    g_mutex_unlock(&s_sharedVideoLock);
    return *width > 0 && *height > 0;
}

#if PS5WK_GPU_RADV
// copyVideoFrame in a WebGL context (WebGL 1 or 2 on ANGLE): the frame copied
// with glCopySubTextureCHROMIUM (CHROMIUM_copy_texture, enabled in the
// context the first time; ANGLE's Vulkan backend has it), which stores the
// frame's bytes as they are in any format, as texImage2D does: sRGB
// textures too, which a blit encoded (ES has no switch for that, and ANGLE
// no EXT_sRGB_write_control here), and it can flip, which WebGL 1's
// ANGLE_framebuffer_blit can't. WebGL 1 has no fences either. None here: the
// frame's writer flushed it under s_sharedVideoLock (publishVideoFrame), the
// copy is flushed under it too, and the next write waits for that lock
// (waitForVideoFrameCopies). ANGLE's contexts here share one device and one
// queue, so the GPU runs the write, the copy and the next write in that
// order. Entry points from ANGLE directly: epoxy has no CHROMIUM_copy_texture.
// What copyVideoFrame returns then: copied, and no fence to wait on or delete
// (WebKit's PS5WebGLVideoCopy.cpp knows the value).
static void* const kVideoFrameCopiedInContext = reinterpret_cast<void*>(1);

static bool copyVideoFrameWebGL(GLuint source, GLuint destination, int width, int height, bool flipY)
{
    using RequestExtension = void (*)(const char*);
    using CopySubTexture = void (*)(GLuint, GLint, GLenum, GLuint, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLboolean, GLboolean, GLboolean);
    static RequestExtension requestExtension = reinterpret_cast<RequestExtension>(eglGetProcAddress("glRequestExtensionANGLE"));
    static CopySubTexture copySubTexture = reinterpret_cast<CopySubTexture>(eglGetProcAddress("glCopySubTextureCHROMIUM"));
    static bool loggedFailure;
    if (!copySubTexture || !requestExtension)
        return false;
    if (!epoxy_has_gl_extension("GL_CHROMIUM_copy_texture")) {
        constexpr GLenum requestableExtensionsANGLE = 0x93A8; // GL_REQUESTABLE_EXTENSIONS_ANGLE
        const char* requestable = reinterpret_cast<const char*>(glGetString(requestableExtensionsANGLE));
        if (!requestable || !strstr(requestable, "GL_CHROMIUM_copy_texture")) {
            if (!loggedFailure) {
                loggedFailure = true;
                g_message("[wpe-ps5] video frame copy: no CHROMIUM_copy_texture in a WebGL context: no video in WebGL textures");
            }
            return false;
        }
        requestExtension("GL_CHROMIUM_copy_texture");
    }
    // The stream's row 0 is the picture's top, as texImage2D's without UNPACK_FLIP_Y.
    copySubTexture(source, 0, GL_TEXTURE_2D, destination, 0, 0, 0, 0, 0, width, height, flipY, GL_FALSE, GL_FALSE);
    glFlush();
    return true;
}
#endif

void* PS5Renderer::copyVideoFrame(uint64_t key, unsigned texture, bool flipY)
{
    if (!key || !texture)
        return nullptr;
    GLsync done = nullptr;
    g_mutex_lock(&s_sharedVideoLock);
#if PS5WK_GPU_RADV
    // GPU=radv: this runs in the caller's context. WebGL's (WebKit's
    // PS5WebGLVideoCopy.cpp; ANGLE_webgl_compatibility) copy with
    // CHROMIUM_copy_texture (copyVideoFrameWebGL). A WebGL 1 context is
    // OpenGL ES 2.0, without fences or glBlitFramebuffer: epoxy aborted at
    // glWaitSync ("No provider of glWaitSync found", a WebGL 1 page's video
    // texture, 2026-11-20). WebGPU's reader context blits below, into its
    // RGBA8 and BGRA8 textures.
    if (epoxy_has_gl_extension("GL_ANGLE_webgl_compatibility")) {
        void* copied = nullptr;
        if (auto* entry = sharedVideoFrame(key, false); entry && entry->written && copyVideoFrameWebGL(entry->texture, texture, entry->width, entry->height, flipY))
            copied = kVideoFrameCopiedInContext;
        g_mutex_unlock(&s_sharedVideoLock);
        return copied;
    }
#endif
    if (auto* entry = sharedVideoFrame(key, false); entry && entry->written) {
        glWaitSync(entry->written, 0, GL_TIMEOUT_IGNORED);
        GLuint fbos[2] = { };
        glGenFramebuffers(2, fbos);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbos[0]);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, entry->texture, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbos[1]);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        const int w = entry->width, h = entry->height;
        // An sRGB texture gets the frame's bytes as they are, as texImage2D
        // stores them: no linear-to-sRGB encoding on the write (on by
        // default in ES contexts; EXT_sRGB_write_control turns it off).
#if PS5WK_GPU_RADV
        // GPU=radv: WebGPU's textures here, never sRGB (WebGL's copy above).
#else
        static bool checkedSRGB;
        if (!checkedSRGB) {
            while (glGetError() != GL_NO_ERROR) { }
        }
        glDisable(GL_FRAMEBUFFER_SRGB);
        if (!checkedSRGB) {
            checkedSRGB = true;
            if (GLenum error = glGetError())
                g_message("[wpe-ps5] video frame copy: sRGB writes can't be turned off (GL error 0x%x): sRGB WebGL textures get encoded twice", error);
        }
#endif
        // The stream's row 0 is the picture's top, as texImage2D's without
        // UNPACK_FLIP_Y.
        glBlitFramebuffer(0, 0, w, h, 0, flipY ? h : 0, w, flipY ? 0 : h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
#if !PS5WK_GPU_RADV
        glEnable(GL_FRAMEBUFFER_SRGB);
#endif
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDeleteFramebuffers(2, fbos);
        if (entry->read)
            glDeleteSync(entry->read);
        entry->read = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        done = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
    }
    g_mutex_unlock(&s_sharedVideoLock);
    return done;
}

void PS5Renderer::handleVideo(PS5VideoStream& stream, const PS5VideoFrame* frame, const uint8_t* data)
{
    auto* gl = static_cast<PS5VideoGL*>(stream.consumerData);
    if (stream.closed) {
        publishVideoFrame(stream.key, 0, 0, 0);
        if (gl) {
            if (gl->buffer)
                glDeleteBuffers(1, &gl->buffer);
            if (gl->fbo)
                glDeleteFramebuffers(1, &gl->fbo);
            if (gl->texture)
                glDeleteTextures(1, &gl->texture);
            delete gl;
            stream.consumerData = nullptr;
        }
        return;
    }
    if (!gl) {
        gl = new PS5VideoGL;
        stream.consumerData = gl;
    }

    if (frame && frame->remote && !frame->picture.glBuffer)
        frame = nullptr; // not GPU memory (fake decoder): nothing to draw
    if (frame && frame->width && frame->height && frame->width <= 8192 && frame->height <= 8192) {
        int64_t start = nowUS();
        waitForVideoFrameCopies(stream.key);
        const int w = static_cast<int>(frame->width), h = static_cast<int>(frame->height), ch = (h + 1) / 2;
        const bool remote = frame->remote;
        size_t needed = remote ? 0 : frame->format == PS5WK_VFMT_NV12
            ? frame->offset[1] + static_cast<size_t>(frame->stride[1]) * ch
            : frame->offset[2] + static_cast<size_t>(frame->stride[2]) * ch;
        needed = std::min<size_t>(needed, stream.slotBytes);
        bool ok = true;
        if (!remote && needed > gl->bytes) {
            if (gl->buffer)
                glDeleteBuffers(1, &gl->buffer);
            size_t bytes = (needed + 0xffff) & ~static_cast<size_t>(0xffff);
            const GLbitfield flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
            glGenBuffers(1, &gl->buffer);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, gl->buffer);
            glBufferStorage(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr, flags);
            gl->map = static_cast<uint8_t*>(glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(bytes), flags));
            gl->bytes = gl->map ? bytes : 0;
            if (!gl->map) {
                g_message("[wpe-ps5] video stream %d: GL buffer of %zu bytes: map failed (0x%x)", stream.id, bytes, glGetError());
                glDeleteBuffers(1, &gl->buffer);
                gl->buffer = 0;
                ok = false;
            }
        }
        if (ok && (gl->width != w || gl->height != h)) {
            if (!gl->texture)
                glGenTextures(1, &gl->texture);
            glBindTexture(GL_TEXTURE_2D, gl->texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            if (!gl->fbo)
                glGenFramebuffers(1, &gl->fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, gl->fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl->texture, 0);
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                g_message("[wpe-ps5] video stream %d: framebuffer for %dx%d incomplete (0x%x)", stream.id, w, h, status);
                ok = false;
            } else {
                gl->width = w;
                gl->height = h;
                g_message("[wpe-ps5] video stream %d: %dx%d %s", stream.id, w, h,
                    remote ? (frame->picture.bitDepth > 8 ? "hardware-decoded, 10-bit" : "hardware-decoded NV12")
                           : frame->format == PS5WK_VFMT_NV12 ? "NV12" : "I420");
            }
        }
        if (ok) {
            if (!remote)
                memcpy(gl->map, data, needed);
            glBindFramebuffer(GL_FRAMEBUFFER, gl->fbo);
            glViewport(0, 0, w, h);
            glDisable(GL_BLEND);
            glDisable(GL_SCISSOR_TEST);
            glUseProgram(m_convertProgram);
            glBindVertexArray(m_vao);
            if (remote) {
                // Zero copy: straight from the decoder's output buffer (Spike 9).
                const PS5VideoPicture& pic = frame->picture;
                GLint pitch = static_cast<GLint>(pic.pitch), y = static_cast<GLint>(pic.offset);
                GLint uv = y + pitch * static_cast<GLint>(pic.codedHeight);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, pic.glBuffer);
                glUniform1i(m_uFormat, pic.bitDepth > 8 ? PS5WK_VFMT_P010LSB : PS5WK_VFMT_NV12);
                glUniform3i(m_uStrides, pitch, pitch, 0);
                glUniform3i(m_uOffsets, y, uv, 0);
                stats.remoteFrames++;
            } else {
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, gl->buffer);
                glUniform1i(m_uFormat, static_cast<GLint>(frame->format));
                glUniform3i(m_uStrides, static_cast<GLint>(frame->stride[0]), static_cast<GLint>(frame->stride[1]), static_cast<GLint>(frame->stride[2]));
                glUniform3i(m_uOffsets, static_cast<GLint>(frame->offset[0]), static_cast<GLint>(frame->offset[1]), static_cast<GLint>(frame->offset[2]));
            }
            glUniform1i(m_uColor, static_cast<GLint>(frame->colorimetry));
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            gl->valid = true;
            gl->lastFrameUS = start;
            gl->frames++;
            publishVideoFrame(stream.key, gl->texture, w, h);
        }
        double ms = (nowUS() - start) / 1000.0;
        stats.videoFrames++;
        m_videoFramesConverted++;
        stats.videoMS += ms;
        stats.videoMaxMS = std::max(stats.videoMaxMS, ms);
    }

    if (gl->valid && stream.geometry.visible && stream.geometry.width > 0 && stream.geometry.height > 0 && m_videoDrawCount < kMaxVideoStreams)
        m_videoDraws[m_videoDrawCount++] = { gl->texture, stream.geometry.x, stream.geometry.y, stream.geometry.width, stream.geometry.height, stream.key };
    if (gl->valid && m_videoSourceCount < kMaxVideoStreams)
        m_videoSources[m_videoSourceCount++] = { stream.id, gl->texture, gl->width, gl->height, gl->lastFrameUS, stream.key, gl->frames };
}

// Render thread, from the compositor's paint (after this frame's consume
// pass): a video layer's contents.
unsigned PS5Renderer::compositorVideoTexture(uint64_t key, int* width, int* height, uint32_t* frames)
{
    PS5Renderer& self = singleton();
    *width = *height = 0;
    *frames = 0;
    if (!key)
        return 0;
    for (unsigned i = 0; i < self.m_videoSourceCount; i++) {
        const auto& source = self.m_videoSources[i];
        if (source.key != key)
            continue;
        bool listed = false;
        for (unsigned k = 0; k < self.m_layerVideoKeyCount; k++)
            listed |= self.m_layerVideoKeys[k] == key;
        if (!listed && self.m_layerVideoKeyCount < kMaxVideoStreams)
            self.m_layerVideoKeys[self.m_layerVideoKeyCount++] = key;
        *width = source.width;
        *height = source.height;
        *frames = source.frames;
        return source.texture;
    }
    return 0;
}

void PS5Renderer::drawScene()
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_width, m_height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindVertexArray(m_vao);
    glActiveTexture(GL_TEXTURE0);

    auto ndc = [&](double x, double y, double w, double h, float out[4]) {
        out[0] = static_cast<float>(x / m_width * 2.0 - 1.0);
        out[1] = static_cast<float>(1.0 - (y + h) / m_height * 2.0);
        out[2] = static_cast<float>((x + w) / m_width * 2.0 - 1.0);
        out[3] = static_cast<float>(1.0 - y / m_height * 2.0);
    };

    // Where the page goes: below the application's bar (setPageTop), scaled
    // to fit, centred (normally 1:1). A page as wide as the screen is drawn
    // 1:1 from the top of its area: just after the bar comes or goes, the
    // last frame at the old height stays until WebKit paints one at the new
    // (cut at the bottom, or black there, for a frame or two). With a
    // composited scene, the scene texture is the page.
    const int contentWidth = m_sceneTexture ? m_sceneWidth : m_pageWidth;
    const int contentHeight = m_sceneTexture ? m_sceneHeight : m_pageHeight;
    const int top = m_pageTop.load(std::memory_order_relaxed), areaHeight = m_height - top;
    double scale = 1, pageX = 0, pageY = 0, pageW = 0, pageH = 0;
    if (contentWidth > 0) {
        if (contentWidth != m_width)
            scale = std::min(static_cast<double>(m_width) / contentWidth, static_cast<double>(areaHeight) / contentHeight);
        pageW = contentWidth * scale;
        pageH = contentHeight * scale;
        pageX = (m_width - pageW) / 2;
        pageY = top + (contentWidth != m_width ? (areaHeight - pageH) / 2 : 0);
        // Lifted above the on-screen keyboard (videos and the page together).
        pageY -= m_keyboardLift.load(std::memory_order_relaxed);
    }

    // Videos under the page, clipped to it.
    if (m_videoDrawCount && contentWidth > 0) {
        glUseProgram(m_program);
        glUniform1i(m_uTexture, 0);
        glUniform1i(m_uOpaque, 1);
        glDisable(GL_BLEND);
        glEnable(GL_SCISSOR_TEST);
        glScissor(static_cast<GLint>(pageX), static_cast<GLint>(m_height - pageY - pageH), static_cast<GLsizei>(pageW), static_cast<GLsizei>(pageH));
        for (unsigned i = 0; i < m_videoDrawCount; i++) {
            const auto& v = m_videoDraws[i];
            bool layer = false;
            for (unsigned k = 0; m_sceneTexture && v.key && k < m_layerVideoKeyCount; k++)
                layer |= m_layerVideoKeys[k] == v.key;
            if (layer)
                continue; // a video layer of the composited scene
            float r[4];
            ndc(pageX + v.x * scale, pageY + v.y * scale, v.width * scale, v.height * scale, r);
            glBindTexture(GL_TEXTURE_2D, v.texture);
            glUniform4f(m_uRect, r[0], r[1], r[2], r[3]);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        glDisable(GL_SCISSOR_TEST);
    }

    // The page, premultiplied, over the videos (holes are transparent).
    if (contentWidth > 0) {
        float r[4];
        ndc(pageX, pageY, pageW, pageH, r);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        if (m_sceneTexture) {
            // The composited scene (TextureMapper painted it top row first).
            glUseProgram(m_program);
            glUniform1i(m_uTexture, 0);
            glUniform1i(m_uOpaque, 0);
            glBindTexture(GL_TEXTURE_2D, m_sceneTexture);
            glUniform4f(m_uRect, r[0], r[1], r[2], r[3]);
        } else if (!m_pageAsTexture) {
            glUseProgram(m_pageProgram);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_pageBuffer);
            glUniform4f(m_uPageNDC, r[0], r[1], r[2], r[3]);
            glUniform4f(m_uPageRect, static_cast<float>(pageX), static_cast<float>(pageY), static_cast<float>(pageW), static_cast<float>(pageH));
            glUniform2i(m_uPageSize, m_pageWidth, m_pageHeight);
            glUniform1i(m_uPageStride, m_pageWidth);
            glUniform1i(m_uScreenHeight, m_height);
        } else {
            glUseProgram(m_program);
            glUniform1i(m_uTexture, 0);
            glUniform1i(m_uOpaque, 0);
            glBindTexture(GL_TEXTURE_2D, m_pageTexture);
            glUniform4f(m_uRect, r[0], r[1], r[2], r[3]);
        }
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // The application's UI (address bar, menu) over the page.
    drawUILayer();

    // A message over the page (bottom centre), e.g. "No headset".
    if (m_messageTexture && m_messageUntilUS)
        drawPanel(m_messageTexture, m_messageWidth, m_messageHeight, m_width / 2.0, m_height - m_messageHeight / 2.0 - 60, 1.0);

    if (m_drawPointer) {
        float r[4];
        ndc(m_drawPointerX - pointerSize / 2.0, m_drawPointerY - pointerSize / 2.0, pointerSize, pointerSize, r);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(m_program);
        glUniform1i(m_uTexture, 0);
        glUniform1i(m_uOpaque, 0);
        glBindTexture(GL_TEXTURE_2D, m_pointerTexture);
        glUniform4f(m_uRect, r[0], r[1], r[2], r[3]);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // The performance overlay, over everything.
    if (m_perfOn && m_perf) {
        PS5PerfState state { m_compositorReady ? m_compositor : nullptr, m_compositor != nullptr, m_sceneTexture != 0,
            m_pageWidth, m_pageHeight, m_pageBufferBytes, m_videoFramesConverted, m_vao };
        m_perf->draw(m_width, m_height, state);
    }
    glDisable(GL_BLEND);
}

// A premultiplied RGBA panel (PS5Text) centred at (centerX, centerY), TV pixels.
void PS5Renderer::drawPanel(unsigned texture, int width, int height, double centerX, double centerY, double scale)
{
    const double w = width * scale, h = height * scale, x = centerX - w / 2, y = centerY - h / 2;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_width, m_height);
    glBindVertexArray(m_vao);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(m_program);
    glUniform1i(m_uTexture, 0);
    glUniform1i(m_uOpaque, 0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform4f(m_uRect, static_cast<float>(x / m_width * 2.0 - 1.0), static_cast<float>(1.0 - (y + h) / m_height * 2.0),
        static_cast<float>((x + w) / m_width * 2.0 - 1.0), static_cast<float>(1.0 - y / m_height * 2.0));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
}

// Uploads a PS5Text image into a texture (made on first use).
static void uploadPanel(unsigned& texture, const PS5TextImage& image)
{
    if (!texture) {
        GLuint name = 0;
        glGenTextures(1, &name);
        texture = name;
    }
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
}

void ps5RendererUploadPanel(unsigned& texture, const PS5TextImage& image)
{
    uploadPanel(texture, image);
}

void PS5Renderer::setUILayer(const uint8_t* bgra, int x, int y, int width, int height)
{
    if (!m_initialized)
        return;
    g_mutex_lock(&m_lock);
    if (bgra && width > 0 && height > 0) {
        m_uiPending.assign(bgra, bgra + static_cast<size_t>(width) * height * 4);
        m_uiPendingRect[0] = x;
        m_uiPendingRect[1] = y;
        m_uiPendingRect[2] = width;
        m_uiPendingRect[3] = height;
    } else {
        m_uiPending.clear();
        memset(m_uiPendingRect, 0, sizeof m_uiPendingRect);
    }
    m_uiChanged = true;
    m_frameRequested = true;
    g_cond_broadcast(&m_cond);
    g_mutex_unlock(&m_lock);
}

// Render thread: a new UI image goes into the GL buffer (the previous
// frame's GPU work is done, the swap waited for it), or a texture.
void PS5Renderer::applyUILayer()
{
    if (!m_uiStagingNew)
        return;
    m_uiStagingNew = false;
    const int width = m_uiRect[2], height = m_uiRect[3];
    const size_t needed = static_cast<size_t>(std::max(0, width)) * std::max(0, height) * 4;
    if (!needed || m_uiStaging.size() < needed) {
        m_uiRect[2] = 0;
        m_uiStaging.clear();
        m_uiStaging.shrink_to_fit();
        return;
    }
    if (!m_uiAsTexture && m_pageProgram && needed > m_uiBufferBytes) {
        if (m_uiBuffer)
            glDeleteBuffers(1, &m_uiBuffer);
        // Room for a whole screen, so a bigger menu doesn't reallocate.
        size_t bytes = std::max(needed, static_cast<size_t>(m_width) * m_height * 4);
        const GLbitfield flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        glGenBuffers(1, &m_uiBuffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_uiBuffer);
        glBufferStorage(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr, flags);
        m_uiMap = static_cast<uint8_t*>(glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(bytes), flags));
        if (!m_uiMap) {
            g_message("[wpe-ps5] ui layer: buffer map failed (GL error 0x%x): a texture instead", glGetError());
            glDeleteBuffers(1, &m_uiBuffer);
            m_uiBuffer = 0;
            m_uiBufferBytes = 0;
            m_uiAsTexture = true;
        } else
            m_uiBufferBytes = bytes;
    }
    if (!m_uiAsTexture && m_pageProgram && m_uiMap)
        memcpy(m_uiMap, m_uiStaging.data(), needed);
    else {
        m_uiAsTexture = true;
        if (!m_uiTexture) {
            GLuint name = 0;
            glGenTextures(1, &name);
            m_uiTexture = name;
        }
        glBindTexture(GL_TEXTURE_2D, m_uiTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, kPS5BGRAInternalFormat, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, m_uiStaging.data());
    }
    // The copy is in GL now: no second 8 MB kept on the CPU side.
    m_uiStaging.clear();
    m_uiStaging.shrink_to_fit();
}

// Fades: a menu that opens or closes, the loading bar coming and going.
// Off with /data/ps5webkit/no-ui-fade (a file), and on the texture path.
// Under m_lock: no GL here.
bool PS5Renderer::uiFadeWanted()
{
    static const bool wanted = [] {
        struct stat st;
        return stat("/data/ps5webkit/no-ui-fade", &st) != 0;
    }();
    return wanted && !m_uiAsTexture && m_pageProgram && (!m_uiProgramTried || m_uiProgram);
}

// The fade's program, made on first use (render thread, not under m_lock).
void PS5Renderer::ensureUIProgram()
{
    if (m_uiProgramTried)
        return;
    m_uiProgramTried = true;
    char error[512] = { };
    m_uiProgram = link(kVertexShader, kUIFragmentShader, error, sizeof(error));
    if (!m_uiProgram) {
        g_message("[wpe-ps5] ui layer: fade shader failed (%s): no fades", error);
        return;
    }
    m_uUINDC = glGetUniformLocation(m_uiProgram, "rect");
    m_uUIRect = glGetUniformLocation(m_uiProgram, "pageRect");
    m_uUISize = glGetUniformLocation(m_uiProgram, "pageSize");
    m_uUIStride = glGetUniformLocation(m_uiProgram, "pageStride");
    m_uUIScreenHeight = glGetUniformLocation(m_uiProgram, "screenHeight");
    m_uUIOpacity = glGetUniformLocation(m_uiProgram, "opacity");
}

void PS5Renderer::drawUILayer()
{
    const int x = m_uiRect[0], y = m_uiRect[1], w = m_uiRect[2], h = m_uiRect[3];
    if (w <= 0 || h <= 0 || (!m_uiMap && !m_uiTexture))
        return;
    // In over 150 ms (ease out), out over 120 ms (ease in).
    float opacity = 1;
    if (m_uiFadeStartUS)
        ensureUIProgram();
    if (m_uiFadeStartUS && !m_uiProgram) {
        m_uiFadeStartUS = 0;
        if (m_uiFadingOut) {
            m_uiFadingOut = false;
            m_uiRect[2] = 0;
            return;
        }
    }
    if (m_uiFadeStartUS) {
        const double t = std::min(1.0, (nowUS() - m_uiFadeStartUS) / (m_uiFadingOut ? 120000.0 : 150000.0));
        if (m_uiFadingOut) {
            if (t >= 1) {
                m_uiRect[2] = 0;
                m_uiFadingOut = false;
                m_uiFadeStartUS = 0;
                return;
            }
            opacity = static_cast<float>((1 - t) * (1 - t));
        } else {
            opacity = static_cast<float>(1 - (1 - t) * (1 - t) * (1 - t));
            if (t >= 1)
                m_uiFadeStartUS = 0;
        }
    }
    float r[4] = {
        static_cast<float>(static_cast<double>(x) / m_width * 2.0 - 1.0),
        static_cast<float>(1.0 - static_cast<double>(y + h) / m_height * 2.0),
        static_cast<float>(static_cast<double>(x + w) / m_width * 2.0 - 1.0),
        static_cast<float>(1.0 - static_cast<double>(y) / m_height * 2.0),
    };
    glBindVertexArray(m_vao);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    if (!m_uiAsTexture && m_uiProgram && opacity < 1) {
        glUseProgram(m_uiProgram);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_uiBuffer);
        glUniform4f(m_uUINDC, r[0], r[1], r[2], r[3]);
        glUniform4f(m_uUIRect, static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h));
        glUniform2i(m_uUISize, w, h);
        glUniform1i(m_uUIStride, w);
        glUniform1i(m_uUIScreenHeight, m_height);
        glUniform1f(m_uUIOpacity, opacity);
    } else if (!m_uiAsTexture) {
        glUseProgram(m_pageProgram);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_uiBuffer);
        glUniform4f(m_uPageNDC, r[0], r[1], r[2], r[3]);
        glUniform4f(m_uPageRect, static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h));
        glUniform2i(m_uPageSize, w, h);
        glUniform1i(m_uPageStride, w);
        glUniform1i(m_uScreenHeight, m_height);
    } else {
        glUseProgram(m_program);
        glUniform1i(m_uTexture, 0);
        glUniform1i(m_uOpaque, 0);
        glBindTexture(GL_TEXTURE_2D, m_uiTexture);
        glUniform4f(m_uRect, r[0], r[1], r[2], r[3]);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void PS5Renderer::showMessage(const char* text, double seconds)
{
    g_mutex_lock(&m_lock);
    m_messageText = text ? text : "";
    m_messageSeconds = seconds;
    m_messageChanged = true;
    g_cond_broadcast(&m_cond);
    g_mutex_unlock(&m_lock);
}

// Render thread: a new message is rasterized; an expired one comes down.
void PS5Renderer::updateMessage(int64_t now)
{
    g_mutex_lock(&m_lock);
    bool changed = m_messageChanged;
    std::string text = m_messageText;
    double seconds = m_messageSeconds;
    m_messageChanged = false;
    g_mutex_unlock(&m_lock);
    if (changed) {
        PS5TextImage image;
        if (!text.empty() && seconds > 0 && ps5RenderTextPanel(text.c_str(), 40, image)) {
            uploadPanel(m_messageTexture, image);
            m_messageWidth = image.width;
            m_messageHeight = image.height;
            m_messageUntilUS = now + static_cast<int64_t>(seconds * 1e6);
        } else
            m_messageUntilUS = 0;
    }
    if (m_messageUntilUS && now >= m_messageUntilUS)
        m_messageUntilUS = 0;
}

void PS5Renderer::writeCapture(char* path)
{
    const int w = m_width, h = m_height;
    const int ow = w / 2, oh = h / 2;
    int64_t start = nowUS();
    auto* rgba = static_cast<uint8_t*>(g_try_malloc(static_cast<size_t>(w) * h * 4));
    auto* rgb = static_cast<uint8_t*>(g_try_malloc(static_cast<size_t>(ow) * oh * 3));
    if (!rgba || !rgb) {
        g_message("[wpe-ps5] capture %s: out of memory", path);
        g_free(rgba);
        g_free(rgb);
        g_free(path);
        return;
    }
    glFinish();
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    GLenum glError = glGetError();
    // GL rows are bottom-up: output row y comes from GL rows h-2y-1 and h-2y-2.
    uint64_t sum = 0;
    unsigned nonBlack = 0;
    for (int y = 0; y < oh; y++) {
        const uint8_t* r0 = rgba + static_cast<size_t>(h - 2 * y - 1) * w * 4;
        const uint8_t* r1 = rgba + static_cast<size_t>(h - 2 * y - 2) * w * 4;
        uint8_t* out = rgb + static_cast<size_t>(y) * ow * 3;
        for (int x = 0; x < ow; x++) {
            for (int c = 0; c < 3; c++) {
                unsigned v = r0[8 * x + c] + r0[8 * x + 4 + c] + r1[8 * x + c] + r1[8 * x + 4 + c];
                out[3 * x + c] = static_cast<uint8_t>((v + 2) / 4);
                sum += out[3 * x + c];
            }
            if (out[3 * x] | out[3 * x + 1] | out[3 * x + 2])
                nonBlack++;
        }
    }
    FILE* file = fopen(path, "wb");
    bool ok = false;
    if (file) {
        fprintf(file, "P6\n%d %d\n255\n", ow, oh);
        ok = fwrite(rgb, 1, static_cast<size_t>(ow) * oh * 3, file) == static_cast<size_t>(ow) * oh * 3;
        ok = !fclose(file) && ok;
    }
    g_message("[wpe-ps5] SHOT %s %dx%d %s (gl error 0x%x, mean %.1f, %.1f%% non-black, %.0f ms)", path, ow, oh,
        ok ? "written" : "FAILED", glError, static_cast<double>(sum) / (static_cast<double>(ow) * oh * 3),
        100.0 * nonBlack / (static_cast<double>(ow) * oh), (nowUS() - start) / 1000.0);
    g_free(rgba);
    g_free(rgb);
    g_free(path);
}
