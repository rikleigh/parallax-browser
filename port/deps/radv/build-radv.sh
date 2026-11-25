#!/usr/bin/env bash
# Build RADV (Mesa 26.2 with the PS5 winsys, mpereiraesaa/PS5_Mesa) as a
# static archive for the browser title, with OUR payload SDK and libc++.
#   port/deps/radv/build-radv.sh [--clean]      (port/deps/build.sh radv runs this and radv-object.sh)
# Came out of spike 12 (spikes/12-webgpu/p0b-radv/NOTES.md has the why of each choice).
# Adapted from ref/PS5_Vulkan/tools/build-radv.sh (release variant). Differences:
# our toolchain/ps5-payload-sdk instead of the PS5_Vulkan SDK fork; only the
# fork's platform headers (ps5platform/*.h: AGC, kernel, VideoOut
# declarations) are added to the include path.
# Output: build/radv-ps5/install/{lib/libvulkan_radeon.ps5.a,include/vulkan}
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
. "$ROOT/env.sh"
MESA_FORK="$ROOT/ref/PS5_Mesa"
MESA_REV=01d70579b8d410d6a482e36c6f184798ae4c83ad      # pinned by ref/PS5_Vulkan/tools/build-radv.sh
SDK_FORK="$ROOT/ref/mihawk-PS5_PayloadSDK"
SDK_FORK_REV=b83202be73e930050e00de0f1b4d9ec46c0391bf  # pinned by ref/PS5_Vulkan/tools/setup-native-dependencies.sh
W="$ROOT/build/radv-ps5"; SRC="$W/src"; BLD="$W/build"; CLC="$W/clc-build"; INST="$W/install"
VENV="$W/venv"    # meson 1.10.1, mako, pyyaml, packaging (as Spike 2's ps5-opengl build had)
BREW=/opt/homebrew
if [[ ! -x $VENV/bin/meson ]]; then
    echo "python venv $VENV (meson 1.10.1, mako, pyyaml, packaging)"
    mkdir -p "$W"
    py=$(command -v python3.13 || command -v python3.12 || command -v python3)
    "$py" -m venv "$VENV"
    "$VENV/bin/pip" install -q meson==1.10.1 mako pyyaml packaging
fi
export PATH="$VENV/bin:$BREW/opt/bison/bin:$PATH"

if [[ ${1:-} == --clean ]]; then rm -rf "$SRC" "$BLD" "$CLC" "$W/sdkfork"; fi
mkdir -p "$W"
# The fork at its pinned revision, then port/deps/patches/radv/*.patch on top.
# The stamp holds both: a new or changed patch extracts the source again.
PATCHES="$ROOT/port/deps/patches/radv"
STAMP="$MESA_REV $(cat "$PATCHES"/*.patch 2>/dev/null | shasum | cut -c1-12)"
if [[ ! -f $SRC/.revision || $(<"$SRC/.revision") != "$STAMP" ]]; then
    rm -rf "$SRC"; mkdir -p "$SRC"
    git -C "$MESA_FORK" archive "$MESA_REV" | tar -x -C "$SRC"
    for p in "$PATCHES"/*.patch; do
        [[ -f $p ]] || continue
        patch -s -d "$SRC" -p1 < "$p" || { echo "radv: $p does not apply" >&2; exit 1; }
    done
    echo "$STAMP" > "$SRC/.revision"
fi
if [[ ! -f $W/sdkfork/.revision ]]; then
    rm -rf "$W/sdkfork"; mkdir -p "$W/sdkfork"
    git -C "$SDK_FORK" archive "$SDK_FORK_REV" platform include | tar -x -C "$W/sdkfork"
    echo "$SDK_FORK_REV" > "$W/sdkfork/.revision"
fi

# Host tools: mesa_clc and vtn_bindgen2 (poly's OpenCL kernels), against
# Homebrew LLVM + libclc + SPIRV-LLVM-Translator.
if [[ ! -x $CLC/src/compiler/clc/mesa_clc || ! -x $CLC/src/compiler/spirv/vtn_bindgen2 ]]; then
    ( unset LLVM_CONFIG; export PATH="$BREW/opt/llvm/bin:$PATH"
      export PKG_CONFIG_PATH="$BREW/opt/libclc/share/pkgconfig:$BREW/opt/spirv-llvm-translator/lib/pkgconfig:$BREW/lib/pkgconfig:$BREW/share/pkgconfig"
      [[ -f $CLC/build.ninja ]] || meson setup "$CLC" "$SRC" -Dbuildtype=release -Dmesa-clc=enabled -Dinstall-mesa-clc=true \
        -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
        -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dllvm=enabled -Dshared-llvm=enabled \
        -Dbuild-tests=false -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dxmlconfig=disabled \
        -Dtools= > "$W/clc.setup.log" 2>&1 || { tail -30 "$W/clc.setup.log" >&2; exit 1; }
      ninja -C "$CLC" src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 > "$W/clc.build.log" 2>&1 ||
        { grep -E "error|FAILED" "$W/clc.build.log" | head -30 >&2; exit 1; } )
fi
mkdir -p "$W/clc-bin"
ln -sf "$CLC/src/compiler/clc/mesa_clc" "$CLC/src/compiler/spirv/vtn_bindgen2" "$W/clc-bin/"
export PATH="$W/clc-bin:$PATH"

LIBCXX="$ROOT/toolchain/libcxx19/install"
cat > "$W/cross.ini" <<INI
[binaries]
c = '$PS5_PAYLOAD_SDK/bin/prospero-clang'
cpp = '$PS5_PAYLOAD_SDK/bin/prospero-clang++'
ar = '$PS5_PAYLOAD_SDK/bin/prospero-ar'
strip = '$PS5_PAYLOAD_SDK/bin/prospero-strip'
pkg-config = '$ROOT/port/deps/pkg-config-ps5'

[host_machine]
system = 'freebsd'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[built-in options]
default_library = 'static'
c_args = ['-I$W/sdkfork/platform/include', '-ffunction-sections', '-fdata-sections']
cpp_args = ['-I$W/sdkfork/platform/include', '-ffunction-sections', '-fdata-sections', '-nostdinc++', '-isystem', '$LIBCXX/include/c++/v1']

[properties]
needs_exe_wrapper = true
INI
if [[ ! -f $BLD/build.ninja ]]; then
    meson setup "$BLD" "$SRC" --cross-file "$W/cross.ini" \
        -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= -Dradv-winsys=ps5 \
        -Dllvm=disabled -Damd-use-llvm=false -Dvideo-codecs= \
        -Dbuildtype=debugoptimized -Db_ndebug=true \
        -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
        -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dzlib=enabled --force-fallback-for=zlib \
        -Dexpat=disabled -Dxmlconfig=disabled -Dshader-cache=enabled -Dbuild-tests=false -Dvulkan-layers= -Dtools= \
        -Dmesa-clc=system -Dradv-build-id="$MESA_REV" > "$W/setup.log" 2>&1 || { tail -30 "$W/setup.log" >&2; exit 1; }
fi
ninja -C "$BLD" src/amd/vulkan/libvulkan_radeon.a > "$W/build.log" 2>&1 ||
    { grep -E "error|FAILED" "$W/build.log" | head -40 >&2; exit 1; }
rm -rf "$INST"; mkdir -p "$INST/lib" "$INST/include"
cp "$BLD/src/amd/vulkan/libvulkan_radeon.a" "$INST/lib/libvulkan_radeon.ps5.a"
cp -r "$SRC/include/vulkan" "$INST/include/vulkan"
cp -r "$SRC/include/vk_video" "$INST/include/vk_video" 2>/dev/null || true
ls -la "$INST/lib/libvulkan_radeon.ps5.a"
