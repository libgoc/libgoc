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
 *   goc_take_cb, goc_put_cb            (callback, loop-thread delivery)
 */

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <stdatomic.h>
#include <semaphore.h>
#include <uv.h>
#include <gc.h>
#include "minicoro.h"
#include "../include/goc.h"
#include "chan_type.h"
#include "internal.h"

/* --------------------------------------------------------------------------
 * wake
 *
 * Called under ch->lock. Unified wakeup path for a single parked entry.
 * Steps:
 *   1. Acquire-load e->cancelled; if set → ch->dead_count++; return.
 *   2. CAS e->woken 0→1 (acq_rel); if fails → ch->dead_count++; return.
 *   3. Write value to *e->result_slot (if non-NULL).
 *   4. Set e->ok = GOC_OK.
 *   5. Dispatch by kind.
 *
 * NOTE: goc_close does NOT call wake. It iterates the lists directly and
 * sets e->ok = GOC_CLOSED after winning the CAS (see goc_close below).
 * Like wake(), goc_close must skip any entry where e->cancelled == 1.
 * Cancelled entries belong to goc_alts losers whose coroutine is already
 * MCO_DEAD; calling post_to_run_queue on them would resume a dead coroutine
 * and produce a SIGSEGV.
 * -------------------------------------------------------------------------- */
void wake(goc_chan* ch, goc_entry* e, void* value)
{
    if (atomic_load_explicit(&e->cancelled, memory_order_acquire)) {
        ch->dead_count++;
        return;
    }

    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &e->woken, &expected, 1,
            memory_order_acq_rel, memory_order_acquire)) {
        ch->dead_count++;
        return;
    }

    if (e->result_slot != NULL)
        *e->result_slot = value;

    e->ok = GOC_OK;

    switch (e->kind) {
    case GOC_FIBER:
        post_to_run_queue(e->pool, e);
        break;
    case GOC_CALLBACK:
        post_callback(e, value);
        break;
    case GOC_SYNC:
        sem_post(e->sync_sem_ptr);
        break;
    }
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
    goc_entry** pp = &ch->takers;
    while (*pp) {
        goc_entry* e = *pp;
        if (atomic_load_explicit(&e->cancelled, memory_order_acquire))
            *pp = e->next;   /* unlink */
        else
            pp = &e->next;
    }

    /* Compact putters */
    pp = &ch->putters;
    while (*pp) {
        goc_entry* e = *pp;
        if (atomic_load_explicit(&e->cancelled, memory_order_acquire))
            *pp = e->next;
        else
            pp = &e->next;
    }

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
 * goc_close
 *
 * Idempotent. CAS on close_guard ensures exactly one caller proceeds.
 * Does NOT destroy the mutex — ownership transfers to goc_shutdown (gc.c).
 * -------------------------------------------------------------------------- */
void goc_close(goc_chan* ch)
{
    /* Serialise: only one caller wins the CAS 0→1 */
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &ch->close_guard, &expected, 1,
            memory_order_acq_rel, memory_order_acquire))
        return;

    uv_mutex_lock(ch->lock);
    ch->closed = 1;

    /* Wake all parked takers with GOC_CLOSED */
    {
        goc_entry* e = ch->takers;
        while (e != NULL) {
            /* Snapshot next before any dispatch: for GOC_FIBER entries,
             * post_to_run_queue may allow a pool thread to resume the fiber
             * immediately, which can cause the entry to be freed (if it was
             * stack-allocated in goc_take) or collected (if GC-heap allocated
             * in goc_alts).  Reading e->next after dispatch is use-after-free.
             * This mirrors the same fix in chan_put_to_taker. */
            goc_entry* next = e->next;
            /* Skip cancelled entries (goc_alts losers): their coroutine is
             * already dead; calling post_to_run_queue on it would resume a
             * MCO_DEAD coro and crash.  This mirrors the same guard in wake(). */
            if (!atomic_load_explicit(&e->cancelled, memory_order_acquire)) {
                int exp = 0;
                if (atomic_compare_exchange_strong_explicit(
                        &e->woken, &exp, 1,
                        memory_order_acq_rel, memory_order_acquire)) {
                    e->ok = GOC_CLOSED;
                    switch (e->kind) {
                    case GOC_FIBER:    post_to_run_queue(e->pool, e); break;
                    case GOC_CALLBACK: post_callback(e, NULL);        break;
                    case GOC_SYNC:     sem_post(e->sync_sem_ptr);     break;
                    }
                }
            }
            e = next;
        }
    }

    /* Wake all parked putters with GOC_CLOSED */
    {
        goc_entry* e = ch->putters;
        while (e != NULL) {
            /* Same next-snapshot fix as the takers loop above. */
            goc_entry* next = e->next;
            /* Same cancelled guard as the takers loop above. */
            if (!atomic_load_explicit(&e->cancelled, memory_order_acquire)) {
                int exp = 0;
                if (atomic_compare_exchange_strong_explicit(
                        &e->woken, &exp, 1,
                        memory_order_acq_rel, memory_order_acquire)) {
                    e->ok = GOC_CLOSED;
                    switch (e->kind) {
                    case GOC_FIBER:    post_to_run_queue(e->pool, e); break;
                    case GOC_CALLBACK: post_callback(e, NULL);        break;
                    case GOC_SYNC:     sem_post(e->sync_sem_ptr);     break;
                    }
                }
            }
            e = next;
        }
    }

    uv_mutex_unlock(ch->lock);
    chan_unregister(ch);
}

/* --------------------------------------------------------------------------
 * goc_take  (fiber context only)
 * -------------------------------------------------------------------------- */
goc_val_t goc_take(goc_chan* ch)
{
    if (mco_running() == NULL) {
        fprintf(stderr, "goc_take called from OS thread\n");
        abort();
    }

    uv_mutex_lock(ch->lock);

    /* FIX: compact_dead_entries must be called under the lock — it mutates
     * ch->takers, ch->putters, and ch->dead_count. The threshold check uses
     * the current value of dead_count, which is now read while holding the
     * lock and therefore consistent. */
    if (ch->dead_count >= GOC_DEAD_COUNT_THRESHOLD)
        compact_dead_entries(ch);

    void* val = NULL;

    /* Fast path: buffered value */
    if (chan_take_from_buffer(ch, &val)) {
        uv_mutex_unlock(ch->lock);
        return (goc_val_t){ val, GOC_OK };
    }

    /* Fast path: parked putter */
    if (chan_take_from_putter(ch, &val)) {
        uv_mutex_unlock(ch->lock);
        return (goc_val_t){ val, GOC_OK };
    }

    /* Fast path: closed and empty */
    if (ch->closed) {
        uv_mutex_unlock(ch->lock);
        return (goc_val_t){ NULL, GOC_CLOSED };
    }

    /* Slow path: park on this channel */
    void* local_result = NULL;

    goc_entry e = { 0 };
    e.kind             = GOC_FIBER;
    e.coro             = mco_running();
    e.pool             = ((goc_entry*)mco_get_user_data(mco_running()))->pool;
    e.result_slot      = &local_result;
    e.ok               = GOC_CLOSED;   /* default; overwritten by wake on success */
    e.stack_canary_ptr = (uint32_t*)e.coro->stack_base;

    /* Append to takers list */
    goc_entry** pp = &ch->takers;
    while (*pp) pp = &(*pp)->next;
    *pp = &e;

    void* stack_base = e.coro->stack_base;
    void* stack_top  = (char*)stack_base + e.coro->stack_size;
    GC_add_roots(stack_base, stack_top);

    uv_mutex_unlock(ch->lock);
    mco_yield(mco_running());

    GC_remove_roots(stack_base, stack_top);

    return (goc_val_t){ local_result, e.ok };
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

    /* FIX: compact_dead_entries must be called under the lock — it mutates
     * ch->takers, ch->putters, and ch->dead_count. The threshold check uses
     * the current value of dead_count, which is now read while holding the
     * lock and therefore consistent. */
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
    goc_entry e = { 0 };
    e.kind             = GOC_FIBER;
    e.coro             = mco_running();
    e.pool             = ((goc_entry*)mco_get_user_data(mco_running()))->pool;
    e.put_val          = val;
    e.ok               = GOC_CLOSED;   /* default; overwritten by wake on success */
    e.stack_canary_ptr = (uint32_t*)e.coro->stack_base;

    /* Append to putters list */
    goc_entry** pp = &ch->putters;
    while (*pp) pp = &(*pp)->next;
    *pp = &e;

    void* stack_base = e.coro->stack_base;
    void* stack_top  = (char*)stack_base + e.coro->stack_size;
    GC_add_roots(stack_base, stack_top);

    uv_mutex_unlock(ch->lock);
    mco_yield(mco_running());

    GC_remove_roots(stack_base, stack_top);

    return e.ok;
}

/* --------------------------------------------------------------------------
 * goc_take_sync  (OS thread context)
 * -------------------------------------------------------------------------- */
goc_val_t goc_take_sync(goc_chan* ch)
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
        return (goc_val_t){ val, GOC_OK };
    }

    /* Fast path: parked putter */
    if (chan_take_from_putter(ch, &val)) {
        uv_mutex_unlock(ch->lock);
        return (goc_val_t){ val, GOC_OK };
    }

    /* Fast path: closed and empty */
    if (ch->closed) {
        uv_mutex_unlock(ch->lock);
        return (goc_val_t){ NULL, GOC_CLOSED };
    }

    /* Slow path: park via semaphore */
    goc_entry e = { 0 };
    e.kind         = GOC_SYNC;
    e.result_slot  = &e.cb_result;
    e.ok           = GOC_CLOSED;
    sem_init(&e.sync_sem, 0, 0);
    e.sync_sem_ptr = &e.sync_sem;

    goc_entry** pp = &ch->takers;
    while (*pp) pp = &(*pp)->next;
    *pp = &e;

    uv_mutex_unlock(ch->lock);
    sem_wait(&e.sync_sem);
    sem_destroy(&e.sync_sem);

    return (goc_val_t){ e.cb_result, e.ok };
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

    /* Slow path: park via semaphore */
    goc_entry e = { 0 };
    e.kind         = GOC_SYNC;
    e.put_val      = val;
    e.ok           = GOC_CLOSED;
    sem_init(&e.sync_sem, 0, 0);
    e.sync_sem_ptr = &e.sync_sem;

    goc_entry** pp = &ch->putters;
    while (*pp) pp = &(*pp)->next;
    *pp = &e;

    uv_mutex_unlock(ch->lock);
    sem_wait(&e.sync_sem);
    sem_destroy(&e.sync_sem);

    return e.ok;
}

/* --------------------------------------------------------------------------
 * goc_take_try  (non-blocking, any context)
 * -------------------------------------------------------------------------- */
goc_val_t goc_take_try(goc_chan* ch)
{
    uv_mutex_lock(ch->lock);

    void* val = NULL;

    if (chan_take_from_buffer(ch, &val)) {
        uv_mutex_unlock(ch->lock);
        return (goc_val_t){ val, GOC_OK };
    }

    if (chan_take_from_putter(ch, &val)) {
        uv_mutex_unlock(ch->lock);
        return (goc_val_t){ val, GOC_OK };
    }

    if (ch->closed) {
        uv_mutex_unlock(ch->lock);
        return (goc_val_t){ NULL, GOC_CLOSED };
    }

    /* Open but empty */
    uv_mutex_unlock(ch->lock);
    return (goc_val_t){ NULL, GOC_EMPTY };
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
    goc_entry** pp = &ch->takers;
    while (*pp) pp = &(*pp)->next;
    *pp = e;

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
    goc_entry** pp = &ch->putters;
    while (*pp) pp = &(*pp)->next;
    *pp = e;

    uv_mutex_unlock(ch->lock);
}
