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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  include <winsock2.h>
#endif
#include <uv.h>
#include <gc.h>
#include "minicoro.h"
#include "chan_type.h"
#include "../include/goc.h"

/* Prevent accidental direct uv_close() calls inside libgoc source files.
 * All close paths must go through the goc handle state machine. */
#define uv_close DO_NOT_CALL_uv_close_directly_use_goc_uv_close_internal

static inline void goc_uv_close_internal(uv_handle_t* handle, uv_close_cb cb)
{
    #undef uv_close
    uv_close(handle, cb);
    #define uv_close DO_NOT_CALL_uv_close_directly_use_goc_uv_close_internal
}
#include "config.h"

static inline unsigned long long goc_uv_thread_id(void)
{
    return (unsigned long long)(uintptr_t)uv_thread_self();
}

static inline void goc_uv_init_log(const char *name, int rc,
                                   uv_loop_t *loop, uv_handle_t *handle)
{
    GOC_DBG("[uv_init] %s rc=%d handle=%p type=%s loop=%p alive=%d thread=%llu\n",
            name,
            rc,
            (void*)handle,
            handle ? uv_handle_type_name(uv_handle_get_type(handle)) : "<null>",
            (void*)loop,
            loop ? uv_loop_alive(loop) : -1,
            goc_uv_thread_id());
}

extern _Thread_local int goc_uv_callback_depth;
extern int goc_on_loop_thread(void);

void goc_worker_io_handle_opened(void);
void goc_worker_io_handle_closed(void);

static inline void goc_uv_close_log(const char *reason, uv_handle_t *handle)
{
    GOC_DBG("[uv_close] %s handle=%p type=%s active=%d closing=%d has_ref=%d loop=%p callback_depth=%d thread=%llu\n",
            reason,
            (void*)handle,
            handle ? uv_handle_type_name(uv_handle_get_type(handle)) : "<null>",
            handle ? uv_is_active(handle) : -1,
            handle ? uv_is_closing(handle) : -1,
            handle ? uv_has_ref(handle) : -1,
            handle ? (void*)handle->loop : NULL,
            goc_uv_callback_depth,
            goc_uv_thread_id());
}

#define ABORT(fmt, ...) \
    do { \
        fprintf(stderr, "libgoc: " fmt, ##__VA_ARGS__); \
        abort(); \
    } while (0)


static inline void goc_uv_callback_enter(const char *name)
{
    goc_uv_callback_depth++;
    GOC_DBG("[uv_cb] enter %s depth=%d thread=%llu\n",
            name,
            goc_uv_callback_depth,
            goc_uv_thread_id());
}

static inline void goc_uv_callback_exit(const char *name)
{
    GOC_DBG("[uv_cb] exit %s depth=%d thread=%llu\n",
            name,
            goc_uv_callback_depth,
            goc_uv_thread_id());
    goc_uv_callback_depth--;
}

static inline void goc_uv_handle_log(const char *prefix, uv_handle_t *handle)
{
    if (!handle) {
        GOC_DBG("[uv_handle] %s handle=NULL thread=%llu\n",
                prefix, goc_uv_thread_id());
        return;
    }
    GOC_DBG("[uv_handle] %s handle=%p type=%s active=%d closing=%d has_ref=%d loop=%p data=%p thread=%llu\n",
            prefix,
            (void*)handle,
            uv_handle_type_name(uv_handle_get_type(handle)),
            uv_is_active(handle),
            uv_is_closing(handle),
            uv_has_ref(handle),
            (void*)handle->loop,
            handle->data,
            goc_uv_thread_id());
}

typedef struct {
    const char *context;
    size_t      count;
} goc_uv_walk_arg_t;

static void goc_uv_walk_handle_cb(uv_handle_t *handle, void *arg)
{
    goc_uv_walk_arg_t *ctx = (goc_uv_walk_arg_t *)arg;
    GOC_DBG("[uv_walk] ctx=%s handle=%p type=%s active=%d closing=%d has_ref=%d loop=%p data=%p thread=%llu\n",
            ctx->context,
            (void*)handle,
            uv_handle_type_name(uv_handle_get_type(handle)),
            uv_is_active(handle),
            uv_is_closing(handle),
            uv_has_ref(handle),
            (void*)handle->loop,
            handle->data,
            goc_uv_thread_id());
    ctx->count++;
}

static inline void goc_uv_walk_log(uv_loop_t *loop, const char *context)
{
    goc_uv_walk_arg_t arg = { context, 0 };
    uv_walk(loop, goc_uv_walk_handle_cb, &arg);
    GOC_DBG("[uv_walk] end context=%s loop=%p handle_count=%zu thread=%llu\n",
            context,
            (void*)loop,
            arg.count,
            goc_uv_thread_id());
}

/* Internal runtime helper: yield the current fiber to the scheduler. */
/* Public API declaration moved to include/goc.h. */

/* ---------------------------------------------------------------------------
 * goc_sync_t — portable binary semaphore (mutex + condvar)
 *
 * Replaces sem_t for the GOC_SYNC blocking path in goc_take_sync,
 * goc_put_sync, and goc_alts_sync.  sem_init for unnamed POSIX semaphores
 * returns ENOSYS on macOS (they are not supported), making sem_wait return
 * EINVAL immediately and silently corrupting every sync call on macOS.
 * uv_cond_wait works correctly on Linux, macOS, and Windows.
 *
 * Semantics: single-use binary semaphore.  goc_sync_post may be called before
 * or after goc_sync_wait; if post fires first, wait returns immediately.
 * --------------------------------------------------------------------------- */

typedef struct {
    uv_mutex_t mtx;
    uv_cond_t  cond;
    int        ready;
} goc_sync_t;

static inline void goc_sync_init(goc_sync_t* s) {
    uv_mutex_init(&s->mtx);
    uv_cond_init(&s->cond);
    s->ready = 0;
}

static inline void goc_sync_post(goc_sync_t* s) {
    uv_mutex_lock(&s->mtx);
    s->ready = 1;
    uv_cond_signal(&s->cond);
    uv_mutex_unlock(&s->mtx);
}

static inline void goc_sync_wait(goc_sync_t* s) {
    uv_mutex_lock(&s->mtx);
    while (!s->ready)
        uv_cond_wait(&s->cond, &s->mtx);
    uv_mutex_unlock(&s->mtx);
}

static inline void goc_sync_destroy(goc_sync_t* s) {
    uv_mutex_destroy(&s->mtx);
    uv_cond_destroy(&s->cond);
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

typedef struct timer_heap_entry timer_heap_entry_t;

typedef struct {
    uv_timer_t*         handle;
    timer_heap_entry_t* heap;
    size_t              len;
    size_t              cap;
} goc_timer_manager_t;

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
    uint64_t           id;             /* monotonically increasing fiber ID; assigned at creation */
    _Atomic int        cancelled;       /* set to 1 to cancel this entry (alts loser path) */
    _Atomic int        woken;           /* CAS 0→1 to claim wake; only one winner */
    _Atomic int*       fired;           /* shared fired flag for goc_alts; NULL otherwise */
    struct goc_entry*  next;            /* intrusive linked list (takers / putters) */

    /* Fiber / pool context */
    goc_pool*          pool;            /* pool on which the fiber runs */
    size_t             home_worker_idx; /* preferred worker for resumed fiber */
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
    void             (*cb)(goc_chan* ch, void* val, goc_status_t ok, void* ud);      /* take callback */
    void             (*put_cb)(goc_chan* ch, void* val, goc_status_t ok, void* ud);  /* put callback */
    void*              ud;
    void*              cb_result;   /* value delivered to take callback; reused as result_slot target for SYNC */
    void*              put_val;     /* value the putter wants to send */

    /* Pending _cb channel operation.
     *
     * goc_put_cb / goc_take_cb post the entry directly to the loop queue
     * instead of acquiring ch->lock inline.  The loop thread processes the
     * channel operation under the lock on behalf of the caller.
     *
     * ch       — target channel; NULL once the entry is parked or resolved.
     * is_put   — true = pending put (put_val is the value to send);
     *            false = pending take (cb receives the value). */
    struct goc_chan*   ch;
    bool               is_put;
    bool               free_on_drain;  /* if true, drain/loop_process frees 'e' after delivery */

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
     *   mco_get_user_data(e->coro)).  Its values are generation-encoded:
     *     0 = parking in progress (fiber set it just before releasing locks,
     *         mco_yield has not yet returned on the pool worker side)
     *     odd = active park generation in progress; the fiber has not yet
     *           reached MCO_SUSPENDED for that generation.
     *     even = fiber is safely MCO_SUSPENDED for the encoded generation.
     *
     *   Fiber (goc_take / goc_put / goc_alts slow path):
     *     Sets fiber_entry->parked = (gen << 1) | 1 while holding channel
     *     lock(s), then releases the lock(s) and calls mco_yield.
     *
     *   pool_worker_fn (after mco_resume returns):
     *     Bumps the odd parked value to the even suspended value by adding 1.
     *
     *   wake() and goc_close() (GOC_FIBER case):
     *     Spin until the fiber_entry parked state reaches the exact suspended
     *     value for the park generation being awakened.  This guarantees the
     *     coroutine is truly MCO_SUSPENDED before any worker thread calls
     *     mco_resume.
     * -------------------------------------------------------------------------- */
    _Atomic uint64_t         parked;      /* encoded park generation/state */
    uint64_t                 parked_gen;  /* park generation assigned to this entry */
};

struct goc_spawn_req {
    struct goc_spawn_req* next;
    void               (*fn)(void*);
    void*                 fn_arg;
    goc_chan*             join_ch;
    size_t                home_worker_idx; /* optional worker affinity for deferred spawns */
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
    #define goc_stack_canary_check(ptr)   do { if (*(ptr) != GOC_STACK_CANARY) { ABORT("stack canary corrupted at %p (val=0x%08x); likely stack overflow\n", (void*)(ptr), *(ptr)); } } while(0)
#endif

/* ---------------------------------------------------------------------------
 * goc_timeout_timer_ctx
 *
 * Defined here (rather than in timeout.c) so that loop.c's central timer
 * manager can access the struct fields directly without an extra header.
 * The embedded uv_timer_t has been removed; lifetime is now managed by the
 * GC-allocated min-heap inside the central timer manager.
 * --------------------------------------------------------------------------- */

typedef struct goc_timeout_timer_ctx {
    goc_chan*   ch;
    _Atomic int start_state;      /* 0=not-started, 1=started, 2=closed */
    _Atomic int cancel_requested;
    size_t      heap_idx;         /* index in the central timer heap; SIZE_MAX if not in heap */
} goc_timeout_timer_ctx;

/* ---------------------------------------------------------------------------
 * Internal Function Declarations (cross-module)
 * --------------------------------------------------------------------------- */

/* gc.c → used by channel.c */
void chan_register(goc_chan* ch);
void chan_unregister(goc_chan* ch);

/* channel.c → used by goc_io.c */
goc_chan_close_state_t goc_chan_close_state(goc_chan* ch);
static inline bool goc_chan_has_state(goc_chan* ch, goc_chan_close_state_t state)
{
    return goc_chan_close_state(ch) == state;
}
static inline bool goc_chan_is_open(goc_chan* ch)
{
    return goc_chan_has_state(ch, GOC_CHAN_OPEN);
}

/* gc.c → called from goc_init (gc.c) */
void live_uv_handles_init(void);

/* gc.c → used by goc_io.c (goc_handle_register/unregister) */
void gc_handle_register(void* p);
void gc_handle_unregister(void* p);
void goc_deferred_handle_unreg_init(void);
void goc_deferred_handle_unreg_shutdown(void);
void goc_deferred_handle_unreg_add(uv_handle_t* handle);
void goc_deferred_handle_unreg_flush(void);
size_t goc_deferred_handle_unreg_len(void);
void goc_deferred_close_add(uv_handle_t* handle, uv_close_cb cb, goc_chan* ch);
void goc_deferred_close_flush(void);
size_t goc_deferred_close_len(void);

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
void post_to_specific_worker(goc_pool* pool,
                             size_t worker_idx,
                             goc_entry* entry);
int  goc_pool_id(goc_pool* pool);
void pool_submit_spawn(goc_pool* pool,
                       void (*fn)(void*),
                       void* arg,
                       goc_chan* join_ch);
void pool_submit_spawn_to_worker(goc_pool* pool,
                                 size_t worker_idx,
                                 void (*fn)(void*),
                                 void* arg,
                                 goc_chan* join_ch);

/* Internal helper: spawn a fiber on a specific worker. */
/* Public API declaration moved to include/goc.h. */

/* Test helper: spin until at least n workers are idle (parked on idle_sem).
 * Not part of the public API; declared here for use by internal test files. */
void pool_wait_all_idle(goc_pool* pool, size_t n);

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
/* Public API declaration moved to include/goc.h. */
void post_callback(goc_entry* entry, void* value);
int  goc_loop_submit_callback_if_running(goc_entry* entry);
void post_on_loop(void (*fn)(void*), void* arg);
int  post_on_loop_checked(void (*fn)(void*), void* arg);
void goc_loop_wakeup(void);

/* goc_io.c → init / shutdown helpers */
void goc_io_init(void);
void goc_io_shutdown(void);

/* Internal lifecycle helper: invoke hooks registered for the phase. */
void goc_run_lifecycle_hooks(goc_lifecycle_hook_phase_t phase,
                             void* event_arg);

/* pool.c → current worker helpers */
uv_loop_t* goc_current_worker_loop(void);
bool       goc_current_worker_has_pending_tasks(void);
int  post_on_handle_loop(uv_loop_t* loop, void (*fn)(void*), void* arg);
size_t     goc_pool_thread_count(goc_pool* pool);

/* loop.c central timer manager → called from timeout.c */
goc_timer_manager_t* goc_global_timer_mgr(void);
void goc_timer_manager_insert(goc_timer_manager_t* mgr,
                              goc_timeout_timer_ctx* ctx,
                              uint64_t deadline_ns);
void goc_timer_manager_remove(goc_timer_manager_t* mgr,
                              goc_timeout_timer_ctx* ctx);

/* timeout.c → called from loop.c timer manager */
void goc_timeout_ctx_expire(goc_timeout_timer_ctx* tctx);

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

static inline uv_loop_t* goc_worker_or_default_loop(void) {
    uv_loop_t* l = goc_current_worker_loop();
    return l ? l : g_loop;
}

/* Defined in fiber.c */
extern goc_pool*             g_default_pool;

/* ---------------------------------------------------------------------------
 * GC-aware libuv thread wrappers
 *
 * Use goc_thread_create / goc_thread_join everywhere in libgoc instead
 * of calling uv_thread_create directly.
 *
 * uv_thread_create does not register new threads with Boehm GC.
 * goc_thread_create uses a trampoline (goc_thread_trampoline in gc.c)
 * that calls GC_register_my_thread at startup and GC_unregister_my_thread
 * at exit on all platforms.  GC_allow_register_threads() must have been
 * called first — goc_init() does this.
 * --------------------------------------------------------------------------- */

int goc_thread_create(uv_thread_t* t, uv_thread_cb fn, void* arg);
int goc_thread_join(uv_thread_t* t);

#endif /* GOC_INTERNAL_H */
