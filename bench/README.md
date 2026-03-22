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
Channel ping-pong: 200000 round trips in 119ms (1668462 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 308ms (1619555 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 502ms (398338 msg/s)
Spawn idle tasks: 200000 goroutines in 1423ms (140512 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1687ms (1340 primes/s)

=== Pool Size: 2 ===
GOMAXPROCS=2
Channel ping-pong: 200000 round trips in 128ms (1554949 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 334ms (1493978 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 469ms (426361 msg/s)
Spawn idle tasks: 200000 goroutines in 811ms (246540 tasks/s)
Prime sieve: 2262 primes up to 20000 in 873ms (2591 primes/s)

=== Pool Size: 4 ===
GOMAXPROCS=4
Channel ping-pong: 200000 round trips in 131ms (1519533 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 343ms (1455247 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 436ms (458290 msg/s)
Spawn idle tasks: 200000 goroutines in 633ms (315799 tasks/s)
Prime sieve: 2262 primes up to 20000 in 448ms (5041 primes/s)

=== Pool Size: 8 ===
GOMAXPROCS=8
Channel ping-pong: 200000 round trips in 120ms (1657939 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 285ms (1748604 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 395ms (506307 msg/s)
Spawn idle tasks: 200000 goroutines in 609ms (328174 tasks/s)
Prime sieve: 2262 primes up to 20000 in 237ms (9539 primes/s)
```

### libgoc canary — (default) — (`make -C libgoc run-all`)

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 204ms (978885 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 1190ms (420037 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 693ms (288464 msg/s)
Spawn idle tasks: 200000 fibers in 10193ms (19621 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1071ms (2110 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 108ms (1842506 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 625ms (799331 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 430ms (464449 msg/s)
Spawn idle tasks: 200000 fibers in 7917ms (25261 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1078ms (2098 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 100ms (1996624 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 652ms (765776 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 509ms (392907 msg/s)
Spawn idle tasks: 200000 fibers in 10282ms (19451 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1322ms (1710 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 129ms (1547855 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 652ms (766678 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 767ms (260445 msg/s)
Spawn idle tasks: 200000 fibers in 16028ms (12478 tasks/s)
Prime sieve: 2262 primes up to 20000 in 2462ms (919 primes/s)
```

### libgoc vmem (`make -C libgoc LIBGOC_VMEM=ON BUILD_DIR=../../build-bench-vmem build run-all`)

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 532ms (375559 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 14030ms (35636 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1235ms (161866 msg/s)
Spawn idle tasks: 200000 fibers in 6159ms (32469 tasks/s)
Prime sieve: 2262 primes up to 20000 in 8497ms (266 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 319ms (625794 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 12560ms (39807 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1444ms (138416 msg/s)
Spawn idle tasks: 200000 fibers in 8075ms (24767 tasks/s)
Prime sieve: 2262 primes up to 20000 in 9429ms (240 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 252ms (792687 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 12996ms (38473 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1896ms (105433 msg/s)
Spawn idle tasks: 200000 fibers in 12875ms (15533 tasks/s)
Prime sieve: 2262 primes up to 20000 in 9620ms (235 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 278ms (718181 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 9540ms (52409 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1947ms (102685 msg/s)
Spawn idle tasks: 200000 fibers in 17817ms (11225 tasks/s)
Prime sieve: 2262 primes up to 20000 in 9140ms (247 primes/s)
```

### Clojure core.async (`make -C clojure run-all`)

```
=== Pool Size: 1 ===
CLOJURE_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 167ms (1192028 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 173ms (2883906 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 642ms (311273 msg/s)
Spawn idle tasks: 200000 go-blocks in 280ms (711755 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1202ms (1881 primes/s)

=== Pool Size: 2 ===
CLOJURE_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 232ms (861049 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 266ms (1874808 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 568ms (351951 msg/s)
Spawn idle tasks: 200000 go-blocks in 253ms (788886 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1213ms (1864 primes/s)

=== Pool Size: 4 ===
CLOJURE_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 283ms (706072 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 253ms (1976053 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 586ms (340867 msg/s)
Spawn idle tasks: 200000 go-blocks in 260ms (767344 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1323ms (1709 primes/s)

=== Pool Size: 8 ===
CLOJURE_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 344ms (581133 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 342ms (1460544 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 764ms (261466 msg/s)
Spawn idle tasks: 200000 go-blocks in 327ms (610215 tasks/s)
Prime sieve: 2262 primes up to 20000 in 5700ms (397 primes/s)
```

## Report: libgoc vs. Go Baseline (+ Clojure)

This report evaluates the performance of **libgoc (post-optimization)**, **libgoc vmem**, and **Clojure core.async** relative to the **Go** runtime. All figures represent operations per second; the multiplier in parentheses indicates performance relative to the Go baseline (e.g., **1.10x** represents 10% faster, while **0.50x** represents half the speed).

---

### Channel ping-pong (round trips/s)
*Measures overhead of basic synchronization and context switching.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 1,668,462 | 978,885 **(0.59x)** | 375,559 **(0.23x)** | 1,192,028 **(0.71x)** |
| **2** | 1,554,949 | 1,842,506 **(1.18x)** | 625,794 **(0.40x)** | 861,049 **(0.55x)** |
| **4** | 1,519,533 | 1,996,624 **(1.31x)** | 792,687 **(0.52x)** | 706,072 **(0.46x)** |
| **8** | 1,657,939 | 1,547,855 **(0.93x)** | 718,181 **(0.43x)** | 581,133 **(0.35x)** |

### Ring (hops/s)
*Measures message passing latency across a circular topology.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 1,619,555 | 420,037 **(0.26x)** | 35,636 **(0.02x)** | 2,883,906 **(1.78x)** |
| **2** | 1,493,978 | 799,331 **(0.54x)** | 39,807 **(0.03x)** | 1,874,808 **(1.25x)** |
| **4** | 1,455,247 | 765,776 **(0.53x)** | 38,473 **(0.03x)** | 1,976,053 **(1.36x)** |
| **8** | 1,748,604 | 766,678 **(0.44x)** | 52,409 **(0.03x)** | 1,460,544 **(0.84x)** |

### Selective receive / fan-out / fan-in (msg/s)
*Evaluates complex orchestration and selection logic.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 398,338 | 288,464 **(0.72x)** | 161,866 **(0.41x)** | 311,273 **(0.78x)** |
| **2** | 426,361 | 464,449 **(1.09x)** | 138,416 **(0.32x)** | 351,951 **(0.83x)** |
| **4** | 458,290 | 392,907 **(0.86x)** | 105,433 **(0.23x)** | 340,867 **(0.74x)** |
| **8** | 506,307 | 260,445 **(0.51x)** | 102,685 **(0.20x)** | 261,466 **(0.52x)** |

### Spawn idle tasks (tasks/s)
*Tests the efficiency of task creation and scheduling.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 140,512 | 19,621 **(0.14x)** | 32,469 **(0.23x)** | 711,755 **(5.07x)** |
| **2** | 246,540 | 25,261 **(0.10x)** | 24,767 **(0.10x)** | 788,886 **(3.20x)** |
| **4** | 315,799 | 19,451 **(0.06x)** | 15,533 **(0.05x)** | 767,344 **(2.43x)** |
| **8** | 328,174 | 12,478 **(0.04x)** | 11,225 **(0.03x)** | 610,215 **(1.86x)** |

### Prime sieve (primes/s)
*High-concurrency filtering test.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 1,340 | 2,110 **(1.57x)** | 266 **(0.20x)** | 1,881 **(1.40x)** |
| **2** | 2,591 | 2,098 **(0.81x)** | 240 **(0.09x)** | 1,864 **(0.72x)** |
| **4** | 5,041 | 1,710 **(0.34x)** | 235 **(0.05x)** | 1,709 **(0.34x)** |
| **8** | 9,539 | 919 **(0.10x)** | 247 **(0.03x)** | 397 **(0.04x)** |

---

## Summary

### Go

Go remains the reference baseline and scales the most predictably in this run.
Ping-pong stays in a tight 1.52–1.67 M round-trips/s band, ring improves to
1.75 M hops/s at pool=8, and the fan-out benchmark climbs steadily to 506k
msg/s. Prime sieve shows the clearest scheduler scaling, rising from 1,340
primes/s at pool=1 to 9,539 primes/s at pool=8. Spawn idle remains far faster
than libgoc, though still below Clojure's stackless go-block model.

### libgoc

#### canary

This run reflects the current memory-derived bounded-admission policy.

**Ping-pong: strongest at pool=2/4, weaker at pool=8.** Canary peaks at
1.84–2.00 M round trips/s at pool=2/4 (1.18–1.31× Go), then drops to 1.55 M at
pool=8 (0.93× Go).

**Ring: still below Go but improved in mid pools.** Throughput is
0.53–0.54× Go at pool=2/4 and 0.44× at pool=8.

**Fan-out/fan-in: competitive at pool=2 only.** Canary reaches 1.09× Go at
pool=2, but is below Go at pool=1/4/8.

**Spawn idle tasks: materially higher than prior bounded run at low pools.**
Canary now reaches ~19.6k–25.3k tasks/s at pool=1/2, then falls as pool size
increases.

**Prime sieve: strong at low pools, collapses at high pools.** Canary is 1.57×
Go at pool=1 and 0.81× at pool=2, but drops sharply by pool=8.

#### vmem

vmem remains substantially slower than Go and generally below canary on channel-
heavy benchmarks.

**Ring and prime sieve remain the main bottlenecks.** Ring stays near
35k–52k hops/s (~0.02–0.03× Go), and prime sieve remains 0.03–0.20× Go.

**Spawn-idle improved notably at lower pools in this run.** vmem reaches
32.5k tasks/s at pool=1 and 24.8k at pool=2, outperforming canary in spawn-idle
at pool=1 and remaining near it at pool=2.

At the moment, vmem should still be treated as a bounded-correctness/stress
configuration rather than a near-parity performance mode.

### Clojure

**Ring: Clojure still shines.** It leads the field at pool=1 and remains above
Go through pool=4, topping out at 2.88 M hops/s at pool=1.

**Ping-pong: consistently below Go and canary's best case.** Clojure ranges
from 0.35× to 0.71× Go here.

**Fan-out/fan-in: steady but sub-Go.** The benchmark stays in a 0.52–0.83× Go
band, ahead of current vmem but generally behind Go and canary pool=2.

**Spawn idle: still the easiest winner.** go-block creation remains by far the
cheapest model in this comparison, at 1.86–5.07× Go and an order of magnitude
above libgoc.

**Prime sieve: only pool=1 is competitive.** Clojure slightly beats Go at
pool=1, then falls off quickly as the JVM pool size increases.
