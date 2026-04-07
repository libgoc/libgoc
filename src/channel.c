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
#include "../include/goc_stats.h"
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
        {
            long _spin = 0;
            while (atomic_load_explicit(&fe->parked, memory_order_acquire) == 0) {
                sched_yield();
                if (++_spin == 10000000L) {
                    GOC_DBG("wake(): SPIN STALL >10M iters coro=%p fe=%p ch=%p\n",
                            (void*)e->coro, (void*)fe, (void*)ch);
                }
            }
            if (_spin > 100) { GOC_DBG("wake(): spin resolved after %ld iters coro=%p\n", _spin, (void*)e->coro); }
        }
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
 * wake_claim
 *
 * Like wake(), but for GOC_FIBER entries does NOT spin or post. Instead it
 * returns the fiber's goc_entry (fe) so the caller can release ch->lock
 * before spinning.  For GOC_CALLBACK and GOC_SYNC it dispatches immediately
 * (those paths have no spin) and returns NULL.
 *
 * Caller contract for a non-NULL return value:
 *   1. Release ch->lock.
 *   2. Spin: while (atomic_load(&fe->parked, acquire) == 0) sched_yield();
 *   3. post_to_run_queue(fe->pool, fe);
 * -------------------------------------------------------------------------- */
bool wake_claim(goc_chan* ch, goc_entry* e, void* value, goc_entry** fe_out)
{
    *fe_out = NULL;

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
    case GOC_FIBER:
        *fe_out = (goc_entry*)mco_get_user_data(e->coro);
        return true;
    case GOC_CALLBACK:
        post_callback(e, value);
        return true;
    case GOC_SYNC:
        goc_sync_post(e->sync_sem_ptr);
        return true;
    }
    return true; /* unreachable */
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
#ifdef GOC_ENABLE_STATS
    uint64_t removed = 0;
#endif
    /* Compact takers */
    goc_entry* last_taker = NULL;
    goc_entry** pp = &ch->takers;
    while (*pp) {
        goc_entry* e = *pp;
        if (atomic_load_explicit(&e->cancelled, memory_order_acquire)) {
            GOC_DBG("compact_dead_entries: unlinking taker e=%p ch=%p kind=%d coro=%p\n",
                    (void*)e, (void*)ch, (int)e->kind,
                    (e->kind == GOC_FIBER ? (void*)e->coro : NULL));
            *pp = e->next;   /* unlink */
            if (e->free_on_drain) free(e);
#ifdef GOC_ENABLE_STATS
            removed++;
#endif
        } else {
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
        if (atomic_load_explicit(&e->cancelled, memory_order_acquire)) {
            *pp = e->next;
            if (e->free_on_drain) free(e);
#ifdef GOC_ENABLE_STATS
            removed++;
#endif
        } else {
            last_putter = e;
            pp = &e->next;
        }
    }
    ch->putters_tail = last_putter;

    ch->dead_count = 0;
#ifdef GOC_ENABLE_STATS
    atomic_fetch_add_explicit(&ch->compaction_runs,  1,       memory_order_relaxed);
    atomic_fetch_add_explicit(&ch->entries_removed,  removed, memory_order_relaxed);
#endif
}

/* --------------------------------------------------------------------------
 * goc_chan_make
 * -------------------------------------------------------------------------- */
goc_chan* goc_chan_make(size_t buf_size)
{
    GOC_DBG("goc_chan_make: before goc_malloc(chan) buf_size=%zu\n", buf_size);
    goc_chan* ch = goc_malloc(sizeof(goc_chan));   /* zero-initialised by GC_malloc */
    GOC_DBG("goc_chan_make: after goc_malloc(chan) ch=%p\n", (void*)ch);

    if (buf_size > 0) {
        GOC_DBG("goc_chan_make: before goc_malloc(buf) ch=%p\n", (void*)ch);
        ch->buf = goc_malloc(buf_size * sizeof(void*));
        GOC_DBG("goc_chan_make: after goc_malloc(buf) ch=%p buf=%p\n", (void*)ch, (void*)ch->buf);
    }

    ch->buf_size = buf_size;
    ch->item_count = 0;

    ch->lock = malloc(sizeof(uv_mutex_t));        /* plain malloc — libuv constraint */
    uv_mutex_init(ch->lock);

    chan_register(ch);
    /* Telemetry: channel opened — scan/compaction counters start at 0 */
    GOC_STATS_CHANNEL_STATUS((int)(intptr_t)ch, /*status=*/1, (int)ch->buf_size, (int)ch->item_count,
                             0, 0, 0, 0);
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
/* --------------------------------------------------------------------------
 * goc_close
 *
 * Idempotent. CAS on close_guard ensures exactly one caller proceeds.
 * Does NOT destroy the mutex — ownership transfers to goc_shutdown (gc.c).
 * -------------------------------------------------------------------------- */

int goc_chan_is_closing(goc_chan* ch)
{
    return atomic_load_explicit(&ch->close_guard, memory_order_acquire);
}

void goc_close(goc_chan* ch)
{
    void (*on_close)(void*) = NULL;
    void* on_close_ud = NULL;

    GOC_DBG("goc_close: called on ch=%p\n", (void*)ch);

    /* Serialise: only one caller wins the CAS 0→1 */
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &ch->close_guard, &expected, 1,
            memory_order_acq_rel, memory_order_acquire)) {
        GOC_DBG("goc_close: ch=%p CAS lost expected=%d now=%d\n",
                (void*)ch, expected,
                atomic_load_explicit(&ch->close_guard, memory_order_acquire));
        return;
    }
    GOC_DBG("goc_close: ch=%p CAS won\n", (void*)ch);

    uv_mutex_lock(ch->lock);
    ch->closed = 1;
    GOC_DBG("goc_close: ch=%p locked, takers=%p putters=%p item_count=%zu\n",
            (void*)ch, (void*)ch->takers, (void*)ch->putters, ch->item_count);

    /* Count non-cancelled waiters: used for the to_post heap allocation and
     * (when GOC_ENABLE_STATS) for the close telemetry counter.  Entries may
     * be stack-allocated (GOC_SYNC) and freed the moment we post them, so we
     * must not read them after the wake loop. */
    size_t waiter_count = 0;
    for (goc_entry* e = ch->takers; e != NULL; e = e->next)
        if (!atomic_load_explicit(&e->cancelled, memory_order_acquire))
            waiter_count++;
    for (goc_entry* e = ch->putters; e != NULL; e = e->next)
        if (!atomic_load_explicit(&e->cancelled, memory_order_acquire))
            waiter_count++;
#ifdef GOC_ENABLE_STATS
    uint64_t close_removed = (uint64_t)waiter_count;
#endif

    /* Collect GOC_FIBER entries to post after lock release.
     * Use plain malloc (not goc_malloc/GC_malloc) to avoid triggering a GC
     * stop-the-world while ch->lock is held, which can deadlock if another
     * thread is blocked waiting on the same lock. */
    goc_entry** to_post = waiter_count ? malloc(sizeof(goc_entry*) * waiter_count) : NULL;
    size_t to_post_count = 0;
    int cancelled_found = 0;
    for (int _pass = 0; _pass < 2; _pass++) {
        goc_entry* e = (_pass == 0) ? ch->takers : ch->putters;
        while (e != NULL) {
            goc_entry* next = e->next;
            if (atomic_load_explicit(&e->cancelled, memory_order_acquire)) {
                cancelled_found = 1;
            } else if (try_claim_wake(e)) {
                GOC_DBG("goc_close: claimed waiter ch=%p e=%p kind=%d arm_pass=%d coro=%p\n",
                        (void*)ch, (void*)e, (int)e->kind, _pass,
                        (e->kind == GOC_FIBER ? (void*)e->coro : NULL));
                e->ok = GOC_CLOSED;
                if (e->kind == GOC_FIBER)
                    to_post[to_post_count++] = (goc_entry*)mco_get_user_data(e->coro);
                else if (e->kind == GOC_CALLBACK)
                    post_callback(e, NULL);
                else if (e->kind == GOC_SYNC)
                    goc_sync_post(e->sync_sem_ptr);
            } else {
                GOC_DBG("goc_close: try_claim_wake failed ch=%p e=%p kind=%d arm_pass=%d cancelled=%d woken=%d fired=%p\n",
                        (void*)ch, (void*)e, (int)e->kind, _pass,
                        (int)atomic_load_explicit(&e->cancelled, memory_order_acquire),
                        (int)atomic_load_explicit(&e->woken, memory_order_acquire),
                        (void*)e->fired);
            }
            e = next;
        }
    }

    /* If any cancelled entries remain, compact them before releasing the lock */
    if (cancelled_found)
        compact_dead_entries(ch);

    /* Release lock before posting */
    uv_mutex_unlock(ch->lock);

    /* Spin and post for all collected GOC_FIBER entries */
    for (size_t i = 0; i < to_post_count; i++) {
        goc_entry* fe = to_post[i];
        long _spin = 0;
        while (atomic_load_explicit(&fe->parked, memory_order_acquire) == 0) {
            sched_yield();
            if (++_spin == 10000000L) {
                GOC_DBG("goc_close(): SPIN STALL >10M iters ch=%p fe=%p\n",
                        (void*)ch, (void*)fe);
            }
        }
        if (_spin > 100) { GOC_DBG("goc_close(): spin resolved after %ld iters ch=%p\n", _spin, (void*)ch); }
        post_to_run_queue(fe->pool, fe);
    }
    free(to_post);

    /* Telemetry: update counters for entries woken by close, then emit event */
#ifdef GOC_ENABLE_STATS
    if (close_removed > 0) {
        atomic_fetch_add_explicit(&ch->entries_removed, close_removed, memory_order_relaxed);
        atomic_fetch_add_explicit(&ch->compaction_runs, 1,             memory_order_relaxed);
    }
#endif
    GOC_STATS_CHANNEL_STATUS((int)(intptr_t)ch, /*status=*/0, (int)ch->buf_size, (int)ch->item_count,
                             atomic_load_explicit(&ch->taker_scans,     memory_order_relaxed),
                             atomic_load_explicit(&ch->putter_scans,    memory_order_relaxed),
                             atomic_load_explicit(&ch->compaction_runs, memory_order_relaxed),
                             atomic_load_explicit(&ch->entries_removed, memory_order_relaxed));

    on_close = ch->on_close;
    on_close_ud = ch->on_close_ud;

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
        fprintf(stderr, "libgoc: goc_take: called from OS thread (not in fiber context)\n");
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
        GOC_DBG("goc_take: fast-path BUFFERED ch=%p val=%p\n", (void*)ch, val);
        goc_val_t* r = goc_malloc(sizeof(goc_val_t));
        r->val = val; r->ok = GOC_OK;
        return r;
    }

    /* Fast path: parked putter — claim under lock, spin+post outside */
    goc_entry* fe_putter = NULL;
    if (chan_take_from_putter_claim(ch, &val, &fe_putter)) {
        uv_mutex_unlock(ch->lock);
        if (fe_putter != NULL) {
            long _spin = 0;
            while (atomic_load_explicit(&fe_putter->parked, memory_order_acquire) == 0) {
                sched_yield();
                if (++_spin == 10000000L) { GOC_DBG("goc_take fast-path: SPIN STALL >10M iters ch=%p fe_putter=%p\n", (void*)ch, (void*)fe_putter); }
            }
            post_to_run_queue(fe_putter->pool, fe_putter);
        }
        GOC_DBG("goc_take: fast-path PUTTER ch=%p val=%p\n", (void*)ch, val);
        goc_val_t* r = goc_malloc(sizeof(goc_val_t));
        r->val = val; r->ok = GOC_OK;
        return r;
    }

    /* Fast path: closed and empty */
    if (ch->closed) {
        GOC_DBG("goc_take: fast-path CLOSED ch=%p item_count=%zu putters=%p\n",
                (void*)ch, ch->item_count, (void*)ch->putters);
        uv_mutex_unlock(ch->lock);
        goc_val_t* r = goc_malloc(sizeof(goc_val_t));
        r->val = NULL; r->ok = GOC_CLOSED;
        return r;
    }

    /* Slow path: park on this channel */
    void* local_result = NULL;

    goc_entry* e = malloc(sizeof(goc_entry));
    *e = (goc_entry){ 0 };
    e->kind             = GOC_FIBER;
    e->coro             = mco_running();
    e->pool             = ((goc_entry*)mco_get_user_data(mco_running()))->pool;
    e->result_slot      = &local_result;
    e->ok               = GOC_CLOSED;   /* default; overwritten by wake on success */
    goc_stack_canary_init(e);

    GOC_DBG("goc_take: parking coro=%p on ch=%p\n", (void*)mco_running(), (void*)ch);

    /* Append to takers list */
    GOC_DBG("takers_append: ch=%p e=%p kind=%d coro=%p\n",
            (void*)ch, (void*)e, (int)e->kind, (void*)e->coro);
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

    GOC_DBG("goc_take: resumed coro=%p ch=%p ok=%d val=%p\n", (void*)mco_running(), (void*)ch, (int)e->ok, local_result);
    goc_val_t* r = goc_malloc(sizeof(goc_val_t));
    r->val = local_result; r->ok = e->ok;
    free(e);
    return r;
}

/* --------------------------------------------------------------------------
 * goc_put  (fiber context only)
 * -------------------------------------------------------------------------- */
goc_status_t goc_put(goc_chan* ch, void* val)
{
    if (mco_running() == NULL) {
        fprintf(stderr, "libgoc: goc_put: called from OS thread (not in fiber context)\n");
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

    /* Fast path: parked taker — claim under lock, spin+post outside */
    goc_entry* fe_taker = NULL;
    if (chan_put_to_taker_claim(ch, val, &fe_taker)) {
        uv_mutex_unlock(ch->lock);
        if (fe_taker != NULL) {
            long _spin = 0;
            while (atomic_load_explicit(&fe_taker->parked, memory_order_acquire) == 0) {
                sched_yield();
                if (++_spin == 10000000L) { GOC_DBG("goc_put fast-path: SPIN STALL >10M iters ch=%p fe_taker=%p\n", (void*)ch, (void*)fe_taker); }
            }
            post_to_run_queue(fe_taker->pool, fe_taker);
        }
        return GOC_OK;
    }

    /* Fast path: buffer space available */
    if (chan_put_to_buffer(ch, val)) {
        uv_mutex_unlock(ch->lock);
        return GOC_OK;
    }

    /* Slow path: park on this channel */
    goc_entry* e = malloc(sizeof(goc_entry));
    *e = (goc_entry){ 0 };
    e->kind             = GOC_FIBER;
    e->coro             = mco_running();
    e->pool             = ((goc_entry*)mco_get_user_data(mco_running()))->pool;
    e->put_val          = val;
    e->ok               = GOC_CLOSED;   /* default; overwritten by wake on success */
    goc_stack_canary_init(e);

    GOC_DBG("goc_put: parking coro=%p on ch=%p\n", (void*)mco_running(), (void*)ch);

    /* Append to putters list */
    chan_list_append(&ch->putters, &ch->putters_tail, e);

    /* Same yield-gate as goc_take slow path. */
    goc_entry* fiber_entry = (goc_entry*)mco_get_user_data(mco_running());
    atomic_store_explicit(&fiber_entry->parked, 0, memory_order_release);

    uv_mutex_unlock(ch->lock);
    mco_yield(mco_running());
    /* pool_worker_fn has set fiber_entry->parked = 1 by this point */

    GOC_DBG("goc_put: resumed coro=%p ch=%p ok=%d\n", (void*)mco_running(), (void*)ch, (int)e->ok);
    goc_status_t ok = e->ok;
    free(e);
    return ok;
}

/* --------------------------------------------------------------------------
 * goc_take_sync  (OS thread context)
 * -------------------------------------------------------------------------- */
goc_val_t* goc_take_sync(goc_chan* ch)
{
    if (mco_running() != NULL) {
        fprintf(stderr, "libgoc: goc_take_sync: called from fiber context\n");
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

    GOC_DBG("takers_append: ch=%p e=%p kind=%d coro=%p\n",
            (void*)ch, (void*)&e, (int)e.kind, NULL);
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
        fprintf(stderr, "libgoc: goc_put_sync: called from fiber context\n");
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
 *
 * Non-blocking: posts the pending take to the loop thread, which will
 * perform the channel operation under ch->lock and fire cb on delivery.
 * -------------------------------------------------------------------------- */
void goc_take_cb(goc_chan* ch,
                 void (*cb)(void* val, goc_status_t ok, void* ud),
                 void* ud)
{
    assert(cb != NULL && "goc_take_cb: cb must not be NULL");

    goc_entry* e = malloc(sizeof(goc_entry));
    *e = (goc_entry){ 0 };
    e->kind          = GOC_CALLBACK;
    e->ch            = ch;
    e->is_put        = false;
    e->cb            = cb;
    e->ud            = ud;
    e->result_slot   = &e->cb_result;
    e->free_on_drain = true;

    post_callback(e, NULL);
}

/* --------------------------------------------------------------------------
 * goc_put_cb
 *
 * Non-blocking: posts the pending put to the loop thread, which will
 * perform the channel operation under ch->lock and fire cb on delivery.
 * -------------------------------------------------------------------------- */
void goc_put_cb(goc_chan* ch, void* val,
                void (*cb)(goc_status_t ok, void* ud),
                void* ud)
{
    GOC_DBG("goc_put_cb: ch=%p val=%p put_cb=%p\n", (void*)ch, val, (void*)(uintptr_t)cb);
    goc_entry* e = malloc(sizeof(goc_entry));
    *e = (goc_entry){ 0 };
    e->kind          = GOC_CALLBACK;
    e->ch            = ch;
    e->is_put        = true;
    e->put_val       = val;
    e->put_cb        = cb;
    e->ud            = ud;
    e->free_on_drain = true;

    post_callback(e, NULL);
}
