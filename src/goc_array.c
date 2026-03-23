/* src/goc_array.c
 *
 * Implements goc_array — a memory-managed, mutable dynamic array.
 *
 * Design summary
 * --------------
 * The array is backed by a contiguous GC-managed buffer of void* slots.
 * Three fields track the live region:
 *
 *   data[head .. head+len-1]  — live elements
 *   data[0 .. head-1]         — head headroom (available for push_head)
 *   data[head+len .. cap-1]   — tail headroom (available for push_tail)
 *
 * On each grow the capacity is doubled and the existing elements are
 * re-centred in the new buffer, leaving roughly equal headroom at both
 * ends.  This gives amortized O(1) cost for push and push_head.
 *
 * Slicing is O(1): a new goc_array header is allocated and pointed at the
 * same backing buffer (data pointer, adjusted head, updated len/cap).
 *
 * goc_array_to_c() is O(1): it returns a direct pointer into the backing
 * buffer at &data[head].  The pointer is valid until the next reallocation.
 *
 * All allocations use goc_malloc / goc_realloc so the GC owns every byte.
 * No manual free() is ever required.
 *
 * See ARRAY.md for the full design rationale.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/goc.h"
#include "../include/goc_array.h"

/* ---------------------------------------------------------------------------
 * Internal constants
 * ---------------------------------------------------------------------------*/

/* Minimum backing-buffer capacity (in void* slots). */
#define GOC_ARRAY_INIT_CAP 8

/* ---------------------------------------------------------------------------
 * Internal struct definition
 * ---------------------------------------------------------------------------*/

struct goc_array {
    void** data;  /* GC-managed backing buffer of void* slots              */
    size_t head;  /* index of the first live element in data               */
    size_t len;   /* number of live elements                               */
    size_t cap;   /* total allocated capacity (number of void* slots)      */
};

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------------*/

/*
 * array_grow — ensure arr has room for at least one more element.
 *
 * grow_for_head: non-zero when growing to satisfy a push_head call.
 *   The existing elements are placed closer to the tail of the new
 *   buffer so that more head headroom is available.
 *
 * Doubles capacity.  Existing elements are centred in the new buffer
 * (biased slightly toward the tail when grow_for_head, toward the head
 * otherwise), giving amortized O(1) for alternating push / push_head
 * sequences.
 */
static void array_grow(goc_array* arr, int grow_for_head)
{
    size_t new_cap = (arr->cap < GOC_ARRAY_INIT_CAP)
                     ? GOC_ARRAY_INIT_CAP
                     : arr->cap * 2;

    void** new_data = (void**)goc_malloc(new_cap * sizeof(void*));

    /* Place existing elements so the "useful" end has more headroom. */
    size_t slack = new_cap - arr->len;
    size_t new_head;
    if (grow_for_head) {
        /* Bias toward tail: leave 3/4 slack at head, 1/4 at tail. */
        new_head = (slack * 3) / 4;
    } else {
        /* Bias toward head: leave 1/4 slack at head, 3/4 at tail. */
        new_head = slack / 4;
    }

    if (arr->len > 0) {
        memcpy(new_data + new_head,
               arr->data + arr->head,
               arr->len * sizeof(void*));
    }

    arr->data = new_data;
    arr->head = new_head;
    arr->cap  = new_cap;
    /* arr->len is unchanged */
}

/* ---------------------------------------------------------------------------
 * Construction
 * ---------------------------------------------------------------------------*/

goc_array* goc_array_make(size_t initial_cap)
{
    goc_array* arr = (goc_array*)goc_malloc(sizeof(goc_array));

    size_t cap = (initial_cap < GOC_ARRAY_INIT_CAP)
                 ? GOC_ARRAY_INIT_CAP
                 : initial_cap;

    arr->data = (void**)goc_malloc(cap * sizeof(void*));
    arr->head = cap / 4;  /* start with 1/4 cap head headroom */
    arr->len  = 0;
    arr->cap  = cap;

    return arr;
}

goc_array* goc_array_from(void** items, size_t n)
{
    goc_array* arr = goc_array_make(n);

    if (n > 0) {
        memcpy(arr->data + arr->head, items, n * sizeof(void*));
        arr->len = n;
    }

    return arr;
}

/* ---------------------------------------------------------------------------
 * Length
 * ---------------------------------------------------------------------------*/

size_t goc_array_len(const goc_array* arr)
{
    return arr->len;
}

/* ---------------------------------------------------------------------------
 * Random access
 * ---------------------------------------------------------------------------*/

void* goc_array_get(const goc_array* arr, size_t i)
{
    if (i >= arr->len) {
        fprintf(stderr,
                "libgoc: goc_array_get: index %zu out of bounds (len=%zu)\n",
                i, arr->len);
        abort();
    }
    return arr->data[arr->head + i];
}

void goc_array_set(goc_array* arr, size_t i, void* val)
{
    if (i >= arr->len) {
        fprintf(stderr,
                "libgoc: goc_array_set: index %zu out of bounds (len=%zu)\n",
                i, arr->len);
        abort();
    }
    arr->data[arr->head + i] = val;
}

/* ---------------------------------------------------------------------------
 * Tail push / pop
 * ---------------------------------------------------------------------------*/

void goc_array_push(goc_array* arr, void* val)
{
    if (arr->head + arr->len >= arr->cap) {
        array_grow(arr, 0);
    }
    arr->data[arr->head + arr->len] = val;
    arr->len++;
}

void* goc_array_pop(goc_array* arr)
{
    if (arr->len == 0) {
        fprintf(stderr, "libgoc: goc_array_pop: array is empty\n");
        abort();
    }
    arr->len--;
    return arr->data[arr->head + arr->len];
}

/* ---------------------------------------------------------------------------
 * Head push / pop
 * ---------------------------------------------------------------------------*/

void goc_array_push_head(goc_array* arr, void* val)
{
    if (arr->head == 0) {
        array_grow(arr, 1);
    }
    arr->head--;
    arr->data[arr->head] = val;
    arr->len++;
}

void* goc_array_pop_head(goc_array* arr)
{
    if (arr->len == 0) {
        fprintf(stderr, "libgoc: goc_array_pop_head: array is empty\n");
        abort();
    }
    void* val = arr->data[arr->head];
    arr->head++;
    arr->len--;
    return val;
}

/* ---------------------------------------------------------------------------
 * Concat
 * ---------------------------------------------------------------------------*/

goc_array* goc_array_concat(const goc_array* a, const goc_array* b)
{
    size_t total = a->len + b->len;
    goc_array* out = goc_array_make(total);

    if (a->len > 0) {
        memcpy(out->data + out->head,
               a->data + a->head,
               a->len * sizeof(void*));
    }
    if (b->len > 0) {
        memcpy(out->data + out->head + a->len,
               b->data + b->head,
               b->len * sizeof(void*));
    }
    out->len = total;

    return out;
}

/* ---------------------------------------------------------------------------
 * Slicing
 * ---------------------------------------------------------------------------*/

goc_array* goc_array_slice(const goc_array* arr, size_t start, size_t end)
{
    if (start > end || end > arr->len) {
        fprintf(stderr,
                "libgoc: goc_array_slice: invalid range [%zu, %zu) "
                "(len=%zu)\n",
                start, end, arr->len);
        abort();
    }

    goc_array* s = (goc_array*)goc_malloc(sizeof(goc_array));
    s->data = arr->data;             /* shared backing buffer */
    s->head = arr->head + start;
    s->len  = end - start;
    s->cap  = arr->cap;              /* full capacity (Go-style slice) */

    return s;
}

/* ---------------------------------------------------------------------------
 * C-array interop
 * ---------------------------------------------------------------------------*/

void** goc_array_to_c(const goc_array* arr)
{
    if (arr->len == 0) {
        return NULL;
    }
    return arr->data + arr->head;
}
