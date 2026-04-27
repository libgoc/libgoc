/* goc_dict.h — Public API for libgoc ordered dictionaries
 *
 * A small ordered dictionary mapping null-terminated C strings to `void*`
 * values with insertion-order iteration. All memory is GC-managed.
 *
 * Consumers: #include "goc_dict.h"  (or <goc_dict.h> when installed)
 *
 * Copyright (c) Divyansh Prakash
 *
 * Compile requirements: -std=c11
 * Required defines: -DGC_THREADS  -D_GNU_SOURCE
 *
 * See docs/DICT.md for design rationale and full API documentation.
 */

#ifndef GOC_DICT_H
#define GOC_DICT_H

#include <stddef.h>
#include <stdbool.h>

#include "goc.h"
#include "goc_array.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------------- */

/** Opaque ordered dictionary type. */
typedef struct goc_dict goc_dict;

/** Single dictionary entry used for array conversions. */
typedef struct {
    const char* key;
    void* val;
} goc_dict_entry_t;

/* -------------------------------------------------------------------------
 * Construction
 * ---------------------------------------------------------------------- */

/**
 * goc_dict_make(initial_cap) — Allocate and return a new empty dictionary.
 *
 * initial_cap : hint for the initial hash table capacity. The actual table
 *                size is rounded up to a power of two with a minimum.
 *
 * Returns a GC-managed pointer. Never returns NULL.
 */
goc_dict* goc_dict_make(size_t initial_cap);

/**
 * _goc_dict_of_impl(kvs, n) — Build a new dict from an array of key/value
 * pairs.
 *
 * This function is an implementation detail for goc_dict_of().
 */
goc_dict* _goc_dict_of_impl(const goc_dict_entry_t* kvs, size_t n);

/**
 * _goc_dict_of_boxed_impl(elem_size, pair_size, val_offset, pairs, n) —
 * Build a new dict from inline boxed key/value pairs of scalar type T.
 *
 * This function is an implementation detail for goc_dict_of_boxed().
 */
goc_dict* _goc_dict_of_boxed_impl(size_t elem_size,
                                   size_t pair_size,
                                   size_t val_offset,
                                   goc_boxed_type_t boxed_type,
                                   const void* pairs,
                                   size_t n);

/**
 * _goc_dict_merge_many(dicts, n) — Merge n dictionaries left-to-right.
 *
 * This function is an implementation detail for goc_dict_merge().
 */
goc_dict* _goc_dict_merge_many(goc_dict** dicts, size_t n);

/**
 * goc_dict_zip_c(keys, vals, n) — Build a new dict by pairing keys[i]
 * with vals[i] for 0 <= i < n.
 *
 * Duplicate keys are resolved last-write-wins. Iteration order follows the
 * key array order.
 *
 * Returns a GC-managed dict. Never returns NULL.
 */
goc_dict* goc_dict_zip_c(const char** keys, void** vals, size_t n);

/**
 * _goc_dict_zip_impl(keys, vals) — Build a dict from two goc_array values.
 *
 * The key array must contain char* values and vals must contain void* values.
 * Aborts if the arrays have different lengths.
 */
goc_dict* _goc_dict_zip_impl(const goc_array* keys, const goc_array* vals);

/* -------------------------------------------------------------------------
 * Query
 * ---------------------------------------------------------------------- */

/**
 * goc_dict_contains(d, key) — Return true if key is present in d.
 *
 * Amortized O(1).
 */
bool goc_dict_contains(const goc_dict* d, const char* key);

/**
 * goc_dict_get(d, key, not_found) — Return the value for key or not_found if
 * the key is absent.
 *
 * Amortized O(1).
 */
void* goc_dict_get(const goc_dict* d, const char* key, void* not_found);

/**
 * goc_dict_get_in(d, path, not_found) — Return the value at path within a
 * nested dict/array structure rooted at d.
 *
 * Path syntax matches goc_schema_validate / goc_json_error_path:
 *   .key  — lookup a key in the current dict
 *   .[N]  — lookup an index in the current array
 *   ""   — return d itself
 *
 * Returns not_found on any invalid input, malformed path, missing key, or
 * out-of-bounds index. Precondition: intermediate nodes must be correctly
 * typed as dict or array according to the path.
 */
void* goc_dict_get_in(const goc_dict* d, const char* path, void* not_found);

/**
 * goc_dict_len(d) — Return the number of live entries in the dict.
 *
 * O(1).
 */
size_t goc_dict_len(const goc_dict* d);

/* -------------------------------------------------------------------------
 * Mutation
 * ---------------------------------------------------------------------- */

/**
 * goc_dict_set(d, key, val) — Insert or update key with val.
 *
 * If the key is new, it is appended in insertion order.
 * Amortized O(1).
 */
void goc_dict_set(goc_dict* d, const char* key, void* val);

/**
 * goc_dict_pop(d, key, not_found) — Remove key and return its value.
 *
 * Returns not_found when the key is absent. Amortized O(1).
 */
void* goc_dict_pop(goc_dict* d, const char* key, void* not_found);

/* -------------------------------------------------------------------------
 * Other operations
 * ---------------------------------------------------------------------- */

/**
 * goc_dict_copy(d) — Return a shallow copy of d with the same insertion order.
 *
 * The returned dict shares values by pointer, but the dictionary structure is
 * independent.
 */
goc_dict* goc_dict_copy(const goc_dict* d);

/**
 * goc_dict_select_c(d, keys, n) — Return a new dict containing only the
 * requested keys in the same order as the provided key list.
 *
 * Missing keys are skipped.
 */
goc_dict* goc_dict_select_c(const goc_dict* d, const char** keys, size_t n);

/**
 * goc_dict_entries(d) — Convert a dict into a goc_array of
 * goc_dict_entry_t*.
 *
 * The returned array preserves insertion order and is GC-managed.
 * Nulled slots left by pop() are skipped.
 */
goc_array* goc_dict_entries(const goc_dict* d);

/**
 * goc_dict_keys(d) — Return a goc_array of live keys in insertion order.
 *
 * Nulled slots left by pop() are skipped.
 */
goc_array* goc_dict_keys(const goc_dict* d);

/**
 * goc_dict_vals(d) — Return a goc_array of live values
 * in insertion order of keys.
 */
goc_array* goc_dict_vals(const goc_dict* d);

/**
 * goc_dict_from_entries(entries) — Build a dict from a goc_array of
 * goc_dict_entry_t* entries.
 *
 * Later entries override earlier duplicates.
 */
goc_dict* goc_dict_from_entries(const goc_array* entries);

/* -------------------------------------------------------------------------
 * Macros
 * ---------------------------------------------------------------------- */

/**
 * goc_dict_of(...) — Create a dict from inline {key, val} pairs.
 *
 * The entry count is derived from the argument list.
 */
#define goc_dict_of(...) \
    _goc_dict_of_impl((goc_dict_entry_t[]){__VA_ARGS__}, \
                      sizeof((goc_dict_entry_t[]){__VA_ARGS__}) / \
                      sizeof(goc_dict_entry_t))

#define _GOC_ALIGN_UP(n, a) (((n) + (a) - 1) & ~((a) - 1))

/**
 * goc_dict_of_boxed(T, ...) — Create a dict from inline boxed scalar values.
 *
 * Each scalar value is boxed as type T.
 */
#define goc_dict_of_boxed(T, ...) \
    _goc_dict_of_boxed_impl(sizeof(T), \
        sizeof(struct { const char* k; T v; }), \
        _GOC_ALIGN_UP(sizeof(const char*), _Alignof(T)), \
        GOC_BOXED_TYPE(T), \
        (struct { const char* k; T v; }[]){__VA_ARGS__}, \
        sizeof((struct { const char* k; T v; }[]){__VA_ARGS__}) / \
        sizeof(struct { const char* k; T v; }))

/**
 * goc_dict_get_unboxed(T, d, key, not_found) — Lookup and unbox a dict value.
 *
 * The not_found value is auto-boxed as type T. If the key is absent, the
 * boxed not_found is returned and then unboxed.
 */
#define goc_dict_get_unboxed(T, d, key, not_found) \
    goc_unbox(T, goc_dict_get((d), (key), goc_box(T, (not_found))))

/**
 * goc_dict_get_in_boxed(T, d, path, not_found) — Lookup a nested path and
 * unbox the result as T.
 */
#define goc_dict_get_in_boxed(T, d, path, not_found) \
    goc_unbox(T, goc_dict_get_in((d), (path), goc_box(T, (not_found))))

/**
 * goc_dict_set_boxed(T, d, key, val) — Box val and store it in the dict.
 */
#define goc_dict_set_boxed(T, d, key, val) \
    goc_dict_set((d), (key), goc_box(T, (val)))

/**
 * goc_dict_pop_unboxed(T, d, key, not_found) — Remove a key and unbox its
 * value.
 *
 * The not_found value is auto-boxed as type T. If the key is absent, the
 * boxed not_found is returned and then unboxed.
 */
#define goc_dict_pop_unboxed(T, d, key, not_found) \
    goc_unbox(T, goc_dict_pop((d), (key), goc_box(T, (not_found))))

/**
 * goc_dict_merge(...) — Merge dictionaries left-to-right.
 *
 * Later dictionaries override earlier ones for duplicate keys.
 */
#define goc_dict_merge(...) \
    _goc_dict_merge_many((goc_dict*[]){__VA_ARGS__}, \
                         sizeof((goc_dict*[]){__VA_ARGS__}) / sizeof(goc_dict*))

/**
 * goc_dict_select(d, ...) — Convenience wrapper for goc_dict_select_c.
 */
#define goc_dict_select(d, ...) \
    goc_dict_select_c((d), (const char*[]){__VA_ARGS__}, \
                      sizeof((const char*[]){__VA_ARGS__}) / sizeof(char*))

/**
 * goc_dict_zip(keys_arr, vals_arr) — Build a dict from two goc_array objects.
 *
 * Aborts if the arrays have different lengths.
 */
#define goc_dict_zip(keys_arr, vals_arr) \
    _goc_dict_zip_impl((keys_arr), (vals_arr))

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GOC_DICT_H */
