/*
 * src/timeout.c — Timeout channels via libuv timers
 *
 * Implements: goc_timeout
 * All timer handles are malloc-allocated and live on the libuv event loop
 * thread. The GC heap is never used for handle storage.
 */

#include <stdlib.h>
#include <uv.h>
#include "../include/goc.h"
#include "internal.h"

/* -------------------------------------------------------------------------
 * Internal types
 * ---------------------------------------------------------------------- */

typedef struct {
    uv_async_t  async;        /* MUST be first member — cast from uv_handle_t* is safe */
    goc_chan*   ch;
    uv_timer_t* t;
    uint64_t    ms;
    uint64_t    deadline_ns;  /* uv_hrtime() snapshot taken at goc_timeout() call time */
} goc_timeout_req;

/* -------------------------------------------------------------------------
 * Static callbacks (loop thread)
 * ---------------------------------------------------------------------- */

static void free_handle_cb(uv_handle_t* h) { free(h); }

static void on_timeout(uv_timer_t* t)
{
    goc_chan* ch = (goc_chan*)t->data;
    goc_close(ch);
    uv_close((uv_handle_t*)t, free_handle_cb);
}

static void on_start_timer(uv_async_t* h)
{
    goc_timeout_req* req = (goc_timeout_req*)h;

    /* Subtract the time already spent waiting for the async dispatch so that
     * the timer fires at the wall-clock deadline recorded in goc_timeout(),
     * not req->ms after the loop thread happens to process this callback.
     * Clamp to zero: if the deadline already passed, fire on the next loop
     * iteration (remaining == 0), which is the correct "expired" behaviour. */
    uint64_t now_ns     = uv_hrtime();
    uint64_t elapsed_ns = (now_ns > req->deadline_ns)
                        ? (now_ns - req->deadline_ns) : 0;
    uint64_t elapsed_ms = elapsed_ns / 1000000ULL;
    uint64_t remaining  = (elapsed_ms < req->ms) ? (req->ms - elapsed_ms) : 0;

    uv_timer_init(g_loop, req->t);
    uv_timer_start(req->t, on_timeout, remaining, 0);  /* one-shot */

    /* Close the async handle; it is one-shot and no longer needed. */
    uv_close((uv_handle_t*)h, free_handle_cb);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

goc_chan* goc_timeout(uint64_t ms)
{
    goc_chan*         ch  = goc_chan_make(0);   /* rendezvous channel */
    goc_timeout_req*  req = malloc(sizeof(goc_timeout_req));
    uv_timer_t*       t   = malloc(sizeof(uv_timer_t));

    t->data          = ch;
    req->ch          = ch;
    req->t           = t;
    req->ms          = ms;
    req->deadline_ns = uv_hrtime();  /* snapshot call time for dispatch-latency correction */

    /* uv_async_init is safe to call from any thread. */
    uv_async_init(g_loop, &req->async, on_start_timer);

    /* Signal the event loop thread to start the timer. */
    uv_async_send(&req->async);

    return ch;
}
