/*
 * bench/libgoc/bench.c — CSP concurrency benchmarks for libgoc
 *
 * Five micro-benchmarks that stress different aspects of the libgoc runtime.
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
 * Building
 * --------
 *   make -C bench/libgoc build          # single build
 *   make -C bench/libgoc run            # build + run (current GOC_POOL_THREADS)
 *   make -C bench/libgoc run-all        # build + run with pool sizes 1/2/4/8
 *
 * Environment variables
 * ---------------------
 *   GOC_POOL_THREADS   Number of worker threads in the default pool (default:
 *                      max(4, nproc)).  Set by the Makefile run-all target.
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

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <uv.h>


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
        size_t n = (size_t)(uintptr_t)v->val;
        if (n >= a->rounds) {
            goc_close(a->send);
            return;
        }
        goc_put(a->send, (void*)(uintptr_t)(n + 1));
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
    goc_put_sync(a, (void*)(uintptr_t)0);
    goc_take_sync(j1);
    goc_take_sync(j2);
    uint64_t t1 = uv_hrtime();

    double s    = (double)(t1 - t0) / 1e9;
    int    ms   = (int)(s * 1000);
    double rate = (double)(ping_rounds) / s;
    printf("Channel ping-pong: %zu round trips in %dms (%.0f round trips/s)\n",
           ping_rounds, ms, rate);
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
        size_t n = (size_t)(uintptr_t)v->val;
        if (n == 0) {
            goc_close(a->send);
            return;
        }
        goc_put(a->send, (void*)(uintptr_t)(n - 1));
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
    goc_put_sync(channels[0], (void*)(uintptr_t)ring_hops);
    goc_take_all_sync(joins, ring_nodes);
    uint64_t t1 = uv_hrtime();

    double s    = (double)(t1 - t0) / 1e9;
    int    ms   = (int)(s * 1000);
    double rate = (double)(ring_hops) / s;
    printf("Ring benchmark: %zu hops across %zu tasks in %dms (%.0f hops/s)\n",
           ring_hops, ring_nodes, ms, rate);
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

    goc_alt_op* ops     = goc_malloc(sizeof(goc_alt_op) * a->workers);
    size_t      n_active = a->workers;
    for (size_t i = 0; i < a->workers; i++) {
        ops[i].ch      = a->outs[i];
        ops[i].op_kind = GOC_ALT_TAKE;
        ops[i].put_val = NULL;
    }

    size_t received = 0;
    while (received < a->tasks && n_active > 0) {
        goc_alts_result* r = goc_alts(ops, n_active);
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
        goc_put_sync(in, (void*)(uintptr_t)i);
    goc_close(in);

    goc_take_sync(done);
    goc_take_all_sync(worker_joins, workers);
    uint64_t t1 = uv_hrtime();

    double s    = (double)(t1 - t0) / 1e9;
    int    ms   = (int)(s * 1000);
    double rate = (double)tasks / s;
    printf("Selective receive / fan-out / fan-in: %zu messages with %zu workers in %dms (%.0f msg/s)\n",
           tasks, workers, ms, rate);
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
 *
 * NOTE: This benchmark is currently disabled in main().
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

    goc_take_all_sync(joins, count);
    uint64_t t1 = uv_hrtime();

    double s    = (double)(t1 - t0) / 1e9;
    int    ms   = (int)(s * 1000);
    double rate = (double)count / s;
    printf("Spawn idle tasks: %zu fibers in %dms (%.0f tasks/s)\n",
           count, ms, rate);
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
 *
 * NOTE: This benchmark is currently disabled in main().
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
        if (goc_put(a->out, (void*)(uintptr_t)i) != GOC_OK)
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
        size_t n = (size_t)(uintptr_t)v->val;
        if (n % a->prime != 0)
            goc_put(a->out, (void*)(uintptr_t)n);
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
        size_t prime = (size_t)(uintptr_t)v->val;
        count++;

        goc_chan*      next  = goc_chan_make(0);
        filter_args_t* fargs = goc_malloc(sizeof(filter_args_t));
        fargs->in    = ch;
        fargs->out   = next;
        fargs->prime = prime;
        goc_go(filter_fn, fargs);
        ch = next;
    }

    goc_put(a->result_ch, (void*)(uintptr_t)count);
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
    goc_val_t* r = goc_take_sync(result_ch);
    uint64_t t1 = uv_hrtime();

    size_t count = (size_t)(uintptr_t)r->val;
    double s     = (double)(t1 - t0) / 1e9;
    int    ms    = (int)(s * 1000);
    double rate  = (double)count / s;
    printf("Prime sieve: %zu primes up to %zu in %dms (%.0f primes/s)\n",
           count, max, ms, rate);
}


/* =========================================================================
 * main
 * =========================================================================
 */
int main(void) {
    goc_init();

    size_t ping_rounds    = 200000;
    size_t ring_nodes     = 128;
    size_t ring_hops      = 500000;
    size_t select_workers = 8;
    size_t select_tasks   = 200000;
    size_t spawn_count    = 200000;
    size_t prime_max      = 20000;

    bench_ping_pong(ping_rounds);
    bench_ring(ring_nodes, ring_hops);
    bench_fan_in(select_workers, select_tasks);
    bench_spawn_idle(spawn_count);
    bench_prime_sieve(prime_max);

    goc_shutdown();
    return 0;
}
