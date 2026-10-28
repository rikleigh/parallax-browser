// ps5wk-shot onload= script: pause the first <video> for 2 s, then resume
// (what YouTube's cookie dialog does), to check that frames keep coming
// after a pause. Upload to /data/ps5webkit/shot/.
(function () {
  var v = document.querySelector('video');
  if (!v) return 'no video';
  setTimeout(function () {
    v.pause();
    console.log('PAUSERESUME paused at ' + v.currentTime.toFixed(2));
    setTimeout(function () { v.play(); console.log('PAUSERESUME resumed at ' + v.currentTime.toFixed(2)); }, 2000);
  }, 1000);
  return 'pause in 1 s, resume 2 s later';
})();
