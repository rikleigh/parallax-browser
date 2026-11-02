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
#include "PS5PerfOverlay.h"

#include "PS5Compositor.h"
#include "PS5VideoDecoder.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <epoxy/gl.h>
#include <errno.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <glib.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/tcp_var.h>
#include <time.h>
#include <vector>

extern "C" double ps5wk_title_direct_mb(void) __attribute__((weak)); // apps/title/titlemem.c
extern "C" int sceKernelAvailableFlexibleMemorySize(size_t*) __attribute__((weak));

static int64_t nowUS()
{
    return g_get_monotonic_time();
}

// ------------------------------------------------- the children's lines

namespace {

struct ChildStats {
    char tag[24];
    bool network;
    bool alive;
    // "[perf] cpu <cores> rss <MB> pas <MB>", once a second.
    double cpu, rssMB, pasMB;
    int64_t perfAt;
    // NetworkProcess: " tcp <bytes in> <bytes out>" (system-wide totals).
    bool tcp, tcpInterfaces;
    uint64_t tcpIn, tcpOut;
    // "[render] [wc: ]<n> updates in <s> s (<rate>/s), <avg> ms average[, p50 <x>, p95 <y>]", every 5 s.
    double updateRate, updateMS, updateP95MS;
    int64_t renderAt;
};

GMutex s_childLock; // static: no g_mutex_init needed
ChildStats s_children[8];

struct Counter {
    char name[24];
    uint64_t value;
};
GMutex s_counterLock;
constexpr unsigned kCounters = 6;
Counter s_counters[kCounters];

} // namespace

// s_childLock held.
static ChildStats& childSlot(const char* tag)
{
    ChildStats* empty = nullptr;
    ChildStats* ended = nullptr;
    for (auto& child : s_children) {
        if (child.tag[0] && !strcmp(child.tag, tag))
            return child;
        if (!child.tag[0] && !empty)
            empty = &child;
        else if (child.tag[0] && !child.alive && !ended)
            ended = &child;
    }
    ChildStats& slot = empty ? *empty : ended ? *ended : s_children[0];
    slot = { };
    snprintf(slot.tag, sizeof slot.tag, "%s", tag);
    slot.network = !strncmp(tag, "network", 7);
    slot.alive = true;
    slot.cpu = slot.rssMB = slot.pasMB = slot.updateRate = slot.updateMS = slot.updateP95MS = -1;
    return slot;
}

bool ps5PerfNoteChildLine(const char* tag, const char* line)
{
    if (!tag || !line || line[0] != '[')
        return false;
    if (!strncmp(line, "[perf] ", 7)) {
        double cpu = -1, rss = -1, pas = -1;
        unsigned long long tcpIn = 0, tcpOut = 0;
        int n = sscanf(line, "[perf] cpu %lf rss %lf pas %lf", &cpu, &rss, &pas);
        // " tcp <in> <out>" (TCP counters) or " if <in> <out>" (interfaces).
        const char* bytes = strstr(line, " tcp ");
        if (!bytes)
            bytes = strstr(line, " if ");
        bool haveBytes = bytes && sscanf(strchr(bytes + 1, ' '), " %llu %llu", &tcpIn, &tcpOut) == 2;
        if (n >= 1) {
            g_mutex_lock(&s_childLock);
            ChildStats& child = childSlot(tag);
            child.alive = true;
            child.cpu = cpu;
            child.rssMB = rss;
            child.pasMB = pas;
            child.perfAt = nowUS();
            child.tcp = haveBytes;
            child.tcpInterfaces = haveBytes && bytes[1] == 'i';
            child.tcpIn = tcpIn;
            child.tcpOut = tcpOut;
            g_mutex_unlock(&s_childLock);
        }
        return true;
    }
    if (!strncmp(line, "[render] ", 9)) {
        const char* p = line + 9;
        if (!strncmp(p, "wc: ", 4))
            p += 4;
        unsigned count = 0;
        double seconds = 0, rate = 0, average = 0, p50 = 0, p95 = 0;
        int n = sscanf(p, "%u updates in %lf s (%lf/s), %lf ms average, p50 %lf, p95 %lf", &count, &seconds, &rate, &average, &p50, &p95);
        if (n >= 4) {
            g_mutex_lock(&s_childLock);
            ChildStats& child = childSlot(tag);
            child.updateRate = rate;
            child.updateMS = average;
            child.updateP95MS = n >= 6 ? p95 : -1;
            child.renderAt = nowUS();
            g_mutex_unlock(&s_childLock);
        }
    }
    return false;
}

void ps5PerfChildEnded(const char* tag)
{
    g_mutex_lock(&s_childLock);
    for (auto& child : s_children) {
        if (child.tag[0] && !strcmp(child.tag, tag))
            child.alive = false;
    }
    g_mutex_unlock(&s_childLock);
}

void ps5PerfSetCounter(const char* name, uint64_t value)
{
    if (!name || !*name)
        return;
    g_mutex_lock(&s_counterLock);
    Counter* slot = nullptr;
    for (auto& counter : s_counters) {
        if (counter.name[0] && !strcmp(counter.name, name)) {
            slot = &counter;
            break;
        }
        if (!counter.name[0] && !slot)
            slot = &counter;
    }
    if (slot) {
        if (!slot->name[0])
            snprintf(slot->name, sizeof slot->name, "%s", name);
        slot->value = value;
    }
    g_mutex_unlock(&s_counterLock);
}

// ------------------------------------------------------------ the sampler

namespace {

struct Sample {
    int64_t at { 0 };
    double processCPU { -1 }, renderCPU { -1 }, mainCPU { -1 }; // seconds so far
    int64_t mainPollUS { 0 };
    double flexibleMB { -1 }, directMB { -1 };
    bool tcp { false };
    uint64_t tcpIn { 0 }, tcpOut { 0 };
    PS5VideoDecoder::Budget budget { };
};

struct Vertex {
    float x, y, u, v;
    uint32_t color; // premultiplied RGBA8
};

} // namespace

// The calling thread's CPU time (s): getrusage(RUSAGE_THREAD); the thread
// CPU clocks (pthread_getcpuclockid) read 0 on the PS5.
static double selfThreadCPUSeconds()
{
    struct rusage usage;
    if (!getrusage(RUSAGE_THREAD, &usage))
        return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
    struct timespec ts;
    if (!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts))
        return ts.tv_sec + ts.tv_nsec / 1e9;
    return -1;
}

// The main thread measures itself while the overlay is on (a GLib timeout,
// every 250 ms): CPU seconds (us) and when.
static std::atomic<int64_t> s_mainCPUUS { -1 }, s_mainCPUAt { 0 };
static std::atomic<bool> s_mainSamplerOn { false };
static bool s_mainSamplerRunning; // main thread

void ps5PerfMainThreadSampler(bool on)
{
    s_mainSamplerOn = on;
    if (!on || s_mainSamplerRunning)
        return;
    s_mainSamplerRunning = true;
    g_timeout_add(250, [](gpointer) -> gboolean {
        if (!s_mainSamplerOn) {
            s_mainSamplerRunning = false;
            return G_SOURCE_REMOVE;
        }
        double cpu = selfThreadCPUSeconds();
        s_mainCPUUS = cpu >= 0 ? static_cast<int64_t>(cpu * 1e6) : -1;
        s_mainCPUAt = nowUS();
        return G_SOURCE_CONTINUE;
    }, nullptr);
}

static double threadCPUSeconds(pthread_t thread)
{
    clockid_t clock;
    struct timespec ts;
    if (pthread_getcpuclockid(thread, &clock) || clock_gettime(clock, &ts))
        return -1;
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// TCP payload bytes in and out, system-wide (the browser is all there is).
static bool tcpBytes(uint64_t& in, uint64_t& out)
{
    struct tcpstat stats;
    memset(&stats, 0, sizeof stats);
    size_t size = sizeof stats;
    if (sysctlbyname("net.inet.tcp.stats", &stats, &size, nullptr, 0) && errno != ENOMEM)
        return false;
    if (size < offsetof(struct tcpstat, tcps_rcvbyte) + sizeof(stats.tcps_rcvbyte))
        return false;
    in = stats.tcps_rcvbyte;
    out = stats.tcps_sndbyte;
    return true;
}

// Premultiplied RGBA8, bytes r g b a in memory.
static constexpr uint32_t rgba(unsigned r, unsigned g, unsigned b, double a)
{
    return static_cast<uint32_t>(r * a) | static_cast<uint32_t>(g * a) << 8 | static_cast<uint32_t>(b * a) << 16
        | static_cast<uint32_t>(255 * a) << 24;
}

// Text colours, as control characters in the overlay's lines.
enum : char { cLabel = 1, cValue, cWarn, cBad, cGood, cDim };
static const uint32_t kTextColors[] = {
    rgba(255, 255, 255, 1), // (unused)
    rgba(130, 195, 255, 1), // label
    rgba(255, 255, 255, 1), // value
    rgba(255, 205, 70, 1), // warn
    rgba(255, 95, 85, 1), // bad
    rgba(110, 230, 120, 1), // good
    rgba(150, 150, 150, 1), // dim
};

static std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static std::string format(const char* fmt, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof buffer, fmt, args);
    va_end(args);
    return buffer;
}

struct PS5PerfOverlay::Impl {
    pthread_t renderThread { }, mainThread { };
    int corner { 1 }; // 0 top left, 1 top right, 2 bottom left, 3 bottom right
    int size1080 { 22 }; // font pixel size at 1080p
    bool graph { true };

    // Sampler thread (lock/cond guard active, quit, latest).
    pthread_t sampler { };
    bool samplerStarted { false };
    GMutex lock;
    GCond cond;
    bool active { false };
    Sample latest;

    // Render thread.
    Sample previous;
    PS5CompositorPerf previousWC { };
    bool havePreviousWC { false };
    int64_t previousWCAt { 0 };
    uint64_t previousVideoFrames { 0 };
    int64_t previousVideoAt { 0 };
    uint64_t previousCounters[kCounters] { };
    char previousCounterNames[kCounters][24] { };
    int64_t previousCountersAt { 0 };
    struct Window {
        int64_t start;
        unsigned frames, intervals, missed, uploads, composited, ovlFrames;
        double renderSum, renderMax, workSum, workMax, swapSum, swapMax, applySum, videoSum, sceneSum, drawSum, intervalSum, intervalMax, uploadSum;
        double ovlSum, ovlMax;
    } win { };
    int64_t lastSwapEnd { 0 };
    static constexpr unsigned kRing = 512;
    struct GraphSample {
        float intervalMS, workMS; // workMS: apply + video + scene (render minus draw, which waits for the flip)
    };
    GraphSample ring[kRing] { };
    unsigned ringHead { 0 }, ringCount { 0 };
    double rebuildMS { 0 };
    int64_t previousMainCPUUS { -1 }, previousMainCPUAt { 0 }, previousRenderAt { 0 };
    double previousRenderCPU { -1 };
    unsigned rebuilds { 0 };
    uint64_t netIn { 0 }, netOut { 0 };
    int64_t netAt { 0 };
    double netInRate { 0 }, netOutRate { 0 };
    bool netRates { false };

    // GL (render thread).
    bool glTried { false }, glReady { false };
    GLuint program { 0 }, vao { 0 }, vbo { 0 }, atlas { 0 };
    GLint uScreen { -1 }, uAtlas { -1 };
    Vertex* map { nullptr }; // two halves of capacity vertices (one per frame, alternating)
    std::vector<Vertex> staging; // when the buffer could not be mapped
    size_t capacity { 0 };
    unsigned half { 0 };
    uint64_t textVersion { 0 }, halfTextVersion[2] { };
    std::vector<Vertex> textVertices, graphVertices;
    // Atlas metrics (pixels).
    int fontSize { 0 }, advance { 0 }, ascent { 0 }, lineHeight { 0 }, pad { 0 }, cellW { 0 }, cellH { 0 }, atlasW { 0 }, atlasH { 0 };
    float solidU { 0 }, solidV { 0 };
    int lastW { 0 }, lastH { 0 };
    // Layout of the panel (pixels), kept from one text update to the next.
    int panelX { 0 }, panelY { 0 }, panelW { 0 }, panelH { 0 }, graphX { 0 }, graphY { 0 }, graphW { 0 }, graphH { 0 };
    int widestLine { 0 };

    void parseOptions(const char*);
    Sample takeSample();
    bool ensureGL(int height);
    void addQuad(std::vector<Vertex>&, float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t color);
    void addSolid(std::vector<Vertex>&, float x, float y, float w, float h, uint32_t color);
    void rebuild(int64_t now, int width, int height, const PS5PerfState&);
    size_t buildGraph(Vertex* out, size_t room);
};

void PS5PerfOverlay::Impl::parseOptions(const char* options)
{
    if (!options)
        return;
    gchar** words = g_strsplit_set(options, " \t\r\n,", 0);
    for (unsigned i = 0; words[i]; i++) {
        const char* w = words[i];
        int value;
        if (!strcmp(w, "pos=tl"))
            corner = 0;
        else if (!strcmp(w, "pos=tr"))
            corner = 1;
        else if (!strcmp(w, "pos=bl"))
            corner = 2;
        else if (!strcmp(w, "pos=br"))
            corner = 3;
        else if (sscanf(w, "size=%d", &value) == 1)
            size1080 = std::clamp(value, 10, 48);
        else if (sscanf(w, "graph=%d", &value) == 1)
            graph = value;
    }
    g_strfreev(words);
}

Sample PS5PerfOverlay::Impl::takeSample()
{
    Sample s;
    s.at = nowUS();
    struct rusage usage;
    if (!getrusage(RUSAGE_SELF, &usage))
        s.processCPU = usage.ru_utime.tv_sec + usage.ru_stime.tv_sec + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
    s.renderCPU = threadCPUSeconds(renderThread); // (diagnostics: the clocks read 0)
    s.mainCPU = threadCPUSeconds(mainThread);
    s.mainPollUS = ps5wkMainLoopPollUS();
    size_t flexible = 0;
    if (sceKernelAvailableFlexibleMemorySize && !sceKernelAvailableFlexibleMemorySize(&flexible))
        s.flexibleMB = flexible / 1048576.0;
    if (ps5wk_title_direct_mb)
        s.directMB = ps5wk_title_direct_mb();
    s.tcp = tcpBytes(s.tcpIn, s.tcpOut);
    PS5VideoDecoder::budget(s.budget);
    return s;
}

static void* samplerMain(void* data)
{
    auto& d = *static_cast<PS5PerfOverlay::Impl*>(data);
    g_mutex_lock(&d.lock);
    for (;;) {
        if (!d.active) {
            g_cond_wait(&d.cond, &d.lock);
            continue;
        }
        g_mutex_unlock(&d.lock);
        Sample sample = d.takeSample();
        g_mutex_lock(&d.lock);
        d.latest = sample;
        g_cond_wait_until(&d.cond, &d.lock, nowUS() + 250000); // at least one new sample per text update
    }
    return nullptr;
}

PS5PerfOverlay::PS5PerfOverlay(pthread_t renderThread, pthread_t mainThread, const char* options)
    : m_impl(new Impl)
{
    m_impl->renderThread = renderThread;
    m_impl->mainThread = mainThread;
    g_mutex_init(&m_impl->lock);
    g_cond_init(&m_impl->cond);
    m_impl->parseOptions(options);
}

PS5PerfOverlay::~PS5PerfOverlay()
{
    // Never destroyed (the renderer lives as long as the title).
}

void PS5PerfOverlay::setActive(bool active)
{
    Impl& d = *m_impl;
    g_mutex_lock(&d.lock);
    d.active = active;
    if (active && !d.samplerStarted) {
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, 256 << 10);
        d.samplerStarted = !pthread_create(&d.sampler, &attributes, samplerMain, &d);
        if (d.samplerStarted)
            pthread_detach(d.sampler);
        pthread_attr_destroy(&attributes);
    }
    g_cond_broadcast(&d.cond);
    g_mutex_unlock(&d.lock);
    if (active) {
        // Fresh windows: differences start from now.
        d.previous = { };
        d.havePreviousWC = false;
        d.previousVideoAt = 0;
        d.previousCountersAt = 0;
        d.win = { };
        d.lastSwapEnd = 0;
        d.ringCount = 0;
        d.widestLine = 0;
        d.netAt = 0;
        d.netRates = false;
        m_nextUpdateUS = 0;
    }
    g_message("[wpe-ps5] [perf] overlay %s", active ? "on" : "off");
}

void PS5PerfOverlay::noteFrame(const PS5PerfFrame& f)
{
    Impl& d = *m_impl;
    auto& w = d.win;
    if (!w.start)
        w.start = f.start;
    double render = (f.swapStart - f.start) / 1000.0, swap = (f.swapEnd - f.swapStart) / 1000.0;
    w.frames++;
    w.renderSum += render;
    w.renderMax = std::max(w.renderMax, render);
    w.swapSum += swap;
    w.swapMax = std::max(w.swapMax, swap);
    // Work: the frame before its draws (the first draw into the window
    // waits for the previous frame's flip, so "render" is mostly waiting).
    double work = (f.painted - f.start) / 1000.0;
    w.workSum += work;
    w.workMax = std::max(w.workMax, work);
    w.applySum += (f.applied - f.start) / 1000.0;
    w.videoSum += (f.consumed - f.applied) / 1000.0;
    w.sceneSum += (f.painted - f.consumed) / 1000.0;
    w.drawSum += (f.drawn - f.painted) / 1000.0;
    if (f.uploadMS > 0) {
        w.uploads++;
        w.uploadSum += f.uploadMS;
    }
    if (f.composited)
        w.composited++;
    float interval = d.lastSwapEnd ? (f.swapEnd - d.lastSwapEnd) / 1000.0f : 0;
    d.lastSwapEnd = f.swapEnd;
    // Frames less than 100 ms apart are a run (animation, scrolling, video);
    // a longer gap is the page sitting still.
    if (interval > 0 && interval < 100) {
        w.intervals++;
        w.intervalSum += interval;
        w.intervalMax = std::max<double>(w.intervalMax, interval);
        if (interval > 25)
            w.missed++;
    }
    d.ring[d.ringHead] = { interval, (f.painted - f.start) / 1000.0f };
    d.ringHead = (d.ringHead + 1) % Impl::kRing;
    d.ringCount = std::min(d.ringCount + 1, Impl::kRing);
}

static const char* kVertexShader = R"(#version 430 core
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 tc;
layout(location = 2) in vec4 col;
uniform vec2 screen;
out vec2 uv;
out vec4 color;
void main()
{
    uv = tc;
    color = col;
    gl_Position = vec4(pos.x / screen.x * 2.0 - 1.0, 1.0 - pos.y / screen.y * 2.0, 0.0, 1.0);
}
)";

static const char* kFragmentShader = R"(#version 430 core
uniform sampler2D atlas;
in vec2 uv;
in vec4 color;
out vec4 o;
void main()
{
    o = color * texture(atlas, uv).a;
}
)";

static GLuint compileShader(GLenum type, const char* source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = { };
        glGetShaderInfoLog(shader, sizeof log, nullptr, log);
        g_message("[wpe-ps5] [perf] shader: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool PS5PerfOverlay::Impl::ensureGL(int height)
{
    if (glTried)
        return glReady;
    glTried = true;
    int64_t start = nowUS();

    // The glyph atlas: printable ASCII in 16 x 6 cells of the monospaced
    // font, plus one solid cell (panel, graph), white with coverage in every
    // channel (premultiplied).
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    static const char* const fonts[] = { "/data/ps5webkit/fonts/NotoSansMono-Bold.ttf", "/data/ps5webkit/fonts/NotoSans-Bold.ttf" };
    if (FT_Init_FreeType(&library)) {
        g_message("[wpe-ps5] [perf] FT_Init_FreeType failed: no overlay");
        return false;
    }
    for (const char* font : fonts) {
        if (!FT_New_Face(library, font, 0, &face))
            break;
        face = nullptr;
    }
    if (!face) {
        g_message("[wpe-ps5] [perf] no font in /data/ps5webkit/fonts: no overlay");
        FT_Done_FreeType(library);
        return false;
    }
    fontSize = std::max(10, static_cast<int>(std::lround(size1080 * height / 1080.0)));
    FT_Set_Pixel_Sizes(face, 0, fontSize);
    ascent = static_cast<int>((face->size->metrics.ascender + 32) >> 6);
    int descent = static_cast<int>((-face->size->metrics.descender + 32) >> 6);
    advance = FT_Load_Char(face, 'M', FT_LOAD_DEFAULT) ? fontSize * 6 / 10 : static_cast<int>((face->glyph->advance.x + 32) >> 6);
    lineHeight = ascent + descent + std::max(2, fontSize / 8);
    pad = 2 + fontSize / 10;
    cellW = advance + 2 * pad;
    cellH = ascent + descent + 2 * pad;
    atlasW = 16 * cellW;
    atlasH = 6 * cellH;
    std::vector<uint8_t> pixels(static_cast<size_t>(atlasW) * atlasH * 4, 0);
    for (int c = 33; c < 127; c++) {
        if (FT_Load_Char(face, c, FT_LOAD_RENDER))
            continue;
        const FT_Bitmap& bitmap = face->glyph->bitmap;
        int index = c - 32, cellX = (index % 16) * cellW, cellY = (index / 16) * cellH;
        int left = pad + face->glyph->bitmap_left, top = pad + ascent - face->glyph->bitmap_top;
        for (unsigned row = 0; row < bitmap.rows; row++) {
            int y = top + static_cast<int>(row);
            if (y < 0 || y >= cellH)
                continue;
            for (unsigned col = 0; col < bitmap.width; col++) {
                int x = left + static_cast<int>(col);
                if (x < 0 || x >= cellW)
                    continue;
                uint8_t g = bitmap.buffer[row * bitmap.pitch + col];
                uint8_t* p = &pixels[(static_cast<size_t>(cellY + y) * atlasW + cellX + x) * 4];
                p[0] = p[1] = p[2] = p[3] = std::max(p[3], g);
            }
        }
    }
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    {
        int index = 127 - 32, cellX = (index % 16) * cellW, cellY = (index / 16) * cellH;
        for (int y = 0; y < cellH; y++)
            memset(&pixels[(static_cast<size_t>(cellY + y) * atlasW + cellX) * 4], 255, static_cast<size_t>(cellW) * 4);
        solidU = (cellX + cellW / 2.0f) / atlasW;
        solidV = (cellY + cellH / 2.0f) / atlasH;
    }
    glGenTextures(1, &atlas);
    glBindTexture(GL_TEXTURE_2D, atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, atlasW, atlasH, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = vs ? compileShader(GL_FRAGMENT_SHADER, kFragmentShader) : 0;
    if (!vs || !fs) {
        if (vs)
            glDeleteShader(vs);
        return false;
    }
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = { };
        glGetProgramInfoLog(program, sizeof log, nullptr, log);
        g_message("[wpe-ps5] [perf] program: %s", log);
        return false;
    }
    uScreen = glGetUniformLocation(program, "screen");
    uAtlas = glGetUniformLocation(program, "atlas");

    // The vertices: panel + glyphs (rewritten twice a second) and the graph
    // (every frame), in the half of the buffer the previous frame did not use.
    capacity = 4096 * 6;
    const GLsizeiptr bytes = static_cast<GLsizeiptr>(2 * capacity * sizeof(Vertex));
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    glBufferStorage(GL_ARRAY_BUFFER, bytes, nullptr, flags | GL_DYNAMIC_STORAGE_BIT);
    map = static_cast<Vertex*>(glMapBufferRange(GL_ARRAY_BUFFER, 0, bytes, flags));
    if (!map)
        staging.resize(capacity);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, color)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    GLenum error = glGetError();
    glReady = !error;
    g_message("[wpe-ps5] [perf] overlay ready: font %d px (cell %dx%d, line %d), atlas %dx%d, vertex buffer %s, GL error 0x%x, %.1f ms",
        fontSize, advance, cellH, lineHeight, atlasW, atlasH, map ? "mapped" : "not mapped (glBufferSubData)", error, (nowUS() - start) / 1000.0);
    return glReady;
}

void PS5PerfOverlay::Impl::addQuad(std::vector<Vertex>& out, float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t color)
{
    const Vertex a { x, y, u0, v0, color }, b { x + w, y, u1, v0, color }, c { x, y + h, u0, v1, color }, e { x + w, y + h, u1, v1, color };
    out.push_back(a);
    out.push_back(b);
    out.push_back(c);
    out.push_back(c);
    out.push_back(b);
    out.push_back(e);
}

void PS5PerfOverlay::Impl::addSolid(std::vector<Vertex>& out, float x, float y, float w, float h, uint32_t color)
{
    addQuad(out, x, y, w, h, solidU, solidV, solidU, solidV, color);
}

static unsigned visibleLength(const std::string& line)
{
    unsigned n = 0;
    for (char c : line)
        n += static_cast<unsigned char>(c) >= 32;
    return n;
}

void PS5PerfOverlay::Impl::rebuild(int64_t now, int width, int height, const PS5PerfState& state)
{
    // Numbers over the time since the previous text update.
    const auto& w = win;
    const double seconds = w.start ? std::max(0.001, (now - w.start) / 1e6) : 0.5;
    const double frames = std::max(1u, w.frames);
    Sample sample;
    g_mutex_lock(&lock);
    sample = latest;
    g_mutex_unlock(&lock);
    const bool haveSample = sample.at && previous.at && sample.at > previous.at;
    const double sampleSeconds = haveSample ? (sample.at - previous.at) / 1e6 : 0;

    PS5CompositorPerf wc { };
    bool haveWC = state.compositor && state.compositor->perfSnapshot(wc);
    double wcSeconds = 0;
    PS5CompositorPerf dwc { };
    if (haveWC) {
        if (!havePreviousWC) {
            previousWC = wc;
            previousWCAt = now;
        }
        wcSeconds = std::max(0.001, (now - previousWCAt) / 1e6);
        dwc.updates = wc.updates - previousWC.updates;
        dwc.updateApplyUS = wc.updateApplyUS - previousWC.updateApplyUS;
        dwc.paints = wc.paints - previousWC.paints;
        dwc.paintUS = wc.paintUS - previousWC.paintUS;
        dwc.tileUploads = wc.tileUploads - previousWC.tileUploads;
        dwc.tileUploadBytes = wc.tileUploadBytes - previousWC.tileUploadBytes;
        dwc.scrollFrames = wc.scrollFrames - previousWC.scrollFrames;
        dwc.scrollLatencies = wc.scrollLatencies - previousWC.scrollLatencies;
        dwc.scrollLatencyUS = wc.scrollLatencyUS - previousWC.scrollLatencyUS;
        dwc.clampedEvents = wc.clampedEvents - previousWC.clampedEvents;
        dwc.wheelEvents = wc.wheelEvents - previousWC.wheelEvents;
    }

    ChildStats children[8];
    g_mutex_lock(&s_childLock);
    memcpy(children, s_children, sizeof children);
    g_mutex_unlock(&s_childLock);
    // The live WebProcess (the newest web child with fresh lines) and NetworkProcess.
    const ChildStats* web = nullptr;
    const ChildStats* net = nullptr;
    for (const auto& child : children) {
        if (!child.tag[0] || !child.alive || now - child.perfAt > 5000000)
            continue;
        const ChildStats*& slot = child.network ? net : web;
        if (!slot || child.perfAt > slot->perfAt)
            slot = &child;
    }
    double webCPU = 0, netCPU = 0;
    unsigned webCount = 0;
    for (const auto& child : children) {
        if (!child.tag[0] || !child.alive || now - child.perfAt > 5000000 || child.cpu < 0)
            continue;
        if (child.network)
            netCPU += child.cpu;
        else {
            webCPU += child.cpu;
            webCount++;
        }
    }
    const ChildStats* render = nullptr; // freshest [render] line
    for (const auto& child : children) {
        if (child.tag[0] && child.alive && child.renderAt && now - child.renderAt < 15000000 && (!render || child.renderAt > render->renderAt))
            render = &child;
    }

    std::vector<std::string> lines;
    auto ms = [](double v, double warn, double bad) { return v >= bad ? cBad : v >= warn ? cWarn : cValue; };

    // Frames.
    double fps = w.frames / seconds;
    double pageRate = haveWC && state.composited ? dwc.paints / wcSeconds : w.uploads / seconds;
    std::string scroll = haveWC ? format("%3.0f/s", dwc.scrollFrames / wcSeconds) : std::string("  -  ");
    lines.push_back(format("%cFPS %c%5.1f  %cpage %c%3.0f/s  %cscroll %c%s  %cmiss %c%u", cLabel, cValue, fps, cLabel, cValue, pageRate,
        cLabel, cValue, scroll.c_str(), cLabel, w.missed ? cWarn : cValue, w.missed));
    double intervalAvg = w.intervals ? w.intervalSum / w.intervals : 0;
    lines.push_back(format("%cframe %c%4.1f%c avg %c%4.1f%c max %cwork %c%4.1f%c/%c%4.1f", cLabel, cValue, intervalAvg, cLabel,
        ms(w.intervalMax, 25, 40), w.intervalMax, cLabel, cLabel, cValue, w.workSum / frames, cLabel, ms(w.workMax, 10, 14), w.workMax));
    lines.push_back(format("%c apply %c%.1f %cvideo %c%.1f %cscene %c%.1f %cdraw %c%.1f", cLabel, cValue, w.applySum / frames, cLabel, cValue,
        w.videoSum / frames, cLabel, cValue, w.sceneSum / frames, cLabel, cValue, w.drawSum / frames));
    lines.push_back(format("%c render %c%.1f%c/%c%.1f %cswap %c%.1f%c/%c%.1f %cGPU %cn/a", cLabel, cValue, w.renderSum / frames, cLabel, cValue,
        w.renderMax, cLabel, cValue, w.swapSum / frames, cLabel, cValue, w.swapMax, cLabel, cDim));

    // CPU.
    auto percent = [&](double a, double b) { return haveSample && a >= 0 && b >= 0 ? 100 * (a - b) / sampleSeconds : -1.0; };
    // The threads' own getrusage(RUSAGE_THREAD); the main thread's from its
    // timeout, else its time outside poll.
    double mainCPU = -1, renderCPU = -1;
    int64_t mainCPUUS = s_mainCPUUS, mainCPUAt = s_mainCPUAt;
    if (mainCPUUS >= 0 && previousMainCPUUS >= 0 && mainCPUAt > previousMainCPUAt)
        mainCPU = 100.0 * (mainCPUUS - previousMainCPUUS) / (mainCPUAt - previousMainCPUAt);
    else if (haveSample)
        mainCPU = 100 * (1 - (sample.mainPollUS - previous.mainPollUS) / 1e6 / sampleSeconds);
    if (mainCPUAt != previousMainCPUAt) {
        previousMainCPUUS = mainCPUUS;
        previousMainCPUAt = mainCPUAt;
    }
    double renderSelf = selfThreadCPUSeconds();
    if (renderSelf >= 0 && previousRenderCPU >= 0 && now > previousRenderAt)
        renderCPU = 100 * (renderSelf - previousRenderCPU) / ((now - previousRenderAt) / 1e6);
    previousRenderCPU = renderSelf;
    previousRenderAt = now;
    if (++rebuilds == 20) // once, ~10 s in: which clocks work here
        g_message("[wpe-ps5] [perf] CPU so far: render thread %.3f s (rusage), %.3f s (thread clock); main thread %.3f s (rusage), %.3f s (thread clock); process %.3f s",
            renderSelf, sample.renderCPU, mainCPUUS / 1e6, sample.mainCPU, sample.processCPU);
    double titleCPU = percent(sample.processCPU, previous.processCPU) / 100;
    lines.push_back(format("%cCPU main %c%4.1f%% %crender %c%4.1f%% %ctitle %c%.2f", cLabel, ms(mainCPU, 60, 90), std::max(0.0, mainCPU),
        cLabel, ms(renderCPU, 60, 90), std::max(0.0, renderCPU), cLabel, cValue, std::max(0.0, titleCPU)));
    if (web || net)
        lines.push_back(format("%c    web %c%.2f%s %cnet %c%.2f %ccores", cLabel, ms(webCPU, 0.8, 0.98), webCPU, webCount > 1 ? "+" : "",
            cLabel, cValue, netCPU, cLabel));
    else
        lines.push_back(format("%c    web/net %c(no [perf] lines yet)", cLabel, cDim));

    // Memory.
    lines.push_back(format("%cMEM flex %c%.0f%c free  %cdirect %c%.0f%c MB", cLabel, ms(-sample.flexibleMB, -96, -48), sample.flexibleMB, cLabel,
        cLabel, cValue, sample.directMB, cLabel));
    {
        // Resident memory where the child can read it (kern.proc), else libpas only.
        auto child = [&](const char* name, const ChildStats* c) {
            if (!c)
                return format("%c%s %c-  ", cLabel, name, cDim);
            if (c->rssMB >= 0)
                return format("%c%s rss %c%.0f%c pas %c%.0f  ", cLabel, name, cValue, c->rssMB, cLabel, cValue, c->pasMB);
            return format("%c%s pas %c%.0f  ", cLabel, name, cValue, c->pasMB);
        };
        lines.push_back(format("%c    ", cLabel) + child("web", web) + child("net", net) + format("%cMB", cLabel));
    }

    // Compositing.
    if (haveWC) {
        lines.push_back(format("%cWC  %c%s %c%u%c pg %c%u%c layers %cGL %c%.0f%c MB%s", cLabel, state.composited ? cGood : cWarn,
            state.composited ? "on" : "idle", cValue, wc.pages, cLabel, cValue, wc.layers, cLabel, cLabel, wc.coverageReduced ? cWarn : cValue,
            wc.glTextureMB, cLabel, wc.coverageReduced ? " CAP" : ""));
        lines.push_back(format("%c    tiles %c%.0f/s %.1f MB/s %cpaint %c%.1f%c ms", cLabel, cValue, dwc.tileUploads / wcSeconds,
            dwc.tileUploadBytes / 1048576.0 / wcSeconds, cLabel, cValue, dwc.paints ? dwc.paintUS / 1000.0 / dwc.paints : 0.0, cLabel));
    } else {
        lines.push_back(format("%cWC  %c%s", cLabel, cDim, state.compositorBuild ? "off: page on the CPU path" : "off (CPU build)"));
        lines.push_back(format("%cPAGE %c%.0f%c/s copy %c%.1f%c ms %c%dx%d", cLabel, cValue, w.uploads / seconds, cLabel, cValue,
            w.uploads ? w.uploadSum / w.uploads : 0.0, cLabel, cValue, state.pageWidth, state.pageHeight));
    }
    {
        std::string line = format("%cWP  ", cLabel);
        if (haveWC)
            line += format("%c%.0f%c upd/s %capply %c%.2f ", cValue, dwc.updates / wcSeconds, cLabel, cLabel, cValue,
                dwc.updates ? dwc.updateApplyUS / 1000.0 / dwc.updates : 0.0);
        if (render)
            line += render->updateP95MS >= 0
                ? format("%cupd %c%.1f%c/%c%.0f%c ms %c%.0f/s", cLabel, ms(render->updateMS, 12, 16.7), render->updateMS, cLabel, cValue,
                    render->updateP95MS, cLabel, cValue, render->updateRate)
                : format("%cupd %c%.1f%c ms %c%.0f/s", cLabel, ms(render->updateMS, 12, 16.7), render->updateMS, cLabel, cValue, render->updateRate);
        else if (!haveWC)
            line += format("%cno updates", cDim);
        lines.push_back(line);
    }
    if (haveWC) {
        double latency = dwc.scrollLatencies ? dwc.scrollLatencyUS / 1000.0 / dwc.scrollLatencies : 0;
        lines.push_back(format("%cSCRL pad>frame %c%.0f%c avg %c%.0f%c max %cclamp %c%u", cLabel, ms(latency, 40, 70), latency, cLabel,
            ms(wc.scrollLatencyMaxMS, 50, 90), wc.scrollLatencyMaxMS, cLabel, cLabel, dwc.clampedEvents ? cWarn : cValue,
            static_cast<unsigned>(dwc.clampedEvents)));
    } else
        lines.push_back(format("%cSCRL %cn/a (no async scrolling)", cLabel, cDim));

    // Video.
    {
        if (!previousVideoAt) {
            previousVideoFrames = state.videoFramesConverted;
            previousVideoAt = now;
        }
        double videoSeconds = std::max(0.001, (now - previousVideoAt) / 1e6);
        double videoRate = (state.videoFramesConverted - previousVideoFrames) / videoSeconds;
        previousVideoFrames = state.videoFramesConverted;
        previousVideoAt = now;
        const auto& b = sample.budget;
        lines.push_back(format("%cVID %c%u%c/%c%u%c dec %c%.0f%c/%c%.0f%c MB %c%.0f%c fr/s", cLabel, b.decoders >= b.maxDecoders && b.maxDecoders ? cWarn : cValue,
            b.decoders, cLabel, cValue, b.maxDecoders, cLabel, cValue, b.directMB, cLabel, cValue, b.maxDirectMB, cLabel, cValue, videoRate, cLabel));
    }

    // Network: the title's own TCP counters, else the NetworkProcess's
    // (once a second: a rate over its own timestamps, kept between lines).
    if (haveSample && sample.tcp && previous.tcp && sample.tcpIn >= previous.tcpIn && sample.tcpOut >= previous.tcpOut) {
        netInRate = (sample.tcpIn - previous.tcpIn) / sampleSeconds;
        netOutRate = (sample.tcpOut - previous.tcpOut) / sampleSeconds;
        netRates = true;
    } else if (!sample.tcp && net && net->tcp && net->perfAt != netAt) {
        if (netAt && net->tcpIn >= netIn && net->tcpOut >= netOut && net->perfAt > netAt) {
            netInRate = (net->tcpIn - netIn) / ((net->perfAt - netAt) / 1e6);
            netOutRate = (net->tcpOut - netOut) / ((net->perfAt - netAt) / 1e6);
            netRates = true;
        }
        netIn = net->tcpIn;
        netOut = net->tcpOut;
        netAt = net->perfAt;
    }
    if (netRates)
        lines.push_back(format("%cNET in %c%.2f %cout %c%.2f %cMB/s%s", cLabel, cValue, netInRate / 1048576.0, cLabel, cValue,
            netOutRate / 1048576.0, cLabel, net && net->tcpInterfaces ? "" : " (TCP)"));
    else
        lines.push_back(format("%cNET %c%s", cLabel, cDim, sample.at && !sample.tcp && !(net && net->tcp) ? "n/a (no TCP counters)" : "..."));

    // Named counters (e.g. the ad blocker's "blocked").
    {
        Counter counters[kCounters];
        g_mutex_lock(&s_counterLock);
        memcpy(counters, s_counters, sizeof counters);
        g_mutex_unlock(&s_counterLock);
        double counterSeconds = previousCountersAt ? std::max(0.001, (now - previousCountersAt) / 1e6) : 0;
        std::string line;
        for (unsigned i = 0; i < kCounters; i++) {
            if (!counters[i].name[0])
                continue;
            bool known = counterSeconds > 0 && !strcmp(previousCounterNames[i], counters[i].name);
            double rate = known ? (counters[i].value - previousCounters[i]) / counterSeconds : 0;
            line += format("%c%s %c%llu%c (+%.0f/s) ", cLabel, counters[i].name, cValue, static_cast<unsigned long long>(counters[i].value), cLabel, rate);
            previousCounters[i] = counters[i].value;
            memcpy(previousCounterNames[i], counters[i].name, sizeof previousCounterNames[i]);
        }
        previousCountersAt = now;
        if (!line.empty())
            lines.push_back(line);
    }

    // The overlay's own cost (render thread CPU; its GPU work is a few hundred quads).
    lines.push_back(format("%cOVL %c%.3f%c ms/frame, max %c%.2f%c, text %c%.2f%c ms", cLabel, cValue, w.ovlFrames ? w.ovlSum / w.ovlFrames : 0.0,
        cLabel, cValue, w.ovlMax, cLabel, cValue, rebuildMS, cLabel));

    // Layout: the panel in its corner, inside the TV's safe area.
    const double scale = height / 1080.0;
    unsigned widest = 0;
    for (const auto& line : lines)
        widest = std::max(widest, visibleLength(line));
    widestLine = std::max(widestLine, static_cast<int>(widest)); // grows only: no jitter
    const int padX = static_cast<int>(std::lround(14 * scale)), padY = static_cast<int>(std::lround(10 * scale));
    panelW = widestLine * advance + 2 * padX;
    graphH = graph ? static_cast<int>(std::lround(lineHeight * 2.6)) : 0;
    const int graphGap = graph ? padY : 0;
    panelH = static_cast<int>(lines.size()) * lineHeight + 2 * padY + graphH + graphGap;
    const int marginX = static_cast<int>(std::lround(width * 0.025)), marginY = static_cast<int>(std::lround(height * 0.03));
    panelX = corner & 1 ? width - marginX - panelW : marginX;
    panelY = corner & 2 ? height - marginY - panelH : marginY;
    graphX = panelX + padX;
    graphW = panelW - 2 * padX;
    graphY = panelY + padY + static_cast<int>(lines.size()) * lineHeight + graphGap;

    textVertices.clear();
    addSolid(textVertices, panelX, panelY, panelW, panelH, rgba(8, 10, 14, 0.84));
    for (size_t i = 0; i < lines.size(); i++) {
        int x = panelX + padX, y = panelY + padY + static_cast<int>(i) * lineHeight;
        uint32_t color = kTextColors[cValue];
        for (char c : lines[i]) {
            unsigned char u = static_cast<unsigned char>(c);
            if (u < 32) {
                if (u < sizeof kTextColors / sizeof kTextColors[0])
                    color = kTextColors[u];
                continue;
            }
            if (u > 32 && u < 127) {
                int index = u - 32, cellX = (index % 16) * cellW, cellY = (index / 16) * cellH;
                addQuad(textVertices, x - pad, y - pad, cellW, cellH, static_cast<float>(cellX) / atlasW, static_cast<float>(cellY) / atlasH,
                    static_cast<float>(cellX + cellW) / atlasW, static_cast<float>(cellY + cellH) / atlasH, color);
            }
            x += advance;
        }
    }
    if (textVertices.size() > capacity / 2)
        textVertices.resize(capacity / 2 / 6 * 6);
    textVersion++;
    lastW = width;
    lastH = height;

    // Next window.
    if (sample.at)
        previous = sample;
    if (haveWC) {
        previousWC = wc;
        previousWCAt = now;
        havePreviousWC = true;
    }
    win = { };
    win.start = now;
}

// The frame-time graph, newest frame on the right: bars of the interval
// between presented frames (green within a vsync and a quarter, yellow
// within two and a quarter, red above; grey ticks for idle gaps) with the
// render thread's work for that frame before its draws (apply, video,
// scene) in blue inside; lines at 16.7 and 33.3 ms, the top is 50 ms.
size_t PS5PerfOverlay::Impl::buildGraph(Vertex* out, size_t room)
{
    if (!graph || graphW <= 0 || graphH <= 0)
        return 0;
    auto& vertices = graphVertices;
    vertices.clear();
    addSolid(vertices, graphX, graphY, graphW, graphH, rgba(20, 24, 30, 0.75));
    const float full = 50.0f;
    const int barW = std::max(2, static_cast<int>(std::lround(3 * graphH / 72.0)));
    const unsigned bars = std::min<unsigned>(ringCount, static_cast<unsigned>(graphW / barW));
    for (unsigned i = 0; i < bars; i++) {
        const auto& s = ring[(ringHead + Impl::kRing - 1 - i) % Impl::kRing];
        float x = graphX + graphW - static_cast<float>(i + 1) * barW;
        if (s.intervalMS <= 0 || s.intervalMS >= 100) {
            float h = graphH * 0.12f;
            addSolid(vertices, x, graphY + graphH - h, barW - 1, h, rgba(140, 140, 140, 0.8));
            continue;
        }
        float h = std::min(1.0f, s.intervalMS / full) * graphH;
        uint32_t color = s.intervalMS <= 20.8f ? rgba(90, 210, 100, 0.9) : s.intervalMS <= 37.5f ? rgba(250, 200, 60, 0.95) : rgba(250, 80, 70, 0.95);
        addSolid(vertices, x, graphY + graphH - h, barW - 1, h, color);
        float r = std::min(1.0f, s.workMS / full) * graphH;
        addSolid(vertices, x, graphY + graphH - r, barW - 1, r, rgba(80, 150, 255, 0.95));
    }
    for (float line : { 16.7f, 33.3f })
        addSolid(vertices, graphX, graphY + graphH - std::lround(line / full * graphH), graphW, 1, rgba(255, 255, 255, 0.45));
    size_t count = std::min(vertices.size(), room / 6 * 6);
    memcpy(out, vertices.data(), count * sizeof(Vertex));
    return count;
}

void PS5PerfOverlay::draw(int width, int height, const PS5PerfState& state)
{
    const int64_t start = nowUS();
    Impl& d = *m_impl;
    if (!d.ensureGL(height))
        return;
    if (start >= m_nextUpdateUS || width != d.lastW || height != d.lastH) {
        d.rebuild(start, width, height, state);
        m_nextUpdateUS = start + 500000;
        d.rebuildMS = (nowUS() - start) / 1000.0;
    }

    d.half ^= 1;
    Vertex* base = d.map ? d.map + d.half * d.capacity : d.staging.data();
    if (!d.map || d.halfTextVersion[d.half] != d.textVersion) {
        memcpy(base, d.textVertices.data(), d.textVertices.size() * sizeof(Vertex));
        d.halfTextVersion[d.half] = d.textVersion;
    }
    size_t count = d.textVertices.size();
    count += d.buildGraph(base + count, d.capacity - count);
    if (!d.map) {
        glBindBuffer(GL_ARRAY_BUFFER, d.vbo);
        glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(d.half * d.capacity * sizeof(Vertex)), static_cast<GLsizeiptr>(count * sizeof(Vertex)), base);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    glUseProgram(d.program);
    glUniform2f(d.uScreen, static_cast<float>(width), static_cast<float>(height));
    glUniform1i(d.uAtlas, 0);
    glBindVertexArray(d.vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d.atlas);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, static_cast<GLint>(d.half * d.capacity), static_cast<GLsizei>(count));
    glDisable(GL_BLEND);
    glBindVertexArray(state.vao);
    glUseProgram(0);

    double ms = (nowUS() - start) / 1000.0;
    d.win.ovlFrames++;
    d.win.ovlSum += ms;
    d.win.ovlMax = std::max(d.win.ovlMax, ms);
}
