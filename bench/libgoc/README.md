# libgoc Benchmarks

Standalone CSP benchmarks implemented in C using libgoc.

Built separately from the main libgoc library; no changes to the main CMake
build are required to run them.

## Prerequisites

- C11 compiler (GCC or Clang)
- CMake ≥ 3.20
- pkg-config
- libuv (static libraries available)
- Threaded Boehm GC (`bdw-gc-threaded`, static libraries available)

`bench-libgoc` is built to link statically against `libuv` and Boehm GC, so the benchmark requires the static archives for those dependencies to be available to `pkg-config`.

## Building and Running

### canary mode (default — fixed stacks with stack-overflow detection)

```sh
# Build
make build

# Single run
make run

# Multi-pool testing (GOC_POOL_THREADS = 1, 2, 4, 8)
make run all=1
```

### vmem mode (virtual-memory-backed stacks — opt-in)

```sh
# Build and run benchmarks against a vmem libgoc build
make vmem=1 BUILD_DIR=../../build-bench-vmem build run all=1
```

## Benchmarks

All seven benchmarks are **implemented and enabled** in `bench.c`.

| # | Name | Status |
|---|------|--------|
| 1 | **Channel ping-pong** — two fibers exchange a token back and forth | ✅ enabled |
| 2 | **Ring** — a token is forwarded around a ring of N fibers | ✅ enabled |
| 3 | **Selective receive / fan-out / fan-in** — producer → N workers → `goc_alts` collector | ✅ enabled |
| 4 | **Spawn idle tasks** — spawn many fibers that park immediately, then wake them | ✅ enabled |
| 5 | **Prime sieve** — concurrent Eratosthenes pipeline | ✅ enabled |
| 6 | **HTTP ping-pong** — two HTTP/1.1 servers bounce a counter back and forth | ✅ enabled |
| 7 | **HTTP server throughput** — one server serves plaintext under keep-alive client load | ✅ enabled |

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
HTTP ping-pong: <rounds> round trips in <ms>ms (<rate> round trips/s, avg <us> p50 <us> p95 <us> p99 <us>, warmup <rounds>)
HTTP server throughput: <requests> requests in <ms>ms (<rate> req/s, <errors> errors, concurrency <n>, warmup <ms>ms)
```

## Multi-Pool Testing

`make run all=1` tests performance at different `GOC_POOL_THREADS` settings (1, 2, 4, 8).

3 runs of canary benchmarks can be found in [bench/logs/canary.log](../logs/canary.log).

3 runs of vmem benchmarks can be found in [bench/logs/vmem.log](../logs/vmem.log).
