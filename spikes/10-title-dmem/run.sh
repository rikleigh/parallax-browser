#!/usr/bin/env bash
# Upload and launch Spike 10 (PPSA99015); the log goes to tools/logserver.sh
# (and /data/ps5webkit/spike10.log on the console).
#   spikes/10-title-dmem/run.sh          the spike
#   spikes/10-title-dmem/run.sh --pad    the ~100 MB padded build (binary size vs flexible memory)
# The clip it decodes, /data/ps5webkit/hwdec/vrclip_7680x3840.mp4, is Spike 9's
# (spikes/09-hw-decode/run.sh uploads it).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
dir="$HERE/build/titles/PPSA99015"
[[ ${1:-} == --pad ]] && dir="$HERE/build/titles-pad/PPSA99015"
[[ -f $dir/eboot.bin ]] || { echo "build first: spikes/10-title-dmem/build-title.sh" >&2; exit 1; }
exec "$ROOT/tools/launch.sh" --install "$dir"
