/*
 * adblock-job.c: the ad blocker's list work in ps5wk-child, as a task payload
 * of its own (port/webkit/ADBLOCK.md, "Compile and update in a child").
 *
 * The title (apps/title/adblock.c) writes a job file and starts the child
 * ELF with the header "PS5WK-LAUNCH 1 adblock 0 <job file>"
 * (wpe_process_manager_ps5_run_task). This process then
 *   compile: compiles a content-blocker JSON file into the title's
 *            WebKitUserContentFilterStore directory under the identifier the
 *            title chose (the slot not in use), with the same WebKit code and
 *            file format, so the title only loads it (ms);
 *   update:  downloads uBO Lite's manifest.json, and if its version (or the
 *            converter's) is newer than the list in use (or the job is
 *            forced), the default rulesets at that release tag (libsoup with
 *            the payloads' TLS), converts them with dnr-to-webkit.js in
 *            JavaScriptCore, compiles the result as above and only then
 *            saves it as adblock-rules.json + adblock-info.ini;
 * and writes a result file last. The title reads the result when this
 * process's output closes; no result (a crash, a kill, the time limit) means
 * failure, and the title keeps the list in use. Everything this process
 * allocated (the compile peaks at several hundred MB) goes when it exits.
 *
 * Output: "[adblock-job] ..." lines, relayed to the title's log; lines
 * "[adblock-job] status: <text>" go to the Settings page as they are, and
 * "[adblock-job] phase: compiling" tells the title the compile started.
 * The process ends by itself when the title closes the connection (stdin
 * EOF) and after AB_JOB_LIMIT_S whatever happens.
 */
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <wpe/webkit.h>

#define AB_REPO "https://raw.githubusercontent.com/uBlockOrigin/uBOL-home"
#define AB_JOB_LIMIT_S (20 * 60)

int ps5wk_adblock_job(const char* jobPath);

extern size_t pas_page_malloc_num_allocated_bytes __attribute__((weak));
int sceKernelAvailableFlexibleMemorySize(size_t*) __attribute__((weak));

static gint64 jobStart;

#define JOBLOG(...) do { fprintf(stderr, "[adblock-job] " __VA_ARGS__); fputc('\n', stderr); } while (0)

static void status(const char* format, ...) G_GNUC_PRINTF(1, 2);
static void status(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    char* text = g_strdup_vprintf(format, args);
    va_end(args);
    for (char* c = text; *c; c++) {
        if (*c == '\n' || *c == '\r')
            *c = ' ';
    }
    fprintf(stderr, "[adblock-job] status: %s\n", text);
    g_free(text);
}

static double msSince(gint64 start)
{
    return (g_get_monotonic_time() - start) / 1000.0;
}

static void memLine(const char* what)
{
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    double pas = &pas_page_malloc_num_allocated_bytes ? pas_page_malloc_num_allocated_bytes / 1048576.0 : -1;
    JOBLOG("[mem] %s at %.1f s: peak rss %.1f MB, libpas pages %.1f MB", what, msSince(jobStart) / 1000, usage.ru_maxrss / 1024.0, pas);
}

/* The title closed the connection (it ended, or gave up on us): stop. */
static void* stdinWatcher(void* data)
{
    (void)data;
    for (;;) {
        struct pollfd p = { 0, POLLIN, 0 };
        int n = poll(&p, 1, -1);
        if (n < 0 && errno == EINTR)
            continue;
        char c;
        if (n < 0 || (p.revents & (POLLHUP | POLLERR | POLLNVAL)) || read(0, &c, 1) <= 0)
            break;
    }
    JOBLOG("the title closed the connection: ending");
    _exit(3);
    return NULL;
}

static void* timeLimit(void* data)
{
    (void)data;
    struct timespec ts = { AB_JOB_LIMIT_S, 0 };
    while (nanosleep(&ts, &ts) && errno == EINTR) { }
    JOBLOG("still running after %d s: ending (the title keeps its list)", AB_JOB_LIMIT_S);
    _exit(4);
    return NULL;
}

static gboolean writeAtomically(const char* path, const char* contents, gsize length)
{
    char* temporary = g_strdup_printf("%s.tmp", path);
    gboolean ok = FALSE;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        ok = TRUE;
        for (gsize done = 0; ok && done < length;) {
            ssize_t n = write(fd, contents + done, length - done);
            ok = n > 0;
            done += n > 0 ? (gsize)n : 0;
        }
        fchmod(fd, 0666); /* the title runs under another uid */
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

static gboolean writeKeyFile(GKeyFile* file, const char* path)
{
    gsize length = 0;
    char* text = g_key_file_to_data(file, &length, NULL);
    gboolean ok = text && writeAtomically(path, text, length);
    g_free(text);
    return ok;
}

/* Every file in dir (not recursive), or only those starting with prefix and
 * not named keep1/keep2. */
static unsigned removeFiles(const char* dir, const char* prefix, const char* keep1, const char* keep2)
{
    DIR* d = opendir(dir);
    if (!d)
        return 0;
    unsigned removed = 0;
    struct dirent* entry;
    while ((entry = readdir(d))) {
        const char* name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..") || (prefix && !g_str_has_prefix(name, prefix))
            || (keep1 && !strcmp(name, keep1)) || (keep2 && !strcmp(name, keep2)))
            continue;
        char* path = g_build_filename(dir, name, NULL);
        removed += !unlink(path);
        g_free(path);
    }
    closedir(d);
    return removed;
}

/* -1, 0, 1: dotted numbers (2026.1026.1812). */
static int compareVersions(const char* a, const char* b)
{
    if (!a || !*a || !b || !*b)
        return a && *a ? 1 : b && *b ? -1 : 0;
    char** x = g_strsplit(a, ".", -1);
    char** y = g_strsplit(b, ".", -1);
    int result = 0;
    for (int i = 0; !result && (x[i] || y[i]); i++) {
        long u = x[i] ? strtol(x[i], NULL, 10) : -1, v = y[i] ? strtol(y[i], NULL, 10) : -1;
        result = u < v ? -1 : u > v;
    }
    g_strfreev(x);
    g_strfreev(y);
    return result;
}

/* ---- downloads (libsoup, the payloads' TLS: CA bundle in /data/ps5webkit/etc/ssl) ---- */

static SoupSession* session;

/* url's body, or NULL (*missing for a 404 when optional). */
static GBytes* fetch(const char* url, gboolean optional, gboolean* missing, char** failure)
{
    if (!session)
        session = soup_session_new_with_options("timeout", 60, "idle-timeout", 30, "user-agent", "ps5webkit-adblock", NULL);
    SoupMessage* message = soup_message_new("GET", url);
    if (!message) {
        *failure = g_strdup_printf("bad URL %s", url);
        return NULL;
    }
    GError* error = NULL;
    GBytes* body = NULL;
    for (int attempt = 1; attempt <= 3 && !body; attempt++) {
        g_clear_error(&error);
        body = soup_session_send_and_read(session, message, NULL, &error);
        guint code = soup_message_get_status(message);
        if (body && code != 200) {
            g_bytes_unref(body);
            body = NULL;
            if (code == 404 && optional) {
                *missing = TRUE;
                break;
            }
            if (code >= 400 && code < 500)
                attempt = 3; /* not worth another try */
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "HTTP %u", code);
        }
        if (!body && attempt < 3 && !*missing) {
            JOBLOG("download of %s failed (%s), attempt %d; retrying", url, error ? error->message : "?", attempt);
            g_usleep(attempt * G_USEC_PER_SEC);
            g_object_unref(message);
            message = soup_message_new("GET", url);
        }
    }
    if (!body && !*missing)
        *failure = g_strdup_printf("%s for %s", error ? error->message : "failed", url);
    g_clear_error(&error);
    g_object_unref(message);
    return body;
}

/* ---- the compile (WebKit's own, as WebKitUserContentFilterStore in the title) ---- */

typedef struct {
    GMainLoop* loop;
    WebKitUserContentFilter* filter;
    GError* error;
} CompileWait;

static void compiled(GObject* object, GAsyncResult* result, gpointer data)
{
    CompileWait* wait = data;
    wait->filter = webkit_user_content_filter_store_save_from_file_finish(WEBKIT_USER_CONTENT_FILTER_STORE(object), result, &wait->error);
    g_main_loop_quit(wait->loop);
}

/* NULL, or why not. */
static char* compileList(WebKitUserContentFilterStore* store, const char* storeDir, const char* identifier, const char* path, double* ms)
{
    struct stat st;
    JOBLOG("phase: compiling");
    JOBLOG("compiling %s (%.1f MB) as %s into %s", path, stat(path, &st) ? -1.0 : st.st_size / 1048576.0, identifier, storeDir);
    memLine("compile start");
    gint64 start = g_get_monotonic_time();
    CompileWait wait = { g_main_loop_new(NULL, FALSE), NULL, NULL };
    GFile* file = g_file_new_for_path(path);
    webkit_user_content_filter_store_save_from_file(store, identifier, file, NULL, compiled, &wait);
    g_object_unref(file);
    g_main_loop_run(wait.loop);
    g_main_loop_unref(wait.loop);
    *ms = msSince(start);
    memLine("compiled");
    if (!wait.filter) {
        char* why = g_strdup(wait.error ? wait.error->message : "unknown error");
        g_clear_error(&wait.error);
        return why;
    }
    webkit_user_content_filter_unref(wait.filter);
    /* The title (another uid) maps it, and deletes or replaces it later. */
    char* compiledPath = g_strdup_printf("%s/ContentRuleList-%s", storeDir, identifier);
    if (chmod(compiledPath, 0666))
        JOBLOG("chmod %s: errno %d", compiledPath, errno);
    JOBLOG("compiled %s in %.0f ms (%.1f MB)", compiledPath, *ms, stat(compiledPath, &st) ? -1.0 : st.st_size / 1048576.0);
    g_free(compiledPath);
    return NULL;
}

/* ---- the conversion (dnr-to-webkit.js in JavaScriptCore) ---- */

static char* jscError(JSCContext* context)
{
    JSCException* exception = jsc_context_get_exception(context);
    if (!exception)
        return NULL;
    char* text = jsc_exception_report(exception);
    jsc_context_clear_exception(context);
    return text;
}

/* Long value of "key":<number> in a flat JSON object (the stats line). */
static long jsonNumber(const char* json, const char* key)
{
    char* pattern = g_strdup_printf("\"%s\":", key);
    const char* at = strstr(json, pattern);
    long value = at ? strtol(at + strlen(pattern), NULL, 10) : -1;
    g_free(pattern);
    return value;
}

typedef struct {
    char* version;
    int converter;
    long rules, dnrRules, skipped;
    gint64 generated;
    char* sha256;
    char* rulesets;
} NewList;

static void infoToKeyFile(GKeyFile* file, const NewList* info)
{
    g_key_file_set_string(file, "list", "source", "uBlock Origin Lite (github.com/uBlockOrigin/uBOL-home, GPL-3.0)");
    g_key_file_set_string(file, "list", "version", info->version ? info->version : "");
    g_key_file_set_string(file, "list", "rulesets", info->rulesets ? info->rulesets : "");
    g_key_file_set_integer(file, "list", "converter", info->converter);
    g_key_file_set_int64(file, "list", "rules", info->rules);
    g_key_file_set_int64(file, "list", "dnr-rules", info->dnrRules);
    g_key_file_set_int64(file, "list", "skipped", info->skipped);
    g_key_file_set_int64(file, "list", "generated", info->generated);
    g_key_file_set_string(file, "list", "sha256", info->sha256 ? info->sha256 : "");
    g_key_file_set_string(file, "list", "origin", "update");
}

/* Update: "ok" (a new list compiled and saved), "uptodate", or NULL with
 * *message set. */
static const char* runUpdate(GKeyFile* job, WebKitUserContentFilterStore* store, const char* storeDir, const char* identifier,
    const char* work, char** message, NewList* list, double* compileMS)
{
    const char* result = NULL;
    char* converterPath = g_key_file_get_string(job, "job", "converter", NULL);
    char* dest = g_key_file_get_string(job, "job", "dest", NULL);
    char* currentVersion = g_key_file_get_string(job, "job", "current-version", NULL);
    int currentConverter = g_key_file_get_integer(job, "job", "current-converter", NULL);
    gboolean force = g_key_file_get_boolean(job, "job", "force", NULL);
    char* failure = NULL;
    gchar* script = NULL;
    gsize scriptLength = 0;
    JSCVirtualMachine* vm = NULL;
    JSCContext* js = NULL;
    JSCValue* converter = NULL;
    GPtrArray* ids = g_ptr_array_new_with_free_func(g_free);
    char* version = NULL;
    char* converted = NULL;
    char* rulesPath = g_build_filename(work, "update-rules.json", NULL);
    GString* fetchedList = g_string_new(NULL);
    size_t fetchedBytes = 0;

    status("Checking for a new list…");
    gint64 start = g_get_monotonic_time();
    GBytes* manifest = fetch(AB_REPO "/main/chromium/manifest.json", FALSE, &(gboolean) { FALSE }, &failure);
    if (!manifest) {
        *message = g_strdup_printf("Could not check for a new list: %s", failure);
        goto done;
    }
    double manifestMS = msSince(start);

    gint64 jsStart = g_get_monotonic_time();
    GError* error = NULL;
    if (!converterPath || !g_file_get_contents(converterPath, &script, &scriptLength, &error)) {
        *message = g_strdup_printf("The converter %s is missing: %s", converterPath ? converterPath : "(none)", error ? error->message : "?");
        g_clear_error(&error);
        goto done;
    }
    vm = jsc_virtual_machine_new();
    js = jsc_context_new_with_virtual_machine(vm);
    JSCValue* ignored = jsc_context_evaluate_with_source_uri(js, script, (gssize)scriptLength, converterPath, 1);
    g_clear_object(&ignored);
    char* jsFailure = jscError(js);
    if (jsFailure) {
        *message = g_strdup_printf("The converter %s did not load: %s", converterPath, jsFailure);
        g_free(jsFailure);
        goto done;
    }
    converter = jsc_context_get_value(js, "DNRToWebKit");
    JSCValue* manifestText = jsc_value_new_string_from_bytes(js, manifest);
    JSCValue* manifestInfo = jsc_value_object_invoke_method(converter, "rulesetsFromManifest", JSC_TYPE_VALUE, manifestText, G_TYPE_NONE);
    g_object_unref(manifestText);
    char* manifestJSON = manifestInfo && jsc_value_is_string(manifestInfo) ? jsc_value_to_string(manifestInfo) : NULL;
    g_clear_object(&manifestInfo);
    jsFailure = jscError(js);
    if (!manifestJSON || jsFailure) {
        *message = g_strdup_printf("uBO Lite's manifest was not understood: %s", jsFailure ? jsFailure : "no answer");
        g_free(jsFailure);
        g_free(manifestJSON);
        goto done;
    }
    JSCValue* parsed = jsc_value_new_from_json(js, manifestJSON);
    g_free(manifestJSON);
    JSCValue* versionValue = jsc_value_object_get_property(parsed, "version");
    JSCValue* idsValue = jsc_value_object_get_property(parsed, "rulesets");
    version = jsc_value_to_string(versionValue);
    JSCValue* lengthValue = jsc_value_object_get_property(idsValue, "length");
    int count = jsc_value_to_int32(lengthValue);
    g_object_unref(lengthValue);
    for (int i = 0; i < count; i++) {
        JSCValue* item = jsc_value_object_get_property_at_index(idsValue, (guint)i);
        g_ptr_array_add(ids, jsc_value_to_string(item));
        g_object_unref(item);
    }
    g_object_unref(versionValue);
    g_object_unref(idsValue);
    g_object_unref(parsed);
    JSCValue* converterVersion = jsc_value_object_get_property(converter, "VERSION");
    list->converter = jsc_value_to_int32(converterVersion);
    g_object_unref(converterVersion);

    int c = compareVersions(version, currentVersion);
    gboolean newer = c > 0 || (!c && list->converter > currentConverter);
    JOBLOG("update check: uBO Lite %s on GitHub (converter %d), in use %s (converter %d)%s; manifest in %.0f ms, JavaScriptCore and converter ready in %.0f ms",
        version, list->converter, currentVersion && *currentVersion ? currentVersion : "none", currentConverter,
        newer ? "" : force ? ": not newer, converting anyway (asked from Settings)" : ": up to date", manifestMS, msSince(jsStart));
    if (!newer && !force) {
        *message = g_strdup_printf("The list is up to date (uBO Lite %s).", currentVersion && *currentVersion ? currentVersion : version);
        result = "uptodate";
        goto done;
    }

    status("Downloading uBO Lite %s…", version);
    gint64 downloadStart = g_get_monotonic_time();
    JSCValue* inputs = jsc_value_new_object(js, NULL, NULL);
    for (guint i = 0; i < ids->len && !*message; i++) {
        const char* id = g_ptr_array_index(ids, i);
        for (int regex = 0; regex < 2 && !*message; regex++) {
            char* url = g_strdup_printf(AB_REPO "/%s/chromium/rulesets/%s/%s.json", version, regex ? "regex" : "main", id);
            gboolean missing = FALSE;
            GBytes* body = fetch(url, regex, &missing, &failure);
            if (body) {
                char* key = g_strdup_printf("%s%s", id, regex ? ".regex" : "");
                JSCValue* text = jsc_value_new_string_from_bytes(js, body);
                jsc_value_object_set_property(inputs, key, text);
                g_object_unref(text);
                fetchedBytes += g_bytes_get_size(body);
                g_string_append_printf(fetchedList, "%s%s", fetchedList->len ? ", " : "", key);
                g_free(key);
                g_bytes_unref(body);
            } else if (!missing)
                *message = g_strdup_printf("Could not download the list: %s", failure ? failure : url);
            g_free(url);
        }
    }
    if (*message) {
        g_object_unref(inputs);
        goto done;
    }
    JOBLOG("downloaded uBO Lite %s: %s (%.1f MB) in %.0f ms", version, fetchedList->str, fetchedBytes / 1048576.0, msSince(downloadStart));
    memLine("downloaded");

    status("Converting uBO Lite %s…", version);
    gint64 convertStart = g_get_monotonic_time();
    JSCValue* output = jsc_value_object_invoke_method(converter, "convertJSON", JSC_TYPE_VALUE, inputs, G_TYPE_NONE);
    g_object_unref(inputs);
    jsFailure = jscError(js);
    if (!jsFailure && output && jsc_value_is_string(output))
        converted = jsc_value_to_string(output);
    g_clear_object(&output);
    double convertMS = msSince(convertStart);
    memLine("converted");
    g_clear_object(&converter);
    g_clear_object(&js);
    g_clear_object(&vm);
    if (!converted) {
        *message = g_strdup_printf("The conversion failed: %s", jsFailure ? jsFailure : "no result");
        g_free(jsFailure);
        goto done;
    }
    char* newline = strchr(converted, '\n');
    long rules = newline ? jsonNumber(converted, "output") : -1;
    if (!newline || rules <= 0 || newline[1] != '[') {
        *message = g_strdup("The conversion gave no rules.");
        goto done;
    }
    *newline = 0;
    const char* rulesText = newline + 1;
    size_t rulesLength = strlen(rulesText);
    JOBLOG("converted uBO Lite %s in %.0f ms: %s; %.1f MB of WebKit JSON", version, convertMS, converted, rulesLength / 1048576.0);
    list->version = g_strdup(version);
    list->rules = rules;
    list->dnrRules = jsonNumber(converted, "input");
    list->skipped = jsonNumber(converted, "skipped");
    list->generated = g_get_real_time() / G_USEC_PER_SEC;
    list->sha256 = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar*)rulesText, rulesLength);
    GString* joined = g_string_new(NULL);
    for (guint i = 0; i < ids->len; i++)
        g_string_append_printf(joined, "%s%s", i ? ";" : "", (char*)g_ptr_array_index(ids, i));
    list->rulesets = g_string_free(joined, FALSE);
    if (!writeAtomically(rulesPath, rulesText, rulesLength)) {
        *message = g_strdup_printf("The new list could not be written to %s (%s).", rulesPath, g_strerror(errno));
        goto done;
    }
    g_clear_pointer(&converted, g_free);

    status("Compiling uBO Lite %s (%ld rules)…", version, rules);
    char* why = compileList(store, storeDir, identifier, rulesPath, compileMS);
    if (why) {
        *message = g_strdup_printf("The list %s could not be compiled (%s); the list in use stays.", version, why);
        g_free(why);
        goto done;
    }

    /* Saved only once compiled: the rules first, the info last (an info file
     * always describes the rules next to it; the old info goes first, so a
     * half pair is never chosen). */
    char* destRules = g_build_filename(dest, "adblock-rules.json", NULL);
    char* destInfo = g_build_filename(dest, "adblock-info.ini", NULL);
    unlink(destInfo);
    GKeyFile* file = g_key_file_new();
    infoToKeyFile(file, list);
    gboolean ok = !rename(rulesPath, destRules) && writeKeyFile(file, destInfo);
    g_key_file_unref(file);
    if (ok) {
        JOBLOG("saved uBO Lite %s (%ld rules) as %s", version, rules, destRules);
        result = "ok";
    } else
        *message = g_strdup_printf("The new list could not be saved in %s (%s).", dest, g_strerror(errno));
    g_free(destRules);
    g_free(destInfo);

done:
    if (manifest)
        g_bytes_unref(manifest);
    g_clear_object(&converter);
    g_clear_object(&js);
    g_clear_object(&vm);
    unlink(rulesPath);
    g_free(rulesPath);
    g_free(script);
    g_free(converted);
    g_free(version);
    g_ptr_array_unref(ids);
    g_string_free(fetchedList, TRUE);
    g_free(failure);
    g_free(converterPath);
    g_free(dest);
    g_free(currentVersion);
    return result;
}

int ps5wk_adblock_job(const char* jobPath)
{
    jobStart = g_get_monotonic_time();
    size_t flexible = 0;
    if (sceKernelAvailableFlexibleMemorySize)
        sceKernelAvailableFlexibleMemorySize(&flexible);
    JOBLOG("pid %d, job %s, flexible memory %zu MB", (int)getpid(), jobPath, flexible >> 20);
    pthread_t thread;
    if (!pthread_create(&thread, NULL, stdinWatcher, NULL))
        pthread_detach(thread);
    if (!pthread_create(&thread, NULL, timeLimit, NULL))
        pthread_detach(thread);
    /* Behind the pages: the compile is one busy core for up to a minute. */
    if (setpriority(PRIO_PROCESS, 0, 10))
        JOBLOG("setpriority: errno %d", errno);

    GKeyFile* job = g_key_file_new();
    GError* error = NULL;
    if (!g_key_file_load_from_file(job, jobPath, G_KEY_FILE_NONE, &error)) {
        JOBLOG("cannot read the job %s: %s", jobPath, error ? error->message : "?");
        return 2;
    }
    char* mode = g_key_file_get_string(job, "job", "mode", NULL);
    char* storeDir = g_key_file_get_string(job, "job", "store", NULL);
    char* identifier = g_key_file_get_string(job, "job", "identifier", NULL);
    char* work = g_key_file_get_string(job, "job", "work", NULL);
    char* resultPath = g_key_file_get_string(job, "job", "result", NULL);
    if (!mode || !storeDir || !identifier || !work || !resultPath) {
        JOBLOG("incomplete job %s", jobPath);
        return 2;
    }

    /* WebKit's compiler writes its NFAs to TMPDIR (SerializedNFA-*), and
     * ContentRuleList-XXXXXX next to the result until it is complete: a
     * killed job leaves them behind, so clear both. */
    char* tmp = g_build_filename(work, "tmp", NULL);
    g_mkdir_with_parents(tmp, 0777);
    g_setenv("TMPDIR", tmp, TRUE);
    unsigned staleTmp = removeFiles(tmp, NULL, NULL, NULL);
    unsigned staleStore = removeFiles(storeDir, "ContentRuleList-", "ContentRuleList-ubol-a", "ContentRuleList-ubol-b");
    if (staleTmp || staleStore)
        JOBLOG("removed %u stale temporary files from %s and %u from %s", staleTmp, tmp, staleStore, storeDir);

    WebKitUserContentFilterStore* store = webkit_user_content_filter_store_new(storeDir);
    GKeyFile* result = g_key_file_new();
    char* message = NULL;
    const char* outcome = NULL;
    double compileMS = 0;
    NewList list = { 0 };
    if (!strcmp(mode, "compile")) {
        char* source = g_key_file_get_string(job, "job", "source", NULL);
        char* version = g_key_file_get_string(job, "job", "version", NULL);
        status("Compiling the list %s…", version ? version : "");
        char* why = source ? compileList(store, storeDir, identifier, source, &compileMS) : g_strdup("no source");
        if (why)
            message = g_strdup_printf("The list %s could not be compiled (%s); the list in use stays.", version ? version : "", why);
        else
            outcome = "ok";
        g_free(why);
        g_free(source);
        g_free(version);
    } else if (!strcmp(mode, "update")) {
        outcome = runUpdate(job, store, storeDir, identifier, work, &message, &list, &compileMS);
        if (list.version && outcome && !strcmp(outcome, "ok"))
            infoToKeyFile(result, &list);
    } else
        message = g_strdup_printf("unknown mode %s", mode);
    g_object_unref(store);
    removeFiles(tmp, NULL, NULL, NULL);
    g_rmdir(tmp);
    /* A failed compile leaves its ContentRuleList-XXXXXX behind. */
    if (!outcome)
        removeFiles(storeDir, "ContentRuleList-", "ContentRuleList-ubol-a", "ContentRuleList-ubol-b");

    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    double totalMS = msSince(jobStart);
    g_key_file_set_string(result, "result", "status", outcome ? outcome : "failed");
    g_key_file_set_string(result, "result", "message", message ? message : "");
    g_key_file_set_string(result, "result", "identifier", identifier);
    g_key_file_set_int64(result, "result", "compile-ms", (gint64)compileMS);
    g_key_file_set_int64(result, "result", "total-ms", (gint64)totalMS);
    g_key_file_set_int64(result, "result", "peak-rss-mb", usage.ru_maxrss / 1024);
    gboolean written = writeKeyFile(result, resultPath);
    JOBLOG("%s in %.1f s (compile %.1f s), peak rss %.1f MB%s%s%s", outcome ? outcome : "FAILED", totalMS / 1000, compileMS / 1000,
        usage.ru_maxrss / 1024.0, message ? ": " : "", message ? message : "", written ? "" : "; RESULT NOT WRITTEN");
    return written && outcome ? 0 : 1;
}
