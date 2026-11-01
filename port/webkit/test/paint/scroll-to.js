// Title test-onload.js for CPU vs WC captures after scrolling
// (COMPOSITING-PLAN.md, M1/M3): 2 s after the load, scrolls down to
// 1520 px in 40 px steps (one per 16 ms timer tick) and stops there, so the
// title's +10 s screenshot shows the same scrolled state on both paths.
(function () {
    var y = 0;
    var id = setInterval(function () {
        y += 40;
        window.scrollTo(0, y);
        if (y >= 1520) {
            clearInterval(id);
            console.log("SCROLL-TO done, scrollY " + window.scrollY);
        }
    }, 16);
    return "scrolling";
})();
