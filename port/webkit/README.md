# WPE WebKit for the PS5 (Milestones 1 and 2)

WebKit 15486a0178, `PORT=WPE`, built with ps5-payload-sdk for a jailbroken PS5
on 13.60. Goal of Milestone 1 (FEASIBILITY §1a, §4.8): browse on the TV with
pages painted on the CPU.

- **Title PPSA99020 = UIProcess** (`apps/title/ps5browser.c`) with the PS5 WPE
  Platform backend (`wpe-ps5/`): the page on the TV through ps5-opengl's EGL
  window surface, DualSense input, the system keyboard (libSceImeDialog).
- **Payload `ps5wk-child.elf` = WebProcess and NetworkProcess** (one
  multi-call ELF, `apps/child/ps5wk-child.cpp`), started by the title through
  the local elfldr with Spike 8's reversed rendezvous (`wpe-ps5/PS5LaunchProtocol.h`).
  8 GB of flexible memory, JavaScriptCore with the JIT (patch 0030, *JIT* below), Skia on the CPU, frames sent
  to the UIProcess as shared memory (WebKit's `RenderTargetSHMImage` path
  with `hardware-acceleration-policy = never`, no GL in payloads).
- **Payload `ps5wk-shot.elf`**: a UIProcess on WPE's headless display, for
  payload-only first light (loads a URL, reports a checksum and a thumbnail).

Milestone 2 step 1 (2026-10-28): `<video>`/`<audio>` and MSE on GStreamer,
software decoders in the WebProcess, audio played by the title (see
*Media* below). Step 2a (same day): hole-punch video, the title draws the
decoded frames under the page (see *Hole-punch video* below). Step 2b
(same day): H.264/HEVC/VP9 decoded in the title by Videodec2 (see *Step 2b*).

GPU compositing (2026-11-01/02, on by default since 2026-11-02): pages are
composited on the GPU by WebKit's WC compositor in the title, with
title-side (async) scrolling; the CPU path stays as the fallback and the
Settings switch (see *Settings* and COMPOSITING-PLAN.md).

No WebGL, WebXR, GPU process, Web Audio, WebRTC, speech, geolocation,
sandbox, introspection, docs, journald or libbacktrace.

## Send a link from another device

While the browser title runs, it serves a small link server on
**TCP 9040** of the PS5 (`http://192.168.1.50:9040/`). Anything sent to it
loads in the browser straight away. It is meant for the home LAN, and by
default it asks for no password.

- **A phone or any browser:** open `http://192.168.1.50:9040/`, type or
  paste an address (or search words), and press *Open on PS5*. The page also
  shows the address the PS5 is on now.
- **Bookmarklet (Safari, Chrome, Firefox on the Mac):** the same page has a
  *Send to PS5* link. Drag it to the bookmarks bar, then click it on any page.
  It opens a small pop-up at
  `http://<ps5>:9040/open?url=<this page>`, which loads the page on the PS5,
  shows "Opening on the PS5", and closes itself. A pop-up or navigation is
  used instead of `fetch`, because an https page may not fetch from a plain
  http address (mixed content). If the pop-up is blocked, the bookmarklet
  opens the same address in the current tab instead.
- Links load in the tab shown (see *Tabs, bookmarks, history*).
- **Terminal:** `tools/ps5open <url>` (bare hosts get `https://`, other words
  a DuckDuckGo search). With no argument it sends the front Safari tab, or
  Chrome's if Safari has none: if Safari or Chrome is the frontmost app, that
  one is used; `-s` or `-c` picks one. `ps5open --status` prints the page
  the PS5 is showing. For `ps5open` anywhere, link it onto your PATH:
  `ln -s "$PWD/tools/ps5open" /usr/local/bin/ps5open`.
  **If the browser isn't running**, ps5open starts it through the launcher
  payload (`tools/launch.sh PPSA99020 <url>`) and the title opens that link
  instead of the start page. Long links (over 200 characters) go in
  `/data/ps5webkit/open-url.txt` over FTP first; the title reads that file
  once at startup, then deletes it. ps5open never closes another title. If
  a game is running, it says so and stops.
- **curl / scripts:** `curl 'http://192.168.1.50:9040/open?url=example.com'`
  or `curl --data-urlencode url=https://example.com http://192.168.1.50:9040/open`
  (a plain body `curl -d https://example.com …/open` works too). The reply is
  `OK loading <url>` (HTTP 200), or `ERR <why>` (400, 403 for a wrong PIN, or
  503 while the browser is closing). Send `Accept: application/json` for
  `{"ok":true,"url":…}`. `GET /status` gives `{"ok":true,"uri":…}`. CORS is
  open (`Access-Control-Allow-Origin: *`, plus the Private Network Access
  header), so http pages and local tools can call it with `fetch`.
- **On the console** the title's control socket takes `open <url>` too
  (`/system_tmp/ps5wk/ps5browser-control.sock`, the socket that the launcher's
  `stop` uses).

### "Send to PS5" in the Share menu

- **Mac (Shortcuts app):** create a new shortcut and turn on *Show in Share
  Sheet* (in *Details*), with Safari web pages and URLs as input. Add the
  *Run Shell Script* action with input *Shortcut Input* passed as arguments,
  and the script `/path/to/ps5webkit/tools/ps5open "$1"`. Name the
  shortcut "Send to PS5". It then shows in Safari's Share menu, and under
  Services and Quick Actions. To give it a key, use System Settings >
  Keyboard > Keyboard Shortcuts > Services. Run without input, ps5open sends
  the front tab.
- **iPhone/iPad (Shortcuts):** create a new shortcut with *Show in Share
  Sheet* and input *URLs*. Add the action *Get Contents of URL* with the
  address `http://192.168.1.50:9040/open?url=` followed by *Shortcut Input*,
  URL-encoded by an *Encode Shortcut Input with URL encoding* action placed
  before it. Allow local network access the first time it runs. Share any
  page, then pick *Send to PS5*.
- **Android:** use the bookmarklet in Chrome's bookmarks, or keep the page
  `http://192.168.1.50:9040/` as a home-screen shortcut.

### What is allowed, and the details

- Only `http://`, `https://` and `file:///data/ps5webkit/…` URLs load (no `..`).
  `javascript:`, `data:`, `about:` and other files are refused. Control
  characters are refused, and spaces in a URL become `%20`.
- **View in VR:** a sent link leaves VR first. The headset is released, and
  the page loads on the TV, just as following a link from the page does.
- The server runs on its own thread and serves one request at a time. A
  request must arrive within 3 s and 64 KB, so a slow or oversized client
  gets 408 or 413, and the title never waits on it. The page loads on the
  GLib main loop. The title log shows `push from http <ip>: <url>`, then
  `push: loading <url>` and `load committed: <url>`.
- If the title is suspended (rest mode), it can't answer. ps5open then times
  out and tries the launcher, which reports that the title is running.
- **Knobs:** `/data/ps5webkit/push-pin.txt` (a PIN, e.g. `4821`) makes every
  request carry `pin=4821` (a query or form field, or an `X-PS5-PIN` header).
  The page then shows a PIN box, and `http://<ps5>:9040/?pin=4821` gives a
  bookmarklet with the PIN built in. `/data/ps5webkit/push-off` (a file):
  the server listens on 127.0.0.1 only, so nothing on the LAN reaches it but
  the in-app Settings still work (see *Settings*). Both files are read at
  startup.
- The port is fixed at 9040. The other ports in use are elfldr 9021, the
  launcher 9031, ftpsrv 2121 and the Mac's log server 9999.
- **Checked on the console** (2026-10-29, log only, no TV): with the browser
  stopped, ps5open started it with the link as the launch argument (the
  title gets it as argv[1], with an empty argv[0]), and with a 514-character
  link through `open-url.txt`. Both loaded at startup. Six links sent while
  it ran committed (GET, form POST, plain POST, JSON, the bookmarklet's
  `/open?url=` and `/?url=` with `Accept: text/html`). `javascript:` and
  `file:///etc/passwd` were refused. A 5 s trickle got 408, a 40 KB body
  413, random bytes 400, and 20 concurrent requests all got 200; the title
  kept running throughout. Not checked yet: the bookmarklet and Shortcuts in
  a real Safari/Chrome/iPhone, the PIN on the console (tested on the Mac
  with a host build of the same code), and `ps5open` with no argument
  (reading the front tab needs macOS Automation permission the first time).

### Log in with Chrome's cookies

`tools/ps5open --cookies [url]` copies Chrome's cookies for that site to the
PS5 browser, then opens the page there (no url: the front Chrome tab).
`tools/chrome-cookies.py <site>` reads them: cookies of the whole site (e.g.
`bbc.co.uk` and every subdomain, for `www.bbc.co.uk`), from the Chrome profile whose cookies changed
last (`PS5_CHROME_PROFILE="Profile 1"` picks another; `--list-profiles`).
Chrome encrypts cookie values with the "Chrome Safe Storage" key in the login
keychain, so macOS asks once ("Always Allow" stops the question). Values are
decrypted with `openssl` (AES-128-CBC, database version 24 drops the SHA-256
domain prefix). Nothing is written on the Mac.

- The title's `POST /cookies` takes Netscape cookies.txt lines (domain,
  subdomains flag, path, secure, expiry in Unix seconds or 0, name, value,
  and an optional SameSite None/Lax/Strict; `#HttpOnly_` before the domain)
  and adds them with `webkit_cookie_manager_add_cookie`, so no restart is
  needed. Persistent ones go to `browser/data/cookies.sqlite`; session
  cookies (expiry 0) last until the browser closes. The log shows
  `push from http <ip>: N cookies for <domains>`.
- Open by default, like the rest of the link server. Knob:
  `/data/ps5webkit/push-cookie-key.txt` makes `/cookies` require the same
  key in an `X-PS5-Key` header (`PS5_COOKIE_KEY` for ps5open).
- If the browser isn't running, ps5open starts it on its home page and waits
  for that page before sending: in one test, a cookie sent within a second
  or two of startup never reached the saved store.
- Some sites (Google/YouTube, banks, streaming services) tie a login to the
  device and may log out one side; log in on the PS5 itself for those.
- **Checked on the console** (2026-10-29): host-only and domain, secure and
  plain, HttpOnly, SameSite None/Lax cookies were all added and saved, and a
  page on the Mac received them in its `Cookie` header. The Chrome reader
  needs the keychain prompt, so the first real run is the owner's.

## Settings

The browser's main knobs are on one settings page. It works from the PS5 and
from any other device, and both use the same code (`apps/title/settings.c`,
served by the link server in `ps5browser.c`).

- **On the PS5:** press **R3** (click the right stick) to open Settings. Press
  R3 again, or O, or *Back to the page*, to go back to the page you were on.
  The start page also has a *Settings* tile. The page is
  `http://127.0.0.1:9040/settings` and is made for the D-pad: big buttons,
  a pink focus ring and the browser's dark palette. X picks an option. For the home page
  field, X or Triangle brings up the keyboard. R3 does nothing while you are
  in View in VR, because the headset owns the pad there.
- **On a Mac or phone:** open `http://192.168.1.50:9040/settings`. It
  follows the system's light or dark mode.

Each setting has a one-line explanation, its current value, its default and
a badge that says what applies it. When a saved change is not in use yet,
the badge says so and a banner offers the restart that applies it. Changes
are saved as soon as you press a button.

| Setting | Stored as | Applies |
| --- | --- | --- |
| JavaScript JIT: all tiers (default) / up to DFG / Baseline only / off | `child.env`: `PS5WK_JIT=1/dfg/baseline/0` | page restart |
| Painting threads: 1-8 (default 4; 1 = the old single-thread path) | `PS5WK_PAINT_THREADS` | page restart |
| Fast scrolling (default on) | `PS5WK_SCROLL_COPY=0` when off | page restart |
| Page memory limit: off / 2 / 3 (default) / 4 / 6 GB | `PS5WK_MEM_RESTART_MB` | page restart |
| Give back freed memory (default on) | `PS5WK_PAS_DECOMMIT=0` when off | page restart (the NetworkProcess at the next browser start) |
| Video under the page, i.e. hole punch (default on) | `PS5WK_HOLEPUNCH=0` when off | page restart |
| Offer VP9 video (default on; off makes YouTube send H.264) | `PS5WK_MEDIA_VP9=0` when off | page restart |
| GPU compositing (default on; includes async scrolling) | `/data/ps5webkit/no-compositing` when off | page restart |
| Hardware video decoding (default on) | `/data/ps5webkit/no-hwdec` when off | browser restart |
| Sound (default on; off = volume 0) | `/data/ps5webkit/audio-silent` when off | browser restart |
| Link server on the network (default on) | `/data/ps5webkit/push-off` when off | browser restart |
| Home page (empty = the built-in start page) | `/data/ps5webkit/home-url.txt` | next start |

The page also has these actions: *Clear cookies* and *Clear all website
data* (`webkit_website_data_manager_clear` on the browser's network session:
cookies, or every type, of any age), *Restart page* and *Restart browser*.
Clearing data, turning the link server off from another device and
restarting the browser each ask for a second press.

- **child.env** is rewritten in place. Comments, `JSC_*`, `WEBKIT_DEBUG` and
  any other lines stay as they were. The first line of a setting gets the
  new value and any repeats of it are dropped. A setting put back to its
  default loses its line. The new file goes to `child.env.tmp` and is then
  renamed over `child.env`, so a child never reads half a file.
- **Restart page** ends the WebProcess and loads the page shown in a new one,
  the same way the memory limit does (`memRestart`). The new process reads
  `child.env`. The log shows `[mem] WebProcess ended (Settings: restart
  page)` and `RESULT ps5browser page-restart PASS`. The NetworkProcess keeps
  running, and of these settings only *Give back freed memory* matters to
  it.
- **Restart browser** needs the launcher payload (`tools/launcher`). The
  title asks the launcher for `status`, and goes ahead only if the reply
  names PPSA99020. It writes the page you were on to `open-url.txt` (the page
  R3 left, when Settings itself is showing) and winds down exactly as for the
  launcher's `stop`: media stopped, audio ports and sockets closed, the
  WebProcess ended. After the wind-down it asks for `status` once more. Then
  `title_exit.c` sends the launcher `kill -9` and `launch PPSA99020` on one
  connection (`spike_title_relaunch`). The launcher reads the second line
  only once the kill is done. Its kill only ever targets the running app,
  which is this title, and its `launch` refuses to start while any app is
  running. If the launcher isn't running, the page says to restart the
  browser from the home screen, and nothing closes. On the Mac, the page
  waits until the browser answers again with a new start time.
- **GPU compositing** (`compositing`, on by default since 2026-11-02):
  pages are put together on the GPU in layers by WebKit's WC compositor
  inside the title, and the right stick scrolls them on the GPU (async
  scrolling, frame-locked stick). See COMPOSITING-PLAN.md. WebKit reads the
  setting whenever a page gets a new WebProcess, so *Restart page* applies
  it. The log shows `[wc] page N: WC compositor` and `[ascroll] drawing area
  N: async scrolling (... stick frame, overflow off, sync on; defaults)`.
  Off (`no-compositing`) paints on the CPU as before, and the CPU-only
  settings (painting threads, fast scrolling, video under the page) apply
  again. The title falls back to the CPU by itself when the GL scene host
  can't start (`[wc] compositing is on but the scene host is not
  available`), in a `WC=0` build, and for a page whose GL textures stay over
  the 160 MB cap (that page gets a new WebProcess that paints on the CPU;
  the next WebProcess composites again). The old opt-in file `compositing`
  is no longer needed and changes nothing.
- **Async scrolling** has no Settings entry. `/data/ps5webkit/wc-async-scroll`
  changes it: the word `off` (or `0`) turns it off (the WebProcess scrolls,
  as before); `key=value` words override the defaults, which are
  `stickframe=1 overflow=0 sync=1 syncreserve=5 earlyreply=1 glide=1 snap=1
  gestureend=40 gesturenodes=1 unconsumed=1 thumb=1 checkerboard=0`. A file left over
  from testing (e.g. `stickframe=1`) keeps working. COMPOSITING-PLAN.md
  (*Async scrolling*) has what each one does; `overflow=1` (carousels and
  overflow boxes scrolled in the title too) works on the test pages but
  hasn't been checked on the TV.
- **Link server off** (`push-off`): the server now listens on 127.0.0.1 only,
  instead of not running at all. R3 still opens Settings, but phones,
  `ps5open` and the bookmarklet can't reach the PS5. Turn it back on from the
  PS5's Settings, or delete the file over FTP.

API (the page uses it too). No CORS headers are sent. A request with an
`Origin` header must come from the same address it was sent to, and the
address must be an IP, `localhost`, a name without dots or a `.local`/`.lan`/`.home`
name. So no other web page can change settings, whether by posting a form
or through DNS rebinding. A PIN set in `push-pin.txt` applies as it does for
`/open`, except to the PS5 itself (127.0.0.1). The page then shows a PIN
box.

```sh
curl http://192.168.1.50:9040/settings.json        # every setting: value, default, inUse, restart, pending; uri, started, status
curl -d '{"jit":"dfg","paint_threads":2}' http://192.168.1.50:9040/settings   # or form fields: -d jit=dfg -d sound=0
curl -X POST 'http://192.168.1.50:9040/restart?scope=page'     # or scope=browser
curl -X POST 'http://192.168.1.50:9040/clear?what=cookies'     # or what=all
```

Settings are named by their id (`jit`, `paint_threads`, `scroll_copy`,
`mem_restart_mb`, `pas_decommit`, `holepunch`, `media_vp9`, `compositing`, `hwdec`, `sound`,
`link_server`, `home_url`) or by their child.env or file name
(`PS5WK_JIT=0`). Booleans take `1`/`0`, `true`/`false` or `on`/`off`, and
`null` means the default. A request with any invalid value changes nothing
(400). The reply is the full settings JSON plus a `"message"` such as
`Saved: JavaScript JIT. Restart the page to apply.` The results of clearing
data and restarting the page arrive later, in `"status"`. The log shows
`settings from http <ip>: ...` and `settings: ...` lines.

Not exposed, because they are test or debug knobs: `test-urls.txt`,
`test-onload.js`, `vr-sim`, `vr-probe`, `vr-selftest`, `debug-screenshot`,
`vdec-probe`, `vdec-refuse`, `page-texture`, `audio-null`, `audio-selftest`,
and the A/B and logging lines in child.env (`PS5WK_PAINT_ASYNC`,
`PS5WK_PAINT_RECORD`, `PS5WK_PAINT_BBH`, `PS5WK_PAINT_MIN_PX`,
`PS5WK_IDLE_GAP`, `PS5WK_SHM_READBACK`, `PS5WK_*_LOG`, `PS5WK_PROFILE`,
`PS5WK_FD_TRACE`, `PS5WK_DECODER_THREADS`, `PS5WK_MEDIA_AV1`, `PS5WK_HWDEC`,
`PS5WK_PROCESS_SWAPS`, `PS5WK_STILL_FRAME`, `PS5WK_MEDIA_SUSPEND_PREROLLED`,
`PS5WK_WC_LAYER_LOG`). They still work in child.env,
and the page leaves them alone. Also not exposed: the GPU compositing knobs
`wc-async-scroll` (above), `wc-gl-cap` (the GL texture cap in MB, default
160), `wc-scroll-log`, `wc-frame-log`, `wc-scene-stats`, `wc-fan-draws`,
`pad-scroll-test`, the decoder budget's `vdec-budget` (*Step 2b* below) and
`POST /restart?scope=crash` (kills the WebProcess, for crash-recovery tests).

The ad blocker has its own section in Settings and its own notes in
ADBLOCK.md; the performance overlay (Settings > *Performance overlay*,
L3 + R3) is described in OVERLAY.md.

**Checked on the console** (2026-10-29, log only, no TV): from the Mac,
`/settings.json` read the console's files. Setting JIT to DFG and painting
threads to 2 wrote exactly those two lines and kept the comments. *Restart
page*, with the PS5 browser showing its own `http://127.0.0.1:9040/settings`,
started a new WebProcess with `JIT: on (PS5WK_JIT=dfg …)` and `painting on 2
threads`, and the page reloaded from the title's own server with no stall.
A test cookie was added, and *Clear cookies* removed it from
`cookies.sqlite`. *Restart browser* wound down (0 audio streams, the audio
server stopped). The launcher log showed `kill -9`, `OK killed PPSA99020`
and `launch PPSA99020`, and the new title answered after about 4 s. With
the link server off, the restarted title listened on 127.0.0.1 only: the Mac
got no answer, and the PS5 browser loaded Settings. Requests with a foreign
`Origin` or `Host` got 403. Screen captures showed the page rendered by the
PS5's WebKit (the script ran, focus was on the selected JIT button). Not
checked yet: R3 and D-pad navigation on the TV, the PIN box, and *Clear all
website data* (skipped so the owner's site data was kept; same code path
as cookies).

## Tabs, bookmarks, history

The browser has tabs, bookmarks, a history and an address bar, all on the
pad. `apps/title/browserui.c` (tabs, the bar and menu, the pad while the
menu is open, the `/browser` API) and `apps/title/browserdata.c` (the
files).

**Pad.** On a page, every button does what it did before, except
**Options**, which now opens the **browser menu** instead of the URL prompt.
The D-pad's focus starts on the address field, so **Options, then X** (or
Triangle) brings up the system keyboard (URL layout, *Go*): an address, or
words to search for (DuckDuckGo Lite, as before).

| In the menu | |
| --- | --- |
| D-pad | move the focus (a violet-to-magenta outline): the bar's buttons (left/right), down into the list, up back to the bar; left/right in a list switch lists |
| X | pick: a button, a tab (switch to it), a bookmark or history entry (open it in this tab), *New tab*; with the pointer shown, what is under the pointer |
| left stick | the pointer (the focus follows it) |
| right stick | scroll the list |
| L1 / R1 | previous / next list: Tabs, Bookmarks, History |
| Square | close the tab / delete the bookmark / delete the history entry |
| R2 | open the bookmark or history entry in a new tab |
| Triangle | the keyboard for the address |
| O or Options | close the menu |
| L3 / R3 | close the menu, then home page / Settings |
| touchpad | close the menu, then View in VR |

The bar: Back, Forward, Reload (Stop while loading), Home, the address
field (the page's title and address), the tab count, Bookmark (☆ adds the
page shown, ★ removes it) and New tab (the home page). History has *Clear
all history* at the top (X twice). While a page loads (menu closed), the bar
shows at the top with the page's title, address and a progress line, and
goes 1.5 s after the load ends; the pad still drives the page.
`/data/ps5webkit/no-load-bar` (a file) turns that off.

**Tabs and memory.** The tab shown and the most recently used other tab
are **live**: each has its own WebKitWebView, all in the one WebProcess
(patch 0018 keeps one WebProcess for everything), with the same settings,
content filters (the ad blocker) and policies. Switching to a live tab is
instant: nothing reloads, a feed keeps its place and its state. A hidden
live tab's page is hidden as on a desktop browser: its view is unmapped
(page visibility `hidden`, `requestAnimationFrame` stops), its timers are
throttled and its CSS animations suspended (`HiddenPageDOMTimerThrottlingEnabled`
and `HiddenPageCSSAnimationSuspensionEnabled`, which WPE leaves off). Its
**media goes quiet**: a `<video>` or `<audio>` playing with sound is paused
when its tab is hidden (`[media] page hidden: pausing ...`, patch 0073), as
if the user had paused it, and stays paused when the tab is shown again
(press play); a hidden page can't start playing with sound by itself
either (the next item of a playlist: WebKit's
`RequireUserGestureToStartAudiblePlaybackWhenHidden`). Muted playback (feed
previews) is suspended while hidden and resumes when shown, as WebKit does.
Web Audio (`AudioContext`) is not paused. `PS5WK_BACKGROUND_AUDIO=1` in
`child.env` lets hidden tabs play on. With GPU compositing its
scene stays in the title but is never shown (patch 0072); on the CPU path
its frames are handed back undrawn.

A page shown again runs exactly as before it was hidden. WebKit keeps one
`DisplayRefreshMonitor` (the clock behind `requestAnimationFrame` and
every rendering update: IntersectionObserver, lazy images, scroll events)
for the whole WebProcess, and the WC drawing area that made it, the first
view's, drove it: once that view went (a discarded or closed tab) every
other page lost its clock for good (`rAF clock 0.0/s` in the `[render] wc`
lines), and rAF and IntersectionObserver only ran when something else
started a rendering update: images, videos and new feed items came late
on a page shown again and on every page loaded after it, restored tabs
included. Patch 0073 hands the monitor to the shown page's drawing area
on every switch and to another page's when its drawing area goes
(`[render] refresh clock (rAF): drawing area N drives it now`).

The other tabs are **discarded**: their address, title, scroll position and
WebKit's serialized session state (the back/forward list,
`webkit_web_view_get_session_state`, a few hundred bytes to a few KB; over
512 KB only the address is kept). Picking one restores its state into a
view and goes to its current entry, then scrolls back to where it was
(again while the page is still at the top, for 3 s). WebKit loads a
restored entry as a normal load (the HTTP cache revalidates,
`wasRestoredFromSession`), so it costs a page load, not memory (measured
2026-11-03, load started to finished: an encyclopedia article 0.97 s in a
new tab, 0.50 s restored; a news front page 1.05 s and 0.72 s). The view
comes from the least recently used hidden live tab, which is discarded
(`[tabs] tab N discarded (least recently used)`), so there are never more
views than live tabs.

- **How many live tabs:** `/data/ps5webkit/live-tabs` (a file holding 1-4,
  default 2; 1 = every switch reloads, as before). Read at start. Every
  change logs which tabs keep their pages: `[tabs] live (2 of 2 allowed)
  after a switch to a live tab: 1 hidden 0 s ago, 3 shown; 2 discarded of 4
  tabs`.
- **Switching** doesn't wait for the shown page's scroll position when
  that page stays live (up to 400 ms on a busy page before); it is read
  in the background and kept for when the tab is discarded.
- **Memory:** every 5 s the title reads the WebProcess's libpas memory (the
  memory limit's status file) and its own free flexible memory. Over 60% of
  the page memory limit (`PS5WK_MEM_RESTART_MB`, default 3 GB) or under
  96 MB free in the title, the hidden live tabs are discarded and only one
  live tab is allowed until the WebProcess is under 45% again (`[tabs]
  memory tight` / `fine again`). The limit itself restarts the WebProcess
  (every page in it): the hidden live tabs are discarded first, and when
  the WebProcess ends for any reason (crash, the GL texture cap's CPU
  fallback) they go back to their records and only the shown page reloads.
- The Tabs list marks live hidden tabs *kept live*.

A page that came from a form (POST) is never sent again by a switch or a
restore: the tab shows *This page came from a form* with a link that loads
the address without the form. A new tab starts with a fresh back/forward
list (an empty `about:blank` first entry, built in WebKit's session state
format, which Back skips). At most **12 tabs** (a 13th is refused with a
message). Links with `target=_blank` and `window.open()` after a click open
in a new tab; pop-ups without a click are dropped (`browser: pop-up without
a click blocked`). Links sent from another device (`/open`, ps5open) load in
the tab shown.

**Session restore.** The tabs, the one shown, their scroll positions and
back/forward lists are saved 2 s after a change and at the wind-down
(before the `about:blank` of the wind-down), and come back at the next
start; the tab shown loads, the others when picked. A link given at start
(ps5open while the browser is closed, *Restart browser*) opens in a new tab
after the restored ones, unless it is the page the restored tab shows. Test
runs (`test-urls.txt`) neither restore nor write the session.
`/data/ps5webkit/no-session-restore` (a file) starts with one tab (the
session is still written).

**Files** (`/data/ps5webkit`, JSON, each written to `<name>.tmp`, synced
and renamed over the old one; no sync, no telemetry):

| File | Contents |
| --- | --- |
| `session.json` | `{"version":1,"active":N,"tabs":[{"url","title","scrollX","scrollY","form","state"}]}`; `state`: the session state, base64; `form` 1: the page is a form's result |
| `bookmarks.json` | `{"version":1,"bookmarks":[{"url","title","added"}]}`, newest first, at most 500 |
| `history.json` | `{"version":1,"history":[{"url","title","last","visits"}]}`, newest first, at most 1000 pages; http/https only (not the start page, Settings or files); written 10 s after a change |
| `start-data.js` | `window.ps5wkData = {bookmarks, top}`: the start page's *Bookmarks* and *Most visited* (pages visited at least twice). A script file next to `start.html`, so only `file:` pages there can read it, not web pages |

The start page (`runtime/start.html`, still the home page and what a new
tab opens) has the Parallax logo and wordmark, a field for an address or
words to search for (as the bar's: an address with a dot and no spaces,
else DuckDuckGo Lite), then tiles for the bookmarks, the most visited pages
and the links, and the test pages as smaller pills. The D-pad moves between
tiles (up from the first row: the field, which brings up the keyboard); L1/R1
and X work as on any page. It needs `parallax-logo.png` next to it
(`runtime/`, uploaded with it by `test/media/make-media.sh`; without it the
wordmark stands alone). To update both by hand:
`curl -T port/webkit/runtime/start.html ftp://192.168.1.50:2121/data/ps5webkit/start.html`,
and the same for `parallax-logo.png`.

**The look** (*Parallax Browser*). `apps/title/brand.h` holds the palette,
sampled from the title's artwork (`sce_sys/icon0.png`, `pic0.png`: three
glass panes, violet, cyan-edged blue and magenta, on a near-black navy),
and the UI's radii and focus sizes; the start page has the same values as
CSS variables. Ink `#01001A`, indigo `#3202C2`, violet `#6D0EFD`, blue
`#156BFD`, cyan `#0EE3FE`, magenta `#FC0EC6`; surfaces base `#07061F`,
surface `#0D0B2A`, raised `#161440`, raised-hi `#211D56`, hairline
`#2B2766`; text `#F4F2FF`, `#B9B5E0`, `#8A86B8` (17:1, 9.8:1, 5.6:1 on the
surface). The bar and menu are deep-navy glass over the page; the focused
item is raised with a violet-to-magenta outline and a soft glow; the tab
shown is marked in cyan; the progress line is the violet-to-magenta
gradient. The menu's header has the Parallax mark (48 px, made from
icon0.png by `apps/title/brand-assets.py`, shipped by `package.sh` as
`/app0/ui/parallax-mark.bgra`) and wordmark, the lists between L1 and R1
key caps; rows have a tile with the site's initial in one of the brand's
tints; the footer shows what the buttons do for the list shown. Messages,
the View in VR hint and the TV notice (`PS5Text.cpp`) are the same navy
glass with a violet-to-magenta edge. The settings, crash and form pages
use the palette too.

**How it is drawn.** The title draws the bar and menu itself, like the
performance overlay, instead of a second WebView (our platform has one view
and the renderer one page; a second WebView would need a second page in
the renderer and likely a second WebProcess). `wpe-ps5/PS5UICanvas.cpp`
rasterizes rectangles and UTF-8 text on the CPU (FreeType, Noto Sans with
the symbol, math, CJK and script Noto fonts as fallbacks, no shaping;
rendered glyphs cached) into a premultiplied BGRA canvas;
`wpe_display_ps5_set_ui_layer()` hands a copy to `PS5Renderer`, which puts
it in a persistent GL buffer (a memcpy on the render thread, the page's own
shader reads it) and draws it over the page or the composited scene, under
the messages, the pointer and the performance overlay. So it is the same
with GPU compositing on or off, and not shown in the headset (View in VR
draws the TV notice instead). A menu frame is a 1920x1080 canvas (8 MB,
freed when the menu closes; the renderer keeps one 8 MB GL buffer after
the first use); the loading bar 1920x104 (0.8 MB). The drawing itself is
`apps/title/browserdraw.c`, from a plain state that `browserui.c` fills
(no WebKit calls), on the canvas's rounded rectangles, gradients, outlines,
glows, images and text; over a uniform area a translucent fill reuses the
last blend, so the menu costs less than the plainer one did (2.6 ms
against 3.6 ms on the Mac harness, best of 60; the loading bar 0.5 ms
against 0.4 ms). The log says what it costs on the console:
`browser: menu drawn in N ms (+ N ms to hand it over)` for the first frame,
`browser: menu closed (drawn N times, N ms on average, N ms at most)`.
The renderer fades a layer that appears in (150 ms) and one removed out
(120 ms, from the image last shown): its own copy of the page shader
times an opacity, frames at the vsync while it lasts, nothing for the main
thread to redraw; a change to a layer already shown is drawn at once.
`/data/ps5webkit/no-ui-fade` (a file) turns the fades off.
**Seeing it on the Mac:** `port/webkit/apps/title/uipreview/build.sh [dir]`
builds the canvas, the notices and browserdraw.c for the Mac (Homebrew's
glib, freetype, libpng; the Noto fonts in `toolchain/wpe-deps/dl/fonts`)
and renders every screen (the lists, focus on the bar and in a list, empty
lists, the loading bar, notices) over a page screenshot to PNG, with sample
data, and prints the draw cost.
`wpe_display_ps5_set_ui_mode()` gives the pad to the application while the
menu is open: every button to its handler (D-pad, L1/R1, L2/R2 repeat while
held), the left stick moves the pointer without page events, the right
stick and pointer moves to `wpe_display_ps5_set_ui_pointer_callback()`.
The browser's button handler chains View in VR's (`ps5wk_vr_buttons`).

Live tabs on the platform: `wpe_display_ps5_set_active_view()` picks the
view that is mapped (its page visible), focused, shown and given the pad;
once it has been called a new view stays unmapped until it is made active
(`WPEViewPS5`'s `can_be_mapped`). On the CPU path a hidden view's frames
go straight back undrawn, and the first frame after a view becomes active
replaces the whole screen whatever its damage; patch 0072 makes a page
shown again repaint whole (WPE's drawing area only resumed painting). With
GPU compositing the scene host shows the newest scene whose page is not
hidden (patch 0072: `DrawingAreaProxyWCPS5::viewIsBecomingVisible` /
`Invisible`, `[wc] scene N hidden` / `shown`), at once on a switch; a
hidden page's scene keeps applying its updates and its GL textures (they
count toward the 160 MB cap: over it (the ~1 s average), the title is asked
to let a hidden page go before the shown page loses tile coverage,
`ps5wk_wc_set_hidden_pages_callback`, and drops the least recently used
one; asked again 5 s later if still over). Until patch 0073 this was at
128 MB (the log line), which a heavy feed's texture pool alone passes
while it scrolls (129 MB average, 219 MB in one frame, 150 MB of it the
pool): the hidden tab went every time that page was shown, so only the
heavy page ever stayed live. ps5browser.c's handlers act on the
active view only (`ps5wk_set_active_web_view`: the load handlers, View in
VR's page commands, the ad blocker's reload).

**API** (link server, the settings' rules: no CORS, the same Host and
Origin, the PIN; for log-only tests, since the pad can't be driven
remotely; indexes are 1-based as in the menu):

```sh
curl http://192.168.1.50:9040/browser/state       # tabs, menu focus, back/forward, bookmarks, the last 50 history entries
curl -X POST 'http://192.168.1.50:9040/browser/tab?action=new&url=example.com'   # or action=switch&index=2, action=close&index=2
curl -X POST 'http://192.168.1.50:9040/browser/key?name=options'  # a menu press: options up down left right x o square triangle l1 r1 r2 l3 r3
curl -X POST 'http://192.168.1.50:9040/browser/address?text=ps5+vr2'  # as typed on the keyboard
curl -X POST 'http://192.168.1.50:9040/browser/bookmark'          # toggle the page shown (or url=...)
curl -X POST 'http://192.168.1.50:9040/browser/history?action=clear'
curl -X POST 'http://192.168.1.50:9040/browser/shot?name=menu'    # screen to /data/ps5webkit/shots/ui-menu.ppm (960x540)
curl -X POST http://192.168.1.50:9040/browser/save                # write session/bookmarks/history now
```

The log shows `browser: ...` lines (new tab, `tab N: back to <url>
(scroll x,y)`, `scrolled back to`, `came from a form`, menu open/closed,
bookmarks) and `browser API from ...`. Test page: `test/browser/post.html`
(posts a form once, for the form-result case).

**Checked on the console** (2026-11-02, log only, GPU compositing on): new
tab, switch and close through the API and simulated menu keys; screenshots
of the menu (tabs, history, focus ring) and of the loading bar; a new tab's
Back skips its empty first entry while a restored tab keeps its own back
list; a page opened at an anchor (scroll 3207) came back at 3207 after two
switches; six tabs of news, forum and code sites cycled 6 times with one
WebProcess throughout (its peak RSS 582 MB, that of the heaviest page; the
title's free flexible memory stayed at 279 MB and its direct memory went
with the page shown, as before); the wind-down saved 6 tabs, the next start
restored them (tab 4 shown) and opened the start link in a 7th; a form
result tab showed *This page came from a form* on every return, with
nothing posted. With GPU compositing off (`no-compositing`) the menu draws
the same over the CPU-painted page, and the start page showed its
*Bookmarks* and *Most visited* (start-data.js is loaded with a fresh query:
the WebProcess's memory cache kept the first copy). The open menu costs the
title ~16 MB of heap (its canvas and the copy on its way to the renderer),
given back when it closes. Not checked yet: the pad on the TV (focus,
pointer, repeat, the keyboard from the address field).

**Live tabs checked on the console** (2026-11-03, log only): a feed test
page (`test/paint/wcfeed.html?videos=0`, scrolling itself with timers and
`requestAnimationFrame`) and a long encyclopedia article as the two live
tabs, GPU compositing on. Switching between them took 0.1 ms in the title
(`[tabs] tab N shown live`), and the screen showed the other page's scene
at once (screenshot 2 s after the switch). Hidden, the feed's rAF went
from 60/s to 0 and its timer-driven scrolling from 150 px/s to 3 px/s
(timer throttling); the WebProcess's CPU was 0.14 cores with the feed shown
and 0.01 with the article shown and the feed hidden (0.05-0.06 with one
static page). Memory with both live: one WebProcess, peak RSS 276 MB,
libpas 316 MB committed; the hidden feed's scene held 49 MB of GL textures
in the title (73 -> 24 MB when it was closed), which is most of the
title's extra direct memory (699-714 vs 658 MB). A third tab discarded the
least recently used one and reused its view. GPU compositing off: the
first switch back to a live tab showed the old page until the repaint fix
above; with it, each switch showed the right page.

## Build

```sh
port/webkit/build.sh                     # src shim configure build
port/webkit/build.sh src                 # clone + patches (FRESH_SRC=1 re-applies)
port/webkit/build.sh configure           # cmake (log: toolchain/wpe-webkit/configure.log)
NINJA_TARGETS="ps5wk-child ps5wk-shot ps5browser" port/webkit/build.sh build
port/webkit/build.sh export-patches      # after editing the clone: commits -> patches/
port/webkit/apps/title/package.sh        # title folder -> toolchain/wpe-webkit/titles/PPSA99020
port/webkit/test/run-shot.sh [url]       # payload-only first light on the console

# CPU-only build (no WC compositor), in toolchain/wpe-webkit/build-cpu:
WC=0 NINJA_TARGETS="ps5browser ps5wk-child" port/webkit/build.sh build
BLD=$PWD/toolchain/wpe-webkit/build-cpu port/webkit/apps/title/package.sh
```

- **GPU compositing is built in by default** (since 2026-11-02): `WC=1`
  (`USE_TEXTURE_MAPPER`, `USE_GRAPHICS_LAYER_WC`, patches 0037-0067) is the
  default, and `ENABLE_MEDIA_CONTROLS_CONTEXT_MENUS` /
  `ENABLE_VIDEO_USES_ELEMENT_FULLSCREEN` are off in every build (they used
  to be passed in `EXTRA_CMAKE_ARGS`). The title still paints on the CPU at
  run time when compositing is off in Settings, the scene host isn't
  available or a page goes over the GL texture cap (*Settings* above).
  `WC=0` builds without the compositor; its build dir defaults to
  `toolchain/wpe-webkit/build-cpu` (logs there too), so the two
  configurations don't rebuild each other. `build` reconfigures by itself
  when the build dir was configured with the other `WC` setting.
- Second worktree: `toolchain/wpe-webkit/src-wc` (branch `ps5-wc`) and
  `build-wc` were where GPU compositing was developed; `ps5-wc` was merged
  into `ps5` on 2026-11-02 and the worktree is kept for parallel work (at
  `ps5`'s commits; build it with `SRC=$PWD/toolchain/wpe-webkit/src-wc
  BLD=$PWD/toolchain/wpe-webkit/build-wc LOGDIR=$PWD/toolchain/wpe-webkit/build-wc
  port/webkit/build.sh build`). Commits made there go to `ps5` and are
  exported from it.

- Source: `toolchain/wpe-webkit/src`, a clone of `ref/WebKit` that uses its
  object store through `alternates` (ref/WebKit is never written), sparse
  (Source/ without libwebrtc). Branch `ps5` = 15486a0178 + one commit per
  patch; `export-patches` writes them back to `patches/`.
- Build: `toolchain/wpe-webkit/build` (WC; `build-cpu` for `WC=0`), Release, `-O2 -gline-tables-only`
  (`DEBUG_FLAGS=-g0` for the smallest build), ninja, all cores.
- Toolchain: `prospero-clang` 18 + libc++ 19.1.7 (`toolchain/libcxx19`),
  dependencies from `toolchain/wpe-sysroot` (port/deps, its
  `ps5-wpe.cmake` toolchain file and `pkg-config-ps5`), ICU 76.1.
- `FAKE_DEPS=1`: placeholder libepoxy/libxkbcommon (headers that `#error`)
  so configure completes and JSC/WTF/Skia build while port/deps finishes
  them. Not for a real build.
- WebKit is a static library (`WebKit_LIBRARY_TYPE STATIC` on PS5); the
  executables in `apps/` link it. The title relinks ps5browser's link line
  with PS5VR's native-app CRT and ps5-opengl (`apps/title/package.sh`).

## Patches (`patches/`, applied in order by `build.sh src`)

| patch | what |
| --- | --- |
| 0001-cmake-cross-compile-from-macOS-and-PS5-define | skip WebKitXcodeSDK and the compiler include-dir probe when cross-compiling from macOS; API tests off; `-DPS5_PAYLOAD_SDK=1` from OptionsJSCOnly and OptionsWPE when the SDK toolchain sets `PS5` |
| 0002-ps5-payload-sdk-is-FreeBSD-not-PLATFORM-PLAYSTATION | `__SCE__` no longer selects bmalloc/libpas PLAYSTATION; libpas on for FreeBSD x86_64; 16 KiB page ceiling |
| 0003-freebsd-portability-fixes | libpas FreeBSD monotonic clock, `<malloc.h>` guard, bytecode-cache version without `dladdr` |
| 0004-clang18-suppress-attribute-on-function | `[[clang::suppress]]` on a function is clang 19+ |
| 0005-jsc-shell-ps5-entry-point | `jsc` shell main for payloads (argv/stdin), for the JSC test payload |
| 0006-libpas-ps5-16k-pages | libpas commit/decommit within 16 KiB pages (found on the console in Spike 4) |
| 0010-wpe-platform-ps5-backend-and-process-manager | `ENABLE_WPE_PLATFORM_PS5`: builds `wpe/ps5` (a symlink to `port/webkit/wpe-ps5`) into WPEPlatform and WebKit; enables `WPEProcessManager` (Android-only upstream) for PS5 in WPEPlatform and ProcessLauncherGLib; WebKit as a static library; `apps/` added when `PS5_PORT_DIR` is set |
| 0011-wpe-on-freebsd-portability | WPE on a FreeBSD userland: WTF picks the generic memory footprint and no Linux-only sources (as PlatformGTK does), plus later fixes found while building |

| 0012-ipc-unix-log-failures | diagnostics: log why each IPC connection closes, in `Platform/IPC/glib/ConnectionGLib.cpp` (the file GLib ports use) |
| 0013-ps5-payload-exit-and-cache-without-hard-links | `AuxiliaryProcess::didClose` exits at once (`_exit`) in PS5 payloads: the GLib teardown after the run loop stopped aborted (SIGABRT) in every swapped-out WebProcess. The network cache copies a blob when `link()` fails (EOPNOTSUPP on `/data`) instead of leaving the record without its body (`Failed to create hard link`) |
| 0014-ps5-exit-on-ui-close-and-no-forced-compositing | payloads `_exit(0)` on the IPC thread as soon as the connection to the UI process closes (end of stream or HUP; 0013 only covered `didClose` on the main thread, and swapped-out processes still aborted); `RenderLayerCompositor::enableCompositingMode` is a no-op when accelerated compositing is off: `document.startViewTransition()` (YouTube watch pages) forced compositing on and `NonCompositedFrameRenderer::graphicsLayerFactory()` RELEASE_ASSERTed (SIGABRT, every watch page) |

| 0015-media-build-fixes-video-without-web-audio | ENABLE_VIDEO with Web Audio off: `platform/audio/gstreamer` on the include path (AudioDecoder.cpp includes it under USE(GSTREAMER)); no WEBCORE_EXPORT on a member of the exported `MediaSourcePrivate` (dllexport inside dllexport is an error with this toolchain); libpas uses libc's `backtrace()` on FreeBSD (its own stub clashed with libc.a's once GStreamer pulled `backtrace.o` in) |
| 0016-ps5-software-video-paint-and-media-policy | PS5 only (`PS5_PAYLOAD_SDK`): AV1 not advertised (no hardware AV1; `PS5WK_MEDIA_AV1=1` advertises it), `PS5WK_MEDIA_VP9=0` hides VP9; avdec `max-threads` 4 (`PS5WK_DECODER_THREADS`), `max-errors` -1 (a reference-less frame after a flush doesn't end playback); playsink's `videoconvert` on 4 threads; video painting: the frame is scaled to the device-pixel size with GstVideoConverter (4 threads, linear) and wrapped zero-copy in an SkImage cached for the repaints of the same frame (Skia's raster bilinear scaling of 1080p cost ~30 ms per frame, ImageGStreamer copied the frame on every paint, and `convertSampleToImage` ran a conversion pipeline even when the format already matched); non-composited rendering updates longer than 6 ms are followed by an equal idle gap (NonCompositedFrameRenderer), so JavaScript timers and MSE appends still run while video plays |
| 0017-ps5-hole-punch-video | PS5 only: `MediaPlayerPrivateGStreamer` makes `ps5videosink` (port/deps/gst) as the video sink (`PS5WK_HOLEPUNCH=0`: the step-1 path), `paint()` clears the video's rectangle to transparent and reports it to the sink (`ps5_video_sink_set_geometry`), canvas `drawImage` still gets the frame (`paintCurrentFrameInContext`), new samples no longer repaint the page (only the first one, for the hole), the streaming thread no longer waits for a paint; MSE `TrackQueue` water levels 1000/500 ms (were 200/100 ms: the busy main thread let the video run dry on YouTube) |
| 0018-ps5-single-web-process-and-no-decoder-drains | PS5 only: one WebProcess for all navigations (no process swaps, WebProcess cache or prewarming; `PS5WK_PROCESS_SWAPS=1` restores them): each payload costs ~300 of 8192 system page tables and swaps exhausted them; still-frame events are dropped before avdec too (`PS5WK_STILL_FRAME=1` keeps them): avdec's drain flushed libav's reference frames, so video stayed broken until the next keyframe |
| 0019-ps5-gc-suspended-thread-registers | PS5 only: WTF's `registersFromUContext` takes the mcontext at ucontext +0x40 (the PS5 kernel's layout; the SDK header says +0x10). JSC's conservative GC read a suspended thread's stack pointer from the kernel's `mc_addr` instead: 0 (stack not scanned at all) or a stale fault address above the stack (wrapped size: libpas out-of-memory panic in `gatherConservativeRoots`). `captureStack` also scans the whole stack if a stack pointer is ever outside it (counted, logged after the threads resume) |
| 0020-ps5-hardware-decoded-video-samples | PS5 only: samples from `ps5vdecproxy` (`video/x-raw(memory:PS5Remote)`, a picture decoded in the title) are not painted (canvas `drawImage` gets nothing) and `videoFrameForCurrentTime` returns null, instead of mapping a placeholder buffer |
| 0030-ps5-jit | the JIT (Baseline, DFG, FTL, YARR) for PS5 payloads: off by default on PS5 (`useJIT=false` in `Options::overrideDefaults`, a title can map RWX memory but faults executing it), turned on by ps5wk-child for WebProcesses and by the jsc shell; a 128 MB executable pool (x86_64's 1 GB would cost ~512 page tables); WebAssembly built (B3 does not compile without it) but off at run time, no fast memory; DFG/FTL compiler threads get 2 MB stacks; WTF's fault handlers use the alternate signal stack and pass faults that are not theirs to the crash guard; `ps5wk_jit_memory()` for the `[mem]` line and the crash guard; jsc shell links the dependency libraries and reads ICU data from /data/ps5webkit/share/icu; `InlineCacheCompiler.h` includes `CCallHelpers.h` (LLIntOffsetsExtractor did not compile with the JIT on) |
| 0031-ps5-libpas-decommit | PS5 only: libpas gives decommitted memory back (replaces 0006's zero-only decommit): 16 KiB granules (the page size), decommit maps fresh anonymous pages over whole pages (`madvise` and `mprotect` leave them resident on the console, `test/memprobe`), commit is a no-op; executable memory is only zeroed; `PS5WK_PAS_DECOMMIT=0` restores the zero-only behaviour; `pas_page_malloc_ps5_decommitted_bytes` for the `[mem]` line |
| 0032-ps5-cpu-painting | PS5 only, CPU page painting (see *CPU painting* below): a frame is recorded into an SkPicture and replayed in horizontal bands on `PS5WK_PAINT_THREADS` threads (default 4; `PS5WK_PAINT_ASYNC=1`: replayed on the pool while the main thread goes on); scrolls move the last frame's pixels and only the uncovered strip and what changed is painted, every frame starts from a copy of the last one (`PS5WK_SCROLL_COPY=0`: repaint on scroll, as upstream); damage is painted as non-overlapping rects (`rectsForPainting`; iterating the damage painted overlaps several times); no full-frame `readPixels` of the SHM surface onto itself (`PS5WK_SHM_READBACK=1` restores it); Skia's heap (`sk_malloc`) is WebKit's libpas (the console's libc malloc collapses under threads, `test/lockbench`); `[render]` lines with per-phase timing; the 0016 throttle no longer drops an update when it fires during a pending frame |
| 0033-ps5-hlsdemux2 | PS5 only: HLS through adaptivedemux2's `hlsdemux2` when `WEBKIT_GST_ENABLE_HLS_SUPPORT=1` (upstream always demotes it: it downloads with its own libsoup, which a sandboxed media process can't; the PS5 WebProcess can), the only HLS demuxer playbin3 takes; webkitwebsrc adds the page's User-Agent to its http-headers event (the NetworkProcess adds it to the real request), which hlsdemux2 uses for its own requests |
| 0034-ps5-network-cache-and-dns | PS5 only (see *Locks, malloc and networking* below): the disk cache writes each body once, straight to the record's blob file, and counts it (0013 wrote `Blobs/<hash>` and copied it in full, and the copies were counted nowhere, so the cache never shrank); DocumentBrowser's disk cache is desktop-sized (1 GB with 16 GB free; was 75 MB); typed DNS lookups (libsoup's IPv4-only/IPv6-only) are answered from a prefetched Default answer, filtered by family, and "not found" when it has no address of that family |
| 0035-ps5-profiling | PS5 only (see *Profiling* below): JIT counters for the profiler and the `[mem]` line (`ps5wk_jit_counts`: compiles by tier and outcome, compile ms, refused executable allocations, the executable-memory pressure multiplier); every executable allocation noted with the tier of the compiling thread (`ps5wk_jit_tier(pc)`); WTF threads named with `pthread_set_name_np` |
| 0036-ps5-media-state | PS5 only (see *Feeds: spinners over playing videos* below): `requestVideoFrameCallback` works with the hole-punch sink and CPU painting (presented frames were only counted on the accelerated path, so callbacks never ran); a buffering change that arrives during an async state change is applied when it completes (it was dropped, leaving the player at HaveCurrentData once the download was done), an older one is no longer "delayed" again by every later seek, loop or resume, and the download-complete message runs the normal buffering update; seamless looping (non-flushing SEGMENT seeks) is off for http(s) progressive media, whose qtdemux pull loop waits for bytes only the main thread delivers (the page froze when a looping video resumed from WebKit's off-screen suspend); the MSE player keeps its pipeline when the ended notification started a loop seek (it tore it down under the seek: "seeking" for good); the "Autoplay blocked ... PageConsentRequired" line is only logged for an element that has the autoplay attribute and is still waiting to autoplay |

GPU compositing (COMPOSITING-PLAN.md; `WC=1` builds, the default since
2026-11-02). These were developed on branch `ps5-wc` (worktree `src-wc`) and
regrouped into patches when they were merged into `ps5`; the last column
names the `ps5-wc` commits (still reachable as branch `ps5-wc-history` in
the clone) that older notes refer to. Each regrouped commit's message holds
the original messages.

| patch | what | ps5-wc commits |
| --- | --- | --- |
| 0037-ps5-wc-build-options | `USE_TEXTURE_MAPPER` + `USE_GRAPHICS_LAYER_WC` on WPE next to coordinated graphics and Skia: `Platform/WC.cmake` included, TextureMapperTile built, the GPU-process-only WC files left out | 687d4c1cfb |
| 0038-ps5-wc-without-gpu-process | WC without the GPU process: `UpdateInfo` and `DrawingAreaWC` build on WPE, `flushImageBuffers` only with the GPU process, `DrawingArea::create` picks WC at run time, `glGetStringi` extensions in core profiles, WPE definitions `PageClient` and `WebPageProxy` need | 9928301709 |
| 0039-ps5-wc-ui-process-drawing-area | `UpdateWC` message to the UI process; `DrawingAreaProxyWCPS5` (the run-time compositing switch, preferences per new WebProcess), `PS5WCHost` interface and the null scene host for ps5wk-shot | 2d6c10a99d a69c8b94ca a6cee1584e 444b57d4fa dca82b46b4 |
| 0040-ps5-wc-texture-mapper-on-gl-core | TextureMapper on PS5Renderer's desktop GL 4.6 core context (S1b): its own VAO, core-profile extension list, shader compile/link checks (`#version 330 core` fallback, `PS5WK_TEXMAP_GLSL=core`) | 4482488ce0 |
| 0041-ps5-wc-tile-uploads | tile uploads through a staging ring and an FBO pass (`BitmapTextureUploaderPS5`; `glTexSubImage2D` detiles on the CPU at ~5 ms per tile) | 9762a8c577 |
| 0042-ps5-wc-scene-host | `WCScene::apply`/`paint` split with a borrowed context; `PS5WCHost`, the scene host on the title's render thread (M1) | 8b9c728b7a 9371aa286b |
| 0043-ps5-wc-tile-grid-and-coverage | tiles survive layer resizes, up to 8 dirty parts per tile; coverage half a view ahead, one more row kept; tile paint stats (M2) | 057ebfbb87 1106c18420 |
| 0044-ps5-wc-frame-pacing | DisplayRefreshMonitor driven by the title's presents, `[render]` lines for WC; no scene repaint for updates without layer changes (M2) | 74efe25133 29a9396e3d |
| 0045-ps5-wc-scrollbars-fixed-and-sticky | scrollbars painted into their layers; no full repaint of fixed and sticky content per scroll step; composited sticky elements (M3) | c945b3ae90 1b74f03f8a 4e0fe1c13e |
| 0046-ps5-wc-tile-painting-arena | tiles painted into a shared-memory arena, recorded once and replayed on the painting threads (M5) | 8af4f0f1d2 |
| 0047-ps5-wc-accelerated-animations | transform and opacity animations run in the WC scene (M3) | fea93260ac |
| 0048-ps5-wc-async-tile-replay | tile replay asynchronous by default; recordings snapshot ImageBuffers | 2f640b9488 |
| 0049-ps5-wc-texture-pool-off-main-thread | no BitmapTexturePool timer on the title's main thread; a log line when the pool goes over its limit | 7d09449da9 c290e972ee |
| 0050-ps5-wc-animations-at-display-time | animations sampled at the frame's display time; animated layers tiled whole | b83769b247 |
| 0051-ps5-wc-video-layers | composited video layers name their ps5videosink stream key and draw the title's converted frames (M4) | 02a3f8847b 0e6aee371a |
| 0052-ps5-media-suspend-after-preroll | muted videos made off screen go to their suspend state once prerolled (`PS5WK_MEDIA_SUSPEND_PREROLLED=0`: old behaviour; M6) | b9007fb2c3 |
| 0053-ps5-wc-gl-texture-cap | GL texture cap in the title (reduced coverage, then the CPU path for that WebProcess); scene teardown empties the texture pool (M6) | 900ba6f6ad |
| 0054-ps5-wc-sticky-at-current-scroll-position | composited sticky layers placed with the current scroll position | b235085eb2 |
| 0055-ps5-wc-reply-and-update-pacing | a rendering update asked for after a reply starts at once; title frames without an update wait for the expected one | 95e259c204 80573f3544 |
| 0056-ps5-wc-tiles-outside-scissor | tiles outside the scissor box are not drawn | 8f6597b8f5 |
| 0057-ps5-wc-overlap-on-scroll-layout | overlap recomputed on scroll steps that also need a layout | edfee43cea |
| 0058-ps5-wc-async-scrolling | async scrolling in the title: `WCScrollingCoordinator` in the WebProcess, `PS5WCScrollingTree` in the title, wheel events handled there, ScrollUpdatesWC back (AS0-AS3) | 8b7e3617ac |
| 0059-ps5-image-decode-metadata-without-waiting | painting doesn't wait for an image's background decode to read its metadata (and the stack-overflow fix of that guard) | 730265f633 2295014834 |
| 0060-ps5-wc-async-scrolling-stick-and-sub-scrollers | early replies, the frame-locked stick (`stickframe=1`), async overflow scrollers (`overflow=1`) (AS4, AS5) | e8f6923ada |
| 0061-ps5-wc-scene-paint-batching-and-timing | scene quads as triangles (the driver batches only `GL_TRIANGLES`: 2800-layer scene paint 82-96 -> 39-41 ms; `wc-fan-draws` for A/B); scene and tile-arena timing (`wc-scene-stats`); lagging scrollbar hidden (AS6) | 513c01b0c4 |
| 0062-ps5-wc-async-scrolling-overflow-and-snap | horizontal overflow scrollers and scroll snapping under async scrolling | b8033dea3f |
| 0063-ps5-wc-perf-overlay-counters | counters for the title's performance overlay (`PS5CompositorClient::perfSnapshot`, OVERLAY.md) | 9e25fc7e55 |
| 0064-ps5-wc-async-scrolling-gl-cap-and-gesture-nodes | no lock-up at the GL cap; gesture nodes so carousels aren't resnapped under the stick; pool surfaces released after 50 ms near the cap | 2e90eec762 85fdae9903 |
| 0065-ps5-wc-overflow-proxy-tiles-and-snap-glide | tiles for children of overflow scroll proxies (the missing last card); snap carousels glide on after the stick | 9c7811f80e |
| 0066-ps5-wc-async-scrolling-sync | frames that scroll wait for the update made at their position, so scroll-linked content moves with the page (`sync=0` off) | f4d8a064a5 |
| 0067-ps5-wc-on-by-default | GPU compositing on unless `/data/ps5webkit/no-compositing`; async scrolling on with it unless `wc-async-scroll` says `off`, frame-locked stick by default; `[ascroll]` lines whenever async scrolling is on | (new) |
| 0068-ps5-wc-long-feed-webprocess-costs | long feeds (COMPOSITING-PLAN.md, *Long feeds*): `will-change: opacity` at opacity 0 composites no layer of its own; layer positions after a document scroll skip unconstrained subtrees (clip rects stay cached for hit tests); no per-layer event regions with the WC scrolling coordinator; the layer flush skips clean, tile-less subtrees far from the view; `[render] wc flush` / `[render] compositing` lines and the `[wc-reasons]` census (`PS5WK_WC_LAYER_LOG=3`); A/B: `PS5WK_WC_WILLCHANGE_HIDDEN=1`, `PS5WK_LAYERPOS_SKIP=0`, `PS5WK_WC_EVENT_REGIONS=1`, `PS5WK_WC_CULL=0` | (new) |
| 0069-ps5-wc-long-feed-title-costs | long feeds, the title: DOM-only wheel events paced by the WebProcess's updates (or 250 ms), events the root can't take not resent for 250 ms after the WebProcess found nothing to scroll (`wheelpace=0`, `unconsumedpace=0` in `wc-async-scroll`); scene paint skips subtrees whose bounds miss the clip, no coordinated "future" transforms (`wc-no-cull`) | (new) |
| 0070-ps5-wc-feed-append-costs | feeds that load more (COMPOSITING-PLAN.md, *Feeds that load more*): a transform animation nested in an animating ancestor (loading spinners) gets its extent through the ancestor's keyframes instead of compositing every later layer; wheel events the root can't take at most one per WebProcess update, deltas carried; wheel transaction (kept target) for DOM-only wheel events; far or clipped-out intersection observer targets skipped for a bounded time; `[render] wc phases` line. A/B: `PS5WK_WC_NESTED_ANIM_EXTENT=0`, `PS5WK_WHEEL_TRANSACTION=0`, `PS5WK_IO_SKIP=0`, `unconsumedpace=0` | (new) |
| 0071-ps5-input-multiline-hint | a multiline input hint for textarea and contenteditable elements (`InputMethodState::Hint::Multiline` -> `WEBKIT_INPUT_HINT_MULTILINE` -> `WPE_INPUT_HINT_MULTILINE`): the system keyboard's OK presses Return after the text only in single-line fields (*System keyboard*) | (new) |
| 0072-ps5-hidden-pages-for-live-tabs | the browser's live background tabs: a page shown again repaints whole on the CPU path (`DrawingAreaCoordinatedGraphics::resumePainting`); the WC scene host never shows a hidden page's scene and shows a page's scene as soon as its view is shown (`viewIsBecomingVisible`/`Invisible`, `PS5WCHost::setPageVisible`); near the GL texture cap with hidden scenes, the title is asked to close the hidden pages first (`ps5wk_wc_set_hidden_pages_callback`) |
| 0073-ps5-live-tabs-refresh-clock-media-and-gl-pressure | live tabs: the process's one `DisplayRefreshMonitor` follows the shown page's WC drawing area and is handed on when its drawing area goes (it stayed bound to the first view's: rAF and rendering updates of every other page lost their clock once that view went); a hidden page pauses its media playing with sound and can't start any (`PS5WK_BACKGROUND_AUDIO=1` turns it off); hidden pages are dropped only over the GL texture cap (was 80% of it), with a 2 s hold-off before the shown page's coverage is reduced |

0011 also carries: WTF FreeBSD sources, VIDEO=OFF build fixes (media sources,
TextExtraction, AudioEncoder/PlatformRawAudioData), Skia `malloc_np.h`, DNS
includes, WKAPICast PlayStation guard, `EGL_DEFAULT_DISPLAY` as an int on PS5,
xdgmime globals renamed (clash with GIO's copy in a static link), CPU-only
WPE preferences on PS5 (no hardware acceleration, no compositing; in WC
builds 0039 turns compositing on per WebProcess at run time), and the
128 MB JSC structure heap on PS5.

The memmem shim and `<sys/sysinfo.h>` from Spike 4 are in `shim/`
(force-included `ps5shim.h`, `libps5shim.a`).

## Media (Milestone 2, step 1)

`<video>`, `<audio>` and MSE play on the static GStreamer from port/deps
(`USE_GSTREAMER_FULL=ON`, `USE_GSTREAMER_GL=OFF`, `ENABLE_VIDEO=ON`,
`ENABLE_MEDIA_SOURCE=ON`, Web Audio off). `apps/common/media-stubs.h` (the
Milestone 1 placeholder media interfaces) is gone.

### Decision: decode in the WebProcess, play audio in the title

- **Video: software decoding in the WebProcess payload** (avdec_h264,
  avdec_vp9, avdec_h265 from FFmpeg 7.1.1; dav1d for AV1, not advertised),
  painted through WebKit's normal non-composited video path into the page
  bitmap. The hardware decoder only works in a title (Spike 9) and its frames
  would have to reach the page by hole punching, which needs compositing in
  the title; that is step 2. Software decoding was hours of work, not days,
  and covers ≤1080p (measured below).
- **Audio: through the title.** An elfldr payload *can* open sceAudioOut
  (`test/audio/audioprobe.c`: main port of the system user 255, S16 48 kHz,
  2000 ms of audio consumed in 2013 ms; other user ids 0x80260011), and the
  first build played from the WebProcess (`ps5audiosink`, direct mode). But
  **the audio daemon never releases what payload processes took**: after
  eight payload processes had used sceAudioOut (2026-10-28: the probe, the
  gst smoke test and six WebProcesses, some closing their port, some not),
  `sceAudioOutInit` returned 0x80260002 in the ninth, and then blocked
  forever (IPC wait `IpmWSv0`) in every new process. A title launch an hour
  later hung in the launcher (`IpmWEf0`) and the console rebooted (11:44).
  **Never use sceAudioOut from a payload.**
  A title is an application the system cleans up after: 12 launch/SIGKILL
  cycles of PPSA99020 in a row, each opening a port and leaving it open when
  killed (`/data/ps5webkit/audio-selftest`), all reopened it (handle
  0x20000000, Init 0).
- So the WebProcess's `ps5audiosink` (port/deps/gst/gstps5audiosink.c,
  autoaudiosink picks it at rank PRIMARY) ships S16 48 kHz PCM over a unix
  socket, `/system_tmp/ps5wk/audio-<UIProcess pid>.sock`
  (`PS5WK_AUDIO_SOCKET`, set by ps5wk-child from its launch path), to the
  audio server in the UIProcess (`apps/common/ps5audioserver.c`): one
  connection and one sceAudioOut port per stream, 256-frame grains, a
  "frames played" count sent back per grain for the sink's `delay()` (A/V
  sync), a flush/seek closes the connection (queued audio dropped). Blocking
  in sceAudioOutOutput paces the WebProcess's pipeline clock through the
  socket (about 4 grains, 21 ms, buffered on each side). The title closes
  its ports when main returns (`ps5wk_audio_server_stop`); a SIGKILL leaves
  that to the system (checked above). ps5wk-shot runs the same server with a
  paced null output, so harness runs never touch sceAudioOut.
- Long-term (FEASIBILITY §1a) the hardware decoder element lives in the
  title with hole-punch video; audio already lives there.

### Knobs

The knobs meant for users (JIT, painting, memory, hole punch, VP9, hardware
decoding, sound, the link server, the home page) can be changed on the
**Settings** page (R3 on the PS5, or `http://<ps5>:9040/settings`; see
*Settings*). The table lists them all, debug knobs included.

| | |
| --- | --- |
| `/data/ps5webkit/audio-null` | title: consume audio silently (no sceAudioOut) |
| `/data/ps5webkit/audio-silent` | title: real sceAudioOut ports at volume 0 (log-only runs) |
| `/data/ps5webkit/audio-selftest` | title: open a port at start, 0.1 s of silence, leave it open (`RESULT ps5browser audio-selftest`) |
| `[ps5audio]` lines | per stream: `sceAudioOutGetPortState` when the port opens and after 2 s (output bits, channels, volume, reroute count), failed `sceAudioOutOpen`/`SetVolume` calls, and at close the samples' peak and RMS and the count of failed `sceAudioOutOutput` calls (is the WebProcess sending silence? is the port refusing data?) |
| `child.env`: `PS5WK_DECODER_THREADS`, `PS5WK_MEDIA_VP9=0`, `PS5WK_MEDIA_AV1=1`, `GST_DEBUG=...` | WebProcess decoding, codec advertising, GStreamer logs |
| `child.env`: `PS5WK_PROFILE=1` | sampling profiler, every thread of both children weighted by CPU time (`=wall`: the main thread by wall time; `=threads`: every thread every 20 ms by wall time, idle or not, for hangs between threads; `apps/common/profiler.c`, see *Profiling*); `test/prof/prof-run.sh` runs pages, `test/prof.sh` summarizes the newest profiles |
| `child.env`: `PS5WK_CPU_LOG=0` | turn off the `[cpu] webN: x cores` lines (every 5 s while above 0.3 cores) |
| `[render]` lines (WebProcess, every 5 s while rendering at least 5 updates) | rendering updates per second; total time per update (avg, p50, p95, max); the update phase (style, layout, rAF, JavaScript); paint (avg, p95, max; Mpx painted per update, updates painted in bands, threads, time spent copying the last frame forward, async rasters); `copy` (the rest: frame hand-off); `frame done` (frame sent until the UIProcess presented it, vsync included); updates followed by the idle gap |
| `child.env`: `PS5WK_RENDER_LOG=0` / `1` (default) / `2` / `3` | `[render]` lines off; every 5 s; also one `[render1]` line per update (`test/paint/paint-ab-summary.py` reads them); also every scroll and the rects painted |
| `child.env`: `PS5WK_PAINT_THREADS=<1-8>` (default 4) | CPU painting threads (see *CPU painting*): 1 paints directly on the main thread as upstream; more record the frame and replay it in bands (the main thread takes bands too) |
| `child.env`: `PS5WK_PAINT_ASYNC=1` | the main thread records and goes on (JavaScript, timers, input) while `PS5WK_PAINT_THREADS` pool threads replay; the frame is sent when they are done (off by default: a little less throughput on image-heavy pages) |
| `child.env`: `PS5WK_SCROLL_COPY=0` | scrolls repaint the whole view (upstream), and frames don't start from a copy of the last one |
| `child.env`: `PS5WK_PAINT_MIN_PX=<n>` (default 65536), `PS5WK_PAINT_BBH=0`, `PS5WK_PAINT_RECORD=1` | A/B knobs: damage below n pixels is painted directly; record without a bounding box hierarchy; record and replay even on one thread |
| `child.env`: `PS5WK_IDLE_GAP=<percent>` (default 100) | the idle gap after a rendering update longer than 6 ms, in percent of its duration (0 = none) |
| `child.env`: `PS5WK_SHM_READBACK=1` | the old full-frame `readPixels` of the shared-memory surface onto itself after each frame (A/B only) |
| `child.env`: `WEBKIT_GST_ENABLE_HLS_SUPPORT=0` | no HLS in `<video>` (the child sets 1; see *HLS streams*) |
| `WEBKIT_GST_VIDEO_DECODING_LIMIT` | set by the child: 7680x4320@60 when the title decodes in hardware (step 2b; the child asks the title's video server at start), else 1920x1080@60 (software: YouTube stays ≤1080p) |
| `child.env`: `PS5WK_MEM_LOG=<s>` (0 = off), `PS5WK_CPU_LOG=all` | the WebProcess memory watchdog period (default 60 s); `[cpu]` lines every 5 s whatever the load |
| `child.env`: `PS5WK_MEM_RESTART_MB=<MB>` (default 3072, 0 = off) | WebProcess memory limit: over this much libpas memory committed, the title (and ps5wk-shot's stress mode) ends the WebProcess at the next top-level navigation and loads the page in a new one (see *WebProcess memory*) |
| `child.env`: `PS5WK_PAS_DECOMMIT=0` | libpas only zeroes decommitted memory (before 0031: memory is a high-water mark), for A/B tests |
| `child.env`: `PS5WK_JIT=0` / `baseline` / `dfg` / `1` (default) | JavaScriptCore's JIT in WebProcesses (see *JIT* below): off (LLInt only), up to Baseline, up to DFG, all tiers; any `JSC_<option>=...` line is passed to JSC as well |

The children also set `WPE_SHELL_MEDIA_DISK_CACHE_PATH=/data/ps5webkit/home/cache/media`
(progressive downloads buffer on disk; WebKit's default `/var/tmp` doesn't
exist for payloads: "Could not create temp file").

### Test media and pages

Start page tile "Sound test (beeps)": `media/sound-test.m4a`, audio only (a
440 Hz tone with a 1760 Hz beep at the start of every second, 60 s), for "is
there sound at all" checks without any video decoder involved.
`test/media/make-media.sh [--upload]` makes the clips with FFmpeg on the Mac
(test patterns with a running clock, a white flash and a 1760 Hz beep at the
start of every second: A/V sync by eye and ear; nothing third-party is
committed) and uploads them with `video.html` (file:///data/ps5webkit/video.html,
tile "Video test" on the start page; `?src=<url>` plays a URL, the stats line
and `VIDEOSTATS` console lines give frames, drops and fps).
`test/media/yt-stats.js` (ps5wk-shot `js=`) dumps the first `<video>`,
YouTube's stats for nerds and `MediaSource.isTypeSupported` answers;
`test/media/yt-consent.js` (ps5wk-shot `onload=`, new) answers YouTube's
cookie dialog, which pauses the video in a fresh session, and logs
`YTSTATS` every 3 s.

### Measured (ps5wk-shot, 1920x1080 viewport, video element 1280x720, 2026-10-28)

| clip | fps shown | dropped | WebProcess CPU |
| --- | --- | --- | --- |
| local 720p30 H.264 + AAC (1:1, before the paint fixes) | 28.5 | 0 | 1.44 cores |
| local 1080p30 H.264 + AAC | 29.9 | 0 | 0.91–1.05 cores |
| local 1080p60 H.264 + AAC | 59.9 | 0 | 1.59–1.95 cores |
| local 1080p30 VP9 + Opus | 29.9 | 0 | 1.03 cores |
| Sintel trailer 720p24 H.264 + AAC over HTTPS (moov at the end, range requests) | 24 | 0 | 0.55 cores |
| YouTube "Never Gonna Give You Up": MSE VP9 720p25 (itag 247) + AAC (140) | 25 | 0 | 0.87 cores |
| YouTube Big Buck Bunny 60 fps: VP9 720p60 (302) chosen | plays, JS starved before the throttle | | 1.2–1.7 cores |

Before the paint fixes 1080p30 managed ~19 fps and starved the page's
JavaScript completely (events and timers ran only after the video ended):
the profile showed 81 % of the main thread in Skia's `bilerp_clamp_8888`.
YouTube asks `isTypeSupported` and gets: avc1 yes, vp9/vp09 yes, av01 no,
VP9 at 3840x2160 no, at 1920x1080@60 yes; mp4a yes; `audio/webm; opus` no
(not needed so far: YouTube sent AAC).

**In the title (log-only run, 2026-10-28 11:58, audio-silent):** the local
1080p30 MP4 played with its audio going through the title's sceAudioOut
(32.8 s of audio in 34.3 s, handle 0x20000000 at volume 0) at **20–23 fps**
(0 dropped, WebProcess 0.46 cores; the title's vsync-paced frame
completion plus the rendering throttle is the suspect for the gap to 30), and
the title's flexible memory was 299 MB free (eboot 129 MB with GStreamer,
was 109 MB). The YouTube URL after it did not report "load finished" within
60 s, and the launcher's kill then got no reply: open.

### Console wedge at 12:00 and the clean wind-down (2026-10-28)

After the log-only run above, `tools/launch.sh --kill` got no reply, and
the console froze: the title stayed on screen, the PS button did nothing,
FTP and the launcher stopped answering, ping still worked, and the owner
power-cycled. What the logs say:
- **Nothing was playing when the kill was asked for.** The MP4's audio
  stream had closed when the page left for YouTube (`stream from pid 150
  closed: 32.8 s`). The YouTube WebProcess (web6) never started media: no
  `ps5audiosink` line, never above 0.3 cores.
- **The title was still alive afterwards.** Its 60-second `[mem]` lines went
  on until 12:04:17 (6 after the URL timeout, the log file's mtime). So
  the SIGKILL never reached it. The launcher was stuck *before* the kill, in
  its system-service calls (`sceLncUtilGetAppIdOfRunningBigApp` / the
  `sceKernelGetAppInfo` scan), or in writing its log on /data, as it was in
  `sceLncUtilLaunchApp` (`IpmWEf0`) at 11:43. With the PS button and FTP
  dead too, a system service or /data was hung. The cause is unknown.
- **Already slow before the kill:** the elfldr uploads of web5 and web6
  took 4.5 s each (683–837 ms earlier in the same run), while web4 was still
  decoding the 1080p clip and GStreamer's downloadbuffer was copying that
  local file to /data (`Enabling on-disk buffering`). Local files no longer
  get an on-disk copy (0016).

**What could hold up a title's teardown** (from the code): a thread inside
`sceAudioOutOutput` (at most one 5.3 ms grain, then it returns), the audio
server's stream threads blocked in `recv` on the WebProcess socket, the GL
thread in `eglSwapBuffers`/VideoOut flips (main thread, at most one vsync), the
control/listen threads in `accept`, the payload sockets. All of these are
interruptible waits for a SIGKILL. The 12 kill cycles had GL running and a
port open. The one kind of wait that a kill can't interrupt is a system IPC
call (`Ipm*`), as the hung audio probes showed. The title makes those only
for sceAudioOut, the pad and the IME.

**Wind-down (checked on the console, below):**
- `tools/launch.sh --stop`, and every time launch.sh is about to stop
  PPSA99020, now sends the launcher's new `stop` command. The launcher
  connects to the title's unix socket
  `/system_tmp/ps5wk/ps5browser-control.sock` and sends `quit`. This makes no
  system calls, so it works while services are slow.
- The title answers `OK winding down` and then:
  1. Loads about:blank. The WebProcess stops playback, its GStreamer
     pipeline goes to NULL, and its audio sink disconnects.
  2. Waits up to 2 s for the audio streams to end.
  3. Ends the WebProcess (`webkit_web_view_terminate_web_process`).
  4. Stops the audio server: no new streams, `shutdown()` on every stream
     socket, waits up to 2 s for the stream threads. Each thread closes its
     own port after its last grain, so a port is never closed under a writer.
     The server then closes any port left over.
  5. Quits the main loop. `title_exit.c` then asks the launcher for
     `kill -9`, with nothing playing.
- launch.sh polls `status` and kills only if the title is still running
  after `STOP_WAIT` seconds (default 10).
- The launcher now runs each command on a worker thread. A command that
  hasn't answered in 15 s gets `ERR <cmd> blocked for 15 s in <stage>`, the
  name of the call it is stuck in, and the launcher keeps serving other
  connections. It also answers the client before writing its log on /data.
- Diagnostics for the next runs: `[present]` lines from the title (upload
  and swap times per 5 s) and `[render]` lines from the WebProcess.

**Checked on the console (12:12–12:14, owner nearby):**
- **Wind-down at the start page:** `launch.sh --stop` gave "OK stop: OK winding
  down", then "OK stopped (wound down)". The title took 56 ms from "quit"
  to leaving its main loop, then asked for its own SIGKILL.
- **Wind-down during playback:** local 1080p30 clip, real sceAudioOut port
  at volume 0, 13 s in. The audio stream closed 51 ms after "quit", 0
  threads were left, the title was gone, and `status` said "OK none".
- **Relaunch with the same clip:** the port opened again (0x20000000),
  then the same clean wind-down.
- **fps breakdown in the title** (1080p30 clip in a 1280x720 element,
  25 s, no debug-screenshot):

  | What | Measured |
  |---|---|
  | Video | **30 fps, 0 dropped**, WebProcess 0.77–0.85 cores |
  | WebProcess `[render]` | 35–37 updates/s (video plus the stats line), 10–10.7 ms average; 150 of ~180 updates per 5 s followed by the idle gap |
  | Title `[present]` | 34–35 uploads/s, **14.5 ms average per ~3 MB texture upload** (`glTexSubImage2D` through ps5-opengl, about 210 MB/s); `eglSwapBuffers` 0.1 ms |

  The 20–23 fps of the 11:58 run is gone. That run had debug-screenshot
  on and an on-disk copy of the clip. The title's texture upload is the
  next limit: 14.5 ms × 60 frames/s is 87 % of its main thread. Hole punching
  (step 2) removes it for video. Until then, 1080p60 in the title will
  probably drop frames.

**Why YouTube didn't finish in 60 s:** two process swaps, file:// → youtube
(web5) and then YouTube's Cross-Origin-Opener-Policy (web6), each a fresh
114 MB payload through elfldr that took about 10 s to connect (4.5 s upload)
while the old WebProcess was still decoding. In the shot harness, YouTube
finishes loading in 5–6 s with one WebProcess.

**Why the title showed 20–23 fps (11:58 run):** not reproduced; see the
breakdown above (30 fps). The title uploads every damaged rectangle into a GL texture and
presents with a vsync-paced `eglSwapBuffers`. Frame completion goes back to
the WebProcess after that, and the WebProcess's idle gap follows any update
longer than 6 ms. The `[present]` and `[render]` lines measure each part.

Still open after step 1 (the first three are addressed by step 2a, below):
title fps, YouTube 60 fps under its overlays (every video frame repainted
the player's gradient/blur layers on the CPU), the 24 s without video
frames on YouTube after the consent dialog (audio and clock ran on); and
Opus in WebM for MSE.

## Hole-punch video (Milestone 2 step 2a, 2026-10-28)

Step 1 painted every decoded frame into the page on the CPU: on YouTube
each frame also repainted the player's overlays (about 7 page updates/s
at 69 ms each in the TV test), and the title uploaded every page update as
a texture (17.8 ms for 3.8 MB). Now the page gets a transparent hole and the
title draws the frame under it.

**WebProcess** (patch 0017, `port/deps/gst/gstps5videosink.c`):
- `ps5videosink` (rank NONE; WebKit makes it by name) takes I420 or NV12
  from avdec_* (playsink converts anything else), copies each frame into a
  ring of 6 slots of anonymous shared memory (3 MB each, enough for 1080p
  without reallocating; the fd goes to the title over SCM_RIGHTS) and sends a
  128-byte FRAME message with the CLOCK_MONOTONIC time the frame is due.
  The due time comes from the pipeline clock, which is the audio sink's when
  there is audio (it counts what the title's sceAudioOut has played), with
  the clock-to-monotonic offset smoothed (the audio clock steps in 5.3 ms
  grains). The sink syncs 40 ms early (its `get_times` moves the start
  time; playsink owns `ts-offset`), so frames reach the title ~37 ms ahead.
  The title releases a slot as soon as it has copied it; no free slot for
  100 ms drops the frame.
- `paint()` clears the video's rectangle (`clearRect`, device-pixel snapped)
  and calls `ps5_video_sink_set_geometry()`; the sink sends GEOMETRY when it
  changes. Whatever the page paints after the video (controls, captions,
  YouTube's gradients) stays on top of it; whatever clips the video
  (overflow, rounded corners) covers it with opaque page pixels, so the
  title needs no clip of its own. Rotated or mirrored videos fall back to
  painting the frame into the page.
- New samples still reach WebKit (`repaint-requested`: video size,
  current-time tasks, canvas `drawImage`), but only the first one repaints
  the page; the streaming thread no longer waits for the main thread.
- `getVideoPlaybackQuality()` (YouTube's stats for nerds, `VIDEOSTATS`)
  counts frames the title showed, and drops by the sink's QoS, by the title
  and for lack of a slot (`stats` property).

**Title** (`wpe-ps5/PS5VideoServer.cpp`, `PS5Renderer.cpp`):
- One receiver thread per sink connection
  (`/system_tmp/ps5wk/video-<UIProcess pid>.sock`, `PS5WK_VIDEO_SOCKET` in the
  children) queues FRAME messages and maps the shared memory.
- The render thread owns EGL/GL (the UI main thread no longer touches GL).
  It sleeps until a page buffer, a pointer move or a due frame arrives, and
  presents every vsync while a video is playing. For each video it shows the
  newest frame due by the next vsync (plus half a frame), drops older ones,
  memcpys the slot into a persistent GL buffer, releases the slot, and
  converts I420/NV12 to an RGBA texture in one fragment pass (BT.601/709,
  limited/full range from the caps). Then: videos at their rectangles, the
  page blended over them (premultiplied; transparent only in the holes),
  the pointer.
- Geometry is applied together with the next page buffer (the hole and the
  video move in the same frame), or after 100 ms without one.
- The page itself now lives in a persistent, coherent GL buffer that the
  fragment shader reads (`unpackUnorm4x8`), so a page update is a memcpy of
  the damaged rows: 1.0 ms for a full 7.2 MB YouTube page, 0.0–0.1 ms for
  the usual small updates (was `glTexSubImage2D`: 14.5–36 ms, ps5-opengl
  tiles on the CPU at ~210 MB/s). The swap waits for the GPU (`ST_FLUSH_WAIT`),
  so the next frame's copies never race the GPU.
- `ps5wk-shot` runs the same server with a 60 Hz null consumer (`[video]`
  lines) and reports the hole (`RESULT ps5wk-shot holes`).

**Measured** (2026-10-28; harness = `ps5wk-shot` at 1920x1080 with the null
consumer; title = log-only PPSA99020 runs, audio-silent):

| | shown | dropped | WebProcess CPU | WebProcess `[render]` |
|---|---|---|---|---|
| harness, local 1080p30 H.264 (step 1: 0.91–1.05 cores) | 30.0/s | 1 at the start | 0.31 cores | 9/s, 1.2 ms |
| harness, local 1080p60 H.264 (step 1: 1.59–1.95 cores) | 60.0/s | 2–3 at the start | 0.54 cores | 9/s, 1.2 ms |
| harness, YouTube Big Buck Bunny, VP9 720p60 (inline player) | 60/s | 3 in 40 s, through the cookie dialog's pause | 1.2–1.3 cores while the page loads, then 0.43–0.65 | ~5/s |
| harness, same at 1080p60 (`yt-1080.js`: `setPlaybackQualityRange`) | 60/s | 1 in 34 s | 1.5 cores while loading, then 0.68–0.92 | |
| title, local 1080p60 H.264 | 59.5–59.9/s | ~1 per 5 s | 0.43 cores | 9/s, 0.9 ms |
| title, local 1080p30 H.264 | 30.0/s | 0 after the start | | |
| title, clipped/overlaid/scrolling test (`?test=geometry`) | 30.0/s | 0 after the start | 0.38 cores | 32/s, 0.7 ms |
| title, YouTube Big Buck Bunny 720p60 | 44–55/s for the first 20 s (page still loading), then 59–60/s | 32 in 27 s, nearly all in the first 20 s | ~1.0 cores while loading, then 0.35–0.43 | 5/s, 14 ms |

Title side, all runs: 59.9 frames/s presented while a video plays (vsync
interval 16.68 ms: the TV runs at 59.94 Hz, so 60 fps content loses about
one frame every 17 s), video copy+convert 0.47 ms per 1080p frame (0.27 ms
at 720p), frames shown 0–5 ms after due on average, page updates 0.0–1.4 ms.
Title process CPU (getrusage) 0.05–0.10 cores, and its main thread is busy
under 1 % of the time (wall time outside the main loop's poll). Thread CPU
clocks read 0 in a title, so the render thread is not broken out; its frame
takes ~5 ms of wall time, nearly all of it waiting for the previous flip.
Screenshots (title readback) confirmed position, rounded clipping, a
translucent overlay over the video and scrolling.

**Found on the way:**
- **YouTube video stalled for 20–35 s after a pause** (step 1 saw "24 s
  without video frames" too). WebKit's MSE `TrackQueue` keeps only 100–200 ms
  of samples ahead, refilled from the main thread, which YouTube's
  JavaScript (no JIT) blocks for longer. The video ran dry, WebKitMediaSrc
  drained the decoder (still-frame events), QoS then had the decoder skip
  ahead, and the sink waited for a frame 35 s in the future. With 1000/500 ms
  water levels (0017) it played through the cookie dialog's pause without a
  stall (harness, three runs).
- playsink overwrites `ts-offset` on its sinks (its `av-offset`); hence
  `get_times`.
- A swapped-out WebProcess aborted once while exiting (the IPC thread's
  `_exit` racing a thread whose `ThreadGlobalData` destructor reached
  `Font::~Font` off the main thread: `MainThreadSharedTimer` RELEASE_ASSERT);
  the crash guard caught it. Same family as the earlier `Font::~Font`
  teardown abort; not in the video path.

**Knobs:** `child.env`: `PS5WK_HOLEPUNCH=0` (frames painted into the page
again, step 1). Title: `/data/ps5webkit/page-texture` (page as a texture,
the old upload path); `/data/ps5webkit/test-onload.js` (evaluated in each
test URL's page 2 s after it loads; `test/media/yt-consent.js` answers
YouTube's cookie dialog in log-only runs). Test page:
`video.html?src=...&test=geometry` (rounded clipping box, a label over the
video, the page scrolls by 160 px every 3 s). Harness scripts:
`test/media/pause-resume.js`, `test/media/seek.js` (forward, back, paused
seek: frames resume after each flush), `test/media/yt-1080.js`.

### TV test of step 2a and what it found (2026-10-28, afternoon)

The owner's TV test: the local clips played well with few drops. On YouTube,
main-document loads failed (`tlsv1 alert protocol version` three times,
`Bad file descriptor`, `Connection reset by peer`, within 15 s around process
swaps), then the browser froze; video stayed black for ~10 s at the start
of one Sintel load while audio played; and YouTube "still stutters a bit".

**TLS/EBADF errors: found and fixed.** Reproduced with the harness's new
stress mode (`ps5wk-shot cycle=<URL list> rounds=<n> dwell=<s>`;
`test/stress/youtube-swaps.txt` alternates file:// and YouTube watch pages,
so every load is a process swap): 1–2 failed loads per 20, the same errors.
An fd trace in the payloads (`PS5WK_FD_TRACE=1` in `child.env`: every close of
a socket and every inet `socket()` with its stack, plus every close that
fails with EBADF, from the shim's `close`/`socket` wraps) showed GSocket's
own `g_socket_close` of fd 20 failing with EBADF, and just before it a close
of fd 20 from **libc++'s `std::filesystem::remove_all`**, called by
`NetworkStorageManager` (`deleteEmptyOriginDirectory`) whenever a WebProcess
disconnects. Cause (probe `test/openat/openat.c`): **libkernel's `openat`
returns the errno as a positive value** in payloads: `openat(file,
O_DIRECTORY)` = 20 (ENOTDIR), `openat(missing)` = 2; `fstatat` and `unlinkat`
too (`open` is right). remove_all opens every entry with O_DIRECTORY, takes
20 for a directory fd, `fdopendir` fails, and it closes fd 20, whichever TLS
socket holds it. Fixed in ps5compat (`port/deps/compat/atfuncs.c`,
`--wrap=openat,fstatat,unlinkat,utimensat`): positive results become -1/errno;
for openat a positive result is kept only if it is open on the named file
(same device and inode). After the fix: 20 stress loads, 0 failed, 0 EBADF
(before: 13 double closes of fd 20 per run).

**Payload count / page tables: fixed by one WebProcess (0018).** Findings
first (before the fix): during the
stress runs elfldr began failing to spawn WebProcesses ("Error spawning
payload", up to 6 retries), and two WebProcesses hit memory failures (libc
heap could not grow by 8 MB, SIGABRT; a libpas out-of-memory panic in GC).
Several WebProcesses stay alive across swaps (the YouTube process lived
413 s while file:// pages got their own, plus the prewarmed one), and each
costs ~300 of the system's 8192 CPU page tables (*Findings*: three exhausted
it). After the last run elfldr could not start any payload at all (not even
klogsrv). On the TV, ten WebProcesses were launched in 154 s and the
NetworkProcess ended just before the freeze. Patch 0018 makes the process
pool use a single WebProcess on PS5 (`setUsesSingleWebProcess`, checked
before every swap reason including COOP), with no WebProcess cache and no
prewarming (`PS5WK_PROCESS_SWAPS=1` in the UIProcess restores upstream). After
a reboot: page tables 4870/8192 idle; a 10-load stress run (2 WebProcess
launches) peaked at 6583 and returned to 4939; elfldr still spawned after
every run; 0 failed loads. Navigations also no longer upload a 114 MB
payload each.

**Video black at the start: found and fixed (0018).** Reproduced in the
harness on Sintel (video stuck at frame 69 from t=8 to 15 s around the
cookie dialog's pause, audio running). `GST_DEBUG=webkitmsesrc:7` showed the
video samples up to PTS 14.5 s already pushed, preceded by WebKitMediaSrc
still-frame events (sent whenever 100 ms pass without samples; YouTube's
appends come in bursts while the main thread is busy), and output resumed
exactly at the next keyframe (PTS 15). avdec handles a still-frame event by
draining, which flushes libav (`avcodec_flush_buffers`) and drops its
reference frames: every inter frame after it failed to decode (silently,
with 0016's `max-errors -1`) until the next keyframe. This is also the
earlier "24–35 s without video" on YouTube. 0018 drops still-frame events
before avdec on PS5, as WebKit already does for other decoders
(`PS5WK_STILL_FRAME=1` lets them through). After: Sintel played through
the pause without a gap (harness and title); with the events let through
again (A/B) the 12 s freeze came back.

**"Stutter" on YouTube (analysis):** Sintel on YouTube is 24 fps; the title
showed it at a steady 23.4–24.0 frames/s, 0 dropped, 2–3 ms after due. On a
59.94 Hz output, 24 fps can only alternate 2 and 3 vsyncs per frame (3:2
cadence), which judders on pans; the local clips are 30/60 fps (even
cadence). The `[video]` lines now include how many vsyncs each frame stayed
on screen (`held 1/2/3/4+ vsyncs` and the last 16 holds), to tell 3:2
cadence from pacing jitter: Sintel shows a clean `3232323...` (inherent
3:2, not jitter). For 60 fps, frame selection now has hysteresis (an older
due frame is dropped for a newer one only if the newer one is due a
quarter vsync inside the threshold): the local 1080p60 clip in the title
went from ~1 drop per 5 s to 0 in steady state. YouTube Big Buck Bunny
720p60 in the title: 54–59 frames/s with drops while the page loads (the
WebProcess at 1.2 cores drops late frames), then 58.7–59.7/s with 0–2
drops per 5 s at 0.34–0.41 cores.

### WebProcess out-of-memory crash in GC: found and fixed (0019)

TV test 2026-10-28 (evening): web5, YouTube's service-worker WebProcess,
died after 150 s with SIGILL in `pas_panic_on_out_of_memory_error` <-
`bmalloc_allocate` <- `JSC::MachineThreads::gatherConservativeRoots` on the
GC collector thread; the stress runs had the same stack after 122 s. The
payload had 8 GB of flexible memory and nothing else was short: the
allocation itself was absurd. `gatherConservativeRoots` copies every other
VM thread's stack, from the thread's stack pointer at suspension to its
stack origin, into one buffer. WTF suspends threads with SIGUSR1 and reads
the stack pointer from `&ucontext->uc_mcontext` (+0x10 in the SDK header),
`mc_rsp` = ucontext +0xc8. The PS5 kernel puts the mcontext at +0x40
(found for the crash guard, `test/sigctx/sigctx.c`), so +0xc8 is the
kernel's `mc_addr`. Probe `test/sigctx/suspend.c` (a worker faulting in
fresh heap pages, suspended 200 times the way WTF does): +0xc8 was **0 in
197 samples** (JSC then skips the thread: its stack was never scanned,
so objects only it referenced could be freed under it) and **a heap
address above the stack in 3** (size = origin - "sp" wraps: the
out-of-memory panic); the real rsp (+0xf8) was inside the stack 200/200.
Not an mmap size, alignment, address-space or page-table limit: no mmap
failed. Fix (0019): the mcontext at +0x40 on PS5, and a whole-stack scan if
a stack pointer is ever outside its stack. After it: a 300 s YouTube
harness session, no crash, no out-of-stack stack pointer reported, 0
failed mmaps, memory flat after 3 minutes (libpas 632 MB, 96 mappings
holding 855 MB). The title links the same WTF and gets the fix too.

The libc-heap failure of the stress runs ("could not grow by 8 MB",
`SceLibcInternalHeap`, SIGABRT on a thread start) happened while elfldr
was already failing to spawn payloads: the system page tables were
exhausted (fixed by 0018).

**Memory watchdog:** every WebProcess/NetworkProcess logs a line per
minute (`PS5WK_MEM_LOG=<s>`, 0 = off):

    [mem] web4 at 300 s: peak rss 553.0 MB; flexible free 8158.0 MB (+0.0 since start); libpas pages 632.4 MB, compact 0.3 of 128 MB; mmaps 96 live, 854.7 MB (peak 854.7, largest 128.0), 3912 calls, 3814 munmaps, 0 failed

All mmaps of the child go through `--wrap=mmap,munmap`
(`apps/child/ps5wk-child.cpp`): live count and bytes, the largest, and a
`[mem] mmap FAILED: <bytes> at <addr>, align 2^n, prot, flags, fd: errno
(caller)` line for each failure (written with `write(2)`, no stdio in
mmap). Payloads can't read their own RSS (`kern.proc.pid` is refused), so
it's getrusage's peak; flexible memory doesn't move with anonymous memory
in payloads (only `sceKernel*Flexible*` maps count). libpas pages are what
it keeps committed (0006: decommit only zeroes on PS5).

### Step 2b: Videodec2 in the title (built 2026-10-28)

The compositing path stays; only where frames come from changes. Built as
planned below, with these details:

- **WebProcess, `ps5vdecproxy`** (`port/deps/gst/gstps5vdecproxy.c`, in the
  `ps5video` plugin, rank PRIMARY+100): a GstVideoDecoder for H.264 (8-bit
  4:2:0 up to 4096x2304), HEVC Main/Main10 and VP9 profiles 0/2 (up to
  8192x4352), progressive. It takes avc/avc3/hvc1/hev1 as well as
  byte-stream (decodebin3 offers the parser's packetized caps; packetized
  input is converted to Annex-B here, codec_data's parameter sets before
  keyframes). It connects to the title's video server at READY (HELLO, role
  DECODER) and fails READY *without* an error message when the title has no
  decoder; anything outside the list is refused in the accept-caps query.
  decodebin3 then takes avdec_*/dav1d: software stays the fallback (and
  the only path in `ps5wk-shot`). Each AU goes to the title inline on the
  socket with the frame number; PICTURE replies (display order) finish
  frames with a 16-byte placeholder buffer (`struct ps5wk_remote_picture`:
  decoder, token, epoch) in `video/x-raw(memory:PS5Remote)` caps; VP9
  hidden frames, frames before the first keyframe and errors are released.
  At most 32 frames pending (was 16: see *Hover previews* below). Seek =
  DFLUSH with a new epoch; EOS = DDRAIN. Waits for the title end at once on a
  flush or PAUSED -> READY.
  Pictures outside the segment (decoding restarts at the keyframe before a
  seek target) are released at once (PRELEASE): the base class would clip
  them and the title would never get the slot back.
- **`ps5videosink`** accepts the remote caps and sends a REMOTE FRAME
  (decoder + token, due time as before); `prepare()` reports placeholders it
  dropped as late without rendering (RDROP). playbin's native-video flag is
  already set with this sink, so nothing converts in between.
- **Title, `PS5VideoDecoder`** (`wpe-ps5/PS5VideoDecoder.cpp`): per decoder
  connection a receiver (AU queue) and a decoder thread. Output buffers are
  persistent GL buffers made on the render thread (`allocateVideoBuffer`);
  12 slots up to 16 MB frames, 9 up to 50 MB, 8 above. The decoder picks a
  free slot per `Decode` call (`vdec2_decode_into`, new in Spike 9's vdec2)
  and gets each picture's own pts back with **`sceVideodec2GetPictureInfo`**,
  read from the 13.60 disassembly: the decoder keeps 1 KiB of metadata per
  picture at the end of the output buffer; the picture info's `thisSize`
  must be the codec's (AVC 0x68/0x78, HEVC 0x88/0x98/0xa8/0xb8, VP9 exactly
  0x58), `ptsData` is at +0x10 (the PS4 layout). The pts given is the frame
  number, plus bit 62 for VP9 hidden frames (superframes split here), so
  alt-ref pictures are dropped by pts, not by call order (Spike 9's
  off-by-one). H.264/HEVC parameter sets are cached and put back in front
  of the first keyframe after a reset. Flush = `vdec2_renew`. A picture
  sent to the WebProcess is the WebProcess's until it comes back: when
  the consumer has presented it (pinned while drawn, released after the
  swap) or skipped it, on PRELEASE (the proxy's drops, and every
  placeholder buffer freed before the sink handed it over), on a flush, or
  when the connection closes. The title never takes one back by itself
  (until 2026-11-01 it did after 1.5 s without a free slot: see *Feeds
  with several videos* below).
  Decoders are opened at 3840x2160 for VP9 (13 MB of flexible memory) and
  at least 1920x1088 for H.264, so YouTube's resolution switches within
  that keep the decoder; pipeline depth 4 up to 4K, 2 above (8K: 139 MB of
  flexible memory instead of 223).
- **Renderer**: a REMOTE frame converts straight from the decoder's GL
  buffer (no memcpy), NV12 or the 10-bit LSB-aligned variant (`format_ 4`).
- **Backend**: the decoding itself is a function table the executable
  installs: Videodec2 in the title (`apps/title/titlevdec.c`; `vdec2_load`
  runs first thing in `main`, before the unjail), a fake in ps5wk-shot
  (`fakevdec=1`: empty pictures in display order by PTS, so the whole
  WebProcess side and the slot bookkeeping run in payload-only tests).
- The WebProcess asks the title at start (QUERY role) and raises
  `WEBKIT_GST_VIDEO_DECODING_LIMIT` to 7680x4320@60 when it decodes in
  hardware: YouTube offers VP9 up to 2160p60, and `hvc1`/`hev1` are
  advertised (h265parse and a decoder exist).

**Measured** (title, log-only runs 2026-10-28, audio through sceAudioOut at
volume 0, video in a 1280x720 element; clips from `test/media/make-media.sh`):

| clip | shown | decode call | WebProcess | title flexible memory free | direct memory |
|---|---|---|---|---|---|
| H.264 1080p30 (software before: 0.31 cores) | 30.0/s, 1 drop at the start | 1.0–1.2 ms | **0.05–0.06 cores** | 299 -> 274 MB | decoder 70 MB + 12 x 3.2 MB |
| HEVC 3840x2160p30, x265 with B-frames, 20 Mbit/s | 30.0/s, 1 drop | 1.8–2.3 ms | 0.06 cores | 298 -> 221 MB | 337 MB + 12 x 12 MB |
| HEVC Main10 3840x2160p30 | 30.0/s, 2 drops | 0.6–1.4 ms | 0.05 cores | 297 -> 221 MB | 512 MB + 9 x 23.9 MB |
| HEVC 7680x3840p30, 68 Mbit/s (software: 20 fps at most) | 30.0/s, 1 drop | 12.6 ms (≈79 fps of capacity at depth 2) | 0.07 cores | 297 -> 157 MB | 941 MB + 9 x 42 MB; 1856 MB in use in all |
| VP9 1080p30 (WebM) | 30.0/s | 1.0 ms | 0.05–0.06 cores | 297 -> 284 MB | 535 MB + 12 x 12 MB |
| YouTube Big Buck Bunny VP9 **3840x2160p60** (MSE) | 59.3–59.7/s, 0–2 drops per 5 s after the start | 1.4–3.2 ms avg | 0.22–0.45 cores (YouTube's JavaScript) | 297 -> 284 MB | as above |

0 decode errors and 0 pictures without a pts in every run (3931 pictures,
305 VP9 hidden frames dropped, in 66 s of YouTube 4K60). The title's
conversion pass: 0.3 ms per 1080p frame, 0.7 ms 4K, 1.5 ms 4K 10-bit,
3.1 ms 8K; the title presents 59.9 frames/s throughout. Decoder bring-up
3–95 ms (VP9 3 ms); the proxy's first DOPEN round trip 150–250 ms.
Direct memory in use goes back to ~595 MB (the browser's baseline) when a
decoder closes. Seeking (local 8K: forward, back, paused; YouTube
`seekTo`), YouTube's 720p -> 1440p -> 2160p switches and switching videos
all resumed; screen readbacks showed the decoded frames correctly under the
page (8K clip, YouTube 4K). Harness (fake decoder): local H.264 with seeks,
YouTube with the cookie dialog's pause, 87 VP9 hidden frames; without a
decoder (normal harness) avdec plays as before.

**Knobs:** `/data/ps5webkit/no-hwdec` (title: no hardware decoding,
software in the WebProcess as before), `child.env`: `PS5WK_HWDEC=0` (the
WebProcess doesn't ask), `ps5wk-shot fakevdec=1`. Logs: `[vdec]` (title:
backend, each decoder's memory, every 5 s AUs/pictures/decode time/slots),
`[ps5vdecproxy]` (WebProcess, every 5 s). Title test lists take a dwell per
URL (`<url> 40`), screenshots also at +30 s for dwells of 34 s or more;
`test/media/title-seek-and-2160.js` (title `test-onload.js`: seeks on
video.html, 2160p and a seek on YouTube), `test/media/yt-2160.js`.

**TV test of 05afe91 (2026-10-28, evening): YouTube 4K stuck after a few
seconds, found and fixed.** The owner's YouTube session (more JavaScript:
the WebProcess at 1.0–1.2 cores, the player re-laid out once) played
VP9 2160p60 for ~5 s, then the decoder sat at "12/12 slots held, 0 pinned"
and only the 1.5 s take-back moved it (0.7 pictures/s); local HEVC clips
were fine. Two places dropped late placeholders *without anybody telling
the title*, so their decoder slots stayed held:
1. GstVideoDecoder's own QoS (since 1.18) drops output buffers later than
   the sink's last QoS report inside `finish_frame`.
2. GstBaseSink checks lateness *before* calling `prepare()` (when a sink has
   one) and drops the buffer without `prepare()` or `render()`, so the
   RDROP report in `prepare()` never fired for late buffers.
The slot bookkeeping only recovered when a later frame got rendered; a
burst of more than 12 late frames took every slot, nothing could be decoded
in time again, and every new picture was late too. My log-only runs had a
lighter page and never more than a few late frames in a row. Reproduced in
the harness with a slow fake decoder (`ps5wk-shot fakevdec=1
fakevdecdelay=22`: 12/12 held, 0.7 pictures/s, as on the TV). Fix: the
proxy turns the base class's QoS off (`qos` FALSE) and drops late pictures
itself (`gst_video_decoder_get_max_decode_time` < 0) with a PRELEASE;
`ps5videosink` sets max-lateness -1 for hardware-decoded caps, so it never
drops one silently (the title skips stale frames at each vsync and releases
them); the take-back safety net returns the older half of the held slots
at once. Harness after the fix: a 6 s burst of a too-slow decoder
(`fakevdecdelay=22:6`) drops 360 late pictures, then plays 60.0 frames/s
with 8/12 slots in use; seeks and YouTube unchanged. `ps5wk-shot
childenv=K=V;K=V` (new) sets environment for that run's children only
(`/data/ps5webkit/child-<pid>.env`), e.g. GST_DEBUG, without touching the
title's.

**Resolution switches inside a stream (BBC News, found and fixed
2026-10-28).** In a log-only run, BBC News's embedded H.264 video (dash.js,
avc3: parameter sets in band) went from 396x704 to 540x960; the title kept
the decoder (the new size fits the 1920x1088 one) and Videodec2 failed
every decode call after the switch (960 x rc 0x811d0300, no picture).
Reproduced with `media/switch-h264.mp4` (make-media.sh: 396x704 -> 540x960
-> 1280x720 -> 396x704 in one avc3 MP4, 5 s each): 0 pictures after the first
switch. Videodec2 needs a drain and a reset before a new sequence, even
one that fits. Two fixes, both drain the pictures of the old format first
(they are shown) and then `reset` the decoder: the proxy drains (DDRAIN)
before a DOPEN for another size or depth (GstVideoDecoder doesn't drain on
new caps); and the title drains and resets by itself when a keyframe brings
a different SPS (or HEVC VPS/SPS) than the one it decoded, for switches
whose caps keep the container's size (an MP4 track with in-band SPS).
Identical repeated SPSs (codec_data's copy plus the in-band one) don't
count. A decoder that no longer fits is still reopened as before; slots
follow the existing rules (the drain hands out or releases every picture).
Checked in the title: `switch-h264.mp4` with seeks (an in-band switch and a
seek across one) and `switch-h264.m2ts` (MPEG-TS: the caps follow each SPS,
three proxy drains) play every picture, 0 decode errors (598 of 601 shown,
1 dropped); YouTube 4K60 with the switch to 2160p and a `seekTo`: 5761
pictures, 0 errors, ~60 frames/s. Harness (fake decoder):
`switch-h264.m2ts` at 30 frames/s through the switches. The decoder's
close line counts `format changes`.

**Hover previews that froze the page (a VR site, found 2026-10-30).** Listing
pages play small H.264 High clips on hover (380x222 `avc1.640016`, level
2.2; 440x248 `avc1.64001E`, level 3.0). The title's decoder never sent a
picture (17 AUs sent, 0 pictures), and when the page removed the preview the
WebProcess hung until the browser was restarted. Two causes. (1) As far as
the counts tell (to be confirmed with preview.html), Videodec2 holds
pictures until the stream's DPB is full, sized from the level and the
picture size (16 frames at these sizes, the maximum), plus its pipeline
depth of 4: about 19-20 AUs before the first picture (BBC's 832x468 level
3.1, DPB 11: 26 AUs, 12 pictures; a 500x300 level 2.1 preview, DPB 7,
played). The proxy stopped sending at 16 frames pending and waited for a
picture: deadlock. The limit is now 32 (the title queues up to 64 AUs).
(2) `handle_frame` waited for that picture in a loop that never looked at
flushing; removing the video sets the pipeline to NULL on the WebProcess
main thread, whose sink pad deactivation waits for the streaming thread:
the main thread stopped ("0 page uploads"). The waits now end at once on
FLUSH_START or PAUSED -> READY (the pads' flushing flags and a wakeup pipe
in the poll), and after 9 s without any reply from the title the stream
fails with a decode error instead of waiting. Test:
`test/media/preview.html` (uploaded with the clips by `make-media.sh
--upload`) cycles 380x222/440x248 previews made with FFmpeg, with and
without the SPS's VUI reorder limits (`strip-vui-restriction.py`), each
removed after 2.5 s; `PREVIEW` lines give the frames shown and how long the
removal took. The level 2.0 clip (DPB 7) is the control that should play
even with the old limit.

**Feeds with several videos: stalled decoders (found and fixed
2026-11-01).** On a feed page with several autoplaying videos (the owner's
session, `tv18.log`), some videos spun forever or crawled, and the title
logged `no free output slot for 1.5 s: took back 6 pictures` 34 times over
24 decoders (19 over 15 the session before), 0 decode errors. Who held the
pictures: the title decodes ahead into 12 output slots and keeps each
picture until the WebProcess gives it back. A video that is not playing
legitimately holds them: a paused or prerolled player (preload, an autoplay
that waits, a player resumed later) has 1 picture in the sink's preroll, 3
in playsink's `vqueue`, 1 in the decoder's blocked push and the rest
decoded but unread in the proxy's socket (it reads replies only while it
handles input); WebKit also suspends muted videos outside the viewport
(`managePlayerSuspend`: non-MSE pipelines to NULL, resumed with a seek; MSE
players to PAUSED, decoder kept). With up to 32 access units in flight the
decoder filled all 12 slots and waited, and after 1.5 s the safety net
**took back the older half of pictures the WebProcess still owned**,
decoded more into them, and again every 1.5-2 s. When the player then ran,
its frames named pictures that had been overwritten: the title skipped
them (a frozen start, then a jump: 19-65 frames dropped per resumed
player), and the decoder had thrown away whole runs of pictures. Each
round also sent 6 more replies into the unread socket. The safety net
existed because pictures used to be dropped without a release (QoS, TV
test 2026-10-28). Now:
- **Ownership instead of a timeout**: a picture is the WebProcess's from
  PICTURE until the sink hands it to the title (FRAME) or it is given back;
  the decoder simply waits for a slot (one log line after 10 s: `every
  output slot held for 10 s ... the video is paused or hidden`).
- **Every unrendered picture is given back by its buffer**: the proxy's
  placeholder is now memory with a free callback; freed before the sink
  sent its FRAME (dropped anywhere downstream, flushed, never rendered,
  pipeline torn down) it sends PRELEASE from whichever thread frees it,
  through a refcounted connection object (sends under its lock, so a
  release never lands inside an AU, and a new connection after a restart
  gets a new object: old placeholders can't release a new decoder's
  pictures). The sink sets `handed_over` when its FRAME went out, and no
  longer sends the preroll picture a second time when the same buffer is
  rendered on PLAYING (the title had shown it and freed its slot). RDROP is
  no longer sent.
- **Releases are exact**: PRELEASE and a skipped frame free exactly that
  picture (they used to free every older unpinned one too, which could free
  pictures still queued in front of a late one); only a presented picture
  still frees older held ones of its decoder, counted as `lost without a
  release` (0 in every run since).
- The title's decoder connection never waits on its AU queue any more (the
  PRELEASEs that free slots come on that socket; the proxy bounds the
  queue at 32), and the proxy's reply waits retry after a signal (a
  profiler run showed `poll` interrupted by SIGPROF making the title
  "unable" to decode: 8 videos fell back to software).
- Logs: `decoder N from pid P closed: ...; K PRELEASEs, W waits of 10 s for
  a slot, L pictures lost without a release`, the stats line's `lost
  without a release`, the proxy's `given back unrendered`, the sink's
  `preroll frames not sent again`.

Test: `test/media/feed.html` (clips from `make-media.sh`: `feed-*`, 480x854
and 854x480 H.264 Main 3.1 with B-frames and AAC, VP9 + Opus, fragmented
MP4 for MSE), served with `tools/serve-media.py --port 8773 --page
port/webkit/test/media/feed.html` (now with byte ranges; MSE from a file://
page fails with MediaError 4). Seven players: autoplaying muted, one left
paused after preroll then played, an unmuted autoplay, two MSE players
(video and audio SourceBuffers, appending a few seconds ahead), one seeking
into the middle of a GOP at load, a VP9 one, players below the fold;
scrolled in and out, paused and resumed, removed and added, an MSE seek
while suspended. `FEED` lines every 2 s, `FEED STUCK` only while a player is
at least half visible. Log-only title runs (same timeline):

| | before (2 runs: file://, http) | after (3 runs: http, file://, http with the wall profiler) |
| --- | --- | --- |
| `took back` / `no free output slot` lines | 10 and 15 (60 and 90 pictures) | 0, 0, 0 |
| frames dropped in the first 2 s by players resumed from a paused preroll (hidden at load, then scrolled in: E, F, M) | 19, 25, 19 | 0, 0, 0 in every run |
| the MSE player M after 16 s suspended (PAUSED) | 2 more dropped | 1 |
| decoders in steady play | 30.0 pictures/s | 30.0 pictures/s, 7/12 slots held |
| pictures lost without a release / PRELEASEs | (not counted) | 0 / 211-572 per run |
| visible players stuck | B once, when played from its preroll (2-4 s, see below) | the same |

What the test also found (open, not caused by the decoder slots; also
before the fix and with software decoding):
- **The page froze when players resumed** (found and fixed the same day:
  seamless looping's non-flushing seek, see *Feeds: spinners over playing
  videos*) in all 3 runs served over http
  (never from file://), at a scroll that suspended some players and resumed
  others: the WebProcess main thread stopped (`0 page uploads`, no more
  `FEED` lines, the title had to end the WebProcess). A wall-clock profile
  (`PS5WK_PROFILE=wall`) shows it in WebKit's resume-from-suspend seek
  (`MediaPlayerPrivateGStreamer::updateStates` -> `doSeek` -> seek event
  -> `gst_qtdemux_handle_src_event` -> waiting for qtdemux's sink-pad
  stream lock): qtdemux's streaming thread doesn't let go, presumably
  blocked on data from webkitwebsrc, which needs the main thread. Not seen
  in the owner's session (its page kept painting). Next: a CPU-mode
  profile can't show idle threads; a stack dump of every thread is needed.
- **Every progressive A/V clip drops ~41 frames (1.4 s) when it first
  plays** (A, B, D; MSE players don't), before and after the fix; a player
  played from preroll (B) stands still for 2-4 s first. Looks like the
  audio clock at start; not looked into yet.
- **Memory per decoder**: H.264 decoders are opened at 1920x1088 (24 MB of
  the title's ~283 MB free flexible memory, 70 MB direct plus 12 x 3.2 MB
  output buffers: 3 decoders took direct memory in use from 589 to 916 MB),
  VP9 at 3840x2160 (13 MB flexible, 535 MB direct plus 12 x 12 MB: three
  VP9 and one H.264 decoder reached 1845 MB in the owner's session). No
  allocation failed; the direct memory budget wasn't the limit at 3-4
  decoders, flexible memory would be at about 10 H.264 decoders (WebKit keeps
  a suspended MSE player's decoder: a long feed accumulates them; past the
  limit new videos fall back to software at the accept-caps DOPEN).

**Feeds: spinners over playing videos, and the resume freeze (found and
fixed 2026-11-01, patch 0036).** On a feed of autoplaying muted videos (the
owner's session, `tv19.log`: decoding healthy, 0 take-backs, 30-35
pictures/s) some videos kept the page's loading spinner on top while they
played, and the log had ~30 `Autoplay blocked with reason:
PageConsentRequired` lines. A page decides its spinner from what it reads:
media events, `readyState`, `buffered`, `currentTime` progress, play()
promises and `requestVideoFrameCallback`. Several of those were wrong:
- **`requestVideoFrameCallback` never called back.** Only the accelerated
  path (`pushTextureToCompositor`) counted presented frames; with
  `ps5videosink` (hole punch) and with CPU painting the count stayed 0, so
  `videoFrameMetadata()` never had a new frame and every callback stayed
  pending. Players that hide their poster/spinner on the first frame
  callback, or show it while callbacks stop, kept it up. Each new sample
  is now counted in `triggerRepaint`.
- **`readyState` stuck at HAVE_CURRENT_DATA (2) while playing**, so the
  page got `waiting` and never `playing`: progressive players after a loop,
  after a seek, and after WebKit's off-screen suspend/resume (muted videos
  outside the viewport). `updateStates()` "delays" a buffering change that
  arrives during an async state change by putting `m_isBuffering` back,
  expecting a later buffering message; (a) a download that reached 100%
  during the resume seek sent no further message, so the change was lost
  for good, and (b) `m_wasBuffering` keeps the value from before the last
  update, so after buffering had completed, every later async state change
  (each loop or seek) "delayed" that old change again. Now only a change
  that came with the current buffering update is held back, it is applied
  when the state change completes, and the download-complete message runs
  the normal buffering update.
- **MSE loops stuck in `seeking`.** At the end of a looping MSE video the
  element seeks to 0 from the ended notification, synchronously, and then
  the MSE player's `didEnd()` tore the pipeline down under that seek: the
  seek never completed (no `seeked`, `currentTime` held at 0 for good)
  while the pipeline rebuilt by play() played underneath, and the title's
  decoder was reopened on every loop. The pipeline is now kept when the
  ended notification started a seek.
- **The page freeze** (previous section, all http runs): a wall-time profile
  of every thread (`PS5WK_PROFILE=threads`, new) showed the WebProcess main
  thread in `gst_qtdemux_handle_src_event` -> `gst_pad_pause_task` (a
  *non-flushing* seek waiting for qtdemux's streaming thread), qtdemux's
  pull loop in `gst_download_buffer_wait_for_data`, and webkitwebsrc's
  threads waiting for data, which WebKit's loader only delivers on the main
  thread: a deadlock. The non-flushing seek is upstream's seamless looping
  (a SEGMENT seek after every flushing seek and at play(), for looping
  videos), here after the resume seek. Upstream disables on-disk buffering
  for looping media for this reason (gstreamer#3129), but only through
  playbin's flag, which playbin3 ignores (WebKit sets urisourcebin's own
  `download` property). Sending the SEGMENT seek from a GStreamer thread
  instead only moved the hang: that thread held the pipeline's state lock
  while it waited, and the main thread's next seek waited for that lock.
  Seamless looping is now off for http(s) progressive media (loops use the
  flushing seek); MSE and local files keep it.
- **The `PageConsentRequired` lines were not the policy.** The title's
  website policy is `WEBKIT_AUTOPLAY_ALLOW` (since Milestone 2; ps5wk-shot
  too): muted and unmuted autoplay are allowed, the test page's unmuted
  autoplay played and no play() promise was rejected in any run. WebKit
  prints that line from `setReadyState` when an element reaches
  HAVE_ENOUGH_DATA and the `autoplay` *attribute* path has nothing to do:
  `!autoplay` (no autoplay attribute: the page calls play()), `!autoplaying`
  (play() was already called), `!paused` (already playing). It needs
  HAVE_ENOUGH_DATA, so it cannot explain videos that never got a source
  (the product page in *Profiling*: `readyState` 0, a page-side decision).
  The line is now printed only for an element that has the attribute and is
  still waiting to autoplay. Policy kept: ALLOW (a TV browser, pad input
  isn't always on the video; desktop browsers allow muted autoplay and
  Safari's "stop media with sound" would break YouTube's autoplay).

Test: `test/media/feed-players.html` (clips from `make-media.sh`: `feed-*`),
served with `tools/serve-media.py --port 8774 --page
port/webkit/test/media/feed-players.html`. Eight feed players: MSE ones
that append one fragment at a time up to a buffer goal (2, 4, 6, 30 s, as
hls.js/dash.js) and progressive ones (H.264, VP9), muted autoplay through
the `autoplay` attribute and through play(), IntersectionObserver
play/pause at 50% visibility (some players not paused by the page, so WebKit
suspends them off screen), scrolls, pause/play, seeks, loops. Every player
computes six spinner models from standard signals only (E events, T
video.js-style, R readyState, B buffered ahead, P currentTime progress, V
frame callbacks) and logs every media event, play() results, a state line
every 2 s and `FP SPIN <player> <model>` when a model's spinner stays on
for 2 s over a visible video whose currentTime advances. Desktop Chrome: 0
SPIN, all play() resolved. Log-only title runs (same page; the "before"
build is the start of this round):

| | before | after (clean run, no debug logging) |
| --- | --- | --- |
| `FP SPIN` (spinner on 2 s over a playing video) | 22: 16 frame callbacks (`v0`: no callback at all), 3 events, 3 readyState | 1: progressive WebM `buffered` empty (below) |
| state samples with readyState 2 while visible and playing | 20 | 0 |
| `seeking` / `seeked` events | 6 / 2 (MSE loops never finished) | 11 / 11 |
| play() rejected / `PageConsentRequired` lines | 0 / 19 | 0 / 0 |
| page | froze at t=72 (resume scroll), WebProcess ended by the title | ran to the end (96 s), in 3 runs |

Found in the test page itself on the way: its MSE pump called
`endOfStream()` while the other SourceBuffer was still appending; the next
`appendBuffer()` reopened the MediaSource (as the spec says), `sourceopen`
fired again and added SourceBuffers, and the video track, already told
"all samples enqueued", froze while the audio clock ran on (also once a
MediaError 4 "Failed to push buffer" at start). Fixed in the page; WebKit
cannot revive a GStreamer stream that got EOS before such a reopen (rare:
players don't append after `endOfStream()`). Still open: progressive WebM
over http reports empty `buffered` while playing (WebKit turns download
buffering off for WebM and the stream-mode buffering query gives no
ranges); the ~41 frames dropped when a progressive clip first played
(previous section) were 0-3 per player in the last runs, not looked into.

Not done yet: HDR (10-bit BT.2020/PQ streams are shown with the BT.709
matrix, no tone mapping); canvas `drawImage` of a hardware-decoded video
draws nothing; two 8K decoders at once won't fit the title's flexible
memory (the second waits up to 3 s, then the proxy fails that stream).

**Decoder budget (2026-11-01, COMPOSITING-PLAN.md *M6*).** The title admits
at most 8 hardware decoders at once, with their memory (decoder + output
buffers, measured around each open) within 2048 MB of direct memory, and
only while 64 MB of flexible memory stay free. An open over budget waits up
to 0.5 s for decoders to close (off-screen videos being suspended), then is
refused, and that pipeline run decodes in software (a suspended video that
comes back asks again). VP9 gets its 4K headroom only for streams above
1080p while it fits in half the budget; otherwise its decoder is sized to
the stream (at least 1920x1088). Muted videos made off screen (a feed at
load) go to their suspend state once prerolled, like videos that scrolled
away (patch 0052; `PS5WK_MEDIA_SUSPEND_PREROLLED=0` in child.env: the old
behaviour). `/data/ps5webkit/vdec-budget` overrides it (`decoders= direct=
reserve= wait= vp9headroom=0/1/2`); the open/close lines log each
decoder's cost and `budget N decoders, X MB (peak ...)`.

The plan this followed:
1. **WebProcess:** a proxy decoder element, `ps5vdecproxy` (klass
   Codec/Decoder/Video, rank above avdec_* for H.264, HEVC 8/10-bit and VP9
   from the parsers, AU-aligned byte-stream), sends each access unit to the
   title over the same connection (AUs in the shared-memory ring, an AU
   message with PTS/DTS/keyframe/codec data), and outputs a placeholder
   buffer per frame (`video/x-raw(memory:PS5Remote)`, PTS-ordered through a
   small reorder queue sized from the SPS) that `ps5videosink` accepts. So
   playbin, QoS, A/V sync, MSE and the FRAME/GEOMETRY messages work as now;
   FRAME names a PTS instead of a slot. AV1 (no hardware) and anything
   Videodec2 refuses stay on dav1d/avdec with frames as now.
2. **Title:** a decoder thread per stream (Spike 9's `vdec2`: one decoder
   per codec, pipeline depth 2–4) decodes the AUs ahead of time, straight
   into output slots that are persistent GL buffers from the renderer
   (direct memory type 12, GPU VA = CPU VA), and queues each picture with
   its PTS (`sceVideodec2GetPictureInfo`; VP9 alt-ref pictures dropped by
   PTS, as Spike 9 found). `PS5VideoServer::consume` pairs FRAME messages
   with decoded pictures by PTS and hands the GL buffer to `handleVideo`:
   no memcpy, and the conversion pass gains the 10-bit LSB-aligned variant
   (Spike 9's shader). Flush/seek resets the decoder.
3. **Budget:** flexible memory 24 MB for 1080p H.264 up to ~140 MB for 8K
   HEVC at depth 2 (Spike 9), against 299 MB free in the title today; an
   output slot pool of about depth + 4 frames per stream.
4. **"View in VR"** samples the same RGBA texture on a sphere or dome
   (Spike 9's `vr360`/`vr180`), so it needs nothing from the page.

## HLS streams (`<video>` with an .m3u8, 2026-10-30)

A video site's player put an HLS master playlist (a signed nginx-vod
`.urlset/master.m3u8`) in a `<source>`, which failed at once (networkState 3,
nothing fetched): WebKit only advertises `application/vnd.apple.mpegurl` /
`application/x-mpegurl` with `WEBKIT_GST_ENABLE_HLS_SUPPORT=1`, and it always
demotes adaptivedemux2's `hlsdemux2` (it downloads with its own libsoup,
which a sandboxed media process can't).

- **ps5wk-child** sets `WEBKIT_GST_ENABLE_HLS_SUPPORT=1` (child.env can set
  0). `canPlayType('application/vnd.apple.mpegurl')` now says "maybe", so
  players that prefer native HLS (Safari-style) use it instead of hls.js +
  MSE.
- **Patch 0033** keeps `hlsdemux2`'s rank on PS5 when HLS is on. The
  WebProcess is a payload with libsoup and the same TLS setup as the
  NetworkProcess, so hlsdemux2's own downloads work. webkitwebsrc fetches
  the master playlist (through the NetworkProcess, with cookies), hlsdemux2
  the variant playlists and segments. It takes the User-Agent and Referer
  from webkitwebsrc's http-headers event; the User-Agent is added there by
  0033, since the NetworkProcess sets it on the real request only. **Not
  sent: the page's cookies** (WebKit keeps them in the NetworkProcess), and
  the requests skip WebKit's CORS checks. Signed segment URLs need neither.
  hlsdemux2 also plays separate audio renditions (`EXT-X-MEDIA`).
- **First try, dropped:** gst-plugins-bad's legacy `hlsdemux`, which
  downloads through webkitwebsrc. playbin3 (WebKit's choice for all
  playback on GStreamer 1.28) refuses it: "Adaptive demuxer is not
  streams-aware". With `WEBKIT_GST_USE_PLAYBIN2=1` it fetched the first
  segment and then stalled. It is not built.
- Downstream nothing changes: tsdemux or qtdemux, h264parse/h265parse, the
  hardware decoder (`ps5vdecproxy`) or avdec, aacparse and avdec_aac. A
  variant switch is a new SPS (TS) or a new init segment (fMP4), so caps
  change mid-stream and the proxy drains and reopens the decoder as for
  BBC's switches (*Resolution switches* above).

**Checked in the title** (log-only, 2026-10-30): `hls-ts` over HTTP and
from file:// played to the end, 640x360 -> 1280x720 at 2 s (the proxy
drained, the title kept its 1920x1088 decoder), 1200 pictures, 0 decode
errors, 1 frame dropped, 30 frames/s, audio to the title. `hls-fmp4` (HEVC)
over HTTP: 960x540 -> 1920x1080 reopened the decoder (8 frames dropped at
the switch, none after), 0 errors. Every request after the master playlist
came from hlsdemux2 with the browser's User-Agent (`tools/serve-media.py`).
A first HEVC stream with x265's default open GOPs dropped ~5 frames at every
segment start: ffmpeg's segments overlapped in PTS (the keyframe and a B
frame with the same timestamp); `make-media.sh` now encodes closed GOPs.
Not checked: seeking in an HLS stream, AES-128 segments, live playlists.

Test streams: `test/media/make-media.sh` makes `hls-ts/` (640x360 and
1280x720 H.264 + AAC, 2 s TS segments) and `hls-fmp4/` (960x540 and
1920x1080 HEVC + AAC, fMP4), each variant labelled, the low one listed
first so hlsdemux2 starts there and switches up. `video.html` plays them from
file:// (`--upload` copies them) and takes `&type=<MIME>` to load through
`<source type=...>` as players do (it logs `canPlayType`, the `<source>`
error and `resize` events with the new size). Over HTTP from the Mac:
`tools/serve-media.py` (port 8771, logs each request's User-Agent and Range),
then
`file:///data/ps5webkit/video.html?src=http://<mac>:8771/hls-ts/master.m3u8&type=application/x-mpegURL`.

**4K60 H.264 HLS froze for good (a VR site on the TV, found and fixed
2026-10-30).** 2160x1080 Main level 5.0 switching up to 3840x1920 60 fps
level 5.2 played for a few seconds after each switch up. Then Videodec2 failed every AU
with `0x811d0304` in 0.08 ms, through keyframes too, and the picture stopped
while the audio went on. Only a switch down, whose drain resets the decoder,
brought it back. Three things, all fixed:
1. **Videodec2's error states are sticky** (libSceVideodec2 13.60,
   disassembly). `sceVideodec2Decode` keeps flags in the decoder object
   (+0x48, +0x4c, +0x50). Once a decode sets one, every later call returns
   that code at once, without decoding, until `sceVideodec2Reset` (which
   zeroes +0x44..+0x53) or a new decoder. `0x811d0300` comes from VdecCore's
   `0x80c0000f` (decode status 0x18, hardware result 9): a new sequence
   without a reset, BBC's switch. `0x811d0304` comes from `0x80c00013`
   (status 0x15 or 0x1a, hardware result 3 or 11). `0x811d0100` is a
   decode after a Flush. The other errors are per AU: `0x811d0301`
   (VdecCore 0x80c00012), `0x811d0302` (0x80c00011), `0x811d0303`
   (0x80c00010). The title never reset a decoder on errors. **Now**
   (`PS5VideoDecoder.cpp`) a sticky code, or 8 failed calls in a row,
   resets it: `vdec2_renew` (delete, create, Reset), the pictures in
   flight are dropped, and decoding resumes at the next keyframe, with
   the cached parameter sets in front. If the failing AU is a keyframe,
   it goes to the fresh decoder again at once, so no GOP is lost. The
   first failure of a run logs the AU (keyframe or not, NAL types), slot
   waits and call times; the 5 s line counts `resets after errors` and
   pictures the decoder `flagged`. Each new H.264 SPS is logged with its
   profile, level, size, reference frames, VUI reorder and DPB, and the
   DPB the level allows at that size (`MORE THAN THE LEVEL ALLOWS` when
   exceeded).
2. **What put it there (synthetic repro): the IDR of an HLS segment was
   lost.** `make-media.sh`'s `hls-vr/` (2160x1080 level 5.0 and two
   3840x1920 60 fps level 5.2 variants with different references and
   B-frames; `mixed.m3u8` switches between them every few segments) gave
   exactly the TV log's picture. `0x811d0304` hit the second AU of every
   segment that switched variants, and two AUs fewer than the files hold
   reached the title per switch. tsdemux checks MPEG-TS continuity
   counters. Segments from separate muxers jump there, and on a jump it
   dropped both the unfinished PES and **the packet that starts the next
   PES**. That threw away the new segment's whole first PES: its IDR,
   with the SPS/PPS. Videodec2 then got P frames with no reference
   picture, under the old variant's SPS. **Patch
   `port/deps/patches/gst-plugins-bad/0001-...`**: a packet that starts a
   PES is kept after a jump, and only the unfinished PES before it is
   dropped (one AU per jump). The child logs
   `[tsdemux] continuity jump on pid ...` (first 10, then every 100th).
   hlsdemux2's own variant switches (`master.m3u8`) reset tsdemux and
   never hit this. Whether the site's segments jump in continuity (each
   segment muxed on its own) is for the TV check: the `[tsdemux]` line
   will tell.
3. **Proxy:** codec_data's parameter sets go in front of a keyframe only
   when it has no SPS and PPS (and VPS) of its own. Two copies that
   differed would put two different sets with one ID in an AU (PS5VR saw
   `0x811d0301` from that).

Checked in the title (log-only, 2026-10-30; `hls-vr` over HTTP from the
Mac, `tools/serve-media.py`):

| run | before | after |
| --- | --- | --- |
| `mixed.m3u8` (6 switches, 2520 AUs) | 2508 AUs reached the title; 27 errors (`0x811d0304` and `0x811d0301`), 6 resets; without the reset it froze at the first switch | 2514 AUs, **2514 pictures, 0 errors**, 5 in-band SPS changes drained and reset, 60 frames/s at 3840x1920 |
| `master.m3u8` (hlsdemux2's ABR, up, a↔b, down) | 0 errors | 0 errors |
| `switch-h264.mp4` with `vdec-inject noseqreset` (sticky `0x811d0300` on hardware) | 0 pictures after the first switch (BBC) | 3 resets, the keyframe decoded again each time: 588 of 600 pictures |
| `refs16.m3u8` (16 references at level 5.2: not conforming) | — | `0x811d0302`/`0x811d0303` from the first IDR; reset at every keyframe, no picture (not decodable by a level-sized DPB; software would be needed) |

The same build played `sync-1080p30-h264.mp4` (avc1, codec_data only),
`hls-fmp4` (HEVC with a switch), `sync-2160p30-hevc.mp4` and VP9 with 0
errors. Test knobs, `/data/ps5webkit/vdec-inject` (words, read at each
decoder open): `noseqreset` (skips the drain at a new in-band SPS, which
makes the sticky `0x811d0300` happen), `norecover` (the old behaviour),
`corrupt=<key>` and `truncate=<key>` (damage or cut that AU). The fake
backend fails with `0x811d0304` from call n until a reset under
`PS5WK_FAKE_VDEC_STICKY=<n>` (ps5wk-shot).

## View in VR (Milestone 2 step 3, 2026-10-28)

The `<video>` playing in the page goes into the PS VR2 headset; the page
keeps playing it and the title draws the same decoded frames (hardware or
software path) on a sphere, dome or big flat screen. Nothing changes in
WebKit (no patch): it is all in the title (`wpe-ps5/PS5RendererVR.cpp`,
`PS5Text.cpp`, `apps/title/viewinvr.c`).

**Pad.** On a page: **touchpad button** = View in VR (a message on the TV
says so for 4 s when a video of 640 px or more starts, at most every 2
minutes). In VR the whole pad belongs to it:

| button | in VR |
| --- | --- |
| X | play / pause (the page's video element, or YouTube's player API) |
| D-pad left / right | seek -10 s / +10 s |
| Triangle | next projection: flat, 180 SBS, 180 TB, 180 mono, 360 mono, 360 TB, 360 SBS, fisheye SBS |
| Square | recentre (the video's front goes where you look; yaw only) |
| O, Options or touchpad | back to the TV, playback continues (also while the headset is coming up) |

The headset shows a hint for a few seconds after entering and after each
press (projection, playing/paused and time, the controls); the TV shows
"Playing in the PS VR2 headset", the page title, the projection and the
controls, and always "Back to the TV: ○, Options or the touchpad". Without a
connected headset the TV shows "PS VR2 not connected..." for 8 s and nothing
else happens (no VR mode; see *No headset* below). VR also ends by itself,
with a message on the TV, when the headset is unplugged, when no head pose
arrives within 15 s, and when the system holds the pad for 10 s. Leaving a page, a WebProcess crash and the title's
wind-down (`tools/launch.sh --stop`) leave VR first; the wind-down waits for
the headset to be released.

**How it works.**
- `viewinvr.c` (UIProcess main thread) runs a script in the page: the
  playing `<video>` (largest first; page and same-origin frames), its
  address and the page title, plus YouTube's `projectionType`/`stereoLayout`
  from the player response. The projection guess: YouTube's data if any
  (`EQUIRECTANGULAR` = 360, `..._THREED_TOP_BOTTOM` = 360 TB, `RECTANGULAR` =
  flat); else keywords in title, page address, stream address and the
  element's attributes: `180`/`360` as a number on its own (not `360p`,
  `640x360`), `sbs`/`3dh`/`side by side` (and `lr` next to an angle),
  `tb`/`ou`/`3dv`/`top-bottom`/`over-under`, `mono`, `fisheye[NNN]`,
  `mkx200`/`mkx220`, `vrca220`, `rf52`; 180 without a stereo word is
  side by side (VR180 nearly always is); SBS/TB without an angle is 180
  for square eyes and 360 for 2:1 eyes. When the page says nothing (or
  names an angle but no stereo layout), the video's own pictures decide (see
  *Projection guess* below); a Triangle choice is remembered per site. With
  no `<video>` reachable (a cross-origin player frame) it still enters VR on
  the title's newest video stream, without player controls.
- The display (`wpe_display_ps5_vr_*`, `wpe_display_ps5_set_button_handler`)
  hands the request to the render thread and captures the pad.
- The render thread (`PS5RendererVR.cpp`): `psvr2_open` (Hmd2, VrTracker2,
  reprojection; Spike 3b's `spikes/common/psvr2`), three 4096x2048 eye
  buffers (two 2048 eyes side by side) as immutable GL textures exported as
  T#s, `psvr2_start` (120 Hz out, half-rate rendering). Then it is paced by
  the headset instead of the TV: per frame (60/s) it takes a pending page
  buffer (the UI thread waits for those), `psvr2_begin_frame` (pose for the
  predicted display time), `PS5VideoServer::consume` with that display time
  (converted from the process clock to CLOCK_MONOTONIC) so the newest due
  video frame is converted and older ones are dropped with their decoder
  slots released, the eye pass, `glFinish`, `psvr2_submit`, then
  `presented()` gives the shown picture's slot back: the same slot flow as
  on the TV, at the headset's frame rate. The TV is redrawn only when its
  notice changes (a TV swap is cheap when the previous flip is long done).
- Eye pass: one fragment pass per eye, 4 samples per pixel on a rotated
  grid (Spike 9: 2048 x4 removes shimmer, 1.2 ms), each sample along its
  view ray: equirectangular 360/180, equidistant fisheye (field of view from
  the name, 180 by default), or a 70-degree-wide flat screen in a dark room;
  stereo layouts pick the eye's half (left/top = left eye). The hint is
  head-locked at the same tangents in both eyes. The video's front starts
  at the tracking space's forward (-z, where the system centres its own VR
  views, as PS5VR does); Square moves it to where the head looks (yaw
  only). Not recentred on the head at entry: the tracker's first poses
  after it registers are a stale default (see *Headset test 1*).
- Leaving: `psvr2_close` leaves VR mode, unregisters, closes the tracker,
  reprojection and Hmd2 and now **releases the ~130 MB of direct memory**
  psvr2 took (Spike 3b/9 leaked it per session); a failed open undoes
  whatever was set up.
- `package.sh` writes PS5VR's `attribute`/`attribute3` (0x62000000 /
  0x04080c40) into PPSA99020's param.json: Hmd2 refuses a title without
  them (FEASIBILITY §3). Spikes 3b and 9 drew on the TV with the same values.
  Bisected on the console (2026-10-28, `vr-probe`, no headset connected):
  only attribute3 bit 0x400 is checked by `sceHmd2Initialize` (0x8a72001f
  without it); with attribute 0 / attribute3 0x400 the whole open path
  (Hmd2 open, VrTracker2 registration, reprojection) succeeds as with the full
  values; VR mode itself (`EnableVrMode`) needs a headset to check, so the
  full values stay. `VR_ATTRS=0` or `VR_ATTRS=<attribute>:<attribute3>` (hex)
  makes package.sh write other values. The attributes don't change audio:
  the sound test played on the TV with the full values, with 0/0x400 and with
  none (owner, 2026-10-28; an earlier "no sound" was the TV being off).
  `/data/ps5webkit/vr-probe` now also runs on its own (no `vr-sim`) after the
  first test URL, and with `full` in the file it opens and closes the whole
  psvr2 path (`RESULT ps5browser vr-probe-full`).
- Text (TV notice, hint, messages): FreeType with the browser's Noto fonts
  (NotoSans-Bold; the pad's ✕ ○ △ □ ◀ ▶ from NotoSansSymbols2).

**Projection guess (2026-10-30).** Many VR streams carry no hint at all
(a 3840x1920 HLS stream with a plain address: 180 side by side, shown flat
before). Order: YouTube's data, then the page's keywords, then the
pictures, then a remembered choice on top.
- *Pictures* (`apps/title/vrlayout.c`, plain C): on the touchpad the title
  asks the render thread for three 128x128 luma thumbnails of the playing
  video 200 ms apart (`wpe_display_ps5_video_thumbnails`; a small shader
  averages 8 x 8 taps per cell of the converted RGBA frame, so hardware,
  software and 10-bit frames alike, 64 KB `glReadPixels`; fewer when paused,
  none without a video), then enters VR (about 0.4 s later than before).
  Per thumbnail: the left and right halves are compared (normalised
  correlation after taking out each row's and each column's mean, best of
  shifts up to 4 cells sideways and 1 up/down for disparity), likewise the
  top and bottom halves; the share of rows whose left and right edges break
  (a 360 equirectangular picture's edges join); dark corners around a
  bright circle in each half (fisheye). The frame's shape picks which split
  matters: 2:1 and 4:1 side by side, 1:1 and 1:2 top/bottom, anything else is
  flat. Halves that match (0.6 or more) are stereo: 2:1 = 180 SBS (fisheye
  SBS 190 with circles), 4:1 = 360 SBS, 1:1 = 360 TB, 1:2 = 180 TB. Halves
  that differ (under 0.4): a 2:1 frame whose edges join (under 25% of rows
  break) is 360 mono, otherwise flat. In between, or too little detail (a
  dark fade): the shape alone (2:1 of 1280 or more = 180 SBS, 1:1 of 1440 or
  more = 360 TB, 1:2 = 180 TB), and in VR it looks again after 3 s and then
  every 5 s (at most three times) until something decides or you pick.
- *Remembered choices*: Triangle's last pick is stored when you leave VR,
  keyed by the page's origin, the frame's shape (aspect x 100, e.g. `200`)
  and what the guess was ("on this site, when the guess is flat for a 2:1
  frame, show 180 SBS"), in `/data/ps5webkit/vr-projections.txt` (one line
  each, newest last, at most 64; `vr-projections-sim.txt` with `vr-sim`).
  Cycling back to the guess forgets it. Nothing about any site is in the
  code.
- The log says what decided and why: `vr: projection: 180° 3D side by side
  (2:1 frame, halves match 0.98) [origin, 3 thumbnails, 3 detailed: halves
  0.98, top/bottom -0.09, edges break 78%, fisheye 0]`; the headset hint
  and the TV notice show the projection with a short reason ("Guessed: ...",
  "From the page", "Remembered for this site", "Your choice: ...").
- Tested on the Mac: `test/vrlayout/run.sh` (FFmpeg thumbnails of the clips
  below, both as exact cell averages and the shader's taps) 48/48: the
  `guess_*` clips (180 SBS 0.98, 360 mono with 17% edge breaks, 360 TB
  0.98, 180 TB 0.99, fisheye 0.98 with circles, flat 2:1 fractal zoom with
  80-100% edge breaks), the flat test patterns and 2:1 HLS variants (flat),
  and the `vrtest_*` grids (their grids repeat, so the 360 mono grid looks
  like SBS to the pictures: its keywords decide).
- Tested on the console (log-only, `vr-sim`, 2026-10-30): `vr-guesstest` over
  the seven `guess_*` clips, `vrtest_180_LR` (keywords) and the 2:1 HLS
  variants: 27/27 PASS (guess, remembered, forgotten for each), the same
  numbers as on the Mac: 180 SBS halves 0.98, 360 mono halves -0.09 and 17%
  edge breaks, 360 TB 0.98, 180 TB 0.99, fisheye 0.98 with circles, flat
  2:1 zoom 34-80% breaks, HLS 98-99%. Thumbnails: 1.7 ms each at 3840x1920
  (15 ms for the first, the shader compile), 0.4-0.6 ms at 2048x2048 and
  below. `vr-selftest` on `guess_a`: PASS (guessed 180 SBS; re-entries with
  the video paused took one thumbnail). An edge measure counting only rows
  with detail gave 39% breaks for the 360 clip on the console (19% on the
  Mac) and was dropped.

**Test clips.** `test/media/make-vr-media.sh [--upload]` makes HEVC test
patterns (FFmpeg `v360` from a labelled equirectangular grid: a cell every
30 degrees with its yaw/pitch, FRONT/RIGHT/BACK/LEFT/UP/DOWN, a green front
cell, a clock at the front; stereo clips have a red "LEFT EYE" and a blue
"RIGHT EYE" grid): `vrtest_360_mono`, `vrtest_360_TB`, `vrtest_360_SBS`,
`vrtest_180_mono`, `vrtest_180_LR`, `vrtest_180_TB` (2048x4096), `vrtest_MKX200_LR`
(200-degree fisheye). The start page has a "View in VR tests" row. The
same script makes the layout-guess clips (H.264, no keywords in the names;
a generated world from six FFmpeg patterns, nothing periodic): `guess_a`
180 SBS 3840x1920, `guess_b` 360 mono 3840x1920, `guess_c` 360 TB
2048x2048, `guess_d` 180 TB 1440x2880, `guess_e` fisheye 190 SBS
3840x1920, `guess_f` flat 3840x1920, `guess_g` flat 1920x1080.

**Knobs (log-only tests).** `/data/ps5webkit/vr-sim`: no Hmd2 at all; the eye
buffers are rendered on a 60 Hz timer with a scripted head direction, and a
screen capture request reads them back (`[vr] SHOT`). `/data/ps5webkit/vr-selftest`
(only with `vr-sim`): after the first test URL loads, enters VR through the
same path as the touchpad, captures the eye buffers looking front, right,
back, left, up and down, cycles every projection, presses X, right, left, X,
Square and O, then enters with the touchpad and leaves three more times
(touchpad, Options, O)
(`RESULT ps5browser vr-selftest`). `/data/ps5webkit/vr-sim-untracked` (with
`vr-sim` and `vr-selftest`): the simulated headset gives no head pose; the
self-test enters and checks that VR ends by itself within 25 s
(`RESULT ps5browser vr-selftest-untracked`). `/data/ps5webkit/vr-guesstest` (only with
`vr-sim`; lines `<part of the address> <projection number>`): after every
test URL, enters VR, checks the guess (`RESULT ps5browser vr-guess ... guess`),
presses Triangle and leaves, enters again (`... remembered`: the choice is
back), presses Triangle round to the guess and leaves (`... forgotten`); use
a dwell of 30 s per URL. `/data/ps5webkit/vr-probe`: afterwards
opens and closes Hmd2 twice without tracker or VR mode (nothing shows in a
headset): binding, param.json attributes, device status. Logs: `[vr]`
(every 5 s: headset frames, render time, tracked, video frames shown,
projection, errors).

**Tested (log-only, 2026-10-28, `vr-sim` + `vr-selftest`; no headset was
driven).** Run 1: `vrtest_180_LR` decoded by Videodec2 (3840x1920 HEVC):
guess "180 3D side by side" from the address, simulated headset up in 27 ms,
eye-buffer captures front/right/back/left/up/down and every projection
correct (upright, red left eye and blue right eye in the right halves,
black outside 180 degrees, flat screen where expected; they match a CPU
port of the eye pass run on the Mac over the same clips), X/seek/recentre
reached the page's video (`Paused 0:52`, `+10 s`, `-10 s`, `Playing`).
Run 2: `sync-1080p30-h264` with `no-hwdec` (software frames through shared
memory): guess "flat", the same sequence, then three more enter/leave
cycles: `RESULT vr-selftest PASS`. Both runs: eye pass 0.8–1.3 ms per
frame (2 x 2048 x 2048, 4 samples) when no capture is taken, 60 headset
frames/s, a 30 fps video shown at 28–30 frames/s, decoder slots flowing
(0 take-backs), TV notice redrawn 16–17 times in a minute (swap 0.1 ms),
title flexible memory unchanged (298 MB), direct memory back to its
pre-VR level after leaving (the eye buffers). `vr-probe`: Hmd2 binds with
the new param.json, opens the headset (device status 2, not worn) and
closes, twice. The real Hmd2 VR mode (`psvr2_start`, submits, tracking,
`psvr2_close` with its memory release) has not run in the browser yet: that
is the owner's headset check.

**Headset test 1 (owner, 2026-10-28) and fixes.** 360 mono, 180 SBS,
360 TB, fisheye, flat, the controls and repeated enter/leave worked in the
headset (60 frames/s, 120 Hz out, 0 submit errors; each exit released 129
MB, all rc 0). Two problems:
- **The view started facing LEFT 90.** Every entry logged "recentred (yaw
  -91.x)" while the owner faced forward: the tracker's first poses after it
  registers are a stale default, as PS5VR notes ("always yaw 90.5", its sign
  convention), so it never recentres on entry. Our yaw maths equals PS5VR's
  (a CPU check over 100,000 random poses: our yaw0 is minus PS5VR's angle,
  and after a recentre the head's forward maps to longitude 0); the bug was
  using the stale pose. Now the front starts at tracking-space forward and
  only Square recentres; `[vr] head yaw` is logged at headset frames 2, 60
  and 300.
- **180 TB (then 1920x3840) did not play:** Videodec2 refused the size and
  playback failed with FormatError. A size probe in the title
  (`/data/ps5webkit/vdec-probe`, `[vdec] probe` lines) found: HEVC at level
  5.1 opens at most 2176 lines (1920x2304, 2880x2880, 2048x4096 refused,
  0x811d0200 at QueryDecoderMemoryInfo); at level 6.1, 1920x3840, 3840x3840,
  4096x4096 and 4352x4352 open; above 4352 lines nothing does (5120x5120,
  8192x8192, 2880x5760); H.264 and VP9 open portrait sizes (1080x1920,
  2160x3840, 2304x4096 H.264; up to 4096x4096 VP9). So vdec2 now asks for
  level 6.1 above 2176 lines or 4096 columns. But a decoder opened that way
  still fails every decode call (0x811d0111) on a stream whose own SPS says
  level 5.1, as encoders write for 2048x4096 or 2880x2880 (the spec allows
  it); a 3840x3840 stream tagged 6.0 decodes. (A square decoder covering a
  portrait picture failed the same way.) Fixes in the WebProcess's
  `ps5vdecproxy`: HEVC above 2176 lines or 4096 columns with a level below 6
  is not taken (software decoding instead), and the first accept-caps query
  now has the title open the decoder (DOPEN, kept for set_format), so any
  size the title refuses falls back to software decoding instead of failing
  playback. `/data/ps5webkit/vdec-refuse` (`<w>x<h>`) makes the title refuse
  a size, to test that path. Checked log-only: `vrtest_180_TB` (now
  2048x4096, level 5.1) plays in software at 30 frames/s and the eye buffers
  show 180 TB correctly; `vrtest_360_TB` with `vdec-refuse 3840x3840` falls
  back to software and plays; self-test PASS both times. Next: rewriting
  the SPS level (general_level_idc) to 6.1 for such streams could keep them
  on the hardware decoder.

**No headset (2026-11-02, fixed 2026-11-03).** The owner pressed the
touchpad by accident with the PS VR2 unplugged, twice (tv26, tv33): View in
VR went all the way into VR mode and could not be left. Hmd2 opens without
a headset (`sceHmd2Open` gives a handle, `status 2 worn 0 lens 0 mm`), and so
do the tracker, the reprojection, `EnableVrMode`, `RegisterBuffers2` and 120 Hz
output: the title then submitted 60 frames/s for 466 s with 0 tracked poses.
No pad press reached View in VR in either session (nothing was logged, and an
O or touchpad press would have logged `vr: back to the TV (pad)`; the
controls worked in *Headset test 1*), so the system presumably had the pad
(the TV may have shown a system notice instead of ours). The PS5's own close
did not end the process either; only the launcher's `quit` (wind-down) did.
The title handles no system events: its only clean exits are the launcher's
`quit`, *Restart browser* and a SIGKILL; a system close is ShellUI's
suspend-then-kill (FEASIBILITY R12), which apparently did not complete while
the title held VR mode (not reproduced). Fixes:
- *Connected* is device status 0 (every headset session: `status 0 worn 1
  lens 63-68 mm`, once `status 0 worn 0 lens 66 mm` off the head); unplugged
  is status 2, lens 0 (every `vr-probe` and both incidents). `psvr2_open`
  with `psvr2_require_headset(1)` (the title) stops right after
  `sceHmd2Open` when the status is not 0 (or the read fails), closes Hmd2 and
  returns `PSVR2_NO_HEADSET`: no tracker, reprojection or VR mode. The TV
  says "PS VR2 not connected. Connect the headset, turn it on and press the
  touchpad again." and the log `[vr] PS VR2 not connected (device status 2,
  ...): staying on the TV`. The spikes and `vr-probe` keep the old behaviour.
- In VR the render thread reads the device status every second (`[vr]
  headset device status 0, worn (lens 66 mm) after 1.0 s`, and on every
  change); status not 0 twice in a row ends VR ("not connected any more").
  No tracked pose at all within 15 s of entering ends VR too (a headset
  that never shows up; 15 s leaves time to put it on after pressing the
  touchpad).
- The pad: every press in VR is logged (`vr: pad 0x2000 in VR`), and the
  display logs when the pad read starts or stops failing, the system's
  *intercepted* bit (0x80000000: its UI has the pad, the buttons read 0)
  and the connected flag (`pad: read 0x0, intercepted by the system, ...;
  View in VR has the pad`). While View in VR has the pad, the system holding
  it (read failing or intercepted) for 10 s with no press getting through
  ends VR ("The system had the controller for 10 s"), so a system menu or
  close is never stuck behind VR mode. O, Options and the touchpad all leave
  (Options used to show the hint again), also while the headset comes up;
  the TV notice says so on its last line.
- A stop asked while the headset was coming up could leave the title
  thinking it was in VR (the start finished after the stop was queued):
  the start's answer now carries the entry's number and a stale one stops
  the headset again.
- **`unregister 0x80290009`** is VideoOut's RESOURCE_BUSY: the buffer set on
  screen cannot be unregistered (PS5VR's notes say the same of the TV). It
  shows on every stop after frames were flipped, including all the
  sessions of *Headset test 1*, which re-entered cleanly many times; the
  `sceVideoOutClose` right after returns 0 and releases the set, then the
  tracker, reprojection and Hmd2 close with 0 and the 129 MB go back. The
  order (DisableVrMode, unregister with 0.5 s of retries, VideoOut close,
  tracker unregister/finalize, reprojection terminate, Hmd2 close/terminate,
  direct memory, then the GL eye buffers) is PS5VR's. Nothing in it points
  at the console dropping off the network after the relaunch: the wind-down
  released everything (all rc 0, `released in 878 ms`), main returned and the
  launcher killed the title; the new process died while starting its network
  process, before any VR call. Cause unknown; 466 s of VR mode without a
  headset beforehand is the only unusual thing.

Not done: YouTube's own 360/180 formats. Its player data says
`projectionType` per format (read and logged; `EQUIRECTANGULAR` and
`EQUIRECTANGULAR_THREED_TOP_BOTTOM` map to 360 / 360 TB), but `MESH`
(equi-angular cubemap, and VR180's mesh) is not unpacked: it is shown as
360 equirectangular, which will look wrong. Whether YouTube serves 360
video at all to this browser (Safari user agent, no WebGL) is not checked.
the site's JSON feed (exact projection) is not used yet.

## JIT (patch 0030, 2026-10-28)

JavaScriptCore's JIT runs in the WebProcess payload: Baseline, DFG, FTL
and the regexp JIT (YARR). The executable memory is plain RWX anonymous memory,
upstream's Linux/FreeBSD x86_64 model (`ExecutableAllocator`'s fixed pool,
no W^X): payloads may execute it (Spike 1), and the dual-mapping
`sceKernelJit*` route isn't needed. **The JIT is opt-in on PS5**
(`useJIT=false` in JSC's defaults). A title can map RWX memory but faults
when it executes it (Spike 1 title run: `jit-rwx ... executing faulted`).
So the UIProcess title, ps5wk-shot and the NetworkProcess stay on the LLInt
without any change of their own. `ps5wk-child` turns the JIT on for
WebProcesses (`PS5WK_JIT`, see *Knobs*; the log says `JIT: on (...)`).

- **Pool:** 256 MB reserved per WebProcess since 2026-11-01 (ps5wk-child;
  patch 0030's own default stays 128 MB; upstream x86_64: 1 GB). Each 2 MB
  reserved costs one CPU page table whether touched or not, so 256 MB is
  +64 against 128 MB. `/data/ps5webkit/no-bigjitpool` keeps 128 MB,
  `JSC_jitMemoryReservationSize=<bytes>` in child.env sets any size; the
  `JIT:` log line names the pool and where it came from. See *JIT pool and
  HSTS writes*.
- **WebAssembly** is compiled in, because B3 (the FTL backend) doesn't build
  without it upstream (`B3WasmStruct*`), and `ENABLE_WEBASSEMBLY_BBQJIT` alone
  didn't compile either. It is off at run time (`useWasm=false`, no fast
  memory: 4 GB+ reservations); `JSC_useWasm=true` turns it on, untested.
- **Signal contexts:** everything in JSC that reads registers from a signal
  goes through WTF's `registersFromUContext` (VM traps, thread suspension for
  GC and the sampling profiler, the wasm fault handler). 0019 already moved it
  to the PS5 kernel's layout, and nothing reads `uc_mcontext` directly. VM traps in
  JIT code were checked on the console with both signal-based traps (the
  default) and polling traps. With the JIT on, JSC installs SIGSEGV/SIGBUS
  handlers (signal-based VM traps). 0030 runs them on the crash guard's alternate
  stack (SA_ONSTACK), and faults that aren't JSC's go to the crash guard as
  before. Its report names a pc in JIT code as `(JIT code +0x...)`.
- **Build:** `build.sh` configures `JIT=1` by default (`JIT=0`: the LLInt-only
  build as before). The executables grow by ~15 MB stripped (ps5wk-child 114 ->
  129 MB, elfldr loads it in ~0.2 s as before; ps5browser 114 -> 129 MB, which
  costs the title ~15 MB of flexible memory, since the image counts 1:1).

Checked on the console (13.60, `spikes/04-jsc/jit-check.sh`: the WPE build's
`jsc` as a payload, and `test/jit-ab.sh`: ps5wk-shot A/B):

| | LLInt (`--useJIT=0`) | Baseline | DFG | FTL (default) |
| --- | --- | --- | --- | --- |
| Spike 4 bench, jsc payload: total | 3062 ms | 1505 ms | 511 ms | **440 ms** (7.0x) |
| richards / deltablue / numeric / strings / alloc | 583 / 521 / 800 / 393 / 766 | 358 / 181 / 461 / 192 / 313 | 79 / 53 / 95 / 154 / 131 | 65 / 41 / 56 / 154 / 124 |
| same bench as a page in the WebProcess (`test/jit-bench.html`) | 3014 ms | | | **398 ms** (7.6x) |

- Correctness: `correctness.js` 30/30 with and without the JIT;
  `jit-stress.js` (tier-up to DFG/FTL, OSR exits, exceptions through JIT
  frames, GC from inside hot loops, watchpoint invalidation, regexp JIT,
  stack overflow in JIT code) 16/16 in four modes (all tiers, no FTL,
  eager tier-up without concurrent JIT, LLInt), and the checksums are the same
  in every mode and on macOS's system jsc. `jit-watchdog.js`: an endless
  DFG/FTL loop is terminated by the watchdog with signal and with polling traps.
- Pages (ps5wk-shot, 2-3 runs per mode, `jit-ab.sh`): YouTube watch page,
  BBC News, Reddit and Wikipedia load and paint with the JIT, with no crash and 0 failed
  mmaps. A 12-load stress cycle over six pages in one WebProcess had no
  termination (one Reddit `js_challenge` redirect is cancelled with and
  without the JIT). Load events are mostly network-bound here: YouTube's `load`
  event came 2.5–3.4 s after the start with the JIT against 3.1–6.5 s without.
  The busy main thread (~1 core) after it is shorter: two 5 s windows of
  1.11/0.74 cores against 1.00/1.00. BBC, Reddit and Wikipedia are within run-to-run
  noise. Video in the harness (fake title decoder) dropped 2–3 frames during
  load either way.
- Memory: YouTube makes ~40 MB of JIT code (of 128 MB; code GC brings it
  back down: 43 -> 20 MB in the stress cycle). The WebProcess heap is larger
  with the JIT: libpas pages on YouTube at 20 s were 589 MB (LLInt),
  770 MB (Baseline only), 780 MB (up to DFG) and 885 MB (all tiers; 772 MB with
  one compiler thread per tier, `JSC_numberOf{Baseline,DFG,FTL}CompilerThreads=1`,
  which ps5wk-child now sets by default; child.env lines override it).
  After the 12-load cycle they were 625 MB against 1104 MB. The payload has 8 GB of
  flexible memory, but on PS5 libpas never returns pages (0006), and
  touched memory costs page tables: the kernel's `page table CPU` count with
  ps5wk-shot + NetworkProcess + WebProcess peaked at ~6575/8192 with the JIT
  against ~6290 without (idle 5023).
- **Title PPSA99020 with the JIT build** (log-only runs, 2026-10-28 evening):
  the WebProcess logs `JIT: on`, the NetworkProcess `JIT off`, and the
  title maps no RWX memory (flexible memory 284 MB at start, ~15 MB less
  than before, as the bigger image predicts). Start page, YouTube (4K60 VP9 in
  Videodec2, 60 s), Wikipedia and BBC News load, with no crash. Page tables
  peaked at 6965/8192 (title + NetworkProcess + two WebProcesses). YouTube's
  first ~20 s on the real decoder: 12 and 8 frames dropped with the JIT
  (two runs) against 22 without; late access units 118/61 against 79.
  Rendering updates take 45–90 ms each at ~6/s during load either way.
- **Where the WebProcess main thread goes** (`PS5WK_PROFILE=1`; the profiler
  now writes JIT pcs as `jit`, and `test/prof.sh` adds a by-area summary):
  while YouTube loads, the main thread is 65–91% busy for ~15 s. Roughly
  40–65% of that is JavaScript (JIT code plus the JSC runtime, allocator
  included), 10–30% paint and ~15% style and layout. Once playing without
  interaction it is 3–11% busy. So the JIT shortens the load phase, but a page
  that keeps repainting (overlays, controls) is limited by CPU painting
  (`[render]` ~35 ms per update in the owner's session), which the JIT
  doesn't touch.

## WebProcess memory (patch 0031 and the memory limit, 2026-10-28)

Before 0031, libpas memory in the WebProcess was a high-water mark: 0006
made decommit only zero the memory (pages are 16 KiB and libpas decommitted
4 KiB granules), and one WebProcess serves every navigation (0018). With the
JIT, the owner's 2-minute session reached 1.1 GB of libpas pages.

**What the PS5 kernel does** (`test/memprobe/memprobe.c`, a payload: 64 MB
anonymous mapping, every page touched, the middle 32 MB given back, residency
by `mincore`): `madvise(MADV_FREE)`, `madvise(MADV_DONTNEED)` and
`mprotect(PROT_NONE)` return 0 but leave all 32 MB resident with the contents
kept; `mmap(MAP_FIXED)` of fresh anonymous memory over the range drops them
(0 MB resident, reads back zero, touchable again); `munmap` works too. Page
size 16384. `sceKernelAvailableFlexibleMemorySize` stays at 8158 MB whatever
a payload maps or touches, so it measures nothing for payloads.

**0031:** 16 KiB granules on PS5 (as libpas does on arm64 and Sony's
PlayStation port), decommit maps fresh anonymous pages over the whole pages
of the range (partial-page edges zeroed), commit is a no-op. Executable
memory is only zeroed. `PS5WK_PAS_DECOMMIT=0` (child.env) restores the
zero-only decommit for A/B tests. The `[mem]` line now says
`libpas pages X MB (Y given back, Z committed)`; it also prints
`resident in big mappings` (mincore over every mapping of 1 MB or more), but
that number drops to ~30-50 MB on light pages in both modes: the console
swaps payload memory out (`VM Stats ... swap out` in the kernel log grew by
320-480 MB during each run below), so it is not a measure of what the
process holds. Committed (mapped minus given back) is.

**Long-session test** (ps5wk-shot stress, `test/stress/long-session.txt`:
YouTube 4K watch page, BBC News, old.reddit.com, Wikipedia, GitHub, Hacker
News; 8 rounds = 48 loads, 10 s dwell, JIT on, `[mem]` every 10 s;
`childenv=PS5WK_MEM_LOG=10;PS5WK_MEM_RESTART_MB=0`, plus
`PS5WK_PAS_DECOMMIT=0` for "before"; both runs with 16 KiB granules):

| load | before: libpas committed (= pages) | after: libpas pages | after: given back | after: committed |
|---|---|---|---|---|
| 2-3 | 861 MB | 802 MB | 115 MB | 686 MB |
| 9 | 989 | 1029 | 210 | 818 |
| 16 | 1117 | 1093 | 358 | 735 |
| 23 | 1184 | 1093 | 377 | 716 |
| 29-48 | 1184 (flat) | 1125 (flat) | 237-419 | 670-760 on light pages, 830-890 on YouTube; 817 at the end |
| peak RSS (getrusage) | 1036 MB | | | 925 MB |

Both: 48 loads, 0 failed, 0 WebProcess terminations, no crash; page tables
peaked at ~7030/8192 in both (about 400 of that is a second WebProcess that
WebKit starts at each old.reddit.com load and that exits after ~20 s; seen
in both modes, not from this change). So the heap levels off at ~1.1-1.2 GB
of address space in this mix either way, and with 0031 300-420 MB of it is
back with the kernel at any time: committed memory now falls when you leave
a heavy page, instead of staying at its peak.

**Memory limit (safety net).** `PS5WK_MEM_RESTART_MB` in child.env (default
3072, 0 = off): ps5wk-child's memory thread compares libpas committed memory
with it every 5 s and writes `/system_tmp/ps5wk/webmem-<UIProcess pid>`
(`apps/common/webmem.h`). Over the limit, the title ends the WebProcess at
the next top-level navigation (load started: stop, terminate, load the same
address in a new WebProcess; the crash handler and the test-URL bookkeeping
ignore it) and logs `[mem] WebProcess ... restarting it for <url>` and
`RESULT ps5browser mem-restart`; ps5wk-shot's stress mode does the same
before its next load (`memory-limit restarts` in its RESULT line). Still one
WebProcess at a time (0018): the old one ends before the new one starts, so
the page-table cost doesn't grow, except that WebKit also starts a spare
WebProcess when the old one is terminated, which exits unused after ~10 s
(~300 page tables meanwhile). Checked in the title (log-only,
`PS5WK_MEM_RESTART_MB=400`): YouTube passed 567 MB, the navigation to
Wikipedia restarted the WebProcess, Wikipedia loaded in 2.1-2.3 s and Hacker
News after it in the new process, twice. A navigation that was a POST is
reloaded as a GET (load-changed gives no method); at 3 GB this should be
rare: the 48-load mix stayed under 0.9 GB committed.

## CPU painting (patch 0032, 2026-10-29)

Pages are painted on the CPU in the WebProcess (no GPU in payloads,
FEASIBILITY §4.8), non-composited: each rendering update paints the damaged
rects of the whole view with Skia into a shared-memory buffer, and the title
copies the damaged rows into its GL buffer (1 ms for a full page). The time
went into painting, on the main thread, one thread only.

**What changed** (WebKit patch 0032, `ps5wk-child.cpp`):

- **Painting on several threads.** The frame is recorded into an SkPicture
  (main thread, with an R-tree), then replayed in horizontal bands of whole
  rows on `PS5WK_PAINT_THREADS` (default 4) threads, the main thread
  included. Small damage (under 64k pixels or 64 rows) is painted directly.
  Recording plus one-thread replay costs about what direct painting costs
  (Wikipedia 4.3 + 4.2 ms against 13.8 ms direct), so nothing is lost.
- **The console's libc malloc and pthread mutexes collapse under
  contention** (`test/lockbench`, a payload): a malloc/free pair takes 0.3 us
  on one thread and 48/106/162 us per pair on 2/4/6 threads (3.3 -> 0.04
  million pairs/s in total); a pthread mutex lock/unlock 17 ns on one thread
  and 4-21 us on 2-6; `sem_wait`/`sem_post` stay at ~1 us and plain ALU work
  scales. With Skia on libc malloc, four threads replayed GitHub in 49 ms
  against 4.4 ms on one. Now Skia's heap (`sk_malloc`, patch 0032) and every
  C++ `new`/`delete` in ps5wk-child go to WebKit's libpas (per-thread
  caches): GitHub replays in 1.4 ms on four threads. glib, GStreamer and
  libsoup still use libc malloc and `GMutex` (pthread): contention between
  the main thread and those threads probably costs too (not changed yet).
  The title's heap (Spike 10's dlmalloc) also locks with a pthread mutex.
- **Scrolling moves pixels.** A scroll (`DrawingArea::scroll`, called by
  WebKit when the frame can blit) shifts the last frame's pixels and paints
  only the uncovered strip plus what changed; damage from earlier in the
  same frame moves with the content. Every frame starts from a copy of the
  last one (the stale parts of the buffer, ~1 ms), so a buffer of the swap
  chain no longer repaints what changed in the frames it missed. Pages with
  fixed or sticky elements over the content (BBC News's banner, Wikipedia's
  sticky columns) still damage most of the view on each scroll step.
- **Smaller fixes:** the damage was painted rect by rect from `Damage`'s
  iterator, whose rects overlap (each overlap painted again; now
  `rectsForPainting()`); after each frame the SHM surface was read back onto
  itself (`readPixels` of the whole 8 MB frame into the same memory); the
  0016 throttle timer dropped an update when it fired while a frame was
  pending.

**Measured** (`test/paint/paint-ab.sh`: ps5wk-shot at 1920x1080, the page
scrolls down and up for 10 s, 40 px per 16 ms timer tick; per rendering
update during the scroll; "before" = `PS5WK_PAINT_THREADS=1
PS5WK_SHM_READBACK=1 PS5WK_SCROLL_COPY=0` on the libc-malloc build, or one
thread where noted):

| page | before: update / paint ms (avg) | updates/s | after (defaults): update / paint ms | updates/s |
| --- | --- | --- | --- | --- |
| local test page (`test/paint/static.html?plain`: text, shadows, gradients, rotated cards, SVG) | 49.9 / 49.7 | 10 | 3.2 / 3.1 (0.05 Mpx painted per update) | 85 |
| same, scroll copy off (`t4ns`) | | | 20.5 / 20.3 (2 Mpx) | 23 |
| BBC News | 37.5 / 32.8 | 22.6 | 18.8 / 13.7 (record 5.9, replay 6.7) | 40.8 |
| Wikipedia (one thread on the same build) | 14.3 / 13.4 | 35 | 4.3 / 2.8 | 95 |
| GitHub (one thread, before libpas) | 11.7 / 9.6 | 43 | 8.0 / 5.7 | 55 (`PS5WK_PAINT_ASYNC=1`: 61, main thread 6.5 ms per update) |
| Hacker News | ~1 / ~0.6 | ~37 | ~1.6 / ~1.3 | ~34 (only the scrollbar and the strip change) |

Single runs vary with the pages' content (Wikipedia and YouTube serve
different banners from run to run); YouTube's home page doesn't scroll in
the harness (the consent dialog), and old.reddit.com redirects to a login
page. With libc malloc and 4 threads text-heavy pages got slower (GitHub
paint 54.7 ms per update, Wikipedia 14.9); image-heavy BBC already doubled.

Pixels: the local test page's last frame after the scroll (scrolled back to
0) is identical with 4 threads and with async replay; against one-thread
direct painting about 1600 of 2 million pixels differ by antialiasing levels
(max 56 of 255, at rotated cards' edges and the scrollbar), not visible in
side-by-side crops.

**Title** (log-only PPSA99020 run, defaults, 2026-10-29 00:26): YouTube 4K60
VP9 in Videodec2 with the hole punch at 47-60 frames/s shown, BBC News,
Wikipedia, GitHub, Hacker News and YouTube's home page load, no crash.
BBC News keeps repainting its whole view while idle: 26-30 updates/s at
16-19 ms each (2.07 Mpx in bands; the owner's session before showed 35-70 ms
per update at 6-14/s on busy pages). Flexible memory stayed at 283-284 MB.

**Not done:** frames aren't pipelined (with `PS5WK_PAINT_ASYNC=1` the next
update still waits for the replay); glib/GStreamer malloc and pthread
mutexes; composited scrolling of fixed/sticky content.

## Locks, malloc and networking (2026-10-30)

`test/lockbench` showed the console's libc malloc and pthread mutexes
collapsing as soon as two threads meet (FEASIBILITY §9: a malloc/free pair
0.3 us alone, 48-162 us with 2-6 threads; a contended mutex 4-20 us per
hand-over; semaphores fine). 0032 moved Skia and C++ `new` in the child to
libpas. This round moves the rest, in `apps/common/`:

- **ps5locks.c** (ps5wk-child and the title, `--wrap` of every
  `pthread_mutex_*`, `pthread_cond_*`, `pthread_rwlock_*` and the mutex and
  condition attribute calls; `PS5_LOCK_FUNCTIONS` in apps/CMakeLists.txt):
  futex locks on `_umtx_op(UMTX_OP_WAIT_UINT_PRIVATE/WAKE_PRIVATE)`, the
  call libpas's own lock already uses on the console. The unlocking thread
  may take the lock straight back (no hand-over convoy); a waiter spins 100
  times, then sleeps. The 8 bytes of `pthread_mutex_t` (a pointer on this
  libc, NULL or 1 for the static initializers) hold the futex word itself;
  recursive mutexes point to a small malloc'd block (owner and count).
  Condition variables are a sequence futex with a waiter count and the
  attribute's clock (glib's GCond waits on CLOCK_MONOTONIC); rwlocks prefer
  readers (a reader may read again while a writer waits). Errorcheck
  mutexes act as normal ones (this libc's default type *is* errorcheck, so
  they can't be told apart). This covers GMutex/GCond/GRecMutex/GRWLock,
  libc++ `std::mutex`/`condition_variable`, WTF's `Mutex`/`ThreadCondition`
  and so ParkingLot's park path, libpas's scavenger, and in the title Spike
  10's dlmalloc arenas (8 arenas, each behind a pthread mutex) and the
  render, decoder and audio threads. `check-lock-imports.sh` runs after each
  link (and in package.sh) and fails the build if the executable imports a
  mutex/cond/rwlock function that is not wrapped.
- **ps5malloc.c** (ps5wk-child only): `malloc`, `calloc`, `realloc`, `free`,
  `posix_memalign`, `aligned_alloc`, `memalign` and `malloc_usable_size` on
  libpas (WTF's exported `tryFastAlignedMalloc`/`fastFree` and
  `bmalloc::api::tryReallocOutOfLine`: fastMalloc's heap, so `new`/`free`
  mixes work too). glib, GStreamer, libsoup, FreeType, HarfBuzz, ICU,
  fontconfig, libxml2, SQLite and OpenSSL now allocate with per-thread
  caches. **Ownership:** a block is libpas's exactly when it lies in an
  anonymous mapping made through the child's `__wrap_mmap` (libpas gets all
  its memory there; the console's malloc lives in another module and never
  does): a two-level bitmap of 16 KiB pages, set by `__wrap_mmap`, cleared by
  `__wrap_munmap` before the real unmap. `free`/`realloc` of anything else
  (`getcwd(NULL)` and `realpath(NULL)` still come from the console's libc)
  go to the console's libc. None of the libc functions the
  child imports frees or reallocates a block the caller made (no `getline`,
  `putenv`, `open_memstream`). **Emulated TLS:** `__thread` is emulated
  (compiler-rt's `__emutls_get_address`, which mallocs a thread's block on
  first use), and libpas keeps its thread cache in a `__thread`: the child
  now defines `__emutls_get_address` (compiler-rt's logic) on the console's
  malloc, otherwise the first allocation on each thread recursed.
- **Off switches** (decided at the first call, before main, for the life of
  the process): `/data/ps5webkit/no-fastlock` (children),
  `/data/ps5webkit/no-fastlock-title` (the title), `/data/ps5webkit/no-pasmalloc`
  (children's malloc). The child logs `locks: futex ...; malloc: libpas ...`
  at start, and the `[mem]` line ends with `locks futex: N mutex parks, N cond
  waits, N rwlock parks; malloc libpas: N foreign frees, N foreign reallocs, N
  fallbacks`.
- **Memory accounting changes:** libpas's committed memory (the `[mem]`
  line and the `PS5WK_MEM_RESTART_MB` limit) now includes what glib and
  GStreamer allocate, which the console's heap held before (uncounted).
- **Checks on the Mac:** `test/lockbench/host-test.sh` builds ps5locks.c and
  ps5malloc.c for macOS (`__ulock_wait/__ulock_wake` stand in for
  `_umtx_op`, libpas is stubbed) and runs mutex, timed lock, recursive,
  condition variable (signal, broadcast, recursive mutex), timed wait on both
  clocks, rwlock and bitmap/emulated-TLS tests; it passes under
  ThreadSanitizer and UBSan. The console's numbers come from
  `ps5wk-lockbench` (`NINJA_TARGETS=ps5wk-lockbench`; libc against futex
  locks and libpas, 1-6 threads, condition ping-pong and a 4+4 queue).

**Measured on the console** (2026-10-30, log-only):

| `ps5wk-lockbench`, ns per operation per thread (1 / 2 / 4 / 6 threads) | console's libc | ps5locks / ps5malloc |
| --- | --- | --- |
| malloc/free pair | 279 / 46 676 / 106 535 / 162 113 | 46 / 45 / 53 / 90 |
| mutex lock/unlock (one shared mutex) | 21 / 4 251 / 13 646 / 21 012 | 19 / 89 / 364 / 588 (spin 100; spin 0: 130 / 187 / 277) |
| rwlock, 1 write in 64 | 18 / 135 / 757 / 1 322 | 17 / 84 / 184 / 314 |
| condition ping-pong, round trip | 8.5 us | 3.7 us |
| 4 producers + 4 consumers queue | 0.13 M items/s | 2.85 M items/s |

Total malloc throughput with 6 threads: 0.04 -> 67 M pairs/s. In a tight
4-6 thread loop spin 0 beats spin 100 (21 vs 10 M locks/s); at 2 threads spin
100 wins (22.5 vs 15.4); the default stays 100. `_umtx_op` wait/wake works
in payloads and in the title (imports 0 of 444 NULL).

End to end the browser did not get measurably faster: the page painting
was already on libpas (0032), and in these runs the threads seldom met.
- `test/paint/paint-ab.sh`, t4, one round, off switches on vs off: scroll
  updates/s Wikipedia 81.6 -> 79.4, local page 86.0 -> 85.6, GitHub 54.5 ->
  57.8, Hacker News 37.8 -> 37.0, BBC News 48.2 -> 40.8 (BBC's page changes
  between runs); during the load, rendering updates were shorter on Wikipedia
  (avg 12.1 -> 6.7 ms, p95 158 -> 3.4) and GitHub (44 -> 28 ms), single runs.
- Title, YouTube 4K60 VP9 (hardware decoding, hole punch) for ~62 s, then
  the page detaches its MediaSource by itself in both modes: 3643 frames, 16
  dropped (all off) vs 3715 frames, 12 dropped (new); WebProcess 0.5-1.2
  cores and title 0.7-1.1 cores in both.
- Long session (48 loads of `test/stress/long-session.txt`, new child): 0
  failures, 0 crashes, 0 fallbacks; peak RSS 923 MB (925 MB after 0031);
  libpas committed 790-1035 MB (670-890 before: it now holds what glib and
  GStreamer allocate). NetworkProcess: libpas 266-288 MB of pages, peak RSS
  208-218 MB, as before.

**Networking** (from the code, and `test/net/net-ab.sh`, which loads each
site cold and warm through ps5wk-shot and prints DNS/connect/TLS/TTFB from
Resource Timing, `net-summary.py`):

- The NetworkProcess is its own ps5wk-child payload (one per UIProcess);
  libsoup runs on its main thread, `getaddrinfo` on glib's resolver pool
  (up to 20 threads), TLS handshakes on the GTask pool (10 threads, then slow
  growth).
- libsoup: one session, 256 connections, 6 per host, HTTP/2 with ALPN (so
  most hosts use one connection), idle timeout 115 s, TCP_NODELAY.
- DNS: WebKit's `WebKitCachedResolver` keeps answers 60 s (400 per family
  map). DNS prefetching filled only the Default map, which libsoup's typed
  lookups never read: fixed in 0034. GNetworkAddress's happy eyeballs waits up
  to 50 ms for the IPv6 answer once the IPv4 one is in; a prefetched host now
  gets "no IPv6" at once from the cache.
- TLS: glib-networking's OpenSSL backend, CA bundle
  `/data/ps5webkit/etc/ssl/cert.pem` loaded once per process, session
  resumption on (50 sessions, 10 min, in memory per process); each new
  connection builds its own `SSL_CTX` (OpenSSL 3, not changed).
- Disk cache `/data/ps5webkit/browser/cache/WebKitCache/Version 18`; before
  0034 every body over 16 KiB was written twice and the cache had no working
  size limit. Memory cache 96 MB (DocumentBrowser). Preconnect
  (`<link rel=preconnect>`) and speculative revalidation are on.
- 0034 checked on the console: with the old child, every cache directory
  had `Blobs/` copies of the bodies (34 KB to 12.9 MB per site, beside the
  records' copies); with the new one `Blobs/` stays empty, and the warm loads
  were served from the records' blobs (Wikipedia 30 of 31 resources, GitHub
  173 of 174, BBC 105 of 106). One round of cold/warm loads, old child vs new
  (ms to the load event): Wikipedia 891/874 vs 1077/864, GitHub 2287/2875 vs
  2247/1584, Hacker News 871/343 vs 554/281, BBC 1379/1290 vs 1509/820,
  YouTube 3656/2681 vs 2923/2463. Single runs over the internet; the
  cold-load spread is network noise. New TLS connections took 20-50 ms,
  connect 35-80 ms (YouTube's video hosts ~245 ms), DNS 20-70 ms per new host;
  the old child's cold YouTube load once spent 2 x 570 ms in DNS.
- Not changed, worth measuring first: 8 KB reads per main-loop callback
  (`gDefaultReadBufferSize`), per-connection `SSL_CTX_new`, the WebProcess's
  own libsoup for hlsdemux2 (no shared connections or TLS sessions with the
  NetworkProcess).

## Profiling: where the time goes (2026-10-31)

The locks/malloc round made those 30-2000x faster in microbenchmarks and
browsing no faster, so this round measured before changing anything.

**Tooling** (all off unless `PS5WK_PROFILE` is set; checked on the console):

- **`apps/common/profiler.c`** (ps5wk-child, both processes). `PS5WK_PROFILE=1`:
  a helper thread sends SIGPROF (`pthread_kill`) to every thread every 2 ms;
  the handler reads the thread's *own* CPU clock and records a sample
  weighted by the CPU it used since its last one, so the profile is CPU time
  per thread, and threads that stayed idle are signalled less and less
  (down to ~1/s). On 13.60 `pthread_getcpuclockid()` + `clock_gettime()` of
  another thread returns the *calling* thread's time, hence the reading
  inside the handler (`CLOCK_THREAD_CPUTIME_ID` of the calling thread is
  right). `PS5WK_PROFILE=wall`: the main thread every 2 ms, idle or not
  (what it waits on; slows the page down noticeably). `PS5WK_PROFILE=threads`
  (2026-11-01): every thread every 20 ms by wall time, idle or not (`PS5WK_PROFILE_US`
  changes the tick): where each thread waits, for hangs between threads (it
  found the resume-from-suspend freeze below). `PS5WK_PROFILE=itimer`
  (`setitimer(ITIMER_PROF)`) exists but has never run on the console.
- **Unwinding** (`apps/common/ps5unwind.c`): the executable's own
  `.eh_frame_hdr`/`.eh_frame` (DWARF CFI) in the signal handler, every stack
  read bounds-checked against the thread's stack (the crash guard's
  `pthread_create` trampoline records it); rbp chains for JSC's LLInt and JIT
  frames and through libkernel/libc. Up to 128 frames. `test/prof/unwind-test.sh`
  checks the CFI rules against `llvm-dwarfdump --eh-frame` on the Mac
  (27k rows, all equal).
- **Output**: `/data/ps5webkit/prof/<tag>-<pid>.prof`, stacks folded per
  1 s window, per-thread CPU and names (WTF threads are now named on PS5,
  patch 0035), JIT pcs tagged with the tier that compiled them (patch 0035
  notes every executable allocation with the compiling thread's tier), and
  the JIT counters below.
- **`test/prof/prof-run.sh <url>...`**: ps5wk-shot at 1920x1080 cold/warm with
  the profiler, `page.js` (TTFB, DOMContentLoaded, load, FCP, resources),
  `DIAG=1` (`page-diag.js`: html classes, Apple's `causeForBase`, videos,
  images, `pushState`/`beforeunload`), `SCROLL=1` (paint-ab's scroll),
  `PROFILE=wall`, `YTCONSENT=1` (YouTube's "reject all" cookie, then a reload).
  It waits until elfldr is idle between runs. **`test/prof/prof.py`**:
  symbolizes (llvm-symbolizer), CPU per thread, the main thread by *area*
  (innermost frame that names one) and by *phase* (outermost), JavaScript by
  tier, a per-second timeline, `--folded` for flame graphs (speedscope).
  `test/prof.sh` summarizes the newest profiles on the console (title runs).
- **Patch 0035**: JIT counters (`ps5wk_jit_counts`: compiles by tier and
  outcome, compile ms, refused executable allocations, the executable-memory
  pressure multiplier); in the `[mem]` line too.
- **Fixed on the way**: elfldr sometimes refuses loopback connections for a
  moment (ECONNREFUSED; two of four back-to-back harness runs failed to start
  their children): `WPEProcessManagerPS5` now retries for up to 10 s (the
  title too, once rebuilt). The child's `[cpu]`/`[mem]` loggers resume their
  sleep after a signal.

**Measured** (ps5wk-shot, single cold runs over the internet, profiler on;
main thread = WebKit's main thread in the WebProcess, CPU over the ~15-25 s
of the run):

| page | load event / FCP (ms) | main thread CPU | where (main thread) | other threads |
| --- | --- | --- | --- | --- |
| YouTube search results | 2484 / 2092 | 14.1 s (78% busy) | JS 56% (C++ runtime 24, JIT code 19, parse 7, LLInt 4), style 13, layout 6, paint 6 | JIT compilers 3.0 s; pressure multiplier 1.91 |
| YouTube home (no account: "try searching"), warm | 3353 / 2460 | 8.3 s | JS ~55%, style 12% | JIT compilers 1.9 s; multiplier 1.82 |
| The Verge /tech | 8905 / 3185 | 9.5 s | JS ~60% (runtime 29, JIT code 13, parse 11), style 8, layout 6 | JIT 3.1 s, a Worker 0.6 s; NetworkProcess main 1.8 s (26% HSTS writes) |
| BBC News (load + scroll) | 1759 / 581 | 14.4 s (79% busy) | layout 19, paint raster 16 (bilinear image scaling alone 7%), JS 26, style 9 | 3 paint threads 1.8 s each |
| GitHub (load + scroll) | 2272 / 1071 | 8.2 s | paint raster 22 (clearing 6%), layout 11, JS 17, style 9 | 3 paint threads 0.8 s each |
| Apple iPhone page (load + scroll) | 3118 / 522 | 9.0 s | scroll: paint (record+raster) 40%, copying the last frame forward 17%, layout 20 | 3 paint threads 0.7 s each |
| Wikipedia | 727 / 582 | 2.0 s | layout 20, style 16, JS 13 | - |
| Hacker News | 497 / 368 | 0.7 s | mostly idle | - |

- **JavaScript** is the biggest single cost on script-heavy pages (YouTube,
  The Verge: 55-60% of a main thread that is 40-80% busy for 15-25 s). Of
  that, JIT-compiled code is only 10-19 points; the rest is the C++ runtime
  (property lookups, `JSLexicalEnvironment::getOwnPropertySlot`, hash
  tables, strings), the parser/bytecode generator (7-12%) and the LLInt
  (4-5%). The tiers are used and never fail: YouTube compiled 3573 Baseline,
  2279 DFG, 77 FTL code blocks, 0 failed, 0 refused allocations. But
  executable memory pressure raises every tier-up threshold: JSC multiplies
  them by `available / (available - used)`, and with our 128 MB pool
  YouTube reaches **x1.8-1.9** (upstream's 1 GB: ~x1.05).
- **CPU painting** dominates scrolling on pages with fixed/sticky content:
  BBC repaints 2.07 Mpx per scroll step (paint 25-28 ms of 35-43 ms per
  update, 11-22 updates/s); bilinear scaling of images is 7% of the main
  thread; copying the last frame forward is 1-3 ms per update (17% of the
  main thread while scrolling the Apple page). Wikipedia and GitHub scroll
  at 55-95 updates/s.
- **The bitmap transfer is cheap**: the title uploads a full 8 MB frame in
  ~1.0 ms (`[present]` lines of the owner's session; 0.05-0.17 cores for the
  title while browsing and during hardware video). The WebProcess used ~0.8
  cores while YouTube played.
- **Loads**: style, layout and JS during load make rendering updates of
  30-130 ms (YouTube: update 32 ms avg, p95 132; paint 8 ms avg for 1.1 Mpx).
- **NetworkProcess**: libsoup's HSTS store writes SQLite (journal created,
  written, deleted) on the network main thread for *every* response with a
  `Strict-Transport-Security` header, because each one moves the policy's
  expiry: The Verge load spent 0.48 s of CPU and **~3.3 s of wall time (12%)**
  of the thread that carries all network I/O and IPC in it; 1-7% of the CPU
  on the other sites. The disk cache's blob writes (SHA-1, mmap) run on a
  background thread (0.3-0.5 s per load).
- **Platform taxes** seen in the flat JS profile: libpas page construction
  and epoch reads ~2.5%, emulated TLS (`__emutls_get_address`) 0.5%,
  `MonotonicTime::now` 0.6%.
- `requestAnimationFrame` runs at 110-158/s while scrolling (63/s idle):
  rendering updates aren't paced to 60 Hz, so rAF-driven animations do up to
  2.5x the work.

**The owner's reports** (2026-10-31 session log, `tv17.log`, and harness runs):

- **YouTube "reloads"**: the session has exactly two top-level loads on
  YouTube (home, a channel); everything after is YouTube's in-page
  navigation. The title logs `load started:` with the *previous* URI, which
  makes back/forward look like reloads. Slow thumbnails: in the harness the
  thumbnails in view do load (IntersectionObserver, visibility and rAF work:
  `test/prof/visibility.html`); YouTube's main thread is 78% busy for ~15 s
  with JavaScript, and thumbnails are filled in by that JavaScript.
- **The Verge three times, reddit twice**: no reload loop. In the harness
  /tech loads once and stays (no `pushState`, no `beforeunload` in 16 s). The
  session's sequence is /tech -> an article (committed) -> back to /tech
  (the article's load ended by the back navigation) -> /tech again. reddit:
  old.reddit redirected to its login page, whose script then navigated to
  www.reddit.com.
- **Apple's product page**: no CSS scroll-driven animations
  (`animation-timeline`, `scroll-timeline`, `view-timeline`: none). It is
  Apple's own scroll-driven JS: scroll listeners, `getBoundingClientRect`,
  rAF, CSS custom properties (`setProperty('--...')` x52), transforms and
  opacity, sticky containers (19 sticky elements), and `inline-media`
  videos that are loaded and scrubbed as their sections scroll into view;
  the realtime viewer needs WebGL2 (we have none: it falls back to images).
  The page chose its full experience in the harness (`html.enhanced
  inline-media`, no `causeForBase`), but none of its 8 videos ever got a
  source while the page was scrolled end to end (`readyState` 0), so the
  sections that are videos stay still, which can look like scrolling being
  stuck in a pinned section. This was in the harness (no hardware decoder
  there; still to be checked in the title). Scrolling itself ran at 45-97
  updates/s in the harness. Without accelerated compositing every
  transform/opacity change is a CPU repaint.

**Ranked bottlenecks and fixes** (not done yet, apart from the pool and the
HSTS writes, see *JIT pool and HSTS writes*; effort is a rough guess):

1. **JavaScript on the main thread** (YouTube, news sites: 55-60% of a busy
   main thread). Cheapest lever first: a larger JIT pool
   (`JSC_jitMemoryReservationSize`, 128 -> 256-512 MB; YouTube's thresholds
   are x1.9 now), and an A/B of tier thresholds (`JSC_thresholdFor*`,
   `PS5WK_JIT`). Then the runtime itself: why `getOwnPropertySlot` on lexical
   environments and the parser show up so much (bytecode cache: JSC's
   `CachedBytecode` for repeat visits). Effort: hours for the pool A/B, days
   for the rest.
2. **CPU painting of fixed/sticky pages while scrolling** (BBC 2 Mpx per
   step, 11-22 updates/s): composited layers for fixed/sticky content
   painted once and moved, or GPU painting in the title (FEASIBILITY §4.8);
   smaller wins: cache scaled images (bilinear scaling 7%), skip the
   copy-forward when the whole view is repainted (1-3 ms). Effort: days to
   weeks.
3. **HSTS writes on the network thread** (up to 3.3 s of a load): keep HSTS
   policies in memory and write them only when they really change (a
   libsoup patch: ignore expiry moves of less than a day, or journal in
   memory/WAL). Effort: hours.
4. **Rendering updates not paced to the display** (rAF 110-158/s while
   scrolling): cap updates at 60/s. Effort: hours.
5. **Apple's videos never loading** (harness): check in the title with
   hardware decoding; if they stay empty, trace the page's inline-media
   load trigger. Effort: a day.
6. Platform taxes (libpas page construction, emulated TLS, clock reads):
   1-3% each, later.

## JIT pool and HSTS writes (2026-11-01)

The two cheap items from *Profiling*. Harness only (ps5wk-shot, log-only,
`test/prof/prof-run.sh` with `PS5WK_MEM_LOG=5`, summarized by
`test/prof/ab-summary.py`); runs over the internet, cold cache, 1920x1080.

**HSTS writes (deps patch `libsoup/0001-hsts-db-write-only-real-policy-changes.patch`).**
libsoup's `SoupHSTSEnforcerDB` wrote an SQLite transaction (journal
created, written, deleted) on the NetworkProcess main thread for every
response with `Strict-Transport-Security`, because each one moves the
policy's expiry by the seconds since the last. The patch remembers what the
database holds per host and writes only real changes: a new host, a removed
policy (max-age=0 or expired), includeSubDomains changed, or the expiry
moved by more than a day (or by half the max-age when that is shorter, so
short policies are still renewed). The in-memory policy is always the
current one, so enforcement is unchanged; after a restart a policy can
expire at most that much earlier. Rows read at startup are no longer
written back either (upstream rewrote every row).

| The Verge /tech, NetworkProcess main thread | runs | HSTS code | share |
| --- | --- | --- | --- |
| before (05dffa6 build, CPU profile) | 1 | 0.48 s of 1.84 s CPU | 26% |
| before (wall-clock profile) | 1 | 3.43 s of 27.4 s | 12.5% |
| after (CPU profile) | 7 | 0.00-0.01 s of 1.4-2.5 s CPU | 0.0-0.6% (median 0.4%) |

Persistence and enforcement were **not yet checked on the console** (the
console runs were stopped, see *Console health*): `test/net/hsts-test.sh`
does it in four ps5wk-shot runs against hsts.badssl.com (HSTS over https,
plain http without a redirect): the policy is stored, the row is not
rewritten by a later response in a new NetworkProcess, http:// commits as
https:// after a restart, and a control without a policy stays http. Still
not run (2026-11-01: console work stopped when FTP downloads slowed to
1-2.3 MB/s); the script now starts ps5wk-shot from the console's disk
(`SHOT_REMOTE`, as `prof-run.sh`), it used to send it to elfldr each run.

**JIT pool (ps5wk-child: 256 MB; `/data/ps5webkit/no-bigjitpool` keeps
128 MB).** JSC multiplies every tier-up threshold by
`available / (available - used)` with `available` = 3/4 of the pool (on
PS5 the pool is one anonymous RWX mapping managed by JSC's MetaAllocator,
not the libpas JIT heap). What the pool costs (`test/memprobe/rwxprobe.c`
and the kernel's `page table CPU` count): **one CPU page table per 2 MB
reserved, at mmap time, touched or not** (128 MB +64, 256 MB +128, 512 MB
+256, 1 GB +512; writing 48 MB into it added none; munmap gave them back).
Real memory is only the code pages JSC writes (and they stay resident:
decommit doesn't free on PS5).

A/B, 2 interleaved rounds, so **2 runs per page and size** (medians; ranges
in brackets where the runs differed):

| page | pool | threshold multiplier | main-thread CPU | process CPU | load finished | JIT code | libpas committed | peak rss |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| YouTube search | 128 MB | x1.65 (1.56-1.73) | 10.8 s (10.2-11.4) | 14.9 s | 13 s (9.6-16.3) | 36 MB (32-41) | 644 MB | 594 MB |
| | 256 MB | x1.49 | 13.8 s (12.0-15.6) | 19.8 s | 17 s | 62 MB (61-63) | 718 MB | 681 MB |
| | 512 MB | x1.21 | 11.9 s (11.6-12.1) | 17.1 s | 16 s | 34 MB (32-35) | 587 MB | 552 MB |
| YouTube channel | 128 MB | x1.49 | 7.3 s | 9.4 s | 9.3 s | 31 MB | 545 MB | 495 MB |
| | 256 MB | x1.22 | 7.1 s | 9.2 s | 9.6 s | 34 MB | 543 MB | 507 MB |
| | 512 MB | x1.10 | 7.8 s | 10.5 s | 9.9 s | 34 MB | 560 MB | 506 MB |
| The Verge /tech | 128 MB | x1.22 | 17.7 s | 28.5 s | 15.8 s (13.8-17.9) | 17 MB | 706 MB | 672 MB |
| | 256 MB | x1.10 | 17.2 s | 27.6 s | 14.6 s | 18 MB | 691 MB | 664 MB |
| | 512 MB | x1.05 | 17.0 s | 27.5 s | 13.3 s | 17 MB | 681 MB | 646 MB |

- The multiplier falls as the formula predicts. Everything else is within
  run-to-run noise with two runs: The Verge's CPU drifts down 4% from 128
  to 512 MB; on the YouTube channel 512 MB compiled more (DFG 929 against
  743, FTL 16 against 5) and used 7% more CPU; YouTube search differs more
  between its own runs (the results page changes from load to load) than
  between sizes, and wrote 61-63 MB of code at 256 MB in both runs
  (unexplained; 32-41 MB at the other sizes). No real-memory cost showed:
  JIT code, libpas committed and peak rss are the same within noise.
- **Default: 256 MB.** It takes most of the threshold inflation away
  (YouTube channel x1.49 -> x1.22, The Verge x1.22 -> x1.10) for +64 page
  tables per WebProcess against 512 MB's +192, and 512 MB showed no
  further gain. **How sure:** low. This rests on 2 runs per configuration,
  the mechanism (lower thresholds) is measured, a speedup is not. Tier-up
  threshold options (`JSC_thresholdFor*`) were not tried. More rounds were
  stopped for the console's health (below).

**Console health (open issue).** Two console crashes are not explained:
one during a 129 MB ps5wk-shot send to elfldr (an earlier session), and one
overnight 2026-10-31/11-01 at an unknown time after ~01:05, following
stalled 129 MB sends between 00:40 and 00:50; the console was found off and
repaired its database on boot. Measured since (klogsrv with Mac timestamps,
a ping every 5 s, a health check before each round):
- **Last night's link**: from ~00:40 FTP ran at ~0.12 MB/s down and
  ~0.5 MB/s up and elfldr took the 129 MB ps5wk-shot at a trickle (the
  Mac's send queue stood full for 10+ minutes; pings up to 1 s while
  sending, 5 ms idle). The owner's ps5upload sends multi-GB files without
  problems, so a big transfer alone is an unlikely cause.
- **Swap grows with our runs and doesn't come back**: SceShellCore's
  `swap out` went 0 -> 18 MB after the first run, 382 MB after 9 runs (about
  +20 MB per run), stayed at 378-380 MB after 6 minutes idle, and reached
  418 MB after 9 more (+4 MB per run). Idle system RSS fell from 1194 to
  551-617 MB: system processes are pushed out while a WebProcess holds
  ~0.7-1 GB, and their pages stay out until touched. Swap and memory
  pressure are the main suspect for the crashes.
- **elfldr keeps ~145 MB after a big ELF**: its FMEM stood at 145.2 of
  159.9 MB for the whole session after receiving ps5wk-shot over loopback
  (268 of 301 MB while receiving it); a freshly started elfldr has 4.1 of
  18.6 MB.
- **FTP downloads**: 8 MB/s after an idle gap, ~2.5 MB/s for a download
  started right after another transfer, also on a fresh boot (8.4, 3.4,
  2.6 MB/s back to back; 8.2 after 30 s idle). The apparent session-long
  slowdown from 8 to 2.4 MB/s was this timing effect, not the console
  degrading. Uploads 12-15 MB/s and idle pings 4-6 ms throughout.
- Not climbing: page tables peak at 6000-6400 during a run and return to
  4928-4951 after each; the kernel's `fdescfree(), but remain
  BUDGET_FD_FILE (ret=N)` warning at every process exit is a system-wide
  count that falls back to the same floor (403, 413 after an elfldr
  restart) after every run, so it is not a leak; no processes were left
  over after any run.
- **2026-11-01** (the feed round, 6 log-only title runs and the uploads
  of two builds): swap out stayed at 463-480 MB (it was 859 MB while the
  owner's session ran, 480 MB after it), page tables back at 4950-4966 after
  every run, pings 4-6 ms idle. FTP downloads fell during the round: 5-6
  MB/s right after an upload at 08:39 and 09:30, 1 MB/s at 10:03, and
  2.3 MB/s, then 1.4-1.7, after a 60 s idle gap (8 MB/s after a gap on
  earlier days), with pings up to 250-311 ms while a download ran; uploads
  10-14 MB/s. Console work stopped there (the HSTS check was not run).
- **2026-11-01, after the owner's reboot** (the spinner round: 6 log-only
  title runs of 100-160 s, 4 uploads of the 129 MB child into the title
  folder, klogsrv re-sent (136 KB)): swap out 385.9-392.4 MB the whole time
  (389 at the start: no growth), page tables up to 6286 during a run and
  back to 4972-4974 after each, pings 11 ms average (404 ms max, during
  transfers; 0 lost), no panic. FTP: 8 MB uploads 11-13 MB/s, the 129 MB
  child 6.6-8.4 MB/s, downloads 2 MB/s right after a transfer and 4 MB/s
  (a 4.6 MB file) after 60 s idle.
- Mitigation in the tooling: `run-shot.sh SHOT_REMOTE=<path>` (prof-run.sh
  uses it by default) uploads ps5wk-shot once over FTP and starts it from
  the console's disk through `test/shotrelay` (a small payload that sends
  it to the local elfldr, as the title sends its children), so no large
  payload goes from the Mac to elfldr any more.

## JSC `Date` (FEASIBILITY §8 item 5)

Not a clock bug. WTF's wall clock on this libc is
`clock_gettime(CLOCK_REALTIME)` (`WTF/wtf/CurrentTime.cpp`, generic branch),
and it is right on the console: Spike 4's unmodified `jsc.elf` prints
`new Date().toISOString() = 2026-10-27T23:45:41.311Z` (the Mac read 23:27 GMT
at upload; the run includes a 20 s upload). The "1970" in Spike 4 came from
`js/hello.js`, which prints `new Date(0)`. The payload time zone is UTC
(`getTimezoneOffset() = 0`, `Intl` tz `UTC`); the children set `TZ=UTC`.
Showing local time needs the console's time-zone setting (libSceSystemService)
later. Console log: `port/webkit/test/date.js`, results below.

## PS5 WPE Platform backend (`wpe-ps5/`)

| file | |
| --- | --- |
| `WPEDisplayPS5` | connects EGL (ps5-opengl, one window surface, GL 4.6 core), one 1080p `WPEScreen`, the pad (8 ms poll on the main loop), the IME dialog, and installs `WPEProcessManagerPS5` as the default process manager |
| `WPEToplevelPS5` | always fullscreen, the surface size |
| `WPEViewPS5` | `render_buffer`: hands the WebProcess's SHM buffer and its damage to the render thread, which copies and presents it; the buffer is reported rendered and released on the main thread afterwards |
| `PS5Renderer` | the GL side, on its own render thread: the page in a persistent GL buffer read by the shader (memcpy of damaged rows; `/data/ps5webkit/page-texture` = the old `glTexSubImage2D` path), hole-punch video under it (YUV -> RGBA on the GPU), round pointer sprite, swap (vsync), screen readback |
| `PS5VideoServer`, `PS5VideoProtocol.h` | hole-punch video: the WebProcesses' `ps5videosink` connections, frame queues, geometry, frame selection by due time (see *Hole-punch video*) |
| `PS5VideoDecoder` | hardware decoding (step 2b): `ps5vdecproxy` connections, decoder threads over an installed backend (Videodec2 in the title, a fake in ps5wk-shot), output slots in GL buffers |
| `WPEInputMethodContextPS5` | the system keyboard for focused fields |
| `WPEProcessManagerPS5` | children as elfldr payloads (below) |
| `PS5Sce.h` | weak declarations of libScePad and libSceUserService (no SDK headers); the libSceImeDialog function table that the title fills (`apps/title/titleime.c`) |

Pad map: D-pad = arrow keys (spatial navigation between links in the
browser); X = click at the pointer when it is shown (it shows for 8 s after
the left stick moves), otherwise Enter; O = Back; left stick = pointer;
right stick = scroll under the pointer; Options = the browser menu (address
bar, tabs, bookmarks, history; see *Tabs, bookmarks, history*);
Triangle = keyboard for the focused field (it also opens by itself when X
focuses a field); Square = reload; L1/R1 = previous/next field; L2/R2 = zoom;
R3 = Settings (XF86Tools; see *Settings*); L3 = home page; L3 + R3 =
performance overlay; touchpad = View in VR.

GL goes through libepoxy; in the title, `--wrap=dlopen/dlsym` and
`apps/title/dlshim.c` resolve epoxy's lookups to the statically linked
ps5-opengl (EGL 1.4 from its symbol table, GL from `eglGetProcAddress`).

## System keyboard (libSceImeDialog)

The system loads libScePad and libSceUserService for every title, but not
libSceImeDialog.sprx: listed as needed in eboot.bin, its imports stayed NULL.
`apps/title/titleime.c` loads it at start the way Spike 9 loads its decoder:
`sceSysmoduleLoadModule(0x96)` (the ImeDialog id in 13.60's libSceSysmodule
table, as on the PS4), `sceKernelLoadStartModule` of
`/<sandbox word>/common/lib/libSceImeDialog.sprx` for the handle, then
`sceKernelDlsym` (refused, as for every system library so far) and the
13.60 export offsets in `apps/title/ime_exports_1360.h` (anchor string
`checkImeDialogInitParam` checked first). libSceCommonDialog is bound the
same way and `sceCommonDialogInitialize` returns 0. Log on the console:

    ime: sysmodule 0x96 -> 0x00000000; load /<word>/common/lib/libSceImeDialog.sprx -> 0xe4
    ime: libSceImeDialog.sprx: 5 of 5 bound (0 by name, base 800e68000)
    ime: libSceCommonDialog.sprx: 1 of 1 bound (0 by name, base 800e48000)

`sceImeDialogInit` takes the PS4 `SceImeDialogParam` (96 bytes; the
disassembly copies 0x60 bytes and reads `type` at +4 and `option` at +0x20).
The second argument is the extended parameter (`PS5ImeParamExtended` in
`PS5Sce.h`, 136 bytes as in OpenOrbis and shadPS4): without it the dialog
took touchpad input as a pointer, so the key selector jumped around and the
D-pad couldn't move it. `wpe_display_ps5_request_text` passes it with
`PS5_IME_EXT_OPTION_DISABLE_POINTER` (0x40), and opens the dialog without it
if init refuses it.

When the keyboard opens by itself (`WPEInputMethodContextPS5.cpp`): a field
that gets focus within 1 s of a click opens it only if the pointer was over
an editable node. The title reports that from WebKitWebView
`mouse-target-changed` (`webkit_hit_test_result_context_is_editable`, to
`wpe_display_ps5_set_pointer_over_editable`), and the display sends a pointer
move at the pointer just before each click so the hit test is fresh; its
answer reaches the main thread before the focus change the click causes. A
search button whose click handler refocuses its field (the keyboard used to
reopen after each search) no longer opens it. Focus moved by the D-pad or
L1/R1 still opens it; Return (X with the pointer hidden), other buttons and
focus a page sets by itself (autofocus) don't. A click on a `<label>` doesn't
either (Triangle opens it). The log says `IME: field focused after a <why>:
keyboard opens / stays closed`. Test page: `test/input/refocus.html`.

The keyboard's OK acts like a phone keyboard's Go key: in a single-line
field the text is committed and then Return is pressed and released (real
keydown/keypress/keyup, as X sends with the pointer hidden), so the form or
the page's own Enter handler submits. The key events go to the WebProcess
after the committed text on the same connection, so the page sees the new
value first. OK is labelled Search in a search field (`type=search`), Go in
other single-line fields, and keeps the default label with no Return in a
textarea or contenteditable element: patch 0071 adds a multiline input hint
(`InputMethodState::Hint::Multiline` -> `WEBKIT_INPUT_HINT_MULTILINE` ->
`WPE_INPUT_HINT_MULTILINE`), and a new line in the text around the cursor
counts as multiline too. Cancel changes nothing. The log says
`IME: committed N bytes, then Return`.

A site's suggestion list that shows again after its search button is
clicked is the site's own behaviour: in desktop Chrome (2026-11-02) the
button's click handler puts the focus back in the field, the list shows,
and it goes when the results arrive and the site moves the focus to its
menu button (about 1.4 s later there; longer on the console while the
results load).

## Process launching

`WPEProcessManagerPS5` (WebKit's `WPEProcessManager` hook):

1. `launch()` dups WebKit's client socket, starts a thread that streams
   `ps5wk-child.elf` to 127.0.0.1:9021 followed by
   `PS5WK-LAUNCH 1 <web|network> <id> /system_tmp/ps5wk/ui-<pid>.sock`,
   and returns at once (WebKit's messages wait in the socketpair).
2. The payload reads that line from stdin (elfldr hands it the rest of the
   stream), creates `/system_tmp/ps5wk` (0777) and connects to the title's
   socket; the title binds it as soon as the directory exists.
3. hello → welcome + the IPC fd (SCM_RIGHTS); the child calls
   `WebKit::WebProcessMain` / `NetworkProcessMain` with
   `<name> <id> <fd>` on a 64 MB-stack thread.
4. The child's stdout/stderr come back on the elfldr connection; the title
   relays them to its log, prefixed `[web<id>]` / `[network<id>]`.

## Findings on the console (2026-10-28)

- **CPU page tables are a system-wide pool of 8192 pages** (`VM Stats ...
  page table CPU n/8192` in the kernel log), and the system already uses
  ~4550. Address-space reservations cost page tables on the PS5 even when
  untouched, and JSC reserves a 4 GB structure heap per process (8 GB
  while aligning). Three WebKit payloads exhausted the pool (`[KERNEL] system
  page table exhausted`): elfldr then failed to map new payloads
  (`elfldr.c:190:pt_mmap: Cannot allocate memory`) and libpas's 128 MB
  reservation failed (SIGILL in `pas_compact_heap_reservation`). Fixed in
  0011: a 128 MB structure heap, as Sony's PlayStation port uses. Watch that
  counter whenever something reserves address space.
- **elfldr takes one payload at a time.** Two concurrent uploads left the
  second payload with unapplied relocations (SIGSEGV in a static
  initializer). `WPEProcessManagerPS5` serializes uploads, waits for the
  previous payload's first output line and retries "Error spawning payload".
  Serialized, a 93 MB upload takes ~0.3 s and a child is connected
  0.7–1.3 s after `launch()`.
- **ICU data is not linked in** (stub data library) and is read from
  `/data/ps5webkit/share/icu/icudt76l.dat` (`ICU_DATA`), which keeps the
  child at 93 MB instead of 125 MB.
- The payload wall clock is right (see JSC `Date` above).

- **YouTube stayed on its loading skeleton because of `ENABLE_VIDEO=OFF`.**
  With page console messages on (`enable-write-console-messages-to-stdout`,
  `[webN] CONSOLE ...` lines in the title log and the shot output), the
  main bundle (kevlar_base) threw `ReferenceError: Can't find variable:
  HTMLVideoElement`: TypeScript decorator metadata names the class while
  components are defined, so the whole module aborted. A document-start user
  script (`apps/common/media-stubs.h`) defines placeholder
  `HTMLMediaElement`/`HTMLVideoElement`/`HTMLAudioElement` interfaces; with
  it the home page renders its app (cookie-consent dialog, guide, masthead)
  in `ps5wk-shot`. `<video>` stays `HTMLUnknownElement`, so playback is
  still Milestone 2. A watch page (`/watch?v=...`) loaded (title "Me at the
  zoo - YouTube") but its WebProcess died 1.7 s later (termination reason 0,
  no output); the console powered off shortly afterwards.
  `run-shot.sh` takes `SHOT_ARGS="settle=<s> js=<file on the console>"` to
  watch a page longer and dump page state.

- **file:// pages showed as source**: no shared-mime-info on the console,
  so GIO and WebKit's xdgmime had no extension map. A hand-written
  `share/mime/globs2` (port/deps `mime` step) plus `XDG_DATA_DIRS=/data/ps5webkit/share`
  in the children, the title and the shot fixes it:
  `file:///data/ps5webkit/start.html` loads as `text/html` in `ps5wk-shot`.
  The title's home page is that file again (`PS5WK_HOME_URL`; the console's
  `home-url.txt` now names it too).

- **Crash guard in the payloads** (`apps/common/crashguard.c`, ps5wk-child
  and ps5wk-shot): SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT/SIGTRAP/SIGSYS
  handlers on an alternate signal stack (every thread gets one through
  `--wrap=pthread_create`) print one report and `_exit(70)`, so a crashing
  payload no longer reaches the system crash reporter (suspected in two
  console power-offs after WebProcess crashes):

      [crash web5 pid 145] signal 11 (SIGSEGV) code 1 addr 0x... pc 0x... (elf +0x...) sp 0x... thread 0x...
      [crash web5 pid 145] stack: +0x... +0x...   (stack words after a call instruction, innermost first)

  `llvm-addr2line -f -C -e toolchain/wpe-webkit/build/bin/ps5wk-child 0x<elf offset>`.
  Do not read the payload's own ELF header: elfldr maps the first PT_LOAD
  (file offset 0x4000) at the image base, so `__ehdr_start` is the base but
  no header is there (the first build did, and faulted before its handlers
  were installed). The PS5 kernel's ucontext is not the SDK header's:
  FreeBSD's mcontext sits at +0x40 instead of +0x10 (`test/sigctx/sigctx.c`:
  rip +0xe0, rsp +0xf8, rbp +0x88); with the header's layout the first
  reports had `sp 0x0` and a wrong pc. `abort()` (all WebKit RELEASE_ASSERTs
  here) shows as SIGABRT code 0, pc in libkernel's raise, `addr` = pc + 2;
  the stack scan then gives the WebKit frames.
- **Payload threads had 64 KiB stacks.** The payload libc's default thread
  stack is 64 KiB (`test/stack/stack.c`: default attr 64 KiB, 16 KiB guard),
  and WTF leaves most thread types to the platform default, Web Workers'
  JavaScript threads included (JSC alone reserves 192 KiB of stack zones;
  only PLATFORM(PLAYSTATION) asks for 512 KiB). The crash guard's
  `pthread_create` wrapper now gives every thread without its own stack at
  least 2 MiB (FreeBSD's amd64 default). Prime suspect for the silent
  WebProcess death on YouTube watch pages (not re-tested yet). The title's
  own wrapper (`titlemem.c`) still uses the requested size.

- **YouTube in the title, TV test 2026-10-28**: the page failed with
  `Error receiving data: Connection reset by peer` three times, while the
  swapped-out start-page WebProcess aborted (SIGABRT, caught by the crash
  guard) and the NetworkProcess logged `Failed to create hard link` for
  every cache write. `ps5wk-shot` with the title's own session directories
  (`SHOT_ARGS="data=/data/ps5webkit/browser/data cache=/data/ps5webkit/browser/cache"`)
  loaded YouTube fine, so the cache contents and cookies (in memory only) were
  not the cause on their own. With patch 0013 (clean exit, cache copies)
  a log-only title run of start.html -> YouTube -> example.com -> YouTube
  loaded all four with no reset, no abort and no hard-link errors. The reset
  was not reproduced, so its cause stays open (most likely the aborting
  process or the record without a body). `new Audio()` is stubbed as well
  (`ReferenceError: Can't find variable: Audio` from voice search).

- **YouTube watch pages crashed every WebProcess** 4–7 s after loading
  (TV test 2026-10-28, a reload loop). Symbolized crash guard stack:
  `jsDocumentPrototypeFunction_startViewTransition` ->
  `Document::setActiveViewTransition` -> `RenderLayerCompositor::enableCompositingMode`
  -> `FrameRenderer::graphicsLayerFactory()` RELEASE_ASSERT (we paint without
  compositing). Fixed in 0014; `ps5wk-shot` keeps the watch page up for 30 s
  showing the player's "Your browser can't play this video". The title
  shows an error page instead of reloading after a second WebProcess crash
  on the same address within 30 s.
  Still open: `Error receiving data: Bad file descriptor` (and the earlier
  `Connection reset by peer`) on YouTube's main document in the title, both
  at the moment a swapped-out WebProcess died and the NetworkProcess saw its
  connection close; not reproduced in the harness or log-only title runs
  since those processes exit cleanly.

## Status and timings (2026-10-28)

### Builds (Mac, 14 cores, Release, -O2 -gline-tables-only)

| | |
| --- | --- |
| clean full build (`rm -rf build`, configure + ninja of ps5wk-child, ps5wk-shot, ps5browser, 8365 steps) | **747 s wall** (configure 37 s, ninja 710 s) |
| incremental relink after a one-file change | 7–31 s |
| disk | build ~4.8 GB, source clone ~0.85 GB (objects shared with ref/WebKit) |

All three executables link: `ps5wk-child` (WebProcess+NetworkProcess payload,
93 MB stripped, ICU data external), `ps5wk-shot` (headless UIProcess payload),
`ps5browser` (UIProcess, linked as a payload ELF; `apps/title/package.sh`
relinks it as the title, not yet run).

### Console (payload-only, `test/run-shot.sh`)

First light 2026-10-28: the WebProcess payload paints pages with Skia on the
CPU and hands them over as SHM buffers (thumbnail of the first one:
`test/first-light-example.com.png`).

    RESULT ps5wk-shot setup PASS UIProcess up at 0.0 s, loading https://example.com/
    RESULT ps5wk-shot network PASS first response committed at 1.4 s
    RESULT ps5wk-shot load PASS https://example.com/ finished at 1.4 s, title "Example Domain"
    RESULT ps5wk-shot paint FAIL 1280x720, 77 frames, first at 1.4 s, fnv64 b8e5138fc7b8a468, 100.00% non-white pixels
      (the page rendered correctly; the check counted the light-grey background as "non-white" and was fixed afterwards)

    RESULT ps5wk-shot setup PASS UIProcess up at 0.0 s, loading https://en.wikipedia.org/wiki/PlayStation_5
    RESULT ps5wk-shot network PASS first response committed at 1.5 s
    RESULT ps5wk-shot load PASS https://en.wikipedia.org/wiki/PlayStation_5 finished at 3.5 s, title "PlayStation 5 - Wikipedia"
    RESULT ps5wk-shot paint PASS 1280x720, 346 frames, first at 1.4 s, fnv64 4bdf213cb91b1482, 99.97% of pixels differ from the background

Both children launch through elfldr from a payload UIProcess (upload ~0.3 s,
connected 0.7–1.3 s after launch). The NetworkProcess runs TLS (OpenSSL) and
HTTP/2, and fontconfig/Noto (CJK included), images, CSS and JS all work.

What blocked it last, for the record: WebProcess<->NetworkProcess IPC
closed at the first out-of-line message. The cause was
**`fcntl(fd, F_DUPFD_CLOEXEC, 0)` returning 0 (fd 0, the payload's stdin
socket) in libkernel**, so `WTF::dupCloseOnExec` sent a socket in place of
shared memory (`test/seqpacket/dupfd.c`). Fixed with `__wrap_fcntl` in
`shim/ps5shim.c` (`-Wl,--wrap=fcntl`); requested in port/deps/REQUESTS.md
for ps5compat, since glib uses it too. Patch 0012 makes every
IPC-connection close path in `Platform/IPC/glib/ConnectionGLib.cpp` log why.

Other probes in `test/` (payloads, RESULT lines on stdout):
`jitpool` (2048 MB of JIT shm in one payload, no limit hit),
`seqpacket/seqpacket` (SCM_RIGHTS over stream/seqpacket/dgram PASS),
`seqpacket/shmfd` (SHM_ANON and named shm fds pass and map), `seqpacket/dupfd`
(the F_DUPFD_CLOEXEC bug), `seqpacket/reserve` (RW or PROT_NONE
reservations do not reduce `sceKernelAvailableFlexibleMemorySize`).

### Title PPSA99020 on the TV (2026-10-28, night run)

**The title loads and shows pages on the TV.** Verified from the log and a
screen readback (nobody watching the TV): example.com, Wikipedia and
DuckDuckGo Lite, the right way up and in the right colours.

    RESULT ps5browser display PASS PS5 display connected
    RESULT ps5browser load PASS https://example.com/ "Example Domain" (2.5 s after start)
    RESULT ps5browser url PASS 1/3 https://example.com/ loaded in 2.5 s: title "Example Domain"; flexible 334.0 MB
    RESULT ps5browser load PASS https://en.wikipedia.org/wiki/PlayStation_5 "PlayStation 5 - Wikipedia" (2.5 s after start)
    RESULT ps5browser url PASS 2/3 https://en.wikipedia.org/wiki/PlayStation_5 loaded in 2.5 s: title "PlayStation 5 - Wikipedia"; flexible 334.0 MB
    RESULT ps5browser load PASS https://lite.duckduckgo.com/lite?q=ps5 "ps5 at DuckDuckGo" (0.9 s after start)
    RESULT ps5browser url PASS 3/3 https://lite.duckduckgo.com/lite?q=ps5 loaded in 1.0 s: title "ps5 at DuckDuckGo"; flexible 334.0 MB
    RESULT ps5browser urls PASS 3 of 3 test URLs loaded
    [wpe-ps5] pad: init 0x0, user 285379873, open -> 53085952

Screen readbacks (960x540, halved from the 1080p frame on the TV):
`test/title-01-example.com-2s.png`, `test/title-03-en.wikipedia.org-2s.png`,
`test/title-05-lite.duckduckgo.com-2s.png` (+ the `-10s` ones, and
`title-first-frame.png`, the blank first frame). Load times count from
`webkit_web_view_load_uri`; a new WebProcess payload for the first page adds
~2 s before that (upload ~0.35 s, connected 1–2.3 s after launch).

Flexible memory (`sceKernelAvailableFlexibleMemorySize`, `[mem]` lines):

| milestone | flexible free | direct memory in use |
| --- | --- | --- |
| main (image already charged: 440 -> 335 MB) | 335.0 MB | heap 2 MB |
| display connected (EGL/GL up) | 334.0 MB | heap 12.8 MB |
| web view created | 334.0 MB | heap 22.8 MB, anon mmaps 256 MB (libpas 128 MB compact reservation, JSC 128 MB structure heap), 3 thread stacks |
| after example.com / Wikipedia / DDG | 334.0 / 334.0 / 334.0 MB | heap 26.8 MB (peak 34.7), anon mmaps 264–272 MB, 9–15 thread stacks (1.1–1.9 MB) |

Everything the UIProcess allocates is in direct memory; flexible memory
stays at 334 MB through three page loads (~190 MB more than Spike 10's
budget needs for the 8K decoder at depth 2).

#### Title vs payload: what broke, and the fixes

| symptom | cause | fix |
| --- | --- | --- |
| `[GLib ERROR] Creating pipes for GWakeup: Invalid argument` | `pipe2(O_CLOEXEC)` EINVAL in a title | `__wrap_pipe2` (`shim/ps5shim.c`): pipe() + flags by hand |
| `pad: libScePad/libSceUserService not linked` | only weak references to the SCE functions: imports came out NULL | strong references in `apps/title/dlshim.c`; the pad opens for the logged-in user |
| SIGILL in libpas (`pas_bootstrap_free_heap...`, `pas_immortal_heap...`) at `webkit_settings_new` | anonymous mmaps (libpas's 128 MB compact heap reservation) failed: flexible memory | `apps/title/titlemem.c`: anonymous RW mmaps -> direct memory (MAP_ALIGNED honoured, partial munmap, MAP_FIXED re-zeroing); Spike 10's `dheap.c` for malloc instead of ps5-opengl's 128 MB `app_heap.c`; thread stacks in direct memory (`--wrap=pthread_create/join/exit`); bmalloc through the system heap (`Malloc=1`) |
| `JSC::StructureMemoryManager` RELEASE_ASSERT | the structure heap wants 128 MB alignment (`MAP_ALIGNED(27)`) | honoured by the mmap wrapper |
| `IPC::createPlatformConnection` RELEASE_ASSERT | **`fcntl(F_GETFD/F_SETFD/F_DUPFD)` return EINVAL in a title** | `__wrap_fcntl`: FD flags report success (no exec in a title), F_DUPFD falls back to dup |
| SYSTEM_ILLEGAL_FUNCTION_CALL in `dup` | **libc.a's `syscalls.o` makes raw syscalls, which a title may not**, and once linked it overrides libkernel's exports (dup) | the title links `libc-title.a` (libc.a without syscalls.o); `apps/title/titlesys.c` provides what libkernel lacks (openat/fstatat/unlinkat on AT_FDCWD or absolute paths, eaccess, utimensat, chown family, closefrom, statfs/getfsstat ENOSYS, readlink/link/symlink) |
| `dup(ipc fd) errno 1` | **`dup()` is EPERM in a title** (dup2 too) | dup through SCM_RIGHTS to ourselves (`scmDup`, `--wrap=dup,dup2`) |
| SIGSEGV at address 0 in `currentExecutablePath` | `libScePosixForWebKit.so`'s stub exports `__wrap_readlink`, `__wrap_link`, `__wrap_symlink` (and `__wrap_mmap`); lld bound them instead of libps5compat's and they were NULL at run time | that stub is no longer linked |
| first page never finished loading (UI main loop blocked) | **`F_SETFL O_NONBLOCK` and FIONBIO on sockets are EACCES in a title**: GSocket's fds stayed blocking | emulated O_NONBLOCK: `__wrap_fcntl` remembers it, `--wrap=recvmsg,sendmsg,recv,send,recvfrom,sendto` add MSG_DONTWAIT, `--wrap=close` forgets it |
| nothing on stderr | `dup2(log, 2)` refused | `stderr = fdopen(log socket)` in `ps5browser.c` |

Probes in the title log at every start: `imports: N of M NULL` (every
function the title imports, from `apps/title/importcheck.c`, generated by
`package.sh`), and `probe:` lines (socketpair types, fcntl, FIONBIO,
MSG_DONTWAIT, dup, pipe2). Pipes behave (F_SETFL works on them); sockets
don't.

#### Process count and page tables

Each WebProcess payload costs ~300 of the system-wide 8192 CPU page tables
(`VM Stats ... page table CPU` in the kernel log: 4650 with nothing running,
5256 with the title + NetworkProcess + one WebProcess, 6155 after three
sites). WebKit kept every visited site's process (back/forward cache and
process cache) plus a prewarmed one, so 6 payloads were alive after three
navigations. `ps5browser.c` now sets `enable-page-cache` FALSE and the
DOCUMENT_BROWSER cache model, which should leave the current page's process
and one prewarmed process. **Built, not yet verified on the console** (the
console went offline during that run, below).

Children ended by aborting: when the UIProcess closed their IPC socket,
`exit()` ran static destructors and crashed in `WebCore::Font::~Font`
(ThreadGlobalData, WTFCrash), so every ended page process produced a crash
report for a ~600 MB payload. `apps/child/ps5wk-child.cpp` now `_exit`s
after WebKit returns. **Built, not yet verified.**

#### Packaging

`apps/title/package.sh` -> `toolchain/wpe-webkit/titles/PPSA99020` (200 MB:
`eboot.bin` 109 MB + `ps5wk-child.elf` 90 MB + param.json + libc.prx).
Title link specifics: `ps5-pie-webkit.ld` (WTF/JSC's page-aligned writable
sections moved into the data segment, or the converter fails with "LLVM
layout leaves no room for PS5 process parameters"); weak references that no
stub provides are defined as 0 (`_ZTH...` thread_local hooks, LSan hooks,
`__cxa_thread_atexit_impl`, `sceCommonDialogInitialize`, `__syscall`); the
SCE libraries the backend uses are linked without --as-needed;
ps5-opengl's `mkstemps` shim renamed (clashes with libc.a); libc.a without
`syscalls.o`, no `libScePosixForWebKit.so`; and the wraps:
malloc family + `aligned_alloc`/`memalign` (dheap), `mmap/munmap/madvise`,
`pthread_create/join/exit` (titlemem), `dlopen/dlsym` (dlshim, epoxy),
`fcntl/pipe2/dup/dup2`, `recvmsg/sendmsg/recv/send/recvfrom/sendto/close`
(ps5shim). Then a second link with `importcheck.o`.

Runtime needs on the console (already there): `/data/ps5webkit` runtime tree
from port/deps (certs, fonts, fonts.conf, xkb) and
`/data/ps5webkit/share/icu/icudt76l.dat` (uploaded by `test/run-shot.sh`).
Optional: `/data/ps5webkit/home-url.txt` (start page; Settings sets it),
`/data/ps5webkit/child.env` (`WEBKIT_DEBUG=...` for the children; very
verbose).

#### Debug knobs (title)

- `/data/ps5webkit/debug-screenshot` (a file, any content) or
  `PS5WK_SCREENSHOT=1`: the first page frame, and 2 s and 10 s after each
  test URL finishes, are read back (`glReadPixels` of the composed back
  buffer before the swap, ~100 ms), halved and flipped to top-down, and
  written to `/data/ps5webkit/shots/NN-<host>-2s.ppm`; the log says
  `[wpe-ps5] SHOT <path> ... mean, % non-black`. `test/fetch-shots.sh`
  fetches them over FTP and writes `test/title-*.png`.
  `wpe_display_ps5_capture_screen()` is the API.
- `/data/ps5webkit/test-urls.txt`: one URL per line; loaded in turn instead
  of the home page, each with RESULT lines (load time, flexible memory) and
  screenshots, 12 s apart; 60 s timeout per URL.
- `[mem]` lines at main, display, web view, every load and every 60 s.
- Crash triage: `toolchain/wpe-webkit/titles/ps5browser-symbols.elf`
  (refreshed by `package.sh`), `llvm-addr2line -f -C -i -e ... <rip - 0x400000>`.
  `TITLE_LDFLAGS="--trace-symbol=x" package.sh` traces the title link.

Not yet checked: the ImeDialog struct layout (keyboard), the pad map in
use (only the pad opening is verified), spatial navigation with the D-pad,
fonts for Arabic/Hebrew/Devanagari (example.com's second paragraph shows
boxes: no Noto font for that script in `/data/ps5webkit/fonts`).

#### Console state at the end of the night run (03:01 GMT)

The console dropped off the network (no ping, no ports) a few seconds into
the run that tested the page-cache/cache-model change, ~3 h 50 min after the
owner's last pad input, so most likely rest mode. The kernel log's last
lines were a system app starting (NPXS40112) and
`mDBG: Waiting for debug subsystem`. Earlier in the night a system app
(NPXS40112, SceSpZeroConf) had crashed once while several aborted child
payloads were being crash-reported; whether that is related is unknown.

### Next steps

1. Verify the two unverified fixes above on the console (page-table count
   staying flat while browsing; children exiting without crash reports):
   `tools/launch.sh PPSA99020` with `test-urls.txt` and watch
   `page table CPU` in the kernel log and `(output closed after ...)` lines.
2. Owner on the TV: pad map, Options -> keyboard -> URL, X on links,
   scrolling, zoom (see the morning checklist in the session report).
3. Move the ps5shim fcntl/pipe2/dup wrappers into libps5compat (port/deps)
   if glib-only programs need them too; today every WebKit executable links
   ps5shim with `--wrap=fcntl,pipe2` and the title adds the rest.
4. Fonts for more scripts; process prewarming at start; TZ from the system
   settings.

Debug knobs: `/data/ps5webkit/child.env` (KEY=VALUE lines read by the
child, e.g. `WEBKIT_DEBUG=Network,Loading,Process`); `SHOT_DEBUG=...` for
`run-shot.sh` sets WEBKIT_DEBUG in the UIProcess, which the NetworkProcess
inherits through its creation parameters; `ENABLE_RELEASE_LOG=ON` prints
RELEASE_LOG to stderr. The kernel log (`logs/klog-*.log`) shows crashes with
registers; symbolize with
`llvm-addr2line -e toolchain/wpe-webkit/build/bin/ps5wk-child <rip - 0x200000000>`.
