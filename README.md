[![CI](https://github.com/divs1210/libgoc/actions/workflows/ci.yml/badge.svg)](https://github.com/divs1210/libgoc/actions/workflows/ci.yml)

# libgoc
> A Go-style CSP concurrency runtime for C: threadpools, stackful coroutines, channels, select, async I/O, and garbage collection in one coherent API.

**libgoc** is a runtime library for C programs that want Go-style CSP concurrency backed by a managed memory environment. It is written in plain C for maximum reach and portability.

The library provides stackful coroutines ("fibers"), channels, a select primitive (`goc_alts`), timeout channels, and a managed thread pool. Boehm GC is a required dependency and is linked automatically.

**libgoc is built for:**

- **C developers** who want a Go-style concurrency runtime without switching to Go.
- **Language implementors** targeting C/C++ as a compilation target, or writing multithreaded interpreters.

See the [Design Doc](./DESIGN.md) for implementation details and [TODO](./TODO.md) for planned future work.

> **Boehm GC must be compiled with thread support.** libgoc calls `GC_allow_register_threads()` during initialisation. With `-DGC_THREADS` active, the GC's pthread wrapper automatically registers every pool worker thread and the uv loop thread via `GC_call_with_stack_base` — all threads are created with `GC_pthread_create` and must not call `GC_register_my_thread` / `GC_unregister_my_thread` manually, as doing so double-registers the thread and causes a crash. A non-threaded build of the GC (i.e. one built without `--enable-threads`) is missing the required symbols and will crash at runtime. See the per-platform instructions in [Building and Testing](#building-and-testing) for how to satisfy this requirement on each OS.

**Platform:** Linux, macOS, and Windows.

**Dependencies:**

| | |
|---|---|
| `minicoro` | fiber suspend/resume (cross-platform; POSIX and Windows) |
| `libuv` | event loop, timers, cross-thread wakeup |
| Boehm GC | garbage collection; **must be built with `--enable-threads`**; linked automatically, initialised by `goc_init`; owns thread pool worker creation via `GC_pthread_create` / `GC_pthread_join` |

> See [minicoro limitations](#minicoro-limitations) in the Public API section for fiber stack constraints.

---

## Table of Contents

- [Examples](#examples)
  - [1. Ping-pong](#1-ping-pong)
  - [2. Custom thread pool with `goc_go_on`](#2-custom-thread-pool-with-goc_go_on)
  - [3. Using goc_malloc](#3-using-goc_malloc)
- [Public API](#public-api)
  - [Initialisation and shutdown](#initialisation-and-shutdown)
  - [Memory allocation](#memory-allocation)
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
  - [Thread pool](#thread-pool)
  - [Scheduler loop access](#scheduler-loop-access)
- [Building and Testing](#building-and-testing)
  - [Prerequisites](#prerequisites)
  - [macOS](#macos)
  - [Linux](#linux)
  - [Windows](#windows)
  - [Build types](#build-types)
  - [Shared library](#shared-library)
  - [Code coverage](#code-coverage)
  - [Sanitizers](#sanitizers)

---

## Examples

### 1. Ping-pong

Two fibers exchange a message back and forth over a pair of unbuffered channels.
This is the canonical CSP "ping-pong" pattern — each fiber blocks on a take,
then immediately puts to wake the other side.

```c
#include "goc.h"
#include <stdio.h>

#define N_ROUNDS 5

typedef struct {
    goc_chan* recv;   /* channel this fiber reads from  */
    goc_chan* send;   /* channel this fiber writes to   */
    const char* name;
} player_args_t;

static void player(void* arg) {
    player_args_t* a = arg;

    goc_val_t v;
    while ((v = goc_take(a->recv)).ok == GOC_OK) {
        int count = (int)(intptr_t)v.val;
        printf("%s %d\n", a->name, count);
        if (count >= N_ROUNDS) {
            goc_close(a->send);
            return;
        }
        goc_put(a->send, (void*)(intptr_t)(count + 1));
    }
}

int main(void) {
    goc_init();

    goc_chan* a_to_b = goc_chan_make(0);
    goc_chan* b_to_a = goc_chan_make(0);

    player_args_t ping_args = { .recv = b_to_a, .send = a_to_b, .name = "ping" };
    player_args_t pong_args = { .recv = a_to_b, .send = b_to_a, .name = "pong" };

    goc_chan* done_ping = goc_go(player, &ping_args);
    goc_chan* done_pong = goc_go(player, &pong_args);

    /* Kick off the exchange with the first message. */
    goc_put_sync(a_to_b, (void*)(intptr_t)1);

    /* Wait for both fibers to finish. */
    goc_take_sync(done_ping);
    goc_take_sync(done_pong);

    goc_shutdown();
    return 0;
}
```

**What this example demonstrates:**

- `goc_chan_make(0)` — unbuffered channels enforce a synchronous rendezvous:
  each `goc_put` blocks until the other fiber calls `goc_take`, and vice versa.
- `goc_go` — spawns both player fibers on the default pool and returns a join
  channel that is closed automatically when the fiber returns.
- `goc_put_sync` — sends the opening message from the main OS thread (outside
  any fiber) without blocking a pool worker.
- `goc_close` — when the round limit is reached the active fiber closes the
  forward channel, causing the partner's next `goc_take` to return
  `GOC_CLOSED` and exit its loop cleanly.
- `goc_take_sync` — blocks the main thread until each join channel is closed,
  providing a simple, channel-based join without extra synchronisation.

---

### 2. Custom thread pool with `goc_go_on`

Use `goc_pool_make` when you need workloads isolated from the default pool —
for example, CPU-bound tasks that should not starve I/O fibers.

This example fans out several independent jobs onto a dedicated pool, then
collects all results from main using `goc_take_sync`.

```c
#include "goc.h"
#include <stdio.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------
 * Worker fiber: sum integers in [lo, hi) and send the result back.
 * In a real program this would be image processing, compression, etc.
 * ------------------------------------------------------------------------- */
typedef struct {
    long      lo, hi;
    goc_chan* result_ch;
} worker_args_t;

static void sum_range(void* arg) {
    worker_args_t* a = arg;
    long acc = 0;
    for (long i = a->lo; i < a->hi; i++)
        acc += i;
    goc_put(a->result_ch, (void*)(intptr_t)acc);
}

/* =========================================================================
 * main
 * ========================================================================= */
#define N_WORKERS 4
#define RANGE     1000000L

int main(void) {
    goc_init();

    /*
     * A dedicated pool for CPU-bound work. The default pool (started by
     * goc_init) is left free for I/O fibers and other goc_go calls.
     */
    goc_pool* cpu_pool = goc_pool_make(N_WORKERS);

    goc_chan*     result_ch = goc_chan_make(0);
    worker_args_t workers[N_WORKERS];
    long          chunk = RANGE / N_WORKERS;

    /* Fan out: spawn each worker on the CPU pool with goc_go_on. */
    for (int i = 0; i < N_WORKERS; i++) {
        workers[i].lo        = i * chunk;
        workers[i].hi        = (i == N_WORKERS - 1) ? RANGE : (i + 1) * chunk;
        workers[i].result_ch = result_ch;
        goc_go_on(cpu_pool, sum_range, &workers[i]);
    }

    /* Fan in: collect results from the main thread with goc_take_sync. */
    long total = 0;
    for (int i = 0; i < N_WORKERS; i++) {
        goc_val_t v = goc_take_sync(result_ch);
        if (v.ok == GOC_OK) total += (long)(intptr_t)v.val;
    }

    printf("sum [0, %ld) = %ld\n", RANGE, total);

    /*
     * Destroy the CPU pool. 
     * Optional, shown here for completeness.
     * All undestroyed pools are automatically cleaned up by goc_shutdown().
     */
    goc_pool_destroy(cpu_pool);

    goc_shutdown();
    return 0;
}
```

**What this example demonstrates:**

- `goc_pool_make` / `goc_pool_destroy` — creates and tears down a dedicated
  pool, isolated from the default pool started by `goc_init`.
- `goc_go_on` — pins each worker fiber to the CPU pool.
- Fan-out / fan-in over a shared result channel — no explicit synchronisation
  primitives beyond channels.
- `goc_pool_destroy` blocks till all the fibers running on the pool finish, then frees resources.
  `goc_shutdown` tears down the rest of the runtime.

---

### 3. Using goc_malloc

`goc_malloc` allocates memory on the Boehm GC heap. Allocations are collected
automatically when no longer reachable — no `free` is needed. This is the
intended allocator for long-lived program objects: nodes, buffers, application data, and so on.

```c
#include "goc.h"
#include <stdio.h>

typedef struct node_t {
    int           value;
    struct node_t* next;
} node_t;

static void build_list(void* arg) {
    goc_chan* result_ch = arg;

    /* Build a linked list entirely on the GC heap — no free() required. */
    node_t* head = NULL;
    for (int i = 9; i >= 0; i--) {
        node_t* n = goc_malloc(sizeof(node_t));
        n->value = i;
        n->next  = head;
        head     = n;
    }

    goc_put(result_ch, head);
}

int main(void) {
    goc_init();

    goc_chan* result_ch = goc_chan_make(0);
    goc_go(build_list, result_ch);

    goc_val_t v = goc_take_sync(result_ch);
    if (v.ok == GOC_OK) {
        for (node_t* n = v.val; n; n = n->next)
            printf("%d ", n->value);
        printf("\n");
    }

    goc_shutdown();
    return 0;
}
```

**A few things to keep in mind:**

- `goc_malloc` is a thin wrapper around `GC_malloc`. Memory is zero-initialised.
- **libuv handles** must be allocated with plain `malloc`. See [`goc_malloc`](#memory-allocation).

---

## Public API

### Initialisation and shutdown

| Function | Signature | Description |
|---|---|---|
| `goc_init` | `void goc_init(void)` | Initialise the runtime. Must be called **exactly once**, as the **first call in `main()`**, before any other library or application code that could trigger GC allocation. Must be called from the process main thread; calling from any non-main thread prints an error to `stderr` and aborts. Calls `GC_INIT()` and `GC_allow_register_threads()` unconditionally — do not call these yourself. Initialises the callback queue, the live-channel registry, and both async handles, then spawns the libuv loop thread and creates the default thread pool. Calling `goc_init` more than once is undefined behaviour. |
| `goc_shutdown` | `void goc_shutdown(void)` | Shut down the runtime. Blocks until all in-flight fibers on every pool have completed naturally, then tears down all pools (the default pool and any user-created pools not yet destroyed), destroys channel mutexes, signals the loop thread to close all handles, joins the loop thread, and frees the event loop. Must be called from the process main thread (the same thread that called `goc_init`); calling from any non-main thread prints an error to `stderr` and aborts. Will block forever if any fiber is waiting on a channel event that will never arrive. May not be called again after it returns. |

---

### Memory allocation

| Function | Signature | Description |
|---|---|---|
| `goc_malloc` | `void* goc_malloc(size_t n)` | Allocate `n` bytes on the GC-managed heap. No `free` required — the collector reclaims unreachable memory automatically. Backed by `GC_malloc` internally. |

```c
my_obj_t* obj = goc_malloc(sizeof(my_obj_t));
// obj is automatically collected when no longer reachable
```

> **Note:** libuv handles (`uv_timer_t`, `uv_async_t`, etc.) must still be allocated with plain `malloc`, not `goc_malloc`. libuv holds internal references to handles until `uv_close` completes; GC-allocated handles risk collection before `uv_close` fires.

---

### Value type

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

### Channels

| Function | Signature | Description |
|---|---|---|
| `goc_chan_make` | `goc_chan* goc_chan_make(size_t buf_size)` | Create a channel. `buf_size == 0` → unbuffered (synchronous rendezvous). `buf_size > 0` → buffered ring of that capacity. |
| `goc_close` | `void goc_close(goc_chan* ch)` | Close a channel, waking all parked fibers with `ok=GOC_CLOSED`. Subsequent puts return `GOC_CLOSED`; subsequent takes drain any buffered values, then return `{NULL, GOC_CLOSED}`. |

```c
// Unbuffered
goc_chan* ch = goc_chan_make(0);

// Buffered (capacity 16)
goc_chan* ch = goc_chan_make(16);

goc_close(ch);
```

---

### Utilities

| Function | Signature | Description |
|---|---|---|
| `goc_in_fiber` | `bool goc_in_fiber(void)` | Returns `true` if the caller is executing inside a fiber, `false` otherwise. Safe to call from any context after `goc_init` returns. |

---

### Fiber launch

| Function | Signature | Description |
|---|---|---|
| `goc_go` | `goc_chan* goc_go(void (*fn)(void*), void* arg)` | Spawn a fiber on the default pool with the default 64 KB stack. Returns a **join channel** that is closed automatically when the fiber returns. Pass the join channel as an arm to `goc_alts` or call `goc_take`/`goc_take_sync` on it to await fiber completion. The join channel may be ignored if no join is needed. |
| `goc_go_on` | `goc_chan* goc_go_on(goc_pool* pool, void (*fn)(void*), void* arg)` | Spawn on a specific pool with the default stack size. Returns a join channel with the same semantics as `goc_go`. |

```c
typedef struct { goc_chan* ch; int n; } args_t;

static void producer(void* arg) {
    args_t* a = arg;
    for (int i = 0; i < a->n; i++)
        goc_put(a->ch, (void*)(intptr_t)i);
    goc_close(a->ch);
}

args_t a = { .ch = goc_chan_make(0), .n = 10 };
goc_chan* done = goc_go(producer, &a);

// Await completion from a plain OS thread:
goc_take_sync(done);
```

---

### minicoro limitations

libgoc uses [minicoro](https://github.com/edubart/minicoro) for fiber switching. Two hard constraints apply to all fiber entry functions:

**C++ exceptions are not supported.** Throwing a C++ exception that unwinds across a `mco_yield` / `mco_resume` boundary is undefined behaviour — the exception mechanism's internal state is not preserved across a coroutine switch. In mixed C/C++ codebases, all fiber entry functions must be declared `extern "C"` and must not allow any C++ exception to propagate out of them.

**Stack overflow aborts the process.** libgoc writes a canary value at the low end of each fiber stack on creation and validates it on every resume. If the canary has been overwritten, the runtime calls `abort()` immediately with a diagnostic message. This turns silent heap corruption into a deterministic, debuggable crash. Stack overflow is still a programming error — avoid large stack-allocated buffers and deep recursion inside fibers. If a fiber is known to need more stack space, restructure the work to use `goc_malloc`-allocated buffers on the GC heap instead.

---

### Channel I/O — callbacks (any context)

Non-blocking. Safe from the libuv loop thread, pool threads, or any other context. The callback is always invoked on the **libuv loop thread** — do not call `goc_take`/`goc_put` from inside it.

| Function | Signature | Description |
|---|---|---|
| `goc_take_cb` | `void goc_take_cb(goc_chan* ch, void (*cb)(void* val, goc_status_t ok, void* ud), void* ud)` | Register a callback to receive the next value. `ok==GOC_CLOSED` means the channel was closed. |
| `goc_put_cb` | `void goc_put_cb(goc_chan* ch, void* val, void (*cb)(goc_status_t ok, void* ud), void* ud)` | Register a callback to deliver `val`. If `ok==GOC_CLOSED`, the value was not consumed. When the value is `goc_malloc`-allocated (the recommended pattern), no explicit cleanup is needed — the GC reclaims it automatically once it becomes unreachable. `cb` may be `NULL` if no completion notification is needed. |

```c
// In a libuv read callback (loop thread) — forward data into a fiber:
static void on_put_done(goc_status_t ok, void* ud) {
    (void)ok; (void)ud;  // GC-allocated; no free needed even if channel closed
}

static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    if (nread <= 0) return;
    my_msg_t* msg = goc_malloc(sizeof(my_msg_t));
    memcpy(msg->data, buf->base, nread);
    msg->len = nread;
    goc_put_cb(data_ch, msg, on_put_done, msg);
}
```

---

### Channel I/O — non-blocking (any context)

| Function | Signature | Description |
|---|---|---|
| `goc_take_try` | `goc_val_t goc_take_try(goc_chan* ch)` | Non-blocking receive. Returns immediately from any context. Returns `{val, GOC_OK}` if a value was available, `{NULL, GOC_CLOSED}` if the channel is closed, or `{NULL, GOC_EMPTY}` if the channel is open but empty. Never suspends. |

---

### Channel I/O — fiber context

These functions may suspend the calling fiber. **Call only from inside a fiber**, not from the libuv loop thread or a plain OS thread.

| Function | Signature | Description |
|---|---|---|
| `goc_take` | `goc_val_t goc_take(goc_chan* ch)` | Receive the next value from `ch`. Blocks until a value is available or the channel is closed. Returns `{val, GOC_OK}` on success, `{NULL, GOC_CLOSED}` on close. Asserts that the caller is running inside a fiber — calling from a bare OS thread aborts with a clear message. |
| `goc_put` | `goc_status_t goc_put(goc_chan* ch, void* val)` | Send `val` into `ch`. Blocks until a receiver accepts or the channel is closed. Returns `GOC_OK` on success, `GOC_CLOSED` on close. Asserts that the caller is running inside a fiber — calling from a bare OS thread aborts with a clear message. |

```c
static void consumer(void* arg) {
    goc_chan* ch = arg;
    goc_val_t v;
    while ((v = goc_take(ch)).ok == GOC_OK)
        printf("got %ld\n", (intptr_t)v.val);
    // channel closed
}
```

---

### Channel I/O — blocking OS threads

Blocks the calling OS thread (not a fiber). Calling these from a fiber is a runtime error and aborts with a diagnostic message. Also never call them from the libuv loop thread (this would deadlock the event loop).

| Function | Signature | Description |
|---|---|---|
| `goc_take_sync` | `goc_val_t goc_take_sync(goc_chan* ch)` | Receive a value, blocking the OS thread. Returns `{val, GOC_OK}` on success, `{NULL, GOC_CLOSED}` on close. |
| `goc_put_sync` | `goc_status_t goc_put_sync(goc_chan* ch, void* val)` | Send `val`, blocking the OS thread. Returns `GOC_OK` on success, `GOC_CLOSED` on close. |

```c
// From a plain OS thread (e.g. the application's main thread):
goc_val_t result = goc_take_sync(result_ch);
if (result.ok == GOC_OK)
    process(result.val);
```

---

### Select (`goc_alts`)

Non-deterministic choice across multiple channel operations.

| Function | Signature | Description |
|---|---|---|
| `goc_alts` | `goc_alts_result goc_alts(goc_alt_op* ops, size_t n)` | **Fiber context only.** Wait until one of the `n` ops fires and return its result. If a `GOC_ALT_DEFAULT` arm is present and no other arm is immediately ready, the default arm fires without suspending the fiber. |
| `goc_alts_sync` | `goc_alts_result goc_alts_sync(goc_alt_op* ops, size_t n)` | **Blocking OS-thread context only.** Same semantics as `goc_alts` but blocks the calling OS thread instead of suspending a fiber. Calling from fiber context is a runtime error and aborts with a diagnostic message. |

```c
typedef enum {
    GOC_ALT_TAKE,    /* receive from ch */
    GOC_ALT_PUT,     /* send put_val into ch */
    GOC_ALT_DEFAULT, /* fires immediately if no other arm is ready; ch must be NULL */
} goc_alt_kind;

typedef struct {
    goc_chan*    ch;        /* channel this arm operates on; NULL for GOC_ALT_DEFAULT */
    goc_alt_kind op_kind;  /* GOC_ALT_TAKE, GOC_ALT_PUT, or GOC_ALT_DEFAULT */
    void*        put_val;  /* value to send; used only when op_kind == GOC_ALT_PUT */
} goc_alt_op;

typedef struct {
    size_t    index;  /* index of the winning arm in ops[] */
    goc_val_t value;  /* received value and ok flag; value.ok==GOC_CLOSED means the channel was closed */
} goc_alts_result;
```

```c
// Blocking select: wait for whichever channel becomes ready first
goc_alt_op ops[] = {
    { .ch = data_ch,   .op_kind = GOC_ALT_TAKE },   // arm 0: take from data_ch
    { .ch = cancel_ch, .op_kind = GOC_ALT_TAKE },   // arm 1: take from cancel_ch
};
goc_alts_result r = goc_alts(ops, 2);

if (r.index == 0 && r.value.ok == GOC_OK)
    process(r.value.val);
else
    return; // cancelled or channel closed
```

```c
// Non-blocking select with a default arm: never suspends the fiber
goc_alt_op ops[] = {
    { .ch = data_ch, .op_kind = GOC_ALT_TAKE },
    { .ch = NULL,    .op_kind = GOC_ALT_DEFAULT },  // arm 1: fires if data_ch is empty
};
goc_alts_result r = goc_alts(ops, 2);

if (r.index == 1)
    /* no value was ready — do something else */;
```

---

### Timeout channel

| Function | Signature | Description |
|---|---|---|
| `goc_timeout` | `goc_chan* goc_timeout(uint64_t ms)` | Return a channel that is **closed** after `ms` milliseconds. Use as an arm in `goc_alts` to implement deadlines. |

```c
goc_alt_op ops[] = {
    { .ch = data_ch,           .op_kind = GOC_ALT_TAKE },
    { .ch = goc_timeout(500),  .op_kind = GOC_ALT_TAKE },
};
goc_alts_result r = goc_alts(ops, 2);

if (r.index == 1)
    printf("timed out after 500 ms\n");
```

---

### Thread pool

The default pool is created by `goc_init` with `max(4, hardware_concurrency)` worker threads. This can be overridden by setting the `GOC_POOL_THREADS` environment variable before calling `goc_init`.

```c
typedef enum {
    GOC_DRAIN_OK      = 0,  /* all fibers finished within the deadline */
    GOC_DRAIN_TIMEOUT = 1,  /* deadline expired; pool remains valid and running */
} goc_drain_result_t;
```

| Function | Signature | Description |
|---|---|---|
| `goc_pool_make` | `goc_pool* goc_pool_make(size_t threads)` | Create a pool with `threads` worker threads. Worker thread registration with Boehm GC is handled automatically via `GC_pthread_create` / `GC_pthread_join` — do not register or unregister threads manually. |
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

### Scheduler loop access

| Function | Signature | Description |
|---|---|---|
| `goc_scheduler` | `uv_loop_t* goc_scheduler(void)` | Return the `uv_loop_t*` owned by the runtime. Safe to call from any thread after `goc_init` returns. **Do not call `uv_run` or `uv_loop_close` on the returned pointer.** |

Use `goc_scheduler` to register user-owned libuv handles on the same event loop as the runtime. All such handles must be freed only inside the `uv_close` completion callback. See [`goc_malloc`](#memory-allocation) for allocation requirements.

```c
static void on_handle_closed(uv_handle_t* h) { free(h); }

uv_tcp_t* server = malloc(sizeof(uv_tcp_t));
uv_tcp_init(goc_scheduler(), server);

// ... later, to tear down:
uv_close((uv_handle_t*)server, on_handle_closed);
```

---

## Building and Testing

libgoc ships with a comprehensive, phased test suite covering the full public API. See the [Testing section in the Design Doc](./DESIGN.md#testing) for a breakdown of the test phases and what each one covers.

### Prerequisites

| Dependency | macOS | Linux (Debian/Ubuntu) | Linux (Fedora/RHEL) | Windows |
|---|---|---|---|---|
| CMake ≥ 3.16 | `brew install cmake` | `apt install cmake` | `dnf install cmake` | [cmake.org](https://cmake.org/download/) |
| libuv | `brew install libuv` | `apt install libuv1-dev` | `dnf install libuv-devel` | vcpkg or source build |
| Boehm GC | `brew install bdw-gc` | source build (see below) | `dnf install gc-devel` | vcpkg or source build |
| pkg-config | `brew install pkg-config` | `apt install pkg-config` | `dnf install pkgconfig` | — |
| minicoro | vendored (submodule); instantiated via `src/minicoro.c` |

A C11 compiler (clang or gcc on POSIX; MSVC or clang-cl on Windows) is required.

**Build configuration constants** (`src/config.h`):

| Constant | Default | Description |
|---|---|---|
| `GOC_PAGE_SIZE` | platform | Memory page size used for stack alignment |
| `GOC_DEFAULT_STACK_SIZE` | `65536` | Per-fiber stack size (64 KB) |
| `GOC_DEAD_COUNT_THRESHOLD` | `8` | Dead fiber reap threshold before GC hint |
| `GOC_ALTS_STACK_THRESHOLD` | `8` | Max `goc_alts` arms before scratch buffer moves to heap |

---

### macOS

> **Boehm GC:** Homebrew's `bdw-gc` formula is built with thread support unconditionally and ships a `bdw-gc-threaded.pc` pkg-config alias. No extra steps are needed — the `brew install` command below is sufficient.

```sh
# 1. Install dependencies (Homebrew)
brew install cmake libuv bdw-gc pkg-config

# 2. Configure
cmake -B build

# 3. Build
cmake --build build

# 4. Run tests
ctest --test-dir build --output-on-failure

# Or run a single phase directly for full output
./build/test_p1_foundation
```

---

### Linux

```sh
# 1. Install dependencies (Debian/Ubuntu shown; see table above for RPM)
sudo apt update
sudo apt install cmake libuv1-dev libatomic-ops-dev pkg-config build-essential

# Ubuntu's libgc-dev is NOT compiled with --enable-threads, which libgoc requires.
# GC_allow_register_threads and the GC_pthread_create / GC_pthread_join wrappers
# are absent in the system package and will cause a crash at runtime.
# Build Boehm GC from source instead:
wget https://github.com/ivmai/bdwgc/releases/download/v8.2.6/gc-8.2.6.tar.gz
tar xf gc-8.2.6.tar.gz && cd gc-8.2.6
./configure --enable-threads=posix --enable-thread-local-alloc --enable-shared --prefix=/usr/local
make -j$(nproc) && sudo make install && sudo ldconfig && cd ..

# The source build does not always generate a bdw-gc-threaded.pc pkg-config alias.
# Create it manually if it is missing:
if [ ! -f /usr/local/lib/pkgconfig/bdw-gc-threaded.pc ]; then
    sudo ln -s /usr/local/lib/pkgconfig/bdw-gc.pc /usr/local/lib/pkgconfig/bdw-gc-threaded.pc
fi

# Ensure pkg-config searches /usr/local (not on the default path on all distros):
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
# To make this permanent:
# echo 'export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH' >> ~/.bashrc

# 2. Configure
cmake -B build

# 3. Build
cmake --build build

# 4. Run tests
ctest --test-dir build --output-on-failure

# Or run a single phase directly
./build/test_p1_foundation
```

---

### Windows

> **Boehm GC:** vcpkg's `bdw-gc` port is built with `GC_THREADS` enabled. The `vcpkg install` command below is sufficient — no manual source build is required.

```bat
REM 1. Install dependencies via vcpkg (or adjust paths as needed)
vcpkg install libuv bdw-gc

REM 2. Configure (pass vcpkg toolchain if using vcpkg)
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake

REM 3. Build
cmake --build build --config RelWithDebInfo

REM 4. Run tests
ctest --test-dir build --output-on-failure -C RelWithDebInfo

REM Or run a single phase directly
build\RelWithDebInfo\test_p1_foundation.exe
```

---

### Build types

```sh
# Debug (no optimisation, debug symbols)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Release
cmake -B build -DCMAKE_BUILD_TYPE=Release

# RelWithDebInfo (default)
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

---

### Shared library

A shared library target (`goc_shared`) is available via `-DLIBGOC_SHARED=ON`. It produces `libgoc.so` (Linux), `libgoc.dylib` (macOS), or `libgoc.dll` (Windows) alongside the default static library.

```sh
cmake -B build -DLIBGOC_SHARED=ON
cmake --build build
sudo cmake --install build   # installs headers, libgoc.a, libgoc.so, and libgoc.pc
```

The install step writes a `libgoc.pc` pkg-config file to `<prefix>/lib/pkgconfig/`. Downstream projects can then locate and link libgoc without knowing its install prefix:

```sh
# Compile and link a consumer with pkg-config
cc $(pkg-config --cflags libgoc) my_app.c $(pkg-config --libs libgoc) -o my_app
```

In a CMake-based consumer, use `pkg_check_modules` in the same way as libgoc itself uses it for libuv:

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBGOC REQUIRED IMPORTED_TARGET libgoc)
target_link_libraries(my_target PRIVATE PkgConfig::LIBGOC)
```

---

### Code coverage

Code coverage instrumentation is opt-in via `-DLIBGOC_COVERAGE=ON`. It requires GCC or Clang and uses `gcov`-compatible `.gcda`/`.gcno` files. If `lcov` and `genhtml` are found, a `coverage` build target is also registered that runs the test suite and produces a self-contained HTML report.

**Install lcov**

| Platform | Command |
|---|---|
| macOS | `brew install lcov` |
| Debian/Ubuntu | `apt install lcov` |
| Fedora/RHEL | `dnf install lcov` |

**Configure and build**

```sh
# Coverage builds should use Debug to avoid optimisation hiding branches
cmake -B build-cov \
      -DCMAKE_BUILD_TYPE=Debug \
      -DLIBGOC_COVERAGE=ON
cmake --build build-cov
```

**Generate the HTML report**

```sh
cmake --build build-cov --target coverage
# Report written to: build-cov/coverage_html/index.html
open build-cov/coverage_html/index.html   # macOS
xdg-open build-cov/coverage_html/index.html  # Linux
```

The `coverage` target runs `ctest` internally, so there is no need to invoke the test binary separately. The final report includes branch coverage and filters out system headers and build-system generated files.

> **Note:** Coverage and sanitizer builds are mutually exclusive — configure them in separate build directories. Coverage is also incompatible with `-DCMAKE_BUILD_TYPE=Release` optimisation levels that inline or eliminate branches.

---

### Sanitizers

AddressSanitizer and ThreadSanitizer builds are available as opt-in targets.

```sh
# AddressSanitizer
cmake -B build-asan -DLIBGOC_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure

# ThreadSanitizer
cmake -B build-tsan -DLIBGOC_TSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

> **Note:** ASAN and TSAN are mutually exclusive — configure them in separate build directories.

---

*(C) Divyansh Prakash, 2026*
