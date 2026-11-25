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
#include "PS5Text.h"

#include <algorithm>
#include <cmath>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <glib.h>
#include <string>

// The fonts' folder (uipreview, the Mac harness, builds with its own).
#ifndef PS5WK_FONT_DIR
#define PS5WK_FONT_DIR "/data/ps5webkit/fonts"
#endif

static const char* const kFontFiles[] = {
    PS5WK_FONT_DIR "/NotoSans-Bold.ttf",
    PS5WK_FONT_DIR "/NotoSansSymbols2-Regular.ttf",
    PS5WK_FONT_DIR "/NotoSansSymbols-Regular.ttf",
    PS5WK_FONT_DIR "/NotoSansMath-Regular.ttf",
};
static constexpr unsigned kFontCount = sizeof(kFontFiles) / sizeof(kFontFiles[0]);

static FT_Library s_library;
static FT_Face s_faces[kFontCount];
static bool s_tried;
static bool s_ok;

static bool loadFonts()
{
    if (s_tried)
        return s_ok;
    s_tried = true;
    if (FT_Init_FreeType(&s_library)) {
        g_message("[wpe-ps5] text: FT_Init_FreeType failed");
        return false;
    }
    for (unsigned i = 0; i < kFontCount; i++) {
        if (FT_New_Face(s_library, kFontFiles[i], 0, &s_faces[i]))
            s_faces[i] = nullptr;
    }
    s_ok = s_faces[0];
    g_message("[wpe-ps5] text: %s", s_ok ? "fonts loaded" : "NotoSans-Bold.ttf missing: no text");
    return s_ok;
}

// The face that has this character (the first one if none has it).
static FT_Face faceFor(gunichar c, FT_UInt* glyph)
{
    for (unsigned i = 0; i < kFontCount; i++) {
        if (s_faces[i] && (*glyph = FT_Get_Char_Index(s_faces[i], c)))
            return s_faces[i];
    }
    *glyph = 0;
    return s_faces[0];
}

struct Line {
    std::u32string text;
    int size { 0 };
    int width { 0 };
};

static int lineWidth(Line& line)
{
    int width = 0;
    for (gunichar c : line.text) {
        FT_UInt glyph;
        FT_Face face = faceFor(c, &glyph);
        FT_Set_Pixel_Sizes(face, 0, line.size);
        if (!FT_Load_Glyph(face, glyph, FT_LOAD_DEFAULT))
            width += static_cast<int>(face->glyph->advance.x >> 6);
    }
    line.width = width;
    return width;
}

// How far inside a width x height rectangle with rounded corners the middle
// of pixel (x, y) is: its distance to the edge, negative outside.
static float insideRounded(int x, int y, int width, int height, float radius)
{
    float dx = std::max({ radius - x - 0.5f, x + 0.5f - (width - radius), 0.0f });
    float dy = std::max({ radius - y - 0.5f, y + 0.5f - (height - radius), 0.0f });
    if (!dx && !dy) // the straight parts
        return std::min({ x + 0.5f, width - x - 0.5f, y + 0.5f, height - y - 0.5f });
    return radius - std::sqrt(dx * dx + dy * dy);
}

bool ps5RenderTextPanel(const char* text, int pixelSize, PS5TextImage& out, PS5TextBar* bar)
{
    out = { };
    if (bar)
        *bar = { };
    if (!text || !*text || !loadFonts())
        return false;

    std::vector<Line> lines;
    gchar** parts = g_strsplit(text, "\n", 0);
    for (unsigned i = 0; parts[i]; i++) {
        Line line;
        line.size = i ? std::max(8, static_cast<int>(pixelSize * 0.62)) : pixelSize;
        glong count = 0;
        gunichar* ucs = g_utf8_to_ucs4(parts[i], -1, nullptr, &count, nullptr);
        if (ucs)
            line.text.assign(reinterpret_cast<char32_t*>(ucs), count);
        g_free(ucs);
        lines.push_back(line);
    }
    g_strfreev(parts);

    const int pad = pixelSize * 6 / 10;
    int width = 0, height = pad;
    for (auto& line : lines) {
        width = std::max(width, lineWidth(line));
        height += line.size * 13 / 10;
    }
    width += 2 * pad + pixelSize / 2;
    // The progress bar: a quarter of the text's height, under its last line.
    const int barHeight = bar ? std::max(6, pixelSize / 4) : 0;
    const int barTop = height + (bar ? pixelSize / 6 : 0);
    if (bar) {
        width = std::max(width, pixelSize * 12);
        height = barTop + barHeight + pad * 9 / 10;
    } else
        height += pad - pixelSize / 5;
    width = std::min(width, 4096);
    height = std::min(height, 2048);
    out.width = width;
    out.height = height;
    out.rgba.assign(static_cast<size_t>(width) * height * 4, 0);

    // Panel: Parallax's navy glass (apps/title/brand.h: PX_SURFACE, 95 %
    // opaque) with a thin edge from the logo's violet to its magenta.
    const float radius = pixelSize * 0.6f;
    const float panelAlpha = 0.95f, edgeAlpha = 0.9f;
    const float edge = std::max(1.5f, pixelSize / 26.0f);
    static const float surface[3] = { 0x0d / 255.0f, 0x0b / 255.0f, 0x2a / 255.0f };
    static const float violet[3] = { 0x6d / 255.0f, 0x0e / 255.0f, 0xfd / 255.0f };
    static const float magenta[3] = { 0xfc / 255.0f, 0x0e / 255.0f, 0xc6 / 255.0f };
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float inside = insideRounded(x, y, width, height, radius);
            float coverage = std::clamp(inside + 0.5f, 0.0f, 1.0f);
            float ring = coverage - std::clamp(inside - edge + 0.5f, 0.0f, 1.0f);
            float t = width > 1 ? static_cast<float>(x) / (width - 1) : 0;
            float a = coverage * panelAlpha, e = ring * edgeAlpha;
            uint8_t* p = &out.rgba[(static_cast<size_t>(y) * width + x) * 4];
            for (int k = 0; k < 3; k++) {
                float edgeColor = violet[k] + (magenta[k] - violet[k]) * t;
                p[k] = static_cast<uint8_t>(std::lround(255 * (edgeColor * e + surface[k] * a * (1 - e))));
            }
            p[3] = static_cast<uint8_t>(std::lround(255 * (e + a * (1 - e))));
        }
    }

    // The progress bar's track (the text's lavender, faint) and the bar when
    // full (the edge's violet to magenta), both with round ends.
    if (bar && height <= 2048) {
        const int barWidth = width - 2 * (pad + pixelSize / 4);
        bar->x = (width - barWidth) / 2;
        bar->y = barTop;
        bar->full.width = barWidth;
        bar->full.height = barHeight;
        bar->full.rgba.assign(static_cast<size_t>(barWidth) * barHeight * 4, 0);
        static const float track[3] = { 0xb9 / 255.0f, 0xb5 / 255.0f, 0xe0 / 255.0f };
        const float trackAlpha = 0.22f;
        for (int y = 0; y < barHeight; y++) {
            for (int x = 0; x < barWidth; x++) {
                float coverage = std::clamp(insideRounded(x, y, barWidth, barHeight, barHeight / 2.0f) + 0.5f, 0.0f, 1.0f);
                float t = barWidth > 1 ? static_cast<float>(x) / (barWidth - 1) : 0;
                uint8_t* full = &bar->full.rgba[(static_cast<size_t>(y) * barWidth + x) * 4];
                uint8_t* p = &out.rgba[(static_cast<size_t>(bar->y + y) * width + bar->x + x) * 4];
                float a = coverage * trackAlpha;
                for (int k = 0; k < 3; k++) {
                    full[k] = static_cast<uint8_t>(std::lround(255 * (violet[k] + (magenta[k] - violet[k]) * t) * coverage));
                    p[k] = static_cast<uint8_t>(std::lround(255 * track[k] * a + p[k] * (1 - a)));
                }
                full[3] = static_cast<uint8_t>(std::lround(255 * coverage));
                p[3] = static_cast<uint8_t>(std::lround(255 * a + p[3] * (1 - a)));
            }
        }
    }

    // Glyphs over the panel: the first line in white (PX_TEXT), the others
    // lavender (PX_TEXT_2).
    int baseline = pad;
    for (size_t index = 0; index < lines.size(); index++) {
        auto& line = lines[index];
        static const float first[3] = { 0xf4 / 255.0f, 0xf2 / 255.0f, 0xff / 255.0f };
        static const float other[3] = { 0xb9 / 255.0f, 0xb5 / 255.0f, 0xe0 / 255.0f };
        const float* color = index ? other : first;
        baseline += line.size;
        int penX = (width - line.width) / 2;
        for (gunichar c : line.text) {
            FT_UInt glyph;
            FT_Face face = faceFor(c, &glyph);
            FT_Set_Pixel_Sizes(face, 0, line.size);
            if (FT_Load_Glyph(face, glyph, FT_LOAD_RENDER))
                continue;
            const FT_Bitmap& bitmap = face->glyph->bitmap;
            const int left = penX + face->glyph->bitmap_left, top = baseline - face->glyph->bitmap_top;
            for (unsigned row = 0; row < bitmap.rows; row++) {
                int y = top + static_cast<int>(row);
                if (y < 0 || y >= height)
                    continue;
                for (unsigned col = 0; col < bitmap.width; col++) {
                    int x = left + static_cast<int>(col);
                    if (x < 0 || x >= width)
                        continue;
                    float g = bitmap.buffer[row * bitmap.pitch + col] / 255.0f;
                    uint8_t* p = &out.rgba[(static_cast<size_t>(y) * width + x) * 4];
                    for (int k = 0; k < 3; k++)
                        p[k] = static_cast<uint8_t>(std::lround(255 * color[k] * g + p[k] * (1 - g)));
                    p[3] = static_cast<uint8_t>(std::lround(255 * g + p[3] * (1 - g)));
                }
            }
            penX += static_cast<int>(face->glyph->advance.x >> 6);
        }
        baseline += line.size * 3 / 10;
    }
    return true;
}
