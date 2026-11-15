#!/usr/bin/env bash
# Build Spike 13 stage 1: the WPE build's jsc relinked as registered title
# PPSA99017 (replaces the stage 0 probe in build/titles/PPSA99017), plus the
# same build's payload jsc for the comparison runs:
#   build/titles/PPSA99017        the title (jsctitle.c is its main)
#   build/jsc-payload.elf         the payload jsc (stripped), same objects
#
#   spikes/13-title-jit/build-jsc-title.sh [build dir]   (default build-webgpu)
#
# The link follows port/webkit/apps/title/package.sh: jsc's link line from
# ninja (minus PS5Main.cpp.o, the payload entry point), the title CRT, Spike
# 10's dheap.c and titlemem.c (heap, anonymous mmaps and stacks in direct
# memory), titlesys.c and titleabort.c, libc.a minus its raw syscalls. No
# GL, no WebKit: JSC, WTF and libpas only.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
. "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
HERE="$ROOT/spikes/13-title-jit"
D10="$ROOT/spikes/10-title-dmem"
TDIR="$ROOT/port/webkit/apps/title"
BLD="${1:-$ROOT/toolchain/wpe-webkit/build-webgpu}"
NATIVE="$ROOT/ref/PS5VR/app/tools/native-app"
PSGL="$ROOT/ref/ps5-opengl/native-app"
LIBCXX="$ROOT/toolchain/libcxx19/install"
SDK="$PS5_PAYLOAD_SDK"; SYS="$SDK/target"
LLD="$SDK/bin/prospero-lld"
NM=/opt/homebrew/opt/llvm@18/bin/llvm-nm
READELF=/opt/homebrew/opt/llvm@18/bin/llvm-readelf
B="$HERE/build"; W="$B/jsc-link"; OBJ="$W/obj"; mkdir -p "$OBJ" "$B/tool"
TITLE=${TITLE:-PPSA99017} # TITLE=PPSA99018: a second copy (Spike 14 0c)

# 0. jsc in the WebKit build (only jsc.cpp.o and PS5Main.cpp.o are new), and
# the payload jsc: ninja's link line again with payloadenv.c wrapping main,
# so the payload starts with the title's JSC settings (jscenv.h).
ninja -C "$BLD" bin/jsc | tail -1
prospero-clang -std=gnu11 -O2 -Wall -Werror -c "$HERE/payloadenv.c" -o "$B/payloadenv.o"
plink="$(ninja -C "$BLD" -t commands bin/jsc | tail -1)"
[[ $plink == *" -o bin/jsc "* ]] || { echo "unexpected jsc link line" >&2; exit 1; }
plink="${plink/ -o bin\/jsc / -o $B/jsc-payload-full.elf $B/payloadenv.o -Wl,--wrap=main -Wl,--wrap=_Z12jscShellMainiPPc }"
(cd "$BLD" && eval "$plink")
prospero-strip -o "$B/jsc-payload.elf" "$B/jsc-payload-full.elf"

# 1. Host converter/signer (build-probe.sh builds it).
TOOL="$B/tool/ps5-native-tool"
[[ -x $TOOL ]] || { echo "run build-probe.sh first (it builds $TOOL)" >&2; exit 1; }

# 2. Title objects.
TF=(-fno-plt -fno-stack-protector -ffunction-sections -fdata-sections)
CF=(-O2 -Wall -Werror -I"$ROOT/spikes" -I"$D10" -DLOG_HOST="\"$PS5WK_LOG_HOST\"" -DLOG_PORT="$PS5WK_LOG_PORT")
rm -f "$OBJ"/*.o
prospero-clang++ -std=c++20 -O2 -fno-exceptions -fno-rtti "${TF[@]}" -c "$NATIVE/app_crt.cpp" -o "$OBJ/app_crt.o"
prospero-clang -std=c11 -O2 "${TF[@]}" -DcatchReturnFromMain=ps5_opengl_catchReturnFromMain -Dmkstemps=ps5_opengl_mkstemps \
    -c "$PSGL/runtime_shims.c" -o "$OBJ/runtime_shims.o"
# package.sh: 32-byte blocks (JSC's Lexer is stored with vmovaps ymm).
prospero-clang -std=gnu11 -O2 "${TF[@]}" -DMALLOC_ALIGNMENT=32 -I"$D10" -c "$D10/dheap.c" -o "$OBJ/dheap.o"
# titlemem.c's mmap wrapper becomes titlemem_mmap: jsctitle.c's __wrap_mmap
# handles executable mappings and calls it for everything.
prospero-clang -std=gnu11 -O2 -Wall "${TF[@]}" -I"$ROOT/spikes" -I"$D10" -D__wrap_mmap=titlemem_mmap \
    -c "$TDIR/titlemem.c" -o "$OBJ/titlemem.o"
prospero-clang -std=gnu11 -O2 -Wall "${TF[@]}" -c "$TDIR/titlesys.c" -o "$OBJ/titlesys.o"
prospero-clang -std=gnu11 -O2 -Wall -fno-omit-frame-pointer "${TF[@]}" -c "$TDIR/titleabort.c" -o "$OBJ/titleabort.o"
prospero-clang -std=gnu11 "${CF[@]}" "${TF[@]}" -c "$ROOT/spikes/common/psvr2/psvr2.c" -o "$OBJ/psvr2.o"
prospero-clang -std=gnu11 "${CF[@]}" "${TF[@]}" -c "$ROOT/spikes/common/title_exit.c" -o "$OBJ/title_exit.o"
prospero-clang -std=gnu11 "${CF[@]}" "${TF[@]}" -DTITLE_ID="\"$TITLE\"" -c "$HERE/jsctitle.c" -o "$OBJ/jsctitle.o"
cp "$SYS/lib/libc.a" "$W/libc-title.a"
/opt/homebrew/opt/llvm@18/bin/llvm-ar d "$W/libc-title.a" syscalls.o

# 3. jsc's objects and libraries, from the link line ninja used.
cmd="$(ninja -C "$BLD" -t commands bin/jsc | tail -1)"
inputs=()
for tok in $cmd; do
    case "$tok" in
        */PS5Main.cpp.o) ;;
        *.o|*.a) [[ $tok = /* ]] || tok="$BLD/$tok"; inputs+=("$tok") ;;
        -l*|-L*) inputs+=("$tok") ;;
        -Wl,--wrap=*) IFS=, read -ra w <<< "${tok#-Wl,}"; inputs+=("${w[@]}") ;;
    esac
done
echo "link inputs: ${#inputs[@]}"

sdk_stubs=("$SYS/lib/libkernel.so")
for so in $(LC_ALL=C ls "$SYS"/lib/*.so); do
    case $(basename "$so") in
        libSceAgc.so|libSceAgcDriver.so|libkernel.so|libkernel_sys.so|libScePosixForWebKit.so) continue ;;
    esac
    sdk_stubs+=("$so")
done
title_link() {
    "$LLD" -T "$TDIR/ps5-pie-webkit.ld" --eh-frame-hdr -z nodynamic-undefined-weak \
        --wrap=malloc --wrap=calloc --wrap=realloc --wrap=free --wrap=posix_memalign \
        --wrap=aligned_alloc --wrap=memalign --wrap=malloc_usable_size \
        --wrap=mmap --wrap=munmap --wrap=madvise --wrap=abort \
        --wrap=pthread_create --wrap=pthread_join --wrap=pthread_exit --wrap=exit \
        --version-script "$NATIVE/app-symbols.map" --exclude-libs=ALL \
        --gc-sections --error-limit=0 -e _start -o "$W/llvm-pie.elf" "$@" \
        "$OBJ"/*.o "${inputs[@]}" -L"$SYS/lib" \
        "$LIBCXX/lib/libc++.a" "$LIBCXX/lib/libc++abi.a" "$LIBCXX/lib/libunwind.a" \
        --as-needed "${sdk_stubs[@]}" --start-group "$W/libc-title.a" --end-group \
        2> "$W/link.log" || {
            { grep -oE 'undefined symbol: .*' "$W/link.log" | sort -u | head -60; } || true
            grep -E "duplicate symbol" -A3 "$W/link.log" | head -30 || true
            tail -5 "$W/link.log"; exit 1; }
}
title_link
# Weak references nothing provides: 0, as package.sh does.
for so in "${sdk_stubs[@]}"; do
    "$NM" -D --defined-only "$so" 2>/dev/null | awk '{print $NF}'
done | sort -u > "$W/stub-symbols.txt"
defsyms=()
for sym in $("$READELF" --dyn-syms "$W/llvm-pie.elf" | awk '$7 == "UND" && $5 == "WEAK" { print $8 }'); do
    grep -qx "$sym" "$W/stub-symbols.txt" || defsyms+=("--defsym=$sym=0")
done
if (( ${#defsyms[@]} )); then
    echo "weak symbols set to null: ${defsyms[*]#--defsym=}"
    title_link "${defsyms[@]}"
fi

# 4. Convert, fake-sign, assemble.
"$TOOL" link --in "$W/llvm-pie.elf" --out "$W/eboot.elf" --stub-dir "$SYS/lib" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf > "$W/convert.log"
app="$B/titles/$TITLE"; rm -rf -- "$app"; mkdir -p "$app/sce_sys" "$app/sce_module"
"$TOOL" self --sign --in "$W/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F > "$W/sign.log"
cp "$NATIVE/runtime/libc.prx" "$app/sce_module/libc.prx"
python3 - "$PSGL/param.json" "$app/sce_sys/param.json" "$TITLE" "ps5webkit spike 13 ($TITLE)" \
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
echo "title: $app (eboot $(du -h "$app/eboot.bin" | cut -f1)); payload jsc: $B/jsc-payload.elf ($(du -h "$B/jsc-payload.elf" | cut -f1))"
