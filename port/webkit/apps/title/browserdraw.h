/*
 * Drawing the browser's own UI (browserui.c): the bar (above the page, also
 * the loading bar in full screen) and the Options menu, into a
 * WPEDisplayPS5Canvas. Only
 * drawing: browserui.c fills a BUIState from the tabs, bookmarks, history
 * and the menu's focus, and keeps the pad and the menu's logic. Kept apart
 * so the same code renders on the Mac (apps/title/uipreview) for design
 * work. The palette and sizes are in brand.h.
 */
#pragma once

#include <glib.h>
#include <wpe/ps5/wpe-ps5.h>

enum { TB_BACK, TB_FORWARD, TB_RELOAD, TB_HOME, TB_ADDRESS, TB_TABS, TB_STAR, TB_FULLSCREEN, TB_GAMEPAD, TB_PRIVATE, TB_SAFER, TB_NEWTAB, TB_SETTINGS, TB_COUNT };
enum { SEC_TABS, SEC_BOOKMARKS, SEC_HISTORY, SEC_DOWNLOADS, SEC_FILES, SEC_COUNT };
enum { ZONE_TOOLBAR, ZONE_LIST };
enum { HOT_TOOLBAR, HOT_SECTION, HOT_ROW, HOT_CHOICE };

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
    /* the Downloads and Files lists: title = the file's name, url = the
     * address it came from (Files: NULL) */
    char kind[8];      /* the file-type tile: "ZIP", "ELF", "PKG", "DIR" */
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

/* A dialog over the list: what X on a download or a file offers (its
 * owner, downloads.c or files.c, fills it; browserui.c moves the focus). */
#define BUI_CHOICES_MAX 12
typedef struct {
    char label[96];
    char detail[160]; /* a line under it ("" none) */
    int tone;         /* BUI_TONE_ACTIVE: suggested; BUI_TONE_WARN: replaces or deletes */
    int id;           /* the owner's */
    gboolean confirm; /* done on a second press (the first arms it) */
} BUIChoice;

typedef struct {
    char title[200];
    char lines[3][200]; /* under the title: where it is, what's inside */
    int lineTones[3];
    int lineCount;
    BUIChoice choices[BUI_CHOICES_MAX];
    int count;
    int focus; /* index */
    int armed; /* index pressed once (confirm), -1 none */
} BUIChoices;

/* What a choice did (the dialog's owner tells browserui.c). */
enum {
    BUI_CHOSEN_CLOSE,      /* the dialog closes */
    BUI_CHOSEN_STAY,       /* it stays (a choice armed, or nothing done) */
    BUI_CHOSEN_FILES,      /* the Files list (it was set up) */
    BUI_CHOSEN_DOWNLOADS,  /* the Downloads list */
    BUI_CHOSEN_DETAILS,    /* the row's details card */
    BUI_CHOSEN_MENU_CLOSE, /* done: back to the page (a download's folder chosen) */
};

typedef struct {
    int screenW, screenH;
    /* the bar */
    gboolean loading;
    double progress; /* 0..1 */
    gboolean canBack, canForward, canNewTab, starred;
    /* gamepad mode on for the site shown; private browsing (the bar's tint) */
    gboolean gamepadOn, privateMode;
    /* Safer browsing on (no JIT, WebAssembly or WebGL): the shield filled */
    gboolean saferOn;
    /* full screen chosen (the address bar hidden); fullScreenWhy: why the
     * page is in full screen without it ("the page": a video), or NULL */
    gboolean fullScreen;
    const char* fullScreenWhy;
    const char* title;
    const char* uri;
    unsigned tabCount, maxTabs;
    /* the menu; userName: whose data it shows (the PS5 user's name) */
    const char* userName;
    int zone, toolbarFocus, section;
    unsigned bookmarkCount, historyCount, downloadCount;
    /* downloads: how many are receiving (the bar's ring) and their progress
     * together (0..1, < 0: sizes not known) */
    unsigned downloadsActive;
    double downloadsProgress;
    /* the footer in the Downloads and Files lists: what X, Square and R2
     * do for the row focused (NULL: nothing), and O (NULL: "Close") */
    const char* downloadCross;
    const char* downloadSquare;
    const char* listR2;
    const char* listCircle;
    const char* filesError; /* Files: the drives couldn't be listed (why) */
    /* the footer on the Home button: what Square does (NULL: nothing) */
    const char* homeSquare;
    /* a details card over the list, or NULL */
    const BUIDetails* details;
    /* a dialog over the list, or NULL */
    const BUIChoices* choices;
    int rowCount, listTop, listFocus;
    void (*row)(int index, BUIRow* out, void* user);
    void* user;
} BUIState;

int bui_bar_height(void);
int bui_row_height(void);
/* How many rows the menu's list shows. */
int bui_visible_rows(int screenH);
/* How many choices a dialog shows at once (it scrolls to the focus). */
int bui_visible_choices(int screenH, const BUIChoices* choices);

/* The bar, @interactive: in the menu or above the page (focus: zone
 * ZONE_TOOLBAR and toolbarFocus, -1 none; pointer targets), else the
 * loading bar. The canvas is bui_bar_height() high (or the menu's). */
void bui_draw_bar(WPEDisplayPS5Canvas* canvas, const BUIState* state, gboolean interactive);
/* The whole menu on a screen-sized canvas: dimmed page, bar, lists. */
void bui_draw_menu(WPEDisplayPS5Canvas* canvas, const BUIState* state);

/* The pointer target under (x, y) from the last bui_draw_menu, or NULL. */
const BUIHot* bui_hot_at(double x, double y);

/* ---- View in VR (viewinvr.c): the player panel in the headset and the
 * card on the TV meanwhile. */
typedef struct {
    const char* title;      /* the video's (the page's title) */
    const char* projection; /* "180° 3D side by side" */
    const char* message;    /* a line under the title (why this projection, "Recentred"); NULL: none */
    gboolean controls;      /* the page's video element was found: play/pause and seeking */
    gboolean paused;
    double time;            /* seconds */
    double duration;        /* seconds; not finite or <= 0: live or not known (no seek bar) */
    double buffered;        /* buffered up to (seconds), < 0: not known */
    gboolean seeking;       /* a seek target being chosen (not sent yet) */
    double target;          /* its time */
} BUIVRState;

/* The panel's canvas size: the headset shows it head-locked, below the
 * view's centre (wpe-ps5 PS5RendererVR.cpp). */
#define BUI_VR_PANEL_W 1240
#define BUI_VR_PANEL_H 400
void bui_draw_vr_panel(WPEDisplayPS5Canvas* canvas, const BUIVRState* state);
/* The TV's card while the video plays in the headset (shown centred on black). */
#define BUI_VR_TV_W 1400
#define BUI_VR_TV_H 560
void bui_draw_vr_tv(WPEDisplayPS5Canvas* canvas, const BUIVRState* state);
/* "1:23", "1:02:03"; "--:--" when not known. */
void bui_format_time(double seconds, char* out, size_t size);
