// ps5wk-shot onload= script: answer YouTube's cookie dialog ("Reject all"),
// which otherwise pauses the video in a fresh session. Polls for 30 s.
// Upload to /data/ps5webkit/shot/.
(function () {
  var tries = 0;
  var timer = setInterval(function () {
    var b = Array.prototype.slice.call(document.querySelectorAll('button, tp-yt-paper-button, ytd-button-renderer a'))
      .filter(function (e) { return /^\s*(Reject all|Accept all)\s*$/i.test(e.textContent || ''); });
    var reject = b.filter(function (e) { return /reject/i.test(e.textContent); })[0] || b[0];
    if (reject) {
      reject.click();
      console.log('consent: clicked ' + reject.textContent.trim() + ' after ' + tries * 0.5 + ' s');
      clearInterval(timer);
    } else if (++tries > 60) clearInterval(timer);
  }, 500);
  return 'polling for the consent dialog';
})();
// Progress log every 3 s (title log / shot output: "YTSTATS ...").
setInterval(function () {
  var v = document.querySelector('video');
  if (!v) return;
  var q = v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality() : {};
  var p = document.getElementById('movie_player');
  console.log('YTSTATS t=' + v.currentTime.toFixed(2) + ' paused=' + v.paused + ' ready=' + v.readyState +
    ' buffered=' + (v.buffered.length ? v.buffered.end(v.buffered.length - 1).toFixed(1) : '-') +
    ' frames=' + q.totalVideoFrames + '/' + q.droppedVideoFrames + ' state=' + (p && p.getPlayerState ? p.getPlayerState() : '?') +
    ' size=' + v.videoWidth + 'x' + v.videoHeight);
}, 3000);
