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
`floor(0.6 × (available_hardware_memory / fiber_stack_size))`).
The `0.6` factor keeps roughly
40% headroom for GC/runtime overhead while still pushing
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

Example (canary mode, pool=8):

```
Channel ping-pong: 200000 round trips in 88ms (2248271 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 495ms (1008686 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 840ms (238041 msg/s)
Spawn idle tasks: 200000 fibers in 1203ms (166231 tasks/s)
Prime sieve: 2262 primes up to 20000 in 877ms (2577 primes/s)
```

## Multi-Pool Testing

`make run-all` tests performance at different `GOC_POOL_THREADS` settings.

### canary mode (default — `LIBGOC_VMEM=OFF`)

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 128ms (1558221 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 653ms (764691 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 759ms (263350 msg/s)
Spawn idle tasks: 200000 fibers in 1518ms (131733 tasks/s)
Prime sieve: 2262 primes up to 20000 in 830ms (2722 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 131ms (1523077 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 649ms (770295 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 967ms (206796 msg/s)
Spawn idle tasks: 200000 fibers in 1286ms (155421 tasks/s)
Prime sieve: 2262 primes up to 20000 in 887ms (2549 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 116ms (1715892 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 680ms (734346 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 862ms (231845 msg/s)
Spawn idle tasks: 200000 fibers in 1309ms (152715 tasks/s)
Prime sieve: 2262 primes up to 20000 in 883ms (2560 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 98ms (2021993 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 497ms (1004947 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 867ms (230415 msg/s)
Spawn idle tasks: 200000 fibers in 1322ms (151212 tasks/s)
Prime sieve: 2262 primes up to 20000 in 893ms (2531 primes/s)
```

### vmem mode (`-DLIBGOC_VMEM=ON`)

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 356ms (560870 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 14388ms (34750 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 881ms (226977 msg/s)
Spawn idle tasks: 200000 fibers in 3189ms (62697 tasks/s)
Prime sieve: 2262 primes up to 20000 in 3553ms (637 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 442ms (452244 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 12795ms (39076 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1009ms (198157 msg/s)
Spawn idle tasks: 200000 fibers in 3227ms (61968 tasks/s)
Prime sieve: 2262 primes up to 20000 in 3219ms (703 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 301ms (663829 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 14892ms (33574 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 735ms (271943 msg/s)
Spawn idle tasks: 200000 fibers in 3112ms (64251 tasks/s)
Prime sieve: 2262 primes up to 20000 in 3691ms (613 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 221ms (903838 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 10420ms (47982 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 839ms (238363 msg/s)
Spawn idle tasks: 200000 fibers in 10663ms (18756 tasks/s)
Prime sieve: 2262 primes up to 20000 in 2447ms (924 primes/s)
```

With the current memory-derived admission cap defaults (`GOC_DEFAULT_LIVE_FIBER_MEMORY_FACTOR = 0.6`), this run shows:

- **Canary** scales strongly with pool size: ping-pong reaches 2.25 M round
  trips/s at pool=8 (0.98× Go), and ring throughput crosses 1 M hops/s
  (0.44× Go). Spawn idle tasks improved dramatically (84–166k tasks/s vs
  the previous ~18k), reflecting scheduler improvements.
- **vmem** ping-pong reaches 887k round trips/s at pool=8 (0.39× Go).
  Ring and sieve remain costly due to TLB/page-fault pressure from
  mmap-backed stacks. The admission cap prevents memory exhaustion.
- vmem still behaves as a bounded-correctness/stress configuration rather than
  a near-parity performance mode.
