// prof-run.sh DIAG=1: page diagnostics printed as console lines ("DIAG ...")
// 3 s and 12 s after the load: which experience a feature-detecting page
// chose (html classes, Apple's causeForBase), videos and their state,
// canvases, sticky elements, how often requestAnimationFrame and scroll
// listeners run, images still loading, and navigations (history.pushState
// /replaceState/location changes, beforeunload) to tell real reloads from
// in-page navigations.
(() => {
    const t0 = performance.now();
    let raf = 0, scrolls = 0, pushes = 0, replaces = 0;
    const origRAF = window.requestAnimationFrame.bind(window);
    window.requestAnimationFrame = cb => { raf++; return origRAF(cb); };
    window.addEventListener("scroll", () => scrolls++, { passive: true, capture: true });
    const hp = history.pushState.bind(history), hr = history.replaceState.bind(history);
    history.pushState = function (...a) { pushes++; console.log("DIAG pushState " + String(a[2]).slice(0, 120)); return hp(...a); };
    history.replaceState = function (...a) { replaces++; return hr(...a); };
    window.addEventListener("beforeunload", () => console.log("DIAG beforeunload at " + Math.round(performance.now() - t0) + " ms"));
    window.addEventListener("popstate", () => console.log("DIAG popstate " + location.href.slice(0, 120)));
    const report = label => {
        const html = document.documentElement;
        const videos = [...document.querySelectorAll("video")];
        const imgs = [...document.images];
        let sticky = 0;
        for (const e of document.querySelectorAll("*"))
            if (getComputedStyle(e).position === "sticky") sticky++;
        const gl2 = !!document.createElement("canvas").getContext("webgl2");
        const gl = !!document.createElement("canvas").getContext("webgl");
        console.log("DIAG " + label + " html=[" + html.className.slice(0, 300) + "] causeForBase=" + (html.causeForBase || "-")
            + " webgl=" + gl + " webgl2=" + gl2 + " canvases=" + document.querySelectorAll("canvas").length
            + " sticky=" + sticky + " rafPerS=" + (raf / ((performance.now() - t0) / 1000)).toFixed(1) + " scrollEvents=" + scrolls
            + " push=" + pushes + " replace=" + replaces + " url=" + location.href.slice(0, 100)
            + " imgs=" + imgs.length + " incomplete=" + imgs.filter(i => !i.complete).length
            + " lazy=" + imgs.filter(i => i.loading === "lazy").length + " zeroSize=" + imgs.filter(i => i.complete && !i.naturalWidth).length);
        const empty = imgs.filter(i => i.complete && !i.naturalWidth);
        const ytimg = performance.getEntriesByType("resource").filter(r => /ytimg|\.(jpe?g|png|webp|avif)(\?|$)/.test(r.name));
        console.log("DIAG " + label + " image requests=" + ytimg.length + " (last at " + (ytimg.length ? Math.round(ytimg[ytimg.length - 1].startTime) : "-")
            + " ms); empty imgs in view=" + empty.filter(i => { const r = i.getBoundingClientRect(); return r.bottom > 0 && r.top < innerHeight && r.width > 0; }).length);
        for (const i of empty.slice(0, 3))
            console.log("DIAG empty img " + i.outerHTML.slice(0, 200) + " parent=" + (i.parentElement ? i.parentElement.tagName + "." + i.parentElement.className.slice(0, 60) : "-"));
        for (const v of videos.slice(0, 8))
            console.log("DIAG video " + (v.currentSrc || v.src || "-").slice(0, 100) + " ready=" + v.readyState + " paused=" + v.paused
                + " t=" + v.currentTime.toFixed(2) + " err=" + (v.error ? v.error.code : 0) + " autoplay=" + v.autoplay + " muted=" + v.muted
                + " " + v.clientWidth + "x" + v.clientHeight);
    };
    setTimeout(() => report("3s"), 3000);
    setTimeout(() => report("12s"), 12000);
    return "diag armed";
})()
