/*
 * src/internal.h
 *
 * Internal types, structs, inline helpers, and forward declarations shared
 * across .c modules. Never included by include/goc.h or by any consumer of
 * the public API.
 */

#ifndef GOC_INTERNAL_H
#define GOC_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <semaphore.h>
#include <uv.h>
#include <gc.h>
#include "minicoro.h"
#include "../include/goc.h"
#include "config.h"
#include "chan_type.h"

/* ---------------------------------------------------------------------------
 * Forward Declarations
 *
 * goc_chan  — full definition pulled in via chan_type.h above.
 * goc_entry — forward-declared in chan_type.h; full definition below.
 * goc_pool  — opaque; full definition lives only in pool.c.
 * --------------------------------------------------------------------------- */

typedef struct goc_pool goc_pool;   /* defined in pool.c */

/* ---------------------------------------------------------------------------
 * goc_entry_kind
 * --------------------------------------------------------------------------- */

typedef enum {
    GOC_FIBER,
    GOC_CALLBACK,
    GOC_SYNC,
} goc_entry_kind;

/* ---------------------------------------------------------------------------
 * goc_entry
 *
 * Represents a single parked waiter on a channel (taker or putter).
 * GC-heap allocated.
 * --------------------------------------------------------------------------- */

struct goc_entry {
    goc_entry_kind     kind;
    _Atomic int        cancelled;       /* set to 1 to cancel this entry (alts loser path) */
    _Atomic int        woken;           /* CAS 0→1 to claim wake; only one winner */
    _Atomic int*       fired;           /* pointer to the shared fired flag for goc_alts */
    struct goc_entry*  next;            /* intrusive linked list (takers / putters) */

    /* Fiber / pool context */
    goc_pool*          pool;            /* pool on which the fiber runs */
    mco_coro*          coro;            /* the fiber's coroutine handle */
    uint32_t*          stack_canary_ptr;/* points to lowest stack word; checked before resume */

    /* Result delivery */
    void**             result_slot;     /* where the delivered value is written */
    goc_status_t       ok;             /* GOC_OK / GOC_CLOSED / GOC_EMPTY */
    size_t             arm_idx;        /* which goc_alts arm this entry represents */

    /* Fiber launch fields */
    void             (*fn)(void*);
    void*              fn_arg;
    goc_chan*          join_ch;

    /* Callback fields (GOC_CALLBACK) */
    void             (*cb)(void* val, goc_status_t ok, void* ud);   /* take callback */
    void             (*put_cb)(goc_status_t ok, void* ud);          /* put callback */
    void*              ud;
    void*              cb_result;   /* value delivered to take callback; reused as result_slot target for SYNC */
    void*              put_val;     /* value the putter wants to send */

    /* Sync fields (GOC_SYNC) */
    sem_t              sync_sem;
    sem_t*             sync_sem_ptr;   /* points to sync_sem (own) or a shared sem in goc_alts_sync */
};

/* ---------------------------------------------------------------------------
 * Canary Constant
 *
 * Written to the lowest stack word after mco_create.
 * Checked before every mco_resume in pool_worker_fn.
 * --------------------------------------------------------------------------- */

#define GOC_STACK_CANARY  0xDEADC0DEu

/* ---------------------------------------------------------------------------
 * Internal Function Declarations (cross-module)
 *
 * wake must be declared before the inline ring-buffer helpers because those
 * helpers call wake internally.
 * --------------------------------------------------------------------------- */

/* channel.c → used by alts.c, fiber.c */
void wake(goc_chan* ch, goc_entry* e, void* value);
void compact_dead_entries(goc_chan* ch);

/* gc.c → used by channel.c */
void chan_register(goc_chan* ch);
void chan_unregister(goc_chan* ch);

/* pool.c → used by fiber.c, channel.c */
void post_to_run_queue(goc_pool* pool, goc_entry* entry);
void pool_fiber_born(goc_pool* pool);

/* loop.c → used by channel.c, alts.c, timeout.c, gc.c */
void loop_init(void);
void loop_shutdown(void);
void post_callback(goc_entry* entry, void* value);

/* gc.c → used by pool.c, loop.c */
void live_channels_init(void);
void pool_registry_init(void);

/* pool.c → used by gc.c */
void pool_registry_destroy_all(void);

/* ---------------------------------------------------------------------------
 * Inline Ring-Buffer Helpers
 *
 * These operate on goc_chan fields. Because internal.h includes chan_type.h,
 * the full struct goc_chan definition is always in scope when these helpers
 * are compiled. Any .c file that includes internal.h can use these helpers
 * without any additional include.
 * --------------------------------------------------------------------------- */

static inline int chan_put_to_buffer(goc_chan* ch, void* val) {
    if (ch->buf_count >= ch->buf_size) return 0;
    size_t tail = (ch->buf_head + ch->buf_count) % ch->buf_size;
    ch->buf[tail] = val;
    ch->buf_count++;
    return 1;
}

static inline int chan_take_from_buffer(goc_chan* ch, void** out) {
    if (ch->buf_count == 0) return 0;
    *out = ch->buf[ch->buf_head];
    ch->buf_head = (ch->buf_head + 1) % ch->buf_size;
    ch->buf_count--;
    return 1;
}

static inline int chan_put_to_taker(goc_chan* ch, void* val) {
    goc_entry** pp = &ch->takers;
    while (*pp && atomic_load_explicit(&(*pp)->cancelled, memory_order_acquire))
        pp = &(*pp)->next;
    goc_entry* e = *pp;
    if (!e) return 0;
    /* Save e->next before calling wake(). For GOC_FIBER entries, wake() calls
     * post_to_run_queue, which may allow a pool thread to resume the waiting
     * fiber immediately. That fiber's stack frame — which contains the
     * stack-allocated goc_entry (from goc_take's slow path) — is then
     * deallocated as the coroutine unwinds past mco_yield. Reading e->next
     * after wake() would therefore be a use-after-free. Snapshotting next
     * here, before wake(), is safe because e->next is only ever written under
     * ch->lock, which we still hold. */
    goc_entry* next = e->next;
    wake(ch, e, val);
    *pp = next;
    return 1;
}

static inline int chan_take_from_putter(goc_chan* ch, void** out) {
    goc_entry** pp = &ch->putters;
    while (*pp && atomic_load_explicit(&(*pp)->cancelled, memory_order_acquire))
        pp = &(*pp)->next;
    goc_entry* e = *pp;
    if (!e) return 0;
    *out = e->put_val;
    /* Save e->next before calling wake() for the same reason as chan_put_to_taker:
     * wake() may resume the parked fiber immediately on another pool thread,
     * freeing the stack frame that contains the stack-allocated goc_entry.
     * e->put_val is read above (before wake), which is safe. e->next must be
     * snapshotted here, also before wake(), while ch->lock is still held. */
    goc_entry* next = e->next;
    wake(ch, e, NULL);
    *pp = next;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Global Extern Declarations
 * --------------------------------------------------------------------------- */

/* Defined in loop.c */
extern uv_loop_t*            g_loop;
extern _Atomic(uv_async_t*)  g_wakeup;

/* Defined in fiber.c */
extern goc_pool*             g_default_pool;

#endif /* GOC_INTERNAL_H */
