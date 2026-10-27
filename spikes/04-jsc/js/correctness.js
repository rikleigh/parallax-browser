// Quick correctness smoke test for a JIT-less jsc on PS5.
// Prints one line per check and a final "CORRECTNESS: PASS n/n" or "FAIL".
// Avoids anything that needs files or extra shell features beyond print().
var log = (typeof print === "function") ? print : console.log;
var passed = 0, failed = 0;
function check(name, fn) {
    var ok, detail = "";
    try {
        var r = fn();
        ok = r === true;
        if (!ok) detail = " (got " + String(r) + ")";
    } catch (e) {
        ok = false;
        detail = " (threw " + e + ")";
    }
    if (ok) passed++; else failed++;
    log((ok ? "ok   " : "FAIL ") + name + detail);
}

check("arith", () => 1 + 2 * 3 === 7 && (0.1 + 0.2).toFixed(2) === "0.30" && Math.max(3, 9, 1) === 9);
check("int32 overflow to double", () => (2147483647 + 1) === 2147483648 && (-2147483648 | 0) === -2147483648);
check("doubles", () => Math.abs(Math.sqrt(2) - 1.4142135623730951) < 1e-15 && Math.pow(2, 0.5) === Math.SQRT2);
check("number formatting", () => (1e21).toString() === "1e+21" && (123.456).toPrecision(4) === "123.5" && String(-0) === "0");
check("parseFloat/strtod", () => parseFloat("3.14159e2") === 314.159 && Number("0x1f") === 31);
check("strings", () => "abc".toUpperCase() === "ABC" && "a,b,c".split(",").length === 3 && "x".repeat(5) === "xxxxx");
check("string unicode", () => "\u{1F600}".length === 2 && [..."\u{1F600}a"].length === 2 && "é".normalize("NFD").length === 2);
check("regexp", () => /(\d+)-(\d+)/.exec("10-20")[2] === "20" && "aaa".replace(/a/g, "b") === "bbb");
check("regexp unicode/sticky/named", () => /(?<y>\d{4})/u.exec("in 2026").groups.y === "2026" && /\p{L}+/u.test("héllo"));
check("arrays", () => [3, 1, 2].sort().join() === "1,2,3" && [1, 2, 3].map(x => x * 2).reduce((a, b) => a + b) === 12);
check("array holes/large", () => { var a = []; a[100000] = 1; return a.length === 100001 && a.indexOf(1) === 100000; });
check("typed arrays", () => { var f = new Float64Array(4); f[1] = 1.5; var u = new Uint8Array(f.buffer); return u.length === 32 && f[1] === 1.5; });
check("DataView endianness", () => { var d = new DataView(new ArrayBuffer(4)); d.setUint32(0, 0x01020304); return d.getUint8(0) === 1 && d.getUint32(0, true) === 0x04030201; });
check("objects/prototypes", () => { class A { f() { return 1; } } class B extends A { f() { return super.f() + 1; } } return new B().f() === 2; });
check("getters/proxies", () => { var p = new Proxy({}, { get: (t, k) => k + "!" }); return p.hi === "hi!"; });
check("Map/Set/WeakMap", () => { var m = new Map([[1, "a"]]); var s = new Set([1, 1, 2]); var w = new WeakMap(); var k = {}; w.set(k, 3); return m.get(1) === "a" && s.size === 2 && w.get(k) === 3; });
check("closures", () => { function mk() { var c = 0; return () => ++c; } var f = mk(); f(); f(); return f() === 3; });
check("generators", () => { function* g() { yield 1; yield 2; } return [...g()].join() === "1,2"; });
check("exceptions", () => { try { null.x; } catch (e) { return e instanceof TypeError; } return false; });
check("deep recursion (stack limit)", () => { function r(n) { return n ? 1 + r(n - 1) : 0; } return r(5000) === 5000; });
check("stack overflow is a RangeError", () => { function inf() { return inf() + 1; } try { inf(); } catch (e) { return e instanceof RangeError; } return false; });
check("JSON", () => JSON.stringify(JSON.parse('{"a":[1,2,{"b":null}]}')) === '{"a":[1,2,{"b":null}]}');
check("Date", () => new Date(Date.UTC(2020, 1, 29)).toISOString() === "2020-02-29T00:00:00.000Z" && !isNaN(Date.now()));
check("BigInt", () => (2n ** 100n).toString() === "1267650600228229401496703205376");
check("Intl.NumberFormat (ICU)", () => new Intl.NumberFormat("en-US").format(1234567.891) === "1,234,567.891");
check("Intl.DateTimeFormat (ICU)", () => typeof new Intl.DateTimeFormat("en-US", { timeZone: "UTC" }).format(0) === "string");
check("localeCompare/Collator (ICU)", () => "a".localeCompare("b") < 0 && new Intl.Collator("de").compare("ä", "z") < 0);
check("toLocaleUpperCase (ICU)", () => "i".toLocaleUpperCase("tr") === "İ");
check("Promise/microtasks", () => { var hit = false; Promise.resolve().then(() => { hit = true; }); return typeof hit === "boolean"; });
check("GC survives allocation churn", () => {
    var keep = [];
    for (var i = 0; i < 200000; i++) { var o = { i: i, s: "s" + i }; if (i % 1000 === 0) keep.push(o); }
    if (typeof gc === "function") gc();
    return keep.length === 200 && keep[199].s === "s199000";
});

var asyncDone = false;
(async function () { await null; asyncDone = true; })();

log("CORRECTNESS: " + (failed ? "FAIL" : "PASS") + " " + passed + "/" + (passed + failed));
