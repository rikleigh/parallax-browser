// JIT stress for jsc on the PS5: hot code that tiers up to Baseline, DFG and
// FTL, OSR exits, exceptions thrown through JIT frames, GC while JIT code
// runs, invalidation (watchpoints), the regexp JIT, and stack overflow in
// JIT code. Every test prints a checksum: run once with the JIT and once
// with --useJIT=0 and compare the CHECK lines (run-on-ps5.sh / jit-check.sh);
// the script also checks what it can on its own. Prints
// "JIT-STRESS: PASS n/n" (self-checks) and the tiers reached.
var log = (typeof print === "function") ? print : console.log;
var has = (name) => typeof globalThis[name] === "function";
var dfgCompiles = (f) => has("numberOfDFGCompiles") ? numberOfDFGCompiles(f) : -1;
var gcNow = () => { if (has("fullGC")) fullGC(); else if (has("gc")) gc(); };
var edenNow = () => { if (has("edenGC")) edenGC(); };
var keepOut = (f) => { if (has("noInline")) noInline(f); };

var passed = 0, failed = 0, tiers = [];
function check(name, ok, detail) {
    if (ok) passed++; else { failed++; log("FAIL " + name + (detail !== undefined ? ": " + detail : "")); }
}
function checksum(name, value) { log("CHECK " + name + " " + value); }
function tier(name, f) { var n = dfgCompiles(f); tiers.push(name + "=" + n); return n; }
function hash(h, v) { return (Math.imul(h ^ (v | 0), 16777619) >>> 0); }

// 1. Integer loop (tiers up through all tiers; overflow check in the loop).
function intLoop(n) {
    var s = 0;
    for (var i = 0; i < n; i++) { s = (s + i * 7) | 0; s ^= s >>> 3; }
    return s;
}
keepOut(intLoop);
(function () {
    var r = 0;
    for (var k = 0; k < 200; k++) r = hash(r, intLoop(10000 + k));
    checksum("int-loop", r);
    tier("intLoop", intLoop);
})();

// 2. Double math, -0, NaN, overflow to double, Math.* intrinsics.
function mathKernel(n) {
    var a = 0.5, b = -0, nanCount = 0, big = 0x7ffffff0;
    for (var i = 0; i < n; i++) {
        a = Math.sqrt(a * a + i) / (1 + Math.abs(Math.sin(i)));
        if (Object.is(b * -1, 0)) b = -0;
        if ((i % 97) === 0 && isNaN(Math.sqrt(-i - 1))) nanCount++;
        big = big + 3; // int32 overflow -> double
    }
    return [a.toFixed(6), Object.is(b, -0), nanCount, big, Math.imul(0x7fffffff, 3), Math.clz32(i | 0), Math.fround(1 / 3)].join(",");
}
keepOut(mathKernel);
(function () {
    var r;
    for (var k = 0; k < 60; k++) r = mathKernel(20000);
    checksum("math", r);
    check("math -0", r.split(",")[1] === "true", r);
    check("math overflow", r.split(",")[3] === String(0x7ffffff0 + 3 * 20000), r);
    tier("mathKernel", mathKernel);
})();

// 3. OSR exit: specialised on int32, then fed doubles, strings, objects,
// undefined; results compared with a straightforward recomputation.
function addAll(arr) {
    var s = 0;
    for (var i = 0; i < arr.length; i++) s = s + arr[i];
    return s;
}
keepOut(addAll);
(function () {
    var ints = []; for (var i = 0; i < 1000; i++) ints.push(i);
    var r = 0;
    for (var k = 0; k < 300; k++) r += addAll(ints);
    check("osr ints", r === 300 * 499500, r);
    var dbl = ints.map((x) => x + 0.25);
    check("osr doubles", addAll(dbl) === 499500 + 250, addAll(dbl));
    var mixed = ints.slice(0, 10).concat(["x"]);
    check("osr string", addAll(mixed) === "45x", addAll(mixed));
    var withObj = [1, 2, { valueOf() { return 39; } }];
    check("osr valueOf", addAll(withObj) === 42, addAll(withObj));
    check("osr undefined", Number.isNaN(addAll([1, undefined])), addAll([1, undefined]));
    var big = [2147483647, 1];
    check("osr overflow", addAll(big) === 2147483648, addAll(big));
    // Back to ints: re-optimises after the exits.
    for (var k = 0; k < 300; k++) r = addAll(ints);
    check("osr re-ints", r === 499500, r);
    checksum("osr", [addAll(dbl), addAll(mixed), r].join(","));
    tier("addAll", addAll);
})();

// 4. Exceptions thrown from deep JIT frames, caught in JIT code; finally.
function thrower(i) { if ((i & 255) === 255) throw new RangeError("e" + i); return i & 7; }
function middle(i) { var x = 0; try { x = thrower(i) + 1; } finally { x += 2; } return x; }
function catcher(n) {
    var caught = 0, sum = 0;
    for (var i = 0; i < n; i++) {
        try { sum += middle(i); } catch (e) { if (e instanceof RangeError && e.message === "e" + i) caught++; }
    }
    return caught + ":" + sum;
}
keepOut(thrower); keepOut(middle); keepOut(catcher);
(function () {
    var r;
    for (var k = 0; k < 50; k++) r = catcher(25600);
    check("exceptions", r.split(":")[0] === "100", r);
    checksum("exceptions", r);
    tier("catcher", catcher);
})();

// 5. GC while JIT code runs: allocation in hot loops, full/eden GCs from
// inside them, then the object graph is walked and checked.
function buildList(n, tag) {
    var head = null;
    for (var i = 0; i < n; i++) {
        head = { next: head, i: i, tag: tag, arr: [i, i * 2, "s" + i], f: function () { return this.i; } };
        if ((i & 4095) === 0) edenNow();
    }
    return head;
}
function walk(head) {
    var s = 0, n = 0;
    for (var p = head; p; p = p.next) {
        if (p.arr[1] !== p.i * 2 || p.arr[2] !== "s" + p.i || p.f() !== p.i) return -1;
        s = hash(s, p.i ^ p.tag); n++;
    }
    return s + "/" + n;
}
keepOut(buildList); keepOut(walk);
(function () {
    var keep = [], r = 0, bad = 0;
    for (var round = 0; round < 40; round++) {
        var list = buildList(20000, round);
        if ((round % 8) === 0) gcNow();
        keep[round & 3] = list;
        var w = walk(list);
        if (w === -1) bad++;
        r = hash(r, parseInt(w));
    }
    gcNow();
    for (var j = 0; j < 4; j++) if (walk(keep[j]) === -1) bad++;
    check("gc integrity", bad === 0, bad);
    checksum("gc", r);
    tier("walk", walk);
})();

// 6. Polymorphic property access, getters/setters, prototype changes after
// optimisation (watchpoint invalidation / jettison).
function Shape(kind) { this.kind = kind; this.x = 1; }
Shape.prototype.area = function () { return this.x * 2; };
function getArea(o) { return o.area() + (o.extra || 0); }
keepOut(getArea);
(function () {
    var objs = [new Shape(1), { area() { return 5; }, extra: 1 }, Object.create(Shape.prototype, { x: { get() { return 10; } } }),
        new (class extends Shape { area() { return super.area() + 100; } })(2)];
    var s = 0;
    for (var k = 0; k < 50000; k++) s += getArea(objs[k & 3]);
    var before = s;
    Shape.prototype.area = function () { return this.x * 3; }; // invalidates the optimised code's assumption
    for (var k = 0; k < 50000; k++) s += getArea(objs[k & 3]);
    var mono = new Shape(3), m = 0;
    for (var k = 0; k < 100000; k++) m += getArea(mono);
    Object.defineProperty(Shape.prototype, "area", { value: function () { return -1; } });
    m += getArea(mono);
    check("invalidation", m === 300000 - 1, m);
    checksum("props", before + "," + s + "," + m);
    tier("getArea", getArea);
})();

// 7. Typed arrays, closures, arguments, generators, spread in hot code.
function typed(n) {
    var f = new Float64Array(256), u = new Uint8ClampedArray(256), i32 = new Int32Array(256);
    for (var i = 0; i < n; i++) {
        var j = i & 255;
        f[j] = f[j] * 0.5 + i; u[j] = i * 3; i32[j] = (i32[j] + i) | 0;
    }
    var h = 0;
    for (var j = 0; j < 256; j++) h = hash(h, f[j] | 0) ^ u[j] ^ i32[j];
    return h >>> 0;
}
function* gen(n) { for (var i = 0; i < n; i++) yield i * i; }
function sumArgs() { var s = 0; for (var i = 0; i < arguments.length; i++) s += arguments[i]; return s; }
function makeAdder(k) { return (x) => x + k; }
keepOut(typed); keepOut(sumArgs);
(function () {
    var r = 0;
    for (var k = 0; k < 40; k++) r = hash(r, typed(50000));
    var g = 0;
    for (var k = 0; k < 2000; k++) for (var v of gen(50)) g += v;
    var a = 0, adders = [makeAdder(1), makeAdder(2), makeAdder(3)];
    for (var k = 0; k < 100000; k++) a = adders[k % 3](a) & 0xffffff;
    var sa = 0;
    for (var k = 0; k < 50000; k++) sa += sumArgs(k, 1, 2, ...[3, 4]);
    check("generators", g === 2000 * 40425, g);
    checksum("misc", [r, g, a, sa].join(","));
    tier("typed", typed);
})();

// 8. Regexp JIT: captures, named groups, unicode, sticky, backrefs, replace.
function regexKernel(n) {
    var re1 = /(\d{4})-(\d{2})-(\d{2})T(\d+):(\d+)/;
    var re2 = /(?<word>[a-z]+)\s+\k<word>/i;
    var re3 = /\p{Script=Greek}+/u;
    var re4 = /a|b/y;
    var h = 0, text = "Log 2026-10-28T17:42 the the quick αβγ brown fox ";
    for (var i = 0; i < n; i++) {
        var m = re1.exec(text + i);
        h = hash(h, +m[1] + +m[2] + +m[5]);
        var w = re2.exec("Hello hello world " + (i & 7));
        h = hash(h, w.groups.word.length);
        h = hash(h, re3.exec(text)[0].length);
        re4.lastIndex = i & 1; h = hash(h, re4.test("ab") ? 1 : 0);
        h = hash(h, ("x" + i).replace(/(\d)/g, "<$1>").length);
    }
    return h;
}
keepOut(regexKernel);
(function () {
    var r = 0;
    for (var k = 0; k < 20; k++) r = regexKernel(5000);
    check("regexp sanity", "2026-10-28T17:42".match(/(\d{4})-(\d{2})/)[2] === "10");
    checksum("regexp", r);
})();

// 9. Stack overflow inside JIT code: RangeError, then normal service.
function recurse(n) { return n === 0 ? 0 : 1 + recurse(n - 1); }
function deep(n) { return deep(n + 1) + 1; }
keepOut(recurse);
(function () {
    var r = 0;
    for (var k = 0; k < 2000; k++) r += recurse(100);
    var overflowed = false;
    for (var k = 0; k < 5; k++) {
        try { deep(0); } catch (e) { overflowed = e instanceof RangeError; }
    }
    check("stack overflow", overflowed);
    check("after overflow", recurse(1000) === 1000);
    checksum("recursion", r);
})();

// 10. String building and JSON in hot loops (rope resolution, GC of strings).
function strings(n) {
    var h = 0;
    for (var i = 0; i < n; i++) {
        var s = "k" + i + ":" + (i * 1.5) + ";";
        var o = JSON.parse(JSON.stringify({ s: s, a: [i, s.length], n: null }));
        h = hash(h, o.a[1] + o.s.charCodeAt(1) + s.indexOf(":"));
    }
    return h;
}
keepOut(strings);
(function () {
    var r = 0;
    for (var k = 0; k < 20; k++) r = hash(r, strings(5000));
    checksum("strings", r);
})();

log("tiers (DFG compiles per function, -1 = unknown): " + tiers.join(" "));
log("JIT-STRESS: " + (failed ? "FAIL" : "PASS") + " " + passed + "/" + (passed + failed));
