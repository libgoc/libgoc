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

### canary mode (default — fixed stacks with stack-overflow detection)

```sh
# Build
make build

# Single run
make run

# Multi-pool testing (GOC_POOL_THREADS = 1, 2, 4, 8)
make run-all
```

### vmem mode (virtual-memory-backed stacks — opt-in)

```sh
# Build and run benchmarks against a vmem libgoc build
make LIBGOC_VMEM=ON BUILD_DIR=../../build-bench-vmem build run-all
```

In all builds, libgoc now throttles the number of simultaneously materialised
fibers per pool by default (`GOC_MAX_LIVE_FIBERS`, default
`floor(0.7 × (available_hardware_memory / fiber_stack_size)
  × (pool_threads / hardware_threads))`). The `0.7` factor keeps roughly
30% headroom for GC/runtime overhead while still pushing
high throughput. Set `GOC_MAX_LIVE_FIBERS=0` to disable the throttle, or pick
an explicit positive cap for repeatable stress testing.

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

Example (canary mode, pool=1):

```
Channel ping-pong: 200000 round trips in 197ms (1010144 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 1170ms (427214 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 727ms (274870 msg/s)
Spawn idle tasks: 200000 fibers in 4915ms (40688 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1312ms (1724 primes/s)
```

## Multi-Pool Testing

`make run-all` tests performance at different `GOC_POOL_THREADS` settings.

### canary mode (default — `LIBGOC_VMEM=OFF`)

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

### vmem mode (`-DLIBGOC_VMEM=ON`)

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

With the current memory-and-pool-derived admission cap defaults, this run shows:

- **Canary** is strongest at low-to-mid pools, including wins over Go in
  ping-pong at pool=2/4 and fan-out/fan-in at pool=2. Ring and prime sieve
  remain below Go at higher pool sizes.
- **vmem** remains significantly slower than canary on ring/fan-out/prime, but
  improved materially on spawn-idle versus prior runs at pool=1/2, reflecting
  lower creation pressure under the updated admission defaults.
- Overall, vmem still behaves as a bounded-correctness/stress configuration
  rather than a near-parity performance mode.
