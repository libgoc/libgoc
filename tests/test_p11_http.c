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
 */

#if !defined(_WIN32) && !defined(__APPLE__)
#  define _GNU_SOURCE
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "test_harness.h"
#include "goc.h"
#include "goc_array.h"
#include "goc_http.h"

/* =========================================================================
 * Helpers
 * ====================================================================== */

static int s_port = 18400;

static int next_port(void)
{
    return s_port++;
}

static char s_url_buf[256];
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
    int rc = (int)goc_unbox_int(
        goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port))->val);
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
    int       port;
    int       has_ct;
    int       absent_ok;
    int       case_ok;
} p11_hdr_t;

static p11_hdr_t* g_hdr;

static void handler_inspect_headers(goc_http_ctx_t* ctx)
{
    const char* ct  = goc_http_server_header(ctx, "content-type");
    g_hdr->has_ct    = (ct != NULL);
    const char* x   = goc_http_server_header(ctx, "x-no-such-header-p11");
    g_hdr->absent_ok = (x == NULL);
    const char* ct2 = goc_http_server_header(ctx, "CONTENT-TYPE");
    g_hdr->case_ok   = (ct2 != NULL);
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "ok"));
}

static void fiber_p11_hdrs(void* arg)
{
    p11_hdr_t*    a   = (p11_hdr_t*)arg;
    g_hdr = a;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "POST", "/hdr", handler_inspect_headers);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));

    goc_take(goc_http_post(local_url("/hdr", a->port),
                            "application/json", "{}", goc_http_request_opts()));
    goc_take(goc_timeout(20));

    goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_5(void)
{
    TEST_BEGIN("P11.5  goc_http_server_header: present header found");
    p11_hdr_t args;
    memset(&args, 0, sizeof(args));
    args.done = goc_chan_make(1);
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

typedef struct {
    goc_chan* done;
    int       port;
    int       timed_out;
} p11_to_t;

static void fiber_p11_18(void* arg)
{
    p11_to_t*     a   = (p11_to_t*)arg;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "GET", "/slow", handler_slow);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));

    goc_http_request_opts_t* opts = goc_http_request_opts();
    opts->timeout_ms = 150;

    goc_http_response_t* r =
        (goc_http_response_t*)goc_take(
            goc_http_get(local_url("/slow", a->port), opts))->val;
    a->timed_out = (!r || r->status == 0);

    /* Wait for handler_slow (500 ms) to finish before closing. */
    goc_take(goc_timeout(600));
    goc_take(goc_http_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_18(void)
{
    TEST_BEGIN("P11.18 HTTP client: timeout fires → status 0 (timeout)");
    p11_to_t args = { goc_chan_make(1), next_port(), 0 };
    goc_go(fiber_p11_18, &args);
    goc_take_sync(args.done);
    ASSERT(args.timed_out);
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
    GOC_DBG("P11.25 handler_check_injection: entered ctx=%p\n", (void*)ctx);
    /* If injection succeeded, the server sees X-Injected as a header. */
    const char* v = goc_http_server_header(ctx, "x-injected");
    g_inject->injected = (v != NULL);
    GOC_DBG("P11.25 handler_check_injection: x-injected=%s\n", v ? v : "<NULL>");
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "ok"));
    GOC_DBG("P11.25 handler_check_injection: responded\n");
}

static void fiber_p11_25(void* arg)
{
    p11_inject_t* a = (p11_inject_t*)arg;
    GOC_DBG("P11.25 fiber: start port=%d done=%p\n", a->port, (void*)a->done);
    g_inject = a;
    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "GET", "/check-inject", handler_check_injection);
    GOC_DBG("P11.25 fiber: listening\n");
    goc_take(goc_http_server_listen(srv, "127.0.0.1", a->port));
    GOC_DBG("P11.25 fiber: listen done\n");

    /* Header value contains embedded CRLF — would inject "X-Injected: evil"
     * into raw HTTP request bytes if not sanitised. */
    goc_http_header_t* hdr =
        (goc_http_header_t*)goc_malloc(sizeof(goc_http_header_t));
    hdr->name  = "x-legitimate";
    hdr->value = "foo\r\nX-Injected: evil";

    goc_http_request_opts_t* opts = goc_http_request_opts();
    opts->headers = goc_array_make(1);
    goc_array_push(opts->headers, hdr);

    GOC_DBG("P11.25 fiber: before goc_http_get\n");
    goc_take(goc_http_get(local_url("/check-inject", a->port), opts));
    GOC_DBG("P11.25 fiber: after goc_http_get\n");
    GOC_DBG("P11.25 fiber: before timeout\n");
    goc_take(goc_timeout(20));
    GOC_DBG("P11.25 fiber: after timeout\n");

    GOC_DBG("P11.25 fiber: before server_close\n");
    goc_take(goc_http_server_close(srv));
    GOC_DBG("P11.25 fiber: after server_close\n");
    goc_put(a->done, goc_box_int(1));
    GOC_DBG("P11.25 fiber: done put\n");
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
 * Mirrors the ping-pong example from HTTP.md.  Two servers on separate
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
done:;
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

#define P11_28_ITERS  8
#define P11_28_ROUNDS 250

typedef struct {
    goc_chan* test_done;
    int       ok;
} p11_ff_reg_t;

typedef struct {
    goc_chan* done;
    int       rounds;
    char      url_a[64];
    char      url_b[64];
} p11_ff_ctx_t;

static p11_ff_ctx_t* g_ff_ctx = NULL;

static void ff_handler_a(goc_http_ctx_t* ctx)
{
    int n = atoi(goc_http_server_body_str(ctx));
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "ok"));
    if (n >= g_ff_ctx->rounds) {
        goc_close(g_ff_ctx->done);
        return;
    }
    goc_http_request_opts_t* opts = goc_http_request_opts();
    opts->keep_alive = 1;
    goc_http_post(g_ff_ctx->url_b, "text/plain", goc_sprintf("%d", n + 1), opts);
}

static void ff_handler_b(goc_http_ctx_t* ctx)
{
    int n = atoi(goc_http_server_body_str(ctx));
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "ok"));
    goc_http_request_opts_t* opts = goc_http_request_opts();
    opts->keep_alive = 1;
    goc_http_post(g_ff_ctx->url_a, "text/plain", goc_sprintf("%d", n + 1), opts);
}

static void fiber_p11_28(void* arg)
{
    p11_ff_reg_t* a = (p11_ff_reg_t*)arg;
    a->ok = 1;

    for (int i = 0; i < P11_28_ITERS; i++) {
        int port_a = next_port();
        int port_b = next_port();

        p11_ff_ctx_t* ctx = (p11_ff_ctx_t*)goc_malloc(sizeof(p11_ff_ctx_t));
        ctx->done   = goc_chan_make(0);
        ctx->rounds = P11_28_ROUNDS;
        snprintf(ctx->url_a, sizeof(ctx->url_a), "http://127.0.0.1:%d/ping", port_a);
        snprintf(ctx->url_b, sizeof(ctx->url_b), "http://127.0.0.1:%d/ping", port_b);
        g_ff_ctx = ctx;

        goc_http_server_t* srv_a = goc_http_server_make(goc_http_server_opts());
        goc_http_server_t* srv_b = goc_http_server_make(goc_http_server_opts());
        goc_http_server_route(srv_a, "POST", "/ping", ff_handler_a);
        goc_http_server_route(srv_b, "POST", "/ping", ff_handler_b);

        goc_take(goc_http_server_listen(srv_a, "127.0.0.1", port_a));
        goc_take(goc_http_server_listen(srv_b, "127.0.0.1", port_b));

        goc_http_request_opts_t* opts = goc_http_request_opts();
        opts->keep_alive = 1;
        goc_http_post(ctx->url_a, "text/plain", "0", opts);

        goc_take(ctx->done);

        /* Intentional immediate teardown: this is the race window we guard. */
        goc_take(goc_http_server_close(srv_a));
        goc_take(goc_http_server_close(srv_b));
    }

    goc_put(a->test_done, goc_box_int(1));
}

static void test_p11_28(void)
{
    TEST_BEGIN("P11.28 Regression: fire-and-forget ping-pong survives repeated teardown");
    p11_ff_reg_t args;
    memset(&args, 0, sizeof(args));
    args.test_done = goc_chan_make(1);
    goc_go(fiber_p11_28, &args);
    goc_take_sync(args.test_done);
    ASSERT(args.ok == 1);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P11.29 — Regression: fire-and-forget connect churn + immediate teardown
 *
 * Stresses outbound connect/write callback lifetimes by launching many
 * fire-and-forget requests, then tearing the server down quickly. The goal is
 * to catch connect/write lifecycle regressions (including libuv req asserts).
 * ========================================================================= */

#define P11_29_ITERS     6
#define P11_29_REQUESTS  600

typedef struct {
    goc_chan* done;
    int       ok;
} p11_ff_churn_t;

static void fiber_p11_29(void* arg)
{
    p11_ff_churn_t* a = (p11_ff_churn_t*)arg;
    a->ok = 1;

    for (int iter = 0; iter < P11_29_ITERS; iter++) {
        int port = next_port();
        goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
        goc_http_server_route(srv, "POST", "/ok", handler_ping);
        goc_take(goc_http_server_listen(srv, "127.0.0.1", port));

        const char* url = local_url("/ok", port);
        for (int i = 0; i < P11_29_REQUESTS; i++) {
            goc_http_request_opts_t* opts = goc_http_request_opts();
            opts->keep_alive = 0; /* force connect churn */
            goc_http_post(url, "text/plain", "x", opts);
        }

        /* Keep this intentionally short to maximize overlap with in-flight IO.
         */
        goc_take(goc_timeout(15));
        goc_take(goc_http_server_close(srv));
    }

    goc_put(a->done, goc_box_int(1));
}

static void test_p11_29(void)
{
    TEST_BEGIN("P11.29 Regression: fire-and-forget connect churn survives repeated teardown");
    p11_ff_churn_t args;
    memset(&args, 0, sizeof(args));
    args.done = goc_chan_make(1);
    goc_go(fiber_p11_29, &args);
    goc_take_sync(args.done);
    ASSERT(args.ok == 1);
    TEST_PASS();
done:;
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void)
{
    install_crash_handler();
    goc_test_arm_watchdog(90);

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


    printf("\n%d/%d tests passed", g_tests_passed, g_tests_run);
    if (g_tests_failed)
        printf(", %d FAILED", g_tests_failed);
    printf("\n");

    goc_shutdown();
    return g_tests_failed ? 1 : 0;
}
