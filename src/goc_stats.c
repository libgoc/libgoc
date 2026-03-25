#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <uv.h>
#include "../include/goc_stats.h"
#include "internal.h"

#ifdef GOC_ENABLE_STATS

/* --------------------------------------------------------------------------
 * MPSC lock-free queue of pending stats events (Vyukov-style)
 *
 * Both the sentinel and data nodes are heap-allocated so that sq_pop can
 * always free the *old* sentinel unconditionally, with no special cases.
 *
 * Ownership rule:
 *   g_sq_tail always points to the current sentinel (a consumed dummy node).
 *   sq_pop copies the event out of `next`, advances g_sq_tail to `next`
 *   (which becomes the new sentinel), and frees the old `tail`.
 *   The caller never sees a node pointer and never calls free().
 * -------------------------------------------------------------------------- */

typedef struct stats_node {
    _Atomic(struct stats_node *) next;
    struct goc_stats_event       ev;
} stats_node;

static _Atomic(stats_node *) g_sq_head;   /* producers exchange here */
static stats_node            *g_sq_tail;  /* consumer (loop thread) only */

static void sq_init(void) {
    stats_node *sentinel = (stats_node *)malloc(sizeof(stats_node));
    atomic_store_explicit(&sentinel->next, NULL, memory_order_relaxed);
    atomic_store_explicit(&g_sq_head, sentinel, memory_order_relaxed);
    g_sq_tail = sentinel;
}

/* Push from any thread. */
static void sq_push(stats_node *node) {
    atomic_store_explicit(&node->next, NULL, memory_order_relaxed);
    stats_node *prev = atomic_exchange_explicit(&g_sq_head, node,
                                                memory_order_acq_rel);
    atomic_store_explicit(&prev->next, node, memory_order_release);
}

/* Pop on the loop thread only.
 * Copies the event into *out, frees the old sentinel, returns true.
 * Returns false when the queue is empty (g_sq_tail->next == NULL). */
static bool sq_pop(struct goc_stats_event *out) {
    stats_node *tail = g_sq_tail;
    stats_node *next = atomic_load_explicit(&tail->next, memory_order_acquire);
    if (!next) return false;
    *out      = next->ev;  /* copy before advancing */
    g_sq_tail = next;      /* next is now the new sentinel */
    free(tail);            /* old sentinel is no longer referenced */
    return true;
}

/* Free the remaining sentinel after a final drain (loop thread only). */
static void sq_destroy(void) {
    free(g_sq_tail);
    g_sq_tail = NULL;
}

/* --------------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------------- */

static _Atomic int                  stats_enabled   = 0;
static _Atomic int                  mutex_inited    = 0;
static uv_mutex_t                   g_cb_mutex;
static _Atomic(goc_stats_callback)  g_cb            = NULL;
static _Atomic(void *)              g_cb_ud         = NULL;

static uv_async_t                  *g_stats_async   = NULL;
static _Atomic int                  g_stats_closing  = 0;
static uv_mutex_t                   g_close_mutex;
static uv_cond_t                    g_close_cond;
static int                          g_close_done     = 0;

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static uint64_t goc_stats_now(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Drain the queue and fire the callback for each event. Loop thread only. */
static void stats_drain(void) {
    goc_stats_callback cb = atomic_load_explicit(&g_cb, memory_order_acquire);
    void *ud              = atomic_load_explicit(&g_cb_ud, memory_order_relaxed);
    struct goc_stats_event ev;
    while (sq_pop(&ev)) {
        if (cb) cb(&ev, ud);
    }
}

static void stats_on_close(uv_handle_t *h) {
    free(h);
    g_stats_async = NULL;
    sq_destroy();
    uv_mutex_lock(&g_close_mutex);
    g_close_done = 1;
    uv_cond_signal(&g_close_cond);
    uv_mutex_unlock(&g_close_mutex);
}

/* Loop-thread callback: drain events; if closing, drain once more then close. */
static void stats_on_async(uv_async_t *h) {
    stats_drain();
    if (atomic_load_explicit(&g_stats_closing, memory_order_acquire)) {
        stats_drain(); /* catch anything pushed just before the flag was set */
        uv_close((uv_handle_t *)h, stats_on_close);
    }
}

/* --------------------------------------------------------------------------
 * Default callback
 * -------------------------------------------------------------------------- */

static void goc_stats_default_callback(const struct goc_stats_event *ev, void *ud) {
    (void)ud;
    const char *type = "?";
    switch (ev->type) {
        case GOC_STATS_EVENT_POOL_STATUS:    type = "POOL";    break;
        case GOC_STATS_EVENT_WORKER_STATUS:  type = "WORKER";  break;
        case GOC_STATS_EVENT_FIBER_STATUS:   type = "FIBER";   break;
        case GOC_STATS_EVENT_CHANNEL_STATUS: type = "CHANNEL"; break;
    }
    printf("[goc_stats] %s @ %llu: ", type, (unsigned long long)ev->timestamp);
    switch (ev->type) {
        case GOC_STATS_EVENT_POOL_STATUS:
            printf("pool=%p status=%d threads=%d\n",
                   ev->data.pool.id, ev->data.pool.status, ev->data.pool.thread_count);
            break;
        case GOC_STATS_EVENT_WORKER_STATUS:
            printf("id=%d status=%d pending=%d\n",
                   ev->data.worker.id, ev->data.worker.status, ev->data.worker.pending_jobs);
            break;
        case GOC_STATS_EVENT_FIBER_STATUS:
            printf("id=%d last_worker=%d status=%d\n",
                   ev->data.fiber.id, ev->data.fiber.last_worker_id, ev->data.fiber.status);
            break;
        case GOC_STATS_EVENT_CHANNEL_STATUS:
            printf("id=%d status=%d buf_size=%d item_count=%d\n",
                   ev->data.channel.id, ev->data.channel.status,
                   ev->data.channel.buf_size, ev->data.channel.item_count);
            break;
        default:
            printf("(unknown event)\n");
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void goc_stats_set_callback(goc_stats_callback cb, void *ud) {
    uv_mutex_lock(&g_cb_mutex);
    atomic_store_explicit(&g_cb_ud, ud, memory_order_relaxed);
    atomic_store_explicit(&g_cb,    cb, memory_order_release);
    uv_mutex_unlock(&g_cb_mutex);
}

void goc_stats_init(void) {
    if (atomic_load_explicit(&stats_enabled, memory_order_acquire)) return;

    /* Initialise mutexes/condvar exactly once across init/shutdown cycles. */
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(
            &mutex_inited, &expected, 1,
            memory_order_acq_rel, memory_order_acquire)) {
        uv_mutex_init(&g_cb_mutex);
        uv_mutex_init(&g_close_mutex);
        uv_cond_init(&g_close_cond);
    }

    uv_mutex_lock(&g_cb_mutex);
    atomic_store_explicit(&g_cb_ud, (void *)NULL,               memory_order_relaxed);
    atomic_store_explicit(&g_cb,    goc_stats_default_callback,  memory_order_release);
    uv_mutex_unlock(&g_cb_mutex);

    sq_init();

    uv_mutex_lock(&g_close_mutex);
    g_close_done = 0;
    uv_mutex_unlock(&g_close_mutex);

    atomic_store_explicit(&g_stats_closing, 0, memory_order_release);

    g_stats_async = (uv_async_t *)malloc(sizeof(uv_async_t));
    uv_async_init(g_loop, g_stats_async, stats_on_async);

    atomic_store_explicit(&stats_enabled, 1, memory_order_release);
}

void goc_stats_shutdown(void) {
    if (!atomic_load_explicit(&stats_enabled, memory_order_acquire)) return;

    atomic_store_explicit(&stats_enabled,   0, memory_order_release);
    atomic_store_explicit(&g_stats_closing, 1, memory_order_release);
    uv_async_send(g_stats_async);

    uv_mutex_lock(&g_close_mutex);
    while (!g_close_done)
        uv_cond_wait(&g_close_cond, &g_close_mutex);
    uv_mutex_unlock(&g_close_mutex);

    uv_mutex_lock(&g_cb_mutex);
    atomic_store_explicit(&g_cb,    NULL, memory_order_release);
    atomic_store_explicit(&g_cb_ud, NULL, memory_order_relaxed);
    uv_mutex_unlock(&g_cb_mutex);
}

bool goc_stats_is_enabled(void) {
    return atomic_load_explicit(&stats_enabled, memory_order_acquire) != 0;
}

/* --------------------------------------------------------------------------
 * Internal emit functions
 * -------------------------------------------------------------------------- */

static void goc_stats_dispatch(const struct goc_stats_event *ev) {
    if (!atomic_load_explicit(&stats_enabled, memory_order_acquire)) return;

    stats_node *node = (stats_node *)malloc(sizeof(stats_node));
    if (!node) return;
    node->ev = *ev;

    sq_push(node);
    uv_async_send(g_stats_async);
}

void goc_stats_submit_event_pool(void *id, int status, int thread_count) {
    struct goc_stats_event ev;
    ev.type                   = GOC_STATS_EVENT_POOL_STATUS;
    ev.timestamp              = goc_stats_now();
    ev.data.pool.id           = id;
    ev.data.pool.status       = status;
    ev.data.pool.thread_count = thread_count;
    goc_stats_dispatch(&ev);
}

void goc_stats_submit_event_worker(int id, int status, int pending_jobs) {
    struct goc_stats_event ev;
    ev.type                     = GOC_STATS_EVENT_WORKER_STATUS;
    ev.timestamp                = goc_stats_now();
    ev.data.worker.id           = id;
    ev.data.worker.status       = status;
    ev.data.worker.pending_jobs = pending_jobs;
    goc_stats_dispatch(&ev);
}

void goc_stats_submit_event_fiber(int id, int last_worker_id, int status) {
    struct goc_stats_event ev;
    ev.type                      = GOC_STATS_EVENT_FIBER_STATUS;
    ev.timestamp                 = goc_stats_now();
    ev.data.fiber.id             = id;
    ev.data.fiber.last_worker_id = last_worker_id;
    ev.data.fiber.status         = status;
    goc_stats_dispatch(&ev);
}

void goc_stats_submit_event_channel(int id, int status, int buf_size, int item_count) {
    struct goc_stats_event ev;
    ev.type                    = GOC_STATS_EVENT_CHANNEL_STATUS;
    ev.timestamp               = goc_stats_now();
    ev.data.channel.id         = id;
    ev.data.channel.status     = status;
    ev.data.channel.buf_size   = buf_size;
    ev.data.channel.item_count = item_count;
    goc_stats_dispatch(&ev);
}

#else /* !GOC_ENABLE_STATS */

void goc_stats_set_callback(goc_stats_callback cb, void *ud) { (void)cb; (void)ud; }
void goc_stats_init(void)     {}
void goc_stats_shutdown(void) {}
bool goc_stats_is_enabled(void) { return false; }

#endif /* GOC_ENABLE_STATS */
