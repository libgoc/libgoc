/* src/gc.c
 *
 * Owns:
 *   - goc_init / goc_shutdown lifecycle entry points
 *   - goc_malloc
 *   - live-channels registry (live_channels array + g_live_mutex)
 *   - pool registry initialisation (delegates to pool.c)
 *   - gc_pthread_create / gc_pthread_join (Windows only; POSIX aliases in
 *     internal.h)
 */

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>
#include <gc.h>
#include <gc/gc_mark.h>
#include "../include/goc.h"
#include "chan_type.h"
#include "internal.h"

/* ---------------------------------------------------------------------------
 * gc_pthread_create / gc_pthread_join  (Windows-only implementation)
 *
 * On POSIX these are #defined as GC_pthread_create / GC_pthread_join in
 * internal.h.  On Windows, bdwgc (MSYS2 UCRT64) is compiled with Win32
 * threads and does not provide GC_pthread_create, so we implement the
 * equivalent behaviour here: a generic trampoline registers the thread with
 * the GC before calling the real thread function and unregisters it on exit.
 * ---------------------------------------------------------------------------*/

#ifdef _WIN32

typedef struct {
    void* (*fn)(void*);
    void*  arg;
} gc_thread_args_t;

static void* gc_thread_trampoline(void* raw)
{
    /* Extract fn/arg and free the heap-allocated carrier before we touch
     * any GC-managed memory (avoids a window where the carrier is live but
     * not yet reachable by the GC). */
    gc_thread_args_t* w   = (gc_thread_args_t*)raw;
    void* (*fn)(void*)    = w->fn;
    void*  arg            = w->arg;
    free(w);

    struct GC_stack_base sb;
    GC_get_stack_base(&sb);
    GC_register_my_thread(&sb);
    void* ret = fn(arg);
    GC_unregister_my_thread();
    return ret;
}

int gc_pthread_create(pthread_t* t, const pthread_attr_t* a,
                      void* (*fn)(void*), void* arg)
{
    gc_thread_args_t* w = malloc(sizeof(gc_thread_args_t));
    if (!w) return ENOMEM;
    w->fn  = fn;
    w->arg = arg;
    int rc = pthread_create(t, a, gc_thread_trampoline, w);
    if (rc != 0) free(w);
    return rc;
}

int gc_pthread_join(pthread_t t, void** retval)
{
    return pthread_join(t, retval);
}

#endif /* _WIN32 */

/* ---------------------------------------------------------------------------
 * Fiber stack root push  (replaces GC_add_roots / GC_remove_roots)
 *
 * Problem: GC_add_roots / GC_remove_roots use a fixed internal table
 * (MAX_ROOT_SETS ≈ 2048 in BDW-GC 8.2.6).  Benchmarks that spawn large
 * numbers of fibers (e.g. bench_spawn_idle with 200,000 fibers) exhaust
 * the table, causing BDW-GC to abort with "Too many root sets".
 *
 * Solution: maintain a lock-free linked list of live fiber stack ranges
 * and register a GC_push_other_roots callback that iterates the list and
 * calls GC_push_all_eager for each live entry.  This bypasses the root-set
 * table entirely — the callback is called during each GC mark phase.
 *
 * Thread-safety / stop-the-world:
 *   BDW-GC stops the world (via SIGPWR on Linux) before calling the callback.
 *   A pthread_mutex in the callback would deadlock if a stopped thread held
 *   it.  Instead the list uses only atomic operations:
 *     - register: malloc a node, store fields, CAS-prepend to head.
 *     - unregister: scan the list and CAS stack_base from base to NULL.
 *   Nodes are never removed from the list; dead entries (stack_base == NULL)
 *   are skipped by the callback.  Memory overhead: one malloc'd node per
 *   fiber lifetime.
 * --------------------------------------------------------------------------- */

typedef struct fiber_root_node fiber_root_node;
struct fiber_root_node {
    _Atomic(void*)              stack_base; /* NULL = dead / unregistered */
    void*                       stack_top;
    _Atomic(fiber_root_node*)   next;
};

static _Atomic(fiber_root_node*) fiber_root_head = NULL;
static GC_push_other_roots_proc  prev_push_roots  = NULL;

static void push_fiber_roots(void)
{
    fiber_root_node* n = atomic_load_explicit(&fiber_root_head, memory_order_acquire);
    while (n) {
        void* base = atomic_load_explicit(&n->stack_base, memory_order_acquire);
        if (base != NULL)
            GC_push_all_eager(base, n->stack_top);
        n = atomic_load_explicit(&n->next, memory_order_acquire);
    }
    if (prev_push_roots)
        prev_push_roots();
}

void* goc_fiber_root_register(void* base, void* top)
{
    fiber_root_node* n = malloc(sizeof(fiber_root_node));
    atomic_init(&n->stack_base, base);
    n->stack_top = top;
    fiber_root_node* head;
    do {
        head = atomic_load_explicit(&fiber_root_head, memory_order_relaxed);
        atomic_store_explicit(&n->next, head, memory_order_relaxed);
    } while (!atomic_compare_exchange_weak_explicit(
                 &fiber_root_head, &head, n,
                 memory_order_release, memory_order_relaxed));
    return n;
}

void goc_fiber_root_unregister(void* handle)
{
    fiber_root_node* n = (fiber_root_node*)handle;
    void* expected = atomic_load_explicit(&n->stack_base, memory_order_relaxed);
    atomic_compare_exchange_strong_explicit(
        &n->stack_base, &expected, NULL,
        memory_order_acq_rel, memory_order_acquire);
}

void goc_fiber_roots_init(void)
{
    prev_push_roots = GC_get_push_other_roots();
    GC_set_push_other_roots(push_fiber_roots);
}

/* ---------------------------------------------------------------------------
 * Live-channels registry
 * Only gc.c reads or writes these variables. channel.c accesses them only
 * through chan_register / chan_unregister which acquire g_live_mutex.
 * ---------------------------------------------------------------------------*/

static goc_chan** live_channels     = NULL;
static size_t     live_channels_len = 0;
static size_t     live_channels_cap = 0;
static uv_mutex_t g_live_mutex;   /* plain malloc; not GC-heap (uv constraint) */

static pthread_t  g_main_thread;
static bool       g_main_thread_set = false;

__attribute__((constructor))
static void capture_main_thread_at_load(void)
{
    g_main_thread = pthread_self();
    g_main_thread_set = true;
}

static void lifecycle_abort_non_main_thread(const char* fn_name)
{
    if (!g_main_thread_set) {
        g_main_thread = pthread_self();
        g_main_thread_set = true;
    }

    if (!pthread_equal(pthread_self(), g_main_thread)) {
        fprintf(stderr,
                "libgoc: %s must be called from the main thread\n",
                fn_name);
        abort();
    }
}

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
    lifecycle_abort_non_main_thread("goc_init");

    /* Step 1 — Initialise Boehm GC. */
    GC_INIT();

    /* Step 1.1 — Register fiber-stack push callback (replaces GC_add_roots). */
    goc_fiber_roots_init();

    /* Step 2 — Allow worker threads to register themselves with the GC. */
    GC_allow_register_threads();

    /* Step 3 — Live-channels registry (this file). */
    live_channels_init();

    /* Step 4 — Pool registry (pool.c). */
    pool_registry_init();

    /* Step 4.1 — Mutex registry (mutex.c). */
    mutex_registry_init();

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
    lifecycle_abort_non_main_thread("goc_shutdown");

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

    /* B.2.1 — Destroy all RW mutex internal locks. */
    mutex_registry_destroy_all();

    /* B.3 — Shut down the event loop and join the loop thread. */
    loop_shutdown();
}
