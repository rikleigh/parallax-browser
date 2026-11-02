#!/usr/bin/env node
/*
 * ubol-convert.js: uBlock Origin Lite's network rules (declarativeNetRequest)
 * to WebKit content-blocker JSON, for the list bundled with the browser
 * title (port/webkit/ADBLOCK.md). The conversion itself is
 * port/webkit/adblock/dnr-to-webkit.js, the same script the title runs on
 * the console when it updates the lists.
 *
 *   node tools/ubol-convert.js            pinned release -> toolchain/adblock/
 *   node tools/ubol-convert.js --latest   newest release (prints the pins to put below)
 *   node tools/ubol-convert.js --check    convert and print the stats only
 *   node tools/ubol-convert.js --test     self-test of the regex/urlFilter mapping
 *
 * Source: github.com/uBlockOrigin/uBOL-home (GPL-3.0), the unpacked Chromium
 * build at the release tag (chromium/manifest.json lists the rulesets and
 * which are enabled by default; chromium/rulesets/main/<id>.json and
 * chromium/rulesets/regex/<id>.json are the DNR rules). Downloads are kept
 * in toolchain/adblock/cache/<version>/ (never committed).
 */
'use strict';
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const DNRToWebKit = require(path.join(ROOT, 'port/webkit/adblock/dnr-to-webkit.js'));
const OUT = path.join(ROOT, 'toolchain/adblock');
const REPO = 'https://raw.githubusercontent.com/uBlockOrigin/uBOL-home';

/* The pinned release: its version and the sha256 of every file used
 * (manifest first, then main/ and regex/ rulesets; null: 404 at that tag). */
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

    const convertStart = Date.now();
    const result = DNRToWebKit.convert(rulesets);
    const convertMS = Date.now() - convertStart;
    const s = result.stats;
    console.log(`uBO Lite ${version}: rulesets ${enabled.join(', ')}`);
    for (const [id, per] of Object.entries(s.perRuleset))
        console.log(`  ${id.padEnd(24)} ${String(per.input).padStart(6)} DNR rules -> ${String(per.output).padStart(6)} WebKit rules, ${per.skipped} skipped`);
    console.log(`total: ${s.input} DNR rules -> ${s.output} WebKit rules; ${s.skipped} skipped; converted in ${convertMS} ms`);
    console.log(`  redirects as block ${s.redirectsAsBlock}, as allow ${s.redirectsAsAllow}; regex rules expanded ${s.regexExpanded}, widened ${s.regexWidened}; rules with lossy exclusions ${s.lossyExclusions}; domains dropped ${s.domainsDropped}; rules with types dropped ${s.typesDropped}; truncated ${s.truncated}`);
    console.log('  skipped by reason:');
    for (const [reason, count] of Object.entries(s.skippedBy).sort((a, b) => b[1] - a[1]))
        console.log(`    ${String(count).padStart(5)}  ${reason}`);
    if (checkOnly)
        return;

    fs.mkdirSync(OUT, { recursive: true });
    const rulesText = JSON.stringify(result.rules);
    /* GKeyFile (ini) for the title: adblock.c reads it with GLib. */
    const info = [
        '[list]',
        'source=uBlock Origin Lite (github.com/uBlockOrigin/uBOL-home, GPL-3.0)',
        `version=${version}`,
        `rulesets=${enabled.join(';')}`,
        `converter=${DNRToWebKit.VERSION}`,
        `rules=${s.output}`,
        `dnr-rules=${s.input}`,
        `skipped=${s.skipped}`,
        `generated=${Math.floor(Date.now() / 1000)}`,
        `sha256=${sha256(Buffer.from(rulesText))}`,
        'origin=bundled',
        '',
    ].join('\n');
    fs.writeFileSync(path.join(OUT, 'adblock-rules.json'), rulesText);
    fs.writeFileSync(path.join(OUT, 'adblock-info.ini'), info);
    console.log(`wrote ${path.relative(ROOT, OUT)}/adblock-rules.json (${(rulesText.length / 1048576).toFixed(1)} MB) and adblock-info.ini in ${Date.now() - started} ms`);
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
