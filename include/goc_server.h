/*
 * include/goc_server.h — HTTP/1.1 + HTTP/2 server and client for libgoc
 *
 * Provides a route-based HTTP server that dispatches requests into fibers,
 * and a channel-based client for making outbound HTTP calls.  Both are
 * powered by libh2o sharing libgoc's libuv event loop.
 *
 * Design
 * ------
 * Server: when a request arrives, H2O's on_req callback fires on the event
 * loop thread; goc_server immediately spawns a fiber via goc_go() and returns
 * 0 to H2O (signalling async handling).  The fiber owns the request until it
 * calls goc_server_respond().  Inside the fiber, the full libgoc API is
 * available: channels, goc_alts, timeouts, and all goc_io operations.
 *
 * Client: outbound requests return a channel that delivers a
 * goc_http_response_t* when the response arrives.  The event loop is never
 * blocked; other fibers continue to run while the request is in flight.
 *
 * Thread safety
 * -------------
 * All goc_server functions that touch H2O state dispatch work to the event
 * loop thread internally via a uv_async_t bridge (the same mechanism used by
 * goc_io).
 *
 * Include this header alongside goc.h:
 *   #include "goc.h"
 *   #include "goc_server.h"
 *
 * Compile requirements: -std=c11, link -lgoc (enabled by default; disable with -DLIBGOC_SERVER=OFF).
 *   (goc.h does NOT automatically include goc_server.h)
 *
 * See SERVER.md for the full API reference and design notes.
 */

#ifndef GOC_SERVER_H
#define GOC_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include "goc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Status codes
 * ====================================================================== */

typedef enum {
    GOC_SERVER_ERR = 0,  /* operation failed          */
    GOC_SERVER_OK  = 1,  /* operation succeeded       */
} goc_server_status_t;

/* =========================================================================
 * Data structures
 * ====================================================================== */

/* goc_server_t — opaque server object.
 * Created by goc_server_make(); destroyed by goc_server_close(). */
typedef struct goc_server goc_server_t;

/* goc_http_header_t — a single HTTP header name/value pair.
 * Both fields are GC-managed null-terminated strings. */
typedef struct {
    const char* name;
    const char* value;
} goc_http_header_t;

/* goc_http_ctx_t — per-request context passed to handler fibers.
 * Valid only for the lifetime of the handler (until goc_server_respond*
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
 * Return GOC_SERVER_OK to continue, GOC_SERVER_ERR to short-circuit with
 * a 500 response. */
typedef goc_server_status_t (*goc_http_middleware_t)(goc_http_ctx_t* ctx);

/* goc_server_opts_t — configuration passed to goc_server_make().
 *
 * Obtain a heap-allocated zero-initialised default with goc_server_opts();
 * set only the fields you need.
 *
 * Example (all defaults):
 *   goc_server_t* srv = goc_server_make(goc_server_opts());
 *
 * Example (custom pool, one middleware):
 *   goc_server_opts_t* opts = goc_server_opts();
 *   opts->pool       = my_pool;
 *   opts->middleware = goc_array_of(auth_mw);
 *   goc_server_t* srv = goc_server_make(opts);
 */
typedef struct {
    /* Pool whose libuv loop the server runs on.
     * Default (NULL): goc_default_pool(). */
    goc_pool* pool;

    /* Middleware chain executed in order inside the per-request fiber before
     * the handler is called.  goc_array of goc_http_middleware_t function
     * pointers.  Default (NULL): no middleware. */
    goc_array* middleware;
} goc_server_opts_t;

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
    /* Extra headers to send with the request.
     * goc_array of goc_http_header_t*.  Default (NULL): none. */
    goc_array* headers;

    /* Request timeout in milliseconds.  0 = no timeout. */
    uint64_t timeout_ms;
} goc_http_request_opts_t;

/* =========================================================================
 * 1. Server lifecycle
 * ====================================================================== */

/**
 * goc_server_opts — allocate and return a default options struct.
 *
 * All fields zero-initialised: pool defaults to goc_default_pool(),
 * middleware to NULL.  Never returns NULL (aborts on allocation failure).
 */
goc_server_opts_t* goc_server_opts(void);

/**
 * goc_server_make — allocate and initialise a server from opts.
 *
 * Returns a GC-managed pointer.  Never returns NULL (aborts on failure).
 * Store routes with goc_server_route() before calling goc_server_listen().
 */
goc_server_t* goc_server_make(const goc_server_opts_t* opts);

/**
 * goc_server_listen — bind and start listening on host:port.
 *
 * Returns 0 on success, negative libuv error code on failure.
 * Safe to call from main() before the fiber scheduler has started.
 */
int goc_server_listen(goc_server_t* srv, const char* host, int port);

/**
 * goc_server_close — gracefully stop the server.
 *
 * Stops accepting new connections and drains in-flight requests.
 * Returns a channel delivering goc_box_int(0) when shutdown is complete.
 */
goc_chan* goc_server_close(goc_server_t* srv);

/* =========================================================================
 * 2. Routing
 * ====================================================================== */

/**
 * goc_server_route — register a route.
 *
 * method  : "GET", "POST", etc., or "*" to match any method.
 * pattern : path prefix string.  Use "/*" to match all paths.
 * handler : fiber body called with the per-request goc_http_ctx_t*.
 *
 * Routes are matched in registration order; the first match wins.
 * Unmatched requests receive an automatic 404 Not Found.
 * Must be called before goc_server_listen().
 */
void goc_server_route(goc_server_t* srv, const char* method,
                      const char* pattern, goc_http_handler_t handler);

/* =========================================================================
 * 3. Request context helpers
 * ====================================================================== */

/**
 * goc_server_header — return the value of the first header matching name.
 *
 * Comparison is case-insensitive.
 * Returns NULL if the header is absent.
 */
const char* goc_server_header(goc_http_ctx_t* ctx, const char* name);

/**
 * goc_server_body_str — return the request body as a null-terminated string.
 *
 * Returns "" for an empty body.  The returned pointer is GC-managed and
 * valid for the lifetime of ctx.
 */
const char* goc_server_body_str(goc_http_ctx_t* ctx);

/* =========================================================================
 * 4. Sending responses
 * ====================================================================== */

/**
 * goc_server_respond — send a complete string response.
 *
 * body         : null-terminated string body.
 * content_type : MIME type; NULL defaults to "text/plain".
 *
 * Returns a channel delivering goc_box_int(0) when the response has been
 * flushed by libuv.  Exactly one respond* function must be called per
 * handler invocation.
 */
goc_chan* goc_server_respond(goc_http_ctx_t* ctx, int status,
                             const char* content_type, const char* body);

/**
 * goc_server_respond_buf — send a binary response.
 *
 * Same as goc_server_respond but accepts an arbitrary byte buffer.
 * Use for binary or pre-serialised payloads.
 */
goc_chan* goc_server_respond_buf(goc_http_ctx_t* ctx, int status,
                                 const char* content_type,
                                 const char* buf, size_t len);

/**
 * goc_server_respond_error — send an error response.
 *
 * Convenience wrapper around goc_server_respond with "text/plain".
 */
goc_chan* goc_server_respond_error(goc_http_ctx_t* ctx, int status,
                                   const char* message);

/* =========================================================================
 * 6. HTTP client
 * ====================================================================== */

/**
 * goc_http_request_opts — allocate and return a default request options
 * struct.  Zero-initialised: no extra headers, no timeout.
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

#endif /* GOC_SERVER_H */
