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
