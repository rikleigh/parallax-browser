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

// The application's own UI images (the browser's address bar and menu):
// rectangles and UTF-8 text rasterized on the CPU into premultiplied BGRA,
// shown over the page with wpe_display_ps5_set_ui_layer(). FreeType with
// the Noto fonts the browser uses (/data/ps5webkit/fonts): Noto Sans
// Regular/Bold, then the symbol, math, CJK and script fonts for characters
// it lacks (no shaping: one glyph per character, left to right). Rendered
// glyphs are cached. Main thread (its own FreeType library: PS5Text's is
// the render thread's).

#include "config.h"
#include "WPEDisplayPS5.h"

#include "PS5Renderer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <glib.h>
#include <string>
#include <unordered_map>
#include <vector>

struct _WPEDisplayPS5Canvas {
    int width { 0 };
    int height { 0 };
    std::vector<uint32_t> px; // premultiplied, 0xAARRGGBB (bytes B G R A)
};

namespace {

// The fonts' folder (uipreview, the Mac harness, builds with its own).
#ifndef PS5WK_FONT_DIR
#define PS5WK_FONT_DIR "/data/ps5webkit/fonts"
#endif

static const char* const kRegular = PS5WK_FONT_DIR "/NotoSans-Regular.ttf";
static const char* const kBold = PS5WK_FONT_DIR "/NotoSans-Bold.ttf";
// Tried in order for a character Noto Sans lacks; opened on first need.
static const char* const kFallbacks[] = {
    PS5WK_FONT_DIR "/NotoSansSymbols2-Regular.ttf",
    PS5WK_FONT_DIR "/NotoSansSymbols-Regular.ttf",
    PS5WK_FONT_DIR "/NotoSansMath-Regular.ttf",
    PS5WK_FONT_DIR "/NotoSansCJK-Regular.ttc",
    PS5WK_FONT_DIR "/NotoSansArabic-Regular.ttf",
    PS5WK_FONT_DIR "/NotoSansHebrew-Regular.ttf",
    PS5WK_FONT_DIR "/NotoSansDevanagari-Regular.ttf",
    PS5WK_FONT_DIR "/NotoSansThai-Regular.ttf",
    PS5WK_FONT_DIR "/NotoSansBengali-Regular.ttf",
    PS5WK_FONT_DIR "/NotoSansTamil-Regular.ttf",
    PS5WK_FONT_DIR "/NotoSansArmenian-Regular.ttf",
    PS5WK_FONT_DIR "/NotoSansGeorgian-Regular.ttf",
    PS5WK_FONT_DIR "/NotoSansEthiopic-Regular.ttf",
};
static constexpr unsigned kFallbackCount = sizeof(kFallbacks) / sizeof(kFallbacks[0]);
static constexpr unsigned kFaceCount = 2 + kFallbackCount; // 0 regular, 1 bold, then the fallbacks

struct Fonts {
    FT_Library library { nullptr };
    FT_Face faces[kFaceCount] { };
    bool tried[kFaceCount] { };
    bool initialized { false };
    bool ok { false };
};
static Fonts s_fonts;

static FT_Face face(unsigned index)
{
    if (index >= kFaceCount || !s_fonts.library)
        return nullptr;
    if (!s_fonts.tried[index]) {
        s_fonts.tried[index] = true;
        const char* path = index == 0 ? kRegular : index == 1 ? kBold : kFallbacks[index - 2];
        if (FT_New_Face(s_fonts.library, path, 0, &s_fonts.faces[index]))
            s_fonts.faces[index] = nullptr;
    }
    return s_fonts.faces[index];
}

static bool fontsReady()
{
    if (!s_fonts.initialized) {
        s_fonts.initialized = true;
        if (!FT_Init_FreeType(&s_fonts.library))
            s_fonts.ok = face(0) || face(1);
        g_message("[wpe-ps5] ui canvas: %s", s_fonts.ok ? "fonts loaded" : "Noto Sans missing: no text");
    }
    return s_fonts.ok;
}

// The face (index) that has c: the requested weight first, then the other
// weight, then the fallbacks; 0 glyph = the first face's missing glyph.
static unsigned faceFor(gunichar c, bool bold, FT_UInt* glyph)
{
    unsigned first = bold ? 1 : 0;
    unsigned order[kFaceCount];
    order[0] = first;
    order[1] = 1 - first;
    for (unsigned i = 0; i < kFallbackCount; i++)
        order[2 + i] = 2 + i;
    for (unsigned k = 0; k < kFaceCount; k++) {
        FT_Face f = face(order[k]);
        if (f && (*glyph = FT_Get_Char_Index(f, c)))
            return order[k];
    }
    *glyph = 0;
    return face(first) ? first : 1 - first;
}

struct Glyph {
    int left { 0 }, top { 0 }, width { 0 }, rows { 0 };
    int advance { 0 }; // pixels
    std::vector<uint8_t> coverage;
};
static std::unordered_map<uint64_t, Glyph> s_glyphs;

static const Glyph* glyphFor(gunichar c, bool bold, int size)
{
    FT_UInt index = 0;
    unsigned faceIndex = faceFor(c, bold, &index);
    uint64_t key = (static_cast<uint64_t>(faceIndex) << 56) | (static_cast<uint64_t>(size & 0xffff) << 40) | (static_cast<uint64_t>(index) << 8) | (bold ? 1 : 0);
    auto it = s_glyphs.find(key);
    if (it != s_glyphs.end())
        return &it->second;
    if (s_glyphs.size() > 4000)
        s_glyphs.clear();
    Glyph g;
    FT_Face f = face(faceIndex);
    if (f && !FT_Set_Pixel_Sizes(f, 0, size) && !FT_Load_Glyph(f, index, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT)) {
        FT_GlyphSlot slot = f->glyph;
        g.left = slot->bitmap_left;
        g.top = slot->bitmap_top;
        g.width = static_cast<int>(slot->bitmap.width);
        g.rows = static_cast<int>(slot->bitmap.rows);
        g.advance = static_cast<int>((slot->advance.x + 32) >> 6);
        if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY && g.width > 0 && g.rows > 0) {
            g.coverage.resize(static_cast<size_t>(g.width) * g.rows);
            for (int y = 0; y < g.rows; y++)
                memcpy(g.coverage.data() + static_cast<size_t>(y) * g.width, slot->bitmap.buffer + y * slot->bitmap.pitch, g.width);
        } else
            g.width = g.rows = 0;
    }
    return &s_glyphs.emplace(key, std::move(g)).first->second;
}

static inline uint32_t premultiply(uint32_t argb, unsigned extraAlpha)
{
    unsigned a = ((argb >> 24) * extraAlpha + 127) / 255;
    unsigned r = (((argb >> 16) & 0xff) * a + 127) / 255;
    unsigned g = (((argb >> 8) & 0xff) * a + 127) / 255;
    unsigned b = ((argb & 0xff) * a + 127) / 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

// source over, both premultiplied
static inline uint32_t over(uint32_t dst, uint32_t src)
{
    unsigned sa = src >> 24;
    if (sa == 255)
        return src;
    if (!sa)
        return dst;
    unsigned inv = 255 - sa;
    unsigned a = sa + ((dst >> 24) * inv + 127) / 255;
    unsigned r = ((src >> 16) & 0xff) + (((dst >> 16) & 0xff) * inv + 127) / 255;
    unsigned g = ((src >> 8) & 0xff) + (((dst >> 8) & 0xff) * inv + 127) / 255;
    unsigned b = (src & 0xff) + ((dst & 0xff) * inv + 127) / 255;
    return (std::min(a, 255u) << 24) | (std::min(r, 255u) << 16) | (std::min(g, 255u) << 8) | std::min(b, 255u);
}

static std::u32string decode(const char* utf8)
{
    std::u32string out;
    if (!utf8)
        return out;
    const char* end = nullptr;
    g_utf8_validate(utf8, -1, &end); // keep what is valid
    std::string valid(utf8, end - utf8);
    glong count = 0;
    gunichar* ucs = g_utf8_to_ucs4(valid.c_str(), -1, nullptr, &count, nullptr);
    if (ucs)
        out.assign(reinterpret_cast<char32_t*>(ucs), count);
    g_free(ucs);
    return out;
}

static int advanceOf(gunichar c, bool bold, int size)
{
    if (c < 0x20 || c == 0x7f)
        c = ' ';
    return glyphFor(c, bold, size)->advance;
}

} // namespace

WPEDisplayPS5Canvas* wpe_display_ps5_canvas_new(int width, int height)
{
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
        return nullptr;
    auto* canvas = new _WPEDisplayPS5Canvas;
    canvas->width = width;
    canvas->height = height;
    canvas->px.assign(static_cast<size_t>(width) * height, 0);
    return canvas;
}

void wpe_display_ps5_canvas_free(WPEDisplayPS5Canvas* canvas)
{
    delete canvas;
}

int wpe_display_ps5_canvas_get_width(WPEDisplayPS5Canvas* canvas)
{
    return canvas ? canvas->width : 0;
}

int wpe_display_ps5_canvas_get_height(WPEDisplayPS5Canvas* canvas)
{
    return canvas ? canvas->height : 0;
}

void wpe_display_ps5_canvas_clear(WPEDisplayPS5Canvas* canvas, guint32 argb)
{
    if (!canvas)
        return;
    std::fill(canvas->px.begin(), canvas->px.end(), premultiply(argb, 255));
}

void wpe_display_ps5_canvas_fill(WPEDisplayPS5Canvas* canvas, int x, int y, int width, int height, int radius, guint32 argb)
{
    if (!canvas || width <= 0 || height <= 0)
        return;
    radius = std::clamp(radius, 0, std::min(width, height) / 2);
    const uint32_t color = premultiply(argb, 255);
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(canvas->width, x + width), y1 = std::min(canvas->height, y + height);
    for (int py = y0; py < y1; py++) {
        uint32_t* row = canvas->px.data() + static_cast<size_t>(py) * canvas->width;
        // Rounded corners: the horizontal inset of this row, with a soft edge.
        double inset = 0;
        double dy = 0;
        if (radius) {
            if (py < y + radius)
                dy = y + radius - (py + 0.5);
            else if (py >= y + height - radius)
                dy = (py + 0.5) - (y + height - radius);
        }
        if (dy > 0)
            inset = radius - std::sqrt(std::max(0.0, radius * radius - dy * dy));
        for (int px = x0; px < x1; px++) {
            uint32_t c = color;
            if (inset > 0) {
                double fromLeft = (px + 0.5) - x, fromRight = (x + width) - (px + 0.5);
                double edge = std::min(fromLeft, fromRight) - inset; // > 0.5: inside
                if (edge <= -0.5)
                    continue;
                if (edge < 0.5)
                    c = premultiply(argb, static_cast<unsigned>((edge + 0.5) * 255));
            }
            row[px] = over(row[px], c);
        }
    }
}

int wpe_display_ps5_canvas_text_width(WPEDisplayPS5Canvas*, int size, gboolean bold, const char* utf8)
{
    if (!utf8 || !*utf8 || size <= 0 || !fontsReady())
        return 0;
    int width = 0;
    for (char32_t c : decode(utf8))
        width += advanceOf(c, bold, size);
    return width;
}

int wpe_display_ps5_canvas_text(WPEDisplayPS5Canvas* canvas, int x, int baseline, int maxWidth, int size, gboolean bold, guint32 argb, const char* utf8)
{
    if (!canvas || !utf8 || !*utf8 || size <= 0 || !fontsReady())
        return 0;
    std::u32string text = decode(utf8);
    // Too wide: cut and end with an ellipsis.
    if (maxWidth > 0) {
        int total = 0;
        for (char32_t c : text)
            total += advanceOf(c, bold, size);
        if (total > maxWidth) {
            const int ellipsis = advanceOf(0x2026, bold, size);
            int used = 0;
            size_t keep = 0;
            for (; keep < text.size(); keep++) {
                int a = advanceOf(text[keep], bold, size);
                if (used + a + ellipsis > maxWidth)
                    break;
                used += a;
            }
            text.resize(keep);
            while (!text.empty() && text.back() == ' ')
                text.pop_back();
            text.push_back(0x2026);
        }
    }
    int penX = x;
    for (char32_t ch : text) {
        gunichar c = ch < 0x20 || ch == 0x7f ? ' ' : ch;
        const Glyph* g = glyphFor(c, bold, size);
        const int gx = penX + g->left, gy = baseline - g->top;
        for (int row = 0; row < g->rows; row++) {
            int py = gy + row;
            if (py < 0 || py >= canvas->height)
                continue;
            uint32_t* dst = canvas->px.data() + static_cast<size_t>(py) * canvas->width;
            const uint8_t* cov = g->coverage.data() + static_cast<size_t>(row) * g->width;
            for (int col = 0; col < g->width; col++) {
                int px = gx + col;
                if (px < 0 || px >= canvas->width || !cov[col])
                    continue;
                dst[px] = over(dst[px], premultiply(argb, cov[col]));
            }
        }
        penX += g->advance;
    }
    return penX - x;
}

void wpe_display_ps5_set_ui_layer(WPEDisplayPS5* display, WPEDisplayPS5Canvas* canvas, int x, int y)
{
    g_return_if_fail(WPE_IS_DISPLAY_PS5(display));
    if (!canvas) {
        PS5Renderer::singleton().setUILayer(nullptr, 0, 0, 0, 0);
        return;
    }
    PS5Renderer::singleton().setUILayer(reinterpret_cast<const uint8_t*>(canvas->px.data()), x, y, canvas->width, canvas->height);
}
