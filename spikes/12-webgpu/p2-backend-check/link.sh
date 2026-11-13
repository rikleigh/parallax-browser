#!/bin/bash
# Spike 12 P2: links the backend's objects (./check.sh first) with main.cpp, libwebgpu_dawn.a and
# the WTF and bmalloc objects of build-webgl into a PS5 ELF, and lists the symbols that stay
# undefined, by what would define them. Nothing is run or sent to a console.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
. "$ROOT/env.sh"
SRC="$ROOT/toolchain/wpe-webkit/src-webgl"
BUILD="$ROOT/toolchain/wpe-webkit/build-webgl"
SYSROOT="$ROOT/toolchain/wpe-sysroot"
LIBCXX="$ROOT/toolchain/libcxx19/install"
BACKEND="$SRC/Source/WebGPU/WebGPUDawn"
OBJ="$HERE/obj"
NM="$(dirname "$LLVM_CONFIG")/llvm-nm"

# main.cpp with the flags of the backend.
COMMAND="$(ninja -C "$BUILD" -t commands 2>/dev/null | grep 'CMakeFiles/WebKitShared.dir/.*unified-sources/UnifiedSource' | head -1 || true)"
FLAGS="$(python3 - "$COMMAND" <<'PY'
import shlex, sys
args = shlex.split(sys.argv[1])
out = []
i = 0
while i < len(args) - 1:
    a = args[i]
    if a in ('-MT', '-MF', '-o', '-Xclang'):
        i += 2
        continue
    if a in ('-MD', '-c', '-Winvalid-pch', '-DBUILDING_WEBKIT', '-DBUILDING_WebKit', '-DWebKit_EXPORTS'):
        i += 1
        continue
    out.append(a)
    i += 1
print('\n'.join(out))
PY
)"
(cd "$BUILD" && IFS=$'\n' && $FLAGS -I"$OBJ/include" -I"$BACKEND" -DUSE_DAWN=1 -DSTATICALLY_LINKED_WITH_WEBGPU \
    --system-header-prefix=wtf/ --system-header-prefix=bmalloc/ -c "$HERE/main.cpp" -o "$OBJ/main.main.o")

# The backend as the static library the build makes of it.
rm -f "$OBJ/libWebGPUDawn.a"
BACKEND_OBJECTS=()
for source in "$BACKEND"/*.cpp; do
    BACKEND_OBJECTS+=("$OBJ/$(basename "$source" .cpp).o")
done
"$(dirname "$LLVM_CONFIG")/llvm-ar" rcs "$OBJ/libWebGPUDawn.a" "${BACKEND_OBJECTS[@]}"

# WTF and bmalloc are object libraries in the build tree.
find "$BUILD/Source/WTF" "$BUILD/Source/bmalloc" -name '*.o' > "$OBJ/wtf-objects.rsp"
rm -f "$OBJ/libWTFCheck.a"
"$(dirname "$LLVM_CONFIG")/llvm-ar" rcs "$OBJ/libWTFCheck.a" @"$OBJ/wtf-objects.rsp"

# Undefined symbols are warnings, so that the link finishes and they can be listed.
prospero-clang++ -nostdlib++ -Wl,--gc-sections -Wl,--warn-unresolved-symbols -Wl,-Map="$OBJ/check.map" \
    "$OBJ/main.main.o" "$OBJ/libWebGPUDawn.a" "$OBJ/libWTFCheck.a" "$SYSROOT/lib/libwebgpu_dawn.a" \
    "$LIBCXX/lib/libc++.a" "$LIBCXX/lib/libc++abi.a" "$LIBCXX/lib/libunwind.a" \
    -o "$OBJ/check.elf" > "$OBJ/link.log" 2>&1 || { cat "$OBJ/link.log" | head -60; echo "link failed" >&2; exit 1; }

"$NM" -u "$OBJ/check.elf" | awk '{print $2}' | sort -u > "$OBJ/undefined.txt"
"$NM" -u "$OBJ/libWebGPUDawn.a" 2>/dev/null | awk '$1 == "U" {print $2}' | sort -u > "$OBJ/backend-needs.txt"
"$NM" --defined-only "$OBJ/libWebGPUDawn.a" "$OBJ/libWTFCheck.a" "$SYSROOT/lib/libwebgpu_dawn.a" "$LIBCXX/lib/libc++.a" "$LIBCXX/lib/libc++abi.a" 2>/dev/null | awk 'NF == 3 {print $3}' | sort -u > "$OBJ/defined-here.txt"

echo "linked $OBJ/check.elf ($(wc -c < "$OBJ/check.elf" | tr -d ' ') bytes)"
# The ELF imports libc, libm and pthread from the system. Anything of ours left undefined is a bug.
echo "undefined in the ELF: $(wc -l < "$OBJ/undefined.txt" | tr -d ' ') (obj/undefined.txt)"
echo "  of WebGPU, Dawn, Tint, WTF, bmalloc, JavaScriptCore, ICU or libc++:"
grep -E 'WebGPU|wgpu|dawn|tint|WTF|bmalloc|pas_|JSC|icu|^u[a-z]+_|^_ZNSt|^_ZSt|^__cxa|^_Z' "$OBJ/undefined.txt" | sed 's/^/    /' || echo "    none"
# What the backend's own objects need that none of the backend, WTF, bmalloc, Dawn and libc++ define.
comm -23 "$OBJ/backend-needs.txt" "$OBJ/defined-here.txt" > "$OBJ/backend-unresolved.txt"
echo "needed by the backend and defined by none of the backend, WTF, bmalloc, Dawn, libc++: $(tr '\n' ' ' < "$OBJ/backend-unresolved.txt")"
