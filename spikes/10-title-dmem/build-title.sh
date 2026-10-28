#!/usr/bin/env bash
# Build Spike 10 (title_dmem) as registered title PPSA99015, twice:
#   build/titles/PPSA99015      the spike (heap, range, stress, decoder, ...)
#   build/titles-pad/PPSA99015  the same title padded with ~100 MB of non-zero
#                               text + rodata (-DPAD): start-up memory only
#
# Packaging follows spikes/02-gl-egl/ps5-opengl/package-title.sh (PS5VR's
# vendored native-app tooling: CRT, PIE link script, converter, fake signer,
# libc.prx), without ps5-opengl: no GL is drawn, and its app_heap.c would
# take the malloc wraps this spike's dheap.c owns.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
HERE="$ROOT/spikes/10-title-dmem"
S09="$ROOT/spikes/09-hw-decode"
NATIVE="$ROOT/ref/PS5VR/app/tools/native-app"
PSGL="$ROOT/ref/ps5-opengl/native-app"
WPE="$ROOT/toolchain/wpe-sysroot"
SDK="$PS5_PAYLOAD_SDK"; SYS="$SDK/target"
LLD="$SDK/bin/prospero-lld"
B="$HERE/build"; OBJ="$B/obj"; mkdir -p "$OBJ" "$B/tool"
TITLE=PPSA99015

CF=(-O2 -g -Wall -Werror -I"$ROOT/spikes" -I"$HERE" -I"$S09"
    -DLOG_HOST="\"$PS5WK_LOG_HOST\"" -DLOG_PORT="$PS5WK_LOG_PORT")
TF=(-fno-plt -fno-stack-protector -ffunction-sections -fdata-sections)

# 1. Host converter/signer, patched as package-title.sh does (256 MiB libc
#    heap), so the start-up numbers match the other titles and the browser.
TOOL="$B/tool/ps5-native-tool"
if [[ ! -x $TOOL ]]; then
    cp "$NATIVE"/{native_app_builder.cpp,self_container.cpp,self_container.hpp,elf_object.cpp,elf_object.hpp,sce_module_writer.cpp,sce_module_writer.hpp,hash.hpp} "$B/tool/"
    heap_default='write_u64(result.data, result.heap_size, std::numeric_limits<std::uint64_t>::max());'
    [[ $(grep -Fc "$heap_default" "$B/tool/sce_module_writer.cpp") == 1 ]]
    sed -i '' "s/$heap_default/write_u64(result.data, result.heap_size, 0x10000000ULL);/" "$B/tool/sce_module_writer.cpp"
    /usr/bin/clang++ -std=c++20 -O2 "$B"/tool/{native_app_builder,self_container,elf_object,sce_module_writer}.cpp -lz -o "$TOOL"
fi

# 2. Objects.
prospero-clang++ -std=c++20 -O2 -fno-exceptions -fno-rtti "${TF[@]}" -c "$NATIVE/app_crt.cpp" -o "$OBJ/app_crt.o"
prospero-clang -std=c11 -O2 "${TF[@]}" -DcatchReturnFromMain=ps5_opengl_catchReturnFromMain \
    -c "$PSGL/runtime_shims.c" -o "$OBJ/runtime_shims.o"
prospero-clang -std=gnu11 "${CF[@]}" "${TF[@]}" -c "$HERE/dheap.c" -o "$OBJ/dheap.o"
prospero-clang -std=gnu11 "${CF[@]}" "${TF[@]}" -c "$HERE/title_dmem.c" -o "$OBJ/title_dmem.o"
prospero-clang -std=gnu11 "${CF[@]}" "${TF[@]}" -DPAD -Wno-unused-function -c "$HERE/title_dmem.c" -o "$OBJ/title_dmem_pad.o"
prospero-clang++ -std=c++20 "${CF[@]}" "${TF[@]}" -fno-exceptions -fno-rtti -c "$HERE/cxx_test.cpp" -o "$OBJ/cxx_test.o"
for f in vdec2 au; do
    prospero-clang -std=gnu11 "${CF[@]}" "${TF[@]}" -c "$S09/$f.c" -o "$OBJ/$f.o"
done
prospero-clang -std=gnu11 "${CF[@]}" "${TF[@]}" -c "$ROOT/spikes/common/psvr2/psvr2.c" -o "$OBJ/psvr2.o"
prospero-clang -std=gnu11 "${CF[@]}" "${TF[@]}" -c "$ROOT/spikes/common/title_exit.c" -o "$OBJ/title_exit.o"

# glib from the WPE dependency sysroot, if it is there.
GLIB_LIBS=() GLIB_WRAPS=()
if [[ -f $WPE/lib/libglib-2.0.a ]]; then
    prospero-clang -std=gnu11 "${CF[@]}" "${TF[@]}" -I"$WPE/include/glib-2.0" -I"$WPE/lib/glib-2.0/include" \
        -c "$HERE/glib_test.c" -o "$OBJ/glib.o"
    GLIB_LIBS=(--start-group "$WPE/lib/libglib-2.0.a" "$WPE/lib/libpcre2-8.a" "$WPE/lib/libintl.a"
               "$WPE/lib/libiconv.a" "$WPE/lib/libps5compat.a" --end-group)
    GLIB_WRAPS=(--wrap=readlink --wrap=eaccess --wrap=faccessat --wrap=link --wrap=symlink --wrap=ppoll)
else
    prospero-clang -std=gnu11 "${CF[@]}" "${TF[@]}" -c "$HERE/glib_stub.c" -o "$OBJ/glib.o"
fi

# Padding: ~50 MB of text and ~50 MB of rodata, all non-zero. Text is
# execute-only in a title (reading it faults with SYSTEM_XO_VIOLATION), so
# it is touched by calling it: every 4 KiB page is 4095 NOPs and a RET.
# Rodata is random bytes.
if [[ ! -f $B/pad_ro.bin || ! -f $B/pad_text_nop.bin ]]; then
    python3 -c 'import sys; sys.stdout.buffer.write((b"\x90" * 4095 + b"\xc3") * (50 * 256))' > "$B/pad_text_nop.bin"
    head -c $((50 << 20)) /dev/urandom > "$B/pad_ro.bin"
fi
prospero-clang -c "$HERE/pad.S" -DPAD_TEXT_FILE="\"$B/pad_text_nop.bin\"" -DPAD_RO_FILE="\"$B/pad_ro.bin\"" -o "$OBJ/pad.o"

# 3. Link, convert, sign, assemble. $1 = output dir, rest = objects.
sdk_stubs=("$SYS/lib/libkernel.so")
for so in $(LC_ALL=C ls "$SYS"/lib/*.so); do
    case $(basename "$so") in
        libSceAgc.so|libSceAgcDriver.so|libkernel.so|libkernel_sys.so) continue ;;
    esac
    sdk_stubs+=("$so")
done
WRAPS=()
for f in malloc calloc realloc free posix_memalign aligned_alloc memalign malloc_usable_size; do
    WRAPS+=("--wrap=$f")
done

package() {
    local out=$1; shift
    local w="$B/link-$(basename "$out")"; mkdir -p "$w"
    "$LLD" -T "$NATIVE/ps5-pie.ld" --eh-frame-hdr "${WRAPS[@]}" "${GLIB_WRAPS[@]}" \
        --version-script "$NATIVE/app-symbols.map" --exclude-libs=ALL \
        --gc-sections --error-limit=0 -e _start -o "$w/llvm-pie.elf" \
        "$OBJ"/{app_crt,runtime_shims}.o "$@" "${GLIB_LIBS[@]}" -L"$SYS/lib" \
        "$SYS/lib/libc++.a" "$SYS/lib/libc++abi.a" "$SYS/lib/libunwind.a" \
        --as-needed "${sdk_stubs[@]}" --start-group "$SYS/lib/libc.a" --end-group \
        2> "$w/link.log" || {
            { grep -oE 'undefined symbol: .*' "$w/link.log" | sort -u | head -40; } || true
            tail -5 "$w/link.log"; exit 1; }
    "$TOOL" link --in "$w/llvm-pie.elf" --out "$w/eboot.elf" --stub-dir "$SYS/lib" \
        --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf > "$w/convert.log"
    local app="$out/$TITLE"; rm -rf -- "$app"; mkdir -p "$app/sce_sys" "$app/sce_module"
    "$TOOL" self --sign --in "$w/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F > "$w/sign.log"
    cp "$NATIVE/runtime/libc.prx" "$app/sce_module/libc.prx"
    # ps5-opengl's param.json (memory settings) with PS5VR's attributes, as
    # Spike 9 (a title that decodes with Videodec2).
    python3 - "$PSGL/param.json" "$app/sce_sys/param.json" "$TITLE" "ps5webkit spike 10" \
        "$ROOT/ref/PS5VR/app/sce_sys/param.json" <<'PY'
import json, sys
p = json.load(open(sys.argv[1])); vr = json.load(open(sys.argv[5]))
title, name = sys.argv[3], sys.argv[4]
p["titleId"] = title
p["conceptId"] = title[4:]
p["contentId"] = f"UP9000-{title}_00-PS5WEBKITDMEM001"
p["localizedParameters"]["en-US"]["titleName"] = name
for k in ("attribute", "attribute2", "attribute3"):
    p[k] = vr.get(k, 0)
open(sys.argv[2], "w").write(json.dumps(p, indent=2) + "\n")
PY
    echo "title: $app (eboot $(du -h "$app/eboot.bin" | cut -f1); ELF $(du -h "$w/llvm-pie.elf" | cut -f1))"
}

COMMON=("$OBJ"/{dheap,cxx_test,glib,vdec2,au,psvr2,title_exit}.o)
package "$B/titles" "$OBJ/title_dmem.o" "${COMMON[@]}"
package "$B/titles-pad" "$OBJ/title_dmem_pad.o" "$OBJ/pad.o" "${COMMON[@]}"
