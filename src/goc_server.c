/*
 * src/goc_server.c — HTTP server and client for libgoc
 *
 * Implements the API declared in include/goc_server.h.
 *
 * Server: powered by libh2o sharing libgoc's libuv event loop.
 *   A single catch-all H2O handler dispatches all requests into fibers via
 *   goc_go_on().  Route matching (method + path prefix) is done in on_req
 *   before spawning the fiber.  goc_server_respond* dispatches h2o_send_inline
 *   to the event loop thread via a one-shot uv_async_t (same as goc_io).
 *
 * HTTP client: libuv TCP + hand-written HTTP/1.1.
 *   URL parsing uses H2O's h2o_url_parse.
 *   DNS uses uv_getaddrinfo.
 *   The request is marshalled to the loop thread via uv_async_t.
 *   Timeout is honoured via goc_timeout + goc_alts (fiber context only).
 */

#if !defined(_WIN32) && !defined(__APPLE__)
#  define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#include <h2o.h>
#include "../include/goc_server.h"
#include "../include/goc_array.h"
#include "internal.h"

/* =========================================================================
 * Shared utilities
 * ====================================================================== */

static void srv_handle_unregister(uv_handle_t* h)
{
    gc_handle_unregister(h);
}

static void close_on_put(goc_status_t ok, void* ud)
{
    (void)ok;
    goc_close((goc_chan*)ud);
}

/* =========================================================================
 * Internal types
 * ====================================================================== */

typedef struct {
    const char*        method;
    const char*        pattern;
    goc_http_handler_t handler;
} goc_route_t;

/* H2O handler extension — one per server, alloced by H2O. */
typedef struct {
    h2o_handler_t    super;  /* MUST be first */
    struct goc_server* srv;
} goc_h2o_handler_t;

/*
 * Per-request container.
 * The public goc_http_ctx_t is embedded last so WRAPPER_FROM_CTX can recover
 * the full container from a ctx pointer using offsetof arithmetic.
 */
typedef struct {
    h2o_req_t*         h2o_req;
    struct goc_server* srv;
    goc_http_handler_t handler;
    goc_http_ctx_t     ctx;   /* MUST be last */
} goc_req_wrapper_t;

#define WRAPPER_FROM_CTX(p) \
    ((goc_req_wrapper_t*)((char*)(p) - offsetof(goc_req_wrapper_t, ctx)))

/* Main server object. */
struct goc_server {
    h2o_globalconf_t config;
    h2o_context_t    h2o_ctx;
    h2o_accept_ctx_t accept_ctx;
    uv_tcp_t*        tcp;        /* plain-malloc'd; NULL before listen */
    int              ctx_ready;

    goc_route_t*     routes;
    size_t           n_routes;
    size_t           cap_routes;

    goc_pool*        pool;
    goc_array*       middleware;
};

/* =========================================================================
 * Forward declaration of on_req (defined after route helpers)
 * ====================================================================== */
static int goc_server_on_req(h2o_handler_t* self, h2o_req_t* req);

/* =========================================================================
 * 1. Server lifecycle
 * ====================================================================== */

goc_server_opts_t* goc_server_opts(void)
{
    goc_server_opts_t* o =
        (goc_server_opts_t*)goc_malloc(sizeof(goc_server_opts_t));
    memset(o, 0, sizeof(*o));
    return o;
}

goc_server_t* goc_server_make(const goc_server_opts_t* opts)
{
    goc_server_t* srv =
        (goc_server_t*)goc_malloc(sizeof(goc_server_t));
    memset(srv, 0, sizeof(*srv));

    h2o_config_init(&srv->config);

    h2o_hostconf_t* hconf = h2o_config_register_host(
        &srv->config,
        h2o_iovec_init(H2O_STRLIT("default")),
        65535);
    h2o_pathconf_t* pconf = h2o_config_register_path(hconf, "/", 0);

    goc_h2o_handler_t* h = (goc_h2o_handler_t*)h2o_create_handler(
        pconf, sizeof(goc_h2o_handler_t));
    h->super.on_req = goc_server_on_req;
    h->srv          = srv;

    srv->pool       = opts && opts->pool ? opts->pool : NULL;
    srv->middleware = opts ? opts->middleware : NULL;

    srv->cap_routes = 8;
    srv->routes     =
        (goc_route_t*)goc_malloc(srv->cap_routes * sizeof(goc_route_t));

    return srv;
}

/* ---- listen dispatch (executes on the event loop thread) --------------- */

typedef struct {
    uv_async_t    async;  /* MUST be first */
    goc_server_t* srv;
    const char*   host;
    int           port;
    int           result;
    goc_sync_t    sync;
} goc_listen_dispatch_t;

static void on_accept(uv_stream_t* listener, int status); /* forward */

static void listen_dispatch_done(uv_handle_t* h)
{
    goc_listen_dispatch_t* d = (goc_listen_dispatch_t*)h;
    gc_handle_unregister(d);
    goc_sync_post(&d->sync);
}

static void on_listen_dispatch(uv_async_t* h)
{
    goc_listen_dispatch_t* d = (goc_listen_dispatch_t*)h;
    goc_server_t*          s = d->srv;

    if (!s->ctx_ready) {
        h2o_context_init(&s->h2o_ctx, g_loop, &s->config);
        s->ctx_ready = 1;
        /* Pin the server object as a GC root for the duration of the listen
         * lifetime.  H2O registers internal handles (timers, multithread
         * receivers) on the loop whose close callbacks reference &s->h2o_ctx.
         * Boehm GC cannot see those plain-malloc libuv handles as roots, so we
         * must keep srv alive explicitly until h2o_context_dispose completes. */
        gc_handle_register(s);
    }
    s->accept_ctx.ctx   = &s->h2o_ctx;
    s->accept_ctx.hosts = s->config.hosts;

    s->tcp = (uv_tcp_t*)malloc(sizeof(uv_tcp_t));
    if (!s->tcp) { d->result = UV_ENOMEM; goto done; }
    uv_tcp_init(g_loop, s->tcp);
    s->tcp->data = s;

    {
        struct sockaddr_storage addr;
        int r = uv_ip4_addr(d->host, d->port, (struct sockaddr_in*)&addr);
        if (r < 0)
            r = uv_ip6_addr(d->host, d->port, (struct sockaddr_in6*)&addr);
        if (r < 0) { d->result = r; goto done; }

        r = uv_tcp_bind(s->tcp, (const struct sockaddr*)&addr, 0);
        if (r < 0) { d->result = r; goto done; }

        r = uv_listen((uv_stream_t*)s->tcp, 128, on_accept);
        if (r < 0) { d->result = r; goto done; }
    }
    d->result = 0;

done:
    uv_close((uv_handle_t*)h, listen_dispatch_done);
}

int goc_server_listen(goc_server_t* srv, const char* host, int port)
{
    goc_listen_dispatch_t* d =
        (goc_listen_dispatch_t*)goc_malloc(sizeof(goc_listen_dispatch_t));
    d->srv    = srv;
    d->host   = host;
    d->port   = port;
    d->result = 0;
    goc_sync_init(&d->sync);

    gc_handle_register(d);
    int rc = uv_async_init(g_loop, &d->async, on_listen_dispatch);
    if (rc < 0) {
        gc_handle_unregister(d);
        goc_sync_destroy(&d->sync);
        return rc;
    }
    uv_async_send(&d->async);

    goc_sync_wait(&d->sync);
    goc_sync_destroy(&d->sync);
    return d->result;
}

/* ---- close dispatch ---------------------------------------------------- */

typedef struct {
    uv_async_t    async;  /* MUST be first */
    goc_server_t* srv;
    goc_chan*     ch;
} goc_srv_close_dispatch_t;

static void srv_tcp_closed(uv_handle_t* h)
{
    free(h);
}

/* drain timer — fires after H2O's close callbacks have processed */
typedef struct {
    uv_timer_t    timer; /* MUST be first */
    goc_server_t* srv;
    goc_chan*     ch;
} goc_close_drain_t;

static void on_close_drain_free(uv_handle_t* h)
{
    goc_close_drain_t* d = (goc_close_drain_t*)h;
    gc_handle_unregister(d->srv); /* release GC pin acquired in on_listen_dispatch */
    free(d);
}

static void on_close_drain_timer(uv_timer_t* h)
{
    goc_close_drain_t* d = (goc_close_drain_t*)h;
    goc_put_cb(d->ch, goc_box_int(0), close_on_put, d->ch);
    uv_close((uv_handle_t*)h, on_close_drain_free);
}

static void on_srv_close_dispatch(uv_async_t* h)
{
    goc_srv_close_dispatch_t* d = (goc_srv_close_dispatch_t*)h;
    goc_server_t* srv = d->srv;

    if (srv->tcp) {
        uv_close((uv_handle_t*)srv->tcp, srv_tcp_closed);
        srv->tcp = NULL;
    }
    if (srv->ctx_ready) {
        h2o_context_dispose(&srv->h2o_ctx);
        srv->ctx_ready = 0;
    }

    /* Deliver "done" via a 0-timeout timer so H2O's queued close callbacks
     * can fire in this same loop iteration before we release the GC pin. */
    goc_close_drain_t* drain =
        (goc_close_drain_t*)malloc(sizeof(goc_close_drain_t));
    drain->srv = srv;
    drain->ch  = d->ch;
    uv_timer_init(g_loop, &drain->timer);
    uv_timer_start(&drain->timer, on_close_drain_timer, 0, 0);

    uv_close((uv_handle_t*)h, srv_handle_unregister);
}

goc_chan* goc_server_close(goc_server_t* srv)
{
    goc_chan*                 ch = goc_chan_make(1);
    goc_srv_close_dispatch_t* d  =
        (goc_srv_close_dispatch_t*)goc_malloc(sizeof(goc_srv_close_dispatch_t));
    d->srv = srv;
    d->ch  = ch;

    gc_handle_register(d);
    int rc = uv_async_init(g_loop, &d->async, on_srv_close_dispatch);
    if (rc < 0) {
        gc_handle_unregister(d);
        goc_put_cb(ch, goc_box_int(0), close_on_put, ch);
        return ch;
    }
    uv_async_send(&d->async);
    return ch;
}

/* =========================================================================
 * 2. Routing
 * ====================================================================== */

void goc_server_route(goc_server_t* srv, const char* method,
                      const char* pattern, goc_http_handler_t handler)
{
    if (srv->n_routes >= srv->cap_routes) {
        size_t       new_cap = srv->cap_routes * 2;
        goc_route_t* nr      =
            (goc_route_t*)goc_malloc(new_cap * sizeof(goc_route_t));
        memcpy(nr, srv->routes, srv->n_routes * sizeof(goc_route_t));
        srv->routes     = nr;
        srv->cap_routes = new_cap;
    }
    goc_route_t* r = &srv->routes[srv->n_routes++];
    r->method  = method;
    r->pattern = pattern;
    r->handler = handler;
}

/* =========================================================================
 * Accept callback (event loop thread)
 * ====================================================================== */

static void on_conn_close(uv_handle_t* h)
{
    free(h);
}

static void on_accept(uv_stream_t* listener, int status)
{
    if (status < 0) return;
    goc_server_t* srv = (goc_server_t*)listener->data;

    uv_tcp_t* conn = (uv_tcp_t*)malloc(sizeof(uv_tcp_t));
    if (!conn) return;
    uv_tcp_init(g_loop, conn);

    if (uv_accept(listener, (uv_stream_t*)conn) == 0) {
        h2o_socket_t* sock =
            h2o_uv_socket_create((uv_stream_t*)conn, on_conn_close);
        h2o_accept(&srv->accept_ctx, sock);
    } else {
        uv_close((uv_handle_t*)conn, on_conn_close);
    }
}

/* =========================================================================
 * on_req — H2O event loop thread callback
 * ====================================================================== */

static void fiber_request_handler(void* arg); /* forward */

static int route_match(const char* method, const char* path,
                        const char* rmeth, const char* rpat)
{
    if (strcmp(rmeth, "*") != 0 && strcmp(rmeth, method) != 0)
        return 0;

    if (strcmp(rpat, "/*") == 0)
        return 1;

    size_t plen = strlen(rpat);
    if (plen > 0 && rpat[plen - 1] == '*')
        return strncmp(path, rpat, plen - 1) == 0;

    if (strcmp(path, rpat) == 0)
        return 1;

    return strncmp(path, rpat, plen) == 0 &&
           (path[plen] == '/' || path[plen] == '\0');
}

static int goc_server_on_req(h2o_handler_t* self, h2o_req_t* req)
{
    goc_h2o_handler_t* h   = (goc_h2o_handler_t*)self;
    goc_server_t*      srv = h->srv;

    /* Null-terminated method. */
    char* method = (char*)goc_malloc(req->method.len + 1);
    memcpy(method, req->method.base, req->method.len);
    method[req->method.len] = '\0';

    /* Split path from query. */
    size_t path_len = (req->query_at == SIZE_MAX) ? req->path.len : req->query_at;
    char*  path     = (char*)goc_malloc(path_len + 1);
    memcpy(path, req->path.base, path_len);
    path[path_len] = '\0';

    const char* query = "";
    if (req->query_at != SIZE_MAX) {
        size_t q_len = req->path.len - req->query_at - 1;
        char*  q     = (char*)goc_malloc(q_len + 1);
        memcpy(q, req->path.base + req->query_at + 1, q_len);
        q[q_len] = '\0';
        query    = q;
    }

    /* Copy headers. */
    goc_array* headers = goc_array_make((size_t)req->headers.size);
    for (size_t i = 0; i < req->headers.size; i++) {
        h2o_header_t* src = &req->headers.entries[i];

        char* name  = (char*)goc_malloc(src->name->len + 1);
        memcpy(name, src->name->base, src->name->len);
        name[src->name->len] = '\0';

        char* value = (char*)goc_malloc(src->value.len + 1);
        memcpy(value, src->value.base, src->value.len);
        value[src->value.len] = '\0';

        goc_http_header_t* hdr =
            (goc_http_header_t*)goc_malloc(sizeof(goc_http_header_t));
        hdr->name  = name;
        hdr->value = value;
        goc_array_push(headers, hdr);
    }

    /* Copy body. */
    goc_array* body = goc_array_make(req->entity.len);
    for (size_t i = 0; i < req->entity.len; i++)
        goc_array_push(body, goc_box_int((unsigned char)req->entity.base[i]));

    /* Route lookup. */
    goc_http_handler_t handler = NULL;
    for (size_t i = 0; i < srv->n_routes; i++) {
        if (route_match(method, path,
                         srv->routes[i].method,
                         srv->routes[i].pattern)) {
            handler = srv->routes[i].handler;
            break;
        }
    }

    if (!handler) {
        req->res.status = 404;
        req->res.reason = "Not Found";
        h2o_send_inline(req, H2O_STRLIT("Not Found\n"));
        return 0;
    }

    goc_req_wrapper_t* w =
        (goc_req_wrapper_t*)goc_malloc(sizeof(goc_req_wrapper_t));
    w->h2o_req       = req;
    w->srv           = srv;
    w->handler       = handler;
    w->ctx.method    = method;
    w->ctx.path      = path;
    w->ctx.query     = query;
    w->ctx.headers   = headers;
    w->ctx.body      = body;
    w->ctx.user_data = NULL;

    goc_pool* pool = srv->pool ? srv->pool : goc_default_pool();
    goc_go_on(pool, fiber_request_handler, w);
    return 0;
}

/* =========================================================================
 * Fiber: runs middleware chain then the route handler
 * ====================================================================== */

static void fiber_request_handler(void* arg)
{
    goc_req_wrapper_t* w   = (goc_req_wrapper_t*)arg;
    goc_http_ctx_t*    ctx = &w->ctx;
    goc_server_t*      srv = w->srv;

    if (srv->middleware) {
        size_t n = goc_array_len(srv->middleware);
        for (size_t i = 0; i < n; i++) {
            goc_http_middleware_t mw =
                (goc_http_middleware_t)(uintptr_t)goc_array_get(srv->middleware, i);
            if (mw(ctx) != GOC_SERVER_OK) {
                goc_take(goc_server_respond_error(ctx, 500,
                                                   "Internal Server Error"));
                return;
            }
        }
    }
    w->handler(ctx);
}

/* =========================================================================
 * 3. Request context helpers
 * ====================================================================== */

const char* goc_server_header(goc_http_ctx_t* ctx, const char* name)
{
    if (!ctx || !name || !ctx->headers) return NULL;
    size_t n = goc_array_len(ctx->headers);
    for (size_t i = 0; i < n; i++) {
        goc_http_header_t* h =
            (goc_http_header_t*)goc_array_get(ctx->headers, i);
        if (h && strcasecmp(h->name, name) == 0)
            return h->value;
    }
    return NULL;
}

const char* goc_server_body_str(goc_http_ctx_t* ctx)
{
    if (!ctx || !ctx->body || goc_array_len(ctx->body) == 0)
        return "";
    size_t len = goc_array_len(ctx->body);
    char*  buf = (char*)goc_malloc(len + 1);
    for (size_t i = 0; i < len; i++)
        buf[i] = (char)goc_unbox_int(goc_array_get(ctx->body, i));
    buf[len] = '\0';
    return buf;
}

/* =========================================================================
 * 4. Sending responses
 * ====================================================================== */

typedef struct {
    uv_async_t  async;         /* MUST be first */
    h2o_req_t*  req;
    int         status;
    const char* content_type;
    const char* body;          /* GC-managed copy */
    size_t      body_len;
    goc_chan*   ch;
} goc_respond_dispatch_t;

static const char* status_reason(int status)
{
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "";
    }
}

static void on_respond_dispatch(uv_async_t* h)
{
    goc_respond_dispatch_t* d = (goc_respond_dispatch_t*)h;

    d->req->res.status = d->status;
    d->req->res.reason = status_reason(d->status);

    h2o_add_header(&d->req->pool, &d->req->res.headers,
                   H2O_TOKEN_CONTENT_TYPE, NULL,
                   d->content_type, strlen(d->content_type));

    h2o_send_inline(d->req, d->body, d->body_len);

    goc_put_cb(d->ch, goc_box_int(0), close_on_put, d->ch);
    uv_close((uv_handle_t*)h, srv_handle_unregister);
}

goc_chan* goc_server_respond_buf(goc_http_ctx_t* ctx, int status,
                                  const char* content_type,
                                  const char* buf, size_t len)
{
    if (!content_type) content_type = "text/plain";

    goc_req_wrapper_t* w = WRAPPER_FROM_CTX(ctx);

    char* body_copy = (char*)goc_malloc(len + 1);
    memcpy(body_copy, buf, len);
    body_copy[len] = '\0';

    goc_chan*               ch = goc_chan_make(1);
    goc_respond_dispatch_t* d  =
        (goc_respond_dispatch_t*)goc_malloc(sizeof(goc_respond_dispatch_t));
    d->req          = w->h2o_req;
    d->status       = status;
    d->content_type = content_type;
    d->body         = body_copy;
    d->body_len     = len;
    d->ch           = ch;

    gc_handle_register(d);
    int rc = uv_async_init(g_loop, &d->async, on_respond_dispatch);
    if (rc < 0) {
        gc_handle_unregister(d);
        goc_put_cb(ch, goc_box_int(0), close_on_put, ch);
        return ch;
    }
    uv_async_send(&d->async);
    return ch;
}

goc_chan* goc_server_respond(goc_http_ctx_t* ctx, int status,
                              const char* content_type, const char* body)
{
    return goc_server_respond_buf(ctx, status, content_type,
                                   body ? body : "",
                                   body ? strlen(body) : 0);
}

goc_chan* goc_server_respond_error(goc_http_ctx_t* ctx, int status,
                                    const char* message)
{
    return goc_server_respond(ctx, status, "text/plain",
                               message ? message : "");
}

/* =========================================================================
 * 6. HTTP client — libuv TCP + HTTP/1.1
 *
 * All transport structs (uv_tcp_t, uv_connect_t, uv_write_t,
 * goc_http_client_t, parsing buffers) are plain-malloc'd so that libuv can
 * own them safely.  Only the final goc_http_response_t and its string/array
 * fields are goc_malloc'd so the GC keeps them alive for the caller.
 * ====================================================================== */

goc_http_request_opts_t* goc_http_request_opts(void)
{
    goc_http_request_opts_t* o =
        (goc_http_request_opts_t*)goc_malloc(sizeof(goc_http_request_opts_t));
    memset(o, 0, sizeof(*o));
    return o;
}

/* ----------------------------------------------------------------------- *
 * HTTP/1.1 client state machine
 * ----------------------------------------------------------------------- */

typedef struct goc_http_client {
    uv_tcp_t*    tcp;      /* plain-malloc'd */
    uv_write_t   write_req;

    char*        req_buf;  /* plain-malloc'd request string */
    size_t       req_len;

    /* Response accumulation (plain-malloc'd, grown as data arrives). */
    char*        resp_buf;
    size_t       resp_len;
    size_t       resp_cap;

    /* Parsed response (goc_malloc'd — handed to caller). */
    goc_http_response_t* response;
    size_t       body_start;
    ssize_t      content_length; /* -1 = read until EOF */

    goc_chan*    ch;  /* goc_malloc'd channel to deliver response */
    int          done; /* guards against double-deliver */
} goc_http_client_t;

/* Deliver the response on c->ch and begin TCP cleanup. */
static void http_client_deliver(goc_http_client_t* c)
{
    if (c->done) return;
    c->done = 1;

    if (!c->response) {
        c->response =
            (goc_http_response_t*)goc_malloc(sizeof(goc_http_response_t));
        memset(c->response, 0, sizeof(*c->response));
        c->response->headers = goc_array_make(0);
        c->response->body    = "";
    }

    /* Attach body. */
    size_t blen = (c->resp_len > c->body_start)
                  ? c->resp_len - c->body_start : 0;
    if (c->content_length >= 0 && (size_t)c->content_length < blen)
        blen = (size_t)c->content_length;

    char* body = (char*)goc_malloc(blen + 1);
    if (blen && c->resp_buf)
        memcpy(body, c->resp_buf + c->body_start, blen);
    body[blen] = '\0';
    c->response->body     = body;
    c->response->body_len = blen;

    goc_put_cb(c->ch, c->response, close_on_put, c->ch);
}

static void http_client_tcp_closed(uv_handle_t* h)
{
    goc_http_client_t* c = (goc_http_client_t*)h->data;
    if (c->resp_buf) { free(c->resp_buf); c->resp_buf = NULL; }
    if (c->req_buf)  { free(c->req_buf);  c->req_buf  = NULL; }
    free(h); /* free the uv_tcp_t */
    free(c);
}

static void http_client_close(goc_http_client_t* c)
{
    http_client_deliver(c);
    if (c->tcp) {
        uv_tcp_t* tcp = c->tcp;
        c->tcp = NULL;
        tcp->data = c;
        uv_close((uv_handle_t*)tcp, http_client_tcp_closed);
    }
}

/* Parse status line + headers from resp_buf; returns 1 when complete. */
static int parse_response_head(goc_http_client_t* c)
{
    char*  buf = c->resp_buf;
    size_t len = c->resp_len;

    /* Find \r\n\r\n. */
    char* eoh = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i]=='\r' && buf[i+1]=='\n' && buf[i+2]=='\r' && buf[i+3]=='\n') {
            eoh = buf + i + 4;
            break;
        }
    }
    if (!eoh) return 0;

    c->body_start = (size_t)(eoh - buf);

    /* Allocate response. */
    goc_http_response_t* resp =
        (goc_http_response_t*)goc_malloc(sizeof(goc_http_response_t));
    memset(resp, 0, sizeof(*resp));
    resp->headers = goc_array_make(8);
    c->response   = resp;
    c->content_length = -1;

    /* Status line. */
    int major = 0, minor = 0, status = 0;
    sscanf(buf, "HTTP/%d.%d %d", &major, &minor, &status);
    resp->status = status;

    /* Headers. */
    char* line = memchr(buf, '\n', len);
    if (!line) return 1;
    line++;

    while (line < eoh - 1) {
        char* nl = memchr(line, '\n', (size_t)(eoh - line));
        if (!nl) break;

        char* colon = memchr(line, ':', (size_t)(nl - line));
        if (!colon) { line = nl + 1; continue; }

        size_t nlen = (size_t)(colon - line);
        while (nlen > 0 && isspace((unsigned char)line[nlen - 1])) nlen--;

        char* vstart = colon + 1;
        while (vstart < nl && isspace((unsigned char)*vstart)) vstart++;
        size_t vlen = (size_t)(nl - vstart);
        while (vlen > 0 && isspace((unsigned char)vstart[vlen - 1])) vlen--;

        char* name  = (char*)goc_malloc(nlen + 1);
        memcpy(name, line, nlen); name[nlen] = '\0';
        char* value = (char*)goc_malloc(vlen + 1);
        memcpy(value, vstart, vlen); value[vlen] = '\0';

        if (strcasecmp(name, "content-length") == 0)
            c->content_length = (ssize_t)atol(value);

        goc_http_header_t* hdr =
            (goc_http_header_t*)goc_malloc(sizeof(goc_http_header_t));
        hdr->name  = name;
        hdr->value = value;
        goc_array_push(resp->headers, hdr);

        line = nl + 1;
    }
    return 1;
}

static void http_alloc_cb(uv_handle_t* h, size_t suggested, uv_buf_t* buf)
{
    (void)h;
    buf->base = (char*)malloc(suggested);
    buf->len  = buf->base ? suggested : 0;
}

static void http_on_read(uv_stream_t* stream, ssize_t nread,
                          const uv_buf_t* buf)
{
    goc_http_client_t* c = (goc_http_client_t*)stream->data;

    if (nread < 0) {
        if (buf->base) free(buf->base);
        http_client_close(c);
        return;
    }

    /* Grow buffer. */
    size_t needed = c->resp_len + (size_t)nread + 1;
    if (needed > c->resp_cap) {
        size_t nc = c->resp_cap ? c->resp_cap * 2 : 4096;
        while (nc < needed) nc *= 2;
        char* nb = (char*)malloc(nc);
        if (c->resp_len) memcpy(nb, c->resp_buf, c->resp_len);
        if (c->resp_buf) free(c->resp_buf);
        c->resp_buf = nb;
        c->resp_cap = nc;
    }
    memcpy(c->resp_buf + c->resp_len, buf->base, (size_t)nread);
    c->resp_len += (size_t)nread;
    c->resp_buf[c->resp_len] = '\0';
    free(buf->base);

    /* Parse headers if not done. */
    if (!c->response) {
        if (!parse_response_head(c)) return; /* need more data */
    }

    /* Check if body is complete (Content-Length). */
    if (c->content_length >= 0) {
        size_t bso_far = c->resp_len > c->body_start
                         ? c->resp_len - c->body_start : 0;
        if (bso_far >= (size_t)c->content_length) {
            uv_read_stop(stream);
            http_client_close(c);
        }
    }
}

static void http_on_write(uv_write_t* req, int status)
{
    goc_http_client_t* c = (goc_http_client_t*)req->data;
    if (status < 0) {
        http_client_close(c);
        return;
    }
    uv_read_start((uv_stream_t*)c->tcp, http_alloc_cb, http_on_read);
}

static void http_on_connect(uv_connect_t* req, int status)
{
    goc_http_client_t* c = (goc_http_client_t*)req->data;
    free(req);

    if (status < 0) {
        http_client_close(c);
        return;
    }

    uv_buf_t wb = uv_buf_init(c->req_buf, (unsigned int)c->req_len);
    c->write_req.data = c;
    uv_write(&c->write_req, (uv_stream_t*)c->tcp, &wb, 1, http_on_write);
}

/* Build raw HTTP/1.1 request. */
static char* build_request(const char* method, const char* host,
                             const char* pq,   /* path[?query] */
                             const char* ct,   /* content-type, may be NULL */
                             const char* body, size_t body_len,
                             goc_array*  extra_hdrs, /* may be NULL */
                             size_t* out_len)
{
    size_t cap = strlen(method) + strlen(pq) + strlen(host) + 256;
    if (ct) cap += strlen(ct) + 40;
    cap += body_len;
    if (extra_hdrs) {
        size_t n = goc_array_len(extra_hdrs);
        for (size_t i = 0; i < n; i++) {
            goc_http_header_t* h =
                (goc_http_header_t*)goc_array_get(extra_hdrs, i);
            if (h) cap += strlen(h->name) + strlen(h->value) + 4;
        }
    }

    char* buf = (char*)malloc(cap);
    size_t pos = 0;

#define APPEND(...)  pos += (size_t)snprintf(buf + pos, cap - pos, __VA_ARGS__)

    APPEND("%s %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n",
           method, pq, host);

    if (ct && body_len > 0)
        APPEND("Content-Type: %s\r\nContent-Length: %zu\r\n", ct, body_len);

    if (extra_hdrs) {
        size_t n = goc_array_len(extra_hdrs);
        for (size_t i = 0; i < n; i++) {
            goc_http_header_t* h =
                (goc_http_header_t*)goc_array_get(extra_hdrs, i);
            if (h) APPEND("%s: %s\r\n", h->name, h->value);
        }
    }

    APPEND("\r\n");
    if (body && body_len > 0) {
        memcpy(buf + pos, body, body_len);
        pos += body_len;
    }
#undef APPEND

    *out_len = pos;
    return buf;
}

/* ----------------------------------------------------------------------- *
 * Dispatch struct marshals the request to the loop thread
 * ----------------------------------------------------------------------- */

typedef struct {
    uv_async_t async;  /* MUST be first */
    /* Inputs (goc_malloc'd strings so GC keeps them alive during dispatch). */
    char*      method;
    char*      host;
    uint16_t   port;
    char*      path_and_query;
    char*      content_type;
    char*      body;
    size_t     body_len;
    goc_array* extra_headers;
    goc_chan*  ch;
} goc_http_req_dispatch_t;

/* getaddrinfo context; plain-malloc'd so uv_getaddrinfo can own it. */
typedef struct {
    uv_getaddrinfo_t  req;    /* MUST be first */
    char*             method;
    char*             host;
    uint16_t          port;
    char*             pq;
    char*             ct;
    char*             body;
    size_t            body_len;
    goc_array*        extra_headers;
    goc_chan*         ch;
} goc_getaddrinfo_ctx_t;

static void on_getaddrinfo(uv_getaddrinfo_t* req, int status,
                            struct addrinfo* res)
{
    goc_getaddrinfo_ctx_t* g = (goc_getaddrinfo_ctx_t*)req;

    if (status < 0 || !res) {
        if (res) uv_freeaddrinfo(res);
        goc_http_response_t* r =
            (goc_http_response_t*)goc_malloc(sizeof(goc_http_response_t));
        memset(r, 0, sizeof(*r));
        r->headers = goc_array_make(0);
        r->body    = "";
        goc_put_cb(g->ch, r, close_on_put, g->ch);
        free(g);
        return;
    }

    /* Build HTTP client state. */
    goc_http_client_t* c =
        (goc_http_client_t*)malloc(sizeof(goc_http_client_t));
    memset(c, 0, sizeof(*c));
    c->ch             = g->ch;
    c->content_length = -1;
    c->req_buf        = build_request(g->method, g->host, g->pq,
                                       g->ct, g->body, g->body_len,
                                       g->extra_headers, &c->req_len);

    c->tcp = (uv_tcp_t*)malloc(sizeof(uv_tcp_t));
    uv_tcp_init(g_loop, c->tcp);
    c->tcp->data = c;

    /* Copy address and set port. */
    struct sockaddr_storage addr;
    memcpy(&addr, res->ai_addr, res->ai_addrlen);
    if (addr.ss_family == AF_INET)
        ((struct sockaddr_in*)&addr)->sin_port = htons(g->port);
    else
        ((struct sockaddr_in6*)&addr)->sin6_port = htons(g->port);
    uv_freeaddrinfo(res);

    uv_connect_t* creq = (uv_connect_t*)malloc(sizeof(uv_connect_t));
    creq->data = c;
    int r = uv_tcp_connect(creq, c->tcp,
                            (const struct sockaddr*)&addr,
                            http_on_connect);
    if (r < 0) {
        free(creq);
        http_client_close(c);
    }

    free(g);
}

static void on_http_req_dispatch(uv_async_t* h)
{
    goc_http_req_dispatch_t* d = (goc_http_req_dispatch_t*)h;

    goc_getaddrinfo_ctx_t* g =
        (goc_getaddrinfo_ctx_t*)malloc(sizeof(goc_getaddrinfo_ctx_t));
    memset(g, 0, sizeof(*g));
    g->method       = d->method;
    g->host         = d->host;
    g->port         = d->port;
    g->pq           = d->path_and_query;
    g->ct           = d->content_type;
    g->body         = d->body;
    g->body_len     = d->body_len;
    g->extra_headers = d->extra_headers;
    g->ch           = d->ch;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int r = uv_getaddrinfo(g_loop, &g->req, on_getaddrinfo,
                            d->host, NULL, &hints);
    if (r < 0) {
        free(g);
        goc_http_response_t* resp =
            (goc_http_response_t*)goc_malloc(sizeof(goc_http_response_t));
        memset(resp, 0, sizeof(*resp));
        resp->headers = goc_array_make(0);
        resp->body    = "";
        goc_put_cb(d->ch, resp, close_on_put, d->ch);
    }

    uv_close((uv_handle_t*)h, srv_handle_unregister);
}

goc_chan* goc_http_request(const char* method, const char* url,
                            const char* content_type,
                            const char* body, size_t body_len,
                            goc_http_request_opts_t* opts)
{
    goc_chan* ch = goc_chan_make(1);

    h2o_url_t parsed;
    if (h2o_url_parse(url, SIZE_MAX, &parsed) != 0) {
        goc_http_response_t* r =
            (goc_http_response_t*)goc_malloc(sizeof(goc_http_response_t));
        memset(r, 0, sizeof(*r));
        r->headers = goc_array_make(0);
        r->body    = "";
        goc_put_cb(ch, r, close_on_put, ch);
        return ch;
    }

    char* host = (char*)goc_malloc(parsed.host.len + 1);
    memcpy(host, parsed.host.base, parsed.host.len);
    host[parsed.host.len] = '\0';

    const char* path_src = parsed.path.len > 0 ? parsed.path.base : "/";
    size_t      path_src_len = parsed.path.len > 0 ? parsed.path.len : 1;
    char* pq = (char*)goc_malloc(path_src_len + 1);
    memcpy(pq, path_src, path_src_len);
    pq[path_src_len] = '\0';

    uint16_t port = h2o_url_get_port(&parsed);

    char* body_copy = NULL;
    if (body && body_len > 0) {
        body_copy = (char*)goc_malloc(body_len + 1);
        memcpy(body_copy, body, body_len);
        body_copy[body_len] = '\0';
    }

    goc_http_req_dispatch_t* d =
        (goc_http_req_dispatch_t*)goc_malloc(sizeof(goc_http_req_dispatch_t));
    d->method        = (char*)method;  /* caller's lifetime >= dispatch */
    d->host          = host;
    d->port          = port;
    d->path_and_query = pq;
    d->content_type  = content_type ? (char*)content_type : NULL;
    d->body          = body_copy;
    d->body_len      = body_len;
    d->extra_headers = opts ? opts->headers : NULL;
    d->ch            = ch;

    gc_handle_register(d);
    int rc = uv_async_init(g_loop, &d->async, on_http_req_dispatch);
    if (rc < 0) {
        gc_handle_unregister(d);
        goc_http_response_t* r =
            (goc_http_response_t*)goc_malloc(sizeof(goc_http_response_t));
        memset(r, 0, sizeof(*r));
        r->headers = goc_array_make(0);
        r->body    = "";
        goc_put_cb(ch, r, close_on_put, ch);
        return ch;
    }
    uv_async_send(&d->async);

    /* Honour optional timeout (fiber context only). */
    if (opts && opts->timeout_ms > 0 && goc_in_fiber()) {
        goc_chan*    tch = goc_timeout(opts->timeout_ms);
        goc_alt_op  ops[2] = {
            { ch,  GOC_ALT_TAKE, NULL },
            { tch, GOC_ALT_TAKE, NULL },
        };
        goc_alts_result* res = goc_alts(ops, 2);
        if (res->ch == tch) {
            /* Timeout: close the original channel so it is removed from
             * live_channels before goc_shutdown destroys its lock.  The HTTP
             * client will fire close_on_put harmlessly (goc_close is idempotent). */
            goc_close(ch);
            goc_chan* toch = goc_chan_make(1);
            goc_http_response_t* r =
                (goc_http_response_t*)goc_malloc(sizeof(goc_http_response_t));
            memset(r, 0, sizeof(*r));
            r->headers = goc_array_make(0);
            r->body    = "";
            goc_put_cb(toch, r, close_on_put, toch);
            return toch;
        }
        /* Response arrived before timeout — re-wrap on a fresh channel. */
        goc_chan* ch2 = goc_chan_make(1);
        goc_put_cb(ch2, res->value.val, close_on_put, ch2);
        return ch2;
    }

    return ch;
}

/* REST convenience wrappers. */

goc_chan* goc_http_get(const char* url, goc_http_request_opts_t* opts)
{
    return goc_http_request("GET", url, NULL, NULL, 0, opts);
}

goc_chan* goc_http_post(const char* url, const char* content_type,
                         const char* body, goc_http_request_opts_t* opts)
{
    return goc_http_request("POST", url, content_type,
                             body, body ? strlen(body) : 0, opts);
}

goc_chan* goc_http_post_buf(const char* url, const char* content_type,
                             const char* buf, size_t len,
                             goc_http_request_opts_t* opts)
{
    return goc_http_request("POST", url, content_type, buf, len, opts);
}

goc_chan* goc_http_put(const char* url, const char* content_type,
                        const char* body, goc_http_request_opts_t* opts)
{
    return goc_http_request("PUT", url, content_type,
                             body, body ? strlen(body) : 0, opts);
}

goc_chan* goc_http_patch(const char* url, const char* content_type,
                          const char* body, goc_http_request_opts_t* opts)
{
    return goc_http_request("PATCH", url, content_type,
                             body, body ? strlen(body) : 0, opts);
}

goc_chan* goc_http_delete(const char* url, goc_http_request_opts_t* opts)
{
    return goc_http_request("DELETE", url, NULL, NULL, 0, opts);
}
