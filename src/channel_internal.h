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
bool wake(goc_chan* ch, goc_entry* e, void* value);
void compact_dead_entries(goc_chan* ch);
void chan_set_on_close(goc_chan* ch, void (*on_close)(void*), void* ud);

/* --------------------------------------------------------------------------
 * chan_list_append — O(1) tail append.
 *
 * Appends `e` to the waiter list headed by `*head` / `*tail`.
 * When `*tail` is NULL (unknown or empty), falls back to an O(N) traversal
 * that also repairs `*tail`.  This lazy repair keeps the common append path
 * O(1) while remaining correct after removals that only invalidate the tail
 * pointer rather than repairing it.
 * Must be called with ch->lock held.  `e->next` need not be NULL on entry.
 * -------------------------------------------------------------------------- */
static inline void chan_list_append(goc_entry** head, goc_entry** tail,
                                    goc_entry* e)
{
    e->next = NULL;
    if (*tail) {
        (*tail)->next = e;
        *tail = e;
    } else if (*head == NULL) {
        /* Empty list */
        *head = e;
        *tail = e;
    } else {
        /* tail was invalidated by a prior removal; walk to find real tail */
        goc_entry* cur = *head;
        while (cur->next) cur = cur->next;
        cur->next = e;
        *tail = e;
    }
}

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
    while (*pp) {
        goc_entry* e = *pp;
        if (atomic_load_explicit(&e->cancelled, memory_order_acquire)) {
            *pp = e->next;
            /* Invalidate tail when we remove the last entry. */
            if (*pp == NULL) ch->takers_tail = NULL;
            continue;
        }
        /* Save e->next before calling wake(). For GOC_FIBER entries, wake() calls
         * post_to_run_queue, which may allow a pool thread to resume the waiting
         * fiber immediately. That fiber's stack frame - which contains the
         * stack-allocated goc_entry (from goc_take's slow path) - is then
         * deallocated as the coroutine unwinds past mco_yield. Reading e->next
         * after wake() would therefore be a use-after-free. Snapshotting next
         * here, before wake(), is safe because e->next is only ever written under
         * ch->lock, which we still hold. */
        goc_entry* next = e->next;
        if (wake(ch, e, val)) {
            *pp = next;
            /* Invalidate tail when we remove the last entry. */
            if (next == NULL) ch->takers_tail = NULL;
            return 1;
        }
        *pp = next;
        if (next == NULL) ch->takers_tail = NULL;
    }
    return 0;
}

static inline int chan_take_from_putter(goc_chan* ch, void** out) {
    goc_entry** pp = &ch->putters;
    while (*pp) {
        goc_entry* e = *pp;
        if (atomic_load_explicit(&e->cancelled, memory_order_acquire)) {
            *pp = e->next;
            if (*pp == NULL) ch->putters_tail = NULL;
            continue;
        }
        void* val = e->put_val;
        /* Save e->next before calling wake() for the same reason as chan_put_to_taker:
         * wake() may resume the parked fiber immediately on another pool thread,
         * freeing the stack frame that contains the stack-allocated goc_entry.
         * e->put_val is read above (before wake), which is safe. e->next must be
         * snapshotted here, also before wake(), while ch->lock is still held. */
        goc_entry* next = e->next;
        if (wake(ch, e, NULL)) {
            *out = val;
            *pp = next;
            if (next == NULL) ch->putters_tail = NULL;
            return 1;
        }
        *pp = next;
        if (next == NULL) ch->putters_tail = NULL;
    }
    return 0;
}

#endif /* GOC_CHANNEL_INTERNAL_H */
