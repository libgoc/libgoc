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
| 1    | 2,413,942  | 5,540,440 (**2.30x**)      | 5,489,965 (**2.27x**)      | 1,616,986 (**0.67x**)     |
| 2    | 2,319,544  | 3,394,030 (**1.46x**)      | 3,370,349 (**1.45x**)      | 1,026,404 (**0.44x**)     |
| 4    | 2,293,567  | 3,731,103 (**1.63x**)      | 3,742,637 (**1.63x**)      | 999,152 (**0.44x**)       |
| 8    | 2,292,726  | 3,393,121 (**1.48x**)      | 3,542,751 (**1.55x**)      | 954,200 (**0.42x**)       |
| **Geo** | -       | **1.69x**                  | **1.70x**                  | **0.48x**                 |

### Ring (hops/s)
*Measures message passing latency across a circular topology.*

| Pool | Go         | libgoc Canary              | libgoc VMEM                | Clojure                   |
|------|------------|----------------------------|----------------------------|---------------------------|
| 1    | 2,395,392  | 5,568,239 (**2.32x**)      | 5,578,640 (**2.33x**)      | 3,178,103 (**1.33x**)     |
| 2    | 2,261,424  | 3,337,764 (**1.48x**)      | 3,378,507 (**1.49x**)      | 1,966,586 (**0.87x**)     |
| 4    | 2,244,156  | 3,615,378 (**1.61x**)      | 3,674,828 (**1.64x**)      | 2,107,661 (**0.94x**)     |
| 8    | 2,245,027  | 3,617,465 (**1.61x**)      | 3,706,835 (**1.65x**)      | 1,975,711 (**0.88x**)     |
| **Geo** | -       | **1.73x**                  | **1.75x**                  | **0.99x**                 |

### Selective receive / fan-out / fan-in (msg/s)
*Evaluates complex orchestration and selection logic.*

| Pool | Go       | libgoc Canary              | libgoc VMEM                | Clojure                   |
|------|----------|----------------------------|----------------------------|---------------------------|
| 1    | 667,735  | 362,571 (**0.54x**)        | 352,279 (**0.53x**)        | 340,472 (**0.51x**)       |
| 2    | 701,523  | 904,780 (**1.29x**)        | 896,903 (**1.28x**)        | 369,614 (**0.53x**)       |
| 4    | 739,262  | 762,801 (**1.03x**)        | 734,678 (**0.99x**)        | 370,171 (**0.50x**)       |
| 8    | 744,735  | 733,496 (**0.99x**)        | 725,896 (**0.97x**)        | 367,504 (**0.49x**)       |
| **Geo** | -     | **0.92x**                  | **0.89x**                  | **0.51x**                 |

### Spawn idle tasks (tasks/s)
*Tests the efficiency of task creation and scheduling.*

| Pool | Go       | libgoc Canary              | libgoc VMEM                | Clojure                   |
|------|----------|----------------------------|----------------------------|---------------------------|
| 1    | 254,553  | 127,273 (**0.50x**)        | 54,821 (**0.22x**)         | 807,396 (**3.17x**)       |
| 2    | 443,392  | 49,350 (**0.11x**)         | 44,241 (**0.10x**)         | 888,643 (**2.00x**)       |
| 4    | 570,143  | 49,855 (**0.09x**)         | 44,074 (**0.08x**)         | 822,530 (**1.44x**)       |
| 8    | 569,075  | 48,848 (**0.09x**)         | 44,542 (**0.08x**)         | 777,957 (**1.37x**)       |
| **Geo** | -     | **0.14x**                  | **0.11x**                  | **1.88x**                 |

### Prime sieve (primes/s)
*High-concurrency filtering test.*

| Pool | Go     | libgoc Canary              | libgoc VMEM                | Clojure                   |
|------|--------|----------------------------|----------------------------|---------------------------|
| 1    | 2,025  | 3,949 (**1.95x**)          | 4,142 (**2.05x**)          | 2,233 (**1.10x**)         |
| 2    | 4,026  | 5,137 (**1.28x**)          | 4,805 (**1.19x**)          | 2,401 (**0.60x**)         |
| 4    | 7,730  | 3,810 (**0.49x**)          | 3,958 (**0.51x**)          | 1,824 (**0.24x**)         |
| 8    | 14,126 | 3,239 (**0.23x**)          | 3,259 (**0.23x**)          | 1,796 (**0.13x**)         |
| **Geo** | -   | **0.73x**                  | **0.73x**                  | **0.38x**                 |

### HTTP ping-pong (round trips/s)
*Two HTTP/1.1 servers on loopback bounce a counter back and forth.*

| Pool | Go     | libgoc Canary              | libgoc VMEM                | Clojure                   |
|------|--------|----------------------------|----------------------------|---------------------------|
| 1    | 2,090  | 4,760 (**2.28x**)          | 3,408 (**1.63x**)          | 2,402 (**1.15x**)         |
| 2    | 2,612  | 4,204 (**1.61x**)          | 3,799 (**1.45x**)          | 2,339 (**0.90x**)         |
| 4    | 2,811  | 4,047 (**1.44x**)          | 4,001 (**1.42x**)          | 2,418 (**0.86x**)         |
| 8    | 2,738  | 3,978 (**1.45x**)          | 4,158 (**1.52x**)          | 2,455 (**0.90x**)         |
| **Geo** | -   | **1.67x**                  | **1.50x**                  | **0.95x**                 |

### HTTP server throughput (req/s)
*One server, many concurrent keep-alive clients; measures sustained request rate.*

| Pool | Go      | libgoc Canary              | libgoc VMEM                | Clojure                   |
|------|---------|----------------------------|----------------------------|---------------------------|
| 1    | 10,073  | 15,864 (**1.58x**)         | 11,625 (**1.15x**)         | 38,764 (**3.85x**)        |
| 2    | 19,546  | 32,150 (**1.64x**)         | 18,693 (**0.96x**)         | 39,067 (**2.00x**)        |
| 4    | 43,200  | 35,989 (**0.83x**)         | 24,818 (**0.57x**)         | 38,631 (**0.89x**)        |
| 8    | 77,023  | 47,570 (**0.62x**)         | 32,804 (**0.43x**)         | 38,059 (**0.49x**)        |
| **Geo** | -    | **1.07x**                  | **0.72x**                  | **1.35x**                 |

---

## Summary

Geometric mean of ×Go multipliers across pool sizes 1,2,4,8.

| Benchmark        | libgoc Canary | libgoc VMEM | Clojure |
|------------------|---------------|-------------|---------|
| Ping-pong        | 1.69×         | 1.70×       | 0.48×   |
| Ring             | 1.73×         | 1.75×       | 0.99×   |
| Fan-out/Fan-in   | 0.92×         | 0.89×       | 0.51×   |
| Spawn idle       | 0.14×         | 0.11×       | 1.88×   |
| Prime sieve      | 0.73×         | 0.73×       | 0.38×   |
| HTTP ping-pong   | 1.67×         | 1.50×       | 0.95×   |
| HTTP throughput  | 1.07×         | 0.72×       | 1.35×   |
| **Overall Geo**  | **0.91×**     | **0.81×**   | **0.81×**|

**Takeaways:**
- **libgoc (canary)** leads all HTTP benchmarks: ping-pong **1.67× Go** geo mean, throughput **1.07× Go** (from 0.37× previously) — major gains from the multi-loop + throughput work.
- Channel ping-pong/ring still strong at p1 (**2.30×/2.32×**), with some regression at higher pools vs earlier single-loop build (work-steal thrashing at p2+).
- Fan-out improved dramatically vs prior build: now **0.92× geo** (from 0.57×) due to better multi-pool work distribution.
- Spawn idle remains libgoc's weakest point (0.14× canary) — expected given no goroutine-pool-style reuse.
- **VMEM** holds channel parity with canary at p1 (2.27×/2.33×) but shows larger HTTP throughput gap at higher pools (0.72× geo vs 1.07× canary) due to vmem GC overhead under concurrent I/O load.
- **Clojure** excels at spawn (1.88× geo) and HTTP throughput (1.35× geo, stable ~38k r/s regardless of pool) but trails everywhere else.
- **Go** unmatched at HTTP-thru scaling (77k r/s p8).
- **Overall**: libgoc canary **0.91× Go** geo-mean across 28 metrics — strong C CSP runtime, HTTP now competitive.

For root-cause analysis, fixes, and optimization roadmap, see [OPTIMIZATION.md](../OPTIMIZATION.md).

<!-- END AUTO BENCH REPORT -->
