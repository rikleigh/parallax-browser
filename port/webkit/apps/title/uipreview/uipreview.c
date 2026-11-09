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

typedef struct {
    const char *file, *host, *kind, *detail, *state;
    double progress;
    int running, tone;
} DownloadSample;
static const DownloadSample kDownloads[] = {
    { "etaHEN-2.1B.bin", "github.com", "BIN", "48.2 MB of 131 MB · 11.6 MB/s · 7 s left", "37%", 0.368, 1, BUI_TONE_ACTIVE },
    { "ps5-homebrew-store-1.4.pkg", "cdn.example.org", "PKG", "1.18 GB of 2.00 GB · 21.4 MB/s · 39 s left", "Restarted from zero", 0.59, 1, BUI_TONE_WARN },
    { "Big_Buck_Bunny_1080p.zip", "download.blender.org", "ZIP", "312 MB of 1.21 GB · Paused when the browser closed", "Paused", 0.25, 0, BUI_TONE_PLAIN },
    { "kstuff.elf", "github.com", "ELF", "84.0 KB of 412 KB · Connection lost", "Retrying in 5 s", 0.20, 0, BUI_TONE_WARN },
    { "firmware-notes.txt", "example.com", "TXT", "Not enough space: needs 2.00 GB, 1.10 GB free", "Failed", -1, 0, BUI_TONE_WARN },
    { "ftpsrv-ps5.elf", "github.com", "ELF", "412 KB · 03 Nov 14:22", "Done", -1, 0, BUI_TONE_DONE },
    { "wpe-sysroot.tar.gz", "files.example.net", "GZ", "96.4 MB · 03 Nov 11:05", "Done", -1, 0, BUI_TONE_DONE },
};
#define DOWNLOAD_COUNT 7
static int deleteArmedRow = -1;

static int tabCount = 5, shownTab = 0, armed = 0, bookmarkCount = 6;

static void row(int index, BUIRow* r, void* user)
{
    const BUIState* s = user;
    if (s->section == SEC_TABS) {
        if (index >= tabCount) {
            r->title = "New tab";
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
    } else if (s->section == SEC_DOWNLOADS) {
        const DownloadSample* d = &kDownloads[index % DOWNLOAD_COUNT];
        r->title = d->file;
        r->url = d->host;
        snprintf(r->kind, sizeof r->kind, "%s", d->kind);
        snprintf(r->detail, sizeof r->detail, "%s", d->detail);
        snprintf(r->state, sizeof r->state, "%s", d->state);
        r->progress = d->progress;
        r->running = d->running;
        r->tone = d->tone;
        if (index == deleteArmedRow) {
            r->armed = TRUE;
            snprintf(r->state, sizeof r->state, "Press \xE2\x96\xA1 again to delete the file");
            r->tone = BUI_TONE_WARN;
        }
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
    s.userName = "Player 1";
    s.zone = ZONE_TOOLBAR;
    s.toolbarFocus = TB_ADDRESS;
    s.section = SEC_TABS;
    s.bookmarkCount = (unsigned)bookmarkCount;
    s.historyCount = HISTORY_COUNT;
    s.downloadCount = DOWNLOAD_COUNT;
    s.rowCount = tabCount + 1;
    s.row = row;
    return s;
}

static const char* outDir;

static void shotAt(const char* name, WPEDisplayPS5Canvas* canvas, int x, int y)
{
    uint32_t* frame = malloc((size_t)W * H * 4);
    memcpy(frame, background, (size_t)W * H * 4);
    wpe_display_ps5_set_ui_layer((WPEDisplayPS5*)1, canvas, x, y);
    over(frame, W, H, uipreview_layer, uipreview_layer_rect[0], uipreview_layer_rect[1], uipreview_layer_rect[2], uipreview_layer_rect[3], 0);
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.png", outDir, name);
    savePNG(path, frame, W, H);
    free(frame);
    printf("  %s\n", path);
}

static void shot(const char* name, WPEDisplayPS5Canvas* canvas)
{
    shotAt(name, canvas, 0, 0);
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

static void vrShot(const char* name, const BUIVRState* v)
{
    WPEDisplayPS5Canvas* panel = wpe_display_ps5_canvas_new(BUI_VR_PANEL_W, BUI_VR_PANEL_H);
    bui_draw_vr_panel(panel, v);
    shotAt(name, panel, (W - BUI_VR_PANEL_W) / 2, H - BUI_VR_PANEL_H - 80);
    wpe_display_ps5_canvas_free(panel);
}

static void vrTVShot(const char* name, const BUIVRState* v)
{
    uint32_t* saved = background;
    uint32_t* black = calloc((size_t)W * H, 4);
    for (size_t i = 0; i < (size_t)W * H; i++)
        black[i] = 0xff000000u;
    background = black;
    WPEDisplayPS5Canvas* card = wpe_display_ps5_canvas_new(BUI_VR_TV_W, BUI_VR_TV_H);
    bui_draw_vr_tv(card, v);
    shotAt(name, card, (W - BUI_VR_TV_W) / 2, (H - BUI_VR_TV_H) / 2);
    wpe_display_ps5_canvas_free(card);
    background = saved;
    free(black);
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
    s.toolbarFocus = TB_SETTINGS;
    menuShot("menu-toolbar-settings", &s);

    s = baseState();
    s.toolbarFocus = TB_GAMEPAD;
    s.gamepadOn = TRUE;
    menuShot("menu-toolbar-gamepad-on", &s);

    s = baseState();
    s.toolbarFocus = TB_PRIVATE;
    s.privateMode = TRUE;
    menuShot("menu-toolbar-private", &s);

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

    /* downloads */
    s = baseState();
    s.section = SEC_DOWNLOADS;
    s.rowCount = DOWNLOAD_COUNT;
    s.downloadsActive = 2;
    s.downloadsProgress = 0.52;
    s.zone = ZONE_LIST;
    s.listFocus = 0;
    s.downloadCross = "Pause";
    s.downloadSquare = "Cancel";
    menuShot("menu-downloads", &s);

    s.listFocus = 2;
    s.downloadCross = "Resume";
    menuShot("menu-downloads-paused", &s);

    deleteArmedRow = 5;
    s.listFocus = 5;
    s.downloadCross = "Details";
    s.downloadSquare = "Delete file";
    menuShot("menu-downloads-delete", &s);
    deleteArmedRow = -1;

    {
        BUIDetails d;
        memset(&d, 0, sizeof d);
        d.title = "ftpsrv-ps5.elf";
        const char* labels[] = { "Address", "Came from", "File", "Size", "SHA-256", "Validator", "Started", "State" };
        const char* values[] = { "https://github.com/ps5-payload-dev/ftpsrv/releases/download/v0.11/ftpsrv-ps5.elf",
            "https://objects.githubusercontent.com/github-production-release-asset-2e65be/512345678/0a1b2c3d-4e5f-6789-abcd-ef0123456789?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=releaseassetproduction%2F20261103%2Fus-east-1%2Fs3%2Faws4_request&X-Amz-Date=20261103T142201Z&X-Amz-Expires=300&X-Amz-Signature=5f2b",
            "/data/ps5webkit/downloads/ftpsrv-ps5.elf", "412 KB (421 888 bytes)", "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",
            "\"0x8DCF2A1B3C4D5E6\"  ·  Mon, 02 Nov 2026 18:04:11 GMT", "03 Nov 2026 14:22:01, done 14:22:03", "Done" };
        for (int i = 0; i < 8; i++) {
            d.labels[i] = labels[i];
            d.values[i] = values[i];
        }
        d.count = 8;
        s.details = &d;
        menuShot("menu-downloads-details", &s);
        s.details = NULL;
    }

    s = baseState();
    s.section = SEC_DOWNLOADS;
    s.downloadCount = 0;
    s.rowCount = 0;
    menuShot("menu-downloads-empty", &s);

    s = baseState();
    s.downloadsActive = 1;
    s.downloadsProgress = 0.37;
    menuShot("menu-tabs-downloading", &s);

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
    s.downloadsActive = 1;
    s.downloadsProgress = 0.62;
    wpe_display_ps5_canvas_clear(bar, 0);
    bui_draw_bar(bar, &s, FALSE);
    shot("loadbar-downloading", bar);
    s.downloadsActive = 3;
    s.downloadsProgress = -1;
    wpe_display_ps5_canvas_clear(bar, 0);
    bui_draw_bar(bar, &s, FALSE);
    shot("loadbar-downloading-3", bar);
    s.downloadsActive = 0;
    wpe_display_ps5_canvas_free(bar);

    noticeShot("notice-bookmarked", "Bookmarked", 40, 0);
    noticeShot("notice-download-started", "Download started: ftpsrv-ps5.elf\n(Options, then R1: Downloads)", 40, 0);
    noticeShot("notice-downloaded", "Downloaded ftpsrv-ps5.elf", 40, 0);
    noticeShot("notice-download-failed", "Download failed: firmware-notes.txt\nNot enough space: needs 2.00 GB, 1.10 GB free", 40, 0);
    noticeShot("notice-vr", "PS VR2 not connected\nConnect the headset and press the touchpad again", 40, 0);
    noticeShot("notice-tv-vr",
        "Playing in the PS VR2 headset\nBig Buck Bunny 60fps 4K\n180\xC2\xB0 3D side by side  \xC2\xB7  The guess\n"
        "\xE2\x9C\x95 play/pause   \xE2\x97\x80 \xE2\x96\xB6 seek 10 s   \xE2\x96\xB3 projection   \xE2\x96\xA1 recentre\n"
        "Back to the TV: \xE2\x97\x8B, Options or the touchpad",
        52, 1);

    /* View in VR: the headset's panel (over the page picture, as a stand-in
     * for the video, where the headset shows it: below the centre), and
     * the TV's card on black */
    {
        BUIVRState v;
        memset(&v, 0, sizeof v);
        v.title = "Big Buck Bunny 60fps 4K - Official Blender Foundation Short Film";
        v.projection = "180\xC2\xB0 3D side by side";
        v.message = "The guess: \xE2\x96\xB3 picks another, remembered for this site";
        v.controls = TRUE;
        v.time = 83;
        v.duration = 634;
        v.buffered = 121;
        vrShot("vr-panel-playing", &v);
        v.paused = TRUE;
        v.message = "Recentred";
        vrShot("vr-panel-paused", &v);
        v.paused = FALSE;
        v.seeking = TRUE;
        v.target = 263;
        vrShot("vr-panel-seeking", &v);
        v.target = 23;
        vrShot("vr-panel-seeking-back", &v);
        v.seeking = FALSE;
        v.time = 3725;
        v.duration = 5410;
        v.buffered = 3760;
        v.message = NULL;
        vrShot("vr-panel-long", &v);
        v.duration = 1.0 / 0.0;
        v.time = 1834;
        vrShot("vr-panel-live", &v);
        v.controls = FALSE;
        v.message = "No <video> in the page: the headset shows the stream the page plays";
        vrShot("vr-panel-nocontrols", &v);

        memset(&v, 0, sizeof v);
        v.title = "Big Buck Bunny 60fps 4K - Official Blender Foundation Short Film";
        v.projection = "180\xC2\xB0 3D side by side";
        v.message = "The guess";
        v.controls = TRUE;
        v.time = 83;
        v.duration = 634;
        v.buffered = 121;
        vrTVShot("vr-tv", &v);
        v.seeking = TRUE;
        v.target = 263;
        vrTVShot("vr-tv-seeking", &v);
    }

    /* the cost of drawing the menu (fonts and glyphs warm) */
    s = baseState();
    s.zone = ZONE_LIST;
    s.listFocus = 2;
    s.user = &s;
    WPEDisplayPS5Canvas* canvas = wpe_display_ps5_canvas_new(W, H);
    bui_draw_menu(canvas, &s);
    /* the best of 60 (the Mac's own noise out) */
    const int runs = 60;
    double menuMS = 1e9, barMS = 1e9;
    for (int i = 0; i < runs; i++) {
        double start = nowMS();
        bui_draw_menu(canvas, &s);
        menuMS = MIN(menuMS, nowMS() - start);
    }
    bar = wpe_display_ps5_canvas_new(W, bui_bar_height());
    s.loading = TRUE;
    s.progress = 0.5;
    for (int i = 0; i < runs; i++) {
        double start = nowMS();
        wpe_display_ps5_canvas_clear(bar, 0);
        bui_draw_bar(bar, &s, FALSE);
        barMS = MIN(barMS, nowMS() - start);
    }
    printf("draw cost on this Mac (best of %d): menu %.2f ms, loading bar %.2f ms\n", runs, menuMS, barMS);
    return 0;
}
