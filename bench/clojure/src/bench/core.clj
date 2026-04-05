; bench/clojure/src/bench/core.clj — CSP concurrency benchmarks for Clojure core.async
;
; Seven micro-benchmarks that mirror bench/go/main.go and bench/libgoc/bench.c
; so that Go, libgoc, and Clojure results can be compared directly.
;
; Benchmarks
; ----------
; 1. Channel ping-pong — two go-blocks exchange a token back and forth.
; 2. Ring — a token is forwarded around a ring of N go-blocks.
; 3. Selective receive / fan-out / fan-in — producer → N workers → alts! collector.
; 4. Spawn idle tasks — spawn N go-blocks that park immediately, then wake them all.
; 5. Prime sieve — concurrent Eratosthenes pipeline of go-block filter stages.
; 6. HTTP ping-pong — two http-kit servers bounce a counter over HTTP/1.1.
; 7. HTTP server throughput — one http-kit server serves a tiny plaintext
;    response while many concurrent keep-alive clients issue GET requests.
;
; Running
; -------
;   clojure -M -m bench.core                        ; single run (default pool)
;   make run                                         ; same via Makefile
;   make run-all                                     ; pool sizes 1, 2, 4, 8

(ns bench.core
  (:require [clojure.core.async :as a
             :refer [go go-loop chan <! >! <!! >!! close! alts!]]
            [clojure.string :as str]
            [org.httpkit.server :as http-server]
            [org.httpkit.client :as http-client]))

(def ^:private ping-rounds    200000)
(def ^:private ring-nodes     128)
(def ^:private ring-hops      500000)
(def ^:private select-workers 8)
(def ^:private select-tasks   200000)
(def ^:private spawn-count    200000)
(def ^:private prime-max      20000)
(def ^:private http-rounds    2000)
(def ^:private http-tp-concurrency 32)
(def ^:private http-tp-warmup-ms   1000)
(def ^:private http-tp-measure-ms  5000)



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
;
; Worker output channels are closed when workers drain shared input. Collector
; removes closed outputs from the active alts! set and exits after exactly
; `tasks` successful receives. Timing includes producer send/close, collector
; completion, and worker join.

(defn bench-fan-in [workers tasks]
  (let [in   (chan)
        outs (vec (repeatedly workers chan))
        ; fan-out workers
        worker-joins
             (mapv (fn [out]
                     (go-loop []
                       (if-let [v (<! in)]
                         (do (>! out v) (recur))
                         (do (close! out) :done))))
                   outs)
        ; collector: alts! across all worker output channels
        done (chan)
        _    (go
               (loop [received    0
                      active-outs (vec outs)]
                 (if (< received tasks)
                   (let [[v ch] (alts! active-outs)]
                     (if (nil? v)
                       (recur received (vec (remove #(= % ch) active-outs)))
                       (recur (inc received) active-outs)))
                   (close! done))))
        t0   (System/nanoTime)]
    (dotimes [i tasks] (>!! in i))
    (close! in)
    (<!! done)
    (run! <!! worker-joins)
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


(defn bench-http-ping-pong [rounds]
  (let [warmup     (max 100 (long (/ rounds 10)))
        total      (+ warmup rounds)
        done-ch    (chan)
        samples    (long-array rounds)
        meas-start (atom 0)
        meas-end   (atom 0)
        url-a      "http://127.0.0.1:19090/ping"
        url-b      "http://127.0.0.1:19091/ping"
        parse-msg  (fn [s]
                     (let [[n t] (str/split (str/trim s) #"," 2)
                           n'    (Long/parseLong n)
                           t'    (if (and t (not (str/blank? t)))
                                   (Long/parseLong t)
                                   0)]
                       [n' t']))
        handler-a  (fn [req]
                     (let [[n sent-ns] (parse-msg (slurp (:body req)))
                           now         (System/nanoTime)]
                       (when (and (> n warmup) (<= n total) (pos? sent-ns))
                         (aset-long samples (int (- n warmup 1)) (- now sent-ns)))
                       (if (>= n total)
                         (do (reset! meas-end now)
                             (close! done-ch))
                         (do
                           (when (= n warmup)
                             (reset! meas-start now))
                             @(http-client/post url-b {:body (str (inc n) "," (System/nanoTime))})))
                       {:status 200 :body "ok"}))
        handler-b  (fn [req]
                     (let [[n sent-ns] (parse-msg (slurp (:body req)))]
                           @(http-client/post url-a {:body (str n "," sent-ns)})
                       {:status 200 :body "ok"}))
        srv-a      (http-server/run-server handler-a {:port 19090})
        srv-b      (http-server/run-server handler-b {:port 19091})]
    @(http-client/post url-a {:body "0,0"})
    (<!! done-ch)
    (let [start-ns (if (pos? @meas-start) @meas-start (System/nanoTime))
          end-ns   (if (> @meas-end start-ns) @meas-end (inc start-ns))
          meas-ns  (- end-ns start-ns)
          s        (/ meas-ns 1e9)
          ms       (long (/ meas-ns 1e6))
          vals     (vec samples)
          sorted   (vec (sort vals))
          idx      (fn [pct]
                     (int (Math/round (/ (* pct (dec rounds)) 100.0))))
          p50-us   (/ (double (nth sorted (idx 50))) 1000.0)
          p95-us   (/ (double (nth sorted (idx 95))) 1000.0)
          p99-us   (/ (double (nth sorted (idx 99))) 1000.0)
          avg-us   (/ (/ (double (reduce + vals)) rounds) 1000.0)
          rate     (/ rounds s)]
      (srv-a)
      (srv-b)
      (printf "HTTP ping-pong: %d round trips in %dms (%.0f round trips/s, avg %.1fus p50 %.1fus p95 %.1fus p99 %.1fus, warmup %d)\n"
              rounds ms rate avg-us p50-us p95-us p99-us warmup))))


; ============================================================================
; 7. HTTP server throughput
; ============================================================================

(defn bench-http-server-throughput [concurrency warmup-ms measure-ms]
  (when (and (pos? concurrency) (>= warmup-ms 0) (pos? measure-ms))
    (let [url             "http://127.0.0.1:19100/plaintext"
          handler         (fn [_req] {:status 200 :body "OK"})
          stop-server     (http-server/run-server handler {:port 19100})
          warmup-end-ns   (+ (System/nanoTime) (* warmup-ms 1000000))
          measure-end-ns  (+ warmup-end-ns (* measure-ms 1000000))
          workers         (mapv (fn [_]
                                  (future
                                    (loop [succ 0
                                           err  0]
                                      (if (>= (System/nanoTime) measure-end-ns)
                                        {:succ succ :err err}
                                        (let [resp      (try
                                                          @(http-client/get url {:timeout 1000})
                                                          (catch Throwable _
                                                            {:status 0}))
                                              done-ns   (System/nanoTime)
                                              in-window (and (>= done-ns warmup-end-ns)
                                                             (< done-ns measure-end-ns))
                                              ok?       (= 200 (:status resp))]
                                          (cond
                                            (and in-window ok?)
                                            (recur (inc succ) err)

                                            in-window
                                            (recur succ (inc err))

                                            :else
                                            (recur succ err)))))))
                                (range concurrency))
          totals          (reduce (fn [{acc-succ :succ acc-err :err}
                                      {w-succ :succ w-err :err}]
                                    {:succ (+ acc-succ w-succ)
                                     :err  (+ acc-err w-err)})
                                  {:succ 0 :err 0}
                                  (mapv deref workers))
          seconds         (/ measure-ms 1000.0)
          rate            (/ (:succ totals) seconds)]
      (stop-server)
      (printf "HTTP server throughput: %d requests in %dms (%.0f req/s, %d errors, concurrency %d, warmup %dms)\n"
              (:succ totals) measure-ms rate (:err totals) concurrency warmup-ms))))


; ============================================================================
; main
; ============================================================================

(defn -main [& _args]
  (bench-ping-pong   ping-rounds)
  (bench-ring        ring-nodes ring-hops)
  (bench-fan-in      select-workers select-tasks)
  (bench-spawn-idle  spawn-count)
  (bench-prime-sieve prime-max)
  (bench-http-ping-pong http-rounds)
  (bench-http-server-throughput http-tp-concurrency
                                http-tp-warmup-ms
                                http-tp-measure-ms)
  (flush)
  (shutdown-agents))
