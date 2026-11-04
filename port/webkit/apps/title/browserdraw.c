/*
 * The browser's own UI drawn into a canvas: see browserdraw.h. The look:
 * deep-navy glass over the page (brand.h), the focused item raised with a
 * violet-to-magenta outline and a soft glow, the pad's buttons as key caps.
 */
#include "browserdraw.h"
#include "brand.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The logo (made from sce_sys/icon0.png by brand-assets.py, in the title
 * package by package.sh); without it the wordmark stands alone. */
#ifndef BUI_MARK_PATH
#define BUI_MARK_PATH "/app0/ui/parallax-mark.bgra"
#endif

#define WHITE 0xffffffffu

/* the bar */
#define BAR_H 104
#define BTN_W 76
#define BTN_H 64
#define BTN_Y 20
#define GAP 12
#define EDGE 28 /* the screen's left and right margins in the bar */
/* the menu */
#define PANEL_X 120
#define PANEL_Y (BAR_H + 20)
#define PANEL_BOTTOM 32
#define HEADER_H 88
#define FOOTER_H 72
#define ROW_H 86   /* row pitch */
#define ROW_BOX 76 /* a row's box */
#define LIST_TOP (PANEL_Y + HEADER_H + 16)
/* type sizes */
#define T_ICON 32
#define T_SECTION 27
#define T_TITLE 25
#define T_ADDRESS_TITLE 24
#define T_URL 20
#define T_HINT 20
#define T_CAP 17

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

static int listBottom(int screenH)
{
    return screenH - PANEL_BOTTOM - FOOTER_H - 8;
}

int bui_visible_rows(int screenH)
{
    return (listBottom(screenH) - LIST_TOP + (ROW_H - ROW_BOX)) / ROW_H;
}

/* ---------------------------------------------------------------- pieces */

static guint32* mark;
static int markSize;

static void loadMark(void)
{
    static gboolean tried;
    if (tried)
        return;
    tried = TRUE;
    gchar* data = NULL;
    gsize length = 0;
    if (!g_file_get_contents(BUI_MARK_PATH, &data, &length, NULL))
        return;
    guint32 w = 0, h = 0;
    if (length >= 12 && !memcmp(data, "PXMK", 4)) {
        memcpy(&w, data + 4, 4);
        memcpy(&h, data + 8, 4);
    }
    if (w && w == h && w <= 256 && length == 12 + (gsize)w * h * 4) {
        mark = g_new(guint32, w * h);
        memcpy(mark, data + 12, (size_t)w * h * 4);
        markSize = (int)w;
    }
    g_free(data);
}

/* Text centred on (cx, cy), cut to maxWidth. */
static void textCentred(WPEDisplayPS5Canvas* c, int cx, int cy, int maxWidth, int size, gboolean bold, guint32 color, const char* text)
{
    int w = wpe_display_ps5_canvas_text_width(c, size, bold, text);
    if (maxWidth > 0 && w > maxWidth)
        w = maxWidth;
    wpe_display_ps5_canvas_text(c, cx - w / 2, cy + size * 36 / 100, maxWidth, size, bold, color, text);
}

/* The focused look: glow, raised fill, gradient outline. */
static void focusBox(WPEDisplayPS5Canvas* c, int x, int y, int w, int h, int radius)
{
    wpe_display_ps5_canvas_glow(c, x, y, w, h, radius, PX_FOCUS_GLOW, PX_A(PX_FOCUS_FROM, 0x80), PX_A(PX_FOCUS_TO, 0x80));
    wpe_display_ps5_canvas_fill(c, x, y, w, h, radius, PX_A(PX_RAISED_HI, 0xf4));
    wpe_display_ps5_canvas_stroke(c, x, y, w, h, radius, PX_FOCUS_RING, PX_FOCUS_FROM, PX_FOCUS_TO, FALSE);
}

/* A pane of glass: translucent fill, a lit top edge fading down. */
static void glass(WPEDisplayPS5Canvas* c, int x, int y, int w, int h, int radius, guint32 fill)
{
    wpe_display_ps5_canvas_fill(c, x, y, w, h, radius, fill);
    wpe_display_ps5_canvas_stroke(c, x, y, w, h, radius, 1, PX_A(WHITE, 0x2a), PX_A(WHITE, 0x08), TRUE);
}

/* A key cap for the pad's button (✕ ○ △ □, "touchpad", or a label such
 * as L1), centred on cy. Returns its width. */
static int keyCap(WPEDisplayPS5Canvas* c, int x, int cy, const char* key)
{
    if (!strcmp(key, "touchpad")) {
        const int w = 46, h = 28;
        wpe_display_ps5_canvas_fill(c, x, cy - h / 2, w, h, 8, PX_A(WHITE, 0x14));
        wpe_display_ps5_canvas_stroke(c, x, cy - h / 2, w, h, 8, 2, PX_TEXT_2, PX_TEXT_2, FALSE);
        return w;
    }
    const gboolean face = !strcmp(key, "✕") || !strcmp(key, "○") || !strcmp(key, "△") || !strcmp(key, "□");
    if (face) {
        const int d = 34;
        wpe_display_ps5_canvas_fill(c, x, cy - d / 2, d, d, d / 2, PX_A(WHITE, 0x1f));
        textCentred(c, x + d / 2, cy, 0, 19, FALSE, PX_TEXT, key);
        return d;
    }
    const int h = 30, w = wpe_display_ps5_canvas_text_width(c, T_CAP, TRUE, key) + 20;
    wpe_display_ps5_canvas_fill(c, x, cy - h / 2, w, h, 8, PX_A(WHITE, 0x1f));
    textCentred(c, x + w / 2, cy, 0, T_CAP, TRUE, PX_TEXT, key);
    return w;
}

/* A key cap and what it does. Returns the width used. */
static int hint(WPEDisplayPS5Canvas* c, int x, int cy, const char* key, const char* label)
{
    int w = keyCap(c, x, cy, key) + 10;
    w += wpe_display_ps5_canvas_text(c, x + w, cy + T_HINT * 36 / 100, 0, T_HINT, FALSE, PX_TEXT_2, label);
    return w;
}

static int hintWidth(WPEDisplayPS5Canvas* c, const char* key, const char* label)
{
    int cap = !strcmp(key, "touchpad") ? 46 : (!strcmp(key, "✕") || !strcmp(key, "○") || !strcmp(key, "△") || !strcmp(key, "□")) ? 34
                                                                                                                             : wpe_display_ps5_canvas_text_width(c, T_CAP, TRUE, key) + 20;
    return cap + 10 + wpe_display_ps5_canvas_text_width(c, T_HINT, FALSE, label);
}

static gboolean isStartPage(const char* uri)
{
    return !strncmp(uri, "file:///data/ps5webkit/start.html", 33);
}

/* Where an address starts showing: https:// and www. are left out. */
static const char* hostStart(const char* url)
{
    if (!strncmp(url, "https://", 8))
        url += 8;
    if (!strncmp(url, "www.", 4))
        url += 4;
    return url;
}

/* An address: the host brighter than the rest. */
static void drawURL(WPEDisplayPS5Canvas* c, int x, int baseline, int maxWidth, int size, const char* url)
{
    if (isStartPage(url)) {
        wpe_display_ps5_canvas_text(c, x, baseline, maxWidth, size, FALSE, PX_TEXT_2, "Start page");
        return;
    }
    const char* host = hostStart(url);
    const char* scheme = strstr(host, "://");
    const char* hostEnd = scheme ? scheme + 3 : host; /* http://, file://: the host after it */
    hostEnd = strchr(hostEnd, '/');
    if (!hostEnd || hostEnd == host) {
        wpe_display_ps5_canvas_text(c, x, baseline, maxWidth, size, FALSE, PX_TEXT_2, host);
        return;
    }
    char hostPart[256];
    snprintf(hostPart, sizeof hostPart, "%.*s", (int)(hostEnd - host), host);
    int w = wpe_display_ps5_canvas_text(c, x, baseline, maxWidth, size, FALSE, PX_TEXT_2, hostPart);
    if (maxWidth - w > size && strcmp(hostEnd, "/"))
        wpe_display_ps5_canvas_text(c, x + w, baseline, maxWidth - w, size, FALSE, PX_TEXT_3, hostEnd);
}

/* The name a host is known by: its domain without the subdomains and the
 * suffix (en.wikipedia.org, www.bbc.co.uk: "wikipedia", "bbc"). */
static void siteName(const char* host, const char* end, const char** nameStart, const char** nameEnd)
{
    const char* dots[4] = { 0 }; /* the last dots, nearest the end first */
    int count = 0;
    for (const char* p = end; p-- > host;) {
        if (*p == '.' && count < 4)
            dots[count++] = p;
    }
    *nameStart = host;
    *nameEnd = end;
    if (!count)
        return;
    /* ".co.uk", ".com.au": a second-level suffix under a country's */
    int labels = 1;
    if (count >= 2 && end - dots[0] == 3) {
        size_t n = (size_t)(dots[0] - dots[1] - 1);
        static const char* const second[] = { "co", "com", "org", "net", "ac", "gov", "edu", "ne", "or" };
        for (unsigned i = 0; i < G_N_ELEMENTS(second); i++) {
            if (strlen(second[i]) == n && !strncmp(dots[1] + 1, second[i], n))
                labels = 2;
        }
    }
    if (count < labels)
        return;
    *nameEnd = dots[labels - 1];
    *nameStart = count > labels ? dots[labels] + 1 : host;
}

/* A small square with the site's initial, tinted by the site. */
static void siteTile(WPEDisplayPS5Canvas* c, int x, int y, int size, const char* url, const char* glyph)
{
    static const guint32 tints[] = { PX_VIOLET, PX_BLUE, PX_MAGENTA, PX_CYAN, 0xffb30cdeu /* orchid, where the panes cross */ };
    char initial[8] = "";
    guint32 tint = PX_LINE;
    if (glyph)
        snprintf(initial, sizeof initial, "%s", glyph);
    else if (isStartPage(url)) {
        snprintf(initial, sizeof initial, "P");
        tint = PX_VIOLET;
    } else {
        const char* host = hostStart(url);
        const char* scheme = strstr(host, "://");
        if (scheme)
            host = scheme + 3;
        const char* end = host;
        while (*end && *end != '/' && *end != ':' && *end != '?' && *end != '#')
            end++;
        const char* name;
        const char* nameEnd;
        siteName(host, end, &name, &nameEnd);
        if (nameEnd > name) {
            gunichar ch = g_utf8_get_char_validated(name, nameEnd - name);
            ch = ch == (gunichar)-1 || ch == (gunichar)-2 ? '?' : g_unichar_toupper(ch);
            initial[g_unichar_to_utf8(ch, initial)] = 0;
            guint hash = 5381;
            for (const char* p = name; p < nameEnd; p++)
                hash = hash * 33 + (unsigned char)*p;
            tint = tints[hash % G_N_ELEMENTS(tints)];
        }
    }
    if (glyph)
        wpe_display_ps5_canvas_stroke(c, x, y, size, size, PX_RADIUS_CHIP + 2, 2, PX_A(PX_TEXT_3, 0xa0), PX_A(PX_TEXT_3, 0xa0), FALSE);
    else {
        wpe_display_ps5_canvas_fill(c, x, y, size, size, PX_RADIUS_CHIP + 2, PX_A(tint, 0x60));
        wpe_display_ps5_canvas_stroke(c, x, y, size, size, PX_RADIUS_CHIP + 2, 1, PX_A(tint, 0xe0), PX_A(tint, 0xe0), FALSE);
    }
    textCentred(c, x + size / 2, y + size / 2, size, size / 2, TRUE, PX_TEXT, initial);
}

/* ---------------------------------------------------------------- the bar */

static void drawButton(WPEDisplayPS5Canvas* c, int x, int y, int w, int h, const char* label, guint32 color, gboolean focused, int index)
{
    if (focused)
        focusBox(c, x, y, w, h, PX_RADIUS);
    else
        glass(c, x, y, w, h, PX_RADIUS, PX_A(PX_RAISED, 0xe6));
    textCentred(c, x + w / 2, y + h / 2, w - 8, T_ICON, FALSE, color, label);
    if (index >= 0)
        addHot(x, y, w, h, HOT_TOOLBAR, index);
}

static void drawProgress(WPEDisplayPS5Canvas* c, int screenW, double progress)
{
    if (progress < 0.05)
        progress = 0.05;
    if (progress > 1)
        progress = 1;
    const int w = (int)(screenW * progress), y = BAR_H - 4;
    /* the brand's gradient over the whole width, shown up to the progress */
    guint32 head = PX_FOCUS_FROM;
    {
        /* the colour at the head: violet to magenta along the screen */
        guint32 a = PX_FOCUS_FROM, b = PX_FOCUS_TO, out = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            int ca = (a >> shift) & 0xff, cb = (b >> shift) & 0xff;
            out |= (guint32)(ca + (cb - ca) * progress) << shift;
        }
        head = out;
    }
    wpe_display_ps5_canvas_fill_gradient(c, 0, y, w, 4, 0, PX_FOCUS_FROM, head, FALSE);
    /* a soft light at the head */
    wpe_display_ps5_canvas_glow(c, w - 48, y, 48, 4, 2, 10, PX_A(head, 0x00), PX_A(head, 0xa0));
}

/* The bar's download indicator: a ring that fills clockwise from the top in
 * the brand's gradient (violet to magenta along the arc) around an arrow;
 * the number of downloads when more than one. Drawn into a small image
 * (the canvas has no arcs). */
#define RING 44
static void downloadRing(WPEDisplayPS5Canvas* c, int x, int y, unsigned count, double progress)
{
    static guint32 px[RING * RING];
    const double cx = RING / 2.0, cy = RING / 2.0, outer = RING / 2.0 - 3, width = 4.5, inner = outer - width;
    /* unknown size: a quarter arc */
    const double filled = progress < 0 ? 0.25 : progress;
    for (int j = 0; j < RING; j++) {
        for (int i = 0; i < RING; i++) {
            double dx = i + 0.5 - cx, dy = j + 0.5 - cy, r = sqrt(dx * dx + dy * dy);
            /* coverage of the ring (1 px of anti-aliasing at both edges) */
            double cover = fmin(fmax(outer + 0.5 - r, 0), 1) * fmin(fmax(r - inner + 0.5, 0), 1);
            guint32 out = 0;
            if (cover > 0) {
                double angle = atan2(dx, -dy); /* 0 at the top, clockwise */
                if (angle < 0)
                    angle += 2 * G_PI;
                double t = angle / (2 * G_PI);
                guint32 color;
                unsigned alpha;
                if (t <= filled) {
                    double k = filled > 0 ? t / filled : 0;
                    guint32 a = PX_FOCUS_FROM, b = PX_FOCUS_TO;
                    color = 0;
                    for (int shift = 0; shift < 24; shift += 8) {
                        int ca = (a >> shift) & 0xff, cb = (b >> shift) & 0xff;
                        color |= (guint32)(ca + (cb - ca) * k) << shift;
                    }
                    alpha = 255;
                } else {
                    color = 0xffffff;
                    alpha = 0x30;
                }
                unsigned a8 = (unsigned)(alpha * cover + 0.5);
                out = (a8 << 24) | ((((color >> 16) & 0xff) * a8 / 255) << 16) | ((((color >> 8) & 0xff) * a8 / 255) << 8) | ((color & 0xff) * a8 / 255);
            }
            px[j * RING + i] = out;
        }
    }
    wpe_display_ps5_canvas_image(c, x, y, RING, RING, px, 255);
    if (count > 1) {
        char number[8];
        snprintf(number, sizeof number, "%u", count > 9 ? 9 : count);
        textCentred(c, x + RING / 2, y + RING / 2, 0, 18, TRUE, PX_TEXT, number);
    } else
        textCentred(c, x + RING / 2, y + RING / 2, 0, 20, TRUE, PX_TEXT, "↓");
}

void bui_draw_bar(WPEDisplayPS5Canvas* c, const BUIState* s, gboolean interactive)
{
    const int screenW = s->screenW;
    const gboolean toolbar = interactive && s->zone == ZONE_TOOLBAR;
    wpe_display_ps5_canvas_fill_gradient(c, 0, 0, screenW, BAR_H, 0, PX_A(PX_BASE, 0xf6), PX_A(PX_SURFACE, 0xee), TRUE);
    wpe_display_ps5_canvas_fill(c, 0, BAR_H - 1, screenW, 1, 0, PX_A(PX_LINE, 0xff));

    const char* labels[TB_COUNT] = { "←", "→", s->loading ? "✕" : "↻", "⌂", NULL, NULL, "+", "⚙" };
    const gboolean enabled[TB_COUNT] = { s->canBack, s->canForward, TRUE, TRUE, TRUE, TRUE, s->canNewTab, TRUE };
    int x = EDGE;
    for (int i = TB_BACK; i <= TB_HOME; i++) {
        drawButton(c, x, BTN_Y, BTN_W, BTN_H, labels[i], enabled[i] ? PX_TEXT : PX_TEXT_OFF, toolbar && s->toolbarFocus == i, interactive ? i : -1);
        x += BTN_W + GAP;
    }
    /* the right: settings, new tab, bookmark, the tab count */
    const int settingsX = screenW - EDGE - BTN_W, newTabX = settingsX - GAP - BTN_W, starX = newTabX - GAP - BTN_W, countX = starX - GAP - 64;
    drawButton(c, settingsX, BTN_Y, BTN_W, BTN_H, labels[TB_SETTINGS], PX_TEXT, toolbar && s->toolbarFocus == TB_SETTINGS, interactive ? TB_SETTINGS : -1);
    drawButton(c, newTabX, BTN_Y, BTN_W, BTN_H, labels[TB_NEWTAB], enabled[TB_NEWTAB] ? PX_TEXT : PX_TEXT_OFF, toolbar && s->toolbarFocus == TB_NEWTAB,
        interactive ? TB_NEWTAB : -1);
    drawButton(c, starX, BTN_Y, BTN_W, BTN_H, s->starred ? "★" : "☆", s->starred ? PX_PINK : PX_TEXT, toolbar && s->toolbarFocus == TB_STAR,
        interactive ? TB_STAR : -1);
    {
        const int box = 34, bx = countX + (64 - box) / 2, by = BTN_Y + (BTN_H - box) / 2;
        char count[16];
        snprintf(count, sizeof count, "%u", s->tabCount);
        wpe_display_ps5_canvas_stroke(c, bx, by, box, box, 9, 2, PX_TEXT_2, PX_TEXT_2, FALSE);
        textCentred(c, bx + box / 2, by + box / 2, box, 18, TRUE, PX_TEXT, count);
    }

    /* downloads running: the ring left of the tab count */
    int fieldEnd = countX - 8;
    if (s->downloadsActive) {
        const int ringX = countX - 4 - RING;
        downloadRing(c, ringX, BTN_Y + (BTN_H - RING) / 2, s->downloadsActive, s->downloadsProgress);
        fieldEnd = ringX - 10;
    }

    /* the address field */
    const int ax = x + 4, aw = fieldEnd - ax, ay = BTN_Y, ah = BTN_H;
    const gboolean addressFocused = toolbar && s->toolbarFocus == TB_ADDRESS;
    if (addressFocused)
        focusBox(c, ax, ay, aw, ah, PX_RADIUS);
    else {
        wpe_display_ps5_canvas_fill(c, ax, ay, aw, ah, PX_RADIUS, PX_A(PX_INK, 0xe6));
        wpe_display_ps5_canvas_stroke(c, ax, ay, aw, ah, PX_RADIUS, 1, PX_A(PX_LINE, 0xff), PX_A(PX_LINE, 0xff), FALSE);
    }
    if (interactive)
        addHot(ax, ay, aw, ah, HOT_TOOLBAR, TB_ADDRESS);
    int textW = aw - 48;
    if (addressFocused) {
        /* what X does here, on the right */
        const char* what = "Type an address or search";
        const int hw = hintWidth(c, "✕", what) + 28;
        wpe_display_ps5_canvas_fill(c, ax + aw - hw - 12, ay + 12, hw, ah - 24, PX_RADIUS_CHIP, PX_A(PX_INK, 0xb0));
        hint(c, ax + aw - hw + 2, ay + ah / 2, "✕", what);
        textW -= hw + 16;
    }
    const char* title = s->title ? s->title : "";
    const char* uri = s->uri ? s->uri : "";
    if (*title) {
        wpe_display_ps5_canvas_text(c, ax + 24, ay + 29, textW, T_ADDRESS_TITLE, TRUE, PX_TEXT, title);
        drawURL(c, ax + 24, ay + 54, textW, T_URL, uri);
    } else if (*uri)
        drawURL(c, ax + 24, ay + 41, textW, 23, uri);
    else
        wpe_display_ps5_canvas_text(c, ax + 24, ay + 41, textW, 23, FALSE, PX_TEXT_3, "Address or search");

    if (s->loading)
        drawProgress(c, screenW, s->progress);
}

/* ---------------------------------------------------------------- the menu */

static void drawHeader(WPEDisplayPS5Canvas* c, const BUIState* s, int px, int py, int pw)
{
    const int cy = py + HEADER_H / 2;
    int x = px + 28;
    loadMark();
    if (mark) {
        wpe_display_ps5_canvas_image(c, x, cy - markSize / 2, markSize, markSize, mark, 255);
        x += markSize + 14;
    }
    x += wpe_display_ps5_canvas_text(c, x, cy + 10, 0, 28, TRUE, PX_TEXT, "Parallax");
    x += 32;
    wpe_display_ps5_canvas_fill(c, x, cy - 20, 1, 40, 0, PX_A(PX_LINE, 0xff));
    x += 32;

    /* the lists, between L1 and R1 */
    x += keyCap(c, x, cy, "L1") + 22;
    static const char* names[SEC_COUNT] = { "Tabs", "Bookmarks", "History", "Downloads" };
    for (int sec = 0; sec < SEC_COUNT; sec++) {
        char count[32];
        if (sec == SEC_TABS)
            snprintf(count, sizeof count, "%u/%u", s->tabCount, s->maxTabs);
        else
            snprintf(count, sizeof count, "%u", sec == SEC_BOOKMARKS ? s->bookmarkCount : sec == SEC_HISTORY ? s->historyCount : s->downloadCount);
        const gboolean on = sec == s->section;
        const int nw = wpe_display_ps5_canvas_text_width(c, T_SECTION, TRUE, names[sec]);
        const int cw = wpe_display_ps5_canvas_text_width(c, 21, FALSE, count);
        const int w = nw + 10 + cw;
        const int baseline = cy + 9;
        wpe_display_ps5_canvas_text(c, x, baseline, 0, T_SECTION, TRUE, on ? PX_TEXT : PX_TEXT_2, names[sec]);
        wpe_display_ps5_canvas_text(c, x + nw + 10, baseline, 0, 21, FALSE, on ? PX_TEXT_2 : PX_TEXT_3, count);
        if (on)
            wpe_display_ps5_canvas_fill_gradient(c, x, baseline + 14, w, 4, 2, PX_FOCUS_FROM, PX_FOCUS_TO, FALSE);
        addHot(x - 12, py + 12, w + 24, HEADER_H - 24, HOT_SECTION, sec);
        x += w + 36;
    }
    keyCap(c, x - 14, cy, "R1");
    (void)pw;
}

static void drawFooter(WPEDisplayPS5Canvas* c, const BUIState* s, int px, int py, int pw, int ph)
{
    const int fy = py + ph - FOOTER_H;
    wpe_display_ps5_canvas_fill(c, px + 24, fy, pw - 48, 1, 0, PX_A(PX_LINE, 0xc0));
    const int cy = fy + FOOTER_H / 2;
    int x = px + 32;
    const gboolean list = s->zone == ZONE_LIST;
    if (s->details) {
        x += hint(c, x, cy, "○", "Back to the list") + 32;
        hint(c, x, cy, "Options", "Close the menu");
        return;
    }
    if (s->section == SEC_TABS) {
        x += hint(c, x, cy, "✕", list ? "Switch" : "Select") + 32;
        x += hint(c, x, cy, "□", "Close tab") + 32;
    } else if (s->section == SEC_DOWNLOADS) {
        if (!list)
            x += hint(c, x, cy, "✕", "Select") + 32;
        if (list && s->downloadCross)
            x += hint(c, x, cy, "✕", s->downloadCross) + 32;
        if (list && s->downloadSquare)
            x += hint(c, x, cy, "□", s->downloadSquare) + 32;
        if (list && s->rowCount && !(s->downloadCross && !strcmp(s->downloadCross, "Details")))
            x += hint(c, x, cy, "R2", "Details") + 32;
    } else {
        x += hint(c, x, cy, "✕", list ? "Open" : "Select") + 32;
        x += hint(c, x, cy, "R2", "New tab") + 32;
        x += hint(c, x, cy, "□", "Delete") + 32;
    }
    x += hint(c, x, cy, "△", "Address") + 32;
    hint(c, x, cy, "○", "Close");

    /* on the right: what the other buttons do from here */
    static const char* const keys[] = { "L3", "R3", "touchpad" };
    static const char* const labels[] = { "Home", "Settings", "View in VR" };
    int right = px + pw - 32, w = 0;
    for (unsigned i = 0; i < G_N_ELEMENTS(keys); i++)
        w += hintWidth(c, keys[i], labels[i]) + (i ? 28 : 0);
    x = right - w;
    for (unsigned i = 0; i < G_N_ELEMENTS(keys); i++)
        x += hint(c, x, cy, keys[i], labels[i]) + 28;
}

/* The file-type tile: the extension in a tile tinted by it. */
static void kindTile(WPEDisplayPS5Canvas* c, int x, int y, int size, const char* kind)
{
    static const guint32 tints[] = { PX_VIOLET, PX_BLUE, PX_MAGENTA, PX_CYAN, 0xffb30cdeu };
    guint hash = 5381;
    for (const char* p = kind; *p; p++)
        hash = hash * 33 + (unsigned char)*p;
    guint32 tint = tints[hash % G_N_ELEMENTS(tints)];
    wpe_display_ps5_canvas_fill(c, x, y, size, size, PX_RADIUS_CHIP + 2, PX_A(tint, 0x60));
    wpe_display_ps5_canvas_stroke(c, x, y, size, size, PX_RADIUS_CHIP + 2, 1, PX_A(tint, 0xe0), PX_A(tint, 0xe0), FALSE);
    int textSize = strlen(kind) <= 3 ? 16 : 14;
    textCentred(c, x + size / 2, y + size / 2, size - 6, textSize, TRUE, PX_TEXT, kind);
}

static guint32 toneColor(int tone)
{
    return tone == BUI_TONE_ACTIVE ? PX_CYAN_TEXT : tone == BUI_TONE_WARN ? PX_PINK : tone == BUI_TONE_DONE ? PX_TEXT_2 : PX_TEXT_3;
}

/* A download: name, then where from and how far, a progress bar in the
 * brand's gradient, the state on the right. */
static void drawDownloadRow(WPEDisplayPS5Canvas* c, const BUIRow* r, int rx, int ry, int rw, gboolean focused)
{
    if (focused)
        focusBox(c, rx, ry, rw, ROW_BOX, PX_RADIUS);
    else
        wpe_display_ps5_canvas_fill(c, rx, ry, rw, ROW_BOX, PX_RADIUS, PX_A(PX_RAISED, 0xb0));
    int x = rx + 20;
    const int tile = 48;
    kindTile(c, x, ry + (ROW_BOX - tile) / 2, tile, r->kind[0] ? r->kind : "FILE");
    x += tile + 20;

    /* on the right: the state ("Done" with a tick) */
    int stateW = 0;
    if (r->state[0]) {
        const gboolean done = r->tone == BUI_TONE_DONE;
        char text[80];
        snprintf(text, sizeof text, "%s%s", done ? "✓ " : "", r->state);
        stateW = wpe_display_ps5_canvas_text_width(c, T_URL, r->tone != BUI_TONE_PLAIN, text);
        wpe_display_ps5_canvas_text(c, rx + rw - 24 - stateW, ry + 37, 0, T_URL, r->tone != BUI_TONE_PLAIN, toneColor(r->tone), text);
    }
    const int right = rx + rw - 24 - (stateW ? stateW + 28 : 0);
    const int textW = right - x;
    wpe_display_ps5_canvas_text(c, x, ry + 30, textW, T_TITLE, TRUE, r->armed ? PX_PINK : PX_TEXT, r->title ? r->title : "");
    /* the host, then how far */
    int w = 0;
    if (r->url && *r->url)
        w = wpe_display_ps5_canvas_text(c, x, ry + 55, textW, T_URL - 1, FALSE, PX_TEXT_2, r->url);
    if (r->detail[0] && textW - w > 40) {
        char line[140];
        snprintf(line, sizeof line, "%s%s", w ? "  ·  " : "", r->detail);
        wpe_display_ps5_canvas_text(c, x + w, ry + 55, textW - w, T_URL - 1, FALSE, r->tone == BUI_TONE_WARN && !r->running ? PX_PINK : PX_TEXT_3, line);
    }
    /* the bar: a track, the received part in the gradient, a light at its head */
    if (r->progress >= 0) {
        const int bx = x, by = ry + ROW_BOX - 12, bw = rx + rw - 24 - x, bh = 4;
        wpe_display_ps5_canvas_fill(c, bx, by, bw, bh, 2, PX_A(WHITE, 0x18));
        int fw = (int)(bw * r->progress);
        if (fw > 0) {
            if (fw < 6)
                fw = 6;
            guint32 head = PX_FOCUS_FROM;
            guint32 a = PX_FOCUS_FROM, b = PX_FOCUS_TO, out = 0xff000000u;
            for (int shift = 0; shift < 24; shift += 8) {
                int ca = (a >> shift) & 0xff, cb = (b >> shift) & 0xff;
                out |= (guint32)(ca + (cb - ca) * r->progress) << shift;
            }
            head = out;
            wpe_display_ps5_canvas_fill_gradient(c, bx, by, fw, bh, 2, r->running ? PX_FOCUS_FROM : PX_A(PX_FOCUS_FROM, 0x90),
                r->running ? head : PX_A(head, 0x90), FALSE);
            if (r->running)
                wpe_display_ps5_canvas_glow(c, bx + fw - 24, by, 24, bh, 2, 8, PX_A(head, 0x00), PX_A(head, 0xa0));
        }
    }
}

/* Breaks @text into lines of at most @width px (at UTF-8 boundaries, at a
 * space when there is one near the end); up to @max lines, the last cut
 * with an ellipsis. Returns the lines drawn. */
static int drawWrapped(WPEDisplayPS5Canvas* c, int x, int baseline, int width, int size, guint32 color, const char* text, int lineH, int max)
{
    int lines = 0;
    const char* p = text ? text : "";
    while (*p && lines < max) {
        if (lines == max - 1 || wpe_display_ps5_canvas_text_width(c, size, FALSE, p) <= width) {
            wpe_display_ps5_canvas_text(c, x, baseline + lines * lineH, width, size, FALSE, color, p);
            return lines + 1;
        }
        /* the longest prefix that fits */
        size_t lo = 1, hi = strlen(p), best = 1;
        char piece[1024];
        while (lo <= hi) {
            size_t mid = (lo + hi) / 2;
            const char* end = g_utf8_find_prev_char(p, p + mid + 1);
            size_t n = end && end > p ? (size_t)(end - p) : mid;
            if (n >= sizeof piece)
                n = sizeof piece - 1;
            memcpy(piece, p, n);
            piece[n] = 0;
            if (wpe_display_ps5_canvas_text_width(c, size, FALSE, piece) <= width) {
                best = n;
                lo = mid + 1;
            } else
                hi = mid - 1;
        }
        /* a break after a space, a slash or a dash in the last third */
        for (size_t i = best; i > best * 2 / 3; i--) {
            if (p[i - 1] == ' ' || p[i - 1] == '/' || p[i - 1] == '-' || p[i - 1] == '&' || p[i - 1] == '?') {
                best = i;
                break;
            }
        }
        memcpy(piece, p, best);
        piece[best] = 0;
        wpe_display_ps5_canvas_text(c, x, baseline + lines * lineH, width, size, FALSE, color, piece);
        p += best;
        lines++;
    }
    return lines;
}

/* A download's details: a card over the list. */
static void drawDetails(WPEDisplayPS5Canvas* c, const BUIDetails* d, int px, int pw, int screenH)
{
    const int x = px + 64, y = LIST_TOP + 4, w = pw - 128, h = listBottom(screenH) - LIST_TOP - 8;
    wpe_display_ps5_canvas_glow(c, x, y, w, h, PX_RADIUS_PANEL, PX_FOCUS_GLOW, PX_A(PX_FOCUS_FROM, 0x60), PX_A(PX_FOCUS_TO, 0x60));
    wpe_display_ps5_canvas_fill(c, x, y, w, h, PX_RADIUS_PANEL, PX_A(PX_SURFACE, 0xff));
    wpe_display_ps5_canvas_fill(c, x, y, w, h, PX_RADIUS_PANEL, PX_A(PX_RAISED, 0xc0));
    wpe_display_ps5_canvas_stroke(c, x, y, w, h, PX_RADIUS_PANEL, 2, PX_FOCUS_FROM, PX_FOCUS_TO, FALSE);
    wpe_display_ps5_canvas_text(c, x + 40, y + 60, w - 80, 30, TRUE, PX_TEXT, d->title ? d->title : "");
    wpe_display_ps5_canvas_fill_gradient(c, x + 40, y + 78, 96, 4, 2, PX_FOCUS_FROM, PX_FOCUS_TO, FALSE);
    int ly = y + 130;
    const int labelW = 170, vx = x + 40 + labelW, vw = w - 80 - labelW, lineH = 30;
    for (int i = 0; i < d->count && ly < y + h - 20; i++) {
        wpe_display_ps5_canvas_text(c, x + 40, ly, labelW - 16, T_URL, TRUE, PX_TEXT_3, d->labels[i]);
        int lines = drawWrapped(c, vx, ly, vw, 22, PX_TEXT, d->values[i], lineH, 3);
        ly += lines * lineH + 16;
    }
}

static void drawRow(WPEDisplayPS5Canvas* c, const BUIState* s, const BUIRow* r, int rx, int ry, int rw, gboolean focused)
{
    if (s->section == SEC_DOWNLOADS) {
        drawDownloadRow(c, r, rx, ry, rw, focused);
        return;
    }
    if (focused)
        focusBox(c, rx, ry, rw, ROW_BOX, PX_RADIUS);
    else
        wpe_display_ps5_canvas_fill(c, rx, ry, rw, ROW_BOX, PX_RADIUS, PX_A(PX_RAISED, 0xb0));
    const char* title = r->title ? r->title : "";
    const char* url = r->url ? r->url : "";
    int x = rx + 20;
    if (s->section == SEC_TABS) {
        /* the tab's number; the tab shown: cyan, with a bar at the left */
        if (r->current)
            wpe_display_ps5_canvas_fill(c, rx + 6, ry + 16, 4, ROW_BOX - 32, 2, PX_CYAN);
        if (r->number) {
            char number[16];
            snprintf(number, sizeof number, "%d", r->number);
            textCentred(c, x + 14, ry + ROW_BOX / 2, 0, 22, TRUE, r->current ? PX_CYAN_TEXT : PX_TEXT_3, number);
        }
        x += 40;
    }
    const int tile = 48;
    siteTile(c, x, ry + (ROW_BOX - tile) / 2, tile, url, r->action ? (s->section == SEC_TABS ? "+" : "✕") : NULL);
    x += tile + 20;

    /* on the right: a note */
    int noteW = 0;
    if (r->note[0]) {
        if (s->section == SEC_TABS) {
            /* "kept live": a pill with a cyan dot */
            noteW = wpe_display_ps5_canvas_text_width(c, 18, FALSE, r->note) + 44;
            const int nx = rx + rw - 24 - noteW, ny = ry + (ROW_BOX - 32) / 2;
            wpe_display_ps5_canvas_fill(c, nx, ny, noteW, 32, 16, PX_A(WHITE, 0x10));
            wpe_display_ps5_canvas_fill(c, nx + 14, ny + 12, 8, 8, 4, PX_CYAN);
            wpe_display_ps5_canvas_text(c, nx + 30, ny + 22, 0, 18, FALSE, PX_TEXT_2, r->note);
        } else {
            noteW = wpe_display_ps5_canvas_text_width(c, T_URL, FALSE, r->note);
            wpe_display_ps5_canvas_text(c, rx + rw - 24 - noteW, ry + ROW_BOX / 2 + 7, 0, T_URL, FALSE, PX_TEXT_3, r->note);
        }
    }
    const int textW = rx + rw - 24 - x - (noteW ? noteW + 24 : 0);
    const guint32 titleColor = r->armed ? PX_PINK : PX_TEXT;
    wpe_display_ps5_canvas_text(c, x, ry + 33, textW, T_TITLE, TRUE, titleColor, title);
    if (r->action)
        wpe_display_ps5_canvas_text(c, x, ry + 60, textW, T_URL, FALSE, PX_TEXT_3, url);
    else
        drawURL(c, x, ry + 60, textW, T_URL, url);
}

void bui_draw_menu(WPEDisplayPS5Canvas* c, const BUIState* s)
{
    const int screenW = s->screenW, screenH = s->screenH;
    hotCount = 0;
    /* the page under it, dimmed toward the ink */
    wpe_display_ps5_canvas_clear(c, PX_A(PX_INK, 0xb4));
    bui_draw_bar(c, s, TRUE);

    const int px = PANEL_X, py = PANEL_Y, pw = screenW - 2 * PANEL_X, ph = screenH - PANEL_Y - PANEL_BOTTOM;
    glass(c, px, py, pw, ph, PX_RADIUS_PANEL, PX_A(PX_SURFACE, 0xfa));
    drawHeader(c, s, px, py, pw);
    wpe_display_ps5_canvas_fill(c, px + 24, py + HEADER_H, pw - 48, 1, 0, PX_A(PX_LINE, 0xc0));

    const int rows = s->rowCount, visible = bui_visible_rows(screenH);
    const int rx = px + 24, rw = pw - 48 - 16;
    if (!rows) {
        const gboolean bookmarks = s->section == SEC_BOOKMARKS, downloads = s->section == SEC_DOWNLOADS;
        const int cx = px + pw / 2, cy = (LIST_TOP + listBottom(screenH)) / 2;
        textCentred(c, cx, cy - 72, 0, 64, FALSE, PX_TEXT_3, bookmarks ? "☆" : downloads ? "↓" : "↺");
        textCentred(c, cx, cy + 6, 0, 30, TRUE, PX_TEXT_2, bookmarks ? "No bookmarks yet" : downloads ? "No downloads yet" : "No history yet");
        textCentred(c, cx, cy + 52, pw - 96, 22, FALSE, PX_TEXT_3,
            bookmarks ? "☆ in the bar (Options, then up and right) adds the page shown."
            : downloads ? "Files a page can't show (archives, packages) are saved to /data/ps5webkit/downloads." : "The pages you visit come here.");
    }
    int ry = LIST_TOP;
    for (int row = s->listTop; row < rows && row < s->listTop + visible; row++) {
        BUIRow r;
        memset(&r, 0, sizeof r);
        s->row(row, &r, s->user);
        drawRow(c, s, &r, rx, ry, rw, s->zone == ZONE_LIST && row == s->listFocus);
        addHot(rx, ry, rw, ROW_BOX, HOT_ROW, row);
        ry += ROW_H;
    }
    if (rows > visible) {
        /* a scroll bar on the right */
        const int trackX = px + pw - 26, trackY = LIST_TOP, trackH = visible * ROW_H - (ROW_H - ROW_BOX);
        int thumbH = trackH * visible / rows;
        if (thumbH < 32)
            thumbH = 32;
        const int thumbY = trackY + (trackH - thumbH) * s->listTop / (rows - visible);
        wpe_display_ps5_canvas_fill(c, trackX, trackY, 4, trackH, 2, PX_A(PX_LINE, 0xc0));
        wpe_display_ps5_canvas_fill(c, trackX, thumbY, 4, thumbH, 2, PX_TEXT_3);
    }
    if (s->details)
        drawDetails(c, s->details, px, pw, screenH);
    drawFooter(c, s, px, py, pw, ph);
}
