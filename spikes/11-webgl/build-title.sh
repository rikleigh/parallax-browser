#!/usr/bin/env bash
# Build Spike 11 (ANGLE over ps5-opengl) as a registered title.
#   spikes/11-webgl/build-title.sh [VARIANT [TITLE_ID]]
# VARIANT names toolchain/ps5-opengl-<VARIANT> (default gles-async, Spike 2b's
# fastest build; "gles" is the plain one). Needs build/angle-ps5/lib/libangle_ps5.a
# from build-angle.sh. Output: spikes/11-webgl/build/titles/<TITLE_ID>.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/env.sh"
HERE="$ROOT/spikes/11-webgl"
VARIANT="${1:-gles-async}" TITLE="${2:-PPSA99006}"
PREFIX="$ROOT/toolchain/ps5-opengl-$VARIANT"
ANGLE="$ROOT/build/angle-ps5"
LIBCXX="$ROOT/toolchain/libcxx19/install"
[[ -f $ANGLE/lib/libangle_ps5.a ]] || { echo "run build-angle.sh first" >&2; exit 1; }
OBJ="$HERE/build/obj-$VARIANT"; mkdir -p "$OBJ"
CF=(-O2 -g -Wall -Werror -ffunction-sections -fdata-sections -I"$ROOT/spikes")
# C side: ps5-opengl's headers (native bench, the egl* lookup table, title glue)
prospero-clang -std=c11 "${CF[@]}" -I"$PREFIX/include" -c "$HERE/src/main.c" -o "$OBJ/main.o"
prospero-clang -std=c11 "${CF[@]}" -I"$PREFIX/include" -c "$HERE/src/native_bench.c" -o "$OBJ/native_bench.o"
prospero-clang -std=c11 "${CF[@]}" -I"$PREFIX/include" -c "$HERE/src/egl_lookup.c" -o "$OBJ/egl_lookup.o"
prospero-clang -std=c11 "${CF[@]}" -I"$PREFIX/include" -c "$ROOT/spikes/common/psvr2/psvr2.c" -o "$OBJ/psvr2.o"
prospero-clang -std=c11 "${CF[@]}" -I"$PREFIX/include" -c "$ROOT/spikes/common/title_exit.c" -o "$OBJ/title_exit.o"
# C++ side: ANGLE's headers only (its EGL/GLES types differ from ps5-opengl's)
prospero-clang++ -std=c++20 "${CF[@]}" -Wno-unused-function -nostdinc++ -isystem "$LIBCXX/include/c++/v1" \
    -I"$ANGLE/src/include" -I"$ANGLE/src/src" -c "$HERE/src/angle_tests.cpp" -o "$OBJ/angle_tests.o"
PS5_CXXLIB="$LIBCXX/lib" "$ROOT/spikes/02-gl-egl/ps5-opengl/package-title.sh" -p "$PREFIX" -t "$TITLE" \
    -n "ps5webkit spike 11 ANGLE" -o "$HERE/build/titles" \
    "$OBJ/main.o" "$OBJ/angle_tests.o" "$OBJ/native_bench.o" "$OBJ/egl_lookup.o" \
    "$OBJ/psvr2.o" "$OBJ/title_exit.o" \
    "$ANGLE/lib/libangle_ps5.a" "$ROOT/toolchain/wpe-sysroot/lib/libz.a"
