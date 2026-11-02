/*
 * dnr-to-webkit.js: declarativeNetRequest (DNR) rules, as uBlock Origin Lite
 * ships them, to WebKit content-blocker JSON (port/webkit/ADBLOCK.md).
 *
 * One self-contained script with no dependencies, for two hosts:
 *   - node on the Mac (tools/ubol-convert.js: the list bundled with the
 *     title, tests): require() it.
 *   - JavaScriptCore inside the browser title on the PS5 (adblock.c: the
 *     on-console update): evaluated as a classic script, then
 *     DNRToWebKit.convertJSON(...) is called. No console, no URL, no timers
 *     there: plain ECMAScript only.
 *
 * The mapping mirrors WebKit's own translator for Safari web extensions
 * (Source/WebKit/UIProcess/Extensions/Cocoa/_WKWebExtensionDeclarativeNet
 * RequestRule.mm): urlFilter to a regex, one rule per requestDomain, rules
 * sorted by priority and action (allow before block), allow rules as
 * ignore-following-rules, initiatorDomains as if-frame-url, excluded*
 * through extra ignore-following-rules, allowAllRequests as an
 * if-top-url/if-frame-url rule. Differences, each counted in the stats:
 *   - regexFilter (RE2) is rewritten into WebKit's regex subset (\d, {n,m},
 *     (?:), alternation expanded into several rules) or skipped; WebKit
 *     fails a whole list on one unsupported regex, so everything emitted is
 *     checked against the rules of WebCore's URLFilterParser.
 *   - "redirect" and "modifyHeaders" only act in WebKit for web extensions
 *     (DocumentLoader::allowsActiveContentRuleListActionsForURL): a
 *     redirect to a blank resource (noop.js, 1x1.gif, noop-1s.mp4...)
 *     becomes a block, a redirect to a surrogate script (google-ima.js,
 *     googletagservices_gpt.js...) becomes an allow (the real script loads
 *     rather than the page breaking), query transforms and modifyHeaders
 *     are skipped.
 *   - a trailing ^ also matches the end of the URL, as in DNR.
 *   - domains are checked (lowercase ASCII, as WebKit requires) and request
 *     domains are anchored at the end of the host ([:/?]).
 *   - exclusion rules (ignore-following-rules made from excludedRequestDomains,
 *     excludedRequestMethods, or initiatorDomains with excludedInitiatorDomains)
 *     go last in their priority group, so they only shadow lower priorities.
 */
var DNRToWebKit = (function () {
    'use strict';

    var VERSION = 1; /* of this converter: part of the compiled list's identity */
    var MAX_RULES = 150000; /* WebCore's ContentExtensionParser limit per list */
    var MAX_EXPANSIONS = 64; /* regex alternation: rules made from one */

    var RESOURCE_TYPES = {
        font: 'font', image: 'image', main_frame: 'top-document', media: 'media', other: 'other',
        ping: 'ping', script: 'script', stylesheet: 'style-sheet', sub_frame: 'child-document',
        websocket: 'websocket', xmlhttprequest: 'fetch'
    };
    /* DNR types WebKit has no equivalent for (dropped from a rule's list). */
    var UNSUPPORTED_TYPES = { object: 1, csp_report: 1, webtransport: 1, webbundle: 1 };
    var ALL_TYPES = Object.keys(RESOURCE_TYPES);
    var DEFAULT_TYPES = ALL_TYPES.filter(function (t) { return t !== 'main_frame'; });

    /* Within one priority, as Chrome orders them (_WKWebExtension...Rule compare). */
    var ACTION_RANK = { allow: 5, allowAllRequests: 4, block: 3, upgradeScheme: 2, redirect: 1, modifyHeaders: 0 };

    var KNOWN_CONDITION_KEYS = {
        urlFilter: 1, regexFilter: 1, isUrlFilterCaseSensitive: 1, requestDomains: 1, excludedRequestDomains: 1,
        initiatorDomains: 1, excludedInitiatorDomains: 1, domains: 1, excludedDomains: 1, topDomains: 1,
        excludedTopDomains: 1, resourceTypes: 1, excludedResourceTypes: 1, requestMethods: 1,
        excludedRequestMethods: 1, domainType: 1
    };
    var REQUEST_METHODS = { connect: 1, 'delete': 1, get: 1, head: 1, options: 1, patch: 1, post: 1, put: 1 };

    /* uBO's web_accessible_resources that stand for nothing: blocking is the
     * closest WebKit can do. Any other redirect target is a surrogate. */
    var BLANK_RESOURCES = /\/(noop[^\/]*|1x1\.gif|2x2\.png|3x2\.png|32x32\.png|empty|click2load\.html)$/;

    var DOMAIN_ANCHOR = '^[^:]+://+([^:/]+\\.)?';
    var SEPARATOR = '[^a-zA-Z0-9_.%-]';

    function SkipRule(reason) { this.reason = reason; }

    function escapeRegex(text, specials) {
        var out = '';
        for (var i = 0; i < text.length; i++) {
            var c = text.charAt(i);
            out += specials.indexOf(c) >= 0 ? '\\' + c : c;
        }
        return out;
    }

    function isASCII(text) {
        for (var i = 0; i < text.length; i++) {
            var code = text.charCodeAt(i);
            if (code < 0x21 || code > 0x7e)
                return false;
        }
        return true;
    }

    /* A domain as WebKit takes it (lowercase ASCII, no wildcards), or null. */
    function cleanDomain(domain) {
        if (typeof domain !== 'string' || !domain.length || domain.length > 253)
            return null;
        for (var i = 0; i < domain.length; i++) {
            var c = domain.charAt(i);
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c === '.' || c === '-' || c === '_'))
                return null;
        }
        return domain;
    }

    /* Included domain lists lose bad entries (null if none are left);
     * excluded lists must be whole or the rule would apply too widely. */
    function domainList(list, excluded, stats) {
        if (list === undefined)
            return undefined;
        if (!Array.isArray(list) || !list.length)
            throw new SkipRule('empty domain list');
        var out = [];
        for (var i = 0; i < list.length; i++) {
            var d = cleanDomain(list[i]);
            if (d)
                out.push(d);
            else if (excluded)
                throw new SkipRule('non-ASCII or invalid excluded domain');
            else
                stats.domainsDropped++;
        }
        if (!out.length)
            throw new SkipRule('no valid domain left');
        return out;
    }

    /* ---- urlFilter (DNR's adblock-like syntax) to a WebKit regex ---- */

    function urlFilterToRegex(filter) {
        if (!filter)
            return null;
        if (!isASCII(filter))
            throw new SkipRule('non-ASCII urlFilter');
        var domainAnchor = filter.slice(0, 2) === '||';
        if (domainAnchor)
            filter = filter.slice(2);
        var startAnchor = !domainAnchor && filter.charAt(0) === '|';
        if (startAnchor)
            filter = filter.slice(1);
        var endAnchor = filter.length > 0 && filter.charAt(filter.length - 1) === '|';
        if (endAnchor)
            filter = filter.slice(0, -1);
        /* A trailing ^ is a separator or the end of the URL. */
        var trailingSeparator = filter.length > 0 && filter.charAt(filter.length - 1) === '^';
        if (trailingSeparator)
            filter = filter.slice(0, -1);
        var regex = '';
        for (var i = 0; i < filter.length; i++) {
            var c = filter.charAt(i);
            if (c === '*')
                regex += '.*';
            else if (c === '^')
                regex += SEPARATOR;
            else if ('?+[](){}$|\\.'.indexOf(c) >= 0)
                regex += '\\' + c;
            else
                regex += c;
        }
        if (trailingSeparator)
            regex += endAnchor ? '(' + SEPARATOR + ')?$' : '(' + SEPARATOR + '.*)?$';
        else if (endAnchor)
            regex += '$';
        if (domainAnchor)
            regex = DOMAIN_ANCHOR + regex;
        else if (startAnchor)
            regex = '^' + regex;
        if (!regex.length)
            return null;
        return regex;
    }

    /* requestDomain combined with the urlFilter, as WebKit's translator does
     * (_combineRequestDomain:withURLFilter:), but with the host's end
     * anchored when there is no filter. Returns a regex. */
    function longestCommonSubstring(a, b) {
        var best = '';
        for (var i = 0; i < a.length; i++) {
            for (var j = 0; j < b.length; j++) {
                var k = 0;
                while (i + k < a.length && j + k < b.length && a.charAt(i + k) === b.charAt(j + k))
                    k++;
                if (k > best.length)
                    best = a.substr(i, k);
            }
        }
        return best;
    }

    function requestDomainRegex(domain, urlFilter) {
        if (!urlFilter)
            return DOMAIN_ANCHOR + escapeRegex(domain, '.') + '[:/?]';
        var trimmedFilter = urlFilter.replace(/^\.+/, '');
        if (trimmedFilter.indexOf(domain) === 0)
            return urlFilterToRegex(urlFilter.slice(0, 2) === '||' ? urlFilter : '||' + urlFilter);
        var common = longestCommonSubstring(domain, urlFilter);
        if (common.length > 1) {
            var modified = urlFilter.slice(0, 2) === '||' ? urlFilter : '||' + urlFilter;
            var at = modified.indexOf(common);
            return urlFilterToRegex(modified.slice(0, at) + domain + modified.slice(at + common.length));
        }
        if (urlFilter.slice(0, 2) === '||' || urlFilter.slice(0, 3) === '://') {
            if (urlFilter.replace('://', '').indexOf('/') >= 0) {
                var parts = urlFilter.slice(3).split('/');
                parts[0] = parts[0].charAt(parts[0].length - 1) === '*' ? parts[0] + domain : parts[0] + '*' + domain;
                return urlFilterToRegex('://' + parts.join('/'));
            }
            return urlFilterToRegex(urlFilter.charAt(urlFilter.length - 1) === '*' ? urlFilter + domain : urlFilter + '*' + domain);
        }
        return urlFilterToRegex('||' + domain + '*' + urlFilter);
    }

    /* ---- regexFilter (RE2) to WebKit's regex subset ----
     * WebCore's URLFilterParser takes: ASCII literals and escapes, '.',
     * character classes without \d-style classes, groups, the quantifiers
     * ? * + (lazy or not), '^' only first and '$' only last (outside
     * groups). No alternation, {n,m}, \b, \d, lookarounds or backreferences.
     * Alternation is expanded into several patterns, {n,m} unrolled, \d and
     * \w spelled out; anything else skips the rule. */

    function RegexTranslator(source) {
        this.s = source;
        this.i = 0;
    }

    RegexTranslator.prototype.fail = function (what) {
        throw new SkipRule('regex: ' + what);
    };

    RegexTranslator.prototype.peek = function () {
        return this.i < this.s.length ? this.s.charAt(this.i) : '';
    };

    function cartesian(left, right) {
        if (left.length * right.length > MAX_EXPANSIONS)
            throw new SkipRule('regex: too many alternatives');
        var out = [];
        for (var a = 0; a < left.length; a++) {
            for (var b = 0; b < right.length; b++)
                out.push(left[a] + right[b]);
        }
        return out;
    }

    /* alternatives: list of strings (each a sequence in WebKit syntax). */
    RegexTranslator.prototype.parseAlternation = function () {
        var out = this.parseSequence();
        while (this.peek() === '|') {
            this.i++;
            out = out.concat(this.parseSequence());
            if (out.length > MAX_EXPANSIONS)
                this.fail('too many alternatives');
        }
        return out;
    };

    RegexTranslator.prototype.parseSequence = function () {
        var seq = [''];
        for (;;) {
            var c = this.peek();
            if (c === '' || c === '|' || c === ')')
                return seq;
            var atom = this.parseAtom(); /* { alts: [...], quantifiable: bool, single: bool } */
            atom = this.parseQuantifier(atom);
            seq = cartesian(seq, atom);
        }
    };

    function hexValue(text) {
        var v = parseInt(text, 16);
        return isNaN(v) ? -1 : v;
    }

    /* One character as a literal in WebKit syntax (escaped if special). */
    function literal(c) {
        return '\\^$.|?*+()[]{}/'.indexOf(c) >= 0 ? '\\' + c : c;
    }

    function classLiteral(c) {
        return '\\]^-['.indexOf(c) >= 0 ? '\\' + c : c;
    }

    RegexTranslator.prototype.parseEscape = function (inClass) {
        this.i++; /* the backslash */
        var c = this.peek();
        if (c === '')
            this.fail('trailing backslash');
        this.i++;
        if (c === 'd')
            return { cls: '0-9' };
        if (c === 'w')
            return { cls: 'a-zA-Z0-9_' };
        if (c === 'x') {
            var v = hexValue(this.s.substr(this.i, 2));
            if (v < 0x21 || v > 0x7e || !/^[0-9a-fA-F]{2}$/.test(this.s.substr(this.i, 2)))
                this.fail('\\x escape');
            this.i += 2;
            return { ch: String.fromCharCode(v) };
        }
        if (/[a-zA-Z0-9]/.test(c))
            this.fail('\\' + c + (inClass ? ' in a class' : ''));
        if (c.charCodeAt(0) < 0x21 || c.charCodeAt(0) > 0x7e)
            this.fail('non-ASCII');
        return { ch: c };
    };

    RegexTranslator.prototype.parseClass = function () {
        this.i++; /* [ */
        var out = '[';
        if (this.peek() === '^') {
            out += '^';
            this.i++;
        }
        var count = 0;
        for (;;) {
            var c = this.peek();
            if (c === '')
                this.fail('unterminated class');
            if (c === ']' && count)
                break;
            if (c === ']' || (c === '[' && this.s.charAt(this.i + 1) === ':'))
                this.fail('class syntax');
            var item;
            if (c === '\\')
                item = this.parseEscape(true);
            else {
                if (c.charCodeAt(0) < 0x21 || c.charCodeAt(0) > 0x7e)
                    this.fail('non-ASCII');
                this.i++;
                item = { ch: c };
            }
            count++;
            if (item.cls) {
                out += item.cls;
                continue;
            }
            /* a range? */
            if (this.peek() === '-' && this.s.charAt(this.i + 1) !== ']' && this.s.charAt(this.i + 1) !== '') {
                this.i++;
                var end;
                if (this.peek() === '\\')
                    end = this.parseEscape(true);
                else {
                    end = { ch: this.peek() };
                    this.i++;
                }
                if (end.cls || !end.ch || end.ch.charCodeAt(0) < item.ch.charCodeAt(0) || end.ch.charCodeAt(0) > 0x7e)
                    this.fail('class range');
                out += classLiteral(item.ch) + '-' + classLiteral(end.ch);
            } else
                out += classLiteral(item.ch);
        }
        this.i++; /* ] */
        return out + ']';
    };

    RegexTranslator.prototype.parseAtom = function () {
        var c = this.peek();
        if (c === '(') {
            this.i++;
            if (this.peek() === '?') {
                if (this.s.charAt(this.i + 1) !== ':')
                    this.fail('lookaround or named group');
                this.i += 2;
            }
            var inner = this.parseAlternation();
            if (this.peek() !== ')')
                this.fail('unbalanced parenthesis');
            this.i++;
            return { alts: inner, group: true };
        }
        if (c === '[')
            return { alts: [this.parseClass()], unit: true };
        if (c === '\\') {
            var e = this.parseEscape(false);
            return { alts: [e.cls ? '[' + e.cls + ']' : literal(e.ch)], unit: true };
        }
        if (c === '.')
            return this.i++, { alts: ['.'], unit: true };
        if (c === '^')
            return this.i++, { alts: ['^'], anchor: true };
        if (c === '$')
            return this.i++, { alts: ['$'], anchor: true };
        if ('*+?{'.indexOf(c) >= 0)
            this.fail('quantifier without an atom');
        if (c === ']' || c === '}')
            return this.i++, { alts: [literal(c)], unit: true };
        if (c.charCodeAt(0) < 0x21 || c.charCodeAt(0) > 0x7e)
            this.fail('non-ASCII or space');
        this.i++;
        return { alts: [literal(c)], unit: true };
    };

    /* Applies a quantifier to an atom; returns its alternatives. */
    RegexTranslator.prototype.parseQuantifier = function (atom) {
        var c = this.peek();
        var min, max;
        if (c === '*' || c === '+' || c === '?') {
            this.i++;
            min = c === '+' ? 1 : 0;
            max = c === '?' ? 1 : Infinity;
        } else if (c === '{') {
            var m = /^\{(\d+)(,(\d*))?\}/.exec(this.s.slice(this.i));
            if (!m)
                this.fail('brace');
            this.i += m[0].length;
            min = parseInt(m[1], 10);
            max = m[2] === undefined ? min : m[3] === '' ? Infinity : parseInt(m[3], 10);
            if (max < min || min > 64)
                this.fail('repetition count');
            if (max !== Infinity && max - min > 16) {
                max = Infinity; /* {8,100}: at least 8 (a few more URLs match) */
                this.widened = (this.widened || 0) + 1;
            }
        } else {
            /* no quantifier: a group's alternatives inline (so ^ and $
             * inside "(^|/)" end up first or last, which WebKit takes) */
            return atom.alts;
        }
        if (this.peek() === '?')
            this.i++; /* lazy: the same set of URLs */
        if (atom.anchor)
            this.fail('quantified anchor');
        var unit;
        if (atom.alts.length > 1) {
            if (max !== 1 || min !== 0)
                this.fail('quantified alternation');
            return atom.alts.concat(['']);
        }
        unit = atom.group ? '(' + atom.alts[0] + ')' : atom.alts[0];
        if (atom.group && /(^|[^\\])[\^$]/.test(atom.alts[0].replace(/\[[^\]]*\]/g, '')))
            this.fail('anchor in a quantified group');
        if (atom.group && atom.alts[0] === '')
            return [''];
        var out = '';
        for (var k = 0; k < min; k++)
            out += unit;
        if (max === Infinity)
            out += unit + '*';
        else
            for (var j = min; j < max; j++)
                out += unit + '?';
        if (min === 1 && max === Infinity)
            out = unit + '+';
        return [out];
    };

    /* Scans a WebKit pattern for anchors where URLFilterParser refuses them. */
    function checkAnchors(pattern) {
        var depth = 0, inClass = false;
        for (var i = 0; i < pattern.length; i++) {
            var c = pattern.charAt(i);
            if (c === '\\') {
                i++;
                continue;
            }
            if (inClass) {
                if (c === ']')
                    inClass = false;
                continue;
            }
            if (c === '[')
                inClass = true;
            else if (c === '(')
                depth++;
            else if (c === ')')
                depth--;
            else if (c === '^' && (i || depth))
                return false;
            else if (c === '$' && (i !== pattern.length - 1 || depth))
                return false;
        }
        return !inClass && !depth;
    }

    function translateRegex(source, stats) {
        if (typeof source !== 'string' || !source.length)
            throw new SkipRule('empty regexFilter');
        if (!isASCII(source))
            throw new SkipRule('regex: non-ASCII');
        var t = new RegexTranslator(source);
        var alts = t.parseAlternation();
        if (t.i !== source.length)
            t.fail('unbalanced parenthesis');
        if (t.widened && stats)
            stats.regexWidened++;
        var out = [];
        for (var i = 0; i < alts.length; i++) {
            var p = alts[i];
            if (!p.length)
                p = '.*'; /* an empty alternative matches every URL */
            if (!checkAnchors(p))
                t.fail('misplaced anchor');
            if (out.indexOf(p) < 0)
                out.push(p);
        }
        return out;
    }

    /* ---- rules ---- */

    function resourceTypesFor(condition, stats) {
        var included = condition.resourceTypes, excluded = condition.excludedResourceTypes;
        var types;
        if (included) {
            types = [];
            for (var i = 0; i < included.length; i++) {
                if (RESOURCE_TYPES[included[i]]) {
                    if (types.indexOf(included[i]) < 0)
                        types.push(included[i]);
                } else if (!UNSUPPORTED_TYPES[included[i]])
                    throw new SkipRule('unknown resource type ' + included[i]);
            }
            if (!types.length)
                throw new SkipRule('only resource types WebKit lacks (object, csp_report...)');
            if (types.length < included.length)
                stats.typesDropped++;
        } else if (excluded) {
            types = ALL_TYPES.filter(function (t) { return excluded.indexOf(t) < 0; });
            if (!types.length)
                throw new SkipRule('every resource type excluded');
        } else
            types = DEFAULT_TYPES;
        return types.map(function (t) { return RESOURCE_TYPES[t]; });
    }

    function frameURLRegex(domain) {
        return DOMAIN_ANCHOR + escapeRegex(domain, '.') + '[:/]';
    }

    function wildcardDomain(domain) {
        return '*' + domain;
    }

    /* The WebKit rules for one DNR rule, with sort keys. */
    function convertRule(rule, rulesetID, order, stats) {
        if (!rule || typeof rule !== 'object' || !rule.action || !rule.condition)
            throw new SkipRule('malformed rule');
        var condition = rule.condition, action = rule.action;
        for (var key in condition) {
            if (!KNOWN_CONDITION_KEYS[key])
                throw new SkipRule('condition ' + key);
        }
        var dnrType = action.type;
        var type; /* allow | allowAllRequests | block | upgradeScheme */
        if (dnrType === 'block' || dnrType === 'allow' || dnrType === 'allowAllRequests' || dnrType === 'upgradeScheme')
            type = dnrType;
        else if (dnrType === 'redirect') {
            var target = action.redirect || {};
            if (target.extensionPath) {
                if (BLANK_RESOURCES.test(target.extensionPath)) {
                    type = 'block';
                    stats.redirectsAsBlock++;
                } else {
                    type = 'allow';
                    stats.redirectsAsAllow++;
                }
            } else
                throw new SkipRule('redirect without extensionPath (URL transform)');
        } else if (dnrType === 'modifyHeaders')
            throw new SkipRule('modifyHeaders (web extensions only in WebKit)');
        else
            throw new SkipRule('action ' + dnrType);

        var priority = typeof rule.priority === 'number' ? rule.priority : 1;
        var rank = ACTION_RANK[type];
        var webkitAction = type === 'block' ? 'block' : type === 'upgradeScheme' ? 'make-https' : 'ignore-following-rules';

        var requestDomains = domainList(condition.requestDomains, false, stats);
        var excludedRequestDomains = domainList(condition.excludedRequestDomains, true, stats);
        var initiatorDomains = domainList(condition.initiatorDomains || condition.domains, false, stats);
        var excludedInitiatorDomains = domainList(condition.excludedInitiatorDomains || condition.excludedDomains, true, stats);
        var topDomains = domainList(condition.topDomains, false, stats);
        var excludedTopDomains = domainList(condition.excludedTopDomains, true, stats);
        if ((topDomains || excludedTopDomains) && (initiatorDomains || excludedInitiatorDomains || (topDomains && excludedTopDomains)))
            throw new SkipRule('two page conditions (WebKit takes one)');

        var methods = condition.requestMethods, excludedMethods = condition.excludedRequestMethods;
        [methods, excludedMethods].forEach(function (list) {
            if (list && (!Array.isArray(list) || !list.length || list.some(function (m) { return !REQUEST_METHODS[m]; })))
                throw new SkipRule('request method');
        });

        var types = resourceTypesFor(condition, stats);
        var caseSensitive = condition.isUrlFilterCaseSensitive === true;

        /* The url-filter patterns this rule matches (several: one per request
         * domain or regex alternative). */
        var patterns;
        if (condition.regexFilter !== undefined) {
            if (condition.urlFilter !== undefined)
                throw new SkipRule('urlFilter and regexFilter');
            patterns = translateRegex(condition.regexFilter, stats);
            if (patterns.length > 1)
                stats.regexExpanded++;
            /* WebKit's translator ignores requestDomains next to a regex;
             * DNR applies both. Keep the rule only without them. */
            if (requestDomains)
                throw new SkipRule('regexFilter with requestDomains');
        } else if (requestDomains) {
            if (condition.urlFilter !== undefined && !isASCII(condition.urlFilter))
                throw new SkipRule('non-ASCII urlFilter');
            patterns = requestDomains.map(function (d) { return requestDomainRegex(d, condition.urlFilter); });
        } else
            patterns = [urlFilterToRegex(condition.urlFilter) || '.*'];

        var out = [];
        function emit(trigger, actionType, exclusion) {
            out.push({ priority: priority, rank: rank, exclusion: exclusion, order: order, rule: { trigger: trigger, action: { type: actionType } } });
        }
        function baseTrigger(pattern) {
            var t = { 'url-filter': pattern };
            if (caseSensitive && pattern !== '.*')
                t['url-filter-is-case-sensitive'] = true;
            t['resource-type'] = types;
            if (condition.domainType === 'firstParty')
                t['load-type'] = ['first-party'];
            else if (condition.domainType === 'thirdParty')
                t['load-type'] = ['third-party'];
            else if (condition.domainType !== undefined)
                throw new SkipRule('domainType ' + condition.domainType);
            return t;
        }
        function withPageCondition(t, includeList, excludeList) {
            if (includeList)
                t['if-frame-url'] = includeList.map(frameURLRegex);
            else if (excludeList)
                t['unless-frame-url'] = excludeList.map(frameURLRegex);
            else if (topDomains)
                t['if-domain'] = topDomains.map(wildcardDomain);
            else if (excludedTopDomains)
                t['unless-domain'] = excludedTopDomains.map(wildcardDomain);
            return t;
        }

        if (type === 'allowAllRequests') {
            /* A frame and everything it loads (WebKit's translator). */
            var frameTypes = condition.resourceTypes || [];
            for (var a = 0; a < patterns.length; a++) {
                var t = { 'url-filter': '.*' };
                if (frameTypes.indexOf('main_frame') >= 0)
                    t['if-top-url'] = [patterns[a]];
                else {
                    t['if-frame-url'] = [patterns[a]];
                    t['load-context'] = ['child-frame'];
                }
                emit(t, 'ignore-following-rules', false);
            }
            return out;
        }

        var methodList = methods || [null];
        for (var p = 0; p < patterns.length; p++) {
            for (var m = 0; m < methodList.length; m++) {
                var exclusions = [];
                /* excludedRequestDomains / excludedRequestMethods: rules that
                 * stop the ones after them (lossy: see the header). */
                if (excludedRequestDomains) {
                    for (var e = 0; e < excludedRequestDomains.length; e++) {
                        var et = baseTrigger(requestDomainRegex(excludedRequestDomains[e], null));
                        delete et['url-filter-is-case-sensitive'];
                        exclusions.push(withPageCondition(et, initiatorDomains, initiatorDomains ? null : excludedInitiatorDomains));
                    }
                }
                if (excludedMethods) {
                    for (var x = 0; x < excludedMethods.length; x++) {
                        var mt = withPageCondition(baseTrigger(patterns[p]), initiatorDomains, initiatorDomains ? null : excludedInitiatorDomains);
                        mt['request-method'] = excludedMethods[x];
                        exclusions.push(mt);
                    }
                }
                if (initiatorDomains && excludedInitiatorDomains) {
                    var it = baseTrigger(patterns[p]);
                    if (methodList[m])
                        it['request-method'] = methodList[m];
                    it['if-frame-url'] = excludedInitiatorDomains.map(frameURLRegex);
                    exclusions.push(it);
                }
                var main = withPageCondition(baseTrigger(patterns[p]), initiatorDomains, excludedInitiatorDomains);
                if (methodList[m])
                    main['request-method'] = methodList[m];
                var exclusion = exclusions.length > 0;
                if (exclusion)
                    stats.lossyExclusions++;
                for (var k = 0; k < exclusions.length; k++)
                    emit(exclusions[k], 'ignore-following-rules', true);
                emit(main, webkitAction, exclusion);
                if (webkitAction === 'make-https')
                    emit(main, 'ignore-following-rules', exclusion);
            }
        }
        return out;
    }

    function newStats() {
        return {
            converter: VERSION, input: 0, output: 0, skipped: 0, skippedBy: {}, perRuleset: {},
            redirectsAsBlock: 0, redirectsAsAllow: 0, regexExpanded: 0, regexWidened: 0, lossyExclusions: 0,
            domainsDropped: 0, typesDropped: 0, truncated: 0
        };
    }

    /* rulesets: [{ id, rules: [DNR rule...] }, ...]. Returns { rules, stats }.
     * options.maxRules: cap (default 150000; the lowest priorities go). */
    function convert(rulesets, options) {
        options = options || {};
        var stats = newStats();
        var all = [];
        var order = 0;
        for (var r = 0; r < rulesets.length; r++) {
            var set = rulesets[r];
            var per = stats.perRuleset[set.id] = { input: 0, output: 0, skipped: 0 };
            var rules = set.rules || [];
            for (var i = 0; i < rules.length; i++) {
                per.input++;
                stats.input++;
                try {
                    var converted = convertRule(rules[i], set.id, order++, stats);
                    for (var c = 0; c < converted.length; c++)
                        all.push(converted[c]);
                    per.output += converted.length;
                } catch (error) {
                    if (!(error instanceof SkipRule))
                        throw error;
                    per.skipped++;
                    stats.skipped++;
                    stats.skippedBy[error.reason] = (stats.skippedBy[error.reason] || 0) + 1;
                }
            }
        }
        /* Highest priority first; then allow before block; exclusions last
         * in their group; then as declared (Array.prototype.sort is stable). */
        all.sort(function (a, b) {
            if (a.priority !== b.priority)
                return b.priority - a.priority;
            if (a.exclusion !== b.exclusion)
                return a.exclusion ? 1 : -1;
            if (a.rank !== b.rank)
                return b.rank - a.rank;
            return a.order - b.order;
        });
        var max = options.maxRules || MAX_RULES;
        if (all.length > max) {
            stats.truncated = all.length - max;
            all.length = max;
        }
        var out = new Array(all.length);
        for (var j = 0; j < all.length; j++)
            out[j] = all[j].rule;
        stats.output = out.length;
        return { rules: out, stats: stats };
    }

    /* For the console: JSON texts in, JSON text out (fewer objects cross
     * the C/JS boundary). rulesetsJSON: {"<id>": "<DNR JSON text>", ...}, as
     * text or as an object. Returns one text: the stats as JSON on the first
     * line, then the WebKit rules (a JSON array). */
    function convertJSON(rulesetsJSON, optionsJSON) {
        var input = typeof rulesetsJSON === 'string' ? JSON.parse(rulesetsJSON) : rulesetsJSON;
        var sets = [];
        for (var id in input)
            sets.push({ id: id, rules: typeof input[id] === 'string' ? JSON.parse(input[id]) : input[id] });
        input = null;
        var result = convert(sets, optionsJSON ? JSON.parse(optionsJSON) : {});
        sets = null;
        var rulesText = JSON.stringify(result.rules);
        result.rules = null;
        return JSON.stringify({ stats: result.stats }) + '\n' + rulesText;
    }

    /* uBO Lite's manifest.json: its version and the rulesets enabled by
     * default (the network rules: rulesets/main/<id>.json and, if the
     * release has one, rulesets/regex/<id>.json). As JSON text. */
    function rulesetsFromManifest(manifestText) {
        var manifest = JSON.parse(manifestText);
        var resources = (manifest.declarative_net_request || {}).rule_resources || [];
        var ids = [];
        for (var i = 0; i < resources.length; i++) {
            if (resources[i].enabled && /^[a-z0-9._-]+$/.test(resources[i].id))
                ids.push(resources[i].id);
        }
        if (typeof manifest.version !== 'string' || !/^[0-9.]+$/.test(manifest.version) || !ids.length)
            throw new Error('not a uBO Lite manifest');
        return JSON.stringify({ version: manifest.version, rulesets: ids });
    }

    return {
        VERSION: VERSION,
        rulesetsFromManifest: rulesetsFromManifest,
        MAX_RULES: MAX_RULES,
        convert: convert,
        convertJSON: convertJSON,
        translateRegex: function (source) {
            try {
                return translateRegex(source);
            } catch (error) {
                if (error instanceof SkipRule)
                    return null;
                throw error;
            }
        },
        urlFilterToRegex: urlFilterToRegex,
        checkAnchors: checkAnchors
    };
})();

if (typeof module !== 'undefined' && module.exports)
    module.exports = DNRToWebKit;
