/*
 * goc.h — Public API for libgoc
 *
 * A Go-style CSP concurrency runtime for C: thread pools, stackful coroutines,
 * channels, select, async I/O, and garbage collection in one coherent API.
 *
 * Consumers: #include "goc.h"  (or <goc.h> when installed)
 *
 * Compile requirements: -std=c11
 * Required defines (build system / compiler command line):
 *   -DGC_THREADS  -D_GNU_SOURCE
 */

#ifndef GOC_H
#define GOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Opaque types
 * ---------------------------------------------------------------------- */

typedef struct goc_chan goc_chan;
typedef struct goc_pool goc_pool;

/* -------------------------------------------------------------------------
 * Status and value types
 * ---------------------------------------------------------------------- */

typedef enum {
    GOC_EMPTY  = -1,  /* channel open but no value available (goc_take_try only) */
    GOC_CLOSED =  0,  /* channel was closed                                       */
    GOC_OK     =  1,  /* value delivered successfully                             */
} goc_status_t;

typedef struct {
    void*        val;
    goc_status_t ok;
} goc_val_t;

/* -------------------------------------------------------------------------
 * Drain result type
 * ---------------------------------------------------------------------- */

typedef enum {
    GOC_DRAIN_OK      = 0,
    GOC_DRAIN_TIMEOUT = 1,
} goc_drain_result_t;

/* -------------------------------------------------------------------------
 * Select types
 * ---------------------------------------------------------------------- */

typedef enum {
    GOC_ALT_TAKE,
    GOC_ALT_PUT,
    GOC_ALT_DEFAULT,
} goc_alt_kind;

typedef struct {
    goc_chan*    ch;
    goc_alt_kind op_kind;
    void*        put_val;
} goc_alt_op;

typedef struct {
    size_t    index;
    goc_val_t value;
} goc_alts_result;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

/**
 * goc_init() — Initialise the runtime.
 *
 * Must be called exactly once before any other goc_* function.
 * Initialises Boehm GC, the libuv event loop thread, the default thread pool,
 * the live-channels registry, and the pool registry.
 */
void goc_init(void);

/**
 * goc_shutdown() — Shut down the runtime.
 *
 * Drains all pools, stops the event loop, destroys all live channel mutexes,
 * and releases all runtime resources. No goc_* call may be made after this
 * returns (except goc_init to re-initialise).
 */
void goc_shutdown(void);

/**
 * goc_scheduler() — Return the libuv event loop used by the runtime.
 *
 * The returned pointer is valid from goc_init() until goc_shutdown() returns.
 * Callers may schedule their own libuv handles on this loop but must not stop
 * or close the loop themselves.
 */
uv_loop_t* goc_scheduler(void);

/* -------------------------------------------------------------------------
 * Memory
 * ---------------------------------------------------------------------- */

/**
 * goc_malloc() — Allocate n bytes on the Boehm GC heap.
 *
 * Memory is managed by the garbage collector; callers need not free it.
 * Aborts on allocation failure. Never returns NULL.
 */
void* goc_malloc(size_t n);

/* -------------------------------------------------------------------------
 * Fiber launch
 * ---------------------------------------------------------------------- */

/**
 * goc_in_fiber() — Return true if the calling code is running inside a fiber.
 */
bool goc_in_fiber(void);

/**
 * goc_go() — Spawn a fiber on the default thread pool.
 *
 * fn    : fiber entry point; receives arg.
 * arg   : arbitrary user data passed to fn.
 *
 * Returns a join channel that is closed when fn returns. The caller may
 * goc_take() or goc_take_sync() on the join channel to wait for completion,
 * or ignore it entirely (the channel is GC-collected when unreachable).
 */
goc_chan* goc_go(void (*fn)(void*), void* arg);

/**
 * goc_go_on() — Spawn a fiber on a specific thread pool.
 *
 * pool  : target pool (must not be NULL; must not have been destroyed).
 * fn    : fiber entry point.
 * arg   : arbitrary user data passed to fn.
 *
 * Returns a join channel as with goc_go().
 */
goc_chan* goc_go_on(goc_pool* pool, void (*fn)(void*), void* arg);

/* -------------------------------------------------------------------------
 * Channel lifecycle
 * ---------------------------------------------------------------------- */

/**
 * goc_chan_make() — Allocate and return a new channel.
 *
 * buf_size : capacity of the internal ring buffer (0 = rendezvous channel).
 *
 * The channel itself is GC-heap allocated. Its internal mutex is plain-malloc
 * allocated (libuv constraint) and is freed during goc_shutdown() or when
 * goc_close() triggers cleanup.
 */
goc_chan* goc_chan_make(size_t buf_size);

/**
 * goc_close() — Close a channel.
 *
 * Idempotent: a second call on an already-closed channel is a no-op.
 * After closing:
 *   - All parked takers are woken with GOC_CLOSED.
 *   - All parked putters are woken with GOC_CLOSED.
 *   - Subsequent goc_put / goc_put_sync return GOC_CLOSED immediately.
 *   - goc_take / goc_take_sync drain any remaining buffered values before
 *     returning GOC_CLOSED.
 */
void goc_close(goc_chan* ch);

/* -------------------------------------------------------------------------
 * Channel I/O — Fiber context
 *
 * Must only be called from within a fiber (i.e. goc_in_fiber() == true).
 * Calling from an OS thread results in undefined behaviour.
 * ---------------------------------------------------------------------- */

/**
 * goc_take() — Receive a value from ch (fiber context).
 *
 * Parks the calling fiber until a value is available or the channel is closed.
 * Returns {val, GOC_OK} on success, {NULL, GOC_CLOSED} if the channel is
 * closed and empty.
 */
goc_val_t goc_take(goc_chan* ch);

/**
 * goc_put() — Send val to ch (fiber context).
 *
 * Parks the calling fiber until a receiver is ready or the channel's buffer
 * has space.
 * Returns GOC_OK on success, GOC_CLOSED if the channel is closed.
 */
goc_status_t goc_put(goc_chan* ch, void* val);

/* -------------------------------------------------------------------------
 * Channel I/O — OS thread context
 *
 * Safe to call from any OS thread (including the event-loop thread, though
 * blocking calls will stall the loop — prefer callbacks from the loop thread).
 * ---------------------------------------------------------------------- */

/**
 * goc_take_sync() — Blocking receive from ch (OS thread context).
 *
 * Blocks the calling OS thread until a value is available or the channel is
 * closed.  Returns {val, GOC_OK} or {NULL, GOC_CLOSED}.
 */
goc_val_t goc_take_sync(goc_chan* ch);

/**
 * goc_take_try() — Non-blocking receive (any context).
 *
 * Returns immediately with one of:
 *   {val,  GOC_OK}     — a value was available and has been dequeued.
 *   {NULL, GOC_CLOSED} — the channel is closed and empty.
 *   {NULL, GOC_EMPTY}  — the channel is open but has no value ready.
 *
 * Never parks the caller.
 */
goc_val_t goc_take_try(goc_chan* ch);

/**
 * goc_put_sync() — Blocking send to ch (OS thread context).
 *
 * Blocks the calling OS thread until the value has been delivered or the
 * channel is closed.
 * Returns GOC_OK on success, GOC_CLOSED if the channel is closed.
 */
goc_status_t goc_put_sync(goc_chan* ch, void* val);

/* -------------------------------------------------------------------------
 * Channel I/O — Callbacks (loop-thread delivery)
 *
 * These functions register a callback that will be invoked on the libuv event
 * loop thread when the operation completes.  They never block the caller.
 * ---------------------------------------------------------------------- */

/**
 * goc_take_cb() — Asynchronous receive with callback.
 *
 * ch  : channel to receive from.
 * cb  : called on the loop thread with (val, ok, ud) when a value arrives or
 *       the channel closes. Must not be NULL.
 * ud  : opaque user data forwarded to cb.
 */
void goc_take_cb(goc_chan* ch,
                 void (*cb)(void* val, goc_status_t ok, void* ud),
                 void* ud);

/**
 * goc_put_cb() — Asynchronous send with optional callback.
 *
 * ch  : channel to send to.
 * val : value to send.
 * cb  : called on the loop thread with (ok, ud) when the send completes.
 *       May be NULL if the caller does not need notification.
 * ud  : opaque user data forwarded to cb.
 */
void goc_put_cb(goc_chan* ch, void* val,
                void (*cb)(goc_status_t ok, void* ud),
                void* ud);

/* -------------------------------------------------------------------------
 * Select
 * ---------------------------------------------------------------------- */

/**
 * goc_alts() — Select over multiple channel operations (fiber context only).
 *
 * ops : array of n goc_alt_op descriptors.  Each op specifies a channel,
 *       whether the arm is a take, put, or default, and (for put arms) the
 *       value to send.
 * n   : number of ops (must be >= 1).
 *
 * Arms are evaluated in randomised order to prevent starvation.  If a
 * GOC_ALT_DEFAULT arm is present and no other arm is immediately ready, the
 * default arm fires without parking.
 *
 * Parks the calling fiber until exactly one arm fires.
 *
 * Returns {index, {val, ok}} identifying the arm that fired.  For put arms,
 * value.val is NULL; value.ok is GOC_OK or GOC_CLOSED.
 *
 * Must only be called from within a fiber.
 */
goc_alts_result goc_alts(goc_alt_op* ops, size_t n);

/**
 * goc_alts_sync() — Select over multiple channel operations (OS thread context).
 *
 * Same semantics as goc_alts() but blocks the calling OS thread instead of
 * parking a fiber.  Safe to call from any OS thread.
 */
goc_alts_result goc_alts_sync(goc_alt_op* ops, size_t n);

/* -------------------------------------------------------------------------
 * Timeout
 * ---------------------------------------------------------------------- */

/**
 * goc_timeout() — Return a channel that is closed after ms milliseconds.
 *
 * The returned channel is a rendezvous channel (buf_size == 0).  It is closed
 * by a one-shot libuv timer after the specified delay.  Callers can use it
 * as a GOC_ALT_TAKE arm in goc_alts / goc_alts_sync to implement deadlines.
 *
 * The timer handle is plain-malloc allocated on the event loop thread and is
 * freed automatically when the timer fires.
 */
goc_chan* goc_timeout(uint64_t ms);

/* -------------------------------------------------------------------------
 * Thread pool
 * ---------------------------------------------------------------------- */

/**
 * goc_pool_make() — Create a new thread pool with the given number of threads.
 *
 * threads : number of OS worker threads to spawn (must be >= 1).
 *
 * Returns a pointer to the new pool.  The pool is registered in the global
 * pool registry and will be drained and destroyed during goc_shutdown() if
 * not explicitly destroyed first.
 */
goc_pool* goc_pool_make(size_t threads);

/**
 * goc_pool_destroy() — Drain and destroy pool.
 *
 * Blocks until all fibers running on pool have completed, then stops the
 * worker threads and frees pool resources.  Behavior is undefined if fibers
 * on this pool block indefinitely.
 *
 * Must not be called from within a worker thread that belongs to `pool`.
 * Doing so aborts with a diagnostic message.
 */
void goc_pool_destroy(goc_pool* pool);

/**
 * goc_pool_destroy_timeout() — Drain and destroy pool with a deadline.
 *
 * Waits up to ms milliseconds for all fibers to complete.
 * Returns GOC_DRAIN_OK if the pool was fully drained and destroyed within the
 * timeout, or GOC_DRAIN_TIMEOUT if the deadline was reached before all fibers
 * finished (pool is left running in this case).
 *
 * Must not be called from within a worker thread that belongs to `pool`.
 * Doing so aborts with a diagnostic message.
 */
goc_drain_result_t goc_pool_destroy_timeout(goc_pool* pool, uint64_t ms);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GOC_H */
