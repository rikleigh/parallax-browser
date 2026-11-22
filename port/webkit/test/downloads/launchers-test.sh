#!/usr/bin/env bash
# Finding the homebrew launchers and handing them an installed app
# (apps/common/ps5launchers.c) on the Mac, against launchers/fake.py
# standing in for ShadowMountPlus's API and Kraken Mount's or TITAN-CORE's:
#   port/webkit/test/downloads/launchers-test.sh
# Needs glib (brew install glib) and python3.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
apps="$root/port/webkit/apps"
smp="${SMP_PORT:-18101}"
kp="${KRAKEN_PORT:-18097}"
work="$(mktemp -d)"
pids=()
cleanup() {
    for p in "${pids[@]:-}"; do [ -z "$p" ] || kill "$p" 2>/dev/null || true; done
    rm -rf "$work"
}
trap cleanup EXIT
cc -O1 -g -Wall -Wextra -Werror $(pkg-config --cflags glib-2.0) -o "$work/t" "$here/launchers/launchers-test.c" \
    "$apps/common/ps5launchers.c" "$apps/title/minijson.c" $(pkg-config --libs glib-2.0)
fails=0
check() { # expected name got
    if [[ "$3" == "$1"* ]]; then echo "PASS $2"; else echo "FAIL $2: $3 (expected $1)"; fails=$((fails + 1)); fi
}
has() { # pattern name file
    if grep -q -- "$1" "$3"; then echo "PASS $2"; else echo "FAIL $2: no \"$1\" in $(cat "$3" | tr '\n' '|')"; fails=$((fails + 1)); fi
}
start() { # mode port log state
    python3 -I "$here/launchers/fake.py" "$@" &
    pids+=($!)
    for _ in $(seq 50); do
        if python3 -I -c "import socket,sys; socket.create_connection(('127.0.0.1',int(sys.argv[1])),0.2)" "$2" 2>/dev/null; then return; fi
        sleep 0.1
    done
    echo "FAIL the fake $1 didn't start"; exit 1
}
stop() { kill "${pids[@]}" 2>/dev/null || true; wait 2>/dev/null || true; pids=(); }
con="$work/console"
mkdir -p "$con/data" "$con/user/app"
t() { "$work/t" "$1" "$con" "$smp" "$kp" "${@:2}"; }
H=/data/homebrew/PPSA01234

# nothing runs
check '{"scanner":null,"found":"","images":null,"smp":false,"smpApi":false,"kraken":false,"titan":false,"waits":false,"kstuff":true,"etahen":false' "nothing running" "$(t detect)"
check "NO No homebrew launcher" "nothing scans" "$(t scans $H)"
check "0 " "nothing: not ready" "$(t ready $H PPSA01234)"
check '{"scanner":"Kraken Mount","found":"","images":null,' "the setting names Kraken Mount" "$(t detect kraken)"
check '{"scanner":null,' "the setting says none" "$(t detect self)"
mkdir -p "$con/data/etaHEN"
check '{"scanner":null,"found":"","images":null,"smp":false,"smpApi":false,"kraken":false,"titan":false,"waits":false,"kstuff":true,"etahen":true' "etaHEN's folder" "$(t detect)"

# ShadowMountPlus, its port from config.ini
mkdir -p "$con/data/shadowmount"
printf '[config]\nversion = 2\n[api]\n# port = 1\nport = %s ; ours\n[scan]\nport = 5\n' "$smp" > "$con/data/shadowmount/config.ini"
: > "$con/data/.kstuff_noautomount"
echo '{"scan_paths":[]}' > "$work/smp.json"
start smp "$smp" "$work/smp.log" "$work/smp.json"
check '{"scanner":"ShadowMountPlus","found":"ShadowMountPlus","images":"ShadowMountPlus","smp":true,"smpApi":true,"kraken":false,"titan":false,"waits":false,"kstuff":false' \
    "ShadowMountPlus found (config.ini's port)" "$("$work/t" detect "$con" 0 "$kp")"
printf '[api]\nenabled = off\nport = %s\n' "$smp" > "$con/data/shadowmount/config.ini"
check '{"scanner":null,' "its API off (and no PS5 process list): not found" "$("$work/t" detect "$con" 0 "$kp")"
printf '[api]\nport = %s\n' "$smp" > "$con/data/shadowmount/config.ini"
check "YES" "it scans <drive>/homebrew" "$(t scans $H)"
check "YES" "and USB drives' homebrew" "$(t scans /mnt/usb3/homebrew/PPSA01234)"
check "YES" "outside its folders: its list" "$(t scans /data/Downloads/PPSA01234)"
: > "$work/smp.log"
check "0 " "announce in a scan folder" "$(t announce $H PPSA01234 0)"
has 'POST /api/v1/scan {"reset_attempts":true}' "... asks for a scan" "$work/smp.log"
: > "$work/smp.log"
check "0 " "announce outside them" "$(t announce '/data/My "apps"/PPSA01234' PPSA01234 0)"
has 'POST /api/v1/manual/add {"path":"/data/My \\"apps\\"/PPSA01234"}' "... adds it to its list, quoted" "$work/smp.log"
echo '{"scan_paths":["/data/games/"]}' > "$work/smp.json"
check '{"scanner":"ShadowMountPlus"' "custom scan_paths" "$(t detect)"
check "smp:/data/games" "... read, trailing slash trimmed" "$(t detect | grep -o 'smp:[^ ]*')"
: > "$work/smp.log"
t announce $H PPSA01234 1 > /dev/null
has 'POST /api/v1/manual/add {"path":"/data/homebrew/PPSA01234"}' "homebrew outside custom folders: its list" "$work/smp.log"
grep -q 'games/mount' "$work/smp.log" && { echo "FAIL replaced: it shouldn't be mounted"; fails=$((fails+1)); } || echo "PASS replaced: not mounted (it mounts at each start)"
check "0 " "not in its game list yet" "$(t ready $H PPSA01234)"
mkdir -p "$con/user/app/PPSA01234"
printf '%s\n' "$H" > "$con/user/app/PPSA01234/mount.lnk"
check "0 " "mount.lnk alone (written before it registers): not yet" "$(t ready $H PPSA01234)"
echo '{"games":[{"title_id":"PPSA01234","path":"/data/homebrew/PPSA01234","installed":false,"mounted":true}]}' > "$work/smp.json"
check "0 " "listed, not installed: not yet" "$(t ready $H PPSA01234)"
echo '{"games":[{"title_id":"PPSA01234","path":"/data/homebrew/OTHER","installed":true,"mounted":true}]}' > "$work/smp.json"
check "0 " "installed from another folder: not this one" "$(t ready $H PPSA01234)"
check "/data/homebrew/OTHER" "... which is where it's installed from" "$(t source PPSA01234)"
echo '{"games":[{"title_id":"PPSA01234","path":"/data/homebrew/PPSA01234","installed":true,"mounted":false}]}' > "$work/smp.json"
check "1 " "installed from it (released after: not mounted): ready" "$(t ready $H PPSA01234)"
check "0 " "not mounted: it can be replaced" "$(t unmount PPSA01234)"
echo '{"busy":true,"games":[{"title_id":"PPSA01234","path":"/data/homebrew/PPSA01234","installed":true,"mounted":true}]}' > "$work/smp.json"
check "-1 ShadowMountPlus has it mounted (it's running): close the app first" "mounted (running): the replace is refused" "$(t unmount PPSA01234)"
echo '{}' > "$work/smp.json"
check "/data/homebrew/PPSA01234" "not in its list: mount.lnk says where" "$(t source PPSA01234)"
printf '/data/homebrew/PPSA01234.exfat\n/data/x\n' > "$con/user/app/PPSA01234/mount_img.lnk"
check "/data/homebrew/PPSA01234.exfat" "... an image's first layer first" "$(t source PPSA01234)"
rm "$con/user/app/PPSA01234/mount_img.lnk"
printf '[api]\nenabled = off\n' > "$con/data/shadowmount/config.ini"
check "1 " "its API off (the setting names it): mount.lnk" "$(t ready $H PPSA01234 smp)"
printf '[api]\nport = %s\n' "$smp" > "$con/data/shadowmount/config.ini"
stop

# Kraken Mount
rm -f "$con/data/.kstuff_noautomount" "$con/user/app/PPSA01234/mount.lnk"
echo '{"scanpath":[],"games":[{"title_id":"PPSA01234","path":"/data/homebrew/PPSA01234","status":"checking","error":null}]}' > "$work/k.json"
start kraken "$kp" "$work/k.log" "$work/k.json"
check '{"scanner":"Kraken Mount","found":"Kraken Mount","images":null,"smp":false,"smpApi":false,"kraken":true,"titan":false,"waits":false,"kstuff":true' "Kraken Mount found" "$(t detect)"
check "YES" "it scans homebrew" "$(t scans $H)"
check "NO Kraken Mount doesn't look in /data/Downloads" "not outside its folders" "$(t scans /data/Downloads/PPSA01234)"
check "-1 Kraken Mount doesn't look" "announce outside: refused" "$(t announce /data/Downloads/PPSA01234 PPSA01234 0)"
: > "$work/k.log"
check "0 " "announce" "$(t announce $H PPSA01234 1)"
has 'POST /api/rescan' "... rescan" "$work/k.log"
has 'POST /api/rebuild/PPSA01234' "... replaced: rebuilt" "$work/k.log"
check "0 " "checking: not yet (a chunked reply)" "$(t ready $H PPSA01234)"
echo '{"games":[{"title_id":"PPSA01234","path":"/data/homebrew/PPSA01234","status":"ready"}]}' > "$work/k.json"
check "1 " "ready" "$(t ready $H PPSA01234)"
echo '{"games":[{"title_id":"PPSA01234","path":"/data/homebrew/PPSA01234","status":"error","error":"no eboot.bin"}]}' > "$work/k.json"
check "-1 no eboot.bin" "its error" "$(t ready $H PPSA01234)"
echo '{"games":[{"title_id":"PPSA01234","path":"/mnt/usb0/homebrew/PPSA01234","status":"ready"}]}' > "$work/k.json"
check "0 " "another copy listed (maybe from before its next pass): not yet" "$(t ready $H PPSA01234)"
echo '{"games":[{"title_id":"PPSA01234","path":"/mnt/usb0/homebrew/PPSA01234","status":"ready"},{"title_id":"PPSA01234","path":"/data/homebrew/PPSA01234","status":"duplicate"}]}' > "$work/k.json"
check "-1 Another copy of it is in its library" "ours a duplicate: failed" "$(t ready $H PPSA01234)"
echo '{"scanpath":["/mnt/usb0/games"],"games":[]}' > "$work/k.json"
check "NO Kraken Mount doesn't look in /data/homebrew" "its scanpath replaces the defaults" "$(t scans $H)"
check "YES" "... and is used" "$(t scans /mnt/usb0/games/PPSA01234)"
check '{"scanner":"ShadowMountPlus","found":"Kraken Mount","images":null,"smp":true,"smpApi":false,"kraken":false' "the setting names ShadowMountPlus instead (images: as found)" "$(t detect smp)"
echo '{"scanpath":["/data"],"games":[]}' > "$work/k.json"
check "YES" "scanpath /data: a game one folder below it" "$(t scans $H)"
echo '{"scanpath":["/mnt/usb0"],"games":[]}' > "$work/k.json"
check "NO" "... not below a drive's root" "$(t scans /mnt/usb0/homebrew/PPSA01234)"
echo '{"paused":true,"games":[{"title_id":"PPSA99207","path":"/data/homebrew/PPSA99207","status":"ready"}]}' > "$work/k.json"
check '{"scanner":"Kraken Mount","found":"Kraken Mount","images":null,"smp":false,"smpApi":false,"kraken":true,"titan":false,"waits":true' "paused while the browser runs: it waits" "$(t detect)"
echo '{"games":[{"title_id":"PPSA01234","path":"/mnt/usb0/homebrew/PPSA01234","status":"ready"}]}' > "$work/k.json"
check "/mnt/usb0/homebrew/PPSA01234" "where Kraken has it from" "$(t source PPSA01234)"
echo '{"scan_paths":[]}' > "$work/smp.json"
start smp "$smp" "$work/smp.log" "$work/smp.json"
echo '{"scanpath":["/mnt/usb0/games"],"games":[]}' > "$work/k.json"
check '{"scanner":"ShadowMountPlus","found":"ShadowMountPlus"' "both answer (Kraken waiting for it to exit): ShadowMountPlus" "$(t detect)"
check "YES" "... and its folders count, not Kraken's" "$(t scans $H)"
stop

# images: what takes .ffpkg, what ShadowMountPlus finds by itself, its list
echo '{"scan_paths":[],"smp_version":"1.7"}' > "$work/smp.json"
start smp "$smp" "$work/smp.log" "$work/smp.json"
check "1 " "ShadowMountPlus 1.7 mounts .ffpkg" "$(t ufs2)"
echo '{"scan_paths":[],"smp_version":"1.7beta12"}' > "$work/smp.json"
check "1 " "... 1.7beta12 too" "$(t ufs2)"
echo '{"scan_paths":[],"smp_version":"1.6.3"}' > "$work/smp.json"
check "0 ShadowMountPlus 1.6.3 doesn't mount .ffpkg" "1.6.3 doesn't" "$(t ufs2)"
echo '{"scan_paths":[],"smp_version":""}' > "$work/smp.json"
check "-1 " "its version not known: not known" "$(t ufs2)"
echo '{"scan_paths":[]}' > "$work/smp.json"
check "YES" "an image in /data/homebrew: found by its scan" "$(t itself /data/homebrew/PPSA01234.ffpkg)"
check "YES" "... in /mnt/usb2/homebrew" "$(t itself /mnt/usb2/homebrew/PPSA01234.exfat)"
check "NO" "... elsewhere: not" "$(t itself /data/ps5webkit/downloads/PPSA01234.ffpkg)"
: > "$work/smp.log"
check "0 " "an image elsewhere announced" "$(t announce /data/dl/PPSA01234.ffpkg PPSA01234 0)"
has 'POST /api/v1/manual/add {"path":"/data/dl/PPSA01234.ffpkg"}' "... added to its list by path" "$work/smp.log"
echo '{"scan_paths":[],"manual_refuse":true}' > "$work/smp.json"
check "-1 ShadowMountPlus didn't add /data/dl/PPSA01234.ffpkg to its list (it refused: not a game source)" "... refused: says so" \
    "$(t announce /data/dl/PPSA01234.ffpkg PPSA01234 0)"
echo '{"scan_paths":[]}' > "$work/smp.json"
: > "$work/smp.log"
t announce /data/homebrew/PPSA01234.exfat PPSA01234 0 > /dev/null
has 'POST /api/v1/scan' "an image in its folders: a scan" "$work/smp.log"
echo '{"scan_paths":["/mnt/usb0/games"]}' > "$work/smp.json"
check "NO" "its own scan folders: /data/homebrew isn't one then" "$(t itself /data/homebrew/PPSA01234.ffpkg)"
check "YES" "... /mnt/usb0/games is" "$(t itself /mnt/usb0/games/PPSA01234.ffpkg)"
stop
check "-1 ShadowMountPlus isn't running" "nothing running: .ffpkg not known" "$(t ufs2)"
printf '/data/a.exfat' > "$con/data/shadowmount/manual.lst"
check "0" "manual.lst: added" "$(t manual /data/dl/PPSA01234.ffpkg)"
check "0" "manual.lst: added again" "$(t manual /data/dl/PPSA01234.ffpkg)"
[ "$(cat "$con/data/shadowmount/manual.lst")" = "$(printf '/data/a.exfat\n/data/dl/PPSA01234.ffpkg')" ] && echo "PASS manual.lst: once, on a line of its own" \
    || { echo "FAIL manual.lst: $(cat "$con/data/shadowmount/manual.lst" | tr '\n' '|')"; fails=$((fails + 1)); }

# TITAN-CORE (Kraken Mount's fork) with its ShadowMountPlus
: > "$con/data/.kstuff_noautomount"
echo '{"scanpath":["/mnt/usb0/games"],"games":[]}' > "$work/k.json"
echo '{"scan_paths":[]}' > "$work/smp.json"
start titan "$kp" "$work/k.log" "$work/k.json"
start smp "$smp" "$work/smp.log" "$work/smp.json"
check '{"scanner":"TITAN-CORE","found":"TITAN-CORE","images":"TITAN-CORE","smp":true,"smpApi":true,"kraken":false,"titan":true' "TITAN-CORE found" "$(t detect)"
check "YES" "outside TITAN's folders: its ShadowMountPlus" "$(t scans $H)"
: > "$work/k.log"; : > "$work/smp.log"
t announce $H PPSA01234 0 > /dev/null
has 'POST /api/v1/scan' "both asked: ShadowMountPlus" "$work/smp.log"
has 'POST /api/rescan' "... and TITAN-CORE" "$work/k.log"
echo '{"games":[{"title_id":"PPSA01234","path":"/data/homebrew/PPSA01234","installed":true,"mounted":true}]}' > "$work/smp.json"
check "1 " "registered by its ShadowMountPlus" "$(t ready $H PPSA01234)"
stop

# something else on 8097 that answers 200 to everything
start any "$kp" "$work/k.log"
check '{"scanner":null,' "a catch-all server isn't TITAN-CORE" "$(t detect)"
stop

# nothing answers in time: a port that accepts and says nothing
python3 -I -c "
import socket,time,sys
s=socket.socket();s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);s.bind(('127.0.0.1',int(sys.argv[1])));s.listen(5)
c=[]
while True: c.append(s.accept()[0])" "$kp" &
pids+=($!)
sleep 0.5
start_s=$(python3 -I -c 'import time;print(time.time())')
check '{"scanner":null' "a silent port: not found" "$(t detect)"
took=$(python3 -I -c "import time,sys;print(round(time.time()-float(sys.argv[1]),1))" "$start_s")
python3 -I -c "import sys;sys.exit(0 if float(sys.argv[1])<4 else 1)" "$took" && echo "PASS ... in ${took}s" || { echo "FAIL took ${took}s"; fails=$((fails+1)); }
stop

if [ "$fails" -eq 0 ]; then echo "ALL PASS"; else echo "$fails FAILED"; exit 1; fi
