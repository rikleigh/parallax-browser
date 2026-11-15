#!/usr/bin/env bash
# Build Spike 13 stage 0 (probe.c) as registered title PPSA99017:
#   build/titles/PPSA99017
# Packaging follows spikes/10-title-dmem/build-title.sh (PS5VR's native-app
# tooling: CRT, PIE link script, converter, fake signer, libc.prx), with
# libc.a minus its raw syscalls as the browser title links it
# (port/webkit/apps/title/package.sh): mmap/mprotect/shm_open come from
# libkernel, as they do in the browser.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
HERE="$ROOT/spikes/13-title-jit"
D10="$ROOT/spikes/10-title-dmem"
NATIVE="$ROOT/ref/PS5VR/app/tools/native-app"
PSGL="$ROOT/ref/ps5-opengl/native-app"
SDK="$PS5_PAYLOAD_SDK"; SYS="$SDK/target"
LLD="$SDK/bin/prospero-lld"
AR=/opt/homebrew/opt/llvm@18/bin/llvm-ar
B="$HERE/build"; OBJ="$B/obj"; mkdir -p "$OBJ" "$B/tool"
TITLE=PPSA99017

CF=(-O2 -g -Wall -Werror -I"$ROOT/spikes" -I"$D10"
    -DLOG_HOST="\"$PS5WK_LOG_HOST\"" -DLOG_PORT="$PS5WK_LOG_PORT")
TF=(-fno-plt -fno-stack-protector -ffunction-sections -fdata-sections)

# 1. Host converter/signer, patched as package-title.sh does (256 MiB libc heap).
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
prospero-clang -std=gnu11 "${CF[@]}" "${TF[@]}" -c "$HERE/probe.c" -o "$OBJ/probe.o"
prospero-clang -std=gnu11 "${CF[@]}" "${TF[@]}" -c "$ROOT/spikes/common/psvr2/psvr2.c" -o "$OBJ/psvr2.o"
prospero-clang -std=gnu11 "${CF[@]}" "${TF[@]}" -c "$ROOT/spikes/common/title_exit.c" -o "$OBJ/title_exit.o"
cp "$SYS/lib/libc.a" "$B/libc-title.a"
"$AR" d "$B/libc-title.a" syscalls.o

# 3. Link, convert, sign, assemble.
sdk_stubs=("$SYS/lib/libkernel.so")
for so in $(LC_ALL=C ls "$SYS"/lib/*.so); do
    case $(basename "$so") in
        libSceAgc.so|libSceAgcDriver.so|libkernel.so|libkernel_sys.so) continue ;;
    esac
    sdk_stubs+=("$so")
done
w="$B/link"; mkdir -p "$w"
"$LLD" -T "$NATIVE/ps5-pie.ld" --eh-frame-hdr \
    --version-script "$NATIVE/app-symbols.map" --exclude-libs=ALL \
    --gc-sections --error-limit=0 -e _start -o "$w/llvm-pie.elf" \
    "$OBJ"/{app_crt,runtime_shims,probe,psvr2,title_exit}.o -L"$SYS/lib" \
    "$SYS/lib/libc++.a" "$SYS/lib/libc++abi.a" "$SYS/lib/libunwind.a" \
    --as-needed "${sdk_stubs[@]}" --start-group "$B/libc-title.a" --end-group \
    2> "$w/link.log" || {
        { grep -oE 'undefined symbol: .*' "$w/link.log" | sort -u | head -40; } || true
        tail -5 "$w/link.log"; exit 1; }
"$TOOL" link --in "$w/llvm-pie.elf" --out "$w/eboot.elf" --stub-dir "$SYS/lib" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf > "$w/convert.log"
app="$B/titles/$TITLE"; rm -rf -- "$app"; mkdir -p "$app/sce_sys" "$app/sce_module"
"$TOOL" self --sign --in "$w/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F > "$w/sign.log"
cp "$NATIVE/runtime/libc.prx" "$app/sce_module/libc.prx"
# Spike 10's param.json recipe: ps5-opengl's memory settings, PS5VR's attributes.
python3 - "$PSGL/param.json" "$app/sce_sys/param.json" "$TITLE" "ps5webkit spike 13" \
    "$ROOT/ref/PS5VR/app/sce_sys/param.json" <<'PY'
import json, sys
p = json.load(open(sys.argv[1])); vr = json.load(open(sys.argv[5]))
title, name = sys.argv[3], sys.argv[4]
p["titleId"] = title
p["conceptId"] = title[4:]
p["contentId"] = f"UP9000-{title}_00-PS5WEBKITJIT0001"
p["localizedParameters"]["en-US"]["titleName"] = name
for k in ("attribute", "attribute2", "attribute3"):
    p[k] = vr.get(k, 0)
open(sys.argv[2], "w").write(json.dumps(p, indent=2) + "\n")
PY
echo "title: $app (eboot $(du -h "$app/eboot.bin" | cut -f1))"
