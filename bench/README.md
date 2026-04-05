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
| **1** | 2,447,909 | 1,646,256 **(0.67x)** | 838,105 **(0.34x)** | 1,369,179 **(0.56x)** |
| **2** | 2,310,314 | 1,594,188 **(0.69x)** | 857,950 **(0.37x)** | 892,946 **(0.39x)** |
| **4** | 2,239,694 | 1,838,472 **(0.82x)** | 897,918 **(0.40x)** | 897,918 **(0.40x)** |
| **8** | 2,250,091 | 2,100,004 **(0.93x)** | 857,950 **(0.38x)** | 857,950 **(0.38x)** |

### Ring (hops/s)
*Measures message passing latency across a circular topology.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 2,395,948 | 750,376 **(0.31x)** | 34,568 **(0.01x)** | 2,905,882 **(1.21x)** |
| **2** | 2,300,722 | 750,376 **(0.33x)** | 34,994 **(0.02x)** | 1,953,867 **(0.85x)** |
| **4** | 2,254,373 | 1,030,803 **(0.46x)** | 50,348 **(0.02x)** | 2,013,532 **(0.89x)** |
| **8** | 2,252,683 | 968,683 **(0.43x)** | 47,920 **(0.02x)** | 1,702,872 **(0.76x)** |

### Selective receive / fan-out / fan-in (msg/s)
*Evaluates complex orchestration and selection logic.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 743,281 | 215,656 **(0.29x)** | 231,598 **(0.31x)** | 321,857 **(0.43x)** |
| **2** | 710,355 | 215,656 **(0.30x)** | 231,003 **(0.33x)** | 355,976 **(0.50x)** |
| **4** | 721,281 | 199,327 **(0.28x)** | 245,966 **(0.34x)** | 347,696 **(0.48x)** |
| **8** | 743,281 | 215,656 **(0.29x)** | 245,966 **(0.33x)** | 258,087 **(0.35x)** |

### Spawn idle tasks (tasks/s)
*Tests the efficiency of task creation and scheduling.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 236,666 | 156,132 **(0.66x)** | 54,381 **(0.23x)** | 771,490 **(3.26x)** |
| **2** | 437,059 | 69,953 **(0.16x)** | 54,381 **(0.12x)** | 876,916 **(2.01x)** |
| **4** | 569,010 | 73,290 **(0.13x)** | 73,290 **(0.13x)** | 792,051 **(1.39x)** |
| **8** | 569,010 | 69,681 **(0.12x)** | 73,290 **(0.13x)** | 661,676 **(1.16x)** |

### Prime sieve (primes/s)
*High-concurrency filtering test.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 2,020 | 2,876 **(1.42x)** | 907 **(0.45x)** | 2,335 **(1.16x)** |
| **2** | 4,053 | 2,879 **(0.71x)** | 907 **(0.22x)** | 2,335 **(0.58x)** |
| **4** | 7,769 | 2,794 **(0.36x)** | 826 **(0.11x)** | 1,814 **(0.23x)** |
| **8** | 14,276 | 2,794 **(0.20x)** | 841 **(0.06x)** | 1,315 **(0.09x)** |

### HTTP ping-pong (round trips/s)
*Two HTTP/1.1 servers on loopback bounce a counter back and forth.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 1,978 | 3,327 **(1.68x)** | 2,730 **(1.38x)** | 1,823 **(0.92x)** |
| **2** | 2,743 | 2,871 **(1.05x)** | 2,751 **(1.00x)** | 1,727 **(0.63x)** |
| **4** | 2,458 | 2,328 **(0.95x)** | 2,291 **(0.93x)** | 1,773 **(0.72x)** |
| **8** | 2,837 | 1,748 **(0.62x)** | 1,884 **(0.66x)** | 1,884 **(0.66x)** |

### HTTP server throughput (req/s)
*One server, many concurrent keep-alive clients; measures sustained request rate.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 10,156 | 8,088 **(0.80x)** | 8,230 **(0.81x)** | 33,019 **(3.25x)** |
| **2** | 19,831 | 7,934 **(0.40x)** | 7,875 **(0.40x)** | 32,101 **(1.62x)** |
| **4** | 43,920 | 7,802 **(0.18x)** | 7,807 **(0.18x)** | 32,444 **(0.74x)** |
| **8** | 76,273 | 7,218 **(0.09x)** | 8,230 **(0.11x)** | 32,197 **(0.42x)** |

---

## Summary

Geometric mean of the ×Go multipliers across pool sizes 1, 2, 4, 8.

| Benchmark | libgoc (canary) | libgoc (vmem) | Clojure |
| :--- | :---: | :---: | :---: |
| Ping-pong | 0.77× | 0.37× | 0.43× |
| Ring | 0.38× | 0.02× | 0.91× |
| Fan-out/Fan-in | 0.29× | 0.33× | 0.44× |
| Spawn idle | 0.20× | 0.13× | 1.80× |
| Prime sieve | 0.52× | 0.16× | 0.34× |
| HTTP ping-pong | 1.01× | 0.96× | 0.72× |
| HTTP throughput | 0.27× | 0.28× | 1.13× |

**Takeaways:**
- libgoc canary ping-pong peaks at 0.93× Go at pool=8 with a geomean of 0.77×, a solid improvement over the previous snapshot. vmem climbs to 0.37× geomean, roughly doubling its prior result.
- Ring remains a weak point for canary (0.38× geomean), with best throughput at pool=4–8 (~0.43–0.46× Go). vmem stays bottlenecked at ~0.02× due to virtual-memory stack overhead. Clojure is no longer matching Go exactly, settling at 0.91× geomean in this run.
- Fan-out/fan-in: vmem (0.33×) and canary (0.29×) are close, with vmem slightly ahead. Both remain well below Clojure (0.44×) and Go. Clojure's advantage narrowed compared to prior runs.
- Spawn idle tasks remains the sharpest regression under contention: canary drops to 0.20× geomean, falling from 0.66× at pool=1 to 0.12% at pool=2+. vmem (0.13×) is similarly affected. Clojure retains a large lead (1.80×), though reduced from before as Go's baseline improved.
- Prime sieve: canary leads at pool=1 (1.42× Go) but trails at higher pool sizes (0.52× geomean). vmem holds at 0.16×. Clojure slips to 0.34× as Go's scaling advantage widens.
- HTTP ping-pong: canary (1.01×) and vmem (0.96×) are roughly at parity with Go, with canary edging ahead at pool=1 (1.68×).
- HTTP server throughput does not scale with pool size in libgoc or vmem (both plateau around 7–8 k req/s), while Go scales linearly to 76 k req/s at pool=8. Clojure (1.13× geomean) is ahead of libgoc at low pool counts but also plateaus, losing its advantage at pool=4+.

For root-cause analysis of these results, proposed fixes, and the prioritized optimization roadmap, see [OPTIMIZATION.md](../OPTIMIZATION.md).

<!-- END AUTO BENCH REPORT -->
