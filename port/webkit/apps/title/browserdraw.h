/*
 * Drawing the browser's own UI (browserui.c): the address bar (also the
 * loading bar) and the Options menu, into a WPEDisplayPS5Canvas. Only
 * drawing: browserui.c fills a BUIState from the tabs, bookmarks, history
 * and the menu's focus, and keeps the pad and the menu's logic. Kept apart
 * so the same code renders on the Mac (apps/title/uipreview) for design
 * work. The palette and sizes are in brand.h.
 */
#pragma once

#include <glib.h>
#include <wpe/ps5/wpe-ps5.h>

enum { TB_BACK, TB_FORWARD, TB_RELOAD, TB_HOME, TB_ADDRESS, TB_STAR, TB_NEWTAB, TB_SETTINGS, TB_COUNT };
enum { SEC_TABS, SEC_BOOKMARKS, SEC_HISTORY, SEC_DOWNLOADS, SEC_COUNT };
enum { ZONE_TOOLBAR, ZONE_LIST };
enum { HOT_TOOLBAR, HOT_SECTION, HOT_ROW };

/* Where the pointer can pick something (filled by the last draw). */
typedef struct {
    int x, y, w, h;
    int kind, index;
} BUIHot;

/* One row of the menu's list. */
typedef struct {
    const char* title;
    const char* url;
    char note[64];    /* on the right: "kept live", the time of a visit */
    int number;       /* the tab's number (1-based), 0: none */
    gboolean current; /* the tab shown */
    gboolean action;  /* "+ New tab", "Clear all history" */
    gboolean armed;   /* "Clear all history" pressed once */
    /* the Downloads list: title = the file's name, url = the address it came from */
    char kind[8];      /* the file-type tile: "ZIP", "ELF", "PKG" */
    double progress;   /* 0..1 for the bar under the name; < 0: no bar */
    gboolean running;  /* receiving: the bar's head glows */
    char detail[112];  /* "312 MB of 1.2 GB · 18.4 MB/s · 48 s left" */
    char state[64];    /* on the right: "Paused", "Failed: connection lost" */
    int tone;          /* the state's colour: BUI_TONE_* */
} BUIRow;

enum { BUI_TONE_PLAIN, BUI_TONE_ACTIVE, BUI_TONE_WARN, BUI_TONE_DONE };

/* The details card over the list (a download's): label and value lines. */
typedef struct {
    const char* title;
    const char* labels[10];
    const char* values[10];
    int count;
} BUIDetails;

typedef struct {
    int screenW, screenH;
    /* the bar */
    gboolean loading;
    double progress; /* 0..1 */
    gboolean canBack, canForward, canNewTab, starred;
    const char* title;
    const char* uri;
    unsigned tabCount, maxTabs;
    /* the menu */
    int zone, toolbarFocus, section;
    unsigned bookmarkCount, historyCount, downloadCount;
    /* downloads: how many are receiving (the bar's ring) and their progress
     * together (0..1, < 0: sizes not known) */
    unsigned downloadsActive;
    double downloadsProgress;
    /* the footer in the Downloads list: what X and Square do for the row
     * focused (NULL: nothing) */
    const char* downloadCross;
    const char* downloadSquare;
    /* a details card over the list, or NULL */
    const BUIDetails* details;
    int rowCount, listTop, listFocus;
    void (*row)(int index, BUIRow* out, void* user);
    void* user;
} BUIState;

int bui_bar_height(void);
int bui_row_height(void);
/* How many rows the menu's list shows. */
int bui_visible_rows(int screenH);

/* The bar, @interactive: in the menu (focus, pointer targets), else the
 * loading bar. The canvas is bui_bar_height() high (or the menu's). */
void bui_draw_bar(WPEDisplayPS5Canvas* canvas, const BUIState* state, gboolean interactive);
/* The whole menu on a screen-sized canvas: dimmed page, bar, lists. */
void bui_draw_menu(WPEDisplayPS5Canvas* canvas, const BUIState* state);

/* The pointer target under (x, y) from the last bui_draw_menu, or NULL. */
const BUIHot* bui_hot_at(double x, double y);
