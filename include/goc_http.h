/*
 * include/goc_http.h — HTTP/1.1 server and client for libgoc
 *
 * Provides a route-based HTTP/1.1 server that dispatches requests into fibers,
 * and a channel-based client for making outbound HTTP/1.1 calls.  Both are
 * built on goc_io TCP and the vendored picohttpparser (MIT) — no additional
 * runtime dependencies beyond libuv.
 *
 * Design
 * ------
 * Server: goc_http_server_listen binds a GC-managed uv_tcp_t and calls
 * goc_io_tcp_server_make to obtain an accept channel.  An accept-loop fiber
 * runs on the caller's pool, dispatching each accepted connection to a
 * per-connection fiber.  The connection fiber reads a complete HTTP/1.1
 * request via goc_io, parses it with picohttpparser, matches a route, runs
 * middleware, calls the handler, and writes the response.  The fiber owns the
 * request until it calls goc_http_server_respond().
 *
 * Client: outbound requests return a channel that delivers a
 * goc_http_response_t* when the response arrives.  The event loop is never
 * blocked; other fibers continue to run while the request is in flight.
 *
 * Thread safety
 * -------------
 * All goc_http_server functions that operate on connection state run inside
 * fibers on the caller's pool.  Response delivery dispatches to the event
 * loop thread internally via a uv_async_t bridge (the same mechanism used by
 * goc_io).
 *
 * Include this header alongside goc.h:
 *   #include "goc.h"
 *   #include "goc_http.h"
 *
 * Compile requirements: -std=c11, link -lgoc (enabled by default; disable with -DLIBGOC_SERVER=OFF).
 *   (goc.h does NOT automatically include goc_http.h)
 *
 * See HTTP.md for the full API reference and design notes.
 */

#ifndef GOC_HTTP_H
#define GOC_HTTP_H

#include <stddef.h>
#include <stdint.h>
#include "goc.h"
#include "goc_array.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Status codes
 * ====================================================================== */

typedef enum {
    GOC_HTTP_ERR = 0,  /* operation failed          */
    GOC_HTTP_OK  = 1,  /* operation succeeded       */
} goc_http_status_t;

/* =========================================================================
 * Data structures
 * ====================================================================== */

/* goc_http_server_t — opaque server object.
 * Created by goc_http_server_make(); destroyed by goc_http_server_close(). */
typedef struct goc_http_server goc_http_server_t;

/* goc_http_header_t — a single HTTP header name/value pair.
 * Both fields are GC-managed null-terminated strings. */
typedef struct {
    const char* name;
    const char* value;
} goc_http_header_t;

/* goc_http_ctx_t — per-request context passed to handler fibers.
 * Valid only for the lifetime of the handler (until goc_http_server_respond*
 * delivers its result).  Do not store a pointer to this struct past that
 * point. */
typedef struct {
    const char* method;      /* "GET", "POST", … (null-terminated)         */
    const char* path;        /* request path, e.g. "/api/ping"             */
    const char* query;       /* query string without '?', or "" if absent  */
    goc_array*  headers;     /* goc_array of goc_http_header_t*            */
    goc_array*  body;        /* request body bytes; empty goc_array if none*/
    void*       user_data;   /* set by middleware; NULL by default         */
} goc_http_ctx_t;

/* goc_http_handler_t — user-supplied fiber body for a route. */
typedef void (*goc_http_handler_t)(goc_http_ctx_t* ctx);

/* goc_http_middleware_t — runs inside the per-request fiber before the
 * handler is called.  May mutate ctx->user_data.
 * Return GOC_HTTP_OK to continue, GOC_HTTP_ERR to short-circuit with
 * a 500 response. */
typedef goc_http_status_t (*goc_http_middleware_t)(goc_http_ctx_t* ctx);

/* goc_http_server_opts_t — configuration passed to goc_http_server_make().
 *
 * Obtain a heap-allocated zero-initialised default with goc_http_server_opts();
 * set only the fields you need.
 *
 * Example (all defaults):
 *   goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
 *
 * Example (custom pool, one middleware):
 *   goc_http_server_opts_t* opts = goc_http_server_opts();
 *   opts->pool       = my_pool;
 *   opts->middleware = goc_array_of(auth_mw);
 *   goc_http_server_t* srv = goc_http_server_make(opts);
 */
typedef struct {
    /* Pool whose libuv loop the server runs on.
     * Default (NULL): goc_default_pool(). */
    goc_pool* pool;

    /* Middleware chain executed in order inside the per-request fiber before
     * the handler is called.  goc_array of goc_http_middleware_t function
     * pointers.  Default (NULL): no middleware. */
    goc_array* middleware;
} goc_http_server_opts_t;

/* goc_http_response_t — result delivered by an outbound HTTP request channel.
 * All fields are GC-managed and valid for the lifetime of the fiber that
 * goc_take()s the channel. */
typedef struct {
    int         status;    /* HTTP status code, e.g. 200, 404            */
    goc_array*  headers;   /* goc_array of goc_http_header_t*            */
    const char* body;      /* null-terminated response body              */
    size_t      body_len;  /* byte length of body (excluding null byte)  */
} goc_http_response_t;

/* goc_http_request_opts_t — options for outbound HTTP requests.
 * Obtain a heap-allocated zero-initialised default with
 * goc_http_request_opts(); set only the fields you need. */
typedef struct {
    /* Pool to run the outbound client fiber on.
     * Default (NULL): goc_current_or_default_pool(). */
    goc_pool* pool;

    /* Extra headers to send with the request.
     * goc_array of goc_http_header_t*.  Default (NULL): none. */
    goc_array* headers;

    /* Request timeout in milliseconds.  0 = no timeout. */
    uint64_t timeout_ms;

    /* Enable HTTP/1.1 keep-alive and connection reuse.
     * 0 = disabled (default), non-zero = enabled.
     *
     * When enabled, goc_http may reuse an existing connection to the same
     * host:port for subsequent requests on the same worker thread.
     */
    int keep_alive;
} goc_http_request_opts_t;

/* =========================================================================
 * 1. Server lifecycle
 * ====================================================================== */

/**
 * goc_http_server_opts — allocate and return a default options struct.
 *
 * All fields zero-initialised: pool defaults to goc_default_pool(),
 * middleware to NULL.  Never returns NULL (aborts on allocation failure).
 */
goc_http_server_opts_t* goc_http_server_opts(void);

/**
 * goc_http_server_make — allocate and initialise a server from opts.
 *
 * Returns a GC-managed pointer.  Never returns NULL (aborts on failure).
 * Store routes with goc_http_server_route() before calling goc_http_server_listen().
 */
goc_http_server_t* goc_http_server_make(const goc_http_server_opts_t* opts);

/**
 * goc_http_server_listen — bind and start listening on host:port.
 *
 * Returns a channel that delivers goc_box_int(rc) once the server is
 * actually listening (rc == 0) or has failed (rc < 0, libuv error code).
 * Always call goc_take() on the returned channel before sending any
 * requests to guarantee the server is ready to accept connections.
 *
 * Synchronous errors (address parse, TCP init, bind) are delivered on
 * the channel rather than returned directly, so the call pattern is
 * always:
 *
 *   goc_chan* ready = goc_http_server_listen(srv, host, port);
 *   goc_take(ready);
 */
goc_chan* goc_http_server_listen(goc_http_server_t* srv, const char* host, int port);

/**
 * goc_http_server_close — gracefully stop the server.
 *
 * Stops accepting new connections and drains in-flight requests.
 * Waits for all active connections to close before completing shutdown.
 * Returns a channel delivering goc_box_int(0) when shutdown is complete.
 */
goc_chan* goc_http_server_close(goc_http_server_t* srv);

/* =========================================================================
 * 2. Routing
 * ====================================================================== */

/**
 * goc_http_server_route — register a route.
 *
 * method  : "GET", "POST", etc., or "*" to match any method.
 * pattern : path prefix string.  Use "/*" to match all paths.
 * handler : fiber body called with the per-request goc_http_ctx_t*.
 *
 * Routes are matched in registration order; the first match wins.
 * Unmatched requests receive an automatic 404 Not Found.
 * Must be called before goc_http_server_listen().
 */
void goc_http_server_route(goc_http_server_t* srv, const char* method,
                      const char* pattern, goc_http_handler_t handler);

/* =========================================================================
 * 3. Request context helpers
 * ====================================================================== */

/**
 * goc_http_server_header — return the value of the first header matching name.
 *
 * Comparison is case-insensitive.
 * Returns NULL if the header is absent.
 */
const char* goc_http_server_header(goc_http_ctx_t* ctx, const char* name);

/**
 * goc_http_server_body_str — return the request body as a null-terminated string.
 *
 * Returns "" for an empty body.  The returned pointer is GC-managed and
 * valid for the lifetime of ctx.
 */
const char* goc_http_server_body_str(goc_http_ctx_t* ctx);

/* =========================================================================
 * 4. Sending responses
 * ====================================================================== */

/**
 * goc_http_server_respond — send a complete string response.
 *
 * body         : null-terminated string body.
 * content_type : MIME type; NULL defaults to "text/plain".
 *
 * Returns a channel delivering goc_box_int(0) when the response has been
 * flushed by libuv.  Exactly one respond* function must be called per
 * handler invocation.
 */
goc_chan* goc_http_server_respond(goc_http_ctx_t* ctx, int status,
                             const char* content_type, const char* body);

/**
 * goc_http_server_respond_buf — send a binary response.
 *
 * Same as goc_http_server_respond but accepts an arbitrary byte buffer.
 * Use for binary or pre-serialised payloads.
 */
goc_chan* goc_http_server_respond_buf(goc_http_ctx_t* ctx, int status,
                                 const char* content_type,
                                 const char* buf, size_t len);

/**
 * goc_http_server_respond_error — send an error response.
 *
 * Convenience wrapper around goc_http_server_respond with "text/plain".
 */
goc_chan* goc_http_server_respond_error(goc_http_ctx_t* ctx, int status,
                                   const char* message);

/* =========================================================================
 * 6. HTTP client
 * ====================================================================== */

/**
 * goc_http_request_opts — allocate and return a default request options
 * struct.  Zero-initialised: no extra headers, no timeout,
 * keep-alive disabled, and pool set to goc_current_or_default_pool().
 */
goc_http_request_opts_t* goc_http_request_opts(void);

/**
 * goc_http_request — send an HTTP request with an arbitrary method.
 *
 * method       : "GET", "POST", etc.
 * url          : full URL, e.g. "http://api.example.com/items".
 * content_type : MIME type for the body; NULL for bodyless requests.
 * body         : request body bytes; NULL for bodyless requests.
 * body_len     : byte length of body; 0 for bodyless requests.
 * opts         : request options; use goc_http_request_opts() for defaults.
 *
 * Returns a channel that delivers a goc_http_response_t* when the response
 * arrives.  Must be called from fiber context.
 */
goc_chan* goc_http_request(const char* method, const char* url,
                           const char* content_type,
                           const char* body, size_t body_len,
                           goc_http_request_opts_t* opts);

/** goc_http_get — send a GET request.  Channel delivers goc_http_response_t*. */
goc_chan* goc_http_get(const char* url, goc_http_request_opts_t* opts);

/** goc_http_post — send a POST request with a string body. */
goc_chan* goc_http_post(const char* url, const char* content_type,
                        const char* body, goc_http_request_opts_t* opts);

/** goc_http_post_buf — send a POST request with a binary body. */
goc_chan* goc_http_post_buf(const char* url, const char* content_type,
                            const char* buf, size_t len,
                            goc_http_request_opts_t* opts);

/** goc_http_put — send a PUT request. */
goc_chan* goc_http_put(const char* url, const char* content_type,
                       const char* body, goc_http_request_opts_t* opts);

/** goc_http_patch — send a PATCH request. */
goc_chan* goc_http_patch(const char* url, const char* content_type,
                         const char* body, goc_http_request_opts_t* opts);

/** goc_http_delete — send a DELETE request. */
goc_chan* goc_http_delete(const char* url, goc_http_request_opts_t* opts);

#ifdef __cplusplus
}
#endif

#endif /* GOC_HTTP_H */
