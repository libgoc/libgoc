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

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 2,444,611 | 1,578,329 **(0.65x)** | 565,448 **(0.23x)** | 1,574,662 **(0.64x)** |
| **2** | 2,297,554 | 1,547,300 **(0.67x)** | 548,104 **(0.24x)** | 1,026,913 **(0.45x)** |
| **4** | 2,255,150 | 1,708,094 **(0.76x)** | 679,304 **(0.30x)** | 1,071,377 **(0.48x)** |
| **8** | 2,324,713 | 1,961,411 **(0.84x)** | 861,469 **(0.37x)** | 965,557 **(0.42x)** |

### Ring (hops/s)
*Measures message passing latency across a circular topology.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 2,410,934 | 762,732 **(0.32x)** | 34,346 **(0.01x)** | 3,154,408 **(1.31x)** |
| **2** | 2,304,645 | 746,943 **(0.32x)** | 33,915 **(0.01x)** | 1,935,692 **(0.84x)** |
| **4** | 2,403,568 | 1,019,167 **(0.42x)** | 50,312 **(0.02x)** | 2,470,090 **(1.03x)** |
| **8** | 2,259,049 | 967,802 **(0.43x)** | 47,827 **(0.02x)** | 2,012,361 **(0.89x)** |

### Selective receive / fan-out / fan-in (msg/s)
*Evaluates complex orchestration and selection logic.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 675,606 | 221,407 **(0.33x)** | 250,848 **(0.37x)** | 334,225 **(0.49x)** |
| **2** | 694,154 | 196,567 **(0.28x)** | 233,751 **(0.34x)** | 372,238 **(0.54x)** |
| **4** | 746,758 | 214,383 **(0.29x)** | 225,000 **(0.30x)** | 379,341 **(0.51x)** |
| **8** | 747,718 | 197,473 **(0.26x)** | 252,645 **(0.34x)** | 365,949 **(0.49x)** |

### Spawn idle tasks (tasks/s)
*Tests the efficiency of task creation and scheduling.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 254,612 | 147,659 **(0.58x)** | 93,269 **(0.37x)** | 879,620 **(3.45x)** |
| **2** | 454,971 | 68,991 **(0.15x)** | 32,800 **(0.07x)** | 858,573 **(1.89x)** |
| **4** | 568,774 | 68,272 **(0.12x)** | 64,391 **(0.11x)** | 851,906 **(1.50x)** |
| **8** | 567,625 | 68,628 **(0.12x)** | 94,419 **(0.17x)** | 789,762 **(1.39x)** |

### Prime sieve (primes/s)
*High-concurrency filtering test.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 2,020 | 2,826 **(1.40x)** | 1,005 **(0.50x)** | 2,133 **(1.06x)** |
| **2** | 4,067 | 2,879 **(0.71x)** | 847 **(0.21x)** | 2,400 **(0.59x)** |
| **4** | 7,798 | 2,906 **(0.37x)** | 888 **(0.11x)** | 1,868 **(0.24x)** |
| **8** | 14,148 | 2,889 **(0.20x)** | 847 **(0.06x)** | 1,829 **(0.13x)** |

### HTTP ping-pong (round trips/s)
*Two HTTP/1.1 servers on loopback bounce a counter back and forth.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 2,073 | 3,287 **(1.59x)** | 3,022 **(1.46x)** | 2,354 **(1.14x)** |
| **2** | 2,309 | 3,029 **(1.31x)** | 2,938 **(1.27x)** | 2,379 **(1.03x)** |
| **4** | 2,818 | 2,479 **(0.88x)** | 2,395 **(0.85x)** | 2,436 **(0.86x)** |
| **8** | 2,721 | 1,850 **(0.68x)** | 2,278 **(0.84x)** | 2,415 **(0.89x)** |

### HTTP server throughput (req/s)
*One server, many concurrent keep-alive clients; measures sustained request rate.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 9,914 | 8,257 **(0.83x)** | 8,405 **(0.85x)** | 38,208 **(3.85x)** |
| **2** | 20,049 | 7,929 **(0.40x)** | 8,065 **(0.40x)** | 37,996 **(1.90x)** |
| **4** | 44,073 | 7,979 **(0.18x)** | 7,767 **(0.18x)** | 38,048 **(0.86x)** |
| **8** | 76,973 | 7,256 **(0.09x)** | 7,652 **(0.10x)** | 38,048 **(0.49x)** |

---

## Summary

Geometric mean of the ×Go multipliers across pool sizes 1, 2, 4, 8.

| Benchmark | libgoc (canary) | libgoc (vmem) | Clojure |
| :--- | :---: | :---: | :---: |
| Ping-pong | 0.72× | 0.28× | 0.49× |
| Ring | 0.37× | 0.02× | 1.00× |
| Fan-out/Fan-in | 0.29× | 0.34× | 0.51× |
| Spawn idle | 0.19× | 0.15× | 1.92× |
| Prime sieve | 0.52× | 0.16× | 0.37× |
| HTTP ping-pong | 1.06× | 1.07× | 0.97× |
| HTTP throughput | 0.27× | 0.28× | 1.33× |

**Takeaways:**
- libgoc canary ping-pong peaks at 0.84× Go at pool=8 with a geomean of 0.72×, showing continued scaling progress. vmem holds steady at 0.28×.
- Ring remains a weak point for canary (0.37× geomean), with best throughput at pool=4–8 (~0.42–0.43× Go). vmem stays bottlenecked at ~0.02× due to virtual-memory stack overhead. Clojure matches Go exactly (1.00× geomean).
- Fan-out/fan-in: vmem (0.34×) edges out canary (0.29×) here, though both remain well below Clojure (0.51×) and Go.
- Spawn idle tasks is still the sharpest regression under contention: canary drops to 0.19× geomean, falling from 0.58× at pool=1 to 0.12× at pool=2+. vmem (0.15×) suffers similarly. Clojure retains a large lead (1.92×).
- Prime sieve: canary leads at pool=1 (1.40× Go) but trails at higher pool sizes (0.52× geomean). vmem improved to 0.16× (up from the previous snapshot). Clojure holds at 0.37×.
- HTTP ping-pong is a bright spot: both canary (1.06×) and vmem (1.07×) outperform Go at pool sizes 1–2, suggesting the HTTP layer integrates efficiently with the task scheduler at low concurrency. Performance degrades at pool=8 under contention.
- HTTP server throughput does not scale with pool size in libgoc or vmem (both plateau around 7–8 k req/s), while Go scales linearly to 77 k req/s at pool=8. Clojure (1.33× geomean) benefits from its thread-pool model at low pool counts but also plateaus at higher sizes.

For root-cause analysis of these results, proposed fixes, and the prioritized optimization roadmap, see [OPTIMIZATION.md](../OPTIMIZATION.md).

<!-- END AUTO BENCH REPORT -->
