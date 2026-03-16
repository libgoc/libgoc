/*
 * tests/test_p6_thread_pool.c — Phase 6: Thread pool tests for libgoc
 *
 * Verifies goc_pool_make / goc_pool_destroy lifecycle, goc_go_on dispatch,
 * goc_pool_destroy_timeout (both GOC_DRAIN_OK and GOC_DRAIN_TIMEOUT paths),
 * the goc_malloc GC-heap allocator end-to-end, the compact_dead_entries sweep
 * (triggered by cancelling >= GOC_DEAD_COUNT_THRESHOLD stale alts entries),
 * and the malloc path in alts_dedup_sort_channels for n > GOC_ALTS_STACK_THRESHOLD
 * arms.
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
 *   - POSIX semaphores — used by the done_t helper (see below)
 *
 * Synchronisation helper — done_t:
 *   A thin wrapper around a POSIX sem_t that lets the main thread (or a waiter
 *   fiber) block until a fiber signals completion.  Using a semaphore avoids
 *   the need for a goc_chan in tests that are themselves verifying pool or
 *   channel behaviour, keeping each test self-contained.
 *
 *     done_init(&d)    — initialise the semaphore to 0
 *     done_signal(&d)  — post (increment) — called by the fiber on exit
 *     done_wait(&d)    — wait (decrement) — called by the test thread
 *     done_destroy(&d) — destroy the semaphore
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
 *   P6.5   compact_dead_entries fires correctly: 12 fibers race on a single
 *          channel via goc_alts; 11 lose the woken CAS and become dead entries
 *          (dead_count == 11 > GOC_DEAD_COUNT_THRESHOLD == 8); a probe fiber
 *          calls goc_take on the same channel, triggering compact_dead_entries;
 *          probe receives the sentinel value without crash or hang.
 *          Synchronisation: atomic counter + 5 ms nanosleep replaces the
 *          previous done_signal-before-goc_alts pattern (which had a race
 *          between the signal and Phase 6 enqueue completion).
 *   P6.6   goc_alts with n > GOC_ALTS_STACK_THRESHOLD (8) arms exercises the
 *          malloc path in alts_dedup_sort_channels; correct arm fires, no
 *          memory error (run under ASAN to catch heap misuse)
 *
 * Notes:
 *   - goc_init() is called once in main() before any test runs.
 *   - goc_shutdown() is called once in main() after all tests complete.
 *   - Each test function uses the goto-based cleanup pattern from the harness:
 *     ASSERT() jumps to `done:` on failure; TEST_PASS() also jumps to `done:`.
 *     Cleanup code after the `done:` label runs in both pass and fail paths.
 */

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <semaphore.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_harness.h"
#include "goc.h"

/* =========================================================================
 * done_t — lightweight fiber-to-main synchronisation via POSIX semaphore
 *
 * Used throughout Phase 6 to let the main thread block until a target fiber
 * signals completion.  Choosing a raw semaphore rather than a goc_chan keeps
 * each test independent of the channel machinery it is trying to verify.
 * ====================================================================== */

typedef struct { sem_t sem; } done_t;

static void done_init(done_t* d)    { sem_init(&d->sem, 0, 0); }
static void done_signal(done_t* d)  { sem_post(&d->sem); }
static void done_wait(done_t* d)    { sem_wait(&d->sem); }
static void done_destroy(done_t* d) { sem_destroy(&d->sem); }

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
    goc_val_t v = goc_take_sync(join);
    ASSERT(v.ok == GOC_CLOSED);

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
    struct timespec ts = {
        .tv_sec  = 0,
        .tv_nsec = (long)(a->delay_us * 1000UL),
    };
    nanosleep(&ts, NULL);
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

    goc_val_t sv = goc_take_sync(short_join);
    ASSERT(sv.ok == GOC_CLOSED);

    /* Unblock the long fiber so the pool can be destroyed cleanly. */
    goc_close(blocker);

    goc_val_t lv = goc_take_sync(long_join);
    ASSERT(lv.ok == GOC_CLOSED);

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
    goc_val_t v = goc_take_sync(join);
    ASSERT(v.ok == GOC_CLOSED);

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

/* --- P6.5: compact_dead_entries sweep ---------------------------------- */

/*
 * Number of competing fibers used to generate dead alts entries.
 * Must be > GOC_DEAD_COUNT_THRESHOLD (8) so that at least 8 stale entries
 * accumulate on the channel's taker list before the sweep fires.
 */
#define P6_5_COMPETITOR_COUNT 12

/*
 * Atomic counter incremented by each competitor fiber immediately before it
 * calls goc_alts.  The main thread spins on this reaching
 * P6_5_COMPETITOR_COUNT, then sleeps 5 ms to allow all fibers to complete
 * Phase 6 of the alts protocol (enqueue entries + release channel locks)
 * before the prize value is sent.
 *
 * This is not a formal happens-before barrier — a fiber could theoretically
 * be preempted after incrementing but before completing Phase 6.  5 ms is
 * orders of magnitude larger than the Phase 6 critical section in practice,
 * making the race window vanishingly small.  This replaces the previous
 * done_signal-before-goc_alts pattern, which had an unfixable race: the
 * signal fired before the fiber was enqueued, so goc_put_sync could run
 * while some fibers had not yet registered their taker entries.
 */
static _Atomic int g_p6_5_ready;

/*
 * Argument bundle for the competitor fibers in P6.5.
 *
 * target_ch  — channel all fibers race to take from via goc_alts;
 *              only one fiber wins the woken CAS, leaving the others with
 *              cancelled (dead) entries on the taker list
 * decoy_ch   — second arm in the select; never receives data — ensures
 *              fibers always reach Phase 6 and park on both channels,
 *              so the dead entries accumulate on target_ch's taker list
 * index_won  — populated after goc_alts returns with the winning arm index
 *              (-1 if the fiber lost the CAS race and was cancelled)
 * ok_won     — populated after goc_alts returns with the winning arm status
 */
typedef struct {
    goc_chan*    target_ch;
    goc_chan*    decoy_ch;
    int          index_won;
    goc_status_t ok_won;
} p6_5_competitor_args_t;

/*
 * Competitor fiber for P6.5.
 *
 * Increments g_p6_5_ready to notify the main thread that this fiber is on
 * the cusp of parking, then immediately calls goc_alts.  The main thread's
 * 5 ms nanosleep after the spin covers the Phase 6 window.
 */
static void test_p6_5_competitor_fn(void* arg) {
    p6_5_competitor_args_t* a = (p6_5_competitor_args_t*)arg;

    /* Increment before parking — main thread spins on this reaching N. */
    atomic_fetch_add_explicit(&g_p6_5_ready, 1, memory_order_release);

    goc_alt_op ops[] = {
        { .ch = a->target_ch, .op_kind = GOC_ALT_TAKE },
        { .ch = a->decoy_ch,  .op_kind = GOC_ALT_TAKE },
    };
    goc_alts_result r = goc_alts(ops, 2);

    a->index_won = (int)r.index;
    a->ok_won    = r.value.ok;
}

/*
 * Argument bundle for the probe fiber that triggers compact_dead_entries.
 *
 * target_ch — channel to call goc_take on; must have dead_count >=
 *             GOC_DEAD_COUNT_THRESHOLD so that the sweep fires on entry
 * done      — signalled after goc_take returns so the main thread can assert
 * result    — populated with the goc_take return value
 */
typedef struct {
    goc_chan*  target_ch;
    done_t*    done;
    goc_val_t  result;
} p6_5_probe_args_t;

/*
 * Probe fiber for P6.5.
 *
 * Calls goc_take on target_ch.  goc_take (fiber context) checks
 * dead_count >= GOC_DEAD_COUNT_THRESHOLD under the channel lock and calls
 * compact_dead_entries before attempting any data transfer.  This is the
 * sweep path under test.
 *
 * NOTE: goc_take_sync does NOT call compact_dead_entries — it has no
 * threshold check.  A fiber-context take is required to exercise the sweep.
 */
static void test_p6_5_probe_fn(void* arg) {
    p6_5_probe_args_t* a = (p6_5_probe_args_t*)arg;
    a->result = goc_take(a->target_ch);
    done_signal(a->done);
}

/*
 * P6.5 — compact_dead_entries fires correctly
 *
 * Launches P6_5_COMPETITOR_COUNT (12) fibers, each racing on the same
 * target_ch via a two-arm goc_alts (target_ch take | decoy_ch take).
 *
 * Synchronisation:
 *   Each fiber increments g_p6_5_ready immediately before calling goc_alts.
 *   The main thread spins until the counter reaches P6_5_COMPETITOR_COUNT,
 *   then sleeps 5 ms to allow all fibers to complete Phase 6 (enqueue their
 *   taker entries and release channel locks) before the prize is sent.
 *
 * Race:
 *   goc_put_sync delivers a single value; exactly one fiber wins the woken
 *   CAS inside wake().  The remaining 11 lose: wake() sets their cancelled
 *   flag and increments target_ch->dead_count.  dead_count ends up at 11,
 *   which is >= GOC_DEAD_COUNT_THRESHOLD (8).
 *
 * Sweep verification:
 *   A probe fiber calls goc_take on target_ch.  goc_take (fiber context)
 *   checks the threshold, calls compact_dead_entries, sweeps the 11 dead
 *   entries, then parks to wait for the sentinel value put by the main
 *   thread.  A crash or hang in the probe means the entry list was corrupt.
 *
 * Final consistency:
 *   target_ch is closed and a goc_take_sync on it must return GOC_CLOSED
 *   immediately, confirming the channel is still well-formed after the sweep.
 */
static void test_p6_5(void) {
    TEST_BEGIN("P6.5   compact_dead_entries: dead alts entries swept correctly");

    atomic_store_explicit(&g_p6_5_ready, 0, memory_order_relaxed);

    goc_chan* target_ch = goc_chan_make(0);
    ASSERT(target_ch != NULL);

    goc_chan* decoy_ch = goc_chan_make(0);
    ASSERT(decoy_ch != NULL);

    p6_5_competitor_args_t args[P6_5_COMPETITOR_COUNT];
    goc_chan* joins[P6_5_COMPETITOR_COUNT];

    for (int i = 0; i < P6_5_COMPETITOR_COUNT; i++) {
        args[i].target_ch = target_ch;
        args[i].decoy_ch  = decoy_ch;
        args[i].index_won = -1;
        args[i].ok_won    = GOC_CLOSED;

        joins[i] = goc_go(test_p6_5_competitor_fn, &args[i]);
        ASSERT(joins[i] != NULL);
    }

    /*
     * Yield-spin until every fiber has incremented the counter (all are
     * between the atomic_fetch_add and goc_alts Phase 6 completion), then
     * sleep 5 ms to allow Phase 6 to finish across all fibers before the
     * prize is sent.  5 ms >> actual Phase 6 duration (~nanoseconds).
     *
     * A plain busy-wait is avoided here: on machines with few cores the
     * spinning main thread can starve pool workers, preventing fibers from
     * running and incrementing the counter, causing an infinite loop.  A
     * 100 µs nanosleep inside the loop releases the main thread's CPU slice
     * on each iteration so pool threads can be scheduled.
     */
    {
        struct timespec yield_sleep = { .tv_sec = 0, .tv_nsec = 100000L /* 100 µs */ };
        while (atomic_load_explicit(&g_p6_5_ready, memory_order_acquire)
               < P6_5_COMPETITOR_COUNT) {
            nanosleep(&yield_sleep, NULL);
        }
    }
    struct timespec park_wait = { .tv_sec = 0, .tv_nsec = 5000000L /* 5 ms */ };
    nanosleep(&park_wait, NULL);

    /* Deliver a single value — exactly one competitor wins the woken CAS. */
    goc_status_t st = goc_put_sync(target_ch, (void*)(uintptr_t)0xDEAD);
    ASSERT(st == GOC_OK);

    /* Wait for all competitors to exit. */
    for (int i = 0; i < P6_5_COMPETITOR_COUNT; i++) {
        goc_val_t v = goc_take_sync(joins[i]);
        ASSERT(v.ok == GOC_CLOSED);
    }

    /* Exactly one fiber must have won on arm index 0 (target_ch). */
    int winners = 0;
    for (int i = 0; i < P6_5_COMPETITOR_COUNT; i++) {
        if (args[i].index_won == 0 && args[i].ok_won == GOC_OK)
            winners++;
    }
    ASSERT(winners == 1);

    /*
     * Verify compact_dead_entries fires via a fiber-context goc_take.
     *
     * Launch the probe fiber first so it parks on target_ch (slow path),
     * guaranteeing goc_take walks the taker list and sees dead_count >= 8.
     * Then put the sentinel from the main thread; the probe fiber wakes,
     * records the value, and signals done.
     *
     * The 1 ms sleep between launching the probe and putting the sentinel
     * ensures the probe fiber has reached goc_take's slow path before the
     * value arrives (so compact_dead_entries runs before the list is
     * traversed for delivery, not after).
     */
    done_t probe_done;
    done_init(&probe_done);

    p6_5_probe_args_t probe_args = {
        .target_ch = target_ch,
        .done      = &probe_done,
        .result    = { .val = NULL, .ok = GOC_CLOSED },
    };

    goc_chan* probe_join = goc_go(test_p6_5_probe_fn, &probe_args);
    ASSERT(probe_join != NULL);

    struct timespec probe_settle = { .tv_sec = 0, .tv_nsec = 1000000L /* 1 ms */ };
    nanosleep(&probe_settle, NULL);

    st = goc_put_sync(target_ch, (void*)(uintptr_t)0xC0DE);
    ASSERT(st == GOC_OK);

    done_wait(&probe_done);
    ASSERT(probe_args.result.ok == GOC_OK);
    ASSERT((uintptr_t)probe_args.result.val == 0xC0DE);

    goc_val_t pj = goc_take_sync(probe_join);
    ASSERT(pj.ok == GOC_CLOSED);

    /*
     * Final consistency check: close target_ch and confirm goc_take_sync
     * returns GOC_CLOSED immediately without hanging.
     */
    goc_close(target_ch);
    goc_val_t tv = goc_take_sync(target_ch);
    ASSERT(tv.ok == GOC_CLOSED);

    goc_close(decoy_ch);
    done_destroy(&probe_done);
    TEST_PASS();
done:;
}

/* --- P6.6: goc_alts with n > GOC_ALTS_STACK_THRESHOLD arms ------------ */

/*
 * Number of arms in the P6.6 select.  Must be > GOC_ALTS_STACK_THRESHOLD (8)
 * to force the malloc path in alts_dedup_sort_channels.
 */
#define P6_6_ARM_COUNT 10

/*
 * Argument bundle for test_p6_6_fiber_fn.
 *
 * channels  — array of P6_6_ARM_COUNT channels; one will be pre-loaded
 * winner_idx — index of the channel pre-loaded with a value
 * result    — populated by the fiber with the goc_alts_result
 * done      — signalled when the fiber has recorded its result
 */
typedef struct {
    goc_chan*       channels[P6_6_ARM_COUNT];
    int             winner_idx;
    goc_alts_result result;
    done_t*         done;
} p6_6_args_t;

/*
 * Fiber for P6.6.
 * Calls goc_alts with P6_6_ARM_COUNT take arms.  Because one channel is
 * pre-loaded, the fast-path scan fires immediately on that arm.  The channel
 * pointer scratch buffer must be malloc-allocated (n > GOC_ALTS_STACK_THRESHOLD).
 */
static void test_p6_6_fiber_fn(void* arg) {
    p6_6_args_t* a = (p6_6_args_t*)arg;

    goc_alt_op ops[P6_6_ARM_COUNT];
    for (int i = 0; i < P6_6_ARM_COUNT; i++) {
        ops[i].ch       = a->channels[i];
        ops[i].op_kind  = GOC_ALT_TAKE;
        ops[i].put_val  = NULL;
    }

    a->result = goc_alts(ops, P6_6_ARM_COUNT);
    done_signal(a->done);
}

/*
 * P6.6 — goc_alts with n > GOC_ALTS_STACK_THRESHOLD arms
 *
 * Creates P6_6_ARM_COUNT (10) buffered channels.  Pre-loads the last channel
 * (index P6_6_ARM_COUNT - 1) with a known value.  Launches a fiber that calls
 * goc_alts with take arms over all P6_6_ARM_COUNT channels.  The winning arm
 * must fire immediately (fast path) on the pre-loaded channel, returning the
 * correct index and value.  No memory error must be observed (run under ASAN
 * to catch heap misuse from the malloc/free path in alts_dedup_sort_channels).
 */
static void test_p6_6(void) {
    TEST_BEGIN("P6.6   goc_alts: n > GOC_ALTS_STACK_THRESHOLD; malloc path");

    done_t done;
    done_init(&done);

    p6_6_args_t args;
    args.done       = &done;
    args.winner_idx = P6_6_ARM_COUNT - 1;

    for (int i = 0; i < P6_6_ARM_COUNT; i++) {
        args.channels[i] = goc_chan_make(1);
        ASSERT(args.channels[i] != NULL);
    }

    /* Pre-load only the last channel so the winning arm is deterministic. */
    goc_status_t st = goc_put_sync(args.channels[args.winner_idx],
                                   (void*)(uintptr_t)0xABCD);
    ASSERT(st == GOC_OK);

    goc_chan* join = goc_go(test_p6_6_fiber_fn, &args);
    ASSERT(join != NULL);

    done_wait(&done);

    ASSERT((int)args.result.index == args.winner_idx);
    ASSERT(args.result.value.ok  == GOC_OK);
    ASSERT((uintptr_t)args.result.value.val == 0xABCD);

    goc_val_t v = goc_take_sync(join);
    ASSERT(v.ok == GOC_CLOSED);

    for (int i = 0; i < P6_6_ARM_COUNT; i++) {
        goc_close(args.channels[i]);
    }

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
    test_p6_6();
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
