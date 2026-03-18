/* src/loop.c
 *
 * Owns the libuv event loop (g_loop) and its thread.
 * Owns the cross-thread wakeup handle (g_wakeup) and shutdown handle
 * (g_shutdown_async).
 * Owns the MPSC callback queue (g_cb_queue) and its drain logic (on_wakeup).
 * Exposes: goc_scheduler(), loop_init(), loop_shutdown(), post_callback().
 */

#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <uv.h>
#include <gc.h>
#include "../include/goc.h"
#include "internal.h"

/* --------------------------------------------------------------------------
 * MPSC queue (Vyukov-style intrusive lock-free queue)
 * Nodes are GC-allocated so the GC can trace the goc_entry* inside.
 * -------------------------------------------------------------------------- */

typedef struct mpsc_node {
    _Atomic(struct mpsc_node *) next;
    goc_entry                  *entry;
} mpsc_node;

typedef struct {
    _Atomic(mpsc_node *) head;  /* producers exchange here */
    mpsc_node           *tail;  /* consumer-only */
} mpsc_queue_t;

/* --------------------------------------------------------------------------
 * Global state (exported via extern in internal.h)
 * -------------------------------------------------------------------------- */

uv_loop_t            *g_loop           = NULL;
_Atomic(uv_async_t *) g_wakeup;                 /* exported */

static uv_async_t    *g_wakeup_raw     = NULL;  /* raw pointer behind g_wakeup */
static uv_async_t    *g_shutdown_async = NULL;
static pthread_t      g_loop_thread;
static mpsc_queue_t   g_cb_queue;

/* --------------------------------------------------------------------------
 * MPSC queue implementation
 * -------------------------------------------------------------------------- */

static void cb_queue_init(void)
{
    mpsc_node *sentinel = (mpsc_node *)goc_malloc(sizeof(mpsc_node));
    atomic_store_explicit(&sentinel->next, NULL, memory_order_relaxed);
    sentinel->entry = NULL;
    atomic_store_explicit(&g_cb_queue.head, sentinel, memory_order_relaxed);
    g_cb_queue.tail = sentinel;
}

/* Enqueue an entry onto g_cb_queue (producer — any thread). */
static void cb_queue_push(mpsc_node *node)
{
    atomic_store_explicit(&node->next, NULL, memory_order_relaxed);
    /* Vyukov: exchange head and link previous head's next to this node. */
    mpsc_node *prev = atomic_exchange_explicit(&g_cb_queue.head, node,
                                               memory_order_acq_rel);
    atomic_store_explicit(&prev->next, node, memory_order_release);
}

/* Dequeue from g_cb_queue (consumer — loop thread only).
 * Returns the entry or NULL if the queue is empty. */
static goc_entry *cb_queue_pop(void)
{
    mpsc_node *tail = g_cb_queue.tail;
    mpsc_node *next = atomic_load_explicit(&tail->next, memory_order_acquire);
    if (!next)
        return NULL;                /* queue is empty */
    g_cb_queue.tail = next;         /* advance tail past sentinel */
    goc_entry *e = next->entry;
    /* Old tail (sentinel) is now garbage — GC will collect it. */
    return e;
}

/* --------------------------------------------------------------------------
 * post_callback — called from any thread (pool workers, loop thread)
 * -------------------------------------------------------------------------- */

void post_callback(goc_entry *entry, void *value)
{
    /* For take callbacks: store the delivered value. */
    entry->cb_result = value;

    mpsc_node *node = (mpsc_node *)goc_malloc(sizeof(mpsc_node));
    node->entry = entry;
    cb_queue_push(node);

    uv_async_t *w = atomic_load_explicit(&g_wakeup, memory_order_acquire);
    if (w)
        uv_async_send(w);
}

/* --------------------------------------------------------------------------
 * goc_scheduler — public
 * -------------------------------------------------------------------------- */

uv_loop_t *goc_scheduler(void)
{
    return g_loop;
}

/* --------------------------------------------------------------------------
 * Loop thread callbacks
 * -------------------------------------------------------------------------- */

static void free_handle_cb(uv_handle_t *h)
{
    free(h);
}

static void on_wakeup_closed(uv_handle_t *h)
{
    /* Set g_wakeup to NULL only once libuv guarantees no further callbacks
     * will fire for this handle, preventing a race with post_callback. */
    atomic_store_explicit(&g_wakeup, NULL, memory_order_release);
    free(h);
}

/* Drain the callback queue and fire pending callbacks (loop thread). */
static void drain_cb_queue(void)
{
    goc_entry *e;
    while ((e = cb_queue_pop()) != NULL) {
        if (e->kind == GOC_CALLBACK) {
            if (e->cb)
                e->cb(e->cb_result, e->ok, e->ud);
            else if (e->put_cb)
                e->put_cb(e->ok, e->ud);
        }
    }
}

/* on_wakeup — loop thread; drains g_cb_queue and fires callbacks. */
static void on_wakeup(uv_async_t *h)
{
    (void)h;
    drain_cb_queue();
}

/* on_shutdown_signal — loop thread; fires remaining callbacks, then closes
 * both async handles.  Closing all handles causes uv_run to exit. */
static void on_shutdown_signal(uv_async_t *h)
{
    (void)h;

    /* Fire any remaining pending callbacks before closing. */
    drain_cb_queue();

    /* Close wakeup handle; on_wakeup_closed will NULL g_wakeup and free. */
    uv_close((uv_handle_t *)g_wakeup_raw, on_wakeup_closed);

    /* Close the shutdown async handle itself. */
    uv_close((uv_handle_t *)g_shutdown_async, free_handle_cb);
}

/* --------------------------------------------------------------------------
 * Loop thread entry point
 * -------------------------------------------------------------------------- */

static void *loop_thread_fn(void *arg)
{
    (void)arg;
    while (uv_run(g_loop, UV_RUN_ONCE)) {}
    return NULL;
}

/* --------------------------------------------------------------------------
 * loop_init / loop_shutdown — internal (declared in internal.h)
 * -------------------------------------------------------------------------- */

void loop_init(void)
{
    /* 1. Allocate and initialise the event loop. */
    g_loop = (uv_loop_t *)malloc(sizeof(uv_loop_t));
    assert(g_loop);
    uv_loop_init(g_loop);

    /* 2. Wakeup async handle. */
    g_wakeup_raw = (uv_async_t *)malloc(sizeof(uv_async_t));
    assert(g_wakeup_raw);
    uv_async_init(g_loop, g_wakeup_raw, on_wakeup);

    /* 3. Publish wakeup pointer atomically. */
    atomic_store_explicit(&g_wakeup, g_wakeup_raw, memory_order_release);

    /* 4. Shutdown async handle. */
    g_shutdown_async = (uv_async_t *)malloc(sizeof(uv_async_t));
    assert(g_shutdown_async);
    uv_async_init(g_loop, g_shutdown_async, on_shutdown_signal);

    /* 5. Initialise the MPSC callback queue. */
    cb_queue_init();

    /* 6. Spawn the loop thread (GC-registered on all platforms via
     *    gc_pthread_create — see internal.h). */
    gc_pthread_create(&g_loop_thread, NULL, loop_thread_fn, NULL);
}

void loop_shutdown(void)
{
    /* Signal the loop thread to drain, close handles, and exit. */
    uv_async_send(g_shutdown_async);

    /* Wait for the loop thread to finish. */
    gc_pthread_join(g_loop_thread, NULL);

    /* All handles are closed; the loop should be idle. */
    assert(uv_loop_close(g_loop) == 0);
    free(g_loop);
    g_loop = NULL;
}
