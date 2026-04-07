/*
 * bench/libgoc/bench.c — CSP concurrency benchmarks for libgoc
 *
 * Seven micro-benchmarks that stress different aspects of the libgoc runtime.
 * Each benchmark is self-contained and can be read independently.
 *
 * Benchmarks
 * ----------
 * 1. Channel ping-pong — two fibers exchange a single token back and forth,
 *    measuring the raw cost of one send + one receive and the context switch
 *    that entails.
 *
 * 2. Ring — many fibers arranged in a circle each forward a counter token to
 *    the next node, stressing scheduling throughput across a larger group.
 *
 * 3. Selective receive / fan-out / fan-in — one producer fans work out to N
 *    worker fibers; a collector fiber uses goc_alts to receive from all N
 *    output channels simultaneously, stressing select logic and load
 *    distribution.
 *
 * 4. Spawn idle tasks — spawn a large number of fibers that immediately park
 *    on a shared channel, then wake them all by closing it.  Measures fiber
 *    creation overhead and the cost of blocking/unblocking many lightweight
 *    tasks.
 *
 * 5. Prime sieve — a classic concurrent pipeline where each stage filters
 *    multiples of a discovered prime.  Stresses long chains of fibers and
 *    sustained channel traffic.
 *
 * 6. HTTP ping-pong — two HTTP/1.1 servers on loopback ports bounce a
 *    counter back and forth, measuring end-to-end request/response overhead
 *    including TCP I/O and integration with the libgoc HTTP layer.
 *
 * 7. HTTP server throughput — one HTTP/1.1 server serves a tiny plaintext
 *    response while many concurrent keep-alive clients issue GET requests,
 *    measuring sustained requests/second under load.
 *
 * Building
 * --------
 *   make -C bench/libgoc build          # single build
 *   make -C bench/libgoc run            # build + run
 *   make -C bench/libgoc run all=1      # build + run with pool sizes 1/2/4/8
 *
 * Environment variables
 * ---------------------
 *   GOC_POOL_THREADS   Number of worker threads in the default pool.
 *                      `make run` uses threads=nproc by default; `make run all=1`
 *                      iterates over 1, 2, 4, 8.
 *   GOC_MAX_LIVE_FIBERS
 *                      Pool live-fiber cap. Defaults to
 *                      floor(0.6 × (memory / stack_size));
 *                      set to 0 to disable.
 *
 * Output format
 * -------------
 * All benchmarks print a single line:
 *   <description>: <count> <unit> in <ms>ms (<rate> <unit>/s)
 */

#include "goc.h"
#include "goc_http.h"
#include "goc_stats.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>


/* =========================================================================
 * Stats helper (GOC_ENABLE_STATS only)
 * =========================================================================
 *
 * Reads the three global telemetry accessors and prints a one-line summary.
 * Called at the end of each benchmark function so per-run counters can be
 * compared across benchmark configurations.
 *
 * steal stats    — lifetime totals from goc_pool_get_steal_stats; deltas
 *                  across a single benchmark run are not isolated here, so
 *                  treat these as cumulative floor values.
 * timeout stats  — lifetime alloc/expiration counts from goc_timeout_get_stats
 * cb queue hwm   — peak callback-queue depth since the last reset (lifetime)
 * =========================================================================
 */
#ifdef GOC_ENABLE_STATS
static void bench_print_stats(void) {
    uint64_t steal_att = 0, steal_suc = 0, steal_mis = 0, idle_wak = 0;
    goc_pool_get_steal_stats(&steal_att, &steal_suc, &steal_mis, &idle_wak);

    uint64_t to_allocs = 0, to_expires = 0;
    goc_timeout_get_stats(&to_allocs, &to_expires);

    size_t cb_hwm = goc_cb_queue_get_hwm();

    printf("  [stats] steal: %" PRIu64 " attempts / %" PRIu64 " successes"
           " / %" PRIu64 " misses"
           "  |  idle wakeups: %" PRIu64
           "  |  timeouts: %" PRIu64 " alloc / %" PRIu64 " fired"
           "  |  cb-queue hwm: %zu\n",
           steal_att, steal_suc, steal_mis, idle_wak, to_allocs, to_expires, cb_hwm);
}
#else
static inline void bench_print_stats(void) {}
#endif


/* =========================================================================
 * 1. Channel ping-pong
 * =========================================================================
 *
 * Two fibers (player_fn) share a pair of unbuffered channels (a → b, b → a).
 * One fiber starts the game by receiving on 'recv'; the other is kicked off
 * by the main thread placing the value 0 on channel 'a'.
 *
 * Each player receives a counter, checks if the round limit has been reached,
 * and either closes the forward channel (terminating the partner) or
 * increments the counter and sends it back.  Total channel operations =
 * 2 × ping_rounds (one send + one receive per round trip).
 */

typedef struct {
    goc_chan* recv;    /* channel this player reads from */
    goc_chan* send;    /* channel this player writes to  */
    size_t    rounds;  /* total round-trip count         */
} ping_pong_args_t;

/*
 * player_fn — fiber entry point for each ping-pong participant.
 *
 * Loops: take from recv, send to send, increment counter.
 * Exits when a GOC_CLOSED receive is seen or the round limit is reached.
 */
static void player_fn(void* arg) {
    ping_pong_args_t* a = (ping_pong_args_t*)arg;
    for (;;) {
        goc_val_t* v = goc_take(a->recv);
        if (v->ok != GOC_OK)
            return;
        size_t n = (size_t)goc_unbox_uint(v->val);
        if (n >= a->rounds) {
            goc_close(a->send);
            return;
        }
        goc_put(a->send, goc_box_uint(n + 1));
    }
}

/*
 * bench_ping_pong — run ping-pong for ping_rounds round trips.
 *
 * Timing starts when the first message is placed on channel 'a' and ends
 * after both join channels are closed (i.e. both fibers have returned).
 */
static void bench_ping_pong(size_t ping_rounds) {
    goc_chan* a = goc_chan_make(0);
    goc_chan* b = goc_chan_make(0);

    ping_pong_args_t args_ba = { .recv = b, .send = a, .rounds = ping_rounds };
    ping_pong_args_t args_ab = { .recv = a, .send = b, .rounds = ping_rounds };

    goc_chan* j1 = goc_go(player_fn, &args_ba);
    goc_chan* j2 = goc_go(player_fn, &args_ab);

    uint64_t t0 = uv_hrtime();
    goc_put(a, goc_box_uint(0));
    goc_take(j1);
    goc_take(j2);
    uint64_t t1 = uv_hrtime();

    double s    = (double)(t1 - t0) / 1e9;
    int    ms   = (int)(s * 1000);
    double rate = (double)(ping_rounds) / s;
    printf("Channel ping-pong: %zu round trips in %dms (%.0f round trips/s)\n",
           ping_rounds, ms, rate);
    bench_print_stats();
}


/* =========================================================================
 * 2. Ring benchmark
 * =========================================================================
 *
 * ring_nodes fibers are arranged in a ring.  Each fiber reads a counter from
 * its 'recv' channel.  If the counter has reached 0 it closes its 'send'
 * channel (propagating shutdown around the ring); otherwise it decrements the
 * counter and forwards it to 'send'.
 *
 * The main thread seeds the ring with the value ring_hops and then joins all
 * fibers.  Because the token traverses the full ring on each decrement, the
 * total number of channel operations is 2 × ring_hops (one send + one receive
 * per hop).
 */

typedef struct {
    goc_chan* recv;  /* incoming channel for this node */
    goc_chan* send;  /* outgoing channel for this node */
} ring_node_args_t;

/*
 * ring_node_fn — fiber entry point for one ring node.
 *
 * Forwards the counter around the ring, decrementing each hop.
 * Closes its outgoing channel when either the counter reaches 0 or the
 * incoming channel is closed by the previous node.
 */
static void ring_node_fn(void* arg) {
    ring_node_args_t* a = (ring_node_args_t*)arg;
    for (;;) {
        goc_val_t* v = goc_take(a->recv);
        if (v->ok != GOC_OK) {
            goc_close(a->send);
            return;
        }
        size_t n = (size_t)goc_unbox_uint(v->val);
        if (n == 0) {
            goc_close(a->send);
            return;
        }
        goc_put(a->send, goc_box_uint(n - 1));
    }
}

/*
 * bench_ring — run the ring benchmark with ring_nodes fibers and ring_hops hops.
 *
 * Timing starts when the seed value is placed on channels[0] and ends after
 * all fibers have joined.
 */
static void bench_ring(size_t ring_nodes, size_t ring_hops) {
    if (ring_nodes < 1)
        return;

    /* Create one channel per node; node i reads from channels[i] and
     * writes to channels[(i+1) % ring_nodes]. */
    goc_chan** channels = goc_malloc(sizeof(goc_chan*) * ring_nodes);
    for (size_t i = 0; i < ring_nodes; i++) {
        channels[i] = goc_chan_make(0);
    }

    goc_chan** joins = goc_malloc(sizeof(goc_chan*) * ring_nodes);
    for (size_t i = 0; i < ring_nodes; i++) {
        ring_node_args_t* args = goc_malloc(sizeof(ring_node_args_t));
        args->recv = channels[i];
        args->send = channels[(i + 1) % ring_nodes];
        joins[i] = goc_go(ring_node_fn, args);
    }

    uint64_t t0 = uv_hrtime();
    goc_put(channels[0], goc_box_uint(ring_hops));
    goc_take_all(joins, ring_nodes);
    uint64_t t1 = uv_hrtime();

    double s    = (double)(t1 - t0) / 1e9;
    int    ms   = (int)(s * 1000);
    double rate = (double)(ring_hops) / s;
    printf("Ring benchmark: %zu hops across %zu tasks in %dms (%.0f hops/s)\n",
           ring_hops, ring_nodes, ms, rate);
    bench_print_stats();
}


/* =========================================================================
 * 3. Selective receive / fan-out / fan-in
 * =========================================================================
 *
 * One producer (the main thread) sends 'tasks' values into a single
 * unbuffered channel 'in'.  N worker fibers (fan_out_worker_fn) each read
 * from 'in' and forward to their own private output channel 'out[i]'.
 * A dedicated collector fiber (fan_in_fn) calls goc_alts over all N output
 * channels, receiving whichever is ready first, and counts until it has seen
 * all 'tasks' values.
 *
 * Worker output channels are closed when workers drain the shared input.
 * Collector removes closed outputs from the active goc_alts set and exits
 * after exactly `tasks` successful receives.
 *
 * This stresses goc_alts (select) under realistic fan-in load and validates
 * that all messages are delivered exactly once.
 */

typedef struct {
    goc_chan* in;   /* shared input channel (producer → worker) */
    goc_chan* out;  /* private output channel (worker → collector) */
} fan_out_worker_args_t;

/*
 * fan_out_worker_fn — one fan-out worker fiber.
 *
 * Forwards every value it receives from 'in' to 'out', then closes 'out'
 * when 'in' closes.
 */
static void fan_out_worker_fn(void* arg) {
    fan_out_worker_args_t* a = (fan_out_worker_args_t*)arg;
    for (;;) {
        goc_val_t* v = goc_take(a->in);
        if (v->ok != GOC_OK) {
            goc_close(a->out);
            return;
        }
        goc_put(a->out, v->val);
    }
}

typedef struct {
    goc_chan** outs;     /* array of worker output channels          */
    size_t     workers;  /* number of workers (length of outs[])     */
    size_t     tasks;    /* total messages to receive before stopping */
    goc_chan*  done;     /* closed when the collector has finished   */
} fan_in_args_t;

/*
 * fan_in_fn — collector fiber using goc_alts to receive from all workers.
 *
 * Maintains a dynamic ops[] array. When a worker output closes, that channel
 * is removed from the active set via swap-with-last so goc_alts only scans
 * live outputs. Closes 'done' when all expected messages have been received.
 */
static void fan_in_fn(void* arg) {
    fan_in_args_t* a = (fan_in_args_t*)arg;

    goc_alt_op_t* ops    = goc_malloc(sizeof(goc_alt_op_t) * a->workers);
    size_t      n_active = a->workers;
    for (size_t i = 0; i < a->workers; i++) {
        ops[i].ch      = a->outs[i];
        ops[i].op_kind = GOC_ALT_TAKE;
        ops[i].put_val = NULL;
    }

    size_t received = 0;
    while (received < a->tasks && n_active > 0) {
        goc_alts_result_t* r = goc_alts(ops, n_active);
        if (r->value.ok == GOC_OK) {
            received++;
        } else {
            for (size_t k = 0; k < n_active; k++) {
                if (ops[k].ch == r->ch) {
                    ops[k] = ops[n_active - 1];
                    break;
                }
            }
            n_active--;
        }
    }

    goc_close(a->done);
}

/*
 * bench_fan_in — run fan-out/fan-in with the given worker and task counts.
 *
 * Timing includes producer send/close, collector completion, and worker join.
 */
static void bench_fan_in(size_t workers, size_t tasks) {
    if (workers < 1)
        return;

    goc_chan*  in   = goc_chan_make(0);
    goc_chan** outs = goc_malloc(sizeof(goc_chan*) * workers);
    goc_chan** worker_joins = goc_malloc(sizeof(goc_chan*) * workers);

    for (size_t i = 0; i < workers; i++) {
        outs[i] = goc_chan_make(0);
        fan_out_worker_args_t* args = goc_malloc(sizeof(fan_out_worker_args_t));
        args->in  = in;
        args->out = outs[i];
        worker_joins[i] = goc_go(fan_out_worker_fn, args);
    }

    goc_chan*      done     = goc_chan_make(0);
    fan_in_args_t* fan_args = goc_malloc(sizeof(fan_in_args_t));
    fan_args->outs    = outs;
    fan_args->workers = workers;
    fan_args->tasks   = tasks;
    fan_args->done    = done;
    goc_go(fan_in_fn, fan_args);

    uint64_t t0 = uv_hrtime();
    for (size_t i = 0; i < tasks; i++)
        goc_put(in, goc_box_uint(i));
    goc_close(in);

    goc_take(done);
    goc_take_all(worker_joins, workers);
    uint64_t t1 = uv_hrtime();

    double s    = (double)(t1 - t0) / 1e9;
    int    ms   = (int)(s * 1000);
    double rate = (double)tasks / s;
    printf("Selective receive / fan-out / fan-in: %zu messages with %zu workers in %dms (%.0f msg/s)\n",
           tasks, workers, ms, rate);
    bench_print_stats();
}


/* =========================================================================
 * 4. Spawn / join benchmark
 * =========================================================================
 *
 * Spawns 'count' fibers (idle_fn), each of which parks immediately on a
 * shared channel 'park'.  After all fibers have been launched, the main
 * thread closes 'park', waking all of them at once.  Each fiber then
 * returns and its join channel is drained.
 *
 * This measures:
 *   - Fiber creation throughput (goc_go cost × count)
 *   - Mass wakeup throughput (goc_close broadcasting to all parked fibers)
 */

/*
 * idle_fn — parks immediately on 'park', then returns.
 */
static void idle_fn(void* arg) {
    goc_chan* park = (goc_chan*)arg;
    goc_take(park);
}

/*
 * bench_spawn_idle — spawn 'count' fibers and wake them all via goc_close.
 *
 * Timing covers the full lifecycle: spawn, park (via goc_close), and join.
 */
static void bench_spawn_idle(size_t count) {
    goc_chan*  park  = goc_chan_make(0);
    goc_chan** joins = goc_malloc(sizeof(goc_chan*) * count);

    uint64_t t0 = uv_hrtime();
    for (size_t i = 0; i < count; i++)
        joins[i] = goc_go(idle_fn, park);

    goc_close(park);

    goc_take_all(joins, count);
    uint64_t t1 = uv_hrtime();

    double s    = (double)(t1 - t0) / 1e9;
    int    ms   = (int)(s * 1000);
    double rate = (double)count / s;
    printf("Spawn idle tasks: %zu fibers in %dms (%.0f tasks/s)\n",
           count, ms, rate);
    bench_print_stats();
}


/* =========================================================================
 * 5. Prime sieve benchmark
 * =========================================================================
 *
 * A classic concurrent prime sieve (Sieve of Eratosthenes over channels).
 *
 * generate_fn sends integers [2, max] on a channel.  sieve_fn reads the
 * first available number (which is always prime), spawns a filter_fn fiber
 * that removes all multiples of that prime from the stream, and repeats
 * until the stream closes.
 *
 * The pipeline length grows with the number of primes found:
 *   sieve → filter(2) → filter(3) → filter(5) → ... → filter(Pn)
 *
 * This stresses long chains of fibers and sustained point-to-point channel
 * traffic over the full run.
 */

typedef struct {
    goc_chan* out;  /* output channel for generated integers */
    size_t    max;  /* generate integers in the range [2, max] */
} generate_args_t;

/*
 * generate_fn — produces integers [2, max] on a channel, then closes it.
 */
static void generate_fn(void* arg) {
    generate_args_t* a = (generate_args_t*)arg;
    for (size_t i = 2; i <= a->max; i++) {
        if (goc_put(a->out, goc_box_uint(i)) != GOC_OK)
            return;
    }
    goc_close(a->out);
}

typedef struct {
    goc_chan* in;     /* input channel                            */
    goc_chan* out;    /* output channel (filtered stream)         */
    size_t    prime;  /* multiples of this prime are removed      */
} filter_args_t;

/*
 * filter_fn — passes values that are not divisible by 'prime', closes 'out'
 * when 'in' closes.
 */
static void filter_fn(void* arg) {
    filter_args_t* a = (filter_args_t*)arg;
    for (;;) {
        goc_val_t* v = goc_take(a->in);
        if (v->ok != GOC_OK) {
            goc_close(a->out);
            return;
        }
        size_t n = (size_t)goc_unbox_uint(v->val);
        if (n % a->prime != 0)
            goc_put(a->out, goc_box_uint(n));
    }
}

typedef struct {
    size_t    max;        /* upper bound for the sieve            */
    goc_chan* result_ch;  /* sends the prime count when finished  */
} sieve_args_t;

/*
 * sieve_fn — orchestrates the prime sieve pipeline.
 *
 * Each iteration: receive the next prime from the current channel, spawn a
 * new filter stage for that prime, and advance 'ch' to the filter's output.
 * Sends the total prime count on result_ch when the stream is exhausted.
 */
static void sieve_fn(void* arg) {
    sieve_args_t* a = (sieve_args_t*)arg;

    goc_chan*        gen_out  = goc_chan_make(0);
    generate_args_t* gen_args = goc_malloc(sizeof(generate_args_t));
    gen_args->out = gen_out;
    gen_args->max = a->max;
    goc_go(generate_fn, gen_args);

    goc_chan* ch    = gen_out;
    size_t    count = 0;

    for (;;) {
        goc_val_t* v = goc_take(ch);
        if (v->ok != GOC_OK)
            break;
        size_t prime = (size_t)goc_unbox_uint(v->val);
        count++;

        goc_chan*      next  = goc_chan_make(0);
        filter_args_t* fargs = goc_malloc(sizeof(filter_args_t));
        fargs->in    = ch;
        fargs->out   = next;
        fargs->prime = prime;
        goc_go(filter_fn, fargs);
        ch = next;
    }

    goc_put(a->result_ch, goc_box_uint(count));
}

/*
 * bench_prime_sieve — run the concurrent prime sieve up to 'max'.
 *
 * Timing starts before spawning the sieve fiber and ends when the prime count
 * is received on result_ch.
 */
static void bench_prime_sieve(size_t max) {
    goc_chan*     result_ch = goc_chan_make(0);
    sieve_args_t* args      = goc_malloc(sizeof(sieve_args_t));
    args->max       = max;
    args->result_ch = result_ch;

    uint64_t t0 = uv_hrtime();
    goc_go(sieve_fn, args);
    goc_val_t* r = goc_take(result_ch);
    uint64_t t1 = uv_hrtime();

    size_t count = (size_t)goc_unbox_uint(r->val);
    double s     = (double)(t1 - t0) / 1e9;
    int    ms    = (int)(s * 1000);
    double rate  = (double)count / s;
    printf("Prime sieve: %zu primes up to %zu in %dms (%.0f primes/s)\n",
           count, max, ms, rate);
    bench_print_stats();
}


/* =========================================================================
 * 6. HTTP ping-pong benchmark
 * =========================================================================
 *
 * Two HTTP/1.1 servers (A on port 19090, B on port 19091) bounce a counter
 * back and forth, mirroring the ping-pong example in docs/HTTP.md.
 *
 * Server A (POST /ping): reads the counter, responds "ok", then forwards
 * counter+1 to server B.  When counter >= rounds it closes `done` and stops.
 * Server B (POST /ping): reads the counter, responds "ok", then forwards
 * counter+1 back to server A.
 *
 * Timing: starts just before the first fire-and-forget POST to A, ends when
 * `done` is closed (i.e., after `rounds` HTTP round trips have completed).
 *
 * NOTE: The round count is intentionally lower than the CSP benchmarks
 * because the current libgoc HTTP client opens a new TCP connection for
 * every request.  The benchmark still exercises the HTTP server, client,
 * and fiber-scheduling integration end-to-end.
 */

#define HTTP_PP_PORT_A 19090
#define HTTP_PP_PORT_B 19091
#define HTTP_PP_URL_A  "http://127.0.0.1:19090/ping"
#define HTTP_PP_URL_B  "http://127.0.0.1:19091/ping"

/*
 * State shared between the two HTTP handler fibers.
 * Written once in bench_http_ping_pong before the servers start; read-only
 * thereafter (safe: the ping-pong chain is sequential).
 */
static struct {
    goc_chan* done;
    size_t    warmup;
    size_t    measured;
    size_t    total;
    uint64_t* lat_ns;
    uint64_t  t_meas_start;
    uint64_t  t_meas_end;
} g_http_pp;

static int cmp_u64(const void* a, const void* b) {
    const uint64_t va = *(const uint64_t*)a;
    const uint64_t vb = *(const uint64_t*)b;
    return (va > vb) - (va < vb);
}

static double percentile_us(const uint64_t* sorted, size_t n, int pct) {
    if (n == 0)
        return 0.0;
    size_t idx = (size_t)(((uint64_t)pct * (uint64_t)(n - 1) + 50u) / 100u);
    return (double)sorted[idx] / 1000.0;
}

/*
 * http_pp_handler_a — POST /ping handler for server A.
 *
 * Responds "ok" to the caller, then forwards counter+1 to server B.
 * Closes `done` instead of forwarding when the round limit is reached.
 */
static void http_pp_handler_a(goc_http_ctx_t* ctx) {
    int n = 0;
    uint64_t sent_ns = 0;
    const char* body = goc_http_server_body_str(ctx);
    (void)sscanf(body, "%d,%" SCNu64, &n, &sent_ns);

    uint64_t now = uv_hrtime();
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "ok"));

    if ((size_t)n > g_http_pp.warmup && (size_t)n <= g_http_pp.total && sent_ns > 0) {
        size_t idx = (size_t)n - g_http_pp.warmup - 1;
        g_http_pp.lat_ns[idx] = now - sent_ns;
    }

    if ((size_t)n >= g_http_pp.total) {
        g_http_pp.t_meas_end = now;
        goc_close(g_http_pp.done);
        return;
    }

    if ((size_t)n == g_http_pp.warmup)
        g_http_pp.t_meas_start = now;

    goc_http_request_opts_t* opts = goc_http_request_opts();
    opts->keep_alive = 1;
    /* Keep relay synchronous so benchmark teardown does not race with
     * in-flight client write callbacks. */
    goc_take(goc_http_post(HTTP_PP_URL_B, "text/plain",
                           goc_sprintf("%d,%" PRIu64, n + 1, uv_hrtime()),
                           opts));
}

/*
 * http_pp_handler_b — POST /ping handler for server B.
 *
 * Responds "ok" to the caller, then forwards counter+1 back to server A.
 */
static void http_pp_handler_b(goc_http_ctx_t* ctx) {
    int n = 0;
    uint64_t sent_ns = 0;
    const char* body = goc_http_server_body_str(ctx);
    (void)sscanf(body, "%d,%" SCNu64, &n, &sent_ns);

    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "ok"));

    goc_http_request_opts_t* opts = goc_http_request_opts();
    opts->keep_alive = 1;
    /* Keep relay synchronous so benchmark teardown does not race with
     * in-flight client write callbacks. */
    goc_take(goc_http_post(HTTP_PP_URL_A, "text/plain",
                           goc_sprintf("%d,%" PRIu64, n, sent_ns), opts));
}

/*
 * bench_http_ping_pong — run HTTP ping-pong for `rounds` round trips.
 *
 * Starts two servers, fires the initial request, waits for completion, then
 * shuts both servers down gracefully before returning.
 */
static void bench_http_ping_pong(size_t rounds) {
    g_http_pp.done   = goc_chan_make(0);
    g_http_pp.measured = rounds;
    g_http_pp.warmup   = rounds / 10;
    if (g_http_pp.warmup < 100)
        g_http_pp.warmup = 100;
    g_http_pp.total      = g_http_pp.warmup + g_http_pp.measured;
    g_http_pp.lat_ns     = (uint64_t*)goc_malloc(sizeof(uint64_t) * g_http_pp.measured);
    g_http_pp.t_meas_start = 0;
    g_http_pp.t_meas_end   = 0;

    goc_http_server_t* srv_a = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv_a, "POST", "/ping", http_pp_handler_a);
    goc_chan* ready_a = goc_http_server_listen(srv_a, "127.0.0.1", HTTP_PP_PORT_A);

    goc_http_server_t* srv_b = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv_b, "POST", "/ping", http_pp_handler_b);
    goc_chan* ready_b = goc_http_server_listen(srv_b, "127.0.0.1", HTTP_PP_PORT_B);

    goc_take(ready_a);
    goc_take(ready_b);

    goc_http_request_opts_t* opts = goc_http_request_opts();
    opts->keep_alive = 1;
    goc_http_post(HTTP_PP_URL_A, "text/plain", "0,0", opts);
    goc_take(g_http_pp.done);

    if (g_http_pp.t_meas_start == 0 || g_http_pp.t_meas_end <= g_http_pp.t_meas_start) {
        g_http_pp.t_meas_start = uv_hrtime();
        g_http_pp.t_meas_end = g_http_pp.t_meas_start + 1;
    }

    goc_take(goc_http_server_close(srv_a));
    goc_take(goc_http_server_close(srv_b));

    uint64_t meas_ns = g_http_pp.t_meas_end - g_http_pp.t_meas_start;
    double s    = (double)meas_ns / 1e9;
    int    ms   = (int)(s * 1000);
    double rate = (double)g_http_pp.measured / s;

    uint64_t* sorted = (uint64_t*)goc_malloc(sizeof(uint64_t) * g_http_pp.measured);
    memcpy(sorted, g_http_pp.lat_ns, sizeof(uint64_t) * g_http_pp.measured);
    qsort(sorted, g_http_pp.measured, sizeof(uint64_t), cmp_u64);

    uint64_t sum_ns = 0;
    for (size_t i = 0; i < g_http_pp.measured; i++)
        sum_ns += g_http_pp.lat_ns[i];

    double avg_us = ((double)sum_ns / (double)g_http_pp.measured) / 1000.0;
    double p50_us = percentile_us(sorted, g_http_pp.measured, 50);
    double p95_us = percentile_us(sorted, g_http_pp.measured, 95);
    double p99_us = percentile_us(sorted, g_http_pp.measured, 99);

    printf("HTTP ping-pong: %zu round trips in %dms (%.0f round trips/s, avg %.1fus p50 %.1fus p95 %.1fus p99 %.1fus, warmup %zu)\n",
           g_http_pp.measured, ms, rate, avg_us, p50_us, p95_us, p99_us,
           g_http_pp.warmup);
    bench_print_stats();
}


/* =========================================================================
 * 7. HTTP server throughput benchmark
 * =========================================================================
 *
 * One HTTP/1.1 loopback server serves GET /plaintext with a fixed tiny body
 * "OK".  `concurrency` client fibers issue back-to-back keep-alive GETs
 * using a shared request shape, and we measure successful responses over a
 * fixed measurement window after a warmup phase.
 */

#define HTTP_TP_PORT 19100
#define HTTP_TP_URL  "http://127.0.0.1:19100/plaintext"

static void http_tp_handler(goc_http_ctx_t* ctx) {
    goc_take(goc_http_server_respond(ctx, 200, "text/plain", "OK"));
}

typedef struct {
    size_t   worker_id;
    uint64_t warmup_end_ns;
    uint64_t measure_end_ns;
    uint64_t* succ_out;
    uint64_t* err_out;
} http_tp_worker_args_t;

static void http_tp_worker(void* arg) {
    http_tp_worker_args_t* a = (http_tp_worker_args_t*)arg;
    uint64_t succ = 0;
    uint64_t err  = 0;
    uint64_t req_count = 0;

    goc_http_request_opts_t* opts = goc_http_request_opts();
    opts->keep_alive = 1;
    opts->timeout_ms = 1000;

    for (;;) {
        if (uv_hrtime() >= a->measure_end_ns)
            break;

        req_count++;
        goc_http_response_t* r =
            (goc_http_response_t*)goc_take(goc_http_get(HTTP_TP_URL, opts))->val;
        uint64_t done_ns = uv_hrtime();

        if (done_ns >= a->warmup_end_ns && done_ns < a->measure_end_ns) {
            if (r && r->status == 200)
                succ++;
            else
                err++;
        }
    }

    *a->succ_out = succ;
    *a->err_out  = err;
}

static void bench_http_server_throughput(size_t concurrency,
                                         int warmup_ms,
                                         int measure_ms) {
    if (concurrency == 0 || warmup_ms < 0 || measure_ms <= 0)
        return;

    goc_http_server_t* srv = goc_http_server_make(goc_http_server_opts());
    goc_http_server_route(srv, "GET", "/plaintext", http_tp_handler);
    goc_take(goc_http_server_listen(srv, "127.0.0.1", HTTP_TP_PORT));

    uint64_t start_ns      = uv_hrtime();
    uint64_t warmup_end_ns = start_ns + (uint64_t)warmup_ms * 1000000ull;
    uint64_t measure_end_ns = warmup_end_ns + (uint64_t)measure_ms * 1000000ull;

    uint64_t* succ = (uint64_t*)goc_malloc(sizeof(uint64_t) * concurrency);
    uint64_t* err  = (uint64_t*)goc_malloc(sizeof(uint64_t) * concurrency);
    goc_chan** joins = (goc_chan**)goc_malloc(sizeof(goc_chan*) * concurrency);
    http_tp_worker_args_t* args =
        (http_tp_worker_args_t*)goc_malloc(sizeof(http_tp_worker_args_t) * concurrency);

    for (size_t i = 0; i < concurrency; i++) {
        succ[i] = 0;
        err[i]  = 0;
        args[i].worker_id      = i;
        args[i].warmup_end_ns  = warmup_end_ns;
        args[i].measure_end_ns = measure_end_ns;
        args[i].succ_out       = &succ[i];
        args[i].err_out        = &err[i];
        joins[i] = goc_go(http_tp_worker, &args[i]);
    }

    goc_take_all(joins, concurrency);
    goc_take(goc_http_server_close(srv));

    uint64_t total_succ = 0;
    uint64_t total_err  = 0;
    for (size_t i = 0; i < concurrency; i++) {
        total_succ += succ[i];
        total_err  += err[i];
    }

    double s = (double)measure_ms / 1000.0;
    double rate = (double)total_succ / s;
    printf("HTTP server throughput: %" PRIu64 " requests in %dms (%.0f req/s, %" PRIu64 " errors, concurrency %zu, warmup %dms)\n",
           total_succ, measure_ms, rate, total_err, concurrency, warmup_ms);
    bench_print_stats();
}


/* =========================================================================
 * main
 * =========================================================================
 */
static void main_fiber(void* _) {
    size_t ping_rounds    = 200000;
    size_t ring_nodes     = 128;
    size_t ring_hops      = 500000;
    size_t select_workers = 8;
    size_t select_tasks   = 200000;
    size_t spawn_count    = 200000;
    size_t prime_max      = 20000;
    size_t http_rounds    = 2000;
    size_t http_tp_concurrency = 32;
    int http_tp_warmup_ms = 1000;
    int http_tp_measure_ms = 5000;

    const char* http_only = getenv("GOC_BENCH_HTTP_ONLY");
    int run_http_only = (http_only && http_only[0] == '1');

    if (!run_http_only) {
        bench_ping_pong(ping_rounds);
        bench_ring(ring_nodes, ring_hops);
        bench_fan_in(select_workers, select_tasks);
        bench_spawn_idle(spawn_count);
        bench_prime_sieve(prime_max);
    }

    bench_http_ping_pong(http_rounds);
    bench_http_server_throughput(http_tp_concurrency,
                                 http_tp_warmup_ms,
                                 http_tp_measure_ms);
}

int main(void) {
    goc_init();
    goc_go(main_fiber, NULL);
    goc_shutdown();
    return 0;
}
