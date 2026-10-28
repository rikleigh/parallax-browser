/* Console smoke test for the PS5 WPE dependency sysroot.
 *
 * A payload (elfldr) that links glib/gio, libsoup, glib-networking (OpenSSL,
 * static GIO module), fontconfig, freetype and harfbuzz statically and checks:
 *   mainloop  GMainLoop with a timeout plus a worker thread
 *   tcp       GIO TCP connection (GSocketClient -> example.com:80)
 *   https-*   libsoup HTTPS GETs with certificate validation on
 *   badcert   an expired certificate is rejected (validation really is on)
 *   http2     HTTP/2 negotiated (ALPN) for at least one of the HTTPS hosts
 *   fontconfig/harfbuzz  "sans-serif" from the shipped config, then shaping
 *   xkb       WPEPlatform's default keymap (evdev/pc105/us) from the shipped XKB data
 *
 * Every check prints one "RESULT <name> PASS|FAIL <detail>" line.
 * Needs the runtime tree (port/deps/smoke/upload-runtime.sh) in /data/ps5webkit.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <gio/gio.h>
#include <libsoup/soup.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>
#include <openssl/opensslv.h>
#include <xkbcommon/xkbcommon.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509_vfy.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>

/* From glib-networking's static OpenSSL module (libgioopenssl.a). */
extern void g_io_openssl_load(GIOModule *module);

static int n_pass, n_fail;

static void result(const char *name, gboolean ok, const char *fmt, ...)
{
    char detail[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    printf("RESULT %s %s %s\n", name, ok ? "PASS" : "FAIL", detail);
    fflush(stdout);
    if (ok)
        n_pass++;
    else
        n_fail++;
}

/* ------------------------------------------------------------- mainloop */
typedef struct {
    GMainLoop *loop;
    GThread *main_thread;
    gboolean worker_called_back;
    gboolean callback_on_main;
    gboolean timeout_fired;
    gint64 started;
    gint64 timeout_at;
} LoopTest;

static gboolean on_worker_done(gpointer data)
{
    LoopTest *t = data;
    t->worker_called_back = TRUE;
    t->callback_on_main = g_thread_self() == t->main_thread;
    return G_SOURCE_REMOVE;
}

static gpointer worker(gpointer data)
{
    LoopTest *t = data;
    volatile guint64 acc = 0;
    for (guint64 i = 0; i < 5000000; i++)
        acc += i * i;
    g_usleep(50 * 1000);
    g_main_context_invoke(NULL, on_worker_done, t);
    return GUINT_TO_POINTER((guint)(acc & 1) + 1);
}

static gboolean on_timeout(gpointer data)
{
    LoopTest *t = data;
    t->timeout_fired = TRUE;
    t->timeout_at = g_get_monotonic_time();
    g_main_loop_quit(t->loop);
    return G_SOURCE_REMOVE;
}

static void test_mainloop(void)
{
    LoopTest t = { 0 };
    t.loop = g_main_loop_new(NULL, FALSE);
    t.main_thread = g_thread_self();
    t.started = g_get_monotonic_time();
    g_timeout_add(300, on_timeout, &t);
    GThread *th = g_thread_new("smoke-worker", worker, &t);
    g_main_loop_run(t.loop);
    gpointer ret = g_thread_join(th);
    double ms = (t.timeout_at - t.started) / 1000.0;
    result("mainloop", t.timeout_fired && t.worker_called_back && t.callback_on_main && ret != NULL,
           "timeout=%d after %.1f ms (want ~300), worker callback=%d on-main=%d",
           t.timeout_fired, ms, t.worker_called_back, t.callback_on_main);
    g_main_loop_unref(t.loop);
}

/* ------------------------------------------------------------------ tcp */
static void test_tcp(void)
{
    GError *err = NULL;
    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_timeout(client, 20);
    gint64 t0 = g_get_monotonic_time();
    GSocketConnection *conn = g_socket_client_connect_to_host(client, "example.com", 80, NULL, &err);
    if (!conn) {
        result("tcp", FALSE, "connect example.com:80: %s", err ? err->message : "?");
        g_clear_error(&err);
        g_object_unref(client);
        return;
    }
    double connect_ms = (g_get_monotonic_time() - t0) / 1000.0;
    GSocketAddress *ra = g_socket_connection_get_remote_address(conn, NULL);
    char *addr = ra ? g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(ra))) : g_strdup("?");
    const char *req = "HEAD / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    char buf[256] = { 0 };
    gsize n = 0;
    gboolean ok = g_output_stream_write_all(out, req, strlen(req), NULL, NULL, &err) &&
                  g_input_stream_read_all(in, buf, sizeof buf - 1, &n, NULL, &err);
    char *eol = strpbrk(buf, "\r\n");
    if (eol)
        *eol = 0;
    ok = ok && g_str_has_prefix(buf, "HTTP/1.");
    result("tcp", ok, "example.com -> %s, connect %.0f ms, reply \"%s\"%s%s", addr, connect_ms, buf,
           err ? ", error: " : "", err ? err->message : "");
    g_clear_error(&err);
    g_free(addr);
    g_clear_object(&ra);
    g_object_unref(conn);
    g_object_unref(client);
}

/* ---------------------------------------------------------------- https */
static const char *http_version_name(SoupHTTPVersion v)
{
    switch (v) {
    case SOUP_HTTP_1_0: return "HTTP/1.0";
    case SOUP_HTTP_1_1: return "HTTP/1.1";
    case SOUP_HTTP_2_0: return "HTTP/2";
    default: return "?";
    }
}

static gboolean https_get(SoupSession *s, const char *name, const char *url, gboolean *h2)
{
    GError *err = NULL;
    SoupMessage *m = soup_message_new("GET", url);
    gint64 t0 = g_get_monotonic_time();
    GBytes *body = soup_session_send_and_read(s, m, NULL, &err);
    double ms = (g_get_monotonic_time() - t0) / 1000.0;
    guint status = soup_message_get_status(m);
    SoupHTTPVersion ver = soup_message_get_http_version(m);
    GTlsCertificateFlags tlserr = soup_message_get_tls_peer_certificate_errors(m);
    GTlsCertificate *cert = soup_message_get_tls_peer_certificate(m);
    char *subject = cert ? g_tls_certificate_get_subject_name(cert) : NULL;
    char *issuer = cert ? g_tls_certificate_get_issuer_name(cert) : NULL;
    GUri *final = soup_message_get_uri(m);
    char *final_s = final ? g_uri_to_string(final) : g_strdup("?");
    gboolean ok = body && !err && status >= 200 && status < 400 && tlserr == 0 && cert;
    result(name, ok, "%s -> %u %s, %s, %zu bytes, %.0f ms, tls-errors=0x%x, cert \"%s\" by \"%s\"%s%s",
           url, status, final_s, http_version_name(ver), body ? g_bytes_get_size(body) : 0, ms, tlserr,
           subject ? subject : "-", issuer ? issuer : "-", err ? ", error: " : "", err ? err->message : "");
    if (ok && ver == SOUP_HTTP_2_0)
        *h2 = TRUE;
    g_free(subject);
    g_free(issuer);
    g_free(final_s);
    g_clear_error(&err);
    if (body)
        g_bytes_unref(body);
    g_object_unref(m);
    return ok;
}

static void test_badcert(SoupSession *s)
{
    GError *err = NULL;
    SoupMessage *m = soup_message_new("GET", "https://expired.badssl.com/");
    GBytes *body = soup_session_send_and_read(s, m, NULL, &err);
    GTlsCertificateFlags tlserr = soup_message_get_tls_peer_certificate_errors(m);
    gboolean rejected = !body && err && g_error_matches(err, G_TLS_ERROR, G_TLS_ERROR_BAD_CERTIFICATE);
    result("badcert", rejected, "https://expired.badssl.com/ %s (tls-errors=0x%x%s%s)",
           rejected ? "rejected" : "NOT rejected", tlserr, err ? ", " : "", err ? err->message : "");
    g_clear_error(&err);
    if (body)
        g_bytes_unref(body);
    g_object_unref(m);
}

static void test_https(void)
{
    GTlsBackend *be = g_tls_backend_get_default();
    GTlsDatabase *db = g_tls_backend_get_default_database(be);
    result("tls-backend", g_tls_backend_supports_tls(be) && db != NULL, "%s, default database %s (%s, SSL_CERT_FILE=%s)",
           G_OBJECT_TYPE_NAME(be), db ? G_OBJECT_TYPE_NAME(db) : "none", OPENSSL_VERSION_TEXT,
           g_getenv("SSL_CERT_FILE") ? g_getenv("SSL_CERT_FILE") : "unset");
    g_clear_object(&db);

    SoupSession *s = soup_session_new_with_options("timeout", 30, "user-agent",
        "Mozilla/5.0 (PlayStation 5) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15 ps5webkit-smoke", NULL);
    gboolean h2 = FALSE;
    https_get(s, "https-example", "https://example.com/", &h2);
    https_get(s, "https-youtube", "https://www.youtube.com/", &h2);
    result("http2", h2, "HTTP/2 negotiated via ALPN on at least one host: %s", h2 ? "yes" : "no");
    test_badcert(s);
    g_object_unref(s);
}

/* ----------------------------------------------------------- fontconfig */
static char *match_font(const char *pattern, char **family)
{
    FcPattern *p = FcNameParse((const FcChar8 *)pattern);
    FcConfigSubstitute(NULL, p, FcMatchPattern);
    FcDefaultSubstitute(p);
    FcResult r;
    FcPattern *m = FcFontMatch(NULL, p, &r);
    char *file = NULL;
    FcChar8 *s;
    if (m && FcPatternGetString(m, FC_FILE, 0, &s) == FcResultMatch)
        file = g_strdup((const char *)s);
    if (family)
        *family = (m && FcPatternGetString(m, FC_FAMILY, 0, &s) == FcResultMatch) ? g_strdup((const char *)s) : g_strdup("?");
    if (m)
        FcPatternDestroy(m);
    FcPatternDestroy(p);
    return file;
}

static void test_fonts(void)
{
    gint64 t0 = g_get_monotonic_time();
    FcBool inited = FcInit();
    double ms = (g_get_monotonic_time() - t0) / 1000.0;
    FcFontSet *all = inited ? FcConfigGetFonts(NULL, FcSetSystem) : NULL;
    int nfonts = all ? all->nfont : 0;
    char *conf = (char *)FcConfigFilename(NULL);
    result("fontconfig-init", inited && nfonts > 0, "FcInit %.0f ms, %d faces, config %s", ms, nfonts,
           conf ? conf : "(none)");
    FcStrFree((FcChar8 *)conf);

    char *fam = NULL;
    char *file = match_font("sans-serif", &fam);
    result("fontconfig-sans", file && strstr(file, "/NotoSans-"), "sans-serif -> %s (%s)", file ? file : "none", fam);
    g_free(fam);
    char *mono = match_font("monospace", &fam);
    result("fontconfig-mono", mono && strstr(mono, "NotoSansMono-"), "monospace -> %s (%s)", mono ? mono : "none", fam);
    g_free(fam);
    g_free(mono);
    char *cjk = match_font("sans-serif:lang=ja", &fam);
    result("fontconfig-cjk", cjk && strstr(cjk, "CJK"), "sans-serif:lang=ja -> %s (%s)", cjk ? cjk : "none", fam);
    g_free(fam);
    g_free(cjk);

    /* FreeType + HarfBuzz on the matched face. */
    FT_Library ft;
    FT_Face face;
    gboolean ok = FALSE;
    unsigned n = 0, nonzero = 0;
    hb_position_t advance = 0;
    if (file && FT_Init_FreeType(&ft) == 0) {
        if (FT_New_Face(ft, file, 0, &face) == 0) {
            FT_Set_Char_Size(face, 0, 16 * 64, 72, 72);
            hb_font_t *font = hb_ft_font_create_referenced(face);
            hb_buffer_t *buf = hb_buffer_create();
            hb_buffer_add_utf8(buf, "Hello, PS5 \xc3\xa9\xef\xac\x81", -1, 0, -1); /* e-acute + "fi" ligature */
            hb_buffer_guess_segment_properties(buf);
            hb_shape(font, buf, NULL, 0);
            hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &n);
            hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, &n);
            for (unsigned i = 0; i < n; i++) {
                nonzero += info[i].codepoint != 0;
                advance += pos[i].x_advance;
            }
            ok = n > 0 && nonzero == n && advance > 0;
            hb_buffer_destroy(buf);
            hb_font_destroy(font);
        }
        FT_Done_FreeType(ft);
    }
    result("harfbuzz", ok, "shaped %u glyphs (%u non-.notdef), advance %d/64 px, harfbuzz %s", n, nonzero, advance,
           hb_version_string());
    g_free(file);
}


/* ----------------------------------------------------------- diagnostics */
static void test_libc_quirks(void)
{
    char buf[256];
    errno = 0;
    ssize_t n = readlink("/data/ps5webkit/etc/fonts/fonts.conf", buf, sizeof buf - 1);
    int e = errno;
    result("readlink-regular-file", n == -1, "readlink(regular file) = %zd errno=%d (POSIX: -1/EINVAL)", n, e);
}

static void test_openssl_raw(void)
{
    unsigned char rnd[32];
    gint64 t0 = g_get_monotonic_time();
    int r = RAND_bytes(rnd, sizeof rnd);
    result("openssl-rand", r == 1, "RAND_bytes=%d in %.1f ms", r, (g_get_monotonic_time() - t0) / 1000.0);

    struct addrinfo hints = { 0 }, *ai = NULL;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_INET;
    if (getaddrinfo("example.com", "443", &hints, &ai) != 0 || !ai) {
        result("openssl-raw", FALSE, "getaddrinfo failed");
        return;
    }
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    struct timeval tv = { 15, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
        result("openssl-raw", FALSE, "connect: errno %d", errno);
        close(fd);
        freeaddrinfo(ai);
        return;
    }
    freeaddrinfo(ai);
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    int lv = SSL_CTX_load_verify_locations(ctx, "/data/ps5webkit/etc/ssl/cert.pem", NULL);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    static const unsigned char alpn[] = "\x02h2\x08http/1.1";
    SSL_CTX_set_alpn_protos(ctx, alpn, sizeof alpn - 1);
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, "example.com");
    SSL_set1_host(ssl, "example.com");
    t0 = g_get_monotonic_time();
    int c = SSL_connect(ssl);
    double ms = (g_get_monotonic_time() - t0) / 1000.0;
    const unsigned char *proto = NULL;
    unsigned plen = 0;
    SSL_get0_alpn_selected(ssl, &proto, &plen);
    char ebuf[256] = "";
    unsigned long ec = ERR_peek_last_error();
    if (ec)
        ERR_error_string_n(ec, ebuf, sizeof ebuf);
    result("openssl-raw", c == 1 && SSL_get_verify_result(ssl) == X509_V_OK,
           "SSL_connect=%d (ssl_err=%d errno=%d) %.0f ms, %s, verify=%ld, alpn=%.*s, load_verify=%d %s", c,
           SSL_get_error(ssl, c), errno, ms, SSL_get_version(ssl), SSL_get_verify_result(ssl), (int)plen,
           proto ? (const char *)proto : "", lv, ebuf);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);
}

static void test_gio_tls(void)
{
    GError *err = NULL;
    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_timeout(client, 15);
    g_socket_client_set_tls(client, TRUE);
    gint64 t0 = g_get_monotonic_time();
    GSocketConnection *conn = g_socket_client_connect_to_host(client, "example.com", 443, NULL, &err);
    double ms = (g_get_monotonic_time() - t0) / 1000.0;
    /* GSocketClient performs the handshake while connecting and returns a
     * GTcpWrapperConnection around the GTlsClientConnection. */
    GIOStream *base = conn && G_IS_TCP_WRAPPER_CONNECTION(conn)
        ? g_tcp_wrapper_connection_get_base_io_stream(G_TCP_WRAPPER_CONNECTION(conn)) : NULL;
    gboolean ok = base && G_IS_TLS_CONNECTION(base);
    char *proto = NULL;
    if (ok)
        proto = g_strdup(g_tls_connection_get_negotiated_protocol(G_TLS_CONNECTION(base)));
    result("gio-tls", ok, "GSocketClient tls=TRUE example.com:443 %.0f ms, %s%s%s", ms,
           base ? G_OBJECT_TYPE_NAME(base) : "no TLS stream", err ? ", error: " : "", err ? err->message : "");
    g_free(proto);
    g_clear_error(&err);
    g_clear_object(&conn);
    g_object_unref(client);
}

static void test_soup_http(void)
{
    GError *err = NULL;
    SoupSession *s = soup_session_new_with_options("timeout", 15, NULL);
    SoupMessage *m = soup_message_new("GET", "http://example.com/");
    gint64 t0 = g_get_monotonic_time();
    GBytes *body = soup_session_send_and_read(s, m, NULL, &err);
    result("soup-http", body && soup_message_get_status(m) == 200, "http://example.com/ -> %u, %zu bytes, %.0f ms%s%s",
           soup_message_get_status(m), body ? g_bytes_get_size(body) : 0, (g_get_monotonic_time() - t0) / 1000.0,
           err ? ", error: " : "", err ? err->message : "");
    g_clear_error(&err);
    if (body)
        g_bytes_unref(body);
    g_object_unref(m);
    g_object_unref(s);
}

#ifdef SMOKE_TLS_DEBUG
static gboolean dbg_ready(GObject *stream, gpointer data) { *(gboolean *)data = TRUE; return G_SOURCE_REMOVE; }
static int dbg_polls;
static gint dbg_poll(GPollFD *fds, guint nfds, gint timeout)
{
    gint64 t0 = g_get_monotonic_time();
    gint r = poll((struct pollfd *)fds, nfds, timeout);
    if (dbg_polls++ < 6) {
        printf("  poll(nfds=%u, timeout=%d) = %d in %.0f ms:", nfds, timeout, r, (g_get_monotonic_time() - t0) / 1000.0);
        for (guint i = 0; i < nfds; i++) printf(" [fd=%d ev=0x%x rev=0x%x]", fds[i].fd, fds[i].events, fds[i].revents);
        printf("\n");
    }
    return r;
}
static gpointer dbg_thread(gpointer data)
{
    GSocketConnection *conn = data;
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    GMainContext *ctx = g_main_context_new();
    g_main_context_set_poll_func(ctx, dbg_poll);
    g_main_context_push_thread_default(ctx);
    gboolean ready = FALSE;
    GSource *src = g_pollable_output_stream_create_source(G_POLLABLE_OUTPUT_STREAM(out), NULL);
    g_source_set_callback(src, (GSourceFunc)dbg_ready, &ready, NULL);
    g_source_attach(src, ctx);
    gint64 t0 = g_get_monotonic_time();
    while (!ready && g_get_monotonic_time() - t0 < 3000000) g_main_context_iteration(ctx, TRUE);
    printf("thread: out writable source fired=%d after %.0f ms\n", ready, (g_get_monotonic_time() - t0) / 1000.0);
    g_source_destroy(src); g_source_unref(src);
    const char *req = "HEAD / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    GError *err = NULL;
    gssize w = g_pollable_output_stream_write_nonblocking(G_POLLABLE_OUTPUT_STREAM(out), req, strlen(req), NULL, &err);
    printf("thread: write_nonblocking=%zd %s\n", w, err ? err->message : "");
    g_clear_error(&err);
    ready = FALSE;
    src = g_pollable_input_stream_create_source(G_POLLABLE_INPUT_STREAM(in), NULL);
    g_source_set_callback(src, (GSourceFunc)dbg_ready, &ready, NULL);
    g_source_attach(src, ctx);
    t0 = g_get_monotonic_time();
    while (!ready && g_get_monotonic_time() - t0 < 5000000) {
        gboolean r = g_main_context_iteration(ctx, TRUE);
        (void)r;
    }
    printf("thread: in readable source fired=%d after %.0f ms\n", ready, (g_get_monotonic_time() - t0) / 1000.0);
    char buf[64] = {0};
    gssize n = g_pollable_input_stream_read_nonblocking(G_POLLABLE_INPUT_STREAM(in), buf, sizeof buf - 1, NULL, &err);
    printf("thread: read_nonblocking=%zd \"%.20s\" %s\n", n, buf, err ? err->message : "");
    /* raw poll on the fd from this thread */
    int fd = g_socket_get_fd(g_socket_connection_get_socket(conn));
    struct pollfd pfd = { fd, POLLIN | POLLOUT, 0 };
    int pr = poll(&pfd, 1, 1000);
    printf("thread: poll(fd=%d)=%d revents=0x%x\n", fd, pr, pfd.revents);
    int pp[2]; pipe(pp);
    struct pollfd p2[2] = { { fd, POLLOUT, 0 }, { pp[0], POLLIN, 0 } };
    pr = poll(p2, 2, 1000);
    printf("thread: poll(sock POLLOUT + pipe POLLIN)=%d revents=0x%x,0x%x\n", pr, p2[0].revents, p2[1].revents);
    g_main_context_pop_thread_default(ctx);
    return NULL;
}
#endif

/* ------------------------------------------------------------------ xkb */
static void test_xkb(void)
{
    /* As WPEKeymapXKB does (wpe_keymap_xkb_new). */
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
    int inc = ctx ? xkb_context_include_path_append_default(ctx) : 0;
    struct xkb_rule_names names = { "evdev", "pc105", "us", "", "" };
    gint64 t0 = g_get_monotonic_time();
    struct xkb_keymap *km = ctx ? xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    double ms = (g_get_monotonic_time() - t0) / 1000.0;
    xkb_keysym_t sym = 0;
    if (km) {
        struct xkb_state *st = xkb_state_new(km);
        sym = xkb_state_key_get_one_sym(st, 38); /* evdev KEY_A (30) + 8 */
        xkb_state_unref(st);
    }
    char name[32] = "";
    xkb_keysym_get_name(sym, name, sizeof name);
    result("xkb", km && sym == XKB_KEY_a, "include-path=%d, keymap evdev/pc105/us %s in %.0f ms, keycode 38 -> %s", inc,
           km ? "compiled" : "FAILED", ms, name);
    if (km)
        xkb_keymap_unref(km);
    if (ctx)
        xkb_context_unref(ctx);
}

/* --------------------------------------------------------------- driver */
static gpointer watchdog(gpointer data)
{
    g_usleep(240 * G_USEC_PER_SEC);
    printf("RESULT watchdog FAIL smoke test still running after 240 s\n");
    fflush(stdout);
    _exit(2);
    return data;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("ps5webkit deps smoke test: glib %u.%u.%u, libsoup %u.%u.%u, %s, fontconfig %d.%d.%d\n",
           glib_major_version, glib_minor_version, glib_micro_version, soup_get_major_version(),
           soup_get_minor_version(), soup_get_micro_version(), OPENSSL_VERSION_TEXT, FC_MAJOR, FC_MINOR,
           FC_REVISION);
    g_thread_unref(g_thread_new("watchdog", watchdog, NULL));

    /* Static GIO module: register glib-networking's OpenSSL TLS backend. */
    g_io_openssl_load(NULL);

#ifdef SMOKE_TLS_DEBUG

    dup2(1, 2);
    g_setenv("G_MESSAGES_DEBUG", "all", TRUE);
    g_message("debug logging on");
    {
        GError *err = NULL;
        {
            char b[256];
            errno = 0; ssize_t n1 = readlink("/data/ps5webkit/nonexistent", b, sizeof b); int e1 = errno;
            unlink("/data/ps5webkit/cache/lnk");
            int sl = symlink("fonts.conf", "/data/ps5webkit/cache/lnk");
            errno = 0; ssize_t n2 = readlink("/data/ps5webkit/cache/lnk", b, sizeof b); int e2 = errno;
            printf("readlink(missing)=%zd errno=%d; symlink()=%d; readlink(link)=%zd errno=%d \"%.*s\"\n", n1, e1, sl, n2, e2, n2 > 0 ? (int)n2 : 0, b);
            errno = 0; ssize_t n3 = readlink("/data/ps5webkit/etc", b, sizeof b); int e3 = errno;
            printf("readlink(dir)=%zd errno=%d\n", n3, e3);
            int pp[2]; pipe(pp); write(pp[1], "x", 1);
            struct pollfd pf = { pp[0], POLLIN, 0 };
            struct timespec ts = { 1, 0 };
            errno = 0; int pr = ppoll(&pf, 1, &ts, NULL); int e4 = errno;
            printf("ppoll(readable pipe)=%d errno=%d revents=0x%x\n", pr, e4, pf.revents);
            pf.revents = 0; pr = poll(&pf, 1, 1000);
            printf("poll(readable pipe)=%d revents=0x%x\n", pr, pf.revents);
        }
        GSocketClient *client = g_socket_client_new();
        g_socket_client_set_timeout(client, 15);
        gint64 t0 = g_get_monotonic_time();
        GSocketConnection *conn = g_socket_client_connect_to_host(client, "example.com", 443, NULL, &err);
        printf("step1 tcp connect: %p %.0f ms %s\n", (void *)conn, (g_get_monotonic_time() - t0) / 1000.0, err ? err->message : "");
        GSocketConnectable *id = g_network_address_new("example.com", 443);
        t0 = g_get_monotonic_time();
        GIOStream *tls = g_tls_client_connection_new(G_IO_STREAM(conn), id, &err);
        printf("step2 tls_client_connection_new: %p %.0f ms %s\n", (void *)tls, (g_get_monotonic_time() - t0) / 1000.0, err ? err->message : "");
        {
            GSocketConnection *c2 = g_socket_client_connect_to_host(client, "example.com", 80, NULL, NULL);
            g_thread_join(g_thread_new("dbg", dbg_thread, c2));
        }
        t0 = g_get_monotonic_time();
        gboolean ok = tls && g_tls_connection_handshake(G_TLS_CONNECTION(tls), NULL, &err);
        printf("step3 handshake: %d %.0f ms %s\n", ok, (g_get_monotonic_time() - t0) / 1000.0, err ? err->message : "");
    }
    return 0;
#endif
    test_mainloop();
    test_libc_quirks();
    test_tcp();
    test_soup_http();
    test_openssl_raw();
    test_gio_tls();
    test_https();
    test_fonts();
    test_xkb();

    printf("RESULT summary %s %d passed, %d failed\n", n_fail ? "FAIL" : "PASS", n_pass, n_fail);
    fflush(stdout);
    return n_fail ? 1 : 0;
}
