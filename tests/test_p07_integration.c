/*
 * tests/test_p7_integration.c — Phase 7: Integration tests for libgoc
 *
 * Verifies end-to-end behaviour of the runtime under realistic, multi-fiber
 * workloads: pipelines, fan-out/fan-in, high-volume stress, multi-sender
 * convergence, and timeout-based cancellation.  Each test exercises several
 * subsystems simultaneously (channels, fibers, pools, select, timeout) and
 * confirms that composed behaviour is correct under concurrent load.
 *
 * Build:  cmake -B build && cmake --build build
 * Run:    ctest --test-dir build --output-on-failure
 *         ./build/test_p7_integration
 *
 * Compile requirements: -std=c11 -DGC_THREADS -D_GNU_SOURCE
 *
 * Dependencies:
 *   - libgoc (goc.h)  — runtime under test
 *   - Boehm GC        — must be the threaded variant (bdw-gc-threaded);
 *                        initialised internally by goc_init()
 *   - libuv           — event loop; drives fiber scheduling and timers
 *   - pthreads (mutex + condvar for done_t) — see below
 *
 * Synchronisation helper — done_t:
 *   A portable mutex+condvar semaphore that lets the main thread (or a waiter
 *   fiber) block until a fiber signals completion.  Using mutex+condvar avoids
 *   the need for a goc_chan in tests that are themselves exercising channel
 *   behaviour, keeping each test self-contained.
 *
 *     done_init(&d)    — initialise the mutex and condvar
 *     done_signal(&d)  — set flag and signal — called by the fiber on exit
 *     done_wait(&d)    — wait until flag is set — called by the test thread
 *     done_destroy(&d) — destroy the mutex and condvar
 *
 * Test coverage (Phase 7 — Integration):
 *
 *   P7.1  Pipeline: producer → transformer → consumer, 16 items, all values
 *         correct
 *   P7.2  Fan-out / fan-in: 1 producer, 4 workers, result aggregation, 20
 *         items, sum verified
 *   P7.3  High-volume stress: 10 000 messages, sum verified
 *   P7.4  Multi-fiber: 8 senders on 1 unbuffered channel, all IDs received
 *         exactly once
 *   P7.5  Timeout + cancellation: slower fiber's result discarded cleanly,
 *         shutdown completes without hang
 *
 * Notes:
 *   - goc_init() is called once in main() before any test runs.
 *   - goc_shutdown() is called once in main() after all tests complete.
 *   - Each test function uses the goto-based cleanup pattern from the harness:
 *     ASSERT() jumps to `done:` on failure; TEST_PASS() also jumps to `done:`.
 *     Cleanup code after the `done:` label runs in both pass and fail paths.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <uv.h>

#include "test_harness.h"
#include "goc.h"

/* =========================================================================
 * done_t — lightweight fiber-to-main synchronisation via mutex + condvar
 *
 * Used throughout Phase 7 to let the main thread block until a target fiber
 * signals completion.  Choosing mutex+condvar rather than a goc_chan keeps
 * each test independent of the channel machinery it is trying to verify.
 * ====================================================================== */

typedef struct {
    uv_mutex_t mtx;
    uv_cond_t  cond;
    int        flag;
} done_t;

static void done_init(done_t* d) {
    uv_mutex_init(&d->mtx);
    uv_cond_init(&d->cond);
    d->flag = 0;
}
static void done_signal(done_t* d) {
    uv_mutex_lock(&d->mtx);
    d->flag = 1;
    uv_cond_signal(&d->cond);
    uv_mutex_unlock(&d->mtx);
}
static void done_wait(done_t* d) {
    uv_mutex_lock(&d->mtx);
    while (!d->flag)
        uv_cond_wait(&d->cond, &d->mtx);
    d->flag = 0;
    uv_mutex_unlock(&d->mtx);
}
static void done_destroy(done_t* d) {
    uv_mutex_destroy(&d->mtx);
    uv_cond_destroy(&d->cond);
}

/* =========================================================================
 * Phase 7 — Integration
 * ====================================================================== */

/* --- P7.1: Pipeline: producer → transformer → consumer ---------------- */

/*
 * Pipeline topology:
 *
 *   producer_fiber  ─[src_ch]→  transformer_fiber  ─[dst_ch]→  consumer_fiber
 *
 * The producer sends integers 1..N as goc_box_uint() values.  The
 * transformer doubles each value and forwards it.  The consumer accumulates
 * the sum into a shared result slot, then signals done.
 *
 * Expected result:  sum of (2 * i) for i in [1, N]  =  N * (N+1)
 */

#define P7_1_N 16

typedef struct {
    goc_chan* src_ch;
    int       n;
} p7_1_producer_args_t;

typedef struct {
    goc_chan* src_ch;
    goc_chan* dst_ch;
} p7_1_transformer_args_t;

typedef struct {
    goc_chan*  dst_ch;
    done_t*    done;
    uintptr_t  sum;
} p7_1_consumer_args_t;

/*
 * Producer: sends 1, 2, … n to src_ch then closes the channel.
 */
static void test_p7_1_producer_fn(void* arg) {
    p7_1_producer_args_t* a = (p7_1_producer_args_t*)arg;
    for (int i = 1; i <= a->n; i++) {
        goc_status_t st = goc_put(a->src_ch, goc_box_uint(i));
        if (st != GOC_OK) break;
    }
    goc_close(a->src_ch);
}

/*
 * Transformer: reads values from src_ch, doubles each one, forwards to dst_ch.
 * Stops and closes dst_ch when src_ch is closed.
 */
static void test_p7_1_transformer_fn(void* arg) {
    p7_1_transformer_args_t* a = (p7_1_transformer_args_t*)arg;
    for (;;) {
        goc_val_t* v = goc_take(a->src_ch);
        if (v->ok != GOC_OK) break;
        uintptr_t doubled = goc_unbox_uint(v->val) * 2;
        goc_status_t st = goc_put(a->dst_ch, goc_box_uint(doubled));
        if (st != GOC_OK) break;
    }
    goc_close(a->dst_ch);
}

/*
 * Consumer: drains dst_ch and accumulates the sum; signals done when finished.
 */
static void test_p7_1_consumer_fn(void* arg) {
    p7_1_consumer_args_t* a = (p7_1_consumer_args_t*)arg;
    uintptr_t sum = 0;
    for (;;) {
        goc_val_t* v = goc_take(a->dst_ch);
        if (v->ok != GOC_OK) break;
        sum += goc_unbox_uint(v->val);
    }
    a->sum = sum;
    done_signal(a->done);
}

/*
 * P7.1 — Pipeline: producer → transformer → consumer, 16 items, all values correct
 *
 * Three fibers are chained via two buffered channels.  After all items flow
 * through, the consumer signals done and the test verifies the accumulated sum
 * equals N*(N+1) (sum of 2*i for i in 1..N).
 */
static void test_p7_1(void) {
    TEST_BEGIN("P7.1   pipeline: producer→transformer→consumer, 16 items");

    done_t done;
    done_init(&done);

    /* Use small buffers to keep the pipeline flowing without all-at-once
     * buffering; forces interleaved handoffs between the three fibers. */
    goc_chan* src_ch = goc_chan_make(4);
    ASSERT(src_ch != NULL);
    goc_chan* dst_ch = goc_chan_make(4);
    ASSERT(dst_ch != NULL);

    p7_1_producer_args_t    pargs = { .src_ch = src_ch, .n = P7_1_N };
    p7_1_transformer_args_t targs = { .src_ch = src_ch, .dst_ch = dst_ch };
    p7_1_consumer_args_t    cargs = { .dst_ch = dst_ch, .done = &done, .sum = 0 };

    /* Launch all three fibers; order doesn't matter — channels synchronise them. */
    goc_chan* pjoin = goc_go(test_p7_1_producer_fn,    &pargs);
    ASSERT(pjoin != NULL);
    goc_chan* tjoin = goc_go(test_p7_1_transformer_fn, &targs);
    ASSERT(tjoin != NULL);
    goc_chan* cjoin = goc_go(test_p7_1_consumer_fn,    &cargs);
    ASSERT(cjoin != NULL);

    /* Block until the consumer is done. */
    done_wait(&done);

    /* Wait for all fibers to finish. */
    goc_take_sync(pjoin);
    goc_take_sync(tjoin);
    goc_take_sync(cjoin);

    /* Expected sum: Σ(2*i, i=1..N) = N*(N+1) */
    uintptr_t expected = (uintptr_t)P7_1_N * (P7_1_N + 1);
    ASSERT(cargs.sum == expected);

    done_destroy(&done);
    TEST_PASS();
done:;
}

/* --- P7.2: Fan-out / fan-in ------------------------------------------- */

/*
 * Fan-out / fan-in topology:
 *
 *                            ┌─ worker_0 ─┐
 *   producer ─[work_ch]──┬──┤─ worker_1 ─├──[result_ch]─ consumer (main)
 *                        │  │─ worker_2 ─│
 *                        └──┤─ worker_3 ─┘
 *
 * The producer sends integers 1..N.  Each worker takes from work_ch, multiplies
 * by 2, and puts the result onto result_ch.  The main thread drains result_ch
 * and verifies the sum.
 *
 * To signal completion to the workers, the producer closes work_ch after
 * sending all items.  Each worker exits its receive loop when it sees
 * GOC_CLOSED.  A separate done counter tracks how many workers have exited;
 * when all NWORKERS workers are done, the last one closes result_ch so the
 * main thread's drain loop terminates.
 */

#define P7_2_N       20
#define P7_2_NWORKERS 4

typedef struct {
    goc_chan* work_ch;
    int       n;
} p7_2_producer_args_t;

typedef struct {
    goc_chan*     work_ch;
    goc_chan*     result_ch;
    /* Shared among all workers: the worker that decrements this to 0 closes
     * result_ch.  Accessed only via atomic decrement using _Atomic or we
     * can use a semaphore count; here we use a plain mutex-free approach
     * with a goc_chan of capacity 1 as a "drain gate". */
    goc_chan*     gate_ch;  /* closed by the last worker to finish */
    int           nworkers; /* total number of workers */
} p7_2_worker_args_t;

/*
 * Producer: sends 1..n to work_ch then closes it.
 */
static void test_p7_2_producer_fn(void* arg) {
    p7_2_producer_args_t* a = (p7_2_producer_args_t*)arg;
    for (int i = 1; i <= a->n; i++) {
        goc_status_t st = goc_put(a->work_ch, goc_box_uint(i));
        if (st != GOC_OK) break;
    }
    goc_close(a->work_ch);
}

/*
 * Worker: drains work_ch, doubles each value, puts onto result_ch.
 * Puts a sentinel (NULL) onto gate_ch when done to signal the coordinator.
 */
static void test_p7_2_worker_fn(void* arg) {
    p7_2_worker_args_t* a = (p7_2_worker_args_t*)arg;
    for (;;) {
        goc_val_t* v = goc_take(a->work_ch);
        if (v->ok != GOC_OK) break;
        uintptr_t result = goc_unbox_uint(v->val) * 2;
        goc_put(a->result_ch, goc_box_uint(result));
    }
    /* Signal that this worker is done by putting a sentinel onto the gate. */
    goc_put(a->gate_ch, NULL);
}

/*
 * P7.2 — Fan-out / fan-in: 1 producer, 4 workers, 20 items, sum verified
 *
 * A single producer fan-outs work to 4 concurrent workers via a shared work
 * channel.  Results are fanned-in to a result channel that the main thread
 * drains.  All 20 items must be processed and the sum must equal N*(N+1).
 */
static void test_p7_2(void) {
    TEST_BEGIN("P7.2   fan-out/fan-in: 1 producer, 4 workers, 20 items");

    /* Buffered channels to reduce contention; capacity > NWORKERS to avoid
     * the gate puts stalling while result_ch drains. */
    goc_chan* work_ch   = goc_chan_make(P7_2_NWORKERS);
    ASSERT(work_ch != NULL);
    goc_chan* result_ch = goc_chan_make(P7_2_N);
    ASSERT(result_ch != NULL);
    /* gate_ch collects one sentinel per worker; capacity == NWORKERS so no
     * worker ever blocks on its final put. */
    goc_chan* gate_ch   = goc_chan_make(P7_2_NWORKERS);
    ASSERT(gate_ch != NULL);

    p7_2_producer_args_t pargs = { .work_ch = work_ch, .n = P7_2_N };
    p7_2_worker_args_t   wargs = {
        .work_ch   = work_ch,
        .result_ch = result_ch,
        .gate_ch   = gate_ch,
        .nworkers  = P7_2_NWORKERS,
    };

    goc_chan* pjoin = goc_go(test_p7_2_producer_fn, &pargs);
    ASSERT(pjoin != NULL);

    goc_chan* wjoins[P7_2_NWORKERS];
    for (int i = 0; i < P7_2_NWORKERS; i++) {
        wjoins[i] = goc_go(test_p7_2_worker_fn, &wargs);
        ASSERT(wjoins[i] != NULL);
    }

    /* Wait for all workers to finish (each puts one sentinel onto gate_ch). */
    for (int i = 0; i < P7_2_NWORKERS; i++) {
        goc_val_t* g = goc_take_sync(gate_ch);
        /* GOC_OK (value delivered) or GOC_CLOSED both signal worker done. */
        (void)g;
    }

    /* Workers are done; close result_ch so the drain loop below terminates. */
    goc_close(result_ch);

    /* Drain result_ch and accumulate. */
    uintptr_t sum = 0;
    for (;;) {
        goc_val_t* v = goc_take_sync(result_ch);
        if (v->ok != GOC_OK) break;
        sum += goc_unbox_uint(v->val);
    }

    /* Wait for producer and all worker fibers. */
    goc_take_sync(pjoin);
    for (int i = 0; i < P7_2_NWORKERS; i++) {
        goc_take_sync(wjoins[i]);
    }

    /* Expected: Σ(2*i, i=1..N) = N*(N+1) */
    uintptr_t expected = (uintptr_t)P7_2_N * (P7_2_N + 1);
    ASSERT(sum == expected);

    goc_close(work_ch);
    goc_close(gate_ch);
    TEST_PASS();
done:;
}

/* --- P7.3: High-volume stress ------------------------------------------ */

/*
 * A single producer fiber sends 10 000 integer values through a buffered
 * channel.  The main thread drains the channel and verifies the sum.
 * This exercises the ring-buffer machinery under sustained load and
 * confirms that no values are lost, duplicated, or corrupted.
 */

#define P7_3_N 10000

typedef struct {
    goc_chan* ch;
    int       n;
} p7_3_producer_args_t;

static void test_p7_3_producer_fn(void* arg) {
    p7_3_producer_args_t* a = (p7_3_producer_args_t*)arg;
    for (int i = 1; i <= a->n; i++) {
        goc_status_t st = goc_put(a->ch, goc_box_uint(i));
        if (st != GOC_OK) break;
    }
    goc_close(a->ch);
}

/*
 * P7.3 — High-volume stress: 10 000 messages, sum verified
 *
 * Uses a moderately-sized ring buffer (64 slots) to force many producer-yields
 * and consumer-wakeups.  The expected sum is N*(N+1)/2.
 */
static void test_p7_3(void) {
    TEST_BEGIN("P7.3   high-volume stress: 10 000 messages, sum verified");

    goc_chan* ch = goc_chan_make(64);
    ASSERT(ch != NULL);

    p7_3_producer_args_t pargs = { .ch = ch, .n = P7_3_N };
    goc_chan* pjoin = goc_go(test_p7_3_producer_fn, &pargs);
    ASSERT(pjoin != NULL);

    uintptr_t sum = 0;
    for (;;) {
        goc_val_t* v = goc_take_sync(ch);
        if (v->ok != GOC_OK) break;
        sum += goc_unbox_uint(v->val);
    }

    goc_take_sync(pjoin);

    /* Expected: Σ(i, i=1..N) = N*(N+1)/2 */
    uintptr_t expected = (uintptr_t)P7_3_N * (P7_3_N + 1) / 2;
    ASSERT(sum == expected);

    TEST_PASS();
done:;
}

/* --- P7.4: Multi-fiber — 8 senders, 1 unbuffered channel -------------- */

/*
 * Eight sender fibers each send a unique ID (0..7) onto a single rendezvous
 * channel.  The main thread receives all 8 values and verifies that each ID
 * appears exactly once.
 *
 * This exercises the case where multiple fibers compete to put onto a
 * single unbuffered channel; exactly one fiber is unparked per take and
 * no value may be lost, duplicated, or out-of-domain.
 */

#define P7_4_NSENDERS 8

typedef struct {
    goc_chan* ch;
    uintptr_t id;
} p7_4_sender_args_t;

static void test_p7_4_sender_fn(void* arg) {
    p7_4_sender_args_t* a = (p7_4_sender_args_t*)arg;
    goc_put(a->ch, goc_box_uint(a->id));
}

/*
 * P7.4 — Multi-fiber: 8 senders on 1 unbuffered channel, all IDs exactly once
 *
 * Launches P7_4_NSENDERS fibers, each sending a distinct uintptr_t ID.  The
 * main thread receives all N values and checks that (a) exactly N values
 * arrived, and (b) each ID in [0, N) was received exactly once.
 */
static void test_p7_4(void) {
    TEST_BEGIN("P7.4   multi-fiber: 8 senders on 1 unbuffered channel");

    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    p7_4_sender_args_t sargs[P7_4_NSENDERS];
    goc_chan* joins[P7_4_NSENDERS];

    for (uintptr_t i = 0; i < P7_4_NSENDERS; i++) {
        sargs[i] = (p7_4_sender_args_t){ .ch = ch, .id = i };
        joins[i] = goc_go(test_p7_4_sender_fn, &sargs[i]);
        ASSERT(joins[i] != NULL);
    }

    /* Receive exactly NSENDERS values. */
    int seen[P7_4_NSENDERS];
    memset(seen, 0, sizeof seen);

    for (int i = 0; i < P7_4_NSENDERS; i++) {
        goc_val_t* v = goc_take_sync(ch);
        ASSERT(v->ok == GOC_OK);
        uintptr_t id = goc_unbox_uint(v->val);
        ASSERT(id < P7_4_NSENDERS);
        ASSERT(seen[id] == 0);   /* no duplicate */
        seen[id] = 1;
    }

    /* All IDs must have been received. */
    for (int i = 0; i < P7_4_NSENDERS; i++) {
        ASSERT(seen[i] == 1);
    }

    /* Wait for all sender fibers to complete. */
    goc_take_all_sync(joins, (size_t)P7_4_NSENDERS);

    goc_close(ch);
    TEST_PASS();
done:;
}

/* --- P7.5: Timeout + cancellation ------------------------------------- */

/*
 * A "slow" fiber computes a result but takes longer than the deadline.
 * A "fast" fiber delivers a result before the deadline.
 * The main thread uses goc_alts with a data channel and a timeout arm to
 * race the two.  Only the fast fiber's result should be observed.  After
 * the select completes, the result channel is closed so the slow fiber
 * can drain cleanly on its next put (GOC_CLOSED), avoiding a hang.
 *
 * Topology:
 *
 *   fast_fiber  ─[result_ch]─┐
 *                             ├─ goc_alts ─→ main thread observes fast result
 *   timeout_ch ──────────────┘
 *
 *   slow_fiber attempts goc_put on result_ch after the select has already
 *   completed; it receives GOC_CLOSED and exits cleanly.
 */

#define P7_5_FAST_DELAY_MS    0
#define P7_5_SLOW_DELAY_MS  150
#define P7_5_TIMEOUT_MS      80

typedef struct {
    goc_chan*  result_ch;
    goc_chan*  ready_ch;
    uint64_t   delay_ms;
    uintptr_t  value;
} p7_5_worker_args_t;

static void test_p7_5_worker_fn(void* arg) {
    p7_5_worker_args_t* a = (p7_5_worker_args_t*)arg;
    if (a->ready_ch) {
        goc_put(a->ready_ch, NULL);
    }
    goc_take(goc_timeout(a->delay_ms));
    /* Ignore the return status: the slow fiber will get GOC_CLOSED if the
     * channel has already been closed by the time it wakes up. */
    goc_put(a->result_ch, goc_box_uint(a->value));
}

typedef struct {
    p7_5_worker_args_t* args;
    goc_chan* blocked_ch;
} fast_fiber_args_t;

static void fast_fiber_wrapper(void* arg) {
    fast_fiber_args_t* ffa = (fast_fiber_args_t*)arg;
    if (ffa->args->ready_ch) {
        goc_put(ffa->args->ready_ch, NULL);
    }
    goc_take(goc_timeout(ffa->args->delay_ms));
    /* Signal only after the delay has elapsed: main must not enter goc_alts_sync
     * until the fast fiber is about to put on result_ch, otherwise there is a
     * window where the timeout arm can fire before result_ch becomes ready. */
    goc_put(ffa->blocked_ch, NULL);
    goc_put(ffa->args->result_ch, goc_box_uint(ffa->args->value));
}

/*
 * P7.5 — Timeout + cancellation: slower fiber's result discarded cleanly
 *
 * The main thread races a data channel against a timeout using goc_alts_sync.
 * The fast fiber wins; the correct value is received.  The timeout arm must
 * NOT fire.  After the select, result_ch is closed; the slow fiber's
 * subsequent goc_put returns GOC_CLOSED and the fiber exits without hanging.
 * All joins must complete, proving no goroutine leak.
 *
 * NOTE: A readiness handshake is used to make this deterministic across
 * platforms: the fast worker signals once it reaches its send point before
 * goc_alts_sync starts, so the data arm is pending immediately.
 */
static void test_p7_5(void) {
    TEST_BEGIN("P7.5   timeout+cancellation: slow result discarded, no hang");

    goc_chan* result_ch = goc_chan_make(0);
    ASSERT(result_ch != NULL);
    goc_chan* fast_ready_ch = goc_chan_make(0);
    ASSERT(fast_ready_ch != NULL);
    goc_chan* fast_blocked_ch = goc_chan_make(0);
    ASSERT(fast_blocked_ch != NULL);

    p7_5_worker_args_t fast_args = {
        .result_ch = result_ch,
        .ready_ch  = fast_ready_ch,
        .delay_ms  = P7_5_FAST_DELAY_MS,
        .value     = 0xFADE,
    };
    p7_5_worker_args_t slow_args = {
        .result_ch = result_ch,
        .ready_ch  = NULL,
        .delay_ms  = P7_5_SLOW_DELAY_MS,
        .value     = 0xDEAD,
    };

    fast_fiber_args_t fast_fiber_args = { .args = &fast_args, .blocked_ch = fast_blocked_ch };
    goc_chan* fast_join = goc_go(fast_fiber_wrapper, &fast_fiber_args);
    ASSERT(fast_join != NULL);
    goc_chan* slow_join = goc_go(test_p7_5_worker_fn, &slow_args);
    ASSERT(slow_join != NULL);

    /* Wait until the fast worker has reached its send point. This removes
     * scheduler jitter from the select race so the test is deterministic: the
     * data arm is already pending when goc_alts_sync starts. */
    goc_take_sync(fast_ready_ch);
    goc_close(fast_ready_ch);
    goc_take_sync(fast_blocked_ch);
    goc_close(fast_blocked_ch);

    goc_chan* tch = goc_timeout(P7_5_TIMEOUT_MS);
    ASSERT(tch != NULL);

    goc_alt_op ops[] = {
        { .ch = result_ch, .op_kind = GOC_ALT_TAKE },   /* index 0: data */
        { .ch = tch,       .op_kind = GOC_ALT_TAKE },   /* index 1: timeout */
    };
    goc_alts_result* r = goc_alts_sync(ops, 2);

    /* Close result_ch unconditionally — ensures that any fiber still parked
     * as a putter (e.g., the fast fiber if the timeout won on a slow runner)
     * receives GOC_CLOSED and exits cleanly regardless of which alts arm won.
     * goc_close is idempotent; calling it again after the normal path is safe. */
    goc_close(result_ch);

    /* Drain all fibers before asserting so that goc_shutdown never waits on
     * a live fiber left behind by an assertion failure jumping to done:.
     * goc_take_sync blocks until each fiber's join channel is closed (i.e.,
     * the fiber has returned and fiber_trampoline has called goc_close). */
    goc_take_sync(fast_join);
    goc_take_sync(slow_join);

    /* Drain the timeout channel: the libuv timer fires at ~P7_5_TIMEOUT_MS ms
     * from creation; by the time we reach here both worker fibers have already
     * completed, so the timer has almost certainly fired.  If not, this call
     * parks briefly until it does. */
    goc_take_sync(tch);

    /* The fast fiber must have won: correct index and value. */
    ASSERT(r->ch == result_ch);
    ASSERT(r->value.ok == GOC_OK);
    ASSERT(goc_unbox_uint(r->value.val) == 0xFADE);

    TEST_PASS();
done:;
}

/* =========================================================================
 * main
 *
 * Initialises the runtime once, runs all Phase 7 tests in order, shuts down
 * the runtime, then prints a summary and exits with 0 on success or 1 if any
 * test failed.
 * ====================================================================== */

int main(void) {
    install_crash_handler();

    printf("libgoc test suite — Phase 7: Integration\n");
    printf("=========================================\n\n");

    goc_init();

    printf("Phase 7 — Integration\n");
    test_p7_1();
    test_p7_2();
    test_p7_3();
    test_p7_4();
    test_p7_5();
    printf("\n");

    goc_shutdown();

    printf("=========================================\n");
    printf("Results: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed > 0) {
        printf(", %d FAILED", g_tests_failed);
    }
    printf("\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
