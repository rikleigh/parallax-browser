#!/usr/bin/env bash
# The exFAT image writer (apps/common/ps5exfat.c) on the Mac:
#   port/webkit/test/downloads/exfat-test.sh [--no-big]
# Builds images of made-up game folders (nested and empty folders, empty
# files, a few thousand files in one folder, Unicode names, sizes on and
# around cluster boundaries, a sparse file past 4 GiB, ~1.5 GB of random
# data for the throughput), checks each with fsck_exfat and check.py (the
# layout ShadowMountPlus wants), mounts it read-only with hdiutil and
# compares it with its source (exfat/compare.py: diff -r with names
# NFC-normalized, since macOS's exFAT driver lists them decomposed); then
# checks what must be refused.
# --no-big skips the 4 GiB file and the throughput build (up to about
# 4.5 GB of disk while they run, removed after each).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
big=1
[[ ${1:-} == --no-big ]] && big=0
work="$(mktemp -d)"
devices=()
cleanup() {
    for d in "${devices[@]+"${devices[@]}"}"; do hdiutil detach -force "$d" >/dev/null 2>&1 || true; done
    rm -rf "$work"
}
trap cleanup EXIT
tool="$work/exfat-tool"
cc -std=c11 -O2 -Wall -Wextra -Werror -o "$tool" "$here/exfat/exfat-tool.c" "$root/port/webkit/apps/common/ps5exfat.c"
fails=0
passes=0
pass() { echo "PASS $*"; passes=$((passes + 1)); }
failed() { echo "FAIL $*"; fails=$((fails + 1)); }

attach() { # image [mountpoint]: prints the /dev/diskN
    local out
    if [ -n "${2:-}" ]; then
        out="$(hdiutil attach -readonly -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$2" "$1")"
    else
        out="$(hdiutil attach -readonly -nomount -imagekey diskimage-class=CRawDiskImage "$1")"
    fi
    local dev
    dev="$(printf '%s\n' "$out" | awk 'NR==1 {print $1}')"
    devices+=("$dev")
    echo "$dev"
}
detach() { hdiutil detach "$1" >/dev/null 2>&1 || hdiutil detach -force "$1" >/dev/null; }

# build name src image cluster [label]: builds, then fsck, check.py, mount and compare.
image_check() {
    local name="$1" src="$2" image="$3" cluster="$4" label="${5:-}" got size measured
    measured="$("$tool" measure "$src" "$([ "$cluster" = 65536 ] && echo 0 || echo "$cluster")")"
    got="$("$tool" build "$src" "$image" "$([ "$cluster" = 65536 ] && echo 0 || echo "$cluster")" "$label" || true)"
    if [[ $got != OK* ]]; then failed "$name: build: $got"; return; fi
    size=$(stat -f %z "$image")
    pass "$name: built ($got)"
    [ "$measured" = "$size" ] && pass "$name: measure $measured = the image's size" || failed "$name: measure $measured, image $size"
    local last
    last="$(sed -E 's/.*last=([0-9]+)\/([0-9]+).*/\1 \2/' <<<"$got")"
    [ "${last% *}" = "${last#* }" ] && pass "$name: progress ends at done = total" || failed "$name: progress last $last"
    local calls seconds
    calls="$(sed -E 's/.*calls=([0-9]+).*/\1/' <<<"$got")"
    seconds="$(awk '{print $3}' <<<"$got")"
    if awk -v c="$calls" -v s="$seconds" 'BEGIN {exit !(c <= s * 10 + 3)}'; then
        pass "$name: $calls progress calls in ${seconds}s (at most ~10/s)"
    else
        failed "$name: $calls progress calls in ${seconds}s"
    fi

    local layout
    layout="$(python3 -I "$here/exfat/check.py" "$image" "$cluster" "$label" || true)"
    [[ $layout == OK* ]] && pass "$name: layout $layout" || failed "$name: layout $layout"

    local dev fsck
    dev="$(attach "$image")"
    fsck="$(fsck_exfat -n "$dev" 2>&1)" && rc=0 || rc=$?
    detach "$dev"
    if [ $rc = 0 ] && grep -q "appears to be OK" <<<"$fsck"; then
        pass "$name: fsck_exfat -n: $(grep 'appears to be OK' <<<"$fsck" | sed 's/^\*\* //')"
    else
        failed "$name: fsck_exfat -n (exit $rc):"; printf '%s\n' "$fsck" | sed 's/^/    /'
    fi

    mkdir -p "$work/mnt"
    dev="$(attach "$image" "$work/mnt")"
    if python3 -I "$here/exfat/compare.py" "$src" "$work/mnt" >"$work/diff.txt" 2>&1; then
        pass "$name: mounted, compared with the source: $(cat "$work/diff.txt")"
    else
        failed "$name: mounted, compared with the source:"; head -20 "$work/diff.txt" | sed 's/^/    /'
    fi
    # Modification times survive (exFAT keeps 10 ms).
    local f a b timeBad=0
    while IFS= read -r f; do
        a=$(stat -f %m "$src/$f"); b=$(stat -f %m "$work/mnt/$f")
        [ "$a" = "$b" ] || { timeBad=1; echo "    $f: $a vs $b"; }
    done < <(cd "$src" && find . -mindepth 1 -type f | head -50)
    [ $timeBad = 0 ] && pass "$name: modification times kept" || failed "$name: modification times differ"
    if [ -n "$label" ]; then
        local shown
        shown="$(diskutil info "$dev" | awk -F': *' '/Volume Name/ {print $2}')"
        [ "$shown" = "$label" ] && pass "$name: volume label '$shown'" || failed "$name: volume label '$shown', wanted '$label'"
    fi
    detach "$dev"
}

# The mixed tree.
src="$work/PPSA12345-app"
mkdir -p "$src/sce_sys" "$src/empty" "$src/deep/a/b/c/d/e/f" "$src/deep/a/b/c2" "$src/exact" "$src/many"
head -c 100000 /dev/urandom >"$src/eboot.bin"
printf '{"titleId":"PPSA12345"}' >"$src/sce_sys/param.json"
head -c 65536 /dev/urandom >"$src/sce_sys/icon0.png"
: >"$src/zero"
: >"$src/deep/a/b/c2/empty.txt"
head -c 3000 /dev/urandom >"$src/deep/a/b/c/file.bin"
for n in 1 4095 4096 4097 65535 65536 65537 131072 1048576 1048577; do head -c $n /dev/urandom >"$src/exact/$n"; done
for i in $(seq -w 0 2999); do echo "$i" >"$src/many/file-$i.txt"; done
mkdir -p "$src/Ünïcødé" "$src/日本語フォルダ" "$src/中文"
echo café >"$src/café.txt"
echo naïve >"$src/Ünïcødé/naïve résumé.bin"
echo jp >"$src/日本語フォルダ/ファイル.dat"
echo game >"$src/emoji 🎮🚀.txt"
echo zh >"$src/中文/文件名.txt"
echo long >"$src/$(printf 'x%.0s' $(seq 1 255))"
echo dots >"$src/name with spaces. and.dots"
touch -t 200102030405.06 "$src/eboot.bin"
touch -t 203012312359.58 "$src/zero"

image_check "mixed tree, 64 KiB clusters" "$src" "$work/mixed.exfat" 65536
image_check "mixed tree, 4 KiB clusters (multi-cluster folders)" "$src" "$work/mixed4k.exfat" 4096
image_check "mixed tree, 256 KiB clusters, label" "$src" "$work/mixed256k.exfat" 262144 PPSA12345
rm -f "$work"/mixed*.exfat

# An empty folder.
mkdir -p "$work/empty-src"
image_check "an empty folder" "$work/empty-src" "$work/empty.exfat" 65536
rm -f "$work/empty.exfat"

sparse() { python3 -I -c 'import sys; open(sys.argv[1], "ab").truncate(int(sys.argv[2]))' "$1" "$2"; }
if [ $big = 1 ]; then
    free=$(df -k "$work" | awk 'NR==2 {print $4}')
    if [ "$free" -lt $((10 * 1024 * 1024)) ]; then
        echo "SKIP the big images: less than 10 GB free"
    else
        # A sparse file past 4 GiB with data at its start, around the 4 GiB
        # boundary and at its end. (Cluster-multiple sizes are in the mixed
        # tree.)
        bsrc="$work/big-src"
        mkdir -p "$bsrc/sce_sys"
        echo '{}' >"$bsrc/sce_sys/param.json"
        echo eboot >"$bsrc/eboot.bin"
        sparse "$bsrc/big.bin" $((4 * 1024 * 1024 * 1024 + 7 * 1024 + 3))
        for off in 0 $((4096 * 1024 - 1)) $((4096 * 1024 + 6)); do # KiB offsets: start, 1 KiB before 4 GiB, near the end
            head -c 1024 /dev/urandom | dd of="$bsrc/big.bin" bs=1024 seek=$off conv=notrunc 2>/dev/null
        done
        image_check "a sparse file of 4 GiB + 7 KiB + 3 bytes" "$bsrc" "$work/big.exfat" 65536
        rm -rf "$bsrc" "$work/big.exfat"

        # Throughput: ~1.5 GB of random data in a few files.
        tsrc="$work/speed-src"
        mkdir -p "$tsrc/sce_sys" "$tsrc/data"
        echo '{}' >"$tsrc/sce_sys/param.json"
        for i in 1 2 3; do head -c $((500 * 1024 * 1024)) /dev/urandom >"$tsrc/data/chunk$i.bin"; done
        for i in $(seq 1 200); do head -c $((i * 1000)) /dev/urandom >"$tsrc/data/small$i.bin"; done
        image_check "1.5 GB of random data" "$tsrc" "$work/speed.exfat" 65536
        rm -rf "$tsrc" "$work/speed.exfat"
    fi
fi

# What must be refused, and what fails cleanly.
refuse() { # expect name src image [cluster] [cancel-after]
    local got
    got="$("$tool" build "$3" "$4" "${5:-0}" "" "${6:-}" || true)"
    if [[ $got == "$1"* ]]; then pass "$2: $got"; else failed "$2: $got (expected $1)"; fi
    if [ "$1" != OK ] && [ -e "$4.part" ]; then failed "$2: $4.part was left behind"; fi
}
r="$work/refuse"
mkdir -p "$r/link/sub" && echo x >"$r/link/sub/a" && ln -s a "$r/link/sub/b"
refuse REFUSED "a symbolic link in the folder" "$r/link" "$r/out1.exfat"
mkdir -p "$r/fifo" && mkfifo "$r/fifo/pipe"
refuse REFUSED "a fifo in the folder" "$r/fifo" "$r/out2.exfat"
mkdir -p "$r/qmark" && echo x >"$r/qmark/what?.txt"
refuse REFUSED "a name with ? (exFAT can't hold it)" "$r/qmark" "$r/out3.exfat"
mkdir -p "$r/colon" && echo x >"$r/colon/a:b"
refuse REFUSED "a name with : (exFAT can't hold it)" "$r/colon" "$r/out4.exfat"
mkdir -p "$r/dot" && echo x >"$r/dot/name."
refuse REFUSED "a name ending in a dot (exFAT drivers can't open it)" "$r/dot" "$r/out12.exfat"
mkdir -p "$r/space" && echo x >"$r/space/name "
refuse REFUSED "a name ending in a space (exFAT drivers can't open it)" "$r/space" "$r/out13.exfat"
mkdir -p "$r/ok" && echo x >"$r/ok/a"
echo existing >"$r/exists.exfat"
refuse REFUSED "an image that already exists" "$r/ok" "$r/exists.exfat"
[ "$(cat "$r/exists.exfat")" = existing ] && pass "the existing image is untouched" || failed "the existing image was changed"
refuse REFUSED "a source that isn't a folder" "$r/ok/a" "$r/out5.exfat"
refuse REFUSED "a source that doesn't exist" "$r/nothing" "$r/out6.exfat"
refuse REFUSED "a cluster size that isn't a power of two" "$r/ok" "$r/out7.exfat" 3000
refuse REFUSED "a cluster size under 4 KiB" "$r/ok" "$r/out8.exfat" 2048
# Names that differ only in case need a case-sensitive file system.
hdiutil create -quiet -size 20m -fs "Case-sensitive APFS" -volname exfatcase "$work/case.dmg"
mkdir -p "$work/case"
casedev="$(hdiutil attach -nobrowse -mountpoint "$work/case" "$work/case.dmg" | awk 'NR==1 {print $1}')"
devices+=("$casedev")
mkdir -p "$work/case/src" && echo 1 >"$work/case/src/Name.txt" && echo 2 >"$work/case/src/NAME.TXT"
mkdir -p "$work/case/src2/É" && echo 1 >"$work/case/src2/é"
refuse REFUSED "names that differ only in case" "$work/case/src" "$r/out9.exfat"
refuse REFUSED "names that differ only in case, up-cased by the table (é, É)" "$work/case/src2" "$r/out14.exfat"
detach "$casedev"
mkdir -p "$r/cancel" && head -c $((64 * 1024 * 1024)) /dev/urandom >"$r/cancel/data.bin"
refuse CANCELLED "cancelled at the first progress call" "$r/cancel" "$r/out10.exfat" 0 1
[ ! -e "$r/out10.exfat" ] && pass "a cancelled build leaves no image" || failed "a cancelled build left an image"
mkdir -p "$r/unreadable" && echo x >"$r/unreadable/secret" && chmod 000 "$r/unreadable/secret"
refuse FAILED "a file that can't be read (not the folder's fault)" "$r/unreadable" "$r/out11.exfat"
chmod 644 "$r/unreadable/secret"

echo "exfat-test: $passes passed, $fails failed"
[ "$fails" = 0 ] || exit 1
