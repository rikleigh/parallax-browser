#!/usr/bin/env bash
# Build Spike 12 P1 (Dawn on RADV beside ps5-opengl) as a registered title.
# The RADV object and the GL side are P0b's (run its build-title.sh once).
#   spikes/12-webgpu/p1-dawn-title/build-title.sh [TITLE_ID]
# Needs build/radv-ps5/install from port/deps/build.sh radv.
# RADV and ps5-opengl each carry a Mesa 26.2 (4163 global symbols in common):
# RADV is linked into one relocatable object first and every symbol but the
# ICD's three entry points made local, so the two never meet at link time.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
. "$ROOT/env.sh"
HERE="$ROOT/spikes/12-webgpu/p1-dawn-title"; P0B="$ROOT/spikes/12-webgpu/p0b-radv"
TITLE="${1:-PPSA99012}"
PREFIX="$ROOT/toolchain/ps5-opengl-gles-noprof-m8"
RADV="$ROOT/build/radv-ps5"
LLVM=/opt/homebrew/opt/llvm@18/bin
OBJ="$HERE/build/obj"; mkdir -p "$OBJ"
[[ -f $RADV/install/lib/libvulkan_radeon.ps5.a ]] || { echo "run port/deps/build.sh radv first" >&2; exit 1; }

"$ROOT/port/deps/radv/radv-object.sh"

CF=(-O2 -g -Wall -Werror -ffunction-sections -fdata-sections -I"$ROOT/spikes" -I"$HERE/src" -I"$P0B/src"
    -DLOG_HOST="\"$PS5WK_LOG_HOST\"" -DLOG_PORT="$PS5WK_LOG_PORT")
COMMON="$ROOT/port/webkit/apps/common"
prospero-clang -std=gnu11 "${CF[@]}" -I"$COMMON" -c "$HERE/src/main.c" -o "$OBJ/main.o"
prospero-clang -std=gnu11 "${CF[@]}" -I"$PREFIX/include" -c "$P0B/src/gl_side.c" -o "$OBJ/gl_side.o"
prospero-clang++ -std=c++20 "${CF[@]}" -fno-exceptions -fno-rtti -nostdinc++ -isystem "$ROOT/toolchain/libcxx19/install/include/c++/v1" \
    -I"$ROOT/toolchain/wpe-sysroot/include" -c "$HERE/src/dawn_side.cpp" -o "$OBJ/dawn_side.o"
prospero-clang -std=c11 "${CF[@]}" -c "$P0B/src/cpu_model.c" -o "$OBJ/cpu_model.o"
prospero-clang -std=c11 "${CF[@]}" -c "$COMMON/crashguard.c" -o "$OBJ/crashguard.o"
prospero-clang -std=c11 "${CF[@]}" -I"$PREFIX/include" -c "$ROOT/spikes/common/psvr2/psvr2.c" -o "$OBJ/psvr2.o"
prospero-clang -std=c11 "${CF[@]}" -c "$ROOT/spikes/common/title_exit.c" -o "$OBJ/title_exit.o"
PS5_CXXLIB="$ROOT/toolchain/libcxx19/install/lib" "$P0B/package-title.sh" -p "$PREFIX" -t "$TITLE" \
    -n "ps5webkit spike 12 Dawn" -o "$HERE/build/titles" \
    "$OBJ/main.o" "$OBJ/gl_side.o" "$OBJ/dawn_side.o" "$OBJ/crashguard.o" "$OBJ/psvr2.o" "$OBJ/title_exit.o" \
    "$OBJ/cpu_model.o" "$RADV/radv-local.o" "$ROOT/toolchain/wpe-sysroot/lib/libwebgpu_dawn.a" "$ROOT/toolchain/wpe-sysroot/lib/libz.a" \
    --wrap=dlopen --wrap=dlsym --wrap=dlclose --defsym=__cxa_thread_atexit_impl=0  # weak in libc++abi (it has a fallback); the converter refuses undefined symbols
