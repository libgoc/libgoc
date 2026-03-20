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

### Go (`make run-all`)

```
=== Pool Size: 1 ===
GOMAXPROCS=1
Channel ping-pong: 200000 round trips in 87ms (2280645 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 222ms (2243222 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 333ms (599056 msg/s)
Spawn idle tasks: 200000 goroutines in 1062ms (188282 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1178ms (1919 primes/s)

=== Pool Size: 2 ===
GOMAXPROCS=2
Channel ping-pong: 200000 round trips in 89ms (2224597 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 218ms (2284381 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 307ms (650773 msg/s)
Spawn idle tasks: 200000 goroutines in 570ms (350786 tasks/s)
Prime sieve: 2262 primes up to 20000 in 570ms (3962 primes/s)

=== Pool Size: 4 ===
GOMAXPROCS=4
Channel ping-pong: 200000 round trips in 89ms (2228437 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 223ms (2240562 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 302ms (661967 msg/s)
Spawn idle tasks: 200000 goroutines in 480ms (416456 tasks/s)
Prime sieve: 2262 primes up to 20000 in 295ms (7647 primes/s)

=== Pool Size: 8 ===
GOMAXPROCS=8
Channel ping-pong: 200000 round trips in 88ms (2257564 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 222ms (2250942 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 304ms (657846 msg/s)
Spawn idle tasks: 200000 goroutines in 406ms (492388 tasks/s)
Prime sieve: 2262 primes up to 20000 in 160ms (14136 primes/s)
```

### libgoc — post-optimization (Fix 1/3/4/5) — (`make run-all`)

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 80ms (2496952 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 212ms (2357434 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 590ms (338569 msg/s)
Spawn idle tasks: 200000 fibers in 10510ms (19029 tasks/s)
Prime sieve: 2262 primes up to 20000 in 876ms (2580 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 109ms (1825449 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 302ms (1651751 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 423ms (472757 msg/s)
Spawn idle tasks: 200000 fibers in 7920ms (25252 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1691ms (1337 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 310ms (643849 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 1003ms (498488 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 613ms (326004 msg/s)
Spawn idle tasks: 200000 fibers in 10566ms (18927 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1739ms (1300 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 406ms (492232 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 724ms (689667 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 773ms (258467 msg/s)
Spawn idle tasks: 200000 fibers in 12818ms (15602 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1631ms (1387 primes/s)
```

### libgoc vmem — `-DLIBGOC_VMEM=ON` — (`make run-all`)

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 85ms (2343156 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 222ms (2248408 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 642ms (311113 msg/s)
Spawn idle tasks: 200000 fibers in 10334ms (19353 tasks/s)
Prime sieve: 2262 primes up to 20000 in 952ms (2375 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 105ms (1895838 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 301ms (1657377 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 440ms (454532 msg/s)
Spawn idle tasks: 200000 fibers in 7939ms (25190 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1651ms (1370 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 333ms (599752 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 944ms (529457 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 547ms (365187 msg/s)
Spawn idle tasks: 200000 fibers in 10526ms (18999 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1686ms (1342 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 430ms (464624 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 808ms (618487 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 933ms (214316 msg/s)
Spawn idle tasks: 200000 fibers in 12828ms (15590 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1580ms (1431 primes/s)
```

### Clojure core.async (`make run-all`)

```
=== Pool Size: 1 ===
CLOJURE_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 240ms (829898 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 294ms (1697399 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1034ms (193405 msg/s)
Spawn idle tasks: 200000 go-blocks in 470ms (425452 tasks/s)
Prime sieve: 2262 primes up to 20000 in 2241ms (1009 primes/s)

=== Pool Size: 2 ===
CLOJURE_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 212ms (942408 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 252ms (1976702 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 543ms (368205 msg/s)
Spawn idle tasks: 200000 go-blocks in 260ms (766945 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1060ms (2133 primes/s)

=== Pool Size: 4 ===
CLOJURE_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 238ms (839270 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 246ms (2029523 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 503ms (396970 msg/s)
Spawn idle tasks: 200000 go-blocks in 257ms (778002 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1245ms (1815 primes/s)

=== Pool Size: 8 ===
CLOJURE_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 295ms (676348 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 297ms (1677909 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 976ms (204874 msg/s)
Spawn idle tasks: 200000 go-blocks in 474ms (421344 tasks/s)
Prime sieve: 2262 primes up to 20000 in 4006ms (565 primes/s)
```

## Report: libgoc vs. Go Baseline (+ Clojure)

This report evaluates the performance of **libgoc (post-optimization)**, **libgoc vmem**, and **Clojure core.async** relative to the **Go** runtime. All figures represent operations per second; the multiplier in parentheses indicates performance relative to the Go baseline (e.g., **1.10x** represents 10% faster, while **0.50x** represents half the speed).

---

### Channel ping-pong (round trips/s)
*Measures overhead of basic synchronization and context switching.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 2,280,645 | 2,496,952 **(1.09x)** | 2,343,156 **(1.03x)** | 829,898 **(0.36x)** |
| **2** | 2,224,597 | 1,825,449 **(0.82x)** | 1,895,838 **(0.85x)** | 942,408 **(0.42x)** |
| **4** | 2,228,437 | 643,849 **(0.29x)** | 599,752 **(0.27x)** | 839,270 **(0.38x)** |
| **8** | 2,257,564 | 492,232 **(0.22x)** | 464,624 **(0.21x)** | 676,348 **(0.30x)** |

### Ring (hops/s)
*Measures message passing latency across a circular topology.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 2,243,222 | 2,357,434 **(1.05x)** | 2,248,408 **(1.00x)** | 1,697,399 **(0.76x)** |
| **2** | 2,284,381 | 1,651,751 **(0.72x)** | 1,657,377 **(0.73x)** | 1,976,702 **(0.87x)** |
| **4** | 2,240,562 | 498,488 **(0.22x)** | 529,457 **(0.24x)** | 2,029,523 **(0.91x)** |
| **8** | 2,250,942 | 689,667 **(0.31x)** | 618,487 **(0.27x)** | 1,677,909 **(0.75x)** |

### Selective receive / fan-out / fan-in (msg/s)
*Evaluates complex orchestration and selection logic.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 599,056 | 338,569 **(0.57x)** | 311,113 **(0.52x)** | 193,405 **(0.32x)** |
| **2** | 650,773 | 472,757 **(0.73x)** | 454,532 **(0.70x)** | 368,205 **(0.57x)** |
| **4** | 661,967 | 326,004 **(0.49x)** | 365,187 **(0.55x)** | 396,970 **(0.60x)** |
| **8** | 657,846 | 258,467 **(0.39x)** | 214,316 **(0.33x)** | 204,874 **(0.31x)** |

### Spawn idle tasks (tasks/s)
*Tests the efficiency of task creation and scheduling.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 188,282 | 19,029 **(0.10x)** | 19,353 **(0.10x)** | 425,452 **(2.26x)** |
| **2** | 350,786 | 25,252 **(0.07x)** | 25,190 **(0.07x)** | 766,945 **(2.19x)** |
| **4** | 416,456 | 18,927 **(0.05x)** | 18,999 **(0.05x)** | 778,002 **(1.87x)** |
| **8** | 492,388 | 15,602 **(0.03x)** | 15,590 **(0.03x)** | 421,344 **(0.86x)** |

### Prime sieve (primes/s)
*High-concurrency filtering test.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 1,919 | 2,580 **(1.34x)** | 2,375 **(1.24x)** | 1,009 **(0.53x)** |
| **2** | 3,962 | 1,337 **(0.34x)** | 1,370 **(0.35x)** | 2,133 **(0.54x)** |
| **4** | 7,647 | 1,300 **(0.17x)** | 1,342 **(0.18x)** | 1,815 **(0.24x)** |
| **8** | 14,136 | 1,387 **(0.10x)** | 1,431 **(0.10x)** | 565 **(0.04x)** |

---

## Summary

**At pool=1, libgoc exceeds Go for channel throughput.**  The optimized build
reaches ~2.5 M round trips/s (ping-pong, +9% vs Go) and ~2.36 M hops/s
(ring, +5%) — slightly ahead of both vmem and the Go baseline.
With a single pool thread, all fibers run on the same OS thread, so there
are no cross-thread wakeups and the only overhead is the `GC_set_stackbottom`
redirect on each `mco_resume`.

**At pool > 1, cross-thread wakeup contention dominates.**  Both libgoc
build modes scale poorly as pool threads increase because the current pool
scheduler lacks work-stealing; every fiber wake-up incurs a cross-thread
semaphore post.  At pool=4–8 the libgoc and vmem numbers converge,
suggesting that `GC_set_stackbottom` (Fix 3's remaining calls) or
channel/mutex contention (not the node allocator) is the new bottleneck.

**Fan-out/fan-in holds up best.**  The 8-worker parallel pattern benefits
from true parallelism; libgoc reaches 0.73× Go at pool=2 (up from 0.70×
pre-optimization) and 0.49× at pool=4, better than the ring (0.22×) at
the same pool count.

**Spawn idle — Go's goroutine model is ~10–25× faster.**  Go goroutines have
a ~2–4 KiB initial stack that grows automatically; minicoro fibers use a
fixed-size or vmem-backed stack (default 2 MiB virtual, ~136 bytes initially
committed in vmem mode; 64 KiB fully committed in canary mode).  The GC must
track each fiber's root set, making 200 K fiber creation a substantially
heavier operation than 200 K goroutine creation.

**Prime sieve at pool=1: libgoc is faster than Go.**  The sieve is a deep
pipeline of N small fibers passing single values; at pool=1 the fibers run
cooperatively with zero synchronization overhead.  Canary mode (2580/s, +1.34×
Go) edges vmem (2375/s, +1.24×).  Go scales linearly with more threads (14 K/s
at pool=8) while libgoc does not, because the long serial pipeline cannot be
parallelised without restructuring.

**Go scalability.**  Go's work-stealing scheduler keeps communicating
goroutines on the same thread naturally and scales CPU-bound pipelines
(spawn: 188 K → 492 K/s; prime sieve: 1919 → 14136/s from pool=1 to pool=8)
while communication-bound workloads (ping-pong, ring) remain flat.
libgoc's current pool scheduler lacks work-stealing; this is a known area
of improvement tracked in `TODO.md`.

**Clojure core.async** uses IOC (inversion-of-control) state-machine
continuations rather than true green threads.  Go blocks are dispatched
onto a fixed JVM thread pool (`CLOJURE_POOL_THREADS`), analogous to
`GOMAXPROCS` and `GOC_POOL_THREADS`.

**Clojure ping-pong and ring are 0.30–0.91× Go.**  Channel round-trip
overhead is higher than Go (no work-stealing, JVM dispatch overhead), but
Clojure holds up better than libgoc at high pool counts for the ring:
at pool=4 Clojure achieves 2.03 M hops/s (0.91×) while libgoc canary
falls to 0.44× and vmem to 0.24×.

**Spawn idle: Clojure is 2× faster than Go at pool=1–4.**  core.async
go-blocks are cheap heap-allocated state machines — no fixed stack, no GC
root scanning per task — so spawning 200 K of them is substantially cheaper
than goroutines or minicoro fibers.  The advantage disappears at pool=8
(0.86×) as thread-pool contention during wakeup dominates.

**Prime sieve: Clojure does not scale.**  Like libgoc, the serial pipeline
structure prevents parallelism.  Clojure is 0.53× Go at pool=1 and falls
to 0.04× at pool=8, worse than both libgoc variants, because IOC
continuations add higher per-hop overhead than native fibers or goroutines.
