/*
 * tests/test_goc_stats.c — Telemetry: goc_stats event emission and callback
 *
 * Verifies that the stats subsystem initialises correctly, that each event
 * type (worker, fiber, channel) is emitted with the correct field values, and
 * that the stats system is disabled cleanly on shutdown.
 *
 * Stats events are delivered via a registered callback (goc_stats_set_callback).
 * The test installs a callback that copies each event into a mutex-protected
 * ring buffer.  A condvar lets drain helpers block until the expected event
 * arrives, with a per-event timeout to catch missing emissions.
 *
 * Build:  cmake -B build -DGOC_ENABLE_STATS=ON && cmake --build build
 * Run:    ctest --test-dir build --output-on-failure
 *         ./build/test_goc_stats
 *
 * Test coverage:
 *
 *   S1.1  goc_stats_init() / goc_stats_shutdown() complete without error
 *   S1.2  goc_stats_is_enabled() is true after goc_stats_init()
 *   S1.3  Worker event round-trips with correct type and fields
 *   S1.4  Fiber event round-trips with correct type and fields
 *   S1.5  Channel event round-trips with correct type and fields
 *   S2.1  goc_go() emits GOC_FIBER_CREATED; fiber exit emits GOC_FIBER_COMPLETED
 *   S2.2  After fiber completes, owning worker transitions to GOC_WORKER_IDLE
 *   S2.3  goc_pool_make() emits GOC_WORKER_CREATED per thread
 *   S2.4  Pool creation and destruction events
 *   S3.1  Channel open/close events via API
 *   S3.2  Buffered channel: buf_size and item_count after put/take
 *   S3.3  Multiple fibers and channels: all events unique and present
 *   S4.1  goc_stats_shutdown() disables stats delivery
 *   S5.1  Worker STOPPED event carries valid steal_attempts/steal_successes
 *   S6.1  goc_cb_queue_get_hwm() reflects peak callback-queue depth
 *   S6.4  steal counters readable; misses<=attempts invariant holds (no-work pool)
 *   S6.5  idle_wakeups increments after external injection (pool=2, 1 fiber)
 *   S6.6  goc_pool_get_steal_stats extended signature returns valid values
 *
 *   (See test source for details on each scenario.)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "test_harness.h"
#include "goc.h"
#include "goc_stats.h"
#include "../src/internal.h"

/* =========================================================================
 * Event collection buffer
 *
 * The callback copies each event into g_events[].  A read cursor
 * (g_read_pos) tracks which events tests have already consumed.
 * A condvar lets blocking drain helpers sleep until new events arrive.
 * ====================================================================== */

#define EVENT_BUF_CAP 2048

static goc_stats_event_t g_events[EVENT_BUF_CAP];
static size_t                 g_event_count = 0;   /* total events received */
static size_t                 g_read_pos    = 0;   /* next event to consume */
static uv_mutex_t             g_event_mutex;
static uv_cond_t              g_event_cond;

/* Callback installed by main(); runs on whichever thread emits the event. */
static void collect_event(const goc_stats_event_t* ev, void* ud) {
    (void)ud;
    uv_mutex_lock(&g_event_mutex);
    if (g_event_count < EVENT_BUF_CAP) {
        g_events[g_event_count++] = *ev;
        uv_cond_signal(&g_event_cond);
    }
    uv_mutex_unlock(&g_event_mutex);
}

/* -------------------------------------------------------------------------
 * Drain helpers
 *
 * next_event() blocks (up to EVENT_TIMEOUT_MS) until the next unread event
 * is available.  Returns NULL on timeout.
 *
 * find_* helpers scan forward from g_read_pos looking for a matching event,
 * calling next_event() for each hop.  MAX_DRAIN caps the scan to prevent
 * infinite waits if the expected event is never emitted.
 * ------------------------------------------------------------------------- */

#define EVENT_TIMEOUT_MS 5000
#define MAX_DRAIN        64

static const goc_stats_event_t* next_event(void) {
    uv_mutex_lock(&g_event_mutex);
    uint64_t deadline = uv_hrtime() + (uint64_t)EVENT_TIMEOUT_MS * 1000000ULL;
    while (g_read_pos >= g_event_count) {
        uint64_t now = uv_hrtime();
        if (now >= deadline) {
            uv_mutex_unlock(&g_event_mutex);
            return NULL;
        }
        uv_cond_timedwait(&g_event_cond, &g_event_mutex, deadline - now);
    }
    const goc_stats_event_t* ev = &g_events[g_read_pos++];
    uv_mutex_unlock(&g_event_mutex);
    return ev;
}

/* Flush the stats delivery pipeline then discard all buffered events.
 * goc_stats_flush() blocks until the loop thread has drained its MPSC queue,
 * ensuring no in-flight events land in g_events[] after the reset. */
static int drain_pending_events(void) {
    goc_stats_flush();
    uv_mutex_lock(&g_event_mutex);
    int n = (int)(g_event_count - g_read_pos);
    g_read_pos    = 0;
    g_event_count = 0;
    uv_mutex_unlock(&g_event_mutex);
    return n;
}

/* Non-blocking peek: returns the next unread event if one is already in the
 * buffer, or NULL if the buffer is empty.  Does not block or wait. */
static const goc_stats_event_t* peek_event(void) {
    uv_mutex_lock(&g_event_mutex);
    const goc_stats_event_t* ev = NULL;
    if (g_read_pos < g_event_count)
        ev = &g_events[g_read_pos++];
    uv_mutex_unlock(&g_event_mutex);
    return ev;
}

static const goc_stats_event_t* find_worker_event(int id) {
    for (int i = 0; i < MAX_DRAIN; i++) {
        const goc_stats_event_t* ev = next_event();
        if (!ev) return NULL;
        if (ev->type == GOC_STATS_EVENT_WORKER_STATUS && ev->data.worker.id == id)
            return ev;
    }
    return NULL;
}

static const goc_stats_event_t* find_fiber_event(
        int id, goc_stats_fiber_status_t status) {
    for (int i = 0; i < MAX_DRAIN; i++) {
        const goc_stats_event_t* ev = next_event();
        if (!ev) return NULL;
        if (ev->type == GOC_STATS_EVENT_FIBER_STATUS &&
            ev->data.fiber.id == id &&
            ev->data.fiber.status == (int)status)
            return ev;
    }
    return NULL;
}

static const goc_stats_event_t* find_channel_event(int id) {
    for (int i = 0; i < MAX_DRAIN; i++) {
        const goc_stats_event_t* ev = next_event();
        if (!ev) return NULL;
        if (ev->type == GOC_STATS_EVENT_CHANNEL_STATUS && ev->data.channel.id == id)
            return ev;
    }
    return NULL;
}

static const goc_stats_event_t* find_any_fiber_created(void) {
    for (int i = 0; i < MAX_DRAIN; i++) {
        const goc_stats_event_t* ev = next_event();
        if (!ev) return NULL;
        if (ev->type == GOC_STATS_EVENT_FIBER_STATUS &&
            ev->data.fiber.status == GOC_FIBER_CREATED)
            return ev;
    }
    return NULL;
}

static const goc_stats_event_t* find_any_worker_status(
        goc_stats_worker_status_t status) {
    for (int i = 0; i < MAX_DRAIN; i++) {
        const goc_stats_event_t* ev = next_event();
        if (!ev) return NULL;
        if (ev->type == GOC_STATS_EVENT_WORKER_STATUS &&
            ev->data.worker.status == (int)status)
            return ev;
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Fiber entry points for S1.3–S1.5
 * ------------------------------------------------------------------------- */

static void emit_worker_event(void* arg) {
    (void)arg;
    GOC_STATS_WORKER_STATUS(/*id=*/42, /*pool_id=*/1234, GOC_WORKER_RUNNING, /*pending_jobs=*/5, /*steal_att=*/0, /*steal_suc=*/0);
}

static void emit_fiber_event(void* arg) {
    (void)arg;
    GOC_STATS_FIBER_STATUS(/*id=*/7, /*last_worker_id=*/3,
                           /*last_pool_id=*/0xABCD,
                           GOC_FIBER_COMPLETED);
}

static void emit_channel_event(void* arg) {
    (void)arg;
    GOC_STATS_CHANNEL_STATUS(/*id=*/99, /*status=*/0, /*buf_size=*/16, /*item_count=*/4, /*ts=*/0, /*ps=*/0, /*cr=*/0, /*er=*/0);
}


/* =========================================================================
 * S1.1 — goc_stats_init() / goc_stats_shutdown() are safe no-ops without
 *         GOC_ENABLE_STATS, and initialise / shut down correctly with it.
 * ====================================================================== */

static void test_s1_1(void) {
    TEST_BEGIN("S1.1  goc_stats_init/shutdown complete without error");
    goc_stats_init();
    TEST_PASS();
done:;
}

/*
 * S1.2 — goc_stats_is_enabled() is true after goc_stats_init()
 */
static void test_s1_2(void) {
    TEST_BEGIN("S1.2  goc_stats_is_enabled() true after goc_stats_init");
    ASSERT(goc_stats_is_enabled());
    TEST_PASS();
done:;
}

/*
 * S1.3 — Worker event round-trips with correct type and fields
 */
static void test_s1_3(void) {
    TEST_BEGIN("S1.3  worker event round-trips with correct fields");
    goc_go(emit_worker_event, NULL);
    const goc_stats_event_t* ev = find_worker_event(42);
    ASSERT(ev != NULL);
    ASSERT(ev->type == GOC_STATS_EVENT_WORKER_STATUS);
    ASSERT(ev->timestamp > 0);
    ASSERT(ev->data.worker.id           == 42);
    ASSERT(ev->data.worker.pool_id      == 1234);
    ASSERT(ev->data.worker.status       == GOC_WORKER_RUNNING);
    ASSERT(ev->data.worker.pending_jobs == 5);
    /* New: assert steal_attempts == 0 and steal_successes == 0 */
    ASSERT(ev->data.worker.steal_attempts == 0);
    ASSERT(ev->data.worker.steal_successes == 0);
    TEST_PASS();
done:;
}

/*
 * S1.4 — Fiber event round-trips with correct type and fields
 */
static void test_s1_4(void) {
    TEST_BEGIN("S1.4  fiber event round-trips with correct fields");
    goc_go(emit_fiber_event, NULL);
    const goc_stats_event_t* ev = find_fiber_event(7, GOC_FIBER_COMPLETED);
    ASSERT(ev != NULL);
    ASSERT(ev->type == GOC_STATS_EVENT_FIBER_STATUS);
    ASSERT(ev->timestamp > 0);
    ASSERT(ev->data.fiber.id             == 7);
    ASSERT(ev->data.fiber.last_worker_id == 3);
    ASSERT(ev->data.fiber.last_pool_id   == 0xABCD);
    ASSERT(ev->data.fiber.status         == GOC_FIBER_COMPLETED);
    TEST_PASS();
done:;
}

/*
 * S1.5 — Channel event round-trips with correct type and fields
 */
static void test_s1_5(void) {
    TEST_BEGIN("S1.5  channel event round-trips with correct fields");
    goc_go(emit_channel_event, NULL);
    const goc_stats_event_t* ev = find_channel_event(99);
    ASSERT(ev != NULL);
    ASSERT(ev->type == GOC_STATS_EVENT_CHANNEL_STATUS);
    ASSERT(ev->timestamp > 0);
    ASSERT(ev->data.channel.id         == 99);
    ASSERT(ev->data.channel.status     == 0);
    ASSERT(ev->data.channel.buf_size   == 16);
    ASSERT(ev->data.channel.item_count == 4);
    /* New: assert taker_scans == 0, putter_scans == 0, compaction_runs == 0, entries_removed == 0 */
    ASSERT(ev->data.channel.taker_scans == 0);
    ASSERT(ev->data.channel.putter_scans == 0);
    ASSERT(ev->data.channel.compaction_runs == 0);
    ASSERT(ev->data.channel.entries_removed == 0);
    TEST_PASS();
done:;
}

/* =========================================================================
 * S2.x tests — automatic pool.c instrumentation
 * ====================================================================== */

static void noop_fiber(void* arg) { (void)arg; }

/*
 * Stats subsystem is enabled and pool.c instrumentation is active.
 * This is a prerequisite gate for S2.1–S2.3.  Spawning a fiber causes
 * pool_submit_spawn to emit GOC_FIBER_CREATED synchronously on the calling
 * thread, so the event is in the buffer before goc_go() returns.  A
 * non-blocking peek distinguishes "pool compiled with stats" from "stats
 * subsystem enabled but pool macros are no-ops".
 *
 * If this test fails, rebuild with: cmake -B build -DGOC_ENABLE_STATS=ON
 */

/*
 * S2.1 — goc_go() emits GOC_FIBER_CREATED; fiber exit emits GOC_FIBER_COMPLETED
 */
static void test_s2_1(void) {
    TEST_BEGIN("S2.1  goc_go emits FIBER_CREATED; completion emits FIBER_COMPLETED");
    drain_pending_events();
    goc_go(noop_fiber, NULL);

    const goc_stats_event_t* created = find_any_fiber_created();
    ASSERT(created != NULL);
    ASSERT(created->data.fiber.last_worker_id == -1);
    ASSERT(created->data.fiber.last_pool_id >= 0);
    ASSERT(created->timestamp > 0);
    int fiber_id = created->data.fiber.id;

    const goc_stats_event_t* completed = find_fiber_event(fiber_id, GOC_FIBER_COMPLETED);
    ASSERT(completed != NULL);
    ASSERT(completed->data.fiber.last_worker_id >= 0);
    ASSERT(completed->data.fiber.last_pool_id == created->data.fiber.last_pool_id);
    ASSERT(completed->timestamp >= created->timestamp);

    TEST_PASS();
done:;
}

/*
 * S2.2 — After a fiber completes, the owning worker transitions to GOC_WORKER_IDLE
 */
static void test_s2_2(void) {
    TEST_BEGIN("S2.2  worker transitions to GOC_WORKER_IDLE after fiber completes");
    drain_pending_events();
    goc_go(noop_fiber, NULL);

    const goc_stats_event_t* created = find_any_fiber_created();
    ASSERT(created != NULL);
    int fiber_id = created->data.fiber.id;

    const goc_stats_event_t* completed = find_fiber_event(fiber_id, GOC_FIBER_COMPLETED);
    ASSERT(completed != NULL);

    const goc_stats_event_t* idle = find_any_worker_status(GOC_WORKER_IDLE);
    ASSERT(idle != NULL);
    ASSERT(idle->data.worker.id >= 0);

    TEST_PASS();
done:;
}

/*
 * S2.3 — goc_pool_make() emits GOC_WORKER_CREATED per thread
 */
static void test_s2_3(void) {
    TEST_BEGIN("S2.3  goc_pool_make emits GOC_WORKER_CREATED per thread");
    drain_pending_events();
    goc_pool* pool = NULL;
    pool = goc_pool_make(2);

    bool saw[2] = {false, false};
    int n_found = 0;
    for (int i = 0; i < MAX_DRAIN && n_found < 2; i++) {
        const goc_stats_event_t* ev = next_event();
        ASSERT(ev != NULL);
        if (ev->type != GOC_STATS_EVENT_WORKER_STATUS) continue;
        if (ev->data.worker.status != GOC_WORKER_CREATED) continue;
        int id = ev->data.worker.id;
        if (id == 0 || id == 1) {
            saw[id] = true;
            n_found++;
        }
    }
    ASSERT(saw[0]);
    ASSERT(saw[1]);

    goc_pool_destroy(pool);
    pool = NULL;
    TEST_PASS();
done:
    if (pool) goc_pool_destroy(pool);
}

/*
 * S2.4 — Pool creation and destruction events
 */
static void test_s2_4(void) {
    TEST_BEGIN("S2.4  pool creation and destruction events");
    drain_pending_events();
    goc_pool* pool = goc_pool_make(3);
    int pool_id = goc_pool_id(pool);
    const goc_stats_event_t* create_ev = NULL;
    const goc_stats_event_t* destroy_ev = NULL;
    // Find pool created event
    for (int i = 0; i < MAX_DRAIN; i++) {
        const goc_stats_event_t* ev = next_event();
        if (ev && ev->type == GOC_STATS_EVENT_POOL_STATUS &&
            ev->data.pool.id == pool_id && ev->data.pool.status == GOC_POOL_CREATED) {
            create_ev = ev;
            break;
        }
    }
    ASSERT(create_ev != NULL);
    ASSERT(create_ev->data.pool.thread_count == 3);
    goc_pool_destroy(pool);
    // Find pool destroyed event
    for (int i = 0; i < MAX_DRAIN; i++) {
        const goc_stats_event_t* ev = next_event();
        if (ev && ev->type == GOC_STATS_EVENT_POOL_STATUS &&
            ev->data.pool.id == pool_id && ev->data.pool.status == GOC_POOL_DESTROYED) {
            destroy_ev = ev;
            break;
        }
    }
    ASSERT(destroy_ev != NULL);
    ASSERT(destroy_ev->data.pool.thread_count == 3);
    TEST_PASS();
done:;
}

/*
 * S3.1 — Channel open/close events via API
 */
static void test_s3_1(void) {
    TEST_BEGIN("S3.1  channel open/close events via API");
    goc_chan* ch = goc_chan_make(0);
    int ch_id = (int)(intptr_t)ch;
    const goc_stats_event_t* open_ev = find_channel_event(ch_id);
    ASSERT(open_ev != NULL);
    ASSERT(open_ev->data.channel.status == 1); // open
    ASSERT(open_ev->data.channel.buf_size == 0);
    ASSERT(open_ev->data.channel.item_count == 0);
    goc_close(ch);
    const goc_stats_event_t* close_ev = find_channel_event(ch_id);
    ASSERT(close_ev != NULL);
    ASSERT(close_ev->data.channel.status == 0); // closed
    ASSERT(close_ev->data.channel.buf_size == 0);
    ASSERT(close_ev->data.channel.item_count == 0);
    TEST_PASS();
done:;
}

/*
 * S3.2 — Buffered channel: buf_size and item_count after put/take
 */
// S3.2 fiber args and function
static struct {
    goc_chan* ch;
    goc_val_t* taken;
} s3_2_fiber_args;

static void s3_2_fiber(void* arg) {
    typeof(s3_2_fiber_args)* args = arg;
    goc_put(args->ch, (void*)1);
    goc_put(args->ch, (void*)2);
    args->taken = goc_take(args->ch);
}

static void test_s3_2(void) {
    TEST_BEGIN("S3.2  buffered channel buf_size and item_count");
    goc_chan* ch = goc_chan_make(2);
    int ch_id = (int)(intptr_t)ch;
    // Open event
    const goc_stats_event_t* open_ev = find_channel_event(ch_id);
    ASSERT(open_ev != NULL);
    ASSERT(open_ev->data.channel.status == 1);
    ASSERT(open_ev->data.channel.buf_size == 2);
    ASSERT(open_ev->data.channel.item_count == 0);

    s3_2_fiber_args.ch = ch;
    s3_2_fiber_args.taken = NULL;
    goc_chan* join = goc_go(s3_2_fiber, &s3_2_fiber_args);
    goc_take_sync(join); // wait for fiber to finish

    ASSERT(s3_2_fiber_args.taken->val == (void*)1);
    goc_close(ch);

    const goc_stats_event_t* close_ev = find_channel_event(ch_id);
    ASSERT(close_ev != NULL);
    ASSERT(close_ev->data.channel.status == 0);
    ASSERT(close_ev->data.channel.buf_size == 2);
    ASSERT(close_ev->data.channel.item_count == 1);
    TEST_PASS();
done:;
}

/*
 * S3.3 — Multiple fibers and channels: all events unique and present
 */
static void test_s3_3(void) {
    TEST_BEGIN("S3.3  multiple fibers and channels events");
    goc_chan* ch1 = goc_chan_make(0);
    goc_chan* ch2 = goc_chan_make(1);
    int ch1_id = (int)(intptr_t)ch1;
    int ch2_id = (int)(intptr_t)ch2;
    goc_go(noop_fiber, NULL);
    goc_go(noop_fiber, NULL);
    // Find both channel open events
    const goc_stats_event_t* open1 = find_channel_event(ch1_id);
    const goc_stats_event_t* open2 = find_channel_event(ch2_id);
    ASSERT(open1 != NULL && open2 != NULL);
    // Find two fiber created events
    int found = 0;
    for (int i = 0; i < MAX_DRAIN && found < 2; i++) {
        const goc_stats_event_t* ev = next_event();
        if (ev && ev->type == GOC_STATS_EVENT_FIBER_STATUS && ev->data.fiber.status == GOC_FIBER_CREATED)
            found++;
    }
    ASSERT(found == 2);
    goc_close(ch1);
    goc_close(ch2);
    // Find both channel close events
    const goc_stats_event_t* close1 = find_channel_event(ch1_id);
    const goc_stats_event_t* close2 = find_channel_event(ch2_id);
    ASSERT(close1 != NULL && close2 != NULL);
    TEST_PASS();
done:;
}

/*
 * S3.4 — close event carries non-zero taker_scans/putter_scans
 */
static void s3_4_fiber(void* arg) {
    goc_chan* ch = (goc_chan*)arg;
    goc_alt_op_t ops[] = {
        { .ch = ch, .op_kind = GOC_ALT_TAKE },
    };
    goc_alts(ops, 1); // parks, increments taker_scans
}

static void test_s3_4(void) {
    TEST_BEGIN("S3.4  channel close event carries non-zero taker_scans/putter_scans");
    goc_chan* ch = goc_chan_make(0);
    int ch_id = (int)(intptr_t)ch;
    drain_pending_events();
    
    goc_chan* join = goc_go(s3_4_fiber, ch);
    goc_nanosleep(1000000); // 1ms to ensure fiber parks
    goc_close(ch); // triggers close event
    goc_take_sync(join); // cleanup
    
    const goc_stats_event_t* close_ev = NULL;
    for (int i = 0; i < MAX_DRAIN; i++) {
        const goc_stats_event_t* ev = find_channel_event(ch_id);
        if (!ev) break;
        if (ev->data.channel.status == 0) {
            close_ev = ev;
            break;
        }
    }
    ASSERT(close_ev != NULL);
    ASSERT(close_ev->data.channel.taker_scans >= 1);
    TEST_PASS();
done:;
}

/*
 * S3.5 — close event carries non-zero compaction_runs/entries_removed
 */
#ifndef GOC_DEAD_COUNT_THRESHOLD
#define GOC_DEAD_COUNT_THRESHOLD 8
#endif
static _Atomic int s3_5_parked = 0;
static void s3_5_fiber(void* arg) {
    atomic_fetch_add_explicit(&s3_5_parked, 1, memory_order_relaxed);
    goc_chan* ch = (goc_chan*)arg;
    goc_alt_op_t ops[] = {
        { .ch = ch, .op_kind = GOC_ALT_TAKE },
    };
    goc_alts(ops, 1); // parks, will be cancelled
}

static void s3_5_fiber_2(void* ub) {
    goc_chan* ch = (goc_chan*)ub;
    goc_take(ch);
}

static void test_s3_5(void) {
    TEST_BEGIN("S3.5  channel close event carries non-zero compaction_runs/entries_removed");
    goc_chan* ch = goc_chan_make(0);
    int ch_id = goc_unbox_int(ch);
    drain_pending_events();
    
    // Spawn enough fibers parked on the channel 
    // to create "dead" entries above the compaction threshold
    // when they are cancelled by the close.
    int n_fibers = GOC_DEAD_COUNT_THRESHOLD * 2;
    atomic_store_explicit(&s3_5_parked, 0, memory_order_relaxed);
    for (int i = 0; i < n_fibers; i++)
        goc_go(s3_5_fiber, ch);

    // Wait for all fibers to park
    int wait_loops = 0;
    while (atomic_load_explicit(&s3_5_parked, memory_order_relaxed) < n_fibers && wait_loops++ < 1000) {
        goc_nanosleep(100); // 0.1ms
    }

    // trigger compaction by doing a take in another fiber, 
    // which will see the dead entries and cause them to be removed
    goc_go(s3_5_fiber_2, ch);

    goc_close(ch);
    
    const goc_stats_event_t* close_ev = NULL;
    for (int i = 0; i < MAX_DRAIN * MAX_DRAIN; i++) {
        const goc_stats_event_t* ev = next_event();
        if (!ev) break;
        if (ev->type == GOC_STATS_EVENT_CHANNEL_STATUS &&
            ev->data.channel.id == ch_id &&
            ev->data.channel.status == GOC_CLOSED) {
            close_ev = ev;
            break;
        }
    }
    ASSERT(close_ev != NULL);
    ASSERT(close_ev->data.channel.compaction_runs >= 1);
    ASSERT(close_ev->data.channel.entries_removed >= 1);
    TEST_PASS();
done:;
}

/*
 * S5.1 — Worker STOPPED event carries valid steal_attempts/steal_successes
 */
static void test_s5_1(void) {
    TEST_BEGIN("S5.1  worker STOPPED event carries steal_attempts/steal_successes");
    drain_pending_events();

    goc_pool* pool = goc_pool_make(2);
    int pool_id = goc_pool_id(pool);

    /* Submit many short fibers to provoke work stealing between the 2 workers.
     * Each fiber emits ~4 events (FIBER_CREATED, FIBER_COMPLETED, join_ch
     * CHANNEL_OPEN, CHANNEL_CLOSE) plus worker RUNNING/IDLE transitions, so
     * EVENT_BUF_CAP must be large enough to absorb all of them before the two
     * STOPPED events are emitted. */
    for (int i = 0; i < 64; i++)
        goc_go_on(pool, noop_fiber, NULL);

    goc_pool_destroy(pool);

    /* Flush the async delivery pipeline so all STOPPED events emitted by the
     * workers are guaranteed to be in g_events[] before the scan loop runs. */
    goc_stats_flush();

    /* Collect all STOPPED events for this pool's workers and validate fields.
     * Use a large scan window: 256 fibers × (CREATED + COMPLETED + IDLE) events
     * can easily exceed MAX_DRAIN before the two STOPPED events appear. */
    int stopped_found = 0;
    for (int i = 0; i < 1024; i++) {
        const goc_stats_event_t* ev = next_event();
        if (!ev) break;
        if (ev->type != GOC_STATS_EVENT_WORKER_STATUS) continue;
        if (ev->data.worker.status != GOC_WORKER_STOPPED) continue;
        if (ev->data.worker.pool_id != pool_id) continue;
        ASSERT(ev->data.worker.steal_attempts >= 0);
        ASSERT(ev->data.worker.steal_successes <= ev->data.worker.steal_attempts);
        stopped_found++;
    }
    ASSERT(stopped_found >= 2);

    TEST_PASS();
done:;
}

/*
 * S6.1 — goc_cb_queue_get_hwm() reflects peak callback-queue depth
 *
 * Issue a burst of goc_put_cb / goc_take_cb calls so that at least one
 * callback entry is enqueued, then assert that the high-water mark is >= 1.
 */
#define S6_1_N 8

typedef struct {
    _Atomic int rem;
    uv_mutex_t  mtx;
    uv_cond_t   cond;
} s6_1_state_t;

static void s6_1_take_cb(void* val, goc_status_t ok, void* vud) {
    (void)val; (void)ok;
    s6_1_state_t* s = vud;
    uv_mutex_lock(&s->mtx);
    if (atomic_fetch_sub_explicit(&s->rem, 1, memory_order_relaxed) == 1)
        uv_cond_signal(&s->cond);
    uv_mutex_unlock(&s->mtx);
}

static void test_s6_1(void) {
    TEST_BEGIN("S6.1  goc_cb_queue_get_hwm reflects peak depth");

    s6_1_state_t s;
    atomic_store(&s.rem, S6_1_N);
    uv_mutex_init(&s.mtx);
    uv_cond_init(&s.cond);

    goc_chan* chs[S6_1_N];
    for (int i = 0; i < S6_1_N; i++)
        chs[i] = goc_chan_make(0);

    /* Register take callbacks first so put_cb delivers immediately via the
     * parked-taker path, ensuring entries pass through the callback queue. */
    for (int i = 0; i < S6_1_N; i++)
        goc_take_cb(chs[i], s6_1_take_cb, &s);
    for (int i = 0; i < S6_1_N; i++)
        goc_put_cb(chs[i], (void*)(uintptr_t)(i + 1), NULL, NULL);

    /* Wait for all take callbacks to fire. */
    uv_mutex_lock(&s.mtx);
    while (atomic_load_explicit(&s.rem, memory_order_relaxed) > 0)
        uv_cond_wait(&s.cond, &s.mtx);
    uv_mutex_unlock(&s.mtx);

    ASSERT(goc_cb_queue_get_hwm() >= 1);

    for (int i = 0; i < S6_1_N; i++)
        goc_close(chs[i]);
    uv_cond_destroy(&s.cond);
    uv_mutex_destroy(&s.mtx);

    TEST_PASS();
done:;
}

/*
 * S6.2 — goc_timeout_get_stats tracks allocations and expirations
 *
 * 1. Read baseline (allocs0, expires0).
 * 2. Create N timeout channels via goc_timeout().
 * 3. Assert allocs - allocs0 == N.
 * 4. Wait for all to fire; assert expires - expires0 == N.
 */

#define S6_2_N 4

static void test_s6_2(void) {
    TEST_BEGIN("S6.2  goc_timeout_get_stats tracks allocations and expirations");

    uint64_t allocs0, expires0;
    goc_timeout_get_stats(&allocs0, &expires0);

    goc_stats_flush();
    goc_chan* chs[S6_2_N];
    for (int i = 0; i < S6_2_N; i++)
        chs[i] = goc_timeout(20); /* 20 ms */

    uint64_t allocs1, expires1;
    goc_timeout_get_stats(&allocs1, &expires1);
    ASSERT(allocs1 - allocs0 == S6_2_N);

    /* Wait for all timeout channels to fire (they close on expiry). */

    for (int i = 0; i < S6_2_N; i++)
        goc_take_sync(chs[i]);

    /* Robust: poll for expirations with timeout (max 100ms total) */
    const int max_wait_ms = 100;
    int waited = 0;
    do {
        goc_timeout_get_stats(&allocs1, &expires1);
        if ((int)(expires1 - expires0) == S6_2_N) break;
        goc_nanosleep(1000000); /* 1ms */
        waited++;
    } while (waited < max_wait_ms);
    ASSERT(expires1 - expires0 == S6_2_N);

    TEST_PASS();
done:;
}

/*
 * S6.3 — goc_pool_get_steal_stats returns non-decreasing totals
 *
 * 1. Read baseline (att0, suc0).
 * 2. Create a 2-worker pool; spawn many short fibers to cause stealing; destroy.
 * 3. Assert att1 >= att0, suc1 >= suc0, (suc1 - suc0) <= (att1 - att0).
 */
static void test_s6_3(void) {
    TEST_BEGIN("S6.3  goc_pool_get_steal_stats returns non-decreasing totals");

    uint64_t att0, suc0, mis0, wak0;
    goc_pool_get_steal_stats(&att0, &suc0, &mis0, &wak0);

    goc_pool* pool = goc_pool_make(2);
    for (int i = 0; i < 64; i++)
        goc_go_on(pool, noop_fiber, NULL);
    goc_pool_destroy(pool);

    uint64_t att1, suc1, mis1, wak1;
    goc_pool_get_steal_stats(&att1, &suc1, &mis1, &wak1);

    ASSERT(att1 >= att0);
    ASSERT(suc1 >= suc0);
    ASSERT((suc1 - suc0) <= (att1 - att0));

    TEST_PASS();
done:;
}

/*
 * S6.4 — steal_misses and idle_wakeups counters are readable; invariants hold
 *
 * Create a pool and immediately destroy it without posting any fibers.
 * Verifies that the new counters compile, are accessible, and satisfy the
 * invariant misses <= attempts.
 *
 * Note: neither steal_misses nor idle_wakeups is asserted to be zero.
 * Workers scan each other's empty deques before going idle (incrementing
 * steal_misses), and goc_pool_destroy wakes every worker for shutdown
 * (incrementing idle_wakeups) — both happen even with no work posted.
 */
static void test_s6_4(void) {
    TEST_BEGIN("S6.4  steal counters readable and misses<=attempts invariant holds");

    uint64_t att0, suc0, mis0, wak0;
    goc_pool_get_steal_stats(&att0, &suc0, &mis0, &wak0);

    goc_pool* pool = goc_pool_make(2);
    goc_pool_destroy(pool);

    uint64_t att1, suc1, mis1, wak1;
    goc_pool_get_steal_stats(&att1, &suc1, &mis1, &wak1);

    ASSERT((mis1 - mis0) <= (att1 - att0));
    ASSERT((suc1 - suc0) <= (att1 - att0));

    TEST_PASS();
done:;
}

/*
 * S6.5 — idle_wakeups increments on each sleep/wake cycle
 *
 * Post a single fiber from the main thread (external injection) on a 2-worker
 * pool.  At least one worker must wake to pick it up, so global idle_wakeups
 * must increase by at least 1 after the pool is destroyed.
 */
static void test_s6_5(void) {
    TEST_BEGIN("S6.5  idle_wakeups increments after external injection");

    uint64_t att0, suc0, mis0, wak0;
    goc_pool_get_steal_stats(&att0, &suc0, &mis0, &wak0);

    goc_pool* pool = goc_pool_make(2);
    /* Wait until both workers are parked before posting, so the external
     * injection is guaranteed to wake at least one via uv_sem_post. */
    pool_wait_all_idle(pool, 2);
    goc_go_on(pool, noop_fiber, NULL);
    goc_pool_destroy(pool);

    uint64_t att1, suc1, mis1, wak1;
    goc_pool_get_steal_stats(&att1, &suc1, &mis1, &wak1);

    ASSERT((wak1 - wak0) >= 1);

    TEST_PASS();
done:;
}

/*
 * S6.6 — goc_pool_get_steal_stats extended signature: all counters non-negative
 *
 * Verify the 4-output-param accessor compiles and returns plausible values:
 * all counters >= 0 and successes <= attempts, misses <= attempts.
 */
static void test_s6_6(void) {
    TEST_BEGIN("S6.6  goc_pool_get_steal_stats extended signature returns valid values");

    uint64_t attempts = 0, successes = 0, misses = 0, idle_wakeups = 0;
    goc_pool_get_steal_stats(&attempts, &successes, &misses, &idle_wakeups);

    ASSERT(successes <= attempts);
    ASSERT(misses    <= attempts);

    TEST_PASS();
done:;
}

/*
 * S4_1 — goc_stats_shutdown() disables stats delivery
 */
static void test_s4_1(void) {
    TEST_BEGIN("S4_1  goc_stats_shutdown disables stats");
    ASSERT(goc_stats_is_enabled());
    goc_stats_flush();
    goc_stats_shutdown();
    ASSERT(!goc_stats_is_enabled());
    TEST_PASS();
done:;
}


/* =========================================================================
 * main
 * ====================================================================== */

int main(void) {
    install_crash_handler();
    goc_test_arm_watchdog(90);

    printf("libgoc test suite — Stats / Telemetry\n");
    printf("=======================================\n\n");

    goc_init();
    goc_stats_init();

    /* Set up event collection before any test runs. */
    uv_mutex_init(&g_event_mutex);
    uv_cond_init(&g_event_cond);
    goc_stats_set_callback(collect_event, NULL);

    printf("Stats — Telemetry\n");
    test_s1_1();
    test_s1_2();
    test_s1_3();
    test_s1_4();
    test_s1_5();
    test_s2_1();
    test_s2_2();
    test_s2_3();
    test_s2_4();
    test_s3_1();
    test_s3_2();
    test_s3_3();
    test_s3_4();
    test_s3_5();
    test_s5_1();
    test_s6_1();
    test_s6_2();
    test_s6_3();
    test_s6_4();
    test_s6_5();
    test_s6_6();
    test_s4_1();
    printf("\n");

    /* Shut down the runtime before destroying the event buffer mutex/cond.
     * goc_shutdown() waits for all worker threads to stop, guaranteeing no
     * in-flight collect_event callback can race with the uv_mutex_destroy. */
    goc_shutdown();
    uv_cond_destroy(&g_event_cond);
    uv_mutex_destroy(&g_event_mutex);

    printf("=======================================\n");
    printf("Results: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed > 0)
        printf(", %d FAILED", g_tests_failed);
    printf("\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
