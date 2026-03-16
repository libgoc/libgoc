/*
 * tests/test_p4_callbacks.c — Phase 4: Callback tests for libgoc
 *
 * Verifies the non-blocking callback API: goc_take_cb() and goc_put_cb().
 * Callbacks are delivered on the libuv event loop thread and must never block.
 * These tests build on the channel I/O primitives verified in Phase 3 and must
 * pass before the select and timeout tests (Phase 5+) are attempted.
 *
 * Build:  cmake -B build && cmake --build build
 * Run:    ctest --test-dir build --output-on-failure
 *         ./build/test_p4_callbacks
 *
 * Compile requirements: -std=c11 -DGC_THREADS -D_GNU_SOURCE
 *
 * Dependencies:
 *   - libgoc (goc.h)  — runtime under test
 *   - Boehm GC        — must be the threaded variant (bdw-gc-threaded);
 *                        initialised internally by goc_init()
 *   - libuv           — event loop; drives callback delivery
 *   - POSIX semaphores — used by done_t to synchronise callbacks back to the
 *                        main test thread
 *
 * Synchronisation helper — done_t:
 *   A thin wrapper around a POSIX sem_t.  Callbacks run on the libuv loop
 *   thread; done_t lets the main thread block until a callback has fired.
 *   Using a semaphore avoids depending on the channel machinery under test.
 *
 *     done_init(&d)    — initialise the semaphore to 0
 *     done_signal(&d)  — post (increment) — called inside the callback
 *     done_wait(&d)    — wait (decrement) — called by the test thread
 *     done_destroy(&d) — destroy the semaphore
 *
 * Test coverage (Phase 4 — Callbacks):
 *
 *   P4.1  goc_take_cb fires with ok==GOC_OK and the correct value when a fiber
 *         sends a value to the channel after the callback is registered
 *   P4.2  goc_take_cb fires with ok==GOC_CLOSED when the channel is closed
 *         before a value is ever delivered
 *   P4.3  goc_put_cb delivers a value consumed by a fiber; the completion
 *         callback fires with ok==GOC_OK
 *   P4.4  goc_put_cb with cb=NULL (fire-and-forget) delivers the value without
 *         crashing; a subsequent goc_take_sync retrieves the correct value
 *   P4.5  goc_put_cb on a closed channel: the completion callback fires with
 *         ok==GOC_CLOSED; no hang
 *   P4.6  goc_put_cb with buffer space and no parked taker: value is enqueued
 *         in the ring buffer and the completion callback fires with ok==GOC_OK;
 *         a subsequent goc_take_sync retrieves the correct value (exercises the
 *         buffer path, distinct from the parked-taker path in P4.3)
 *
 * Notes:
 *   - goc_init() is called once in main() before any test runs.
 *   - goc_shutdown() is called once in main() after all tests complete.
 *   - The test harness uses the same goto-based cleanup pattern as earlier
 *     phases; see the harness section comments for details.
 *   - Callbacks must be short and non-blocking (they run on the loop thread).
 *     Each callback sets a result field and posts a semaphore; the main thread
 *     performs all ASSERTs after done_wait() returns.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <semaphore.h>

#include "test_harness.h"
#include "goc.h"

/* =========================================================================
 * done_t — lightweight callback-to-main synchronisation via POSIX semaphore
 *
 * Callbacks are delivered on the libuv loop thread, which is separate from
 * both the pool threads and the main test thread.  A raw semaphore lets the
 * main thread block until the callback has fired, without depending on any
 * goc_chan, goc_put, or goc_take behaviour that is itself under test.
 * ====================================================================== */

typedef struct { sem_t sem; } done_t;

static void done_init(done_t* d)    { sem_init(&d->sem, 0, 0); }
static void done_signal(done_t* d)  { sem_post(&d->sem); }
static void done_wait(done_t* d)    { sem_wait(&d->sem); }
static void done_destroy(done_t* d) { sem_destroy(&d->sem); }

/* =========================================================================
 * Phase 4 — Callbacks
 * ====================================================================== */

/* --- P4.1: goc_take_cb delivers a value from a fiber ------------------- */

/*
 * Result bundle written by the take callback in P4.1 and P4.2.
 *
 * val    — value received (or NULL on closed)
 * ok     — status reported by the callback
 * done   — signalled when the callback has written its results
 */
typedef struct {
    void*        val;
    goc_status_t ok;
    done_t*      done;
} take_cb_result_t;

/*
 * goc_take_cb callback used in P4.1 and P4.2.
 * Records the received value and status, then signals the main thread.
 * Must not block — runs on the libuv loop thread.
 */
static void take_cb(void* val, goc_status_t ok, void* ud) {
    take_cb_result_t* r = (take_cb_result_t*)ud;
    r->val = val;
    r->ok  = ok;
    done_signal(r->done);
}

/*
 * Fiber entry point for P4.1.
 * Sends a fixed payload to the channel, then returns.
 * The send must succeed; no synchronisation with the callback is needed
 * here because the pool scheduler will park the fiber until a taker (the
 * callback entry) is available.
 */
typedef struct {
    goc_chan*    ch;
    uintptr_t    payload;
} sender_args_t;

static void sender_fiber_fn(void* arg) {
    sender_args_t* a = (sender_args_t*)arg;
    goc_put(a->ch, (void*)a->payload);
}

/*
 * P4.1 — goc_take_cb delivers a value sent by a fiber; ok==GOC_OK, value intact
 *
 * Register a take callback, then launch a fiber that puts a known value.
 * The callback must fire with ok==GOC_OK and val==42.
 *
 * This exercises the parked-callback taker path: the callback entry is placed
 * on the channel's taker list, and the fiber's goc_put wakes it.
 */
static void test_p4_1(void) {
    TEST_BEGIN("P4.1  goc_take_cb delivers fiber value; ok==GOC_OK");

    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    done_t done;
    done_init(&done);

    take_cb_result_t result = { .val = NULL, .ok = GOC_EMPTY, .done = &done };

    /* Register the callback before the fiber is launched so the callback entry
     * is already parked when the fiber's goc_put runs. */
    goc_take_cb(ch, take_cb, &result);

    sender_args_t sargs = { .ch = ch, .payload = 42 };
    goc_chan* join = goc_go(sender_fiber_fn, &sargs);
    ASSERT(join != NULL);

    done_wait(&done);

    ASSERT(result.ok == GOC_OK);
    ASSERT((uintptr_t)result.val == 42);

    /* Wait for the sender fiber to finish before cleaning up. */
    goc_take_sync(join);

    goc_close(ch);
    done_destroy(&done);
    TEST_PASS();
done:;
}

/* --- P4.2: goc_take_cb fires GOC_CLOSED when channel closes before delivery */

/*
 * P4.2 — goc_take_cb fires with ok==GOC_CLOSED when the channel is closed
 *
 * Register a take callback on a rendezvous channel, then close the channel
 * before any fiber sends a value.  The callback must be woken with
 * ok==GOC_CLOSED and val==NULL.
 *
 * This exercises the "close wakes all parked entries" path in goc_close,
 * specifically for GOC_CALLBACK entries on the taker list.
 */
static void test_p4_2(void) {
    TEST_BEGIN("P4.2  goc_take_cb fires GOC_CLOSED when channel closed first");

    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    done_t done;
    done_init(&done);

    take_cb_result_t result = { .val = (void*)(uintptr_t)0xFF, .ok = GOC_OK, .done = &done };

    goc_take_cb(ch, take_cb, &result);

    /* Close the channel without sending anything. The callback must fire. */
    goc_close(ch);

    done_wait(&done);

    ASSERT(result.ok == GOC_CLOSED);
    ASSERT(result.val == NULL);

    done_destroy(&done);
    TEST_PASS();
done:;
}

/* --- P4.3: goc_put_cb delivers a value consumed by a fiber ------------- */

/*
 * Result bundle written by the put completion callback in P4.3 and P4.5.
 *
 * ok   — status reported by the completion callback
 * done — signalled when the callback has written its result
 */
typedef struct {
    goc_status_t ok;
    done_t*      done;
} put_cb_result_t;

/*
 * goc_put_cb completion callback used in P4.3, P4.5, and P4.6.
 * Records the delivery status, then signals the main thread.
 * Must not block — runs on the libuv loop thread.
 */
static void put_cb(goc_status_t ok, void* ud) {
    put_cb_result_t* r = (put_cb_result_t*)ud;
    r->ok = ok;
    done_signal(r->done);
}

/*
 * Fiber entry point for P4.3.
 * Takes one value from the channel, records it in *out, and signals done.
 *
 * out  — pointer to a uintptr_t that receives the taken value
 * done — signalled after the take completes
 */
typedef struct {
    goc_chan*  ch;
    uintptr_t  out;
    done_t*    done;
} taker_args_t;

static void taker_fiber_fn(void* arg) {
    taker_args_t* a = (taker_args_t*)arg;
    goc_val_t v = goc_take(a->ch);
    if (v.ok == GOC_OK) {
        a->out = (uintptr_t)v.val;
    }
    done_signal(a->done);
}

/*
 * P4.3 — goc_put_cb delivers a value consumed by a fiber; ok==GOC_OK
 *
 * Launch a fiber that parks in goc_take(), then call goc_put_cb().
 * The runtime must match the callback putter to the parked fiber taker.
 * The put completion callback must fire with ok==GOC_OK.
 *
 * This exercises the parked-taker wakeup path from a GOC_CALLBACK putter.
 */
static void test_p4_3(void) {
    TEST_BEGIN("P4.3  goc_put_cb delivers to parked fiber; ok==GOC_OK");

    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    done_t taker_done, put_done;
    done_init(&taker_done);
    done_init(&put_done);

    taker_args_t targs = { .ch = ch, .out = 0, .done = &taker_done };
    goc_chan* join = goc_go(taker_fiber_fn, &targs);
    ASSERT(join != NULL);

    put_cb_result_t presult = { .ok = GOC_EMPTY, .done = &put_done };

    /* Give the fiber a moment to park in goc_take; then issue the callback put. */
    goc_put_cb(ch, (void*)(uintptr_t)99, put_cb, &presult);

    /* Wait for both the fiber take and the put completion callback. */
    done_wait(&taker_done);
    done_wait(&put_done);

    ASSERT(presult.ok == GOC_OK);
    ASSERT(targs.out == 99);

    goc_take_sync(join);

    goc_close(ch);
    done_destroy(&taker_done);
    done_destroy(&put_done);
    TEST_PASS();
done:;
}

/* --- P4.4: goc_put_cb with cb==NULL (fire-and-forget) ------------------ */

/*
 * Fiber entry point for P4.4.
 * Puts a single value and signals the main thread.
 *
 * Unlike P4.3, this fiber runs *after* the goc_put_cb has enqueued the value,
 * so it is used here only to consume the value from a buffered channel.
 * The channel for P4.4 is buffered, so the fiber is not strictly needed for
 * delivery — it is used to verify that the correct value arrived.
 */

/*
 * P4.4 — goc_put_cb with cb==NULL does not crash; value is delivered
 *
 * Call goc_put_cb with a NULL completion callback (fire-and-forget mode) on a
 * buffered channel.  The runtime must enqueue the value without crashing.
 * A subsequent goc_take_sync retrieves the value and confirms integrity.
 *
 * This exercises the no-callback branch in the put_cb dispatch path and the
 * buffer-enqueue fast path when no taker is parked.
 */
static void test_p4_4(void) {
    TEST_BEGIN("P4.4  goc_put_cb cb==NULL fire-and-forget; value intact");

    goc_chan* ch = goc_chan_make(4);
    ASSERT(ch != NULL);

    /* Fire-and-forget: no completion callback, no user data. */
    goc_put_cb(ch, (void*)(uintptr_t)77, NULL, NULL);

    /* The value should now be in the ring buffer. Take it synchronously. */
    goc_val_t v = goc_take_sync(ch);
    ASSERT(v.ok == GOC_OK);
    ASSERT((uintptr_t)v.val == 77);

    goc_close(ch);
    TEST_PASS();
done:;
}

/* --- P4.5: goc_put_cb on a closed channel fires GOC_CLOSED ------------- */

/*
 * P4.5 — goc_put_cb on a closed channel: completion callback fires GOC_CLOSED
 *
 * Close the channel first, then call goc_put_cb.  The runtime must detect
 * that the channel is already closed and deliver the completion callback with
 * ok==GOC_CLOSED immediately (no hang, no crash).
 *
 * This exercises the closed-channel fast path in goc_put_cb.
 */
static void test_p4_5(void) {
    TEST_BEGIN("P4.5  goc_put_cb on closed channel fires GOC_CLOSED; no hang");

    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    goc_close(ch);

    done_t done;
    done_init(&done);

    put_cb_result_t result = { .ok = GOC_OK, .done = &done };

    goc_put_cb(ch, (void*)(uintptr_t)55, put_cb, &result);

    done_wait(&done);

    ASSERT(result.ok == GOC_CLOSED);

    done_destroy(&done);
    TEST_PASS();
done:;
}

/* --- P4.6: goc_put_cb buffers value when no parked taker --------------- */

/*
 * P4.6 — goc_put_cb enqueues into ring buffer when no taker is parked;
 *         completion callback fires GOC_OK; goc_take_sync retrieves value
 *
 * Call goc_put_cb on a buffered channel before any fiber or OS thread has
 * issued a take.  The value must be placed in the ring buffer and the
 * completion callback must fire immediately with ok==GOC_OK (no need to wait
 * for a taker).  A subsequent goc_take_sync must return the correct value.
 *
 * This is the buffer fast-path (step 4 in the goc_put_cb algorithm), distinct
 * from the parked-taker wakeup path verified in P4.3.
 */
static void test_p4_6(void) {
    TEST_BEGIN("P4.6  goc_put_cb buffers value; ok==GOC_OK; take retrieves it");

    goc_chan* ch = goc_chan_make(8);
    ASSERT(ch != NULL);

    done_t done;
    done_init(&done);

    put_cb_result_t result = { .ok = GOC_EMPTY, .done = &done };

    /* No taker registered — value should go straight into the ring buffer. */
    goc_put_cb(ch, (void*)(uintptr_t)123, put_cb, &result);

    /* Wait for the completion callback to confirm the buffer enqueue. */
    done_wait(&done);

    ASSERT(result.ok == GOC_OK);

    /* Now drain the buffered value with a synchronous take. */
    goc_val_t v = goc_take_sync(ch);
    ASSERT(v.ok == GOC_OK);
    ASSERT((uintptr_t)v.val == 123);

    goc_close(ch);
    done_destroy(&done);
    TEST_PASS();
done:;
}

/* =========================================================================
 * main
 *
 * Initialises the runtime once, runs all Phase 4 tests in order, shuts down
 * the runtime, then prints a summary and exits with 0 on success or 1 if any
 * test failed.
 * ====================================================================== */

int main(void) {
    install_crash_handler();

    printf("libgoc test suite — Phase 4: Callbacks\n");
    printf("=========================================\n\n");

    goc_init();

    printf("Phase 4 — Callbacks\n");
    test_p4_1();
    test_p4_2();
    test_p4_3();
    test_p4_4();
    test_p4_5();
    test_p4_6();
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
