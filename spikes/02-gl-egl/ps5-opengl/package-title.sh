#!/usr/bin/env bash
# Link PS5 objects against an installed ps5-opengl SDK and package them as a
# native folder title (eboot.bin + sce_sys/param.json + sce_module/libc.prx),
# replacing the deleted blackbearreloaded/ps5-native-app-boilerplate with the
# copy of its tooling vendored in ref/PS5VR (app/tools/native-app: converter,
# fake-signer, CRT, PIE link script, libc.prx; PS5VR's hardware-validated
# loader constants). ps5-opengl apps must run as a registered title, not via
# elfldr.
#
# usage: package-title.sh [-p SDK_PREFIX] [-t TITLE_ID] [-n NAME] -o OUTDIR obj.o...
#   default SDK_PREFIX = toolchain/ps5-opengl-gles, TITLE_ID = PPSA99006
#
# Install on an owner-configured console that runs native folder apps: upload
# OUTDIR/<TITLE_ID> to /data/homebrew/<TITLE_ID>, register the folder title
# once, launch it. stdout/stderr go to /download0/ps5-opengl.log
# (ps5-opengl's runtime_shims.c).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
. "$ROOT/env.sh"
PREFIX="$ROOT/toolchain/ps5-opengl-gles" TITLE=PPSA99006 NAME="ps5webkit GL probe" OUT=
while getopts p:t:n:o: opt; do
    case $opt in
        p) PREFIX=$OPTARG ;; t) TITLE=$OPTARG ;; n) NAME=$OPTARG ;; o) OUT=$OPTARG ;;
        *) sed -n '2,16p' "$0"; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[[ -n $OUT && $# -gt 0 ]] || { sed -n '2,16p' "$0"; exit 2; }
[[ $TITLE =~ ^[A-Z]{4}[0-9]{5}$ ]] || { echo "bad title id: $TITLE" >&2; exit 2; }
NATIVE="$ROOT/ref/PS5VR/app/tools/native-app"     # vendored boilerplate tooling
PSGL="$ROOT/ref/ps5-opengl/native-app"            # ps5-opengl's title runtime pieces
SDK="$PS5_PAYLOAD_SDK"; SYS="$SDK/target"
W="$HERE/build/title-$TITLE"; mkdir -p "$W/tool" "$W/obj"
LLD="$SDK/bin/prospero-lld"

# 1. Host converter/signer. ps5-opengl's build patched the boilerplate's
#    module writer to a 256 MiB libc heap (its app_heap.c wraps malloc).
TOOL="$W/tool/ps5-native-tool"
if [[ ! -x $TOOL ]]; then
    cp "$NATIVE"/{native_app_builder.cpp,self_container.cpp,self_container.hpp,elf_object.cpp,elf_object.hpp,sce_module_writer.cpp,sce_module_writer.hpp,hash.hpp} "$W/tool/"
    heap_default='write_u64(result.data, result.heap_size, std::numeric_limits<std::uint64_t>::max());'
    [[ $(grep -Fc "$heap_default" "$W/tool/sce_module_writer.cpp") == 1 ]]
    sed -i '' "s/$heap_default/write_u64(result.data, result.heap_size, 0x10000000ULL);/" \
        "$W/tool/sce_module_writer.cpp"
    /usr/bin/clang++ -std=c++20 -O2 "$W"/tool/{native_app_builder,self_container,elf_object,sce_module_writer}.cpp \
        -lz -o "$TOOL"
fi

# 2. Title runtime: boilerplate CRT (_start -> main -> catchReturnFromMain)
#    and C++ allocation runtime from PS5VR; ps5-opengl's heap + shims.
TF=(-fno-plt -fno-stack-protector -ffunction-sections -fdata-sections)
prospero-clang++ -std=c++20 -O2 -fno-exceptions -fno-rtti "${TF[@]}" -c "$NATIVE/app_crt.cpp" -o "$W/obj/app_crt.o"
# runtime_shims.c's catchReturnFromMain sleeps forever after main, and the
# system then kills the title ("debug error" on close). Renamed here, so a
# title links spikes/common/title_exit.c (or the CRT's empty default) instead.
prospero-clang -std=c11 -O2 "${TF[@]}" -DcatchReturnFromMain=ps5_opengl_catchReturnFromMain \
    -c "$PSGL/runtime_shims.c" -o "$W/obj/runtime_shims.o"
prospero-clang -std=c11 -O2 "${TF[@]}" -c "$PSGL/app_heap.c" -o "$W/obj/app_heap.o"

# 3. Intermediate PIE link (flags of ps5-opengl's tools/build-native-test-app.sh
#    + PS5VR app/scripts/build.sh).
# Order matters: an import binds to the first stub that defines it. A title
# loads libkernel.sprx, so libkernel goes first; libkernel_web only supplies
# what libkernel lacks, and libkernel_sys (the payload variant) is left out.
# Bound to libkernel_web instead, stat() was left NULL by the console's loader
# (spike 02/03b crashed in main on 13.60). LC_ALL=C: macOS sorted the glob
# with libkernel_sys/_web ahead of libkernel.so.
sdk_stubs=("$SYS/lib/libkernel.so")
for so in $(LC_ALL=C ls "$SYS"/lib/*.so); do
    case $(basename "$so") in
        libSceAgc.so|libSceAgcDriver.so|libkernel.so|libkernel_sys.so) continue ;;
    esac
    sdk_stubs+=("$so")
done
"$LLD" -T "$NATIVE/ps5-pie.ld" --eh-frame-hdr \
    --wrap=malloc --wrap=calloc --wrap=realloc --wrap=free --wrap=posix_memalign \
    --wrap=malloc_usable_size --version-script "$NATIVE/app-symbols.map" --exclude-libs=ALL \
    --gc-sections --error-limit=0 -e _start -o "$W/llvm-pie.elf" \
    "$W"/obj/*.o "$@" -u ps5_agc_gate2_run \
    --start-group "$PREFIX/lib/libPS5OpenGL.a" --end-group -L"$PREFIX/lib" -L"$SYS/lib" \
    "$SYS/lib/libc++.a" "$SYS/lib/libc++abi.a" "$SYS/lib/libunwind.a" \
    "$PREFIX/lib/libSceAgc.so" "$PREFIX/lib/libSceAgcDriver.so" \
    --as-needed "${sdk_stubs[@]}" --start-group "$SYS/lib/libc.a" --end-group \
    2> "$W/link.log" || {
        { grep -oE 'undefined symbol: .*' "$W/link.log" | sort -u | head -40; } || true
        tail -5 "$W/link.log"; exit 1; }

# 4. Convert (SCE dynamic metadata), fake-sign, assemble the folder title.
"$TOOL" link --in "$W/llvm-pie.elf" --out "$W/eboot.elf" --stub-dir "$SYS/lib" \
    --stub "$PREFIX/lib/libSceAgc.so" --stub "$PREFIX/lib/libSceAgcDriver.so" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
app="$OUT/$TITLE"; rm -rf -- "$app"; mkdir -p "$app/sce_sys" "$app/sce_module"
"$TOOL" self --sign --in "$W/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$NATIVE/runtime/libc.prx" "$app/sce_module/libc.prx"
python3 - "$PSGL/param.json" "$app/sce_sys/param.json" "$TITLE" "$NAME" <<'PY'
import json, sys
p = json.load(open(sys.argv[1]))
title, name = sys.argv[3], sys.argv[4]
p["titleId"] = title
p["conceptId"] = title[4:]
p["contentId"] = f"UP9000-{title}_00-PS5WEBKITGL00001"
p["localizedParameters"]["en-US"]["titleName"] = name
open(sys.argv[2], "w").write(json.dumps(p, indent=2) + "\n")
PY
"$TOOL" self --inspect --file "$app/eboot.bin" | grep -E 'integrity|digest' || true
echo "title: $app ($(du -sh "$app" | cut -f1))"
