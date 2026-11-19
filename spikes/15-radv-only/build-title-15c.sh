#!/usr/bin/env bash
# Build Spike 15c (an Hmd2 eye buffer from a RADV VkImage, through ANGLE) as a registered title.
#   spikes/15-radv-only/build-title-15c.sh [TITLE_ID]
# Needs build/angle-vk-ps5/lib/libangle_vk_ps5.a (build-angle-vk.sh) and
# build/radv-ps5/install (port/deps/build.sh radv).
# Output: spikes/15-radv-only/build/titles/<TITLE_ID> (default PPSA99022).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/env.sh"
HERE="$ROOT/spikes/15-radv-only"
TITLE="${1:-PPSA99022}"
ANGLE="$ROOT/build/angle-vk-ps5"
RADV="$ROOT/build/radv-ps5"
LIBCXX="$ROOT/toolchain/libcxx19/install"
[[ -f $ANGLE/lib/libangle_vk_ps5.a ]] || { echo "run build-angle-vk.sh first" >&2; exit 1; }
[[ -f $RADV/install/lib/libvulkan_radeon.ps5.a ]] || { echo "run port/deps/build.sh radv first" >&2; exit 1; }

# RADV as one object with only the ICD's entry points global. With no
# ps5-opengl in the link the localisation isn't needed, but the object also
# nulls the weak entry points the title converter would refuse.
"$ROOT/port/deps/radv/radv-object.sh"

OBJ="$HERE/build/obj-15c"; mkdir -p "$OBJ"
CF=(-O2 -g -Wall -Werror -ffunction-sections -fdata-sections -I"$ROOT/spikes")
COMMON="$ROOT/port/webkit/apps/common"   # the browser's crash guard
prospero-clang -std=c11 "${CF[@]}" -DSPIKE15_C -I"$COMMON" -c "$HERE/src/main.c" -o "$OBJ/main.o"
prospero-clang -std=c11 "${CF[@]}" -c "$HERE/src/radv_title.c" -o "$OBJ/radv_title.o"
prospero-clang -std=c11 "${CF[@]}" -c "$COMMON/crashguard.c" -o "$OBJ/crashguard.o"
prospero-clang -std=c11 "${CF[@]}" -c "$ROOT/spikes/common/title_exit.c" -o "$OBJ/title_exit.o"
prospero-clang -std=c11 "${CF[@]}" -c "$ROOT/spikes/common/psvr2/psvr2.c" -o "$OBJ/psvr2.o"
prospero-clang++ -std=c++20 "${CF[@]}" -Wno-unused-function -nostdinc++ -isystem "$LIBCXX/include/c++/v1" \
    -I"$ANGLE/src/include" -I"$ANGLE/src/src" -I"$RADV/install/include" -c "$HERE/src/vr_tsharp.cpp" -o "$OBJ/vr_tsharp.o"
PS5_CXXLIB="$LIBCXX/lib" "$HERE/package-title.sh" -t "$TITLE" \
    -n "ps5webkit spike 15c VR T# on RADV" -o "$HERE/build/titles" \
    "$OBJ/main.o" "$OBJ/radv_title.o" "$OBJ/vr_tsharp.o" "$OBJ/crashguard.o" "$OBJ/title_exit.o" "$OBJ/psvr2.o" \
    "$ANGLE/lib/libangle_vk_ps5.a" "$RADV/radv-local.o" "$ROOT/toolchain/wpe-sysroot/lib/libz.a"
# PS VR2 titles declare it in param.json: Hmd2 refuses a title whose attribute
# words are 0 with 0x8a72001f (Spike 3b; the first 15c run). PS5VR's values,
# as Spikes 3b and 9 and the browser's package.sh take them.
python3 - "$HERE/build/titles/$TITLE/sce_sys/param.json" "$ROOT/ref/PS5VR/app/sce_sys/param.json" <<'PY'
import json, sys
p = json.load(open(sys.argv[1])); vr = json.load(open(sys.argv[2]))
for k in ("attribute", "attribute2", "attribute3"):
    p[k] = vr.get(k, 0)
open(sys.argv[1], "w").write(json.dumps(p, indent=2) + "\n")
print("param.json: attribute 0x%x attribute3 0x%x (PS VR2)" % (p["attribute"], p["attribute3"]))
PY
