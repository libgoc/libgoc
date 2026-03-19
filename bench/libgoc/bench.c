#include "goc.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <uv.h>

// Benchmarks
// ==========

// 1. Ping Pong Benchmark
// ======================
typedef struct {
    goc_chan* recv;
    goc_chan* send;
    size_t    rounds;
} ping_pong_args_t;

static void player_loop_fn(void* arg, goc_chan* ret_ch, goc_chan* break_ch) {
    ping_pong_args_t* a = (ping_pong_args_t*)arg;

    goc_val_t v = goc_take(a->recv);
    if (v.ok != GOC_OK) {
        goc_close(break_ch);
        return;
    }
    size_t n = (size_t)(uintptr_t)v.val;
    if (n >= a->rounds) {
        goc_close(a->send);
        goc_put(ret_ch, NULL);
        goc_close(break_ch);
        return;
    }
    goc_put(a->send, (void*)(uintptr_t)(n + 1));
}

static void player_fn(void* arg) {
    ping_pong_args_t* a = (ping_pong_args_t*)arg;
    for (;;) {
        goc_val_t v = goc_take(a->recv);
        if (v.ok != GOC_OK)
            return;
        size_t n = (size_t)(uintptr_t)v.val;
        if (n >= a->rounds) {
            goc_close(a->send);
            return;
        }
        goc_put(a->send, (void*)(uintptr_t)(n + 1));
    }
}

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


// 2. Ring Benchmark
// =================
typedef struct {
    goc_chan* recv;
    goc_chan* send;
} ring_node_args_t;

static void ring_node_fn(void* arg) {
    ring_node_args_t* a = (ring_node_args_t*)arg;
    for (;;) {
        goc_val_t v = goc_take(a->recv);
        if (v.ok != GOC_OK) {
            goc_close(a->send);
            return;
        }
        size_t n = (size_t)(uintptr_t)v.val;
        if (n == 0) {
            goc_close(a->send);
            return;
        }
        goc_put(a->send, (void*)(uintptr_t)(n - 1));
    }
}

static void bench_ring(size_t ring_nodes, size_t ring_hops) {
    if (ring_nodes < 1)
        return;

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
    for (size_t i = 0; i < ring_nodes; i++) {
        goc_take_sync(joins[i]);
    }
    uint64_t t1 = uv_hrtime();

    double s    = (double)(t1 - t0) / 1e9;
    int    ms   = (int)(s * 1000);
    double rate = (double)(ring_hops) / s;
    printf("Ring benchmark: %zu hops across %zu tasks in %dms (%.0f hops/s)\n",
           ring_hops, ring_nodes, ms, rate);
}


// 3. Selective Receive / Fan-out / Fan-in Benchmark
// =================================================
typedef struct {
    goc_chan* in;
    goc_chan* out;
} fan_out_worker_args_t;

static void fan_out_worker_fn(void* arg) {
    fan_out_worker_args_t* a = (fan_out_worker_args_t*)arg;
    for (;;) {
        goc_val_t v = goc_take(a->in);
        if (v.ok != GOC_OK) {
            goc_close(a->out);
            return;
        }
        goc_put(a->out, v.val);
    }
}

typedef struct {
    goc_chan** outs;
    size_t     workers;
    size_t     tasks;
    goc_chan*  done;
} fan_in_args_t;

static void fan_in_fn(void* arg) {
    fan_in_args_t* a = (fan_in_args_t*)arg;

    goc_alt_op* ops = goc_malloc(sizeof(goc_alt_op) * a->workers);
    size_t n_active = a->workers;
    for (size_t i = 0; i < a->workers; i++) {
        ops[i].ch       = a->outs[i];
        ops[i].op_kind  = GOC_ALT_TAKE;
        ops[i].put_val  = NULL;
    }

    size_t received = 0;
    while (received < a->tasks && n_active > 0) {
        goc_alts_result r = goc_alts(ops, n_active);
        if (r.value.ok == GOC_OK) {
            received++;
        } else {
            /* channel closed — remove it by swapping with the last active slot */
            ops[r.index] = ops[n_active - 1];
            n_active--;
        }
    }

    goc_close(a->done);
}

static void bench_fan_in(size_t workers, size_t tasks) {
    if (workers < 1)
        return;

    goc_chan*  in   = goc_chan_make(0);
    goc_chan** outs = goc_malloc(sizeof(goc_chan*) * workers);

    for (size_t i = 0; i < workers; i++) {
        outs[i] = goc_chan_make(0);
        fan_out_worker_args_t* args = goc_malloc(sizeof(fan_out_worker_args_t));
        args->in  = in;
        args->out = outs[i];
        goc_go(fan_out_worker_fn, args);
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
    uint64_t t1 = uv_hrtime();

    double s    = (double)(t1 - t0) / 1e9;
    double rate = (double)tasks / s;
    printf("Selective receive / fan-out / fan-in: %zu messages with %zu workers in %.3fs (%.0f msg/s)\n",
           tasks, workers, s, rate);
}


// 4. Spawn / Join Benchmark
// =========================
static void idle_fn(void* arg) {
    goc_chan* park = (goc_chan*)arg;
    goc_take(park);
}

static void bench_spawn_idle(size_t count) {
    goc_chan*  park  = goc_chan_make(0);
    goc_chan** joins = goc_malloc(sizeof(goc_chan*) * count);

    uint64_t t0 = uv_hrtime();
    for (size_t i = 0; i < count; i++)
        joins[i] = goc_go(idle_fn, park);

    goc_close(park);

    for (size_t i = 0; i < count; i++)
        goc_take_sync(joins[i]);
    uint64_t t1 = uv_hrtime();

    double s    = (double)(t1 - t0) / 1e9;
    double rate = (double)count / s;
    printf("Spawn idle tasks: %zu goroutines in %.3fs (%.0f tasks/s)\n",
           count, s, rate);
}


// 5. Prime Sieve Benchmark
// ========================
typedef struct {
    goc_chan* out;
    size_t    max;
} generate_args_t;

static void generate_fn(void* arg) {
    generate_args_t* a = (generate_args_t*)arg;
    for (size_t i = 2; i <= a->max; i++) {
        if (goc_put(a->out, (void*)(uintptr_t)i) != GOC_OK)
            return;
    }
    goc_close(a->out);
}

typedef struct {
    goc_chan* in;
    goc_chan* out;
    size_t    prime;
} filter_args_t;

static void filter_fn(void* arg) {
    filter_args_t* a = (filter_args_t*)arg;
    for (;;) {
        goc_val_t v = goc_take(a->in);
        if (v.ok != GOC_OK) {
            goc_close(a->out);
            return;
        }
        size_t n = (size_t)(uintptr_t)v.val;
        if (n % a->prime != 0)
            goc_put(a->out, (void*)(uintptr_t)n);
    }
}

typedef struct {
    size_t    max;
    goc_chan* result_ch;
} sieve_args_t;

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
        goc_val_t v = goc_take(ch);
        if (v.ok != GOC_OK)
            break;
        size_t prime = (size_t)(uintptr_t)v.val;
        count++;

        goc_chan*     next   = goc_chan_make(0);
        filter_args_t* fargs = goc_malloc(sizeof(filter_args_t));
        fargs->in    = ch;
        fargs->out   = next;
        fargs->prime = prime;
        goc_go(filter_fn, fargs);
        ch = next;
    }

    goc_put(a->result_ch, (void*)(uintptr_t)count);
}

static void bench_prime_sieve(size_t max) {
    goc_chan*     result_ch = goc_chan_make(0);
    sieve_args_t* args      = goc_malloc(sizeof(sieve_args_t));
    args->max       = max;
    args->result_ch = result_ch;

    uint64_t t0 = uv_hrtime();
    goc_go(sieve_fn, args);
    goc_val_t r = goc_take_sync(result_ch);
    uint64_t t1 = uv_hrtime();

    size_t count = (size_t)(uintptr_t)r.val;
    double s     = (double)(t1 - t0) / 1e9;
    double rate  = (double)count / s;
    printf("Prime sieve: %zu primes up to %zu in %.3fs (%.0f primes/s)\n",
           count, max, s, rate);
}


// Main
// ====
int main(void) {
    goc_init();

    size_t ping_rounds    = 200000;
    size_t ring_nodes     = 128;
    size_t ring_hops      = 500000;
    // size_t select_workers = 8;
    // size_t select_tasks   = 200000;
    // size_t spawn_count    = 200000;
    // size_t prime_max      = 20000;

    bench_ping_pong(ping_rounds);
    bench_ring(ring_nodes, ring_hops);
    // bench_fan_in(select_workers, select_tasks);
    // bench_spawn_idle(spawn_count);
    // bench_prime_sieve(prime_max);

    goc_shutdown();
    return 0;
}
