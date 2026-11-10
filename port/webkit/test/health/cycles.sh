#!/usr/bin/env bash
# Console health over repeated title runs (CONSOLE-HEALTH.md).
#   port/webkit/test/health/cycles.sh [N] [dwell s] [url]
# Each cycle: a process list (test/ps/ps.elf through elfldr: payload.elf
# count and any left over), the newest klog VM Stats line if a klog capture
# is running (KLOG=<file>), launch PPSA99207 (start page unless url), wait
# <dwell> s, stop it, and the run's numbers from the title log (LOG=<file>,
# the tools/logserver.sh output). Hold console.lock while it runs.
# One line per cycle to stdout and $OUT (default /tmp/ps5wk-health.tsv).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$ROOT/env.sh" >/dev/null
N=${1:-5}; DWELL=${2:-25}; URL=${3:-}
LOG=${LOG:?LOG=<title log file (tools/logserver.sh output)>}
OUT=${OUT:-/tmp/ps5wk-health.tsv}
KLOG=${KLOG:-}
# built from ps.c when it is newer (ps.elf is not in git)
PS_ELF="$ROOT/port/webkit/test/ps/ps.elf"
PS_SRC="$ROOT/port/webkit/test/ps/ps.c"
if [[ ! -f $PS_ELF || $PS_SRC -nt $PS_ELF ]]; then
    prospero-clang -O2 -o "$PS_ELF" "$PS_SRC" || { echo "cannot build $PS_SRC" >&2; exit 1; }
fi

snapshot() {
    local ps n left
    ps=$({ cat "$PS_ELF"; echo; } | socat -t 10 - "TCP:$PS5_HOST:${PS5_PORT:-9021}" 2>/dev/null)
    n=$(echo "$ps" | awk '/^pid / && $12=="payload.elf" && $11>3' | wc -l | tr -d ' ')
    # payload.elf processes but this ps (pid:state:vsz MB:age s; the launcher is the ~18 MB one)
    left=$(echo "$ps" | awk '/^pid / && $12=="payload.elf" && $11>3 {printf "%s:%s:%sMB:%ss ", $2, $4, $7, $11}')
    echo "payloads=$n [$left]"
    echo "$ps" >> "${OUT%.tsv}-ps.txt"
}

vmstats() {
    [[ -n $KLOG && -f $KLOG ]] || { echo "vm=-"; return; }
    grep "VM Stats" "$KLOG" | tail -1 | sed -E 's/.*RSS ([0-9.]+).*swap out ([0-9.]+), page table CPU ([0-9]+)\/8192 GPU ([0-9]+).*/rss=\1 swap=\2 pt=\3 gpt=\4/'
}

for c in $(seq 1 "$N"); do
    before=$(vmstats); snap=$(snapshot)
    off=$(stat -f %z "$LOG")
    t0=$(date +%s)
    "$ROOT/tools/launch.sh" PPSA99207 ${URL:+"$URL"} >/dev/null 2>&1 || echo "cycle $c: launch failed" >&2
    end=$((t0 + DWELL)); until [[ $(date +%s) -ge $end ]]; do perl -e 'select undef,undef,undef,0.5'; done
    "$ROOT/tools/launch.sh" --stop >/dev/null 2>&1 || echo "cycle $c: stop failed" >&2
    perl -e 'select undef,undef,undef,2'
    run=$(python3 -I "$HERE/parse-run.py" "$LOG" "$off")
    line="$(date +%H:%M:%S) cycle=$c $before $snap $run"
    echo "$line" | tee -a "$OUT"
done
