// ps5wk-shot onload= script: seek the first <video> forward, back and to a
// paused position, to check that frames keep coming after each flush.
// Upload to /data/ps5webkit/shot/.
(function () {
  var v = document.querySelector('video');
  if (!v) return 'no video';
  var steps = [[1000, function () { v.currentTime = 20; }], [3000, function () { v.currentTime = 5; }],
               [5000, function () { v.pause(); v.currentTime = 30; }], [7000, function () { v.play(); }]];
  steps.forEach(function (s) {
    setTimeout(function () { s[1](); console.log('SEEKTEST step at ' + v.currentTime.toFixed(2) + ' paused=' + v.paused); }, s[0]);
  });
  return 'seeking';
})();
