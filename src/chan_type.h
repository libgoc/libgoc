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
    goc_entry*   takers_tail;  /* last node in takers list; NULL when empty */
    goc_entry*   putters;
    goc_entry*   putters_tail; /* last node in putters list; NULL when empty */
    uv_mutex_t*  lock;         /* plain malloc; not GC-heap (libuv constraint) */
    int          closed;
    size_t       dead_count;
    _Atomic int  close_guard;  /* CAS 0→1 to serialise close; prevents double-close races */
    void       (*on_close)(void*);
    void*        on_close_ud;
};

#endif /* GOC_CHAN_TYPE_H */
