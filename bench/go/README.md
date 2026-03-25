# Go Benchmarks

Standalone CSP benchmarks implemented in Go.  All five benchmarks are enabled
and mirror those in `bench/libgoc/bench.c` for a direct performance comparison.

## Running

```sh
# Single run (current GOMAXPROCS)
make run

# Multi-pool testing — runs with GOMAXPROCS = 1, 2, 4, 8
make run-all
```

## Benchmarks

| # | Name | Description |
|---|------|-------------|
| 1 | **Channel ping-pong** | Two goroutines exchange a token back and forth |
| 2 | **Ring** | A token is forwarded around a ring of N goroutines |
| 3 | **Selective receive / fan-out / fan-in** | Producer → N workers → `reflect.Select` collector |
| 4 | **Spawn idle tasks** | Spawn N goroutines that block immediately, then wake them |
| 5 | **Prime sieve** | Concurrent Eratosthenes pipeline |

## Output Format

All benchmarks produce a single line per run:

```
<description>: <count> <unit> in <ms>ms (<rate> <unit>/s)
```

## Multi-Pool Testing

`make run-all` tests performance at different `GOMAXPROCS` settings:

```
=== Pool Size: 1 ===
GOMAXPROCS=1
Channel ping-pong: 200000 round trips in 81ms (2450068 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 208ms (2396744 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 301ms (663626 msg/s)
Spawn idle tasks: 200000 goroutines in 1238ms (161433 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1249ms (1810 primes/s)

=== Pool Size: 2 ===
GOMAXPROCS=2
Channel ping-pong: 200000 round trips in 87ms (2292035 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 217ms (2298805 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 280ms (712778 msg/s)
Spawn idle tasks: 200000 goroutines in 475ms (420510 tasks/s)
Prime sieve: 2262 primes up to 20000 in 549ms (4114 primes/s)

=== Pool Size: 4 ===
GOMAXPROCS=4
Channel ping-pong: 200000 round trips in 87ms (2277465 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 222ms (2250022 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 269ms (741545 msg/s)
Spawn idle tasks: 200000 goroutines in 358ms (557693 tasks/s)
Prime sieve: 2262 primes up to 20000 in 293ms (7694 primes/s)

=== Pool Size: 8 ===
GOMAXPROCS=8
Channel ping-pong: 200000 round trips in 87ms (2277539 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 221ms (2254470 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 267ms (747653 msg/s)
Spawn idle tasks: 200000 goroutines in 345ms (579313 tasks/s)
Prime sieve: 2262 primes up to 20000 in 159ms (14206 primes/s)
```
