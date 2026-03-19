# libgoc Benchmarks

Standalone CSP benchmarks implemented in C using libgoc for performance comparison with Go.

These benchmarks are built separately from the main libgoc library build.

## Prerequisites

- C11 compiler
- CMake  
- pkg-config
- libuv
- Threaded Boehm GC (bdw-gc-threaded)

## Running

```sh
# Single run (uses current GOC_POOL_THREADS setting)
make run

# Multi-pool testing (runs with pool sizes 1, 2, 4, 8)
make run-all
```

## Benchmarks Included

1. **Channel ping-pong** — Two fibers pass a message back and forth
2. **Ring benchmark** — Token passing around a ring of fibers
3. **Selective receive / fan-out / fan-in** — Producer-consumer with select
4. **Spawn idle tasks** — Fiber creation and join overhead
5. **Prime sieve** — Concurrent prime pipeline

The spawn-idle benchmark defaults to 50,000 fibers on Linux (20,000 on macOS) to
avoid exhausting per-fiber stack mappings. The prime sieve default is 20,000 on
Linux (10,000 on macOS) for the same reason. Override them with:

```sh
GOC_BENCH_SPAWN_COUNT=200000 make run
GOC_BENCH_PRIME_MAX=20000 make run
```

You can also tune the fan-out/fan-in workload:

```sh
GOC_BENCH_SELECT_WORKERS=8 GOC_BENCH_SELECT_TASKS=200000 make run
```

## Output Format

- **Time**: Integer milliseconds (e.g., `234ms`)  
- **Rates**: Operations per second (e.g., `1234567 ops/s`)

## Multi-Pool Testing

The `make run-all` command tests performance with different `GOC_POOL_THREADS` settings:

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 89ms (2244633 round trips/s)
...

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 253ms (788650 round trips/s)
...
```

This helps analyze how libgoc's fiber scheduler performs with different thread pool sizes.
