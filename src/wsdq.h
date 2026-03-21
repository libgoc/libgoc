/*
 * src/wsdq.h
 *
 * Work-stealing deque (goc_wsdeque) and per-worker MPSC injector queue
 * (goc_injector) used by the work-stealing thread pool scheduler.
 *
 * goc_wsdeque — Chase–Lev circular work-stealing deque.
 *   - push_bottom / pop_bottom: owner-only (single owner thread).
 *   - steal_top: any thread; serialised internally by a short mutex.
 *
 * goc_injector — simple MPSC queue backed by a mutex-protected linked list.
 *   - injector_push: any thread (multiple producers).
 *   - injector_pop:  owner-only (single consumer).
 *
 * Both types store goc_entry* pointers opaquely; they never read or write
 * any field inside a goc_entry.
 */

#ifndef GOC_WSDQ_H
#define GOC_WSDQ_H

#include <stddef.h>
#include <stdatomic.h>
#include <uv.h>
#include "internal.h"   /* goc_entry */

/* -------------------------------------------------------------------------
 * goc_wsdeque — Chase–Lev work-stealing deque
 * ---------------------------------------------------------------------- */

struct goc_wsdeque {
    _Atomic size_t        bottom;   /* written only by owner */
    _Atomic size_t        top;      /* written by thieves (under steal_lock) */
    _Atomic(goc_entry**) buf;      /* pointer to ring buffer; updated on grow */
    size_t                capacity; /* current capacity; always a power of two */
    uv_mutex_t            steal_lock; /* serialises concurrent steal_top calls
                                         and buffer-swap during push_bottom growth */
};

typedef struct goc_wsdeque goc_wsdeque;

/* -------------------------------------------------------------------------
 * goc_injector — MPSC queue (mutex-protected linked list)
 * ---------------------------------------------------------------------- */

typedef struct goc_injector_node {
    goc_entry*               entry;
    struct goc_injector_node* next;
} goc_injector_node;

struct goc_injector {
    goc_injector_node* head;  /* oldest entry (pop side) */
    goc_injector_node* tail;  /* newest entry (push side) */
    uv_mutex_t         lock;
};

typedef struct goc_injector goc_injector;

/* -------------------------------------------------------------------------
 * goc_wsdeque API
 * ---------------------------------------------------------------------- */

/* Initialise a deque with initial capacity `cap` (must be a power of two). */
void wsdeque_init(goc_wsdeque* dq, size_t cap);

/* Release all memory owned by the deque (does not free `dq` itself).
 * Must NOT be called while any thread is concurrently calling steal_top. */
void wsdeque_destroy(goc_wsdeque* dq);

/* Owner-only: push entry onto the bottom. Grows the deque if full.
 * Must only be called by the single owner thread. */
void wsdeque_push_bottom(goc_wsdeque* dq, goc_entry* entry);

/* Owner-only: pop from the bottom. Returns NULL if empty.
 * Must only be called by the single owner thread. */
goc_entry* wsdeque_pop_bottom(goc_wsdeque* dq);

/* Any thread: steal from the top. Returns NULL if empty or lost a race. */
goc_entry* wsdeque_steal_top(goc_wsdeque* dq);

/* -------------------------------------------------------------------------
 * goc_injector API
 * ---------------------------------------------------------------------- */

/* Initialise the injector queue. */
void injector_init(goc_injector* inj);

/* Release all memory owned by the injector (does not free `inj` itself).
 * Drains and frees any remaining pending nodes; does not free the entries. */
void injector_destroy(goc_injector* inj);

/* Any thread: enqueue an entry for the owning worker. */
void injector_push(goc_injector* inj, goc_entry* entry);

/* Owner-only: dequeue an entry. Returns NULL if empty. */
goc_entry* injector_pop(goc_injector* inj);

#endif /* GOC_WSDQ_H */
