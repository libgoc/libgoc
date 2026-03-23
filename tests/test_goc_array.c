/*
 * tests/test_goc_array.c — goc_array: dynamic array tests
 *
 * Verifies the full goc_array API: construction, push/pop from both ends,
 * random access, grow-cycle correctness, concat, slice, and C-array interop.
 *
 * Build:  cmake -B build && cmake --build build
 * Run:    ctest --test-dir build --output-on-failure
 *         ./build/test_goc_array
 *
 * Compile requirements: -std=c11 -DGC_THREADS -D_GNU_SOURCE
 *
 * Test coverage:
 *
 *   A1   goc_array_make() returns a non-NULL, empty array
 *   A2   push / get / len — elements stored in correct order, length updated
 *   A3   set overwrites element at given index
 *   A4   pop returns tail elements in LIFO order; length decrements
 *   A5   push_head / pop_head — FIFO ordering from head
 *   A6   push + push_head across multiple grow/centering cycles
 *   A7   concat produces correct combined array; originals unchanged
 *   A8   slice produces correct subarray view; original unchanged
 *   A9   from / to_c C-array interop round-trip
 *   A10  to_c returns NULL for an empty array
 *   A11  concat / slice on empty arrays
 *   A12  goc_array_from with n=0 produces an empty array
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "test_harness.h"
#include "goc.h"
#include "goc_array.h"

/* =========================================================================
 * goc_array tests
 * ====================================================================== */

/*
 * A1 — goc_array_make() returns a non-NULL, empty array
 */
static void test_array_make(void) {
    TEST_BEGIN("A1   goc_array_make returns non-NULL empty array");
    goc_array* arr = goc_array_make(0);
    ASSERT(arr != NULL);
    ASSERT(goc_array_len(arr) == 0);
    TEST_PASS();
done:;
}

/*
 * A2 — goc_array_push / goc_array_get / goc_array_len
 *
 * Push several elements to the tail and verify they are retrievable in
 * order and that the length is updated correctly.
 */
static void test_array_push_get(void) {
    TEST_BEGIN("A2   push / get / len");
    goc_array* arr = goc_array_make(0);
    for (int i = 0; i < 16; i++) {
        goc_array_push(arr, goc_box_int(i));
    }
    ASSERT(goc_array_len(arr) == 16);
    for (int i = 0; i < 16; i++) {
        ASSERT(goc_unbox_int(goc_array_get(arr, (size_t)i)) == i);
    }
    TEST_PASS();
done:;
}

/*
 * A3 — goc_array_set: in-place element replacement
 */
static void test_array_set(void) {
    TEST_BEGIN("A3   set overwrites element at index");
    goc_array* arr = goc_array_make(0);
    goc_array_push(arr, goc_box_int(1));
    goc_array_push(arr, goc_box_int(2));
    goc_array_push(arr, goc_box_int(3));
    goc_array_set(arr, 1, goc_box_int(99));
    ASSERT(goc_unbox_int(goc_array_get(arr, 0)) == 1);
    ASSERT(goc_unbox_int(goc_array_get(arr, 1)) == 99);
    ASSERT(goc_unbox_int(goc_array_get(arr, 2)) == 3);
    TEST_PASS();
done:;
}

/*
 * A4 — goc_array_pop: tail removal returns correct values
 */
static void test_array_pop(void) {
    TEST_BEGIN("A4   pop returns tail elements in LIFO order");
    goc_array* arr = goc_array_make(0);
    goc_array_push(arr, goc_box_int(10));
    goc_array_push(arr, goc_box_int(20));
    goc_array_push(arr, goc_box_int(30));
    ASSERT(goc_unbox_int(goc_array_pop(arr)) == 30);
    ASSERT(goc_unbox_int(goc_array_pop(arr)) == 20);
    ASSERT(goc_unbox_int(goc_array_pop(arr)) == 10);
    ASSERT(goc_array_len(arr) == 0);
    TEST_PASS();
done:;
}

/*
 * A5 — goc_array_push_head / goc_array_pop_head
 *
 * Prepend elements and verify FIFO ordering via pop_head.
 */
static void test_array_push_pop_head(void) {
    TEST_BEGIN("A5   push_head / pop_head — FIFO from head");
    goc_array* arr = goc_array_make(0);
    goc_array_push_head(arr, goc_box_int(3));
    goc_array_push_head(arr, goc_box_int(2));
    goc_array_push_head(arr, goc_box_int(1));
    ASSERT(goc_array_len(arr) == 3);
    ASSERT(goc_unbox_int(goc_array_pop_head(arr)) == 1);
    ASSERT(goc_unbox_int(goc_array_pop_head(arr)) == 2);
    ASSERT(goc_unbox_int(goc_array_pop_head(arr)) == 3);
    ASSERT(goc_array_len(arr) == 0);
    TEST_PASS();
done:;
}

/*
 * A6 — Mixed push / push_head across multiple growth cycles
 *
 * Exercises both ends across enough insertions to trigger several
 * reallocation/centering cycles.
 */
static void test_array_grow(void) {
    TEST_BEGIN("A6   push+push_head across multiple grow cycles");
    goc_array* arr = goc_array_make(0);
    const int N = 64;
    /* interleave: push_head i, push i+N */
    for (int i = 0; i < N; i++) {
        goc_array_push_head(arr, goc_box_int(N - 1 - i));
        goc_array_push(arr, goc_box_int(N + i));
    }
    ASSERT(goc_array_len(arr) == (size_t)(N * 2));
    /* arr should now contain [0, 1, ..., 2N-1] */
    for (int i = 0; i < N * 2; i++) {
        ASSERT(goc_unbox_int(goc_array_get(arr, (size_t)i)) == i);
    }
    TEST_PASS();
done:;
}

/*
 * A7 — goc_array_concat
 */
static void test_array_concat(void) {
    TEST_BEGIN("A7   concat produces correct combined array");
    goc_array* a = goc_array_make(0);
    goc_array* b = goc_array_make(0);
    goc_array_push(a, goc_box_int(1));
    goc_array_push(a, goc_box_int(2));
    goc_array_push(b, goc_box_int(3));
    goc_array_push(b, goc_box_int(4));
    goc_array* c = goc_array_concat(a, b);
    ASSERT(goc_array_len(c) == 4);
    ASSERT(goc_unbox_int(goc_array_get(c, 0)) == 1);
    ASSERT(goc_unbox_int(goc_array_get(c, 1)) == 2);
    ASSERT(goc_unbox_int(goc_array_get(c, 2)) == 3);
    ASSERT(goc_unbox_int(goc_array_get(c, 3)) == 4);
    /* originals are unchanged */
    ASSERT(goc_array_len(a) == 2);
    ASSERT(goc_array_len(b) == 2);
    TEST_PASS();
done:;
}

/*
 * A8 — goc_array_slice: O(1) subarray view
 */
static void test_array_slice(void) {
    TEST_BEGIN("A8   slice produces correct subarray view");
    goc_array* arr = goc_array_make(0);
    for (int i = 0; i < 8; i++) {
        goc_array_push(arr, goc_box_int(i));
    }
    goc_array* s = goc_array_slice(arr, 2, 5);
    ASSERT(goc_array_len(s) == 3);
    ASSERT(goc_unbox_int(goc_array_get(s, 0)) == 2);
    ASSERT(goc_unbox_int(goc_array_get(s, 1)) == 3);
    ASSERT(goc_unbox_int(goc_array_get(s, 2)) == 4);
    /* original is unchanged */
    ASSERT(goc_array_len(arr) == 8);
    TEST_PASS();
done:;
}

/*
 * A9 — goc_array_from / goc_array_to_c interop
 */
static void test_array_c_interop(void) {
    TEST_BEGIN("A9   from/to_c C-array interop round-trip");
    void* src[4] = {
        goc_box_int(10),
        goc_box_int(20),
        goc_box_int(30),
        goc_box_int(40),
    };
    goc_array* arr = goc_array_from(src, 4);
    ASSERT(goc_array_len(arr) == 4);

    void** c = goc_array_to_c(arr);
    ASSERT(c != NULL);
    for (int i = 0; i < 4; i++) {
        ASSERT(goc_unbox_int(c[i]) == goc_unbox_int(src[i]));
    }
    TEST_PASS();
done:;
}

/*
 * A10 — goc_array_to_c returns NULL for an empty array
 */
static void test_array_to_c_empty(void) {
    TEST_BEGIN("A10  to_c returns NULL for empty array");
    goc_array* arr = goc_array_make(0);
    ASSERT(goc_array_to_c(arr) == NULL);
    TEST_PASS();
done:;
}

/*
 * A11 — empty-array edge cases for concat and slice
 */
static void test_array_empty_ops(void) {
    TEST_BEGIN("A11  concat / slice on empty arrays");
    goc_array* empty = goc_array_make(0);
    goc_array* a     = goc_array_make(0);
    goc_array_push(a, goc_box_int(42));

    goc_array* lhs = goc_array_concat(empty, a);
    goc_array* rhs = goc_array_concat(a, empty);
    ASSERT(goc_array_len(lhs) == 1);
    ASSERT(goc_unbox_int(goc_array_get(lhs, 0)) == 42);
    ASSERT(goc_array_len(rhs) == 1);
    ASSERT(goc_unbox_int(goc_array_get(rhs, 0)) == 42);

    goc_array* s = goc_array_slice(a, 0, 0);
    ASSERT(goc_array_len(s) == 0);
    TEST_PASS();
done:;
}

/*
 * A12 — goc_array_from with n==0 produces an empty array
 */
static void test_array_from_empty(void) {
    TEST_BEGIN("A12  goc_array_from with n=0 produces empty array");
    goc_array* arr = goc_array_from(NULL, 0);
    ASSERT(arr != NULL);
    ASSERT(goc_array_len(arr) == 0);
    ASSERT(goc_array_to_c(arr) == NULL);
    TEST_PASS();
done:;
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void) {
    install_crash_handler();

    printf("libgoc test suite — goc_array\n");
    printf("==============================\n\n");

    goc_init();

    test_array_make();
    test_array_push_get();
    test_array_set();
    test_array_pop();
    test_array_push_pop_head();
    test_array_grow();
    test_array_concat();
    test_array_slice();
    test_array_c_interop();
    test_array_to_c_empty();
    test_array_empty_ops();
    test_array_from_empty();

    goc_shutdown();

    printf("\n==============================\n");
    printf("Results: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed > 0) {
        printf(", %d FAILED", g_tests_failed);
    }
    printf("\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
