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
Channel ping-pong: 200000 round trips in 82ms (2431608 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 208ms (2395887 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 335ms (595850 msg/s)
Spawn idle tasks: 200000 goroutines in 1292ms (154711 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1190ms (1900 primes/s)

=== Pool Size: 2 ===
GOMAXPROCS=2
Channel ping-pong: 200000 round trips in 91ms (2192214 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 217ms (2297843 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 287ms (696528 msg/s)
Spawn idle tasks: 200000 goroutines in 508ms (393206 tasks/s)
Prime sieve: 2262 primes up to 20000 in 558ms (4053 primes/s)

=== Pool Size: 4 ===
GOMAXPROCS=4
Channel ping-pong: 200000 round trips in 89ms (2240324 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 216ms (2305941 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 265ms (753517 msg/s)
Spawn idle tasks: 200000 goroutines in 452ms (441883 tasks/s)
Prime sieve: 2262 primes up to 20000 in 290ms (7787 primes/s)

=== Pool Size: 8 ===
GOMAXPROCS=8
Channel ping-pong: 200000 round trips in 87ms (2292833 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 217ms (2298159 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 265ms (752862 msg/s)
Spawn idle tasks: 200000 goroutines in 358ms (558500 tasks/s)
Prime sieve: 2262 primes up to 20000 in 155ms (14526 primes/s)
```

### libgoc canary — (default) — (`make -C libgoc run-all`)

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 114ms (1743937 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 667ms (748901 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 410ms (487486 msg/s)
Spawn idle tasks: 200000 fibers in 11060ms (18082 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1089ms (2077 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 113ms (1768442 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 689ms (725162 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 457ms (436805 msg/s)
Spawn idle tasks: 200000 fibers in 11308ms (17687 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1070ms (2113 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 97ms (2055682 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 692ms (721671 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 481ms (415434 msg/s)
Spawn idle tasks: 200000 fibers in 11047ms (18104 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1130ms (2002 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 105ms (1887001 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 540ms (924756 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 642ms (311521 msg/s)
Spawn idle tasks: 200000 fibers in 11024ms (18141 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1150ms (1965 primes/s)
```

### libgoc vmem (`make -C libgoc LIBGOC_VMEM=ON BUILD_DIR=../../build-bench-vmem build run-all`)

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 275ms (725450 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 11381ms (43933 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 892ms (224167 msg/s)
Spawn idle tasks: 200000 fibers in 13477ms (14840 tasks/s)
Prime sieve: 2262 primes up to 20000 in 6619ms (342 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 280ms (713797 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 12032ms (41554 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1050ms (190303 msg/s)
Spawn idle tasks: 200000 fibers in 13100ms (15267 tasks/s)
Prime sieve: 2262 primes up to 20000 in 7859ms (288 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 217ms (920515 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 12595ms (39696 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1304ms (153290 msg/s)
Spawn idle tasks: 200000 fibers in 13440ms (14880 tasks/s)
Prime sieve: 2262 primes up to 20000 in 7833ms (289 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 246ms (811155 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 9375ms (53329 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1778ms (112439 msg/s)
Spawn idle tasks: 200000 fibers in 13808ms (14484 tasks/s)
Prime sieve: 2262 primes up to 20000 in 7720ms (293 primes/s)
```

### Clojure core.async (`make -C clojure run-all`)

```
=== Pool Size: 1 ===
CLOJURE_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 153ms (1301978 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 191ms (2608232 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 652ms (306730 msg/s)
Spawn idle tasks: 200000 go-blocks in 301ms (663426 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1545ms (1463 primes/s)

=== Pool Size: 2 ===
CLOJURE_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 266ms (750500 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 265ms (1884720 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 572ms (349289 msg/s)
Spawn idle tasks: 200000 go-blocks in 290ms (689487 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1031ms (2192 primes/s)

=== Pool Size: 4 ===
CLOJURE_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 261ms (763754 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 253ms (1975028 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 583ms (342562 msg/s)
Spawn idle tasks: 200000 go-blocks in 268ms (745718 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1179ms (1917 primes/s)

=== Pool Size: 8 ===
CLOJURE_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 311ms (641775 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 272ms (1836152 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 610ms (327600 msg/s)
Spawn idle tasks: 200000 go-blocks in 300ms (664562 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1560ms (1449 primes/s)
```

## Report: libgoc vs. Go Baseline (+ Clojure)

This report evaluates the performance of **libgoc (post-optimization)**, **libgoc vmem**, and **Clojure core.async** relative to the **Go** runtime. All figures represent operations per second; the multiplier in parentheses indicates performance relative to the Go baseline (e.g., **1.10x** represents 10% faster, while **0.50x** represents half the speed).

---

### Channel ping-pong (round trips/s)
*Measures overhead of basic synchronization and context switching.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 2,431,608 | 1,743,937 **(0.72x)** | 725,450 **(0.30x)** | 1,301,978 **(0.54x)** |
| **2** | 2,192,214 | 1,768,442 **(0.81x)** | 713,797 **(0.33x)** | 750,500 **(0.34x)** |
| **4** | 2,240,324 | 2,055,682 **(0.92x)** | 920,515 **(0.41x)** | 763,754 **(0.34x)** |
| **8** | 2,292,833 | 1,887,001 **(0.82x)** | 811,155 **(0.35x)** | 641,775 **(0.28x)** |

### Ring (hops/s)
*Measures message passing latency across a circular topology.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 2,395,887 | 748,901 **(0.31x)** | 43,933 **(0.02x)** | 2,608,232 **(1.09x)** |
| **2** | 2,297,843 | 725,162 **(0.32x)** | 41,554 **(0.02x)** | 1,884,720 **(0.82x)** |
| **4** | 2,305,941 | 721,671 **(0.31x)** | 39,696 **(0.02x)** | 1,975,028 **(0.86x)** |
| **8** | 2,298,159 | 924,756 **(0.40x)** | 53,329 **(0.02x)** | 1,836,152 **(0.80x)** |

### Selective receive / fan-out / fan-in (msg/s)
*Evaluates complex orchestration and selection logic.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 595,850 | 487,486 **(0.82x)** | 224,167 **(0.38x)** | 306,730 **(0.51x)** |
| **2** | 696,528 | 436,805 **(0.63x)** | 190,303 **(0.27x)** | 349,289 **(0.50x)** |
| **4** | 753,517 | 415,434 **(0.55x)** | 153,290 **(0.20x)** | 342,562 **(0.45x)** |
| **8** | 752,862 | 311,521 **(0.41x)** | 112,439 **(0.15x)** | 327,600 **(0.44x)** |

### Spawn idle tasks (tasks/s)
*Tests the efficiency of task creation and scheduling.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 154,711 | 18,082 **(0.12x)** | 14,840 **(0.10x)** | 663,426 **(4.29x)** |
| **2** | 393,206 | 17,687 **(0.04x)** | 15,267 **(0.04x)** | 689,487 **(1.75x)** |
| **4** | 441,883 | 18,104 **(0.04x)** | 14,880 **(0.03x)** | 745,718 **(1.69x)** |
| **8** | 558,500 | 18,141 **(0.03x)** | 14,484 **(0.03x)** | 664,562 **(1.19x)** |

### Prime sieve (primes/s)
*High-concurrency filtering test.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 1,900 | 2,077 **(1.09x)** | 342 **(0.18x)** | 1,463 **(0.77x)** |
| **2** | 4,053 | 2,113 **(0.52x)** | 288 **(0.07x)** | 2,192 **(0.54x)** |
| **4** | 7,787 | 2,002 **(0.26x)** | 289 **(0.04x)** | 1,917 **(0.25x)** |
| **8** | 14,526 | 1,965 **(0.14x)** | 293 **(0.02x)** | 1,449 **(0.10x)** |

---

## Summary

Geometric mean of the ×Go multipliers across pool sizes 1, 2, 4, 8.

| System | Ping-pong | Ring | Fan-out/Fan-in | Spawn idle | Prime sieve |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **libgoc (canary)** | 0.81× | 0.33× | 0.58× | 0.05× | 0.38× |
| **libgoc (vmem)**   | 0.34× | 0.02× | 0.24× | 0.04× | 0.06× |
| **Clojure**         | 0.36× | 0.88× | 0.48× | 1.97× | 0.32× |

**Takeaways:**
- libgoc canary is within ~20% of Go on ping-pong and competitive on prime sieve at pool=1.
- Ring is bottlenecked by the work-stealing scheduler's cross-worker latency; Go's goroutine scheduler has significantly lower handoff overhead.
- Spawn idle tasks reflect the cost of materialising a minicoro fiber with a fixed-size canary-protected stack vs. Go's dynamically-sized goroutine stacks — expected gap.
- Clojure's ring/spawn advantages come from its continuation-passing, heap-allocated go-block model.
- vmem's ring/sieve costs are dominated by TLB/page-fault pressure from mmap-backed stacks.