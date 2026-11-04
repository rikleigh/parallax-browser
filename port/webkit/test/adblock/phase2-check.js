// Ad blocker phase 2 check (port/webkit/ADBLOCK.md), as the title's
// /data/ps5webkit/test-onload.js: evaluated in each test URL's page 2 s after
// it loaded. Measurement only: what the page shows is reported, nothing is
// changed (except YouTube's cookie dialog, answered "Reject all", as
// measure.js does). Console lines the title relays ([webN]):
//   [phase2] <host> at <t> s: load 3456 ms, dcl 2100 ms, elements 4321, requests 87
//   [phase2] <host> ads: slots 3 shown 0 | player ad-showing no (samples 20, ad 0) | adPlacements undefined
// The YouTube part looks at the page's own ad containers (renderers whose
// name has "ad-slot", "promoted", "in-feed-ad", the masthead and player ad
// slots) only to count them; the blocking itself comes from uBO Lite's data.
(function () {
  var host = location.host;
  var t0 = Date.now();
  var youtube = /(^|\.)youtube\.com$/.test(host);
  function nav() { return performance.getEntriesByType('navigation')[0] || {}; }
  /* On screen with a size (an empty container that takes no space doesn't count). */
  function shown(e) { var r = e.getBoundingClientRect(); return r.width > 1 && r.height > 1 && getComputedStyle(e).visibility !== 'hidden'; }
  var samples = 0, adSamples = 0, adSeen = false, lastAdText = '';
  function sample() {
    var p = document.querySelector('.html5-video-player');
    if (!p) return;
    samples++;
    if (/\bad-showing\b|\bad-interrupting\b/.test(p.className)) {
      adSamples++;
      adSeen = true;
      var b = p.querySelector('.ytp-ad-text, .ytp-ad-preview-text, .ytp-ad-simple-ad-badge');
      lastAdText = b ? (b.textContent || '').trim().slice(0, 40) : '';
    }
  }
  function report(label) {
    var n = nav();
    console.log('[phase2] ' + host + ' at ' + label + ': load ' + Math.round(n.loadEventEnd || 0) + ' ms, dcl ' +
      Math.round(n.domContentLoadedEventEnd || 0) + ' ms, elements ' + document.getElementsByTagName('*').length +
      ', requests ' + performance.getEntriesByType('resource').length);
    if (!youtube) return;
    var slots = Array.prototype.slice.call(document.querySelectorAll(
      'ytd-ad-slot-renderer, ytd-in-feed-ad-layout-renderer, ytd-promoted-sparkles-web-renderer, ytd-promoted-video-renderer, ' +
      'ytd-display-ad-renderer, ytd-search-pyv-renderer, #masthead-ad, #player-ads, ytd-banner-promo-renderer, ytd-statement-banner-renderer'));
    var visible = slots.filter(shown);
    var pr = window.ytInitialPlayerResponse;
    var v = document.querySelector('video');
    console.log('[phase2] ' + host + ' ads at ' + label + ': slots ' + slots.length + ' shown ' + visible.length +
      (visible.length ? ' (' + visible.map(function (e) { var r = e.getBoundingClientRect(); return e.tagName.toLowerCase() + (e.id ? '#' + e.id : '') + ' ' + Math.round(r.width) + 'x' + Math.round(r.height); }).slice(0, 4).join(' ') + ')' : '') +
      ' | player ad-showing ' + (adSeen ? 'YES' : 'no') + ' (samples ' + samples + ', with ad ' + adSamples + (lastAdText ? ', "' + lastAdText + '"' : '') + ')' +
      ' | ytInitialPlayerResponse ' + (pr === undefined ? 'undefined' : 'adPlacements ' + (pr && pr.adPlacements ? pr.adPlacements.length : 'none') +
        ', playerAds ' + (pr && pr.playerAds ? pr.playerAds.length : 'none')) +
      (v ? ' | video t=' + v.currentTime.toFixed(1) + ' paused=' + v.paused + ' size=' + v.videoWidth + 'x' + v.videoHeight +
        ' duration=' + (isFinite(v.duration) ? v.duration.toFixed(0) : v.duration) : ''));
  }
  report('2 s');
  setTimeout(function () { report('12 s'); }, 10000);
  setTimeout(function () { report('30 s'); }, 28000);
  if (!youtube) return 'phase2 check';
  setInterval(sample, 500);
  var tries = 0;
  var timer = setInterval(function () {
    var b = Array.prototype.slice.call(document.querySelectorAll('button, tp-yt-paper-button, ytd-button-renderer a'))
      .filter(function (e) { return /^\s*(Reject all|Accept all)\s*$/i.test(e.textContent || ''); });
    var reject = b.filter(function (e) { return /reject/i.test(e.textContent); })[0] || b[0];
    if (reject) {
      reject.click();
      console.log('consent: clicked ' + reject.textContent.trim());
      clearInterval(timer);
    } else if (++tries > 60) clearInterval(timer);
  }, 500);
  return 'phase2 check (YouTube)';
})();
