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

## Running

From this directory:

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

### Go (`make -C go run-all`)

```
=== Pool Size: 1 ===
GOMAXPROCS=1
Channel ping-pong: 200000 round trips in 81ms (2450068 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 208ms (2396744 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 301ms (663626 msg/s)
Spawn idle tasks: 200000 goroutines in 1238ms (161433 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1249ms (1810 primes/s)

=== Pool Size: 2 ===
GOMAXPROCS=2
Channel ping-pong: 200000 round trips in 87ms (2292035 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 217ms (2298805 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 280ms (712778 msg/s)
Spawn idle tasks: 200000 goroutines in 475ms (420510 tasks/s)
Prime sieve: 2262 primes up to 20000 in 549ms (4114 primes/s)

=== Pool Size: 4 ===
GOMAXPROCS=4
Channel ping-pong: 200000 round trips in 87ms (2277465 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 222ms (2250022 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 269ms (741545 msg/s)
Spawn idle tasks: 200000 goroutines in 358ms (557693 tasks/s)
Prime sieve: 2262 primes up to 20000 in 293ms (7694 primes/s)

=== Pool Size: 8 ===
GOMAXPROCS=8
Channel ping-pong: 200000 round trips in 87ms (2277539 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 221ms (2254470 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 267ms (747653 msg/s)
Spawn idle tasks: 200000 goroutines in 345ms (579313 tasks/s)
Prime sieve: 2262 primes up to 20000 in 159ms (14206 primes/s)
```

### libgoc canary — (default) — (`make -C libgoc run-all`)

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 128ms (1558221 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 653ms (764691 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 759ms (263350 msg/s)
Spawn idle tasks: 200000 fibers in 1518ms (131733 tasks/s)
Prime sieve: 2262 primes up to 20000 in 830ms (2722 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 131ms (1523077 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 649ms (770295 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 967ms (206796 msg/s)
Spawn idle tasks: 200000 fibers in 1286ms (155421 tasks/s)
Prime sieve: 2262 primes up to 20000 in 887ms (2549 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 116ms (1715892 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 680ms (734346 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 862ms (231845 msg/s)
Spawn idle tasks: 200000 fibers in 1309ms (152715 tasks/s)
Prime sieve: 2262 primes up to 20000 in 883ms (2560 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 98ms (2021993 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 497ms (1004947 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 867ms (230415 msg/s)
Spawn idle tasks: 200000 fibers in 1322ms (151212 tasks/s)
Prime sieve: 2262 primes up to 20000 in 893ms (2531 primes/s)
```

### libgoc vmem (`make -C libgoc LIBGOC_VMEM=ON BUILD_DIR=../../build-bench-vmem build run-all`)

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 356ms (560870 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 14388ms (34750 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 881ms (226977 msg/s)
Spawn idle tasks: 200000 fibers in 3189ms (62697 tasks/s)
Prime sieve: 2262 primes up to 20000 in 3553ms (637 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 442ms (452244 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 12795ms (39076 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1009ms (198157 msg/s)
Spawn idle tasks: 200000 fibers in 3227ms (61968 tasks/s)
Prime sieve: 2262 primes up to 20000 in 3219ms (703 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 301ms (663829 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 14892ms (33574 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 735ms (271943 msg/s)
Spawn idle tasks: 200000 fibers in 3112ms (64251 tasks/s)
Prime sieve: 2262 primes up to 20000 in 3691ms (613 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 221ms (903838 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 10420ms (47982 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 839ms (238363 msg/s)
Spawn idle tasks: 200000 fibers in 10663ms (18756 tasks/s)
Prime sieve: 2262 primes up to 20000 in 2447ms (924 primes/s)
```

### Clojure core.async (`make -C clojure run-all`)

```
=== Pool Size: 1 ===
CLOJURE_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 143ms (1393573 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 163ms (3060197 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 596ms (335299 msg/s)
Spawn idle tasks: 200000 go-blocks in 245ms (816057 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1043ms (2167 primes/s)

=== Pool Size: 2 ===
CLOJURE_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 220ms (908386 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 253ms (1974783 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 537ms (372276 msg/s)
Spawn idle tasks: 200000 go-blocks in 216ms (923576 tasks/s)
Prime sieve: 2262 primes up to 20000 in 973ms (2324 primes/s)

=== Pool Size: 4 ===
CLOJURE_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 248ms (803577 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 239ms (2090092 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 542ms (368716 msg/s)
Spawn idle tasks: 200000 go-blocks in 226ms (882022 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1303ms (1735 primes/s)

=== Pool Size: 8 ===
CLOJURE_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 262ms (761833 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 228ms (2189096 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 551ms (362718 msg/s)
Spawn idle tasks: 200000 go-blocks in 239ms (835404 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1417ms (1596 primes/s)
```

## Report: libgoc vs. Go Baseline (+ Clojure)

This report evaluates the performance of **libgoc canary**, **libgoc vmem**, and **Clojure core.async** relative to the **Go** runtime. All figures represent operations per second; the multiplier in parentheses indicates performance relative to the Go baseline (e.g., **1.10x** means 10% faster, **0.50x** means half the speed).

---

### Channel ping-pong (round trips/s)
*Measures overhead of basic synchronization and context switching.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 2,450,068 | 1,558,221 **(0.64x)** | 560,870 **(0.23x)** | 1,393,573 **(0.57x)** |
| **2** | 2,292,035 | 1,523,077 **(0.66x)** | 452,244 **(0.20x)** | 908,386 **(0.40x)** |
| **4** | 2,277,465 | 1,715,892 **(0.75x)** | 663,829 **(0.29x)** | 803,577 **(0.35x)** |
| **8** | 2,277,539 | 2,021,993 **(0.89x)** | 903,838 **(0.40x)** | 761,833 **(0.33x)** |

### Ring (hops/s)
*Measures message passing latency across a circular topology.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 2,396,744 | 764,691 **(0.32x)** | 34,750 **(0.01x)** | 3,060,197 **(1.28x)** |
| **2** | 2,298,805 | 770,295 **(0.34x)** | 39,076 **(0.02x)** | 1,974,783 **(0.86x)** |
| **4** | 2,250,022 | 734,346 **(0.33x)** | 33,574 **(0.01x)** | 2,090,092 **(0.93x)** |
| **8** | 2,254,470 | 1,004,947 **(0.45x)** | 47,982 **(0.02x)** | 2,189,096 **(0.97x)** |

### Selective receive / fan-out / fan-in (msg/s)
*Evaluates complex orchestration and selection logic.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 663,626 | 263,350 **(0.40x)** | 226,977 **(0.34x)** | 335,299 **(0.51x)** |
| **2** | 712,778 | 206,796 **(0.29x)** | 198,157 **(0.28x)** | 372,276 **(0.52x)** |
| **4** | 741,545 | 231,845 **(0.31x)** | 271,943 **(0.37x)** | 368,716 **(0.50x)** |
| **8** | 747,653 | 230,415 **(0.31x)** | 238,363 **(0.32x)** | 362,718 **(0.49x)** |

### Spawn idle tasks (tasks/s)
*Tests the efficiency of task creation and scheduling.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 161,433 | 131,733 **(0.82x)** | 62,697 **(0.39x)** | 816,057 **(5.06x)** |
| **2** | 420,510 | 155,421 **(0.37x)** | 61,968 **(0.15x)** | 923,576 **(2.20x)** |
| **4** | 557,693 | 152,715 **(0.27x)** | 64,251 **(0.12x)** | 882,022 **(1.58x)** |
| **8** | 579,313 | 151,212 **(0.26x)** | 18,756 **(0.03x)** | 835,404 **(1.44x)** |

### Prime sieve (primes/s)
*High-concurrency filtering test.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 1,810 | 2,722 **(1.50x)** | 637 **(0.35x)** | 2,167 **(1.20x)** |
| **2** | 4,114 | 2,549 **(0.62x)** | 703 **(0.17x)** | 2,324 **(0.56x)** |
| **4** | 7,694 | 2,560 **(0.33x)** | 613 **(0.08x)** | 1,735 **(0.23x)** |
| **8** | 14,206 | 2,531 **(0.18x)** | 924 **(0.07x)** | 1,596 **(0.11x)** |

---

## Summary

Geometric mean of the ×Go multipliers across pool sizes 1, 2, 4, 8.

| Benchmark | libgoc (canary) | libgoc (vmem) | Clojure |
| :--- | :---: | :---: | :---: |
| Ping-pong | 0.73× | 0.28× | 0.41× |
| Ring | 0.36× | 0.01× | 1.01× |
| Fan-out/Fan-in | 0.33× | 0.32× | 0.50× |
| Spawn idle | 0.32× | 0.13× | 2.18× |
| Prime sieve | 0.41× | 0.13× | 0.36× |

**Takeaways:**
- libgoc canary's ping-pong performance now reaches 0.89× Go at pool=8, with a geometric mean of 0.73×—a clear improvement, though still trailing Go at lower pool sizes.
- Ring throughput for canary is up to 1M hops/s at pool=8 (0.45× Go), with a geometric mean of 0.36×. vmem remains extremely slow on this test (0.01×).
- Fan-out/fan-in for both canary and vmem is stable (0.33× and 0.32×), with Clojure at 0.50×. No major regressions, but Go remains well ahead.
- Spawn idle tasks: canary is at 0.32× Go, vmem at 0.13×. Clojure's advantages come from its continuation-passing, heap-allocated go-block model, which avoids stack materialisation entirely.
- Prime sieve: canary and Clojure are close (0.41× and 0.36×), vmem lags (0.13×). Notably, canary outperforms Go at pool=1 on this test (1.50×).
- Overall, canary shows improved scaling and stability, vmem is only competitive on selective receive, and Clojure continues to excel at task creation and ring topologies.
