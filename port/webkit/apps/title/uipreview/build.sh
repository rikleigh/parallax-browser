#!/usr/bin/env bash
# uipreview: build the browser's UI drawing for the Mac and render every
# screen to PNG (menu lists, focus, loading bar, notices), with the draw
# cost. No console, no WebKit build: PS5UICanvas.cpp, PS5Text.cpp and
# browserdraw.c with stand-ins for the renderer and the platform header.
#
#   port/webkit/apps/title/uipreview/build.sh [out-dir]   (default: build/uipreview/out)
#
# Needs Homebrew's glib, freetype and libpng; the Noto fonts from
# toolchain/wpe-deps/dl/fonts.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TITLE="$(cd "$HERE/.." && pwd)"
PORT="$(cd "$TITLE/../.." && pwd)"
ROOT="$(cd "$PORT/../.." && pwd)"
B="${UIPREVIEW_BUILD:-$ROOT/build/uipreview}"
OUTDIR="${1:-$B/out}"
FONTS="${FONTS:-$ROOT/toolchain/wpe-deps/dl/fonts}"
BG="${BG:-$PORT/test/title-04-en.wikipedia.org-10s.png}"
mkdir -p "$B/src/wpe/ps5" "$OUTDIR"

# The canvas API from the real header, as a plain C header.
{
    echo '#pragma once'
    echo '#include <glib.h>'
    echo 'G_BEGIN_DECLS'
    echo 'typedef struct _WPEDisplayPS5 WPEDisplayPS5;'
    echo '#define WPE_API'
    echo '#define WPE_IS_DISPLAY_PS5(x) ((x) != NULL)'
    sed -n '/^typedef struct _WPEDisplayPS5Canvas/,/^\/\* UI mode/p' "$PORT/wpe-ps5/WPEDisplayPS5.h" | sed '$d'
    echo 'G_END_DECLS'
} > "$B/src/wpe/ps5/wpe-ps5.h"
echo '#include <wpe/ps5/wpe-ps5.h>' > "$B/src/WPEDisplayPS5.h"
: > "$B/src/config.h"
cat > "$B/src/PS5Renderer.h" <<'H'
#pragma once
#include <stdint.h>
class PS5Renderer {
public:
    static PS5Renderer& singleton();
    void setUILayer(const uint8_t* bgra, int x, int y, int width, int height);
};
H
# Copies, so their quoted includes find the stand-ins.
cp "$PORT/wpe-ps5/PS5UICanvas.cpp" "$PORT/wpe-ps5/PS5Text.cpp" "$PORT/wpe-ps5/PS5Text.h" "$B/src/"

# The logo as the title ships it (package.sh).
[[ $B/parallax-mark.bgra -nt $TITLE/sce_sys/icon0.png && $B/parallax-mark.bgra -nt $TITLE/brand-assets.py ]] ||
    python3 "$TITLE/brand-assets.py" mark 48 "$B/parallax-mark.bgra"

PKG="$(pkg-config --cflags glib-2.0 freetype2 libpng)"
LIBS="$(pkg-config --libs glib-2.0 freetype2 libpng)"
DEFS=(-DPS5WK_FONT_DIR="\"$FONTS\"")
c++ -std=c++20 -O2 -I"$B/src" $PKG "${DEFS[@]}" -c "$B/src/PS5UICanvas.cpp" -o "$B/canvas.o"
c++ -std=c++20 -O2 -I"$B/src" $PKG "${DEFS[@]}" -c "$B/src/PS5Text.cpp" -o "$B/text.o"
c++ -std=c++20 -O2 -I"$B/src" $PKG -c "$HERE/uipreview-glue.cpp" -o "$B/glue.o"
cc -std=gnu11 -O2 -Wall -I"$B/src" -I"$TITLE" $PKG -DBUI_MARK_PATH="\"$B/parallax-mark.bgra\"" -c "$TITLE/browserdraw.c" -o "$B/draw.o"
cc -std=gnu11 -O2 -Wall -I"$B/src" -I"$TITLE" $PKG -c "$HERE/uipreview.c" -o "$B/uipreview.o"
c++ "$B"/{canvas,text,glue,draw,uipreview}.o $LIBS -o "$B/uipreview"
"$B/uipreview" "$OUTDIR" "$BG"
