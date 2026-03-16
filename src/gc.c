/* src/gc.c
 *
 * Owns:
 *   - goc_init / goc_shutdown lifecycle entry points
 *   - goc_malloc
 *   - live-channels registry (live_channels array + g_live_mutex)
 *   - pool registry initialisation (delegates to pool.c)
 */

#include <stdlib.h>
#include <assert.h>
#include <gc.h>
#include "../include/goc.h"
#include "internal.h"

/* ---------------------------------------------------------------------------
 * Live-channels registry
 * Only gc.c reads or writes these variables. channel.c accesses them only
 * through chan_register / chan_unregister which acquire g_live_mutex.
 * ---------------------------------------------------------------------------*/

static goc_chan** live_channels     = NULL;
static size_t     live_channels_len = 0;
static size_t     live_channels_cap = 0;
static uv_mutex_t g_live_mutex;   /* plain malloc; not GC-heap (uv constraint) */

/* ---------------------------------------------------------------------------
 * goc_malloc
 * ---------------------------------------------------------------------------*/

void* goc_malloc(size_t n) {
    return GC_malloc(n);
}

/* ---------------------------------------------------------------------------
 * live_channels_init  (internal; declared in internal.h)
 *
 * Allocates the live_channels array with an initial capacity of 64 entries
 * via plain malloc. Initialises g_live_mutex.
 * Must be called before the loop thread is spawned and before any channel
 * is created.
 * ---------------------------------------------------------------------------*/

void live_channels_init(void) {
    live_channels_cap = 64;
    live_channels     = malloc(live_channels_cap * sizeof(goc_chan*));
    assert(live_channels != NULL);
    live_channels_len = 0;
    uv_mutex_init(&g_live_mutex);
}

/* ---------------------------------------------------------------------------
 * chan_register  (internal; declared in internal.h)
 *
 * Called by channel.c:goc_chan_make after a new channel is fully initialised.
 * Appends ch to live_channels, growing the array (doubling) if needed.
 * ---------------------------------------------------------------------------*/

void chan_register(goc_chan* ch) {
    uv_mutex_lock(&g_live_mutex);

    if (live_channels_len == live_channels_cap) {
        size_t new_cap  = live_channels_cap * 2;
        goc_chan** grown = realloc(live_channels, new_cap * sizeof(goc_chan*));
        assert(grown != NULL);
        live_channels     = grown;
        live_channels_cap = new_cap;
    }

    live_channels[live_channels_len++] = ch;

    uv_mutex_unlock(&g_live_mutex);
}

/* ---------------------------------------------------------------------------
 * chan_unregister  (internal; declared in internal.h)
 *
 * Called by channel.c:goc_close. Removes ch from live_channels by linear
 * scan; fills the hole with the last element (swap-with-last).
 * ---------------------------------------------------------------------------*/

void chan_unregister(goc_chan* ch) {
    uv_mutex_lock(&g_live_mutex);

    for (size_t i = 0; i < live_channels_len; i++) {
        if (live_channels[i] == ch) {
            live_channels[i] = live_channels[--live_channels_len];
            break;
        }
    }

    uv_mutex_unlock(&g_live_mutex);
}

/* ---------------------------------------------------------------------------
 * goc_init
 *
 * Full initialisation sequence (must be called exactly once, from the main
 * thread, before any other libgoc function):
 *
 *   1. GC_INIT()
 *   2. GC_allow_register_threads()
 *   3. live_channels_init()    — allocates live_channels + g_live_mutex
 *   4. pool_registry_init()    — allocates pool registry (src/pool.c)
 *   5. loop_init()             — allocates g_loop, g_wakeup, spawns loop thread
 *   6. g_default_pool = goc_pool_make(N)
 *        N = GOC_POOL_THREADS env var if set to a valid positive integer,
 *            otherwise max(4, hardware_concurrency).
 * ---------------------------------------------------------------------------*/

void goc_init(void) {
    /* Step 1 — Initialise Boehm GC. */
    GC_INIT();

    /* Step 2 — Allow worker threads to register themselves with the GC. */
    GC_allow_register_threads();

    /* Step 3 — Live-channels registry (this file). */
    live_channels_init();

    /* Step 4 — Pool registry (pool.c). */
    pool_registry_init();

    /* Step 5 — libuv event loop + loop thread (loop.c). */
    loop_init();

    /* Step 6 — Default fiber pool.
     *
     * Determine thread count:
     *   - If GOC_POOL_THREADS is set and parses to a positive integer, use it.
     *   - Otherwise: max(4, hardware_concurrency) where hardware_concurrency
     *     is obtained from uv_available_parallelism().
     */
    size_t n = 0;

    const char* env = getenv("GOC_POOL_THREADS");
    if (env != NULL) {
        char*  end = NULL;
        long   v   = strtol(env, &end, 10);
        if (end != env && *end == '\0' && v > 0) {
            n = (size_t)v;
        }
    }

    if (n == 0) {
        unsigned int hw = uv_available_parallelism();
        n = (hw > 4) ? (size_t)hw : 4;
    }

    g_default_pool = goc_pool_make(n);
}

/* ---------------------------------------------------------------------------
 * goc_shutdown
 *
 * Orderly teardown. Blocks until all fibers on all pools have run to
 * completion, then tears down the event loop and frees all resources.
 * Intended to be called once at the end of main().
 *
 * Sequence:
 *   B.1  pool_registry_destroy_all()  — drains (blocks) and destroys every pool
 *   B.2  Destroy all channel mutexes; free live_channels; destroy g_live_mutex
 *   B.3  loop_shutdown()              — signals loop thread, joins, frees g_loop
 * ---------------------------------------------------------------------------*/

void goc_shutdown(void) {
    /* B.1 — Drain and destroy all registered pools (including g_default_pool). */
    pool_registry_destroy_all();

    /* B.2 — Destroy channel mutexes and tear down the live-channels registry.
     *
     * No lock is needed here: pool_registry_destroy_all() (B.1) blocks until
     * every pool has drained — all fibers have run to completion before we
     * reach this point. No other thread will call chan_register / chan_unregister
     * after that drain completes.
     */
    for (size_t i = 0; i < live_channels_len; i++) {
        goc_chan* ch = live_channels[i];
        uv_mutex_destroy(ch->lock);
        free(ch->lock);
    }
    free(live_channels);
    live_channels     = NULL;
    live_channels_len = 0;
    live_channels_cap = 0;

    uv_mutex_destroy(&g_live_mutex);

    /* B.3 — Shut down the event loop and join the loop thread. */
    loop_shutdown();
}
