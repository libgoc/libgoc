# `libgoc` Optimization Roadmap

A prioritized roadmap for improving throughput, tail latency, memory efficiency, and operational tuning in `libgoc`, based on the current runtime design (`DESIGN.md`).

---

## Objectives

- Improve scheduler scaling under cross-worker handoff pressure.
- Reduce spawn/materialization overhead and fiber stack memory pressure.
- Improve hot-path performance for channel operations under contention.
- Reduce GC mark overhead at high suspended-fiber counts.
- Lower timer/callback overhead where benchmark evidence justifies it.
- Keep cross-platform behavior consistent (Linux/macOS/Windows).
- Preserve existing correctness guarantees and safety invariants.

---

## Success Metrics (track before/after every phase)

- **Throughput**: ops/s for representative benches (`spawn`, pipeline, fan-in/out).
- **Tail latency**: P95/P99 for channel handoff and timeout firing.
- **Scheduler efficiency**: steal success rate, failed steal probes, idle wakeups.
- **GC cost**: mark/pause time, collections per second, memory retained.
- **Timer overhead**: allocations/timeouts created, callback queue high-water mark.

> Rule: no optimization lands without benchmark deltas and regression checks.

---

## Benchmark Signals Driving Priority

Based on `bench/libgoc/README.md` results (canary mode, pool=1–8):

- **Spawn idle** gap has closed significantly: canary now reaches 84k–166k tasks/s (up from ~18k previously), reflecting recent scheduler improvements. Still below Go parity but no longer the dominant deficit.
- **Ring** now exceeds 1M hops/s at pool=8 (~0.44× Go). Scheduler/handoff costs are reduced but non-trivial; further gains remain possible.
- **Fan-out/fan-in** peaks at pool=4 (258k msg/s) and dips slightly at pool=8 (238k msg/s). Contention exists but the scaling pattern is non-monotonic, not a straight degradation.
- **Prime sieve** peaks at pool=4 (2821 primes/s) and dips slightly at pool=8 (2577 primes/s) — it is not competitive only at pool=1; throughput improves up to pool=4.
- **vmem mode** behaves as a bounded-correctness/stress configuration: ring in particular is 20–40× slower than canary due to TLB/page-fault pressure. vmem is not a target for performance parity work.
- **Timeout/callback-heavy workloads are not yet explicitly benchmarked**, so those optimizations remain important but lower-confidence until coverage is added.

### Phase 0 telemetry baseline (canary, `GOC_ENABLE_STATS=1`, 2026-03-26)

Raw `bench_print_stats()` output across pool sizes 1–8:

```
=== Pool Size: 1 ===
Channel ping-pong: 200000 round trips in 235ms (849952 round trips/s)
  [stats] steal: 0 attempts / 0 successes  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Ring benchmark: 500000 hops across 128 tasks in 1325ms (377350 hops/s)
  [stats] steal: 0 attempts / 0 successes  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1401ms (142754 msg/s)
  [stats] steal: 0 attempts / 0 successes  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Spawn idle tasks: 200000 fibers in 4240ms (47159 tasks/s)
  [stats] steal: 0 attempts / 0 successes  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Prime sieve: 2262 primes up to 20000 in 821ms (2755 primes/s)
  [stats] steal: 0 attempts / 0 successes  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0

=== Pool Size: 2 ===
Channel ping-pong: 200000 round trips in 126ms (1587155 round trips/s)
  [stats] steal: 2 attempts / 0 successes  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Ring benchmark: 500000 hops across 128 tasks in 670ms (745783 hops/s)
  [stats] steal: 2 attempts / 0 successes  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1065ms (187682 msg/s)
  [stats] steal: 2 attempts / 0 successes  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Spawn idle tasks: 200000 fibers in 2712ms (73731 tasks/s)
  [stats] steal: 2 attempts / 0 successes  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Prime sieve: 2262 primes up to 20000 in 936ms (2416 primes/s)
  [stats] steal: 2 attempts / 0 successes  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0

=== Pool Size: 4 ===
Channel ping-pong: 200000 round trips in 120ms (1653954 round trips/s)
  [stats] steal: 12 attempts / 0 successes  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Ring benchmark: 500000 hops across 128 tasks in 776ms (643707 hops/s)
  [stats] steal: 12 attempts / 0 successes  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 906ms (220541 msg/s)
  [stats] steal: 12 attempts / 0 successes  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Spawn idle tasks: 200000 fibers in 2966ms (67427 tasks/s)
  [stats] steal: 12 attempts / 0 successes  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Prime sieve: 2262 primes up to 20000 in 811ms (2788 primes/s)
  [stats] steal: 12 attempts / 0 successes  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0

=== Pool Size: 8 ===
Channel ping-pong: 200000 round trips in 100ms (1992066 round trips/s)
  [stats] steal: 56 attempts / 0 successes  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Ring benchmark: 500000 hops across 128 tasks in 514ms (972028 hops/s)
  [stats] steal: 56 attempts / 0 successes  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 849ms (235570 msg/s)
  [stats] steal: 56 attempts / 0 successes  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Spawn idle tasks: 200000 fibers in 2919ms (68508 tasks/s)
  [stats] steal: 56 attempts / 0 successes  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Prime sieve: 2262 primes up to 20000 in 818ms (2764 primes/s)
  [stats] steal: 56 attempts / 0 successes  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
```

**Key findings:**

- **Steal success rate is 0% across all pool sizes and all benchmarks.** Attempts scale with pool size (0 → 2 → 12 → 56 lifetime total across 5 benchmarks), but not a single steal ever succeeds. This confirms the scheduler is effectively push-based: when a fiber is woken, it is pushed directly onto the target worker's deque, so by the time an idle worker probes for work the deque is already drained. Work stealing is purely a fallback that never fires in practice.
- **Timeouts: 0/0 everywhere.** No benchmark exercises the timeout subsystem. Timeout and callback-queue optimizations remain unvalidated by the current benchmark suite — per the coverage-gap rule above, those items stay gated until dedicated benchmarks exist.
- **cb-queue hwm: 0 everywhere.** The callback queue never accumulates depth under any of these workloads, including the 200k-fiber mass-wakeup in spawn idle. This is consistent with the push-based wakeup path bypassing the callback queue for fiber resumes.
- **Throughput numbers (updated baseline):** ping-pong 850k–1992k rtt/s; ring 377k–972k hops/s; fan-in 143k–236k msg/s; spawn 47k–74k tasks/s; sieve ~2.8k primes/s at pool=1/4/8.

---

## Phase 0 — Instrumentation First (Highest ROI, Lowest Risk)

### Already in place (`goc_stats` — see `TELEMETRY.md`)

The `goc_stats` subsystem (enabled via `-DGOC_ENABLE_STATS=ON`) already covers:

- Pool lifecycle (created/destroyed, thread count)
- Worker status transitions (created, running, idle, stopped) + pending job count
- Fiber lifecycle (created, completed, last worker ID)
- Channel lifecycle (open/closed, buffer size, item count at event time)

### Phase 0 — Completed

All Phase 0 instrumentation gaps have been filled:

- **Channel alts scan counters**: `taker_scans` and `putter_scans` atomic counters on `goc_chan`, incremented in all three `alts` code paths (`alts_try_immediate`, `goc_alts`, `goc_alts_sync`). Reported in the channel close event.
- **Compaction telemetry**: `compaction_runs` and `entries_removed` counters on `goc_chan`, updated in `compact_dead_entries` and in `goc_close` for entries removed at close time. Reported in the channel close event.
- **Callback queue high-water mark**: `g_cb_queue_depth` / `g_cb_queue_hwm` atomics in `loop.c`; accessible via `goc_cb_queue_get_hwm()`.
- **Timeout counters**: `g_timeout_allocations` / `g_timeout_expirations` atomics in `timeout.c`; accessible via `goc_timeout_get_stats()`.
- **Per-worker steal counters**: `steal_attempts` / `steal_successes` on `goc_worker` and global aggregates `g_steal_attempts` / `g_steal_successes` in `pool.c`; per-worker values reported on `STOPPED` events; global aggregate accessible via `goc_pool_get_steal_stats()`.
- **`goc_stats_flush()`**: new synchronous flush API that blocks until the async delivery loop has drained all in-flight events; used in tests to avoid races.
- **Benchmark stats output**: `bench_print_stats()` helper in `bench/libgoc/bench.c` prints a one-line summary of all three accessor values at the end of each benchmark.

See `TELEMETRY.md` for the updated API reference.

---

## Phase 1 — Benchmark-Driven Near-Term Work

### 1) Scheduler scaling: cross-worker handoff and steal efficiency

Prioritize ring/fan-in/fan-out bottlenecks with scheduler-focused changes:

- work-steal victim hinting (probe recently productive victims before full random scan)
- tighter instrumentation of failed steal probes and wake/sleep churn
- verify improvements at thread counts 2/4/8

- **Why**: both ring and fan-out/fan-in show a consistent pool=4→8 throughput dip, indicating cross-worker contention that doesn't resolve with more threads. Ring at 0.44× Go and fan-out dropping ~8% from pool=4 to pool=8 are the clearest remaining scheduler signals.
- **Impact**: high for throughput scaling.
- **Risk**: low-medium.

### 2) Spawn/materialization and stack policy

- optimize materialization path (fiber creation fast path)
- evaluate stack-size policy defaults per benchmark profile
- add detached spawn API (`goc_go_detached`) for true fire-and-forget paths

- **Why**: spawn idle improved dramatically (84–166k tasks/s, up from ~18k) and is no longer the dominant deficit. Further gains are still possible on the materialization fast path and for fire-and-forget patterns, but this is now lower urgency than scheduler scaling.
- **Impact**: medium for spawn-heavy workloads.
- **Risk**: medium.

### 3) Adaptive live-fiber admission cap

Evolve from static cap to adaptive cap using GC pressure + queue depth + memory headroom.

- **Why**: static cap is robust but not workload-aware at scale.
- **Impact**: medium-high for mixed workloads.
- **Risk**: medium (avoid oscillations; keep deterministic fallback).

### 4) Expose tuning knobs in one place

Create one canonical tuning surface (docs + env var reference + recommended presets) for:

- `GOC_POOL_THREADS`
- `GOC_MAX_LIVE_FIBERS`
- stack size (`LIBGOC_STACK_SIZE`)
- stack mode guidance (canary vs vmem)
- any new scheduler/timeout tuning flags added in this roadmap

- **Why**: users need repeatable, workload-specific tuning without digging through multiple docs/files.
- **Impact**: medium (enables real-world gains quickly).
- **Risk**: low.

---

## Phase 2 — Targeted Subsystem Optimizations

### 5) Compaction trigger refinement (hybrid)

Keep fixed threshold but add a density check (`dead_count` relative to live waiters).

- **Why**: avoids over-compacting tiny queues while still cleaning stale-heavy queues quickly.
- **Impact**: small-medium depending on `alts` churn.
- **Risk**: low.

### 6) Callback queue observability + soft backpressure (gated)

Use callback queue depth/high-water metrics to optionally coalesce wakeups and avoid excessive `uv_async_send` churn.

- **Why**: valid optimization, but current benchmark suite lacks callback-storm coverage.
- **Impact**: medium in callback-heavy workloads.
- **Risk**: low-medium (must preserve callback ordering guarantees).

### 7) Timeout batching (gated)

Move from per-timeout `uv_async_t` + `uv_timer_t` allocation to a loop-thread timer manager (heap or wheel) backed by fewer long-lived handles.

- **Why**: current path allocates and closes handles per timeout; likely costly at scale.
- **Impact**: medium-high in timeout-heavy workloads.
- **Risk**: medium (careful shutdown semantics and close ordering required).

---

## Phase 3 — Advanced / Experimental

### 8) Optional reduced stack-root scan range

Explore scanning `[scan_from, stack_top]` instead of full `[stack_base, stack_top]` for suspended fibers, behind a strict opt-in flag.

- **Why**: full-range scan is safe but potentially expensive with many suspended fibers.
- **Impact**: potentially high GC mark-time reduction.
- **Risk**: high (must prove no missed roots; keep conservative default).

### 9) Waiter-list partitioning by entry kind

Evaluate separating fiber/callback/sync wait queues or kind-grouping to reduce branchy scans.

- **Why**: mixed-kind queues force branching in hot wake paths.
- **Impact**: medium if workloads are heavily mixed.
- **Risk**: medium-high (touches core channel invariants).

---

## Prioritization Matrix

| Item | Impact | Risk | Priority |
|---|---|---|---|
| Phase 0 instrumentation | High (enabler) | Low | P0 |
| Scheduler scaling (handoff/steal) | High | Low-Med | P1 |
| Spawn/materialization + stack policy | Medium | Medium | P2 |
| Adaptive admission cap | Med-High | Medium | P1 |
| Expose tuning knobs in one place | Medium | Low | P1 |
| Compaction hybrid trigger | Small-Med | Low | P2 |
| Callback queue tuning (gated) | Medium | Low-Med | P2 |
| Timeout batching (gated) | Med-High | Medium | P2 |
| Reduced stack-root scan | High | High | P3 (experimental) |
| Waiter-list partitioning | Medium | Med-High | P3 |

---

## Benchmark Plan Per Phase

For each phase:

1. Run existing benchmark suites plus one stress case for the touched subsystem.
2. Compare against baseline on:
   - throughput
   - P95/P99 latency
   - memory footprint / GC time
3. Reject changes that improve one metric while causing unacceptable regressions in others.

### Coverage-gap rule

- Before promoting callback/timeout optimizations, first add dedicated callback-storm and timeout-storm benchmarks.
- Keep those items in **gated P2** status until benchmark coverage exists and captures a reproducible bottleneck.

### Recommended extra benchmark scenarios

- **Spawn-heavy**: bounded vs unbounded live fibers.
- **Timeout-heavy**: large concurrent timeout creation bursts.
- **Callback-heavy**: loop-thread callback flood.
- **Mixed**: channels + timeouts + callback completions together.

---

## Guardrails

- Preserve shutdown correctness (`uv_close` loop-thread ownership, callback queue drain).
- Preserve channel close/wake invariants (`close_guard`, `try_claim_wake`, `cancelled` handling).
- Keep platform parity (POSIX + Windows thread-registration differences).
- Keep conservative behavior as default for any experimental GC-scan optimizations.

---

## Milestone Definition of Done

A roadmap item is complete only if:

1. Code lands with tests and docs updates.
2. Benchmarks show measurable improvement for the intended workload.
3. No correctness regressions in existing test phases.
4. Feature flags/defaults are documented with recommended settings.