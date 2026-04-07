# `goc_array` — Dynamic Array

A memory-managed, mutable dynamic array for **libgoc**.

Elements are stored as `void*` pointers (type-erased, consistent with channels and other libgoc APIs).  All memory is managed by the Boehm GC — no `free()` is required or permitted.

---

## Table of Contents

1. [Quick Example](#quick-example)
2. [Design](#design)
3. [API Reference](#api-reference)
   - [Construction](#construction)
   - [Length](#length)
   - [Random Access](#random-access)
   - [Tail Push / Pop](#tail-push--pop)
   - [Head Push / Pop](#head-push--pop)
   - [Concat](#concat)
   - [Slicing](#slicing)
   - [String Interop](#string-interop)
   - [C-Array Interop](#c-array-interop)
4. [Complexity Summary](#complexity-summary)
5. [Thread Safety](#thread-safety)
6. [Examples](#examples)

---

## Quick Example

### Building and traversing an array

```c
#include "goc_array.h"
#include <stdio.h>

goc_array* arr = goc_array_make(0);

goc_array_push(arr, goc_box_int(10));
goc_array_push(arr, goc_box_int(20));
goc_array_push(arr, goc_box_int(30));

for (size_t i = 0; i < goc_array_len(arr); i++) {
    printf("%d\n", goc_unbox_int(goc_array_get(arr, i)));
}
// 10
// 20
// 30
```

---

## Design

### Backing buffer

A `goc_array` is backed by a single contiguous GC-managed buffer of `void*` slots.  Three fields track the live region:

```
data[0 .. head-1]          — head headroom (for push_head)
data[head .. head+len-1]   — live elements
data[head+len .. cap-1]    — tail headroom (for push_tail)
```

Random access is O(1): `get(i)` returns `data[head + i]`.

### Growth strategy

When either end runs out of headroom the backing buffer is doubled and the existing elements are re-centred:

- Grow triggered by `push_tail` (no tail room): new elements are placed at `new_cap / 4`, leaving ~¾ of the new capacity available for tail growth.
- Grow triggered by `push_head` (no head room): new elements are placed at `3 * new_cap / 4 - len`, leaving ~¾ of the new capacity available for head growth.

This gives **amortized O(1)** cost for any sequence of `push` and `push_head` calls — including worst-case alternating patterns — because the total number of copy operations across all growths is proportional to the total number of insertions.

The old backing buffer is unreachable after a grow and will be collected by the GC on the next cycle.

### Slicing

`goc_array_slice(arr, start, end)` allocates a new `goc_array` header that points at the **same** backing buffer with adjusted `head` and `len`.  No element copying is performed (O(1)).

The GC retains the backing buffer as long as at least one `goc_array` (the original or any slice) is reachable.

Slice semantics follow Go's: the slice's `cap` equals the original's `cap`, so `push_tail` on the slice can write into slots that the original would otherwise use.  Callers that want an independent copy should use `goc_array_concat`.

### C-array interop

`goc_array_to_c(arr)` returns `&arr->data[arr->head]` — a direct pointer into the live region of the backing buffer.  This is O(1) and zero-copy.  The pointer is valid until the next structural modification of `arr` that triggers a reallocation.

---

## API Reference

### Construction

```c
#include "goc_array.h"

goc_array* goc_array_make(size_t initial_cap);
goc_array* goc_array_from(void** items, size_t n);
```

| Function | Description |
|---|---|
| `goc_array_make(initial_cap)` | Allocate an empty array with a pre-allocated backing buffer of at least `initial_cap` slots (0 → default of 8). Length starts at 0. |
| `goc_array_from(items, n)` | Create an array by copying `n` elements from the C array `items`. Pass `NULL, 0` for an empty array. O(n). |

### Length

```c
size_t goc_array_len(const goc_array* arr);
```

Returns the number of live elements.  O(1).

### Random Access

```c
void*  goc_array_get(const goc_array* arr, size_t i);
void   goc_array_set(goc_array* arr, size_t i, void* val);
```

`i` must be `< goc_array_len(arr)`; both functions abort on out-of-bounds access.  O(1).

### Tail Push / Pop

```c
void   goc_array_push(goc_array* arr, void* val);
void*  goc_array_pop(goc_array* arr);
```

`goc_array_pop` aborts if the array is empty.  Both are **amortized O(1)**.

### Head Push / Pop

```c
void   goc_array_push_head(goc_array* arr, void* val);
void*  goc_array_pop_head(goc_array* arr);
```

`goc_array_pop_head` aborts if the array is empty.  Both are **amortized O(1)**.

### Concat

```c
goc_array* goc_array_concat(const goc_array* a, const goc_array* b);
```

Returns a new array containing all elements of `a` followed by all elements of `b`.  Neither input is modified.  **O(n)** where `n = len(a) + len(b)`.

### Slicing

```c
goc_array* goc_array_slice(const goc_array* arr, size_t start, size_t end);
```

Returns a shallow-copy subarray covering `[start, end)`.  Both `start` and `end` must satisfy `start <= end <= goc_array_len(arr)`; aborts on invalid bounds.  **O(1)**.

### String Interop

```c
goc_array* goc_array_from_str(const char* s);
char*      goc_array_to_str(const goc_array* arr);
```

| Function | Description |
|---|---|
| `goc_array_from_str(s)` | Create a byte array from a null-terminated C string. Each byte is stored as `goc_box_int(byte)`; the null terminator is not included. `NULL` input returns an empty array. **O(n)**. |
| `goc_array_to_str(arr)` | Convert a byte array back to a GC-heap null-terminated C string. Each element is unboxed as a byte. An empty array returns `""`. **O(n)**. |

```c
goc_array* arr = goc_array_from_str("hello");  /* len == 5 */
char*      s   = goc_array_to_str(arr);         /* "hello"  */
```

> This is raw byte interop; no Unicode-specific semantics are implied.

### C-Array Interop

```c
void**     goc_array_to_c(const goc_array* arr);
goc_array* goc_array_from(void** items, size_t n);   /* see Construction */
```

`goc_array_to_c` returns a pointer to the first live element (a contiguous `void*[]`), or `NULL` when the array is empty.  The pointer is valid until the next push/pop that reallocates the backing buffer.  **O(1)**.

---

## Complexity Summary

| Operation | Complexity |
|---|---|
| `goc_array_get` / `goc_array_set` | O(1) |
| `goc_array_push` | O(1) amortized |
| `goc_array_pop` | O(1) |
| `goc_array_push_head` | O(1) amortized |
| `goc_array_pop_head` | O(1) |
| `goc_array_concat` | O(n) |
| `goc_array_slice` | O(1) |
| `goc_array_from` | O(n) |
| `goc_array_to_c` | O(1) |
| `goc_array_from_str` | O(n) |
| `goc_array_to_str` | O(n) |

---

## Thread Safety

`goc_array` is **not** thread-safe.  Concurrent reads and writes require external synchronisation.  Use `goc_mutex` or channels to coordinate access from multiple fibers or OS threads.

---

## Examples

### Using as a queue (FIFO)

```c
goc_array* queue = goc_array_make(0);

goc_array_push(queue, item_a);   /* enqueue */
goc_array_push(queue, item_b);

void* first = goc_array_pop_head(queue);  /* dequeue */
```

### Using as a stack (LIFO)

```c
goc_array* stack = goc_array_make(0);

goc_array_push(stack, item_a);   /* push */
goc_array_push(stack, item_b);

void* top = goc_array_pop(stack);  /* pop */
```

### Slicing

```c
goc_array* arr = goc_array_from((void*[]){
    goc_box_int(1), goc_box_int(2), goc_box_int(3),
    goc_box_int(4), goc_box_int(5)
}, 5);

goc_array* middle = goc_array_slice(arr, 1, 4);
/* middle contains [2, 3, 4]; shares arr's backing buffer */
```

### C-array interop

```c
goc_array* arr = goc_array_make(0);
goc_array_push(arr, goc_box_int(1));
goc_array_push(arr, goc_box_int(2));

void** c = goc_array_to_c(arr);
/* goc_unbox_int(c[0]) == 1, goc_unbox_int(c[1]) == 2 */

/* Going the other way: */
void* items[] = { goc_box_int(10), goc_box_int(20), goc_box_int(30) };
goc_array* from_c = goc_array_from(items, 3);
```
