# Benchmarks

This directory contains standalone CSP benchmarks implemented in Go and in C using libgoc. Benchmarks are kept separate from the main libgoc build and are not part of CI/CD.

## Benchmarks

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
