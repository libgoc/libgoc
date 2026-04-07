/*
 * goc_stats.h — Public API for libgoc telemetry
 *
 * Events are delivered synchronously via a registered callback.  The callback
 * is called on whichever thread emits the event (pool workers, main thread,
 * etc.) so it must be async-signal-safe with respect to the caller: keep it
 * short and non-blocking.  Use a mutex + buffer in the callback to hand off to
 * a consumer thread if heavier processing is needed.
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


typedef enum goc_stats_event_type {
    GOC_STATS_EVENT_POOL_STATUS,
    GOC_STATS_EVENT_WORKER_STATUS,
    GOC_STATS_EVENT_FIBER_STATUS,
    GOC_STATS_EVENT_CHANNEL_STATUS,
} goc_stats_event_type_t;

typedef enum goc_stats_pool_status {
    GOC_POOL_CREATED = 0,
    GOC_POOL_DESTROYED = 1,
} goc_stats_pool_status_t;

typedef enum goc_stats_worker_status {
    GOC_WORKER_CREATED = 0,
    GOC_WORKER_RUNNING = 1,
    GOC_WORKER_IDLE    = 2,
    GOC_WORKER_STOPPED = 3,
} goc_stats_worker_status_t;

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

typedef void (*goc_stats_callback)(const goc_stats_event_t* ev, void* ud);

void goc_stats_set_callback(goc_stats_callback cb, void* ud);

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

void goc_stats_init(void);
void goc_stats_shutdown(void);
bool goc_stats_is_enabled(void);

/* Block until the stats delivery loop has drained all in-flight events from
 * the internal queue and delivered them to the registered callback.  Use this
 * before resetting test buffers to avoid a race between the async delivery
 * thread and the consumer. No-op when stats are disabled. */
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
/* Telemetry accessors — available when GOC_ENABLE_STATS is defined */
void   goc_timeout_get_stats(uint64_t *allocations, uint64_t *expirations);
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
