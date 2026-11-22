#!/usr/bin/env bash
# The UFS2 image writer and reader (apps/common/ps5ufs2.c) and what an
# image or package holds (apps/common/ps5image.c) on the Mac:
#   port/webkit/test/downloads/image-test.sh [--no-big]
# - .ffpkg images of made-up apps (nested and empty folders, empty files,
#   Unicode names, a folder of 3000 files, files at the direct, single and
#   double indirect block edges, and with --big off a file past 4 GiB),
#   each checked by image/check_ufs2.py (an fsck-style walk written from
#   FreeBSD's headers, not from the writer), read back by ps5ufs2_verify,
#   and its files read with ps5ufs2_read_file;
# - with DUMP_FORGE=<a ps5-dump-forge checkout> and a working cargo, the
#   same folders built by dump-forge's own UFS2 crate (the writer this one
#   is ported from) and compared byte for byte;
# - what must be refused (links, fifos, an existing image, the image inside
#   its folder) and a cancelled build (no .part left);
# - ps5image_info and ps5image_icon on .ffpkg and .exfat images (also one
#   with its FAT zeroed, as other builders write them, and one with its
#   app in a folder), and on synthetic PS4 (CNT) and PS5 (FIH) packages
#   from image/make_pkg.py: retail, encrypted entries, entries found by
#   name, a byte order mark, truncated and hostile ones.
# --no-big skips the ~540 MB double-indirect file and the 4.5 GB one.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
big=1
[[ ${1:-} == --no-big ]] && big=0
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
apps="$root/port/webkit/apps/common"
tool="$work/image-tool"
cc -std=c11 -O2 -Wall -Wextra -Werror -o "$tool" "$here/image/image-tool.c" "$apps/ps5image.c" "$apps/ps5ufs2.c" "$apps/ps5exfat.c" \
    "$apps/ps5install.c" -lpthread
cc -std=c11 -O2 -Wall -Wextra -Werror -o "$work/exfat-tool" "$here/exfat/exfat-tool.c" "$apps/ps5exfat.c"
# the same with AddressSanitizer, for the hostile images
cc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o "$work/image-tool-asan" "$here/image/image-tool.c" \
    "$apps/ps5image.c" "$apps/ps5ufs2.c" "$apps/ps5exfat.c" "$apps/ps5install.c" -lpthread
fails=0
passes=0
pass() { echo "PASS $*"; passes=$((passes + 1)); }
failed() { echo "FAIL $*"; fails=$((fails + 1)); }
# within seconds cmd...: its output, or "TIMEOUT" if it ran longer (killed)
within() {
    local limit=$1 pid i
    shift
    "$@" >"$work/within.out" 2>&1 &
    pid=$!
    for ((i = 0; i < limit * 10; i++)); do
        kill -0 $pid 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 $pid 2>/dev/null; then
        kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null || true
        echo TIMEOUT
    else
        wait $pid 2>/dev/null || true
        cat "$work/within.out"
    fi
}
jget() { python3 -I -c 'import json,sys; v=json.loads(sys.argv[1]); print(json.dumps(v.get(sys.argv[2])) if not isinstance(v.get(sys.argv[2]),str) else v.get(sys.argv[2]))' "$1" "$2"; }

# A title()-style app: param.json, eboot.bin, an icon (a real PNG).
app() { # dir id name
    mkdir -p "$1/sce_sys"
    printf '{"titleId":"%s","contentId":"UP9000-%s_00-TESTAPP000000000","contentVersion":"01.002.000","applicationCategoryType":0,"localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"%s"}}}' \
        "$2" "$2" "$3" >"$1/sce_sys/param.json"
    head -c 70000 /dev/urandom >"$1/eboot.bin"
    python3 -I - "$1/sce_sys/icon0.png" <<'PY'
import struct, sys, zlib
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
w = h = 64
raw = b''.join(b'\0' + bytes([x * 4, y * 4, 128, 255]) * 1 for y in range(h) for x in [0])  # placeholder
raw = b''.join(b'\0' + b''.join(bytes([x * 4, y * 4, 128, 255]) for x in range(w)) for y in range(h))
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
PY
}

# ffpkg_check name src image: build, measure, verify, check_ufs2.py, read back.
ffpkg_check() {
    local name="$1" src="$2" image="$3" got size measured last
    measured="$("$tool" ffpkg-measure "$src")"
    got="$("$tool" ffpkg-build "$src" "$image" || true)"
    if [[ $got != OK* ]]; then failed "$name: build: $got"; return; fi
    size=$(stat -f %z "$image")
    pass "$name: built ($got, $((size >> 20)) MB)"
    [ "$measured" = "$size" ] && pass "$name: measure = the image's size" || failed "$name: measure $measured, image $size"
    [ $((size % 65536)) = 0 ] && pass "$name: a whole number of 64 KiB blocks" || failed "$name: size $size"
    last="$(sed -E 's/.*last=([0-9]+)\/([0-9]+).*/\1 \2/' <<<"$got")"
    [ "${last% *}" = "${last#* }" ] && pass "$name: progress ends at done = total" || failed "$name: progress $last"
    got="$("$tool" ffpkg-verify "$image" "$src" || true)"
    [[ $got == OK* ]] && pass "$name: ps5ufs2_verify: $got" || failed "$name: ps5ufs2_verify: $got"
    got="$(python3 -I "$here/image/check_ufs2.py" "$image" "$src" || true)"
    [[ $got == OK* ]] && pass "$name: check_ufs2.py: $got" || failed "$name: check_ufs2.py: $got"
    if [ -f "$src/sce_sys/param.json" ]; then
        "$tool" read "$image" sce_sys/param.json >"$work/read.json" && cmp -s "$work/read.json" "$src/sce_sys/param.json" \
            && pass "$name: ps5ufs2_read_file gives param.json back" || failed "$name: read param.json"
    fi
    if [ -n "${DUMP_FORGE:-}" ] && [ -x "$work/forge-ffpkg" ]; then
        "$work/forge-ffpkg" "$src" "$work/forge.ffpkg" >/dev/null
        cmp -s "$work/forge.ffpkg" "$image" && pass "$name: byte for byte what dump-forge's writer makes" \
            || failed "$name: differs from dump-forge's image ($(cmp "$work/forge.ffpkg" "$image" 2>&1 | head -1))"
        rm -f "$work/forge.ffpkg"
    fi
}

# dump-forge's writer, when there is a checkout and a cargo that runs
if [ -n "${DUMP_FORGE:-}" ]; then
    if command -v cargo >/dev/null && cargo --version >/dev/null 2>&1; then
        mkdir -p "$DUMP_FORGE/crates/ps5-dump-forge-ufs2/examples"
        cat >"$DUMP_FORGE/crates/ps5-dump-forge-ufs2/examples/ps5wk_ffpkg.rs" <<'RS'
use std::sync::atomic::AtomicBool;
use ps5upload_fpkg::source::FolderSource;
fn main() {
    let a: Vec<String> = std::env::args().collect();
    let mut src = FolderSource::open(std::path::Path::new(&a[1])).unwrap();
    let cancel = AtomicBool::new(false);
    let layout = ps5_dump_forge_ufs2::plan(&src, &Default::default(), &cancel).unwrap();
    let mut out = std::fs::File::create(&a[2]).unwrap();
    ps5_dump_forge_ufs2::write(&mut src, &layout, &mut out, &cancel, &mut |_, _| {}).unwrap();
}
RS
        if (cd "$DUMP_FORGE" && cargo build -q --release --example ps5wk_ffpkg -p ps5-dump-forge-ufs2) >"$work/cargo.log" 2>&1; then
            cp "$DUMP_FORGE/target/release/examples/ps5wk_ffpkg" "$work/forge-ffpkg"
            pass "dump-forge's UFS2 writer built for the comparison"
        else
            echo "SKIP dump-forge's writer didn't build: $(tail -1 "$work/cargo.log")"
        fi
    else
        echo "SKIP the comparison with dump-forge's writer: cargo doesn't run here"
    fi
else
    echo "SKIP the comparison with dump-forge's writer (DUMP_FORGE=<checkout> to run it)"
fi

# -- images of apps
src="$work/PPSA12345-app"
app "$src" PPSA12345 "Test App"
mkdir -p "$src/empty" "$src/deep/a/b/c/d/e" "$src/many" "$src/Ünïcødé" "$src/日本語"
: >"$src/zero"
echo café >"$src/café.txt"
echo jp >"$src/日本語/ファイル.dat"
echo long >"$src/$(printf 'x%.0s' $(seq 1 255))"
head -c 3000 /dev/urandom >"$src/deep/a/b/c/d/e/file.bin"
for n in 1 65535 65536 65537 786432 786433 1048577; do head -c $n /dev/urandom >"$src/size-$n"; done # 786432: the 12 direct blocks
for i in $(seq -w 0 2999); do echo "$i" >"$src/many/file-with-a-longer-name-$i.txt"; done
ffpkg_check "a mixed app (3000 files in one folder, block edges, Unicode)" "$src" "$work/mixed.ffpkg"
small="$work/small"
app "$small" PPSA00001 "Small"
ffpkg_check "a small app" "$small" "$work/small.ffpkg"
empty="$work/emptyfolder"
mkdir -p "$empty"
ffpkg_check "an empty folder" "$empty" "$work/empty.ffpkg"
if [ $big = 1 ]; then
    dbl="$work/double"
    app "$dbl" PPSA00002 "Double"
    head -c $((65536 * (12 + 8192) + 70001)) /dev/urandom >"$dbl/double-indirect.bin" # past the single indirect block
    ffpkg_check "a file in double indirect blocks (~540 MB)" "$dbl" "$work/double.ffpkg"
    rm -rf "$dbl" "$work/double.ffpkg"
    huge="$work/huge"
    app "$huge" PPSA00003 "Huge"
    mkfile -n 4500m "$huge/sparse-4.5g.bin"
    t0=$(date +%s)
    ffpkg_check "a 4.5 GB file" "$huge" "$work/huge.ffpkg"
    echo "    (4.5 GB in $(( $(date +%s) - t0 )) s)"
    rm -rf "$huge" "$work/huge.ffpkg"
fi

# -- verify finds what differs
verify_refuses() { # name image folder pattern
    local got
    got="$("$tool" ffpkg-verify "$2" "$3" || true)"
    if [[ $got == DIFF* ]] && grep -qi -- "$4" <<<"$got"; then pass "verify: $1: $got"; else failed "verify: $1: $got (wanted /$4/)"; fi
}
cp -R "$small" "$work/changed"
python3 -I -c 'import sys; f=open(sys.argv[1],"r+b"); f.seek(5000); b=f.read(1); f.seek(5000); f.write(bytes([b[0]^1]))' "$work/changed/eboot.bin"
verify_refuses "a byte changed" "$work/small.ffpkg" "$work/changed" "differs"
cp -R "$small" "$work/extra" && echo x >"$work/extra/new.txt"
verify_refuses "a file more in the folder" "$work/small.ffpkg" "$work/extra" "entries"
cp -R "$small" "$work/fewer" && rm "$work/fewer/eboot.bin"
verify_refuses "a file fewer" "$work/small.ffpkg" "$work/fewer" "not in the folder"
cp -R "$small" "$work/longer" && echo more >>"$work/longer/eboot.bin"
verify_refuses "a size changed" "$work/small.ffpkg" "$work/longer" "bytes in the image"
cp "$work/small.ffpkg" "$work/damaged.ffpkg"
python3 -I -c 'import sys; f=open(sys.argv[1],"r+b"); f.seek(65536+1372); f.write(b"\0\0\0\0")' "$work/damaged.ffpkg"
verify_refuses "the superblock's magic gone" "$work/damaged.ffpkg" "$small" "not a UFS2"
head -c 200000 /dev/urandom >"$work/random.ffpkg"
verify_refuses "random bytes" "$work/random.ffpkg" "$small" "not a UFS2"
got="$(python3 -I "$here/image/check_ufs2.py" "$work/damaged.ffpkg" "$small" || true)"
[[ $got == BAD* ]] && pass "check_ufs2.py refuses the damaged one: $got" || failed "check_ufs2.py on damage: $got"
rm -rf "$work/changed" "$work/extra" "$work/fewer" "$work/longer"
mkdir -p "$work/cancel" && head -c 300000000 /dev/urandom >"$work/cancel/big.bin"
got="$("$tool" ffpkg-build "$work/cancel" "$work/cancel.ffpkg" 2 || true)"
[[ $got == CANCELLED* ]] && [ ! -e "$work/cancel.ffpkg" ] && [ ! -e "$work/cancel.ffpkg.part" ] && pass "a cancelled build leaves nothing: $got" \
    || failed "cancel: $got"
"$tool" ffpkg-build "$work/cancel" "$work/cancel.ffpkg" >/dev/null
got="$("$tool" ffpkg-verify "$work/cancel.ffpkg" "$work/cancel" 2 || true)"
[[ $got == CANCELLED* ]] && pass "verify cancelled: $got" || failed "verify cancel: $got"
rm -rf "$work/cancel" "$work/cancel.ffpkg"

# -- refused
refuse() { # name src image pattern
    local got
    got="$("$tool" ffpkg-build "$2" "$3" || true)"
    if [[ $got == REFUSED* ]] && grep -qi -- "$4" <<<"$got"; then pass "refused: $1: $got"; else failed "refused: $1: $got"; fi
}
mkdir -p "$work/link" && ln -s /etc/hosts "$work/link/hosts"
refuse "a symbolic link" "$work/link" "$work/o1.ffpkg" "link"
mkdir -p "$work/fifo" && mkfifo "$work/fifo/pipe"
refuse "a fifo" "$work/fifo" "$work/o2.ffpkg" "special"
refuse "an image that is there already" "$small" "$work/small.ffpkg" "already exists"
refuse "the image inside its folder" "$small" "$small/self.ffpkg" "inside"
refuse "not a folder" "$work/small.ffpkg" "$work/o3.ffpkg" "isn't a folder"

# -- what images hold
info="$("$tool" info "$work/small.ffpkg" || true)"
[ "$(jget "$info" kind)" = ffpkg ] && [ "$(jget "$info" id)" = PPSA00001 ] && [ "$(jget "$info" name)" = Small ] && [ "$(jget "$info" version)" = 01.002.000 ] \
    && [ "$(jget "$info" appAtRoot)" = true ] && [ "$(jget "$info" icon)" = true ] && [ "$(jget "$info" contentId)" = UP9000-PPSA00001_00-TESTAPP000000000 ] \
    && pass "info on a .ffpkg: $info" || failed "info .ffpkg: $info"
got="$("$tool" icon "$work/small.ffpkg" "$work/icon.png")" && cmp -s "$work/icon.png" "$small/sce_sys/icon0.png" \
    && pass "the icon out of a .ffpkg: $got" || failed "icon .ffpkg: $got"
got="$("$work/exfat-tool" build "$small" "$work/small.exfat" 0 PPSA00001)"
info="$("$tool" info "$work/small.exfat" || true)"
[ "$(jget "$info" kind)" = exfat ] && [ "$(jget "$info" id)" = PPSA00001 ] && [ "$(jget "$info" appAtRoot)" = true ] && [ "$(jget "$info" icon)" = true ] \
    && pass "info on an .exfat (ps5exfat's): $info" || failed "info .exfat: $info ($got)"
got="$("$tool" icon "$work/small.exfat" "$work/icon2.png")" && cmp -s "$work/icon2.png" "$small/sce_sys/icon0.png" \
    && pass "the icon out of an .exfat: $got" || failed "icon .exfat: $got"
"$tool" read "$work/small.exfat" SCE_SYS/PARAM.JSON >"$work/upper.json" && cmp -s "$work/upper.json" "$small/sce_sys/param.json" \
    && pass "exFAT names match in either case" || failed "exFAT case"
# a zeroed FAT (contiguous files, as some builders write them): still read
cp "$work/small.exfat" "$work/zerofat.exfat"
python3 -I - "$work/zerofat.exfat" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b'); b = f.read(512)
sector = 1 << b[108]
fat, length = struct.unpack_from('<II', b, 80)
f.seek(fat * sector + 8); f.write(b'\0' * (length * sector - 8))
PY
got="$("$tool" read "$work/zerofat.exfat" sce_sys/param.json || true)"
[ "$got" = "$(cat "$small/sce_sys/param.json")" ] && pass "an .exfat with its FAT zeroed is read as contiguous" || failed "zeroed FAT: $got"
# the app in a folder inside the image: not at its root
mkdir -p "$work/nested" && cp -R "$small" "$work/nested/PPSA00001"
"$work/exfat-tool" build "$work/nested" "$work/nested.exfat" >/dev/null
info="$("$tool" info "$work/nested.exfat" || true)"
[ "$(jget "$info" appAtRoot)" = false ] && pass "an image with its app in a folder: not at its root" || failed "nested: $info"
"$tool" ffpkg-build "$work/nested" "$work/nested.ffpkg" >/dev/null
info="$("$tool" info "$work/nested.ffpkg" || true)"
[ "$(jget "$info" appAtRoot)" = false ] && [ "$(jget "$info" id)" = "" ] && pass "... and the same for a .ffpkg" || failed "nested ffpkg: $info"
got="$("$tool" info "$work/random.ffpkg" || true)"
[[ $got == ERR* ]] && pass "random bytes aren't anything: $got" || failed "random: $got"

# -- hostile images (a download can be anything): answered quickly, no
# out-of-bounds access (AddressSanitizer)
hostile() { # name image: info and an icon, each within 10 s, clean under ASan
    local a b
    a="$(within 10 "$work/image-tool-asan" info "$2")"
    b="$(within 10 "$work/image-tool-asan" icon "$2" "$work/h.png")"
    if [[ ( $a == ERR* || $a == '{'* ) && ( $b == ERR* || $b == OK* ) ]]; then
        pass "hostile image, $1: ${a:0:90}"
    else
        failed "hostile image, $1: ${a:0:300} / ${b:0:300}"
    fi
}
# an exFAT root whose chain loops on itself, with no end-of-folder entry
python3 -I - "$work/small.exfat" "$work/loop.exfat" <<'PY'
import shutil, struct, sys
shutil.copyfile(sys.argv[1], sys.argv[2])
f = open(sys.argv[2], 'r+b'); b = f.read(512)
sector = 1 << b[108]; cluster = sector << b[109]
fat, heap, root = struct.unpack_from('<I', b, 80)[0], struct.unpack_from('<I', b, 88)[0], struct.unpack_from('<I', b, 96)[0]
f.seek(fat * sector + root * 4); f.write(struct.pack('<I', root))
at = heap * sector + (root - 2) * cluster
f.seek(at); d = bytearray(f.read(cluster))
for i in range(0, cluster, 32):
    d[i] = (d[i] & 0x7f) or 0x05  # every entry unused, none the end
f.seek(at); f.write(d)
PY
hostile "an exFAT root that loops" "$work/loop.exfat"
# an exFAT sce_sys folder that claims 2^60 bytes and loops on itself
python3 -I - "$work/small.exfat" "$work/bigdir.exfat" <<'PY'
import shutil, struct, sys
shutil.copyfile(sys.argv[1], sys.argv[2])
f = open(sys.argv[2], 'r+b'); b = f.read(512)
sector = 1 << b[108]; cluster = sector << b[109]
fat, heap, root = struct.unpack_from('<I', b, 80)[0], struct.unpack_from('<I', b, 88)[0], struct.unpack_from('<I', b, 96)[0]
at = heap * sector + (root - 2) * cluster
f.seek(at); d = f.read(cluster)
for i in range(0, cluster - 96, 32):
    if d[i] == 0x85:
        units = d[i + 32 + 3]
        name = d[i + 64 + 2:i + 64 + 2 + 2 * min(units, 15)].decode('utf-16-le')
        if name.lower() == 'sce_sys':
            s = at + i + 32
            first = struct.unpack_from('<I', d, i + 32 + 20)[0]
            f.seek(s + 1); f.write(bytes([d[i + 33] & ~0x02]))      # a FAT chain
            f.seek(s + 8); f.write(struct.pack('<Q', 1 << 60))       # ValidDataLength
            f.seek(s + 24); f.write(struct.pack('<Q', 1 << 60))      # DataLength
            f.seek(fat * sector + first * 4); f.write(struct.pack('<I', first))
            f.seek(heap * sector + (first - 2) * cluster)
            f.write((b'\x05' + b'\0' * 31) * (cluster // 32))       # nothing in it, and no end entry
            sys.exit(0)
sys.exit('no sce_sys entry')
PY
hostile "an exFAT folder 2^60 bytes long that loops" "$work/bigdir.exfat"
# a UFS2 root folder whose last entry header runs past its 512 bytes
python3 -I - "$work/small.ffpkg" "$work/short.ffpkg" <<'PY'
import shutil, struct, sys
shutil.copyfile(sys.argv[1], sys.argv[2])
f = open(sys.argv[2], 'r+b')
f.seek(65536); sb = f.read(1376)
iblkno = struct.unpack_from('<i', sb, 16)[0]; fsize = struct.unpack_from('<i', sb, 52)[0]
f.seek(iblkno * fsize + 2 * 256); d = f.read(256)
block = struct.unpack_from('<Q', d, 112)[0]
entry = struct.pack('<IHBB', 2, 508, 4, 1) + b'.\0\0\0'
f.seek(block * fsize); f.write(entry + b'\0' * (508 - len(entry)) + b'\xff\xff\xff\xff')
f.seek(iblkno * fsize + 2 * 256 + 16); f.write(struct.pack('<Q', 512))
PY
hostile "a UFS2 folder entry cut by the folder's end" "$work/short.ffpkg"
python3 -I - "$work/small.ffpkg" "$work/odd.ffpkg" <<'PY'
import shutil, struct, sys
shutil.copyfile(sys.argv[1], sys.argv[2])
f = open(sys.argv[2], 'r+b')
f.seek(65536); sb = f.read(1376)
iblkno = struct.unpack_from('<i', sb, 16)[0]; fsize = struct.unpack_from('<i', sb, 52)[0]
f.seek(iblkno * fsize + 2 * 256 + 16); f.write(struct.pack('<Q', 509))  # not whole 512-byte chunks
PY
hostile "a UFS2 folder 509 bytes long" "$work/odd.ffpkg"

# -- packages
pkg() { python3 -I "$here/image/make_pkg.py" "$@"; }
check_pkg() { # name file key=value...
    local name="$1" file="$2" info ok=1 kv k v
    shift 2
    info="$("$tool" info "$file" || true)"
    for kv in "$@"; do
        k="${kv%%=*}"; v="${kv#*=}"
        [ "$(jget "$info" "$k" 2>/dev/null || true)" = "$v" ] || ok=0
    done
    [ $ok = 1 ] && pass "$name: $info" || failed "$name: $info (wanted $*)"
}
pkg "$work/p4.pkg" ps4 --name 'Tëst "App"'
check_pkg "a PS4 fake package" "$work/p4.pkg" kind=pkg platform=PS4 id=CUSA12345 'name=Tëst "App"' version=01.00 category=gd fake=true icon=true encrypted=false
pkg "$work/p4p.pkg" ps4 --category gp
check_pkg "a PS4 patch: its own version (APP_VER)" "$work/p4p.pkg" category=gp version=01.05
pkg "$work/p4r.pkg" ps4 --retail
check_pkg "a PS4 retail package" "$work/p4r.pkg" fake=false id=CUSA12345
pkg "$work/p5.pkg" ps5 --name 'PS5 App' --version 02.001.000
check_pkg "a PS5 fake package (FIH, its CNT inside)" "$work/p5.pkg" kind=pkg platform=PS5 id=PPSA12345 'name=PS5 App' version=02.001.000 fake=true icon=true
pkg "$work/p5b.pkg" ps5 --bom
check_pkg "a param.json with a byte order mark" "$work/p5b.pkg" id=PPSA12345 'name=Test App'
pkg "$work/p5n.pkg" ps5 --by-name
check_pkg "entries found by name" "$work/p5n.pkg" id=PPSA12345 icon=true
pkg "$work/p5e.pkg" ps5 --encrypted
check_pkg "encrypted entries: the ID from the content ID" "$work/p5e.pkg" id=PPSA12345 encrypted=true icon=false name=
pkg "$work/UP9000-PPSA54321_00-RETAIL.pkg" ps5 --retail
check_pkg "a PS5 retail package: the ID from its file name" "$work/UP9000-PPSA54321_00-RETAIL.pkg" fake=false encrypted=true id=PPSA54321
for h in count offset names sfo; do
    pkg "$work/h4.pkg" ps4 --hostile $h
    pkg "$work/h5.pkg" ps5 --hostile $h
    a="$("$tool" info "$work/h4.pkg" || true)"; b="$("$tool" info "$work/h5.pkg" || true)"
    [[ ( $a == ERR* || $a == '{'* ) && ( $b == ERR* || $b == '{'* ) ]] && pass "hostile ($h): handled: ${a:0:70}… / ${b:0:70}…" || failed "hostile $h: $a / $b"
done
for n in 3 100 300 8300 70000; do
    pkg "$work/t4.pkg" ps4 --truncate $n
    pkg "$work/t5.pkg" ps5 --truncate $n
    a="$("$tool" info "$work/t4.pkg" || true)"; b="$("$tool" info "$work/t5.pkg" || true)"
    [[ ( $a == ERR* || $a == '{'* ) && ( $b == ERR* || $b == '{'* ) ]] && pass "cut to $n bytes: handled" || failed "truncated $n: $a / $b"
done
pkg "$work/t5.pkg" ps5 --truncate 300
got="$("$tool" info "$work/t5.pkg" || true)"
[[ $got == *"cut short"* ]] && pass "a PS5 package cut short says so: $got" || failed "cut short: $got"
got="$("$tool" icon "$work/p5.pkg" "$work/p5.png")" && [ "$(head -c 8 "$work/p5.png" | xxd -p)" = 89504e470d0a1a0a ] \
    && pass "the icon out of a package: $got" || failed "pkg icon: $got"
got="$("$tool" icon "$work/p5e.pkg" "$work/x.png" || true)"
[[ $got == ERR*encrypted* ]] && pass "no icon from an encrypted one: $got" || failed "encrypted icon: $got"
head -c 5000 /dev/urandom >"$work/notpng"
pkg "$work/np.pkg" ps4 --icon "$work/notpng"
got="$("$tool" icon "$work/np.pkg" "$work/x.png" || true)"
[[ $got == *"isn't a PNG"* ]] && [ ! -e "$work/x.png" ] && pass "an icon that isn't a PNG is refused: $got" || failed "not png: $got"
head -c $((5 * 1024 * 1024)) /dev/urandom >"$work/hugeicon"
pkg "$work/hi.pkg" ps4 --icon "$work/hugeicon"
got="$("$tool" icon "$work/hi.pkg" "$work/x.png" || true)"
[[ $got == ERR* ]] && pass "a 5 MB icon is refused (4 MiB at most): $got" || failed "huge icon: $got"

sdk="$root/toolchain/ps5-payload-sdk"
if [ -x "$sdk/bin/prospero-clang" ] && [ -f "$root/env.sh" ]; then
    for f in ps5ufs2 ps5image; do
        if (. "$root/env.sh" >/dev/null 2>&1; "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -c "$apps/$f.c" -o "$work/$f.o") 2>"$work/ps5cc.log"; then
            pass "$f.c compiles for the PS5 (no warnings)"
        else
            cat "$work/ps5cc.log"; failed "$f.c doesn't compile for the PS5"
        fi
    done
fi
echo "$passes passed, $fails failed"
[ $fails = 0 ]
