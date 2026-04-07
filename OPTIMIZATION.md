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

---

## Current Benchmark State

*Best-of-3 runs, libgoc canary vs Go baseline. Full tables in [bench/README.md](bench/README.md).*

| Benchmark              | p1         | p2         | p4         | p8         | Geo mean |
|------------------------|------------|------------|------------|------------|----------|
| Channel ping-pong      | **2.30×**  | 1.46×      | 1.63×      | 1.48×      | **1.69×** |
| Ring                   | **2.32×**  | 1.48×      | 1.61×      | 1.61×      | **1.73×** |
| Fan-out / fan-in       | 0.54×      | **1.29×**  | 1.03×      | 0.99×      | **0.92×** |
| Spawn idle tasks       | 0.50×      | 0.11×      | 0.09×      | 0.09×      | **0.14×** |
| Prime sieve            | **1.95×**  | 1.28×      | 0.49×      | 0.23×      | **0.73×** |
| HTTP ping-pong         | **2.28×**  | 1.61×      | 1.44×      | 1.45×      | **1.67×** |
| HTTP throughput        | 1.58×      | 1.64×      | **0.83×**  | 0.62×      | **1.07×** |
| **Overall geo-mean**   |            |            |            |            | **0.91×** |

---

## Known Bottlenecks

### 1. Work-Steal Thrashing (medium-priority)

Stats at p4 show steal miss rates of ~85–90% across all benchmarks:

```
prime sieve p4:  4,149,135 attempts / 645,201 successes / 3,503,934 misses (84% miss)
http-thru  p4:  6,342,422 attempts / 829,951 successes / 5,512,471 misses (87% miss)
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
prime sieve p8:  9,881,795 steal attempts / 751,711 successes  (92% miss)
                 idle wakeups: 1,047,217
```

The sieve topology is a pipeline — each task passes to exactly one neighbor.
With 8 workers and a single-channel chain, most workers always find nothing to
steal. The idle wakeup count (~1M) shows repeated unnecessary wakeup/sleep
cycles.

**Next step**: the steal thrashing fix (item 1) will help here. Additionally,
consider a sleep-before-steal heuristic — if a worker found nothing last cycle,
don't wake a neighbor on enqueue unless the neighbor is provably idle.

### 4. HTTP Throughput Saturation at p4+ (medium-priority)

HTTP throughput scales p1→p4 (13k→36k req/s) but plateaus or regresses at p8
(26–35k). Errors appear at p8 (66 errors in one run).

TCP/UDP handles are already on per-worker loops and the HTTP client idle pool
uses per-worker-pool mutexes (`idle_mutex` on `http_worker_pool_t`), so the
obvious scalability culprits are ruled out. The error source at p8 is unknown.

**Next step**: reproduce the p8 errors reliably and identify the root cause.
Likely candidates: accept-queue overflow under high concurrency, or a
connection-lifecycle race during pool shutdown.

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
| P1 | Investigate p8 HTTP errors | Correctness fix |
| P2 | VMEM GC under I/O load | Close the canary/vmem HTTP gap |
| P3 | Tune `STEAL_BACKOFF_THRESHOLD` | Prime sieve p4–p8 recovery |

---

## Non-Goals

- **Spawn-idle parity with Go** at high pool sizes: Go's goroutine scheduler
  reuses stacks and has a dedicated P-M-G model. Matching it without a similar
  redesign is unrealistic in the near term.
- **Prime sieve parity at p8**: the benchmark's topology is inherently serial;
  no scheduler can parallelize a single-chain pipeline beyond what the chain
  supports.
