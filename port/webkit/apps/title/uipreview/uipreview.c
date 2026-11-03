/*
 * uipreview: the browser's own UI (browserdraw.c on PS5UICanvas.cpp, and
 * the notices of PS5Text.cpp) rendered on the Mac to PNG files, for design
 * work without the console. Sample tabs, bookmarks and history; the UI is
 * composited over a page screenshot as on the TV. See build.sh.
 *
 *   uipreview <out-dir> <background.png> [fonts-dir]
 */
#include "../browserdraw.h"

#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PS5UICanvas.cpp's set_ui_layer ends here (uipreview-glue.cpp). */
extern const uint8_t* uipreview_layer;
extern int uipreview_layer_rect[4];
/* PS5Text.cpp through uipreview-glue.cpp: premultiplied RGBA. */
int uipreview_text_panel(const char* text, int pixelSize, uint8_t** rgba, int* width, int* height);

#define W 1920
#define H 1080

static uint32_t* background;

static int loadPNG(const char* path, uint32_t** out, int* w, int* h)
{
    png_image image;
    memset(&image, 0, sizeof image);
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path))
        return 0;
    image.format = PNG_FORMAT_BGRA;
    *out = malloc(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, NULL, *out, 0, NULL))
        return 0;
    *w = (int)image.width;
    *h = (int)image.height;
    return 1;
}

static void savePNG(const char* path, const uint32_t* px, int w, int h)
{
    png_image image;
    memset(&image, 0, sizeof image);
    image.version = PNG_IMAGE_VERSION;
    image.width = (png_uint_32)w;
    image.height = (png_uint_32)h;
    image.format = PNG_FORMAT_BGRA;
    if (!png_image_write_to_file(&image, path, 0, px, 0, NULL))
        fprintf(stderr, "uipreview: %s not written: %s\n", path, image.message);
}

/* src (premultiplied BGRA) over dst (opaque BGRA). */
static void over(uint32_t* dst, int dw, int dh, const uint8_t* src, int x0, int y0, int sw, int sh, int rgba)
{
    for (int y = 0; y < sh; y++) {
        int dy = y0 + y;
        if (dy < 0 || dy >= dh)
            continue;
        for (int x = 0; x < sw; x++) {
            int dx = x0 + x;
            if (dx < 0 || dx >= dw)
                continue;
            const uint8_t* s = src + ((size_t)y * sw + x) * 4;
            uint8_t* d = (uint8_t*)&dst[(size_t)dy * dw + dx];
            unsigned a = s[3], inv = 255 - a;
            unsigned sb = rgba ? s[2] : s[0], sr = rgba ? s[0] : s[2];
            d[0] = (uint8_t)(sb + (d[0] * inv + 127) / 255);
            d[1] = (uint8_t)(s[1] + (d[1] * inv + 127) / 255);
            d[2] = (uint8_t)(sr + (d[2] * inv + 127) / 255);
            d[3] = 255;
        }
    }
}

/* ---- sample data */

typedef struct {
    const char *title, *url, *note;
} Sample;

static const Sample kTabs[] = {
    { "PlayStation 5 - Wikipedia", "https://en.wikipedia.org/wiki/PlayStation_5", NULL },
    { "Hacker News", "https://news.ycombinator.com/", "kept live" },
    { "WebKit Feature Status", "https://webkit.org/status/", NULL },
    { "Big Buck Bunny 60fps 4K - Official Blender Foundation Short Film", "https://www.youtube.com/watch?v=aqz-KE-bpKQ", NULL },
    { "ps5 vr2 at DuckDuckGo", "https://lite.duckduckgo.com/lite?q=ps5+vr2", NULL },
};
static const Sample kBookmarks[] = {
    { "BBC News - Home", "https://www.bbc.co.uk/news", NULL },
    { "The Verge", "https://www.theverge.com/", NULL },
    { "GitHub", "https://github.com/", NULL },
    { "r/PS5", "https://old.reddit.com/r/PS5/", NULL },
    { "Wikipedia", "https://en.wikipedia.org/", NULL },
    { "Wikipedia, the free encyclopedia", "https://en.wikipedia.org/wiki/Main_Page", NULL },
};
static const char* const kHistoryTitles[] = { "PlayStation 5 - Wikipedia", "Hacker News", "WebKit Feature Status", "ps5 vr2 at DuckDuckGo",
    "GitHub", "The Verge", "BBC News - Home", "Big Buck Bunny 60fps 4K", "Example Domain", "Wikipedia", "r/PS5", "HTML5 test" };
static const char* const kHistoryURLs[] = { "https://en.wikipedia.org/wiki/PlayStation_5", "https://news.ycombinator.com/", "https://webkit.org/status/",
    "https://lite.duckduckgo.com/lite?q=ps5+vr2", "https://github.com/", "https://www.theverge.com/", "https://www.bbc.co.uk/news",
    "https://www.youtube.com/watch?v=aqz-KE-bpKQ", "https://example.com/", "https://en.wikipedia.org/", "https://old.reddit.com/r/PS5/", "https://html5test.co/" };
#define HISTORY_COUNT 40

static int tabCount = 5, shownTab = 0, armed = 0, bookmarkCount = 6;

static void row(int index, BUIRow* r, void* user)
{
    const BUIState* s = user;
    if (s->section == SEC_TABS) {
        if (index >= tabCount) {
            r->title = "+ New tab";
            r->url = "Opens the home page";
            r->action = TRUE;
            return;
        }
        r->title = kTabs[index].title;
        r->url = kTabs[index].url;
        r->number = index + 1;
        r->current = index == shownTab;
        if (kTabs[index].note)
            snprintf(r->note, sizeof r->note, "%s", kTabs[index].note);
    } else if (s->section == SEC_BOOKMARKS) {
        r->title = kBookmarks[index].title;
        r->url = kBookmarks[index].url;
    } else {
        if (!index) {
            r->title = armed ? "Press X again to clear all history" : "Clear all history";
            r->url = "Bookmarks and open tabs stay";
            r->action = TRUE;
            r->armed = armed;
            return;
        }
        int i = (index - 1) % 12;
        r->title = kHistoryTitles[i];
        r->url = kHistoryURLs[i];
        snprintf(r->note, sizeof r->note, "%02d Nov %02d:%02d", 3 - (index - 1) / 14, 21 - (index - 1) % 14, (index * 17) % 60);
    }
}

static BUIState baseState(void)
{
    BUIState s;
    memset(&s, 0, sizeof s);
    s.screenW = W;
    s.screenH = H;
    s.progress = 1;
    s.canBack = TRUE;
    s.canNewTab = TRUE;
    s.title = kTabs[0].title;
    s.uri = kTabs[0].url;
    s.tabCount = (unsigned)tabCount;
    s.maxTabs = 12;
    s.zone = ZONE_TOOLBAR;
    s.toolbarFocus = TB_ADDRESS;
    s.section = SEC_TABS;
    s.bookmarkCount = (unsigned)bookmarkCount;
    s.historyCount = HISTORY_COUNT;
    s.rowCount = tabCount + 1;
    s.row = row;
    return s;
}

static const char* outDir;

static void shot(const char* name, WPEDisplayPS5Canvas* canvas)
{
    uint32_t* frame = malloc((size_t)W * H * 4);
    memcpy(frame, background, (size_t)W * H * 4);
    wpe_display_ps5_set_ui_layer((WPEDisplayPS5*)1, canvas, 0, 0);
    over(frame, W, H, uipreview_layer, uipreview_layer_rect[0], uipreview_layer_rect[1], uipreview_layer_rect[2], uipreview_layer_rect[3], 0);
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.png", outDir, name);
    savePNG(path, frame, W, H);
    free(frame);
    printf("  %s\n", path);
}

static void menuShot(const char* name, BUIState* s)
{
    WPEDisplayPS5Canvas* canvas = wpe_display_ps5_canvas_new(W, H);
    s->user = s;
    bui_draw_menu(canvas, s);
    shot(name, canvas);
    wpe_display_ps5_canvas_free(canvas);
}

static void noticeShot(const char* name, const char* text, int size, int centred)
{
    uint32_t* frame = malloc((size_t)W * H * 4);
    memcpy(frame, background, (size_t)W * H * 4);
    uint8_t* rgba;
    int w, h;
    if (uipreview_text_panel(text, size, &rgba, &w, &h)) {
        /* as PS5Renderer draws messages: centred, 60 px above the bottom */
        over(frame, W, H, rgba, (W - w) / 2, centred ? (H - h) / 2 : H - h - 60, w, h, 1);
        free(rgba);
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.png", outDir, name);
    savePNG(path, frame, W, H);
    free(frame);
    printf("  %s\n", path);
}

static double nowMS(void)
{
    return g_get_monotonic_time() / 1000.0;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: uipreview <out-dir> <background.png>\n");
        return 2;
    }
    outDir = argv[1];
    int bw, bh;
    uint32_t* bg;
    if (!loadPNG(argv[2], &bg, &bw, &bh)) {
        fprintf(stderr, "uipreview: %s not read\n", argv[2]);
        return 1;
    }
    /* scaled to the TV (nearest) */
    background = malloc((size_t)W * H * 4);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            background[(size_t)y * W + x] = bg[(size_t)(y * bh / H) * bw + x * bw / W];

    BUIState s = baseState();
    menuShot("menu-tabs-address", &s);

    s = baseState();
    s.zone = ZONE_LIST;
    s.listFocus = 1;
    menuShot("menu-tabs-list", &s);

    s = baseState();
    s.zone = ZONE_TOOLBAR;
    s.toolbarFocus = TB_STAR;
    s.starred = TRUE;
    s.loading = TRUE;
    s.progress = 0.4;
    menuShot("menu-toolbar-star-loading", &s);

    s = baseState();
    s.section = SEC_BOOKMARKS;
    s.rowCount = bookmarkCount;
    s.zone = ZONE_LIST;
    s.listFocus = 2;
    menuShot("menu-bookmarks", &s);

    s = baseState();
    s.section = SEC_BOOKMARKS;
    s.bookmarkCount = 0;
    s.rowCount = 0;
    menuShot("menu-bookmarks-empty", &s);

    s = baseState();
    s.section = SEC_HISTORY;
    s.rowCount = HISTORY_COUNT + 1;
    s.zone = ZONE_LIST;
    s.listTop = 6;
    s.listFocus = 9;
    menuShot("menu-history", &s);

    armed = 1;
    s = baseState();
    s.section = SEC_HISTORY;
    s.rowCount = HISTORY_COUNT + 1;
    s.zone = ZONE_LIST;
    s.listFocus = 0;
    menuShot("menu-history-clear", &s);
    armed = 0;

    /* the loading bar, alone over the page */
    s = baseState();
    s.loading = TRUE;
    s.progress = 0.45;
    s.title = "";
    s.uri = "https://en.wikipedia.org/wiki/PlayStation_5";
    WPEDisplayPS5Canvas* bar = wpe_display_ps5_canvas_new(W, bui_bar_height());
    wpe_display_ps5_canvas_clear(bar, 0);
    bui_draw_bar(bar, &s, FALSE);
    shot("loadbar", bar);
    s.title = kTabs[0].title;
    s.progress = 0.8;
    wpe_display_ps5_canvas_clear(bar, 0);
    bui_draw_bar(bar, &s, FALSE);
    shot("loadbar-titled", bar);
    wpe_display_ps5_canvas_free(bar);

    noticeShot("notice-bookmarked", "Bookmarked", 40, 0);
    noticeShot("notice-vr", "PS VR2 not connected\nConnect the headset and press the touchpad again", 40, 0);
    noticeShot("notice-tv-vr",
        "Playing in the PS VR2 headset\nBig Buck Bunny 60fps 4K\n180\xC2\xB0 3D side by side  \xC2\xB7  The guess\n"
        "\xE2\x9C\x95 play/pause   \xE2\x97\x80 \xE2\x96\xB6 seek 10 s   \xE2\x96\xB3 projection   \xE2\x96\xA1 recentre\n"
        "Back to the TV: \xE2\x97\x8B, Options or the touchpad",
        52, 1);

    /* the cost of drawing the menu (fonts and glyphs warm) */
    s = baseState();
    s.zone = ZONE_LIST;
    s.listFocus = 2;
    s.user = &s;
    WPEDisplayPS5Canvas* canvas = wpe_display_ps5_canvas_new(W, H);
    bui_draw_menu(canvas, &s);
    const int runs = 40;
    double start = nowMS();
    for (int i = 0; i < runs; i++)
        bui_draw_menu(canvas, &s);
    double menuMS = (nowMS() - start) / runs;
    bar = wpe_display_ps5_canvas_new(W, bui_bar_height());
    s.loading = TRUE;
    s.progress = 0.5;
    start = nowMS();
    for (int i = 0; i < runs; i++) {
        wpe_display_ps5_canvas_clear(bar, 0);
        bui_draw_bar(bar, &s, FALSE);
    }
    double barMS = (nowMS() - start) / runs;
    printf("draw cost on this Mac: menu %.2f ms, loading bar %.2f ms\n", menuMS, barMS);
    return 0;
}
