# Benchmarks

This directory contains standalone CSP benchmarks implemented in Go and in C
using libgoc.

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

## Benchmark Status

| # | Benchmark | Go | libgoc |
|---|-----------|:--:|:------:|
| 1 | Channel ping-pong | ✅ | ✅ |
| 2 | Ring | ✅ | ✅ |
| 3 | Selective receive / fan-out / fan-in | ✅ | ✅ |
<<<<<<< divs1210/issue19
| 4 | Spawn idle tasks | ✅ | ✅ |
| 5 | Prime sieve | ✅ | ✅ |
=======
| 4 | Spawn idle tasks | ✅ | 🚧 |
| 5 | Prime sieve | ✅ | 🚧 |

🚧 — Implemented in `bench/libgoc/bench.c` but disabled in `main()`.
>>>>>>> main

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

### libgoc canary — (default) — (`make run-all`)

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 84ms (2353892 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 215ms (2322813 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 637ms (313967 msg/s)
Spawn idle tasks: 200000 fibers in 10885ms (18373 tasks/s)
Prime sieve: 2262 primes up to 20000 in 749ms (3017 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 105ms (1892095 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 297ms (1678839 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 437ms (456951 msg/s)
Spawn idle tasks: 200000 fibers in 8053ms (24834 tasks/s)
Prime sieve: 2262 primes up to 20000 in 648ms (3486 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 151ms (1319020 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 502ms (995646 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 456ms (437770 msg/s)
Spawn idle tasks: 200000 fibers in 8836ms (22633 tasks/s)
Prime sieve: 2262 primes up to 20000 in 747ms (3026 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 252ms (790813 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 439ms (1138495 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 562ms (355597 msg/s)
Spawn idle tasks: 200000 fibers in 9996ms (20007 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1561ms (1449 primes/s)
```
<<<<<<< divs1210/issue19

## Report

All numbers are operations per second (higher is better).

### Channel ping-pong (round trips/s)

```
Pool  Go           libgoc vmem  libgoc canary
----  -----------  -----------  -------------
   1  2,280,645    2,343,156    2,353,892
   2  2,224,597    1,895,838    1,892,095
   4  2,228,437      599,752    1,319,020
   8  2,257,564      464,624      790,813
=======
Benchmark           Pool  Go (ops/s)   libgoc (ops/s)  Ratio (libgoc/Go)
------------------  ----  -----------  --------------  -----------------
Channel ping-pong      1  2,280,645      1,658,644          0.73×
Channel ping-pong      2  2,224,597        679,017          0.31×
Channel ping-pong      4  2,228,437        448,940          0.20×
Channel ping-pong      8  2,257,564        339,584          0.15×
>>>>>>> main
```

### Ring (hops/s)

```
Pool  Go           libgoc vmem  libgoc canary
----  -----------  -----------  -------------
   1  2,243,222    2,248,408    2,322,813
   2  2,284,381    1,657,377    1,678,839
   4  2,240,562      529,457      995,646
   8  2,250,942      618,487    1,138,495
```

### Selective receive / fan-out / fan-in (msg/s)

```
Pool  Go        libgoc vmem  libgoc canary
----  --------  -----------  -------------
   1   599,056      311,113        313,967
   2   650,773      454,532        456,951
   4   661,967      365,187        437,770
   8   657,846      214,316        355,597
```

### Spawn idle tasks (tasks/s)

```
Pool  Go        libgoc vmem  libgoc canary
----  --------  -----------  -------------
   1   188,282       19,353         18,373
   2   350,786       25,190         24,834
   4   416,456       18,999         22,633
   8   492,388       15,590         20,007
```

### Prime sieve (primes/s)

```
Pool  Go        libgoc vmem  libgoc canary
----  --------  -----------  -------------
   1    1,919        2,375          3,017
   2    3,962        1,370          3,486
   4    7,647        1,342          3,026
   8   14,136        1,431          1,449
```

## Summary

**At pool=1, libgoc matches Go for channel throughput.**  Both vmem and canary
modes reach ~2.3–2.35 M round trips/s (ping-pong) and ~2.25–2.32 M hops/s
(ring) vs Go's ~2.28 M and ~2.24 M respectively — essentially identical.
With a single pool thread, all fibers run on the same OS thread, so there
are no cross-thread wakeups and the only overhead is the `GC_set_stackbottom`
redirect on each `mco_resume`.

**At pool > 1, vmem degrades significantly; canary holds up better.**  As
pool threads increase, cross-thread wakeups begin to dominate.  Vmem stacks
also trigger higher GC scan overhead (each suspended fiber's committed pages
must be scanned), so at pool=4 vmem ping-pong falls to 600 K/s while canary
stays at 1.3 M/s (2.2× better).  At pool=8 the gap narrows (465 K vs 791 K,
1.7× canary advantage) as wakeup latency becomes the dominant cost for both.

**Fan-in scales less adversely.**  The fan-out/fan-in pattern has 8 workers
running in true parallel, so extra threads help distribute the sender load.
Both modes are within ~15% of each other and degrade more slowly than
ping-pong or ring.

**Spawn idle — Go's goroutine model is ~10–25× faster.**  Go goroutines have
a ~2–4 KiB initial stack that grows automatically; minicoro fibers use a
fixed-size or vmem-backed stack (default 2 MiB virtual, ~136 bytes initially
committed in vmem mode; 64 KiB fully committed in canary mode).  The GC must
track each fiber's root set, making 200 K fiber creation a substantially
heavier operation than 200 K goroutine creation.

**Prime sieve at pool=1: libgoc is faster than Go.**  The sieve is a deep
pipeline of N small fibers passing single values; at pool=1 the fibers run
cooperatively with zero synchronization overhead.  Canary mode (3017/s) edges
vmem (2375/s) because fixed stacks have lower GC scan cost per fiber.  Go
scales linearly with more threads (14 K/s at pool=8) while libgoc does not,
because the long serial pipeline cannot be parallelised without restructuring.

**Go scalability.**  Go's work-stealing scheduler keeps communicating
goroutines on the same thread naturally and scales CPU-bound pipelines
(spawn: 188 K → 492 K/s; prime sieve: 1919 → 14136/s from pool=1 to pool=8)
while communication-bound workloads (ping-pong, ring) remain flat.
libgoc's current pool scheduler lacks work-stealing; this is a known area
of improvement tracked in `TODO.md`.
