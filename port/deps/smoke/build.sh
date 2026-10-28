#!/usr/bin/env bash
# Build the dependency smoke-test payloads against toolchain/wpe-sysroot.
#   port/deps/smoke/build.sh       -> toolchain/wpe-deps/build/smoke/smoke.elf     (Milestone 1)
#   port/deps/smoke/build.sh gst   -> toolchain/wpe-deps/build/smoke/gstsmoke.elf  (Milestone 2)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
. "$ROOT/env.sh"
PKGCONF="$HERE/../pkg-config-ps5"
LIBCXX="$ROOT/toolchain/libcxx19/install"
OUT="$ROOT/toolchain/wpe-deps/build/smoke"
mkdir -p "$OUT"
LIBCXX_LIBS="-nostdlib++ $LIBCXX/lib/libc++.a $LIBCXX/lib/libc++abi.a $LIBCXX/lib/libunwind.a"

if [ "${1:-}" = gst ]; then
    # Test clips, 2 s each, made on the host with FFmpeg (brew install ffmpeg:
    # libx264, libvpx, libopus, libsvtav1) and linked in as byte arrays.
    # Not committed.
    SRCV="-f lavfi -i testsrc2=size=320x180:rate=30"
    SRCA="-f lavfi -i sine=frequency=440:sample_rate=48000"
    FF="ffmpeg -hide_banner -loglevel error -y"
    [ -s "$OUT/smoke.mp4" ] || $FF $SRCV $SRCA -t 2 -c:v libx264 -profile:v main -pix_fmt yuv420p -g 30 \
        -c:a aac -b:a 96k -movflags +faststart "$OUT/smoke.mp4"
    [ -s "$OUT/smoke.webm" ] || $FF $SRCV $SRCA -t 2 -c:v libvpx-vp9 -b:v 300k -g 30 -deadline realtime \
        -c:a libopus -b:a 64k "$OUT/smoke.webm"
    [ -s "$OUT/smoke-av1.mp4" ] || $FF $SRCV -t 2 -c:v libsvtav1 -preset 10 -g 30 -pix_fmt yuv420p \
        -movflags +faststart "$OUT/smoke-av1.mp4"
    : > "$OUT/clips.c"
    for c in smoke.mp4 smoke.webm smoke-av1.mp4; do
        sym=$(echo "$c" | tr '.-' '__')
        (cd "$OUT" && cp "$c" "$sym" && xxd -i "$sym" | sed 's/^unsigned/const unsigned/' >> clips.c && rm "$sym")
    done
    MODS="gstreamer-full-1.0 gstreamer-app-1.0"
    CFLAGS="-O2 -g0 -Wall -Wextra -Wno-unused-parameter $("$PKGCONF" --cflags $MODS)"
    prospero-clang $CFLAGS -c "$HERE/gstsmoke.c" -o "$OUT/gstsmoke.o"
    prospero-clang -O2 -c "$OUT/clips.c" -o "$OUT/clips.o"
    prospero-clang -o "$OUT/gstsmoke" "$OUT/gstsmoke.o" "$OUT/clips.o" -Wl,--gc-sections \
        $("$PKGCONF" --libs gstreamer-full-1.0) $LIBCXX_LIBS
    prospero-strip -o "$OUT/gstsmoke.elf" "$OUT/gstsmoke"
    ls -la "$OUT/gstsmoke.elf"
    exit 0
fi

MODS="libsoup-3.0 gioopenssl gio-unix-2.0 fontconfig freetype2 harfbuzz openssl xkbcommon"
CFLAGS="-O2 -g0 -Wall -Wextra -Wno-unused-parameter $("$PKGCONF" --cflags $MODS)"
LIBS="$("$PKGCONF" --libs $MODS)"

prospero-clang $CFLAGS -c "$HERE/smoke.c" -o "$OUT/smoke.o"
# Link order: our objects, the sysroot libs (pkg-config --static order), then
# libc++ 19 for harfbuzz/ICU (C++), then the SDK's default libc/kernel libs.
prospero-clang -o "$OUT/smoke" "$OUT/smoke.o" -Wl,--gc-sections $LIBS $LIBCXX_LIBS
prospero-strip -o "$OUT/smoke.elf" "$OUT/smoke"
ls -la "$OUT/smoke.elf"
