#!/usr/bin/env bash
# The download service's archive unpacker (apps/common/ps5archive.c) on the
# Mac, against libarchive 3.8.1 with our patches (mac-libarchive.sh):
#   port/webkit/test/downloads/archive-test.sh
# Makes ZIP, 7z and tar archives of a small tree (and the splits of each)
# with zip, 7zz, tar and python3, unpacks them and compares the result file
# by file; RAR4/RAR5 come from libarchive 3.8.1's own test files (decoded
# from the tarball port/deps/build.sh libarchive downloads). Then checks
# that unsafe, incomplete and damaged archives are refused or fail with the
# right message, that ZIP and RAR5 archives with a password unpack with it
# (and say when it's needed or wrong), and that ps5archive.c compiles for
# the PS5.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
# libarchive 3.8.1 with port/deps/patches/libarchive (RAR5 passwords) and
# OpenSSL, as on the PS5: built once by mac-libarchive.sh.
LA="${LIBARCHIVE:-$("$here/mac-libarchive.sh")}"
SEVENZ="${SEVENZ:-/opt/homebrew/bin/7zz}"
LA_TARBALL="$root/toolchain/wpe-deps/dl/libarchive-3.8.1.tar.xz"
LA_URL=https://www.libarchive.org/downloads/libarchive-3.8.1.tar.xz
LA_SHA=19f917d42d530f98815ac824d90c7eaf648e9d9a50e4f309c812457ffa5496b5
work="$(mktemp -d)"
trap 'chmod -R u+w "$work" 2>/dev/null; rm -rf "$work"' EXIT
at="$work/archive-test"
cc -std=c11 -O2 -Wall -Wextra -Werror -I"$LA/include" -o "$at" "$here/archive/archive-test.c" \
    "$root/port/webkit/apps/common/ps5archive.c" -L"$LA/lib" -Wl,-rpath,"$LA/lib" -larchive -lz
export COPYFILE_DISABLE=1 # no ._ files from tar
fails=0
passes=0
pass() { echo "PASS $1"; passes=$((passes + 1)); }
failed() { echo "FAIL $1"; fails=$((fails + 1)); }

# check <expected (a glob)> <name> <archive-test args...>; PW=<password>
# before it gives the archive's password.
check() {
    local expect="$1" name="$2"
    shift 2
    local got
    got="$(PASSWORD="${PW:-}" "$at" "$@" 2>&1 || true)"
    if [[ "$got" == $expect ]]; then pass "$name: $got"; else failed "$name: $got (expected $expect)"; fi
}

# same <name> <expected tree> <unpacked tree>: names (NFC: libarchive turns
# 7z/RAR names to NFD on the Mac only), types and contents; all 0777.
same() {
    local name="$1" want="$2" got="$3" r
    r="$(python3 -I - "$want" "$got" <<'PY'
import hashlib, os, sys, unicodedata
def tree(top):
    out = {}
    for d, dirs, files in os.walk(top):
        for n in dirs + files:
            p = os.path.join(d, n)
            rel = unicodedata.normalize("NFC", os.path.relpath(p, top))
            if os.path.islink(p):
                out[rel] = "link"
            elif os.path.isdir(p):
                out[rel] = "dir"
            else:
                out[rel] = hashlib.sha256(open(p, "rb").read()).hexdigest()
    return out
a, b = tree(sys.argv[1]), tree(sys.argv[2])
if a != b:
    diff = sorted(set(a.items()) ^ set(b.items()))[:4]
    print("differ:", diff)
PY
)"
    if [ -z "$r" ]; then pass "$name: same files"; else failed "$name: $r"; fi
    local bad
    bad=$(find "$got" -mindepth 1 ! -perm 777 | head -3)
    [ -z "$bad" ] || failed "$name: not 0777: $bad"
}

# extract <name> <archive> [reference tree]: unpacks into a fresh folder
# and compares it with the reference (default: the source tree).
extract() {
    local name="$1" archive="$2" want="${3:-$work/src}"
    rm -rf "$work/out"
    check "OK progress=ok" "$name: unpacks" extract "$archive" "$work/out"
    same "$name" "$want" "$work/out"
}

# ------------------------------------------------------------- the tree
src="$work/src"
mkdir -p "$src/Game-1.0/sce_sys" "$src/Game-1.0/data/empty" "$work/a"
echo '{"titleId":"TEST00001"}' > "$src/Game-1.0/sce_sys/param.json"
head -c 300000 /dev/urandom > "$src/Game-1.0/eboot.bin"
head -c 5000000 /dev/urandom > "$src/Game-1.0/data/big.bin" # > ps5archive's 4 MB write buffer
cp /bin/ls "$src/Game-1.0/data/code.bin"                     # something for the BCJ/ARM filters
printf 'unicode' > "$src/Game-1.0/data/日本語 é.txt"
: > "$src/Game-1.0/data/zero"
a="$work/a"

# ------------------------------------------------------------- ZIP
(cd "$src" && zip -qr -X "$a/deflate.zip" Game-1.0 && zip -qr -X -0 "$a/stored.zip" Game-1.0 &&
    zip -qr -X -s 1m "$a/split.zip" Game-1.0)
(cd "$src" && "$SEVENZ" a -bd -bso0 -tzip -mm=BZip2 "$a/bzip2.zip" Game-1.0 &&
    "$SEVENZ" a -bd -bso0 -tzip -mm=LZMA "$a/lzma.zip" Game-1.0 &&
    "$SEVENZ" a -bd -bso0 -tzip -mm=Deflate64 "$a/deflate64.zip" Game-1.0 &&
    "$SEVENZ" a -bd -bso0 -tzip -v1m "$a/bytes.zip" Game-1.0)
check "OK format=ZIP volumes=1 entries=10 unpacked=* sizes_known=1 top=Game-1.0/ title=Game-1.0" "ZIP: inspect" inspect "$a/deflate.zip"
# an empty folder in the app (an image can't take files added there later)
check "data/empty" "ZIP: the app's empty folder" emptyfolder "$a/deflate.zip"
extract "ZIP deflate" "$a/deflate.zip"
extract "ZIP stored" "$a/stored.zip"
extract "ZIP bzip2" "$a/bzip2.zip"
extract "ZIP lzma" "$a/lzma.zip"
check "OK format=ZIP volumes=1 *" "ZIP deflate64: inspect" inspect "$a/deflate64.zip"
check "FAILED The archive uses something that isn't supported*" "ZIP deflate64: unpacking" extract "$a/deflate64.zip" "$work/d64"
parts() { (cd "$a" && for p in "$@"; do ls -d $p; done | tr '\n' ' ' | sed 's/ $//'); } # a set's names (patterns), in order
nz=$(parts 'split.z[0-9][0-9]' split.zip | wc -w | tr -d ' ')
check "$(parts 'split.z[0-9][0-9]' split.zip)" "ZIP split (.z01..): the parts" volumes "$a/split.z03"
extract "ZIP split (.z01.., .zip last)" "$a/split.zip"
nb=$(parts 'bytes.zip.0*' | wc -w | tr -d ' ')
check "$(parts 'bytes.zip.0*')" "ZIP byte split (.zip.001): the parts" volumes "$a/bytes.zip.004"
extract "ZIP byte split (.zip.001)" "$a/bytes.zip.001"
mv "$a/split.z02" "$work/hidden"
check "FAILED Part 2 of $nz is missing: split.z02" "ZIP split, a part missing" inspect "$a/split.zip"
mv "$work/hidden" "$a/split.z02"
mv "$a/bytes.zip.00$nb" "$work/hidden"
check "FAILED Part $nb is missing: bytes.zip.00$nb" "ZIP byte split, the last part missing" inspect "$a/bytes.zip.001"
mv "$work/hidden" "$a/bytes.zip.00$nb"

python3 -I - "$a" <<'PY'
import os, stat, sys, zipfile, warnings
w = sys.argv[1]
warnings.simplefilter("ignore")
def make(name, entries, method=zipfile.ZIP_DEFLATED):
    with zipfile.ZipFile(f"{w}/{name}.zip", "w", method) as z:
        for e in entries:
            if isinstance(e, zipfile.ZipInfo):
                z.writestr(e, b"x")
            else:
                z.writestr(e[0], e[1])
def special(name, mode):
    i = zipfile.ZipInfo(name)
    i.create_system = 3
    i.external_attr = mode << 16
    return i
make("parent", [("Game/../../evil", b"x")])
make("absolute", [("/etc/evil", b"x")])
make("drive", [("C:/evil", b"x")])
make("backslash-root", [("\\evil", b"x")])
make("backslash-parent", [("Game\\..\\..\\evil", b"x")])
make("windows", [("Game\\sub\\file.txt", b"made on Windows")])
make("symlink", [("Game/a", b"x"), special("Game/link", stat.S_IFLNK | 0o777)])
make("fifo", [special("Game/fifo", stat.S_IFIFO | 0o666)])
make("device", [special("Game/dev", stat.S_IFCHR | 0o666)])
make("twice", [("Game/a", b"1"), ("Game/a", b"2")])
make("file-and-folder", [("Game/a", b"1"), ("Game/a/b", b"2")])
make("control", [("Game/bad\x01name", b"x")])
make("dots", [("./Game/./a", b"dots"), ("Game//b", b"slashes")])
make("empty", [])
make("root-title", [("eboot.bin", b"elf"), ("sce_sys/param.json", b"{}"), ("deeper/x/eboot.bin", b"e"), ("deeper/x/sce_sys/param.json", b"{}")])
make("deep-title", [("pack/readme.txt", b"r"), ("pack/b/eboot.bin", b"e"), ("pack/b/sce_sys/param.json", b"{}"), ("pack/a/c/eboot.bin", b"e"), ("pack/a/c/sce_sys/param.json", b"{}")])
make("no-title", [("pack/eboot.bin", b"e"), ("pack/sce_sys/other.json", b"{}")])
make("big", [("Game/big", b"\0" * 3000000)])
# zip64: a forced zip64 entry, and more entries than a plain ZIP holds
with zipfile.ZipFile(f"{w}/zip64.zip", "w") as z:
    with z.open("Game/forced", "w", force_zip64=True) as f:
        f.write(b"zip64 " * 1000)
    for i in range(70000):
        z.writestr(f"Game/many/{i // 1000}/{i}", b"")
# a CRC that doesn't match (stored data, one byte flipped)
make("crc", [("Game/a", b"stored data here")], zipfile.ZIP_STORED)
d = bytearray(open(f"{w}/crc.zip", "rb").read())
d[d.find(b"stored data here")] ^= 1
open(f"{w}/crc.zip", "wb").write(d)
open(f"{w}/notarchive.zip", "wb").write(b"not an archive at all " * 100)
PY
check "REFUSED Unsafe path in the archive: Game/../../evil" "ZIP with ../" inspect "$a/parent.zip"
check "REFUSED Unsafe path in the archive: /etc/evil" "ZIP with an absolute path" inspect "$a/absolute.zip"
check "REFUSED Unsafe path in the archive: C:/evil" "ZIP with a drive letter" inspect "$a/drive.zip"
check "REFUSED Unsafe path*" "ZIP with a leading backslash" inspect "$a/backslash-root.zip"
check "REFUSED Unsafe path*" "ZIP with ..\\" inspect "$a/backslash-parent.zip"
check "REFUSED Links aren't supported: Game/link" "ZIP with a symbolic link" inspect "$a/symlink.zip"
# libarchive reads a ZIP FIFO as a file (Info-ZIP stores stdin that way)
check "OK format=ZIP volumes=1 entries=1 *" "ZIP with a FIFO: a plain file" inspect "$a/fifo.zip"
check "REFUSED Special files aren't supported: Game/dev" "ZIP with a device" inspect "$a/device.zip"
check "REFUSED The archive has Game/a twice" "ZIP with an entry twice" inspect "$a/twice.zip"
check "REFUSED The archive has Game/a as a file and a folder" "ZIP with a file that's also a folder" inspect "$a/file-and-folder.zip"
check "REFUSED A name in the archive has control characters" "ZIP with a control character" inspect "$a/control.zip"
check "REFUSED The archive is empty" "empty ZIP" inspect "$a/empty.zip"
check "FAILED This isn't an archive that can be unpacked" "not an archive" inspect "$a/notarchive.zip"
check "OK *top=- title=." "title at the root (the shallowest wins)" inspect "$a/root-title.zip"
check "-" "an app without an empty folder" emptyfolder "$a/root-title.zip"
check "OK *top=pack/ title=pack/b" "title two folders down (shallowest, then by name)" inspect "$a/deep-title.zip"
check "OK *top=pack/ title=-" "no title (no param.json)" inspect "$a/no-title.zip"
check "REFUSED The archive unpacks to more than 1 MB" "over max_bytes" inspect "$a/big.zip" 100 1048576
check "REFUSED The archive has more than 1000 files" "over max_entries" inspect "$a/zip64.zip" 1000 1073741824
check "OK format=ZIP volumes=1 entries=70001 *" "zip64 (70000 entries, a forced zip64 entry)" inspect "$a/zip64.zip"
rm -rf "$work/out"
check "OK progress=ok" "zip64: unpacks" extract "$a/zip64.zip" "$work/out"
[ "$(find "$work/out/Game/many" -type f | wc -l | tr -d ' ')" = 70000 ] && [ "$(cat "$work/out/Game/forced")" = "$(python3 -c 'print("zip64 " * 1000, end="")')" ] &&
    pass "zip64: every file there" || failed "zip64: files missing or wrong"
rm -rf "$work/out"
check "OK progress=ok" "Windows separators" extract "$a/windows.zip" "$work/out"
[ "$(cat "$work/out/Game/sub/file.txt" 2>/dev/null)" = "made on Windows" ] && pass "Windows separators: Game/sub/file.txt" || failed "Windows separators: no Game/sub/file.txt"
rm -rf "$work/out"
check "OK progress=ok" "./ and // parts" extract "$a/dots.zip" "$work/out"
[ "$(cat "$work/out/Game/a")$(cat "$work/out/Game/b")" = "dotsslashes" ] && pass "./ and // parts: dropped" || failed "./ and // parts"
check "OK format=ZIP *" "bad CRC: inspect reads headers only" inspect "$a/crc.zip"
check "FAILED The archive is damaged or incomplete (ZIP bad CRC*" "bad CRC: unpacking fails" extract "$a/crc.zip" "$work/crc"
# passwords: PKWARE's (zip -P) and WinZip AES (7zz)
(cd "$src" && zip -qr -X -P secret "$a/encrypted.zip" Game-1.0)
(cd "$src" && "$SEVENZ" a -bd -bso0 -tzip -mem=AES256 -psecret "$a/aes.zip" Game-1.0)
for z in encrypted aes; do
    check "PASSWORD It has a password: enter it to unpack it" "$z ZIP: its password is needed" inspect "$a/$z.zip"
    check "PASSWORD It has a password: enter it to unpack it" "$z ZIP: unpacking without it" extract "$a/$z.zip" "$work/enc"
    [ ! -e "$work/enc" ] && pass "$z ZIP: nothing written" || failed "$z ZIP: $work/enc was made"
    PW=nope check "PASSWORD That isn't its password wrong" "$z ZIP: a wrong password" inspect "$a/$z.zip"
    PW=nope check "PASSWORD That isn't its password" "$z ZIP: unpacking with a wrong password" extract "$a/$z.zip" "$work/enc"
    [ ! -e "$work/enc" ] && pass "$z ZIP: nothing written for a wrong password" || failed "$z ZIP: $work/enc was made"
    PW=secret check "OK format=ZIP volumes=1 entries=10 * encrypted" "$z ZIP: inspect with its password" inspect "$a/$z.zip"
    PW=secret extract "$z ZIP with its password" "$a/$z.zip"
done

# ------------------------------------------------------------- 7z
seven() { # <name> <7zz options...>
    local name="$1"
    shift
    (cd "$src" && "$SEVENZ" a -bd -bso0 -bse0 "$@" "$a/$name.7z" Game-1.0)
}
seven lzma2
seven lzma -m0=LZMA
seven bcj -mf=BCJ
seven arm -mf=ARM
seven arm64 -mf=ARM64
seven bcj2 -mf=BCJ2
seven ppmd -m0=PPMd
seven bzip2 -m0=BZip2
seven deflate -m0=Deflate
seven copy -m0=Copy
seven nonsolid -ms=off
seven encrypted -psecret
seven encrypted-names -psecret -mhe=on
(cd "$src" && "$SEVENZ" a -bd -bso0 -v1m "$a/split.7z" Game-1.0)
check "OK format=7-Zip volumes=1 entries=10 unpacked=* sizes_known=1 top=Game-1.0/ title=Game-1.0" "7z: inspect" inspect "$a/lzma2.7z"
check "data/empty" "7z: the app's empty folder" emptyfolder "$a/lzma2.7z"
for m in lzma2 lzma bcj arm arm64 bcj2 ppmd bzip2 deflate copy nonsolid; do
    extract "7z $m" "$a/$m.7z"
done
# libarchive can't decrypt 7-Zip's AES: refused, even with the password
for z in encrypted encrypted-names; do
    check "REFUSED 7-Zip archives with a password can't be unpacked here" "7z $z" inspect "$a/$z.7z"
    PW=secret check "REFUSED 7-Zip archives with a password can't be unpacked here" "7z $z, with its password" extract "$a/$z.7z" "$work/enc"
done
n7=$(parts 'split.7z.0*' | wc -w | tr -d ' ')
check "$(parts 'split.7z.0*')" "7z split: the parts" volumes "$a/split.7z.002"
extract "7z split (.7z.001)" "$a/split.7z.003"
mv "$a/split.7z.003" "$work/hidden"
check "FAILED Part 3 of $n7 is missing: split.7z.003" "7z split, a part missing" extract "$a/split.7z.001" "$work/nope"
mv "$work/hidden" "$a/split.7z.003"
mv "$a/split.7z.00$n7" "$work/hidden"
check "FAILED Part $n7 of $n7 is missing: split.7z.00$n7" "7z split, the last part missing" inspect "$a/split.7z.001"
mv "$work/hidden" "$a/split.7z.00$n7"
head -c 2000000 "$a/lzma2.7z" > "$a/truncated.7z"
check "FAILED The archive is damaged or incomplete*" "truncated 7z" inspect "$a/truncated.7z"

# ------------------------------------------------------------- tar
(cd "$src" && tar -cf "$a/plain.tar" Game-1.0 && tar -czf "$a/t.tar.gz" Game-1.0 && tar -cjf "$a/t.tar.bz2" Game-1.0 &&
    tar -cJf "$a/t.tar.xz" Game-1.0 && tar -cf - Game-1.0 | zstd -q > "$a/t.tar.zst" &&
    tar -cf - Game-1.0 | xz --format=lzma > "$a/t.tar.lzma" && tar -cf "$a/dot.tar" .)
check "OK format=tar.gz volumes=1 entries=10 *top=Game-1.0/ title=Game-1.0" "tar.gz: inspect" inspect "$a/t.tar.gz"
extract "tar" "$a/plain.tar"
extract "tar.gz" "$a/t.tar.gz"
extract "tar.bz2" "$a/t.tar.bz2"
extract "tar.xz" "$a/t.tar.xz"
extract "tar.zst" "$a/t.tar.zst"
extract "tar.lzma" "$a/t.tar.lzma"
extract "tar of ./" "$a/dot.tar"
head -c 3000000 "$a/t.tar.gz" > "$a/truncated.tar.gz"
check "FAILED The archive is damaged or incomplete*" "truncated tar.gz" extract "$a/truncated.tar.gz" "$work/trunc"
python3 -I - "$a" <<'PY'
import io, sys, tarfile
w = sys.argv[1]
def make(name, *infos):
    with tarfile.open(f"{w}/{name}.tar", "w", format=tarfile.GNU_FORMAT) as t:
        for info, data in infos:
            t.addfile(info, io.BytesIO(data) if data is not None else None)
def info(name, type=tarfile.REGTYPE, size=0, link=""):
    i = tarfile.TarInfo(name)
    i.type, i.size, i.linkname = type, size, link
    return i
make("t-symlink", (info("Game/a", size=1), b"x"), (info("Game/l", tarfile.SYMTYPE, link="/etc/passwd"), None))
make("t-hardlink", (info("Game/a", size=1), b"x"), (info("Game/h", tarfile.LNKTYPE, link="Game/a"), None))
make("t-parent", (info("../evil", size=1), b"x"))
make("t-absolute", (info("/tmp/evil", size=1), b"x"))
make("t-device", (info("Game/dev", tarfile.CHRTYPE), None))
make("t-fifo", (info("Game/fifo", tarfile.FIFOTYPE), None))
make("t-latin1", (info("Game/caf\udce9.txt", size=4), b"cafe")) # a Latin-1 byte
PY
check "REFUSED Links aren't supported: Game/l" "tar with a symbolic link" inspect "$a/t-symlink.tar"
check "REFUSED Links aren't supported: Game/h" "tar with a hard link" inspect "$a/t-hardlink.tar"
check "REFUSED Unsafe path in the archive: ../evil" "tar with ../" inspect "$a/t-parent.tar"
check "REFUSED Unsafe path in the archive: /tmp/evil" "tar with an absolute path" inspect "$a/t-absolute.tar"
check "REFUSED Special files aren't supported: Game/dev" "tar with a device" inspect "$a/t-device.tar"
check "REFUSED Special files aren't supported: Game/fifo" "tar with a FIFO" inspect "$a/t-fifo.tar"
rm -rf "$work/out"
check "OK progress=ok" "tar with a name that isn't UTF-8" extract "$a/t-latin1.tar" "$work/out"
[ "$(cat "$work/out/Game/caf_.txt" 2>/dev/null)" = cafe ] && pass "a byte that isn't UTF-8 becomes _" || failed "no Game/caf_.txt"

# ------------------------------------------------------------- RAR
# libarchive's own RAR test files: BSD-licensed, from libarchive/test/ in
# the 3.8.1 tarball (uuencoded there).
tarball="$LA_TARBALL"
if [ ! -s "$tarball" ]; then
    tarball="$work/libarchive-3.8.1.tar.xz"
    curl -sSfL -o "$tarball" "$LA_URL" 2>/dev/null || rm -f "$tarball"
fi
if [ -s "$tarball" ] && echo "$LA_SHA  $tarball" | shasum -a 256 -c - >/dev/null 2>&1; then
    rar="$work/rar"
    mkdir -p "$rar/uu"
    tar -xf "$tarball" -C "$rar/uu" --strip-components=3 'libarchive-3.8.1/libarchive/test/test_read_format_rar*.rar.uu'
    for f in "$rar"/uu/*.uu; do
        n="$(basename "$f" .uu)"
        n="${n#test_read_format_}"
        uudecode -o "$rar/$n" "$f"
    done
    # bsdtar's unpacking of the same file is the reference
    for f in rar_windows rar5_stored rar5_compressed rar5_multiple_files rar5_win32 rar5_arm rar5_blake2 rar5_solid; do
        mkdir -p "$work/ref-$f"
        "$LA/bin/bsdtar" -xf "$rar/$f.rar" -C "$work/ref-$f"
        chmod -R u+rw "$work/ref-$f"
        extract "$f" "$rar/$f.rar" "$work/ref-$f"
    done
    check "OK format=RAR volumes=1 entries=5 *" "RAR4: inspect" inspect "$rar/rar_windows.rar"
    check "OK format=RAR5 volumes=1 entries=8 *" "RAR5: inspect" inspect "$rar/rar5_win32.rar"
    # RAR5 multivolume: 8 parts, two files spread over them
    check "OK format=RAR5 volumes=8 entries=2 unpacked=510280 sizes_known=1 top=home/ title=-" "RAR5 multivolume (.part01..08): inspect" \
        inspect "$rar/rar5_multiarchive.part05.rar"
    rm -rf "$work/out"
    check "OK progress=ok" "RAR5 multivolume: unpacks" extract "$rar/rar5_multiarchive.part01.rar" "$work/out"
    [ "$(find "$work/out" -type f -exec cat {} + | wc -c | tr -d ' ')" = 510280 ] && pass "RAR5 multivolume: 510280 bytes" || failed "RAR5 multivolume: wrong size"
    rm -rf "$work/out"
    check "OK progress=ok" "RAR5 solid multivolume: unpacks" extract "$rar/rar5_multiarchive_solid.part01.rar" "$work/out"
    # RAR4 multivolume: a link in the last part, so refused, after reading
    # a file that spans the first three
    check "REFUSED Links aren't supported: testlink" "RAR4 multivolume (.part0001..4): read to the last part" inspect "$rar/rar_multivolume.part0002.rar"
    mkdir -p "$rar/old"
    cp "$rar/rar_multivolume.part0001.rar" "$rar/old/set.rar"
    for i in 2 3 4; do cp "$rar/rar_multivolume.part000$i.rar" "$rar/old/set.r0$((i - 2))"; done
    check "set.rar set.r00 set.r01 set.r02" "old-style RAR set (.rar, .r00..): the parts" volumes "$rar/old/set.r01"
    check "REFUSED Links aren't supported: testlink" "old-style RAR set: read to the last part" inspect "$rar/old/set.rar"
    mv "$rar/rar_multivolume.part0003.rar" "$work/hidden"
    check "FAILED Part 3 of 4 is missing: rar_multivolume.part0003.rar" "RAR4 multivolume, a part missing" inspect "$rar/rar_multivolume.part0001.rar"
    mv "$work/hidden" "$rar/rar_multivolume.part0003.rar"
    mv "$rar/rar5_multiarchive.part08.rar" "$work/hidden"
    check "FAILED Part 8 is missing: rar5_multiarchive.part08.rar" "RAR5 multivolume, the last part missing" inspect "$rar/rar5_multiarchive.part01.rar"
    mv "$work/hidden" "$rar/rar5_multiarchive.part08.rar"
    rm "$rar/old/set.r02"
    check "FAILED Part 4 is missing: set.r02" "old-style RAR set, the last part missing" inspect "$rar/old/set.rar"
    check "REFUSED Links aren't supported: testlink" "RAR4 with a symbolic link" inspect "$rar/rar.rar"
    check "REFUSED Links aren't supported: 表*よ/ファイル" "RAR4 Unicode names (and a link)" inspect "$rar/rar_unicode.rar"
    check "REFUSED Links aren't supported: symlink.txt" "RAR5 with a symbolic link" inspect "$rar/rar5_symlink.rar"
    check "REFUSED Links aren't supported: hardlink.txt" "RAR5 with a hard link" inspect "$rar/rar5_hardlink.rar"
    # Passwords (port/deps/patches/libarchive): libarchive's own files, made
    # by RAR. a-d.txt hold "This is from <x>.txt"; their password is
    # "password", except rar[45]_encrypted's d.txt: "password2". The
    # rar_encryption_* files' is "12345678".
    mkdir -p "$work/rar-want"
    for x in a b c d; do printf 'This is from %s.txt' $x >"$work/rar-want/$x.txt"; done
    # RAR4 (RAR 3.x's AES): no check value, so a wrong password shows when
    # the file is unpacked, unless the names are encrypted (their CRC)
    check "PASSWORD It has a password: enter it to unpack it" "RAR4 with a password: needed" inspect "$rar/rar4_encrypted.rar"
    PW=password check "OK format=RAR volumes=1 entries=4 * encrypted" "RAR4 with a password: inspect" inspect "$rar/rar4_encrypted.rar"
    PW=password check "PASSWORD That isn't its password, or the archive is damaged" "RAR4: a file with another password" \
        extract "$rar/rar4_encrypted.rar" "$work/out"
    check "PASSWORD It has a password: enter it to unpack it names" "RAR4 -hp (names too): needed" inspect "$rar/rar4_encrypted_filenames.rar"
    PW=nope check "PASSWORD That isn't its password names wrong" "RAR4 -hp: a wrong one" inspect "$rar/rar4_encrypted_filenames.rar"
    PW=password extract "RAR4 -hp with its password" "$rar/rar4_encrypted_filenames.rar" "$work/rar-want"
    PW=12345678 check "OK format=RAR volumes=1 entries=2 * encrypted" "RAR4 -hp (12345678): inspect" inspect "$rar/rar_encryption_header.rar"
    for f in rar_encryption_header rar_encryption_data rar_encryption_partially; do
        rm -rf "$work/out"
        PW=12345678 check "OK progress=ok" "RAR4 with a password: $f unpacks (CRCs checked)" extract "$rar/$f.rar" "$work/out"
    done
    # RAR5
    check "PASSWORD It has a password: enter it to unpack it" "RAR5 with a password: needed" inspect "$rar/rar5_encrypted.rar"
    PW=nope check "PASSWORD That isn't its password wrong" "RAR5 with a password: a wrong one" inspect "$rar/rar5_encrypted.rar"
    PW=password check "OK format=RAR5 volumes=1 entries=4 * encrypted" "RAR5 with a password: inspect" inspect "$rar/rar5_encrypted.rar"
    PW=password check "PASSWORD That isn't its password" "RAR5: a file with another password" extract "$rar/rar5_encrypted.rar" "$work/out"
    check "PASSWORD It has a password: enter it to unpack it names" "RAR5 -hp (names too): needed" inspect "$rar/rar5_encrypted_filenames.rar"
    PW=nope check "PASSWORD That isn't its password names wrong" "RAR5 -hp: a wrong one" inspect "$rar/rar5_encrypted_filenames.rar"
    for f in rar5_encrypted_filenames rar5_solid_encrypted rar5_solid_encrypted_filenames; do
        PW=password extract "RAR5 with a password: $f" "$rar/$f.rar" "$work/rar-want"
    done
    # A recovery record (rar -rr: a service header "RR" whose only extra
    # field is one libarchive skips) after the files: libarchive 3.8.1
    # failed on it, with no message, once the files were read (0003).
    mkdir -p "$work/ref-stored" "$work/rr"
    "$at" extract "$rar/rar5_stored.rar" "$work/ref-stored" >/dev/null
    python3 -I "$here/archive/rar5rr.py" "$rar/rar5_stored.rar" "$work/rr/rr.rar"
    check "OK format=RAR5 volumes=1 entries=1 *" "RAR5 with a recovery record: inspect" inspect "$work/rr/rr.rar"
    extract "RAR5 with a recovery record" "$work/rr/rr.rar" "$work/ref-stored"
    if command -v openssl >/dev/null; then
        python3 -I "$here/archive/rar5enc.py" --hp --password 'pässwörd' "$work/rr/enc" "$work/rr/rr.rar"
        PW='pässwörd' check "OK format=RAR5 volumes=1 entries=1 * encrypted" "RAR5 -hp with a recovery record: inspect" inspect "$work/rr/enc/rr.rar"
        PW='pässwörd' extract "RAR5 -hp with a recovery record" "$work/rr/enc/rr.rar" "$work/ref-stored"
    fi
    # Bigger ones: libarchive's multivolume sets encrypted the way RAR does
    # (archive/rar5enc.py: one AES stream per file across the volumes,
    # checksums as HMACs; --hp the headers too). Their parts end inside AES
    # blocks.
    if command -v openssl >/dev/null; then
        mkdir -p "$work/ref-multi" "$work/ref-solid"
        "$at" extract "$rar/rar5_multiarchive.part01.rar" "$work/ref-multi" >/dev/null
        "$at" extract "$rar/rar5_multiarchive_solid.part01.rar" "$work/ref-solid" >/dev/null
        for opts in "" "--hp"; do
            for set in rar5_multiarchive rar5_multiarchive_solid; do
                enc="$work/enc5-$set${opts:+-hp}"
                python3 -I "$here/archive/rar5enc.py" $opts --password 'pässwörd' "$enc" "$rar/$set".part0*.rar
                ref="$work/ref-multi"
                [ "$set" = rar5_multiarchive_solid ] && ref="$work/ref-solid"
                PW='pässwörd' extract "RAR5 $set encrypted${opts:+ (-hp)}" "$enc/$set.part01.rar" "$ref"
            done
        done
        PW='passwörd' check "PASSWORD That isn't its password*" "RAR5 multivolume -hp: a wrong one" inspect "$work/enc5-rar5_multiarchive-hp/rar5_multiarchive.part03.rar"
        # RAR4's 4-part set (a file across three parts, one across two),
        # its link left out (archive/rar4enc.py); unpacking checks the CRCs
        for opts in "" "--hp"; do
            enc="$work/enc4${opts:+-hp}"
            python3 -I "$here/archive/rar4enc.py" $opts --drop testlink "$enc" "$rar"/rar_multivolume.part000*.rar
            rm -rf "$work/out"
            PW=password check "OK progress=ok" "RAR4 multivolume encrypted${opts:+ (-hp)}: unpacks" extract "$enc/rar_multivolume.part0001.rar" "$work/out"
            [ "$(find "$work/out" -type f -exec cat {} + | wc -c | tr -d ' ')" = 241688220 ] && pass "RAR4 multivolume encrypted${opts:+ (-hp)}: 241688220 bytes" \
                || failed "RAR4 multivolume encrypted${opts:+ (-hp)}: wrong size"
        done
        PW=nope check "PASSWORD That isn't its password names wrong" "RAR4 multivolume -hp: a wrong one" inspect "$work/enc4-hp/rar_multivolume.part0002.rar"
    else
        echo "SKIP RAR5 encrypted multivolume: no openssl"
    fi
else
    echo "SKIP RAR: no libarchive-3.8.1.tar.xz (port/deps/build.sh libarchive downloads it)"
fi

# ------------------------------------------------------------- unpacking
mkdir -p "$work/elsewhere" "$work/linked"
ln -s "$work/elsewhere" "$work/linked/Game-1.0"
check "FAILED A folder on the drive is a link: Game-1.0" "a link on the drive is not followed" extract "$a/deflate.zip" "$work/linked"
[ -z "$(ls -A "$work/elsewhere")" ] && pass "nothing written through the link" || failed "written through the link"
rm -rf "$work/twice"
"$at" extract "$a/deflate.zip" "$work/twice" >/dev/null
check "FAILED Game-1.0/*already exists" "a file already there isn't replaced" extract "$a/deflate.zip" "$work/twice"
check "FAILED Not enough space: needs *GB, *GB free" "free space" extract "$a/deflate.zip" "$work/full" 0 0 4611686018427387904
check "CANCELLED Cancelled" "cancelled" extract "$a/lzma2.7z" "$work/cancel" 0 0 0 cancel
mkdir -p "$work/readonly"
chmod 0555 "$work/readonly"
check "FAILED A folder couldn't be made: Game-1.0*" "a folder that can't be written to" extract "$a/deflate.zip" "$work/readonly"
rm -rf "$work/deep/new"
check "OK progress=ok" "dest made when missing (and its parents)" extract "$a/deflate.zip" "$work/deep/new/dest"

# ------------------------------------------------------------- names
for n in a.zip b.7z c.RAR d.part1.rar e.part01.rar f.r00 g.7z.001 h.zip.001 i.z01 j.tar k.tar.gz l.tgz m.tar.xz n.TAR.BZ2 o.tar.zst; do
    check yes "is an archive: $n" is "$n"
done
for n in a.txt b.gz c.001 d.exe .zip e.rar.txt f.z1; do
    check no "isn't an archive: $n" is "$n"
done
check "FAILED The file is missing: nothing.zip" "a file that isn't there" inspect "$work/nothing.zip"

# ------------------------------------------------------------- PS5
sdk="$root/toolchain/ps5-payload-sdk"
sysroot="$root/toolchain/wpe-sysroot"
if [ -x "$sdk/bin/prospero-clang" ] && [ -f "$sysroot/include/archive.h" ]; then
    if out="$(. "$root/env.sh" && prospero-clang -std=c11 -O2 -Wall -Wextra -Werror \
        -isystem "$root/port/webkit/shim/include" -include ps5shim.h -I"$sysroot/include" \
        -c "$root/port/webkit/apps/common/ps5archive.c" -o "$work/ps5archive-ps5.o" 2>&1)"; then
        pass "ps5archive.c compiles for the PS5 (no warnings)"
    else
        failed "ps5archive.c for the PS5: $out"
    fi
else
    echo "SKIP PS5 compile: no prospero-clang or no archive.h in toolchain/wpe-sysroot (port/deps/build.sh libarchive)"
fi

echo "archive-test: $passes passed, $fails failed"
[ "$fails" = 0 ] || exit 1
