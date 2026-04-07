/*
 * goc_array.h — Public API for libgoc dynamic array
 *
 * A memory-managed, mutable dynamic array backed by the Boehm GC heap.
 * Stores elements as `void*` pointers (type-erased, like channels).
 *
 * Consumers: #include "goc_array.h"  (or <goc_array.h> when installed)
 *
 * Compile requirements: -std=c11
 * Required defines: -DGC_THREADS  -D_GNU_SOURCE
 *
 * Thread safety: goc_array instances are NOT thread-safe. Use channels or
 * mutexes to coordinate access from multiple fibers / OS threads.
 *
 * Memory: all allocations are on the Boehm GC heap (via goc_malloc /
 * goc_realloc). No manual free() is required or permitted. Arrays and their
 * backing buffers are collected automatically when unreachable.
 *
 * See docs/ARRAY.md for design rationale and full API documentation.
 */

#ifndef GOC_ARRAY_H
#define GOC_ARRAY_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Opaque type
 * ---------------------------------------------------------------------- */

typedef struct goc_array goc_array;

/* -------------------------------------------------------------------------
 * Construction
 * ---------------------------------------------------------------------- */

/**
 * goc_array_make() — Allocate and return a new empty dynamic array.
 *
 * initial_cap : hint for the initial backing-buffer capacity (number of
 *               void* slots to pre-allocate). Pass 0 to use the default
 *               (GOC_ARRAY_INIT_CAP). The array starts with length 0
 *               regardless of initial_cap.
 *
 * Returns a GC-managed pointer. Never returns NULL (aborts on OOM).
 */
goc_array* goc_array_make(size_t initial_cap);

/**
 * goc_array_from() — Create a dynamic array from an existing C array.
 *
 * items : pointer to an array of n void* values to copy in.
 * n     : number of elements (0 is allowed; produces an empty array).
 *
 * The items are copied into a fresh GC-managed backing buffer. The original
 * C array is not referenced after this call returns.
 *
 * Returns a GC-managed pointer. Never returns NULL.
 */
goc_array* goc_array_from(void** items, size_t n);

/* -------------------------------------------------------------------------
 * Length
 * ---------------------------------------------------------------------- */

/**
 * goc_array_len() — Return the number of live elements in arr.
 */
size_t goc_array_len(const goc_array* arr);

/* -------------------------------------------------------------------------
 * Random access — O(1)
 * ---------------------------------------------------------------------- */

/**
 * goc_array_get() — Return the element at index i.
 *
 * i must be < goc_array_len(arr). Aborts on out-of-bounds access.
 */
void* goc_array_get(const goc_array* arr, size_t i);

/**
 * goc_array_set() — Replace the element at index i with val.
 *
 * i must be < goc_array_len(arr). Aborts on out-of-bounds access.
 */
void goc_array_set(goc_array* arr, size_t i, void* val);

/* -------------------------------------------------------------------------
 * Tail push / pop — amortized O(1)
 * ---------------------------------------------------------------------- */

/**
 * goc_array_push() — Append val to the tail of arr.
 *
 * Amortized O(1). May reallocate the backing buffer; after reallocation the
 * previous backing buffer becomes unreachable and will be GC-collected.
 */
void goc_array_push(goc_array* arr, void* val);

/**
 * goc_array_pop() — Remove and return the tail element of arr.
 *
 * O(1). arr must not be empty; aborts if goc_array_len(arr) == 0.
 */
void* goc_array_pop(goc_array* arr);

/* -------------------------------------------------------------------------
 * Head push / pop — amortized O(1)
 * ---------------------------------------------------------------------- */

/**
 * goc_array_push_head() — Prepend val to the head of arr.
 *
 * Amortized O(1). May reallocate the backing buffer.
 */
void goc_array_push_head(goc_array* arr, void* val);

/**
 * goc_array_pop_head() — Remove and return the head element of arr.
 *
 * O(1). arr must not be empty; aborts if goc_array_len(arr) == 0.
 */
void* goc_array_pop_head(goc_array* arr);

/* -------------------------------------------------------------------------
 * Concat — O(n)
 * ---------------------------------------------------------------------- */

/**
 * goc_array_concat() — Return a new array containing all elements of a
 * followed by all elements of b.
 *
 * O(n) where n = len(a) + len(b). Allocates a fresh backing buffer; neither
 * a nor b is modified.
 */
goc_array* goc_array_concat(const goc_array* a, const goc_array* b);

/* -------------------------------------------------------------------------
 * Slicing — O(1) shallow copy
 * ---------------------------------------------------------------------- */

/**
 * goc_array_slice() — Return a shallow-copy subarray covering [start, end).
 *
 * start : index of the first element to include.
 * end   : one past the last element to include (exclusive upper bound).
 *
 * Both start and end must satisfy start <= end <= goc_array_len(arr).
 * Aborts on invalid bounds.
 *
 * O(1): the slice shares arr's backing buffer — no element copying is
 * performed. The slice is a fully independent goc_array value: push / pop
 * operations on either the slice or the original may reallocate their own
 * backing buffers independently once the shared region is exhausted.
 *
 * As long as either the slice or the original is reachable, the GC will
 * retain the shared backing buffer.
 */
goc_array* goc_array_slice(const goc_array* arr, size_t start, size_t end);

/* -------------------------------------------------------------------------
 * C-array interop
 * ---------------------------------------------------------------------- */

/**
 * goc_array_to_c() — Return a pointer to the first element of arr as a
 * contiguous C array of void* values.
 *
 * O(1). The returned pointer is valid until the next structural modification
 * of arr (push, pop, push_head, pop_head) that triggers a reallocation.
 * The pointer points directly into arr's GC-managed backing buffer; callers
 * must keep arr reachable to prevent the buffer from being collected.
 *
 * Returns NULL when goc_array_len(arr) == 0.
 */
void** goc_array_to_c(const goc_array* arr);

/* -------------------------------------------------------------------------
 * String interop
 * ---------------------------------------------------------------------- */

/**
 * goc_array_from_str() — Create a byte array from a null-terminated C string.
 *
 * Each byte of s is stored as goc_box_int(byte). The null terminator is not
 * included. Equivalent to iterating over s and calling goc_array_push for each
 * byte.
 *
 * Returns a GC-managed pointer. Never returns NULL.
 * Passing NULL for s produces an empty array.
 */
goc_array* goc_array_from_str(const char* s);

/**
 * goc_array_to_str() — Return a GC-heap null-terminated string from a byte array.
 *
 * Each element is interpreted as goc_box_int(byte). The result is a fresh
 * GC-heap allocation of len+1 bytes with a null terminator appended.
 *
 * Returns a GC-managed pointer. Never returns NULL.
 * An empty array produces an empty string ("").
 */
char* goc_array_to_str(const goc_array* arr);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GOC_ARRAY_H */
