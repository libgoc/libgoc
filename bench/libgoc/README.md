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

# Enable runtime instrumentation counters
make LIBGOC_STATS=ON run-all
```

Runtime instrumentation counters are **OFF by default** in benchmark builds
(`LIBGOC_STATS=OFF`). Enable them explicitly with `LIBGOC_STATS=ON` when you
want per-benchmark stats lines (scan lengths, compaction, callback queue,
timeout, and scheduler attempts/successes).

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

Example (canary mode, pool=1):

```
Channel ping-pong: 200000 round trips in 115ms (1728969 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 688ms (726077 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 420ms (475822 msg/s)
Spawn idle tasks: 200000 fibers in 13692ms (14607 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1059ms (2136 primes/s)
```

## Multi-Pool Testing

`make run-all` tests performance at different `GOC_POOL_THREADS` settings.

### canary mode (default — `LIBGOC_VMEM=OFF`)

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 114ms (1743937 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 667ms (748901 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 410ms (487486 msg/s)
Spawn idle tasks: 200000 fibers in 11060ms (18082 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1089ms (2077 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 113ms (1768442 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 689ms (725162 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 457ms (436805 msg/s)
Spawn idle tasks: 200000 fibers in 11308ms (17687 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1070ms (2113 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 97ms (2055682 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 692ms (721671 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 481ms (415434 msg/s)
Spawn idle tasks: 200000 fibers in 11047ms (18104 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1130ms (2002 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 105ms (1887001 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 540ms (924756 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 642ms (311521 msg/s)
Spawn idle tasks: 200000 fibers in 11024ms (18141 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1150ms (1965 primes/s)
```

### vmem mode (`-DLIBGOC_VMEM=ON`)

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 275ms (725450 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 11381ms (43933 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 892ms (224167 msg/s)
Spawn idle tasks: 200000 fibers in 13477ms (14840 tasks/s)
Prime sieve: 2262 primes up to 20000 in 6619ms (342 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 280ms (713797 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 12032ms (41554 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1050ms (190303 msg/s)
Spawn idle tasks: 200000 fibers in 13100ms (15267 tasks/s)
Prime sieve: 2262 primes up to 20000 in 7859ms (288 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 217ms (920515 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 12595ms (39696 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1304ms (153290 msg/s)
Spawn idle tasks: 200000 fibers in 13440ms (14880 tasks/s)
Prime sieve: 2262 primes up to 20000 in 7833ms (289 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 246ms (811155 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 9375ms (53329 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1778ms (112439 msg/s)
Spawn idle tasks: 200000 fibers in 13808ms (14484 tasks/s)
Prime sieve: 2262 primes up to 20000 in 7720ms (293 primes/s)
```

With the current memory-derived admission cap defaults (`GOC_DEFAULT_LIVE_FIBER_MEMORY_FACTOR = 0.6`), this run shows:

- **Canary** is competitive with Go on ping-pong (0.72–0.92×), peaking at
  2.1 M round trips/s at pool=4. Spawn idle tasks are notably faster than
  the previous run (18k vs 14k tasks/s) due to the wspool fix reducing
  overhead. Ring throughput improves to 0.47× Go at pool=8.
- **vmem** shows similar ping-pong (0.30–0.41× Go) and ring/sieve numbers.
  The admission cap prevents memory exhaustion from materialising too many
  mmap stacks simultaneously.
- vmem still behaves as a bounded-correctness/stress configuration rather than
  a near-parity performance mode.
