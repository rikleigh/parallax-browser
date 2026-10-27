#!/usr/bin/env bash
# Build pacbrew-repo's Mesa 26.2 PS5 port (RADV + zink + EGL "videoout") and
# the Spike 2 payload against it, inside a native linux/arm64 Ubuntu 24.04
# container (see Dockerfile). Run on the Mac:
#
#   spikes/02-gl-egl/pacbrew-mesa/build.sh            # everything
#   spikes/02-gl-egl/pacbrew-mesa/build.sh pkgs       # pacbrew packages + payload
#   spikes/02-gl-egl/pacbrew-mesa/build.sh payload    # only gl_egl_mesa.elf
#   KEEP_SRC=1 ... build.sh pkgs                      # keep makepkg src/ trees
#
# Outputs (all under the repo, the repo is mounted at the same absolute path
# inside the container so every baked-in path is valid on the Mac too):
#   toolchain/pacbrew-sysroot/   ps5-payload-sdk (from ref/ps5-payload-sdk)
#                                + libcxx, openlibm, zlib, zstd, mesa in
#                                target/user/homebrew (pacbrew's layout)
#   toolchain/pacbrew-build/     downloads, makepkg trees, logs (deletable)
#   spikes/02-gl-egl/pacbrew-mesa/gl_egl_mesa.elf
#
# Nothing here touches toolchain/ps5-payload-sdk (the Mac's SDK) or the
# ps5-opengl trees.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SYSROOT="$ROOT/toolchain/pacbrew-sysroot"
WORK="$ROOT/toolchain/pacbrew-build"
PACBREW="$ROOT/ref/pacbrew-repo"
SDK_SRC="$ROOT/ref/ps5-payload-sdk"
IMAGE=ps5wk-pacbrew-mesa:latest

# --------------------------------------------------------------- on the Mac
if [[ "${1:-}" != "--inside" ]]; then
    command -v docker >/dev/null || { echo "docker missing" >&2; exit 1; }
    docker build --platform linux/arm64 -t "$IMAGE" "$HERE"
    mkdir -p "$WORK" "$SYSROOT"
    exec docker run --rm --platform linux/arm64 \
         -e KEEP_SRC="${KEEP_SRC:-0}" \
         -e MAKEFLAGS="${MAKEFLAGS:--j$(sysctl -n hw.ncpu 2>/dev/null || nproc)}" \
         -v "$ROOT:$ROOT" -w "$HERE" "$IMAGE" \
         bash "$HERE/build.sh" --inside "${1:-all}"
fi

# --------------------------------------------------------- in the container
STEP="${2:-all}"
export PS5_PAYLOAD_SDK="$SYSROOT"
export LLVM_CONFIG=/usr/bin/llvm-config-18
mkdir -p "$WORK/dl" "$WORK/pkgs" "$WORK/logs"

log() { printf '\n==> %s\n' "$*"; }

# Downloads are untrusted: fetched into dl/, checked against the hash the
# PKGBUILD pins, and each one extracted into its own fresh directory.
fetch() { # url sha256 out
    local url=$1 sum=$2 out="$WORK/dl/$3"
    [[ -f "$out" ]] || curl -fL --retry 3 -o "$out.part" "$url"
    [[ -f "$out" ]] || mv "$out.part" "$out"
    echo "$sum  $out" | sha256sum -c -
}

build_sdk() {
    log "ps5-payload-sdk from ref/ps5-payload-sdk -> $SYSROOT"
    # Build a private copy: ref/ps5-payload-sdk holds the Mac build's objects.
    rm -rf "$WORK/sdk-src"
    mkdir -p "$WORK/sdk-src"
    tar -C "$SDK_SRC" --exclude=.git --exclude='*.o' --exclude='*.a' -cf - . \
        | tar -C "$WORK/sdk-src" -xf -
    make -C "$WORK/sdk-src" clean >/dev/null
    make -C "$WORK/sdk-src" DESTDIR="$SYSROOT" install
}

build_libcxx() {
    # pacbrew-repo/libcxx/PKGBUILD (== ps5-payload-sdk/libcxx.sh), but only
    # the runtimes subtrees are extracted: the full llvm-project tree is
    # ~1.5 GB and this Mac has little disk.
    log "libcxx 18.1.8 (pacbrew-repo/libcxx/PKGBUILD flags)"
    local v=18.1.8 src="$WORK/libcxx-src"
    fetch "https://github.com/llvm/llvm-project/releases/download/llvmorg-$v/llvm-project-$v.src.tar.xz" \
          0b58557a6d32ceee97c8d533a59b9212d87e0fc4d2833924eb6c611247db2f2a \
          "llvm-project-$v.src.tar.xz"
    rm -rf "$src"; mkdir -p "$src"
    tar -C "$src" -xJf "$WORK/dl/llvm-project-$v.src.tar.xz" \
        --exclude='*/test' --exclude='*/benchmarks' \
        "llvm-project-$v.src/runtimes" "llvm-project-$v.src/libcxx" \
        "llvm-project-$v.src/libcxxabi" "llvm-project-$v.src/libunwind" \
        "llvm-project-$v.src/cmake" "llvm-project-$v.src/llvm/cmake" \
        "llvm-project-$v.src/llvm/utils/llvm-lit" "llvm-project-$v.src/third-party"
    # shellcheck disable=SC1091
    ( source "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
      ${CMAKE} -S "$src/llvm-project-$v.src/runtimes" -B "$src/build" \
	     -DCMAKE_INSTALL_PREFIX="${PS5_PAYLOAD_SDK}/target" \
	     -DCMAKE_BUILD_TYPE=Release \
	     -DCMAKE_CXX_FLAGS="-nostdlib++" \
	     -DLLVM_ENABLE_RUNTIMES="libunwind;libcxxabi;libcxx" \
	     -DLLVM_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_TESTS=OFF \
	     -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBUNWIND_INCLUDE_TESTS=OFF \
	     -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
	     -DLIBCXX_ENABLE_STATIC=YES \
	     -DLIBCXX_ENABLE_RTTI=YES \
	     -DLIBCXX_ENABLE_EXCEPTIONS=YES \
	     -DLIBCXX_ENABLE_THREADS=YES \
	     -DLIBCXX_ENABLE_SHARED=NO \
	     -DLIBCXX_ENABLE_ABI_LINKER_SCRIPT=NO \
	     -DLIBCXX_CXX_ABI=libcxxabi \
	     -DLIBCXXABI_ENABLE_STATIC=YES \
	     -DLIBCXXABI_ENABLE_EXCEPTIONS=YES \
	     -DLIBCXXABI_ENABLE_THREADS=YES \
	     -DLIBCXXABI_USE_LLVM_UNWINDER=YES \
	     -DLIBCXXABI_ENABLE_SHARED=NO \
	     -DLIBCXXABI_USE_COMPILER_RT=NO \
	     -DLIBCXXABI_HAS_CXA_THREAD_ATEXIT_IMPL=NO \
	     -DLIBUNWIND_ENABLE_STATIC=YES \
	     -DLIBUNWIND_ENABLE_THREADS=YES \
	     -DLIBUNWIND_IS_BAREMETAL=YES \
	     -DLIBUNWIND_ENABLE_SHARED=NO \
	     -DLIBUNWIND_USE_COMPILER_RT=NO
      ${MAKE} -C "$src/build"
      ${MAKE} -C "$src/build" install DESTDIR=/ )
    rm -rf "$src"
}

# makepkg configuration: the distro's makepkg.conf carries host CFLAGS/LDFLAGS
# (on arm64: -march=armv8-a...) that would leak into the PS5 cross compiler.
# pacbrew's CI runs on x86_64, where those flags happen to be harmless.
write_makepkg_conf() {
    cat /etc/makepkg.conf > "$WORK/makepkg.conf"
    cat >> "$WORK/makepkg.conf" <<'EOF'
CARCH="x86_64"
CHOST="x86_64-pc-linux-gnu"
CFLAGS="-O2 -pipe"
CXXFLAGS="-O2 -pipe"
LDFLAGS=""
LTOFLAGS=""
DEBUG_CFLAGS=""
DEBUG_CXXFLAGS=""
BUILDENV=(!distcc color !ccache !check !sign)
OPTIONS=(!strip !docs libtool staticlibs emptydirs !zipman !purge !debug !lto)
PKGEXT='.pkg.tar.gz'
SRCEXT='.src.tar.gz'
EOF
}

# Run a pacbrew PKGBUILD with makepkg from a private copy of its directory
# (so ref/pacbrew-repo stays clean) and unpack the package into the sysroot,
# as `pacman -U` would (no pacman database: the container is throwaway).
makepkg_pkg() {
    local name=$1 dir="$WORK/pkgs/$1"
    log "makepkg $name"
    rm -rf "$dir"; mkdir -p "$dir"
    cp -a "$PACBREW/$name/." "$dir/"
    ( cd "$dir"
      SRCDEST="$WORK/dl" makepkg --config "$WORK/makepkg.conf" -d -f -C --noconfirm 2>&1 \
          | tee "$WORK/logs/makepkg-$name.log" )
    local pkg
    pkg=$(ls "$dir"/ps5-payload-*.pkg.tar.gz | head -1)
    # -m/--no-same-owner: the bind mount's parent dirs (/Users...) belong to
    # root in the container, so restoring their times/owners fails.
    bsdtar -xmf "$pkg" -C / --no-same-owner --exclude '.PKGINFO' --exclude '.BUILDINFO' \
           --exclude '.MTREE' --exclude '.INSTALL'
    # Keep the package archive, drop the build trees (KEEP_SRC=1 keeps them,
    # e.g. to read the patched Mesa source and generated files).
    [[ "${KEEP_SRC:-0}" == 1 ]] || rm -rf "$dir/src" "$dir/pkg"
}

build_payload() {
    log "gl_egl_mesa.elf"
    local hb="$PS5_PAYLOAD_SDK/target/user/homebrew"
    local src="$ROOT/spikes/02-gl-egl/src/gl_egl.c"
    [[ -f "$HERE/gl_egl.c" ]] && src="$HERE/gl_egl.c"
    # LOG_HOST: in a payload stdout already comes back over the elfldr socket;
    # point the second sink at loopback so the connect fails at once instead
    # of timing out against a LAN address.
    "$PS5_PAYLOAD_SDK/bin/prospero-clang" -Wall -Wextra -O2 -g \
        -I"$ROOT/spikes" -I"$hb/include" \
        -DLOG_HOST='"127.0.0.1"' \
        -o "$HERE/gl_egl_mesa.elf" "$src" "$HERE/mesa_env.c" \
        -L"$hb/lib" -lEGL -lGL -lpthread
    ls -l "$HERE/gl_egl_mesa.elf"
}

case "$STEP" in
    all)
        build_sdk
        build_libcxx
        write_makepkg_conf
        for p in openlibm zlib zstd mesa; do makepkg_pkg "$p"; done
        build_payload ;;
    pkgs)
        write_makepkg_conf
        for p in openlibm zlib zstd mesa; do makepkg_pkg "$p"; done
        build_payload ;;
    sdk) build_sdk ;;
    libcxx) build_libcxx ;;
    pkg-*) write_makepkg_conf; makepkg_pkg "${STEP#pkg-}" ;;
    payload) build_payload ;;
    *) echo "unknown step $STEP" >&2; exit 1 ;;
esac
log "done: $STEP"
