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
#include <pthread.h>
#include <uv.h>
#include <gc.h>
#include "minicoro.h"
#include "../include/goc.h"
#include "config.h"

/* ---------------------------------------------------------------------------
 * goc_sync_t — portable binary semaphore (mutex + condvar)
 *
 * Replaces sem_t for the GOC_SYNC blocking path in goc_take_sync,
 * goc_put_sync, and goc_alts_sync.  sem_init for unnamed POSIX semaphores
 * returns ENOSYS on macOS (they are not supported), making sem_wait return
 * EINVAL immediately and silently corrupting every sync call on macOS.
 * pthread_cond_wait works correctly on Linux, macOS, and Windows (MSYS2).
 *
 * Semantics: single-use binary semaphore.  goc_sync_post may be called before
 * or after goc_sync_wait; if post fires first, wait returns immediately.
 * --------------------------------------------------------------------------- */

typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cond;
    int             ready;
} goc_sync_t;

static inline void goc_sync_init(goc_sync_t* s) {
    pthread_mutex_init(&s->mtx, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->ready = 0;
}

static inline void goc_sync_post(goc_sync_t* s) {
    pthread_mutex_lock(&s->mtx);
    s->ready = 1;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mtx);
}

static inline void goc_sync_wait(goc_sync_t* s) {
    pthread_mutex_lock(&s->mtx);
    while (!s->ready)
        pthread_cond_wait(&s->cond, &s->mtx);
    pthread_mutex_unlock(&s->mtx);
}

static inline void goc_sync_destroy(goc_sync_t* s) {
    pthread_mutex_destroy(&s->mtx);
    pthread_cond_destroy(&s->cond);
}

/* ---------------------------------------------------------------------------
 * Forward Declarations
 *
 * goc_entry — full definition below.
 * goc_pool  — opaque; full definition lives only in pool.c.
 * --------------------------------------------------------------------------- */

typedef struct goc_pool goc_pool;   /* defined in pool.c */
typedef struct goc_entry goc_entry;
typedef struct goc_spawn_req goc_spawn_req;

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
    _Atomic int*       fired;           /* shared fired flag for goc_alts; NULL otherwise */
    struct goc_entry*  next;            /* intrusive linked list (takers / putters) */

    /* Fiber / pool context */
    goc_pool*          pool;            /* pool on which the fiber runs */
    mco_coro*          coro;            /* the fiber's coroutine handle */
    uint32_t*          stack_canary_ptr;/* points to lowest stack word; checked before resume */
    void*              fiber_root_handle; /* opaque handle returned by goc_fiber_root_register */

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
    goc_sync_t         sync_obj;
    goc_sync_t*        sync_sem_ptr;   /* points to sync_obj (own) or a shared goc_sync_t in goc_alts_sync */

    /* Yield-gate: guards the race between wake() and mco_yield().
     *
     * There is a brief window between when a parking fiber releases
     * ch->lock (or the alts lock set) and when it actually calls mco_yield.
     * If wake() calls post_to_run_queue during that window, a pool worker
     * calls mco_resume on a MCO_RUNNING coroutine — mco_resume silently
     * returns MCO_NOT_SUSPENDED, the entry is "consumed", and the fiber hangs
     * after calling mco_yield with nobody left to resume it.
     *
     * Protocol (GOC_FIBER entries only):
     *   `parked` lives on the fiber's INITIAL goc_entry (accessible via
     *   mco_get_user_data(e->coro)).  Its values:
     *     0 = parking in progress (fiber set it just before releasing locks,
     *         mco_yield has not yet returned on the pool worker side)
     *     1 = fiber is safely MCO_SUSPENDED (pool_worker_fn set it after
     *         mco_resume returned)
     *
     *   Fiber (goc_take / goc_put / goc_alts slow path):
     *     Sets fiber_entry->parked = 0 while still holding the channel lock(s),
     *     then releases the lock(s) and calls mco_yield.
     *
     *   pool_worker_fn (after mco_resume returns):
     *     Sets fiber_entry->parked = 1.
     *
     *   wake() and goc_close() (GOC_FIBER case):
     *     Spin with sched_yield() while fiber_entry->parked == 0 before
     *     calling post_to_run_queue.  This guarantees the coroutine is truly
     *     MCO_SUSPENDED before any worker thread calls mco_resume. */
    _Atomic int              parked;      /* per-fiber flag; see protocol above */
};

struct goc_spawn_req {
    struct goc_spawn_req* next;
    void               (*fn)(void*);
    void*                 fn_arg;
    goc_chan*             join_ch;
};

/* ---------------------------------------------------------------------------
 * Canary Constant
 *
 * Written to the lowest stack word after mco_create.
 * Checked before every mco_resume in pool_worker_fn.
 * --------------------------------------------------------------------------- */

#define GOC_STACK_CANARY  0xDEADC0DEu

/* ---------------------------------------------------------------------------
 * Conditional Stack Protection Macros
 *
 * With virtual memory allocator (LIBGOC_VMEM_ENABLED), stack overflow
 * protection is unnecessary since stacks can grow dynamically. These macros
 * compile to no-ops to avoid overhead and potential bugs with virtual stacks.
 *
 * With fixed-size stacks, full protection is enabled to catch overflows.
 * --------------------------------------------------------------------------- */

#ifdef LIBGOC_VMEM_ENABLED
    /* Virtual memory mode: no stack boundaries to protect */
    #define goc_stack_canary_init(entry)  do { (entry)->stack_canary_ptr = NULL; } while(0)
    #define goc_stack_canary_set(ptr)     do { (void)(ptr); } while(0)
    #define goc_stack_canary_check(ptr)   do { (void)(ptr); } while(0)
#else
    /* Fixed stack mode: enable overflow protection */
    #define goc_stack_canary_init(entry)  do { (entry)->stack_canary_ptr = (uint32_t*)(entry)->coro->stack_base; } while(0)
    #define goc_stack_canary_set(ptr)     do { *(ptr) = GOC_STACK_CANARY; } while(0)
    #define goc_stack_canary_check(ptr)   do { if (*(ptr) != GOC_STACK_CANARY) { fprintf(stderr, "libgoc: stack canary corrupted at %p (val=0x%08x); likely stack overflow\n", (void*)(ptr), *(ptr)); abort(); } } while(0)
#endif

/* ---------------------------------------------------------------------------
 * Internal Function Declarations (cross-module)
 * --------------------------------------------------------------------------- */

/* gc.c → used by channel.c */
void chan_register(goc_chan* ch);
void chan_unregister(goc_chan* ch);

/* gc.c → used by fiber.c, pool.c */
void* goc_fiber_root_register(mco_coro* coro, void* top, goc_entry* entry);
void  goc_fiber_root_unregister(void* handle);
void  goc_fiber_root_update_sp(void* handle, mco_coro* coro);
void  goc_fiber_roots_init(void);

/* fiber.c → used by pool.c */
goc_entry* goc_fiber_entry_create(goc_pool* pool,
                                  void (*fn)(void*),
                                  void* arg,
                                  goc_chan* join_ch);

/* minicoro.c → used by gc.c (push_fiber_roots callback) */
void* mco_get_suspended_sp(mco_coro* co);

/* pool.c → used by fiber.c, channel.c */
void post_to_run_queue(goc_pool* pool, goc_entry* entry);
void pool_submit_spawn(goc_pool* pool,
                       void (*fn)(void*),
                       void* arg,
                       goc_chan* join_ch);

/* Inline helper used by wake() and goc_close to atomically claim a parked
 * entry for dispatch.  For goc_alts entries (fired != NULL), first CAS fired
 * 0→1 (acq_rel) — if another arm already fired, return false immediately.
 * Then CAS woken 0→1 (acq_rel) — if another caller already claimed the entry,
 * return false.  Returns true only when the caller wins both CASes (or fired
 * is NULL and only the woken CAS is needed). */
static inline bool try_claim_wake(goc_entry* e) {
    if (e->fired != NULL) {
        int expected_fired = 0;
        if (!atomic_compare_exchange_strong_explicit(
                e->fired, &expected_fired, 1,
                memory_order_acq_rel, memory_order_acquire)) {
            return false;
        }
    }

    int expected = 0;
    return atomic_compare_exchange_strong_explicit(
        &e->woken, &expected, 1,
        memory_order_acq_rel, memory_order_acquire);
}

/* loop.c → used by channel.c, alts.c, timeout.c, gc.c */
void loop_init(void);
void loop_shutdown(void);
void post_callback(goc_entry* entry, void* value);

/* gc.c → used by pool.c, loop.c */
void live_channels_init(void);
void pool_registry_init(void);
void mutex_registry_init(void);

/* pool.c → used by gc.c */
void pool_registry_destroy_all(void);
void mutex_registry_destroy_all(void);

/* ---------------------------------------------------------------------------
 * Global Extern Declarations
 * --------------------------------------------------------------------------- */

/* Defined in loop.c */
extern uv_loop_t*            g_loop;
extern _Atomic(uv_async_t*)  g_wakeup;

/* Defined in fiber.c */
extern goc_pool*             g_default_pool;

/* ---------------------------------------------------------------------------
 * Portable GC-aware pthread wrappers
 *
 * Use gc_pthread_create / gc_pthread_join everywhere in libgoc instead of
 * calling GC_pthread_create / pthread_create directly.
 *
 * On POSIX (Linux / macOS): simple aliases for GC_pthread_create /
 * GC_pthread_join, which register the new thread with Boehm GC automatically
 * via GC_call_with_stack_base.  Threads must NOT call GC_register_my_thread
 * manually on POSIX — doing so double-registers the thread and corrupts the
 * GC's internal thread table.
 *
 * On Windows (MSYS2 UCRT64): bdwgc is compiled with Win32 threads and does
 * not provide GC_pthread_create.  gc_pthread_create uses a generic trampoline
 * that calls GC_register_my_thread before the thread body and
 * GC_unregister_my_thread after it exits.  GC_allow_register_threads() must
 * have been called first — goc_init() does this.
 * --------------------------------------------------------------------------- */

#ifdef _WIN32
int gc_pthread_create(pthread_t* t, const pthread_attr_t* a,
                      void* (*fn)(void*), void* arg);
int gc_pthread_join(pthread_t t, void** retval);
#else
#  define gc_pthread_create  GC_pthread_create
#  define gc_pthread_join    GC_pthread_join
#endif

#endif /* GOC_INTERNAL_H */
