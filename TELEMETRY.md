# libgoc - Telemetry (`goc_stats`)

> Asynchronous, event-based telemetry for pools, workers, fibers, and channels in libgoc.

---

## Table of Contents

1. [Overview](#overview)
2. [Design](#design)
3. [Event Types](#event-types)
4. [API Reference](#api-reference)
	- [Lifecycle](#lifecycle)
	- [Callback Registration](#callback-registration)
	- [Event Structure](#event-structure)
	- [Emission Macros](#emission-macros)
	- [Telemetry Accessors](#telemetry-accessors)
5. [Build Flags](#build-flags)
6. [Examples](#examples)
7. [Testing](#testing)


---

## Overview

`goc_stats` provides an asynchronous telemetry system for libgoc. It emits events for key runtime objects (fibers, workers, channels) at lifecycle and status transitions. Events are pushed onto an internal lock-free MPSC queue and delivered on the libuv loop thread, so emitting threads are never blocked by the callback.

---

## Design

- **Asynchronous delivery:** Each event is copied into a heap-allocated node and pushed onto a lock-free MPSC queue. The emitting thread then calls `uv_async_send` on a single persistent handle and returns immediately. The loop thread drains the queue and fires the callback.
- **Event types:** pool, fiber, worker, and channel events are supported. Each event includes a timestamp and relevant fields.
- **Build-time enable/disable:** Telemetry is enabled by defining `GOC_ENABLE_STATS` at build time. When disabled, all macros become no-ops and have zero runtime cost. **Note:** THIS IS DISABLED IN PRE-BUILT BINARIES.
- **Thread safety:** `goc_stats_set_callback` is safe to call from any thread. The callback always runs on the loop thread; no additional locking is needed to access loop-thread state from it.

---


## Event Types

### Pool Events (`GOC_STATS_EVENT_POOL_STATUS`)
| Field          | Type    | Description                   |
|----------------|---------|-------------------------------|
| `id`           | `void*` | Pool pointer                  |
| `status`       | `int`   | See `goc_stats_pool_status`   |
| `thread_count` | `int`   | Number of worker threads      |

| `goc_stats_pool_status` | Value | Meaning          |
|-------------------------|-------|------------------|
| `GOC_POOL_CREATED`      | `0`   | Pool initialised |
| `GOC_POOL_DESTROYED`    | `1`   | Pool torn down   |

### Worker Events (`GOC_STATS_EVENT_WORKER_STATUS`)
| Field              | Type       | Description                                                        |
|--------------------|------------|--------------------------------------------------------------------|
| `id`               | `int`      | Worker index                                                       |
| `pool_id`          | `void*`    | Pool pointer                                                       |
| `status`           | `int`      | See `goc_stats_worker_status`                                      |
| `pending_jobs`     | `int`      | Live fiber count in the pool                                       |
| `steal_attempts`   | `uint64_t` | Lifetime steal attempts for this worker (only meaningful at `STOPPED`) |
| `steal_successes`  | `uint64_t` | Lifetime steal successes for this worker (only meaningful at `STOPPED`) |

| `goc_stats_worker_status` | Value | Meaning                       |
|---------------------------|-------|-------------------------------|
| `GOC_WORKER_CREATED`      | `0`   | Thread started                |
| `GOC_WORKER_RUNNING`      | `1`   | Picked up a fiber to execute  |
| `GOC_WORKER_IDLE`         | `2`   | No work; sleeping on semaphore|
| `GOC_WORKER_STOPPED`      | `3`   | Thread exiting                |

### Fiber Events (`GOC_STATS_EVENT_FIBER_STATUS`)
| Field            | Type  | Description                         |
|------------------|-------|-------------------------------------|
| `id`             | `int` | Monotonically increasing fiber ID   |
| `last_worker_id` | `int` | Worker that last ran this fiber (`-1` if not yet scheduled) |
| `status`         | `int` | See `goc_stats_fiber_status`        |

| `goc_stats_fiber_status` | Value | Meaning                      |
|--------------------------|-------|------------------------------|
| `GOC_FIBER_CREATED`      | `0`   | Fiber allocated and enqueued |
| `GOC_FIBER_COMPLETED`    | `1`   | Fiber function returned      |

### Channel Events (`GOC_STATS_EVENT_CHANNEL_STATUS`)
| Field              | Type       | Description                                                              |
|--------------------|------------|--------------------------------------------------------------------------|
| `id`               | `int`      | Channel pointer (cast to int)                                            |
| `status`           | `int`      | `1` = open, `0` = closed                                                 |
| `buf_size`         | `int`      | Declared buffer capacity                                                 |
| `item_count`       | `int`      | Items in buffer at event time                                            |
| `taker_scans`      | `uint64_t` | Times a take arm scanned this channel in `goc_alts` (only at close)     |
| `putter_scans`     | `uint64_t` | Times a put arm scanned this channel in `goc_alts` (only at close)      |
| `compaction_runs`  | `uint64_t` | Times `compact_dead_entries` ran on this channel (only at close)        |
| `entries_removed`  | `uint64_t` | Total dead entries removed across all compactions (only at close)       |

---

## API Reference

### Lifecycle

```c
#include "goc_stats.h"

void goc_stats_init(void);
void goc_stats_shutdown(void);
bool goc_stats_is_enabled(void);

/* Block until the stats delivery loop has drained all in-flight events.
 * Use before resetting test buffers to avoid races with the async delivery
 * thread.  No-op when stats are disabled. */
void goc_stats_flush(void);
```

### Callback Registration

```c
typedef void (*goc_stats_callback)(const struct goc_stats_event* ev, void* ud);
void goc_stats_set_callback(goc_stats_callback cb, void* ud);
```

By default, a callback is installed that prints all events to stdout in a human-readable format. This is useful for debugging and quick inspection. You can override this by calling `goc_stats_set_callback` with your own function. Pass `NULL` to disable all event delivery.

The callback is always invoked on the libuv loop thread, with no internal locks held, and may be replaced at any time from any thread. `goc_stats_shutdown()` blocks until all queued events have been delivered, so it is safe to tear down callback resources immediately after it returns. Calling `goc_stats_set_callback(NULL, NULL)` disables delivery for subsequent events; any events already in the queue at that moment will be drained and silently dropped.

### Event Structure

```c
struct goc_stats_event {
	enum goc_stats_event_type type;
	uint64_t timestamp;
	union {
		struct { void* id; int status; int thread_count; } pool;
		struct {
			int      id;
			void*    pool_id;
			int      status;
			int      pending_jobs;
			uint64_t steal_attempts;   /* only meaningful at STOPPED */
			uint64_t steal_successes;  /* only meaningful at STOPPED */
		} worker;
		struct { int id; int last_worker_id; int status; } fiber;
		struct {
			int      id;
			int      status;
			int      buf_size;
			int      item_count;
			uint64_t taker_scans;     /* only at close */
			uint64_t putter_scans;    /* only at close */
			uint64_t compaction_runs; /* only at close */
			uint64_t entries_removed; /* only at close */
		} channel;
	} data;
};
```

### Emission Macros

Macros emit events if telemetry is enabled, or become no-ops otherwise:

```c
#ifdef GOC_ENABLE_STATS
#  define GOC_STATS_POOL_STATUS(id, status, thread_count) \
	goc_stats_submit_event_pool((id), (status), (thread_count))
#  define GOC_STATS_WORKER_STATUS(id, pool_id, status, pending_jobs, steal_att, steal_suc) \
	goc_stats_submit_event_worker((id), (pool_id), (status), (pending_jobs), (steal_att), (steal_suc))
#  define GOC_STATS_FIBER_STATUS(id, last_worker_id, status) \
	goc_stats_submit_event_fiber((id), (last_worker_id), (status))
#  define GOC_STATS_CHANNEL_STATUS(id, status, buf_size, item_count, ts, ps, cr, er) \
	goc_stats_submit_event_channel((id), (status), (buf_size), (item_count), (ts), (ps), (cr), (er))
#else
#  define GOC_STATS_POOL_STATUS(id, status, thread_count)                             ((void)0)
#  define GOC_STATS_WORKER_STATUS(id, pool_id, status, pending_jobs, steal_att, suc)  ((void)0)
#  define GOC_STATS_FIBER_STATUS(id, last_worker_id, status)                          ((void)0)
#  define GOC_STATS_CHANNEL_STATUS(id, status, buf_size, item_count, ts, ps, cr, er)  ((void)0)
#endif
```

### Telemetry Accessors

When `GOC_ENABLE_STATS` is defined, three global accessor functions are available for reading aggregate runtime counters directly (without going through the callback mechanism):

```c
/* Steal counters — aggregate across all pools and workers, lifetime totals */
void goc_pool_get_steal_stats(uint64_t *attempts, uint64_t *successes);

/* Timeout channel counters */
void goc_timeout_get_stats(uint64_t *allocations, uint64_t *expirations);

/* Callback queue peak depth since process start */
size_t goc_cb_queue_get_hwm(void);
```

When `GOC_ENABLE_STATS` is not defined these functions are still present but always write zeros / return zero, so callers do not need `#ifdef` guards.

---

## Build Flags

- To enable telemetry, build with `-DGOC_ENABLE_STATS=ON` (CMake) or `GOC_ENABLE_STATS=1` (Makefile).
- When disabled, all macros are no-ops and have zero runtime cost.

---

## Examples



### Default stdout output

After `goc_stats_init()`, all events are printed to stdout by the built-in default callback. For a program that creates a pool, spawns a fiber, and shuts down, the output looks like:

```
[goc_stats] WORKER @ 1748000000000000000: id=0 pool=0x55a1b2c3d000 status=0 pending=0
[goc_stats] WORKER @ 1748000000001000000: id=1 pool=0x55a1b2c3d000 status=0 pending=0
[goc_stats] POOL @ 1748000000002000000: pool=0x55a1b2c3d000 status=0 threads=2
[goc_stats] FIBER @ 1748000000003000000: id=0 last_worker=-1 status=0
[goc_stats] WORKER @ 1748000000004000000: id=0 pool=0x55a1b2c3d000 status=1 pending=1
[goc_stats] FIBER @ 1748000000005000000: id=0 last_worker=0 status=1
[goc_stats] WORKER @ 1748000000006000000: id=0 pool=0x55a1b2c3d000 status=2 pending=0
[goc_stats] POOL @ 1748000000007000000: pool=0x55a1b2c3d000 status=1 threads=2
[goc_stats] WORKER @ 1748000000008000000: id=0 pool=0x55a1b2c3d000 status=3 pending=0
[goc_stats] WORKER @ 1748000000009000000: id=1 pool=0x55a1b2c3d000 status=3 pending=0
```

See the [Event Types](#event-types) tables for status enum values.

### Custom callback

To suppress the default output or handle events differently, call `goc_stats_set_callback` with your own function or with `NULL` to disable.

```c
#include "goc_stats.h"

static void my_stats_cb(const struct goc_stats_event* ev, void* ud) {
    // ... handle event ...
}

int main(void) {
    goc_stats_init();
    goc_stats_set_callback(my_stats_cb, NULL); // override default
    // ... run workload ...
    goc_stats_shutdown();
    return 0;
}
```


### Emitting events (internal use)

```c
GOC_STATS_POOL_STATUS(goc_box_int(pool), GOC_POOL_CREATED, thread_count);
GOC_STATS_POOL_STATUS(goc_box_int(pool), GOC_POOL_DESTROYED, thread_count);
/* steal_attempts and steal_successes are 0 except at STOPPED */
GOC_STATS_WORKER_STATUS(worker_id, pool, GOC_WORKER_RUNNING, pending_jobs, 0, 0);
GOC_STATS_WORKER_STATUS(worker_id, pool, GOC_WORKER_STOPPED, 0, steal_att, steal_suc);
GOC_STATS_FIBER_STATUS(fiber_id, last_worker_id, GOC_FIBER_CREATED);
/* scan/compaction counters are 0 at open; populated at close */
GOC_STATS_CHANNEL_STATUS((int)(intptr_t)ch, 1, ch->buf_size, 0, 0, 0, 0, 0); // open
GOC_STATS_CHANNEL_STATUS((int)(intptr_t)ch, 0, ch->buf_size, ch->item_count,
                         taker_scans, putter_scans, compaction_runs, entries_removed); // close
```

---

## Testing

See `tests/test_goc_stats.c` for tests.