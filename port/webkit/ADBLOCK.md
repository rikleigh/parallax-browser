# Ad blocker (phase 1: network rules)

The browser blocks ads and trackers with **uBlock Origin Lite**'s network
rules (github.com/uBlockOrigin/uBOL-home, GPL-3.0, like this project),
applied by WebKit's own content blocker (`WebKitUserContentFilterStore`,
`ENABLE_CONTENT_EXTENSIONS`, on in both builds). Phase 1 is network blocking
only: no cosmetic filtering (element hiding) and no scriptlets.

Files:

| | |
| --- | --- |
| `adblock/dnr-to-webkit.js` | the converter: uBO Lite's declarativeNetRequest (DNR) rules to WebKit content-blocker JSON. Plain JavaScript, no dependencies; the same file runs under node on the Mac and in JavaScriptCore on the console (in the child below) |
| `../../tools/ubol-convert.js` | Mac tool: downloads the pinned uBO Lite release, checks its sha256 pins, converts, writes `toolchain/adblock/adblock-rules.json` + `adblock-info.ini` (never committed) |
| `apps/title/adblock.c` | the title side: loading the compiled list, the filter on the web view, the per-site allow list, starting compile and update jobs |
| `apps/child/adblock-job.c` | the compile and the update, in `ps5wk-child` started as a task payload (*Compile and update in a child process* below) |
| `wpe-ps5/WPEProcessManagerPS5.cpp` | `wpe_process_manager_ps5_run_task`: starts the child ELF in another mode through elfldr, like the WebProcesses |
| `apps/title/package.sh` | puts `adblock-rules.json`, `adblock-info.ini` and `dnr-to-webkit.js` in the title (`/app0/adblock`), so blocking works offline and on first start |
| `test/adblock/index.html` | synthetic test page (made-up paths on well-known ad hosts and on control hosts) |
| `test/adblock/measure.js`, `urls.txt` | A/B measurement on real pages (test URL list + `test-onload.js`) |

## Using it

- **Settings > Browser > Ad blocking** (On by default) turns it off and on
  everywhere. It applies at once: the filter comes off (or goes back on)
  the web view, and the page shown reloads. Stored as the flag file
  `/data/ps5webkit/adblock-off` (present = off).
- **Per site:** on the PS5 press **R3** on the page, then the first card
  under *Ad blocking*: **Allow ads on <site>** (the site and its
  subdomains; `www.` is dropped). Going back to the page (R3 or O) loads it
  without blocking. The same card then offers *Block ads on <site> again*,
  and *Sites where ads are allowed* lists every entry with a *Remove*
  button. From another device the card is for the page the PS5 shows, and
  the PS5 reloads it. Stored in `/data/ps5webkit/adblock-allow.txt` (one
  site per line, editable over FTP; read at start).
  There is no pad shortcut: every button is taken (WPEDisplayPS5.cpp:
  D-pad, X, O, Square, Triangle, L1/R1, L2/R2, Options, R3, L3, touchpad),
  and R3 + one button press is short.
- **Lists:** the *Lists* card shows the uBO Lite version, the rule count and
  where the list came from (bundled or downloaded), and has **Update lists
  now**.
- API (same PIN and origin rules as the other settings routes):
  `POST /adblock?do=update`, `POST /adblock?do=allow&site=<host>`,
  `POST /adblock?do=block&site=<host>`; `/settings.json` has an `adblock`
  member (enabled, filterOn, site, sites, list, status, updating, compiling,
  lastCheck).

Per-site allowing removes the filter from the web view while an allowed site
is shown (load-changed: started, redirected, committed) instead of adding a
second "ignore-previous-rules" filter: WebKit evaluates every filter on its
own, so an exception in one list cannot undo a block in another.

## How the list gets onto the console

1. **Bundled:** `package.sh` copies `toolchain/adblock/adblock-rules.json`
   (made by `node tools/ubol-convert.js` if missing) into the title.
2. **First start:** a child process compiles it into
   `/data/ps5webkit/browser/contentfilters/ContentRuleList-ubol-a`, and the
   title loads that (`compiled.ini` in `/data/ps5webkit/adblock` says which
   slot and from which list). Later starts only load it (2-6 ms).
3. **Update on the console**, by itself once a day (first check 2 minutes
   after start, never while a page loads or in VR; `last-check` holds the
   time; `/data/ps5webkit/adblock-noupdate` turns it off) or with *Update
   lists now*:
   all in one child process:
   - `manifest.json` of the newest uBO Lite (raw.githubusercontent.com,
     `uBOL-home/main/chromium/`), downloaded with libsoup and the payloads'
     TLS (glib-networking OpenSSL, `/data/ps5webkit/etc/ssl/cert.pem`, as in
     the NetworkProcess);
   - if its version (or the converter's version) is newer than the list in
     use, or the update was asked for from Settings: the default rulesets
     at that release's tag (`rulesets/main/<id>.json`,
     `rulesets/regex/<id>.json`);
   - converted by `dnr-to-webkit.js` in JavaScriptCore (LLInt);
   - compiled into the other slot (`ubol-b`) while the old list keeps
     blocking, and only then saved as
     `/data/ps5webkit/adblock/adblock-rules.json` + `adblock-info.ini`;
   - the title loads the new slot, swaps it in (new filter added before the
     old one is removed) and deletes the old slot.
   Any failure (download, conversion, compile, the child crashing or being
   ended) keeps the list in use; the *Lists* card and `[adblock]` log lines
   say what happened. At start the newer of the bundled and the downloaded
   list is used. The daily check is a child launch too (a few seconds,
   memory outside the title); it ends after the manifest when nothing is
   new.

### Updating the bundled list (Mac)

    node tools/ubol-convert.js --latest   # converts the newest release, prints new pins
    # put PINNED_VERSION / PINNED_SHA256 from the output into tools/ubol-convert.js
    node tools/ubol-convert.js            # verifies the pins, writes toolchain/adblock/
    node tools/ubol-convert.js --test     # converter self-test

The pins: the release version and the sha256 of `manifest.json` and of each
ruleset file at that tag (null = no regex ruleset in that release).
Pinned now: **uBO Lite 2026.1026.1812**.

## Conversion

Rulesets: the ones uBO Lite enables by default (`manifest.json`
`declarative_net_request.rule_resources[].enabled`): ublock-filters,
easylist, easyprivacy, pgl, ublock-badware, urlhaus-full, each with its
`regex/` part if the release has one. Not used: the strictblock and urlskip
rulesets, cosmetic filters, scriptlets.

The mapping mirrors WebKit's own DNR translator for Safari web extensions
(`Source/WebKit/UIProcess/Extensions/Cocoa/_WKWebExtensionDeclarativeNetRequestRule.mm`):

| DNR | WebKit |
| --- | --- |
| `urlFilter` (`\|\|`, `\|`, `*`, `^`) | `url-filter` regex (`^[^:]+://+([^:/]+\.)?` for `\|\|`, `[^a-zA-Z0-9_.%-]` for `^`; a trailing `^` also matches the end of the URL, as in DNR) |
| `regexFilter` (RE2) | rewritten into WebKit's subset (`\d`, `\w`, `{n,m}` unrolled, `(?:)`, alternation expanded into up to 64 rules) or skipped |
| `requestDomains` | one rule per domain (`...domain[:/?]`, combined with the urlFilter as WebKit's translator does) |
| `initiatorDomains` / `excludedInitiatorDomains` | `if-frame-url` / `unless-frame-url` |
| `topDomains` / `excludedTopDomains` | `if-domain` / `unless-domain` |
| `resourceTypes` / `excludedResourceTypes` | `resource-type` (default: all but `main_frame`, as in DNR; `object`, `csp_report` dropped) |
| `domainType` | `load-type` |
| `requestMethods` / `excludedRequestMethods` | `request-method` (one rule per method) |
| `isUrlFilterCaseSensitive` | `url-filter-is-case-sensitive` |
| `block` | `block` |
| `allow` | `ignore-following-rules` |
| `allowAllRequests` | `ignore-following-rules` with `if-top-url` / `if-frame-url` |
| `upgradeScheme` | `make-https` |
| priority | rules sorted by priority, then allow before block (Chrome's order) |

Differences from DNR, all counted in the stats:

- **redirect** and **modifyHeaders** only act in WebKit for web extensions
  (`DocumentLoader::allowsActiveContentRuleListActionsForURL`), not for a
  WebKitUserContentFilter. A redirect to a blank resource (noop.js, 1x1.gif,
  noop-1s.mp4, ...) becomes a block; a redirect to a surrogate script
  (google-ima.js, googletagservices_gpt.js, ...) becomes an allow, so the
  real script loads instead of the page breaking; URL transforms and
  modifyHeaders are skipped.
- **excluded request domains / methods**, and initiatorDomains together with
  excludedInitiatorDomains, become extra ignore-following-rules (as in
  WebKit's translator); those stop *every* later rule for that request, so
  they are placed last in their priority group.
- every emitted regex is checked against WebCore's URLFilterParser rules
  (`^` first, `$` last, no `|`, `{}`, `\b`, lookarounds), because one
  unsupported regex fails the whole list.

Result for 2026.1026.1812 (node and the console give the same bytes,
sha256 `04185856...`):

    19127 DNR rules -> 119449 WebKit rules (25.7 MB of JSON); 441 skipped
      ublock-filters 5637 -> 7346 (316 skipped), regex 210 -> 176 (119)
      easylist 3715 -> 46189 (4), regex 18 -> 65 (12)
      easyprivacy 9075 -> 56313 (4), regex 7 -> 8
      pgl 1 -> 3471, ublock-badware 441 -> 4040, regex 22 -> 19 (10)
      urlhaus-full 1 -> 1790
    redirects as block 551, as allow 82; regex rules expanded 40, widened 7
    skipped: URL-transform redirects 255, modifyHeaders 51, regexFilter with
      requestDomains 44, regex repetition > 64 31, responseHeaders
      condition 30, object/csp_report only 11, \b 11, other regex 6,
      invalid excluded domain 2, two page conditions 1

WebKit's limit is 150000 rules per list.

## Measured on the console (2026-11-02, WC build, log-only)

**Synthetic page** (`test/adblock/index.html`): 14 of 14 ad hosts blocked
(fetch rejected in 15-55 ms, `CONSOLE CONTENTBLOCKER INFO Content blocker
prevented ...` in the log), 3 of 3 control hosts answered; with blocking off,
2 of 14 "blocked" (network errors) and the rest answered by the servers in
170-1000 ms. Same result in GPU mode (`compositing` + `wc-async-scroll`).

**Compile and load** (119449 rules, 25.7 MB JSON):

| | |
| --- | --- |
| compile (first start, or after an update) | in the title (before the child process): 44-67 s on a WebKit work-queue thread (one core), plus a ~1.2 s JSON parse on the title's main loop. Now in the child: 53-57 s, and none of it in the title |
| title memory while compiling | in the title: direct heap peak 557-691 MB, **~420 MB of it kept until the browser restarted**. Now: flat (26.8 MB before, during and after; below) |
| compiled file | 78.7 MB in `/data/ps5webkit/browser/contentfilters` (includes the 26 MB source) |
| load at later starts | 6 ms (170 ms once) |
| WebProcess | +10 MB peak RSS on example.com (177.2 -> 187.2 MB); over nine heavy pages the WebProcess stayed lower with blocking on (peak RSS 682-814 MB on, 779-844 MB off) |
| NetworkProcess | no difference (153.0 / 153.1 MB) |

**On-console update** (forced from Settings, same release): manifest
0.1-0.4 s, 10 files (4.7 MB) downloaded in 5.0-5.5 s, conversion in
JavaScriptCore (LLInt) **1.25 s**, title heap +400 MB during the conversion
(+150 MB once the JSC context is released), then the compile above. The
output is byte-identical to node's (same sha256). The daily check costs only
the manifest download when nothing is new.

**Real pages** (`test/adblock/urls.txt`, `measure.js`; blocking off first,
then on, so the second run had a warm HTTP cache; Resource Timing count 17 s
after load; "blocked" = CONTENTBLOCKER lines):

| page | requests off -> on | blocked | load event off -> on |
| --- | --- | --- | --- |
| The Verge | (load never finished in 60 s) -> 172 | 20 | > 60 s -> 31.2 s |
| BBC News | 145 -> 121 | 6 | 5.9 -> 3.7 s |
| CNN | 112 -> 85 | 40 | 17.6 -> 15.7 s |
| reddit r/PS5 | 126 -> 75 | 14 | 13.3 -> 11.7 s |
| Daily Mail | (redirect: 111 at 2 s) -> 158 | 56 | - -> 27.7 s |
| The Independent | (not measured) -> 53 | 24 | - -> 13.3 s |
| MDN | 78 -> 66 | 14 | 1.4 -> 1.5 s |
| Apple iPhone page | 55 -> 53 | 2 | 6.1 -> 6.6 s |
| YouTube (Big Buck Bunny) | plays (4K, frames advancing) -> plays | 14 | 20.6 -> 7.0 s |

WebProcess CPU stayed at 0.4-0.6 cores per page either way and the title's
main thread at ~0 % (no difference above the noise in these log-only runs).
Bytes: cross-origin responses without Timing-Allow-Origin report no size, so
Resource Timing can't give a total; not reported. Every page loaded with
blocking on and its title; YouTube played. Nothing broke in these runs; a
site that does is what the per-site allow is for (no site-specific code).

## Title fixes this needed

The title had never run JavaScript, compiled a content filter or deleted a
file through WebKit:

- `package.sh`: the title's heap (dheap.c, dlmalloc) hands out 32-byte
  aligned blocks (`-DMALLOC_ALIGNMENT=32`). WebKit's TZone types fall back
  to plain malloc in the title (bmalloc through the system heap) whatever
  their alignment; clang writes 32-byte-aligned types with `vmovaps ymm`,
  and JSC's Lexer at a 16-byte address was a SIGBUS (code 3, pc = fault
  address) in the first script. No measurable heap growth at idle.
- `ps5browser.c`: `TMPDIR=/data/ps5webkit/home/tmp` (no /tmp for a title;
  the compiler writes its NFAs there); `JSC_useJIT=false` (the default for
  the title already, made explicit).
- `titlesys.c`: `pathconf` (libc++ `current_path`, now linked), `lstat`
  (EPERM in a title; `stat` works) and `remove` (fails in a title): without
  them WebKit's `deleteFile` failed and every compile left ~2500
  `SerializedNFA-*` files and the replaced list behind.
- TLS in the title is untried: the update downloads in the child process
  (libsoup, as the NetworkProcess does). The title no longer runs any
  JavaScript itself.

## Compile and update in a child process

WebKit's compile of this list peaks at several hundred MB. In the title,
whose heap is dlmalloc over direct memory (`dheap.c`), ~420 MB of that
stayed mapped until the browser restarted, and an update's JavaScriptCore
conversion added ~400 MB more. Both now run in `ps5wk-child`, started as a
*task payload*: the title writes `/data/ps5webkit/adblock/work/job.ini`
and calls `wpe_process_manager_ps5_run_task(manager, "adblock", <job>)`,
which uploads the child ELF to elfldr exactly as for a WebProcess (same
serialized, retried path) with the header
`PS5WK-LAUNCH 1 adblock 0 <job file>`. `ps5wk-child.cpp`'s `main` sees the
type and runs `ps5wk_adblock_job` (`apps/child/adblock-job.c`) instead of a
WebKit process: no IPC socket, no rendezvous.

- **Same format, same code:** the child compiles with WebKit's public
  `WebKitUserContentFilterStore` into the title's store directory under
  the slot the title chose (the one not in use). It is the same WebKit
  build as the title (packaged together), so the file's
  `ContentRuleListStore::CurrentContentRuleListFileVersion` matches and the
  title only loads it (`webkit_user_content_filter_store_load`: 2 ms).
  The child `chmod`s what it writes to 0666 (payloads run as uid 0, the
  title as uid 1; the title deletes the old slot later).
- **Bundled list:** a payload can't see the title's `/app0`, so the title
  copies `/app0/adblock/adblock-rules.json` (and the converter) into
  `work/` on a short thread (streamed, 256 KiB buffer) before the launch.
- **Progress:** the child's output comes back on the elfldr connection and
  is relayed to the log as `[adblock] [adblock-job] ...`;
  `[adblock-job] status: <text>` lines become the *Lists* card's status,
  `[adblock-job] phase: compiling` switches `updating` to `compiling` in
  `/settings.json`.
- **Result:** the child writes `work/result.ini` last (ok / uptodate /
  failed, message, compile ms, peak RSS). The title reads it when the
  child's output closes. No result (crash: the crash guard ends the child,
  a kill, the 20-minute limit) = failure, and the list in use stays. An
  update's new list is saved only after it compiled, so a list that
  doesn't compile is never picked at the next start.
- **One job at a time** (`jobRunning`); an update asked for during a job
  answers "the list is being updated or compiled".
- **The title going away:** the child watches its stdin (the elfldr
  connection) and ends when it closes, so a restarted title never has two
  children writing the same slot. Checked: the title stopped 15 s into a
  compile; the child's temporary file stopped growing and no slot was
  written.
- **Temporary files:** `TMPDIR` for the child is `work/tmp` (WebKit's
  `SerializedNFA-*`); WebKit writes `ContentRuleList-XXXXXX` next to the
  result until it is complete. A payload can list directories, so each job
  first removes leftovers of an ended one (both places), and clears
  `work/tmp` at the end; the title removes the job files after each job
  and at start.
- The child runs at `setpriority(10)` (one busy core for about a minute).

**Measured on the console** (2026-11-02, WC build, log-only, 119449 rules):

| | title direct heap (before / during / after) | child | total |
| --- | --- | --- | --- |
| first-start compile (Wikipedia loading at the same time) | 26.8 / 26.8 / 26.8 MB (peak 26.8) | peak RSS 472 MB, compile 52.7 s | 55.3 s launch to list in use; load 2 ms |
| forced update (`POST /adblock?do=update`) | 26.8 / 26.8 / 26.8 MB (peak 26.8) | peak RSS 477 MB (408 MB after the conversion); manifest 0.26 s, 10 files (4.7 MB) 3.4 s, conversion 3.1 s, compile 56.8 s | 64.9 s; same sha256 as node (`04185856...`) |
| before (compile in the title) | 27 / 557-691 / 370-450 MB (kept until restart) | - | 44-67 s; +400 MB more during an update's conversion |

Flexible memory stayed at 282 MB in the title throughout. Page loads
during the compile (BBC News, MDN, Wikipedia, twice each, warm cache):
1.0-1.6 s, against 1.1-2.1 s with nothing running: no slowdown measured.
The upload of the child ELF (130 MB) to elfldr took 0.25-0.28 s.

**Was it only the allocator?** Measured once with the old in-title
compile followed by `dheap_trim()` (dlmalloc `mspace_trim` on every
arena): the heap went from 370.8 MB after the compile to 134.8 MB, against
26.8 MB before it. So a trim gives back about two thirds, but ~110 MB stays
(blocks still in use scattered over 2 MB segments), and the 557-691 MB
peak in the title remains. Not used; the child process returns all of it.

## Options if it is too heavy

The compile cost (time; memory now only in the child) scales with the rule
count. Easyprivacy is 56313 of the 119449 rules (trackers, not ads);
easylist 46189. Leaving easyprivacy out would roughly halve compile time
and memory; it would be a list of rulesets in `rulesetsFromManifest` (not
done: the owner asked for the default set).

## Not in phase 1

Cosmetic filtering (element hiding: `css-display-none` rules from uBO
Lite's `scripting/` data), scriptlets, redirects to surrogate resources
(needs WebKit's active-action patterns, web extensions only), strict blocking
of whole sites, a count of blocked requests in the UI process (WebKit has no
GLib signal for it; the WebProcess logs each block), and a pad shortcut.
