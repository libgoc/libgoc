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
Channel ping-pong: 200000 round trips in 224ms (892022 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 1398ms (357644 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1436ms (139226 msg/s)
Spawn idle tasks: 200000 fibers in 2374ms (84244 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1597ms (1416 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 219ms (912402 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 1256ms (397976 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 1443ms (138512 msg/s)
Spawn idle tasks: 200000 fibers in 2350ms (85079 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1167ms (1938 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 111ms (1788707 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 698ms (716300 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 773ms (258405 msg/s)
Spawn idle tasks: 200000 fibers in 1392ms (143669 tasks/s)
Prime sieve: 2262 primes up to 20000 in 801ms (2821 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 88ms (2248271 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 495ms (1008686 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 840ms (238041 msg/s)
Spawn idle tasks: 200000 fibers in 1203ms (166231 tasks/s)
Prime sieve: 2262 primes up to 20000 in 877ms (2577 primes/s)
```

### vmem mode (`-DLIBGOC_VMEM=ON`)

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 739ms (270336 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 18645ms (26816 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 808ms (247494 msg/s)
Spawn idle tasks: 200000 fibers in 3041ms (65766 tasks/s)
Prime sieve: 2262 primes up to 20000 in 2790ms (811 primes/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 337ms (592109 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 13771ms (36308 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 785ms (254468 msg/s)
Spawn idle tasks: 200000 fibers in 11263ms (17756 tasks/s)
Prime sieve: 2262 primes up to 20000 in 2374ms (952 primes/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 283ms (706642 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 14543ms (34380 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 733ms (272542 msg/s)
Spawn idle tasks: 200000 fibers in 3064ms (65263 tasks/s)
Prime sieve: 2262 primes up to 20000 in 3184ms (710 primes/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 225ms (886815 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 10013ms (49935 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 652ms (306298 msg/s)
Spawn idle tasks: 200000 fibers in 3156ms (63358 tasks/s)
Prime sieve: 2262 primes up to 20000 in 3141ms (720 primes/s)
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
