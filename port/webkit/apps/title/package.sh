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
NAME="${NAME:-ps5webkit browser}"
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
prospero-clang -std=gnu11 -O2 "${TF[@]}" -I"$D10" -c "$D10/dheap.c" -o "$W/obj/dheap.o"
prospero-clang -std=gnu11 -O2 -Wall "${TF[@]}" -I"$ROOT/spikes" -I"$D10" -c "$HERE/titlemem.c" -o "$W/obj/titlemem.o"
# libc.a without syscalls.o (raw syscalls: illegal in a title, and they
# override libkernel's exports); titlesys.c defines what libkernel lacks.
prospero-clang -std=gnu11 -O2 -Wall "${TF[@]}" -c "$HERE/titlesys.c" -o "$W/obj/titlesys.o"
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
        --wrap=mmap --wrap=munmap --wrap=madvise \
        --wrap=pthread_create --wrap=pthread_join --wrap=pthread_exit \
        --wrap=dlopen --wrap=dlsym --wrap=fcntl --wrap=pipe2 --wrap=dup --wrap=dup2 \
        --wrap=recvmsg --wrap=sendmsg --wrap=recv --wrap=send --wrap=recvfrom --wrap=sendto --wrap=close \
        --version-script "$NATIVE/app-symbols.map" --exclude-libs=ALL \
        --gc-sections --error-limit=0 -e _start -o "$W/llvm-pie.elf" ${TITLE_LDFLAGS:-} "$@" \
        "$W"/obj/*.o "${inputs[@]}" "$BLD/shim/libps5shim.a" -u ps5_agc_gate2_run \
        --start-group "$PREFIX/lib/libPS5OpenGL.a" --end-group -L"$PREFIX/lib" -L"$SYS/lib" \
        "$LIBCXX/lib/libc++.a" "$LIBCXX/lib/libc++abi.a" "$LIBCXX/lib/libunwind.a" \
        "$PREFIX/lib/libSceAgc.so" "$PREFIX/lib/libSceAgcDriver.so" \
        "$SYS/lib/libScePad.so" "$SYS/lib/libSceImeDialog.so" "$SYS/lib/libSceUserService.so" \
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
    [[ $sym == ps5wk_title_check_imports ]] && continue # importcheck.o, second link
    grep -qx "$sym" "$W/stub-symbols.txt" || defsyms+=("--defsym=$sym=0")
done
(( ${#defsyms[@]} )) && echo "weak symbols set to null: ${defsyms[*]#--defsym=}"
# Every imported function, for the NULL-import check at start (importcheck.c).
/opt/homebrew/opt/llvm@18/bin/llvm-readelf --dyn-syms "$W/llvm-pie.elf" |
    awk '$7 == "UND" && $4 == "FUNC" && $5 == "GLOBAL" { sub(/@.*/, "", $8); print $8 }' |
    grep -E '^[A-Za-z_][A-Za-z0-9_]*$' | sort -u | sed 's/.*/IMPORT(&)/' > "$W/imports.inc"
echo "imports: $(wc -l < "$W/imports.inc") functions"
prospero-clang -std=c11 -O2 "${TF[@]}" -fno-builtin -Wno-incompatible-library-redeclaration -I"$W" \
    -c "$HERE/importcheck.c" -o "$W/importcheck.o"
title_link "${defsyms[@]}" "$W/importcheck.o"

# Symbols for crash triage (kernel-log rips minus the 0x400000 load base).
cp "$W/llvm-pie.elf" "$OUT/ps5browser-symbols.elf"

# 4. Convert, fake-sign, assemble the folder title.
"$TOOL" link --in "$W/llvm-pie.elf" --out "$W/eboot.elf" --stub-dir "$SYS/lib" \
    --stub "$PREFIX/lib/libSceAgc.so" --stub "$PREFIX/lib/libSceAgcDriver.so" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
app="$OUT/$TITLE"; rm -rf -- "$app"; mkdir -p "$app/sce_sys" "$app/sce_module"
"$TOOL" self --sign --in "$W/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$NATIVE/runtime/libc.prx" "$app/sce_module/libc.prx"
prospero-strip -o "$app/ps5wk-child.elf" "$CHILD"
python3 - "$PSGL/param.json" "$app/sce_sys/param.json" "$TITLE" "$NAME" <<'PY'
import json, sys
p = json.load(open(sys.argv[1]))
title, name = sys.argv[3], sys.argv[4]
p["titleId"] = title
p["conceptId"] = title[4:]
p["contentId"] = f"UP9000-{title}_00-PS5WEBKITBRWS01"
p["localizedParameters"]["en-US"]["titleName"] = name
open(sys.argv[2], "w").write(json.dumps(p, indent=2) + "\n")
PY
ls -la "$app" "$app/sce_sys"
echo "title: $app ($(du -sh "$app" | cut -f1)); eboot $(du -h "$app/eboot.bin" | cut -f1), child $(du -h "$app/ps5wk-child.elf" | cut -f1)"
