/*
 * src/fiber.c — Fiber launch and lifecycle
 *
 * Implements goc_in_fiber, goc_go, and goc_go_on.
 * Owns g_default_pool (set by goc_init via gc.c).
 * Defines fiber_trampoline (the minicoro entry point).
 */

#include <stdlib.h>
#include <stdio.h>
#include <uv.h>
#include <gc.h>
#include "minicoro.h"
#include "../include/goc.h"
#include "../include/goc_stats.h"
#include "internal.h"

/* ---------------------------------------------------------------------------
 * Global state
 * --------------------------------------------------------------------------- */

goc_pool* g_default_pool = NULL;   /* set by goc_init; exported via extern in internal.h */

static _Atomic uint64_t g_fiber_id_counter = 0;

/* ---------------------------------------------------------------------------
 * goc_in_fiber
 * --------------------------------------------------------------------------- */

bool goc_in_fiber(void) {
    return mco_running() != NULL;
}

goc_pool* goc_current_pool(void) {
    mco_coro* co = mco_running();
    if (!co)
        return NULL;
    goc_entry* entry = (goc_entry*)mco_get_user_data(co);
    return entry ? entry->pool : NULL;
}

goc_pool* goc_current_or_default_pool(void) {
    goc_pool* pool = goc_current_pool();
    return pool ? pool : g_default_pool;
}

uv_thread_t goc_current_thread(void) {
    return uv_thread_self();
}

void* goc_current_fiber(void) {
    return (void*)mco_running();
}

/* ---------------------------------------------------------------------------
 * fiber_trampoline
 *
 * minicoro entry point for every fiber.  Retrieves the goc_entry that was
 * stored as user data on the coroutine, invokes the user function, then
 * closes the join channel so that any waiter on goc_take(join_ch) unblocks.
 * --------------------------------------------------------------------------- */

static void fiber_trampoline(mco_coro* co) {
    goc_entry* entry = (goc_entry*)mco_get_user_data(co);
    entry->fn(entry->fn_arg);
    /* Telemetry: fiber finished */
    GOC_STATS_FIBER_STATUS(entry->id, 0, goc_pool_id(entry->pool), GOC_FIBER_COMPLETED);
    goc_close(entry->join_ch);
}

/* ---------------------------------------------------------------------------
 * goc_fiber_entry_create — allocate and fully initialise a runnable fiber
 * --------------------------------------------------------------------------- */

goc_entry* goc_fiber_entry_create(goc_pool* pool,
                                  void (*fn)(void*),
                                  void* arg,
                                  goc_chan* join_ch) {
    /* 1. Allocate the entry on the GC heap (zero-initialised by GC_malloc). */
    goc_entry* entry = (goc_entry*)goc_malloc(sizeof(goc_entry));

    /* 2. Populate fiber launch fields. */
    entry->kind     = GOC_FIBER;
    entry->id       = atomic_fetch_add_explicit(&g_fiber_id_counter, 1, memory_order_relaxed);
    entry->fn       = fn;
    entry->fn_arg   = arg;
    entry->join_ch  = join_ch;
    entry->pool     = pool;

    /* 3. Initialise the minicoro descriptor with the trampoline and stack size. */
    mco_desc desc   = mco_desc_init(fiber_trampoline, LIBGOC_STACK_SIZE); /* 0 = use minicoro's default */
    desc.user_data  = entry;

    /* 4. Create the coroutine. */
    mco_result rc = mco_create(&entry->coro, &desc);
    if (rc != MCO_SUCCESS) {
        fprintf(stderr, "libgoc: mco_create failed (%d)\n", rc);
        abort();
    }

    /* 5. Register the fiber stack so BDW-GC scans it while the fiber is
     *    suspended.  Uses a GC_push_other_roots callback (gc.c) instead of
     *    GC_add_roots to avoid exhausting BDW-GC's fixed root-set table when
     *    large numbers of fibers are created (e.g. bench_spawn_idle).
     *    Unregistered in pool.c before mco_destroy when the fiber reaches
     *    MCO_DEAD. */
    void* fiber_stack_top    = (char*)entry->coro->stack_base + entry->coro->stack_size;
    GOC_DBG("fiber_create: entry=%p coro=%p stack_base=%p stack_top=%p stack_size=%zu\n",
            (void*)entry, (void*)entry->coro,
            (void*)entry->coro->stack_base, fiber_stack_top,
            entry->coro->stack_size);
    entry->fiber_root_handle = goc_fiber_root_register(entry->coro, fiber_stack_top, entry);

    /* 6. Record the canary pointer (lowest word of the fiber stack). */
    goc_stack_canary_init(entry);

    /* 7. Write the canary value so pool_worker_fn can detect stack overflow. */
    goc_stack_canary_set(entry->stack_canary_ptr);

    /* Telemetry: fiber created; -1 = not yet scheduled on any worker */
    GOC_STATS_FIBER_STATUS(entry->id, -1, goc_pool_id(entry->pool), GOC_FIBER_CREATED);

    return entry;
}

/* ---------------------------------------------------------------------------
 * goc_go_on — launch a fiber on a specific pool
 * --------------------------------------------------------------------------- */

goc_chan* goc_go_on(goc_pool* pool, void (*fn)(void*), void* arg) {
    /* 1. Create the join channel (rendezvous; closed when fiber returns). */
    goc_chan* join_ch = goc_chan_make(0);

    /* 2. Hand the spawn request to the pool. If the pool is currently below
     *    its live-fiber cap, the fiber is created immediately. Otherwise the
     *    request is queued and admitted later as earlier fibers finish. */
    pool_submit_spawn(pool, fn, arg, join_ch);

    /* 3. Return the join channel to the caller immediately. */
    return join_ch;
}

/* ---------------------------------------------------------------------------
 * goc_go — launch a fiber on the default pool
 * --------------------------------------------------------------------------- */

goc_chan* goc_go(void (*fn)(void*), void* arg) {
    return goc_go_on(goc_current_or_default_pool(), fn, arg);
}

/* ---------------------------------------------------------------------------
 * goc_default_pool — return the default pool created by goc_init
 * --------------------------------------------------------------------------- */

goc_pool* goc_default_pool(void) {
    return g_default_pool;
}
