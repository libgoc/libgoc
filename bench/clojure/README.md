# Clojure core.async Benchmarks

Standalone CSP benchmarks implemented in Clojure using
[`core.async`](https://github.com/clojure/core.async).  All five benchmarks
mirror those in `bench/go/main.go` and `bench/libgoc/bench.c` for a direct
three-way performance comparison.

## Prerequisites

- [Clojure CLI tools](https://clojure.org/guides/install_clojure) (`clojure`)

## Running

```sh
# Single run (default pool size = 8)
make run

# Multi-pool testing — runs with CLOJURE_POOL_THREADS = 1, 2, 4, 8
make run-all

# Single run with an explicit pool size
make run POOL_SIZE=4
```

The `POOL_SIZE` variable sets the JVM system property
`clojure.core.async.pool-size`, which controls the number of threads in
`core.async`'s go-block dispatch thread pool.

## Benchmarks

| # | Name | Description |
|---|------|-------------|
| 1 | **Channel ping-pong** | Two go-blocks exchange a token back and forth |
| 2 | **Ring** | A token is forwarded around a ring of N go-blocks |
| 3 | **Selective receive / fan-out / fan-in** | Producer → N workers → `alts!` collector |
| 4 | **Spawn idle tasks** | Spawn N go-blocks that park immediately, then wake them |
| 5 | **Prime sieve** | Concurrent Eratosthenes pipeline |

## Output Format

All benchmarks produce a single line per run:

```
<description>: <count> <unit> in <ms>ms (<rate> <unit>/s)
```

## Notes on core.async semantics

- `go` blocks are **not** OS threads or green threads — they are
  state-machine continuations dispatched onto a fixed-size thread pool.
  `POOL_SIZE` controls how many OS threads run these continuations in
  parallel (analogous to `GOMAXPROCS` / `GOC_POOL_THREADS`).
- `alts!` is used in the fan-in benchmark to select across multiple input
  channels, directly mirroring `reflect.Select` (Go) and `goc_alts` (libgoc).
