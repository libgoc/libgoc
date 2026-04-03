/*
 * src/goc_http.c — HTTP server and client for libgoc
 *
 * Server: pure goc_io TCP + picohttpparser (MIT, zero new runtime dependencies).
 *   goc_http_server_listen initialises a GC-managed uv_tcp_t, binds it, and calls
 *   goc_io_tcp_server_make to obtain an accept channel.  An accept-loop fiber
 *   runs on the caller's pool, dispatching each accepted uv_tcp_t to a
 *   per-connection fiber.  The connection fiber reads a complete HTTP/1.1
 *   request via goc_io_read_start, parses it with phr_parse_request, matches
 *   a route, runs middleware, calls the handler, and writes the response via
 *   goc_io_write (which marshals uv_write to the event loop thread via a
 *   one-shot uv_async_t bridge).  No extra threads or mutexes are required
 *   beyond what goc_io already uses.
 *
 * HTTP client: libuv TCP + hand-written HTTP/1.1 (unchanged).
 *   URL parsing uses a small self-contained parser.
 *   DNS uses uv_getaddrinfo.
 *   The request is marshalled to the loop thread via uv_async_t.
 *   Timeout is honoured via goc_timeout + goc_alts (fiber context only).
 */

#if !defined(_WIN32) && !defined(__APPLE__)
#  define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  define strncasecmp _strnicmp
#  define strcasecmp  _stricmp
#else
#  include <strings.h>
#endif
#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#include "../vendor/picohttpparser/picohttpparser.h"
#include "../include/goc_http.h"
#include "../include/goc_io.h"
#include "../include/goc_array.h"
#include "internal.h"

/* =========================================================================
 * Shared utilities
 * ====================================================================== */

static void close_on_put(goc_status_t ok, void* ud)
{
    (void)ok;
    goc_close((goc_chan*)ud);
}

/* Used by the HTTP client dispatch to close one-shot uv_async_t handles. */
static void srv_handle_unregister(uv_handle_t* h)
{
    gc_handle_unregister(h);
}

/* GC-managed strdup helper. */
static char* gc_strdup(const char* s)
{
    size_t n = strlen(s);
    char*  d = (char*)goc_malloc(n + 1);
    memcpy(d, s, n + 1);
    return d;
}

/* =========================================================================
 * Internal types
 * ====================================================================== */

typedef struct {
    const char*        method;
    const char*        pattern;
    goc_http_handler_t handler;
} goc_route_t;

/*
 * Per-request fiber container.
 * The public goc_http_ctx_t is embedded last so WRAPPER_FROM_CTX can recover
 * the full container from a ctx pointer using offsetof arithmetic.
 */
typedef struct {
    uv_tcp_t*          conn;   /* GC-registered per-connection handle */
    struct goc_http_server* srv;
    goc_http_handler_t handler;
    goc_http_ctx_t     ctx;    /* MUST be last */
} goc_req_wrapper_t;

#define WRAPPER_FROM_CTX(p) \
    ((goc_req_wrapper_t*)((char*)(p) - offsetof(goc_req_wrapper_t, ctx)))

/* Main server object (GC-allocated). */
struct goc_http_server {
    uv_tcp_t*    tcp;          /* GC-allocated TCP listen handle */
    goc_chan*    accept_ch;    /* from goc_io_tcp_server_make */

    goc_route_t* routes;
    size_t       n_routes;
    size_t       cap_routes;

    goc_pool*    pool;
    goc_array*   middleware;
};

/* =========================================================================
 * Forward declarations
 * ====================================================================== */
static void accept_loop_fiber(void* arg);
static void handle_conn_fiber(void* arg);

/* =========================================================================
 * HTTP status helper
 * ====================================================================== */

static const char* http_status_str(int code)
{
    switch (code) {
        case 100: return "Continue";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
    }
}

/* =========================================================================
 * 1. Server lifecycle
 * ====================================================================== */

goc_http_server_opts_t* goc_http_server_opts(void)
{
    goc_http_server_opts_t* o =
        (goc_http_server_opts_t*)goc_malloc(sizeof(goc_http_server_opts_t));
    memset(o, 0, sizeof(*o));
    return o;
}

goc_http_server_t* goc_http_server_make(const goc_http_server_opts_t* opts)
{
    goc_http_server_t* srv =
        (goc_http_server_t*)goc_malloc(sizeof(goc_http_server_t));
    memset(srv, 0, sizeof(*srv));

    srv->pool       = opts && opts->pool ? opts->pool : NULL;
    srv->middleware = opts ? opts->middleware : NULL;

    srv->cap_routes = 8;
    srv->routes     =
        (goc_route_t*)goc_malloc(srv->cap_routes * sizeof(goc_route_t));

    return srv;
}

goc_chan* goc_http_server_listen(goc_http_server_t* srv, const char* host, int port)
{
    goc_chan* ready_ch = goc_chan_make(1);

    /* Resolve bind address. */
    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));
    int r = uv_ip4_addr(host, port, (struct sockaddr_in*)&addr);
    if (r < 0)
        r = uv_ip6_addr(host, port, (struct sockaddr_in6*)&addr);
    if (r < 0) {
        goc_put_cb(ready_ch, goc_box_int(r), close_on_put, ready_ch);
        return ready_ch;
    }

    /* Initialise the GC-managed TCP listen handle. */
    srv->tcp = (uv_tcp_t*)goc_malloc(sizeof(uv_tcp_t));
    r = goc_unbox_int(goc_take(goc_io_tcp_init(srv->tcp))->val);
    if (r < 0) {
        /* uv_tcp_init failed: no libuv state was committed, so no uv_close is
         * needed.  Null the pointer so the server struct is not left with a
         * stale reference to the unused allocation. */
        srv->tcp = NULL;
        goc_put_cb(ready_ch, goc_box_int(r), close_on_put, ready_ch);
        return ready_ch;
    }

    /* Bind. */
    r = goc_unbox_int(
            goc_take(goc_io_tcp_bind(srv->tcp,
                                     (const struct sockaddr*)&addr))->val);
    if (r < 0) {
        goc_io_handle_close((uv_handle_t*)srv->tcp, NULL);
        srv->tcp = NULL;
        goc_put_cb(ready_ch, goc_box_int(r), close_on_put, ready_ch);
        return ready_ch;
    }

    /* Start listening; ready_ch will receive goc_box_int(0) when uv_listen
     * has been called on the event-loop thread. */
    srv->accept_ch = goc_io_tcp_server_make(srv->tcp, 128, ready_ch);

    /* Pin the server struct in the GC while the accept loop fiber runs. */
    gc_handle_register(srv);

    /* Spawn the accept loop fiber. */
    goc_pool* pool = srv->pool ? srv->pool : goc_default_pool();
    goc_go_on(pool, accept_loop_fiber, srv);

    return ready_ch;
}

goc_chan* goc_http_server_close(goc_http_server_t* srv)
{
    goc_chan* ch = goc_chan_make(1);

    /* Close the accept channel so the accept loop fiber breaks. */
    if (srv->accept_ch) {
        goc_close(srv->accept_ch);
        srv->accept_ch = NULL;
    }

    /* Close the TCP listen handle (asynchronous; no callback needed). */
    if (srv->tcp) {
        goc_io_handle_close((uv_handle_t*)srv->tcp, NULL);
        srv->tcp = NULL;
    }

    /* Unpin the server from the GC root list. */
    gc_handle_unregister(srv);

    goc_put_cb(ch, goc_box_int(0), close_on_put, ch);
    return ch;
}

/* =========================================================================
 * 2. Routing
 * ====================================================================== */

void goc_http_server_route(goc_http_server_t* srv, const char* method,
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

/* =========================================================================
 * 3. Accept loop and per-connection fibers
 * ====================================================================== */

typedef struct {
    goc_http_server_t* srv;
    uv_tcp_t*     conn;
} conn_arg_t;

static void accept_loop_fiber(void* arg)
{
    goc_http_server_t* srv      = (goc_http_server_t*)arg;
    goc_chan*     accept_ch = srv->accept_ch; /* cache before any yield point */
    if (!accept_ch) return; /* server closed before fiber started */
    for (;;) {
        goc_val_t* v = goc_take(accept_ch);
        if (!v || v->ok != GOC_OK)
            break;  /* channel closed — server is shutting down */
        uv_tcp_t* conn = (uv_tcp_t*)v->val;
        conn_arg_t* a  = (conn_arg_t*)goc_malloc(sizeof(conn_arg_t));
        a->srv  = srv;
        a->conn = conn;
        goc_pool* pool = srv->pool ? srv->pool : goc_default_pool();
        goc_go_on(pool, handle_conn_fiber, a);
    }
}

/* Maximum raw request size before we abort the connection. */
#define GOC_SERVER_MAX_REQ_SIZE (8 * 1024 * 1024)
/* Maximum number of HTTP headers accepted per request. */
#define GOC_SERVER_MAX_HDRS     64



static void handle_conn_fiber(void* arg)
{
    conn_arg_t*   a    = (conn_arg_t*)arg;
    goc_http_server_t* srv  = a->srv;
    uv_tcp_t*     conn = a->conn;

    /* Accumulation buffer (plain-malloc; we control its entire lifetime). */
    size_t buf_cap = 4096;
    size_t buf_len = 0;
    char*  buf     = (char*)malloc(buf_cap);
    if (!buf) {
        goc_io_handle_close((uv_handle_t*)conn, NULL);
        return;
    }

    goc_chan* read_ch = goc_io_read_start((uv_stream_t*)conn);

    /* ---- Read until we have a complete HTTP request head ---- */
    const char*       method        = NULL;
    size_t            method_len    = 0;
    const char*       path          = NULL;
    size_t            path_len      = 0;
    int               minor_version = 0;
    struct phr_header headers[GOC_SERVER_MAX_HDRS];
    size_t            num_headers   = GOC_SERVER_MAX_HDRS;
    int               pret          = -2;

    while (pret == -2) {
        goc_val_t* v = goc_take(read_ch);
        if (!v || v->ok != GOC_OK)
            goto cleanup;
        goc_io_read_t* r = (goc_io_read_t*)v->val;
        if (r->nread < 0)
            goto cleanup;  /* EOF or read error before full head */

        size_t chunk = (size_t)r->nread;
        if (buf_len + chunk > GOC_SERVER_MAX_REQ_SIZE)
            goto stop_and_close;
        if (buf_len + chunk > buf_cap) {
            size_t nc = buf_cap;
            while (nc < buf_len + chunk) nc *= 2;
            char* nb = (char*)realloc(buf, nc);
            if (!nb) goto stop_and_close;
            buf = nb; buf_cap = nc;
        }
        memcpy(buf + buf_len, r->buf->base, chunk);
        buf_len += chunk;

        num_headers = GOC_SERVER_MAX_HDRS;
        pret = phr_parse_request(buf, buf_len,
                                  &method, &method_len,
                                  &path,   &path_len,
                                  &minor_version,
                                  headers, &num_headers, 0);
    }

    if (pret < 0) {
        /* HTTP parse error — send 400 Bad Request */
        static const char resp400[] =
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Type: text/plain\r\nContent-Length: 11\r\n"
            "Connection: close\r\n\r\nBad Request";
        uv_buf_t b = uv_buf_init((char*)resp400, sizeof(resp400) - 1);
        goc_take(goc_io_write((uv_stream_t*)conn, &b, 1));
        goto stop_and_close;
    }

    {
        /* ---- Find Content-Length and read remaining body bytes ---- */
        ssize_t content_length = 0;
        for (size_t i = 0; i < num_headers; i++) {
            if (headers[i].name_len == 14 &&
                strncasecmp(headers[i].name, "content-length", 14) == 0) {
                char tmp[32];
                size_t vl = headers[i].value_len < 31
                            ? headers[i].value_len : 31;
                memcpy(tmp, headers[i].value, vl);
                tmp[vl] = '\0';
                content_length = (ssize_t)atol(tmp);
                break;
            }
        }

        size_t head_consumed = (size_t)pret;
        size_t body_end = head_consumed +
                          (content_length > 0 ? (size_t)content_length : 0);

        while (buf_len < body_end) {
            goc_val_t* v = goc_take(read_ch);
            if (!v || v->ok != GOC_OK) break;
            goc_io_read_t* r = (goc_io_read_t*)v->val;
            if (r->nread < 0) break;

            size_t chunk = (size_t)r->nread;
            if (buf_len + chunk > GOC_SERVER_MAX_REQ_SIZE)
                goto stop_and_close;
            if (buf_len + chunk > buf_cap) {
                size_t nc = buf_cap;
                while (nc < buf_len + chunk) nc *= 2;
                char* nb = (char*)realloc(buf, nc);
                if (!nb) goto stop_and_close;
                buf = nb; buf_cap = nc;
            }
            memcpy(buf + buf_len, r->buf->base, chunk);
            buf_len += chunk;
        }

        /*
         * Stop reading and drain the channel.  This ensures stream->data is
         * NULL before goc_io_handle_close overwrites it with the close context.
         */
        goc_io_read_stop((uv_stream_t*)conn);
        for (;;) {
            goc_val_t* dv = goc_take(read_ch);
            if (!dv || dv->ok != GOC_OK) break;
        }

        /* ---- Build GC-managed method/path/query strings ---- */
        char* method_str = (char*)goc_malloc(method_len + 1);
        memcpy(method_str, method, method_len);
        method_str[method_len] = '\0';

        /* Split path from query string. */
        size_t      path_only_len = path_len;
        const char* q_start       = NULL;
        for (size_t i = 0; i < path_len; i++) {
            if (path[i] == '?') {
                q_start       = path + i + 1;
                path_only_len = i;
                break;
            }
        }

        char* path_str = (char*)goc_malloc(path_only_len + 1);
        memcpy(path_str, path, path_only_len);
        path_str[path_only_len] = '\0';

        size_t query_len = q_start ? (path_len - path_only_len - 1) : 0;
        char*  query_str = (char*)goc_malloc(query_len + 1);
        if (q_start && query_len > 0)
            memcpy(query_str, q_start, query_len);
        query_str[query_len] = '\0';

        /* ---- Route lookup ---- */
        goc_http_handler_t handler = NULL;
        for (size_t i = 0; i < srv->n_routes; i++) {
            if (route_match(method_str, path_str,
                             srv->routes[i].method,
                             srv->routes[i].pattern)) {
                handler = srv->routes[i].handler;
                break;
            }
        }

        if (!handler) {
            static const char resp404[] =
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Type: text/plain\r\nContent-Length: 10\r\n"
                "Connection: close\r\n\r\nNot Found\n";
            uv_buf_t b = uv_buf_init((char*)resp404, sizeof(resp404) - 1);
            goc_take(goc_io_write((uv_stream_t*)conn, &b, 1));
            free(buf);
            goc_io_handle_close((uv_handle_t*)conn, NULL);
            return;
        }

        /* ---- Build GC-managed headers array ---- */
        goc_array* req_headers = goc_array_make(num_headers);
        for (size_t i = 0; i < num_headers; i++) {
            goc_http_header_t* hdr =
                (goc_http_header_t*)goc_malloc(sizeof(goc_http_header_t));
            char* hname = (char*)goc_malloc(headers[i].name_len + 1);
            memcpy(hname, headers[i].name, headers[i].name_len);
            hname[headers[i].name_len] = '\0';
            char* hval  = (char*)goc_malloc(headers[i].value_len + 1);
            memcpy(hval, headers[i].value, headers[i].value_len);
            hval[headers[i].value_len] = '\0';
            hdr->name  = hname;
            hdr->value = hval;
            goc_array_push(req_headers, hdr);
        }

        /* ---- Build GC-managed body array (one element per byte) ---- */
        size_t body_offset  = head_consumed;
        size_t act_body_len = (content_length > 0 && buf_len > body_offset)
                              ? buf_len - body_offset : 0;
        if (content_length > 0 && act_body_len > (size_t)content_length)
            act_body_len = (size_t)content_length;

        goc_array* body_arr = goc_array_make(act_body_len);
        for (size_t i = 0; i < act_body_len; i++)
            goc_array_push(body_arr,
                goc_box_int((unsigned char)buf[body_offset + i]));

        free(buf); buf = NULL;

        /* ---- Build the per-request wrapper ---- */
        goc_req_wrapper_t* w =
            (goc_req_wrapper_t*)goc_malloc(sizeof(goc_req_wrapper_t));
        w->conn          = conn;
        w->srv           = srv;
        w->handler       = handler;
        w->ctx.method    = method_str;
        w->ctx.path      = path_str;
        w->ctx.query     = query_str;
        w->ctx.headers   = req_headers;
        w->ctx.body      = body_arr;
        w->ctx.user_data = NULL;

        /* ---- Run middleware chain then the route handler (in this fiber) ---- */
        goc_http_ctx_t* ctx = &w->ctx;
        if (srv->middleware) {
            size_t n = goc_array_len(srv->middleware);
            for (size_t i = 0; i < n; i++) {
                goc_http_middleware_t mw =
                    (goc_http_middleware_t)(uintptr_t)
                        goc_array_get(srv->middleware, i);
                if (mw(ctx) != GOC_HTTP_OK) {
                    goc_take(goc_http_server_respond_error(ctx, 500,
                                                       "Internal Server Error"));
                    goc_io_handle_close((uv_handle_t*)conn, NULL);
                    return;
                }
            }
        }
        w->handler(ctx);

        /* ---- Close connection after handler returns ---- */
        goc_io_handle_close((uv_handle_t*)conn, NULL);
        return;
    }

stop_and_close:
    /* Stop reading, drain the channel, close the connection. */
    goc_io_read_stop((uv_stream_t*)conn);
    for (;;) {
        goc_val_t* v = goc_take(read_ch);
        if (!v || v->ok != GOC_OK) break;
    }
    free(buf);
    goc_io_handle_close((uv_handle_t*)conn, NULL);
    return;

cleanup:
    /* read_ch closed before we finished reading (client EOF/error). */
    goc_io_read_stop((uv_stream_t*)conn);
    for (;;) {
        goc_val_t* v = goc_take(read_ch);
        if (!v || v->ok != GOC_OK) break;
    }
    free(buf);
    goc_io_handle_close((uv_handle_t*)conn, NULL);
}

/* =========================================================================
 * 4. Request context helpers
 * ====================================================================== */

const char* goc_http_server_header(goc_http_ctx_t* ctx, const char* name)
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

const char* goc_http_server_body_str(goc_http_ctx_t* ctx)
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
 * 5. Sending responses
 * ====================================================================== */

/*
 * goc_http_server_respond_buf — send an HTTP/1.1 response from the current fiber.
 *
 * Fibers run on pool worker threads, not the event loop thread.  libuv stream
 * operations (uv_write) are NOT thread-safe and must only be called from the
 * event loop thread.  We allocate the response buffer with goc_malloc so the
 * GC keeps it alive while the async dispatch is in flight, then dispatch the
 * write to the event loop thread via goc_io_write.
 */
goc_chan* goc_http_server_respond_buf(goc_http_ctx_t* ctx, int status,
                                  const char* content_type,
                                  const char* buf, size_t len)
{
    if (!content_type) content_type = "text/plain";

    goc_req_wrapper_t* w = WRAPPER_FROM_CTX(ctx);

    const char* stat_str = http_status_str(status);
    size_t hdr_max = 128 + strlen(content_type) + 32;
    /* GC-managed so the collector keeps it alive while goc_io_write dispatches
     * asynchronously to the event loop thread. */
    char*  resp    = (char*)goc_malloc(hdr_max + len);

    int hlen = snprintf(resp, hdr_max,
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        status, stat_str, content_type, len);
    if (hlen < 0 || (size_t)hlen >= hdr_max) {
        memcpy(resp, "HTTP/1.1 500 Internal Server Error\r\n\r\n", 38);
        hlen = 38;
        len  = 0;
    }
    if (len > 0)
        memcpy(resp + hlen, buf, len);

    uv_buf_t wbuf = uv_buf_init(resp, (unsigned int)((size_t)hlen + len));
    return goc_io_write((uv_stream_t*)w->conn, &wbuf, 1);
}

goc_chan* goc_http_server_respond(goc_http_ctx_t* ctx, int status,
                              const char* content_type, const char* body)
{
    return goc_http_server_respond_buf(ctx, status, content_type,
                                   body ? body : "",
                                   body ? strlen(body) : 0);
}

goc_chan* goc_http_server_respond_error(goc_http_ctx_t* ctx, int status,
                                    const char* message)
{
    return goc_http_server_respond(ctx, status, "text/plain",
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

/* -------------------------------------------------------------------------
 * Simple URL parser: "http://host[:port]/path[?query]"
 * Returns 0 on success; fills *out_host (GC-alloc), *out_port, *out_path
 * (GC-alloc, includes leading '/').
 * ---------------------------------------------------------------------- */
static int parse_url(const char* url, char** out_host,
                      uint16_t* out_port, char** out_path)
{
    /* Skip scheme ("http://"). */
    const char* p = strstr(url, "://");
    if (!p) return -1;
    p += 3;

    /* Authority ends at first '/' or end of string. */
    const char* slash = strchr(p, '/');
    const char* auth_end = slash ? slash : p + strlen(p);

    /* Locate last ':' in authority — handles IPv6 like [::1]:8080. */
    const char* colon = NULL;
    for (const char* c = p; c < auth_end; c++)
        if (*c == ':') colon = c;

    size_t host_len;
    if (colon) {
        host_len    = (size_t)(colon - p);
        *out_port   = (uint16_t)atoi(colon + 1);
    } else {
        host_len    = (size_t)(auth_end - p);
        *out_port   = 80;
    }

    *out_host = (char*)goc_malloc(host_len + 1);
    memcpy(*out_host, p, host_len);
    (*out_host)[host_len] = '\0';

    if (slash) {
        size_t path_len = strlen(slash);
        *out_path = (char*)goc_malloc(path_len + 1);
        memcpy(*out_path, slash, path_len + 1);
    } else {
        *out_path = (char*)goc_malloc(2);
        (*out_path)[0] = '/';
        (*out_path)[1] = '\0';
    }

    return 0;
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
            /* Skip headers whose name or value contains CR or LF to prevent
             * header-injection attacks. */
            if (h &&
                !strchr(h->name,  '\r') && !strchr(h->name,  '\n') &&
                !strchr(h->value, '\r') && !strchr(h->value, '\n'))
                APPEND("%s: %s\r\n", h->name, h->value);
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

    char*    host = NULL;
    char*    pq   = NULL;
    uint16_t port = 80;

    if (parse_url(url, &host, &port, &pq) != 0) {
        goc_http_response_t* r =
            (goc_http_response_t*)goc_malloc(sizeof(goc_http_response_t));
        memset(r, 0, sizeof(*r));
        r->headers = goc_array_make(0);
        r->body    = "";
        goc_put_cb(ch, r, close_on_put, ch);
        return ch;
    }

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
        goc_alt_op_t  ops[2] = {
            { ch,  GOC_ALT_TAKE, NULL },
            { tch, GOC_ALT_TAKE, NULL },
        };
        goc_alts_result_t* res = goc_alts(ops, 2);
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
