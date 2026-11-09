/*
 * ubol-scripting.js: uBlock Origin Lite's cosmetic filters and scriptlets
 * (its rulesets/scripting/ data) for the browser (port/webkit/ADBLOCK.md,
 * phase 2). Like dnr-to-webkit.js, one self-contained script with no
 * dependencies, for node on the Mac (tools/ubol-convert.js) and for
 * JavaScriptCore in the console's list-update child (apps/child/
 * adblock-job.c). It only reads uBO Lite's files as data (JSON arrays on
 * known lines, and the scriptlet library split at its top-level function
 * declarations by a small lexer); nothing of them runs here.
 *
 * Two outputs:
 *   - WebKit content-blocker rules for the GENERIC cosmetic filters
 *     (uBO Lite's scripting/generic/<id>.js: lowly and highly generic
 *     selectors, their per-site exceptions, and generic-details.json's
 *     "unhide" sites = $generichide). They go first in the content-blocker
 *     list, so WebKit puts every selector without an exception into one
 *     precompiled style sheet shared by all documents; selectors with
 *     exceptions get unless-top-url; the generichide sites get an
 *     ignore-previous-rules rule placed right after them (it only stops
 *     these cosmetic rules: the network rules come after it).
 *   - The site data for the title (apps/title/adblock-scripting.c): the
 *     per-site cosmetic filters (scripting/specific/<id>.json) and the
 *     scriptlets (scripting/scriptlet/<world>/<id>.js with the library in
 *     scriptlet-begin.js), merged into one host index. The title looks up
 *     the host of each frame about to load and registers, for that host
 *     only, a user style sheet and main-world / isolated-world user scripts
 *     with just the matching filters and scriptlet calls (and the library
 *     functions those calls need), as uBO Lite's content scripts would run
 *     them.
 *
 * Site data format (text, one record per line, fields separated by tabs;
 * in a field \\, \n, \r, \t are escaped):
 *   ubol-scripting <FORMAT> <uBO Lite version> <VERSION>
 *   L <world> <name> <deps, comma separated> <source>   library item (world main|isolated; name * = the whole library, no split)
 *   A <world> <rank> <function name> <arguments as a JSON array>   scriptlet call, numbered from 0 in file order
 *   C <kind> <text>   cosmetic, numbered from 0: kind c = a CSS rule, p = a procedural filter (uBO Lite's JSON)
 *   H <host> <refs>   sorted by host (byte order); host is a name, an entity (name.*), * or an ancestor (name>>)
 *   RA <needle> <regex> <refs>   scriptlets for hosts matching regex (each of the frame's host names)
 *   RC <needle> <regex> <refs>   cosmetics for hosts matching regex (the frame's host only)
 *   P <source>   uBO Lite's css-procedural-api.js (for procedural filters, isolated world)
 * refs: comma separated: a<n> call, a-<n> exception for call n, xm / xi no
 * scriptlets in that world (#@#+js()), c<n> cosmetic, c-<n> exception.
 */
var UBOLScripting = (function () {
    'use strict';

    var VERSION = 2; /* of this converter: part of the list's identity (with dnr-to-webkit.js's) */

    /* A generic selector whose subject (the rightmost compound) is only `*`
     * (pseudo-classes at most) is tested on every element of every page, and
     * with a sibling combinator (`#x ~ *`) WebKit walks every earlier sibling
     * for each element and restyles all later siblings on every insertion:
     * quadratic on long lists and charts (Speedometer's Charts and NewsSite
     * suites were 20-50% slower with one such selector in the global style
     * sheet). Too costly for a style sheet every frame of every page gets;
     * uBO Lite applies these after a DOM survey instead. (2: dropped.) */
    function costlyGeneric(selector) {
        var flat = String(selector).replace(/"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'/g, '""')
            .replace(/\[[^\]]*\]/g, '[a]');
        while (/\([^()]*\)/.test(flat))
            flat = flat.replace(/\([^()]*\)/g, '{}');
        var parts = flat.trim().split(/\s*[\s>+~]\s*/);
        var subject = parts[parts.length - 1];
        return /^\*?(:{1,2}[\w-]+(\{\})?)*$/.test(subject) && subject.length > 0 && subject.charAt(0) === '*';
    }
    var FORMAT = 1;  /* of the site data */
    var WORLDS = ['main', 'isolated'];

    /* uBO Lite's file names (relative to chromium/ in uBOL-home). */
    var DETAILS = {
        rulesets: 'rulesets/ruleset-details.json',
        scriptlets: 'rulesets/scriptlet-details.json',
        generic: 'rulesets/generic-details.json'
    };
    var PROCEDURAL_API = 'js/scripting/css-procedural-api.js';

    function scriptletPath(world, id) { return 'rulesets/scripting/scriptlet/' + world + '/' + id + '.js'; }
    function genericPath(id) { return 'rulesets/scripting/generic/' + id + '.js'; }
    function specificPath(id) { return 'rulesets/scripting/specific/' + id + '.json'; }

    function asArray(value) { return Array.isArray(value) ? value : []; }

    function detailsMap(text) {
        var map = {};
        var list = JSON.parse(text);
        if (!Array.isArray(list))
            throw new Error('not a uBO Lite details file');
        for (var i = 0; i < list.length; i++) {
            var e = list[i];
            if (Array.isArray(e) && typeof e[0] === 'string')
                map[e[0]] = e[1] || {};
            else if (e && typeof e.id === 'string')
                map[e.id] = e;
        }
        return map;
    }

    /* The files this converter reads for the given rulesets, besides the
     * three details files (asked for first). JSON text in, JSON text out. */
    function filesNeeded(rulesetDetailsText, scriptletDetailsText, idsJSON) {
        var ids = typeof idsJSON === 'string' ? JSON.parse(idsJSON) : idsJSON;
        var rulesets = detailsMap(rulesetDetailsText);
        var scriptlets = detailsMap(scriptletDetailsText);
        var out = [];
        var worlds = {};
        for (var i = 0; i < ids.length; i++) {
            var id = ids[i];
            if (!/^[a-z0-9._-]+$/.test(id))
                continue;
            var css = (rulesets[id] || {}).css || {};
            if (css.generic > 0)
                out.push(genericPath(id));
            if (css.specific > 0)
                out.push(specificPath(id));
            for (var w = 0; w < WORLDS.length; w++) {
                if ((scriptlets[id] || {})[WORLDS[w].toUpperCase()]) {
                    out.push(scriptletPath(WORLDS[w], id));
                    worlds[WORLDS[w]] = true;
                }
            }
        }
        for (var v = 0; v < WORLDS.length; v++) {
            if (worlds[WORLDS[v]])
                out.push(scriptletPath(WORLDS[v], 'scriptlet-begin'));
        }
        out.push(PROCEDURAL_API);
        return JSON.stringify(out);
    }

    /* `const NAME = <JSON>;` lines of a uBO Lite generated file (comments
     * like /* 123 *\/ and a new Map(...) wrapper removed). */
    function constants(text) {
        var out = {};
        var lines = text.split('\n');
        var re = /^\s*const ([A-Za-z_$][\w$]*) = (.*);\s*$/;
        for (var i = 0; i < lines.length; i++) {
            var m = re.exec(lines[i]);
            if (!m)
                continue;
            var value = m[2].replace(/^\/\* *\d+ *\*\/ ?/, '');
            if (/^new Map\(/.test(value) && /\)$/.test(value))
                value = value.slice(8, -1).replace(/^\/\* *\d+ *\*\/ ?/, '');
            try {
                out[m[1]] = JSON.parse(value);
            } catch (e) {
                /* not data (code) */
            }
        }
        return out;
    }

    function refList(text) {
        if (typeof text === 'number')
            return [text];
        if (typeof text !== 'string' || !text.length)
            return [];
        return JSON.parse('[' + text + ']');
    }

    /* ---- the scriptlet library: a lexer good enough for brace matching ---- */

    var REGEX_KEYWORDS = { 'return': 1, 'typeof': 1, 'instanceof': 1, 'in': 1, 'of': 1, 'new': 1, 'delete': 1, 'void': 1,
        'throw': 1, 'case': 1, 'do': 1, 'else': 1, 'yield': 1, 'await': 1 };

    function isIdStart(c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c === '_' || c === '$' || c > '\x7f'; }
    function isIdPart(c) { return isIdStart(c) || (c >= '0' && c <= '9'); }

    /* scriptlet-begin.js: its IIFE body split into top-level function and
     * class declarations, each with the other declarations it names.
     * Returns { items: [{ name, source, deps }], whole } or { error, whole }. */
    function splitLibrary(src) {
        var head = /\(function [A-Za-z_$][\w$]*\(\$SECRET_ID\$\) \{\n/.exec(src);
        if (!head)
            return { error: 'no framework IIFE' };
        var bodyStart = head.index + head[0].length;
        var i = bodyStart, n = src.length;
        var stack = []; /* '{', '(', '[', '${' */
        var prev = null; /* { t: 'id'|'num'|'str'|'re'|'p', v } */
        var items = [];
        var item = null; /* the declaration being read */
        var stmtStart = true;
        var others = [];
        var bodyEnd = -1;

        function regexAllowed() {
            if (!prev)
                return true;
            if (prev.t === 'num' || prev.t === 'str' || prev.t === 're')
                return false;
            if (prev.t === 'id')
                return !!REGEX_KEYWORDS[prev.v];
            return prev.v !== ')' && prev.v !== ']' && prev.v !== '}';
        }
        function skipString(q) {
            i++;
            while (i < n && src[i] !== q) {
                if (src[i] === '\\')
                    i++;
                else if (src[i] === '\n')
                    throw new Error('newline in a string at ' + i);
                i++;
            }
            i++;
        }
        /* From after ` or a template's closing }, to after ` or ${. */
        function skipTemplate() {
            while (i < n) {
                var c = src[i];
                if (c === '\\') {
                    i += 2;
                    continue;
                }
                if (c === '`') {
                    i++;
                    return false;
                }
                if (c === '$' && src[i + 1] === '{') {
                    i += 2;
                    return true;
                }
                i++;
            }
            throw new Error('unterminated template');
        }
        function skipRegex() {
            i++;
            var inClass = false;
            while (i < n) {
                var c = src[i];
                if (c === '\\') {
                    i += 2;
                    continue;
                }
                if (c === '\n')
                    throw new Error('newline in a regex at ' + i);
                if (inClass) {
                    if (c === ']')
                        inClass = false;
                } else if (c === '[')
                    inClass = true;
                else if (c === '/')
                    break;
                i++;
            }
            i++;
            while (i < n && isIdPart(src[i]))
                i++;
        }

        try {
            while (i < n) {
                var c = src[i];
                if (c === ' ' || c === '\t' || c === '\n' || c === '\r') {
                    i++;
                    continue;
                }
                if (c === '/' && src[i + 1] === '/') {
                    while (i < n && src[i] !== '\n')
                        i++;
                    continue;
                }
                if (c === '/' && src[i + 1] === '*') {
                    var close = src.indexOf('*/', i + 2);
                    if (close < 0)
                        throw new Error('unterminated comment');
                    i = close + 2;
                    continue;
                }
                var start = i;
                var atTop = stack.length === 0;
                if (atTop && stmtStart && !item) {
                    /* the first token of a top-level statement */
                    var word = /^[A-Za-z_$][\w$]*/.exec(src.slice(i, i + 64));
                    var w = word ? word[0] : c;
                    if (w === 'function' || w === 'class' || w === 'async') {
                        var decl = /^(?:async\s+)?(function\*?|class)\s+([A-Za-z_$][\w$]*)/.exec(src.slice(i, i + 200));
                        if (!decl)
                            throw new Error('unnamed declaration at ' + i);
                        item = { name: decl[2], start: i, ids: {}, opened: false };
                    } else if (c === '}') {
                        bodyEnd = i;
                        break;
                    } else
                        others.push(src.slice(i, src.indexOf('\n', i)));
                }
                stmtStart = false;
                if (c === '"' || c === '\'') {
                    skipString(c);
                    prev = { t: 'str' };
                } else if (c === '`') {
                    i++;
                    if (skipTemplate())
                        stack.push('${');
                    prev = { t: 'str' };
                } else if (isIdStart(c)) {
                    while (i < n && isIdPart(src[i]))
                        i++;
                    var id = src.slice(start, i);
                    if (item && !(prev && prev.t === 'p' && (prev.v === '.' || prev.v === '?.')))
                        item.ids[id] = true;
                    prev = { t: 'id', v: id };
                } else if (c >= '0' && c <= '9') {
                    while (i < n && (isIdPart(src[i]) || src[i] === '.'))
                        i++;
                    prev = { t: 'num' };
                } else if (c === '/' && regexAllowed()) {
                    skipRegex();
                    prev = { t: 're' };
                } else if (c === '{' || c === '(' || c === '[') {
                    if (c === '{' && item && !item.opened && stack.length === 0)
                        item.opened = true;
                    stack.push(c);
                    i++;
                    prev = { t: 'p', v: c };
                } else if (c === '}' || c === ')' || c === ']') {
                    var top = stack.pop();
                    if (top === undefined)
                        throw new Error('unbalanced ' + c + ' at ' + i);
                    if (top === '${') {
                        if (c !== '}')
                            throw new Error('bad template at ' + i);
                        i++;
                        if (skipTemplate())
                            stack.push('${');
                        prev = { t: 'str' };
                        continue;
                    }
                    if ({ '}': '{', ')': '(', ']': '[' }[c] !== top)
                        throw new Error('mismatched ' + c + ' at ' + i);
                    i++;
                    prev = { t: 'p', v: c };
                    if (stack.length === 0) {
                        if (item && item.opened && c === '}') {
                            item.end = i;
                            items.push(item);
                            item = null;
                            stmtStart = true;
                        } else if (c === '}' && !item)
                            stmtStart = true;
                    }
                } else if (c === ';') {
                    i++;
                    prev = { t: 'p', v: ';' };
                    if (stack.length === 0)
                        stmtStart = true;
                } else {
                    var p3 = src.substr(i, 3), p2 = src.substr(i, 2);
                    var op = p3 === '...' || p3 === '===' || p3 === '!==' || p3 === '>>>' || p3 === '**=' ? p3
                        : ['?.', '=>', '==', '!=', '<=', '>=', '&&', '||', '??', '++', '--', '+=', '-=', '*=', '/=', '|=', '&=', '<<', '>>', '**'].indexOf(p2) >= 0 ? p2 : c;
                    if (op === '?.' && src[i + 2] >= '0' && src[i + 2] <= '9')
                        op = '?';
                    i += op.length;
                    prev = { t: 'p', v: op };
                }
            }
        } catch (e) {
            return { error: 'lexer: ' + e.message };
        }
        if (bodyEnd < 0 || item)
            return { error: 'the framework body did not end' };
        var whole = src.slice(bodyStart, bodyEnd);
        /* What else is at the top level must be the framework's own set-up. */
        var known = [/^;/, /^const scriptletGlobals = /, /^if \( typeof \$SCRIPTLET_GLOBALS\$/, /^scriptletGlobals\.(origins|functionRefs) = /];
        for (var o = 0; o < others.length; o++) {
            var ok = false;
            for (var k = 0; k < known.length && !ok; k++)
                ok = known[k].test(others[o]);
            if (!ok)
                return { error: 'unknown top-level code: ' + others[o].slice(0, 60), whole: whole };
        }
        /* The formatting check: each declaration starts a line and its
         * closing brace is alone on a line (as uBO Lite writes them). */
        var names = {};
        for (var j = 0; j < items.length; j++) {
            var it = items[j];
            if (src[it.start - 1] !== '\n' || src[it.end - 1] !== '}' || src[it.end - 2] !== '\n' || (src[it.end] !== '\n' && src[it.end] !== undefined))
                return { error: 'declaration ' + it.name + ' not laid out as expected', whole: whole };
            if (names[it.name])
                return { error: 'declared twice: ' + it.name, whole: whole };
            names[it.name] = true;
        }
        var out = [];
        for (var q = 0; q < items.length; q++) {
            var deps = [];
            for (var name in items[q].ids) {
                if (names[name] && name !== items[q].name)
                    deps.push(name);
            }
            out.push({ name: items[q].name, source: src.slice(items[q].start, items[q].end), deps: deps });
        }
        return { items: out, whole: whole };
    }

    function functionRefs(src) {
        var m = /scriptletGlobals\.functionRefs = (?:\/\* *\d+ *\*\/ ?)?\[([^\]]*)\];/.exec(src);
        if (!m)
            return null;
        return m[1].split(',').map(function (s) { return s.trim(); });
    }

    /* ---- CSS ---- */

    function cssSafe(text) {
        /* One rule per line in the title's style sheet: nothing that could
         * open or close a block or a comment, or escape what follows. */
        return typeof text === 'string' && text.length > 0 && /^[^{}\n\r]*$/.test(text) && text.indexOf('/*') < 0
            && !/\\$/.test(text);
    }

    /* uBO Lite's addDeclaratives (css-procedural-api.js), at conversion time. */
    function cssRuleFromDeclarative(details) {
        var tasks = details.tasks, action = details.action;
        var mq, selector, style;
        if (Array.isArray(tasks)) {
            if (!Array.isArray(tasks[0]) || tasks[0][0] !== 'matches-media')
                return null;
            mq = tasks[0][1];
            if (tasks.length > 2)
                return null;
            if (tasks.length === 2) {
                if (tasks[1][0] !== 'spath')
                    return null;
                selector = tasks[1][1];
            }
        }
        if (Array.isArray(action)) {
            if (action[0] !== 'style')
                return null;
            selector = selector || details.selector;
            style = action[1];
        }
        if (mq === undefined && style === undefined && selector === undefined)
            return null;
        if (selector === undefined)
            selector = details.selector;
        if (!cssSafe(selector) || (style !== undefined && !cssSafe(style)) || (mq !== undefined && !cssSafe(mq)))
            return null;
        if (mq === undefined)
            return selector + '{' + style + '}';
        if (style === undefined)
            return '@media ' + mq + '{' + selector + '{display:none!important}}';
        return '@media ' + mq + '{' + selector + '{' + style + '}}';
    }

    /* A host (or entity) as a WebKit top-URL regex. */
    function topURLRegex(host) {
        var entity = /\.\*$/.test(host);
        var name = entity ? host.slice(0, -2) : host;
        if (!/^[a-z0-9._-]+$/.test(name))
            return null;
        return '^[^:]+://+([^:/]+\\.)?' + name.replace(/\./g, '\\.') + (entity ? '\\.[^:/]+' : '') + '[:/]';
    }

    /* ---- escaping for the site data ---- */

    function esc(text) {
        return String(text).replace(/[\\\n\r\t]/g, function (c) {
            return c === '\\' ? '\\\\' : c === '\n' ? '\\n' : c === '\r' ? '\\r' : '\\t';
        });
    }

    /* ---- the conversion ---- */

    /* files: { '<path>': '<text>' }, ids: the enabled rulesets in order.
     * options.exemptTopURLs: top-URL regexes where generic cosmetics are off
     * (the browser's own pages). Returns { rules, siteData, stats }. */
    function convert(files, ids, options) {
        options = options || {};
        var version = options.version || '';
        var stats = {
            scripting: VERSION, genericSelectors: 0, genericWithExceptions: 0, genericExceptionHostsDropped: 0, generichideSites: 0,
            specificHosts: 0, cosmetics: 0, cosmeticCSS: 0, procedurals: 0, cosmeticsDropped: 0,
            calls: { main: 0, isolated: 0 }, scriptletHosts: 0, libraryItems: { main: 0, isolated: 0 }, librarySplit: {},
            hostKeys: 0, regexes: 0, rules: 0, siteDataBytes: 0
        };
        function need(path) {
            var text = files[path];
            if (typeof text !== 'string')
                throw new Error('missing ' + path);
            return text;
        }
        var rulesetDetails = detailsMap(need(DETAILS.rulesets));
        var scriptletDetails = detailsMap(need(DETAILS.scriptlets));
        var genericDetails = files[DETAILS.generic] ? detailsMap(files[DETAILS.generic]) : {};

        /* -- generic cosmetics -> content-blocker rules -- */
        var genericOrder = [], genericSeen = {}, exceptions = {};
        var unhide = [], unhideSeen = {};
        for (var r = 0; r < ids.length; r++) {
            var id = ids[r];
            var css = (rulesetDetails[id] || {}).css || {};
            if (css.generic > 0) {
                var g = constants(need(genericPath(id)));
                var add = function (list) {
                    for (var s = 0; s < list.length; s++) {
                        var sel = list[s].trim();
                        if (sel && !genericSeen[sel]) {
                            genericSeen[sel] = true;
                            genericOrder.push(sel);
                        }
                    }
                };
                var lowly = asArray(g.lowlyGeneric);
                for (var l = 0; l < lowly.length; l++)
                    add(String(lowly[l][1]).split(',\n'));
                if (typeof g.highlyGeneric === 'string' && g.highlyGeneric)
                    add(g.highlyGeneric.split(',\n'));
                var exc = asArray(g.exceptions), hn = asArray(g.hostnames);
                for (var e = 0; e < exc.length && e < hn.length; e++) {
                    var excepted = String(exc[e]).split('\n');
                    for (var x = 0; x < excepted.length; x++) {
                        var key = excepted[x].trim();
                        if (!key)
                            continue;
                        (exceptions[key] = exceptions[key] || []).push(hn[e]);
                    }
                }
            }
            var gd = genericDetails[id] || {};
            var un = asArray(gd.unhide);
            for (var u = 0; u < un.length; u++) {
                if (!unhideSeen[un[u]]) {
                    unhideSeen[un[u]] = true;
                    unhide.push(un[u]);
                }
            }
        }
        var universal = [], conditional = [];
        for (var gi = 0; gi < genericOrder.length; gi++) {
            var selector = genericOrder[gi];
            if (costlyGeneric(selector)) {
                stats.genericCostlyDropped = (stats.genericCostlyDropped || 0) + 1;
                continue;
            }
            var hosts = exceptions[selector];
            if (!hosts) {
                universal.push({ trigger: { 'url-filter': '.*' }, action: { type: 'css-display-none', selector: selector } });
                continue;
            }
            var regexes = [], seenRe = {};
            for (var h = 0; h < hosts.length; h++) {
                var re = topURLRegex(hosts[h]);
                if (!re) {
                    stats.genericExceptionHostsDropped++;
                    continue;
                }
                if (!seenRe[re]) {
                    seenRe[re] = true;
                    regexes.push(re);
                }
            }
            var trigger = { 'url-filter': '.*', 'resource-type': ['document'] };
            if (regexes.length)
                trigger['unless-top-url'] = regexes;
            conditional.push({ trigger: trigger, action: { type: 'css-display-none', selector: selector } });
            stats.genericWithExceptions++;
        }
        var rules = universal.concat(conditional);
        stats.genericSelectors = rules.length;
        var ignoreTop = (options.exemptTopURLs || []).slice();
        for (var q = 0; q < unhide.length; q++) {
            var ure = topURLRegex(unhide[q]);
            if (ure)
                ignoreTop.push(ure);
        }
        stats.generichideSites = unhide.length;
        if (rules.length && ignoreTop.length)
            rules.push({ trigger: { 'url-filter': '.*', 'if-top-url': ignoreTop }, action: { type: 'ignore-previous-rules' } });
        stats.rules = rules.length;

        /* -- the host index for the title -- */
        var hostRefs = {}; /* key -> [token] */
        function addRef(host, token) {
            var list = hostRefs[host];
            if (!list)
                list = hostRefs[host] = [];
            list.push(token);
        }
        var lines = [];
        lines.push(['ubol-scripting', FORMAT, version, VERSION].join('\t'));
        var regexLines = [];

        /* scriptlets */
        var calls = [], callIndex = {};
        var libraries = {};
        for (var w = 0; w < WORLDS.length; w++) {
            var world = WORLDS[w];
            var bundles = [];
            for (var r2 = 0; r2 < ids.length; r2++) {
                if ((scriptletDetails[ids[r2]] || {})[world.toUpperCase()])
                    bundles.push({ id: ids[r2], rank: r2, text: need(scriptletPath(world, ids[r2])) });
            }
            if (!bundles.length)
                continue;
            var begin = need(scriptletPath(world, 'scriptlet-begin'));
            var refsByIndex = functionRefs(begin);
            if (!refsByIndex)
                throw new Error(world + ' scriptlet-begin.js: no functionRefs');
            var lib = splitLibrary(begin);
            var known = {};
            if (lib.items) {
                for (var li = 0; li < lib.items.length; li++)
                    known[lib.items[li].name] = true;
                for (var fr = 0; fr < refsByIndex.length && !lib.error; fr++) {
                    if (!known[refsByIndex[fr]])
                        lib = { error: 'functionRefs names ' + refsByIndex[fr] + ', not declared', whole: lib.whole };
                }
            }
            if (lib.error && !lib.whole)
                throw new Error(world + ' scriptlet-begin.js: ' + lib.error);
            stats.librarySplit[world] = lib.error ? 'whole library (' + lib.error + ')' : 'split';
            libraries[world] = lib;
            var wc = world === 'main' ? 'm' : 'i';
            for (var b = 0; b < bundles.length; b++) {
                var data = constants(bundles[b].text);
                var hostnames = asArray(data.scriptletHostnames), arglistRefs = asArray(data.arglistRefs);
                var arglists = asArray(data.arglists), args = asArray(data.args);
                var local = {};
                var callFor = function (index) {
                    if (local[index] !== undefined)
                        return local[index];
                    var text = arglists[index];
                    if (index === 0 && text === '')
                        return (local[index] = 'x' + wc);
                    var list = refList(text);
                    var fn = refsByIndex[list[0]];
                    if (!fn)
                        throw new Error(bundles[b].id + ': arglist ' + index + ' names function ' + list[0]);
                    var values = list.slice(1).map(function (a) {
                        if (typeof args[a] !== 'string')
                            throw new Error(bundles[b].id + ': argument ' + a + ' missing');
                        return args[a];
                    });
                    var argsJSON = JSON.stringify(values);
                    var key = world + '\u0001' + fn + '\u0001' + argsJSON;
                    if (callIndex[key] === undefined) {
                        callIndex[key] = calls.length;
                        calls.push({ world: world, rank: bundles[b].rank, fn: fn, args: argsJSON });
                        stats.calls[world]++;
                    }
                    return (local[index] = callIndex[key]);
                };
                var tokensFor = function (refsText) {
                    var refs = refList(refsText), out = [];
                    for (var k = 0; k < refs.length; k++) {
                        var v = refs[k];
                        if (v >= 0) {
                            var c = callFor(v);
                            out.push(typeof c === 'string' ? c : 'a' + c);
                        } else {
                            var target = ~v;
                            if (target === 0 && arglists[0] === '')
                                continue;
                            out.push('a-' + callFor(target));
                        }
                    }
                    return out;
                };
                for (var hi = 0; hi < hostnames.length && hi < arglistRefs.length; hi++) {
                    var tokens = tokensFor(arglistRefs[hi]);
                    for (var t = 0; t < tokens.length; t++)
                        addRef(hostnames[hi], tokens[t]);
                }
                var rx = asArray(data.scriptletFromRegexes);
                for (var ri = 0; ri + 2 < rx.length; ri += 3)
                    regexLines.push(['RA', esc(rx[ri]), esc(rx[ri + 1]), tokensFor(String(rx[ri + 2])).join(',')].join('\t'));
            }
        }

        /* per-site cosmetics */
        var cosmetics = [], cosmeticIndex = {};
        function cosmeticFor(raw) {
            if (cosmeticIndex[raw] !== undefined)
                return cosmeticIndex[raw];
            var record = null;
            if (raw.charAt(0) === '{') {
                var o = null;
                try { o = JSON.parse(raw); } catch (e) { o = null; }
                if (o && o.cssable) {
                    var rule = cssRuleFromDeclarative(o);
                    if (rule)
                        record = { kind: 'c', text: rule };
                } else if (o && typeof o.selector === 'string') {
                    record = { kind: 'p', text: raw };
                    stats.procedurals++;
                }
            } else if (cssSafe(raw))
                record = { kind: 'c', text: raw + '{display:none!important}' };
            if (!record) {
                stats.cosmeticsDropped++;
                return (cosmeticIndex[raw] = -1);
            }
            if (record.kind === 'c')
                stats.cosmeticCSS++;
            cosmeticIndex[raw] = cosmetics.length;
            cosmetics.push(record);
            return cosmetics.length - 1;
        }
        for (var r3 = 0; r3 < ids.length; r3++) {
            var css3 = (rulesetDetails[ids[r3]] || {}).css || {};
            if (!(css3.specific > 0))
                continue;
            var spec = JSON.parse(need(specificPath(ids[r3])));
            var selectors = asArray(spec.selectors), selectorLists = asArray(spec.selectorLists);
            var listRefs = asArray(spec.selectorListRefs), shosts = asArray(spec.hostnames);
            var listTokens = function (listIndex) {
                var refs = refList(selectorLists[listIndex]), out = [];
                for (var k = 0; k < refs.length; k++) {
                    var v = refs[k];
                    var raw = selectors[v >= 0 ? v : ~v];
                    if (typeof raw !== 'string')
                        continue;
                    var c = cosmeticFor(raw);
                    if (c >= 0)
                        out.push(v >= 0 ? 'c' + c : 'c-' + c);
                }
                return out;
            };
            for (var sh = 0; sh < shosts.length && sh < listRefs.length; sh++) {
                var stoks = listTokens(listRefs[sh]);
                for (var st = 0; st < stoks.length; st++)
                    addRef(shosts[sh], stoks[st]);
            }
            var srx = asArray(spec.regexes);
            for (var sr = 0; sr + 2 < srx.length; sr += 3)
                regexLines.push(['RC', esc(srx[sr]), esc(srx[sr + 1]), listTokens(srx[sr + 2]).join(',')].join('\t'));
        }
        stats.cosmetics = cosmetics.length;

        /* -- the site data -- */
        for (var lw = 0; lw < WORLDS.length; lw++) {
            var lb = libraries[WORLDS[lw]];
            if (!lb)
                continue;
            if (lb.error) {
                lines.push(['L', WORLDS[lw], '*', '', esc(lb.whole)].join('\t'));
                stats.libraryItems[WORLDS[lw]] = 1;
                continue;
            }
            for (var it = 0; it < lb.items.length; it++)
                lines.push(['L', WORLDS[lw], lb.items[it].name, lb.items[it].deps.join(','), esc(lb.items[it].source)].join('\t'));
            stats.libraryItems[WORLDS[lw]] = lb.items.length;
        }
        for (var ci = 0; ci < calls.length; ci++)
            lines.push(['A', calls[ci].world, calls[ci].rank, calls[ci].fn, esc(calls[ci].args)].join('\t'));
        for (var co = 0; co < cosmetics.length; co++)
            lines.push(['C', cosmetics[co].kind, esc(cosmetics[co].text)].join('\t'));
        var keys = Object.keys(hostRefs).filter(function (k) { return /^[^\t\n\r\\]+$/.test(k); });
        keys.sort(function (a, b) { return a < b ? -1 : a > b ? 1 : 0; }); /* ASCII host names: byte order */
        var scriptletHosts = 0, specificHosts = 0;
        for (var ki = 0; ki < keys.length; ki++) {
            var refs = hostRefs[keys[ki]];
            var anyA = false, anyC = false;
            for (var rr = 0; rr < refs.length; rr++) {
                if (refs[rr].charAt(0) === 'c')
                    anyC = true;
                else
                    anyA = true;
            }
            scriptletHosts += anyA;
            specificHosts += anyC;
            lines.push(['H', keys[ki], refs.join(',')].join('\t'));
        }
        stats.hostKeys = keys.length;
        stats.scriptletHosts = scriptletHosts;
        stats.specificHosts = specificHosts;
        stats.regexes = regexLines.length;
        for (var rl = 0; rl < regexLines.length; rl++)
            lines.push(regexLines[rl]);
        if (typeof files[PROCEDURAL_API] === 'string' && stats.procedurals)
            lines.push(['P', esc(files[PROCEDURAL_API])].join('\t'));
        var siteData = lines.join('\n') + '\n';
        stats.siteDataBytes = siteData.length;
        return { rules: rules, siteData: siteData, stats: stats };
    }

    /* For the console: files as a JSON object of texts (or an object), ids
     * as JSON. Returns the stats (JSON) on the first line, the WebKit rules
     * (a JSON array, no newline in it) on the second, then the site data. */
    function convertJSON(filesJSON, idsJSON, optionsJSON) {
        var files = typeof filesJSON === 'string' ? JSON.parse(filesJSON) : filesJSON;
        var ids = typeof idsJSON === 'string' ? JSON.parse(idsJSON) : idsJSON;
        var result = convert(files, ids, optionsJSON ? JSON.parse(optionsJSON) : {});
        return JSON.stringify({ stats: result.stats }) + '\n' + JSON.stringify(result.rules) + '\n' + result.siteData;
    }

    return {
        VERSION: VERSION,
        FORMAT: FORMAT,
        DETAILS: DETAILS,
        filesNeeded: filesNeeded,
        convert: convert,
        convertJSON: convertJSON,
        splitLibrary: splitLibrary,
        topURLRegex: topURLRegex
    };
})();

if (typeof module !== 'undefined' && module.exports)
    module.exports = UBOLScripting;
