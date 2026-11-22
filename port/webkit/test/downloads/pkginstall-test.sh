#!/usr/bin/env bash
# The package installer's loopback server (apps/common/ps5pkginstall.c) on
# the Mac, with a stand-in for the console's installer:
#   port/webkit/test/downloads/pkginstall-test.sh
# curl plays the console: HEAD, byte ranges (suffix, open-ended, past the
# end), 8 parallel range requests over keep-alive, the .crc sidecar, a
# wrong token, other paths, the query the console adds, more than 16
# connections at once. The job ends once every byte was served and things
# are quiet; also when refused, cancelled, stalled (a reader that stops
# reading too; wrong-token requests don't hold it off), or the file changes.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
work="$(mktemp -d)"
pids=()
cleanup() { for p in "${pids[@]+"${pids[@]}"}"; do kill "$p" 2>/dev/null || true; done; rm -rf "$work"; }
trap cleanup EXIT
apps="$root/port/webkit/apps/common"
tool="$work/pkginstall-tool"
cc -std=c11 -O2 -Wall -Wextra -Werror -o "$tool" "$here/pkginstall/pkginstall-tool.c" "$apps/ps5pkginstall.c" "$apps/ps5install.c" -lpthread
fails=0
passes=0
pass() { echo "PASS $*"; passes=$((passes + 1)); }
failed() { echo "FAIL $*"; fails=$((fails + 1)); }

pkg="$work/UP9000-PPSA12345_00-TEST.pkg"
head -c 9000001 /dev/urandom >"$pkg"
size=$(stat -f %z "$pkg")

start() { # [refuse] [stall] [cancel-file]: starts the tool, waits for its URL
    rm -f "$work/url" "$work/out"
    "$tool" "$pkg" "$work/url" "${1:-0}" "${2:-20000}" "${3:-}" >"$work/out" 2>&1 &
    pids+=($!)
    tool_pid=$!
    for _ in $(seq 1 100); do [ -f "$work/url" ] && break; sleep 0.05; done
    url="$(cat "$work/url" 2>/dev/null || true)"
}
finish() { wait "$tool_pid" 2>/dev/null || true; result="$(grep '^RESULT' "$work/out" || true)"; }
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }

# -- a whole install, every way the console asks
start
if [[ $url =~ ^http://127\.0\.0\.1:[0-9]+/[0-9a-f]{16}/UP9000-PPSA12345_00-TEST\.pkg$ ]] && [ ${#url} -le 127 ]; then
    pass "the URL: $url (${#url} bytes)"
else
    failed "the URL: $url"
fi
hdr="$(curl -sI "$url")"
grep -qi "content-length: $size" <<<"$hdr" && grep -qi 'accept-ranges: bytes' <<<"$hdr" && grep -q '200' <<<"$hdr" \
    && pass "HEAD: 200, the length, Accept-Ranges" || failed "HEAD: $hdr"
grep -qi 'last-modified: Wed, 01 Jan 2025' <<<"$hdr" && pass "HEAD: the fixed Last-Modified" || failed "Last-Modified: $hdr"
got="$(curl -s -r 0-99 "$url" | cmp - <(head -c 100 "$pkg") && echo same)"
[ "$got" = same ] && pass "Range 0-99" || failed "Range 0-99"
hdr="$(curl -s -D - -o "$work/tail" -H 'Range: bytes=-1000' "$url")"
grep -q '206' <<<"$hdr" && grep -qi "content-range: bytes $((size - 1000))-$((size - 1))/$size" <<<"$hdr" && cmp -s "$work/tail" <(tail -c 1000 "$pkg") \
    && pass "Range -1000 (the last 1000 bytes): 206 and Content-Range" || failed "suffix range: $hdr"
[ "$(code -H "Range: bytes=$size-" "$url")" = 416 ] && pass "a range past the end: 416" || failed "past the end"
[ "$(code "${url%.pkg}.crc")" = 404 ] && pass "the .crc sidecar: 404" || failed ".crc"
bad="$(sed -E 's#/[0-9a-f]{16}/#/0000000000000000/#' <<<"$url")"
[ "$(code "$bad")" = 404 ] && pass "a wrong token: 404" || failed "wrong token"
base="${url%/*/*}"
for p in / /etc/passwd "/$(cut -d/ -f4 <<<"$url")/../../etc/passwd" "/$(cut -d/ -f4 <<<"$url")/"; do
    [ "$(code --path-as-is "$base$p")" = 404 ] && pass "path $p: 404" || failed "path $p"
done
[ "$(code -X POST "$url")" = 405 ] && pass "POST: 405" || failed "POST"
[ "$(code -r 0-9 "$url?product=0287&serverIpAddr=127.0.0.1&r=00000000")" = 206 ] && pass "the console's query is ignored" || failed "query"
# 8 parallel parts on one keep-alive connection each, covering the file
chunk=$(( (size + 7) / 8 ))
for i in 0 1 2 3 4 5 6 7; do
    a=$((i * chunk)); b=$(( (i + 1) * chunk - 1 )); [ $b -ge $size ] && b=$((size - 1))
    curl -s -r "$a-$b" -o "$work/part$i" "$url" "$url" -o /dev/null &
done
wait $(jobs -p | grep -v "$tool_pid") 2>/dev/null || true
cat "$work"/part{0..7} >"$work/joined"
cmp -s "$work/joined" "$pkg" && pass "8 parallel ranges (2 requests a connection) put back together" || failed "parallel ranges differ"
finish
grep -q "installer: $url (Test package)" "$work/out" && pass "the installer got the URL and the name" || failed "installer call: $(head -3 "$work/out")"
[[ $result == "RESULT 0 "*"served=$size/$size"* ]] && pass "done once every byte was served: $result" || failed "end: $result"
grep -q 'every byte served' "$work/out" && pass "the log says so" || failed "log: $(tail -3 "$work/out")"

# -- more than 16 connections at once: 503 for the rest
start
for i in $(seq 1 20); do
    (exec 3<>"/dev/tcp/127.0.0.1/$(sed -E 's#.*:([0-9]+)/.*#\1#' <<<"$url")"; sleep 2) &
done
sleep 0.5
[ "$(code "$url" -o /dev/null)" = 503 ] && pass "a 17th connection: 503" || failed "17th connection"
sleep 2
[ "$(code -r 0-0 "$url")" = 206 ] && pass "served again once they closed" || failed "after the flood"
curl -s -o /dev/null "$url"
finish
[[ $result == "RESULT 0 "* ]] && pass "and finished: $result" || failed "flood end: $result"

# -- refused by the installer
start 80A3000F
finish
[[ $result == "RESULT -1 code=80A3000F"*"different game"* ]] && pass "refused: $result" || failed "refused: $result"

# -- cancelled
rm -f "$work/cancel"
start 0 20000 "$work/cancel"
curl -s -r 0-999 -o /dev/null "$url"
touch "$work/cancel"
finish
[[ $result == "RESULT -3 "*"Cancelled"* ]] && pass "cancelled: $result" || failed "cancel: $result"
[ "$(code --max-time 2 "$url")" = 000 ] && pass "nothing listens after the end" || failed "still listening"

# -- stalled: half fetched, then nothing
start 0 1500
curl -s -r 0-999 -o /dev/null "$url"
finish
[[ $result == "RESULT -2 "*"stopped copying"* ]] && pass "stalled: $result" || failed "stall: $result"

# -- a console that stops reading mid-send: the stall limit still ends it
# (sends don't block: the server polls and gives up when stopping)
big="$work/UP9000-PPSA12345_00-BIG.pkg"
head -c 67108864 /dev/zero >"$big"
saved="$pkg"; pkg="$big"
start 0 1500
port="$(sed -E 's#.*:([0-9]+)/.*#\1#' <<<"$url")"
python3 -I -c '
import socket, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
s.sendall(("GET " + sys.argv[2] + " HTTP/1.1\r\nHost: x\r\n\r\n").encode())
time.sleep(12)  # never reads
' "$port" "/${url#http://*/}" &
reader=$!
t0=$SECONDS
finish
kill $reader 2>/dev/null || true; wait $reader 2>/dev/null || true
[[ $result == "RESULT -2 "*"stopped copying"* ]] && [ $((SECONDS - t0)) -lt 10 ] \
    && pass "a reader that stops: stalled out in $((SECONDS - t0)) s: $result" || failed "stuck reader: $result after $((SECONDS - t0)) s"
pkg="$saved"; rm -f "$big"

# -- requests without the token aren't the console: they don't hold off the stall
start 0 1500
bad="$(sed -E 's#/[0-9a-f]{16}/#/0000000000000000/#' <<<"$url")"
curl -s -r 0-999 -o /dev/null "$url"
( for _ in $(seq 1 16); do curl -s -o /dev/null --max-time 1 "$bad"; sleep 0.25; done ) &
poker=$!
finish
kill $poker 2>/dev/null || true; wait $poker 2>/dev/null || true
[[ $result == "RESULT -2 "*"stopped copying"* ]] && pass "wrong-token requests don't count as activity: $result" || failed "strangers: $result"

# -- the file changes under it
start
curl -s -r 0-9 -o /dev/null "$url"
touch -t 202001010000 "$pkg"
curl -s -r 10-19 -o /dev/null "$url" || true
finish
[[ $result == "RESULT -2 "*"changed"* ]] && pass "the file changed: $result" || failed "changed: $result"

sdk="$root/toolchain/ps5-payload-sdk"
if [ -x "$sdk/bin/prospero-clang" ] && [ -f "$root/env.sh" ]; then
    if (. "$root/env.sh" >/dev/null 2>&1; "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -c "$apps/ps5pkginstall.c" \
        -o "$work/p.o") 2>"$work/ps5cc.log"; then
        pass "ps5pkginstall.c compiles for the PS5 (no warnings)"
    else
        cat "$work/ps5cc.log"; failed "ps5pkginstall.c doesn't compile for the PS5"
    fi
fi
echo "$passes passed, $fails failed"
[ $fails = 0 ]
