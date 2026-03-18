/*
 * src/channel_internal.h
 *
 * Channel-only internal declarations and inline helpers.
 * Included by channel-facing modules (channel.c, alts.c, mutex.c).
 */

#ifndef GOC_CHANNEL_INTERNAL_H
#define GOC_CHANNEL_INTERNAL_H

#include "chan_type.h"
#include "internal.h"

/* channel.c -> used by alts.c, mutex.c */
void wake(goc_chan* ch, goc_entry* e, void* value);
void compact_dead_entries(goc_chan* ch);
void chan_set_on_close(goc_chan* ch, void (*on_close)(void*), void* ud);

static inline int chan_put_to_buffer(goc_chan* ch, void* val) {
    if (ch->buf_count >= ch->buf_size) return 0;
    size_t tail = (ch->buf_head + ch->buf_count) % ch->buf_size;
    ch->buf[tail] = val;
    ch->buf_count++;
    return 1;
}

static inline int chan_take_from_buffer(goc_chan* ch, void** out) {
    if (ch->buf_count == 0) return 0;
    *out = ch->buf[ch->buf_head];
    ch->buf_head = (ch->buf_head + 1) % ch->buf_size;
    ch->buf_count--;
    return 1;
}

static inline int chan_put_to_taker(goc_chan* ch, void* val) {
    goc_entry** pp = &ch->takers;
    while (*pp && atomic_load_explicit(&(*pp)->cancelled, memory_order_acquire))
        pp = &(*pp)->next;
    goc_entry* e = *pp;
    if (!e) return 0;
    /* Save e->next before calling wake(). For GOC_FIBER entries, wake() calls
     * post_to_run_queue, which may allow a pool thread to resume the waiting
     * fiber immediately. That fiber's stack frame - which contains the
     * stack-allocated goc_entry (from goc_take's slow path) - is then
     * deallocated as the coroutine unwinds past mco_yield. Reading e->next
     * after wake() would therefore be a use-after-free. Snapshotting next
     * here, before wake(), is safe because e->next is only ever written under
     * ch->lock, which we still hold. */
    goc_entry* next = e->next;
    wake(ch, e, val);
    *pp = next;
    return 1;
}

static inline int chan_take_from_putter(goc_chan* ch, void** out) {
    goc_entry** pp = &ch->putters;
    while (*pp && atomic_load_explicit(&(*pp)->cancelled, memory_order_acquire))
        pp = &(*pp)->next;
    goc_entry* e = *pp;
    if (!e) return 0;
    *out = e->put_val;
    /* Save e->next before calling wake() for the same reason as chan_put_to_taker:
     * wake() may resume the parked fiber immediately on another pool thread,
     * freeing the stack frame that contains the stack-allocated goc_entry.
     * e->put_val is read above (before wake), which is safe. e->next must be
     * snapshotted here, also before wake(), while ch->lock is still held. */
    goc_entry* next = e->next;
    wake(ch, e, NULL);
    *pp = next;
    return 1;
}

#endif /* GOC_CHANNEL_INTERNAL_H */
