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
Channel ping-pong: 200000 round trips in 224ms (892022 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 1398ms (357644 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1436ms (139226 msg/s)
Spawn idle tasks: 200000 fibers in 2374ms (84244 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1597ms (1416 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 219ms (912402 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 1256ms (397976 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1443ms (138512 msg/s)
Spawn idle tasks: 200000 fibers in 2350ms (85079 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1167ms (1938 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 111ms (1788707 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 698ms (716300 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 773ms (258405 msg/s)
Spawn idle tasks: 200000 fibers in 1392ms (143669 tasks/s)
Prime sieve: 2262 primes up to 20000 in 801ms (2821 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 88ms (2248271 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 495ms (1008686 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 840ms (238041 msg/s)
Spawn idle tasks: 200000 fibers in 1203ms (166231 tasks/s)
Prime sieve: 2262 primes up to 20000 in 877ms (2577 primes/s)
```

### libgoc vmem (`make -C libgoc LIBGOC_VMEM=ON BUILD_DIR=../../build-bench-vmem build run-all`)

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 739ms (270336 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 18645ms (26816 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 808ms (247494 msg/s)
Spawn idle tasks: 200000 fibers in 3041ms (65766 tasks/s)
Prime sieve: 2262 primes up to 20000 in 2790ms (811 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 337ms (592109 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 13771ms (36308 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 785ms (254468 msg/s)
Spawn idle tasks: 200000 fibers in 11263ms (17756 tasks/s)
Prime sieve: 2262 primes up to 20000 in 2374ms (952 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 283ms (706642 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 14543ms (34380 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 733ms (272542 msg/s)
Spawn idle tasks: 200000 fibers in 3064ms (65263 tasks/s)
Prime sieve: 2262 primes up to 20000 in 3184ms (710 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 225ms (886815 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 10013ms (49935 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 652ms (306298 msg/s)
Spawn idle tasks: 200000 fibers in 3156ms (63358 tasks/s)
Prime sieve: 2262 primes up to 20000 in 3141ms (720 primes/s)
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

This report evaluates the performance of **libgoc canary**, **libgoc vmem**, and **Clojure core.async** relative to the **Go** runtime. All figures represent operations per second; the multiplier in parentheses indicates performance relative to the Go baseline (e.g., **1.10x** means 10% faster, **0.50x** means half the speed).

---

### Channel ping-pong (round trips/s)
*Measures overhead of basic synchronization and context switching.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 2,431,608 | 892,022 **(0.37x)** | 270,336 **(0.11x)** | 1,301,978 **(0.54x)** |
| **2** | 2,192,214 | 912,402 **(0.42x)** | 592,109 **(0.27x)** | 750,500 **(0.34x)** |
| **4** | 2,240,324 | 1,788,707 **(0.80x)** | 706,642 **(0.32x)** | 763,754 **(0.34x)** |
| **8** | 2,292,833 | 2,248,271 **(0.98x)** | 886,815 **(0.39x)** | 641,775 **(0.28x)** |

### Ring (hops/s)
*Measures message passing latency across a circular topology.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 2,395,887 | 357,644 **(0.15x)** | 26,816 **(0.01x)** | 2,608,232 **(1.09x)** |
| **2** | 2,297,843 | 397,976 **(0.17x)** | 36,308 **(0.02x)** | 1,884,720 **(0.82x)** |
| **4** | 2,305,941 | 716,300 **(0.31x)** | 34,380 **(0.01x)** | 1,975,028 **(0.86x)** |
| **8** | 2,298,159 | 1,008,686 **(0.44x)** | 49,935 **(0.02x)** | 1,836,152 **(0.80x)** |

### Selective receive / fan-out / fan-in (msg/s)
*Evaluates complex orchestration and selection logic.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 595,850 | 139,226 **(0.23x)** | 247,494 **(0.42x)** | 306,730 **(0.51x)** |
| **2** | 696,528 | 138,512 **(0.20x)** | 254,468 **(0.37x)** | 349,289 **(0.50x)** |
| **4** | 753,517 | 258,405 **(0.34x)** | 272,542 **(0.36x)** | 342,562 **(0.45x)** |
| **8** | 752,862 | 238,041 **(0.32x)** | 306,298 **(0.41x)** | 327,600 **(0.44x)** |

### Spawn idle tasks (tasks/s)
*Tests the efficiency of task creation and scheduling.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 154,711 | 84,244 **(0.54x)** | 65,766 **(0.43x)** | 663,426 **(4.29x)** |
| **2** | 393,206 | 85,079 **(0.22x)** | 17,756 **(0.05x)** | 689,487 **(1.75x)** |
| **4** | 441,883 | 143,669 **(0.33x)** | 65,263 **(0.15x)** | 745,718 **(1.69x)** |
| **8** | 558,500 | 166,231 **(0.30x)** | 63,358 **(0.11x)** | 664,562 **(1.19x)** |

### Prime sieve (primes/s)
*High-concurrency filtering test.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 1,900 | 1,416 **(0.75x)** | 811 **(0.43x)** | 1,463 **(0.77x)** |
| **2** | 4,053 | 1,938 **(0.48x)** | 952 **(0.23x)** | 2,192 **(0.54x)** |
| **4** | 7,787 | 2,821 **(0.36x)** | 710 **(0.09x)** | 1,917 **(0.25x)** |
| **8** | 14,526 | 2,577 **(0.18x)** | 720 **(0.05x)** | 1,449 **(0.10x)** |

---

## Summary

Geometric mean of the ×Go multipliers across pool sizes 1, 2, 4, 8.

| Benchmark | libgoc (canary) | libgoc (vmem) | Clojure |
| :--- | :---: | :---: | :---: |
| Ping-pong | 0.59× | 0.25× | 0.36× |
| Ring | 0.24× | 0.02× | 0.88× |
| Fan-out/Fan-in | 0.27× | 0.39× | 0.48× |
| Spawn idle | 0.33× | 0.13× | 1.97× |
| Prime sieve | 0.39× | 0.15× | 0.32× |

**Takeaways:**
- libgoc canary ping-pong scales strongly with pool size, reaching near-parity with Go at pool=8 (0.98×). Ring throughput also crosses 1 M hops/s at pool=8 (0.44×).
- Spawn idle tasks improved dramatically over previous runs (84–166k tasks/s vs ~18k), driven by scheduler improvements. Still behind Go due to fixed-size stack materialisation cost vs. Go's dynamically-sized goroutines.
- Fan-out/fan-in canary throughput (139–258k msg/s) is below previous numbers, suggesting regression in selective-receive/alts overhead worth investigating.
- vmem fan-out has improved vs. the prior run (247–306k msg/s, 0.39× geomean) and now exceeds canary on this benchmark — likely because vmem's larger virtual stacks reduce stack-overflow guard overhead.
- vmem ring remains severely bottlenecked by TLB/page-fault pressure from mmap-backed stacks (~27–50k hops/s vs. Go's 2.3 M).
- Clojure's ring/spawn advantages come from its continuation-passing, heap-allocated go-block model, which avoids stack materialisation entirely.
