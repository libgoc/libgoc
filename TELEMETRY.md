# libgoc - Telemetry (`goc_stats`)

> Synchronous, event-based telemetry for fibers, workers, and channels in libgoc.

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
5. [Build Flags](#build-flags)
6. [Examples](#examples)
7. [Testing](#testing)


---

## Overview

`goc_stats` provides a synchronous, callback-based telemetry system for libgoc. It emits events for key runtime objects (fibers, workers, channels) at lifecycle and status transitions. Events are delivered immediately on the emitting thread, allowing for low-latency monitoring, debugging, and integration with external systems.

---

## Design

- **Synchronous delivery:** Events are emitted synchronously on the thread that triggers them (pool worker, main thread, etc.). Callbacks must be fast and non-blocking.
- **Event types:** Fiber, worker, and channel events are supported. Each event includes a timestamp and relevant fields.
- **Build-time enable/disable:** Telemetry is enabled by defining `GOC_ENABLE_STATS` at build time. When disabled, all macros become no-ops and have zero runtime cost.
- **Thread safety:** The callback may be invoked from any thread. Use a mutex or buffer to hand off events for heavy processing.

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
| Field          | Type  | Description                    |
|----------------|-------|--------------------------------|
| `id`           | `int` | Worker index                   |
| `status`       | `int` | See `goc_stats_worker_status`  |
| `pending_jobs` | `int` | Live fiber count in the pool   |

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
| Field        | Type  | Description                   |
|--------------|-------|-------------------------------|
| `id`         | `int` | Channel pointer (cast to int) |
| `status`     | `int` | `1` = open, `0` = closed      |
| `buf_size`   | `int` | Declared buffer capacity      |
| `item_count` | `int` | Items in buffer at event time |

---

## API Reference

### Lifecycle

```c
#include "goc_stats.h"

void goc_stats_init(void);
void goc_stats_shutdown(void);
bool goc_stats_is_enabled(void);
```

### Callback Registration

```c
typedef void (*goc_stats_callback)(const struct goc_stats_event* ev, void* ud);
void goc_stats_set_callback(goc_stats_callback cb, void* ud);
```

By default, a callback is installed that prints all events to stdout in a human-readable format. This is useful for debugging and quick inspection. You can override this by calling `goc_stats_set_callback` with your own function. Pass `NULL` to disable all event delivery.

The callback is called with no internal locks held and may be replaced at any time.

### Event Structure

```c
struct goc_stats_event {
	enum goc_stats_event_type type;
	uint64_t timestamp;
	union {
		struct { int id; int status; int thread_count; } pool;
		struct { int id; int status; int pending_jobs; } worker;
		struct { int id; int last_worker_id; int status; } fiber;
		struct { int id; int status; int buf_size; int item_count; } channel;
	} data;
};
```

### Emission Macros

Macros emit events if telemetry is enabled, or become no-ops otherwise:

```c
#ifdef GOC_ENABLE_STATS
#  define GOC_STATS_POOL_STATUS(id, status, thread_count) \
	goc_stats_submit_event_pool((id), (status), (thread_count))
#  define GOC_STATS_WORKER_STATUS(id, status, pending_jobs) \
	goc_stats_submit_event_worker((id), (status), (pending_jobs))
#  define GOC_STATS_FIBER_STATUS(id, last_worker_id, status) \
	goc_stats_submit_event_fiber((id), (last_worker_id), (status))
#  define GOC_STATS_CHANNEL_STATUS(id, status, buf_size, item_count) \
	goc_stats_submit_event_channel((id), (status), (buf_size), (item_count))
#else
#  define GOC_STATS_POOL_STATUS(id, status, thread_count)            ((void)0)
#  define GOC_STATS_WORKER_STATUS(id, status, pending_jobs)          ((void)0)
#  define GOC_STATS_FIBER_STATUS(id, last_worker_id, status)         ((void)0)
#  define GOC_STATS_CHANNEL_STATUS(id, status, buf_size, item_count) ((void)0)
#endif
```

---

## Build Flags

- To enable telemetry, build with `-DGOC_ENABLE_STATS=ON` (CMake) or `GOC_ENABLE_STATS=1` (Makefile).
- When disabled, all macros are no-ops and have zero runtime cost.

---

## Examples



### Default stdout output

After `goc_stats_init()`, all events are printed to stdout by the built-in default callback. For a program that creates a pool, spawns a fiber, and shuts down, the output looks like:

```
[goc_stats] WORKER @ 1748000000000000000: id=0 status=0 pending=0
[goc_stats] WORKER @ 1748000000001000000: id=1 status=0 pending=0
[goc_stats] POOL @ 1748000000002000000: pool=0x55a1b2c3d000 status=0 threads=2
[goc_stats] FIBER @ 1748000000003000000: id=0 last_worker=-1 status=0
[goc_stats] WORKER @ 1748000000004000000: id=0 status=1 pending=1
[goc_stats] FIBER @ 1748000000005000000: id=0 last_worker=0 status=1
[goc_stats] WORKER @ 1748000000006000000: id=0 status=2 pending=0
[goc_stats] POOL @ 1748000000007000000: pool=0x55a1b2c3d000 status=1 threads=2
[goc_stats] WORKER @ 1748000000008000000: id=0 status=3 pending=0
[goc_stats] WORKER @ 1748000000009000000: id=1 status=3 pending=0
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
GOC_STATS_POOL_STATUS(goc_box_int(pool), GOC_POOL_CREATED, thread_count); // pool created
GOC_STATS_POOL_STATUS(goc_box_int(pool), GOC_POOL_DESTROYED, thread_count); // pool destroyed
GOC_STATS_WORKER_STATUS(worker_id, GOC_WORKER_RUNNING, pending_jobs);
GOC_STATS_FIBER_STATUS(fiber_id, last_worker_id, GOC_FIBER_CREATED);
GOC_STATS_CHANNEL_STATUS(goc_box_int(ch), 1, ch->buf_size, 0); // channel open
GOC_STATS_CHANNEL_STATUS(goc_box_int(ch), 0, ch->buf_size, 0); // channel close
```

---

## Testing

- See `tests/test_goc_stats.c` for comprehensive tests of event emission, callback delivery, and field correctness.
- Run with telemetry enabled to verify all event types and edge cases.

