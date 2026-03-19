# Benchmarks

This directory contains standalone CSP benchmarks implemented in Go and in C using libgoc.

1. **Channel ping-pong** — Two tasks pass a single message back and forth, measuring the basic cost of a send/receive and the context switches it causes.
2. **Ring benchmark** — Many tasks are arranged in a circle and pass a token around, stressing scheduling and handoff overhead across a larger group.
3. **Selective receive / fan-out / fan-in** — One producer feeds many workers, and a collector selects across multiple output channels, stressing select logic and load distribution.
4. **Spawning many idle tasks** — Create lots of tasks that immediately block, highlighting creation time and memory overhead for lightweight tasks.
5. **Prime sieve** — A pipeline of filters passes numbers through channels to find primes, stressing long chains of tasks and sustained channel traffic.

## Running

From this directory:

### Go

```sh
# Single run (uses current GOMAXPROCS)
make -C go run

# Multi-pool testing (runs with pool sizes 1, 2, 4, 8)
make -C go run-all
```

### libgoc
```sh
# Single run (uses current GOC_POOL_THREADS)
make -C libgoc run

# Multi-pool testing (runs with pool sizes 1, 2, 4, 8)
make -C libgoc run-all
```

## Output Format

Both implementations now use consistent integer millisecond formatting:
- **Time**: Integer milliseconds (e.g., `234ms`, `1567ms`)
- **Rates**: Floating-point operations per second (e.g., `1234567 ops/s`)
- **Organization**: Clear section headers for multi-pool runs

## Benchmark Notes

- **Go**: All 5 benchmarks are enabled and functional
- **libgoc**: All 5 benchmarks are enabled. The spawn-idle and prime-sieve defaults
  are lower on macOS (20k spawns / 10k primes) to avoid per-fiber stack mapping
  limits. Override with `GOC_BENCH_SPAWN_COUNT` or `GOC_BENCH_PRIME_MAX` if you want
  to push higher.

## Runs

### Benchmark Environment

| Property        | Value                          |
|-----------------|--------------------------------|
| **CPU**         | AMD Ryzen 7 5800H              |
| **Cores**       | 8 cores / 16 threads (SMT)     |
| **Max Clock**   | 4463 MHz                       |
| **L1d / L1i**   | 256 KiB each (per core)        |
| **L2 Cache**    | 4 MiB (per core)               |
| **L3 Cache**    | 16 MiB (shared)                |
| **RAM**         | 13 GiB                         |
| **OS**          | Ubuntu 24.04.4 LTS             |
| **Kernel**      | Linux 6.11.0 x86_64            |

### Go (make run-all)

```
=== Pool Size: 1 ===
GOMAXPROCS=1
Channel ping-pong: 200000 round trips in 87ms (2280645 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 222ms (2243222 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 333ms (599056 msg/s)
Spawn idle tasks: 200000 goroutines in 1062ms (188282 tasks/s)
Prime sieve: 2262 primes up to 20000 in 1178ms (1919 primes/s)

=== Pool Size: 2 ===
GOMAXPROCS=2
Channel ping-pong: 200000 round trips in 89ms (2224597 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 218ms (2284381 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 307ms (650773 msg/s)
Spawn idle tasks: 200000 goroutines in 570ms (350786 tasks/s)
Prime sieve: 2262 primes up to 20000 in 570ms (3962 primes/s)

=== Pool Size: 4 ===
GOMAXPROCS=4
Channel ping-pong: 200000 round trips in 89ms (2228437 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 223ms (2240562 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 302ms (661967 msg/s)
Spawn idle tasks: 200000 goroutines in 480ms (416456 tasks/s)
Prime sieve: 2262 primes up to 20000 in 295ms (7647 primes/s)

=== Pool Size: 8 ===
GOMAXPROCS=8
Channel ping-pong: 200000 round trips in 88ms (2257564 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 222ms (2250942 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 304ms (657846 msg/s)
Spawn idle tasks: 200000 goroutines in 406ms (492388 tasks/s)
Prime sieve: 2262 primes up to 20000 in 160ms (14136 primes/s)
```

### libgoc (make run-all)

Output now includes all five benchmarks; the snippet below is truncated for brevity.

```
=== Pool Size: 1 ===
GOC_POOL_THREADS=1
Channel ping-pong: 200000 round trips in 89ms (2244633 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 656ms (761308 hops/s)

=== Pool Size: 2 ===
GOC_POOL_THREADS=2
Channel ping-pong: 200000 round trips in 253ms (788650 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 1325ms (377239 hops/s)

=== Pool Size: 4 ===
GOC_POOL_THREADS=4
Channel ping-pong: 200000 round trips in 384ms (519721 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 1639ms (305005 hops/s)

=== Pool Size: 8 ===
GOC_POOL_THREADS=8
Channel ping-pong: 200000 round trips in 320ms (624659 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 1019ms (490455 hops/s)
```

## Report

### Chart

### Summary
