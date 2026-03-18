/*
 * src/alts.c
 *
 * Implements goc_alts (fiber context) and goc_alts_sync (OS thread context).
 *
 * Both functions provide a Go-style select over an array of goc_alt_op arms.
 * Execution follows a seven-phase protocol:
 *
 *   Phase 1 — Shuffle arm indices to prevent starvation.
 *   Phase 2 — Non-blocking scan: attempt each arm without parking.
 *   Phase 3 — Prepare park: allocate entries and the shared fired flag.
 *   Phase 4 — Acquire channel locks in ascending pointer order.
 *   Phase 5 — Re-scan under locks; return immediately if any arm fires.
 *   Phase 6 — Enqueue all entries and yield / sem_wait.
 *   Phase 7 — On resume: cancel losers, extract winner, return result.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <assert.h>
#include <semaphore.h>
#include <uv.h>
#include <gc.h>
#include "minicoro.h"
#include "../include/goc.h"
#include "chan_type.h"
#include "internal.h"

/* -------------------------------------------------------------------------
 * alts_rand_seed
 *
 * Per-call seed for rand_r(). Using a global atomic counter avoids the
 * thread-safety problem with rand() (which shares global state) while
 * keeping the implementation simple. The shuffle is for starvation
 * avoidance, not security, so the quality of the PRNG is unimportant.
 * ------------------------------------------------------------------------- */
static _Atomic unsigned int g_alts_rand_seed = 1;

/* -------------------------------------------------------------------------
 * alts_shuffle
 *
 * Fisher-Yates in-place shuffle on indices[0..n-1].
 * Used to randomise arm evaluation order in Phase 1.
 * ------------------------------------------------------------------------- */
static void alts_shuffle(size_t *indices, size_t n) {
    if (n < 2) return;
    /* rand_r() with a per-call seed snapshot is thread-safe. The seed is an
     * atomic counter rather than per-fiber state; collisions between threads
     * merely reduce shuffle variety, which is acceptable for fairness. */
    unsigned int seed = atomic_fetch_add_explicit(&g_alts_rand_seed, 1,
                                                  memory_order_relaxed);
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = (size_t)rand_r(&seed) % (i + 1);
        size_t tmp = indices[i];
        indices[i] = indices[j];
        indices[j] = tmp;
    }
}

/* -------------------------------------------------------------------------
 * alts_dedup_sort_channels
 *
 * Builds a deduplicated, ascending-pointer-sorted array of channel pointers
 * from ops[0..n-1] into scratch[0..scratch_n-1].
 *
 * Returns the number of unique channels written into scratch.
 *
 * scratch must hold at least n elements (the worst case with no duplicates).
 * The caller is responsible for allocating and freeing scratch:
 *   - stack-allocate when n <= GOC_ALTS_STACK_THRESHOLD
 *   - malloc/free otherwise
 *
 * GOC_ALT_DEFAULT arms (ch == NULL) are skipped.
 * ------------------------------------------------------------------------- */
static size_t alts_dedup_sort_channels(goc_alt_op *ops, size_t n,
                                        goc_chan **scratch, size_t scratch_n) {
    size_t count = 0;

    /* collect non-NULL channel pointers */
    for (size_t i = 0; i < n; i++) {
        if (ops[i].op_kind == GOC_ALT_DEFAULT || ops[i].ch == NULL)
            continue;
        assert(count < scratch_n && "alts_dedup_sort_channels: scratch overflow");
        scratch[count++] = ops[i].ch;
    }

    /* insertion sort by pointer value — n is small in practice */
    for (size_t i = 1; i < count; i++) {
        goc_chan *key = scratch[i];
        size_t j = i;
        while (j > 0 && scratch[j - 1] > key) {
            scratch[j] = scratch[j - 1];
            j--;
        }
        scratch[j] = key;
    }

    /* deduplicate (sorted, so duplicates are adjacent) */
    if (count == 0) return 0;
    size_t unique = 1;
    for (size_t i = 1; i < count; i++) {
        if (scratch[i] != scratch[unique - 1])
            scratch[unique++] = scratch[i];
    }
    return unique;
}

/* -------------------------------------------------------------------------
 * goc_alts  (fiber context only)
 *
 * assert(mco_running() != NULL)
 * ------------------------------------------------------------------------- */
goc_alts_result goc_alts(goc_alt_op *ops, size_t n) {
    /* ------------------------------------------------------------------ */
    /* Fiber-context guard                                                  */
    /* ------------------------------------------------------------------ */
    mco_coro *running = mco_running();
    if (!running) {
        /* Calling goc_alts from a bare OS thread is a programming error. */
        fprintf(stderr, "goc_alts: cannot be called from OS thread (not in fiber context)\n");
        abort();
    }

    goc_entry *self = (goc_entry *)mco_get_user_data(running);
    goc_pool  *pool = self->pool;

    /* ------------------------------------------------------------------ */
    /* Phase 1 — Shuffle                                                    */
    /* Build a shuffled index array.  A GOC_ALT_DEFAULT arm is excluded    */
    /* from the shuffle; it is identified and placed at the end.           */
    /* Enforce that at most one default arm is provided.                   */
    /* ------------------------------------------------------------------ */
    size_t  default_idx  = (size_t)-1;
    size_t  n_nondefault = 0;
    size_t  n_default = 0;
    /* VLA: n is expected to be small (single digits in practice). A
     * pathologically large n could overflow the fiber's 64 KB stack; callers
     * are responsible for keeping arm counts reasonable. */
    size_t  indices[n];

    for (size_t i = 0; i < n; i++) {
        if (ops[i].op_kind == GOC_ALT_DEFAULT) {
            n_default++;
            if (n_default > 1) {
                fprintf(stderr, "goc_alts: more than one default arm provided (got %zu)\n", n_default);
                abort();
            }
            default_idx = i;
        } else {
            indices[n_nondefault++] = i;
        }
    }

    alts_shuffle(indices, n_nondefault);

    /* ------------------------------------------------------------------ */
    /* Phase 2 — Non-blocking scan (no lock held between iterations)       */
    /* ------------------------------------------------------------------ */
    for (size_t si = 0; si < n_nondefault; si++) {
        size_t    i  = indices[si];
        goc_chan *ch = ops[i].ch;

        if (ops[i].op_kind == GOC_ALT_TAKE) {
            uv_mutex_lock(ch->lock);
            void *val = NULL;
            if (chan_take_from_buffer(ch, &val)) {
                uv_mutex_unlock(ch->lock);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = val, .ok = GOC_OK } };
            }
            if (chan_take_from_putter(ch, &val)) {
                uv_mutex_unlock(ch->lock);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = val, .ok = GOC_OK } };
            }
            if (ch->closed && ch->buf_count == 0) {
                uv_mutex_unlock(ch->lock);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = NULL, .ok = GOC_CLOSED } };
            }
            uv_mutex_unlock(ch->lock);

        } else { /* GOC_ALT_PUT */
            uv_mutex_lock(ch->lock);
            if (ch->closed) {
                uv_mutex_unlock(ch->lock);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = NULL, .ok = GOC_CLOSED } };
            }
            if (chan_put_to_taker(ch, ops[i].put_val)) {
                uv_mutex_unlock(ch->lock);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = NULL, .ok = GOC_OK } };
            }
            if (chan_put_to_buffer(ch, ops[i].put_val)) {
                uv_mutex_unlock(ch->lock);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = NULL, .ok = GOC_OK } };
            }
            uv_mutex_unlock(ch->lock);
        }
    }

    /* Default arm fires if no non-default arm was immediately ready. */
    if (default_idx != (size_t)-1) {
        return (goc_alts_result){ .index = default_idx,
                                  .value = { .val = NULL, .ok = GOC_OK } };
    }

    /* ------------------------------------------------------------------ */
    /* Phase 3 — Prepare park                                              */
    /* ------------------------------------------------------------------ */
    /* Allocate one entry per non-default arm and a shared fired flag.     */
    goc_entry **entries   = goc_malloc(n_nondefault * sizeof(goc_entry *));
    _Atomic int *fired_flag = goc_malloc(sizeof(_Atomic int));
    atomic_store_explicit(fired_flag, 0, memory_order_relaxed);

    for (size_t si = 0; si < n_nondefault; si++) {
        size_t i = indices[si];
        goc_entry *e = goc_malloc(sizeof(goc_entry));

        e->kind            = GOC_FIBER;
        e->cancelled       = 0;
        e->woken           = 0;
        e->fired           = fired_flag;
        e->next            = NULL;
        e->pool            = pool;
        e->coro            = running;
        e->stack_canary_ptr = (uint32_t *)running->stack_base;
        e->ok              = GOC_CLOSED; /* safe default; overwritten on wake */
        e->arm_idx         = i;

        if (ops[i].op_kind == GOC_ALT_TAKE) {
            e->result_slot = &e->cb_result;
            e->put_val     = NULL;
        } else {
            e->result_slot = NULL;
            e->put_val     = ops[i].put_val;
        }

        e->fn      = NULL;
        e->fn_arg  = NULL;
        e->join_ch = NULL;
        e->cb      = NULL;
        e->put_cb  = NULL;
        e->ud      = NULL;
        e->cb_result = NULL;

        entries[si] = e;
    }

    /* Build the sorted unique channel list for lock acquisition order.    */
    goc_chan *stack_scratch[GOC_ALTS_STACK_THRESHOLD];
    goc_chan **sorted_chans;
    size_t    n_unique;

    if (n <= GOC_ALTS_STACK_THRESHOLD) {
        sorted_chans = stack_scratch;
    } else {
        sorted_chans = malloc(n * sizeof(goc_chan *));
    }

    n_unique = alts_dedup_sort_channels(ops, n, sorted_chans, n);

    /* ------------------------------------------------------------------ */
    /* Phase 4 — Acquire all channel locks in ascending pointer order      */
    /* ------------------------------------------------------------------ */
    for (size_t i = 0; i < n_unique; i++) {
        uv_mutex_lock(sorted_chans[i]->lock);
    }

    /* ------------------------------------------------------------------ */
    /* Phase 5 — Re-scan under locks                                       */
    /* ------------------------------------------------------------------ */
    for (size_t si = 0; si < n_nondefault; si++) {
        size_t    i  = indices[si];
        goc_chan *ch = ops[i].ch;

        if (ops[i].op_kind == GOC_ALT_TAKE) {
            void *val = NULL;
            if (chan_take_from_buffer(ch, &val)) {
                for (size_t j = n_unique; j-- > 0; )
                    uv_mutex_unlock(sorted_chans[j]->lock);
                if (n > GOC_ALTS_STACK_THRESHOLD) free(sorted_chans);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = val, .ok = GOC_OK } };
            }
            if (chan_take_from_putter(ch, &val)) {
                for (size_t j = n_unique; j-- > 0; )
                    uv_mutex_unlock(sorted_chans[j]->lock);
                if (n > GOC_ALTS_STACK_THRESHOLD) free(sorted_chans);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = val, .ok = GOC_OK } };
            }
            if (ch->closed && ch->buf_count == 0) {
                for (size_t j = n_unique; j-- > 0; )
                    uv_mutex_unlock(sorted_chans[j]->lock);
                if (n > GOC_ALTS_STACK_THRESHOLD) free(sorted_chans);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = NULL, .ok = GOC_CLOSED } };
            }

        } else { /* GOC_ALT_PUT */
            if (ch->closed) {
                for (size_t j = n_unique; j-- > 0; )
                    uv_mutex_unlock(sorted_chans[j]->lock);
                if (n > GOC_ALTS_STACK_THRESHOLD) free(sorted_chans);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = NULL, .ok = GOC_CLOSED } };
            }
            if (chan_put_to_taker(ch, ops[i].put_val)) {
                for (size_t j = n_unique; j-- > 0; )
                    uv_mutex_unlock(sorted_chans[j]->lock);
                if (n > GOC_ALTS_STACK_THRESHOLD) free(sorted_chans);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = NULL, .ok = GOC_OK } };
            }
            if (chan_put_to_buffer(ch, ops[i].put_val)) {
                for (size_t j = n_unique; j-- > 0; )
                    uv_mutex_unlock(sorted_chans[j]->lock);
                if (n > GOC_ALTS_STACK_THRESHOLD) free(sorted_chans);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = NULL, .ok = GOC_OK } };
            }
        }
    }

    /* ------------------------------------------------------------------ */
    /* Phase 6 — Enqueue entries and yield                                 */
    /* ------------------------------------------------------------------ */
    for (size_t si = 0; si < n_nondefault; si++) {
        size_t     i  = indices[si];
        goc_chan  *ch = ops[i].ch;
        goc_entry *e  = entries[si];

        if (ops[i].op_kind == GOC_ALT_TAKE) {
            /* Append to ch->takers */
            goc_entry **pp = &ch->takers;
            while (*pp) pp = &(*pp)->next;
            *pp = e;
        } else {
            /* Append to ch->putters */
            goc_entry **pp = &ch->putters;
            while (*pp) pp = &(*pp)->next;
            *pp = e;
        }
    }

    /* Register fiber stack as a GC root for the suspension window. */
    void *stack_base = running->stack_base;
    void *stack_top  = (char *)stack_base + running->stack_size;
    GC_add_roots(stack_base, stack_top);

    /* Release all locks in reverse order before yielding. */
    for (size_t j = n_unique; j-- > 0; )
        uv_mutex_unlock(sorted_chans[j]->lock);

    if (n > GOC_ALTS_STACK_THRESHOLD) free(sorted_chans);

    /* Suspend this fiber; the pool worker will resume us when a channel fires. */
    mco_yield(running);

    /* ------------------------------------------------------------------ */
    /* Phase 7 — On resume                                                 */
    /* ------------------------------------------------------------------ */
    GC_remove_roots(stack_base, stack_top);

    /* Cancel all losing entries (woken == 0). */
    goc_entry *winner = NULL;
    for (size_t si = 0; si < n_nondefault; si++) {
        goc_entry *e = entries[si];
        if (atomic_load_explicit(&e->woken, memory_order_acquire)) {
            winner = e;
        } else {
            atomic_store_explicit(&e->cancelled, 1, memory_order_release);
        }
    }

    /* The woken CAS in wake() / goc_close() guarantees exactly one entry wins.
     * A NULL winner here means the protocol is broken — abort rather than
     * silently producing undefined behaviour via a NULL dereference. */
    if (winner == NULL) {
        fprintf(stderr, "goc_alts: no winner after wake — woken CAS invariant violated\n");
        abort();
    }

    /* winner must not be NULL — the runtime guarantees exactly one wake */
    void *result_val = (winner->result_slot) ? *winner->result_slot : NULL;
    return (goc_alts_result){ .index = winner->arm_idx,
                              .value = { .val = result_val, .ok = winner->ok } };
}

/* -------------------------------------------------------------------------
 * goc_alts_sync  (OS thread context)
 *
 * Same seven-phase protocol as goc_alts, but:
 *   - Must not run in fiber context.
 *   - Entries use GOC_SYNC and share a single semaphore on the OS stack.
 *   - Yield is replaced by sem_wait; sem_destroy is called after.
 * ------------------------------------------------------------------------- */
goc_alts_result goc_alts_sync(goc_alt_op *ops, size_t n) {
    if (mco_running() != NULL) {
        fprintf(stderr, "goc_alts_sync: cannot be called from fiber context\n");
        abort();
    }

    /* ------------------------------------------------------------------ */
    /* Phase 1 — Shuffle                                                    */
    /* Enforce that at most one default arm is provided.                   */
    /* ------------------------------------------------------------------ */
    size_t  default_idx  = (size_t)-1;
    size_t  n_nondefault = 0;
    size_t  n_default = 0;
    /* VLA: same size constraint as goc_alts — n should be small. */
    size_t  indices[n];

    for (size_t i = 0; i < n; i++) {
        if (ops[i].op_kind == GOC_ALT_DEFAULT) {
            n_default++;
            if (n_default > 1) {
                fprintf(stderr, "goc_alts_sync: more than one default arm provided (got %zu)\n", n_default);
                abort();
            }
            default_idx = i;
        } else {
            indices[n_nondefault++] = i;
        }
    }

    alts_shuffle(indices, n_nondefault);

    /* ------------------------------------------------------------------ */
    /* Phase 2 — Non-blocking scan                                         */
    /* ------------------------------------------------------------------ */
    for (size_t si = 0; si < n_nondefault; si++) {
        size_t    i  = indices[si];
        goc_chan *ch = ops[i].ch;

        if (ops[i].op_kind == GOC_ALT_TAKE) {
            uv_mutex_lock(ch->lock);
            void *val = NULL;
            if (chan_take_from_buffer(ch, &val)) {
                uv_mutex_unlock(ch->lock);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = val, .ok = GOC_OK } };
            }
            if (chan_take_from_putter(ch, &val)) {
                uv_mutex_unlock(ch->lock);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = val, .ok = GOC_OK } };
            }
            if (ch->closed && ch->buf_count == 0) {
                uv_mutex_unlock(ch->lock);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = NULL, .ok = GOC_CLOSED } };
            }
            uv_mutex_unlock(ch->lock);

        } else { /* GOC_ALT_PUT */
            uv_mutex_lock(ch->lock);
            if (ch->closed) {
                uv_mutex_unlock(ch->lock);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = NULL, .ok = GOC_CLOSED } };
            }
            if (chan_put_to_taker(ch, ops[i].put_val)) {
                uv_mutex_unlock(ch->lock);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = NULL, .ok = GOC_OK } };
            }
            if (chan_put_to_buffer(ch, ops[i].put_val)) {
                uv_mutex_unlock(ch->lock);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = NULL, .ok = GOC_OK } };
            }
            uv_mutex_unlock(ch->lock);
        }
    }

    if (default_idx != (size_t)-1) {
        return (goc_alts_result){ .index = default_idx,
                                  .value = { .val = NULL, .ok = GOC_OK } };
    }

    /* ------------------------------------------------------------------ */
    /* Phase 3 — Prepare park (SYNC variant)                              */
    /* ------------------------------------------------------------------ */
    sem_t shared_sem;
    sem_init(&shared_sem, 0, 0);

    goc_entry **entries    = goc_malloc(n_nondefault * sizeof(goc_entry *));
    _Atomic int *fired_flag = goc_malloc(sizeof(_Atomic int));
    atomic_store_explicit(fired_flag, 0, memory_order_relaxed);

    for (size_t si = 0; si < n_nondefault; si++) {
        size_t i = indices[si];
        goc_entry *e = goc_malloc(sizeof(goc_entry));

        e->kind             = GOC_SYNC;
        e->cancelled        = 0;
        e->woken            = 0;
        e->fired            = fired_flag;
        e->next             = NULL;
        e->pool             = NULL;
        e->coro             = NULL;
        e->stack_canary_ptr = NULL;
        e->ok               = GOC_CLOSED;
        e->arm_idx          = i;
        e->sync_sem_ptr     = &shared_sem;

        if (ops[i].op_kind == GOC_ALT_TAKE) {
            e->result_slot = &e->cb_result;
            e->put_val     = NULL;
        } else {
            e->result_slot = &e->cb_result; /* will hold delivered val; not used for put */
            e->put_val     = ops[i].put_val;
        }

        e->fn      = NULL;
        e->fn_arg  = NULL;
        e->join_ch = NULL;
        e->cb      = NULL;
        e->put_cb  = NULL;
        e->ud      = NULL;
        e->cb_result = NULL;

        entries[si] = e;
    }

    /* Build sorted unique channel list. */
    goc_chan *stack_scratch[GOC_ALTS_STACK_THRESHOLD];
    goc_chan **sorted_chans;
    size_t    n_unique;

    if (n <= GOC_ALTS_STACK_THRESHOLD) {
        sorted_chans = stack_scratch;
    } else {
        sorted_chans = malloc(n * sizeof(goc_chan *));
    }

    n_unique = alts_dedup_sort_channels(ops, n, sorted_chans, n);

    /* ------------------------------------------------------------------ */
    /* Phase 4 — Acquire locks in ascending pointer order                  */
    /* ------------------------------------------------------------------ */
    for (size_t i = 0; i < n_unique; i++) {
        uv_mutex_lock(sorted_chans[i]->lock);
    }

    /* ------------------------------------------------------------------ */
    /* Phase 5 — Re-scan under locks                                       */
    /* ------------------------------------------------------------------ */
    for (size_t si = 0; si < n_nondefault; si++) {
        size_t    i  = indices[si];
        goc_chan *ch = ops[i].ch;

        if (ops[i].op_kind == GOC_ALT_TAKE) {
            void *val = NULL;
            if (chan_take_from_buffer(ch, &val)) {
                for (size_t j = n_unique; j-- > 0; )
                    uv_mutex_unlock(sorted_chans[j]->lock);
                if (n > GOC_ALTS_STACK_THRESHOLD) free(sorted_chans);
                sem_destroy(&shared_sem);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = val, .ok = GOC_OK } };
            }
            if (chan_take_from_putter(ch, &val)) {
                for (size_t j = n_unique; j-- > 0; )
                    uv_mutex_unlock(sorted_chans[j]->lock);
                if (n > GOC_ALTS_STACK_THRESHOLD) free(sorted_chans);
                sem_destroy(&shared_sem);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = val, .ok = GOC_OK } };
            }
            if (ch->closed && ch->buf_count == 0) {
                for (size_t j = n_unique; j-- > 0; )
                    uv_mutex_unlock(sorted_chans[j]->lock);
                if (n > GOC_ALTS_STACK_THRESHOLD) free(sorted_chans);
                sem_destroy(&shared_sem);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = NULL, .ok = GOC_CLOSED } };
            }

        } else { /* GOC_ALT_PUT */
            if (ch->closed) {
                for (size_t j = n_unique; j-- > 0; )
                    uv_mutex_unlock(sorted_chans[j]->lock);
                if (n > GOC_ALTS_STACK_THRESHOLD) free(sorted_chans);
                sem_destroy(&shared_sem);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = NULL, .ok = GOC_CLOSED } };
            }
            if (chan_put_to_taker(ch, ops[i].put_val)) {
                for (size_t j = n_unique; j-- > 0; )
                    uv_mutex_unlock(sorted_chans[j]->lock);
                if (n > GOC_ALTS_STACK_THRESHOLD) free(sorted_chans);
                sem_destroy(&shared_sem);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = NULL, .ok = GOC_OK } };
            }
            if (chan_put_to_buffer(ch, ops[i].put_val)) {
                for (size_t j = n_unique; j-- > 0; )
                    uv_mutex_unlock(sorted_chans[j]->lock);
                if (n > GOC_ALTS_STACK_THRESHOLD) free(sorted_chans);
                sem_destroy(&shared_sem);
                return (goc_alts_result){ .index = i,
                                          .value = { .val = NULL, .ok = GOC_OK } };
            }
        }
    }

    /* ------------------------------------------------------------------ */
    /* Phase 6 — Enqueue entries and block                                 */
    /* ------------------------------------------------------------------ */
    for (size_t si = 0; si < n_nondefault; si++) {
        size_t     i  = indices[si];
        goc_chan  *ch = ops[i].ch;
        goc_entry *e  = entries[si];

        if (ops[i].op_kind == GOC_ALT_TAKE) {
            goc_entry **pp = &ch->takers;
            while (*pp) pp = &(*pp)->next;
            *pp = e;
        } else {
            goc_entry **pp = &ch->putters;
            while (*pp) pp = &(*pp)->next;
            *pp = e;
        }
    }

    /* Release all locks in reverse order before blocking. */
    for (size_t j = n_unique; j-- > 0; )
        uv_mutex_unlock(sorted_chans[j]->lock);

    if (n > GOC_ALTS_STACK_THRESHOLD) free(sorted_chans);

    /* Block the OS thread until a channel fires. */
    sem_wait(&shared_sem);

    /* ------------------------------------------------------------------ */
    /* Phase 7 — After wake                                                */
    /* ------------------------------------------------------------------ */

    /*
     * sem_destroy is safe here: wake() wins the woken CAS and completes
     * sem_post before any other code path can post on shared_sem.  All
     * losing entries either have cancelled set or lost the woken CAS, so
     * no further sem_post on shared_sem will occur.
     */
    sem_destroy(&shared_sem);

    /* Cancel losers; locate winner. */
    goc_entry *winner = NULL;
    for (size_t si = 0; si < n_nondefault; si++) {
        goc_entry *e = entries[si];
        if (atomic_load_explicit(&e->woken, memory_order_acquire)) {
            winner = e;
        } else {
            atomic_store_explicit(&e->cancelled, 1, memory_order_release);
        }
    }

    /* Same invariant as goc_alts: exactly one entry must have won the woken CAS. */
    if (winner == NULL) {
        fprintf(stderr, "goc_alts_sync: no winner after wake — woken CAS invariant violated\n");
        abort();
    }

    void *result_val = (winner->result_slot) ? *winner->result_slot : NULL;
    return (goc_alts_result){ .index = winner->arm_idx,
                              .value = { .val = result_val, .ok = winner->ok } };
}
