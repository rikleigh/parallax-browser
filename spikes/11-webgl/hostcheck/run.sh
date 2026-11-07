#!/usr/bin/env bash
# Host check: ANGLE (x86_64 macOS build of the same patched tree) over Spike
# 2's Rosetta build of ps5-opengl's EGL facade + the real ps5 screen + Mesa.
# Needs spikes/02-gl-egl/ps5-opengl/hostcheck/run.sh to have run (its objects
# and Mesa libraries are reused). No GPU and no PS5 shader compiler, so this
# settles ANGLE's EGL negotiation, ES context creation, feature detection and
# shader translation; drawing is the console's job.
#   spikes/11-webgl/hostcheck/run.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SPIKE2="$ROOT/spikes/02-gl-egl/ps5-opengl"
T="$SPIKE2/build/gles"; O2="$SPIKE2/build/hostcheck"; M="$T/third_party/mesa-26.2.0"
W="$ROOT/build/angle-ps5"; SRC="$W/src"; B="$W/host-build"; O="$W/host"
[[ -f $O2/obj/ps5_egl.o && -f $SRC/.ps5-patched ]] || { echo "run spike 2's hostcheck and build-angle.sh first" >&2; exit 1; }
mkdir -p "$O"
arch=(-arch x86_64 -mmacosx-version-min=13.0)

if [[ ! -f $B/build.ninja ]]; then
    cmake -G Ninja -S "$ROOT/spikes/11-webgl/angle" -B "$B" -DANGLE_PS5_HOST=ON \
        -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
        -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=Debug \
        -DANGLE_SRC="$SRC" -DHOST_EXTRA_SOURCES="$HERE/system_utils_host.cpp" > "$O/configure.log" 2>&1 \
        || { tail -20 "$O/configure.log"; exit 1; }
fi
ninja -C "$B" -j"${JOBS:-8}" angle_ps5 2>&1 | grep -E "error|FAILED|\[[0-9]+/[0-9]+\] Linking" || true

cc() { /usr/bin/clang "${arch[@]}" "$@"; }
cc -std=c11 -O1 -g -DEGL_NO_PLATFORM_SPECIFIC_TYPES=1 -I"$M/include" -c "$ROOT/spikes/11-webgl/src/egl_lookup.c" -o "$O/egl_lookup.o"
cc -std=c11 -O1 -g -c "$HERE/hostcheck_main.c" -o "$O/hostcheck_main.o"
/usr/bin/clang++ "${arch[@]}" -std=c++20 -O1 -g -DHOST_CHECK=1 -I"$SRC/include" -I"$SRC/src" \
    -c "$ROOT/spikes/11-webgl/src/angle_tests.cpp" -o "$O/angle_tests.o"

libs=(src/mesa/glapi/glapi/libglapi_bridge.a src/mesa/libmesa.a src/mesa/libmesa_sse41.a
      src/gallium/auxiliary/libgallium.a src/mesa/glapi/shared-glapi/libglapi.a
      src/compiler/glsl/libglsl.a src/compiler/glsl/glcpp/libglcpp.a src/compiler/glsl/libglsl_util.a
      src/compiler/spirv/libvtn.a src/compiler/nir/libnir.a src/compiler/libcompiler.a
      src/util/libmesa_util.a src/util/libmesa_util_simd.a src/util/libmesa_util_clflush.a
      src/util/libmesa_util_clflushopt.a src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a)
mlibs=(); for l in "${libs[@]}"; do mlibs+=("$O2/mesa/$l"); done
# Weak PS5 hooks stay NULL (no AGC backend), as in spike 2's run.sh.
weak=(); for w in $(/opt/homebrew/opt/llvm@18/bin/llvm-nm "$T"/build/core33-native-runtime/ps5_{screen,egl}.o |
                    awk '$1=="w"{print $2}' | sort -u); do weak+=("-Wl,-U,_$w"); done
objs=("$O2"/obj/ps5_screen.o "$O2"/obj/ps5_egl.o "$O2"/obj/u_framebuffer.o "$O2"/obj/platform_stubs.o)
for f in ac_formats gfx10_format_table; do [[ -f $O2/obj/$f.o ]] && objs+=("$O2/obj/$f.o"); done
[[ -f $O2/gen_stubs.o ]] && objs+=("$O2/gen_stubs.o")
/usr/bin/clang++ "${arch[@]}" -o "$O/hostcheck_angle" "$O/hostcheck_main.o" "$O/angle_tests.o" "$O/egl_lookup.o" \
    "${objs[@]}" "$B/libangle_ps5.a" "${mlibs[@]}" "${weak[@]}" -lz -framework Foundation 2> "$O/link.log" \
    || { grep -E "Undefined|symbol|error" "$O/link.log" | head -30; exit 1; }
echo "running $O/hostcheck_angle under Rosetta"
arch -x86_64 "$O/hostcheck_angle" 2>&1 | tee "$O/run.log" | grep -vE "^\[ps5-"
