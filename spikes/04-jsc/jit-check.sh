#!/usr/bin/env bash
# JIT checks for jsc on the console (WebKit patch 0030): correctness, JIT
# stress (checksums compared with an LLInt run), VM traps in JIT code, and
# the benchmark per tier. Uses the WPE build's jsc (it reads ICU data from
# /data/ps5webkit/share/icu, which run-shot.sh uploads).
#
#   spikes/04-jsc/jit-check.sh [build dir]        (default toolchain/wpe-webkit/build)
#   STEPS="bench" spikes/04-jsc/jit-check.sh      (any of: correct stress traps bench)
#
# One payload per run (stdin mode of run-on-ps5.sh). Take the console lock first.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
. "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
BLD="${1:-$ROOT/toolchain/wpe-webkit/build}"
OUT="$HERE/results/jit-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
prospero-strip -o "$OUT/jsc.elf" "$BLD/bin/jsc"
export JSC_ELF="$OUT/jsc.elf"
STEPS="${STEPS:-correct stress traps bench}"

run() { # name, opts, js
    local name=$1 opts=$2 js=$3
    echo "== $name ($opts)"
    JSC_OPTS="$opts" "$HERE/run-on-ps5.sh" "$HERE/js/$js" > "$OUT/$name.log" 2>&1 || true
    grep -E "CORRECTNESS|JIT-STRESS|^FAIL|tiers|WATCHDOG|terminated|BENCH|^  [a-z]+ +[0-9]|crash|Exception|signal" "$OUT/$name.log" | sed 's/^/   /' || true
}

for step in $STEPS; do
    case $step in
        correct)
            run correct-jit "" correctness.js
            run correct-llint "--useJIT=0" correctness.js ;;
        stress)
            run stress-jit "" jit-stress.js
            run stress-dfg "--useFTLJIT=0" jit-stress.js
            run stress-llint "--useJIT=0" jit-stress.js
            run stress-eager "--thresholdForJITAfterWarmUp=10 --thresholdForOptimizeAfterWarmUp=20 --thresholdForFTLOptimizeAfterWarmUp=50 --useConcurrentJIT=0" jit-stress.js
            for v in jit dfg eager; do
                if diff <(grep '^CHECK' "$OUT/stress-$v.log") <(grep '^CHECK' "$OUT/stress-llint.log") > /dev/null; then
                    echo "   checksums $v == llint: SAME ($(grep -c '^CHECK' "$OUT/stress-$v.log") tests)"
                else
                    echo "   checksums $v != llint: DIFFERENT"; diff <(grep '^CHECK' "$OUT/stress-$v.log") <(grep '^CHECK' "$OUT/stress-llint.log") | sed 's/^/     /'
                fi
            done ;;
        traps)
            run traps-signal "--watchdog=3000" jit-watchdog.js
            run traps-polling "--watchdog=3000 --usePollingTraps=1" jit-watchdog.js ;;
        bench)
            run bench-llint "--useJIT=0" bench.js
            run bench-baseline "--useDFGJIT=0" bench.js
            run bench-dfg "--useFTLJIT=0" bench.js
            run bench-ftl "" bench.js ;;
    esac
done
echo "logs: $OUT"
