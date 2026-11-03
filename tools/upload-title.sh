#!/usr/bin/env bash
# Upload a folder title to the console's /data/homebrew/<TITLE_ID> over
# ftpsrv (port 2121). tools/launch.sh --install also registers and launches
# it. A title that is already registered keeps a copy of its
# param.json, icon0.png and backgrounds in /user/app/<TITLE_ID>/sce_sys,
# refreshed here too.
#   tools/upload-title.sh spikes/08-title-ipc/build/titles/PPSA99009
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/env.sh"
dir="${1:?usage: upload-title.sh path/to/<TITLE_ID>}"
id="$(basename "$dir")"
[[ $id =~ ^[A-Z]{4}[0-9]{5}$ && -f $dir/eboot.bin ]] || { echo "not a title folder: $dir" >&2; exit 1; }
ftp="ftp://$PS5_HOST:${PS5_FTP_PORT:-2121}"
(cd "$dir" && find . -type f | sed 's|^\./||') | while read -r f; do
    curl -sS --ftp-create-dirs -T "$dir/$f" "$ftp/data/homebrew/$id/$f"
    echo "  $f"
done
if curl -sS --max-time 5 "$ftp/user/app/$id/sce_sys/" >/dev/null 2>&1; then
    for f in param.json icon0.png pic0.png pic0.dds pic1.dds; do
        if [[ -f $dir/sce_sys/$f ]]; then
            curl -sS -T "$dir/sce_sys/$f" "$ftp/user/app/$id/sce_sys/$f" && echo "  (registered copy of $f refreshed)"
        fi
    done
fi
echo "uploaded $id to /data/homebrew/$id"
