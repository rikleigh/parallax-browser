/*
 * The browser's own UI drawn into a canvas: see browserdraw.h.
 */
#include "browserdraw.h"

#include <stdio.h>
#include <string.h>

#define C_DIM 0xa0000000u
#define C_BAR 0xf2161b24u
#define C_PANEL 0xf51a202bu
#define C_BUTTON 0xff252d3bu
#define C_FOCUS_BG 0xff34445eu
#define C_FOCUS 0xffffd54au
#define C_TEXT 0xffe8ecf2u
#define C_DIM_TEXT 0xff9aa4b2u
#define C_ACCENT 0xff9cc7ffu
#define C_PROGRESS 0xff4c9affu
#define C_ROW 0xff212836u
#define C_SECTION_ON 0xff2d3a52u

#define BAR_H 96
#define PANEL_X 160
#define PANEL_Y (BAR_H + 24)
#define ROW_H 76
#define SECTION_H 70

static BUIHot hots[64];
static unsigned hotCount;

static void addHot(int x, int y, int w, int h, int kind, int index)
{
    if (hotCount < G_N_ELEMENTS(hots))
        hots[hotCount++] = (BUIHot) { x, y, w, h, kind, index };
}

const BUIHot* bui_hot_at(double x, double y)
{
    for (unsigned i = hotCount; i-- > 0;) {
        const BUIHot* h = &hots[i];
        if (x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h)
            return h;
    }
    return NULL;
}

int bui_bar_height(void)
{
    return BAR_H;
}

int bui_row_height(void)
{
    return ROW_H;
}

int bui_visible_rows(int screenH)
{
    const int panelH = screenH - PANEL_Y - 40;
    return (panelH - SECTION_H - 64) / ROW_H;
}

static void drawButton(WPEDisplayPS5Canvas* c, int x, int y, int w, int h, const char* label, gboolean focused, gboolean enabled, int index)
{
    if (focused) {
        wpe_display_ps5_canvas_fill(c, x - 3, y - 3, w + 6, h + 6, 14, C_FOCUS);
        wpe_display_ps5_canvas_fill(c, x, y, w, h, 12, C_FOCUS_BG);
    } else
        wpe_display_ps5_canvas_fill(c, x, y, w, h, 12, C_BUTTON);
    int size = 34;
    int tw = wpe_display_ps5_canvas_text_width(c, size, TRUE, label);
    wpe_display_ps5_canvas_text(c, x + (w - tw) / 2, y + h / 2 + size * 36 / 100, w - 8, size, TRUE,
        enabled ? C_TEXT : 0xff5a6270u, label);
    if (index >= 0)
        addHot(x, y, w, h, HOT_TOOLBAR, index);
}

void bui_draw_bar(WPEDisplayPS5Canvas* c, const BUIState* s, gboolean interactive)
{
    const int screenW = s->screenW;
    const int y = 16, h = 64, w = 84, gap = 12;
    wpe_display_ps5_canvas_fill(c, 0, 0, screenW, BAR_H, 0, C_BAR);
    int x = 24;
    const char* labels[TB_COUNT] = { "◀", "▶", s->loading ? "✕" : "↻", "⌂", NULL, NULL, "+" };
    gboolean enabled[TB_COUNT] = { s->canBack, s->canForward, TRUE, TRUE, TRUE, TRUE, s->canNewTab };
    for (int i = TB_BACK; i <= TB_HOME; i++) {
        drawButton(c, x, y, w, h, labels[i], interactive && s->zone == ZONE_TOOLBAR && s->toolbarFocus == i, enabled[i], interactive ? i : -1);
        x += w + gap;
    }
    const int right = screenW - 24;
    const int newTabX = right - w, starX = newTabX - gap - w, countX = starX - gap - 96;
    drawButton(c, newTabX, y, w, h, labels[TB_NEWTAB], interactive && s->zone == ZONE_TOOLBAR && s->toolbarFocus == TB_NEWTAB, enabled[TB_NEWTAB], interactive ? TB_NEWTAB : -1);
    drawButton(c, starX, y, w, h, s->starred ? "★" : "☆", interactive && s->zone == ZONE_TOOLBAR && s->toolbarFocus == TB_STAR, TRUE, interactive ? TB_STAR : -1);
    char count[32];
    snprintf(count, sizeof count, "%u tab%s", s->tabCount, s->tabCount == 1 ? "" : "s");
    int cw = wpe_display_ps5_canvas_text_width(c, 22, FALSE, count);
    wpe_display_ps5_canvas_text(c, countX + (96 - cw) / 2, y + 40, 96, 22, FALSE, C_DIM_TEXT, count);

    const int ax = x + 8, aw = countX - 16 - ax;
    gboolean addressFocused = interactive && s->zone == ZONE_TOOLBAR && s->toolbarFocus == TB_ADDRESS;
    if (addressFocused)
        wpe_display_ps5_canvas_fill(c, ax - 3, y - 3, aw + 6, h + 6, 14, C_FOCUS);
    wpe_display_ps5_canvas_fill(c, ax, y, aw, h, 12, addressFocused ? C_FOCUS_BG : 0xff0f131au);
    if (interactive)
        addHot(ax, y, aw, h, HOT_TOOLBAR, TB_ADDRESS);
    const char* title = s->title ? s->title : "";
    const char* uri = s->uri ? s->uri : "";
    if (!strncmp(uri, "file:///data/ps5webkit/start.html", 33))
        uri = "Start page";
    if (*title) {
        wpe_display_ps5_canvas_text(c, ax + 20, y + 28, aw - 40, 23, TRUE, C_TEXT, title);
        wpe_display_ps5_canvas_text(c, ax + 20, y + 54, aw - 40, 19, FALSE, C_DIM_TEXT, uri);
    } else
        wpe_display_ps5_canvas_text(c, ax + 20, y + 42, aw - 40, 23, FALSE, C_TEXT, *uri ? uri : "Address or search");
    if (addressFocused) {
        const char* hint = "X: type an address or search";
        int hw = wpe_display_ps5_canvas_text_width(c, 18, FALSE, hint);
        wpe_display_ps5_canvas_fill(c, ax + aw - hw - 36, y + 14, hw + 24, 36, 8, 0xe0101520u);
        wpe_display_ps5_canvas_text(c, ax + aw - hw - 24, y + 39, 0, 18, FALSE, C_FOCUS, hint);
    }
    if (s->loading) {
        double progress = s->progress;
        if (progress < 0.05)
            progress = 0.05;
        wpe_display_ps5_canvas_fill(c, 0, BAR_H - 5, (int)(screenW * progress), 5, 0, C_PROGRESS);
    }
}

void bui_draw_menu(WPEDisplayPS5Canvas* c, const BUIState* s)
{
    const int screenW = s->screenW, screenH = s->screenH;
    hotCount = 0;
    wpe_display_ps5_canvas_clear(c, C_DIM);
    bui_draw_bar(c, s, TRUE);
    const int px = PANEL_X, py = PANEL_Y, pw = screenW - 2 * PANEL_X, ph = screenH - PANEL_Y - 40;
    wpe_display_ps5_canvas_fill(c, px, py, pw, ph, 18, C_PANEL);
    static const char* names[SEC_COUNT] = { "Tabs", "Bookmarks", "History" };
    int sx = px + 24;
    for (int sec = 0; sec < SEC_COUNT; sec++) {
        char label[48];
        if (sec == SEC_TABS)
            snprintf(label, sizeof label, "%s  %u/%u", names[sec], s->tabCount, s->maxTabs);
        else
            snprintf(label, sizeof label, "%s  %u", names[sec], sec == SEC_BOOKMARKS ? s->bookmarkCount : s->historyCount);
        int lw = wpe_display_ps5_canvas_text_width(c, 26, TRUE, label) + 48;
        if (sec == s->section)
            wpe_display_ps5_canvas_fill(c, sx, py + 14, lw, SECTION_H - 20, 10, C_SECTION_ON);
        wpe_display_ps5_canvas_text(c, sx + 24, py + 14 + (SECTION_H - 20) / 2 + 9, 0, 26, TRUE, sec == s->section ? C_TEXT : C_DIM_TEXT, label);
        addHot(sx, py + 14, lw, SECTION_H - 20, HOT_SECTION, sec);
        sx += lw + 12;
    }
    const char* keys = "L1/R1 lists   □ remove   R2 new tab   △ address   O close";
    int kw = wpe_display_ps5_canvas_text_width(c, 19, FALSE, keys);
    wpe_display_ps5_canvas_text(c, px + pw - kw - 28, py + 50, 0, 19, FALSE, C_DIM_TEXT, keys);

    const int rows = s->rowCount, visible = bui_visible_rows(screenH);
    const int rx = px + 20, rw = pw - 40;
    int ry = py + SECTION_H + 8;
    if (!rows) {
        const char* empty = s->section == SEC_BOOKMARKS ? "No bookmarks yet. ☆ in the bar (Options, then up and right) adds the page shown."
                                                        : "No history yet.";
        wpe_display_ps5_canvas_text(c, rx + 20, ry + 48, rw - 40, 24, FALSE, C_DIM_TEXT, empty);
    }
    for (int row = s->listTop; row < rows && row < s->listTop + visible; row++) {
        BUIRow r = { 0 };
        s->row(row, &r, s->user);
        const char* title = r.title ? r.title : "";
        const char* url = r.url ? r.url : "";
        gboolean focused = s->zone == ZONE_LIST && row == s->listFocus;
        if (focused) {
            wpe_display_ps5_canvas_fill(c, rx - 3, ry - 3, rw + 6, ROW_H - 8 + 6, 12, C_FOCUS);
            wpe_display_ps5_canvas_fill(c, rx, ry, rw, ROW_H - 8, 10, C_FOCUS_BG);
        } else
            wpe_display_ps5_canvas_fill(c, rx, ry, rw, ROW_H - 8, 10, C_ROW);
        addHot(rx, ry, rw, ROW_H - 8, HOT_ROW, row);
        int tx = rx + 24;
        if (s->section == SEC_TABS && r.action)
            tx += 48;
        if (r.number) {
            char number[16];
            snprintf(number, sizeof number, "%d", r.number);
            if (r.current)
                wpe_display_ps5_canvas_fill(c, rx, ry, 8, ROW_H - 8, 4, C_ACCENT);
            wpe_display_ps5_canvas_text(c, tx, ry + 44, 0, 24, TRUE, r.current ? C_ACCENT : C_DIM_TEXT, number);
            tx += 48;
        }
        int extraW = r.note[0] ? wpe_display_ps5_canvas_text_width(c, 19, FALSE, r.note) : 0;
        int textW = rx + rw - 24 - tx - (extraW ? extraW + 24 : 0);
        wpe_display_ps5_canvas_text(c, tx, ry + 30, textW, 23, TRUE, r.current ? C_ACCENT : C_TEXT, title);
        wpe_display_ps5_canvas_text(c, tx, ry + 57, textW, 18, FALSE, C_DIM_TEXT, url);
        if (extraW)
            wpe_display_ps5_canvas_text(c, rx + rw - 24 - extraW, ry + 44, 0, 19, FALSE, C_DIM_TEXT, r.note);
        ry += ROW_H;
    }
    if (rows > visible) {
        int trackY = py + SECTION_H + 8, trackH = visible * ROW_H - 8;
        int thumbH = trackH * visible / rows, thumbY = trackY + (trackH - thumbH) * s->listTop / (rows - visible);
        wpe_display_ps5_canvas_fill(c, px + pw - 14, thumbY, 6, thumbH < 24 ? 24 : thumbH, 3, 0xff5a6478u);
    }
    wpe_display_ps5_canvas_text(c, px + 28, py + ph - 18, pw - 56, 18, FALSE, 0xff6c7686u,
        "Left stick: pointer   Right stick: scroll   L3: home   R3: Settings   Touchpad: View in VR");
}
