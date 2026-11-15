#!/usr/bin/env bash
# Spike 13 stage 1 runs: jsc in the title (PPSA99017, build-jsc-title.sh)
# against the same build's payload jsc (Spike 4's run-on-ps5.sh, stdin mode).
#
#   spikes/13-title-jit/run.sh                 every step
#   STEPS="wasm wasm-load" spikes/13-title-jit/run.sh
# Steps: install correct stress traps bench wasm wasm-load wasm-pin, and Spike 14 stage 0: s0a s0b s0c
# Results: spikes/13-title-jit/results/<time>/<run>.log and a summary.
# Uploads js/ and Spike 4's js/ to /data/jsc. Uses the ICU data the browser
# keeps in /data/ps5webkit/share/icu. Stops nothing but PPSA99017.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
FTP="ftp://$PS5_HOST:${PS5_FTP_PORT:-2121}"
TITLE=PPSA99017
APP="$HERE/build/titles/$TITLE"
PAYLOAD="$HERE/build/jsc-payload.elf"
OUT="$HERE/results/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
STEPS="${STEPS:-install correct stress traps bench wasm wasm-load wasm-pin}"
SUMMARY="$OUT/summary.txt"
# The WebProcess's JSC settings come from jscenv.h in both the title and the
# payload jsc (environment, not options: see jscenv.h), plus env.txt lines.
WP_OPTS=""

say() { echo "$*" | tee -a "$SUMMARY"; }
put() { curl -s --ftp-create-dirs -T "$1" "$FTP$2"; }
putText() { local f="$OUT/.put"; printf '%s\n' "$2" > "$f"; put "$f" "$1"; }
log() { ls -t "$ROOT"/logs/title-*.log | head -1; }

title_running() { "$ROOT/tools/launch.sh" --status 2>/dev/null | grep -q "$TITLE"; }

# title_run <name> <args> [cpu] [load "<threads> <seconds>"] [side "cpu:duty ..."]
title_run() {
    local name=$1 args=$2 cpu=${3:-} load=${4:-0} side=${5:-}
    while title_running; do sleep 1; done
    putText /data/jsc/side.txt "$side"
    putText /data/jsc/args.txt "$args"
    putText /data/jsc/cpu.txt "$cpu"
    putText /data/jsc/load.txt "$load"
    local f; f=$(log)
    local from=$(( $(wc -l < "$f") + 1 ))
    "$ROOT/tools/launch.sh" "$TITLE" > "$OUT/$name.launch" 2>&1 || { say "$name: launch failed: $(tail -1 "$OUT/$name.launch")"; return 1; }
    local t0=$SECONDS
    until tail -n +"$from" "$f" | grep -q '^\[jsc-title\] done'; do
        if (( SECONDS - t0 > ${RUN_TIMEOUT:-300} )); then say "$name: no '[jsc-title] done' after ${RUN_TIMEOUT:-300} s"; break; fi
        if (( SECONDS - t0 > 10 )) && ! title_running; then sleep 2; tail -n +"$from" "$f" | grep -q '^\[jsc-title\] done' || { say "$name: title ended without '[jsc-title] done'"; break; }; fi
        sleep 1
    done
    tail -n +"$from" "$f" > "$OUT/$name.log"
}

# payload_run <name> <opts> <js> [cpu] [side "cpu:duty ..."]
payload_run() {
    local name=$1 opts=$2 js=$3
    putText /data/jsc/payload-cpu.txt "${4:-}"
    putText /data/jsc/payload-side.txt "${5:-}"
    # PS5Main.cpp takes argv from /data/jsc/args.txt when it exists (the
    # title's file): without it the payload runs stdin with JSC_OPTS.
    curl -s "$FTP/" -Q "*DELE /data/jsc/args.txt" > /dev/null || true
    JSC_ELF="$PAYLOAD" JSC_OPTS="${WP_OPTS:+$WP_OPTS }$opts" "$ROOT/spikes/04-jsc/run-on-ps5.sh" "$js" > "$OUT/$name.log" 2>&1 || true
}

show() { # name pattern
    grep -E "$2" "$OUT/$1.log" | sed "s/^/   $1: /" | tee -a "$SUMMARY" || say "   $1: (no match for $2)"
}
checkline() { grep -E "$2" "$OUT/$1.log" | head -1 | sed -E 's/.*(CHECK|check:)//' ; }

# A title kept running beside payload runs: load "<threads> <seconds>".
bg_title_start() {
    putText /data/jsc/args.txt none; putText /data/jsc/side.txt ""; putText /data/jsc/cpu.txt ""
    putText /data/jsc/load.txt "$1"
    while title_running; do sleep 1; done
    local f; f=$(log); local from=$(( $(wc -l < "$f") + 1 ))
    "$ROOT/tools/launch.sh" "$TITLE" > "$OUT/bg.launch" 2>&1
    local t0=$SECONDS; until tail -n +"$from" "$f" | grep -q 'staying'; do (( SECONDS - t0 > 30 )) && break; sleep 0.5; done
    BG_FROM=$from
}
bg_title_stop() { # name
    "$ROOT/tools/launch.sh" --killtitle "$TITLE" > /dev/null 2>&1 || true
    while title_running; do sleep 1; done
    tail -n +"$BG_FROM" "$(log)" | grep -E 'jsc-title' > "$OUT/$1.log" || true
}
rate() { grep -hE 'WASM-BIG RESULT' "$OUT/$1.log" | sed -E 's/.*last10 mean ([0-9.]+).*/\1/' | head -1; }
row() { # name what
    local r; r=$(rate "$1")
    say "   $1: ${r:-?} Msteps/s  ($2) $(grep -hE 'check:' "$OUT/$1.log" | sed -E 's/.*check: ([0-9a-f-]+).*/\1/' | head -1)"
    grep -hE '(jsc-payload|jsc-title)\] (side [0-9]|shell thread pinned)' "$OUT/$1.log" | sed "s/^/      /" | tee -a "$SUMMARY" || true
}
WASM="$ROOT/spikes/13-title-jit/js/wasm-big.js"

for step in $STEPS; do
    say "== $step"
    case $step in
        install)
            put "$ROOT/spikes/13-title-jit/js/wasm-big.js" /data/jsc/wasm-big.js
            for js in correctness.js jit-stress.js jit-watchdog.js bench.js; do put "$ROOT/spikes/04-jsc/js/$js" "/data/jsc/$js"; done
            putText /data/jsc/env.txt "# JSC_name=value lines"
            putText /data/jsc/args.txt none
            putText /data/jsc/load.txt 0
            while title_running; do sleep 1; done
            "$ROOT/tools/launch.sh" --install "$APP" > "$OUT/install.log" 2>&1 || true
            tail -2 "$OUT/install.log" | tee -a "$SUMMARY"
            # --install launches it: with no args file it would run the old one; wait for it.
            sleep 3; while title_running; do sleep 1; done ;;
        correct)
            title_run correct-title-jit "/data/jsc/correctness.js"
            show correct-title-jit 'CORRECTNESS|executable mmap|exit|FAIL'
            title_run correct-title-llint "--useJIT=0 /data/jsc/correctness.js"
            show correct-title-llint 'CORRECTNESS|exit' ;;
        stress)
            for v in "jit:" "dfg:--useFTLJIT=0" "llint:--useJIT=0" "eager:--thresholdForJITAfterWarmUp=10 --thresholdForOptimizeAfterWarmUp=20 --thresholdForFTLOptimizeAfterWarmUp=50 --useConcurrentJIT=0"; do
                title_run "stress-title-${v%%:*}" "${v#*:} /data/jsc/jit-stress.js"
                show "stress-title-${v%%:*}" 'JIT-STRESS|tiers|exit'
            done
            for v in jit dfg eager; do
                if diff <(grep '^CHECK' "$OUT/stress-title-$v.log") <(grep '^CHECK' "$OUT/stress-title-llint.log") > /dev/null &&
                   [[ -n $(grep '^CHECK' "$OUT/stress-title-$v.log") ]]; then
                    say "   checksums $v == llint: SAME ($(grep -c '^CHECK' "$OUT/stress-title-$v.log") tests)"
                else say "   checksums $v != llint: DIFFERENT or missing"; fi
            done ;;
        traps)
            title_run traps-title "--watchdog=3000 /data/jsc/jit-watchdog.js"
            show traps-title 'WATCHDOG|terminated|exit' ;;
        bench)
            title_run bench-title "/data/jsc/bench.js"
            show bench-title 'BENCH|exit'
            payload_run bench-payload "" "$ROOT/spikes/04-jsc/js/bench.js"
            show bench-payload 'BENCH' ;;
        wasm)
            title_run wasm-title "/data/jsc/wasm-big.js"
            show wasm-title 'WASM-BIG(:| check| RESULT)|exit'
            payload_run wasm-payload "" "$ROOT/spikes/13-title-jit/js/wasm-big.js"
            show wasm-payload 'WASM-BIG(:| check| RESULT)' ;;
        wasm-load)
            # A title keeping its CPUs busy; the payload runs meanwhile.
            LOADN=${LOADN:-12} # pinned to CPUs 1-12; CPU 0 left to main/log (jsctitle.c)
            putText /data/jsc/args.txt none
            putText /data/jsc/cpu.txt ""
            putText /data/jsc/load.txt "$LOADN 60"
            f=$(log); from=$(( $(wc -l < "$f") + 1 ))
            "$ROOT/tools/launch.sh" "$TITLE" > "$OUT/load.launch" 2>&1
            t0=$SECONDS; until tail -n +"$from" "$f" | grep -q 'load: [0-9]* busy'; do (( SECONDS - t0 > 30 )) && break; sleep 0.5; done
            payload_run wasm-payload-busy "" "$ROOT/spikes/13-title-jit/js/wasm-big.js"
            show wasm-payload-busy 'WASM-BIG( check| RESULT)'
            while title_running; do sleep 1; done
            tail -n +"$from" "$f" > "$OUT/load-title.log"
            show load-title 'load:'
            # jsc in the title with the same load beside it.
            title_run wasm-title-busy "/data/jsc/wasm-big.js" "" "$((LOADN - 1)) 60"
            show wasm-title-busy 'WASM-BIG( check| RESULT)|load:|exit' ;;
        s0a)
            # Spike 14 0a: the payload jsc on CPU 14, the fight's other threads as side threads.
            payload_run a0-none "" "$WASM" 14; row a0-none "payload on 14, no title"
            bg_title_start "0 600"
            payload_run a1-idle "" "$WASM" 14; row a1-idle "payload on 14, idle title"
            payload_run a2-idle-15h "" "$WASM" 14 "15:50"; row a2-idle-15h "idle title, side 15 at 50%"
            payload_run a3-idle-15f "" "$WASM" 14 "15:100"; row a3-idle-15f "idle title, side 15 at 100%"
            payload_run a4-idle-fight "" "$WASM" 14 "15:50 13:50"; row a4-idle-fight "idle title, side 15 and 13 at 50%"
            bg_title_stop bg-idle
            bg_title_start "12 600"
            payload_run a5-busy-fight "" "$WASM" 14 "15:50 13:50"; row a5-busy-fight "busy title (12 threads), side 15 and 13 at 50%"
            bg_title_stop bg-busy
            show bg-busy 'beat t=(5|30|60) ' ;;
        s0b)
            # Spike 14 0b: the same in the title. CPU pairs from the title's APIC map.
            title_run b1-cpu3 "/data/jsc/wasm-big.js" 3; row b1-cpu3 "title, jsc on 3"
            map=$(grep -h 'cpu:apic' "$OUT/b1-cpu3.log" | head -1 | sed 's/.*cpu:apic//'); say "   APIC map (cpu:apic):$map"
            sib=""; others=()
            a3=$(tr ' ' '\n' <<<"$map" | awk -F: '$1==3{print $2}')
            for e in $map; do c=${e%%:*}; a=${e##*:}; [[ $c == 3 ]] && continue
                if (( a / 2 == a3 / 2 )); then sib=$c; elif (( c != 0 )); then others+=("$c:$a"); fi; done
            # two other cores, not siblings of each other
            o1=${others[0]%%:*}; oa=${others[0]##*:}; o2=""
            for e in "${others[@]:1}"; do (( ${e##*:} / 2 != oa / 2 )) && { o2=${e%%:*}; break; }; done
            say "   jsc on 3, sibling $sib, other cores $o1 $o2"
            title_run b2-sib-full "/data/jsc/wasm-big.js" 3 0 "$sib:100"; row b2-sib-full "title, jsc on 3, sibling $sib at 100%"
            title_run b3-sib-fight "/data/jsc/wasm-big.js" 3 0 "$sib:50 $o1:50"; row b3-sib-fight "title, jsc on 3, side $sib and $o1 at 50% (payload-like placement)"
            title_run b4-apart-fight "/data/jsc/wasm-big.js" 3 0 "$o1:50 $o2:50"; row b4-apart-fight "title, jsc on 3, side $o1 and $o2 at 50% (other cores)"
            title_run b5-any-fight "/data/jsc/wasm-big.js" "" 0 "-:50 -:50"; row b5-any-fight "title, all unpinned, two sides at 50%" ;;
        s0c)
            # Spike 14 0c: a second title while one runs.
            T2=${T2:-PPSA99018}
            "$ROOT/tools/launch.sh" --install "$HERE/build/titles/$T2" > "$OUT/c-install.log" 2>&1 || true
            tail -2 "$OUT/c-install.log" | tee -a "$SUMMARY"
            sleep 3; "$ROOT/tools/launch.sh" --killtitle "$T2" >/dev/null 2>&1 || true
            bg_title_start "1 150"
            say "   first: $("$ROOT/tools/launch.sh" --apps 2>&1 | tail -1)"
            putText /data/jsc/load.txt "1 90"
            say "   launch-also $T2: $("$ROOT/tools/launch.sh" --also "$T2" 2>&1 | tail -1)"
            for i in 1 2 3 4 5 6 7 8 9 10 11 12; do sleep 5; say "   t=$((i*5)) s: status $("$ROOT/tools/launch.sh" --status 2>&1 | tail -1); apps $("$ROOT/tools/launch.sh" --apps 2>&1 | tail -1)"; done
            "$ROOT/tools/launch.sh" --killtitle "$T2" 2>&1 | tail -1 | tee -a "$SUMMARY"
            bg_title_stop c-titles
            grep -E 'beat|staying|title PPSA|main mask|cpu:apic' "$OUT/c-titles.log" | sed 's/^/   /' | tee -a "$SUMMARY" ;;
        wasm-pin)
            title_run wasm-title-cpu3 "/data/jsc/wasm-big.js" 3
            show wasm-title-cpu3 'WASM-BIG( check| RESULT)|pinned|exit' ;;
    esac
done
echo "results: $OUT"
