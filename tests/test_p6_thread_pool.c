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
 *   - pthreads (mutex + condvar for done_t) — see below
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
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "test_harness.h"
#include "goc.h"
#include "wsdq.h"

/* =========================================================================
 * done_t — lightweight fiber-to-main synchronisation via mutex + condvar
 *
 * Used throughout Phase 6 to let the main thread block until a target fiber
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
                                   (void*)(uintptr_t)0xABCD);
    ASSERT(st == GOC_OK);

    goc_chan* join = goc_go(test_p6_5_fiber_fn, &args);
    ASSERT(join != NULL);

    done_wait(&done);

    ASSERT(args.result->ch == args.channels[args.winner_idx]);
    ASSERT(args.result->value.ok  == GOC_OK);
    ASSERT((uintptr_t)args.result->value.val == 0xABCD);

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
    pthread_mutex_t* mu;
    _Atomic bool*  done;
} p6_11_thief_arg;

static void* p6_11_thief(void* arg) {
    p6_11_thief_arg* a = (p6_11_thief_arg*)arg;
    while (true) {
        goc_entry* e = wsdq_steal_top(a->dq);
        if (e != NULL) {
            pthread_mutex_lock(a->mu);
            a->seen[e->arm_idx] = true;
            (*a->total)++;
            pthread_mutex_unlock(a->mu);
            free(e);
        } else if (atomic_load_explicit(a->done, memory_order_acquire)) {
            /* drain loop */
            while ((e = wsdq_steal_top(a->dq)) != NULL) {
                pthread_mutex_lock(a->mu);
                a->seen[e->arm_idx] = true;
                (*a->total)++;
                pthread_mutex_unlock(a->mu);
                free(e);
            }
            break;
        }
    }
    return NULL;
}

static void test_p6_11(void) {
    TEST_BEGIN("P6.11  wsdq concurrent owner-push / thief-steal");

    goc_wsdq dq;
    wsdq_init(&dq, 256);

    bool* seen = calloc(P6_N, sizeof(bool));
    int total = 0;
    pthread_mutex_t mu;
    pthread_mutex_init(&mu, NULL);
    _Atomic bool done = false;

    p6_11_thief_arg arg = { &dq, seen, &total, &mu, &done };

    pthread_t thieves[P6_T];
    for (int t = 0; t < P6_T; t++)
        pthread_create(&thieves[t], NULL, p6_11_thief, &arg);

    for (int i = 0; i < P6_N; i++)
        wsdq_push_bottom(&dq, make_entry(i));

    atomic_store_explicit(&done, true, memory_order_release);

    for (int t = 0; t < P6_T; t++)
        pthread_join(thieves[t], NULL);

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

    pthread_mutex_destroy(&mu);
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
    pthread_mutex_t* mu;
    _Atomic bool*   done;
} p6_12_thief_arg;

static void* p6_12_thief(void* arg) {
    p6_12_thief_arg* a = (p6_12_thief_arg*)arg;
    goc_entry* e;
    while (true) {
        e = wsdq_steal_top(a->dq);
        if (e != NULL) {
            pthread_mutex_lock(a->mu);
            a->seen[e->arm_idx] = true;
            (*a->total)++;
            pthread_mutex_unlock(a->mu);
            free(e);
        } else if (atomic_load_explicit(a->done, memory_order_acquire)) {
            while ((e = wsdq_steal_top(a->dq)) != NULL) {
                pthread_mutex_lock(a->mu);
                a->seen[e->arm_idx] = true;
                (*a->total)++;
                pthread_mutex_unlock(a->mu);
                free(e);
            }
            break;
        }
    }
    return NULL;
}

static void test_p6_12(void) {
    TEST_BEGIN("P6.12  wsdq concurrent push+pop+steal under contention");

    goc_wsdq dq;
    wsdq_init(&dq, 256);

    bool* seen = calloc(P6_N, sizeof(bool));
    int total = 0;
    pthread_mutex_t mu;
    pthread_mutex_init(&mu, NULL);
    _Atomic bool done = false;

    p6_12_thief_arg arg = { &dq, seen, &total, &mu, &done };

    pthread_t thieves[P6_T];
    for (int t = 0; t < P6_T; t++)
        pthread_create(&thieves[t], NULL, p6_12_thief, &arg);

    /* Owner: push N entries, immediately try to pop each one back */
    for (int i = 0; i < P6_N; i++) {
        wsdq_push_bottom(&dq, make_entry(i));
        goc_entry* e = wsdq_pop_bottom(&dq);
        if (e != NULL) {
            pthread_mutex_lock(&mu);
            seen[e->arm_idx] = true;
            total++;
            pthread_mutex_unlock(&mu);
            free(e);
        }
    }

    atomic_store_explicit(&done, true, memory_order_release);

    for (int t = 0; t < P6_T; t++)
        pthread_join(thieves[t], NULL);

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

    pthread_mutex_destroy(&mu);
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

static void* p6_16_producer(void* arg) {
    p6_16_prod_arg* a = (p6_16_prod_arg*)arg;
    for (int i = 0; i < a->count; i++)
        injector_push(a->inj, make_entry(a->start + i));
    return NULL;
}

static void test_p6_16(void) {
    TEST_BEGIN("P6.16 injector concurrent push from multiple threads");

    goc_injector inj;
    injector_init(&inj);

    int per_thread = P6_N / P6_T;
    p6_16_prod_arg args[P6_T];
    pthread_t prods[P6_T];
    for (int t = 0; t < P6_T; t++) {
        args[t].inj   = &inj;
        args[t].start = t * per_thread;
        args[t].count = per_thread;
        pthread_create(&prods[t], NULL, p6_16_producer, &args[t]);
    }
    for (int t = 0; t < P6_T; t++)
        pthread_join(prods[t], NULL);

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
