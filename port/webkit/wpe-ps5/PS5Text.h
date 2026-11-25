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
#include <vector>

// Text the title draws itself (not the page): the "View in VR" hint in the
// headset, the notice on the TV while the video plays in the headset, and
// short messages over the page. FreeType (already linked for WebKit) with
// the Noto fonts the browser uses (/data/ps5webkit/fonts), glyph fallback
// through the symbol fonts for the pad's button shapes. Render thread only.
struct PS5TextImage {
    int width { 0 };
    int height { 0 };
    std::vector<uint8_t> rgba; // premultiplied RGBA, row 0 = top
};

// UTF-8 text, '\n' between lines; the first line is drawn at pixelSize,
// the others at 0.62 of it, all centred, white and lavender on a rounded
// navy glass panel with a violet-to-magenta edge (Parallax's palette). False (and an empty image) when the fonts are missing.
//
// With a bar the panel also has a progress bar under its text: its empty
// track is drawn in the panel at (bar->x, bar->y), and bar->full is the bar
// when full (the track's size), to be drawn over the track from its left,
// as much of it as the progress.
struct PS5TextBar {
    int x { 0 };
    int y { 0 };
    PS5TextImage full;
};
bool ps5RenderTextPanel(const char* text, int pixelSize, PS5TextImage& out, PS5TextBar* bar = nullptr);
