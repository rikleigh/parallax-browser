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

#pragma once

#include <stdint.h>

// Presents CPU-painted page buffers on the TV through ps5-opengl: one EGL
// window surface (a second one fails with EGL_BAD_ALLOC on 13.60, Spike 9),
// one desktop GL 4.6 core context on the UI main thread. The page is a BGRA
// texture updated from the WebProcess's shared-memory buffer (damaged rects
// only); the pad pointer is drawn on top.
class PS5Renderer {
public:
    static PS5Renderer& singleton();

    bool initialize(char* error, unsigned errorSize);
    bool isInitialized() const { return m_initialized; }

    void* eglDisplay() const { return m_eglDisplay; }
    int width() const { return m_width; }
    int height() const { return m_height; }

    struct Rect { int x, y, width, height; };
    // Copies the damaged parts of a premultiplied BGRA buffer into the page
    // texture. No damage rects means the whole buffer.
    void uploadPage(const uint8_t* data, int width, int height, unsigned stride, const Rect* damage, unsigned damageCount);

    // Draws the page (letterboxed if its size differs from the surface), the
    // pointer if visible and an optional status bar, then swaps.
    void present(bool pointerVisible, double pointerX, double pointerY);

    bool hasPage() const { return m_pageWidth > 0; }

private:
    PS5Renderer() = default;
    bool createPrograms(char* error, unsigned errorSize);

    bool m_initialized { false };
    void* m_eglDisplay { nullptr };
    void* m_eglSurface { nullptr };
    void* m_eglContext { nullptr };
    int m_width { 0 };
    int m_height { 0 };

    unsigned m_program { 0 };
    int m_uRect { -1 };
    int m_uTexture { -1 };
    int m_uOpaque { -1 };
    unsigned m_vao { 0 };
    unsigned m_pageTexture { 0 };
    int m_pageWidth { 0 };
    int m_pageHeight { 0 };
    unsigned m_pointerTexture { 0 };
};
