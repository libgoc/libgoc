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
    size_t          active_count;
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

    struct GC_stack_base sb;
    GC_get_stack_base(&sb);
    GC_register_my_thread(&sb);

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

        /* Decrement active_count unconditionally (fiber yielded or exited). */
        pthread_mutex_lock(&pool->drain_mutex);
        pool->active_count--;
        pthread_mutex_unlock(&pool->drain_mutex);

        /* Check for fiber completion *after* the decrement has landed. */
        if (mco_status(coro) == MCO_DEAD) {
            mco_destroy(coro);

            pthread_mutex_lock(&pool->drain_mutex);
            pthread_cond_broadcast(&pool->drain_cond);
            pthread_mutex_unlock(&pool->drain_mutex);
        }
    }

    GC_unregister_my_thread();
    return NULL;
}

/* -------------------------------------------------------------------------
 * post_to_run_queue — internal; called from fiber.c and channel.c
 * ---------------------------------------------------------------------- */

void post_to_run_queue(goc_pool* pool, goc_entry* entry) {
    pthread_mutex_lock(&pool->drain_mutex);
    pool->active_count++;
    pthread_mutex_unlock(&pool->drain_mutex);

    runq_push(&pool->runq, entry);
    uv_sem_post(&pool->work_sem);
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
    atomic_store(&pool->shutdown, 0);

    pool->thread_count = threads;
    pool->threads = malloc(threads * sizeof(pthread_t));

    for (size_t i = 0; i < threads; i++) {
        pthread_create(&pool->threads[i], NULL, pool_worker_fn, pool);
    }

    registry_add(pool);

    return pool;
}

/* -------------------------------------------------------------------------
 * goc_pool_destroy
 * ---------------------------------------------------------------------- */

void goc_pool_destroy(goc_pool* pool) {
    /* 1. Wait for all active fibers to yield or complete. */
    pthread_mutex_lock(&pool->drain_mutex);
    while (pool->active_count > 0) {
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
        pthread_join(pool->threads[i], NULL);
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
    /* Build an absolute deadline. */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec  += (time_t)(ms / 1000);
    deadline.tv_nsec += (long)((ms % 1000) * 1000000L);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec  += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&pool->drain_mutex);
    int timed_out = 0;
    while (pool->active_count > 0 && !timed_out) {
        int rc = pthread_cond_timedwait(&pool->drain_cond,
                                        &pool->drain_mutex,
                                        &deadline);
        if (rc == ETIMEDOUT) {
            timed_out = 1;
        }
    }
    pthread_mutex_unlock(&pool->drain_mutex);

    if (timed_out && pool->active_count > 0) {
        /* Pool stays valid and running — do not tear it down. */
        return GOC_DRAIN_TIMEOUT;
    }

    /* Drain completed within the deadline; perform full shutdown. */
    atomic_store_explicit(&pool->shutdown, 1, memory_order_release);

    for (size_t i = 0; i < pool->thread_count; i++) {
        uv_sem_post(&pool->work_sem);
    }

    for (size_t i = 0; i < pool->thread_count; i++) {
        pthread_join(pool->threads[i], NULL);
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
