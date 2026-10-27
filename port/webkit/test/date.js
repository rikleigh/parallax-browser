// JSC Date on the console: wall clock, local time zone, Date(0) sanity.
const now = new Date();
print("Date.now() = " + Date.now());
print("new Date().toISOString() = " + now.toISOString());
print("new Date().toString() = " + now.toString());
print("getTimezoneOffset() = " + now.getTimezoneOffset());
print("Intl tz = " + new Intl.DateTimeFormat().resolvedOptions().timeZone);
print("performance-ish: " + (typeof performance !== "undefined" ? performance.now() : "n/a"));
const y = now.getUTCFullYear();
print("RESULT webkit-date " + (y >= 2026 ? "PASS" : "FAIL") + " new Date() = " + now.toISOString());
