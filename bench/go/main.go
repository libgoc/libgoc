/*
Package main contains standalone CSP concurrency benchmarks for Go.

Seven micro-benchmarks exercise different aspects of Go's goroutine scheduler
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
  6. HTTP ping-pong — two HTTP/1.1 servers on loopback ports bounce a counter
     back and forth, exercising net/http server/client throughput.
  7. HTTP server throughput — one HTTP/1.1 server serves a tiny plaintext
	  response while many concurrent keep-alive clients issue GET requests,
	  measuring sustained requests/second under load.

Usage:
  go run .                    # single run
  GOMAXPROCS=4 go run .       # single run with explicit parallelism
  make run all=1              # run with GOMAXPROCS = 1, 2, 4, 8
*/
package main

import (
	"fmt"
	"io"
	"net"
	"net/http"
	"reflect"
	"sort"
	"strconv"
	"strings"
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
	// Lower than CSP benchmarks: net/http pools connections, but each new
	// connection pair still adds setup overhead.  2000 rounds gives a fast,
	// stable rate measurement.
	httpRounds    := 2000
	httpTPConcurrency := 32
	httpTPWarmupMs := 1000
	httpTPMeasureMs := 5000

	pingPong(pingRounds)
	ringBenchmark(ringNodes, ringHops)
	fanInBenchmark(selectWorkers, selectTasks)
	spawnIdle(spawnCount)
	primeSieve(primeMax)
	httpPingPong(httpRounds)
	httpServerThroughput(httpTPConcurrency, httpTPWarmupMs, httpTPMeasureMs)
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


// ============================================================================
// 6. HTTP ping-pong
// ============================================================================

// httpPingPong measures HTTP/1.1 loopback request/response throughput.
//
	// Two servers (A and B) bounce a counter back and forth. Server A reads the
	// counter, writes "ok", then forwards counter+1 to B and waits for that
	// request to complete. Server B does the same back to A. When A sees counter
	// >= rounds it closes `done` instead of forwarding.
//
// Go's http.Client reuses keep-alive connections by default, so throughput
// here is higher than for libgoc (which opens a new TCP connection per
// request). The numbers are still included for end-to-end comparison.
//
// Uses fixed loopback ports (19090/19091) and /ping endpoints to mirror the
// libgoc and Clojure benchmark setup as closely as possible.
// The initial POST is synchronous so the timer starts only after the first
// server handshake has been initiated.
func httpPingPong(rounds int) {
	const addrA = "127.0.0.1:19090"
	const addrB = "127.0.0.1:19091"
	const urlA = "http://127.0.0.1:19090/ping"
	const urlB = "http://127.0.0.1:19091/ping"

	warmup := rounds / 10
	if warmup < 100 {
		warmup = 100
	}
	total := warmup + rounds

	done := make(chan struct{})
	var doneOnce sync.Once
	latNs := make([]int64, rounds)
	var measStart int64
	var measEnd int64

	lnA, err := net.Listen("tcp", addrA)
	if err != nil {
		fmt.Printf("HTTP ping-pong: listen error on %s: %v\n", addrA, err)
		return
	}
	lnB, err := net.Listen("tcp", addrB)
	if err != nil {
		lnA.Close()
		fmt.Printf("HTTP ping-pong: listen error on %s: %v\n", addrB, err)
		return
	}

	client := &http.Client{}

	muxA := http.NewServeMux()
	muxA.HandleFunc("/ping", func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		n, sentNs := parseCounterStamp(string(body))
		now := time.Now().UnixNano()
		w.Write([]byte("ok"))

		if n > warmup && n <= total && sentNs > 0 {
			latNs[n-warmup-1] = now - sentNs
		}

		if n >= total {
			measEnd = now
			doneOnce.Do(func() { close(done) })
			return
		}
		if n == warmup {
			measStart = now
		}

		resp, _ := client.Post(urlB, "text/plain",
			strings.NewReader(fmt.Sprintf("%d,%d", n+1, time.Now().UnixNano())))
		if resp != nil {
			resp.Body.Close()
		}
	})

	muxB := http.NewServeMux()
	muxB.HandleFunc("/ping", func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		n, sentNs := parseCounterStamp(string(body))
		w.Write([]byte("ok"))
		resp, _ := client.Post(urlA, "text/plain",
			strings.NewReader(fmt.Sprintf("%d,%d", n, sentNs)))
		if resp != nil {
			resp.Body.Close()
		}
	})

	srvA := &http.Server{Handler: muxA}
	srvB := &http.Server{Handler: muxB}
	go srvA.Serve(lnA)
	go srvB.Serve(lnB)

	resp, _ := client.Post(urlA, "text/plain", strings.NewReader("0,0"))
	if resp != nil {
		resp.Body.Close()
	}
	<-done

	if measStart == 0 || measEnd <= measStart {
		measStart = time.Now().UnixNano()
		measEnd = measStart + 1
	}
	measured := time.Duration(measEnd - measStart)

	srvA.Close()
	srvB.Close()

	sorted := append([]int64(nil), latNs...)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i] < sorted[j] })

	var sum int64
	for _, v := range latNs {
		sum += v
	}

	ms := int(measured.Nanoseconds() / 1_000_000)
	rate := float64(rounds) / measured.Seconds()
	avgUs := float64(sum) / float64(rounds) / 1000.0
	p50Us := percentileUs(sorted, 50)
	p95Us := percentileUs(sorted, 95)
	p99Us := percentileUs(sorted, 99)

	fmt.Printf("HTTP ping-pong: %d round trips in %dms (%.0f round trips/s, avg %.1fus p50 %.1fus p95 %.1fus p99 %.1fus, warmup %d)\n",
		rounds, ms, rate, avgUs, p50Us, p95Us, p99Us, warmup)
}

func parseCounterStamp(s string) (int, int64) {
	parts := strings.SplitN(strings.TrimSpace(s), ",", 2)
	if len(parts) == 0 {
		return 0, 0
	}
	n, _ := strconv.Atoi(parts[0])
	if len(parts) < 2 {
		return n, 0
	}
	t, _ := strconv.ParseInt(parts[1], 10, 64)
	return n, t
}

func percentileUs(sorted []int64, pct int) float64 {
	if len(sorted) == 0 {
		return 0
	}
	idx := (pct*(len(sorted)-1) + 50) / 100
	return float64(sorted[idx]) / 1000.0
}


// ============================================================================
// 7. HTTP server throughput
// ============================================================================

// httpServerThroughput measures sustained HTTP plaintext throughput.
//
// One loopback server serves GET /plaintext with body "OK". `concurrency`
// worker goroutines continuously issue keep-alive GET requests and count
// completions in the measurement window after warmup.
func httpServerThroughput(concurrency int, warmupMs int, measureMs int) {
	if concurrency <= 0 || warmupMs < 0 || measureMs <= 0 {
		return
	}

	const addr = "127.0.0.1:19100"
	url := "http://" + addr + "/plaintext"

	ln, err := net.Listen("tcp", addr)
	if err != nil {
		fmt.Printf("HTTP server throughput: listen error: %v\n", err)
		return
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/plaintext", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		w.Write([]byte("OK"))
	})

	srv := &http.Server{Handler: mux}
	go srv.Serve(ln)

	transport := &http.Transport{
		DisableKeepAlives:   false,
		MaxIdleConns:        concurrency * 2,
		MaxIdleConnsPerHost: concurrency * 2,
		IdleConnTimeout:     30 * time.Second,
	}
	client := &http.Client{
		Transport: transport,
		Timeout:   1 * time.Second,
	}

	start := time.Now()
	warmupEnd := start.Add(time.Duration(warmupMs) * time.Millisecond)
	measureEnd := warmupEnd.Add(time.Duration(measureMs) * time.Millisecond)

	succ := make([]uint64, concurrency)
	errs := make([]uint64, concurrency)

	var wg sync.WaitGroup
	wg.Add(concurrency)
	for i := 0; i < concurrency; i++ {
		idx := i
		go func() {
			defer wg.Done()
			var okCount uint64
			var errCount uint64

			for {
				if !time.Now().Before(measureEnd) {
					break
				}

				resp, reqErr := client.Get(url)
				done := time.Now()
				inWindow := !done.Before(warmupEnd) && done.Before(measureEnd)

				if reqErr == nil && resp != nil {
					io.Copy(io.Discard, resp.Body)
					status := resp.StatusCode
					resp.Body.Close()
					if inWindow {
						if status == http.StatusOK {
							okCount++
						} else {
							errCount++
						}
					}
				} else if inWindow {
					errCount++
				}
			}

			succ[idx] = okCount
			errs[idx] = errCount
		}()
	}

	wg.Wait()
	srv.Close()
	transport.CloseIdleConnections()

	var totalSucc uint64
	var totalErr uint64
	for i := 0; i < concurrency; i++ {
		totalSucc += succ[i]
		totalErr += errs[i]
	}

	seconds := float64(measureMs) / 1000.0
	rate := float64(totalSucc) / seconds
	fmt.Printf("HTTP server throughput: %d requests in %dms (%.0f req/s, %d errors, concurrency %d, warmup %dms)\n",
		totalSucc, measureMs, rate, totalErr, concurrency, warmupMs)
}
