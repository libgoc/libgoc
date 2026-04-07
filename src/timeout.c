/*
 * src/timeout.c — Timeout channels via the central timer manager
 *
 * Implements: goc_timeout
 * Each goc_timeout call allocates a GC-managed goc_timeout_timer_ctx and
 * inserts it into the central min-heap timer manager (loop.c).  No per-timeout
 * uv_timer_t handle is allocated; the manager owns a single long-lived
 * uv_timer_t for the entire loop.
 *
 * Thread safety: goc_timeout may be called from any thread.  Timer insertion
 * (goc_timer_manager_insert) is dispatched to the loop thread via post_on_loop().
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdint.h>
#include <uv.h>
#include "../include/goc.h"
#include "../include/goc_stats.h"
#include "internal.h"
#include "channel_internal.h"

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

/* goc_timeout_timer_ctx is defined in internal.h */

static void on_cancel_timer(void* arg)
{
    goc_timeout_timer_ctx* tctx = (goc_timeout_timer_ctx*)arg;
    int state = atomic_load_explicit(&tctx->start_state, memory_order_acquire);
    int cancel = atomic_load_explicit(&tctx->cancel_requested, memory_order_acquire);

    GOC_DBG("on_cancel_timer: tctx=%p ch=%p state=%d cancel=%d\n",
            (void*)tctx, (void*)tctx->ch, state, cancel);

    if (state != 1) {
        GOC_DBG("on_cancel_timer: skipping remove because start_state=%d\n", state);
        return;
    }

    atomic_store_explicit(&tctx->start_state, 2, memory_order_release);
    goc_timer_manager_remove(goc_global_timer_mgr(), tctx);
}

static void on_timeout_channel_closed(void* ud)
{
    goc_timeout_timer_ctx* tctx = (goc_timeout_timer_ctx*)ud;
    atomic_store_explicit(&tctx->cancel_requested, 1, memory_order_release);
    GOC_DBG("on_timeout_channel_closed: tctx=%p ch=%p state=%d\n",
            (void*)tctx, (void*)tctx->ch,
            atomic_load_explicit(&tctx->start_state, memory_order_acquire));
    post_on_loop(on_cancel_timer, tctx);
}

/* -------------------------------------------------------------------------
 * goc_timeout_ctx_expire — called by the central timer manager (loop thread)
 * ---------------------------------------------------------------------- */

void goc_timeout_ctx_expire(goc_timeout_timer_ctx* tctx)
{
    int state = atomic_load_explicit(&tctx->start_state, memory_order_acquire);
    int cancel = atomic_load_explicit(&tctx->cancel_requested, memory_order_acquire);
    GOC_DBG("goc_timeout_ctx_expire: tctx=%p ch=%p start_state=%d cancel_requested=%d\n",
            (void*)tctx, (void*)tctx->ch, state, cancel);
    atomic_fetch_add_explicit(&g_timeout_expirations, 1, memory_order_relaxed);
    goc_close(tctx->ch);
    /* start_state is set to 2 by the caller (manager) before invoking this. */
}

/* -------------------------------------------------------------------------
 * on_start_timer — loop thread; inserts tctx into the central timer manager
 * ---------------------------------------------------------------------- */

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

    if (atomic_load_explicit(&req->timer_ctx->cancel_requested, memory_order_acquire)) {
        GOC_DBG("on_start_timer: cancel requested before insert req=%p tctx=%p ch=%p\n",
                (void*)req, (void*)req->timer_ctx, (void*)req->timer_ctx->ch);
        return;  /* nothing registered yet; GC will collect tctx */
    }

    atomic_store_explicit(&req->timer_ctx->start_state, 1, memory_order_release);

    if (atomic_load_explicit(&req->timer_ctx->cancel_requested, memory_order_acquire)) {
        GOC_DBG("on_start_timer: cancel requested after state=1 req=%p tctx=%p\n",
                (void*)req, (void*)req->timer_ctx);
        atomic_store_explicit(&req->timer_ctx->start_state, 2, memory_order_release);
        return;  /* haven't inserted into manager yet; nothing to remove */
    }

    uint64_t fire_at_ns = now_ns + remaining * 1000000ULL;
    GOC_DBG("on_start_timer: inserting into manager tctx=%p fire_at_ns=%llu remaining=%llu\n",
            (void*)req->timer_ctx, (unsigned long long)fire_at_ns,
            (unsigned long long)remaining);
    goc_timer_manager_insert(goc_global_timer_mgr(), req->timer_ctx, fire_at_ns);
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
    atomic_store_explicit(&tctx->start_state, 0, memory_order_relaxed);
    atomic_store_explicit(&tctx->cancel_requested, 0, memory_order_relaxed);
    tctx->heap_idx = SIZE_MAX;

    req->timer_ctx   = tctx;
    req->ms          = ms;
    req->deadline_ns = uv_hrtime();  /* snapshot call time for dispatch-latency correction */

    /* tctx lifetime is managed by the GC-allocated timer heap inside the
     * central manager.  No gc_handle_register call is needed here. */
    chan_set_on_close(ch, on_timeout_channel_closed, tctx);

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
