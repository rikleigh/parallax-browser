/*
 * Pop-ups (ps5browser; README: "Pop-ups").
 *
 * WebKit is told that script may open windows by itself
 * (javascript-can-open-windows-automatically, ps5browser.c), so that every
 * new window is asked of the browser (browserui.c onCreate) instead of
 * being dropped inside the WebProcess without a word. One that comes
 * without a press is refused there, with a notice on screen, unless the
 * page's site is on the allow list kept here.
 *
 * The list: popups-allow.txt in the PS5 user's folder (profile.c), one site
 * per line, a host and its subdomains ("www." dropped), as the ad blocker's
 * adblock-allow.txt. The site is the page's, the tab's address, whichever
 * frame asked. Private browsing works on a copy that goes when it ends.
 *
 * What was blocked on the page shown (how many, the last address) is kept
 * for Settings, until the next page loads, in memory only: nothing of it is
 * written. Private browsing's start and end forget it, and another device
 * asking during private browsing is not told it (ps5wk_popups_json). The
 * log lines about it are browserui.c's: the address only in development
 * builds, and none at all while private browsing is on (titlelog.c).
 */
#define SPIKE_NAME "ps5browser"
#include "common/spike.h"
#include "popups.h"
#include "adblock.h"
#include "browser.h"
#include "profile.h"

#include <string.h>

#define POPUPS_ALLOW_NAME "popups-allow.txt"

static GMutex lock;
static GPtrArray* sites;        /* char*, sorted: the saved list */
static GPtrArray* privateSites; /* the private session's copy, or NULL */
/* the page shown: where pop-ups were blocked, how many, the last one */
static char* blockedSite;
static unsigned blockedCount;
static char* blockedLast;

static int compareStrings(gconstpointer a, gconstpointer b)
{
    return strcmp(*(char* const*)a, *(char* const*)b);
}

static char* allowFile(void)
{
    return g_build_filename(ps5wk_profile_dir(), POPUPS_ALLOW_NAME, NULL);
}

/* lock held: the list in use. */
static GPtrArray* list(void)
{
    return privateSites ? privateSites : sites;
}

/* The entry of from covering site (itself or a parent), or NULL. */
static const char* entryFor(GPtrArray* from, const char* site)
{
    if (!site || !from)
        return NULL;
    size_t siteLength = strlen(site);
    for (guint i = 0; i < from->len; i++) {
        const char* entry = g_ptr_array_index(from, i);
        size_t length = strlen(entry);
        if (!strcmp(site, entry) || (siteLength > length && site[siteLength - length - 1] == '.' && !strcmp(site + siteLength - length, entry)))
            return entry;
    }
    return NULL;
}

void ps5wk_popups_init(void)
{
    GPtrArray* read = g_ptr_array_new_with_free_func(g_free);
    char* path = allowFile();
    gchar* contents = NULL;
    if (g_file_get_contents(path, &contents, NULL, NULL)) {
        char** lines = g_strsplit(contents, "\n", -1);
        for (char** line = lines; *line; line++) {
            char* site = **line == '#' ? NULL : ps5wk_adblock_clean_site(*line);
            if (site)
                g_ptr_array_add(read, site);
        }
        g_strfreev(lines);
        g_free(contents);
    }
    g_free(path);
    g_ptr_array_sort(read, compareStrings);
    g_mutex_lock(&lock);
    if (sites)
        g_ptr_array_unref(sites);
    sites = read;
    g_mutex_unlock(&lock);
    spike_log("popups: blocked without a press, allowed on %u site%s (%s)", read->len, read->len == 1 ? "" : "s", POPUPS_ALLOW_NAME);
}

gboolean ps5wk_popups_allowed(const char* uri)
{
    char* site = ps5wk_adblock_site_for_uri(uri);
    g_mutex_lock(&lock);
    gboolean allowed = entryFor(list(), site) != NULL;
    g_mutex_unlock(&lock);
    g_free(site);
    return allowed;
}

/* lock held. */
static void forgetBlocked(void)
{
    g_clear_pointer(&blockedSite, g_free);
    g_clear_pointer(&blockedLast, g_free);
    blockedCount = 0;
}

unsigned ps5wk_popups_blocked(const char* uri, const char* popup)
{
    char* site = ps5wk_adblock_site_for_uri(uri);
    g_mutex_lock(&lock);
    if (g_strcmp0(site, blockedSite)) {
        forgetBlocked();
        blockedSite = g_strdup(site);
    }
    g_free(blockedLast);
    blockedLast = g_strdup(popup);
    unsigned count = ++blockedCount;
    g_mutex_unlock(&lock);
    g_free(site);
    return count;
}

void ps5wk_popups_page(const char* uri)
{
    /* The browser's own pages (Settings, opened to allow the site) and
     * file: pages keep what was blocked on the page before them. */
    if (!uri || (g_ascii_strncasecmp(uri, "http://", 7) && g_ascii_strncasecmp(uri, "https://", 8)))
        return;
    g_mutex_lock(&lock);
    forgetBlocked();
    g_mutex_unlock(&lock);
}

gboolean ps5wk_popups_blocked_on(const char* uri)
{
    char* site = ps5wk_adblock_site_for_uri(uri);
    g_mutex_lock(&lock);
    gboolean blocked = site && blockedCount && !g_strcmp0(site, blockedSite);
    g_mutex_unlock(&lock);
    g_free(site);
    return blocked;
}

/* lock held. */
static gboolean writeSites(void)
{
    GString* text = g_string_new("# Sites that may open pop-ups by themselves (Settings), one per line: a host and its subdomains.\n");
    for (guint i = 0; i < sites->len; i++)
        g_string_append_printf(text, "%s\n", (char*)g_ptr_array_index(sites, i));
    char* path = allowFile();
    gboolean ok = g_file_set_contents(path, text->str, (gssize)text->len, NULL);
    g_free(path);
    g_string_free(text, TRUE);
    return ok;
}

const char* ps5wk_popups_set_allowed(const char* host, gboolean allowed)
{
    char* site = ps5wk_adblock_clean_site(host);
    if (!site)
        return "not a site (a host name such as example.com)";
    g_mutex_lock(&lock);
    GPtrArray* in = list();
    guint index = 0;
    gboolean present = FALSE;
    for (; in && index < in->len && !present; index++)
        present = !strcmp(g_ptr_array_index(in, index), site);
    if (present)
        index--;
    gboolean changed = in && present != allowed;
    if (changed && allowed) {
        g_ptr_array_add(in, g_strdup(site));
        g_ptr_array_sort(in, compareStrings);
    } else if (changed)
        g_ptr_array_remove_index(in, index);
    /* The private session's copy is never written. */
    gboolean ok = in && (!changed || in != sites || writeSites());
    gboolean kept = in == sites;
    g_mutex_unlock(&lock);
    spike_log("popups: %s %s%s%s", allowed ? "allowed on" : "blocked again on", site, !changed ? " (no change)" : ok ? "" : " (could not save the list)",
        kept ? "" : " (private browsing: not saved)");
    g_free(site);
    return ok ? NULL : "could not save the list of sites where pop-ups are allowed";
}

void ps5wk_popups_set_private(gboolean enabled)
{
    g_mutex_lock(&lock);
    forgetBlocked(); /* it was the other session's page */
    if (privateSites && !enabled)
        g_clear_pointer(&privateSites, g_ptr_array_unref);
    else if (!privateSites && enabled && sites) {
        privateSites = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; i < sites->len; i++)
            g_ptr_array_add(privateSites, g_strdup(g_ptr_array_index(sites, i)));
    }
    g_mutex_unlock(&lock);
}

char* ps5wk_popups_json(const char* uri, gboolean savedOnly)
{
    /* savedOnly is another device during private browsing: nothing of the
     * private pages, whatever uri says (the caller passes none then too), so
     * no site row, no count and no address that was blocked. */
    char* site = savedOnly ? NULL : ps5wk_adblock_site_for_uri(uri);
    GString* out = g_string_new("\"popups\":{\"site\":");
    g_mutex_lock(&lock);
    GPtrArray* from = savedOnly ? sites : list();
    gboolean here = site && blockedCount && !g_strcmp0(site, blockedSite);
    if (site)
        ps5wk_json_string(out, site);
    else
        g_string_append(out, "null");
    g_string_append(out, ",\"siteAllowedBy\":");
    const char* entry = entryFor(from, site);
    if (entry)
        ps5wk_json_string(out, entry);
    else
        g_string_append(out, "null");
    g_string_append_printf(out, ",\"blocked\":%u,\"last\":", here ? blockedCount : 0);
    if (here && blockedLast)
        ps5wk_json_string(out, blockedLast);
    else
        g_string_append(out, "null");
    g_string_append(out, ",\"sites\":[");
    for (guint i = 0; from && i < from->len; i++) {
        if (i)
            g_string_append_c(out, ',');
        ps5wk_json_string(out, g_ptr_array_index(from, i));
    }
    g_mutex_unlock(&lock);
    g_string_append(out, "]},");
    g_free(site);
    return g_string_free(out, FALSE);
}
