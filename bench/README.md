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
| **1** | 2,457,209 | 1,808,355 (**0.74x**) | 572,271 (**0.23x**) | 1,371,802 (**0.56x**) |
| **2** | 2,286,765 | 1,541,661 (**0.67x**) | 712,687 (**0.31x**) | 888,511 (**0.39x**) |
| **4** | 2,231,281 | 1,814,131 (**0.81x**) | 701,266 (**0.31x**) | 750,468 (**0.34x**) |
| **8** | 2,238,000 | 2,187,743 (**0.98x**) | 870,422 (**0.39x**) | 870,422 (**0.39x**) |

### Ring (hops/s)
*Measures message passing latency across a circular topology.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 2,393,527 | 724,197 (**0.30x**) | 34,289 (**0.01x**) | 2,986,310 (**1.25x**) |
| **2** | 2,240,982 | 671,157 (**0.30x**) | 33,841 (**0.02x**) | 1,814,003 (**0.81x**) |
| **4** | 2,236,216 | 952,478 (**0.43x**) | 49,519 (**0.02x**) | 2,040,718 (**0.91x**) |
| **8** | 2,216,189 | 961,118 (**0.43x**) | 48,386 (**0.02x**) | 1,436,079 (**0.65x**) |

### Selective receive / fan-out / fan-in (msg/s)
*Evaluates complex orchestration and selection logic.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 676,714 | 212,603 (**0.31x**) | 223,556 (**0.33x**) | 313,850 (**0.46x**) |
| **2** | 626,372 | 189,179 (**0.30x**) | 232,895 (**0.37x**) | 354,587 (**0.57x**) |
| **4** | 636,947 | 181,153 (**0.28x**) | 222,772 (**0.35x**) | 336,116 (**0.53x**) |
| **8** | 673,835 | 192,791 (**0.29x**) | 225,680 (**0.33x**) | 251,957 (**0.37x**) |

### Spawn idle tasks (tasks/s)
*Tests the efficiency of task creation and scheduling.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 231,601 | 132,532 (**0.57x**) | 48,068 (**0.21x**) | 805,454 (**3.48x**) |
| **2** | 442,301 | 68,255 (**0.15x**) | 56,037 (**0.13x**) | 804,495 (**1.82x**) |
| **4** | 560,554 | 69,561 (**0.12x**) | 56,317 (**0.10x**) | 729,972 (**1.30x**) |
| **8** | 568,271 | 69,319 (**0.12x**) | 55,690 (**0.10x**) | 620,894 (**1.09x**) |

### Prime sieve (primes/s)
*High-concurrency filtering test.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 1,998 | 2,846 (**1.43x**) | 736 (**0.37x**) | 2,038 (**1.02x**) |
| **2** | 4,027 | 2,413 (**0.60x**) | 795 (**0.20x**) | 2,389 (**0.59x**) |
| **4** | 7,698 | 2,579 (**0.34x**) | 799 (**0.10x**) | 1,870 (**0.24x**) |
| **8** | 13,924 | 2,684 (**0.19x**) | 911 (**0.07x**) | 1,282 (**0.09x**) |

### HTTP ping-pong (round trips/s)
*Two HTTP/1.1 servers on loopback bounce a counter back and forth.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 1,949 | 3,184 (**1.63x**) | 2,806 (**1.44x**) | 1,770 (**0.91x**) |
| **2** | 2,477 | 2,876 (**1.16x**) | 2,938 (**1.19x**) | 1,739 (**0.70x**) |
| **4** | 2,569 | 2,472 (**0.96x**) | 2,380 (**0.93x**) | 1,707 (**0.66x**) |
| **8** | 2,412 | 1,700 (**0.70x**) | 2,348 (**0.97x**) | 1,756 (**0.73x**) |

### HTTP server throughput (req/s)
*One server, many concurrent keep-alive clients; measures sustained request rate.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 10,012 | 7,772 (**0.78x**) | 8,298 (**0.83x**) | 31,661 (**3.16x**) |
| **2** | 19,453 | 7,551 (**0.39x**) | 8,087 (**0.42x**) | 30,153 (**1.55x**) |
| **4** | 43,033 | 7,617 (**0.18x**) | 7,837 (**0.18x**) | 31,630 (**0.73x**) |
| **8** | 76,548 | 7,185 (**0.09x**) | 7,229 (**0.09x**) | 31,690 (**0.41x**) |

---

## Summary

Geometric mean of the ×Go multipliers across pool sizes 1, 2, 4, 8.

| Benchmark | libgoc (canary) | libgoc (vmem) | Clojure |
| :--- | :---: | :---: | :---: |
| Ping-pong | 0.79× | 0.31× | 0.42× |
| Ring | 0.36× | 0.02× | 0.90× |
| Fan-out/Fan-in | 0.31× | 0.34× | 0.48× |
| Spawn idle | 0.22× | 0.13× | 1.60× |
| Prime sieve | 0.44× | 0.17× | 0.23× |
| HTTP ping-pong | 1.11× | 1.13× | 0.75× |
| HTTP throughput | 0.32× | 0.33× | 1.01× |

**Takeaways:**
- **libgoc (canary)** shows a notable improvement in channel ping-pong (now up to 0.98× Go at pool=8) and HTTP ping-pong (1.63× Go at pool=1), but still trails Go in ring and fan-out/fan-in benchmarks, especially at higher pool sizes.
- **libgoc (vmem)** remains bottlenecked in ring and spawn idle tasks (multipliers ~0.01–0.13×), but achieves parity with Go in HTTP ping-pong and throughput at low pool sizes.
- **Clojure** excels in spawn idle tasks and HTTP throughput at low pool sizes, but its multipliers drop slightly in other areas, and it does not scale as well as Go or libgoc in most benchmarks.
- **Go** scales very well, especially in HTTP server throughput, where it continues to scale linearly with pool size, reaching 76k req/s at pool=8, while all other systems plateau.
- **Prime sieve**: libgoc (canary) leads at pool=1 (1.43× Go), but Go quickly overtakes as pool size increases.
- **Overall**: The new results confirm that libgoc’s improvements are real but incremental.

For root-cause analysis of these results, proposed fixes, and the prioritized optimization roadmap, see [OPTIMIZATION.md](../OPTIMIZATION.md).

<!-- END AUTO BENCH REPORT -->
