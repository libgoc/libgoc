#ifndef GOC_CHAN_TYPE_H
#define GOC_CHAN_TYPE_H

#include <stddef.h>
#include <stdatomic.h>
#include <uv.h>

typedef struct goc_entry goc_entry;   /* forward declaration; full definition in internal.h */

struct goc_chan {
    void**       buf;
    size_t       buf_size;
    size_t       buf_head;
    size_t       buf_count;
    goc_entry*   takers;
    goc_entry*   putters;
    uv_mutex_t*  lock;         /* plain malloc; not GC-heap (libuv constraint) */
    int          closed;
    size_t       dead_count;
    _Atomic int  close_guard;  /* CAS 0→1 to serialise close; prevents double-close races */
};

#endif /* GOC_CHAN_TYPE_H */
