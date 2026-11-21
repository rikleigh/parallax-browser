#!/usr/bin/env bash
# The archive peek (apps/common/ps5peek.c: what an archive holds, read
# with Range requests before it is downloaded, or from the drive after) on
# the Mac, against serve.py's /real/ files:
#   port/webkit/test/downloads/peek-test.sh
# Needs libsoup 3 and libarchive (brew install libsoup libarchive), zip and
# 7zz (brew install sevenzip). Makes an app folder (sce_sys/param.json,
# eboot.bin) next to 48 MiB of incompressible data, archives it several
# ways and checks what the peek finds and how few bytes it needed.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
apps="$root/port/webkit/apps/common"
LA="${LIBARCHIVE:-/opt/homebrew/opt/libarchive}"
SEVENZ="${SEVENZ:-/opt/homebrew/bin/7zz}"
LA_TARBALL="$root/toolchain/wpe-deps/dl/libarchive-3.8.1.tar.xz"
port="${PEEK_PORT:-8793}"
work="$(mktemp -d)"
server=""
cleanup() {
    [ -z "$server" ] || kill "$server" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

fails=0
passes=0
pass() { echo "PASS $*"; passes=$((passes + 1)); }
failed() { echo "FAIL $*"; fails=$((fails + 1)); }

drive="$work/peek-drive"
if cc -std=c11 -O2 -Wall -Wextra -Werror -I"$LA/include" -o "$drive" "$here/peek/peek-drive.c" "$apps/ps5peek.c" "$apps/ps5archive.c" \
    "$apps/ps5fetch.c" "$apps/ps5install.c" -L"$LA/lib" -larchive -lz $(pkg-config --cflags --libs libsoup-3.0) 2>"$work/cc.log"; then
    pass "builds without warnings (-Wall -Wextra)"
else
    cat "$work/cc.log"
    failed "doesn't build without warnings"
    exit 1
fi

# ------------------------------------------------------------- fixtures
files="$work/files"
src="$work/src"
mkdir -p "$files" "$src"
app() { # <folder> <title ID or ""> <name> <version>
    mkdir -p "$1/sce_sys"
    head -c 2000000 /dev/urandom > "$1/eboot.bin"
    local id=""
    [ -z "$2" ] || id="\"titleId\": \"$2\","
    cat > "$1/sce_sys/param.json" <<JSON
{
  "applicationCategoryType": 0,
  "contentId": "IV0000-$2_00-PEEKTEST00000000",
  "contentVersion": "$4",
  $id
  "localizedParameters": {
    "defaultLanguage": "en-US",
    "en-US": { "titleName": "$3" },
    "fr-FR": { "titleName": "$3 (FR)" }
  }
}
JSON
}
mkdir -p "$src/one"
app "$src/one/Peek App" PPSA01234 "Peek Test App" 01.002.000
mkdir -p "$src/one/Peek App/data"
head -c $((48 << 20)) /dev/urandom > "$src/one/Peek App/data/big.bin"
for i in $(seq 1 40); do echo "file $i" > "$src/one/Peek App/data/small-$i.txt"; done

(cd "$src/one" && zip -qr "$files/app.zip" "Peek App")
(cd "$src/one" && zip -qr0 "$files/app-stored.zip" "Peek App")
(cd "$src/one" && zip -qr -P secret "$files/encrypted.zip" "Peek App")
(cd "$src/one" && "$SEVENZ" a -bd -bso0 -bse0 -ms=off "$files/app.7z" "Peek App")
(cd "$src/one" && "$SEVENZ" a -bd -bso0 -bse0 -ms=on "$files/solid.7z" "Peek App")
(cd "$src/one" && tar -czf "$files/app.tar.gz" "Peek App")
# a param.json without a titleId: the ID comes from the folder's name
mkdir -p "$src/guess"
app "$src/guess/PPSA05555-app" "" "No ID" 01.000.000
(cd "$src/guess" && zip -qr "$files/guess.zip" "PPSA05555-app")
# two apps
mkdir -p "$src/two"
app "$src/two/First" PPSA00001 "First" 01.000.000
app "$src/two/Second" PPSA00002 "Second" 01.000.000
(cd "$src/two" && zip -qr "$files/two.zip" First Second)
# an app inside another app's folder: one app (as when unpacked, see ps5install_count)
mkdir -p "$src/nested"
app "$src/nested/Outer" PPSA00003 "Outer" 01.000.000
app "$src/nested/Outer/extras/Inner" PPSA00004 "Inner" 01.000.000
(cd "$src/nested" && zip -qr "$files/nested.zip" Outer)
# many files spread through the archive (a dump: no request per file),
# and ZIP64 (over 65535 entries), both made with Python's zipfile
python3 - "$src/one/Peek App" "$files/many.zip" "$files/zip64.zip" <<'PY'
import os, sys, zipfile
app, many, z64 = sys.argv[1:]
with zipfile.ZipFile(many, "w", zipfile.ZIP_DEFLATED) as z:
    for i in range(3000):
        z.writestr("Many/data/f%04d.bin" % i, os.urandom(16384))
        if i == 1500:
            z.write(os.path.join(app, "sce_sys/param.json"), "Many/sce_sys/param.json")
            z.write(os.path.join(app, "eboot.bin"), "Many/eboot.bin")
with zipfile.ZipFile(z64, "w", zipfile.ZIP_STORED) as z:
    z.write(os.path.join(app, "sce_sys/param.json"), "Big/sce_sys/param.json")
    z.write(os.path.join(app, "eboot.bin"), "Big/eboot.bin")
    for i in range(70000):
        z.writestr("Big/d/%05d" % i, b"")
PY
# an archive without an app
mkdir -p "$src/plain/Docs"
echo hello > "$src/plain/Docs/readme.txt"
(cd "$src/plain" && zip -qr "$files/plain.zip" Docs)
head -c 3000000 /dev/urandom > "$files/random.zip"
# RAR (no RAR writer here: libarchive's own test files)
if [ -f "$LA_TARBALL" ]; then
    mkdir -p "$work/uu"
    tar -xf "$LA_TARBALL" -C "$work/uu" --strip-components=3 libarchive-3.8.1/libarchive/test/test_read_format_rar5_multiple_files.rar.uu \
        libarchive-3.8.1/libarchive/test/test_read_format_rar_windows.rar.uu 2>/dev/null || true
    for f in "$work"/uu/*.uu; do
        [ -f "$f" ] || continue
        n="$(basename "$f" .uu)"
        (cd "$files" && uudecode -o "${n#test_read_format_}" "$f")
    done
fi
# an installed copy (version 01.001.000) for the "installed" check
mkdir -p "$work/user-app"
app "$work/user-app/PPSA01234" PPSA01234 "Peek Test App" 01.001.000
mv "$work/user-app/PPSA01234/sce_sys" "$work/user-app/PPSA01234/sce_sys.tmp" && mkdir -p "$work/user-app/PPSA01234/sce_sys" \
    && mv "$work/user-app/PPSA01234/sce_sys.tmp/param.json" "$work/user-app/PPSA01234/sce_sys/" && rm -rf "$work/user-app/PPSA01234/sce_sys.tmp" \
    "$work/user-app/PPSA01234/eboot.bin"

python3 "$here/serve.py" --port "$port" --root "$files" >"$work/server.log" 2>&1 &
server=$!
for _ in $(seq 1 50); do
    curl -s -o /dev/null "http://127.0.0.1:$port/page" && break
    sleep 0.1
done

size() { stat -f %z "$files/$1"; }
peek() { # <name> [options...]: peeks /real/<name>
    local name="$1"
    shift
    "$drive" --url "http://127.0.0.1:$port/real/$name" --size "$(size "${name%%\?*}")" --user-app "$work/user-app" "$@"
}
# check <description> <output> <python expression on j (the JSON), or ERR:<text>>
check() {
    local what="$1" out="$2" want="$3"
    if [[ "$want" == ERR:* ]]; then
        if [[ "$out" == "ERR "*"${want#ERR:}"* ]]; then pass "$what: $out"; else failed "$what: got $out"; fi
        return
    fi
    if [[ "$out" == ERR* ]]; then
        failed "$what: $out"
        return
    fi
    if python3 -c "import json,sys; j=json.loads(sys.argv[1]); sys.exit(0 if ($want) else 1)" "$out"; then
        pass "$what: $(python3 -c "import json,sys; j=json.loads(sys.argv[1]); print('%s, %s requests, %d KiB in %s ms' % (j.get('titleId') or j.get('refused') or j['format'], j['requests'], j['fetched']//1024, j['ms']))" "$out")"
    else
        failed "$what: $out"
    fi
}
small='j["fetched"] < 1048576'

check "ZIP (deflate): the app, read from a few requests" "$(peek app.zip)" \
    "j['format']=='ZIP' and j['titleId']=='PPSA01234' and j['name']=='Peek Test App' and j['version']=='01.002.000' and j['titles']==1 and j['titleDir']=='Peek App' and not j['guessed'] and $small and j['requests']<=4"
check "ZIP: installed 01.001.000 is seen" "$(peek app.zip)" "j['installed']=='01.001.000'"
check "ZIP: nothing installed (another user-app folder)" \
    "$("$drive" --url "http://127.0.0.1:$port/real/app.zip" --size "$(size app.zip)" --user-app "$work/none")" "j['installed'] is None"
check "ZIP with 3000 files spread over 48 MB: no request per file" "$(peek many.zip)" "j['titleId']=='PPSA01234' and j['entries']==3002 and j['requests']<=3"
check "ZIP64 (70002 entries)" "$(peek zip64.zip)" "j['titleId']=='PPSA01234' and j['entries']==70002 and j['requests']<=8"
check "ZIP stored (no compression)" "$(peek app-stored.zip)" "j['titleId']=='PPSA01234' and $small"
check "7-Zip, not solid" "$(peek app.7z)" "j['format']=='7-Zip' and j['titleId']=='PPSA01234' and j['titles']==1 and $small"
out="$(peek solid.7z)"
check "7-Zip, solid: the app is found (param.json if it fits)" "$out" "j['format']=='7-Zip' and j['titles']==1 and j['titleDir']=='Peek App'"
check "ZIP: a param.json without a titleId: the ID from the folder's name" "$(peek guess.zip)" "j['titleId']=='PPSA05555' and j['guessed']"
check "ZIP with two apps" "$(peek two.zip)" "j['titles']==2"
check "ZIP with an app inside an app" "$(peek nested.zip)" "j['titles']==1 and j['titleId']=='PPSA00003'"
check "ZIP without an app" "$(peek plain.zip)" "j['titles']==0 and j['titleId'] is None and j['entries']==2"
check "encrypted ZIP: refused" "$(peek encrypted.zip)" "j['refused'] and 'ncrypted' in j['refused']"
check "tar.gz: read through, over the limit" "$(peek app.tar.gz --max-bytes 4194304)" "ERR:More than"
check "not an archive" "$(peek random.zip)" "ERR:"
check "a server without ranges" "$(peek app.zip?noranges=1)" "ERR:answered 200"
check "a cookie the server wants, missing" "$(peek "app.zip?cookie=k=v")" "ERR:answered 403"
printf '127.0.0.1\tFALSE\t/\tFALSE\t0\tk\tv\n' > "$work/cookies.txt"
check "the cookie given" "$(peek "app.zip?cookie=k=v" --cookies "$work/cookies.txt")" "j['titleId']=='PPSA01234'"
check "too slow (rate=8 KiB/s, 1.5 s)" "$(peek "app.zip?rate=8" --timeout 1500)" "ERR:"
# a few bytes a second: every read gets something, so only the deadline stops it
t0=$(date +%s)
out="$(peek "app.zip?rate=0.02" --timeout 2000)"
took=$(($(date +%s) - t0))
if [[ "$out" == "ERR It took too long"* ]] && [ "$took" -le 5 ]; then pass "a server sending 20 bytes a second: given up after ${took} s"; else failed "a dribbling server: $out after ${took} s"; fi
for f in rar5_multiple_files.rar rar_windows.rar; do
    if [ -f "$files/$f" ]; then
        check "RAR: $f" "$(peek "$f")" "j['format'].startswith('RAR') and j['entries']>0 and j['titles']==0"
    fi
done
check "on the drive: ZIP" "$("$drive" --path "$files/app.zip" --user-app "$work/user-app")" "j['titleId']=='PPSA01234' and not j['remote'] and j['installed']=='01.001.000'"
check "on the drive: solid 7-Zip (param.json read through)" "$("$drive" --path "$files/solid.7z" --user-app "$work/user-app")" \
    "j['titleId']=='PPSA01234' and j['name']=='Peek Test App' and not j['guessed']"
check "on the drive: encrypted" "$("$drive" --path "$files/encrypted.zip" --user-app "$work/user-app")" "j['refused']"

echo "peek-test: $passes passed, $fails failed"
[ "$fails" = 0 ]
