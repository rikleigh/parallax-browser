// Ad-blocker A/B measurement (port/webkit/ADBLOCK.md), as the title's
// /data/ps5webkit/test-onload.js: evaluated in each test URL's page 2 s
// after it loaded. Prints, as console lines the title relays ([webN]):
//   [adblock-measure] <host> at 2 s: requests 87, transfer 1234 KB (visible), load 3456 ms, dcl 2100 ms
// and the same 15 s later ("at 17 s": ads keep loading after onload).
// Requests: Resource Timing entries (requests that went out; blocked ones
// never do); the buffer is raised to 2000 entries, but loads before this
// script ran are capped at the default 250 ("(capped)"). Transfer: only
// same-origin and Timing-Allow-Origin responses report sizes, so a lower
// bound. Blocked requests are counted from the log ("CONTENTBLOCKER INFO").
// On YouTube it also answers the cookie dialog and prints YTSTATS lines
// (port/webkit/test/media/yt-consent.js).
(function () {
  try { performance.setResourceTimingBufferSize(2000); } catch (e) {}
  function report(label) {
    var r = performance.getEntriesByType('resource');
    var n = performance.getEntriesByType('navigation')[0] || {};
    var bytes = 0;
    for (var i = 0; i < r.length; i++)
      bytes += r[i].transferSize || 0;
    console.log('[adblock-measure] ' + location.host + ' at ' + label + ': requests ' + r.length + (r.length === 250 ? ' (capped)' : '') +
      ', transfer ' + Math.round(bytes / 1024) + ' KB (visible), load ' + Math.round(n.loadEventEnd || 0) + ' ms, dcl ' +
      Math.round(n.domContentLoadedEventEnd || 0) + ' ms');
  }
  report('2 s');
  setTimeout(function () { report('17 s'); }, 15000);
  if (!/youtube\.com$/.test(location.host))
    return 'measuring';
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
  setInterval(function () {
    var v = document.querySelector('video');
    if (!v) return;
    var q = v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality() : {};
    console.log('YTSTATS t=' + v.currentTime.toFixed(2) + ' paused=' + v.paused + ' ready=' + v.readyState +
      ' frames=' + q.totalVideoFrames + '/' + q.droppedVideoFrames + ' size=' + v.videoWidth + 'x' + v.videoHeight);
  }, 5000);
  return 'measuring (YouTube)';
})();
