/*
 * Gamepad mode (ps5browser; README: "Gamepad mode"): per site, the whole
 * controller goes to the page. The Gamepad API is always there (the
 * display feeds it); gamepad mode stops the pad meaning anything to the
 * browser and can send keys for keyboard-only games:
 *
 *   - it is never switched on by itself: △ held 1 s on the page (or the
 *     browser menu's gamepad button) turns it on, Options held 1 s (or the
 *     button) turns it off. It stays on while the tab shows the same site
 *     and goes off with another site or another tab; nothing about on or
 *     off is kept;
 *   - when on, wpe_display_ps5_set_gamepad_mode() gets the site's mapping
 *     (sites.json, browserdata.c: a preset, keys, sticks), and spatial
 *     navigation is off (the arrow keys are the game's);
 *   - a page that reads the gamepad gets a hint, once per visit to the site:
 *     "This page supports controllers · hold △ for gamepad mode"; it takes
 *     no buttons;
 *   - □ on the menu's gamepad button opens the mapping page (in a browser
 *     page tab, browserui.c);
 *   - /gamepad (link server) is the mapping page: presets, a key per button,
 *     what the sticks do; it talks to /browser/gamepad (browserui.c's API).
 *
 * Main thread only.
 */
#define SPIKE_NAME "ps5browser"
#include "common/spike.h"
#include "browser.h"

#include <stdlib.h>
#include <string.h>

static WPEDisplayPS5* display;
static WebKitWebView* view;  /* the tab shown */
static char* host;           /* its site ("file:" for local files), NULL: none */
static gboolean modeOn;      /* gamepad mode applied for it */
static WebKitWebView* onView; /* gamepad mode was turned on in this tab... */
static char* onHost;         /* ...on this site; NULL: off */
static gboolean pageReads;   /* WebKit reads the gamepad for the page shown */
static char* hintedHost;     /* the site whose visit had its hint */

/* ---------------------------------------------------------------- keys, buttons, presets */

typedef struct {
    const char* name; /* KeyboardEvent.code, "Click" for the mouse */
    const char* label;
    guint keyval;
    guint keycode; /* evdev + 8 */
} Key;

static const Key namedKeys[] = {
    { "Click", "Mouse click", WPE_KEY_Pointer_Button1, 0 },
    { "Space", "Space", WPE_KEY_space, 65 },
    { "Enter", "Enter", WPE_KEY_Return, 36 },
    { "Escape", "Esc", WPE_KEY_Escape, 9 },
    { "Tab", "Tab", WPE_KEY_Tab, 23 },
    { "Backspace", "Backspace", WPE_KEY_BackSpace, 22 },
    { "ShiftLeft", "Shift", WPE_KEY_Shift_L, 50 },
    { "ControlLeft", "Ctrl", WPE_KEY_Control_L, 37 },
    { "AltLeft", "Alt", WPE_KEY_Alt_L, 64 },
    { "ArrowUp", "↑", WPE_KEY_Up, 111 },
    { "ArrowDown", "↓", WPE_KEY_Down, 116 },
    { "ArrowLeft", "←", WPE_KEY_Left, 113 },
    { "ArrowRight", "→", WPE_KEY_Right, 114 },
};
/* a..z: evdev + 8 */
static const guint8 letterCodes[26] = { 38, 56, 54, 40, 26, 41, 42, 43, 31, 44, 45, 46, 58, 57, 32, 33, 24, 27, 39, 28, 30, 55, 25, 53, 29, 52 };

/* The key named @name (into @out); FALSE: no such key. */
static gboolean keyNamed(const char* name, Key* out)
{
    for (unsigned i = 0; i < G_N_ELEMENTS(namedKeys); i++) {
        if (!strcmp(namedKeys[i].name, name)) {
            *out = namedKeys[i];
            return TRUE;
        }
    }
    if (!strncmp(name, "Key", 3) && name[3] >= 'A' && name[3] <= 'Z' && !name[4]) {
        static char labels[26][2];
        int i = name[3] - 'A';
        labels[i][0] = name[3];
        *out = (Key) { name, labels[i], WPE_KEY_a + i, letterCodes[i] };
        return TRUE;
    }
    if (!strncmp(name, "Digit", 5) && name[5] >= '0' && name[5] <= '9' && !name[6]) {
        static char labels[10][2];
        int d = name[5] - '0';
        labels[d][0] = name[5];
        *out = (Key) { name, labels[d], WPE_KEY_0 + d, d ? 9 + d : 19 };
        return TRUE;
    }
    return FALSE;
}

typedef struct {
    const char* name;
    const char* label;
    guint32 bit;
} Button;

static const Button buttons[] = {
    { "cross", "✕", WPE_DISPLAY_PS5_BUTTON_CROSS },
    { "circle", "○", WPE_DISPLAY_PS5_BUTTON_CIRCLE },
    { "square", "□", WPE_DISPLAY_PS5_BUTTON_SQUARE },
    { "triangle", "△", WPE_DISPLAY_PS5_BUTTON_TRIANGLE },
    { "up", "D-pad ↑", WPE_DISPLAY_PS5_BUTTON_UP },
    { "down", "D-pad ↓", WPE_DISPLAY_PS5_BUTTON_DOWN },
    { "left", "D-pad ←", WPE_DISPLAY_PS5_BUTTON_LEFT },
    { "right", "D-pad →", WPE_DISPLAY_PS5_BUTTON_RIGHT },
    { "l1", "L1", WPE_DISPLAY_PS5_BUTTON_L1 },
    { "r1", "R1", WPE_DISPLAY_PS5_BUTTON_R1 },
    { "l2", "L2", WPE_DISPLAY_PS5_BUTTON_L2 },
    { "r2", "R2", WPE_DISPLAY_PS5_BUTTON_R2 },
    { "l3", "L3", WPE_DISPLAY_PS5_BUTTON_L3 },
    { "r3", "R3", WPE_DISPLAY_PS5_BUTTON_R3 },
    { "options", "Options", WPE_DISPLAY_PS5_BUTTON_OPTIONS },
    { "touchpad", "Touchpad", WPE_DISPLAY_PS5_BUTTON_TOUCHPAD },
};

static const Button* buttonNamed(const char* name)
{
    for (unsigned i = 0; i < G_N_ELEMENTS(buttons); i++) {
        if (!strcmp(buttons[i].name, name))
            return &buttons[i];
    }
    return NULL;
}

static const char* const stickNames[] = { "gamepad", "arrows", "wasd", "pointer", "scroll" };

static WPEDisplayPS5StickAction stickAction(const char* name)
{
    for (unsigned i = 0; name && i < G_N_ELEMENTS(stickNames); i++) {
        if (!strcmp(stickNames[i], name))
            return (WPEDisplayPS5StickAction)i;
    }
    return WPE_DISPLAY_PS5_STICK_GAMEPAD;
}

typedef struct {
    const char* id;
    const char* label;
    const char* help;
    const char* keys;
    const char* left;
    const char* right;
} Preset;

static const Preset presets[] = {
    { "gamepad", "Controller only", "For games that support controllers: the page gets the whole controller, no keys.", "", "gamepad", "gamepad" },
    { "arrows", "Arrows + Space", "Arrow-key games: D-pad and left stick are the arrows, ✕ Space, ○ Esc, □ Z, △ X, Options Enter.",
        "up=ArrowUp down=ArrowDown left=ArrowLeft right=ArrowRight cross=Space circle=Escape square=KeyZ triangle=KeyX l1=ShiftLeft r1=ControlLeft options=Enter",
        "arrows", "gamepad" },
    { "wasd", "WASD + mouse", "Shooters and 3D games: left stick WASD, right stick the pointer, R2 click, ✕ Space, □ R, △ E, L2 Shift, Options Esc.",
        "up=ArrowUp down=ArrowDown left=ArrowLeft right=ArrowRight cross=Space circle=KeyC square=KeyR triangle=KeyE l1=KeyQ r1=KeyF l2=ShiftLeft r2=Click options=Escape touchpad=Tab",
        "wasd", "pointer" },
    { "mouse", "Point and click", "Mouse games: left stick the pointer, ✕ or R2 click, right stick scrolls, ○ Esc.",
        "cross=Click r2=Click circle=Escape options=Escape up=ArrowUp down=ArrowDown left=ArrowLeft right=ArrowRight",
        "pointer", "scroll" },
};

static const Preset* presetNamed(const char* id)
{
    for (unsigned i = 0; id && i < G_N_ELEMENTS(presets); i++) {
        if (!strcmp(presets[i].id, id))
            return &presets[i];
    }
    return NULL;
}

/* "cross=Space circle=Escape": checked and normalised (a new string), or
 * NULL and *error. */
static char* normaliseKeys(const char* text, char** error)
{
    GString* out = g_string_new(NULL);
    char** tokens = g_strsplit_set(text ? text : "", " ,;\n", -1);
    for (char** t = tokens; *t; t++) {
        if (!**t)
            continue;
        char* eq = strchr(*t, '=');
        Key key;
        if (!eq) {
            *error = g_strdup_printf("\"%s\": button=Key", *t);
            break;
        }
        *eq = 0;
        if (!buttonNamed(*t)) {
            *error = g_strdup_printf("no button \"%s\"", *t);
            break;
        }
        if (!*(eq + 1) || !strcmp(eq + 1, "None"))
            continue;
        if (!keyNamed(eq + 1, &key)) {
            *error = g_strdup_printf("no key \"%s\"", eq + 1);
            break;
        }
        g_string_append_printf(out, "%s%s=%s", out->len ? " " : "", *t, eq + 1);
    }
    g_strfreev(tokens);
    if (*error) {
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}

/* ---------------------------------------------------------------- applying */

/* The site of @uri for its settings, or NULL (no settings: the browser's own pages). */
static char* siteOf(const char* uri)
{
    if (!uri || !*uri)
        return NULL;
    if (g_str_has_prefix(uri, "file:"))
        return g_strdup("file:");
    GUri* parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
    if (!parsed)
        return NULL;
    const char* scheme = g_uri_get_scheme(parsed);
    const char* h = g_uri_get_host(parsed);
    char* site = NULL;
    if (h && *h && (!g_strcmp0(scheme, "http") || !g_strcmp0(scheme, "https"))
        && !(!strcmp(h, "127.0.0.1") && g_uri_get_port(parsed) == 9040))
        site = g_ascii_strdown(h, -1);
    g_uri_unref(parsed);
    return site;
}

static void apply(gboolean announce)
{
    PS5WKSite* site = host ? ps5wk_site_find(host) : NULL;
    gboolean on = onHost && view == onView && !g_strcmp0(host, onHost);
    GArray* keys = g_array_new(FALSE, FALSE, sizeof(WPEDisplayPS5ButtonKey));
    if (on && site && site->keys) {
        char** tokens = g_strsplit(site->keys, " ", -1);
        for (char** t = tokens; *t; t++) {
            char* eq = strchr(*t, '=');
            Key key;
            const Button* button;
            if (!eq)
                continue;
            *eq = 0;
            if ((button = buttonNamed(*t)) && keyNamed(eq + 1, &key)) {
                WPEDisplayPS5ButtonKey k = { button->bit, key.keyval, key.keycode };
                g_array_append_val(keys, k);
            }
        }
        g_strfreev(tokens);
    }
    wpe_display_ps5_set_gamepad_mode(display, on, &g_array_index(keys, WPEDisplayPS5ButtonKey, 0), keys->len,
        on && site ? stickAction(site->left) : WPE_DISPLAY_PS5_STICK_GAMEPAD, on && site ? stickAction(site->right) : WPE_DISPLAY_PS5_STICK_GAMEPAD);
    g_array_unref(keys);
    /* The arrow keys are the game's, not the focus's. */
    if (view) {
        WebKitSettings* settings = webkit_web_view_get_settings(view);
        if (webkit_settings_get_enable_spatial_navigation(settings) == on)
            webkit_settings_set_enable_spatial_navigation(settings, !on);
    }
    if (announce && on != modeOn)
        wpe_display_ps5_show_message(display, on ? "Gamepad mode on · hold Options to leave it" : "Gamepad mode off", on ? 4 : 2);
    if (on != modeOn)
        spike_log("gamepad: mode %s for %s", on ? "on" : "off", host ? host : "(no site)");
    gboolean changed = on != modeOn;
    modeOn = on;
    if (changed) /* full screen while it is on */
        ps5wk_browser_layout_changed();
}

void ps5wk_gamepad_page(WebKitWebView* shown, const char* uri)
{
    view = shown;
    char* site = siteOf(uri);
    if (g_strcmp0(site, host)) {
        g_free(host);
        host = site;
        site = NULL;
    }
    g_free(site);
    /* Another tab or another site: gamepad mode off; a new visit, a new hint. */
    if (onHost && (view != onView || g_strcmp0(host, onHost))) {
        g_clear_pointer(&onHost, g_free);
        onView = NULL;
    }
    if (g_strcmp0(host, hintedHost))
        g_clear_pointer(&hintedHost, g_free);
    apply(TRUE);
}

gboolean ps5wk_gamepad_active(void)
{
    return modeOn;
}

const char* ps5wk_gamepad_site(void)
{
    return host;
}

/* ---------------------------------------------------------------- the hint */

/* Once per visit to a site whose page reads the controller, while gamepad
 * mode is off. It takes no buttons. */
static void hintIfWanted(void)
{
    if (!pageReads || !host || modeOn || !g_strcmp0(host, hintedHost))
        return;
    g_free(hintedHost);
    hintedHost = g_strdup(host);
    wpe_display_ps5_show_message(display, "This page supports controllers · hold △ for gamepad mode", 4);
    spike_log("gamepad: hint shown on %s", host);
}

static void onGamepadInUse(WPEDisplayPS5* d, gboolean inUse, gpointer data)
{
    (void)d;
    (void)data;
    pageReads = inUse;
    hintIfWanted();
}

gboolean ps5wk_gamepad_buttons(guint32 pressed)
{
    (void)pressed;
    /* A page that started reading the controller before it was shown. */
    hintIfWanted();
    return FALSE;
}

/* ---------------------------------------------------------------- on and off */

void ps5wk_gamepad_enter(void)
{
    if (!host) {
        wpe_display_ps5_show_message(display, "Gamepad mode is for web pages", 2);
        return;
    }
    if (modeOn)
        return;
    PS5WKSite* site = ps5wk_site_get(host);
    if ((!site->keys || !*site->keys) && !site->left && !site->right) {
        /* No mapping yet: a page that reads the controller wants it whole;
         * otherwise the arrow keys are the best guess. Kept for the site. */
        const Preset* preset = presetNamed(pageReads ? "gamepad" : "arrows");
        site->keys = g_strdup(preset->keys);
        site->left = g_strdup(preset->left);
        site->right = g_strdup(preset->right);
        ps5wk_site_changed(site);
    }
    g_free(onHost);
    onHost = g_strdup(host);
    onView = view;
    apply(TRUE);
}

void ps5wk_gamepad_leave(void)
{
    if (!modeOn)
        return;
    g_clear_pointer(&onHost, g_free);
    onView = NULL;
    apply(FALSE);
    wpe_display_ps5_show_message(display, "Gamepad mode off · hold △ to turn it on again", 3);
    spike_log("gamepad: left on %s", host ? host : "(no site)");
}

void ps5wk_gamepad_toggle(void)
{
    if (modeOn)
        ps5wk_gamepad_leave();
    else
        ps5wk_gamepad_enter();
}

void ps5wk_gamepad_init(WPEDisplayPS5* d)
{
    display = d;
    wpe_display_ps5_set_gamepad_callback(display, onGamepadInUse, NULL);
}

/* ---------------------------------------------------------------- /browser/gamepad */

static void siteJSON(GString* out, const PS5WKSite* site)
{
    g_string_append(out, "{\"host\":");
    ps5wk_json_string(out, site->host);
    g_string_append(out, ",\"keys\":");
    ps5wk_json_string(out, site->keys ? site->keys : "");
    g_string_append(out, ",\"left\":");
    ps5wk_json_string(out, site->left ? site->left : "gamepad");
    g_string_append(out, ",\"right\":");
    ps5wk_json_string(out, site->right ? site->right : "gamepad");
    g_string_append(out, "}");
}

static char* stateJSON(const char* message)
{
    GString* out = g_string_new("{\"ok\":true,\"current\":");
    if (host)
        ps5wk_json_string(out, host);
    else
        g_string_append(out, "null");
    g_string_append_printf(out, ",\"on\":%s,\"pageReads\":%s,\"private\":%s,\"message\":", modeOn ? "true" : "false",
        pageReads ? "true" : "false", ps5wk_browser_private() ? "true" : "false");
    ps5wk_json_string(out, message ? message : "");
    g_string_append(out, ",\"sites\":[");
    GPtrArray* sites = ps5wk_sites();
    for (unsigned i = 0; i < sites->len; i++) {
        if (i)
            g_string_append_c(out, ',');
        siteJSON(out, g_ptr_array_index(sites, i));
    }
    g_string_append(out, "],\"buttons\":[");
    for (unsigned i = 0; i < G_N_ELEMENTS(buttons); i++) {
        g_string_append_printf(out, "%s{\"name\":\"%s\",\"label\":", i ? "," : "", buttons[i].name);
        ps5wk_json_string(out, buttons[i].label);
        g_string_append_c(out, '}');
    }
    g_string_append(out, "],\"keys\":[");
    for (unsigned i = 0; i < G_N_ELEMENTS(namedKeys); i++) {
        g_string_append_printf(out, "%s{\"name\":\"%s\",\"label\":", i ? "," : "", namedKeys[i].name);
        ps5wk_json_string(out, namedKeys[i].label);
        g_string_append_c(out, '}');
    }
    for (int i = 0; i < 26; i++)
        g_string_append_printf(out, ",{\"name\":\"Key%c\",\"label\":\"%c\"}", 'A' + i, 'A' + i);
    for (int i = 0; i < 10; i++)
        g_string_append_printf(out, ",{\"name\":\"Digit%d\",\"label\":\"%d\"}", i, i);
    g_string_append(out, "],\"presets\":[");
    for (unsigned i = 0; i < G_N_ELEMENTS(presets); i++) {
        const Preset* p = &presets[i];
        g_string_append_printf(out, "%s{\"id\":\"%s\",\"label\":", i ? "," : "", p->id);
        ps5wk_json_string(out, p->label);
        g_string_append(out, ",\"help\":");
        ps5wk_json_string(out, p->help);
        g_string_append_printf(out, ",\"keys\":\"%s\",\"left\":\"%s\",\"right\":\"%s\"}", p->keys, p->left, p->right);
    }
    g_string_append(out, "]}\n");
    return g_string_free(out, FALSE);
}

char* ps5wk_gamepad_api(gboolean post, const char* site, const char* preset, const char* keys,
    const char* left, const char* right, gboolean remove, int* status)
{
    *status = 200;
    if (!post)
        return stateJSON(NULL);
    char* error = NULL;
    char* message = NULL;
    char* name = site && *site ? g_ascii_strdown(site, -1) : NULL;
    if (!name || strchr(name, '/') || strchr(name, ' '))
        error = g_strdup("site=<host> (or file: for local files)");
    else if (remove) {
        ps5wk_site_remove(name);
        message = g_strdup_printf("%s: settings removed.", name);
    } else {
        const Preset* p = preset && *preset ? presetNamed(preset) : NULL;
        char* normalised = NULL;
        if (preset && *preset && !p)
            error = g_strdup("preset: gamepad, arrows, wasd or mouse");
        else if ((left && *left && stickAction(left) == WPE_DISPLAY_PS5_STICK_GAMEPAD && strcmp(left, "gamepad"))
            || (right && *right && stickAction(right) == WPE_DISPLAY_PS5_STICK_GAMEPAD && strcmp(right, "gamepad")))
            error = g_strdup("left/right: gamepad, arrows, wasd, pointer or scroll");
        else if (keys && !(normalised = normaliseKeys(keys, &error)))
            ;
        else {
            PS5WKSite* s = ps5wk_site_get(name);
            if (p) {
                g_free(s->keys);
                g_free(s->left);
                g_free(s->right);
                s->keys = g_strdup(p->keys);
                s->left = g_strdup(p->left);
                s->right = g_strdup(p->right);
            }
            if (normalised) {
                g_free(s->keys);
                s->keys = normalised;
                normalised = NULL;
            }
            if (left && *left) {
                g_free(s->left);
                s->left = g_strdup(left);
            }
            if (right && *right) {
                g_free(s->right);
                s->right = g_strdup(right);
            }
            ps5wk_site_changed(s);
            message = g_strdup_printf("%s: saved%s.", name, ps5wk_browser_private() ? " (until private browsing ends)" : "");
        }
        g_free(normalised);
    }
    if (!error && name && !g_strcmp0(name, host))
        apply(FALSE);
    g_free(name);
    if (error) {
        *status = 400;
        GString* out = g_string_new("{\"ok\":false,\"error\":");
        ps5wk_json_string(out, error);
        g_string_append(out, "}\n");
        g_free(error);
        g_free(message);
        return g_string_free(out, FALSE);
    }
    spike_log("gamepad: %s", message);
    char* reply = stateJSON(message);
    g_free(message);
    return reply;
}

/* ---------------------------------------------------------------- the page (/gamepad) */

/* Made for the D-pad at 1920x1080 as the settings page is (buttons, no
 * <select>); also from a phone at the PS5's address, with the PIN. Its
 * script reads and changes /browser/gamepad. */
const char ps5wk_gamepad_page_html[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>Gamepad mode</title>\n"
    "<script>if(location.protocol=='parallax:'||location.hostname=='127.0.0.1'||location.hostname=='localhost')document.documentElement.className='tv'</script>\n"
    "<style>\n"
    ":root{color-scheme:light dark;--bg:#f7f5ff;--fg:#14113d;--muted:#5a5584;--card:#fff;--line:#dcd6f7;--accent:#6d0efd;--on:#fff;--warn:#a04b00;--focus:#d10aa6}\n"
    "@media(prefers-color-scheme:dark){:root{--bg:#07061f;--fg:#f4f2ff;--muted:#b9b5e0;--card:#110f35;--line:#2b2766;--accent:#a77bff;--on:#01001a;--warn:#ffb648;--focus:#ff5cd6}}\n"
    ":root.tv{color-scheme:dark;--bg:#07061f;--fg:#f4f2ff;--muted:#b9b5e0;--card:#110f35;--line:#2b2766;--accent:#a77bff;--on:#01001a;--warn:#ffb648;--focus:#ff5cd6}\n"
    "*{box-sizing:border-box}\n"
    "html{font-size:clamp(16px,1.25vw,26px)}\n"
    "body{margin:0;background:var(--bg);color:var(--fg);font:1rem/1.45 -apple-system,system-ui,sans-serif}\n"
    "main{max-width:56rem;margin:0 auto;padding:1.2rem 16px 3rem}\n"
    "header{display:flex;align-items:center;gap:.8rem;flex-wrap:wrap}\n"
    "h1{font-size:1.6rem;margin:0;flex:1}\n"
    "h1 span{display:block;font-size:.8rem;font-weight:600;color:var(--muted);letter-spacing:.02em}\n"
    "h2{font-size:1rem;margin:1.8rem 0 .4rem;color:var(--muted);text-transform:uppercase;letter-spacing:.06em}\n"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:.8rem;padding:.9rem 1rem;margin:.5rem 0}\n"
    ".row{display:flex;flex-wrap:wrap;gap:.3rem .8rem;align-items:center}\n"
    ".name{font-weight:600;font-size:1.1rem}\n"
    ".help{color:var(--muted);margin:.25rem 0 .6rem}\n"
    ".seg{display:flex;flex-wrap:wrap;gap:.5rem}\n"
    "button,input{font:inherit;color:inherit}\n"
    "button{min-height:2.6rem;padding:.45rem 1rem;border-radius:.6rem;border:2px solid var(--line);background:transparent;cursor:pointer}\n"
    "button[aria-pressed=true]{background:var(--accent);border-color:var(--accent);color:var(--on);font-weight:600}\n"
    "button.danger{border-color:var(--warn);color:var(--warn)}\n"
    "button:focus,input:focus{outline:5px solid var(--focus);outline-offset:3px}\n"
    "input{min-height:2.6rem;padding:.45rem .8rem;border:2px solid var(--line);border-radius:.6rem;background:var(--bg);width:8rem}\n"
    ".map{display:grid;grid-template-columns:repeat(auto-fill,minmax(12rem,1fr));gap:.5rem}\n"
    ".map button{display:flex;justify-content:space-between;gap:.6rem;text-align:left}\n"
    ".map b{min-width:4.5rem}\n"
    ".map span{color:var(--muted)}\n"
    ".map button.set span{color:var(--fg);font-weight:600}\n"
    ".keys{display:grid;grid-template-columns:repeat(auto-fill,minmax(5.5rem,1fr));gap:.4rem;margin-top:.6rem}\n"
    ".keys button{padding:.3rem .4rem}\n"
    "#status{min-height:1.5em;margin:.6rem 0;font-weight:600}\n"
    ".note{color:var(--muted)}\n"
    "</style></head><body><main>\n"
    "<header><h1><span>Parallax · per-site controls</span>Gamepad mode</h1><button id=\"back\" hidden>Back to the page</button></header>\n"
    "<p class=\"help\">In gamepad mode the whole controller belongs to the page: the browser stops using it (no pointer, scrolling or Back). <b>Hold △ on the page for a second</b> to turn it on, <b>hold Options for a second</b> to leave it; it goes off by itself on another site or tab. The settings here are kept for each site. Pages that support controllers read it directly; for keyboard games, each button can press a key.</p>\n"
    "<div id=\"pinbox\" class=\"card\" hidden><p>This PS5 asks for the link server's PIN.</p><div class=\"row\"><input id=\"pin\" inputmode=\"numeric\" autocomplete=\"off\" placeholder=\"PIN\"><button id=\"pinok\">OK</button></div></div>\n"
    "<div id=\"status\" role=\"status\"></div>\n"
    "<div id=\"site\"></div>\n"
    "<h2>Other sites</h2>\n"
    "<div id=\"others\" class=\"card\"><p class=\"note\">Loading…</p></div>\n"
    "</main><script>\n"
    "var S=null,site='',pin='',picking='';\n"
    "try{pin=sessionStorage.getItem('ps5pin')||''}catch(e){}\n"
    "var query=new URLSearchParams(location.search);if(query.get('pin'))pin=query.get('pin');\n"
    "function $(id){return document.getElementById(id)}\n"
    "function el(tag,cls,text){var e=document.createElement(tag);if(cls)e.className=cls;if(text!=null)e.textContent=text;return e}\n"
    "function say(t){$('status').textContent=t||''}\n"
    "function api(params){\n"
    "  var h={'Accept':'application/json'};if(pin)h['X-PS5-PIN']=pin;\n"
    "  var o={headers:h,cache:'no-store'};\n"
    "  if(params){o.method='POST';h['Content-Type']='application/x-www-form-urlencoded';o.body=new URLSearchParams(params).toString()}\n"
    "  return fetch('/browser/gamepad',o).then(function(r){return r.json().then(function(j){j.code=r.status;return j})});\n"
    "}\n"
    "function took(j,focus){\n"
    "  if(j.code==403){$('pinbox').hidden=false;say(j.error||'PIN needed');$('pin').focus();return}\n"
    "  if(!j.ok){say(j.error||'Not saved');return}\n"
    "  $('pinbox').hidden=true;S=j;\n"
    "  if(!site)site=query.get('site')||j.current||(j.sites[0]&&j.sites[0].host)||'';\n"
    "  render(focus);say(j.message);\n"
    "}\n"
    "function load(focus){api(null).then(function(j){took(j,focus)}).catch(function(){say('The browser did not answer.')})}\n"
    "function send(params,focus){params.site=site;api(params).then(function(j){took(j,focus)}).catch(function(){say('Not saved: the browser did not answer.')})}\n"
    "function entry(){for(var i=0;i<S.sites.length;i++)if(S.sites[i].host==site)return S.sites[i];return {host:site,keys:'',left:'gamepad',right:'gamepad'}}\n"
    "function presetName(e){for(var i=0;i<S.presets.length;i++){var pr=S.presets[i];if(pr.keys==(e.keys||'')&&pr.left==e.left&&pr.right==e.right)return pr.label}return 'own buttons'}\n"
    "function keyMap(e){var m={};(e.keys||'').split(' ').forEach(function(t){var p=t.split('=');if(p[1])m[p[0]]=p[1]});return m}\n"
    "function keyLabel(name){for(var i=0;i<S.keys.length;i++)if(S.keys[i].name==name)return S.keys[i].label;return name}\n"
    "function seg(items,current,onpick,idp){\n"
    "  var d=el('div','seg');items.forEach(function(it){var b=el('button',null,it[1]);b.id=idp+it[0];b.setAttribute('aria-pressed',it[0]==current);b.onclick=function(){onpick(it[0])};d.appendChild(b)});return d}\n"
    "var STICKS=[['gamepad','Controller'],['arrows','Arrow keys'],['wasd','W A S D'],['pointer','Pointer'],['scroll','Scroll']];\n"
    "function render(focus){\n"
    "  var box=$('site');box.textContent='';\n"
    "  if(!site){box.appendChild(el('div','card note','Open a game, then come here from the menu (Options, then □ on the controller button).'));renderOthers();return}\n"
    "  var e=entry(),m=keyMap(e);\n"
    "  var c=el('div','card');box.appendChild(c);\n"
    "  var head=el('div','row');head.appendChild(el('span','name',site=='file:'?'Local files':site));\n"
    "  if(S.current==site)head.appendChild(el('span','note',S.on?'· on now':'· the page shown'));\n"
    "  if(S.private)head.appendChild(el('span','note','· private: kept until private browsing ends'));\n"
    "  c.appendChild(head);\n"
    "  c.appendChild(el('h2',null,'Presets'));\n"
    "  var p=el('div','seg');c.appendChild(p);\n"
    "  S.presets.forEach(function(pr){var b=el('button',null,pr.label);b.id='p'+pr.id;b.title=pr.help;\n"
    "    b.setAttribute('aria-pressed',pr.keys==(e.keys||'')&&pr.left==e.left&&pr.right==e.right);\n"
    "    b.onclick=function(){send({preset:pr.id},'p'+pr.id)};p.appendChild(b)});\n"
    "  var hp=el('p','help');S.presets.forEach(function(pr){if(pr.keys==(e.keys||'')&&pr.left==e.left&&pr.right==e.right)hp.textContent=pr.help});c.appendChild(hp);\n"
    "  c.appendChild(el('h2',null,'Left stick'));\n"
    "  c.appendChild(seg(STICKS,e.left,function(v){send({left:v},'l'+v)},'l'));\n"
    "  c.appendChild(el('h2',null,'Right stick'));\n"
    "  c.appendChild(seg(STICKS,e.right,function(v){send({right:v},'r'+v)},'r'));\n"
    "  c.appendChild(el('h2',null,'Buttons'));\n"
    "  c.appendChild(el('p','help','The page always gets the controller too; a key here is pressed as well. △ held for a second turns gamepad mode on, Options held for a second leaves it.'));\n"
    "  var g=el('div','map');c.appendChild(g);\n"
    "  S.buttons.forEach(function(bt){\n"
    "    var b=el('button',m[bt.name]?'set':null);b.id='b'+bt.name;\n"
    "    b.appendChild(el('b',null,bt.label));b.appendChild(el('span',null,m[bt.name]?keyLabel(m[bt.name]):'—'));\n"
    "    b.onclick=function(){picking=picking==bt.name?'':bt.name;render('b'+bt.name)};g.appendChild(b);\n"
    "    if(picking==bt.name){\n"
    "      var k=el('div','keys');k.style.gridColumn='1/-1';g.appendChild(k);\n"
    "      [{name:'None',label:'None'}].concat(S.keys).forEach(function(key){\n"
    "        var kb=el('button',null,key.label);kb.id='k'+key.name;kb.setAttribute('aria-pressed',(m[bt.name]||'None')==key.name);\n"
    "        kb.onclick=function(){if(key.name=='None')delete m[bt.name];else m[bt.name]=key.name;picking='';\n"
    "          send({keys:Object.keys(m).map(function(n){return n+'='+m[n]}).join(' ')},'b'+bt.name)};\n"
    "        k.appendChild(kb)});\n"
    "      if(!focus)focus='k'+(m[bt.name]||'None');\n"
    "    }\n"
    "  });\n"
    "  var rm=el('button','danger','Forget this site');rm.id='forget';rm.style.marginTop='1rem';\n"
    "  rm.onclick=function(){send({remove:'1'});site=''};c.appendChild(rm);\n"
    "  renderOthers();\n"
    "  var f=focus&&$(focus);if(f)f.focus();\n"
    "}\n"
    "function renderOthers(){\n"
    "  var o=$('others');o.textContent='';var any=0;\n"
    "  S.sites.forEach(function(s){if(s.host==site)return;any=1;\n"
    "    var r=el('div','row');r.style.margin='.3rem 0';\n"
    "    var b=el('button',null,(s.host=='file:'?'Local files':s.host)+' · '+presetName(s));\n"
    "    b.onclick=function(){site=s.host;picking='';render('pgamepad')};r.appendChild(b);o.appendChild(r)});\n"
    "  if(!any)o.appendChild(el('p','note','No other sites yet.'));\n"
    "}\n"
    "$('pinok').onclick=function(){pin=$('pin').value.trim();try{sessionStorage.setItem('ps5pin',pin)}catch(e){}load()};\n"
    "$('pin').onkeydown=function(e){if(e.key=='Enter')$('pinok').click()};\n"
    "/* On the PS5: closes this browser page tab, back to the page it was opened over (O does too). */\n"
    "$('back').hidden=document.documentElement.className!='tv';\n"
    "$('back').onclick=function(){var h={'Content-Type':'application/x-www-form-urlencoded'};if(pin)h['X-PS5-PIN']=pin;\n"
    "  fetch('/browser/page',{method:'POST',headers:h,body:'action=close',cache:'no-store'}).then(function(r){if(!r.ok)history.back()}).catch(function(){history.back()})};\n"
    "load('pgamepad');\n"
    "</script></body></html>\n";
