// prof-run.sh YTCONSENT=1 (prepended to the onload script): on YouTube without
// a consent cookie, set the "reject all" one (SOCS=CAI) and reload, so the run
// profiles the home feed instead of the consent dialog. The reload is logged.
// On consent.youtube.com (some pages redirect there first) it goes on to the
// page the redirect came from.
if (location.hostname == "consent.youtube.com") {
    document.cookie = "SOCS=CAI; domain=.youtube.com; path=/; max-age=31536000; secure; samesite=lax";
    const next = new URLSearchParams(location.search).get("continue");
    console.log("YTCONSENT cookie set on the consent page at " + Math.round(performance.now()) + " ms, going on to " + next);
    if (next)
        location.href = next;
} else if (/(^|\.)youtube\.com$/.test(location.hostname) && !/(^|; )SOCS=/.test(document.cookie)) {
    document.cookie = "SOCS=CAI; domain=.youtube.com; path=/; max-age=31536000; secure; samesite=lax";
    console.log("YTCONSENT cookie set at " + Math.round(performance.now()) + " ms, reloading");
    location.reload();
}
