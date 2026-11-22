#!/usr/bin/env bash
# The download engine (apps/common/ps5fetch.c) on the Mac, against serve.py:
#   port/webkit/test/downloads/fetch-test.sh [--quick]
# Needs libsoup 3 (brew install libsoup). Every case downloads a generated
# file and checks its SHA-256 (or how it failed); --quick skips the slow
# ones (throttled speeds, stalls). Also compiles ps5fetch.c for the PS5 when
# the toolchain is there.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
quick="${1:-}"
work="$(mktemp -d)"
server=""
cleanup() {
    [ -z "$server" ] || kill "$server" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

if ! pkg-config --exists libsoup-3.0; then
    echo "FAIL libsoup 3 isn't installed (brew install libsoup)"
    exit 1
fi
fails=0
passes=0
pass() { echo "PASS $*"; passes=$((passes + 1)); }
failed() { echo "FAIL $*"; fails=$((fails + 1)); }

# warning-free with -Wall -Wextra
if cc -std=c11 -O2 -Wall -Wextra -Werror -o "$work/fetch-drive" "$here/fetch/fetch-drive.c" "$root/port/webkit/apps/common/ps5fetch.c" \
    $(pkg-config --cflags --libs libsoup-3.0) 2>"$work/cc.log"; then
    pass "builds without warnings (-Wall -Wextra)"
else
    cat "$work/cc.log"
    failed "doesn't build without warnings"
    exit 1
fi

sdk="$root/toolchain/ps5-payload-sdk"
# the SDK's clang wrapper runs the LLVM that llvm-config names (README: llvm@18)
[ -n "${LLVM_CONFIG:-}" ] || [ ! -x /opt/homebrew/opt/llvm@18/bin/llvm-config ] || export LLVM_CONFIG=/opt/homebrew/opt/llvm@18/bin/llvm-config
sysroot="$root/toolchain/wpe-sysroot"
if [ -x "$sdk/bin/prospero-clang" ] && [ -d "$sysroot/include/libsoup-3.0" ]; then
    if "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -c -o "$work/ps5fetch-ps5.o" \
        -isystem "$sysroot/include/glib-2.0" -isystem "$sysroot/lib/glib-2.0/include" -isystem "$sysroot/include/libsoup-3.0" \
        -isystem "$sysroot/include" "$root/port/webkit/apps/common/ps5fetch.c" 2>"$work/ps5cc.log"; then
        pass "compiles for the PS5 without warnings"
    else
        cat "$work/ps5cc.log"
        failed "doesn't compile for the PS5"
    fi
else
    echo "SKIP no PS5 toolchain or sysroot"
fi

port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
python3 "$here/serve.py" --port "$port" >"$work/serve.log" 2>&1 &
server=$!
base="http://127.0.0.1:$port"
for _ in $(seq 50); do
    curl -sf "$base/stats" >/dev/null 2>&1 && break
    sleep 0.1
done

drive() { "$work/fetch-drive" "$@" 2>>"$work/drive.log"; }
field() { # field name "result line"
    local v="${2#*" $1="}"
    [ "$v" != "$2" ] || v="${2#"$1="}"
    [ "$1" = error ] && echo "$v" || echo "${v%% *}"
}
sha_of() { shasum -a 256 "$1" 2>/dev/null | cut -d' ' -f1; }
want_sha() { python3 "$here/serve.py" --sha "$1" --version "${2:-1}"; }
stat_of() { # path key: the server's count
    curl -s "$base/stats" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get(sys.argv[1],{}).get(sys.argv[2],0))' "$1" "$2" || echo 0
}
# expect_done name result file size [version]
expect_done() {
    local name="$1" r="$2" file="$3" size="$4" version="${5:-1}"
    if [ "$(field state "$r")" = done ] && [ "$(sha_of "$file")" = "$(want_sha "$size" "$version")" ] \
        && [ ! -e "$file.part" ] && [ ! -e "$file.part.ps5dl" ]; then
        pass "$name: $(field secs "$r") s, ranges=$(field ranges "$r") restarted=$(field restarted "$r")"
        return 0
    fi
    failed "$name: $r (sha $(sha_of "$file"))"
    return 1
}
mib=1048576

# 1-2. the whole file, 1 and 8 connections
for n in 1 8; do
    f="$work/whole$n.bin"
    r=$(drive --conns $n --chunk $mib --ua ParallaxTest/1.0 --referer "$base/page" "$base/file/33554432/whole$n.bin" "$f")
    expect_done "whole file, $n connection(s)" "$r" "$f" 33554432 || true
done
if grep -q "whole8.bin.*UA=ParallaxTest/1.0" "$work/serve.log"; then pass "User-Agent sent"; else failed "no User-Agent in the server's log"; fi

# 3. no ranges: one stream, even with 8 asked; and a dropped one starts over
f="$work/noranges.bin"
r=$(drive --conns 8 --chunk $mib "$base/file/20971520/noranges.bin?noranges=1" "$f")
expect_done "no ranges: one stream" "$r" "$f" 20971520 && {
    [ "$(field ranges "$r")" = 0 ] && [ "$(field maxconns "$r")" -le 1 ] && pass "no ranges: ranges=0, at most 1 connection" \
        || failed "no ranges: $r"
}
f="$work/noranges-drop.bin"
r=$(drive --conns 8 --stall 5 "$base/file/20971520/noranges-drop.bin?noranges=1&drop=7340032" "$f")
expect_done "no ranges, dropped at 7 MB: asked again" "$r" "$f" 20971520 || true
f="$work/nolen.bin"
r=$(drive --conns 4 "$base/file/5000000/nolen.bin?noranges=1&nolen=1" "$f")
expect_done "no ranges and no Content-Length" "$r" "$f" 5000000 || true
f="$work/empty.bin"
r=$(drive --conns 4 "$base/file/0/empty.bin" "$f")
expect_done "a 0-byte file (416 to the probe)" "$r" "$f" 0 || true

# 4. throttled per connection: 1, 4, 8 connections
if [ "$quick" != --quick ]; then
    for n in 1 4 8; do
        f="$work/thr$n.bin"
        r=$(drive --conns $n --chunk $mib "$base/file/16777216/thr$n.bin?rate=2048" "$f")
        expect_done "throttled to 2 MiB/s per connection, $n connection(s)" "$r" "$f" 16777216 || true
        eval "speed$n=$(field speed "$r")"
    done
    echo "     speeds: 1: $((speed1 / 1024)) KiB/s, 4: $((speed4 / 1024)) KiB/s, 8: $((speed8 / 1024)) KiB/s ($(python3 -c "print('%.1fx, %.1fx' % ($speed4 / $speed1, $speed8 / $speed1))"))"
    if [ "$speed4" -ge $((speed1 * 3)) ]; then pass "4 connections >= 3x 1 connection"; else failed "4 connections < 3x 1"; fi
    if [ "$speed8" -ge "$speed4" ]; then pass "8 connections >= 4"; else failed "8 connections slower than 4"; fi
fi

# 5. stop, then start again: continues, nothing fetched twice
f="$work/resume.bin"
u="$base/file/33554432/resume.bin?rate=2048"
r=$(drive --conns 4 --chunk $mib --stop-at 12000000 "$u" "$f")
s=$(drive --saved "$f")
if [ "$(field state "$r")" = paused ] && [ -e "$f.part" ] && [ -e "$f.part.ps5dl" ] && [ "$(field saved "$s")" = 1 ] \
    && [ "$(field received "$s")" = "$(field received "$r")" ] && [ "$(field total "$s")" = 33554432 ]; then
    pass "stopped: .part and .ps5dl kept, $(field received "$s") bytes saved"
else
    failed "stop: $r / $s"
fi
saved=$(field received "$s")
before=$(stat_of /file/33554432/resume.bin bytes)
r=$(drive --conns 4 --chunk $mib "$u" "$f")
after=$(stat_of /file/33554432/resume.bin bytes)
expect_done "started again" "$r" "$f" 33554432 && {
    sent=$((after - before))
    if [ "$sent" = $((33554432 - saved)) ] && [ "$(field restarted "$r")" = 0 ]; then
        pass "started again: the server sent $sent bytes, exactly the $((33554432 - saved)) missing"
    else
        failed "started again: the server sent $sent bytes, $((33554432 - saved)) were missing ($r)"
    fi
}

# 6. the file changed between stop and start: starts over
f="$work/changed.bin"
u="$base/file/16777216/changed.bin?rate=2048"
r=$(drive --conns 4 --chunk $mib --stop-at 6000000 "$u" "$f")
curl -s "$base/version?v=2" >/dev/null
r=$(drive --conns 4 --chunk $mib "$u" "$f")
curl -s "$base/version?v=1" >/dev/null
expect_done "changed while paused" "$r" "$f" 16777216 2 && {
    [ "$(field restarted "$r")" = 1 ] && pass "changed while paused: restarted=1" || failed "changed while paused: restarted=0"
}
# ... and while running: fails (retryable), the next start starts over
f="$work/midchange.bin"
u="$base/file/16777216/midchange.bin?changeafter=3&rate=4096"
r=$(drive --conns 4 --chunk $mib "$u" "$f")
if [ "$(field state "$r")" = failed ] && [ "$(field retryable "$r")" = 1 ] && [[ "$r" == *"changed on the server"* ]]; then
    pass "changed while running: failed, retryable ($(field error "$r"))"
else
    failed "changed while running: $r"
fi
r=$(drive --conns 4 --chunk $mib "$u" "$f")
expect_done "changed while running, started again" "$r" "$f" 16777216 2 && {
    [ "$(field restarted "$r")" = 1 ] && pass "changed while running, started again: restarted=1" || failed "restarted=0"
}

# 7. connections dropped in the middle of chunks
f="$work/drops.bin"
r=$(drive --conns 8 --chunk $mib "$base/file/33554432/drops.bin?drop=3000000,9000001,20000000,30000003&rate=8192" "$f")
expect_done "4 connections dropped mid-chunk" "$r" "$f" 33554432 || true
f="$work/drops1.bin"
r=$(drive --conns 1 --chunk 4194304 "$base/file/12582912/drops1.bin?drop=1000000,5000000" "$f")
expect_done "dropped twice, 1 connection" "$r" "$f" 12582912 || true

# 8. stalls
if [ "$quick" != --quick ]; then
    f="$work/stall1.bin"
    r=$(drive --conns 4 --chunk $mib --stall 3 "$base/file/16777216/stall1.bin?stall=5000000&rate=4096" "$f")
    expect_done "stalled once (stall_s 3): asked again" "$r" "$f" 16777216 || true
    f="$work/stallall.bin"
    r=$(drive --conns 1 --chunk $mib --stall 3 "$base/file/8388608/stallall.bin?stallall=3000000" "$f")
    secs=$(field secs "$r")
    if [ "$(field state "$r")" = failed ] && [ "$(field retryable "$r")" = 1 ] && [ -e "$f.part" ] && [ -e "$f.part.ps5dl" ] \
        && python3 -c "import sys; sys.exit(not 5 <= float(sys.argv[1]) <= 12)" "$secs"; then
        pass "stalls every time (stall_s 3): failed after $secs s, retryable ($(field error "$r"))"
    else
        failed "stalls every time: $r"
    fi
    f="$work/stall8.bin"
    r=$(drive --conns 8 --chunk $mib --stall 3 --timeout 60 "$base/file/16777216/stall8.bin?stallall=9000000&rate=4096" "$f")
    if [ "$(field state "$r")" = failed ] && [ "$(field retryable "$r")" = 1 ] && [ "$(field received "$r")" -ge 15000000 ]; then
        pass "8 connections, one range stalls every time: the rest came ($(field received "$r") bytes), then failed after $(field secs "$r") s"
    else
        failed "8 connections, one range stalls: $r"
    fi
fi

# 9. at most 2 connections at once (429 / 503 beyond)
for code in 429 503; do
    f="$work/busy$code.bin"
    p="/file/33554432/busy$code.bin"
    r=$(drive --conns 8 --chunk $mib "$base$p?maxconn=2&busy=$code&rate=4096" "$f")
    expect_done "$code beyond 2 connections, 8 asked" "$r" "$f" 33554432 && {
        busy=$(stat_of "$p" busy)
        late=$(field lateconns "$r")
        if [ "$late" -le 2 ] && [ "$busy" -le 16 ]; then
            pass "$code: lowered to $late connection(s) in the second half, $busy refusals in all ($(field secs "$r") s)"
        else
            failed "$code: $late connections in the second half, $busy refusals"
        fi
    }
done
f="$work/busy1.bin"
r=$(drive --conns 1 --stall 4 "$base/file/4194304/busy1.bin?maxconn=0&busy=503" "$f")
if [ "$(field state "$r")" = failed ] && [ "$(field retryable "$r")" = 1 ] && [ "$(field status "$r")" = 503 ]; then
    pass "503 every time: failed after $(field secs "$r") s, retryable ($(field error "$r"))"
else
    failed "503 every time: $r"
fi

# 9b. more connections (max_connections) and the end taken over
if [ "$quick" != --quick ]; then
    # 1 MiB/s each, 12 MiB/s for all (a host's limit for one address): 8,
    # then 12 (faster), then 18 (no faster): back to 12
    f="$work/grow.bin"
    r=$(drive --conns 8 --max-conns 32 --chunk $mib --timeout 90 "$base/file/268435456/grow.bin?rate=1024&totalrate=12288" "$f")
    expect_done "1 MiB/s each, 12 MiB/s in all, 8 growing to 32" "$r" "$f" 268435456 && {
        if [ "$(field allowed "$r")" = 12 ] && [ "$(field maxconns "$r")" -gt 12 ]; then
            pass "grew to $(field maxconns "$r"), went back to 12 when that was no faster ($(($(field speed "$r") / 1048576)) MiB/s)"
        else
            failed "growing to the host's limit: allowed=$(field allowed "$r") maxconns=$(field maxconns "$r"), not 12 after more"
        fi
    }
    # no limit per connection, 8 MiB/s in all (the line): more don't help
    f="$work/line.bin"
    r=$(drive --conns 8 --max-conns 32 --chunk $mib --timeout 90 "$base/file/167772160/line.bin?totalrate=8192" "$f")
    expect_done "8 MiB/s in all (the line), 8 growing to 32" "$r" "$f" 167772160 && {
        if [ "$(field allowed "$r")" = 8 ] && [ "$(field maxconns "$r")" -gt 8 ]; then
            pass "tried $(field maxconns "$r"), stayed at 8 (the line was full)"
        else
            failed "the line: allowed=$(field allowed "$r") maxconns=$(field maxconns "$r"), not 8 after a try"
        fi
    }
    # 403 beyond 10 at once: back to 8, the download goes on
    f="$work/grow403.bin"
    p="/file/134217728/grow403.bin"
    r=$(drive --conns 8 --max-conns 32 --chunk $mib --timeout 90 "$base$p?rate=1024&maxconn=10&busy=403" "$f")
    expect_done "403 beyond 10 connections, 8 growing to 32" "$r" "$f" 134217728 && {
        if [ "$(field allowed "$r")" = 8 ] && [ "$(stat_of "$p" busy)" -ge 1 ]; then
            pass "403 to an added connection: back to 8, $(stat_of "$p" busy) refusal(s)"
        else
            failed "403 while growing: allowed=$(field allowed "$r"), $(stat_of "$p" busy) refusals"
        fi
    }
    # a signed address that expires while it grows (403 to all of them):
    # asked again, not taken as a host refusing that many
    f="$work/growexp.bin"
    p="/release/201326592/growexp.bin"
    r=$(drive --conns 8 --max-conns 32 --chunk $mib --timeout 90 "$base$p?rate=1024&ttl=9" "$f")
    expect_done "a signed address expiring while growing" "$r" "$f" 201326592 && {
        if [ "$(field allowed "$r")" -gt 8 ] && [ "$(stat_of "$p" requests)" -ge 2 ]; then
            pass "expired while growing: asked again ($(stat_of "$p" requests) times), allowed=$(field allowed "$r")"
        else
            failed "expired while growing: allowed=$(field allowed "$r"), the link asked $(stat_of "$p" requests) times"
        fi
    }
    # 5 chunks of 8 MiB on 4 connections at 1 MiB/s each: without taking
    # over, the fifth comes alone (about 16 s); taken over, about 10 s
    f="$work/end.bin"
    p="/file/41943040/end.bin"
    r=$(drive --conns 4 --chunk 8388608 --timeout 60 "$base$p?rate=1024" "$f")
    expect_done "the end taken over (5 chunks, 4 connections)" "$r" "$f" 41943040 && {
        secs=$(field secs "$r")
        sent=$(stat_of "$p" bytes)
        if python3 -c "import sys; sys.exit(not float(sys.argv[1]) < 13.5)" "$secs" && [ "$sent" -le $((41943040 + 4 * 1048576)) ]; then
            pass "the end: $secs s (one connection alone would be ~16 s), $sent bytes sent for 41943040"
        else
            failed "the end: $secs s, $sent bytes sent for 41943040"
        fi
    }
fi

# 10. a signed final address that expires
f="$work/release.bin"
r=$(drive --conns 4 --chunk $mib "$base/release/33554432/release.bin?ttl=2&rate=2048" "$f")
asked=$(stat_of /release/33554432/release.bin requests)
expect_done "release link, signed address valid 2 s" "$r" "$f" 33554432 && {
    [ "$asked" -ge 2 ] && [[ "$(field final "$r")" == *"/signed/"* ]] \
        && pass "signed address expired: the release link was asked $asked times" || failed "release link asked $asked times ($r)"
}
f="$work/expired.bin"
r=$(drive --conns 4 --chunk $mib --orig "$base/release/8388608/expired.bin" "$base/signed/8388608/expired.bin?exp=1" "$f")
expect_done "an expired final address given first: the original is asked" "$r" "$f" 8388608 || true
f="$work/gone.bin"
r=$(drive --conns 4 --orig "$base/signed/8388608/gone.bin?exp=2" "$base/signed/8388608/gone.bin?exp=1" "$f")
if [ "$(field state "$r")" = failed ] && [ "$(field retryable "$r")" = 0 ] && [ "$(field status "$r")" = 403 ]; then
    pass "the original refused too: failed, not retryable ($(field error "$r"))"
else
    failed "the original refused too: $r"
fi

# 11. cookies
printf '# Netscape HTTP Cookie File\n127.0.0.1\tFALSE\t/\tFALSE\t0\tother\tx\n#HttpOnly_127.0.0.1\tFALSE\t/\tFALSE\t%s\tsess\tabc123\n' \
    $(($(date +%s) + 3600)) >"$work/cookies.txt"
printf '127.0.0.1\tFALSE\t/\tFALSE\t1000\tsess\tabc123\n' >"$work/expired-cookies.txt"
f="$work/cookie.bin"
r=$(drive --conns 4 --chunk $mib --cookies "$work/cookies.txt" "$base/file/8388608/cookie.bin?cookie=sess=abc123" "$f")
expect_done "cookie required, given" "$r" "$f" 8388608 || true
f="$work/nocookie.bin"
for c in none expired; do
    if [ $c = none ]; then
        r=$(drive --conns 4 "$base/file/8388608/nocookie.bin?cookie=sess=abc123" "$f")
    else
        r=$(drive --conns 4 --cookies "$work/expired-cookies.txt" "$base/file/8388608/nocookie.bin?cookie=sess=abc123" "$f")
    fi
    if [ "$(field state "$r")" = failed ] && [ "$(field retryable "$r")" = 0 ] && [ "$(field status "$r")" = 403 ] && [ ! -e "$f.part" ]; then
        pass "cookie required, $c: 403, not retryable ($(field error "$r"))"
    else
        failed "cookie required, $c: $r"
    fi
done

# a server whose answer claims an absurd size (2^62 bytes): refused at
# once, not cut into chunks (that took the engine's lock for good)
f="$work/huge.bin"
t0=$(date +%s)
r=$(drive --conns 4 "$base/file/8388608/huge.bin?fakesize=4611686018427387904" "$f")
if [ "$(field state "$r")" = failed ] && [[ "$(field error "$r")" == *"impossible size"* ]] && [ $(($(date +%s) - t0)) -lt 10 ] && [ ! -e "$f.part" ]; then
    pass "an impossible size: refused ($(field error "$r"))"
else
    failed "an impossible size: $r"
fi

# 12. cancel
f="$work/cancel.bin"
r=$(drive --conns 4 --chunk $mib --cancel-at 4000000 "$base/file/33554432/cancel.bin?rate=1024" "$f")
if [ "$(field state "$r")" = cancelled ] && [ ! -e "$f.part" ] && [ ! -e "$f.part.ps5dl" ] && [ ! -e "$f" ]; then
    pass "cancelled: .part and .ps5dl gone ($(field secs "$r") s)"
else
    failed "cancel: $r"
fi
f="$work/cancel2.bin"
r=$(drive --conns 4 --chunk $mib --stop-at 4000000 "$base/file/33554432/cancel2.bin?rate=1024" "$f")
r=$(drive --conns 4 --chunk $mib --cancel-at 6000000 "$base/file/33554432/cancel2.bin?rate=1024" "$f")
if [ "$(field state "$r")" = cancelled ] && [ ! -e "$f.part" ] && [ ! -e "$f.part.ps5dl" ]; then
    pass "paused, started, cancelled: files gone"
else
    failed "paused, started, cancelled: $r"
fi

# 13. not enough free space: fails before writing anything
f="$work/huge.bin"
r=$(drive --conns 4 "$base/file/8796093022208/huge.bin" "$f")
if [ "$(field state "$r")" = failed ] && [ "$(field retryable "$r")" = 0 ] && [[ "$r" == *"free space"* ]] && [ ! -e "$f.part" ] \
    && [ ! -e "$f.part.ps5dl" ] && [ "$(field total "$r")" = 8796093022208 ]; then
    pass "8 TiB: refused before writing ($(field error "$r"))"
else
    failed "8 TiB: $r"
fi

# 14. past 4 GiB: a resume at 5 GiB of a 6 GiB file (a sparse .part)
f="$work/big.bin"
big=6442450944
five=5368709120
python3 - "$f" "$five" "$big" <<'PY'
import sys
path, five, big = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
with open(path + ".part", "wb") as out:
    out.truncate(five)
with open(path + ".part.ps5dl", "w") as out:
    out.write('[ps5dl]\nversion=1\nurl=x\nfinal_url=x\ntotal=%d\netag="v1-%d"\nlast_modified=\nchunk=1048576\nranges=true\ndone=0-%d\n'
              % (big, big, five))
PY
r=$(drive --conns 8 --chunk $mib --stop-at $((five + 40 * mib)) "$base/file/$big/big.bin" "$f")
s=$(drive --saved "$f")
if [ "$(field state "$r")" = paused ] && [ "$(field restarted "$r")" = 0 ] && [ "$(field total "$s")" = $big ] \
    && [ "$(field received "$s")" -ge $((five + 40 * mib)) ] && [ "$(field received "$s")" = "$(field received "$r")" ]; then
    pass "6 GiB, resumed at 5 GiB: $(($(field received "$s") - five)) bytes past 5 GiB, state round trip"
else
    failed "6 GiB resume: $r / $s"
fi
if python3 - "$f" <<'PY'; then pass "6 GiB: the bytes past 5 GiB are the right ones (every 4 KiB block's number checked)"; else failed "6 GiB: wrong bytes"; fi
import struct, sys
path = sys.argv[1]
done = dict(l.split("=", 1) for l in open(path + ".part.ps5dl").read().splitlines() if "=" in l)["done"]
spans = [tuple(map(int, s.split("-"))) for s in done.split(",")]
checked = 0
with open(path + ".part", "rb") as f:
    for a, b in spans:
        if b <= 5368709120:
            continue
        a = max(a, 5368709120)
        for block in range((a + 4095) // 4096, b // 4096):
            f.seek(block * 4096)
            if struct.unpack(">QQ", f.read(16)) != (block, 1):
                print("block", block, "wrong")
                sys.exit(1)
            checked += 1
sys.exit(checked < 8192)
PY

# 15. https against a public host (skipped offline)
if [ "$quick" != --quick ] && curl -sfI --max-time 5 https://proof.ovh.net/files/10Mb.dat >/dev/null 2>&1; then
    f="$work/ovh.bin"
    r=$(drive --conns 4 --chunk $mib --timeout 120 https://proof.ovh.net/files/10Mb.dat "$f")
    curl -sf --max-time 120 -o "$work/ovh-curl.bin" https://proof.ovh.net/files/10Mb.dat || true
    if [ "$(field state "$r")" = done ] && [ "$(sha_of "$f")" = "$(sha_of "$work/ovh-curl.bin")" ]; then
        pass "https (proof.ovh.net, 10 MB): ranges=$(field ranges "$r"), $(field secs "$r") s, same as curl"
    else
        failed "https: $r"
    fi
else
    echo "SKIP https (no network, or --quick)"
fi

echo "fetch-test: $passes passed, $fails failed"
[ "$fails" = 0 ] || { echo "the server's log: $work/serve.log (deleted on exit); the engine's progress: drive.log"; exit 1; }
