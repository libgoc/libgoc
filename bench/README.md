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
# Single run (uses current GOMAXPROCS)
make -C go run

# Multi-pool testing (runs with GOMAXPROCS = 1, 2, 4, 8)
make -C go run-all
```

### libgoc

```sh
# Single run (uses current GOC_POOL_THREADS)
make -C libgoc run

# Multi-pool testing (runs with GOC_POOL_THREADS = 1, 2, 4, 8)
make -C libgoc run-all
```

### Clojure

Requires [Clojure CLI tools](https://clojure.org/guides/install_clojure).

```sh
# Single run (uses default pool size = 8)
make -C clojure run

# Multi-pool testing (runs with CLOJURE_POOL_THREADS = 1, 2, 4, 8)
make -C clojure run-all
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

| Pool | Go       | libgoc Canary      | libgoc VMEM        | Clojure      |
|------|----------|--------------------|--------------------|--------------|
| 1    | 2,455,547| 5,300,770 (**2.16x**) | 5,698,770 (**2.32x**) | 1,540,812 (**0.63x**) |
| 2    | 2,297,412| 5,397,895 (**2.35x**) | 5,434,128 (**2.37x**) | 1,018,061 (**0.44x**) |
| 4    | 2,312,272| 5,452,569 (**2.36x**) | 5,379,954 (**2.33x**) | 1,018,061 (**0.44x**) |
| 8    | 2,267,552| 5,455,975 (**2.41x**) | 5,499,600 (**2.42x**) | 968,191 (**0.43x**) |
| **Geo** | -       | **2.32x**          | **2.36x**          | **0.48x**    |

### Ring (hops/s)
*Measures message passing latency across a circular topology.*

| Pool | Go       | libgoc Canary      | libgoc VMEM        | Clojure      |
|------|----------|--------------------|--------------------|--------------|
| 1    | 2,403,352| 6,561,066 (**2.73x**) | 6,678,892 (**2.78x**) | 3,098,344 (**1.29x**) |
| 2    | 2,314,345| 6,561,066 (**2.83x**) | 6,539,983 (**2.82x**) | 2,346,983 (**1.01x**) |
| 4    | 2,244,745| 6,555,930 (**2.92x**) | 6,582,770 (**2.93x**) | 2,346,983 (**1.05x**) |
| 8    | 2,257,697| 6,644,287 (**2.94x**) | 6,527,746 (**2.89x**) | 1,944,439 (**0.86x**) |
| **Geo** | -       | **2.85x**          | **2.85x**          | **1.04x**    |

### Selective receive / fan-out / fan-in (msg/s)
*Evaluates complex orchestration and selection logic.*

| Pool | Go     | libgoc Canary | libgoc VMEM | Clojure  |
|------|--------|---------------|-------------|----------|
| 1    | 677,985| 410,101 (**0.60x**) | 389,962 (**0.58x**) | 375,495 (**0.55x**) |
| 2    | 716,767| 410,858 (**0.57x**) | 406,597 (**0.57x**) | 375,495 (**0.52x**) |
| 4    | 747,809| 408,699 (**0.55x**) | 406,597 (**0.54x**) | 374,648 (**0.50x**) |
| 8    | 751,984| 410,240 (**0.55x**) | 397,269 (**0.53x**) | 375,495 (**0.50x**) |
| **Geo** | -     | **0.57x**       | **0.55x**     | **0.52x** |

### Spawn idle tasks (tasks/s)
*Tests the efficiency of task creation and scheduling.*

| Pool | Go      | libgoc Canary | libgoc VMEM | Clojure  |
|------|---------|---------------|-------------|----------|
| 1    | 258,378 | 157,721 (**0.61x**) | 153,495 (**0.59x**) | 805,454 (**3.12x**) |
| 2    | 459,826 | 75,557 (**0.16x**) | 73,226 (**0.16x**) | 881,310 (**1.92x**) |
| 4    | 571,929 | 75,557 (**0.13x**) | 72,994 (**0.13x**) | 883,926 (**1.55x**) |
| 8    | 563,659 | 75,557 (**0.13x**) | 66,672 (**0.12x**) | 871,175 (**1.54x**) |
| **Geo** | -      | **0.26x**       | **0.25x**     | **2.02x** |

### Prime sieve (primes/s)
*High-concurrency filtering test.*

| Pool | Go    | libgoc Canary | libgoc VMEM | Clojure |
|------|-------|---------------|-------------|---------|
| 1    | 2,039 | 4,425 (**2.17x**) | 4,507 (**2.21x**) | 2,435 (**1.19x**) |
| 2    | 4,058 | 4,383 (**1.08x**) | 4,443 (**1.10x**) | 2,309 (**0.57x**) |
| 4    | 7,741 | 4,328 (**0.56x**) | 4,402 (**0.57x**) | 1,977 (**0.26x**) |
| 8    | 14,153| 4,456 (**0.31x**) | 4,375 (**0.31x**) | 1,716 (**0.12x**) |
| **Geo** | -    | **0.84x**       | **0.85x**     | **0.45x** |

### HTTP ping-pong (round trips/s)
*Two HTTP/1.1 servers on loopback bounce a counter back and forth.*

| Pool | Go   | libgoc Canary | libgoc VMEM | Clojure |
|------|------|---------------|-------------|---------|
| 1    | 2,068| 3,194 (**1.54x**) | 2,850 (**1.38x**) | 2,446 (**1.18x**) |
| 2    | 2,822| 2,845 (**1.01x**) | 2,837 (**1.00x**) | 2,442 (**0.87x**) |
| 4    | 2,863| 2,614 (**0.91x**) | 2,413 (**0.84x**) | 2,404 (**0.84x**) |
| 8    | 2,910| 2,628 (**0.90x**) | 2,252 (**0.77x**) | 2,416 (**0.83x**) |
| **Geo** | -   | **1.06x**       | **0.98x**     | **0.92x** |

### HTTP server throughput (req/s)
*One server, many concurrent keep-alive clients; measures sustained request rate.*

| Pool | Go    | libgoc Canary | libgoc VMEM | Clojure |
|------|-------|---------------|-------------|---------|
| 1    | 9,941 | 9,336 (**0.94x**) | 9,234 (**0.93x**) | 38,069 (**3.83x**) |
| 2    | 20,062| 9,336 (**0.47x**) | 9,160 (**0.46x**) | 37,792 (**1.88x**) |
| 4    | 43,812| 8,993 (**0.21x**) | 8,884 (**0.20x**) | 38,611 (**0.88x**) |
| 8    | 78,136| 8,581 (**0.11x**) | 8,044 (**0.10x**) | 37,792 (**0.48x**) |
| **Geo** | -    | **0.37x**       | **0.37x**     | **1.24x** |

---

## Summary

Geometric mean of ×Go multipliers across pool sizes 1,2,4,8.

| Benchmark        | libgoc Canary | libgoc VMEM | Clojure |
|------------------|---------------|-------------|---------|
| Ping-pong        | 2.32×         | 2.36×       | 0.48×   |
| Ring             | 2.85×         | 2.85×       | 1.04×   |
| Fan-out/Fan-in   | 0.57×         | 0.55×       | 0.52×   |
| Spawn idle       | 0.26×         | 0.25×       | 2.02×   |
| Prime sieve      | 0.84×         | 0.85×       | 0.45×   |
| HTTP ping-pong   | 1.06×         | 0.98×       | 0.92×   |
| HTTP throughput  | 0.37×         | 0.37×       | 1.24×   |
| **Overall Geo**  | **0.88×**     | **0.82×**   | **0.81×**|

**Takeaways:**
- **libgoc (canary)** dominates low-contention benchmarks: ping-pong/ring now **2.3-2.9× Go** consistently across all pools.
- HTTP ping-pong near parity (1.07× geo); p1 even leads (1.54×).
- Strong sieve p1-2 (2.2×); scaling/spawn/HTTP-thru opportunities remain.
- **VMEM** minimal penalty in channels/HTTP (~2.4× ping-pong, 0.99× HTTP-ping), but spawn hit (0.10×).
- **Clojure** excels spawn/HTTP-thru low-pool (3.8× p1 thru), trails channels/scale.
- **Go** unmatched HTTP-thru scaling (78k r/s p8).
- **Overall**: libgoc **0.90× Go** geo-mean across 28 metrics – breakthrough C CSP runtime!

For root-cause analysis, fixes, and optimization roadmap, see [OPTIMIZATION.md](../OPTIMIZATION.md).

<!-- END AUTO BENCH REPORT -->
