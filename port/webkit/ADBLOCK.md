# Ad blocker

The browser blocks ads and trackers with **uBlock Origin Lite**'s rules
(github.com/uBlockOrigin/uBOL-home, GPL-3.0, like this project). Phase 1:
its network rules, applied by WebKit's own content blocker
(`WebKitUserContentFilterStore`, `ENABLE_CONTENT_EXTENSIONS`, on in both
builds). Phase 2 (*Phase 2: cosmetic filters and scriptlets* below): its
element hiding and its scriptlets, from the same release and the same
update, through the same content blocker and per-site WebKit user scripts
and style sheets. No site-specific code: everything comes from uBO Lite's
data.

Files:

| | |
| --- | --- |
| `adblock/dnr-to-webkit.js` | the converter: uBO Lite's declarativeNetRequest (DNR) rules to WebKit content-blocker JSON. Plain JavaScript, no dependencies; the same file runs under node on the Mac and in JavaScriptCore on the console (in the child below) |
| `../../tools/ubol-convert.js` | Mac tool: downloads the pinned uBO Lite release, checks its sha256 pins, converts, writes `toolchain/adblock/adblock-rules.json` + `adblock-info.ini` (never committed) |
| `apps/title/adblock.c` | the title side: loading the compiled list, the filter on the web view, the per-site allow list, starting compile and update jobs |
| `apps/child/adblock-job.c` | the compile and the update, in `ps5wk-child` started as a task payload (*Compile and update in a child process* below) |
| `wpe-ps5/WPEProcessManagerPS5.cpp` | `wpe_process_manager_ps5_run_task`: starts the child ELF in another mode through elfldr, like the WebProcesses |
| `apps/title/package.sh` | puts `adblock-rules.json`, `adblock-scripting.txt`, `adblock-info.ini`, `dnr-to-webkit.js` and `ubol-scripting.js` in the title (`/app0/adblock`), so blocking works offline and on first start |
| `adblock/ubol-scripting.js`, `apps/title/adblock-scripting.c` | phase 2 (below) |
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
- Both switches cover phase 2 as well: with blocking off, or on an allowed
  site, there are no cosmetic filters and no scriptlets either (the generic
  cosmetic rules are part of the filter; the per-site user content is
  removed while an allowed site is shown).
  There is no pad shortcut: every button is taken (WPEDisplayPS5.cpp:
  D-pad, X, O, Square, Triangle, L1/R1, L2/R2, Options, R3, L3, touchpad),
  and R3 + one button press is short.
- **Lists:** the *Lists* card shows the uBO Lite version, the rule count and
  where the list came from (bundled or downloaded), the cosmetic filter and
  scriptlet counts (and those on the page shown), and has **Update lists
  now**.
- API (same PIN and origin rules as the other settings routes):
  `POST /adblock?do=update`, `POST /adblock?do=allow&site=<host>`,
  `POST /adblock?do=block&site=<host>`; `/settings.json` has an `adblock`
  member (enabled, filterOn, site, sites, list, scripting, status,
  updating, compiling, lastCheck).

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

The pins: the release version and the sha256 of `manifest.json`, of each
ruleset file at that tag (null = no regex ruleset in that release) and of
each phase 2 file (the details files, `rulesets/scripting/...`,
`css-procedural-api.js`).
Pinned now: **uBO Lite 2026.1026.1812**.

## Conversion

Rulesets: the ones uBO Lite enables by default (`manifest.json`
`declarative_net_request.rule_resources[].enabled`): ublock-filters,
easylist, easyprivacy, pgl, ublock-badware, urlhaus-full, each with its
`regex/` part if the release has one. Not used: the strictblock and urlskip
rulesets (the cosmetic filters and scriptlets of these rulesets: phase 2).

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
| first-start compile of the bundled list (copied from `/app0` in 0.5 s) | 26.8 / 26.8 / 26.8 MB | peak RSS 472 MB, compile 51.5 s | 53.9 s |
| forced update (`POST /adblock?do=update`) | 26.8 / 26.8 / 26.8 MB (peak 26.8) | peak RSS 477 MB (408 MB after the conversion); manifest 0.26 s, 10 files (4.7 MB) 3.4 s, conversion 3.1 s, compile 56.8 s | 64.9 s; same sha256 as node (`04185856...`) |
| before (compile in the title) | 27 / 557-691 / 370-450 MB (kept until restart) | - | 44-67 s; +400 MB more during an update's conversion |

Flexible memory stayed at 282 MB in the title throughout. Failure check:
a newer downloaded list that WebKit refuses (an unsupported regex) gave
`compile job (the source list changed) ended after 2.5 s: failed: ...`,
the Settings status said so and the list in use stayed (filter on, same
version); the next job removed the temporary file it had left (since then
the child removes it at once). Page loads
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

## Phase 2: cosmetic filters and scriptlets

Phase 2 adds uBO Lite's element hiding and scriptlets, from the same pinned
release and the same update job as the network rules, with no site-specific
code: every selector, every scriptlet and every site they apply to comes
from uBO Lite's `rulesets/scripting/` data (its maintainers' fixes arrive
with a list update). Files:

| | |
| --- | --- |
| `adblock/ubol-scripting.js` | the converter for uBO Lite's scripting data (node on the Mac, JavaScriptCore in the update child, like `dnr-to-webkit.js`) |
| `apps/title/adblock-scripting.c` | the title's per-site lookup and the scripts it builds (plain GLib) |
| `apps/title/adblock.c` | registers the results as WebKit user content (decide-policy), the setting and the allow list |
| `test/adblock/phase2-check.js` | `test-onload.js` for measurements: load times, element counts and, on YouTube, ad containers shown and the player's ad state |

### uBO Lite's data

In uBOL-home, `chromium/rulesets/` at the release tag:

| file | what | default rulesets, 2026.1026.1812 |
| --- | --- | --- |
| `scripting/generic/<id>.js` | generic cosmetic filters: a "lowly generic" map (hash of a class or id -> selectors), a "highly generic" selector list, and per-site exceptions (`#@#`) | 13946 selectors (easylist 13634), 374 with exceptions |
| `generic-details.json` | sites where generic cosmetic filtering is off (`$generichide`) | 1274 sites |
| `scripting/specific/<id>.json` | per-site cosmetic filters: selectors, selector lists, a host index (names and `name.*` entities, sorted by length), a few regexes; negative references are exceptions; a selector starting with `{` is JSON: a style (`cssable`) or a procedural filter | 16682 filters (915 procedural) on 12746 host entries |
| `scripting/scriptlet/{main,isolated}/<id>.js` | per ruleset and world: host index (`*` = every site, `name.*`, `site>>` = frames below that site), argument-list references (negative = exception, `0` = `#@#+js()`), regexes, argument lists, arguments | 4109 MAIN + 1143 ISOLATED calls on 24759 host entries |
| `scripting/scriptlet/{main,isolated}/scriptlet-begin.js` | the scriptlet library: every scriptlet function and its helpers (`json-prune`, `set-constant`, `trusted-replace-xhr-response`, ...) | 188 KB (97 declarations) + 56 KB (38) |
| `ruleset-details.json`, `scriptlet-details.json` | which rulesets have which files and worlds | |
| `../js/scripting/css-procedural-api.js` | uBO Lite's procedural filterer (`:has-text`, `:upward`, `:matches-css`, ...) | 28 KB |

uBO Lite itself registers these as content scripts: `css-generic.js` (a
MutationObserver survey of class and id names, then a style sheet),
`css-specific.js` (per-site selectors at document start), and per world the
library, every ruleset bundle and `scriptlet-end.js` in every frame of every
site (about 700 KB per frame in MAIN), each bundle finding its calls for the
frame's host at run time.

### How it works here

Everything is converted once (on the Mac for the bundled list, in the
update child on the console), only read as data: JSON arrays on known lines
of uBO Lite's generated scripts, and the library split at its top-level
`function`/`class` declarations by a small lexer (strings, templates,
regexes and comments skipped) that also records which declarations each
one names. Two checks guard the split (the lexer's brace matching, and uBO
Lite's layout: each declaration starts a line and ends with a `}` alone on
one); if either fails, or the library has top-level code the converter
doesn't know, the whole library goes into each script instead (correct,
only bigger).

1. **Generic cosmetic filters: WebKit content-blocker rules**, at the front
   of the same list as the network rules (one compile, one filter, so the
   Settings switch and the per-site allow list cover them). Each selector
   is a `css-display-none` rule with `url-filter: .*`. WebKit puts every
   such rule that has no condition and comes before the list's first
   ignore rule into **one precompiled style sheet** shared by all documents
   (`ContentExtension::compileGlobalDisplayNoneStyleSheet`), applied to every
   frame as a user style sheet; class and id selectors land in WebKit's
   hashed rule buckets, so like uBO Lite's survey only elements carrying
   that class or id ever test them. The 374 selectors with exceptions get
   `unless-top-url` (the excepted sites and entities as top-URL regexes) and
   `resource-type: document` (checked once per document, not per
   subresource). Then one `ignore-previous-rules` rule with `if-top-url`
   for the 1274 generichide sites and the browser's own pages (`file:` =
   the start page, `http://127.0.0.1:9040/` = Settings): it only stops the
   cosmetic rules before it; the network rules come after it.
2. **Per-site cosmetic filters and scriptlets: WebKit user content,
   registered per host.** The converter merges all rulesets into one host
   index (`adblock-scripting.txt`, 2.5 MB, next to `adblock-rules.json`).
   The title reads it at start into one buffer (an index of pointers into
   it). For every frame about to load, `decide-policy` (navigation action,
   main frame and subframes) looks up the frame's host as uBO Lite does:
   the host, each parent domain, `*`, then the entities (`name.*`); for a
   subframe also the top-level site's `site>>` entries; the regexes; then
   the exceptions (scriptlet exceptions per call, `#@#+js()` per world,
   cosmetic exceptions across rulesets). For that host it registers on the
   shared `WebKitUserContentManager`, each with an allow list of
   `http://<host>/*` and `https://<host>/*`, all frames:
   - a **user style sheet** (user level) with its cosmetic filters, one
     rule per line (`selector{display:none!important}`, the style filters
     as written, `@media` ones as uBO Lite's `addDeclaratives` writes them);
   - a **main-world user script** at document start
     (`webkit_user_script_new`: WebKit's page content world, the same world
     as the page's scripts, so `json-prune` replaces the page's own
     `JSON.parse`, `fetch`, `XMLHttpRequest` before the page's first script
     runs) with only the library declarations its calls need (and what
     they name) and the calls, each in a `try` block as uBO Lite runs them;
   - an **isolated-world user script** (`webkit_user_script_new_for_world`,
     world `ubol`) with the ISOLATED calls and, where the site has
     procedural filters, uBO Lite's `css-procedural-api.js` with a small
     shim (`cssAPI.insert` as an adopted style sheet, as uBO Lite's own
     immediate path does) and those filters.
   The scriptlets for every site (`*`: currently uBO Lite's ClickFix
   clipboard guard) are one main-world script for all http(s) frames with
   the hosts that have their own script on its block list (their script
   contains the generic calls minus their exceptions). The user content is
   added while WebKit waits for the policy answer: the WebProcess gets it
   before the frame's request goes out, so it is there at document start.
   Hosts stay registered (32, least recently used out), so a site costs a
   lookup and the IPC once per session.
3. **Off switches**: the Settings switch and the allow list remove all of
   it (`scriptingOff`) while an allowed site is shown or blocking is off;
   the next page sets it up again. The cosmetic rules in the filter go
   with the filter, as in phase 1. Test knobs:
   `/data/ps5webkit/adblock-scripting-off` (phase 1 only, read at start),
   `/data/ps5webkit/adblock-scripting-debug` (a scriptlet call that throws
   logs `[ubol] <name> threw` to the page console).

**Update:** the update child fetches the three details files and the files
they name at the same release tag, converts them with `ubol-scripting.js`
in the same JavaScriptCore context, writes one list (cosmetic rules, then
network rules), compiles it, and saves `adblock-scripting.txt` with the
rules before the info file; any failure (a download, the conversion, the
compile) keeps the list and the site data in use. The list's converter
version is now `dnr-to-webkit VERSION * 1000 + ubol-scripting VERSION`
(1001), so a phase-1 list (1) is older than the bundled one, and a
downloaded phase-1 list without site data gets the bundled site data and
an update check 2 minutes after start.

### What it covers

Everything in uBO Lite's default rulesets that has a scripting part
(ublock-filters, easylist, easyprivacy, ublock-badware; pgl and
urlhaus-full have none): generic and per-site element hiding, style
filters, procedural filters, the MAIN and ISOLATED scriptlets with their
exceptions, entities and `site>>` entries, and generichide.

**YouTube** (www.youtube.com, from uBO Lite's data as it stands at
2026.1103.1743): 26 main-world calls (`json-prune-fetch-response` and
`json-prune-xhr-response` on `/player`, `/playlist` and the shorts feed,
`trusted-replace-xhr-response` / `trusted-replace-fetch-response` on
`adPlacements` / `adSlots`, `set-constant` on
`ytInitialPlayerResponse.{playerAds,adPlacements,adSlots}`, `json-prune` for
shorts ads, `trusted-prevent-dom-bypass` so an iframe can't hand the page a
clean `fetch` / `JSON.parse`, `trusted-edit-inbound-object` on
`JSON.stringify` (the request-side tricks against server-stitched ads),
`adjust-setTimeout`), 2 isolated-world calls (`replace-node-text` on the
player bootstrap script, `remove-node-text`), 18 CSS rules (feed, sidebar,
search and shorts ad slots, masthead ad, merch shelf, product stickers, the
"ad blockers" toast) and 1 procedural filter. Scripts 67.5 KB (main) +
44.4 KB (isolated, 28 KB of it uBO Lite's procedural filterer) + 1.9 KB CSS;
uBO Lite itself injects about 700 KB (MAIN) + 490 KB (ISOLATED) into every
frame.

On the console (log-only runs, `test/adblock/phase2-check.js`):

| | phase 2 off | phase 2 on |
| --- | --- | --- |
| `ytInitialPlayerResponse` of two music videos | `adPlacements` 1 (`START@0`: a pre-roll scheduled at 0 s) | no `adPlacements`, no `playerAds` |
| ad containers with a size (watch and search pages) | 0 | 0 |
| player in its ad state (`ad-showing`, sampled every 0.5 s for 30 s) | never | never |
| playback | 1080p-4K, frames advancing | the same (hardware video path, MSE), 26 calls, none threw (`adblock-scripting-debug`) |

So the classic pre-roll is removed where YouTube schedules it (the
placement is gone from the player's data before the player reads it); in
these logged-out runs YouTube did not actually play an ad even with all
blocking off (the scheduled pre-roll never started, and no ad slot was
filled), so the visible difference on the TV could not be shown here.
**Server-stitched ads** (the ad inside the video stream): uBO Lite's
`trusted-edit-inbound-object` / `replace-node-text` calls run, but whether
they work depends on what YouTube serves; not observed in these runs; best
effort, and it is the part uBO Lite's maintainers change most often (an
update brings their changes).

Other pages: all of `test/adblock/urls.txt` load and work with phase 2 on
(The Verge 2 + 3 calls and 4 rules, CNN 9 calls and 15 rules, reddit 2 calls,
11 rules and 4 procedural, Daily Mail 2 calls, 14 rules, 2 procedural, The
Independent 2 calls and 10 rules, BBC and Apple none beyond the generic
call, MDN 2 rules and the generic call excepted). The browser's own pages
(start page, Settings) get nothing: `test/adblock/cosmetic.html` over http
hides 4 of 4 generic test elements (global style sheet and an
exception-carrying rule) and 0 of 2 controls; the same file as a `file:`
page hides none.

### Measured on the console (2026-11-04, WC build, log-only)

**Update and compile** (2026.1103.1743, the daily check):

| | phase 1 list | phase 1 + 2 list |
| --- | --- | --- |
| files | 10 (4.7 MB) | + 20 scripting files (2.7 MB): downloaded and converted in 8.6 s |
| rules | 122080 | 136036 (13956 cosmetic) |
| JSON / compiled file | 26.2 MB / 76.4 MB | 27.6 MB / 78.7 MB |
| compile in the child | 53.5 s, peak RSS 483 MB | 53.1 s, peak RSS 507 MB |

The universal cosmetic rules add no NFA work (WebKit keeps them out of the
DFA). The same conversion in node and in the console's child gives the same
bytes (checked in node with the child's splicing: same sha256 as
`ubol-convert.js`).

**Title:** site data read in 8-9 ms at start (2.5 MB kept as one buffer +
0.9 MB of index in the title's heap); a lookup takes 0.03-0.13 ms per frame
host; the generic script is 17.5 KB.

**WebProcess memory** (peak RSS 60 s into `cosmetic.html`, a small page):
169.4 MB with blocking off, 187.4 MB with phase 1 + the generic cosmetic
rules, 188.0 MB with everything (per-site scripts and the generic
scriptlet: +0.6 MB). Phase 1 alone measured +10 MB earlier (example.com,
177.2 -> 187.2 MB, an older build), so the generic cosmetic style sheet
(about 13.5k selectors, parsed once per WebProcess, a rule set per
document) costs roughly 8 MB. On heavy pages the WebProcess peak stayed in
the same range (707/814 MB at 60/120 s on, 689/815 MB off).

**Long-feed scrolling** (`[render]` update cost while the test script
scrolls 24 px per 16 ms for 20 s; averages over the 5 s windows with
scrolling; two passes per run, first and second visit):

| | feedlayers.html (500 items, 9.5k elements, http) | YouTube search results (14-16k elements) |
| --- | --- | --- |
| blocking off | 9.3 / 12.5 ms avg, p95 14.8 / 19.6 ms; rAF 1197-1198 of 1200 | 11.8 / 10.7 ms, p95 49.8 / 48.7 ms; rAF 715 / 849 |
| phase 1 + generic cosmetics (per-site off) | 9.3 / 12.8 ms, p95 14.4 / 19.2 ms; rAF 1197-1202 | 9.5 ms, p95 42.0 ms; rAF 860 (one pass) |
| everything on (two runs) | 11.7 / 12.9 and 9.2 / 12.2 ms, p95 18.5 / 19.4 and 14.5 / 18.7 ms; rAF 1197-1200 | 13.7 / 10.6 and 12.3 / 8.9 ms, p95 68.8 / 45.0 and 50.0 / 30.8 ms; rAF 704-826 |

No difference above the run-to-run noise on either page: the generic
selectors are hashed by id, class and tag, so the elements of a feed don't
test them, and the per-site rules are a handful. YouTube's own scrolling
cost dominates its page.

**Page loads** (load event, phase 2 on / off, one pass each, warm and cold
caches mixed, so only gross differences would show): YouTube watch pages
2.9-3.2 s on (five loads) against 2.8-5.5 s off (six); the other test pages
moved both ways by up to a few seconds (The Verge 12.7 / 10.6 s, CNN 12.5 /
5.3 s, reddit 5.3 / 8.8 s, Apple 1.7 / 2.1 s, MDN 0.8 / 0.6 s), with the
same request counts: real-site noise, nothing systematic.

### Cost of the generic style sheet (2026-11-09)

Speedometer 3.1 was 12% faster with blocking off (README: "Speedometer").
Since then: generic selectors whose subject is only `*` (pseudo-classes at
most) are dropped by the converter (`costlyGeneric` in ubol-scripting.js,
VERSION 2: one selector, `#cmdTextDisplay ~ *`, which made every element
walk its earlier siblings and every insertion restyle the later ones);
WebKit keeps the user style rules while a document's user style sheets
are unchanged (patch 0105) and shares rule feature vectors between style
resolvers (patch 0106). Blocking still costs about 10%: matching the user
rules (3% of the main thread), merging their features into each new
resolver, building them once per new document (during loads), and the
network rules' URL matching (1%).

### Limits

- Not checked on the console yet: the per-site allow list with phase 2 (an
  allowed site getting no cosmetic filters or scriptlets). The code removes
  the per-site user content as phase 1 removes the filter. The setting was
  checked (started with blocking off: 0 of 4 test elements hidden, YouTube's
  pre-roll placement left in place).

- Frames match by their own host; `site>>` entries are checked against the
  top-level page only (uBO Lite checks every ancestor). `about:blank`,
  `srcdoc` and `data:` frames get no per-site scripts (uBO Lite's
  `matchOriginAsFallback`); the generic cosmetic rules apply to them.
- Generic cosmetic exceptions and generichide follow the top-level page
  (WebKit's top-URL conditions), not the frame.
- Scriptlet exceptions apply across rulesets (as in uBO), uBO Lite applies
  them per ruleset bundle.
- No surrogate resources: scriptlets that hand the page one of uBO Lite's
  web-accessible resources get none (`scriptletGlobals.warOrigin` is unset),
  as with phase 1's redirects.
- A host's user style sheet is added to the user content manager shared by
  the tabs; WebKit then recomputes the injected style sheets of every page
  (once per new host per session, hosts are kept).
- Procedural filters (915) run uBO Lite's filterer with its own
  MutationObserver, only on the sites that have them.
- uBO Lite's popup blocking (`scripting/popup`), strict blocking and
  urlskip data are not used.

## Not done

Redirects to surrogate resources
(needs WebKit's active-action patterns, web extensions only), strict blocking
of whole sites, a count of blocked requests in the UI process (WebKit has no
GLib signal for it; the WebProcess logs each block), and a pad shortcut.
