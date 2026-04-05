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
 *   goc_io_write (which dispatches uv_write to the event loop thread via the
 *   loop-thread callback queue).  No extra threads or mutexes are required
 *   beyond what goc_io already uses.
 *
 * HTTP client: pure goc_io fiber-based HTTP/1.1.
 *   URL parsing uses a small self-contained parser.
 *   A single http_client_fiber drives DNS (goc_io_getaddrinfo), TCP connect
 *   (goc_io_tcp_connect), write (goc_io_write), and read (goc_io_read_start)
 *   via goc_take.  Timeout is honoured via goc_timeout + goc_alts (fiber
 *   context only).
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
#include <stdatomic.h>
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
    GOC_DBG("close_on_put(http): closing ch=%p ok=%d\n", ud, (int)ok);
    goc_close((goc_chan*)ud);
    GOC_DBG("close_on_put(http): done ch=%p\n", ud);
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
    int                keep_alive;
    goc_http_ctx_t     ctx;    /* MUST be last */
} goc_req_wrapper_t;

#define WRAPPER_FROM_CTX(p) \
    ((goc_req_wrapper_t*)((char*)(p) - offsetof(goc_req_wrapper_t, ctx)))

/* Main server object (GC-allocated). */
struct goc_http_server {
    uv_tcp_t*    tcp;          /* GC-allocated TCP listen handle */
    goc_chan*    accept_ch;    /* from goc_io_tcp_server_make */
    goc_chan*    close_ch;     /* broadcast shutdown to live connection fibers */

    _Atomic int  active_conns; /* count of in-flight connection fibers */
    goc_chan*    shutdown_ch;  /* closed when active_conns drains to 0 */

    goc_route_t* routes;
    size_t       n_routes;
    size_t       cap_routes;

    goc_pool*    pool;
    goc_array*   middleware;
};

/* Per-worker-thread HTTP keep-alive slot for outbound client requests.
 *
 * This is intentionally tiny (one reusable connection per thread) to keep the
 * implementation simple and lock-free while still removing most connect/setup
 * overhead for common repeated calls to the same host:port.
 */
#if defined(_MSC_VER)
#  define GOC_THREAD_LOCAL __declspec(thread)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define GOC_THREAD_LOCAL _Thread_local
#else
#  define GOC_THREAD_LOCAL __thread
#endif

typedef struct {
    uv_tcp_t* tcp;
    char*     host;
    uint16_t  port;
    int       in_use;
} goc_http_keepalive_slot_t;

static GOC_THREAD_LOCAL goc_http_keepalive_slot_t g_http_ka_slot = {0};
static _Atomic uint64_t g_http_client_req_seq = 0;
static _Atomic int g_http_non_keepalive_inflight = 0;

#define GOC_HTTP_NON_KEEPALIVE_MAX 64

static void http_client_non_keepalive_acquire(void)
{
    for (;;) {
        /* During shutdown, skip the throttle so fibers reach the fast-fail
         * DNS/connect path and exit promptly instead of spinning on 1ms
         * timeouts and stalling pool drain. */
        if (goc_loop_is_shutting_down())
            return;
        int cur = atomic_load_explicit(&g_http_non_keepalive_inflight,
                                       memory_order_relaxed);
        while (cur < GOC_HTTP_NON_KEEPALIVE_MAX) {
            if (atomic_compare_exchange_weak_explicit(
                    &g_http_non_keepalive_inflight,
                    &cur,
                    cur + 1,
                    memory_order_acq_rel,
                    memory_order_relaxed)) {
                return;
            }
        }
        goc_take(goc_timeout(1));
    }
}

static void http_client_non_keepalive_release(void)
{
    int prev = atomic_fetch_sub_explicit(&g_http_non_keepalive_inflight,
                                         1,
                                         memory_order_acq_rel);
    if (prev <= 0)
        atomic_store_explicit(&g_http_non_keepalive_inflight,
                              0,
                              memory_order_release);
}

static int keepalive_slot_matches(const goc_http_keepalive_slot_t* s,
                                  const char* host, uint16_t port)
{
    return s && s->tcp && s->host && s->port == port && strcmp(s->host, host) == 0;
}

static void keepalive_slot_drop(goc_http_keepalive_slot_t* s)
{
    if (!s)
        return;
    GOC_DBG("keepalive_slot_drop: slot=%p tcp=%p host=%s port=%u in_use=%d\n",
            (void*)s, (void*)s->tcp, s->host ? s->host : "<null>",
            (unsigned)s->port, s->in_use);
    if (s->tcp) {
        goc_io_handle_close((uv_handle_t*)s->tcp, NULL);
        s->tcp = NULL;
    }
    s->host  = NULL;
    s->port  = 0;
    s->in_use = 0;
}

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
    o->pool = goc_current_or_default_pool();
    return o;
}

goc_http_server_t* goc_http_server_make(const goc_http_server_opts_t* opts)
{
    goc_http_server_t* srv =
        (goc_http_server_t*)goc_malloc(sizeof(goc_http_server_t));
    memset(srv, 0, sizeof(*srv));

    srv->pool       = opts && opts->pool ? opts->pool : goc_current_or_default_pool();
    srv->middleware = opts ? opts->middleware : NULL;
    srv->close_ch   = goc_chan_make(0);
    srv->active_conns = 0;
    srv->shutdown_ch  = goc_chan_make(0);

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
        goc_put(ready_ch, goc_box_int(r));
        goc_close(ready_ch);
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
        goc_put(ready_ch, goc_box_int(r));
        goc_close(ready_ch);
        return ready_ch;
    }

    /* Bind. */
    r = goc_unbox_int(
            goc_take(goc_io_tcp_bind(srv->tcp,
                                     (const struct sockaddr*)&addr))->val);
    if (r < 0) {
        goc_io_handle_close((uv_handle_t*)srv->tcp, NULL);
        srv->tcp = NULL;
        goc_put(ready_ch, goc_box_int(r));
        goc_close(ready_ch);
        return ready_ch;
    }

    /* Start listening; ready_ch will receive goc_box_int(0) when uv_listen
     * has been called on the event-loop thread. */
    srv->accept_ch = goc_io_tcp_server_make(srv->tcp, 128, ready_ch);

    /* Pin the server struct in the GC while the accept loop fiber runs. */
    gc_handle_register(srv);

    /* Spawn the accept loop fiber. */
    goc_pool* pool = srv->pool ? srv->pool : goc_current_or_default_pool();
    goc_go_on(pool, accept_loop_fiber, srv);

    return ready_ch;
}

goc_chan* goc_http_server_close(goc_http_server_t* srv)
{
    goc_chan* ch = goc_chan_make(1);

    GOC_DBG("goc_http_server_close: srv=%p tcp=%p accept_ch=%p close_ch=%p\n",
            (void*)srv, (void*)srv->tcp, (void*)srv->accept_ch, (void*)srv->close_ch);

    if (srv->close_ch) {
        goc_close(srv->close_ch);
        srv->close_ch = NULL;
    }

    /* Close the accept channel so the accept loop fiber breaks. */
    int was_listening = (srv->accept_ch != NULL);
    if (srv->accept_ch) {
        goc_close(srv->accept_ch);
        srv->accept_ch = NULL;
    }

    /* Close the TCP listen handle (asynchronous; no callback needed). */
    if (srv->tcp) {
        goc_io_handle_close((uv_handle_t*)srv->tcp, NULL);
        srv->tcp = NULL;
    }

    /* Wait for all in-flight connection fibers (and accept_loop_fiber) to
     * finish.  accept_loop_fiber counts itself in active_conns, so if listen
     * was ever called active_conns is guaranteed > 0 until the accept loop
     * exits — we can unconditionally goc_take.  If listen was never called
     * nobody will close shutdown_ch, so we close it ourselves. */
    if (was_listening)
        goc_take(srv->shutdown_ch);
    else
        goc_close(srv->shutdown_ch);
    srv->shutdown_ch = NULL;

    /* Unpin the server from the GC root list. */
    gc_handle_unregister(srv);

    GOC_DBG("goc_http_server_close: completed srv=%p tcp=%p accept_ch=%p close_ch=%p\n",
            (void*)srv, (void*)srv->tcp, (void*)srv->accept_ch, (void*)srv->close_ch);
    goc_put(ch, goc_box_int(0));
    goc_close(ch);
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

static int request_wants_keepalive(const struct phr_header* headers,
                                   size_t num_headers,
                                   int minor_version)
{
    /* HTTP/1.1 default keep-alive unless Connection: close.
     * HTTP/1.0 default close unless Connection: keep-alive. */
    int default_keep = (minor_version >= 1);
    for (size_t i = 0; i < num_headers; i++) {
        if (headers[i].name_len == 10 &&
            strncasecmp(headers[i].name, "connection", 10) == 0) {
            if (headers[i].value_len == 5 &&
                strncasecmp(headers[i].value, "close", 5) == 0)
                return 0;
            if (headers[i].value_len == 10 &&
                strncasecmp(headers[i].value, "keep-alive", 10) == 0)
                return 1;
        }
    }
    return default_keep;
}

/* =========================================================================
 * 3. Accept loop and per-connection fibers
 * ====================================================================== */

typedef struct {
    goc_http_server_t* srv;
    uv_tcp_t*     conn;
    goc_chan*     close_ch; /* captured at spawn; srv->close_ch may be NULL by the time the fiber runs */
} conn_arg_t;

static void accept_loop_fiber(void* arg)
{
    goc_http_server_t* srv      = (goc_http_server_t*)arg;
    goc_chan*     accept_ch = srv->accept_ch; /* cache before any yield point */
    goc_chan*     close_ch  = srv->close_ch;  /* cache before any yield point */
    GOC_DBG("accept_loop_fiber: started, accept_ch=%p\n", (void*)accept_ch);
    if (!accept_ch) {
        GOC_DBG("accept_loop_fiber: accept_ch is NULL, closing shutdown_ch and returning\n");
        goc_close(srv->shutdown_ch); /* active_conns was never incremented; we are the only closer */
        return;
    }
    /* Count this fiber itself so active_conns > 0 until we finish all spawns.
     * This prevents goc_http_server_close from declaring the drain complete
     * before we have finished incrementing for connections we are about to
     * spawn. */
    atomic_fetch_add_explicit(&srv->active_conns, 1, memory_order_release);
    for (;;) {
        GOC_DBG("accept_loop_fiber: waiting for connection on accept_ch=%p\n", (void*)accept_ch);
        goc_val_t* v = goc_take(accept_ch);
        GOC_DBG("accept_loop_fiber: goc_take returned v=%p ok=%d\n", (void*)v, v ? (int)v->ok : -1);
        if (!v || v->ok != GOC_OK)
            break;  /* channel closed — server is shutting down */
        uv_tcp_t* conn = (uv_tcp_t*)v->val;
        conn_arg_t* a  = (conn_arg_t*)goc_malloc(sizeof(conn_arg_t));
        a->srv      = srv;
        a->conn     = conn;
        a->close_ch = close_ch;
        GOC_DBG("accept_loop_fiber: spawning handle_conn_fiber for conn=%p\n", (void*)conn);
        atomic_fetch_add_explicit(&srv->active_conns, 1, memory_order_release);
        goc_pool* pool = srv->pool ? srv->pool : goc_current_or_default_pool();
        goc_go_on(pool, handle_conn_fiber, a);
    }

    /* Drain any connections buffered in accept_ch that were not yet spawned.
     * on_server_connection calls uv_accept immediately, so these handles have
     * a live server-side fd.  Close them now to send FIN to clients; without
     * this, clients waiting on read hang indefinitely. */
    for (;;) {
        goc_val_t* v = goc_take(accept_ch);
        if (!v || v->ok != GOC_OK)
            break;
        uv_tcp_t* conn = (uv_tcp_t*)v->val;
        GOC_DBG("accept_loop_fiber: closing unspawned conn=%p\n", (void*)conn);
        gc_handle_unregister(conn);
        goc_io_handle_close((uv_handle_t*)conn, NULL);
    }

    GOC_DBG("accept_loop_fiber: exiting (accept_ch closed)\n");
    if (atomic_fetch_sub_explicit(&srv->active_conns, 1, memory_order_release) == 1)
        goc_close(srv->shutdown_ch);
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
    goc_chan*     close_ch = a->close_ch;
    GOC_DBG("handle_conn_fiber: started conn=%p\n", (void*)conn);

    /* Accumulation buffer (plain-malloc; we control its entire lifetime). */
    size_t buf_cap = 4096;
    char*  buf     = (char*)malloc(buf_cap);
    if (!buf) {
        GOC_DBG("handle_conn_fiber: malloc failed, closing conn\n");
        goc_io_handle_close((uv_handle_t*)conn, NULL);
        return;
    }

    for (;;) {
        size_t buf_len = 0;
        int read_started = 0;
        int must_close_conn = 0;

        GOC_DBG("handle_conn_fiber: calling goc_io_read_start on conn=%p\n", (void*)conn);
        goc_chan* read_ch = goc_io_read_start((uv_stream_t*)conn);
        read_started = 1;
        GOC_DBG("handle_conn_fiber: goc_io_read_start returned read_ch=%p\n", (void*)read_ch);

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
            GOC_DBG("handle_conn_fiber: entering goc_alts read_ch=%p close_ch=%p\n",
                    (void*)read_ch, (void*)close_ch);
            goc_alt_op_t ops[] = {
                { .ch = read_ch,  .op_kind = GOC_ALT_TAKE, .put_val = NULL },
                { .ch = close_ch, .op_kind = GOC_ALT_TAKE, .put_val = NULL },
            };
            goc_alts_result_t* sel = goc_alts(ops, 2);
            if (!sel || sel->ch != read_ch || sel->value.ok != GOC_OK) {
                must_close_conn = 1;
                break;
            }
            goc_val_t* v = &sel->value;
            goc_io_read_t* r = (goc_io_read_t*)v->val;
            if (r->nread < 0) {
                must_close_conn = 1;
                break;
            }

            size_t chunk = (size_t)r->nread;
            if (buf_len + chunk > GOC_SERVER_MAX_REQ_SIZE) {
                must_close_conn = 1;
                break;
            }
            if (buf_len + chunk > buf_cap) {
                size_t nc = buf_cap;
                while (nc < buf_len + chunk) nc *= 2;
                char* nb = (char*)realloc(buf, nc);
                if (!nb) {
                    must_close_conn = 1;
                    break;
                }
                buf = nb;
                buf_cap = nc;
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

        if (!must_close_conn && pret < 0) {
            static const char resp400[] =
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Type: text/plain\r\nContent-Length: 11\r\n"
                "Connection: close\r\n\r\nBad Request";
            uv_buf_t b = uv_buf_init((char*)resp400, sizeof(resp400) - 1);
            goc_take(goc_io_write((uv_stream_t*)conn, &b, 1));
            must_close_conn = 1;
        }

        int keep_alive_req = 0;
        ssize_t content_length = 0;
        size_t head_consumed = 0;
        size_t body_end = 0;

        if (!must_close_conn) {
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

            keep_alive_req = request_wants_keepalive(headers, num_headers, minor_version);
            head_consumed = (size_t)pret;
            body_end = head_consumed +
                      (content_length > 0 ? (size_t)content_length : 0);

            while (buf_len < body_end) {
                goc_alt_op_t ops[] = {
                    { .ch = read_ch,  .op_kind = GOC_ALT_TAKE, .put_val = NULL },
                    { .ch = close_ch, .op_kind = GOC_ALT_TAKE, .put_val = NULL },
                };
                goc_alts_result_t* sel = goc_alts(ops, 2);
                if (!sel || sel->ch != read_ch || sel->value.ok != GOC_OK) {
                    must_close_conn = 1;
                    break;
                }
                goc_val_t* v = &sel->value;
                goc_io_read_t* r = (goc_io_read_t*)v->val;
                if (r->nread < 0) {
                    must_close_conn = 1;
                    break;
                }

                size_t chunk = (size_t)r->nread;
                if (buf_len + chunk > GOC_SERVER_MAX_REQ_SIZE) {
                    must_close_conn = 1;
                    break;
                }
                if (buf_len + chunk > buf_cap) {
                    size_t nc = buf_cap;
                    while (nc < buf_len + chunk) nc *= 2;
                    char* nb = (char*)realloc(buf, nc);
                    if (!nb) {
                        must_close_conn = 1;
                        break;
                    }
                    buf = nb;
                    buf_cap = nc;
                }
                memcpy(buf + buf_len, r->buf->base, chunk);
                buf_len += chunk;
            }
        }

        if (read_started) {
            goc_io_read_stop((uv_stream_t*)conn);
            for (;;) {
                goc_val_t* dv = goc_take(read_ch);
                if (!dv || dv->ok != GOC_OK)
                    break;
            }
            read_started = 0;
        }

        if (must_close_conn)
            break;

        /* ---- Build GC-managed method/path/query strings ---- */
        char* method_str = (char*)goc_malloc(method_len + 1);
        memcpy(method_str, method, method_len);
        method_str[method_len] = '\0';

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
            const char* body404 = "Not Found\n";
            char resp404[256];
            int n = snprintf(resp404, sizeof(resp404),
                             "HTTP/1.1 404 Not Found\r\n"
                             "Content-Type: text/plain\r\n"
                             "Content-Length: %zu\r\n"
                             "Connection: %s\r\n\r\n"
                             "%s",
                             strlen(body404),
                             keep_alive_req ? "keep-alive" : "close",
                             body404);
            if (n > 0) {
                uv_buf_t b = uv_buf_init(resp404, (unsigned int)n);
                goc_take(goc_io_write((uv_stream_t*)conn, &b, 1));
            }
            if (!keep_alive_req)
                break;
            continue;
        }

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

        size_t body_offset  = head_consumed;
        size_t act_body_len = (content_length > 0 && buf_len > body_offset)
                              ? buf_len - body_offset : 0;
        if (content_length > 0 && act_body_len > (size_t)content_length)
            act_body_len = (size_t)content_length;

        goc_array* body_arr = goc_array_make(act_body_len);
        for (size_t i = 0; i < act_body_len; i++)
            goc_array_push(body_arr,
                           goc_box_int((unsigned char)buf[body_offset + i]));

        goc_req_wrapper_t* w =
            (goc_req_wrapper_t*)goc_malloc(sizeof(goc_req_wrapper_t));
        w->conn          = conn;
        w->srv           = srv;
        w->handler       = handler;
        w->keep_alive    = keep_alive_req;
        w->ctx.method    = method_str;
        w->ctx.path      = path_str;
        w->ctx.query     = query_str;
        w->ctx.headers   = req_headers;
        w->ctx.body      = body_arr;
        w->ctx.user_data = NULL;

        goc_http_ctx_t* ctx = &w->ctx;
        if (srv->middleware) {
            size_t n = goc_array_len(srv->middleware);
            int middleware_ok = 1;
            for (size_t i = 0; i < n; i++) {
                goc_http_middleware_t mw =
                    (goc_http_middleware_t)(uintptr_t)
                        goc_array_get(srv->middleware, i);
                if (mw(ctx) != GOC_HTTP_OK) {
                    goc_take(goc_http_server_respond_error(ctx, 500,
                                                           "Internal Server Error"));
                    middleware_ok = 0;
                    break;
                }
            }
            if (!middleware_ok) {
                if (!w->keep_alive)
                    break;
                continue;
            }
        }

        w->handler(ctx);
        if (!w->keep_alive)
            break;
    }

    free(buf);
    goc_io_handle_close((uv_handle_t*)conn, NULL);
    GOC_DBG("handle_conn_fiber: connection closed conn=%p\n", (void*)conn);
    if (atomic_fetch_sub_explicit(&srv->active_conns, 1, memory_order_release) == 1)
        goc_close(srv->shutdown_ch);
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
                        "Connection: %s\r\n"
                        "\r\n",
                        status, stat_str, content_type, len,
                        w->keep_alive ? "keep-alive" : "close");
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
 * 6. HTTP client — goc_io fiber-based HTTP/1.1
 *
 * A single http_client_fiber drives the entire lifecycle:
 *   DNS (goc_io_getaddrinfo) → connect (goc_io_tcp_connect) →
 *   write (goc_io_write) → read (goc_io_read_start).
 * All I/O operations are performed with goc_take, exactly like the server
 * connection fiber.  The result goc_http_response_t (and its string/array
 * fields) are goc_malloc'd so the GC keeps them alive for the caller.
 * ====================================================================== */

goc_http_request_opts_t* goc_http_request_opts(void)
{
    goc_http_request_opts_t* o =
        (goc_http_request_opts_t*)goc_malloc(sizeof(goc_http_request_opts_t));
    memset(o, 0, sizeof(*o));
    o->pool = goc_current_or_default_pool();
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

static int http_client_parse_numeric_addr(const char* host,
                                          uint16_t port,
                                          struct sockaddr_storage* out_addr)
{
    if (!host || !out_addr)
        return UV_EINVAL;

    memset(out_addr, 0, sizeof(*out_addr));

    int rc = uv_ip4_addr(host, port, (struct sockaddr_in*)out_addr);
    if (rc == 0)
        return 0;

    if (host[0] == '[') {
        size_t len = strlen(host);
        if (len >= 2 && host[len - 1] == ']') {
            char* bare = (char*)goc_malloc(len - 1);
            memcpy(bare, host + 1, len - 2);
            bare[len - 2] = '\0';
            rc = uv_ip6_addr(bare, port, (struct sockaddr_in6*)out_addr);
            if (rc == 0)
                return 0;
        }
    }

    return uv_ip6_addr(host, port, (struct sockaddr_in6*)out_addr);
}

/* Parse status line + headers from a raw response buffer.
 * Returns 1 when headers are complete, 0 when more data is needed. */
static int parse_response_head_buf(char* buf, size_t len,
                                   goc_http_response_t** out_resp,
                                   size_t* out_body_start,
                                   ssize_t* out_content_length)
{
    /* Find \r\n\r\n. */
    char* eoh = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i]=='\r' && buf[i+1]=='\n' && buf[i+2]=='\r' && buf[i+3]=='\n') {
            eoh = buf + i + 4;
            break;
        }
    }
    if (!eoh) return 0;

    *out_body_start = (size_t)(eoh - buf);

    /* Allocate response. */
    goc_http_response_t* resp =
        (goc_http_response_t*)goc_malloc(sizeof(goc_http_response_t));
    memset(resp, 0, sizeof(*resp));
    resp->headers = goc_array_make(8);
    *out_resp           = resp;
    *out_content_length = -1;

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
            *out_content_length = (ssize_t)atol(value);

        goc_http_header_t* hdr =
            (goc_http_header_t*)goc_malloc(sizeof(goc_http_header_t));
        hdr->name  = name;
        hdr->value = value;
        goc_array_push(resp->headers, hdr);

        line = nl + 1;
    }
    return 1;
}

static int response_has_connection_close(const goc_http_response_t* resp)
{
    if (!resp || !resp->headers)
        return 0;
    size_t n = goc_array_len(resp->headers);
    for (size_t i = 0; i < n; i++) {
        goc_http_header_t* h = (goc_http_header_t*)goc_array_get(resp->headers, i);
        if (!h || !h->name || !h->value)
            continue;
        if (strcasecmp(h->name, "connection") == 0 &&
            strcasecmp(h->value, "close") == 0)
            return 1;
    }
    return 0;
}

/* Build raw HTTP/1.1 request (plain-malloc'd; caller must free). */
static char* build_request(const char* method, const char* host,
                             const char* pq,   /* path[?query] */
                             const char* ct,   /* content-type, may be NULL */
                             const char* body, size_t body_len,
                             int keep_alive,
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

        APPEND("%s %s HTTP/1.1\r\nHost: %s\r\nConnection: %s\r\n",
            method, pq, host, keep_alive ? "keep-alive" : "close");

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
 * Fiber argument for http_client_fiber
 * ----------------------------------------------------------------------- */

typedef struct {
    uint64_t   req_id;
    char*      method;
    char*      host;
    uint16_t   port;
    char*      path_and_query;
    char*      content_type;
    char*      body;       /* goc_malloc'd copy, may be NULL */
    size_t     body_len;
    int        keep_alive;
    goc_array* extra_headers;
    goc_chan*  ch;         /* result channel — caller is waiting on this */
} http_client_arg_t;

static void http_client_fiber(void* arg)
{
    http_client_arg_t* a = (http_client_arg_t*)arg;
    char* resp_buf = NULL;
    uv_tcp_t* tcp = NULL;
    int tcp_initialized = 0;
    int using_keepalive_slot = 0;
    int churn_slot_acquired = 0;
    GOC_DBG("http_client_fiber[%llu]: started method=%s host=%s port=%u path=%s ch=%p keep_alive=%d slot={tcp=%p host=%s port=%u in_use=%d}\n",
            (unsigned long long)a->req_id,
            a->method, a->host, (unsigned)a->port, a->path_and_query, (void*)a->ch,
            a->keep_alive,
            (void*)g_http_ka_slot.tcp,
            g_http_ka_slot.host ? g_http_ka_slot.host : "<null>",
            (unsigned)g_http_ka_slot.port,
            g_http_ka_slot.in_use);

    if (!a->keep_alive) {
        http_client_non_keepalive_acquire();
        churn_slot_acquired = 1;
    }

    if (a->keep_alive &&
        !g_http_ka_slot.in_use &&
        keepalive_slot_matches(&g_http_ka_slot, a->host, a->port)) {
        tcp = g_http_ka_slot.tcp;
        tcp_initialized = 1;
        g_http_ka_slot.in_use = 1;
        using_keepalive_slot = 1;
        GOC_DBG("http_client_fiber[%llu]: reusing keep-alive tcp=%p host=%s port=%u\n",
            (unsigned long long)a->req_id, (void*)tcp, a->host, (unsigned)a->port);
    }

    if (!tcp) {
        struct sockaddr_storage addr;
        int have_addr = (http_client_parse_numeric_addr(a->host, a->port, &addr) == 0);
        if (have_addr) {
            GOC_DBG("http_client_fiber[%llu]: numeric host fast-path host=%s port=%u family=%d\n",
                (unsigned long long)a->req_id, a->host, (unsigned)a->port, (int)addr.ss_family);
        } else {
            /* --- DNS --- */
            struct addrinfo hints;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family   = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            GOC_DBG("http_client_fiber[%llu]: DNS lookup for host=%s\n",
                (unsigned long long)a->req_id, a->host);
            goc_val_t* vdns = goc_take(goc_io_getaddrinfo(a->host, NULL, &hints));
            GOC_DBG("http_client_fiber[%llu]: DNS returned v=%p ok=%d\n",
                (unsigned long long)a->req_id, (void*)vdns, vdns ? (int)vdns->ok : -1);
            if (!vdns || vdns->ok != GOC_OK) goto deliver_error;
            goc_io_getaddrinfo_t* dns = (goc_io_getaddrinfo_t*)vdns->val;
            if (!dns || dns->ok != GOC_IO_OK || !dns->res) {
                GOC_DBG("http_client_fiber[%llu]: DNS error dns=%p\n",
                    (unsigned long long)a->req_id, (void*)dns);
                goto deliver_error;
            }

            /* Pick first address and set port. */
            memcpy(&addr, dns->res->ai_addr, dns->res->ai_addrlen);
            if (addr.ss_family == AF_INET)
                ((struct sockaddr_in*)&addr)->sin_port = htons(a->port);
            else
                ((struct sockaddr_in6*)&addr)->sin6_port = htons(a->port);
            uv_freeaddrinfo(dns->res);
        }

        /* --- TCP init + connect --- */
        tcp = (uv_tcp_t*)goc_malloc(sizeof(uv_tcp_t));
        GOC_DBG("http_client_fiber[%llu]: tcp_init tcp=%p\n",
            (unsigned long long)a->req_id, (void*)tcp);
        int rc = goc_unbox_int(goc_take(goc_io_tcp_init(tcp))->val);
        GOC_DBG("http_client_fiber[%llu]: tcp_init rc=%d\n",
            (unsigned long long)a->req_id, rc);
        if (rc < 0) {
            GOC_DBG("http_client_fiber[%llu]: tcp_init failed (tcp left uninitialized)\n",
                    (unsigned long long)a->req_id);
            tcp = NULL;
            goto deliver_error;
        }
        tcp_initialized = 1;

        rc = goc_unbox_int(
            goc_take(goc_io_tcp_connect(tcp, (const struct sockaddr*)&addr))->val);
        GOC_DBG("http_client_fiber[%llu]: tcp_connect rc=%d tcp=%p\n",
            (unsigned long long)a->req_id, rc, (void*)tcp);
        if (rc < 0) {
            /* Includes ordinary connect failures and loop-teardown rejection
             * when fire-and-forget requests outlive the runtime shutdown
             * fence. Treat both as a normal error path and close the
             * initialized handle. */
            GOC_DBG("http_client_fiber[%llu]: tcp_connect failed rc=%d tcp=%p\n",
                (unsigned long long)a->req_id, rc, (void*)tcp);
            if (tcp_initialized)
                goc_io_handle_close((uv_handle_t*)tcp, NULL);
            tcp = NULL;
            tcp_initialized = 0;
            goto deliver_error;
        }
    }

    /* --- Write request --- */
    size_t req_len;
    char* req_plain = build_request(a->method, a->host, a->path_and_query,
                                    a->content_type, a->body, a->body_len,
                                    a->keep_alive,
                                    a->extra_headers, &req_len);
    /* Copy into goc_malloc so the GC keeps the buffer alive while
     * goc_io_write dispatches asynchronously to the event loop thread. */
    char* gc_req = (char*)goc_malloc(req_len);
    memcpy(gc_req, req_plain, req_len);
    free(req_plain);

    uv_buf_t wb = uv_buf_init(gc_req, (unsigned int)req_len);
    GOC_DBG("http_client_fiber[%llu]: writing request len=%zu tcp=%p\n",
            (unsigned long long)a->req_id, req_len, (void*)tcp);
    int rc = goc_unbox_int(
        goc_take(goc_io_write((uv_stream_t*)tcp, &wb, 1))->val);
    GOC_DBG("http_client_fiber[%llu]: write rc=%d tcp=%p\n",
            (unsigned long long)a->req_id, rc, (void*)tcp);
    if (rc < 0) {
        GOC_DBG("http_client_fiber[%llu]: write failed tcp=%p\n",
                (unsigned long long)a->req_id, (void*)tcp);
        if (tcp_initialized)
            goc_io_handle_close((uv_handle_t*)tcp, NULL);
        tcp = NULL;
        tcp_initialized = 0;
        goto deliver_error;
    }

    /* --- Read + parse response --- */
    {
        size_t resp_len = 0;
        size_t resp_cap = 0;
        goc_http_response_t* response = NULL;
        ssize_t content_length = -1;
        size_t  body_start     = 0;

        GOC_DBG("http_client_fiber[%llu]: starting read on tcp=%p\n",
            (unsigned long long)a->req_id, (void*)tcp);
        goc_chan* rd = goc_io_read_start((uv_stream_t*)tcp);
        GOC_DBG("http_client_fiber[%llu]: read_ch=%p\n",
            (unsigned long long)a->req_id, (void*)rd);
        for (;;) {
            GOC_DBG("http_client_fiber[%llu]: waiting for response chunk resp_len=%zu\n",
                (unsigned long long)a->req_id, resp_len);
            goc_val_t* v = goc_take(rd);
            GOC_DBG("http_client_fiber[%llu]: response goc_take v=%p ok=%d\n",
                (unsigned long long)a->req_id, (void*)v, v ? (int)v->ok : -1);
            if (!v || v->ok != GOC_OK) break;
            goc_io_read_t* r = (goc_io_read_t*)v->val;
            GOC_DBG("http_client_fiber[%llu]: response chunk nread=%zd\n",
                (unsigned long long)a->req_id, r->nread);
            if (r->nread < 0) break;

            /* Grow plain-malloc buffer. */
            size_t needed = resp_len + (size_t)r->nread + 1;
            if (needed > resp_cap) {
                size_t nc = resp_cap ? resp_cap * 2 : 4096;
                while (nc < needed) nc *= 2;
                char* nb = (char*)realloc(resp_buf, nc);
                if (!nb) break;
                resp_buf = nb; resp_cap = nc;
            }
            memcpy(resp_buf + resp_len, r->buf->base, (size_t)r->nread);
            resp_len += (size_t)r->nread;
            resp_buf[resp_len] = '\0';

            /* Parse headers once we have them. */
            if (!response)
                parse_response_head_buf(resp_buf, resp_len,
                                        &response, &body_start,
                                        &content_length);

            /* Stop once Content-Length body is complete. */
            if (response && content_length >= 0) {
                size_t got = resp_len > body_start ? resp_len - body_start : 0;
                if (got >= (size_t)content_length) break;
            }
        }
        GOC_DBG("http_client_fiber[%llu]: read loop done, stopping read rd=%p tcp=%p\n",
            (unsigned long long)a->req_id, (void*)rd, (void*)tcp);
        goc_io_read_stop((uv_stream_t*)tcp);
        for (;;) {
            goc_val_t* dv = goc_take(rd);
            GOC_DBG("http_client_fiber[%llu]: read drain dv=%p ok=%d\n",
                (unsigned long long)a->req_id, (void*)dv, dv ? (int)dv->ok : -1);
            if (!dv || dv->ok != GOC_OK)
                break;
        }
        GOC_DBG("http_client_fiber[%llu]: read drain complete tcp=%p\n",
            (unsigned long long)a->req_id, (void*)tcp);

        /* --- Attach body and deliver --- */
        if (!response) {
            if (using_keepalive_slot)
                keepalive_slot_drop(&g_http_ka_slot);
            else if (tcp) {
                GOC_DBG("http_client_fiber[%llu]: closing tcp=%p after missing response\n",
                        (unsigned long long)a->req_id, (void*)tcp);
                if (tcp_initialized)
                    goc_io_handle_close((uv_handle_t*)tcp, NULL);
                tcp = NULL;
                tcp_initialized = 0;
            }
            if (resp_buf) free(resp_buf);
            goto deliver_error;
        }
        size_t blen = resp_len > body_start ? resp_len - body_start : 0;
        if (content_length >= 0 && (size_t)content_length < blen)
            blen = (size_t)content_length;
        char* body = (char*)goc_malloc(blen + 1);
        if (blen && resp_buf) memcpy(body, resp_buf + body_start, blen);
        body[blen] = '\0';
        if (resp_buf) free(resp_buf);
        response->body     = body;
        response->body_len = blen;

        int can_keep = a->keep_alive &&
                       content_length >= 0 &&
                       !response_has_connection_close(response);
        if (can_keep) {
            if (using_keepalive_slot) {
                g_http_ka_slot.in_use = 0;
                GOC_DBG("http_client_fiber[%llu]: returned keep-alive tcp=%p to slot\n",
                        (unsigned long long)a->req_id, (void*)tcp);
            } else if (!g_http_ka_slot.in_use) {
                keepalive_slot_drop(&g_http_ka_slot);
                g_http_ka_slot.tcp    = tcp;
                g_http_ka_slot.host   = a->host;
                g_http_ka_slot.port   = a->port;
                g_http_ka_slot.in_use = 0;
                GOC_DBG("http_client_fiber[%llu]: stored tcp=%p in keep-alive slot host=%s port=%u\n",
                        (unsigned long long)a->req_id, (void*)tcp, a->host, (unsigned)a->port);
                tcp = NULL;
                tcp_initialized = 0;
            } else {
                GOC_DBG("http_client_fiber[%llu]: keep-alive slot busy, closing tcp=%p\n",
                        (unsigned long long)a->req_id, (void*)tcp);
                if (tcp_initialized)
                    goc_io_handle_close((uv_handle_t*)tcp, NULL);
                tcp_initialized = 0;
            }
        } else {
            if (using_keepalive_slot)
                keepalive_slot_drop(&g_http_ka_slot);
            else if (tcp) {
                GOC_DBG("http_client_fiber[%llu]: closing non-keepalive tcp=%p can_keep=%d\n",
                        (unsigned long long)a->req_id, (void*)tcp, can_keep);
                if (tcp_initialized)
                    goc_io_handle_close((uv_handle_t*)tcp, NULL);
                tcp_initialized = 0;
            }
        }

        GOC_DBG("http_client_fiber[%llu]: delivering response status=%d blen=%zu on ch=%p\n",
                (unsigned long long)a->req_id, response->status, blen, (void*)a->ch);
        goc_put(a->ch, response);
        goc_close(a->ch);
        if (churn_slot_acquired)
            http_client_non_keepalive_release();
        return;
    }

deliver_error: {
    GOC_DBG("http_client_fiber[%llu]: deliver_error, delivering status=0 on ch=%p tcp=%p using_slot=%d\n",
            (unsigned long long)a->req_id, (void*)a->ch, (void*)tcp, using_keepalive_slot);
    if (using_keepalive_slot)
        keepalive_slot_drop(&g_http_ka_slot);
    else if (tcp) {
        GOC_DBG("http_client_fiber[%llu]: closing tcp=%p on error path (initialized=%d)\n",
                (unsigned long long)a->req_id, (void*)tcp, tcp_initialized);
        if (tcp_initialized)
            goc_io_handle_close((uv_handle_t*)tcp, NULL);
        tcp = NULL;
        tcp_initialized = 0;
    }
    if (resp_buf) free(resp_buf);
    goc_http_response_t* r =
        (goc_http_response_t*)goc_malloc(sizeof(goc_http_response_t));
    memset(r, 0, sizeof(*r));
    r->headers = goc_array_make(0);
    r->body    = "";
    goc_put(a->ch, r);
    goc_close(a->ch);
    if (churn_slot_acquired)
        http_client_non_keepalive_release();
    }
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
        goc_put(ch, r);
        goc_close(ch);
        return ch;
    }

    char* body_copy = NULL;
    if (body && body_len > 0) {
        body_copy = (char*)goc_malloc(body_len + 1);
        memcpy(body_copy, body, body_len);
        body_copy[body_len] = '\0';
    }

    http_client_arg_t* a =
        (http_client_arg_t*)goc_malloc(sizeof(http_client_arg_t));
    a->req_id         = atomic_fetch_add_explicit(&g_http_client_req_seq, 1, memory_order_relaxed) + 1;
    a->method         = (char*)method;  /* caller's lifetime >= fiber */
    a->host           = host;
    a->port           = port;
    a->path_and_query = pq;
    a->content_type   = content_type ? (char*)content_type : NULL;
    a->body           = body_copy;
    a->body_len       = body_len;
    a->keep_alive     = opts ? opts->keep_alive : 0;
    a->extra_headers  = opts ? opts->headers : NULL;
    a->ch             = ch;

            GOC_DBG("goc_http_request[%llu]: method=%s url=%s host=%s port=%u path=%s keep_alive=%d timeout_ms=%llu ch=%p\n",
            (unsigned long long)a->req_id, method, url, host, (unsigned)port, pq,
            opts ? opts->keep_alive : 0,
                (unsigned long long)(opts ? opts->timeout_ms : 0),
            (void*)ch);

    goc_pool* req_pool = (opts && opts->pool) ? opts->pool : goc_current_or_default_pool();
    goc_go_on(req_pool, http_client_fiber, a);

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
            goc_put(toch, r);
            goc_close(toch);
            return toch;
        }
        /* Response arrived before timeout — re-wrap on a fresh channel. */
        goc_chan* ch2 = goc_chan_make(1);
        goc_put(ch2, res->value.val);
        goc_close(ch2);
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
