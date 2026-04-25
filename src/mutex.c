/*
 * src/mutex.c
 *
 * Go-like RW mutexes implemented with channel-backed lock handles.
 *
 * API contract:
 *   goc_mutex* mx = goc_mutex_make();
 *   goc_chan*  lk = goc_read_lock(mx);   // or goc_write_lock(mx)
 *   goc_take(lk) / goc_take_sync(lk);    // wait until lock is granted
 *   goc_close(lk);                       // release lock
 */

#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <stdatomic.h>
#include "../include/goc.h"
#include "channel_internal.h"

typedef struct goc_mutex_release_ctx goc_mutex_release_ctx;

typedef struct goc_mutex_waiter {
    struct goc_mutex_waiter* next;
    goc_chan*                gate;
    int                      is_writer;
    goc_mutex_release_ctx*   ctx;
} goc_mutex_waiter;

struct goc_mutex {
    uv_mutex_t*       lock;
    size_t            active_readers;
    int               writer_active;
    size_t            waiting_writers;
    goc_mutex_waiter* q_head;
    goc_mutex_waiter* q_tail;
};

struct goc_mutex_release_ctx {
    goc_mutex* mx;
    goc_mutex_waiter* waiter;
    int        is_writer;
    _Atomic int acquired;
    _Atomic int handled;
};

/* ---------------------------------------------------------------------------
 * Global registry for goc_mutex internal locks
 * --------------------------------------------------------------------------- */

static goc_mutex** g_live_mutexes     = NULL;
static size_t      g_live_mutexes_len = 0;
static size_t      g_live_mutexes_cap = 0;
static uv_mutex_t  g_live_mutexes_lock;

void mutex_registry_init(void)
{
    g_live_mutexes_cap = 32;
    g_live_mutexes = malloc(g_live_mutexes_cap * sizeof(goc_mutex*));
    assert(g_live_mutexes != NULL);
    g_live_mutexes_len = 0;
    uv_mutex_init(&g_live_mutexes_lock);
}

static void mutex_register(goc_mutex* mx)
{
    uv_mutex_lock(&g_live_mutexes_lock);

    if (g_live_mutexes_len == g_live_mutexes_cap) {
        size_t new_cap = g_live_mutexes_cap * 2;
        goc_mutex** grown = realloc(g_live_mutexes, new_cap * sizeof(goc_mutex*));
        assert(grown != NULL);
        g_live_mutexes = grown;
        g_live_mutexes_cap = new_cap;
    }

    g_live_mutexes[g_live_mutexes_len++] = mx;

    uv_mutex_unlock(&g_live_mutexes_lock);
}

void mutex_registry_destroy_all(void)
{
    for (size_t i = 0; i < g_live_mutexes_len; i++) {
        goc_mutex* mx = g_live_mutexes[i];
        uv_mutex_destroy(mx->lock);
        free(mx->lock);
    }

    free(g_live_mutexes);
    g_live_mutexes = NULL;
    g_live_mutexes_len = 0;
    g_live_mutexes_cap = 0;

    uv_mutex_destroy(&g_live_mutexes_lock);
}

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */

static void mutex_enqueue(goc_mutex* mx, goc_mutex_waiter* w)
{
    w->next = NULL;
    if (mx->q_tail == NULL) {
        mx->q_head = w;
        mx->q_tail = w;
    } else {
        mx->q_tail->next = w;
        mx->q_tail = w;
    }
}

static goc_mutex_waiter* mutex_dequeue(goc_mutex* mx)
{
    goc_mutex_waiter* w = mx->q_head;
    if (w == NULL)
        return NULL;

    mx->q_head = w->next;
    if (mx->q_head == NULL)
        mx->q_tail = NULL;

    w->next = NULL;
    return w;
}

static void mutex_send_grant(goc_chan* gate)
{
    if (goc_in_fiber())
        (void)goc_put(gate, (void*)1);
    else
        (void)goc_put_sync(gate, (void*)1);
}

static void mutex_release(goc_mutex* mx, int is_writer)
{
    goc_mutex_waiter* grant_head = NULL;
    goc_mutex_waiter* grant_tail = NULL;

    uv_mutex_lock(mx->lock);

    if (is_writer) {
        mx->writer_active = 0;
    } else if (mx->active_readers > 0) {
        mx->active_readers--;
    }

    if (!mx->writer_active && mx->active_readers == 0 && mx->q_head != NULL) {
        if (mx->q_head->is_writer) {
            goc_mutex_waiter* w = mutex_dequeue(mx);
            mx->waiting_writers--;
            mx->writer_active = 1;
            atomic_store_explicit(&w->ctx->acquired, 1, memory_order_release);
            grant_head = grant_tail = w;
        } else {
            while (mx->q_head != NULL && !mx->q_head->is_writer) {
                goc_mutex_waiter* r = mutex_dequeue(mx);
                mx->active_readers++;
                atomic_store_explicit(&r->ctx->acquired, 1, memory_order_release);
                if (grant_tail == NULL) {
                    grant_head = grant_tail = r;
                } else {
                    grant_tail->next = r;
                    grant_tail = r;
                }
            }
        }
    }

    uv_mutex_unlock(mx->lock);

    for (goc_mutex_waiter* it = grant_head; it != NULL; it = it->next)
        mutex_send_grant(it->gate);
}

static void mutex_cancel_waiter(goc_mutex* mx, goc_mutex_waiter* target)
{
    uv_mutex_lock(mx->lock);

    goc_mutex_waiter* prev = NULL;
    goc_mutex_waiter* cur = mx->q_head;
    while (cur != NULL) {
        if (cur == target) {
            if (prev == NULL)
                mx->q_head = cur->next;
            else
                prev->next = cur->next;

            if (mx->q_tail == cur)
                mx->q_tail = prev;

            if (cur->is_writer && mx->waiting_writers > 0)
                mx->waiting_writers--;

            break;
        }
        prev = cur;
        cur = cur->next;
    }

    uv_mutex_unlock(mx->lock);
}

static void mutex_on_lock_channel_closed(void* ud)
{
    goc_mutex_release_ctx* ctx = (goc_mutex_release_ctx*)ud;
    if (atomic_exchange_explicit(&ctx->handled, 1, memory_order_acq_rel))
        return;

    if (atomic_load_explicit(&ctx->acquired, memory_order_acquire))
        mutex_release(ctx->mx, ctx->is_writer);
    else
        mutex_cancel_waiter(ctx->mx, ctx->waiter);
}

static goc_chan* mutex_lock_request(goc_mutex* mx, int is_writer)
{
    goc_mutex_waiter* w = goc_new(goc_mutex_waiter);
    w->gate = goc_chan_make(1);
    w->is_writer = is_writer;
    w->next = NULL;

    goc_mutex_release_ctx* ctx = goc_new(goc_mutex_release_ctx);
    ctx->mx = mx;
    ctx->waiter = w;
    ctx->is_writer = is_writer;
    atomic_store_explicit(&ctx->acquired, 0, memory_order_release);
    atomic_store_explicit(&ctx->handled, 0, memory_order_release);
    w->ctx = ctx;
    chan_set_on_close(w->gate, mutex_on_lock_channel_closed, ctx);

    int grant_now = 0;

    uv_mutex_lock(mx->lock);

    if (is_writer) {
        if (!mx->writer_active && mx->active_readers == 0 && mx->q_head == NULL) {
            mx->writer_active = 1;
            atomic_store_explicit(&ctx->acquired, 1, memory_order_release);
            grant_now = 1;
        } else {
            mutex_enqueue(mx, w);
            mx->waiting_writers++;
        }
    } else {
        if (!mx->writer_active && mx->waiting_writers == 0 && mx->q_head == NULL) {
            mx->active_readers++;
            atomic_store_explicit(&ctx->acquired, 1, memory_order_release);
            grant_now = 1;
        } else {
            mutex_enqueue(mx, w);
        }
    }

    uv_mutex_unlock(mx->lock);

    if (grant_now && !atomic_load_explicit(&ctx->handled, memory_order_acquire))
        mutex_send_grant(w->gate);

    return w->gate;
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

goc_mutex* goc_mutex_make(void)
{
    goc_mutex* mx = goc_new(goc_mutex);
    mx->lock = malloc(sizeof(uv_mutex_t));
    uv_mutex_init(mx->lock);

    mutex_register(mx);
    return mx;
}

goc_chan* goc_read_lock(goc_mutex* mx)
{
    return mutex_lock_request(mx, 0);
}

goc_chan* goc_write_lock(goc_mutex* mx)
{
    return mutex_lock_request(mx, 1);
}
