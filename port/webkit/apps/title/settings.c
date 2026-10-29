/*
 * settings.c: the browser's settings (README: "Settings"), for the link
 * server's /settings page, /settings.json and POST /settings (ps5browser.c).
 *
 * One table, three kinds of storage in /data/ps5webkit:
 *   child.env lines (KEY=VALUE, read by each WebProcess and NetworkProcess
 *     when it starts): rewritten with every other line (comments, JSC_*,
 *     WEBKIT_DEBUG...) kept as it was; a setting back at its default loses
 *     its line. Written to child.env.tmp, then renamed over child.env.
 *   flag files (no-hwdec, audio-silent, push-off): present = that feature
 *     off; the title reads them when it starts.
 *   home-url.txt: the page the browser opens when it starts.
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

typedef enum { KIND_ENV, KIND_FLAG, KIND_FILE } SettingKind;
typedef enum { TYPE_CHOICE, TYPE_BOOL, TYPE_NUMBER, TYPE_TEXT } SettingType;

typedef struct {
    const char* id;
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

/* Semantics and defaults: ps5wk-child.cpp (JIT, memory limit), patches
 * 0032 (painting, scrolling), 0031 (decommit), 0017 (hole punch), 0016
 * (VP9), titlevdec.c (no-hwdec), ps5browser.c (the rest). */
static const Setting settings[] = {
    { "jit", KIND_ENV, TYPE_CHOICE, "PS5WK_JIT", "1", PS5WK_SETTINGS_PAGE, "JavaScript JIT",
        "Compiles page scripts to machine code. Lower tiers or Off are slower; try them if a site crashes.",
        "1=All tiers (fastest)|dfg=Up to DFG|baseline=Baseline only|0=Off (interpreter)", 0, 0 },
    { "paint_threads", KIND_ENV, TYPE_NUMBER, "PS5WK_PAINT_THREADS", "4", PS5WK_SETTINGS_PAGE, "Painting threads",
        "CPU threads that draw the page. More is smoother on busy pages; 1 draws on one thread, as before.",
        "1=1|2=2|3=3|4=4|6=6|8=8", 1, 8 },
    { "scroll_copy", KIND_ENV, TYPE_BOOL, "PS5WK_SCROLL_COPY", "1", PS5WK_SETTINGS_PAGE, "Fast scrolling",
        "Scrolling moves the pixels already drawn instead of drawing the whole view again.", NULL, 0, 0 },
    { "mem_restart_mb", KIND_ENV, TYPE_NUMBER, "PS5WK_MEM_RESTART_MB", "3072", PS5WK_SETTINGS_PAGE, "Page memory limit",
        "When a page's process has used this much memory, the next link you follow opens in a fresh process, before memory runs out.",
        "0=Off|2048=2 GB|3072=3 GB|4096=4 GB|6144=6 GB", 0, 16384 },
    { "pas_decommit", KIND_ENV, TYPE_BOOL, "PS5WK_PAS_DECOMMIT", "1", PS5WK_SETTINGS_PAGE, "Give back freed memory",
        "Returns memory that pages no longer use to the system. Off keeps it (the old behaviour). The network process picks it up at the next browser start.",
        NULL, 0, 0 },
    { "holepunch", KIND_ENV, TYPE_BOOL, "PS5WK_HOLEPUNCH", "1", PS5WK_SETTINGS_PAGE, "Video under the page",
        "Video is drawn straight to the screen beneath the page, which is smoother. Off paints every frame into the page.", NULL, 0, 0 },
    { "media_vp9", KIND_ENV, TYPE_BOOL, "PS5WK_MEDIA_VP9", "1", PS5WK_SETTINGS_PAGE, "Offer VP9 video",
        "Lets sites such as YouTube send VP9. Off makes YouTube send H.264 instead.", NULL, 0, 0 },
    { "hwdec", KIND_FLAG, TYPE_BOOL, "no-hwdec", "1", PS5WK_SETTINGS_BROWSER, "Hardware video decoding",
        "The PS5's video decoder plays H.264, HEVC and VP9, up to 8K. Off decodes in software (1080p at most).", NULL, 0, 0 },
    { "sound", KIND_FLAG, TYPE_BOOL, "audio-silent", "1", PS5WK_SETTINGS_BROWSER, "Sound",
        "Off plays every page at volume 0.", NULL, 0, 0 },
    { "link_server", KIND_FLAG, TYPE_BOOL, "push-off", "1", PS5WK_SETTINGS_BROWSER, "Link server on the network",
        "Lets a phone or Mac send links here and open this page (port 9040). Off: only the PS5 itself can open Settings (R3).",
        NULL, 0, 0 },
    { "home_url", KIND_FILE, TYPE_TEXT, "home-url.txt", "", 0, "Home page",
        "Opens when the browser starts. Empty: the built-in start page.", NULL, 0, 0 },
};
#define SETTING_COUNT (sizeof settings / sizeof settings[0])

static GMutex lock;
static char* dataDir;
static PS5WKSettingsNormalize normalizeURL;
static char* applied[SETTING_COUNT];
static char* status;
static unsigned statusSerial;

static char* pathFor(const char* name)
{
    return g_build_filename(dataDir ? dataDir : "/data/ps5webkit", name, NULL);
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
    if (s->kind == KIND_FLAG) {
        char* path = pathFor(s->name);
        gboolean present = fileExists(path);
        g_free(path);
        return g_strdup(present ? "0" : "1");
    }
    if (s->kind == KIND_FILE) {
        char* path = pathFor(s->name);
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

void ps5wk_settings_init(const char* dir, PS5WKSettingsNormalize normalize)
{
    g_mutex_lock(&lock);
    g_free(dataDir);
    dataDir = g_strdup(dir);
    normalizeURL = normalize;
    recordApplied(PS5WK_SETTINGS_PAGE | PS5WK_SETTINGS_BROWSER);
    g_mutex_unlock(&lock);
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
    g_string_append(out, "\"settings\":[");
    for (size_t i = 0; i < SETTING_COUNT; i++) {
        const Setting* s = &settings[i];
        char* value = currentValue(s, env);
        gboolean waiting = s->scope && applied[i] && strcmp(value, applied[i]);
        if (waiting)
            pending[s->scope] = TRUE;
        g_string_append_printf(out, "%s{\"id\":\"%s\",\"type\":\"%s\",\"value\":", i ? "," : "", s->id, typeNames[s->type]);
        appendJSONString(out, value);
        g_string_append(out, ",\"default\":");
        appendJSONString(out, s->fallback);
        g_string_append(out, ",\"inUse\":");
        appendJSONString(out, s->scope && applied[i] ? applied[i] : value);
        g_string_append_printf(out, ",\"restart\":\"%s\",\"pending\":%s,\"group\":\"%s\",\"key\":", scopeName(s->scope),
            waiting ? "true" : "false", s->kind == KIND_ENV ? "page" : "browser");
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
    g_string_append_printf(out, "],\"pending\":{\"page\":%s,\"browser\":%s},\"groups\":{\"page\":\"Pages\",\"browser\":\"Browser\"},"
        "\"statusSerial\":%u,\"status\":", pending[PS5WK_SETTINGS_PAGE] ? "true" : "false",
        pending[PS5WK_SETTINGS_BROWSER] ? "true" : "false", statusSerial);
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
        char* path = s->kind == KIND_ENV ? NULL : pathFor(s->name);
        if (s->kind == KIND_ENV) {
            char* next = setEnvLine(newEnv, s->name, strcmp(values[i], s->fallback) ? values[i] : NULL);
            g_free(newEnv);
            newEnv = next;
            g_string_append_printf(envDone, "%s%s", envDone->len ? ", " : "", s->label);
            envScopes |= s->scope;
            continue;
        } else if (s->kind == KIND_FLAG) {
            if (!strcmp(values[i], "0")) { /* the file turns the feature off */
                int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                ok = fd >= 0;
                if (ok) {
                    (void)!write(fd, "1\n", 2);
                    close(fd);
                }
            } else
                ok = !unlink(path) || !fileExists(path);
        } else if (!*values[i]) {
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

/* The settings page: built from /settings.json by its script (no innerHTML:
 * every value is set as text). Large controls with a clear focus ring for
 * the D-pad (spatial navigation) at 1920x1080, buttons instead of <select>
 * (WPE has no popup menus here), and a second press to confirm what is
 * hard to undo. Light and dark. */
const char ps5wk_settings_page[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>Browser settings</title>\n"
    "<script>if(location.hostname=='127.0.0.1'||location.hostname=='localhost')document.documentElement.className='tv'</script>\n"
    "<style>\n"
    ":root{color-scheme:light dark;--bg:#f4f5f7;--fg:#15181d;--muted:#5d6672;--card:#fff;--line:#c9ced6;--accent:#0b5cd5;--on:#fff;--warn:#a04b00;--focus:#e8590c}\n"
    "@media(prefers-color-scheme:dark){:root{--bg:#10141c;--fg:#e8ecf2;--muted:#9aa4b2;--card:#1a202b;--line:#3a4352;--accent:#6aa8ff;--on:#08111f;--warn:#ffb648;--focus:#ffd75e}}\n"
    ":root.tv{color-scheme:dark;--bg:#10141c;--fg:#e8ecf2;--muted:#9aa4b2;--card:#1a202b;--line:#3a4352;--accent:#6aa8ff;--on:#08111f;--warn:#ffb648;--focus:#ffd75e}\n"
    "*{box-sizing:border-box}\n"
    "html{font-size:clamp(16px,1.25vw,26px)}\n"
    "body{margin:0;background:var(--bg);color:var(--fg);font:1rem/1.45 -apple-system,system-ui,sans-serif}\n"
    "main{max-width:56rem;margin:0 auto;padding:1.2rem 16px 3rem}\n"
    "header{display:flex;align-items:center;gap:.8rem;flex-wrap:wrap}\n"
    "h1{font-size:1.6rem;margin:0;flex:1}\n"
    "h2{font-size:1rem;margin:1.8rem 0 .4rem;color:var(--muted);text-transform:uppercase;letter-spacing:.06em}\n"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:.8rem;padding:.9rem 1rem;margin:.5rem 0}\n"
    ".row{display:flex;flex-wrap:wrap;gap:.3rem .8rem;align-items:baseline}\n"
    ".name{font-weight:600;font-size:1.1rem}\n"
    ".badge{font-size:.8rem;padding:.05rem .55rem;border-radius:1rem;border:1px solid var(--line);color:var(--muted);white-space:nowrap}\n"
    ".badge.wait{border-color:var(--warn);color:var(--warn);font-weight:600}\n"
    ".help{color:var(--muted);margin:.25rem 0 .6rem}\n"
    ".seg{display:flex;flex-wrap:wrap;gap:.5rem}\n"
    "button,input{font:inherit;color:inherit}\n"
    "button{min-height:2.6rem;padding:.45rem 1rem;border-radius:.6rem;border:2px solid var(--line);background:transparent;cursor:pointer}\n"
    "button[aria-pressed=true]{background:var(--accent);border-color:var(--accent);color:var(--on);font-weight:600}\n"
    "button.primary{background:var(--accent);border-color:var(--accent);color:var(--on);font-weight:600}\n"
    "button.danger{border-color:var(--warn);color:var(--warn)}\n"
    "button.armed{background:var(--warn);border-color:var(--warn);color:var(--on)}\n"
    "button:focus,input:focus{outline:5px solid var(--focus);outline-offset:3px}\n"
    "input{flex:1;min-width:10rem;min-height:2.6rem;padding:.45rem .8rem;border:2px solid var(--line);border-radius:.6rem;background:var(--bg)}\n"
    ".def{font-weight:400;opacity:.8}\n"
    "#status{min-height:1.5em;margin:.6rem 0;font-weight:600}\n"
    "#banner{border-color:var(--warn)}\n"
    ".note{color:var(--muted)}\n"
    "code{word-break:break-all}\n"
    "</style></head><body><main>\n"
    "<header><h1>Browser settings</h1><button id=\"back\" hidden>Back to the page</button></header>\n"
    "<p id=\"status\" role=\"status\">Loading…</p>\n"
    "<div id=\"banner\" class=\"card\" hidden><p id=\"bannerText\"></p><div class=\"seg\"><button id=\"bannerPage\" class=\"primary\" hidden>Restart page</button><button id=\"bannerBrowser\" class=\"primary\" hidden>Restart browser</button></div></div>\n"
    "<div id=\"pinbox\" class=\"card\" hidden><p>This PS5 asks for the link server's PIN.</p><div class=\"row\"><input id=\"pin\" inputmode=\"numeric\" autocomplete=\"off\" placeholder=\"PIN\"><button id=\"pinok\" class=\"primary\">OK</button></div></div>\n"
    "<div id=\"list\"></div>\n"
    "<div id=\"actions\" hidden>\n"
    "<h2>Website data</h2>\n"
    "<div class=\"card\"><p class=\"help\">Clearing cookies logs you out of every site. Clearing all data also removes the cache, local storage and site databases.</p>\n"
    "<div class=\"seg\"><button id=\"clearCookies\" class=\"danger\">Clear cookies</button><button id=\"clearAll\" class=\"danger\">Clear all website data</button></div></div>\n"
    "<h2>Restart</h2>\n"
    "<div class=\"card\"><p class=\"help\">Restart page: the page's process starts again with the settings above (Pages). Restart browser: the browser closes and opens again on the page you were on (Browser settings).</p>\n"
    "<div class=\"seg\"><button id=\"restartPage\">Restart page</button><button id=\"restartBrowser\">Restart browser</button></div></div>\n"
    "<p class=\"note\" id=\"where\"></p>\n"
    "<p class=\"note\">On the PS5: R3 (press the right stick) opens and closes this page; O goes back.</p>\n"
    "</div>\n"
    "<script>\n"
    "var S=null,pin='',started=0,polling=0;\n"
    "try{pin=sessionStorage.getItem('ps5pin')||''}catch(e){}\n"
    "var query=new URLSearchParams(location.search);if(query.get('pin'))pin=query.get('pin');\n"
    "var onPS5=location.hostname=='127.0.0.1'||location.hostname=='localhost';\n"
    "function $(id){return document.getElementById(id)}\n"
    "function el(tag,cls,text){var e=document.createElement(tag);if(cls)e.className=cls;if(text!=null)e.textContent=text;return e}\n"
    "function say(t){$('status').textContent=t||''}\n"
    "function api(method,path,body){\n"
    "  var h={'Accept':'application/json'};if(pin)h['X-PS5-PIN']=pin;if(body)h['Content-Type']='application/json';\n"
    "  return fetch(path,{method:method,headers:h,body:body?JSON.stringify(body):undefined,cache:'no-store'})\n"
    "    .then(function(r){return r.json().then(function(j){j.code=r.status;return j},function(){return{ok:false,code:r.status,error:'HTTP '+r.status}})});\n"
    "}\n"
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
    "function render(focus){\n"
    "  var list=$('list'),seen={};list.textContent='';\n"
    "  S.settings.forEach(function(s){\n"
    "    if(!seen[s.group]){seen[s.group]=1;list.appendChild(el('h2',null,S.groups[s.group]||s.group))}\n"
    "    var c=el('div','card'),r=el('div','row');\n"
    "    r.appendChild(el('span','name',s.label));\n"
    "    r.appendChild(el('span','badge'+(s.pending?' wait':''),s.pending?'Changed: '+applies(s).toLowerCase():applies(s)));\n"
    "    c.appendChild(r);c.appendChild(el('p','help',s.help));\n"
    "    if(s.type=='text'){\n"
    "      var row=el('div','row'),input=el('input');input.id='in-'+s.id;input.value=s.value;input.placeholder='Built-in start page';\n"
    "      input.setAttribute('autocapitalize','off');input.setAttribute('spellcheck','false');input.setAttribute('inputmode','url');\n"
    "      var save=el('button','primary','Save');save.id='b-'+s.id+'-save';\n"
    "      save.onclick=function(){set(s,input.value,save.id)};\n"
    "      input.onkeydown=function(e){if(e.key=='Enter')set(s,input.value,save.id)};\n"
    "      row.appendChild(input);row.appendChild(save);c.appendChild(row);\n"
    "    }else{\n"
    "      var seg=el('div','seg');\n"
    "      options(s).forEach(function(o){\n"
    "        var b=el('button',null,o.label);b.id='b-'+s.id+'-'+o.value;b.setAttribute('aria-pressed',String(o.value==s.value));\n"
    "        if(o.value==s['default'])b.appendChild(el('span','def',' (default)'));\n"
    "        b.onclick=function(){\n"
    "          if(s.id=='link_server'&&o.value=='0'&&s.value!='0'&&!onPS5)\n"
    "            arm(b,'Press again: this page then opens only on the PS5',function(){set(s,o.value,b.id)});\n"
    "          else set(s,o.value,b.id);\n"
    "        };\n"
    "        seg.appendChild(b);\n"
    "      });\n"
    "      c.appendChild(seg);\n"
    "    }\n"
    "    list.appendChild(c);\n"
    "  });\n"
    "  var p=S.pending.page,w=S.pending.browser;\n"
    "  $('banner').hidden=!p&&!w;$('bannerPage').hidden=!p;$('bannerBrowser').hidden=!w;\n"
    "  $('bannerText').textContent=p&&w?'Some changes wait for a page restart, others for a browser restart.':p?'Changes wait for a page restart.':'Changes wait for a browser restart.';\n"
    "  $('actions').hidden=false;$('back').hidden=!onPS5;\n"
    "  $('where').textContent=S.uri?'The PS5 is showing '+S.uri:'';\n"
    "  var f=focus=='first'?list.querySelector('button[aria-pressed=true]'):focus&&$(focus);if(f)f.focus();\n"
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
    "function restartBrowser(b){arm(b,'Press again to restart the browser',function(){\n"
    "  action('/restart?scope=browser','Restarting the browser…',function(){say('Restarting the browser. This page reconnects when it is back (about 20 s).');if(!onPS5)setTimeout(waitForRestart,3000)})})}\n"
    "$('restartPage').onclick=restartPage;$('bannerPage').onclick=restartPage;\n"
    "$('restartBrowser').onclick=function(){restartBrowser(this)};$('bannerBrowser').onclick=function(){restartBrowser(this)};\n"
    "$('clearCookies').onclick=function(){arm(this,'Press again: log out of every site',function(){action('/clear?what=cookies','Clearing cookies…',function(){setTimeout(function(){load('clearCookies')},1500)})})};\n"
    "$('clearAll').onclick=function(){arm(this,'Press again: clear all website data',function(){action('/clear?what=all','Clearing website data…',function(){setTimeout(function(){load('clearAll')},2000)})})};\n"
    "$('back').onclick=function(){if(history.length>1)history.back();else if(S&&S.returnUri)location.href=S.returnUri};\n"
    "$('pinok').onclick=function(){pin=$('pin').value.trim();try{sessionStorage.setItem('ps5pin',pin)}catch(e){}load()};\n"
    "$('pin').onkeydown=function(e){if(e.key=='Enter')$('pinok').click()};\n"
    "load(onPS5?'first':null);\n"
    "</script></main></body></html>\n";
