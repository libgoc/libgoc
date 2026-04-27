/* src/goc_dict.c
 *
 * Implements goc_dict — an ordered dictionary with open-addressing hash table
 * lookup and insertion-order iteration.
 *
 * Keys are preserved in insertion order via a sidecar goc_array. The hash
 * table uses tombstones to support removals without breaking probe sequences.
 *
 * All allocations are GC-managed through goc_new / goc_new_n.
 *
 * See docs/DICT.md for behavior and invariants.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "internal.h"
#include "../include/goc_dict.h"

/* ---------------------------------------------------------------------------
 * Internal constants
 * ---------------------------------------------------------------------------*/

#define GOC_DICT_TOMBSTONE ((char*)1)
#define GOC_DICT_MIN_CAP 8

/* ---------------------------------------------------------------------------
 * Internal struct definition
 * ---------------------------------------------------------------------------*/

struct goc_dict {
    void**     table;
    char**     table_keys;
    size_t     table_cap;
    size_t     occupied;
    goc_array* keys;
    size_t     len;
};

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------------*/

/**
 * _goc_dict_round_cap(requested_cap) — Round requested_cap up to a valid
 * power-of-two capacity.
 */
static size_t _goc_dict_round_cap(size_t requested_cap)
{
    size_t cap = GOC_DICT_MIN_CAP;
    while (cap < requested_cap) {
        cap *= 2;
    }
    return cap;
}

/**
 * _goc_dict_alloc_table(cap) — Allocate and zero a new hash table.
 */
static void** _goc_dict_alloc_table(size_t cap)
{
    void** table = (void**)goc_new_n(void*, cap);
    memset(table, 0, cap * sizeof(void*));
    return table;
}

/**
 * _goc_dict_alloc_keys(cap) — Allocate and zero a new parallel key table.
 */
static char** _goc_dict_alloc_keys(size_t cap)
{
    char** keys = (char**)goc_new_n(char*, cap);
    memset(keys, 0, cap * sizeof(char*));
    return keys;
}

/**
 * _goc_dict_hash(key, cap) — Compute an index for key using FNV-1a.
 */
static size_t _goc_dict_hash(const char* key, size_t cap)
{
    uint64_t hash = 1469598103934665603ULL;
    while (*key) {
        hash ^= (unsigned char)*key++;
        hash *= 1099511628211ULL;
    }
    return (size_t)(hash & (uint64_t)(cap - 1));
}

/**
 * _goc_dict_probe(d, key) — Probe for key, returning the slot index.
 *
 * Uses linear probing with tombstone reuse. If the key is absent, returns
 * the first available slot (either NULL or the first tombstone encountered).
 */
static size_t _goc_dict_probe(const goc_dict* d, const char* key)
{
    size_t cap = d->table_cap;
    size_t idx = _goc_dict_hash(key, cap);
    size_t first_tombstone = SIZE_MAX;

    for (;;) {
        char* current = d->table_keys[idx];
        if (current == NULL) {
            return (first_tombstone == SIZE_MAX) ? idx : first_tombstone;
        }
        if (current == GOC_DICT_TOMBSTONE) {
            if (first_tombstone == SIZE_MAX) {
                first_tombstone = idx;
            }
        } else if (strcmp(current, key) == 0) {
            return idx;
        }
        idx = (idx + 1) & (cap - 1);
    }
}

/**
 * _goc_dict_rehash(d, new_cap) — Resize the hash table and reinsert live
 * entries.
 */
static void _goc_dict_rehash(goc_dict* d, size_t new_cap)
{
    void** new_table = _goc_dict_alloc_table(new_cap);
    char** new_table_keys = _goc_dict_alloc_keys(new_cap);

    for (size_t i = 0; i < d->table_cap; i++) {
        char* key = d->table_keys[i];
        if (key == NULL || key == GOC_DICT_TOMBSTONE) {
            continue;
        }

        size_t dst = _goc_dict_hash(key, new_cap);
        while (new_table_keys[dst] != NULL) {
            dst = (dst + 1) & (new_cap - 1);
        }
        new_table_keys[dst] = key;
        new_table[dst] = d->table[i];
    }

    d->table = new_table;
    d->table_keys = new_table_keys;
    d->table_cap = new_cap;
    d->occupied = d->len;
}

/**
 * _goc_dict_make_internal(initial_cap) — Allocate a new dict with internal
 * buffers sized for initial_cap.
 */
static goc_dict* _goc_dict_make_internal(size_t initial_cap)
{
    goc_dict* d = (goc_dict*)goc_new(goc_dict);
    size_t cap = _goc_dict_round_cap(initial_cap);
    d->table = _goc_dict_alloc_table(cap);
    d->table_keys = _goc_dict_alloc_keys(cap);
    d->table_cap = cap;
    d->occupied = 0;
    d->keys = goc_array_make(initial_cap);
    d->len = 0;
    return d;
}

/**
 * _goc_dict_is_live_slot(d, slot) — Return true when the slot contains a live
 * key/value mapping.
 */
static bool _goc_dict_is_live_slot(const goc_dict* d, size_t slot)
{
    char* current = d->table_keys[slot];
    return current != NULL && current != GOC_DICT_TOMBSTONE;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------*/

goc_dict* goc_dict_make(size_t initial_cap)
{
    return _goc_dict_make_internal(initial_cap);
}

bool goc_dict_contains(const goc_dict* d, const char* key)
{
    size_t slot = _goc_dict_probe(d, key);
    char* current = d->table_keys[slot];
    return current != NULL && current != GOC_DICT_TOMBSTONE;
}

void* goc_dict_get(const goc_dict* d, const char* key, void* not_found)
{
    size_t slot = _goc_dict_probe(d, key);
    char* current = d->table_keys[slot];
    if (current == NULL || current == GOC_DICT_TOMBSTONE) {
        return not_found;
    }
    return d->table[slot];
}

size_t goc_dict_len(const goc_dict* d)
{
    return d->len;
}

void goc_dict_set(goc_dict* d, const char* key, void* val)
{
    if (d->occupied * 4 > d->table_cap * 3) {
        _goc_dict_rehash(d, d->table_cap * 2);
    }

    size_t slot = _goc_dict_probe(d, key);
    char* current = d->table_keys[slot];
    if (current != NULL && current != GOC_DICT_TOMBSTONE) {
        d->table[slot] = val;
        return;
    }

    d->table[slot] = val;
    d->table_keys[slot] = (char*)key;
    goc_array_push(d->keys, (void*)key);
    d->len++;
    if (current == NULL) {
        d->occupied++;
    }
}

void* goc_dict_pop(goc_dict* d, const char* key, void* not_found)
{
    size_t slot = _goc_dict_probe(d, key);
    char* current = d->table_keys[slot];
    if (current == NULL || current == GOC_DICT_TOMBSTONE) {
        return not_found;
    }

    void* val = d->table[slot];
    d->table[slot] = NULL;
    d->table_keys[slot] = GOC_DICT_TOMBSTONE;
    d->len--;

    size_t key_count = goc_array_len(d->keys);
    for (size_t i = 0; i < key_count; i++) {
        char* stored_key = (char*)goc_array_get(d->keys, i);
        if (stored_key == current) {
            goc_array_set(d->keys, i, NULL);
            break;
        }
    }

    return val;
}

/**
 * goc_dict_copy(d) — Return a shallow copy of d.
 *
 * The copy shares value pointers but has an independent dictionary structure.
 */
goc_dict* goc_dict_copy(const goc_dict* d)
{
    goc_dict* copy = _goc_dict_make_internal(d->table_cap);
    memcpy(copy->table, d->table, d->table_cap * sizeof(void*));
    memcpy(copy->table_keys, d->table_keys, d->table_cap * sizeof(char*));
    copy->keys = goc_array_copy(d->keys);
    copy->len = d->len;
    copy->occupied = d->occupied;
    return copy;
}

/**
 * goc_dict_select_c(d, keys, n) — Build a sub-dict containing only the
 * requested keys.
 */
goc_dict* goc_dict_select_c(const goc_dict* d, const char** keys, size_t n)
{
    goc_dict* result = _goc_dict_make_internal(d->table_cap);
    for (size_t i = 0; i < n; i++) {
        const char* key = keys[i];
        size_t slot = _goc_dict_probe(d, key);
        if (_goc_dict_is_live_slot(d, slot)) {
            goc_dict_set(result, key, d->table[slot]);
        }
    }
    return result;
}

/**
 * goc_dict_entries(d) — Convert a dict to a goc_array of goc_dict_entry_t*.
 */
goc_array* goc_dict_entries(const goc_dict* d)
{
    goc_array* entries = goc_array_make(goc_dict_len(d));
    size_t key_count = goc_array_len(d->keys);

    for (size_t i = 0; i < key_count; i++) {
        char* key = (char*)goc_array_get(d->keys, i);
        if (key == NULL) {
            continue;
        }

        size_t slot = _goc_dict_probe(d, key);
        if (!_goc_dict_is_live_slot(d, slot)) {
            continue;
        }

        goc_dict_entry_t* entry = (goc_dict_entry_t*)goc_new(goc_dict_entry_t);
        entry->key = key;
        entry->val = d->table[slot];
        goc_array_push(entries, entry);
    }

    return entries;
}

/**
 * goc_dict_keys(d) — Return a goc_array of live keys in insertion order.
 */
goc_array* goc_dict_keys(const goc_dict* d)
{
    goc_array* keys = goc_array_make(goc_dict_len(d));
    size_t key_count = goc_array_len(d->keys);

    for (size_t i = 0; i < key_count; i++) {
        char* key = (char*)goc_array_get(d->keys, i);
        if (key == NULL) {
            continue;
        }

        size_t slot = _goc_dict_probe(d, key);
        if (!_goc_dict_is_live_slot(d, slot)) {
            continue;
        }

        goc_array_push(keys, key);
    }

    return keys;
}

/**
 * goc_dict_vals(d) — Return a goc_array of live values
 * in insertion order of keys.
 */
goc_array* goc_dict_vals(const goc_dict* d)
{
    goc_array* vals = goc_array_make(goc_dict_len(d));
    size_t key_count = goc_array_len(d->keys);

    for (size_t i = 0; i < key_count; i++) {
        char* key = (char*)goc_array_get(d->keys, i);
        if (key == NULL) {
            continue;
        }

        size_t slot = _goc_dict_probe(d, key);
        if (!_goc_dict_is_live_slot(d, slot)) {
            continue;
        }

        goc_array_push(vals, d->table[slot]);
    }

    return vals;
}

/**
 * goc_dict_from_entries(entries) — Build a dict from an array of dict entries.
 */
goc_dict* goc_dict_from_entries(const goc_array* entries)
{
    goc_dict* result = _goc_dict_make_internal(goc_array_len(entries));
    size_t n = goc_array_len(entries);

    for (size_t i = 0; i < n; i++) {
        goc_dict_entry_t* entry = (goc_dict_entry_t*)goc_array_get(entries, i);
        goc_dict_set(result, entry->key, entry->val);
    }

    return result;
}

/**
 * _goc_dict_of_impl(kvs, n) — Build a dict from an array of key/value pairs.
 */
goc_dict* _goc_dict_of_impl(const goc_dict_entry_t* kvs, size_t n)
{
    goc_dict* d = _goc_dict_make_internal(n);
    for (size_t i = 0; i < n; i++) {
        goc_dict_set(d, kvs[i].key, kvs[i].val);
    }
    return d;
}

/**
 * _goc_dict_of_boxed_impl(elem_size, pair_size, val_offset, pairs, n) —
 * Build a dict from inline boxed key/value pairs of scalar type T.
 */
goc_dict* _goc_dict_of_boxed_impl(size_t elem_size,
                                   size_t pair_size,
                                   size_t val_offset,
                                   const void* pairs,
                                   size_t n)
{
    goc_dict* d = _goc_dict_make_internal(n);
    const char* base = (const char*)pairs;

    for (size_t i = 0; i < n; i++) {
        const char* key = *(const char**)(base + i * pair_size);
        const void* value_ptr = base + i * pair_size + val_offset;
        void* boxed = _goc_box_impl(value_ptr, elem_size);
        goc_dict_set(d, key, boxed);
    }

    return d;
}

/**
 * _goc_dict_merge_many(dicts, n) — Merge n dictionaries left-to-right.
 */
goc_dict* _goc_dict_merge_many(goc_dict** dicts, size_t n)
{
    if (n == 0) {
        return _goc_dict_make_internal(0);
    }

    goc_dict* merged = goc_dict_copy(dicts[0]);
    for (size_t i = 1; i < n; i++) {
        goc_dict* source = dicts[i];
        size_t count = goc_array_len(source->keys);
        for (size_t j = 0; j < count; j++) {
            char* key = (char*)goc_array_get(source->keys, j);
            if (key == NULL) {
                continue;
            }
            size_t slot = _goc_dict_probe(source, key);
            if (!_goc_dict_is_live_slot(source, slot)) {
                continue;
            }
            goc_dict_set(merged, key, source->table[slot]);
        }
    }
    return merged;
}

/**
 * goc_dict_zip_c(keys, vals, n) — Build a dict from paired C arrays.
 */
goc_dict* goc_dict_zip_c(const char** keys, void** vals, size_t n)
{
    goc_dict* d = _goc_dict_make_internal(n);
    for (size_t i = 0; i < n; i++) {
        goc_dict_set(d, keys[i], vals[i]);
    }
    return d;
}

/**
 * _goc_dict_zip_impl(keys, vals) — Build a dict from two goc_array objects.
 */
goc_dict* _goc_dict_zip_impl(const goc_array* keys, const goc_array* vals)
{
    size_t n_keys = goc_array_len(keys);
    size_t n_vals = goc_array_len(vals);
    if (n_keys != n_vals) {
        ABORT("goc_dict_zip: key/value length mismatch (%zu != %zu)\n", n_keys, n_vals);
    }

    goc_dict* d = _goc_dict_make_internal(n_keys);
    for (size_t i = 0; i < n_keys; i++) {
        const char* key = (const char*)goc_array_get(keys, i);
        void* val = goc_array_get(vals, i);
        goc_dict_set(d, key, val);
    }
    return d;
}
