#!/usr/bin/env node
/*
 * ubol-convert.js: uBlock Origin Lite's network rules (declarativeNetRequest)
 * to WebKit content-blocker JSON, and its cosmetic filters and scriptlets
 * (rulesets/scripting/, phase 2), for the list bundled with the browser
 * title (port/webkit/ADBLOCK.md). The conversions themselves are
 * port/webkit/adblock/dnr-to-webkit.js and ubol-scripting.js, the same
 * scripts the console runs when it updates the lists. Downloaded files are
 * only parsed (JSON, and known lines of uBO Lite's generated scripts),
 * never run.
 *
 *   node tools/ubol-convert.js            pinned release -> toolchain/adblock/
 *   node tools/ubol-convert.js --latest   newest release (prints the pins to put below)
 *   node tools/ubol-convert.js --check    convert and print the stats only
 *   node tools/ubol-convert.js --test     self-test of the regex/urlFilter mapping
 *
 * Source: github.com/uBlockOrigin/uBOL-home (GPL-3.0), the unpacked Chromium
 * build at the release tag (chromium/manifest.json lists the rulesets and
 * which are enabled by default; chromium/rulesets/main/<id>.json and
 * chromium/rulesets/regex/<id>.json are the DNR rules;
 * chromium/rulesets/*-details.json, chromium/rulesets/scripting/ and
 * chromium/js/scripting/css-procedural-api.js the cosmetic filters and
 * scriptlets). Writes adblock-rules.json (cosmetic rules first),
 * adblock-scripting.txt (the title's per-site data) and adblock-info.ini.
 * Downloads are kept in toolchain/adblock/cache/<version>/ (never
 * committed).
 */
'use strict';
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const DNRToWebKit = require(path.join(ROOT, 'port/webkit/adblock/dnr-to-webkit.js'));
const UBOLScripting = require(path.join(ROOT, 'port/webkit/adblock/ubol-scripting.js'));
/* The browser's own pages (file: start page, Settings): no generic cosmetics. */
const EXEMPT_TOP_URLS = ['^file:', '^http://127\\.0\\.0\\.1:9040/'];
const OUT = path.join(ROOT, 'toolchain/adblock');
const REPO = 'https://raw.githubusercontent.com/uBlockOrigin/uBOL-home';

/* The pinned release: its version and the sha256 of every file used
 * (manifest first, then main/ and regex/ rulesets, then the cosmetic and
 * scriptlet files; null: 404 at that tag). */
const PINNED_VERSION = '2026.1026.1812';
const PINNED_SHA256 = {
    'manifest.json': 'deb7a0f6c3f6e9019e850e9802843e3b9dea40d7321427245bacecb2a4e3231c',
    'rulesets/main/ublock-filters.json': '6f1c02a05cee75a9da272189868b4e9b1244367f8b9e4cd06ee1dbd484bd136c',
    'rulesets/regex/ublock-filters.json': 'd8ad067c768fdc6611523fe4a724826d8f0cf42ce78a823b928bf948b5019047',
    'rulesets/main/easylist.json': 'c61ef249a02460ddabed307a1f0907f0b667491cd5831c028cce8ab0f679bb1f',
    'rulesets/regex/easylist.json': '520c57625639ed6df11b4b893d0437b3e110b12de406112c49ce51e6dcc33940',
    'rulesets/main/easyprivacy.json': 'e592a1bd771f968ac3cb0857946425820df31a450621708b65888ea8fd9f14b1',
    'rulesets/regex/easyprivacy.json': '58f915430b560588e2f3a2f544e09294d9e15c38456547038ac74bea05a721fb',
    'rulesets/main/pgl.json': '782bfb76e37532da57728bcbf925e43956e673fb6db46bdda952e0c069a17f86',
    'rulesets/regex/pgl.json': null,
    'rulesets/main/ublock-badware.json': 'a7b019eb6fd590b11ea6495f87c69ac2514881680ee7dda9b54bd386b0c1eac6',
    'rulesets/regex/ublock-badware.json': 'daa240ac64d28d9b3d1a40afa3027b1b8c6179388dc4fa5d571b8d37d445a69e',
    'rulesets/main/urlhaus-full.json': 'fea43794adaa2417752046407bddd03ce65a4237361e27b5e6d6998208fd2e79',
    'rulesets/regex/urlhaus-full.json': null,
    'rulesets/ruleset-details.json': '0117c1724796bf231363faf51989b1469309f9eb76b7ca57df2ee0dc7e7ac026',
    'rulesets/scriptlet-details.json': '361103c5126aa14e6301e9057d7bc08ee9a120ddb7b51e1ce0fec5df5f7c39b2',
    'rulesets/generic-details.json': 'bcb2eb654e0fb4b362dd517261d9a2eef89ae3d77bb78df950744261feda831c',
    'rulesets/scripting/generic/ublock-filters.js': '5d3b43fce68ae3ff4564cc82f1d2c1f2849a5d15c0b6949d3003e7411a4f5a12',
    'rulesets/scripting/specific/ublock-filters.json': '9663f9804758588499926d6f4b8b8bd5d924df423ac6bb4beca7523a2b8f5c40',
    'rulesets/scripting/scriptlet/main/ublock-filters.js': '8e576b3a08e8c281ec13049af31331c4fff0349055937e4e1fd7f33bb2ad515a',
    'rulesets/scripting/scriptlet/isolated/ublock-filters.js': 'b87e018d307b85b9ae57486eb35962858635db42aa925fadb727c15893766331',
    'rulesets/scripting/generic/easylist.js': 'd6b1e8806b7fe426b26b8bb30a0b66c9134f7d48968ea053d0cc0b4c12a18f7f',
    'rulesets/scripting/specific/easylist.json': '9e011ab834bd4a126a757519bf3f837601aef95703a53bc86e5948b830fcc29f',
    'rulesets/scripting/generic/easyprivacy.js': 'ee1cf3812ab19c82b73b3d3e50aad1cf8bcb7351ceee1538a64adb66b5ca0eca',
    'rulesets/scripting/specific/easyprivacy.json': 'c35e55cc79fc7d895e75da74f34f3afc7a203f47b9c4831e4047d7356075ab6f',
    'rulesets/scripting/scriptlet/main/easyprivacy.js': '91dc6407ce31a064e9263fdf3a1400ce26a51ad9b6d8ff33c579e1a345576e9a',
    'rulesets/scripting/scriptlet/isolated/easyprivacy.js': '269b315c397604c094643e81574e3c4185f0ac309676aca944ff96a82f151ab0',
    'rulesets/scripting/generic/ublock-badware.js': 'a3fe007ee47a1d65768f3dbf3449b07fb3f98012b8c0a476dbf873d7a299efea',
    'rulesets/scripting/specific/ublock-badware.json': '71ec2acde775f815576f7dbae6da0c0c54a08a09daa7ff8b69095ba1b59f2c51',
    'rulesets/scripting/scriptlet/main/ublock-badware.js': '74766a4b536b8d53e0e05de25642895195c13a806e0c4b59cf8b85766f32d3ae',
    'rulesets/scripting/scriptlet/isolated/ublock-badware.js': '74dab86802e5d9c44cd1804e75c1dddac16fa3446dc1a5c5455d6b011d583585',
    'rulesets/scripting/scriptlet/main/scriptlet-begin.js': 'e22c2f3fd774347986e9184a1c15a3e40b7d94b8a94b69b329024df01b2de3be',
    'rulesets/scripting/scriptlet/isolated/scriptlet-begin.js': '56d81ff18fbbb0a114802b348b48fdafa79e3699a6f05f3e336856a38c781ea9',
    'js/scripting/css-procedural-api.js': 'cf5effd5ce562556dee74de3cfd4f0ac8d9202b6e74818a66b9842ee573241c8',
};

function sha256(buffer) {
    return crypto.createHash('sha256').update(buffer).digest('hex');
}

async function fetchFile(version, file, cacheDir, optional) {
    const cached = path.join(cacheDir, file);
    if (fs.existsSync(cached))
        return fs.readFileSync(cached);
    if (fs.existsSync(cached + '.404'))
        return null;
    const url = `${REPO}/${version}/chromium/${file}`;
    const response = await fetch(url);
    fs.mkdirSync(path.dirname(cached), { recursive: true });
    if (response.status === 404 && optional) {
        fs.writeFileSync(cached + '.404', '');
        return null;
    }
    if (!response.ok)
        throw new Error(`${url}: HTTP ${response.status}`);
    const data = Buffer.from(await response.arrayBuffer());
    fs.writeFileSync(cached, data);
    return data;
}

async function latestVersion() {
    const response = await fetch(`${REPO}/main/chromium/manifest.json`);
    if (!response.ok)
        throw new Error(`manifest: HTTP ${response.status}`);
    return (await response.json()).version;
}

function selfTest() {
    const t = DNRToWebKit.translateRegex;
    const cases = [
        ['^https?:\\/\\/[a-z]{8,15}\\.top\\/[a-z]{4}\\/', 'expands'],
        ['(^|\\/)ads\\/', ['^ads\\/', '\\/ads\\/']],
        ['\\/z-[a-z0-9]{7,10}(\\.js)?$', 'expands'],
        ['a|b', ['a', 'b']],
        ['\\d+', ['[0-9]+']],
        ['(?=x)', null],
        ['\\bfoo', null],
        ['a{2}', ['aa']],
        ['(a|b)*', null],
        ['(x$)?', null],
        ['[\\w-]+', ['[a-zA-Z0-9_\\-]+']],
    ];
    let failed = 0;
    for (const [input, expected] of cases) {
        const got = t(input);
        const ok = expected === 'expands' ? Array.isArray(got) && got.length > 0
            : JSON.stringify(got) === JSON.stringify(expected);
        if (!ok)
            failed++;
        console.log(`${ok ? 'ok  ' : 'FAIL'} ${input} -> ${JSON.stringify(got)}`);
    }
    const u = DNRToWebKit.urlFilterToRegex;
    const urlCases = [
        ['||ads.example.com^', '^[^:]+://+([^:/]+\\.)?ads\\.example\\.com([^a-zA-Z0-9_.%-].*)?$'],
        ['/banner/*.gif|', '/banner/.*\\.gif$'],
        ['|https://x.com/a?b', '^https://x\\.com/a\\?b'],
    ];
    for (const [input, expected] of urlCases) {
        const got = u(input);
        if (got !== expected)
            failed++;
        console.log(`${got === expected ? 'ok  ' : 'FAIL'} ${input} -> ${got}`);
    }
    /* Everything emitted must pass the anchor check WebKit applies. */
    for (const [input] of cases) {
        for (const p of t(input) || []) {
            if (!DNRToWebKit.checkAnchors(p)) {
                failed++;
                console.log(`FAIL anchors ${p}`);
            }
        }
    }
    console.log(failed ? `${failed} failed` : 'all passed');
    process.exit(failed ? 1 : 0);
}

async function main() {
    const args = process.argv.slice(2);
    if (args.includes('--test'))
        return selfTest();
    const latest = args.includes('--latest');
    const checkOnly = args.includes('--check');
    const version = latest ? await latestVersion() : PINNED_VERSION;
    const cacheDir = path.join(OUT, 'cache', version);
    const started = Date.now();

    const manifestData = await fetchFile(version, 'manifest.json', cacheDir, false);
    const manifest = JSON.parse(manifestData);
    if (manifest.version !== version)
        throw new Error(`manifest version ${manifest.version}, expected ${version}`);
    const enabled = JSON.parse(DNRToWebKit.rulesetsFromManifest(manifestData.toString())).rulesets;

    const digests = { 'manifest.json': sha256(manifestData) };
    const rulesets = [];
    for (const id of enabled) {
        for (const kind of ['main', 'regex']) {
            const file = `rulesets/${kind}/${id}.json`;
            const data = await fetchFile(version, file, cacheDir, kind === 'regex');
            digests[file] = data ? sha256(data) : null;
            if (data)
                rulesets.push({ id: kind === 'main' ? id : `${id}.regex`, rules: JSON.parse(data) });
        }
    }
    function checkPins() {
        if (!latest) {
            const mismatches = Object.keys(PINNED_SHA256).filter(f => PINNED_SHA256[f] !== digests[f])
                .concat(Object.keys(digests).filter(f => !(f in PINNED_SHA256)));
            if (mismatches.length && !args.includes('--repin')) {
                console.error(`sha256 differs from the pins for: ${mismatches.join(', ')}`);
                console.error('(new pins, if the files are trusted: node tools/ubol-convert.js --repin)');
                printPins(version, digests);
                process.exit(1);
            }
            if (args.includes('--repin'))
                printPins(version, digests);
        } else
            printPins(version, digests);
    }

    /* Phase 2: cosmetic filters and scriptlets. */
    const scriptingFiles = {};
    for (const file of Object.values(UBOLScripting.DETAILS)) {
        const data = await fetchFile(version, file, cacheDir, file === UBOLScripting.DETAILS.generic);
        digests[file] = data ? sha256(data) : null;
        if (data)
            scriptingFiles[file] = data.toString();
    }
    const needed = JSON.parse(UBOLScripting.filesNeeded(scriptingFiles[UBOLScripting.DETAILS.rulesets],
        scriptingFiles[UBOLScripting.DETAILS.scriptlets], JSON.stringify(enabled)));
    for (const file of needed) {
        const data = await fetchFile(version, file, cacheDir, false);
        digests[file] = sha256(data);
        scriptingFiles[file] = data.toString();
    }
    checkPins();

    const convertStart = Date.now();
    const result = DNRToWebKit.convert(rulesets);
    const convertMS = Date.now() - convertStart;
    const s = result.stats;
    const scriptingStart = Date.now();
    const scripting = UBOLScripting.convert(scriptingFiles, enabled, { version, exemptTopURLs: EXEMPT_TOP_URLS });
    const scriptingMS = Date.now() - scriptingStart;
    const ss = scripting.stats;
    console.log(`uBO Lite ${version}: rulesets ${enabled.join(', ')}`);
    for (const [id, per] of Object.entries(s.perRuleset))
        console.log(`  ${id.padEnd(24)} ${String(per.input).padStart(6)} DNR rules -> ${String(per.output).padStart(6)} WebKit rules, ${per.skipped} skipped`);
    console.log(`total: ${s.input} DNR rules -> ${s.output} WebKit rules; ${s.skipped} skipped; converted in ${convertMS} ms`);
    console.log(`  redirects as block ${s.redirectsAsBlock}, as allow ${s.redirectsAsAllow}; regex rules expanded ${s.regexExpanded}, widened ${s.regexWidened}; rules with lossy exclusions ${s.lossyExclusions}; domains dropped ${s.domainsDropped}; rules with types dropped ${s.typesDropped}; truncated ${s.truncated}`);
    console.log('  skipped by reason:');
    for (const [reason, count] of Object.entries(s.skippedBy).sort((a, b) => b[1] - a[1]))
        console.log(`    ${String(count).padStart(5)}  ${reason}`);
    console.log(`cosmetic filters and scriptlets (converted in ${scriptingMS} ms): ${ss.genericSelectors} generic selectors as WebKit rules ` +
        `(${ss.genericWithExceptions} with exceptions, ${ss.generichideSites} generichide sites), ${ss.cosmetics} per-site cosmetic filters ` +
        `(${ss.procedurals} procedural, ${ss.cosmeticsDropped} dropped) on ${ss.specificHosts} host entries, scriptlet calls ` +
        `${ss.calls.main} main + ${ss.calls.isolated} isolated on ${ss.scriptletHosts} host entries; library ${ss.librarySplit.main} ` +
        `(${ss.libraryItems.main}) / ${ss.librarySplit.isolated} (${ss.libraryItems.isolated}); site data ${(ss.siteDataBytes / 1048576).toFixed(2)} MB`);
    const allRules = scripting.rules.concat(result.rules);
    if (allRules.length > DNRToWebKit.MAX_RULES)
        throw new Error(`${allRules.length} rules: over WebKit's ${DNRToWebKit.MAX_RULES}`);
    console.log(`list: ${allRules.length} WebKit rules (${scripting.rules.length} cosmetic + ${result.rules.length} network)`);
    if (checkOnly)
        return;

    fs.mkdirSync(OUT, { recursive: true });
    const rulesText = JSON.stringify(allRules);
    /* GKeyFile (ini) for the title: adblock.c reads it with GLib. */
    const info = [
        '[list]',
        'source=uBlock Origin Lite (github.com/uBlockOrigin/uBOL-home, GPL-3.0)',
        `version=${version}`,
        `rulesets=${enabled.join(';')}`,
        `converter=${DNRToWebKit.VERSION * 1000 + UBOLScripting.VERSION}`,
        `rules=${allRules.length}`,
        `dnr-rules=${s.input}`,
        `skipped=${s.skipped}`,
        `generated=${Math.floor(Date.now() / 1000)}`,
        `sha256=${sha256(Buffer.from(rulesText))}`,
        'origin=bundled',
        `scripting=${UBOLScripting.VERSION}`,
        `cosmetic-rules=${scripting.rules.length}`,
        'scripting-file=adblock-scripting.txt',
        `scripting-sha256=${sha256(Buffer.from(scripting.siteData))}`,
        '',
    ].join('\n');
    fs.writeFileSync(path.join(OUT, 'adblock-rules.json'), rulesText);
    fs.writeFileSync(path.join(OUT, 'adblock-scripting.txt'), scripting.siteData);
    fs.writeFileSync(path.join(OUT, 'adblock-info.ini'), info);
    console.log(`wrote ${path.relative(ROOT, OUT)}/adblock-rules.json (${(rulesText.length / 1048576).toFixed(1)} MB), adblock-scripting.txt (${(scripting.siteData.length / 1048576).toFixed(1)} MB) and adblock-info.ini in ${Date.now() - started} ms`);
}

function printPins(version, digests) {
    console.log(`const PINNED_VERSION = '${version}';\nconst PINNED_SHA256 = {`);
    for (const [file, digest] of Object.entries(digests))
        console.log(`    '${file}': ${digest ? `'${digest}'` : 'null'},`);
    console.log('};');
}

main().catch(error => {
    console.error(error.stack || String(error));
    process.exit(1);
});
