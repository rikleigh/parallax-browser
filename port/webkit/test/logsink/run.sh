#!/usr/bin/env bash
# The title's log queue (apps/title/titlelog.c) on the Mac: does a logging
# call ever wait while the log server stops reading or goes away?
#   port/webkit/test/logsink/run.sh [outdir]
# Builds logsink-test with the Mac's clang and runs three cases against
# listener.pl (port 9971, 8 KB receive buffer):
#   direct  the old blocking path; the listener is stopped (SIGSTOP) for 10 s
#   queue   titlelog.c, same stop; TCP buffer 16 KB (log-sink file)
#   restart titlelog.c; the listener is killed and started again 5 s later
#   reset   titlelog.c with stall=4: the stop outlasts it, the connection is
#           reset and a new one made (into the stopped listener's backlog)
# Prints each run's slowest call per second and checks the listener's copy:
# every thread's lines in order, a gap only where a "[log] ... dropped" note
# says so.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
OUT="${1:-$(mktemp -d)}"
mkdir -p "$OUT"
PORT=9971
clang -O2 -Wall -I"$ROOT/spikes" -I"$ROOT/port/webkit/apps/title" "$HERE/logsink-test.c" "$ROOT/port/webkit/apps/title/titlelog.c" \
    -o "$OUT/logsink-test" -lpthread
tick() { perl -e 'select undef,undef,undef,$ARGV[0]' "$1"; }

run_case() { # name mode action
    local name=$1 mode=$2 action=$3
    rm -f "$OUT/log-sink"
    [[ $mode == queue ]] && echo "127.0.0.1:$PORT queue=16 stall=$([[ $name == reset ]] && echo 4 || echo 30)" > "$OUT/log-sink"
    perl "$HERE/listener.pl" -r 8192 $PORT > "$OUT/$name.listener" & local lpid=$!
    tick 0.5
    "$OUT/logsink-test" "$mode" $PORT 20 "$OUT" 3> "$OUT/$name.report" & local tpid=$!
    tick 4
    if [[ $action == stop ]]; then
        kill -STOP $lpid; tick 10; kill -CONT $lpid
    else
        kill $lpid; wait $lpid 2>/dev/null || true; tick 5
        perl "$HERE/listener.pl" -r 8192 $PORT >> "$OUT/$name.listener" & lpid=$!
    fi
    wait $tpid || true
    tick 1
    kill $lpid 2>/dev/null || true; wait $lpid 2>/dev/null || true
    echo "== $name ($mode; at t=4 s the listener is $([[ $action == stop ]] && echo "stopped for 10 s" || echo "killed, back 5 s later"))"
    cat "$OUT/$name.report"
    python3 -I - "$OUT/$name.listener" "$OUT/logsink-test.log" <<'PY'
import re, sys
def check_file(path):
    last, bad, lines = {}, 0, 0
    for line in open(path, errors="replace"):
        m = re.match(r"(\[app msg\] t\d|\[wpe-ps5\] stderr|\[web7\]) seq (\d+) padding", line)
        if m:
            lines += 1
            key, seq = m.group(1), int(m.group(2))
            bad += key in last and seq != last[key] + 1
            last[key] = seq
    print(f"   file log: {lines} lines, {bad} gaps or out of order")
check_file(sys.argv[2])
last, gaps, bad, notes, lines = {}, 0, 0, [], 0
pending_note = False
for raw in open(sys.argv[1], errors="replace"):
    line = raw.split(" ", 1)[1].rstrip("\n") if " " in raw else raw
    if line.startswith("[log]"):
        notes.append(raw.rstrip()); pending_note = pending_note or "dropped" in line; continue
    m = re.match(r"(\[app msg\] t\d|\[wpe-ps5\] stderr|\[web7\]) seq (\d+) padding", line)
    if not m:
        continue
    lines += 1
    key, seq = m.group(1), int(m.group(2))
    if key in last and seq != last[key] + 1:
        if seq <= last[key]:
            bad += 1
        elif pending_note:
            gaps += 1
        else:
            bad += 1
            print("   unannounced gap:", key, last[key], "->", seq)
    last[key] = seq
print(f"   listener: {lines} lines; {gaps} gaps after a drop note; {bad} out of order or unannounced")
for n in notes:
    print("   " + n)
PY
}
run_case direct direct stop
run_case queue queue stop
run_case restart queue restart
run_case reset queue stop
echo "(files in $OUT)"
