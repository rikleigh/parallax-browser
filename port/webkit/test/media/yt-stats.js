// ps5wk-shot js= dump for media pages (YouTube in particular): the first
// <video>'s state, playback quality, YouTube's own stats if present, and
// what MediaSource.isTypeSupported answers. Upload to /data/ps5webkit/shot/.
(function () {
  var r = {};
  var v = document.querySelector('video');
  if (v) {
    var q = v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality() : {};
    r.video = { src: (v.currentSrc || '').slice(0, 60), t: v.currentTime, dur: v.duration, paused: v.paused, ended: v.ended,
      muted: v.muted, ready: v.readyState, net: v.networkState, size: v.videoWidth + 'x' + v.videoHeight,
      total: q.totalVideoFrames, dropped: q.droppedVideoFrames, err: v.error ? v.error.code + ' ' + v.error.message : null,
      buffered: v.buffered.length ? v.buffered.start(0).toFixed(1) + '-' + v.buffered.end(v.buffered.length - 1).toFixed(1) : '' };
  }
  try {
    var p = document.getElementById('movie_player');
    if (p && p.getStatsForNerds) {
      var s = p.getStatsForNerds();
      r.yt = { codecs: s.codecs, res: s.resolution, dims: s.dims_and_frames, buffer: s.buffer_health_seconds, conn: s.bandwidth_kbps, state: p.getPlayerState && p.getPlayerState() };
    }
    if (p && p.getVideoData) r.ytVideo = p.getVideoData().title;
  } catch (e) { r.ytErr = String(e); }
  var types = ['video/mp4; codecs="avc1.640028"', 'video/webm; codecs="vp9"', 'video/webm; codecs="vp09.00.51.08"',
    'video/mp4; codecs="av01.0.08M.08"', 'audio/mp4; codecs="mp4a.40.2"', 'audio/webm; codecs="opus"',
    'video/webm; codecs="vp9"; width=3840; height=2160', 'video/webm; codecs="vp9"; width=1920; height=1080; framerate=60'];
  r.mse = {};
  if (window.MediaSource) types.forEach(function (t) { r.mse[t] = MediaSource.isTypeSupported(t); });
  r.vis = document.visibilityState + (document.hasFocus() ? ' focus' : ' nofocus');
  try {
    var gv = performance.getEntriesByType('resource').filter(function (e) { return /googlevideo|videoplayback/.test(e.name); });
    r.segments = gv.length + ' fetches: ' + gv.slice(-6).map(function (e) { return Math.round(e.startTime / 100) / 10 + 's ' + (e.transferSize || e.encodedBodySize || 0) + 'B ' + Math.round(e.duration) + 'ms'; }).join(', ');
  } catch (e) { r.segErr = String(e); }
  r.title = document.title;
  return JSON.stringify(r);
})();
