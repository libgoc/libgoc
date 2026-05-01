/* goc_schema.h — Public API for libgoc value schemas
 *
 * A lightweight schema library for validating libgoc values independently
 * of any serialisation format.
 *
 * Copyright (c) Divyansh Prakash
 *
 * Compile requirements: -std=c11 -DGC_THREADS -D_GNU_SOURCE
 */

#ifndef GOC_SCHEMA_H
#define GOC_SCHEMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <regex.h>

#include "goc.h"
#include "goc_array.h"
#include "goc_dict.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct goc_schema          goc_schema;
/** Recursive schema reference type used for self-referential schemas. */
typedef struct goc_schema_ref      goc_schema_ref;

/**
 * goc_schema_kind_t — identifies the abstract kind of a schema.
 *
 * The enum values classify schemas by their runtime structure and are used
 * by validation and serialization logic.
 */
typedef enum {
    GOC_SCHEMA_ANY,
    GOC_SCHEMA_NULL,
    GOC_SCHEMA_BOOL,
    GOC_SCHEMA_INT,
    GOC_SCHEMA_UINT,
    GOC_SCHEMA_BYTE,
    GOC_SCHEMA_UBYTE,
    GOC_SCHEMA_REAL,
    GOC_SCHEMA_NUMBER,
    GOC_SCHEMA_COMPLEX,
    GOC_SCHEMA_STR,
    GOC_SCHEMA_ARR,
    GOC_SCHEMA_TUPLE,
    GOC_SCHEMA_DICT,
    GOC_SCHEMA_CONST,
    GOC_SCHEMA_CONSTRAINED,
    GOC_SCHEMA_COMPOSITION,
    GOC_SCHEMA_PREDICATE,
    GOC_SCHEMA_REF,
} goc_schema_kind_t;

/**
 * goc_schema_field_t — descriptor for a named field in goc_schema_dict().
 *
 * key         : field name; must be a string literal or stable pointer.
 * schema      : schema the field value must satisfy.
 * optional    : if true, the field may be absent from the object.
 * title       : short human-readable label (metadata only, not validated).
 * description : longer human-readable description (metadata only).
 * default_val : default value used when the field is absent (metadata only).
 * deprecated  : advisory flag; does not affect validation.
 * read_only   : advisory flag; does not affect validation.
 * write_only  : advisory flag; does not affect validation.
 */
typedef struct {
    const char* key;
    goc_schema* schema;
    bool        optional;
    const char* title;
    const char* description;
    void*       default_val;
    bool        deprecated;
    bool        read_only;
    bool        write_only;
} goc_schema_field_t;

/**
 * goc_schema_item_t — descriptor for one positional element in goc_schema_tuple().
 *
 * schema : schema the element at this position must satisfy.
 */
typedef struct {
    goc_schema* schema;
} goc_schema_item_t;

/**
 * goc_schema_dep_req_t — a dependentRequired entry: if key is present, all
 * property names listed in required must also be present.
 *
 * key      : the triggering property name.
 * required : goc_array of const char* property names that become required.
 */
typedef struct {
    const char* key;
    goc_array*  required;
} goc_schema_dep_req_t;

/**
 * goc_schema_dep_schema_t — a dependentSchemas entry: if key is present, the
 * object must also satisfy schema.
 *
 * key    : the triggering property name.
 * schema : additional schema applied when key is present.
 */
typedef struct {
    const char* key;
    goc_schema* schema;
} goc_schema_dep_schema_t;

/**
 * goc_schema_pattern_prop_t — a patternProperties entry: properties whose
 * names match pattern must satisfy schema.
 *
 * pattern : POSIX ERE pattern string.
 * schema  : schema applied to matching property values.
 * regex   : compiled regex (populated lazily by the validator; set to NULL
 *           at construction time).
 */
typedef struct {
    const char* pattern;
    goc_schema* schema;
    regex_t*    regex;
} goc_schema_pattern_prop_t;

/**
 * goc_schema_dict_opts_t — additional constraints for goc_schema_dict().
 *
 * strict          : if true, no properties beyond the declared fields are allowed.
 * min_properties  : minimum number of properties (0 = no lower bound).
 * max_properties  : maximum number of properties (0 = no upper bound).
 * property_names  : schema each property name must satisfy; NULL = unrestricted.
 * pattern_props   : goc_array of goc_schema_pattern_prop_t*; may be NULL.
 * dep_required    : goc_array of goc_schema_dep_req_t*; may be NULL.
 * dep_schemas     : goc_array of goc_schema_dep_schema_t*; may be NULL.
 */
typedef struct {
    bool       strict;
    size_t     min_properties;
    size_t     max_properties;
    goc_schema* property_names;
    goc_array* pattern_props;
    goc_array* dep_required;
    goc_array* dep_schemas;
} goc_schema_dict_opts_t;

/**
 * goc_schema_meta_t — non-validating annotations attached to a schema.
 *
 * title       : short human-readable label.
 * description : longer human-readable description.
 * default_val : suggested default value (not enforced by the validator).
 * examples    : goc_array of example values (not enforced by the validator).
 * deprecated  : advisory flag.
 * read_only   : advisory flag.
 * write_only  : advisory flag.
 */
typedef struct {
    const char* title;
    const char* description;
    void*       default_val;
    goc_array*  examples;
    bool        deprecated;
    bool        read_only;
    bool        write_only;
} goc_schema_meta_t;

/** Opaque validation error returned by goc_schema_validate(). */
typedef struct goc_schema_error goc_schema_error;

/** Return the dot-separated path to the failing value (e.g. ".db.port"). */
const char* goc_schema_error_path   (const goc_schema_error*);
/** Return a human-readable description of the violated constraint. */
const char* goc_schema_error_message(const goc_schema_error*);

/**
 * Singleton schemas for the base types.
 *
 * goc_schema_null()  : accepts only null (NULL pointer).
 * goc_schema_bool()  : accepts only bool* values (created with goc_box(bool, …)).
 * goc_schema_int()   : accepts any boxed signed integer (int, short, long, long long)
 *                      and char/signed char values.
 * goc_schema_uint()  : accepts any boxed unsigned integer (unsigned int, unsigned
 *                      short, unsigned long, unsigned long long) and unsigned char
 *                      values.
 * goc_schema_byte()  : accepts only boxed char or signed char values.
 * goc_schema_ubyte() : accepts only boxed unsigned char values.
 * goc_schema_real()  : accepts float, double, or long double boxed values.
 * goc_schema_complex(): accepts any boxed complex value (float complex,
 *                       double complex, or long double complex).
 * goc_schema_number(): accepts any boxed numeric value (int, uint, byte, ubyte,
 *                       real, or complex).
 * goc_schema_str()   : accepts only null-terminated char* values.
 * goc_schema_any()   : accepts any non-null value regardless of type.
 *
 * Lazily initialised on first call; GC-managed and must not be freed by callers.
 */
goc_schema* goc_schema_null(void);
goc_schema* goc_schema_bool(void);
goc_schema* goc_schema_int(void);
goc_schema* goc_schema_uint(void);
goc_schema* goc_schema_byte(void);
goc_schema* goc_schema_ubyte(void);
goc_schema* goc_schema_real(void);
goc_schema* goc_schema_complex(void);
goc_schema* goc_schema_number(void);
goc_schema* goc_schema_str(void);
goc_schema* goc_schema_any(void);
/** Singleton: accepts any goc_array* regardless of element type. */
goc_schema* goc_schema_arr_any(void);
/** Singleton: accepts any goc_dict* regardless of field contents. */
goc_schema* goc_schema_dict_any(void);

/* Scalar constructors */
/** Create a bool const schema. */
goc_schema* goc_schema_bool_const(bool val);
/** Create an int const schema. */
goc_schema* goc_schema_int_const(int64_t val);
/** Create a real const schema. */
goc_schema* goc_schema_real_const(double val);
/** Create a string const schema. */
goc_schema* goc_schema_str_const(const char* val);

/** Create an int minimum schema. */
goc_schema* goc_schema_int_min(int64_t min);
/** Create an int maximum schema. */
goc_schema* goc_schema_int_max(int64_t max);
/** Create an int range schema. */
goc_schema* goc_schema_int_range(int64_t min, int64_t max);
/** Create an int enum schema. */
goc_schema* goc_schema_int_enum(goc_array* vals);

/** Create a real minimum schema. */
goc_schema* goc_schema_real_min(long double min);
/** Create a real maximum schema. */
goc_schema* goc_schema_real_max(long double max);
/** Create a real range schema. */
goc_schema* goc_schema_real_range(long double min, long double max);
/** Create an exclusive real minimum schema. */
goc_schema* goc_schema_real_ex_min(long double min);
/** Create an exclusive real maximum schema. */
goc_schema* goc_schema_real_ex_max(long double max);
/** Create a real multiple-of schema. */
goc_schema* goc_schema_real_multiple(long double factor);

/** Create a string minimum length schema. */
goc_schema* goc_schema_str_min_len(size_t min);
/** Create a string maximum length schema. */
goc_schema* goc_schema_str_max_len(size_t max);
/** Create a string length range schema. */
goc_schema* goc_schema_str_len(size_t min, size_t max);
/** Create a string pattern schema. */
goc_schema* goc_schema_str_pattern(const char* pattern);
/** Create a string enum schema. */
goc_schema* goc_schema_str_enum(goc_array* vals);
/** Create a string format schema. */
goc_schema* goc_schema_str_format(const char* format);

/* Compound schemas */
/** Create a homogeneous array schema. */
goc_schema* goc_schema_arr(goc_schema* elem);
/** Create a bounded array schema. */
goc_schema* goc_schema_arr_len(goc_schema* elem, size_t min, size_t max);
/** Create a unique-items array schema. */
goc_schema* goc_schema_arr_unique(goc_schema* elem);
/**
 * goc_schema_arr_contains() — array schema requiring a matching element.
 *
 * elem         : schema every element must satisfy (NULL = unconstrained).
 * contains     : schema at least one element must satisfy.
 * min_contains : minimum number of elements satisfying contains (0 = no lower bound).
 * max_contains : maximum number of elements satisfying contains (0 = no upper bound).
 */
goc_schema* goc_schema_arr_contains(goc_schema* elem,
                                    goc_schema* contains,
                                    size_t min_contains,
                                    size_t max_contains);
/**
 * goc_schema_tuple() — fixed-length array schema with per-position schemas.
 *
 * items            : goc_array of goc_schema_item_t* (one per position).
 * additional_items : schema applied to elements beyond the declared positions;
 *                    NULL disallows additional items.
 */
goc_schema* goc_schema_tuple(goc_array* items, goc_schema* additional_items);
/**
 * goc_schema_dict() — object (goc_dict) schema with named field constraints.
 *
 * fields : goc_array of goc_schema_field_t* descriptors.
 * opts   : additional constraints (strict mode, min/max properties, etc.).
 */
goc_schema* goc_schema_dict(goc_array* fields, goc_schema_dict_opts_t opts);

/* Conditional / composition */
/**
 * goc_schema_if() — conditional schema (if/then/else).
 *
 * super : parent schema for the conditional schema.
 * cond  : the condition schema applied first.
 * then_ : applied when cond passes; NULL = no additional constraint on pass.
 * else_ : applied when cond fails; NULL = no additional constraint on fail.
 */
goc_schema* goc_schema_if(goc_schema* super,
                           goc_schema* cond,
                           goc_schema* then_,
                           goc_schema* else_);
/** Create a not schema. */
goc_schema* goc_schema_not(goc_schema* super, goc_schema* schema);
/**
 * Create a predicate schema — valid iff fn(val) returns true.
 * @param super parent schema for this predicate.
 * @param fn   callback receiving the boxed value; must not be NULL.
 * @param name human-readable name used in error messages (may be NULL).
 */
goc_schema* goc_schema_predicate(goc_schema* super, bool (*fn)(void* val), const char* name);
/** Internal helper: create an any_of composition schema from a schema array. */
goc_schema* _goc_schema_any_of_impl(goc_schema* super, goc_array* schemas);
/** Internal helper: create a one_of composition schema from a schema array. */
goc_schema* _goc_schema_one_of_impl(goc_schema* super, goc_array* schemas);
/** Internal helper: create an all_of composition schema from a schema array. */
goc_schema* _goc_schema_all_of_impl(goc_schema* super, goc_array* schemas);

/* Ref */
/** Allocate a recursive schema reference cell. */
goc_schema_ref* goc_schema_ref_make(void);
goc_schema_ref* goc_schema_ref_make_of(goc_schema* super);
/** Set the target of a recursive schema reference. */
void            goc_schema_ref_set(goc_schema_ref*, goc_schema*);
/** Get the target of a recursive schema reference. */
goc_schema*     goc_schema_ref_get(goc_schema_ref*);

/* Registry */
/** Register a schema under a unique name. Aborts if name is already taken. */
goc_schema* goc_schema_named(const char* name, goc_schema* schema);
/** Look up a registered schema by name. Returns NULL if missing. */
goc_schema* goc_schema_lookup(const char* name);
/** Return the name the schema was registered under, or NULL if unnamed. */
const char* goc_schema_name(const goc_schema* schema);

/**
 * goc_schema_make_tagged() — create a tagged JSON object dict.
 *
 * Returns a goc_dict* with "goc_schema" set to the registered name of schema
 * and "goc_value" set to val. schema must have been registered with
 * goc_schema_named(); aborts if it has no name.
 */
goc_dict* goc_schema_make_tagged(goc_schema* schema, void* val);
/** Return the schema used to encode tagged JSON objects. */
goc_schema* goc_schema_tagged_schema(void);

/* Schema methods */
/** Register an untyped method pointer on a schema by name. */
void  goc_schema_method_set(goc_schema* schema, const char* method_name, void* fn);
/** Look up a registered method pointer on a schema by name. */
void* goc_schema_method_get(const goc_schema* schema, const char* method_name);

/**
 * goc_schema_method(method_name, ret_type, ...)
 *
 * Defines a typed schema method wrapper for a method named by the generated
 * function identifiers. The string key used for lookup is the stringified
 * `method_name`.
 *
 * Example:
 *   goc_schema_method(validate_str, bool, void* val, size_t len);
 *
 * Generates:
 *   typedef bool (*validate_str_fn)(void* val, size_t len);
 *   static inline void validate_str_set(goc_schema* s, validate_str_fn fn) { ... }
 *   static inline validate_str_fn validate_str_get(const goc_schema* s) { ... }
 *
 * Limitation: if the parameter list contains a comma inside a type, such as a
 * function-pointer parameter, typedef that type before invoking the macro.
 */
#define goc_schema_method(method_name, ret_type, ...) \
    typedef ret_type (*method_name##_fn)(__VA_ARGS__); \
    static inline void method_name##_set(goc_schema* schema, method_name##_fn fn) { \
        goc_schema_method_set(schema, #method_name, (void*)fn); \
    } \
    static inline method_name##_fn method_name##_get(const goc_schema* schema) { \
        return (method_name##_fn)goc_schema_method_get(schema, #method_name); \
    }

/* Annotations */
/** Attach metadata to a schema. */
goc_schema*        goc_schema_with_meta(goc_schema*, goc_schema_meta_t meta);
/** Read attached schema metadata. */
goc_schema_meta_t* goc_schema_meta(goc_schema*);

/* Validation */
/**
 * goc_schema_derive() — declare that child is a subtype of parent in the
 * global schema hierarchy.
 *
 * After this call, goc_schema_is_a(child, parent) returns true.
 * Transitivity is resolved at query time; duplicate pairs are harmless.
 * Thread-safe; may be called from any thread at any time after goc_init().
 */
void goc_schema_derive(goc_schema* child, goc_schema* parent);

/**
 * goc_schema_is_a() — return true if child is a declared subtype of parent.
 *
 * Returns false if either argument is NULL.
 * Reflexive: goc_schema_is_a(s, s) is always true.
 * Transitive: if A→B and B→C are declared, goc_schema_is_a(A, C) is true.
 * Thread-safe; concurrent calls proceed in parallel.
 */
bool goc_schema_is_a(goc_schema* child, goc_schema* parent);

/**
 * goc_schema_parent() — return the direct parent schema in the hierarchy.
 *
 * Returns NULL if the schema has no parent or schema is NULL.
 */
goc_schema* goc_schema_parent(const goc_schema* schema);

goc_schema_kind_t goc_schema_kind(const goc_schema* schema);

/**
 * goc_schema_ancestors() — return all ancestor schemas reachable via parent edges.
 *
 * The returned goc_array* contains every transitive parent schema, excluding
 * the original schema itself. Results contain no duplicates.
 */
goc_array* goc_schema_ancestors(goc_schema* schema);

/**
 * goc_schema_descendants() — return all schemas that transitively derive from the target.
 *
 * The returned goc_array* contains every schema that has the target as a direct
 * or indirect parent. Results contain no duplicates.
 */
goc_array* goc_schema_descendants(goc_schema* schema);

/**
 * goc_schema_is_valid() — validate val against schema and return a boolean.
 *
 * Returns true when val satisfies all constraints, false otherwise.
 * If schema is NULL this function aborts.
 */
bool goc_schema_is_valid(goc_schema* schema, void* val);

/**
 * goc_schema_check() — validate val against schema and abort on failure.
 *
 * If validation fails, aborts with a diagnostic message showing the failure
 * path and reason.
 */
void goc_schema_check(goc_schema* schema, void* val);

/**
 * goc_schema_validate() — validate val against schema.
 *
 * Returns NULL when val satisfies all constraints, or a GC-managed
 * goc_schema_error* describing the first violation found.
 * Passing NULL for schema aborts with a diagnostic message.
 */
goc_schema_error* goc_schema_validate(goc_schema*, void* val);

/**
 * goc_schema_dict_of(field, ...) — shorthand for goc_schema_dict() with default opts.
 * Each argument is a goc_schema_field_t literal; no additional options are applied.
 */
#define goc_schema_dict_of(...) \
    goc_schema_dict(goc_array_of_boxed(goc_schema_field_t, __VA_ARGS__), \
                   (goc_schema_dict_opts_t){0})

/**
 * goc_schema_tuple_of(additional_items, item, ...) — shorthand for goc_schema_tuple().
 * Each item argument is a goc_schema_item_t literal.
 * additional_items is passed directly to goc_schema_tuple().
 */
#define goc_schema_tuple_of(add, ...) \
    goc_schema_tuple(goc_array_of_boxed(goc_schema_item_t, __VA_ARGS__), (add))

/** goc_schema_any_of(super, ...) — value must satisfy at least one of the given schemas. */
#define goc_schema_any_of(super, ...)  _goc_schema_any_of_impl(super, goc_array_of(__VA_ARGS__))
/** goc_schema_one_of(super, ...) — value must satisfy exactly one of the given schemas. */
#define goc_schema_one_of(super, ...)  _goc_schema_one_of_impl(super, goc_array_of(__VA_ARGS__))
/** goc_schema_all_of(super, ...) — value must satisfy all of the given schemas. */
#define goc_schema_all_of(super, ...)  _goc_schema_all_of_impl(super, goc_array_of(__VA_ARGS__))

#ifdef __cplusplus
}
#endif

#endif /* GOC_SCHEMA_H */
