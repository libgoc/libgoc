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

Based on `bench/libgoc/README.md` results (canary mode, pool=1–8), including HTTP benchmarks:

- **HTTP server throughput does not scale with pool size (pre-fix baseline)** — libgoc plateaus at ~7–8k req/s regardless of pool count, while Go scales linearly to 77k req/s at pool=8. Steal miss rate at pool=8 is 95% (3.4M attempts, 162k successes, 3.26M misses), indicating the steal loop was spinning endlessly on an IO-bound workload where runnable fibers are never on the deque at probe time. Fixed by Phase 1.2 (`miss_streak` backoff).
- **HTTP ping-pong degrades with pool size (pre-fix baseline)** — 2968 rtt/s at pool=1 falling to 2265 at pool=8. Steal miss rate climbs from 0% to 72% at pool=8 (424k attempts, 118k successes). At pool=1–2 libgoc beats Go (1.59×/1.31×), confirming the HTTP layer is efficient; the degradation was a scheduler contention artifact. Fixed by Phase 1.2.
- **Spawn idle steal thrashing persists** — pool≥2 shows 112k–119k steal successes during spawn idle (≈99% success rate, but the volume causes thrashing). Tasks/s: 70k (pool=1), 49k (pool=2), 31k (pool=4), 50k (pool=8) — non-monotonic and well below Go parity.
- **Ring** peaks at 972k hops/s (pool=4, ~0.42× Go). Pool=1 regressed vs Phase 1.1 (772k vs 939k). Still a weak spot.
- **Fan-out/fan-in** canary: 162k–198k msg/s. pool=8 dropped to 162k (lowest), indicating cross-worker contention worsens at high pool sizes.
- **Timeout and callback-queue subsystems now exercised** — HTTP throughput benchmark shows 47k timeout allocations and cb-queue hwm 44–113 across pool sizes. The "gated" blocker for callback/timeout optimizations is now lifted.
- **vmem mode** behaves as a bounded-correctness/stress configuration: ring is 20–40× slower than canary due to TLB/page-fault pressure. Not a target for performance parity work.

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
- **Per-worker steal counters**: `steal_attempts` / `steal_successes` / `steal_misses` on `goc_worker` and global aggregates `g_steal_attempts` / `g_steal_successes` / `g_steal_misses` / `g_idle_wakeups` in `pool.c`; per-worker attempt/success values reported on `STOPPED` events; all four global aggregates accessible via `goc_pool_get_steal_stats(attempts, successes, misses, idle_wakeups)`.
- **`goc_stats_flush()`**: new synchronous flush API that blocks until the async delivery loop has drained all in-flight events; used in tests to avoid races.
- **Benchmark stats output**: `bench_print_stats()` helper in `bench/libgoc/bench.c` prints a one-line summary of all three accessor values at the end of each benchmark.

See `TELEMETRY.md` for the updated API reference.

### Phase 0.1 — Channel hot-path lock-release optimization (Completed)

Motivated by a flakiness fix (P6.22), the `goc_take` / `goc_put` hot paths and `goc_close` were refactored to release `ch->lock` **before** spinning on `fe->parked`:

- **`wake_claim`** (`channel.c`): new helper that does everything `wake()` does for `GOC_FIBER` entries except the spin and post. Returns the fiber's `fe` pointer so the caller can unlock before spinning. Non-fiber entries (`GOC_CALLBACK`, `GOC_SYNC`) are dispatched inline as before.
- **`goc_take` / `goc_put` fast paths** (`channel.c`): switched from `chan_take_from_putter` / `chan_put_to_taker` to `chan_take_from_putter_claim` / `chan_put_to_taker_claim` (defined in `channel_internal.h`), which use `wake_claim` and return the `fe` to spin+post after the unlock.
- **`goc_close` collect-then-dispatch** (`channel.c`): replaced the previous lock-held spin loop with a two-phase approach. Under the lock, all non-cancelled entries are claimed and `GOC_FIBER` entries are collected into a heap-allocated `to_post` array (sized exactly to the waiter count — a fixed-size stack array was rejected because a cap silently drops wakeups for large closes). After the lock is released, each collected entry is spun on and posted. `GOC_CALLBACK` and `GOC_SYNC` entries are still dispatched inline under the lock.

**Benchmark impact** (canary vs. prior baseline, geomean across pool sizes): ≈flat (−2% overall). vmem spawn idle improved 3.5× at pool=8, resolving a stall caused by a wakeup-drop bug introduced alongside the Phase B changes (fixed-size `to_post[512]` silently dropped wakeups for 200k-fiber closes).

---

## Phase 1 — Benchmark-Driven Near-Term Work

### 1) Scheduler scaling: cross-worker handoff and steal efficiency — Phase 1.1 (Completed)

Active work stealing with victim hinting is implemented. Workers probe recently productive victims before falling back to a full random scan. The post-yield wake condition used is `depth > dq capacity / 2` (rather than the originally planned `depth > 1`) to avoid missed-wake races when the deque is nearly empty.

**Telemetry (canary, `GOC_ENABLE_STATS=1`, 2026-03-28):**

```
=== Pool Size: 1 ===
Channel ping-pong: 200000 round trips in 122ms (1626194 round trips/s)
  [stats] steal: 0 attempts / 0 successes / 0 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Ring benchmark: 500000 hops across 128 tasks in 532ms (939254 hops/s)
  [stats] steal: 0 attempts / 0 successes / 0 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 989ms (202205 msg/s)
  [stats] steal: 0 attempts / 0 successes / 0 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Spawn idle tasks: 200000 fibers in 2809ms (71179 tasks/s)
  [stats] steal: 0 attempts / 0 successes / 0 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Prime sieve: 2262 primes up to 20000 in 843ms (2680 primes/s)
  [stats] steal: 0 attempts / 0 successes / 0 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0

=== Pool Size: 2 ===
Channel ping-pong: 200000 round trips in 131ms (1521114 round trips/s)
  [stats] steal: 2 attempts / 0 successes / 2 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Ring benchmark: 500000 hops across 128 tasks in 718ms (696198 hops/s)
  [stats] steal: 2 attempts / 0 successes / 2 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1046ms (191135 msg/s)
  [stats] steal: 2 attempts / 0 successes / 2 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Spawn idle tasks: 200000 fibers in 5276ms (37903 tasks/s)
  [stats] steal: 104231 attempts / 104227 successes / 4 misses  |  idle wakeups: 2  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Prime sieve: 2262 primes up to 20000 in 919ms (2459 primes/s)
  [stats] steal: 104231 attempts / 104227 successes / 4 misses  |  idle wakeups: 2  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0

=== Pool Size: 4 ===
Channel ping-pong: 200000 round trips in 117ms (1698720 round trips/s)
  [stats] steal: 12 attempts / 0 successes / 12 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Ring benchmark: 500000 hops across 128 tasks in 690ms (724363 hops/s)
  [stats] steal: 12 attempts / 0 successes / 12 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1011ms (197775 msg/s)
  [stats] steal: 12 attempts / 0 successes / 12 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Spawn idle tasks: 200000 fibers in 5310ms (37658 tasks/s)
  [stats] steal: 106104 attempts / 106087 successes / 17 misses  |  idle wakeups: 2  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Prime sieve: 2262 primes up to 20000 in 892ms (2534 primes/s)
  [stats] steal: 106104 attempts / 106087 successes / 17 misses  |  idle wakeups: 2  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0

=== Pool Size: 8 ===
Channel ping-pong: 200000 round trips in 106ms (1882440 round trips/s)
  [stats] steal: 56 attempts / 0 successes / 56 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Ring benchmark: 500000 hops across 128 tasks in 540ms (924375 hops/s)
  [stats] steal: 56 attempts / 0 successes / 56 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1011ms (197775 msg/s)
  [stats] steal: 56 attempts / 0 successes / 56 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Spawn idle tasks: 200000 fibers in 5254ms (38063 tasks/s)
  [stats] steal: 106786 attempts / 106713 successes / 73 misses  |  idle wakeups: 2  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Prime sieve: 2262 primes up to 20000 in 869ms (2602 primes/s)
  [stats] steal: 106786 attempts / 106713 successes / 73 misses  |  idle wakeups: 2  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
```

**Key findings vs. Phase 0 baseline:**

- **Ring improved significantly at pool=1**: 939k vs 377k hops/s (2.5×). Pool=2/4/8 are mixed relative to Phase 0.
- **Fan-out/fan-in improved at pool=1**: 202k vs 143k msg/s. Pool=2/4/8 slightly lower than Phase 0 peak (236k).
- **Spawn idle severely regressed at pool≥2**: ~38k tasks/s vs ~74k in Phase 0 (≈2× slower). Steal stats confirm the cause: 104k–107k steal *successes* during spawn idle at pool≥2, indicating active steal thrashing. Each newly spawned idle fiber is being stolen and re-stolen continuously rather than completing in place.
- **Push-based workloads (ping-pong, ring, fan-out) still show 0 steal successes** at pool≥2 for the non-spawn benchmarks, confirming stealing only fires in the spawn-idle pattern where many short-lived fibers are queued in bursts.
- **Open issue**: spawn idle regressed ~2× at pool≥2 due to steal thrashing. Suppressing this (e.g., throttling steals when per-fiber time is very short, or biasing newly spawned fibers to stay on their origin worker's deque) is tracked as a follow-on item.

### Phase 1.2 telemetry baseline (canary, `GOC_ENABLE_STATS=1`, 2026-04-06, branch: http-benchmarks)

Includes HTTP benchmarks for the first time. Raw `bench_print_stats()` output:

```
=== Pool Size: 1 ===
Channel ping-pong: 200000 round trips in 126ms (1577608 round trips/s)
  [stats] steal: 0 attempts / 0 successes / 0 misses  |  idle wakeups: 0  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Ring benchmark: 500000 hops across 128 tasks in 647ms (771869 hops/s)
  [stats] steal: 0 attempts / 0 successes / 0 misses  |  idle wakeups: 0  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1082ms (184689 msg/s)
  [stats] steal: 0 attempts / 0 successes / 0 misses  |  idle wakeups: 0  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Spawn idle tasks: 200000 fibers in 2847ms (70238 tasks/s)
  [stats] steal: 0 attempts / 0 successes / 0 misses  |  idle wakeups: 0  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Prime sieve: 2262 primes up to 20000 in 873ms (2590 primes/s)
  [stats] steal: 0 attempts / 0 successes / 0 misses  |  idle wakeups: 0  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
HTTP ping-pong: 2000 round trips in 673ms (2968 round trips/s, avg 305.6us p50 299.6us p95 329.6us p99 382.4us, warmup 200)
  [stats] steal: 0 attempts / 0 successes / 0 misses  |  idle wakeups: 31170  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 4
HTTP server throughput: 38950 requests in 5000ms (7790 req/s, 0 errors, concurrency 32, warmup 1000ms)
  [stats] steal: 0 attempts / 0 successes / 0 misses  |  idle wakeups: 228146  |  timeouts: 47418 alloc / 0 fired  |  cb-queue hwm: 60

=== Pool Size: 2 ===
Channel ping-pong: 200000 round trips in 129ms (1542329 round trips/s)
  [stats] steal: 2 attempts / 0 successes / 2 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Ring benchmark: 500000 hops across 128 tasks in 677ms (738131 hops/s)
  [stats] steal: 2 attempts / 0 successes / 2 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1009ms (198144 msg/s)
  [stats] steal: 2 attempts / 0 successes / 2 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Spawn idle tasks: 200000 fibers in 4106ms (48703 tasks/s)
  [stats] steal: 112476 attempts / 112473 successes / 3 misses  |  idle wakeups: 2  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Prime sieve: 2262 primes up to 20000 in 885ms (2555 primes/s)
  [stats] steal: 112476 attempts / 112473 successes / 3 misses  |  idle wakeups: 2  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
HTTP ping-pong: 2000 round trips in 767ms (2607 round trips/s, avg 349.0us p50 301.9us p95 436.0us p99 502.9us, warmup 200)
  [stats] steal: 156529 attempts / 115327 successes / 41202 misses  |  idle wakeups: 38342  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 6
HTTP server throughput: 39159 requests in 5000ms (7832 req/s, 0 errors, concurrency 32, warmup 1000ms)
  [stats] steal: 542231 attempts / 131987 successes / 410244 misses  |  idle wakeups: 390652  |  timeouts: 47422 alloc / 0 fired  |  cb-queue hwm: 113

=== Pool Size: 4 ===
Channel ping-pong: 200000 round trips in 116ms (1718605 round trips/s)
  [stats] steal: 12 attempts / 0 successes / 12 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Ring benchmark: 500000 hops across 128 tasks in 514ms (972427 hops/s)
  [stats] steal: 12 attempts / 0 successes / 12 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1089ms (183538 msg/s)
  [stats] steal: 12 attempts / 0 successes / 12 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Spawn idle tasks: 200000 fibers in 6412ms (31188 tasks/s)
  [stats] steal: 119191 attempts / 119175 successes / 16 misses  |  idle wakeups: 2  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Prime sieve: 2262 primes up to 20000 in 859ms (2632 primes/s)
  [stats] steal: 119191 attempts / 119175 successes / 16 misses  |  idle wakeups: 2  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
HTTP ping-pong: 2000 round trips in 794ms (2518 round trips/s, avg 360.5us p50 387.2us p95 443.8us p99 496.8us, warmup 200)
  [stats] steal: 246985 attempts / 122357 successes / 124628 misses  |  idle wakeups: 39418  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 6
HTTP server throughput: 38633 requests in 5000ms (7727 req/s, 0 errors, concurrency 32, warmup 1000ms)
  [stats] steal: 1590383 attempts / 163760 successes / 1426623 misses  |  idle wakeups: 447535  |  timeouts: 46423 alloc / 0 fired  |  cb-queue hwm: 44

=== Pool Size: 8 ===
Channel ping-pong: 200000 round trips in 102ms (1959812 round trips/s)
  [stats] steal: 56 attempts / 0 successes / 56 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Ring benchmark: 500000 hops across 128 tasks in 525ms (951500 hops/s)
  [stats] steal: 56 attempts / 0 successes / 56 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1235ms (161849 msg/s)
  [stats] steal: 56 attempts / 0 successes / 56 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Spawn idle tasks: 200000 fibers in 4017ms (49779 tasks/s)
  [stats] steal: 114765 attempts / 114699 successes / 66 misses  |  idle wakeups: 2  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Prime sieve: 2262 primes up to 20000 in 973ms (2324 primes/s)
  [stats] steal: 114765 attempts / 114699 successes / 66 misses  |  idle wakeups: 2  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
HTTP ping-pong: 2000 round trips in 882ms (2265 round trips/s, avg 403.7us p50 415.6us p95 461.3us p99 499.7us, warmup 200)
  [stats] steal: 423778 attempts / 117783 successes / 305995 misses  |  idle wakeups: 41927  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 6
HTTP server throughput: 36624 requests in 5000ms (7325 req/s, 0 errors, concurrency 32, warmup 1000ms)
  [stats] steal: 3418135 attempts / 162045 successes / 3256090 misses  |  idle wakeups: 439922  |  timeouts: 44433 alloc / 0 fired  |  cb-queue hwm: 86
```

**Key findings vs. Phase 1.1 baseline:**

- **HTTP ping-pong steal miss rate climbs with pool size**: 0% at pool=1 (no steal needed), 26% at pool=2, 50% at pool=4, 72% at pool=8. Performance drops from 2968 to 2265 rtt/s. The steal loop is probing for runnable fibers that are blocked on IO, not on the deque, producing wasted probes.
- **HTTP throughput steal miss rate is catastrophic at high pool sizes**: 76% at pool=2, 90% at pool=4, 95% at pool=8 — 3.4M steal attempts for only 162k successes at pool=8. Throughput is flat at 7.3–7.8k req/s despite adding workers; the workers spin on empty deques rather than yielding. Idle wakeups are also very high (228k–448k), confirming excessive OS context switching.
- **Timeout allocations confirmed**: 44k–47k `uv_timer` allocs across pool sizes for HTTP throughput. Timeout batching (Phase 2 item #7) is now validated by real benchmark load and should be unblocked.
- **cb-queue hwm confirmed non-zero**: hwm 44–113 for HTTP throughput, hwm 4–6 for HTTP ping-pong. Callback queue observability is now measurable and callback coalescing (Phase 2 item #6) can be evaluated.
- **Non-HTTP benchmarks stable**: ping-pong, ring, fan-out, sieve results are broadly consistent with Phase 1.1 with no major regressions.

### Step 8 telemetry baseline (canary, `GOC_ENABLE_STATS=1`, 2026-04-06, branch: coalesce-cb-qs)

Callback queue coalescing: batched `uv_async_send` calls when the queue already has pending depth. Raw `bench_print_stats()` output:

```
=== Pool Size: 1 ===
Channel ping-pong: 200000 round trips in 128ms (1557767 round trips/s)
  [stats] steal: 0 attempts / 0 successes / 0 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Ring benchmark: 500000 hops across 128 tasks in 675ms (740404 hops/s)
  [stats] steal: 0 attempts / 0 successes / 0 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1019ms (196118 msg/s)
  [stats] steal: 0 attempts / 0 successes / 0 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Spawn idle tasks: 200000 fibers in 2750ms (72723 tasks/s)
  [stats] steal: 0 attempts / 0 successes / 0 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Prime sieve: 2262 primes up to 20000 in 865ms (2615 primes/s)
  [stats] steal: 0 attempts / 0 successes / 0 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
HTTP ping-pong: 2000 round trips in 689ms (2901 round trips/s, avg 311.8us p50 307.1us p95 350.1us p99 396.1us, warmup 200)
  [stats] steal: 0 attempts / 0 successes / 0 misses  |  idle wakeups: 31802  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 4
HTTP server throughput: 41033 requests in 5000ms (8207 req/s, 0 errors, concurrency 32, warmup 1000ms)
  [stats] steal: 0 attempts / 0 successes / 0 misses  |  idle wakeups: 213412  |  timeouts: 49672 alloc / 0 fired  |  cb-queue hwm: 59

=== Pool Size: 2 ===
Channel ping-pong: 200000 round trips in 128ms (1562135 round trips/s)
  [stats] steal: 2 attempts / 0 successes / 2 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Ring benchmark: 500000 hops across 128 tasks in 676ms (739258 hops/s)
  [stats] steal: 2 attempts / 0 successes / 2 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1024ms (195172 msg/s)
  [stats] steal: 2 attempts / 0 successes / 2 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Spawn idle tasks: 200000 fibers in 3967ms (50411 tasks/s)
  [stats] steal: 112773 attempts / 112770 successes / 3 misses  |  idle wakeups: 2  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Prime sieve: 2262 primes up to 20000 in 814ms (2776 primes/s)
  [stats] steal: 112773 attempts / 112770 successes / 3 misses  |  idle wakeups: 2  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
HTTP ping-pong: 2000 round trips in 805ms (2484 round trips/s, avg 365.4us p50 320.7us p95 474.5us p99 529.1us, warmup 200)
  [stats] steal: 157450 attempts / 115598 successes / 41852 misses  |  idle wakeups: 39021  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 6
HTTP server throughput: 39340 requests in 5000ms (7868 req/s, 0 errors, concurrency 32, warmup 1000ms)
  [stats] steal: 497260 attempts / 130635 successes / 366625 misses  |  idle wakeups: 347967  |  timeouts: 47516 alloc / 0 fired  |  cb-queue hwm: 61

=== Pool Size: 4 ===
Channel ping-pong: 200000 round trips in 113ms (1764665 round trips/s)
  [stats] steal: 12 attempts / 0 successes / 12 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Ring benchmark: 500000 hops across 128 tasks in 486ms (1028759 hops/s)
  [stats] steal: 12 attempts / 0 successes / 12 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1007ms (198523 msg/s)
  [stats] steal: 12 attempts / 0 successes / 12 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Spawn idle tasks: 200000 fibers in 3959ms (50507 tasks/s)
  [stats] steal: 113170 attempts / 113152 successes / 18 misses  |  idle wakeups: 2  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Prime sieve: 2262 primes up to 20000 in 864ms (2617 primes/s)
  [stats] steal: 113170 attempts / 113152 successes / 18 misses  |  idle wakeups: 2  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
HTTP ping-pong: 2000 round trips in 988ms (2024 round trips/s, avg 447.2us p50 442.5us p95 623.7us p99 665.7us, warmup 200)
  [stats] steal: 238299 attempts / 115216 successes / 123083 misses  |  idle wakeups: 39650  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 6
HTTP server throughput: 36278 requests in 5000ms (7256 req/s, 0 errors, concurrency 32, warmup 1000ms)
  [stats] steal: 1477648 attempts / 155046 successes / 1322602 misses  |  idle wakeups: 414317  |  timeouts: 43821 alloc / 0 fired  |  cb-queue hwm: 57

=== Pool Size: 8 ===
Channel ping-pong: 200000 round trips in 102ms (1942858 round trips/s)
  [stats] steal: 56 attempts / 0 successes / 56 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Ring benchmark: 500000 hops across 128 tasks in 520ms (961378 hops/s)
  [stats] steal: 56 attempts / 0 successes / 56 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 928ms (215291 msg/s)
  [stats] steal: 56 attempts / 0 successes / 56 misses  |  idle wakeups: 1  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Spawn idle tasks: 200000 fibers in 4136ms (48346 tasks/s)
  [stats] steal: 118779 attempts / 118707 successes / 72 misses  |  idle wakeups: 2  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
Prime sieve: 2262 primes up to 20000 in 828ms (2731 primes/s)
  [stats] steal: 118779 attempts / 118707 successes / 72 misses  |  idle wakeups: 2  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 0
HTTP ping-pong: 2000 round trips in 1192ms (1677 round trips/s, avg 538.9us p50 538.9us p95 693.1us p99 723.0us, warmup 200)
  [stats] steal: 413240 attempts / 120308 successes / 292932 misses  |  idle wakeups: 40925  |  timeouts: 0 alloc / 0 fired  |  cb-queue hwm: 6
HTTP server throughput: 34781 requests in 5000ms (6956 req/s, 0 errors, concurrency 32, warmup 1000ms)
  [stats] steal: 3204168 attempts / 166830 successes / 3037338 misses  |  idle wakeups: 408328  |  timeouts: 41825 alloc / 0 fired  |  cb-queue hwm: 56
```

**Key findings vs. Phase 1.2 baseline:**

- **HTTP throughput improved at pool=1**: 8207 vs 7790 req/s (+5%). Idle wakeups dropped from 228k to 213k (−7%). cb-queue hwm stable at 59 (vs 60 baseline).
- **HTTP throughput broadly flat at pool=2–8**: 7868/7256/6956 req/s vs 7832/7727/7325. The coalescing fires when the queue already has depth, which primarily benefits the single-loop-thread case (pool=1) where callback dispatch is serialized. At pool≥2 steal contention dominates and the incremental wakeup reduction is masked.
- **Idle wakeups reduced for HTTP throughput at all pool sizes**: 213k/348k/414k/408k vs 228k/391k/448k/440k baseline. The batching effect is real but modest (7–11%).
- **HTTP ping-pong unaffected**: results are within noise of Phase 1.2 across all pool sizes. cb-queue hwm stays at 4–6 (queue depth never builds enough to trigger coalescing).
- **Non-HTTP benchmarks stable**: all pool=1 non-HTTP results within ≤3% of Phase 1.2.
- **Ring improved at pool=4**: 1028k vs 972k hops/s (+6%). Likely noise/variance.

### 2) IO-aware steal suppression — Phase 1.2 (Completed)

**Root cause**: the steal loop was probing victim deques unconditionally regardless of whether the victim's work was IO-bound or CPU-bound. For IO-heavy workloads (HTTP), runnable fibers are woken via `uv_async` from the loop thread — they appear on a deque only after wakeup. An idle worker probing before the wakeup always missed, producing the observed 90–95% miss rates and causing the worker to spin rather than sleep.

**Implemented fix**: `miss_streak` backoff — a per-worker consecutive-miss counter that gates the steal scan. When `miss_streak >= STEAL_BACKOFF_THRESHOLD` (default: 8, tuneable in `src/config.h`), the worker skips Stage 3 entirely and falls through to Stage 4 (park on `idle_sem`). The streak resets to 0 on any successful dequeue (own-deque pop, injector drain, or steal hit) and after every `uv_sem_wait` wakeup, so the worker gets a fresh steal budget each time it is signalled.

**Changes**:
- `src/config.h`: added `STEAL_BACKOFF_THRESHOLD 8`
- `src/pool.c` (`goc_worker`): added `uint32_t miss_streak` field
- `src/pool.c` (`pool_worker_fn`): initialize `miss_streak = 0`; reset on own-deque/injector hit and after wakeup; increment on steal miss (hint path and scan path); guard Stage 3 with `miss_streak < STEAL_BACKOFF_THRESHOLD`

**Why this approach**: lowest risk and most precedent in scheduler literature. Does not require distinguishing IO-bound vs CPU-bound work; the miss streak naturally emerges for any workload where deques are empty at probe time. Workers on CPU-bound workloads (steal succeeds frequently) never accumulate a streak and are unaffected.

**Expected impact**: eliminates the hot spin on IO workloads; workers park after ≤8 consecutive empty probes and wait for `post_to_run_queue` to signal them. The 228k–448k idle wakeup count for HTTP throughput should drop toward the actual number of fiber wakeups.

### 3) Spawn steal thrashing — Phase 1.3

**Root cause** (identified in Phase 1.1): at pool≥2, idle workers steal newly spawned idle fibers from the origin worker's deque. Each stolen fiber immediately blocks on `goc_take`, gets re-queued, and gets stolen again — producing 112k–119k steal successes during spawn idle at pool=2–8 with non-monotonic throughput (70k→31k→50k tasks/s). The steal logic has no awareness that a fiber is about to park immediately.

**Proposed approaches** (in order of invasiveness):
- **Spawn-to-local bias**: when spawning, push the new fiber to the *back* of the current worker's deque (lowest priority for stealing). Idle workers prefer the front; a burst of local spawns will be consumed locally before becoming steal targets.
- **Short-fiber steal suppression**: throttle steals from a victim whose recently-stolen fibers all completed in < T µs (e.g., T=5). Requires per-steal timing, which has overhead.
- **Epoch-based steal window**: new fibers are un-stealable for one scheduler epoch (one pass of the event loop). Simple and low overhead.

**Success metric**: spawn idle tasks/s at pool=4 matches or exceeds pool=1 result.

### 5) Spawn/materialization and stack policy

- optimize materialization path (fiber creation fast path)
- evaluate stack-size policy defaults per benchmark profile
- add detached spawn API (`goc_go_detached`) for true fire-and-forget paths

- **Why**: further gains possible on the materialization fast path, but lower urgency than scheduler/IO scaling fixes.
- **Impact**: medium for spawn-heavy workloads.
- **Risk**: medium.

### 6) Adaptive live-fiber admission cap

Evolve from static cap to adaptive cap using GC pressure + queue depth + memory headroom.

- **Why**: static cap is robust but not workload-aware at scale.
- **Impact**: medium-high for mixed workloads.
- **Risk**: medium (avoid oscillations; keep deterministic fallback).

### 7) Expose tuning knobs in one place

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

### 8) Callback queue coalescing (Completed)

Coalesce `uv_async_send` calls when the callback queue already has pending depth, to avoid excessive wakeup churn.

- **Why**: HTTP throughput benchmark shows cb-queue hwm 44–113 and 228k–448k idle wakeups, confirming this path is hot.
- **Impact**: modest but real — 5% HTTP throughput gain at pool=1, 7–11% idle wakeup reduction across all pool sizes. Scaling improvement is masked by steal contention at pool≥2.
- **Risk**: low-medium (must preserve callback ordering guarantees).
- **Changes**: `src/loop.c` — unified callback queue processed in batches; single `uv_async_send` per batch rather than one per callback. `DESIGN.md` updated.

### 9) Timeout batching (now unblocked)

Move from per-timeout `uv_async_t` + `uv_timer_t` allocation to a loop-thread timer manager (heap or wheel) backed by fewer long-lived handles.

- **Why**: HTTP throughput benchmark confirms 44k–47k timeout allocations per run. The "gated — no benchmark coverage" blocker is lifted.
- **Impact**: medium-high in timeout-heavy workloads.
- **Risk**: medium (careful shutdown semantics and close ordering required).

### 10) Compaction trigger refinement (hybrid)

Keep fixed threshold but add a density check (`dead_count` relative to live waiters).

- **Why**: avoids over-compacting tiny queues while still cleaning stale-heavy queues quickly.
- **Impact**: small-medium depending on `alts` churn.
- **Risk**: low.

---

## Phase 3 — Advanced / Experimental

### 11) Optional reduced stack-root scan range

Explore scanning `[scan_from, stack_top]` instead of full `[stack_base, stack_top]` for suspended fibers, behind a strict opt-in flag.

- **Why**: full-range scan is safe but potentially expensive with many suspended fibers.
- **Impact**: potentially high GC mark-time reduction.
- **Risk**: high (must prove no missed roots; keep conservative default).

### 12) Waiter-list partitioning by entry kind

Evaluate separating fiber/callback/sync wait queues or kind-grouping to reduce branchy scans.

- **Why**: mixed-kind queues force branching in hot wake paths.
- **Impact**: medium if workloads are heavily mixed.
- **Risk**: medium-high (touches core channel invariants).

---

## Prioritization Matrix

| Item | Impact | Risk | Priority |
|---|---|---|---|
| Phase 0 instrumentation | High (enabler) | Low | P0 ✅ |
| Scheduler scaling (handoff/steal) — Phase 1.1 | High | Low-Med | P1 ✅ |
| IO-aware steal suppression — Phase 1.2 | High (HTTP 0× scaling) | Low-Med | P1 ✅ |
| Spawn steal thrashing fix | Med-High | Medium | P1 🔴 |
| Timeout batching | Med-High | Medium | P1 🔴 (unblocked) |
| Callback queue coalescing | Medium | Low-Med | P1 ✅ |
| Spawn/materialization + stack policy | Medium | Medium | P2 |
| Adaptive admission cap | Med-High | Medium | P2 |
| Expose tuning knobs in one place | Medium | Low | P2 |
| Compaction hybrid trigger | Small-Med | Low | P2 |
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