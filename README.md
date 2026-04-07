[![CI](https://github.com/divs1210/libgoc/actions/workflows/ci.yml/badge.svg)](https://github.com/divs1210/libgoc/actions/workflows/ci.yml)
[![CD](https://github.com/divs1210/libgoc/actions/workflows/cd.yml/badge.svg)](https://github.com/divs1210/libgoc/actions/workflows/cd.yml)

![libgoc](assets/logo.png)

# libgoc
> A Go-style CSP concurrency runtime for C: threadpools, stackful coroutines, channels, select, async I/O, and garbage collection in one coherent API.

**libgoc** is a runtime library for C programs that want Go-style CSP concurrency backed by a managed memory environment. It is written in plain C for maximum reach and portability.

The library provides stackful coroutines ("fibers"), channels, a select primitive (`goc_alts`), timeout channels, a managed thread pool, and optional runtime telemetry (`goc_stats`). Boehm GC is a required dependency and is linked automatically.

**libgoc is built for:**

- **C developers** who want a Go-style concurrency runtime without switching to Go.
- **Language implementors** targeting C/C++ as a compilation target, or writing multithreaded interpreters.

**Dependencies:**

| | |
|---|---|
| `minicoro` | fiber suspend/resume (cross-platform; POSIX and Windows) |
| `libuv` | event loop, threads, timers, cross-thread wakeup |
| Boehm GC | garbage collection |
| `picohttpparser` | HTTP/1.1 request parser (vendored MIT); used by `goc_http`; compiled in by default; disable with `-DLIBGOC_SERVER=OFF` |

**Pre-built static libraries** are available on the [Releases page](https://github.com/divs1210/libgoc/releases):
- Linux (x86-64)
- macOS (arm64)
- Windows (x86-64)

**Helper libraries:**
- [Async I/O](./docs/IO.md)
- [Async HTTP Client/Server](./docs/HTTP.md)
- [Dynamic Array](./docs/ARRAY.md)
- [Telemetry](./docs/TELEMETRY.md)

**Also see:**
- [Design Doc](./docs/DESIGN.md)
- [Benchmarks](/bench/README.md)

---

## Table of Contents

- [Examples](#examples)
  - [1. Ping-pong](#1-ping-pong)
  - [2. Custom thread pool with `goc_go_on`](#2-custom-thread-pool-with-goc_go_on)
  - [3. Using goc_malloc](#3-using-goc_malloc)
- [Public API](#public-api)
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
  - [Async I/O →](./docs/IO.md)
  - [HTTP Client/Server →](./docs/HTTP.md)
- [Best Practices](#best-practices)
- [Benchmarks](#benchmarks)
- [Building and Testing](#building-and-testing)
  - [Prerequisites](#prerequisites)
  - [macOS](#macos)
  - [Linux](#linux)
  - [Windows](#windows)
  - [Build types](#build-types)
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

static void player_fiber(void* arg) {
    player_args_t* a = arg;

    goc_val_t* v;
    while ((v = goc_take(a->recv))->ok == GOC_OK) {
        int count = goc_unbox_int(v->val);
        printf("%s %d\n", a->name, count);
        if (count >= N_ROUNDS) {
            goc_close(a->send);
            return;
        }
        goc_put(a->send, goc_box_int(count + 1));
    }
}

static void main_fiber(void* _) {
    goc_chan* a_to_b = goc_chan_make(0);
    goc_chan* b_to_a = goc_chan_make(0);

    player_args_t ping_args = { .recv = b_to_a, .send = a_to_b, .name = "ping" };
    player_args_t pong_args = { .recv = a_to_b, .send = b_to_a, .name = "pong" };

    goc_chan* done_ping = goc_go(player_fiber, &ping_args);
    goc_chan* done_pong = goc_go(player_fiber, &pong_args);

    /* Kick off the exchange with the first message. */
    goc_put(a_to_b, goc_box_int(1));

    /* Wait for both fibers to finish. */
    goc_take(done_ping);
    goc_take(done_pong);
}

int main(void) {
    goc_init();
    goc_go(main_fiber, NULL);
    goc_shutdown();
    return 0;
}
```

**What this example demonstrates:**

- `goc_chan_make(0)` — unbuffered channels enforce a synchronous rendezvous:
  each `goc_put` blocks until the other fiber calls `goc_take`, and vice versa.
- `goc_go` — spawns both player fibers on the current pool (or default pool
    when called outside fiber context) and returns a join
  channel that is closed automatically when the fiber returns.
- `goc_close` — when the round limit is reached the active fiber closes the
  forward channel, causing the partner's next `goc_take` to return
  `GOC_CLOSED` and exit its loop cleanly.

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
    goc_put(a->result_ch, goc_box_int(acc));
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
        goc_val_t* v = goc_take_sync(result_ch);
        if (v->ok == GOC_OK) total += (long)goc_unbox_int(v->val);
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

static node_t* build_list(int n) {
    node_t* head = NULL;
    for (int i = n - 1; i >= 0; i--) {
        node_t* node = goc_malloc(sizeof(node_t));
        node->value = i;
        node->next  = head;
        head        = node;
    }
    return head;
}

int main(void) {
    goc_init();

    node_t* node = build_list(10);
    while (node != NULL) {
        printf("%d ", node->value);
        node = node->next;
    }
    printf("\n");

    goc_shutdown();
    return 0;
}
```

**A few things to keep in mind:**

- `goc_malloc` is a thin wrapper around `GC_malloc`. Memory is zero-initialised.

---

## Public API

### Initialisation and shutdown

| Function | Signature | Description |
|---|---|---|
| `goc_init` | `void goc_init(void)` | Initialise the runtime. Must be called exactly once as the first call in `main()`, from the process main thread. |
| `goc_shutdown` | `void goc_shutdown(void)` | Shut down the runtime. Blocks until all in-flight fibers finish, then tears down all pools, channels, and the event loop. Must be called from the process main thread. |

---

### Memory allocation

| Function | Signature | Description |
|---|---|---|
| `goc_malloc` | `void* goc_malloc(size_t n)` | Allocate `n` bytes on the GC-managed heap. No `free` required — the collector reclaims unreachable memory automatically. Backed by `GC_malloc` internally. Aborts on allocation failure and never returns `NULL`. |

```c
my_obj_t* obj = goc_malloc(sizeof(my_obj_t));
// obj is automatically collected when no longer reachable
```

---

### String helpers

libgoc provides a GC-managed string formatting helper. The result is allocated on the Boehm GC heap; the caller must not `free` it.

| Function | Signature | Description |
|---|---|---|
| `goc_sprintf` | `char* goc_sprintf(const char* fmt, ...)` | Formats `fmt` and the given arguments into a null-terminated GC-heap string. Never returns `NULL`. Aborts on allocation failure. |

```c
// Format a message and send it on a channel
char* msg = goc_sprintf("fiber %d ready", id);
goc_put(ch, msg);
```

---

### Scalar boxing helpers

libgoc channels and arrays carry `void*` values. These macros eliminate the repetitive double-cast needed to pass integers through a `void*` slot and to recover them.

| Macro | Expands to | Description |
|---|---|---|
| `goc_box_int(x)` | `(void*)(intptr_t)(x)` | Encode a signed integer as `void*` |
| `goc_unbox_int(p)` | `(intptr_t)(p)` | Decode a `void*` back to `intptr_t` |
| `goc_box_uint(x)` | `(void*)(uintptr_t)(x)` | Encode an unsigned integer as `void*` |
| `goc_unbox_uint(p)` | `(uintptr_t)(p)` | Decode a `void*` back to `uintptr_t` |

```c
// Channel
goc_put(ch, goc_box_int(42));
intptr_t n = goc_unbox_int(goc_take(ch)->val);

// Array
goc_array_push(arr, goc_box_int(42));
intptr_t n = goc_unbox_int(goc_array_get(arr, 0));
```

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

### Utilities

| Function | Signature | Description |
|---|---|---|
| `goc_in_fiber` | `bool goc_in_fiber(void)` | Returns `true` if the caller is executing inside a fiber, `false` otherwise. Safe to call from any context after `goc_init` returns. |
| `goc_current_pool` | `goc_pool* goc_current_pool(void)` | Return the current fiber's pool, or `NULL` when called outside fiber context. |
| `goc_current_or_default_pool` | `goc_pool* goc_current_or_default_pool(void)` | Return current pool when in fiber context, otherwise return the default pool. |
| `goc_current_thread` | `uv_thread_t goc_current_thread(void)` | Return the current OS thread id (libuv thread type). |
| `goc_current_fiber` | `void* goc_current_fiber(void)` | Return the current runtime fiber handle, or `NULL` outside fiber context. |

---

### Fiber launch

| Function | Signature | Description |
|---|---|---|
| `goc_go` | `goc_chan* goc_go(void (*fn)(void*), void* arg)` | Spawn a fiber on the current pool when called from a fiber; otherwise on the default pool. Returns a **join channel** that is closed automatically when the fiber returns. Pass the join channel as an arm to `goc_alts` or call `goc_take`/`goc_take_sync` on it to await fiber completion. The join channel may be ignored if no join is needed. |
| `goc_go_on` | `goc_chan* goc_go_on(goc_pool* pool, void (*fn)(void*), void* arg)` | Spawn on a specific pool. Returns a join channel with the same semantics as `goc_go`. |

```c
typedef struct { goc_chan* ch; int n; } args_t;

static void producer(void* arg) {
    args_t* a = arg;
    for (int i = 0; i < a->n; i++)
        goc_put(a->ch, goc_box_int(i));
    goc_close(a->ch);
}

args_t a = { .ch = goc_chan_make(0), .n = 10 };
goc_chan* done = goc_go(producer, &a);

// Await completion from a plain OS thread:
goc_take_sync(done);
```

---

### minicoro limitations

libgoc uses [minicoro](https://github.com/edubart/minicoro) for fiber switching. Three constraints apply:

- **C++ exceptions** must not propagate across fiber boundaries (across `mco_yield`/`mco_resume`). Declare fiber entry functions `extern "C"` in mixed C/C++ codebases.
- **Fixed stacks** — default canary-protected stacks are portable and overflow-safe (abort on overwrite). Use `goc_malloc` for large data instead of stack allocation. Enable dynamic stacks with `-DLIBGOC_VMEM=ON` if needed.
- **`src/minicoro.c`** is compiled with isolated flags that prevent GC/TLS conflicts and enable conservative-GC-friendly memory zeroing.

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

// on_read runs on the libuv loop thread — goc_put_cb is safe to call here.
// The callback (on_put_done) will also be invoked on the loop thread.
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
| `goc_take_try` | `goc_val_t* goc_take_try(goc_chan* ch)` | Non-blocking receive. Returns immediately from any context. Returns a GC-managed pointer to `{val, GOC_OK}` if a value was available, `{NULL, GOC_CLOSED}` if the channel is closed, or `{NULL, GOC_EMPTY}` if the channel is open but empty. Never suspends. |

---

### Channel I/O — fiber context

These functions may suspend the calling fiber. **Call only from inside a fiber**, not from the libuv loop thread or a plain OS thread.

| Function | Signature | Description |
|---|---|---|
| `goc_take` | `goc_val_t* goc_take(goc_chan* ch)` | Receive the next value from `ch`. Blocks until a value is available or the channel is closed. Returns a GC-managed pointer to `{val, GOC_OK}` on success, `{NULL, GOC_CLOSED}` on close. Asserts that the caller is running inside a fiber — calling from a bare OS thread aborts with a clear message. |
| `goc_put` | `goc_status_t goc_put(goc_chan* ch, void* val)` | Send `val` into `ch`. Blocks until a receiver accepts or the channel is closed. Returns `GOC_OK` on success, `GOC_CLOSED` on close. Asserts that the caller is running inside a fiber — calling from a bare OS thread aborts with a clear message. |
| `goc_take_all` | `goc_val_t** goc_take_all(goc_chan** chs, size_t n)` | Receive from each channel in `chs[0..n-1]` in order, then wait for all results. Returns a GC-managed array of `n` `goc_val_t*` results in the same order as `chs[]`. Each element follows `goc_take` semantics: `{val, GOC_OK}` on success, `{NULL, GOC_CLOSED}` on close. Must only be called from a fiber. |

---

### Channel I/O — blocking OS threads

Blocks the calling OS thread (not a fiber). Calling these from a fiber is a runtime error and aborts with a diagnostic message. Also never call them from the libuv loop thread (this would deadlock the event loop).

| Function | Signature | Description |
|---|---|---|
| `goc_take_sync` | `goc_val_t* goc_take_sync(goc_chan* ch)` | Receive a value, blocking the OS thread. Returns a GC-managed pointer to `{val, GOC_OK}` on success, `{NULL, GOC_CLOSED}` on close. |
| `goc_put_sync` | `goc_status_t goc_put_sync(goc_chan* ch, void* val)` | Send `val`, blocking the OS thread. Returns `GOC_OK` on success, `GOC_CLOSED` on close. |
| `goc_take_all_sync` | `goc_val_t** goc_take_all_sync(goc_chan** chs, size_t n)` | Receive from each channel in `chs[0..n-1]` in order, then block the OS thread until all results are ready. Returns a GC-managed array of `n` `goc_val_t*` results in the same order as `chs[]`. Each element follows `goc_take_sync` semantics: `{val, GOC_OK}` on success, `{NULL, GOC_CLOSED}` on close. Must not be called from a fiber. |

---

### Select (`goc_alts`)

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

### Timeout channel

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

### RW mutexes

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

### Thread pool

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

### Scheduler loop access

| Function | Signature | Description |
|---|---|---|
| `goc_scheduler` | `uv_loop_t* goc_scheduler(void)` | Return the `uv_loop_t*` owned by the runtime. Safe to call from any thread after `goc_init`. **Do not call `uv_run` or `uv_loop_close` on it.** |

---

### Async I/O

See [IO.md](./docs/IO.md) for the full `goc_io` API reference.

---

### HTTP Server

libgoc ships an HTTP/1.1 server and client built on `goc_io` TCP and the vendored [picohttpparser](https://github.com/h2o/picohttpparser) (MIT). It integrates with the fiber scheduler and has no additional runtime dependencies beyond libuv.

See [HTTP.md](./docs/HTTP.md) for the full `goc_http` API reference.

---

## Best Practices

Used the right way, **libgoc** provides a runtime environment very similar to Go's.

**The blocking versions of take/put/alts are intended only for the initial setup in the `main` function, and should not be used otherwise.**

A typical program's main function should be like this:

```c
static void main_fiber(void* _) {
    // User code comes here.
    // Since this is a fiber context,
    // async channel ops work here
    // and in all code reachable from here
}

int main(void) {
    goc_init();

    // reify main thread as main fiber
    goc_go(main_fiber, NULL);

    goc_shutdown();
    return 0;
}
```

---

## Benchmarks

Benchmark suites for Go and libgoc live under [`bench/`](./bench/README.md). They are separate from the library build and CI, and are intended for performance exploration only.

---

## Building and Testing

**Pre-built static libraries** are available on the [Releases page](https://github.com/divs1210/libgoc/releases).

libgoc ships with a comprehensive, phased test suite covering the full public API. See the [Testing section in the Design Doc](./docs/DESIGN.md#testing) for a breakdown of the test phases and what each one covers.

**`test.sh`** — Full build + test runner with optional watch mode:

```sh
./test.sh              # build and run all tests
WATCH=1 ./test.sh      # rebuild and rerun on any src/include/tests change
./test.sh -dbg 1       # enable verbose [GOC_DBG] output
```

Options: `-dbg <0|1>`, `-rp <0|1>` (SO_REUSEPORT for HTTP tests), `-vmem <0|1>`. Output is streamed to console and `test.log`. In watch mode, only previously-failing tests are rerun on the next change.

**`run_test_loop.sh`** — Stress a single test for flakiness detection:

```sh
./run_test_loop.sh tests/test_p06_thread_pool.c           # run up to 20 times
./run_test_loop.sh tests/test_p06_thread_pool.c -max-tries 100 -trace 1
```

Builds only the named target, runs it in a loop, and exits on the first failure. Each run is timestamped; log path is printed on exit.

### Prerequisites

| Dependency | macOS | Linux (Debian/Ubuntu) | Linux (Fedora/RHEL) | Windows |
|---|---|---|---|---|
| CMake ≥ 3.20 | `brew install cmake` | `apt install cmake` | `dnf install cmake` | MSYS2 UCRT64 (bundled) |
| libuv | `brew install libuv` | `apt install libuv1-dev` | `dnf install libuv-devel` | MSYS2 UCRT64 — see [Windows](#windows) |
| Boehm GC | `brew install bdw-gc` | source build (see below) | `dnf install gc-devel` | MSYS2 UCRT64 — see [Windows](#windows) |
| pkg-config | `brew install pkg-config` | `apt install pkg-config` | `dnf install pkgconfig` | MSYS2 UCRT64 (bundled) |
| minicoro | vendored (`vendor/minicoro/`); instantiated via `src/minicoro.c` |

A C11 compiler is required: GCC or Clang on Linux/macOS; MinGW-w64 GCC via MSYS2 UCRT64 on Windows.

libgoc is built to link statically against `libuv` and Boehm GC. Ensure static versions of those dependencies are available to `pkg-config` before configuring.

---

### macOS

```sh
# 1. Install dependencies (Homebrew)
brew install cmake libuv bdw-gc pkg-config

# Homebrew's bdw-gc does not ship a bdw-gc-threaded.pc pkg-config alias.
# Create it once in the global Homebrew pkgconfig directory:
PKGDIR="$(brew --prefix)/lib/pkgconfig"
[ -f "$PKGDIR/bdw-gc-threaded.pc" ] || cp "$PKGDIR/bdw-gc.pc" "$PKGDIR/bdw-gc-threaded.pc"

# 2. Configure
export PKG_CONFIG_ALL_STATIC=1
cmake -B build -DLIBGOC_STATIC_DEPENDENCIES=ON

# 3. Build
cmake --build build

# 4. Run tests
ctest --test-dir build --output-on-failure

# Or run a single phase directly for full output
./build/test_p01_foundation
```

---

### Linux

```sh
# 1. Install dependencies (Debian/Ubuntu shown; see table above for RPM)
sudo apt update
sudo apt install cmake libuv1-dev libatomic-ops-dev pkg-config build-essential

# Ubuntu's libgc-dev is NOT compiled with --enable-threads, which libgoc requires.
# GC_allow_register_threads is required for libgoc's goc_thread_create/
# goc_thread_join wrappers; the system package can crash at runtime.
# Build Boehm GC from source instead:
wget https://github.com/ivmai/bdwgc/releases/download/v8.2.6/gc-8.2.6.tar.gz
tar xf gc-8.2.6.tar.gz && cd gc-8.2.6
./configure --enable-threads=posix --enable-thread-local-alloc --disable-shared --enable-static --prefix=/usr/local
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
export PKG_CONFIG_ALL_STATIC=1
cmake -B build -DLIBGOC_STATIC_DEPENDENCIES=ON

# 3. Build
cmake --build build

# 4. Run tests
ctest --test-dir build --output-on-failure

# Or run a single phase directly
./build/test_p01_foundation
```

---

### Windows

libgoc uses libuv thread primitives (`uv_thread_t`, etc.) and C11 atomics via `<stdatomic.h>` (`_Atomic`, `atomic_*`). MSVC builds are still not supported (notably due to bdwgc/toolchain constraints, including vcpkg's Win32-threads build), so the recommended Windows setup remains **MSYS2/MinGW-w64 (UCRT64)**.

```sh
# 1. Install MSYS2 from https://www.msys2.org/, then in a UCRT64 shell:
pacman -S mingw-w64-ucrt-x86_64-gcc \
          mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-libuv \
          mingw-w64-ucrt-x86_64-gc \
          mingw-w64-ucrt-x86_64-pkg-config

# 2. Create the bdw-gc-threaded pkg-config alias if it is missing
PKGDIR="/ucrt64/lib/pkgconfig"
[ -f "$PKGDIR/bdw-gc-threaded.pc" ] || cp "$PKGDIR/bdw-gc.pc" "$PKGDIR/bdw-gc-threaded.pc"

# 3. Configure and build everything (library + tests)
export PKG_CONFIG_ALL_STATIC=1
cmake -B build -DLIBGOC_STATIC_DEPENDENCIES=ON
cmake --build build --parallel $(nproc)

# 4. Run tests
ctest --test-dir build --output-on-failure
```

> **Tests:** Phases P1–P7 and P9 run normally on Windows. Phase 8 (safety tests) requires `fork()`/`waitpid()` to isolate processes that call `abort()` — these POSIX APIs are not available in MinGW. The P8 test binary builds successfully but all 11 tests report `skip` at runtime.

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

### Stack allocator

```sh
# Default: canary-protected stacks (recommended, portable)
cmake -B build

# Enable virtual memory allocator (dynamic stack growth)
cmake -B build -DLIBGOC_VMEM=ON
```

The default fiber stack size can be set at build time:

```sh
cmake -B build -DLIBGOC_STACK_SIZE=131072   # 128 KB
```

---

### Installation and pkg-config

`libgoc` is installed as a static archive plus headers. The install step writes a `libgoc.pc` pkg-config file to `<prefix>/lib/pkgconfig/`, so downstream projects can locate and link `libgoc` without knowing its install prefix.

```sh
cmake -B build
cmake --build build
sudo cmake --install build   # installs goc.h, goc_io.h, goc_array.h, libgoc.a, and libgoc.pc
```

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
