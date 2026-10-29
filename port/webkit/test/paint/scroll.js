// paint-ab.sh: evaluated by ps5wk-shot (onload=) 2 s after the load finished.
// Scrolls the page down for 5 s and back up for 5 s, 40 px per 16 ms timer
// tick (rendering updates coalesce the ticks), between two console markers.
(function () {
    console.log("PAINT-SCROLL-START");
    var start = Date.now(), dir = 1, ticks = 0;
    var id = setInterval(function () {
        var t = Date.now() - start;
        if (t > 5000)
            dir = -1;
        if (t > 10000) {
            clearInterval(id);
            window.scrollTo(0, 0); // the same final frame in every mode (timer ticks vary)
            console.log("PAINT-SCROLL-END " + ticks + " ticks, scrollY " + window.scrollY + ", height " + document.documentElement.scrollHeight);
            return;
        }
        ticks++;
        window.scrollBy(0, dir * 40);
    }, 16);
    return "scrolling";
})();
