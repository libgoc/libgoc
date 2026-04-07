# Go Benchmarks

Standalone CSP benchmarks implemented in Go.  All seven benchmarks are enabled
and mirror those in `bench/libgoc/bench.c` for a direct performance comparison.

## Running

```sh
# Single run (current GOMAXPROCS)
make run

# Multi-pool testing — runs with GOMAXPROCS = 1, 2, 4, 8
make run all=1
```

## Benchmarks

| # | Name | Description |
|---|------|-------------|
| 1 | **Channel ping-pong** | Two goroutines exchange a token back and forth |
| 2 | **Ring** | A token is forwarded around a ring of N goroutines |
| 3 | **Selective receive / fan-out / fan-in** | Producer → N workers → `reflect.Select` collector |
| 4 | **Spawn idle tasks** | Spawn N goroutines that block immediately, then wake them |
| 5 | **Prime sieve** | Concurrent Eratosthenes pipeline |
| 6 | **HTTP ping-pong** | Two HTTP/1.1 servers bounce a counter between each other |
| 7 | **HTTP server throughput** | One server serves plaintext while concurrent keep-alive clients load it |

## Output Format

All benchmarks produce a single line per run:

```
<description>: <count> <unit> in <ms>ms (<rate> <unit>/s)
```

## Multi-Pool Testing

`make run all=1` tests performance at different `GOMAXPROCS` settings (GOMAXPROCS = 1, 2, 4, 8).

3 runs of this benchmark can be found in [bench/logs/go.log](../logs/go.log).
