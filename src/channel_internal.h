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
/* Claims e for wake.  Returns true on success.  For GOC_FIBER, sets *fe_out
 * to the fiber entry the caller must spin+post after releasing ch->lock;
 * for all other kinds dispatches immediately and sets *fe_out = NULL. */
bool wake_claim(goc_chan* ch, goc_entry* e, void* value, goc_entry** fe_out);
void compact_dead_entries(goc_chan* ch);
void chan_set_on_close(goc_chan* ch, void (*on_close)(void*), void* ud);

/* Internal debug helpers. */
void goc_chan_set_debug_tag(goc_chan* ch, const char* tag);
const char* goc_chan_get_debug_tag(goc_chan* ch);
void goc_debug_set_close_phase(const char* phase);
const char* goc_debug_get_close_phase(void);

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
    ch->item_count++;
    return 1;
}

static inline int chan_take_from_buffer(goc_chan* ch, void** out) {
    if (ch->buf_count == 0) return 0;
    *out = ch->buf[ch->buf_head];
    ch->buf_head = (ch->buf_head + 1) % ch->buf_size;
    ch->buf_count--;
    if (ch->item_count > 0) ch->item_count--;
    return 1;
}

static inline int chan_put_to_taker(goc_chan* ch, void* val) {
    goc_entry** pp = &ch->takers;
    GOC_DBG("chan_put_to_taker: entry ch=%p val=%p takers=%p\n",
            (void*)ch, val, (void*)ch->takers);
    while (*pp) {
        goc_entry* e = *pp;
        if (atomic_load_explicit(&e->cancelled, memory_order_acquire)) {
            *pp = e->next;
            /* Invalidate tail when we remove the last entry. */
            if (*pp == NULL) ch->takers_tail = NULL;
            continue;
        }
        GOC_DBG("chan_put_to_taker: examining taker e=%p kind=%d coro=%p cancelled=%d\n",
                (void*)e, (int)e->kind, (void*)e->coro,
                (int)atomic_load_explicit(&e->cancelled, memory_order_acquire));
        /* Save e->next before calling wake(). For GOC_FIBER entries, wake()
         * may allow another worker to resume the waiter immediately, making
         * this parked entry unreachable/collectable before we iterate again.
         * Reading e->next after wake() would therefore be unsafe. Snapshotting
         * next here is safe because e->next is only written under ch->lock,
         * which we still hold. */
        goc_entry* next = e->next;
        if (wake(ch, e, val)) {
            *pp = next;
            /* Invalidate tail when we remove the last entry. */
            if (next == NULL) ch->takers_tail = NULL;
            GOC_DBG("chan_put_to_taker: woke entry=%p kind=%d coro=%p\n",
                    (void*)e, (int)e->kind, (e->kind == GOC_FIBER ? (void*)e->coro : NULL));
            return 1;
        }
        GOC_DBG("chan_put_to_taker: wake failed e=%p ch=%p\n", (void*)e, (void*)ch);
        *pp = next;
        if (next == NULL) ch->takers_tail = NULL;
    }
    GOC_DBG("chan_put_to_taker: no taker found ch=%p\n", (void*)ch);
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
        /* Save e->next before calling wake() for the same reason as
         * chan_put_to_taker: wake() may resume the parked fiber immediately on
         * another worker, making this entry unreachable/collectable. e->put_val
         * is read above (before wake), which is safe. e->next must be
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

/* --------------------------------------------------------------------------
 * chan_put_to_taker_claim — like chan_put_to_taker but uses wake_claim().
 *
 * On success returns 1 and sets *fe_out to the fiber entry that the caller
 * must spin+post after releasing ch->lock (NULL for non-fiber takers).
 * Caller must unlock ch->lock before spinning.
 * -------------------------------------------------------------------------- */
static inline int chan_put_to_taker_claim(goc_chan* ch, void* val,
                                          goc_entry** fe_out,
                                          uint64_t* parked_gen_out)
{
    goc_entry** pp = &ch->takers;
    GOC_DBG("chan_put_to_taker_claim: entry ch=%p val=%p takers=%p\n",
            (void*)ch, val, (void*)ch->takers);
    while (*pp) {
        goc_entry* e = *pp;
        if (atomic_load_explicit(&e->cancelled, memory_order_acquire)) {
            *pp = e->next;
            if (*pp == NULL) ch->takers_tail = NULL;
            continue;
        }
        GOC_DBG("chan_put_to_taker_claim: examining taker e=%p kind=%d coro=%p cancelled=%d\n",
                (void*)e, (int)e->kind, (void*)e->coro,
                (int)atomic_load_explicit(&e->cancelled, memory_order_acquire));
        goc_entry* next = e->next;
        goc_entry* fe = NULL;
        GOC_DBG("chan_put_to_taker_claim: before wake_claim e=%p parked_gen=%llu parked_gen_addr=%p\n",
                (void*)e, (unsigned long long)e->parked_gen, (void*)&e->parked_gen);
        if (wake_claim(ch, e, val, &fe)) {
            *pp = next;
            if (next == NULL) ch->takers_tail = NULL;
            *fe_out = fe;
            if (parked_gen_out)
                *parked_gen_out = (e->parked_gen << 1) + 2;
            GOC_DBG("chan_put_to_taker_claim: claimed taker e=%p ch=%p fe_out=%p parked_gen=%llu parked_state=%llu\n",
                    (void*)e, (void*)ch, (void*)*fe_out,
                    (unsigned long long)e->parked_gen,
                    (unsigned long long)((e->parked_gen << 1) + 2));
            return 1;
        }
        GOC_DBG("chan_put_to_taker_claim: wake_claim failed e=%p ch=%p\n", (void*)e, (void*)ch);
        *pp = next;
        if (next == NULL) ch->takers_tail = NULL;
    }
    GOC_DBG("chan_put_to_taker_claim: no taker found ch=%p\n", (void*)ch);
    return 0;
}

/* --------------------------------------------------------------------------
 * chan_take_from_putter_claim — like chan_take_from_putter but uses wake_claim().
 *
 * On success returns 1, writes the putter's value to *out, and sets *fe_out
 * to the fiber entry that the caller must spin+post after releasing ch->lock
 * (NULL for non-fiber putters).
 * -------------------------------------------------------------------------- */
static inline int chan_take_from_putter_claim(goc_chan* ch, void** out,
                                              goc_entry** fe_out,
                                              uint64_t* parked_gen_out)
{
    goc_entry** pp = &ch->putters;
    while (*pp) {
        goc_entry* e = *pp;
        if (atomic_load_explicit(&e->cancelled, memory_order_acquire)) {
            *pp = e->next;
            if (*pp == NULL) ch->putters_tail = NULL;
            continue;
        }
        void* val = e->put_val;
        goc_entry* next = e->next;
        goc_entry* fe = NULL;
        GOC_DBG("chan_take_from_putter_claim: before wake_claim e=%p parked_gen=%llu parked_gen_addr=%p\n",
                (void*)e, (unsigned long long)e->parked_gen, (void*)&e->parked_gen);
        if (wake_claim(ch, e, NULL, &fe)) {
            *out = val;
            *pp = next;
            if (next == NULL) ch->putters_tail = NULL;
            *fe_out = fe;
            if (parked_gen_out)
                *parked_gen_out = (e->parked_gen << 1) + 2;
            return 1;
        }
        *pp = next;
        if (next == NULL) ch->putters_tail = NULL;
    }
    return 0;
}

#endif /* GOC_CHANNEL_INTERNAL_H */
