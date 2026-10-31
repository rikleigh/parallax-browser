// prof-run.sh: evaluated by ps5wk-shot (onload=) when the load finished.
// One "ONLOAD-JS" result: navigation milestones (ms from the navigation
// start), first paint / first contentful paint, and resources by type
// (count, KB transferred, from the cache). prof-run.sh appends scroll.js
// when SCROLL=1.
(() => {
    const ms = v => Math.round(v);
    const n = performance.getEntriesByType("navigation")[0] || {};
    const paint = {};
    for (const p of performance.getEntriesByType("paint"))
        paint[p.name] = ms(p.startTime);
    const types = {};
    for (const r of performance.getEntriesByType("resource")) {
        const t = types[r.initiatorType] || (types[r.initiatorType] = [0, 0, 0]);
        t[0]++;
        t[1] += (r.transferSize || 0) / 1024;
        if (!r.transferSize && r.decodedBodySize)
            t[2]++;
    }
    const res = Object.entries(types).map(([k, v]) => k + ":" + v[0] + "/" + Math.round(v[1]) + "KB/" + v[2] + "c").join(" ");
    const line = "PAGE ttfb=" + ms(n.responseStart) + " domInteractive=" + ms(n.domInteractive) + " dcl=" + ms(n.domContentLoadedEventEnd)
        + " load=" + ms(n.loadEventEnd) + " fp=" + (paint["first-paint"] ?? "-") + " fcp=" + (paint["first-contentful-paint"] ?? "-")
        + " dom=" + document.getElementsByTagName("*").length + " scripts=" + document.scripts.length
        + " height=" + document.documentElement.scrollHeight + " res " + res;
    console.log(line); // also a console line: scripts appended after this one return their own value
    return line;
})()
