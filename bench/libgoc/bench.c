#include "goc.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <uv.h>

// Helpers
// =======
// ns_to_seconds converts a nanosecond duration to seconds.
static double ns_to_seconds(uint64_t ns) {
    return (double)ns / 1e9;
}

// print_rate prints a benchmark throughput line for the given operation.
static void print_rate(const char* name, size_t ops, uint64_t ns, const char* unit) {
    double seconds = ns_to_seconds(ns);
    double rate = seconds > 0.0 ? (double)ops / seconds : 0.0;

    printf("%s: %zu %s in %.3f s (%.0f %s/s)\n", name, ops, unit, seconds, rate, unit);
}


// 1. Ping Pong Benchmark
// ======================
typedef struct {
    goc_chan* recv;
    goc_chan* send;
    size_t max;
} ping_args_t;

// ping_fiber bounces an incrementing counter between two channels until max is reached.
static void ping_fiber(void* arg) {
    ping_args_t* a = arg;

    for (;;) {
        goc_val_t v = goc_take(a->recv);
        if (v.ok != GOC_OK) {
            return;
        }

        size_t count = (size_t)(intptr_t)v.val;
        if (count >= a->max) {
            goc_close(a->send);
            return;
        }

        goc_put(a->send, (void*)(intptr_t)(count + 1));
    }
}

// bench_ping_pong measures round-trip latency between two channels.
static void bench_ping_pong(size_t rounds) {
    goc_chan* a_to_b = goc_chan_make(0);
    goc_chan* b_to_a = goc_chan_make(0);

    ping_args_t ping = { .recv = b_to_a, .send = a_to_b, .max = rounds };
    ping_args_t pong = { .recv = a_to_b, .send = b_to_a, .max = rounds };

    goc_chan* done_ping = goc_go(ping_fiber, &ping);
    goc_chan* done_pong = goc_go(ping_fiber, &pong);

    uint64_t start = uv_hrtime();
    goc_put_sync(a_to_b, (void*)(intptr_t)0);
    goc_take_sync(done_ping);
    goc_take_sync(done_pong);
    uint64_t end = uv_hrtime();

    print_rate("Channel ping-pong", rounds, end - start, "round trips");
}


// 2. Ring Benchmark
// =================
typedef struct {
    goc_chan* in;
    goc_chan* out;
} ring_node_t;

// ring_node forwards a counter around the ring until it reaches zero.
static void ring_node(void* arg) {
    ring_node_t* node = arg;

    for (;;) {
        goc_val_t v = goc_take(node->in);
        if (v.ok != GOC_OK) {
            goc_close(node->out);
            return;
        }

        size_t remaining = (size_t)(intptr_t)v.val;
        if (remaining == 0) {
            goc_close(node->out);
            return;
        }

        goc_put(node->out, (void*)(intptr_t)(remaining - 1));
    }
}

// bench_ring measures message passing latency around a ring of nodes.
static void bench_ring(size_t nodes, size_t hops) {
    goc_chan** chans = goc_malloc(sizeof(goc_chan*) * nodes);
    ring_node_t* args = goc_malloc(sizeof(ring_node_t) * nodes);
    goc_chan** done = goc_malloc(sizeof(goc_chan*) * nodes);

    for (size_t i = 0; i < nodes; i++) {
        chans[i] = goc_chan_make(0);
    }

    for (size_t i = 0; i < nodes; i++) {
        args[i].in = chans[i];
        args[i].out = chans[(i + 1) % nodes];
        done[i] = goc_go(ring_node, &args[i]);
    }

    uint64_t start = uv_hrtime();
    goc_put_sync(chans[0], (void*)(intptr_t)hops);

    for (size_t i = 0; i < nodes; i++) {
        goc_take_sync(done[i]);
    }

    uint64_t end = uv_hrtime();
    print_rate("Ring benchmark", hops, end - start, "hops");
}


// 3. Selective Receive / Fan-out / Fan-in Benchmark
// =================================================
typedef struct {
    goc_chan* in;
    goc_chan* out;
} worker_args_t;

// fan_worker forwards messages from the shared input to its own output.
static void fan_worker(void* arg) {
    worker_args_t* a = arg;

    for (;;) {
        goc_val_t v = goc_take(a->in);
        if (v.ok != GOC_OK) {
            return;
        }
        goc_put(a->out, v.val);
    }
}


// 4. Spawn / Join Benchmark
// =========================
typedef struct {
    goc_chan** outs;
    size_t out_count;
    size_t total;
} fan_in_args_t;

// fan_in_collector consumes from all worker outputs using goc_alts until the total is reached.
static void fan_in_collector(void* arg) {
    fan_in_args_t* a = arg;
    goc_alt_op* ops = goc_malloc(sizeof(goc_alt_op) * a->out_count);

    for (size_t i = 0; i < a->out_count; i++) {
        ops[i].ch = a->outs[i];
        ops[i].op_kind = GOC_ALT_TAKE;
        ops[i].put_val = NULL;
    }

    size_t received = 0;
    while (received < a->total) {
        goc_alts_result res = goc_alts(ops, a->out_count);
        if (res.value.ok == GOC_OK) {
            received++;
        }
    }
}

// bench_select_fan measures selective receive fan-out/fan-in throughput.
static void bench_select_fan(size_t workers, size_t tasks) {
    goc_chan* input = goc_chan_make(0);
    goc_chan** outs = goc_malloc(sizeof(goc_chan*) * workers);
    worker_args_t* args = goc_malloc(sizeof(worker_args_t) * workers);
    goc_chan** worker_done = goc_malloc(sizeof(goc_chan*) * workers);

    for (size_t i = 0; i < workers; i++) {
        outs[i] = goc_chan_make(0);
        args[i].in = input;
        args[i].out = outs[i];
        worker_done[i] = goc_go(fan_worker, &args[i]);
    }

    fan_in_args_t collector = {
        .outs = outs,
        .out_count = workers,
        .total = tasks,
    };
    goc_chan* collector_done = goc_go(fan_in_collector, &collector);

    uint64_t start = uv_hrtime();
    for (size_t i = 0; i < tasks; i++) {
        goc_put_sync(input, (void*)(intptr_t)i);
    }
    goc_close(input);

    goc_take_sync(collector_done);
    for (size_t i = 0; i < workers; i++) {
        goc_take_sync(worker_done[i]);
    }
    uint64_t end = uv_hrtime();

    print_rate("Selective receive / fan-out / fan-in", tasks, end - start, "messages");
}

static void idle_fiber(void* arg) {
    goc_chan* park = arg;
    goc_take(park);
}

// bench_spawn measures the cost of spawning and joining idle fibers.
static void bench_spawn(size_t count) {
    goc_chan* park = goc_chan_make(0);
    goc_chan** joins = goc_malloc(sizeof(goc_chan*) * count);

    uint64_t start = uv_hrtime();
    for (size_t i = 0; i < count; i++) {
        joins[i] = goc_go(idle_fiber, park);
    }

    goc_close(park);
    for (size_t i = 0; i < count; i++) {
        goc_take_sync(joins[i]);
    }
    uint64_t end = uv_hrtime();

    print_rate("Spawn idle tasks", count, end - start, "fibers");
}


// 5. Prime Sieve Benchmark
// ========================
typedef struct {
    goc_chan* out;
    size_t max;
} gen_args_t;

// generator emits integers from 2 up to max then closes the channel.
static void generator(void* arg) {
    gen_args_t* a = arg;
    for (size_t i = 2; i <= a->max; i++) {
        goc_put(a->out, (void*)(intptr_t)i);
    }
    goc_close(a->out);
}

typedef struct {
    goc_chan* in;
    goc_chan* out;
    size_t prime;
} filter_args_t;

// filter removes multiples of prime from the input stream.
static void filter(void* arg) {
    filter_args_t* a = arg;

    for (;;) {
        goc_val_t v = goc_take(a->in);
        if (v.ok != GOC_OK) {
            goc_close(a->out);
            return;
        }

        size_t n = (size_t)(intptr_t)v.val;
        if (n % a->prime != 0) {
            goc_put(a->out, (void*)(intptr_t)n);
        }
    }
}

// sieve implements a concurrent prime sieve and returns the number of primes <= max.
static size_t sieve(size_t max) {
    goc_chan* ch = goc_chan_make(0);
    gen_args_t gen = { .out = ch, .max = max };
    goc_chan* gen_done = goc_go(generator, &gen);

    size_t count = 0;
    for (;;) {
        goc_val_t v = goc_take_sync(ch);
        if (v.ok != GOC_OK) {
            break;
        }

        size_t prime = (size_t)(intptr_t)v.val;
        count++;

        goc_chan* next = goc_chan_make(0);
        filter_args_t* filter_args = goc_malloc(sizeof(filter_args_t));
        filter_args->in = ch;
        filter_args->out = next;
        filter_args->prime = prime;
        goc_go(filter, filter_args);

        ch = next;
    }

    goc_take_sync(gen_done);
    return count;
}

// bench_primes measures the throughput of the concurrent prime sieve.
static void bench_primes(size_t max) {
    uint64_t start = uv_hrtime();
    size_t primes = sieve(max);
    uint64_t end = uv_hrtime();

    double seconds = ns_to_seconds(end - start);
    double rate = seconds > 0.0 ? (double)primes / seconds : 0.0;
    printf("Prime sieve: %zu primes up to %zu in %.3f s (%.0f primes/s)\n", primes, max, seconds, rate);
}

int main(void) {
    const size_t ping_rounds = 200000;
    const size_t ring_nodes = 128;
    const size_t ring_hops = 500000;
    const size_t select_workers = 8;
    const size_t select_tasks = 200000;
    const size_t spawn_tasks = 200000;
    const size_t prime_max = 20000;

    goc_init();

    bench_ping_pong(ping_rounds);
    bench_ring(ring_nodes, ring_hops);
    bench_select_fan(select_workers, select_tasks);
    bench_spawn(spawn_tasks);
    bench_primes(prime_max);

    goc_shutdown();
    return 0;
}
