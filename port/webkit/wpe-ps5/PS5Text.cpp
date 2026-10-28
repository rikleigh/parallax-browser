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

static const char* const kFontFiles[] = {
    "/data/ps5webkit/fonts/NotoSans-Bold.ttf",
    "/data/ps5webkit/fonts/NotoSansSymbols2-Regular.ttf",
    "/data/ps5webkit/fonts/NotoSansSymbols-Regular.ttf",
    "/data/ps5webkit/fonts/NotoSansMath-Regular.ttf",
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

bool ps5RenderTextPanel(const char* text, int pixelSize, PS5TextImage& out)
{
    out = { };
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

    const int pad = pixelSize / 2;
    int width = 0, height = pad;
    for (auto& line : lines) {
        width = std::max(width, lineWidth(line));
        height += line.size * 13 / 10;
    }
    width += 2 * pad + pixelSize / 2;
    height += pad - pixelSize / 5;
    width = std::min(width, 4096);
    height = std::min(height, 2048);
    out.width = width;
    out.height = height;
    out.rgba.assign(static_cast<size_t>(width) * height * 4, 0);

    // Panel: rounded rectangle, dark and 80 % opaque (premultiplied).
    const float radius = pixelSize * 0.6f;
    const float panelAlpha = 0.80f;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float dx = std::max({ radius - x - 0.5f, x + 0.5f - (width - radius), 0.0f });
            float dy = std::max({ radius - y - 0.5f, y + 0.5f - (height - radius), 0.0f });
            float coverage = std::clamp(radius - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
            float a = coverage * panelAlpha;
            uint8_t* p = &out.rgba[(static_cast<size_t>(y) * width + x) * 4];
            p[0] = static_cast<uint8_t>(a * 0.06f * 255);
            p[1] = static_cast<uint8_t>(a * 0.08f * 255);
            p[2] = static_cast<uint8_t>(a * 0.11f * 255);
            p[3] = static_cast<uint8_t>(a * 255);
        }
    }

    // Glyphs, white, composited over the panel.
    int baseline = pad;
    for (auto& line : lines) {
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
                    for (int k = 0; k < 4; k++)
                        p[k] = static_cast<uint8_t>(std::lround(255 * g + p[k] * (1 - g)));
                }
            }
            penX += static_cast<int>(face->glyph->advance.x >> 6);
        }
        baseline += line.size * 3 / 10;
    }
    return true;
}
