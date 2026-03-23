/*
 * tests/test_p1_foundation.c — Phase 1: Foundation tests for libgoc
 *
 * Verifies the core lifecycle and memory primitives that all subsequent phases
 * depend on.  These tests must pass before any channel or fiber tests are
 * attempted.
 *
 * Build:  cmake -B build && cmake --build build
 * Run:    ctest --test-dir build --output-on-failure
 *         ./build/test_p1_foundation
 *
 * Compile requirements: -std=c11 -DGC_THREADS -D_GNU_SOURCE
 *
 * Dependencies:
 *   - libgoc (goc.h)  — runtime under test
 *   - Boehm GC        — must be the threaded variant (bdw-gc-threaded);
 *                        initialised internally by goc_init()
 *   - libuv           — event loop; accessed only through goc_scheduler()
 *
 * Test coverage (Phase 1 — Foundation):
 *
 *   P1.1  goc_scheduler() returns a non-NULL uv_loop_t* after goc_init()
 *   P1.2  goc_scheduler() returns the identical pointer on repeated calls
 *         (the event loop is a singleton for the lifetime of the runtime)
 *   P1.3  goc_malloc() returns non-NULL and zero-initialises the allocation
 *         (memory is on the Boehm GC heap; callers need not free it)
 *   P1.4  goc_in_fiber() returns false when called from the main OS thread
 *         (the predicate is true only inside a fiber body)
 *
 * Notes:
 *   - goc_init() is called once in main() before any test runs.
 *   - goc_shutdown() is called once in main() after all tests complete;
 *     no goc_* function may be called after it returns.
 *   - The test harness uses a goto-based cleanup pattern: each test function
 *     has a single `done:` label that is the target of TEST_PASS / TEST_FAIL /
 *     ASSERT.  This keeps cleanup deterministic even when assertions fire.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "test_harness.h"
#include "goc.h"
#include "goc_array.h"

/* =========================================================================
 * Phase 1 — Foundation
 * ====================================================================== */

/*
 * P1.1 — goc_scheduler() returns non-NULL after goc_init()
 *
 * goc_scheduler() exposes the internal libuv event loop used by the runtime.
 * The pointer is valid from goc_init() until goc_shutdown() returns.  A
 * non-NULL return is the minimum proof that the loop was successfully created
 * and started.
 */
static void test_p1_1(void) {
    TEST_BEGIN("P1.1  goc_scheduler() non-NULL after goc_init");
    uv_loop_t* loop = goc_scheduler();
    ASSERT(loop != NULL);
    TEST_PASS();
done:;
}

/*
 * P1.2 — goc_scheduler() returns the identical pointer across repeated calls
 *
 * The runtime owns exactly one libuv event loop for its entire lifetime.
 * Callers may cache the pointer safely; this test asserts that the pointer
 * does not change between successive calls, which would indicate accidental
 * re-creation or re-initialisation of the loop.
 */
static void test_p1_2(void) {
    TEST_BEGIN("P1.2  goc_scheduler() pointer is stable across calls");
    uv_loop_t* a = goc_scheduler();
    uv_loop_t* b = goc_scheduler();
    ASSERT(a != NULL);
    ASSERT(a == b);
    TEST_PASS();
done:;
}

/*
 * P1.3 — goc_malloc() returns non-NULL and zero-initialises the allocation
 *
 * goc_malloc() wraps GC_MALLOC, which zero-initialises on allocation.  The
 * test allocates 64 bytes and walks the entire buffer checking that every byte
 * is 0.  The allocation is intentionally not freed: Boehm GC will collect it
 * when it becomes unreachable, which is the expected usage pattern.
 *
 * Note: goc_malloc() aborts the process on allocation failure, so a NULL
 * return is treated as a hard runtime error rather than a test failure.
 */
static void test_p1_3(void) {
    TEST_BEGIN("P1.3  goc_malloc returns non-NULL; memory is zero-initialised");
    const size_t SZ = 64;
    unsigned char* p = (unsigned char*)goc_malloc(SZ);
    ASSERT(p != NULL);
    for (size_t i = 0; i < SZ; i++) {
        ASSERT(p[i] == 0);
    }
    TEST_PASS();
done:;
}

/*
 * P1.4 — goc_in_fiber() returns false from the main OS thread
 *
 * goc_in_fiber() checks whether the current execution context is a libgoc
 * fiber (a stackful coroutine managed by minicoro).  When called from any
 * plain OS thread — including the main thread — it must return false.
 * The complementary assertion (true inside a fiber) is verified in P2.6.
 */
static void test_p1_4(void) {
    TEST_BEGIN("P1.4  goc_in_fiber() returns false from main thread");
    ASSERT(goc_in_fiber() == false);
    TEST_PASS();
done:;
}

/* =========================================================================
 * Phase 1 — Dynamic Array (goc_array)
 * ====================================================================== */

/*
 * P1.5 — goc_array_make() returns a non-NULL, empty array
 */
static void test_p1_5(void) {
    TEST_BEGIN("P1.5  goc_array_make returns non-NULL empty array");
    goc_array* arr = goc_array_make(0);
    ASSERT(arr != NULL);
    ASSERT(goc_array_len(arr) == 0);
    TEST_PASS();
done:;
}

/*
 * P1.6 — goc_array_push / goc_array_get / goc_array_len
 *
 * Push several elements to the tail and verify they are retrievable in
 * order and that the length is updated correctly.
 */
static void test_p1_6(void) {
    TEST_BEGIN("P1.6  push / get / len");
    goc_array* arr = goc_array_make(0);
    for (int i = 0; i < 16; i++) {
        goc_array_push(arr, (void*)(intptr_t)i);
    }
    ASSERT(goc_array_len(arr) == 16);
    for (int i = 0; i < 16; i++) {
        ASSERT((intptr_t)goc_array_get(arr, (size_t)i) == i);
    }
    TEST_PASS();
done:;
}

/*
 * P1.7 — goc_array_set: in-place element replacement
 */
static void test_p1_7(void) {
    TEST_BEGIN("P1.7  set overwrites element at index");
    goc_array* arr = goc_array_make(0);
    goc_array_push(arr, (void*)(intptr_t)1);
    goc_array_push(arr, (void*)(intptr_t)2);
    goc_array_push(arr, (void*)(intptr_t)3);
    goc_array_set(arr, 1, (void*)(intptr_t)99);
    ASSERT((intptr_t)goc_array_get(arr, 0) == 1);
    ASSERT((intptr_t)goc_array_get(arr, 1) == 99);
    ASSERT((intptr_t)goc_array_get(arr, 2) == 3);
    TEST_PASS();
done:;
}

/*
 * P1.8 — goc_array_pop: tail removal returns correct values
 */
static void test_p1_8(void) {
    TEST_BEGIN("P1.8  pop returns tail elements in LIFO order");
    goc_array* arr = goc_array_make(0);
    goc_array_push(arr, (void*)(intptr_t)10);
    goc_array_push(arr, (void*)(intptr_t)20);
    goc_array_push(arr, (void*)(intptr_t)30);
    ASSERT((intptr_t)goc_array_pop(arr) == 30);
    ASSERT((intptr_t)goc_array_pop(arr) == 20);
    ASSERT((intptr_t)goc_array_pop(arr) == 10);
    ASSERT(goc_array_len(arr) == 0);
    TEST_PASS();
done:;
}

/*
 * P1.9 — goc_array_push_head / goc_array_pop_head
 *
 * Prepend elements and verify FIFO ordering via pop_head.
 */
static void test_p1_9(void) {
    TEST_BEGIN("P1.9  push_head / pop_head — FIFO from head");
    goc_array* arr = goc_array_make(0);
    goc_array_push_head(arr, (void*)(intptr_t)3);
    goc_array_push_head(arr, (void*)(intptr_t)2);
    goc_array_push_head(arr, (void*)(intptr_t)1);
    ASSERT(goc_array_len(arr) == 3);
    ASSERT((intptr_t)goc_array_pop_head(arr) == 1);
    ASSERT((intptr_t)goc_array_pop_head(arr) == 2);
    ASSERT((intptr_t)goc_array_pop_head(arr) == 3);
    ASSERT(goc_array_len(arr) == 0);
    TEST_PASS();
done:;
}

/*
 * P1.10 — Mixed push / push_head across multiple growth cycles
 *
 * Exercises both ends across enough insertions to trigger several
 * reallocation/centering cycles.
 */
static void test_p1_10(void) {
    TEST_BEGIN("P1.10 push+push_head across multiple grow cycles");
    goc_array* arr = goc_array_make(0);
    const int N = 64;
    /* interleave: push_head i, push i+N */
    for (int i = 0; i < N; i++) {
        goc_array_push_head(arr, (void*)(intptr_t)(N - 1 - i));
        goc_array_push(arr, (void*)(intptr_t)(N + i));
    }
    ASSERT(goc_array_len(arr) == (size_t)(N * 2));
    /* arr should now contain [0, 1, ..., 2N-1] */
    for (int i = 0; i < N * 2; i++) {
        ASSERT((intptr_t)goc_array_get(arr, (size_t)i) == i);
    }
    TEST_PASS();
done:;
}

/*
 * P1.11 — goc_array_concat
 */
static void test_p1_11(void) {
    TEST_BEGIN("P1.11 concat produces correct combined array");
    goc_array* a = goc_array_make(0);
    goc_array* b = goc_array_make(0);
    goc_array_push(a, (void*)(intptr_t)1);
    goc_array_push(a, (void*)(intptr_t)2);
    goc_array_push(b, (void*)(intptr_t)3);
    goc_array_push(b, (void*)(intptr_t)4);
    goc_array* c = goc_array_concat(a, b);
    ASSERT(goc_array_len(c) == 4);
    ASSERT((intptr_t)goc_array_get(c, 0) == 1);
    ASSERT((intptr_t)goc_array_get(c, 1) == 2);
    ASSERT((intptr_t)goc_array_get(c, 2) == 3);
    ASSERT((intptr_t)goc_array_get(c, 3) == 4);
    /* originals are unchanged */
    ASSERT(goc_array_len(a) == 2);
    ASSERT(goc_array_len(b) == 2);
    TEST_PASS();
done:;
}

/*
 * P1.12 — goc_array_slice: O(1) subarray view
 */
static void test_p1_12(void) {
    TEST_BEGIN("P1.12 slice produces correct subarray view");
    goc_array* arr = goc_array_make(0);
    for (int i = 0; i < 8; i++) {
        goc_array_push(arr, (void*)(intptr_t)i);
    }
    goc_array* s = goc_array_slice(arr, 2, 5);
    ASSERT(goc_array_len(s) == 3);
    ASSERT((intptr_t)goc_array_get(s, 0) == 2);
    ASSERT((intptr_t)goc_array_get(s, 1) == 3);
    ASSERT((intptr_t)goc_array_get(s, 2) == 4);
    /* original is unchanged */
    ASSERT(goc_array_len(arr) == 8);
    TEST_PASS();
done:;
}

/*
 * P1.13 — goc_array_from / goc_array_to_c interop
 */
static void test_p1_13(void) {
    TEST_BEGIN("P1.13 from/to_c C-array interop round-trip");
    void* src[4] = {
        (void*)(intptr_t)10,
        (void*)(intptr_t)20,
        (void*)(intptr_t)30,
        (void*)(intptr_t)40,
    };
    goc_array* arr = goc_array_from(src, 4);
    ASSERT(goc_array_len(arr) == 4);

    void** c = goc_array_to_c(arr);
    ASSERT(c != NULL);
    for (int i = 0; i < 4; i++) {
        ASSERT((intptr_t)c[i] == (intptr_t)src[i]);
    }
    TEST_PASS();
done:;
}

/*
 * P1.14 — goc_array_to_c returns NULL for an empty array
 */
static void test_p1_14(void) {
    TEST_BEGIN("P1.14 to_c returns NULL for empty array");
    goc_array* arr = goc_array_make(0);
    ASSERT(goc_array_to_c(arr) == NULL);
    TEST_PASS();
done:;
}

/*
 * P1.15 — empty-array edge cases for concat and slice
 */
static void test_p1_15(void) {
    TEST_BEGIN("P1.15 concat / slice on empty arrays");
    goc_array* empty = goc_array_make(0);
    goc_array* a     = goc_array_make(0);
    goc_array_push(a, (void*)(intptr_t)42);

    goc_array* lhs = goc_array_concat(empty, a);
    goc_array* rhs = goc_array_concat(a, empty);
    ASSERT(goc_array_len(lhs) == 1);
    ASSERT((intptr_t)goc_array_get(lhs, 0) == 42);
    ASSERT(goc_array_len(rhs) == 1);
    ASSERT((intptr_t)goc_array_get(rhs, 0) == 42);

    goc_array* s = goc_array_slice(a, 0, 0);
    ASSERT(goc_array_len(s) == 0);
    TEST_PASS();
done:;
}

/*
 * P1.16 — goc_array_from with n==0 produces an empty array
 */
static void test_p1_16(void) {
    TEST_BEGIN("P1.16 goc_array_from with n=0 produces empty array");
    goc_array* arr = goc_array_from(NULL, 0);
    ASSERT(arr != NULL);
    ASSERT(goc_array_len(arr) == 0);
    ASSERT(goc_array_to_c(arr) == NULL);
    TEST_PASS();
done:;
}

/* =========================================================================
 * main
 *
 * Initialises the runtime once, runs all Phase 1 tests in order, shuts down
 * the runtime, then prints a summary and exits with 0 on success or 1 if any
 * test failed.
 * ====================================================================== */

int main(void) {
    install_crash_handler();

    printf("libgoc test suite — Phase 1: Foundation\n");
    printf("=========================================\n\n");

    goc_init();

    printf("Phase 1 — Foundation\n");
    test_p1_1();
    test_p1_2();
    test_p1_3();
    test_p1_4();
    printf("\n");

    printf("Phase 1 — Dynamic Array\n");
    test_p1_5();
    test_p1_6();
    test_p1_7();
    test_p1_8();
    test_p1_9();
    test_p1_10();
    test_p1_11();
    test_p1_12();
    test_p1_13();
    test_p1_14();
    test_p1_15();
    test_p1_16();
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
