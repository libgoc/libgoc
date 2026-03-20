; bench/clojure/src/bench/core.clj — CSP concurrency benchmarks for Clojure core.async
;
; Five micro-benchmarks that mirror bench/go/main.go and bench/libgoc/bench.c
; so that Go, libgoc, and Clojure results can be compared directly.
;
; Benchmarks
; ----------
; 1. Channel ping-pong — two go-blocks exchange a token back and forth.
; 2. Ring — a token is forwarded around a ring of N go-blocks.
; 3. Selective receive / fan-out / fan-in — producer → N workers → alts! collector.
; 4. Spawn idle tasks — spawn N go-blocks that park immediately, then wake them all.
; 5. Prime sieve — concurrent Eratosthenes pipeline of go-block filter stages.
;
; Running
; -------
;   clojure -M -m bench.core                        ; single run (default pool)
;   make run                                         ; same via Makefile
;   make run-all                                     ; pool sizes 1, 2, 4, 8

(ns bench.core
  (:require [clojure.core.async :as a
             :refer [go go-loop chan <! >! <!! >!! close! alts!]]))

(def ^:private ping-rounds    200000)
(def ^:private ring-nodes     128)
(def ^:private ring-hops      500000)
(def ^:private select-workers 8)
(def ^:private select-tasks   200000)
(def ^:private spawn-count    200000)
(def ^:private prime-max      20000)


; ============================================================================
; 1. Channel ping-pong
; ============================================================================
;
; Two go-blocks (player) share a pair of unbuffered channels (a ↔ b).
; One block is kicked off when main places 0 on channel 'a'.  Each player
; receives a counter; if it has reached `rounds` it closes the forward
; channel (terminating its partner), otherwise it increments and sends back.
; Total channel ops: 2 × rounds.

(defn- player
  "One ping-pong participant.  Returns the go-block's result channel (join handle)."
  [recv send rounds]
  (go-loop []
    (when-let [v (<! recv)]
      (if (>= v rounds)
        (close! send)
        (do (>! send (inc v))
            (recur))))))

(defn bench-ping-pong [rounds]
  (let [a  (chan)
        b  (chan)
        j1 (player b a rounds)
        j2 (player a b rounds)
        t0 (System/nanoTime)]
    (>!! a 0)
    (<!! j1)
    (<!! j2)
    (let [s    (/ (- (System/nanoTime) t0) 1e9)
          ms   (long (* s 1000))
          rate (/ rounds s)]
      (printf "Channel ping-pong: %d round trips in %dms (%.0f round trips/s)\n"
              rounds ms rate))))


; ============================================================================
; 2. Ring benchmark
; ============================================================================
;
; `ring-nodes` go-blocks arranged in a circle.  Each reads from channels[i]
; and writes to channels[(i+1) % ring-nodes].  When the counter reaches 0 the
; node closes its outgoing channel, propagating shutdown around the ring.
; Total channel ops: 2 × ring-hops.

(defn- ring-node
  "One ring participant.  Returns the go-block's result channel (join handle)."
  [in out]
  (go-loop []
    (if-let [v (<! in)]
      (if (zero? v)
        (close! out)
        (do (>! out (dec v))
            (recur)))
      (close! out))))

(defn bench-ring [nodes hops]
  (let [channels (vec (repeatedly nodes chan))
        joins    (mapv (fn [i] (ring-node (channels i)
                                          (channels (mod (inc i) nodes))))
                       (range nodes))
        t0       (System/nanoTime)]
    (>!! (channels 0) hops)
    (doseq [j joins] (<!! j))
    (let [s    (/ (- (System/nanoTime) t0) 1e9)
          ms   (long (* s 1000))
          rate (/ hops s)]
      (printf "Ring benchmark: %d hops across %d tasks in %dms (%.0f hops/s)\n"
              hops nodes ms rate))))


; ============================================================================
; 3. Selective receive / fan-out / fan-in
; ============================================================================
;
; One producer sends `tasks` values into a shared unbuffered channel 'in'.
; `workers` go-blocks each forward every value from 'in' to their own private
; output channel.  A collector go-block uses alts! (analogous to reflect.Select
; in Go and goc_alts in libgoc) to receive from whichever worker is ready,
; counting until all `tasks` values have been seen.

(defn bench-fan-in [workers tasks]
  (let [in   (chan)
        outs (vec (repeatedly workers chan))
        ; fan-out workers
        _    (doseq [out outs]
               (go-loop []
                 (if-let [v (<! in)]
                   (do (>! out v) (recur))
                   (close! out))))
        ; collector: alts! across all worker output channels
        done (chan)
        _    (go
               (loop [received    0
                      active-outs (vec outs)]
                 (if (< received tasks)
                   (let [[v ch] (alts! active-outs)]
                     (if (nil? v)
                       ; channel closed — remove from active set
                       (recur received (vec (remove #(= % ch) active-outs)))
                       (recur (inc received) active-outs)))
                   (close! done))))
        t0   (System/nanoTime)]
    (dotimes [i tasks] (>!! in i))
    (close! in)
    (<!! done)
    (let [s    (/ (- (System/nanoTime) t0) 1e9)
          ms   (long (* s 1000))
          rate (/ tasks s)]
      (printf "Selective receive / fan-out / fan-in: %d messages with %d workers in %dms (%.0f msg/s)\n"
              tasks workers ms rate))))


; ============================================================================
; 4. Spawn idle tasks
; ============================================================================
;
; `count` go-blocks are launched; each parks immediately on receiving from
; the shared channel 'park'.  Once all are running, 'park' is closed, waking
; all of them at once.  Timing covers the full lifecycle: spawn, park, wake,
; and join.

(defn bench-spawn-idle [count]
  ; core.async channels allow at most 1024 pending takes.  Batch go-blocks
  ; into groups of 512 (each sharing one park channel) to stay within limits
  ; while preserving the spawn-block-wake semantics of the benchmark.
  (let [batch-size 512
        parks      (vec (repeatedly (long (Math/ceil (/ count batch-size))) chan))
        t0         (System/nanoTime)
        joins      (vec (map (fn [i]
                               (go (<! (parks (quot i batch-size))) :done))
                             (range count)))]
    (run! close! parks)
    (run! <!! joins)
    (let [s    (/ (- (System/nanoTime) t0) 1e9)
          ms   (long (* s 1000))
          rate (/ count s)]
      (printf "Spawn idle tasks: %d go-blocks in %dms (%.0f tasks/s)\n"
              count ms rate))))


; ============================================================================
; 5. Prime sieve
; ============================================================================
;
; Classic concurrent Sieve of Eratosthenes over channels.  generate emits
; [2, max]; each discovered prime spawns a filter-ch stage that removes its
; multiples.  Stresses long chains of go-blocks and sustained channel traffic.

(defn- generate
  "Emits integers [2, max] on the returned channel, then closes it."
  [max]
  (let [out (chan)]
    (go
      (loop [i 2]
        (when (<= i max)
          (>! out i)
          (recur (inc i))))
      (close! out))
    out))

(defn- filter-ch
  "Returns a new channel forwarding values from `in` not divisible by `prime`."
  [in prime]
  (let [out (chan)]
    (go
      (loop []
        (if-let [v (<! in)]
          (do (when (pos? (mod v prime))
                (>! out v))
              (recur))
          (close! out))))
    out))

(defn bench-prime-sieve [max]
  (let [result (chan)
        t0     (System/nanoTime)]
    (go
      (loop [ch    (generate max)
             count 0]
        (if-let [prime (<! ch)]
          (recur (filter-ch ch prime) (inc count))
          (>! result count))))
    (let [count (<!! result)
          s     (/ (- (System/nanoTime) t0) 1e9)
          ms    (long (* s 1000))
          rate  (/ count s)]
      (printf "Prime sieve: %d primes up to %d in %dms (%.0f primes/s)\n"
              count max ms rate))))


; ============================================================================
; main
; ============================================================================

(defn -main [& _args]
  (let [pool-size (Long/parseLong
                   (System/getProperty "clojure.core.async.pool-size" "8"))]
    (printf "CLOJURE_POOL_THREADS=%d\n" pool-size)
    (flush))
  (bench-ping-pong    ping-rounds)
  (bench-ring         ring-nodes ring-hops)
  (bench-fan-in       select-workers select-tasks)
  (bench-spawn-idle   spawn-count)
  (bench-prime-sieve  prime-max)
  (flush)
  (shutdown-agents))
