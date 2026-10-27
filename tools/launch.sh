#!/usr/bin/env bash
# Start a registered title on the PS5 without pressing Play, through the
# launcher payload (tools/launcher). Sends the payload first if it isn't
# running (after every jailbreak).
#   tools/launch.sh PPSA99009 [arg]   stop our running title, start this one
#   tools/launch.sh -f PPSA99009      also stop a title that isn't ours
#   tools/launch.sh --status | --kill | --quit
# Only titles in PS5WK_OWN_TITLES are stopped without -f, so the owner's own
# games are never closed by accident.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/env.sh" >/dev/null
PORT=9031
OWN="${PS5WK_OWN_TITLES:-PPSA99006 PPSA99007 PPSA99009 PPSA99010 PPSA99011 PPSA99012}"

ask() { printf '%s\n' "$@" | socat -t 15 - "TCP:$PS5_HOST:$PORT" 2>/dev/null; }

ensure_launcher() {
    ask status >/dev/null && return 0
    local elf="$ROOT/tools/launcher/launcher.elf"
    [[ -f "$elf" ]] || make -s -C "$ROOT/tools/launcher" >&2
    echo "==> sending launcher payload" >&2
    socat -u "FILE:$elf" "TCP:$PS5_HOST:$PS5_PORT"
    for _ in $(seq 50); do
        ask status >/dev/null && return 0
        sleep 0.2
    done
    echo "launcher did not come up on $PS5_HOST:$PORT" >&2
    exit 1
}

force=0
case "${1:-}" in
    -f) force=1; shift ;;
    --status) ensure_launcher; ask status; exit ;;
    --kill) ensure_launcher; ask kill; exit ;;
    --quit) ask quit; exit ;;
    ""|-h|--help) sed -n '2,9p' "$0"; exit 1 ;;
esac
title="$1"; shift
ensure_launcher

status="$(ask status)"
read -r ok app running <<<"$status"
if [[ "$ok" == OK && "$app" != none ]]; then
    if [[ $force == 0 && " $OWN " != *" $running "* ]]; then
        echo "$running is running and isn't one of ours; use -f to close it" >&2
        exit 1
    fi
    reply="$(ask kill)"; echo "$reply"
    [[ "$reply" == OK* ]] || exit 1
fi
reply="$(ask "launch $title${1:+ $1}")"; echo "$reply"
[[ "$reply" == OK* ]]
