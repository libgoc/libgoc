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
make -C go run
```

### libgoc
```sh
make -C libgoc run
```

## Report

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

### Go

```sh
$ make -C go run
go run .
GOMAXPROCS=16
Channel ping-pong: 200000 round trips in 144.297139ms (1386029 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 368.671869ms (1356220 hops/s)
Selective receive / fan-out / fan-in: 200000 messages with 8 workers in 479.445303ms (417149 msg/s)
Spawn idle tasks: 200000 goroutines in 770.926955ms (259428 tasks/s)
Prime sieve: 2262 primes up to 20000 in 171.285332ms (13206 primes/s)
```

### libgoc

```sh
$ GOC_POOL_THREADS=1 make -C libgoc run
./bench-libgoc
Channel ping-pong: 200000 round trips in 0.048s (4177962 round trips/s)
Ring benchmark: 500000 hops across 128 tasks in 0.341s (1464775 hops/s)
```