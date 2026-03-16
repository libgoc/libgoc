/*
 * tests/test_p3_channel_io.c — Phase 3: Channel I/O tests for libgoc
 *
 * Verifies the full set of channel send/receive operations across both OS-thread
 * and fiber contexts, including blocking, non-blocking, and closed-channel
 * edge cases.  These tests build on the foundation and channel lifecycle
 * established in Phases 1–2.
 *
 * Build:  cmake -B build && cmake --build build
 * Run:    ctest --test-dir build --output-on-failure
 *         ./build/test_p3_channel_io
 *
 * Compile requirements: -std=c11 -DGC_THREADS -D_GNU_SOURCE
 *
 * Dependencies:
 *   - libgoc (goc.h)  — runtime under test
 *   - Boehm GC        — must be the threaded variant (bdw-gc-threaded);
 *                        initialised internally by goc_init()
 *   - libuv           — event loop; drives fiber scheduling
 *   - pthreads        — used in P3.7 and P3.9 to park an OS thread
 *   - POSIX semaphores — used by the done_t helper (see below)
 *
 * Synchronisation helper — done_t:
 *   A thin wrapper around a POSIX sem_t that lets the main thread (or a
 *   waiter fiber) block until a fiber signals a step is complete, without
 *   introducing a dependency on the channel machinery under test.
 *
 *     done_init(&d)    — initialise the semaphore to 0
 *     done_signal(&d)  — post (increment) — called by the fiber on a step
 *     done_wait(&d)    — wait (decrement) — called by the test or main thread
 *     done_destroy(&d) — destroy the semaphore
 *
 * Test coverage (Phase 3 — Channel I/O):
 *
 *   P3.1   Rendezvous: fiber puts, main drains via goc_take_sync — value intact
 *   P3.2   Buffered channel: fiber fills, main drains — all values intact, order
 *          preserved
 *   P3.3   goc_take_try on an open empty channel → ok == GOC_EMPTY
 *   P3.4   goc_take_try on a buffered channel with a value → ok == GOC_OK,
 *          correct value
 *   P3.5   goc_take_try on a closed channel → ok == GOC_CLOSED
 *   P3.6   goc_take_sync on a closed empty channel → ok == GOC_CLOSED; no hang
 *   P3.7   goc_close wakes an OS thread parked in goc_take_sync with
 *          ok == GOC_CLOSED; no hang
 *   P3.8   goc_put_sync delivers a value to a waiting fiber; returns GOC_OK
 *   P3.9   goc_close wakes an OS thread parked in goc_put_sync with
 *          ok == GOC_CLOSED; no hang
 *   P3.10  goc_close wakes a fiber parked in goc_take with ok == GOC_CLOSED
 *   P3.11  goc_close wakes a fiber parked in goc_put with ok == GOC_CLOSED
 *   P3.12  Buffered channel closed with values in the ring: goc_take_sync drains
 *          all buffered values (ok == GOC_OK) before returning {NULL, GOC_CLOSED}
 *   P3.13  Buffered channel closed with values in the ring: fiber draining with
 *          goc_take receives all buffered values (ok == GOC_OK) before
 *          {NULL, GOC_CLOSED}
 *   P3.14  Legitimate NULL payload through unbuffered channel: fiber puts NULL,
 *          taker receives ok == GOC_OK and val == NULL
 *
 * Notes:
 *   - goc_init() is called once in main() before any test runs.
 *   - goc_shutdown() is called once in main() after all tests complete.
 *   - The test harness uses the same goto-based cleanup pattern as Phase 1–2;
 *     each test function has a single `done:` label reached by TEST_PASS /
 *     TEST_FAIL / ASSERT to keep cleanup deterministic.
 *   - Tests that involve OS-thread blocking (P3.7, P3.9) spawn a pthread that
 *     parks in the blocking call; the main thread then calls goc_close() and
 *     joins the pthread to confirm it was woken.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <semaphore.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>

#include "test_harness.h"
#include "goc.h"

/* =========================================================================
 * done_t — lightweight fiber-to-main synchronisation via POSIX semaphore
 *
 * Used throughout Phase 3 to let the main thread wait for a fiber to reach
 * a particular point, without depending on the channel operations under test.
 * ====================================================================== */

typedef struct { sem_t sem; } done_t;

static void done_init(done_t* d)    { sem_init(&d->sem, 0, 0); }
static void done_signal(done_t* d)  { sem_post(&d->sem); }
static void done_wait(done_t* d)    { sem_wait(&d->sem); }
static void done_destroy(done_t* d) { sem_destroy(&d->sem); }

/* =========================================================================
 * Phase 3 — Channel I/O
 * ====================================================================== */

/* -------------------------------------------------------------------------
 * P3.1 — Rendezvous: fiber puts, main takes via goc_take_sync — value intact
 *
 * A rendezvous channel (capacity 0) requires sender and receiver to meet.
 * The fiber calls goc_put() which parks it until the main thread calls
 * goc_take_sync(); the value must arrive intact.
 * ---------------------------------------------------------------------- */

typedef struct {
    goc_chan* ch;
    uintptr_t value;
} send_args_t;

static void send_fiber_fn(void* arg) {
    send_args_t* a = (send_args_t*)arg;
    goc_put(a->ch, (void*)a->value);
}

static void test_p3_1(void) {
    TEST_BEGIN("P3.1  rendezvous: fiber puts, main goc_take_sync — value intact");
    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    send_args_t args = { .ch = ch, .value = 0xDEADBEEFUL };
    goc_chan* join = goc_go(send_fiber_fn, &args);
    ASSERT(join != NULL);

    goc_val_t v = goc_take_sync(ch);
    ASSERT(v.ok == GOC_OK);
    ASSERT((uintptr_t)v.val == 0xDEADBEEFUL);

    /* Wait for the fiber to finish. */
    goc_val_t jv = goc_take_sync(join);
    ASSERT(jv.ok == GOC_CLOSED);

    goc_close(ch);
    TEST_PASS();
done:;
}

/* -------------------------------------------------------------------------
 * P3.2 — Buffered channel: fiber fills, main drains — all values intact,
 *         order preserved
 *
 * A fiber puts N values sequentially into a buffered channel.  The main
 * thread then drains the channel with goc_take_sync() and verifies that all
 * values arrive in the order they were sent.
 * ---------------------------------------------------------------------- */

#define P3_2_COUNT 8

typedef struct {
    goc_chan* ch;
    int       count;
} fill_args_t;

static void fill_fiber_fn(void* arg) {
    fill_args_t* a = (fill_args_t*)arg;
    for (int i = 0; i < a->count; i++) {
        goc_put(a->ch, (void*)(uintptr_t)i);
    }
}

static void test_p3_2(void) {
    TEST_BEGIN("P3.2  buffered: fiber fills, main drains — order preserved");
    goc_chan* ch = goc_chan_make(P3_2_COUNT);
    ASSERT(ch != NULL);

    fill_args_t args = { .ch = ch, .count = P3_2_COUNT };
    goc_chan* join = goc_go(fill_fiber_fn, &args);
    ASSERT(join != NULL);

    /* Wait for the fiber to finish filling before draining. */
    goc_val_t jv = goc_take_sync(join);
    ASSERT(jv.ok == GOC_CLOSED);

    for (int i = 0; i < P3_2_COUNT; i++) {
        goc_val_t v = goc_take_sync(ch);
        ASSERT(v.ok == GOC_OK);
        ASSERT((uintptr_t)v.val == (uintptr_t)i);
    }

    goc_close(ch);
    TEST_PASS();
done:;
}

/* -------------------------------------------------------------------------
 * P3.3 — goc_take_try on an open empty channel → ok == GOC_EMPTY
 *
 * A non-blocking take on an open channel that has no value buffered must
 * return GOC_EMPTY immediately without parking.
 * ---------------------------------------------------------------------- */

static void test_p3_3(void) {
    TEST_BEGIN("P3.3  goc_take_try on open empty channel → GOC_EMPTY");
    goc_chan* ch = goc_chan_make(4);
    ASSERT(ch != NULL);

    goc_val_t v = goc_take_try(ch);
    ASSERT(v.ok == GOC_EMPTY);
    ASSERT(v.val == NULL);

    goc_close(ch);
    TEST_PASS();
done:;
}

/* -------------------------------------------------------------------------
 * P3.4 — goc_take_try on a buffered channel with a value → ok == GOC_OK
 *
 * After a synchronous put fills one slot of a buffered channel, a
 * non-blocking take must succeed and return the correct value.
 * ---------------------------------------------------------------------- */

static void test_p3_4(void) {
    TEST_BEGIN("P3.4  goc_take_try on buffered channel with value → GOC_OK");
    goc_chan* ch = goc_chan_make(4);
    ASSERT(ch != NULL);

    goc_status_t st = goc_put_sync(ch, (void*)(uintptr_t)99);
    ASSERT(st == GOC_OK);

    goc_val_t v = goc_take_try(ch);
    ASSERT(v.ok == GOC_OK);
    ASSERT((uintptr_t)v.val == 99);

    goc_close(ch);
    TEST_PASS();
done:;
}

/* -------------------------------------------------------------------------
 * P3.5 — goc_take_try on a closed channel → ok == GOC_CLOSED
 *
 * After goc_close() the non-blocking take must return GOC_CLOSED (not
 * GOC_EMPTY) regardless of whether the channel had any buffered values.
 * ---------------------------------------------------------------------- */

static void test_p3_5(void) {
    TEST_BEGIN("P3.5  goc_take_try on closed empty channel → GOC_CLOSED");
    goc_chan* ch = goc_chan_make(4);
    ASSERT(ch != NULL);

    goc_close(ch);

    goc_val_t v = goc_take_try(ch);
    ASSERT(v.ok == GOC_CLOSED);
    ASSERT(v.val == NULL);

    TEST_PASS();
done:;
}

/* -------------------------------------------------------------------------
 * P3.6 — goc_take_sync on a closed empty channel → ok == GOC_CLOSED; no hang
 *
 * A blocking take on an already-closed empty channel must return immediately
 * with GOC_CLOSED rather than blocking forever.
 * ---------------------------------------------------------------------- */

static void test_p3_6(void) {
    TEST_BEGIN("P3.6  goc_take_sync on closed empty channel → GOC_CLOSED");
    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    goc_close(ch);

    goc_val_t v = goc_take_sync(ch);
    ASSERT(v.ok == GOC_CLOSED);
    ASSERT(v.val == NULL);

    TEST_PASS();
done:;
}

/* -------------------------------------------------------------------------
 * P3.7 — goc_close wakes an OS thread parked in goc_take_sync → GOC_CLOSED
 *
 * Strategy: spawn a pthread that calls goc_take_sync() on an empty rendezvous
 * channel, parking it.  The main thread then calls goc_close() and joins the
 * thread to confirm it was woken with ok == GOC_CLOSED.
 * ---------------------------------------------------------------------- */

typedef struct {
    goc_chan*    ch;
    goc_val_t    result;
    done_t*      parked; /* signalled just before the blocking call */
} sync_taker_args_t;

static void* sync_taker_thread(void* arg) {
    sync_taker_args_t* a = (sync_taker_args_t*)arg;
    done_signal(a->parked);
    a->result = goc_take_sync(a->ch);
    return NULL;
}

static void test_p3_7(void) {
    TEST_BEGIN("P3.7  goc_close wakes OS thread in goc_take_sync → GOC_CLOSED");
    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    done_t parked;
    done_init(&parked);

    sync_taker_args_t args = { .ch = ch, .parked = &parked };

    pthread_t tid;
    pthread_create(&tid, NULL, sync_taker_thread, &args);

    /* Wait until the thread is about to block (or has already blocked). */
    done_wait(&parked);

    /* Give the thread a moment to reach goc_take_sync before closing. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 5000000L /* 5 ms */ };
    nanosleep(&ts, NULL);

    goc_close(ch);
    pthread_join(tid, NULL);

    ASSERT(args.result.ok == GOC_CLOSED);
    ASSERT(args.result.val == NULL);

    done_destroy(&parked);
    TEST_PASS();
done:;
}

/* -------------------------------------------------------------------------
 * P3.8 — goc_put_sync delivers a value to a waiting fiber; returns GOC_OK
 *
 * A fiber parks in goc_take() on a rendezvous channel.  The main thread then
 * calls goc_put_sync(), which must deliver the value and return GOC_OK.  The
 * fiber records the received value so the test can verify it.
 * ---------------------------------------------------------------------- */

typedef struct {
    goc_chan*    ch;
    done_t*      ready;   /* signalled when the fiber is about to take */
    goc_val_t    result;  /* filled in by the fiber after goc_take returns */
    done_t*      done;    /* signalled after the fiber records the result */
} take_fiber_args_t;

static void take_fiber_fn(void* arg) {
    take_fiber_args_t* a = (take_fiber_args_t*)arg;
    done_signal(a->ready);
    a->result = goc_take(a->ch);
    done_signal(a->done);
}

static void test_p3_8(void) {
    TEST_BEGIN("P3.8  goc_put_sync delivers value to waiting fiber → GOC_OK");
    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    done_t ready, done_sem;
    done_init(&ready);
    done_init(&done_sem);

    take_fiber_args_t args = {
        .ch    = ch,
        .ready = &ready,
        .done  = &done_sem,
    };

    goc_chan* join = goc_go(take_fiber_fn, &args);
    ASSERT(join != NULL);

    /* Wait until the fiber is about to call goc_take. */
    done_wait(&ready);

    /* Small delay so the fiber has time to park. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 5000000L /* 5 ms */ };
    nanosleep(&ts, NULL);

    goc_status_t st = goc_put_sync(ch, (void*)(uintptr_t)0xCAFEUL);
    ASSERT(st == GOC_OK);

    /* Wait for the fiber to record its result. */
    done_wait(&done_sem);

    ASSERT(args.result.ok == GOC_OK);
    ASSERT((uintptr_t)args.result.val == 0xCAFEUL);

    goc_val_t jv = goc_take_sync(join);
    ASSERT(jv.ok == GOC_CLOSED);

    goc_close(ch);
    done_destroy(&ready);
    done_destroy(&done_sem);
    TEST_PASS();
done:;
}

/* -------------------------------------------------------------------------
 * P3.9 — goc_close wakes an OS thread parked in goc_put_sync → GOC_CLOSED
 *
 * Strategy: spawn a pthread that calls goc_put_sync() on a rendezvous channel
 * with no taker, parking it.  The main thread then calls goc_close() and
 * joins the thread to confirm it was woken with status == GOC_CLOSED.
 * ---------------------------------------------------------------------- */

typedef struct {
    goc_chan*    ch;
    goc_status_t result;
    done_t*      parked; /* signalled just before the blocking call */
} sync_putter_args_t;

static void* sync_putter_thread(void* arg) {
    sync_putter_args_t* a = (sync_putter_args_t*)arg;
    done_signal(a->parked);
    a->result = goc_put_sync(a->ch, (void*)(uintptr_t)1UL);
    return NULL;
}

static void test_p3_9(void) {
    TEST_BEGIN("P3.9  goc_close wakes OS thread in goc_put_sync → GOC_CLOSED");
    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    done_t parked;
    done_init(&parked);

    sync_putter_args_t args = { .ch = ch, .parked = &parked };

    pthread_t tid;
    pthread_create(&tid, NULL, sync_putter_thread, &args);

    /* Wait until the thread is about to block. */
    done_wait(&parked);

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 5000000L /* 5 ms */ };
    nanosleep(&ts, NULL);

    goc_close(ch);
    pthread_join(tid, NULL);

    ASSERT(args.result == GOC_CLOSED);

    done_destroy(&parked);
    TEST_PASS();
done:;
}

/* -------------------------------------------------------------------------
 * P3.10 — goc_close wakes a fiber parked in goc_take → ok == GOC_CLOSED
 *
 * A fiber parks in goc_take() on an empty rendezvous channel.  The main
 * thread then calls goc_close(); the fiber must wake with GOC_CLOSED.
 * ---------------------------------------------------------------------- */

typedef struct {
    goc_chan*  ch;
    done_t*    parked;   /* signalled before goc_take */
    goc_val_t  result;
    done_t*    done;     /* signalled after goc_take returns */
} parked_taker_args_t;

static void parked_taker_fn(void* arg) {
    parked_taker_args_t* a = (parked_taker_args_t*)arg;
    done_signal(a->parked);
    a->result = goc_take(a->ch);
    done_signal(a->done);
}

static void test_p3_10(void) {
    TEST_BEGIN("P3.10 goc_close wakes fiber in goc_take → GOC_CLOSED");
    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    done_t parked, done_sem;
    done_init(&parked);
    done_init(&done_sem);

    parked_taker_args_t args = {
        .ch     = ch,
        .parked = &parked,
        .done   = &done_sem,
    };

    goc_chan* join = goc_go(parked_taker_fn, &args);
    ASSERT(join != NULL);

    done_wait(&parked);

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 5000000L /* 5 ms */ };
    nanosleep(&ts, NULL);

    goc_close(ch);

    /* Wait for the fiber to record its result. */
    done_wait(&done_sem);

    ASSERT(args.result.ok == GOC_CLOSED);
    ASSERT(args.result.val == NULL);

    goc_val_t jv = goc_take_sync(join);
    ASSERT(jv.ok == GOC_CLOSED);

    done_destroy(&parked);
    done_destroy(&done_sem);
    TEST_PASS();
done:;
}

/* -------------------------------------------------------------------------
 * P3.11 — goc_close wakes a fiber parked in goc_put → ok == GOC_CLOSED
 *
 * A fiber parks in goc_put() on a rendezvous channel that has no taker.
 * The main thread calls goc_close(); the fiber must wake with GOC_CLOSED.
 * ---------------------------------------------------------------------- */

typedef struct {
    goc_chan*     ch;
    done_t*       parked;  /* signalled before goc_put */
    goc_status_t  result;
    done_t*       done;    /* signalled after goc_put returns */
} parked_putter_args_t;

static void parked_putter_fn(void* arg) {
    parked_putter_args_t* a = (parked_putter_args_t*)arg;
    done_signal(a->parked);
    a->result = goc_put(a->ch, (void*)(uintptr_t)1UL);
    done_signal(a->done);
}

static void test_p3_11(void) {
    TEST_BEGIN("P3.11 goc_close wakes fiber in goc_put → GOC_CLOSED");
    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    done_t parked, done_sem;
    done_init(&parked);
    done_init(&done_sem);

    parked_putter_args_t args = {
        .ch     = ch,
        .parked = &parked,
        .done   = &done_sem,
    };

    goc_chan* join = goc_go(parked_putter_fn, &args);
    ASSERT(join != NULL);

    done_wait(&parked);

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 5000000L /* 5 ms */ };
    nanosleep(&ts, NULL);

    goc_close(ch);

    done_wait(&done_sem);

    ASSERT(args.result == GOC_CLOSED);

    goc_val_t jv = goc_take_sync(join);
    ASSERT(jv.ok == GOC_CLOSED);

    done_destroy(&parked);
    done_destroy(&done_sem);
    TEST_PASS();
done:;
}

/* -------------------------------------------------------------------------
 * P3.12 — Buffered channel closed with values in the ring: goc_take_sync
 *          drains all buffered values (ok == GOC_OK) before GOC_CLOSED
 *
 * A fiber puts N values into a buffered channel and signals done.  The main
 * thread then calls goc_close() before draining and verifies that all N
 * values arrive with ok == GOC_OK followed by a single {NULL, GOC_CLOSED}.
 * ---------------------------------------------------------------------- */

#define P3_12_COUNT 4

static void test_p3_12(void) {
    TEST_BEGIN("P3.12 closed buffered channel: goc_take_sync drains before CLOSED");
    goc_chan* ch = goc_chan_make(P3_12_COUNT);
    ASSERT(ch != NULL);

    fill_args_t args = { .ch = ch, .count = P3_12_COUNT };
    goc_chan* join = goc_go(fill_fiber_fn, &args);
    ASSERT(join != NULL);

    /* Wait for the fiber to finish filling. */
    goc_val_t jv = goc_take_sync(join);
    ASSERT(jv.ok == GOC_CLOSED);

    /* Close with values still in the ring. */
    goc_close(ch);

    /* Drain: expect all N values with GOC_OK. */
    for (int i = 0; i < P3_12_COUNT; i++) {
        goc_val_t v = goc_take_sync(ch);
        ASSERT(v.ok == GOC_OK);
        ASSERT((uintptr_t)v.val == (uintptr_t)i);
    }

    /* The next take must return GOC_CLOSED. */
    goc_val_t last = goc_take_sync(ch);
    ASSERT(last.ok == GOC_CLOSED);
    ASSERT(last.val == NULL);

    TEST_PASS();
done:;
}

/* -------------------------------------------------------------------------
 * P3.13 — Buffered channel closed with values in the ring: fiber draining
 *          with goc_take receives all buffered values before GOC_CLOSED
 *
 * Same as P3.12 but the draining is done from fiber context via goc_take().
 * ---------------------------------------------------------------------- */

#define P3_13_COUNT 4

typedef struct {
    goc_chan* ch;
    int       expected_count;
    int       ok_count;       /* number of GOC_OK takes observed */
    bool      got_closed;     /* true if final take was GOC_CLOSED */
    bool      order_ok;       /* true if values arrived in order */
    done_t*   done;
} drain_fiber_args_t;

static void drain_fiber_fn(void* arg) {
    drain_fiber_args_t* a = (drain_fiber_args_t*)arg;
    a->ok_count   = 0;
    a->got_closed = false;
    a->order_ok   = true;

    for (;;) {
        goc_val_t v = goc_take(a->ch);
        if (v.ok == GOC_CLOSED) {
            a->got_closed = true;
            break;
        }
        if (v.ok == GOC_OK) {
            if ((uintptr_t)v.val != (uintptr_t)a->ok_count) {
                a->order_ok = false;
            }
            a->ok_count++;
        }
    }
    done_signal(a->done);
}

static void test_p3_13(void) {
    TEST_BEGIN("P3.13 closed buffered channel: fiber goc_take drains before CLOSED");
    goc_chan* ch = goc_chan_make(P3_13_COUNT);
    ASSERT(ch != NULL);

    /* Fill the channel from a fiber. */
    fill_args_t fill = { .ch = ch, .count = P3_13_COUNT };
    goc_chan* fill_join = goc_go(fill_fiber_fn, &fill);
    ASSERT(fill_join != NULL);

    goc_val_t fjv = goc_take_sync(fill_join);
    ASSERT(fjv.ok == GOC_CLOSED);

    /* Close with values in the ring. */
    goc_close(ch);

    /* Drain from a fiber. */
    done_t done_sem;
    done_init(&done_sem);

    drain_fiber_args_t drain = {
        .ch             = ch,
        .expected_count = P3_13_COUNT,
        .done           = &done_sem,
    };
    goc_chan* drain_join = goc_go(drain_fiber_fn, &drain);
    ASSERT(drain_join != NULL);

    done_wait(&done_sem);

    ASSERT(drain.ok_count == P3_13_COUNT);
    ASSERT(drain.got_closed == true);
    ASSERT(drain.order_ok == true);

    goc_val_t djv = goc_take_sync(drain_join);
    ASSERT(djv.ok == GOC_CLOSED);

    done_destroy(&done_sem);
    TEST_PASS();
done:;
}

/* -------------------------------------------------------------------------
 * P3.14 — Legitimate NULL payload through unbuffered channel
 *
 * NULL is a valid payload in libgoc.  The status field distinguishes a
 * successful NULL delivery (GOC_OK) from a close signal (GOC_CLOSED).
 * A fiber puts NULL; the main thread verifies ok == GOC_OK and val == NULL.
 * ---------------------------------------------------------------------- */

static void null_sender_fn(void* arg) {
    goc_chan* ch = (goc_chan*)arg;
    goc_put(ch, NULL);
}

static void test_p3_14(void) {
    TEST_BEGIN("P3.14 NULL payload through rendezvous channel → GOC_OK, val==NULL");
    goc_chan* ch = goc_chan_make(0);
    ASSERT(ch != NULL);

    goc_chan* join = goc_go(null_sender_fn, ch);
    ASSERT(join != NULL);

    goc_val_t v = goc_take_sync(ch);
    ASSERT(v.ok == GOC_OK);
    ASSERT(v.val == NULL);

    goc_val_t jv = goc_take_sync(join);
    ASSERT(jv.ok == GOC_CLOSED);

    goc_close(ch);
    TEST_PASS();
done:;
}

/* =========================================================================
 * main
 *
 * Initialises the runtime once, runs all Phase 3 tests in order, shuts down
 * the runtime, then prints a summary and exits with 0 on success or 1 if any
 * test failed.
 * ====================================================================== */

int main(void) {
    install_crash_handler();

    printf("libgoc test suite — Phase 3: Channel I/O\n");
    printf("==========================================\n\n");

    goc_init();

    printf("Phase 3 — Channel I/O\n");
    test_p3_1();
    test_p3_2();
    test_p3_3();
    test_p3_4();
    test_p3_5();
    test_p3_6();
    test_p3_7();
    test_p3_8();
    test_p3_9();
    test_p3_10();
    test_p3_11();
    test_p3_12();
    test_p3_13();
    test_p3_14();
    printf("\n");

    goc_shutdown();

    printf("==========================================\n");
    printf("Results: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed > 0) {
        printf(", %d FAILED", g_tests_failed);
    }
    printf("\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
