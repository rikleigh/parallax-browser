/*
 * Parallax Browser: the palette and the UI's sizes, in one place.
 *
 * The colours are sampled from the artwork (sce_sys/icon0.png, pic0.png):
 * three glass panes, violet at the back, cyan-edged blue in the middle,
 * magenta at the front, on a near-black navy. The title's canvas UI
 * (browserdraw.c) and the notices (wpe-ps5/PS5Text.cpp) use them;
 * runtime/start.html has the same values as CSS variables (--ink, ...).
 * 0xAARRGGBB, not premultiplied.
 */
#pragma once

/* ---- sampled from the artwork */
#define PX_INK 0xff01001au     /* the background behind the panes */
#define PX_INDIGO 0xff3202c2u  /* the back pane's body */
#define PX_VIOLET 0xff6d0efdu  /* the back pane's lit edge */
#define PX_BLUE 0xff156bfdu    /* the middle pane's body */
#define PX_CYAN 0xff0ee3feu    /* the middle pane's edge */
#define PX_MAGENTA 0xfffc0ec6u /* the front pane's edge */

/* ---- surfaces, from the ink up (glass: drawn translucent over the page) */
#define PX_BASE 0xff07061fu      /* the bar */
#define PX_SURFACE 0xff0d0b2au   /* the menu's panel */
#define PX_RAISED 0xff161440u    /* rows, buttons */
#define PX_RAISED_HI 0xff211d56u /* the focused row or button */
#define PX_LINE 0xff2b2766u      /* hairlines, outlines */

/* ---- text (contrast on PX_SURFACE) */
#define PX_TEXT 0xfff4f2ffu     /* 17:1 */
#define PX_TEXT_2 0xffb9b5e0u   /* 9.8:1, secondary */
#define PX_TEXT_3 0xff8a86b8u   /* 5.6:1, hints and addresses */
#define PX_TEXT_OFF 0xff545080u /* disabled */
#define PX_PINK 0xffff5cd6u     /* magenta lightened for text (7:1): a starred page, a warning */
#define PX_CYAN_TEXT 0xff7fefffu /* cyan for text: the tab shown */
#define PX_PRIVATE_BAR 0xff1d0626u   /* the bar in private browsing (plum) */
#define PX_PRIVATE_BAR_2 0xff2a0a3au

/* ---- focus and selection: the back pane to the front one */
#define PX_FOCUS_FROM PX_VIOLET
#define PX_FOCUS_TO PX_MAGENTA

/* the colour with another alpha (0-255) */
#define PX_A(color, alpha) ((((guint32)(alpha) & 0xffu) << 24) | ((guint32)(color) & 0xffffffu))

/* ---- sizes (TV pixels, 1920x1080) */
#define PX_RADIUS_PANEL 24
#define PX_RADIUS 16      /* rows, buttons, fields */
#define PX_RADIUS_CHIP 10 /* tiles, key caps */
#define PX_FOCUS_RING 3   /* the focus outline's width */
#define PX_FOCUS_GLOW 18  /* how far the focus glow reaches */
