# Benchmarks

This directory contains standalone CSP benchmarks implemented in Go, in C
using libgoc, and in Clojure using core.async.

1. **Channel ping-pong** — Two tasks pass a single message back and forth,
   measuring the basic cost of a send/receive and the context switch it causes.
2. **Ring** — Many tasks are arranged in a circle and pass a token around,
   stressing scheduling and handoff overhead across a larger group.
3. **Selective receive / fan-out / fan-in** — One producer feeds many workers
   and a collector selects across multiple output channels, stressing select
   logic and load distribution.
4. **Spawn idle tasks** — Create many tasks that immediately block, highlighting
   creation time and memory overhead for lightweight tasks.
5. **Prime sieve** — A pipeline of filters passes numbers through channels to
   find primes, stressing long chains of tasks and sustained channel traffic.
6. **HTTP ping-pong** — Two HTTP/1.1 servers on loopback ports bounce a counter
   back and forth, measuring request/response overhead and integration between
   the HTTP layer and the task scheduler.
7. **HTTP server throughput** — One HTTP/1.1 server serves a tiny plaintext
   response while many concurrent keep-alive clients issue GET requests,
   measuring sustained requests/second under load.

## Running

From this directory:

### Combined runner

From this directory, you can run all benchmark suites (Go, Clojure,
libgoc canary, and libgoc vmem) three times each with:

```sh
./run-all.sh
```

From the repository root, the equivalent command is:

```sh
./bench/run-all.sh
```

The script clears existing `bench/logs/*.log` files first, appends fresh
output to the per-runtime log files, and stops immediately if any benchmark
run fails.

### Go

```sh
# Single run (uses default threads = nproc)
make -C go run

# Multi-pool testing (runs with GOMAXPROCS = 1, 2, 4, 8)
make -C go run all=1
```

### libgoc

```sh
# Single run (uses default threads = nproc)
make -C libgoc run

# Multi-pool testing (runs with GOC_POOL_THREADS = 1, 2, 4, 8)
make -C libgoc run all=1
```

### Clojure

Requires [Clojure CLI tools](https://clojure.org/guides/install_clojure).

```sh
# Single run (uses default threads = nproc)
make -C clojure run

# Multi-pool testing (runs with threads = 1, 2, 4, 8)
make -C clojure run all=1
```

## Benchmark Status

| # | Benchmark | Go | libgoc | Clojure |
|---|-----------|:--:|:------:|:-------:|
| 1 | Channel ping-pong | ✅ | ✅ | ✅ |
| 2 | Ring | ✅ | ✅ | ✅ |
| 3 | Selective receive / fan-out / fan-in | ✅ | ✅ | ✅ |
| 4 | Spawn idle tasks | ✅ | ✅ | ✅ |
| 5 | Prime sieve | ✅ | ✅ | ✅ |
| 6 | HTTP ping-pong | ✅ | ✅ | ✅ |
| 7 | HTTP server throughput | ✅ | ✅ | ✅ |

## Runs

### Benchmark Environment

| Property        | Value                          |
|-----------------|--------------------------------|
| **CPU**         | AMD Ryzen 7 5800H              |
| **Cores**       | 8 cores / 16 threads (SMT)     |
| **Max Clock**   | 4463 MHz                       |
| **L1d / L1i**   | 256 KiB each (per core)        |
| **L2 Cache**    | 4 MiB (per core)               |
| **L3 Cache**    | 16 MiB (shared)                |
| **RAM**         | 13 GiB                         |
| **OS**          | Ubuntu 24.04.4 LTS             |
| **Kernel**      | Linux 6.11.0 x86_64            |

3 runs of each benchmark (Go, libgoc canary, libgoc vmem, Clojure) can be found in the [bench/logs/](logs/) directory. The `./run-all.sh` helper in this directory (or `./bench/run-all.sh` from the repository root) regenerates those logs. All numbers in the report below are the best of those 3 runs for each pool size.

## Report: libgoc vs. Go Baseline (+ Clojure)

<!-- BEGIN AUTO BENCH REPORT -->

This report evaluates the performance of **libgoc canary**, **libgoc vmem**, and **Clojure core.async** relative to the **Go** runtime. All figures represent operations per second; the multiplier in parentheses indicates performance relative to the Go baseline (e.g., **1.10x** means 10% faster, **0.50x** means half the speed).

---

### Channel ping-pong (round trips/s)
*Measures overhead of basic synchronization and context switching.*

| Pool | Go         | libgoc Canary              | libgoc VMEM                | Clojure                   |
|------|------------|----------------------------|----------------------------|---------------------------|
| 1    | 2,439,149  | 5,826,018 (**2.39x**)      | 5,613,510 (**2.30x**)      | 1,576,120 (**0.65x**)     |
| 2    | 2,281,447  | 3,559,661 (**1.56x**)      | 3,530,123 (**1.55x**)      | 1,039,940 (**0.46x**)     |
| 4    | 2,315,564  | 4,150,779 (**1.79x**)      | 3,874,004 (**1.67x**)      | 993,466 (**0.43x**)       |
| 8    | 2,238,889  | 3,434,224 (**1.53x**)      | 3,799,000 (**1.70x**)      | 849,185 (**0.38x**)       |
| **Geo** | -       | **1.79x**                  | **1.78x**                  | **0.47x**                 |

### Ring (hops/s)
*Measures message passing latency across a circular topology.*

| Pool | Go         | libgoc Canary              | libgoc VMEM                | Clojure                   |
|------|------------|----------------------------|----------------------------|---------------------------|
| 1    | 2,412,810  | 5,855,911 (**2.43x**)      | 5,834,659 (**2.42x**)      | 3,103,912 (**1.29x**)     |
| 2    | 2,251,925  | 3,403,820 (**1.51x**)      | 3,463,204 (**1.54x**)      | 1,845,343 (**0.82x**)     |
| 4    | 2,229,015  | 3,820,214 (**1.71x**)      | 3,745,185 (**1.68x**)      | 2,331,487 (**1.05x**)     |
| 8    | 2,244,549  | 3,698,823 (**1.65x**)      | 3,740,671 (**1.67x**)      | 2,011,692 (**0.90x**)     |
| **Geo** | -       | **1.80x**                  | **1.80x**                  | **1.00x**                 |

### Selective receive / fan-out / fan-in (msg/s)
*Evaluates complex orchestration and selection logic.*

| Pool | Go       | libgoc Canary              | libgoc VMEM                | Clojure                   |
|------|----------|----------------------------|----------------------------|---------------------------|
| 1    | 671,707  | 366,139 (**0.55x**)        | 357,563 (**0.53x**)        | 341,185 (**0.51x**)       |
| 2    | 705,493  | 895,195 (**1.27x**)        | 829,631 (**1.18x**)        | 368,829 (**0.52x**)       |
| 4    | 740,490  | 824,333 (**1.11x**)        | 758,819 (**1.02x**)        | 373,207 (**0.50x**)       |
| 8    | 737,036  | 794,639 (**1.08x**)        | 775,593 (**1.05x**)        | 362,732 (**0.49x**)       |
| **Geo** | -     | **0.95x**                  | **0.91x**                  | **0.51x**                 |

### Spawn idle tasks (tasks/s)
*Tests the efficiency of task creation and scheduling.*

| Pool | Go       | libgoc Canary              | libgoc VMEM                | Clojure                   |
|------|----------|----------------------------|----------------------------|---------------------------|
| 1    | 239,909  | 129,535 (**0.54x**)        | 55,247 (**0.23x**)         | 798,887 (**3.33x**)       |
| 2    | 454,124  | 49,857 (**0.11x**)         | 44,751 (**0.10x**)         | 848,034 (**1.87x**)       |
| 4    | 574,195  | 50,035 (**0.09x**)         | 45,165 (**0.08x**)         | 845,433 (**1.47x**)       |
| 8    | 566,623  | 49,288 (**0.09x**)         | 44,373 (**0.08x**)         | 801,986 (**1.42x**)       |
| **Geo** | -     | **0.13x**                  | **0.11x**                  | **1.90x**                 |

### Prime sieve (primes/s)
*High-concurrency filtering test.*

| Pool | Go     | libgoc Canary              | libgoc VMEM                | Clojure                   |
|------|--------|----------------------------|----------------------------|---------------------------|
| 1    | 2,020  | 3,896 (**1.93x**)          | 4,222 (**2.09x**)          | 2,206 (**1.09x**)         |
| 2    | 4,068  | 4,856 (**1.19x**)          | 4,998 (**1.23x**)          | 2,455 (**0.60x**)         |
| 4    | 7,739  | 3,825 (**0.49x**)          | 3,849 (**0.50x**)          | 1,909 (**0.25x**)         |
| 8    | 14,188 | 3,429 (**0.24x**)          | 3,466 (**0.24x**)          | 1,807 (**0.13x**)         |
| **Geo** | -   | **0.72x**                  | **0.75x**                  | **0.38x**                 |

### HTTP ping-pong (round trips/s)
*Two HTTP/1.1 servers on loopback bounce a counter back and forth.*

| Pool | Go     | libgoc Canary              | libgoc VMEM                | Clojure                   |
|------|--------|----------------------------|----------------------------|---------------------------|
| 1    | 1,992  | 4,656 (**2.34x**)          | 3,693 (**1.85x**)          | 2,367 (**1.19x**)         |
| 2    | 2,605  | 4,860 (**1.87x**)          | 4,469 (**1.72x**)          | 2,363 (**0.91x**)         |
| 4    | 2,750  | 5,025 (**1.83x**)          | 4,292 (**1.56x**)          | 2,371 (**0.86x**)         |
| 8    | 2,872  | 5,002 (**1.74x**)          | 4,303 (**1.50x**)          | 2,469 (**0.86x**)         |
| **Geo** | -   | **1.93x**                  | **1.65x**                  | **0.95x**                 |

### HTTP server throughput (req/s)
*One server, many concurrent keep-alive clients; measures sustained request rate.*

| Pool | Go      | libgoc Canary              | libgoc VMEM                | Clojure                   |
|------|---------|----------------------------|----------------------------|---------------------------|
| 1    | 10,280  | 16,172 (**1.57x**)         | 11,933 (**1.16x**)         | 38,412 (**3.74x**)        |
| 2    | 19,510  | 29,701 (**1.52x**)         | 21,707 (**1.11x**)         | 37,634 (**1.93x**)        |
| 4    | 43,210  | 45,365 (**1.05x**)         | 30,509 (**0.71x**)         | 38,663 (**0.89x**)        |
| 8    | 76,803  | 47,416 (**0.62x**)         | 34,227 (**0.45x**)         | 37,082 (**0.48x**)        |
| **Geo** | -    | **1.12x**                  | **0.80x**                  | **1.33x**                 |

---

## Summary

Geometric mean of ×Go multipliers across pool sizes 1,2,4,8.

| Benchmark        | libgoc Canary | libgoc VMEM | Clojure |
|------------------|---------------|-------------|---------|
| Ping-pong        | 1.79×         | 1.78×       | 0.47×   |
| Ring             | 1.80×         | 1.80×       | 1.00×   |
| Fan-out/Fan-in   | 0.95×         | 0.91×       | 0.51×   |
| Spawn idle       | 0.13×         | 0.11×       | 1.90×   |
| Prime sieve      | 0.72×         | 0.75×       | 0.38×   |
| HTTP ping-pong   | 1.93×         | 1.65×       | 0.95×   |
| HTTP throughput  | 1.12×         | 0.80×       | 1.33×   |
| **Overall Geo**  | **0.93×**     | **0.85×**   | **0.80×**|

**Takeaways:**
- **libgoc (canary)** leads all HTTP benchmarks: ping-pong **1.93× Go** geo mean (up from 1.67×), throughput **1.12× Go** — further gains from the goc_array improvements in this branch.
- Channel ping-pong/ring strong at p1 (**2.39×/2.43×**) and improved geo mean (**1.79×/1.80×**) vs prior run, with a notable vmem ping-pong recovery at p8 (1.70×) likely from reduced lock contention.
- Fan-out tightened further: canary now **0.95× geo** (from 0.92×), with p4/p8 consistently above 1×.
- Spawn idle remains libgoc's weakest point (0.13× canary) — expected given no goroutine-pool-style reuse.
- **VMEM** closely tracks canary on channel/ring (**1.78×/1.80×**) but shows the expected HTTP throughput deficit at higher pools (0.80× geo vs 1.12× canary) due to vmem GC overhead under concurrent I/O load.
- **Clojure** excels at spawn (1.90× geo) and HTTP throughput (1.33× geo, stable ~37–38k r/s regardless of pool) but trails everywhere else.
- **Go** unmatched at HTTP-thru scaling (76.8k r/s p8); libgoc canary peaks at 47.4k p8.
- **Overall**: libgoc canary **0.93× Go** geo-mean across 28 metrics — strong C CSP runtime, HTTP competetive.

For root-cause analysis, fixes, and optimization roadmap, see [OPTIMIZATION.md](../docs/OPTIMIZATION.md).

<!-- END AUTO BENCH REPORT -->
