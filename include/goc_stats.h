/*
 * goc_stats.h — Public API for libgoc telemetry
 *
 * Events are delivered asynchronously via a lock-free MPSC queue.  Emitting
 * threads (pool workers, main thread, etc.) push events onto the queue and
 * call uv_async_send; the callback is always invoked on the libuv loop thread
 * with no internal locks held, so it may safely access loop-thread state.
 *
 * Copyright (c) Divyansh Prakash
 *
 * Typical usage:
 *   goc_stats_init();
 *   goc_stats_set_callback(my_cb, my_ud);
 *   // ... run workload ...
 *   goc_stats_shutdown();
 */

#ifndef GOC_STATS_H
#define GOC_STATS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Event types
 * ---------------------------------------------------------------------- */


/**
 * goc_stats_event_type_t — discriminator for the goc_stats_event_t union.
 *
 * GOC_STATS_EVENT_POOL_STATUS    : pool created or destroyed.
 * GOC_STATS_EVENT_WORKER_STATUS  : worker lifecycle change.
 * GOC_STATS_EVENT_FIBER_STATUS   : fiber created or completed.
 * GOC_STATS_EVENT_CHANNEL_STATUS : channel opened or closed.
 */
typedef enum goc_stats_event_type {
    GOC_STATS_EVENT_POOL_STATUS,
    GOC_STATS_EVENT_WORKER_STATUS,
    GOC_STATS_EVENT_FIBER_STATUS,
    GOC_STATS_EVENT_CHANNEL_STATUS,
} goc_stats_event_type_t;

/**
 * goc_stats_pool_status_t — status value for GOC_STATS_EVENT_POOL_STATUS events.
 *
 * GOC_POOL_CREATED   : a new pool has been initialised.
 * GOC_POOL_DESTROYED : the pool has been shut down and its resources freed.
 */
typedef enum goc_stats_pool_status {
    GOC_POOL_CREATED = 0,
    GOC_POOL_DESTROYED = 1,
} goc_stats_pool_status_t;

/**
 * goc_stats_worker_status_t — status value for GOC_STATS_EVENT_WORKER_STATUS events.
 *
 * GOC_WORKER_CREATED : worker thread has been spawned.
 * GOC_WORKER_RUNNING : worker is executing a fiber.
 * GOC_WORKER_IDLE    : worker is parked waiting for work.
 * GOC_WORKER_STOPPED : worker thread has exited.
 */
typedef enum goc_stats_worker_status {
    GOC_WORKER_CREATED = 0,
    GOC_WORKER_RUNNING = 1,
    GOC_WORKER_IDLE    = 2,
    GOC_WORKER_STOPPED = 3,
} goc_stats_worker_status_t;

/**
 * goc_stats_fiber_status_t — status value for GOC_STATS_EVENT_FIBER_STATUS events.
 *
 * GOC_FIBER_CREATED   : fiber has been queued for execution.
 * GOC_FIBER_COMPLETED : fiber entry function has returned.
 */
typedef enum goc_stats_fiber_status {
    GOC_FIBER_CREATED   = 0,
    GOC_FIBER_COMPLETED = 1,
} goc_stats_fiber_status_t;

typedef struct goc_stats_event {
    goc_stats_event_type_t type;
    uint64_t timestamp;
    union {
        struct { int id; int status; int thread_count; } pool;
        struct {
            int      id;
            int      pool_id;
            int      status;
            int      pending_jobs;
            uint64_t steal_attempts;   /* lifetime steal attempts (only meaningful at STOPPED) */
            uint64_t steal_successes;  /* lifetime steal successes (only meaningful at STOPPED) */
            /* steal_misses and idle_wakeups are NOT in the per-event struct;
             * use goc_pool_get_steal_stats() to read aggregate lifetime totals. */
        } worker;
        struct { int id; int last_worker_id; int last_pool_id; int status; } fiber;
        struct {
            int      id;
            int      status;
            int      buf_size;
            int      item_count;
            uint64_t taker_scans;     /* alts take-arm scans on this channel (only at close) */
            uint64_t putter_scans;    /* alts put-arm scans on this channel (only at close) */
            uint64_t compaction_runs; /* compact_dead_entries calls (only at close) */
            uint64_t entries_removed; /* dead entries removed across all compactions (only at close) */
        } channel;
    } data;
} goc_stats_event_t;

/* -------------------------------------------------------------------------
 * Callback API
 *
 * goc_stats_set_callback — install (or clear) the event callback.
 *   cb  : function called for every emitted event; NULL to unregister.
 *   ud  : opaque pointer forwarded to cb unchanged.
 *
 * The callback is called with no internal locks held.  It may be replaced
 * or cleared at any time (thread-safe).  Setting NULL disables delivery.
 * ---------------------------------------------------------------------- */

/**
 * goc_stats_callback — event delivery function type.
 *
 * ev : the emitted event; valid only for the duration of the call.
 * ud : opaque user data registered with goc_stats_set_callback().
 *
 * Called on the libuv event loop thread with no internal locks held.
 */
typedef void (*goc_stats_callback)(const goc_stats_event_t* ev, void* ud);

/**
 * goc_stats_set_callback() — install (or clear) the event delivery callback.
 *
 * cb : function invoked for every emitted event; pass NULL to unregister.
 * ud : opaque pointer forwarded to cb unchanged.
 *
 * Thread-safe. The callback may be replaced or cleared at any time.
 */
void goc_stats_set_callback(goc_stats_callback cb, void* ud);

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

/** Initialise the stats subsystem. Must be called before any emit macro fires. */
void goc_stats_init(void);
/** Shut down the stats subsystem, flushing all pending events. */
void goc_stats_shutdown(void);
/** Return true when stats have been initialised and a callback is registered. */
bool goc_stats_is_enabled(void);

/**
 * goc_stats_flush() — block until all in-flight events have been delivered.
 *
 * Use before resetting test buffers to avoid a race between the async delivery
 * thread and the consumer.  No-op when stats are disabled.
 */
void goc_stats_flush(void);

/* -------------------------------------------------------------------------
 * Internal emit functions (called by macros below; not for direct use)
 * ---------------------------------------------------------------------- */

#ifdef GOC_ENABLE_STATS
void goc_stats_submit_event_pool(int id, int status, int thread_count);
void goc_stats_submit_event_worker(int id, int pool_id, int status, int pending_jobs,
                                   uint64_t steal_attempts, uint64_t steal_successes);
void goc_stats_submit_event_fiber(int id, int last_worker_id, int last_pool_id, int status);
void goc_stats_submit_event_channel(int id, int status, int buf_size, int item_count,
                                    uint64_t taker_scans, uint64_t putter_scans,
                                    uint64_t compaction_runs, uint64_t entries_removed);
/**
 * goc_timeout_get_stats() — read aggregate timeout counters.
 *
 * allocations : total goc_timeout() calls since process start.
 * expirations : total timers that have fired since process start.
 */
void   goc_timeout_get_stats(uint64_t *allocations, uint64_t *expirations);
/** goc_cb_queue_get_hwm() — return the high-water-mark depth of the callback queue. */
size_t goc_cb_queue_get_hwm(void);
/*
 * goc_pool_get_steal_stats — read aggregate work-stealing and idle counters.
 *
 * All four outputs are cumulative, relaxed-order lifetime totals across all
 * pools and workers.  They are not reset between benchmark runs.
 *
 * attempts     : total number of wsdq_steal_top calls made (both hint-path
 *                and fallback randomised scan).
 * successes    : subset of attempts that returned a non-NULL entry.
 * misses       : subset of attempts that returned NULL (wsdq empty or lost
 *                to a concurrent steal).  Equals attempts − successes.
 * idle_wakeups : number of times a worker returned from uv_sem_wait, i.e.
 *                one unit per sleep/wake cycle.  High values relative to
 *                successes indicate steal thrashing or spurious wakeups.
 */
void   goc_pool_get_steal_stats(uint64_t *attempts, uint64_t *successes,
                                uint64_t *misses,   uint64_t *idle_wakeups);
#endif

/* -------------------------------------------------------------------------
 * Emission macros (no-op unless GOC_ENABLE_STATS is defined)
 *
 * GOC_STATS_POOL_STATUS(id, status, thread_count)
 *   Emit a pool lifecycle event.
 * GOC_STATS_WORKER_STATUS(id, pool_id, status, pending_jobs, steal_att, steal_suc)
 *   Emit a worker lifecycle event with work-stealing counters.
 * GOC_STATS_FIBER_STATUS(id, last_worker_id, last_pool_id, status)
 *   Emit a fiber lifecycle event.
 * GOC_STATS_CHANNEL_STATUS(id, status, buf_size, item_count, ts, ps, cr, er)
 *   Emit a channel lifecycle event (ts=taker_scans, ps=putter_scans,
 *   cr=compaction_runs, er=entries_removed).
 * ---------------------------------------------------------------------- */

#ifdef GOC_ENABLE_STATS
#  define GOC_STATS_POOL_STATUS(id, status, thread_count) \
    goc_stats_submit_event_pool((id), (status), (thread_count))
#  define GOC_STATS_WORKER_STATUS(id, pool_id, status, pending_jobs, steal_att, steal_suc) \
    goc_stats_submit_event_worker((id), (pool_id), (status), (pending_jobs), (steal_att), (steal_suc))
#  define GOC_STATS_FIBER_STATUS(id, last_worker_id, last_pool_id, status) \
    goc_stats_submit_event_fiber((id), (last_worker_id), (last_pool_id), (status))
#  define GOC_STATS_CHANNEL_STATUS(id, status, buf_size, item_count, ts, ps, cr, er) \
    goc_stats_submit_event_channel((id), (status), (buf_size), (item_count), (ts), (ps), (cr), (er))
#else
#  define GOC_STATS_POOL_STATUS(id, status, thread_count)                            ((void)0)
#  define GOC_STATS_WORKER_STATUS(id, pool_id, status, pending_jobs, steal_att, suc) ((void)0)
#  define GOC_STATS_FIBER_STATUS(id, last_worker_id, last_pool_id, status)           ((void)0)
#  define GOC_STATS_CHANNEL_STATUS(id, status, buf_size, item_count, ts, ps, cr, er) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* GOC_STATS_H */
