/*
 * tests/test_p10_take_all.c — Phase 10: goc_take_all / goc_take_all_sync tests
 *
 * Verifies the helper functions that receive from an array of channels in one
 * call, covering both the fiber (goc_take_all) and OS-thread
 * (goc_take_all_sync) variants.
 *
 * Build:  cmake -B build && cmake --build build
 * Run:    ctest --test-dir build --output-on-failure
 *         ./build/test_p10_take_all
 *
 * Compile requirements: -std=c11 -DGC_THREADS -D_GNU_SOURCE
 *
 * Test coverage (Phase 10 — goc_take_all helpers):
 *
 *   P10.1  goc_take_all_sync with n==0 returns a non-NULL empty array; no crash
 *   P10.2  goc_take_all_sync receives from N buffered channels; all values
 *          intact, order matches channel order
 *   P10.3  goc_take_all_sync on already-closed channels returns GOC_CLOSED for
 *          each element
 *   P10.4  goc_take_all_sync blocks until a fiber sends on each channel;
 *          values intact, return order matches channel order
 *   P10.5  goc_take_all from fiber context receives from N buffered channels;
 *          all values intact, order matches channel order
 *   P10.6  goc_take_all from fiber blocks until a separate fiber sends on each
 *          channel; values intact
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "test_harness.h"
#include "goc.h"

/* =========================================================================
 * Phase 10 — goc_take_all / goc_take_all_sync
 * ====================================================================== */

/* -------------------------------------------------------------------------
 * P10.1 — goc_take_all_sync with n==0: no crash, returns non-NULL
 *
 * An empty channel array is a valid edge case. The function must not crash
 * and must return a non-NULL (possibly zero-length) GC array.
 * ---------------------------------------------------------------------- */

static void test_p10_1(void) {
    TEST_BEGIN("P10.1 goc_take_all_sync(n=0) → non-NULL, no crash");

    goc_val_t** results = goc_take_all_sync(NULL, 0);
    ASSERT(results != NULL);

    TEST_PASS();
done:;
}

/* -------------------------------------------------------------------------
 * P10.2 — goc_take_all_sync on pre-filled buffered channels
 *
 * Three buffered channels are each filled with one value before
 * goc_take_all_sync is called.  The results array must contain all three
 * values with ok==GOC_OK, in channel order.
 * ---------------------------------------------------------------------- */

#define P10_2_N 3

static void test_p10_2(void) {
    TEST_BEGIN("P10.2 goc_take_all_sync on buffered channels → all GOC_OK, correct values");

    goc_chan* chs[P10_2_N];
    uintptr_t vals[P10_2_N] = { 0x11UL, 0x22UL, 0x33UL };

    for (int i = 0; i < P10_2_N; i++) {
        chs[i] = goc_chan_make(1);
        ASSERT(chs[i] != NULL);
        goc_status_t s = goc_put_sync(chs[i], (void*)vals[i]);
        ASSERT(s == GOC_OK);
    }

    goc_val_t** results = goc_take_all_sync(chs, P10_2_N);
    ASSERT(results != NULL);

    for (int i = 0; i < P10_2_N; i++) {
        ASSERT(results[i] != NULL);
        ASSERT(results[i]->ok == GOC_OK);
        ASSERT((uintptr_t)results[i]->val == vals[i]);
    }

    TEST_PASS();
done:;
}

/* -------------------------------------------------------------------------
 * P10.3 — goc_take_all_sync on already-closed channels
 *
 * Closing an empty channel before the call means every element of the
 * result must have ok==GOC_CLOSED.
 * ---------------------------------------------------------------------- */

#define P10_3_N 4

static void test_p10_3(void) {
    TEST_BEGIN("P10.3 goc_take_all_sync on closed channels → all GOC_CLOSED");

    goc_chan* chs[P10_3_N];
    for (int i = 0; i < P10_3_N; i++) {
        chs[i] = goc_chan_make(0);
        ASSERT(chs[i] != NULL);
        goc_close(chs[i]);
    }

    goc_val_t** results = goc_take_all_sync(chs, P10_3_N);
    ASSERT(results != NULL);

    for (int i = 0; i < P10_3_N; i++) {
        ASSERT(results[i] != NULL);
        ASSERT(results[i]->ok == GOC_CLOSED);
    }

    TEST_PASS();
done:;
}

/* -------------------------------------------------------------------------
 * P10.4 — goc_take_all_sync blocks until fibers send
 *
 * Five rendezvous channels are created.  Five fibers each put one value
 * on a distinct channel.  goc_take_all_sync must block until every fiber
 * has sent, then return all values with ok==GOC_OK in channel order.
 * ---------------------------------------------------------------------- */

#define P10_4_N 5

typedef struct {
    goc_chan* ch;
    uintptr_t val;
} p10_4_sender_args_t;

static void p10_4_sender_fn(void* arg) {
    p10_4_sender_args_t* a = (p10_4_sender_args_t*)arg;
    goc_put(a->ch, (void*)a->val);
}

static void test_p10_4(void) {
    TEST_BEGIN("P10.4 goc_take_all_sync blocks until fibers send → correct values");

    goc_chan*             chs[P10_4_N];
    goc_chan*             joins[P10_4_N];
    p10_4_sender_args_t  args[P10_4_N];

    for (int i = 0; i < P10_4_N; i++) {
        chs[i]       = goc_chan_make(0);
        ASSERT(chs[i] != NULL);
        args[i].ch   = chs[i];
        args[i].val  = (uintptr_t)(i + 1) * 0x10UL;
        joins[i]     = goc_go(p10_4_sender_fn, &args[i]);
        ASSERT(joins[i] != NULL);
    }

    goc_val_t** results = goc_take_all_sync(chs, P10_4_N);
    ASSERT(results != NULL);

    for (int i = 0; i < P10_4_N; i++) {
        ASSERT(results[i] != NULL);
        ASSERT(results[i]->ok == GOC_OK);
        ASSERT((uintptr_t)results[i]->val == args[i].val);
        /* Wait for the fiber to finish. */
        goc_val_t* jv = goc_take_sync(joins[i]);
        ASSERT(jv->ok == GOC_CLOSED);
    }

    TEST_PASS();
done:;
}

/* -------------------------------------------------------------------------
 * P10.5 — goc_take_all from fiber on pre-filled buffered channels
 *
 * Three buffered channels are pre-filled by the main thread.  A fiber
 * calls goc_take_all; the main thread waits for the result via a result
 * channel and verifies all values.
 * ---------------------------------------------------------------------- */

#define P10_5_N 3

typedef struct {
    goc_chan** chs;
    size_t     n;
    goc_chan*  result_ch;  /* fiber sends the results array back through here */
} p10_5_args_t;

static void p10_5_taker_fn(void* arg) {
    p10_5_args_t* a = (p10_5_args_t*)arg;
    goc_val_t** results = goc_take_all(a->chs, a->n);
    goc_put(a->result_ch, results);
}

static void test_p10_5(void) {
    TEST_BEGIN("P10.5 goc_take_all from fiber on buffered channels → all GOC_OK, correct values");

    goc_chan*    chs[P10_5_N];
    uintptr_t   vals[P10_5_N] = { 0xAAUL, 0xBBUL, 0xCCUL };

    for (int i = 0; i < P10_5_N; i++) {
        chs[i] = goc_chan_make(1);
        ASSERT(chs[i] != NULL);
        goc_status_t s = goc_put_sync(chs[i], (void*)vals[i]);
        ASSERT(s == GOC_OK);
    }

    goc_chan*   result_ch = goc_chan_make(1);
    ASSERT(result_ch != NULL);

    p10_5_args_t args = { .chs = chs, .n = P10_5_N, .result_ch = result_ch };
    goc_chan* join = goc_go(p10_5_taker_fn, &args);
    ASSERT(join != NULL);

    goc_val_t* rv = goc_take_sync(result_ch);
    ASSERT(rv != NULL);
    ASSERT(rv->ok == GOC_OK);

    goc_val_t** results = (goc_val_t**)rv->val;
    ASSERT(results != NULL);

    for (int i = 0; i < P10_5_N; i++) {
        ASSERT(results[i] != NULL);
        ASSERT(results[i]->ok == GOC_OK);
        ASSERT((uintptr_t)results[i]->val == vals[i]);
    }

    goc_val_t* jv = goc_take_sync(join);
    ASSERT(jv->ok == GOC_CLOSED);
    goc_close(result_ch);

    TEST_PASS();
done:;
}

/* -------------------------------------------------------------------------
 * P10.6 — goc_take_all from fiber blocks until a separate fiber sends
 *
 * Four rendezvous channels.  Four sender fibers are spawned first.
 * A taker fiber calls goc_take_all, parks on each channel in turn, and
 * sends the whole results array back via a result channel.  The main
 * thread verifies all four values.
 * ---------------------------------------------------------------------- */

#define P10_6_N 4

typedef struct {
    goc_chan** chs;
    size_t     n;
    goc_chan*  result_ch;
} p10_6_taker_args_t;

typedef struct {
    goc_chan* ch;
    uintptr_t val;
} p10_6_sender_args_t;

static void p10_6_sender_fn(void* arg) {
    p10_6_sender_args_t* a = (p10_6_sender_args_t*)arg;
    goc_put(a->ch, (void*)a->val);
}

static void p10_6_taker_fn(void* arg) {
    p10_6_taker_args_t* a = (p10_6_taker_args_t*)arg;
    goc_val_t** results = goc_take_all(a->chs, a->n);
    goc_put(a->result_ch, results);
}

static void test_p10_6(void) {
    TEST_BEGIN("P10.6 goc_take_all from fiber blocks until senders send → correct values");

    goc_chan*            chs[P10_6_N];
    goc_chan*            sender_joins[P10_6_N];
    p10_6_sender_args_t sender_args[P10_6_N];

    for (int i = 0; i < P10_6_N; i++) {
        chs[i]              = goc_chan_make(0);
        ASSERT(chs[i] != NULL);
        sender_args[i].ch   = chs[i];
        sender_args[i].val  = (uintptr_t)(0x100UL * (i + 1));
        sender_joins[i]     = goc_go(p10_6_sender_fn, &sender_args[i]);
        ASSERT(sender_joins[i] != NULL);
    }

    goc_chan* result_ch = goc_chan_make(1);
    ASSERT(result_ch != NULL);

    p10_6_taker_args_t taker_args = { .chs = chs, .n = P10_6_N, .result_ch = result_ch };
    goc_chan* taker_join = goc_go(p10_6_taker_fn, &taker_args);
    ASSERT(taker_join != NULL);

    goc_val_t* rv = goc_take_sync(result_ch);
    ASSERT(rv != NULL);
    ASSERT(rv->ok == GOC_OK);

    goc_val_t** results = (goc_val_t**)rv->val;
    ASSERT(results != NULL);

    for (int i = 0; i < P10_6_N; i++) {
        ASSERT(results[i] != NULL);
        ASSERT(results[i]->ok == GOC_OK);
        ASSERT((uintptr_t)results[i]->val == sender_args[i].val);
        goc_val_t* jv = goc_take_sync(sender_joins[i]);
        ASSERT(jv->ok == GOC_CLOSED);
    }

    goc_val_t* tjv = goc_take_sync(taker_join);
    ASSERT(tjv->ok == GOC_CLOSED);
    goc_close(result_ch);

    TEST_PASS();
done:;
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void) {
    install_crash_handler();

    printf("libgoc test suite — Phase 10: goc_take_all helpers\n");
    printf("====================================================\n\n");

    goc_init();

    printf("Phase 10 — goc_take_all / goc_take_all_sync\n");
    test_p10_1();
    test_p10_2();
    test_p10_3();
    test_p10_4();
    test_p10_5();
    test_p10_6();
    printf("\n");

    goc_shutdown();

    printf("====================================================\n");
    printf("Results: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed > 0)
        printf(", %d FAILED", g_tests_failed);
    printf("\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
