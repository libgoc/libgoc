# libgoc HTTP (`goc_http`)

> A CSP-style HTTP/1.1 library for libgoc — a route-based server that dispatches requests into fibers, and a channel-based client for making outbound HTTP calls, both built on `goc_io` TCP and picohttpparser with no additional runtime dependencies.

**Header:** `#include "goc_http.h"`

`goc_http.h` is a separate header; include it alongside `goc.h`:

```c
#include "goc.h"
#include "goc_http.h"
```

---

## Table of Contents

- [Ping-Pong Example](#ping-pong-example)
- [Design](#design)
- [Thread Safety](#thread-safety)
- [Status Codes](#status-codes)
- [Data Structures](#data-structures)
- [1. Server Lifecycle](#1-server-lifecycle)
- [2. Routing](#2-routing)
- [3. Request Context](#3-request-context)
- [4. Sending Responses](#4-sending-responses)
- [5. Middleware](#5-middleware)
- [6. HTTP Client](#6-http-client)

---

## Ping-Pong Example

Two HTTP servers bounce a counter back and forth over HTTP.

- **Server A** (port 8080): on `POST /ping`, reads the counter from the body,
  increments it, and POSTs to server B. When the round limit is reached it
  responds `200` to its caller and stops.
- **Server B** (port 8081): on `POST /ping`, reads the counter, increments it,
  and POSTs back to server A.
- `main_fiber` sets up both servers, fires the first request, then waits on a
  done-channel that server A closes when finished.
- Both servers share the default thread pool.

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "goc.h"
#include "goc_http.h"

#define ROUNDS   10000
#define ADDR_A   "http://127.0.0.1:8080/ping"
#define ADDR_B   "http://127.0.0.1:8081/ping"

static goc_chan* done;   /* closed by server A when counter reaches ROUNDS */

/* Server A handler: receives counter, responds, forwards to B (or stops). */
static void handler_a(goc_http_ctx_t* ctx) {
    int n = atoi(goc_http_server_body_str(ctx));
    goc_chan* resp_ch = goc_http_server_respond(ctx, 200, "text/plain", "ok");
    goc_take(resp_ch);

    if (n >= ROUNDS) {
        goc_close(done);
        return;
    }

    char* msg = goc_sprintf("%d", n + 1);
    goc_http_request_opts_t* fwd_opts = goc_http_request_opts();
    fwd_opts->keep_alive = 1;
    goc_http_post(ADDR_B, "text/plain", msg, fwd_opts);
}

/* Server B handler: receives counter, responds, forwards to A. */
static void handler_b(goc_http_ctx_t* ctx) {
    int n = atoi(goc_http_server_body_str(ctx));
    goc_chan* resp_ch = goc_http_server_respond(ctx, 200, "text/plain", "ok");
    goc_take(resp_ch);

    char* msg = goc_sprintf("%d", n + 1);
    goc_http_request_opts_t* fwd_opts = goc_http_request_opts();
    fwd_opts->keep_alive = 1;
    goc_http_post(ADDR_A, "text/plain", msg, fwd_opts);
}

static void main_fiber(void* _) {
    done = goc_chan_make(0);

    goc_http_server_opts_t* opts_a = goc_http_server_opts();
    goc_http_server_t* a = goc_http_server_make(opts_a);
    goc_http_server_route(a, "POST", "/ping", handler_a);
    goc_chan* ready_a = goc_http_server_listen(a, "127.0.0.1", 8080);

    goc_http_server_opts_t* opts_b = goc_http_server_opts();
    goc_http_server_t* b = goc_http_server_make(opts_b);
    goc_http_server_route(b, "POST", "/ping", handler_b);
    goc_chan* ready_b = goc_http_server_listen(b, "127.0.0.1", 8081);

    /* Wait for both servers to be ready before sending the first request. */
    goc_take(ready_a);
    goc_take(ready_b);

    goc_http_request_opts_t* start_opts = goc_http_request_opts();
    start_opts->keep_alive = 1;
    goc_http_post(ADDR_A, "text/plain", "0", start_opts);

    goc_take(done);

    goc_chan* close_a = goc_http_server_close(a);
    goc_chan* close_b = goc_http_server_close(b);
    goc_take(close_a);
    goc_take(close_b);

    printf("ping-pong: %d round trips complete\n", ROUNDS);
}

int main(void) {
    goc_init();
    goc_go(main_fiber, NULL);
    goc_shutdown();
    return 0;
}
```

---

## Design

Both the server and client are built on `goc_io` TCP. In the multi-listener path, each pool worker uses its own `uv_loop_t` (see Thread Safety and the `SO_REUSEPORT`
section below); single-listener mode uses the global `g_loop`.

**Server**: `goc_http_server_listen` has two operating modes selected at runtime:

- **SO_REUSEPORT multi-listener** (Linux, pool size ≥ 2): one `uv_tcp_t` listener is created per pool worker, each bound to the same `host:port` with `SO_REUSEPORT`. The kernel load-balances `accept()` calls across all N listeners, distributing connections across workers without any application-level round-robin. Each worker runs its own accept-loop fiber (`reuseport_accept_loop_fiber`) that initialises its TCP handle on the worker's own `uv_loop_t`, avoiding cross-thread dispatch for all subsequent I/O on accepted connections.

- **Single-listener fallback** (pool size == 1 or Mac / Windows): a single GC-managed `uv_tcp_t` is bound and `goc_io_tcp_server_make` is used to obtain an accept channel. One accept-loop fiber dispatches all connections.

In both modes, each accepted connection is handled by a per-connection fiber spawned via `goc_go_on`. The connection fiber reads a complete HTTP/1.1 request via `goc_io_read_start`, parses it with `phr_parse_request` (picohttpparser), matches a route, runs middleware, calls the handler, and writes the response via `goc_io_write`. In the multi-listener path all I/O on a connection uses the same worker's loop directly (no cross-thread hops). No extra threads or mutexes are required beyond what `goc_io` already uses.

**Client**: outbound requests return a channel that delivers a
`goc_http_response_t*` when the response arrives. A single `http_client_fiber`
on the current-or-default pool drives the entire request: DNS lookup via
`goc_io_getaddrinfo`, TCP connect via `goc_io_tcp_connect`, write via
`goc_io_write`, and read/parse via `goc_io_read_start`. All I/O completes
through `goc_take` in the fiber. The event loop is never blocked; other fibers
continue to run while the request is in flight.

```
 incoming TCP connection
    │
    ▼
goc_io_tcp_server_make   ←── uv_loop_t ───► libgoc scheduler
    │ delivers uv_tcp_t* per connection
    │ accept-loop fiber calls goc_go(handle_conn_fiber)
    ▼
per-connection fiber
    │ goc_io_read_start → phr_parse_request
    │ route match + middleware chain
    │ goc_go(handler_fiber, ctx)
    ▼
handler fiber
    │ CSP operations (channels, I/O, timeouts, …)
    │ goc_take(goc_http_server_respond(ctx, …))
    ▼
goc_io_write (→ dispatch_on_handle_loop → direct call or post_on_loop → uv_write) → TCP → client
```

The raw connection handle is never exposed to user code. All access goes
through `goc_http_ctx_t` helpers; the context is valid only until
`goc_http_server_respond()` (or `goc_http_server_respond_error()`) delivers its result.

---

## Thread Safety

Server-side functions (`goc_http_server_respond`, `goc_http_server_header`, etc.) operate
within the per-connection fiber and do not need any cross-thread dispatch.
Client-side functions (`goc_http_get`, `goc_http_post`, etc.) launch a fiber
on the current-or-default pool that drives the entire request through `goc_io` channel
operations and are safe to call from any fiber.

`goc_http_ctx_t` must not be passed across independent requests or stored
beyond the lifetime of the handler fiber.

---

## Status Codes

```c
typedef enum {
    GOC_HTTP_ERR = 0,  /* operation failed          */
    GOC_HTTP_OK  = 1,  /* operation succeeded       */
} goc_http_status_t;
```

---

## Data Structures

```c
/* goc_http_server_t — opaque server object.
 * Created by goc_http_server_make(); destroyed by goc_http_server_close(). */
typedef struct goc_http_server goc_http_server_t;

/* goc_http_ctx_t — per-request context passed to handler fibers.
 * Valid only for the lifetime of the handler (until goc_http_server_respond*
 * delivers its result).  Do not store a pointer to this struct past that point. */
typedef struct {
    const char*  method;      /* "GET", "POST", … (null-terminated)         */
    const char*  path;        /* request path, e.g. "/api/ping"             */
    const char*  query;       /* query string without '?', or "" if absent  */
    goc_array*   headers;     /* goc_array of goc_http_header_t*            */
    goc_array*   body;        /* request body bytes; empty goc_array if none*/
    void*        user_data;   /* set by middleware; NULL by default         */
} goc_http_ctx_t;

/* goc_http_header_t — a single HTTP header name/value pair. */
typedef struct {
    const char*  name;
    const char*  value;
} goc_http_header_t;

/* goc_http_handler_t — user-supplied fiber body for a route. */
typedef void (*goc_http_handler_t)(goc_http_ctx_t* ctx);

/* goc_http_middleware_t — runs inside the per-request fiber before the handler.
 * May mutate ctx->user_data.  Return GOC_HTTP_OK to continue,
 * GOC_HTTP_ERR to short-circuit with a 500. */
typedef goc_http_status_t (*goc_http_middleware_t)(goc_http_ctx_t* ctx);

/* goc_http_server_opts_t — configuration passed to goc_http_server_make().
 *
 * Obtain a heap-allocated zero-initialised default with goc_http_server_opts();
 * set only the fields you need.
 *
 * Example (one middleware):
 *   goc_http_server_opts_t* opts = goc_http_server_opts();
 *   opts->middleware = goc_array_of(auth_mw);
 *   goc_http_server_t* srv = goc_http_server_make(opts);
 */
typedef struct {
    /* Middleware chain executed in order inside the per-request fiber before
     * the handler is called.  goc_array of goc_http_middleware_t function pointers.
     * Default (NULL): no middleware. */
    goc_array* middleware;
} goc_http_server_opts_t;

/* goc_http_response_t — result delivered by an outbound HTTP request channel.
 * All fields are GC-managed and valid for the lifetime of the fiber. */
typedef struct {
    int         status;   /* HTTP status code, e.g. 200, 404            */
    goc_array*  headers;  /* goc_array of goc_http_header_t*            */
    const char* body;     /* null-terminated response body              */
    size_t      body_len; /* byte length of body (excluding null byte)  */
} goc_http_response_t;

/* goc_http_request_opts_t — options for outbound requests.
 * Obtain a heap-allocated zero-initialised default with goc_http_request_opts(). */
typedef struct {
    /* Extra headers to send with the request.
     * goc_array of goc_http_header_t*.  Default (NULL): none. */
    goc_array* headers;

    /* Request timeout in milliseconds.  0 = no timeout. */
    uint64_t timeout_ms;

    /* Enable HTTP/1.1 keep-alive and connection reuse.
     * 0 = disabled (default), non-zero = enabled.
     */
    int keep_alive;
} goc_http_request_opts_t;
```

---

## 1. Server Lifecycle

| Function | Signature | Description |
|---|---|---|
| `goc_http_server_opts` | `goc_http_server_opts_t* goc_http_server_opts(void)` | Allocate and return a default options struct. `middleware` defaults to NULL. |
| `goc_http_server_make` | `goc_http_server_t* goc_http_server_make(const goc_http_server_opts_t* opts)` | Allocate and initialise a server from `opts`. Returns a GC-managed pointer. Never returns NULL (aborts on failure). |
| `goc_http_server_listen` | `goc_chan* goc_http_server_listen(goc_http_server_t* srv, const char* host, int port)` | Bind and start listening on `host:port`. On Linux with SO_REUSEPORT and a pool of size ≥ 2, creates one listener per worker for kernel-level load balancing. Falls back to a single listener otherwise. Returns a channel that delivers `goc_box_int(rc)` once all listeners are ready (rc == 0 = ready; rc < 0 = first libuv error). Always `goc_take()` the channel before sending requests. |
| `goc_http_server_reuseport_listener_count` | `int goc_http_server_reuseport_listener_count(goc_http_server_t* srv)` | Return the number of active SO_REUSEPORT listener slots for the server. Returns 0 on single-listener mode or if `srv` is NULL. |
| `goc_http_server_reuseport_listener_accept_count` | `int goc_http_server_reuseport_listener_accept_count(goc_http_server_t* srv, int slot)` | Return the number of accepted connections handled by the given reuseport listener slot. Meaningful only when `GOC_ENABLE_STATS` is enabled; otherwise returns 0. Returns 0 for single-listener mode or invalid slot numbers. |
| `goc_http_server_close` | `goc_chan* goc_http_server_close(goc_http_server_t* srv)` | Gracefully stop accepting new connections and drain in-flight requests. Returns a channel delivering `goc_box_int(0)` when shutdown is complete. Safe from any context. |

```c
goc_http_server_opts_t* opts = goc_http_server_opts();
goc_http_server_t* srv = goc_http_server_make(opts);
goc_chan* ready = goc_http_server_listen(srv, "0.0.0.0", 8080);
goc_val_t* ready_val = goc_take(ready);
int rc = (int)goc_unbox_int(ready_val->val);
if (rc < 0) { /* handle error */ }

// ... run until done ...

goc_chan* close_ch = goc_http_server_close(srv);
goc_take(close_ch);
```

---

## 2. Routing

Routes are matched in registration order. The first matching route wins.
A route pattern is a path prefix string; exact matching and simple wildcards
(`*`) are supported.

| Function | Signature | Description |
|---|---|---|
| `goc_http_server_route` | `void goc_http_server_route(goc_http_server_t* srv, const char* method, const char* pattern, goc_http_handler_t handler)` | Register a route. `method` may be `"GET"`, `"POST"`, etc., or `"*"` to match any method. `pattern` is matched as a path prefix; use `"/*"` to match all paths. Must be called before `goc_http_server_listen`. |

```c
goc_http_server_route(srv, "GET",  "/api/ping",  ping_handler);
goc_http_server_route(srv, "POST", "/api/echo",  echo_handler);
goc_http_server_route(srv, "*",    "/*",         not_found_handler);
```

Unmatched requests receive an automatic `404 Not Found`.

---

## 3. Request Context

Access request data through `goc_http_ctx_t` fields and the helpers below.
All returned strings are GC-managed and valid for the lifetime of the context.

| Function | Signature | Description |
|---|---|---|
| `goc_http_server_header` | `const char* goc_http_server_header(goc_http_ctx_t* ctx, const char* name)` | Return the value of the first header matching `name` (case-insensitive), or NULL if absent. |
| `goc_http_server_body_str` | `const char* goc_http_server_body_str(goc_http_ctx_t* ctx)` | Return request body as a null-terminated string. Empty string if no body. |

```c
void my_handler(goc_http_ctx_t* ctx) {
    const char* ct   = goc_http_server_header(ctx, "content-type");
    const char* body = goc_http_server_body_str(ctx);
    // ...
}
```

---

## 4. Sending Responses

Exactly one of these **must** be called before the handler fiber returns.
Calling more than one, or returning without calling either, is undefined behaviour.

Each function returns a channel that delivers `goc_box_int(rc)` (rc == 0 on
success, negative libuv error code on failure) once the response has been
flushed by libuv. `goc_take` it to wait for the flush before doing further
work that depends on the client having received the response.

| Function | Signature | Description |
|---|---|---|
| `goc_http_server_respond` | `goc_chan* goc_http_server_respond(goc_http_ctx_t* ctx, int status, const char* content_type, const char* body)` | Send a complete response. `body` is a null-terminated string. `content_type` may be NULL to default to `"text/plain"`. Returns a channel delivering `goc_box_int(0)` on flush. |
| `goc_http_server_respond_buf` | `goc_chan* goc_http_server_respond_buf(goc_http_ctx_t* ctx, int status, const char* content_type, const char* buf, size_t len)` | Same as `goc_http_server_respond` but for arbitrary bytes. Use for binary or pre-serialised payloads. |
| `goc_http_server_respond_error` | `goc_chan* goc_http_server_respond_error(goc_http_ctx_t* ctx, int status, const char* message)` | Send an error response with `text/plain` body. Convenience wrapper around `goc_http_server_respond`. |

```c
void ping_handler(goc_http_ctx_t* ctx) {
    goc_chan* resp_ch = goc_http_server_respond(ctx, 200, "text/plain", "pong");
    goc_take(resp_ch);
}

void echo_handler(goc_http_ctx_t* ctx) {
    const char* body = goc_http_server_body_str(ctx);
    goc_chan* resp_ch = goc_http_server_respond_buf(ctx, 200, "application/octet-stream",
                                                   body, strlen(body));
    goc_take(resp_ch);
}
```

---

## 5. Middleware

Middleware runs inside the per-request fiber, in registration order, before
the route handler is called. Because it runs in a fiber, it can do anything
a handler can — channel operations, `goc_io`, timeouts — without blocking the
event loop.

Returning `GOC_HTTP_ERR` from any middleware short-circuits the chain and
sends a `500` to the client; the handler is not called.

```c
goc_http_status_t auth_middleware(goc_http_ctx_t* ctx) {
    const char* tok = goc_http_server_header(ctx, "authorization");
    if (!tok) return GOC_HTTP_ERR;
    ctx->user_data = (void*)tok;
    return GOC_HTTP_OK;
}

goc_http_server_opts_t* opts = goc_http_server_opts();
opts->middleware = goc_array_of(auth_middleware);
goc_http_server_t* srv = goc_http_server_make(opts);
```

---

## 6. HTTP Client

All client functions return a channel that delivers a `goc_http_response_t*`
when the response arrives. `goc_take` the channel to receive the result.
Must be called from fiber context.

`goc_http_request_opts_t` carries headers, timeout, and a keep-alive toggle.
Pass `goc_http_request_opts()` for defaults (no extra headers, no timeout,
keep-alive disabled). Outbound request fibers execute on the current-or-default
pool automatically; there is no explicit HTTP pool override.

When `keep_alive` is enabled, the HTTP client reuses idle connections to the
same `host:port` within the same worker pool. Each worker maintains a local
idle connection stack, and workers in the same `goc_pool` may steal idle
connections from one another to improve reuse. Connection reuse is bounded by
an internal total connection cap (`GOC_HTTP_MAX_CONNECTIONS`, currently 64).
If the cap is exhausted, requests wait for a free slot with an internal queue
timeout (`GOC_HTTP_QUEUE_TIMEOUT_MS`, currently 10000 ms). Idle connections are
closed after an internal idle timeout (`GOC_HTTP_IDLE_TIMEOUT_MS`, currently
30000 ms), and new connections are established with a connect timeout of
`GOC_HTTP_CONNECT_TIMEOUT_MS` (3000 ms).

Because the functions return channels, multiple requests can be issued in
parallel and awaited with `goc_alts` or `goc_take_all`:

```c
goc_http_request_opts_t* opts1 = goc_http_request_opts();
goc_chan* c1 = goc_http_get(URL_A, opts1);

goc_http_request_opts_t* opts2 = goc_http_request_opts();
goc_chan* c2 = goc_http_get(URL_B, opts2);

goc_http_response_t* r1 = goc_take(c1)->val;
goc_http_response_t* r2 = goc_take(c2)->val;
```

### Options

| Function | Signature | Description |
|---|---|---|
| `goc_http_request_opts` | `goc_http_request_opts_t* goc_http_request_opts(void)` | Allocate and return a default request options struct. No headers, no timeout, keep-alive disabled. |

### REST helpers

| Function | Signature | Description |
|---|---|---|
| `goc_http_get` | `goc_chan* goc_http_get(const char* url, goc_http_request_opts_t* opts)` | Send a GET request. Channel delivers `goc_http_response_t*`. |
| `goc_http_post` | `goc_chan* goc_http_post(const char* url, const char* content_type, const char* body, goc_http_request_opts_t* opts)` | Send a POST request with a string body. Channel delivers `goc_http_response_t*`. |
| `goc_http_post_buf` | `goc_chan* goc_http_post_buf(const char* url, const char* content_type, const char* buf, size_t len, goc_http_request_opts_t* opts)` | Send a POST request with a binary body. Channel delivers `goc_http_response_t*`. |
| `goc_http_put` | `goc_chan* goc_http_put(const char* url, const char* content_type, const char* body, goc_http_request_opts_t* opts)` | Send a PUT request. Channel delivers `goc_http_response_t*`. |
| `goc_http_patch` | `goc_chan* goc_http_patch(const char* url, const char* content_type, const char* body, goc_http_request_opts_t* opts)` | Send a PATCH request. Channel delivers `goc_http_response_t*`. |
| `goc_http_delete` | `goc_chan* goc_http_delete(const char* url, goc_http_request_opts_t* opts)` | Send a DELETE request. Channel delivers `goc_http_response_t*`. |

### Generic

| Function | Signature | Description |
|---|---|---|
| `goc_http_request` | `goc_chan* goc_http_request(const char* method, const char* url, const char* content_type, const char* body, size_t body_len, goc_http_request_opts_t* opts)` | Send a request with an arbitrary method. `body` and `content_type` may be NULL for bodyless requests. Underlies all REST helpers. Channel delivers `goc_http_response_t*`. |

```c
/* simple GET */
goc_http_request_opts_t* get_opts = goc_http_request_opts();
goc_chan* get_ch = goc_http_get("http://api.example.com/items", get_opts);
goc_val_t* get_val = goc_take(get_ch);
goc_http_response_t* r = (goc_http_response_t*)get_val->val;
if (r->status == 200)
    printf("%s\n", r->body);

/* POST with a custom header */
goc_http_request_opts_t* opts = goc_http_request_opts();
goc_http_header_t* h = goc_malloc(sizeof(*h));
h->name  = "Authorization";
h->value = "Bearer my-token";
opts->headers    = goc_array_of(h);
opts->timeout_ms = 5000;
opts->keep_alive = 1;

goc_chan* post_ch = goc_http_post("http://api.example.com/items",
                                   "application/json",
                                   "{\"name\":\"foo\"}",
                                   opts);
goc_val_t* post_val = goc_take(post_ch);
goc_http_response_t* post_r = (goc_http_response_t*)post_val->val;
```

---

