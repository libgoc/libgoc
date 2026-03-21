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
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <uv.h>
#include <gc.h>
#include "../include/goc.h"
#include "minicoro.h"
#include "internal.h"
#include "wsdq.h"

/* -------------------------------------------------------------------------
 * goc_worker — per-worker state
 * ---------------------------------------------------------------------- */

typedef struct {
    pthread_t      thread;
    goc_wsdq    deque;
    goc_injector   injector;   /* MPSC queue: external callers push here */
    uv_sem_t       idle_sem;
    size_t         index;
    goc_pool*      pool;
} goc_worker;

/* -------------------------------------------------------------------------
 * goc_pool — full definition (opaque outside pool.c)
 * ---------------------------------------------------------------------- */

struct goc_pool {
    goc_worker*        workers;
    size_t             thread_count;
    _Atomic size_t     idle_count;
    _Atomic size_t     next_push_idx;
    _Atomic int        shutdown;
    pthread_mutex_t    drain_mutex;
    pthread_cond_t     drain_cond;
    size_t             active_count; /* fibers currently queued or executing */
    size_t             live_count;   /* fibers still alive (drain signal) */
};

/* -------------------------------------------------------------------------
 * _Thread_local worker pointer
 *
 * Set at the top of pool_worker_fn; cleared on exit.  Used by
 * post_to_run_queue to detect internal callers (a fiber running on a pool
 * thread) versus external callers (main thread, libuv loop, other pool).
 * See pr.md §Note: _Thread_local and minicoro for why this is correct even
 * when minicoro switches stacks.
 * ---------------------------------------------------------------------- */

static _Thread_local goc_worker* tl_worker = NULL;

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
 * pool_worker_fn — thread entry point (work-stealing loop)
 * ---------------------------------------------------------------------- */

static void* pool_worker_fn(void* arg) {
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
                entry = wsdq_steal_top(&pool->workers[victim].deque);
                if (entry != NULL) goto run;
            }
        }

        /* 4. No work found anywhere — go idle.
         *
         * Increment idle_count with seq_cst BEFORE sleeping so that a
         * concurrent post_to_run_queue can observe it after its own seq_cst
         * operation.  Then double-check own injector and deque to close the
         * sleep-miss race window (see pr.md §Correctness Notes). */
        atomic_fetch_add_explicit(&pool->idle_count, 1, memory_order_seq_cst);

        entry = injector_pop(&tl_worker->injector);
        if (entry == NULL)
            entry = wsdq_pop_bottom(&tl_worker->deque);
        if (entry != NULL) {
            atomic_fetch_sub_explicit(&pool->idle_count, 1, memory_order_relaxed);
            goto run;
        }

        uv_sem_wait(&tl_worker->idle_sem);
        atomic_fetch_sub_explicit(&pool->idle_count, 1, memory_order_relaxed);
        continue;   /* re-check shutdown and try again */

run:
        /* --- from here, identical to old pool_worker_fn --- */

        /* Canary check — abort on stack overflow before corrupting anything. */
        goc_stack_canary_check(entry->stack_canary_ptr);

        /* Save coro handle before resuming: if this is a parking entry
         * (stack-allocated inside goc_take on the fiber's own stack), the
         * fiber may run to completion during mco_resume — clobbering the
         * memory where `entry` lives before control returns here.  The
         * mco_coro object itself is minicoro heap-allocated and remains valid
         * until mco_destroy, so `coro` is safe to dereference after resume. */
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

        /* If the fiber just parked, release the yield-gate so that any
         * wake() spinning on parked==0 can proceed. */
        goc_entry* fe = (goc_entry*)mco_get_user_data(coro);
        if (fe != NULL)
            atomic_store_explicit(&fe->parked, 1, memory_order_release);

        /* Update cached fiber SP so the next GC cycle scans only the used
         * portion of the stack instead of the full vmem allocation. */
        if (mco_status(coro) == MCO_SUSPENDED && fe != NULL)
            goc_fiber_root_update_sp(fe->fiber_root_handle, coro);

        pthread_mutex_lock(&pool->drain_mutex);
        pool->active_count--;
        pthread_mutex_unlock(&pool->drain_mutex);

        if (mco_status(coro) == MCO_DEAD) {
            if (fe != NULL)
                goc_fiber_root_unregister(fe->fiber_root_handle);
            mco_destroy(coro);

            pthread_mutex_lock(&pool->drain_mutex);
            pool->live_count--;
            pthread_cond_broadcast(&pool->drain_cond);
            pthread_mutex_unlock(&pool->drain_mutex);
        }
    }

    tl_worker = NULL;
    return NULL;
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
 * increment in pool_worker_fn.  See pr.md §Correctness Notes.
 * ---------------------------------------------------------------------- */

void post_to_run_queue(goc_pool* pool, goc_entry* entry) {
    pthread_mutex_lock(&pool->drain_mutex);
    pool->active_count++;
    pthread_mutex_unlock(&pool->drain_mutex);

    goc_worker* w = tl_worker;
    if (w != NULL && w->pool == pool) {
        /* Internal caller: push to executing worker's own deque.
         * Safe: the owner is inside mco_resume, not touching the deque. */
        wsdq_push_bottom(&w->deque, entry);

        /* Explicit seq_cst fence: the bottom store in wsdq_push_bottom is
         * only release; we need seq_cst here so idle_count reads below form a
         * total order with the worker's seq_cst increment (ARM/POWER safety). */
        atomic_thread_fence(memory_order_seq_cst);

        /* Wake one idle worker (not self — self is running a fiber) so it
         * can steal the just-pushed entry. */
        if (atomic_load_explicit(&pool->idle_count, memory_order_seq_cst) > 0
                && pool->thread_count > 1) {
            size_t idx = (w->index + 1) % pool->thread_count;
            uv_sem_post(&pool->workers[idx].idle_sem);
        }
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
 * pool_fiber_born — increment live_count exactly once per new fiber
 *
 * Called from goc_go_on (fiber.c) before post_to_run_queue, so that
 * live_count tracks the number of fibers alive on the pool rather than
 * the number of scheduler events.  This is the only correct drain signal:
 * a parked fiber must still count as live even while active_count is zero.
 * ---------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * pool_fiber_born — Increment live fiber count for drain coordination
 *
 * Called when a fiber is spawned to track outstanding work for pool draining.
 * Must be paired with pool_fiber_died when the fiber completes.
 * ---------------------------------------------------------------------- */

void pool_fiber_born(goc_pool* pool) {
    pthread_mutex_lock(&pool->drain_mutex);
    pool->live_count++;
    pthread_mutex_unlock(&pool->drain_mutex);
}

/* -------------------------------------------------------------------------
 * pool_abort_if_called_from_worker
 *
 * Destroying a pool from one of its own worker threads is invalid: the
 * destroy path waits on drain/join and would attempt to join the caller
 * thread itself. Detect this explicitly and abort with a diagnostic.
 * ---------------------------------------------------------------------- */

static void pool_abort_if_called_from_worker(goc_pool* pool, const char* api_name) {
    pthread_t self = pthread_self();
    for (size_t i = 0; i < pool->thread_count; i++) {
        if (pthread_equal(self, pool->workers[i].thread)) {
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
    pool->workers      = malloc(threads * sizeof(goc_worker));

    for (size_t i = 0; i < threads; i++) {
        wsdq_init(&pool->workers[i].deque, 256);
        injector_init(&pool->workers[i].injector);
        uv_sem_init(&pool->workers[i].idle_sem, 0);
        pool->workers[i].index = i;
        pool->workers[i].pool  = pool;
    }

    atomic_store(&pool->idle_count,    0);
    atomic_store(&pool->next_push_idx, 0);
    atomic_store(&pool->shutdown,      0);

    pthread_mutex_init(&pool->drain_mutex, NULL);
    pthread_cond_init(&pool->drain_cond, NULL);

    pool->active_count = 0;
    pool->live_count   = 0;

    for (size_t i = 0; i < threads; i++) {
        gc_pthread_create(&pool->workers[i].thread, NULL,
                          pool_worker_fn, &pool->workers[i]);
    }

    registry_add(pool);

    return pool;
}

/* -------------------------------------------------------------------------
 * goc_pool_destroy
 * ---------------------------------------------------------------------- */

void goc_pool_destroy(goc_pool* pool) {
    pool_abort_if_called_from_worker(pool, "goc_pool_destroy");

    /* 1. Wait for all live fibers to exit (live_count reaches zero). */
    pthread_mutex_lock(&pool->drain_mutex);
    while (pool->live_count > 0) {
        pthread_cond_wait(&pool->drain_cond, &pool->drain_mutex);
    }
    pthread_mutex_unlock(&pool->drain_mutex);

    /* 2. Signal workers to exit. */
    atomic_store_explicit(&pool->shutdown, 1, memory_order_release);

    /* 3. Unblock all waiting workers (one post per worker). */
    for (size_t i = 0; i < pool->thread_count; i++) {
        uv_sem_post(&pool->workers[i].idle_sem);
    }

    /* 4. Reap worker threads. */
    for (size_t i = 0; i < pool->thread_count; i++) {
        gc_pthread_join(pool->workers[i].thread, NULL);
    }

    /* 5. Destroy per-worker resources. */
    for (size_t i = 0; i < pool->thread_count; i++) {
        uv_sem_destroy(&pool->workers[i].idle_sem);
        wsdq_destroy(&pool->workers[i].deque);
        injector_destroy(&pool->workers[i].injector);
    }

    /* 6. Destroy drain primitives. */
    pthread_mutex_destroy(&pool->drain_mutex);
    pthread_cond_destroy(&pool->drain_cond);

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

    /* Build an absolute deadline. */
    struct timespec deadline;
    timespec_get(&deadline, TIME_UTC);
    deadline.tv_sec  += (time_t)(ms / 1000);
    deadline.tv_nsec += (long)((ms % 1000) * 1000000L);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec  += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&pool->drain_mutex);
    int timed_out = 0;
    while (pool->live_count > 0 && !timed_out) {
        int rc = pthread_cond_timedwait(&pool->drain_cond,
                                        &pool->drain_mutex,
                                        &deadline);
        if (rc == ETIMEDOUT) {
            timed_out = 1;
        }
    }
    pthread_mutex_unlock(&pool->drain_mutex);

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
#ifndef _WIN32
        GC_pthread_join(pool->workers[i].thread, NULL);
#else
        pthread_join(pool->workers[i].thread, NULL);
#endif
    }

    for (size_t i = 0; i < pool->thread_count; i++) {
        uv_sem_destroy(&pool->workers[i].idle_sem);
        wsdq_destroy(&pool->workers[i].deque);
        injector_destroy(&pool->workers[i].injector);
    }

    pthread_mutex_destroy(&pool->drain_mutex);
    pthread_cond_destroy(&pool->drain_cond);

    registry_remove(pool);

    free(pool->workers);
    free(pool);

    return GOC_DRAIN_OK;
}
