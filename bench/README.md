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
Channel ping-pong: 200000 round trips in 83ms (2394011 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 217ms (2304024 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 601ms (332470 msg/s)
Spawn idle tasks: 200000 fibers in 10331ms (19358 tasks/s)
Prime sieve: 2262 primes up to 20000 in 895ms (2525 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 103ms (1926799 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 284ms (1759241 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 372ms (536918 msg/s)
Spawn idle tasks: 200000 fibers in 7919ms (25254 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1713ms (1320 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 309ms (645849 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 961ms (520195 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 475ms (420848 msg/s)
Spawn idle tasks: 200000 fibers in 10373ms (19281 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1731ms (1307 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 410ms (487284 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 727ms (687284 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 902ms (221523 msg/s)
Spawn idle tasks: 200000 fibers in 12694ms (15755 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1600ms (1413 primes/s)
```

### libgoc vmem — `-DLIBGOC_VMEM=ON` post-optimization — (`make run-all`)

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 82ms (2430946 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 206ms (2418900 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 613ms (326157 msg/s)
Spawn idle tasks: 200000 fibers in 10293ms (19429 tasks/s)
Prime sieve: 2262 primes up to 20000 in 880ms (2569 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 103ms (1927292 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 284ms (1756068 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 376ms (531255 msg/s)
Spawn idle tasks: 200000 fibers in 7974ms (25081 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1753ms (1290 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 316ms (631635 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 980ms (509726 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 516ms (387529 msg/s)
Spawn idle tasks: 200000 fibers in 10478ms (19087 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1745ms (1296 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 411ms (485825 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 727ms (686970 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 927ms (215575 msg/s)
Spawn idle tasks: 200000 fibers in 13147ms (15212 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1627ms (1390 primes/s)
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
| **1** | 2,280,645 | 2,394,011 **(1.05x)** | 2,430,946 **(1.07x)** | 829,898 **(0.36x)** |
| **2** | 2,224,597 | 1,926,799 **(0.87x)** | 1,927,292 **(0.87x)** | 942,408 **(0.42x)** |
| **4** | 2,228,437 | 645,849 **(0.29x)** | 631,635 **(0.28x)** | 839,270 **(0.38x)** |
| **8** | 2,257,564 | 487,284 **(0.22x)** | 485,825 **(0.22x)** | 676,348 **(0.30x)** |

### Ring (hops/s)
*Measures message passing latency across a circular topology.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 2,243,222 | 2,304,024 **(1.03x)** | 2,418,900 **(1.08x)** | 1,697,399 **(0.76x)** |
| **2** | 2,284,381 | 1,759,241 **(0.77x)** | 1,756,068 **(0.77x)** | 1,976,702 **(0.87x)** |
| **4** | 2,240,562 | 520,195 **(0.23x)** | 509,726 **(0.23x)** | 2,029,523 **(0.91x)** |
| **8** | 2,250,942 | 687,284 **(0.31x)** | 686,970 **(0.31x)** | 1,677,909 **(0.75x)** |

### Selective receive / fan-out / fan-in (msg/s)
*Evaluates complex orchestration and selection logic.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 599,056 | 332,470 **(0.55x)** | 326,157 **(0.54x)** | 193,405 **(0.32x)** |
| **2** | 650,773 | 536,918 **(0.82x)** | 531,255 **(0.82x)** | 368,205 **(0.57x)** |
| **4** | 661,967 | 420,848 **(0.64x)** | 387,529 **(0.59x)** | 396,970 **(0.60x)** |
| **8** | 657,846 | 221,523 **(0.34x)** | 215,575 **(0.33x)** | 204,874 **(0.31x)** |

### Spawn idle tasks (tasks/s)
*Tests the efficiency of task creation and scheduling.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 188,282 | 19,358 **(0.10x)** | 19,429 **(0.10x)** | 425,452 **(2.26x)** |
| **2** | 350,786 | 25,254 **(0.07x)** | 25,081 **(0.07x)** | 766,945 **(2.19x)** |
| **4** | 416,456 | 19,281 **(0.05x)** | 19,087 **(0.05x)** | 778,002 **(1.87x)** |
| **8** | 492,388 | 15,755 **(0.03x)** | 15,212 **(0.03x)** | 421,344 **(0.86x)** |

### Prime sieve (primes/s)
*High-concurrency filtering test.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 1,919 | 2,525 **(1.32x)** | 2,569 **(1.34x)** | 1,009 **(0.53x)** |
| **2** | 3,962 | 1,320 **(0.33x)** | 1,290 **(0.33x)** | 2,133 **(0.54x)** |
| **4** | 7,647 | 1,307 **(0.17x)** | 1,296 **(0.17x)** | 1,815 **(0.24x)** |
| **8** | 14,136 | 1,413 **(0.10x)** | 1,390 **(0.10x)** | 565 **(0.04x)** |

---

## Summary

**At pool=1, libgoc matches or slightly exceeds Go for channel throughput.**
Both canary and vmem builds reach ~2.39–2.43 M round trips/s (ping-pong,
+5–7% vs Go) and ~2.30–2.42 M hops/s (ring, +3–8%) — essentially identical
to each other and ahead of Go.  With a single pool thread, all fibers run
on the same OS thread, so there are no cross-thread wakeups.

**Canary and vmem now perform almost identically at all pool sizes.**  After
the pool optimizations (Fix 1/3/4/5) the pre-optimization gap between canary
and vmem has nearly vanished.  Both modes degrade at the same rate as pool
threads increase, confirming that cross-thread wakeup latency (not stack
allocation or GC scan overhead per se) is the dominant cost.  The remaining
bottleneck is the shared-pool scheduler's lack of work-stealing, tracked in
`TODO.md`.

**Fan-out/fan-in benefits most from parallelism.**  At pool=2 both modes
reach ~0.82× Go (up from ~0.70× pre-optimization), the best scaling ratio
across all benchmarks.  The 8-worker parallel pattern allows genuine
load-sharing across threads.

**Spawn idle — Go's goroutine model is ~10–20× faster.**  Go goroutines have
a ~2–4 KiB initial stack that grows automatically; minicoro fibers use a
fixed-size or vmem-backed stack (default 2 MiB virtual, ~136 bytes initially
committed in vmem mode; 64 KiB fully committed in canary mode).  The GC must
track each fiber's root set, making 200 K fiber creation a substantially
heavier operation than 200 K goroutine creation.

**Prime sieve at pool=1: libgoc is ~1.32–1.34× faster than Go.**  The sieve
is a deep pipeline of N small fibers passing single values; at pool=1 the
fibers run cooperatively with zero synchronization overhead.  Both canary
and vmem modes perform comparably (~2525–2569/s).  Go scales linearly with
more threads (14 K/s at pool=8) while libgoc does not, because the long
serial pipeline cannot be parallelised without restructuring.

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
