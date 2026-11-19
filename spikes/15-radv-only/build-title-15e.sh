#!/usr/bin/env bash
# Build Spike 15e (images shared between ANGLE's VkDevice and a second one) as a registered title.
#   spikes/15-radv-only/build-title-15e.sh [TITLE_ID]
# Needs build/angle-vk-ps5/lib/libangle_vk_ps5.a (build-angle-vk.sh) and
# build/radv-ps5/install (port/deps/build.sh radv).
# Output: spikes/15-radv-only/build/titles/<TITLE_ID> (default PPSA99024).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/env.sh"
HERE="$ROOT/spikes/15-radv-only"
TITLE="${1:-PPSA99024}"
ANGLE="$ROOT/build/angle-vk-ps5"
RADV="$ROOT/build/radv-ps5"
LIBCXX="$ROOT/toolchain/libcxx19/install"
[[ -f $ANGLE/lib/libangle_vk_ps5.a ]] || { echo "run build-angle-vk.sh first" >&2; exit 1; }
[[ -f $RADV/install/lib/libvulkan_radeon.ps5.a ]] || { echo "run port/deps/build.sh radv first" >&2; exit 1; }

# RADV as one object with only the ICD's entry points global. With no
# ps5-opengl in the link the localisation isn't needed, but the object also
# nulls the weak entry points the title converter would refuse.
"$ROOT/port/deps/radv/radv-object.sh"

OBJ="$HERE/build/obj-15e"; mkdir -p "$OBJ"
CF=(-O2 -g -Wall -Werror -ffunction-sections -fdata-sections -I"$ROOT/spikes")
COMMON="$ROOT/port/webkit/apps/common"   # the browser's crash guard
prospero-clang -std=c11 "${CF[@]}" -DSPIKE15_E -I"$COMMON" -c "$HERE/src/main.c" -o "$OBJ/main.o"
prospero-clang -std=c11 "${CF[@]}" -c "$HERE/src/radv_title.c" -o "$OBJ/radv_title.o"
prospero-clang -std=c11 "${CF[@]}" -c "$COMMON/crashguard.c" -o "$OBJ/crashguard.o"
prospero-clang -std=c11 "${CF[@]}" -c "$ROOT/spikes/common/title_exit.c" -o "$OBJ/title_exit.o"
prospero-clang -std=c11 "${CF[@]}" -c "$ROOT/spikes/common/psvr2/psvr2.c" -o "$OBJ/psvr2.o"
prospero-clang++ -std=c++20 "${CF[@]}" -Wno-unused-function -nostdinc++ -isystem "$LIBCXX/include/c++/v1" \
    -I"$ANGLE/src/include" -I"$ANGLE/src/src" -I"$RADV/install/include" -c "$HERE/src/vk_share_15e.cpp" -o "$OBJ/vk_share_15e.o"
PS5_CXXLIB="$LIBCXX/lib" "$HERE/package-title.sh" -t "$TITLE" \
    -n "ps5webkit spike 15e Vulkan sharing" -o "$HERE/build/titles" \
    "$OBJ/main.o" "$OBJ/radv_title.o" "$OBJ/vk_share_15e.o" "$OBJ/crashguard.o" "$OBJ/title_exit.o" "$OBJ/psvr2.o" \
    "$ANGLE/lib/libangle_vk_ps5.a" "$RADV/radv-local.o" "$ROOT/toolchain/wpe-sysroot/lib/libz.a"
