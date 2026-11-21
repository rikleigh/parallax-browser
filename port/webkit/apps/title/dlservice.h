/*
 * The download service's client (ps5browser): starts apps/child/dl-job.c
 * as a task payload when something needs it, connects to it over
 * 127.0.0.1 with the token from its job file, and carries lines both ways
 * (dl-job.c's header lists them). See port/webkit/README.md ("Downloads").
 */
#pragma once

#include <glib.h>

/* What the service said (main loop): verb ("fetch", "fetchend", "job",
 * "jobend"; "gone" when the service ended), its parameters. */
typedef void (*PS5WKDlServiceEvent)(const char* verb, GHashTable* params, gpointer user);

/* Main loop, once at start. */
void ps5wk_dlservice_init(PS5WKDlServiceEvent handler, gpointer user);

/* Any thread: a line to the service, started if it isn't running (lines
 * wait until it is connected). Pairs of key, value (value NULL: left out),
 * then NULL. */
void ps5wk_dlservice_send(const char* verb, ...) G_GNUC_NULL_TERMINATED;

/* A route thread (never the main loop): a request ("req"), the service
 * started if needed; waits up to timeoutMS. The reply's JSON (g_free), or
 * NULL with *error (g_free) saying why. Pairs as above, then NULL. */
char* ps5wk_dlservice_request(guint timeoutMS, char** error, const char* op, ...) G_GNUC_NULL_TERMINATED;

/* The main loop: the same without waiting; done runs on the main loop
 * with the reply's JSON or why not (both freed after it returns). */
typedef void (*PS5WKDlServiceReply)(const char* json, const char* error, gpointer user);
void ps5wk_dlservice_request_async(guint timeoutMS, PS5WKDlServiceReply done, gpointer user, const char* op, ...) G_GNUC_NULL_TERMINATED;

/* Whether the service is connected now (any thread). */
gboolean ps5wk_dlservice_connected(void);

/* The wind-down: no new start. */
void ps5wk_dlservice_wind_down(void);
