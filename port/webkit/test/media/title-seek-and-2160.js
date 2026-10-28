// Title log-only runs (/data/ps5webkit/test-onload.js, evaluated 2 s after
// each test URL loads): on video.html, seek forward, back and while paused
// (as seek.js does in the harness); on YouTube, answer the cookie dialog,
// ask for 2160p, and seek once after 25 s (MSE flush with hardware decoding).
(function () {
  var v = document.querySelector('video');
  if (location.hostname.indexOf('youtube') < 0) {
    if (!v) return 'no video';
    var steps = [[1000, function () { v.currentTime = v.duration * 0.6; }], [3000, function () { v.currentTime = 3; }],
                 [5000, function () { v.pause(); v.currentTime = v.duration * 0.8; }], [7000, function () { v.play(); }]];
    steps.forEach(function (s) {
      setTimeout(function () { s[1](); console.log('SEEKTEST step at ' + v.currentTime.toFixed(2) + ' paused=' + v.paused); }, s[0]);
    });
    return 'seeking';
  }
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
      p.setPlaybackQualityRange('hd2160', 'hd2160');
      console.log('YTQUALITY asked hd2160, now ' + (p.getPlaybackQuality ? p.getPlaybackQuality() : '?'));
    }
    if (++q > 6) clearInterval(qt);
  }, 2000);
  setTimeout(function () {
    var p = document.getElementById('movie_player');
    if (p && p.seekTo) { p.seekTo(120, true); console.log('SEEKTEST youtube seekTo 120'); }
  }, 25000);
  setInterval(function () {
    var w = document.querySelector('video');
    if (!w) return;
    var qq = w.getVideoPlaybackQuality ? w.getVideoPlaybackQuality() : {};
    console.log('YTSTATS t=' + w.currentTime.toFixed(2) + ' paused=' + w.paused + ' frames=' + qq.totalVideoFrames + '/' +
      qq.droppedVideoFrames + ' size=' + w.videoWidth + 'x' + w.videoHeight);
  }, 3000);
  return 'consent + hd2160 + seek at 25 s';
})();
