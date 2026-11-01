#!/usr/bin/env bash
# HSTS in the payload harness (ps5wk-shot, a fresh data directory): checks
# that a Strict-Transport-Security policy is stored, survives a restart, is
# not rewritten for every response (port/deps/patches/libsoup/0001), and
# that http:// is upgraded to https:// from the stored policy.
#
#   port/webkit/test/net/hsts-test.sh [host]
#
# The host must send Strict-Transport-Security over https and answer plain
# http without a redirect of its own (default hsts.badssl.com, a TLS test
# site that does both), so an https load after an http request can only come
# from the stored policy. Steps, each a new ps5wk-shot (a new NetworkProcess):
#   1. https://<host>/: the policy is stored
#   2. the database (sqlite3 on the Mac; WebKit keeps it in the network
#      session's cache directory): host, max-age, expiry, includeSubDomains
#   3. https://<host>/ again: the row is not rewritten (same expiry)
#   4. http://<host>/: must commit as https://<host>/
#   control: http://<host>/ with another fresh data directory stays http
# Take the console lock first. Uses ../run-shot.sh (SHOT_REMOTE=<path> starts
# ps5wk-shot from the console's disk).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
FTP="ftp://$PS5_HOST:${PS5_FTP_PORT:-2121}"
HOST="${1:-hsts.badssl.com}"
OUT="${SHOT_OUT:-$ROOT/toolchain/wpe-webkit/shots-net/hsts-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"
export SHOT_OUT="$OUT"
DATA=/data/ps5webkit/hsts-test/data-$RANDOM
CONTROL=/data/ps5webkit/hsts-test/control-$RANDOM
DB="$DATA/cache/hsts-storage.sqlite"
uploaded=
shot() { # <url> <data dir> [extra args]
    local url=$1 data=$2; shift 2
    sleep 15 # elfldr refuses loopback connections right after a payload
    SKIP_UPLOAD=$uploaded SHOT_ARGS="settle=3 data=$data cache=$data/cache $*" "$ROOT/port/webkit/test/run-shot.sh" "$url" 60 > "$OUT/last.out" 2>&1 || true
    uploaded=1
    local log; log="$(ls -t "$OUT"/2*.log | head -1)"
    grep -E "^load (started|redirected|committed|finished)|^RESULT ps5wk-shot load" "$log" | sed 's/^/   /'
    LOG="$log"
}
fetchdb() { # <name>
    rm -f "$OUT/$1.sqlite"
    curl -s "$FTP$DB" -o "$OUT/$1.sqlite" || true
    if [[ -s $OUT/$1.sqlite ]]; then
        sqlite3 "$OUT/$1.sqlite" "SELECT host, max_age, expiry, include_subdomains FROM soup_hsts_policies;" | sed "s/^/   $1: /"
    else
        echo "   $1: no database at $DB"
    fi
}
echo "== 1. https://$HOST/"
shot "https://$HOST/" "$DATA"
echo "== 2. stored"
fetchdb first
sleep 2
echo "== 3. https://$HOST/ again, a new NetworkProcess"
shot "https://$HOST/" "$DATA"
fetchdb second
if [[ -s $OUT/first.sqlite && -s $OUT/second.sqlite ]]; then
    a="$(sqlite3 "$OUT/first.sqlite" "SELECT expiry FROM soup_hsts_policies WHERE host='$HOST';")"
    b="$(sqlite3 "$OUT/second.sqlite" "SELECT expiry FROM soup_hsts_policies WHERE host='$HOST';")"
    [[ -n $a && $a == "$b" ]] && echo "RESULT hsts not-rewritten PASS expiry $a kept" || echo "RESULT hsts not-rewritten FAIL expiry $a -> $b"
fi
echo "== 4. http://$HOST/ after a restart"
shot "http://$HOST/" "$DATA"
if grep -q "^load committed at .*: https://$HOST/" "$LOG"; then
    echo "RESULT hsts upgrade PASS http://$HOST/ committed as https"
else
    echo "RESULT hsts upgrade FAIL (see $LOG)"
fi
echo "== control: http://$HOST/ with no stored policy"
shot "http://$HOST/" "$CONTROL"
if grep -q "^load committed at .*: http://$HOST/" "$LOG"; then
    echo "RESULT hsts control PASS stays http without a policy"
else
    echo "RESULT hsts control FAIL (see $LOG)"
fi
for d in "$DATA" "$CONTROL"; do
    curl -s "$FTP/" -Q "DELE $d/cache/hsts-storage.sqlite" >/dev/null 2>&1 || true
done
echo "out: $OUT"
