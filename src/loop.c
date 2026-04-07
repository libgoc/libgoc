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
#include <sched.h>
#include <uv.h>
#include <gc.h>
#include "../include/goc.h"
#include "internal.h"
#include "channel_internal.h"

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
static uv_thread_t    g_loop_thread;
static mpsc_queue_t   g_cb_queue;
static uv_mutex_t     g_loop_submit_lock;

typedef enum {
    GOC_LOOP_STATE_STOPPED = 0,
    GOC_LOOP_STATE_RUNNING = 1,
    GOC_LOOP_STATE_SHUTTING_DOWN = 2,
} goc_loop_state_t;

static _Atomic int g_loop_state = GOC_LOOP_STATE_STOPPED;

/* Loop-thread drain reentrancy guard.
 * on_wakeup may trigger callbacks that enqueue more work; avoid recursively
 * re-entering drain_cb_queue on the same thread frame. */
static _Thread_local int g_in_cb_drain = 0;

/* Forward declarations used by post_callback fast-path. */
static void drain_cb_queue(void);

static int on_loop_thread(void)
{
    uv_thread_t self = uv_thread_self();
    return uv_thread_equal(&self, &g_loop_thread);
}

static void drain_cb_queue_guarded(void)
{
    if (g_in_cb_drain)
        return;
    g_in_cb_drain = 1;
    drain_cb_queue();
    g_in_cb_drain = 0;
}

/* Callback queue depth tracking — relaxed atomics; zero hot-path overhead */
static _Atomic size_t g_cb_queue_depth = 0;
static _Atomic size_t g_cb_queue_hwm   = 0;

/* Bound callback processing per wakeup to avoid starving libuv I/O/timers
 * when callback producers are very hot (e.g. debug-heavy HTTP stress). */
#define GOC_CB_DRAIN_BUDGET 512

/* --------------------------------------------------------------------------
 * MPSC queue implementation
 * -------------------------------------------------------------------------- */

static void cb_queue_init(void)
{
    mpsc_node *sentinel = (mpsc_node *)malloc(sizeof(mpsc_node));
    atomic_store_explicit(&sentinel->next, NULL, memory_order_relaxed);
    sentinel->entry = NULL;
    atomic_store_explicit(&g_cb_queue.head, sentinel, memory_order_relaxed);
    g_cb_queue.tail = sentinel;
}

/* Enqueue an entry onto g_cb_queue (producer — any thread).
 * Returns the queue depth *before* this push (0 means queue was empty). */
static size_t cb_queue_push(mpsc_node *node)
{
    atomic_store_explicit(&node->next, NULL, memory_order_relaxed);
    /* Vyukov: exchange head and link previous head's next to this node. */
    mpsc_node *prev = atomic_exchange_explicit(&g_cb_queue.head, node,
                                               memory_order_acq_rel);
    atomic_store_explicit(&prev->next, node, memory_order_release);

    /* Update depth and high-water mark (relaxed — approximate is fine). */
    size_t prev_depth = atomic_fetch_add_explicit(&g_cb_queue_depth, 1, memory_order_relaxed);
    size_t depth = prev_depth + 1;
    size_t hwm   = atomic_load_explicit(&g_cb_queue_hwm, memory_order_relaxed);
    while (depth > hwm) {
        if (atomic_compare_exchange_weak_explicit(&g_cb_queue_hwm, &hwm, depth,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed))
            break;
    }
    return prev_depth;
}

/* Dequeue from g_cb_queue (consumer — loop thread only).
 * Returns the entry or NULL if the queue is empty. */
static goc_entry *cb_queue_pop(void)
{
    mpsc_node *tail = g_cb_queue.tail;
    mpsc_node *next = atomic_load_explicit(&tail->next, memory_order_acquire);
    if (!next)
        return NULL;
    g_cb_queue.tail = next;         /* advance tail past sentinel */
    goc_entry *e = next->entry;
    free(tail);                     /* old sentinel; malloc'd, not GC-tracked */
    atomic_fetch_sub_explicit(&g_cb_queue_depth, 1, memory_order_relaxed);
    return e;
}

/* Accessor — safe to call from any thread at any time. */
size_t goc_cb_queue_get_hwm(void)
{
    return atomic_load_explicit(&g_cb_queue_hwm, memory_order_relaxed);
}

/* --------------------------------------------------------------------------
 * post_callback — called from any thread (pool workers, loop thread)
 * -------------------------------------------------------------------------- */

void post_callback(goc_entry *entry, void *value)
{
    /* For take callbacks: store the delivered value. */
    entry->cb_result = value;

    mpsc_node *node = (mpsc_node *)malloc(sizeof(mpsc_node));
    node->entry = entry;
    size_t prev_depth = cb_queue_push(node);

    uv_async_t *w = atomic_load_explicit(&g_wakeup, memory_order_acquire);
    GOC_DBG("post_callback: entry=%p ch=%p is_put=%d wakeup=%p prev_depth=%zu\n",
            (void*)entry, (void*)entry->ch, (int)entry->is_put, (void*)w, prev_depth);

    /* Coalescing: skip uv_async_send when the queue was already non-empty.
     * A prior send is already in flight; on_wakeup will drain everything
     * enqueued up to that point, including this entry. */
    if (w && prev_depth == 0) {
        int rc = uv_async_send(w);
        GOC_DBG("post_callback: SENT rc=%d wakeup=%p\n", rc, (void*)w);
        if (rc < 0) {
            GOC_DBG("post_callback: uv_async_send FAILED wakeup=%p rc=%d\n", (void*)w, rc);
        }
    } else if (!w && on_loop_thread()) {
        /* Shutdown/teardown edge: wakeup handle may already be gone. Drain
         * directly so loop-thread callbacks are not stranded. */
        drain_cb_queue_guarded();
    }
}

/* --------------------------------------------------------------------------
 * post_on_loop — dispatch a function to run on the loop thread
 * -------------------------------------------------------------------------- */

typedef struct {
    void (*fn)(void*);
    void* arg;
    goc_entry e;   /* embedded entry — freed together with task */
} goc_loop_task_t;

static void on_loop_task_cb(void* val, goc_status_t ok, void* ud)
{
    (void)val;
    (void)ok;
    goc_loop_task_t* task = (goc_loop_task_t*)ud;
    task->fn(task->arg);
    free(task);
}

void post_on_loop(void (*fn)(void*), void* arg)
{
    goc_loop_task_t* task = (goc_loop_task_t*)malloc(sizeof(goc_loop_task_t));
    task->fn = fn;
    task->arg = arg;
    task->e = (goc_entry){ 0 };
    task->e.kind = GOC_CALLBACK;
    task->e.cb   = on_loop_task_cb;
    task->e.ud   = task;
    post_callback(&task->e, NULL);
}

int post_on_loop_checked(void (*fn)(void*), void* arg)
{
    int rc = 0;

    uv_mutex_lock(&g_loop_submit_lock);
    if (atomic_load_explicit(&g_loop_state, memory_order_acquire) !=
        GOC_LOOP_STATE_RUNNING) {
        rc = UV_ECANCELED;
    } else {
        goc_loop_task_t* task =
            (goc_loop_task_t*)malloc(sizeof(goc_loop_task_t));
        task->fn  = fn;
        task->arg = arg;
        task->e   = (goc_entry){ 0 };
        task->e.kind = GOC_CALLBACK;
        task->e.cb   = on_loop_task_cb;
        task->e.ud   = task;
        post_callback(&task->e, NULL);
    }
    uv_mutex_unlock(&g_loop_submit_lock);

    if (rc < 0) {
        GOC_DBG("post_on_loop_checked: rejected fn=%p arg=%p state=%d rc=%d\n",
                (void*)fn, arg,
                atomic_load_explicit(&g_loop_state, memory_order_relaxed),
                rc);
    }
    return rc;
}

int goc_loop_is_shutting_down(void)
{
    return atomic_load_explicit(&g_loop_state, memory_order_acquire) !=
           GOC_LOOP_STATE_RUNNING;
}

static void free_handle_cb(uv_handle_t *h);  /* defined in loop-thread callbacks section below */

/* --------------------------------------------------------------------------
 * Central timer manager
 *
 * Replaces per-timeout uv_timer_t allocation with a single long-lived
 * uv_timer_t driven by a GC-allocated min-heap of pending timeouts.
 *
 * The heap array is goc_malloc'd so the GC can trace goc_timeout_timer_ctx*
 * pointers inside and keep ctx objects alive while they are pending — no
 * per-timeout gc_handle_register call is needed.
 *
 * All operations run on the loop thread (no extra lock required).
 * -------------------------------------------------------------------------- */

struct timer_heap_entry {
    uint64_t               deadline_ns;
    goc_timeout_timer_ctx* ctx;
};

#define TIMER_HEAP_INIT_CAP 64

static timer_heap_entry_t* g_timer_heap     = NULL;  /* goc_malloc'd */
static size_t               g_timer_heap_len = 0;
static size_t               g_timer_heap_cap = 0;
static uv_timer_t*          g_timer_mgr      = NULL;  /* malloc'd; one per loop */

static void on_timer_mgr_fire(uv_timer_t* h);  /* forward declaration */

static void timer_heap_swap(size_t a, size_t b)
{
    timer_heap_entry_t tmp = g_timer_heap[a];
    g_timer_heap[a]        = g_timer_heap[b];
    g_timer_heap[b]        = tmp;
    g_timer_heap[a].ctx->heap_idx = a;
    g_timer_heap[b].ctx->heap_idx = b;
}

static void timer_heap_sift_up(size_t i)
{
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (g_timer_heap[parent].deadline_ns <= g_timer_heap[i].deadline_ns)
            break;
        timer_heap_swap(parent, i);
        i = parent;
    }
}

static void timer_heap_sift_down(size_t i)
{
    for (;;) {
        size_t smallest = i;
        size_t left  = 2 * i + 1;
        size_t right = 2 * i + 2;
        if (left  < g_timer_heap_len &&
            g_timer_heap[left].deadline_ns  < g_timer_heap[smallest].deadline_ns)
            smallest = left;
        if (right < g_timer_heap_len &&
            g_timer_heap[right].deadline_ns < g_timer_heap[smallest].deadline_ns)
            smallest = right;
        if (smallest == i)
            break;
        timer_heap_swap(i, smallest);
        i = smallest;
    }
}

/* Re-arm the backing uv_timer_t to fire at the earliest pending deadline,
 * or stop it if the heap is empty. */
static void timer_mgr_rearm(void)
{
    if (g_timer_heap_len == 0) {
        uv_timer_stop(g_timer_mgr);
        return;
    }
    uint64_t now_ns   = uv_hrtime();
    uint64_t next_ns  = g_timer_heap[0].deadline_ns;
    uint64_t delay_ms = (next_ns > now_ns)
                      ? ((next_ns - now_ns + 999999ULL) / 1000000ULL)
                      : 0;
    uv_timer_start(g_timer_mgr, on_timer_mgr_fire, delay_ms, 0);
}

/* Loop-thread callback: fires all expired entries and re-arms. */
static void on_timer_mgr_fire(uv_timer_t* h)
{
    (void)h;
    uint64_t now_ns = uv_hrtime();
    GOC_DBG("on_timer_mgr_fire: now_ns=%llu heap_len=%zu\n",
            (unsigned long long)now_ns, g_timer_heap_len);

    while (g_timer_heap_len > 0 && g_timer_heap[0].deadline_ns <= now_ns) {
        goc_timeout_timer_ctx* ctx = g_timer_heap[0].ctx;
        GOC_DBG("on_timer_mgr_fire: expiring tctx=%p ch=%p deadline=%llu\n",
                (void*)ctx, (void*)ctx->ch,
                (unsigned long long)g_timer_heap[0].deadline_ns);

        /* Remove root from heap (swap with last element, then sift down). */
        ctx->heap_idx = SIZE_MAX;
        size_t last = --g_timer_heap_len;
        if (last > 0) {
            g_timer_heap[0] = g_timer_heap[last];
            g_timer_heap[0].ctx->heap_idx = 0;
            timer_heap_sift_down(0);
        }

        /* Mark closed before firing so on_cancel_timer (if posted) is a no-op. */
        atomic_store_explicit(&ctx->start_state, 2, memory_order_release);
        goc_timeout_ctx_expire(ctx);
    }

    timer_mgr_rearm();
}

/* Public: insert ctx into the heap; re-arm the backing timer if needed.
 * Called from on_start_timer (loop thread only). */
goc_timer_manager_t* goc_global_timer_mgr(void)
{
    static goc_timer_manager_t g_mgr;
    return &g_mgr;
}

void goc_timer_manager_insert(goc_timer_manager_t* mgr,
                              goc_timeout_timer_ctx* ctx,
                              uint64_t deadline_ns)
{
    /* Grow the GC-managed heap array if full. */
    (void)mgr;
    if (g_timer_heap_len == g_timer_heap_cap) {
        size_t new_cap = g_timer_heap_cap ? g_timer_heap_cap * 2 : TIMER_HEAP_INIT_CAP;
        g_timer_heap = (timer_heap_entry_t*)goc_realloc(
                            g_timer_heap, new_cap * sizeof(timer_heap_entry_t));
        g_timer_heap_cap = new_cap;
    }

    size_t i = g_timer_heap_len++;
    g_timer_heap[i].deadline_ns = deadline_ns;
    g_timer_heap[i].ctx         = ctx;
    ctx->heap_idx               = i;
    timer_heap_sift_up(i);

    /* Re-arm only when the new entry became the new earliest deadline. */
    if (ctx->heap_idx == 0)
        timer_mgr_rearm();
}

/* Public: remove ctx from the heap (cancel path).
 * Called from on_cancel_timer (loop thread only). */
void goc_timer_manager_remove(goc_timer_manager_t* mgr,
                              goc_timeout_timer_ctx* ctx)
{
    (void)mgr;
    size_t i = ctx->heap_idx;
    if (i == SIZE_MAX || i >= g_timer_heap_len)
        return;

    ctx->heap_idx = SIZE_MAX;

    size_t last = --g_timer_heap_len;
    if (i == last) {
        /* Was the last element; just shrink. Re-arm (may stop if heap now empty). */
        timer_mgr_rearm();
        return;
    }

    /* Replace hole with the last element and restore heap property. */
    goc_timeout_timer_ctx* moved = g_timer_heap[last].ctx;
    g_timer_heap[i] = g_timer_heap[last];
    moved->heap_idx = i;

    timer_heap_sift_up(i);
    /* After sift_up, moved may have risen; find its current position via heap_idx. */
    timer_heap_sift_down(moved->heap_idx);

    /* The root may have changed; re-arm unconditionally. */
    timer_mgr_rearm();
}

/* Initialise the timer manager; called from loop_init after uv_loop_init. */
static void goc_timer_manager_init(void)
{
    g_timer_heap     = (timer_heap_entry_t*)goc_malloc(
                            TIMER_HEAP_INIT_CAP * sizeof(timer_heap_entry_t));
    g_timer_heap_cap = TIMER_HEAP_INIT_CAP;
    g_timer_heap_len = 0;

    g_timer_mgr = (uv_timer_t*)malloc(sizeof(uv_timer_t));
    assert(g_timer_mgr != NULL);
    int rc = uv_timer_init(g_loop, g_timer_mgr);
    GOC_DBG("goc_timer_manager_init: uv_timer_init timer=%p rc=%d\n",
            (void*)g_timer_mgr, rc);
    (void)rc;
    /* Do not start the timer yet; it will be armed on the first insert. */
}

/* Drain and close the timer manager; called from on_shutdown_signal (loop thread).
 * Closes all pending timeout channels, then closes the backing uv_timer_t. */
static void goc_timer_manager_shutdown(void)
{
    GOC_DBG("goc_timer_manager_shutdown: draining %zu pending timeouts\n",
            g_timer_heap_len);

    uv_timer_stop(g_timer_mgr);

    for (size_t i = 0; i < g_timer_heap_len; i++) {
        goc_timeout_timer_ctx* ctx = g_timer_heap[i].ctx;
        /* Set state=2 so any in-flight on_cancel_timer callbacks are no-ops. */
        atomic_store_explicit(&ctx->start_state, 2, memory_order_release);
        ctx->heap_idx = SIZE_MAX;
        GOC_DBG("goc_timer_manager_shutdown: closing tctx=%p ch=%p\n",
                (void*)ctx, (void*)ctx->ch);
        goc_close(ctx->ch);
    }
    g_timer_heap_len = 0;

    uv_close((uv_handle_t*)g_timer_mgr, free_handle_cb);
    g_timer_mgr = NULL;
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

static void unregister_gc_handle_cb(uv_handle_t *h)
{
    gc_handle_unregister(h);
}

static void on_shutdown_signal(uv_async_t *h);

static void close_remaining_handle_cb(uv_handle_t *h, void *arg)
{
    (void)arg;
    if (uv_is_closing(h))
        return;

    if (h == (uv_handle_t *)g_wakeup_raw ||
        h == (uv_handle_t *)g_shutdown_async)
        return;

    GOC_DBG("shutdown uv_walk close: handle=%p type=%s active=%d has_ref=%d\n",
            (void*)h,
            uv_handle_type_name(uv_handle_get_type(h)),
            uv_is_active(h),
            uv_has_ref(h));
    uv_close(h, unregister_gc_handle_cb);
}

static void on_wakeup_closed(uv_handle_t *h)
{
    GOC_DBG("on_wakeup_closed: h=%p\n", (void*)h);
    /* Set g_wakeup to NULL only once libuv guarantees no further callbacks
     * will fire for this handle, preventing a race with post_callback. */
    atomic_store_explicit(&g_wakeup, NULL, memory_order_release);
    free(h);
}

/* --------------------------------------------------------------------------
 * loop_process_pending_put / loop_process_pending_take
 *
 * Called on the loop thread to perform the channel operation for a
 * goc_put_cb / goc_take_cb entry that was posted without holding ch->lock.
 * Acquires ch->lock, attempts fast delivery, and parks the entry if no
 * match is available yet.  Fires the optional callback immediately when
 * the operation resolves; otherwise the entry stays parked and wake() will
 * call post_callback() later to fire it.
 * -------------------------------------------------------------------------- */

static void loop_process_pending_put(goc_entry *e)
{
    goc_chan *ch = e->ch;
    e->ch = NULL;   /* clear so a future drain_cb_queue pass skips this entry */

    goc_entry *fe_taker = NULL;

    uv_mutex_lock(ch->lock);

    if (ch->dead_count >= GOC_DEAD_COUNT_THRESHOLD)
        compact_dead_entries(ch);

    GOC_DBG("loop_process_pending_put: ch=%p closed=%d item_count=%zu takers=%p val=%p\n",
            (void*)ch, ch->closed, ch->item_count, (void*)ch->takers, e->put_val);

    /* Walk taker list to verify each entry is accessible before claim. */
    {
        goc_entry *_t = ch->takers;
        int _i = 0;
        while (_t) {
            GOC_DBG("loop_process_pending_put: taker[%d]=%p cancelled=%d woken=%d kind=%d coro=%p\n",
                    _i++, (void*)_t,
                    (int)atomic_load_explicit(&_t->cancelled, memory_order_acquire),
                    (int)atomic_load_explicit(&_t->woken, memory_order_acquire),
                    (int)_t->kind,
                    (_t->kind == GOC_FIBER ? (void*)_t->coro : NULL));
            _t = _t->next;
        }
    }

    /* Closed: always fail (matches goc_put ordering). */
    if (ch->closed) {
        e->ok = GOC_CLOSED;
        GOC_DBG("loop_process_pending_put: ch=%p CLOSED \u2192 dropping put val=%p\n",
                (void*)ch, e->put_val);
        uv_mutex_unlock(ch->lock);
        if (e->put_cb) e->put_cb(GOC_CLOSED, e->ud);
        if (e->free_on_drain) free(e);
        return;
    }

    /* Parked taker available: deliver directly. */
    if (chan_put_to_taker_claim(ch, e->put_val, &fe_taker)) {
        e->ok = GOC_OK;
        GOC_DBG("loop_process_pending_put: ch=%p delivered to parked taker put_cb=%p\n", (void*)ch, (void*)(uintptr_t)e->put_cb);
        uv_mutex_unlock(ch->lock);
        if (fe_taker != NULL) {
            long spin = 0;
            while (atomic_load_explicit(&fe_taker->parked, memory_order_acquire) == 0) {
                sched_yield();
                if (++spin == 10000000L) {
                    GOC_DBG("loop_process_pending_put: SPIN STALL >10M ch=%p fe_taker=%p\n",
                            (void*)ch, (void*)fe_taker);
                }
            }
            GOC_DBG("loop_process_pending_put: posting taker fe=%p pool=%p parked=%d ch=%p\n",
                    (void*)fe_taker, (void*)fe_taker->pool,
                    (int)atomic_load_explicit(&fe_taker->parked, memory_order_acquire),
                    (void*)ch);
            post_to_run_queue(fe_taker->pool, fe_taker);
            GOC_DBG("loop_process_pending_put: posted taker fe=%p ch=%p\n",
                    (void*)fe_taker, (void*)ch);
        }
        if (e->put_cb) e->put_cb(GOC_OK, e->ud);
        if (e->free_on_drain) free(e);
        return;
    }

    /* Buffer space available. */
    if (chan_put_to_buffer(ch, e->put_val)) {
        e->ok = GOC_OK;
        GOC_DBG("loop_process_pending_put: ch=%p buffered val put_cb=%p\n", (void*)ch, (void*)(uintptr_t)e->put_cb);
        uv_mutex_unlock(ch->lock);
        if (e->put_cb) e->put_cb(GOC_OK, e->ud);
        if (e->free_on_drain) free(e);
        return;
    }

    /* No match yet: park.  wake() → post_callback() will fire put_cb later. */
    GOC_DBG("loop_process_pending_put: ch=%p parked (no taker, no buffer space)\n", (void*)ch);
    chan_list_append(&ch->putters, &ch->putters_tail, e);
    uv_mutex_unlock(ch->lock);
}

static void loop_process_pending_take(goc_entry *e)
{
    goc_chan *ch = e->ch;
    e->ch = NULL;   /* clear so a future drain_cb_queue pass skips this entry */

    void *val = NULL;
    goc_entry *fe_putter = NULL;

    uv_mutex_lock(ch->lock);

    if (ch->dead_count >= GOC_DEAD_COUNT_THRESHOLD)
        compact_dead_entries(ch);

    /* Value available from buffer. */
    if (chan_take_from_buffer(ch, &val)) {
        e->cb_result = val;
        e->ok = GOC_OK;
        uv_mutex_unlock(ch->lock);
        if (e->cb) e->cb(val, GOC_OK, e->ud);
        if (e->free_on_drain) free(e);
        return;
    }

    /* Value available from parked putter. */
    if (chan_take_from_putter_claim(ch, &val, &fe_putter)) {
        e->cb_result = val;
        e->ok = GOC_OK;
        uv_mutex_unlock(ch->lock);
        if (fe_putter != NULL) {
            long spin = 0;
            while (atomic_load_explicit(&fe_putter->parked, memory_order_acquire) == 0) {
                sched_yield();
                if (++spin == 10000000L) {
                    GOC_DBG("loop_process_pending_take: SPIN STALL >10M ch=%p fe_putter=%p\n",
                            (void*)ch, (void*)fe_putter);
                }
            }
            GOC_DBG("loop_process_pending_take: posting putter fe=%p pool=%p parked=%d ch=%p\n",
                    (void*)fe_putter, (void*)fe_putter->pool,
                    (int)atomic_load_explicit(&fe_putter->parked, memory_order_acquire),
                    (void*)ch);
            post_to_run_queue(fe_putter->pool, fe_putter);
            GOC_DBG("loop_process_pending_take: posted putter fe=%p ch=%p\n",
                    (void*)fe_putter, (void*)ch);
        }
        if (e->cb) e->cb(val, GOC_OK, e->ud);
        if (e->free_on_drain) free(e);
        return;
    }

    /* Closed and empty. */
    if (ch->closed) {
        e->ok = GOC_CLOSED;
        uv_mutex_unlock(ch->lock);
        if (e->cb) e->cb(NULL, GOC_CLOSED, e->ud);
        if (e->free_on_drain) free(e);
        return;
    }

    /* No match yet: park.  wake() → post_callback() will fire cb later. */
    GOC_DBG("takers_append: ch=%p e=%p kind=%d coro=%p\n",
            (void*)ch, (void*)e, (int)e->kind,
            (e->kind == GOC_FIBER ? (void*)e->coro : NULL));
    chan_list_append(&ch->takers, &ch->takers_tail, e);
    uv_mutex_unlock(ch->lock);
}

/* Drain the callback queue and fire pending callbacks (loop thread). */
static void drain_cb_queue(void)
{
    goc_entry *e;
    int iter = 0;
    size_t budget = GOC_CB_DRAIN_BUDGET;
    while (budget-- > 0 && (e = cb_queue_pop()) != NULL) {
        GOC_DBG("drain_cb_queue: iter=%d popped e=%p kind=%d ch=%p is_put=%d woken=%d\n",
                ++iter, (void*)e, (int)e->kind, (void*)e->ch, (int)e->is_put,
                (int)atomic_load_explicit(&e->woken, memory_order_acquire));
        if (e->kind != GOC_CALLBACK)
            continue;

        /* Pending channel op posted by goc_put_cb / goc_take_cb:
         * e->ch is non-NULL and the entry has not yet been claimed by wake().
         * Process the channel operation here on the loop thread. */
        if (e->ch != NULL &&
            !atomic_load_explicit(&e->woken, memory_order_acquire)) {
            if (e->is_put) {
                GOC_DBG("drain_cb_queue: processing pending PUT e=%p ch=%p\n", (void*)e, (void*)e->ch);
                loop_process_pending_put(e);
                GOC_DBG("drain_cb_queue: processed pending PUT e=%p\n", (void*)e);
            } else {
                GOC_DBG("drain_cb_queue: processing pending TAKE e=%p ch=%p\n", (void*)e, (void*)e->ch);
                loop_process_pending_take(e);
                GOC_DBG("drain_cb_queue: processed pending TAKE e=%p\n", (void*)e);
            }
            continue;
        }

        /* Already claimed by wake(): fire the callback. */
        GOC_DBG("drain_cb_queue: ch=already-cleared woken=1 ok=%d is_put=%d\n",
                (int)e->ok, (int)e->is_put);
        bool fod = e->free_on_drain;
        if (e->cb)
            e->cb(e->cb_result, e->ok, e->ud);
        else if (e->put_cb)
            e->put_cb(e->ok, e->ud);
        if (fod) free(e);
    }

    /* More callbacks queued? Yield back to libuv and request another wakeup
     * pass instead of monopolizing the loop thread in one giant drain. */
    if (atomic_load_explicit(&g_cb_queue_depth, memory_order_relaxed) > 0) {
        uv_async_t *w = atomic_load_explicit(&g_wakeup, memory_order_acquire);
        if (w)
            uv_async_send(w);
    }
}

/* on_wakeup — loop thread; drains g_cb_queue and fires callbacks. */
static void on_wakeup(uv_async_t *h)
{
    (void)h;
    GOC_DBG("on_wakeup: draining cb_queue depth=%zu\n",
            atomic_load_explicit(&g_cb_queue_depth, memory_order_relaxed));
    drain_cb_queue_guarded();
    size_t remaining_depth = atomic_load_explicit(&g_cb_queue_depth, memory_order_relaxed);
    GOC_DBG("on_wakeup: drain done remaining_depth=%zu\n", remaining_depth);

}

/* on_shutdown_signal — loop thread; fires remaining callbacks, then closes
 * both async handles.  Closing all handles causes uv_run to exit. */
static void on_shutdown_signal(uv_async_t *h)
{
    (void)h;
    GOC_DBG("on_shutdown_signal: entered\n");

    uv_mutex_lock(&g_loop_submit_lock);
    atomic_store_explicit(&g_loop_state, GOC_LOOP_STATE_SHUTTING_DOWN,
                          memory_order_release);
    uv_mutex_unlock(&g_loop_submit_lock);

    /* Fire any remaining pending callbacks before closing. */
    GOC_DBG("on_shutdown_signal: before drain_cb_queue\n");
    drain_cb_queue_guarded();
    GOC_DBG("on_shutdown_signal: after drain_cb_queue\n");

    /* Shut down the central timer manager: close remaining timeout channels
     * and close the backing uv_timer_t before uv_walk sweeps up other handles. */
    GOC_DBG("on_shutdown_signal: timer manager shutdown\n");
    goc_timer_manager_shutdown();

    /* Close wakeup handle; on_wakeup_closed will NULL g_wakeup and free. */
    GOC_DBG("on_shutdown_signal: closing wakeup handle=%p\n", (void*)g_wakeup_raw);
    uv_close((uv_handle_t *)g_wakeup_raw, on_wakeup_closed);
    GOC_DBG("on_shutdown_signal: wakeup close queued\n");

    /* Close the shutdown async handle itself. */
    GOC_DBG("on_shutdown_signal: closing shutdown handle=%p\n", (void*)g_shutdown_async);
    uv_close((uv_handle_t *)g_shutdown_async, free_handle_cb);
    GOC_DBG("on_shutdown_signal: shutdown close queued\n");

    GOC_DBG("on_shutdown_signal: uv_walk closing remaining handles\n");
    uv_walk(g_loop, close_remaining_handle_cb, NULL);

    GOC_DBG("on_shutdown_signal: exit\n");
}

/* --------------------------------------------------------------------------
 * Loop thread entry point
 * -------------------------------------------------------------------------- */

static void loop_thread_fn(void *arg)
{
    (void)arg;
    long iter = 0;
    static _Atomic int last_loop_iter = 0;
    while (1) {
        ++iter;
        if (iter % 500 == 0) {
            GOC_DBG("loop_thread_fn: before UV_RUN iter=%ld alive=%d\n",
                    iter, uv_loop_alive(g_loop));
        }
        int ret = uv_run(g_loop, UV_RUN_ONCE);
        if (iter % 500 == 0) {
            GOC_DBG("loop_thread_fn: after UV_RUN iter=%ld ret=%d depth=%zu\n",
                    iter, ret, atomic_load_explicit(&g_cb_queue_depth, memory_order_relaxed));
        }
        /* Safe point: event pass complete, no libuv callbacks running, no
         * ch->lock held.  Drive incremental GC collection. */
        GC_collect_a_little();
        atomic_store_explicit(&last_loop_iter, iter, memory_order_relaxed);
        if (!ret) break;
    }
    GOC_DBG("loop_thread_fn: loop EXITED iter=%ld\n", iter);
}

/* --------------------------------------------------------------------------
 * loop_init / loop_shutdown — internal (declared in internal.h)
 * -------------------------------------------------------------------------- */

void loop_init(void)
{
    uv_mutex_init(&g_loop_submit_lock);
    atomic_store_explicit(&g_loop_state, GOC_LOOP_STATE_STOPPED,
                          memory_order_relaxed);

    /* 1. Allocate and initialise the event loop. */
    g_loop = (uv_loop_t *)malloc(sizeof(uv_loop_t));
    assert(g_loop);
    uv_loop_init(g_loop);

    /* 2. Wakeup async handle. */
    g_wakeup_raw = (uv_async_t *)malloc(sizeof(uv_async_t));
    assert(g_wakeup_raw);
    int rc = uv_async_init(g_loop, g_wakeup_raw, on_wakeup);
    GOC_DBG("loop_init: uv_async_init wakeup=%p rc=%d\n", (void*)g_wakeup_raw, rc);

    /* 3. Publish wakeup pointer atomically. */
    atomic_store_explicit(&g_wakeup, g_wakeup_raw, memory_order_release);
    GOC_DBG("loop_init: published g_wakeup=%p\n", (void*)g_wakeup_raw);

    /* 4. Shutdown async handle. */
    g_shutdown_async = (uv_async_t *)malloc(sizeof(uv_async_t));
    assert(g_shutdown_async);
    rc = uv_async_init(g_loop, g_shutdown_async, on_shutdown_signal);
    GOC_DBG("loop_init: uv_async_init shutdown=%p rc=%d\n", (void*)g_shutdown_async, rc);

    /* 5. Initialise the MPSC callback queue. */
    cb_queue_init();

    /* 6. Initialise the central timer manager (one uv_timer_t for all timeouts). */
    goc_timer_manager_init();

    atomic_store_explicit(&g_loop_state, GOC_LOOP_STATE_RUNNING,
                          memory_order_release);

    /* 7. Spawn the loop thread (GC-registered on all platforms via
     *    goc_thread_create — see internal.h). */
    goc_thread_create(&g_loop_thread, loop_thread_fn, NULL);
}

void loop_shutdown(void)
{
    GOC_DBG("loop_shutdown: begin depth=%zu hwm=%zu state=%d\n",
            atomic_load_explicit(&g_cb_queue_depth, memory_order_relaxed),
            atomic_load_explicit(&g_cb_queue_hwm, memory_order_relaxed),
            atomic_load_explicit(&g_loop_state, memory_order_relaxed));
    GOC_DBG("loop_shutdown: sending shutdown async\n");

    uv_mutex_lock(&g_loop_submit_lock);
    atomic_store_explicit(&g_loop_state, GOC_LOOP_STATE_SHUTTING_DOWN,
                          memory_order_release);
    uv_mutex_unlock(&g_loop_submit_lock);

    /* Signal the loop thread to drain, close handles, and exit. */
    int rcs = uv_async_send(g_shutdown_async);
    GOC_DBG("loop_shutdown: shutdown send rc=%d\n", rcs);

    GOC_DBG("loop_shutdown: joining loop thread\n");
    /* Wait for the loop thread to finish. */
    goc_thread_join(&g_loop_thread);
        GOC_DBG("loop_shutdown: joined depth=%zu hwm=%zu\n",
            atomic_load_explicit(&g_cb_queue_depth, memory_order_relaxed),
            atomic_load_explicit(&g_cb_queue_hwm, memory_order_relaxed));
        GOC_DBG("loop_shutdown: done\n");

    /* All handles are closed; the loop should be idle. */
    assert(uv_loop_close(g_loop) == 0);
    uv_mutex_destroy(&g_loop_submit_lock);
    atomic_store_explicit(&g_loop_state, GOC_LOOP_STATE_STOPPED,
                          memory_order_release);
    free(g_loop);
    g_loop = NULL;
}
