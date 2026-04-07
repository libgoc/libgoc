# HTTP Throughput Scaling — Optimization Plan

## Problem

HTTP server throughput does not scale with pool size. It degrades slightly as
more workers are added:

| Pool | libgoc (req/s) | Go (req/s) | ratio |
|------|---------------|------------|-------|
| 1    | 8,770         | 9,941      | 0.94× |
| 2    | 7,999         | 20,062     | 0.40× |
| 4    | 7,461         | 43,812     | 0.17× |
| 8    | 7,022         | 78,136     | 0.09× |

All other benchmarks that are purely fiber-scheduler-bound scale normally or
are flat by design. The degradation is specific to I/O-bound workloads that
go through stream/TCP handles.

## Root Cause: Single-Loop I/O Bottleneck

Every stream and TCP handle operation (`uv_read_start`, `uv_write`,
`uv_read_stop`, `uv_shutdown`, handle init/close, UDP send/recv, etc.) is
dispatched to the **global loop thread** via `post_on_loop()`:

```
fiber (worker N)
  → post_on_loop(fn, arg)           [malloc dispatch struct, enqueue to g_cb_queue]
  → loop thread: fn(arg)            [uv_write / uv_read_start / ...]
  → libuv callback fires on g_loop  [on_write_cb / on_read_cb]
  → wake(ch, entry, val)            [post_to_run_queue → uv_async_send(worker.wakeup)]
  → fiber resumes on worker N
```

Each I/O operation on a TCP connection requires **two cross-thread hops**:
1. Worker → loop thread (dispatch)
2. Loop thread → worker (callback wakeup)

With a single loop thread, throughput is capped by how fast the loop thread can
process `uv_write` + callback pairs. Adding more workers increases concurrency,
which increases I/O dispatch rate, which saturates the single loop thread. The
stats confirm this: `cb-queue hwm` reaches 58–79 at pool=8 with high miss and
idle-wakeup counts indicating contention.

Go uses per-goroutine netpoller integration (via the kernel's epoll/kqueue
directly in the Go runtime), which naturally parallelises I/O across OS threads.

## Solution: Per-Worker I/O Loops for Handle-Affine Operations

Each worker already has a `uv_loop_t loop` (from the `better-throughput`
branch). The goal is to make TCP connections owned by a worker have their
handles initialized on that worker's loop, and to have handle-affine I/O ops
execute directly on the owning worker thread — eliminating the cross-thread
dispatch entirely for the common case.

---

## Implementation Plan

### Phase 1 — Service the worker loop after every fiber resume

**What:** After each `mco_resume` (and GC safe point), call
`uv_run(&worker.loop, UV_RUN_NOWAIT)` to drain any pending I/O callbacks
queued on the worker's own loop. Currently, `uv_run` is only called when the
worker is idle (`UV_RUN_ONCE` blocks). Adding a `UV_RUN_NOWAIT` pass after
every resume ensures that I/O callbacks on worker-owned handles fire promptly
without waiting for the worker to go idle.

**Where:** `pool.c`, `pool_worker_fn`, after `GC_collect_a_little()`.

**Pseudocode change:**
```
    GC_set_stackbottom(NULL, &orig_sb)
    GC_collect_a_little()
+   uv_run(&worker.loop, UV_RUN_NOWAIT)   /* drain worker-owned I/O callbacks */

    fe = mco_get_user_data(coro)
    ...
```

**Risk:** `UV_RUN_NOWAIT` returns immediately if there is nothing pending; the
cost is one syscall (`epoll_wait` with timeout=0) per resume when the worker
has no I/O. Benchmark canary to confirm overhead is acceptable on CPU-bound
workloads (ping-pong, ring).

---

### Phase 2 — Route handle init to the calling worker's loop

**What:** When `goc_io_tcp_init`, `goc_io_pipe_init`, `goc_io_udp_init`, etc.
are called from a pool-worker fiber, initialize the libuv handle on that
worker's own `uv_loop_t` instead of dispatching to `g_loop`. The handle is
then owned by the worker's loop and its callbacks fire on that worker's thread.

**Mechanism:**
- `goc_io_handle_init_dispatch_t` currently hard-codes `g_loop` in the
  `on_handle_init_dispatch` callback (`goc_io.c:1428`).
- Add a `uv_loop_t* target_loop` field to `goc_handle_init_dispatch_t`.
- In `goc_io_tcp_init` (and friends), set `target_loop =
  goc_worker_or_default_loop()`.
- If `target_loop != g_loop`: call `uv_tcp_init(target_loop, handle)` directly
  from the fiber (no `post_on_loop` needed — the handle is being init'd on the
  calling worker's loop, which is safe to call from the worker thread).
- If `target_loop == g_loop` (called from outside a worker): dispatch via
  `post_on_loop` as today.

**Handle data tag:** Add a `uv_loop_t* home_loop` pointer to each handle's
associated context struct (or store it in `handle->data`). This is needed so
that subsequent handle-affine operations can detect whether they are on the
handle's home loop thread.

**Scope:** `goc_io_tcp_init`, `goc_io_pipe_init`, `goc_io_udp_init`,
`goc_io_tty_init`, `goc_io_signal_init`.  
`goc_io_fs_event_init` and `goc_io_fs_poll_init` can continue using `g_loop`
for now — they are not on the HTTP critical path.

---

### Phase 3 — Eliminate post_on_loop for same-worker handle ops

**What:** For `goc_io_read_start`, `goc_io_read_stop`, `goc_io_write`,
`goc_io_shutdown`, and `goc_io_write2`: if the calling fiber is running on the
same worker that owns the handle (i.e., `tl_worker != NULL &&
&tl_worker->loop == handle->loop`), invoke the libuv operation directly
instead of going through `post_on_loop`.

**Why it is safe:** The worker thread is the loop thread for its own
`uv_loop_t`. Calling `uv_read_start`, `uv_write`, etc. from the worker thread
that owns the loop is thread-safe per libuv's threading model. The subsequent
`uv_run(UV_RUN_NOWAIT)` pass (Phase 1) will drain the resulting callbacks.

**Dispatch path after Phase 3 (happy path):**
```
fiber (worker N, handle owned by worker N)
  → uv_write(...)                    [direct call — no malloc, no queue]
  → uv_run(UV_RUN_NOWAIT) after resume
  → on_write_cb fires on worker N
  → wake(ch, entry, val)
  → fiber resumes on worker N
```
This is one thread, zero cross-thread hops.

**Cross-worker / external path (unchanged):** If the calling worker is not the
handle's owner (or the caller is not a worker thread at all), dispatch to the
home loop. For handles on `g_loop`, continue using `post_on_loop`. For handles
on a worker loop, introduce `post_on_worker_loop(worker_idx, fn, arg)`:
- Enqueue the task on a per-worker MPSC task queue (separate from the fiber
  run queue).
- Call `uv_async_send(worker->wakeup)` to wake the worker's loop.
- When the worker's loop runs (either `UV_RUN_NOWAIT` or `UV_RUN_ONCE`), drain
  the task queue before returning to the fiber scheduler.

**New data structure:**
```c
typedef struct goc_worker_task {
    void (*fn)(void*);
    void* arg;
    struct goc_worker_task* next;   /* singly-linked; pushed/popped under lock */
} goc_worker_task_t;

/* per-worker */
goc_worker_task_t* _Atomic task_queue_head;  /* MPSC push-end (lockless CAS) */
goc_worker_task_t* task_queue_tail;          /* pop-end (owner only) */
uv_mutex_t         task_queue_lock;
```

Drain the task queue in a new `drain_worker_tasks(worker)` helper called from
the `uv_async_t` wakeup callback on the worker's loop. This mirrors the role
of `drain_cb_queue` on `g_loop`.

---

### Phase 4 — Accept-loop connection distribution

**What:** The accept-loop fiber (`goc_http.c:accept_loop_fiber`) currently runs
on whichever pool worker it lands on. All accepted connections are init'd on
the global loop (Phase 2 above changes this to the accept fiber's worker loop,
but all connections still land on one worker). For throughput to scale,
connections must be distributed across all workers.

**Mechanism — SO_REUSEPORT (preferred on Linux ≥ 3.9):**
- Create one `uv_tcp_t` listener per worker, all bound to the same
  `host:port` with `SO_REUSEPORT`.
- Each listener is initialized on its worker's loop (via Phase 2's direct-init
  path).
- The kernel load-balances `accept()` calls across the N listeners — no
  application-level round-robin needed.
- Each worker runs its own `accept_loop_fiber` and spawns connection fibers
  that inherit the worker's loop.
- `goc_http_server_listen` spawns N accept fibers (one per worker in the pool)
  instead of one.

**Fallback — application-level round-robin:**
If `SO_REUSEPORT` is not available (macOS, older kernels):
- Accept on `g_loop` as today (single `uv_tcp_t` listener).
- After `uv_accept`, transfer the connection handle to a target worker using
  `post_on_worker_loop` (Phase 3).
- Target worker calls `uv_tcp_init` on its own loop and `uv_accept` is replaced
  by `uv_tcp_open(fd)` after extracting the fd via `uv_fileno`.

Detect `SO_REUSEPORT` support at compile time (`#ifdef UV_TCP_REUSEPORT` or
`#ifdef SO_REUSEPORT`) and fall back automatically.

---

### Phase 5 — Handle close on owner loop

**What:** `goc_io_handle_close` currently dispatches to `g_loop` via
`post_on_loop`. After Phases 2–4, handles may be owned by worker loops. Close
must be dispatched to the handle's home loop.

**Mechanism:**
- Read `handle->loop` to determine the home loop.
- If `handle->loop == g_loop`: dispatch via `post_on_loop` as today.
- If `handle->loop == &workers[i].loop`: dispatch via `post_on_worker_loop(i,
  on_handle_close_dispatch, d)`.
- If the calling thread *is* the owning worker: call directly.

---

## Expected Impact

After Phases 1–4, the per-connection I/O path for a worker-owned connection
becomes:

- `uv_read_start` / `uv_write` — direct call from fiber, 0 extra threads
- Read/write callbacks — fire on the owning worker thread, wake fiber via
  `post_to_run_queue` (same-worker fast path: direct deque push)
- Accept — distributed by kernel across N workers (SO_REUSEPORT)

This mirrors Go's per-M netpoller model. Throughput should scale near-linearly
with pool size for connection-bound workloads.

Estimated throughput target (pool=8, 8-core machine): ≥ 60,000 req/s
(currently 7,022 req/s; Go achieves 78,136 req/s).

---

## Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| `UV_RUN_NOWAIT` adds overhead on CPU-bound benchmarks (ping-pong, ring) | Run canary benchmarks after Phase 1; gate on no regression > 5% |
| Handle stolen by work-stealing and run on a different worker | IO ops on cross-worker handles use `post_on_worker_loop` dispatch path; `tl_worker` always reflects current OS thread |
| SO_REUSEPORT not portable | Compile-time detection + application-level round-robin fallback |
| Worker shutdown while handles are still open | `goc_pool_destroy` drains fibers first; handle close happens before worker loop teardown; add assertion |
| `post_on_worker_loop` task queue contention | Only used for cross-worker ops (rare in the steady state); worker's own direct-call path dominates |
| Interaction with `goc_pool_destroy` / worker teardown | `uv_loop_close` must be called after all handles on the worker loop are closed; drain worker task queue in shutdown path |

---

## Phasing and Commits

Each phase can be implemented and benchmarked independently:

1. **Phase 1** (`UV_RUN_NOWAIT` after resume) — one-line change in `pool.c`,
   immediate benchmark to verify no regression.
2. **Phase 2** (worker-loop handle init) — `goc_io.c` + `internal.h`; no
   behavior change for external callers.
3. **Phase 3** (inline IO for same-worker handles + `post_on_worker_loop`) —
   largest change; requires new per-worker task queue.
4. **Phase 4** (SO_REUSEPORT accept distribution) — `goc_http.c` +
   `goc_io.c`; separate benchmarkable step.
5. **Phase 5** (handle close routing) — small follow-on after Phase 3.
