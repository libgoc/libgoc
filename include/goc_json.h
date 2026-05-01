/* goc_json.h — Public API for libgoc JSON serialization and parsing
 *
 * The JSON subsystem is schema-driven: values are serialized and parsed
 * according to their `goc_schema*` descriptions. Custom behavior can be
 * attached to schemas with `goc_json_set_methods`, which is useful for
 * tagged objects and application-defined JSON payloads.
 *
 * Typical usage:
 *   goc_json_result r = goc_json_parse(json_text);
 *   goc_json_result s = goc_json_stringify(schema, value);
 *
 * The returned `goc_json_result` contains either `res` on success or `err`
 * on failure. Successful stringification returns a GC-managed `char*`.
 */

#ifndef GOC_JSON_H
#define GOC_JSON_H

#include "goc_schema.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque JSON error object returned by failed JSON operations. */
typedef struct goc_json_error goc_json_error;

/** Result of a JSON parse or stringify operation. */
typedef struct { void* res; goc_json_error* err; } goc_json_result;

/* -------------------------------------------------------------------------
 * JSON schema method helpers
 * ---------------------------------------------------------------------- */

goc_schema_method(goc_json_to_json,   char*, void* val);
goc_schema_method(goc_json_read_json, void*, void* val);

/* -------------------------------------------------------------------------
 * Error handling
 * ---------------------------------------------------------------------- */

/**
 * goc_json_error_message() — return a human-readable description of a JSON error.
 *
 * The returned string is GC-managed and valid for the lifetime of the caller's
 * current GC arena.
 */
const char*     goc_json_error_message   (const goc_json_error*);
const char*     goc_json_error_path      (const goc_json_error*);

/* -------------------------------------------------------------------------
 * Parsing
 * ---------------------------------------------------------------------- */

/**
 * goc_json_parse() — parse a JSON string into libgoc values.
 *
 * On success, `res` contains the parsed value and `err` is NULL. On failure,
 * `res` is NULL and `err` points to an error object describing the failure.
 */
goc_json_result goc_json_parse           (const char* json);

/* -------------------------------------------------------------------------
 * Serialization
 * ---------------------------------------------------------------------- */

/**
 * goc_json_stringify() — serialize a libgoc value using a schema.
 *
 * `schema` describes the runtime type of `val` and any nested values.
 * The returned `goc_json_result` contains a GC-managed string on success.
 */
goc_json_result goc_json_stringify       (goc_schema* schema, void* val);

/**
 * goc_json_stringify_pretty() — serialize a libgoc value with indentation.
 *
 * Produces human-readable JSON with 4-space indentation.
 */
goc_json_result goc_json_stringify_pretty(goc_schema* schema, void* val);

/* -------------------------------------------------------------------------
 * Custom JSON methods
 * ---------------------------------------------------------------------- */

/**
 * goc_json_set_methods() — install custom JSON I/O callbacks on a schema.
 *
 * `to_json` is used during serialization, and `read_json` is used during
 * parsing when the schema has custom JSON handling. Pass `NULL` to leave a
 * callback unchanged.
 */
void goc_json_set_methods(goc_schema*           schema,
                          goc_json_to_json_fn   to_json,
                          goc_json_read_json_fn read_json);

#ifdef __cplusplus
}
#endif

#endif /* GOC_JSON_H */
