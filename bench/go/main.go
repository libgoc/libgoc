package main

import (
	"fmt"
	"reflect"
	"sync"
	"time"
)

func main() {
	pingRounds := 200000
	ringNodes := 128
	ringHops := 500000
	selectWorkers := 8
	selectTasks := 200000
	spawnCount := 200000
	primeMax := 20000

	pingPong(pingRounds)
	ringBenchmark(ringNodes, ringHops)
	fanInBenchmark(selectWorkers, selectTasks)
	spawnIdle(spawnCount)
	primeSieve(primeMax)
}


// ============ Benchmarks ============


// 1. Ping Pong Benchmark
// ======================
// pingPong measures a two-goroutine channel ping-pong for the specified rounds.
func pingPong(rounds int) {
	a := make(chan int)
	b := make(chan int)

	var wg sync.WaitGroup
	wg.Add(2)

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
	ms := int(duration.Nanoseconds() / 1000000)

	rate := float64(rounds) / duration.Seconds()
	fmt.Printf("Channel ping-pong: %d round trips in %dms (%.0f round trips/s)\n", rounds, ms, rate)
}


// 2. Ring Benchmark
// =================
// ringBenchmark measures sending a token around a ring of goroutines.
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
		in := channels[i]
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
	ms := int(duration.Nanoseconds() / 1000000)

	rate := float64(hops) / duration.Seconds()
	fmt.Printf("Ring benchmark: %d hops across %d tasks in %dms (%.0f hops/s)\n", hops, nodes, ms, rate)
}


// 3. Fan-In Benchmark
// ===================
// fanInBenchmark measures selective receive fan-out/fan-in throughput.
func fanInBenchmark(workers, tasks int) {
	if workers < 1 {
		return
	}

	in := make(chan int)
	outs := make([]chan int, workers)

	var workerWG sync.WaitGroup
	workerWG.Add(workers)

	for i := range outs {
		outs[i] = make(chan int)
		out := outs[i]

		go func() {
			defer workerWG.Done()
			for v := range in {
				out <- v
			}
		}()
	}

	done := make(chan struct{})
	go func() {
		cases := make([]reflect.SelectCase, workers)
		for i, ch := range outs {
			cases[i] = reflect.SelectCase{Dir: reflect.SelectRecv, Chan: reflect.ValueOf(ch)}
		}

		received := 0
		for received < tasks {
			idx, value, ok := reflect.Select(cases)
			if !ok {
				cases[idx].Chan = reflect.ValueOf((<-chan int)(nil))
				continue
			}
			_ = int(value.Int())
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
	ms := int(duration.Nanoseconds() / 1000000)

	rate := float64(tasks) / duration.Seconds()
	fmt.Printf("Selective receive / fan-out / fan-in: %d messages with %d workers in %dms (%.0f msg/s)\n", tasks, workers, ms, rate)
}


// 4. Spawn Idle Tasks Benchmark
// =============================
// spawnIdle measures the cost of spawning and joining idle goroutines.
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
	ms := int(duration.Nanoseconds() / 1000000)

	rate := float64(count) / duration.Seconds()
	fmt.Printf("Spawn idle tasks: %d goroutines in %dms (%.0f tasks/s)\n", count, ms, rate)
}


// 5. Prime Sieve Benchmark
// ========================
// primeSieve measures the rate of generating primes up to max using a sieve.
func primeSieve(max int) {
	start := time.Now()
	count := sieve(max)
	duration := time.Since(start)
	ms := int(duration.Nanoseconds() / 1000000)

	rate := float64(count) / duration.Seconds()
	fmt.Printf("Prime sieve: %d primes up to %d in %dms (%.0f primes/s)\n", count, max, ms, rate)
}

// sieve performs a concurrent prime sieve and returns the number of primes <= max.
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

// generate emits integers from 2 up to max and then closes the channel.
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

// filter removes multiples of prime from the input channel.
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
