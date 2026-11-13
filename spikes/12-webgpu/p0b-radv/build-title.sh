#!/usr/bin/env bash
# Build Spike 12 P0b/P0c (RADV beside ps5-opengl) as a registered title.
#   spikes/12-webgpu/p0b-radv/build-title.sh [TITLE_ID]
# Needs build/radv-ps5/install from port/deps/build.sh radv.
# RADV and ps5-opengl each carry a Mesa 26.2 (4163 global symbols in common):
# RADV is linked into one relocatable object first and every symbol but the
# ICD's three entry points made local, so the two never meet at link time.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
. "$ROOT/env.sh"
HERE="$ROOT/spikes/12-webgpu/p0b-radv"
TITLE="${1:-PPSA99012}"
PREFIX="$ROOT/toolchain/ps5-opengl-gles-noprof-m8"
RADV="$ROOT/build/radv-ps5"
LLVM=/opt/homebrew/opt/llvm@18/bin
OBJ="$HERE/build/obj"; mkdir -p "$OBJ"
[[ -f $RADV/install/lib/libvulkan_radeon.ps5.a ]] || { echo "run port/deps/build.sh radv first" >&2; exit 1; }

"$ROOT/port/deps/radv/radv-object.sh"

for s in tri.vert tri.frag fill.comp; do
    glslangValidator -V --quiet --vn "${s/./_}" -o "$OBJ/${s/./_}.h" "$HERE/shaders/$s"
done
CF=(-O2 -g -Wall -Werror -ffunction-sections -fdata-sections -I"$ROOT/spikes" -I"$HERE/src" -I"$OBJ"
    -DLOG_HOST="\"$PS5WK_LOG_HOST\"" -DLOG_PORT="$PS5WK_LOG_PORT")
COMMON="$ROOT/port/webkit/apps/common"
prospero-clang -std=gnu11 "${CF[@]}" -I"$COMMON" -c "$HERE/src/main.c" -o "$OBJ/main.o"
prospero-clang -std=gnu11 "${CF[@]}" -I"$PREFIX/include" -c "$HERE/src/gl_side.c" -o "$OBJ/gl_side.o"
prospero-clang -std=gnu11 "${CF[@]}" -I"$RADV/install/include" -c "$HERE/src/vk_side.c" -o "$OBJ/vk_side.o"
prospero-clang -std=c11 "${CF[@]}" -c "$HERE/src/cpu_model.c" -o "$OBJ/cpu_model.o"
prospero-clang -std=c11 "${CF[@]}" -c "$COMMON/crashguard.c" -o "$OBJ/crashguard.o"
prospero-clang -std=c11 "${CF[@]}" -I"$PREFIX/include" -c "$ROOT/spikes/common/psvr2/psvr2.c" -o "$OBJ/psvr2.o"
prospero-clang -std=c11 "${CF[@]}" -c "$ROOT/spikes/common/title_exit.c" -o "$OBJ/title_exit.o"
PS5_CXXLIB="$ROOT/toolchain/libcxx19/install/lib" "$HERE/package-title.sh" -p "$PREFIX" -t "$TITLE" \
    -n "ps5webkit spike 12 RADV" -o "$HERE/build/titles" \
    "$OBJ/main.o" "$OBJ/gl_side.o" "$OBJ/vk_side.o" "$OBJ/crashguard.o" "$OBJ/psvr2.o" "$OBJ/title_exit.o" \
    "$OBJ/cpu_model.o" "$RADV/radv-local.o" "$ROOT/toolchain/wpe-sysroot/lib/libz.a"
