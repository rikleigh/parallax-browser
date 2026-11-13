#!/usr/bin/env bash
# Build Spike 12 P0d (the GL probe for Dawn's GLES backend) as a registered
# title, against the GL driver the browser ships.
#   spikes/12-webgpu/p0d-glprobe/build-title.sh [VARIANT [TITLE_ID]]
# VARIANT names toolchain/ps5-opengl-<VARIANT> (default gles-noprof-m8).
# Output: spikes/12-webgpu/p0d-glprobe/build/titles/<TITLE_ID> (default
# PPSA99016). The Mac's address for the TCP log is compiled in from env.sh
# (PS5WK_LOG_HOST; the file log on the console is written either way).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
. "$ROOT/env.sh"
HERE="$ROOT/spikes/12-webgpu/p0d-glprobe"
VARIANT="${1:-gles-noprof-m8}" TITLE="${2:-PPSA99016}"
PREFIX="$ROOT/toolchain/ps5-opengl-$VARIANT"
[[ -f $PREFIX/lib/libPS5OpenGL.a ]] || { echo "no driver at $PREFIX" >&2; exit 1; }
OBJ="$HERE/build/obj-$VARIANT"; mkdir -p "$OBJ"
CF=(-O2 -g -Wall -Werror -ffunction-sections -fdata-sections -I"$ROOT/spikes"
    -DLOG_HOST="\"$PS5WK_LOG_HOST\"" -DLOG_PORT="$PS5WK_LOG_PORT")
COMMON="$ROOT/port/webkit/apps/common"   # the browser's crash guard
prospero-clang -std=gnu11 "${CF[@]}" -I"$COMMON" -c "$HERE/src/main_title.c" -o "$OBJ/main_title.o"
prospero-clang -std=c11 "${CF[@]}" -I"$PREFIX/include" -c "$HERE/src/glprobe.c" -o "$OBJ/glprobe.o"
prospero-clang -std=c11 "${CF[@]}" -I"$PREFIX/include" -c "$HERE/src/glprobe_gpu.c" -o "$OBJ/glprobe_gpu.o"
prospero-clang -std=c11 "${CF[@]}" -c "$COMMON/crashguard.c" -o "$OBJ/crashguard.o"
prospero-clang -std=c11 "${CF[@]}" -I"$PREFIX/include" -c "$ROOT/spikes/common/psvr2/psvr2.c" -o "$OBJ/psvr2.o"
prospero-clang -std=c11 "${CF[@]}" -c "$ROOT/spikes/common/title_exit.c" -o "$OBJ/title_exit.o"
"$ROOT/spikes/02-gl-egl/ps5-opengl/package-title.sh" -p "$PREFIX" -t "$TITLE" \
    -n "ps5webkit spike 12 GL probe" -o "$HERE/build/titles" \
    "$OBJ/main_title.o" "$OBJ/glprobe.o" "$OBJ/glprobe_gpu.o" "$OBJ/crashguard.o" "$OBJ/psvr2.o" "$OBJ/title_exit.o"
