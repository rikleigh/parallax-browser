#!/usr/bin/env bash
# The homebrew installer (apps/common/ps5install.c) on the Mac, without the
# registration (PS5 only): finding the title folder, reading param.json,
# moving it to <drive>/homebrew/<ID>, replacing an installed copy, and the
# files /user/app/<ID> gets.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cc -O2 -Wall -Wextra -Werror -o "$work/t" "$here/install/install-test.c" "$root/port/webkit/apps/common/ps5install.c"
fails=0
check() { # expect name got
    if [[ "$3" == "$1"* ]]; then echo "PASS $2: $3"; else echo "FAIL $2: $3 (expected $1)"; fails=$((fails + 1)); fi
}
title() { # dir id name
    mkdir -p "$1/sce_sys"
    printf 'eboot' > "$1/eboot.bin"
    printf 'png' > "$1/sce_sys/icon0.png"
    cat > "$1/sce_sys/param.json" <<J
{ "contentVersion": "01.002.000", "titleId": "$2",
  "localizedParameters": { "defaultLanguage": "en-US", "de-DE": { "titleName": "Nicht" }, "en-US": { "titleName": "$3" } } }
J
}
export PS5INSTALL_DRIVE="$work/drive"
mkdir -p "$work/drive/downloads/My App v1.2/PPSA01234-app" "$work/user"
title "$work/drive/downloads/My App v1.2/PPSA01234-app" PPSA01234 'Cool \"App\"'
check "OK PPSA01234|Cool \"App\"|01.002.000|$work/drive/downloads/My App v1.2/PPSA01234-app" "nested two deep, default language's name" \
    "$("$work/t" find "$work/drive/downloads/My App v1.2")"
check "ERR -1 No app here" "too deep for depth 0" "$("$work/t" find "$work/drive/downloads/My App v1.2" 0)"
check "OK $work/drive/homebrew/PPSA01234" "install moves it" "$("$work/t" install "$work/drive/downloads/My App v1.2" "$work/user")"
[[ -f "$work/drive/homebrew/PPSA01234/eboot.bin" && ! -e "$work/drive/downloads/My App v1.2/PPSA01234-app" ]] && echo "PASS moved, not copied" || { echo "FAIL move"; fails=$((fails+1)); }
check "$work/drive/homebrew/PPSA01234" "mount.lnk names the folder" "$(cat "$work/user/PPSA01234/mount.lnk")"
[[ -f "$work/user/PPSA01234/sce_sys/param.json" && -f "$work/user/PPSA01234/sce_sys/icon0.png" ]] && echo "PASS sce_sys copied" || { echo "FAIL sce_sys"; fails=$((fails+1)); }
# a newer version replaces the installed one
mkdir -p "$work/drive/downloads/v2"
title "$work/drive/downloads/v2" PPSA01234 'Cool App 2'
printf 'v2' > "$work/drive/downloads/v2/eboot.bin"
check "OK $work/drive/homebrew/PPSA01234" "reinstall replaces" "$("$work/t" install "$work/drive/downloads/v2" "$work/user")"
check "v2" "the new eboot is there" "$(cat "$work/drive/homebrew/PPSA01234/eboot.bin")"
left=$(ls "$work/drive/homebrew" | grep -c old || true)
check "0" "the old copy is deleted" "$left"
# bad title IDs and missing files
mkdir -p "$work/drive/downloads/bad"
title "$work/drive/downloads/bad" "ppsa1" 'Bad'
check "ERR -2 The title's param.json has no valid title ID" "bad title ID" "$("$work/t" find "$work/drive/downloads/bad")"
mkdir -p "$work/drive/downloads/noeboot/sce_sys" && echo '{"titleId":"PPSA00001"}' > "$work/drive/downloads/noeboot/sce_sys/param.json"
check "ERR -1" "no eboot.bin" "$("$work/t" find "$work/drive/downloads/noeboot")"
mkdir -p "$work/elsewhere/x"
title "$work/elsewhere/x" PPSA05555 'Off drive'
check "ERR -1 Apps can be installed from internal storage" "not on a console drive" "$("$work/t" install "$work/elsewhere/x" "$work/user")"
echo "$fails failure(s)"
exit $((fails > 0))
