// scroll.js plus a requestAnimationFrame counter (COMPOSITING-PLAN.md, M2):
// a rAF loop runs from the start, and every second the page logs how many
// callbacks ran ("RAF-RATE n/s"), then scrolls down for 5 s and back up for
// 5 s, 40 px per 16 ms timer tick, between the same markers as scroll.js.
// Title: /data/ps5webkit/test-onload.js; ps5wk-shot: onload=<this file>.
(function () {
    var frames = 0, windowStart = performance.now(), rates = [], scrolling = false;
    function count() {
        frames++;
        var now = performance.now();
        if (now - windowStart >= 1000) {
            var rate = frames * 1000 / (now - windowStart);
            if (scrolling)
                rates.push(rate);
            console.log("RAF-RATE " + rate.toFixed(1) + "/s" + (scrolling ? " scrolling" : ""));
            frames = 0;
            windowStart = now;
        }
        if (rates.length < 30)
            requestAnimationFrame(count);
    }
    requestAnimationFrame(count);
    setTimeout(function () {
        console.log("PAINT-SCROLL-START");
        scrolling = true;
        var start = Date.now(), dir = 1, ticks = 0;
        var id = setInterval(function () {
            var t = Date.now() - start;
            if (t > 5000)
                dir = -1;
            if (t > 10000) {
                clearInterval(id);
                window.scrollTo(0, 0);
                scrolling = false;
                var sum = 0;
                rates.forEach(function (r) { sum += r; });
                console.log("PAINT-SCROLL-END " + ticks + " ticks, scrollY " + window.scrollY + ", height " + document.documentElement.scrollHeight
                    + "; rAF " + (rates.length ? (sum / rates.length).toFixed(1) : "-") + "/s avg while scrolling, max " + (rates.length ? Math.max.apply(null, rates).toFixed(1) : "-"));
                rates.length = 30; // stop the rAF loop
                return;
            }
            ticks++;
            window.scrollBy(0, dir * 40);
        }, 16);
    }, 2000);
    return "scrolling";
})();
