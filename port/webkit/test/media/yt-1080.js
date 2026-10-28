// ps5wk-shot onload= script: yt-consent.js, then ask YouTube's player for
// 1080p (setPlaybackQualityRange), so a 1920x1080 stream is decoded even in
// the small inline player. Upload to /data/ps5webkit/shot/.
(function () {
  var tries = 0;
  var timer = setInterval(function () {
    var b = Array.prototype.slice.call(document.querySelectorAll('button, tp-yt-paper-button, ytd-button-renderer a'))
      .filter(function (e) { return /^\s*(Reject all|Accept all)\s*$/i.test(e.textContent || ''); });
    var reject = b.filter(function (e) { return /reject/i.test(e.textContent); })[0] || b[0];
    if (reject) { reject.click(); console.log('consent: clicked ' + reject.textContent.trim()); clearInterval(timer); }
    else if (++tries > 60) clearInterval(timer);
  }, 500);
  var q = 0;
  var qt = setInterval(function () {
    var p = document.getElementById('movie_player');
    if (p && p.setPlaybackQualityRange) {
      p.setPlaybackQualityRange('hd1080', 'hd1080');
      console.log('YTQUALITY asked hd1080, now ' + (p.getPlaybackQuality ? p.getPlaybackQuality() : '?'));
    }
    if (++q > 10) clearInterval(qt);
  }, 2000);
  return 'consent + hd1080';
})();
setInterval(function () {
  var v = document.querySelector('video');
  if (!v) return;
  var q = v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality() : {};
  console.log('YTSTATS t=' + v.currentTime.toFixed(2) + ' paused=' + v.paused + ' frames=' + q.totalVideoFrames + '/' + q.droppedVideoFrames +
    ' size=' + v.videoWidth + 'x' + v.videoHeight);
}, 3000);
