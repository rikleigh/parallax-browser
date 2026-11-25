// uipreview: what PS5UICanvas.cpp and PS5Text.cpp need from the renderer.
#include "PS5Renderer.h"
#include "PS5Text.h"
#include <cstdlib>
#include <cstring>

extern "C" {
const uint8_t* uipreview_layer;
int uipreview_layer_rect[4];

int uipreview_text_panel(const char* text, int pixelSize, uint8_t** rgba, int* width, int* height)
{
    PS5TextImage image;
    if (!ps5RenderTextPanel(text, pixelSize, image))
        return 0;
    *rgba = static_cast<uint8_t*>(malloc(image.rgba.size()));
    memcpy(*rgba, image.rgba.data(), image.rgba.size());
    *width = image.width;
    *height = image.height;
    return 1;
}

// A notice with a progress bar, as PS5Renderer draws it: the full bar's left
// part over the panel's track.
int uipreview_progress_panel(const char* text, int pixelSize, double fraction, uint8_t** rgba, int* width, int* height)
{
    PS5TextImage image;
    PS5TextBar bar;
    if (!ps5RenderTextPanel(text, pixelSize, image, &bar))
        return 0;
    const int filled = static_cast<int>(bar.full.width * fraction + 0.5);
    for (int y = 0; y < bar.full.height; y++) {
        for (int x = 0; x < filled && x < bar.full.width; x++) {
            const uint8_t* s = &bar.full.rgba[(static_cast<size_t>(y) * bar.full.width + x) * 4];
            uint8_t* d = &image.rgba[(static_cast<size_t>(bar.y + y) * image.width + bar.x + x) * 4];
            for (int k = 0; k < 4; k++)
                d[k] = static_cast<uint8_t>(s[k] + d[k] * (255 - s[3]) / 255);
        }
    }
    *rgba = static_cast<uint8_t*>(malloc(image.rgba.size()));
    memcpy(*rgba, image.rgba.data(), image.rgba.size());
    *width = image.width;
    *height = image.height;
    return 1;
}
}

PS5Renderer& PS5Renderer::singleton()
{
    static PS5Renderer renderer;
    return renderer;
}

void PS5Renderer::setUILayer(const uint8_t* bgra, int x, int y, int width, int height)
{
    uipreview_layer = bgra;
    uipreview_layer_rect[0] = x;
    uipreview_layer_rect[1] = y;
    uipreview_layer_rect[2] = width;
    uipreview_layer_rect[3] = height;
}
