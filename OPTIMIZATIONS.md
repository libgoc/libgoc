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

Based on `bench/README.md` results:

- **Spawn idle** has the largest gap vs Go (canary ~0.03x–0.12x).
- **Ring** is consistently weak (canary ~0.31x–0.40x), pointing to scheduler/handoff costs.
- **Fan-out/fan-in** degrades as thread count rises, indicating scaling/contention issues.
- **Prime sieve** is competitive only at pool=1, then declines with more threads.
- **Timeout/callback-heavy workloads are not yet explicitly benchmarked**, so those optimizations remain important but lower-confidence until coverage is added.

---

## Phase 0 — Instrumentation First (Highest ROI, Lowest Risk)

### Scope

1. Add runtime counters (compile-time gated) for:
   - channel queue scan lengths
   - dead-entry compaction runs and removed entries
   - callback queue depth/high-water mark
   - timeout allocations and expirations
   - per-worker dequeue/pop/steal attempts + successes
2. Add benchmark output fields for these counters.
3. Add a short tuning section in docs mapping workload types to key knobs.

### Expected Impact

- Faster identification of real bottlenecks.
- Prevents optimizing the wrong subsystem.

### Risk

- Low (mostly additive, diagnostics-focused).

---

## Phase 1 — Benchmark-Driven Near-Term Work

### 1) Scheduler scaling: cross-worker handoff and steal efficiency

Prioritize ring/fan-in/fan-out bottlenecks with scheduler-focused changes:

- work-steal victim hinting (probe recently productive victims before full random scan)
- tighter instrumentation of failed steal probes and wake/sleep churn
- verify improvements at thread counts 2/4/8

- **Why**: ring and multi-thread fan-out/fan-in gaps are large and persistent.
- **Impact**: high for throughput scaling.
- **Risk**: low-medium.

### 2) Spawn/materialization and stack policy

Target the dominant spawn gap first:

- optimize materialization path (fiber creation fast path)
- evaluate stack-size policy defaults per benchmark profile
- add detached spawn API (`goc_go_detached`) for true fire-and-forget paths

- **Why**: `spawn idle` is currently the largest deficit.
- **Impact**: high for spawn-heavy workloads.
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
| Spawn/materialization + stack policy | High | Medium | P1 |
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
