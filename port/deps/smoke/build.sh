#!/usr/bin/env bash
# Build the dependency smoke-test payload against toolchain/wpe-sysroot.
#   port/deps/smoke/build.sh   -> toolchain/wpe-deps/build/smoke/smoke.elf
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
. "$ROOT/env.sh"
PKGCONF="$HERE/../pkg-config-ps5"
LIBCXX="$ROOT/toolchain/libcxx19/install"
OUT="$ROOT/toolchain/wpe-deps/build/smoke"
mkdir -p "$OUT"

MODS="libsoup-3.0 gioopenssl gio-unix-2.0 fontconfig freetype2 harfbuzz openssl xkbcommon"
CFLAGS="-O2 -g0 -Wall -Wextra -Wno-unused-parameter $("$PKGCONF" --cflags $MODS)"
LIBS="$("$PKGCONF" --libs $MODS)"

prospero-clang $CFLAGS -c "$HERE/smoke.c" -o "$OUT/smoke.o"
# Link order: our objects, the sysroot libs (pkg-config --static order), then
# libc++ 19 for harfbuzz/ICU (C++), then the SDK's default libc/kernel libs.
prospero-clang -o "$OUT/smoke" "$OUT/smoke.o" -Wl,--gc-sections $LIBS \
    -nostdlib++ "$LIBCXX/lib/libc++.a" "$LIBCXX/lib/libc++abi.a" "$LIBCXX/lib/libunwind.a"
prospero-strip -o "$OUT/smoke.elf" "$OUT/smoke"
ls -la "$OUT/smoke.elf"
