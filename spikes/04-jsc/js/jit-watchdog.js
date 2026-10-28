//jsc: --watchdog=3000
// VM traps in JIT code: an endless loop that tiers up to DFG/FTL must be
// stopped by the watchdog (--watchdog=<ms>, a VMTraps termination request).
// With signal-based traps (the default with the DFG) the trap thread
// suspends the JS thread, reads its pc from the signal context and patches
// the optimised code; with --usePollingTraps=1 the code polls a flag.
// Expected: "WATCHDOG: start", then jsc reports the termination and exits;
// "WATCHDOG: FAIL" if the loop ever ends.
var log = (typeof print === "function") ? print : console.log;
function spin(n) { var s = 0; for (var i = 0; i < n; i++) s = (s + i) | 0; return s; }
log("WATCHDOG: start");
var t0 = Date.now(), x = 0;
for (;;) {
    x ^= spin(100000);
    if (Date.now() - t0 > 60000) break;
}
log("WATCHDOG: FAIL loop ran 60 s (" + x + ")");
