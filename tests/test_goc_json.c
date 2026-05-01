/* tests/test_goc_json.c — JSON parser and serializer tests
 *
 * Verifies JSON parsing, serialization, custom tagged object handling,
 * and result-based error reporting.
 *
 * Build:  cmake -B build && cmake --build build
 * Run:    ctest --test-dir build --output-on-failure
 *         ./build/test_goc_json
 *
 * Test coverage:
 *
 *   J1.  Basic JSON parsing for null, booleans, integers, reals, strings.
 *   J2.  Parsing arrays and objects into libgoc containers.
 *   J3.  Invalid JSON parsing reports an error.
 *   J4.  Basic JSON serialization and pretty-print formatting.
 *   J5.  Complex tagged object roundtrip for built-in `goc/complex`.
 *   J6.  Custom schema-backed tagged int-string roundtrip via
 *        `goc_json_set_methods` and `goc_schema_make_tagged`.
 *   J6b. Custom tagged-object `read_json` output is rejected during parse.
 *   J7.  JSON stringify nested error path reporting.
 *   J8.  JSON stringify root error path reporting.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <complex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_harness.h"
#include "goc.h"
#include "goc_array.h"
#include "goc_dict.h"
#include "goc_schema.h"
#include "goc_json.h"

static void test_json_parse_basic(void) {
    TEST_BEGIN("JSON1 parse basic values");

    goc_json_result r = goc_json_parse("null");
    ASSERT(r.err == NULL);
    ASSERT(r.res == NULL);

    r = goc_json_parse("true");
    ASSERT(r.err == NULL);
    ASSERT(goc_unbox(bool, r.res) == true);

    r = goc_json_parse("123");
    ASSERT(r.err == NULL);
    ASSERT(goc_unbox(int64_t, r.res) == 123);

    r = goc_json_parse("9223372036854775808");
    ASSERT(r.err == NULL);
    ASSERT(goc_unbox(uint64_t, r.res) == 9223372036854775808ULL);

    r = goc_json_parse("3.1415");
    ASSERT(r.err == NULL);
    ASSERT(goc_unbox(double, r.res) == 3.1415);

    r = goc_json_parse("\"hello\"");
    ASSERT(r.err == NULL);
    ASSERT(strcmp((char*)r.res, "hello") == 0);

    TEST_PASS();

done:;
}

static void test_json_parse_array_and_object(void) {
    TEST_BEGIN("JSON2 parse array and object");

    goc_json_result r = goc_json_parse("[10, 20, 30]");
    ASSERT(r.err == NULL);
    goc_array* arr = (goc_array*)r.res;
    ASSERT(goc_array_len(arr) == 3);
    ASSERT(goc_unbox(int64_t, goc_array_get(arr, 0)) == 10);
    ASSERT(goc_unbox(int64_t, goc_array_get(arr, 2)) == 30);

    r = goc_json_parse("{\"host\": \"localhost\", \"port\": 8080}");
    ASSERT(r.err == NULL);
    goc_dict* cfg = (goc_dict*)r.res;
    ASSERT(strcmp(goc_dict_get(cfg, "host", NULL), "localhost") == 0);
    ASSERT(goc_unbox(int64_t, goc_dict_get(cfg, "port", NULL)) == 8080);

    TEST_PASS();

done:;
}

static void test_json_parse_invalid(void) {
    TEST_BEGIN("JSON3 parse invalid JSON");

    goc_json_result r = goc_json_parse("{unclosed");
    ASSERT(r.err != NULL);
    ASSERT(r.res == NULL);
    ASSERT(strlen(goc_json_error_message(r.err)) > 0);

    TEST_PASS();

done:;
}

static void test_json_stringify_basic(void) {
    TEST_BEGIN("JSON4 stringify basic values");

    /* Verify basic dictionary serialization and pretty formatting. */
    goc_dict* point = goc_dict_of(
        {"x", goc_box(int64_t, 1)},
        {"y", goc_box(int64_t, 2)}
    );
    goc_schema* point_schema = goc_schema_dict_of(
        {"x", goc_schema_int()},
        {"y", goc_schema_int()}
    );

    goc_json_result r = goc_json_stringify(point_schema, point);
    ASSERT(r.err == NULL);
    ASSERT(strcmp((char*)r.res, "{\"x\":1,\"y\":2}") == 0);

    r = goc_json_stringify_pretty(point_schema, point);
    ASSERT(r.err == NULL);
    ASSERT(strstr((char*)r.res, "\n") != NULL);
    ASSERT(strstr((char*)r.res, "    \"x\"") != NULL);

    TEST_PASS();

done:;
}

static void test_json_stringify_error_path(void) {
    TEST_BEGIN("JSON7 stringify nested error path");

    goc_schema* s = goc_schema_dict_of(
        {"items", goc_schema_arr(goc_schema_int())}
    );

    goc_array* bad_arr = goc_array_make(0);
    goc_array_push(bad_arr, goc_box(int64_t, 1));
    goc_array_push(bad_arr, goc_dict_make(0));
    goc_dict* val = goc_dict_make(0);
    goc_dict_set(val, "items", bad_arr);

    goc_json_result r = goc_json_stringify(s, val);
    ASSERT(r.err != NULL);
    ASSERT(strcmp(goc_json_error_path(r.err), ".items.[1]") == 0);

    TEST_PASS();

done:;
}

static void test_json_stringify_error_root_path(void) {
    TEST_BEGIN("JSON8 stringify root error path");

    goc_json_result r = goc_json_stringify(goc_schema_int(), goc_box(double, 1.5));
    ASSERT(r.err != NULL);
    ASSERT(strcmp(goc_json_error_path(r.err), "") == 0);

    TEST_PASS();

done:;
}

static void test_json_complex_tagged_roundtrip(void) {
    TEST_BEGIN("JSON5 complex tagged roundtrip");

    /* Complex numbers are serialized as tagged JSON objects and should
     * roundtrip through parse/stringify using the built-in complex schema. */
    double complex z = 1.5 - 3.0 * I;
    void* boxed = goc_box(double complex, z);
    goc_schema* schema = goc_schema_complex();

    goc_json_result r = goc_json_stringify(schema, boxed);
    ASSERT(r.err == NULL);
    ASSERT(strstr((char*)r.res, "\"goc_schema\":\"goc/complex\"") != NULL);

    goc_json_result p = goc_json_parse((char*)r.res);
    ASSERT(p.err == NULL);
    double complex parsed = goc_unbox(double complex, p.res);
    ASSERT(creal(parsed) == creal(z));
    ASSERT(cimag(parsed) == cimag(z));

    TEST_PASS();

done:;
}

static goc_schema* int_string_schema;

/* Custom schema predicate to ensure the tagged int value is not NULL. */
static bool int_string_not_null(void* x) {
    return x != NULL;
}

/*
 * Custom JSON serialization for the tagged int-string schema.
 * The returned value is an already-serialized JSON string for the tagged
 * object produced by goc_schema_make_tagged().
 */
static char* int_string_to_json(void* val) {
    int64_t n = goc_unbox(int64_t, val);
    char* s = goc_sprintf("%lld", (long long)n);
    goc_json_result tmp = goc_json_stringify(
        goc_schema_tagged_schema(),
        goc_schema_make_tagged(int_string_schema, s)
    );
    return tmp.res;
}

/*
 * Custom JSON deserialization for the tagged int-string schema.
 * The input `val` is the default-parsed libgoc value for the inner `goc_value`.
 */
static void* int_string_read_json(void* val) {
    int64_t n = strtoll((const char*)val, NULL, 10);
    return goc_box(int64_t, n);
}

static void test_json_intstring_tagged_roundtrip(void) {
    TEST_BEGIN("JSON6 intstring tagged roundtrip");

    /* Register a named predicate schema so the tagged JSON wrapper can refer to it. */
    int_string_schema = goc_schema_named("app/intstring",
        goc_schema_predicate(
            goc_schema_int(),
            int_string_not_null,
            NULL));
    goc_json_set_methods(int_string_schema, int_string_to_json, int_string_read_json);

    void* boxed = goc_box(int64_t, 123456789012345LL);

    goc_json_result out_r = goc_json_stringify(int_string_schema, boxed);
    if (out_r.err != NULL) {
        fprintf(stderr, "Error stringifying: %s\n", goc_json_error_message(out_r.err));
    }
    ASSERT(out_r.err == NULL);
    ASSERT(strstr((char*)out_r.res, "\"goc_schema\":\"app/intstring\"") != NULL);
    ASSERT(strstr((char*)out_r.res, "\"goc_value\":\"123456789012345\"") != NULL);

    goc_json_result p = goc_json_parse((char*)out_r.res);
    ASSERT(p.err == NULL);

    int64_t parsed = goc_unbox(int64_t, p.res);
    ASSERT(parsed == 123456789012345LL);

    TEST_PASS();

done:;
}

/* 
 * Custom JSON deserialization for the invalid tagged int-string test.
 * The callback intentionally returns a value that does not satisfy the
 * resolved schema so parse must fail with a tagged-object parse error.
 */
static void* int_string_invalid_read_json(void* val) {
    (void)val;
    return goc_strdup("invalid tagged result");
}

static void test_json_intstring_tagged_invalid_read_json(void) {
    TEST_BEGIN("JSON6b intstring tagged invalid read_json rejects parse");

    goc_schema* invalid_schema = goc_schema_named("app/intstring-invalid",
        goc_schema_predicate(goc_schema_int(), int_string_not_null, NULL));
    goc_json_set_methods(invalid_schema, NULL, int_string_invalid_read_json);

    const char* payload = "{\"goc_schema\":\"app/intstring-invalid\",\"goc_value\":\"123\"}";
    goc_json_result r = goc_json_parse(payload);
    ASSERT(r.err != NULL);
    ASSERT(r.res == NULL);
    ASSERT(strstr(goc_json_error_message(r.err), "tagged object parse failed") != NULL);

    TEST_PASS();

done:;
}

int main(void) {
    install_crash_handler();

    printf("libgoc test suite — goc_json\n");
    printf("==============================\n\n");

    goc_init();

    test_json_parse_basic();
    test_json_parse_array_and_object();
    test_json_parse_invalid();
    test_json_stringify_basic();
    test_json_stringify_error_path();
    test_json_stringify_error_root_path();
    test_json_complex_tagged_roundtrip();
    test_json_intstring_tagged_roundtrip();
    test_json_intstring_tagged_invalid_read_json();

    goc_shutdown();

    REPORT(g_tests_run, g_tests_passed, g_tests_failed);
    return (g_tests_failed == 0) ? 0 : 1;
}
