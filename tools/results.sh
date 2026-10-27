#!/usr/bin/env bash
# Collect RESULT lines from saved spike logs as a Markdown table, newest
# result per (spike, test) winning:
#   tools/results.sh [logs/*.log]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
shopt -s nullglob
files=("$@")
[[ ${#files[@]} -gt 0 ]] || files=("$ROOT"/logs/*.log)
[[ ${#files[@]} -gt 0 ]] || { echo "no logs yet" >&2; exit 0; }
echo "| Spike | Test | Result | Detail |"
echo "| --- | --- | --- | --- |"
grep -h '^RESULT ' "${files[@]}" | awk '
  { key = $2 " " $3; status[key] = $4; d = ""; for (i = 5; i <= NF; i++) d = d (i > 5 ? " " : "") $i;
    detail[key] = d; if (!(key in seen)) { seen[key] = 1; order[n++] = key } }
  END { for (i = 0; i < n; i++) { k = order[i]; split(k, p, " ");
        gsub(/\|/, "\\|", detail[k]); printf "| %s | %s | %s | %s |\n", p[1], p[2], status[k], detail[k] } }'
