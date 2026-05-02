# goc.h — Public API Reference

All declarations live in `include/goc.h`. No other header is required for the
core runtime; include `goc_io.h`, `goc_http.h`, `goc_array.h`, `goc_dict.h`,
or `goc_schema.h` separately when those subsystems are needed.

---

## Table of Contents

- [Initialisation and shutdown](#initialisation-and-shutdown)
- [Memory allocation](#memory-allocation)
- [String helpers](#string-helpers)
- [Value type](#value-type)
- [Channels](#channels)
- [Utilities](#utilities)
- [Fiber launch](#fiber-launch)
- [minicoro limitations](#minicoro-limitations)
- [Channel I/O — callbacks (any context)](#channel-io--callbacks-any-context)
- [Channel I/O — non-blocking (any context)](#channel-io--non-blocking-any-context)
- [Channel I/O — fiber context](#channel-io--fiber-context)
- [Channel I/O — blocking OS threads](#channel-io--blocking-os-threads)
- [Select (`goc_alts`)](#select-goc_alts)
- [Timeout channel](#timeout-channel)
- [RW mutexes](#rw-mutexes)
- [Thread pool](#thread-pool)
- [Scheduler loop access](#scheduler-loop-access)

---

## Initialisation and shutdown

| Function | Signature | Description |
|---|---|---|
| `goc_init` | `void goc_init(void)` | Initialise the runtime. Must be called exactly once as the first call in `main()`, from the process main thread. |
| `goc_shutdown` | `void goc_shutdown(void)` | Shut down the runtime. Blocks until all in-flight fibers finish, then tears down all pools, channels, and the event loop. Must be called from the process main thread. |

---

## Memory allocation

| Function | Signature | Description |
|---|---|---|
| `goc_malloc` | `void* goc_malloc(size_t n)` | Allocate `n` bytes on the GC-managed heap and return a `void*`. |
| `goc_new` | `T* goc_new(T)` | Allocate a single `T` value on the GC heap and return a `T*`. |
| `goc_new_n` | `T* goc_new_n(T, size_t n)` | Allocate an array of `n` `T` values on the GC heap and return a `T*`. |
| `goc_box` | `T* goc_box(T, val)` | Allocate a GC-managed copy of scalar `val` and return it as `T*`. |
| `goc_unbox` | `T goc_unbox(T, void* x)` | Dereference a boxed scalar pointer previously created by `goc_box`. |

```c
my_obj_t* obj = goc_new(my_obj_t);
my_obj_t** objs = goc_new_n(my_obj_t, 8);
int* boxed = goc_box(int, 42);
int value = goc_unbox(int, boxed);
// obj, objs, and boxed are all GC-managed and automatically reclaimed.

// The type parameter `T` passed to `goc_unbox(T, x)` must match the type
// originally used with `goc_box(T, val)`. Unboxing with a different scalar
// type is undefined behavior.
```

---

### Example: using `goc_malloc`, `goc_new`, and `goc_new_n`

```c
#include "goc.h"
#include <stdio.h>

typedef struct {
    double x;
    double y;
} Point;

int main(void) {
    goc_init();

    /* create a new Point object */
    Point* p = goc_new(Point);
    p->x = 1.5;
    p->y = 2.7;

    printf("Point(%f, %f)\n", p->x, p->y);

    /* create an array of Points */
    Point* ps = goc_new_n(Point, 5);
    Point* p0 = &ps[0];

    goc_shutdown();
    return 0;
}
```

**A few things to keep in mind:**

- `goc_malloc` is a thin wrapper around `GC_malloc`. Memory is zero-initialised.
- `goc_new(T)` allocates a single `T` on the GC heap and returns a `T*`.
- `goc_new_n(T, n)` allocates an array of `n` values of type `T` on the GC heap and returns a `T*`.

---

## String helpers

libgoc provides a GC-managed string formatting helper. The result is allocated on the Boehm GC heap; the caller must not `free` it.

| Function | Signature | Description |
|---|---|---|
| `goc_sprintf` | `char* goc_sprintf(const char* fmt, ...)` | Formats `fmt` and the given arguments into a null-terminated GC-heap string. Never returns `NULL`. |

```c
// Format a message and send it on a channel
char* msg = goc_sprintf("fiber %d ready", id);
goc_put(ch, msg);
```

---

## Value type

Channel take operations and select arms return a `goc_val_t` instead of a bare pointer. This distinguishes a genuine `NULL` value sent on a channel from a channel-closed notification, and (for non-blocking takes) from a channel that is open but momentarily empty.

```c
typedef enum {
    GOC_EMPTY  = -1,  /* channel open but no value available (goc_take_try only) */
    GOC_CLOSED =  0,  /* channel was closed */
    GOC_OK     =  1,  /* value delivered successfully */
} goc_status_t;

typedef struct {
    void*        val;  /* the received value; meaningful only when ok == GOC_OK */
    goc_status_t ok;
} goc_val_t;
```

---

## Channels

| Function | Signature | Description |
|---|---|---|
| `goc_chan_make` | `goc_chan* goc_chan_make(size_t buf_size)` | Create a channel. `buf_size == 0` → unbuffered (synchronous rendezvous). `buf_size > 0` → buffered ring of that capacity. The channel object itself is GC-managed; its internal mutex is plain-`malloc` allocated and is released during `goc_shutdown()`. |
| `goc_close` | `void goc_close(goc_chan* ch)` | Close a channel. The operation is idempotent: closing an already-closed channel is a no-op. Closing wakes all parked takers and putters with `ok=GOC_CLOSED`. Subsequent puts return `GOC_CLOSED`; subsequent takes drain any buffered values, then return `{NULL, GOC_CLOSED}`. |

```c
// Unbuffered
goc_chan* ch = goc_chan_make(0);

// Buffered (capacity 16)
goc_chan* ch = goc_chan_make(16);

goc_close(ch);
```

---

## Utilities

| Function | Signature | Description |
|---|---|---|
| `goc_in_fiber` | `bool goc_in_fiber(void)` | Returns `true` if the caller is executing inside a fiber, `false` otherwise. Safe to call from any context after `goc_init` returns. |
| `goc_current_pool` | `goc_pool* goc_current_pool(void)` | Return the current fiber's pool, or `NULL` when called outside fiber context. |
| `goc_current_or_default_pool` | `goc_pool* goc_current_or_default_pool(void)` | Return current pool when in fiber context, otherwise return the default pool. |
| `goc_current_thread` | `uv_thread_t goc_current_thread(void)` | Return the current OS thread id (libuv thread type). |
| `goc_current_fiber` | `void* goc_current_fiber(void)` | Return the current runtime fiber handle, or `NULL` outside fiber context. |

---

## Fiber launch

| Function | Signature | Description |
|---|---|---|
| `goc_go` | `goc_chan* goc_go(void (*fn)(void*), void* arg)` | Spawn a fiber on the current pool when called from a fiber; otherwise on the default pool. Returns a **join channel** that is closed automatically when the fiber returns. Pass the join channel as an arm to `goc_alts` or call `goc_take`/`goc_take_sync` on it to await fiber completion. The join channel may be ignored if no join is needed. |
| `goc_go_on` | `goc_chan* goc_go_on(goc_pool* pool, void (*fn)(void*), void* arg)` | Spawn on a specific pool. Returns a join channel with the same semantics as `goc_go`. |

```c
#include "goc_dict.h"

static void producer(void* arg) {
    goc_dict* d  = arg;
    goc_chan* ch = goc_dict_get(d, "ch", NULL);
    int       n  = goc_dict_get_unboxed(int, d, "n", 0);
    for (int i = 0; i < n; i++)
        goc_put_boxed(int, ch, i);
    goc_close(ch);
}

goc_chan* done = goc_go(producer, goc_dict_of(
    {"ch", goc_chan_make(0)},
    {"n",  goc_box(int, 10)}
));

// Await completion from a plain OS thread:
goc_take_sync(done);
```

---

## minicoro limitations

libgoc uses [minicoro](https://github.com/edubart/minicoro) for fiber switching. Three constraints apply:

- **C++ exceptions** must not propagate across fiber boundaries (across `mco_yield`/`mco_resume`). Declare fiber entry functions `extern "C"` in mixed C/C++ codebases.
- **Fixed stacks** — default canary-protected stacks are portable and overflow-safe (abort on overwrite). Use `goc_malloc` for large data instead of stack allocation. Enable dynamic stacks with `-DLIBGOC_VMEM=ON` if needed.
- **`src/minicoro.c`** is compiled with isolated flags that prevent GC/TLS conflicts and enable conservative-GC-friendly memory zeroing.

---

## Channel I/O — callbacks (any context)

Non-blocking. Safe from the libuv loop thread, pool threads, or any other context. The callback is always invoked on the **libuv loop thread** — do not call `goc_take`/`goc_put` from inside it.

| Function | Signature | Description |
|---|---|---|
| `goc_take_cb` | `void goc_take_cb(goc_chan* ch, void (*cb)(void* val, goc_status_t ok, void* ud), void* ud)` | Register a callback to receive the next value. `ok==GOC_CLOSED` means the channel was closed. |
| `goc_put_cb` | `void goc_put_cb(goc_chan* ch, void* val, void (*cb)(goc_status_t ok, void* ud), void* ud)` | Register a callback to deliver `val`. If `ok==GOC_CLOSED`, the value was not consumed. When the value is `goc_malloc`-allocated (the recommended pattern), no explicit cleanup is needed — the GC reclaims it automatically once it becomes unreachable. `cb` may be `NULL` if no completion notification is needed. |
| `goc_put_cb_boxed` | `void goc_put_cb_boxed(T, goc_chan* ch, T val, void (*cb)(goc_status_t ok, void* ud), void* ud)` | Shorthand for `goc_put_cb(ch, goc_box(T, val), cb, ud)` in any context. |

```c
// In a libuv read callback (loop thread) — forward data into a fiber:
static void on_put_done(goc_status_t ok, void* ud) {
    (void)ok; (void)ud;  // GC-allocated; no free needed even if channel closed
}

// on_read runs on the libuv loop thread — goc_put_cb is safe to call here.
// The callback (on_put_done) will also be invoked on the loop thread.
static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    if (nread <= 0) return;
    my_msg_t* msg = goc_new(my_msg_t);
    memcpy(msg->data, buf->base, nread);
    msg->len = nread;
    goc_put_cb(data_ch, msg, on_put_done, msg);
}

/* For scalar boxed values, the callback shorthand may be used instead: */
/* goc_put_cb_boxed(int, data_ch, 42, on_put_done, NULL); */
```

---

## Channel I/O — non-blocking (any context)

| Function | Signature | Description |
|---|---|---|
| `goc_take_try` | `goc_val_t* goc_take_try(goc_chan* ch)` | Non-blocking receive. Returns immediately from any context. Returns a GC-managed pointer to `{val, GOC_OK}` if a value was available, `{NULL, GOC_CLOSED}` if the channel is closed, or `{NULL, GOC_EMPTY}` if the channel is open but empty. Never suspends. |

---

## Channel I/O — fiber context

These functions may suspend the calling fiber. **Call only from inside a fiber**, not from the libuv loop thread or a plain OS thread.

| Function | Signature | Description |
|---|---|---|
| `goc_take` | `goc_val_t* goc_take(goc_chan* ch)` | Receive the next value from `ch`. Blocks until a value is available or the channel is closed. Returns a GC-managed pointer to `{val, GOC_OK}` on success, `{NULL, GOC_CLOSED}` on close. Asserts that the caller is running inside a fiber — calling from a bare OS thread aborts with a clear message. |
| `goc_take_all` | `goc_val_t** goc_take_all(goc_chan** chs, size_t n)` | Receive from each channel in `chs[0..n-1]` in order, then wait for all results. Returns a GC-managed array of `n` `goc_val_t*` results in the same order as `chs[]`. Each element follows `goc_take` semantics: `{val, GOC_OK}` on success, `{NULL, GOC_CLOSED}` on close. Must only be called from a fiber. |
| `goc_put` | `goc_status_t goc_put(goc_chan* ch, void* val)` | Send `val` into `ch`. Blocks until a receiver accepts or the channel is closed. Returns `GOC_OK` on success, `GOC_CLOSED` on close. Asserts that the caller is running inside a fiber — calling from a bare OS thread aborts with a clear message. |
| `goc_put_boxed` | `goc_status_t goc_put_boxed(T, goc_chan* ch, T val)` | Shorthand for `goc_put(ch, goc_box(T, val))` in fiber context. |

---

## Channel I/O — blocking OS threads

Blocks the calling OS thread (not a fiber). Calling these from a fiber is a runtime error and aborts with a diagnostic message. Also never call them from the libuv loop thread (this would deadlock the event loop).

| Function | Signature | Description |
|---|---|---|
| `goc_take_sync` | `goc_val_t* goc_take_sync(goc_chan* ch)` | Receive a value, blocking the OS thread. Returns a GC-managed pointer to `{val, GOC_OK}` on success, `{NULL, GOC_CLOSED}` on close. |
| `goc_take_all_sync` | `goc_val_t** goc_take_all_sync(goc_chan** chs, size_t n)` | Receive from each channel in `chs[0..n-1]` in order, then block the OS thread until all results are ready. Returns a GC-managed array of `n` `goc_val_t*` results in the same order as `chs[]`. Each element follows `goc_take_sync` semantics: `{val, GOC_OK}` on success, `{NULL, GOC_CLOSED}` on close. Must not be called from a fiber. |
| `goc_put_sync` | `goc_status_t goc_put_sync(goc_chan* ch, void* val)` | Send `val`, blocking the OS thread. Returns `GOC_OK` on success, `GOC_CLOSED` on close. |
| `goc_put_sync_boxed` | `goc_status_t goc_put_sync_boxed(T, goc_chan* ch, T val)` | Shorthand for `goc_put_sync_boxed(T, ch, val)` from an OS thread. |

---

## Select (`goc_alts`)

Non-deterministic choice across multiple channel operations.

| Function | Signature | Description |
|---|---|---|
| `goc_alts` | `goc_alts_result_t* goc_alts(goc_alt_op_t* ops, size_t n)` | **Fiber context only.** Wait until one of the `n` ops fires and return a GC-managed pointer to its result. If a `GOC_ALT_DEFAULT` arm is present and no other arm is immediately ready, the default arm fires without suspending the fiber. |
| `goc_alts_sync` | `goc_alts_result_t* goc_alts_sync(goc_alt_op_t* ops, size_t n)` | **Blocking OS-thread context only.** Same semantics as `goc_alts` but blocks the calling OS thread instead of suspending a fiber. Calling from fiber context is a runtime error and aborts with a diagnostic message. |

```c
typedef enum {
    GOC_ALT_TAKE,    /* receive from ch */
    GOC_ALT_PUT,     /* send put_val into ch */
    GOC_ALT_DEFAULT, /* fires immediately if no other arm is ready; ch must be NULL */
} goc_alt_kind_t;

typedef struct {
    goc_chan*    ch;        /* channel this arm operates on; NULL for GOC_ALT_DEFAULT */
    goc_alt_kind_t op_kind;  /* GOC_ALT_TAKE, GOC_ALT_PUT, or GOC_ALT_DEFAULT */
    void*        put_val;  /* value to send; used only when op_kind == GOC_ALT_PUT */
} goc_alt_op_t;

typedef struct {
    goc_chan* ch;     /* channel of the winning arm; NULL when GOC_ALT_DEFAULT fires */
    goc_val_t value;  /* received value and ok flag; value.ok==GOC_CLOSED means the channel was closed */
} goc_alts_result_t;
```

```c
// Blocking select: wait for whichever channel becomes ready first
goc_alt_op_t ops[] = {
    { .ch = data_ch,   .op_kind = GOC_ALT_TAKE },   // take from data_ch
    { .ch = cancel_ch, .op_kind = GOC_ALT_TAKE },   // take from cancel_ch
};
goc_alts_result_t* r = goc_alts(ops, 2);

if (r->ch == data_ch && r->value.ok == GOC_OK)
    process(r->value.val);
else
    return; // cancelled or channel closed
```

```c
// Non-blocking select with a default arm: never suspends the fiber
goc_alt_op_t ops[] = {
    { .ch = data_ch, .op_kind = GOC_ALT_TAKE },
    { .ch = NULL,    .op_kind = GOC_ALT_DEFAULT },  // fires if data_ch is empty
};
goc_alts_result_t* r = goc_alts(ops, 2);

if (r->ch == NULL)
    /* no value was ready — do something else */;
```

---

## Timeout channel

| Function | Signature | Description |
|---|---|---|
| `goc_timeout` | `goc_chan* goc_timeout(uint64_t ms)` | Return a channel that is **closed** after `ms` milliseconds. Use as an arm in `goc_alts` to implement deadlines. |

```c
goc_chan* tch = goc_timeout(500);
goc_alt_op_t ops[] = {
    { .ch = data_ch, .op_kind = GOC_ALT_TAKE },
    { .ch = tch,     .op_kind = GOC_ALT_TAKE },
};
goc_alts_result_t* r = goc_alts(ops, 2);

if (r->ch == tch)
    printf("timed out after 500 ms\n");
```

---

## RW mutexes

RW mutexes are channel-backed lock handles:

1. Call `goc_read_lock` or `goc_write_lock` to get a per-acquisition lock channel.
2. Call `goc_take` / `goc_take_sync` on that channel to wait until the lock is granted.
3. Call `goc_close` on that lock channel to release the lock.

Readers may hold the lock concurrently. Writers are exclusive. If a writer is queued,
subsequent readers queue behind it (writer preference) to avoid writer starvation.

| Function | Signature | Description |
|---|---|---|
| `goc_mutex_make` | `goc_mutex* goc_mutex_make(void)` | Create a new RW mutex. |
| `goc_read_lock` | `goc_chan* goc_read_lock(goc_mutex* mx)` | Request a read lock; returns a lock channel to await/close. |
| `goc_write_lock` | `goc_chan* goc_write_lock(goc_mutex* mx)` | Request a write lock; returns a lock channel to await/close. |

```c
goc_mutex* mx = goc_mutex_make();

/* Read side */
goc_chan* r = goc_read_lock(mx);
goc_take_sync(r);      /* lock acquired */
/* critical section */
goc_close(r);          /* release */

/* Write side */
goc_chan* w = goc_write_lock(mx);
goc_take_sync(w);      /* lock acquired */
/* critical section */
goc_close(w);          /* release */
```

---

## Thread pool

The default pool is created by `goc_init` with `max(4, hardware_concurrency)` worker threads. This can be overridden by setting the `GOC_POOL_THREADS` environment variable to a positive integer before calling `goc_init`. Invalid values (non-numeric, zero, or negative) are silently ignored and the default is used.

Pass `-DLIBGOC_DEBUG=ON` to CMake to enable verbose timestamped `[GOC_DBG]` diagnostic output to `stderr` from the scheduler, I/O, and HTTP layers at compile time. In debug builds, output is buffered in-process and must be flushed explicitly via `goc_dbg_flush()` (normal exit) or `goc_dbg_flush_signal_safe()` (from a signal handler); neither fires automatically. Off by default.

```c
typedef enum {
    GOC_DRAIN_OK      = 0,  /* all fibers finished within the deadline */
    GOC_DRAIN_TIMEOUT = 1,  /* deadline expired; pool remains valid and running */
} goc_drain_result_t;
```

| Function | Signature | Description |
|---|---|---|
| `goc_pool_make` | `goc_pool* goc_pool_make(size_t threads)` | Create a pool with `threads` worker threads. |
| `goc_default_pool` | `goc_pool* goc_default_pool(void)` | Return the default thread pool created by `goc_init`. The default pool is created with `max(4, hardware_concurrency)` worker threads (overridable via `GOC_POOL_THREADS`). The returned pointer is valid from `goc_init` until `goc_shutdown` returns. |
| `goc_pool_destroy` | `void goc_pool_destroy(goc_pool* pool)` | Wait for all in-flight fibers on the pool to complete naturally, then drain the run queue, join all worker threads, and release pool resources. Blocks indefinitely if any fiber is parked on a channel event that never arrives. Safe to call while fibers are still queued or running — the drain is the synchronisation barrier. **Must not be called from within a worker thread that belongs to `pool`** (including from a fiber running on that pool); that path aborts with a diagnostic message. |
| `goc_pool_destroy_timeout` | `goc_drain_result_t goc_pool_destroy_timeout(goc_pool* pool, uint64_t ms)` | Like `goc_pool_destroy`, but returns `GOC_DRAIN_OK` if the drain completes within `ms` milliseconds, or `GOC_DRAIN_TIMEOUT` if the timeout expires before all fibers have finished. On timeout the pool is **not** destroyed — worker threads continue running and the pool remains valid. The caller may retry later or take other action (e.g. closing channels to unblock parked fibers, then calling `goc_pool_destroy`). **Must not be called from within a worker thread that belongs to `pool`**; that path aborts with a diagnostic message. |

```c
goc_pool* io_pool = goc_pool_make(4);
goc_go_on(io_pool, my_fiber_fn, arg);
goc_pool_destroy(io_pool);
```

```c
if (goc_pool_destroy_timeout(io_pool, 2000) == GOC_DRAIN_TIMEOUT) {
    goc_close(shared_ch);
    goc_pool_destroy(io_pool);
}
```

---

### Example: custom thread pool with `goc_go_on`

```c
#include "goc.h"
#include <stdio.h>

#define N_WORKERS 4

static void worker(void* arg) {
    int id = goc_unbox(int, arg);
    printf("worker %d done\n", id);
}

int main(void) {
    goc_init();

    goc_pool* pool = goc_pool_make(N_WORKERS);

    for (int i = 0; i < N_WORKERS; i++)
        goc_go_on(pool, worker, goc_box(int, i));

    /*
     * Destroy the CPU pool.
     * Optional, shown here for completeness.
     * All undestroyed pools are automatically cleaned up by goc_shutdown().
     */
    goc_pool_destroy(pool);

    goc_shutdown();
    return 0;
}
```

**What this example demonstrates:**

- `goc_pool_make` / `goc_pool_destroy` — creates and tears down a dedicated pool, isolated from the default pool started by `goc_init`.
- `goc_go_on` — pins each worker fiber to the pool.
- `goc_pool_destroy` blocks until all fibers on the pool finish, then frees resources.

---

## Scheduler loop access

| Function | Signature | Description |
|---|---|---|
| `goc_scheduler` | `uv_loop_t* goc_scheduler(void)` | Return the `uv_loop_t*` owned by the runtime. Safe to call from any thread after `goc_init`. **Do not call `uv_run` or `uv_loop_close` on it.** |
