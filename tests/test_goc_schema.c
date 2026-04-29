/* tests/test_goc_schema.c — goc_schema validation tests
 *
 * Verifies schema construction, recursive refs, object/array constraints,
 * and registry helpers.
 *
 * Test coverage:
 *   SC1   schema registry add and get
 *   SC2   schema hierarchy (derive / is_a)
 *   SC3   scalar schema type checks
 *   SC4   goc_schema_check invalid value aborts
 *   SC5   boxed scalar alias matrix
 *   SC6   scalar const schema checks
 *   SC7   string length and pattern constraints
 *   SC8   numeric constraint boundaries
 *   SC9   complex number schema support
 *   SC10  string format validation
 *   SC11  array length constraint
 *   SC12  homogeneous and bounded arrays
 *   SC13  array unique value equality
 *   SC14  array contains semantics
 *   SC15  tuple schema behavior
 *   SC16  object optionals and property count boundaries
 *   SC17  object propertyNames and patternProps
 *   SC18  object dependency schemas
 *   SC19  object patternProps regex caching
 *   SC20  object strict mode and required fields
 *   SC21  conditional and composition schemas
 *   SC22  ref and metadata behavior
 *   SC23  error path construction and nested composite schemas
 *   SC24  recursive schema reference
 *   SC25  public schema hierarchy helpers
 *   SC26  predicate schema
 *   SC27  global registry — built-in entries and user registration
 *
 * Build:  cmake -B build && cmake --build build
 * Run:   ctest --test-dir build --output-on-failure
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <complex.h>
#include <math.h>
#include <string.h>

#include "test_harness.h"
#include "goc.h"
#include "goc_array.h"
#include "goc_dict.h"
#include "goc_schema.h"

static void test_schema_registry(void) {
    TEST_BEGIN("SC1   schema registry add and get");
    goc_schema_registry* reg = goc_schema_registry_make();
    goc_schema* schema = goc_schema_int_min(0);

    goc_schema_registry_add(reg, "positiveInt", schema);
    ASSERT(goc_schema_registry_get(reg, "positiveInt") == schema);
    ASSERT(goc_schema_registry_get(reg, "missing") == NULL);

    TEST_PASS();

done:;
}

static void test_schema_hierarchy(void) {
    TEST_BEGIN("SC2   schema hierarchy (derive / is_a)");

    /* reflexive */
    ASSERT(goc_schema_is_a(goc_schema_int(), goc_schema_int()));
    ASSERT(goc_schema_is_a(goc_schema_any(), goc_schema_any()));

    /* direct builtin numeric sub-hierarchy */
    ASSERT(goc_schema_is_a(goc_schema_byte(), goc_schema_int()));
    ASSERT(goc_schema_is_a(goc_schema_bool(), goc_schema_byte()));
    ASSERT(goc_schema_is_a(goc_schema_ubyte(), goc_schema_byte()));
    ASSERT(goc_schema_is_a(goc_schema_uint(), goc_schema_int()));
    ASSERT(goc_schema_is_a(goc_schema_int(), goc_schema_real()));
    ASSERT(goc_schema_is_a(goc_schema_complex(), goc_schema_number()));
    ASSERT(goc_schema_is_a(goc_schema_real(), goc_schema_complex()));
    ASSERT(goc_schema_is_a(goc_schema_int(), goc_schema_number()));

    /* transitive */
    ASSERT(goc_schema_is_a(goc_schema_byte(), goc_schema_number()));
    ASSERT(goc_schema_is_a(goc_schema_byte(), goc_schema_any()));
    ASSERT(goc_schema_is_a(goc_schema_real(), goc_schema_any()));
    ASSERT(goc_schema_is_a(goc_schema_str(), goc_schema_any()));
    ASSERT(goc_schema_is_a(goc_schema_bool(), goc_schema_any()));
    ASSERT(goc_schema_is_a(goc_schema_number(), goc_schema_any()));

    /* non-relationships */
    ASSERT(!goc_schema_is_a(goc_schema_int(), goc_schema_byte()));
    ASSERT(!goc_schema_is_a(goc_schema_real(), goc_schema_int()));
    ASSERT(!goc_schema_is_a(goc_schema_str(), goc_schema_number()));
    ASSERT(!goc_schema_is_a(goc_schema_any(), goc_schema_int()));

    /* NULL safety */
    ASSERT(!goc_schema_is_a(NULL, goc_schema_number()));
    ASSERT(!goc_schema_is_a(goc_schema_int(), NULL));

    /* user-declared relation */
    goc_schema* my_id_schema = goc_schema_any_of(goc_schema_int(), goc_schema_str());
    goc_schema_derive(my_id_schema, goc_schema_any());
    ASSERT(goc_schema_is_a(my_id_schema, goc_schema_any()));
    ASSERT(!goc_schema_is_a(my_id_schema, goc_schema_int()));

    TEST_PASS();

done:;
}

#if !defined(_WIN32)
#  include <sys/wait.h>

static void schema_check_abort_child(void* arg) {
    (void)arg;
    goc_schema_check(goc_schema_int(), goc_box(double, 42.0));
}
#endif

static void test_schema_scalar_types(void) {
    TEST_BEGIN("SC3   scalar schema type checks");
    goc_schema_error* err;

    ASSERT(goc_schema_is_valid(goc_schema_bool(), goc_box(bool, true)));

    ASSERT(goc_schema_is_valid(goc_schema_int(), goc_box(int64_t, 42)));

    err = goc_schema_validate(goc_schema_int(), goc_box(double, 42.0));
    ASSERT(err != NULL);
    ASSERT(strcmp(goc_schema_error_message(err), "expected int, got real") == 0);

    ASSERT(goc_schema_is_valid(goc_schema_number(), goc_box(int64_t, 42)));
    ASSERT(goc_schema_is_valid(goc_schema_number(), goc_box(unsigned int, 42u)));
    ASSERT(goc_schema_is_valid(goc_schema_number(), goc_box(char, 'A')));
    ASSERT(goc_schema_is_valid(goc_schema_number(), goc_box(unsigned char, 255)));
    ASSERT(goc_schema_is_valid(goc_schema_number(), goc_box(double, 1.0)));
    ASSERT(!goc_schema_is_valid(goc_schema_number(), "hello"));
    ASSERT(!goc_schema_is_valid(goc_schema_number(), goc_box(bool, true)));

    TEST_PASS();

done:;
}

static void test_schema_check_abort(void) {
    TEST_BEGIN("SC4   goc_schema_check invalid value aborts");

#if defined(_WIN32)
    TEST_SKIP("abort tests are skipped on Windows");
#else
    ASSERT(goc_schema_is_valid(goc_schema_int(), goc_box(int64_t, 42)));
    bool got_sigabrt = fork_expect_sigabrt(schema_check_abort_child, NULL);
    ASSERT(got_sigabrt);
    TEST_PASS();
#endif

done:;
}

static void test_schema_boxed_scalar_aliases(void) {
    TEST_BEGIN("SC5   boxed scalar alias matrix");
    goc_schema_error* err;

    ASSERT(goc_schema_is_valid(goc_schema_byte(), goc_box(char, 'A')));
    ASSERT(goc_schema_is_valid(goc_schema_byte(), goc_box(signed char, -1)));
    err = goc_schema_validate(goc_schema_byte(), goc_box(unsigned char, 255));
    ASSERT(err != NULL);

    ASSERT(goc_schema_is_valid(goc_schema_ubyte(), goc_box(unsigned char, 255)));
    err = goc_schema_validate(goc_schema_ubyte(), goc_box(char, 'A'));
    ASSERT(err != NULL);
    err = goc_schema_validate(goc_schema_ubyte(), goc_box(unsigned int, 42u));
    ASSERT(err != NULL);

    ASSERT(goc_schema_is_valid(goc_schema_int(), goc_box(int, -42)));
    ASSERT(goc_schema_is_valid(goc_schema_int(), goc_box(int64_t, INT64_MIN)));
    ASSERT(goc_schema_is_valid(goc_schema_int(), goc_box(long, (long)-42)));
    ASSERT(goc_schema_is_valid(goc_schema_int(), goc_box(char, 'A')));
    err = goc_schema_validate(goc_schema_int(), goc_box(unsigned int, 42u));
    ASSERT(err != NULL);
    err = goc_schema_validate(goc_schema_int(), "hello");
    ASSERT(err != NULL);

    ASSERT(goc_schema_is_valid(goc_schema_uint(), goc_box(unsigned int, 42u)));
    ASSERT(goc_schema_is_valid(goc_schema_uint(), goc_box(unsigned long long, UINT64_MAX)));
    ASSERT(goc_schema_is_valid(goc_schema_uint(), goc_box(unsigned char, 255)));
    err = goc_schema_validate(goc_schema_uint(), goc_box(int, -1));
    ASSERT(err != NULL);
    ASSERT(!goc_schema_is_valid(goc_schema_uint(), "hello"));


    ASSERT(goc_schema_is_valid(goc_schema_real(), goc_box(float, 1.5f)));
    ASSERT(goc_schema_is_valid(goc_schema_real(), goc_box(long double, 1.5L)));

    TEST_PASS();

done:;
}

static void test_schema_scalar_const(void) {
    TEST_BEGIN("SC6   scalar const schema checks");
    goc_schema_error* err;

    ASSERT(goc_schema_is_valid(goc_schema_bool_const(true), goc_box(bool, true)));

    err = goc_schema_validate(goc_schema_bool_const(true), goc_box(bool, false));
    ASSERT(err != NULL);
    ASSERT(strstr(goc_schema_error_message(err), "expected const value true") != NULL);

    ASSERT(goc_schema_is_valid(goc_schema_int_const(42), goc_box(int64_t, 42)));

    err = goc_schema_validate(goc_schema_int_const(42), goc_box(int64_t, 43));
    ASSERT(err != NULL);
    ASSERT(strstr(goc_schema_error_message(err), "expected const value 42") != NULL);

    ASSERT(goc_schema_is_valid(goc_schema_real_const(1.5), goc_box(double, 1.5)));

    err = goc_schema_validate(goc_schema_real_const(1.5), goc_box(double, 1.6));
    ASSERT(err != NULL);
    ASSERT(strstr(goc_schema_error_message(err), "expected const value 1.5") != NULL);

    ASSERT(goc_schema_is_valid(goc_schema_str_const("hello"), "hello"));

    err = goc_schema_validate(goc_schema_str_const("hello"), "world");
    ASSERT(err != NULL);
    ASSERT(strstr(goc_schema_error_message(err), "expected const value hello") != NULL);

    TEST_PASS();

done:;
}

static void test_schema_str_len_and_pattern(void) {
    TEST_BEGIN("SC7   string length and pattern constraints");
    goc_schema_error* err;

    ASSERT(goc_schema_is_valid(goc_schema_str_len(3, 5), "hello"));

    err = goc_schema_validate(goc_schema_str_len(3, 5), "hi");
    ASSERT(err != NULL);
    ASSERT(strstr(goc_schema_error_message(err), "expected minimum") != NULL);

    ASSERT(goc_schema_is_valid(goc_schema_str_pattern("^[a-z]+$"), "abc"));
    ASSERT(!goc_schema_is_valid(goc_schema_str_pattern("^[a-z]+$"), "abc123"));

    TEST_PASS();

done:;
}

static void test_schema_numeric_constraints(void) {
    TEST_BEGIN("SC8   numeric constraint boundaries");
    goc_schema_error* err;

    ASSERT(goc_schema_is_valid(goc_schema_int_min(0), goc_box(int64_t, 0)));
    err = goc_schema_validate(goc_schema_int_min(0), goc_box(int64_t, -1));
    ASSERT(err != NULL);
    ASSERT(strstr(goc_schema_error_message(err), "expected minimum 0") != NULL);

    ASSERT(goc_schema_is_valid(goc_schema_int_max(5), goc_box(int64_t, 5)));
    err = goc_schema_validate(goc_schema_int_max(5), goc_box(int64_t, 6));
    ASSERT(err != NULL);
    ASSERT(strstr(goc_schema_error_message(err), "expected maximum 5") != NULL);

    ASSERT(goc_schema_is_valid(goc_schema_int_range(1, 3), goc_box(int64_t, 1)));
    ASSERT(goc_schema_is_valid(goc_schema_int_range(1, 3), goc_box(int64_t, 3)));
    err = goc_schema_validate(goc_schema_int_range(1, 3), goc_box(int64_t, 0));
    ASSERT(err != NULL);
    ASSERT(strstr(goc_schema_error_message(err), "expected minimum 1") != NULL);
    err = goc_schema_validate(goc_schema_int_range(1, 3), goc_box(int64_t, 4));
    ASSERT(err != NULL);
    ASSERT(strstr(goc_schema_error_message(err), "expected maximum 3") != NULL);

    goc_array* int_vals = goc_array_of_boxed(int64_t, 1, 2, 3);
    ASSERT(goc_schema_is_valid(goc_schema_int_enum(int_vals), goc_box(int64_t, 2)));
    err = goc_schema_validate(goc_schema_int_enum(int_vals), goc_box(int64_t, 4));
    ASSERT(err != NULL);

    ASSERT(goc_schema_is_valid(goc_schema_real_min(0.5), goc_box(double, 0.5)));
    err = goc_schema_validate(goc_schema_real_min(0.5), goc_box(double, 0.4));
    ASSERT(err != NULL);

    ASSERT(goc_schema_is_valid(goc_schema_real_max(2.0), goc_box(double, 2.0)));
    err = goc_schema_validate(goc_schema_real_max(2.0), goc_box(double, 2.1));
    ASSERT(err != NULL);

    ASSERT(goc_schema_is_valid(goc_schema_real_range(1.5, 2.5), goc_box(double, 1.5)));
    err = goc_schema_validate(goc_schema_real_range(1.5, 2.5), goc_box(double, 1.4));
    ASSERT(err != NULL);
    err = goc_schema_validate(goc_schema_real_range(1.5, 2.5), goc_box(double, 2.6));
    ASSERT(err != NULL);

    ASSERT(goc_schema_is_valid(goc_schema_real_ex_min(1.0), goc_box(double, 1.1)));
    err = goc_schema_validate(goc_schema_real_ex_min(1.0), goc_box(double, 1.0));
    ASSERT(err != NULL);

    ASSERT(goc_schema_is_valid(goc_schema_real_ex_max(3.0), goc_box(double, 2.9)));
    err = goc_schema_validate(goc_schema_real_ex_max(3.0), goc_box(double, 3.0));
    ASSERT(err != NULL);

    ASSERT(goc_schema_is_valid(goc_schema_real_multiple(0.5), goc_box(double, 1.0)));
    ASSERT(!goc_schema_is_valid(goc_schema_real_multiple(0.5), goc_box(double, 1.1)));

    TEST_PASS();

done:;
}

static void test_schema_complex(void) {
    TEST_BEGIN("SC9   complex number schema support");
    goc_schema_error* err;

    ASSERT(goc_schema_is_valid(goc_schema_complex(), goc_box(long double complex, 1.0L + 2.0L * I)));
    ASSERT(goc_schema_is_valid(goc_schema_number(), goc_box(long double complex, 1.0L + 2.0L * I)));
    ASSERT(!goc_schema_is_valid(goc_schema_real(), goc_box(long double complex, 1.0L + 2.0L * I)));

    err = goc_schema_validate(goc_schema_complex(), goc_box(double, 42.0));
    ASSERT(err != NULL);
    ASSERT(strcmp(goc_schema_error_message(err), "expected complex, got real") == 0);

    goc_schema_check(goc_schema_complex(), goc_box(long double complex, 0.0L + 0.0L * I));

    TEST_PASS();

done:;
}

static void test_schema_str_format(void) {
    TEST_BEGIN("SC10  string format validation");
    goc_schema_error* err;

    ASSERT(goc_schema_is_valid(goc_schema_str_format("email"), "alice@example.com"));
    ASSERT(!goc_schema_is_valid(goc_schema_str_format("email"), "not-an-email"));

    ASSERT(goc_schema_is_valid(goc_schema_str_format("unknown-format"), "anything"));

    TEST_PASS();

done:;
}

static void test_schema_array_length(void) {
    TEST_BEGIN("SC11  array length constraint");

    goc_array* arr = goc_array_of_boxed(int64_t, 1, 2);
    ASSERT(goc_schema_is_valid(goc_schema_arr_len(goc_schema_int(), 2, 3), arr));

    goc_array_pop(arr);
    ASSERT(!goc_schema_is_valid(goc_schema_arr_len(goc_schema_int(), 2, 3), arr));

    TEST_PASS();

done:;
}

static void test_schema_arr_homogeneous_and_len(void) {
    TEST_BEGIN("SC12  homogeneous and bounded arrays");

    goc_array* arr = goc_array_of_boxed(int64_t, 1, 2);
    goc_array_push(arr, "xyz");

    goc_schema_error* err = goc_schema_validate(goc_schema_arr(goc_schema_int()), arr);
    ASSERT(err != NULL);

    goc_array_pop(arr);
    ASSERT(goc_schema_is_valid(goc_schema_arr(goc_schema_int()), arr));

    goc_array_pop(arr);
    goc_schema* len_schema = goc_schema_arr_len(goc_schema_int(), 1, 2);
    ASSERT(goc_schema_is_valid(len_schema, arr));

    goc_array_pop(arr);
    err = goc_schema_validate(len_schema, arr);
    ASSERT(err != NULL);

    goc_array* arr2 = goc_array_of_boxed(int64_t, 1, 2, 3);
    ASSERT(!goc_schema_is_valid(len_schema, arr2));

    TEST_PASS();

done:;
}

static void test_schema_arr_unique_value_eq(void) {
    TEST_BEGIN("SC13  array unique value equality");
    goc_array* arr = goc_array_of_boxed(int64_t, 1, 1);

    goc_schema_error* err = goc_schema_validate(goc_schema_arr_unique(goc_schema_int()), arr);
    ASSERT(err != NULL);
    ASSERT(strstr(goc_schema_error_message(err), "duplicate elements") != NULL);

    goc_array* nan_arr = goc_array_of_boxed(double, NAN, NAN);
    ASSERT(goc_schema_is_valid(goc_schema_arr_unique(goc_schema_real()), nan_arr));

    TEST_PASS();

done:;
}

static void test_schema_arr_contains(void) {
    TEST_BEGIN("SC14  array contains semantics");
    goc_array* arr = goc_array_of_boxed(int64_t, 1, 2, 3);

    goc_schema_error* err = goc_schema_validate(
        goc_schema_arr_contains(goc_schema_int(),
                                goc_schema_int_min(2),
                                1,
                                0),
        arr);
    ASSERT(err == NULL);

    err = goc_schema_validate(
        goc_schema_arr_contains(goc_schema_int(),
                                goc_schema_int_min(4),
                                1,
                                0),
        arr);
    ASSERT(err != NULL);
    ASSERT(strstr(goc_schema_error_message(err), "expected at least") != NULL);

    err = goc_schema_validate(
        goc_schema_arr_contains(goc_schema_int(),
                                goc_schema_int_min(2),
                                0,
                                1),
        arr);
    ASSERT(err != NULL);
    ASSERT(strstr(goc_schema_error_message(err), "expected at most") != NULL);

    TEST_PASS();

done:;
}

static void test_schema_tuple(void) {
    TEST_BEGIN("SC15  tuple schema behavior");
    goc_schema_error* err;
    goc_schema* tuple_schema = goc_schema_tuple(
        goc_array_of_boxed(goc_schema_item_t,
            (goc_schema_item_t){ goc_schema_int() },
            (goc_schema_item_t){ goc_schema_str() }
        ),
        NULL);

    goc_array* ok = goc_array_of_boxed(int64_t, 1);
    goc_array_push(ok, "hello");
    ASSERT(goc_schema_is_valid(tuple_schema, ok));

    goc_array* extra = goc_array_of_boxed(int64_t, 1);
    goc_array_push(extra, "hello");
    goc_array_push(extra, goc_box(bool, true));
    ASSERT(!goc_schema_is_valid(tuple_schema, extra));


    goc_schema* tuple_any = goc_schema_tuple(
        goc_array_of_boxed(goc_schema_item_t,
            (goc_schema_item_t){ goc_schema_int() },
            (goc_schema_item_t){ goc_schema_str() }
        ),
        goc_schema_any());
    ASSERT(goc_schema_is_valid(tuple_any, extra));

    TEST_PASS();

done:;
}

static void test_schema_obj_optional_and_props(void) {
    TEST_BEGIN("SC16  object optionals and property count boundaries");
    goc_schema_error* err;
    goc_schema* schema = goc_schema_dict_of(
        {"id", goc_schema_int()},
        {"note", goc_schema_str(), true, NULL, NULL, NULL, false, false, false}
    );

    goc_dict* value = goc_dict_make(0);
    goc_dict_set(value, "id", goc_box(int64_t, 1));
    ASSERT(goc_schema_is_valid(schema, value));

    goc_dict_set(value, "note", "ok");
    ASSERT(goc_schema_is_valid(schema, value));

    goc_dict_set(value, "note", goc_box(bool, true));
    err = goc_schema_validate(schema, value);
    ASSERT(err != NULL);

    goc_schema* minmax = goc_schema_dict(goc_array_of_boxed(goc_schema_field_t,
        (goc_schema_field_t){"a", goc_schema_int(), false, NULL, NULL, NULL, false, false, false}
    ),
    (goc_schema_dict_opts_t){ .min_properties = 1, .max_properties = 2 });
    goc_dict* too_few = goc_dict_make(0);
    err = goc_schema_validate(minmax, too_few);
    ASSERT(err != NULL);

    goc_dict* too_many = goc_dict_of(
        {"a", goc_box(int64_t, 1)},
        {"b", goc_box(int64_t, 2)},
        {"c", goc_box(int64_t, 3)}
    );
    ASSERT(!goc_schema_is_valid(minmax, too_many));


    goc_dict* boundary = goc_dict_of(
        {"a", goc_box(int64_t, 1)},
        {"b", goc_box(int64_t, 2)}
    );
    ASSERT(goc_schema_is_valid(minmax, boundary));

    TEST_PASS();

done:;
}

static void test_schema_obj_property_names(void) {
    TEST_BEGIN("SC17  object propertyNames and patternProps");
    goc_schema_error* err;
    goc_schema* property_names = goc_schema_str_pattern("^[a-z]+$");
    goc_schema* schema = goc_schema_dict(goc_array_of_boxed(goc_schema_field_t,
        (goc_schema_field_t){"name", goc_schema_str(), false, NULL, NULL, NULL, false, false, false}
    ),
    (goc_schema_dict_opts_t){ .property_names = property_names });

    goc_dict* good = goc_dict_of(
        {"name", "alice"}
    );
    ASSERT(goc_schema_is_valid(schema, good));

    goc_dict* bad = goc_dict_of(
        {"name", "alice"},
        {"bad1", "bob"}
    );
    err = goc_schema_validate(schema, bad);
    ASSERT(err != NULL);
    ASSERT(strstr(goc_schema_error_message(err), "key 'bad1'") != NULL);

    goc_schema_pattern_prop_t p1 = { "^foo", goc_schema_int(), NULL };
    goc_schema_pattern_prop_t p2 = { "^foo", goc_schema_str(), NULL };
    goc_schema* pp_schema = goc_schema_dict(goc_array_of_boxed(goc_schema_field_t,
        (goc_schema_field_t){"id", goc_schema_int(), false, NULL, NULL, NULL, false, false, false}
    ),
    (goc_schema_dict_opts_t){ .pattern_props = goc_array_of_boxed(goc_schema_pattern_prop_t, p1, p2) });

    goc_dict* multi = goc_dict_of(
        {"id", goc_box(int64_t, 1)},
        {"fooBar", goc_box(bool, true)}
    );
    ASSERT(!goc_schema_is_valid(pp_schema, multi));


    TEST_PASS();

done:;
}

static void test_schema_obj_dependencies(void) {
    TEST_BEGIN("SC18  object dependency schemas");
    goc_schema* dep_req_schema = goc_schema_dict(goc_array_of_boxed(goc_schema_field_t,
        (goc_schema_field_t){"primary", goc_schema_int(), false, NULL, NULL, NULL, false, false, false}
    ),
    (goc_schema_dict_opts_t){ .dep_required = goc_array_of_boxed(goc_schema_dep_req_t,
        (goc_schema_dep_req_t){ "primary", goc_array_of("secondary") }
    ) });

    goc_dict* missing_dep = goc_dict_of(
        {"primary", goc_box(int64_t, 1)}
    );
    goc_schema_error* err = goc_schema_validate(dep_req_schema, missing_dep);
    ASSERT(err != NULL);
    ASSERT(strstr(goc_schema_error_message(err), "field 'secondary' required when 'primary' is present") != NULL);

    goc_schema* dep_schema = goc_schema_dict(goc_array_of_boxed(goc_schema_field_t,
        (goc_schema_field_t){"trigger", goc_schema_str(), false, NULL, NULL, NULL, false, false, false},
        (goc_schema_field_t){"dependent", goc_schema_int(), false, NULL, NULL, NULL, false, false, false}
    ),
    (goc_schema_dict_opts_t){ .dep_schemas = goc_array_of_boxed(goc_schema_dep_schema_t,
        (goc_schema_dep_schema_t){ "trigger", goc_schema_dict_of(
            {"trigger", goc_schema_str()},
            {"dependent", goc_schema_int()}
        ) }
    ) });

    goc_dict* trigger_missing = goc_dict_of(
        {"trigger", "yes"}
    );
    ASSERT(!goc_schema_is_valid(dep_schema, trigger_missing));


    goc_dict* trigger_present = goc_dict_of(
        {"trigger", "yes"},
        {"dependent", goc_box(int64_t, 1)}
    );
    ASSERT(goc_schema_is_valid(dep_schema, trigger_present));

    TEST_PASS();

done:;
}

static void test_schema_obj_pattern_props(void) {
    TEST_BEGIN("SC19  object patternProps regex caching");
    goc_schema_error* err;

    goc_schema_pattern_prop_t prop1 = { "^foo", goc_schema_int(), NULL };
    goc_schema_pattern_prop_t prop2 = { "^bar", goc_schema_str(), NULL };
    goc_array* props = goc_array_of_boxed(goc_schema_pattern_prop_t, prop1, prop2);
    goc_schema* schema = goc_schema_dict(goc_array_of_boxed(goc_schema_field_t,
        (goc_schema_field_t){"id", goc_schema_int(), false, NULL, NULL, NULL, false, false, false}
    ),
    (goc_schema_dict_opts_t){ .pattern_props = props });

    goc_dict* value = goc_dict_make(0);
    goc_dict_set(value, "id", goc_box(int64_t, 1));
    goc_dict_set(value, "foo1", goc_box(int64_t, 2));
    goc_dict_set(value, "bar1", "baz");

    ASSERT(goc_schema_is_valid(schema, value));

    goc_dict_set(value, "foo2", goc_box(bool, true));
    err = goc_schema_validate(schema, value);
    ASSERT(err != NULL);
    ASSERT(strstr(goc_schema_error_path(err), "foo2") != NULL);

    TEST_PASS();

done:;
}

static void test_schema_obj_strict_and_required(void) {
    TEST_BEGIN("SC20  object strict mode and required fields");
    goc_schema_error* err;
    goc_schema* strict_schema = goc_schema_dict(goc_array_of_boxed(goc_schema_field_t,
        (goc_schema_field_t){"id", goc_schema_int(), false, NULL, NULL, NULL, false, false, false},
        (goc_schema_field_t){"name", goc_schema_str(), false, NULL, NULL, NULL, false, false, false}
    ),
    (goc_schema_dict_opts_t){ .strict = true });

    goc_dict* value = goc_dict_make(0);
    goc_dict_set(value, "id", goc_box(int64_t, 1));
    goc_dict_set(value, "name", "alice");

    ASSERT(goc_schema_is_valid(strict_schema, value));

    goc_dict_set(value, "extra", goc_box(bool, true));
    err = goc_schema_validate(strict_schema, value);
    ASSERT(err != NULL);
    ASSERT(strstr(goc_schema_error_message(err), "unexpected key") != NULL);

    TEST_PASS();

done:;
}

static void test_schema_conditional_and_composition(void) {
    TEST_BEGIN("SC21  conditional and composition schemas");
    goc_schema_error* err;

    goc_schema* cond = goc_schema_if(goc_schema_bool(),
                                     goc_schema_int(),
                                     goc_schema_str());
    err = goc_schema_validate(cond, goc_box(bool, true));
    ASSERT(err != NULL);

    err = goc_schema_validate(cond, goc_box(bool, false));
    ASSERT(err != NULL);

    goc_schema* any_of = goc_schema_any_of(goc_schema_int(), goc_schema_str());
    ASSERT(goc_schema_is_valid(any_of, goc_box(int64_t, 1)));
    ASSERT(goc_schema_is_valid(any_of, "ok"));
    err = goc_schema_validate(any_of, goc_box(bool, true));
    ASSERT(err != NULL);

    goc_schema* one_of = goc_schema_one_of(goc_schema_int(), goc_schema_int_const(1));
    ASSERT(goc_schema_is_valid(one_of, goc_box(int64_t, 2)));
    err = goc_schema_validate(one_of, goc_box(int64_t, 1));
    ASSERT(err != NULL);

    goc_schema* all_of = goc_schema_all_of(goc_schema_int(), goc_schema_int_min(0));
    ASSERT(goc_schema_is_valid(all_of, goc_box(int64_t, 1)));
    err = goc_schema_validate(all_of, goc_box(int64_t, -1));
    ASSERT(err != NULL);

    goc_schema* not_schema = goc_schema_not(goc_schema_int());
    ASSERT(!goc_schema_is_valid(not_schema, goc_box(int64_t, 1)));

    ASSERT(goc_schema_is_valid(not_schema, goc_box(bool, true)));

    TEST_PASS();

done:;
}

static void test_schema_ref_double_set_and_meta(void) {
    TEST_BEGIN("SC22  ref and metadata behavior");
    goc_schema_error* err;
    goc_schema_ref* ref = goc_schema_ref_make();
    goc_schema_ref_set(ref, goc_schema_int());
    goc_schema_ref_set(ref, goc_schema_str());

    goc_schema* s = (goc_schema*)ref;
    ASSERT(goc_schema_is_valid(s, goc_box(int64_t, 1)));
    ASSERT(!goc_schema_is_valid(s, "bad"));


    goc_schema_meta_t meta = {"title", "desc", NULL, NULL, true, true, false};
    goc_schema_with_meta(s, meta);
    goc_schema_meta_t* stored = goc_schema_meta(s);
    ASSERT(stored != NULL);
    ASSERT(stored->deprecated);
    ASSERT(stored->read_only);
    ASSERT(!stored->write_only);

    meta.deprecated = false;
    ASSERT(stored->deprecated);

    goc_schema_meta_t overwrite = {"title2", "desc2", NULL, NULL, false, false, true};
    goc_schema_with_meta(s, overwrite);
    stored = goc_schema_meta(s);
    ASSERT(stored != NULL);
    ASSERT(stored->deprecated == false);
    ASSERT(stored->write_only == true);

    TEST_PASS();

done:;
}

static void test_schema_error_paths_and_nested_composite(void) {
    TEST_BEGIN("SC23  error path construction and nested composite schemas");
    goc_schema_error* err;

    goc_schema* address = goc_schema_dict_of(
        {"city", goc_schema_str()}
    );
    goc_schema* person = goc_schema_dict_of(
        {"address", address}
    );

    goc_dict* bad_person = goc_dict_of(
        {"address", goc_dict_of({"city", goc_box(int64_t, 1)})}
    );
    err = goc_schema_validate(person, bad_person);
    ASSERT(err != NULL);
    ASSERT(strstr(goc_schema_error_path(err), ".address.city") != NULL);

    goc_schema* nested = goc_schema_dict_of(
        {"records", goc_schema_arr(
            goc_schema_tuple(goc_array_of_boxed(goc_schema_item_t,
                (goc_schema_item_t){ goc_schema_int() },
                (goc_schema_item_t){ goc_schema_str() }
            ), NULL)
        )}
    );
    goc_array* bad_record = goc_array_make(0);
    goc_array_push_boxed(int64_t, bad_record, 1);
    goc_array_push_boxed(int64_t, bad_record, 2);
    goc_array* records = goc_array_make(0);
    goc_array_push(records, bad_record);
    goc_dict* parent = goc_dict_of(
        {"records", records}
    );
    err = goc_schema_validate(nested, parent);
    ASSERT(err != NULL);
    ASSERT(strstr(goc_schema_error_path(err), ".records.[0].[1]") != NULL);

    TEST_PASS();

done:;
}

static void test_schema_recursive_ref(void) {
    TEST_BEGIN("SC24  recursive schema reference");
    goc_schema_ref* comment_ref = goc_schema_ref_make();

    goc_schema* comment_schema = goc_schema_dict_of(
        {"id", goc_schema_int()},
        {"body", goc_schema_str()},
        {"replies", goc_schema_arr((goc_schema*)comment_ref)}
    );
    goc_schema_ref_set(comment_ref, comment_schema);

    goc_dict* reply = goc_dict_of(
        {"id", goc_box(int64_t, 2)},
        {"body", "child"},
        {"replies", goc_array_make(0)}
    );

    goc_array* reply_list = goc_array_make(0);
    goc_array_push(reply_list, reply);

    goc_dict* comment = goc_dict_of(
        {"id", goc_box(int64_t, 1)},
        {"body", "root"},
        {"replies", reply_list}
    );

    ASSERT(goc_schema_is_valid(comment_schema, comment));

    TEST_PASS();

done:;
}

static void test_schema_hierarchy_helpers(void) {
    TEST_BEGIN("SC25  public schema hierarchy helpers");

    goc_schema* custom_child = goc_schema_any_of(goc_schema_int(), goc_schema_str());
    goc_schema_derive(custom_child, goc_schema_int());
    goc_schema_derive(custom_child, goc_schema_str());

    goc_array* parents = goc_schema_parents(custom_child);
    bool has_int = false;
    bool has_str = false;
    for (size_t i = 0; i < goc_array_len(parents); i++) {
        goc_schema* parent = (goc_schema*)goc_array_get(parents, i);
        if (parent == goc_schema_int()) has_int = true;
        if (parent == goc_schema_str()) has_str = true;
    }
    ASSERT(has_int);
    ASSERT(has_str);

    goc_array* ancestors = goc_schema_ancestors(custom_child);
    has_int = false;
    has_str = false;
    for (size_t i = 0; i < goc_array_len(ancestors); i++) {
        goc_schema* ancestor = (goc_schema*)goc_array_get(ancestors, i);
        if (ancestor == goc_schema_int()) has_int = true;
        if (ancestor == goc_schema_str()) has_str = true;
    }
    ASSERT(has_int);
    ASSERT(has_str);

    goc_array* descendants = goc_schema_descendants(goc_schema_int());
    bool found_child = false;
    for (size_t i = 0; i < goc_array_len(descendants); i++) {
        if (goc_array_get(descendants, i) == custom_child) {
            found_child = true;
            break;
        }
    }
    ASSERT(found_child);

    TEST_PASS();

done:;
}

static bool is_prime(void* val) {
    if (!goc_schema_is_valid(goc_schema_int(), val)) return false;
    int64_t n = goc_unbox(int64_t, val);
    if (n < 2) return false;
    for (int64_t i = 2; i * i <= n; i++)
        if (n % i == 0) return false;
    return true;
}

static void test_schema_predicate(void) {
    TEST_BEGIN("SC26  predicate schema");
    goc_schema_error* err;

    goc_schema* prime = goc_schema_predicate(is_prime, "prime");

    ASSERT(goc_schema_is_valid(prime, goc_box(int64_t, 2)));
    ASSERT(goc_schema_is_valid(prime, goc_box(int64_t, 7)));
    ASSERT(goc_schema_is_valid(prime, goc_box(int64_t, 97)));

    ASSERT(!goc_schema_is_valid(prime, goc_box(int64_t, 1)));
    ASSERT(!goc_schema_is_valid(prime, goc_box(int64_t, 4)));
    ASSERT(!goc_schema_is_valid(prime, goc_box(int64_t, 0)));

    err = goc_schema_validate(prime, goc_box(int64_t, 4));
    ASSERT(err != NULL);
    ASSERT(strstr(goc_schema_error_message(err), "prime") != NULL);

    /* compose: positive prime via all_of */
    goc_schema* pos_prime = goc_schema_all_of(
        goc_schema_int_min(2),
        goc_schema_predicate(is_prime, "prime")
    );
    ASSERT(goc_schema_is_valid(pos_prime, goc_box(int64_t, 13)));
    ASSERT(!goc_schema_is_valid(pos_prime, goc_box(int64_t, 1)));

    /* hierarchy: prime derives from int */
    goc_schema_derive(prime, goc_schema_int());
    ASSERT(goc_schema_is_a(prime, goc_schema_int()));
    ASSERT(goc_schema_is_a(prime, goc_schema_real()));

    /* NULL name — falls back to "predicate" in error message */
    goc_schema* unnamed = goc_schema_predicate(is_prime, NULL);
    err = goc_schema_validate(unnamed, goc_box(int64_t, 6));
    ASSERT(err != NULL);
    ASSERT(strstr(goc_schema_error_message(err), "predicate") != NULL);

    TEST_PASS();

done:;
}

static void test_schema_global_registry(void) {
    TEST_BEGIN("SC27  global registry — built-in entries and user registration");

    goc_schema_registry* reg = goc_schema_global_registry();
    ASSERT(reg != NULL);

    /* same pointer each call */
    ASSERT(goc_schema_global_registry() == reg);

    /* all built-ins registered */
    ASSERT(goc_schema_registry_get(reg, "goc/any")     == goc_schema_any());
    ASSERT(goc_schema_registry_get(reg, "goc/null")    == goc_schema_null());
    ASSERT(goc_schema_registry_get(reg, "goc/bool")    == goc_schema_bool());
    ASSERT(goc_schema_registry_get(reg, "goc/int")     == goc_schema_int());
    ASSERT(goc_schema_registry_get(reg, "goc/uint")    == goc_schema_uint());
    ASSERT(goc_schema_registry_get(reg, "goc/byte")    == goc_schema_byte());
    ASSERT(goc_schema_registry_get(reg, "goc/ubyte")   == goc_schema_ubyte());
    ASSERT(goc_schema_registry_get(reg, "goc/number")  == goc_schema_number());
    ASSERT(goc_schema_registry_get(reg, "goc/real")    == goc_schema_real());
    ASSERT(goc_schema_registry_get(reg, "goc/complex") == goc_schema_complex());
    ASSERT(goc_schema_registry_get(reg, "goc/str")     == goc_schema_str());
    ASSERT(goc_schema_registry_get(reg, "goc/arr")     == goc_schema_arr_any());
    ASSERT(goc_schema_registry_get(reg, "goc/dict")    == goc_schema_dict_any());

    /* arr_any and dict_any are stable singletons */
    ASSERT(goc_schema_arr_any()  == goc_schema_arr_any());
    ASSERT(goc_schema_dict_any() == goc_schema_dict_any());

    /* arr_any accepts any goc_array*, rejects non-arrays */
    ASSERT(goc_schema_is_valid(goc_schema_arr_any(),  goc_array_of_boxed(int64_t, 1, 2)));
    ASSERT(!goc_schema_is_valid(goc_schema_arr_any(), goc_box(int64_t, 1)));
    ASSERT(!goc_schema_is_valid(goc_schema_arr_any(), "not an array"));

    /* dict_any accepts any goc_dict*, rejects non-dicts */
    ASSERT(goc_schema_is_valid(goc_schema_dict_any(),  goc_dict_of_boxed(int64_t, {"x", 1})));
    ASSERT(goc_schema_is_valid(goc_schema_dict_any(),  goc_dict_make(0)));
    ASSERT(!goc_schema_is_valid(goc_schema_dict_any(), goc_box(int64_t, 1)));
    ASSERT(!goc_schema_is_valid(goc_schema_dict_any(), "not a dict"));

    /* unknown name returns NULL */
    ASSERT(goc_schema_registry_get(reg, "goc/missing") == NULL);

    /* user registration */
    goc_schema* my_schema = goc_schema_int_min(0);
    goc_schema_registry_add(reg, "test/positive_int", my_schema);
    ASSERT(goc_schema_registry_get(reg, "test/positive_int") == my_schema);

    TEST_PASS();

done:;
}

int main(void) {
    install_crash_handler();

    printf("libgoc test suite — goc_schema\n");
    printf("==============================\n\n");

    goc_init();

    test_schema_registry();
    test_schema_hierarchy();
    test_schema_scalar_types();
    test_schema_check_abort();
    test_schema_boxed_scalar_aliases();
    test_schema_scalar_const();
    test_schema_str_len_and_pattern();
    test_schema_numeric_constraints();
    test_schema_complex();
    test_schema_str_format();
    test_schema_array_length();
    test_schema_arr_homogeneous_and_len();
    test_schema_arr_unique_value_eq();
    test_schema_arr_contains();
    test_schema_tuple();
    test_schema_obj_optional_and_props();
    test_schema_obj_property_names();
    test_schema_obj_dependencies();
    test_schema_obj_pattern_props();
    test_schema_obj_strict_and_required();
    test_schema_conditional_and_composition();
    test_schema_ref_double_set_and_meta();
    test_schema_error_paths_and_nested_composite();
    test_schema_recursive_ref();
    test_schema_hierarchy_helpers();
    test_schema_predicate();
    test_schema_global_registry();

    goc_shutdown();

    REPORT(g_tests_run, g_tests_passed, g_tests_failed);
    return (g_tests_failed == 0) ? 0 : 1;
}
