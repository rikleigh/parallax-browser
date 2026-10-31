// prof-run.sh YTCONSENT=1 (prepended to the onload script): on YouTube without
// a consent cookie, set the "reject all" one (SOCS=CAI) and reload, so the run
// profiles the home feed instead of the consent dialog. The reload is logged.
if (/(^|\.)youtube\.com$/.test(location.hostname) && !/(^|; )SOCS=/.test(document.cookie)) {
    document.cookie = "SOCS=CAI; domain=.youtube.com; path=/; max-age=31536000; secure; samesite=lax";
    console.log("YTCONSENT cookie set at " + Math.round(performance.now()) + " ms, reloading");
    location.reload();
}
