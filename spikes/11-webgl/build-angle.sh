#!/usr/bin/env bash
# Build ANGLE (WebKit's vendored copy) for the PS5 title, out of tree.
#   spikes/11-webgl/build-angle.sh [--clean]
# Source: a copy of toolchain/wpe-webkit/src/Source/ThirdParty/ANGLE in
# build/angle-ps5/src (git-ignored) with patches/*.patch applied; the WebKit
# tree itself is never touched. Output: build/angle-ps5/lib/libangle_ps5.a.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/env.sh"
HERE="$ROOT/spikes/11-webgl"
ANGLE_ORIG="$ROOT/toolchain/wpe-webkit/src/Source/ThirdParty/ANGLE"
W="$ROOT/build/angle-ps5"; SRC="$W/src"; BLD="$W/build"
JOBS="${JOBS:-8}"

if [[ ${1:-} == --clean || ! -f $SRC/.ps5-patched ]]; then
    rm -rf "$SRC" "$BLD"
    mkdir -p "$W"
    echo "==> copying $ANGLE_ORIG -> $SRC"
    rsync -a --exclude .git "$ANGLE_ORIG/" "$SRC/"
    for p in "$HERE"/patches/*.patch; do
        echo "==> patch $(basename "$p")"
        patch -p1 -s -d "$SRC" < "$p"
    done
    touch "$SRC/.ps5-patched"
fi

if [[ ! -f $BLD/build.ninja ]]; then
    cmake -G Ninja -S "$HERE/angle" -B "$BLD" \
        -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DANGLE_SRC="$SRC" -DLIBCXX="$ROOT/toolchain/libcxx19/install" \
        -DZLIB_INCLUDE="$ROOT/toolchain/wpe-sysroot/include" \
        -DCMAKE_VERBOSE_MAKEFILE=OFF > "$W/configure.log" 2>&1 || { tail -30 "$W/configure.log"; exit 1; }
fi
ninja -C "$BLD" -j"$JOBS" angle_ps5 2>&1 | tee "$W/build.log" | grep -E "error|^\[|FAILED" | tail -40
mkdir -p "$W/lib"
cp "$BLD/libangle_ps5.a" "$W/lib/"
ls -la "$W/lib/libangle_ps5.a"
