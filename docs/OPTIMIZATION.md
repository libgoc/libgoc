# libgoc Optimization Plan

## Architecture

libgoc uses a **work-stealing fiber scheduler** backed by **libuv** for I/O.
Each pool worker runs its own native OS thread. Fibers are cooperatively
scheduled within each worker; blocked fibers are suspended and the worker
picks the next runnable task off its local deque (or steals from a neighbor).

## What Has Been Done

### Phase 0 — Instrumentation
Added `goc_stats` subsystem: per-pool counters for steal attempts/successes,
idle wakeups, timeout allocations, and callback-queue high-water mark. Bench
output now appends a `[stats]` line after every benchmark. Enables data-driven
optimization rather than guesswork.

### Phase 1 — Work Stealing
Enabled active work stealing (fan-out 2–4.5× at p2). Each idle worker spins
over neighbors' deques before sleeping. Result: fan-out/fan-in improved from
~0.57× to ~0.92× geo-mean vs Go.

### Phase 2 — Multi-Loop Architecture  
Replaced the single global `uv_loop_t` with **per-worker loops**:
- Each worker owns a `uv_loop_t` + `uv_async_t` wakeup handle.
- Idle workers sleep in `uv_run(worker_loop, UV_RUN_ONCE)` instead of a semaphore.
- All I/O submissions (fs, dns, random) route through `goc_worker_or_default_loop()`,
  which returns the calling worker's loop or falls back to `g_loop`.
- Stream/TCP/UDP handles remain handle-affine to existing loops.
- Wake paths (enqueue, steal, shutdown) all call `uv_async_send(worker->wakeup)`.

Also in this phase:
- **Incremental GC**: disabled stop-the-world collection; `collect_a_little`
  called on each fiber completion to amortize GC cost.
- **Timeout batching**: coalesce pending timeouts into fewer libuv timer operations.
- **Callback-queue coalescing**: merge pending callbacks before dispatching to
  reduce lock round-trips.
- **HTTP improvements**: REUSEPORT listener (Linux), userspace load balancing
  elsewhere, connection pooling, keep-alive fixes.
- **Full handle migration**: UDP, TCP, and pipe handles are now also routed to
  per-worker loops via `goc_worker_or_default_loop()` (commit `b605efe`);
  `g_loop` is now the fallback for handles that must remain loop-global
  (fs_event, fs_poll, spawn).

### Phase 3 — Unified Scalar Handling & Array Helpers
Introduced a cleaner public API for array construction and scalar boxing:
- **`goc_array_of(...)`** / **`goc_array_of_boxed(T, ...)`**: inline construction
  macros where element count is derived from a compound literal — no explicit
  size argument required.
- **`goc_box(T, val)`** / **`goc_unbox(T, p)`**: unified two-argument macro API
  replacing old per-type helpers (e.g. `goc_box_int`). Currently still
  heap-allocate via `_goc_box_impl`; `_Generic` dispatch for allocation-free
  scalars is the remaining work (see Future Optimizations).

---

## Current Benchmark State

*Best-of-3 runs, libgoc canary vs Go baseline. Full tables in [bench/README.md](bench/README.md).*

| Benchmark              | p1         | p2         | p4         | p8         | Geo mean |
|------------------------|------------|------------|------------|------------|----------|
| Channel ping-pong      | **2.39×**  | 1.56×      | 1.79×      | 1.53×      | **1.79×** |
| Ring                   | **2.43×**  | 1.51×      | 1.71×      | 1.65×      | **1.80×** |
| Fan-out / fan-in       | 0.55×      | **1.27×**  | 1.11×      | 1.08×      | **0.95×** |
| Spawn idle tasks       | 0.54×      | 0.11×      | 0.09×      | 0.09×      | **0.13×** |
| Prime sieve            | **1.93×**  | 1.19×      | 0.49×      | 0.24×      | **0.72×** |
| HTTP ping-pong         | **2.34×**  | 1.87×      | 1.83×      | 1.74×      | **1.93×** |
| HTTP throughput        | 1.57×      | 1.52×      | **1.05×**  | 0.62×      | **1.12×** |
| **Overall geo-mean**   |            |            |            |            | **0.93×** |

---

## Known Bottlenecks

### 1. Work-Steal Thrashing (medium-priority)

Stats at p4 show steal miss rates of ~84–97% across benchmarks (higher miss rates
for narrow-pipeline workloads where only one worker is ever runnable):

```
channel ping-pong p4:  155,079 attempts /   4,993 successes / 150,086 misses (97% miss)
ring              p4:  593,123 attempts /  23,783 successes / 569,340 misses (96% miss)
fan-out           p4:  1,002,261 attempts / 131,768 successes / 870,493 misses (87% miss)
prime sieve       p4:  4,235,421 attempts / 658,601 successes / 3,576,820 misses (84% miss)
http-thru         p4:  6,494,856 attempts / 840,234 successes / 5,654,622 misses (87% miss)
```

A per-worker `miss_streak` counter already gates stealing: once a worker
accumulates `STEAL_BACKOFF_THRESHOLD` (= 8) consecutive misses it skips the
steal phase and parks in `uv_run(ONCE)`. The stats reflect misses that
occurred within that window before the worker gave up, not unbounded spinning.

The remaining question is whether the threshold is well-tuned. At 8, workers
stop stealing quickly enough for IO-bound workloads (good for HTTP latency)
but may give up too early for compute-bound pipelines where the next runnable
fiber appears a few microseconds later (hurts prime sieve at p4–p8).

**Next step**: evaluate whether `STEAL_BACKOFF_THRESHOLD` should be raised for
compute-bound pools, or made adaptive (e.g. higher after a recent steal
success, lower after a long idle streak).

### 2. Spawn Idle Task Performance (medium-priority)

Spawn-idle is 0.14× Go geo-mean and barely changes from p2 to p8 (~49k tasks/s
vs Go's 443k–570k). The cb-queue HWM reaches 127,000+ at p2+, meaning hundreds
of thousands of pending callbacks pile up. Cost is dominated by:

1. **Fiber allocation**: each fiber is a fresh GC allocation (no reuse pool).
2. **GC pressure**: incremental collection helps, but 200k allocations still
   trigger many `collect_a_little` stalls at high concurrency.

**Next step**: introduce a fiber free-list / pool per worker. Reuse stack
allocations across fiber lifetimes. This is the single biggest remaining
performance gap vs Go.

### 3. Prime Sieve Scaling Collapse (medium-priority)

Prime sieve drops from 1.95× at p1 to 0.23× at p8. Stats confirm why:

```
prime sieve p8:  9,984,784 steal attempts / 759,880 successes / 9,224,904 misses (92% miss)
                 idle wakeups: 1,051,556
```

The sieve topology is a pipeline — each task passes to exactly one neighbor.
With 8 workers and a single-channel chain, most workers always find nothing to
steal. The idle wakeup count (~1M) shows repeated unnecessary wakeup/sleep
cycles.

**Next step**: the steal thrashing fix (item 1) will help here. Additionally,
consider a sleep-before-steal heuristic — if a worker found nothing last cycle,
don't wake a neighbor on enqueue unless the neighbor is provably idle.

### 4. HTTP Throughput Saturation at p4+ (medium-priority)

HTTP throughput scales well p1→p4 (16k→45k req/s) but plateaus at p8 (~47k).
Occasional errors (32 per run) appear intermittently at p1 or p4 across runs —
not concentrated at p8 as previously observed — suggesting a low-rate race
rather than a systematic overload condition.

TCP/UDP handles are already on per-worker loops and the HTTP client idle pool
uses per-worker-pool mutexes (`idle_mutex` on `http_worker_pool_t`), so the
obvious scalability culprits are ruled out.

**Next step**: reproduce the errors reliably and identify the root cause — currently unknown.

### 5. Fan-out at p1 (low-priority)

Fan-out/fan-in at p1 is 0.54× Go. With a single worker there is no
parallelism; all 8 worker goroutines (in the benchmark) run sequentially. This
is structural — a single-threaded cooperative scheduler will always lose to
Go's preemptive multi-threading here.

**No near-term fix**; document as known limitation for single-core deployments.

---

## Roadmap

| Priority | Item | Expected Gain |
|----------|------|---------------|
| P0 | Fiber reuse pool per worker | Spawn-idle 5–10× improvement |
| P1 | Investigate intermittent HTTP errors | Correctness fix |
| P2 | VMEM GC under I/O load | Close the canary/vmem HTTP gap |
| P3 | Tune `STEAL_BACKOFF_THRESHOLD` | Prime sieve p4–p8 recovery |

---

## Optional / Breaking Changes

### Tagged Union (`goc_item_t`) for Scalar Interop

`goc_array` and `goc_chan` currently store `void*` elements. For scalars (e.g.
`char`, `int`, `double`) this means one heap allocation per element — extremely
wasteful for string arrays and numeric pipelines.

Replace the element representation with a tagged union `goc_item_t` so scalars
are stored inline with no heap allocation.

**Foundation done** (Phase 3):
- `goc_array_of(...)` / `goc_array_of_boxed(T, ...)`: inline construction macros.
- `goc_box(T, val)` / `goc_unbox(T, p)`: unified macro API replacing old per-type
  helpers. Still heap-allocate; `_Generic` scalar dispatch is the remaining work.

**Remaining implementation scope** (breaking API change):

1. Define `goc_tag_t` enum and `goc_item_t` tagged union in `goc.h`.
2. Make `goc_box` / `goc_unbox` allocation-free for scalars via `_Generic`
   dispatch; add a `_goc_tag_of(T)` helper. The `default:` branch falls back
   to `_goc_box_impl` (heap, via `goc_new`). Add explicit pointer-type arms
   (`char*`, `int*`, etc.) to prevent pointer-to-pointer double-wrap.
3. Change `goc_val_t.val` from `void*` → `goc_item_t`; update all
   `result->val` cast sites to `goc_unbox(T, result->val)`.
4. Change `goc_array` backing buffer from `void**` → `goc_item_t*`.
5. Fix `goc_array_from_str` / `goc_array_to_str` to construct `goc_item_t`
   inline (no per-char heap alloc).
6. Change `goc_put(goc_chan*, void*)` → `goc_put(goc_chan*, goc_item_t)`;
   update `put_val` in the internal entry struct and all `goc_put` /
   `goc_put_sync` / `goc_put_cb` call sites.
7. Update all callers: `goc_array.c`, `channel.c`, `fiber.c`, `goc_io.c`,
   `goc_http.c`, `mutex.c`, `loop.c`, and all tests. Grep: `->val`,
   `put_val`, `goc_array_get`, `goc_array_push`, `goc_box`, `goc_unbox`,
   `goc_take`, `goc_val_t`.

**Expected gain**: eliminates O(n) heap allocs for scalar arrays/channels; particularly impactful for `goc_array_from_str` and numeric fan-out workloads.

---

## Non-Goals

- **Spawn-idle parity with Go** at high pool sizes: Go's goroutine scheduler
  reuses stacks and has a dedicated P-M-G model. Matching it without a similar
  redesign is unrealistic in the near term.
- **Prime sieve parity at p8**: the benchmark's topology is inherently serial;
  no scheduler can parallelize a single-chain pipeline beyond what the chain
  supports.
