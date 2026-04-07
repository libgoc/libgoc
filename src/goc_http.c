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
 *   via goc_take.  Timeout is honoured via goc_timeout + goc_alts/goc_alts_sync
 *   through a helper wrapper that supports both fiber and OS-thread callers.
 */

#if defined(__linux__)
#  if !defined(_GNU_SOURCE)
#    define _GNU_SOURCE
#  endif
#endif

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  define strncasecmp _strnicmp
#  define strcasecmp  _stricmp
#else
#  include <strings.h>
#  include <unistd.h>
#  include <sys/socket.h>
#  include <sched.h>
#endif
#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#include <errno.h>

static void http_client_fiber(void* arg);
void (*g_http_client_fiber_ptr)(void*) = http_client_fiber;
#include <stdbool.h>
#include <stdatomic.h>
#if defined(_WIN32)
#  include <winsock2.h>
#endif
#include <uv.h>
#if !defined(GOC_HTTP_REUSEPORT)
#  if defined(SO_REUSEPORT)
#    define GOC_HTTP_REUSEPORT 1
#  else
#    define GOC_HTTP_REUSEPORT 0
#  endif
#endif

#if GOC_HTTP_REUSEPORT && defined(__linux__)
#  define GOC_HTTP_REUSEPORT_LINUX 1
#else
#  define GOC_HTTP_REUSEPORT_LINUX 0
#endif

#include "../vendor/picohttpparser/picohttpparser.h"
#include "../include/goc_http.h"
#include "../include/goc_io.h"
#include "../include/goc_array.h"
#include "internal.h"
#include "channel_internal.h"
#include <inttypes.h>

/* =========================================================================
 * Shared utilities
 * ====================================================================== */

static goc_val_t* goc_http_chan_take(goc_chan* ch)
{
    return goc_in_fiber() ? goc_take(ch) : goc_take_sync(ch);
}

static goc_alts_result_t* goc_http_chan_alts(goc_alt_op_t* ops, size_t n)
{
    return goc_in_fiber() ? goc_alts(ops, n) : goc_alts_sync(ops, n);
}

static goc_status_t goc_http_chan_put(goc_chan* ch, void* val)
{
    return goc_in_fiber() ? goc_put(ch, val) : goc_put_sync(ch, val);
}


#if GOC_HTTP_REUSEPORT_LINUX
static bool goc_http_reuseport_supported(int family)
{
    int fd = socket(family, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    int optval = 1;
    int rc = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
    int saved_errno = errno;
    close(fd);
    if (rc < 0) {
        GOC_DBG(
                "goc_http_reuseport_supported: SO_REUSEPORT unsupported family=%d errno=%d\n",
                family, saved_errno);
    }
    return rc == 0;
}
#endif

typedef struct {
    goc_chan* ready_ch;
    goc_chan* internal_ready_ch;
} goc_http_listen_ready_forward_arg_t;

static void goc_http_listen_ready_forward(void* arg)
{
    goc_http_listen_ready_forward_arg_t* f =
        (goc_http_listen_ready_forward_arg_t*)arg;
    goc_val_t* v = goc_http_chan_take(f->internal_ready_ch);
    int rc = UV_ECANCELED;
    if (v && v->ok == GOC_OK)
        rc = goc_unbox_int(v->val);
    goc_http_chan_put(f->ready_ch, goc_box_int(rc));
    goc_close(f->ready_ch);
    free(f);
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
    /* Single-listener path (n_listeners == 0): */
    uv_tcp_t*    tcp;          /* GC-allocated TCP listen handle */
    goc_chan*    accept_ch;    /* from goc_io_tcp_server_make */

    /* Multi-listener path (SO_REUSEPORT, n_listeners > 0): */
    uv_tcp_t**   listener_tcps;                    /* [n_listeners] */
    goc_chan**   listener_accept_chs;              /* [n_listeners] */
#if GOC_ENABLE_STATS
    atomic_int*  listener_accept_counts;           /* [n_listeners] accept count */
#endif
    size_t       n_listeners;

    goc_chan*    close_ch;     /* broadcast shutdown to live connection fibers */

    _Atomic int  active_conns; /* count of in-flight connection fibers */
    goc_chan*    shutdown_ch;  /* closed when active_conns drains to 0 */

    goc_route_t* routes;
    size_t       n_routes;
    size_t       cap_routes;

    goc_array* middleware;
};

typedef enum {
    HTTP_CONN_CONNECTING,
    HTTP_CONN_IN_USE,
    HTTP_CONN_IDLE,
    HTTP_CONN_CLOSING,
    HTTP_CONN_CLOSED,
} http_conn_state_t;

typedef struct http_conn {
    uv_tcp_t*         tcp;
    http_conn_state_t state;
    bool              timed_out;
    char*             host;
    uint16_t          port;
    uint64_t          last_used_ms;
    struct http_conn* next;
} http_conn_t;

typedef struct http_client_arg_t http_client_arg_t;

static _Atomic int      g_http_total_connections = 0; /* CONNECTING + IN_USE + IDLE */
static _Atomic int      g_http_idle_connections = 0;
static _Atomic int      g_http_client_inflight = 0;
static _Atomic uint64_t g_http_client_req_seq = 0;
static _Atomic uint64_t g_http_wakeup_version = 0;
static _Atomic int      g_http_slot_waiters    = 0;

static void http_client_assert_totals(void);
static void http_conn_assert_state(http_conn_t* conn,
                                   http_conn_state_t expected);

typedef struct {
    goc_pool*    pool;
    int          worker_id;
    http_conn_t* idle_stack;
    goc_chan*    wait_ch;
    uv_mutex_t   idle_mutex; /* protects idle_stack; acquired independently of g_http_worker_pools_mutex */
} http_worker_pool_t;

static _Thread_local http_worker_pool_t* g_http_worker_pool = NULL;
static http_worker_pool_t** g_http_worker_pools = NULL;
static size_t g_http_worker_pools_len = 0;
static size_t g_http_worker_pools_cap = 0;
static uv_mutex_t g_http_worker_pools_mutex;
static _Atomic int g_http_worker_pools_mutex_init = 0;
static _Atomic int g_http_lifecycle_hook_registered = 0;

static _Atomic(goc_chan*) g_http_sweeper_ch = NULL;
static _Atomic int g_http_sweeper_started = 0;

static const int      GOC_HTTP_MAX_CONNECTIONS   = 64;
static const uint64_t GOC_HTTP_IDLE_TIMEOUT_MS   = 30000;
static const uint64_t GOC_HTTP_QUEUE_TIMEOUT_MS  = 10000;
static const uint64_t GOC_HTTP_CONNECT_TIMEOUT_MS = 3000;

static void http_client_cleanup_worker_pool(http_worker_pool_t* wp)
{
    if (!wp)
        return;

    http_conn_t* conn = wp->idle_stack;
    while (conn) {
        http_conn_t* next = conn->next;
        if (conn->tcp)
            goc_io_handle_close((uv_handle_t*)conn->tcp);
        if (conn->host)
            free(conn->host);
        free(conn);
        conn = next;
    }
    wp->idle_stack = NULL;
    if (g_http_worker_pool == wp)
        g_http_worker_pool = NULL;

    if (wp->wait_ch) {
        goc_close(wp->wait_ch);
        wp->wait_ch = NULL;
    }
}

static void http_client_close_wait_chs(void)
{
    if (!atomic_load_explicit(&g_http_worker_pools_mutex_init,
                              memory_order_acquire)) {
        return;
    }

    uv_mutex_lock(&g_http_worker_pools_mutex);
    for (size_t i = 0; i < g_http_worker_pools_len; i++) {
        http_worker_pool_t* wp = g_http_worker_pools[i];
        if (!wp || !wp->wait_ch)
            continue;

        goc_close(wp->wait_ch);
        wp->wait_ch = NULL;
    }
    uv_mutex_unlock(&g_http_worker_pools_mutex);
}

static void http_client_close_sweeper_ch(void)
{
    goc_chan* ch = atomic_load_explicit(&g_http_sweeper_ch,
                                        memory_order_acquire);
    if (!ch)
        return;

    if (atomic_compare_exchange_strong_explicit(&g_http_sweeper_ch,
                                                &ch,
                                                NULL,
                                                memory_order_acq_rel,
                                                memory_order_relaxed)) {
        goc_close(ch);
    }
}

static void http_client_cleanup_pool_entries(goc_pool* pool)
{
    if (!atomic_load_explicit(&g_http_worker_pools_mutex_init,
                              memory_order_acquire)) {
        return;
    }

    uv_mutex_lock(&g_http_worker_pools_mutex);
    for (size_t i = 0; i < g_http_worker_pools_len; i++) {
        http_worker_pool_t* wp = g_http_worker_pools[i];
        if (!wp || wp->pool != pool)
            continue;

        http_client_cleanup_worker_pool(wp);
        uv_mutex_destroy(&wp->idle_mutex);
        free(wp);
        g_http_worker_pools[i] = NULL;
    }
    uv_mutex_unlock(&g_http_worker_pools_mutex);
}

static void http_client_close_wait_chs_hook(void* ub)
{
    goc_pool* pool = (goc_pool*)ub;
    http_client_close_wait_chs();
    http_client_close_sweeper_ch();
    http_client_cleanup_pool_entries(pool);
}

void goc_http_reset_globals(void* ub)
{
    (void)ub;
    if (atomic_load_explicit(&g_http_worker_pools_mutex_init,
                              memory_order_acquire)) {
        uv_mutex_lock(&g_http_worker_pools_mutex);
        for (size_t i = 0; i < g_http_worker_pools_len; i++) {
            if (g_http_worker_pools[i]) {
                http_client_cleanup_worker_pool(g_http_worker_pools[i]);
                free(g_http_worker_pools[i]);
            }
        }
        free(g_http_worker_pools);
        g_http_worker_pools = NULL;
        g_http_worker_pools_len = 0;
        g_http_worker_pools_cap = 0;
        uv_mutex_unlock(&g_http_worker_pools_mutex);
        uv_mutex_destroy(&g_http_worker_pools_mutex);
        atomic_store_explicit(&g_http_worker_pools_mutex_init,
                              0,
                              memory_order_release);
    }

    goc_chan* sweeper_ch = atomic_load_explicit(&g_http_sweeper_ch,
                                                  memory_order_acquire);
    if (sweeper_ch) {
        goc_close(sweeper_ch);
        atomic_store_explicit(&g_http_sweeper_ch,
                              NULL,
                              memory_order_release);
    }
    atomic_store_explicit(&g_http_sweeper_started,
                          0,
                          memory_order_release);

    atomic_store_explicit(&g_http_total_connections,
                          0,
                          memory_order_release);
    atomic_store_explicit(&g_http_idle_connections,
                          0,
                          memory_order_release);
    atomic_store_explicit(&g_http_client_inflight,
                          0,
                          memory_order_release);
    atomic_store_explicit(&g_http_client_req_seq,
                          0,
                          memory_order_release);
    g_http_worker_pool = NULL;
}

/* =========================================================================
 * Forward declarations
 * ====================================================================== */
static void accept_loop_fiber(void* arg);
static void accept_fd_transfer_fiber(void* arg);
static void handle_conn_fiber(void* arg);
static void http_accept_loop(goc_http_server_t* srv,
                             goc_chan* accept_ch,
                             goc_chan* close_ch,
                             int fallback,
                             size_t slot);
#if GOC_HTTP_REUSEPORT_LINUX
static void reuseport_accept_loop_fiber(void* arg);
#endif

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
        case 408: return "Request Timeout";
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

    srv->middleware = opts ? opts->middleware : NULL;
    srv->close_ch   = goc_chan_make(0);
    goc_chan_set_debug_tag(srv->close_ch, "http_close_ch");
    srv->active_conns = 0;
    srv->shutdown_ch  = goc_chan_make(0);
    goc_chan_set_debug_tag(srv->shutdown_ch, "http_shutdown_ch");

    srv->cap_routes = 8;
    srv->routes     =
        (goc_route_t*)goc_malloc(srv->cap_routes * sizeof(goc_route_t));

    return srv;
}

/* Arg passed to each reuseport_accept_loop_fiber. */
#ifdef GOC_HTTP_REUSEPORT
typedef struct {
    goc_http_server_t*     srv;
    struct sockaddr_storage addr;
    socklen_t               addrlen;
    size_t                  slot;         /* index into listener_* arrays */
    goc_chan*               slot_ready_ch;/* per-fiber: delivers rc when uv_listen returns */
} reuseport_accept_arg_t;
#endif

goc_chan* goc_http_server_listen(goc_http_server_t* srv, const char* host, int port)
{
    goc_chan* ready_ch = goc_chan_make(1);

    /* Resolve bind address. */
    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));
    int r = uv_ip4_addr(host, port, (struct sockaddr_in*)&addr);
    socklen_t addrlen = sizeof(struct sockaddr_in);
    if (r < 0) {
        r = uv_ip6_addr(host, port, (struct sockaddr_in6*)&addr);
        addrlen = sizeof(struct sockaddr_in6);
    }
    if (r < 0) {
        goc_http_chan_put(ready_ch, goc_box_int(r));
        goc_close(ready_ch);
        return ready_ch;
    }

    goc_pool* pool = goc_current_or_default_pool();
    GOC_DBG(
            "goc_http_server_listen: srv=%p host=%s port=%d pool=%p nw_pending=%zu\n",
            (void*)srv, host, port, (void*)pool, goc_pool_thread_count(pool));

#if GOC_HTTP_REUSEPORT_LINUX
    /* SO_REUSEPORT path: one listener per worker, kernel load-balances. */
    size_t nw = goc_pool_thread_count(pool);
    int use_reuseport = nw > 1 && goc_http_reuseport_supported(addr.ss_family);
    if (nw > 1 && !use_reuseport) {
        GOC_DBG(
                "goc_http_server_listen: SO_REUSEPORT unsupported at runtime; falling back to single listener\n");
    }
    if (use_reuseport) {
        GOC_DBG(
                "goc_http_server_listen: SO_REUSEPORT path using %zu listeners\n",
                nw);
        srv->n_listeners          = nw;
        srv->listener_tcps        = (uv_tcp_t**)goc_malloc(nw * sizeof(uv_tcp_t*));
        srv->listener_accept_chs  = (goc_chan**)goc_malloc(nw * sizeof(goc_chan*));
#if GOC_ENABLE_STATS
        srv->listener_accept_counts =
            (atomic_int*)malloc(nw * sizeof(atomic_int));
#endif
        memset(srv->listener_tcps,       0, nw * sizeof(uv_tcp_t*));
        memset(srv->listener_accept_chs, 0, nw * sizeof(goc_chan*));
#if GOC_ENABLE_STATS
        for (size_t i = 0; i < nw; i++) {
            atomic_init(&srv->listener_accept_counts[i], 0);
        }
#endif

        /* Per-fiber ready channels. */
        goc_chan** slot_ready_chs = (goc_chan**)malloc(nw * sizeof(goc_chan*));
        for (size_t i = 0; i < nw; i++) {
            slot_ready_chs[i] = goc_chan_make(1);
            reuseport_accept_arg_t* a =
                (reuseport_accept_arg_t*)goc_malloc(sizeof(reuseport_accept_arg_t));
            a->srv           = srv;
            a->addr          = addr;
            a->addrlen       = addrlen;
            a->slot          = i;
            a->slot_ready_ch = slot_ready_chs[i];
            goc_go_on_worker(pool, i, reuseport_accept_loop_fiber, a);
        }

        /* Pin the server struct in the GC while accept loop fibers run. */
        gc_handle_register(srv);

        /* Collect per-fiber ready signals; forward first error (or 0). */
        int first_err = 0;
        for (size_t i = 0; i < nw; i++) {
            goc_val_t* v = goc_http_chan_take(slot_ready_chs[i]);
            int rc = (v && v->ok == GOC_OK) ? goc_unbox_int(v->val) : UV_ECANCELED;
            if (rc < 0 && first_err == 0)
                first_err = rc;
        }

        if (first_err < 0) {
            GOC_DBG(
                    "goc_http_server_listen: reuseport listen failed first_err=%d, falling back to single listener\n",
                    first_err);
            for (size_t i = 0; i < nw; i++) {
                if (srv->listener_accept_chs && srv->listener_accept_chs[i])
                    goc_close(srv->listener_accept_chs[i]);
                if (srv->listener_tcps && srv->listener_tcps[i])
                    goc_io_handle_close((uv_handle_t*)srv->listener_tcps[i]);
            }
            srv->listener_accept_chs = NULL;
            srv->listener_tcps = NULL;
            srv->n_listeners = 0;
#if GOC_ENABLE_STATS
            if (srv->listener_accept_counts) {
                free(srv->listener_accept_counts);
                srv->listener_accept_counts = NULL;
            }
#endif
            free(slot_ready_chs);
        } else {
            free(slot_ready_chs);
            GOC_DBG(
                    "goc_http_server_listen: reuseport listeners ready first_err=%d\n",
                    first_err);
            goc_http_chan_put(ready_ch, goc_box_int(first_err));
            goc_close(ready_ch);
            return ready_ch;
        }
    }
#endif

    /* Single-listener fallback (pool_size == 1 or no SO_REUSEPORT). */
    srv->tcp = (uv_tcp_t*)goc_malloc(sizeof(uv_tcp_t));
    r = goc_unbox_int(goc_http_chan_take(goc_io_tcp_init(srv->tcp))->val);
    if (r < 0) {
        srv->tcp = NULL;
        goc_http_chan_put(ready_ch, goc_box_int(r));
        goc_close(ready_ch);
        return ready_ch;
    }

    r = goc_unbox_int(
            goc_http_chan_take(goc_io_tcp_bind(srv->tcp,
                                     (const struct sockaddr*)&addr))->val);
    if (r < 0) {
        GOC_DBG(
            "goc_http_server_listen: tcp_bind failed tcp=%p r=%d\n",
            (void*)srv->tcp, r);
        goc_io_handle_close((uv_handle_t*)srv->tcp);
        srv->tcp = NULL;
        goc_http_chan_put(ready_ch, goc_box_int(r));
        goc_close(ready_ch);
        return ready_ch;
    }

    GOC_DBG(
            "goc_http_server_listen: tcp_bind succeeded tcp=%p, creating accept_ch\n",
            (void*)srv->tcp);
    /* Start listening; internal_ready_ch will receive goc_box_int(0) when uv_listen
     * has been called on the event-loop thread, or UV_ECANCELED if that channel
     * closes unexpectedly. */
    goc_chan* internal_ready_ch = goc_chan_make(1);
    srv->accept_ch = goc_io_tcp_server_make(srv->tcp, 128, internal_ready_ch);
    if (srv->accept_ch)
        goc_chan_set_debug_tag(srv->accept_ch, "http_accept_ch");

    /* Pin the server struct in the GC while the accept loop fiber runs. */
    gc_handle_register(srv);

    /* Forward the internal ready result into the externally returned channel.
     * This guarantees callers always get an explicit rc or UV_ECANCELED. */
    goc_http_listen_ready_forward_arg_t* f =
        (goc_http_listen_ready_forward_arg_t*)malloc(
            sizeof(goc_http_listen_ready_forward_arg_t));
    f->ready_ch = ready_ch;
    f->internal_ready_ch = internal_ready_ch;
    goc_go_on(pool, goc_http_listen_ready_forward, f);

    /* Spawn the single accept loop fiber. */
    goc_go_on(pool, accept_loop_fiber, srv);

    return ready_ch;
}

int goc_http_server_reuseport_listener_count(goc_http_server_t* srv)
{
    return srv ? (int)srv->n_listeners : 0;
}

int goc_http_server_reuseport_listener_accept_count(
        goc_http_server_t* srv, int slot)
{
#if GOC_ENABLE_STATS
    if (!srv || slot < 0 || (size_t)slot >= srv->n_listeners ||
        !srv->listener_accept_counts)
        return 0;
    return atomic_load_explicit(&srv->listener_accept_counts[slot],
                                memory_order_relaxed);
#else
    (void)srv;
    (void)slot;
    return 0;
#endif
}

goc_chan* goc_http_server_close(goc_http_server_t* srv)
{
    goc_chan* ch = goc_chan_make(1);

    GOC_DBG(
            "goc_http_server_close: srv=%p tcp=%p accept_ch=%p n_listeners=%zu close_ch=%p\n",
            (void*)srv, (void*)srv->tcp, (void*)srv->accept_ch,
            srv->n_listeners, (void*)srv->close_ch);

    if (srv->close_ch) {
        GOC_DBG("goc_http_server_close: closing close_ch=%p\n", (void*)srv->close_ch);
        goc_close(srv->close_ch);
    }

    int was_listening = 0;

    if (srv->n_listeners > 0) {
        /* SO_REUSEPORT multi-listener path: close all accept channels and TCP handles. */
        was_listening = 1;
        for (size_t i = 0; i < srv->n_listeners; i++) {
            if (srv->listener_accept_chs && srv->listener_accept_chs[i]) {
                GOC_DBG(
                    "goc_http_server_close: closing listener_accept_chs[%zu]=%p\n",
                    i, (void*)srv->listener_accept_chs[i]);
                goc_close(srv->listener_accept_chs[i]);
            }
            if (srv->listener_tcps && srv->listener_tcps[i]) {
                GOC_DBG(
                    "goc_http_server_close: closing listener_tcp[%zu]=%p\n",
                    i, (void*)srv->listener_tcps[i]);
                goc_io_handle_close((uv_handle_t*)srv->listener_tcps[i]);
            }
        }
    } else {
        /* Single-listener path. */
        was_listening = (srv->accept_ch != NULL);
        if (srv->accept_ch) {
            GOC_DBG("goc_http_server_close: closing accept_ch=%p\n", (void*)srv->accept_ch);
            goc_close(srv->accept_ch);
        }
        if (srv->tcp) {
            GOC_DBG("goc_http_server_close: closing tcp=%p\n", (void*)srv->tcp);
            goc_io_handle_close((uv_handle_t*)srv->tcp);
        }
    }

    GOC_DBG(
            "goc_http_server_close: begin drain was_listening=%d accept_ch=%p active_conns=%d shutdown_ch=%p\n",
            was_listening,
            (void*)srv->accept_ch,
            atomic_load(&srv->active_conns),
            (void*)srv->shutdown_ch);
    GOC_DBG(
            "goc_http_server_close: close dispatch complete accept_ch=%p active_conns=%d shutdown_ch=%p\n",
            (void*)srv->accept_ch,
            atomic_load(&srv->active_conns),
            (void*)srv->shutdown_ch);

    /* Wait for all in-flight connection fibers (and accept loop fibers) to
     * finish.  Each accept fiber counts itself in active_conns, so if listen
     * was ever called active_conns is guaranteed > 0 until all accept loops
     * exit — we can unconditionally goc_take.  If listen was never called
     * nobody will close shutdown_ch, so we close it ourselves. */
    if (was_listening) {
        GOC_DBG(
                "goc_http_server_close: [PRE WAIT] accept_ch=%p active_conns=%d shutdown_ch=%p\n",
                (void*)srv->accept_ch,
                atomic_load(&srv->active_conns),
                (void*)srv->shutdown_ch);
        goc_val_t* v = goc_http_chan_take(srv->shutdown_ch);
        GOC_DBG(
                "goc_http_server_close: [WAIT DONE] accept_ch=%p active_conns=%d shutdown_ch=%p v=%p ok=%d\n",
                (void*)srv->accept_ch,
                atomic_load(&srv->active_conns),
                (void*)srv->shutdown_ch,
                (void*)v, v ? (int)v->ok : -1);
        GOC_DBG(
                "goc_http_server_close: [POST WAIT] accept_ch=%p active_conns=%d shutdown_ch=%p\n",
                (void*)srv->accept_ch,
                atomic_load(&srv->active_conns),
                (void*)srv->shutdown_ch);
    } else {
        GOC_DBG(
                "goc_http_server_close: closing shutdown_ch (context: server close, not listening)\n");
        goc_close(srv->shutdown_ch);
        GOC_DBG(
                "goc_http_server_close: shutdown_ch closed (context: server close, not listening)\n");
    }
    srv->shutdown_ch = NULL;

    if (srv->n_listeners == 0) {
        srv->accept_ch = NULL;
        srv->tcp = NULL;
    } else if (srv->listener_accept_chs) {
        for (size_t i = 0; i < srv->n_listeners; i++) {
            srv->listener_accept_chs[i] = NULL;
            srv->listener_tcps[i] = NULL;
        }
    }
#if GOC_ENABLE_STATS
    if (srv->listener_accept_counts) {
        free(srv->listener_accept_counts);
        srv->listener_accept_counts = NULL;
    }
#endif
    srv->close_ch = NULL;

    /* Unpin the server from the GC root list. */
    gc_handle_unregister(srv);

    GOC_DBG("goc_http_server_close: completed srv=%p\n", (void*)srv);
    goc_http_chan_put(ch, goc_box_int(0));
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
    size_t        slot;
} conn_arg_t;

#if GOC_HTTP_REUSEPORT_LINUX
/* Each reuseport_accept_loop_fiber:
 *  1. Inits its own uv_tcp_t on the current worker's loop (Phase 2 direct path).
 *  2. Sets SO_REUSEPORT on the socket via setsockopt.
 *  3. Binds and listens.
 *  4. Signals slot_ready_ch with the listen rc.
 *  5. Runs its own accept loop, spawning connection fibers.
 *
 * Because the handle is on this worker's loop all I/O is local — no
 * cross-thread dispatch needed.
 */
static void reuseport_accept_loop_fiber(void* arg)
{
    reuseport_accept_arg_t* a   = (reuseport_accept_arg_t*)arg;
    goc_http_server_t*      srv = a->srv;
    size_t                  slot = a->slot;
    goc_chan*               slot_ready_ch = a->slot_ready_ch;
    goc_chan*               close_ch = srv->close_ch;
    GOC_DBG(
            "reuseport_accept_loop_fiber[%zu]: current_worker=%d loop=%p srv=%p close_ch=%p slot_ready_ch=%p\n",
            slot, goc_current_worker_id(), (void*)goc_current_worker_loop(), (void*)srv,
            (void*)close_ch, (void*)slot_ready_ch);

    /* Init TCP handle on this worker's own loop. */
    uv_tcp_t* tcp = (uv_tcp_t*)goc_malloc(sizeof(uv_tcp_t));
    int r = goc_unbox_int(goc_take(goc_io_tcp_init(tcp))->val);
    if (r < 0) {
        goc_http_chan_put(slot_ready_ch, goc_box_int(r));
        goc_close(slot_ready_ch);
        return;
    }

    /* Set socket reuse options before bind so the kernel can load-balance
     * across all N listeners bound to the same port. */
    uv_os_sock_t rawfd = socket(a->addr.ss_family, SOCK_STREAM, 0);
#if defined(_WIN32)
    if (rawfd == INVALID_SOCKET) {
        int eno = WSAGetLastError();
        GOC_DBG(
                "reuseport_accept_loop_fiber[%zu]: socket failed family=%d errno=%d\n",
                slot, a->addr.ss_family, eno);
        goc_io_handle_close((uv_handle_t*)tcp);
        goc_http_chan_put(slot_ready_ch, goc_box_int(-eno));
        goc_close(slot_ready_ch);
        return;
    }
#else
    if (rawfd < 0) {
        int eno = errno;
        GOC_DBG(
                "reuseport_accept_loop_fiber[%zu]: socket failed family=%d errno=%d\n",
                slot, a->addr.ss_family, eno);
        goc_io_handle_close((uv_handle_t*)tcp);
        goc_http_chan_put(slot_ready_ch, goc_box_int(-eno));
        goc_close(slot_ready_ch);
        return;
    }
#endif

    int optval = 1;
#if defined(_WIN32)
    if (setsockopt((SOCKET)rawfd, SOL_SOCKET, SO_REUSEADDR,
                   (const char*)&optval, sizeof(optval)) != 0) {
        int eno = WSAGetLastError();
        GOC_DBG(
                "reuseport_accept_loop_fiber[%zu]: setsockopt(SO_REUSEADDR) failed fd=%llu errno=%d\n",
                slot, (unsigned long long)rawfd, eno);
    }
    if (setsockopt((SOCKET)rawfd, SOL_SOCKET, SO_REUSEPORT,
                   (const char*)&optval, sizeof(optval)) != 0) {
        int eno = WSAGetLastError();
        GOC_DBG(
                "reuseport_accept_loop_fiber[%zu]: setsockopt(SO_REUSEPORT) failed fd=%llu errno=%d\n",
                slot, (unsigned long long)rawfd, eno);
        closesocket((SOCKET)rawfd);
        goc_io_handle_close((uv_handle_t*)tcp);
        goc_http_chan_put(slot_ready_ch, goc_box_int(-eno));
        goc_close(slot_ready_ch);
        return;
    }
#else
    if (setsockopt((int)rawfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        int eno = errno;
        GOC_DBG(
                "reuseport_accept_loop_fiber[%zu]: setsockopt(SO_REUSEADDR) failed fd=%d errno=%d\n",
                slot, (int)rawfd, eno);
    }
    if (setsockopt((int)rawfd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) < 0) {
        int eno = errno;
        GOC_DBG(
                "reuseport_accept_loop_fiber[%zu]: setsockopt(SO_REUSEPORT) failed fd=%d errno=%d\n",
                slot, (int)rawfd, eno);
        close(rawfd);
        goc_io_handle_close((uv_handle_t*)tcp);
        goc_http_chan_put(slot_ready_ch, goc_box_int(-eno));
        goc_close(slot_ready_ch);
        return;
    }
#endif

    int rc = uv_tcp_open(tcp, rawfd);
    if (rc < 0) {
        GOC_DBG(
                "reuseport_accept_loop_fiber[%zu]: uv_tcp_open failed tcp=%p rawfd=%d rc=%d\n",
                slot, (void*)tcp, (int)rawfd, rc);
        close(rawfd);
        goc_io_handle_close((uv_handle_t*)tcp);
        goc_http_chan_put(slot_ready_ch, goc_box_int(rc));
        goc_close(slot_ready_ch);
        return;
    }

    /* Bind. */
    r = goc_unbox_int(
            goc_take(goc_io_tcp_bind(tcp, (const struct sockaddr*)&a->addr))->val);
    if (r < 0) {
        GOC_DBG(
                "reuseport_accept_loop_fiber[%zu]: bind failed tcp=%p addr_family=%d rc=%d\n",
                slot, (void*)tcp, a->addr.ss_family, r);
        goc_io_handle_close((uv_handle_t*)tcp);
        goc_http_chan_put(slot_ready_ch, goc_box_int(r));
        goc_close(slot_ready_ch);
        return;
    }

    /* Listen — slot_ready_ch receives rc when uv_listen returns. */
    GOC_DBG(
            "reuseport_accept_loop_fiber[%zu]: starting tcp=%p slot_ready_ch=%p close_ch=%p\n",
            slot, (void*)tcp, (void*)slot_ready_ch, (void*)close_ch);
    goc_chan* accept_ch = goc_io_tcp_server_make(tcp, 128, slot_ready_ch);
    if (accept_ch)
        goc_chan_set_debug_tag(accept_ch, "http_listener_accept_ch");
    GOC_DBG(
            "reuseport_accept_loop_fiber[%zu]: accept_ch=%p tcp=%p loop=%p current_worker=%d\n",
            slot, (void*)accept_ch, (void*)tcp, (void*)tcp->loop, goc_current_worker_id());

    /* Store in the server's listener arrays for the close path. */
    srv->listener_tcps[slot]       = tcp;
    srv->listener_accept_chs[slot] = accept_ch;

    if (close_ch && !goc_chan_is_open(close_ch)) {
        GOC_DBG(
                "reuseport_accept_loop_fiber[%zu]: server already closing, closing accept_ch=%p tcp=%p\n",
                slot, (void*)accept_ch, (void*)tcp);
        goc_close(accept_ch);
        goc_io_handle_close((uv_handle_t*)tcp);
        return;
    }

    GOC_DBG(
            "reuseport_accept_loop_fiber[%zu]: accept_ch=%p installed tcp=%p\n",
            slot, (void*)accept_ch, (void*)tcp);
    http_accept_loop(srv, accept_ch, close_ch, 0, slot);

}
#endif

/* On Windows uv_os_fd_t is HANDLE (void*); on POSIX it is int.
 * goc_io_tcp_open takes uv_os_fd_t, so we carry uv_os_fd_t throughout
 * and do platform casts only inside these helpers. */

static void sock_close_raw(uv_os_fd_t fd)
{
#if defined(_WIN32)
    closesocket((SOCKET)(uintptr_t)fd);
#else
    close((int)fd);
#endif
}

static int sock_is_valid(uv_os_fd_t fd)
{
#if defined(_WIN32)
    return (SOCKET)(uintptr_t)fd != INVALID_SOCKET;
#else
    return (int)fd >= 0;
#endif
}

/* Duplicate a socket so the copy can be opened on a different event loop.
 * Returns the platform invalid sentinel on failure. */
static uv_os_fd_t sock_dup(uv_os_fd_t fd)
{
#if defined(_WIN32)
    WSAPROTOCOL_INFOW proto_info;
    if (WSADuplicateSocketW((SOCKET)(uintptr_t)fd,
                            GetCurrentProcessId(), &proto_info) != 0)
        return (uv_os_fd_t)(uintptr_t)INVALID_SOCKET;
    SOCKET s = WSASocketW(proto_info.iAddressFamily,
                          proto_info.iSocketType,
                          proto_info.iProtocol,
                          &proto_info, 0, WSA_FLAG_OVERLAPPED);
    return (uv_os_fd_t)(uintptr_t)s;
#else
    return (uv_os_fd_t)dup((int)fd);
#endif
}

/* Carries a dup'd socket fd to a target worker for re-opening on that
 * worker's loop, so epoll/kqueue ownership moves off the accept worker. */
typedef struct {
    goc_http_server_t* srv;
    uv_os_fd_t         fd;
    goc_chan*          close_ch;
} accept_fd_arg_t;

static void handle_conn_fiber(void* arg);

static void http_dispatch_conn(goc_http_server_t* srv, goc_pool* pool,
                               size_t target_worker, uv_tcp_t* conn,
                               goc_chan* close_ch, size_t slot)
{
    conn_arg_t* a = (conn_arg_t*)goc_malloc(sizeof(conn_arg_t));
    a->srv      = srv;
    a->conn     = conn;
    a->close_ch = close_ch;
    a->slot     = slot;
    goc_go_on_worker(pool, target_worker, handle_conn_fiber, a);
}

/* Dispatch an accepted connection to target_worker with load-balancing.
 *
 * On Linux: dup the socket fd and re-open it on the target worker's epoll so
 * all server-side I/O runs on that worker's event loop.  epoll supports a dup'd
 * fd being added to a second epoll instance once the original is removed.
 *
 * On macOS/Windows: fall back to http_dispatch_conn (no fd move).  I/O crosses
 * back to the accept worker via goc_io cross-loop dispatch.  fd-transfer is
 * unsupported there: kqueue ties events to the fd-number and fires spurious
 * events when two instances watch the same socket; IOCP allows only one
 * completion-port association per socket. */
static int http_accept_dispatch(goc_http_server_t* srv,
                                goc_pool*           pool,
                                size_t              target_worker,
                                uv_tcp_t*           conn,
                                goc_chan*           close_ch)
{
#if defined(__linux__)
    uv_os_fd_t raw_fd;
    if (uv_fileno((const uv_handle_t*)conn, &raw_fd) < 0) {
        goc_io_handle_close((uv_handle_t*)conn);
        return -1;
    }
    uv_os_fd_t dup_fd = sock_dup(raw_fd);
    if (!sock_is_valid(dup_fd)) {
        goc_io_handle_close((uv_handle_t*)conn);
        return -1;
    }
    goc_io_handle_close((uv_handle_t*)conn);

    accept_fd_arg_t* fa = (accept_fd_arg_t*)malloc(sizeof(*fa));
    fa->srv      = srv;
    fa->fd       = dup_fd;
    fa->close_ch = close_ch;
    goc_go_on_worker(pool, target_worker, accept_fd_transfer_fiber, fa);
#else
    http_dispatch_conn(srv, pool, target_worker, conn, close_ch, SIZE_MAX);
#endif
    return 0;
}

static void accept_fd_transfer_fiber(void* arg)
{
    accept_fd_arg_t*   a        = (accept_fd_arg_t*)arg;
    goc_http_server_t* srv      = a->srv;
    uv_os_fd_t         raw_fd   = a->fd;
    goc_chan*          close_ch = a->close_ch;
    free(a);

    uv_tcp_t* tcp = (uv_tcp_t*)goc_malloc(sizeof(uv_tcp_t));
    int r = goc_unbox_int(goc_take(goc_io_tcp_init(tcp))->val);
    if (r < 0) {
        GOC_DBG("accept_fd_transfer_fiber: tcp_init failed rc=%d\n", r);
        sock_close_raw(raw_fd);
        if (atomic_fetch_sub_explicit(&srv->active_conns, 1,
                                      memory_order_acq_rel) == 1)
            goc_close(srv->shutdown_ch);
        return;
    }

    r = goc_unbox_int(goc_take(goc_io_tcp_open(tcp, raw_fd))->val);
    if (r < 0) {
        GOC_DBG("accept_fd_transfer_fiber: tcp_open failed rc=%d\n", r);
        sock_close_raw(raw_fd);
        goc_io_handle_close((uv_handle_t*)tcp);
        if (atomic_fetch_sub_explicit(&srv->active_conns, 1,
                                      memory_order_acq_rel) == 1)
            goc_close(srv->shutdown_ch);
        return;
    }

    conn_arg_t conn_a;
    conn_a.srv      = srv;
    conn_a.conn     = tcp;
    conn_a.close_ch = close_ch;
    conn_a.slot     = SIZE_MAX;
    handle_conn_fiber(&conn_a);
}

static void accept_loop_fiber(void* arg)
{
    goc_http_server_t* srv      = (goc_http_server_t*)arg;
    goc_chan*     accept_ch = srv->accept_ch; /* cache before any yield point */
    goc_chan*     close_ch  = srv->close_ch;  /* cache before any yield point */
    http_accept_loop(srv, accept_ch, close_ch, 1, SIZE_MAX);
}

static void http_accept_loop(goc_http_server_t* srv,
                             goc_chan* accept_ch,
                             goc_chan* close_ch,
                             int fallback,
                             size_t slot)
{
    if (!accept_ch) {
        GOC_DBG("http_accept_loop: no accept_ch, closing shutdown_ch=%p\n",
                (void*)srv->shutdown_ch);
        goc_close(srv->shutdown_ch);
        return;
    }

    atomic_fetch_add_explicit(&srv->active_conns, 1, memory_order_release);
    for (;;) {
        goc_val_t* v = goc_take(accept_ch);
        GOC_DBG(
                "http_accept_loop: accept_ch=%p returned v=%p ok=%d active_conns=%d\n",
                (void*)accept_ch,
                (void*)v,
                v ? (int)v->ok : -1,
                atomic_load(&srv->active_conns));
        if (!v || v->ok != GOC_OK) {
            GOC_DBG(
                    "http_accept_loop: accept_ch=%p closed or error, exiting active_conns=%d\n",
                    (void*)accept_ch,
                    atomic_load(&srv->active_conns));
            break;
        }

        uv_tcp_t* conn = (uv_tcp_t*)v->val;
        GOC_DBG(
                "http_accept_loop: accepted conn=%p active_conns=%d\n",
                (void*)conn,
                atomic_load(&srv->active_conns));

#if GOC_ENABLE_STATS
        if (slot != SIZE_MAX && srv->listener_accept_counts)
            atomic_fetch_add_explicit(&srv->listener_accept_counts[slot],
                                      1, memory_order_relaxed);
#endif

        atomic_fetch_add_explicit(&srv->active_conns, 1, memory_order_release);

        goc_pool* pool = goc_current_or_default_pool();
        size_t worker_count = goc_pool_thread_count(pool);
        size_t target_worker = goc_current_worker_id();
        if (worker_count > 1 && fallback) {
            static size_t next_target = 0;
            target_worker = next_target % worker_count;
            next_target++;
        }

        GOC_DBG(
                "http_accept_loop: scheduling conn on worker=%zu pool=%p conn=%p active_conns=%d\n",
                target_worker,
                (void*)pool,
                (void*)conn,
                atomic_load(&srv->active_conns));

        if (worker_count > 1 && fallback) {
            if (http_accept_dispatch(srv, pool, target_worker, conn, close_ch) < 0) {
                if (atomic_fetch_sub_explicit(&srv->active_conns, 1,
                                              memory_order_acq_rel) == 1)
                    goc_close(srv->shutdown_ch);
            }
        } else {
            http_dispatch_conn(srv, pool, target_worker, conn, close_ch, slot);
        }
    }

    GOC_DBG(
            "http_accept_loop: draining buffered accept_ch=%p active_conns=%d\n",
            (void*)accept_ch,
            atomic_load(&srv->active_conns));
    for (;;) {
        goc_val_t* v = goc_take(accept_ch);
        if (!v || v->ok != GOC_OK)
            break;
        uv_tcp_t* conn = (uv_tcp_t*)v->val;
        GOC_DBG(
                "http_accept_loop: draining leftover conn=%p active_conns=%d\n",
                (void*)conn,
                atomic_load(&srv->active_conns));
        gc_handle_unregister(conn);
        goc_io_handle_close((uv_handle_t*)conn);
    }

    GOC_DBG(
            "http_accept_loop: completed drain accept_ch=%p active_conns=%d\n",
            (void*)accept_ch,
            atomic_load(&srv->active_conns));
    if (atomic_fetch_sub_explicit(&srv->active_conns, 1, memory_order_release) == 1)
        goc_close(srv->shutdown_ch);
}

/* Maximum raw request size before we abort the connection. */
#define GOC_SERVER_MAX_REQ_SIZE (8 * 1024 * 1024)
/* Maximum number of HTTP headers accepted per request. */
#define GOC_SERVER_MAX_HDRS     64

#define free_buf(buf) do {            \
    if (buf) {                          \
        free(buf);                      \
        (buf) = NULL;                   \
    }                                   \
} while (0)

static void handle_conn_fiber(void* arg)
{
    static _Atomic uint64_t s_conn_fiber_seq = 0;
    conn_arg_t*   a    = (conn_arg_t*)arg;
    goc_http_server_t* srv  = a->srv;
    uv_tcp_t*     conn = a->conn;
    goc_chan*     close_ch = a->close_ch;
    size_t        slot = a->slot;
    uint64_t cfid = atomic_fetch_add_explicit(&s_conn_fiber_seq, 1, memory_order_relaxed) + 1;
    GOC_DBG(
            "handle_conn_fiber[%llu]: entry conn=%p srv=%p close_ch=%p active_conns=%d current_worker=%d loop=%p conn_loop=%p slot=%zu\n",
            (unsigned long long)cfid, (void*)conn, (void*)srv, (void*)close_ch,
            atomic_load(&srv->active_conns), goc_current_worker_id(),
            (void*)goc_current_worker_loop(), (void*)conn->loop, slot);
    {
        struct sockaddr_storage peer;
            int plen = sizeof(peer);
            if (uv_tcp_getpeername(conn, (struct sockaddr*)&peer, &plen) == 0) {
                char ipstr[64] = {0};
                int port = 0;
                if (peer.ss_family == AF_INET) {
                    struct sockaddr_in* s = (struct sockaddr_in*)&peer;
                    uv_inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof(ipstr));
                    port = ntohs(s->sin_port);
                }
                    }
        }


    /* Accumulation buffer (plain-malloc; we control its entire lifetime). */
    size_t buf_cap = 4096;
    char*  buf     = NULL;
    struct phr_header* headers = NULL;

    buf = (char*)malloc(buf_cap);
    size_t req_iter = 0;
    if (!buf) {
        goc_io_handle_close((uv_handle_t*)conn);
        goto done;
    }

    /* Heap-allocate the header array: GOC_SERVER_MAX_HDRS * sizeof(phr_header)
     * = 64 * 32 = 2048 bytes.  Keeping this off the fiber's stack prevents
     * stack overflow under concurrent load (minicoro default stack is ~56 KB;
     * this array alone consumed ~4% of that, plus deep call chains during
     * goc_alts/goc_io_write pushed the total over the limit). */
    headers = (struct phr_header*)malloc(
                                       GOC_SERVER_MAX_HDRS * sizeof(struct phr_header));
    if (!headers) {
        goc_io_handle_close((uv_handle_t*)conn);
        goto done;
    }

    for (;;) {
        size_t buf_len = 0;
        int read_started = 0;
        int must_close_conn = 0;
        const char* must_close_reason = NULL;

        GOC_DBG(
                "handle_conn_fiber[%llu]: request loop start iter=%zu conn=%p close_ch=%p active_conns=%d\n",
                (unsigned long long)cfid,
                req_iter,
                (void*)conn,
                (void*)close_ch,
                atomic_load(&srv->active_conns));
        goc_chan* read_ch = goc_io_read_start((uv_stream_t*)conn);
        read_started = 1;
        GOC_DBG(
                "handle_conn_fiber[%llu]: started read on conn=%p read_ch=%p close_ch=%p\n",
                (unsigned long long)cfid,
                (void*)conn,
                (void*)read_ch,
                (void*)close_ch);

        /* ---- Read until we have a complete HTTP request head ---- */
        const char*       method        = NULL;
        size_t            method_len    = 0;
        const char*       path          = NULL;
        size_t            path_len      = 0;
        int               minor_version = 0;
        size_t            num_headers   = GOC_SERVER_MAX_HDRS;
        int               pret          = -2;

        while (pret == -2) {
            goc_alt_op_t ops[] = {
                { .ch = read_ch,  .op_kind = GOC_ALT_TAKE, .put_val = NULL },
                { .ch = close_ch, .op_kind = GOC_ALT_TAKE, .put_val = NULL },
            };
            goc_alts_result_t* sel = goc_alts(ops, 2);
        GOC_DBG(
                "handle_conn_fiber[%llu]: goc_alts returned sel=%p ch=%p ok=%d read_ch=%p close_ch=%p\n",
                (unsigned long long)cfid,
                (void*)sel,
                (void*)(sel ? (void*)sel->ch : NULL),
                sel ? (int)sel->value.ok : -1,
                (void*)read_ch,
                (void*)close_ch);
        if (!sel || sel->ch != read_ch || sel->value.ok != GOC_OK) {
            if (!sel) {
                must_close_reason = "alts_returned_null";
            } else if (sel->ch == close_ch) {
                must_close_reason = "close_ch_fired";
            } else if (sel->ch == read_ch && sel->value.ok != GOC_OK) {
                must_close_reason = "read_ch_closed_eof";
            } else {
                must_close_reason = "alts_unknown";
            }
            must_close_conn = 1;
            GOC_DBG(
                    "handle_conn_fiber[%llu]: closing on read wait reason=%s sel_ch=%p close_ch=%p read_ch=%p active_conns=%d\n",
                    (unsigned long long)cfid,
                    must_close_reason,
                    sel ? (void*)sel->ch : NULL,
                    (void*)close_ch,
                    (void*)read_ch,
                    atomic_load(&srv->active_conns));
            break;
        }
            goc_val_t* v = &sel->value;
            goc_io_read_t* r = (goc_io_read_t*)v->val;
            if (r->nread < 0) {
                must_close_conn = 1;
                must_close_reason = "nread<0";
                GOC_DBG("handle_conn_fiber[%llu]: read error nread=%zd reason=%s\n",
                        (unsigned long long)cfid, r->nread, must_close_reason);
                break;
            }

            size_t chunk = (size_t)r->nread;
            if (buf_len + chunk > GOC_SERVER_MAX_REQ_SIZE) {
                must_close_conn = 1;
                must_close_reason = "buffer_overflow";
                break;
            }
            if (buf_len + chunk > buf_cap) {
                size_t nc = buf_cap;
                while (nc < buf_len + chunk) nc *= 2;
                char* nb = (char*)realloc(buf, nc);
                if (!nb) {
                    must_close_conn = 1;
                    must_close_reason = "malloc_failed";
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
            must_close_reason = "bad_request";
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
            GOC_DBG("handle_conn_fiber[%llu]: request parsed keep_alive=%d content_length=%zd head_consumed=%zu body_end=%zu\n",
                    (unsigned long long)cfid,
                    keep_alive_req,
                    content_length,
                    head_consumed,
                    body_end);

            while (buf_len < body_end) {
                goc_alt_op_t ops[] = {
                    { .ch = read_ch,  .op_kind = GOC_ALT_TAKE, .put_val = NULL },
                    { .ch = close_ch, .op_kind = GOC_ALT_TAKE, .put_val = NULL },
                };
                goc_alts_result_t* sel = goc_alts(ops, 2);
                GOC_DBG("handle_conn_fiber[%llu]: body goc_alts returned sel=%p ch=%p ok=%d read_ch=%p close_ch=%p\n",
                        (unsigned long long)cfid,
                        (void*)sel,
                        (void*)(sel ? (void*)sel->ch : NULL),
                        sel ? (int)sel->value.ok : -1,
                        (void*)read_ch,
                        (void*)close_ch);
                if (!sel || sel->ch != read_ch || sel->value.ok != GOC_OK) {
                    if (!sel) {
                        must_close_reason = "body_alts_returned_null";
                    } else if (sel->ch == close_ch) {
                        must_close_reason = "body_close_ch_fired";
                    } else if (sel->ch == read_ch && sel->value.ok != GOC_OK) {
                        must_close_reason = "body_read_ch_closed_eof";
                    } else {
                        must_close_reason = "body_alts_unknown";
                    }
                    must_close_conn = 1;
                    GOC_DBG("handle_conn_fiber[%llu]: closing on body read wait reason=%s sel_ch=%p close_ch=%p read_ch=%p active_conns=%d\n",
                            (unsigned long long)cfid,
                            must_close_reason,
                            sel ? (void*)sel->ch : NULL,
                            (void*)close_ch,
                            (void*)read_ch,
                            atomic_load(&srv->active_conns));
                    break;
                }
                goc_val_t* v = &sel->value;
                goc_io_read_t* r = (goc_io_read_t*)v->val;
                if (r->nread < 0) {
                    must_close_reason = "body_nread<0";
                    must_close_conn = 1;
                    GOC_DBG("handle_conn_fiber[%llu]: body read error nread=%zd reason=%s\n",
                            (unsigned long long)cfid, r->nread, must_close_reason);
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
            GOC_DBG("handle_conn_fiber[%llu]: stopping read on conn=%p read_ch=%p close_ch=%p\n",
                    (unsigned long long)cfid, (void*)conn, (void*)read_ch, (void*)close_ch);
            goc_io_read_stop((uv_stream_t*)conn);
            goc_close(read_ch);
            GOC_DBG("handle_conn_fiber[%llu]: read_ch=%p closed, waiting for EOS/EOF\n",
                    (unsigned long long)cfid,
                    (void*)read_ch);
            for (;;) {
                goc_val_t* dv = goc_take(read_ch);
                if (!dv || dv->ok != GOC_OK)
                    break;
            }
            GOC_DBG("handle_conn_fiber[%llu]: read_ch=%p drained after close\n",
                    (unsigned long long)cfid,
                    (void*)read_ch);
            read_started = 0;
        }

        if (must_close_conn) {
            GOC_DBG("handle_conn_fiber[%llu]: connection will close conn=%p reason=%s keep_alive=%d active_conns=%d\n",
                    (unsigned long long)cfid, (void*)conn,
                    must_close_reason ? must_close_reason : "<null>", keep_alive_req,
                    atomic_load(&srv->active_conns));
            break;
        }

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
            GOC_DBG("handle_conn_fiber[%llu]: no route for request conn=%p keep_alive=%d closing=%d\n",
                    (unsigned long long)cfid,
                    (void*)conn,
                    keep_alive_req,
                    !keep_alive_req);
            if (!keep_alive_req) {
                break;
            }
            req_iter++;
            continue;
        }

        goc_array* req_headers = goc_array_make(num_headers);
        for (size_t i = 0; i < num_headers; i++) {
            goc_http_header_t* hdr =
                (goc_http_header_t*)goc_malloc(sizeof(goc_http_header_t));
            char* hname = (char*)goc_malloc(headers[i].name_len + 1);
            memcpy(hname, headers[i].name, headers[i].name_len);
            hname[headers[i].name_len] = '\0';
            char* hval = (char*)goc_malloc(headers[i].value_len + 1);
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
        GOC_DBG("handle_conn_fiber[%llu]: invoking handler conn=%p method=%s path=%s keep_alive=%d req_iter=%zu body_len=%zu\n",
                (unsigned long long)cfid,
                (void*)conn,
                method_str,
                path_str,
                keep_alive_req,
                req_iter,
                body_end > head_consumed ? body_end - head_consumed : 0);
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
                if (!w->keep_alive) {
                    break;
                }
                continue;
            }
        }

        w->handler(ctx);
        if (!w->keep_alive) {
            GOC_DBG("handle_conn_fiber[%llu]: handler completed, closing conn=%p keep_alive=%d\n",
                    (unsigned long long)cfid,
                    (void*)conn,
                    w->keep_alive);
            break;
        }
        req_iter++;
    }

done:
    free_buf(buf);
    if (headers)
        free(headers);
    GOC_DBG("handle_conn_fiber[%llu]: exit conn=%p active_conns_before=%d\n",
            (unsigned long long)cfid, (void*)conn,
            atomic_load(&srv->active_conns));
    goc_io_handle_close((uv_handle_t*)conn);
    if (atomic_fetch_sub_explicit(&srv->active_conns, 1, memory_order_release) == 1) {
        GOC_DBG("handle_conn_fiber[%llu]: last active_conn, closing shutdown_ch=%p\n",
                (unsigned long long)cfid,
                (void*)srv->shutdown_ch);
        goc_close(srv->shutdown_ch);
    }
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

    GOC_DBG("goc_http_server_respond_buf: ctx=%p conn=%p status=%d content_type=%s body_len=%zu keep_alive=%d\n",
            (void*)ctx, (void*)w->conn, status, content_type, len, w->keep_alive);

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
    GOC_DBG("goc_http_server_respond: ctx=%p status=%d content_type=%s body_len=%zu\n",
            (void*)ctx, status, content_type ? content_type : "<null>",
            body ? strlen(body) : 0);
    return goc_http_server_respond_buf(ctx, status, content_type,
                                   body ? body : "",
                                   body ? strlen(body) : 0);
}

goc_chan* goc_http_server_respond_error(goc_http_ctx_t* ctx, int status,
                                    const char* message)
{
    GOC_DBG("goc_http_server_respond_error: ctx=%p status=%d message=%s\n",
            (void*)ctx, status, message ? message : "<null>");
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
    return o;
}

int goc_http_client_inflight(void)
{
    return atomic_load_explicit(&g_http_client_inflight, memory_order_acquire);
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

typedef struct http_client_arg_t {
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

    int        keepalive_slot_owner;
    int        keepalive_slot_acquired;
    http_worker_pool_t* keepalive_slot_owner_pool;

    _Atomic(uv_tcp_t*) tcp;
    _Atomic(int)         tcp_initialized;
    _Atomic(int)         cancel_requested;
} http_client_arg_t;

static uint64_t http_now_ms(void)
{
    return uv_hrtime() / 1000000ULL;
}

static void http_client_assert_totals(void)
{
    int total = atomic_load_explicit(&g_http_total_connections,
                                     memory_order_acquire);
    int idle = atomic_load_explicit(&g_http_idle_connections,
                                    memory_order_acquire);
    if (total < 0 || idle < 0 || total > GOC_HTTP_MAX_CONNECTIONS ||
        idle > GOC_HTTP_MAX_CONNECTIONS || idle > total) {
        ABORT("http_client invariant violated total=%d idle=%d",
              total,
              idle);
    }
}

static void http_conn_assert_state(http_conn_t* conn,
                                   http_conn_state_t expected)
{
    if (!conn)
        return;
    if (conn->state != expected) {
        ABORT("http_conn bad state: expected=%d got=%d host=%s port=%u",
              expected,
              conn->state,
              conn->host ? conn->host : "<null>",
              conn->port);
    }
}

static void http_conn_transition(http_conn_t* conn,
                                 http_conn_state_t from,
                                 http_conn_state_t to)
{
    GOC_DBG("http_conn_transition: host=%s port=%u from=%d to=%d\n",
            conn->host ? conn->host : "<null>",
            conn->port,
            from,
            to);
    http_conn_assert_state(conn, from);
    conn->state = to;
}

static http_conn_t* http_conn_make(const char* host, uint16_t port)
{
    http_conn_t* conn = (http_conn_t*)calloc(1, sizeof(*conn));
    if (!conn)
        return NULL;

    conn->state = HTTP_CONN_CONNECTING;
    conn->timed_out = false;
    conn->host = host ? strdup(host) : NULL;
    if (host && !conn->host) {
        free(conn);
        return NULL;
    }
    conn->port = port;
    conn->last_used_ms = 0;
    conn->tcp = NULL;
    conn->next = NULL;
    return conn;
}

static void http_client_register_lifecycle_hook(void)
{
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_http_lifecycle_hook_registered,
                                                &expected,
                                                1,
                                                memory_order_acq_rel,
                                                memory_order_relaxed)) {
        goc_register_lifecycle_hook(GOC_LIFECYCLE_HOOK_PRE_LOOP_SHUTDOWN,
                                     goc_http_reset_globals,
                                     NULL);
    }
}

static void http_client_init_worker_pool_registry(void)
{
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_http_worker_pools_mutex_init,
                                                &expected,
                                                1,
                                                memory_order_acq_rel,
                                                memory_order_relaxed)) {
        uv_mutex_init(&g_http_worker_pools_mutex);
        http_client_register_lifecycle_hook();
    } else {
        while (!atomic_load_explicit(&g_http_worker_pools_mutex_init,
                                     memory_order_acquire)) {
            goc_yield();
        }
    }
}

static http_worker_pool_t* http_client_current_worker_pool(void)
{
    if (g_http_worker_pool)
        return g_http_worker_pool;

    int worker_id = goc_current_worker_id();
    goc_pool* pool = goc_current_pool();
    if (!pool)
        pool = goc_default_pool();

    http_worker_pool_t* wp = calloc(1, sizeof(*wp));
    if (!wp)
        ABORT("http_client_current_worker_pool: allocation failed\n");

    wp->pool = pool;
    wp->worker_id = worker_id;
    wp->idle_stack = NULL;
    wp->wait_ch = NULL;
    uv_mutex_init(&wp->idle_mutex);
    http_client_init_worker_pool_registry();
    uv_mutex_lock(&g_http_worker_pools_mutex);
    bool pool_hook_registered = false;
    for (size_t i = 0; i < g_http_worker_pools_len; i++) {
        if (g_http_worker_pools[i] &&
            g_http_worker_pools[i]->pool == pool) {
            pool_hook_registered = true;
            break;
        }
    }
    if (g_http_worker_pools_len == g_http_worker_pools_cap) {
        size_t new_cap = g_http_worker_pools_cap ? g_http_worker_pools_cap * 2 : 8;
        http_worker_pool_t** new_pools = realloc(g_http_worker_pools,
                                                new_cap * sizeof(*new_pools));
        if (!new_pools) {
            uv_mutex_unlock(&g_http_worker_pools_mutex);
            ABORT("http_client_current_worker_pool: registry realloc failed\n");
        }
        g_http_worker_pools = new_pools;
        g_http_worker_pools_cap = new_cap;
    }
    g_http_worker_pools[g_http_worker_pools_len++] = wp;
    if (!pool_hook_registered) {
        goc_register_lifecycle_hook(
            GOC_LIFECYCLE_HOOK_PRE_POOL_DESTROY,
            http_client_close_wait_chs_hook,
            pool);
    }
    uv_mutex_unlock(&g_http_worker_pools_mutex);

    g_http_worker_pool = wp;
    return g_http_worker_pool;
}


static int http_client_workers_have_same_pool(http_worker_pool_t* a,
                                              http_worker_pool_t* b)
{
    return a && b && a->pool == b->pool;
}

static int http_client_try_acquire_slot(void)
{
    int current = atomic_load_explicit(&g_http_total_connections,
                                      memory_order_acquire);
    while (current < GOC_HTTP_MAX_CONNECTIONS) {
        if (atomic_compare_exchange_weak_explicit(&g_http_total_connections,
                                                  &current,
                                                  current + 1,
                                                  memory_order_acq_rel,
                                                  memory_order_relaxed)) {
            GOC_DBG("http_client_try_acquire_slot: acquired slot total=%d max=%d\n",
                    current + 1, GOC_HTTP_MAX_CONNECTIONS);
            http_client_assert_totals();
            return 1;
        }
    }
    GOC_DBG("http_client_try_acquire_slot: full total=%d max=%d\n",
            current, GOC_HTTP_MAX_CONNECTIONS);
    return 0;
}

static goc_chan* http_client_wait_ch(void)
{
    http_worker_pool_t* wp = http_client_current_worker_pool();
    if (!wp)
        return NULL;

    if (wp->wait_ch)
        return wp->wait_ch;

    wp->wait_ch = goc_chan_make(1);
    GOC_DBG("http_client_wait_ch: worker=%d created %p\n",
            wp->worker_id,
            (void*)wp->wait_ch);
    return wp->wait_ch;
}

static void http_client_signal_waiters(void)
{
    atomic_fetch_add_explicit(&g_http_wakeup_version,
                              1,
                              memory_order_release);

    if (atomic_load_explicit(&g_http_slot_waiters, memory_order_acquire) == 0)
        return;

    if (!atomic_load_explicit(&g_http_worker_pools_mutex_init,
                              memory_order_acquire)) {
        return;
    }

    uv_mutex_lock(&g_http_worker_pools_mutex);
    for (size_t i = 0; i < g_http_worker_pools_len; i++) {
        http_worker_pool_t* wp = g_http_worker_pools[i];
        if (!wp || !wp->wait_ch)
            continue;

        GOC_DBG("http_client_signal_waiters: worker=%d signaling waiters on %p\n",
                wp->worker_id,
                (void*)wp->wait_ch);
        goc_alt_op_t ops[2] = {
            { wp->wait_ch,  GOC_ALT_PUT,     NULL },
            { NULL,         GOC_ALT_DEFAULT, NULL },
        };
        goc_alts(ops, 2);
    }
    uv_mutex_unlock(&g_http_worker_pools_mutex);
}

static goc_chan* http_client_sweeper_ch(void)
{
    goc_chan* ch = atomic_load_explicit(&g_http_sweeper_ch,
                                       memory_order_acquire);
    if (ch)
        return ch;

    goc_chan* new_ch = goc_chan_make(1);
    if (!new_ch)
        return NULL;

    goc_chan_set_debug_tag(new_ch, "http_sweeper_ch");
    GOC_DBG("http_client_sweeper_ch: created %p\n",
            (void*)new_ch);

    if (!atomic_compare_exchange_strong_explicit(&g_http_sweeper_ch,
                                                &ch,
                                                new_ch,
                                                memory_order_acq_rel,
                                                memory_order_relaxed)) {
        goc_close(new_ch);
    }

    return atomic_load_explicit(&g_http_sweeper_ch,
                                memory_order_acquire);
}

static void http_client_signal_sweeper(void)
{
    goc_chan* ch = http_client_sweeper_ch();
    GOC_DBG("http_client_signal_sweeper: signaling sweeper %p\n",
            (void*)ch);

    if (!ch)
        return;

    goc_alt_op_t ops[2] = {
        { ch,           GOC_ALT_PUT,     NULL },
        { NULL,         GOC_ALT_DEFAULT, NULL },
    };
    goc_alts(ops, 2);
}

static void http_client_release_slot(void)
{
    int prev = atomic_fetch_sub_explicit(&g_http_total_connections,
                                         1,
                                         memory_order_acq_rel);
    int after = prev <= 0 ? 0 : prev - 1;
    if (prev <= 0)
        atomic_store_explicit(&g_http_total_connections,
                              0,
                              memory_order_release);
    GOC_DBG("http_client_release_slot: released slot prev=%d after=%d\n",
            prev, after);
    http_client_assert_totals();
}

static void http_client_return_connection_local(http_conn_t* conn,
                                                int healthy);

static http_conn_t* http_client_take_idle_connection(http_worker_pool_t* current_wp,
                                                     const char* host,
                                                     uint16_t port,
                                                     http_worker_pool_t** out_owner_wp)
{
    if (!current_wp || !host || !out_owner_wp)
        return NULL;

    http_conn_t* conn = NULL;
    http_worker_pool_t* owner_wp = NULL;

    /* Local search — per-worker lock only, no global contention. */
    uv_mutex_lock(&current_wp->idle_mutex);
    http_conn_t** prev = &current_wp->idle_stack;
    http_conn_t* cur = current_wp->idle_stack;
    while (cur) {
        if (cur->port == port && cur->host && strcmp(cur->host, host) == 0) {
            *prev = cur->next;
            conn = cur;
            owner_wp = current_wp;
            break;
        }
        prev = &cur->next;
        cur = cur->next;
    }
    uv_mutex_unlock(&current_wp->idle_mutex);

    /* Cross-worker steal — snapshot registry under global lock, then acquire
     * each target's idle_mutex individually to avoid global contention. */
    if (!conn) {
        uv_mutex_lock(&g_http_worker_pools_mutex);
        size_t nwp = g_http_worker_pools_len;
        uv_mutex_unlock(&g_http_worker_pools_mutex);

        for (size_t i = 0; i < nwp && !conn; i++) {
            http_worker_pool_t* wp = g_http_worker_pools[i];
            if (!wp || wp == current_wp || wp->pool != current_wp->pool)
                continue;

            uv_mutex_lock(&wp->idle_mutex);
            prev = &wp->idle_stack;
            cur = wp->idle_stack;
            while (cur) {
                if (cur->port == port && cur->host && strcmp(cur->host, host) == 0) {
                    *prev = cur->next;
                    conn = cur;
                    owner_wp = wp;
                    break;
                }
                prev = &cur->next;
                cur = cur->next;
            }
            uv_mutex_unlock(&wp->idle_mutex);
        }
    }

    if (conn) {
        atomic_fetch_sub_explicit(&g_http_idle_connections,
                                  1,
                                  memory_order_acq_rel);
        http_client_assert_totals();
    }

    if (conn)
        http_conn_transition(conn, HTTP_CONN_IDLE, HTTP_CONN_IN_USE);

    *out_owner_wp = owner_wp;
    return conn;
}

static void http_client_idle_close(http_conn_t* conn)
{
    if (!conn)
        return;
    http_conn_transition(conn, HTTP_CONN_CLOSING, HTTP_CONN_CLOSED);
    if (conn->tcp)
        goc_io_handle_close((uv_handle_t*)conn->tcp);
    if (conn->host)
        free(conn->host);
    free(conn);
}

static void http_client_destroy_connection(http_conn_t* conn)
{
    if (!conn)
        return;

    switch (conn->state) {
    case HTTP_CONN_CONNECTING:
    case HTTP_CONN_IN_USE:
    case HTTP_CONN_IDLE:
        http_conn_transition(conn, conn->state, HTTP_CONN_CLOSING);
        break;
    case HTTP_CONN_CLOSING:
        break;
    default:
        ABORT("http_conn bad state while destroying: %d host=%s port=%u",
              conn->state,
              conn->host ? conn->host : "<null>",
              conn->port);
    }

    http_conn_transition(conn, HTTP_CONN_CLOSING, HTTP_CONN_CLOSED);
    if (conn->tcp)
        goc_io_handle_close((uv_handle_t*)conn->tcp);
    if (conn->host)
        free(conn->host);
    free(conn);
}

static void http_client_return_connection_local(http_conn_t* conn,
                                                int healthy)
{
    if (!conn)
        return;

    http_worker_pool_t* wp = http_client_current_worker_pool();
    if (!wp) {
        if (conn->tcp)
            goc_io_handle_close((uv_handle_t*)conn->tcp);
        if (conn->host)
            free(conn->host);
        free(conn);
        http_client_release_slot();
        http_client_signal_waiters();
        return;
    }

    if (healthy &&
        atomic_load_explicit(&g_http_idle_connections,
                             memory_order_acquire) < GOC_HTTP_MAX_CONNECTIONS) {
        http_conn_transition(conn, HTTP_CONN_IN_USE, HTTP_CONN_IDLE);
        conn->last_used_ms = http_now_ms();

        uv_mutex_lock(&wp->idle_mutex);
        conn->next = wp->idle_stack;
        wp->idle_stack = conn;
        atomic_fetch_add_explicit(&g_http_idle_connections,
                                  1,
                                  memory_order_acq_rel);
        uv_mutex_unlock(&wp->idle_mutex);

        http_client_assert_totals();
        GOC_DBG("http_client_return_connection_local: returned connection to worker=%d idle=%d\n",
                wp->worker_id,
                atomic_load_explicit(&g_http_idle_connections,
                                     memory_order_acquire));
        http_client_signal_waiters();
        return;
    }

    http_conn_transition(conn, HTTP_CONN_IN_USE, HTTP_CONN_CLOSING);
    if (conn->tcp)
        goc_io_handle_close((uv_handle_t*)conn->tcp);
    if (conn->host)
        free(conn->host);
    free(conn);
    http_client_release_slot();
    http_client_signal_waiters();
}

static void http_client_idle_sweep(bool force_close_all)
{
    uint64_t now = http_now_ms();
    http_worker_pool_t* wp = http_client_current_worker_pool();
    if (!wp)
        return;

    uv_mutex_lock(&wp->idle_mutex);
    http_conn_t** prev = &wp->idle_stack;
    http_conn_t* conn = wp->idle_stack;

    size_t closed_count = 0;
    while (conn) {
        if (force_close_all || now - conn->last_used_ms > GOC_HTTP_IDLE_TIMEOUT_MS) {
            http_conn_assert_state(conn, HTTP_CONN_IDLE);
            http_conn_t* stale = conn;
            *prev = conn->next;
            conn = conn->next;
            atomic_fetch_sub_explicit(&g_http_idle_connections,
                                      1,
                                      memory_order_acq_rel);
            http_client_assert_totals();
            http_conn_transition(stale, HTTP_CONN_IDLE, HTTP_CONN_CLOSING);
            http_client_idle_close(stale);
            http_client_release_slot();
            closed_count++;
            continue;
        }
        prev = &conn->next;
        conn = conn->next;
    }
    uv_mutex_unlock(&wp->idle_mutex);

    while (closed_count--) {
        http_client_signal_waiters();
    }
}

static void http_client_idle_sweep_worker(void* arg)
{
    bool force_close = false;
    if (arg) {
        bool* force_close_all = (bool*)arg;
        force_close = *force_close_all;
        free(force_close_all);
    }
    http_client_idle_sweep(force_close);
}

static void http_client_sweep_all(bool force_close_all)
{
    if (!atomic_load_explicit(&g_http_worker_pools_mutex_init,
                              memory_order_acquire)) {
        return;
    }

    http_worker_pool_t* self_wp = http_client_current_worker_pool();

    uv_mutex_lock(&g_http_worker_pools_mutex);
    for (size_t i = 0; i < g_http_worker_pools_len; i++) {
        http_worker_pool_t* wp = g_http_worker_pools[i];
        if (!wp || !wp->pool)
            continue;

        if (force_close_all && wp == self_wp)
            continue;

        size_t idx = 0;
        if (wp->worker_id >= 0)
            idx = (size_t)wp->worker_id;

        bool* arg = malloc(sizeof(*arg));
        if (!arg)
            ABORT("http_client_sweep_all: allocation failed\n");

        *arg = force_close_all;
        goc_go_on_worker(wp->pool, idx, http_client_idle_sweep_worker,
                         arg);
    }
    uv_mutex_unlock(&g_http_worker_pools_mutex);

    if (force_close_all && self_wp)
        http_client_idle_sweep(true);

}

static void http_client_idle_sweeper(void* _)
{
    goc_chan* ch = http_client_sweeper_ch();
    if (!ch)
        return;

    for (;;) {
        if (atomic_load_explicit(&g_http_sweeper_ch,
                                 memory_order_acquire) == NULL ||
            goc_loop_is_shutting_down()) {
            break;
        }

        /* Sweep connections idle for > GOC_HTTP_IDLE_TIMEOUT_MS */
        http_client_sweep_all(false);

        int inflight = atomic_load_explicit(&g_http_client_inflight,
                                           memory_order_acquire);
        if (inflight == 0) {
            GOC_DBG("http_client_idle_sweeper: no inflight requests, exiting global sweeper\n");
            /* Sweep all idle connections */
            http_client_sweep_all(true);
            break;
        }

        /* Wait for either a new request or a timeout */
        goc_chan* timeout_ch = goc_timeout(GOC_HTTP_QUEUE_TIMEOUT_MS);
        goc_alt_op_t ops[2] = {
            { ch,          GOC_ALT_TAKE, NULL },
            { timeout_ch,  GOC_ALT_TAKE, NULL },
        };
        goc_alts(ops, 2);
        goc_close(timeout_ch);

        if (atomic_load_explicit(&g_http_sweeper_ch,
                                 memory_order_acquire) == NULL ||
            goc_loop_is_shutting_down()) {
            break;
        }
    }

    atomic_store_explicit(&g_http_sweeper_started,
                          0,
                          memory_order_release);
}

static void http_client_ensure_sweeper(void)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&g_http_sweeper_started,
                                                &expected,
                                                1,
                                                memory_order_acq_rel,
                                                memory_order_relaxed)) {
        return;
    }

    /* This ensures that the sweeper channel is initialized before any sweeper goroutine
       is started. Without this, there could be a race condition where the sweeper goroutine
       tries to use the channel before it is created.
    */
    http_client_sweeper_ch();

    goc_pool* pool = goc_current_or_default_pool();
    if (!pool)
        return;

    size_t idx = 0;
    http_worker_pool_t* wp = http_client_current_worker_pool();
    if (wp && wp->worker_id >= 0)
        idx = (size_t)wp->worker_id;

    goc_go_on_worker(pool, idx, http_client_idle_sweeper, NULL);
}

static void http_client_cancel(void* ud)
{
    http_client_arg_t* a = (http_client_arg_t*)ud;
    if (!atomic_load_explicit(&a->cancel_requested, memory_order_acquire))
        return;

    uv_tcp_t* tcp = atomic_load_explicit(&a->tcp, memory_order_acquire);
    int tcp_initialized = atomic_load_explicit(&a->tcp_initialized, memory_order_acquire);
    if (tcp && tcp_initialized) {
        goc_io_read_stop((uv_stream_t*)tcp);
        goc_io_handle_close((uv_handle_t*)tcp);
        atomic_store_explicit(&a->tcp, NULL, memory_order_release);
        atomic_store_explicit(&a->tcp_initialized, 0, memory_order_release);
    }
}

static void http_client_fiber(void* arg)
{
    http_client_arg_t* a = (http_client_arg_t*)arg;
    char* resp_buf = NULL;
    uv_tcp_t* tcp = NULL;
    int tcp_initialized = 0;
    int using_idle_connection = 0;
    int keepalive_retry = 0;
    int connect_retry = 0;
    http_conn_t* req_conn = NULL;
    int active_inflight = atomic_fetch_add_explicit(&g_http_client_inflight,
                                                   1,
                                                   memory_order_acq_rel) + 1;
    const char* error_reason = NULL;
    GOC_DBG("http_client_fiber[%llu]: started method=%s host=%s port=%u path=%s ch=%p keep_alive=%d active_inflight=%d current_worker=%d\n",
            (unsigned long long)a->req_id,
            a->method, a->host, a->port, a->path_and_query, (void*)a->ch,
            a->keep_alive,
            active_inflight,
            goc_current_worker_id());
    GOC_DBG("http_client_fiber[%llu]: entry arg=%p ch=%p pool=%p\n",
            (unsigned long long)a->req_id,
            (void*)arg,
            (void*)a->ch,
            (void*)goc_current_or_default_pool());

retry_connect:
    http_client_ensure_sweeper();

    if (atomic_load_explicit(&a->cancel_requested, memory_order_acquire)) {
        error_reason = "cancelled";
        goto deliver_error;
    }

    if (a->keep_alive) {
        http_worker_pool_t* wp = http_client_current_worker_pool();
        if (wp) {
            http_worker_pool_t* owner_wp = NULL;
            http_conn_t* conn = http_client_take_idle_connection(wp, a->host, a->port, &owner_wp);
            if (conn) {
                GOC_DBG("http_client_fiber[%llu]: reused idle connection on worker=%d host=%s port=%u idle=%d owner=%d\n",
                        (unsigned long long)a->req_id,
                        wp->worker_id,
                        a->host,
                        a->port,
                        atomic_load_explicit(&g_http_idle_connections,
                                             memory_order_acquire),
                        owner_wp ? owner_wp->worker_id : -1);
                req_conn = conn;
                tcp = conn->tcp;
                tcp_initialized = 1;
                atomic_store_explicit(&a->tcp, tcp, memory_order_release);
                atomic_store_explicit(&a->tcp_initialized, 1, memory_order_release);
                a->keepalive_slot_owner = goc_current_worker_id();
                a->keepalive_slot_owner_pool = wp;
                a->keepalive_slot_acquired = 1;
                using_idle_connection = 1;
            }
        }
    }

    if (!tcp) {
        uint64_t wakeup_version = atomic_load_explicit(&g_http_wakeup_version,
                                                      memory_order_acquire);
        int slot_acquired = 0;
        for (;;) {
            if (!http_client_try_acquire_slot()) {
                if (atomic_load_explicit(&g_http_idle_connections,
                                         memory_order_acquire) > 0) {
                    http_client_sweep_all(true);
                    if (http_client_try_acquire_slot()) {
                        slot_acquired = 1;
                        break;
                    }
                }

                goc_chan* timeout_ch = goc_timeout(GOC_HTTP_QUEUE_TIMEOUT_MS);
                goc_chan* wait_ch = http_client_wait_ch();
                uint64_t observed_version = atomic_load_explicit(&g_http_wakeup_version,
                                                               memory_order_acquire);
                if (observed_version != wakeup_version) {
                    goc_close(timeout_ch);
                    wakeup_version = observed_version;
                    continue;
                }

                GOC_DBG("http_client_fiber[%llu]: waiting for slot on %p\n",
                        (unsigned long long)a->req_id,
                        (void*)wait_ch);
                goc_alt_op_t ops[2] = {
                    { wait_ch,     GOC_ALT_TAKE, NULL },
                    { timeout_ch,  GOC_ALT_TAKE, NULL },
                };
                if (goc_loop_is_shutting_down()) {
                    error_reason = "shutdown";
                    goto deliver_error;
                }

                atomic_fetch_add_explicit(&g_http_slot_waiters, 1,
                                          memory_order_release);
                goc_alts_result_t* res = goc_alts(ops, 2);
                atomic_fetch_sub_explicit(&g_http_slot_waiters, 1,
                                          memory_order_release);
                goc_close(timeout_ch);
                if (res->ch == timeout_ch) {
                    error_reason = "queue_timeout";
                    GOC_DBG("http_client_fiber[%llu]: queue timeout after %llu ms total=%d idle=%d\n",
                            (unsigned long long)a->req_id,
                            (unsigned long long)GOC_HTTP_QUEUE_TIMEOUT_MS,
                            atomic_load_explicit(&g_http_total_connections,
                                                 memory_order_acquire),
                            atomic_load_explicit(&g_http_idle_connections,
                                                 memory_order_acquire));
                    goto deliver_error;
                }
                if (res->ch == wait_ch && res->value.ok == 0) {
                    if (goc_loop_is_shutting_down()) {
                        error_reason = "shutdown";
                        goto deliver_error;
                    }
                    wakeup_version = atomic_load_explicit(&g_http_wakeup_version,
                                                         memory_order_acquire);
                    continue;
                }
                wakeup_version = atomic_load_explicit(&g_http_wakeup_version,
                                                     memory_order_acquire);
                GOC_DBG("http_client_fiber[%llu]: woke from wait total=%d idle=%d\n",
                        (unsigned long long)a->req_id,
                        atomic_load_explicit(&g_http_total_connections,
                                             memory_order_acquire),
                        atomic_load_explicit(&g_http_idle_connections,
                                             memory_order_acquire));
                continue;
            } else {
                slot_acquired = 1;
                break;
            }
        }

        if (slot_acquired) {
            a->keepalive_slot_owner = goc_current_worker_id();
            a->keepalive_slot_owner_pool = http_client_current_worker_pool();
            a->keepalive_slot_acquired = 1;
            if (!req_conn) {
                req_conn = http_conn_make(a->host, a->port);
                if (!req_conn) {
                    error_reason = "oom";
                    http_client_release_slot();
                    http_client_signal_waiters();
                    goto deliver_error;
                }
            }
        }
    }

    if (atomic_load_explicit(&a->cancel_requested, memory_order_acquire)) {
        error_reason = "cancelled";
        goto deliver_error;
    }

    GOC_DBG("http_client_fiber[%llu]: connection acquisition complete tcp=%p tcp_initialized=%d keep_alive=%d\n",
            (unsigned long long)a->req_id,
            (void*)tcp,
            tcp_initialized,
            a->keep_alive);

    if (!tcp) {
        struct sockaddr_storage addr;
        int have_addr = (http_client_parse_numeric_addr(a->host, a->port, &addr) == 0);
        GOC_DBG("http_client_fiber[%llu]: address selection have_addr=%d host=%s port=%u\n",
                (unsigned long long)a->req_id,
                have_addr,
                a->host ? a->host : "<null>",
                (unsigned)a->port);
        if (!have_addr) {
            /* --- DNS --- */
            struct addrinfo hints;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family   = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            goc_val_t* vdns = goc_take(goc_io_getaddrinfo(a->host, NULL, &hints));
            GOC_DBG("http_client_fiber[%llu]: dns lookup returned v=%p ok=%d\n",
                    (unsigned long long)a->req_id,
                    (void*)vdns,
                    vdns ? (int)vdns->ok : -1);
            if (!vdns || vdns->ok != GOC_OK) {
                error_reason = "dns_lookup";
                goto deliver_error;
            }
            goc_io_getaddrinfo_t* dns = (goc_io_getaddrinfo_t*)vdns->val;
            if (!dns || dns->ok != GOC_IO_OK || !dns->res) {
                error_reason = "dns_lookup";
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
        GOC_DBG("http_client_fiber[%llu]: tcp_init starting tcp=%p\n",
                (unsigned long long)a->req_id,
                (void*)tcp);
        int rc = goc_unbox_int(goc_take(goc_io_tcp_init(tcp))->val);
        GOC_DBG("http_client_fiber[%llu]: tcp_init completed tcp=%p rc=%d\n",
                (unsigned long long)a->req_id,
                (void*)tcp,
                rc);
        if (rc < 0) {
            error_reason = "tcp_init";
            tcp = NULL;
            goto deliver_error;
        }
        tcp_initialized = 1;
        if (req_conn)
            req_conn->tcp = tcp;
        atomic_store_explicit(&a->tcp, tcp, memory_order_release);
        atomic_store_explicit(&a->tcp_initialized, 1, memory_order_release);

        if (atomic_load_explicit(&a->cancel_requested, memory_order_acquire)) {
            error_reason = "cancelled";
            goto deliver_error;
        }

        GOC_DBG("http_client_fiber[%llu]: tcp_connect prep tcp=%p tcp_initialized=%d using_idle_connection=%d keep_alive=%d host=%s port=%u\n",
                (unsigned long long)a->req_id,
                (void*)tcp,
                tcp_initialized,
                using_idle_connection,
                a->keep_alive,
                a->host ? a->host : "<null>",
                (unsigned)a->port);

        GOC_DBG("http_client_fiber[%llu]: tcp_connect starting tcp=%p\n",
                (unsigned long long)a->req_id,
                (void*)tcp);
        goc_chan* connect_ch = goc_io_tcp_connect(tcp, (const struct sockaddr*)&addr);
        goc_chan* timeout_ch = goc_timeout(GOC_HTTP_CONNECT_TIMEOUT_MS);
        goc_alt_op_t ops[2] = {
            { connect_ch, GOC_ALT_TAKE, NULL },
            { timeout_ch, GOC_ALT_TAKE, NULL },
        };
        goc_alts_result_t* res = goc_alts(ops, 2);
        goc_close(timeout_ch);

        if (res->ch == timeout_ch) {
            GOC_DBG("http_client_fiber[%llu]: connect timeout after %llu ms tcp=%p\n",
                    (unsigned long long)a->req_id,
                    (unsigned long long)GOC_HTTP_CONNECT_TIMEOUT_MS,
                    (void*)tcp);
            if (req_conn) {
                req_conn->timed_out = true;
                http_conn_transition(req_conn,
                                     HTTP_CONN_CONNECTING,
                                     HTTP_CONN_CLOSING);
            }
            if (tcp_initialized) {
                goc_io_handle_close((uv_handle_t*)tcp);
                if (req_conn)
                    req_conn->tcp = NULL;
            }
            tcp = NULL;
            tcp_initialized = 0;
            atomic_store_explicit(&a->tcp, NULL, memory_order_release);
            atomic_store_explicit(&a->tcp_initialized, 0, memory_order_release);
            goc_close(connect_ch);
            if (using_idle_connection && !keepalive_retry) {
                GOC_DBG("http_client_fiber[%llu]: timed-out keepalive socket, dropping idle connection and retrying\n",
                        (unsigned long long)a->req_id);
                a->keepalive_slot_acquired = 0;
                a->keepalive_slot_owner = -1;
                a->keepalive_slot_owner_pool = NULL;
                using_idle_connection = 0;
                keepalive_retry = 1;
                if (req_conn) {
                    http_client_destroy_connection(req_conn);
                    req_conn = NULL;
                }
                http_client_release_slot();
                http_client_signal_waiters();
                goto retry_connect;
            }
            error_reason = "connect_timeout";
            goto deliver_error;
        }

        goc_close(connect_ch);
        rc = goc_unbox_int(res->value.val);
        GOC_DBG("http_client_fiber[%llu]: tcp_connect completed tcp=%p rc=%d\n",
                (unsigned long long)a->req_id,
                (void*)tcp,
                rc);
        if (req_conn && req_conn->timed_out) {
            GOC_DBG("http_client_fiber[%llu]: late connect completion ignored tcp=%p\n",
                    (unsigned long long)a->req_id,
                    (void*)tcp);
            if (tcp_initialized) {
                goc_io_handle_close((uv_handle_t*)tcp);
                if (req_conn)
                    req_conn->tcp = NULL;
            }
            tcp = NULL;
            tcp_initialized = 0;
            atomic_store_explicit(&a->tcp, NULL, memory_order_release);
            atomic_store_explicit(&a->tcp_initialized, 0, memory_order_release);
            if (req_conn) {
                http_client_destroy_connection(req_conn);
                req_conn = NULL;
            }
            error_reason = "connect_timeout";
            goto deliver_error;
        }

        if (rc == 0 && req_conn)
            http_conn_transition(req_conn, HTTP_CONN_CONNECTING, HTTP_CONN_IN_USE);
        if (rc < 0) {
            if (using_idle_connection && !keepalive_retry) {
                GOC_DBG("http_client_fiber[%llu]: keepalive socket failed, dropping idle connection and retrying\n",
                        (unsigned long long)a->req_id);
                if (tcp_initialized) {
                    goc_io_handle_close((uv_handle_t*)tcp);
                    if (req_conn)
                        req_conn->tcp = NULL;
                }
                tcp = NULL;
                tcp_initialized = 0;
                atomic_store_explicit(&a->tcp, NULL, memory_order_release);
                atomic_store_explicit(&a->tcp_initialized, 0, memory_order_release);
                a->keepalive_slot_acquired = 0;
                a->keepalive_slot_owner = -1;
                a->keepalive_slot_owner_pool = NULL;
                using_idle_connection = 0;
                keepalive_retry = 1;
                if (req_conn) {
                    http_client_destroy_connection(req_conn);
                    req_conn = NULL;
                }
                http_client_release_slot();
                http_client_signal_waiters();
                goto retry_connect;
            }

            /* Only retry the narrow failure cases that can be safely recovered
             * before sending the request. This matches Go semantics: stale
             * keepalive sockets are dropped once, and transient local bind
             * failures are retried once. Do not retry after request
             * transmission has begun or the response has already started. */
            if ((rc == UV_EADDRINUSE || rc == UV_EADDRNOTAVAIL) && !connect_retry) {
                GOC_DBG("http_client_fiber[%llu]: tcp_connect got %s, retrying once\n",
                        (unsigned long long)a->req_id,
                        rc == UV_EADDRINUSE ? "UV_EADDRINUSE" : "UV_EADDRNOTAVAIL");
                if (tcp_initialized) {
                    goc_io_handle_close((uv_handle_t*)tcp);
                    if (req_conn)
                        req_conn->tcp = NULL;
                }
                tcp = NULL;
                tcp_initialized = 0;
                atomic_store_explicit(&a->tcp, NULL, memory_order_release);
                atomic_store_explicit(&a->tcp_initialized, 0, memory_order_release);
                connect_retry = 1;
                goto retry_connect;
            }
            error_reason = "tcp_connect";
            if (tcp_initialized) {
                goc_io_handle_close((uv_handle_t*)tcp);
                if (req_conn)
                    req_conn->tcp = NULL;
            }
            tcp = NULL;
            tcp_initialized = 0;
            atomic_store_explicit(&a->tcp, NULL, memory_order_release);
            atomic_store_explicit(&a->tcp_initialized, 0, memory_order_release);
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

    GOC_DBG(
            "http_client_fiber[%llu]: sending request tcp=%p loop=%p tcp_initialized=%d using_idle_connection=%d keep_alive=%d req_len=%zu active_inflight=%d\n",
            (unsigned long long)a->req_id,
            (void*)tcp,
            (void*)tcp ? (void*)tcp->loop : NULL,
            tcp_initialized,
            using_idle_connection,
            a->keep_alive,
            req_len,
            active_inflight);
    GOC_DBG("http_client_fiber[%llu]: sending request tcp=%p tcp_initialized=%d using_idle_connection=%d keep_alive=%d req_len=%zu active_inflight=%d\n",
            (unsigned long long)a->req_id,
            (void*)tcp,
            tcp_initialized,
            using_idle_connection,
            a->keep_alive,
            req_len,
            active_inflight);

    uv_buf_t wb = uv_buf_init(gc_req, (unsigned int)req_len);
    GOC_DBG("http_client_fiber[%llu]: goc_io_write starting tcp=%p wb_len=%zu\n",
            (unsigned long long)a->req_id,
            (void*)tcp,
            req_len);
    int rc = goc_unbox_int(
        goc_take(goc_io_write((uv_stream_t*)tcp, &wb, 1))->val);
    GOC_DBG("http_client_fiber[%llu]: goc_io_write completed tcp=%p rc=%d\n",
            (unsigned long long)a->req_id,
            (void*)tcp,
            rc);
    if (atomic_load_explicit(&a->cancel_requested, memory_order_acquire)) {
        error_reason = "cancelled";
        goto deliver_error;
    }
    if (rc < 0) {
        if (using_idle_connection && !keepalive_retry) {
            tcp = NULL;
            tcp_initialized = 0;
            atomic_store_explicit(&a->tcp, NULL, memory_order_release);
            atomic_store_explicit(&a->tcp_initialized, 0, memory_order_release);
            using_idle_connection = 0;
            a->keepalive_slot_acquired = 0;
            a->keepalive_slot_owner = -1;
            a->keepalive_slot_owner_pool = NULL;
            keepalive_retry = 1;
            if (req_conn) {
                http_client_destroy_connection(req_conn);
                req_conn = NULL;
            }
            http_client_release_slot();
            http_client_signal_waiters();
            goto retry_connect;
        }
        error_reason = "write";
        GOC_DBG("http_client_fiber[%llu]: closing tcp on write error tcp=%p tcp_initialized=%d using_idle_connection=%d keep_alive=%d\n",
                (unsigned long long)a->req_id, (void*)tcp, tcp_initialized,
                using_idle_connection, a->keep_alive);
        if (tcp_initialized)
            goc_io_handle_close((uv_handle_t*)tcp);
        tcp = NULL;
        tcp_initialized = 0;
        atomic_store_explicit(&a->tcp, NULL, memory_order_release);
        atomic_store_explicit(&a->tcp_initialized, 0, memory_order_release);
        goto deliver_error;
    }

    /* --- Read + parse response --- */
    {
        size_t resp_len = 0;
        size_t resp_cap = 0;
        goc_http_response_t* response = NULL;
        goc_chan* rd = NULL;
        ssize_t content_length = -1;
        size_t  body_start     = 0;

        rd = goc_io_read_start((uv_stream_t*)tcp);
        GOC_DBG(
                "http_client_fiber[%llu]: read start rd=%p tcp=%p loop=%p stream->data=%p\n",
                (unsigned long long)a->req_id,
                (void*)rd,
                (void*)tcp,
                (void*)tcp->loop,
                (void*)tcp->data);
        GOC_DBG("http_client_fiber[%llu]: read start rd=%p tcp=%p stream->data=%p\n",
                (unsigned long long)a->req_id,
                (void*)rd,
                (void*)tcp,
                (void*)tcp->data);
        for (;;) {
            goc_val_t* v = goc_take(rd);
            if (!v || v->ok != GOC_OK) {
                GOC_DBG("http_client_fiber[%llu]: read loop exit v=%p ok=%d tcp=%p resp_len=%zu rd=%p\n",
                        (unsigned long long)a->req_id,
                        (void*)v,
                        v ? (int)v->ok : -1,
                        (void*)tcp,
                        resp_len,
                        (void*)rd);
                break;
            }
            goc_io_read_t* r = (goc_io_read_t*)v->val;
            GOC_DBG("http_client_fiber[%llu]: read chunk tcp=%p nread=%zd resp_len_before=%zu\n",
                    (unsigned long long)a->req_id,
                    (void*)tcp,
                    r->nread,
                    resp_len);
            if (atomic_load_explicit(&a->cancel_requested, memory_order_acquire)) {
                error_reason = "cancelled";
                break;
            }
            if (r->nread < 0) { break; }

            /* Grow plain-malloc buffer. */
            size_t needed = resp_len + (size_t)r->nread + 1;
            if (needed > resp_cap) {
                size_t nc = resp_cap ? resp_cap * 2 : 4096;
                while (nc < needed) nc *= 2;
                char* nb = (char*)realloc(resp_buf, nc);
                if (!nb) { break; }
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
        goc_io_read_stop((uv_stream_t*)tcp);
        GOC_DBG("http_client_fiber[%llu]: goc_io_read_stop requested tcp=%p rd=%p stream->data=%p\n",
                (unsigned long long)a->req_id,
                (void*)tcp,
                (void*)rd,
                (void*)tcp->data);
        GOC_DBG("http_client_fiber[%llu]: deferring rd close to read-stop callback rd=%p\n",
                (unsigned long long)a->req_id,
                (void*)rd);
        GOC_DBG("http_client_fiber[%llu]: waiting for rd close via read-stop callback rd=%p\n",
                (unsigned long long)a->req_id,
                (void*)rd);
        GOC_DBG("http_client_fiber[%llu]: read loop complete tcp=%p resp_len=%zu body_start=%zu content_length=%zd response=%p\n",
                (unsigned long long)a->req_id,
                (void*)tcp,
                resp_len,
                body_start,
                content_length,
                (void*)response);
        for (;;) {
            goc_val_t* dv = goc_take(rd);
            if (!dv || dv->ok != GOC_OK)
                break;
        }
        GOC_DBG("http_client_fiber[%llu]: rd fully drained and closed rd=%p tcp=%p\n",
                (unsigned long long)a->req_id,
                (void*)rd,
                (void*)tcp);

        /* --- Attach body and deliver --- */
        if (!response) {
                error_reason = "missing_response";
            GOC_DBG("http_client_fiber[%llu]: response missing after read tcp=%p tcp_initialized=%d using_idle_connection=%d keep_alive=%d resp_len=%zu body_start=%zu content_length=%zd\n",
                    (unsigned long long)a->req_id,
                    (void*)tcp,
                    tcp_initialized,
                    using_idle_connection,
                    a->keep_alive,
                    resp_len,
                    body_start,
                    content_length);
            if (using_idle_connection && !keepalive_retry) {
                GOC_DBG("http_client_fiber[%llu]: keepalive socket stale, retrying without idle connection\n",
                        (unsigned long long)a->req_id);
                if (tcp_initialized)
                    goc_io_handle_close((uv_handle_t*)tcp);
                a->keepalive_slot_acquired = 0;
                a->keepalive_slot_owner = -1;
                a->keepalive_slot_owner_pool = NULL;
                using_idle_connection = 0;
                tcp = NULL;
                tcp_initialized = 0;
                keepalive_retry = 1;
                if (req_conn) {
                    http_client_destroy_connection(req_conn);
                    req_conn = NULL;
                }
                http_client_release_slot();
                http_client_signal_waiters();
                free_buf(resp_buf);
                resp_buf = NULL;
                resp_len = 0;
                resp_cap = 0;
                response = NULL;
                content_length = -1;
                body_start = 0;
                goto retry_connect;
            }
            if (using_idle_connection) {
                if (tcp_initialized)
                    goc_io_handle_close((uv_handle_t*)tcp);
                tcp = NULL;
                tcp_initialized = 0;
                a->keepalive_slot_acquired = 0;
                a->keepalive_slot_owner = -1;
                if (req_conn) {
                    http_client_destroy_connection(req_conn);
                    req_conn = NULL;
                }
                http_client_release_slot();
                http_client_signal_waiters();
            } else if (tcp) {
                if (tcp_initialized)
                    goc_io_handle_close((uv_handle_t*)tcp);
                tcp = NULL;
                tcp_initialized = 0;
            }
            free_buf(resp_buf);
            goto deliver_error;
        }
        size_t blen = resp_len > body_start ? resp_len - body_start : 0;
        if (content_length >= 0 && (size_t)content_length < blen)
            blen = (size_t)content_length;
        char* body = (char*)goc_malloc(blen + 1);
        if (blen && resp_buf) memcpy(body, resp_buf + body_start, blen);
        body[blen] = '\0';
        free_buf(resp_buf);
        response->body     = body;
        response->body_len = blen;

        int can_keep = a->keep_alive &&
                       content_length >= 0 &&
                       !response_has_connection_close(response);
        if (can_keep) {
            http_client_return_connection_local(req_conn, 1);
            req_conn = NULL;
            tcp = NULL;
            tcp_initialized = 0;
        } else {
            if (req_conn) {
                http_client_return_connection_local(req_conn, 0);
                req_conn = NULL;
            } else {
                if (tcp_initialized)
                    goc_io_handle_close((uv_handle_t*)tcp);
                if (a->keepalive_slot_acquired) {
                    http_client_release_slot();
                    http_client_signal_waiters();
                }
            }
            tcp = NULL;
            tcp_initialized = 0;
        }

        GOC_DBG(
                "http_client_fiber[%llu]: delivering response ch=%p response=%p status=%d body_len=%zu keep_alive=%d\n",
                (unsigned long long)a->req_id,
                (void*)a->ch,
                (void*)response,
                response ? response->status : -1,
                response ? response->body_len : 0,
                a->keep_alive);
        GOC_DBG("http_client_fiber[%llu]: delivering response ch=%p response=%p status=%d body_len=%zu\n",
                (unsigned long long)a->req_id,
                (void*)a->ch,
                (void*)response,
                response ? response->status : -1,
                response ? response->body_len : 0);
        goc_put(a->ch, response);
        GOC_DBG(
                "http_client_fiber[%llu]: response delivered ch=%p\n",
                (unsigned long long)a->req_id,
                (void*)a->ch);
        GOC_DBG("http_client_fiber[%llu]: response delivered ch=%p\n",
                (unsigned long long)a->req_id,
                (void*)a->ch);
        goc_close(a->ch);
        GOC_DBG("http_client_fiber[%llu]: goc_close(a->ch) completed ch=%p\n",
                (unsigned long long)a->req_id,
                (void*)a->ch);
        active_inflight = atomic_fetch_sub_explicit(&g_http_client_inflight,
                                                   1,
                                                   memory_order_acq_rel) - 1;
        GOC_DBG("http_client_fiber[%llu]: exiting success keep_alive=%d active_inflight=%d\n",
                (unsigned long long)a->req_id, a->keep_alive,
                active_inflight);
        return;
    }

deliver_error: {
    GOC_DBG("http_client_fiber[%llu]: deliver_error reason=%s tcp=%p tcp_initialized=%d keep_alive=%d active_inflight=%d\n",
            (unsigned long long)a->req_id,
            error_reason ? error_reason : "unknown",
            (void*)tcp,
            tcp_initialized,
            a->keep_alive,
            active_inflight);
    if (req_conn) {
        http_client_destroy_connection(req_conn);
        req_conn = NULL;
    } else if (tcp_initialized) {
        goc_io_handle_close((uv_handle_t*)tcp);
    }
    tcp = NULL;
    tcp_initialized = 0;
    if (a->keepalive_slot_acquired) {
        http_client_release_slot();
        http_client_signal_waiters();
    }
    free_buf(resp_buf);
    goc_http_response_t* r =
        (goc_http_response_t*)goc_malloc(sizeof(goc_http_response_t));
    memset(r, 0, sizeof(*r));
    r->headers = goc_array_make(0);
    r->body    = "";
    GOC_DBG("http_client_fiber[%llu]: delivering error response ch=%p response=%p\n",
            (unsigned long long)a->req_id,
            (void*)a->ch,
            (void*)r);
    goc_put(a->ch, r);
    GOC_DBG("http_client_fiber[%llu]: error response delivered ch=%p\n",
            (unsigned long long)a->req_id,
            (void*)a->ch);
    goc_close(a->ch);
    GOC_DBG("http_client_fiber[%llu]: goc_close(a->ch) completed ch=%p\n",
            (unsigned long long)a->req_id,
            (void*)a->ch);
    active_inflight = atomic_fetch_sub_explicit(&g_http_client_inflight,
                                               1,
                                               memory_order_acq_rel) - 1;
    GOC_DBG("http_client_fiber[%llu]: exiting error reason=%s keep_alive=%d active_inflight=%d\n",
            (unsigned long long)a->req_id,
            error_reason ? error_reason : "unknown",
            a->keep_alive,
            active_inflight);
    }
}

goc_chan* goc_http_request(const char* method, const char* url,
                            const char* content_type,
                            const char* body, size_t body_len,
                            goc_http_request_opts_t* opts)
{
    GOC_DBG("goc_http_request: method=%s url=%s content_type=%s body_len=%zu opts=%p\n",
            method ? method : "<null>",
            url ? url : "<null>",
            content_type ? content_type : "<null>",
            body_len,
            (void*)opts);

    goc_chan* ch = goc_chan_make(1);

    char*    host = NULL;
    char*    pq   = NULL;
    uint16_t port = 80;

    if (parse_url(url, &host, &port, &pq) != 0) {
        GOC_DBG("goc_http_request: parse_url failed url=%s\n",
                url ? url : "<null>");
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
    atomic_store_explicit(&a->tcp, NULL, memory_order_relaxed);
    atomic_store_explicit(&a->tcp_initialized, 0, memory_order_relaxed);
    atomic_store_explicit(&a->cancel_requested, 0, memory_order_relaxed);
    a->keepalive_slot_owner = -1;
    a->keepalive_slot_owner_pool = NULL;
    a->keepalive_slot_acquired = 0;
    chan_set_on_close(ch, http_client_cancel, a);

            GOC_DBG("goc_http_request[%llu]: entry method=%s url=%s host=%s port=%u path=%s keep_alive=%d timeout_ms=%llu ch=%p\n",
            (unsigned long long)a->req_id, method, url, host, (unsigned)port, pq,
            opts ? opts->keep_alive : 0,
                (unsigned long long)(opts ? opts->timeout_ms : 0),
            (void*)ch);

    goc_pool* req_pool = goc_current_or_default_pool();
    GOC_DBG("goc_http_request[%llu]: scheduling client fiber req_id=%llu ch=%p pool=%p keep_alive=%d\n",
            (unsigned long long)a->req_id,
            (unsigned long long)a->req_id,
            (void*)ch,
            (void*)req_pool,
            opts ? opts->keep_alive : 0);
    goc_go_on(req_pool, http_client_fiber, a);

    /* Honour optional timeout (fiber context only). */
    if (opts && opts->timeout_ms > 0) {
        goc_chan*    tch = goc_timeout(opts->timeout_ms);
        goc_alt_op_t  ops[2] = {
            { ch,  GOC_ALT_TAKE, NULL },
            { tch, GOC_ALT_TAKE, NULL },
        };
        goc_alts_result_t* res = goc_http_chan_alts(ops, 2);
        if (res->ch == tch) {
            /* Timeout: cancel the underlying HTTP fiber immediately, then
             * close the request channel so the client fiber can tear down its
             * socket early instead of continuing to read a stale response. */
            atomic_store_explicit(&a->cancel_requested, 1, memory_order_release);
            http_client_cancel(a);
            GOC_DBG("goc_http_request[%llu]: timeout after %u ms url=%s keep_alive=%d\n",
                    (unsigned long long)a->req_id,
                    (unsigned)(opts ? opts->timeout_ms : 0),
                    url, a->keep_alive);
            goc_close(ch);
            goc_chan* toch = goc_chan_make(1);
            goc_http_response_t* r = goc_malloc(sizeof(goc_http_response_t));
            r->status = 408;
            r->headers = goc_array_make(0);
            r->body    = http_status_str(r->status);
            r->body_len = strlen(r->body);
            GOC_DBG("goc_http_request: timeout response ch=%p url=%s keep_alive=%d\n",
                    (void*)toch,
                    url ? url : "<null>",
                    a->keep_alive);
            goc_http_chan_put(toch, r);
            goc_close(toch);
            return toch;
        }
        /* Response arrived before timeout — cancel the timer and re-wrap. */
        GOC_DBG("goc_http_request[%llu]: response arrived before timeout url=%s keep_alive=%d\n",
                (unsigned long long)a->req_id,
                url,
                a->keep_alive);
        goc_close(tch);
        goc_chan* ch2 = goc_chan_make(1);
        goc_http_chan_put(ch2, res->value.val);
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
