/*
 * src/wsdq.c
 *
 * Implementation of goc_wsdq (Chase–Lev work-stealing deque) and
 * goc_injector (MPSC injector queue).
 *
 * Memory model notes — see pr.md §Step 3 for the full rationale:
 *
 *   wsdq_push_bottom:
 *     - Loads top with acquire, bottom with relaxed (owner is sole writer).
 *     - Buffer growth executes under steal_lock (prevents thief use-after-free).
 *     - Stores incremented bottom with release (pairs with steal_top acquire).
 *
 *   wsdq_steal_top:
 *     - Holds steal_lock for the entire operation (serialises concurrent thieves).
 *     - Loads top and bottom with acquire.
 *     - Uses CAS on top (acq_rel) instead of a plain store so that it loses
 *       the race if pop_bottom's own CAS on top already claimed the last element.
 *       Without the CAS, both paths could return the same entry (double delivery).
 *
 *   wsdq_pop_bottom:
 *     - Decrements bottom with seq_cst via atomic_fetch_sub (Chase–Lev
 *       correctness requirement; prevents double-delivery of last element).
 *     - Loads top with acquire.
 *     - Restores bottom on empty/CAS-lose with relaxed (owner-only).
 */

#include <stdlib.h>
#include <assert.h>
#include <stdatomic.h>
#include "wsdq.h"

/* =========================================================================
 * goc_wsdq — Chase–Lev circular work-stealing deque
 * ====================================================================== */

void wsdq_init(goc_wsdq* dq, size_t cap) {
    assert(cap > 0 && (cap & (cap - 1)) == 0);  /* must be power of two */

    atomic_init(&dq->bottom, 0);
    atomic_init(&dq->top,    0);
    dq->capacity = cap;

    goc_entry** buf = malloc(cap * sizeof(goc_entry*));
    atomic_init(&dq->buf, buf);

    uv_mutex_init(&dq->steal_lock);
}

void wsdq_destroy(goc_wsdq* dq) {
    goc_entry** buf = atomic_load_explicit(&dq->buf, memory_order_relaxed);
    free(buf);
    uv_mutex_destroy(&dq->steal_lock);
}

void wsdq_push_bottom(goc_wsdq* dq, goc_entry* entry) {
    size_t b   = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    size_t t   = atomic_load_explicit(&dq->top,    memory_order_acquire);
    goc_entry** buf = atomic_load_explicit(&dq->buf, memory_order_relaxed);
    size_t cap = dq->capacity;

    if (b - t >= cap) {
        /* Grow: allocate 2× buffer, copy live entries with modular indexing,
         * swap under steal_lock to prevent thieves reading the freed buffer. */
        size_t new_cap = cap * 2;
        goc_entry** new_buf = malloc(new_cap * sizeof(goc_entry*));

        for (size_t i = t; i < b; i++)
            new_buf[i % new_cap] = buf[i % cap];

        uv_mutex_lock(&dq->steal_lock);
        atomic_store_explicit(&dq->buf, new_buf, memory_order_release);
        dq->capacity = new_cap;
        uv_mutex_unlock(&dq->steal_lock);

        free(buf);
        buf = new_buf;
        cap = new_cap;
    }

    buf[b % cap] = entry;
    /* Release store: makes the slot write above visible to steal_top callers
     * that load bottom with acquire. */
    atomic_store_explicit(&dq->bottom, b + 1, memory_order_release);
}

goc_entry* wsdq_pop_bottom(goc_wsdq* dq) {
    /* seq_cst fetch_sub: required by Chase–Lev to form a total order with
     * the thief's acquire load of bottom, preventing double-delivery of the
     * last element on weak memory models (ARM, POWER). */
    size_t old_b = atomic_fetch_sub_explicit(&dq->bottom, 1, memory_order_seq_cst);

    goc_entry** buf = atomic_load_explicit(&dq->buf, memory_order_acquire);
    size_t cap = dq->capacity;

    size_t t = atomic_load_explicit(&dq->top, memory_order_acquire);

    /* Use old_b (pre-decrement value) for the emptiness check to avoid
     * unsigned wraparound when bottom == 0.  new_b = old_b - 1 would wrap
     * to SIZE_MAX if old_b == 0, and SIZE_MAX < t would be FALSE, causing
     * a spurious non-empty result and bottom corruption. */
    if (old_b <= t) {
        /* Deque was empty before the decrement (or a thief raced ahead).
         * Restore bottom to its pre-decrement value and return NULL. */
        atomic_store_explicit(&dq->bottom, old_b, memory_order_relaxed);
        return NULL;
    }

    size_t new_b = old_b - 1;  /* safe: old_b > t >= 0, so old_b >= 1 */
    goc_entry* entry = buf[new_b % cap];

    if (new_b > t) {
        /* More than one element: no race with thieves on this slot. */
        return entry;
    }

    /* new_b == t: last element; race with a concurrent steal_top. */
    size_t expected = t;
    if (!atomic_compare_exchange_strong_explicit(
            &dq->top, &expected, t + 1,
            memory_order_acq_rel, memory_order_relaxed)) {
        /* Thief won the race. Restore bottom; return NULL. */
        atomic_store_explicit(&dq->bottom, t + 1, memory_order_relaxed);
        return NULL;
    }
    /* We won the CAS: advance bottom past the consumed slot. */
    atomic_store_explicit(&dq->bottom, t + 1, memory_order_relaxed);
    return entry;
}

goc_entry* wsdq_steal_top(goc_wsdq* dq) {
    uv_mutex_lock(&dq->steal_lock);

    size_t t = atomic_load_explicit(&dq->top,    memory_order_acquire);
    size_t b = atomic_load_explicit(&dq->bottom, memory_order_acquire);

    if (t >= b) {
        uv_mutex_unlock(&dq->steal_lock);
        return NULL;
    }

    goc_entry** buf = atomic_load_explicit(&dq->buf, memory_order_acquire);
    size_t cap = dq->capacity;

    goc_entry* entry = buf[t % cap];

    /* CAS on top: interacts with pop_bottom's CAS in the last-element race.
     * Without a CAS, if pop_bottom wins the owner-side CAS on top between our
     * read of the entry and our store, both sides would return the same entry
     * (double delivery).  With a CAS, if pop_bottom already incremented top,
     * our CAS fails and we correctly return NULL.
     * The steal_lock serialises concurrent thieves, so the CAS effectively
     * always succeeds unless pop_bottom's CAS raced us on the last element. */
    size_t expected = t;
    bool ok = atomic_compare_exchange_strong_explicit(
                  &dq->top, &expected, t + 1,
                  memory_order_acq_rel, memory_order_relaxed);

    uv_mutex_unlock(&dq->steal_lock);
    return ok ? entry : NULL;
}

/* =========================================================================
 * goc_injector — MPSC queue (mutex-protected singly-linked list)
 * ====================================================================== */

void injector_init(goc_injector* inj) {
    inj->head = NULL;
    inj->tail = NULL;
    uv_mutex_init(&inj->lock);
}

void injector_destroy(goc_injector* inj) {
    uv_mutex_lock(&inj->lock);
    goc_injector_node* n = inj->head;
    while (n != NULL) {
        goc_injector_node* next = n->next;
        free(n);
        n = next;
    }
    inj->head = NULL;
    inj->tail = NULL;
    uv_mutex_unlock(&inj->lock);
    uv_mutex_destroy(&inj->lock);
}

void injector_push(goc_injector* inj, goc_entry* entry) {
    goc_injector_node* node = malloc(sizeof(goc_injector_node));
    node->entry = entry;
    node->next  = NULL;

    uv_mutex_lock(&inj->lock);
    if (inj->tail != NULL) {
        inj->tail->next = node;
    } else {
        inj->head = node;
    }
    inj->tail = node;
    uv_mutex_unlock(&inj->lock);
}

goc_entry* injector_pop(goc_injector* inj) {
    uv_mutex_lock(&inj->lock);
    goc_injector_node* node = inj->head;
    if (node == NULL) {
        uv_mutex_unlock(&inj->lock);
        return NULL;
    }
    inj->head = node->next;
    if (inj->head == NULL)
        inj->tail = NULL;
    uv_mutex_unlock(&inj->lock);

    goc_entry* entry = node->entry;
    free(node);
    return entry;
}
