#include "goc.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <uv.h>

// Helpers
// =======

// 1. Ping Pong Benchmark
// ======================
typedef struct {
    goc_chan* recv;
    goc_chan* send;
    size_t    rounds;
} ping_pong_args_t;

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
    double rate = (double)ping_rounds / s;
    printf("Channel ping-pong: %zu round trips in %.3fs (%.0f round trips/s)\n",
           ping_rounds, s, rate);
}


// 2. Ring Benchmark
// =================



// 3. Selective Receive / Fan-out / Fan-in Benchmark
// =================================================



// 4. Spawn / Join Benchmark
// =========================



// 5. Prime Sieve Benchmark
// ========================



// Main
// ====
int main(void) {
    goc_init();

    const size_t ping_rounds = 3000;
    bench_ping_pong(ping_rounds);

    goc_shutdown();
    return 0;
}
