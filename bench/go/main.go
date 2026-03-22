/*
Package main contains standalone CSP concurrency benchmarks for Go.

Five micro-benchmarks exercise different aspects of Go's goroutine scheduler
and channel primitives.  The benchmarks mirror those in bench/libgoc/bench.c
so that Go and libgoc results can be compared directly.

Benchmarks
  1. Channel ping-pong — two goroutines exchange a single token back and forth,
     measuring the raw cost of one send + one receive.
  2. Ring — many goroutines in a circle each forward a counter token to the next
     node, stressing scheduling throughput across a larger group.
  3. Selective receive / fan-out / fan-in — one producer fans work out to N
     worker goroutines; a collector uses reflect.Select to receive from all N
     output channels, stressing select logic and load distribution.
  4. Spawn idle tasks — spawn a large number of goroutines that block immediately
     on a shared channel, then wake them all; measures creation and wakeup cost.
  5. Prime sieve — a concurrent pipeline where each stage filters multiples of a
     discovered prime, stressing long chains of goroutines and sustained traffic.

Usage:
  go run .                    # single run (current GOMAXPROCS)
  GOMAXPROCS=4 go run .       # single run with explicit parallelism
  make run-all                # run with GOMAXPROCS = 1, 2, 4, 8
*/
package main

import (
	"fmt"
	"reflect"
	"sync"
	"time"
)

func main() {
	pingRounds    := 200000
	ringNodes     := 128
	ringHops      := 500000
	selectWorkers := 8
	selectTasks   := 200000
	spawnCount    := 200000
	primeMax      := 20000

	pingPong(pingRounds)
	ringBenchmark(ringNodes, ringHops)
	fanInBenchmark(selectWorkers, selectTasks)
	spawnIdle(spawnCount)
	primeSieve(primeMax)
}


// ============================================================================
// 1. Channel ping-pong
// ============================================================================

// pingPong runs a two-goroutine ping-pong for the given number of round trips.
//
// Two goroutines share a pair of unbuffered channels (a ↔ b).  One goroutine
// starts by receiving on b; the other is kicked off when main places 0 on a.
// Each player receives a counter, and either closes the forward channel
// (if the round limit is reached) or increments the counter and sends it back.
//
// Total channel operations: 2 × rounds (one send + one receive per trip).
func pingPong(rounds int) {
	a := make(chan int)
	b := make(chan int)

	var wg sync.WaitGroup
	wg.Add(2)

	// player loops until it receives a value ≥ rounds, then closes the
	// forward channel to signal its partner to stop.
	player := func(recv <-chan int, send chan<- int) {
		defer wg.Done()
		for v := range recv {
			if v >= rounds {
				close(send)
				return
			}
			send <- v + 1
		}
	}

	go player(b, a)
	go player(a, b)

	start := time.Now()
	a <- 0
	wg.Wait()
	duration := time.Since(start)
	ms := int(duration.Nanoseconds() / 1_000_000)

	rate := float64(rounds) / duration.Seconds()
	fmt.Printf("Channel ping-pong: %d round trips in %dms (%.0f round trips/s)\n", rounds, ms, rate)
}


// ============================================================================
// 2. Ring benchmark
// ============================================================================

// ringBenchmark runs a token-passing ring of goroutines.
//
// nodes goroutines are arranged in a circle; each reads from channels[i] and
// forwards to channels[(i+1)%nodes].  A counter token is placed on channels[0]
// with value hops.  Each goroutine decrements the counter by 1 and forwards;
// when it reaches 0 the goroutine closes its outgoing channel (propagating
// shutdown around the ring).
//
// Total channel operations: 2 × hops (one send + one receive per hop).
func ringBenchmark(nodes, hops int) {
	if nodes < 1 {
		return
	}

	channels := make([]chan int, nodes)
	for i := range channels {
		channels[i] = make(chan int)
	}

	var wg sync.WaitGroup
	wg.Add(nodes)

	for i := 0; i < nodes; i++ {
		in  := channels[i]
		out := channels[(i+1)%nodes]

		go func(in <-chan int, out chan<- int) {
			defer wg.Done()
			for v := range in {
				if v == 0 {
					close(out)
					return
				}
				out <- v - 1
			}
			close(out)
		}(in, out)
	}

	start := time.Now()
	channels[0] <- hops
	wg.Wait()
	duration := time.Since(start)
	ms := int(duration.Nanoseconds() / 1_000_000)

	rate := float64(hops) / duration.Seconds()
	fmt.Printf("Ring benchmark: %d hops across %d tasks in %dms (%.0f hops/s)\n", hops, nodes, ms, rate)
}


// ============================================================================
// 3. Selective receive / fan-out / fan-in
// ============================================================================

// fanInBenchmark measures select-based fan-out/fan-in throughput.
//
// One producer (the calling goroutine) sends tasks values into the shared
// unbuffered channel 'in'. workers goroutines each forward every value they
// receive from 'in' to their own private output channel. A single collector
// goroutine uses reflect.Select to receive from all workers simultaneously,
// counting until it has seen all tasks values.
//
// Worker output channels are closed when their worker drains the shared input.
// The collector removes closed outputs from the active select set and exits
// after exactly `tasks` successful receives. Timing includes producer
// send/close, collector completion, and worker join.
//
// reflect.Select is used because Go's built-in select statement requires a
// statically known set of cases; reflect.Select enables the same dynamic
// multi-way receive that goc_alts provides in libgoc.
func fanInBenchmark(workers, tasks int) {
	if workers < 1 {
		return
	}

	in   := make(chan int)
	outs := make([]chan int, workers)

	var workerWG sync.WaitGroup
	workerWG.Add(workers)

	// Fan-out: each worker drains 'in' and forwards to its own output channel.
	for i := range outs {
		outs[i] = make(chan int)
		out := outs[i]

		go func() {
			defer workerWG.Done()
			for v := range in {
				out <- v
			}
			close(out)
		}()
	}

	// Fan-in: collector uses reflect.Select to receive from any ready worker.
	// Closed worker outputs are nil'd out so Select skips them subsequently.
	done := make(chan struct{})
	go func() {
		cases := make([]reflect.SelectCase, workers)
		for i, ch := range outs {
			cases[i] = reflect.SelectCase{Dir: reflect.SelectRecv, Chan: reflect.ValueOf(ch)}
		}

		received := 0
		for received < tasks {
			// The received value is intentionally discarded — the benchmark
			// measures throughput (message count), not the values themselves.
			idx, _, ok := reflect.Select(cases)
			if !ok {
				cases[idx].Chan = reflect.ValueOf((<-chan int)(nil))
				continue
			}
			received++
		}
		close(done)
	}()

	start := time.Now()
	for i := 0; i < tasks; i++ {
		in <- i
	}
	close(in)

	<-done
	workerWG.Wait()
	duration := time.Since(start)
	ms := int(duration.Nanoseconds() / 1_000_000)

	rate := float64(tasks) / duration.Seconds()
	fmt.Printf("Selective receive / fan-out / fan-in: %d messages with %d workers in %dms (%.0f msg/s)\n", tasks, workers, ms, rate)
}


// ============================================================================
// 4. Spawn idle tasks
// ============================================================================

// spawnIdle measures the cost of spawning and joining idle goroutines.
//
// count goroutines are launched; each blocks immediately on receiving from
// the shared channel 'park'.  Once all goroutines are running, 'park' is
// closed, waking all of them at once, and the function waits for them all to
// finish.
//
// Timing covers the full lifecycle: spawn, block on channel, close/wake, join.
func spawnIdle(count int) {
	var wg sync.WaitGroup
	wg.Add(count)

	park := make(chan struct{})

	start := time.Now()
	for i := 0; i < count; i++ {
		go func() {
			defer wg.Done()
			<-park
		}()
	}

	close(park)
	wg.Wait()
	duration := time.Since(start)
	ms := int(duration.Nanoseconds() / 1_000_000)

	rate := float64(count) / duration.Seconds()
	fmt.Printf("Spawn idle tasks: %d goroutines in %dms (%.0f tasks/s)\n", count, ms, rate)
}


// ============================================================================
// 5. Prime sieve
// ============================================================================

// primeSieve runs the concurrent prime sieve up to max and prints the result.
func primeSieve(max int) {
	start := time.Now()
	count := sieve(max)
	duration := time.Since(start)
	ms := int(duration.Nanoseconds() / 1_000_000)

	rate := float64(count) / duration.Seconds()
	fmt.Printf("Prime sieve: %d primes up to %d in %dms (%.0f primes/s)\n", count, max, ms, rate)
}

// sieve runs the concurrent Sieve of Eratosthenes and returns the number of
// primes ≤ max.
//
// It repeatedly reads the next value from the current channel head (which is
// always prime), spawns a filter goroutine to remove that prime's multiples
// from the stream, and advances to the filter's output channel.
func sieve(max int) int {
	ch := generate(max)
	count := 0

	for {
		prime, ok := <-ch
		if !ok {
			break
		}
		count++
		ch = filter(ch, prime)
	}

	return count
}

// generate emits integers from 2 through max on the returned channel, then
// closes it.
func generate(max int) <-chan int {
	out := make(chan int)
	go func() {
		for i := 2; i <= max; i++ {
			out <- i
		}
		close(out)
	}()
	return out
}

// filter returns a new channel that forwards all values from in that are not
// multiples of prime, closing the output channel when in closes.
func filter(in <-chan int, prime int) <-chan int {
	out := make(chan int)
	go func() {
		for v := range in {
			if v%prime != 0 {
				out <- v
			}
		}
		close(out)
	}()
	return out
}
