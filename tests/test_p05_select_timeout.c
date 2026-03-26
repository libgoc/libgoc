/*
 * tests/test_p5_select_timeout.c — Phase 5: Select and timeout tests for libgoc
 *
 * Verifies goc_alts() (fiber-context select), goc_alts_sync() (OS-thread
 * select), and goc_timeout() (libuv-backed deadline channel).  Tests exercise
 * the fast-path (immediately-ready arm), the park path (arm not ready at call
 * time), the GOC_ALT_DEFAULT arm, closed-channel fast and slow paths, and the
 * timeout arm in a select.
 *
 * Build:  cmake -B build && cmake --build build
 * Run:    ctest --test-dir build --output-on-failure
 *         ./build/test_p5_select_timeout
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
 *   the need for a goc_chan in tests that are themselves verifying channel and
 *   select behaviour, keeping each test self-contained.
 *
 *     done_init(&d)    — initialise the mutex and condvar
 *     done_signal(&d)  — set flag and signal — called by the fiber on exit
 *     done_wait(&d)    — wait until flag is set — called by the test thread
 *     done_destroy(&d) — destroy the mutex and condvar
 *
 * Test coverage (Phase 5 — Select and timeout):
 *
 *   P5.1   goc_alts take arm: immediately-ready channel; correct index and value
 *   P5.2   goc_alts put arm: immediately-ready taker; result {NULL, GOC_OK}
 *   P5.3   goc_alts parks on take arm then wakes when value arrives
 *   P5.4   goc_alts parks on put arm then wakes when taker arrives
 *   P5.5   goc_alts GOC_ALT_DEFAULT fires when no other arm is ready
 *   P5.6   goc_alts GOC_ALT_DEFAULT: non-default ready arm wins; default does not fire
 *   P5.7   goc_alts take: channel already closed at call time — fast path returns
 *          ok==GOC_CLOSED without parking
 *   P5.8   goc_alts take: channel closed while fiber is parked — wakes with GOC_CLOSED
 *   P5.9   goc_alts put: channel closed while fiber is parked — wakes with GOC_CLOSED
 *   P5.10  goc_alts timeout arm fires when data channel stays empty for the deadline
 *   P5.11  goc_alts_sync blocks OS thread on take arm until a fiber sends a value
 *   P5.12  goc_alts_sync blocks OS thread on put arm until a fiber takes the value
 *   P5.13  goc_timeout closes its channel after the deadline, not before
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
#include <time.h>
#include <pthread.h>
#include <uv.h>

#include "test_harness.h"
#include "goc.h"
#include "goc_stats.h"

/* =========================================================================
 * done_t — lightweight fiber-to-main synchronisation via mutex + condvar
 *
 * Used throughout Phase 5 to let the main thread block until a target fiber
 * signals completion.  Choosing mutex+condvar rather than a goc_chan keeps
 * each test independent of the channel machinery it is trying to verify.
 * ====================================================================== */

typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cond;
    int             flag;
} done_t;

static void done_init(done_t* d) {
    pthread_mutex_init(&d->mtx, NULL);
    pthread_cond_init(&d->cond, NULL);
    d->flag = 0;
}
static void done_signal(done_t* d) {
    pthread_mutex_lock(&d->mtx);
    d->flag = 1;
    pthread_cond_signal(&d->cond);
    pthread_mutex_unlock(&d->mtx);
}
static void done_wait(done_t* d) {
    pthread_mutex_lock(&d->mtx);
    while (!d->flag)
        pthread_cond_wait(&d->cond, &d->mtx);
    d->flag = 0;
    pthread_mutex_unlock(&d->mtx);
}
static void done_destroy(done_t* d) {
    pthread_mutex_destroy(&d->mtx);
    pthread_cond_destroy(&d->cond);
}

/* =========================================================================
 * Phase 5 — Select and timeout
 * ====================================================================== */

/* --- P5.1: immediately-ready take arm ---------------------------------- */

/*
 * Argument bundle for test_p5_1_fiber_fn.
 *
 * ch      — channel the fiber will call goc_alts on; pre-loaded with a value
 *            by the main thread before the fiber is launched
 * done    — signalled by the fiber when it has recorded its result
 * result  — populated by the fiber with the goc_alts_result
 */
typedef struct {
    goc_chan*       ch;
    done_t*         done;
    goc_alts_result* result;
} p5_1_args_t;

/*
 * Fiber for P5.1.
 * Calls goc_alts with a single take arm on a channel that already holds a
 * value.  The arm must fire immediately (fast path, no park) and return the
 * correct index and value.
 */
static void test_p5_1_fiber_fn(void* arg) {
    p5_1_args_t* a = (p5_1_args_t*)arg;
    goc_alt_op ops[] = {
        { .ch = a->ch, .op_kind = GOC_ALT_TAKE },
    };
    a->result = goc_alts(ops, 1);
    done_signal(a->done);
}

/*
 * P5.1 — goc_alts selects the immediately-ready take arm (no parking)
 *
 * A buffered channel is pre-loaded with a known value from the main thread
 * before the fiber is launched.  When the fiber calls goc_alts, the take arm
 * is immediately ready (fast-path Phase 2 scan): goc_alts must return without
 * parking, with index == 0, ok == GOC_OK, and the correct value.
 */
static void test_p5_1(void) {
    TEST_BEGIN("P5.1   goc_alts: immediately-ready take arm fires");
    done_t done;
    done_init(&done);

    goc_chan* ch = goc_chan_make(1);
    ASSERT(ch != NULL);

    /* Pre-load the channel so the take arm is immediately ready. */
    goc_status_t st = goc_put_sync(ch, goc_box_uint(0xCAFE));
    ASSERT(st == GOC_OK);

    p5_1_args_t args = { .ch = ch, .done = &done };
    goc_chan* join = goc_go(test_p5_1_fiber_fn, &args);
    ASSERT(join != NULL);

    done_wait(&done);

    ASSERT(args.result->ch == args.ch);
    ASSERT(args.result->value.ok == GOC_OK);
    ASSERT(goc_unbox_uint(args.result->value.val) == 0xCAFE);

    goc_val_t* v = goc_take_sync(join);
    ASSERT(v->ok == GOC_CLOSED);

    goc_close(ch);
    done_destroy(&done);
    TEST_PASS();
done:;
}

/* --- P5.2: immediately-ready put arm ----------------------------------- */

/*
 * Argument bundle for test_p5_2_fiber_fn.
 *
 * put_ch   — channel the fiber puts a value onto via goc_alts
 * drain_ch — receives the put_val after the alts returns; the main thread
 *             reads from this channel to prevent the put from blocking
 * done     — signalled by the fiber once the result is populated
 * result   — the goc_alts_result from the fiber
 */
typedef struct {
    goc_chan*       put_ch;
    done_t*         done;
    goc_alts_result* result;
} p5_2_args_t;

/*
 * Taker fiber for P5.2 — parks on put_ch waiting for the alts fiber.
 */
typedef struct {
    goc_chan* put_ch;
    done_t*   ready;   /* signalled just before goc_take parks */
} p5_2_taker_args_t;

static void test_p5_2_taker_fn(void* arg) {
    p5_2_taker_args_t* a = (p5_2_taker_args_t*)arg;
    done_signal(a->ready);   /* tell main the taker is about to park */
    goc_take(a->put_ch);     /* park until the alts fiber puts */
}

/*
 * Fiber for P5.2.
 * Calls goc_alts with a single put arm after a taker fiber is already parked
 * on the channel.  The arm must fire immediately and return {NULL, GOC_OK}
 * with index == 0.
 */
static void test_p5_2_fiber_fn(void* arg) {
    p5_2_args_t* a = (p5_2_args_t*)arg;
    goc_alt_op ops[] = {
        { .ch = a->put_ch, .op_kind = GOC_ALT_PUT, .put_val = goc_box_uint(0xBEEF) },
    };
    a->result = goc_alts(ops, 1);
    done_signal(a->done);
}

/*
 * P5.2 — goc_alts fires the put arm immediately when a taker is parked
 *
 * A taker fiber parks on a rendezvous channel.  The alts fiber then calls
 * goc_alts with a put arm on that channel; the put should complete
 * immediately (taker already waiting) and the result must be {NULL, GOC_OK}
 * with index == 0.
 */
static void test_p5_2(void) {
    TEST_BEGIN("P5.2   goc_alts: immediately-ready put arm fires");
    done_t taker_ready, alts_done;
    done_init(&taker_ready);
    done_init(&alts_done);

    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    /* Launch taker fiber first; wait until it is parked. */
    p5_2_taker_args_t targs = { .put_ch = ch, .ready = &taker_ready };
    goc_chan* taker_join = goc_go(test_p5_2_taker_fn, &targs);
    ASSERT(taker_join != NULL);
    done_wait(&taker_ready);

    /* Now launch the alts fiber; the put arm should fire immediately. */
    p5_2_args_t args = { .put_ch = ch, .done = &alts_done };
    goc_chan* alts_join = goc_go(test_p5_2_fiber_fn, &args);
    ASSERT(alts_join != NULL);

    done_wait(&alts_done);

    ASSERT(args.result->ch == args.put_ch);
    ASSERT(args.result->value.ok == GOC_OK);
    ASSERT(args.result->value.val == NULL); /* put arms always return NULL val */

    goc_val_t* v;
    v = goc_take_sync(taker_join);
    ASSERT(v->ok == GOC_CLOSED);
    v = goc_take_sync(alts_join);
    ASSERT(v->ok == GOC_CLOSED);

    goc_close(ch);
    done_destroy(&taker_ready);
    done_destroy(&alts_done);
    TEST_PASS();
done:;
}

/* --- P5.3: parks on take arm then wakes when value arrives ------------- */

/*
 * Argument bundle for the alts-take fiber in P5.3.
 *
 * ch     — channel to take from via goc_alts
 * ready  — signalled by the fiber just before it calls goc_alts (so the main
 *           thread can wait until the fiber is about to park)
 * done   — signalled once goc_alts returns
 * result — the goc_alts_result
 */
typedef struct {
    goc_chan*       ch;
    done_t*         ready;
    done_t*         done;
    goc_alts_result* result;
} p5_3_args_t;

static void test_p5_3_fiber_fn(void* arg) {
    p5_3_args_t* a = (p5_3_args_t*)arg;
    done_signal(a->ready);  /* let main know we are about to call goc_alts */
    goc_alt_op ops[] = {
        { .ch = a->ch, .op_kind = GOC_ALT_TAKE },
    };
    a->result = goc_alts(ops, 1);
    done_signal(a->done);
}

/*
 * P5.3 — goc_alts parks on a take arm then wakes when a value arrives
 *
 * The fiber calls goc_alts on an empty rendezvous channel; it must park.
 * The main thread then delivers a value via goc_put_sync.  The fiber must
 * wake with index == 0, ok == GOC_OK, and the correct value.
 */
static void test_p5_3(void) {
    TEST_BEGIN("P5.3   goc_alts: parks on take arm, wakes on value");
    done_t ready, done;
    done_init(&ready);
    done_init(&done);

    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    p5_3_args_t args = { .ch = ch, .ready = &ready, .done = &done };
    goc_chan* join = goc_go(test_p5_3_fiber_fn, &args);
    ASSERT(join != NULL);

    done_wait(&ready);
    /* Small yield to allow the fiber to actually park inside goc_alts before
     * we put.  The ready semaphore is posted just before goc_alts is called,
     * so there is a narrow window; a brief sleep closes it reliably. */
    goc_nanosleep(5000000); /* 5 ms */

    goc_status_t st = goc_put_sync(ch, goc_box_uint(0x1234));
    ASSERT(st == GOC_OK);

    done_wait(&done);

    ASSERT(args.result->ch == args.ch);
    ASSERT(args.result->value.ok == GOC_OK);
    ASSERT(goc_unbox_uint(args.result->value.val) == 0x1234);

    goc_val_t* v = goc_take_sync(join);
    ASSERT(v->ok == GOC_CLOSED);

    goc_close(ch);
    done_destroy(&ready);
    done_destroy(&done);
    TEST_PASS();
done:;
}

/* --- P5.4: parks on put arm then wakes when taker arrives -------------- */

/*
 * Argument bundle for the alts-put fiber in P5.4.
 */
typedef struct {
    goc_chan*       ch;
    done_t*         ready;
    done_t*         done;
    goc_alts_result* result;
} p5_4_args_t;

static void test_p5_4_fiber_fn(void* arg) {
    p5_4_args_t* a = (p5_4_args_t*)arg;
    done_signal(a->ready);
    goc_alt_op ops[] = {
        { .ch = a->ch, .op_kind = GOC_ALT_PUT, .put_val = goc_box_uint(0x5678) },
    };
    a->result = goc_alts(ops, 1);
    done_signal(a->done);
}

/*
 * P5.4 — goc_alts parks on a put arm then wakes when a taker arrives
 *
 * The fiber calls goc_alts with a put arm on a rendezvous channel that has no
 * taker; it must park.  The main thread then calls goc_take_sync to consume the
 * value.  The fiber must wake with index == 0, ok == GOC_OK, val == NULL.
 */
static void test_p5_4(void) {
    TEST_BEGIN("P5.4   goc_alts: parks on put arm, wakes when taker arrives");
    done_t ready, done;
    done_init(&ready);
    done_init(&done);

    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    p5_4_args_t args = { .ch = ch, .ready = &ready, .done = &done };
    goc_chan* join = goc_go(test_p5_4_fiber_fn, &args);
    ASSERT(join != NULL);

    done_wait(&ready);
    goc_nanosleep(5000000); /* 5 ms */

    goc_val_t* v = goc_take_sync(ch);
    ASSERT(v->ok == GOC_OK);
    ASSERT(goc_unbox_uint(v->val) == 0x5678);

    done_wait(&done);

    ASSERT(args.result->ch == args.ch);
    ASSERT(args.result->value.ok == GOC_OK);
    ASSERT(args.result->value.val == NULL);

    v = goc_take_sync(join);
    ASSERT(v->ok == GOC_CLOSED);

    goc_close(ch);
    done_destroy(&ready);
    done_destroy(&done);
    TEST_PASS();
done:;
}

/* --- P5.5: GOC_ALT_DEFAULT fires when no other arm is ready ------------ */

/*
 * Argument bundle for the default-arm fiber in P5.5.
 */
typedef struct {
    goc_chan*       ch;
    done_t*         done;
    goc_alts_result* result;
} p5_5_args_t;

static void test_p5_5_fiber_fn(void* arg) {
    p5_5_args_t* a = (p5_5_args_t*)arg;
    goc_alt_op ops[] = {
        { .ch = a->ch,  .op_kind = GOC_ALT_TAKE },
        { .ch = NULL,   .op_kind = GOC_ALT_DEFAULT },
    };
    a->result = goc_alts(ops, 2);
    done_signal(a->done);
}

/*
 * P5.5 — GOC_ALT_DEFAULT fires when no other arm is immediately ready
 *
 * The data channel is empty and closed-channel fast path does not apply
 * (channel is open).  goc_alts must return the default arm's index (1) with
 * {NULL, GOC_OK} without parking the fiber.
 */
static void test_p5_5(void) {
    TEST_BEGIN("P5.5   goc_alts: DEFAULT fires when no other arm is ready");
    done_t done;
    done_init(&done);

    goc_chan* ch = goc_chan_make(0);  /* empty, no parked putters */
    ASSERT(ch != NULL);

    p5_5_args_t args = { .ch = ch, .done = &done };
    goc_chan* join = goc_go(test_p5_5_fiber_fn, &args);
    ASSERT(join != NULL);

    done_wait(&done);

    ASSERT(args.result->ch == NULL);           /* default arm: ch is NULL */
    ASSERT(args.result->value.ok == GOC_OK);
    ASSERT(args.result->value.val == NULL);

    goc_val_t* v = goc_take_sync(join);
    ASSERT(v->ok == GOC_CLOSED);

    goc_close(ch);
    done_destroy(&done);
    TEST_PASS();
done:;
}

/* --- P5.6: non-default arm wins when immediately ready ----------------- */

/*
 * Argument bundle for the default-vs-ready fiber in P5.6.
 */
typedef struct {
    goc_chan*       ch;
    done_t*         done;
    goc_alts_result* result;
} p5_6_args_t;

static void test_p5_6_fiber_fn(void* arg) {
    p5_6_args_t* a = (p5_6_args_t*)arg;
    goc_alt_op ops[] = {
        { .ch = a->ch, .op_kind = GOC_ALT_TAKE },
        { .ch = NULL,  .op_kind = GOC_ALT_DEFAULT },
    };
    a->result = goc_alts(ops, 2);
    done_signal(a->done);
}

/*
 * P5.6 — non-default arm wins when it is immediately ready
 *
 * The data channel is pre-loaded with a value before the fiber calls goc_alts.
 * The take arm (index 0) must win; the default arm (index 1) must not fire.
 */
static void test_p5_6(void) {
    TEST_BEGIN("P5.6   goc_alts: non-default arm wins when ready");
    done_t done;
    done_init(&done);

    goc_chan* ch = goc_chan_make(1);
    ASSERT(ch != NULL);

    goc_status_t st = goc_put_sync(ch, goc_box_uint(0xABCD));
    ASSERT(st == GOC_OK);

    p5_6_args_t args = { .ch = ch, .done = &done };
    goc_chan* join = goc_go(test_p5_6_fiber_fn, &args);
    ASSERT(join != NULL);

    done_wait(&done);

    ASSERT(args.result->ch == args.ch);         /* take arm wins, not default */
    ASSERT(args.result->value.ok == GOC_OK);
    ASSERT(goc_unbox_uint(args.result->value.val) == 0xABCD);

    goc_val_t* v = goc_take_sync(join);
    ASSERT(v->ok == GOC_CLOSED);

    goc_close(ch);
    done_destroy(&done);
    TEST_PASS();
done:;
}

/* --- P5.7: take arm on already-closed channel — fast path -------------- */

/*
 * Argument bundle for the closed-channel fast-path fiber in P5.7.
 */
typedef struct {
    goc_chan*       ch;
    done_t*         done;
    goc_alts_result* result;
} p5_7_args_t;

static void test_p5_7_fiber_fn(void* arg) {
    p5_7_args_t* a = (p5_7_args_t*)arg;
    goc_alt_op ops[] = {
        { .ch = a->ch, .op_kind = GOC_ALT_TAKE },
    };
    a->result = goc_alts(ops, 1);
    done_signal(a->done);
}

/*
 * P5.7 — take arm on an already-closed channel: fast path, no parking
 *
 * goc_close() is called before the fiber launches.  When the fiber calls
 * goc_alts, the Phase 2 non-blocking scan must detect the closed channel and
 * return immediately with index == 0 and ok == GOC_CLOSED, without parking.
 */
static void test_p5_7(void) {
    TEST_BEGIN("P5.7   goc_alts: closed channel take arm — fast path");
    done_t done;
    done_init(&done);

    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);
    goc_close(ch);  /* close before the fiber is even launched */

    p5_7_args_t args = { .ch = ch, .done = &done };
    goc_chan* join = goc_go(test_p5_7_fiber_fn, &args);
    ASSERT(join != NULL);

    done_wait(&done);

    ASSERT(args.result->ch == args.ch);
    ASSERT(args.result->value.ok == GOC_CLOSED);

    goc_val_t* v = goc_take_sync(join);
    ASSERT(v->ok == GOC_CLOSED);

    done_destroy(&done);
    TEST_PASS();
done:;
}

/* --- P5.8: take arm — channel closed while fiber is parked ------------- */

/*
 * Argument bundle for the parked-take fiber in P5.8.
 */
typedef struct {
    goc_chan*       ch;
    done_t*         ready;
    done_t*         done;
    goc_alts_result* result;
} p5_8_args_t;

static void test_p5_8_fiber_fn(void* arg) {
    p5_8_args_t* a = (p5_8_args_t*)arg;
    done_signal(a->ready);
    goc_alt_op ops[] = {
        { .ch = a->ch, .op_kind = GOC_ALT_TAKE },
    };
    a->result = goc_alts(ops, 1);
    done_signal(a->done);
}

/*
 * P5.8 — take arm: channel closed while fiber is parked in goc_alts
 *
 * The fiber calls goc_alts on an open, empty rendezvous channel and parks.
 * The main thread then closes the channel.  The fiber must wake with
 * index == 0 and ok == GOC_CLOSED; no hang.
 */
static void test_p5_8(void) {
    TEST_BEGIN("P5.8   goc_alts: wakes with CLOSED when parked take ch closed");
    done_t ready, done;
    done_init(&ready);
    done_init(&done);

    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    p5_8_args_t args = { .ch = ch, .ready = &ready, .done = &done };
    goc_chan* join = goc_go(test_p5_8_fiber_fn, &args);
    ASSERT(join != NULL);

    done_wait(&ready);
    goc_nanosleep(5000000); /* 5 ms */

    goc_close(ch);  /* wake the parked fiber with CLOSED */

    done_wait(&done);

    ASSERT(args.result->ch == args.ch);
    ASSERT(args.result->value.ok == GOC_CLOSED);

    goc_val_t* v = goc_take_sync(join);
    ASSERT(v->ok == GOC_CLOSED);

    done_destroy(&ready);
    done_destroy(&done);
    TEST_PASS();
done:;
}

/* --- P5.9: put arm — channel closed while fiber is parked -------------- */

/*
 * Argument bundle for the parked-put fiber in P5.9.
 */
typedef struct {
    goc_chan*       ch;
    done_t*         ready;
    done_t*         done;
    goc_alts_result* result;
} p5_9_args_t;

static void test_p5_9_fiber_fn(void* arg) {
    p5_9_args_t* a = (p5_9_args_t*)arg;
    done_signal(a->ready);
    goc_alt_op ops[] = {
        { .ch = a->ch, .op_kind = GOC_ALT_PUT, .put_val = goc_box_uint(0xDEAD) },
    };
    a->result = goc_alts(ops, 1);
    done_signal(a->done);
}

/*
 * P5.9 — put arm: channel closed while fiber is parked in goc_alts
 *
 * Mirrors P5.8 for the put direction.  The fiber calls goc_alts with a put arm
 * on a rendezvous channel that has no taker, so it parks.  The main thread then
 * closes the channel.  The fiber must wake with index == 0 and ok == GOC_CLOSED.
 */
static void test_p5_9(void) {
    TEST_BEGIN("P5.9   goc_alts: wakes with CLOSED when parked put ch closed");
    done_t ready, done;
    done_init(&ready);
    done_init(&done);

    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    p5_9_args_t args = { .ch = ch, .ready = &ready, .done = &done };
    goc_chan* join = goc_go(test_p5_9_fiber_fn, &args);
    ASSERT(join != NULL);

    done_wait(&ready);
    goc_nanosleep(5000000); /* 5 ms */

    goc_close(ch);

    done_wait(&done);

    ASSERT(args.result->ch == args.ch);
    ASSERT(args.result->value.ok == GOC_CLOSED);

    goc_val_t* v = goc_take_sync(join);
    ASSERT(v->ok == GOC_CLOSED);

    done_destroy(&ready);
    done_destroy(&done);
    TEST_PASS();
done:;
}

/* --- P5.10: timeout arm fires when data channel stays empty ------------ */

/*
 * Argument bundle for the timeout-select fiber in P5.10.
 *
 * data_ch     — data channel that is never written to; the timeout must win
 * timeout_ms  — deadline in milliseconds passed to goc_timeout
 * timeout_ch  — populated by the fiber with the channel returned by goc_timeout;
 *               used by the test to compare against result->ch
 * done        — signalled when goc_alts returns
 * result      — the goc_alts_result
 */
typedef struct {
    goc_chan*       data_ch;
    uint64_t        timeout_ms;
    goc_chan*       timeout_ch;
    done_t*         done;
    goc_alts_result* result;
} p5_10_args_t;

static void test_p5_10_fiber_fn(void* arg) {
    p5_10_args_t* a = (p5_10_args_t*)arg;
    a->timeout_ch = goc_timeout(a->timeout_ms);
    goc_alt_op ops[] = {
        { .ch = a->data_ch,    .op_kind = GOC_ALT_TAKE },
        { .ch = a->timeout_ch, .op_kind = GOC_ALT_TAKE },
    };
    a->result = goc_alts(ops, 2);
    done_signal(a->done);
}

/*
 * P5.10 — timeout arm fires when the data channel stays empty
 *
 * The fiber calls goc_alts with a take arm on an empty data channel and a take
 * arm on a goc_timeout channel (50 ms).  Nobody writes to the data channel, so
 * the timeout arm must win after roughly 50 ms.  result.ok must be
 * GOC_CLOSED (the timer closed the timeout channel).
 */
static void test_p5_10(void) {
    TEST_BEGIN("P5.10  goc_alts: timeout arm fires after deadline");
    done_t done;
    done_init(&done);

    goc_chan* data_ch = goc_chan_make(0);
    ASSERT(data_ch != NULL);

    p5_10_args_t args = {
        .data_ch    = data_ch,
        .timeout_ms = 50,
        .done       = &done,
    };
    goc_chan* join = goc_go(test_p5_10_fiber_fn, &args);
    ASSERT(join != NULL);

    done_wait(&done);

    ASSERT(args.result->ch == args.timeout_ch);  /* timeout arm won */
    ASSERT(args.result->value.ok == GOC_CLOSED); /* timer fired → channel closed */

    goc_val_t* v = goc_take_sync(join);
    ASSERT(v->ok == GOC_CLOSED);

    goc_close(data_ch);
    done_destroy(&done);
    TEST_PASS();
done:;
}

/* --- P5.11: goc_alts_sync blocks OS thread on take arm ----------------- */

/*
 * Sender fiber for P5.11.
 * Waits a short time to ensure the main thread reaches goc_alts_sync first,
 * then puts a value onto the channel.
 */
typedef struct {
    goc_chan* ch;
    uint64_t  delay_us;
} p5_11_sender_args_t;

static void test_p5_11_sender_fn(void* arg) {
    p5_11_sender_args_t* a = (p5_11_sender_args_t*)arg;
    goc_nanosleep((uint64_t)a->delay_us * 1000);
    goc_put(a->ch, goc_box_uint(0x9ABC));
}

/*
 * P5.11 — goc_alts_sync blocks the OS thread on a take arm until a fiber sends
 *
 * The main thread calls goc_alts_sync on an empty rendezvous channel; it must
 * block.  A fiber then sends a value after a short delay.  goc_alts_sync must
 * return with index == 0, ok == GOC_OK, and the correct value.
 */
static void test_p5_11(void) {
    TEST_BEGIN("P5.11  goc_alts_sync: blocks on take arm until fiber sends");

    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    p5_11_sender_args_t sargs = { .ch = ch, .delay_us = 20000 /* 20 ms */ };
    goc_chan* join = goc_go(test_p5_11_sender_fn, &sargs);
    ASSERT(join != NULL);

    goc_alt_op ops[] = {
        { .ch = ch, .op_kind = GOC_ALT_TAKE },
    };
    goc_alts_result* r = goc_alts_sync(ops, 1);

    ASSERT(r->ch == ch);
    ASSERT(r->value.ok == GOC_OK);
    ASSERT(goc_unbox_uint(r->value.val) == 0x9ABC);

    goc_val_t* v = goc_take_sync(join);
    ASSERT(v->ok == GOC_CLOSED);

    goc_close(ch);
    TEST_PASS();
done:;
}

/* --- P5.12: goc_alts_sync blocks OS thread on put arm ------------------ */

/*
 * Receiver fiber for P5.12.
 * Waits a short time then calls goc_take() to consume the value that the main
 * thread is waiting to deliver via goc_alts_sync.
 */
typedef struct {
    goc_chan* ch;
    uint64_t  delay_us;
    uintptr_t received_val;
} p5_12_receiver_args_t;

static void test_p5_12_receiver_fn(void* arg) {
    p5_12_receiver_args_t* a = (p5_12_receiver_args_t*)arg;
    goc_nanosleep((uint64_t)a->delay_us * 1000);
    goc_val_t* v = goc_take(a->ch);
    if (v->ok == GOC_OK) {
        a->received_val = goc_unbox_uint(v->val);
    }
}

/*
 * P5.12 — goc_alts_sync blocks the OS thread on a put arm until a fiber takes
 *
 * The main thread calls goc_alts_sync with a put arm on a rendezvous channel;
 * it must block.  A fiber then calls goc_take after a short delay to consume
 * the value.  goc_alts_sync must return with index == 0, ok == GOC_OK, val == NULL.
 * The fiber is verified to have received the correct value.
 */
static void test_p5_12(void) {
    TEST_BEGIN("P5.12  goc_alts_sync: blocks on put arm until fiber takes");

    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    p5_12_receiver_args_t rargs = { .ch = ch, .delay_us = 20000 /* 20 ms */ };
    goc_chan* join = goc_go(test_p5_12_receiver_fn, &rargs);
    ASSERT(join != NULL);

    goc_alt_op ops[] = {
        { .ch = ch, .op_kind = GOC_ALT_PUT, .put_val = goc_box_uint(0xF00D) },
    };
    goc_alts_result* r = goc_alts_sync(ops, 1);

    ASSERT(r->ch == ch);
    ASSERT(r->value.ok == GOC_OK);
    ASSERT(r->value.val == NULL);

    /* Wait for the fiber to finish and confirm it received the correct value. */
    goc_val_t* v = goc_take_sync(join);
    ASSERT(v->ok == GOC_CLOSED);
    ASSERT(rargs.received_val == 0xF00D);

    goc_close(ch);
    TEST_PASS();
done:;
}

/* --- P5.13: goc_timeout closes its channel after the deadline ---------- */

/*
 * P5.13 — goc_timeout closes its channel after the deadline, not before
 *
 * Measures the elapsed time from goc_timeout() to when goc_take_sync() returns
 * on the timeout channel (which is closed by the libuv timer).  The elapsed
 * time must be at least the requested deadline.  A generous upper bound
 * (10× the deadline) catches gross hangs or premature firing.
 *
 * goc_take_sync() on a rendezvous channel blocks until someone puts a value or
 * closes the channel.  Since goc_timeout creates a rendezvous (buf_size == 0)
 * channel that is only ever closed (never written to), goc_take_sync returns
 * with ok == GOC_CLOSED when the timer fires.
 */
static void test_p5_13(void) {
    TEST_BEGIN("P5.13  goc_timeout: channel closes after deadline, not before");

    const uint64_t timeout_ms = 50;

    goc_chan* tch = goc_timeout(timeout_ms);
    ASSERT(tch != NULL);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    goc_val_t* v = goc_take_sync(tch);

    clock_gettime(CLOCK_MONOTONIC, &t1);

    ASSERT(v->ok == GOC_CLOSED);

    /* Use signed arithmetic so that when tv_nsec wraps (t1.tv_nsec <
     * t0.tv_nsec, which happens when the measurement straddles a second
     * boundary), the negative nanosecond delta is correctly borrowed from the
     * second difference rather than wrapping to a huge uint64_t value. */
    int64_t elapsed_ms =
        (int64_t)(t1.tv_sec  - t0.tv_sec)  * 1000LL +
        (int64_t)(t1.tv_nsec - t0.tv_nsec) / 1000000LL;

    /* Must have waited at least the deadline (allow 5 ms early-fire tolerance
     * for clock resolution and scheduler jitter). */
    ASSERT(elapsed_ms + 5 >= (int64_t)timeout_ms);

    /* Must not have hung indefinitely (upper bound: 10× deadline). */
    ASSERT(elapsed_ms < (int64_t)(timeout_ms * 10));

    TEST_PASS();
done:;
}

/* --- P5.14: multiple channels close while fiber is parked --------------- */

/*
 * Argument bundle for the multi-close parked select fiber in P5.14.
 */
typedef struct {
    goc_chan*       ch1;
    goc_chan*       ch2;
    done_t*         ready;
    done_t*         done;
    goc_alts_result* result;
} p5_14_args_t;

static void test_p5_14_fiber_fn(void* arg) {
    p5_14_args_t* a = (p5_14_args_t*)arg;
    done_signal(a->ready);
    goc_alt_op ops[] = {
        { .ch = a->ch1, .op_kind = GOC_ALT_TAKE },
        { .ch = a->ch2, .op_kind = GOC_ALT_TAKE },
    };
    a->result = goc_alts(ops, 2);
    done_signal(a->done);
}

/*
 * P5.14 — multiple channels close while fiber is parked in goc_alts
 *
 * The fiber parks on two take arms.  The main thread closes both channels
 * back-to-back before the fiber resumes.  The select must wake exactly once
 * and return with ok == GOC_CLOSED without crashing or hanging.
 */
static void test_p5_14(void) {
    TEST_BEGIN("P5.14  goc_alts: single wake when multiple channels close");
    /* P5.10 and P5.13 each allocate a goc_timeout channel, leaving the event
     * loop backlogged with stats events.  Flush before the fiber parks so the
     * loop is uncontested when goc_alts needs to schedule the fiber. */
    goc_stats_flush();
    done_t ready, done;
    done_init(&ready);
    done_init(&done);

    goc_chan* ch1 = goc_chan_make(0);
    goc_chan* ch2 = goc_chan_make(0);
    ASSERT(ch1 != NULL);
    ASSERT(ch2 != NULL);

    p5_14_args_t args = { .ch1 = ch1, .ch2 = ch2, .ready = &ready, .done = &done };
    goc_chan* join = goc_go(test_p5_14_fiber_fn, &args);
    ASSERT(join != NULL);

    done_wait(&ready);
    goc_nanosleep(5000000); /* 5 ms */

    goc_close(ch1);
    goc_close(ch2);

    done_wait(&done);

    ASSERT(args.result->value.ok == GOC_CLOSED);

    goc_val_t* v = goc_take_sync(join);
    ASSERT(v->ok == GOC_CLOSED);

    done_destroy(&ready);
    done_destroy(&done);
    TEST_PASS();
done:;
}

/* --- P5.15: goc_timeout_get_stats reflects test-suite timeout usage ----- */

/*
 * P5.15 — goc_timeout_get_stats reflects timeout allocations and expirations
 *
 * By this point P5.10 and P5.13 have each called goc_timeout() once, so
 * allocations must be >= 2.  Expirations must be <= allocations (a timeout
 * that has not yet fired is not an error).
 */
static void test_p5_15(void) {
    TEST_BEGIN("P5.15  goc_timeout_get_stats: allocs >= 2, expires <= allocs");

    uint64_t allocs, expires;
    goc_timeout_get_stats(&allocs, &expires);

    /* P5.10 and P5.13 each allocate one timeout channel. */
    ASSERT(allocs >= 2);
    ASSERT(expires <= allocs);

    TEST_PASS();
done:;
}

/* --- P5.16: alts close event carries taker_scans ------------------------ */

/*
 * Minimal stats event buffer used only by P5.16.
 */

typedef struct {
    struct goc_stats_event ev;
    int                    found;
} p5_16_capture_t;

static void p5_16_cb(const struct goc_stats_event* ev, void* ud) {
    p5_16_capture_t* c = (p5_16_capture_t*)ud;
    if (ev->type != GOC_STATS_EVENT_CHANNEL_STATUS) return;
    if (ev->data.channel.status != 0) return; /* only capture close events */
    if (!c->found) {
        c->ev    = *ev;
        c->found = 1;
    }
}

typedef struct {
    goc_chan* ch;
    done_t*   ready;
} p5_16_args_t;

static void test_p5_16_fiber_fn(void* arg) {
    p5_16_args_t* a = (p5_16_args_t*)arg;
    done_signal(a->ready); /* signal: about to park in goc_alts */
    goc_alt_op ops[] = { { .ch = a->ch, .op_kind = GOC_ALT_TAKE } };
    goc_alts(ops, 1); /* parks; no putter — increments taker_scans */
}


/*
 * P5.16 — channel close event carries taker_scans >= 1 after alts park
 *
 * A fiber calls goc_alts with a single take arm on an empty channel.  It
 * finds no match, increments taker_scans, then parks.  Closing the channel
 * unblocks the fiber and emits a CHANNEL_STATUS close event.  The event must
 * report taker_scans >= 1.
 */
static void test_p5_16(void) {
    TEST_BEGIN("P5.16  alts close event: taker_scans >= 1 after parked take arm");
    /* Preceding tests (P5.10, P5.13, P5.14) saturate the event loop with stats
     * events.  Flush before the fiber parks so the 5 ms sleep is sufficient for
     * goc_alts to reach mco_yield on an uncontested loop. */
    goc_stats_flush();
    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    /* Register callback after chan_make so the open event is not captured. */
    p5_16_capture_t cap = { .found = 0 };
    goc_stats_set_callback(p5_16_cb, &cap);

    done_t ready;
    done_init(&ready);

    p5_16_args_t args = { .ch = ch, .ready = &ready };
    goc_chan* join = goc_go(test_p5_16_fiber_fn, &args);
    ASSERT(join != NULL);

    /* Wait until the fiber has signalled it is about to park, then flush the
     * stats pipeline.  The flush submits an async event to the libuv loop and
     * blocks until the loop processes it.  Because the fiber is already
     * running on the event loop thread when done_signal fires, the loop cannot
     * process the flush sentinel until after the fiber parks inside goc_alts
     * (i.e. after taker_scans has been incremented).  This is a deterministic
     * barrier — no sleep required. */
    done_wait(&ready);
    goc_stats_flush();

    /* Closing the channel unblocks the fiber and emits the close event. */
    goc_close(ch);
    goc_take_sync(join); /* wait for fiber to finish */

    /* Flush the stats delivery pipeline so the close event reaches p5_16_cb
     * before we read cap.found. */
    goc_stats_flush();

    ASSERT(cap.found);
    ASSERT(cap.ev.data.channel.taker_scans >= 1);

    goc_stats_set_callback(NULL, NULL);
    done_destroy(&ready);

    TEST_PASS();
done:;
}

/* =========================================================================
 * main
 *
 * Initialises the runtime once, runs all Phase 5 tests in order, shuts down
 * the runtime, then prints a summary and exits with 0 on success or 1 if any
 * test failed.
 * ====================================================================== */

int main(void) {
    install_crash_handler();
    goc_test_arm_watchdog(30);

    printf("libgoc test suite — Phase 5: Select and timeout\n");
    printf("=================================================\n\n");

    goc_init();
    goc_stats_init();
    /* Suppress the verbose default callback; tests install their own. */
    goc_stats_set_callback(NULL, NULL);

    printf("Phase 5 — Select and timeout\n");
    test_p5_1();
    test_p5_2();
    test_p5_3();
    test_p5_4();
    test_p5_5();
    test_p5_6();
    test_p5_7();
    test_p5_8();
    test_p5_9();
    test_p5_10();
    test_p5_11();
    test_p5_12();
    test_p5_13();
    test_p5_14();
    test_p5_15();
    test_p5_16();
    printf("\n");

    goc_stats_shutdown();
    goc_shutdown();

    printf("=================================================\n");
    printf("Results: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed > 0) {
        printf(", %d FAILED", g_tests_failed);
    }
    printf("\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
