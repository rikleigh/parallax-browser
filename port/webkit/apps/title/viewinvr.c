/*
 * View in VR (ps5browser): the <video> playing in the page, in the PS VR2
 * headset.
 *
 * The touchpad button asks the page which video plays (a script in the
 * page: the playing <video>, largest first, in the page and same-origin
 * frames), guesses the projection from the page title, the address and the
 * stream's address (`180`, `360`, `SBS`/`LR`, `TB`/`OU`, `fisheye`,
 * `MKX200`; YouTube's own projection data when it has some; otherwise a
 * flat screen), and has the display take the video into the headset
 * (wpe-ps5 PS5RendererVR.cpp: the title draws the same decoded frames there;
 * the page keeps playing). While in VR the pad is ours:
 *
 *   X                play / pause (the page's video element, so the site's
 *                    player stays in sync)
 *   D-pad left/right seek -10 s / +10 s
 *   Triangle         next projection
 *   Square           recentre (the video's front where you look)
 *   Options          show the hint again
 *   O or touchpad    back to the TV (playback continues)
 *
 * The TV shows a notice meanwhile. Without a headset the shortcut shows a
 * message on the TV instead.
 *
 * /data/ps5webkit/vr-selftest (with /data/ps5webkit/vr-sim, so no headset is
 * touched): a scripted log-only run after the first test page loads.
 */
#define SPIKE_NAME "ps5browser"
#include "common/spike.h"
#include "viewinvr.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>

static WPEDisplayPS5* vrDisplay;
static WebKitWebView* vrWebView;
static gboolean vrOn;       /* the headset shows the video */
static gboolean vrStarting; /* asked, not answered yet */
static int vrProjection;
static double vrFisheye = 180;
static gboolean vrControls; /* the page's video element was found */
static char* vrTitle;

/* ---------------------------------------------------------------- the page's video */

/* Finds the playing video (or the largest one) in the page and same-origin
 * frames, remembers it for the commands below and describes it, one
 * key=value per line. Runs in the page's world: YouTube's player object
 * (#movie_player) is a page script's. */
static const char kProbeScript[] =
    "(function () {\n"
    "  function collect(doc, out, depth) {\n"
    "    try {\n"
    "      for (const v of doc.querySelectorAll('video')) out.push(v);\n"
    "      if (depth < 3) for (const f of doc.querySelectorAll('iframe')) {\n"
    "        try { if (f.contentDocument) collect(f.contentDocument, out, depth + 1); } catch (e) {}\n"
    "      }\n"
    "    } catch (e) {}\n"
    "    return out;\n"
    "  }\n"
    "  const all = collect(document, [], 0);\n"
    "  let best = null, bestScore = -1;\n"
    "  for (const v of all) {\n"
    "    const r = v.getBoundingClientRect();\n"
    "    const score = (v.paused ? 0 : 4e9) + (v.readyState >= 2 ? 2e9 : 0) + Math.min(r.width * r.height, 1e9);\n"
    "    if (score > bestScore) { bestScore = score; best = v; }\n"
    "  }\n"
    "  window.__ps5wkVRVideo = best;\n"
    "  const clean = s => String(s == null ? '' : s).replace(/[\\r\\n]+/g, ' ').slice(0, 600);\n"
    "  const out = ['videos=' + all.length, 'title=' + clean(document.title), 'url=' + clean(location.href)];\n"
    "  if (best) {\n"
    "    out.push('found=1', 'paused=' + (best.paused ? 1 : 0), 'size=' + best.videoWidth + 'x' + best.videoHeight,\n"
    "      'time=' + best.currentTime, 'duration=' + best.duration);\n"
    "    const srcs = [best.currentSrc, best.getAttribute('src'), best.getAttribute('data-src'), best.title];\n"
    "    for (const s of best.querySelectorAll('source')) srcs.push(s.src);\n"
    "    out.push('src=' + clean(srcs.filter(Boolean).join(' ')));\n"
    "    const hints = [];\n"
    "    for (let e = best; e && hints.length < 20; e = e.parentElement)\n"
    "      for (const a of e.attributes || [])\n"
    "        if (/proj|stereo|360|180|fisheye|3d/i.test(a.name)) hints.push(a.name + ' ' + a.value);\n"
    "    out.push('hints=' + clean(hints.join(' ')));\n"
    "  }\n"
    "  try {\n"
    "    const p = document.getElementById('movie_player');\n"
    "    const r = p && p.getPlayerResponse ? p.getPlayerResponse() : window.ytInitialPlayerResponse;\n"
    "    if (r && r.streamingData) {\n"
    "      const proj = new Set(), stereo = new Set(), sizes = new Set();\n"
    "      for (const f of r.streamingData.adaptiveFormats || []) {\n"
    "        if (!/^video/.test(f.mimeType || '')) continue;\n"
    "        if (f.projectionType) proj.add(f.projectionType);\n"
    "        if (f.stereoLayout) stereo.add(f.stereoLayout);\n"
    "        if (f.width) sizes.add(f.width + 'x' + f.height);\n"
    "      }\n"
    "      out.push('yt=' + [...proj].join(' '), 'ytstereo=' + [...stereo].join(' '), 'ytsizes=' + [...sizes].slice(0, 8).join(' '));\n"
    "      if (r.videoDetails && r.videoDetails.title) out.push('yttitle=' + clean(r.videoDetails.title));\n"
    "    }\n"
    "  } catch (e) { out.push('yterror=' + clean(e)); }\n"
    "  return out.join('\\n');\n"
    "})()";

/* Play/pause or seek the remembered video (found again if it went away);
 * YouTube's player API when there is one, so its UI follows. %s = command,
 * %d = seconds. */
static const char kCommandScript[] =
    "(function (cmd, arg) {\n"
    "  let v = window.__ps5wkVRVideo;\n"
    "  if (!v || !v.isConnected) {\n"
    "    v = null;\n"
    "    for (const c of document.querySelectorAll('video')) if (!v || (v.paused && !c.paused)) v = c;\n"
    "    window.__ps5wkVRVideo = v;\n"
    "  }\n"
    "  if (!v) return 'found=0';\n"
    "  const yt = document.getElementById('movie_player');\n"
    "  const api = yt && yt.contains(v) && yt.seekTo && yt.getPlayerState ? yt : null;\n"
    "  let paused = v.paused, time = v.currentTime;\n"
    "  if (cmd === 'toggle') {\n"
    "    const playing = api ? api.getPlayerState() === 1 : !v.paused;\n"
    "    if (playing) { if (api) api.pauseVideo(); else v.pause(); }\n"
    "    else if (api) api.playVideo();\n"
    "    else { const p = v.play(); if (p && p.catch) p.catch(() => {}); }\n"
    "    paused = playing;\n"
    "  } else if (cmd === 'seek') {\n"
    "    const d = isFinite(v.duration) ? v.duration : Infinity;\n"
    "    time = Math.max(0, Math.min(d - 0.5, v.currentTime + arg));\n"
    "    if (api) api.seekTo(time, true); else v.currentTime = time;\n"
    "  }\n"
    "  return ['found=1', 'paused=' + (paused ? 1 : 0), 'time=' + time, 'duration=' + v.duration].join('\\n');\n"
    "})('%s', %d)";

/* The value of "key=" in a key=value-per-line reply (newly allocated), or NULL. */
static char* field(const char* reply, const char* key)
{
    size_t keyLength = strlen(key);
    for (const char* line = reply; line && *line;) {
        const char* end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        if (length > keyLength && !strncmp(line, key, keyLength) && line[keyLength] == '=')
            return g_strndup(line + keyLength + 1, length - keyLength - 1);
        line = end ? end + 1 : NULL;
    }
    return NULL;
}

static double fieldNumber(const char* reply, const char* key, double fallback)
{
    char* value = field(reply, key);
    char* end = NULL;
    double d = value ? g_ascii_strtod(value, &end) : fallback;
    if (!value || end == value)
        d = fallback;
    g_free(value);
    return d;
}

/* ---------------------------------------------------------------- the projection guess */

static const char* const kProjectionLabels[] = {
    "Flat screen", "360\xC2\xB0 mono", "360\xC2\xB0 3D side by side", "360\xC2\xB0 3D top/bottom",
    "180\xC2\xB0 mono", "180\xC2\xB0 3D side by side", "180\xC2\xB0 3D top/bottom", "Fisheye 3D side by side",
};

/* Triangle's order: the common VR formats first. */
static const int kCycle[] = {
    WPE_DISPLAY_PS5_VR_FLAT, WPE_DISPLAY_PS5_VR_180_SBS, WPE_DISPLAY_PS5_VR_180_TB, WPE_DISPLAY_PS5_VR_180,
    WPE_DISPLAY_PS5_VR_360, WPE_DISPLAY_PS5_VR_360_TB, WPE_DISPLAY_PS5_VR_360_SBS, WPE_DISPLAY_PS5_VR_FISHEYE_SBS,
};

static char* projectionLabel(int projection, double fisheye)
{
    if (projection == WPE_DISPLAY_PS5_VR_FISHEYE_SBS)
        return g_strdup_printf("Fisheye %.0f\xC2\xB0 3D side by side", fisheye);
    return g_strdup(projection >= 0 && projection < WPE_DISPLAY_PS5_VR_N_PROJECTIONS ? kProjectionLabels[projection] : "?");
}

/* "180"/"360" standing alone as a number (not 1180, 3600, 360p or 640x360). */
static gboolean hasAngle(const char* text, const char* angle)
{
    for (const char* p = strstr(text, angle); p; p = strstr(p + 1, angle)) {
        char before = p > text ? p[-1] : ' ', after = p[3];
        if (isdigit((unsigned char)before) || isdigit((unsigned char)after))
            continue;
        if (after == 'p' && !isalpha((unsigned char)p[4]))
            continue; /* 360p: a resolution */
        if (before == 'x' && p - text >= 2 && isdigit((unsigned char)p[-2]))
            continue; /* 640x360 */
        return TRUE;
    }
    return FALSE;
}

/* Words of the text, whole (alphanumeric runs) and split where letters
 * meet digits ("vr180" -> "vr", "180"; "mkx200" -> "mkx", "200"). */
static gboolean hasWord(char** words, const char* word)
{
    for (char** w = words; *w; w++) {
        if (!strcmp(*w, word))
            return TRUE;
    }
    return FALSE;
}

static char** splitWords(const char* lower)
{
    GPtrArray* list = g_ptr_array_new();
    const char* p = lower;
    while (*p) {
        while (*p && !isalnum((unsigned char)*p))
            p++;
        const char* start = p;
        while (isalnum((unsigned char)*p))
            p++;
        if (p > start) {
            g_ptr_array_add(list, g_strndup(start, p - start));
            /* pieces where letters and digits meet */
            const char* q = start;
            while (q < p) {
                const char* r = q;
                gboolean digit = isdigit((unsigned char)*q);
                while (r < p && !!isdigit((unsigned char)*r) == digit)
                    r++;
                if (q != start || r != p)
                    g_ptr_array_add(list, g_strndup(q, r - q));
                q = r;
            }
        }
    }
    g_ptr_array_add(list, NULL);
    return (char**)g_ptr_array_free(list, FALSE);
}

/* The number right after a word ("fisheye190", "mkx 200"), 0 if none. */
static int numberAfter(char** words, const char* word)
{
    for (char** w = words; *w; w++) {
        if (!strcmp(*w, word) && w[1] && isdigit((unsigned char)w[1][0]))
            return atoi(w[1]);
    }
    return 0;
}

static int guessProjection(const char* reply, double* fisheye, char* why, size_t whySize)
{
    *fisheye = 180;
    char* yt = field(reply, "yt");
    char* ytStereo = field(reply, "ytstereo");
    int result = -1;
    if (yt && *yt) {
        /* YouTube says: EQUIRECTANGULAR, EQUIRECTANGULAR_THREED_TOP_BOTTOM,
         * MESH (equi-angular cubemap / VR180 mesh: not unpacked yet) or
         * RECTANGULAR. */
        if (strstr(yt, "EQUIRECTANGULAR_THREED_TOP_BOTTOM") || (strstr(yt, "EQUIRECTANGULAR") && ytStereo && strstr(ytStereo, "TOP_BOTTOM")))
            result = WPE_DISPLAY_PS5_VR_360_TB;
        else if (strstr(yt, "EQUIRECTANGULAR"))
            result = WPE_DISPLAY_PS5_VR_360;
        else if (strstr(yt, "MESH"))
            result = WPE_DISPLAY_PS5_VR_360; /* best effort: the frames are a cubemap (see README) */
        else if (strstr(yt, "RECTANGULAR"))
            result = WPE_DISPLAY_PS5_VR_FLAT;
        if (result >= 0)
            snprintf(why, whySize, "YouTube projection %s%s%s", yt, ytStereo && *ytStereo ? ", " : "", ytStereo ? ytStereo : "");
    }
    g_free(yt);
    g_free(ytStereo);
    if (result >= 0)
        return result;

    /* Keywords in the title, the addresses and the element's attributes. */
    const char* keys[] = { "title", "yttitle", "url", "src", "hints" };
    GString* all = g_string_new(NULL);
    for (unsigned i = 0; i < G_N_ELEMENTS(keys); i++) {
        char* value = field(reply, keys[i]);
        if (value) {
            char* unescaped = g_uri_unescape_string(value, NULL);
            g_string_append_printf(all, " %s", unescaped ? unescaped : value);
            g_free(unescaped);
        }
        g_free(value);
    }
    char* lower = g_ascii_strdown(all->str, -1);
    g_string_free(all, TRUE);
    char** words = splitWords(lower);

    gboolean is180 = hasAngle(lower, "180"), is360 = hasAngle(lower, "360");
    gboolean strongSBS = hasWord(words, "sbs") || hasWord(words, "3dh") || hasWord(words, "hsbs") || hasWord(words, "fsbs")
        || hasWord(words, "sidebyside") || strstr(lower, "side by side") || strstr(lower, "side-by-side") || strstr(lower, "side_by_side");
    gboolean strongTB = hasWord(words, "3dv") || hasWord(words, "htb") || hasWord(words, "ftb") || hasWord(words, "topbottom")
        || hasWord(words, "overunder") || strstr(lower, "top-bottom") || strstr(lower, "top_bottom") || strstr(lower, "top bottom")
        || strstr(lower, "top and bottom") || strstr(lower, "over-under") || strstr(lower, "over under") || strstr(lower, "over_under");
    gboolean weakSBS = hasWord(words, "lr") || hasWord(words, "rl");
    gboolean weakTB = hasWord(words, "tb") || hasWord(words, "ou") || hasWord(words, "bt");
    gboolean mono = hasWord(words, "mono") || hasWord(words, "monoscopic") || hasWord(words, "2d");
    int fisheyeFOV = 0;
    if (hasWord(words, "fisheye"))
        fisheyeFOV = numberAfter(words, "fisheye") ? numberAfter(words, "fisheye") : 180;
    if (hasWord(words, "mkx"))
        fisheyeFOV = numberAfter(words, "mkx") ? numberAfter(words, "mkx") : 200;
    if (hasWord(words, "vrca"))
        fisheyeFOV = numberAfter(words, "vrca") ? numberAfter(words, "vrca") : 220;
    if (hasWord(words, "rf52"))
        fisheyeFOV = 190;
    if (fisheyeFOV && (fisheyeFOV < 150 || fisheyeFOV > 240))
        fisheyeFOV = 180;
    gboolean sbs = strongSBS || ((is180 || is360 || fisheyeFOV) && weakSBS);
    gboolean tb = strongTB || ((is180 || is360) && weakTB);

    if (fisheyeFOV) {
        result = WPE_DISPLAY_PS5_VR_FISHEYE_SBS;
        *fisheye = fisheyeFOV;
    } else if (is180)
        /* VR180 is nearly always stereo; side by side unless it says otherwise. */
        result = tb ? WPE_DISPLAY_PS5_VR_180_TB : mono && !sbs ? WPE_DISPLAY_PS5_VR_180 : WPE_DISPLAY_PS5_VR_180_SBS;
    else if (is360)
        result = tb ? WPE_DISPLAY_PS5_VR_360_TB : sbs ? WPE_DISPLAY_PS5_VR_360_SBS : WPE_DISPLAY_PS5_VR_360;
    else if (strongSBS)
        result = WPE_DISPLAY_PS5_VR_180_SBS; /* VR sites' "SBS" without an angle */
    else if (strongTB)
        result = WPE_DISPLAY_PS5_VR_180_TB;
    else
        result = WPE_DISPLAY_PS5_VR_FLAT;
    snprintf(why, whySize, "keywords:%s%s%s%s%s%s%s", is180 ? " 180" : "", is360 ? " 360" : "", sbs ? " sbs" : "", tb ? " tb" : "",
        mono ? " mono" : "", fisheyeFOV ? " fisheye" : "", result == WPE_DISPLAY_PS5_VR_FLAT ? " none (flat)" : "");
    g_strfreev(words);
    g_free(lower);
    return result;
}

/* ---------------------------------------------------------------- hint and TV notice */

static void formatTime(double seconds, char* out, size_t size)
{
    if (seconds != seconds || seconds < 0 || seconds > 1e9) { /* NaN, Infinity (live) */
        snprintf(out, size, "--:--");
        return;
    }
    long s = (long)seconds;
    if (s >= 3600)
        snprintf(out, size, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    else
        snprintf(out, size, "%ld:%02ld", s / 60, s % 60);
}

#define CONTROLS_PLAYER "\xE2\x9C\x95 play/pause   \xE2\x97\x80 \xE2\x96\xB6 seek 10 s"
#define CONTROLS_VIEW "\xE2\x96\xB3 projection   \xE2\x96\xA1 recentre   \xE2\x97\x8B exit"
#define CONTROLS CONTROLS_PLAYER "    " CONTROLS_VIEW

/* status: e.g. "Playing  1:23 / 10:00" (NULL: none). */
static void showHint(const char* status, double seconds)
{
    char* label = projectionLabel(vrProjection, vrFisheye);
    char* hint = g_strdup_printf("%s\n%s%s%s", label, status ? status : "", status ? "\n" : "",
        vrControls ? CONTROLS_PLAYER "\n" CONTROLS_VIEW : CONTROLS_VIEW "\n(no player controls on this page)");
    char* tv = g_strdup_printf("Playing in the PS VR2 headset\n%s\n%s\n%s", vrTitle && *vrTitle ? vrTitle : "Video", label,
        vrControls ? CONTROLS : "\xE2\x97\x8B back to the TV");
    wpe_display_ps5_vr_set_text(vrDisplay, hint, seconds, tv);
    g_free(hint);
    g_free(tv);
    g_free(label);
}

static void statusFromReply(const char* reply, const char* prefix, char* out, size_t size)
{
    char now[16], total[16];
    formatTime(fieldNumber(reply, "time", -1), now, sizeof now);
    formatTime(fieldNumber(reply, "duration", -1), total, sizeof total);
    gboolean paused = fieldNumber(reply, "paused", 0) != 0;
    snprintf(out, size, "%s%s%s  %s / %s", prefix ? prefix : "", prefix ? "  \xC2\xB7  " : "", paused ? "Paused" : "Playing", now, total);
}

/* ---------------------------------------------------------------- commands */

static void onCommandDone(GObject* object, GAsyncResult* result, gpointer data)
{
    char* prefix = data;
    GError* error = NULL;
    JSCValue* value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(object), result, &error);
    char* reply = value ? jsc_value_to_string(value) : NULL;
    if (!reply || fieldNumber(reply, "found", 0) == 0) {
        spike_log("vr: command: %s", error ? error->message : "the video is gone");
        if (vrOn)
            showHint("The page's video is gone", 4);
    } else {
        char status[160];
        statusFromReply(reply, prefix, status, sizeof status);
        spike_log("vr: %s", status);
        if (vrOn)
            showHint(status, 4);
    }
    g_free(reply);
    if (value)
        g_object_unref(value);
    g_clear_error(&error);
    g_free(prefix);
}

static void pageCommand(const char* command, int seconds, const char* prefix)
{
    char script[sizeof kCommandScript + 64];
    snprintf(script, sizeof script, kCommandScript, command, seconds);
    webkit_web_view_evaluate_javascript(vrWebView, script, -1, NULL, "ps5wk-vr-command.js", NULL, onCommandDone, g_strdup(prefix));
}

static void setProjection(int projection, double fisheye)
{
    vrProjection = projection;
    vrFisheye = fisheye;
    wpe_display_ps5_vr_set_projection(vrDisplay, (WPEDisplayPS5VRProjection)projection, fisheye);
    char* label = projectionLabel(projection, fisheye);
    spike_log("vr: projection %s", label);
    g_free(label);
    showHint(NULL, 4);
}

static void nextProjection(void)
{
    unsigned n = G_N_ELEMENTS(kCycle), i = 0;
    while (i < n && kCycle[i] != vrProjection)
        i++;
    int next = kCycle[(i + 1) % n];
    /* fisheye: keep the guessed field of view, else the common 190 */
    setProjection(next, next == WPE_DISPLAY_PS5_VR_FISHEYE_SBS && vrFisheye == 180 ? 190 : vrFisheye);
}

/* ---------------------------------------------------------------- enter / leave */

static void onVRState(WPEDisplayPS5* display, gboolean active, const char* message, gpointer data)
{
    (void)display;
    (void)data;
    vrStarting = FALSE;
    vrOn = active;
    if (active) {
        SPIKE_PASS("vr", "headset shows the video (%s)", message && *message ? message : "up");
        showHint(NULL, 6);
        return;
    }
    SPIKE_RESULT("vr", "INFO", "not in VR: %s", message ? message : "");
    if (message && *message)
        wpe_display_ps5_show_message(vrDisplay, message, 5);
}

static void onProbeDone(GObject* object, GAsyncResult* result, gpointer data)
{
    (void)data;
    GError* error = NULL;
    JSCValue* value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(object), result, &error);
    char* reply = value ? jsc_value_to_string(value) : NULL;
    if (value)
        g_object_unref(value);
    if (!reply) {
        /* No script (page gone, or it threw): the title may still have a
         * video stream (e.g. a cross-origin player frame); try without
         * player controls. */
        spike_log("vr: page probe failed: %s", error ? error->message : "?");
        reply = g_strdup("");
    }
    g_clear_error(&error);
    char* oneLine = g_strdup(reply);
    for (char* p = oneLine; *p; p++)
        if (*p == '\n')
            *p = ' ';
    spike_log("vr: page: %s", oneLine);
    g_free(oneLine);

    char why[160] = "";
    double fisheye = 180;
    int projection = guessProjection(reply, &fisheye, why, sizeof why);
    vrControls = fieldNumber(reply, "found", 0) != 0;
    g_free(vrTitle);
    vrTitle = field(reply, "yttitle");
    if (!vrTitle || !*vrTitle) {
        g_free(vrTitle);
        vrTitle = field(reply, "title");
    }
    if (!vrTitle)
        vrTitle = g_strdup(webkit_web_view_get_title(vrWebView) ? webkit_web_view_get_title(vrWebView) : "");
    if (strlen(vrTitle) > 90)
        strcpy(vrTitle + 87, "...");
    vrProjection = projection;
    vrFisheye = fisheye;
    char* label = projectionLabel(projection, fisheye);
    spike_log("vr: projection guess %s (%s); player controls %s", label, why, vrControls ? "yes" : "no (no <video> found in the page)");
    g_free(label);
    g_free(reply);
    wpe_display_ps5_vr_start(vrDisplay, (WPEDisplayPS5VRProjection)projection, fisheye, onVRState, NULL);
}

void ps5wk_vr_toggle(void)
{
    if (!vrDisplay || vrStarting)
        return;
    if (vrOn || wpe_display_ps5_vr_is_active(vrDisplay)) {
        ps5wk_vr_stop(FALSE, "asked");
        return;
    }
    vrStarting = TRUE;
    spike_log("vr: View in VR asked");
    webkit_web_view_evaluate_javascript(vrWebView, kProbeScript, -1, NULL, "ps5wk-vr-probe.js", NULL, onProbeDone, NULL);
}

void ps5wk_vr_stop(gboolean wait, const char* why)
{
    if (!vrDisplay || (!vrOn && !vrStarting && !wpe_display_ps5_vr_is_active(vrDisplay)))
        return;
    spike_log("vr: back to the TV (%s)", why ? why : "");
    wpe_display_ps5_vr_stop(vrDisplay, wait);
    vrOn = FALSE;
    vrStarting = FALSE;
}

gboolean ps5wk_vr_active(void)
{
    return vrOn;
}

static gboolean onButtons(WPEDisplayPS5* display, guint32 pressed, gpointer data)
{
    (void)display;
    (void)data;
    if (!vrOn && !vrStarting) {
        if (pressed & WPE_DISPLAY_PS5_BUTTON_TOUCHPAD) {
            ps5wk_vr_toggle();
            return TRUE;
        }
        return FALSE;
    }
    if (vrStarting)
        return TRUE; /* the headset is coming up: the pad is ours already */
    if (pressed & (WPE_DISPLAY_PS5_BUTTON_CIRCLE | WPE_DISPLAY_PS5_BUTTON_TOUCHPAD))
        ps5wk_vr_stop(FALSE, "pad");
    else if (pressed & WPE_DISPLAY_PS5_BUTTON_CROSS)
        vrControls ? pageCommand("toggle", 0, NULL) : showHint(NULL, 4);
    else if (pressed & WPE_DISPLAY_PS5_BUTTON_LEFT)
        vrControls ? pageCommand("seek", -10, "-10 s") : showHint(NULL, 4);
    else if (pressed & WPE_DISPLAY_PS5_BUTTON_RIGHT)
        vrControls ? pageCommand("seek", 10, "+10 s") : showHint(NULL, 4);
    else if (pressed & WPE_DISPLAY_PS5_BUTTON_TRIANGLE)
        nextProjection();
    else if (pressed & WPE_DISPLAY_PS5_BUTTON_SQUARE) {
        wpe_display_ps5_vr_recenter(vrDisplay);
        showHint("Recentred", 2);
    } else if (pressed & WPE_DISPLAY_PS5_BUTTON_OPTIONS)
        vrControls ? pageCommand("status", 0, NULL) : showHint(NULL, 6);
    return TRUE;
}

void ps5wk_vr_init(WPEDisplay* display, WebKitWebView* webView)
{
    vrDisplay = WPE_DISPLAY_PS5(display);
    vrWebView = webView;
    wpe_display_ps5_set_button_handler(vrDisplay, onButtons, NULL);
}

/* ---------------------------------------------------------------- log-only self-test */

/* Each step runs 1.5 s after the previous one; captures are the headset's
 * eye buffers (both eyes, quarter size) in /data/ps5webkit/shots. */
int psvr2_probe(int user_id, uint32_t* status, int* worn) __attribute__((weak));
void psvr2_set_logger(void (*fn)(const char*)) __attribute__((weak));
int sceUserServiceGetInitialUser(int32_t* user) __attribute__((weak));

static unsigned selftestStep, selftestShots, selftestCycles;
static gint64 selftestEnterStart;

static void selftestShot(const char* name)
{
    char path[160];
    snprintf(path, sizeof path, "/data/ps5webkit/shots/vr-%02u-%s.ppm", ++selftestShots, name);
    wpe_display_ps5_capture_screen(vrDisplay, path);
}

static void logLine(const char* line)
{
    spike_log("%s", line);
}

static gboolean selftestTick(gpointer data)
{
    (void)data;
    static const struct { const char* name; double yaw, pitch; } looks[] = {
        { "front", 0, 0 }, { "right90", 90, 0 }, { "back", 180, 0 }, { "left90", -90, 0 }, { "up60", 0, 60 }, { "down60", 0, -60 },
    };
    unsigned step = selftestStep++;
    unsigned nLooks = G_N_ELEMENTS(looks), nProj = G_N_ELEMENTS(kCycle);
    /* 0: enter */
    if (step == 0) {
        selftestEnterStart = g_get_monotonic_time();
        ps5wk_vr_toggle();
        return G_SOURCE_CONTINUE;
    }
    if (!vrOn && step < 1 + 2 * nLooks + 2 * nProj + 10) {
        SPIKE_FAIL("vr-selftest", "not in VR at step %u", step);
        return G_SOURCE_REMOVE;
    }
    step--;
    /* looks around in the guessed projection: set the look, then capture */
    if (step < 2 * nLooks) {
        if (step % 2 == 0)
            wpe_display_ps5_vr_sim_look(vrDisplay, looks[step / 2].yaw, looks[step / 2].pitch);
        else {
            char name[64];
            char* label = projectionLabel(vrProjection, vrFisheye);
            for (char* p = label; *p; p++)
                if (!isalnum((unsigned char)*p))
                    *p = '_';
            snprintf(name, sizeof name, "%s-%s", label, looks[step / 2].name);
            g_free(label);
            selftestShot(name);
        }
        return G_SOURCE_CONTINUE;
    }
    step -= 2 * nLooks;
    /* every projection, front, after Triangle */
    if (step < 2 * nProj) {
        if (step == 0)
            wpe_display_ps5_vr_sim_look(vrDisplay, 20, 10);
        if (step % 2 == 0)
            nextProjection();
        else {
            char name[64];
            snprintf(name, sizeof name, "proj%u-look20-10", (unsigned)vrProjection);
            selftestShot(name);
        }
        return G_SOURCE_CONTINUE;
    }
    step -= 2 * nProj;
    /* player controls and recentre */
    switch (step) {
    case 0: onButtons(vrDisplay, WPE_DISPLAY_PS5_BUTTON_CROSS, NULL); return G_SOURCE_CONTINUE;    /* pause */
    case 1: onButtons(vrDisplay, WPE_DISPLAY_PS5_BUTTON_RIGHT, NULL); return G_SOURCE_CONTINUE;    /* +10 s */
    case 2: onButtons(vrDisplay, WPE_DISPLAY_PS5_BUTTON_LEFT, NULL); return G_SOURCE_CONTINUE;     /* -10 s */
    case 3: onButtons(vrDisplay, WPE_DISPLAY_PS5_BUTTON_CROSS, NULL); return G_SOURCE_CONTINUE;    /* play */
    case 4: wpe_display_ps5_vr_sim_look(vrDisplay, 45, 0); return G_SOURCE_CONTINUE;
    case 5: onButtons(vrDisplay, WPE_DISPLAY_PS5_BUTTON_SQUARE, NULL); return G_SOURCE_CONTINUE;   /* recentre at 45 */
    case 6: selftestShot("recentred-at-45"); return G_SOURCE_CONTINUE;
    case 7: onButtons(vrDisplay, WPE_DISPLAY_PS5_BUTTON_OPTIONS, NULL); return G_SOURCE_CONTINUE;
    case 8: pageCommand("seek", -40, "-40 s (test)"); return G_SOURCE_CONTINUE; /* time left for the re-entries */
    case 9: onButtons(vrDisplay, WPE_DISPLAY_PS5_BUTTON_CIRCLE, NULL); return G_SOURCE_CONTINUE;   /* leave */
    default: break;
    }
    step -= 10;
    /* then enter and leave again, three times */
    if (selftestCycles < 3) {
        if (step % 3 == 0)
            ps5wk_vr_toggle();
        else if (step % 3 == 1) {
            if (!vrOn) {
                SPIKE_FAIL("vr-selftest", "re-entry %u did not reach VR", selftestCycles + 1);
                return G_SOURCE_REMOVE;
            }
            ps5wk_vr_toggle();
        } else
            selftestCycles++;
        return G_SOURCE_CONTINUE;
    }
    SPIKE_PASS("vr-selftest", "%u eye-buffer captures, %u projections, controls, 4 entries in %.0f s; VR %s now",
        selftestShots, nProj, (g_get_monotonic_time() - selftestEnterStart) / 1e6, vrOn ? "STILL ON" : "off");
    /* Hmd2 itself, without VR mode (nothing shows in a headset): binding, the
     * param.json attributes, open/close. Only with /data/ps5webkit/vr-probe. */
    int fd = open("/data/ps5webkit/vr-probe", O_RDONLY);
    if (fd >= 0) {
        close(fd);
        if (psvr2_probe) {
            psvr2_set_logger(logLine);
            int32_t user = -1;
            if (sceUserServiceGetInitialUser)
                sceUserServiceGetInitialUser(&user);
            for (int i = 0; i < 2; i++) {
                uint32_t status = 0;
                int worn = 0;
                int rc = psvr2_probe(user, &status, &worn);
                SPIKE_RESULT("vr-probe", rc == 0 ? "PASS" : "INFO", "Hmd2 open %s (rc 0x%08x, device status %u, worn %d)",
                    rc == 0 ? "ok" : "refused", (unsigned)rc, status, worn);
            }
        } else
            SPIKE_SKIP("vr-probe", "no psvr2 in this build");
    }
    return G_SOURCE_REMOVE;
}

static gboolean selftestStart(gpointer data)
{
    (void)data;
    selftestTick(NULL);
    g_timeout_add(1500, selftestTick, NULL);
    return G_SOURCE_REMOVE;
}

void ps5wk_vr_selftest_schedule(unsigned delaySeconds)
{
    static gboolean scheduled;
    int fd = open("/data/ps5webkit/vr-selftest", O_RDONLY);
    if (fd < 0 || scheduled) {
        if (fd >= 0)
            close(fd);
        return;
    }
    close(fd);
    scheduled = TRUE;
    fd = open("/data/ps5webkit/vr-sim", O_RDONLY);
    if (fd < 0) {
        SPIKE_SKIP("vr-selftest", "needs /data/ps5webkit/vr-sim (it never drives a real headset)");
        return;
    }
    close(fd);
    mkdir("/data/ps5webkit/shots", 0777);
    spike_log("vr: self-test in %u s", delaySeconds);
    g_timeout_add_seconds(delaySeconds, selftestStart, NULL);
}
