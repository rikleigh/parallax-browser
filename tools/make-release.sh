#!/usr/bin/env bash
# Build the release archive: the title folder PPSA99207/ at the top (what the
# homebrew catalog, ProsperoStore and ShadowMountPlus expect: it goes into
# /data/homebrew), built and packaged with no log host (a development build
# sends its log to this Mac). The runtime files the title reads from
# /data/ps5webkit (fonts, CA bundle, ICU and XKB data, the start page without
# its test links) are inside it, in runtime/: the title installs them when it
# starts (apps/title/runtime.c).
#   tools/make-release.sh [version]     -> dist/parallax-browser-<version>.zip
# The version defaults to `git describe --tags --always --dirty`. The title's
# debug binaries (for crash addresses) stay in dist/<name>-debug/. The build
# directory's log host is put back afterwards, so development builds keep
# logging.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/env.sh" >/dev/null
# The WebKit build on ANGLE's Vulkan backend over RADV, with WebGL, WebGPU and
# every patch (GPU=radv port/webkit/build.sh). Releases up to 0.2.3 were the
# ps5-opengl build (build-webgpu).
BLD="${BLD:-$ROOT/toolchain/wpe-webkit/build-radv}"
version="${1:-$(git -C "$ROOT" describe --tags --always --dirty)}"

# The zip names a commit (INSTALL.txt): what is built must be that commit.
# Untracked files are fine (notes); a changed tracked file is not.
if [[ -z ${ALLOW_DIRTY:-} ]] && [[ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ]]; then
    echo "the working tree has uncommitted changes (ALLOW_DIRTY=1 to build anyway, not for a release):" >&2
    git -C "$ROOT" status --short --untracked-files=no >&2
    exit 1
fi
# A release version (A.B.C, the tag vA.B.C) must match param.json's
# contentVersion, 0(A+1).BBB.CCC: the browser's own updates (UPDATES.md)
# check the unpacked release against its tag, and ProsperoStore compares
# contentVersion.
if [[ $version =~ ^v?([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
    version="${version#v}"
    want="$(printf '%02d.%03d.%03d' $((BASH_REMATCH[1] + 1)) "${BASH_REMATCH[2]}" "${BASH_REMATCH[3]}")"
    have="$(sed -n 's/.*"contentVersion": *"\([^"]*\)".*/\1/p' "$ROOT/port/webkit/apps/title/sce_sys/param.json")"
    [[ $have == "$want" ]] || { echo "version $version needs contentVersion $want in sce_sys/param.json (it has $have)" >&2; exit 1; }
fi
name="parallax-browser-$version"
out="$ROOT/dist/$name"
stage="$ROOT/dist/$name-debug"

[[ -f $BLD/build.ninja ]] || { echo "missing $BLD: run GPU=radv port/webkit/build.sh" >&2; exit 1; }
grep -q '^ENABLE_WEBGPU:BOOL=ON' "$BLD/CMakeCache.txt" || { echo "$BLD is not a WebGPU build" >&2; exit 1; }
grep -q '^PS5WK_GPU_RADV:[A-Z]*=ON' "$BLD/CMakeCache.txt" || { echo "$BLD is not a GPU=radv build" >&2; exit 1; }
# package.sh links every object in title-work/obj: start from an empty one, so
# nothing left from another build or an experiment gets in.
rm -rf "$ROOT/toolchain/wpe-webkit/title-work/obj"

# 1. The title with no log host.
devhost="$(sed -n 's/^PS5WK_LOG_HOST:STRING=//p' "$BLD/CMakeCache.txt")"
restore() {
    echo "==> putting the build's log host back ($devhost)"
    cmake -DPS5WK_LOG_HOST="$devhost" "$BLD" >/dev/null && ninja -C "$BLD" ps5wk-child ps5browser >/dev/null
}
trap restore EXIT
echo "==> building with no log host"
cmake -DPS5WK_LOG_HOST= "$BLD" >/dev/null
ninja -C "$BLD" ps5wk-child ps5browser
rm -rf "$stage"; mkdir -p "$stage"
# The start page without its test links (they play local test media).
sed '/<!-- tests:/,/<!-- \/tests -->/d' "$ROOT/port/webkit/runtime/start.html" > "$stage/start.html"
! grep -q 'media/' "$stage/start.html" || { echo "start.html still links test media" >&2; exit 1; }
PS5WK_LOG_HOST= BLD="$BLD" OUT="$stage" START_HTML="$stage/start.html" "$ROOT/port/webkit/apps/title/package.sh"
TITLE="$stage/PPSA99207"
# No address of this Mac or the console in the binaries.
for host in "$devhost" "${PS5WK_LOG_HOST:-}" "${PS5_HOST:-}"; do
    [[ $host =~ ^[0-9]+(\.[0-9]+){3}$ ]] || continue
    if grep -rlaF "$host" "$TITLE" >/dev/null; then
        echo "$host is still in the title: $(grep -rlaF "$host" "$TITLE" | tr '\n' ' ')" >&2; exit 1
    fi
done

# No update test hook (UPDATES.md, "Testing"): a development build's only.
for hook in update-test-yes update-source update-store-root PS5WK_UPDATE_TESTING; do
    if grep -rlaF "$hook" "$TITLE" --include='*.bin' --include='*.elf' >/dev/null; then
        echo "the update test hook $hook is in the title: $(grep -rlaF "$hook" "$TITLE" --include='*.bin' --include='*.elf' | tr '\n' ' ')" >&2; exit 1
    fi
done

# No debug tools or diagnostic logs: a development build's only
# (PS5WK_DEBUG_BUILD, defined with a log host; radv's debug patches only with
# RADV_DEBUG_PATCHES=1, port/deps/radv/build-radv.sh). The heap profiler, the
# debug user script and dialogs, the test-URL runs and screenshots, the
# title's crash test (crash-title-test, /restart?scope=crash's), the disk
# write test (POST /debug/write-test and the child's "writetest" task), the
# radv submit log.
# WebKit's (patches-radv 0159: PS5WKDebugBuild.h from the log host): the
# querySelector and Atomics stats, the timers and storage-quota logs, the
# stream-stall stamps, the WebGPU callback latency log; 0160's
# opfs-backend-write-min; 0162's file blob and 0163's writable stream logs;
# 0167's tile allocation failure test (PS5WK_WC_TEST_ALLOC_FAIL); the title's
# debug-user-agent.
for marker in heap-profile '[heapprof]' debug-userscript debug-dialogs '[ps5wk-dialog]' test-urls.txt test-onload \
    debug-screenshot crash-title-test 'crash test:' /debug/write-test '[writetest]' ps5wk-write-test \
    radv-submit-log 'radv/ps5: submit' \
    PS5WK_QSA_STATS '[qsa]' PS5WK_ATOMICS_LOG '[atomics]' '[timers]' '[storage] quota for' \
    '[storage] total quota for all sites' 'no stream threads yet' webgpu-latency-log '[webgpu] title: latency' \
    opfs-backend-write-min '[storage] file blob' '[storage] writable stream closed' PS5WK_WC_TEST_ALLOC_FAIL \
    debug-user-agent; do
    # Options before -e and the path: BSD grep takes anything after a "--"
    # as a file name.
    if grep -rlaF --include='*.bin' --include='*.elf' -e "$marker" "$TITLE" >/dev/null; then
        echo "the debug-only \"$marker\" is in the title: $(grep -rlaF --include='*.bin' --include='*.elf' -e "$marker" "$TITLE" | tr '\n' ' ')" >&2
        [[ $marker == radv* ]] && echo "(radv was built with RADV_DEBUG_PATCHES=1: run port/deps/radv/build-radv.sh without it)" >&2
        exit 1
    fi
done

[[ -f $TITLE/runtime/runtime-version ]] || { echo "the title has no runtime/ (NO_RUNTIME set?)" >&2; exit 1; }
cmp -s "$stage/start.html" "$TITLE/runtime/start.html" || { echo "the title's start page isn't the release one" >&2; exit 1; }

# 2. The archive: PPSA99207/ at the top, LICENSE, the notices and INSTALL.txt in it.
rm -rf "$out" "$out.zip"
app="$out/PPSA99207"
mkdir -p "$app"
rsync -a --exclude .DS_Store "$TITLE/" "$app/"
cp "$ROOT/LICENSE" "$ROOT/THIRD-PARTY-NOTICES.md" "$app/"

# Nothing from Sony and no media: only our ELFs and the clean-room libc.prx.
bad="$(cd "$out" && find . -type f \( -name '*.sprx' -o -name '*.self' -o -name '*.mp4' -o -name '*.ts' \
    -o -name '*.webm' -o -name '*.pkg' -o \( -name '*.prx' ! -path './PPSA99207/sce_module/libc.prx' \) \))"
[[ -z $bad ]] || { echo "unexpected files in the release:" >&2; echo "$bad" >&2; exit 1; }
cmp -s "$app/sce_module/libc.prx" "$ROOT/ref/PS5VR/app/tools/native-app/runtime/libc.prx" ||
    { echo "sce_module/libc.prx is not PS5VR's clean-room libc" >&2; exit 1; }

commit="$(git -C "$ROOT" rev-parse --short HEAD)"
cat > "$app/INSTALL.txt" <<EOF
Parallax Browser $version (source: commit $commit)
https://github.com/rikleigh/parallax-browser

A homebrew web browser for a jailbroken PS5, with View in VR for the PS VR2.
Experimental: tested on firmware 13.60 only.

Requirements
  - A jailbroken PS5 (developed on 13.60) running kstuff and etaHEN, with an
    FTP server. The jailbreak is tethered: run it again after every reboot.
  - Something that registers homebrew folders in /data/homebrew, such as
    etaHEN or ShadowMount+.
  - For View in VR: a PS VR2, paired and set up in the system settings.

Install
  From ProsperoStore: install Parallax Browser there.
  By hand:
  1. Copy the PPSA99207 folder in this archive to /data/homebrew on the
     console over FTP, so that it is /data/homebrew/PPSA99207.
  2. Register /data/homebrew/PPSA99207 with your homebrew tool. Parallax
     Browser then appears on the home screen.
  The first start copies the browser's runtime files (fonts, certificates,
  Unicode data, the start page; about $(du -sh "$app/runtime" | cut -f1 | tr -d ' ')) into /data/ps5webkit.

Update
  Replace /data/homebrew/PPSA99207 with the new folder (or update it from
  the store). The next start brings the runtime files up to date. Each PS5
  user's bookmarks, history, settings and downloads are kept: they live in
  /data/ps5webkit/users, which updates never touch.

Uninstall
  Remove the title with your homebrew tool, then delete /data/homebrew/PPSA99207
  and, to remove your browsing data too, /data/ps5webkit.

Not affiliated with or endorsed by Sony Interactive Entertainment. Licensed
under the GPL-3.0 (LICENSE); third-party components are listed in
THIRD-PARTY-NOTICES.md. The source, with the build steps, is at the address
above.
EOF

(cd "$out" && COPYFILE_DISABLE=1 zip -qry -X "../$name.zip" PPSA99207)
rm -f "$stage/start.html"
rm -rf "$out"
ls -la "$ROOT/dist/$name.zip"
shasum -a 256 "$ROOT/dist/$name.zip"
