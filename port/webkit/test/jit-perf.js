(function () {
    var n = (performance.getEntriesByType && performance.getEntriesByType("navigation")[0]) || {};
    var t = performance.timing || {};
    var paints = performance.getEntriesByType ? performance.getEntriesByType("paint") : [];
    var fcp = -1, fp = -1;
    for (var i = 0; i < paints.length; i++) {
        if (paints[i].name === "first-contentful-paint") fcp = Math.round(paints[i].startTime);
        if (paints[i].name === "first-paint") fp = Math.round(paints[i].startTime);
    }
    var nav0 = t.navigationStart || 0;
    var res = performance.getEntriesByType ? performance.getEntriesByType("resource") : [];
    var scripts = 0;
    for (var j = 0; j < res.length; j++) if (res[j].initiatorType === "script") scripts++;
    return "PERF " + JSON.stringify({
        ttfb: Math.round(n.responseStart || (t.responseStart - nav0)),
        dcl: Math.round(n.domContentLoadedEventEnd || (t.domContentLoadedEventEnd - nav0)),
        load: Math.round(n.loadEventEnd || (t.loadEventEnd - nav0)),
        fp: fp, fcp: fcp, resources: res.length, scripts: scripts,
        now: Math.round(performance.now()), title: document.title.slice(0, 40)
    });
})()
