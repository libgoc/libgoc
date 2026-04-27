/* tests/test_goc_dict.c — goc_dict ordered dictionary tests
 *
 * Verifies the full goc_dict API: construction, lookup, mutation, copy,
 * merge, select, zip, and insertion-order iteration.
 *
 * Build:  cmake -B build && cmake --build build
 * Run:    ctest --test-dir build --output-on-failure
 *         ./build/test_goc_dict
 *
 * Compile requirements: -std=c11 -DGC_THREADS -D_GNU_SOURCE
 *
 * Test coverage:
 *
 *   D1   goc_dict_make() returns a non-NULL, empty dict
 *   D2   goc_dict_set / goc_dict_get retrieve inserted boxed values
 *   D3   goc_dict_contains returns membership correctly
 *   D4   goc_dict_set overwrites existing key without changing len
 *   D5   goc_dict_pop removes entries and returns values
 *   D6   goc_dict_len tracks live entries correctly
 *   D7   goc_dict_entries preserves insertion order
 *   D7a  goc_dict_keys returns live keys in insertion order
 *   D7b  goc_dict_vals returns live values in insertion order
 *   D8   goc_dict_entries / goc_dict_from_entries round-trip
 *   D9   goc_dict_copy returns an independent copy
 *   D10  goc_dict_merge merges dictionaries with later entries winning
 *   D11  goc_dict_merge handles multiple dictionaries
 *   D12  goc_dict_merge() with zero args returns empty dict
 *   D13  goc_dict_select preserves the requested insertion order
 *   D14  goc_dict_zip builds a dictionary from parallel arrays
 *   D15  goc_dict_zip_c builds a dictionary from C arrays
 *   D16  boxed macros round-trip scalar values correctly
 *   D17  goc_dict handles tombstone slots correctly
 *   D18  goc_dict resizes and preserves entries across rehashes
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "test_harness.h"
#include "goc.h"
#include "goc_array.h"
#include "goc_dict.h"

/* D1 — goc_dict_make returns an empty dict */
static void test_dict_make(void) {
    TEST_BEGIN("D1   goc_dict_make returns non-NULL empty dict");
    goc_dict* d = goc_dict_make(0);
    ASSERT(d != NULL);
    ASSERT(goc_dict_len(d) == 0);
    ASSERT(!goc_dict_contains(d, "missing"));
    TEST_PASS();
done:;
}

/* D2 — goc_dict_set / goc_dict_get stores and retrieves boxed values */
static void test_dict_set_get(void) {
    TEST_BEGIN("D2   goc_dict_set / goc_dict_get retrieve inserted values");
    goc_dict* d = goc_dict_make(0);
    goc_dict_set_boxed(int, d, "x", 1);
    goc_dict_set_boxed(int, d, "y", 2);
    ASSERT(goc_dict_get_unboxed(int, d, "x", -1) == 1);
    ASSERT(goc_dict_get_unboxed(int, d, "y", -1) == 2);
    ASSERT(goc_dict_get_unboxed(int, d, "z", -1) == -1);
    TEST_PASS();
done:;
}

/* D3 — goc_dict_contains returns membership correctly */
static void test_dict_contains(void) {
    TEST_BEGIN("D3   goc_dict_contains returns membership correctly");
    goc_dict* d = goc_dict_make(0);
    goc_dict_set_boxed(int, d, "a", 1);
    ASSERT(goc_dict_contains(d, "a"));
    ASSERT(!goc_dict_contains(d, "b"));
    TEST_PASS();
done:;
}

/* D4 — goc_dict_set overwrites the existing value in place */
static void test_dict_overwrite(void) {
    TEST_BEGIN("D4   goc_dict_set overwrites existing key without changing len");
    goc_dict* d = goc_dict_make(0);
    goc_dict_set_boxed(int, d, "a", 1);
    ASSERT(goc_dict_len(d) == 1);
    goc_dict_set_boxed(int, d, "a", 2);
    ASSERT(goc_dict_len(d) == 1);
    ASSERT(goc_dict_get_unboxed(int, d, "a", -1) == 2);
    TEST_PASS();
done:;
}

/* D5 — goc_dict_pop returns boxed values and removes entries */
static void test_dict_pop(void) {
    TEST_BEGIN("D5   goc_dict_pop removes entries and returns values");
    goc_dict* d = goc_dict_make(0);
    goc_dict_set_boxed(int, d, "x", 42);
    ASSERT(goc_dict_pop_unboxed(int, d, "x", -1) == 42);
    ASSERT(!goc_dict_contains(d, "x"));
    ASSERT(goc_dict_len(d) == 0);
    ASSERT(goc_dict_pop_unboxed(int, d, "x", -1) == -1);
    TEST_PASS();
done:;
}

/* D6 — goc_dict_len tracks live entries correctly */
static void test_dict_len(void) {
    TEST_BEGIN("D6   goc_dict_len increments on insert and decrements on pop");
    goc_dict* d = goc_dict_make(0);
    ASSERT(goc_dict_len(d) == 0);
    goc_dict_set_boxed(int, d, "a", 1);
    ASSERT(goc_dict_len(d) == 1);
    goc_dict_set_boxed(int, d, "b", 2);
    ASSERT(goc_dict_len(d) == 2);
    goc_dict_pop(d, "a", NULL);
    ASSERT(goc_dict_len(d) == 1);
    TEST_PASS();
done:;
}

/* D7 — goc_dict_entries preserves insertion order */
static void test_dict_insertion_order(void) {
    TEST_BEGIN("D7   goc_dict_entries preserves insertion order");
    goc_dict* d = goc_dict_make(0);
    goc_dict_set_boxed(int, d, "first", 1);
    goc_dict_set_boxed(int, d, "second", 2);
    goc_dict_set_boxed(int, d, "third", 3);

    goc_array* entries = goc_dict_entries(d);
    ASSERT(goc_array_len(entries) == 3);
    goc_dict_entry_t* first = (goc_dict_entry_t*)goc_array_get(entries, 0);
    goc_dict_entry_t* second = (goc_dict_entry_t*)goc_array_get(entries, 1);
    goc_dict_entry_t* third = (goc_dict_entry_t*)goc_array_get(entries, 2);
    ASSERT(strcmp(first->key, "first") == 0);
    ASSERT(goc_unbox(int, first->val) == 1);
    ASSERT(strcmp(second->key, "second") == 0);
    ASSERT(goc_unbox(int, second->val) == 2);
    ASSERT(strcmp(third->key, "third") == 0);
    ASSERT(goc_unbox(int, third->val) == 3);
    TEST_PASS();
done:;
}

/* D7a — goc_dict_keys returns live keys in insertion order */
static void test_dict_keys(void) {
    TEST_BEGIN("D7a  goc_dict_keys returns live keys in insertion order");
    goc_dict* d = goc_dict_make(0);
    goc_dict_set_boxed(int, d, "first", 1);
    goc_dict_set_boxed(int, d, "second", 2);
    goc_dict_set_boxed(int, d, "third", 3);
    goc_dict_pop(d, "second", NULL);

    goc_array* keys = goc_dict_keys(d);
    ASSERT(goc_array_len(keys) == 2);
    ASSERT(strcmp((char*)goc_array_get(keys, 0), "first") == 0);
    ASSERT(strcmp((char*)goc_array_get(keys, 1), "third") == 0);
    TEST_PASS();
done:;
}

/* D7b — goc_dict_vals returns live values in insertion order */
static void test_dict_vals(void) {
    TEST_BEGIN("D7b  goc_dict_vals returns live values in insertion order of keys");
    goc_dict* d = goc_dict_make(0);
    goc_dict_set_boxed(int, d, "first", 1);
    goc_dict_set_boxed(int, d, "second", 2);
    goc_dict_set_boxed(int, d, "third", 3);
    goc_dict_pop(d, "second", NULL);

    goc_array* vals = goc_dict_vals(d);
    ASSERT(goc_array_len(vals) == 2);
    ASSERT(goc_unbox(int, goc_array_get(vals, 0)) == 1);
    ASSERT(goc_unbox(int, goc_array_get(vals, 1)) == 3);
    TEST_PASS();
done:;
}

/* D8 — goc_dict_entries / goc_dict_from_entries round-trip */
static void test_dict_to_from_array(void) {
    TEST_BEGIN("D8   goc_dict_entries / goc_dict_from_entries round-trip");
    goc_dict* d = goc_dict_make(0);
    goc_dict_set_boxed(int, d, "x", 100);
    goc_dict_set_boxed(int, d, "y", 200);

    goc_array* entries = goc_dict_entries(d);
    goc_dict* copy = goc_dict_from_entries(entries);

    ASSERT(goc_dict_len(copy) == 2);
    ASSERT(goc_dict_get_unboxed(int, copy, "x", -1) == 100);
    ASSERT(goc_dict_get_unboxed(int, copy, "y", -1) == 200);
    TEST_PASS();
done:;
}

/* D9 — goc_dict_copy returns an independent copy of the dictionary */
static void test_dict_copy(void) {
    TEST_BEGIN("D9   goc_dict_copy returns independent copy");
    goc_dict* d = goc_dict_make(0);
    goc_dict_set_boxed(int, d, "a", 1);
    goc_dict_set_boxed(int, d, "b", 2);

    goc_dict* copy = goc_dict_copy(d);
    ASSERT(goc_dict_len(copy) == 2);
    goc_dict_set_boxed(int, copy, "b", 3);
    ASSERT(goc_dict_get_unboxed(int, copy, "b", -1) == 3);
    ASSERT(goc_dict_get_unboxed(int, d, "b", -1) == 2);
    TEST_PASS();
done:;
}

/* D10 — goc_dict_merge merges dictionaries with later entries winning */
static void test_dict_merge(void) {
    TEST_BEGIN("D10  goc_dict_merge merges dicts with later entries winning");
    goc_dict* a = goc_dict_of_boxed(int, {"a", 1}, {"b", 2});
    goc_dict* b = goc_dict_of_boxed(int, {"b", 3}, {"c", 4});
    goc_dict* merged = goc_dict_merge(a, b);

    ASSERT(goc_dict_len(merged) == 3);
    ASSERT(goc_dict_get_unboxed(int, merged, "a", -1) == 1);
    ASSERT(goc_dict_get_unboxed(int, merged, "b", -1) == 3);
    ASSERT(goc_dict_get_unboxed(int, merged, "c", -1) == 4);
    TEST_PASS();
done:;
}

/* D11 — goc_dict_merge handles multiple dictionaries */
static void test_dict_merge_many(void) {
    TEST_BEGIN("D11  goc_dict_merge handles multiple dictionaries");
    goc_dict* a = goc_dict_of_boxed(int, {"x", 1});
    goc_dict* b = goc_dict_of_boxed(int, {"y", 2});
    goc_dict* c = goc_dict_of_boxed(int, {"x", 3});

    goc_dict* merged = goc_dict_merge(a, b, c);
    ASSERT(goc_dict_len(merged) == 2);
    ASSERT(goc_dict_get_unboxed(int, merged, "x", -1) == 3);
    ASSERT(goc_dict_get_unboxed(int, merged, "y", -1) == 2);
    TEST_PASS();
done:;
}

/* D12 — goc_dict_merge returns an empty dict when called with zero args */
static void test_dict_merge_empty(void) {
    TEST_BEGIN("D12  goc_dict_merge() with zero args returns empty dict");
    goc_dict* merged = goc_dict_merge();
    ASSERT(goc_dict_len(merged) == 0);
    TEST_PASS();
done:;
}

/* D13 — goc_dict_select preserves the requested insertion order */
static void test_dict_select(void) {
    TEST_BEGIN("D13  goc_dict_select returns a dict with requested keys");
    goc_dict* d = goc_dict_of_boxed(int, {"a", 1}, {"b", 2}, {"c", 3});
    goc_dict* selected = goc_dict_select(d, "c", "a");
    ASSERT(goc_dict_len(selected) == 2);
    ASSERT(goc_dict_get_unboxed(int, selected, "c", -1) == 3);
    ASSERT(goc_dict_get_unboxed(int, selected, "a", -1) == 1);
    TEST_PASS();
done:;
}

/* D14 — goc_dict_zip builds a dictionary from parallel arrays */
static void test_dict_zip(void) {
    TEST_BEGIN("D14  goc_dict_zip builds a dict from array pairs");
    goc_array* keys = goc_array_of("foo", "bar");
    goc_array* vals = goc_array_of("FOO", "BAR");
    goc_dict* d = goc_dict_zip(keys, vals);
    ASSERT(goc_dict_len(d) == 2);
    ASSERT(strcmp((char*)goc_dict_get(d, "foo", NULL), "FOO") == 0);
    ASSERT(strcmp((char*)goc_dict_get(d, "bar", NULL), "BAR") == 0);
    TEST_PASS();
done:;
}

/* D15 — goc_dict_zip_c builds a dictionary from C arrays */
static void test_dict_zip_c(void) {
    TEST_BEGIN("D15  goc_dict_zip_c builds a dict from C arrays");
    const char* keys[] = {"a", "b"};
    void* vals[] = {goc_box(int, 10), goc_box(int, 20)};
    goc_dict* d = goc_dict_zip_c(keys, vals, 2);
    ASSERT(goc_dict_len(d) == 2);
    ASSERT(goc_dict_get_unboxed(int, d, "a", -1) == 10);
    ASSERT(goc_dict_get_unboxed(int, d, "b", -1) == 20);
    TEST_PASS();
done:;
}

/* D16 — boxed macros round-trip scalar values */
static void test_dict_boxed_macros(void) {
    TEST_BEGIN("D16  boxed macros round-trip scalars correctly");
    goc_dict* d = goc_dict_make(0);
    goc_dict_set_boxed(int, d, "x", 42);
    ASSERT(goc_dict_get_unboxed(int, d, "x", -1) == 42);
    ASSERT(goc_dict_pop_unboxed(int, d, "x", -1) == 42);
    ASSERT(goc_dict_pop_unboxed(int, d, "x", -1) == -1);
    TEST_PASS();
done:;
}

/* D17 — goc_dict handles tombstone slots correctly */
static void test_dict_tombstone_probe(void) {
    TEST_BEGIN("D17  pop and reinsert works across tombstone slots");
    goc_dict* d = goc_dict_make(8);
    goc_dict_set_boxed(int, d, "a", 1);
    goc_dict_set_boxed(int, d, "b", 2);
    goc_dict_pop(d, "a", NULL);
    ASSERT(!goc_dict_contains(d, "a"));
    goc_dict_set_boxed(int, d, "a", 3);
    ASSERT(goc_dict_contains(d, "a"));
    ASSERT(goc_dict_get_unboxed(int, d, "a", -1) == 3);
    TEST_PASS();
done:;
}

/* D18 — goc_dict resizes and preserves entries across rehashes */
static void test_dict_resize(void) {
    TEST_BEGIN("D18  goc_dict resizes and preserves all entries");
    goc_dict* d = goc_dict_make(0);
    for (int i = 0; i < 32; i++) {
        char key[16];
        sprintf(key, "k%d", i);
        goc_dict_set_boxed(int, d, goc_sprintf("%s", key), i);
    }

    for (int i = 0; i < 32; i++) {
        char key[16];
        sprintf(key, "k%d", i);
        ASSERT(goc_dict_get_unboxed(int, d, goc_sprintf("%s", key), -1) == i);
    }

    ASSERT(goc_dict_len(d) == 32);
    TEST_PASS();
done:;
}

/* D19 — goc_dict_get_in supports deep dict/array lookup */
static void test_dict_get_in(void) {
    TEST_BEGIN("D19  goc_dict_get_in supports deep dict/array lookup");

    goc_dict* item0 = goc_dict_of_boxed(int, {"id", 100});
    goc_dict* item1 = goc_dict_of_boxed(int, {"id", 200});
    goc_array* items = goc_array_of(item0, item1);
    goc_dict* cfg = goc_dict_of("items", items);

    ASSERT(goc_unbox(int, goc_dict_get_in(cfg, ".items.[0].id", goc_box(int, -1))) == 100);
    ASSERT(goc_unbox(int, goc_dict_get_in(cfg, ".items.[1].id", goc_box(int, -1))) == 200);
    ASSERT(goc_dict_get_in(cfg, "", NULL) == cfg);

    ASSERT(goc_unbox(int, goc_dict_get_in(cfg, ".missing", goc_box(int, -1))) == -1);
    ASSERT(goc_unbox(int, goc_dict_get_in(cfg, ".items.[2]", goc_box(int, -1))) == -1);
    ASSERT(goc_unbox(int, goc_dict_get_in(cfg, "a.b", goc_box(int, -1))) == -1);
    ASSERT(goc_unbox(int, goc_dict_get_in(cfg, ".items.[abc]", goc_box(int, -1))) == -1);
    ASSERT(goc_unbox(int, goc_dict_get_in(cfg, ".items.[1", goc_box(int, -1))) == -1);

    goc_dict* null_dict = goc_dict_of("a", NULL);
    ASSERT(goc_dict_get_in(null_dict, ".a", goc_box(int, -1)) == NULL);
    ASSERT(goc_unbox(int, goc_dict_get_in(null_dict, ".a.b", goc_box(int, -1))) == -1);

    ASSERT(goc_unbox(int, goc_dict_get_in(NULL, ".a", goc_box(int, -1))) == -1);
    ASSERT(goc_unbox(int, goc_dict_get_in(cfg, NULL, goc_box(int, -1))) == -1);
    TEST_PASS();
done:;
}

int main(void) {
    goc_init();

    test_dict_make();
    test_dict_set_get();
    test_dict_contains();
    test_dict_overwrite();
    test_dict_pop();
    test_dict_len();
    test_dict_insertion_order();
    test_dict_keys();
    test_dict_vals();
    test_dict_to_from_array();
    test_dict_copy();
    test_dict_merge();
    test_dict_merge_many();
    test_dict_merge_empty();
    test_dict_select();
    test_dict_zip();
    test_dict_zip_c();
    test_dict_boxed_macros();
    test_dict_tombstone_probe();
    test_dict_resize();
    test_dict_get_in();

    goc_shutdown();

    REPORT(g_tests_run, g_tests_passed, g_tests_failed);
    return (g_tests_failed == 0) ? 0 : 1;
}
