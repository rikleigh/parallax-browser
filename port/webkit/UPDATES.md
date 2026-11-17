# Updates (spec)

Status: **built and tested on the console (2026-11-17)**, for 0.2.0, so
that 0.2.0 can update itself to later releases. Results under "Console
test" at the end.

Each time the browser opens, it looks for a newer release and, if there is
one, offers it. Nothing is downloaded or installed without the user's yes.
There's no setting to turn the check off: it's one small request to GitHub
per start, and nothing else happens unless the user says yes.

## What the user sees

1. The browser opens as usual. Once the first page has loaded (or after
   10 s), it asks GitHub for the newest release.
2. If one is newer, the **update page** opens over the current tab (a
   browser page like Settings, `parallax://update`): **"Parallax Browser
   0.3.0 is available"**, the release notes, and three buttons: **Update**,
   **What's new** (the release's page on GitHub, in a new tab) and **Later**.
   Later or O closes it; the next start asks again.
3. Update: it downloads and unpacks in the background, with progress on the
   update page (and in Settings). The user can go back to browsing; a
   message says when it's done.
4. When it's ready: **"0.3.0 is ready"**, with **Restart now** and
   **Next time**. Next time installs it when the browser next opens, without
   asking again (the user already said yes).
5. Installing: "Updating to 0.3.0…" for a few seconds, then the browser
   opens again on the page you were on, now 0.3.0, with the message
   **"Updated to 0.3.0"**.

Settings gets an **Updates** section: the version and the state (up to
date, checked at 10:42; 0.3.0 available; downloading 45%; ready; couldn't
check: why), and **Check for updates**, which opens the update page when
there is one.

## Where releases come from

`https://api.github.com/repos/rikleigh/parallax-browser/releases?per_page=10`:
the newest published release by version, drafts skipped and **pre-releases
included**. That is the catalog's rule too (its update job counts
pre-releases), so the browser and ProsperoStore offer the same release.
`releases/latest` would not do: it leaves pre-releases out, and v0.1.0 is
one (it returns 404 today). From the release:

- `tag_name` (`v0.3.0`): the version.
- The asset named `parallax-browser-<version>.zip` (what make-release.sh
  makes): its `browser_download_url`, `size` and `digest` (`sha256:…`,
  which GitHub computes itself; v0.1.0's asset has one).
- `html_url`: the page that What's new opens.

This is the same file ProsperoStore installs: the catalog's daily job
proposes our newest GitHub release, picks the same zip, and pins it by the
same GitHub digest. Both paths install identical bytes. The catalog is
reviewed, so it can list a release a day or more later than GitHub does;
that only affects store installs.

Unauthenticated, GitHub allows 60 API calls an hour per address. One per start
is far inside that.

**Versions.** The tag `vA.B.C` and the zip's `sce_sys/param.json`
`contentVersion` must agree: `contentVersion` is `0(A+1).BBB.CCC`, so 0.2.0
is `01.002.000` and 0.3.1 is `01.003.001`. A release is newer when its
tag is a higher semantic version than the running version. ProsperoStore
compares `contentVersion` as text, and this scheme keeps that order too.

## The pieces

| Piece | Runs as | Does |
| --- | --- | --- |
| `apps/title/updates.c` (new) | the title | Schedule, state, Settings rows, the link server's `/update`, messages; starts the job; at start, installs a staged update through the restart helper |
| `apps/child/update-job.c` (new) | a ps5wk-child task payload (like `adblock-job.c`) | Finds the install, asks GitHub, downloads, checks, unpacks into a staging folder |
| `apps/relaunch/ps5wk-relaunch.c` | the restart helper payload | With an update: after the title is gone, swaps the staged files in, refreshes the system's copies of `sce_sys`, then starts the title |

Why payloads: the console slows a title's own file writes to about 2 MB/s
after a few hundred megabytes, and the unpacked browser is about 470 MB.
Payloads aren't slowed. The ad block job already fetches from GitHub this
way (libsoup with the payloads' TLS, JSON through JavaScriptCore), and the
restart helper already kills and restarts the title.

### 1. Find the install (the job)

The title's own folder is `/app0` to the title, which payloads can't see.
The job looks for the installed folder in the places ShadowMountPlus scans,
as ProsperoStore's self-update kit does: `/data/homebrew`,
`/data/etaHEN/games`, the same on `/mnt/ext0`, `/mnt/ext1` and
`/mnt/usb0`–`7`, and those drives' roots. It wants **exactly one** folder
whose `sce_sys/param.json` says `PPSA99207` and the running
`contentVersion`. None (an image install, or somewhere else) or more than
one: no update, and Settings says why ("installed as an image: update by
hand").

### 2. Check that ProsperoStore doesn't manage it

ProsperoStore treats an app as its own when
`/data/prosperostore/receipts/PPSA99207.json` exists, its `location` is the
folder that holds the app (e.g. `/data/homebrew`), and its `contentVersion`
is the one installed (its `managed()` check). When all three hold, the
browser **does not update itself**. It still checks GitHub, to say that an
update is out.

Also, `/data/prosperostore/journal.json` naming `PPSA99207` means that the
store has an operation on the browser in progress, or interrupted. Then the
browser does nothing at all.

Both are read only. The browser never writes anything under
`/data/prosperostore`.

Why this is enough to avoid conflicts:

- **Only one big app runs at a time.** The store won't touch a running app
  ("Close the app first"), and while the store runs, the browser doesn't.
  The one overlap is the helper's few seconds after the browser closes, so
  the helper checks both files again just before it swaps.
- **An install the store manages stays managed.** The browser never changes
  it, so the store's receipt keeps matching.
- **An install the browser updated** was not the store's to begin with (no
  matching receipt). If the user later picks "Manage with ProsperoStore",
  the store writes a receipt and the browser steps back from then on.
- **If a receipt exists but doesn't match** (someone copied a newer version
  over a store install by hand), the store already shows "Installed outside
  ProsperoStore", so the browser may update it.
- **Folders.** The store keeps its work in `/data/prosperostore` and
  `<drive>/prosperostore`; ours are `/data/ps5webkit/update` and
  `<drive>/ps5webkit-update`. Neither is inside a folder ShadowMountPlus
  scans, so a staged copy can never show up as a second app.

### 3. Download and check (the job)

- The zip goes to the staging folder on the install's drive:
  `/data/ps5webkit/update/` for `/data`, or `<drive>/ps5webkit-update/` for
  the others, so the final step is renames only. A free-space check comes
  first: the zip plus about three times its size.
- Only `https://github.com/` and GitHub's release file host after the
  redirect. The size and the SHA-256 must match the release's `size` and
  `digest`, or the file is deleted.

### 4. Unpack (the job)

- With zlib (stored and deflate only), into `staging/PPSA99207/`.
- The checks are those of ProsperoStore's kit:
  - every entry is under `PPSA99207/`;
  - no absolute paths, no `..`, no links or special files, no encryption,
    no duplicates;
  - limits on the number of files and on the total size.
- `sce_sys/param.json` must say `PPSA99207` and the `contentVersion` that
  belongs to the tag.
- Every file and folder gets mode `0777`: the console won't start an app
  with less.
- When it's done, the job writes `staging/ready.txt`: the version, the
  tag, the install folder, the folder that holds it, the running
  `contentVersion` and the release page. Then, last, it writes
  `/data/ps5webkit/update/staged.txt`, which names the staging folder. A
  staging folder without both is a leftover, and the next job deletes it.

### 5. Install (the title, then the helper)

At start, before tabs load, the title reads `ready.txt`. If everything
still holds, it installs the update:

- the staged version is newer than the running one;
- the install folder still holds the running version;
- ProsperoStore doesn't manage it, and its journal doesn't name the
  browser;
- the user said Update for this version (`ready.txt` is only written
  after a yes).

Restart now does the same at once. The title shows "Updating to
0.3.0…" and starts the restart helper with the header
`PS5WK-LAUNCH 1 update 0 PPSA99207:<pid>:<go file>:<staging folder>`. Then
it winds down, the same as for Restart browser: tabs saved, the page to
come back to written down.

Once the title is gone, the helper:

1. Reads `ready.txt` again and repeats the install checks above (the
   ProsperoStore receipt and journal, the installed version).
2. Moves each top-level entry of the install folder that the release also
   has into `previous/` next to the staging folder (`/data/ps5webkit/update/previous` for `/data`), then moves the new one in (renames on one
   drive). Entries the release doesn't have stay. If a move fails, it undoes
   the moves so far.
3. Copies the new `sce_sys` files over the system's copies in
   `/user/app/PPSA99207/sce_sys` and `/user/appmeta/PPSA99207`, the same as
   ProsperoStore's `refresh_registered` and our `upload-title.sh`. Without
   this, the home screen keeps the old name and pictures.
4. Starts the title, which shows "Updated to 0.3.0".

It deletes the staging folder (the job already deleted the zip). `previous/` is kept
until the next update, as with ProsperoStore's
`/data/prosperostore/previous`: to go back, copy it over the folder by FTP.

The install folder stays where it is, so ShadowMountPlus's mount of it
stays valid.

## Failures

| When | What happens |
| --- | --- |
| No network, GitHub down, rate limit | Settings: "Couldn't check: <why>". Tried again at the next start, or with Check for updates |
| Download fails, wrong size or digest, cancelled | Zip deleted; the update page says so and offers Update again |
| Zip refused (paths, links, the wrong title or version) | Staging deleted; "The 0.3.0 download was refused: <why>"; the same release isn't tried again |
| ProsperoStore manages the install | Nothing downloaded; Settings says so |
| The install isn't a folder, or there's more than one | Settings says why; no download |
| The title doesn't wind down within 60 s | The helper does nothing (as for Restart browser now) |
| A rename fails | The moves are undone; the old version starts; message "The update couldn't be installed" |
| Power lost during the renames | The one unprotected moment, a fraction of a second, as with the store. `previous/` holds the old files |
| Power lost at any other time | A leftover staging folder, deleted by the next job |
| The new version fails to start | Not handled automatically in 0.2.0. `previous/` is there for a copy back by FTP |

## The link server

- `GET /update`: the state as JSON (version, state, available, progress,
  managedBy, error, lastCheck).
- `POST /update?action=check`, `?action=install`.

The update page and Settings use these, as Settings does for the ad
blocker.

## Testing

`/data/ps5webkit/update-test-yes` (a file; test use only) answers yes for
the user: Update when a release is offered, Restart now when it's ready.

`/data/ps5webkit/update-source` (a file; test use only) holds a URL to use
instead of GitHub's API: a JSON file in the API's form, served from the Mac
with `python3 -m http.server`, with `http://` allowed for that file only.
Its zip is a copy of the release with `contentVersion` raised.

1. A full update: check, download, unpack, restart, the new version
   running, the right `contentVersion`, the files byte for byte, the home
   screen tile still working.
2. An update that's refused: wrong digest, a `../` entry, the wrong title ID
   or version. Nothing changes.
3. ProsperoStore managing the install. To avoid touching the real store's
   files, the job and the helper read `/data/ps5webkit/update-store-root`
   (test use only) in place of `/data/prosperostore` when it exists. Test a
   matching receipt, a receipt that doesn't match, and a journal.
4. Later: nothing downloaded, asked again at the next start.
5. Both modes (pages in their own process, and in the browser).

## Not in 0.2.0

- A channel that skips pre-releases.
- Going back to the previous version from Settings.
- Detecting that a new version keeps crashing at start.

## Release checklist addition

- The tag is `vA.B.C`, and `param.json` has the matching `contentVersion`
  (`make-release.sh` checks).
- The zip is attached as `parallax-browser-A.B.C.zip`. A pre-release
  reaches everyone, through the browser and through ProsperoStore alike;
  keep test builds as drafts.

## Console test (2026-11-17)

The Mac served a copy of the build as "0.2.1" (contentVersion raised, a
marker file added), through `update-source` and `update-test-yes`:

| Step | Result |
| --- | --- |
| Check (10 s after start) | Found the one installed copy, `/data/homebrew/PPSA99207`. Picked v0.2.1 over v0.1.0 and a draft, in 1.0 s. The update page opened |
| Download, 194 MB over the LAN | 76.6 s, size and SHA-256 checked |
| Unpack, 465 MB | 14.9 s (in the payload; the title's own writes would be slowed) |
| Install | The helper waited for the title to wind down, moved 9 top-level entries (the old ones to `update/previous`), refreshed 10 `sce_sys` copies in `/user`, and started the title again: about 6 s in all |
| After | `param.json` 01.002.001 in the folder and in the system's copies; the marker there; the log showed the 0.2.1 build running and "the last install: ok 0.2.1"; the next check said "not newer" |
| ProsperoStore receipt for this folder and version (`update-store-root`) | "it manages this install": nothing downloaded, even with `update-test-yes`; a message only |
| A release whose digest doesn't match | Downloaded, refused ("doesn't match the release's"), the staging folder removed, 0.2.0 untouched |
| The real GitHub API | v0.1.0 (a pre-release) and its digest read; "up to date" |
| Unpacker on the Mac (`test/update/zip-test.sh`) | The title as make-release.sh zips it, byte for byte, every file 0777; `../`, absolute, backslash, `./`, a link, a fifo, a duplicate, a bad CRC, not a zip, another title's prefix: all refused |

Not tested on the console: a receipt that doesn't match, ProsperoStore's
journal, the helper's own refusal (a receipt appearing between the download
and the restart), a folder on an external or USB drive, and a rename that
fails part way.
