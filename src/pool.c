/*
 * src/pool.c
 *
 * Thread pool workers, work-stealing deque scheduler, pool registry, and
 * drain logic.  Defines goc_pool and all pool operations.
 *
 * Internal symbols exposed via internal.h:
 *   pool_registry_init()
 *   pool_registry_destroy_all()
 *   post_to_run_queue()
 *
 * Public API implemented here:
 *   goc_pool_make()
 *   goc_pool_destroy()
 *   goc_pool_destroy_timeout()
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <uv.h>
#include <gc.h>
#include "../include/goc.h"
#include "../include/goc_stats.h"
#include "minicoro.h"
#include "internal.h"
#include "wsdq.h"

/* -------------------------------------------------------------------------
 * goc_worker — per-worker state
 * ---------------------------------------------------------------------- */

typedef struct {
    uv_thread_t      thread;
    goc_wsdq         deque;
    goc_injector     injector;        /* MPSC queue: external callers push here */
    uv_sem_t         idle_sem;
    size_t           index;
    goc_pool*        pool;
    _Atomic uint64_t steal_attempts;  /* relaxed counter; read at STOPPED event */
    _Atomic uint64_t steal_successes;
} goc_worker;

/* Global lifetime steal totals across all pools/workers — read via accessor */
static _Atomic uint64_t g_steal_attempts  = 0;
static _Atomic uint64_t g_steal_successes = 0;

/* -------------------------------------------------------------------------
 * goc_pool — full definition (opaque outside pool.c)
 * ---------------------------------------------------------------------- */

struct goc_pool {
    goc_worker*        workers;
    size_t             thread_count;
    size_t             max_live_fibers;
    _Atomic size_t     idle_count;
    _Atomic size_t     next_push_idx;
    _Atomic int        shutdown;
    uv_mutex_t         drain_mutex;
    uv_cond_t          drain_cond;
    size_t             live_count;        /* spawned fibers not yet completed (includes queued spawns) */
    size_t             resident_count;    /* fibers with an allocated coroutine/stack */
    goc_spawn_req*     pending_spawn_head;
    goc_spawn_req*     pending_spawn_tail;
};

/* -------------------------------------------------------------------------
 * _Thread_local worker pointer
 *
 * Set at the top of pool_worker_fn; cleared on exit.  Used by
 * post_to_run_queue to detect internal callers (a fiber running on a pool
 * thread) versus external callers (main thread, libuv loop, other pool).
 * ---------------------------------------------------------------------- */

static _Thread_local goc_worker* tl_worker = NULL;

/* Wake exactly one sleeping worker (optionally excluding one index). */
/* -------------------------------------------------------------------------
 * Pool registry (file-scope; owned entirely by pool.c)
 * ---------------------------------------------------------------------- */

static goc_pool**  g_pool_registry     = NULL;
static size_t      g_pool_registry_len = 0;
static size_t      g_pool_registry_cap = 0;
static uv_mutex_t  g_pool_registry_mutex;

/* -------------------------------------------------------------------------
 * pool_registry_init — allocates registry + mutex; called from gc.c:goc_init
 * ---------------------------------------------------------------------- */

void pool_registry_init(void) {
    g_pool_registry_cap = 8;
    g_pool_registry     = malloc(g_pool_registry_cap * sizeof(goc_pool*));
    g_pool_registry_len = 0;
    uv_mutex_init(&g_pool_registry_mutex);
}

/* -------------------------------------------------------------------------
 * pool_registry_destroy_all — called from gc.c:goc_shutdown (B.1)
 * ---------------------------------------------------------------------- */

void pool_registry_destroy_all(void) {
    uv_mutex_lock(&g_pool_registry_mutex);
    /* Snapshot the list before destroying, since goc_pool_destroy will
     * attempt to unregister itself and take the same lock.  We clear
     * the registry now so that unregister finds nothing to do. */
    size_t    len   = g_pool_registry_len;
    goc_pool** snap = malloc(len * sizeof(goc_pool*));
    memcpy(snap, g_pool_registry, len * sizeof(goc_pool*));
    g_pool_registry_len = 0;
    uv_mutex_unlock(&g_pool_registry_mutex);

    for (size_t i = 0; i < len; i++) {
        goc_pool_destroy(snap[i]);
    }
    free(snap);

    uv_mutex_destroy(&g_pool_registry_mutex);
    free(g_pool_registry);
    g_pool_registry = NULL;
}

/* -------------------------------------------------------------------------
 * registry_add / registry_remove (static helpers)
 * ---------------------------------------------------------------------- */

static void registry_add(goc_pool* pool) {
    uv_mutex_lock(&g_pool_registry_mutex);
    if (g_pool_registry_len == g_pool_registry_cap) {
        g_pool_registry_cap *= 2;
        g_pool_registry = realloc(g_pool_registry,
                                  g_pool_registry_cap * sizeof(goc_pool*));
    }
    g_pool_registry[g_pool_registry_len++] = pool;
    uv_mutex_unlock(&g_pool_registry_mutex);
}

static void registry_remove(goc_pool* pool) {
    uv_mutex_lock(&g_pool_registry_mutex);
    for (size_t i = 0; i < g_pool_registry_len; i++) {
        if (g_pool_registry[i] == pool) {
            g_pool_registry[i] = g_pool_registry[--g_pool_registry_len];
            break;
        }
    }
    uv_mutex_unlock(&g_pool_registry_mutex);
}

/* -------------------------------------------------------------------------
 * Spawn throttling helpers
 * ---------------------------------------------------------------------- */

static void pool_stack_size_probe(mco_coro* co) {
    (void)co;
}

static size_t pool_default_stack_size_bytes(void) {
    /* Let minicoro decide the effective stack size so this code stays aligned
     * with minicoro defaults/constants (including MCO_DEFAULT_STACK_SIZE,
     * MCO_MIN_STACK_SIZE, and alignment behavior) in both canary and vmem. */
    mco_desc desc = mco_desc_init(pool_stack_size_probe, LIBGOC_STACK_SIZE);
    return desc.stack_size;
}

static size_t pool_default_max_live_fibers(void) {
    const char* env = getenv("GOC_MAX_LIVE_FIBERS");
    if (env != NULL) {
        char* end = NULL;
        long  v   = strtol(env, &end, 10);
        if (end != env && *end == '\0' && v >= 0) {
            return (size_t)v;
        }
    }

     /*
      * Default admission cap is derived from memory budget and per-fiber stack
      * size.
      *
      * Formula (no clamp):
      *   floor(factor * (available_memory / stack_size))
      *
      * The factor (<1.0) intentionally leaves headroom for GC metadata,
      * channels/queues, allocator overhead, and the rest of the process.
      */
    const size_t stack_size = pool_default_stack_size_bytes();
    const uint64_t mem_bytes = uv_get_total_memory();

    return (size_t)(GOC_DEFAULT_LIVE_FIBER_MEMORY_FACTOR *
                          ((double)mem_bytes / (double)stack_size));
}

static bool pool_spawn_cap_reached_locked(goc_pool* pool) {
    return pool->max_live_fibers != 0 && pool->resident_count >= pool->max_live_fibers;
}

static void pool_enqueue_spawn_locked(goc_pool* pool, goc_spawn_req* req) {
    req->next = NULL;
    if (pool->pending_spawn_tail != NULL) {
        pool->pending_spawn_tail->next = req;
    } else {
        pool->pending_spawn_head = req;
    }
    pool->pending_spawn_tail = req;
}

static goc_spawn_req* pool_collect_admitted_spawns_locked(goc_pool* pool) {
    goc_spawn_req* admitted_head = NULL;
    goc_spawn_req* admitted_tail = NULL;

    while (pool->pending_spawn_head != NULL && !pool_spawn_cap_reached_locked(pool)) {
        goc_spawn_req* req = pool->pending_spawn_head;
        pool->pending_spawn_head = req->next;
        if (pool->pending_spawn_head == NULL)
            pool->pending_spawn_tail = NULL;

        req->next = NULL;
        if (admitted_tail != NULL) {
            admitted_tail->next = req;
        } else {
            admitted_head = req;
        }
        admitted_tail = req;
        pool->resident_count++;
    }

    return admitted_head;
}

static void pool_dispatch_spawn_list(goc_pool* pool, goc_spawn_req* reqs) {
    while (reqs != NULL) {
        goc_spawn_req* next = reqs->next;
        goc_entry* entry = goc_fiber_entry_create(pool,
                                                  reqs->fn,
                                                  reqs->fn_arg,
                                                  reqs->join_ch);
        post_to_run_queue(pool, entry);
        GC_free(reqs);
        reqs = next;
    }
}

void pool_submit_spawn(goc_pool* pool,
                       void (*fn)(void*),
                       void* arg,
                       goc_chan* join_ch) {
    /* Same-pool spawns originating from a currently running fiber must remain
     * eager even when the pool is at its admission cap. Otherwise a parent
     * fiber can block waiting for a child that never materialises because all
     * resident slots are occupied by similarly waiting parents (prime-sieve
     * style pipelines are a concrete example). The throttle is therefore
     * aimed at external burst spawners (main thread, callbacks, other pools),
     * not at intra-pool dependency edges. */
    bool bypass_throttle = (tl_worker != NULL && tl_worker->pool == pool);

    uv_mutex_lock(&pool->drain_mutex);

    /* live_count tracks all accepted spawn requests, including ones still
     * queued behind the throttle. This keeps pool destruction honest: it
     * must wait for deferred spawns too, not just already-materialised ones. */
    pool->live_count++;

    if (bypass_throttle ||
        (pool->pending_spawn_head == NULL && !pool_spawn_cap_reached_locked(pool))) {
        pool->resident_count++;
        size_t live = pool->live_count;
        uv_mutex_unlock(&pool->drain_mutex);

        goc_entry* entry = goc_fiber_entry_create(pool, fn, arg, join_ch);
        GOC_STATS_WORKER_STATUS(
            (int)atomic_load_explicit(&pool->next_push_idx, memory_order_relaxed)
                 % (int)pool->thread_count,
            pool, GOC_WORKER_RUNNING, (int)live, 0, 0);
        post_to_run_queue(pool, entry);
        return;
    }

    goc_spawn_req* req = (goc_spawn_req*)GC_malloc_uncollectable(sizeof(goc_spawn_req));
    if (req == NULL) {
        uv_mutex_unlock(&pool->drain_mutex);
        fprintf(stderr, "libgoc: failed to allocate deferred spawn request\n");
        abort();
    }

    req->fn      = fn;
    req->fn_arg  = arg;
    req->join_ch = join_ch;
    req->next    = NULL;
    pool_enqueue_spawn_locked(pool, req);

    goc_spawn_req* admitted = pool_collect_admitted_spawns_locked(pool);
    uv_mutex_unlock(&pool->drain_mutex);

    pool_dispatch_spawn_list(pool, admitted);
}

/* -------------------------------------------------------------------------
 * pool_worker_fn — thread entry point (work-stealing loop)
 * ---------------------------------------------------------------------- */

static void pool_worker_fn(void* arg) {
    tl_worker     = (goc_worker*)arg;
    goc_pool* pool = tl_worker->pool;

    /* Per-worker PRNG seed for randomised steal order (anti-thundering-herd).
     * Combines index with the worker pointer for uniqueness. */
    uint32_t seed = (uint32_t)(tl_worker->index ^ (uintptr_t)tl_worker);

    goc_entry* entry;

    while (!atomic_load_explicit(&pool->shutdown, memory_order_acquire)) {

        /* 1. Drain own injector first (entries posted by external callers). */
        entry = injector_pop(&tl_worker->injector);
        if (entry != NULL) goto run;

        /* 2. Pop from own deque (LIFO, cache-warm). */
        entry = wsdq_pop_bottom(&tl_worker->deque);
        if (entry != NULL) goto run;

        /* 3. Steal phase: try each other worker exactly once, starting from a
         *    randomised offset to avoid thundering-herd on a fixed victim.
         *    Self is explicitly excluded. */
        if (pool->thread_count > 1) {
            /* xorshift32 — portable thread-local PRNG (no rand_r on Windows). */
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            size_t offset = 1 + (size_t)(seed % (uint32_t)(pool->thread_count - 1));
            for (size_t i = 0; i < pool->thread_count; i++) {
                size_t victim = (tl_worker->index + offset + i) % pool->thread_count;
                if (victim == tl_worker->index) continue;
#ifdef GOC_ENABLE_STATS
                atomic_fetch_add_explicit(&tl_worker->steal_attempts, 1, memory_order_relaxed);
                atomic_fetch_add_explicit(&g_steal_attempts,           1, memory_order_relaxed);
#endif
                entry = wsdq_steal_top(&pool->workers[victim].deque);
                if (entry != NULL) {
#ifdef GOC_ENABLE_STATS
                    atomic_fetch_add_explicit(&tl_worker->steal_successes, 1, memory_order_relaxed);
                    atomic_fetch_add_explicit(&g_steal_successes,           1, memory_order_relaxed);
#endif
                    goto run;
                }
            }
        }

        /* 4. No work found anywhere — go idle.
         *
         * Increment idle_count with seq_cst BEFORE sleeping so that a
         * concurrent post_to_run_queue can observe it after its own seq_cst
         * operation.  Then double-check own injector and deque to close the
         * sleep-miss race window. */
        atomic_fetch_add_explicit(&pool->idle_count, 1, memory_order_seq_cst);

        entry = injector_pop(&tl_worker->injector);
        if (entry == NULL)
            entry = wsdq_pop_bottom(&tl_worker->deque);
        if (entry != NULL) {
            atomic_fetch_sub_explicit(&pool->idle_count, 1, memory_order_relaxed);
            goto run;
        }

        GOC_STATS_WORKER_STATUS((int)tl_worker->index, pool, GOC_WORKER_IDLE, (int)pool->live_count, 0, 0);
        uv_sem_wait(&tl_worker->idle_sem);
        atomic_fetch_sub_explicit(&pool->idle_count, 1, memory_order_relaxed);
        GOC_STATS_WORKER_STATUS((int)tl_worker->index, pool, GOC_WORKER_RUNNING, (int)pool->live_count, 0, 0);
        continue;   /* re-check shutdown and try again */

run:
        /* --- from here, identical to old pool_worker_fn --- */

        /* Canary check — abort on stack overflow before corrupting anything. */
        goc_stack_canary_check(entry->stack_canary_ptr);

        /* Save coro handle before resuming: another worker can race and
         * advance the coroutine lifecycle while we are in mco_resume. Keep a
         * stable handle (`coro`) across the call; the mco_coro object remains
         * valid until mco_destroy. */
        mco_coro* coro = entry->coro;

        /* Redirect GC stack scan to the fiber's stack for the duration of
         * mco_resume (see DESIGN.md §GC Stack Bottom Redirect). */
        struct GC_stack_base orig_sb;
        GC_get_my_stackbottom(&orig_sb);
        struct GC_stack_base fiber_sb;
        fiber_sb.mem_base = (char*)coro->stack_base + coro->stack_size;
        GC_set_stackbottom(NULL, &fiber_sb);

        mco_resume(coro);

        GC_set_stackbottom(NULL, &orig_sb);

        goc_entry* fe = (goc_entry*)mco_get_user_data(coro);
        mco_state st = mco_status(coro);

        /* Update cached fiber SP so the next GC cycle scans only the used
         * portion of the stack instead of the full vmem allocation. */
        if (st == MCO_SUSPENDED && fe != NULL)
            goc_fiber_root_update_sp(fe->fiber_root_handle, coro);

        /* If the fiber just parked, release the yield-gate so that any
         * wake() spinning on parked==0 can proceed. */
        if (fe != NULL)
            atomic_store_explicit(&fe->parked, 1, memory_order_release);

        if (st == MCO_DEAD) {
            if (fe != NULL) {
                goc_fiber_root_unregister(fe->fiber_root_handle);
            }
            mco_destroy(coro);

            uv_mutex_lock(&pool->drain_mutex);
            if (pool->resident_count > 0)
                pool->resident_count--;
            pool->live_count--;
            goc_spawn_req* admitted = pool_collect_admitted_spawns_locked(pool);
            uv_cond_broadcast(&pool->drain_cond);
            uv_mutex_unlock(&pool->drain_mutex);

            GOC_STATS_WORKER_STATUS((int)tl_worker->index, pool, GOC_WORKER_RUNNING, (int)pool->live_count, 0, 0);
            pool_dispatch_spawn_list(pool, admitted);
        }
    }

    GOC_STATS_WORKER_STATUS((int)tl_worker->index, pool, GOC_WORKER_STOPPED, 0,
                            atomic_load_explicit(&tl_worker->steal_attempts,  memory_order_relaxed),
                            atomic_load_explicit(&tl_worker->steal_successes, memory_order_relaxed));
    tl_worker = NULL;
}

/* -------------------------------------------------------------------------
 * post_to_run_queue — internal; called from fiber.c and channel.c
 *
 * Routes the entry to either the calling worker's own deque (internal
 * caller — a fiber running on a pool thread) or to a target worker's MPSC
 * injector (external caller — main thread, libuv loop, other pool).
 *
 * Sleep-miss race closure: both paths complete their write with a seq_cst
 * effect before reading idle_count, pairing with the worker's seq_cst
 * increment in pool_worker_fn.
 * ---------------------------------------------------------------------- */

void post_to_run_queue(goc_pool* pool, goc_entry* entry) {
    goc_worker* w = tl_worker;
    if (w != NULL && w->pool == pool) {
        /* Internal caller: push to executing worker's own deque.
         * Safe: the owner is inside mco_resume, not touching the deque. */
        wsdq_push_bottom(&w->deque, entry);

        /* Explicit seq_cst fence: the bottom store in wsdq_push_bottom is
         * only release; we need seq_cst here so idle_count reads below form a
         * total order with the worker's seq_cst increment (ARM/POWER safety). */
        atomic_thread_fence(memory_order_seq_cst);

        /* Internal enqueue stays local to the current worker and does not
         * proactively wake peers. This avoids cross-worker steal/resume races
         * in ping-pong style workloads where locality matters for progress.
         * External posts (below) still wake an idle target worker. */
        (void)pool;
    } else {
        /* External caller: push into target worker's injector (MPSC-safe).
         * Round-robin across workers for load distribution. */
        size_t idx = atomic_fetch_add_explicit(&pool->next_push_idx, 1,
                                               memory_order_relaxed)
                     % pool->thread_count;
        injector_push(&pool->workers[idx].injector, entry);

        /* injector_push holds the mutex for the full operation; the unlock
         * is a full memory barrier, providing the seq_cst effect needed for
         * the sleep-miss race closure. */
        if (atomic_load_explicit(&pool->idle_count, memory_order_seq_cst) > 0) {
            uv_sem_post(&pool->workers[idx].idle_sem);
        }
    }
}

/* -------------------------------------------------------------------------
 * pool_abort_if_called_from_worker
 *
 * Destroying a pool from one of its own worker threads is invalid: the
 * destroy path waits on drain/join and would attempt to join the caller
 * thread itself. Detect this explicitly and abort with a diagnostic.
 * ---------------------------------------------------------------------- */

static void pool_abort_if_called_from_worker(goc_pool* pool, const char* api_name) {
    uv_thread_t self = uv_thread_self();
    for (size_t i = 0; i < pool->thread_count; i++) {
        if (uv_thread_equal(&self, &pool->workers[i].thread)) {
            fprintf(stderr,
                    "libgoc: %s called from within target pool worker thread; "
                    "this is unsupported and would deadlock\n",
                    api_name);
            abort();
        }
    }
}

/* -------------------------------------------------------------------------
 * goc_pool_make
 * ---------------------------------------------------------------------- */

goc_pool* goc_pool_make(size_t threads) {
    goc_pool* pool = malloc(sizeof(goc_pool));
    memset(pool, 0, sizeof(goc_pool));

    pool->thread_count = threads;
    pool->max_live_fibers = pool_default_max_live_fibers();
    pool->workers      = malloc(threads * sizeof(goc_worker));

    for (size_t i = 0; i < threads; i++) {
        wsdq_init(&pool->workers[i].deque, 256);
        injector_init(&pool->workers[i].injector);
        uv_sem_init(&pool->workers[i].idle_sem, 0);
        pool->workers[i].index = i;
        pool->workers[i].pool  = pool;
        atomic_store_explicit(&pool->workers[i].steal_attempts,  0, memory_order_relaxed);
        atomic_store_explicit(&pool->workers[i].steal_successes, 0, memory_order_relaxed);
    }

    atomic_store(&pool->idle_count,    0);
    atomic_store(&pool->next_push_idx, 0);
    atomic_store(&pool->shutdown,      0);

    uv_mutex_init(&pool->drain_mutex);
    uv_cond_init(&pool->drain_cond);

    pool->live_count        = 0;
    pool->resident_count    = 0;
    pool->pending_spawn_head = NULL;
    pool->pending_spawn_tail = NULL;

    for (size_t i = 0; i < threads; i++) {
        int rc = goc_thread_create(&pool->workers[i].thread,
                                   pool_worker_fn, &pool->workers[i]);
        if (rc != 0) {
            fprintf(stderr,
                    "libgoc: failed to create worker thread %zu/%zu (errno=%d)\n",
                    i + 1, threads, rc);
            abort();
        }
        GOC_STATS_WORKER_STATUS((int)i, pool, GOC_WORKER_CREATED, 0, 0, 0);
    }

    GOC_STATS_POOL_STATUS(goc_box_int(pool), GOC_POOL_CREATED, (int)threads);
    registry_add(pool);
    return pool;
}

/* -------------------------------------------------------------------------
 * goc_pool_destroy
 * ---------------------------------------------------------------------- */

void goc_pool_destroy(goc_pool* pool) {
    pool_abort_if_called_from_worker(pool, "goc_pool_destroy");
    GOC_STATS_POOL_STATUS(goc_box_int(pool), GOC_POOL_DESTROYED, (int)pool->thread_count);

    /* 1. Wait for all live fibers to exit (live_count reaches zero). */
    uv_mutex_lock(&pool->drain_mutex);
    while (pool->live_count > 0) {
        uv_cond_wait(&pool->drain_cond, &pool->drain_mutex);
    }
    uv_mutex_unlock(&pool->drain_mutex);

    /* 2. Signal workers to exit. */
    atomic_store_explicit(&pool->shutdown, 1, memory_order_release);

    /* 3. Unblock all waiting workers (one post per worker). */
    for (size_t i = 0; i < pool->thread_count; i++) {
        uv_sem_post(&pool->workers[i].idle_sem);
    }

    /* 4. Reap worker threads. */
    for (size_t i = 0; i < pool->thread_count; i++) {
        goc_thread_join(&pool->workers[i].thread);
    }

    /* 5. Destroy per-worker resources. */
    for (size_t i = 0; i < pool->thread_count; i++) {
        uv_sem_destroy(&pool->workers[i].idle_sem);
        wsdq_destroy(&pool->workers[i].deque);
        injector_destroy(&pool->workers[i].injector);
    }

    /* 6. Destroy drain primitives. */
    uv_mutex_destroy(&pool->drain_mutex);
    uv_cond_destroy(&pool->drain_cond);

    /* 7. Remove from registry (no-op if already removed by destroy_all). */
    registry_remove(pool);

    /* 8. Free workers array and pool itself. */
    free(pool->workers);
    free(pool);
}

/* -------------------------------------------------------------------------
 * goc_pool_destroy_timeout
 * ---------------------------------------------------------------------- */

goc_drain_result_t goc_pool_destroy_timeout(goc_pool* pool, uint64_t ms) {
    pool_abort_if_called_from_worker(pool, "goc_pool_destroy_timeout");

    /* Build a relative deadline in nanoseconds. */
    uint64_t deadline = uv_hrtime() + (uint64_t)ms * 1000000ULL;

    uv_mutex_lock(&pool->drain_mutex);
    int timed_out = 0;
    while (pool->live_count > 0 && !timed_out) {
        uint64_t now = uv_hrtime();
        if (now >= deadline) {
            timed_out = 1;
            break;
        }
        int rc = uv_cond_timedwait(&pool->drain_cond,
                                   &pool->drain_mutex,
                                   deadline - now);
        if (rc == UV_ETIMEDOUT) {
            timed_out = 1;
        }
    }
    uv_mutex_unlock(&pool->drain_mutex);

    if (timed_out && pool->live_count > 0) {
        /* Pool stays valid and running — do not tear it down. */
        return GOC_DRAIN_TIMEOUT;
    }

    /* Drain completed within the deadline; perform full shutdown. */
    atomic_store_explicit(&pool->shutdown, 1, memory_order_release);

    for (size_t i = 0; i < pool->thread_count; i++) {
        uv_sem_post(&pool->workers[i].idle_sem);
    }

    for (size_t i = 0; i < pool->thread_count; i++) {
        goc_thread_join(&pool->workers[i].thread);
    }

    for (size_t i = 0; i < pool->thread_count; i++) {
        uv_sem_destroy(&pool->workers[i].idle_sem);
        wsdq_destroy(&pool->workers[i].deque);
        injector_destroy(&pool->workers[i].injector);
    }

    uv_mutex_destroy(&pool->drain_mutex);
    uv_cond_destroy(&pool->drain_cond);

    registry_remove(pool);

    free(pool->workers);
    free(pool);

    return GOC_DRAIN_OK;
}

/* -------------------------------------------------------------------------
 * goc_pool_get_steal_stats — aggregate steal counters across all pools/workers
 * ---------------------------------------------------------------------- */
void goc_pool_get_steal_stats(uint64_t *attempts, uint64_t *successes)
{
    *attempts  = atomic_load_explicit(&g_steal_attempts,  memory_order_relaxed);
    *successes = atomic_load_explicit(&g_steal_successes, memory_order_relaxed);
}
