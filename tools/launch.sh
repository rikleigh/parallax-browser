#!/usr/bin/env bash
# Start, stop and register titles on the PS5 without touching the console,
# through the launcher payload (tools/launcher). Sends the payload first if it
# isn't running (after every jailbreak).
#   tools/launch.sh PPSA99009 [arg]      stop our running title, start this one
#   tools/launch.sh -f PPSA99009         also stop a title that isn't ours
#   tools/launch.sh --install <titledir> upload, register if new, launch
#   tools/launch.sh --register <ID> [dir on the console, default /data/homebrew/<ID>]
#   tools/launch.sh --uninstall <ID>   (spike titles only)
#   tools/launch.sh --status | --stop | --kill [-9] | --quit
# --stop (and stopping our browser title before a launch) first asks the
# title to wind down by itself (launcher "stop": media stopped, audio ports
# and sockets closed, then the title asks for its SIGKILL) and kills it only
# if it is still running after STOP_WAIT seconds (default 10).
# Any "ERR ... blocked ..." or missing reply: stop and look, don't retry.
# Only titles in PS5WK_OWN_TITLES are stopped without -f, so the owner's own
# games are never closed by accident.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/env.sh" >/dev/null
PORT=9031
OWN="${PS5WK_OWN_TITLES:-PPSA99020}"
FTP="ftp://$PS5_HOST:${PS5_FTP_PORT:-2121}"

ask() { printf '%s\n' "$@" | socat -t 20 - "TCP:$PS5_HOST:$PORT" 2>/dev/null; }

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

# Print the reply; succeed only on "OK ...".
run() { local r; r="$(ask "$*")"; echo "${r:-ERR no reply}"; [[ "$r" == OK* ]]; }

# Wind the browser title down, then make sure it is gone.
stop_title() {
    local r
    r="$(ask stop)"; echo "${r:-ERR no reply}"
    case "$r" in
        OK*) ;;
        ERR*blocked*|"") return 1 ;;              # the launcher is stuck: don't go on
        *) echo "(no wind-down; killing)"; run kill; return ;;
    esac
    for _ in $(seq $(( ${STOP_WAIT:-10} * 2 ))); do
        sleep 0.5
        read -r ok app _ <<<"$(ask status)"
        [[ "$ok" == OK && "$app" == none ]] && { echo "OK stopped (wound down)"; return 0; }
        [[ "$ok" != OK ]] && { echo "ERR status: $ok $app"; return 1; }
    done
    echo "still running after ${STOP_WAIT:-10} s: killing"
    run kill
}

force=0
case "${1:-}" in
    -f) force=1; shift ;;
    --status) ensure_launcher; run status; exit ;;
    --kill) ensure_launcher; run kill ${2:-}; exit ;;
    --stop) ensure_launcher; stop_title; exit ;;
    --register) ensure_launcher; run register "${2:?title id}" ${3:-}; exit ;;
    --uninstall) ensure_launcher; run uninstall "${2:?title id}"; exit ;;
    --quit) run quit; exit ;;
    --install)
        dir="${2:?usage: launch.sh --install <titledir>}"; set -- "$(basename "$dir")"
        "$ROOT/tools/upload-title.sh" "$dir" >/dev/null
        echo "uploaded $1"
        ensure_launcher
        if ! curl -s --max-time 5 "$FTP/user/app/$1/mount.lnk" >/dev/null; then
            run register "$1" || exit 1
        fi ;;
    ""|-h|--help) sed -n '2,11p' "$0"; exit 1 ;;
esac
title="$1"; shift
ensure_launcher

read -r ok app running <<<"$(ask status)"
if [[ "$ok" == OK && "$app" != none ]]; then
    if [[ $force == 0 && " $OWN " != *" $running "* ]]; then
        echo "$running is running and isn't one of ours; use -f to close it" >&2
        exit 1
    fi
    if [[ "$running" == PPSA99020 ]]; then stop_title || exit 1; else run kill || exit 1; fi
fi
run launch "$title" ${1:-}
