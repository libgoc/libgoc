/*
 * src/pool.c
 *
 * Thread pool workers, run queue, pool registry, and drain logic.
 * Defines goc_pool and all pool operations.
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

/* -------------------------------------------------------------------------
 * Run-queue node (GC-heap so the entry pointer is visible to the collector)
 * ---------------------------------------------------------------------- */

typedef struct goc_runq_node {
    goc_entry*             entry;
    struct goc_runq_node*  next;
} goc_runq_node;

/* -------------------------------------------------------------------------
 * Two-lock MPMC run queue (Michael & Scott style)
 * ---------------------------------------------------------------------- */

typedef struct {
    goc_runq_node* head;
    goc_runq_node* tail;
    uv_mutex_t     head_lock;
    uv_mutex_t     tail_lock;
} goc_runq;

/* -------------------------------------------------------------------------
 * goc_pool — full definition (opaque outside pool.c)
 * ---------------------------------------------------------------------- */

struct goc_pool {
    pthread_t*      threads;
    size_t          thread_count;
    goc_runq        runq;
    uv_sem_t        work_sem;
    _Atomic int     shutdown;
    pthread_mutex_t drain_mutex;
    pthread_cond_t  drain_cond;
    size_t          active_count; /* fibers currently queued or executing; decremented
                                     unconditionally after every mco_resume (yield or exit).
                                     Used only for internal scheduling accounting. */
    size_t          live_count;   /* fibers still alive on this pool; incremented exactly
                                     once per fiber at birth (pool_fiber_born), decremented
                                     only when mco_status == MCO_DEAD.  This is the correct
                                     drain signal: a parked fiber still has live_count > 0
                                     even though active_count has already been decremented. */
};

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
 * runq_push / runq_pop — two-lock MPMC operations
 * ---------------------------------------------------------------------- */

static void runq_push(goc_runq* q, goc_entry* entry) {
    goc_runq_node* node = goc_malloc(sizeof(goc_runq_node));
    node->entry = entry;
    node->next  = NULL;

    uv_mutex_lock(&q->tail_lock);
    q->tail->next = node;
    q->tail       = node;
    uv_mutex_unlock(&q->tail_lock);
}

static goc_entry* runq_pop(goc_runq* q) {
    uv_mutex_lock(&q->head_lock);
    goc_runq_node* sentinel = q->head;
    goc_runq_node* next     = sentinel->next;
    if (next == NULL) {
        uv_mutex_unlock(&q->head_lock);
        return NULL;
    }
    goc_entry* entry = next->entry;
    q->head          = next;
    uv_mutex_unlock(&q->head_lock);
    /* sentinel (old head) is now unreachable; GC will collect it */
    return entry;
}

/* -------------------------------------------------------------------------
 * pool_worker_fn — thread entry point
 * ---------------------------------------------------------------------- */

static void* pool_worker_fn(void* arg) {
    goc_pool* pool = (goc_pool*)arg;

    while (!atomic_load_explicit(&pool->shutdown, memory_order_acquire)) {
        uv_sem_wait(&pool->work_sem);

        goc_entry* entry = runq_pop(&pool->runq);
        if (entry == NULL) {
            /* Spurious wake (e.g. shutdown signal); re-check loop condition. */
            continue;
        }

        /* Keep the entry pointer GC-rooted for the duration of the resume. */
        GC_add_roots(&entry, &entry + 1);

        /* Canary check — abort on stack overflow before corrupting anything. */
        if (*entry->stack_canary_ptr != GOC_STACK_CANARY) {
            abort();
        }

        /* Save the coroutine handle before resuming.  If this is a parking
         * entry (stack-allocated inside goc_take on the fiber's own stack),
         * the fiber may run to completion during mco_resume — clobbering the
         * memory where `entry` lives before control returns here.  The
         * mco_coro object itself is minicoro heap-allocated and remains valid
         * until mco_destroy, so `coro` is safe to dereference after the
         * resume regardless of what happened to `entry`. */
        mco_coro* coro = entry->coro;

        mco_resume(coro);

        GC_remove_roots(&entry, &entry + 1);

        /* Correctness invariant: every fiber that yields (MCO_SUSPENDED) must
         * be re-posted to a run queue via post_to_run_queue(), which increments
         * active_count before the next resume.  If a fiber yields without being
         * re-queued (a bug in the channel / alts layer), active_count will reach
         * zero prematurely and goc_pool_destroy will return with live coroutines.
         * The canary check above and the assert(winner != NULL) in alts.c are the
         * primary guards against this happening silently.
         * Note: live_count is NOT touched here — it is managed by pool_fiber_born
         * (increment) and the MCO_DEAD branch below (decrement). */
        pthread_mutex_lock(&pool->drain_mutex);
        pool->active_count--;
        pthread_mutex_unlock(&pool->drain_mutex);

        /* Decrement live_count and broadcast only when the fiber has actually
         * exited.  A parked fiber (MCO_SUSPENDED) still counts as live — it will
         * be re-enqueued by wake() when a channel operation resumes it.
         * Broadcasting on every yield would let goc_pool_destroy_timeout see
         * live_count == 0 while fibers are merely parked, causing a premature
         * GOC_DRAIN_OK return and pool teardown. */
        if (mco_status(coro) == MCO_DEAD) {
            mco_destroy(coro);

            pthread_mutex_lock(&pool->drain_mutex);
            pool->live_count--;
            pthread_cond_broadcast(&pool->drain_cond);
            pthread_mutex_unlock(&pool->drain_mutex);
        }
    }

    return NULL;
}

/* -------------------------------------------------------------------------
 * post_to_run_queue — internal; called from fiber.c and channel.c
 * ---------------------------------------------------------------------- */

void post_to_run_queue(goc_pool* pool, goc_entry* entry) {
    pthread_mutex_lock(&pool->drain_mutex);
    pool->active_count++;
    /* live_count is NOT incremented here.  It is incremented exactly once per
     * fiber, at birth, by pool_fiber_born() (called from goc_go_on in fiber.c).
     * Re-queuing a parked fiber via wake() must not inflate live_count, because
     * live_count is the drain signal: it reaches zero only when every fiber has
     * run to MCO_DEAD.  Incrementing it on every re-queue caused it to grow
     * unboundedly and goc_pool_destroy to block forever. */
    pthread_mutex_unlock(&pool->drain_mutex);

    runq_push(&pool->runq, entry);
    uv_sem_post(&pool->work_sem);
}

/* -------------------------------------------------------------------------
 * pool_fiber_born — increment live_count exactly once per new fiber
 *
 * Called from goc_go_on (fiber.c) before post_to_run_queue, so that
 * live_count tracks the number of fibers alive on the pool rather than
 * the number of scheduler events.  This is the only correct drain signal:
 * a parked fiber must still count as live even while active_count is zero.
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
        if (pthread_equal(self, pool->threads[i])) {
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

    /* Initialise run queue with a sentinel node (GC-heap). */
    goc_runq_node* sentinel = goc_malloc(sizeof(goc_runq_node));
    sentinel->entry = NULL;
    sentinel->next  = NULL;
    pool->runq.head = sentinel;
    pool->runq.tail = sentinel;
    uv_mutex_init(&pool->runq.head_lock);
    uv_mutex_init(&pool->runq.tail_lock);

    uv_sem_init(&pool->work_sem, 0);

    pthread_mutex_init(&pool->drain_mutex, NULL);
    pthread_cond_init(&pool->drain_cond, NULL);

    pool->active_count = 0;
    pool->live_count   = 0;
    atomic_store(&pool->shutdown, 0);

    pool->thread_count = threads;
    pool->threads = malloc(threads * sizeof(pthread_t));

    for (size_t i = 0; i < threads; i++) {
        GC_pthread_create(&pool->threads[i], NULL, pool_worker_fn, pool);
    }

    registry_add(pool);

    return pool;
}

/* -------------------------------------------------------------------------
 * goc_pool_destroy
 * ---------------------------------------------------------------------- */

void goc_pool_destroy(goc_pool* pool) {
    pool_abort_if_called_from_worker(pool, "goc_pool_destroy");

    /* 1. Wait for all live fibers to exit (live_count reaches zero).
     *    Parked fibers (MCO_SUSPENDED) still count as live even though
     *    active_count may already be zero; using live_count here prevents a
     *    premature return while fibers are merely blocked on a channel. */
    pthread_mutex_lock(&pool->drain_mutex);
    while (pool->live_count > 0) {
        pthread_cond_wait(&pool->drain_cond, &pool->drain_mutex);
    }
    pthread_mutex_unlock(&pool->drain_mutex);

    /* 2. Signal workers to exit. */
    atomic_store_explicit(&pool->shutdown, 1, memory_order_release);

    /* 3. Unblock all waiting workers (one post per thread). */
    for (size_t i = 0; i < pool->thread_count; i++) {
        uv_sem_post(&pool->work_sem);
    }

    /* 4. Reap worker threads. */
    for (size_t i = 0; i < pool->thread_count; i++) {
        GC_pthread_join(pool->threads[i], NULL);
    }

    /* 5. Destroy synchronisation primitives. */
    uv_sem_destroy(&pool->work_sem);
    uv_mutex_destroy(&pool->runq.head_lock);
    uv_mutex_destroy(&pool->runq.tail_lock);
    pthread_mutex_destroy(&pool->drain_mutex);
    pthread_cond_destroy(&pool->drain_cond);

    /* 6. Drain and release any remaining runq nodes.
     *    (Entries themselves are GC-heap; only the nodes are freed.) */
    goc_runq_node* n = pool->runq.head;
    while (n != NULL) {
        goc_runq_node* next = n->next;
        /* n is GC-heap; the GC will collect it — no explicit free needed.
         * The sentinel and any unconsumed nodes are already unreachable. */
        (void)n;
        n = next;
    }

    /* 7. Remove from registry (no-op if already removed by destroy_all). */
    registry_remove(pool);

    /* 8. Free pool itself. */
    free(pool->threads);
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
        uv_sem_post(&pool->work_sem);
    }

    for (size_t i = 0; i < pool->thread_count; i++) {
        GC_pthread_join(pool->threads[i], NULL);
    }

    uv_sem_destroy(&pool->work_sem);
    uv_mutex_destroy(&pool->runq.head_lock);
    uv_mutex_destroy(&pool->runq.tail_lock);
    pthread_mutex_destroy(&pool->drain_mutex);
    pthread_cond_destroy(&pool->drain_cond);

    /* Drain runq nodes (GC-heap; already unreachable after workers exit). */
    goc_runq_node* n = pool->runq.head;
    while (n != NULL) {
        goc_runq_node* next = n->next;
        (void)n;
        n = next;
    }

    registry_remove(pool);

    free(pool->threads);
    free(pool);

    return GOC_DRAIN_OK;
}
