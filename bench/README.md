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
Channel ping-pong: 200000 round trips in 197ms (1010144 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 1170ms (427214 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 727ms (274870 msg/s)
Spawn idle tasks: 200000 fibers in 4915ms (40688 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1312ms (1724 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 108ms (1843485 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 575ms (869453 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 413ms (483536 msg/s)
Spawn idle tasks: 200000 fibers in 3381ms (59148 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1344ms (1682 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 131ms (1516261 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 583ms (857014 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 472ms (423001 msg/s)
Spawn idle tasks: 200000 fibers in 5921ms (33777 tasks/s)
Prime sieve: 2262 primes up to 20000 in 2376ms (952 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 196ms (1015980 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 829ms (602770 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 841ms (237615 msg/s)
Spawn idle tasks: 200000 fibers in 7806ms (25619 tasks/s)
Prime sieve: 2262 primes up to 20000 in 2271ms (996 primes/s)
```

### libgoc vmem — `-DLIBGOC_VMEM=ON` — (`make -C libgoc BUILD_DIR=../../build-bench-vmem build run-all`)

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 498ms (401100 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 11099ms (45047 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 921ms (216928 msg/s)
Spawn idle tasks: 200000 fibers in 5670ms (35273 tasks/s)
Prime sieve: 2262 primes up to 20000 in 8006ms (283 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 286ms (697435 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 11938ms (41880 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1375ms (145359 msg/s)
Spawn idle tasks: 200000 fibers in 7075ms (28268 tasks/s)
Prime sieve: 2262 primes up to 20000 in 8967ms (252 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 229ms (872107 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 12537ms (39880 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1769ms (113047 msg/s)
Spawn idle tasks: 200000 fibers in 13004ms (15379 tasks/s)
Prime sieve: 2262 primes up to 20000 in 12925ms (175 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 265ms (752246 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 9152ms (54628 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1839ms (108724 msg/s)
Spawn idle tasks: 200000 fibers in 17201ms (11627 tasks/s)
Prime sieve: 2262 primes up to 20000 in 12887ms (176 primes/s)
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
| **1** | 1,668,462 | 1,010,144 **(0.61x)** | 401,100 **(0.24x)** | 1,192,028 **(0.71x)** |
| **2** | 1,554,949 | 1,843,485 **(1.19x)** | 697,435 **(0.45x)** | 861,049 **(0.55x)** |
| **4** | 1,519,533 | 1,516,261 **(1.00x)** | 872,107 **(0.57x)** | 706,072 **(0.46x)** |
| **8** | 1,657,939 | 1,015,980 **(0.61x)** | 752,246 **(0.45x)** | 581,133 **(0.35x)** |

### Ring (hops/s)
*Measures message passing latency across a circular topology.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 1,619,555 | 427,214 **(0.26x)** | 45,047 **(0.03x)** | 2,883,906 **(1.78x)** |
| **2** | 1,493,978 | 869,453 **(0.58x)** | 41,880 **(0.03x)** | 1,874,808 **(1.25x)** |
| **4** | 1,455,247 | 857,014 **(0.59x)** | 39,880 **(0.03x)** | 1,976,053 **(1.36x)** |
| **8** | 1,748,604 | 602,770 **(0.34x)** | 54,628 **(0.03x)** | 1,460,544 **(0.84x)** |

### Selective receive / fan-out / fan-in (msg/s)
*Evaluates complex orchestration and selection logic.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 398,338 | 274,870 **(0.69x)** | 216,928 **(0.54x)** | 311,273 **(0.78x)** |
| **2** | 426,361 | 483,536 **(1.13x)** | 145,359 **(0.34x)** | 351,951 **(0.83x)** |
| **4** | 458,290 | 423,001 **(0.92x)** | 113,047 **(0.25x)** | 340,867 **(0.74x)** |
| **8** | 506,307 | 237,615 **(0.47x)** | 108,724 **(0.21x)** | 261,466 **(0.52x)** |

### Spawn idle tasks (tasks/s)
*Tests the efficiency of task creation and scheduling.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 140,512 | 40,688 **(0.29x)** | 35,273 **(0.25x)** | 711,755 **(5.07x)** |
| **2** | 246,540 | 59,148 **(0.24x)** | 28,268 **(0.11x)** | 788,886 **(3.20x)** |
| **4** | 315,799 | 33,777 **(0.11x)** | 15,379 **(0.05x)** | 767,344 **(2.43x)** |
| **8** | 328,174 | 25,619 **(0.08x)** | 11,627 **(0.04x)** | 610,215 **(1.86x)** |

### Prime sieve (primes/s)
*High-concurrency filtering test.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 1,340 | 1,724 **(1.29x)** | 283 **(0.21x)** | 1,881 **(1.40x)** |
| **2** | 2,591 | 1,682 **(0.65x)** | 252 **(0.10x)** | 1,864 **(0.72x)** |
| **4** | 5,041 | 952 **(0.19x)** | 175 **(0.03x)** | 1,709 **(0.34x)** |
| **8** | 9,539 | 996 **(0.10x)** | 176 **(0.02x)** | 397 **(0.04x)** |

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

The current canary build reflects the new bounded-admission behavior rather
than the older work-stealing-only results.

**Ping-pong: only pool=2 clearly beats Go.** Canary reaches 1.84 M round
trips/s at pool=2 (1.19× Go), matches Go at pool=4, and falls behind at
pool=1 and pool=8.

**Ring: well below Go at every pool size.** Current canary throughput is only
0.26–0.59× Go, with the best result at pool=2 (869k hops/s).

**Fan-out/fan-in: competitive only at pool=2.** Pool=2 reaches 483k msg/s
(1.13× Go), but the benchmark drops below Go at pool=1, 4, and 8.

**Spawn idle tasks: bounded admission helps materially.** Canary now reaches
40.7k tasks/s at pool=1 and 59.1k tasks/s at pool=2, much faster than the old
"materialise all 200k fibers immediately" behavior, but still far behind Go
and Clojure.

**Prime sieve: wins only at pool=1.** The sieve starts slightly ahead of Go at
pool=1 (1.29×) and then degrades sharply as thread count increases.

#### vmem

vmem is no longer close to canary in the current build.

**Across all five benchmarks, vmem is substantially slower than both Go and
canary.** Ping-pong drops to 0.24–0.57× Go, ring collapses to roughly 40–55k
hops/s (about 0.03× Go), and prime sieve bottoms out at only 175–283 primes/s.
Even spawn idle, where the throttle bounds memory use successfully, remains
below canary at every pool size.

At the moment, the vmem configuration should be considered a bounded-correctness
and stress configuration rather than a near-parity performance option.

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
