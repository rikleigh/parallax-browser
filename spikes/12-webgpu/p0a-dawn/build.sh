#!/usr/bin/env bash
# Spike 12 P0a: link smoke.cpp against the cross-built Dawn.
#
#   spikes/12-webgpu/p0a-dawn/build.sh          # PS5 payload ELF (link only; nothing is sent to a console)
#   spikes/12-webgpu/p0a-dawn/build.sh host     # macOS: build the same patched Dawn (null backend) and run smoke
#
# Needs `port/deps/build.sh dawn` first (libwebgpu_dawn.a + headers in
# toolchain/wpe-sysroot); `host` also needs its source tree, so run that step
# with KEEP=1. Output: spikes/12-webgpu/p0a-dawn/build/.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
. "$ROOT/env.sh"
HERE="$ROOT/spikes/12-webgpu/p0a-dawn"
SYSROOT="$ROOT/toolchain/wpe-sysroot"
LIBCXX="$ROOT/toolchain/libcxx19/install"
OUT="$HERE/build"; mkdir -p "$OUT"

if [ "${1:-ps5}" = host ]; then
    SRC="$ROOT/toolchain/wpe-deps/src/dawn"
    [ -f "$SRC/CMakeLists.txt" ] || { echo "no $SRC: run KEEP=1 port/deps/build.sh dawn" >&2; exit 1; }
    B="$OUT/dawn-host"
    # Same options as step_dawn, minus the cross toolchain and the backends
    # that need a driver's headers at run time; Apple clang, its own libc++.
    cmake -G Ninja -S "$SRC" -B "$B" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$B/install" -DCMAKE_INSTALL_LIBDIR=lib \
        -DDAWN_JINJA2_DIR="$SRC/third_party/.jinja2/src/jinja2" \
        -DDAWN_MARKUPSAFE_DIR="$SRC/third_party/.markupsafe/src/markupsafe" \
        -DDAWN_FETCH_DEPENDENCIES=OFF -DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC -DDAWN_ENABLE_INSTALL=ON \
        -DDAWN_ENABLE_VULKAN=OFF -DDAWN_ENABLE_OPENGLES=OFF -DDAWN_ENABLE_NULL=ON \
        -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_METAL=OFF \
        -DDAWN_USE_GLFW=OFF -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DDAWN_BUILD_PROTOBUF=OFF \
        -DDAWN_ENABLE_SPIRV_VALIDATION=OFF -DDAWN_WERROR=OFF \
        -DTINT_BUILD_CMD_TOOLS=OFF -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_IR_BINARY=OFF \
        -DTINT_BUILD_WGSL_READER=ON -DTINT_BUILD_SPV_WRITER=ON -DTINT_BUILD_GLSL_WRITER=ON \
        -DTINT_BUILD_SPV_READER=OFF -DTINT_BUILD_WGSL_WRITER=OFF -DTINT_BUILD_HLSL_WRITER=OFF \
        -DTINT_BUILD_MSL_WRITER=OFF -DTINT_BUILD_GLSL_VALIDATOR=OFF >"$OUT/dawn-host.configure.log" 2>&1
    ninja -C "$B" install >"$OUT/dawn-host.build.log" 2>&1 || { grep -E 'error|FAILED' "$OUT/dawn-host.build.log" | head -40; exit 1; }
    clang++ -std=c++20 -O2 -Wall -fno-exceptions -fno-rtti -I"$B/install/include" \
        "$HERE/smoke.cpp" "$B/install/lib/libwebgpu_dawn.a" \
        -framework CoreFoundation -framework IOKit -framework IOSurface -framework Foundation \
        -o "$OUT/smoke-host"
    exec "$OUT/smoke-host"
fi

DAWN="$SYSROOT/lib/libwebgpu_dawn.a"
[ -f "$DAWN" ] || { echo "no $DAWN: run port/deps/build.sh dawn" >&2; exit 1; }
# Dawn is built without exceptions and RTTI; a consumer need not match.
prospero-clang++ -std=c++20 -O2 -Wall -Werror -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti \
    -nostdinc++ -isystem "$LIBCXX/include/c++/v1" -I"$SYSROOT/include" \
    -c "$HERE/smoke.cpp" -o "$OUT/smoke.o"
# The link line a consumer needs: its objects, libwebgpu_dawn.a, libc++ 19
# (libc++, libc++abi, libunwind); prospero-clang++ adds libc and the kernel
# stubs. -Wl,-Map shows what was pulled in.
prospero-clang++ -nostdlib++ -Wl,--gc-sections -Wl,-Map="$OUT/smoke.map" \
    "$OUT/smoke.o" "$DAWN" \
    "$LIBCXX/lib/libc++.a" "$LIBCXX/lib/libc++abi.a" "$LIBCXX/lib/libunwind.a" \
    -o "$OUT/smoke.elf"
prospero-strip -o "$OUT/smoke.stripped.elf" "$OUT/smoke.elf"
echo "linked $OUT/smoke.elf"
if "$(dirname "$LLVM_CONFIG")/llvm-nm" -u "$OUT/smoke.elf" | grep .; then
    echo "note: the symbols above are imports resolved by the console's libraries at load"
fi
"$(dirname "$LLVM_CONFIG")/llvm-size" "$OUT/smoke.elf" "$OUT/smoke.stripped.elf"
ls -l "$ROOT/toolchain/wpe-sysroot/lib/libwebgpu_dawn.a" "$OUT/smoke.elf" "$OUT/smoke.stripped.elf"
