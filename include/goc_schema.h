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
/* goc_schema_ref embeds goc_schema as its first member. Casting a reference
 * cell to goc_schema* is valid in the implementation, but schema references
 * are treated as opaque by consumers. */
typedef struct goc_schema_ref      goc_schema_ref;
typedef struct goc_schema_registry goc_schema_registry;

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
goc_schema* goc_schema_real_min(double min);
/** Create a real maximum schema. */
goc_schema* goc_schema_real_max(double max);
/** Create a real range schema. */
goc_schema* goc_schema_real_range(double min, double max);
/** Create an exclusive real minimum schema. */
goc_schema* goc_schema_real_ex_min(double min);
/** Create an exclusive real maximum schema. */
goc_schema* goc_schema_real_ex_max(double max);
/** Create a real multiple-of schema. */
goc_schema* goc_schema_real_multiple(double factor);

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
 * cond  : the condition schema applied first.
 * then_ : applied when cond passes; NULL = no additional constraint on pass.
 * else_ : applied when cond fails; NULL = no additional constraint on fail.
 */
goc_schema* goc_schema_if(goc_schema* cond,
                           goc_schema* then_,
                           goc_schema* else_);
/** Create a not schema. */
goc_schema* goc_schema_not(goc_schema* schema);
/**
 * Create a predicate schema — valid iff fn(val) returns true.
 * @param fn   callback receiving the boxed value; must not be NULL.
 * @param name human-readable name used in error messages (may be NULL).
 */
goc_schema* goc_schema_predicate(bool (*fn)(void* val), const char* name);
goc_schema* _goc_schema_any_of_impl(goc_array* schemas);
goc_schema* _goc_schema_one_of_impl(goc_array* schemas);
goc_schema* _goc_schema_all_of_impl(goc_array* schemas);

/* Ref */
/** Allocate a recursive schema reference cell. */
goc_schema_ref* goc_schema_ref_make(void);
/** Set the target of a recursive schema reference. */
void            goc_schema_ref_set(goc_schema_ref*, goc_schema*);
/** Get the target of a recursive schema reference. */
goc_schema*     goc_schema_ref_get(goc_schema_ref*);

/* Registry */
/** Create a named schema registry. */
/**
 * Process-wide shared registry. All built-in scalar schemas are pre-registered
 * under "goc/<name>" (e.g. "goc/int", "goc/real", "goc/str"). User code may
 * register additional schemas here for cross-module lookup.
 */
goc_schema_registry* goc_schema_global_registry(void);

goc_schema_registry* goc_schema_registry_make(void);
/** Add or overwrite a named schema in the registry. */
void                 goc_schema_registry_add(goc_schema_registry*,
                                             const char* name,
                                             goc_schema* schema);
/** Get a named schema from the registry. */
goc_schema*          goc_schema_registry_get(goc_schema_registry*,
                                             const char* name);

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
 * goc_schema_parents() — return direct parents of schema in the hierarchy.
 *
 * Returns a GC-managed goc_array* containing the direct parent schemas.
 * If schema has no parents or schema is NULL, returns an empty array.
 */
goc_array* goc_schema_parents(goc_schema* schema);

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

/** * goc_schema_validate() — validate val against schema.
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

/** goc_schema_any_of(schema, ...) — value must satisfy at least one of the given schemas. */
#define goc_schema_any_of(...)  _goc_schema_any_of_impl(goc_array_of(__VA_ARGS__))
/** goc_schema_one_of(schema, ...) — value must satisfy exactly one of the given schemas. */
#define goc_schema_one_of(...)  _goc_schema_one_of_impl(goc_array_of(__VA_ARGS__))
/** goc_schema_all_of(schema, ...) — value must satisfy all of the given schemas. */
#define goc_schema_all_of(...)  _goc_schema_all_of_impl(goc_array_of(__VA_ARGS__))

#ifdef __cplusplus
}
#endif

#endif /* GOC_SCHEMA_H */
