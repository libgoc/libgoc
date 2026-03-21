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
Channel ping-pong: 200000 round trips in 131ms (1526518 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 308ms (1619735 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 511ms (391272 msg/s)
Spawn idle tasks: 200000 goroutines in 1379ms (144932 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1767ms (1280 primes/s)

=== Pool Size: 2 ===
GOMAXPROCS=2
Channel ping-pong: 200000 round trips in 134ms (1489831 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 330ms (1513880 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 503ms (397010 msg/s)
Spawn idle tasks: 200000 goroutines in 845ms (236451 tasks/s)
Prime sieve: 2262 primes up to 20000 in 898ms (2517 primes/s)

=== Pool Size: 4 ===
GOMAXPROCS=4
Channel ping-pong: 200000 round trips in 130ms (1536391 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 334ms (1493600 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 498ms (401595 msg/s)
Spawn idle tasks: 200000 goroutines in 667ms (299832 tasks/s)
Prime sieve: 2262 primes up to 20000 in 480ms (4710 primes/s)

=== Pool Size: 8 ===
GOMAXPROCS=8
Channel ping-pong: 200000 round trips in 129ms (1540513 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 343ms (1455217 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 503ms (397104 msg/s)
Spawn idle tasks: 200000 goroutines in 696ms (286965 tasks/s)
Prime sieve: 2262 primes up to 20000 in 249ms (9074 primes/s)
```

### libgoc canary — (default) — (`make -C libgoc run-all`)

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 77ms (2592082 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 195ms (2553508 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 606ms (329946 msg/s)
Spawn idle tasks: 200000 fibers in 13292ms (15047 tasks/s)
Prime sieve: 2262 primes up to 20000 in 599ms (3773 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 104ms (1918550 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 274ms (1821430 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 369ms (541717 msg/s)
Spawn idle tasks: 200000 fibers in 9513ms (21023 tasks/s)
Prime sieve: 2262 primes up to 20000 in 441ms (5118 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 108ms (1835567 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 268ms (1861899 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 451ms (442562 msg/s)
Spawn idle tasks: 200000 fibers in 10245ms (19522 tasks/s)
Prime sieve: 2262 primes up to 20000 in 556ms (4064 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 126ms (1579323 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 323ms (1546837 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 517ms (386839 msg/s)
Spawn idle tasks: 200000 fibers in 10622ms (18827 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1007ms (2245 primes/s)
```

### libgoc vmem — `-DLIBGOC_VMEM=ON` — (`make -C libgoc BUILD_DIR=../../build-bench-vmem build run-all`)

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 77ms (2577912 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 201ms (2483812 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 611ms (326836 msg/s)
Spawn idle tasks: 200000 fibers in 12455ms (16057 tasks/s)
Prime sieve: 2262 primes up to 20000 in 611ms (3697 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 105ms (1901673 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 273ms (1826997 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 378ms (528516 msg/s)
Spawn idle tasks: 200000 fibers in 9081ms (22022 tasks/s)
Prime sieve: 2262 primes up to 20000 in 452ms (5004 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 109ms (1831355 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 267ms (1871631 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 459ms (435226 msg/s)
Spawn idle tasks: 200000 fibers in 9691ms (20636 tasks/s)
Prime sieve: 2262 primes up to 20000 in 578ms (3910 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 125ms (1588654 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 305ms (1635970 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 585ms (341391 msg/s)
Spawn idle tasks: 200000 fibers in 10689ms (18710 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1151ms (1964 primes/s)
```

### Clojure core.async (`make -C clojure run-all`)

```
=== Pool Size: 1 ===
CLOJURE_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 166ms (1197631 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 170ms (2937427 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 648ms (308529 msg/s)
Spawn idle tasks: 200000 go-blocks in 281ms (711577 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1270ms (1781 primes/s)

=== Pool Size: 2 ===
CLOJURE_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 248ms (805824 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 267ms (1866505 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 577ms (346304 msg/s)
Spawn idle tasks: 200000 go-blocks in 258ms (773872 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1226ms (1844 primes/s)

=== Pool Size: 4 ===
CLOJURE_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 254ms (785303 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 273ms (1830381 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 572ms (349372 msg/s)
Spawn idle tasks: 200000 go-blocks in 277ms (720400 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1355ms (1669 primes/s)

=== Pool Size: 8 ===
CLOJURE_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 275ms (726918 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 304ms (1642735 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 803ms (248836 msg/s)
Spawn idle tasks: 200000 go-blocks in 407ms (490309 tasks/s)
Prime sieve: 2262 primes up to 20000 in 4212ms (537 primes/s)
```

## Report: libgoc vs. Go Baseline (+ Clojure)

This report evaluates the performance of **libgoc (post-optimization)**, **libgoc vmem**, and **Clojure core.async** relative to the **Go** runtime. All figures represent operations per second; the multiplier in parentheses indicates performance relative to the Go baseline (e.g., **1.10x** represents 10% faster, while **0.50x** represents half the speed).

---

### Channel ping-pong (round trips/s)
*Measures overhead of basic synchronization and context switching.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 1,526,518 | 2,592,082 **(1.70x)** | 2,577,912 **(1.69x)** | 1,197,631 **(0.78x)** |
| **2** | 1,489,831 | 1,918,550 **(1.29x)** | 1,901,673 **(1.28x)** | 805,824 **(0.54x)** |
| **4** | 1,536,391 | 1,835,567 **(1.19x)** | 1,831,355 **(1.19x)** | 785,303 **(0.51x)** |
| **8** | 1,540,513 | 1,579,323 **(1.02x)** | 1,588,654 **(1.03x)** | 726,918 **(0.47x)** |

### Ring (hops/s)
*Measures message passing latency across a circular topology.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 1,619,735 | 2,553,508 **(1.58x)** | 2,483,812 **(1.53x)** | 2,937,427 **(1.81x)** |
| **2** | 1,513,880 | 1,821,430 **(1.20x)** | 1,826,997 **(1.21x)** | 1,866,505 **(1.23x)** |
| **4** | 1,493,600 | 1,861,899 **(1.25x)** | 1,871,631 **(1.25x)** | 1,830,381 **(1.23x)** |
| **8** | 1,455,217 | 1,546,837 **(1.06x)** | 1,635,970 **(1.12x)** | 1,642,735 **(1.13x)** |

### Selective receive / fan-out / fan-in (msg/s)
*Evaluates complex orchestration and selection logic.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 391,272 | 329,946 **(0.84x)** | 326,836 **(0.84x)** | 308,529 **(0.79x)** |
| **2** | 397,010 | 541,717 **(1.36x)** | 528,516 **(1.33x)** | 346,304 **(0.87x)** |
| **4** | 401,595 | 442,562 **(1.10x)** | 435,226 **(1.08x)** | 349,372 **(0.87x)** |
| **8** | 397,104 | 386,839 **(0.97x)** | 341,391 **(0.86x)** | 248,836 **(0.63x)** |

### Spawn idle tasks (tasks/s)
*Tests the efficiency of task creation and scheduling.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 144,932 | 15,047 **(0.10x)** | 16,057 **(0.11x)** | 711,577 **(4.91x)** |
| **2** | 236,451 | 21,023 **(0.09x)** | 22,022 **(0.09x)** | 773,872 **(3.27x)** |
| **4** | 299,832 | 19,522 **(0.07x)** | 20,636 **(0.07x)** | 720,400 **(2.40x)** |
| **8** | 286,965 | 18,827 **(0.07x)** | 18,710 **(0.07x)** | 490,309 **(1.71x)** |

### Prime sieve (primes/s)
*High-concurrency filtering test.*

| Pool | Go (Baseline) | libgoc | libgoc vmem | Clojure |
| :--- | :--- | :--- | :--- | :--- |
| **1** | 1,280 | 3,773 **(2.95x)** | 3,697 **(2.89x)** | 1,781 **(1.39x)** |
| **2** | 2,517 | 5,118 **(2.03x)** | 5,004 **(1.99x)** | 1,844 **(0.73x)** |
| **4** | 4,710 | 4,064 **(0.86x)** | 3,910 **(0.83x)** | 1,669 **(0.35x)** |
| **8** | 9,074 | 2,245 **(0.25x)** | 1,964 **(0.22x)** | 537 **(0.06x)** |

---

## Summary

### Go

Go's work-stealing scheduler keeps communicating goroutines on the same OS
thread naturally. Channel-bound workloads (ping-pong, ring) are flat across
pool sizes (~1.5 M round trips/s, ~1.5 M hops/s at all thread counts),
indicating the scheduler avoids cross-thread bouncing effectively.
CPU-bound pipelines scale well with thread count (prime sieve: 1,280 → 9,074
primes/s from pool=1 to pool=8). Spawn is moderate: goroutine creation is
cheap but GC and scheduler bookkeeping keep it below Clojure's IOC approach.

### libuv

#### canary

With the new work-stealing scheduler, libgoc canary significantly outperforms
the previous single-queue design — especially at multi-thread pool sizes.

**Ping-pong: libgoc canary beats Go at all pool sizes.** The work-stealing
deque allows communicating fiber pairs to stay on the same worker (the woken
fiber is pushed to the owner's own deque), achieving 1.70× Go at pool=1 and
staying ahead through pool=8 (1.02×). Previously, canary degraded to 0.35×
Go at pool=8; that problem is now gone.

**Ring: consistently 1.06–1.58× Go.** Work stealing keeps the ring's token
on the same worker, reducing cross-thread wakeups. At pool=1 libgoc reaches
2.55 M hops/s vs Go's 1.62 M.

**Fan-out/fan-in: 0.84–1.36× Go.** The 8-worker fan-out benefits from
true parallel execution; libgoc exceeds Go at pool=2–4 (1.36× and 1.10×)
as the work-stealing scheduler distributes workers naturally.

**Spawn idle tasks: ~0.07–0.10× Go (10× slower).** Unchanged bottleneck:
minicoro fibers use a fixed-size stack (64 KiB committed in canary mode)
with GC root tracking per fiber. Spawning 200 K fibers is substantially
heavier than 200 K goroutines regardless of the scheduling strategy.

**Prime sieve: 2.95× Go at pool=1, then degrades to 0.25× at pool=8.**
At pool=1 all fibers run cooperatively with zero cross-thread cost.
The serial pipeline structure cannot be parallelised, so adding threads
only adds scheduling overhead. Go scales the sieve by keeping its
goroutine scheduler's work-stealing tightly coupled to the CPU cache.

#### vmem

With the work-stealing scheduler, vmem and canary perform nearly identically
across all benchmarks — the dominant cost at pool > 1 is now cross-thread
wakeup latency, not GC scan overhead. vmem ping-pong reaches 2.58 M/s at
pool=1 (1.69× Go) and holds at 1.59 M/s at pool=8 (1.03× Go), matching
canary within measurement noise.

The old vmem degradation (e.g., 0.21× Go at pool=8 ping-pong in the
pre-work-stealing benchmarks) is fully resolved by the new scheduler.

### Clojure

**Ring: Clojure leads all implementations at pool=1 (1.81× Go, 1.15× libgoc).**
core.async go-blocks are IOC state machines dispatched onto a thread pool;
for the ring pattern, the JVM's thread pool dispatches continuations
efficiently at low contention.

**Ping-pong: 0.47–0.78× Go.** Channel round-trip overhead is higher than
both Go and libgoc due to JVM dispatch overhead; libgoc canary outperforms
Clojure by 2.1–3.6× across pool sizes.

**Fan-out/fan-in: 0.63–0.87× Go.** Consistent but below Go; libgoc canary
exceeds Clojure at pool=2–4 (1.57× and 1.27×).

**Spawn idle: 1.71–4.91× Go (fastest of all at pool=1–8).** core.async
go-blocks are cheap heap-allocated state machines with no fixed stack and
no per-task GC root scanning, making mass spawn far cheaper than either
goroutines or minicoro fibers.

**Prime sieve: 1.39× Go at pool=1, falls to 0.06× at pool=8.** IOC
continuations add per-hop overhead; the serial pipeline cannot parallelise.
Worse than both libgoc variants at pool=8.
