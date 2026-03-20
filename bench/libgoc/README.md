# libgoc Benchmarks

Standalone CSP benchmarks implemented in C using libgoc.

Built separately from the main libgoc library; no changes to the main CMake
build are required to run them.

## Prerequisites

- C11 compiler (GCC or Clang)
- CMake ≥ 3.20
- pkg-config
- libuv
- Threaded Boehm GC (`bdw-gc-threaded`)

## Building and Running

### vmem mode (default — virtual-memory-backed stacks)

```sh
# Build
make build

# Single run
make run

# Multi-pool testing (GOC_POOL_THREADS = 1, 2, 4, 8)
make run-all
```

### canary mode (fixed stacks with stack-overflow detection)

```sh
# Rebuild libgoc with -DLIBGOC_VMEM=OFF, then run
cmake -S ../.. -B ../../build-bench -DCMAKE_BUILD_TYPE=Release -DLIBGOC_VMEM=OFF
cmake --build ../../build-bench --target goc
make build run-all
```

## Benchmarks

All five benchmarks are **implemented and enabled** in `bench.c`.

| # | Name | Status |
|---|------|--------|
| 1 | **Channel ping-pong** — two fibers exchange a token back and forth | ✅ enabled |
| 2 | **Ring** — a token is forwarded around a ring of N fibers | ✅ enabled |
| 3 | **Selective receive / fan-out / fan-in** — producer → N workers → `goc_alts` collector | ✅ enabled |
| 4 | **Spawn idle tasks** — spawn many fibers that park immediately, then wake them | ✅ enabled |
| 5 | **Prime sieve** — concurrent Eratosthenes pipeline | ✅ enabled |

## Output Format

All benchmarks produce a single line per run:

```
<description>: <count> <unit> in <ms>ms (<rate> <unit>/s)
```

Example (vmem mode, pool=1):

```
Channel ping-pong: 200000 round trips in 85ms (2343156 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 222ms (2248408 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 642ms (311113 msg/s)
Spawn idle tasks: 200000 fibers in 10334ms (19353 tasks/s)
Prime sieve: 2262 primes up to 20000 in 952ms (2375 primes/s)
```

## Multi-Pool Testing

`make run-all` tests performance at different `GOC_POOL_THREADS` settings.

### vmem mode (`-DLIBGOC_VMEM=ON`, default)

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

### canary mode (`-DLIBGOC_VMEM=OFF`)

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

Note that pool=1 typically shows the best channel throughput for libgoc because
all fibers run cooperatively on one OS thread with no cross-thread wakeup cost.
Canary mode outperforms vmem at pool=4 and pool=8 because fixed-size stacks
have lower GC scan overhead than virtual-memory-backed stacks when many fibers
are suspended simultaneously.
