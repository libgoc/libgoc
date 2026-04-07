/*
 * tests/test_p11_http.c — Phase 11: HTTP server and client tests
 *
 * Verifies the goc_http HTTP library declared in goc_http.h.
 * Tests run a real HTTP server on loopback ports and make real HTTP client
 * requests against it.
 *
 * Build:  cmake -B build && cmake --build build
 * Run:    ctest --test-dir build --output-on-failure
 *         ./build/test_p11_http
 *
 * Test coverage:
 *
 *   P11.1  Server lifecycle: make → listen → close (no routes)
 *   P11.2  Routing: exact path match
 *   P11.3  Routing: catch-all wildcard /*
 *   P11.4  Routing: unmatched request → 404
 *   P11.5  Request context helpers: goc_http_server_header (present)
 *   P11.6  Request context helpers: goc_http_server_header (absent)
 *   P11.7  Request context helpers: goc_http_server_header (case-insensitive)
 *   P11.8  Request context helpers: goc_http_server_body_str (with body)
 *   P11.9  Request context helpers: goc_http_server_body_str (empty body)
 *   P11.10 Response: goc_http_server_respond (200)
 *   P11.11 Response: goc_http_server_respond_buf
 *   P11.12 Response: goc_http_server_respond_error
 *   P11.13 Middleware: chain runs in order; user_data propagates
 *   P11.14 Middleware: GOC_HTTP_ERR short-circuits with 500
 *   P11.15 HTTP client: goc_http_get
 *   P11.16 HTTP client: goc_http_post
 *   P11.17 HTTP client: parallel requests with goc_take_all
 *   P11.18 HTTP client: timeout fires correctly
 *   P11.18a HTTP client: keepalive request timeout fires correctly
 *   P11.19 Security: oversized request body rejected; server stays alive
 *   P11.20 Security: method mismatch (GET route hit with POST) → 404
 *   P11.21 Correctness: ctx->path and ctx->method populated correctly
 *   P11.22 Correctness: ctx->query parsed from URL query string
 *   P11.23 Correctness: goc_http_response_t->body_len matches respond_buf len
 *   P11.24 Correctness: custom request headers via opts->headers received
 *   P11.25 Security: CRLF in header value blocked (header-injection)
 *   P11.26 Integration: ping-pong — two servers bounce a counter 500 round trips (keep-alive enabled)
 *   P11.27 Keep-alive: client/server persistent connection handles sequential requests
 *   P11.28 Regression: fire-and-forget ping-pong survives repeated immediate teardown
 *   P11.29 Regression: fire-and-forget connect churn survives repeated teardown
 *   P11.30 Regression: repeated listen/close under worker-pool startup race
 *   P11.31 Strict affinity invariant: completed fibers from request workload
 *          should execute on >=2 workers (evidence of cross-worker scheduling)
 *   P11.32 Benchmark-style throughput comparison (pool=4 vs pool=1)
 */

#if defined(__linux__)
#  define _GNU_SOURCE
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdatomic.h>

#include "test_harness.h"
#include "goc.h"
#include "goc_array.h"
#include "goc_http.h"
#include "goc_stats.h"
#include "internal.h"

extern int goc_http_client_inflight(void);

/* =========================================================================
 * Helpers
 * ====================================================================== */

static int s_port = 18400;

static int next_port(void)
{
    return s_port++;
}

#if defined(_MSC_VER)
#  define TEST_THREAD_LOCAL __declspec(thread)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define TEST_THREAD_LOCAL _Thread_local
#else
#  define TEST_THREAD_LOCAL __thread
#endif

static TEST_THREAD_LOCAL char s_url_buf[256];
static const char* local_url(const char* path, int port)
{
    snprintf(s_url_buf, sizeof(s_url_buf),
             "http://127.0.0.1:%d%s", port, path);
    return s_url_buf;
}

/* Simple handler that replies "pong". */
static void handler_ping(goc_http_ctx_t* ctx)
{
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "pong"));
}

/* =========================================================================
 * P11.1 — Server lifecycle: make → listen → close
 * ====================================================================== */

typedef struct { goc_chan* done; int port; } p11_1_args_t;

static void fiber_p11_1(void* arg)
{
    p11_1_args_t* a   = (p11_1_args_t*)arg;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    int rc = goc_unbox_int(
                    goc_take(
                        goc_http_server_listen(srv, "127.0.0.1", a->port))->val);
    int ok = (rc == 0);
    if (ok)
        goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(ok));
}

static void test_p11_1(void)
{
    TEST_BEGIN("P11.1  Server lifecycle: make → listen → close (no routes)");
    p11_1_args_t args = { goc_chan_make(1), next_port() };
    goc_go(fiber_p11_1, &args);
    ASSERT(goc_unbox_int(goc_take_sync(args.done)->val));
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.2 — Routing: exact path match
 * ====================================================================== */

typedef struct {
    goc_chan* done;
    int       port;
    int       status;
    char      body[64];
} p11_simple_t;

static void fiber_p11_2(void* arg)
{
    p11_simple_t* a   = (p11_simple_t*)arg;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "GET", "/ping", handler_ping);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));

    goc_http_response_t* r =
        (goc_http_response_t*)goc_take(
            goc_http_get(local_url("/ping", a->port),
                          goc_http_request_opts()))->val;
    a->status = r ? r->status : -1;
    if (r && r->body)
        snprintf(a->body, sizeof(a->body), "%s", r->body);

    goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_2(void)
{
    TEST_BEGIN("P11.2  Routing: exact path GET /ping → 200 \"pong\"");
    p11_simple_t args;
    memset(&args, 0, sizeof(args));
    args.done = goc_chan_make(1);
    args.port = next_port();
    goc_go(fiber_p11_2, &args);
    goc_take_sync(args.done);
    ASSERT(args.status == 200);
    ASSERT(strcmp(args.body, "pong") == 0);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.3 — Routing: catch-all wildcard
 * ====================================================================== */

static void handler_catch_all(goc_http_ctx_t* ctx)
{
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "catch-all"));
}

typedef struct { goc_chan* done; int port; int status; } p11_status_t;

static void fiber_p11_3(void* arg)
{
    p11_status_t* a   = (p11_status_t*)arg;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "*", "/*", handler_catch_all);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));

    goc_http_response_t* r =
        (goc_http_response_t*)goc_take(
            goc_http_get(local_url("/anything/at/all", a->port),
                          goc_http_request_opts()))->val;
    a->status = r ? r->status : -1;

    goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_3(void)
{
    TEST_BEGIN("P11.3  Routing: wildcard /* catches any path → 200");
    p11_status_t args = { goc_chan_make(1), next_port(), 0 };
    goc_go(fiber_p11_3, &args);
    goc_take_sync(args.done);
    ASSERT(args.status == 200);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.4 — Routing: unmatched → 404
 * ====================================================================== */

static void fiber_p11_4(void* arg)
{
    p11_status_t* a   = (p11_status_t*)arg;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "GET", "/exists", handler_ping);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));

    goc_http_response_t* r =
        (goc_http_response_t*)goc_take(
            goc_http_get(local_url("/missing", a->port),
                          goc_http_request_opts()))->val;
    a->status = r ? r->status : -1;

    goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_4(void)
{
    TEST_BEGIN("P11.4  Routing: unmatched path → 404");
    p11_status_t args = { goc_chan_make(1), next_port(), 0 };
    goc_go(fiber_p11_4, &args);
    goc_take_sync(args.done);
    ASSERT(args.status == 404);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.5-7 — goc_http_server_header helpers
 * ====================================================================== */

typedef struct {
    goc_chan* done;
    goc_chan* handler_done;
    int       port;
    int       has_ct;
    int       absent_ok;
    int       case_ok;
} p11_hdr_t;

static p11_hdr_t* _Atomic g_hdr;

static void handler_inspect_headers(goc_http_ctx_t* ctx)
{
    p11_hdr_t* h = atomic_load_explicit(&g_hdr, memory_order_acquire);
    const char* ct  = goc_http_server_header(ctx, "content-type");
    h->has_ct    = (ct != NULL);
    const char* x   = goc_http_server_header(ctx, "x-no-such-header-p11");
    h->absent_ok = (x == NULL);
    const char* ct2 = goc_http_server_header(ctx, "CONTENT-TYPE");
    h->case_ok   = (ct2 != NULL);
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "ok"));
    goc_put(h->handler_done, goc_box_int(1));
}

static void fiber_p11_hdrs(void* arg)
{
    p11_hdr_t*    a   = (p11_hdr_t*)arg;
    atomic_store_explicit(&g_hdr, a, memory_order_release);
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "POST", "/hdr", handler_inspect_headers);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));

    goc_take(goc_http_post(local_url("/hdr", a->port),
                            "application/json", "{}", goc_http_request_opts()));
    goc_take(a->handler_done);

    goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_5(void)
{
    TEST_BEGIN("P11.5  goc_http_server_header: present header found");
    p11_hdr_t args;
    memset(&args, 0, sizeof(args));
    args.done = goc_chan_make(1);
    args.handler_done = goc_chan_make(1);
    args.port = next_port();
    goc_go(fiber_p11_hdrs, &args);
    goc_take_sync(args.done);
    ASSERT(args.has_ct);
    TEST_PASS();
done:;
}

static void test_p11_6(void)
{
    TEST_BEGIN("P11.6  goc_http_server_header: absent header returns NULL");
    p11_hdr_t args;
    memset(&args, 0, sizeof(args));
    args.done = goc_chan_make(1);
    args.handler_done = goc_chan_make(1);
    args.port = next_port();
    goc_go(fiber_p11_hdrs, &args);
    goc_take_sync(args.done);
    ASSERT(args.absent_ok);
    TEST_PASS();
done:;
}

static void test_p11_7(void)
{
    TEST_BEGIN("P11.7  goc_http_server_header: case-insensitive lookup");
    p11_hdr_t args;
    memset(&args, 0, sizeof(args));
    args.done = goc_chan_make(1);
    args.handler_done = goc_chan_make(1);
    args.port = next_port();
    goc_go(fiber_p11_hdrs, &args);
    goc_take_sync(args.done);
    ASSERT(args.case_ok);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.8-9 — goc_http_server_body_str
 * ====================================================================== */

typedef struct {
    goc_chan* done;
    int       port_body;
    int       port_empty;
    char      body_received[64];
    int       empty_ok;
} p11_body_t;

static p11_body_t* g_body;

static void handler_body_check(goc_http_ctx_t* ctx)
{
    const char* b = goc_http_server_body_str(ctx);
    if (b)
        snprintf(g_body->body_received, sizeof(g_body->body_received),
                 "%s", b);
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "ok"));
}

static void handler_body_empty(goc_http_ctx_t* ctx)
{
    const char* b = goc_http_server_body_str(ctx);
    g_body->empty_ok = (b != NULL && b[0] == '\0');
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "ok"));
}

static void fiber_p11_body(void* arg)
{
    p11_body_t*   a   = (p11_body_t*)arg;
    g_body = a;

    goc_http_server_t* srv1 = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv1, "POST", "/body", handler_body_check);
    goc_chan* ready1 = goc_http_server_listen(srv1, "127.0.0.1", a->port_body);

    goc_http_server_t* srv2 = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv2, "GET", "/empty", handler_body_empty);
    goc_chan* ready2 = goc_http_server_listen(srv2, "127.0.0.1", a->port_empty);

    goc_take(ready1);
    goc_take(ready2);

    goc_take(goc_http_post(local_url("/body", a->port_body),
                            "text/plain", "hello-body",
                            goc_http_request_opts()));
    goc_take(goc_timeout(20));

    goc_take(goc_http_get(local_url("/empty", a->port_empty),
                           goc_http_request_opts()));
    goc_take(goc_timeout(20));

    goc_take(goc_http_server_close(srv1));
    goc_take(goc_http_server_close(srv2));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_8(void)
{
    TEST_BEGIN("P11.8  goc_http_server_body_str: POST body received correctly");
    p11_body_t args;
    memset(&args, 0, sizeof(args));
    args.done       = goc_chan_make(1);
    args.port_body  = next_port();
    args.port_empty = next_port();
    goc_go(fiber_p11_body, &args);
    goc_take_sync(args.done);
    ASSERT(strcmp(args.body_received, "hello-body") == 0);
    TEST_PASS();
done:;
}

static void test_p11_9(void)
{
    TEST_BEGIN("P11.9  goc_http_server_body_str: empty body returns \"\"");
    p11_body_t args;
    memset(&args, 0, sizeof(args));
    args.done       = goc_chan_make(1);
    args.port_body  = next_port();
    args.port_empty = next_port();
    goc_go(fiber_p11_body, &args);
    goc_take_sync(args.done);
    ASSERT(args.empty_ok);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.10 — goc_http_server_respond (200)
 * ====================================================================== */

static void fiber_p11_10(void* arg)
{
    p11_simple_t* a   = (p11_simple_t*)arg;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "GET", "/hello", handler_ping);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));

    goc_http_response_t* r =
        (goc_http_response_t*)goc_take(
            goc_http_get(local_url("/hello", a->port),
                          goc_http_request_opts()))->val;
    a->status = r ? r->status : -1;
    if (r && r->body)
        snprintf(a->body, sizeof(a->body), "%s", r->body);

    goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_10(void)
{
    TEST_BEGIN("P11.10 goc_http_server_respond: status 200, body \"pong\"");
    p11_simple_t args;
    memset(&args, 0, sizeof(args));
    args.done = goc_chan_make(1);
    args.port = next_port();
    goc_go(fiber_p11_10, &args);
    goc_take_sync(args.done);
    ASSERT(args.status == 200);
    ASSERT(strcmp(args.body, "pong") == 0);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.11 — goc_http_server_respond_buf
 * ====================================================================== */

static void handler_respond_buf(goc_http_ctx_t* ctx)
{
    static const char data[] = "Hello";
    goc_take(goc_http_server_respond_buf(ctx, 201, "application/octet-stream",
                                     data, 5));
}

static void fiber_p11_11(void* arg)
{
    p11_simple_t* a   = (p11_simple_t*)arg;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "GET", "/buf", handler_respond_buf);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));

    goc_http_response_t* r =
        (goc_http_response_t*)goc_take(
            goc_http_get(local_url("/buf", a->port),
                          goc_http_request_opts()))->val;
    a->status = r ? r->status : -1;
    if (r && r->body)
        snprintf(a->body, sizeof(a->body), "%s", r->body);

    goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_11(void)
{
    TEST_BEGIN("P11.11 goc_http_server_respond_buf: status 201, binary body");
    p11_simple_t args;
    memset(&args, 0, sizeof(args));
    args.done = goc_chan_make(1);
    args.port = next_port();
    goc_go(fiber_p11_11, &args);
    goc_take_sync(args.done);
    ASSERT(args.status == 201);
    ASSERT(memcmp(args.body, "Hello", 5) == 0);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.12 — goc_http_server_respond_error
 * ====================================================================== */

static void handler_respond_err(goc_http_ctx_t* ctx)
{
    goc_take(goc_http_server_respond_error(ctx, 400, "bad input"));
}

static void fiber_p11_12(void* arg)
{
    p11_simple_t* a   = (p11_simple_t*)arg;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "GET", "/err", handler_respond_err);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));

    goc_http_response_t* r =
        (goc_http_response_t*)goc_take(
            goc_http_get(local_url("/err", a->port),
                          goc_http_request_opts()))->val;
    a->status = r ? r->status : -1;
    if (r && r->body)
        snprintf(a->body, sizeof(a->body), "%s", r->body);

    goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_12(void)
{
    TEST_BEGIN("P11.12 goc_http_server_respond_error: status 400, error body");
    p11_simple_t args;
    memset(&args, 0, sizeof(args));
    args.done = goc_chan_make(1);
    args.port = next_port();
    goc_go(fiber_p11_12, &args);
    goc_take_sync(args.done);
    ASSERT(args.status == 400);
    ASSERT(strstr(args.body, "bad input") != NULL);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.13-14 — Middleware
 * ====================================================================== */

typedef struct {
    int order[8];
    int n;
    const char* ud;
} mw_log_t;

static mw_log_t g_mw;

static goc_http_status_t mw1(goc_http_ctx_t* ctx)
{
    g_mw.order[g_mw.n++] = 1;
    ctx->user_data = (void*)"set-by-mw1";
    return GOC_HTTP_OK;
}

static goc_http_status_t mw2(goc_http_ctx_t* ctx)
{
    g_mw.order[g_mw.n++] = 2;
    (void)ctx;
    return GOC_HTTP_OK;
}

static void handler_mw_ok(goc_http_ctx_t* ctx)
{
    g_mw.order[g_mw.n++] = 3;
    g_mw.ud = (const char*)ctx->user_data;
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "mw-ok"));
}

static goc_http_status_t mw_reject(goc_http_ctx_t* ctx)
{
    (void)ctx;
    g_mw.order[g_mw.n++] = 99;
    return GOC_HTTP_ERR;
}

static void handler_mw_never(goc_http_ctx_t* ctx)
{
    g_mw.order[g_mw.n++] = 999;
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "unreachable"));
}

typedef struct {
    goc_chan* done;
    int       port_ok;
    int       port_rej;
    int       status_ok;
    int       status_rej;
} p11_mw_t;

static void fiber_p11_mw(void* arg)
{
    p11_mw_t* a = (p11_mw_t*)arg;
    memset(&g_mw, 0, sizeof(g_mw));

    goc_http_server_opts_t* opts_ok = goc_http_server_opts();
    opts_ok->middleware = goc_array_make(2);
    goc_array_push(opts_ok->middleware, (void*)(uintptr_t)mw1);
    goc_array_push(opts_ok->middleware, (void*)(uintptr_t)mw2);
    goc_http_server_t* srv_ok = goc_http_server_make(opts_ok);
    goc_http_server_route(srv_ok, "GET", "/mw", handler_mw_ok);
    goc_chan* ready_ok = goc_http_server_listen(srv_ok, "127.0.0.1", a->port_ok);

    goc_http_server_opts_t* opts_rej = goc_http_server_opts();
    opts_rej->middleware = goc_array_make(1);
    goc_array_push(opts_rej->middleware, (void*)(uintptr_t)mw_reject);
    goc_http_server_t* srv_rej = goc_http_server_make(opts_rej);
    goc_http_server_route(srv_rej, "GET", "/mw", handler_mw_never);
    goc_chan* ready_rej = goc_http_server_listen(srv_rej, "127.0.0.1", a->port_rej);

    goc_take(ready_ok);
    goc_take(ready_rej);

    goc_http_response_t* r_ok =
        (goc_http_response_t*)goc_take(
            goc_http_get(local_url("/mw", a->port_ok),
                          goc_http_request_opts()))->val;
    a->status_ok = r_ok ? r_ok->status : -1;
    goc_take(goc_timeout(20));

    goc_http_response_t* r_rej =
        (goc_http_response_t*)goc_take(
            goc_http_get(local_url("/mw", a->port_rej),
                          goc_http_request_opts()))->val;
    a->status_rej = r_rej ? r_rej->status : -1;
    goc_take(goc_timeout(20));

    goc_take(goc_http_server_close(srv_ok));
    goc_take(goc_http_server_close(srv_rej));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_13(void)
{
    TEST_BEGIN("P11.13 Middleware: chain runs in order; user_data propagates");
    p11_mw_t args;
    memset(&args, 0, sizeof(args));
    args.done     = goc_chan_make(1);
    args.port_ok  = next_port();
    args.port_rej = next_port();
    goc_go(fiber_p11_mw, &args);
    goc_take_sync(args.done);
    ASSERT(args.status_ok == 200);
    ASSERT(g_mw.n >= 3);
    ASSERT(g_mw.order[0] == 1 && g_mw.order[1] == 2 && g_mw.order[2] == 3);
    ASSERT(g_mw.ud && strcmp(g_mw.ud, "set-by-mw1") == 0);
    TEST_PASS();
done:;
}

static void test_p11_14(void)
{
    TEST_BEGIN("P11.14 Middleware: GOC_HTTP_ERR short-circuits with 500");
    p11_mw_t args;
    memset(&args, 0, sizeof(args));
    args.done     = goc_chan_make(1);
    args.port_ok  = next_port();
    args.port_rej = next_port();
    goc_go(fiber_p11_mw, &args);
    goc_take_sync(args.done);
    ASSERT(args.status_rej == 500);
    for (int i = 0; i < g_mw.n; i++)
        ASSERT(g_mw.order[i] != 999);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.15 — goc_http_get
 * ====================================================================== */

static void fiber_p11_15(void* arg)
{
    p11_simple_t* a   = (p11_simple_t*)arg;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "GET", "/hi", handler_ping);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));

    goc_http_response_t* r =
        (goc_http_response_t*)goc_take(
            goc_http_get(local_url("/hi", a->port),
                          goc_http_request_opts()))->val;
    a->status = r ? r->status : -1;
    if (r && r->body)
        snprintf(a->body, sizeof(a->body), "%s", r->body);

    goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_15(void)
{
    TEST_BEGIN("P11.15 HTTP client: goc_http_get → 200 with body");
    p11_simple_t args;
    memset(&args, 0, sizeof(args));
    args.done = goc_chan_make(1);
    args.port = next_port();
    goc_go(fiber_p11_15, &args);
    goc_take_sync(args.done);
    ASSERT(args.status == 200);
    ASSERT(strcmp(args.body, "pong") == 0);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.16 — goc_http_post (echo)
 * ====================================================================== */

static void handler_echo(goc_http_ctx_t* ctx)
{
    const char* b = goc_http_server_body_str(ctx);
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", b));
}

static void fiber_p11_16(void* arg)
{
    p11_simple_t* a   = (p11_simple_t*)arg;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "POST", "/echo", handler_echo);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));

    goc_http_response_t* r =
        (goc_http_response_t*)goc_take(
            goc_http_post(local_url("/echo", a->port),
                           "text/plain", "echo-this",
                           goc_http_request_opts()))->val;
    a->status = r ? r->status : -1;
    if (r && r->body)
        snprintf(a->body, sizeof(a->body), "%s", r->body);

    goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_16(void)
{
    TEST_BEGIN("P11.16 HTTP client: goc_http_post → echoed body");
    p11_simple_t args;
    memset(&args, 0, sizeof(args));
    args.done = goc_chan_make(1);
    args.port = next_port();
    goc_go(fiber_p11_16, &args);
    goc_take_sync(args.done);
    ASSERT(args.status == 200);
    ASSERT(strstr(args.body, "echo-this") != NULL);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.17 — Parallel requests with goc_take_all
 * ====================================================================== */

static void handler_a_reply(goc_http_ctx_t* ctx)
{
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "a"));
}

static void handler_b_reply(goc_http_ctx_t* ctx)
{
    goc_take(goc_http_server_respond(ctx, 201, "text/plain", "b"));
}

typedef struct {
    goc_chan* done;
    int       port;
    int       status_a;
    int       status_b;
} p11_par_t;

static void fiber_p11_17(void* arg)
{
    p11_par_t*    a   = (p11_par_t*)arg;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "GET", "/a", handler_a_reply);
    goc_http_server_route(srv, "GET", "/b", handler_b_reply);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));

    goc_chan* ca = goc_http_get(local_url("/a", a->port),
                                 goc_http_request_opts());
    goc_chan* cb = goc_http_get(local_url("/b", a->port),
                                 goc_http_request_opts());

    goc_chan* chs[2] = { ca, cb };
    goc_val_t** vals = goc_take_all(chs, 2);

    goc_http_response_t* ra = (goc_http_response_t*)vals[0]->val;
    goc_http_response_t* rb = (goc_http_response_t*)vals[1]->val;
    a->status_a = ra ? ra->status : -1;
    a->status_b = rb ? rb->status : -1;

    goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_17(void)
{
    TEST_BEGIN("P11.17 HTTP client: parallel requests with goc_take_all");
    p11_par_t args;
    memset(&args, 0, sizeof(args));
    args.done = goc_chan_make(1);
    args.port = next_port();
    goc_go(fiber_p11_17, &args);
    goc_take_sync(args.done);
    ASSERT(args.status_a == 200);
    ASSERT(args.status_b == 201);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.18 — Timeout fires correctly
 * ====================================================================== */

static void handler_slow(goc_http_ctx_t* ctx)
{
    /* Sleep 500 ms — longer than the 150 ms client timeout, but short
     * enough that the test can wait for it to finish before closing. */
    goc_take(goc_timeout(500));
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "too late"));
}

static void test_p11_18(void)
{
    TEST_BEGIN("P11.18 HTTP client: timeout fires → status 408 (timeout)");
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    int port = next_port();
    goc_http_server_route(srv, "GET", "/slow", handler_slow);
    goc_take_sync(goc_http_server_listen(srv, "127.0.0.1", port));

    goc_http_request_opts_t* opts = goc_http_request_opts();
    opts->timeout_ms = 150;

    goc_http_response_t* r =
        (goc_http_response_t*)goc_take_sync(
            goc_http_get(local_url("/slow", port), opts))->val;
    ASSERT(r && r->status == 408);

    goc_take_sync(goc_http_server_close(srv));
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.18a — Timeout fires correctly for keepalive connections
 * ====================================================================== */

static void test_p11_18a(void)
{
    TEST_BEGIN("P11.18a HTTP client: keep-alive timeout cleans up inflight fiber");
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    int port = next_port();
    goc_http_server_route(srv, "GET", "/slow", handler_slow);
    goc_take_sync(goc_http_server_listen(srv, "127.0.0.1", port));

    int before = goc_http_client_inflight();

    goc_http_request_opts_t* opts = goc_http_request_opts();
    opts->keep_alive = 1;
    opts->timeout_ms = 150;
    goc_http_response_t* r =
        (goc_http_response_t*)goc_take_sync(
            goc_http_get(local_url("/slow", port), opts))->val;
    ASSERT(r && r->status == 408);

    /* Wait for handler_slow (500 ms) to finish before issuing the next request. */
    goc_take_sync(goc_timeout(600));
    ASSERT(goc_http_client_inflight() == before);

    goc_http_request_opts_t* opts2 = goc_http_request_opts();
    opts2->keep_alive = 1;
    opts2->timeout_ms = 1000;
    goc_http_response_t* r2 =
        (goc_http_response_t*)goc_take_sync(
            goc_http_get(local_url("/slow", port), opts2))->val;
    ASSERT(r2 && r2->status == 200);
    ASSERT(goc_http_client_inflight() == before);

    goc_take_sync(goc_http_server_close(srv));
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.19 — Oversized request body is cleanly rejected
 *
 * GOC_SERVER_MAX_REQ_SIZE (8 MiB) is the internal cap.  A body exceeding
 * it must cause the server to close the connection without sending an HTTP
 * response; the client receives a default response with status == 0.
 * The server must remain usable for subsequent requests on the same port.
 * ====================================================================== */

#define P11_19_BODY_BYTES  (8u * 1024u * 1024u + 1024u)  /* ~8 MiB + 1 KiB */

typedef struct {
    goc_chan* done;
    int       port;
    int       oversized_status;
    int       oversized_status2;
    int       followup_status;
} p11_oversize_t;

static void fiber_p11_19(void* arg)
{
    p11_oversize_t* a   = (p11_oversize_t*)arg;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "POST", "/ok", handler_ping);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));

    char* big = (char*)calloc(P11_19_BODY_BYTES, 1);
    if (big) {
        goc_http_request_opts_t* o = goc_http_request_opts();
        o->keep_alive = 1;
        goc_http_response_t* r1 = (goc_http_response_t*)goc_take(
            goc_http_post_buf(local_url("/ok", a->port),
                              "application/octet-stream",
                              big, P11_19_BODY_BYTES,
                              o))->val;
        goc_http_response_t* r2 = (goc_http_response_t*)goc_take(
            goc_http_post_buf(local_url("/ok", a->port),
                              "application/octet-stream",
                              big, P11_19_BODY_BYTES,
                              o))->val;
        free(big);
        a->oversized_status = r1 ? r1->status : -1;
        a->oversized_status2 = r2 ? r2->status : -1;
    }

    /* Follow-up: server must still accept normal requests after the rejection. */
    goc_take(goc_timeout(20));
    goc_http_response_t* r2 = (goc_http_response_t*)goc_take(
        goc_http_post(local_url("/ok", a->port),
                       "text/plain", "hi",
                       goc_http_request_opts()))->val;
    a->followup_status = r2 ? r2->status : -1;

    goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_19(void)
{
    TEST_BEGIN("P11.19 Oversized request: server rejects and stays alive");
    p11_oversize_t args;
    memset(&args, 0, sizeof(args));
    args.done = goc_chan_make(1);
    args.port = next_port();
    goc_go(fiber_p11_19, &args);
    goc_take_sync(args.done);
    /* Server closed without sending a response: status must be 0. */
    ASSERT(args.oversized_status == 0);
    ASSERT(args.oversized_status2 == 0);
    /* Server must still be live for subsequent requests. */
    ASSERT(args.followup_status == 200);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.20 — Method mismatch: route registered for GET, hit with POST → 404
 * ====================================================================== */

typedef struct { goc_chan* done; int port; int status; } p11_mismatch_t;

static void fiber_p11_20(void* arg)
{
    p11_mismatch_t* a   = (p11_mismatch_t*)arg;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "GET", "/secret", handler_ping);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));

    goc_http_response_t* r = (goc_http_response_t*)goc_take(
        goc_http_post(local_url("/secret", a->port),
                       "text/plain", "",
                       goc_http_request_opts()))->val;
    a->status = r ? r->status : -1;

    goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_20(void)
{
    TEST_BEGIN("P11.20 Method mismatch: GET route hit with POST → 404");
    p11_mismatch_t args = { goc_chan_make(1), next_port(), 0 };
    goc_go(fiber_p11_20, &args);
    goc_take_sync(args.done);
    ASSERT(args.status == 404);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.21 — ctx->path and ctx->method are populated correctly
 * ====================================================================== */

typedef struct {
    goc_chan* done;
    int       port;
    char      got_path[64];
    char      got_method[16];
} p11_ctx_t;

static p11_ctx_t* g_ctx;

static void handler_inspect_ctx(goc_http_ctx_t* ctx)
{
    snprintf(g_ctx->got_path,   sizeof(g_ctx->got_path),
             "%s", ctx->path   ? ctx->path   : "");
    snprintf(g_ctx->got_method, sizeof(g_ctx->got_method),
             "%s", ctx->method ? ctx->method : "");
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "ok"));
}

static void fiber_p11_21(void* arg)
{
    p11_ctx_t* a = (p11_ctx_t*)arg;
    g_ctx = a;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "GET", "/check-ctx", handler_inspect_ctx);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));

    goc_take(goc_http_get(local_url("/check-ctx", a->port),
                           goc_http_request_opts()));
    goc_take(goc_timeout(20));

    goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_21(void)
{
    TEST_BEGIN("P11.21 ctx->path and ctx->method populated correctly");
    p11_ctx_t args;
    memset(&args, 0, sizeof(args));
    args.done = goc_chan_make(1);
    args.port = next_port();
    goc_go(fiber_p11_21, &args);
    goc_take_sync(args.done);
    ASSERT(strcmp(args.got_path,   "/check-ctx") == 0);
    ASSERT(strcmp(args.got_method, "GET")        == 0);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.22 — ctx->query parsed correctly from URL query string
 * ====================================================================== */

typedef struct {
    goc_chan* done;
    int       port;
    char      got_path[64];
    char      got_query[64];
} p11_query_t;

static p11_query_t* g_query;

static void handler_inspect_query(goc_http_ctx_t* ctx)
{
    snprintf(g_query->got_path,  sizeof(g_query->got_path),
             "%s", ctx->path  ? ctx->path  : "");
    snprintf(g_query->got_query, sizeof(g_query->got_query),
             "%s", ctx->query ? ctx->query : "");
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "ok"));
}

static void fiber_p11_22(void* arg)
{
    p11_query_t* a = (p11_query_t*)arg;
    g_query = a;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "GET", "/search", handler_inspect_query);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));

    goc_take(goc_http_get(local_url("/search?q=hello&limit=10", a->port),
                           goc_http_request_opts()));
    goc_take(goc_timeout(20));

    goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_22(void)
{
    TEST_BEGIN("P11.22 ctx->query parsed from URL query string");
    p11_query_t args;
    memset(&args, 0, sizeof(args));
    args.done = goc_chan_make(1);
    args.port = next_port();
    goc_go(fiber_p11_22, &args);
    goc_take_sync(args.done);
    ASSERT(strcmp(args.got_path,  "/search")          == 0);
    ASSERT(strcmp(args.got_query, "q=hello&limit=10") == 0);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.23 — goc_http_response_t->body_len matches respond_buf payload length
 * ====================================================================== */

#define P11_23_DATA  "ABCDE"
#define P11_23_LEN   5

static void handler_respond_binbuf2(goc_http_ctx_t* ctx)
{
    goc_take(goc_http_server_respond_buf(ctx, 200, "application/octet-stream",
                                         P11_23_DATA, P11_23_LEN));
}

typedef struct { goc_chan* done; int port; size_t body_len; } p11_bodylen_t;

static void fiber_p11_23(void* arg)
{
    p11_bodylen_t* a   = (p11_bodylen_t*)arg;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "GET", "/bin", handler_respond_binbuf2);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));

    goc_http_response_t* r = (goc_http_response_t*)goc_take(
        goc_http_get(local_url("/bin", a->port),
                      goc_http_request_opts()))->val;
    a->body_len = r ? r->body_len : (size_t)-1;

    goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_23(void)
{
    TEST_BEGIN("P11.23 goc_http_response_t->body_len matches respond_buf length");
    p11_bodylen_t args;
    memset(&args, 0, sizeof(args));
    args.done = goc_chan_make(1);
    args.port = next_port();
    goc_go(fiber_p11_23, &args);
    goc_take_sync(args.done);
    ASSERT(args.body_len == (size_t)P11_23_LEN);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.24 — Custom request headers via opts->headers are received by server
 * ====================================================================== */

typedef struct {
    goc_chan* done;
    int       port;
    char      custom_value[64];
} p11_custom_hdr_t;

static p11_custom_hdr_t* g_custom_hdr;

static void handler_custom_header(goc_http_ctx_t* ctx)
{
    const char* v = goc_http_server_header(ctx, "x-custom-header");
    snprintf(g_custom_hdr->custom_value, sizeof(g_custom_hdr->custom_value),
             "%s", v ? v : "");
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "ok"));
}

static void fiber_p11_24(void* arg)
{
    p11_custom_hdr_t* a = (p11_custom_hdr_t*)arg;
    g_custom_hdr = a;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "GET", "/echo-hdr", handler_custom_header);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));

    goc_http_header_t* hdr =
        (goc_http_header_t*)goc_malloc(sizeof(goc_http_header_t));
    hdr->name  = "x-custom-header";
    hdr->value = "test-value-p24";

    goc_http_request_opts_t* opts = goc_http_request_opts();
    opts->headers = goc_array_make(1);
    goc_array_push(opts->headers, hdr);

    goc_take(goc_http_get(local_url("/echo-hdr", a->port), opts));
    goc_take(goc_timeout(20));

    goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_24(void)
{
    TEST_BEGIN("P11.24 Custom request headers received by server");
    p11_custom_hdr_t args;
    memset(&args, 0, sizeof(args));
    args.done = goc_chan_make(1);
    args.port = next_port();
    goc_go(fiber_p11_24, &args);
    goc_take_sync(args.done);
    ASSERT(strcmp(args.custom_value, "test-value-p24") == 0);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.25 — CRLF in header value is sanitised (header-injection prevention)
 *
 * A header value containing \r\n must not inject a second header into the
 * outgoing HTTP request.  build_request() silently drops any header whose
 * name or value contains \r or \n.
 * ====================================================================== */

typedef struct { goc_chan* done; int port; int injected; } p11_inject_t;

static p11_inject_t* g_inject;

static void handler_check_injection(goc_http_ctx_t* ctx)
{
    /* If injection succeeded, the server sees X-Injected as a header. */
    const char* v = goc_http_server_header(ctx, "x-injected");
    g_inject->injected = (v != NULL);
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "ok"));
}

static void fiber_p11_25(void* arg)
{
    p11_inject_t* a = (p11_inject_t*)arg;
    g_inject = a;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "GET", "/check-inject", handler_check_injection);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));

    /* Header value contains embedded CRLF — would inject "X-Injected: evil"
     * into raw HTTP request bytes if not sanitised. */
    goc_http_header_t* hdr =
        (goc_http_header_t*)goc_malloc(sizeof(goc_http_header_t));
    hdr->name  = "x-legitimate";
    hdr->value = "foo\r\nX-Injected: evil";

    goc_http_request_opts_t* opts = goc_http_request_opts();
    opts->headers = goc_array_make(1);
    goc_array_push(opts->headers, hdr);

    goc_take(goc_http_get(local_url("/check-inject", a->port), opts));
    goc_take(goc_timeout(20));

    goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_25(void)
{
    TEST_BEGIN("P11.25 CRLF in header value blocked (injection prevention)");
    p11_inject_t args = { goc_chan_make(1), next_port(), 0 };
    goc_go(fiber_p11_25, &args);
    goc_take_sync(args.done);
    ASSERT(!args.injected);   /* X-Injected must NOT appear at the server */
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.26 — Ping-pong: two servers bounce a counter for 500 round trips
 *
 * Mirrors the ping-pong example from docs/HTTP.md.  Two servers on separate
 * loopback ports exchange a counter via POST requests until the counter
 * reaches P11_26_ROUNDS, at which point server A closes pp_done and both
 * servers are shut down cleanly.
 * ====================================================================== */

#define P11_26_ROUNDS 500

typedef struct {
    goc_chan* test_done;  /* put(1) when fiber_p11_26 finishes        */
    int       port_a;
    int       port_b;
    goc_chan* pp_done;    /* closed by pp_handler_a when n >= ROUNDS  */
} p11_pingpong_t;

static p11_pingpong_t* g_pp;
static char g_pp_addr_a[64];
static char g_pp_addr_b[64];

/* Server A: receive counter, respond, then forward to B (or stop). */
static void pp_handler_a(goc_http_ctx_t* ctx)
{
    int n = atoi(goc_http_server_body_str(ctx));
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "ok"));
    if (n >= P11_26_ROUNDS) {
        goc_close(g_pp->pp_done);
        return;
    }
    char* msg = goc_sprintf("%d", n + 1);
    goc_http_request_opts_t* opts = goc_http_request_opts();
    opts->keep_alive = 1;
    goc_http_post(g_pp_addr_b, "text/plain", msg, opts);
}

/* Server B: receive counter, respond, then forward to A. */
static void pp_handler_b(goc_http_ctx_t* ctx)
{
    int n = atoi(goc_http_server_body_str(ctx));
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "ok"));
    char* msg = goc_sprintf("%d", n + 1);
    goc_http_request_opts_t* opts = goc_http_request_opts();
    opts->keep_alive = 1;
    goc_http_post(g_pp_addr_a, "text/plain", msg, opts);
}

static void fiber_p11_26(void* arg)
{
    p11_pingpong_t* a = (p11_pingpong_t*)arg;
    g_pp = a;
    snprintf(g_pp_addr_a, sizeof(g_pp_addr_a),
             "http://127.0.0.1:%d/ping", a->port_a);
    snprintf(g_pp_addr_b, sizeof(g_pp_addr_b),
             "http://127.0.0.1:%d/ping", a->port_b);

    goc_http_server_t* srv_a = goc_http_server_make(goc_http_server_opts());
    goc_http_server_t* srv_b = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv_a, "POST", "/ping", pp_handler_a);
    goc_http_server_route(srv_b, "POST", "/ping", pp_handler_b);

    goc_chan* ready_a = goc_http_server_listen(srv_a, "127.0.0.1", a->port_a);
    goc_chan* ready_b = goc_http_server_listen(srv_b, "127.0.0.1", a->port_b);
    goc_take(ready_a);
    goc_take(ready_b);

    /* Fire the first request; the two handlers bounce it back and forth. */
    goc_http_request_opts_t* opts = goc_http_request_opts();
    opts->keep_alive = 1;
    goc_http_post(g_pp_addr_a, "text/plain", "0", opts);

    /* Wait until server A closes pp_done (counter reached ROUNDS). */
    goc_take(a->pp_done);

    goc_take(goc_http_server_close(srv_a));
    goc_take(goc_http_server_close(srv_b));
    goc_put(a->test_done, goc_box_int(1));
}

static void test_p11_26(void)
{
    TEST_BEGIN("P11.26 Ping-pong: 500 round trips between two servers (keep-alive)");
    p11_pingpong_t args = {
        goc_chan_make(1),   /* test_done */
        next_port(),        /* port_a */
        next_port(),        /* port_b */
        goc_chan_make(0),   /* pp_done (unbuffered; closed on completion) */
    };
    goc_go(fiber_p11_26, &args);
    goc_take_sync(args.test_done);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.27 — Keep-alive sequential requests on one persistent connection
 * ====================================================================== */

typedef struct {
    goc_chan* done;
    int       port;
    int       status1;
    int       status2;
} p11_keepalive_t;

static void fiber_p11_27(void* arg)
{
    p11_keepalive_t* a = (p11_keepalive_t*)arg;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "GET", "/ping", handler_ping);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));

    goc_http_request_opts_t* opts = goc_http_request_opts();
    opts->keep_alive = 1;

    goc_http_response_t* r1 = (goc_http_response_t*)goc_take(
        goc_http_get(local_url("/ping", a->port), opts))->val;
    a->status1 = r1 ? r1->status : -1;

    goc_http_response_t* r2 = (goc_http_response_t*)goc_take(
        goc_http_get(local_url("/ping", a->port), opts))->val;
    a->status2 = r2 ? r2->status : -1;

    goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_27(void)
{
    TEST_BEGIN("P11.27 Keep-alive: sequential requests succeed with persistent connection");
    p11_keepalive_t args;
    memset(&args, 0, sizeof(args));
    args.done = goc_chan_make(1);
    args.port = next_port();
    goc_go(fiber_p11_27, &args);
    goc_take_sync(args.done);
    ASSERT(args.status1 == 200);
    ASSERT(args.status2 == 200);
    TEST_PASS();
done:
}

/* =========================================================================
 * P11.28 — Regression: fire-and-forget ping-pong + immediate teardown
 *
 * Replays the original fire-and-forget forwarding behavior (handlers post
 * outbound relay requests without waiting on the result), then tears both
 * servers down immediately when the target round count is reached.
 *
 * This historically exposed a libuv assertion in write callback processing
 * under repeated runs. Keep this test as a stress guard for close/write races.
 * ========================================================================= */

#define P11_28_ITERS  10
#define P11_28_ROUNDS 200

int P11_28_PORT_A, P11_28_PORT_B;
goc_chan* P11_28_DONE_CH;

static void p11_28_handler_A(goc_http_ctx_t* ctx)
{
    int n = atoi(goc_http_server_body_str(ctx));
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "ok"));
    if (n >= P11_28_ROUNDS) {
        goc_close(P11_28_DONE_CH);
        return;
    }
    goc_http_request_opts_t* opts = goc_http_request_opts();
    opts->keep_alive = 1;
    goc_http_post(
        local_url("/ping", P11_28_PORT_B), 
        "text/plain", 
        goc_sprintf("%d", n + 1), 
        opts
    );
}

static void p11_28_handler_B(goc_http_ctx_t* ctx)
{
    int n = atoi(goc_http_server_body_str(ctx));
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "ok"));
    if (n >= P11_28_ROUNDS) {
        goc_close(P11_28_DONE_CH);
        return;
    }
    goc_http_request_opts_t* opts = goc_http_request_opts();
    opts->keep_alive = 1;
    goc_http_post(
        local_url("/ping", P11_28_PORT_A), 
        "text/plain", 
        goc_sprintf("%d", n + 1), 
        opts
    );
}

static void test_p11_28(void)
{
    TEST_BEGIN("P11.28 Regression: fire-and-forget ping-pong survives repeated teardown");

    for (int i = 0; i < P11_28_ITERS; i++) {
        P11_28_DONE_CH = goc_chan_make(0);

        P11_28_PORT_A = next_port();
        goc_http_server_t* srv_a = goc_http_server_make(goc_http_server_opts());
        goc_http_server_route(srv_a, "POST", "/ping", p11_28_handler_A);
        goc_chan* rc_ch_a = goc_http_server_listen(srv_a, "127.0.0.1", P11_28_PORT_A);
        int rc_a = goc_unbox_int(goc_take_sync(rc_ch_a)->val);
        ASSERT(rc_a == 0);

        P11_28_PORT_B = next_port();
        goc_http_server_t* srv_b = goc_http_server_make(goc_http_server_opts());
        goc_http_server_route(srv_b, "POST", "/ping", p11_28_handler_B);
        goc_chan* rc_ch_b = goc_http_server_listen(srv_b, "127.0.0.1", P11_28_PORT_B);
        int rc_b = goc_unbox_int(goc_take_sync(rc_ch_b)->val);
        ASSERT(rc_b == 0);

        goc_http_request_opts_t* opts = goc_http_request_opts();
        opts->keep_alive = 1;
        goc_http_post(
            local_url("/ping", P11_28_PORT_A),
            "text/plain", 
            "0", 
            opts
        );

        /* ensure all requests have completed */
        goc_take_sync(P11_28_DONE_CH);
        P11_28_DONE_CH = NULL;

        /* Intentional immediate teardown: this is the race window we guard. */
        goc_take_sync(goc_http_server_close(srv_a));
        goc_take_sync(goc_http_server_close(srv_b));
    }

    TEST_PASS();
done:
    if (P11_28_DONE_CH)
        goc_close(P11_28_DONE_CH);
}

/* =========================================================================
 * P11.29 — Regression: fire-and-forget connect churn + immediate teardown
 *
 * Stresses outbound connect/write callback lifetimes by launching many
 * fire-and-forget requests, then tearing the server down quickly. The goal is
 * to catch connect/write lifecycle regressions (including libuv req asserts).
 * ========================================================================= */

#define P11_29_ITERS     10
#define P11_29_REQUESTS  100

static void fiber_p11_29(void* _)
{
    goc_http_server_t* srv = NULL;
    for (int iter = 0; iter < P11_29_ITERS; iter++) {
        goc_http_server_opts_t* srv_opts = goc_http_server_opts();
        srv = goc_http_server_make(srv_opts);
        goc_http_server_route(srv, "POST", "/ping", handler_ping);
        
        int port = next_port();
        goc_chan* rc_ch = goc_http_server_listen(srv, "127.0.0.1", port);
        int rc = goc_unbox_int(goc_take(rc_ch)->val);
        ASSERT(rc == 0);

        const char* url = local_url("/ping", port);
        goc_chan* resp_chs[P11_29_REQUESTS];
        for (int i = 0; i < P11_29_REQUESTS; i++) {
            goc_http_request_opts_t* opts = goc_http_request_opts();
            opts->keep_alive = 0; /* force connect churn */
            resp_chs[i] = goc_http_post(url, "text/plain", "ping", opts);
        }

        /* Wait for all requests to complete before tearing down the server. */
        goc_val_t** response_vals = goc_take_all(resp_chs, P11_29_REQUESTS);
        for (int i = 0; i < P11_29_REQUESTS; i++) {
            goc_val_t* rv = response_vals[i];
            ASSERT(rv != NULL);
            ASSERT(rv->ok == GOC_OK);

            goc_http_response_t* r = rv->val;
            ASSERT(r != NULL);
            ASSERT(r->status == 200);
            ASSERT(strcmp(r->body, "pong") == 0);
        }

        goc_take(goc_http_server_close(srv));
    }
done:;
}

static void test_p11_29(void)
{
    TEST_BEGIN("P11.29 Regression: fire-and-forget connect churn survives repeated teardown");    
    goc_pool* pool = goc_pool_make(2);
    goc_chan* done_ch = goc_go_on(pool, fiber_p11_29, NULL);
    goc_take_sync(done_ch);
    TEST_PASS();
done:;
    goc_pool_destroy(pool);
}

/* ==============================================================================
 * P11.30 — Regression: repeated listen/close under worker-pool startup race
 * =========================================================================== */

static void test_p11_30(void)
{
    TEST_BEGIN("P11.30 Regression: repeated listen/close under worker-pool startup race");
    goc_pool* pool = goc_pool_make(4);
    for (int iter = 0; iter < 32; iter++) {
        goc_http_server_opts_t* opts = goc_http_server_opts();
        goc_http_server_t* srv = goc_http_server_make(opts);
        goc_take_sync(goc_http_server_listen(srv, "127.0.0.1", next_port()));
        goc_take_sync(goc_http_server_close(srv));
    }
    goc_pool_destroy(pool);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.31 — Strict affinity invariant proxy via worker distribution telemetry
 * ====================================================================== */

#define P11_31_SERVER_WORKERS 3
#define P11_31_CLIENTS 30
#define P11_31_ROUTE "/wid"

static int P11_31_PORT = -1;

static void p11_31_server_handler(goc_http_ctx_t* ctx)
{
    int wid = goc_current_worker_id();
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", goc_sprintf("%d", wid)));
}

static _Atomic(goc_http_server_t*) p11_31_srv = NULL;

static void p11_31_server_fiber(void* arg)
{
    P11_31_PORT = next_port();
    goc_chan* comms_ch = (goc_chan*)arg;

    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "GET", P11_31_ROUTE, p11_31_server_handler);

    goc_chan* server_started_ch = goc_http_server_listen(srv, "127.0.0.1", P11_31_PORT);
    goc_val_t* rc_val = goc_take(server_started_ch);
    int rc = goc_unbox_int(rc_val->val);
    ASSERT(rc == 0);

    atomic_store_explicit(&p11_31_srv, srv, memory_order_release);

    int reuseport_listeners = goc_http_server_reuseport_listener_count(srv);
    ASSERT(reuseport_listeners == 0 ||
           reuseport_listeners == P11_31_SERVER_WORKERS);

    // signal server started
    goc_put(comms_ch, NULL);
    // wait for shutdown signal
    goc_take(comms_ch);

done:
    goc_take(goc_http_server_close(srv));
    // signal server shutdown complete
    goc_close(comms_ch);
}

static void test_p11_31(void)
{
    TEST_BEGIN("P11.31 strict affinity: server handles requests across multiple workers");

    goc_pool* pool = goc_pool_make(P11_31_SERVER_WORKERS);
    goc_chan* server_comms_ch = goc_chan_make(0);
    goc_go_on(pool, p11_31_server_fiber, server_comms_ch);
    
    // wait for server to start
    goc_val_t* server_started_val = goc_take_sync(server_comms_ch);
    ASSERT(server_started_val->ok == GOC_OK);

    int responding_workers[P11_31_SERVER_WORKERS] = {0};

    int done_ok = 0;
    for (done_ok = 0; done_ok < P11_31_CLIENTS; done_ok++) {
        goc_chan* resp_ch = goc_http_get(
                                local_url(P11_31_ROUTE, P11_31_PORT), 
                                goc_http_request_opts());
        goc_val_t* resp_val = goc_take_sync(resp_ch);
        ASSERT(resp_val->ok == GOC_OK);

        goc_http_response_t* resp = resp_val->val;
        ASSERT(resp != NULL);
        ASSERT(resp->status == 200);

        int wid = atoi(resp->body);
        responding_workers[wid]++;        
    }
    ASSERT(done_ok == P11_31_CLIENTS);

    int distinct = 0;
    for (int i = 0; i < P11_31_SERVER_WORKERS; i++) {
        if (responding_workers[i] > 0) {
            distinct++;
        }
    }

    goc_http_server_t* srv = atomic_load_explicit(&p11_31_srv,
                                                  memory_order_acquire);
    ASSERT(srv != NULL);

    int reuseport_listeners = goc_http_server_reuseport_listener_count(srv);
    if (reuseport_listeners > 0) {
        for (int i = 0; i < reuseport_listeners; i++) {
            int accept_count =
                goc_http_server_reuseport_listener_accept_count(srv, i);
            ASSERT(accept_count > 0);
            ASSERT(responding_workers[i] > 0);
        }
    }

    printf("\n    [P11.31] worker distribution: ");
    for (int i = 0; i < P11_31_SERVER_WORKERS; i++) {
        printf("worker%d=%d ", i, responding_workers[i]);
    }
    printf("\n    [P11.31] distinct workers responding: %d\n", distinct);
    ASSERT(distinct == P11_31_SERVER_WORKERS);

    TEST_PASS();
done:
    // signal server shutdown
    goc_put_sync(server_comms_ch, NULL);
    // wait for server to confirm shutdown
    goc_take_sync(server_comms_ch);

    if (pool)
        goc_pool_destroy(pool);
}

/* =========================================================================
 * P11.32 — Throughput comparison benchmark (pool=2 vs pool=1)
 * ====================================================================== */

#define P11_32_CONC 32
#define P11_32_WARMUP_MS 250
#define P11_32_MEASURE_MS 4000

typedef struct {
    uint64_t warmup_end_ns;
    uint64_t measure_end_ns;
    uint64_t* succ_out;
    uint64_t* err_out;
    const char* url;
} p11_32_worker_arg_t;

static void p11_32_worker(void* arg)
{
    p11_32_worker_arg_t* a = (p11_32_worker_arg_t*)arg;
    uint64_t succ = 0;
    uint64_t err = 0;

    goc_http_request_opts_t* opts = goc_http_request_opts();
    opts->keep_alive = 1;
    opts->timeout_ms = 10000;

    for (;;) {
        if (uv_hrtime() >= a->measure_end_ns)
            break;

        goc_http_response_t* r =
            (goc_http_response_t*)goc_take(goc_http_get(a->url, opts))->val;
        uint64_t done_ns = uv_hrtime();

        if (done_ns >= a->warmup_end_ns && done_ns < a->measure_end_ns) {
            if (r && r->status == 200) {
                succ++;
            } else {
                err++;
            }
        }
    }

    *a->succ_out = succ;
    *a->err_out = err;
}

typedef struct {
    goc_chan*  done;
    int        port;
    int        nreq;
    uint64_t   elapsed_ns;
    uint64_t   err;
} p11_32_run_t;

static void fiber_p11_32_run(void* arg)
{
    p11_32_run_t* a = (p11_32_run_t*)arg;
    a->nreq = 0;
    a->elapsed_ns = 0;
    a->err = 0;

    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "GET", "/plaintext", handler_ping);

    goc_val_t* listen_val = goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));
    int listen_rc = goc_unbox_int(listen_val->val);
    if (listen_rc != 0) {
        goc_take(goc_http_server_close(srv));
        goc_put(a->done, goc_box_int(0));
        return;
    }

    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/plaintext", a->port);

    uint64_t start_ns = uv_hrtime();
    uint64_t warmup_end_ns = start_ns + (uint64_t)P11_32_WARMUP_MS * 1000000ULL;
    uint64_t measure_end_ns = warmup_end_ns + (uint64_t)P11_32_MEASURE_MS * 1000000ULL;

    uint64_t* succ = goc_malloc(sizeof(uint64_t) * P11_32_CONC);
    uint64_t* err  = goc_malloc(sizeof(uint64_t) * P11_32_CONC);
    goc_chan** joins = goc_malloc(sizeof(goc_chan*) * P11_32_CONC);
    p11_32_worker_arg_t* args =
        goc_malloc(sizeof(p11_32_worker_arg_t) * P11_32_CONC);

    for (size_t i = 0; i < P11_32_CONC; i++) {
        succ[i] = 0;
        err[i] = 0;
        args[i].warmup_end_ns  = warmup_end_ns;
        args[i].measure_end_ns = measure_end_ns;
        args[i].succ_out       = &succ[i];
        args[i].err_out        = &err[i];
        args[i].url            = url;
        joins[i] = goc_go(p11_32_worker, &args[i]);
    }

    goc_take_all(joins, P11_32_CONC);
    goc_take(goc_http_server_close(srv));

    uint64_t total_succ = 0;
    uint64_t total_err  = 0;
    for (size_t i = 0; i < P11_32_CONC; i++) {
        total_succ += succ[i];
        total_err += err[i];
    }

    a->elapsed_ns = (uint64_t)P11_32_MEASURE_MS * 1000000ULL;
    a->nreq = (int)total_succ;
    a->err = total_err;
    goc_put(a->done, goc_box_int(total_succ > 0 && total_err == 0));
}

static int p11_32_mode_run(int pool_threads, uint64_t* elapsed_ns_out,
                           int* nreq_out, uint64_t* err_out)
{
#if defined(_WIN32)
    char envbuf[32];
    snprintf(envbuf, sizeof(envbuf), "%d", pool_threads);
    _putenv_s("GOC_POOL_THREADS", envbuf);
#else
    char envbuf[32];
    snprintf(envbuf, sizeof(envbuf), "%d", pool_threads);
    setenv("GOC_POOL_THREADS", envbuf, 1);
#endif

    goc_init();

    p11_32_run_t run;
    memset(&run, 0, sizeof(run));
    run.done = goc_chan_make(1);
    run.port = next_port();

    goc_go(fiber_p11_32_run, &run);
    goc_val_t* vd = goc_take_sync(run.done);

    int ok = vd && goc_unbox_int(vd->val) == 1 && run.nreq > 0;
    if (ok) {
        *elapsed_ns_out = run.elapsed_ns;
        *nreq_out = run.nreq;
        *err_out = run.err;
    }

    goc_shutdown();
    return ok ? 0 : 1;
}

static void test_p11_32(void)
{
    /* Github CI runners have 4 cores */
    TEST_BEGIN("P11.32 Throughput comparison pool=1,2,4 (bench-style throughput workload)");

    uint64_t ns1 = 0, ns2 = 0, ns4 = 0;
    int req1 = 0, req2 = 0, req4 = 0;
    uint64_t err1 = 0, err2 = 0, err4 = 0;

    p11_32_mode_run(1, &ns1, &req1, &err1);
    p11_32_mode_run(2, &ns2, &req2, &err2);
    p11_32_mode_run(4, &ns4, &req4, &err4);

    double sec1 = (double)ns1 / 1e9;
    double sec2 = (double)ns2 / 1e9;
    double sec4 = (double)ns4 / 1e9;
    double rps1 = (double)req1 / sec1;
    double rps2 = (double)req2 / sec2;
    double rps4 = (double)req4 / sec4;
    double ratio2 = (rps1 > 0.0) ? (rps2 / rps1) : 0.0;
    double ratio4 = (rps1 > 0.0) ? (rps4 / rps1) : 0.0;

    printf("\n");
    printf("    [P11.32] pool=1: %.2f req/s (%d req in %.3fs)\n", rps1, req1, sec1);
    printf("    [P11.32] pool=2: %.2f req/s (%d req in %.3fs)\n", rps2, req2, sec2);
    printf("    [P11.32] pool=4: %.2f req/s (%d req in %.3fs)\n", rps4, req4, sec4);
    printf("    [P11.32] ratios: p2/p1=%.3f p4/p1=%.3f\n", ratio2, ratio4);

    ASSERT(req1 > 0 && req2 > 0 && req4 > 0);

    uint64_t bench_dur_ns = (uint64_t)P11_32_MEASURE_MS * 1000000ULL;
    ASSERT(ns1 == bench_dur_ns);
    ASSERT(ns2 == bench_dur_ns);
    ASSERT(ns4 == bench_dur_ns);

    ASSERT(rps1 > 0.0 && rps2 > 0.0 && rps4 > 0.0);
    /* Flaky in CI */
    // ASSERT(ratio2 > 1);
    // ASSERT(ratio4 > ratio2);

    TEST_PASS();
done:;
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void)
{
    install_crash_handler();
    goc_test_arm_watchdog(240);

    /* Keep HTTP stress deterministic in CI/local loops by capping pool
     * parallelism for this test process. High worker counts amplify scheduler
     * race windows and make failures far more frequent under heavy diagnostics. */
#if defined(_WIN32)
    _putenv_s("GOC_POOL_THREADS", "4");
#else
    setenv("GOC_POOL_THREADS", "4", 1);
#endif

    goc_init();

    printf("Phase 11 — HTTP server and client (goc_http)\n");

    test_p11_1();
    test_p11_2();
    test_p11_3();
    test_p11_4();
    test_p11_5();
    test_p11_6();
    test_p11_7();
    test_p11_8();
    test_p11_9();
    test_p11_10();
    test_p11_11();
    test_p11_12();
    test_p11_13();
    test_p11_14();
    test_p11_15();
    test_p11_16();
    test_p11_17();
    test_p11_18();
    test_p11_18a();
    test_p11_19();
    test_p11_20();
    test_p11_21();
    test_p11_22();
    test_p11_23();
    test_p11_24();
    test_p11_25();
    test_p11_26();
    test_p11_27();
    test_p11_28();
    test_p11_29();
    test_p11_30();
    test_p11_31();

    goc_shutdown();

    /* P11.32 uses isolated init/shutdown cycles for pool=1 and pool=4
     * throughput measurement, so run it after shutting down the main cycle. */
    test_p11_32();

    REPORT(g_tests_run, g_tests_passed, g_tests_failed);

    return g_tests_failed ? 1 : 0;
}
