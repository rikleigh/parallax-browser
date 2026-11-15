// wasm-big.js - a WebAssembly kernel shaped like the PS2 emulator's hot
// thread (HANDOVER-fps.md: 22% of its time in OMG functions over 100 KB).
// The module is assembled here (no emcc/wat2wasm): one function that is an
// interpreter loop, an opcode byte from a 64 KB program, a br_table over 256
// cases, each case ~40 statements on 16 locals and a 16 MB data area (loads,
// stores, ALU, small ifs), about 120 KB of code in all. Deterministic.
//
//   jsc wasm-big.js [-- seconds]      (default 30)
// Prints:
//   WASM-BIG: module N bytes, function M bytes
//   WASM-BIG check: <checksum> (2000000 steps in T ms)   same in every tier and process
//   WASM-BIG t=<s> <rate> Msteps/s                         each second
//   WASM-BIG RESULT best <x> Msteps/s, last10 mean <y>, all mean <z>
const seconds = (typeof arguments !== "undefined" && arguments.length) ? +arguments[0] : 30;
const now = typeof preciseTime === "function" ? () => preciseTime() * 1000 : () => Date.now();
const out = typeof print === "function" ? print : console.log;

let seed = 0x12345678;
function rnd() { // xorshift32
    seed ^= seed << 13; seed >>>= 0;
    seed ^= seed >>> 17;
    seed ^= seed << 5; seed >>>= 0;
    return seed;
}
function pick(n) { return rnd() % n; }

function uleb(n, a) { do { let b = n & 0x7f; n >>>= 7; if (n) b |= 0x80; a.push(b); } while (n); }
function sleb(n, a) {
    for (;;) {
        const b = n & 0x7f; n >>= 7;
        if ((n === 0 && !(b & 0x40)) || (n === -1 && (b & 0x40))) { a.push(b); return; }
        a.push(b | 0x80);
    }
}

const PROG = 0, PROG_MASK = 0xffff, DATA = 0x10000, DATA_MASK = 0xfffffc, PAGES = 512; // 32 MB
const N_PARAMS = 1; // $n
const L_PC = 1, L_OP = 2, R0 = 3, NR = 16;
const OPS = [0x6a, 0x6b, 0x73, 0x71, 0x72, 0x6c, 0x74, 0x76, 0x77]; // add sub xor and or mul shl shr_u rotl

function lget(i, c) { c.push(0x20); uleb(i, c); }
function lset(i, c) { c.push(0x21); uleb(i, c); }
function i32c(v, c) { c.push(0x41); sleb(v | 0, c); }
function reg() { return R0 + pick(NR); }
function addr(c) { lget(reg(), c); i32c(DATA_MASK, c); c.push(0x71); i32c(DATA, c); c.push(0x6a); }

function statement(c) {
    switch (pick(8)) {
    case 0: case 1: { // r = r OP r
        const a = reg(); lget(reg(), c); lget(reg(), c); c.push(OPS[pick(OPS.length)]); lset(a, c); break;
    }
    case 2: { // r = r OP const
        const a = reg(); lget(reg(), c); i32c(rnd() & 0xfff | 1, c); c.push(OPS[pick(6)]); lset(a, c); break;
    }
    case 3: case 4: { // r = load(data + (r & mask))
        const a = reg(); addr(c); c.push(0x28, 0x02, 0x00); lset(a, c); break;
    }
    case 5: { // store(data + (r & mask), r)
        addr(c); lget(reg(), c); c.push(0x36, 0x02, 0x00); break;
    }
    case 6: { // if (r <u r) r = r + const
        lget(reg(), c); lget(reg(), c); c.push(0x49, 0x04, 0x40);
        const a = reg(); lget(a, c); i32c(rnd() & 0xff, c); c.push(0x6a); lset(a, c);
        c.push(0x0b); break;
    }
    case 7: { // r = select(r, r, r & 1)
        const a = reg(); lget(reg(), c); lget(reg(), c); lget(reg(), c); i32c(1, c); c.push(0x71, 0x1b); lset(a, c); break;
    }
    }
}

function buildFunction() {
    const c = [];
    // locals: pc, op, r0..r15 (i32)
    uleb(1, c); uleb(2 + NR, c); c.push(0x7f);
    // init registers
    for (let i = 0; i < NR; i++) { i32c(rnd(), c); lset(R0 + i, c); }
    c.push(0x02, 0x40); // block $done
    c.push(0x03, 0x40); // loop $L
    // if (!n) br $done; n--
    lget(0, c); c.push(0x45, 0x0d, 0x01);
    lget(0, c); i32c(1, c); c.push(0x6b); lset(0, c);
    // op = load8_u(PROG + pc)
    lget(L_PC, c); c.push(0x2d, 0x00, 0x00); lset(L_OP, c);
    const N = 256;
    for (let i = 0; i < N; i++) c.push(0x02, 0x40); // block $c255 ... $c0
    lget(L_OP, c); c.push(0x0e); uleb(N, c); // br_table
    for (let i = 0; i < N; i++) uleb(i, c);
    uleb(N - 1, c); // default
    for (let k = 0; k < N; k++) {
        c.push(0x0b); // end of block c_k: body k
        const count = 34 + pick(14);
        for (let s = 0; s < count; s++) statement(c);
        // pc = (pc + 1 + (r & 3)) & PROG_MASK
        lget(L_PC, c); i32c(1, c); c.push(0x6a); lget(reg(), c); i32c(3, c); c.push(0x71, 0x6a);
        i32c(PROG_MASK, c); c.push(0x71); lset(L_PC, c);
        c.push(0x0c); uleb(N - 1 - k, c); // br $L
    }
    c.push(0x0b); // end loop
    c.push(0x0b); // end block $done
    // return xor of the registers
    lget(R0, c);
    for (let i = 1; i < NR; i++) { lget(R0 + i, c); c.push(0x73); }
    c.push(0x0b);
    return c;
}

function section(id, body, m) { m.push(id); uleb(body.length, m); for (const b of body) m.push(b); }
function buildModule() {
    const m = [0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00];
    section(1, [0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f], m);            // type: (i32) -> i32
    section(3, [0x01, 0x00], m);                                     // function 0: type 0
    const mem = [0x01, 0x01]; uleb(PAGES, mem); uleb(PAGES, mem);    // memory min=max
    section(5, mem, m);
    section(7, [0x02, 0x03, 0x72, 0x75, 0x6e, 0x00, 0x00, 0x03, 0x6d, 0x65, 0x6d, 0x02, 0x00], m); // export "run" func 0, "mem" memory 0
    const body = buildFunction();
    const code = [0x01]; uleb(body.length, code); for (const b of body) code.push(b);
    section(10, code, m);
    return { bytes: new Uint8Array(m), fnBytes: body.length };
}

function instantiate(bytes) {
    const inst = new WebAssembly.Instance(new WebAssembly.Module(bytes), {});
    const u8 = new Uint8Array(inst.exports.mem.buffer);
    let s = 0x9e3779b9;
    for (let i = 0; i < DATA + (16 << 20); i++) { s ^= s << 13; s ^= s >>> 17; s ^= s << 5; u8[i] = s & 0xff; }
    return inst;
}

function dataHash(inst) {
    const u32 = new Uint32Array(inst.exports.mem.buffer, DATA, (16 << 20) / 4);
    let h = 0;
    for (let i = 0; i < u32.length; i += 97) h = (Math.imul(h, 31) + u32[i]) | 0;
    return h >>> 0;
}

const { bytes, fnBytes } = buildModule();
out(`WASM-BIG: module ${bytes.length} bytes, function ${fnBytes} bytes, ${seconds} s`);

// Check: a fresh instance, a fixed number of steps.
{
    const inst = instantiate(bytes);
    const t0 = now();
    let r = 0;
    for (let i = 0; i < 20; i++) r = (r ^ inst.exports.run(100000)) | 0;
    out(`WASM-BIG check: ${(r >>> 0).toString(16)}-${dataHash(inst).toString(16)} (2000000 steps in ${(now() - t0).toFixed(0)} ms)`);
}

// Rate: steps per second, each second.
{
    const inst = instantiate(bytes);
    let chunk = 20000, steps = 0, start = now(), mark = start;
    const rates = [];
    while (now() - start < seconds * 1000) {
        const t0 = now();
        inst.exports.run(chunk);
        const dt = now() - t0;
        steps += chunk;
        if (dt < 20) chunk = Math.min(chunk * 2, 1 << 24);
        else if (dt > 80 && chunk > 1000) chunk >>= 1;
        const t = now();
        if (t - mark >= 1000) {
            const rate = steps / (t - mark) / 1000;
            rates.push(rate);
            out(`WASM-BIG t=${((t - start) / 1000).toFixed(0)} ${rate.toFixed(2)} Msteps/s`);
            steps = 0; mark = t;
        }
    }
    const mean = a => a.reduce((x, y) => x + y, 0) / a.length;
    out(`WASM-BIG RESULT best ${Math.max(...rates).toFixed(2)} Msteps/s, last10 mean ${mean(rates.slice(-10)).toFixed(2)}, all mean ${mean(rates).toFixed(2)} (${rates.length} s)`);
}
