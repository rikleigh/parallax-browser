#!/usr/bin/env bash
# Build the software-decode fallback for the PS5 payload SDK: dav1d (AV1) and
# a minimal FFmpeg (h264, hevc, vp9 + libdav1d decoders, mov/ivf demuxers),
# both with their x86-64 SIMD (needs nasm: brew install nasm). Sources and
# output live in toolchain/ (gitignored).
#   spikes/09-hw-decode/sw/build-ffmpeg.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
SRC="$ROOT/toolchain/src"; PREFIX="$ROOT/toolchain/ffmpeg-sw"
mkdir -p "$SRC" "$PREFIX"
MESON="$ROOT/spikes/02-gl-egl/ps5-opengl/build/venv/bin/meson"
FFV=7.1.1 DAV1DV=1.5.1
cd "$SRC"
[[ -d ffmpeg-$FFV ]] || curl -sSL "https://ffmpeg.org/releases/ffmpeg-$FFV.tar.xz" | tar xJ
[[ -d dav1d-$DAV1DV ]] || curl -sSL "https://code.videolan.org/videolan/dav1d/-/archive/$DAV1DV/dav1d-$DAV1DV.tar.gz" | tar xz

# dav1d: meson with the SDK's cross file (its @DIRNAME@ resolved)
sed "s|@DIRNAME@|$PS5_PAYLOAD_SDK/toolchain|g" "$PS5_PAYLOAD_SDK/toolchain/prospero.ini" > "$SRC/prospero-cross.ini"
if [[ ! -f $PREFIX/lib/libdav1d.a ]]; then
    rm -rf "$SRC/dav1d-build"
    "$MESON" setup "$SRC/dav1d-build" "$SRC/dav1d-$DAV1DV" --cross-file "$SRC/prospero-cross.ini" \
        --prefix "$PREFIX" --libdir lib --buildtype release -Ddefault_library=static \
        -Denable_tools=false -Denable_tests=false -Denable_asm=true
    ninja -C "$SRC/dav1d-build" install
fi

# FFmpeg: configure as pacbrew's recipe does, minus everything we don't decode
cd "$SRC/ffmpeg-$FFV"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PREFIX/libdata/pkgconfig"
./configure --prefix="$PREFIX" --enable-cross-compile --cross-prefix="$PS5_PAYLOAD_SDK/bin/prospero-" \
    --cc="$PS5_PAYLOAD_SDK/bin/prospero-clang" --cxx="$PS5_PAYLOAD_SDK/bin/prospero-clang++" \
    --ar="$PS5_PAYLOAD_SDK/bin/prospero-ar" --ranlib="$PS5_PAYLOAD_SDK/bin/prospero-ranlib" \
    --nm=/opt/homebrew/opt/llvm@18/bin/llvm-nm --pkg-config=pkg-config \
    --arch=x86_64 --target-os=freebsd --enable-static --disable-shared \
    --disable-everything --disable-programs --disable-doc --disable-debug --disable-network \
    --disable-avdevice --disable-swresample --disable-swscale --disable-avfilter \
    --disable-autodetect --enable-pthreads --enable-libdav1d \
    --enable-decoder=h264,hevc,vp9,libdav1d --enable-parser=h264,hevc,vp9,av1 \
    --enable-demuxer=mov,ivf,h264,hevc,matroska --enable-protocol=file \
    --enable-bsf=vp9_superframe_split,h264_mp4toannexb,hevc_mp4toannexb \
    --extra-cflags="-I$PREFIX/include" --extra-ldflags="-L$PREFIX/lib" > "$SRC/ffmpeg-configure.log"
make -j14 > "$SRC/ffmpeg-make.log" 2>&1 || { tail -30 "$SRC/ffmpeg-make.log"; exit 1; }
make install > /dev/null
ls -la "$PREFIX/lib"/*.a
