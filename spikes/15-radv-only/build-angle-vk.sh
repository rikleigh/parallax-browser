#!/usr/bin/env bash
# Spike 15a: build ANGLE's Vulkan backend for the PS5 title, out of tree.
#   spikes/15-radv-only/build-angle-vk.sh [--clean]
# Source: a copy of toolchain/wpe-webkit/src-webgl/Source/ThirdParty/ANGLE
# (WebKit's vendored ANGLE with the WebGL build's PS5 patches) in
# build/angle-vk-ps5/src (git-ignored), with patches/*.patch applied; the
# WebKit tree itself is never touched. Output:
# build/angle-vk-ps5/lib/libangle_vk_ps5.a.
#
# Needs: RADV built (port/deps/build.sh radv) for its Vulkan headers; Dawn's
# source (port/deps/build.sh dawn) for SPIRV-Headers and SPIRV-Tools headers;
# VulkanMemoryAllocator at ANGLE's DEPS pin, fetched here once.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/env.sh"
HERE="$ROOT/spikes/15-radv-only"
ANGLE_ORIG="$ROOT/toolchain/wpe-webkit/src-webgl/Source/ThirdParty/ANGLE"
W="$ROOT/build/angle-vk-ps5"; SRC="$W/src"; BLD="$W/build"
DAWN_TP="$ROOT/toolchain/wpe-deps/src/dawn/third_party"
VULKAN_INCLUDE="$ROOT/build/radv-ps5/install/include"
VMA="$W/third_party/vma"
VMA_REV=7e55b011e16182fc349149abbd3aaf3b1db46421   # ANGLE DEPS
JOBS="${JOBS:-8}"

for d in "$ANGLE_ORIG/src/libANGLE/renderer/vulkan" "$VULKAN_INCLUDE/vulkan" \
         "$DAWN_TP/spirv-headers/src/include/spirv" "$DAWN_TP/spirv-tools/src/include/spirv-tools"; do
    [[ -d $d ]] || { echo "missing $d" >&2; exit 1; }
done

if [[ ! -f $VMA/include/vk_mem_alloc.h ]]; then
    echo "==> fetching VulkanMemoryAllocator $VMA_REV"
    rm -rf "$VMA"; mkdir -p "$VMA"
    git -C "$VMA" init -q
    git -C "$VMA" fetch -q --depth 1 \
        https://chromium.googlesource.com/external/github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator "$VMA_REV"
    git -C "$VMA" checkout -q FETCH_HEAD
fi

if [[ ${1:-} == --clean || ! -f $SRC/.ps5-patched ]]; then
    rm -rf "$SRC" "$BLD"
    mkdir -p "$W"
    echo "==> copying $ANGLE_ORIG -> $SRC"
    rsync -a --exclude .git --exclude third_party/VK-GL-CTS --exclude third_party/OpenCL-CTS \
        "$ANGLE_ORIG/" "$SRC/"
    for p in "$HERE"/patches/*.patch; do
        echo "==> patch $(basename "$p")"
        patch -p1 -s -d "$SRC" < "$p"
    done
    python3 "$HERE/angle/gen-vulkan-cmake.py" "$SRC" "$SRC/Vulkan.cmake"
    touch "$SRC/.ps5-patched"
fi

if [[ ! -f $BLD/build.ninja ]]; then
    cmake -G Ninja -S "$HERE/angle" -B "$BLD" \
        -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DANGLE_SRC="$SRC" -DLIBCXX="$ROOT/toolchain/libcxx19/install" \
        -DZLIB_INCLUDE="$ROOT/toolchain/wpe-sysroot/include" \
        -DVULKAN_INCLUDE="$VULKAN_INCLUDE" \
        -DSPIRV_HEADERS_INCLUDE="$DAWN_TP/spirv-headers/src/include" \
        -DSPIRV_TOOLS_INCLUDE="$DAWN_TP/spirv-tools/src/include" \
        -DVMA_DIR="$VMA" \
        -DPS5_EXTRA_SOURCES="$HERE/src/ps5_spirv_utils.cpp" \
        -DCMAKE_VERBOSE_MAKEFILE=OFF > "$W/configure.log" 2>&1 || { tail -30 "$W/configure.log"; exit 1; }
fi
set +e
ninja -C "$BLD" -k 0 -j"$JOBS" angle_vk_ps5 > "$W/build.log" 2>&1
rc=$?
set -e
{ grep -E "error:|FAILED" "$W/build.log" || true; } | sort | uniq -c | sort -rn | head -40
[[ $rc == 0 ]] || { echo "build failed (rc $rc), see $W/build.log" >&2; exit $rc; }
mkdir -p "$W/lib"
cp "$BLD/libangle_vk_ps5.a" "$W/lib/"
ls -la "$W/lib/libangle_vk_ps5.a"
