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
fibers per pool by default (`GOC_MAX_LIVE_FIBERS`, default `max(256, 64 ×
GOC_POOL_THREADS)`). This keeps mass-spawn benchmarks from materialising an
unbounded number of fibers at once. Set `GOC_MAX_LIVE_FIBERS=0` to disable the
throttle, or pick an explicit positive cap for repeatable stress testing.

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

### vmem mode (`-DLIBGOC_VMEM=ON`)

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

The current bounded-admission implementation changes the performance story
substantially:

- **Canary** now performs best at pool=2 on most benchmarks. The live-fiber
	cap improves `spawn idle` throughput by preventing all 200k fibers from
	materialising at once, but it also leaves `ring` and `prime sieve` well below
	the older work-stealing-only numbers.
- **vmem** is no longer close to canary. It is dramatically slower on `ring`,
	`fan-out/fan-in`, and `prime sieve`, and should currently be treated as a
	correctness/stress configuration rather than a near-parity performance mode.
