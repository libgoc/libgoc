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
#include <unistd.h>
#include <assert.h>
#include <uv.h>
#include <gc.h>
#include "../include/goc.h"
#include "../include/goc_stats.h"
#include "minicoro.h"
#include "internal.h"
#include "channel_internal.h"
#include "wsdq.h"

/* -------------------------------------------------------------------------
 * goc_worker — per-worker state
 * ---------------------------------------------------------------------- */

/* MPSC task node: pushed locklessly from any thread, drained by the owner. */
typedef struct goc_worker_task {
    void (*fn)(void*);
    void*                    arg;
    struct goc_worker_task*  next;
} goc_worker_task_t;

typedef struct {
    uv_thread_t      thread;
    goc_wsdq         deque;
    goc_injector     injector;        /* MPSC queue: external callers push here */
    uv_loop_t        loop;
    uv_async_t*      wakeup;          /* interrupts UV_RUN_ONCE when work is posted */
    size_t           index;
    goc_pool*        pool;
    _Atomic int      closing;         /* set before the worker closes its loop */
    _Atomic int      pending_handle_tasks; /* in-flight cross-worker handle ops */
    _Atomic uint64_t steal_attempts;  /* relaxed counter; read at STOPPED event */
    _Atomic uint64_t steal_successes;
    _Atomic uint64_t steal_misses;    /* wsdq_steal_top returned NULL */
    _Atomic uint64_t idle_wakeups;    /* worker woke from loop wait cycle */
    size_t           last_steal_victim; /* victim hint for next steal, SIZE_MAX = unset */
    uint32_t         miss_streak;       /* consecutive steal misses; resets on any successful dequeue */
    _Atomic size_t   active_io_handles; /* app I/O handles live on this worker's loop */
    /* Per-worker I/O task queue: cross-thread handle ops post tasks here.
     * Push end is lockless (CAS); drain end is owner-only. */
    _Atomic(goc_worker_task_t*) task_queue;
} goc_worker;

/* Global lifetime steal totals across all pools/workers — read via accessor */
static _Atomic uint64_t g_steal_attempts  = 0;
static _Atomic uint64_t g_steal_successes = 0;
static _Atomic uint64_t g_steal_misses    = 0;
static _Atomic uint64_t g_idle_wakeups    = 0;
static _Atomic int      g_pool_id_counter = 0;

/* -------------------------------------------------------------------------
 * goc_pool — full definition (opaque outside pool.c)
 * ---------------------------------------------------------------------- */

struct goc_pool {
    int                id;
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

/* Drain all tasks queued on the worker's MPSC task queue.
 * Must be called from the worker's own thread (owner-side drain). */
static void drain_worker_tasks(goc_worker* w) {
    /* Atomically take the whole list (LIFO order from push). */
    goc_worker_task_t* list = atomic_exchange_explicit(
        &w->task_queue, NULL, memory_order_acq_rel);
    size_t task_count = 0;
    /* Reverse to FIFO order. */
    goc_worker_task_t* prev = NULL;
    while (list) {
        goc_worker_task_t* next = list->next;
        list->next = prev;
        prev = list;
        list = next;
        task_count++;
    }
    GOC_DBG(
            "drain_worker_tasks: worker=%d loop=%p drained=%zu tasks\n",
            goc_current_worker_id(), (void*)&w->loop, task_count);
    /* Execute in FIFO order. */
    goc_worker_task_t* t = prev;
    while (t) {
        goc_worker_task_t* next = t->next;
        GOC_DBG(
                "drain_worker_tasks: executing task fn=%p arg=%p\n",
                (void*)t->fn, t->arg);
        t->fn(t->arg);
        free(t);
        atomic_fetch_sub_explicit(&w->pending_handle_tasks, 1,
                                  memory_order_acq_rel);
        t = next;
    }
}

void goc_worker_io_handle_opened(void) {
    if (tl_worker) {
        atomic_fetch_add_explicit(&tl_worker->active_io_handles, 1,
                                  memory_order_relaxed);
    }
}

void goc_worker_io_handle_closed(void) {
    if (tl_worker) {
        size_t before = atomic_load_explicit(&tl_worker->active_io_handles,
                                            memory_order_relaxed);
        assert(before > 0 && "goc_worker_io_handle_closed called with no active IO handles");
        atomic_fetch_sub_explicit(&tl_worker->active_io_handles, 1,
                                  memory_order_relaxed);
    }
}

static void pool_worker_close_handle_cb(uv_handle_t* h) {
    gc_handle_unregister(h);
}

static void pool_worker_close_remaining_handle_cb(uv_handle_t* h, void* arg) {
    (void)arg;
    if (uv_is_closing(h))
        return;
    GOC_DBG("pool_worker_close_remaining_handle_cb: closing remaining handle=%p type=%s active=%d has_ref=%d loop=%p data=%p\n",
            (void*)h,
            uv_handle_type_name(uv_handle_get_type(h)),
            uv_is_active(h),
            uv_has_ref(h),
            (void*)h->loop,
            h->data);
    goc_uv_close_log("pool_worker_close_handle_cb", h);
    goc_uv_close_internal(h, pool_worker_close_handle_cb);
}

static void pool_walk_handle_cb(uv_handle_t* handle, void* arg) {
    size_t* count = (size_t*)arg;
    (*count)++;
    GOC_DBG("pool_walk_handle_cb: handle=%p type=%s active=%d has_ref=%d closing=%d loop=%p data=%p\n",
            (void*)handle,
            uv_handle_type_name(uv_handle_get_type(handle)),
            uv_is_active(handle),
            uv_has_ref(handle),
            uv_is_closing(handle),
            (void*)handle->loop,
            handle->data);
}

static void pool_log_worker_loop_state(goc_worker* w, const char* context) {
    size_t count = 0;
    uv_walk(&w->loop, pool_walk_handle_cb, &count);
    GOC_DBG("pool_log_worker_loop_state: %s worker=%zu loop=%p alive=%d closing=%d handle_count=%zu task_queue=%p shutdown=%d\n",
            context,
            w->index,
            (void*)&w->loop,
            uv_loop_alive(&w->loop),
            atomic_load_explicit(&w->closing, memory_order_relaxed),
            count,
            (void*)atomic_load_explicit(&w->task_queue, memory_order_acquire),
            atomic_load_explicit(&w->pool->shutdown, memory_order_relaxed));
}

static void worker_wakeup_cb(uv_async_t* h) {
    (void)h;
    GOC_DBG(
            "worker_wakeup_cb: entered target_worker=%zu current_worker=%d wakeup=%p\n",
            tl_worker ? tl_worker->index : SIZE_MAX,
            goc_current_worker_id(),
            (void*)h);
    if (tl_worker) {
        drain_worker_tasks(tl_worker);
    }
    GOC_DBG(
            "worker_wakeup_cb: exit target_worker=%zu current_worker=%d\n",
            tl_worker ? tl_worker->index : SIZE_MAX,
            goc_current_worker_id());
}

uv_loop_t* goc_current_worker_loop(void) {
    return tl_worker ? &tl_worker->loop : NULL;
}

int goc_current_worker_id(void) {
    return tl_worker ? (int)tl_worker->index : -1;
}

bool goc_current_worker_has_pending_tasks(void) {
    return tl_worker &&
           atomic_load_explicit(&tl_worker->task_queue, memory_order_acquire) != NULL;
}

size_t goc_pool_thread_count(goc_pool* pool) {
    return pool ? pool->thread_count : 0;
}

/* Wake exactly one sleeping worker (optionally excluding one index). */
/* -------------------------------------------------------------------------
 * Pool registry (file-scope; owned entirely by pool.c)
 * ---------------------------------------------------------------------- */

static goc_pool**  g_pool_registry     = NULL;
static size_t      g_pool_registry_len = 0;
static size_t      g_pool_registry_cap = 0;
static uv_mutex_t  g_pool_registry_mutex;

/* Post fn(arg) to the worker that owns the given loop.
 * Safe to call from any thread.  Aborts if loop is not a known worker loop. */
static int worker_try_wakeup(goc_worker* w) {
    if (!w || !w->wakeup) {
        GOC_DBG("worker_try_wakeup: invalid worker or missing wakeup w=%p wakeup=%p\n",
                (void*)w, w ? (void*)w->wakeup : NULL);
        return UV_ECANCELED;
    }
    if (atomic_load_explicit(&w->closing, memory_order_acquire)) {
        GOC_DBG("worker_try_wakeup: worker closing w=%p\n", (void*)w);
        return UV_ECANCELED;
    }
    if (uv_is_closing((uv_handle_t*)w->wakeup)) {
        GOC_DBG("worker_try_wakeup: wakeup closing w=%p wakeup=%p\n",
                (void*)w, (void*)w->wakeup);
        return UV_ECANCELED;
    }

    int rc = uv_async_send(w->wakeup);
    if (rc == UV_EINTR) {
        rc = uv_async_send(w->wakeup);
    }
    if (rc == UV_EBADF || rc == UV_EINVAL) {
        GOC_DBG("worker_try_wakeup: wakeup send race; closing/invalid wakeup w=%p wakeup=%p rc=%d\n",
                (void*)w, (void*)w->wakeup, rc);
        return UV_ECANCELED;
    }
    return rc;
}

int post_on_handle_loop(uv_loop_t* loop, void (*fn)(void*), void* arg) {
    GOC_DBG(
            "post_on_handle_loop: entry loop=%p fn=%p arg=%p\n",
            (void*)loop, (void*)fn, arg);
    /* O(1) lookup: loop.data is set to the owning goc_worker* at pool_make time. */
    goc_worker* w = (goc_worker*)loop->data;
    if (!w) {
        ABORT("post_on_handle_loop: loop %p has no associated worker (loop.data==NULL)\n",
              (void*)loop);
    }
    int loop_alive = uv_loop_alive(loop);
    GOC_DBG(
            "post_on_handle_loop: found worker=%zu pool=%p loop=%p wakeup=%p fn=%p arg=%p shutdown=%d alive=%d\n",
            w->index, (void*)w->pool, (void*)loop, (void*)w->wakeup, (void*)fn,
            arg, atomic_load_explicit(&w->pool->shutdown, memory_order_relaxed),
            loop_alive);
    if (atomic_load_explicit(&w->pool->shutdown, memory_order_acquire)) {
        GOC_DBG(
                "post_on_handle_loop: rejected loop=%p fn=%p arg=%p pool_shutdown=%d\n",
                (void*)loop, (void*)fn, arg,
                atomic_load_explicit(&w->pool->shutdown, memory_order_relaxed));
        return UV_ECANCELED;
    }
    if (atomic_load_explicit(&w->closing, memory_order_acquire)) {
        GOC_DBG(
                "post_on_handle_loop: rejected loop=%p fn=%p arg=%p worker_closing=1\n",
                (void*)loop, (void*)fn, arg);
        return UV_ECANCELED;
    }
    atomic_fetch_add_explicit(&w->pending_handle_tasks, 1, memory_order_acq_rel);
    /* Lockless push onto the MPSC task queue. */
    goc_worker_task_t* task =
        (goc_worker_task_t*)malloc(sizeof(goc_worker_task_t));
    task->fn  = fn;
    task->arg = arg;
    goc_worker_task_t* old;
    do {
        old = atomic_load_explicit(&w->task_queue, memory_order_relaxed);
        task->next = old;
    } while (!atomic_compare_exchange_weak_explicit(
                 &w->task_queue, &old, task,
                 memory_order_release, memory_order_relaxed));
    GOC_DBG(
            "post_on_handle_loop: queued task onto worker=%zu loop=%p old_head=%p\n",
            w->index, (void*)loop, (void*)old);
    int rc = worker_try_wakeup(w);
    GOC_DBG(
            "post_on_handle_loop: worker_try_wakeup worker=%zu wakeup=%p rc=%d loop_alive=%d closing=%d shutdown=%d\n",
            w->index, (void*)w->wakeup, rc, loop_alive,
            atomic_load_explicit(&w->closing, memory_order_acquire),
            atomic_load_explicit(&w->pool->shutdown, memory_order_relaxed));
    if (rc < 0) {
        GOC_DBG("post_on_handle_loop: wakeup failed for loop=%p fn=%p arg=%p rc=%d\n",
                (void*)loop, (void*)fn, arg, rc);
        free(arg);
        ABORT("post_on_handle_loop: cannot wake worker loop %p rc=%d\n",
              (void*)loop, rc);
    }
    return rc;
}

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
    uv_mutex_unlock(&g_pool_registry_mutex);



    GOC_DBG("pool_registry_destroy_all: destroying %zu pools\n", len);

    for (size_t i = 0; i < len; i++) {
        GOC_DBG("pool_registry_destroy_all: destroy[%zu/%zu] pool=%p id=%d\n",
                i + 1, len, (void*)snap[i], snap[i] ? snap[i]->id : -1);
        goc_pool_destroy(snap[i]);
    }
    GOC_DBG("pool_registry_destroy_all: all pools destroyed\n");
    free(snap);

    // Safe: all pools drained/removed, no concurrent ops
    uv_mutex_lock(&g_pool_registry_mutex);
    g_pool_registry_len = 0;
    uv_mutex_unlock(&g_pool_registry_mutex);
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

static size_t pool_pending_spawn_count_locked(const goc_pool* pool) {
    size_t n = 0;
    for (const goc_spawn_req* p = pool->pending_spawn_head; p != NULL; p = p->next)
        n++;
    return n;
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
        GOC_DBG("pool_dispatch_spawn: fn=%p entry=%p join_ch=%p home_worker_idx=%zu\n",
                (void*)(uintptr_t)reqs->fn,
                (void*)entry,
                (void*)reqs->join_ch,
                reqs->home_worker_idx);
        if (reqs->home_worker_idx != SIZE_MAX) {
            entry->home_worker_idx = reqs->home_worker_idx;
            post_to_specific_worker(pool, reqs->home_worker_idx, entry);
        } else {
            post_to_run_queue(pool, entry);
        }
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
        GOC_DBG("pool_submit_spawn: fn=%p bypass=%d resident=%zu live=%zu (fast path)\n",
                (void*)(uintptr_t)fn, (int)bypass_throttle, pool->resident_count, live);
        uv_mutex_unlock(&pool->drain_mutex);

        goc_entry* entry = goc_fiber_entry_create(pool, fn, arg, join_ch);
        GOC_DBG("pool_spawn_fast: fn=%p entry=%p join_ch=%p pool=%p live=%zu resident=%zu\n",
                (void*)(uintptr_t)fn, (void*)entry, (void*)join_ch,
                (void*)pool, live, pool->resident_count);
        post_to_run_queue(pool, entry);
        return;
    }

    GOC_DBG("pool_submit_spawn: fn=%p bypass=%d DEFERRED (cap reached or queue non-empty)\n",
            (void*)(uintptr_t)fn, (int)bypass_throttle);
    GOC_DBG("pool_spawn_deferred: fn=%p pool=%p live=%zu resident=%zu bypass=%d\n",
            (void*)(uintptr_t)fn, (void*)pool, pool->live_count,
            pool->resident_count, (int)bypass_throttle);
    goc_spawn_req* req = (goc_spawn_req*)GC_malloc_uncollectable(sizeof(goc_spawn_req));
    if (req == NULL) {
        uv_mutex_unlock(&pool->drain_mutex);
        ABORT("failed to allocate deferred spawn request\n");
    }

    req->fn              = fn;
    req->fn_arg          = arg;
    req->join_ch         = join_ch;
    req->home_worker_idx = SIZE_MAX;
    req->next            = NULL;
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
    tl_worker->last_steal_victim = SIZE_MAX;
    tl_worker->miss_streak = 0;

    /* Per-worker PRNG seed for randomised steal order (anti-thundering-herd).
     * Combines index with the worker pointer for uniqueness. */
    uint32_t seed = (uint32_t)(tl_worker->index ^ (uintptr_t)tl_worker);

    goc_entry* entry;
    const char* source = "unknown";

    while (!atomic_load_explicit(&pool->shutdown, memory_order_acquire)) {

        /* 1. Pop from own deque (LIFO, cache-warm). */
        entry = wsdq_pop_bottom(&tl_worker->deque);
        GOC_DBG("pool_worker_fn: worker=%zu try pop own deque entry=%p deque_size_approx=%zu\n",
                tl_worker->index, (void*)entry,
                wsdq_approx_size(&tl_worker->deque));
        if (entry != NULL) {
            source = "own";
            tl_worker->miss_streak = 0;
            GOC_DBG("pool_worker_fn: worker=%zu SCHED_PICK source=own entry=%p fn=%p coro=%p home_worker_idx=%zu\n",
                    tl_worker->index,
                    (void*)entry,
                    (void*)(uintptr_t)entry->fn,
                    (void*)entry->coro,
                    entry->home_worker_idx);
            goto run;
        }

        /* 2. Drain own injector (entries posted by external callers). */
        entry = injector_pop(&tl_worker->injector);
        GOC_DBG("pool_worker_fn: worker=%zu try pop injector entry=%p\n",
                tl_worker->index, (void*)entry);
        if (entry != NULL) {
            source = "injector";
            tl_worker->miss_streak = 0;
            GOC_DBG("pool_worker_fn: worker=%zu SCHED_PICK source=injector entry=%p fn=%p coro=%p home_worker_idx=%zu\n",
                    tl_worker->index,
                    (void*)entry,
                    (void*)(uintptr_t)entry->fn,
                    (void*)entry->coro,
                    entry->home_worker_idx);
            GOC_DBG("pool_worker_fn: worker=%zu picked injector entry=%p fn=%p coro=%p home_worker_idx=%zu\n",
                    tl_worker->index,
                    (void*)entry,
                    (void*)(uintptr_t)entry->fn,
                    (void*)entry->coro,
                    entry->home_worker_idx);
            goto run;
        }

        /* 3. Steal phase: victim hinting, then randomized scan.
         *
         * Skip entirely when miss_streak has reached STEAL_BACKOFF_THRESHOLD —
         * this suppresses hot spinning on IO-bound workloads (e.g. HTTP) where
         * runnable fibers are woken via uv_async and never appear on a deque at
         * probe time, producing 90–95% miss rates. The worker falls through to
         * the idle path and parks until posted by post_to_run_queue. */
        if (pool->thread_count > 1 &&
            tl_worker->miss_streak < STEAL_BACKOFF_THRESHOLD) {
            size_t victim_hint = tl_worker->last_steal_victim;
            if (victim_hint != SIZE_MAX && victim_hint != tl_worker->index) {
#ifdef GOC_ENABLE_STATS
                atomic_fetch_add_explicit(&tl_worker->steal_attempts, 1, memory_order_seq_cst);
                atomic_fetch_add_explicit(&g_steal_attempts,           1, memory_order_seq_cst);
#endif
                entry = wsdq_steal_top(&pool->workers[victim_hint].deque);
                if (entry != NULL) {
#ifdef GOC_ENABLE_STATS
                    atomic_fetch_add_explicit(&tl_worker->steal_successes, 1, memory_order_seq_cst);
                    atomic_fetch_add_explicit(&g_steal_successes,           1, memory_order_seq_cst);
#endif
                    source = "steal-hint";
                    tl_worker->last_steal_victim = victim_hint;
                    tl_worker->miss_streak = 0;
                    GOC_DBG("pool_worker_fn: worker=%zu SCHED_PICK source=steal-hint entry=%p fn=%p coro=%p victim=%zu miss_streak=%zu\n",
                            tl_worker->index, (void*)entry,
                            (void*)(uintptr_t)entry->fn,
                            (void*)entry->coro,
                            victim_hint,
                            (size_t)tl_worker->miss_streak);
                    GOC_DBG("pool_worker_fn: worker=%zu steal success victim=%zu entry=%p miss_streak=%zu\n",
                            tl_worker->index, victim_hint, (void*)entry,
                            (size_t)tl_worker->miss_streak);
                    goto run;
                }
#ifdef GOC_ENABLE_STATS
                atomic_fetch_add_explicit(&tl_worker->steal_misses, 1, memory_order_seq_cst);
                atomic_fetch_add_explicit(&g_steal_misses,           1, memory_order_seq_cst);
#endif
                tl_worker->miss_streak++;
                GOC_DBG("pool_worker_fn: worker=%zu steal hint miss victim=%zu miss_streak=%zu\n",
                        tl_worker->index, victim_hint, (size_t)tl_worker->miss_streak);
            }
            /* Fallback: randomized scan as before */
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            size_t offset = 1 + (size_t)(seed % (uint32_t)(pool->thread_count - 1));
            for (size_t i = 0; i < pool->thread_count; i++) {
                size_t victim = (tl_worker->index + offset + i) % pool->thread_count;
                if (victim == tl_worker->index) continue;
#ifdef GOC_ENABLE_STATS
                atomic_fetch_add_explicit(&tl_worker->steal_attempts, 1, memory_order_seq_cst);
                atomic_fetch_add_explicit(&g_steal_attempts,           1, memory_order_seq_cst);
#endif
                entry = wsdq_steal_top(&pool->workers[victim].deque);
                if (entry != NULL) {
#ifdef GOC_ENABLE_STATS
                    atomic_fetch_add_explicit(&tl_worker->steal_successes, 1, memory_order_seq_cst);
                    atomic_fetch_add_explicit(&g_steal_successes,           1, memory_order_seq_cst);
#endif
                    source = "steal";
                    tl_worker->last_steal_victim = victim;
                    tl_worker->miss_streak = 0;
                    GOC_DBG("pool_worker_fn: worker=%zu SCHED_PICK source=steal entry=%p fn=%p coro=%p victim=%zu miss_streak=%zu\n",
                            tl_worker->index, (void*)entry,
                            (void*)(uintptr_t)entry->fn,
                            (void*)entry->coro,
                            victim,
                            (size_t)tl_worker->miss_streak);
                    GOC_DBG("pool_worker_fn: worker=%zu steal success victim=%zu entry=%p miss_streak=%zu\n",
                            tl_worker->index, victim, (void*)entry,
                            (size_t)tl_worker->miss_streak);
                    goto run;
                }
#ifdef GOC_ENABLE_STATS
                atomic_fetch_add_explicit(&tl_worker->steal_misses, 1, memory_order_seq_cst);
                atomic_fetch_add_explicit(&g_steal_misses,           1, memory_order_seq_cst);
#endif
                tl_worker->miss_streak++;
                GOC_DBG("pool_worker_fn: worker=%zu steal miss victim=%zu miss_streak=%zu\n",
                        tl_worker->index, victim, (size_t)tl_worker->miss_streak);
            }
            /* If all fail, clear the hint for next round */
            tl_worker->last_steal_victim = SIZE_MAX;
        }

        /* 4. No work found anywhere — go idle.
         *
         * Before parking, if there is still work in our own deque and any workers are idle,
         * wake one to encourage stealing. This preserves locality for ping-pong workloads,
         * but avoids leaving work stranded if we're about to park.
         * Skip entirely at pool=1 — thread_count is immutable so this branch is free. 
         */
        if (pool->thread_count > 1 &&
            !atomic_load_explicit(&pool->shutdown, memory_order_acquire)) {
            size_t depth = wsdq_approx_size(&tl_worker->deque);
            size_t idle = atomic_load_explicit(&pool->idle_count, memory_order_seq_cst);
            if (depth > 0 && idle > 0) {
                size_t idx = (tl_worker->index + 1) % pool->thread_count;
                if (pool->workers[idx].wakeup) {
                    int rc = worker_try_wakeup(&pool->workers[idx]);
                    GOC_DBG("pool_worker_fn: neighbor wakeup worker=%zu rc=%d idle_count=%zu deque_depth=%zu\n",
                            idx, rc, idle, depth);
                }
            }
        }

        GOC_DBG("pool_worker_fn: worker=%zu idle enter idle_count=%zu deque_size_approx=%zu\n",
                tl_worker->index,
                atomic_load_explicit(&pool->idle_count, memory_order_seq_cst),
                wsdq_approx_size(&tl_worker->deque));
        atomic_fetch_add_explicit(&pool->idle_count, 1, memory_order_seq_cst);

        entry = wsdq_pop_bottom(&tl_worker->deque);
        if (entry == NULL) {
            entry = injector_pop(&tl_worker->injector);
        }
        if (entry != NULL) {
            atomic_fetch_sub_explicit(&pool->idle_count, 1, memory_order_relaxed);
            GOC_DBG("pool_worker_fn: worker=%zu idle found work before uv_run entry=%p source=recheck\n",
                    tl_worker->index, (void*)entry);
            goto run;
        }

        GOC_STATS_WORKER_STATUS((int)tl_worker->index, pool->id, GOC_WORKER_IDLE, (int)pool->live_count, 0, 0);
        GOC_DBG(
                "pool_worker_fn: worker=%zu idle uv_run(ONCE) start loop=%p idle_count=%zu\n",
                tl_worker->index, (void*)&tl_worker->loop,
                atomic_load_explicit(&pool->idle_count, memory_order_seq_cst));
        uv_run(&tl_worker->loop, UV_RUN_ONCE);
        goc_deferred_handle_unreg_flush();
        goc_deferred_close_flush();
        GOC_DBG(
                "pool_worker_fn: worker=%zu idle uv_run(ONCE) end loop=%p\n",
                tl_worker->index, (void*)&tl_worker->loop);
#ifdef GOC_ENABLE_STATS
        atomic_fetch_add_explicit(&tl_worker->idle_wakeups, 1, memory_order_seq_cst);
        atomic_fetch_add_explicit(&g_idle_wakeups,           1, memory_order_seq_cst);
#endif
        atomic_fetch_sub_explicit(&pool->idle_count, 1, memory_order_relaxed);
        GOC_DBG("pool_worker_fn: worker=%zu idle exit idle_count=%zu\n",
                tl_worker->index,
                atomic_load_explicit(&pool->idle_count, memory_order_seq_cst));
        tl_worker->miss_streak = 0;  /* fresh steal budget after wakeup */
        continue;   /* re-check shutdown and try again */

run:
        /* --- from here, identical to old pool_worker_fn --- */
        GOC_STATS_WORKER_STATUS((int)tl_worker->index, pool->id, GOC_WORKER_RUNNING, (int)pool->live_count, 0, 0);
        GOC_DBG("pool_worker_fn: WORKER_ENTRY_RUN worker=%zu source=%s entry=%p fn=%p coro=%p home_worker=%zu ok=%d parked=%llu\n",
                tl_worker->index,
                source,
                (void*)entry,
                (void*)(uintptr_t)entry->fn,
                (void*)entry->coro,
                entry->home_worker_idx,
                (int)entry->ok,
                (unsigned long long)atomic_load_explicit(&entry->parked, memory_order_acquire));
        GOC_DBG(
                "pool_worker_fn: worker=%zu RUNNING fn=%p entry=%p source=%s\n",
                tl_worker->index, (void*)(uintptr_t)entry->fn, (void*)entry,
                source);

        /* Canary check — abort on stack overflow before corrupting anything. */
        goc_stack_canary_check(entry->stack_canary_ptr);

        /* Save coro handle before resuming: another worker can race and
         * advance the coroutine lifecycle while we are in mco_resume. Keep a
         * stable handle (`coro`) across the call; the mco_coro object remains
         * valid until mco_destroy. */
        mco_coro* coro = entry->coro;

        GOC_DBG("pool_worker_fn: SCHED_PRE_RESUME worker=%zu entry=%p coro=%p fn=%p source=%s pre_status=%d\n",
                tl_worker->index,
                (void*)entry,
                (void*)coro,
                (void*)(uintptr_t)entry->fn,
                source,
                coro ? (int)mco_status(coro) : -1);
        GOC_DBG(
                "pool_worker_fn: pre-resume coro=%p status=%d\n",
                (void*)coro,
                coro ? (int)mco_status(coro) : -1);
        GOC_DBG(
                "pool_worker_fn: pre-resume stack_base=%p stack_size=%zu\n",
                coro ? (void*)coro->stack_base : NULL,
                coro ? coro->stack_size : 0);

        /* Redirect GC stack scan to the fiber's stack for the duration of
         * mco_resume (see docs/DESIGN.md §GC Stack Bottom Redirect). */
        struct GC_stack_base orig_sb;
        GC_get_my_stackbottom(&orig_sb);
        struct GC_stack_base fiber_sb;
        fiber_sb.mem_base = (char*)coro->stack_base + coro->stack_size;
        GC_set_stackbottom(NULL, &fiber_sb);

        mco_resume(coro);
        GOC_DBG("pool_worker_fn: SCHED_POST_RESUME worker=%zu entry=%p coro=%p fn=%p source=%s post_status=%d\n",
                tl_worker->index,
                (void*)entry,
                (void*)coro,
                (void*)(uintptr_t)entry->fn,
                source,
                coro ? (int)mco_status(coro) : -1);


        GC_set_stackbottom(NULL, &orig_sb);

        /* Safe point: fiber has yielded, no channel locks held, no fiber
         * stack active on this thread.  Drive incremental GC collection. */
        GC_collect_a_little();

        /* Drain any I/O callbacks queued on the worker's own loop only when
         * there are active app handles or pending cross-worker tasks. */
        bool has_io    = atomic_load_explicit(&tl_worker->active_io_handles,
                                              memory_order_relaxed) > 0;
        bool has_tasks = atomic_load_explicit(&tl_worker->task_queue,
                                              memory_order_relaxed) != NULL;
        if (has_io || has_tasks) {
            GOC_DBG("pool_worker_fn: worker=%zu begin uv_run(NOWAIT) loop=%p\n",
                    tl_worker->index, (void*)&tl_worker->loop);
            uv_run(&tl_worker->loop, UV_RUN_NOWAIT);
            goc_deferred_handle_unreg_flush();
            goc_deferred_close_flush();
            GOC_DBG("pool_worker_fn: worker=%zu end uv_run(NOWAIT) loop=%p\n",
                    tl_worker->index, (void*)&tl_worker->loop);
        }
        GOC_DBG("pool_worker_fn: worker=%zu post-uv_run(NOWAIT) idle_count=%zu deque_size_approx=%zu\n",
                tl_worker->index,
                atomic_load_explicit(&pool->idle_count, memory_order_seq_cst),
                wsdq_approx_size(&tl_worker->deque));

        goc_entry* fe = (goc_entry*)mco_get_user_data(coro);
        mco_state st = mco_status(coro);
        GOC_DBG(
                "pool_worker_fn: post-resume coro=%p st=%d fn=%p\n",
                (void*)coro, (int)st, (void*)(uintptr_t)(fe ? fe->fn : NULL));

        /* Update cached fiber SP so the next GC cycle scans only the used
         * portion of the stack instead of the full vmem allocation. */

        if (st == MCO_SUSPENDED && fe != NULL)
            goc_fiber_root_update_sp(fe->fiber_root_handle, coro);

        /* Post-yield wakeup:
         * If we have more than capacity / 2 items in our deque,
         * and any workers are idle, wake a neighbor.
         */
        if (st == MCO_SUSPENDED && pool->thread_count > 1 &&
            !atomic_load_explicit(&pool->shutdown, memory_order_acquire)) {
            size_t depth = wsdq_approx_size(&tl_worker->deque);
            size_t idle = atomic_load_explicit(&pool->idle_count, memory_order_seq_cst);
            if (depth > tl_worker->deque.capacity/2 && idle > 0) {
                size_t neighbor = (tl_worker->index + 1) % pool->thread_count;
                if (neighbor != tl_worker->index) {
                    /* Prevent compiler from optimizing away the depth calculation. */
                    __asm__ volatile("" ::: "memory");
                    if (pool->workers[neighbor].wakeup) {
                        int rc = worker_try_wakeup(&pool->workers[neighbor]);
                        GOC_DBG("pool_worker_fn: neighbor wakeup worker=%zu rc=%d idle=%zu depth=%zu\n",
                                neighbor, rc, idle, depth);
                    }
                }
            }
        }

        /* If the fiber just parked, release the yield-gate so that any
         * wake() spinning on the suspend state can proceed. */
        if (fe != NULL) {
            uint64_t parked = atomic_load_explicit(&fe->parked, memory_order_acquire);
            if (parked & 1)
                atomic_store_explicit(&fe->parked, parked + 1, memory_order_release);
        }

        if (st == MCO_DEAD) {
            GOC_DBG("pool_worker_fn: POOL_LIVE_DEC start pool=%p worker=%zu fn=%p live_before=%zu resident=%zu idle=%zu\n",
                    (void*)pool, tl_worker->index, (void*)(uintptr_t)entry->fn,
                    pool->live_count, pool->resident_count,
                    atomic_load_explicit(&pool->idle_count, memory_order_relaxed));
            GOC_DBG("pool_worker_fn: worker=%zu FIBER DEAD fn=%p live_count BEFORE=%zu\n",
                    tl_worker->index, (void*)(uintptr_t)entry->fn,
                    pool->live_count);
            if (fe != NULL) {
                goc_fiber_root_unregister(fe->fiber_root_handle);
            }
            mco_destroy(coro);

            uv_mutex_lock(&pool->drain_mutex);
            if (pool->resident_count > 0)
                pool->resident_count--;
            pool->live_count--;
            GOC_DBG("pool_worker_fn: POOL_LIVE_DEC complete pool=%p worker=%zu live_before=%zu live_after=%zu resident=%zu idle=%zu\n",
                    (void*)pool, tl_worker->index, pool->live_count + 1, pool->live_count,
                    pool->resident_count, atomic_load_explicit(&pool->idle_count, memory_order_relaxed));
            goc_spawn_req* admitted = pool_collect_admitted_spawns_locked(pool);
            uv_cond_broadcast(&pool->drain_cond);
            uv_mutex_unlock(&pool->drain_mutex);

            pool_dispatch_spawn_list(pool, admitted);
        }
    }

    /* Final task drain: catch any I/O close dispatches posted to this worker
     * in the race window between the last uv_run(ONCE) and shutdown=1.
     * (Example: goc_http_server_close posts goc_io_handle_close(srv->tcp) to
     * this worker just before pool_destroy sets shutdown; if the worker exited
     * the loop without seeing the wakeup, uv_close is never called and the
     * handle stays active — causing uv_run(DEFAULT) in pool_destroy step 5 to
     * block forever.)  The worker thread is still alive here, so tl_worker and
     * the loop are valid. */
    GOC_DBG("pool_worker_fn: worker=%zu shutdown drain start loop=%p task_queue=%p alive=%d\n",
            tl_worker->index,
            (void*)&tl_worker->loop,
            (void*)atomic_load_explicit(&tl_worker->task_queue, memory_order_acquire),
            uv_loop_alive(&tl_worker->loop));
    atomic_store_explicit(&tl_worker->closing, 1, memory_order_release);
    do {
        drain_worker_tasks(tl_worker);
        GOC_DBG("pool_worker_fn: worker=%zu shutdown drain iteration loop=%p task_queue=%p alive=%d\n",
                tl_worker->index,
                (void*)&tl_worker->loop,
                (void*)atomic_load_explicit(&tl_worker->task_queue, memory_order_acquire),
                uv_loop_alive(&tl_worker->loop));
        /* Run the loop once (non-blocking) to fire any uv_close callbacks that
         * on_handle_close_dispatch just initiated (e.g. the srv->tcp close cb).
         * Repeat until no new tasks arrive during drain/running. */
        uv_run(&tl_worker->loop, UV_RUN_NOWAIT);
        GOC_DBG("pool_worker_fn: worker=%zu shutdown uv_run end loop=%p task_queue=%p alive=%d\n",
                tl_worker->index,
                (void*)&tl_worker->loop,
                (void*)atomic_load_explicit(&tl_worker->task_queue, memory_order_acquire),
                uv_loop_alive(&tl_worker->loop));
    } while (atomic_load_explicit(&tl_worker->task_queue, memory_order_acquire) != NULL);
    GOC_DBG("pool_worker_fn: worker=%zu shutdown drain complete loop=%p task_queue=%p alive=%d\n",
            tl_worker->index,
            (void*)&tl_worker->loop,
            (void*)atomic_load_explicit(&tl_worker->task_queue, memory_order_acquire),
            uv_loop_alive(&tl_worker->loop));

    pool_log_worker_loop_state(tl_worker, "shutdown drain complete");
    while (atomic_load_explicit(&tl_worker->pending_handle_tasks,
                               memory_order_acquire) > 0) {
        GOC_DBG("pool_worker_fn: worker=%zu waiting for pending handle ops=%d loop=%p\n",
                tl_worker->index,
                atomic_load_explicit(&tl_worker->pending_handle_tasks,
                                     memory_order_relaxed),
                (void*)&tl_worker->loop);
        uv_run(&tl_worker->loop, UV_RUN_NOWAIT);
    }

    /* Final worker-loop shutdown must happen on the worker's own thread. */
    if (tl_worker->wakeup && !uv_is_closing((uv_handle_t*)tl_worker->wakeup)) {
        goc_uv_close_log("pool_worker_fn closing worker wakeup", (uv_handle_t*)tl_worker->wakeup);
        GOC_DBG("pool_worker_fn: worker=%zu closing wakeup=%p active=%d closing=%d loop_alive=%d\n",
                tl_worker->index,
                (void*)tl_worker->wakeup,
                uv_is_active((uv_handle_t*)tl_worker->wakeup),
                uv_is_closing((uv_handle_t*)tl_worker->wakeup),
                uv_loop_alive(&tl_worker->loop));
        goc_uv_close_internal((uv_handle_t*)tl_worker->wakeup, NULL);
    }

    GOC_DBG("pool_worker_fn: worker=%zu final shutdown sweep start loop=%p alive=%d\n",
            tl_worker->index, (void*)&tl_worker->loop,
            uv_loop_alive(&tl_worker->loop));
    uv_walk(&tl_worker->loop, pool_worker_close_remaining_handle_cb, NULL);
    tl_worker->loop.stop_flag = 1;

    while (uv_loop_alive(&tl_worker->loop)) {
        pool_log_worker_loop_state(tl_worker, "before uv_run DEFAULT");
        size_t loop_count = 0;
        uv_walk(&tl_worker->loop, pool_walk_handle_cb, &loop_count);
        GOC_DBG("pool_worker_fn: worker=%zu shutdown uv_run DEFAULT start loop=%p alive=%d handle_count=%zu\n",
                tl_worker->index, (void*)&tl_worker->loop,
                uv_loop_alive(&tl_worker->loop), loop_count);
        int run_rc = uv_run(&tl_worker->loop, UV_RUN_DEFAULT);
        GOC_DBG("pool_worker_fn: worker=%zu shutdown uv_run DEFAULT returned rc=%d loop=%p alive=%d\n",
                tl_worker->index, run_rc, (void*)&tl_worker->loop,
                uv_loop_alive(&tl_worker->loop));
        if (run_rc <= 0 && uv_loop_alive(&tl_worker->loop)) {
            size_t alive_count = 0;
            uv_walk(&tl_worker->loop, pool_walk_handle_cb, &alive_count);
            GOC_DBG("pool_worker_fn: worker=%zu uv_run DEFAULT returned %d but loop still alive loop=%p alive=%d handle_count=%zu\n",
                    tl_worker->index, run_rc, (void*)&tl_worker->loop,
                    uv_loop_alive(&tl_worker->loop), alive_count);
        }
        uv_walk(&tl_worker->loop, pool_worker_close_remaining_handle_cb, NULL);
        GOC_DBG("pool_worker_fn: worker=%zu shutdown uv_run DEFAULT complete loop=%p alive=%d\n",
                tl_worker->index, (void*)&tl_worker->loop,
                uv_loop_alive(&tl_worker->loop));
    }

    pool_log_worker_loop_state(tl_worker, "after uv_run DEFAULT");

    /* Purge any remaining async wakeup state and ensure close callbacks are done. */
    while (true) {
        int nowait_rc = uv_run(&tl_worker->loop, UV_RUN_NOWAIT);
        GOC_DBG("pool_worker_fn: worker=%zu purge uv_run NOWAIT returned rc=%d loop=%p alive=%d\n",
                tl_worker->index, nowait_rc, (void*)&tl_worker->loop,
                uv_loop_alive(&tl_worker->loop));
        if (nowait_rc == 0) {
            if (uv_loop_alive(&tl_worker->loop)) {
                size_t nowait_handle_count = 0;
                uv_walk(&tl_worker->loop, pool_walk_handle_cb, &nowait_handle_count);
                GOC_DBG("pool_worker_fn: worker=%zu purge uv_run NOWAIT ended rc=0 but loop still alive handle_count=%zu loop=%p\n",
                        tl_worker->index, nowait_handle_count, (void*)&tl_worker->loop);
            }
            break;
        }
        pool_log_worker_loop_state(tl_worker, "purge uv_run NOWAIT");
        uv_walk(&tl_worker->loop, pool_worker_close_remaining_handle_cb, NULL);
    }

    pool_log_worker_loop_state(tl_worker, "after purge uv_run NOWAIT");
    uv_walk(&tl_worker->loop, pool_worker_close_remaining_handle_cb, NULL);

    if (tl_worker->wakeup) {
        free(tl_worker->wakeup);
        tl_worker->wakeup = NULL;
    }

    while (true) {
        pool_log_worker_loop_state(tl_worker, "before uv_loop_close");
        size_t pre_close_handle_count = 0;
        uv_walk(&tl_worker->loop, pool_walk_handle_cb, &pre_close_handle_count);
        int pre_close_alive = uv_loop_alive(&tl_worker->loop);
        if (!pre_close_alive && pre_close_handle_count == 0) {
            GOC_DBG("pool_worker_fn: worker=%zu loop is already dead with no handles before uv_loop_close, skipping close loop=%p\n",
                    tl_worker->index, (void*)&tl_worker->loop);
            break;
        }

        int loop_close_rc = uv_loop_close(&tl_worker->loop);
        int loop_alive = uv_loop_alive(&tl_worker->loop);
        if (loop_close_rc == 0 && !loop_alive) {
            GOC_DBG("pool_worker_fn: worker=%zu uv_loop_close loop=%p rc=%d alive=%d\n",
                    tl_worker->index, (void*)&tl_worker->loop,
                    loop_close_rc, loop_alive);
            break;
        }

        if (loop_close_rc == 0 && loop_alive) {
            GOC_DBG("pool_worker_fn: worker=%zu uv_loop_close returned 0 but loop still alive, stopping shutdown loop use loop=%p alive=%d\n",
                    tl_worker->index, (void*)&tl_worker->loop, loop_alive);
            break;
        }

        GOC_DBG("pool_worker_fn: worker=%zu uv_loop_close loop=%p rc=%d alive=%d - draining more\n",
                tl_worker->index, (void*)&tl_worker->loop,
                loop_close_rc, loop_alive);

        if (loop_close_rc == UV_EBUSY) {
            GOC_DBG("pool_worker_fn: worker=%zu uv_loop_close busy, dumping remaining handles\n",
                    tl_worker->index);
            uv_walk(&tl_worker->loop, pool_walk_handle_cb, &pre_close_handle_count);
        }

        if (loop_close_rc != 0 && loop_close_rc != UV_EBUSY) {
            GOC_DBG("pool_worker_fn: worker=%zu uv_loop_close unexpected rc=%d loop=%p alive=%d\n",
                    tl_worker->index, loop_close_rc, (void*)&tl_worker->loop,
                    loop_alive);
            break;
        }

        uv_walk(&tl_worker->loop, pool_worker_close_remaining_handle_cb, NULL);
        while (uv_loop_alive(&tl_worker->loop)) {
            pool_log_worker_loop_state(tl_worker, "drain before retry uv_run DEFAULT");
            GOC_DBG("pool_worker_fn: worker=%zu retry shutdown uv_run DEFAULT start loop=%p alive=%d\n",
                    tl_worker->index, (void*)&tl_worker->loop,
                    uv_loop_alive(&tl_worker->loop));
            int retry_rc = uv_run(&tl_worker->loop, UV_RUN_DEFAULT);
            GOC_DBG("pool_worker_fn: worker=%zu retry shutdown uv_run DEFAULT returned rc=%d loop=%p alive=%d\n",
                    tl_worker->index, retry_rc, (void*)&tl_worker->loop,
                    uv_loop_alive(&tl_worker->loop));
            uv_walk(&tl_worker->loop, pool_worker_close_remaining_handle_cb, NULL);
        }
        while (true) {
            int nowait_rc = uv_run(&tl_worker->loop, UV_RUN_NOWAIT);
            GOC_DBG("pool_worker_fn: worker=%zu retry purge uv_run NOWAIT returned rc=%d loop=%p alive=%d\n",
                    tl_worker->index, nowait_rc, (void*)&tl_worker->loop,
                    uv_loop_alive(&tl_worker->loop));
            if (nowait_rc == 0)
                break;
            pool_log_worker_loop_state(tl_worker, "drain retry purge uv_run NOWAIT");
            uv_walk(&tl_worker->loop, pool_worker_close_remaining_handle_cb, NULL);
        }
    }

    GOC_STATS_WORKER_STATUS((int)tl_worker->index, pool->id, GOC_WORKER_STOPPED, 0,
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
    GOC_DBG("post_to_run_queue: fn=%p entry=%p kind=%d pool=%p entry->pool=%p path=%s internal=%d w=%p\n",
            (void*)(uintptr_t)(entry ? entry->fn : NULL), (void*)entry,
            entry ? (int)entry->kind : -1,
            (void*)pool, (void*)(entry ? entry->pool : NULL),
            (w != NULL && w->pool == pool) ? "internal" : "external",
            (w != NULL && w->pool == pool), (void*)w);
    if (w != NULL && w->pool == pool) {
        /* Internal caller: push to executing worker's own deque.
         * Safe: the owner is inside mco_resume, not touching the deque. */
        wsdq_push_bottom(&w->deque, entry);

        /* Explicit seq_cst fence: the bottom store in wsdq_push_bottom is
         * only release; we need seq_cst here so idle_count reads below form a
         * total order with the worker's seq_cst increment (ARM/POWER safety). */
        atomic_thread_fence(memory_order_seq_cst);

        /* If another worker is idle, wake one peer so work posted to our own
         * deque can make progress even when the current fiber continues
         * executing without yielding. */
        if (pool->thread_count > 1 &&
            atomic_load_explicit(&pool->idle_count, memory_order_seq_cst) > 0) {
            size_t neighbor = (w->index + 1) % pool->thread_count;
            if (pool->workers[neighbor].wakeup) {
                int rc = worker_try_wakeup(&pool->workers[neighbor]);
                GOC_DBG("post_to_run_queue: internal enqueue wake peer=%zu rc=%d idle_count=%zu\n",
                        neighbor, rc,
                        atomic_load_explicit(&pool->idle_count, memory_order_seq_cst));
            }
        }

        GOC_DBG("post_to_run_queue: internal enqueue entry=%p fn=%p coro=%p home_worker_idx=%zu worker=%zu idle_count=%zu\n",
                (void*)entry,
                (void*)(uintptr_t)entry->fn,
                (void*)entry->coro,
                entry->home_worker_idx,
                w->index,
                atomic_load_explicit(&pool->idle_count, memory_order_seq_cst));
        (void)pool;
    } else {
        /* External caller: push into target worker's injector (MPSC-safe).
         * Round-robin across workers for load distribution. */
        size_t idx = atomic_fetch_add_explicit(&pool->next_push_idx, 1,
                                               memory_order_relaxed)
                     % pool->thread_count;
        injector_push(&pool->workers[idx].injector, entry);
        GOC_DBG("post_to_run_queue: external enqueue entry=%p target_worker=%zu idle_count=%zu injector_head=%p injector_tail=%p\n",
                (void*)entry, idx,
                atomic_load_explicit(&pool->idle_count, memory_order_seq_cst),
                (void*)pool->workers[idx].injector.head,
                (void*)pool->workers[idx].injector.tail);

        /* injector_push holds the mutex for the full operation; the unlock
         * is a full memory barrier, providing the seq_cst effect needed for
         * the sleep-miss race closure. */
        if (atomic_load_explicit(&pool->shutdown, memory_order_acquire)) {
            GOC_DBG("post_to_run_queue: pool shutdown, skip wakeup for worker=%zu\n",
                    idx);
            return;
        }
        if (pool->workers[idx].wakeup) {
            int rc = worker_try_wakeup(&pool->workers[idx]);
            GOC_DBG("post_to_run_queue: worker_try_wakeup target_worker=%zu rc=%d idle_count=%zu\n",
                    idx, rc,
                    atomic_load_explicit(&pool->idle_count, memory_order_seq_cst));
        }
    }
}

void post_to_specific_worker(goc_pool* pool,
                             size_t worker_idx,
                             goc_entry* entry) {
    if (!pool || worker_idx >= pool->thread_count || !entry) {
        ABORT("post_to_specific_worker: invalid arguments pool=%p worker_idx=%zu entry=%p\n",
              (void*)pool, worker_idx, (void*)entry);
    }

    goc_worker* target = &pool->workers[worker_idx];
    goc_worker* current = tl_worker;
    if (current == target) {
        /* Same-worker posts should stay on the local deque for low-latency
         * execution and to preserve locality. */
        wsdq_push_bottom(&target->deque, entry);
        atomic_thread_fence(memory_order_seq_cst);
        if (pool->thread_count > 1 &&
            atomic_load_explicit(&pool->idle_count, memory_order_seq_cst) > 0) {
            size_t neighbor = (target->index + 1) % pool->thread_count;
            if (pool->workers[neighbor].wakeup) {
                int rc = worker_try_wakeup(&pool->workers[neighbor]);
                GOC_DBG("post_to_specific_worker: internal enqueue wake peer=%zu rc=%d idle_count=%zu\n",
                        neighbor, rc,
                        atomic_load_explicit(&pool->idle_count, memory_order_seq_cst));
            }
        }
        GOC_DBG("post_to_specific_worker: internal enqueue entry=%p target_worker=%zu\n",
                (void*)entry, worker_idx);
    } else {
        injector_push(&target->injector, entry);
        GOC_DBG("post_to_specific_worker: external enqueue entry=%p target_worker=%zu injector_head=%p injector_tail=%p\n",
                (void*)entry, worker_idx,
                (void*)target->injector.head,
                (void*)target->injector.tail);
        if (atomic_load_explicit(&pool->shutdown, memory_order_acquire)) {
            GOC_DBG("post_to_specific_worker: pool shutdown, skip wakeup for worker=%zu\n",
                    worker_idx);
            return;
        }
        if (target->wakeup) {
            int rc = worker_try_wakeup(target);
            GOC_DBG("post_to_specific_worker: worker_try_wakeup target_worker=%zu rc=%d idle_count=%zu\n",
                    worker_idx, rc,
                    atomic_load_explicit(&pool->idle_count, memory_order_seq_cst));
        }
    }
}

void pool_submit_spawn_to_worker(goc_pool* pool,
                                 size_t worker_idx,
                                 void (*fn)(void*),
                                 void* arg,
                                 goc_chan* join_ch) {
    bool bypass_throttle = (tl_worker != NULL && tl_worker->pool == pool);

    uv_mutex_lock(&pool->drain_mutex);
    pool->live_count++;

    if (bypass_throttle ||
        (pool->pending_spawn_head == NULL && !pool_spawn_cap_reached_locked(pool))) {
        pool->resident_count++;
        size_t live = pool->live_count;
        GOC_DBG("pool_submit_spawn_to_worker: fn=%p worker=%zu bypass=%d resident=%zu live=%zu (fast path)\n",
                (void*)(uintptr_t)fn, worker_idx,
                (int)bypass_throttle, pool->resident_count, live);
        uv_mutex_unlock(&pool->drain_mutex);

        goc_entry* entry = goc_fiber_entry_create(pool, fn, arg, join_ch);
        entry->home_worker_idx = worker_idx;
        post_to_specific_worker(pool, worker_idx, entry);
        return;
    }

    GOC_DBG("pool_submit_spawn_to_worker: fn=%p worker=%zu bypass=%d DEFERRED (cap reached or queue non-empty)\n",
            (void*)(uintptr_t)fn, worker_idx, (int)bypass_throttle);
    goc_spawn_req* req = (goc_spawn_req*)GC_malloc_uncollectable(sizeof(goc_spawn_req));
    if (req == NULL) {
        uv_mutex_unlock(&pool->drain_mutex);
        ABORT("failed to allocate deferred spawn request\n");
    }

    req->fn              = fn;
    req->fn_arg          = arg;
    req->join_ch         = join_ch;
    req->home_worker_idx = worker_idx;
    req->next            = NULL;
    pool_enqueue_spawn_locked(pool, req);

    goc_spawn_req* admitted = pool_collect_admitted_spawns_locked(pool);
    uv_mutex_unlock(&pool->drain_mutex);

    pool_dispatch_spawn_list(pool, admitted);
}

/* -------------------------------------------------------------------------
 * pool_abort_if_called_from_worker
 *
 * Destroying a pool from one of its own worker threads is invalid: the
 * destroy path waits on drain/join and would attempt to join the caller
 * thread itself. Detect this explicitly and abort with a diagnostic.
 * ---------------------------------------------------------------------- */

static void pool_abort_if_called_from_worker(goc_pool* pool, const char* api_name) {
    if (tl_worker != NULL && tl_worker->pool == pool) {
        ABORT("%s called from within target pool worker thread; this is unsupported and would deadlock\n", api_name);
    }

    /* The thread-local worker sentinel is the canonical way to detect
     * same-pool worker threads in this codebase. Avoid the legacy
     * uv_thread_equal scan here because it can observe invalid or
     * partially initialized thread ids during pool teardown. */
}

/* -------------------------------------------------------------------------
 * goc_pool_make
 * ---------------------------------------------------------------------- */

goc_pool* goc_pool_make(size_t threads) {
    goc_pool* pool = malloc(sizeof(goc_pool));
    memset(pool, 0, sizeof(goc_pool));

    pool->id           = atomic_fetch_add_explicit(&g_pool_id_counter, 1, memory_order_relaxed);
    pool->thread_count = threads;
    pool->max_live_fibers = pool_default_max_live_fibers();
    pool->workers      = malloc(threads * sizeof(goc_worker));

    for (size_t i = 0; i < threads; i++) {
        wsdq_init(&pool->workers[i].deque, 256);
        injector_init(&pool->workers[i].injector);
        int rc = uv_loop_init(&pool->workers[i].loop);
        GOC_DBG("[uv_init] uv_loop_init worker=%zu rc=%d loop=%p thread=%llu\n",
                i, rc, (void*)&pool->workers[i].loop, goc_uv_thread_id());
        pool->workers[i].wakeup = (uv_async_t*)malloc(sizeof(uv_async_t));
        rc = uv_async_init(&pool->workers[i].loop, pool->workers[i].wakeup, worker_wakeup_cb);
        goc_uv_init_log("uv_async_init worker wakeup", rc, &pool->workers[i].loop, (uv_handle_t*)pool->workers[i].wakeup);
        pool->workers[i].index     = i;
        pool->workers[i].pool      = pool;
        pool->workers[i].loop.data = &pool->workers[i]; /* O(1) reverse-lookup in post_on_handle_loop */
        atomic_store_explicit(&pool->workers[i].steal_attempts,  0, memory_order_relaxed);
        atomic_store_explicit(&pool->workers[i].steal_successes, 0, memory_order_relaxed);
        atomic_store_explicit(&pool->workers[i].steal_misses,    0, memory_order_relaxed);
        atomic_store_explicit(&pool->workers[i].idle_wakeups,    0, memory_order_relaxed);
        atomic_store_explicit(&pool->workers[i].pending_handle_tasks, 0, memory_order_relaxed);
        atomic_store_explicit(&pool->workers[i].active_io_handles, 0, memory_order_relaxed);
        atomic_store_explicit(&pool->workers[i].task_queue, NULL, memory_order_relaxed);
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
        atomic_store_explicit(&pool->workers[i].closing, 0, memory_order_relaxed);
        int rc = goc_thread_create(&pool->workers[i].thread,
                                   pool_worker_fn, &pool->workers[i]);
        if (rc != 0) {
            ABORT("failed to create worker thread %zu/%zu (errno=%d)\n", i + 1, threads, rc);
        }
        GOC_STATS_WORKER_STATUS((int)i, pool->id, GOC_WORKER_CREATED, 0, 0, 0);
    }

    /* Ensure worker threads have reached idle state before the pool is used.
     * This avoids races where a newly-created pool returns before workers are
     * ready to steal or service posted work. */
    pool_wait_all_idle(pool, threads);

    GOC_STATS_POOL_STATUS(pool->id, GOC_POOL_CREATED, (int)threads);
    registry_add(pool);
    return pool;
}

/* -------------------------------------------------------------------------
 * goc_pool_destroy
 * ---------------------------------------------------------------------- */

void goc_pool_destroy(goc_pool* pool) {
    goc_debug_set_close_phase("pool_destroy");
    pool_abort_if_called_from_worker(pool, "goc_pool_destroy");
    GOC_STATS_POOL_STATUS(pool->id, GOC_POOL_DESTROYED, (int)pool->thread_count);
    GOC_DBG("goc_pool_destroy: entry pool=%p workers=%p thread_count=%zu live=%zu resident=%zu shutdown=%d idle=%zu\n",
            (void*)pool,
            (void*)pool->workers,
            pool->thread_count,
            pool->live_count,
            pool->resident_count,
            atomic_load_explicit(&pool->shutdown, memory_order_relaxed),
            atomic_load_explicit(&pool->idle_count, memory_order_relaxed));

    /* Invoke any module-specific destroy hooks before waiting for live fibers
     * to drain. This allows external subsystems like HTTP to wake blocked
     * fibers and observe shutdown without embedding knowledge here. */
    goc_run_lifecycle_hooks(GOC_LIFECYCLE_HOOK_PRE_POOL_DESTROY, pool);

    /* 1. Wait for all live fibers to exit (live_count reaches zero). */
    uv_mutex_lock(&pool->drain_mutex);
    GOC_DBG("goc_pool_destroy: begin drain pool=%p id=%d live=%zu resident=%zu pending_spawn=%zu idle=%zu\n",
            (void*)pool,
            pool->id,
            pool->live_count,
            pool->resident_count,
            pool_pending_spawn_count_locked(pool),
            atomic_load_explicit(&pool->idle_count, memory_order_relaxed));
    while (pool->live_count > 0) {
        int rc = uv_cond_timedwait(&pool->drain_cond,
                                   &pool->drain_mutex,
                                   1000000000ULL /* 1s heartbeat */);
        if (rc == UV_ETIMEDOUT) {
            GOC_DBG("goc_pool_destroy: POOL_DESTROY still draining pool=%p id=%d live=%zu resident=%zu pending_spawn=%zu idle=%zu shutdown=%d\n",
                    (void*)pool,
                    pool->id,
                    pool->live_count,
                    pool->resident_count,
                    pool_pending_spawn_count_locked(pool),
                    atomic_load_explicit(&pool->idle_count, memory_order_relaxed),
                    atomic_load_explicit(&pool->shutdown, memory_order_relaxed));
        } else {
            GOC_DBG("goc_pool_destroy: POOL_DESTROY drain wake pool=%p id=%d live=%zu resident=%zu pending_spawn=%zu rc=%d\n",
                    (void*)pool,
                    pool->id,
                    pool->live_count,
                    pool->resident_count,
                    pool_pending_spawn_count_locked(pool),
                    rc);
        }
    }
    GOC_DBG("goc_pool_destroy: POOL_DESTROY drain complete pool=%p id=%d live=%zu resident=%zu idle=%zu shutdown=%d\n",
            (void*)pool, pool->id, pool->live_count, pool->resident_count,
            atomic_load_explicit(&pool->idle_count, memory_order_relaxed),
            atomic_load_explicit(&pool->shutdown, memory_order_relaxed));
    uv_mutex_unlock(&pool->drain_mutex);

    /* 2. Signal workers to exit. */
    GOC_DBG("goc_pool_destroy: signaling workers shutdown\n");
    atomic_store_explicit(&pool->shutdown, 1, memory_order_release);
    GOC_DBG("goc_pool_destroy: shutdown flag set pool=%p id=%d live=%zu resident=%zu idle=%zu\n",
            (void*)pool, pool->id, pool->live_count,
            pool->resident_count,
            atomic_load_explicit(&pool->idle_count, memory_order_relaxed));

    /* 3. Unblock all waiting workers (one wakeup per worker). */
    for (size_t i = 0; i < pool->thread_count; i++) {
        if (pool->workers[i].wakeup) {
            int wakeup_active = uv_is_active((uv_handle_t*)pool->workers[i].wakeup);
            int wakeup_closing = uv_is_closing((uv_handle_t*)pool->workers[i].wakeup);
            int rcs = worker_try_wakeup(&pool->workers[i]);
            GOC_DBG("goc_pool_destroy: wakeup send worker=%zu wakeup=%p active=%d has_ref=%d closing=%d shutdown=%d rc=%d\n",
                    i,
                    (void*)pool->workers[i].wakeup,
                    wakeup_active,
                    uv_has_ref((uv_handle_t*)pool->workers[i].wakeup),
                    wakeup_closing,
                    atomic_load_explicit(&pool->shutdown, memory_order_relaxed),
                    rcs);
            if (rcs < 0 && rcs != UV_ECANCELED) {
                GOC_DBG("goc_pool_destroy: uv_async_send FAILED worker=%zu wakeup=%p rc=%d\n",
                        i, (void*)pool->workers[i].wakeup, rcs);
            }
        }
    }

    /* 4. Reap worker threads. */
    GOC_DBG("goc_pool_destroy: POOL_DESTROY joining %zu workers pool=%p id=%d\n",
            pool->thread_count, (void*)pool, pool->id);
    for (size_t i = 0; i < pool->thread_count; i++) {
        GOC_DBG("goc_pool_destroy: POOL_DESTROY joining worker=%zu\n", i);
        goc_thread_join(&pool->workers[i].thread);
        GOC_DBG("goc_pool_destroy: POOL_DESTROY joined worker=%zu\n", i);
    }
    GOC_DBG("goc_pool_destroy: POOL_DESTROY all workers joined, proceeding to per-worker resource cleanup\n");
    GOC_DBG("goc_pool_destroy: pool=%p id=%d pre-cleanup live=%zu resident=%zu idle=%zu shutdown=%d\n",
            (void*)pool, pool->id, pool->live_count, pool->resident_count,
            atomic_load_explicit(&pool->idle_count, memory_order_relaxed),
            atomic_load_explicit(&pool->shutdown, memory_order_relaxed));

    /* 5. Destroy per-worker resources. */
    for (size_t i = 0; i < pool->thread_count; i++) {
        GOC_DBG("goc_pool_destroy: worker=%zu loop=%p wakeup=%p alive=%d task_queue=%p\n",
                i, (void*)&pool->workers[i].loop, (void*)pool->workers[i].wakeup,
                uv_loop_alive(&pool->workers[i].loop),
                (void*)atomic_load_explicit(&pool->workers[i].task_queue, memory_order_relaxed));

        /* The worker thread performs its own loop shutdown and uv_loop_close(). */
        wsdq_destroy(&pool->workers[i].deque);
        injector_destroy(&pool->workers[i].injector);
    }

    /* 6. Destroy drain primitives. */
    uv_mutex_destroy(&pool->drain_mutex);
    uv_cond_destroy(&pool->drain_cond);

    /* 7. Remove from registry (no-op if already removed by destroy_all). */
    GOC_DBG("goc_pool_destroy: removing pool from registry pool=%p id=%d\n",
            (void*)pool, pool->id);
    registry_remove(pool);

    /* 8. Free workers array and pool itself. */
    GOC_DBG("goc_pool_destroy: freeing workers array pool=%p workers=%p\n",
            (void*)pool, (void*)pool->workers);
    free(pool->workers);
    GOC_DBG("goc_pool_destroy: freed workers array pool=%p workers=%p\n",
            (void*)pool, (void*)pool->workers);
    GOC_DBG("goc_pool_destroy: freeing pool object %p\n",
            (void*)pool);
    free(pool);
    goc_debug_set_close_phase("normal");
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
        if (pool->workers[i].wakeup) {
            int rc = worker_try_wakeup(&pool->workers[i]);
            GOC_DBG("goc_pool_destroy_timeout: wakeup send worker=%zu wakeup=%p rc=%d\n",
                    i, (void*)pool->workers[i].wakeup, rc);
        }
    }

    for (size_t i = 0; i < pool->thread_count; i++) {
        goc_thread_join(&pool->workers[i].thread);
    }

    for (size_t i = 0; i < pool->thread_count; i++) {
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
void goc_pool_get_steal_stats(uint64_t *attempts, uint64_t *successes,
                              uint64_t *misses,   uint64_t *idle_wakeups)
{
    *attempts     = atomic_load_explicit(&g_steal_attempts,  memory_order_seq_cst);
    *successes    = atomic_load_explicit(&g_steal_successes, memory_order_seq_cst);
    *misses       = atomic_load_explicit(&g_steal_misses,    memory_order_seq_cst);
    *idle_wakeups = atomic_load_explicit(&g_idle_wakeups,    memory_order_seq_cst);
}

/* -------------------------------------------------------------------------
 * pool_wait_all_idle — test helper
 * ---------------------------------------------------------------------- */
void pool_wait_all_idle(goc_pool* pool, size_t n) {
    while (atomic_load_explicit(&pool->idle_count, memory_order_seq_cst) < n)
        uv_sleep(0);
}

int goc_pool_id(goc_pool* pool) {
    return pool ? pool->id : -1;
}
