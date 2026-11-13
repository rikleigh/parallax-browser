#!/usr/bin/env bash
# P0d GL probe on the Mac: the probe (HOST_CHECK) over ps5-opengl's patched EGL
# facade, the real ps5 Gallium screen (caps, limits, formats) and Mesa 26.2,
# built for x86_64 macOS and run under Rosetta. AGC and the PS5 shader
# compiler are aborting stubs, so only what stops short of shader codegen and
# the GPU runs here; the rest is reported as SKIP host.
#
# Uses, read-only, what spikes/02-gl-egl/ps5-opengl/hostcheck/run.sh built:
# the patched driver tree (build/gles) and the host Mesa libraries. The two
# driver objects are compiled again here from that tree, so they match the
# driver the browser ships (the tree is the source of
# toolchain/ps5-opengl-gles-noprof-m8; this script checks that).
#   spikes/12-webgpu/p0d-glprobe/host/run.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROBE="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$PROBE/../../.." && pwd)"
SPIKE2="$ROOT/spikes/02-gl-egl/ps5-opengl"
T="$SPIKE2/build/gles"; O2="$SPIKE2/build/hostcheck"
M="$T/third_party/mesa-26.2.0"; P="$T/third_party/opengnm-psbc"
PREFIX="$ROOT/toolchain/ps5-opengl-gles-noprof-m8"
O="$PROBE/build/host"; mkdir -p "$O/obj" "$PROBE/results"
[[ -f $O2/mesa/src/mesa/libmesa.a && -f $O2/obj/platform_stubs.o ]] ||
    { echo "run spikes/02-gl-egl/ps5-opengl/hostcheck/run.sh first" >&2; exit 1; }
NM=/opt/homebrew/opt/llvm@18/bin/llvm-nm; AR=/opt/homebrew/opt/llvm@18/bin/llvm-ar
arch=(-arch x86_64 -mmacosx-version-min=13.0)
cc() { /usr/bin/clang "${arch[@]}" "$@"; }

# 0. Is the tree what the m8 driver was built from? (Same PS5 objects.)
same=unknown
if (mkdir -p "$O/m8" && cd "$O/m8" && "$AR" x "$PREFIX/lib/libps5_opengl_core33.a" ps5_screen.o ps5_egl.o 2>/dev/null); then
    if cmp -s "$O/m8/ps5_screen.o" "$T/build/core33-native-runtime/ps5_screen.o" &&
       cmp -s "$O/m8/ps5_egl.o" "$T/build/core33-native-runtime/ps5_egl.o"; then same=yes; else same=NO; fi
fi
echo "driver tree $T matches $(basename "$PREFIX"): $same"

# 1. The driver's screen and EGL facade for the host, with the defines of the
#    m8 build (runtime-config.txt), as spike 2's hostcheck/run.sh does.
mk="$T/toolchain/ps5-opengl-core33.mk"
core33=$(sed -n '/^PS5_OPENGL_CORE33_DEFINES := /,/^$/p' "$mk" | grep -oE '\-D[A-Z0-9_]+=[^ \\]+')
common=(-std=c11 -O1 -g -w -ffunction-sections -fdata-sections -DHAVE_FUNC_ATTRIBUTE_PACKED=1
        -I"$O2/mesa/src" -I"$M/src" -I"$M/include" -I"$M/src/gallium/include"
        -I"$T/src/gallium/ps5" -I"$M/src/gallium/auxiliary")
runtime=(-DPS5_NATIVE_TITLE_RUNTIME=1 -DPS5_FRAME_SUSPEND=1 -DPS5_RUNTIME_QUIET=1
         -DPS5_GPU_PRESENT_BATCH=1 -DPS5_MULTIDRAW_BATCH=1 -DPS5_DEFERRED_DRAW_BATCH=1)
if [[ ! -f $O/obj/ps5_screen.o || $T/src/gallium/ps5/ps5_screen.c -nt $O/obj/ps5_screen.o ]]; then
    cc "${common[@]}" -DHAVE_PTHREAD=1 -DHAVE_STRUCT_TIMESPEC=1 -iquote"$P/src" -I"$P/libpsbc" -I"$P/src" \
       $core33 "${runtime[@]}" -include "$T/build/core33-native-runtime/shader-cache-build-id.h" -I"$T/src/platform" \
       -c "$T/src/gallium/ps5/ps5_screen.c" -o "$O/obj/ps5_screen.o"
fi
if [[ ! -f $O/obj/ps5_egl.o || $T/src/egl/ps5_egl.c -nt $O/obj/ps5_egl.o ]]; then
    cc "${common[@]}" -DHAVE_PTHREAD=1 "${runtime[@]}" -DHAVE_STRUCT_TIMESPEC=1 \
       -DPS5_ENABLE_COMPRESSED_FALLBACK_CANDIDATE=1 -DPS5_ENABLE_CORE_CONTEXT_CANDIDATE=1 \
       -DHAVE_OPENGL=1 -DHAVE_OPENGL_ES_1=0 -DHAVE_OPENGL_ES_2=1 -I"$M/src/mesa" \
       -c "$T/src/egl/ps5_egl.c" -o "$O/obj/ps5_egl.o"
fi

# 2. The probe.
pf=(-std=c11 -O1 -g -Wall -Werror -DHOST_CHECK=1 -DEGL_NO_PLATFORM_SPECIFIC_TYPES=1 -I"$M/include")
for f in glprobe glprobe_gpu; do cc "${pf[@]}" -c "$PROBE/src/$f.c" -o "$O/obj/$f.o"; done
cc "${pf[@]}" -c "$HERE/main_host.c" -o "$O/obj/main_host.o"

# 3. Link: spike 2's Mesa host libraries and platform stand-ins; whatever the
#    PS5 platform still lacks becomes an aborting stub.
libs=(src/mesa/glapi/glapi/libglapi_bridge.a src/mesa/libmesa.a src/mesa/libmesa_sse41.a
      src/gallium/auxiliary/libgallium.a src/mesa/glapi/shared-glapi/libglapi.a
      src/compiler/glsl/libglsl.a src/compiler/glsl/glcpp/libglcpp.a src/compiler/glsl/libglsl_util.a
      src/compiler/spirv/libvtn.a src/compiler/nir/libnir.a src/compiler/libcompiler.a
      src/util/libmesa_util.a src/util/libmesa_util_simd.a src/util/libmesa_util_clflush.a
      src/util/libmesa_util_clflushopt.a src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a)
mlibs=(); for l in "${libs[@]}"; do mlibs+=("$O2/mesa/$l"); done
# Weak PS5 hooks stay NULL (no AGC backend), as on a console without it.
weak=(); for w in $("$NM" "$T"/build/core33-native-runtime/ps5_{screen,egl}.o | awk '$1=="w"{print $2}' | sort -u); do
    weak+=("-Wl,-U,_$w"); done
objs=("$O"/obj/{main_host,glprobe,glprobe_gpu,ps5_screen,ps5_egl}.o "$O2"/obj/u_framebuffer.o "$O2"/obj/platform_stubs.o)
for f in ac_formats gfx10_format_table; do [[ -f $O2/obj/$f.o ]] && objs+=("$O2/obj/$f.o"); done
link() { /usr/bin/clang++ "${arch[@]}" -o "$O/glprobe_host" "${objs[@]}" "$@" "${mlibs[@]}" "${weak[@]}" 2> "$O/link.log"; }
: > "$O/gen_stubs.c"
if ! link; then
    grep -oE '"_[A-Za-z0-9_]+"' "$O/link.log" | tr -d '"' | sed 's/^_//' | sort -u |
        while read -r s; do
            printf 'void %s(void){__builtin_printf("hostcheck: platform stub %s reached\\n");__builtin_abort();}\n' "$s" "$s"
        done > "$O/gen_stubs.c"
    echo "aborting stubs: $(cut -d'(' -f1 "$O/gen_stubs.c" | sed 's/void //' | tr '\n' ' ')"
    cc -c -w "$O/gen_stubs.c" -o "$O/gen_stubs.o"
    link "$O/gen_stubs.o" || { cat "$O/link.log"; exit 1; }
fi

log="$PROBE/results/host-$(date +%Y-%m-%d).log"
echo "running $O/glprobe_host under Rosetta (log: $log)"
{ echo "# host run $(date '+%Y-%m-%d %H:%M'), driver tree matches $(basename "$PREFIX"): $same"
  arch -x86_64 "$O/glprobe_host" 2>&1; } > "$log" || true
grep -vE "^\[ps5-" "$log"
