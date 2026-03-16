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

    goc_shutdown();

    printf("=========================================\n");
    printf("Results: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed > 0) {
        printf(", %d FAILED", g_tests_failed);
    }
    printf("\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
