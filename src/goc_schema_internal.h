/* src/goc_schema_internal.h — internal schema implementation details
 *
 * This header is for libgoc internal schema implementation only and is not
 * part of the public API. It defines the runtime schema representation used
 * by schema validation and JSON serialization.
 */

#ifndef GOC_SCHEMA_INTERNAL_H
#define GOC_SCHEMA_INTERNAL_H

#include "../include/goc_schema.h"

/**
 * schema_kind — internal runtime schema kind tags.
 *
 * These values correspond to concrete schema representations in `struct goc_schema`.
 */
typedef enum {
    SCHEMA_NULL, SCHEMA_BOOL, SCHEMA_INT, SCHEMA_REAL, SCHEMA_COMPLEX, SCHEMA_UINT,
    SCHEMA_NUMBER, SCHEMA_BYTE, SCHEMA_UBYTE, SCHEMA_STR, SCHEMA_ANY,
    SCHEMA_BOOL_CONST, SCHEMA_INT_CONST, SCHEMA_REAL_CONST, SCHEMA_STR_CONST,
    SCHEMA_INT_MIN, SCHEMA_INT_MAX, SCHEMA_INT_RANGE, SCHEMA_INT_ENUM,
    SCHEMA_REAL_MIN, SCHEMA_REAL_MAX, SCHEMA_REAL_RANGE,
    SCHEMA_REAL_EX_MIN, SCHEMA_REAL_EX_MAX, SCHEMA_REAL_MULTIPLE,
    SCHEMA_STR_MIN_LEN, SCHEMA_STR_MAX_LEN, SCHEMA_STR_LEN,
    SCHEMA_STR_PATTERN, SCHEMA_STR_ENUM, SCHEMA_STR_FORMAT,
    SCHEMA_ARR, SCHEMA_ARR_LEN, SCHEMA_ARR_UNIQUE, SCHEMA_ARR_CONTAINS,
    SCHEMA_TUPLE, SCHEMA_OBJ,
    SCHEMA_IF, SCHEMA_ANY_OF, SCHEMA_ONE_OF, SCHEMA_ALL_OF, SCHEMA_NOT,
    SCHEMA_REF,
    SCHEMA_PREDICATE,
} schema_kind;

/*
 * Internal runtime schema representation.
 *
 * `kind` identifies the schema type. `name` is the optional registered schema
 * name used by tagged JSON helpers and the global registry. `parent` is the
 * declared parent schema in the derivation hierarchy. `meta` holds optional
 * annotations attached by goc_schema_with_meta(). `methods` stores any custom
 * schema methods registered via goc_schema_method_set().
 */
struct goc_schema {
    goc_schema_kind_t  kind;
    const char*        name;
    goc_schema*        parent;
    goc_schema_meta_t* meta;
    goc_dict*          methods;
    union {
        /* Const value schemas. */
        bool           bool_const;
        int64_t        int_const;
        double         real_const;
        char*          str_const;

        /* Integer constraint schemas. */
        int64_t        int_min;
        int64_t        int_max;
        struct {
            int64_t min;
            int64_t max;
        } int_range;
        goc_array*     int_enum;

        /* Real constraint schemas. */
        long double    real_min;
        long double    real_max;
        struct {
            long double min;
            long double max;
        } real_range;
        long double    real_factor;

        /* String constraint schemas. */
        size_t         str_len_min;
        size_t         str_len_max;
        struct {
            size_t min;
            size_t max;
        } str_len;
        char*          str_pattern;
        regex_t*       str_regex;
        goc_array*     str_enum;
        char*          str_format_name;
        regex_t*       str_format_regex;

        /* Array schemas. */
        struct {
            goc_schema* elem;
        } arr;

        struct {
            goc_schema* elem;
            size_t      min;
            size_t      max;
        } arr_len;

        struct {
            goc_schema* elem;
        } arr_unique;

        struct {
            goc_schema* elem;
            goc_schema* contains;
            size_t      min_contains;
            size_t      max_contains;
        } arr_contains;

        /* Tuple schema. */
        struct {
            goc_array* items;
            goc_schema* additional_items;
        } tuple;

        /* Object schema. */
        struct {
            goc_array* fields;
            goc_schema_dict_opts_t opts;
        } obj;

        /* Composition schemas (any_of/one_of/all_of). */
        goc_array* schemas;

        /* Reference schemas. */
        goc_schema_ref* ref;

        /* Predicate schemas. */
        struct {
            bool (*fn)(void* val);
            const char* name;
        } predicate;
    } u;
};

/*
 * Recursive schema reference cell.
 *
 * The `base` member allows the ref cell to be treated as a `goc_schema*`
 * in the implementation, while `target` points to the concrete schema once
 * the reference is resolved.
 */
struct goc_schema_ref {
    goc_schema  base;
    goc_schema* target;
};

bool goc_schema_is_goc_array(void* val);
bool goc_schema_is_goc_dict(void* val);
bool goc_schema_is_boxed(void* val);
goc_boxed_type_t goc_schema_boxed_type(void* val);
size_t goc_schema_boxed_payload_size(void* val);
bool goc_schema_is_boxed_bool(void* val);
bool goc_schema_is_boxed_byte(void* val);
bool goc_schema_is_boxed_ubyte(void* val);
bool goc_schema_is_boxed_int(void* val);
bool goc_schema_is_boxed_uint(void* val);
bool goc_schema_is_boxed_real(void* val);
bool goc_schema_is_boxed_complex(void* val);
bool goc_schema_val_is_str(void* val);

#endif /* GOC_SCHEMA_INTERNAL_H */
