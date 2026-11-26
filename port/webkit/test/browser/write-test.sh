#!/usr/bin/env bash
# The disk write test of a development build (apps/common/writetest.c; the
# title's POST /debug/write-test), driven from the Mac through the link
# server. For the limit on the title's own writes (README, "The title's own
# file writes are held back", which has what these gave on 2026-11-26): about
# 240 MB/s for the first 1.3 GB a title process writes, then down to about
# 2 MB/s, while payloads keep 240 MB/s.
#
#   write-test.sh run [-n] key=value ...   one test; -n: in the title that is
#                                          running (default: a fresh title
#                                          first, so no test inherits another's
#                                          written bytes)
#   write-test.sh matrix [group ...]       the experiments below, in order
#                                          (groups: base where chunk sync fd
#                                          thread alloc recover probe; all of
#                                          them take about an hour)
#   write-test.sh lines                    the last test's lines again
#
# Keys: writetest.c's (dir mb chunk report direct osync sync syncmb fadvise
# pwrite reopen prealloc overwrite pace passes mb2 between sleep keep rctl uid
# tag),
# and proc=title|payload, thread=new|main, probe=1.
# Each test deletes its file (ps5wk-write-test*.bin in dir) unless keep=1.
# Needs the browser running, PS5_HOST and PS5_PIN (env.local.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
URL="http://$PS5_HOST:9040"
PIN=(--data-urlencode "pin=${PS5_PIN:-}")

up() { curl -s -m 5 -G "$URL/debug/write-test" "${PIN[@]}" 2>/dev/null | grep -q '"ok":true'; }

fresh() {
    echo "== a fresh title"
    "$ROOT/tools/launch.sh" PPSA99207 | tail -1
    for _ in $(seq 90); do up && { sleep 5; return 0; }; sleep 1; done
    echo "the browser's link server did not come back" >&2; exit 1
}

lines() {
    curl -s -m 15 -G "$URL/debug/write-test" "${PIN[@]}" | python3 -I -c '
import json, sys
d = json.load(sys.stdin)
for line in d["lines"]:
    print(line)
print("(still running)" if d["running"] else "(ended)")'
}

run() {
    local restart=1 args=()
    [[ ${1:-} == -n ]] && { restart=0; shift; }
    (( restart )) && fresh
    for a in "$@"; do args+=(--data-urlencode "$a"); done
    echo "== $*"
    curl -s -m 15 -X POST "$URL/debug/write-test" "${PIN[@]}" ${args[@]+"${args[@]}"}
    # A slowed 3 GB test takes about 8 minutes; the longest here well under 40.
    for _ in $(seq 1200); do
        sleep 2
        curl -s -m 10 -G "$URL/debug/write-test" "${PIN[@]}" 2>/dev/null | grep -q '"running":false' && break
    done
    lines
}

# The pid of the title that is running, from the Mac's log (for rctl=).
titlepid() { grep -a 'ps5browser: WPE WebKit' "$(ls -t "$ROOT"/logs/title-*.log | head -1)" | tail -1 | sed -n 's/.*pid \([0-9]*\).*/\1/p'; }

matrix() {
    local groups=("$@")
    (( ${#groups[@]} )) || groups=(base where chunk sync fd thread alloc recover probe)
    for g in "${groups[@]}"; do
        echo "==== $g"
        case "$g" in
        base)    # Below WebKit? The title against a payload, the same file system.
            run mb=3072
            run proc=payload mb=3072
            run -n proc=payload mb=64 "rctl=$(titlepid)"          # the kernel's accounting of the title, if it has any
            run -n proc=payload mb=3072 uid=1                     # a payload with the title's uid (its "start" line has it)
            ;;
        where)   # The file system, and whether bytes that reach no disk count.
            for dir in /data /user/data /download0 /mnt/usb0; do run mb=1792 "dir=$dir"; done   # /download0 holds about 200 MB
            run dir=null mb=8192
            run -n mb=512                                         # after 8 GB to /dev/null in the same process
            ;;
        chunk)
            run mb=2304 chunk=1024
            run mb=1280 chunk=64                                  # each call counts as about 64 KB more: slow from 0.65 GB,
            run mb=128 chunk=4 report=4                           # and from 75 MB (19,000 calls); 1 MB/s and less after
            run mb=1792 chunk=1024 pace=50                        # about 18 MB/s: the amount written, or the rate?
            run -n proc=payload mb=256 chunk=4 report=16          # a payload's small writes: as slow per call, never held back
            ;;
        sync)
            run mb=2304 sync=fdatasync
            run mb=2304 sync=fsync
            run mb=2304 sync=fsync syncmb=512
            run mb=2304 direct=1
            run mb=2304 direct=1 chunk=1024
            run mb=1536 osync=1 chunk=1024
            run proc=payload mb=3072 sync=fdatasync fadvise=1     # fadvise: a payload's only
            ;;
        fd)
            run mb=2304 pwrite=1
            run mb=2304 reopen=256
            run mb=1536 keep=1
            run -n mb=192 tag=second                              # a second file once the first is slow: per file?
            run -n mb=192 overwrite=1                             # over existing blocks; removes the file keep=1 left
            ;;
        thread)  # The main loop stops for this one (about 30 s if it slows as a thread does).
            run mb=1792 thread=main
            run proc=payload mb=3072 thread=new
            ;;
        alloc)   # New blocks or any write: over a file a payload wrote.
            run mb=2304 prealloc=1
            run proc=payload mb=2304 keep=1
            run mb=2304 overwrite=1
            ;;
        recover) # What brings the speed back in the same process.
            run mb=1536 passes=2 mb2=192 between=read
            run mb=1536 passes=2 mb2=192 between=delete
            run mb=1536 passes=2 mb2=192 between=sync
            run mb=1536 passes=2 mb2=1024 sleep=120
            ;;
        probe)   # The browser's own small saves while a thread is held back, and while a payload writes.
            run mb=2304 probe=1
            run proc=payload mb=3072 probe=1
            ;;
        *) echo "no group $g" >&2; exit 1 ;;
        esac
    done
    fresh # the last test's title has written its share: leave a new one
}

case "${1:-}" in
    run) shift; run "$@" ;;
    matrix) shift; matrix "$@" ;;
    lines) lines ;;
    *) sed -n '2,25p' "$0"; exit 1 ;;
esac
