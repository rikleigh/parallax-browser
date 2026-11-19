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
#include "PS5GLBench.h"
#include "PS5GLES.h"

#include <algorithm>
#include <emmintrin.h>
#include <epoxy/gl.h>
#include <glib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Spike S1a: how fast 512x512 premultiplied BGRA tiles (as Skia paints them
// for TextureMapper) reach GL textures on ps5-opengl. Each method uploads a
// batch of N tiles per "frame", then draws them 1:1 into a 1080p scene
// target as a compositor would. Timed separately (wall clock; thread CPU
// clocks read 0 in a title): upload CPU (memcpy + GL calls), upload GPU wait
// (glFinish), draw CPU, draw GPU wait. Correctness: the first and last tile
// of a batch are drawn 1:1 (nearest) into a 512x512 target and read back
// whole against the source pixels.

extern "C" double ps5wk_title_direct_mb(void) __attribute__((weak));

namespace {

constexpr int kTile = 512;
constexpr size_t kTileBytes = static_cast<size_t>(kTile) * kTile * 4;
constexpr int kMaxTiles = 40;
constexpr int kSources = kMaxTiles + 1;
constexpr int kSceneWidth = 1920;
constexpr int kSceneHeight = 1080;

double nowMS()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

double directMB()
{
    return ps5wk_title_direct_mb ? ps5wk_title_direct_mb() : -1;
}

enum class Method { TexSubRGBA, TexSubBGRA, LinearRGBA, LinearBGRA, PBORGBA, PBOBGRA, SSBODraw, SSBODrawNT, TBO };

struct MethodInfo {
    Method method;
    const char* name;
    const char* what;
};

// Least risky first: a crash part way still leaves the earlier lines.
const MethodInfo kMethods[] = {
    { Method::TexSubRGBA, "texsub-rgba", "glTexSubImage2D GL_RGBA into a 2D RGBA8 texture (R/B texture swizzle)" },
    { Method::LinearRGBA, "linear-rgba", "glTexSubImage2D GL_RGBA into a 2-level glTexStorage2D RGBA8 texture (linear layout, R/B swizzle)" },
    { Method::LinearBGRA, "linear-bgra", "glTexSubImage2D GL_BGRA into a 2-level glTexStorage2D RGBA8 texture (linear layout)" },
    { Method::SSBODraw, "ssbo-draw", "memcpy into a persistent buffer, FBO draw (unpackUnorm4x8) into a 2D RGBA8 texture" },
    { Method::SSBODrawNT, "ssbo-draw-nt", "as ssbo-draw, memcpy with non-temporal stores" },
    { Method::TBO, "tbo", "memcpy into a persistent buffer sampled as a GL_RGBA8 buffer texture (texelFetch)" },
    { Method::TexSubBGRA, "texsub-bgra", "glTexSubImage2D GL_BGRA into a 2D RGBA8 texture" },
    { Method::PBORGBA, "pbo-rgba", "memcpy into a persistent GL_PIXEL_UNPACK_BUFFER, glTexSubImage2D GL_RGBA (R/B swizzle)" },
    { Method::PBOBGRA, "pbo-bgra", "memcpy into a persistent GL_PIXEL_UNPACK_BUFFER, glTexSubImage2D GL_BGRA" },
};

const char* kQuadVS = R"(#version 430 core
uniform vec4 rect; // x0 y0 x1 y1 in NDC
out vec2 uv;
void main()
{
    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    uv = c;
    gl_Position = vec4(mix(rect.xy, rect.zw, c), 0.0, 1.0);
}
)";

const char* kSampleFS = R"(#version 430 core
uniform sampler2D tex;
in vec2 uv;
out vec4 color;
void main()
{
    color = texture(tex, uv);
}
)";

const char* kBufferFS = R"(#version 430 core
uniform samplerBuffer tb;
in vec2 uv;
out vec4 color;
void main()
{
    ivec2 p = clamp(ivec2(uv * 512.0), ivec2(0), ivec2(511));
    color = texelFetch(tb, p.y * 512 + p.x).bgra;
}
)";

// Tile texture row y = source row y (as glTexSubImage2D puts it).
const char* kDecodeFS = R"(#version 430 core
layout(std430, binding = 1) readonly buffer Src { uint px[]; };
out vec4 color;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    color = unpackUnorm4x8(px[p.y * 512 + p.x]).zyxw;
}
)";

GLuint compileShader(GLenum type, const char* source)
{
    GLuint shader = glCreateShader(type);
    ps5ShaderSourceGL(shader, source);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = { };
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        g_message("[glbench] shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint linkProgram(const char* vs, const char* fs)
{
    GLuint v = compileShader(GL_VERTEX_SHADER, vs);
    GLuint f = v ? compileShader(GL_FRAGMENT_SHADER, fs) : 0;
    if (!v || !f) {
        if (v)
            glDeleteShader(v);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, v);
    glAttachShader(program, f);
    glLinkProgram(program);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = { };
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        g_message("[glbench] program link failed: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

// Premultiplied BGRA with every channel varying, so a swapped channel or a
// stale tile shows.
void fillSource(uint8_t* p, int index)
{
    uint32_t seed = 0x9e3779b9u * static_cast<uint32_t>(index + 1);
    for (int y = 0; y < kTile; y++) {
        for (int x = 0; x < kTile; x++) {
            uint32_t h = (static_cast<uint32_t>(x) * 0x85ebca6bu) ^ (static_cast<uint32_t>(y) * 0xc2b2ae35u) ^ seed;
            h ^= h >> 15;
            h *= 0x2c1b3c6du;
            h ^= h >> 12;
            uint32_t a = (h >> 24) | 0x10;
            uint8_t* px = p + (static_cast<size_t>(y) * kTile + x) * 4;
            px[0] = static_cast<uint8_t>((h & 0xff) % (a + 1));          // B
            px[1] = static_cast<uint8_t>(((h >> 8) & 0xff) % (a + 1));   // G
            px[2] = static_cast<uint8_t>(((h >> 16) & 0xff) % (a + 1));  // R
            px[3] = static_cast<uint8_t>(a);                             // A
        }
    }
}

void copyNT(uint8_t* dst, const uint8_t* src, size_t bytes)
{
    auto* d = reinterpret_cast<__m128i*>(dst);
    auto* s = reinterpret_cast<const __m128i*>(src);
    for (size_t i = 0; i < bytes / 64; i++) {
        __m128i a = _mm_loadu_si128(s + 0), b = _mm_loadu_si128(s + 1), c = _mm_loadu_si128(s + 2), e = _mm_loadu_si128(s + 3);
        _mm_stream_si128(d + 0, a);
        _mm_stream_si128(d + 1, b);
        _mm_stream_si128(d + 2, c);
        _mm_stream_si128(d + 3, e);
        d += 4;
        s += 4;
    }
    _mm_sfence();
}

void flushRange(const uint8_t* p, size_t bytes)
{
    for (size_t i = 0; i < bytes; i += 64)
        _mm_clflush(p + i);
    _mm_mfence();
}

struct Bench {
    int screenWidth { 0 }, screenHeight { 0 };
    uint8_t* sources[kSources] { };
    uint8_t* readback { nullptr };
    GLuint staging { 0 };
    uint8_t* stagingMap { nullptr };
    GLuint vao { 0 };
    GLuint sampleProgram { 0 }, bufferProgram { 0 }, decodeProgram { 0 };
    GLint sampleRect { -1 }, bufferRect { -1 }, decodeRect { -1 };
    GLuint sceneTexture { 0 }, sceneFBO { 0 };
    GLuint checkTexture { 0 }, checkFBO { 0 };
    GLuint nearestSampler { 0 };

    bool setUp();
    void tearDown();
    void runMethod(const MethodInfo&);
    void memcpyProbe();
    unsigned check(Method, GLuint texture, int source, char* firstError, size_t errorSize);
};

GLuint makeTarget(int width, int height, GLuint* fbo)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        g_message("[glbench] %dx%d target FBO incomplete (0x%x)", width, height, status);
    return texture;
}

bool Bench::setUp()
{
    double mb0 = directMB();
    for (int i = 0; i < kSources; i++) {
        sources[i] = static_cast<uint8_t*>(aligned_alloc(64, kTileBytes));
        if (!sources[i]) {
            g_message("[glbench] out of memory for the source tiles");
            return false;
        }
        fillSource(sources[i], i);
    }
    readback = static_cast<uint8_t*>(aligned_alloc(64, kTileBytes));
    if (!readback)
        return false;

    sampleProgram = linkProgram(kQuadVS, kSampleFS);
    bufferProgram = linkProgram(kQuadVS, kBufferFS);
    decodeProgram = linkProgram(kQuadVS, kDecodeFS);
    if (!sampleProgram || !bufferProgram || !decodeProgram)
        return false;
    sampleRect = glGetUniformLocation(sampleProgram, "rect");
    bufferRect = glGetUniformLocation(bufferProgram, "rect");
    decodeRect = glGetUniformLocation(decodeProgram, "rect");
    glUseProgram(sampleProgram);
    glUniform1i(glGetUniformLocation(sampleProgram, "tex"), 0);
    glUseProgram(bufferProgram);
    glUniform1i(glGetUniformLocation(bufferProgram, "tb"), 0);
    glUseProgram(0);

    glGenVertexArrays(1, &vao);
    glGenSamplers(1, &nearestSampler);
    glSamplerParameteri(nearestSampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(nearestSampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glSamplerParameteri(nearestSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(nearestSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    sceneTexture = makeTarget(kSceneWidth, kSceneHeight, &sceneFBO);
    checkTexture = makeTarget(kTile, kTile, &checkFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // One persistent, coherent buffer: the PBO, SSBO and buffer-texture
    // staging for all methods (tile t at t * 1 MiB).
    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    glGenBuffers(1, &staging);
    glBindBuffer(GL_COPY_WRITE_BUFFER, staging);
    glBufferStorage(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(kMaxTiles * kTileBytes), nullptr, flags);
    stagingMap = static_cast<uint8_t*>(glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, static_cast<GLsizeiptr>(kMaxTiles * kTileBytes), flags));
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    if (!stagingMap) {
        g_message("[glbench] staging buffer map failed (0x%x)", glGetError());
        return false;
    }
    glFinish();
    GLenum error = glGetError();
    g_message("[glbench] set up: %d source tiles (%.0f MB malloc), staging buffer %.0f MB at %p, scene %dx%d, GL error 0x%x; direct memory %.1f -> %.1f MB",
        kSources, kSources * kTileBytes / 1048576.0, kMaxTiles * kTileBytes / 1048576.0, stagingMap, kSceneWidth, kSceneHeight, error, mb0, directMB());
    return error == GL_NO_ERROR;
}

void Bench::tearDown()
{
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindSampler(0, 0);
    glUseProgram(0);
    glBindVertexArray(0);
    if (staging) {
        glBindBuffer(GL_COPY_WRITE_BUFFER, staging);
        glUnmapBuffer(GL_COPY_WRITE_BUFFER);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        glDeleteBuffers(1, &staging);
    }
    GLuint textures[] = { sceneTexture, checkTexture };
    glDeleteTextures(2, textures);
    GLuint fbos[] = { sceneFBO, checkFBO };
    glDeleteFramebuffers(2, fbos);
    glDeleteSamplers(1, &nearestSampler);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(sampleProgram);
    glDeleteProgram(bufferProgram);
    glDeleteProgram(decodeProgram);
    for (auto*& source : sources) {
        free(source);
        source = nullptr;
    }
    free(readback);
    readback = nullptr;
    glFinish();
}

// Memory copies alone, 1 MiB (one tile), best of 8.
void Bench::memcpyProbe()
{
    uint8_t* heap = static_cast<uint8_t*>(aligned_alloc(64, kTileBytes));
    if (!heap)
        return;
    double best[6] = { 1e9, 1e9, 1e9, 1e9, 1e9, 1e9 };
    for (int round = 0; round < 8; round++) {
        const uint8_t* src = sources[round % kSources];
        double t0 = nowMS();
        memcpy(heap, src, kTileBytes);
        asm volatile("" : : "r"(heap) : "memory"); // keep the copy
        double t1 = nowMS();
        memcpy(stagingMap, src, kTileBytes);
        double t2 = nowMS();
        flushRange(stagingMap, kTileBytes); // dirty lines
        double t3 = nowMS();
        copyNT(stagingMap + kTileBytes, src, kTileBytes);
        double t4 = nowMS();
        flushRange(stagingMap + kTileBytes, kTileBytes); // after non-temporal stores
        double t5 = nowMS();
        flushRange(stagingMap + kTileBytes, kTileBytes); // already clean
        double t6 = nowMS();
        double d[6] = { t1 - t0, t2 - t1, t3 - t2, t4 - t3, t5 - t4, t6 - t5 };
        for (int i = 0; i < 6; i++)
            best[i] = std::min(best[i], d[i]);
    }
    free(heap);
    g_message("[glbench] 1 MiB: memcpy to malloc %.3f ms, memcpy to GL buffer %.3f ms, clflush dirty %.3f ms, non-temporal copy to GL buffer %.3f ms, "
              "clflush after it %.3f ms, clflush clean %.3f ms (best of 8)", best[0], best[1], best[2], best[3], best[4], best[5]);
}

void tileRect(int index, int width, int height, float out[4])
{
    // 1:1 512x512 quads across the scene, overlapping (4 x 2 grid).
    int x = (index % 4) * ((width - kTile) / 3);
    int y = ((index / 4) % 2) * (height - kTile);
    out[0] = x * 2.0f / width - 1.0f;
    out[1] = y * 2.0f / height - 1.0f;
    out[2] = (x + kTile) * 2.0f / width - 1.0f;
    out[3] = (y + kTile) * 2.0f / height - 1.0f;
}

unsigned Bench::check(Method method, GLuint texture, int source, char* firstError, size_t errorSize)
{
    glBindFramebuffer(GL_FRAMEBUFFER, checkFBO);
    glViewport(0, 0, kTile, kTile);
    glActiveTexture(GL_TEXTURE0);
    if (method == Method::TBO) {
        glUseProgram(bufferProgram);
        glUniform4f(bufferRect, -1, -1, 1, 1);
        glBindTexture(GL_TEXTURE_BUFFER, texture);
    } else {
        glUseProgram(sampleProgram);
        glUniform4f(sampleRect, -1, -1, 1, 1);
        glBindTexture(GL_TEXTURE_2D, texture);
        glBindSampler(0, nearestSampler);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindSampler(0, 0);
    memset(readback, 0, kTileBytes);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, kTile, kTile, GL_RGBA, GL_UNSIGNED_BYTE, readback);
    unsigned bad = 0;
    const uint8_t* expected = sources[source];
    for (size_t i = 0; i < static_cast<size_t>(kTile) * kTile; i++) {
        const uint8_t* e = expected + i * 4;
        const uint8_t* r = readback + i * 4;
        if (r[0] != e[2] || r[1] != e[1] || r[2] != e[0] || r[3] != e[3]) {
            if (!bad && firstError)
                snprintf(firstError, errorSize, "(%zu,%zu) got RGBA %02x%02x%02x%02x want %02x%02x%02x%02x", i % kTile, i / kTile,
                    r[0], r[1], r[2], r[3], e[2], e[1], e[0], e[3]);
            bad++;
        }
    }
    return bad;
}

void Bench::runMethod(const MethodInfo& info)
{
    const Method method = info.method;
    const bool linear = method == Method::LinearRGBA || method == Method::LinearBGRA;
    const bool rgbaUpload = method == Method::TexSubRGBA || method == Method::LinearRGBA || method == Method::PBORGBA;
    const bool viaPBO = method == Method::PBORGBA || method == Method::PBOBGRA;
    const bool viaDraw = method == Method::SSBODraw || method == Method::SSBODrawNT;
    const bool tbo = method == Method::TBO;
    const GLenum uploadFormat = rgbaUpload ? GL_RGBA : GL_BGRA;

    glFinish();
    while (glGetError() != GL_NO_ERROR) { }
    double mbBefore = directMB();
    double setupStart = nowMS();
    GLuint textures[kMaxTiles] = { };
    GLuint fbos[kMaxTiles] = { };
    glGenTextures(kMaxTiles, textures);
    for (int t = 0; t < kMaxTiles; t++) {
        if (tbo) {
            glBindTexture(GL_TEXTURE_BUFFER, textures[t]);
            glTexBufferRange(GL_TEXTURE_BUFFER, GL_RGBA8, staging, static_cast<GLintptr>(t * kTileBytes), static_cast<GLsizeiptr>(kTileBytes));
            continue;
        }
        glBindTexture(GL_TEXTURE_2D, textures[t]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (linear) {
            // A mip chain makes ps5-opengl keep the texture linear
            // (ps5_linear_sampled_layout); level 1 is never used.
            glTexStorage2D(GL_TEXTURE_2D, 2, GL_RGBA8, kTile, kTile);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        } else
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kTile, kTile, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        if (rgbaUpload) {
            // The bytes are BGRA: let the sampler swap R and B.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        }
        if (viaDraw) {
            glGenFramebuffers(1, &fbos[t]);
            glBindFramebuffer(GL_FRAMEBUFFER, fbos[t]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[t], 0);
            if (!t) {
                GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                if (status != GL_FRAMEBUFFER_COMPLETE)
                    g_message("[glbench] %s: tile FBO incomplete (0x%x)", info.name, status);
            }
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFinish();
    double setupMS = nowMS() - setupStart;
    double mbAfterSetup = directMB();
    GLenum setupError = glGetError();
    g_message("[glbench] %s: %s; set up %d tiles in %.1f ms, direct memory +%.1f MB (%.2f MB per tile), GL error 0x%x", info.name, info.what,
        kMaxTiles, setupMS, mbAfterSetup - mbBefore, (mbAfterSetup - mbBefore) / kMaxTiles, setupError);

    glBindVertexArray(vao);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    const int batches[] = { 1, 4, 12, 40 };
    const int framesFor[] = { 16, 8, 5, 4 };
    const int methodSalt = static_cast<int>(method);
    int frameCounter = 0;
    for (int b = 0; b < 4; b++) {
        const int n = batches[b];
        const int frames = framesFor[b];
        double upCPU = 0, upGPU = 0, drawCPU = 0, drawGPU = 0, upFrameMax = 0, upTileMax = 0;
        unsigned checked = 0, bad = 0;
        char firstError[160] = { };
        for (int f = -1; f < frames; f++) { // f == -1: warm-up, not counted
            frameCounter++;
            auto sourceFor = [&](int t) { return (t + frameCounter * 7 + methodSalt) % kSources; };

            double t0 = nowMS();
            if (viaPBO)
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, staging);
            if (viaDraw) {
                glUseProgram(decodeProgram);
                glUniform4f(decodeRect, -1, -1, 1, 1);
                glViewport(0, 0, kTile, kTile);
            }
            for (int t = 0; t < n; t++) {
                double tileStart = nowMS();
                const uint8_t* src = sources[sourceFor(t)];
                uint8_t* slot = stagingMap + t * kTileBytes;
                switch (method) {
                case Method::TexSubRGBA:
                case Method::TexSubBGRA:
                case Method::LinearRGBA:
                case Method::LinearBGRA:
                    glBindTexture(GL_TEXTURE_2D, textures[t]);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kTile, kTile, uploadFormat, GL_UNSIGNED_BYTE, src);
                    break;
                case Method::PBORGBA:
                case Method::PBOBGRA:
                    memcpy(slot, src, kTileBytes);
                    glBindTexture(GL_TEXTURE_2D, textures[t]);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kTile, kTile, uploadFormat, GL_UNSIGNED_BYTE,
                        reinterpret_cast<const void*>(static_cast<uintptr_t>(t * kTileBytes)));
                    break;
                case Method::SSBODraw:
                case Method::SSBODrawNT:
                    if (method == Method::SSBODrawNT)
                        copyNT(slot, src, kTileBytes);
                    else
                        memcpy(slot, src, kTileBytes);
                    glBindFramebuffer(GL_FRAMEBUFFER, fbos[t]);
                    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, staging, static_cast<GLintptr>(t * kTileBytes), static_cast<GLsizeiptr>(kTileBytes));
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    break;
                case Method::TBO:
                    memcpy(slot, src, kTileBytes);
                    break;
                }
                if (f >= 0)
                    upTileMax = std::max(upTileMax, nowMS() - tileStart);
            }
            if (viaPBO)
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            if (viaDraw)
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
            double t1 = nowMS();
            glFinish();
            double t2 = nowMS();

            // Composite: every tile 1:1 into the scene target.
            glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO);
            glViewport(0, 0, kSceneWidth, kSceneHeight);
            glActiveTexture(GL_TEXTURE0);
            glUseProgram(tbo ? bufferProgram : sampleProgram);
            for (int t = 0; t < n; t++) {
                float r[4];
                tileRect(t, kSceneWidth, kSceneHeight, r);
                glUniform4f(tbo ? bufferRect : sampleRect, r[0], r[1], r[2], r[3]);
                glBindTexture(tbo ? GL_TEXTURE_BUFFER : GL_TEXTURE_2D, textures[t]);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
            double t3 = nowMS();
            glFinish();
            double t4 = nowMS();

            if (f >= 0) {
                upCPU += t1 - t0;
                upGPU += t2 - t1;
                drawCPU += t3 - t2;
                drawGPU += t4 - t3;
                upFrameMax = std::max(upFrameMax, t1 - t0);
            }
            if (f == frames - 1) {
                int toCheck[2] = { 0, n - 1 };
                for (int k = 0; k < (n > 1 ? 2 : 1); k++) {
                    int t = toCheck[k];
                    unsigned wrong = check(method, textures[t], sourceFor(t), firstError[0] ? nullptr : firstError, sizeof(firstError));
                    checked++;
                    if (wrong)
                        bad++;
                }
            }
        }
        GLenum error = glGetError();
        const double tiles = static_cast<double>(n) * frames;
        g_message("[glbench] %s n=%d x%d frames: upload cpu %.3f ms/tile (frame max %.2f ms, tile max %.2f ms), upload gpu wait %.3f ms/tile, "
                  "draw cpu %.3f ms/tile, draw gpu wait %.3f ms/tile; check %u/%u ok%s%s; GL error 0x%x",
            info.name, n, frames, upCPU / tiles, upFrameMax, upTileMax, upGPU / tiles, drawCPU / tiles, drawGPU / tiles, checked - bad, checked,
            bad ? ", first bad pixel " : "", bad ? firstError : "", error);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
    glUseProgram(0);
    glDeleteTextures(kMaxTiles, textures);
    if (viaDraw)
        glDeleteFramebuffers(kMaxTiles, fbos);
    glFinish();
    g_message("[glbench] %s: done, direct memory %.1f MB after delete (%.1f before set up)", info.name, directMB(), mbBefore);
}

bool selected(const char* list, const char* name)
{
    if (!list || !*list)
        return true;
    size_t length = strlen(name);
    for (const char* p = list; (p = strstr(p, name)); p += length) {
        bool startOK = p == list || strchr(" \t\r\n,", p[-1]);
        bool endOK = !p[length] || strchr(" \t\r\n,", p[length]);
        if (startOK && endOK)
            return true;
    }
    return false;
}

} // namespace

void ps5GLBenchMaybeRun(int screenWidth, int screenHeight)
{
    gchar* list = nullptr;
    if (!g_file_get_contents("/data/ps5webkit/glbench", &list, nullptr, nullptr))
        return;
    g_strstrip(list);
    GLint vao = 0, fbo = 0, program = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    g_message("[glbench] start (%s), GL %s, renderer %s, %dx%d screen", *list ? list : "all methods",
        reinterpret_cast<const char*>(glGetString(GL_VERSION)), reinterpret_cast<const char*>(glGetString(GL_RENDERER)), screenWidth, screenHeight);
    double start = nowMS();
    Bench bench;
    bench.screenWidth = screenWidth;
    bench.screenHeight = screenHeight;
    if (bench.setUp()) {
        bench.memcpyProbe();
        for (const auto& info : kMethods) {
            if (selected(list, info.name))
                bench.runMethod(info);
        }
    }
    bench.tearDown();
    glBindVertexArray(vao);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glUseProgram(program);
    glViewport(0, 0, screenWidth, screenHeight);
    while (glGetError() != GL_NO_ERROR) { }
    g_message("[glbench] end, %.1f s, direct memory %.1f MB", (nowMS() - start) / 1000.0, directMB());
    g_free(list);
}
