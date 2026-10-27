#!/usr/bin/env bash
# Upload the clips a job list names (if not already on the console), install
# the job list as /data/ps5webkit/hwdec/jobs.txt and launch the title.
# Job paths in the list are relative to media/; they become
# /data/ps5webkit/hwdec/<file> on the console.
#   spikes/09-hw-decode/run.sh jobs/<list>.txt [--payload] [--no-launch]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
. "$ROOT/env.sh" >/dev/null
list=${1:?usage: run.sh jobs/<list>.txt [--payload] [--no-launch]}; shift
FTP="ftp://$PS5_HOST:${PS5_FTP_PORT:-2121}"
REMOTE=/data/ps5webkit/hwdec
listing=$(curl -s --max-time 10 "$FTP$REMOTE/" || true)
tmp=$(mktemp)
while read -r mode kind file rest; do
    [[ -z $mode || $mode == \#* ]] && continue
    local_file="$HERE/media/$file"
    [[ -f $local_file ]] || { echo "missing $local_file (make-clips.sh / fetch-sample.sh)" >&2; exit 1; }
    size=$(stat -f %z "$local_file")
    if ! grep -qE " $size [A-Z][a-z]{2} .* $file\$" <<<"$listing"; then
        echo "uploading $file ($((size >> 20)) MB)"
        curl -sS --ftp-create-dirs -T "$local_file" "$FTP$REMOTE/$file"
    fi
    echo "$mode $kind $REMOTE/$file $rest" >> "$tmp"
done < "$list"
curl -sS --ftp-create-dirs -T "$tmp" "$FTP$REMOTE/jobs.txt"
echo "jobs.txt:"; sed 's/^/  /' "$tmp"; rm -f "$tmp"
case "${1:-}" in
    --no-launch) ;;
    --payload) "$ROOT/tools/send.sh" "$HERE/hw_decode.elf" ;;
    *) "$ROOT/tools/launch.sh" --install "$HERE/build/titles/PPSA99014" ;;
esac
