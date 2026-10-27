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

#include <cmath>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

// GL is reached through libepoxy. In the title its dlopen()/dlsym() resolve
// to the statically linked ps5-opengl (port/webkit/apps/title/dlshim.c).

PS5Renderer& PS5Renderer::singleton()
{
    static PS5Renderer renderer;
    return renderer;
}

static const char* kVertexShader = R"(#version 330 core
uniform vec4 rect; // x0 y0 x1 y1 in NDC
out vec2 uv;
void main()
{
    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    uv = vec2(c.x, 1.0 - c.y);
    gl_Position = vec4(mix(rect.xy, rect.zw, c), 0.0, 1.0);
}
)";

static const char* kFragmentShader = R"(#version 330 core
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

static GLuint compile(GLenum type, const char* source, char* error, unsigned errorSize)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
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

bool PS5Renderer::createPrograms(char* error, unsigned errorSize)
{
    GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader, error, errorSize);
    GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, kFragmentShader, error, errorSize) : 0;
    if (!vs || !fs)
        return false;
    m_program = glCreateProgram();
    glAttachShader(m_program, vs);
    glAttachShader(m_program, fs);
    glLinkProgram(m_program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(m_program, errorSize, nullptr, error);
        return false;
    }
    m_uRect = glGetUniformLocation(m_program, "rect");
    m_uTexture = glGetUniformLocation(m_program, "tex");
    m_uOpaque = glGetUniformLocation(m_program, "opaque_");
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

bool PS5Renderer::initialize(char* error, unsigned errorSize)
{
    if (m_initialized)
        return true;

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
    eglQuerySurface(display, surface, EGL_WIDTH, &m_width);
    eglQuerySurface(display, surface, EGL_HEIGHT, &m_height);
    if (m_width <= 0 || m_height <= 0) {
        m_width = 1920;
        m_height = 1080;
    }
    eglSwapInterval(display, 1);

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
    m_initialized = true;

    // First frame: black, so the TV shows the title is alive.
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(display, surface);
    g_message("PS5Renderer: EGL %s, GL %s, %dx%d", eglQueryString(display, EGL_VERSION),
        reinterpret_cast<const char*>(glGetString(GL_VERSION)), m_width, m_height);
    return true;
}

void PS5Renderer::uploadPage(const uint8_t* data, int width, int height, unsigned stride, const Rect* damage, unsigned damageCount)
{
    if (!m_initialized || !data || width <= 0 || height <= 0)
        return;

    glBindTexture(GL_TEXTURE_2D, m_pageTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(stride / 4));
    if (width != m_pageWidth || height != m_pageHeight) {
        // New size: allocate and upload everything.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, data);
        m_pageWidth = width;
        m_pageHeight = height;
    } else if (!damageCount) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, data);
    } else {
        for (unsigned i = 0; i < damageCount; i++) {
            int x0 = std::max(0, damage[i].x), y0 = std::max(0, damage[i].y);
            int x1 = std::min(width, damage[i].x + damage[i].width);
            int y1 = std::min(height, damage[i].y + damage[i].height);
            if (x1 <= x0 || y1 <= y0)
                continue;
            glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, x1 - x0, y1 - y0, GL_BGRA, GL_UNSIGNED_BYTE,
                data + static_cast<size_t>(y0) * stride + static_cast<size_t>(x0) * 4);
        }
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void PS5Renderer::present(bool pointerVisible, double pointerX, double pointerY)
{
    if (!m_initialized)
        return;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_width, m_height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(m_program);
    glBindVertexArray(m_vao);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(m_uTexture, 0);

    auto toNDC = [&](double x, double y, double w, double h) {
        float x0 = static_cast<float>(x / m_width * 2.0 - 1.0);
        float x1 = static_cast<float>((x + w) / m_width * 2.0 - 1.0);
        float y0 = static_cast<float>(1.0 - (y + h) / m_height * 2.0);
        float y1 = static_cast<float>(1.0 - y / m_height * 2.0);
        glUniform4f(m_uRect, x0, y0, x1, y1);
    };

    if (m_pageWidth > 0) {
        // Scale to fit, centred (the view is normally the surface size).
        double scale = std::min(static_cast<double>(m_width) / m_pageWidth, static_cast<double>(m_height) / m_pageHeight);
        double w = m_pageWidth * scale, h = m_pageHeight * scale;
        glDisable(GL_BLEND);
        glBindTexture(GL_TEXTURE_2D, m_pageTexture);
        glUniform1i(m_uOpaque, 1);
        toNDC((m_width - w) / 2, (m_height - h) / 2, w, h);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    if (pointerVisible) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glBindTexture(GL_TEXTURE_2D, m_pointerTexture);
        glUniform1i(m_uOpaque, 0);
        toNDC(pointerX - pointerSize / 2.0, pointerY - pointerSize / 2.0, pointerSize, pointerSize);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisable(GL_BLEND);
    }

    eglSwapBuffers(m_eglDisplay, m_eglSurface);
}
