// Page-load breakdown from Navigation and Resource Timing, printed by
// ps5wk-shot as one "ONLOAD-JS ..." result (onload=<this file>): one NAV line,
// then one RES line per resource (the buffer keeps the first 250). Fields:
// name proto start dns connect tls ttfb download transfer encoded
// (ms; proto "-" = from the cache; cross-origin
// resources without Timing-Allow-Origin have dns/connect/tls/ttfb 0).
(() => {
    const ms = v => Math.round(v);
    const row = e => [e.name.slice(0, 100), e.nextHopProtocol || "-", ms(e.startTime),
        ms(e.domainLookupEnd - e.domainLookupStart), ms(e.connectEnd - e.connectStart),
        e.secureConnectionStart > 0 ? ms(e.connectEnd - e.secureConnectionStart) : 0,
        ms(e.responseStart - e.requestStart), ms(e.responseEnd - e.responseStart),
        e.transferSize, e.encodedBodySize].join(" ");
    const n = performance.getEntriesByType("navigation")[0];
    const lines = ["NAV " + row(n) + " dcl=" + ms(n.domContentLoadedEventEnd) + " load=" + ms(n.loadEventEnd)];
    for (const e of performance.getEntriesByType("resource"))
        lines.push("RES " + row(e));
    return lines.join("\n");
})()
