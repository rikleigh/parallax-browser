#!/usr/bin/env bash
# Package the browser as registered title PPSA99207 (sce_sys/param.json): relink the ps5browser
# objects and WebKit's static libraries (the link line CMake/ninja built for
# the payload-style ps5browser ELF) as a native title with ps5-opengl, and put
# the payload child (WebProcess/NetworkProcess) in the title folder.
#
#   port/webkit/apps/title/package.sh            -> toolchain/wpe-webkit/titles/PPSA99207
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

TITLE="${TITLE:-PPSA99207}"
NAME="${NAME:-Parallax Browser}"
WK="$ROOT/toolchain/wpe-webkit"
# The WebKit build with WebGL and every patch (port/webkit/build.sh default).
BLD="${BLD:-$WK/build-webgl}"
OUT="${OUT:-$WK/titles}"
# GPU=gl builds (the release): ps5-opengl built with the browser's patches (spikes/02-gl-egl/ps5-opengl/
# patches, through 0020: WebGL draw batching, the VAO fast path, video copies
# on the GPU, no flush or index scan of unchanged buffers, HDR MSAA resolves
# on the GPU, cube faces rendered in place) and its per-draw profiling
# compiled out: build it with
#   PS5_OPENGL_PREFIX=$ROOT/toolchain/ps5-opengl-gles-noprof-m8 \
#     spikes/02-gl-egl/ps5-opengl/build.sh gles-noprof
# The older gles-noprof (27 Oct) lacks them; gles-async is faster still but
# keeps two cores spinning and takes draws from one thread only.
PREFIX="${PS5_OPENGL:-$ROOT/toolchain/ps5-opengl-gles-noprof-m8}"
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
# GPU=radv builds (port/webkit/build.sh; HANDOVER-radv.md): the title
# renders on WebKit's ANGLE with its Vulkan backend over RADV, epoxy's GL is
# ANGLE (dlshim.c), and ps5-opengl isn't linked (Phase 5). Package them as a
# test title, e.g. TITLE=PPSA99208 NAME="Parallax RADV test" BLD=.../build-radv.
RADV_FLAGS=()
GPU_RADV=0
if grep -q '^PS5WK_GPU_RADV:[A-Z]*=ON' "$BLD/CMakeCache.txt" 2>/dev/null; then
    RADV_FLAGS=(-DPS5WK_GPU_RADV=1)
    GPU_RADV=1
    echo "GPU=radv: the title renders on ANGLE (Vulkan, RADV), without ps5-opengl"
fi
rm -f "$W/obj/runtime_shims.o" "$W/obj/titlert.o" "$W/obj/egl_symbols.inc"
if (( GPU_RADV )); then
    # stdout and stderr into a file, as ps5-opengl's runtime_shims.c does (titlert.c).
    prospero-clang -std=gnu11 -O2 -Wall "${TF[@]}" -c "$HERE/titlert.c" -o "$W/obj/titlert.o"
else
    prospero-clang -std=c11 -O2 "${TF[@]}" -DcatchReturnFromMain=ps5_opengl_catchReturnFromMain -Dmkstemps=ps5_opengl_mkstemps \
        -c "$PSGL/runtime_shims.c" -o "$W/obj/runtime_shims.o"
fi
# The heap: Spike 10's dheap.c (dlmalloc over direct memory) instead of
# ps5-opengl's app_heap.c, plus titlemem.c (anonymous mmaps and thread stacks
# in direct memory, memory report).
rm -f "$W/obj/app_heap.o"
D10="$ROOT/spikes/10-title-dmem"
# Debug aids only in a development build, one with a log host (the release,
# tools/make-release.sh, packages with PS5WK_LOG_HOST empty), as CMake's
# PS5WK_DEBUG_BUILD for ps5browser and ps5wk-child: the heap profiler
# (heapprof.c), dheap's hooks for it, and profiler.c's in-malloc backtraces.
DEBUG_FLAGS=()
if [[ -n ${PS5WK_LOG_HOST:-} ]]; then
    DEBUG_FLAGS=(-DPS5WK_DEBUG_BUILD=1)
fi
# 32-byte blocks: WebKit's TZone types fall back to plain malloc here (bmalloc
# through the system heap) whatever their alignment, and clang stores to
# 32-byte-aligned types with vmovaps ymm: JSC's Lexer at a 16-byte address
# was a SIGBUS (code 3, pc = addr) in the title's first JavaScript (adblock.c).
prospero-clang -std=gnu11 -O2 "${TF[@]}" ${DEBUG_FLAGS[@]+"${DEBUG_FLAGS[@]}"} -DMALLOC_ALIGNMENT=32 -I"$D10" -c "$D10/dheap.c" -o "$W/obj/dheap.o"
prospero-clang -std=gnu11 -O2 -Wall "${TF[@]}" ${DEBUG_FLAGS[@]+"${DEBUG_FLAGS[@]}"} -I"$ROOT/spikes" -I"$D10" -c "$HERE/titlemem.c" -o "$W/obj/titlemem.o"
# The heap profiler (development builds; off unless
# /data/ps5webkit/heap-profile exists). Removed otherwise: the link takes
# every object in $W/obj, and a development build may have left one there.
rm -f "$W/obj/heapprof.o"
if (( ${#DEBUG_FLAGS[@]} )); then
    prospero-clang -std=gnu11 -O2 -Wall "${TF[@]}" "${DEBUG_FLAGS[@]}" -I"$ROOT/spikes" -I"$D10" -c "$HERE/heapprof.c" -o "$W/obj/heapprof.o"
fi
# The system keyboard: libSceImeDialog's imports, bound once titleime.c
# loads the module, and a link-only libSceCommonDialog stub for
# sceCommonDialogInitialize, which the SDK's stubs lack (commondialog-stub.c).
prospero-clang -std=gnu11 -O2 -Wall "${TF[@]}" -I"$ROOT/spikes" -I"$HERE" -c "$HERE/titleime.c" -o "$W/obj/titleime.o"
prospero-clang -std=gnu11 -O2 -fPIC -c "$HERE/commondialog-stub.c" -o "$W/tool/commondialog-stub.o"
"$LLD" --shared -soname libSceCommonDialog.sprx -o "$W/tool/libSceCommonDialog.so" "$W/tool/commondialog-stub.o"
# Hardware video decoding (step 2b): Spike 9's vdec2 + the backend for wpe-ps5's PS5VideoDecoder.
prospero-clang -std=gnu11 -O2 -Wall "${TF[@]}" -I"$ROOT/spikes/09-hw-decode" -c "$ROOT/spikes/09-hw-decode/vdec2.c" -o "$W/obj/vdec2.o"
prospero-clang -std=gnu11 -O2 -Wall "${TF[@]}" -I"$ROOT/spikes" -I"$PORT/wpe-ps5" -c "$HERE/titlevdec.c" -o "$W/obj/titlevdec.o"
# libc.a without syscalls.o (raw syscalls: illegal in a title, and they
# override libkernel's exports); titlesys.c defines what libkernel lacks.
prospero-clang -std=gnu11 -O2 -Wall "${TF[@]}" -c "$HERE/titlesys.c" -o "$W/obj/titlesys.o"
# abort() logs the thread and a backtrace first (--wrap=abort below).
prospero-clang -std=gnu11 -O2 -Wall -fno-omit-frame-pointer "${TF[@]}" -c "$HERE/titleabort.c" -o "$W/obj/titleabort.o"
# The sampling profiler (off unless /data/ps5webkit/title-profile exists;
# ps5browser.c startTitleProfiler).
prospero-clang -std=gnu11 -O2 -Wall "${TF[@]}" ${DEBUG_FLAGS[@]+"${DEBUG_FLAGS[@]}"} -c "$HERE/../common/profiler.c" -o "$W/obj/profiler.o"
prospero-clang -std=gnu11 -O2 -Wall "${TF[@]}" -c "$HERE/../common/ps5unwind.c" -o "$W/obj/ps5unwind.o"
cp "$SYS/lib/libc.a" "$W/libc-title.a"
/opt/homebrew/opt/llvm@18/bin/llvm-ar d "$W/libc-title.a" syscalls.o
(( GPU_RADV )) || /opt/homebrew/opt/llvm@18/bin/llvm-nm "$PREFIX/lib/libps5_opengl_core33.a" 2>/dev/null |
    awk '$2 == "T" && $3 ~ /^egl[A-Z]/ { print "EGL_SYMBOL(" $3 ")" }' | sort -u > "$W/obj/egl_symbols.inc"
prospero-clang -std=c11 -O2 "${TF[@]}" ${RADV_FLAGS[@]+"${RADV_FLAGS[@]}"} -I"$W/obj" -c "$HERE/dlshim.c" -o "$W/obj/dlshim.o"
prospero-clang -std=c11 -O2 "${TF[@]}" -I"$ROOT/spikes" -DLOG_HOST="\"$PS5WK_LOG_HOST\"" -DLOG_PORT="$PS5WK_LOG_PORT" \
    -c "$ROOT/spikes/common/title_exit.c" -o "$W/obj/title_exit.o"
prospero-clang -std=c11 -O2 "${TF[@]}" -I"$ROOT/spikes" -c "$ROOT/spikes/common/psvr2/psvr2.c" -o "$W/obj/psvr2.o"

# WebGPU builds (WEBGPU=1 port/webkit/build.sh: Dawn on RADV, spikes/12-webgpu):
# RADV as one object with only its ICD entry points global (GPU=gl: ps5-opengl
# has a Mesa of its own), titleradv.c for what it needs from the title, and the payload SDK's
# AGC driver stub (116 exports, a superset of ps5-opengl's 5: RADV imports
# sceAgcDriverSetHsOffchipParam). Dawn itself is in ninja's link line.
AGCDRIVER="$PREFIX/lib/libSceAgcDriver.so"
AGC="$PREFIX/lib/libSceAgc.so"
# ps5-opengl and the libSceAgc stub it was built against (GPU=gl only).
gl_inputs=(-u ps5_agc_gate2_run --start-group "$PREFIX/lib/libPS5OpenGL.a" --end-group -L"$PREFIX/lib")
if (( GPU_RADV )); then
    gl_inputs=()
    AGC="$SYS/lib/libSceAgc.so"
    AGCDRIVER="$SYS/lib/libSceAgcDriver.so"
fi
webgpu_inputs=()
rm -f "$W/obj/cpu_model.o" "$W/obj/titleradv.o"
if grep -q '^USE_DAWN:BOOL=ON' "$BLD/CMakeCache.txt" 2>/dev/null; then
    "$ROOT/port/deps/radv/radv-object.sh"
    prospero-clang -std=gnu11 -O2 -Wall "${TF[@]}" ${RADV_FLAGS[@]+"${RADV_FLAGS[@]}"} -c "$HERE/titleradv.c" -o "$W/obj/titleradv.o"
    webgpu_inputs=("$ROOT/build/radv-ps5/radv-local.o" --wrap=dlclose)
    AGCDRIVER="$SYS/lib/libSceAgcDriver.so"
    echo "WebGPU: RADV linked in"
fi

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
        --wrap=dlopen --wrap=dlsym --wrap=access --wrap=fcntl --wrap=pipe2 --wrap=dup --wrap=dup2 \
        --wrap=recvmsg --wrap=sendmsg --wrap=recv --wrap=send --wrap=recvfrom --wrap=sendto --wrap=close --wrap=socket \
        --version-script "$NATIVE/app-symbols.map" --exclude-libs=ALL \
        --gc-sections --error-limit=0 -e _start -o "$W/llvm-pie.elf" ${TITLE_LDFLAGS:-} "$@" \
        "$W"/obj/*.o "${inputs[@]}" ${webgpu_inputs[@]+"${webgpu_inputs[@]}"} "$BLD/shim/libps5shim.a" \
        ${gl_inputs[@]+"${gl_inputs[@]}"} -L"$SYS/lib" \
        "$LIBCXX/lib/libc++.a" "$LIBCXX/lib/libc++abi.a" "$LIBCXX/lib/libunwind.a" \
        "$AGC" "$AGCDRIVER" \
        "$SYS/lib/libScePad.so" "$SYS/lib/libSceUserService.so" "$SYS/lib/libSceImeDialog.so" "$W/tool/libSceCommonDialog.so" \
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
for so in "${sdk_stubs[@]}" "$AGC" "$AGCDRIVER"; do
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
    --stub "$AGC" --stub "$AGCDRIVER" --stub "$W/tool/libSceCommonDialog.so" \
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
# made from icon0.png by brand-assets.py; and for the link server's pages
# (GET /logo.png).
mkdir -p "$app/ui"
python3 "$HERE/brand-assets.py" mark 48 "$app/ui/parallax-mark.bgra"
cp "$PORT/runtime/parallax-logo.png" "$app/ui/parallax-logo.png"
prospero-strip -o "$app/ps5wk-child.elf" "$CHILD"
# The restart helper (apps/relaunch/ps5wk-relaunch.c): the title sends it to
# elfldr to be closed and started again (Restart browser), and to install an
# update. Always rebuilt (-B): with the update test hooks for a development
# build (a log host), without for a release.
make -s -B -C "$HERE/../relaunch" UPDATE_TESTING="$([[ -n ${PS5WK_LOG_HOST:-} ]] && echo 1 || echo 0)" >/dev/null
prospero-strip -o "$app/ps5wk-relaunch.elf" "$HERE/../relaunch/ps5wk-relaunch.elf"
# The ad blocker (port/webkit/ADBLOCK.md): uBO Lite's rules converted for
# WebKit (tools/ubol-convert.js, pinned release; generated, never committed)
# and the converter the title runs for on-console updates (/app0/adblock).
AB="$ROOT/toolchain/adblock"
[[ -f $AB/adblock-rules.json && -f $AB/adblock-info.ini && -f $AB/adblock-scripting.txt ]] || node "$ROOT/tools/ubol-convert.js"
mkdir -p "$app/adblock"
cp "$AB/adblock-rules.json" "$AB/adblock-info.ini" "$AB/adblock-scripting.txt" "$PORT/adblock/dnr-to-webkit.js" \
    "$PORT/adblock/ubol-scripting.js" "$app/adblock/"
# The runtime files: fonts, the CA bundle, ICU, XKB and MIME data (port/deps),
# the start page and the logo, in runtime/. The title copies them into
# /data/ps5webkit when it starts and runtime-version differs from the one
# there (runtime.c): a store installs and updates only the title folder, and
# the payload children can't see /app0. START_HTML: another start page
# (make-release.sh: without the test links). NO_RUNTIME=1 leaves them out
# (a faster upload while developing; the console keeps what it has).
if [[ -z ${NO_RUNTIME:-} ]]; then
    RUNTIME="$ROOT/toolchain/wpe-sysroot/share/ps5webkit-runtime"
    ICUDAT="$ROOT/toolchain/icu/src-76.1/icu/source/data/in/icudt76l.dat"
    [[ -f $RUNTIME/etc/ssl/cert.pem && -f $ICUDAT ]] ||
        { echo "missing $RUNTIME or $ICUDAT: run port/deps/build.sh cacert fonts xkeyboard_config and spikes/04-jsc/build.sh" >&2; exit 1; }
    rt="$app/runtime"; mkdir -p "$rt/share/icu"
    rsync -aL --exclude .DS_Store "$RUNTIME/" "$rt/"
    cp "$ICUDAT" "$rt/share/icu/icudt76l.dat"
    cp "${START_HTML:-$PORT/runtime/start.html}" "$rt/start.html"
    cp "$PORT/runtime/parallax-logo.png" "$rt/"
    # The list of files: the title can open files in /app0 but not list its
    # folders (EPERM), so runtime.c copies what this names.
    (cd "$rt" && find . -type f ! -name runtime-version ! -name runtime-files.txt | sed 's|^\./||' | LC_ALL=C sort) > "$W/runtime-files.txt"
    mv "$W/runtime-files.txt" "$rt/runtime-files.txt"
    (cd "$rt" && find . -type f ! -name runtime-version -print0 | LC_ALL=C sort -z | xargs -0 shasum -a 256 | shasum -a 256 | cut -c1-16) > "$rt/runtime-version"
    echo "runtime: $(find "$rt" -type f | wc -l | tr -d ' ') files, $(du -sh "$rt" | cut -f1), version $(cat "$rt/runtime-version")"
fi
# param.json: the committed sce_sys/param.json, which the homebrew catalog
# reads too (titleId, contentVersion: raise contentVersion in every release,
# 01.<minor>.<patch>, so consoles see an update). View in VR: PS VR2 titles
# declare it there (Hmd2 refuses a title whose attribute words are 0 with
# 0x8a72001f, FEASIBILITY §3); PS5VR's values, as Spikes 3b and 9.
# TITLE/NAME other than the file's change the IDs and name (test titles);
# VR_ATTRS=0 packages without the VR attributes (no View in VR),
# VR_ATTRS=<attribute>:<attribute3> (hex) with other values, for tests.
python3 - "$HERE/sce_sys/param.json" "$app/sce_sys/param.json" "$TITLE" "$NAME" "${VR_ATTRS:-}" <<'PY'
import json, sys
p = json.load(open(sys.argv[1]))
title, name = sys.argv[3], sys.argv[4]
if title != p["titleId"]:
    p["titleId"] = title
    p["conceptId"] = title[4:]
    p["contentId"] = f"UP9000-{title}_00-PS5WEBKITBRWS01"
p["localizedParameters"]["en-US"]["titleName"] = name
if sys.argv[5]:
    a = sys.argv[5].split(":") if sys.argv[5] != "0" else ["0", "0"]
    p["attribute"], p["attribute2"], p["attribute3"] = int(a[0], 16), 0, int(a[1], 16)
print("param.json: %s %s, attribute 0x%x attribute3 0x%x (PS VR2)" % (p["titleId"], p["contentVersion"], p["attribute"], p["attribute3"]))
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
