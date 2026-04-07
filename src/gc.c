/* src/gc.c
 *
 * Owns:
 *   - goc_init / goc_shutdown lifecycle entry points
 *   - goc_malloc
 *   - live-channels registry (live_channels array + g_live_mutex)
 *   - pool registry initialisation (delegates to pool.c)
 *   - goc_thread_create / goc_thread_join (all platforms)
 */

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <stdatomic.h>
#include <uv.h>
#include <gc.h>
#include <gc/gc_mark.h>
#include "../include/goc.h"
#include "chan_type.h"
#include "internal.h"
#include "channel_internal.h"

/* Forward declarations for module lifecycle reset hooks. */
extern void goc_http_reset_globals(void*);
extern size_t goc_cb_queue_get_hwm(void);

#define GOC_LIFECYCLE_HOOK_CAP 16

typedef struct {
    void (*fn)(void*);
    void*     ub;
} goc_lifecycle_hook_t;

static uv_mutex_t g_lifecycle_hook_lock;
static goc_lifecycle_hook_t g_lifecycle_hooks[GOC_LIFECYCLE_HOOK_COUNT][GOC_LIFECYCLE_HOOK_CAP];
static size_t g_lifecycle_hooks_len[GOC_LIFECYCLE_HOOK_COUNT] = {0};

void goc_run_lifecycle_hooks(goc_lifecycle_hook_phase_t phase,
                                void* event_arg)
{
    if (phase < 0 || phase >= GOC_LIFECYCLE_HOOK_COUNT)
        return;

    goc_lifecycle_hook_t hooks[GOC_LIFECYCLE_HOOK_CAP];
    size_t len = 0;

    uv_mutex_lock(&g_lifecycle_hook_lock);
    len = g_lifecycle_hooks_len[phase];
    for (size_t i = 0; i < len; i++)
        hooks[i] = g_lifecycle_hooks[phase][i];
    uv_mutex_unlock(&g_lifecycle_hook_lock);

    for (size_t i = 0; i < len; i++) {
        if (!hooks[i].fn)
            continue;

        if (event_arg == NULL) {
            if (hooks[i].ub == NULL)
                hooks[i].fn(NULL);
        } else {
            if (hooks[i].ub == event_arg)
                hooks[i].fn(hooks[i].ub);
        }
    }
}

void goc_register_lifecycle_hook(goc_lifecycle_hook_phase_t phase,
                                 void (*fn)(void*),
                                 void* ub)
{
    if (phase < 0 || phase >= GOC_LIFECYCLE_HOOK_COUNT || !fn)
        return;

    uv_mutex_lock(&g_lifecycle_hook_lock);
    if (g_lifecycle_hooks_len[phase] < GOC_LIFECYCLE_HOOK_CAP) {
        g_lifecycle_hooks[phase][g_lifecycle_hooks_len[phase]].fn = fn;
        g_lifecycle_hooks[phase][g_lifecycle_hooks_len[phase]].ub = ub;
        g_lifecycle_hooks_len[phase]++;
    }
    uv_mutex_unlock(&g_lifecycle_hook_lock);
}

/* ---------------------------------------------------------------------------
 * goc_thread_create / goc_thread_join  (all platforms)
 *
 * libuv's uv_thread_create does not register new threads with Boehm GC.
 * This trampoline ensures every thread is registered before its body runs
 * and unregistered on exit.  goc_init() calls GC_allow_register_threads()
 * before any threads are spawned, which is required on all platforms.
 * ---------------------------------------------------------------------------*/

typedef struct {
    uv_thread_cb fn;
    void*        arg;
} gc_thread_args_t;

static void goc_thread_trampoline(void* raw)
{
    /* Extract fn/arg and free the heap-allocated carrier before we touch
     * any GC-managed memory (avoids a window where the carrier is live but
     * not yet reachable by the GC). */
    gc_thread_args_t* w = (gc_thread_args_t*)raw;
    uv_thread_cb fn     = w->fn;
    void*        arg    = w->arg;
    free(w);

    struct GC_stack_base sb;
    GC_get_stack_base(&sb);
    GC_register_my_thread(&sb);
    fn(arg);
    GC_unregister_my_thread();
}

int goc_thread_create(uv_thread_t* t, uv_thread_cb fn, void* arg)
{
    gc_thread_args_t* w = malloc(sizeof(gc_thread_args_t));
    if (!w) return UV_ENOMEM;
    w->fn  = fn;
    w->arg = arg;
    int rc = uv_thread_create(t, goc_thread_trampoline, w);
    if (rc != 0) free(w);
    return rc;
}

int goc_thread_join(uv_thread_t* t)
{
    return uv_thread_join(t);
}

/* ---------------------------------------------------------------------------
 * [Fix 5] Fiber stack root push — flat array + bitmap
 *
 * Problem: GC_add_roots / GC_remove_roots use a fixed internal table
 * (MAX_ROOT_SETS ≈ 2048 in BDW-GC 8.2.6).  Benchmarks that spawn large
 * numbers of fibers (e.g. bench_spawn_idle with 200,000 fibers) exhaust
 * the table, causing BDW-GC to abort with "Too many root sets".
 *
 * Original fix: maintain a lock-free linked list of live fiber stack ranges.
 * Drawback: dead nodes accumulate (never freed until goc_shutdown); the
 * push_fiber_roots callback must walk the entire list on every GC mark phase.
 *
 * Fix 5: replace the linked list with a flat array of slots divided into
 * CHUNK_SIZE-slot chunks, plus a parallel atomic bitmap.  Benefits:
 *   1. Slot reuse — freed slots (cleared bitmap bit) are immediately
 *      reclaimed by the next registration.
 *   2. Cache-friendly GC scan — bitmap word iteration skips 64 dead slots
 *      in a single comparison, avoiding pointer-chasing through dead nodes.
 *   3. Smaller slots — no 'coro' or 'next' pointer needed per slot; the
 *      bitmap encodes liveness.
 *
 * Thread-safety / stop-the-world:
 *   BDW-GC stops the world (via SIGPWR on Linux) before calling the callback.
 *   A pthread_mutex in the callback would deadlock if a stopped thread held
 *   it.  The callback therefore reads fiber_root_chunks / fiber_root_bitmap /
 *   fiber_root_num_chunks WITHOUT holding fiber_root_mutex.
 *
 *   Registration and unregistration use fiber_root_mutex (a plain uv_mutex_t).
 *   Growth (adding a new chunk) uses plain malloc — NOT GC_malloc — so the GC
 *   cannot be triggered while the mutex is held.  Growth publishes the new
 *   chunk pointer to fiber_root_chunks[c] BEFORE atomically incrementing
 *   fiber_root_num_chunks; the acquire load of num_chunks in the callback
 *   paired with the release fence before the increment guarantees the callback
 *   always sees a consistent (chunk pointer, bitmap) pair.
 *
 *   Unregistration clears the bitmap bit with memory_order_release via
 *   atomic_fetch_and, so the callback (running after stop-the-world) sees the
 *   cleared bit if the unregister completed before the GC cycle started.
 *
 * Handle returned by goc_fiber_root_register: the slot index cast to void*.
 * Using the index (rather than a pointer into the array) keeps handles valid
 * across potential future realloc of the chunk pointer array.
 * --------------------------------------------------------------------------- */

#define FIBER_ROOT_CHUNK_SIZE  1024
#define FIBER_ROOT_CHUNK_WORDS (FIBER_ROOT_CHUNK_SIZE / 64)
#define FIBER_ROOT_MAX_CHUNKS  256   /* 256 × 1024 = 262 144 max simultaneous fibers */

typedef struct {
    goc_entry*      entry;      /* GC root: keeps the fiber's goc_entry alive */
    void*           stack_base; /* low end of fiber stack (const after init) */
    void*           stack_top;  /* high end of fiber stack (const after init) */
    _Atomic(void*)  scan_from;  /* cached SP hint; kept for future optimisation */
} fiber_root_slot;

/* fiber_root_chunks[c] is malloc'd (not GC-heap) for the same reason as the
 * run queue nodes: we must not call GC_malloc while holding fiber_root_mutex
 * (the GC callback cannot acquire that mutex during stop-the-world). */
static fiber_root_slot*   fiber_root_chunks[FIBER_ROOT_MAX_CHUNKS];

/* Flat bitmap: bit (c * CHUNK_SIZE + i) set ⇒ slot i of chunk c is in use.
 * Static storage — zero-initialised at program start. */
static _Atomic(uint64_t)  fiber_root_bitmap[FIBER_ROOT_MAX_CHUNKS * FIBER_ROOT_CHUNK_WORDS];

static _Atomic(size_t)    fiber_root_num_chunks = 0;
static uv_mutex_t         fiber_root_mutex;
static GC_push_other_roots_proc prev_push_roots = NULL;

static void push_fiber_roots(void)
{
    /* Called during GC stop-the-world.  All app threads are suspended.
     * Acquire load of num_chunks establishes happens-before with the release
     * fence written before the chunk-pointer store in goc_fiber_root_register,
     * guaranteeing fiber_root_chunks[c] is visible for every c < nc. */
    size_t nc = atomic_load_explicit(&fiber_root_num_chunks, memory_order_acquire);
    for (size_t c = 0; c < nc; c++) {
        fiber_root_slot* chunk     = fiber_root_chunks[c];
        size_t           base_word = c * FIBER_ROOT_CHUNK_WORDS;
        for (size_t w = 0; w < FIBER_ROOT_CHUNK_WORDS; w++) {
            uint64_t bword = atomic_load_explicit(
                &fiber_root_bitmap[base_word + w], memory_order_relaxed);
            while (bword) {
                int    bit    = __builtin_ctzll(bword);
                size_t offset = (size_t)w * 64 + (size_t)bit;
                fiber_root_slot* s = &chunk[offset];

                /* Push goc_entry* so the GC keeps the entry alive.  The run
                 * queue that references it is malloc'd (not GC-managed). */
                GC_push_all_eager(&s->entry,
                                  (char*)&s->entry + sizeof(goc_entry*));

                /* Scan the full fiber stack.
                 *
                 * scan_from is only a valid lower bound when the fiber is
                 * suspended (SP saved at the last yield).  While the fiber is
                 * running, scan_from is stale: new call frames are pushed below
                 * the saved SP and are outside [scan_from, stack_top].  Any
                 * GC_malloc inside those frames (e.g. goc_go_on spawning child
                 * fibers) produces a goc_entry* that is only on the active
                 * fiber stack — invisible to GC if scan_from is used.
                 *
                 * Scanning [stack_base, stack_top] is always conservative and
                 * correct: BDW-GC ignores non-pointer-aligned words, so the
                 * extra scan of the unused portion of the stack is safe. */
                GC_push_all_eager(s->stack_base, s->stack_top);

                bword &= bword - 1; /* clear lowest set bit */
            }
        }
    }
    if (prev_push_roots)
        prev_push_roots();
}

void* goc_fiber_root_register(mco_coro* coro, void* top, goc_entry* entry)
{
    uv_mutex_lock(&fiber_root_mutex);

    /* Scan bitmap for a free slot (bit == 0). */
    size_t nc          = atomic_load_explicit(&fiber_root_num_chunks,
                                              memory_order_relaxed);
    size_t total_words = nc * FIBER_ROOT_CHUNK_WORDS;
    size_t slot_idx    = (size_t)-1;

    for (size_t w = 0; w < total_words; w++) {
        uint64_t bword = atomic_load_explicit(&fiber_root_bitmap[w],
                                              memory_order_relaxed);
        if (bword != UINT64_MAX) {
            int bit = __builtin_ctzll(~bword);
            slot_idx = w * 64 + (size_t)bit;
            break;
        }
    }

    if (slot_idx == (size_t)-1) {
        /* All existing slots taken — allocate a new chunk. */
        size_t c = nc;
        assert(c < FIBER_ROOT_MAX_CHUNKS);
        fiber_root_slot* new_chunk = calloc(FIBER_ROOT_CHUNK_SIZE,
                                            sizeof(fiber_root_slot));
        assert(new_chunk != NULL);

        /* Publish chunk pointer BEFORE incrementing num_chunks.  Paired with
         * the acquire load in push_fiber_roots. */
        fiber_root_chunks[c] = new_chunk;
        atomic_thread_fence(memory_order_release);
        atomic_fetch_add_explicit(&fiber_root_num_chunks, 1,
                                  memory_order_relaxed);

        slot_idx = c * FIBER_ROOT_CHUNK_SIZE;
    }

    /* Write slot data BEFORE setting the bitmap bit.
     *
     * push_fiber_roots runs without the mutex (called by BDW-GC during
     * stop-the-world).  If the bitmap bit were set first, a GC stop between
     * the bit-set and the data-write would leave push_fiber_roots reading
     * stale data from the previous occupant of this slot — whose stack was
     * already freed by mco_destroy — causing GC_push_all_eager to scan freed
     * memory and corrupt the heap (SIGABRT under burst-spawn workloads).
     *
     * By writing data first and then publishing the bit with a release fence,
     * any observer that loads the bit and sees it set is guaranteed to also
     * see the correct slot data. */
    size_t           chunk_idx = slot_idx / FIBER_ROOT_CHUNK_SIZE;
    size_t           offset    = slot_idx % FIBER_ROOT_CHUNK_SIZE;
    fiber_root_slot* s         = &fiber_root_chunks[chunk_idx][offset];
    s->entry      = entry;
    s->stack_base = coro->stack_base;
    s->stack_top  = top;
    void* initial_sp = mco_get_suspended_sp(coro);
    atomic_store_explicit(&s->scan_from,
                          initial_sp ? initial_sp : coro->stack_base,
                          memory_order_relaxed);

    /* Publish: release fence ensures slot data is visible before the bit. */
    size_t   bitmap_word = slot_idx / 64;
    uint64_t bitmap_bit  = (uint64_t)1 << (slot_idx % 64);
    atomic_thread_fence(memory_order_release);
    atomic_fetch_or_explicit(&fiber_root_bitmap[bitmap_word],
                             bitmap_bit,
                             memory_order_relaxed);

    uv_mutex_unlock(&fiber_root_mutex);
    return (void*)(uintptr_t)slot_idx;
}

void goc_fiber_root_unregister(void* handle)
{
    size_t   slot_idx = (size_t)(uintptr_t)handle;
    size_t   w        = slot_idx / 64;
    uint64_t mask     = ~((uint64_t)1 << (slot_idx % 64));
    /* release ordering ensures prior slot writes are visible before the bit
     * is cleared, so the GC callback cannot observe a partially-written slot
     * for a newly recycled index. */
    atomic_fetch_and_explicit(&fiber_root_bitmap[w], mask, memory_order_release);
}

void goc_fiber_root_update_sp(void* handle, mco_coro* coro)
{
    /* Called by pool_worker_fn after mco_resume returns (fiber yielded).
     * Updates the cached scan_from so the next GC scan uses the exact SP
     * instead of rescanning the full stack.  No-op if SP is unavailable. */
    void* saved_sp = mco_get_suspended_sp(coro);
    if (saved_sp == NULL)
        return;
    size_t           slot_idx  = (size_t)(uintptr_t)handle;
    size_t           chunk_idx = slot_idx / FIBER_ROOT_CHUNK_SIZE;
    size_t           offset    = slot_idx % FIBER_ROOT_CHUNK_SIZE;
    fiber_root_slot* s         = &fiber_root_chunks[chunk_idx][offset];
    atomic_store_explicit(&s->scan_from, saved_sp, memory_order_release);
}

void goc_fiber_roots_init(void)
{
    uv_mutex_init(&fiber_root_mutex);
    prev_push_roots = GC_get_push_other_roots();
    GC_set_push_other_roots(push_fiber_roots);
}

/* ---------------------------------------------------------------------------
 * Live-channels registry
 * Only gc.c reads or writes these variables. channel.c accesses them only
 * through chan_register / chan_unregister which acquire g_live_mutex.
 * ---------------------------------------------------------------------------*/

static goc_chan** live_channels     = NULL;
static size_t     live_channels_len = 0;
static size_t     live_channels_cap = 0;
static uv_mutex_t g_live_mutex;   /* plain malloc; not GC-heap (uv constraint) */

/* ---------------------------------------------------------------------------
 * Live UV handle roots registry
 *
 * Tracks GC-allocated pointers for both user handles (registered via
 * goc_io_handle_register) and internal context/dispatch structs (registered
 * via gc_handle_register in goc_io.c).  The backing array is GC_malloc-
 * allocated so the collector scans its contents and keeps all tracked objects
 * alive until their corresponding gc_handle_unregister call.
 *
 * Thread-safety: protected by g_live_uv_mutex.
 * ---------------------------------------------------------------------------*/

static void** live_uv_handles     = NULL;  /* GC_malloc'd array */
static size_t        live_uv_handles_len = 0;
static size_t        live_uv_handles_cap = 0;
static uv_mutex_t    g_live_uv_mutex;
static uv_thread_t g_main_thread;
static bool       g_main_thread_set = false;

__attribute__((constructor))
static void capture_main_thread_at_load(void)
{
    g_main_thread = uv_thread_self();
    g_main_thread_set = true;
}

static void lifecycle_abort_non_main_thread(const char* fn_name)
{
    if (!g_main_thread_set) {
        g_main_thread = uv_thread_self();
        g_main_thread_set = true;
    }

    uv_thread_t self = uv_thread_self();
    if (!uv_thread_equal(&self, &g_main_thread)) {
        ABORT("%s must be called from the main thread\n", fn_name);
    }
}

/* ---------------------------------------------------------------------------
 * goc_malloc
 * ---------------------------------------------------------------------------*/

void* goc_malloc(size_t n) {
    void* p = GC_malloc(n);
    if (!p)
        ABORT("goc_malloc: allocation failed\n");
    return p;
}

/* ---------------------------------------------------------------------------
 * goc_realloc
 * ---------------------------------------------------------------------------*/

void* goc_realloc(void* ptr, size_t n) {
    void* p = GC_realloc(ptr, n);
    if (n > 0 && !p)
        ABORT("goc_realloc: allocation failed\n");
    return p;
}

/* ---------------------------------------------------------------------------
 * goc_sprintf
 * ---------------------------------------------------------------------------*/

#include <stdarg.h>

char* goc_sprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (len < 0) {
        ABORT("goc_sprintf: vsnprintf failed (len=%d)\n", len);
    }

    char* buf = (char*)GC_malloc((size_t)len + 1);
    /* GC_malloc aborts on failure, so buf is always non-NULL here. */

    va_start(ap, fmt);
    vsnprintf(buf, (size_t)len + 1, fmt, ap);
    va_end(ap);

    return buf;
}

/* ---------------------------------------------------------------------------
 * live_channels_init  (internal; declared in internal.h)
 *
 * Allocates the live_channels array with an initial capacity of 64 entries
 * via GC_malloc (so Boehm GC scans it for interior channel pointers and
 * does not prematurely collect live channels). Initialises g_live_mutex.
 * Must be called before the loop thread is spawned and before any channel
 * is created.
 * ---------------------------------------------------------------------------*/

void live_channels_init(void) {
    live_channels_cap = 64;
    live_channels     = GC_malloc(live_channels_cap * sizeof(goc_chan*));
    assert(live_channels != NULL);
    live_channels_len = 0;
    uv_mutex_init(&g_live_mutex);
}

/* ---------------------------------------------------------------------------
 * chan_register  (internal; declared in internal.h)
 *
 * Called by channel.c:goc_chan_make after a new channel is fully initialised.
 * Appends ch to live_channels, growing the array (doubling) if needed.
 * ---------------------------------------------------------------------------*/

void chan_register(goc_chan* ch) {
    uv_mutex_lock(&g_live_mutex);

    if (live_channels_len == live_channels_cap) {
        size_t new_cap  = live_channels_cap * 2;
        goc_chan** grown = GC_realloc(live_channels, new_cap * sizeof(goc_chan*));
        assert(grown != NULL);
        live_channels     = grown;
        live_channels_cap = new_cap;
    }

    live_channels[live_channels_len++] = ch;

    uv_mutex_unlock(&g_live_mutex);
}

/* ---------------------------------------------------------------------------
 * chan_unregister  (internal; declared in internal.h)
 *
 * Called by channel.c:goc_close. Removes ch from live_channels by linear
 * scan; fills the hole with the last element (swap-with-last).
 * ---------------------------------------------------------------------------*/

void chan_unregister(goc_chan* ch) {
    uv_mutex_lock(&g_live_mutex);

    for (size_t i = 0; i < live_channels_len; i++) {
        if (live_channels[i] == ch) {
            live_channels[i] = live_channels[--live_channels_len];
            break;
        }
    }

    uv_mutex_unlock(&g_live_mutex);
}

/* ---------------------------------------------------------------------------
 * live_uv_handles_init  (internal; declared in internal.h)
 *
 * Allocates the GC-managed live_uv_handles array and initialises its mutex.
 * Must be called during goc_init, before any channel-backed libuv operation.
 * ---------------------------------------------------------------------------*/

void live_uv_handles_init(void) {
    live_uv_handles_cap = 16;
    live_uv_handles     = GC_malloc(live_uv_handles_cap * sizeof(void*));
    assert(live_uv_handles != NULL);
    live_uv_handles_len = 0;
    uv_mutex_init(&g_live_uv_mutex);
}

/* ---------------------------------------------------------------------------
 * gc_handle_register  (internal; declared in internal.h)
 *
 * Adds p to the GC-visible live_uv_handles array so the collector keeps it
 * alive while libuv holds an opaque reference to it.  Used for both user
 * handles (via goc_io_handle_register) and internal context/dispatch structs.
 * ---------------------------------------------------------------------------*/

void gc_handle_register(void* p) {
    uv_mutex_lock(&g_live_uv_mutex);

    if (live_uv_handles_len == live_uv_handles_cap) {
        size_t  new_cap = live_uv_handles_cap * 2;
        void**  grown   = GC_realloc(live_uv_handles,
                                     new_cap * sizeof(void*));
        assert(grown != NULL);
        live_uv_handles     = grown;
        live_uv_handles_cap = new_cap;
    }

    live_uv_handles[live_uv_handles_len++] = p;

    uv_mutex_unlock(&g_live_uv_mutex);
}

/* ---------------------------------------------------------------------------
 * gc_handle_unregister  (internal; declared in internal.h)
 *
 * Removes p from live_uv_handles so the GC may collect it once all other
 * references drop.
 * ---------------------------------------------------------------------------*/

void gc_handle_unregister(void* p) {
    uv_mutex_lock(&g_live_uv_mutex);

    for (size_t i = 0; i < live_uv_handles_len; i++) {
        if (live_uv_handles[i] == p) {
            live_uv_handles[i] = live_uv_handles[--live_uv_handles_len];
            break;
        }
    }

    uv_mutex_unlock(&g_live_uv_mutex);
}

/* ---------------------------------------------------------------------------
 * goc_init
 *
 * Full initialisation sequence (must be called exactly once, from the main
 * thread, before any other libgoc function):
 *
 *   1. GC_INIT()
 *   2. GC_allow_register_threads()
 *   3. live_channels_init()    — allocates live_channels + g_live_mutex
 *   4. live_uv_handles_init()  — allocates live_uv_handles + g_live_uv_mutex
 *   5. pool_registry_init()    — allocates pool registry (src/pool.c)
 *   6. loop_init()             — allocates g_loop, g_wakeup, spawns loop thread
 *   7. g_default_pool = goc_pool_make(N)
 *        N = GOC_POOL_THREADS env var if set to a valid positive integer,
 *            otherwise max(4, hardware_concurrency).
 * ---------------------------------------------------------------------------*/

void goc_init(void) {
    lifecycle_abort_non_main_thread("goc_init");

    /* Step 1 — Initialise Boehm GC. */
    GC_INIT();

    /* Step 1.1 — Enable incremental (page-fault-driven) collection mode.
     * This must be called before GC_disable() so that GC_collect_a_little()
     * does genuinely bounded, incremental work rather than potentially
     * escalating to a full stop-the-world collection.  Without incremental
     * mode, GC_collect_a_little() may do a full STW pass ("may do more work
     * if further progress requires it, e.g. if incremental collection is
     * disabled" — Boehm GC docs), which would hang if any GC-registered
     * thread is unresponsive. */
    GC_enable_incremental();

    /* Step 1.2 — Disable automatic stop-the-world collections.  goc_malloc
     * (GC_malloc) will still expand the heap on demand but will never trigger
     * a collection implicitly.  Collection is driven explicitly at scheduler
     * safe points (pool_worker_fn after mco_resume, loop_thread_fn after
     * uv_run) via GC_collect_a_little(). */
    GC_disable();

    /* Step 1.2 — Register fiber-stack push callback (replaces GC_add_roots). */
    goc_fiber_roots_init();

    /* Step 2 — Allow worker threads to register themselves with the GC. */
    GC_allow_register_threads();

    /* Step 3 — Live-channels registry (this file). */
    live_channels_init();

    /* Step 3.0 — Lifecycle hook registry. */
    uv_mutex_init(&g_lifecycle_hook_lock);

    /* Step 3.1 — Live UV-handle channel registry (this file). */
    live_uv_handles_init();

    /* Step 4 — Pool registry (pool.c). */
    pool_registry_init();

    /* Step 4.5 — goc_io internal init. */
    goc_io_init();

    /* Step 4.1 — Mutex registry (mutex.c). */
    mutex_registry_init();

    /* Step 5 — libuv event loop + loop thread (loop.c). */
    loop_init();
    goc_run_lifecycle_hooks(GOC_LIFECYCLE_HOOK_POST_LOOP_INIT, NULL);

    /* Step 6 — Default fiber pool.
     *
     * Determine thread count:
     *   - If GOC_POOL_THREADS is set and parses to a positive integer, use it.
     *   - Otherwise: max(4, hardware_concurrency) where hardware_concurrency
     *     is obtained from uv_available_parallelism().
     */
    size_t n = 0;

    const char* env = getenv("GOC_POOL_THREADS");
    if (env != NULL) {
        char*  end = NULL;
        long   v   = strtol(env, &end, 10);
        if (end != env && *end == '\0' && v > 0) {
            n = (size_t)v;
        }
    }

    if (n == 0) {
        unsigned int hw = uv_available_parallelism();
        n = (hw > 4) ? (size_t)hw : 4;
    }

    g_default_pool = goc_pool_make(n);
}


/* ---------------------------------------------------------------------------
 * goc_shutdown
 *
 * Orderly teardown. Blocks until all fibers on all pools have run to
 * completion, then tears down the event loop and frees all resources.
 * Intended to be called once at the end of main().
 *
 * Sequence:
 *   B.1   pool_registry_destroy_all()  — drains (blocks) and destroys every pool
 *   B.2   loop_shutdown()              — signals loop thread, joins, frees g_loop
 *   B.2a  Tear down live_uv_handles registry (live_uv_handles + g_live_uv_mutex)
 *   B.3   Destroy all channel mutexes; free live_channels; destroy g_live_mutex
 *   B.3.1 mutex_registry_destroy_all() — destroy all RW-mutex internal locks
 *
 * B.2 must precede B.3: the loop thread's drain callbacks may still call
 * goc_close() on timeout channels (locking channel mutexes) after all pool
 * fibers have returned.  Joining the loop thread first (B.2) ensures no
 * callback is in flight before those mutexes are destroyed (B.3).
 * ---------------------------------------------------------------------------*/

void goc_shutdown(void) {
    lifecycle_abort_non_main_thread("goc_shutdown");
    goc_debug_set_close_phase("goc_shutdown");
    GOC_DBG("goc_shutdown: begin\n");

    /* B.1 — Drain and destroy all registered pools (including g_default_pool). */
    GOC_DBG("goc_shutdown: B.1 pool_registry_destroy_all begin\n");
    pool_registry_destroy_all();
    GOC_DBG("goc_shutdown: B.1 pool_registry_destroy_all done g_loop_shutting_down=%d\n",
            goc_loop_is_shutting_down());

    goc_run_lifecycle_hooks(GOC_LIFECYCLE_HOOK_PRE_LOOP_SHUTDOWN, NULL);

    /* B.2 — Shut down the event loop and join the loop thread.
     *
     * Must happen before channel/mutex teardown (old B.2): the loop thread's
     * drain_cb_queue_guarded() may still invoke timer-expiry callbacks that
     * call goc_close() on timeout channels, which lock channel mutexes.
     * Destroying those mutexes first would cause UB.
     *
     * Keep g_live_uv_mutex alive until loop shutdown completes: close
     * callbacks that run on the loop thread call gc_handle_unregister(). */
    GOC_DBG("goc_shutdown: B.2 loop_shutdown begin\n");
    loop_shutdown();
    GOC_DBG("goc_shutdown: B.2 loop_shutdown done g_loop_shutting_down=%d hwm=%zu\n",
            goc_loop_is_shutting_down(),
            goc_cb_queue_get_hwm());

    /* B.2a — goc_io internal shutdown. */
    goc_io_shutdown();
    goc_run_lifecycle_hooks(GOC_LIFECYCLE_HOOK_POST_LOOP_SHUTDOWN, NULL);

    /* B.2b — Tear down the live UV handle roots registry. */
    live_uv_handles     = NULL;
    live_uv_handles_len = 0;
    live_uv_handles_cap = 0;
    uv_mutex_destroy(&g_live_uv_mutex);
    GOC_DBG("goc_shutdown: B.2a live_uv teardown done\n");

    uv_mutex_destroy(&g_lifecycle_hook_lock);

    /* B.3 — Destroy channel mutexes and tear down the live-channels registry.
     *
     * Safe now: loop_shutdown() (B.2) has joined the loop thread, so no
     * callbacks referencing channel locks are in flight.
     * No other thread will call chan_register / chan_unregister either:
     * pool_registry_destroy_all() (B.1) drained all pools before we got here.
     */
    for (size_t i = 0; i < live_channels_len; i++) {
        goc_chan* ch = live_channels[i];
        uv_mutex_destroy(ch->lock);
        free(ch->lock);
    }
    GC_free(live_channels); /* was GC_malloc'd; must not use free() */
    live_channels     = NULL;
    live_channels_len = 0;
    live_channels_cap = 0;

    uv_mutex_destroy(&g_live_mutex);
    GOC_DBG("goc_shutdown: B.3 channels/mutex teardown done\n");

    /* B.3.1 — Destroy all RW mutex internal locks. */
    GOC_DBG("goc_shutdown: B.3.1 mutex_registry_destroy_all begin\n");
    mutex_registry_destroy_all();
    GOC_DBG("goc_shutdown: B.3.1 mutex_registry_destroy_all done\n");

    /* B.4 — Free all fiber root chunks accumulated during this lifecycle.
     *
     * All pools have been drained (B.1), so no fiber holds a live slot.
     * No mutex needed — no concurrent register/unregister can be in flight.
     * Reset state (chunk pointers, bitmap, num_chunks) so a subsequent
     * goc_init/goc_shutdown cycle starts clean.
     */
    size_t nc = atomic_load_explicit(&fiber_root_num_chunks, memory_order_relaxed);
    for (size_t c = 0; c < nc; c++) {
        free(fiber_root_chunks[c]);
        fiber_root_chunks[c] = NULL;
    }
    size_t nwords = nc * FIBER_ROOT_CHUNK_WORDS;
    for (size_t w = 0; w < nwords; w++)
        atomic_store_explicit(&fiber_root_bitmap[w], 0, memory_order_relaxed);
    atomic_store_explicit(&fiber_root_num_chunks, 0, memory_order_relaxed);
    uv_mutex_destroy(&fiber_root_mutex);
    GOC_DBG("goc_shutdown: B.4 fiber roots teardown done; shutdown complete\n");

    goc_debug_set_close_phase("normal");

    GOC_DBG("goc_shutdown: END\n");
}
