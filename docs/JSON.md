# `goc_json` — JSON Reader / Writer

A JSON parser and serializer for **libgoc**, wrapping [yyjson](https://github.com/ibireme/yyjson).

[JSON Schema compatible](./SCHEMA.md#json-schema-compatibility).

All memory is managed by the Boehm GC — no `free()` is required or permitted.

---

## Table of Contents

1. [Quick Example](#quick-example)
2. [Design](#design)
3. [API Reference](#api-reference)
   - [Parsing](#parsing)
   - [Serialisation](#serialisation)
4. [Conversion Table](#conversion-table)
5. [Thread Safety](#thread-safety)
6. [Examples](#examples)
7. [Test Coverage](#test-coverage)

---

## Quick Example

```c
#include "goc_schema.h"
#include "goc_json.h"
#include <stdio.h>
#include <stdint.h>

/* Parse JSON string */
goc_json_result r = goc_json_parse("{\"host\": \"localhost\", \"port\": 8080}");
if (r.err)
    printf("%s\n", goc_json_error_message(r.err));

/* Use the parsed object */
goc_dict*   cfg  = r.res;
const char* host = goc_dict_get(cfg, "host", NULL);
int64_t     port = goc_dict_g5et_unboxed(int64_t, cfg, "port", 0);

/* Modify it */
goc_dict_set_boxed(int64_t, cfg, "port", 1337);

/* Define schema */
goc_schema* cfg_schema = goc_schema_dict_of(
    {"host", goc_schema_str()},
    {"port", goc_schema_int()}
);

/* Validate */
goc_schema_error* err = goc_schema_validate(cfg_schema, cfg);
if (err)
    printf("invalid: %s at %s\n",
            goc_schema_error_message(err), 
            goc_schema_error_path(err));

/* Serialize */
goc_json_result out_r = goc_json_stringify(cfg_schema, cfg);
if (out_r.err) {
    printf("error: %s\n", goc_json_error_message(out_r.err));
} else {
    char* out = out_r.res;
    /* {"host":"localhost","port":1337} */
}
```

---

## Design

### Parse path

`goc_json_parse` converts JSON directly into native libgoc types. The caller gets back a `goc_dict*`, `goc_array*`, `char*`, boxed scalar, or `NULL` — no wrapper type to unwrap.

Parsed numeric JSON values are boxed using canonical widest scalar types (`int64_t`, `uint64_t`, `double`). Do not unbox parsed JSON integer values as a narrower scalar type such as `int` unless you explicitly convert from the canonical boxed type.

### Stringify path

Serialising requires knowing the C type of every `void*` value in a dict or array — information C does not carry at runtime. `goc_json_stringify` takes a [`goc_schema*`](./SCHEMA.md) alongside the value; the schema describes the full type tree and is the single source of truth for how each value is encoded. Since `goc_schema` is designed to be JSON Schema–compatible, the same schema can drive both validation and serialisation.

Scalar values should be boxed as canonical widest types: `int64_t` for signed integers, `uint64_t` for unsigned integers, `double` for real numbers, and `double complex` for complex values.

---

## API Reference

### Parsing

```c
typedef struct { void* res; goc_json_error* err; } goc_json_result;

const char* goc_json_error_message(const goc_json_error*);

goc_json_result goc_json_parse(const char* json);
```

| Function | Description |
|---|---|
| `goc_json_parse(json)` | Parse a null-terminated JSON string. On success `err` is `NULL` and `res` holds the root value as its natural libgoc type (see [Conversion Table](#conversion-table)). On failure `err` describes the problem and `res` is `NULL`. A JSON `null` root is a successful parse: `err == NULL`, `res == NULL`. |
| `goc_json_error_message(err)` | Return a human-readable description of the parse failure. |

The returned value and all nested values are GC-managed. The input buffer is not retained after the call.

### Serialisation

```c
typedef char*  (*goc_json_to_json_fn)  (void* val);
typedef void*  (*goc_json_read_json_fn) (void* val);

goc_json_result goc_json_stringify(goc_schema* schema, void* val);
goc_json_result goc_json_stringify_pretty(goc_schema* schema, void* val);
void      goc_json_set_methods(goc_schema* schema,
                               goc_json_to_json_fn to_json,
                               goc_json_read_json_fn read_json);
goc_schema* goc_schema_tagged_schema(void);
goc_dict* goc_schema_make_tagged(goc_schema* schema, void* val);
```

| Function | Description |
|---|---|
| `goc_json_stringify(schema, val)` | Serialize `val` to a compact GC-heap JSON string. `schema` describes the C type of `val` and all nested values. The returned `goc_json_result` contains `res` on success or `err` on failure. |
| `goc_json_stringify_pretty(schema, val)` | Serialize `val` to a human-readable GC-heap JSON string with 4-space indentation. The returned `goc_json_result` contains `res` on success or `err` on failure. |
| `goc_json_set_methods(schema, to_json, read_json)` | Register custom JSON I/O callbacks on `schema`; `to_json` returns a JSON string and `read_json` receives the default-parsed libgoc value for the node. Pass `NULL` to leave one side unchanged. |

`schema` must not be `NULL`. Passing a `val` that does not match `schema` is undefined behaviour; call `goc_schema_is_valid(schema, val)` first if the value is untrusted.

#### Custom schema I/O

`goc_json_set_methods` allows libraries and application code to attach custom JSON serialization and parsing callbacks to a schema.

- `to_json` is called during `goc_json_stringify` / `goc_json_stringify_pretty` and must return a JSON string.
- `read_json` is called during `goc_json_parse` for tagged objects with a matching `"goc_schema"` name, and it receives the parsed `"goc_value"` field directly (e.g. a `char*` for a JSON string, a `goc_dict*` for a JSON object).
- After `read_json` returns, the result is validated against the resolved schema with `goc_schema_is_valid(schema, result)`. If the callback returns a value that does not satisfy the schema, parse fails with a tagged-object parse error.

Methods are inherited through the schema parent chain using `goc_schema_method_get()`: if a schema does not define its own callback, the parent’s callback is used.

#### Tagged-object complex encoding

Complex values and other schema-driven objects are encoded as tagged JSON objects when a custom `to_json` method is registered. The canonical complex format is:

```json
{
    "goc_schema": "goc/complex",
    "goc_value": {
        "real": 1.5,
        "imag": -3.0
    }
}
```

On parse, `goc_json_parse` converts the JSON object to a libgoc value, looks for a `"goc_schema"` field, resolves the registered schema name with `goc_schema_lookup()`, dispatches to the inherited `read_json` method via `goc_schema_method_get()`, and then validates the returned result against the resolved schema before returning the value.

---

## Conversion Table

### Parse (JSON → libgoc)
| JSON type | C type returned |
|---|---|
| object with `"goc_schema": <name>` and a registered `read_json` method | result of `read_json` callback |
| object with `"goc_schema": <name>` but no registered `read_json` method | `goc_dict*` (falls through) |

| JSON type | C type returned |
|---|---|
| `null`                | `NULL` (`err == NULL`, `res == NULL`)                       |
| `true` / `false`      | `goc_box(bool, …)`                                          |
| integer number ≤ `INT64_MAX` | `goc_box(int64_t, …)`                              |
| integer number > `INT64_MAX` | `goc_box(uint64_t, …)`                             |
| floating-point number | `goc_box(double, …)`                                        |
| string                | `char*`                                                     |
| array                 | `goc_array*` (elements follow these same rules recursively) |
| object                | `goc_dict*` (values follow these same rules recursively)    |

### Serialize (libgoc → JSON)

The output format is determined by the schema passed to `goc_json_stringify`.

| Schema | JSON output |
|---|---|
| `goc_schema_null()`    | `null`             |
| `goc_schema_bool()`    | `true` / `false`   |
| `goc_schema_uint()`    | integer            |
| `goc_schema_int()`     | integer            |
| `goc_schema_real()`    | number             |
| `goc_schema_str()`     | `"…"`              |
| `goc_schema_arr(elem)` | `[…]`              |
| `goc_schema_tuple(…)`  | `[…]` (positional) |
| `goc_schema_dict(…)`   | `{…}`              |

---

## Examples

### Parse a nested object

```c
const char* raw =
    "{\"db\": {\"host\": \"localhost\", \"port\": 5432}, \"debug\": true}";

goc_json_result r = goc_json_parse(raw);
if (r.err) { /* handle error */ }

goc_dict*   cfg   = r.res;
goc_dict*   db    = goc_dict_get(cfg, "db", NULL);
const char* host  = goc_dict_get(db,  "host", NULL);
int64_t     port  = goc_dict_get_unboxed(int64_t, db, "port", 0);
bool        debug = goc_dict_get_unboxed(bool,    cfg, "debug", false);
```

### Parse an array of numbers

```c
goc_json_result r = goc_json_parse("[10, 20, 30]");
if (r.err) { /* handle error */ }

goc_array* nums = r.res;
for (int i = 0; i < goc_array_len(nums); i++) {
    int n = goc_array_get_unboxed(int, nums, i);
    printf("%d\n", n);
}
```

### Stringify an object

```c
goc_schema* point_schema = goc_schema_dict_of(
    {"x", goc_schema_real()},
    {"y", goc_schema_real()}
);

goc_dict* point = goc_dict_of_boxed(double,
    {"x",  1.5},
    {"y", -3.0}
);

goc_json_result json_r = goc_json_stringify(point_schema, point);
ASSERT(json_r.err == NULL);
char* json = json_r.res;
/* {"x":1.5,"y":-3.0} */
```

### Stringify an array

```c
goc_schema* tags_schema = goc_schema_arr(goc_schema_str());
goc_array*  tags        = goc_array_of("alpha", "beta", "gamma");
goc_json_result json_r = goc_json_stringify(tags_schema, tags);
ASSERT(json_r.err == NULL);
char*       json        = json_r.res;
/* ["alpha","beta","gamma"] */
```

### Pretty-print

```c
goc_json_result pretty_r = goc_json_stringify_pretty(cfg_schema, cfg);
ASSERT(pretty_r.err == NULL);
char* pretty = pretty_r.res;
/*
{
    "host": "localhost",
    "port": 8080
}
*/
```

### Overriding `to_json` / `read_json` for custom schemas

```c
#include <stdlib.h>
#include <stdio.h>

static goc_schema* int_string_schema;

static bool int_string_not_null(void* x) {
    return x != NULL;
}

static char* int_string_to_json(void* val) {
    int64_t n = goc_unbox(int64_t, val);
    char* s = goc_sprintf("%lld", (long long)n);
    return goc_json_stringify(
        goc_schema_tagged_schema(),
        goc_schema_make_tagged(int_string_schema, s)
    ).res;
}

static void* int_string_read_json(void* val) {
    int64_t n = strtoll((const char*)val, NULL, 10);
    return goc_box(int64_t, n);
}

/* --- setup (once, e.g. after goc_init()) --- */
int_string_schema = goc_schema_named("app/intstring",
    goc_schema_predicate(
        goc_schema_int(),
        int_string_not_null,
        NULL));

goc_json_set_methods(int_string_schema, int_string_to_json, int_string_read_json);

/* Serialize an integer as tagged string JSON: */
void* boxed = goc_box(int64_t, 123456789012345LL);

goc_json_result out_r = goc_json_stringify(int_string_schema, boxed);
ASSERT(out_r.err == NULL);
char* json = out_r.res;
/* {"goc_schema":"app/intstring","goc_value":"123456789012345"} */

/* Parsing the tagged JSON back into an integer triggers int_string_read_json: */
goc_json_result r = goc_json_parse(json);
int64_t parsed = goc_unbox(int64_t, r.res);
```

---

## Test Coverage

The JSON library is exercised by `tests/test_goc_json.c`.

- `J1.` Basic JSON parsing for `null`, booleans, integers, reals, and strings.
- `J2.` Parsing arrays and objects into libgoc containers.
- `J3.` Invalid JSON parse error reporting.
- `J4.` Basic JSON serialization and pretty-print formatting.
- `J5.` Complex tagged object roundtrip for built-in `goc/complex`.
- `J6.` Custom tagged int-string roundtrip via `goc_json_set_methods` and `goc_schema_make_tagged`.
- `J6b.` Invalid tagged-object `read_json` output is rejected during parse.
- `J7.` JSON stringify nested error path reporting.
- `J8.` JSON stringify root error path reporting.
