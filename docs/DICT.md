# `goc_dict` — Ordered Dictionary

An insertion-ordered `string → void*` map for **libgoc**.

Keys are null-terminated C strings; values are `void*` pointers (type-erased, consistent with channels and other libgoc APIs).  All memory is managed by the Boehm GC — no `free()` is required or permitted.

---

## Table of Contents

1. [Quick Example](#quick-example)
2. [Design](#design)
3. [API Reference](#api-reference)
   - [Entry Type](#entry-type)
   - [Construction](#construction)
   - [Lookup](#lookup)
   - [Mutation](#mutation)
   - [Sub-dict](#sub-dict)
   - [Shallow Copy](#shallow-copy)
   - [Merging](#merging)
   - [Array Interop](#array-interop)
   - [Zip Construction](#zip-construction)
4. [Complexity Summary](#complexity-summary)
5. [Thread Safety](#thread-safety)
6. [Examples](#examples)
7. [Test Coverage](#test-coverage)

---

## Quick Example

### Building and querying a dict

```c
#include "goc_dict.h"

goc_dict* d = goc_dict_of_boxed(int, {"x", 10}, {"y", 20});

goc_dict_contains(d, "x");                  /* true                */
goc_dict_get_unboxed(int, d, "x", -1);      /* 10                  */
goc_dict_get_unboxed(int, d, "z", -1);      /* -1 — "z" not found  */
goc_dict_pop_unboxed(int, d, "y", -1);      /* 20 — removes "y"    */
goc_dict_len(d);                            /* 1                   */
```

---

## Design

### Data structure

A `goc_dict` combines a hash table for O(1) average key lookup with an insertion-ordered `goc_array` of keys (`char*`), so iteration always reflects the order in which keys were first inserted.

Each entry is represented by a `goc_dict_entry_t` (see [Entry Type](#entry-type)).  The hash table maps string keys to their values.  Both the table and the key array are backed by GC-managed memory.

### Key semantics

Two keys are equal when `strcmp` returns 0.  Keys must be string literals or GC-heap-allocated strings — the dict stores the pointer directly and does not copy it.

### Amortized O(1) operations

The hash table is resized (doubled) when the load factor exceeds a threshold.  Each resize rehashes all current entries, but the total work across all resizes is proportional to the total number of insertions — giving amortized O(1) for `contains`, `get`, `set`, and `pop`.

### Ordering

Keys are stored in a `goc_array` of `char*` in insertion order.  `pop` removes the key from both the hash table and the key array by nulling out the slot.  Nulled slots are skipped during iteration.

---

## API Reference

### Entry Type

```c
typedef struct {
    char*  key;
    void*  val;
} goc_dict_entry_t;
```

`goc_dict_entry_t` is the unit of exchange when converting between a dict and an array.

### Construction

```c
goc_dict* goc_dict_make(size_t initial_cap);
```

```c
#define goc_dict_of(...)
#define goc_dict_of_boxed(T, ...)
```

`goc_dict_of({"a", p1}, {"b", p2})` is the preferred way to construct a dict from inline `void*` values.  `goc_dict_of_boxed(T, {"a", 1}, {"b", 2})` boxes each scalar value as type `T` automatically.  Both derive the entry count from the argument list.

| Function | Description |
|---|---|
| `goc_dict_make(initial_cap)` | Allocate an empty dict with a pre-allocated hash table rounded up to the next power of two (minimum 8). So `0..8` → 8, `9..16` → 16, etc. Length starts at 0. |
| `goc_dict_of(...)` | Create a dict from inline `{key, val}` pairs. **O(n)**. |
| `goc_dict_of_boxed(T, ...)` | Create a dict from inline `{"key", scalar}` pairs, boxing each value as type `T`. **O(n)**. |

### Lookup

```c
bool   goc_dict_contains(const goc_dict* d, const char* key);
void*  goc_dict_get(const goc_dict* d, const char* key, void* not_found);
size_t goc_dict_len(const goc_dict* d);
```

| Function | Description |
|---|---|
| `goc_dict_contains(d, key)` | Return `true` if `key` is present. **Amortized O(1)**. |
| `goc_dict_get(d, key, not_found)` | Return the value for `key`, or `not_found` if absent. **Amortized O(1)**. |
| `goc_dict_len(d)` | Return the number of live entries. **O(1)**. |

```c
#define goc_dict_get_unboxed(T, d, key, not_found)
```

`goc_dict_get_unboxed(T, d, key, not_found)` retrieves the value for `key` and unboxes it as type `T` in one step, returning `not_found` if absent.

The type parameter `T` must match the type originally used when the stored value was boxed. Unboxing with a different scalar type is undefined behavior.

## Deep lookup

### `goc_dict_get_in(d, path, not_found)`

Returns the value at `path` within the nested dict/array structure rooted at `d`.
`path` uses the same dot-separated format produced by `goc_schema_validate` and
`goc_json_error_path`:

- `.key` — look up `key` in the current dict
- `.[N]` — look up index `N` in the current array
- `""` (empty string) — return `d` itself cast to `void*`

Returns `not_found` on any error: NULL inputs, missing key, out-of-bounds index,
malformed path, or a `NULL` intermediate node when the path continues.
Never aborts.

**Precondition**: every intermediate node must be correctly typed as a dict or
array according to the path. There are no runtime type tags on libgoc values,
so mismatched node types cause undefined behaviour.

### `goc_dict_get_in_boxed(T, d, path, not_found)`

Typed convenience wrapper around `goc_dict_get_in`. Boxes `not_found` before the
lookup and unboxes the result as `T`.

```c
int64_t port = goc_dict_get_in_boxed(int64_t, cfg, ".db.port", 0);
```

### Mutation

```c
void  goc_dict_set(goc_dict* d, const char* key, void* val);
void* goc_dict_pop(goc_dict* d, const char* key, void* not_found);
```

| Function | Description |
|---|---|
| `goc_dict_set(d, key, val)` | Insert or update `key` with `val`. If the key is new, it is appended in insertion order. **Amortized O(1)**. |
| `goc_dict_pop(d, key, not_found)` | Remove `key` and return its value, or `not_found` if absent. **Amortized O(1)**. |

```c
#define goc_dict_set_boxed(T, d, key, val)
#define goc_dict_pop_unboxed(T, d, key, not_found)
```

`goc_dict_set_boxed(T, d, key, val)` boxes `val` and calls `goc_dict_set`. `goc_dict_pop_unboxed(T, d, key, not_found)` removes `key` and returns the value unboxed as `T`, or `not_found` if absent.

### Sub-dict

```c
goc_dict* goc_dict_select_c(const goc_dict* d, const char** keys, size_t n);
```

Returns a new dict containing only the entries whose keys appear in `keys`.  Keys not present in `d` are silently skipped.  Insertion order in the result follows the order of `keys`.  **O(k)** where `k = n`.

```c
#define goc_dict_select(d, ...)
```

`goc_dict_select(d, "a", "b", "c")` is a variadic convenience wrapper that derives the key list and count from the argument list.

### Shallow Copy

```c
goc_dict* goc_dict_copy(const goc_dict* d);
```

Returns a new dict with the same keys and values in the same insertion order.  The entries are shared by pointer — mutations to a value object are visible through both dicts — but adding, updating, or removing keys in one dict does not affect the other.  **O(n)**.

### Merging

```c
#define goc_dict_merge(...)
```

`goc_dict_merge(d1, d2, d3, ...)` merges any number of dicts left-to-right, with each successive dict overriding the previous on key conflicts.  The argument count is derived automatically.  No input is modified.  Called with no arguments, returns an empty dict.  **O(n)** total where `n` is the sum of all dict lengths.

### Array Interop

```c
goc_array*  goc_dict_entries(const goc_dict* d);
goc_array*  goc_dict_keys(const goc_dict* d);
goc_dict*   goc_dict_from_entries(const goc_array* entries);
```

| Function | Description |
|---|---|
| `goc_dict_entries(d)` | Build and return a `goc_array` of `goc_dict_entry_t*` in insertion order by walking the key array and looking up each value in the hash table. Nulled-out slots left by `pop` are skipped. **O(n)**. |
| `goc_dict_keys(d)` | Build and return a `goc_array` of live `char*` keys in insertion order. Nulled-out slots left by `pop` are skipped. **O(n)**. |
| `goc_dict_vals(d)` | Build and return a `goc_array` of live `void*` values in insertion order of keys. **O(n)**. |
| `goc_dict_from_entries(entries)` | Build a dict from a `goc_array` of `goc_dict_entry_t*`. Elements are processed in array order; later entries override earlier ones for duplicate keys. **O(n)**. |

### Zip Construction

```c
goc_dict* goc_dict_zip_c(const char** keys, void** vals, size_t n);
```

Build a dict by pairing `keys[i]` with `vals[i]` for `i` in `[0, n)`.  Insertion order follows the order of `keys`.  Duplicate keys are handled last-write-wins.  **O(n)**.

```c
#define goc_dict_zip(keys_arr, vals_arr)
```

`goc_dict_zip(keys_arr, vals_arr)` is a convenience wrapper for two same-length `goc_array*` arguments (one of `char*`, one of `void*`), deriving `n` from their lengths (aborts if they differ).

---

## Complexity Summary

| Operation | Complexity |
|---|---|
| `goc_dict_of` / `goc_dict_of_boxed` | O(n) |
| `goc_dict_contains` | O(1) amortized |
| `goc_dict_get` / `goc_dict_get_unboxed` | O(1) amortized |
| `goc_dict_set` / `goc_dict_set_boxed` | O(1) amortized |
| `goc_dict_pop` / `goc_dict_pop_unboxed` | O(1) amortized |
| `goc_dict_len` | O(1) |
| `goc_dict_copy` | O(n) |
| `goc_dict_merge` | O(n) total |
| `goc_dict_select_c` / `goc_dict_select` | O(k) |
| `goc_dict_entries` | O(n) |
| `goc_dict_keys` | O(n) |
| `goc_dict_vals` | O(n) |
| `goc_dict_from_entries` | O(n) |
| `goc_dict_zip_c` / `goc_dict_zip` | O(n) |

---


---

## Thread Safety

`goc_dict` is **not** thread-safe.  Concurrent reads and writes require external synchronisation.  Use `goc_mutex` or channels to coordinate access from multiple fibers or OS threads.

---

## Examples

### Counting word frequencies

```c
goc_dict* freq = goc_dict_make(0);

for (int i = 0; i < n_words; i++) {
    const char* w = words[i];
    int count = goc_dict_get_unboxed(int, freq, w, 0);
    goc_dict_set_boxed(int, freq, w, count + 1);
}
```

### Merging two config dicts (override wins)

```c
goc_dict* defaults = goc_dict_make(0);
goc_dict_set_boxed(int, defaults, "timeout", 30);
goc_dict_set_boxed(int, defaults, "retries", 3);

goc_dict* overrides = goc_dict_make(0);
goc_dict_set_boxed(int, overrides, "timeout", 60);

goc_dict* config = goc_dict_merge(defaults, overrides);
/* config["timeout"] == 60, config["retries"] == 3 */
```

### Building from key/value arrays

```c
goc_array* keys = goc_array_of("a", "b", "c");
goc_array* vals = goc_array_of_boxed(int, 1, 2, 3);
goc_dict*  d    = goc_dict_zip(keys, vals);
```

### Selecting a sub-dict

```c
goc_dict* full = goc_dict_make(0);
goc_dict_set(full, "host", "localhost");
goc_dict_set_boxed(int,  full, "port",  8080);
goc_dict_set_boxed(bool, full, "debug", true);

goc_dict* conn = goc_dict_select(full, "host", "port");
/* conn has only "host" and "port" */
```

### Converting to and from an array

```c
goc_dict*  d       = goc_dict_make(0);
goc_dict_set_boxed(int, d, "x", 10);
goc_dict_set_boxed(int, d, "y", 20);

goc_array* entries = goc_dict_entries(d);
/* entries[0]->key == "x", entries[1]->key == "y" */

const goc_array* keys = goc_dict_keys(d);
/* keys[0] == "x", keys[1] == "y" */

goc_dict*  d2      = goc_dict_from_entries(entries);
/* d2 is a fresh dict equivalent to d */
```

---

## Test Coverage

| Test | Description |
|---|---|
| `test_dict_make` | `goc_dict_make()` returns non-NULL empty dict; no membership for missing keys |
| `test_dict_set_get` | `goc_dict_set` stores boxed values; `goc_dict_get` retrieves them; missing keys return not-found default |
| `test_dict_contains` | `goc_dict_contains` reports membership correctly for present and absent keys |
| `test_dict_overwrite` | `goc_dict_set` overwrites existing keys without changing dictionary length |
| `test_dict_pop` | `goc_dict_pop` removes entries and returns boxed values; missing keys return default |
| `test_dict_len` | `goc_dict_len` tracks live entries after insert and pop operations |
| `test_dict_insertion_order` | `goc_dict_entries` preserves insertion order for live entries |
| `test_dict_keys` | `goc_dict_keys` returns live keys in insertion order, skipping popped entries |
| `test_dict_vals` | `goc_dict_vals` returns live values in insertion order of keys |
| `test_dict_to_from_array` | `goc_dict_entries` and `goc_dict_from_entries` round-trip preserves contents |
| `test_dict_copy` | `goc_dict_copy` returns an independent dict with shared value pointers |
| `test_dict_merge` | `goc_dict_merge` merges dicts left-to-right, later entries win for duplicates |
| `test_dict_merge_many` | `goc_dict_merge` handles multiple dictionaries with duplicate overriding semantics |
| `test_dict_merge_empty` | `goc_dict_merge()` with zero arguments returns an empty dict |
| `test_dict_select` | `goc_dict_select` preserves requested key order in the resulting dict |
| `test_dict_get_in` | `goc_dict_get_in` supports deep nested dict/array lookup and failure cases |
| `test_dict_zip` | `goc_dict_zip` builds a dict from parallel goc_array key/value arrays |
| `test_dict_zip_c` | `goc_dict_zip_c` builds a dict from parallel C arrays of keys and values |
| `test_dict_boxed_macros` | boxed macros round-trip scalar values correctly through set/get/pop |
| `test_dict_tombstone_probe` | popped entries leave tombstones; reinserting through tombstone slots works |
| `test_dict_resize` | dict resizes under load and preserves all entries across rehashes |
