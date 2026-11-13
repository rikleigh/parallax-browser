#!/usr/bin/env bash
# Run WebGPU test pages in the browser on the console and collect their
# RESULT lines.
#   [TITLE=PPSA99208] port/webkit/test/webgpu/run-console.sh [page ...]     (default: the five headless pages)
# Needs: the pages uploaded to /data/ps5webkit/webgpu/ (README.md), the
# browser running with WebGPU on (Settings, or /data/ps5webkit/webgpu-on),
# and tools/logserver.sh listening (page console lines reach it through the
# title's log). Each page gets WAIT seconds (default 60) to print its summary.
# QUERY is added to each page's URL (ml-transformers: QUERY="models=http://<mac>:8765/").
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
pages=("$@"); [[ ${#pages[@]} -gt 0 ]] || pages=(adapter device compute offscreen-readback errors)
log="$(ls -t "$ROOT"/logs/title-*.log | head -1)"
# tools/ps5open starts the installed browser (PPSA99207) when none is running:
# after a crash of a test title that would run the pages in the wrong build.
TITLE="${TITLE:-PPSA99207}"
running() { "$ROOT/tools/launch.sh" --status 2>/dev/null | grep -q "$TITLE"; }
for page in "${pages[@]}"; do
    page="${page%.html}"
    running || { echo "RESULT webgpu-$page summary FAIL $TITLE is not running (it ended during the page before, or was never started)"; exit 1; }
    from=$(($(wc -l < "$log") + 1))
    "$ROOT/tools/ps5open" "file:///data/ps5webkit/webgpu/$page.html?run=$(date +%s)${QUERY:+&$QUERY}" >/dev/null
    for _ in $(seq "${WAIT:-60}"); do
        sleep 1
        [[ $(tail -n +"$from" "$log" | grep -a -c "RESULT webgpu-$page summary") -gt 0 ]] && break
    done
    # The title's log has two sinks on some set-ups: each line once.
    tail -n +"$from" "$log" | grep -a -o "RESULT webgpu-$page .*" | awk '!seen[$0]++' || true
    tail -n +"$from" "$log" | grep -a -E "\[crash|\[abort\]|device lost|MESSAGE_CHECK" | awk '!seen[$0]++' | head -20 || true
    [[ $(tail -n +"$from" "$log" | grep -a -c "RESULT webgpu-$page summary") -gt 0 ]] || echo "RESULT webgpu-$page summary FAIL no summary line in ${WAIT:-60} s"
done
