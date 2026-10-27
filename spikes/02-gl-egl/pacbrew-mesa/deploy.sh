#!/usr/bin/env bash
# Deploy pacbrew's Mesa (RADV + zink + EGL videoout) to a jailbroken PS5 and
# run gl_egl_mesa.elf as an elfldr payload. NOT run automatically: needs a
# console with elfldr (port 9021) and ftpsrv (port 2121) running.
#
#   PS5_HOST=192.168.1.50 spikes/02-gl-egl/pacbrew-mesa/deploy.sh          # upload + run
#   PS5_HOST=... deploy.sh --upload-only | --send-only | --dry-run
#
# Where the libraries go: the payload's DT_NEEDED entries are plain sonames
# (libEGL.so.1, libGL.so.1), and so are Mesa's own (libgallium-26.2.0.so ->
# libvulkan.so.1). The SDK's runtime linker (ps5-payload-sdk crt/rtld.c,
# __rtld_find_file) looks a soname up in /system*/lib, $LD_LIBRARY_PATH,
# then /user/homebrew/lib, then the cwd. elfldr gives a payload no
# environment, so /user/homebrew/lib (pacbrew's PS5_HBROOT) is the place.
# Each file is uploaded under its soname (FTP has no symlinks).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
LIBDIR="$ROOT/toolchain/pacbrew-sysroot/target/user/homebrew/lib"
ELF="$HERE/gl_egl_mesa.elf"

PS5_HOST="${PS5_HOST:?set PS5_HOST to the console address}"
PS5_FTP_PORT="${PS5_FTP_PORT:-2121}"
PS5_PORT="${PS5_PORT:-9021}"
REMOTE_LIBDIR="${REMOTE_LIBDIR:-/user/homebrew/lib}"
FTP="ftp://$PS5_HOST:$PS5_FTP_PORT"

mode="${1:-all}"
run() { if [[ "$mode" == "--dry-run" ]]; then printf '+'; printf ' %q' "$@"; echo; else "$@"; fi; }

# local file -> soname on the console. libGLESv2 is not needed by
# gl_egl_mesa.elf (it links libGL) but is what a GLES/WebKit build links.
LIBS=(
    "libEGL.so.1.0.0:libEGL.so.1"
    "libGL.so.1.2.0:libGL.so.1"
    "libGLESv2.so.2.0.0:libGLESv2.so.2"
    "libgallium-26.2.0.so:libgallium-26.2.0.so"
    "libvulkan.so.1.4.354:libvulkan.so.1"
)

upload() {
    local total=0 entry src dst
    for entry in "${LIBS[@]}"; do
        src="$LIBDIR/${entry%%:*}" dst="${entry##*:}"
        [[ -f "$src" ]] || { echo "missing $src: run build.sh first" >&2; exit 1; }
        total=$((total + $(stat -f %z "$src" 2>/dev/null || stat -c %s "$src")))
        echo "==> $dst ($(du -h "$src" | cut -f1))"
        run curl -sS --ftp-create-dirs -T "$src" "$FTP$REMOTE_LIBDIR/$dst"
    done
    echo "==> uploaded $((total / 1024 / 1024)) MiB to $REMOTE_LIBDIR"
    # RADV's shader cache for a payload (patch 0028); Mesa creates it too,
    # this only makes a permission problem visible early. '*' = ignore "exists".
    run curl -sS -o /dev/null -Q "*MKD /data/radv-shader-cache" "$FTP/"
}

send() {
    [[ -f "$ELF" ]] || { echo "missing $ELF: run build.sh payload" >&2; exit 1; }
    # tools/send.sh streams the payload's stdout/stderr (Mesa logs, RESULT
    # lines) back and saves them under logs/.
    run env PS5_HOST="$PS5_HOST" PS5_PORT="$PS5_PORT" "$ROOT/tools/send.sh" "$ELF"
}

case "$mode" in
    all|--dry-run) upload; send ;;
    --upload-only) upload ;;
    --send-only) send ;;
    *) echo "usage: $0 [--upload-only|--send-only|--dry-run]" >&2; exit 1 ;;
esac
