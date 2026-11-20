#!/usr/bin/env bash
# WPE WebKit for the PS5 (Milestone 1: browse on the TV, pages painted on the
# CPU). Builds WebKit 15486a0178 with PORT=WPE and the PS5 WPE Platform
# backend (wpe-ps5/) against ps5-payload-sdk, libc++ 19 and the dependency
# sysroot from port/deps (toolchain/wpe-sysroot).
#
#   port/webkit/build.sh                 # src shim configure build
#   port/webkit/build.sh src             # (re)create the patched source tree
#   port/webkit/build.sh configure       # cmake only (shows missing deps)
#   port/webkit/build.sh build           # ninja (all WebKit targets we need)
#   port/webkit/build.sh export-patches  # clone commits -> port/webkit/patches
#   port/webkit/build.sh status          # disk use, deps found
#
# Layout (the default is WEBGL=1: src-webgl + build-webgl, the tree the
# browser ships from, with WebGL and every patch; WEBGL=0 selects the older
# src + build below, without WebGL):
#   toolchain/wpe-webkit/src-webgl  worktree of src, branch with the WebGL
#                                and later patches; build-webgl its build
#                                (logs in build-webgl/). A full "build" fails
#                                linking WPEWebProcess there: build
#                                NINJA_TARGETS="ps5wk-child ps5wk-shot ps5browser"
#   toolchain/wpe-webkit/src     private clone of ref/WebKit (shared objects,
#                                sparse checkout), branch "ps5" = 15486a0178 +
#                                one commit per patches/*.patch
#   toolchain/wpe-webkit/build   cmake/ninja build (Release, -gline-tables-only),
#                                with the WC compositor (GPU compositing)
#   toolchain/wpe-webkit/build-cpu  WC=0: the same without the compositor
#                                (CPU painting only; logs in build-cpu/)
#   SRC=, BLD=, LOGDIR= override these (a second worktree and build dir:
#   git -C toolchain/wpe-webkit/src worktree add ../src-x -b x ps5;
#   toolchain/wpe-webkit/src-wc + build-wc are one, branch ps5-wc)
#
# Env: JOBS (default ncpu), FRESH_SRC=1, JIT=0 (configure without the JIT), NINJA_TARGETS="...", NINJA_FLAGS="-k 0",
#      DEBUG_FLAGS (default -gline-tables-only; -g0 for the smallest build)
#      WC=1 (default: the WC compositor next to coordinated graphics,
#      USE_TEXTURE_MAPPER + USE_GRAPHICS_LAYER_WC; COMPOSITING-PLAN.md; the
#      title composites by default and falls back to CPU painting at run
#      time); WC=0 builds without it (default build dir build-cpu).
#      `build` reconfigures first when the build dir was configured with
#      the other WC setting.
#      WEBGL=1: WebGL through the GPU process, hosted in the title (ANGLE over
#      ps5-opengl; spikes/11-webgl). Default build dir build-webgl, source
#      worktree src-webgl (branch ps5-webgl).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
. "$ROOT/env.sh" >/dev/null

JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"
WK="$ROOT/toolchain/wpe-webkit"
WC="${WC:-1}"
WEBGL="${WEBGL:-1}"
# GPU=radv (HANDOVER-radv.md, Phase 2): the title's compositor, UI, video
# and overlay on ANGLE's Vulkan backend over RADV instead of ps5-opengl
# (spikes/15-radv-only). WEBGPU=1's options, in its own worktree and build
# dir until parity: src-radv (branch ps5-radv, made from ps5-webgl; its
# commits are exported to patches-radv/) and build-radv. GPU=gl (default):
# ps5-opengl, as before.
GPU="${GPU:-gl}"
if [ "$GPU" = radv ]; then
    WEBGL=1
    WEBGPU=1
    SRC="${SRC:-$WK/src-radv}"
    BLD="${BLD:-$WK/build-radv}"
    LOGDIR="${LOGDIR:-$BLD}"
fi
# WEBGPU=1: WebGPU on Dawn + RADV (spikes/12-webgpu), on top of WEBGL=1's
# tree. Its own build dir, build-webgpu, until it ships.
WEBGPU="${WEBGPU:-0}"
if [ "$WEBGPU" = 1 ]; then
    [ "$WEBGL" = 1 ] || { echo "WEBGPU=1 needs WEBGL=1 (the GPU process side in the title)" >&2; exit 2; }
    BLD="${BLD:-$WK/build-webgpu}"
fi
if [ "$WEBGL" = 1 ]; then
    SRC="${SRC:-$WK/src-webgl}"
    BLD="${BLD:-$WK/build-webgl}"
    LOGDIR="${LOGDIR:-$BLD}"
fi
SRC="${SRC:-$WK/src}"
if [ "$WC" = 1 ]; then
    BLD="${BLD:-$WK/build}"
    LOGDIR="${LOGDIR:-$WK}"
else
    BLD="${BLD:-$WK/build-cpu}"
    LOGDIR="${LOGDIR:-$BLD}"
fi
WEBKIT_REF="$ROOT/ref/WebKit"
WEBKIT_COMMIT=15486a017817cad93f25fdffeb013278866970fc
SHIM="$HERE/shim"
LIBCXX="$ROOT/toolchain/libcxx19/install"
ICU="$ROOT/toolchain/icu/install-ps5"
SYSROOT="${WPE_SYSROOT:-$ROOT/toolchain/wpe-sysroot}"
PKGCONFIG="$ROOT/port/deps/pkg-config-ps5"

log() { printf '\n=== %s\n' "$*"; }

# ------------------------------------------------------------------- source
# A clone that shares ref/WebKit's object store (alternates; ref/WebKit is
# never written to), sparse: Source/ minus libwebrtc, plus the Tools bits the
# build reads. Patches are applied as commits on branch "ps5" so they can be
# edited in place and exported again (export-patches).
step_src() {
    if [ "$GPU" = radv ] && [ ! -e "$SRC/.git" ]; then
        log "worktree $SRC (branch ps5-radv from ps5-webgl)"
        git -C "$WK/src-webgl" worktree add -q "$SRC" -b ps5-radv ps5-webgl
        for p in "$HERE"/patches-radv/*.patch; do
            [ -e "$p" ] || continue
            echo "  $(basename "$p")"
            git -C "$SRC" apply --whitespace=nowarn "$p"
            git -C "$SRC" add -A Source CMakeLists.txt
            git -C "$SRC" commit -qm "$(basename "$p" .patch)"
        done
    fi
    if [ ! -e "$SRC/.git" ]; then
        log "cloning $WEBKIT_REF -> $SRC (shared objects, sparse)"
        mkdir -p "$WK"
        git clone -q --no-checkout "$WEBKIT_REF" "$SRC" || true
        # A clone of a shallow repo copies the pack; use the reference's
        # objects through alternates instead.
        echo "$WEBKIT_REF/.git/objects" > "$SRC/.git/objects/info/alternates"
        rm -f "$SRC"/.git/objects/pack/pack-*
        git -C "$SRC" sparse-checkout set --no-cone '/CMakeLists.txt' '/Source/' \
            '!/Source/ThirdParty/libwebrtc/' '/Tools/Scripts/' '/Tools/CMakeLists.txt' \
            '/Tools/wpe/' '/Tools/MiniBrowser/wpe/' '/Tools/glib/'
        git -C "$SRC" config user.email ps5wk@local
        git -C "$SRC" config user.name ps5wk
    fi
    if [ -z "${FRESH_SRC:-}" ] && git -C "$SRC" rev-parse -q --verify ps5 >/dev/null; then
        log "source tree exists (branch ps5); FRESH_SRC=1 re-applies patches/*.patch"
    else
        log "applying patches/*.patch on $WEBKIT_COMMIT"
        git -C "$SRC" checkout -q -f "$WEBKIT_COMMIT"
        git -C "$SRC" clean -fdq -- Source
        git -C "$SRC" checkout -q -B ps5
        for p in "$HERE"/patches/*.patch; do
            echo "  $(basename "$p")"
            git -C "$SRC" apply --whitespace=nowarn "$p"
            git -C "$SRC" add -A Source CMakeLists.txt
            git -C "$SRC" commit -qm "$(basename "$p" .patch)"
        done
    fi
    # The PS5 backend lives in port/webkit/wpe-ps5; WebKit sees it as wpe/ps5.
    ln -sfn "$HERE/wpe-ps5" "$SRC/Source/WebKit/WPEPlatform/wpe/ps5"
    local exclude
    exclude="$(cd "$SRC" && cd "$(git rev-parse --git-common-dir)" && pwd)/info/exclude"
    grep -qx 'Source/WebKit/WPEPlatform/wpe/ps5' "$exclude" 2>/dev/null ||
        echo 'Source/WebKit/WPEPlatform/wpe/ps5' >> "$exclude"
}

step_export_patches() {
    # GPU=radv: only its own commits on top of ps5-webgl, to patches-radv/.
    local base="$WEBKIT_COMMIT" dir="$HERE/patches"
    [ "$GPU" = radv ] && { base=ps5-webgl; dir="$HERE/patches-radv"; mkdir -p "$dir"; }
    log "exporting $base..HEAD ($(git -C "$SRC" branch --show-current)) to $(basename "$dir")/"
    rm -f "$dir"/*.patch
    for c in $(git -C "$SRC" rev-list --reverse "$base..HEAD"); do
        name="$(git -C "$SRC" log -1 --format=%s "$c")"
        git -C "$SRC" diff --no-color "$c^" "$c" > "$dir/$name.patch"
        echo "  $name.patch ($(git -C "$SRC" diff --shortstat "$c^" "$c"))"
    done
}

# --------------------------------------------------------------------- shim
step_shim() {
    log "libps5shim.a"
    ( . "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
      mkdir -p "$BLD/shim"
      prospero-clang -O2 -fPIC -ffunction-sections -fdata-sections -I"$SHIM/include" \
          -c "$SHIM/ps5shim.c" -o "$BLD/shim/ps5shim.o"
      rm -f "$BLD/shim/libps5shim.a"
      prospero-ar rcs "$BLD/shim/libps5shim.a" "$BLD/shim/ps5shim.o"
      # ICU's stub data library: the 32 MB of ICU data stay out of the ELFs
      # (elfldr refused a 125 MB payload image) and are read from
      # $ICU_DATA/icudt76l.dat on the console (/data/ps5webkit/share/icu).
      local icusrc="$ROOT/toolchain/icu/src-76.1/icu/source"
      prospero-clang++ -O2 -fPIC -nostdinc++ -isystem "$LIBCXX/include/c++/v1" -DU_STATIC_IMPLEMENTATION \
          -I"$icusrc/common" -I"$ICU/include" -c "$icusrc/stubdata/stubdata.cpp" -o "$BLD/shim/icustubdata.o"
      rm -f "$BLD/shim/libicudata-stub.a"
      prospero-ar rcs "$BLD/shim/libicudata-stub.a" "$BLD/shim/icustubdata.o" )
}

# ---------------------------------------------------------------- configure
# Everything optional is off. JIT on (Baseline, DFG, FTL, YARR; patches 0030+;
# JIT=0 builds the asm LLInt only, as Spike 4). WebAssembly is compiled with
# the JIT because B3 (FTL) does not build without it upstream, but it is off
# at run time on PS5 (patch 0030: JSC_useWasm=true turns it on). No WebGL, no
# GPU process, Skia on the CPU. Media (Milestone 2): <video>/<audio> and MSE on
# the static GStreamer from port/deps (gstreamer-full, GL off), software
# decoders in the WebProcess, audio through ps5audiosink (sceAudioOut). WPE Platform API only; headless kept
# for the payload-side test harness.
# The Gamepad API: the DualSense through the PS5 backend (wpe-ps5/WPEGamepadPS5.cpp;
# README: "Gamepad mode"), no libmanette.
JIT_ON=ON; [ "${JIT:-1}" = 0 ] && JIT_ON=OFF
WEBKIT_OPTIONS=(
    -DPORT=WPE -DCMAKE_BUILD_TYPE=Release -DDEVELOPER_MODE=OFF
    -DENABLE_WPE_LEGACY_API=OFF -DENABLE_WPE_QT_API=OFF -DENABLE_WPE_1_1_API=OFF
    -DENABLE_WPE_PLATFORM_DRM=OFF -DENABLE_WPE_PLATFORM_WAYLAND=OFF
    -DENABLE_WPE_PLATFORM_HEADLESS=ON -DENABLE_WPE_PLATFORM_PS5=ON
    -DUSE_SKIA=ON -DUSE_GBM=OFF -DUSE_LIBDRM=OFF -DUSE_VULKAN=OFF
    -DENABLE_JIT="${JIT_ON}" -DENABLE_DFG_JIT="${JIT_ON}" -DENABLE_FTL_JIT="${JIT_ON}" -DENABLE_C_LOOP=OFF
    -DENABLE_WEBASSEMBLY="${JIT_ON}" -DENABLE_WEBASSEMBLY_BBQJIT="${JIT_ON}" -DENABLE_WEBASSEMBLY_OMGJIT="${JIT_ON}" -DENABLE_SAMPLING_PROFILER=OFF -DENABLE_REMOTE_INSPECTOR=OFF
    -DENABLE_RESOURCE_USAGE=OFF -DENABLE_MEMORY_SAMPLER=OFF -DENABLE_RELEASE_LOG=ON
    -DUSE_SYSTEM_MALLOC=OFF
    # -O3 (OPT=-O2 for the old flags) and no libc++ hardening checks
    # (_LIBCPP_HARDENING_MODE_EXTENSIVE, WebKit's default): Speedometer 3.1
    # 5.27 -> 5.62 on the console (README: "Speedometer"). Both take a full
    # rebuild (reconfigure).
    -DUSE_CXX_STDLIB_ASSERTIONS=OFF
    -DENABLE_WEBGL=OFF -DENABLE_WEBGPU=OFF -DENABLE_WEBXR=OFF -DENABLE_GPU_PROCESS=OFF
    -DUSE_GSTREAMER=ON -DUSE_GSTREAMER_FULL=ON -DUSE_GSTREAMER_GL=OFF -DUSE_GSTREAMER_MPEGTS=OFF
    -DENABLE_VIDEO=ON -DENABLE_MEDIA_SOURCE=ON -DENABLE_MEDIA_TELEMETRY=OFF
    -DENABLE_MEDIA_CONTROLS_CONTEXT_MENUS=OFF -DENABLE_VIDEO_USES_ELEMENT_FULLSCREEN=OFF
    -DENABLE_WEB_AUDIO=OFF -DENABLE_MEDIA_STREAM=OFF -DENABLE_WEB_RTC=OFF
    -DENABLE_WEB_CODECS=OFF -DENABLE_MEDIA_RECORDER=OFF -DENABLE_MEDIA_SESSION=OFF
    -DENABLE_ENCRYPTED_MEDIA=OFF -DENABLE_THUNDER=OFF -DUSE_EXTERNAL_HOLEPUNCH=OFF
    -DENABLE_GAMEPAD=ON -DENABLE_SPEECH_SYNTHESIS=OFF -DENABLE_SPELLCHECK=OFF
    -DENABLE_XSLT=OFF -DENABLE_WEBDRIVER=OFF -DENABLE_WEBDRIVER_BIDI=OFF -DENABLE_PDFJS=OFF
    -DENABLE_GEOLOCATION=OFF -DENABLE_BUBBLEWRAP_SANDBOX=OFF -DENABLE_NOTIFICATIONS=OFF
    -DENABLE_WK_WEB_EXTENSIONS=OFF
    -DENABLE_INTROSPECTION=OFF -DENABLE_DOCUMENTATION=OFF -DENABLE_JOURNALD_LOG=OFF
    -DUSE_LIBBACKTRACE=OFF -DUSE_ATK=OFF -DUSE_AVIF=OFF -DUSE_LCMS=OFF -DUSE_JPEGXL=OFF
    -DUSE_LIBHYPHEN=OFF -DUSE_FLITE=OFF -DUSE_LIBSECRET=OFF
    -DUSE_SYSPROF_CAPTURE=OFF -DUSE_SYSTEM_SYSPROF_CAPTURE=OFF
    -DUSE_SKIA_OPENTYPE_SVG=OFF -DUSE_OPENSSL=ON -DUSE_WOFF2="${USE_WOFF2:-OFF}"
    -DENABLE_MINIBROWSER=OFF -DENABLE_API_TESTS=OFF -DENABLE_LAYOUT_TESTS=OFF -DENABLE_TOOLS=OFF
    -DUSE_SYSTEM_UNIFDEF=ON
    -DLD_PACK_RELATIVE_RELOCS=OFF
    -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF -DCLANGD_AUTO_SETUP=OFF -DCMAKE_VERBOSE_MAKEFILE=OFF
)
if [ "$WEBGL" = 1 ]; then
    WEBKIT_OPTIONS+=(-DENABLE_WEBGL=ON -DENABLE_GPU_PROCESS=ON)
    # Web Audio (AudioContext): in the WebProcess (UseGPUProcessForMedia is off
    # with GStreamer), out through autoaudiosink = ps5audiosink like <video>'s
    # sound. Games need it: Unity's sound start-up calls alert("Web Audio API
    # is not supported") without it, and game portals turn that alert into a
    # "the game crashed" screen. WEB_AUDIO=0 builds without it.
    if [ "${WEB_AUDIO:-1}" = 1 ]; then
        WEBKIT_OPTIONS+=(-DENABLE_WEB_AUDIO=ON)
    fi
fi
if [ "$WEBGPU" = 1 ]; then
    # Later -D wins over the -DENABLE_WEBGPU=OFF above.
    WEBKIT_OPTIONS+=(-DENABLE_WEBGPU=ON -DUSE_DAWN=ON)
fi
if [ "$GPU" = radv ]; then
    # ANGLE gets its Vulkan backend (PlatformWPE.cmake), wpe-ps5 renders on it.
    # Headers: RADV's install (port/deps/build.sh radv), SPIR-V from Dawn's
    # source (port/deps/build.sh dawn), VulkanMemoryAllocator at ANGLE's DEPS
    # pin (fetched once by step_vma). Dawn: the radv-only one (port/deps/build.sh
    # dawn-radv: Phase 4's host-pointer textures), not the sysroot's.
    WEBKIT_OPTIONS+=(-DPS5WK_GPU_RADV=ON
        -DPS5WK_DAWN_ROOT="$ROOT/toolchain/wpe-deps/install/dawn-radv"
        -DPS5WK_VULKAN_INCLUDE="$ROOT/build/radv-ps5/install/include"
        -DPS5WK_SPIRV_HEADERS_INCLUDE="$ROOT/toolchain/wpe-deps/src/dawn/third_party/spirv-headers/src/include"
        -DPS5WK_SPIRV_TOOLS_INCLUDE="$ROOT/toolchain/wpe-deps/src/dawn/third_party/spirv-tools/src/include"
        -DPS5WK_VMA_DIR="$ROOT/toolchain/wpe-deps/src/vma")
fi
# VulkanMemoryAllocator for ANGLE's Vulkan backend (GPU=radv), at the
# revision ANGLE's DEPS pins.
VMA_REV=7e55b011e16182fc349149abbd3aaf3b1db46421
step_vma() {
    local vma="$ROOT/toolchain/wpe-deps/src/vma"
    [ -f "$vma/include/vk_mem_alloc.h" ] && return 0
    log "fetching VulkanMemoryAllocator $VMA_REV"
    rm -rf "$vma"; mkdir -p "$vma"
    git -C "$vma" init -q
    git -C "$vma" fetch -q --depth 1 \
        https://chromium.googlesource.com/external/github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator "$VMA_REV"
    git -C "$vma" checkout -q FETCH_HEAD
}
if [ "$WC" = 1 ]; then
    WEBKIT_OPTIONS+=(-DUSE_TEXTURE_MAPPER=ON -DUSE_GRAPHICS_LAYER_WC=ON)
else
    WEBKIT_OPTIONS+=(-DUSE_TEXTURE_MAPPER=OFF -DUSE_GRAPHICS_LAYER_WC=OFF)
fi

step_configure() {
    [ "$GPU" = radv ] && step_vma
    log "configure $BLD"
    mkdir -p "$BLD"
    local cflags="-isystem $SHIM/include -include ps5shim.h -ffunction-sections -fdata-sections ${DEBUG_FLAGS:--gline-tables-only}"
    local cxxflags="-nostdinc++ -isystem $LIBCXX/include/c++/v1 $cflags"
    local libs="$BLD/shim/libps5shim.a $LIBCXX/lib/libc++.a $LIBCXX/lib/libc++abi.a $LIBCXX/lib/libunwind.a"
    local tc="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake"
    # The deps toolchain file (port/deps) knows the sysroot; fall back to the
    # SDK's file plus search paths.
    local depstc
    depstc="$(ls "$ROOT"/port/deps/*.cmake 2>/dev/null | head -1 || true)"
    [ -n "$depstc" ] && [ -z "${FAKE_DEPS:-}" ] && tc="$depstc"
    # FAKE_DEPS=1: placeholder epoxy/xkbcommon (headers that #error), so
    # configure completes and JSC/WTF/bmalloc build while port/deps finishes.
    local roots="$SYSROOT;$ICU" pc="$PKGCONFIG"
    if [ -n "${FAKE_DEPS:-}" ]; then
        roots="$roots;$WK/fake"; pc="$WK/fake/pkg-config"
        EXTRA_CMAKE_ARGS="${EXTRA_CMAKE_ARGS:-} -DEpoxy_INCLUDE_DIR=$WK/fake/include -DEpoxy_LIBRARY=$WK/fake/lib/libepoxy.a"
    fi
    # Static link flags of every dependency (Libs.private included): WebKit's
    # find modules only name the top-level archives.
    local depslibs=""
    [ -z "${FAKE_DEPS:-}" ] && depslibs="$("$PKGCONFIG" --libs gio-unix-2.0 gioopenssl libsoup-3.0 fontconfig freetype2 \
        harfbuzz-icu libxml-2.0 sqlite3 gstreamer-full-1.0 libwebpdemux libwebpmux libjpeg libpng16 epoxy xkbcommon openssl zlib libpsl libarchive | sed 's/ -licudata//g')"
    ( . "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
      export PKG_CONFIG="$pc"
      cmake -G Ninja -S "$SRC" -B "$BLD" \
        -DCMAKE_TOOLCHAIN_FILE="$tc" \
        -DCMAKE_FIND_ROOT_PATH="$roots" -DCMAKE_PREFIX_PATH="$SYSROOT" \
        -DPKG_CONFIG_EXECUTABLE="$pc" -DPS5_PORT_DIR="$HERE" -DPS5WK_LOG_HOST="$PS5WK_LOG_HOST" -DPS5WK_LOG_PORT="$PS5WK_LOG_PORT" -DPS5_DEPS_LIBS="$depslibs" \
        "${WEBKIT_OPTIONS[@]}" \
        -DCMAKE_C_FLAGS="$cflags" -DCMAKE_CXX_FLAGS="$cxxflags" \
        -DCMAKE_C_FLAGS_RELEASE="${OPT:--O3} -DNDEBUG" -DCMAKE_CXX_FLAGS_RELEASE="${OPT:--O3} -DNDEBUG" \
        -DCMAKE_EXE_LINKER_FLAGS="-nostdlib++ -Wl,--gc-sections -L$SYSROOT/lib -Wl,--wrap=fcntl,--wrap=pipe2" \
        -DCMAKE_CXX_STANDARD_LIBRARIES="$libs" \
        -DICU_ROOT="$ICU" -DICU_INCLUDE_DIR="$ICU/include" \
        -DICU_UC_LIBRARY_RELEASE="$ICU/lib/libicuuc.a" \
        -DICU_I18N_LIBRARY_RELEASE="$ICU/lib/libicui18n.a" \
        -DICU_DATA_LIBRARY_RELEASE="$BLD/shim/libicudata-stub.a" \
        ${EXTRA_CMAKE_ARGS:-} ) 2>&1 | tee "$LOGDIR/configure.log" | grep -E "^-- (Enabled|  |Could)|Error|error|Missing|not found|NOT" | head -80
    [ -f "$BLD/build.ninja" ] && echo "configure: OK" || { echo "configure: FAILED (see $LOGDIR/configure.log)"; return 1; }
}

# -------------------------------------------------------------------- build
# The build dir's WC setting (from its CMakeCache), "" when not configured.
configured_wc() {
    [ -f "$BLD/build.ninja" ] && [ -f "$BLD/CMakeCache.txt" ] || return 0
    grep -q '^USE_GRAPHICS_LAYER_WC:BOOL=ON' "$BLD/CMakeCache.txt" && echo 1 || echo 0
}

step_build() {
    local have
    have="$(configured_wc)"
    if [ "$have" != "$WC" ]; then
        log "$BLD is ${have:+configured with WC=$have}${have:-not configured}; configuring for WC=$WC"
        [ -f "$BLD/shim/libps5shim.a" ] || step_shim
        step_configure
    fi
    local targets="${NINJA_TARGETS:-}"
    log "ninja -j$JOBS ${targets:-<all>}"
    local t0=$(date +%s)
    set +e
    ninja -C "$BLD" -j"$JOBS" ${NINJA_FLAGS:-} $targets > "$LOGDIR/ninja.log" 2>&1
    local rc=$?
    set -e
    grep -E "error:|FAILED|undefined symbol" "$LOGDIR/ninja.log" | head -40 || true
    echo "ninja exit $rc after $(( $(date +%s) - t0 )) s" | tee -a "$LOGDIR/build-times.log"
    return "$rc"
}

step_status() {
    df -h "$ROOT" | tail -1
    du -sh "$SRC" "$BLD" 2>/dev/null || true
    ls "$SYSROOT/lib/pkgconfig" 2>/dev/null | sed 's/\.pc$//' | tr '\n' ' '; echo
}

STEPS=("$@")
[ ${#STEPS[@]} -eq 0 ] && STEPS=(src shim configure build)
for s in "${STEPS[@]}"; do
    case "$s" in
        src) step_src ;;
        export-patches) step_export_patches ;;
        shim) step_shim ;;
        configure) step_configure ;;
        build) step_build ;;
        status) step_status ;;
        *) echo "unknown step: $s" >&2; exit 2 ;;
    esac
done
