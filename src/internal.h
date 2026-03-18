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
#include <semaphore.h>
#include <uv.h>
#include <gc.h>
#include "minicoro.h"
#include "../include/goc.h"
#include "config.h"

/* ---------------------------------------------------------------------------
 * Forward Declarations
 *
 * goc_entry — full definition below.
 * goc_pool  — opaque; full definition lives only in pool.c.
 * --------------------------------------------------------------------------- */

typedef struct goc_pool goc_pool;   /* defined in pool.c */
typedef struct goc_entry goc_entry;

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
 * --------------------------------------------------------------------------- */

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
