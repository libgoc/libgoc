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

> Note: the comparative tables below were generated before adding benchmarks
> #6 (HTTP ping-pong) and #7 (HTTP server throughput), so they currently cover
> the first five CSP benchmarks only.

This report evaluates the performance of **libgoc canary**, **libgoc vmem**, and **Clojure core.async** relative to the **Go** runtime. All figures represent operations per second; the multiplier in parentheses indicates performance relative to the Go baseline (e.g., **1.10x** means 10% faster, **0.50x** means half the speed).

---

### Channel ping-pong (round trips/s)
*Measures overhead of basic synchronization and context switching.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 2,462,723 | 1,600,108 **(0.65x)** | 558,292 **(0.23x)** | 1,475,056 **(0.60x)** |
| **2** | 2,299,174 | 1,616,540 **(0.70x)** | 555,948 **(0.24x)** | 917,285 **(0.40x)** |
| **4** | 2,316,612 | 1,825,994 **(0.79x)** | 693,443 **(0.30x)** | 927,094 **(0.40x)** |
| **8** | 2,305,221 | 2,121,074 **(0.92x)** | 859,841 **(0.37x)** | 863,775 **(0.37x)** |

### Ring (hops/s)
*Measures message passing latency across a circular topology.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 2,394,205 | 734,025 **(0.31x)** | 35,110 **(0.01x)** | 3,151,742 **(1.32x)** |
| **2** | 2,278,856 | 691,170 **(0.30x)** | 34,654 **(0.02x)** | 2,018,366 **(0.89x)** |
| **4** | 2,259,653 | 998,693 **(0.44x)** | 50,900 **(0.02x)** | 2,269,710 **(1.00x)** |
| **8** | 2,263,918 | 942,573 **(0.42x)** | 49,061 **(0.02x)** | 2,075,803 **(0.92x)** |

### Selective receive / fan-out / fan-in (msg/s)
*Evaluates complex orchestration and selection logic.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 640,172 | 201,047 **(0.31x)** | 241,784 **(0.38x)** | 344,863 **(0.54x)** |
| **2** | 707,669 | 192,201 **(0.27x)** | 246,297 **(0.35x)** | 376,259 **(0.53x)** |
| **4** | 755,743 | 198,760 **(0.26x)** | 240,128 **(0.32x)** | 378,694 **(0.50x)** |
| **8** | 764,424 | 188,333 **(0.25x)** | 242,987 **(0.32x)** | 369,592 **(0.48x)** |

### Spawn idle tasks (tasks/s)
*Tests the efficiency of task creation and scheduling.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 237,190 | 169,533 **(0.71x)** | 66,555 **(0.28x)** | 815,298 **(3.44x)** |
| **2** | 419,821 | 75,506 **(0.18x)** | 16,195 **(0.04x)** | 917,459 **(2.19x)** |
| **4** | 563,616 | 76,238 **(0.14x)** | 56,376 **(0.10x)** | 842,494 **(1.49x)** |
| **8** | 583,660 | 70,784 **(0.12x)** | 56,404 **(0.10x)** | 905,371 **(1.55x)** |

### Prime sieve (primes/s)
*High-concurrency filtering test.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 2,024 | 2,829 **(1.40x)** | 776 **(0.38x)** | 2,099 **(1.04x)** |
| **2** | 4,088 | 2,889 **(0.71x)** | 640 **(0.16x)** | 2,314 **(0.57x)** |
| **4** | 7,747 | 2,878 **(0.37x)** | 822 **(0.11x)** | 1,853 **(0.24x)** |
| **8** | 14,297 | 2,907 **(0.20x)** | 853 **(0.06x)** | 1,805 **(0.13x)** |

---

## Summary

Geometric mean of the ×Go multipliers across pool sizes 1, 2, 4, 8.

| Benchmark | libgoc (canary) | libgoc (vmem) | Clojure |
| :--- | :---: | :---: | :---: |
| Ping-pong | 0.76× | 0.28× | 0.43× |
| Ring | 0.36× | 0.02× | 1.02× |
| Fan-out/Fan-in | 0.27× | 0.34× | 0.51× |
| Spawn idle | 0.21× | 0.10× | 2.04× |
| Prime sieve | 0.52× | 0.14× | 0.36× |

**Takeaways:**
- libgoc canary ping-pong now peaks at 0.92× Go at pool=8, with a geometric mean of 0.76× (up from the previous snapshot), showing continued scaling progress.
- Ring remains a weak point for canary (0.36× geomean), with best throughput at pool=4 (998,693 hops/s, 0.44× Go). vmem stays bottlenecked at ~0.02× due to virtual-memory stack overhead.
- Fan-out/fan-in shifted in vmem’s favor in this run set: vmem reaches a 0.34× geomean versus canary at 0.27×, though both are still below Clojure (0.51×) and Go.
- Spawn idle tasks is still the largest canary regression under contention: 0.21× geomean, dropping sharply past pool=1. vmem remains volatile here (notably 0.04× at pool=2), while Clojure retains a large advantage (2.04×).
- Prime sieve: canary remains strongest at pool=1 (1.40× Go) but trails at higher pool sizes, landing at 0.52× geomean. vmem is at 0.14× and Clojure at 0.36×.
- Overall, canary shows clear gains in ping-pong and maintains competitive single-thread sieve behavior, but ring and especially spawn-idle scalability continue to dominate the optimization backlog.

<!-- END AUTO BENCH REPORT -->
