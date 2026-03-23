/*
 * tests/test_p2_channels_fibers.c — Phase 2: Channels and fiber launch tests
 * for libgoc
 *
 * Verifies channel creation and lifecycle, fiber spawning via goc_go(), the
 * join-channel mechanism, and the goc_in_fiber() predicate in fiber context.
 * These tests build directly on the foundation established in Phase 1 and
 * must pass before any channel I/O tests (Phase 3+) are attempted.
 *
 * Build:  cmake -B build && cmake --build build
 * Run:    ctest --test-dir build --output-on-failure
 *         ./build/test_p2_channels_fibers
 *
 * Compile requirements: -std=c11 -DGC_THREADS -D_GNU_SOURCE
 *
 * Dependencies:
 *   - libgoc (goc.h)  — runtime under test
 *   - Boehm GC        — must be the threaded variant (bdw-gc-threaded);
 *                        initialised internally by goc_init()
 *   - libuv           — event loop; drives fiber scheduling, provides
 *                        uv_thread_t / uv_mutex_t for done_t and P2.3
 *
 * Synchronisation helper — done_t:
 *   A portable mutex+condvar semaphore that lets the main thread (or a
 *   waiter fiber) block until a fiber signals completion.  Using
 *   mutex+condvar avoids the need for a goc_chan in tests that are
 *   themselves verifying channel/fiber behaviour, keeping each test
 *   self-contained.
 *
 *     done_init(&d)    — initialise the mutex and condvar
 *     done_signal(&d)  — set flag and signal — called by the fiber on exit
 *     done_wait(&d)    — wait until flag is set — called by the test thread
 *     done_destroy(&d) — destroy the mutex and condvar
 *
 * Test coverage (Phase 2 — Channels and fiber launch):
 *
 *   P2.1  goc_chan_make(0) returns non-NULL rendezvous channel (no buffer)
 *   P2.2  goc_chan_make(16) returns non-NULL buffered channel; accepts a
 *         value without a waiting taker and delivers it correctly
 *   P2.3  goc_close() is idempotent: concurrent double-close from two threads
 *         both return without error, channel state is consistent, and no
 *         mutex is double-destroyed (exercises the close_guard CAS path)
 *   P2.4  goc_go() launches a fiber that runs to completion; the join channel
 *         returned by goc_go() is closed when the fiber's entry function returns
 *   P2.5  goc_go() passes the arg pointer through to the fiber unchanged
 *   P2.6  goc_in_fiber() returns true when called from within a fiber body
 *   P2.7  Join channel (OS thread side): goc_take_sync() on the join channel
 *         blocks the main thread until the fiber returns, then unblocks with
 *         ok == GOC_CLOSED
 *   P2.8  Join channel (fiber side): goc_take() on the join channel from a
 *         second fiber parks that fiber until the target fiber returns, then
 *         delivers ok == GOC_CLOSED
 *
 * Notes:
 *   - goc_init() is called once in main() before any test runs.
 *   - goc_shutdown() is called once in main() after all tests complete.
 *   - The test harness uses the same goto-based cleanup pattern as Phase 1;
 *     see the harness section comments for details.
 *   - P2.3 is the only test that creates a raw OS thread directly (via
 *     uv_thread_create); all others use goc_go() for fiber launch.
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
 * Used throughout Phase 2 to let the main thread (or a waiter fiber) block
 * until a target fiber signals that it has completed a particular step.
 * Choosing mutex+condvar rather than a goc_chan keeps each test independent
 * of the channel machinery it is trying to verify.
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
 * Phase 2 — Channels and fiber launch
 * ====================================================================== */

/*
 * P2.1 — goc_chan_make(0) returns non-NULL (rendezvous channel)
 *
 * A capacity of 0 creates a rendezvous channel: a put and a take must
 * meet simultaneously for a value to be transferred.  The channel itself
 * is GC-heap allocated; goc_close() is called here to exercise cleanup, not
 * because a test would hang without it.
 */
static void test_p2_1(void) {
    TEST_BEGIN("P2.1  goc_chan_make(0) returns non-NULL (rendezvous)");
    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);
    goc_close(ch);
    TEST_PASS();
done:;
}

/*
 * P2.2 — goc_chan_make(16) returns non-NULL (buffered channel)
 *
 * A capacity of 16 means up to 16 values can be enqueued without a taker.
 * To confirm that buffering actually works, the test performs a synchronous
 * put from the main OS thread (goc_put_sync) followed immediately by a
 * synchronous take (goc_take_sync) and verifies that the value is preserved.
 * No fiber is needed because the put can complete without a waiting taker.
 */
static void test_p2_2(void) {
    TEST_BEGIN("P2.2  goc_chan_make(16) returns non-NULL (buffered)");
    goc_chan* ch = goc_chan_make(16);
    ASSERT(ch != NULL);
    /* A buffered channel of capacity 16 should accept values without a taker.
     * Put 1 value synchronously from an OS thread to confirm buffering works. */
    goc_status_t st = goc_put_sync(ch, (void*)(uintptr_t)42);
    ASSERT(st == GOC_OK);
    goc_val_t* v = goc_take_sync(ch);
    ASSERT(v->ok == GOC_OK);
    ASSERT((uintptr_t)v->val == 42);
    goc_close(ch);
    TEST_PASS();
done:;
}

/* --- P2.3: concurrent double-close ------------------------------------ */

/*
 * Argument bundle shared between the main thread and the closer thread in
 * the P2.3 double-close test.
 *
 * ch    — the channel both threads will attempt to close
 * ready — signalled by the closer thread once it is about to call goc_close;
 *          the main thread waits on this before proceeding, ensuring both
 *          threads are as close in time as possible when they each call close
 * go    — signalled by the main thread to release the closer thread; both
 *          threads call goc_close immediately after this is posted
 */
typedef struct {
    goc_chan* ch;
    done_t*   ready; /* signals that closer thread is about to close */
    done_t*   go;    /* released when both threads should call goc_close */
} double_close_args_t;

/*
 * Thread entry point for P2.3.
 * Signals ready, waits for the go signal, then calls goc_close.
 * The racing call from the main thread exercises the close_guard CAS path
 * inside goc_close, which ensures idempotency under concurrent access.
 */
static void double_close_thread(void* arg) {
    double_close_args_t* a = (double_close_args_t*)arg;
    done_signal(a->ready);
    done_wait(a->go);
    goc_close(a->ch);
}

/*
 * P2.3 — goc_close() concurrent double-close is idempotent
 *
 * goc_close() must be safe to call simultaneously from two threads on the
 * same channel.  Only the first caller performs the actual close (CAS on
 * close_guard succeeds); the second becomes a no-op.  Neither call may crash,
 * return an error, or leave the channel in an inconsistent state, and the
 * channel's internal mutex must not be double-destroyed.
 *
 * Strategy: spawn a pthread that synchronises with the main thread via two
 * semaphores so that both call goc_close as close together as possible.
 */
static void test_p2_3(void) {
    TEST_BEGIN("P2.3  goc_close concurrent double-close is idempotent");
    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    done_t ready, go;
    done_init(&ready);
    done_init(&go);

    double_close_args_t args = { ch, &ready, &go };

    uv_thread_t tid;
    uv_thread_create(&tid, double_close_thread, &args);

    done_wait(&ready);
    /* Both the spawned thread and this thread will call goc_close. Release
     * the other thread and immediately call goc_close ourselves. */
    done_signal(&go);
    goc_close(ch);

    uv_thread_join(&tid);

    done_destroy(&ready);
    done_destroy(&go);

    TEST_PASS();
done:;
}

/* --- P2.4 / P2.5 / P2.6 helpers -------------------------------------- */

/*
 * Probe struct used by fiber_probe_fn to report back to the test.
 *
 * expected_arg   — pointer the test places into the struct before launching
 *                   the fiber; used in P2.5 to verify arg pass-through
 * received_arg   — filled in by the fiber with the arg it received
 * in_fiber_flag  — set to goc_in_fiber() result from inside the fiber (P2.6)
 * done           — signalled by the fiber just before it returns, so the
 *                   main thread knows the probe fields are populated
 */
typedef struct {
    void*   expected_arg;
    void*   received_arg;
    bool    in_fiber_flag;
    done_t* done;
} fiber_probe_t;

/*
 * Generic fiber entry point used by P2.4, P2.5, and P2.6.
 *
 * Fills in the probe fields and then signals the done semaphore.  Note that
 * arg IS a pointer to the probe struct itself, so received_arg == arg is the
 * expected result for P2.5.
 */
static void fiber_probe_fn(void* arg) {
    fiber_probe_t* p = (fiber_probe_t*)arg;
    p->received_arg   = arg;           /* note: arg IS the probe struct itself */
    p->in_fiber_flag  = goc_in_fiber();
    done_signal(p->done);
}

/*
 * P2.4 — goc_go() launches a fiber that runs to completion; join channel is
 *         closed when the fiber's entry function returns
 *
 * goc_go() returns a rendezvous join channel that is closed by the runtime
 * immediately after the fiber's entry function returns.  The test:
 *   1. Launches a fiber via goc_go().
 *   2. Waits (via semaphore) until the fiber has finished executing.
 *   3. Calls goc_take_sync() on the join channel and asserts ok == GOC_CLOSED.
 *
 * A GOC_CLOSED result confirms the channel was closed (i.e. the fiber exited)
 * rather than returning a spurious value or hanging.
 */
static void test_p2_4(void) {
    TEST_BEGIN("P2.4  goc_go launches fiber; join channel closed on return");
    done_t done;
    done_init(&done);

    fiber_probe_t probe = {
        .expected_arg  = NULL,
        .received_arg  = NULL,
        .in_fiber_flag = false,
        .done          = &done,
    };

    goc_chan* join = goc_go(fiber_probe_fn, &probe);
    ASSERT(join != NULL);

    done_wait(&done);

    /* Join channel must be closed (fiber has returned). */
    goc_val_t* v = goc_take_sync(join);
    ASSERT(v->ok == GOC_CLOSED);

    done_destroy(&done);
    TEST_PASS();
done:;
}

/*
 * P2.5 — goc_go() passes the arg pointer to the fiber without modification
 *
 * The runtime must deliver the exact pointer passed to goc_go() to the fiber's
 * entry function.  The probe struct is used both as the arg and as the
 * communication buffer, so expected_arg == &probe == received_arg.
 */
static void test_p2_5(void) {
    TEST_BEGIN("P2.5  goc_go passes arg pointer through to fiber");
    done_t done;
    done_init(&done);

    fiber_probe_t probe = {
        .expected_arg  = NULL,
        .received_arg  = NULL,
        .in_fiber_flag = false,
        .done          = &done,
    };
    probe.expected_arg = &probe; /* the fiber receives a pointer to probe */

    goc_chan* join = goc_go(fiber_probe_fn, &probe);
    ASSERT(join != NULL);

    done_wait(&done);

    ASSERT(probe.received_arg == probe.expected_arg);

    goc_val_t* v = goc_take_sync(join);
    ASSERT(v->ok == GOC_CLOSED);

    done_destroy(&done);
    TEST_PASS();
done:;
}

/*
 * P2.6 — goc_in_fiber() returns true from within a fiber
 *
 * The complement of P1.4.  fiber_probe_fn records goc_in_fiber() while
 * executing on the pool thread inside the fiber's minicoro coroutine stack.
 * The main thread reads the result after the done semaphore fires.
 */
static void test_p2_6(void) {
    TEST_BEGIN("P2.6  goc_in_fiber() returns true from within a fiber");
    done_t done;
    done_init(&done);

    fiber_probe_t probe = {
        .expected_arg  = NULL,
        .received_arg  = NULL,
        .in_fiber_flag = false,
        .done          = &done,
    };

    goc_chan* join = goc_go(fiber_probe_fn, &probe);
    ASSERT(join != NULL);

    done_wait(&done);

    ASSERT(probe.in_fiber_flag == true);

    goc_val_t* v = goc_take_sync(join);
    ASSERT(v->ok == GOC_CLOSED);

    done_destroy(&done);
    TEST_PASS();
done:;
}

/* --- P2.7: goc_take_sync blocks until join channel closes ------------- */

/*
 * Argument bundle for slow_fiber_fn used in P2.7.
 *
 * sleep_us — duration in microseconds the fiber sleeps to simulate work;
 *             large enough to make a race between the fiber and the main
 *             thread observable, small enough to keep tests fast
 * done     — signalled by the fiber after the sleep, confirming it ran
 */
typedef struct {
    uint64_t  sleep_us;  /* how long the fiber sleeps (busy-spin free) */
    done_t*   done;
} slow_fiber_args_t;

/*
 * Fiber entry point for P2.7.
 * Sleeps for sleep_us microseconds then signals done before returning.
 * goc_nanosleep() is used for cross-platform compatibility.
 */
static void slow_fiber_fn(void* arg) {
    slow_fiber_args_t* a = (slow_fiber_args_t*)arg;
    /* Simulate work with a short sleep on the fiber's OS thread. */
    goc_nanosleep((uint64_t)a->sleep_us * 1000);
    done_signal(a->done);
}

/*
 * P2.7 — goc_take_sync() on the join channel blocks until the fiber returns
 *
 * The main thread calls goc_take_sync() on the join channel before the fiber
 * has finished its 20 ms sleep.  It must block until the fiber returns and
 * the runtime closes the join channel.  The done semaphore is then checked to
 * confirm that the fiber ran fully before goc_take_sync() could return.
 *
 * This also verifies that the OS-thread blocking path in the channel
 * implementation correctly wakes the caller when the channel is closed.
 */
static void test_p2_7(void) {
    TEST_BEGIN("P2.7  join: goc_take_sync blocks until fiber returns");
    done_t done;
    done_init(&done);

    slow_fiber_args_t args = { .sleep_us = 20000 /* 20 ms */, .done = &done };

    goc_chan* join = goc_go(slow_fiber_fn, &args);
    ASSERT(join != NULL);

    /* Block the main thread until the join channel is closed. */
    goc_val_t* v = goc_take_sync(join);
    ASSERT(v->ok == GOC_CLOSED);

    /* Fiber must have signalled before we could unblock. */
    /* (done_wait would return immediately if already signalled) */
    done_wait(&done);

    done_destroy(&done);
    TEST_PASS();
done:;
}

/* --- P2.8: goc_take from a second fiber blocks until target returns --- */

/*
 * Argument bundle for waiter_fiber_fn used in P2.8.
 *
 * join        — join channel of the target fiber; waiter_fiber_fn calls
 *                goc_take() on this channel, which parks it until the target
 *                fiber returns and the channel is closed
 * done        — signalled by the waiter fiber when goc_take() returns, so
 *                the main thread knows the result is populated
 * saw_closed  — set to true if goc_take() returned ok == GOC_CLOSED, which
 *                is the expected outcome when taking from a join channel
 */
typedef struct {
    goc_chan* join;   /* join channel of the "target" fiber */
    done_t*   done;  /* signals the outer test when this fiber completes */
    bool      saw_closed;
} waiter_fiber_args_t;

/*
 * Fiber entry point for the waiter in P2.8.
 * Parks on the target fiber's join channel via goc_take() (fiber-context
 * blocking), then records the result and signals the main thread.
 */
static void waiter_fiber_fn(void* arg) {
    waiter_fiber_args_t* a = (waiter_fiber_args_t*)arg;
    goc_val_t* v = goc_take(a->join);
    a->saw_closed = (v->ok == GOC_CLOSED);
    done_signal(a->done);
}

/*
 * Minimal no-op fiber used as the "target" in P2.8.
 * Returns immediately so its join channel is closed quickly.
 */
static void noop_fiber_fn(void* arg) {
    (void)arg;
}

/*
 * P2.8 — goc_take() from a second fiber parks until the target fiber returns
 *
 * This verifies the fiber-context blocking path on a join channel:
 *   1. A target fiber (noop_fiber_fn) is launched; it returns immediately.
 *   2. A waiter fiber (waiter_fiber_fn) is launched; it calls goc_take() on
 *      the target's join channel.  If the target has not yet returned, the
 *      waiter parks.  Either way it must eventually receive GOC_CLOSED.
 *   3. The main thread waits for the waiter to signal done, then verifies
 *      saw_closed == true and cleans up both join channels.
 *
 * Unlike P2.7 this exercises goc_take() (fiber-context suspend/resume) rather
 * than goc_take_sync() (OS-thread condition variable).
 */
static void test_p2_8(void) {
    TEST_BEGIN("P2.8  join: goc_take from second fiber suspends until target returns");
    done_t done;
    done_init(&done);

    /* Spawn the target fiber. */
    goc_chan* target_join = goc_go(noop_fiber_fn, NULL);
    ASSERT(target_join != NULL);

    /* Spawn a waiter fiber that calls goc_take on the target's join channel. */
    waiter_fiber_args_t wargs = {
        .join       = target_join,
        .done       = &done,
        .saw_closed = false,
    };
    goc_chan* waiter_join = goc_go(waiter_fiber_fn, &wargs);
    ASSERT(waiter_join != NULL);

    /* Wait for the waiter fiber to finish. */
    done_wait(&done);

    ASSERT(wargs.saw_closed == true);

    /* Clean up both join channels. */
    goc_val_t* v;
    v = goc_take_sync(target_join);
    ASSERT(v->ok == GOC_CLOSED);
    v = goc_take_sync(waiter_join);
    ASSERT(v->ok == GOC_CLOSED);

    done_destroy(&done);
    TEST_PASS();
done:;
}

/* =========================================================================
 * main
 *
 * Initialises the runtime once, runs all Phase 2 tests in order, shuts down
 * the runtime, then prints a summary and exits with 0 on success or 1 if any
 * test failed.
 * ====================================================================== */

int main(void) {
    install_crash_handler();

    printf("libgoc test suite — Phase 2: Channels and fiber launch\n");
    printf("========================================================\n\n");

    goc_init();

    printf("Phase 2 — Channels and fiber launch\n");
    test_p2_1();
    test_p2_2();
    test_p2_3();
    test_p2_4();
    test_p2_5();
    test_p2_6();
    test_p2_7();
    test_p2_8();
    printf("\n");

    goc_shutdown();

    printf("========================================================\n");
    printf("Results: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed > 0) {
        printf(", %d FAILED", g_tests_failed);
    }
    printf("\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
