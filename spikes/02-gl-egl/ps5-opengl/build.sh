#!/usr/bin/env bash
# Reproducible macOS (arm64) build of the ps5-opengl SDK (mihawk-99/ps5-opengl
# @ ec06d8c) in two variants:
#
#   stock : upstream sources, upstream Mesa config (-Dgles2=disabled)
#           -> $PS5WK_ROOT/toolchain/ps5-opengl-stock
#   gles  : stock + patches/*.patch (OpenGL ES 2/3 contexts through the EGL
#           facade, Mesa -Dgles2=enabled, GLES2/GLES3 headers, and the PS5
#           texture-descriptor export entry point)
#           -> $PS5WK_ROOT/toolchain/ps5-opengl-gles
#
# usage: build.sh [stock|gles|all] [--clean]     (default: all)
#
# No sudo, no Docker. Inputs: ref/ps5-opengl (git checkout, read only), the
# pinned sources from its dependencies.json (fetched into each variant's own
# third_party/, git commits + Mesa sha256 verified by upstream fetch-sources.py),
# our ps5-payload-sdk (env.sh) and these host tools:
#   Homebrew: llvm@18 (SDK compiler), bison (keg-only, >= 3; macOS ships 2.3),
#             coreutils (GNU realpath -m / sha256sum / install), bash >= 4, ninja
#   Python >= 3.11 (venv created here with meson 1.10.1, mako, pyyaml, packaging)
# Everything else lands in spikes/02-gl-egl/ps5-opengl/build/ (gitignored).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
B="$HERE/build"
REF="$ROOT/ref/ps5-opengl"
PIN=ec06d8c
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

variant=all clean=0
for arg in "$@"; do
    case "$arg" in
        stock|gles|all) variant=$arg ;;
        --clean) clean=1 ;;
        -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

log() { printf '==> %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

# --- host environment ---------------------------------------------------------
# shellcheck disable=SC1091
. "$ROOT/env.sh"
[[ -x $PS5_PAYLOAD_SDK/bin/prospero-clang ]] || die "payload SDK missing: $PS5_PAYLOAD_SDK"
[[ -d $REF/.git ]] || die "missing $REF (git clone of mihawk-99/ps5-opengl)"
git -C "$REF" cat-file -e "$PIN^{commit}" || die "$REF lacks commit $PIN"
BREW=/opt/homebrew
for p in "$BREW/opt/llvm@18/bin/llvm-ar" "$BREW/opt/bison/bin/bison" \
         "$BREW/opt/coreutils/libexec/gnubin/realpath" "$BREW/bin/bash" "$BREW/bin/ninja"; do
    [[ -x $p ]] || die "missing $p (brew install llvm@18 bison coreutils bash ninja)"
done

mkdir -p "$B/shims"
# GNU ar MRI scripts (install step materialises thin archives) and ELF-aware nm.
ln -sf "$BREW/opt/llvm@18/bin/llvm-ar" "$B/shims/ar"
ln -sf "$BREW/opt/llvm@18/bin/llvm-nm" "$B/shims/nm"
if [[ ! -x $B/venv/bin/meson ]]; then
    log "python venv (meson 1.10.1, mako, pyyaml, packaging)"
    py=$(command -v python3.13 || command -v python3.12 || command -v python3)
    "$py" -m venv "$B/venv"
    "$B/venv/bin/pip" install -q meson==1.10.1 mako pyyaml packaging
fi
export PATH="$B/venv/bin:$BREW/opt/bison/bin:$B/shims:$BREW/opt/coreutils/libexec/gnubin:$BREW/bin:$PATH"
export PS5_CCACHE=0 PSBC_JOBS="$JOBS"
bison --version | head -1 | grep -q ' 3\.' || die "bison 3.x not first on PATH"

# --- one variant -----------------------------------------------------------------
build_variant() {
    local name=$1 prefix=$2; shift 2
    local tree="$B/$name"
    # (a second pass: macOS indexers can repopulate a directory mid-delete)
    if (( clean )); then rm -rf -- "$tree" 2>/dev/null || rm -rf -- "$tree"; fi
    if [[ ! -d $tree ]]; then
        log "[$name] export $PIN"
        mkdir -p "$tree"
        git -C "$REF" archive "$PIN" | tar -x -C "$tree"
        # A throwaway repo makes the applied patches visible with `git diff`.
        git -C "$tree" init -q
        git -C "$tree" add -A
        git -C "$tree" -c user.name=build -c user.email=build@localhost commit -qm "ps5-opengl $PIN"
        for patch in "$@"; do
            log "[$name] apply $(basename "$patch")"
            git -C "$tree" apply --whitespace=nowarn "$patch"
        done
    fi
    cd "$tree"

    log "[$name] fetch pinned sources (dependencies.json)"
    python3 tools/fetch-sources.py

    log "[$name] PSBC generated sources"
    # = toolchain/build-opengnm-psbc.sh without its host-binary build (the host
    # psbc CLI only serves Linux-only host tests).
    (
        cd third_party/opengnm-psbc
        python3 src/util/format/u_format_table.py src/util/format/u_format.yaml --enums > src/util/format/u_format_gen.h
        python3 src/util/format/u_format_table.py src/util/format/u_format.yaml --header > src/util/format/u_format_pack.h
        python3 src/util/format/u_format_table.py src/util/format/u_format.yaml > src/util/format/u_format_table.c
        python3 src/util/format_srgb.py > src/util/format_srgb.c
        python3 src/compiler/builtin_types_h.py src/compiler/builtin_types.h
        python3 src/compiler/builtin_types_c.py src/compiler/builtin_types.c
        python3 src/util/process_shader_stats.py src/util/shader_stats.rnc src/util/shader_stats.xml > src/util/shader_stats.h
        python3 src/vulkan/util/vk_struct_type_cast_gen.py --xml src/vulkan/registry/vk.xml \
            --out src/vulkan/util/vk_struct_type_cast.h --beta false
        for g in 11 12; do
            python3 src/amd/packets/parse_cp_pm4_table_data_json.py \
                src/amd/packets/cp_pm4_table_data_gfx11.json src/amd/packets/pm4_it_opcodes_gfx11.h \
                src/amd/packets/cp_pm4_table_data_gfx12.json src/amd/packets/pm4_it_opcodes_gfx12.h \
                "gfx$g" packets_h > "src/amd/common/amd_cp_packets_gfx$g.h"
        done
        python3 src/amd/common/gfx10_format_table.py src/util/format/u_format.yaml \
            src/amd/registers/gfx10-rsrc.json src/amd/registers/gfx11-rsrc.json \
            > src/amd/common/gfx10_format_table.c
        make -j"$JOBS" CONFIG=../../toolchain/opengnm-psbc-host.mak generated > /dev/null
    )

    log "[$name] PSBC compiler archive for PS5 (libpsbc.ps5.a)"
    bash toolchain/build-opengnm-psbc-ps5.sh --if-needed > "$B/$name-psbc.log" 2>&1 ||
        { tail -30 "$B/$name-psbc.log"; die "psbc build failed"; }

    log "[$name] Mesa 26.2.0 (+ upstream mesa-ps5.patch)"
    PS5_MESA_CROSS_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.ini" \
        bash toolchain/build-mesa-ps5.sh > "$B/$name-mesa.log" 2>&1 ||
        { grep -A8 FAILED "$B/$name-mesa.log" | head -40; die "mesa build failed"; }
    grep -E '^ *(gles1|gles2|OpenGL|ES1|ES2)' "$B/$name-mesa.log" | sed 's/^/    /' || true

    log "[$name] runtime (EGL facade + ps5 gallium driver + AGC backend) and install"
    rm -rf -- "$prefix"
    bash toolchain/install-ps5-opengl-gl46.sh "$prefix" > "$B/$name-install.log" 2>&1 ||
        { tail -30 "$B/$name-install.log"; die "install failed"; }
    tail -2 "$B/$name-install.log" | sed 's/^/    /'
    grep -h 'static wrappers' "$B/$name-install.log" | sed 's/^/    /' || true

    log "[$name] checks"
    python3 tests/ps5/verify_gl46_link_surface.py | sed 's/^/    /'
    (cd "$prefix" && sha256sum --check --strict --quiet manifest.sha256) && echo "    manifest OK"
    printf '    EGL_CLIENT_APIS in facade: "%s"\n' \
        "$(strings build/core33-native-runtime/ps5_egl.o | grep -m1 -E '^OpenGL( OpenGL_ES)?$')"
    cd "$HERE"
    if [[ $name == gles ]]; then
        log "[$name] consumer link probe (examples/gles3_probe.c -> payload-style ELF)"
        local out="$B/$name-probe"
        mkdir -p "$out"
        prospero-clang -std=c11 -O2 -Wall -Werror -DGL_GLEXT_PROTOTYPES=1 -I"$prefix/include" \
            -c "$HERE/examples/gles3_probe.c" -o "$out/gles3_probe.o"
        # Same libraries as lib/pkgconfig/ps5-opengl.pc; -z defs => no unresolved symbol.
        prospero-clang -Wl,-z,defs -Wl,--gc-sections "$out/gles3_probe.o" \
            -Wl,-u,ps5_agc_gate2_run -L"$prefix/lib" -Wl,--start-group -lPS5OpenGL -Wl,--end-group \
            "$prefix/lib/libSceAgc.so" "$prefix/lib/libSceAgcDriver.so" \
            -lSceVideoOut -lkernel_web -lSceSystemService -lc++ -lc++abi -lunwind \
            -o "$out/gles3_probe.elf" > "$out/link.log" 2>&1 ||
            { grep -m20 'undefined symbol' "$out/link.log" || tail -20 "$out/link.log"; die "probe link failed"; }
        echo "    linked $(du -h "$out/gles3_probe.elf" | cut -f1) $out/gles3_probe.elf"
    fi
}

case "$variant" in
    stock|all) build_variant stock "$ROOT/toolchain/ps5-opengl-stock" ;;
esac
case "$variant" in
    gles|all)
        patches=()
        for p in "$HERE"/patches/*.patch; do patches+=("$p"); done
        build_variant gles "$ROOT/toolchain/ps5-opengl-gles" "${patches[@]}" ;;
esac
log "done"
