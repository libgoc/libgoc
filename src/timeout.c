/*
 * src/timeout.c — Timeout channels via libuv timers
 *
 * Implements: goc_timeout
 * All timer context structs are GC-allocated and registered via
 * gc_handle_register so the collector keeps them alive while libuv holds
 * references to them.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdint.h>
#include <uv.h>
#include "../include/goc.h"
#include "../include/goc_stats.h"
#include "internal.h"

/* Lifetime timeout counters — relaxed atomics, read via accessor at teardown */
static _Atomic uint64_t g_timeout_allocations = 0;
static _Atomic uint64_t g_timeout_expirations  = 0;

/* -------------------------------------------------------------------------
 * Internal types
 * ---------------------------------------------------------------------- */

typedef struct {
    uv_async_t  async;        /* MUST be first member — cast from uv_handle_t* is safe */
    struct goc_timeout_timer_ctx* timer_ctx;
    uint64_t    ms;
    uint64_t    deadline_ns;  /* uv_hrtime() snapshot taken at goc_timeout() call time */
} goc_timeout_req;

typedef struct goc_timeout_timer_ctx {
    uv_timer_t timer;         /* MUST be first member — freed from uv_close callback */
    goc_chan*  ch;
} goc_timeout_timer_ctx;

/* -------------------------------------------------------------------------
 * Static callbacks (loop thread)
 * ---------------------------------------------------------------------- */

static void unregister_handle_cb(uv_handle_t* h) { gc_handle_unregister(h); }

static void on_timeout(uv_timer_t* t)
{
    goc_timeout_timer_ctx* tctx = (goc_timeout_timer_ctx*)t;
    goc_chan* ch = tctx->ch;
    atomic_fetch_add_explicit(&g_timeout_expirations, 1, memory_order_relaxed);
    goc_close(ch);
    uv_close((uv_handle_t*)t, unregister_handle_cb);
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

    int rc = uv_timer_init(g_loop, &req->timer_ctx->timer);
    if (rc < 0) {
        goc_close(req->timer_ctx->ch);
        gc_handle_unregister(req->timer_ctx);
        uv_close((uv_handle_t*)h, unregister_handle_cb);
        return;
    }

    rc = uv_timer_start(&req->timer_ctx->timer, on_timeout, remaining, 0);  /* one-shot */
    if (rc < 0) {
        goc_close(req->timer_ctx->ch);
        uv_close((uv_handle_t*)&req->timer_ctx->timer, unregister_handle_cb);
        uv_close((uv_handle_t*)h, unregister_handle_cb);
        return;
    }

    /* Close the async handle; it is one-shot and no longer needed. */
    uv_close((uv_handle_t*)h, unregister_handle_cb);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

goc_chan* goc_timeout(uint64_t ms)
{
    goc_chan*               ch   = goc_chan_make(0);   /* rendezvous channel */
    goc_timeout_req*        req  = (goc_timeout_req*)goc_malloc(sizeof(goc_timeout_req));
    goc_timeout_timer_ctx*  tctx = (goc_timeout_timer_ctx*)goc_malloc(sizeof(goc_timeout_timer_ctx));

    tctx->ch = ch;

    req->timer_ctx   = tctx;
    req->ms          = ms;
    req->deadline_ns = uv_hrtime();  /* snapshot call time for dispatch-latency correction */

    /* Register both structs: req holds the async handle libuv will reference,
     * tctx holds the timer handle and the channel pointer. */
    gc_handle_register(req);
    gc_handle_register(tctx);

    /* uv_async_init is safe to call from any thread. */
    int rc = uv_async_init(g_loop, &req->async, on_start_timer);
    if (rc < 0) {
        goc_close(ch);
        gc_handle_unregister(req);
        gc_handle_unregister(tctx);
        return ch;
    }

    atomic_fetch_add_explicit(&g_timeout_allocations, 1, memory_order_relaxed);

    /* Signal the event loop thread to start the timer. */
    rc = uv_async_send(&req->async);
    if (rc < 0) {
        /* At this point req owns an initialized uv_async_t handle that must
         * be closed on the loop thread. If uv_async_send fails, we have no
         * thread-safe way to schedule that close reliably from here, so fail
         * fast instead of silently leaking a live handle and leaving timeout
         * semantics inconsistent. */
        fprintf(stderr, "libgoc: uv_async_send failed in goc_timeout: %s\n", uv_strerror(rc));
        abort();
    }

    return ch;
}

void goc_timeout_get_stats(uint64_t *allocations, uint64_t *expirations)
{
    *allocations = atomic_load_explicit(&g_timeout_allocations, memory_order_relaxed);
    *expirations  = atomic_load_explicit(&g_timeout_expirations,  memory_order_relaxed);
}
