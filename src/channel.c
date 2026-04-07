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

#define POST_CALLBACK(entry, value) do {                                  \
    const char *post_cb_ch_tag = (entry)->ch ? goc_chan_get_debug_tag((entry)->ch) : "<none>"; \
    GOC_DBG("post_callback caller=%s:%d entry=%p id=%llu ch=%p tag=%s cb=%p put_cb=%p\n", \
            __FILE__, __LINE__, (void*)(entry), (unsigned long long)(entry)->id, \
            (void*)(entry)->ch, post_cb_ch_tag, \
            (void*)(uintptr_t)(entry)->cb, (void*)(uintptr_t)(entry)->put_cb); \
    post_callback((entry), (value));                                          \
} while (0)

/* --------------------------------------------------------------------------
 * goc_close
 * -------------------------------------------------------------------------- */
#define GOC_CLOSE_TRACE() \
    GOC_DBG("goc_close caller: %s:%d (%s)\n", __FILE__, __LINE__, __func__)

static const char* goc_close_phase = "normal";

void goc_chan_set_debug_tag(goc_chan* ch, const char* tag) {
    ch->dbg_tag = tag;
}

const char* goc_chan_get_debug_tag(goc_chan* ch) {
    return ch->dbg_tag ? ch->dbg_tag : "(none)";
}

static bool wait_for_fiber_suspended(goc_entry* fe, uint64_t want, const char* context, goc_chan* ch)
{
    long _spin = 0;
    while (true) {
        uint64_t parked = atomic_load_explicit(&fe->parked, memory_order_acquire);
        if (parked == want) {
            return true;
        }
        if (parked == 0 || parked > want) {
            GOC_DBG("%s: stale fiber park ch=%p fe=%p want=%llu parked=%llu\n",
                    context, (void*)ch, (void*)fe,
                    (unsigned long long)want,
                    (unsigned long long)parked);
            return false;
        }
        sched_yield();
        if (++_spin == 1000000L) {
            GOC_DBG("%s: spin reached 1M iters ch=%p fe=%p want=%llu parked=%llu\n",
                    context, (void*)ch, (void*)fe,
                    (unsigned long long)want,
                    (unsigned long long)atomic_load_explicit(&fe->parked, memory_order_acquire));
        }
    }
}

void goc_debug_set_close_phase(const char* phase) {
    goc_close_phase = phase ? phase : "normal";
}

const char* goc_debug_get_close_phase(void) {
    return goc_close_phase;
}

goc_chan_close_state_t goc_chan_close_state(goc_chan* ch)
{
    return atomic_load_explicit(&ch->close_guard, memory_order_acquire);
}

void goc_close_internal(goc_chan* ch)
{
    void (*on_close)(void*) = NULL;
    void* on_close_ud = NULL;

    GOC_CLOSE_TRACE();
    GOC_DBG("goc_close_internal: called on ch=%p tag=%s phase=%s\n",
            (void*)ch, goc_chan_get_debug_tag(ch), goc_debug_get_close_phase());

    /* Serialise: only one caller transitions close from requested to in-progress. */
    int expected = 1;
    if (!atomic_compare_exchange_strong_explicit(
            &ch->close_guard, &expected, 2,
            memory_order_acq_rel, memory_order_acquire)) {
        goc_chan_close_state_t closing = goc_chan_close_state(ch);
        const char* phase = goc_debug_get_close_phase();
        GOC_DBG("goc_close_internal: ch=%p tag=%s phase=%s CAS failed actual=%d closing=%d\n",
                (void*)ch, goc_chan_get_debug_tag(ch), phase,
                expected,
                (int)closing);
        if (expected == GOC_CHAN_OPEN) {
            /* Direct loop-thread close path without a prior request. */
            if (atomic_compare_exchange_strong_explicit(
                    &ch->close_guard, &expected, GOC_CHAN_CLOSING,
                    memory_order_acq_rel, memory_order_acquire)) {
                GOC_DBG("goc_close_internal: direct loop-thread close claimed ch=%p\n",
                        (void*)ch);
            } else {
                GOC_DBG("goc_close_internal: duplicate/late close caller detected, no-op ch=%p tag=%s phase=%s actual=%d closing=%d\n",
                        (void*)ch, goc_chan_get_debug_tag(ch), phase,
                        expected, closing);
                return;
            }
        } else {
            GOC_DBG("goc_close_internal: duplicate close caller detected, no-op ch=%p tag=%s phase=%s actual=%d closing=%d\n",
                    (void*)ch, goc_chan_get_debug_tag(ch), phase,
                    expected, closing);
            return;
        }
    }
    GOC_DBG("goc_close_internal: FIRST CLOSE ch=%p tag=%s phase=%s\n",
            (void*)ch, goc_chan_get_debug_tag(ch), goc_debug_get_close_phase());

    uv_mutex_lock(ch->lock);
    ch->closed = 1;
    GOC_DBG("goc_close_internal: ch=%p locked, takers=%p putters=%p item_count=%zu\n",
            (void*)ch, (void*)ch->takers, (void*)ch->putters, ch->item_count);

    /* Count non-cancelled waiters: used for the to_post heap allocation and
     * (when GOC_ENABLE_STATS) for the close telemetry counter.  Entries may
     * be stack-allocated (GOC_SYNC) and freed the moment we post them, so we
     * must not read them after the wake loop. */
    size_t waiter_count = 0;
    size_t taker_count = 0;
    size_t putter_count = 0;
    for (goc_entry* e = ch->takers; e != NULL; e = e->next) {
        if (!atomic_load_explicit(&e->cancelled, memory_order_acquire))
            waiter_count++;
        taker_count++;
    }
    for (goc_entry* e = ch->putters; e != NULL; e = e->next) {
        if (!atomic_load_explicit(&e->cancelled, memory_order_acquire))
            waiter_count++;
        putter_count++;
    }

    const char* tag = goc_chan_get_debug_tag(ch);
    GOC_DBG("goc_close_internal: CLOSE_DEBUG: ch=%p tag=%s closed=%d takers=%p takers_tail=%p putters=%p putters_tail=%p item_count=%zu taker_entries=%zu putter_entries=%zu noncancelled_waiters=%zu\n",
            (void*)ch, tag, ch->closed, (void*)ch->takers, (void*)ch->takers_tail,
            (void*)ch->putters, (void*)ch->putters_tail, ch->item_count,
            taker_count, putter_count, waiter_count);
    if (strcmp(tag, "park") == 0) {
        GOC_DBG("goc_close_internal: CLOSE_PARK: ch=%p taker_entries=%zu putter_entries=%zu noncancelled_waiters=%zu\n",
                (void*)ch, taker_count, putter_count, waiter_count);
    } else if (strcmp(tag, "join") == 0 && waiter_count > 0) {
        GOC_DBG("goc_close_internal: CLOSE_JOIN: ch=%p taker_entries=%zu putter_entries=%zu noncancelled_waiters=%zu\n",
                (void*)ch, taker_count, putter_count, waiter_count);
    }
    GOC_DBG("goc_close_internal: ch=%p taker_entries=%zu putter_entries=%zu noncancelled_waiters=%zu\n",
            (void*)ch, taker_count, putter_count, waiter_count);
#ifdef GOC_ENABLE_STATS
    uint64_t close_removed = (uint64_t)waiter_count;
#endif

    /* Collect GOC_FIBER entries to post after lock release.
     * Use plain malloc (not goc_malloc/GC_malloc) to avoid triggering a GC
     * stop-the-world while ch->lock is held, which can deadlock if another
     * thread is blocked waiting on the same lock. */
    goc_entry** to_post = waiter_count ? malloc(sizeof(goc_entry*) * waiter_count) : NULL;
    uint64_t*  to_post_want = waiter_count ? malloc(sizeof(uint64_t) * waiter_count) : NULL;
    size_t to_post_count = 0;
    int cancelled_found = 0;
    for (int _pass = 0; _pass < 2; _pass++) {
        goc_entry* e = (_pass == 0) ? ch->takers : ch->putters;
        while (e != NULL) {
            goc_entry* next = e->next;
            if (atomic_load_explicit(&e->cancelled, memory_order_acquire)) {
                cancelled_found = 1;
            } else if (try_claim_wake(e)) {
                GOC_DBG("goc_close_internal: claimed waiter ch=%p e=%p kind=%d arm_pass=%d coro=%p\n",
                        (void*)ch, (void*)e, (int)e->kind, _pass,
                        (e->kind == GOC_FIBER ? (void*)e->coro : NULL));
                e->ok = GOC_CLOSED;
                if (e->kind == GOC_FIBER) {
                    to_post[to_post_count] = (goc_entry*)mco_get_user_data(e->coro);
                    to_post_want[to_post_count] = (e->parked_gen << 1) + 2;
                    to_post_count++;
                } else if (e->kind == GOC_CALLBACK) {
                    /* This callback entry is parked on the channel wait list and
                     * needs to be reposted to the loop queue so it fires. For
                     * goc_alts callback arms we use the shared fired flag to
                     * avoid posting an already-fired arm again. Non-alts callback
                     * entries have no fired flag and can be reposted safely once
                     * try_claim_wake() succeeds. */
                    if (e->fired == NULL) {
                        GOC_DBG("goc_close_internal: callback entry e=%p id=%llu ch=%p tag=%s no fired flag, reposting cb=%p put_cb=%p woken=%d cancelled=%d\n",
                                (void*)e,
                                (unsigned long long)e->id,
                                (void*)ch,
                                goc_chan_get_debug_tag(ch),
                                (void*)(uintptr_t)e->cb,
                                (void*)(uintptr_t)e->put_cb,
                                (int)atomic_load_explicit(&e->woken, memory_order_relaxed),
                                (int)atomic_load_explicit(&e->cancelled, memory_order_relaxed));
                        POST_CALLBACK(e, NULL);
                    } else {
                        int old_fired = atomic_exchange_explicit(e->fired, 1, memory_order_acq_rel);
                        int fired_val = atomic_load_explicit(e->fired, memory_order_acquire);
                        if (old_fired == 0) {
                            GOC_DBG("goc_close_internal: callback entry e=%p id=%llu ch=%p tag=%s fired flag clear, reposting cb=%p put_cb=%p woken=%d cancelled=%d\n",
                                    (void*)e,
                                    (unsigned long long)e->id,
                                    (void*)ch,
                                    goc_chan_get_debug_tag(ch),
                                    (void*)(uintptr_t)e->cb,
                                    (void*)(uintptr_t)e->put_cb,
                                    (int)atomic_load_explicit(&e->woken, memory_order_relaxed),
                                    (int)atomic_load_explicit(&e->cancelled, memory_order_relaxed));
                            POST_CALLBACK(e, NULL);
                        } else {
                            GOC_DBG("goc_close_internal: callback entry e=%p id=%llu ch=%p tag=%s already fired, skip repost cb=%p put_cb=%p woken=%d cancelled=%d fired_val=%d\n",
                                    (void*)e,
                                    (unsigned long long)e->id,
                                    (void*)ch,
                                    goc_chan_get_debug_tag(ch),
                                    (void*)(uintptr_t)e->cb,
                                    (void*)(uintptr_t)e->put_cb,
                                    (int)atomic_load_explicit(&e->woken, memory_order_relaxed),
                                    (int)atomic_load_explicit(&e->cancelled, memory_order_relaxed),
                                    fired_val);
                        }
                    }
                    GOC_DBG("goc_close_internal: post_callback done e=%p id=%llu fired=%p fired_val=%d\n",
                            (void*)e,
                            (unsigned long long)e->id,
                            (void*)e->fired,
                            e->fired ? atomic_load_explicit(e->fired, memory_order_acquire) : 1);
                } else if (e->kind == GOC_SYNC) {
                    goc_sync_post(e->sync_sem_ptr);
                }
            } else {
                GOC_DBG("goc_close_internal: try_claim_wake failed ch=%p e=%p kind=%d arm_pass=%d cancelled=%d woken=%d fired=%p\n",
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

    /* Clearing the waiter lists prevents future goc_take/goc_take_sync calls on
     * a closed channel from walking stale waiter entries that have already
     * been claimed and may be freed by callback delivery. */
    ch->takers = NULL;
    ch->takers_tail = NULL;
    ch->putters = NULL;
    ch->putters_tail = NULL;
    GOC_DBG("goc_close_internal: cleared waiter lists ch=%p takers=%p putters=%p\n",
            (void*)ch, (void*)ch->takers, (void*)ch->putters);
    ch->dead_count = 0;

    /* Release lock before posting */
    uv_mutex_unlock(ch->lock);

    /* Spin and post for all collected GOC_FIBER entries */
    GOC_DBG("goc_close_internal: posting %zu fiber waiters for ch=%p\n",
            to_post_count, (void*)ch);
    for (size_t i = 0; i < to_post_count; i++) {
        goc_entry* fe = to_post[i];
        uint64_t want = to_post_want[i];
        if (!wait_for_fiber_suspended(fe, want, "goc_close_internal: wait for waiter", ch)) {
            GOC_DBG("goc_close_internal: stale waiter, skipping post ch=%p fe=%p coro=%p pool=%p\n",
                    (void*)ch, (void*)fe,
                    (void*)fe->coro, (void*)fe->pool);
            continue;
        }
        GOC_DBG("goc_close_internal: CLOSE: posting entry %p from ch=%p tag=%s pool=%p home_worker=%zu\n",
                (void*)fe, (void*)ch, goc_chan_get_debug_tag(ch), (void*)fe->pool, fe->home_worker_idx);
        if (fe->home_worker_idx != SIZE_MAX &&
            fe->pool && fe->home_worker_idx < goc_pool_thread_count(fe->pool)) {
            post_to_specific_worker(fe->pool, fe->home_worker_idx, fe);
        } else {
            post_to_run_queue(fe->pool, fe);
        }
        GOC_DBG("goc_close_internal: posted wake for ch=%p fe=%p coro=%p pool=%p\n",
                (void*)ch, (void*)fe,
                (void*)fe->coro, (void*)fe->pool);
    }
    free(to_post);
    free(to_post_want);

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

    if (on_close != NULL) {
        GOC_DBG("calling on_close for ch=%p\n", ch);
        on_close(on_close_ud);
        GOC_DBG("returned from on_close for ch=%p\n", ch);
    }
}

static void on_close_dispatch(void *arg)
{
    goc_chan *ch = (goc_chan *)arg;
    if (!ch) {
        GOC_DBG("on_close_dispatch: skipping NULL ch\n");
        return;
    }

    goc_chan_close_state_t close_guard = goc_chan_close_state(ch);
    if (close_guard == GOC_CHAN_CLOSING)
    {
        GOC_DBG("on_close_dispatch: skipping ch=%p already closing/closed close_guard=%d\n",
                (void *)ch,
                (int)close_guard);
        return;
    }

    GOC_DBG("on_close_dispatch: closing ch directly ch=%p close_guard=%d\n",
            (void *)ch, close_guard);
    goc_close_internal(ch);
}

void goc_close(goc_chan *ch)
{
    GOC_DBG("goc_close: entry ch=%p\n", (void *)ch);
    if (!ch) {
        GOC_DBG("goc_close: skipping NULL ch\n");
        return;
    }

    int expected = GOC_CHAN_OPEN;
    if (!atomic_compare_exchange_strong_explicit(
            &ch->close_guard, &expected, GOC_CHAN_CLOSE_REQUESTED,
            memory_order_acq_rel, memory_order_acquire)) {
        GOC_DBG("goc_close: close already requested ch=%p close_guard=%d\n",
                (void*)ch,
                expected);
        return;
    }

    if (goc_on_loop_thread())
    {
        GOC_DBG("goc_close: on g_loop thread, closing ch directly ch=%p\n",
                (void *)ch);
        goc_close_internal(ch);
        return;
    }

    int rc = post_on_loop_checked(on_close_dispatch, ch);
    if (rc < 0)
        ABORT("goc_close: failed to dispatch on loop\n");
}

void goc_close_cb(goc_chan* ch, void* _, goc_status_t __, void* ___)
{
    GOC_DBG("goc_close_cb: closing ch=%p\n", (void*)ch);
    goc_close(ch);
    GOC_DBG("goc_close_cb: done ch=%p\n", (void*)ch);
}

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
    GOC_DBG("wake: entry e=%p ch=%p kind=%d cancelled=%d woken=%d\n",
            (void*)e, (void*)ch, (int)e->kind,
            (int)atomic_load_explicit(&e->cancelled, memory_order_acquire),
            (int)atomic_load_explicit(&e->woken, memory_order_acquire));
    if (atomic_load_explicit(&e->cancelled, memory_order_acquire)) {
        ch->dead_count++;
        return false;
    }

    if (!try_claim_wake(e)) {
        ch->dead_count++;
        GOC_DBG("wake: try_claim_wake failed e=%p ch=%p\n", (void*)e, (void*)ch);
        return false;
    }

    if (e->result_slot != NULL)
        *e->result_slot = value;

    e->ok = GOC_OK;
    GOC_DBG("wake: claimed e=%p ch=%p kind=%d\n", (void*)e, (void*)ch, (int)e->kind);

    switch (e->kind) {
    case GOC_FIBER: {
        /* Spin until the fiber has truly called mco_yield for this park
         * generation.  Abort if the fiber has moved on to a newer park. */
        goc_entry* fe = (goc_entry*)mco_get_user_data(e->coro);
        uint64_t want = (e->parked_gen << 1) + 2;
        {
            long _spin = 0;
            while (true) {
                uint64_t parked = atomic_load_explicit(&fe->parked, memory_order_acquire);
                if (parked == want)
                    break;
                if (parked == 0 || parked > want)
                    return false;
                sched_yield();
                if (++_spin == 10000000L) {
                    GOC_DBG("wake(): SPIN STALL >10M iters coro=%p fe=%p ch=%p\n",
                            (void*)e->coro, (void*)fe, (void*)ch);
                }
            }
            if (_spin > 100) { GOC_DBG("wake(): spin resolved after %ld iters coro=%p\n", _spin, (void*)e->coro); }
        }
        GOC_DBG("wake: posting runnable coro=%p fe=%p ch=%p pool=%p\n",
                (void*)e->coro, (void*)fe, (void*)ch, (void*)fe->pool);
        if (fe->home_worker_idx != SIZE_MAX &&
            fe->pool && fe->home_worker_idx < goc_pool_thread_count(fe->pool)) {
            post_to_specific_worker(fe->pool, fe->home_worker_idx, fe);
        } else {
            post_to_run_queue(fe->pool, fe);
        }
        break;
    }
    case GOC_CALLBACK:
        POST_CALLBACK(e, value);
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
    GOC_DBG("wake_claim: entry e=%p ch=%p kind=%d cancelled=%d woken=%d\n",
            (void*)e, (void*)ch, (int)e->kind,
            (int)atomic_load_explicit(&e->cancelled, memory_order_acquire),
            (int)atomic_load_explicit(&e->woken, memory_order_acquire));

    if (atomic_load_explicit(&e->cancelled, memory_order_acquire)) {
        ch->dead_count++;
        return false;
    }

    if (e->kind == GOC_FIBER) {
        goc_entry* fe = (goc_entry*)mco_get_user_data(e->coro);
        GOC_DBG("wake_claim: before want e=%p parked_gen=%llu parked_gen_addr=%p\n",
                (void*)e,
                (unsigned long long)e->parked_gen,
                (void*)&e->parked_gen);
        uint64_t want = (e->parked_gen << 1) + 2;
        GOC_DBG("wake_claim: computed want=%llu\n",
                (unsigned long long)want);
        uint64_t parked = atomic_load_explicit(&fe->parked, memory_order_acquire);
        if (parked == 0 || parked > want) {
            ch->dead_count++;
            GOC_DBG("wake_claim: stale fiber park e=%p ch=%p parked_gen_addr=%p parked=%llu want=%llu\n",
                    (void*)e, (void*)ch,
                    (void*)&e->parked_gen,
                    (unsigned long long)parked,
                    (unsigned long long)want);
            return false;
        }
    }

    if (!try_claim_wake(e)) {
        ch->dead_count++;
        GOC_DBG("wake_claim: try_claim_wake failed e=%p ch=%p\n", (void*)e, (void*)ch);
        return false;
    }

    if (e->result_slot != NULL)
        *e->result_slot = value;

    e->ok = GOC_OK;

    switch (e->kind) {
    case GOC_FIBER:
        *fe_out = (goc_entry*)mco_get_user_data(e->coro);
        GOC_DBG("wake_claim: claimed e=%p ch=%p kind=FIBER fe_out=%p\n",
                (void*)e, (void*)ch, (void*)*fe_out);
        return true;
    case GOC_CALLBACK:
        POST_CALLBACK(e, value);
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
    ch->dbg_tag = NULL;

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

/* --------------------------------------------------------------------------
 * goc_take  (fiber context only)
 * -------------------------------------------------------------------------- */
goc_val_t* goc_take(goc_chan* ch)
{
    if (mco_running() == NULL) {
        ABORT("goc_take: called from OS thread (not in fiber context)\n");
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
    uint64_t want = 0;
    if (chan_take_from_putter_claim(ch, &val, &fe_putter, &want)) {
        uv_mutex_unlock(ch->lock);
        if (fe_putter != NULL) {
            if (wait_for_fiber_suspended(fe_putter, want, "goc_take: wait for putter", ch)) {
                post_to_run_queue(fe_putter->pool, fe_putter);
            } else {
                GOC_DBG("goc_take: stale putter skip post ch=%p fe=%p\n",
                        (void*)ch, (void*)fe_putter);
            }
        }
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

    GOC_DBG("goc_take: SLOW: ch=%p buf_size=%zu closed=%d takers=%p putters=%p\n",
            (void*)ch, ch->buf_size, ch->closed, (void*)ch->takers, (void*)ch->putters);

    goc_entry* e = malloc(sizeof(goc_entry));
    *e = (goc_entry){ 0 };
    e->kind             = GOC_FIBER;
    e->coro             = mco_running();
    e->pool             = ((goc_entry*)mco_get_user_data(mco_running()))->pool;
    int current_worker = goc_current_worker_id();
    e->home_worker_idx = current_worker >= 0 ? (size_t)current_worker : SIZE_MAX;
    e->result_slot      = &local_result;
    e->ok               = GOC_CLOSED;   /* default; overwritten by wake on success */
    goc_stack_canary_init(e);

    GOC_DBG("goc_take: slow-path park ch=%p buf_size=%zu closed=%d takers=%p putters=%p coro=%p\n",
            (void*)ch,
            ch->buf_size,
            ch->closed,
            (void*)ch->takers,
            (void*)ch->putters,
            (void*)e->coro);

    /* Begin generation-based park handshake for this slow-path entry. */
    goc_entry* fiber_entry = (goc_entry*)mco_get_user_data(mco_running());
    uint64_t gen = (atomic_load_explicit(&fiber_entry->parked, memory_order_relaxed) >> 1) + 1;
    e->parked_gen = gen;
    GOC_DBG("goc_take: park handshake e=%p ch=%p gen=%llu fiber_parked=%llu\n",
            (void*)e, (void*)ch,
            (unsigned long long)gen,
            (unsigned long long)atomic_load_explicit(&fiber_entry->parked, memory_order_relaxed));
    atomic_store_explicit(&fiber_entry->parked, (gen << 1) | 1, memory_order_release);

    /* Append to takers list. */
    chan_list_append(&ch->takers, &ch->takers_tail, e);
    GOC_DBG("goc_take: QUEUED: e=%p ch=%p takers=%p takers_tail=%p putters=%p\n",
            (void*)e, (void*)ch, (void*)ch->takers, (void*)ch->takers_tail,
            (void*)ch->putters);
    GOC_DBG("goc_take: queued taker e=%p ch=%p buf_size=%zu closed=%d takers=%p takers_tail=%p putters=%p coro=%p\n",
            (void*)e, (void*)ch, ch->buf_size, ch->closed,
            (void*)ch->takers, (void*)ch->takers_tail, (void*)ch->putters, (void*)e->coro);

    uv_mutex_unlock(ch->lock);
    GOC_DBG("goc_take: YIELD: ch=%p e=%p coro=%p home_worker=%zu parked_gen=%llu before_yield\n",
            (void*)ch, (void*)e, (void*)e->coro,
            e->home_worker_idx,
            (unsigned long long)e->parked_gen);
    mco_yield(mco_running());
    GOC_DBG("goc_take: RESUMED: ch=%p e=%p coro=%p home_worker=%zu ok=%d parked=%llu after_yield\n",
            (void*)ch, (void*)e, (void*)e->coro,
            e->home_worker_idx,
            (int)e->ok,
            (unsigned long long)atomic_load_explicit(&fiber_entry->parked, memory_order_acquire));
    /* pool_worker_fn has set fiber_entry->parked to the suspended value for this generation. */
    atomic_store_explicit(&fiber_entry->parked, 0, memory_order_release);

    GOC_DBG("goc_take: resumed coro=%p ch=%p ok=%d val=%p parked=%llu\n",
            (void*)mco_running(), (void*)ch, (int)e->ok, local_result,
            (unsigned long long)atomic_load_explicit(&fiber_entry->parked, memory_order_acquire));
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
        ABORT("goc_put: called from OS thread (not in fiber context)\n");
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
        GOC_DBG("goc_put: fast-path CLOSED ch=%p val=%p\n", (void*)ch, val);
        uv_mutex_unlock(ch->lock);
        return GOC_CLOSED;
    }

    /* Fast path: parked taker — claim under lock, spin+post outside */
    goc_entry* fe_taker = NULL;
    uint64_t want = 0;
    if (chan_put_to_taker_claim(ch, val, &fe_taker, &want)) {
        GOC_DBG("goc_put: fast-path delivering val=%p to parked taker fe_taker=%p ch=%p\n",
                val, (void*)fe_taker, (void*)ch);
        uv_mutex_unlock(ch->lock);
        if (fe_taker != NULL) {
            if (wait_for_fiber_suspended(fe_taker, want, "goc_put: wait for taker", ch)) {
                GOC_DBG("goc_put: posting wake for taker fe_taker=%p ch=%p pool=%p\n",
                        (void*)fe_taker, (void*)ch, (void*)fe_taker->pool);
                if (fe_taker->home_worker_idx != SIZE_MAX &&
                    fe_taker->pool && fe_taker->home_worker_idx < goc_pool_thread_count(fe_taker->pool)) {
                    post_to_specific_worker(fe_taker->pool,
                                            fe_taker->home_worker_idx,
                                            fe_taker);
                } else {
                    post_to_run_queue(fe_taker->pool, fe_taker);
                }
            } else {
                GOC_DBG("goc_put: stale taker skip post ch=%p fe_taker=%p\n",
                        (void*)ch, (void*)fe_taker);
            }
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
    int current_worker = goc_current_worker_id();
    e->home_worker_idx = current_worker >= 0 ? (size_t)current_worker : SIZE_MAX;
    e->put_val          = val;
    e->ok               = GOC_CLOSED;   /* default; overwritten by wake on success */
    goc_stack_canary_init(e);

    /* Begin generation-based park handshake for this slow-path entry. */
    goc_entry* fiber_entry = (goc_entry*)mco_get_user_data(mco_running());
    uint64_t gen = (atomic_load_explicit(&fiber_entry->parked, memory_order_relaxed) >> 1) + 1;
    e->parked_gen = gen;
    GOC_DBG("goc_put: park handshake e=%p ch=%p gen=%llu fiber_parked=%llu\n",
            (void*)e, (void*)ch,
            (unsigned long long)gen,
            (unsigned long long)atomic_load_explicit(&fiber_entry->parked, memory_order_relaxed));
    atomic_store_explicit(&fiber_entry->parked, (gen << 1) | 1, memory_order_release);

    /* Append to putters list. */
    chan_list_append(&ch->putters, &ch->putters_tail, e);
    GOC_DBG("goc_put: slow-path park ch=%p putters=%p takers=%p coro=%p\n",
            (void*)ch,
            (void*)ch->putters,
            (void*)ch->takers,
            (void*)e->coro);

    uv_mutex_unlock(ch->lock);
    mco_yield(mco_running());
    /* pool_worker_fn has set fiber_entry->parked to the suspended value for this generation. */
    atomic_store_explicit(&fiber_entry->parked, 0, memory_order_release);

    goc_status_t ok = e->ok;
    free(e);
    return ok;
}

void goc_yield(void)
{
    if (mco_running() == NULL) {
        ABORT("goc_yield: called from OS thread (not in fiber context)\n");
    }

    goc_entry* fe = (goc_entry*)mco_get_user_data(mco_running());
    if (fe != NULL && fe->pool != NULL) {
        int current_worker = goc_current_worker_id();
        size_t target_worker = 0;
        size_t worker_count = goc_pool_thread_count(fe->pool);
        if (current_worker >= 0 && worker_count > 1) {
            target_worker = (size_t)(current_worker + 1) % worker_count;
        } else if (fe->home_worker_idx != SIZE_MAX) {
            target_worker = fe->home_worker_idx;
        }
        post_to_specific_worker(fe->pool, target_worker, fe);
    }

    GOC_DBG("goc_yield: coro=%p before\n", (void*)mco_running());
    mco_yield(mco_running());
    GOC_DBG("goc_yield: coro=%p after\n", (void*)mco_running());
}

/* --------------------------------------------------------------------------
 * goc_take_sync  (OS thread context)
 * -------------------------------------------------------------------------- */
goc_val_t* goc_take_sync(goc_chan* ch)
{
    if (mco_running() != NULL) {
        ABORT("goc_take_sync: called from fiber context\n");
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
        ABORT("goc_put_sync: called from fiber context\n");
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
    const size_t alloc_count = (n == 0) ? 1 : n;
    goc_val_t** results = goc_malloc(alloc_count * sizeof(goc_val_t*));
    if (n == 0)
        return results;

    for (size_t i = 0; i < n; i++) {
        results[i] = goc_take(chs[i]);
    }

    return results;
}

/* --------------------------------------------------------------------------
 * goc_take_all_sync  (OS thread context)
 * -------------------------------------------------------------------------- */
goc_val_t** goc_take_all_sync(goc_chan** chs, size_t n)
{
    const size_t alloc_count = (n == 0) ? 1 : n;
    goc_val_t** results = goc_malloc(alloc_count * sizeof(goc_val_t*));
    if (n == 0)
        return results;

    for (size_t i = 0; i < n; i++) {
        results[i] = goc_take_sync(chs[i]);
    }

    return results;
}

/* --------------------------------------------------------------------------
 * goc_take_cb
 *
 * Non-blocking: posts the pending take to the loop thread, which will
 * perform the channel operation under ch->lock and fire cb on delivery.
 * -------------------------------------------------------------------------- */
void goc_take_cb(goc_chan* ch,
                 void (*cb)(goc_chan* ch, void* val, goc_status_t ok, void* ud),
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

    if (!goc_loop_submit_callback_if_running(e)) {
        GOC_DBG("goc_take_cb: shutdown in progress, rejecting ch=%p cb=%p ud=%p\n",
                (void*)ch, (void*)(uintptr_t)cb, ud);
        free(e);
        return;
    }
}

/* --------------------------------------------------------------------------
 * goc_put_cb
 *
 * Non-blocking: posts the pending put to the loop thread, which will
 * perform the channel operation under ch->lock and fire cb on delivery.
 * -------------------------------------------------------------------------- */
void goc_put_cb(goc_chan* ch, void* val,
                void (*cb)(goc_chan* ch, void* val, goc_status_t ok, void* ud),
                void* ud)
{
    GOC_DBG("goc_put_cb: ch=%p val=%p put_cb=%p ud=%p\n",
            (void*)ch, val, (void*)(uintptr_t)cb, ud);
    goc_entry* e = malloc(sizeof(goc_entry));
    *e = (goc_entry){ 0 };
    e->kind          = GOC_CALLBACK;
    e->ch            = ch;
    e->is_put        = true;
    e->put_val       = val;
    e->put_cb        = cb;
    e->ud            = ud;
    e->free_on_drain = true;

    if (!goc_loop_submit_callback_if_running(e)) {
        GOC_DBG("goc_put_cb: shutdown in progress, rejecting ch=%p val=%p put_cb=%p ud=%p\n",
                (void*)ch, val, (void*)(uintptr_t)cb, ud);
        free(e);
        return;
    }
}
