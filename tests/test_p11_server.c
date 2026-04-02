/*
 * tests/test_p11_server.c — Phase 11: HTTP server and client tests
 *
 * Verifies the goc_server HTTP library declared in goc_server.h.
 * Tests run a real HTTP server on loopback ports and make real HTTP client
 * requests against it.
 *
 * Build:  cmake -B build -DLIBGOC_SERVER=ON && cmake --build build
 * Run:    ctest --test-dir build --output-on-failure
 *         ./build/test_p11_server
 *
 * Test coverage:
 *
 *   P11.1  Server lifecycle: make → listen → close (no routes)
 *   P11.2  Routing: exact path match
 *   P11.3  Routing: catch-all wildcard /*
 *   P11.4  Routing: unmatched request → 404
 *   P11.5  Request context helpers: goc_server_header (present)
 *   P11.6  Request context helpers: goc_server_header (absent)
 *   P11.7  Request context helpers: goc_server_header (case-insensitive)
 *   P11.8  Request context helpers: goc_server_body_str (with body)
 *   P11.9  Request context helpers: goc_server_body_str (empty body)
 *   P11.10 Response: goc_server_respond (200)
 *   P11.11 Response: goc_server_respond_buf
 *   P11.12 Response: goc_server_respond_error
 *   P11.13 Middleware: chain runs in order; user_data propagates
 *   P11.14 Middleware: GOC_SERVER_ERR short-circuits with 500
 *   P11.15 HTTP client: goc_http_get
 *   P11.16 HTTP client: goc_http_post
 *   P11.17 HTTP client: parallel requests with goc_take_all
 *   P11.18 HTTP client: timeout fires correctly
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
#include "goc_server.h"

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
    goc_take(goc_server_respond(ctx, 200, "text/plain", "pong"));
}

/* =========================================================================
 * P11.1 — Server lifecycle: make → listen → close
 * ====================================================================== */

typedef struct { goc_chan* done; int port; } p11_1_args_t;

static void fiber_p11_1(void* arg)
{
    p11_1_args_t* a   = (p11_1_args_t*)arg;
    goc_server_t* srv = goc_server_make(goc_server_opts());
    int rc = goc_server_listen(srv, "127.0.0.1", a->port);
    int ok = (rc == 0);
    if (ok)
        goc_take(goc_server_close(srv));
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
    goc_server_t* srv = goc_server_make(goc_server_opts());
    goc_server_route(srv, "GET", "/ping", handler_ping);
    goc_server_listen(srv, "127.0.0.1", a->port);
    goc_take(goc_timeout(50));

    goc_http_response_t* r =
        (goc_http_response_t*)goc_take(
            goc_http_get(local_url("/ping", a->port),
                          goc_http_request_opts()))->val;
    a->status = r ? r->status : -1;
    if (r && r->body)
        snprintf(a->body, sizeof(a->body), "%s", r->body);

    goc_take(goc_server_close(srv));
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
    goc_take(goc_server_respond(ctx, 200, "text/plain", "catch-all"));
}

typedef struct { goc_chan* done; int port; int status; } p11_status_t;

static void fiber_p11_3(void* arg)
{
    p11_status_t* a   = (p11_status_t*)arg;
    goc_server_t* srv = goc_server_make(goc_server_opts());
    goc_server_route(srv, "*", "/*", handler_catch_all);
    goc_server_listen(srv, "127.0.0.1", a->port);
    goc_take(goc_timeout(50));

    goc_http_response_t* r =
        (goc_http_response_t*)goc_take(
            goc_http_get(local_url("/anything/at/all", a->port),
                          goc_http_request_opts()))->val;
    a->status = r ? r->status : -1;

    goc_take(goc_server_close(srv));
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
    goc_server_t* srv = goc_server_make(goc_server_opts());
    goc_server_route(srv, "GET", "/exists", handler_ping);
    goc_server_listen(srv, "127.0.0.1", a->port);
    goc_take(goc_timeout(50));

    goc_http_response_t* r =
        (goc_http_response_t*)goc_take(
            goc_http_get(local_url("/missing", a->port),
                          goc_http_request_opts()))->val;
    a->status = r ? r->status : -1;

    goc_take(goc_server_close(srv));
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
 * P11.5-7 — goc_server_header helpers
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
    const char* ct  = goc_server_header(ctx, "content-type");
    g_hdr->has_ct    = (ct != NULL);
    const char* x   = goc_server_header(ctx, "x-no-such-header-p11");
    g_hdr->absent_ok = (x == NULL);
    const char* ct2 = goc_server_header(ctx, "CONTENT-TYPE");
    g_hdr->case_ok   = (ct2 != NULL);
    goc_take(goc_server_respond(ctx, 200, "text/plain", "ok"));
}

static void fiber_p11_hdrs(void* arg)
{
    p11_hdr_t*    a   = (p11_hdr_t*)arg;
    g_hdr = a;
    goc_server_t* srv = goc_server_make(goc_server_opts());
    goc_server_route(srv, "POST", "/hdr", handler_inspect_headers);
    goc_server_listen(srv, "127.0.0.1", a->port);
    goc_take(goc_timeout(50));

    goc_take(goc_http_post(local_url("/hdr", a->port),
                            "application/json", "{}", goc_http_request_opts()));
    goc_take(goc_timeout(100));

    goc_take(goc_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_5(void)
{
    TEST_BEGIN("P11.5  goc_server_header: present header found");
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
    TEST_BEGIN("P11.6  goc_server_header: absent header returns NULL");
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
    TEST_BEGIN("P11.7  goc_server_header: case-insensitive lookup");
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
 * P11.8-9 — goc_server_body_str
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
    const char* b = goc_server_body_str(ctx);
    if (b)
        snprintf(g_body->body_received, sizeof(g_body->body_received),
                 "%s", b);
    goc_take(goc_server_respond(ctx, 200, "text/plain", "ok"));
}

static void handler_body_empty(goc_http_ctx_t* ctx)
{
    const char* b = goc_server_body_str(ctx);
    g_body->empty_ok = (b != NULL && b[0] == '\0');
    goc_take(goc_server_respond(ctx, 200, "text/plain", "ok"));
}

static void fiber_p11_body(void* arg)
{
    p11_body_t*   a   = (p11_body_t*)arg;
    g_body = a;

    goc_server_t* srv1 = goc_server_make(goc_server_opts());
    goc_server_route(srv1, "POST", "/body", handler_body_check);
    goc_server_listen(srv1, "127.0.0.1", a->port_body);

    goc_server_t* srv2 = goc_server_make(goc_server_opts());
    goc_server_route(srv2, "GET", "/empty", handler_body_empty);
    goc_server_listen(srv2, "127.0.0.1", a->port_empty);

    goc_take(goc_timeout(50));

    goc_take(goc_http_post(local_url("/body", a->port_body),
                            "text/plain", "hello-body",
                            goc_http_request_opts()));
    goc_take(goc_timeout(100));

    goc_take(goc_http_get(local_url("/empty", a->port_empty),
                           goc_http_request_opts()));
    goc_take(goc_timeout(100));

    goc_take(goc_server_close(srv1));
    goc_take(goc_server_close(srv2));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_8(void)
{
    TEST_BEGIN("P11.8  goc_server_body_str: POST body received correctly");
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
    TEST_BEGIN("P11.9  goc_server_body_str: empty body returns \"\"");
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
 * P11.10 — goc_server_respond (200)
 * ====================================================================== */

static void fiber_p11_10(void* arg)
{
    p11_simple_t* a   = (p11_simple_t*)arg;
    goc_server_t* srv = goc_server_make(goc_server_opts());
    goc_server_route(srv, "GET", "/hello", handler_ping);
    goc_server_listen(srv, "127.0.0.1", a->port);
    goc_take(goc_timeout(50));

    goc_http_response_t* r =
        (goc_http_response_t*)goc_take(
            goc_http_get(local_url("/hello", a->port),
                          goc_http_request_opts()))->val;
    a->status = r ? r->status : -1;
    if (r && r->body)
        snprintf(a->body, sizeof(a->body), "%s", r->body);

    goc_take(goc_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_10(void)
{
    TEST_BEGIN("P11.10 goc_server_respond: status 200, body \"pong\"");
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
 * P11.11 — goc_server_respond_buf
 * ====================================================================== */

static void handler_respond_buf(goc_http_ctx_t* ctx)
{
    static const char data[] = "Hello";
    goc_take(goc_server_respond_buf(ctx, 201, "application/octet-stream",
                                     data, 5));
}

static void fiber_p11_11(void* arg)
{
    p11_simple_t* a   = (p11_simple_t*)arg;
    goc_server_t* srv = goc_server_make(goc_server_opts());
    goc_server_route(srv, "GET", "/buf", handler_respond_buf);
    goc_server_listen(srv, "127.0.0.1", a->port);
    goc_take(goc_timeout(50));

    goc_http_response_t* r =
        (goc_http_response_t*)goc_take(
            goc_http_get(local_url("/buf", a->port),
                          goc_http_request_opts()))->val;
    a->status = r ? r->status : -1;
    if (r && r->body)
        snprintf(a->body, sizeof(a->body), "%s", r->body);

    goc_take(goc_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_11(void)
{
    TEST_BEGIN("P11.11 goc_server_respond_buf: status 201, binary body");
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
 * P11.12 — goc_server_respond_error
 * ====================================================================== */

static void handler_respond_err(goc_http_ctx_t* ctx)
{
    goc_take(goc_server_respond_error(ctx, 400, "bad input"));
}

static void fiber_p11_12(void* arg)
{
    p11_simple_t* a   = (p11_simple_t*)arg;
    goc_server_t* srv = goc_server_make(goc_server_opts());
    goc_server_route(srv, "GET", "/err", handler_respond_err);
    goc_server_listen(srv, "127.0.0.1", a->port);
    goc_take(goc_timeout(50));

    goc_http_response_t* r =
        (goc_http_response_t*)goc_take(
            goc_http_get(local_url("/err", a->port),
                          goc_http_request_opts()))->val;
    a->status = r ? r->status : -1;
    if (r && r->body)
        snprintf(a->body, sizeof(a->body), "%s", r->body);

    goc_take(goc_server_close(srv));
    goc_put(a->done, goc_box_int(1));
}

static void test_p11_12(void)
{
    TEST_BEGIN("P11.12 goc_server_respond_error: status 400, error body");
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

static goc_server_status_t mw1(goc_http_ctx_t* ctx)
{
    g_mw.order[g_mw.n++] = 1;
    ctx->user_data = (void*)"set-by-mw1";
    return GOC_SERVER_OK;
}

static goc_server_status_t mw2(goc_http_ctx_t* ctx)
{
    g_mw.order[g_mw.n++] = 2;
    (void)ctx;
    return GOC_SERVER_OK;
}

static void handler_mw_ok(goc_http_ctx_t* ctx)
{
    g_mw.order[g_mw.n++] = 3;
    g_mw.ud = (const char*)ctx->user_data;
    goc_take(goc_server_respond(ctx, 200, "text/plain", "mw-ok"));
}

static goc_server_status_t mw_reject(goc_http_ctx_t* ctx)
{
    (void)ctx;
    g_mw.order[g_mw.n++] = 99;
    return GOC_SERVER_ERR;
}

static void handler_mw_never(goc_http_ctx_t* ctx)
{
    g_mw.order[g_mw.n++] = 999;
    goc_take(goc_server_respond(ctx, 200, "text/plain", "unreachable"));
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

    goc_server_opts_t* opts_ok = goc_server_opts();
    opts_ok->middleware = goc_array_make(2);
    goc_array_push(opts_ok->middleware, (void*)(uintptr_t)mw1);
    goc_array_push(opts_ok->middleware, (void*)(uintptr_t)mw2);
    goc_server_t* srv_ok = goc_server_make(opts_ok);
    goc_server_route(srv_ok, "GET", "/mw", handler_mw_ok);
    goc_server_listen(srv_ok, "127.0.0.1", a->port_ok);

    goc_server_opts_t* opts_rej = goc_server_opts();
    opts_rej->middleware = goc_array_make(1);
    goc_array_push(opts_rej->middleware, (void*)(uintptr_t)mw_reject);
    goc_server_t* srv_rej = goc_server_make(opts_rej);
    goc_server_route(srv_rej, "GET", "/mw", handler_mw_never);
    goc_server_listen(srv_rej, "127.0.0.1", a->port_rej);

    goc_take(goc_timeout(50));

    goc_http_response_t* r_ok =
        (goc_http_response_t*)goc_take(
            goc_http_get(local_url("/mw", a->port_ok),
                          goc_http_request_opts()))->val;
    a->status_ok = r_ok ? r_ok->status : -1;
    goc_take(goc_timeout(150));

    goc_http_response_t* r_rej =
        (goc_http_response_t*)goc_take(
            goc_http_get(local_url("/mw", a->port_rej),
                          goc_http_request_opts()))->val;
    a->status_rej = r_rej ? r_rej->status : -1;
    goc_take(goc_timeout(50));

    goc_take(goc_server_close(srv_ok));
    goc_take(goc_server_close(srv_rej));
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
    TEST_BEGIN("P11.14 Middleware: GOC_SERVER_ERR short-circuits with 500");
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
    goc_server_t* srv = goc_server_make(goc_server_opts());
    goc_server_route(srv, "GET", "/hi", handler_ping);
    goc_server_listen(srv, "127.0.0.1", a->port);
    goc_take(goc_timeout(50));

    goc_http_response_t* r =
        (goc_http_response_t*)goc_take(
            goc_http_get(local_url("/hi", a->port),
                          goc_http_request_opts()))->val;
    a->status = r ? r->status : -1;
    if (r && r->body)
        snprintf(a->body, sizeof(a->body), "%s", r->body);

    goc_take(goc_server_close(srv));
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
    const char* b = goc_server_body_str(ctx);
    goc_take(goc_server_respond(ctx, 200, "text/plain", b));
}

static void fiber_p11_16(void* arg)
{
    p11_simple_t* a   = (p11_simple_t*)arg;
    goc_server_t* srv = goc_server_make(goc_server_opts());
    goc_server_route(srv, "POST", "/echo", handler_echo);
    goc_server_listen(srv, "127.0.0.1", a->port);
    goc_take(goc_timeout(50));

    goc_http_response_t* r =
        (goc_http_response_t*)goc_take(
            goc_http_post(local_url("/echo", a->port),
                           "text/plain", "echo-this",
                           goc_http_request_opts()))->val;
    a->status = r ? r->status : -1;
    if (r && r->body)
        snprintf(a->body, sizeof(a->body), "%s", r->body);

    goc_take(goc_server_close(srv));
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
    goc_take(goc_server_respond(ctx, 200, "text/plain", "a"));
}

static void handler_b_reply(goc_http_ctx_t* ctx)
{
    goc_take(goc_server_respond(ctx, 201, "text/plain", "b"));
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
    goc_server_t* srv = goc_server_make(goc_server_opts());
    goc_server_route(srv, "GET", "/a", handler_a_reply);
    goc_server_route(srv, "GET", "/b", handler_b_reply);
    goc_server_listen(srv, "127.0.0.1", a->port);
    goc_take(goc_timeout(50));

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

    goc_take(goc_server_close(srv));
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
    goc_take(goc_server_respond(ctx, 200, "text/plain", "too late"));
}

typedef struct {
    goc_chan* done;
    int       port;
    int       timed_out;
} p11_to_t;

static void fiber_p11_18(void* arg)
{
    p11_to_t*     a   = (p11_to_t*)arg;
    goc_server_t* srv = goc_server_make(goc_server_opts());
    goc_server_route(srv, "GET", "/slow", handler_slow);
    goc_server_listen(srv, "127.0.0.1", a->port);
    goc_take(goc_timeout(50));

    goc_http_request_opts_t* opts = goc_http_request_opts();
    opts->timeout_ms = 150;

    goc_http_response_t* r =
        (goc_http_response_t*)goc_take(
            goc_http_get(local_url("/slow", a->port), opts))->val;
    a->timed_out = (!r || r->status == 0);

    /* Wait for handler_slow (500 ms) to finish before closing. */
    goc_take(goc_timeout(600));
    goc_take(goc_server_close(srv));
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
 * main
 * ====================================================================== */

int main(void)
{
    install_crash_handler();
    goc_test_arm_watchdog(120);
    goc_init();

    printf("Phase 11 — HTTP server and client (goc_server)\n");

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

    printf("\n%d/%d tests passed", g_tests_passed, g_tests_run);
    if (g_tests_failed)
        printf(", %d FAILED", g_tests_failed);
    printf("\n");

    goc_shutdown();
    return g_tests_failed ? 1 : 0;
}
