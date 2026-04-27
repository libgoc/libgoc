# `goc_array` — Dynamic Array

A memory-managed, mutable dynamic array for **libgoc**.

Elements are stored as `void*` pointers (type-erased, consistent with channels and other libgoc APIs).  All memory is managed by the Boehm GC — no `free()` is required or permitted.

---

## Table of Contents

1. [Quick Example](#quick-example)
2. [Design](#design)
3. [API Reference](#api-reference)
   - [Construction](#construction)
   - [Inline Construction Helpers](#inline-construction-helpers)
   - [Independent Copy](#independent-copy)
   - [C-Array Interop](#c-array-interop)
   - [Length](#length)
   - [Random Access](#random-access)
   - [Tail Push / Pop](#tail-push--pop)
   - [Head Push / Pop](#head-push--pop)
   - [Concat](#concat)
   - [Slicing](#slicing)
   - [String Interop](#string-interop)
4. [Complexity Summary](#complexity-summary)
5. [Thread Safety](#thread-safety)
6. [Examples](#examples)

---

## Quick Example

### Building and traversing an array

```c
#include "goc_array.h"

goc_array* arr = goc_array_of_boxed(int, 10, 20, 30);
goc_array_push_boxed(int, arr, 40);   // {10, 20, 30, 40}
goc_array_pop_head_unboxed(int, arr); // 10
goc_array_get_unboxed(int, arr, 0);   // 20
goc_array_slice(arr, 1, 3);           // {30, 40}
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

Slice semantics follow Go's: the slice's `cap` equals the original's `cap`, so `push_tail` on the slice can write into slots that the original would otherwise use.  Callers that want an independent copy should use `goc_array_copy`.

### C-array interop

`goc_array_to_c(arr)` returns `&arr->data[arr->head]` — a direct pointer into the live region of the backing buffer.  This is O(1) and zero-copy.  The pointer is valid until the next structural modification of `arr` that triggers a reallocation.

---

## API Reference

### Construction

```c
#include "goc_array.h"

goc_array* goc_array_make(size_t initial_cap);
```

| Function | Description |
|---|---|
| `goc_array_make(initial_cap)` | Allocate an empty array with a pre-allocated backing buffer of at least `initial_cap` slots (0 → default of 8). Length starts at 0. |
| `goc_array_from(items, n)` | Create an array by using the GC-managed C array `items` directly as the backing buffer. No copy is performed. Pass `NULL, 0` for an empty array. `cap == n`; the first push may trigger a grow. Use `goc_array_copy()` for an independent copy and prefer `goc_array_of()` for inline construction. |
| `goc_array_of(...)` | Create an array from inline `void*` arguments. Useful for building inline pointer arrays without manually allocating a C array. O(n). |
| `goc_array_of_boxed(T, ...)` | Create an array from scalar values of type `T`, automatically boxing each value with `goc_box(T, value)`. O(n). |
| `goc_array_copy(arr)` | Create a shallow copy of `arr` with an independent backing buffer. The elements are shared, but mutations to one array do not affect the other. O(n). |

### Inline Construction Helpers

```c
#define goc_array_of(...)
#define goc_array_of_boxed(T, ...)
```

`goc_array_of(...)` is the preferred way to construct an array from inline `void*` arguments. For scalar values, prefer `goc_array_of_boxed(T, ...)`, which boxes each value automatically. Both derive the element count from the argument list — no size argument required.

### Independent Copy

```c
goc_array* goc_array_copy(const goc_array* arr);
```

`goc_array_copy(arr)` returns a shallow copy of `arr` with its own backing buffer. The new array contains the same elements as `arr`, but pushes and pops on one array do not change the other.

```c
goc_array* xs = goc_array_of_boxed(int, 1, 2, 3);
goc_array* ys = goc_array_copy(xs);
```

### C-Array Interop

```c
void**     goc_array_to_c(const goc_array* arr);
goc_array* goc_array_from(void** items, size_t n);
```

`goc_array_to_c` returns a pointer to the first live element (a contiguous `void*[]`), or `NULL` when the array is empty.  The pointer is valid until the next push/pop that reallocates the backing buffer.  **O(1)**.


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

```c
#define goc_array_get_unboxed(T, arr, i)
#define goc_array_set_boxed(T, arr, i, val)
```

`goc_array_get_unboxed(T, arr, i)` retrieves the element at index `i` and unboxes it in one step. `goc_array_set_boxed(T, arr, i, val)` stores a scalar value boxed as `void*`. Both preserve the same O(1) complexity.

### Tail Push / Pop

```c
void   goc_array_push(goc_array* arr, void* val);
void*  goc_array_pop(goc_array* arr);
```

`goc_array_pop` aborts if the array is empty.  Both are **amortized O(1)**.

```c
#define goc_array_push_boxed(T, arr, val)
#define goc_array_pop_unboxed(T, arr)
```

`goc_array_push_boxed(T, arr, val)` boxes `val` and appends it to the tail. `goc_array_pop_unboxed(T, arr)` removes the tail element and returns it unboxed.

### Head Push / Pop

```c
void   goc_array_push_head(goc_array* arr, void* val);
void*  goc_array_pop_head(goc_array* arr);
```

`goc_array_pop_head` aborts if the array is empty.  Both are **amortized O(1)**.

```c
#define goc_array_push_head_boxed(T, arr, val)
#define goc_array_pop_head_unboxed(T, arr)
```

`goc_array_push_head_boxed(T, arr, val)` boxes `val` and prepends it to the head. `goc_array_pop_head_unboxed(T, arr)` removes the head element and returns it unboxed.

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
| `goc_array_from_str(s)` | Create a byte array from a null-terminated C string. Each byte is stored as `goc_box(char, byte)`; the null terminator is not included. `NULL` input returns an empty array. **O(n)**. |
| `goc_array_to_str(arr)` | Convert a byte array back to a GC-heap null-terminated C string. Each element is unboxed as a byte. An empty array returns `""`. **O(n)**. |

```c
goc_array* arr = goc_array_from_str("hello");  /* len == 5 */
char*      s   = goc_array_to_str(arr);         /* "hello"  */
```

> This is raw byte interop; no Unicode-specific semantics are implied.

---

## Complexity Summary

| Operation | Complexity |
|---|---|
| `goc_array_get` / `goc_array_set` | O(1) |
| `goc_array_push_boxed` | O(1) amortized |
| `goc_array_push` | O(1) amortized |
| `goc_array_pop_unboxed` | O(1) |
| `goc_array_pop` | O(1) |
| `goc_array_push_head_boxed` | O(1) amortized |
| `goc_array_push_head` | O(1) amortized |
| `goc_array_pop_head_unboxed` | O(1) |
| `goc_array_pop_head` | O(1) |
| `goc_array_set_boxed` | O(1) |
| `goc_array_get_unboxed` | O(1) |
| `goc_array_concat` | O(n) |
| `goc_array_slice` | O(1) |
| `goc_array_from` | O(1) |
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

### Scalar array round-trip

```c
goc_array* arr = goc_array_of_boxed(int, 1, 2, 3);
goc_array_push_boxed(int, arr, 4);         /* append */
goc_array_push_head_boxed(int, arr, 0);    /* prepend */
goc_array_set_boxed(int, arr, 2, 99);      /* overwrite index 2 */

int head = goc_array_pop_head_unboxed(int, arr);  /* == 0 */
int tail = goc_array_pop_unboxed(int, arr);       /* == 4 */
int mid  = goc_array_get_unboxed(int, arr, 1);    /* == 99 */
```

### Slicing

```c
goc_array* arr = goc_array_of_boxed(int, 1, 2, 3, 4, 5);

goc_array* middle = goc_array_slice(arr, 1, 4);
/* middle contains [2, 3, 4]; shares arr's backing buffer */
```

### C-array interop

```c
goc_array* arr = goc_array_of_boxed(int, 1, 2);

void** c = goc_array_to_c(arr);
/* goc_unbox(int, c[0]) == 1, goc_unbox(int, c[1]) == 2 */

/* Going the other way: */
void* items[] = { goc_box(int, 10), goc_box(int, 20), goc_box(int, 30) };
goc_array* from_c = goc_array_from(items, 3);
/* goc_array_get_unboxed(int, from_c, 0) == 10 */
/* uses items directly as the backing buffer — no copy performed */
```
