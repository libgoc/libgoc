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
#include "internal.h"

/* ---------------------------------------------------------------------------
 * Global state
 * --------------------------------------------------------------------------- */

goc_pool* g_default_pool = NULL;   /* set by goc_init; exported via extern in internal.h */

/* ---------------------------------------------------------------------------
 * goc_in_fiber
 * --------------------------------------------------------------------------- */

bool goc_in_fiber(void) {
    return mco_running() != NULL;
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
    goc_close(entry->join_ch);
}

/* ---------------------------------------------------------------------------
 * goc_go_on — launch a fiber on a specific pool
 * --------------------------------------------------------------------------- */

goc_chan* goc_go_on(goc_pool* pool, void (*fn)(void*), void* arg) {
    /* 1. Create the join channel (rendezvous; closed when fiber returns). */
    goc_chan* join_ch = goc_chan_make(0);

    /* 2. Allocate the entry on the GC heap (zero-initialised by GC_malloc). */
    goc_entry* entry = (goc_entry*)goc_malloc(sizeof(goc_entry));

    /* 3. Populate fiber launch fields. */
    entry->kind     = GOC_FIBER;
    entry->fn       = fn;
    entry->fn_arg   = arg;
    entry->join_ch  = join_ch;
    entry->pool     = pool;

    /* 4. Initialise the minicoro descriptor with the trampoline and stack size. */
    mco_desc desc   = mco_desc_init(fiber_trampoline, 0); /* 0 = use minicoro's default (adapts to active allocator) */
    desc.user_data  = entry;

    /* 5. Create the coroutine. */
    mco_result rc = mco_create(&entry->coro, &desc);
    if (rc != MCO_SUCCESS) {
        fprintf(stderr, "libgoc: mco_create failed (%d)\n", rc);
        abort();
    }

    /* 6. Record the canary pointer (lowest word of the fiber stack). */
    goc_stack_canary_init(entry);

    /* 7. Write the canary value so pool_worker_fn can detect stack overflow. */
    goc_stack_canary_set(entry->stack_canary_ptr);

    /* 8. Record this fiber's birth in live_count (exactly once per fiber),
     *    then hand the entry to the pool run queue.
     *    pool_fiber_born must come before post_to_run_queue so that live_count
     *    is non-zero before the worker could potentially decrement it. */
    pool_fiber_born(pool);
    post_to_run_queue(pool, entry);

    /* 9. Return the join channel to the caller. */
    return join_ch;
}

/* ---------------------------------------------------------------------------
 * goc_go — launch a fiber on the default pool
 * --------------------------------------------------------------------------- */

goc_chan* goc_go(void (*fn)(void*), void* arg) {
    return goc_go_on(g_default_pool, fn, arg);
}

/* ---------------------------------------------------------------------------
 * goc_default_pool — return the default pool created by goc_init
 * --------------------------------------------------------------------------- */

goc_pool* goc_default_pool(void) {
    return g_default_pool;
}
