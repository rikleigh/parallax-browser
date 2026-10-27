#!/usr/bin/env bash
# Spike 4: cross-compile JavaScriptCore's `jsc` shell (JSCOnly port, JIT off,
# asm LLInt) for the PS5 with ps5-payload-sdk. Everything lands in
# spikes/04-jsc/build/ and toolchain/{icu,libcxx19}/.
#
#   spikes/04-jsc/build.sh                # all PS5 steps
#   spikes/04-jsc/build.sh jsc-ps5        # just (re)configure + build jsc for PS5
#   spikes/04-jsc/build.sh jsc-mac        # native macOS JIT-off baseline build
#   spikes/04-jsc/build.sh bench-mac      # run js/ on the macOS build
#
# Steps: icu-host icu-ps5 icu-data-slim libcxx src jsc-ps5 [jsc-mac bench-mac]
# Env: JOBS (default: ncpu), FRESH_SRC=1 (recreate the private WebKit copy),
#      ICU_DATA=full|slim (default full), C_LOOP=ON (CLoop fallback),
#      WASM=ON, SYSTEM_MALLOC=ON, NINJA_FLAGS="-k 0".
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
. "$ROOT/env.sh"

JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"
B="$HERE/build"
SRC="$B/WebKit"                          # private patched copy (ref/WebKit stays clean)
WEBKIT_REF="$ROOT/ref/WebKit"
WEBKIT_COMMIT="15486a01"
SHIM="$HERE/shim"

ICU_VER=76.1
ICU_TGZ=icu4c-76_1-src.tgz
ICU_URL="https://github.com/unicode-org/icu/releases/download/release-76-1/$ICU_TGZ"
ICU_SHA256=dfacb46bfe4747410472ce3e1144bf28a102feeaa4e3875bac9b4c6cf30f4f3e
ICU="$ROOT/toolchain/icu"
ICU_PS5="$ICU/install-ps5"

LLVM_VER=19.1.7
LLVM_TXZ="llvm-project-$LLVM_VER.src.tar.xz"
LLVM_URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/$LLVM_TXZ"
LLVM_SHA256=82401fea7b79d0078043f7598b835284d6650a75b93e64b6f761ea7b63097501
LIBCXX="$ROOT/toolchain/libcxx19"
LIBCXX_PS5="$LIBCXX/install"

log() { printf '\n=== %s\n' "$*"; }
fetch() { # url sha256 destdir
    local url=$1 sum=$2 dir=$3 f
    f="$dir/$(basename "$url")"
    mkdir -p "$dir"
    [ -f "$f" ] || wget -q -O "$f" "$url"
    echo "$sum  $f" | shasum -a 256 -c - >/dev/null
}

# --------------------------------------------------------------------- ICU
step_icu_host() {
    log "ICU $ICU_VER host build (provides icupkg/pkgdata/genccode for the cross build)"
    fetch "$ICU_URL" "$ICU_SHA256" "$ICU/dl"
    if [ ! -d "$ICU/src-$ICU_VER/icu" ]; then
        mkdir -p "$ICU/src-$ICU_VER"   # untrusted archive: own fresh dir
        tar -xzf "$ICU/dl/$ICU_TGZ" -C "$ICU/src-$ICU_VER"
    fi
    mkdir -p "$ICU/build-host"
    (cd "$ICU/build-host" &&
        [ -f Makefile ] || "../src-$ICU_VER/icu/source/configure" \
            --disable-tests --disable-samples --disable-extras --disable-icuio --disable-layoutex \
            CC=clang CXX=clang++ CFLAGS=-O2 CXXFLAGS="-O2 -std=c++17")
    make -C "$ICU/build-host" -j"$JOBS" >/dev/null
}

step_icu_ps5() {
    log "ICU $ICU_VER cross build for x86_64-sie-ps5 (static libs, data as static archive)"
    ( . "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
      mkdir -p "$ICU/build-ps5" && cd "$ICU/build-ps5"
      [ -f Makefile ] || "../src-$ICU_VER/icu/source/configure" \
          --host=x86_64-unknown-freebsd9 --build="$(uname -m | sed s/arm64/aarch64/)-apple-darwin" \
          --with-cross-build="$ICU/build-host" --prefix="$ICU_PS5" \
          --enable-static --disable-shared --with-data-packaging=static \
          --disable-tools --disable-tests --disable-samples --disable-extras \
          --disable-icuio --disable-layoutex --disable-dyload \
          CC=prospero-clang CXX=prospero-clang++ AR=prospero-ar RANLIB=prospero-ranlib \
          CFLAGS="-O2 -fPIC -ffunction-sections -fdata-sections" \
          CXXFLAGS="-O2 -fPIC -std=c++17 -ffunction-sections -fdata-sections"
      make -j"$JOBS" >/dev/null && make install >/dev/null )
    ls -la "$ICU_PS5/lib"/*.a
}

# Optional slimmer ICU data archive for jsc: drops charset converters (*.cnv),
# transliterators, CJK/SE-Asian word-break dictionaries, Unicode char names and
# language/region display names. Everything ECMA-402 needs for formatting,
# collation, normalization, case mapping and segmentation (minus dictionary
# based word breaking) stays. Select with ICU_DATA=slim for the jsc-ps5 step.
step_icu_data_slim() {
    log "ICU slim data archive"
    local dat="$ICU/src-$ICU_VER/icu/source/data/in/icudt76l.dat" w="$B/icu-slim"
    local tools="$ICU/build-host/bin" libs="$ICU/build-host/lib"
    rm -rf "$w"; mkdir -p "$w/out"
    DYLD_LIBRARY_PATH="$libs" "$tools/icupkg" -l "$dat" >"$w/all.txt"
    grep -E '\.cnv$|^translit/|^brkitr/.*\.dict$|^lang/|^region/|^unames\.icu$' "$w/all.txt" >"$w/remove.txt"
    DYLD_LIBRARY_PATH="$libs" "$tools/icupkg" --ignore-deps -r "$w/remove.txt" "$dat" "$w/out/icudt76l.dat"
    DYLD_LIBRARY_PATH="$libs" "$tools/genccode" -q -a gcc -e icudt76 -d "$w" "$w/out/icudt76l.dat"
    ( . "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
      prospero-clang -c "$w/icudt76l_dat.S" -o "$w/icudt76l_dat.o"
      rm -f "$ICU_PS5/lib/libicudata-slim.a"
      prospero-ar rcs "$ICU_PS5/lib/libicudata-slim.a" "$w/icudt76l_dat.o" )
    ls -la "$dat" "$w/out/icudt76l.dat" "$ICU_PS5/lib/libicudata-slim.a"
}

# --------------------------------------------------------------- libc++ 19
# The SDK ships libc++ 18, which lacks std::atomic_ref (WTF/Atomics.h needs it).
# Build libc++/libc++abi/libunwind 19 into a private prefix; the SDK sysroot
# is left untouched (other spikes use it).
step_libcxx() {
    log "libc++ $LLVM_VER for PS5 -> $LIBCXX_PS5"
    fetch "$LLVM_URL" "$LLVM_SHA256" "$LIBCXX/dl"
    local s="$LIBCXX/src/llvm-project-$LLVM_VER.src"
    if [ ! -d "$s/libcxx" ]; then
        mkdir -p "$LIBCXX/src"
        tar -xJf "$LIBCXX/dl/$LLVM_TXZ" -C "$LIBCXX/src" \
            "llvm-project-$LLVM_VER.src/"{runtimes,libcxx,libcxxabi,libunwind,cmake,llvm/cmake,llvm/utils/llvm-lit,third-party}
    fi
    prospero-cmake -G Ninja -S "$s/runtimes" -B "$LIBCXX/build" \
        -DCMAKE_INSTALL_PREFIX="$LIBCXX_PS5" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-nostdlib++" -DCMAKE_VERBOSE_MAKEFILE=OFF \
        -DLLVM_ENABLE_RUNTIMES="libunwind;libcxxabi;libcxx" \
        -DLIBCXX_ENABLE_STATIC=YES -DLIBCXX_ENABLE_SHARED=NO -DLIBCXX_ENABLE_RTTI=YES \
        -DLIBCXX_ENABLE_EXCEPTIONS=YES -DLIBCXX_ENABLE_THREADS=YES \
        -DLIBCXX_ENABLE_ABI_LINKER_SCRIPT=NO -DLIBCXX_CXX_ABI=libcxxabi \
        -DLIBCXXABI_ENABLE_STATIC=YES -DLIBCXXABI_ENABLE_SHARED=NO \
        -DLIBCXXABI_ENABLE_EXCEPTIONS=YES -DLIBCXXABI_ENABLE_THREADS=YES \
        -DLIBCXXABI_USE_LLVM_UNWINDER=YES -DLIBCXXABI_USE_COMPILER_RT=NO \
        -DLIBUNWIND_ENABLE_STATIC=YES -DLIBUNWIND_ENABLE_SHARED=NO \
        -DLIBUNWIND_ENABLE_THREADS=YES -DLIBUNWIND_IS_BAREMETAL=YES -DLIBUNWIND_USE_COMPILER_RT=NO \
        -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
        -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBUNWIND_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_TESTS=OFF >/dev/null
    ninja -C "$LIBCXX/build" install >/dev/null
}

# ------------------------------------------------------- WebKit source copy
# Copy the needed subset of ref/WebKit (sparse checkout shared with other
# spikes) into build/WebKit, commit it as a baseline, apply patches/*.patch.
step_src() {
    if [ -d "$SRC/.git" ] && [ -z "${FRESH_SRC:-}" ]; then
        log "WebKit source copy exists ($SRC); FRESH_SRC=1 to recreate"
        return
    fi
    log "Preparing private WebKit copy at $SRC (from $WEBKIT_REF @ $WEBKIT_COMMIT)"
    rm -rf "$SRC"; mkdir -p "$SRC/Source" "$SRC/Tools/Scripts" "$SRC/Source/WebGPU"
    git -C "$WEBKIT_REF" archive "$WEBKIT_COMMIT" CMakeLists.txt Source/CMakeLists.txt \
        Source/cmake Source/JavaScriptCore Source/WTF Source/bmalloc \
        Source/WebGPU/CMakeLists.txt Source/WebGPU/WebGPUCpp.h Tools/Scripts/hmaptool \
        | tar -x -C "$SRC"
    (cd "$SRC" && git init -q && echo .DS_Store >> .git/info/exclude && git add -A &&
        git -c user.email=spike@local -c user.name=spike commit -qm "WebKit $WEBKIT_COMMIT subset")
    for p in "$HERE"/patches/*.patch; do
        echo "applying $(basename "$p")"
        git -C "$SRC" apply --whitespace=nowarn "$p"
    done
}

# ------------------------------------------------------------ jsc for PS5
step_jsc_ps5() {
    local bd="$B/jsc-ps5${BUILD_SUFFIX:-}"
    log "jsc for PS5 in $bd: JSCOnly, static, JIT/DFG/FTL off, asm LLInt (C_LOOP=${C_LOOP:-OFF}), WASM=${WASM:-OFF}, libpas"
    ( . "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
      mkdir -p "$B/shim-ps5"
      prospero-clang -O2 -fPIC -c "$SHIM/ps5shim.c" -o "$B/shim-ps5/ps5shim.o"
      rm -f "$B/shim-ps5/libps5shim.a"
      prospero-ar rcs "$B/shim-ps5/libps5shim.a" "$B/shim-ps5/ps5shim.o" )

    local cxxinc="-nostdinc++ -isystem $LIBCXX_PS5/include/c++/v1 -isystem $SHIM/include -include ps5shim.h"
    local icudata=libicudata.a
    [ "${ICU_DATA:-full}" = slim ] && icudata=libicudata-slim.a
    local libs="$B/shim-ps5/libps5shim.a $LIBCXX_PS5/lib/libc++.a $LIBCXX_PS5/lib/libc++abi.a $LIBCXX_PS5/lib/libunwind.a"
    # LD_PACK_RELATIVE_RELOCS=OFF: the payload SDK's loader applies .rela.dyn
    # only; with DT_RELR (WebKit's default when lld supports it) most pointers
    # stayed unrelocated and jsc crashed in __crt_start on the console.
    prospero-cmake -G Ninja -S "$SRC" -B "$bd" \
        -DPORT=JSCOnly -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_STATIC_JSC=ON \
        -DLD_PACK_RELATIVE_RELOCS=OFF \
        -DENABLE_JIT=OFF -DENABLE_DFG_JIT=OFF -DENABLE_FTL_JIT=OFF -DENABLE_C_LOOP="${C_LOOP:-OFF}" \
        -DENABLE_WEBASSEMBLY="${WASM:-OFF}" -DENABLE_SAMPLING_PROFILER=OFF \
        -DENABLE_REMOTE_INSPECTOR=OFF -DENABLE_RESOURCE_USAGE=OFF \
        -DUSE_SYSTEM_MALLOC="${SYSTEM_MALLOC:-OFF}" \
        -DENABLE_API_TESTS=OFF -DDEVELOPER_MODE=OFF -DENABLE_TOOLS=OFF \
        -DUSE_SYSTEM_UNIFDEF=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF -DCLANGD_AUTO_SETUP=OFF \
        -DCMAKE_VERBOSE_MAKEFILE=OFF \
        -DCMAKE_C_FLAGS="-isystem $SHIM/include -include ps5shim.h -ffunction-sections -fdata-sections" \
        -DCMAKE_CXX_FLAGS="$cxxinc -ffunction-sections -fdata-sections" \
        -DCMAKE_EXE_LINKER_FLAGS="-nostdlib++ -Wl,--gc-sections" \
        -DCMAKE_CXX_STANDARD_LIBRARIES="$libs" \
        -DICU_INCLUDE_DIR="$ICU_PS5/include" \
        -DICU_UC_LIBRARY_RELEASE="$ICU_PS5/lib/libicuuc.a" \
        -DICU_I18N_LIBRARY_RELEASE="$ICU_PS5/lib/libicui18n.a" \
        -DICU_DATA_LIBRARY_RELEASE="$ICU_PS5/lib/$icudata" >"$bd-configure.log"
    ninja -C "$bd" -j"$JOBS" ${NINJA_FLAGS:-} jsc
    prospero-strip -o "$bd/bin/jsc.elf" "$bd/bin/jsc"
    # Variant with the slim ICU data archive: same objects, relinked.
    if [ -f "$ICU_PS5/lib/libicudata-slim.a" ] && [ "${ICU_DATA:-full}" = full ]; then
        (cd "$bd" && ninja -t commands bin/jsc | tail -1 |
            sed -e 's|libicudata\.a|libicudata-slim.a|g' -e 's|-o bin/jsc |-o bin/jsc-slim |' | sh)
        prospero-strip -o "$bd/bin/jsc-slim.elf" "$bd/bin/jsc-slim"
    fi
    ls -la "$bd/bin/"jsc*
    file "$bd/bin/jsc.elf"
}

# --------------------------------------------- native macOS baseline build
step_jsc_mac() {
    log "jsc for macOS host: same JIT-off asm-LLInt configuration (baseline timing)"
    ( unset CMAKE_TOOLCHAIN_FILE
      cmake -G Ninja -S "$SRC" -B "$B/jsc-mac" \
        -DPORT=JSCOnly -DCMAKE_BUILD_TYPE=Release -DENABLE_STATIC_JSC=ON \
        -DENABLE_JIT=OFF -DENABLE_DFG_JIT=OFF -DENABLE_FTL_JIT=OFF -DENABLE_C_LOOP=OFF \
        -DENABLE_WEBASSEMBLY=OFF -DENABLE_SAMPLING_PROFILER=OFF -DENABLE_REMOTE_INSPECTOR=OFF \
        -DENABLE_API_TESTS=OFF -DDEVELOPER_MODE=OFF -DENABLE_TOOLS=OFF -DUSE_SYSTEM_UNIFDEF=ON \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF -DCLANGD_AUTO_SETUP=OFF -DCMAKE_CXX_FLAGS=-fobjc-arc \
        -DICU_ROOT="$(brew --prefix icu4c 2>/dev/null || echo /opt/homebrew/opt/icu4c)" >"$B/jsc-mac-configure.log"
      ninja -C "$B/jsc-mac" -j"$JOBS" jsc )
}

step_bench_mac() {
    log "Running js/ on the macOS JIT-off build"
    local jsc="$B/jsc-mac/bin/jsc"
    for f in hello correctness bench; do
        echo "--- $f.js"
        /usr/bin/time -p "$jsc" --useJIT=0 "$HERE/js/$f.js"
    done
}

STEPS=("$@")
[ ${#STEPS[@]} -eq 0 ] && STEPS=(icu-host icu-ps5 icu-data-slim libcxx src jsc-ps5)
for s in "${STEPS[@]}"; do
    case "$s" in
        icu-host) step_icu_host ;;
        icu-ps5) step_icu_ps5 ;;
        icu-data-slim) step_icu_data_slim ;;
        libcxx) step_libcxx ;;
        src) step_src ;;
        jsc-ps5) step_jsc_ps5 ;;
        jsc-mac) step_jsc_mac ;;
        bench-mac) step_bench_mac ;;
        *) echo "unknown step: $s" >&2; exit 2 ;;
    esac
done
