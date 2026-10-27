#!/usr/bin/env bash
# Build and run hostcheck: the patched EGL facade + the real ps5 Gallium screen
# + Mesa 26.2 (gles2 enabled), compiled for x86_64 macOS and run under Rosetta.
# Needs build.sh gles to have run (uses its patched tree and generated headers).
# usage: hostcheck/run.sh [tree]     (default build/gles)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPIKE="$(cd "$HERE/.." && pwd)"
T="$(cd "${1:-$SPIKE/build/gles}" && pwd)"
O="$SPIKE/build/hostcheck"
M="$T/third_party/mesa-26.2.0"
P="$T/third_party/opengnm-psbc"
PY="$SPIKE/build/venv/bin"
export PATH="$PY:/opt/homebrew/opt/bison/bin:/opt/homebrew/bin:$PATH"
mkdir -p "$O/obj"
arch=(-arch x86_64 -mmacosx-version-min=13.0)

# 1. Host Mesa, same source tree (mesa-ps5.patch applied), same options as the
#    PS5 build except the compiler and -Dgles2=enabled.
if [[ ! -f $O/mesa/src/mesa/libmesa.a ]]; then
    cat > "$O/x86_64-darwin.ini" <<EOF
[binaries]
c = ['/usr/bin/clang', '-arch', 'x86_64', '-mmacosx-version-min=13.0']
cpp = ['/usr/bin/clang++', '-arch', 'x86_64', '-mmacosx-version-min=13.0']
objc = ['/usr/bin/clang', '-arch', 'x86_64', '-mmacosx-version-min=13.0']
objcpp = ['/usr/bin/clang++', '-arch', 'x86_64', '-mmacosx-version-min=13.0']
ar = '/opt/homebrew/opt/llvm@18/bin/llvm-ar'
[host_machine]
system = 'darwin'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
[built-in options]
default_library = 'static'
EOF
    rm -rf "$O/mesa"
    meson setup "$O/mesa" "$M" --cross-file="$O/x86_64-darwin.ini" --buildtype=debugoptimized \
        -Dplatforms= -Dgallium-drivers=softpipe -Dvulkan-drivers= -Dopengl=true -Degl=disabled \
        -Dgbm=disabled -Dgles1=disabled -Dgles2=enabled -Dglx=disabled -Dllvm=disabled \
        -Dzlib=disabled -Dzstd=disabled -Dshader-cache=disabled -Dxmlconfig=disabled \
        -Dexpat=disabled -Dlibunwind=disabled -Dvalgrind=disabled -Dvideo-codecs= \
        -Dbuild-tests=false -Dtools= -Dgallium-va=disabled -Dgallium-rusticl=false \
        -Dc_args=-Wno-thread-safety-analysis -Dcpp_args=-Wno-thread-safety-analysis > "$O/meson-setup.log"
fi
libs=(src/mesa/glapi/glapi/libglapi_bridge.a src/mesa/libmesa.a src/mesa/libmesa_sse41.a
      src/gallium/auxiliary/libgallium.a src/mesa/glapi/shared-glapi/libglapi.a
      src/compiler/glsl/libglsl.a src/compiler/glsl/glcpp/libglcpp.a src/compiler/glsl/libglsl_util.a
      src/compiler/spirv/libvtn.a src/compiler/nir/libnir.a src/compiler/libcompiler.a
      src/util/libmesa_util.a src/util/libmesa_util_simd.a src/util/libmesa_util_clflush.a
      src/util/libmesa_util_clflushopt.a src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a)
ninja -C "$O/mesa" "${libs[@]}" > "$O/ninja.log"

# 2. The PS5 runtime sources with the flags of toolchain/ps5-opengl-core33.mk.
mk="$T/toolchain/ps5-opengl-core33.mk"
core33=$(sed -n '/^PS5_OPENGL_CORE33_DEFINES := /,/^$/p' "$mk" | grep -oE '\-D[A-Z0-9_]+=[^ \\]+')
common=(-std=c11 -O1 -g -w -ffunction-sections -fdata-sections -DHAVE_FUNC_ATTRIBUTE_PACKED=1
        -I"$O/mesa/src" -I"$M/src" -I"$M/include" -I"$M/src/gallium/include"
        -I"$T/src/gallium/ps5" -I"$M/src/gallium/auxiliary")
runtime=(-DPS5_NATIVE_TITLE_RUNTIME=1 -DPS5_FRAME_SUSPEND=1 -DPS5_DRAW_PROFILE=1
         -DPS5_GPU_PRESENT_BATCH=1 -DPS5_MULTIDRAW_BATCH=1 -DPS5_DEFERRED_DRAW_BATCH=1)
cc() { /usr/bin/clang "${arch[@]}" "$@"; }
cc "${common[@]}" -DHAVE_PTHREAD=1 -DHAVE_STRUCT_TIMESPEC=1 -Wno-error=unused-parameter \
   -iquote"$P/src" -I"$P/libpsbc" -I"$P/src" $core33 "${runtime[@]}" \
   -include "$T/build/core33-native-runtime/shader-cache-build-id.h" -I"$T/src/platform" \
   -c "$T/src/gallium/ps5/ps5_screen.c" -o "$O/obj/ps5_screen.o"
cc "${common[@]}" -DHAVE_PTHREAD=1 "${runtime[@]}" -DHAVE_STRUCT_TIMESPEC=1 \
   -DPS5_ENABLE_COMPRESSED_FALLBACK_CANDIDATE=1 -DPS5_ENABLE_CORE_CONTEXT_CANDIDATE=1 \
   -DHAVE_OPENGL=1 -DHAVE_OPENGL_ES_1=0 -DHAVE_OPENGL_ES_2=1 -I"$M/src/mesa" \
   -c "$T/src/egl/ps5_egl.c" -o "$O/obj/ps5_egl.o"
cc "${common[@]}" -c "$M/src/gallium/auxiliary/util/u_framebuffer.c" -o "$O/obj/u_framebuffer.o"
# psbc's AMD format helpers (descriptor/format tables used by is_format_supported).
for f in src/amd/common/ac_formats.c src/amd/common/gfx10_format_table.c; do
    [[ -f $P/$f ]] || continue
    cc -std=gnu11 -O1 -w -D_GNU_SOURCE -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 \
       -DHAVE_PTHREAD=1 -DHAVE_STRUCT_TIMESPEC=1 -I"$P/include" -I"$P/src" -I"$P/src/amd" \
       -I"$P/src/amd/common" -I"$P/src/gallium/include" -I"$P/include/mesa" \
       -c "$P/$f" -o "$O/obj/$(basename "${f%.c}").o"
done
cc -std=c11 -O1 -g -I"$M/include" -I"$T/src/egl" -c "$HERE/hostcheck.c" -o "$O/obj/hostcheck.o"
cc "${common[@]}" -DHAVE_PTHREAD=1 -DHAVE_STRUCT_TIMESPEC=1 -DHAVE_OPENGL=1 -DHAVE_OPENGL_ES_1=0 -DHAVE_OPENGL_ES_2=1 -I"$M/src/mesa" \
   -I"$O/mesa/src/mesa" -c "$HERE/es_diag.c" -o "$O/obj/es_diag.o"
cc -std=c11 -O1 -g -c "$HERE/platform_stubs.c" -o "$O/obj/platform_stubs.o"

# 3. Link; whatever the PS5 platform still lacks becomes an aborting stub.
rm -f "$O/gen_stubs.o"
objs=("$O"/obj/*.o)
mlibs=(); for l in "${libs[@]}"; do mlibs+=("$O/mesa/$l"); done
# Weak PS5 hooks (ps5_agc_gate2_*) must stay NULL, as on the console without
# the AGC backend: leave them to dyld's flat lookup, which binds them to 0.
# Mach-O nm shows them as plain U, so take the list from the PS5 ELF objects.
weak=(); for w in $(/opt/homebrew/opt/llvm@18/bin/llvm-nm "$T"/build/core33-native-runtime/ps5_{screen,egl}.o |
                    awk '$1=="w"{print $2}' | sort -u); do
    weak+=("-Wl,-U,_$w"); done
link() { /usr/bin/clang++ "${arch[@]}" -o "$O/hostcheck" "${objs[@]}" "$@" "${mlibs[@]}" "${weak[@]}" 2> "$O/link.log"; }
: > "$O/gen_stubs.c"
if ! link; then
    grep -oE '"_[A-Za-z0-9_]+"' "$O/link.log" | tr -d '"' | sed 's/^_//' | sort -u |
        while read -r s; do
            printf 'void %s(void){__builtin_printf("hostcheck: platform stub %s reached\\n");__builtin_abort();}\n' "$s" "$s"
        done > "$O/gen_stubs.c"
    echo "generated $(wc -l < "$O/gen_stubs.c") aborting stubs: $(cut -d'(' -f1 "$O/gen_stubs.c" | sed 's/void //' | tr '\n' ' ')"
    cc -c -w "$O/gen_stubs.c" -o "$O/gen_stubs.o"
    link "$O/gen_stubs.o" || { cat "$O/link.log"; exit 1; }
fi
echo "running $O/hostcheck under Rosetta"
arch -x86_64 "$O/hostcheck"
