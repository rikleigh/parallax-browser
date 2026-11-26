/*
 * settings.c: the browser's settings (README: "Settings"), for the link
 * server's /settings page, /settings.json and POST /settings (ps5browser.c).
 *
 * One table, three kinds of storage in /data/ps5webkit:
 *   child.env lines (KEY=VALUE, read by each WebProcess and NetworkProcess
 *     when it starts): rewritten with every other line (comments, JSC_*,
 *     WEBKIT_DEBUG...) kept as it was; a setting back at its default loses
 *     its line. Written to child.env.tmp, then renamed over child.env.
 *   flag files (no-hwdec, audio-silent, push-off, adblock-off,
 *     no-compositing, no-webgl, no-webgpu, no-pages-in-browser): present = that feature off; the title
 *     reads them when it starts (no-compositing: WebKit in the title reads
 *     it whenever a page gets a new WebProcess; no-webgl: applied at once,
 *     and no-webgpu likewise, ps5browser.c applyWebContentSettings). Opt-in flag files
 *     (perf-overlay, safer-browsing): present = that feature on.
 *     safer-browsing overrides jit, wasm, webgl, webgpu and web_in_title while present (each
 *     WebProcess reads it at start, ps5wk-child.cpp configureJIT; the title
 *     for WebGL) and leaves their own values alone for when it goes.
 *   home-url.txt: the page the browser opens when it starts, in the PS5
 *     user's own folder (profile.c), as KIND_FILE settings are.
 * Each setting says what applies it: a page restart (a new WebProcess), a
 * browser restart (the title) or nothing. The values in use are remembered
 * (ps5wk_settings_applied), so the page can say what waits for a restart.
 *
 * Any thread (the link server's, the main loop): one lock around it all.
 * No WebKit here: the host test builds this file with GLib alone.
 */
#include "settings.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef enum { KIND_ENV, KIND_FLAG, KIND_FLAG_ON, KIND_FILE } SettingKind;
typedef enum { TYPE_CHOICE, TYPE_BOOL, TYPE_NUMBER, TYPE_TEXT } SettingType;

typedef struct {
    const char* id;
    const char* group;    /* one of groups[]: where the page shows it */
    SettingKind kind;
    SettingType type;
    const char* name;     /* the child.env variable, or the file */
    const char* fallback; /* the value without a line or file */
    int scope;            /* PS5WK_SETTINGS_PAGE, _BROWSER or 0 */
    const char* label;
    const char* help;
    const char* choices;  /* the page's buttons, "value=label|...": every
                             value a choice may take, a number's presets */
    int min, max;         /* numbers */
} Setting;

/* The page's groups, in its order (the settings below are in that order
 * too). The page gives general, link, ads and security a place in its first
 * sections; every other group is a heading in its Advanced section, so a
 * new group needs no change to the page. */
static const struct {
    const char* id;
    const char* title;
} groups[] = {
    { "general", "General" },
    { "link", "Phone and computer" },
    { "ads", "Ad blocking" },
    { "security", "Security" },
    { "scripts", "Scripts" },
    { "graphics", "Graphics" },
    { "video", "Video" },
    { "memory", "Memory" },
    { "processes", "Processes" },
    { "experimental", "Experimental and diagnostics" },
};

/* Semantics and defaults: ps5wk-child.cpp (JIT, memory limit), patches
 * 0032 (painting, scrolling), 0031 (decommit), 0017 (hole punch), 0016
 * (VP9), titlevdec.c (no-hwdec), DrawingAreaProxyWCPS5.cpp in WebKit
 * (compositing, WC builds only), adblock.c (adblock: applied at once,
 * ps5browser.c calls ps5wk_adblock_setting_changed), wpe-ps5/PS5Renderer.cpp
 * (perf-overlay, read at start; ps5browser.c applies changes at once and
 * keeps the file in step with the pad's L3 + R3), ps5browser.c (the rest).
 * Help: the page always shows the first sentence (up to the first ". ") and
 * the rest behind its More button, so the first one says what the setting
 * is. What applies a setting is not in the help: the page says it. */
static const Setting settings[] = {
    /* General: what most people come for */
    { "home_url", "general", KIND_FILE, TYPE_TEXT, "home-url.txt", "", 0, "Home page",
        "Opens when the browser starts, in new tabs and with Home. Empty: the built-in start page. On the PS5: □ on the Home button in the browser menu makes the page shown the home page, or goes back to the start page.",
        NULL, 0, 0 },
    { "search", "general", KIND_FILE, TYPE_CHOICE, "search.txt", "google", 0, "Search engine",
        "Searches for words typed in the address bar or on the start page, when they aren't an address.",
        "google=Google|bing=Bing|duckduckgo=DuckDuckGo|ddglite=DuckDuckGo Lite|brave=Brave Search|ecosia=Ecosia|startpage=Startpage", 0, 0 },
    { "sound", "general", KIND_FLAG, TYPE_BOOL, "audio-silent", "1", PS5WK_SETTINGS_BROWSER, "Sound",
        "Off plays every page at volume 0.", NULL, 0, 0 },
    { "link_server", "link", KIND_FLAG, TYPE_BOOL, "push-off", "1", PS5WK_SETTINGS_BROWSER, "Link server on the network",
        "Lets a phone or Mac send links here and open this page (port 9040). Off: only the PS5 itself can open Settings (from the browser menu).",
        NULL, 0, 0 },
    { "adblock", "ads", KIND_FLAG, TYPE_BOOL, "adblock-off", "1", 0, "Ad blocking",
        "Blocks ads and trackers on every site, except the sites where you allow ads. Uses uBlock Origin Lite's network rules, cosmetic filters and scriptlets. Applies at once.",
        NULL, 0, 0 },
    /* Security: Safer browsing; what it turns off is under Scripts,
     * Graphics and Experimental (isOverriddenBySafer) */
    { "safer", "security", KIND_FLAG_ON, TYPE_BOOL, "safer-browsing", "0", PS5WK_SETTINGS_PAGE, "Safer browsing",
        "Turns off the JavaScript JIT, WebAssembly, WebGL and WebGPU, where most attacks on browsers start. Pages run slower (scripts several times slower) and some games and 3D sites won't work. Those four settings (under Advanced) keep their own values for when it's off. Also the shield button in the browser menu.",
        NULL, 0, 0 },
    /* Advanced */
    { "jit", "scripts", KIND_ENV, TYPE_CHOICE, "PS5WK_JIT", "1", PS5WK_SETTINGS_PAGE, "JavaScript JIT",
        "Compiles page scripts to machine code. Lower tiers or Off are slower; try them if a site crashes. Off also removes the most common way in for attacks.",
        "1=All tiers (fastest)|dfg=Up to DFG|baseline=Baseline only|0=Off (interpreter)", 0, 0 },
    { "wasm", "scripts", KIND_ENV, TYPE_BOOL, "PS5WK_WASM", "1", PS5WK_SETTINGS_PAGE, "WebAssembly",
        "Compiled code that some games, editors and video tools run in the page. Needs the JIT (off with it).",
        NULL, 0, 0 },
    { "webgl", "graphics", KIND_FLAG, TYPE_BOOL, "no-webgl", "1", 0, "WebGL",
        "3D graphics for pages and games, on the PS5's GPU. Off: games and maps that need it show an error. Applies to pages loaded from now on.",
        NULL, 0, 0 },
    { "webgpu", "graphics", KIND_FLAG, TYPE_BOOL, "no-webgpu", "1", 0, "WebGPU",
        "The newer 3D and GPU computing interface for pages and games, on the PS5's GPU. Off: sites fall back to WebGL where they can; try it if a site's 3D or video effects misbehave. Applies to pages loaded from now on.",
        NULL, 0, 0 },
    { "compositing", "graphics", KIND_FLAG, TYPE_BOOL, "no-compositing", "1", PS5WK_SETTINGS_PAGE, "GPU compositing",
        "Puts pages together on the GPU, in layers, and scrolls them on the GPU. Off draws pages on the CPU, as before; try it if a page looks wrong.",
        NULL, 0, 0 },
    { "paint_threads", "graphics", KIND_ENV, TYPE_NUMBER, "PS5WK_PAINT_THREADS", "4", PS5WK_SETTINGS_PAGE, "Painting threads",
        "CPU threads that draw the page. More is smoother on busy pages; 1 draws on one thread, as before.",
        "1=1|2=2|3=3|4=4|6=6|8=8", 1, 8 },
    { "scroll_copy", "graphics", KIND_ENV, TYPE_BOOL, "PS5WK_SCROLL_COPY", "1", PS5WK_SETTINGS_PAGE, "Fast scrolling",
        "Scrolling moves the pixels already drawn instead of drawing the whole view again.", NULL, 0, 0 },
    { "hwdec", "video", KIND_FLAG, TYPE_BOOL, "no-hwdec", "1", PS5WK_SETTINGS_BROWSER, "Hardware video decoding",
        "The PS5's video decoder plays H.264, HEVC and VP9, up to 8K. Off decodes in software (1080p at most).", NULL, 0, 0 },
    { "media_vp9", "video", KIND_ENV, TYPE_BOOL, "PS5WK_MEDIA_VP9", "1", PS5WK_SETTINGS_PAGE, "Offer VP9 video",
        "Lets sites such as YouTube send VP9. Off makes YouTube send H.264 instead.", NULL, 0, 0 },
    { "holepunch", "video", KIND_ENV, TYPE_BOOL, "PS5WK_HOLEPUNCH", "1", PS5WK_SETTINGS_PAGE, "Video under the page",
        "Video is drawn straight to the screen beneath the page, which is smoother. Off paints every frame into the page.", NULL, 0, 0 },
    { "mem_restart_mb", "memory", KIND_ENV, TYPE_NUMBER, "PS5WK_MEM_RESTART_MB", "3072", PS5WK_SETTINGS_PAGE, "Page memory limit",
        "When a page's process has used this much memory, the next link you follow opens in a fresh process, before memory runs out.",
        "0=Off|2048=2 GB|3072=3 GB|4096=4 GB|6144=6 GB", 0, 16384 },
    { "pas_decommit", "memory", KIND_ENV, TYPE_BOOL, "PS5WK_PAS_DECOMMIT", "1", PS5WK_SETTINGS_PAGE, "Give back freed memory",
        "Returns memory that pages no longer use to the system. Off keeps it (the old behaviour). The network process picks it up at the next browser start.",
        NULL, 0, 0 },
    { "web_in_title", "processes", KIND_FLAG, TYPE_BOOL, "no-pages-in-browser", "1", PS5WK_SETTINGS_BROWSER, "Pages in the browser process",
        "Runs pages inside the browser itself, which gets more of the PS5's CPU: games and heavy pages run up to twice as fast. The price: a page that crashes closes the whole browser, and pages are not kept apart from the browser. Off runs each page in a separate process: slower, and without those two. While it is on, settings that need a page restart need a browser restart. Safer browsing always keeps pages in their own process.",
        NULL, 0, 0 },
    { "perf_overlay", "experimental", KIND_FLAG_ON, TYPE_BOOL, "perf-overlay", "0", 0, "Performance overlay",
        "Live numbers in a corner of the TV: frames per second, frame times, CPU, memory, GPU compositing, scrolling, video and network. Applies at once. L3 + R3 (both sticks pressed in) also turns it on or off.",
        NULL, 0, 0 },
};
#define SETTING_COUNT (sizeof settings / sizeof settings[0])

static GMutex lock;
static char* dataDir;
static char* userDir; /* KIND_FILE settings: the PS5 user's own */
static PS5WKSettingsNormalize normalizeURL;
static char* applied[SETTING_COUNT];
static char* status;
static unsigned statusSerial;

static char* pathFor(const char* name)
{
    return g_build_filename(dataDir ? dataDir : "/data/ps5webkit", name, NULL);
}

static char* pathOf(const Setting* s)
{
    return s->kind == KIND_FILE && userDir ? g_build_filename(userDir, s->name, NULL) : pathFor(s->name);
}

/* open(), not access(): access() reports existing files as missing in a title. */
static gboolean fileExists(const char* path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return FALSE;
    close(fd);
    return TRUE;
}

/* Writes contents to path through path.tmp and a rename. */
static gboolean writeAtomically(const char* path, const char* contents)
{
    char* temporary = g_strdup_printf("%s.tmp", path);
    gboolean ok = FALSE;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        size_t length = strlen(contents);
        ok = write(fd, contents, length) == (ssize_t)length;
        fchmod(fd, 0666); /* FTP can edit it too */
        fsync(fd);
        ok = !close(fd) && ok;
        if (ok)
            ok = !rename(temporary, path);
        if (!ok)
            unlink(temporary);
    }
    g_free(temporary);
    return ok;
}

/* The value the children get for key: its last KEY=VALUE line, as
 * ps5wk-child reads it (no spaces trimmed; # lines skipped), or NULL. */
static char* envValue(const char* contents, const char* key)
{
    char* value = NULL;
    size_t keyLength = strlen(key);
    for (const char* line = contents; line && *line;) {
        size_t length = strcspn(line, "\n");
        if (length > keyLength && !strncmp(line, key, keyLength) && line[keyLength] == '=') {
            g_free(value);
            value = g_strndup(line + keyLength + 1, length - keyLength - 1);
            value[strcspn(value, "\r")] = 0;
        }
        line += length;
        if (*line)
            line++;
    }
    return value;
}

/* The setting's value as the page shows it: bools "1"/"0", JIT tiers by
 * their canonical names, numbers and text as written. */
static char* currentValue(const Setting* s, const char* env)
{
    if (s->kind == KIND_FLAG || s->kind == KIND_FLAG_ON) {
        char* path = pathOf(s);
        gboolean present = fileExists(path);
        g_free(path);
        return g_strdup(present == (s->kind == KIND_FLAG_ON) ? "1" : "0");
    }
    if (s->kind == KIND_FILE) {
        char* path = pathOf(s);
        gchar* contents = NULL;
        if (!g_file_get_contents(path, &contents, NULL, NULL))
            contents = g_strdup(s->fallback);
        g_free(path);
        return g_strstrip(contents);
    }
    char* raw = envValue(env, s->name);
    if (!raw || !*raw) {
        g_free(raw);
        return g_strdup(s->fallback);
    }
    if (s->type == TYPE_BOOL) {
        /* 0031 looks at the first character only; the others at "0". */
        gboolean off = !strcmp(s->name, "PS5WK_PAS_DECOMMIT") ? raw[0] == '0' : !strcmp(raw, "0");
        g_free(raw);
        return g_strdup(off ? "0" : "1");
    }
    if (!strcmp(s->name, "PS5WK_JIT")) {
        const char* canonical = !strcmp(raw, "off") ? "0" : !strcmp(raw, "ftl") ? "1" : NULL;
        if (canonical) {
            g_free(raw);
            return g_strdup(canonical);
        }
    }
    return raw;
}

static char* readEnv(void)
{
    char* path = pathFor("child.env");
    gchar* contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL))
        contents = g_strdup("");
    g_free(path);
    return contents;
}

static void recordApplied(int scopes)
{
    char* env = readEnv();
    for (size_t i = 0; i < SETTING_COUNT; i++) {
        if (!(settings[i].scope & scopes))
            continue;
        g_free(applied[i]);
        applied[i] = currentValue(&settings[i], env);
    }
    g_free(env);
}

void ps5wk_settings_init(const char* dir, const char* user, PS5WKSettingsNormalize normalize)
{
    g_mutex_lock(&lock);
    g_free(dataDir);
    dataDir = g_strdup(dir);
    g_free(userDir);
    userDir = g_strdup(user);
    normalizeURL = normalize;
    recordApplied(PS5WK_SETTINGS_PAGE | PS5WK_SETTINGS_BROWSER);
    g_mutex_unlock(&lock);
}

/* Pages in the browser process (ps5browser.c): its WebProcess reads the
 * page settings when the browser starts, so they wait for a browser restart
 * and the page has no Restart page. */
static gboolean pagesInBrowser;

void ps5wk_settings_set_pages_in_browser(gboolean on)
{
    g_mutex_lock(&lock);
    pagesInBrowser = on;
    g_mutex_unlock(&lock);
}

static int effectiveScope(const Setting* s)
{
    return pagesInBrowser && s->scope == PS5WK_SETTINGS_PAGE ? PS5WK_SETTINGS_BROWSER : s->scope;
}

void ps5wk_settings_applied(int scopes)
{
    g_mutex_lock(&lock);
    recordApplied(scopes);
    g_mutex_unlock(&lock);
}

void ps5wk_settings_set_status(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    char* text = g_strdup_vprintf(format, args);
    va_end(args);
    g_mutex_lock(&lock);
    g_free(status);
    status = text;
    statusSerial++;
    g_mutex_unlock(&lock);
}

static void appendJSONString(GString* out, const char* s)
{
    g_string_append_c(out, '"');
    for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\')
            g_string_append_printf(out, "\\%c", *p);
        else if (*p < 0x20)
            g_string_append_printf(out, "\\u%04x", *p);
        else
            g_string_append_c(out, (char)*p);
    }
    g_string_append_c(out, '"');
}

/* What Safer browsing turns off while it is on. */
static gboolean isOverriddenBySafer(const Setting* s)
{
    return !strcmp(s->id, "jit") || !strcmp(s->id, "wasm") || !strcmp(s->id, "webgl") || !strcmp(s->id, "webgpu")
        || !strcmp(s->id, "web_in_title");
}

static const char* scopeName(int scope)
{
    return scope == PS5WK_SETTINGS_PAGE ? "page" : scope == PS5WK_SETTINGS_BROWSER ? "browser" : "none";
}

char* ps5wk_settings_json(const char* extra)
{
    static const char* typeNames[] = { "choice", "bool", "number", "text" };
    GString* out = g_string_new("{\"ok\":true,");
    if (extra)
        g_string_append(out, extra);
    g_mutex_lock(&lock);
    char* env = readEnv();
    gboolean pending[3] = { FALSE, FALSE, FALSE };
    gboolean saferOn = FALSE;
    for (size_t i = 0; i < SETTING_COUNT; i++) {
        if (!strcmp(settings[i].id, "safer")) {
            char* safer = currentValue(&settings[i], env);
            saferOn = !strcmp(safer, "1");
            g_free(safer);
        }
    }
    g_string_append(out, "\"settings\":[");
    for (size_t i = 0; i < SETTING_COUNT; i++) {
        const Setting* s = &settings[i];
        char* value = currentValue(s, env);
        int scope = effectiveScope(s);
        gboolean waiting = scope && applied[i] && strcmp(value, applied[i]);
        if (waiting)
            pending[scope] = TRUE;
        g_string_append_printf(out, "%s{\"id\":\"%s\",\"type\":\"%s\",\"value\":", i ? "," : "", s->id, typeNames[s->type]);
        appendJSONString(out, value);
        g_string_append(out, ",\"default\":");
        appendJSONString(out, s->fallback);
        g_string_append(out, ",\"inUse\":");
        appendJSONString(out, s->scope && applied[i] ? applied[i] : value);
        g_string_append_printf(out, ",\"restart\":\"%s\",\"pending\":%s,\"group\":\"%s\"", scopeName(scope),
            waiting ? "true" : "false", s->group);
        /* Safer browsing on (on disk): these say they are off whatever
         * their own value is. */
        if (saferOn && isOverriddenBySafer(s))
            g_string_append(out, ",\"overridden\":true");
        g_string_append(out, ",\"key\":");
        appendJSONString(out, s->name);
        g_string_append(out, ",\"label\":");
        appendJSONString(out, s->label);
        g_string_append(out, ",\"help\":");
        appendJSONString(out, s->help);
        if (s->type == TYPE_NUMBER)
            g_string_append_printf(out, ",\"min\":%d,\"max\":%d", s->min, s->max);
        if (s->choices) {
            g_string_append(out, ",\"choices\":[");
            char** items = g_strsplit(s->choices, "|", -1);
            for (char** item = items; *item; item++) {
                char* eq = strchr(*item, '=');
                *eq = 0;
                g_string_append_printf(out, "%s{\"value\":", item == items ? "" : ",");
                appendJSONString(out, *item);
                g_string_append(out, ",\"label\":");
                appendJSONString(out, eq + 1);
                g_string_append_c(out, '}');
            }
            g_strfreev(items);
            g_string_append_c(out, ']');
        }
        g_string_append_c(out, '}');
        g_free(value);
    }
    g_string_append_printf(out, "],\"pagesInBrowser\":%s,\"pending\":{\"page\":%s,\"browser\":%s},\"groups\":{", pagesInBrowser ? "true" : "false",
        pending[PS5WK_SETTINGS_PAGE] ? "true" : "false", pending[PS5WK_SETTINGS_BROWSER] ? "true" : "false");
    for (size_t i = 0; i < G_N_ELEMENTS(groups); i++)
        g_string_append_printf(out, "%s\"%s\":\"%s\"", i ? "," : "", groups[i].id, groups[i].title);
    g_string_append_printf(out, "},\"statusSerial\":%u,\"status\":", statusSerial);
    appendJSONString(out, status ? status : "");
    g_string_append(out, "}\n");
    g_free(env);
    g_mutex_unlock(&lock);
    return g_string_free(out, FALSE);
}

/* ---- Request bodies: a flat JSON object, or form fields ---- */

static void skipSpace(const char** p)
{
    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n')
        (*p)++;
}

static char* parseJSONString(const char** p)
{
    if (**p != '"')
        return NULL;
    GString* out = g_string_new(NULL);
    for ((*p)++; **p && **p != '"'; (*p)++) {
        if (**p != '\\') {
            g_string_append_c(out, **p);
            continue;
        }
        (*p)++;
        switch (**p) {
        case 'n': g_string_append_c(out, '\n'); break;
        case 't': g_string_append_c(out, '\t'); break;
        case 'r': g_string_append_c(out, '\r'); break;
        case 'b': g_string_append_c(out, '\b'); break;
        case 'f': g_string_append_c(out, '\f'); break;
        case 'u': {
            char hex[5] = { 0 };
            for (int i = 0; i < 4; i++) {
                if (!g_ascii_isxdigit((*p)[1 + i])) {
                    g_string_free(out, TRUE);
                    return NULL;
                }
                hex[i] = (*p)[1 + i];
            }
            g_string_append_unichar(out, (gunichar)strtoul(hex, NULL, 16));
            *p += 4;
            break;
        }
        case 0:
            g_string_free(out, TRUE);
            return NULL;
        default: g_string_append_c(out, **p); break; /* \" \\ \/ */
        }
    }
    if (**p != '"') {
        g_string_free(out, TRUE);
        return NULL;
    }
    (*p)++;
    return g_string_free(out, FALSE);
}

/* {"id": "value" | number | true | false | null, ...}: id -> value, with
 * true/false as "1"/"0" and null as the default (""). NULL if malformed. */
static GHashTable* parseJSON(const char* text)
{
    GHashTable* fields = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    const char* p = text;
    skipSpace(&p);
    if (*p++ != '{')
        goto bad;
    skipSpace(&p);
    if (*p == '}')
        return fields;
    for (;;) {
        skipSpace(&p);
        char* key = parseJSONString(&p);
        if (!key)
            goto bad;
        skipSpace(&p);
        if (*p++ != ':') {
            g_free(key);
            goto bad;
        }
        skipSpace(&p);
        char* value = NULL;
        if (*p == '"')
            value = parseJSONString(&p);
        else if (!strncmp(p, "true", 4))
            value = g_strdup("1"), p += 4;
        else if (!strncmp(p, "false", 5))
            value = g_strdup("0"), p += 5;
        else if (!strncmp(p, "null", 4))
            value = g_strdup(""), p += 4;
        else if (*p == '-' || g_ascii_isdigit(*p)) {
            const char* start = p;
            while (*p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E' || g_ascii_isdigit(*p))
                p++;
            value = g_strndup(start, (gsize)(p - start));
        }
        if (!value) {
            g_free(key);
            goto bad;
        }
        g_hash_table_replace(fields, key, value);
        skipSpace(&p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}')
            return fields;
        goto bad;
    }
bad:
    g_hash_table_unref(fields);
    return NULL;
}

static GHashTable* parseForm(const char* text)
{
    GHashTable* fields = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    char** pairs = g_strsplit(text, "&", -1);
    for (char** pair = pairs; *pair; pair++) {
        g_strstrip(*pair);
        char* eq = strchr(*pair, '=');
        if (!**pair || !eq)
            continue;
        *eq = 0;
        for (char* c = eq + 1; *c; c++) {
            if (*c == '+')
                *c = ' ';
        }
        char* key = g_uri_unescape_string(*pair, NULL);
        char* value = g_uri_unescape_string(eq + 1, NULL);
        if (key && value)
            g_hash_table_replace(fields, key, value);
        else {
            g_free(key);
            g_free(value);
        }
    }
    g_strfreev(pairs);
    return fields;
}

/* The value to store for s, or NULL and *error. */
static char* checkValue(const Setting* s, const char* input, const char** error)
{
    char* value = g_strstrip(g_strdup(input));
    *error = NULL;
    switch (s->type) {
    case TYPE_BOOL:
        if (!strcmp(value, "1") || !g_ascii_strcasecmp(value, "true") || !g_ascii_strcasecmp(value, "on")) {
            g_free(value);
            return g_strdup("1");
        }
        if (!strcmp(value, "0") || !g_ascii_strcasecmp(value, "false") || !g_ascii_strcasecmp(value, "off")) {
            g_free(value);
            return g_strdup("0");
        }
        *error = "on or off (1 or 0)";
        break;
    case TYPE_CHOICE: {
        if (!*value) { /* null: the default */
            g_free(value);
            return g_strdup(s->fallback);
        }
        char** items = g_strsplit(s->choices, "|", -1);
        gboolean found = FALSE;
        for (char** item = items; *item && !found; item++)
            found = !strncmp(*item, value, strlen(value)) && (*item)[strlen(value)] == '=';
        g_strfreev(items);
        if (found)
            return value;
        *error = "not one of the choices";
        break;
    }
    case TYPE_NUMBER: {
        if (!*value) {
            g_free(value);
            return g_strdup(s->fallback);
        }
        char* end = NULL;
        long number = strtol(value, &end, 10);
        if (end == value || *end || number < s->min || number > s->max) {
            *error = "a whole number out of range";
            break;
        }
        g_free(value);
        return g_strdup_printf("%ld", number);
    }
    case TYPE_TEXT: {
        if (!*value)
            return value;
        char* uri = normalizeURL ? normalizeURL(value, error) : g_strdup(value);
        g_free(value);
        return uri;
    }
    }
    g_free(value);
    return NULL;
}

/* child.env with key's lines replaced by key=value (NULL: removed). The
 * first line for the key is rewritten in place, later ones dropped; a new
 * key goes at the end. */
static char* setEnvLine(const char* contents, const char* key, const char* value)
{
    GString* out = g_string_new(NULL);
    size_t keyLength = strlen(key);
    gboolean done = FALSE;
    for (const char* line = contents; *line;) {
        size_t length = strcspn(line, "\n");
        gboolean ours = length > keyLength && !strncmp(line, key, keyLength) && line[keyLength] == '=';
        if (!ours)
            g_string_append_len(out, line, (gssize)length);
        else if (value && !done)
            g_string_append_printf(out, "%s=%s", key, value);
        if (!ours || (value && !done))
            g_string_append_c(out, '\n');
        done |= ours;
        line += length;
        if (*line)
            line++;
    }
    if (value && !done)
        g_string_append_printf(out, "%s=%s\n", key, value);
    return g_string_free(out, FALSE);
}

gboolean ps5wk_settings_update(const char* body, gboolean json, char** message, int* changed)
{
    *changed = 0;
    GHashTable* fields = json ? parseJSON(body ? body : "") : parseForm(body ? body : "");
    if (!fields) {
        *message = g_strdup("not a JSON object of settings");
        return FALSE;
    }
    g_hash_table_remove(fields, "pin");
    if (!g_hash_table_size(fields)) {
        g_hash_table_unref(fields);
        *message = g_strdup("no settings in the request");
        return FALSE;
    }
    /* Check everything first: a bad request changes nothing. */
    char* values[SETTING_COUNT] = { NULL };
    GString* errors = g_string_new(NULL);
    GHashTableIter iter;
    gpointer key, input;
    g_hash_table_iter_init(&iter, fields);
    while (g_hash_table_iter_next(&iter, &key, &input)) {
        size_t i = 0;
        while (i < SETTING_COUNT && strcmp(settings[i].id, key) && strcmp(settings[i].name, key))
            i++;
        const char* error = "unknown setting";
        if (i < SETTING_COUNT) {
            g_free(values[i]);
            values[i] = checkValue(&settings[i], input, &error);
        }
        if (i >= SETTING_COUNT || !values[i])
            g_string_append_printf(errors, "%s%s: %s", errors->len ? "; " : "", (char*)key, error ? error : "invalid");
    }
    g_hash_table_unref(fields);
    if (errors->len) {
        *message = g_string_free(errors, FALSE);
        for (size_t i = 0; i < SETTING_COUNT; i++)
            g_free(values[i]);
        return FALSE;
    }
    g_string_free(errors, TRUE);

    g_mutex_lock(&lock);
    char* env = readEnv();
    char* newEnv = g_strdup(env);
    GString* done = g_string_new(NULL);
    GString* failed = g_string_new(NULL);
    GString* envDone = g_string_new(NULL); /* saved once child.env is */
    int envScopes = 0;
    for (size_t i = 0; i < SETTING_COUNT; i++) {
        const Setting* s = &settings[i];
        if (!values[i])
            continue;
        char* old = currentValue(s, env);
        gboolean same = !strcmp(old, values[i]);
        g_free(old);
        if (same)
            continue;
        gboolean ok = TRUE;
        char* path = s->kind == KIND_ENV ? NULL : pathOf(s);
        if (s->kind == KIND_ENV) {
            char* next = setEnvLine(newEnv, s->name, strcmp(values[i], s->fallback) ? values[i] : NULL);
            g_free(newEnv);
            newEnv = next;
            g_string_append_printf(envDone, "%s%s", envDone->len ? ", " : "", s->label);
            envScopes |= s->scope;
            continue;
        } else if (s->kind == KIND_FLAG || s->kind == KIND_FLAG_ON) {
            /* KIND_FLAG: the file turns the feature off; KIND_FLAG_ON: on */
            if (!strcmp(values[i], s->kind == KIND_FLAG ? "0" : "1")) {
                int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                ok = fd >= 0;
                if (ok) {
                    (void)!write(fd, "1\n", 2);
                    close(fd);
                }
            } else
                ok = !unlink(path) || !fileExists(path);
        } else if (!*values[i] || !strcmp(values[i], s->fallback)) { /* the default: no file */
            ok = !unlink(path) || !fileExists(path);
        } else {
            char* line = g_strdup_printf("%s\n", values[i]);
            ok = writeAtomically(path, line);
            g_free(line);
        }
        g_free(path);
        if (ok)
            *changed |= s->scope ? s->scope : 0;
        g_string_append_printf(ok ? done : failed, "%s%s", (ok ? done : failed)->len ? ", " : "", s->label);
    }
    if (envDone->len) {
        char* path = pathFor("child.env");
        gboolean ok = writeAtomically(path, newEnv);
        g_free(path);
        if (ok)
            *changed |= envScopes;
        g_string_append_printf(ok ? done : failed, "%s%s", (ok ? done : failed)->len ? ", " : "", envDone->str);
    }
    g_string_free(envDone, TRUE);
    g_free(env);
    g_free(newEnv);
    g_mutex_unlock(&lock);
    for (size_t i = 0; i < SETTING_COUNT; i++)
        g_free(values[i]);

    GString* text = g_string_new(NULL);
    if (done->len)
        g_string_append_printf(text, "Saved: %s.", done->str);
    else if (!failed->len)
        g_string_append(text, "No change.");
    if (failed->len)
        g_string_append_printf(text, "%sCould not save: %s.", text->len ? " " : "", failed->str);
    if (*changed & PS5WK_SETTINGS_PAGE)
        g_string_append(text, " Restart the page to apply.");
    if (*changed & PS5WK_SETTINGS_BROWSER)
        g_string_append(text, " Restart the browser to apply.");
    *message = g_string_free(text, FALSE);
    gboolean ok = !failed->len;
    g_string_free(done, TRUE);
    g_string_free(failed, TRUE);
    return ok;
}

/* The search engines of the "search" setting: what goes before the words
 * (escaped). Every choice in the table has one; an unknown value (a file
 * edited by hand) searches with the first. */
static const struct {
    const char* id;
    const char* url;
} searchEngines[] = {
    { "google", "https://www.google.com/search?q=" },
    { "bing", "https://www.bing.com/search?q=" },
    { "duckduckgo", "https://duckduckgo.com/?q=" },
    { "ddglite", "https://lite.duckduckgo.com/lite/?q=" },
    { "brave", "https://search.brave.com/search?q=" },
    { "ecosia", "https://www.ecosia.org/search?q=" },
    { "startpage", "https://www.startpage.com/do/search?q=" },
};

const char* ps5wk_settings_search_url(void)
{
    const char* url = searchEngines[0].url;
    g_mutex_lock(&lock);
    for (size_t i = 0; i < SETTING_COUNT; i++) {
        if (strcmp(settings[i].id, "search"))
            continue;
        char* value = currentValue(&settings[i], "");
        for (size_t e = 0; e < G_N_ELEMENTS(searchEngines); e++) {
            if (!strcmp(value, searchEngines[e].id))
                url = searchEngines[e].url;
        }
        g_free(value);
    }
    g_mutex_unlock(&lock);
    return url;
}

/* The settings page: built from /settings.json by its script (no innerHTML:
 * every value is set as text). Six sections, one shown at a time, picked
 * from a list of buttons (beside the section from 900 px wide, as on the TV;
 * above it on a phone): General, Ads & privacy, Cookies & storage,
 * Downloads, Advanced, Update & restart. A setting is one row: its name, the first sentence of its
 * help (More shows the rest), what it needs to apply, and its buttons. The
 * status line and the restart banner stay in view (beside the section, or
 * stuck to the top).
 * For the D-pad (spatial navigation) at 1920x1080 and 960x540: large
 * controls with a clear focus ring; every button of a row on the row's right
 * edge, so up and down go from row to row and left and right along a row;
 * Left from a row's first button goes to the section list (the script does
 * that one move itself). Buttons instead of <select> (WPE has no popup menus
 * here), and a second press to confirm what is hard to undo. Light and dark. */
const char ps5wk_settings_page[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>Browser settings</title>\n"
    "<script>if(location.protocol=='parallax:'||location.hostname=='127.0.0.1'||location.hostname=='localhost')document.documentElement.className='tv'</script>\n"
    "<style>\n"
    ":root{color-scheme:light dark;--bg:#f7f5ff;--fg:#14113d;--muted:#5a5584;--card:#fff;--line:#dcd6f7;--accent:#6d0efd;--on:#fff;--warn:#a04b00;--focus:#d10aa6}\n"
    "@media(prefers-color-scheme:dark){:root{--bg:#07061f;--fg:#f4f2ff;--muted:#b9b5e0;--card:#110f35;--line:#2b2766;--accent:#a77bff;--on:#01001a;--warn:#ffb648;--focus:#ff5cd6}}\n"
    ":root.tv{color-scheme:dark;--bg:#07061f;--fg:#f4f2ff;--muted:#b9b5e0;--card:#110f35;--line:#2b2766;--accent:#a77bff;--on:#01001a;--warn:#ffb648;--focus:#ff5cd6}\n"
    "*{box-sizing:border-box}\n"
    "[hidden]{display:none!important}\n"
    "html{font-size:clamp(16px,1.25vw,26px)}\n"
    "body{margin:0;background:var(--bg);color:var(--fg);font:1rem/1.45 -apple-system,system-ui,sans-serif}\n"
    "::selection{background:var(--accent);color:var(--on)}\n"
    "main{max-width:62rem;margin:0 auto;padding:1rem 16px 3rem}\n"
    "header{display:flex;align-items:center;gap:.8rem;flex-wrap:wrap;margin-bottom:.9rem}\n"
    "header img{width:2.6rem;height:2.6rem;margin:-.2rem}\n"
    "h1{font-size:1.6rem;margin:0;flex:1}\n"
    "h1 span{display:block;font-size:.8rem;font-weight:600;color:var(--muted);letter-spacing:.02em}\n"
    "h2{font-size:.9rem;margin:1.5rem 0 .4rem;color:var(--muted);text-transform:uppercase;letter-spacing:.06em}\n"
    "#side{display:contents}\n"
    "nav{display:flex;flex-wrap:wrap;gap:.45rem;margin-bottom:.5rem}\n"
    "nav button{text-align:left}\n"
    "nav button[aria-current=true]{border-color:var(--accent);background:color-mix(in srgb,var(--accent) 22%,transparent);font-weight:600}\n"
    "nav .badge{margin-left:.5rem;border-color:var(--accent);color:var(--accent);font-weight:600}\n"
    "#live{position:sticky;top:0;z-index:2;background:var(--bg)}\n"
    "#status{margin:0;padding:.35rem 0;font-weight:600}\n"
    "#status:empty{display:none}\n"
    "#status.new{animation:said 1.4s ease-out}\n"
    "@keyframes said{from{color:var(--accent)}}\n"
    "@media(prefers-reduced-motion:reduce){#status.new{animation:none}}\n"
    "#banner{border:2px solid var(--warn);border-radius:.8rem;padding:.6rem .8rem;margin:.2rem 0 .6rem;background:var(--card)}\n"
    "#banner p{margin:0 0 .5rem}\n"
    ".panel{background:var(--card);border:1px solid var(--line);border-radius:.8rem;padding:0 1rem;margin:.5rem 0}\n"
    ".panel.pad{padding:.9rem 1rem}\n"
    ".pad>p:first-child{margin-top:0}\n"
    ".row{display:flex;flex-wrap:wrap;align-items:center;gap:.5rem 1.2rem;padding:.8rem 0;border-bottom:1px solid var(--line)}\n"
    ".panel>.row:last-child,.panel>.rows:last-child>.row:last-child{border-bottom:0}\n"
    ".txt{flex:1 1 20rem;min-width:0}\n"
    ".name{display:flex;flex-wrap:wrap;align-items:baseline;gap:.2rem .6rem;font-weight:600;font-size:1.08rem}\n"
    ".badge{font-size:.8rem;font-weight:400;padding:.05rem .55rem;border-radius:1rem;border:1px solid var(--line);color:var(--muted);white-space:nowrap}\n"
    ".badge.wait{border-color:var(--warn);color:var(--warn);font-weight:600}\n"
    ".help{color:var(--muted);margin:.2rem 0 0;font-size:.94rem;max-width:42rem}\n"
    ".pad .help{margin:.3rem 0 .6rem}\n"
    ".pad>:last-child{margin-bottom:0}\n"
    ".panel>.pad{padding:.8rem 0}\n"
    ".row.overridden .seg{opacity:.55}\n"
    ".seg{display:flex;flex-wrap:wrap;align-items:center;gap:.45rem;max-width:100%}\n"
    ".wide{flex:1 1 100%}\n"
    ".seg:empty{display:none}\n"
    "button,input{font:inherit;color:inherit}\n"
    "button{min-height:2.5rem;padding:.4rem .95rem;border-radius:.6rem;border:2px solid var(--line);background:transparent;cursor:pointer}\n"
    "button[aria-pressed=true]{background:var(--accent);border-color:var(--accent);color:var(--on);font-weight:600}\n"
    "button.primary{background:var(--accent);border-color:var(--accent);color:var(--on);font-weight:600}\n"
    "button.danger{border-color:var(--warn);color:var(--warn)}\n"
    "button.armed{background:var(--warn);border-color:var(--warn);color:var(--on)}\n"
    "button:disabled{opacity:.5;cursor:default}\n"
    "button.more{order:1;margin-left:auto;min-height:1.8rem;padding:0 .7rem;border-width:1px;border-radius:1rem;font-size:.85rem;color:var(--accent)}\n"
    "button:focus,input:focus{outline:5px solid var(--focus);outline-offset:3px}\n"
    "input{flex:1;min-width:10rem;min-height:2.5rem;padding:.4rem .8rem;border:2px solid var(--line);border-radius:.6rem;background:var(--bg);caret-color:var(--accent)}\n"
    "#phone svg{flex:none;width:11rem;height:11rem;border-radius:.6rem}\n"
    "#phone code{display:block;margin-top:.3rem;font-size:1.15rem;font-weight:600;color:var(--fg)}\n"
    ".note{color:var(--muted);font-size:.94rem;max-width:42rem}\n"
    "code{word-break:break-all}\n"
    ".sizes{width:100%;border-collapse:collapse;margin:.2rem 0 .8rem}.sizes td{padding:.3rem 0;border-bottom:1px solid var(--line)}\n"
    ".sizes td:last-child{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap;padding-left:1rem}.sizes tr.total td{font-weight:600;border-bottom:0}\n"
    "@media(min-width:900px){\n"
    "#app{display:grid;grid-template-columns:14rem minmax(0,1fr);gap:0 1.6rem;align-items:start}\n"
    "#side{display:block;position:sticky;top:.8rem}\n"
    "nav{flex-direction:column;flex-wrap:nowrap}\n"
    "#live{position:static}\n"
    "#banner button{flex:1 1 100%}\n"
    ".row>.seg{margin-left:auto;justify-content:flex-end}\n"
    "button.more{order:0;margin-left:0}\n"
    ".row>.seg.wide{margin-left:0;justify-content:flex-start}\n"
    ".seg.wide>button.more{order:1;margin-left:auto}\n"
    "#content>section>:first-child{margin-top:0}\n"
    "}\n"
    "</style></head><body><main>\n"
    "<header><img src=\"/logo.png\" alt=\"\" onerror=\"this.remove()\"><h1><span>Parallax</span>Browser settings</h1><button id=\"back\" hidden>Back to the page</button></header>\n"
    "<div id=\"pinbox\" class=\"panel pad\" hidden><p>This PS5 asks for the link server's PIN.</p><div class=\"seg\"><input id=\"pin\" inputmode=\"numeric\" autocomplete=\"off\" placeholder=\"PIN\"><button id=\"pinok\" class=\"primary\">OK</button></div></div>\n"
    "<div id=\"app\"><div id=\"side\">\n"
    "<nav id=\"nav\" aria-label=\"Sections\" hidden>\n"
    "<button id=\"n-general\">General</button>\n"
    "<button id=\"n-ads\">Ads &amp; privacy</button>\n"
    "<button id=\"n-storage\">Cookies &amp; storage</button>\n"
    "<button id=\"n-downloads\">Downloads</button>\n"
    "<button id=\"n-advanced\">Advanced</button>\n"
    "<button id=\"n-system\">Update &amp; restart<span class=\"badge\" id=\"upBadge\" hidden></span></button>\n"
    "</nav>\n"
    "<div id=\"live\"><p id=\"status\" role=\"status\">Loading…</p>\n"
    "<div id=\"banner\" hidden><p id=\"bannerText\"></p><div class=\"seg\"><button id=\"bannerPage\" class=\"primary\" hidden>Restart page</button><button id=\"bannerBrowser\" class=\"primary\" hidden>Restart browser</button></div></div></div>\n"
    "</div><div id=\"content\" hidden>\n"
    "<section id=\"t-general\" aria-labelledby=\"n-general\">\n"
    "<div class=\"panel\"><div class=\"rows\" data-group=\"general\"></div>\n"
    "<div class=\"row\"><div class=\"txt\"><div class=\"name\">Gamepad mode</div><p class=\"help\">Gives a game the whole controller, with keys on its buttons for keyboard games. Hold △ on the page to turn it on, Options to leave it.</p></div>\n"
    "<div class=\"seg\"><button id=\"gpOpen\" onclick=\"location.href=(location.protocol=='parallax:'?'parallax://gamepad':'/gamepad')+(pin?'?pin='+encodeURIComponent(pin):'')\">Button mapping</button></div></div></div>\n"
    "<h2>Phone and computer</h2>\n"
    "<div class=\"panel\"><div id=\"phone\" class=\"row\" hidden></div><div class=\"rows\" data-group=\"link\"></div></div>\n"
    "</section>\n"
    "<section id=\"t-ads\" aria-labelledby=\"n-ads\" hidden>\n"
    "<div class=\"panel\">\n"
    "<div class=\"row\" id=\"abSite\" hidden><div class=\"txt\"><div class=\"name\"><span id=\"abSiteName\"></span><span class=\"badge\" id=\"abSiteBadge\"></span></div><p class=\"help\" id=\"abSiteHelp\"></p></div><div class=\"seg\"><button id=\"abSiteButton\" class=\"primary\"></button></div></div>\n"
    "<div class=\"rows\" data-group=\"ads\"></div>\n"
    "<div class=\"row\"><div class=\"txt\"><div class=\"name\">Sites where ads are allowed</div><p class=\"help\" id=\"abSitesHelp\"></p></div><div class=\"seg wide\" id=\"abSites\"></div></div>\n"
    "<div class=\"row\"><div class=\"txt\"><div class=\"name\">Filter lists</div><p class=\"help\" id=\"abStatus\"></p><p class=\"help\" id=\"abList\" hidden></p></div><div class=\"seg\"><button id=\"abMore\" class=\"more\"></button><button id=\"abUpdate\">Update lists now</button></div></div>\n"
    "</div>\n"
    "<h2>Pop-ups</h2>\n"
    "<div class=\"panel\">\n"
    "<div class=\"row\" id=\"popSite\" hidden><div class=\"txt\"><div class=\"name\"><span id=\"popSiteName\"></span><span class=\"badge\" id=\"popSiteBadge\"></span></div><p class=\"help\" id=\"popSiteHelp\"></p></div><div class=\"seg\"><button id=\"popSiteButton\" class=\"primary\"></button></div></div>\n"
    "<div class=\"row\"><div class=\"txt\"><div class=\"name\">Sites where pop-ups are allowed</div><p class=\"help\" id=\"popSitesHelp\"></p></div><div class=\"seg wide\" id=\"popSites\"></div></div>\n"
    "</div>\n"
    "<h2>Security</h2>\n"
    "<div class=\"panel\"><div class=\"rows\" data-group=\"security\"></div></div>\n"
    "</section>\n"
    "<section id=\"t-storage\" aria-labelledby=\"n-storage\" hidden>\n"
    "<div class=\"panel\" id=\"stCard\"><div class=\"row\"><div class=\"txt\"><div class=\"name\"><span id=\"stUser\"></span><span class=\"badge\" id=\"stFree\"></span></div>\n"
    "<p class=\"help\">Clearing the cache, including the files sites keep for offline use, only makes pages load more slowly the next time. Clearing cookies logs you out of every site. Clearing all website data also removes the cache, local storage and site databases. Your history, bookmarks and downloads stay.</p></div>\n"
    "<div class=\"seg\"><button id=\"clearCache\">Clear cache</button><button id=\"clearCookies\" class=\"danger\">Clear cookies</button><button id=\"clearAll\" class=\"danger\">Clear all website data</button></div></div>\n"
    "<div class=\"pad\"><p class=\"help\">Each PS5 user has their own cookies, cache, history, bookmarks, tabs and downloads.</p>\n"
    "<table class=\"sizes\" id=\"stMine\"></table><p class=\"help\">On this PS5, shared by every user:</p><table class=\"sizes\" id=\"stShared\"></table></div></div>\n"
    "</section>\n"
    "<section id=\"t-downloads\" aria-labelledby=\"n-downloads\" hidden>\n"
    "<div id=\"dlCard\"><div class=\"panel\">\n"
    "<div class=\"row\"><div class=\"txt\"><div class=\"name\">Download folder <span class=\"badge\" id=\"dlFree\"></span></div><p class=\"help\"><b id=\"dlWhere\"></b> <code id=\"dlFolder\"></code></p><p class=\"help\" id=\"dlPhone\" hidden>You can change it in Settings on the PS5.</p></div>\n"
    "<div class=\"seg\"><button id=\"dlPick\">Change…</button><button id=\"dlUser\">Use my Downloads folder</button></div></div>\n"
    "<div class=\"row\"><div class=\"txt\"><div class=\"name\">Ask where to save</div><p class=\"help\">Choose a folder each time you start a download.</p></div><div class=\"seg\" id=\"dlAsk\"></div></div>\n"
    "<div class=\"row\"><div class=\"txt\"><div class=\"name\">Connections per download</div><p class=\"help\">More connections make downloads faster from sites that slow each one down. Auto starts with 8 and adds more while they help, up to 32.</p></div><div class=\"seg\" id=\"dlConns\"></div></div>\n"
    "<div class=\"row\"><div class=\"txt\"><div class=\"name\">After unpacking</div><p class=\"help\">Keep the archive, or delete it to save space.</p></div><div class=\"seg\" id=\"dlAfter\"></div></div>\n"
    "<div class=\"row\"><div class=\"txt\"><div class=\"name\">Homebrew launcher <span class=\"badge\" id=\"dlFound\"></span></div><p class=\"help\" id=\"dlLaunch\"></p></div></div>\n"
    "<div class=\"row\"><div class=\"txt\"><div class=\"name\">Finished downloads</div><p class=\"help\" id=\"dlCount\"></p></div><div class=\"seg\"><button id=\"dlClear\" class=\"danger\">Delete finished downloads</button></div></div>\n"
    "<div class=\"pad\"><p class=\"help\">Your downloads are in the menu: press Options, then R1.</p></div></div></div>\n"
    "</section>\n"
    "<section id=\"t-advanced\" aria-labelledby=\"n-advanced\" hidden>\n"
    "<p class=\"note\">For fixing a site that misbehaves, or tuning speed. The defaults suit almost every site.</p>\n"
    "<div id=\"advGroups\"></div>\n"
    "</section>\n"
    "<section id=\"t-system\" aria-labelledby=\"n-system\" hidden>\n"
    "<div class=\"panel\" id=\"upCard\" hidden><div class=\"row\"><div class=\"txt\"><div class=\"name\" id=\"upVersion\"></div><p class=\"help\" id=\"upText\"></p></div>\n"
    "<div class=\"seg\"><button id=\"upCheck\">Check for updates</button><button id=\"upOpen\" class=\"primary\" hidden>Open the update</button></div></div></div>\n"
    "<div class=\"panel\"><div class=\"row\" id=\"restartPageRow\"><div class=\"txt\"><div class=\"name\">Restart page</div><p class=\"help\">The page's process starts again, with the settings that need a page restart.</p></div><div class=\"seg\"><button id=\"restartPage\">Restart page</button></div></div>\n"
    "<div class=\"row\"><div class=\"txt\"><div class=\"name\">Restart browser</div><p class=\"help\" id=\"restartHelp\"></p></div><div class=\"seg\"><button id=\"restartBrowser\">Restart browser</button></div></div></div>\n"
    "<p class=\"note\" id=\"where\"></p>\n"
    "<p class=\"note\">On the PS5: Options, then Settings (⚙) in the browser menu opens this page; O goes back. Ads on the page you came from: Ads &amp; privacy.</p>\n"
    "</section>\n"
    "</div></div>\n"
    "<script>\n"
    "var S=null,pin='',started=0,polling=0,tab='general',open={},slots={};\n"
    "try{pin=sessionStorage.getItem('ps5pin')||''}catch(e){}\n"
    "var query=new URLSearchParams(location.search);if(query.get('pin'))pin=query.get('pin');\n"
    "var onPS5=location.protocol=='parallax:'||location.hostname=='127.0.0.1'||location.hostname=='localhost';\n"
    "var TABS=['general','ads','storage','downloads','advanced','system'];\n"
    "function $(id){return document.getElementById(id)}\n"
    "function el(tag,cls,text){var e=document.createElement(tag);if(cls)e.className=cls;if(text!=null)e.textContent=text;return e}\n"
    "/* The result of every press: beside the sections on a wide screen, stuck to the top on a narrow one. */\n"
    "function say(t){var s=$('status');s.textContent=t||'';s.classList.remove('new');if(t){void s.offsetWidth;s.classList.add('new')}}\n"
    "function api(method,path,body){\n"
    "  var h={'Accept':'application/json'};if(pin)h['X-PS5-PIN']=pin;if(body)h['Content-Type']='application/json';\n"
    "  return fetch(path,{method:method,headers:h,body:body?JSON.stringify(body):undefined,cache:'no-store'})\n"
    "    .then(function(r){return r.json().then(function(j){j.code=r.status;return j},function(){return{ok:false,code:r.status,error:'HTTP '+r.status}})});\n"
    "}\n"
    "/* One section at a time: X on a section's button shows it (#advanced in the address opens on one,\n"
    "   #ads/popSiteButton on one with that button focused). */\n"
    "function show(t,keep){\n"
    "  if(TABS.indexOf(t)<0)t='general';tab=t;\n"
    "  TABS.forEach(function(x){$('t-'+x).hidden=x!=t;$('n-'+x).setAttribute('aria-current',String(x==t))});\n"
    "  if(!keep)window.scrollTo(0,0);\n"
    "  if(!onPS5)try{sessionStorage.setItem('ps5tab',t)}catch(e){}\n"
    "}\n"
    "TABS.forEach(function(x){$('n-'+x).onclick=function(){show(x)}});\n"
    "var first='first';\n"
    "(function(){var h=location.hash.slice(1).split('/'),t=h[0];if(h[1])first=h[1];if(!t&&!onPS5)try{t=sessionStorage.getItem('ps5tab')}catch(e){}show(t,1)})();\n"
    "[].forEach.call(document.querySelectorAll('[data-group]'),function(g){slots[g.getAttribute('data-group')]=g});\n"
    "/* D-pad on the PS5, inside a section. The script makes three moves itself, so that they do not depend on how\n"
    "   spatial navigation scores what is near: Up and Down go to the nearest button on the line above or below, row\n"
    "   after row, and Left from the first button of a line goes to the section list (when that is beside the\n"
    "   section). The rest is the browser's: along a line, in the section list, Up from the first line. */\n"
    "document.addEventListener('keydown',function(e){\n"
    "  var k=e.key,a=document.activeElement,down=k=='ArrowDown';\n"
    "  if(!onPS5||e.defaultPrevented||(!down&&k!='ArrowUp'&&k!='ArrowLeft')||!a||!a.closest||!a.closest('#content .row'))return;\n"
    "  var box=a.getBoundingClientRect(),n=$('n-'+tab),first=null,best=null;\n"
    "  var near=[].map.call($('t-'+tab).querySelectorAll('button,input'),function(b){return{b:b,q:b.getBoundingClientRect()}})\n"
    "    .filter(function(c){return c.b!=a&&!c.b.disabled&&c.q.width});\n"
    "  if(k=='ArrowLeft'){\n"
    "    if((a.tagName=='INPUT'&&a.value)||n.getBoundingClientRect().right>box.left)return;\n"
    "    if(near.some(function(c){return c.q.right<=box.left+1&&c.q.bottom>box.top&&c.q.top<box.bottom}))return;\n"
    "    e.preventDefault();n.focus();return;\n"
    "  }\n"
    "  near.forEach(function(c){c.d=down?c.q.top-box.bottom:box.top-c.q.bottom;if(c.d>=-1&&(!first||c.d<first.d))first=c});\n"
    "  if(!first)return;\n"
    "  near.forEach(function(c){var mid=(c.q.top+c.q.bottom)/2;if(c.d<-1||mid<first.q.top-1||mid>first.q.bottom+1)return;\n"
    "    c.gap=Math.max(0,c.q.left-box.right,box.left-c.q.right);c.edge=Math.abs(c.q.right-box.right);\n"
    "    if(!best||c.gap<best.gap||(c.gap==best.gap&&c.edge<best.edge))best=c});\n"
    "  e.preventDefault();best.b.focus();best.b.closest('.row').scrollIntoView({block:'nearest'});\n"
    "});\n"
    "function applies(s){return s.restart=='page'?'Restart page to apply':s.restart=='browser'?'Restart browser to apply':'No restart needed'}\n"
    "function options(s){\n"
    "  var o=s.type=='bool'?[{value:'1',label:'On'},{value:'0',label:'Off'}]:(s.choices||[]).slice();\n"
    "  if(!o.some(function(c){return c.value==s.value}))o.push({value:s.value,label:s.value+' (set by hand)'});\n"
    "  return o;\n"
    "}\n"
    "function arm(b,text,run){\n"
    "  if(b.dataset.armed){delete b.dataset.armed;b.classList.remove('armed');b.textContent=b.dataset.text;run();return}\n"
    "  b.dataset.text=b.textContent;b.dataset.armed='1';b.classList.add('armed');b.textContent=text;\n"
    "  setTimeout(function(){if(b.dataset.armed){delete b.dataset.armed;b.classList.remove('armed');b.textContent=b.dataset.text}},5000);\n"
    "}\n"
    "/* The first sentence of a help text is always shown; More shows the rest (and stays open over a redraw).\n"
    "   More is one of the row's buttons, so the D-pad finds it: left and right move along a row, up and down from row to row. */\n"
    "function sentences(h){var i=h.indexOf('. ');return i<0?[h,'']:[h.slice(0,i+1),h.slice(i+2)]}\n"
    "function more(b,rest,key,about){\n"
    "  function sync(){var o=!!open[key];rest.hidden=!o;b.textContent=o?'Less':'More';b.setAttribute('aria-label',(o?'Less about ':'More about ')+about);b.setAttribute('aria-expanded',String(o))}\n"
    "  b.onclick=function(){open[key]=!open[key];sync()};sync();\n"
    "}\n"
    "function row(s){\n"
    "  var r=el('div','row'+(s.overridden?' overridden':'')),t=el('div','txt'),n=el('div','name',s.label),h=sentences(s.help),p=el('p','help',h[0]);\n"
    "  r.id='row-'+s.id;\n"
    "  if(s.overridden)n.appendChild(el('span','badge','Off while Safer browsing is on'));\n"
    "  if(s.pending)n.appendChild(el('span','badge wait','Changed: '+applies(s).toLowerCase()));\n"
    "  else if(s.restart!='none')p.appendChild(el('span',null,s.restart=='page'?' Needs a page restart.':' Needs a browser restart.'));\n"
    "  if(s.type!='text'&&s.value!=s['default'])n.appendChild(el('span','badge','Default: '+(options(s).filter(function(c){return c.value==s['default']})[0]||{label:s['default']}).label));\n"
    "  var seg=el('div',s.type=='text'||options(s).length>4?'seg wide':'seg');\n"
    "  t.appendChild(n);t.appendChild(p);\n"
    "  if(h[1]){var rest=el('p','help',h[1]),m=el('button','more');m.id='m-'+s.id;seg.appendChild(m);t.appendChild(rest);more(m,rest,s.id,s.label)}\n"
    "  r.appendChild(t);r.appendChild(seg);\n"
    "  if(s.type=='text'){\n"
    "    var input=el('input');input.id='in-'+s.id;input.value=s.value;input.placeholder='Built-in start page';\n"
    "    input.setAttribute('aria-label',s.label);input.setAttribute('autocapitalize','off');input.setAttribute('spellcheck','false');input.setAttribute('inputmode','url');\n"
    "    var save=el('button','primary','Save');save.id='b-'+s.id+'-save';\n"
    "    save.onclick=function(){set(s,input.value,save.id)};\n"
    "    input.onkeydown=function(e){if(e.key=='Enter')set(s,input.value,save.id)};\n"
    "    seg.appendChild(input);seg.appendChild(save);\n"
    "    if(s.value){var reset=el('button',null,'Use the start page');reset.id='b-'+s.id+'-reset';\n"
    "      reset.onclick=function(){set(s,'',save.id)};seg.appendChild(reset)}\n"
    "  }else options(s).forEach(function(o){\n"
    "    var b=el('button',null,o.label);b.id='b-'+s.id+'-'+o.value;b.setAttribute('aria-pressed',String(o.value==s.value));\n"
    "    b.onclick=function(){\n"
    "      if(s.id=='link_server'&&o.value=='0'&&s.value!='0'&&!onPS5)\n"
    "        arm(b,'Press again: this page then opens only on the PS5',function(){set(s,o.value,b.id)});\n"
    "      else set(s,o.value,b.id);\n"
    "    };\n"
    "    seg.appendChild(b);\n"
    "  });\n"
    "  return r;\n"
    "}\n"
    "/* Each setting goes to its group's place in a section; a group with no place gets a heading under Advanced. */\n"
    "function render(focus){\n"
    "  var adv=$('advGroups'),made={},k;adv.textContent='';for(k in slots)slots[k].textContent='';\n"
    "  S.settings.forEach(function(s){\n"
    "    var box=slots[s.group]||made[s.group];\n"
    "    if(!box){adv.appendChild(el('h2',null,S.groups[s.group]||s.group));box=made[s.group]=el('div','panel');adv.appendChild(box)}\n"
    "    box.appendChild(row(s));\n"
    "  });\n"
    "  var p=S.pending.page,w=S.pending.browser;\n"
    "  $('banner').hidden=!p&&!w;$('bannerPage').hidden=!p;$('bannerBrowser').hidden=!w;\n"
    "  $('bannerText').textContent=p&&w?'Changes wait for a restart. Restart browser applies them all.':p?'Changes wait for a page restart.':'Changes wait for a browser restart.';\n"
    "  $('restartPageRow').hidden=!!S.pagesInBrowser;\n"
    "  $('restartHelp').textContent=(S.pagesInBrowser?'Pages run in the browser process, so every setting that needs a restart applies when the browser restarts. ':'')+'The browser closes and opens again on the page you were on.';\n"
    "  $('nav').hidden=$('content').hidden=false;$('back').hidden=!onPS5;renderAdblock();renderPopups();renderPhone();renderDownloads();renderStorage();renderUpdate();\n"
    "  $('where').textContent=S.uri?'The PS5 is showing '+S.uri:'';\n"
    "  var f=focus=='first'?$('n-'+tab):focus&&$(focus);if(focus&&(!f||!f.getClientRects().length))f=$('n-'+tab);if(f)f.focus();\n"
    "}\n"
    "function load(focus){\n"
    "  return api('GET','/settings.json').then(function(j){\n"
    "    if(j.code==403){$('pinbox').hidden=false;say(j.error||'PIN needed');$('pin').focus();return}\n"
    "    if(!j.ok){say(j.error||'Error');return}\n"
    "    $('pinbox').hidden=true;S=j;started=j.started;render(focus);say(j.status);\n"
    "  },function(){say('The PS5 did not answer. Is the browser running?')});\n"
    "}\n"
    "function set(s,value,focus){\n"
    "  var body={};body[s.id]=value;say('Saving…');\n"
    "  api('POST','/settings',body).then(function(j){\n"
    "    if(j.settings){S=j;render(focus)}\n"
    "    say(j.message||j.error);\n"
    "  },function(){say('The PS5 did not answer.')});\n"
    "}\n"
    "function action(path,text,after){\n"
    "  say(text);\n"
    "  return api('POST',path).then(function(j){say(j.message||j.error);if(j.ok&&after)after()},function(){say('The PS5 did not answer.')});\n"
    "}\n"
    "function waitForRestart(){\n"
    "  if(polling)return;polling=1;var tries=0;\n"
    "  (function poll(){\n"
    "    api('GET','/settings.json').then(function(j){\n"
    "      if(j.ok&&j.started!=started){polling=0;S=j;started=j.started;render();say('The browser has restarted.');return}\n"
    "      if(++tries<45)setTimeout(poll,2000);else{polling=0;say('The browser has not come back yet. Reload this page later.')}\n"
    "    },function(){if(++tries<45)setTimeout(poll,2000);else polling=0});\n"
    "  })();\n"
    "}\n"
    "function restartPage(){action('/restart?scope=page','Restarting the page…',function(){if(!onPS5)setTimeout(function(){load()},2500)})}\n"
    "function restartBrowser(b){var n=S&&S.downloads?S.downloads.active:0;arm(b,n?n+(n==1?' download in progress; it will pause. Press again':' downloads in progress; they will pause. Press again'):'Press again to restart the browser',function(){\n"
    "  action('/restart?scope=browser','Restarting the browser…',function(){say('Restarting the browser. This page reconnects when it is back (about 20 s).');if(!onPS5)setTimeout(waitForRestart,3000)})})}\n"
    "$('restartPage').onclick=restartPage;$('bannerPage').onclick=restartPage;\n"
    "$('restartBrowser').onclick=function(){restartBrowser(this)};$('bannerBrowser').onclick=function(){restartBrowser(this)};\n"
    "function clearData(what,text,id){var from=S.statusSerial,n=0;action('/clear?what='+what,text,function(){\n"
    "  (function poll(){api('GET','/settings.json').then(function(j){if(j.ok&&j.statusSerial!=from||++n>=80){load(id);return}setTimeout(poll,1500)},function(){if(++n<80)setTimeout(poll,1500)})})()})}\n"
    "$('clearCookies').onclick=function(){arm(this,'Press again: log out of every site',function(){clearData('cookies','Clearing cookies…','clearCookies')})};\n"
    "$('clearAll').onclick=function(){arm(this,'Press again: clear all website data',function(){clearData('all','Clearing website data…','clearAll')})};\n"
    "$('clearCache').onclick=function(){clearData('cache','Clearing the cache…','clearCache')};\n"
    "function size(b){return b<0?'Counting…':b>=1e9?(b/1e9).toFixed(1)+' GB':b>=1e6?(b/1e6).toFixed(b>=1e8?0:1)+' MB':b>=1e3?Math.round(b/1e3)+' KB':b>0?b+' bytes':'Nothing'}\n"
    "function sizeRows(t,items,total){t.textContent='';var sum=0;items.forEach(function(i){var r=el('tr');r.appendChild(el('td',null,i.label));r.appendChild(el('td',null,size(i.bytes)));t.appendChild(r);sum=sum<0||i.bytes<0?-1:sum+i.bytes});\n"
    "  if(total){var r=el('tr','total');r.appendChild(el('td',null,total));r.appendChild(el('td',null,size(sum)));t.appendChild(r)}}\n"
    "function renderStorage(){var s=S.storage;$('stCard').hidden=!s;if(!s)return;$('stUser').textContent=s.user;\n"
    "  $('stFree').textContent=s.free>=0?gb(s.free)+' free on the PS5':'free space unknown';\n"
    "  sizeRows($('stMine'),s.items,'Total for '+s.user);\n"
    "  sizeRows($('stShared'),s.shared.filter(function(i){return i.id!='others'||s.otherUsers}).map(function(i){return i.id=='others'?{label:s.otherUsers==1?'1 other user':s.otherUsers+' other users',bytes:i.bytes}:i}));\n"
    "  var c=s.items.filter(function(i){return i.id=='cache'})[0];$('clearCache').textContent=c&&c.bytes>0?'Clear cache ('+size(c.bytes)+')':'Clear cache';\n"
    "  if(s.counting)pollStorage()}\n"
    "var stPolls=0,stTimer=0;\n"
    "function pollStorage(){if(stTimer||++stPolls>20)return;stTimer=setTimeout(function(){stTimer=0;api('GET','/settings.json').then(function(j){if(!j.ok)return;S.storage=j.storage;if(!j.storage.counting)stPolls=0;renderStorage()})},1000)}\n"
    "function gb(b){return b>=1073741824?(b/1073741824).toFixed(1)+' GB':Math.round(b/1048576)+' MB'}\n"
    "function dlPost(o,done){var h={'Accept':'application/json','Content-Type':'application/x-www-form-urlencoded'};if(pin)h['X-PS5-PIN']=pin;\n"
    "  fetch('/dlm',{method:'POST',headers:h,cache:'no-store',body:Object.keys(o).map(function(k){return k+'='+encodeURIComponent(o[k])}).join('&')})\n"
    "    .then(function(r){return r.json()}).then(function(j){if(!j.ok){say(j.error||'That didn’t work');return}done()},function(){say('The PS5 did not answer.')})}\n"
    "function dlReload(then){api('GET','/settings.json').then(function(k){if(k.ok){S=k;renderDownloads();if(then)then()}})}\n"
    "function dlSet(o,focus){o.do='settings';say('Saving…');dlPost(o,function(){say('Saved.');dlReload(function(){var f=focus&&$(focus);if(f)f.focus()})})}\n"
    "function dlSeg(box,list,current,key){box.textContent='';list.forEach(function(o){var b=el('button',null,o[1]);b.id=box.id+'-'+o[0];b.setAttribute('aria-pressed',String(o[0]===current));\n"
    "  b.onclick=function(){var v={};v[key]=o[0]===true?1:o[0]===false?0:o[0];dlSet(v,b.id)};box.appendChild(b)})}\n"
    "/* Change…: the menu's Files list chooses the folder; this page shows it once it's saved. */\n"
    "$('dlPick').onclick=function(){var was=S.downloads&&S.downloads.defaultFolder,n=0;dlPost({do:'pickfolder'},function(){say('Open the folder in Files, then press X on its top row.');\n"
    "  (function poll(){setTimeout(function(){api('GET','/settings.json').then(function(k){if(!k.ok)return;if(k.downloads&&k.downloads.defaultFolder!=was){S=k;renderDownloads();say('New downloads go to '+k.downloads.defaultName+'.')}else if(++n<90)poll()})},2000)})()})};\n"
    "var LAUNCH={'ShadowMountPlus':'Apps you install appear on the home screen when you go back to it.','TITAN-CORE':'Apps you install appear on the home screen when you go back to it.',\n"
    "  'Kraken Mount':'Apps you install appear on the home screen after you close the browser.'};\n"
    "function renderDownloads(){var d=S.downloads;$('dlCard').hidden=!d;if(!d)return;$('dlFolder').textContent=d.defaultFolder||d.folder;$('dlWhere').textContent=d.defaultName||'Downloads';\n"
    "  $('dlPick').hidden=!onPS5;$('dlPhone').hidden=onPS5;\n"
    "  $('dlUser').hidden=!d.defaultFolder||d.defaultFolder==d.folder;$('dlUser').onclick=function(){dlSet({folder:d.folder},'dlPick')};\n"
    "  if(d.connections!=null){dlSeg($('dlAsk'),[[true,'Ask'],[false,'Don’t ask']],d.ask,'ask');dlSeg($('dlConns'),[[0,'Auto'],[1,'1'],[2,'2'],[4,'4'],[8,'8'],[12,'12'],[16,'16']],d.connections,'connections');\n"
    "    dlSeg($('dlAfter'),[[false,'Keep it'],[true,'Delete it']],d.deleteArchives,'deleteArchives')}\n"
    "  var f=d.launcherFound;$('dlFound').textContent=f==null?'Looking…':f||'None';\n"
    "  $('dlLaunch').textContent=f==null?'':f?(LAUNCH[f]||''):'Apps you install appear on the home screen, and start after you restart the PS5 and run the jailbreak.';\n"
    "  if(f==null&&(window.lfTries=(window.lfTries||0)+1)<6)setTimeout(dlReload,1500);\n"
    "  $('dlFree').textContent=d.free>=0?gb(d.free)+' free':'';\n"
    "  $('dlCount').textContent=(d.finished?d.finished+' finished':'None')+(d.active?', '+d.active+' in progress':'')+'.';\n"
    "  var c=$('dlClear');c.disabled=!d.finished;c.textContent=d.finished?'Delete finished downloads ('+d.finished+')':'Delete finished downloads'}\n"
    "$('dlClear').onclick=function(){var n=S.downloads.finished;arm(this,'Press again: delete '+n+' file'+(n==1?'':'s'),function(){action('/clear?what=downloads','Deleting…',function(){setTimeout(function(){load('dlClear')},1000)})})};\n"
    "/* Closes this browser page tab, back to the page Settings was opened over; else back. */\n"
    "function goBack(){if(history.length>1)history.back();else if(S&&S.returnUri)location.href=S.returnUri}\n"
    "$('back').onclick=function(){var h={'Content-Type':'application/x-www-form-urlencoded'};if(pin)h['X-PS5-PIN']=pin;\n"
    "  fetch('/browser/page',{method:'POST',headers:h,body:'action=close',cache:'no-store'}).then(function(r){if(!r.ok)goBack()}).catch(goBack)};\n"
    "$('pinok').onclick=function(){pin=$('pin').value.trim();try{sessionStorage.setItem('ps5pin',pin)}catch(e){}load()};\n"
    "$('pin').onkeydown=function(e){if(e.key=='Enter')$('pinok').click()};\n"
    "function when(t){return t?new Date(t*1000).toLocaleString():'never'}\n"
    "/* On the PS5: the link server's address now, as a QR code for a phone's camera (dark on white). */\n"
    "function qrSVG(rows){\n"
    "  var n=rows.length,q=4,ns='http://www.w3.org/2000/svg',svg=document.createElementNS(ns,'svg'),d='';\n"
    "  svg.setAttribute('viewBox','0 0 '+(n+2*q)+' '+(n+2*q));svg.setAttribute('shape-rendering','crispEdges');svg.setAttribute('role','img');\n"
    "  rows.forEach(function(r,y){for(var x=0;x<n;x++)if(r[x]=='1'){var e=x;while(e<n&&r[e]=='1')e++;d+='M'+(x+q)+','+(y+q)+'h'+(e-x)+'v1h-'+(e-x)+'z';x=e}});\n"
    "  var bg=document.createElementNS(ns,'rect');bg.setAttribute('width','100%');bg.setAttribute('height','100%');bg.setAttribute('fill','#fff');\n"
    "  var p=document.createElementNS(ns,'path');p.setAttribute('d',d);p.setAttribute('fill','#14113d');svg.appendChild(bg);svg.appendChild(p);return svg;\n"
    "}\n"
    "function renderPhone(){\n"
    "  var box=$('phone'),l=S.link;box.textContent='';box.hidden=!onPS5||!l;if(box.hidden)return;\n"
    "  var t=el('div','txt');t.appendChild(el('div','name','Send links from your phone'));\n"
    "  if(l.url&&l.qr){box.appendChild(qrSVG(l.qr));t.appendChild(el('p','help','Scan this with your phone\\'s camera, or open'));t.appendChild(el('code',null,l.url));\n"
    "    if(l.pin)t.appendChild(el('p','help','The page asks for the link server\\'s PIN.'))}\n"
    "  else t.appendChild(el('p','help',l.localOnly?'The link server is set to this PS5 only, so a phone can\\'t reach it. Turn on Link server on the network below, then restart the browser.':'This PS5 has no network address right now.'));\n"
    "  box.appendChild(t);\n"
    "}\n"
    "function renderUpdate(){var u=S.update,n=$('upBadge');$('upCard').hidden=!u;n.hidden=!u||!(u.state=='available'||u.state=='downloading'||u.state=='ready');if(!u)return;\n"
    "  n.textContent=u.latest;$('upVersion').textContent='Parallax Browser '+u.running;\n"
    "  var t={'checking':'Looking for a newer version…','up-to-date':u.refused?'Version '+u.latest+' was refused ('+u.refused+'): the browser offers the next one.':'Up to date.','available':u.managed?'Version '+u.latest+' is available: ProsperoStore installed the browser, so update it there.':'Version '+u.latest+' is available.',\n"
    "    'downloading':'Downloading '+u.latest+(u.total?': '+Math.floor(u.done*100/u.total)+'%':'')+'…','ready':u.latest+' is ready: it installs when the browser next opens, or now from the update page.','installing':'Updating…',\n"
    "    'failed':(u.refused?'Version '+u.latest+' was refused: ':u.retryDownload?'The download failed: ':'Couldn’t check: ')+(u.error||'')}[u.state]||'';\n"
    "  if(u.checkedAt&&(u.state=='up-to-date'||u.state=='available'))t+=' Checked '+new Date(u.checkedAt*1000).toLocaleString()+'.';\n"
    "  if(u.lastInstall)t=u.lastInstall+'. '+t;$('upText').textContent=t+' The browser looks for a newer version each time it opens.';\n"
    "  $('upCheck').disabled=u.state=='checking'||u.state=='downloading'||u.state=='installing'||u.state=='ready';$('upOpen').hidden=!(u.state=='available'||u.state=='downloading'||u.state=='ready'||(u.state=='failed'&&u.retryDownload));\n"
    "}\n"
    "function pollUpdate(){var n=0;(function p(){api('GET','/settings.json').then(function(j){if(!j.ok&&!j.settings)return;S=j;renderUpdate();var s=j.update&&j.update.state;if((s=='checking'||s=='downloading')&&++n<600)setTimeout(p,1500)})})()}\n"
    "$('upCheck').onclick=function(){api('POST','/update?do=check').then(function(j){say(j.ok?'Looking for a newer version…':j.error);setTimeout(pollUpdate,800)},function(){say('The PS5 did not answer.')})};\n"
    "$('upOpen').onclick=function(){location.href=(location.protocol=='parallax:'?'parallax://update':'/update')+(pin&&location.protocol!='parallax:'?'?pin='+encodeURIComponent(pin):'')};\n"
    "function renderAdblock(){\n"
    "  var a=S.adblock;if(!a)return;\n"
    "  $('abSite').hidden=!a.site;\n"
    "  if(a.site){\n"
    "    var allowed=!!a.siteAllowedBy,b=$('abSiteButton');\n"
    "    $('abSiteName').textContent=a.site;\n"
    "    $('abSiteBadge').textContent=!a.enabled?'Ad blocking is off':allowed?'Ads allowed':'Ads blocked';\n"
    "    $('abSiteHelp').textContent=allowed?'Ad blocking is off on '+a.siteAllowedBy+' and its subdomains.':'If this site does not work with ad blocking, allow ads on it (and its subdomains). The page reloads.';\n"
    "    b.textContent=allowed?'Block ads on '+a.siteAllowedBy+' again':'Allow ads on '+a.site;\n"
    "    b.onclick=function(){adblock(allowed?'block':'allow',allowed?a.siteAllowedBy:a.site,'abSiteButton')};\n"
    "  }\n"
    "  var list=$('abSites');list.textContent='';\n"
    "  a.sites.forEach(function(s,i){var b=el('button',null,'Remove '+s);b.id='ab-rm-'+i;b.onclick=function(){adblock('block',s,a.sites.length>1?'ab-rm-'+(i?i-1:0):'abUpdate')};list.appendChild(b)});\n"
    "  $('abSitesHelp').textContent=a.sites.length?'Remove a site to block ads there again.':'None yet. To allow ads on a site, open Settings (⚙) from its page: the site is then the first row here.';\n"
    "  var l=a.list,d=l.version?'uBlock Origin Lite '+l.version+': '+l.rules+' rules, '+(l.origin=='update'?'downloaded '+when(l.generated):'bundled with the browser')+'. Last check for a new list: '+when(a.lastCheck)+' (once a day by itself).'+(a.scripting&&a.scripting.version?' Cosmetic filters for '+a.scripting.hosts+' site entries, '+(a.scripting.calls+a.scripting.isolatedCalls)+' scriptlet calls'+(a.scripting.site?'; on '+a.scripting.site+': '+(a.scripting.siteCalls+a.scripting.siteIsolatedCalls)+' scriptlets, '+a.scripting.siteCSS+' hiding rules'+(a.scripting.siteProcedurals?', '+a.scripting.siteProcedurals+' procedural':''):'')+'.':''):'No list yet.';\n"
    "  $('abStatus').textContent=a.status||sentences(d)[0];$('abList').textContent=d;\n"
    "  var u=$('abUpdate');u.disabled=a.updating||a.compiling;u.textContent=a.updating?'Checking…':a.compiling?'Compiling…':'Update lists now';\n"
    "}\n"
    "more($('abMore'),$('abList'),'abList','the filter lists');\n"
    "function pollAdblock(){var n=0;(function p(){api('GET','/settings.json').then(function(j){if(!j.ok)return;S=j;renderAdblock();if((j.adblock.updating||j.adblock.compiling)&&++n<150)setTimeout(p,2000)})})()}\n"
    "function adblock(what,site,focus){\n"
    "  say(what=='update'?'Checking for a new list…':'Saving…');\n"
    "  api('POST','/adblock?do='+what+(site?'&site='+encodeURIComponent(site):'')).then(function(j){if(j.settings){S=j;render(focus)}say(j.message||j.error);if(what=='update'&&j.ok)setTimeout(pollAdblock,1500)},function(){say('The PS5 did not answer.')});\n"
    "}\n"
    "$('abUpdate').onclick=function(){adblock('update',null,'abUpdate')};\n"
    "/* Pop-ups: the site of the page Settings was opened from, and the sites allowed them (popups.c). */\n"
    "function renderPopups(){\n"
    "  var p=S.popups;if(!p)return;\n"
    "  $('popSite').hidden=!p.site;\n"
    "  if(p.site){\n"
    "    var allowed=!!p.siteAllowedBy,b=$('popSiteButton'),last=p.last&&p.last.length>90?p.last.slice(0,90)+'…':p.last;\n"
    "    $('popSiteName').textContent=p.site;\n"
    "    $('popSiteBadge').textContent=allowed?'Pop-ups allowed':p.blocked>1?p.blocked+' pop-ups blocked':p.blocked?'1 pop-up blocked':'Pop-ups blocked';\n"
    "    $('popSiteHelp').textContent=allowed?'Pages on '+p.siteAllowedBy+' and its subdomains can open new tabs by themselves.'\n"
    "      :(last?'This page tried to open '+last+'. ':'')+'If this site needs its pop-ups (a login or payment window), allow them on it and its subdomains, then try again on the page.';\n"
    "    b.textContent=allowed?'Block pop-ups on '+p.siteAllowedBy+' again':'Allow pop-ups on '+p.site;\n"
    "    b.onclick=function(){popups(allowed?'block':'allow',allowed?p.siteAllowedBy:p.site,'popSiteButton')};\n"
    "  }\n"
    "  var list=$('popSites');list.textContent='';\n"
    "  p.sites.forEach(function(s,i){var b=el('button',null,'Remove '+s);b.id='pop-rm-'+i;b.onclick=function(){popups('block',s,p.sites.length>1?'pop-rm-'+(i?i-1:0):'popSiteButton')};list.appendChild(b)});\n"
    "  $('popSitesHelp').textContent='A page that opens a new tab by itself, without a press, is blocked and the browser says so; a link you press always opens. '\n"
    "    +(p.sites.length?'Remove a site to block its pop-ups again.':'None yet. To allow a site its pop-ups, open Settings (⚙) from its page: the site is then the first row here.');\n"
    "}\n"
    "function popups(what,site,focus){\n"
    "  say('Saving…');\n"
    "  api('POST','/popups?do='+what+'&site='+encodeURIComponent(site)).then(function(j){if(j.settings){S=j;render(focus)}say(j.message||j.error)},function(){say('The PS5 did not answer.')});\n"
    "}\n"
    "load(onPS5?first:null);\n"
    "</script></main></body></html>\n";
