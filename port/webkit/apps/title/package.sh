#!/usr/bin/env bash
# Package the browser as registered title PPSA99020: relink the ps5browser
# objects and WebKit's static libraries (the link line CMake/ninja built for
# the payload-style ps5browser ELF) as a native title with ps5-opengl, and put
# the payload child (WebProcess/NetworkProcess) in the title folder.
#
#   port/webkit/apps/title/package.sh            -> toolchain/wpe-webkit/titles/PPSA99020
#
# Based on spikes/02-gl-egl/ps5-opengl/package-title.sh (PS5VR's native-app
# tooling: CRT, PIE link script, converter, fake signer, libc.prx), with
# libc++ 19 instead of the SDK's libc++ 18 and --wrap=dlopen/dlsym for
# libepoxy (dlshim.c). Never launches anything.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="$(cd "$HERE/../.." && pwd)"
ROOT="$(cd "$PORT/../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
. "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"

TITLE="${TITLE:-PPSA99020}"
NAME="${NAME:-Parallax Browser}"
WK="$ROOT/toolchain/wpe-webkit"
BLD="${BLD:-$WK/build}"
OUT="${OUT:-$WK/titles}"
PREFIX="${PS5_OPENGL:-$ROOT/toolchain/ps5-opengl-gles}"
LIBCXX="$ROOT/toolchain/libcxx19/install"
NATIVE="$ROOT/ref/PS5VR/app/tools/native-app"
PSGL="$ROOT/ref/ps5-opengl/native-app"
SDK="$PS5_PAYLOAD_SDK"; SYS="$SDK/target"
W="$WK/title-work"; mkdir -p "$W/tool" "$W/obj"
LLD="$SDK/bin/prospero-lld"
CHILD="$BLD/bin/ps5wk-child"
[[ -f $CHILD ]] || CHILD="$BLD/ps5-apps/ps5wk-child"

# 1. Host converter/signer (same patch as package-title.sh: 256 MiB libc heap).
TOOL="$W/tool/ps5-native-tool"
if [[ ! -x $TOOL ]]; then
    cp "$NATIVE"/{native_app_builder.cpp,self_container.cpp,self_container.hpp,elf_object.cpp,elf_object.hpp,sce_module_writer.cpp,sce_module_writer.hpp,hash.hpp} "$W/tool/"
    heap_default='write_u64(result.data, result.heap_size, std::numeric_limits<std::uint64_t>::max());'
    [[ $(grep -Fc "$heap_default" "$W/tool/sce_module_writer.cpp") == 1 ]]
    sed -i '' "s/$heap_default/write_u64(result.data, result.heap_size, 0x10000000ULL);/" "$W/tool/sce_module_writer.cpp"
    /usr/bin/clang++ -std=c++20 -O2 "$W"/tool/{native_app_builder,self_container,elf_object,sce_module_writer}.cpp -lz -o "$TOOL"
fi

# 2. Title runtime objects + our shims.
TF=(-fno-plt -fno-stack-protector -ffunction-sections -fdata-sections)
prospero-clang++ -std=c++20 -O2 -fno-exceptions -fno-rtti "${TF[@]}" -c "$NATIVE/app_crt.cpp" -o "$W/obj/app_crt.o"
prospero-clang -std=c11 -O2 "${TF[@]}" -DcatchReturnFromMain=ps5_opengl_catchReturnFromMain -Dmkstemps=ps5_opengl_mkstemps \
    -c "$PSGL/runtime_shims.c" -o "$W/obj/runtime_shims.o"
# The heap: Spike 10's dheap.c (dlmalloc over direct memory) instead of
# ps5-opengl's app_heap.c, plus titlemem.c (anonymous mmaps and thread stacks
# in direct memory, memory report).
rm -f "$W/obj/app_heap.o"
D10="$ROOT/spikes/10-title-dmem"
# 32-byte blocks: WebKit's TZone types fall back to plain malloc here (bmalloc
# through the system heap) whatever their alignment, and clang stores to
# 32-byte-aligned types with vmovaps ymm: JSC's Lexer at a 16-byte address
# was a SIGBUS (code 3, pc = addr) in the title's first JavaScript (adblock.c).
prospero-clang -std=gnu11 -O2 "${TF[@]}" -DMALLOC_ALIGNMENT=32 -I"$D10" -c "$D10/dheap.c" -o "$W/obj/dheap.o"
prospero-clang -std=gnu11 -O2 -Wall "${TF[@]}" -I"$ROOT/spikes" -I"$D10" -c "$HERE/titlemem.c" -o "$W/obj/titlemem.o"
# The system keyboard: libSceImeDialog loaded and bound at run time.
prospero-clang -std=gnu11 -O2 -Wall "${TF[@]}" -I"$ROOT/spikes" -I"$HERE" -c "$HERE/titleime.c" -o "$W/obj/titleime.o"
# Hardware video decoding (step 2b): Spike 9's vdec2 + the backend for wpe-ps5's PS5VideoDecoder.
prospero-clang -std=gnu11 -O2 -Wall "${TF[@]}" -I"$ROOT/spikes/09-hw-decode" -c "$ROOT/spikes/09-hw-decode/vdec2.c" -o "$W/obj/vdec2.o"
prospero-clang -std=gnu11 -O2 -Wall "${TF[@]}" -I"$ROOT/spikes" -I"$PORT/wpe-ps5" -c "$HERE/titlevdec.c" -o "$W/obj/titlevdec.o"
# libc.a without syscalls.o (raw syscalls: illegal in a title, and they
# override libkernel's exports); titlesys.c defines what libkernel lacks.
prospero-clang -std=gnu11 -O2 -Wall "${TF[@]}" -c "$HERE/titlesys.c" -o "$W/obj/titlesys.o"
# abort() logs the thread and a backtrace first (--wrap=abort below).
prospero-clang -std=gnu11 -O2 -Wall -fno-omit-frame-pointer "${TF[@]}" -c "$HERE/titleabort.c" -o "$W/obj/titleabort.o"
cp "$SYS/lib/libc.a" "$W/libc-title.a"
/opt/homebrew/opt/llvm@18/bin/llvm-ar d "$W/libc-title.a" syscalls.o
/opt/homebrew/opt/llvm@18/bin/llvm-nm "$PREFIX/lib/libps5_opengl_core33.a" 2>/dev/null |
    awk '$2 == "T" && $3 ~ /^egl[A-Z]/ { print "EGL_SYMBOL(" $3 ")" }' | sort -u > "$W/obj/egl_symbols.inc"
prospero-clang -std=c11 -O2 "${TF[@]}" -I"$W/obj" -c "$HERE/dlshim.c" -o "$W/obj/dlshim.o"
prospero-clang -std=c11 -O2 "${TF[@]}" -I"$ROOT/spikes" -DLOG_HOST="\"$PS5WK_LOG_HOST\"" -DLOG_PORT="$PS5WK_LOG_PORT" \
    -c "$ROOT/spikes/common/title_exit.c" -o "$W/obj/title_exit.o"
prospero-clang -std=c11 -O2 "${TF[@]}" -I"$ROOT/spikes" -c "$ROOT/spikes/common/psvr2/psvr2.c" -o "$W/obj/psvr2.o"

# 3. ps5browser's objects and libraries, from the link line ninja used.
cmd="$(ninja -C "$BLD" -t commands ps5browser | tail -1)"
inputs=()
for tok in $cmd; do
    case "$tok" in
        *.o|*.a) [[ $tok = /* ]] || tok="$BLD/$tok"; inputs+=("$tok") ;;
        -l*|-L*) inputs+=("$tok") ;;
        -Wl,--wrap=*) IFS=, read -ra w <<< "${tok#-Wl,}"; inputs+=("${w[@]}") ;; # ps5compat libkernel fixes
    esac
done
# libc++ 19 is already in the list (CMAKE_CXX_STANDARD_LIBRARIES); drop the
# shim's memmem if ps5compat provides it too (first archive wins in lld).
echo "link inputs: ${#inputs[@]}"

sdk_stubs=("$SYS/lib/libkernel.so")
for so in $(LC_ALL=C ls "$SYS"/lib/*.so); do
    case $(basename "$so") in
        libSceAgc.so|libSceAgcDriver.so|libkernel.so|libkernel_sys.so) continue ;;
        # Sony's WebKit support library: its stub exports __wrap_readlink,
        # __wrap_link, __wrap_symlink and __wrap_mmap, which won over
        # libps5compat's (left NULL at run time: SIGSEGV at address 0).
        libScePosixForWebKit.so) continue ;;
    esac
    sdk_stubs+=("$so")
done
title_link() {
    "$LLD" -T "$HERE/ps5-pie-webkit.ld" --eh-frame-hdr -z nodynamic-undefined-weak \
        --wrap=malloc --wrap=calloc --wrap=realloc --wrap=free --wrap=posix_memalign \
        --wrap=aligned_alloc --wrap=memalign --wrap=malloc_usable_size \
        --wrap=mmap --wrap=munmap --wrap=madvise --wrap=abort \
        --wrap=pthread_create --wrap=pthread_join --wrap=pthread_exit \
        --wrap=dlopen --wrap=dlsym --wrap=fcntl --wrap=pipe2 --wrap=dup --wrap=dup2 \
        --wrap=recvmsg --wrap=sendmsg --wrap=recv --wrap=send --wrap=recvfrom --wrap=sendto --wrap=close \
        --version-script "$NATIVE/app-symbols.map" --exclude-libs=ALL \
        --gc-sections --error-limit=0 -e _start -o "$W/llvm-pie.elf" ${TITLE_LDFLAGS:-} "$@" \
        "$W"/obj/*.o "${inputs[@]}" "$BLD/shim/libps5shim.a" -u ps5_agc_gate2_run \
        --start-group "$PREFIX/lib/libPS5OpenGL.a" --end-group -L"$PREFIX/lib" -L"$SYS/lib" \
        "$LIBCXX/lib/libc++.a" "$LIBCXX/lib/libc++abi.a" "$LIBCXX/lib/libunwind.a" \
        "$PREFIX/lib/libSceAgc.so" "$PREFIX/lib/libSceAgcDriver.so" \
        "$SYS/lib/libScePad.so" "$SYS/lib/libSceUserService.so" \
        --as-needed "${sdk_stubs[@]}" --start-group "$W/libc-title.a" --end-group \
        2> "$W/link.log" || {
            { grep -oE 'undefined symbol: .*' "$W/link.log" | sort -u | head -60; } || true
            grep -E "duplicate symbol" -A3 "$W/link.log" | head -30 || true
            tail -5 "$W/link.log"; exit 1; }
}
title_link
# Weak references nothing provides (C++ thread_local init hooks, LSan hooks,
# __cxa_thread_atexit_impl, optional SCE calls) stay undefined dynamic
# symbols, which the converter rejects: define them as 0 (null) and relink.
NM=/opt/homebrew/opt/llvm@18/bin/llvm-nm
for so in "${sdk_stubs[@]}" "$PREFIX/lib/libSceAgc.so" "$PREFIX/lib/libSceAgcDriver.so"; do
    "$NM" -D --defined-only "$so" 2>/dev/null | awk '{print $NF}'
done | sort -u > "$W/stub-symbols.txt"
defsyms=()
for sym in $(/opt/homebrew/opt/llvm@18/bin/llvm-readelf --dyn-syms "$W/llvm-pie.elf" | awk '$7 == "UND" && $5 == "WEAK" { print $8 }'); do
    [[ $sym == ps5wk_title_check_imports || $sym == ps5wk_title_build_id ]] && continue # importcheck.o, second link
    grep -qx "$sym" "$W/stub-symbols.txt" || defsyms+=("--defsym=$sym=0")
done
(( ${#defsyms[@]} )) && echo "weak symbols set to null: ${defsyms[*]#--defsym=}"
# Every imported function, for the NULL-import check at start (importcheck.c).
/opt/homebrew/opt/llvm@18/bin/llvm-readelf --dyn-syms "$W/llvm-pie.elf" |
    awk '$7 == "UND" && $4 == "FUNC" && $5 == "GLOBAL" { sub(/@.*/, "", $8); print $8 }' |
    grep -E '^[A-Za-z_][A-Za-z0-9_]*$' | sort -u | sed 's/.*/IMPORT(&)/' > "$W/imports.inc"
echo "imports: $(wc -l < "$W/imports.inc") functions"
# Build id: logged by the title at start ("build <id>"); the unstripped
# binaries of this package are kept under $OUT/debug/<id>/ (step 5).
SRCDIR="${SRC:-$(dirname "$BLD")/$(basename "$BLD" | sed 's/^build/src/')}"
[ -e "$SRCDIR/.git" ] || SRCDIR="$WK/src" # build-cpu: the main tree
BUILD_ID="$(date +%Y%m%d-%H%M%S)-$(git -C "$SRCDIR" rev-parse --short=10 HEAD 2>/dev/null || echo nogit)"
echo "build id: $BUILD_ID"
prospero-clang -std=c11 -O2 "${TF[@]}" -fno-builtin -Wno-incompatible-library-redeclaration -I"$W" \
    -DPS5WK_TITLE_BUILD_ID="\"$BUILD_ID\"" -c "$HERE/importcheck.c" -o "$W/importcheck.o"
title_link "${defsyms[@]}" "$W/importcheck.o"
# ps5locks (copied --wrap options): no unwrapped pthread mutex/cond/rwlock import.
"$PORT/apps/common/check-lock-imports.sh" "$W/llvm-pie.elf" $(printf '%s\n' "${inputs[@]}" | sed -n 's/^--wrap=//p')

# 4. Convert, fake-sign, assemble the folder title.
"$TOOL" link --in "$W/llvm-pie.elf" --out "$W/eboot.elf" --stub-dir "$SYS/lib" \
    --stub "$PREFIX/lib/libSceAgc.so" --stub "$PREFIX/lib/libSceAgcDriver.so" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
app="$OUT/$TITLE"; rm -rf -- "$app"; mkdir -p "$app/sce_sys" "$app/sce_module"
"$TOOL" self --sign --in "$W/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$NATIVE/runtime/libc.prx" "$app/sce_module/libc.prx"
# Home screen icon (512x512 RGB PNG) and background: the system shows
# pic0.dds/pic1.dds (3840x2160 BC7, as its own titles have), made here from
# pic0.png (3840x2160) by tools/bc7dds.c.
cp "$HERE/sce_sys/icon0.png" "$HERE/sce_sys/pic0.png" "$app/sce_sys/"
BC7DDS="$W/bc7dds"
[[ $BC7DDS -nt $ROOT/tools/bc7dds.c ]] || cc -O2 -o "$BC7DDS" "$ROOT/tools/bc7dds.c" -lm
sips -s format bmp "$HERE/sce_sys/pic0.png" --out "$W/pic0.bmp" >/dev/null
"$BC7DDS" "$W/pic0.bmp" "$app/sce_sys/pic0.dds" 3
"$BC7DDS" "$W/pic0.bmp" "$app/sce_sys/pic1.dds" 1
# The logo for the browser's menu header (browserdraw.c reads /app0/ui):
# made from icon0.png by brand-assets.py.
mkdir -p "$app/ui"
python3 "$HERE/brand-assets.py" mark 48 "$app/ui/parallax-mark.bgra"
prospero-strip -o "$app/ps5wk-child.elf" "$CHILD"
# The ad blocker (port/webkit/ADBLOCK.md): uBO Lite's rules converted for
# WebKit (tools/ubol-convert.js, pinned release; generated, never committed)
# and the converter the title runs for on-console updates (/app0/adblock).
AB="$ROOT/toolchain/adblock"
[[ -f $AB/adblock-rules.json && -f $AB/adblock-info.ini ]] || node "$ROOT/tools/ubol-convert.js"
mkdir -p "$app/adblock"
cp "$AB/adblock-rules.json" "$AB/adblock-info.ini" "$PORT/adblock/dnr-to-webkit.js" "$app/adblock/"
# View in VR: PS VR2 titles declare it in param.json (Hmd2 refuses a title
# whose attribute words are 0 with 0x8a72001f, FEASIBILITY §3); PS5VR's
# values, as Spikes 3b and 9 (both also drew on the TV with them).
# VR_ATTRS=0 packages without them (no View in VR), VR_ATTRS=<attribute>:<attribute3>
# (hex) with other values, for tests.
python3 - "$PSGL/param.json" "$app/sce_sys/param.json" "$TITLE" "$NAME" "$ROOT/ref/PS5VR/app/sce_sys/param.json" "${VR_ATTRS:-}" <<'PY'
import json, sys
p = json.load(open(sys.argv[1]))
title, name = sys.argv[3], sys.argv[4]
p["titleId"] = title
p["conceptId"] = title[4:]
p["contentId"] = f"UP9000-{title}_00-PS5WEBKITBRWS01"
p["localizedParameters"]["en-US"]["titleName"] = name
vr = json.load(open(sys.argv[5]))
for k in ("attribute", "attribute2", "attribute3"):
    p[k] = vr.get(k, 0)
if sys.argv[6]:
    a = sys.argv[6].split(":") if sys.argv[6] != "0" else ["0", "0"]
    p["attribute"], p["attribute2"], p["attribute3"] = int(a[0], 16), 0, int(a[1], 16)
print("param.json: attribute 0x%x attribute3 0x%x (PS VR2)" % (p["attribute"], p["attribute3"]))
open(sys.argv[2], "w").write(json.dumps(p, indent=2) + "\n")
PY

# 5. Unstripped binaries for crash triage, per build id; the last 5 kept.
# Kernel-log backtraces of the title are load addresses: minus 0x400000.
#   llvm-addr2line -f -C -i -e $OUT/debug/<id>/ps5browser.elf 0x<addr - 0x400000>
#   (child: ps5wk-child, elf offsets from the crash guard's report)
# Debug sections are zlib-compressed (half the size; addr2line reads them).
DBG="$OUT/debug/$BUILD_ID"; mkdir -p "$DBG"
OBJCOPY=/opt/homebrew/opt/llvm@18/bin/llvm-objcopy
"$OBJCOPY" --compress-debug-sections=zlib "$W/llvm-pie.elf" "$DBG/ps5browser.elf" & p1=$!
"$OBJCOPY" --compress-debug-sections=zlib "$CHILD" "$DBG/ps5wk-child" & p2=$!
wait $p1; wait $p2
{
    echo "build id: $BUILD_ID"
    echo "packaged: $(date '+%Y-%m-%d %H:%M:%S') from $BLD"
    echo "source: $(git -C "$SRCDIR" log -1 --format='%h %s' 2>/dev/null || echo '?')"
    echo "uncommitted in source: $(git -C "$SRCDIR" status --short -uno 2>/dev/null | wc -l | tr -d ' ') files"
    (cd "$app" && shasum eboot.bin ps5wk-child.elf)
} > "$DBG/build.txt"
git -C "$SRCDIR" diff HEAD > "$DBG/source-uncommitted.diff" 2>/dev/null || true
ln -sfn "debug/$BUILD_ID/ps5browser.elf" "$OUT/ps5browser-symbols.elf"
ls -1d "$OUT"/debug/*/ | sort | sed -e :a -e '$d;N;2,5ba' -e 'P;D' | while read -r old; do rm -rf -- "$old"; done
echo "symbols: $DBG ($(du -sh "$DBG" | cut -f1))"
ls -la "$app" "$app/sce_sys"
echo "title: $app ($(du -sh "$app" | cut -f1)); eboot $(du -h "$app/eboot.bin" | cut -f1), child $(du -h "$app/ps5wk-child.elf" | cut -f1)"
