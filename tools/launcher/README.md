# Title launcher

Registers, starts and stops titles from the Mac, so a build → launch →
read log → fix loop needs nobody at the console. `launcher.elf` is an elfldr
payload that stays resident on TCP 9031. `tools/launch.sh` is the client, and
it sends the payload first if it isn't running (after each jailbreak).

```sh
tools/launch.sh --install <titledir>   # upload, register if new, launch
tools/launch.sh PPSA99009              # stop our running title (if any), start this one
tools/launch.sh -f PPSA99009           # also stop a title that isn't ours
tools/launch.sh --register PPSA99013   # register what is in /data/homebrew/PPSA99013
tools/launch.sh --uninstall PPSA99013  # remove a spike title (PPSA99006/07/09–15 only)
tools/launch.sh --status               # "OK <appid> <TITLEID>" or "OK none"
tools/launch.sh --kill
tools/launch.sh --quit                 # stop the launcher
```

Only titles listed in `PS5WK_OWN_TITLES` are stopped without `-f`. The
default list is just PPSA99020 (the browser), so the owner's own titles are
never closed by accident. Never touch PPSA99008 (the owner's), PPSA99000
(ProsperoStore) or PPSA99030 (the YouTube app used for the jailbreak).

The spike titles PPSA99006/07/09–15 were uninstalled on 2026-10-28 with
`--uninstall`, which calls `sceAppInstUtilAppUnInstall` (found through the
export table, like the other AppInstUtil calls). The launcher refuses any other
ID. Their folders in `/data/homebrew` were deleted over FTP.

Titles end through the launcher too: `spikes/common/title_exit.c` asks it to
SIGKILL the title once `main` returns, because that is the only way to end a
GPU title without a debug error (below).

## What works on 13.60 (2026-10-27, checked on the TV)

| Call | Result |
| --- | --- |
| `sceLncUtilLaunchApp` | works with the payload's own authid (0x480000001000000e) |
| `sceLncUtilGetAppIdOfRunningBigApp` / `GetAppTitleId` | work |
| Registration: `/user/app/<ID>/{mount.lnk, sce_sys/param.json}` + `sceAppInstUtilAppInstallAll` | **works**: ShellCore rescans `/user/app` and registers every title folder (`AppInstallTitleDirMain <ID>` in the kernel log), as PS5Upload's Play does. `mount.lnk` names the folder in `/data/homebrew`; etaHEN mounts it at launch |
| `sceAppInstUtilAppUnInstall(title_id)` | **works**: the title leaves the app list and `/user/app/<ID>`; its `/data/homebrew` folder stays |
| `sceAppInstUtilAppInstallTitleDir` | not exported on 13.60 (in any process), despite being in the SDK stubs |
| `libSceAppInstUtil` via `sceKernelDlsym` | refused (0x80020003); `kernel_dynlib_dlsym` (reads the export table) works |
| `sceLncUtilKillApp`, `sceLncUtilForceKillApp` | 0x80940004 with every authid tried |
| `sceSystemServiceKillApp(id, -1, 0, 0)` | blocks 10 s, then 0x80aa0003 (own authid) or 0x80a10009 (system authids) |
| `SIGKILL` to the title's pid (found with `sceKernelGetAppInfo`) | **works, no error on screen**; logged as an app crash (0xa0d0c00d) |
| GPU title ends itself (`sceShellCoreUtilExitApp`, `exit`, `_exit`, or SIGTERM handled that way) | **debug error on screen** (crash reason 0xc; ShellUI suspends before it kills, and the suspend crashes a GPU title) |

The console log is in `/data/ps5webkit/launcher.log`.
