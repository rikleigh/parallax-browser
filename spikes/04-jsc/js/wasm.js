// WebAssembly smoke test + micro benchmark for the WASM=ON variant
// (jsc-ps5-wasm: JIT off, so WebAssembly runs only in the in-place
// interpreter, IPInt). Prints "WASM: PASS" / "WASM: FAIL" / "WASM: unavailable".
var log = (typeof print === "function") ? print : console.log;
var now = (typeof preciseTime === "function") ? () => preciseTime() * 1000 : () => Date.now();

if (typeof WebAssembly !== "object") {
    log("WASM: unavailable (built with ENABLE_WEBASSEMBLY=OFF)");
} else {
    // Tiny hand-assembled module:
    //   (func $add (param i32 i32) (result i32) local.get 0 local.get 1 i32.add)
    //   (func $sum (param i32) (result i32) (local i32)  ;; sum of n..1
    //      block loop local.get 0 i32.eqz br_if 1
    //        local.get 1 local.get 0 i32.add local.set 1
    //        local.get 0 i32.const 1 i32.sub local.set 0 br 0 end end local.get 1)
    function section(id, bytes) { return [id, bytes.length, ...bytes]; }
    function vec(items) { return [items.length, ...items.flat()]; }
    var addBody = [0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b];
    var sumBody = [0x01, 0x01, 0x7f,
        0x02, 0x40, 0x03, 0x40,
        0x20, 0x00, 0x45, 0x0d, 0x01,
        0x20, 0x01, 0x20, 0x00, 0x6a, 0x21, 0x01,
        0x20, 0x00, 0x41, 0x01, 0x6b, 0x21, 0x00,
        0x0c, 0x00, 0x0b, 0x0b,
        0x20, 0x01, 0x0b];
    var bytes = [0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        ...section(1, vec([[0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f], [0x60, 0x01, 0x7f, 0x01, 0x7f]])),
        ...section(3, vec([[0x00], [0x01]])),
        ...section(7, vec([[0x03, 0x61, 0x64, 0x64, 0x00, 0x00], [0x03, 0x73, 0x75, 0x6d, 0x00, 0x01]])),
        ...section(10, vec([[addBody.length, ...addBody], [sumBody.length, ...sumBody]])),
    ];
    try {
        var inst = new WebAssembly.Instance(new WebAssembly.Module(new Uint8Array(bytes)));
        var ok = inst.exports.add(2, 40) === 42 && inst.exports.sum(100) === 5050;
        var t0 = now();
        var r = 0;
        for (var i = 0; i < 200; i++) r = (r + inst.exports.sum(100000)) | 0;
        var dt = now() - t0;
        log("wasm sum loop: " + dt.toFixed(1) + " ms (checksum " + r + ")");
        log("WASM: " + (ok ? "PASS" : "FAIL"));
    } catch (e) {
        log("WASM: FAIL (" + e + ")");
    }
}
