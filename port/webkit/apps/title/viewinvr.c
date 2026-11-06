/*
 * View in VR (ps5browser): the <video> playing in the page, in the PS VR2
 * headset.
 *
 * The touchpad button asks the page which video plays (a script in the
 * page: the playing <video>, largest first, in the page and same-origin
 * frames) and the title for small thumbnails of the playing video. The
 * projection: YouTube's own data when it has some, else keywords in the page
 * title, the address and the stream's address (`180`, `360`, `SBS`/`LR`,
 * `TB`/`OU`, `fisheye`, `MKX200`), else the pictures and the frame's shape
 * (vrlayout.c: halves that match are stereo, a 2:1 picture whose edges join
 * is 360, black corners are fisheye; a 2:1 frame is 180 side by side when
 * nothing decides). A Triangle choice is remembered per site, frame shape
 * and guess (/data/ps5webkit/vr-projections.txt). Then the display takes
 * the video into the headset
 * (wpe-ps5 PS5RendererVR.cpp: the title draws the same decoded frames there;
 * the page keeps playing). While in VR the pad is ours:
 *
 *   X                play / pause (the page's video element, so the site's
 *                    player stays in sync)
 *   D-pad left/right skip -30 s / +30 s (held: on and on, faster after 1.5 s)
 *   right stick      scrub: left and right move the seek target, at full
 *                    tilt a twelfth of the video a second (at least 1 min/s)
 *   Triangle         next projection
 *   Square           recentre (the video's front where you look)
 *   O                cancels a seek being chosen, else as below
 *   O, Options or    back to the TV (playback continues)
 *   touchpad
 *
 * Skips and scrubbing choose a target first, shown on the seek bar with its
 * time; it is sent to the page 0.6 s after the last skip, or 0.5 s after the
 * stick is let go (YouTube's seeks are slow: one seek, not one per press).
 * The player panel (browserdraw.c bui_draw_vr_panel: the title, the
 * projection, play state, the seek bar, the controls) shows in the headset
 * on entry and for 4 s after a press, and stays while paused or seeking;
 * the page is asked for its time every second while it shows (every 2 s
 * otherwise, for the TV). The TV shows a card meanwhile (bui_draw_vr_tv):
 * the same bar and how to get back. Without a
 * connected headset the shortcut shows "PS VR2 not connected" on the TV and
 * stays there. VR also ends by itself when the headset is unplugged, when no
 * head pose arrives within 15 s, and when the system holds the pad for 10 s
 * (wpe-ps5), so the title is never left in VR mode with no way out.
 *
 * /data/ps5webkit/vr-selftest (with /data/ps5webkit/vr-sim, so no headset is
 * touched): a scripted log-only run after the first test page loads.
 */
#define SPIKE_NAME "ps5browser"
#include "common/spike.h"
#include "viewinvr.h"
#include "browserdraw.h"
#include "vrlayout.h"

#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <sys/stat.h>

/* What the page says: YouTube's projection data, or keywords in the title,
 * the addresses and the element's attributes. */
typedef struct {
    int youtube;    /* projection from YouTube's player data, -1: none */
    int angle;      /* 180, 360 or 0 */
    int layout;     /* LAYOUT_* from a stereo word, LAYOUT_ANY: none */
    int fisheyeFOV; /* a fisheye lens named (degrees), 0: none */
    char words[96]; /* what was found, for the log */
} PageHints;

enum { LAYOUT_ANY, LAYOUT_SBS, LAYOUT_TB, LAYOUT_MONO };

static WPEDisplayPS5* vrDisplay;
static WebKitWebView* vrWebView;
static gboolean vrOn;       /* the headset shows the video */
static gboolean vrStarting; /* asked, not answered yet */
static int vrProjection;
static double vrFisheye = 180;
static gboolean vrControls; /* the page's video element was found */
static char* vrTitle;

/* The projection guess of this entry (see guessProjection, decide). */
static unsigned vrSeq;              /* entry attempt: later answers of an older one are dropped */
static char* vrProbeReply;          /* the page's answer, until the thumbnails are in too */
static gboolean vrHaveThumbnails;
static PageHints vrHints;
static VRLayoutStats vrStats;       /* the video's thumbnails seen this entry */
static int vrVideoWidth, vrVideoHeight;
static int vrGuess;                 /* what the guess gave (before a remembered choice) */
static gboolean vrGuessConclusive;  /* the page or the pictures decided (not the frame's shape alone) */
static gboolean vrPicked;           /* Triangle this entry: remembered when leaving */
static gboolean vrRemembered;       /* started from a remembered choice */
static char* vrOrigin;
static char vrAspect[8];
static char vrShown[128];           /* why, for the hint and the TV */
static unsigned vrRefines;          /* thumbnails asked for again in VR */

/* The player: the page's last answer (the time moves on from vrTimeAt while
 * playing), the seek target being chosen, the panel and the TV's card. */
static gboolean vrPaused;
static double vrTime, vrDuration = NAN, vrBuffered = -1;
static gint64 vrTimeAt;
static gboolean vrSeeking;   /* a target is being chosen or was sent */
static gboolean vrSeekSent;  /* sent: waiting for the page's answer */
static unsigned vrSeekSeq;   /* the answer of the newest send ends the seek */
static double vrTarget;
static gint64 vrCommitAt;    /* sent then; 0 while the stick holds it */
static guint32 vrHeld;       /* the buttons held (onCapturedPad) */
static gint64 vrHoldSince, vrHoldNext, vrStickAt;
static gboolean vrPolling;
static gint64 vrPolledAt;
static char vrMessage[160];
static gint64 vrMessageUntil;
static gint64 vrPanelUntil;  /* the panel shows until then (and while paused or seeking) */
static guint vrTick;         /* every 50 ms in VR */
static WPEDisplayPS5Canvas* vrPanel;
static WPEDisplayPS5Canvas* vrCard;
static char vrDrawn[512];    /* what the panel and card show: redrawn when it changes */

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

/* Play/pause, seek (by seconds) or seekto (a time) the remembered video
 * (found again if it went away), or just its state; YouTube's player API
 * when there is one, so its UI follows. %s = command, %.3f = seconds. */
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
    "  } else if (cmd === 'seek' || cmd === 'seekto') {\n"
    "    const d = isFinite(v.duration) ? v.duration : Infinity;\n"
    "    time = Math.max(0, Math.min(d - 0.5, cmd === 'seek' ? v.currentTime + arg : arg));\n"
    "    if (api) api.seekTo(time, true); else v.currentTime = time;\n"
    "  }\n"
    "  let buffered = -1;\n"
    "  try {\n"
    "    for (let i = 0; i < v.buffered.length; i++)\n"
    "      if (v.buffered.start(i) <= time + 0.5 && v.buffered.end(i) >= time) buffered = v.buffered.end(i);\n"
    "  } catch (e) {}\n"
    "  return ['found=1', 'paused=' + (paused ? 1 : 0), 'time=' + time, 'duration=' + v.duration, 'buffered=' + buffered].join('\\n');\n"
    "})('%s', %.3f)";

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

G_STATIC_ASSERT(VRLAYOUT_FLAT == WPE_DISPLAY_PS5_VR_FLAT && VRLAYOUT_360 == WPE_DISPLAY_PS5_VR_360
    && VRLAYOUT_360_SBS == WPE_DISPLAY_PS5_VR_360_SBS && VRLAYOUT_360_TB == WPE_DISPLAY_PS5_VR_360_TB
    && VRLAYOUT_180 == WPE_DISPLAY_PS5_VR_180 && VRLAYOUT_180_SBS == WPE_DISPLAY_PS5_VR_180_SBS
    && VRLAYOUT_180_TB == WPE_DISPLAY_PS5_VR_180_TB && VRLAYOUT_FISHEYE_SBS == WPE_DISPLAY_PS5_VR_FISHEYE_SBS);

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

static void readHints(const char* reply, PageHints* hints)
{
    memset(hints, 0, sizeof *hints);
    hints->youtube = -1;
    char* yt = field(reply, "yt");
    char* ytStereo = field(reply, "ytstereo");
    if (yt && *yt) {
        /* YouTube says: EQUIRECTANGULAR, EQUIRECTANGULAR_THREED_TOP_BOTTOM,
         * MESH (equi-angular cubemap / VR180 mesh: not unpacked yet) or
         * RECTANGULAR. */
        if (strstr(yt, "EQUIRECTANGULAR_THREED_TOP_BOTTOM") || (strstr(yt, "EQUIRECTANGULAR") && ytStereo && strstr(ytStereo, "TOP_BOTTOM")))
            hints->youtube = WPE_DISPLAY_PS5_VR_360_TB;
        else if (strstr(yt, "EQUIRECTANGULAR"))
            hints->youtube = WPE_DISPLAY_PS5_VR_360;
        else if (strstr(yt, "MESH"))
            hints->youtube = WPE_DISPLAY_PS5_VR_360; /* best effort: the frames are a cubemap (see README) */
        else if (strstr(yt, "RECTANGULAR"))
            hints->youtube = WPE_DISPLAY_PS5_VR_FLAT;
        if (hints->youtube >= 0)
            snprintf(hints->words, sizeof hints->words, "YouTube projection %s%s%s", yt, ytStereo && *ytStereo ? ", " : "", ytStereo ? ytStereo : "");
    }
    g_free(yt);
    g_free(ytStereo);
    if (hints->youtube >= 0)
        return;

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

    hints->fisheyeFOV = fisheyeFOV;
    hints->angle = is180 ? 180 : is360 ? 360 : 0;
    hints->layout = tb ? LAYOUT_TB : sbs ? LAYOUT_SBS : mono && hints->angle ? LAYOUT_MONO : LAYOUT_ANY;
    snprintf(hints->words, sizeof hints->words, "keywords%s%s%s%s%s%s", is180 ? " 180" : "", is360 ? " 360" : "", sbs ? " sbs" : "",
        tb ? " tb" : "", mono ? " mono" : "", fisheyeFOV ? " fisheye" : "");
    g_strfreev(words);
    g_free(lower);
}

static gboolean nearRatio(double value, double target)
{
    return target > 0 && fabs(value / target - 1) < 0.04;
}

/* The projection from the page's hints first, then the pictures and the
 * frame's shape (vrlayout.c) where the page says nothing (or names an
 * angle but no stereo layout). why: for the log; shown: for the hint. */
static int guessProjection(const PageHints* hints, const VRLayoutStats* stats, int width, int height, double* fisheye,
    char* why, size_t whySize, char* shown, size_t shownSize)
{
    *fisheye = 180;
    if (hints->youtube >= 0) {
        snprintf(why, whySize, "%s", hints->words);
        snprintf(shown, shownSize, "From the page");
        return hints->youtube;
    }
    if (hints->fisheyeFOV) {
        *fisheye = hints->fisheyeFOV;
        snprintf(why, whySize, "%s %d", hints->words, hints->fisheyeFOV);
        snprintf(shown, shownSize, "From the page");
        return WPE_DISPLAY_PS5_VR_FISHEYE_SBS;
    }
    VRLayoutGuess pictures;
    vrlayout_classify(stats, width, height, &pictures);
    if (hints->angle) {
        /* An angle named: the stereo layout from a word, else from the
         * pictures, else what that angle usually is. */
        int layout = hints->layout;
        const char* from = "";
        if (layout == LAYOUT_ANY && pictures.conclusive && pictures.stereo >= 0) {
            layout = pictures.stereo == 1 ? LAYOUT_SBS : pictures.stereo == 2 ? LAYOUT_TB : LAYOUT_MONO;
            from = "pictures";
        }
        int result;
        if (hints->angle == 180)
            result = layout == LAYOUT_TB ? WPE_DISPLAY_PS5_VR_180_TB : layout == LAYOUT_MONO ? WPE_DISPLAY_PS5_VR_180 : WPE_DISPLAY_PS5_VR_180_SBS;
        else
            result = layout == LAYOUT_TB ? WPE_DISPLAY_PS5_VR_360_TB : layout == LAYOUT_SBS ? WPE_DISPLAY_PS5_VR_360_SBS : WPE_DISPLAY_PS5_VR_360;
        if (*from)
            snprintf(why, whySize, "%s; layout from the pictures: %s", hints->words, pictures.why);
        else
            snprintf(why, whySize, "%s%s", hints->words, hints->layout == LAYOUT_ANY ? (hints->angle == 180 ? " (side by side assumed)" : " (mono assumed)") : "");
        snprintf(shown, shownSize, *from ? "From the page and the picture" : "From the page");
        return result;
    }
    if (hints->layout == LAYOUT_SBS || hints->layout == LAYOUT_TB) {
        /* A stereo word without an angle: the eyes' shape tells (2:1 eyes
         * are 360, square ones 180). */
        double eye = width > 0 && height > 0 ? (double)width / height * (hints->layout == LAYOUT_SBS ? 0.5 : 2) : 1;
        gboolean is360 = nearRatio(eye, 2);
        snprintf(why, whySize, "%s, %s eyes", hints->words, is360 ? "2:1" : "square");
        snprintf(shown, shownSize, "From the page");
        if (hints->layout == LAYOUT_SBS)
            return is360 ? WPE_DISPLAY_PS5_VR_360_SBS : WPE_DISPLAY_PS5_VR_180_SBS;
        return is360 ? WPE_DISPLAY_PS5_VR_360_TB : WPE_DISPLAY_PS5_VR_180_TB;
    }
    /* Nothing on the page: the pictures (or the frame's shape alone). */
    if (pictures.projection == WPE_DISPLAY_PS5_VR_FISHEYE_SBS)
        *fisheye = pictures.fisheye;
    snprintf(why, whySize, "%s", pictures.why);
    snprintf(shown, shownSize, "Guessed: %s", pictures.why);
    return pictures.projection;
}

/* ---------------------------------------------------------------- remembered choices */

/* What the user picked with Triangle, per page origin, frame shape and
 * what the guess was: "when the guess is X for this shape on this site, show
 * Y". A small text file, newest last, at most kMemoryMax lines:
 *   <origin> <aspect key> <guess> <choice> <fisheye degrees>
 * Simulated runs (vr-sim) keep their own file. */
#define kMemoryMax 64

static const char* memoryPath(void)
{
    struct stat st;
    return stat("/data/ps5webkit/vr-sim", &st) == 0 ? "/data/ps5webkit/vr-projections-sim.txt" : "/data/ps5webkit/vr-projections.txt";
}

static char** memoryRead(void)
{
    char* contents = NULL;
    if (!g_file_get_contents(memoryPath(), &contents, NULL, NULL))
        return g_new0(char*, 1);
    char** lines = g_strsplit(contents, "\n", -1);
    g_free(contents);
    return lines;
}

static gboolean memoryLineMatches(const char* line, const char* origin, const char* aspect, int guess, int* choice, double* fisheye)
{
    char o[300], a[16];
    int g = -1, c = -1;
    double f = 180;
    if (sscanf(line, "%299s %15s %d %d %lf", o, a, &g, &c, &f) < 4)
        return FALSE;
    if (strcmp(o, origin) || strcmp(a, aspect) || g != guess || c < 0 || c >= WPE_DISPLAY_PS5_VR_N_PROJECTIONS)
        return FALSE;
    if (choice)
        *choice = c;
    if (fisheye)
        *fisheye = f >= 120 && f <= 240 ? f : 180;
    return TRUE;
}

static gboolean memoryLookup(const char* origin, const char* aspect, int guess, int* choice, double* fisheye)
{
    if (!origin || !*origin)
        return FALSE;
    char** lines = memoryRead();
    gboolean found = FALSE;
    for (char** line = lines; *line; line++)
        found = memoryLineMatches(*line, origin, aspect, guess, choice, fisheye) || found; /* the newest (last) wins */
    g_strfreev(lines);
    return found;
}

/* Remembers the choice (newest last), or forgets it when it is the guess. */
static void memoryStore(const char* origin, const char* aspect, int guess, int choice, double fisheye)
{
    if (!origin || !*origin || strlen(origin) >= 300 || strchr(origin, ' '))
        return;
    char** lines = memoryRead();
    GPtrArray* keep = g_ptr_array_new();
    for (char** line = lines; *line; line++) {
        if (**line && !memoryLineMatches(*line, origin, aspect, guess, NULL, NULL))
            g_ptr_array_add(keep, *line);
    }
    char* added = choice != guess ? g_strdup_printf("%s %s %d %d %.0f", origin, aspect, guess, choice, fisheye) : NULL;
    if (added)
        g_ptr_array_add(keep, added);
    GString* out = g_string_new(NULL);
    for (guint i = keep->len > kMemoryMax ? keep->len - kMemoryMax : 0; i < keep->len; i++)
        g_string_append_printf(out, "%s\n", (char*)g_ptr_array_index(keep, i));
    GError* error = NULL;
    if (!g_file_set_contents(memoryPath(), out->str, (gssize)out->len, &error))
        spike_log("vr: remembering the projection: %s", error ? error->message : "?");
    else
        spike_log("vr: %s for %s, %s frames, guess %d: %s", added ? "remembered" : "forgot", origin, aspect, guess,
            added ? added : "back to the guess");
    g_clear_error(&error);
    g_string_free(out, TRUE);
    g_free(added);
    g_ptr_array_free(keep, TRUE);
    g_strfreev(lines);
}

/* "https://host:port" of an address ("file://" for files), newly allocated. */
static char* originOf(const char* url)
{
    if (!url)
        return g_strdup("");
    const char* scheme = strstr(url, "://");
    if (!scheme)
        return g_strdup("");
    const char* host = scheme + 3;
    size_t length = strcspn(host, "/?#");
    char* origin = g_strdup_printf("%.*s://%.*s", (int)(scheme - url), url, (int)length, host);
    char* lower = g_ascii_strdown(origin, -1);
    g_free(origin);
    return lower;
}

/* ---------------------------------------------------------------- the player panel and the TV's card */

static gboolean seekable(void)
{
    return vrControls && isfinite(vrDuration) && vrDuration > 0;
}

/* The video's time now: the last answer, moved on while playing. */
static double currentTime(void)
{
    double t = vrTime;
    if (!vrPaused && vrTimeAt)
        t += (g_get_monotonic_time() - vrTimeAt) / 1e6;
    return isfinite(vrDuration) && vrDuration > 0 ? fmin(t, vrDuration) : t;
}

static gboolean panelVisible(gint64 now)
{
    return vrOn && (now < vrPanelUntil || vrSeeking || (vrControls && vrPaused));
}

/* Draws the panel and the card when what they show changed (the time to
 * the second, the bar to the pixel), or always when @force. */
static void updatePanel(gboolean force)
{
    if (!vrOn || !vrDisplay)
        return;
    const gint64 now = g_get_monotonic_time();
    const gboolean visible = panelVisible(now);
    const char* message = now < vrMessageUntil ? vrMessage : NULL;
    char* projection = projectionLabel(vrProjection, vrFisheye);
    BUIVRState v = { .title = vrTitle && *vrTitle ? vrTitle : "Video", .projection = projection, .message = message,
        .controls = vrControls, .paused = vrPaused, .time = currentTime(), .duration = vrDuration, .buffered = vrBuffered,
        .seeking = vrSeeking, .target = vrTarget };
    char key[sizeof vrDrawn];
    const double frac = seekable() ? v.time / vrDuration : 0, targetFrac = seekable() ? vrTarget / vrDuration : 0;
    snprintf(key, sizeof key, "%d|%s|%s|%d%d%d|%ld|%d|%ld|%ld|%d|%ld|%.40s", visible, projection, message ? message : "", vrControls, vrPaused,
        vrSeeking, (long)v.time, (int)(frac * 1100), isfinite(vrDuration) ? (long)vrDuration : -1L, (long)vrBuffered, (int)(targetFrac * 1100),
        (long)vrTarget, vrTitle ? vrTitle : "");
    if (!force && !strcmp(key, vrDrawn)) {
        g_free(projection);
        return;
    }
    snprintf(vrDrawn, sizeof vrDrawn, "%s", key);
    if (!vrPanel) {
        vrPanel = wpe_display_ps5_canvas_new(BUI_VR_PANEL_W, BUI_VR_PANEL_H);
        vrCard = wpe_display_ps5_canvas_new(BUI_VR_TV_W, BUI_VR_TV_H);
    }
    if (visible)
        bui_draw_vr_panel(vrPanel, &v);
    /* the TV: why this projection, unless something newer is said */
    v.message = message ? message : vrShown;
    bui_draw_vr_tv(vrCard, &v);
    /* "until changed": hidden by sending none */
    wpe_display_ps5_vr_set_panel(vrDisplay, visible ? vrPanel : NULL, visible ? 3600 : 0, vrCard);
    g_free(projection);
}

static void showPanel(double seconds)
{
    gint64 until = g_get_monotonic_time() + (gint64)(seconds * 1e6);
    if (until > vrPanelUntil)
        vrPanelUntil = until;
    updatePanel(FALSE);
}

/* A line under the title for @seconds (the panel shows as long). */
static void setMessage(const char* text, double seconds)
{
    snprintf(vrMessage, sizeof vrMessage, "%s", text ? text : "");
    vrMessageUntil = *vrMessage ? g_get_monotonic_time() + (gint64)(seconds * 1e6) : 0;
    showPanel(seconds);
}

static void statusFromReply(const char* reply, const char* prefix, char* out, size_t size)
{
    char now[16], total[16];
    bui_format_time(fieldNumber(reply, "time", -1), now, sizeof now);
    bui_format_time(fieldNumber(reply, "duration", -1), total, sizeof total);
    gboolean paused = fieldNumber(reply, "paused", 0) != 0;
    snprintf(out, size, "%s%s%s  %s / %s", prefix ? prefix : "", prefix ? "  \xC2\xB7  " : "", paused ? "Paused" : "Playing", now, total);
}

/* The player's state from a reply (the probe's or a command's). */
static void stateFromReply(const char* reply)
{
    vrPaused = fieldNumber(reply, "paused", 0) != 0;
    vrTime = fieldNumber(reply, "time", 0);
    vrDuration = fieldNumber(reply, "duration", NAN);
    vrBuffered = fieldNumber(reply, "buffered", -1);
    vrTimeAt = g_get_monotonic_time();
}

/* ---------------------------------------------------------------- commands */

typedef struct {
    char* label;      /* for the log */
    unsigned seekSeq; /* a seekto's: its answer ends the seek */
    gboolean poll;    /* the state, every second or two: not logged */
} CommandCall;

static void onCommandDone(GObject* object, GAsyncResult* result, gpointer data)
{
    CommandCall* call = data;
    GError* error = NULL;
    JSCValue* value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(object), result, &error);
    char* reply = value ? jsc_value_to_string(value) : NULL;
    if (call->poll)
        vrPolling = FALSE;
    const gboolean endsSeek = call->seekSeq && call->seekSeq == vrSeekSeq && vrSeekSent;
    if (!reply || fieldNumber(reply, "found", 0) == 0) {
        if (!call->poll) {
            spike_log("vr: command: %s", error ? error->message : "the video is gone");
            if (vrOn)
                setMessage("The page's video is gone", 4);
        }
        if (endsSeek)
            vrSeeking = vrSeekSent = FALSE;
    } else {
        if (endsSeek)
            vrSeeking = vrSeekSent = FALSE;
        /* a poll answered while a seek is chosen or on its way has the old time */
        if (!(call->poll && vrSeeking))
            stateFromReply(reply);
        if (!call->poll) {
            char status[160];
            statusFromReply(reply, call->label, status, sizeof status);
            spike_log("vr: %s", status);
        }
    }
    updatePanel(FALSE);
    g_free(reply);
    if (value)
        g_object_unref(value);
    g_clear_error(&error);
    g_free(call->label);
    g_free(call);
}

static void runCommand(const char* command, double seconds, const char* label, gboolean poll, unsigned seekSeq)
{
    char script[sizeof kCommandScript + 64];
    snprintf(script, sizeof script, kCommandScript, command, seconds);
    CommandCall* call = g_new0(CommandCall, 1);
    call->label = g_strdup(label);
    call->poll = poll;
    call->seekSeq = seekSeq;
    webkit_web_view_evaluate_javascript(vrWebView, script, -1, NULL, "ps5wk-vr-command.js", NULL, onCommandDone, call);
}

static void pageCommand(const char* command, int seconds, const char* label)
{
    runCommand(command, seconds, label, FALSE, 0);
}

/* The seek target chosen: to the page. */
static void commitSeek(void)
{
    if (!vrSeeking || vrSeekSent)
        return;
    vrSeekSent = TRUE;
    vrCommitAt = 0;
    char label[48], when[16];
    bui_format_time(vrTarget, when, sizeof when);
    snprintf(label, sizeof label, "seek to %s", when);
    runCommand("seekto", vrTarget, label, FALSE, ++vrSeekSeq);
}

/* Starts choosing a target at the time now (or goes on from the one chosen). */
static void beginSeek(void)
{
    if (!vrSeeking)
        vrTarget = currentTime();
    vrSeeking = TRUE;
    vrSeekSent = FALSE;
}

static void moveTarget(double by)
{
    vrTarget = fmax(0, fmin(vrDuration - 0.5, vrTarget + by));
}

/* D-pad left/right: 30 s, sent 0.6 s after the last one. */
static void skip(double seconds)
{
    beginSeek();
    moveTarget(seconds);
    vrCommitAt = g_get_monotonic_time() + 600000;
    showPanel(4);
}

static void cancelSeek(void)
{
    vrSeeking = vrSeekSent = FALSE;
    vrCommitAt = 0;
    spike_log("vr: seek cancelled");
    showPanel(3);
}

/* Held D-pad (on and on) and the right stick (scrubbing), at every pad
 * read while VR has the pad. */
static void onCapturedPad(WPEDisplayPS5* display, guint32 held, double rightX, double rightY, gpointer data)
{
    (void)display;
    (void)rightY;
    (void)data;
    const guint32 lr = WPE_DISPLAY_PS5_BUTTON_LEFT | WPE_DISPLAY_PS5_BUTTON_RIGHT;
    const guint32 dir = held & lr, was = vrHeld & lr;
    vrHeld = held;
    if (!vrOn || !seekable())
        return;
    const gint64 now = g_get_monotonic_time();
    if (dir != was) {
        vrHoldSince = now;
        vrHoldNext = now + 400000; /* the press itself went to onButtons */
    } else if ((dir == WPE_DISPLAY_PS5_BUTTON_LEFT || dir == WPE_DISPLAY_PS5_BUTTON_RIGHT) && now >= vrHoldNext) {
        skip(dir == WPE_DISPLAY_PS5_BUTTON_LEFT ? -30 : 30);
        vrHoldNext = now + (now - vrHoldSince > 1500000 ? 70000 : 150000);
    }
    if (rightX != 0) {
        const double dt = vrStickAt ? fmin((now - vrStickAt) / 1e6, 0.05) : 0;
        if (!vrStickAt)
            spike_log("vr: scrubbing with the right stick");
        beginSeek();
        moveTarget(rightX * fmax(60, vrDuration / 12) * dt);
        vrCommitAt = 0; /* held by the stick */
        vrStickAt = now;
        vrPanelUntil = MAX(vrPanelUntil, now + 3000000);
    } else if (vrStickAt) {
        vrStickAt = 0;
        if (vrSeeking && !vrSeekSent)
            vrCommitAt = now + 500000;
    }
}

/* Every 50 ms in VR: a chosen target sent when due, the page asked for its
 * time, the panel redrawn when what it shows changed. */
static gboolean onTick(gpointer data)
{
    (void)data;
    if (!vrOn) {
        vrTick = 0;
        return G_SOURCE_REMOVE;
    }
    const gint64 now = g_get_monotonic_time();
    if (vrSeeking && !vrSeekSent && vrCommitAt && now >= vrCommitAt)
        commitSeek();
    if (vrControls && !vrPolling && now - vrPolledAt >= (panelVisible(now) ? 1000000 : 2000000)) {
        vrPolling = TRUE;
        vrPolledAt = now;
        runCommand("state", 0, NULL, TRUE, 0);
    }
    updatePanel(FALSE);
    return G_SOURCE_CONTINUE;
}

/* Entering: the probe's answer is the first state. */
static void resetPlayer(const char* reply)
{
    vrSeeking = vrSeekSent = vrPolling = FALSE;
    vrCommitAt = vrStickAt = vrPolledAt = 0;
    vrHeld = 0;
    vrMessage[0] = 0;
    vrMessageUntil = vrPanelUntil = 0;
    vrDrawn[0] = 0;
    vrPaused = FALSE;
    vrTime = 0;
    vrDuration = NAN;
    vrBuffered = -1;
    vrTimeAt = 0;
    if (reply && fieldNumber(reply, "found", 0) != 0)
        stateFromReply(reply);
}

static void setProjection(int projection, double fisheye)
{
    vrProjection = projection;
    vrFisheye = fisheye;
    wpe_display_ps5_vr_set_projection(vrDisplay, (WPEDisplayPS5VRProjection)projection, fisheye);
    char* label = projectionLabel(projection, fisheye);
    spike_log("vr: projection %s", label);
    g_free(label);
    setMessage(vrShown, 4);
}

static void nextProjection(void)
{
    unsigned n = G_N_ELEMENTS(kCycle), i = 0;
    while (i < n && kCycle[i] != vrProjection)
        i++;
    int next = kCycle[(i + 1) % n];
    vrPicked = TRUE;
    snprintf(vrShown, sizeof vrShown, next == vrGuess ? "The guess (the site's choice is forgotten when you leave)"
                                                      : "Your choice: remembered for this site when you leave");
    /* fisheye: keep the guessed field of view, else the common 190 */
    setProjection(next, next == WPE_DISPLAY_PS5_VR_FISHEYE_SBS && vrFisheye == 180 ? 190 : vrFisheye);
}

/* ---------------------------------------------------------------- enter / leave */

/* Leaving: a Triangle choice is remembered for the site (or forgotten
 * when it went back to the guess). */
static void rememberChoice(void)
{
    if (!vrPicked)
        return;
    vrPicked = FALSE;
    memoryStore(vrOrigin, vrAspect, vrGuess, vrProjection, vrFisheye);
}

static void onVRState(WPEDisplayPS5* display, gboolean active, const char* message, gpointer data)
{
    (void)display;
    if (active && GPOINTER_TO_UINT(data) != vrSeq) {
        /* Left (O, another page, the wind-down) while the headset was
         * coming up: the render thread may have taken the stop after the
         * start, so make sure it is down. */
        spike_log("vr: the headset came up after leaving was asked: stopping it");
        wpe_display_ps5_vr_stop(vrDisplay, FALSE);
        return;
    }
    vrStarting = FALSE;
    vrOn = active;
    if (active) {
        SPIKE_PASS("vr", "headset shows the video (%s)", message && *message ? message : "up");
        setMessage(vrShown, 6);
        if (!vrTick)
            vrTick = g_timeout_add(50, onTick, NULL);
        return;
    }
    rememberChoice();
    SPIKE_RESULT("vr", "INFO", "not in VR: %s", message ? message : "");
    if (message && *message)
        wpe_display_ps5_show_message(vrDisplay, message, 8);
}

/* The guess from what is known so far (the page's hints, the thumbnails),
 * then a remembered choice for this site, frame shape and guess. Returns
 * the projection; logs it. */
static int decide(double* fisheye, const char* when)
{
    char why[256] = "";
    int guess = guessProjection(&vrHints, &vrStats, vrVideoWidth, vrVideoHeight, fisheye, why, sizeof why, vrShown, sizeof vrShown);
    VRLayoutGuess pictures;
    vrlayout_classify(&vrStats, vrVideoWidth, vrVideoHeight, &pictures);
    vrGuessConclusive = vrHints.youtube >= 0 || vrHints.fisheyeFOV || vrHints.layout != LAYOUT_ANY || pictures.conclusive;
    vrGuess = guess;
    vrlayout_aspect_key(vrVideoWidth, vrVideoHeight, vrAspect, sizeof vrAspect);
    int projection = guess, choice = -1;
    double choiceFisheye = 180;
    vrRemembered = memoryLookup(vrOrigin, vrAspect, guess, &choice, &choiceFisheye);
    if (vrRemembered) {
        projection = choice;
        if (choice == WPE_DISPLAY_PS5_VR_FISHEYE_SBS)
            *fisheye = choiceFisheye;
        snprintf(vrShown, sizeof vrShown, "Remembered for this site (\xE2\x96\xB3 to change)");
    }
    char* label = projectionLabel(projection, *fisheye);
    char* guessLabel = projectionLabel(guess, *fisheye);
    int d = vrStats.detailed ? vrStats.detailed : 1;
    spike_log("vr: projection%s: %s (%s%s%s%s) [%s, %d thumbnails, %d detailed: halves %.2f, top/bottom %.2f, edges break %.0f%%, fisheye %d]",
        when, label, vrRemembered ? "remembered for this site; the guess was " : "", vrRemembered ? guessLabel : "", vrRemembered ? ": " : "", why,
        vrOrigin && *vrOrigin ? vrOrigin : "no origin", vrStats.frames, vrStats.detailed, vrStats.sbs / d, vrStats.tb / d,
        vrStats.wrap / d * 100, vrStats.fisheyeSBS);
    g_free(label);
    g_free(guessLabel);
    return projection;
}

static void onThumbnails(WPEDisplayPS5* display, const guint8* luma, guint frames, guint size, int width, int height, gpointer data);

/* In VR, when the first thumbnails could not decide (too little detail, a
 * fade, a title card): look again a little later, until the user picks. */
static gboolean refineGuess(gpointer data)
{
    if (GPOINTER_TO_UINT(data) != vrSeq || vrPicked || vrRemembered)
        return G_SOURCE_REMOVE;
    if (!vrOn)
        return vrStarting ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE; /* the headset is still coming up */
    wpe_display_ps5_video_thumbnails(vrDisplay, 3, onThumbnails, GUINT_TO_POINTER(vrSeq));
    return G_SOURCE_REMOVE;
}

static void startWhenReady(void)
{
    if (!vrProbeReply || !vrHaveThumbnails)
        return;
    char* reply = vrProbeReply;
    vrProbeReply = NULL;
    readHints(reply, &vrHints);
    vrControls = fieldNumber(reply, "found", 0) != 0;
    resetPlayer(reply);
    if (!vrVideoWidth) { /* no thumbnails: the page's size of the element's video */
        char* size = field(reply, "size");
        if (size)
            sscanf(size, "%dx%d", &vrVideoWidth, &vrVideoHeight);
        g_free(size);
    }
    g_free(vrOrigin);
    char* url = field(reply, "url");
    vrOrigin = originOf(url && *url ? url : webkit_web_view_get_uri(vrWebView));
    g_free(url);
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
    g_free(reply);

    double fisheye = 180;
    int projection = decide(&fisheye, "");
    vrProjection = projection;
    vrFisheye = fisheye;
    spike_log("vr: player controls %s", vrControls ? "yes" : "no (no <video> found in the page)");
    wpe_display_ps5_vr_start(vrDisplay, (WPEDisplayPS5VRProjection)projection, fisheye, onVRState, GUINT_TO_POINTER(vrSeq));
    if (!vrGuessConclusive && !vrRemembered)
        g_timeout_add_seconds(3, refineGuess, GUINT_TO_POINTER(vrSeq));
}

static void onThumbnails(WPEDisplayPS5* display, const guint8* luma, guint frames, guint size, int width, int height, gpointer data)
{
    (void)display;
    if (GPOINTER_TO_UINT(data) != vrSeq)
        return; /* an older entry's */
    for (guint i = 0; luma && i < frames; i++)
        vrlayout_add(&vrStats, luma + (size_t)i * size * size, (int)size);
    if (frames && width > 0 && height > 0) {
        vrVideoWidth = width;
        vrVideoHeight = height;
    }
    if (vrStarting && !vrHaveThumbnails) {
        vrHaveThumbnails = TRUE;
        startWhenReady();
        return;
    }
    /* A second look, in VR. */
    if (!vrOn || vrPicked || vrRemembered)
        return;
    vrRefines++;
    double fisheye = vrFisheye;
    int projection = decide(&fisheye, " (looked again)");
    if (projection != vrProjection || fisheye != vrFisheye) {
        setProjection(projection, fisheye);
        setMessage("Projection guessed again", 4);
    }
    if (!vrGuessConclusive && !vrRemembered && vrRefines < 3)
        g_timeout_add_seconds(5, refineGuess, GUINT_TO_POINTER(vrSeq));
}

static void onProbeDone(GObject* object, GAsyncResult* result, gpointer data)
{
    GError* error = NULL;
    JSCValue* value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(object), result, &error);
    char* reply = value ? jsc_value_to_string(value) : NULL;
    if (value)
        g_object_unref(value);
    if (GPOINTER_TO_UINT(data) != vrSeq || !vrStarting) {
        g_free(reply);
        g_clear_error(&error);
        return;
    }
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
    g_free(vrProbeReply);
    vrProbeReply = reply;
    startWhenReady();
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
    vrSeq++;
    g_free(vrProbeReply);
    vrProbeReply = NULL;
    vrHaveThumbnails = FALSE;
    memset(&vrStats, 0, sizeof vrStats);
    vrVideoWidth = vrVideoHeight = 0;
    vrPicked = vrRemembered = FALSE;
    vrRefines = 0;
    vrShown[0] = 0;
    spike_log("vr: View in VR asked");
    /* Both answers start it: the page's (video, title, addresses) and the
     * video's thumbnails for the layout guess (about 0.4 s). */
    webkit_web_view_evaluate_javascript(vrWebView, kProbeScript, -1, NULL, "ps5wk-vr-probe.js", NULL, onProbeDone, GUINT_TO_POINTER(vrSeq));
    wpe_display_ps5_video_thumbnails(vrDisplay, 3, onThumbnails, GUINT_TO_POINTER(vrSeq));
}

void ps5wk_vr_stop(gboolean wait, const char* why)
{
    if (!vrDisplay || (!vrOn && !vrStarting && !wpe_display_ps5_vr_is_active(vrDisplay)))
        return;
    spike_log("vr: back to the TV (%s)", why ? why : "");
    rememberChoice();
    wpe_display_ps5_vr_stop(vrDisplay, wait);
    vrOn = FALSE;
    vrStarting = FALSE;
    vrSeq++; /* answers still on their way are an old entry's */
}

gboolean ps5wk_vr_active(void)
{
    return vrOn;
}

/* O, Options or the touchpad: back to the TV, at any time in VR (also while
 * the headset comes up). */
#define LEAVE_BUTTONS (WPE_DISPLAY_PS5_BUTTON_CIRCLE | WPE_DISPLAY_PS5_BUTTON_OPTIONS | WPE_DISPLAY_PS5_BUTTON_TOUCHPAD)

static gboolean onButtons(WPEDisplayPS5* display, guint32 pressed, gpointer data)
{
    (void)display;
    (void)data;
    if (!vrOn && !vrStarting) {
        if (pressed & WPE_DISPLAY_PS5_BUTTON_TOUCHPAD) {
            spike_log("vr: touchpad pressed: View in VR");
            ps5wk_vr_toggle();
            return TRUE;
        }
        return FALSE;
    }
    /* Every press in VR is logged: whether presses reach the title at all
     * (2026-11-02: none was seen in 466 s of VR mode without a headset). */
    spike_log("vr: pad 0x%x in VR%s", (unsigned)pressed, vrStarting ? " (headset coming up)" : "");
    if ((pressed & WPE_DISPLAY_PS5_BUTTON_CIRCLE) && vrOn && vrSeeking && !vrSeekSent) {
        cancelSeek(); /* O with a target chosen: back to where it was */
        return TRUE;
    }
    if (pressed & LEAVE_BUTTONS) {
        ps5wk_vr_stop(FALSE, vrStarting ? "pad, while the headset came up" : "pad");
        return TRUE;
    }
    if (vrStarting)
        return TRUE; /* the headset is coming up: the pad is ours already */
    if (pressed & WPE_DISPLAY_PS5_BUTTON_CROSS) {
        if (vrControls) {
            commitSeek(); /* a target chosen goes first */
            pageCommand("toggle", 0, NULL);
        }
        showPanel(4);
    } else if (pressed & (WPE_DISPLAY_PS5_BUTTON_LEFT | WPE_DISPLAY_PS5_BUTTON_RIGHT)) {
        if (seekable())
            skip(pressed & WPE_DISPLAY_PS5_BUTTON_LEFT ? -30 : 30);
        else
            showPanel(4);
    } else if (pressed & WPE_DISPLAY_PS5_BUTTON_TRIANGLE)
        nextProjection();
    else if (pressed & WPE_DISPLAY_PS5_BUTTON_SQUARE) {
        wpe_display_ps5_vr_recenter(vrDisplay);
        setMessage("Recentred", 3);
    } else
        showPanel(4);
    return TRUE;
}

/* The browser's tabs: the page shown is now this view's (VR's page
 * commands and probes go to it). */
void ps5wk_vr_set_web_view(WebKitWebView* webView)
{
    vrWebView = webView;
}

/* The browser's button handler (browserui.c) calls this first. */
gboolean ps5wk_vr_buttons(guint32 pressed)
{
    return onButtons(vrDisplay, pressed, NULL);
}

void ps5wk_vr_init(WPEDisplay* display, WebKitWebView* webView)
{
    vrDisplay = WPE_DISPLAY_PS5(display);
    vrWebView = webView;
    wpe_display_ps5_set_button_handler(vrDisplay, onButtons, NULL);
    wpe_display_ps5_set_captured_pad_callback(vrDisplay, onCapturedPad, NULL);
}

/* ---------------------------------------------------------------- log-only self-test */

/* Each step runs 1.5 s after the previous one; captures are the headset's
 * eye buffers (both eyes, quarter size) in /data/ps5webkit/shots. */
int psvr2_probe(int user_id, uint32_t* status, int* worn) __attribute__((weak));
int psvr2_open(int user_id) __attribute__((weak));
void psvr2_close(void) __attribute__((weak));
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

/* Hmd2 itself, without VR mode (nothing shows in a headset): binding, the
 * param.json attributes, open/close. Only with /data/ps5webkit/vr-probe: after
 * the self-test, or on its own (no vr-sim needed) after the first test URL. */
static gboolean vrProbe(gpointer data)
{
    (void)data;
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
        /* "full" in the file: also what View in VR opens before VR mode
         * (Hmd2, VrTracker2 registration, reprojection), then closes it:
         * which param.json attribute bits each part needs. */
        char word[8] = { 0 };
        if ((fd = open("/data/ps5webkit/vr-probe", O_RDONLY)) >= 0) {
            read(fd, word, sizeof word - 1);
            close(fd);
        }
        if (!strncmp(word, "full", 4) && psvr2_open && psvr2_close) {
            int32_t user = -1;
            if (sceUserServiceGetInitialUser)
                sceUserServiceGetInitialUser(&user);
            int rc = psvr2_open(user);
            psvr2_close();
            SPIKE_RESULT("vr-probe-full", rc == 0 ? "PASS" : "INFO", "psvr2_open %s (rc %d), closed", rc == 0 ? "ok" : "failed", rc);
        }
    }
    return G_SOURCE_REMOVE;
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
    if (!vrOn && step < 1 + 2 * nLooks + 2 * nProj + 9) {
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
    case 1: onButtons(vrDisplay, WPE_DISPLAY_PS5_BUTTON_RIGHT, NULL); return G_SOURCE_CONTINUE;    /* +30 s (sent 0.6 s later) */
    case 2: onButtons(vrDisplay, WPE_DISPLAY_PS5_BUTTON_LEFT, NULL); return G_SOURCE_CONTINUE;     /* -30 s */
    case 3: onButtons(vrDisplay, WPE_DISPLAY_PS5_BUTTON_CROSS, NULL); return G_SOURCE_CONTINUE;    /* play */
    case 4: wpe_display_ps5_vr_sim_look(vrDisplay, 45, 0); return G_SOURCE_CONTINUE;
    case 5: onButtons(vrDisplay, WPE_DISPLAY_PS5_BUTTON_SQUARE, NULL); return G_SOURCE_CONTINUE;   /* recentre at 45 */
    case 6: selftestShot("recentred-at-45"); return G_SOURCE_CONTINUE;
    case 7: pageCommand("seek", -40, "-40 s (test)"); return G_SOURCE_CONTINUE; /* time left for the re-entries */
    case 8: onButtons(vrDisplay, WPE_DISPLAY_PS5_BUTTON_CIRCLE, NULL); return G_SOURCE_CONTINUE;   /* leave with O */
    default: break;
    }
    step -= 9;
    /* then enter (touchpad) and leave again, three times: with the touchpad,
     * Options and O */
    static const guint32 leave[] = { WPE_DISPLAY_PS5_BUTTON_TOUCHPAD, WPE_DISPLAY_PS5_BUTTON_OPTIONS, WPE_DISPLAY_PS5_BUTTON_CIRCLE };
    static const char* const leaveName[] = { "touchpad", "Options", "O" };
    if (selftestCycles < 3) {
        if (step % 3 == 0)
            onButtons(vrDisplay, WPE_DISPLAY_PS5_BUTTON_TOUCHPAD, NULL);
        else if (step % 3 == 1) {
            if (!vrOn) {
                SPIKE_FAIL("vr-selftest", "re-entry %u did not reach VR", selftestCycles + 1);
                return G_SOURCE_REMOVE;
            }
            onButtons(vrDisplay, leave[selftestCycles], NULL);
        } else {
            if (vrOn || wpe_display_ps5_vr_is_active(vrDisplay)) {
                SPIKE_FAIL("vr-selftest", "%s did not leave VR (re-entry %u)", leaveName[selftestCycles], selftestCycles + 1);
                return G_SOURCE_REMOVE;
            }
            spike_log("vr-selftest: %s left VR (re-entry %u)", leaveName[selftestCycles], selftestCycles + 1);
            selftestCycles++;
        }
        return G_SOURCE_CONTINUE;
    }
    SPIKE_PASS("vr-selftest", "%u eye-buffer captures, %u projections, controls, 4 entries in %.0f s; VR %s now",
        selftestShots, nProj, (g_get_monotonic_time() - selftestEnterStart) / 1e6, vrOn ? "STILL ON" : "off");
    vrProbe(NULL);
    return G_SOURCE_REMOVE;
}

/* /data/ps5webkit/vr-sim-untracked (with vr-sim and vr-selftest): the
 * simulated headset gives no head pose; VR must end by itself (no tracking
 * within 15 s) and the TV get its message. */
static unsigned untrackedSeconds;
static gboolean untrackedTick(gpointer data)
{
    (void)data;
    untrackedSeconds++;
    if (untrackedSeconds == 1 && !vrOn && !vrStarting) {
        SPIKE_FAIL("vr-selftest-untracked", "did not enter VR");
        return G_SOURCE_REMOVE;
    }
    if (vrOn || vrStarting || wpe_display_ps5_vr_is_active(vrDisplay)) {
        if (untrackedSeconds > 25) {
            SPIKE_FAIL("vr-selftest-untracked", "still in VR after %u s without a head pose; leaving", untrackedSeconds);
            ps5wk_vr_stop(FALSE, "self-test");
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }
    SPIKE_PASS("vr-selftest-untracked", "VR ended by itself %.0f s after entering without a head pose",
        (g_get_monotonic_time() - selftestEnterStart) / 1e6);
    return G_SOURCE_REMOVE;
}

static gboolean selftestStart(gpointer data)
{
    (void)data;
    struct stat st;
    if (stat("/data/ps5webkit/vr-sim-untracked", &st) == 0) {
        selftestEnterStart = g_get_monotonic_time();
        onButtons(vrDisplay, WPE_DISPLAY_PS5_BUTTON_TOUCHPAD, NULL);
        g_timeout_add(1000, untrackedTick, NULL);
        return G_SOURCE_REMOVE;
    }
    selftestTick(NULL);
    g_timeout_add(1500, selftestTick, NULL);
    return G_SOURCE_REMOVE;
}

/* /data/ps5webkit/vr-guesstest (with vr-sim): after every test URL, enter
 * VR, log the projection guess and check it against the file's lines
 * "<part of the address> <projection number>" (no line: logged only), then
 * the remembered choice: Triangle once, leave, enter again (the choice must
 * come back, "remembered"), Triangle back round to the guess, leave (the
 * choice is forgotten), enter (the guess again). */
static unsigned guessStep, guessWait;
static int guessExpected, guessChosen, guessFirst;
static char* guessURL;

static void guessCheck(const char* what, gboolean ok, const char* detail)
{
    char* label = projectionLabel(vrProjection, vrFisheye);
    SPIKE_RESULT("vr-guess", ok ? "PASS" : "FAIL", "%s %s: %s (%s)%s%s", guessURL ? guessURL : "?", what, label, vrShown, detail ? "; " : "",
        detail ? detail : "");
    g_free(label);
}

static gboolean guessTick(gpointer data)
{
    (void)data;
    /* steps that wait for the headset: up to 10 s */
    gboolean waitsForVR = guessStep == 1 || guessStep == 5 || guessStep == 9;
    if (waitsForVR && !vrOn) {
        if (++guessWait > 20) {
            SPIKE_FAIL("vr-guess", "%s: not in VR at step %u", guessURL ? guessURL : "?", guessStep);
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }
    guessWait = 0;
    switch (guessStep++) {
    case 0: /* enter */
        ps5wk_vr_toggle();
        return G_SOURCE_CONTINUE;
    case 1: case 2: case 3: case 4: case 5: case 6: case 9: case 10: /* wait (and let a second look happen) */
        return G_SOURCE_CONTINUE;
    case 7: { /* the guess */
        char detail[96];
        guessFirst = vrProjection;
        if (guessExpected >= 0) {
            snprintf(detail, sizeof detail, "expected %d", guessExpected);
            guessCheck("guess", vrProjection == guessExpected && !vrRemembered, detail);
        } else
            guessCheck("guess (no expectation)", TRUE, NULL);
        onButtons(vrDisplay, WPE_DISPLAY_PS5_BUTTON_TRIANGLE, NULL);
        guessChosen = vrProjection;
        onButtons(vrDisplay, WPE_DISPLAY_PS5_BUTTON_CIRCLE, NULL); /* leave: remembered */
        return G_SOURCE_CONTINUE;
    }
    case 8: /* enter again, once the simulated headset is down */
        if (wpe_display_ps5_vr_is_active(vrDisplay))
            guessStep--;
        else
            ps5wk_vr_toggle();
        return G_SOURCE_CONTINUE;
    case 11: { /* the choice is back; Triangle round to the guess, leave: forgotten */
        char detail[96];
        snprintf(detail, sizeof detail, "chose %d after the guess %d", guessChosen, guessFirst);
        guessCheck("remembered", vrProjection == guessChosen && vrRemembered, detail);
        for (unsigned i = 1; i < G_N_ELEMENTS(kCycle); i++)
            onButtons(vrDisplay, WPE_DISPLAY_PS5_BUTTON_TRIANGLE, NULL);
        onButtons(vrDisplay, WPE_DISPLAY_PS5_BUTTON_CIRCLE, NULL);
        int choice = -1;
        gboolean still = memoryLookup(vrOrigin, vrAspect, vrGuess, &choice, NULL);
        snprintf(detail, sizeof detail, "back at %d, the guess %d; site memory %s", vrProjection, guessFirst, still ? "STILL HAS IT" : "forgot it");
        guessCheck("forgotten", vrProjection == guessFirst && !still, detail);
        g_free(guessURL);
        guessURL = NULL;
        return G_SOURCE_REMOVE;
    }
    default:
        return G_SOURCE_REMOVE;
    }
}

static gboolean guessStart(gpointer data)
{
    (void)data;
    g_free(guessURL);
    guessURL = g_strdup(webkit_web_view_get_uri(vrWebView) ? webkit_web_view_get_uri(vrWebView) : "");
    guessExpected = -1;
    char* contents = NULL;
    if (g_file_get_contents("/data/ps5webkit/vr-guesstest", &contents, NULL, NULL)) {
        char** lines = g_strsplit(contents, "\n", -1);
        for (char** line = lines; *line; line++) {
            char part[200];
            int projection = -1;
            if (sscanf(*line, "%199s %d", part, &projection) == 2 && strstr(guessURL, part))
                guessExpected = projection;
        }
        g_strfreev(lines);
        g_free(contents);
    }
    if (vrOn || vrStarting)
        ps5wk_vr_stop(FALSE, "guess test");
    guessStep = guessWait = 0;
    spike_log("vr: guess test for %s (expected %d)", guessURL, guessExpected);
    guessTick(NULL);
    g_timeout_add(500, guessTick, NULL);
    return G_SOURCE_REMOVE;
}

void ps5wk_vr_selftest_schedule(unsigned delaySeconds)
{
    static gboolean scheduled;
    struct stat st;
    if (stat("/data/ps5webkit/vr-guesstest", &st) == 0) {
        if (stat("/data/ps5webkit/vr-sim", &st) != 0)
            SPIKE_SKIP("vr-guess", "needs /data/ps5webkit/vr-sim (it never drives a real headset)");
        else
            g_timeout_add_seconds(delaySeconds, guessStart, NULL); /* every test URL */
    }
    int fd = open("/data/ps5webkit/vr-selftest", O_RDONLY);
    if (fd < 0 || scheduled) {
        if (fd >= 0)
            close(fd);
        else if (!scheduled && (fd = open("/data/ps5webkit/vr-probe", O_RDONLY)) >= 0) {
            close(fd);
            g_timeout_add_seconds(delaySeconds, vrProbe, NULL);
        }
        scheduled = TRUE;
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
