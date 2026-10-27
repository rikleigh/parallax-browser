# Title launcher

Starts and stops registered titles from the Mac, so a build → launch →
read log → fix loop needs nobody at the console. `launcher.elf` is an elfldr
payload that stays resident on TCP 9031. `tools/launch.sh` is the client, and
it sends the payload first if it isn't running (after each jailbreak).

```sh
tools/launch.sh PPSA99009        # stop our running title (if any), start this one
tools/launch.sh -f PPSA99009     # also stop a title that isn't ours
tools/launch.sh --status         # "OK <appid> <TITLEID>" or "OK none"
tools/launch.sh --kill
tools/launch.sh --quit           # stop the launcher
```

Typical loop: `tools/upload-title.sh <titledir> && tools/launch.sh <ID>`,
then read the title's log from `tools/logserver.sh`.

Only titles listed in `PS5WK_OWN_TITLES` are stopped without `-f`. The
default list is Spike 2's, 3b's, 8's and the 2b benchmarks' title IDs, so
the owner's own games are never closed by accident. A new title ID must still
be registered once (PS5Upload → Play) before the launcher can start it.

## What works on 13.60 (2026-10-27)

| Call | Result |
| --- | --- |
| `sceLncUtilLaunchApp` | works with the payload's own authid (0x480000001000000e) |
| `sceLncUtilGetAppIdOfRunningBigApp` / `GetAppTitleId` | work |
| `sceLncUtilKillApp`, `sceLncUtilForceKillApp` | 0x80940004 with every authid tried |
| `sceSystemServiceKillApp(id, -1, 0, 0)` | blocks 10 s, then 0x80aa0003 (own authid) or 0x80a10009 (system authids) |
| `SIGKILL` to the title's pid (found with `sceKernelGetAppInfo`) | **works**: the system drops the app at once and the next launch goes ahead |

The console log is in `/data/ps5webkit/launcher.log`.
