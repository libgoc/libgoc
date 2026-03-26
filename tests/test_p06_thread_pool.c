/*
 * tests/test_p6_thread_pool.c — Phase 6: Thread pool tests for libgoc
 *
 * Verifies goc_pool_make / goc_pool_destroy lifecycle, goc_go_on dispatch,
 * goc_pool_destroy_timeout (both GOC_DRAIN_OK and GOC_DRAIN_TIMEOUT paths),
 * the goc_malloc GC-heap allocator end-to-end, and the malloc path in
 * alts_dedup_sort_channels for n > GOC_ALTS_STACK_THRESHOLD arms.
 *
 * Build:  cmake -B build && cmake --build build
 * Run:    ctest --test-dir build --output-on-failure
 *         ./build/test_p6_thread_pool
 *
 * Compile requirements: -std=c11 -DGC_THREADS -D_GNU_SOURCE
 *
 * Dependencies:
 *   - libgoc (goc.h)  — runtime under test
 *   - Boehm GC        — must be the threaded variant (bdw-gc-threaded);
 *                        initialised internally by goc_init()
 *   - libuv           — event loop; drives fiber scheduling and timers
 *   - libuv mutex+condvar sync helpers for done_t — see below
 *
 * Synchronisation helper — done_t:
 *   A portable mutex+condvar semaphore that lets the main thread (or a waiter
 *   fiber) block until a fiber signals completion.  Using mutex+condvar avoids
 *   the need for a goc_chan in tests that are themselves verifying pool or
 *   channel behaviour, keeping each test self-contained.
 *
 *     done_init(&d)    — initialise the mutex and condvar
 *     done_signal(&d)  — set flag and signal — called by the fiber on exit
 *     done_wait(&d)    — wait until flag is set — called by the test thread
 *     done_destroy(&d) — destroy the mutex and condvar
 *
 * Test coverage (Phase 6 — Thread pool):
 *
 *   P6.1   goc_pool_make / goc_pool_destroy lifecycle; goc_go_on dispatches
 *          fiber to the correct pool
 *   P6.2   goc_pool_destroy_timeout returns GOC_DRAIN_OK when all fibers
 *          finish before the deadline
 *   P6.3   goc_pool_destroy_timeout returns GOC_DRAIN_TIMEOUT when fibers are
 *          still running at deadline; pool remains valid — verified by
 *          dispatching a new short-lived fiber via goc_go_on and confirming it
 *          runs to completion before goc_pool_destroy is called
 *   P6.4   goc_malloc end-to-end: fiber builds GC-heap linked list, main
 *          traverses after join
 *   P6.5   goc_alts with n > GOC_ALTS_STACK_THRESHOLD (8) arms exercises the
 *          malloc path in alts_dedup_sort_channels; correct arm fires, no
 *          memory error (run under ASAN to catch heap misuse)
 *
 * Test coverage (Phase 6 continued — goc_wsdq and goc_injector):
 *
 *   P6.6   wsdq push_bottom / pop_bottom round-trip (LIFO, single-threaded)
 *   P6.7   wsdq pop_bottom on empty deque returns NULL
 *   P6.8   wsdq steal_top on empty deque returns NULL
 *   P6.9   wsdq steal_top ordering is FIFO
 *   P6.10  wsdq grows when full (single-threaded)
 *   P6.11  wsdq concurrent owner-push / thief-steal (multi-threaded)
 *   P6.12  wsdq concurrent push + pop + steal under contention
 *   P6.13  wsdq_destroy on non-empty deque is safe
 *   P6.14  injector push/pop round-trip (FIFO, single-threaded)
 *   P6.15  injector_pop on empty returns NULL
 *   P6.16  injector concurrent push from multiple threads
 *   P6.17  wsdq pop_bottom / steal_top race on the last element (no
 *          double-delivery, no loss, no hang)
 *   P6.18  pool internal post: burst of child fibers (spawned inside a fiber
 *          via goc_go_on) all complete — verifies that idle workers beyond
 *          the fixed (w->index+1)%N neighbor are eventually woken/utilized;
 *          also catches deferred-materialization GC race under burst spawning
 *          (SIGABRT from BDW-GC if concurrent goc_fiber_materialize calls
 *          corrupt or exhaust the GC root registration table)
 *   P6.19  pool external post sleep-miss: serial external posts with forced
 *          idle windows — verifies that a worker sleeping in uv_sem_wait is
 *          reliably woken when an external caller pushes to its injector
 *   P6.20  pool external post targets busy worker: injected tasks still
 *          complete even when the round-robin target worker is inside
 *          mco_resume and its idle_sem post is consumed after the fact;
 *          also catches deferred-materialization GC race (same as P6.18)
 *          when the burst of deferred tasks are materialized concurrently
 *   P6.21  pool idle steal gap: work that becomes stealable after a worker's
 *          double-check (but before uv_sem_wait) is still processed within
 *          deadline, not silently dropped; also catches deferred-
 *          materialization GC race under a larger burst (1000 children)
 *   P6.22  double-resume via stack-address reuse: many ping-pong pairs on
 *          unbuffered channels with a high worker count; verifies that
 *          heap-allocated parking entries prevent a waker spinning on the
 *          old entry from claiming the new entry at the same stack address
 *
 *   P6.23  GC bitmap write-ordering: rapid slot reuse with concurrent GC
 *          cycles — verifies that goc_fiber_root_register writes slot data
 *          before publishing the bitmap bit, preventing push_fiber_roots from
 *          reading stale stack pointers from a freed previous occupant
 *
 *   P6.24   (GOC_ENABLE_STATS only) steal fields on STOPPED events are valid:
 *          after goc_pool_destroy each STOPPED event must satisfy
 *          steal_successes <= steal_attempts
 *
 *   P6.25   (GOC_ENABLE_STATS only) goc_pool_get_steal_stats is consistent with
 *          per-worker STOPPED event totals: sum of steal_attempts across all
 *          STOPPED events for the pool's workers equals the delta reported by
 *          goc_pool_get_steal_stats before and after that pool's lifetime
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
#include <uv.h>
#include <stdlib.h>
#include <string.h>

#include "test_harness.h"
#include "goc.h"
#include "wsdq.h"
#ifdef GOC_ENABLE_STATS
#include "goc_stats.h"
#endif

/* =========================================================================
 * done_t — lightweight fiber-to-main synchronisation via mutex + condvar
 *
 * Used throughout Phase 6 to let the main thread block until a target fiber
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
 * Portable barrier helper for tests
 *
 * libuv does not expose a barrier primitive, so we use a uv_mutex + uv_cond
 * shim on all platforms.
 * ====================================================================== */

typedef struct {
    uv_mutex_t mtx;
    uv_cond_t  cond;
    unsigned        parties;
    unsigned        arrived;
    unsigned        generation;
} goc_test_barrier_t;

static int goc_test_barrier_init(goc_test_barrier_t* b,
                                 const void* attr,
                                 unsigned count) {
    (void)attr;
    if (count == 0)
        return -1;
    uv_mutex_init(&b->mtx);
    uv_cond_init(&b->cond);
    b->parties = count;
    b->arrived = 0;
    b->generation = 0;
    return 0;
}

static int goc_test_barrier_wait(goc_test_barrier_t* b) {
    uv_mutex_lock(&b->mtx);
    unsigned gen = b->generation;
    b->arrived++;
    if (b->arrived == b->parties) {
        b->arrived = 0;
        b->generation++;
        uv_cond_broadcast(&b->cond);
        uv_mutex_unlock(&b->mtx);
        return 1;
    }
    while (gen == b->generation)
        uv_cond_wait(&b->cond, &b->mtx);
    uv_mutex_unlock(&b->mtx);
    return 0;
}

static int goc_test_barrier_destroy(goc_test_barrier_t* b) {
    uv_cond_destroy(&b->cond);
    uv_mutex_destroy(&b->mtx);
    return 0;
}

/*
 * done_wait_timeout — like done_wait but gives up after timeout_ms.
 * Returns true if the flag was set, false if the deadline was reached.
 */
static bool done_wait_timeout(done_t* d, int timeout_ms) {
    uint64_t timeout_ns = (uint64_t)timeout_ms * 1000000ULL;
    uv_mutex_lock(&d->mtx);
    int rc = 0;
    while (!d->flag && rc == 0)
        rc = uv_cond_timedwait(&d->cond, &d->mtx, timeout_ns);
    bool got = (d->flag != 0);
    d->flag = 0;
    uv_mutex_unlock(&d->mtx);
    return got;
}

/* =========================================================================
 * Phase 6 — Thread pool
 * ====================================================================== */

/* --- P6.1: pool lifecycle and goc_go_on dispatch ----------------------- */

/*
 * Argument bundle for test_p6_1_fiber_fn.
 *
 * expected_pool — pool the fiber expects to be running on; detected via an
 *                 out-of-band channel put (if the fiber runs we know it was
 *                 dispatched)
 * done          — signalled by the fiber when it has recorded its result
 * ran           — set to true by the fiber to confirm execution
 */
typedef struct {
    goc_pool* expected_pool;
    done_t*   done;
    bool      ran;
} p6_1_args_t;

/*
 * Fiber for P6.1.
 * Records that it ran and signals done.  The fact that it runs at all is
 * proof that goc_go_on dispatched it correctly.
 */
static void test_p6_1_fiber_fn(void* arg) {
    p6_1_args_t* a = (p6_1_args_t*)arg;
    a->ran = true;
    done_signal(a->done);
}

/*
 * P6.1 — goc_pool_make / goc_pool_destroy lifecycle; goc_go_on dispatches
 *         fiber to the correct pool
 *
 * Creates a new pool with 2 worker threads, dispatches a fiber onto it via
 * goc_go_on, waits for the fiber to complete, then destroys the pool.
 * Verifies:
 *   - goc_pool_make returns non-NULL
 *   - goc_go_on returns a non-NULL join channel
 *   - the fiber runs to completion (ran == true)
 *   - goc_take_sync on the join channel returns GOC_CLOSED after completion
 *   - goc_pool_destroy does not crash
 */
static void test_p6_1(void) {
    TEST_BEGIN("P6.1   goc_pool_make/destroy; goc_go_on dispatches fiber");
    done_t done;
    done_init(&done);

    goc_pool* pool = goc_pool_make(2);
    ASSERT(pool != NULL);

    p6_1_args_t args = { .expected_pool = pool, .done = &done, .ran = false };
    goc_chan* join = goc_go_on(pool, test_p6_1_fiber_fn, &args);
    ASSERT(join != NULL);

    done_wait(&done);
    ASSERT(args.ran == true);

    /* Join channel must close once the fiber exits. */
    goc_val_t* v = goc_take_sync(join);
    ASSERT(v->ok == GOC_CLOSED);

    /* Destroy the pool — must not crash or hang. */
    goc_pool_destroy(pool);

    done_destroy(&done);
    TEST_PASS();
done:;
}

/* --- P6.2: goc_pool_destroy_timeout — GOC_DRAIN_OK path ---------------- */

/*
 * Argument bundle for test_p6_2_fiber_fn.
 *
 * delay_us — how long the fiber sleeps before exiting (kept well below the
 *             destroy timeout so the drain must succeed)
 * done     — signalled after sleep so the main thread knows the fiber is live
 */
typedef struct {
    uint64_t delay_us;
    done_t*  started;
} p6_2_args_t;

/*
 * Fiber for P6.2.
 * Signals started, then sleeps briefly and exits.  The total lifetime is well
 * below the 500 ms drain deadline, so goc_pool_destroy_timeout must return
 * GOC_DRAIN_OK.
 */
static void test_p6_2_fiber_fn(void* arg) {
    p6_2_args_t* a = (p6_2_args_t*)arg;
    done_signal(a->started);  /* confirm the fiber is live */
    goc_nanosleep((uint64_t)a->delay_us * 1000);
    /* returns — fiber is done */
}

/*
 * P6.3 — goc_pool_destroy_timeout: GOC_DRAIN_OK when fibers finish in time
 *
 * Dispatches a short-lived fiber (50 ms sleep) then calls
 * goc_pool_destroy_timeout with a generous 500 ms deadline.  The call must
 * return GOC_DRAIN_OK and the pool must be destroyed (no further use).
 */
static void test_p6_2(void) {
    TEST_BEGIN("P6.2   goc_pool_destroy_timeout: GOC_DRAIN_OK in time");
    done_t started;
    done_init(&started);

    goc_pool* pool = goc_pool_make(2);
    ASSERT(pool != NULL);

    p6_2_args_t args = { .delay_us = 50000 /* 50 ms */, .started = &started };
    goc_chan* join = goc_go_on(pool, test_p6_2_fiber_fn, &args);
    ASSERT(join != NULL);

    done_wait(&started);  /* fiber is live; now race the timeout */

    goc_drain_result_t result = goc_pool_destroy_timeout(pool, 500 /* 500 ms */);
    ASSERT(result == GOC_DRAIN_OK);

    done_destroy(&started);
    TEST_PASS();
done:;
}

/* --- P6.3: goc_pool_destroy_timeout — GOC_DRAIN_TIMEOUT path ----------- */

/*
 * Argument bundle for the long-running fiber in P6.3.
 *
 * blocker — rendezvous channel; the fiber blocks on goc_take(blocker) until
 *            the main thread closes it, ensuring the pool is still busy when
 *            goc_pool_destroy_timeout is called
 * started — signalled just before the fiber parks on the blocker channel
 */
typedef struct {
    goc_chan* blocker;
    done_t*   started;
} p6_3_long_args_t;

/*
 * Long-running fiber for P6.3.
 * Parks on blocker until it is closed, then exits.  The main thread calls
 * goc_pool_destroy_timeout before closing the channel, so the drain must time
 * out.
 */
static void test_p6_3_long_fn(void* arg) {
    p6_3_long_args_t* a = (p6_3_long_args_t*)arg;
    done_signal(a->started);   /* tell main the fiber is about to park */
    goc_take(a->blocker);      /* park until main closes the channel */
}

/*
 * Short-lived verification fiber for P6.3.
 * Used after the timeout to confirm the pool is still valid and can run new
 * fibers.
 */
typedef struct {
    done_t* done;
    bool    ran;
} p6_3_short_args_t;

static void test_p6_3_short_fn(void* arg) {
    p6_3_short_args_t* a = (p6_3_short_args_t*)arg;
    a->ran = true;
    done_signal(a->done);
}

/*
 * P6.3 — goc_pool_destroy_timeout: GOC_DRAIN_TIMEOUT when fibers are still
 *         running; pool remains valid afterwards
 *
 * Dispatches a fiber that parks indefinitely on a blocker channel.  Calls
 * goc_pool_destroy_timeout with a short deadline (50 ms); the call must return
 * GOC_DRAIN_TIMEOUT and leave the pool in a running state.  Verifies that the
 * pool is still usable by dispatching a new short-lived fiber via goc_go_on
 * and confirming it runs.  Finally unblocks the long fiber by closing the
 * blocker channel and calls goc_pool_destroy to clean up.
 */
static void test_p6_3(void) {
    TEST_BEGIN("P6.3   goc_pool_destroy_timeout: GOC_DRAIN_TIMEOUT; pool valid");
    done_t long_started, short_done;
    done_init(&long_started);
    done_init(&short_done);

    goc_pool* pool = goc_pool_make(2);
    ASSERT(pool != NULL);

    goc_chan* blocker = goc_chan_make(0);
    ASSERT(blocker != NULL);

    /* Launch the long-running fiber that will keep the pool busy. */
    p6_3_long_args_t largs = { .blocker = blocker, .started = &long_started };
    goc_chan* long_join = goc_go_on(pool, test_p6_3_long_fn, &largs);
    ASSERT(long_join != NULL);

    done_wait(&long_started);  /* wait until the fiber is parked */

    /* Attempt to drain with a short timeout — must time out. */
    goc_drain_result_t result = goc_pool_destroy_timeout(pool, 50 /* 50 ms */);
    ASSERT(result == GOC_DRAIN_TIMEOUT);

    /* Pool must still be alive; dispatch a new short-lived fiber. */
    p6_3_short_args_t sargs = { .done = &short_done, .ran = false };
    goc_chan* short_join = goc_go_on(pool, test_p6_3_short_fn, &sargs);
    ASSERT(short_join != NULL);

    done_wait(&short_done);
    ASSERT(sargs.ran == true);

    goc_val_t* sv = goc_take_sync(short_join);
    ASSERT(sv->ok == GOC_CLOSED);

    /* Unblock the long fiber so the pool can be destroyed cleanly. */
    goc_close(blocker);

    goc_val_t* lv = goc_take_sync(long_join);
    ASSERT(lv->ok == GOC_CLOSED);

    goc_pool_destroy(pool);

    done_destroy(&long_started);
    done_destroy(&short_done);
    TEST_PASS();
done:;
}

/* --- P6.4: goc_malloc end-to-end — GC-heap linked list ----------------- */

/*
 * A singly-linked list node allocated on the GC heap.
 */
typedef struct p6_node {
    int             value;
    struct p6_node* next;
} p6_node_t;

/*
 * Argument bundle for test_p6_4_fiber_fn.
 *
 * count   — number of nodes to allocate
 * result  — set to the head of the built list before done is signalled
 * done    — signalled when the list is ready
 */
typedef struct {
    int        count;
    p6_node_t* result;
    done_t*    done;
} p6_4_args_t;

/*
 * Fiber for P6.4.
 * Builds a linked list of `count` nodes on the GC heap.  Each node's value is
 * its 1-based index.  Stores the head in args->result and signals done.
 */
static void test_p6_4_fiber_fn(void* arg) {
    p6_4_args_t* a = (p6_4_args_t*)arg;

    p6_node_t* head = NULL;
    for (int i = a->count; i >= 1; i--) {
        p6_node_t* node = (p6_node_t*)goc_malloc(sizeof(p6_node_t));
        node->value = i;
        node->next  = head;
        head        = node;
    }
    a->result = head;
    done_signal(a->done);
}

/*
 * P6.4 — goc_malloc end-to-end: fiber builds GC-heap linked list, main
 *         traverses after join
 *
 * A fiber allocates a linked list of 16 nodes via goc_malloc and stores the
 * head pointer.  After the fiber signals completion, the main thread traverses
 * the list and verifies that all 16 values are present and in order (1..16).
 * This confirms that GC-heap memory remains live and accessible after the
 * allocating fiber has exited.
 */
static void test_p6_4(void) {
    TEST_BEGIN("P6.4   goc_malloc: GC-heap linked list survives fiber exit");

    const int count = 16;

    done_t done;
    done_init(&done);

    p6_4_args_t args = { .count = count, .result = NULL, .done = &done };
    goc_chan* join = goc_go(test_p6_4_fiber_fn, &args);
    ASSERT(join != NULL);

    done_wait(&done);

    /* Wait for the fiber to fully exit (join channel closes). */
    goc_val_t* v = goc_take_sync(join);
    ASSERT(v->ok == GOC_CLOSED);

    /* Traverse the list; every node must be present and in order. */
    p6_node_t* node = args.result;
    for (int i = 1; i <= count; i++) {
        ASSERT(node != NULL);
        ASSERT(node->value == i);
        node = node->next;
    }
    ASSERT(node == NULL);  /* list length must be exactly count */

    done_destroy(&done);
    TEST_PASS();
done:;
}

/* --- P6.5: goc_alts with n > GOC_ALTS_STACK_THRESHOLD arms ------------ */

/*
 * Number of arms in the P6.5 select.  Must be > GOC_ALTS_STACK_THRESHOLD (8)
 * to force the malloc path in alts_dedup_sort_channels.
 */
#define P6_5_ARM_COUNT 10

/*
 * Argument bundle for test_p6_5_fiber_fn.
 *
 * channels  — array of P6_5_ARM_COUNT channels; one will be pre-loaded
 * winner_idx — index of the channel pre-loaded with a value
 * result    — populated by the fiber with the goc_alts_result
 * done      — signalled when the fiber has recorded its result
 */
typedef struct {
    goc_chan*       channels[P6_5_ARM_COUNT];
    int             winner_idx;
    goc_alts_result* result;
    done_t*         done;
} p6_5_args_t;

/*
 * Fiber for P6.5.
 * Calls goc_alts with P6_5_ARM_COUNT take arms.  Because one channel is
 * pre-loaded, the fast-path scan fires immediately on that arm.  The channel
 * pointer scratch buffer must be malloc-allocated (n > GOC_ALTS_STACK_THRESHOLD).
 */
static void test_p6_5_fiber_fn(void* arg) {
    p6_5_args_t* a = (p6_5_args_t*)arg;

    goc_alt_op ops[P6_5_ARM_COUNT];
    for (int i = 0; i < P6_5_ARM_COUNT; i++) {
        ops[i].ch       = a->channels[i];
        ops[i].op_kind  = GOC_ALT_TAKE;
        ops[i].put_val  = NULL;
    }

    a->result = goc_alts(ops, P6_5_ARM_COUNT);
    done_signal(a->done);
}

/*
 * P6.5 — goc_alts with n > GOC_ALTS_STACK_THRESHOLD arms
 *
 * Creates P6_5_ARM_COUNT (10) buffered channels.  Pre-loads the last channel
 * (index P6_5_ARM_COUNT - 1) with a known value.  Launches a fiber that calls
 * goc_alts with take arms over all P6_5_ARM_COUNT channels.  The winning arm
 * must fire immediately (fast path) on the pre-loaded channel, returning the
 * correct index and value.  No memory error must be observed (run under ASAN
 * to catch heap misuse from the malloc/free path in alts_dedup_sort_channels).
 */
static void test_p6_5(void) {
    TEST_BEGIN("P6.5   goc_alts: n > GOC_ALTS_STACK_THRESHOLD; malloc path");

    done_t done;
    done_init(&done);

    p6_5_args_t args;
    args.done       = &done;
    args.winner_idx = P6_5_ARM_COUNT - 1;

    for (int i = 0; i < P6_5_ARM_COUNT; i++) {
        args.channels[i] = goc_chan_make(1);
        ASSERT(args.channels[i] != NULL);
    }

    /* Pre-load only the last channel so the winning arm is deterministic. */
    goc_status_t st = goc_put_sync(args.channels[args.winner_idx],
                                   goc_box_uint(0xABCD));
    ASSERT(st == GOC_OK);

    goc_chan* join = goc_go(test_p6_5_fiber_fn, &args);
    ASSERT(join != NULL);

    done_wait(&done);

    ASSERT(args.result->ch == args.channels[args.winner_idx]);
    ASSERT(args.result->value.ok  == GOC_OK);
    ASSERT(goc_unbox_uint(args.result->value.val) == 0xABCD);

    goc_val_t* v = goc_take_sync(join);
    ASSERT(v->ok == GOC_CLOSED);

    for (int i = 0; i < P6_5_ARM_COUNT; i++) {
        goc_close(args.channels[i]);
    }

    done_destroy(&done);
    TEST_PASS();
done:;
}

/* =========================================================================
 * Phase 6 (continued) — goc_wsdq and goc_injector unit tests
 *
 * These tests exercise the work-stealing deque and MPSC injector queue
 * directly, without an active pool. Entries are malloc+memset'd; goc_init
 * has already been called above so GC is available, but these tests do not
 * rely on fiber scheduling.
 * ====================================================================== */

#define P6_N 10000
#define P6_T 4   /* thief / producer threads */

/* Helper: allocate a zeroed goc_entry with arm_idx set to seq */
static goc_entry* make_entry(int seq) {
    goc_entry* e = malloc(sizeof(goc_entry));
    memset(e, 0, sizeof(goc_entry));
    e->arm_idx = (size_t)seq;
    return e;
}

/* ---- P6.6 ---- */
static void test_p6_6(void) {
    TEST_BEGIN("P6.6  wsdq push/pop round-trip (LIFO, single-threaded)");

    goc_wsdq dq;
    wsdq_init(&dq, 8);

    goc_entry* entries[16];
    for (int i = 0; i < 16; i++) {
        entries[i] = make_entry(i);
        wsdq_push_bottom(&dq, entries[i]);
    }
    for (int i = 15; i >= 0; i--) {
        goc_entry* e = wsdq_pop_bottom(&dq);
        ASSERT(e != NULL);
        ASSERT((int)e->arm_idx == i);
        free(e);
    }
    ASSERT(wsdq_pop_bottom(&dq) == NULL);

    wsdq_destroy(&dq);
    TEST_PASS();
done:;
}

/* ---- P6.7 ---- */
static void test_p6_7(void) {
    TEST_BEGIN("P6.7  wsdq pop_bottom on empty returns NULL");

    goc_wsdq dq;
    wsdq_init(&dq, 4);
    ASSERT(wsdq_pop_bottom(&dq) == NULL);
    wsdq_destroy(&dq);
    TEST_PASS();
done:;
}

/* ---- P6.8 ---- */
static void test_p6_8(void) {
    TEST_BEGIN("P6.8  wsdq steal_top on empty returns NULL");

    goc_wsdq dq;
    wsdq_init(&dq, 4);
    ASSERT(wsdq_steal_top(&dq) == NULL);
    wsdq_destroy(&dq);
    TEST_PASS();
done:;
}

/* ---- P6.9 ---- */
static void test_p6_9(void) {
    TEST_BEGIN("P6.9  wsdq steal_top ordering is FIFO");

    goc_wsdq dq;
    wsdq_init(&dq, 4);

    goc_entry* e1 = make_entry(1);
    goc_entry* e2 = make_entry(2);
    goc_entry* e3 = make_entry(3);
    wsdq_push_bottom(&dq, e1);
    wsdq_push_bottom(&dq, e2);
    wsdq_push_bottom(&dq, e3);

    goc_entry* r1 = wsdq_steal_top(&dq);
    goc_entry* r2 = wsdq_steal_top(&dq);
    goc_entry* r3 = wsdq_steal_top(&dq);
    ASSERT(r1 == e1);
    ASSERT(r2 == e2);
    ASSERT(r3 == e3);
    ASSERT(wsdq_steal_top(&dq) == NULL);

    free(e1); free(e2); free(e3);
    wsdq_destroy(&dq);
    TEST_PASS();
done:;
}

/* ---- P6.10 ---- */
static void test_p6_10(void) {
    TEST_BEGIN("P6.10  wsdq grows when full (single-threaded)");

    goc_wsdq dq;
    wsdq_init(&dq, 4);

    int n = 256;
    goc_entry** arr = malloc(n * sizeof(goc_entry*));
    for (int i = 0; i < n; i++) {
        arr[i] = make_entry(i);
        wsdq_push_bottom(&dq, arr[i]);
    }
    for (int i = n - 1; i >= 0; i--) {
        goc_entry* e = wsdq_pop_bottom(&dq);
        ASSERT(e != NULL);
        ASSERT((int)e->arm_idx == i);
        free(e);
    }
    ASSERT(wsdq_pop_bottom(&dq) == NULL);

    free(arr);
    wsdq_destroy(&dq);
    TEST_PASS();
done:;
}

/* ---- P6.11: concurrent owner-push / thief-steal ---- */

typedef struct {
    goc_wsdq*   dq;
    bool*          seen;
    int*           total;
    uv_mutex_t* mu;
    _Atomic bool*  done;
} p6_11_thief_arg;

static void p6_11_thief(void* arg) {
    p6_11_thief_arg* a = (p6_11_thief_arg*)arg;
    while (true) {
        goc_entry* e = wsdq_steal_top(a->dq);
        if (e != NULL) {
            uv_mutex_lock(a->mu);
            a->seen[e->arm_idx] = true;
            (*a->total)++;
            uv_mutex_unlock(a->mu);
            free(e);
        } else if (atomic_load_explicit(a->done, memory_order_acquire)) {
            /* drain loop */
            while ((e = wsdq_steal_top(a->dq)) != NULL) {
                uv_mutex_lock(a->mu);
                a->seen[e->arm_idx] = true;
                (*a->total)++;
                uv_mutex_unlock(a->mu);
                free(e);
            }
            break;
        }
    }
}

static void test_p6_11(void) {
    TEST_BEGIN("P6.11  wsdq concurrent owner-push / thief-steal");

    goc_wsdq dq;
    wsdq_init(&dq, 256);

    bool* seen = calloc(P6_N, sizeof(bool));
    int total = 0;
    uv_mutex_t mu;
    uv_mutex_init(&mu);
    _Atomic bool done = false;

    p6_11_thief_arg arg = { &dq, seen, &total, &mu, &done };

    uv_thread_t thieves[P6_T];
    for (int t = 0; t < P6_T; t++)
        uv_thread_create(&thieves[t], p6_11_thief, &arg);

    for (int i = 0; i < P6_N; i++)
        wsdq_push_bottom(&dq, make_entry(i));

    atomic_store_explicit(&done, true, memory_order_release);

    for (int t = 0; t < P6_T; t++)
        uv_thread_join(&thieves[t]);

    /* Owner drains remainder */
    goc_entry* e;
    while ((e = wsdq_pop_bottom(&dq)) != NULL) {
        seen[e->arm_idx] = true;
        total++;
        free(e);
    }

    ASSERT(total == P6_N);
    for (int i = 0; i < P6_N; i++)
        ASSERT(seen[i]);

    uv_mutex_destroy(&mu);
    free(seen);
    wsdq_destroy(&dq);
    TEST_PASS();
done:;
}

/* ---- P6.12: concurrent push + pop + steal under contention ---- */

typedef struct {
    goc_wsdq*    dq;
    bool*           seen;
    int*            total;
    uv_mutex_t* mu;
    _Atomic bool*   done;
} p6_12_thief_arg;

static void p6_12_thief(void* arg) {
    p6_12_thief_arg* a = (p6_12_thief_arg*)arg;
    goc_entry* e;
    while (true) {
        e = wsdq_steal_top(a->dq);
        if (e != NULL) {
            uv_mutex_lock(a->mu);
            a->seen[e->arm_idx] = true;
            (*a->total)++;
            uv_mutex_unlock(a->mu);
            free(e);
        } else if (atomic_load_explicit(a->done, memory_order_acquire)) {
            while ((e = wsdq_steal_top(a->dq)) != NULL) {
                uv_mutex_lock(a->mu);
                a->seen[e->arm_idx] = true;
                (*a->total)++;
                uv_mutex_unlock(a->mu);
                free(e);
            }
            break;
        }
    }
}

static void test_p6_12(void) {
    TEST_BEGIN("P6.12  wsdq concurrent push+pop+steal under contention");

    goc_wsdq dq;
    wsdq_init(&dq, 256);

    bool* seen = calloc(P6_N, sizeof(bool));
    int total = 0;
    uv_mutex_t mu;
    uv_mutex_init(&mu);
    _Atomic bool done = false;

    p6_12_thief_arg arg = { &dq, seen, &total, &mu, &done };

    uv_thread_t thieves[P6_T];
    for (int t = 0; t < P6_T; t++)
        uv_thread_create(&thieves[t], p6_12_thief, &arg);

    /* Owner: push N entries, immediately try to pop each one back */
    for (int i = 0; i < P6_N; i++) {
        wsdq_push_bottom(&dq, make_entry(i));
        goc_entry* e = wsdq_pop_bottom(&dq);
        if (e != NULL) {
            uv_mutex_lock(&mu);
            seen[e->arm_idx] = true;
            total++;
            uv_mutex_unlock(&mu);
            free(e);
        }
    }

    atomic_store_explicit(&done, true, memory_order_release);

    for (int t = 0; t < P6_T; t++)
        uv_thread_join(&thieves[t]);

    /* Owner drains remainder */
    goc_entry* e;
    while ((e = wsdq_pop_bottom(&dq)) != NULL) {
        seen[e->arm_idx] = true;
        total++;
        free(e);
    }

    ASSERT(total == P6_N);
    for (int i = 0; i < P6_N; i++)
        ASSERT(seen[i]);

    uv_mutex_destroy(&mu);
    free(seen);
    wsdq_destroy(&dq);
    TEST_PASS();
done:;
}

/* ---- P6.13 ---- */
static void test_p6_13(void) {
    TEST_BEGIN("P6.13  wsdq_destroy on non-empty deque is safe");

    goc_wsdq dq;
    wsdq_init(&dq, 4);

    /* Push entries but do not drain; destroy should not crash */
    goc_entry* entries[8];
    for (int i = 0; i < 8; i++) {
        entries[i] = make_entry(i);
        wsdq_push_bottom(&dq, entries[i]);
    }
    wsdq_destroy(&dq);

    /* Free the entries ourselves (caller-managed) */
    for (int i = 0; i < 8; i++)
        free(entries[i]);

    TEST_PASS();
done:;
}

/* ---- P6.14 ---- */
static void test_p6_14(void) {
    TEST_BEGIN("P6.14  injector push/pop round-trip (FIFO, single-threaded)");

    goc_injector inj;
    injector_init(&inj);

    goc_entry* entries[64];
    for (int i = 0; i < 64; i++) {
        entries[i] = make_entry(i);
        injector_push(&inj, entries[i]);
    }
    for (int i = 0; i < 64; i++) {
        goc_entry* e = injector_pop(&inj);
        ASSERT(e != NULL);
        ASSERT((int)e->arm_idx == i);
        free(e);
    }
    ASSERT(injector_pop(&inj) == NULL);

    injector_destroy(&inj);
    TEST_PASS();
done:;
}

/* ---- P6.15 ---- */
static void test_p6_15(void) {
    TEST_BEGIN("P6.15 injector_pop on empty returns NULL");

    goc_injector inj;
    injector_init(&inj);
    ASSERT(injector_pop(&inj) == NULL);
    injector_destroy(&inj);
    TEST_PASS();
done:;
}

/* ---- P6.16: concurrent injector push from multiple threads ---- */

typedef struct {
    goc_injector* inj;
    int           start;  /* sequence numbers [start, start + count) */
    int           count;
} p6_16_prod_arg;

static void p6_16_producer(void* arg) {
    p6_16_prod_arg* a = (p6_16_prod_arg*)arg;
    for (int i = 0; i < a->count; i++)
        injector_push(a->inj, make_entry(a->start + i));
}

static void test_p6_16(void) {
    TEST_BEGIN("P6.16 injector concurrent push from multiple threads");

    goc_injector inj;
    injector_init(&inj);

    int per_thread = P6_N / P6_T;
    p6_16_prod_arg args[P6_T];
    uv_thread_t prods[P6_T];
    for (int t = 0; t < P6_T; t++) {
        args[t].inj   = &inj;
        args[t].start = t * per_thread;
        args[t].count = per_thread;
        uv_thread_create(&prods[t], p6_16_producer, &args[t]);
    }
    for (int t = 0; t < P6_T; t++)
        uv_thread_join(&prods[t]);

    bool* seen = calloc(P6_N, sizeof(bool));
    int total = 0;
    goc_entry* e;
    while ((e = injector_pop(&inj)) != NULL) {
        ASSERT(!seen[e->arm_idx]);
        seen[e->arm_idx] = true;
        total++;
        free(e);
    }
    ASSERT(total == P6_N);
    for (int i = 0; i < P6_N; i++)
        ASSERT(seen[i]);

    free(seen);
    injector_destroy(&inj);
    TEST_PASS();
done:;
}

/* ---- P6.17: pop_bottom / steal_top race on last element ---- */
/*
 * Stress-tests the last-element race between wsdq_pop_bottom (owner, no
 * lock) and wsdq_steal_top (thief, holds steal_lock).  One entry is pushed
 * per round; a barrier synchronises the owner pop and the thief steal
 * so they start simultaneously.  Exactly one must win each round — no double-
 * delivery, no loss, and no hang.
 */
#define P6_17_ROUNDS 100000

typedef struct {
    goc_wsdq*          dq;
    _Atomic int*       thief_got;
    goc_test_barrier_t* barrier;
    int                rounds;
} p6_17_thief_arg;

static void p6_17_thief(void* arg) {
    p6_17_thief_arg* a = (p6_17_thief_arg*)arg;
    for (int r = 0; r < a->rounds; r++) {
        goc_test_barrier_wait(a->barrier);   /* race start */
        goc_entry* e = wsdq_steal_top(a->dq);
        if (e != NULL) {
            atomic_fetch_add_explicit(a->thief_got, 1, memory_order_relaxed);
            free(e);
        }
        goc_test_barrier_wait(a->barrier);   /* round end */
    }
}

static void test_p6_17(void) {
    TEST_BEGIN("P6.17  pop_bottom/steal_top last-element race: no double-delivery, no hang");

    goc_wsdq dq;
    wsdq_init(&dq, 2);

    _Atomic int thief_got = 0;
    goc_test_barrier_t barrier;
    ASSERT(goc_test_barrier_init(&barrier, NULL, 2) == 0);

    p6_17_thief_arg ta = { &dq, &thief_got, &barrier, P6_17_ROUNDS };
    uv_thread_t thief;
    uv_thread_create(&thief, p6_17_thief, &ta);

    int owner_got = 0;
    for (int r = 0; r < P6_17_ROUNDS; r++) {
        wsdq_push_bottom(&dq, make_entry(0));
        goc_test_barrier_wait(&barrier);   /* race start: both race for the single entry */
        goc_entry* e = wsdq_pop_bottom(&dq);
        if (e != NULL) {
            owner_got++;
            free(e);
        }
        goc_test_barrier_wait(&barrier);   /* round end: thief has finished its steal attempt */
    }

    uv_thread_join(&thief);
    goc_test_barrier_destroy(&barrier);

    /* Every pushed entry must be consumed exactly once (no double-delivery, no loss). */
    int total = owner_got + atomic_load_explicit(&thief_got, memory_order_relaxed);
    ASSERT(total == P6_17_ROUNDS);

    wsdq_destroy(&dq);
    TEST_PASS();
done:;
}

/* ---- P6.18: internal post — burst of child fibers all complete ---- */
/*
 * Bugs being tested:
 *
 * (A) pool.c internal path in post_to_run_queue (bug #4):
 *   When a fiber posts work internally, the code always wakes
 *   workers[(w->index + 1) % N] regardless of which worker is actually idle.
 *   If that fixed neighbor is busy, other idle workers beyond it are never
 *   notified and their steal loops only run after they happen to wake for
 *   another reason.  Under a burst of N_CHILDREN internal posts this means
 *   entries pile up in the spawner's deque and may never reach the idle
 *   workers at index+2, index+3, …
 *
 * (B) Deferred fiber materialization GC race (introduced by work-stealing
 *   commit): goc_go_on now defers goc_fiber_materialize to the first
 *   dispatch on a worker thread (entry->coro == NULL at spawn time).  Under
 *   a large burst multiple workers race to call goc_fiber_materialize
 *   concurrently; each call registers a GC root via goc_fiber_root_register.
 *   If the registration logic has a race or the root table is exhausted
 *   before slots are reused, BDW-GC aborts with signal 6 (SIGABRT /
 *   "Too many root sets").  A crash (not just a timeout) from this test
 *   indicates that race.
 *
 * Test strategy:
 *   A parent fiber (running on some worker W) calls goc_go_on N_CHILDREN
 *   times.  Each call takes the internal path: push entry to W's deque,
 *   then unconditionally wake workers[(W+1)%N].  The parent then exits.
 *   Each child atomically increments a shared counter; when it reaches
 *   N_CHILDREN the last child signals done.  The main thread waits with a
 *   3-second deadline.
 *   - A timeout indicates bug (A): idle workers were not notified and
 *     entries stagnated in W's deque.
 *   - A crash (SIGABRT from GC) indicates bug (B): deferred materialization
 *     race under concurrent burst spawning.
 */
#define P6_18_WORKERS  4
#define P6_18_CHILDREN 500

typedef struct {
    goc_pool*    pool;
    done_t*      done;
    _Atomic int* completed;
    int          total;
} p6_18_arg_t;

static void p6_18_child_fn(void* arg) {
    p6_18_arg_t* a = (p6_18_arg_t*)arg;
    /* Signal when the last child finishes. */
    if (atomic_fetch_add_explicit(a->completed, 1, memory_order_acq_rel) + 1
            == a->total)
        done_signal(a->done);
}

static void p6_18_spawner_fn(void* arg) {
    p6_18_arg_t* a = (p6_18_arg_t*)arg;
    /* All goc_go_on calls here take the *internal* post path because
     * tl_worker != NULL (we are running inside a pool worker fiber). */
    for (int i = 0; i < a->total; i++)
        goc_go_on(a->pool, p6_18_child_fn, a);
}

static void test_p6_18(void) {
    TEST_BEGIN("P6.18  internal post: burst of child fibers all complete");

    done_t done;
    done_init(&done);
    _Atomic int completed = 0;

    goc_pool* pool = goc_pool_make(P6_18_WORKERS);
    ASSERT(pool != NULL);

    p6_18_arg_t args = { pool, &done, &completed, P6_18_CHILDREN };

    /* Dispatch the spawner via *external* post so tl_worker is set when the
     * spawner runs, making every child dispatch hit the internal path. */
    goc_go_on(pool, p6_18_spawner_fn, &args);

    /* 3-second deadline: a timeout means idle workers were never woken and
     * entries stagnated in the spawner's deque. */
    bool ok = done_wait_timeout(&done, 3000);
    ASSERT(ok);

    goc_pool_destroy(pool);
    done_destroy(&done);
    TEST_PASS();
done:;
}

/* ---- P6.19: external post sleep-miss — worker wakes reliably ---- */
/*
 * Bug being tested (pool.c external path in post_to_run_queue):
 *   The external path relies on the mutex unlock inside injector_push to
 *   supply the seq_cst barrier needed to close the sleep-miss race.  A
 *   C11 mutex unlock is a *release*, not seq_cst.  On ARM/POWER the
 *   injector_push unlock may not order before the subsequent seq_cst
 *   idle_count read, so the poster could observe idle_count == 0 (stale),
 *   skip the sem_post, and leave a sleeping worker with work in its injector.
 *
 * Test strategy:
 *   Repeat N_ROUNDS times:
 *     1. Wait for the previous fiber to complete (guarantees all workers
 *        have had time to drain their deques and go idle in uv_sem_wait).
 *     2. goc_nanosleep(1ms) to let all workers settle into sem_wait.
 *     3. Post one fiber via goc_go_on (external path — called from the
 *        main OS thread, so tl_worker == NULL).
 *     4. Wait up to 2 seconds for it to signal done.
 *   A timeout on any round means an external post was lost: the sleep-miss
 *   race fired and the injected entry sat unseen in a worker's injector.
 */
#define P6_19_WORKERS 4
#define P6_19_ROUNDS  200

typedef struct {
    done_t* done;
} p6_19_arg_t;

static void p6_19_fiber_fn(void* arg) {
    p6_19_arg_t* a = (p6_19_arg_t*)arg;
    done_signal(a->done);
}

static void test_p6_19(void) {
    TEST_BEGIN("P6.19  external post sleep-miss: worker wakes reliably after idle");

    done_t done;
    done_init(&done);

    goc_pool* pool = goc_pool_make(P6_19_WORKERS);
    ASSERT(pool != NULL);

    p6_19_arg_t args = { &done };

    for (int i = 0; i < P6_19_ROUNDS; i++) {
        /* Give workers time to drain and enter uv_sem_wait. */
        goc_nanosleep(1000000); /* 1 ms */

        /* External post: main thread → tl_worker == NULL → injector path. */
        goc_go_on(pool, p6_19_fiber_fn, &args);

        /* 2-second deadline per round. Timeout = sleep-miss: posted entry
         * sat in injector while the worker slept without being woken. */
        bool ok = done_wait_timeout(&done, 2000);
        if (!ok) {
            ASSERT(false);  /* print location and jump to cleanup */
            break;
        }
    }

    goc_pool_destroy(pool);
    done_destroy(&done);
    TEST_PASS();
done:;
}

/* ---- P6.20: external post targets busy worker — tasks still complete ---- */
/*
 * Bugs being tested:
 *
 * (A) pool.c external path — wasted sem post to busy worker (bug #6):
 *   post_to_run_queue's external path both pushes the entry into
 *   workers[idx].injector AND posts workers[idx].idle_sem.  If workers[idx]
 *   is currently inside mco_resume (busy running a fiber), the sem post is
 *   wasted: it increments the semaphore count of a non-sleeping worker.
 *   Idle workers at other indices have no way to steal from workers[idx]'s
 *   injector (injectors are not stealable), so they cannot help.  The entry
 *   sits in workers[idx].injector until workers[idx] itself loops back and
 *   drains it.
 *
 * (B) Deferred fiber materialization GC race (introduced by work-stealing
 *   commit): the burst of externally-posted tasks all have entry->coro == NULL
 *   at dispatch time.  When the busy worker eventually drains its injector and
 *   processes each entry, it calls goc_fiber_materialize for each one.
 *   Multiple workers may race to materialize concurrently, contending on the
 *   GC root registration table.  A SIGABRT crash from BDW-GC during this test
 *   indicates that race (same underlying issue as in P6.18).
 *
 * Test strategy:
 *   1. 2-worker pool so round-robin alternates strictly between workers 0 and 1.
 *   2. Keep worker 0 occupied for ~20 ms by dispatching a CPU-spinning fiber
 *      (tight loop, no yield) so it stays inside mco_resume.
 *   3. During that window post N tasks externally from the main thread.
 *      Odd-numbered round-robin targets (idx=1,3,5,…) land in worker 1's
 *      injector and are processed immediately.  Even-numbered targets
 *      (idx=0,2,4,…) land in worker 0's injector, which is not drained until
 *      the spinning fiber exits ~20 ms later.  The sem post to worker 0's
 *      idle_sem is wasted; worker 1 cannot steal worker 0's injector.
 *   4. Assert all N tasks complete within 3 seconds (they must, once the
 *      spinner exits and worker 0 drains its injector).
 *   - A timeout means bug (A): the wasted sem post caused queued entries to
 *     stagnate indefinitely.
 *   - A crash (SIGABRT from GC) means bug (B): deferred materialization
 *     of the burst tasks raced against concurrent GC root registration.
 */
#define P6_20_WORKERS  2
#define P6_20_TASKS    200

typedef struct {
    _Atomic int* completed;
    int          total;
    done_t*      done;
} p6_20_task_arg_t;

static void p6_20_task_fn(void* arg) {
    p6_20_task_arg_t* a = (p6_20_task_arg_t*)arg;
    if (atomic_fetch_add_explicit(a->completed, 1, memory_order_acq_rel) + 1
            == a->total)
        done_signal(a->done);
}

typedef struct {
    done_t* started;
} p6_20_spinner_arg_t;

/* CPU-spinning fiber: burns ~20 ms without yielding. */
static void p6_20_spinner_fn(void* arg) {
    p6_20_spinner_arg_t* a = (p6_20_spinner_arg_t*)arg;
    done_signal(a->started);
    volatile long x = 0;
    /* ~20 ms of work at typical clock speeds. */
    for (long i = 0; i < 50000000L; i++) x += i;
    (void)x;
}

static void test_p6_20(void) {
    TEST_BEGIN("P6.20  external post busy-worker: injector tasks still complete");

    done_t done, spinner_started;
    done_init(&done);
    done_init(&spinner_started);

    _Atomic int completed = 0;

    goc_pool* pool = goc_pool_make(P6_20_WORKERS);
    ASSERT(pool != NULL);

    /* Launch the spinner first; wait until it's actually running so that
     * next_push_idx is advanced and the subsequent task posts land on both
     * workers in a predictable round-robin pattern. */
    p6_20_spinner_arg_t sarg = { &spinner_started };
    goc_go_on(pool, p6_20_spinner_fn, &sarg);
    done_wait(&spinner_started);  /* spinner is now burning CPU on one worker */

    /* Post all tasks while the spinner holds one worker inside mco_resume. */
    p6_20_task_arg_t targ = { &completed, P6_20_TASKS, &done };
    for (int i = 0; i < P6_20_TASKS; i++)
        goc_go_on(pool, p6_20_task_fn, &targ);

    /* 3-second deadline: must complete even though ~half the tasks land in
     * the busy worker's injector and sit there until the spinner exits. */
    bool ok = done_wait_timeout(&done, 3000);
    ASSERT(ok);

    goc_pool_destroy(pool);
    done_destroy(&done);
    done_destroy(&spinner_started);
    TEST_PASS();
done:;
}

/* ---- P6.21: idle steal gap — stealable work after double-check ---- */
/*
 * Bugs being tested:
 *
 * (A) pool.c worker loop — idle double-check skips steal phase (bug #7):
 *   The idle double-check (step 4 in pool_worker_fn) re-checks only the
 *   worker's own injector and its own deque before sleeping.  It does NOT
 *   re-run the steal phase (step 3).  Combined with the fixed-neighbor
 *   wakeup in the internal path (bug #4), a window exists:
 *
 *     1. Workers W1 … W(N-1) all complete step 3 (steal), find nothing,
 *        increment idle_count, do the double-check (still nothing), and
 *        are about to call uv_sem_wait.
 *     2. W0 is running a spawner fiber that calls goc_go_on K times
 *        (internal post): each post pushes one entry onto W0's deque and
 *        wakes workers[(W0+1)%N] = W1 only.
 *     3. W1 is woken K times by the K sem posts, but it may not steal all K
 *        entries fast enough while W2 … W(N-1) stay sleeping — they were
 *        past the steal check and will not retry it before sleeping.
 *   A timeout indicates this bug.
 *
 * (B) Deferred fiber materialization GC race (introduced by work-stealing
 *   commit): all 1000 child entries have entry->coro == NULL at spawn time.
 *   When workers steal and run them they call goc_fiber_materialize
 *   concurrently, racing on GC root registration.  A SIGABRT crash from
 *   BDW-GC during this test indicates that race (same issue as P6.18/P6.20).
 *
 * Test strategy: N=4 workers, spawner creates 1000 entries via internal
 * posts.  All entries must be processed within 5 seconds.
 *   - A timeout means bug (A): workers stuck past the steal double-check
 *     were never re-woken and entries stagnated in W0's deque.
 *   - A crash (SIGABRT from GC) means bug (B): concurrent materialization
 *     race under the large burst.
 *
 * Distinction from P6.18: P6.18 uses 500 children + 3s deadline.
 * P6.21 uses 1000 children with an explicit "deep-idle" pre-settle
 * (goc_nanosleep before the spawner) to maximise the probability that W1-W3
 * are past their steal check when the burst arrives.
 */
#define P6_21_WORKERS  4
#define P6_21_CHILDREN 1000

typedef struct {
    goc_pool*    pool;
    done_t*      done;
    _Atomic int* completed;
    int          total;
} p6_21_arg_t;

static void p6_21_child_fn(void* arg) {
    p6_21_arg_t* a = (p6_21_arg_t*)arg;
    if (atomic_fetch_add_explicit(a->completed, 1, memory_order_acq_rel) + 1
            == a->total)
        done_signal(a->done);
}

static void p6_21_spawner_fn(void* arg) {
    p6_21_arg_t* a = (p6_21_arg_t*)arg;
    for (int i = 0; i < a->total; i++)
        goc_go_on(a->pool, p6_21_child_fn, a);
}

static void test_p6_21(void) {
    TEST_BEGIN("P6.21  idle steal gap: stealable work after double-check still completes");

    done_t done;
    done_init(&done);
    _Atomic int completed = 0;

    goc_pool* pool = goc_pool_make(P6_21_WORKERS);
    ASSERT(pool != NULL);

    /* Let all workers settle deep into uv_sem_wait before the burst arrives,
     * maximising the chance they are past the steal phase at step 3. */
    goc_nanosleep(5000000); /* 5 ms */

    p6_21_arg_t args = { pool, &done, &completed, P6_21_CHILDREN };
    goc_go_on(pool, p6_21_spawner_fn, &args);

    /* 5-second deadline: a timeout means workers stuck past the steal
     * double-check were never re-woken and entries stagnated in W0's deque. */
    bool ok = done_wait_timeout(&done, 5000);
    ASSERT(ok);

    goc_pool_destroy(pool);
    done_destroy(&done);
    TEST_PASS();
done:;
}

/* ---- P6.22: double-resume via stack-address reuse ---- */
/*
 * Bug being tested (channel.c slow path — fixed by commit "GC allocate parked
 * entries to avoid race condition"):
 *
 *   goc_take and goc_put originally stack-allocated the parking goc_entry:
 *     goc_entry e = {0};  // on the fiber's coroutine stack
 *   When the fiber parked, it appended &e to the channel's takers/putters
 *   list, then yielded.
 *
 *   Race sequence (requires work-stealing):
 *     1. Fiber A calls goc_take(ch) → slow path → stack entry at address X
 *        with woken=0, appended to takers list.
 *     2. Fiber B calls goc_put(ch) → wake(e_at_X):
 *          a. CAS: woken 0→1 on entry X.
 *          b. Spin: waiting for fe->parked to become 1 (fiber A truly yielded).
 *          c. post_to_run_queue dispatches A to B's deque (entry at addr Y).
 *     3. Worker W2 STEALS Y from B's deque and resumes A before B's spin ends.
 *     4. A returns from goc_take (local_result/e.ok read), immediately calls
 *        goc_take(ch) again in a loop — same call depth → new goc_entry e
 *        at the SAME stack address X, with woken=0, parked=1.
 *     5. B's spin completes (parked==1 from the NEW entry at X), reads
 *        woken==0 on the new entry, wins a SECOND CAS, posts A again.
 *     6. A is double-resumed: one worker resumes an already-running coro →
 *        MCO abort / stall / data corruption.
 *
 *   Fix: heap-allocate the parking entry via goc_malloc so each parking
 *   instance has a unique address; old entries are never confused with new.
 *
 * Test strategy:
 *   P6_22_PAIRS pairs of fibers, each pair doing P6_22_ROUNDS of ping-pong
 *   on an unbuffered channel pair (req + ack).  Sender and receiver each
 *   park on goc_take in a tight loop — re-parking at the exact same stack
 *   offset on every iteration.  A large pool (P6_22_WORKERS) maximises the
 *   steal probability that opens the race window.
 *
 *   If the bug is present: one fiber in a pair is double-resumed; its
 *   partner hangs waiting for a rendezvous that never comes → test times out.
 *   If the fix holds: all pairs complete within the deadline.
 *
 *   A timeout from this test has two possible causes:
 *   (1) Double-resume (this bug): a waker posts a fiber twice; the second
 *       resume races against the first, corrupting coroutine state so the
 *       partner fiber stalls forever waiting for a rendezvous that never
 *       arrives.
 *   (2) Scheduler wakeup bugs (#4 fixed-neighbor wakeup, #7 idle steal gap):
 *       partner fibers are never scheduled because the workers that hold
 *       their entries are not notified.  P6.18 and P6.21 isolate those bugs
 *       in isolation; this test intentionally exercises both simultaneously
 *       with a high worker count (P6_22_WORKERS=8) to maximise concurrency
 *       and will also fail if either scheduler issue causes a stall.
 */
#define P6_22_WORKERS 8
#define P6_22_PAIRS   20
#define P6_22_ROUNDS  300

typedef struct {
    goc_chan*    req;
    goc_chan*    ack;
    _Atomic int* finished;
    _Atomic int  sender_round;
    _Atomic int  receiver_round;
    int          expected;
    done_t*      done;
} p6_22_pair_t;

static bool g_skip_shutdown = false;

static void p6_22_sender_fn(void* arg) {
    p6_22_pair_t* p = (p6_22_pair_t*)arg;
    for (int i = 0; i < P6_22_ROUNDS; i++) {
        /* slow-path put: parks if no receiver is waiting */
        goc_put(p->req, goc_box_uint(i + 1));
        /* slow-path take at the SAME call depth on every iteration;
         * re-uses the same stack slot for the parking goc_entry each time */
        goc_take(p->ack);
        atomic_store_explicit(&p->sender_round, i + 1, memory_order_release);
    }
    if (atomic_fetch_add_explicit(p->finished, 1, memory_order_acq_rel) + 1
            == p->expected)
        done_signal(p->done);
}

static void p6_22_receiver_fn(void* arg) {
    p6_22_pair_t* p = (p6_22_pair_t*)arg;
    for (int i = 0; i < P6_22_ROUNDS; i++) {
        /* slow-path take: same re-use pattern as the sender */
        goc_take(p->req);
        goc_put(p->ack, NULL);
        atomic_store_explicit(&p->receiver_round, i + 1, memory_order_release);
    }
    if (atomic_fetch_add_explicit(p->finished, 1, memory_order_acq_rel) + 1
            == p->expected)
        done_signal(p->done);
}

static void test_p6_22(void) {
    TEST_BEGIN("P6.22  double-resume/stack-address reuse: ping-pong completes without hang");

    done_t done;
    done_init(&done);
    _Atomic int finished = 0;
    bool ok = false;

    goc_pool* pool = goc_pool_make(P6_22_WORKERS);
    ASSERT(pool != NULL);

    p6_22_pair_t pairs[P6_22_PAIRS] = { 0 };
    for (int i = 0; i < P6_22_PAIRS; i++) {
        pairs[i].req      = goc_chan_make(0);   /* unbuffered: always slow path */
        pairs[i].ack      = goc_chan_make(0);
        pairs[i].finished = &finished;
        pairs[i].expected = P6_22_PAIRS * 2;   /* both sender and receiver count */
        pairs[i].done     = &done;
        ASSERT(pairs[i].req != NULL);
        ASSERT(pairs[i].ack != NULL);
    }

    for (int i = 0; i < P6_22_PAIRS; i++) {
        goc_go_on(pool, p6_22_sender_fn,   &pairs[i]);
        goc_go_on(pool, p6_22_receiver_fn, &pairs[i]);
    }

    /* Generous deadline: only a double-resume (hang) causes a timeout.
     * A double-resume crash would appear as SIGABRT / signal 11 here. */
    ok = done_wait_timeout(&done, 15000);
    ASSERT(ok);
    TEST_PASS();
done:;

    if (!ok) {
        printf("    progress: finished=%d/%d\n",
               atomic_load_explicit(&finished, memory_order_acquire),
               P6_22_PAIRS * 2);
        for (int i = 0; i < P6_22_PAIRS; i++) {
            int sr = atomic_load_explicit(&pairs[i].sender_round, memory_order_acquire);
            int rr = atomic_load_explicit(&pairs[i].receiver_round, memory_order_acquire);
            if (sr != P6_22_ROUNDS || rr != P6_22_ROUNDS) {
                printf("    pair[%d]: sender=%d receiver=%d\n", i, sr, rr);
            }
        }
    }

    for (int i = 0; i < P6_22_PAIRS; i++) {
        if (pairs[i].req != NULL)
            goc_close(pairs[i].req);
        if (pairs[i].ack != NULL)
            goc_close(pairs[i].ack);
    }

    if (pool != NULL) {
        if (ok) {
            goc_pool_destroy(pool);
        } else if (goc_pool_destroy_timeout(pool, 100) == GOC_DRAIN_TIMEOUT) {
            g_skip_shutdown = true;
        }
    }

    done_destroy(&done);
}

/* =========================================================================
 * P6.24 / P6.25 — steal telemetry tests (GOC_ENABLE_STATS only)
 * ====================================================================== */

#ifdef GOC_ENABLE_STATS

/* Minimal local event buffer for P6.24 / P6.25.
 * We install a temporary callback, run the pool, then restore the old one. */
#define P6_STEAL_EV_CAP 1024

typedef struct {
    struct goc_stats_event buf[P6_STEAL_EV_CAP];
    size_t                 count;
    uv_mutex_t             mtx;
} p6_steal_evbuf_t;

static void p6_steal_collect(const struct goc_stats_event* ev, void* ud) {
    p6_steal_evbuf_t* b = (p6_steal_evbuf_t*)ud;
    uv_mutex_lock(&b->mtx);
    if (b->count < P6_STEAL_EV_CAP)
        b->buf[b->count++] = *ev;
    uv_mutex_unlock(&b->mtx);
}

/* Fiber that does nothing; used to create work for stealing. */
static void p6_steal_noop(void* arg) { (void)arg; }

/*
 * P6.24 — steal fields on STOPPED events are valid.
 *
 * After goc_pool_destroy() each STOPPED event for the pool must satisfy
 * steal_successes <= steal_attempts.
 */
static void test_p6_24(void) {
    TEST_BEGIN("P6.24  steal fields on STOPPED events are valid");

    p6_steal_evbuf_t evbuf = { .count = 0 };
    uv_mutex_init(&evbuf.mtx);

    goc_stats_init();
    goc_stats_set_callback(p6_steal_collect, &evbuf);

    goc_pool* pool = goc_pool_make(2);
    ASSERT(pool != NULL);
    void* pool_id = pool;

    /* Burst of short fibers to provoke work stealing. */
    for (int i = 0; i < 64; i++)
        goc_go_on(pool, p6_steal_noop, NULL);

    goc_pool_destroy(pool);
    goc_stats_flush();
    goc_stats_shutdown();

    int stopped_found = 0;
    uv_mutex_lock(&evbuf.mtx);
    for (size_t i = 0; i < evbuf.count; i++) {
        const struct goc_stats_event* ev = &evbuf.buf[i];
        if (ev->type != GOC_STATS_EVENT_WORKER_STATUS) continue;
        if (ev->data.worker.status != GOC_WORKER_STOPPED) continue;
        if (ev->data.worker.pool_id != pool_id) continue;
        ASSERT(ev->data.worker.steal_successes <= ev->data.worker.steal_attempts);
        stopped_found++;
    }
    uv_mutex_unlock(&evbuf.mtx);

    ASSERT(stopped_found >= 2);

    TEST_PASS();
done:
    uv_mutex_destroy(&evbuf.mtx);
}

/*
 * P6.25 — goc_pool_get_steal_stats is consistent with per-worker STOPPED totals.
 *
 * The sum of steal_attempts across all STOPPED events for the pool's workers
 * must equal the delta reported by goc_pool_get_steal_stats before and after
 * that pool's lifetime.
 */
static void test_p6_25(void) {
    TEST_BEGIN("P6.25  goc_pool_get_steal_stats consistent with STOPPED event totals");

    p6_steal_evbuf_t evbuf = { .count = 0 };
    uv_mutex_init(&evbuf.mtx);

    goc_stats_init();
    goc_stats_set_callback(p6_steal_collect, &evbuf);

    uint64_t att0 = 0, suc0 = 0;
    goc_pool_get_steal_stats(&att0, &suc0);

    goc_pool* pool = goc_pool_make(2);
    ASSERT(pool != NULL);
    void* pool_id = pool;

    for (int i = 0; i < 64; i++)
        goc_go_on(pool, p6_steal_noop, NULL);

    goc_pool_destroy(pool);
    goc_stats_flush();

    uint64_t att1 = 0, suc1 = 0;
    goc_pool_get_steal_stats(&att1, &suc1);

    goc_stats_shutdown();

    /* Sum steal_attempts from STOPPED events belonging to this pool. */
    uint64_t ev_att_sum = 0;
    uv_mutex_lock(&evbuf.mtx);
    for (size_t i = 0; i < evbuf.count; i++) {
        const struct goc_stats_event* ev = &evbuf.buf[i];
        if (ev->type != GOC_STATS_EVENT_WORKER_STATUS) continue;
        if (ev->data.worker.status != GOC_WORKER_STOPPED) continue;
        if (ev->data.worker.pool_id != pool_id) continue;
        ev_att_sum += ev->data.worker.steal_attempts;
    }
    uv_mutex_unlock(&evbuf.mtx);

    ASSERT((att1 - att0) == ev_att_sum);

    TEST_PASS();
done:
    uv_mutex_destroy(&evbuf.mtx);
}

#endif /* GOC_ENABLE_STATS */

/* ---- P6.23: GC bitmap write-ordering: slot data visible before bit ---- */
/*
 * Bug being tested (gc.c goc_fiber_root_register):
 *   Before the fix, the fiber root bitmap bit was set FIRST (under the mutex),
 *   then slot data (entry pointer, stack_top, scan_from) was written AFTER.
 *   push_fiber_roots runs without the mutex during BDW-GC stop-the-world.
 *   If GC stopped the world between the bit-set and the data-write,
 *   push_fiber_roots would see the bit set and read STALE slot data from the
 *   previous occupant of that slot — a fiber whose mco_coro stack was already
 *   freed by mco_destroy.  GC_push_all_eager on freed memory corrupts the GC
 *   heap, causing later GC operations or allocations to SIGABRT.
 *
 * Fix: write slot data first, then atomic_thread_fence(release), then set bit.
 *   Any observer that sees the bit set is guaranteed to see correct slot data.
 *
 * Test strategy:
 *   Spawn N_WAVES waves of N_PER_WAVE short-lived fibers.  Each fiber calls
 *   GC_malloc to allocate a small object (forcing BDW-GC to run collection
 *   cycles frequently).  The fibers complete immediately, causing rapid slot
 *   reuse in the bitmap.  The combination of high slot-reuse rate and frequent
 *   GC cycles maximises the probability of hitting the write-ordering window.
 *   Without the fix, this reliably produces a SIGABRT from GC heap corruption
 *   (observed via the crash handler installed in main()).
 *   With the fix, all fibers complete and the test passes.
 */
#define P6_23_WORKERS   4
#define P6_23_WAVES     20
#define P6_23_PER_WAVE  50

typedef struct {
    done_t*      done;
    _Atomic int* completed;
    int          total;
} p6_23_arg_t;

static void p6_23_fiber_fn(void* arg) {
    p6_23_arg_t* a = (p6_23_arg_t*)arg;
    /* Allocate on the GC heap to pressure BDW-GC into running collections.
     * The allocation is intentionally kept alive briefly via a volatile read
     * so the compiler cannot elide it. */
    volatile char* gc_buf = (volatile char*)goc_malloc(256);
    (void)*gc_buf;
    if (atomic_fetch_add_explicit(a->completed, 1, memory_order_acq_rel) + 1
            == a->total)
        done_signal(a->done);
}

static void test_p6_23(void) {
    TEST_BEGIN("P6.23  GC bitmap write-ordering: slot reuse under concurrent GC");

    done_t done;
    done_init(&done);

    goc_pool* pool = goc_pool_make(P6_23_WORKERS);
    ASSERT(pool != NULL);

    _Atomic int completed = 0;
    p6_23_arg_t args = { &done, &completed, P6_23_PER_WAVE };

    for (int w = 0; w < P6_23_WAVES; w++) {
        atomic_store_explicit(&completed, 0, memory_order_relaxed);

        for (int i = 0; i < P6_23_PER_WAVE; i++)
            goc_go_on(pool, p6_23_fiber_fn, &args);

        bool ok = done_wait_timeout(&done, 3000);
        ASSERT(ok);
    }

    goc_pool_destroy(pool);
    done_destroy(&done);
    TEST_PASS();
done:;
}

/* =========================================================================
 * main
 *
 * Initialises the runtime once, runs all Phase 6 tests in order, shuts down
 * the runtime, then prints a summary and exits with 0 on success or 1 if any
 * test failed.
 * ====================================================================== */

int main(void) {
    install_crash_handler();

    printf("libgoc test suite — Phase 6: Thread pool\n");
    printf("==========================================\n\n");

    goc_init();

    printf("Phase 6 — Thread pool\n");
    test_p6_1();
    test_p6_2();
    test_p6_3();
    test_p6_4();
    test_p6_5();
    printf("\n");

    printf("Phase 6 (continued) — Work-stealing deque + injector\n");
    test_p6_6();
    test_p6_7();
    test_p6_8();
    test_p6_9();
    test_p6_10();
    test_p6_11();
    test_p6_12();
    test_p6_13();
    test_p6_14();
    test_p6_15();
    test_p6_16();
    test_p6_17();
    test_p6_18();
    test_p6_19();
    test_p6_20();
    test_p6_21();
    test_p6_22();
    test_p6_23();
#ifdef GOC_ENABLE_STATS
    test_p6_24();
    test_p6_25();
#endif
    printf("\n");

    if (!g_skip_shutdown) {
        goc_shutdown();
    } else {
        fprintf(stderr,
                "skipping goc_shutdown: P6.22 left an undrained pool after failure\n");
    }

    printf("==========================================\n");
    printf("Results: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed > 0) {
        printf(", %d FAILED", g_tests_failed);
    }
    printf("\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
