# `goc_schema` — Value Schemas

A runtime types library for **libgoc** for:
- describing shape of structured data
- defining type hierarchies
- runtime type checking
- runtime polymorphism
- runtime reflection

Schemas operate on plain libgoc types: `goc_dict*`, `goc_array*`, `char*`, boxed scalars.

Compatible with [JSON Schema](#json-schema-compatibility).

All features are exercized by the [JSON library](./JSON.md).

---

## Table of Contents

1. [Quick Example](#quick-example)
2. [Comprehensive Example](#comprehensive-example)
3. [Design](#design)
4. [API Reference](#api-reference)
   - [Scalar Schemas](#scalar-schemas)
   - [Const Schemas](#const-schemas)
   - [Constrained Scalar Schemas](#constrained-scalar-schemas)
   - [Compound Schemas](#compound-schemas)
   - [Conditional Schemas](#conditional-schemas)
   - [Composition Schemas](#composition-schemas)
   - [Predicate Schemas](#predicate-schemas)
   - [Recursive Schemas](#recursive-schemas)
   - [Schema Names](#schema-names)
   - [Schema Hierarchy](#schema-hierarchy)
   - [Annotations](#annotations)
   - [Schema Kind](#schema-kind)
   - [Schema Methods](#schema-methods)
   - [Validation](#validation)
5. [JSON Schema Compatibility](#json-schema-compatibility)
6. [Thread Safety](#thread-safety)
7. [Examples](#examples)
8. [Test Coverage](#test-coverage)

---

## Quick Example

```c
#include "goc_schema.h"

goc_schema* point_schema = goc_schema_dict_of(
    {"x", goc_schema_real()},
    {"y", goc_schema_real()}
);

goc_dict* pt = goc_dict_of_boxed(double, {"x", 1.0}, {"y", 2.0});

goc_schema_error* err = goc_schema_validate(point_schema, pt);
if (err)
    fprintf(stderr, "invalid: %s at %s\n",
            goc_schema_error_message(err), goc_schema_error_path(err));
```

---

## Comprehensive Example

A single schema that exercises every feature: constrained scalars, consts, nested objects with dependent and pattern properties, tuples, arrays with contains, composition, conditionals, strict mode, optional fields, annotations, a recursive ref, and a predicate schema.

```c
#include "goc_schema.h"

/* --- recursive: comment can reference itself ----------------------- */
goc_schema_ref* comment_ref = goc_schema_ref_make();

goc_schema* comment_schema = goc_schema_dict_of(
    {"id",      goc_schema_int_min(1)},
    {"body",    goc_schema_str_len(1, 2000)},
    {"replies", goc_schema_arr((goc_schema*)comment_ref), .optional = true}
);
goc_schema_ref_set(comment_ref, comment_schema);

/* --- geo position: exactly [lon, lat], no extra elements ----------- */
goc_schema* position_schema = goc_schema_tuple_of(NULL,
    {goc_schema_real_range(-180.0, 180.0)},   /* longitude */
    {goc_schema_real_range(-90.0,   90.0)}    /* latitude  */
);

/* --- address: strict, keys must be valid identifiers --------------- */
goc_schema* address_schema = goc_schema_dict(
    goc_array_of_boxed(goc_schema_field_t,
        {"street",  goc_schema_str_len(1, 200)},
        {"city",    goc_schema_str_len(1, 100)},
        {"zip",     goc_schema_str_pattern("^[0-9]{5}(-[0-9]{4})?$")},
        {"country", goc_schema_str_len(2, 2)}
    ),
    (goc_schema_dict_opts_t){
        .strict         = true,
        .property_names = goc_schema_str_pattern("^[a-z_][a-z0-9_]*$"),
        .min_properties = 2,
        .max_properties = 10,
    }
);

/* --- id: accept either a positive integer or a UUID string --------- */
goc_schema* id_schema = goc_schema_any_of(
    goc_schema_any(),
    goc_schema_int_min(1),
    goc_schema_str_format("uuid")
);

/* --- role: one of a fixed set -------------------------------------- */
goc_schema* role_schema = goc_schema_str_enum(
    goc_array_of("admin", "editor", "viewer")
);

/* --- tags: unique strings, at least one must be "verified" --------- */
goc_schema* tags_schema = goc_schema_arr_contains(
    goc_schema_str(),
    goc_schema_str_const("verified"),
    1, 0   /* min_contains=1, max_contains=unlimited */
);

/* --- score: real in [0,1] that must also be a "round" value -------- */
static bool is_round(void* val) {
    if (!goc_schema_is_valid(goc_schema_real(), val)) return false;
    double v = goc_unbox(double, val);
    return fmod(v * 100.0, 1.0) == 0.0;   /* at most two decimal places */
}
goc_schema* score_schema = goc_schema_all_of(
    goc_schema_real(),
    goc_schema_real_range(0.0, 1.0),
    goc_schema_predicate(goc_schema_real(), is_round, "round(2dp)")
);

/* --- user: top-level object ---------------------------------------- */
goc_schema* user_schema = goc_schema_dict(
    goc_array_of_boxed(goc_schema_field_t,
        {"id",               id_schema                         },
        {"username",         goc_schema_str_len(3, 32)         },
        {"email",            goc_schema_str_format("email")    },
        {"age",              goc_schema_int_range(0, 150),    
                             .optional = true,
                             .default_val = goc_box(int64_t, 0)},
        {"score",            score_schema,
                             .optional = true                  },
        {"role",             role_schema,
                             .description = "access level",
                             .default_val = "viewer"           },
        {"address",          address_schema,                  
                             .optional = true                  },
        {"location",         position_schema,                 
                             .optional = true                  },
        {"tags",             tags_schema,                     
                             .optional = true                  },
        {"label",            goc_schema_str(),
                             .optional = true                  },
        {"comments",         goc_schema_arr(comment_schema),  
                             .optional = true                  },
        {"permissions",      goc_schema_arr(goc_schema_str()),  
                             .optional = true,
                             .read_only = true                 },
        {"credit_card",      goc_schema_str(),               
                             .optional = true                  },
        {"billing_address",  goc_schema_str(),               
                             .optional = true                  },
        {"metadata",         goc_schema_any(),               
                             .optional = true,
                             .deprecated = true                }
    ),
    (goc_schema_dict_opts_t){
        /* dependentRequired: if "credit_card" present, "billing_address" is required */
        .dep_required = goc_array_of_boxed(goc_schema_dep_req_t,
            {"credit_card", goc_array_of("billing_address")}
        ),
        /* dependentSchemas: if "location" present, "address" must also be present */
        .dep_schemas = goc_array_of_boxed(goc_schema_dep_schema_t,
            {"location", goc_schema_dict_of({"address", address_schema})}
        ),
        /* pattern properties: any "x_*" key must be a string */
        .pattern_props = goc_array_of_boxed(goc_schema_pattern_prop_t,
            {"^x_", goc_schema_str()}
        ),
    }
);

/* --- attach schema-level annotations -------------------------------- */
user_schema = goc_schema_with_meta(user_schema, (goc_schema_meta_t){
    .title       = "User",
    .description = "A registered user account",
    .examples    = goc_array_of(
        "{\"id\": 1, \"username\": \"alice\", \"role\": \"admin\"}"
    ),
});

/* --- conditional: if score present, label is required -------------- */
goc_schema* scored_user_schema = goc_schema_all_of(
    goc_schema_any(),
    user_schema,
    goc_schema_if(
        goc_schema_any(),
        goc_schema_dict_of({"score", goc_schema_real()}),   /* if score is present */
        goc_schema_dict_of({"label", goc_schema_str()}),    /* then label required */
        NULL                                                /* else: no constraint */
    )
);

/* --- validate ------------------------------------------------------ */
goc_schema_error* err = goc_schema_validate(scored_user_schema, user);
if (err)
    fprintf(stderr, "validation failed at %s: %s\n",
            goc_schema_error_path(err), goc_schema_error_message(err));
```

---

## Design

A `goc_schema` describes what a `void*` value is expected to be: its type, constraints on its value, and for compound types, the schemas of its elements or fields.

Schemas are immutable and GC-managed. They are constructed once and reused freely — the same schema can validate or serialise many values.

### Scalar schemas

Plain scalar schemas (`goc_schema_null()`, `goc_schema_bool()`, `goc_schema_int()`, `goc_schema_real()`, `goc_schema_str()`) are lazily-initialised singletons returned by accessor functions. No allocation is needed to use them.

Const schemas (`goc_schema_str_const`, etc.) match exactly one value. Constrained variants (`goc_schema_int_range`, `goc_schema_str_len`, etc.) allocate a new schema object carrying the constraint. The type check is performed first; the constraint is applied only if the type matches.

### Array schemas

`goc_schema_arr(elem)` describes a homogeneous `goc_array*` where every element matches `elem`. Variants add length bounds, uniqueness, and `contains` constraints.

`goc_schema_tuple_of(...)` describes a heterogeneous `goc_array*` with a fixed number of positional schemas. An optional `additional_items` schema covers elements beyond the declared positions; pass `NULL` to reject extra items or `goc_schema_any()` to allow them freely.

### Object schemas

`goc_schema_dict_of(...)` describes a `goc_dict*` with a fixed set of named fields. Fields are required by default; mark optional with `.optional = true`. `goc_schema_dict` takes a `goc_array*` of boxed `goc_schema_field_t` values and a `goc_schema_dict_opts_t` for strictness, property count bounds, key name validation, pattern properties, and dependent constraints. All sub-lists in opts are likewise `goc_array*`.

### Conditional schemas

`goc_schema_if(super, cond, then_, else_)` applies `then_` when the value is valid against `cond`, and `else_` when it is not. `then_` and `else_` may be `NULL`. The `if` branch is never itself a validation error.

### Composition schemas

`goc_schema_any_of`, `goc_schema_one_of`, `goc_schema_all_of`, and `goc_schema_not` implement boolean logic combinators. All of these macros take an explicit `super` schema as their first argument.

### Recursive schemas

A `goc_schema_ref` is a mutable indirection cell resolved after construction. Only the ref cell is mutable; concrete schemas remain immutable.

### Schema names

`goc_schema_named` and `goc_schema_lookup` provide a process-wide registry for named schemas. Only schemas explicitly registered with `goc_schema_named` are included; anonymous inline schemas are not automatically named.

### Annotations

A `goc_schema_meta_t` carries non-validating metadata — title, description, default value, examples, and access flags. Does not affect validation.

---

## API Reference

### Scalar Schemas

```c
goc_schema* goc_schema_any();      /* matches any value                          */
goc_schema* goc_schema_arr_any();  /* expects any goc_array* (any element type)  */
goc_schema* goc_schema_dict_any(); /* expects any goc_dict* (any field contents) */
goc_schema* goc_schema_tagged_schema(); /* expects tagged JSON objects created by goc_schema_make_tagged() */
goc_schema* goc_schema_null();     /* expects NULL                               */
goc_schema* goc_schema_str();      /* expects char*                              */

goc_schema* goc_schema_number();   /* expects any boxed numeric value            */
goc_schema* goc_schema_complex();  /* expects any complex float value            */
goc_schema* goc_schema_real();     /* expects float/double/long double           */
goc_schema* goc_schema_int();      /* expects signed boxed integers              */
goc_schema* goc_schema_uint();     /* expects unsigned boxed integers            */
goc_schema* goc_schema_byte();     /* expects goc_box(char, …) / signed byte     */
goc_schema* goc_schema_ubyte();    /* expects goc_box(unsigned char, …)          */
goc_schema* goc_schema_bool();     /* expects goc_box(bool, …)                   */
```

Note: plain `char` and `signed char` values are boxed as signed bytes and are accepted by `goc_schema_byte()` and `goc_schema_int()`. `unsigned char` values are boxed as `UBYTE` and are accepted by `goc_schema_ubyte()` and `goc_schema_uint()`.

`goc_schema_number()` accepts any boxed numeric value: signed integers, unsigned integers, bytes, ubytes, reals, and complex numbers. Use it as the broad numeric supertype when the signed/unsigned, integer/real, or real/complex distinctions are not important.

Because `goc_box` preserves the boxed scalar type, schema validation depends on that type tag. For example, values parsed from JSON are boxed as `int64_t` or `uint64_t`; unboxing them as a narrower scalar type such as `int` is not safe even if the numeric value itself fits in `int`.

`stdint.h` aliases (`int8_t`, `int16_t`, `int32_t`, `int64_t`, `uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`, `size_t`, `intptr_t`, etc.) and the portability typedefs (`uchar`, `ushort`, `uint`, `ulong`) are transparent to `goc_box` — they resolve to the underlying base type and receive the correct tag automatically. No explicit arms are needed.

`int_fast8_t` and `int_least8_t` variants map to `GOC_BOXED_TYPE_INT` on all practical platforms (they alias `int`, `long`, or `long long`). Use `goc_schema_int()` (not `goc_schema_byte()`) when validating these types — `goc_schema_int()` is the catch-all that accepts both `INT` and `BYTE` and is correct on every platform. The same applies to all other `int_fast*_t` and `int_least*_t` types.

---

### Const Schemas

A const schema matches exactly one value. The type check is implicit.

```c
goc_schema* goc_schema_bool_const (bool        val);
goc_schema* goc_schema_int_const  (int64_t     val);
goc_schema* goc_schema_real_const (double      val);
goc_schema* goc_schema_str_const  (const char* val);
```

`goc_schema_null()` already serves as the null const; there is no separate `goc_schema_null_const`.

---

### Constrained Scalar Schemas

Constrained schemas perform a type check first, then apply the named constraint. All return a GC-managed `goc_schema*`.

#### Integer constraints

```c
goc_schema* goc_schema_int_min   (int64_t min);
goc_schema* goc_schema_int_max   (int64_t max);
goc_schema* goc_schema_int_range (int64_t min, int64_t max);
goc_schema* goc_schema_int_enum  (goc_array* vals);             /* goc_array of goc_box(int64_t, …) */
```

#### Number (real) constraints

```c
goc_schema* goc_schema_real_min      (long double min);
goc_schema* goc_schema_real_max      (long double max);
goc_schema* goc_schema_real_range    (long double min, long double max);
goc_schema* goc_schema_real_ex_min   (long double min);              /* exclusive */
goc_schema* goc_schema_real_ex_max   (long double max);              /* exclusive */
goc_schema* goc_schema_real_multiple (long double factor);
```

#### String constraints

```c
goc_schema* goc_schema_str_min_len (size_t min);
goc_schema* goc_schema_str_max_len (size_t max);
goc_schema* goc_schema_str_len     (size_t min, size_t max);
goc_schema* goc_schema_str_pattern (const char* pattern);       /* POSIX ERE */
goc_schema* goc_schema_str_enum    (goc_array* vals);           /* goc_array of char* */
goc_schema* goc_schema_str_format  (const char* format);
```

Recognised `format` names: `"date-time"`, `"date"`, `"time"`, `"duration"`, `"email"`, `"uri"`, `"uuid"`, `"ipv4"`, `"ipv6"`. Unknown format names are ignored, matching JSON Schema's advisory semantics.

---

### Compound Schemas

#### Homogeneous array

```c
goc_schema* goc_schema_arr        (goc_schema* elem);
goc_schema* goc_schema_arr_len    (goc_schema* elem, size_t min, size_t max);
goc_schema* goc_schema_arr_unique (goc_schema* elem);
goc_schema* goc_schema_arr_contains(goc_schema* elem,
                                       goc_schema* contains,
                                       size_t        min_contains,
                                       size_t        max_contains);
```

`goc_schema_arr_contains` validates that every element matches `elem` and that between `min_contains` and `max_contains` elements match `contains`. Pass `max_contains = 0` for no upper bound. Pass `goc_schema_any()` for `elem` if only the `contains` constraint matters.

#### Tuple (heterogeneous array)

```c
typedef struct {
    goc_schema* schema;
} goc_schema_item_t;

goc_schema* goc_schema_tuple(goc_array* items, goc_schema* additional_items);
```

```c
#define goc_schema_tuple_of(additional, ...)
```

`items` is a `goc_array*` of boxed `goc_schema_item_t`. The macro form is preferred for inline use. Pass `NULL` for `additional_items` to reject elements beyond the declared positions; pass `goc_schema_any()` to allow them freely.

```c
/* example — a (lon, lat) pair, no extra elements */
goc_schema* coord = goc_schema_tuple_of(NULL,
    {goc_schema_real()},
    {goc_schema_real()}
);
```

Each element is wrapped in `{…}` because `goc_schema_item_t` is a struct — the braces allow future fields to be added with zero-initialisation at existing call sites.

#### Object

```c
typedef struct {
    const char*   key;
    goc_schema*   schema;
    bool          optional;      /* default false — field is required */

    /* annotations (nullable; do not affect validation) */
    const char*   title;
    const char*   description;
    void*         default_val;   /* GC-managed; type must match schema */
    bool          deprecated;
    bool          read_only;
    bool          write_only;
} goc_schema_field_t;
```

```c
typedef struct {
    const char*  key;
    goc_array*   required;       /* goc_array of char* — fields required when key is present */
} goc_schema_dep_req_t;

typedef struct {
    const char*   key;
    goc_schema* schema;        /* schema the object must satisfy when key is present */
} goc_schema_dep_schema_t;

typedef struct {
    const char*   pattern;       /* POSIX ERE — matched against key strings */
    goc_schema*   schema;
    regex_t*      regex;         /* compiled lazily by the validator — leave NULL at construction */
} goc_schema_pattern_prop_t;

typedef struct {
    bool           strict;           /* reject extra keys; default false  */
    size_t         min_properties;   /* 0 = no constraint                 */
    size_t         max_properties;   /* 0 = no constraint                 */
    goc_schema*  property_names;   /* schema validated against each key */
    goc_array*     pattern_props;    /* goc_array of boxed goc_schema_pattern_prop_t */
    goc_array*     dep_required;     /* goc_array of boxed goc_schema_dep_req_t      */
    goc_array*     dep_schemas;      /* goc_array of boxed goc_schema_dep_schema_t   */
} goc_schema_dict_opts_t;

goc_schema* goc_schema_dict(goc_array* fields, goc_schema_dict_opts_t opts);
```

```c
#define goc_schema_dict_of(...)
```

`goc_schema_dict_of({"key", schema}, ...)` is the preferred inline form. It calls `goc_schema_dict` with a `goc_array_of_boxed(goc_schema_field_t, …)` internally. All opts default to zero.

```c
/* example */
goc_schema* s = goc_schema_dict_of(
    {"host",  goc_schema_str()},
    {"port",  goc_schema_int_range(1, 65535)},
    {"debug", goc_schema_bool(), .optional = true}
);
```

**`patternProperties`** — each key in the dict is tested against every pattern in `pattern_props`; if it matches, the value is validated against the corresponding schema. A key may match multiple patterns; all matching schemas must pass. Evaluated independently of `fields`.

**`propertyNames`** — every key string in the dict is validated against `property_names`. Useful for enforcing naming conventions.

**`dependentRequired`** — if the dict contains the trigger key, all keys listed in `required` must also be present.

**`dependentSchemas`** — if the dict contains the trigger key, the entire dict must also satisfy the given schema.

---

### Conditional Schemas

```c
goc_schema* goc_schema_if(goc_schema* super,
                             goc_schema* cond,
                             goc_schema* then_,   /* NULL = no constraint on match    */
                             goc_schema* else_);  /* NULL = no constraint on mismatch */
```

Validates `cond` in test mode (failure is not an error). If the value passes `cond`, validates against `then_`; otherwise validates against `else_`. Typically composed inside `goc_schema_all_of`:

```c
goc_schema* s = goc_schema_all_of(
    goc_schema_any(),
    base_schema,
    goc_schema_if(goc_schema_any(), cond, then_, else_)
);
```

---

### Composition Schemas

```c
/* public macros — preferred interface */
#define goc_schema_any_of(super, ...)   /* valid against >= 1 */
#define goc_schema_one_of(super, ...)   /* valid against == 1 */
#define goc_schema_all_of(super, ...)   /* valid against all  */

goc_schema* goc_schema_not(goc_schema* super, goc_schema* schema);    /* must not be valid  */

/* internal — do not call directly */
goc_schema* _goc_schema_any_of_impl(goc_schema* super, goc_array* schemas);
goc_schema* _goc_schema_one_of_impl(goc_schema* super, goc_array* schemas);
goc_schema* _goc_schema_all_of_impl(goc_schema* super, goc_array* schemas);
```

The macros build a `goc_array*` of `goc_schema*` from their arguments and call the corresponding `_impl` function. `schemas` elements are `goc_schema*` — not boxed, since schema pointers are already GC-managed.

```c
/* example — accept int or string */
goc_schema* int_or_str = goc_schema_any_of(goc_schema_any(), goc_schema_int(), goc_schema_str());
```

For `goc_schema_one_of`, validation collects all matching sub-schemas and fails if the count is not exactly 1. The error message names which schemas matched.

---

### Predicate Schemas

```c
goc_schema* goc_schema_predicate(goc_schema* super, bool (*fn)(void* val), const char* name);
```

Valid iff `fn(val)` returns `true`. `fn` receives the raw boxed value and must not be `NULL`. `name` is used in error messages (`"value failed <name>"`); pass `NULL` to fall back to `"predicate"`.

Use `goc_schema_is_valid` inside `fn` to perform type guards before inspecting the value:

```c
static bool is_prime(void* val) {
    if (!goc_schema_is_valid(goc_schema_int(), val)) return false;
    int64_t n = goc_unbox(int64_t, val);
    if (n < 2) return false;
    for (int64_t i = 2; i * i <= n; i++)
        if (n % i == 0) return false;
    return true;
}

goc_schema* prime = goc_schema_predicate(goc_schema_int(), is_prime, "prime");
```

Predicate schemas compose with all other schema types:

```c
/* positive prime: type check via all_of */
goc_schema* pos_prime = goc_schema_all_of(
    goc_schema_int(),
    goc_schema_int_min(2),
    goc_schema_predicate(goc_schema_int(), is_prime, "prime")
);

/* place in the type hierarchy */
goc_schema_derive(prime, goc_schema_int());
assert(goc_schema_is_a(prime, goc_schema_int()));
```

---

### Recursive Schemas

A `goc_schema_ref` is an indirection cell that can be pointed at any `goc_schema*` after construction. Pass it anywhere a `goc_schema*` is expected.

Use `goc_schema_ref_make_of(super)` to create a typed reference whose effective parent is `super`.

```c
goc_schema_ref* goc_schema_ref_make(void);
goc_schema_ref* goc_schema_ref_make_of(goc_schema* super);
void            goc_schema_ref_set (goc_schema_ref*, goc_schema*);
goc_schema*     goc_schema_ref_get (goc_schema_ref*);
```

`goc_schema_ref_set` may only be called once; a second call is a no-op and logs a warning. An unresolved ref fails validation with `"unresolved schema ref"`.

```c
/* example — tree node */
goc_schema_ref* node_ref    = goc_schema_ref_make_of(goc_schema_dict_any());
goc_schema*     node_schema = goc_schema_dict_of(
    {"value",    goc_schema_int()},
    {"children", goc_schema_arr((goc_schema*)node_ref)}
);
goc_schema_ref_set(node_ref, node_schema);
```

---

### Schema Names

`goc_schema_named` and `goc_schema_lookup` provide a process-wide name registry for schemas. Only schemas explicitly registered with `goc_schema_named` are included; anonymous generated schemas are not automatically named.

```c
goc_schema* goc_schema_named(const char* name, goc_schema* schema);
goc_schema* goc_schema_lookup(const char* name);
```

`goc_schema_named` aborts if the name is already taken or if the schema already has a name. `goc_schema_lookup` returns `NULL` if the name is not registered.

```c
goc_schema* user_schema = goc_schema_dict_of(
    {"id", goc_schema_int()},
    {"name", goc_schema_str()}
);

goc_schema_named("myapp/user", user_schema);
assert(goc_schema_lookup("myapp/user") == user_schema);
```

### Tagged JSON helpers

`goc_schema_make_tagged()` and `goc_schema_tagged_schema()` support custom JSON serialization for tagged objects. Use `goc_schema_make_tagged()` inside a custom `to_json` callback to create a tagged value with a registered schema name, and use `goc_schema_tagged_schema()` as the schema passed to `goc_json_stringify()` when serializing that tagged value.

```c
goc_dict*   goc_schema_make_tagged(goc_schema* schema, void* val);
goc_schema* goc_schema_tagged_schema(void);
```

Built-in schemas are pre-registered at initialization under the `goc/` prefix:

| Name | Schema |
|------|--------|
| `"goc/any"`     | `goc_schema_any()`     |
| `"goc/null"`    | `goc_schema_null()`    |
| `"goc/bool"`    | `goc_schema_bool()`    |
| `"goc/int"`     | `goc_schema_int()`     |
| `"goc/uint"`    | `goc_schema_uint()`    |
| `"goc/byte"`    | `goc_schema_byte()`    |
| `"goc/ubyte"`   | `goc_schema_ubyte()`   |
| `"goc/number"`  | `goc_schema_number()`  |
| `"goc/real"`    | `goc_schema_real()`    |
| `"goc/complex"` | `goc_schema_complex()` |
| `"goc/str"`     | `goc_schema_str()`     |
| `"goc/arr_any"` | `goc_schema_arr_any()` |
| `"goc/dict_any"`| `goc_schema_dict_any()` |

---

### Schema Hierarchy

A global derivation graph records declared subtype relationships between schemas. No object needs to be allocated — the graph is process-wide.

```c
void        goc_schema_derive     (goc_schema* child, goc_schema* parent);
bool        goc_schema_is_a       (goc_schema* child, goc_schema* parent);
goc_schema* goc_schema_parent     (const goc_schema* schema);
goc_array*  goc_schema_ancestors  (goc_schema* schema);
goc_array*  goc_schema_descendants(goc_schema* schema);
```

`goc_schema_parent` returns the direct parent schema declared via `goc_schema_derive`, or `NULL` if none was declared (including `goc_schema_any()` itself).

`goc_schema_is_a` is reflexive (`goc_schema_is_a(s, s)` is always `true`) and transitive (declaring `A→B` and `B→C` is sufficient for `goc_schema_is_a(A, C)` to return `true`).

`goc_schema_ancestors` returns a `goc_array*` of every transitive parent schema reachable from `schema` via parent edges, excluding `schema` itself. No duplicates.

`goc_schema_descendants` returns a `goc_array*` of every schema that transitively derives from `schema` (i.e., every schema for which `goc_schema_is_a(s, schema)` would be `true`), excluding `schema` itself. No duplicates.

All four functions are thread-safe.

Example — numeric tower:

```c
goc_schema_derive(goc_schema_number(),  goc_schema_any());
goc_schema_derive(goc_schema_complex(), goc_schema_number());
goc_schema_derive(goc_schema_real(),    goc_schema_complex());
goc_schema_derive(goc_schema_int(),     goc_schema_number());
goc_schema_derive(goc_schema_uint(),    goc_schema_number());
goc_schema_derive(goc_schema_byte(),    goc_schema_int());
goc_schema_derive(goc_schema_ubyte(),   goc_schema_uint());
goc_schema_derive(goc_schema_bool(),    goc_schema_ubyte());

assert(goc_schema_is_a(goc_schema_byte(),  goc_schema_int()));     /* true  — direct     */
assert(goc_schema_is_a(goc_schema_byte(),  goc_schema_number()));  /* true  — transitive */
assert(goc_schema_is_a(goc_schema_real(),  goc_schema_complex())); /* true  — direct     */
assert(goc_schema_is_a(goc_schema_int(),   goc_schema_int()));     /* true  — reflexive  */
```

---


### Annotations

Annotations carry non-validating metadata. Attached to a schema with `goc_schema_with_meta`; read back with `goc_schema_meta`. Validation ignores them.

```c
typedef struct {
    const char*  title;          /* short human-readable label              */
    const char*  description;    /* longer prose                            */
    void*        default_val;    /* GC-managed default; type matches schema */
    goc_array*   examples;       /* goc_array of char* — example strings    */
    bool         deprecated;
    bool         read_only;
    bool         write_only;
} goc_schema_meta_t;

goc_schema*      goc_schema_with_meta(goc_schema* schema, goc_schema_meta_t meta);
goc_schema_meta_t* goc_schema_meta     (goc_schema* schema);  /* NULL if none attached */
```

### Schema Kind

The schema kind identifies the general category of a schema.

```c
goc_schema_kind_t goc_schema_kind(const goc_schema* schema);
```

The returned kind may be one of:
- `GOC_SCHEMA_ANY`
- `GOC_SCHEMA_NULL`
- `GOC_SCHEMA_BOOL`
- `GOC_SCHEMA_INT`
- `GOC_SCHEMA_UINT`
- `GOC_SCHEMA_BYTE`
- `GOC_SCHEMA_UBYTE`
- `GOC_SCHEMA_REAL`
- `GOC_SCHEMA_NUMBER`
- `GOC_SCHEMA_COMPLEX`
- `GOC_SCHEMA_STR`
- `GOC_SCHEMA_ARR`
- `GOC_SCHEMA_TUPLE`
- `GOC_SCHEMA_DICT`
- `GOC_SCHEMA_CONST`
- `GOC_SCHEMA_CONSTRAINED`
- `GOC_SCHEMA_COMPOSITION`
- `GOC_SCHEMA_PREDICATE`
- `GOC_SCHEMA_REF`

Constrained and const schemas return their abstract kind rather than their underlying base kind.

The schema kind is also useful for library integrations: it identifies a schema's abstract runtime category for validation and inherited dispatch such as `goc_json` serialization.

### Schema Methods

Schemas may carry named function pointers for extensible behavior.

```c
void  goc_schema_method_set(goc_schema* schema, const char* method_name, void* fn);
void* goc_schema_method_get(const goc_schema* schema, const char* method_name);
```

A typed helper macro is also provided for common method definition patterns:

```c
#define goc_schema_method(method_name, ret_type, ...)
```

It defines a function-pointer typedef and typed setter/getter helpers whose lookup key is the stringified `method_name`.
The helper names and method lookup key are now derived from the same identifier.

```c
goc_schema_method(goc_json_to_json, char*, void* val);

/* expands to: */
/* typedef char* (*goc_json_to_json_fn)(void* val); */
/* static inline void goc_json_to_json_set(goc_schema* s, goc_json_to_json_fn fn) { ... } */
/* static inline goc_json_to_json_fn goc_json_to_json_get(const goc_schema* s) { ... } */
```

`goc_schema_method_get` walks the parent chain: if the schema does not define `method_name`, the parent is checked, then the parent's parent, and so on up to `any()`.

---

### Validation

```c
/* Opaque error type — inspect via accessors below. */
typedef struct goc_schema_error goc_schema_error;
const char* goc_schema_error_path   (const goc_schema_error*);
const char* goc_schema_error_message(const goc_schema_error*);

goc_schema_error* goc_schema_validate(goc_schema* schema, void* val);
bool goc_schema_is_valid(goc_schema* schema, void* val);
void goc_schema_check(goc_schema* schema, void* val);
```

- `goc_schema_validate` returns `NULL` on success, or a GC-managed `goc_schema_error*` on the first failure.
    - `goc_schema_error_path` returns a dot-separated path to the failing value (e.g. `".db.port"`, `".[2]"`), or `""` for the root.
    - `goc_schema_error_message` returns a human-readable description of the violated constraint (e.g. `"expected int, got real"`).
- `goc_schema_is_valid` is a convenience wrapper around `goc_schema_validate` that returns `true` when the value satisfies the schema and `false` otherwise.
- `goc_schema_check` aborts on validation failure with a diagnostic message describing the failure path and reason.

| Case | Result |
|---|---|
| Type mismatch | `"expected T, got U"` |
| Required field absent | `"missing required field"` |
| Optional field absent | OK |
| Extra keys (default) | OK |
| Extra keys (strict) | `"unexpected key 'k'"` |
| `minProperties` / `maxProperties` violated | `"expected N–M properties, got K"` |
| `propertyNames` fails for key `k` | `"key 'k': <sub-error>"` |
| `patternProperties` fails | path is `".key"`, sub-error from matched schema |
| `dependentRequired` — missing field | `"field 'y' required when 'x' is present"` |
| `dependentSchemas` — sub-schema fails | sub-error, path preserved |
| Element fails in array | path includes index, e.g. `".[2]"` |
| Tuple element fails | path includes index |
| Extra tuple element (no additional) | `"unexpected element at index N"` |
| `contains` — too few matches | `"expected at least N elements matching contains schema"` |
| `contains` — too many matches | `"expected at most N elements matching contains schema"` |
| `uniqueItems` violated | `"duplicate elements at index I and J"` |
| Constraint violation | e.g. `"expected minimum 0, got -1"` |
| Const mismatch | `"expected const value <V>, got <U>"` |
| `if` / `then` / `else` sub-schema fails | sub-error, path preserved |
| `anyOf` — no match | `"value matched none of N schemas"` |
| `oneOf` — not exactly one | `"value matched M of N schemas, expected exactly 1"` |
| `allOf` — sub-schema fails | sub-error, path preserved |
| `not` — sub-schema passes | `"value must not be valid against schema"` |
| Unresolved ref | `"unresolved schema ref"` |

---

## JSON Schema Compatibility

`goc_schema` maps directly to [JSON Schema](https://json-schema.org/) draft 2020-12.

| `goc_schema` | JSON Schema equivalent |
|---|---|
| `goc_schema_null()` | `{"type": "null"}` |
| `goc_schema_bool()` | `{"type": "boolean"}` |
| `goc_schema_int()` | `{"type": "integer"}` |
| `goc_schema_uint()` | no direct JSON Schema equivalent (`integer` is untyped) |
| `goc_schema_byte()` | `{"type": "integer"}` |
| `goc_schema_ubyte()` | `{"type": "integer"}` |
| `goc_schema_real()` | `{"type": "number"}` |
| `goc_schema_number()` | `{"type": "number"}` (broader — also covers integers) |
| `goc_schema_str()` | `{"type": "string"}` |
| `goc_schema_any()` | `true` (or `{}`) |
| `goc_schema_arr_any()` | `{"type": "array"}` |
| `goc_schema_dict_any()` | `{"type": "object"}` |
| `goc_schema_bool_const(v)` | `{"const": v}` |
| `goc_schema_int_const(v)` | `{"const": v}` |
| `goc_schema_real_const(v)` | `{"const": v}` |
| `goc_schema_str_const(v)` | `{"const": v}` |
| `goc_schema_arr(elem)` | `{"type": "array", "items": elem}` |
| `goc_schema_arr_len(elem, min, max)` | `{"type": "array", "items": elem, "minItems": min, "maxItems": max}` |
| `goc_schema_arr_unique(elem)` | `{"type": "array", "items": elem, "uniqueItems": true}` |
| `goc_schema_arr_contains(elem, c, min, max)` | `{"type": "array", "items": elem, "contains": c, "minContains": min, "maxContains": max}` |
| `goc_schema_tuple_of(add, …)` | `{"type": "array", "prefixItems": […], "items": add}` |
| `goc_schema_dict_of(…)` | `{"type": "object", "properties": {…}, "required": […]}` |
| `.strict = true` | `"additionalProperties": false` |
| `.min_properties` / `.max_properties` | `"minProperties"` / `"maxProperties"` |
| `.property_names` | `"propertyNames"` |
| `.pattern_props` | `"patternProperties"` |
| `.dep_required` | `"dependentRequired"` |
| `.dep_schemas` | `"dependentSchemas"` |
| field `.optional = false` | key in `"required"` array |
| field `.optional = true` | key absent from `"required"` |
| `goc_schema_int_min(min)` | `{"type": "integer", "minimum": min}` |
| `goc_schema_int_max(max)` | `{"type": "integer", "maximum": max}` |
| `goc_schema_int_range(min, max)` | `{"type": "integer", "minimum": min, "maximum": max}` |
| `goc_schema_real_min(min)` | `{"type": "number", "minimum": min}` |
| `goc_schema_real_max(max)` | `{"type": "number", "maximum": max}` |
| `goc_schema_real_range(min, max)` | `{"type": "number", "minimum": min, "maximum": max}` |
| `goc_schema_real_ex_min(min)` | `{"type": "number", "exclusiveMinimum": min}` |
| `goc_schema_real_ex_max(max)` | `{"type": "number", "exclusiveMaximum": max}` |
| `goc_schema_real_multiple(f)` | `{"type": "number", "multipleOf": f}` (precision limited to `double` in JSON output) |
| `goc_schema_str_min_len(min)` | `{"type": "string", "minLength": min}` |
| `goc_schema_str_max_len(max)` | `{"type": "string", "maxLength": max}` |
| `goc_schema_str_len(min, max)` | `{"type": "string", "minLength": min, "maxLength": max}` |
| `goc_schema_str_pattern(p)` | `{"type": "string", "pattern": p}` |
| `goc_schema_str_format(f)` | `{"type": "string", "format": f}` |
| `goc_schema_str_enum(vals)` | `{"enum": […]}` |
| `goc_schema_int_enum(vals)` | `{"enum": […]}` |
| `goc_schema_if(super, c, t, e)` | `{"if": c, "then": t, "else": e}` |
| `goc_schema_any_of(super, …)` | `{"anyOf": […]}` |
| `goc_schema_one_of(super, …)` | `{"oneOf": […]}` |
| `goc_schema_all_of(super, …)` | `{"allOf": […]}` |
| `goc_schema_not(super, schema)` | `{"not": schema}` |
| `goc_schema_ref_make_of(super)` | `{"$ref": "#/$defs/Name"}` |
| `goc_schema_named(name, schema)` / `goc_schema_lookup(name)` | named schema registry |
| `goc_schema_derive` / `goc_schema_is_a` | no JSON Schema equivalent |
| `goc_schema_predicate(super, fn, name)` | no JSON Schema equivalent |
| `goc_schema_with_meta(s, {.title=…})` | `"title"` on the schema |
| `goc_schema_with_meta(s, {.description=…})` | `"description"` |
| `goc_schema_with_meta(s, {.default_val=…})` | `"default"` |
| `goc_schema_with_meta(s, {.examples=…})` | `"examples"` |
| `goc_schema_with_meta(s, {.deprecated=true})` | `"deprecated": true` |
| `goc_schema_with_meta(s, {.read_only=true})` | `"readOnly": true` |
| `goc_schema_with_meta(s, {.write_only=true})` | `"writeOnly": true` |
| field `.deprecated` / `.read_only` / `.write_only` | same, scoped to the property |
| `goc_schema_meta(s)` | reads back the annotations — no JSON Schema analogue |

**Not implemented:** `$dynamicRef` / `$dynamicAnchor` (cross-schema open recursion), `$id` / `$anchor` (URI-based schema identity), `$vocabulary` (meta-schema machinery), `unevaluatedProperties` / `unevaluatedItems` (closed-world composition). These are only relevant for building a JSON Schema meta-validator, not for programmatic schema construction.

---

## Thread Safety

Schema objects are immutable after construction (or after `goc_schema_ref_set` is called) and safe to read concurrently from any number of fibers or OS threads without synchronisation. Validation does not modify the schema or the value being validated.

`goc_schema_derive` and `goc_schema_is_a` are also thread-safe. The global derivation graph is protected by a reader-writer lock; concurrent `goc_schema_is_a` calls proceed in parallel, while `goc_schema_derive` serialises only against ongoing readers and other writers.

`goc_schema_ref_set` is not thread-safe; resolve all refs before sharing schemas across threads.

---

## Examples

### Validating a flat object

```c
goc_schema* cfg_schema = goc_schema_dict_of(
    {"host",    goc_schema_str()},
    {"port",    goc_schema_int_range(1, 65535)},
    {"timeout", goc_schema_int_min(0), .optional = true}
);

goc_schema_error* err = goc_schema_validate(cfg_schema, cfg);
if (err) {
    fprintf(stderr, "config error at %s: %s\n",
            goc_schema_error_path(err), goc_schema_error_message(err));
    exit(1);
}
```

### Validating a nested object

```c
goc_schema* addr_schema = goc_schema_dict_of(
    {"street", goc_schema_str()},
    {"city",   goc_schema_str()}
);

goc_schema* user_schema = goc_schema_dict_of(
    {"name",    goc_schema_str_len(1, 256)},
    {"age",     goc_schema_int_range(0, 150)},
    {"address", addr_schema}
);

goc_schema_error* err = goc_schema_validate(user_schema, user_dict);
/* goc_schema_error_path(err) might be ".address.city" */
```

### Schema hierarchy example

```c
assert(goc_schema_is_a(goc_schema_byte(), goc_schema_int()));
assert(goc_schema_is_a(goc_schema_byte(), goc_schema_number()));
assert(goc_schema_is_a(goc_schema_real(), goc_schema_any()));
assert(!goc_schema_is_a(goc_schema_real(), goc_schema_int()));
```

### Const — exact value match

```c
goc_schema* schema_v1 = goc_schema_dict_of(
    {"version", goc_schema_int_const(1)},
    {"payload", goc_schema_any()}
);
```

### Tuple validation

```c
/* GeoJSON position: [longitude, latitude] — no extra elements */
goc_schema* position = goc_schema_tuple_of(NULL,
    {goc_schema_real_range(-180.0, 180.0)},
    {goc_schema_real_range(-90.0,   90.0)}
);
```

### Array with contains

```c
/* list of strings where at least one must equal "active" */
goc_schema* status_list = goc_schema_arr_contains(
    goc_schema_str(),
    goc_schema_str_const("active"),
    1, 0   /* min_contains=1, no upper bound */
);
```

### Conditional: if/then/else

```c
/* if type=="polygon", vertices is required */
goc_schema* shape_schema = goc_schema_all_of(
    goc_schema_dict_of(
        {"type",     goc_schema_str()},
        {"vertices", goc_schema_arr(goc_schema_real()), .optional = true}
    ),
    goc_schema_if(
        goc_schema_any(),
        goc_schema_dict_of({"type", goc_schema_str_const("polygon")}),
        goc_schema_dict_of({"vertices", goc_schema_arr(goc_schema_real())}),
        NULL
    )
);
```

### Pattern properties

```c
/* all "x_*" extension keys must be strings */
goc_schema* extensible = goc_schema_dict(
    goc_array_of_boxed(goc_schema_field_t,
        {"id",   goc_schema_int()},
        {"name", goc_schema_str()}
    ),
    (goc_schema_dict_opts_t){
        .pattern_props = goc_array_of_boxed(goc_schema_pattern_prop_t,
            {"^x_", goc_schema_str()}
        ),
    }
);
```

### Dependent required

```c
/* if "credit_card" present, "billing_address" is also required */
goc_schema* order_schema = goc_schema_dict(
    goc_array_of_boxed(goc_schema_field_t,
        {"name",            goc_schema_str()},
        {"credit_card",     goc_schema_str(), .optional = true},
        {"billing_address", goc_schema_str(), .optional = true}
    ),
    (goc_schema_dict_opts_t){
        .dep_required = goc_array_of_boxed(goc_schema_dep_req_t,
            {"credit_card", goc_array_of("billing_address")}
        ),
    }
);
```

### Strict object with property name validation

```c
goc_schema* strict_point = goc_schema_dict(
    goc_array_of_boxed(goc_schema_field_t,
        {"x", goc_schema_real()},
        {"y", goc_schema_real()}
    ),
    (goc_schema_dict_opts_t){
        .strict         = true,
        .property_names = goc_schema_str_pattern("^[a-z]$"),
    }
);
```

### Composition: anyOf, oneOf, not

```c
/* anyOf — accept int or string id */
goc_schema* id = goc_schema_any_of(goc_schema_any(), goc_schema_int(), goc_schema_str());

/* oneOf — exactly one of two mutually exclusive shapes */
goc_schema* point_or_circle = goc_schema_one_of(
    goc_schema_any(),
    goc_schema_dict_of({"x", goc_schema_real()}, {"y", goc_schema_real()}),
    goc_schema_dict_of({"cx", goc_schema_real()}, {"r", goc_schema_real_min(0.0)})
);

/* not — reject null values */
goc_schema* non_null = goc_schema_not(goc_schema_any(), goc_schema_null());
```

### Array length bounds and uniqueness

```c
/* bounded length */
goc_schema* short_list = goc_schema_arr_len(goc_schema_str(), 1, 10);

/* all elements distinct */
goc_schema* tag_set = goc_schema_arr_unique(goc_schema_str());
```

### Dependent schemas

`dep_schemas` triggers on key *presence* and validates the whole object against an additional schema.
Unlike `dep_required` (which only checks key presence), this can validate the *values* of those extra fields.

```c
/* if "payment_method" is present, "amount" and "currency" must also be present and valid */
goc_schema* payment_detail = goc_schema_dict_of(
    {"amount",   goc_schema_real_min(0.0)},
    {"currency", goc_schema_str_len(3, 3)}
);

goc_schema* order_schema = goc_schema_dict(
    goc_array_of_boxed(goc_schema_field_t,
        {"item",           goc_schema_str()},
        {"payment_method", goc_schema_str(),       .optional = true},
        {"amount",         goc_schema_real(),       .optional = true},
        {"currency",       goc_schema_str(),        .optional = true}
    ),
    (goc_schema_dict_opts_t){
        .dep_schemas = goc_array_of_boxed(goc_schema_dep_schema_t,
            {"payment_method", payment_detail}
        ),
    }
);
```

For value-based conditions ("if type == 'org'"), use `goc_schema_if` instead — see [Conditional: if/then/else](#conditional-ifthenelse).

### Recursive schema (tree node)

```c
goc_schema_ref* node_ref    = goc_schema_ref_make();
goc_schema*     node_schema = goc_schema_dict_of(
    {"value",    goc_schema_int()},
    {"children", goc_schema_arr((goc_schema*)node_ref)}
);
goc_schema_ref_set(node_ref, node_schema);
```

### Named schemas

```c
goc_schema* reg_schema = goc_schema_dict_of(
    {"street", goc_schema_str()},
    {"city",   goc_schema_str()}
);

goc_schema_named("myapp/address", reg_schema);

goc_schema* looked_up = goc_schema_lookup("myapp/address");
assert(looked_up == reg_schema);
```

### Annotations

```c
goc_schema* point_schema = goc_schema_with_meta(
    goc_schema_dict_of(
        {"x", goc_schema_real(), .description = "horizontal coordinate"},
        {"y", goc_schema_real(), .description = "vertical coordinate",
                               .default_val = goc_box(double, 0.0)}
    ),
    (goc_schema_meta_t){
        .title       = "Point",
        .description = "A 2D point in screen space",
    }
);
```

---

## Test Coverage

The `test_goc_schema` suite validates the `goc_schema` subsystem, including type checking, string/number/array/object constraints, recursive refs, conditional and composite schemas, and registry helpers. It also covers the full scalar-boxing alias matrix for signed/unsigned integer and byte-width boxed values. SC2 covers the global schema hierarchy API: direct and transitive `goc_schema_is_a` queries, reflexivity, non-relationships, NULL safety, and the built-in numeric tower plus `goc_schema_number` validation. SC25 covers the public hierarchy helper APIs: parents, ancestors, and descendants. SC4 covers `goc_schema_check` abort behavior for invalid values.

| Test | Description |
|---|---|
| SC1 | schema registry add and get |
| SC2 | schema hierarchy (derive / is_a) |
| SC3 | scalar schema type checks |
| SC4 | goc_schema_check abort behavior |
| SC5 | boxed scalar alias matrix |
| SC6 | scalar const schema checks |
| SC7 | string length and pattern constraints |
| SC8 | numeric constraint boundaries |
| SC9 | complex number schema support |
| SC10 | string format validation |
| SC11 | array length constraint |
| SC12 | homogeneous and bounded arrays |
| SC13 | array unique value equality |
| SC14 | array contains semantics |
| SC15 | tuple schema behavior |
| SC16 | object optionals and property count boundaries |
| SC17 | object propertyNames and patternProps |
| SC18 | object dependency schemas |
| SC19 | object patternProps regex caching |
| SC20 | object strict mode and required fields |
| SC21 | conditional and composition schemas |
| SC22 | ref and metadata behavior |
| SC23 | error path construction and nested composite schemas |
| SC24 | recursive schema reference |
| SC25 | public schema hierarchy helpers |
| SC26 | predicate schema |
| SC27 | global registry — built-in entries and user registration |
