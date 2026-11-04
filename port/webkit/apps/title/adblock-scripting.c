/*
 * adblock-scripting.c: uBlock Origin Lite's per-site cosmetic filters and
 * scriptlets (port/webkit/ADBLOCK.md, phase 2; adblock-scripting.h).
 *
 * The site data (adblock-scripting.txt, made by ubol-scripting.js from uBO
 * Lite's rulesets/scripting/ files) is read once into one buffer; the index
 * points into it. A lookup for a frame host follows uBO Lite's own content
 * scripts (scriptlet bundles, css-specific.js):
 *   - the host's names: the host, each parent domain, "*", then the
 *     entities (name.*) of each, as isolated-api.js / scriptlet-begin.js
 *     compute them; for a subframe also the top-level host's names with
 *     ">>" (uBO Lite checks every ancestor; here the top-level page);
 *     the regex entries last;
 *   - scriptlet calls in ruleset order, minus exceptions (#@#+js(...)); no
 *     calls in a world with a #@#+js() exception;
 *   - cosmetic filters minus exceptions (#@#), across rulesets.
 * The scripts contain the scriptlet library's declarations the calls need
 * (and what those name), not the whole library, then the calls, each in a
 * try block as uBO Lite runs them. Procedural filters run uBO Lite's
 * css-procedural-api.js in the isolated world.
 */
#include "adblock-scripting.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char* name;
    const char* deps;
    const char* source;
} LibItem;

typedef struct {
    guint8 world;
    guint8 rank;
    const char* fn;
    const char* args;
} Call;

typedef struct {
    char kind; /* c: CSS rule, p: procedural (JSON) */
    const char* text;
} Cosmetic;

typedef struct {
    const char* key;
    const char* refs;
} Host;

typedef struct {
    gboolean cosmetic; /* RC (the host only) or RA (each name) */
    const char* needle;
    GRegex* regex;
    const char* refs;
} HostRegex;

struct AbSiteData {
    char* buffer;
    gsize size;
    char* version;
    int converter;
    GArray* lib[2];          /* LibItem */
    GHashTable* libIndex[2]; /* name -> index + 1 */
    gboolean whole[2];       /* the library came unsplit (one item "*") */
    GArray* calls;           /* Call */
    GArray* cosmetics;       /* Cosmetic */
    GArray* hosts;           /* Host, sorted */
    GArray* regexes;         /* HostRegex */
    const char* procedural;  /* css-procedural-api.js */
    GArray* genericCalls[2]; /* guint, sorted: the "*" calls per world */
    guint procedurals;
};

static const char* worldNames[2] = { "main", "isolated" };

/* In place: \\ \n \r \t. */
static char* unescapeField(char* s)
{
    char* out = s;
    for (char* in = s; *in; in++) {
        if (*in == '\\' && in[1]) {
            in++;
            *out++ = *in == 'n' ? '\n' : *in == 'r' ? '\r' : *in == 't' ? '\t' : *in;
        } else
            *out++ = *in;
    }
    *out = 0;
    return s;
}

static int splitFields(char* line, char** fields, int max)
{
    int n = 0;
    fields[n++] = line;
    for (char* c = line; *c && n < max; c++) {
        if (*c == '\t') {
            *c = 0;
            fields[n++] = c + 1;
        }
    }
    return n;
}

static int worldIndex(const char* name)
{
    return !strcmp(name, "main") ? AB_WORLD_MAIN : !strcmp(name, "isolated") ? AB_WORLD_ISOLATED : -1;
}

static int compareHosts(const void* a, const void* b)
{
    return strcmp(((const Host*)a)->key, ((const Host*)b)->key);
}

static const char* findHost(const AbSiteData* data, const char* key)
{
    Host needle = { key, NULL };
    const Host* found = bsearch(&needle, data->hosts->data, data->hosts->len, sizeof(Host), compareHosts);
    return found ? found->refs : NULL;
}

static int compareUints(const void* a, const void* b)
{
    guint x = *(const guint*)a, y = *(const guint*)b;
    return x < y ? -1 : x > y;
}

void ab_site_data_free(AbSiteData* data)
{
    if (!data)
        return;
    for (int w = 0; w < 2; w++) {
        if (data->lib[w])
            g_array_unref(data->lib[w]);
        if (data->libIndex[w])
            g_hash_table_unref(data->libIndex[w]);
        if (data->genericCalls[w])
            g_array_unref(data->genericCalls[w]);
    }
    if (data->calls)
        g_array_unref(data->calls);
    if (data->cosmetics)
        g_array_unref(data->cosmetics);
    if (data->hosts)
        g_array_unref(data->hosts);
    if (data->regexes) {
        for (guint i = 0; i < data->regexes->len; i++) {
            HostRegex* r = &g_array_index(data->regexes, HostRegex, i);
            if (r->regex)
                g_regex_unref(r->regex);
        }
        g_array_unref(data->regexes);
    }
    g_free(data->version);
    g_free(data->buffer);
    g_free(data);
}

static void collectGeneric(AbSiteData* data);

AbSiteData* ab_site_data_load(const char* path, char** error)
{
    AbSiteData* data = g_new0(AbSiteData, 1);
    GError* gerror = NULL;
    if (!g_file_get_contents(path, &data->buffer, &data->size, &gerror)) {
        *error = g_strdup(gerror ? gerror->message : "cannot read");
        g_clear_error(&gerror);
        ab_site_data_free(data);
        return NULL;
    }
    for (int w = 0; w < 2; w++) {
        data->lib[w] = g_array_new(FALSE, FALSE, sizeof(LibItem));
        data->libIndex[w] = g_hash_table_new(g_str_hash, g_str_equal);
    }
    data->calls = g_array_new(FALSE, FALSE, sizeof(Call));
    data->cosmetics = g_array_new(FALSE, FALSE, sizeof(Cosmetic));
    data->hosts = g_array_new(FALSE, FALSE, sizeof(Host));
    data->regexes = g_array_new(FALSE, FALSE, sizeof(HostRegex));
    char* line = data->buffer;
    char* end = data->buffer + data->size;
    gboolean header = FALSE, sorted = TRUE;
    const char* previousKey = NULL;
    while (line < end) {
        char* next = memchr(line, '\n', (size_t)(end - line));
        if (next)
            *next = 0;
        char* f[6];
        int n = *line ? splitFields(line, f, 6) : 0;
        if (!header) {
            if (n < 4 || strcmp(f[0], "ubol-scripting") || atoi(f[1]) != 1) {
                *error = g_strdup("not ubol-scripting data (format 1)");
                ab_site_data_free(data);
                return NULL;
            }
            data->version = g_strdup(f[2]);
            data->converter = atoi(f[3]);
            header = TRUE;
        } else if (n == 5 && !strcmp(f[0], "L")) {
            int w = worldIndex(f[1]);
            if (w >= 0) {
                LibItem item = { f[2], f[3], unescapeField(f[4]) };
                g_array_append_val(data->lib[w], item);
                g_hash_table_insert(data->libIndex[w], (gpointer)item.name, GUINT_TO_POINTER(data->lib[w]->len));
                if (!strcmp(item.name, "*"))
                    data->whole[w] = TRUE;
            }
        } else if (n == 5 && !strcmp(f[0], "A")) {
            int w = worldIndex(f[1]);
            Call call = { (guint8)(w < 0 ? 0 : w), (guint8)atoi(f[2]), f[3], unescapeField(f[4]) };
            if (w < 0)
                call.fn = NULL; /* keeps the numbering */
            g_array_append_val(data->calls, call);
        } else if (n == 3 && !strcmp(f[0], "C")) {
            Cosmetic cosmetic = { f[1][0], unescapeField(f[2]) };
            data->procedurals += cosmetic.kind == 'p';
            g_array_append_val(data->cosmetics, cosmetic);
        } else if (n == 3 && !strcmp(f[0], "H")) {
            Host host = { f[1], f[2] };
            if (previousKey && strcmp(previousKey, host.key) >= 0)
                sorted = FALSE;
            previousKey = host.key;
            g_array_append_val(data->hosts, host);
        } else if (n == 4 && (!strcmp(f[0], "RA") || !strcmp(f[0], "RC"))) {
            HostRegex r = { f[0][1] == 'C', unescapeField(f[1]), NULL, f[3] };
            r.regex = g_regex_new(unescapeField(f[2]), G_REGEX_OPTIMIZE, 0, NULL);
            if (r.regex)
                g_array_append_val(data->regexes, r);
        } else if (n == 2 && !strcmp(f[0], "P"))
            data->procedural = unescapeField(f[1]);
        line = next ? next + 1 : end;
    }
    if (!header || !data->hosts->len) {
        *error = g_strdup("no hosts in the site data");
        ab_site_data_free(data);
        return NULL;
    }
    if (!sorted)
        g_array_sort(data->hosts, compareHosts);
    collectGeneric(data);
    return data;
}

void ab_site_data_info(const AbSiteData* data, AbSiteDataInfo* info)
{
    memset(info, 0, sizeof *info);
    if (!data)
        return;
    info->version = data->version;
    info->converter = data->converter;
    info->hosts = data->hosts->len;
    for (guint i = 0; i < data->calls->len; i++) {
        const Call* c = &g_array_index(data->calls, Call, i);
        if (c->fn)
            info->calls[c->world]++;
    }
    info->cosmetics = data->cosmetics->len;
    info->procedurals = data->procedurals;
    info->bytes = data->size;
    info->indexBytes = data->hosts->len * sizeof(Host) + data->calls->len * sizeof(Call) + data->cosmetics->len * sizeof(Cosmetic);
    for (int w = 0; w < 2; w++) {
        info->libraryItems[w] = data->lib[w]->len;
        info->indexBytes += data->lib[w]->len * (sizeof(LibItem) + 32);
    }
}

/* ---- lookup ---- */

typedef struct {
    GArray* calls;         /* guint, in the order found */
    GArray* ancestorCalls; /* guint: only below the top-level host */
    GHashTable* seen;      /* call + 1 */
    GHashTable* except;    /* call + 1 */
    gboolean disabled[2];
    GArray* cosmetics;     /* guint */
    GHashTable* cosmeticSeen;
    GHashTable* cosmeticExcept;
} Collect;

static void collectInit(Collect* c)
{
    memset(c, 0, sizeof *c);
    c->calls = g_array_new(FALSE, FALSE, sizeof(guint));
    c->ancestorCalls = g_array_new(FALSE, FALSE, sizeof(guint));
    c->seen = g_hash_table_new(NULL, NULL);
    c->except = g_hash_table_new(NULL, NULL);
    c->cosmetics = g_array_new(FALSE, FALSE, sizeof(guint));
    c->cosmeticSeen = g_hash_table_new(NULL, NULL);
    c->cosmeticExcept = g_hash_table_new(NULL, NULL);
}

static void collectClear(Collect* c)
{
    g_array_unref(c->calls);
    g_array_unref(c->ancestorCalls);
    g_hash_table_unref(c->seen);
    g_hash_table_unref(c->except);
    g_array_unref(c->cosmetics);
    g_hash_table_unref(c->cosmeticSeen);
    g_hash_table_unref(c->cosmeticExcept);
}

/* refs: a<n>, a-<n>, xm, xi, c<n>, c-<n>. */
static void applyRefs(const AbSiteData* data, Collect* c, const char* refs, gboolean ancestor, gboolean scriptlets, gboolean cosmetics)
{
    for (const char* p = refs; p && *p;) {
        char type = *p++;
        gboolean exception = FALSE;
        if (type == 'x') {
            if (scriptlets && (*p == 'm' || *p == 'i'))
                c->disabled[*p == 'm' ? AB_WORLD_MAIN : AB_WORLD_ISOLATED] = TRUE;
        } else {
            if (*p == '-') {
                exception = TRUE;
                p++;
            }
            char* after = NULL;
            unsigned long n = strtoul(p, &after, 10);
            p = after;
            gpointer key = GUINT_TO_POINTER((guint)n + 1);
            if (type == 'a' && scriptlets && n < data->calls->len) {
                if (exception)
                    g_hash_table_add(c->except, key);
                else if (!g_hash_table_contains(c->seen, key)) {
                    g_hash_table_add(c->seen, key);
                    guint id = (guint)n;
                    g_array_append_val(ancestor ? c->ancestorCalls : c->calls, id);
                }
            } else if (type == 'c' && cosmetics && n < data->cosmetics->len) {
                if (exception)
                    g_hash_table_add(c->cosmeticExcept, key);
                else if (!g_hash_table_contains(c->cosmeticSeen, key)) {
                    g_hash_table_add(c->cosmeticSeen, key);
                    guint id = (guint)n;
                    g_array_append_val(c->cosmetics, id);
                }
            }
        }
        while (*p && *p != ',')
            p++;
        if (*p == ',')
            p++;
    }
}

/* host, its parent domains, then "*" (if star); the entities (name.*) go to ens. */
static void hostNames(const char* host, gboolean star, GPtrArray* hns, GPtrArray* ens)
{
    g_ptr_array_add(hns, g_strdup(host));
    for (const char* dot = strchr(host, '.'); dot; dot = strchr(dot + 1, '.'))
        g_ptr_array_add(hns, g_strdup(dot + 1));
    if (star)
        g_ptr_array_add(hns, g_strdup("*"));
    if (!ens)
        return;
    for (guint i = 0; i < hns->len; i++) {
        char* hn = g_strdup(g_ptr_array_index(hns, i));
        for (char* dot = strrchr(hn, '.'); dot; dot = strrchr(hn, '.')) {
            *dot = 0;
            g_ptr_array_add(ens, g_strdup_printf("%s.*", hn));
        }
        g_free(hn);
    }
    /* longest first (then reverse order), as uBO Lite sorts them */
    for (guint i = 1; i < ens->len; i++) {
        for (guint j = i; j > 0; j--) {
            const char* a = g_ptr_array_index(ens, j - 1);
            const char* b = g_ptr_array_index(ens, j);
            size_t la = strlen(a), lb = strlen(b);
            if (la > lb || (la == lb && strcmp(a, b) >= 0))
                break;
            gpointer t = ens->pdata[j - 1];
            ens->pdata[j - 1] = ens->pdata[j];
            ens->pdata[j] = t;
        }
    }
}

static void lookupNames(const AbSiteData* data, Collect* c, GPtrArray* names, const char* suffix, gboolean ancestor, gboolean cosmetics)
{
    for (guint i = 0; i < names->len; i++) {
        const char* name = g_ptr_array_index(names, i);
        char* key = suffix ? g_strconcat(name, suffix, NULL) : NULL;
        const char* refs = findHost(data, key ? key : name);
        if (refs)
            applyRefs(data, c, refs, ancestor, TRUE, cosmetics);
        g_free(key);
    }
}

/* Calls of world w left after the exceptions, by ruleset (stable). */
static GArray* finalCalls(const AbSiteData* data, const Collect* c, GArray* found, int w)
{
    GArray* out = g_array_new(FALSE, FALSE, sizeof(guint));
    if (c->disabled[w])
        return out;
    for (int rank = 0; rank < 256; rank++) {
        gboolean any = FALSE;
        for (guint i = 0; i < found->len; i++) {
            guint id = g_array_index(found, guint, i);
            const Call* call = &g_array_index(data->calls, Call, id);
            if (!call->fn || call->world != w || g_hash_table_contains(c->except, GUINT_TO_POINTER(id + 1)))
                continue;
            if (call->rank == rank)
                g_array_append_val(out, id);
            else if (call->rank > rank)
                any = TRUE;
        }
        if (!any)
            break;
    }
    return out;
}

static void markNeeded(const AbSiteData* data, int w, guint index, guint8* needed)
{
    if (needed[index])
        return;
    needed[index] = 1;
    const LibItem* item = &g_array_index(data->lib[w], LibItem, index);
    const char* p = item->deps;
    while (p && *p) {
        const char* comma = strchr(p, ',');
        size_t length = comma ? (size_t)(comma - p) : strlen(p);
        char name[128];
        if (length && length < sizeof name) {
            memcpy(name, p, length);
            name[length] = 0;
            guint found = GPOINTER_TO_UINT(g_hash_table_lookup(data->libIndex[w], name));
            if (found)
                markNeeded(data, w, found - 1, needed);
        }
        p = comma ? comma + 1 : NULL;
    }
}

static gboolean debugCalls;

void ab_site_data_set_debug(gboolean on)
{
    debugCalls = on;
}

static void appendCalls(GString* out, const AbSiteData* data, GArray* ids)
{
    for (guint i = 0; i < ids->len; i++) {
        const Call* call = &g_array_index(data->calls, Call, g_array_index(ids, guint, i));
        if (debugCalls)
            g_string_append_printf(out, "try { %s(...%s); } catch (e) { console.log('[ubol] %s threw: ' + e); }\n", call->fn, call->args, call->fn);
        else
            g_string_append_printf(out, "try { %s(...%s); } catch (e) { }\n", call->fn, call->args);
    }
}

static const char procShim[] =
    "(function () {\n"
    "if (self.cssAPI === undefined) {\n"
    "    self.cssAPI = { insert(css) { try { const sheet = new CSSStyleSheet(); sheet.replaceSync(css); document.adoptedStyleSheets = [ ...document.adoptedStyleSheets, sheet ]; } catch (e) { } } };\n"
    "}\n"
    "if (self.chrome === undefined) { self.chrome = { runtime: { sendMessage() { return Promise.resolve(); } } }; }\n"
    "})();\n";

/* The script for world w, or NULL (nothing to run). */
static char* buildScript(const AbSiteData* data, int w, GArray* calls, GArray* ancestorCalls, const char* host, const char* topHost,
    GPtrArray* procedurals)
{
    gboolean haveProcedurals = w == AB_WORLD_ISOLATED && procedurals && procedurals->len && data->procedural;
    if (!calls->len && !ancestorCalls->len && !haveProcedurals)
        return NULL;
    GString* out = g_string_new(NULL);
    g_string_append_printf(out, "/* uBlock Origin Lite (GPL-3.0, github.com/uBlockOrigin/uBOL-home) %s, %s world, for %s: %u scriptlet call(s) */\n",
        data->version ? data->version : "", worldNames[w], host, calls->len + ancestorCalls->len);
    if (calls->len || ancestorCalls->len) {
        if (data->whole[w]) {
            guint index = GPOINTER_TO_UINT(g_hash_table_lookup(data->libIndex[w], "*")) - 1;
            g_string_append(out, "(function ($SECRET_ID$) {\n");
            g_string_append(out, g_array_index(data->lib[w], LibItem, index).source);
        } else {
            guint8* needed = g_new0(guint8, data->lib[w]->len + 1);
            for (int pass = 0; pass < 2; pass++) {
                GArray* ids = pass ? ancestorCalls : calls;
                for (guint i = 0; i < ids->len; i++) {
                    const Call* call = &g_array_index(data->calls, Call, g_array_index(ids, guint, i));
                    guint found = GPOINTER_TO_UINT(g_hash_table_lookup(data->libIndex[w], call->fn));
                    if (found)
                        markNeeded(data, w, found - 1, needed);
                }
            }
            g_string_append(out, "(function () {\nconst scriptletGlobals = {};\n");
            for (guint i = 0; i < data->lib[w]->len; i++) {
                if (needed[i]) {
                    g_string_append(out, g_array_index(data->lib[w], LibItem, i).source);
                    g_string_append_c(out, '\n');
                }
            }
            g_free(needed);
        }
        g_string_append(out, "\n");
        appendCalls(out, data, calls);
        if (ancestorCalls->len && topHost) {
            g_string_append_printf(out,
                "if (Array.from(self.location.ancestorOrigins || [], o => o.replace(/^[^:]+:\\/\\//, '').replace(/:\\d+$/, '')).includes('%s')) {\n",
                topHost);
            appendCalls(out, data, ancestorCalls);
            g_string_append(out, "}\n");
        }
        if (data->whole[w])
            g_string_append(out, "delete self[$SECRET_ID$];\n})('ubol-' + Math.random().toString(36).slice(2));\n");
        else
            g_string_append(out, "})();\n");
    }
    if (haveProcedurals) {
        g_string_append(out, procShim);
        g_string_append(out, data->procedural);
        g_string_append(out, "\n(function () {\ntry {\nconst api = new self.ProceduralFiltererAPI();\napi.addProcedurals([\n");
        for (guint i = 0; i < procedurals->len; i++)
            g_string_append_printf(out, "%s%s\n", (const char*)g_ptr_array_index(procedurals, i), i + 1 < procedurals->len ? "," : "");
        g_string_append(out, "]);\n} catch (e) { }\n})();\n");
    }
    return g_string_free(out, FALSE);
}

static gboolean sameSet(GArray* a, GArray* sortedB)
{
    if (a->len != sortedB->len)
        return FALSE;
    GArray* copy = g_array_copy(a);
    g_array_sort(copy, compareUints);
    gboolean same = !memcmp(copy->data, sortedB->data, a->len * sizeof(guint));
    g_array_unref(copy);
    return same;
}

static void lookup(const AbSiteData* data, const char* host, const char* topHost, gboolean genericOnly, AbSiteScripts* out)
{
    memset(out, 0, sizeof *out);
    Collect c;
    collectInit(&c);
    GPtrArray* hns = g_ptr_array_new_with_free_func(g_free);
    GPtrArray* ens = g_ptr_array_new_with_free_func(g_free);
    if (genericOnly)
        g_ptr_array_add(hns, g_strdup("*"));
    else
        hostNames(host, TRUE, hns, ens);
    lookupNames(data, &c, hns, NULL, FALSE, TRUE);
    lookupNames(data, &c, ens, NULL, FALSE, TRUE);
    if (!genericOnly && topHost && strcmp(topHost, host)) {
        GPtrArray* thns = g_ptr_array_new_with_free_func(g_free);
        GPtrArray* tens = g_ptr_array_new_with_free_func(g_free);
        hostNames(topHost, FALSE, thns, tens);
        lookupNames(data, &c, thns, ">>", TRUE, FALSE);
        lookupNames(data, &c, tens, ">>", TRUE, FALSE);
        g_ptr_array_unref(thns);
        g_ptr_array_unref(tens);
    }
    if (!genericOnly) {
        for (guint i = 0; i < data->regexes->len; i++) {
            const HostRegex* r = &g_array_index(data->regexes, HostRegex, i);
            for (guint j = 0; j < (r->cosmetic ? 1u : hns->len); j++) {
                const char* hn = g_ptr_array_index(hns, j);
                if (strstr(hn, r->needle) && g_regex_match(r->regex, hn, 0, NULL)) {
                    applyRefs(data, &c, r->refs, FALSE, !r->cosmetic, r->cosmetic);
                    break;
                }
            }
        }
    }
    GString* css = g_string_new(NULL);
    GPtrArray* procedurals = g_ptr_array_new();
    for (guint i = 0; i < c.cosmetics->len; i++) {
        guint id = g_array_index(c.cosmetics, guint, i);
        if (g_hash_table_contains(c.cosmeticExcept, GUINT_TO_POINTER(id + 1)))
            continue;
        const Cosmetic* cosmetic = &g_array_index(data->cosmetics, Cosmetic, id);
        if (cosmetic->kind == 'c') {
            g_string_append(css, cosmetic->text);
            g_string_append_c(css, '\n');
            out->cssRules++;
        } else if (cosmetic->kind == 'p')
            g_ptr_array_add(procedurals, (gpointer)cosmetic->text);
    }
    out->procedurals = procedurals->len;
    if (out->cssRules)
        out->css = g_string_free(css, FALSE);
    else
        g_string_free(css, TRUE);
    for (int w = 0; w < 2; w++) {
        GArray* calls = finalCalls(data, &c, c.calls, w);
        GArray* ancestorCalls = finalCalls(data, &c, c.ancestorCalls, w);
        out->calls[w] = calls->len + ancestorCalls->len;
        out->usesTop = out->usesTop || ancestorCalls->len;
        out->generic[w] = !genericOnly && data->genericCalls[w] && !ancestorCalls->len && sameSet(calls, data->genericCalls[w])
            && (w == AB_WORLD_MAIN || !procedurals->len);
        if (genericOnly) {
            out->generic[w] = TRUE;
            g_array_sort(calls, compareUints);
            if (!data->genericCalls[w])
                ((AbSiteData*)data)->genericCalls[w] = g_array_ref(calls);
        }
        if (!out->generic[w] || genericOnly)
            out->script[w] = buildScript(data, w, calls, ancestorCalls, genericOnly ? "every site" : host, topHost, procedurals);
        g_array_unref(calls);
        g_array_unref(ancestorCalls);
    }
    g_ptr_array_unref(procedurals);
    g_ptr_array_unref(hns);
    g_ptr_array_unref(ens);
    collectClear(&c);
}

static void collectGeneric(AbSiteData* data)
{
    AbSiteScripts scripts;
    lookup(data, "*", NULL, TRUE, &scripts);
    ab_site_scripts_clear(&scripts);
}

void ab_site_data_for_host(const AbSiteData* data, const char* host, const char* topHost, AbSiteScripts* out)
{
    lookup(data, host, topHost, FALSE, out);
}

void ab_site_data_generic(const AbSiteData* data, AbSiteScripts* out)
{
    lookup(data, "*", NULL, TRUE, out);
}

gboolean ab_site_data_has_ancestor_rules(const AbSiteData* data, const char* topHost)
{
    if (!data || !topHost)
        return FALSE;
    GPtrArray* hns = g_ptr_array_new_with_free_func(g_free);
    GPtrArray* ens = g_ptr_array_new_with_free_func(g_free);
    hostNames(topHost, FALSE, hns, ens);
    gboolean found = FALSE;
    for (int pass = 0; pass < 2 && !found; pass++) {
        GPtrArray* names = pass ? ens : hns;
        for (guint i = 0; i < names->len && !found; i++) {
            char* key = g_strconcat(g_ptr_array_index(names, i), ">>", NULL);
            found = findHost(data, key) != NULL;
            g_free(key);
        }
    }
    g_ptr_array_unref(hns);
    g_ptr_array_unref(ens);
    return found;
}

void ab_site_scripts_clear(AbSiteScripts* scripts)
{
    g_free(scripts->script[0]);
    g_free(scripts->script[1]);
    g_free(scripts->css);
    memset(scripts, 0, sizeof *scripts);
}
