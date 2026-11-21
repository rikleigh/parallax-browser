/*
 * The download service's client: see dlservice.h.
 *
 * Start: a random token goes into DL_WORK/job.ini (the job reads and
 * deletes it), the task starts (wpe_process_manager_ps5_run_task, "dl"),
 * and its "[dl-job] port: <n>" line (the relay thread) starts the reader
 * thread, which connects, says hello with the token, sends the lines that
 * waited, then reads the service's lines: replies ("rep") wake the route
 * thread waiting for them, everything else goes to the main loop's handler.
 * When the task ends (it crashed, or the title's connection to it closed)
 * the handler gets "gone"; the next line starts it again.
 */
#define SPIKE_NAME "ps5browser"
#include "common/spike.h"
#include "dlservice.h"
#include "browser.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wpe/ps5/wpe-ps5.h>

#define DL_WORK "/data/ps5webkit/dl"
#define CONNECT_TRIES 50

typedef enum { S_IDLE, S_STARTING, S_CONNECTED } ServiceState;

static GMutex lock;
static GCond changedCond;
static ServiceState state = S_IDLE;
static int fd = -1;
static char token[40];
static GPtrArray* waiting; /* char* lines (with their newline) sent once connected */
static guint generation;   /* a new one per start */
static gboolean windingDown;
static PS5WKDlServiceEvent handler;
static gpointer handlerUser;

/* requests waiting for their "rep" */
typedef struct {
    unsigned n;
    gboolean answered;
    char* json;
    char* error;
} Request;
static GPtrArray* requests; /* Request* (borrowed) */
static unsigned nextRequest = 1;

static void appendPairs(GString* line, va_list args)
{
    gboolean first = TRUE;
    for (const char* key; (key = va_arg(args, const char*));) {
        const char* value = va_arg(args, const char*);
        if (!value)
            continue;
        g_string_append(line, first ? " " : "&");
        first = FALSE;
        g_string_append(line, key);
        g_string_append_c(line, '=');
        g_string_append_uri_escaped(line, value, NULL, FALSE);
    }
    if (first)
        g_string_append_c(line, ' ');
    g_string_append_c(line, '\n');
}

static gboolean writeAll(int socket, const char* data, size_t size)
{
    while (size) {
        ssize_t n = send(socket, data, size, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return FALSE;
        data += n;
        size -= (size_t)n;
    }
    return TRUE;
}

/* ---- events to the main loop ---- */

typedef struct {
    char* verb;
    GHashTable* params;
} Event;

static gboolean eventOnMain(gpointer data)
{
    Event* event = data;
    if (handler)
        handler(event->verb, event->params, handlerUser);
    g_free(event->verb);
    g_hash_table_unref(event->params);
    g_free(event);
    return G_SOURCE_REMOVE;
}

static void postEvent(const char* verb, GHashTable* params)
{
    Event* event = g_new0(Event, 1);
    event->verb = g_strdup(verb);
    event->params = params ? g_hash_table_ref(params) : g_hash_table_new(g_str_hash, g_str_equal);
    g_idle_add(eventOnMain, event);
}

/* ---- the connection ---- */

static void dropConnection(guint startedAs, const char* why)
{
    g_mutex_lock(&lock);
    if (generation != startedAs) {
        g_mutex_unlock(&lock);
        return;
    }
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    gboolean was = state != S_IDLE;
    state = S_IDLE;
    for (unsigned i = 0; requests && i < requests->len; i++) {
        Request* r = g_ptr_array_index(requests, i);
        if (!r->answered) {
            r->answered = TRUE;
            r->error = g_strdup("The download service ended");
        }
    }
    g_cond_broadcast(&changedCond);
    g_mutex_unlock(&lock);
    if (was) {
        spike_log("[dl] the download service is gone: %s", why);
        postEvent("gone", NULL);
    }
}

/* "k=v&k2=v2", percent-encoded: a table of new strings. Unlike
 * g_uri_parse_params, a value that isn't valid UTF-8 once decoded is
 * repaired (U+FFFD) instead of failing the whole line. */
static GHashTable* parseParams(const char* text)
{
    GHashTable* table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    char** pairs = g_strsplit(text ? text : "", "&", -1);
    for (char** pair = pairs; *pair; pair++) {
        if (!**pair)
            continue;
        char* equals = strchr(*pair, '=');
        char* key = g_uri_unescape_segment(*pair, equals ? equals : NULL, NULL);
        char* value = equals ? g_uri_unescape_string(equals + 1, NULL) : g_strdup("");
        if (key && value) {
            if (!g_utf8_validate(value, -1, NULL)) {
                char* valid = g_utf8_make_valid(value, -1);
                g_free(value);
                value = valid;
            }
            g_hash_table_replace(table, key, value);
        } else {
            g_free(key);
            g_free(value);
        }
    }
    g_strfreev(pairs);
    return table;
}

static void handleLine(char* text)
{
    char* space = strchr(text, ' ');
    if (space)
        *space = 0;
    GHashTable* params = parseParams(space ? space + 1 : "");
    if (!strcmp(text, "rep")) {
        const char* n = g_hash_table_lookup(params, "n");
        unsigned number = n ? (unsigned)atoi(n) : 0;
        g_mutex_lock(&lock);
        for (unsigned i = 0; requests && i < requests->len; i++) {
            Request* r = g_ptr_array_index(requests, i);
            if (r->n != number || r->answered)
                continue;
            r->answered = TRUE;
            const char* ok = g_hash_table_lookup(params, "ok");
            if (ok && !strcmp(ok, "1"))
                r->json = g_strdup(g_hash_table_lookup(params, "json"));
            else {
                const char* error = g_hash_table_lookup(params, "error");
                r->error = g_strdup(error && *error ? error : "The download service said no");
            }
            g_cond_broadcast(&changedCond);
        }
        g_mutex_unlock(&lock);
    } else
        postEvent(text, params);
    g_hash_table_unref(params);
}

typedef struct {
    guint generation;
    unsigned port;
} Reader;

static gpointer readerThread(gpointer data)
{
    Reader* reader = data;
    int socketFD = -1;
    for (int i = 0; i < CONNECT_TRIES && socketFD < 0; i++) {
        socketFD = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in address = { 0 };
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons((uint16_t)reader->port);
        if (socketFD >= 0 && connect(socketFD, (struct sockaddr*)&address, sizeof address)) {
            close(socketFD);
            socketFD = -1;
            g_usleep(100 * 1000);
        }
    }
    if (socketFD < 0) {
        dropConnection(reader->generation, "couldn't connect");
        g_free(reader);
        return NULL;
    }
    int one = 1;
    setsockopt(socketFD, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    char hello[80];
    snprintf(hello, sizeof hello, "hello token=%s\n", token);
    g_mutex_lock(&lock);
    gboolean current = generation == reader->generation;
    gboolean ok = current && writeAll(socketFD, hello, strlen(hello));
    for (unsigned i = 0; ok && waiting && i < waiting->len; i++) {
        const char* line = g_ptr_array_index(waiting, i);
        ok = writeAll(socketFD, line, strlen(line));
    }
    if (ok) {
        if (waiting)
            g_ptr_array_set_size(waiting, 0);
        fd = socketFD;
        state = S_CONNECTED;
        g_cond_broadcast(&changedCond);
    }
    g_mutex_unlock(&lock);
    if (!ok) {
        close(socketFD);
        if (current)
            dropConnection(reader->generation, "the first lines couldn't be sent");
        g_free(reader);
        return NULL;
    }
    spike_log("[dl] the download service is connected (port %u)", reader->port);
    GString* pending = g_string_new(NULL);
    char buffer[16384];
    for (;;) {
        ssize_t n = recv(socketFD, buffer, sizeof buffer, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        g_string_append_len(pending, buffer, n);
        char* newline;
        while ((newline = memchr(pending->str, '\n', pending->len))) {
            *newline = 0;
            if (*pending->str)
                handleLine(pending->str);
            g_string_erase(pending, 0, newline - pending->str + 1);
        }
    }
    g_string_free(pending, TRUE);
    dropConnection(reader->generation, "its connection closed");
    g_free(reader);
    return NULL;
}

/* The relay thread: the task's lines (the relay logs them). */
static void taskOutput(const char* line, gpointer data)
{
    static const char prefix[] = "[dl-job] port: ";
    if (strncmp(line, prefix, sizeof prefix - 1))
        return;
    Reader* reader = g_new0(Reader, 1);
    reader->generation = GPOINTER_TO_UINT(data);
    reader->port = (unsigned)atoi(line + sizeof prefix - 1);
    GThread* thread = g_thread_try_new("ps5wk-dl-client", readerThread, reader, NULL);
    if (thread)
        g_thread_unref(thread);
    else {
        g_free(reader);
        dropConnection(GPOINTER_TO_UINT(data), "no thread");
    }
}

static void taskEnded(gboolean launched, gpointer data)
{
    dropConnection(GPOINTER_TO_UINT(data), launched ? "the task ended" : "the task didn't start");
}

/* With lock held: start the task if it isn't running. */
static void ensureLocked(void)
{
    if (state != S_IDLE || windingDown)
        return;
    WPEProcessManager* manager = wpe_process_manager_get_default();
    if (!manager)
        return;
    for (unsigned i = 0; i < 32; i += 8)
        snprintf(token + i, sizeof token - i, "%08x", g_random_int());
    g_mkdir_with_parents(DL_WORK, 0777);
    chmod(DL_WORK, 0777); /* the job runs under another uid */
    char* text = g_strdup_printf("[job]\ntoken=%s\n", token);
    gboolean written = g_file_set_contents(DL_WORK "/job.ini", text, -1, NULL);
    chmod(DL_WORK "/job.ini", 0666);
    g_free(text);
    if (!written) {
        spike_log("[dl] the download service's job file couldn't be written");
        return;
    }
    generation++;
    state = S_STARTING;
    if (!wpe_process_manager_ps5_run_task(manager, "dl", DL_WORK "/job.ini", taskOutput, taskEnded, GUINT_TO_POINTER(generation))) {
        state = S_IDLE;
        spike_log("[dl] the download service couldn't start");
        return;
    }
    spike_log("[dl] starting the download service");
}

void ps5wk_dlservice_send(const char* verb, ...)
{
    GString* line = g_string_new(verb);
    va_list args;
    va_start(args, verb);
    appendPairs(line, args);
    va_end(args);
    g_mutex_lock(&lock);
    ensureLocked();
    if (state == S_CONNECTED && fd >= 0) {
        if (!writeAll(fd, line->str, line->len))
            spike_log("[dl] a line to the download service was lost (%s)", verb);
    } else if (state == S_STARTING) {
        if (!waiting)
            waiting = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(waiting, g_strdup(line->str));
    } else
        spike_log("[dl] the download service isn't running: %s not sent", verb);
    g_mutex_unlock(&lock);
    g_string_free(line, TRUE);
}

/* pairs: key, value, ..., NULL (values never NULL here). */
static char* requestPairs(guint timeoutMS, char** error, const char* op, char** pairs)
{
    Request request = { 0 };
    GString* line = g_string_new("req");
    g_mutex_lock(&lock);
    request.n = nextRequest++;
    g_mutex_unlock(&lock);
    char number[16], *json = NULL;
    snprintf(number, sizeof number, "%u", request.n);
    g_string_append_printf(line, " n=%s&op=", number);
    g_string_append_uri_escaped(line, op, NULL, FALSE);
    for (char** p = pairs; p && p[0] && p[1]; p += 2) {
        g_string_append_printf(line, "&%s=", p[0]);
        g_string_append_uri_escaped(line, p[1], NULL, FALSE);
    }
    g_string_append_c(line, '\n');

    gint64 end = g_get_monotonic_time() + (gint64)timeoutMS * 1000;
    g_mutex_lock(&lock);
    if (!requests)
        requests = g_ptr_array_new();
    ensureLocked();
    /* wait for the connection (a start takes a second or two) */
    while (state == S_STARTING && g_cond_wait_until(&changedCond, &lock, end)) { }
    if (state != S_CONNECTED || fd < 0 || !writeAll(fd, line->str, line->len)) {
        g_mutex_unlock(&lock);
        g_string_free(line, TRUE);
        if (error)
            *error = g_strdup(windingDown ? "The browser is closing" : "The download service isn't running");
        return NULL;
    }
    g_ptr_array_add(requests, &request);
    while (!request.answered && g_cond_wait_until(&changedCond, &lock, end)) { }
    g_ptr_array_remove(requests, &request);
    g_mutex_unlock(&lock);
    g_string_free(line, TRUE);
    if (!request.answered) {
        if (error)
            *error = g_strdup("The download service didn't answer");
        return NULL;
    }
    json = request.json;
    if (error)
        *error = request.error;
    else
        g_free(request.error);
    return json;
}

/* The pairs after op (values NULL left out) as a NULL-terminated vector. */
static char** collectPairs(va_list args)
{
    GPtrArray* out = g_ptr_array_new();
    for (const char* key; (key = va_arg(args, const char*));) {
        const char* value = va_arg(args, const char*);
        if (!value)
            continue;
        g_ptr_array_add(out, g_strdup(key));
        g_ptr_array_add(out, g_strdup(value));
    }
    g_ptr_array_add(out, NULL);
    return (char**)g_ptr_array_free(out, FALSE);
}

char* ps5wk_dlservice_request(guint timeoutMS, char** error, const char* op, ...)
{
    va_list args;
    va_start(args, op);
    char** pairs = collectPairs(args);
    va_end(args);
    char* json = requestPairs(timeoutMS, error, op, pairs);
    g_strfreev(pairs);
    return json;
}

typedef struct {
    guint timeoutMS;
    char* op;
    char** pairs;
    char* json;
    char* error;
    PS5WKDlServiceReply done;
    gpointer user;
} AsyncRequest;

static gboolean asyncReplied(gpointer data)
{
    AsyncRequest* a = data;
    a->done(a->json, a->error, a->user);
    g_free(a->json);
    g_free(a->error);
    g_free(a->op);
    g_strfreev(a->pairs);
    g_free(a);
    return G_SOURCE_REMOVE;
}

static gpointer asyncThread(gpointer data)
{
    AsyncRequest* a = data;
    a->json = requestPairs(a->timeoutMS, &a->error, a->op, a->pairs);
    /* Always through the main loop: g_main_context_invoke would run it here,
     * on this thread, when no loop owns the context (as the title shuts down). */
    g_idle_add_full(G_PRIORITY_DEFAULT, asyncReplied, a, NULL);
    return NULL;
}

void ps5wk_dlservice_request_async(guint timeoutMS, PS5WKDlServiceReply done, gpointer user, const char* op, ...)
{
    AsyncRequest* a = g_new0(AsyncRequest, 1);
    a->timeoutMS = timeoutMS;
    a->op = g_strdup(op);
    va_list args;
    va_start(args, op);
    a->pairs = collectPairs(args);
    va_end(args);
    a->done = done;
    a->user = user;
    GError* error = NULL;
    GThread* thread = g_thread_try_new("dl-request", asyncThread, a, &error);
    if (!thread) {
        a->error = g_strdup("Couldn't start a thread");
        g_clear_error(&error);
        g_idle_add_full(G_PRIORITY_DEFAULT, asyncReplied, a, NULL); /* later, as a reply would be */
        return;
    }
    g_thread_unref(thread);
}

gboolean ps5wk_dlservice_connected(void)
{
    g_mutex_lock(&lock);
    gboolean connected = state == S_CONNECTED;
    g_mutex_unlock(&lock);
    return connected;
}

void ps5wk_dlservice_init(PS5WKDlServiceEvent eventHandler, gpointer user)
{
    handler = eventHandler;
    handlerUser = user;
}

void ps5wk_dlservice_wind_down(void)
{
    g_mutex_lock(&lock);
    windingDown = TRUE;
    g_mutex_unlock(&lock);
}
