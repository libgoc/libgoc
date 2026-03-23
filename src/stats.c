/*
 * src/stats.c
 *
 * Optional runtime instrumentation counters for benchmarking/tuning.
 * Enabled when compiled with -DLIBGOC_STATS_ENABLED.
 */

#include <string.h>
#include <stdatomic.h>
#include "internal.h"

#ifdef LIBGOC_STATS_ENABLED

static _Atomic uint64_t g_chan_put_scan_steps_total;
static _Atomic uint64_t g_chan_put_scan_steps_max;
static _Atomic uint64_t g_chan_take_scan_steps_total;
static _Atomic uint64_t g_chan_take_scan_steps_max;

static _Atomic uint64_t g_dead_compactions;
static _Atomic uint64_t g_dead_entries_removed;

static _Atomic uint64_t g_callback_queue_depth;
static _Atomic uint64_t g_callback_queue_depth_max;

static _Atomic uint64_t g_timeout_allocations;
static _Atomic uint64_t g_timeout_expirations;

static _Atomic uint64_t g_injector_pop_attempts;
static _Atomic uint64_t g_injector_pop_successes;
static _Atomic uint64_t g_deque_pop_attempts;
static _Atomic uint64_t g_deque_pop_successes;
static _Atomic uint64_t g_steal_attempts;
static _Atomic uint64_t g_steal_successes;

static void atomic_max_u64(_Atomic uint64_t* slot, uint64_t v) {
    uint64_t cur = atomic_load_explicit(slot, memory_order_relaxed);
    while (v > cur &&
           !atomic_compare_exchange_weak_explicit(slot, &cur, v,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {}
}

bool goc_stats_enabled(void) {
    return true;
}

void goc_stats_reset(void) {
    atomic_store_explicit(&g_chan_put_scan_steps_total, 0, memory_order_relaxed);
    atomic_store_explicit(&g_chan_put_scan_steps_max, 0, memory_order_relaxed);
    atomic_store_explicit(&g_chan_take_scan_steps_total, 0, memory_order_relaxed);
    atomic_store_explicit(&g_chan_take_scan_steps_max, 0, memory_order_relaxed);

    atomic_store_explicit(&g_dead_compactions, 0, memory_order_relaxed);
    atomic_store_explicit(&g_dead_entries_removed, 0, memory_order_relaxed);

    atomic_store_explicit(&g_callback_queue_depth, 0, memory_order_relaxed);
    atomic_store_explicit(&g_callback_queue_depth_max, 0, memory_order_relaxed);

    atomic_store_explicit(&g_timeout_allocations, 0, memory_order_relaxed);
    atomic_store_explicit(&g_timeout_expirations, 0, memory_order_relaxed);

    atomic_store_explicit(&g_injector_pop_attempts, 0, memory_order_relaxed);
    atomic_store_explicit(&g_injector_pop_successes, 0, memory_order_relaxed);
    atomic_store_explicit(&g_deque_pop_attempts, 0, memory_order_relaxed);
    atomic_store_explicit(&g_deque_pop_successes, 0, memory_order_relaxed);
    atomic_store_explicit(&g_steal_attempts, 0, memory_order_relaxed);
    atomic_store_explicit(&g_steal_successes, 0, memory_order_relaxed);
}

void goc_stats_snapshot(goc_stats_t* out) {
    if (out == NULL)
        return;

    out->chan_put_scan_steps_total = atomic_load_explicit(&g_chan_put_scan_steps_total, memory_order_relaxed);
    out->chan_put_scan_steps_max = atomic_load_explicit(&g_chan_put_scan_steps_max, memory_order_relaxed);
    out->chan_take_scan_steps_total = atomic_load_explicit(&g_chan_take_scan_steps_total, memory_order_relaxed);
    out->chan_take_scan_steps_max = atomic_load_explicit(&g_chan_take_scan_steps_max, memory_order_relaxed);

    out->dead_compactions = atomic_load_explicit(&g_dead_compactions, memory_order_relaxed);
    out->dead_entries_removed = atomic_load_explicit(&g_dead_entries_removed, memory_order_relaxed);

    out->callback_queue_depth = atomic_load_explicit(&g_callback_queue_depth, memory_order_relaxed);
    out->callback_queue_depth_max = atomic_load_explicit(&g_callback_queue_depth_max, memory_order_relaxed);

    out->timeout_allocations = atomic_load_explicit(&g_timeout_allocations, memory_order_relaxed);
    out->timeout_expirations = atomic_load_explicit(&g_timeout_expirations, memory_order_relaxed);

    out->injector_pop_attempts = atomic_load_explicit(&g_injector_pop_attempts, memory_order_relaxed);
    out->injector_pop_successes = atomic_load_explicit(&g_injector_pop_successes, memory_order_relaxed);
    out->deque_pop_attempts = atomic_load_explicit(&g_deque_pop_attempts, memory_order_relaxed);
    out->deque_pop_successes = atomic_load_explicit(&g_deque_pop_successes, memory_order_relaxed);
    out->steal_attempts = atomic_load_explicit(&g_steal_attempts, memory_order_relaxed);
    out->steal_successes = atomic_load_explicit(&g_steal_successes, memory_order_relaxed);
}

void goc_stats_record_chan_put_scan(size_t steps) {
    uint64_t s = (uint64_t)steps;
    atomic_fetch_add_explicit(&g_chan_put_scan_steps_total, s, memory_order_relaxed);
    atomic_max_u64(&g_chan_put_scan_steps_max, s);
}

void goc_stats_record_chan_take_scan(size_t steps) {
    uint64_t s = (uint64_t)steps;
    atomic_fetch_add_explicit(&g_chan_take_scan_steps_total, s, memory_order_relaxed);
    atomic_max_u64(&g_chan_take_scan_steps_max, s);
}

void goc_stats_record_dead_compaction(size_t removed_entries) {
    atomic_fetch_add_explicit(&g_dead_compactions, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_dead_entries_removed, (uint64_t)removed_entries, memory_order_relaxed);
}

void goc_stats_callback_queue_push(void) {
    uint64_t depth = atomic_fetch_add_explicit(&g_callback_queue_depth, 1, memory_order_relaxed) + 1;
    atomic_max_u64(&g_callback_queue_depth_max, depth);
}

void goc_stats_callback_queue_pop(void) {
    uint64_t cur = atomic_load_explicit(&g_callback_queue_depth, memory_order_relaxed);
    while (cur > 0 &&
           !atomic_compare_exchange_weak_explicit(&g_callback_queue_depth, &cur, cur - 1,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {}
}

void goc_stats_record_timeout_alloc(void) {
    atomic_fetch_add_explicit(&g_timeout_allocations, 1, memory_order_relaxed);
}

void goc_stats_record_timeout_expire(void) {
    atomic_fetch_add_explicit(&g_timeout_expirations, 1, memory_order_relaxed);
}

void goc_stats_record_injector_pop_attempt(bool success) {
    atomic_fetch_add_explicit(&g_injector_pop_attempts, 1, memory_order_relaxed);
    if (success)
        atomic_fetch_add_explicit(&g_injector_pop_successes, 1, memory_order_relaxed);
}

void goc_stats_record_deque_pop_attempt(bool success) {
    atomic_fetch_add_explicit(&g_deque_pop_attempts, 1, memory_order_relaxed);
    if (success)
        atomic_fetch_add_explicit(&g_deque_pop_successes, 1, memory_order_relaxed);
}

void goc_stats_record_steal_attempt(bool success) {
    atomic_fetch_add_explicit(&g_steal_attempts, 1, memory_order_relaxed);
    if (success)
        atomic_fetch_add_explicit(&g_steal_successes, 1, memory_order_relaxed);
}

#else

bool goc_stats_enabled(void) {
    return false;
}

void goc_stats_reset(void) {
}

void goc_stats_snapshot(goc_stats_t* out) {
    if (out != NULL)
        memset(out, 0, sizeof(*out));
}

#endif
