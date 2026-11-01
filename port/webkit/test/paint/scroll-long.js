// Long scroll for memory and stability runs (COMPOSITING-PLAN.md, "WC title
// abort"): after 5 s, scrolls down 24 px per 16 ms tick for 4 minutes (an
// endless feed keeps loading), turning back up at the bottom, and logs
// "SCROLL-LONG <s> s: scrollY, height, videos (playing/total), rAF/s" every
// 10 s, then "SCROLL-LONG DONE". Title: /data/ps5webkit/test-onload.js.
(function () {
    var minutes = 4, step = 24, frames = 0, lastFrames = 0;
    (function raf() { frames++; requestAnimationFrame(raf); })();
    setTimeout(function () {
        var t0 = Date.now(), last = t0, dir = 1;
        console.log("SCROLL-LONG start, height " + document.documentElement.scrollHeight);
        var id = setInterval(function () {
            var now = Date.now(), max = document.documentElement.scrollHeight - innerHeight;
            if (scrollY >= max - 1)
                dir = -1;
            else if (scrollY <= 0)
                dir = 1;
            scrollBy(0, dir * step);
            if (now - last >= 10000) {
                var videos = document.querySelectorAll("video"), playing = 0;
                videos.forEach(function (v) { if (!v.paused && v.readyState >= 2) playing++; });
                console.log("SCROLL-LONG " + ((now - t0) / 1000).toFixed(0) + " s: scrollY " + scrollY + ", height " + document.documentElement.scrollHeight
                    + ", videos " + playing + "/" + videos.length + ", rAF " + ((frames - lastFrames) * 1000 / (now - last)).toFixed(1) + "/s");
                last = now;
                lastFrames = frames;
            }
            if (now - t0 > minutes * 60000) {
                clearInterval(id);
                console.log("SCROLL-LONG DONE, scrollY " + scrollY);
            }
        }, 16);
    }, 5000);
    return "scroll-long";
})();
