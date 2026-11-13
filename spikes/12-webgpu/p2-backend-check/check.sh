#!/bin/bash
# Spike 12 P2: compiles every source of the Dawn WebGPU backend (Source/WebGPU/WebGPUDawn) for the
# PS5, outside the WebKit build, and with "link" also links them into a PS5 ELF.
#
#   ./check.sh          compile all sources; objects in obj/
#   ./check.sh link     compile, then link obj/*.o + main.cpp + libwebgpu_dawn.a + WTF into obj/check.elf
#   ./check.sh Buffer   compile only the sources whose name matches
#
# See also ./dawn-assumptions.sh, which runs on the host and checks the behaviour of Dawn that the
# backend relies on (against the null backend), and ./link.sh, which "./check.sh link" runs.
#
# The compiler and flags are those of a WebKit target in the configured build-webgl tree (read
# with `ninja -t commands`, which builds nothing), without its precompiled header.
#
# Not on the include path, on purpose: Source/WebGPU. On a case-insensitive file system Dawn's
# #include "webgpu/webgpu.h" finds Source/WebGPU/WebGPU/WebGPU.h (the C API of the Metal
# implementation) there. <WebGPU/WebGPUCpp.h> comes from a directory with nothing else in it.
#
# ENABLE_WEBGPU and HAVE_WEBGPU_IMPLEMENTATION are not defined: build-webgl's cmakeconfig.h sets
# ENABLE_WEBGPU to 0, and the backend tests neither. WTF's headers are treated as system headers,
# so that the warnings clang 18 has for them in every translation unit do not hide ours.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
. "$ROOT/env.sh"
SRC="$ROOT/toolchain/wpe-webkit/src-webgl"
BUILD="$ROOT/toolchain/wpe-webkit/build-webgl"
SYSROOT="$ROOT/toolchain/wpe-sysroot"
BACKEND="$SRC/Source/WebGPU/WebGPUDawn"
OBJ="$HERE/obj"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"

mkdir -p "$OBJ/include/WebGPU"
# <WebGPU/WebGPUCpp.h> is a copy in the build tree, which can be older than the source.
ln -sf "$SRC/Source/WebGPU/WebGPUCpp.h" "$OBJ/include/WebGPU/WebGPUCpp.h"

COMMAND="$(ninja -C "$BUILD" -t commands 2>/dev/null | grep 'CMakeFiles/WebKitShared.dir/.*unified-sources/UnifiedSource' | head -1 || true)"
if [ -z "$COMMAND" ]; then
    echo "no WebKitShared compile command in $BUILD" >&2
    exit 1
fi

# Keep the compiler and its flags. Drop the precompiled header, the dependency file, the output
# and the source, and the definitions that say the code is part of the WebKit library.
FLAGS="$(python3 - "$COMMAND" <<'PY'
import shlex, sys
args = shlex.split(sys.argv[1])
out = []
i = 0
while i < len(args):
    a = args[i]
    if a in ('-MT', '-MF', '-o'):
        i += 2
        continue
    if a in ('-MD', '-c', '-Winvalid-pch'):
        i += 1
        continue
    if a == '-Xclang':
        i += 2
        continue
    if a in ('-DBUILDING_WEBKIT', '-DBUILDING_WebKit', '-DWebKit_EXPORTS'):
        i += 1
        continue
    if i == len(args) - 1:
        break
    out.append(a)
    i += 1
print('\n'.join(out))
PY
)"

compile_one() {
    local source="$1" object="$2"
    # shellcheck disable=SC2046
    (cd "$BUILD" && IFS=$'\n' && exec $FLAGS \
        -I"$OBJ/include" -I"$BACKEND" \
        -DUSE_DAWN=1 -DSTATICALLY_LINKED_WITH_WEBGPU \
        --system-header-prefix=wtf/ --system-header-prefix=bmalloc/ \
        -fdiagnostics-color=never -fno-color-diagnostics \
        -c "$source" -o "$object")
}

FILTER=""
LINK=0
for argument in "$@"; do
    if [ "$argument" = link ]; then LINK=1; else FILTER="$argument"; fi
done

pids=()
names=()
failed=0
running=0
for source in "$BACKEND"/*.cpp; do
    name="$(basename "$source" .cpp)"
    if [ -n "$FILTER" ] && [[ "$name" != *"$FILTER"* ]]; then continue; fi
    ( compile_one "$source" "$OBJ/$name.o" > "$OBJ/$name.log" 2>&1 ) &
    pids+=($!)
    names+=("$name")
    running=$((running + 1))
    if [ "$running" -ge "$JOBS" ]; then
        wait "${pids[$(( ${#pids[@]} - running ))]}" || true
        running=$((running - 1))
    fi
done

warnings=0
for index in "${!pids[@]}"; do
    name="${names[$index]}"
    if ! wait "${pids[$index]}" 2>/dev/null; then
        # Already reaped above: decide by the object file.
        :
    fi
    if [ -s "$OBJ/$name.log" ]; then
        if grep -q 'error' "$OBJ/$name.log" || [ ! -f "$OBJ/$name.o" ]; then
            echo "FAIL $name"
            failed=$((failed + 1))
        else
            echo "WARN $name"
            warnings=$((warnings + 1))
        fi
        cat "$OBJ/$name.log"
    else
        echo "ok   $name"
    fi
done
echo "${#pids[@]} sources, $failed failed, $warnings with warnings"
[ "$failed" -eq 0 ] || exit 1

if [ "$LINK" -eq 1 ]; then
    "$HERE/link.sh"
fi
