/*
 * src/timeout.c — Timeout channels via libuv timers
 *
 * Implements: goc_timeout
 * All timer context structs are GC-allocated and registered via
 * gc_handle_register so the collector keeps them alive while libuv holds
 * references to them.
 *
 * Thread safety: goc_timeout may be called from any thread.  Timer
 * initialisation (uv_timer_init / uv_timer_start) is dispatched to the loop
 * thread via post_on_loop().
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
    GOC_DBG("on_timeout: timer=%p ch=%p\n", (void*)t, (void*)ch);
    atomic_fetch_add_explicit(&g_timeout_expirations, 1, memory_order_relaxed);
    goc_close(ch);
    uv_close((uv_handle_t*)t, unregister_handle_cb);
}

static void on_start_timer(void* arg)
{
    goc_timeout_req* req = (goc_timeout_req*)arg;
    GOC_DBG("on_start_timer: req=%p ms=%llu ch=%p\n",
            (void*)req, (unsigned long long)req->ms, (void*)req->timer_ctx->ch);

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
    GOC_DBG("on_start_timer: uv_timer_init timer=%p rc=%d remaining=%llu\n",
            (void*)&req->timer_ctx->timer, rc, (unsigned long long)remaining);
    if (rc < 0) {
        goc_close(req->timer_ctx->ch);
        gc_handle_unregister(req->timer_ctx);
        return;
    }

    rc = uv_timer_start(&req->timer_ctx->timer, on_timeout, remaining, 0);  /* one-shot */
    GOC_DBG("on_start_timer: uv_timer_start rc=%d\n", rc);
    if (rc < 0) {
        goc_close(req->timer_ctx->ch);
        uv_close((uv_handle_t*)&req->timer_ctx->timer, unregister_handle_cb);
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

goc_chan* goc_timeout(uint64_t ms)
{
    goc_chan*               ch   = goc_chan_make(0);   /* rendezvous channel */
    goc_timeout_req*        req  = (goc_timeout_req*)goc_malloc(sizeof(goc_timeout_req));
    goc_timeout_timer_ctx*  tctx = (goc_timeout_timer_ctx*)goc_malloc(sizeof(goc_timeout_timer_ctx));
    GOC_DBG("goc_timeout: ms=%llu ch=%p req=%p tctx=%p\n",
            (unsigned long long)ms, (void*)ch, (void*)req, (void*)tctx);

    tctx->ch = ch;

    req->timer_ctx   = tctx;
    req->ms          = ms;
    req->deadline_ns = uv_hrtime();  /* snapshot call time for dispatch-latency correction */

    /* Register tctx: it holds the timer handle and the channel pointer. */
    gc_handle_register(tctx);

    atomic_fetch_add_explicit(&g_timeout_allocations, 1, memory_order_relaxed);

    /* Dispatch timer start to the loop thread. */
    post_on_loop(on_start_timer, req);

    return ch;
}

void goc_timeout_get_stats(uint64_t *allocations, uint64_t *expirations)
{
    *allocations = atomic_load_explicit(&g_timeout_allocations, memory_order_relaxed);
    *expirations  = atomic_load_explicit(&g_timeout_expirations,  memory_order_relaxed);
}
