/*
 * src/channel.c
 *
 * Channel implementation for libgoc.
 *
 * Defines:
 *   wake, compact_dead_entries
 *   goc_chan_make, goc_close
 *   goc_take, goc_put                  (fiber context)
 *   goc_take_sync, goc_put_sync        (OS thread context)
 *   goc_take_try                       (non-blocking, any context)
 *   goc_take_all                       (fiber context — receive from N channels)
 *   goc_take_all_sync                  (OS thread context — receive from N channels)
 *   goc_take_cb, goc_put_cb            (callback, loop-thread delivery)
 */

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <stdatomic.h>
#include <sched.h>
#include <uv.h>
#include <gc.h>
#include "minicoro.h"
#include "../include/goc.h"
#include "channel_internal.h"

/* --------------------------------------------------------------------------
 * wake
 *
 * Called under ch->lock. Unified wakeup path for a single parked entry.
 * Returns true when the entry is claimed and scheduled, false otherwise.
 * Steps:
 *   1. Acquire-load e->cancelled; if set → ch->dead_count++; return false.
 *   2. Call try_claim_wake(e):
 *      a. If e->fired != NULL (goc_alts entry): CAS fired 0→1 (acq_rel);
 *         if another arm already fired → return false.
 *      b. CAS e->woken 0→1 (acq_rel); if fails → return false.
 *   3. Write value to *e->result_slot (if non-NULL).
 *   4. Set e->ok = GOC_OK.
 *   5. Dispatch by kind.
 *
 * NOTE: goc_close does NOT call wake. It iterates the lists directly and
 * sets e->ok = GOC_CLOSED after winning try_claim_wake (see goc_close below).
 * Like wake(), goc_close must skip any entry where e->cancelled == 1.
 * Cancelled entries belong to goc_alts losers whose coroutine is already
 * MCO_DEAD; calling post_to_run_queue on them would resume a dead coroutine
 * and produce a SIGSEGV.
 * -------------------------------------------------------------------------- */
bool wake(goc_chan* ch, goc_entry* e, void* value)
{
    if (atomic_load_explicit(&e->cancelled, memory_order_acquire)) {
        ch->dead_count++;
        return false;
    }

    if (!try_claim_wake(e)) {
        ch->dead_count++;
        return false;
    }

    if (e->result_slot != NULL)
        *e->result_slot = value;

    e->ok = GOC_OK;

    switch (e->kind) {
    case GOC_FIBER: {
        /* Spin until the fiber has truly called mco_yield (parked == 1).
         * There is a brief window between when the parking fiber releases
         * ch->lock and when it calls mco_yield.  Calling post_to_run_queue
         * during that window causes a pool worker to call mco_resume on a
         * MCO_RUNNING coroutine, which silently fails, leaving the fiber
         * permanently suspended with no one to resume it (hang). */
        goc_entry* fe = (goc_entry*)mco_get_user_data(e->coro);
        while (atomic_load_explicit(&fe->parked, memory_order_acquire) == 0)
            sched_yield();
        post_to_run_queue(fe->pool, fe);
        break;
    }
    case GOC_CALLBACK:
        post_callback(e, value);
        break;
    case GOC_SYNC:
        goc_sync_post(e->sync_sem_ptr);
        break;
    }

    return true;
}

/* --------------------------------------------------------------------------
 * compact_dead_entries
 *
 * Must be called with ch->lock held. Walks both takers and putters lists
 * and unlinks every entry where cancelled == 1, then resets dead_count to 0.
 * Called at the top of goc_take and goc_put (after acquiring the lock) when
 * dead_count >= GOC_DEAD_COUNT_THRESHOLD. This amortises list cleanup without
 * requiring inline removal at cancellation time.
 * -------------------------------------------------------------------------- */
void compact_dead_entries(goc_chan* ch)
{
    /* Compact takers */
    goc_entry* last_taker = NULL;
    goc_entry** pp = &ch->takers;
    while (*pp) {
        goc_entry* e = *pp;
        if (atomic_load_explicit(&e->cancelled, memory_order_acquire))
            *pp = e->next;   /* unlink */
        else {
            last_taker = e;
            pp = &e->next;
        }
    }
    ch->takers_tail = last_taker;

    /* Compact putters */
    goc_entry* last_putter = NULL;
    pp = &ch->putters;
    while (*pp) {
        goc_entry* e = *pp;
        if (atomic_load_explicit(&e->cancelled, memory_order_acquire))
            *pp = e->next;
        else {
            last_putter = e;
            pp = &e->next;
        }
    }
    ch->putters_tail = last_putter;

    ch->dead_count = 0;
}

/* --------------------------------------------------------------------------
 * goc_chan_make
 * -------------------------------------------------------------------------- */
goc_chan* goc_chan_make(size_t buf_size)
{
    goc_chan* ch = goc_malloc(sizeof(goc_chan));   /* zero-initialised by GC_malloc */

    if (buf_size > 0)
        ch->buf = goc_malloc(buf_size * sizeof(void*));

    ch->buf_size = buf_size;

    ch->lock = malloc(sizeof(uv_mutex_t));        /* plain malloc — libuv constraint */
    uv_mutex_init(ch->lock);

    chan_register(ch);
    return ch;
}

/* --------------------------------------------------------------------------
 * chan_set_on_close
 *
 * Install or replace an on-close callback for a channel.
 * If the channel is already closed, invoke the callback immediately.
 * -------------------------------------------------------------------------- */
void chan_set_on_close(goc_chan* ch, void (*on_close)(void*), void* ud)
{
    int call_now = 0;

    uv_mutex_lock(ch->lock);
    if (ch->closed) {
        call_now = 1;
    } else {
        ch->on_close = on_close;
        ch->on_close_ud = ud;
    }
    uv_mutex_unlock(ch->lock);

    if (call_now && on_close != NULL)
        on_close(ud);
}

/* -------------------------------------------------------------------------
 * wake_all_parked_entries — Wake all entries in a waiter list with GOC_CLOSED
 *
 * Called by goc_close() to wake all parked entries. Skips cancelled entries
 * (goc_alts losers whose coroutines are already MCO_DEAD).
 * ---------------------------------------------------------------------- */
static void wake_all_parked_entries(goc_entry* head) {
    goc_entry* e = head;
    while (e != NULL) {
        /* Snapshot next before any dispatch: for GOC_FIBER entries,
         * post_to_run_queue may allow a pool thread to resume the fiber
         * immediately, and the parked entry can become unreachable/collected
         * before we iterate again. Reading e->next after dispatch is unsafe. */
        goc_entry* next = e->next;
        /* Skip cancelled entries (goc_alts losers): their coroutine is
         * already dead; calling post_to_run_queue on it would resume a
         * MCO_DEAD coro and crash.  This mirrors the same guard in wake(). */
        if (!atomic_load_explicit(&e->cancelled, memory_order_acquire)) {
            if (try_claim_wake(e)) {
                e->ok = GOC_CLOSED;
                switch (e->kind) {
                case GOC_FIBER: {
                    goc_entry* fe = (goc_entry*)mco_get_user_data(e->coro);
                    while (atomic_load_explicit(&fe->parked, memory_order_acquire) == 0)
                        sched_yield();
                    post_to_run_queue(fe->pool, fe);
                    break;
                }
                case GOC_CALLBACK: post_callback(e, NULL);        break;
                case GOC_SYNC:     goc_sync_post(e->sync_sem_ptr);     break;
                }
            }
        }
        e = next;
    }
}

/* --------------------------------------------------------------------------
 * goc_close
 *
 * Idempotent. CAS on close_guard ensures exactly one caller proceeds.
 * Does NOT destroy the mutex — ownership transfers to goc_shutdown (gc.c).
 * -------------------------------------------------------------------------- */
void goc_close(goc_chan* ch)
{
    void (*on_close)(void*) = NULL;
    void* on_close_ud = NULL;

    /* Serialise: only one caller wins the CAS 0→1 */
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &ch->close_guard, &expected, 1,
            memory_order_acq_rel, memory_order_acquire))
        return;

    uv_mutex_lock(ch->lock);
    ch->closed = 1;

    /* Wake all parked takers with GOC_CLOSED */
    wake_all_parked_entries(ch->takers);

    /* Wake all parked putters with GOC_CLOSED */
    wake_all_parked_entries(ch->putters);

    on_close = ch->on_close;
    on_close_ud = ch->on_close_ud;

    uv_mutex_unlock(ch->lock);
    chan_unregister(ch);

    if (on_close != NULL)
        on_close(on_close_ud);
}

/* --------------------------------------------------------------------------
 * goc_take  (fiber context only)
 * -------------------------------------------------------------------------- */
goc_val_t* goc_take(goc_chan* ch)
{
    if (mco_running() == NULL) {
        fprintf(stderr, "goc_take called from OS thread\n");
        abort();
    }

    uv_mutex_lock(ch->lock);

    /* Amortized cleanup: compact dead entries every GOC_DEAD_COUNT_THRESHOLD
     * cancelled entries to prevent waiter list bloat. Must be called under
     * ch->lock since it mutates ch->takers and ch->putters. */
    if (ch->dead_count >= GOC_DEAD_COUNT_THRESHOLD)
        compact_dead_entries(ch);

    void* val = NULL;

    /* Fast path: buffered value */
    if (chan_take_from_buffer(ch, &val)) {
        uv_mutex_unlock(ch->lock);
        goc_val_t* r = goc_malloc(sizeof(goc_val_t));
        r->val = val; r->ok = GOC_OK;
        return r;
    }

    /* Fast path: parked putter */
    if (chan_take_from_putter(ch, &val)) {
        uv_mutex_unlock(ch->lock);
        goc_val_t* r = goc_malloc(sizeof(goc_val_t));
        r->val = val; r->ok = GOC_OK;
        return r;
    }

    /* Fast path: closed and empty */
    if (ch->closed) {
        uv_mutex_unlock(ch->lock);
        goc_val_t* r = goc_malloc(sizeof(goc_val_t));
        r->val = NULL; r->ok = GOC_CLOSED;
        return r;
    }

    /* Slow path: park on this channel */
    void* local_result = NULL;

    goc_entry* e = goc_malloc(sizeof(goc_entry));
    *e = (goc_entry){ 0 };
    e->kind             = GOC_FIBER;
    e->coro             = mco_running();
    e->pool             = ((goc_entry*)mco_get_user_data(mco_running()))->pool;
    e->result_slot      = &local_result;
    e->ok               = GOC_CLOSED;   /* default; overwritten by wake on success */
    goc_stack_canary_init(e);

    /* Append to takers list */
    chan_list_append(&ch->takers, &ch->takers_tail, e);

    /* Set parked = 0 on the fiber's initial entry while ch->lock is still held.
     * wake() and goc_close() will spin until pool_worker_fn sets it back to 1
     * after mco_resume returns, guaranteeing the coroutine is truly
     * MCO_SUSPENDED before any worker calls mco_resume on it. */
    goc_entry* fiber_entry = (goc_entry*)mco_get_user_data(mco_running());
    atomic_store_explicit(&fiber_entry->parked, 0, memory_order_release);

    uv_mutex_unlock(ch->lock);
    mco_yield(mco_running());
    /* pool_worker_fn has set fiber_entry->parked = 1 by this point */

    goc_val_t* r = goc_malloc(sizeof(goc_val_t));
    r->val = local_result; r->ok = e->ok;
    return r;
}

/* --------------------------------------------------------------------------
 * goc_put  (fiber context only)
 * -------------------------------------------------------------------------- */
goc_status_t goc_put(goc_chan* ch, void* val)
{
    if (mco_running() == NULL) {
        fprintf(stderr, "goc_put called from OS thread\n");
        abort();
    }

    uv_mutex_lock(ch->lock);

    /* Amortized cleanup: compact dead entries every GOC_DEAD_COUNT_THRESHOLD
     * cancelled entries to prevent waiter list bloat. Must be called under
     * ch->lock since it mutates ch->takers and ch->putters. */
    if (ch->dead_count >= GOC_DEAD_COUNT_THRESHOLD)
        compact_dead_entries(ch);

    /* Fast path: channel closed.
     *
     * Checked before the parked-taker and buffer paths deliberately: sending
     * on a closed channel is always an error (matching Go's panic-on-send
     * semantics), regardless of whether a taker is waiting. This ordering
     * means a close that races with a concurrent put will either see the close
     * (returning GOC_CLOSED) or deliver normally — never deliver to a taker
     * on a channel that is already closed from the putter's point of view. */
    if (ch->closed) {
        uv_mutex_unlock(ch->lock);
        return GOC_CLOSED;
    }

    /* Fast path: parked taker */
    if (chan_put_to_taker(ch, val)) {
        uv_mutex_unlock(ch->lock);
        return GOC_OK;
    }

    /* Fast path: buffer space available */
    if (chan_put_to_buffer(ch, val)) {
        uv_mutex_unlock(ch->lock);
        return GOC_OK;
    }

    /* Slow path: park on this channel */
    goc_entry* e = goc_malloc(sizeof(goc_entry));
    *e = (goc_entry){ 0 };
    e->kind             = GOC_FIBER;
    e->coro             = mco_running();
    e->pool             = ((goc_entry*)mco_get_user_data(mco_running()))->pool;
    e->put_val          = val;
    e->ok               = GOC_CLOSED;   /* default; overwritten by wake on success */
    goc_stack_canary_init(e);

    /* Append to putters list */
    chan_list_append(&ch->putters, &ch->putters_tail, e);

    /* Same yield-gate as goc_take slow path. */
    goc_entry* fiber_entry = (goc_entry*)mco_get_user_data(mco_running());
    atomic_store_explicit(&fiber_entry->parked, 0, memory_order_release);

    uv_mutex_unlock(ch->lock);
    mco_yield(mco_running());
    /* pool_worker_fn has set fiber_entry->parked = 1 by this point */

    return e->ok;
}

/* --------------------------------------------------------------------------
 * goc_take_sync  (OS thread context)
 * -------------------------------------------------------------------------- */
goc_val_t* goc_take_sync(goc_chan* ch)
{
    if (mco_running() != NULL) {
        fprintf(stderr, "goc_take_sync called from fiber\n");
        abort();
    }

    uv_mutex_lock(ch->lock);

    void* val = NULL;

    /* Fast path: buffered value */
    if (chan_take_from_buffer(ch, &val)) {
        uv_mutex_unlock(ch->lock);
        goc_val_t* r = goc_malloc(sizeof(goc_val_t));
        r->val = val; r->ok = GOC_OK;
        return r;
    }

    /* Fast path: parked putter */
    if (chan_take_from_putter(ch, &val)) {
        uv_mutex_unlock(ch->lock);
        goc_val_t* r = goc_malloc(sizeof(goc_val_t));
        r->val = val; r->ok = GOC_OK;
        return r;
    }

    /* Fast path: closed and empty */
    if (ch->closed) {
        uv_mutex_unlock(ch->lock);
        goc_val_t* r = goc_malloc(sizeof(goc_val_t));
        r->val = NULL; r->ok = GOC_CLOSED;
        return r;
    }

    /* Slow path: park via condvar */
    goc_entry e = { 0 };
    e.kind         = GOC_SYNC;
    e.result_slot  = &e.cb_result;
    e.ok           = GOC_CLOSED;
    goc_sync_init(&e.sync_obj);
    e.sync_sem_ptr = &e.sync_obj;

    chan_list_append(&ch->takers, &ch->takers_tail, &e);

    uv_mutex_unlock(ch->lock);
    goc_sync_wait(&e.sync_obj);
    goc_sync_destroy(&e.sync_obj);

    goc_val_t* r = goc_malloc(sizeof(goc_val_t));
    r->val = e.cb_result; r->ok = e.ok;
    return r;
}

/* --------------------------------------------------------------------------
 * goc_put_sync  (OS thread context)
 * -------------------------------------------------------------------------- */
goc_status_t goc_put_sync(goc_chan* ch, void* val)
{
    if (mco_running() != NULL) {
        fprintf(stderr, "goc_put_sync called from fiber\n");
        abort();
    }

    uv_mutex_lock(ch->lock);

    /* Closed */
    if (ch->closed) {
        uv_mutex_unlock(ch->lock);
        return GOC_CLOSED;
    }

    /* FIX: check for a parked taker first and deliver directly, matching the
     * spec's priority order and the behaviour of goc_put. The previous code
     * checked for buffer space first and then called chan_put_to_taker with a
     * dummy NULL pointer — that woke the taker with a NULL result while the
     * real value sat in the ring buffer, causing the taker to receive NULL
     * instead of the enqueued value. */

    /* Parked taker: deliver directly */
    if (chan_put_to_taker(ch, val)) {
        uv_mutex_unlock(ch->lock);
        return GOC_OK;
    }

    /* Buffer space available */
    if (chan_put_to_buffer(ch, val)) {
        uv_mutex_unlock(ch->lock);
        return GOC_OK;
    }

    /* Slow path: park via condvar */
    goc_entry e = { 0 };
    e.kind         = GOC_SYNC;
    e.put_val      = val;
    e.ok           = GOC_CLOSED;
    goc_sync_init(&e.sync_obj);
    e.sync_sem_ptr = &e.sync_obj;

    chan_list_append(&ch->putters, &ch->putters_tail, &e);

    uv_mutex_unlock(ch->lock);
    goc_sync_wait(&e.sync_obj);
    goc_sync_destroy(&e.sync_obj);

    return e.ok;
}

/* --------------------------------------------------------------------------
 * goc_take_try  (non-blocking, any context)
 * -------------------------------------------------------------------------- */
goc_val_t* goc_take_try(goc_chan* ch)
{
    uv_mutex_lock(ch->lock);

    void* val = NULL;

    if (chan_take_from_buffer(ch, &val)) {
        uv_mutex_unlock(ch->lock);
        goc_val_t* r = goc_malloc(sizeof(goc_val_t));
        r->val = val; r->ok = GOC_OK;
        return r;
    }

    if (chan_take_from_putter(ch, &val)) {
        uv_mutex_unlock(ch->lock);
        goc_val_t* r = goc_malloc(sizeof(goc_val_t));
        r->val = val; r->ok = GOC_OK;
        return r;
    }

    if (ch->closed) {
        uv_mutex_unlock(ch->lock);
        goc_val_t* r = goc_malloc(sizeof(goc_val_t));
        r->val = NULL; r->ok = GOC_CLOSED;
        return r;
    }

    /* Open but empty */
    uv_mutex_unlock(ch->lock);
    goc_val_t* r = goc_malloc(sizeof(goc_val_t));
    r->val = NULL; r->ok = GOC_EMPTY;
    return r;
}

/* --------------------------------------------------------------------------
 * goc_take_all  (fiber context only)
 * -------------------------------------------------------------------------- */
goc_val_t** goc_take_all(goc_chan** chs, size_t n)
{
    goc_val_t** results = goc_malloc(n * sizeof(goc_val_t*));
    for (size_t i = 0; i < n; i++)
        results[i] = goc_take(chs[i]);
    return results;
}

/* --------------------------------------------------------------------------
 * goc_take_all_sync  (OS thread context)
 * -------------------------------------------------------------------------- */
goc_val_t** goc_take_all_sync(goc_chan** chs, size_t n)
{
    goc_val_t** results = goc_malloc(n * sizeof(goc_val_t*));
    for (size_t i = 0; i < n; i++)
        results[i] = goc_take_sync(chs[i]);
    return results;
}

/* --------------------------------------------------------------------------
 * goc_take_cb
 * -------------------------------------------------------------------------- */
void goc_take_cb(goc_chan* ch,
                 void (*cb)(void* val, goc_status_t ok, void* ud),
                 void* ud)
{
    assert(cb != NULL && "goc_take_cb: cb must not be NULL");

    goc_entry* e = goc_malloc(sizeof(goc_entry));
    e->kind        = GOC_CALLBACK;
    e->cb          = cb;
    e->ud          = ud;
    e->result_slot = &e->cb_result;

    uv_mutex_lock(ch->lock);

    void* val = NULL;

    /* Value available from buffer */
    if (chan_take_from_buffer(ch, &val)) {
        e->cb_result = val;
        e->ok        = GOC_OK;
        uv_mutex_unlock(ch->lock);
        post_callback(e, val);
        return;
    }

    /* Value available from parked putter */
    if (chan_take_from_putter(ch, &val)) {
        e->cb_result = val;
        e->ok        = GOC_OK;
        uv_mutex_unlock(ch->lock);
        post_callback(e, val);
        return;
    }

    /* Closed and empty */
    if (ch->closed) {
        e->ok = GOC_CLOSED;
        uv_mutex_unlock(ch->lock);
        post_callback(e, NULL);
        return;
    }

    /* Slow path: park */
    chan_list_append(&ch->takers, &ch->takers_tail, e);

    uv_mutex_unlock(ch->lock);
}

/* --------------------------------------------------------------------------
 * goc_put_cb
 * -------------------------------------------------------------------------- */
void goc_put_cb(goc_chan* ch, void* val,
                void (*cb)(goc_status_t ok, void* ud),
                void* ud)
{
    goc_entry* e = goc_malloc(sizeof(goc_entry));
    e->kind    = GOC_CALLBACK;
    e->put_val = val;
    e->put_cb  = cb;
    e->ud      = ud;

    uv_mutex_lock(ch->lock);

    /* Parked taker: deliver directly */
    if (chan_put_to_taker(ch, val)) {
        e->ok = GOC_OK;
        uv_mutex_unlock(ch->lock);
        if (cb != NULL)
            post_callback(e, NULL);
        return;
    }

    /* Buffer space available */
    if (chan_put_to_buffer(ch, val)) {
        e->ok = GOC_OK;
        uv_mutex_unlock(ch->lock);
        if (cb != NULL)
            post_callback(e, NULL);
        return;
    }

    /* Channel closed */
    if (ch->closed) {
        e->ok = GOC_CLOSED;
        uv_mutex_unlock(ch->lock);
        if (cb != NULL)
            post_callback(e, NULL);
        return;
    }

    /* Slow path: park */
    chan_list_append(&ch->putters, &ch->putters_tail, e);

    uv_mutex_unlock(ch->lock);
}
