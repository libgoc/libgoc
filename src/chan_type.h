#ifndef GOC_CHAN_TYPE_H
#define GOC_CHAN_TYPE_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <uv.h>

typedef struct goc_entry goc_entry;   /* forward declaration; full definition in internal.h */

typedef enum {
    GOC_CHAN_OPEN = 0,
    GOC_CHAN_CLOSE_REQUESTED = 1,
    GOC_CHAN_CLOSING = 2,
} goc_chan_close_state_t;

struct goc_chan {
    void**       buf;
    size_t       buf_size;
    size_t       buf_head;
    size_t       buf_count;
    size_t       item_count; /* number of items in buffer */
    goc_entry*   takers;
    goc_entry*   takers_tail;  /* last node in takers list; NULL when empty */
    goc_entry*   putters;
    goc_entry*   putters_tail; /* last node in putters list; NULL when empty */
    uv_mutex_t*  lock;         /* plain malloc; not GC-heap (libuv constraint) */
    int          closed;
    size_t       dead_count;
    _Atomic int  close_guard;  /* one of goc_chan_close_state_t */
    const char*  dbg_tag;      /* optional debug tag for log correlation */
    /* Telemetry counters — incremented with memory_order_relaxed; read at close time */
    _Atomic uint64_t taker_scans;     /* times a take arm scanned this channel in alts */
    _Atomic uint64_t putter_scans;    /* times a put arm scanned this channel in alts */
    _Atomic uint64_t compaction_runs; /* times compact_dead_entries ran on this channel */
    _Atomic uint64_t entries_removed; /* total dead entries removed across all compactions */
    void       (*on_close)(void*);
    void*        on_close_ud;
};

#endif /* GOC_CHAN_TYPE_H */
